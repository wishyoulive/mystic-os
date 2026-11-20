/* ============================================================
 *  Mystic OS - net/dns.c
 *
 *  The stub resolver. Read net/dns.h first: it lists which
 *  RFC section every constant and rule comes from, and what is
 *  deliberately not implemented (DNSSEC, search domains, IDNA).
 *
 *  Shape of the file, top to bottom:
 *
 *     1. octet helpers            RFC 1035 s4.1.2 big-endian fields
 *     2. text <-> address         for /etc/hosts, resolv.conf, logs
 *     3. names on the wire        encode, and decode with compression
 *     4. messages                 header, RR, question, OPT
 *     5. response parsing         CNAME walk, answers, SOA for negatives
 *     6. the cache                positive by (name,type), negative per
 *                                 RFC 2308 s2.1, TTL in milliseconds
 *     7. transport                backoff, TC -> TCP, EDNS fallback
 *     8. dns_resolve()            hosts, cache, chain across messages
 *     9. configuration files      /etc/hosts, /etc/resolv.conf
 *    10. POSIX DnsOps             the only code in the file that does I/O
 *
 *  The resolver never allocates. Every buffer it uses is either a
 *  fixed array inside DnsResolver or on the stack of the function
 *  that needs it, and every one of those is bounded by a named
 *  constant so the worst case can be read off the source.
 * ============================================================ */

/* Set to 0 to build the core with no POSIX headers at all (a host harness
 * or a kernel port does that and supplies its own DnsOps). */
#ifndef DNS_ENABLE_POSIX_OPS
#define DNS_ENABLE_POSIX_OPS 1
#endif

#include "dns.h"

#include <string.h>

/* ============================================================
 *  1. Octet helpers
 *
 *  RFC 1035 s4.1.2: "all of the integer fields are in network byte
 *  order" and multi-octet fields are most significant octet first.
 *  These four functions are the only place that assumption lives.
 * ============================================================ */
static uint16_t dns_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t dns_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void dns_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void dns_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

/* RFC 2181 s5.1: "the TTL is a 32 bit unsigned number ... a TTL with the
 * top bit set is interpreted as zero". Doing it on the way in means a
 * hostile or broken 0x80000000 TTL cannot become a 68 year cache entry. */
static uint32_t dns_ttl(uint32_t ttl)
{
    return (ttl & 0x80000000u) ? 0u : ttl;
}

/* ASCII-only case folding. RFC 4343 s3: DNS name comparison is case
 * insensitive for the ASCII letters only; bytes above 0x7F must not be
 * touched or a UTF-8 label would be mangled. */
static int dns_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int dns_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (dns_lower((unsigned char)*a) != dns_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Compare a [start,start+len) token from a text file with a C string. */
static int dns_token_eq(const char *tok, size_t len, const char *name)
{
    size_t i = 0;
    for (; i < len && name[i]; i++) {
        if (dns_lower((unsigned char)tok[i]) != dns_lower((unsigned char)name[i]))
            return 0;
    }
    return i == len && name[i] == 0;
}

/* Lowercase in place, refusing to touch anything that is not a C string
 * within cap. Returns 0 when the name does not fit. */
static int dns_name_copy_lower(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!src) return 0;
    while (src[i]) {
        if (i + 1 >= cap) return 0;
        dst[i] = (char)dns_lower((unsigned char)src[i]);
        i++;
    }
    dst[i] = 0;
    return 1;
}

/* ============================================================
 *  2. Text <-> address
 *
 *  Needed because /etc/hosts and resolv.conf are text, because a
 *  status line has to name an address, and because a PTR query is
 *  spelled in text. Doing it here keeps the core free of
 *  inet_pton/inet_ntop (which a freestanding or Windows build may
 *  not have) and makes the parsing rules explicit.
 * ============================================================ */
static int dns_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decimal 0..255 or 0..65535, most significant digit first, no padding.
 * Returns the number of octets written (1..5). `tmp` needs 5 bytes. */
static int dns_utoa(unsigned v, char *tmp)
{
    char rev[5];
    int n = 0, k;
    if (v == 0) { tmp[0] = '0'; return 1; }
    while (v && n < 5) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (k = 0; k < n; k++) tmp[k] = rev[n - 1 - k];
    return n;
}

/* Dotted quad. Leading zeros are refused: "010" would be 8 to some
 * readers and 10 to others, and a resolver must not guess. */
static int dns_pton4(const char *s, uint8_t out[4])
{
    int part = 0;
    while (part < 4) {
        int v = 0, digits = 0;
        if (*s < '0' || *s > '9') return DNS_E_BADNAME;
        if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return DNS_E_BADNAME;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return DNS_E_BADNAME;
            v = v * 10 + (*s - '0');
            s++;
        }
        if (v > 255) return DNS_E_BADNAME;
        out[part++] = (uint8_t)v;
        if (part == 4) break;
        if (*s != '.') return DNS_E_BADNAME;
        s++;
    }
    return *s == 0 ? DNS_OK : DNS_E_BADNAME;
}

/* RFC 4291 s2.2 text form: up to eight groups of 1..4 hex digits, at most
 * one "::", and optionally a dotted-quad in the low 32 bits. */
static int dns_pton6(const char *s, uint8_t out[16])
{
    uint16_t g[8];
    int ng = 0, dbl = -1, i;
    const char *p = s;

    if (*p == 0) return DNS_E_BADNAME;
    if (*p == ':') {
        if (p[1] != ':') return DNS_E_BADNAME;   /* a lone leading ':' */
        dbl = 0;
        p += 2;
        if (*p == 0) goto expand;                /* "::" */
    }

    for (;;) {
        const char *h = p;
        int n = 0;
        while (dns_hexval((unsigned char)*p) >= 0) { p++; n++; }
        if (*p == '.') {
            /* An IPv4 tail: the last two groups are one dotted quad. */
            char tmp[16];
            size_t tl = 0;
            const char *q = h;
            uint8_t q4[4];
            while (*q && *q != ':') {
                if (tl + 1 >= sizeof tmp) return DNS_E_BADNAME;
                tmp[tl++] = *q++;
            }
            tmp[tl] = 0;
            if (tl == 0) return DNS_E_BADNAME;
            if (dns_pton4(tmp, q4) != DNS_OK) return DNS_E_BADNAME;
            if (ng + 2 > 8) return DNS_E_BADNAME;
            g[ng++] = (uint16_t)(((uint16_t)q4[0] << 8) | q4[1]);
            g[ng++] = (uint16_t)(((uint16_t)q4[2] << 8) | q4[3]);
            p = q;
            if (*p != 0) return DNS_E_BADNAME;   /* the quad must end it */
            break;
        }
        if (n == 0 || n > 4) return DNS_E_BADNAME;
        if (ng >= 8) return DNS_E_BADNAME;
        {
            uint32_t v = 0;
            for (i = 0; i < n; i++) v = (v << 4) | (uint32_t)dns_hexval((unsigned char)h[i]);
            g[ng++] = (uint16_t)v;
        }
        if (*p == 0) break;
        if (*p != ':') return DNS_E_BADNAME;
        p++;
        if (*p == ':') {
            if (dbl >= 0) return DNS_E_BADNAME;  /* a second "::" */
            dbl = ng;
            p++;
            if (*p == 0) break;
        } else if (*p == 0) {
            return DNS_E_BADNAME;                /* trailing single ':' */
        }
    }

expand:
    if (dbl < 0) {
        if (ng != 8) return DNS_E_BADNAME;
        for (i = 0; i < 8; i++) {
            out[i * 2]     = (uint8_t)(g[i] >> 8);
            out[i * 2 + 1] = (uint8_t)(g[i] & 0xFF);
        }
        return DNS_OK;
    }
    {
        int zeros = 8 - ng;
        if (zeros <= 0) return DNS_E_BADNAME;    /* "::" must stand for >= 1 group */
        for (i = 0; i < 8; i++) {
            uint16_t v;
            if (i < dbl)                 v = g[i];
            else if (i < dbl + zeros)    v = 0;
            else                         v = g[i - zeros];
            out[i * 2]     = (uint8_t)(v >> 8);
            out[i * 2 + 1] = (uint8_t)(v & 0xFF);
        }
    }
    return DNS_OK;
}

int dns_addr_pton(const char *text, DnsAddr *out)
{
    if (!text || !out) return DNS_E_ARG;
    memset(out, 0, sizeof *out);

    /* An IPv6 literal may carry a scope id ("fe80::1%eth0"); the resolver
     * has nowhere to put one and the name would not be a DNS name, so it is
     * refused rather than silently truncated. */
    if (strchr(text, '%')) return DNS_E_BADNAME;

    if (strchr(text, ':')) {
        out->family = DNS_AF_INET6;
        return dns_pton6(text, out->bytes);
    }
    out->family = DNS_AF_INET;
    return dns_pton4(text, out->bytes);
}

/* RFC 5952 s4: lowercase hex, no leading zeros in a group, and the longest
 * run of two or more zero groups (the first one on a tie) replaced by "::". */
static const char *dns_ntop6(const uint8_t b[16], char *buf, size_t cap)
{
    uint16_t g[8];
    int i, best = -1, bestlen = 0, run = -1, runlen = 0;
    size_t o = 0;

    for (i = 0; i < 8; i++) g[i] = (uint16_t)(((uint16_t)b[i * 2] << 8) | b[i * 2 + 1]);

    for (i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (run < 0) { run = i; runlen = 0; }
            runlen++;
            if (runlen > bestlen) { bestlen = runlen; best = run; }
        } else {
            run = -1;
            runlen = 0;
        }
    }
    if (bestlen < 2) best = -1;

    for (i = 0; i < 8; i++) {
        if (i == best) {
            if (o + 2 >= cap) return NULL;
            buf[o++] = ':';
            buf[o++] = ':';
            i += bestlen - 1;
            continue;
        }
        if (o && buf[o - 1] != ':') {
            if (o + 1 >= cap) return NULL;
            buf[o++] = ':';
        }
        {
            char tmp[5];
            int n = 0, k;
            uint16_t v = g[i];
            if (v == 0) {
                tmp[n++] = '0';
            } else {
                while (v) {
                    int d = v & 0xF;
                    tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    v = (uint16_t)(v >> 4);
                }
            }
            if (o + (size_t)n >= cap) return NULL;
            for (k = n - 1; k >= 0; k--) buf[o++] = tmp[k];
        }
    }
    buf[o] = 0;
    return buf;
}

const char *dns_addr_ntop(const DnsAddr *a, char *buf, size_t cap)
{
    if (!a || !buf || cap == 0) return NULL;
    if (a->family == DNS_AF_INET6) return dns_ntop6(a->bytes, buf, cap);
    if (a->family == DNS_AF_INET) {
        /* Longest IPv4 text is 15 octets plus the terminator. */
        if (cap < 16) return NULL;
        {
            size_t o = 0;
            int i;
            for (i = 0; i < 4; i++) {
                char tmp[5];
                int n = dns_utoa(a->bytes[i], tmp), k;
                for (k = 0; k < n; k++) buf[o++] = tmp[k];
                if (i != 3) buf[o++] = '.';
            }
            buf[o] = 0;
        }
        return buf;
    }
    return NULL;
}

/* RFC 1035 s3.5 for IPv4 and RFC 3596 s2.5 for IPv6: the address is written
 * least significant octet (or nibble) first, one label each, under
 * in-addr.arpa or ip6.arpa. */
int dns_ptr_name(uint16_t family, const uint8_t addr[16], char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    int i;

    if (!addr || !out || cap == 0) return DNS_E_ARG;
    if (family == DNS_AF_INET) {
        for (i = 3; i >= 0; i--) {
            char tmp[5];
            int n = dns_utoa(addr[i], tmp), k;
            for (k = 0; k < n; k++) {
                if (o + 1 >= cap) return DNS_E_OVERFLOW;
                out[o++] = tmp[k];
            }
            if (o + 1 >= cap) return DNS_E_OVERFLOW;
            out[o++] = '.';
        }
        if (o + 13 >= cap) return DNS_E_OVERFLOW;
        memcpy(out + o, "in-addr.arpa", 12);
        o += 12;
        out[o] = 0;
        return DNS_OK;
    }
    if (family == DNS_AF_INET6) {
        for (i = 15; i >= 0; i--) {
            /* low nibble first, then high nibble: RFC 3596 writes the
             * address as 32 nibbles in reverse order */
            if (o + 4 >= cap) return DNS_E_OVERFLOW;
            out[o++] = hex[addr[i] & 0x0F];
            out[o++] = '.';
            out[o++] = hex[(addr[i] >> 4) & 0x0F];
            out[o++] = '.';
        }
        if (o + 9 >= cap) return DNS_E_OVERFLOW;
        memcpy(out + o, "ip6.arpa", 8);
        o += 8;
        out[o] = 0;
        return DNS_OK;
    }
    return DNS_E_ARG;
}

const char *dns_strerror(int err)
{
    switch (err) {
    case DNS_OK:         return "ok";
    case DNS_E_ARG:      return "invalid argument";
    case DNS_E_BADNAME:  return "name cannot be encoded";
    case DNS_E_WIRE:     return "malformed DNS message";
    case DNS_E_COMPRESS: return "bad name compression pointer";
    case DNS_E_ID:       return "reply ID does not match the query";
    case DNS_E_RCODE:    return "server returned an error code";
    case DNS_E_NXDOMAIN: return "no such name";
    case DNS_E_NODATA:   return "name exists but has no data of that type";
    case DNS_E_DEPTH:    return "CNAME chain too deep";
    case DNS_E_CYCLE:    return "CNAME chain loops";
    case DNS_E_TIMEOUT:  return "no reply within the timeout";
    case DNS_E_NET:      return "network error";
    case DNS_E_OVERFLOW: return "buffer too small";
    case DNS_E_TCP:      return "TCP retry failed";
    case DNS_E_NOCACHE:  return "not in cache";
    case DNS_E_NOSERVER: return "no nameserver configured";
    default:             return "unknown DNS error";
    }
}

/* ============================================================
 *  3. Names on the wire
 * ============================================================ */

/* Encode a presentation-format name at buf[*pos]. A single trailing dot is
 * the root label and is dropped ("example.com." and "example.com" are the
 * same name); an empty string is the root. Everything else that cannot be
 * represented as a plain label sequence is refused rather than guessed:
 * empty labels, labels over 63 octets, names over 255 octets (RFC 1035
 * s2.3.4), and control characters or spaces. */
static int dns_put_name(uint8_t *buf, size_t cap, size_t *pos, const char *name)
{
    size_t p = *pos, n, i = 0, wire = 0;

    if (!name) return DNS_E_ARG;
    n = strlen(name);
    if (n + 1 >= DNS_MAX_NAME) return DNS_E_BADNAME;

    if (n == 0) {                          /* the root */
        if (p >= cap) return DNS_E_OVERFLOW;
        buf[p++] = 0;
        *pos = p;
        return DNS_OK;
    }
    if (name[n - 1] == '.') {
        if (n >= 2 && name[n - 2] == '.') return DNS_E_BADNAME;
        n--;
    }

    while (i < n) {
        size_t lab = 0, k;
        size_t start = i;
        while (i < n && name[i] != '.') { i++; lab++; }
        if (lab == 0 || lab > DNS_LABEL_MAX) return DNS_E_BADNAME;
        for (k = start; k < start + lab; k++) {
            unsigned char c = (unsigned char)name[k];
            if (c <= 0x20 || c == 0x7F) return DNS_E_BADNAME;
        }
        wire += lab + 1;
        /* RFC 1035 s2.3.4: 255 octets of wire name, which includes the
         * final zero octet for the root label. */
        if (wire + 1 > 255) return DNS_E_BADNAME;
        if (p + 1 + lab > cap) return DNS_E_OVERFLOW;
        buf[p++] = (uint8_t)lab;
        for (k = start; k < start + lab; k++)
            buf[p++] = (uint8_t)dns_lower((unsigned char)name[k]);
        if (i < n) i++;                    /* the dot between labels */
    }
    if (p >= cap) return DNS_E_OVERFLOW;
    buf[p++] = 0;
    *pos = p;
    return DNS_OK;
}

/* Decode a name at msg[pos], following RFC 1035 s4.1.4 pointers.
 *
 * Three rules, all enforced, all needed to survive hostile input:
 *
 *   1. A pointer's target must be strictly less than the offset of the
 *      pointer octet itself. RFC 1035 says a pointer addresses a *prior*
 *      occurrence, so a legal message never violates this; doing so can
 *      only be an attempt at a loop. Because every hop strictly decreases
 *      the offset, the walk terminates.
 *   2. At most DNS_MAX_POINTERS hops, as a second fence.
 *   3. The decoded name must stay within 255 octets of wire data
 *      (RFC 1035 s2.3.4), so a message cannot expand into a huge name.
 *
 * *consumed is how many octets of the message the encoded name occupied at
 * msg[pos] (a pointer counts as two), which is what the caller needs to
 * advance past it. */
static int dns_name_read(const uint8_t *msg, size_t len, size_t pos,
                         char *out, size_t cap, size_t *consumed)
{
    size_t p = pos, olen = 0, wire = 0, hops = 0;
    int jumped = 0;

    if (!msg || !out || cap == 0 || !consumed) return DNS_E_ARG;
    out[0] = 0;

    for (;;) {
        uint8_t c;
        if (p >= len) return DNS_E_WIRE;
        c = msg[p];

        if ((c & 0xC0) == 0xC0) {
            size_t off;
            if (p + 1 >= len) return DNS_E_WIRE;
            off = ((size_t)(c & 0x3F) << 8) | (size_t)msg[p + 1];
            if (!jumped) { *consumed = p + 2 - pos; jumped = 1; }
            if (off >= p) return DNS_E_COMPRESS;   /* not a prior occurrence */
            if (++hops > DNS_MAX_POINTERS) return DNS_E_COMPRESS;
            p = off;
            continue;
        }
        if ((c & 0xC0) != 0) return DNS_E_COMPRESS;  /* 0x40/0x80 label types */

        if (c == 0) {
            if (!jumped) *consumed = p + 1 - pos;
            if (olen >= cap) return DNS_E_OVERFLOW;
            out[olen] = 0;
            return DNS_OK;
        }

        wire += (size_t)c + 1;
        if (wire + 1 > 255) return DNS_E_COMPRESS;  /* would be over-long */
        if (p + 1 + (size_t)c > len) return DNS_E_WIRE;
        if (olen + (size_t)c + 2 > cap) return DNS_E_OVERFLOW;
        if (olen) out[olen++] = '.';
        memcpy(out + olen, msg + p + 1, c);
        olen += c;
        p += 1 + (size_t)c;
    }
}

/* ============================================================
 *  4. Messages
 *
 *  RFC 1035 s4.1.1 header, s4.1.2 question, s4.1.3 resource record.
 * ============================================================ */
typedef struct {
    const uint8_t *msg;
    size_t   len;
    size_t   pos;
    uint16_t id, flags, qd, an, ns, ar;
} DnsMsg;

typedef struct {
    char     owner[DNS_MAX_NAME];
    uint16_t type;
    uint16_t rclass;
    uint32_t ttl;      /* sanitised by RFC 2181 s5.1          */
    uint32_t ttl_raw;  /* as it appeared; the OPT RR needs it */
    size_t   rdata_pos;
    size_t   rdata_len;
} DnsRR;

static int dns_msg_open(DnsMsg *m, const uint8_t *msg, size_t len)
{
    if (!msg) return DNS_E_ARG;
    if (len < DNS_HEADER_LEN) return DNS_E_WIRE;
    m->msg = msg;
    m->len = len;
    m->pos = DNS_HEADER_LEN;
    m->id = dns_rd16(msg);
    m->flags = dns_rd16(msg + 2);
    m->qd = dns_rd16(msg + 4);
    m->an = dns_rd16(msg + 6);
    m->ns = dns_rd16(msg + 8);
    m->ar = dns_rd16(msg + 10);
    return DNS_OK;
}

static int dns_rr_read(DnsMsg *m, DnsRR *rr)
{
    size_t used = 0;
    int rc = dns_name_read(m->msg, m->len, m->pos, rr->owner, sizeof rr->owner, &used);

    if (rc != DNS_OK) return rc;
    m->pos += used;
    if (m->pos + 10 > m->len) return DNS_E_WIRE;
    rr->type = dns_rd16(m->msg + m->pos);
    rr->rclass = dns_rd16(m->msg + m->pos + 2);
    rr->ttl_raw = dns_rd32(m->msg + m->pos + 4);
    rr->ttl = dns_ttl(rr->ttl_raw);
    rr->rdata_len = dns_rd16(m->msg + m->pos + 8);
    m->pos += 10;
    rr->rdata_pos = m->pos;
    if (m->pos + rr->rdata_len > m->len) return DNS_E_WIRE;
    m->pos += rr->rdata_len;
    return DNS_OK;
}

/* The query: header, one question, one OPT RR when EDNS0 is on.
 *
 *     0                   1
 *    +---------------------+
 *    | ID                 |  from the resolver's counter (see dns_ask)
 *    | flags = RD        |
 *    | QDCOUNT = 1, AN/NS = 0
 *    | ARCOUNT = edns ? 1 : 0
 *    +---------------------+
 *    | QNAME, QTYPE, QCLASS
 *    +---------------------+
 *    | 0 | TYPE=OPT(41) | CLASS=udp size | TTL=0 (ext rcode/version/flags)
 *    | RDLENGTH = 0
 *    +---------------------+
 *
 *  RFC 6891 s6.1.1: the OPT owner name is the root, and s6.1.3: the four
 *  octets that are a TTL in every other RR are the extended RCODE, the
 *  EDNS version, and the flags. Version 0 and no flags is all this resolver
 *  needs, so the whole field is zero.
 */
static int dns_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *name,
                           uint16_t qtype, int edns, uint16_t udp_size, size_t *out_len)
{
    size_t p = 0;
    int rc;

    if (!buf || !name || !out_len) return DNS_E_ARG;
    if (cap < DNS_HEADER_LEN + 4) return DNS_E_OVERFLOW;

    memset(buf, 0, DNS_HEADER_LEN);
    dns_wr16(buf, id);
    dns_wr16(buf + 2, DNS_F_RD);
    dns_wr16(buf + 4, 1);
    dns_wr16(buf + 6, 0);
    dns_wr16(buf + 8, 0);
    dns_wr16(buf + 10, edns ? 1 : 0);
    p = DNS_HEADER_LEN;

    rc = dns_put_name(buf, cap, &p, name);
    if (rc != DNS_OK) return rc;
    if (p + 4 > cap) return DNS_E_OVERFLOW;
    dns_wr16(buf + p, qtype);
    dns_wr16(buf + p + 2, DNS_C_IN);
    p += 4;

    if (edns) {
        if (p + 11 > cap) return DNS_E_OVERFLOW;
        buf[p++] = 0;                      /* root owner name */
        dns_wr16(buf + p, DNS_T_OPT); p += 2;
        dns_wr16(buf + p, udp_size);  p += 2;
        dns_wr32(buf + p, ((uint32_t)DNS_EDNS_VERSION << 16)); p += 4;
        dns_wr16(buf + p, 0);         p += 2;
    }
    *out_len = p;
    return DNS_OK;
}

/* ============================================================
 *  5. Response parsing
 * ============================================================ */

/* Is `name` the same as `zone`, or a name below it? Used to decide whether
 * an SOA found in a reply actually speaks about the name the CNAME walk
 * ended at: RFC 2308 s2.2 puts the SOA of the zone containing that name in
 * the authority section, and an SOA for some other zone says nothing about
 * it (it must neither time the negative nor stop the chain). Comparison is
 * case insensitive on label boundaries (RFC 4343). */
static int dns_name_is_under(const char *name, const char *zone)
{
    size_t nl = strlen(name), zl = strlen(zone);
    if (zl == 0) return 1;                       /* the root covers everything */
    if (nl < zl) return 0;
    if (!dns_name_eq(name + (nl - zl), zone)) return 0;
    if (nl == zl) return 1;
    return name[nl - zl - 1] == '.';
}

/* Find the smallest negative-caching lifetime in one section, per RFC 2308
 * s5: "the TTL of this record is set from the minimum of the MINIMUM field
 * of the SOA record and the TTL of the SOA itself". Only an SOA that covers
 * `final` is considered (see dns_name_is_under); one for another zone is
 * skipped. `*have` records that an SOA was *seen*, so a legitimate TTL of 0
 * is not mistaken for "no SOA in this section" and then overwritten by a
 * later record. */
static int dns_scan_soa(const DnsMsg *base, uint16_t count, const char *final,
                        uint32_t *neg_ttl, int *have)
{
    DnsMsg m = *base;
    int i;
    for (i = 0; i < (int)count; i++) {
        DnsRR rr;
        char mname[DNS_MAX_NAME], rname[DNS_MAX_NAME];
        size_t u1 = 0, u2 = 0, min_pos;
        uint32_t minimum, ttl;
        int rc = dns_rr_read(&m, &rr);
        if (rc != DNS_OK) return rc;
        if (rr.type != DNS_T_SOA || rr.rclass != DNS_C_IN) continue;
        if (!dns_name_is_under(final, rr.owner)) continue;
        /* RFC 1035 s3.3.13: MNAME, RNAME, SERIAL, REFRESH, RETRY, EXPIRE,
         * MINIMUM - the last five are 32 bit fields, so MINIMUM is 16 octets
         * after the two names. */
        rc = dns_name_read(m.msg, m.len, rr.rdata_pos, mname, sizeof mname, &u1);
        if (rc != DNS_OK) return rc;
        if (u1 > rr.rdata_len) return DNS_E_WIRE;
        rc = dns_name_read(m.msg, m.len, rr.rdata_pos + u1, rname, sizeof rname, &u2);
        if (rc != DNS_OK) return rc;
        if (u1 + u2 + 20 > rr.rdata_len) return DNS_E_WIRE;
        min_pos = rr.rdata_pos + u1 + u2 + 16;
        minimum = dns_ttl(dns_rd32(m.msg + min_pos));
        ttl = rr.ttl;
        if (minimum < ttl) ttl = minimum;
        if (!*have || ttl < *neg_ttl) *neg_ttl = ttl;
        *have = 1;
    }
    return DNS_OK;
}

/* Turn one reply into a DnsAnswer.
 *
 * The CNAME walk happens inside the message first (RFC 1034 s4.3.2: a server
 * that has the chain answers with it), and dns_resolve() takes over when the
 * chain leaves the message. The answer's TTL is the minimum over every RR
 * used, including the CNAMEs (RFC 2181 s5.2, RFC 1034 s5.3.3).
 */
static int dns_parse_reply(const uint8_t *msg, size_t len, uint16_t expect_id,
                           const char *qname, uint16_t qtype, DnsAnswer *out,
                           uint32_t *neg_ttl)
{
    DnsMsg m;
    size_t q_end, an_end;
    char cur[DNS_MAX_NAME], final[DNS_MAX_NAME];
    char visited[DNS_MAX_CNAME_DEPTH + 2][DNS_MAX_NAME];
    int nvis = 0, depth, have_data = 0, have_ptr = 0, i;
    uint32_t min_ttl = 0xFFFFFFFFu;
    uint16_t rcode;
    int rc;

    if (!msg || !qname || !out) return DNS_E_ARG;
    memset(out, 0, sizeof *out);
    if (neg_ttl) *neg_ttl = 0;

    rc = dns_msg_open(&m, msg, len);
    if (rc != DNS_OK) return rc;

    /* RFC 1035 s4.1.1: QR distinguishes a response, and the opcode must be
     * the standard QUERY (0) for this resolver to understand it. */
    if (!(m.flags & DNS_F_QR)) return DNS_E_WIRE;
    if (m.id != expect_id) return DNS_E_ID;
    if ((m.flags & DNS_F_OPCODE) != 0) return DNS_E_RCODE;
    rcode = (uint16_t)(m.flags & 0x000F);

    /* Section 4.1.2: skip the question section. Exactly one question is
     * expected for a standard query, but a server is allowed to echo none
     * (some do on FORMERR), so this only advances, it does not require. */
    for (i = 0; i < (int)m.qd; i++) {
        char tmp[DNS_MAX_NAME];
        size_t used = 0;
        rc = dns_name_read(m.msg, m.len, m.pos, tmp, sizeof tmp, &used);
        if (rc != DNS_OK) return rc;
        m.pos += used;
        if (m.pos + 4 > m.len) return DNS_E_WIRE;
        m.pos += 4;
    }
    q_end = m.pos;

    if (!dns_name_copy_lower(cur, sizeof cur, qname)) return DNS_E_BADNAME;
    memcpy(final, cur, strlen(cur) + 1);
    memcpy(visited[nvis++], cur, strlen(cur) + 1);

    /* First pass: walk the CNAME chain that is inside this message. */
    for (depth = 0; depth <= DNS_MAX_CNAME_DEPTH; depth++) {
        size_t save = m.pos;
        char target[DNS_MAX_NAME];
        uint32_t ttl = 0;
        int found = 0;

        for (i = 0; i < (int)m.an; i++) {
            DnsRR rr;
            size_t u = 0;
            rc = dns_rr_read(&m, &rr);
            if (rc != DNS_OK) return rc;
            if (rr.type != DNS_T_CNAME || rr.rclass != DNS_C_IN) continue;
            if (!dns_name_eq(rr.owner, cur)) continue;
            rc = dns_name_read(m.msg, m.len, rr.rdata_pos, target, sizeof target, &u);
            if (rc != DNS_OK) return rc;
            if (u > rr.rdata_len) return DNS_E_WIRE;
            ttl = rr.ttl;
            found = 1;
            break;
        }
        m.pos = save;
        if (!found) break;
        if (ttl < min_ttl) min_ttl = ttl;

        for (i = 0; i < nvis; i++)
            if (dns_name_eq(visited[i], target)) return DNS_E_CYCLE;
        if (depth + 1 > DNS_MAX_CNAME_DEPTH) return DNS_E_DEPTH;
        if (!dns_name_copy_lower(cur, sizeof cur, target)) return DNS_E_BADNAME;
        memcpy(final, cur, strlen(cur) + 1);
        if (nvis < (int)(sizeof visited / sizeof visited[0]))
            memcpy(visited[nvis++], cur, strlen(cur) + 1);
    }

    /* Second pass: the records of the requested type, owned by the end of
     * the chain. Records for any other owner are other answers to the same
     * question over a different path, and are not ours to return. */
    m.pos = q_end;
    for (i = 0; i < (int)m.an; i++) {
        DnsRR rr;
        rc = dns_rr_read(&m, &rr);
        if (rc != DNS_OK) return rc;
        if (rr.rclass != DNS_C_IN) continue;
        if (rr.type != qtype) continue;
        if (!dns_name_eq(rr.owner, final)) continue;
        if (rr.ttl < min_ttl) min_ttl = rr.ttl;

        if (qtype == DNS_T_A) {
            if (rr.rdata_len != 4) return DNS_E_WIRE;   /* RFC 1035 s3.4.1 */
            if (out->naddr < DNS_MAX_ADDRS) {
                out->addr[out->naddr].family = DNS_AF_INET;
                out->addr[out->naddr].port = 0;
                memcpy(out->addr[out->naddr].bytes, m.msg + rr.rdata_pos, 4);
                out->naddr++;
            }
            have_data = 1;
        } else if (qtype == DNS_T_AAAA) {
            if (rr.rdata_len != 16) return DNS_E_WIRE;  /* RFC 3596 s2.2 */
            if (out->naddr < DNS_MAX_ADDRS) {
                out->addr[out->naddr].family = DNS_AF_INET6;
                out->addr[out->naddr].port = 0;
                memcpy(out->addr[out->naddr].bytes, m.msg + rr.rdata_pos, 16);
                out->naddr++;
            }
            have_data = 1;
        } else if (qtype == DNS_T_PTR) {
            size_t u = 0;
            char target[DNS_MAX_NAME];
            rc = dns_name_read(m.msg, m.len, rr.rdata_pos, target, sizeof target, &u);
            if (rc != DNS_OK) return rc;
            if (u > rr.rdata_len) return DNS_E_WIRE;
            /* The first PTR wins. The flag, not target[0], decides: a PTR
             * to the root is a legal record whose name is the empty string,
             * and it must not be mistaken for "no PTR seen yet". */
            if (!have_ptr) {
                memcpy(out->ptr, target, strlen(target) + 1);
                have_ptr = 1;
            }
            have_data = 1;
        } else {
            /* Any other type: the RRset exists, the caller asked only for
             * existence and the TTL, and the records are not copied. */
            have_data = 1;
        }
    }
    an_end = m.pos;

    /* Authority section: the SOA that times a negative answer. */
    if (neg_ttl) {
        uint32_t neg = 0;
        int have_soa = 0;
        DnsMsg tmp = m;
        tmp.pos = an_end;
        rc = dns_scan_soa(&tmp, m.ns, final, &neg, &have_soa);
        if (rc != DNS_OK) return rc;
        if (!have_soa) {
            /* Some servers put the SOA in the answer section for a NODATA
             * reply; RFC 2308 s3 puts it in authority, but honouring both
             * costs one pass and avoids giving up on a legal-if-unusual
             * message. */
            tmp = m;
            tmp.pos = q_end;
            rc = dns_scan_soa(&tmp, m.an, final, &neg, &have_soa);
            if (rc != DNS_OK) return rc;
        }
        *neg_ttl = have_soa ? neg : 0;
    }

    /* Additional section: the OPT RR carries the top eight bits of the
     * RCODE, which is how BADVERS (16) is expressible (RFC 6891 s6.1.3). */
    m.pos = an_end;
    for (i = 0; i < (int)m.ns; i++) {
        DnsRR rr;
        rc = dns_rr_read(&m, &rr);
        if (rc != DNS_OK) return rc;
    }
    for (i = 0; i < (int)m.ar; i++) {
        DnsRR rr;
        rc = dns_rr_read(&m, &rr);
        if (rc != DNS_OK) return rc;
        if (rr.type != DNS_T_OPT) continue;
        if (rr.owner[0] != 0) return DNS_E_WIRE;       /* RFC 6891 s6.1.1 */
        {
            uint32_t ttl = rr.ttl_raw;
            uint16_t ext = (uint16_t)((ttl >> 24) & 0xFF);
            uint16_t version = (uint16_t)((ttl >> 16) & 0xFF);
            if (version != DNS_EDNS_VERSION) {
                out->rcode = DNS_RCODE_BADVERS;
                return DNS_E_RCODE;
            }
            if (ext) rcode = (uint16_t)((ext << 4) | rcode);
        }
    }

    out->rcode = rcode;
    out->ttl = (min_ttl == 0xFFFFFFFFu) ? 0u : min_ttl;
    memcpy(out->cname, final, strlen(final) + 1);

    if (rcode == DNS_RCODE_NXDOMAIN) return DNS_E_NXDOMAIN;
    if (rcode != DNS_RCODE_NOERROR) return DNS_E_RCODE;
    return have_data ? DNS_OK : DNS_E_NODATA;
}

/* ============================================================
 *  6. Cache
 *
 *  Positive entries are keyed by (name, type). Negative entries follow
 *  RFC 2308 s2.1: an NXDOMAIN answer is cached against the name alone
 *  (type 0 here matches every query for that name), while a NODATA answer
 *  is cached against (name, type), because the name exists and only that
 *  one type is empty.
 *
 *  Lifetimes are kept in milliseconds so a 1 second TTL does not expire
 *  early or late because of when the clock ticked.
 * ============================================================ */
static uint64_t dns_cache_left_ms(const DnsResolver *r, const DnsCacheEntry *e)
{
    return e->expire_ms > r->now ? e->expire_ms - r->now : 0;
}

/* The clock for the two inspection functions below. They are called between
 * lookups (a status line, a test), when r->now is as old as the last query,
 * so they read the ops clock directly instead. r is not modified. */
static uint64_t dns_cache_now(const DnsResolver *r)
{
    if (r->ops && r->ops->now_ms) return r->ops->now_ms(r->ops->ctx);
    return r->now;
}

static int dns_cache_lookup(DnsResolver *r, const char *name, uint16_t type,
                           DnsAnswer *out)
{
    int i;
    for (i = 0; i < DNS_CACHE_MAX; i++) {
        DnsCacheEntry *e = &r->cache[i];
        uint64_t left, secs;
        int k;

        if (!e->used) continue;
        if (!dns_name_eq(e->name, name)) continue;
        if (e->type != type && e->type != 0) continue;

        left = dns_cache_left_ms(r, e);
        if (left == 0) { e->used = 0; continue; }

        /* Round up: a live entry must never report a TTL of 0, which would
         * read as "do not cache" to a caller. */
        secs = (left + 999u) / 1000u;
        if (secs > 0xFFFFFFFFu) secs = 0xFFFFFFFFu;

        memset(out, 0, sizeof *out);
        out->ttl = (uint32_t)secs;
        out->from_cache = 1;
        out->rcode = e->rcode;
        out->qtype = type;
        out->naddr = e->naddr;
        memcpy(out->name, e->name, strlen(e->name) + 1);
        memcpy(out->cname, e->cname, strlen(e->cname) + 1);
        memcpy(out->ptr, e->ptr, strlen(e->ptr) + 1);
        for (k = 0; k < e->naddr && k < DNS_MAX_ADDRS; k++) out->addr[k] = e->addr[k];

        if (e->neg) {
            r->stats.neg_hits++;
            return e->rcode == DNS_RCODE_NXDOMAIN ? DNS_E_NXDOMAIN : DNS_E_NODATA;
        }
        r->stats.cache_hits++;
        return DNS_OK;
    }
    return DNS_E_NOCACHE;
}

/* Store one entry. Eviction: the first free or expired slot, otherwise the
 * entry that expires soonest (ties go to the lowest index), so the choice is
 * deterministic and a test can predict which slot is reused. */
static void dns_cache_store(DnsResolver *r, const char *name, uint16_t type,
                            int neg, uint16_t rcode, uint32_t ttl,
                            const DnsAnswer *a)
{
    int i, slot = -1;
    uint64_t best = 0, ms;
    DnsCacheEntry *e;

    if (ttl == 0) {
        /* RFC 2181 s8: a TTL of 0 means "use only for this transaction". */
        r->stats.refused_ttl_zero++;
        return;
    }
    ms = (uint64_t)ttl * 1000u;
    if (r->now > UINT64_MAX - ms) ms = UINT64_MAX - r->now;

    for (i = 0; i < DNS_CACHE_MAX; i++) {
        if (!r->cache[i].used) { slot = i; break; }
        if (dns_cache_left_ms(r, &r->cache[i]) == 0) { slot = i; break; }
        if (slot < 0 || r->cache[i].expire_ms < best) {
            best = r->cache[i].expire_ms;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;

    e = &r->cache[slot];
    memset(e, 0, sizeof *e);
    /* Fill the key first and only then mark the slot live, so a name that
     * cannot be copied leaves the slot empty instead of claiming an entry
     * for the empty name. */
    if (!dns_name_copy_lower(e->name, sizeof e->name, name)) return;
    e->used = 1;
    e->neg = neg ? 1 : 0;
    e->type = type;
    e->rcode = rcode;
    e->expire_ms = r->now + ms;
    if (a) {
        e->naddr = (uint8_t)(a->naddr < DNS_MAX_ADDRS ? a->naddr : DNS_MAX_ADDRS);
        for (i = 0; i < e->naddr; i++) e->addr[i] = a->addr[i];
        memcpy(e->cname, a->cname, DNS_MAX_NAME);
        e->cname[DNS_MAX_NAME - 1] = 0;
        memcpy(e->ptr, a->ptr, DNS_MAX_PTR);
        e->ptr[DNS_MAX_PTR - 1] = 0;
    }
    if (neg) r->stats.stored_negative++;
    else     r->stats.stored_positive++;
}

void dns_cache_flush(DnsResolver *r)
{
    if (!r) return;
    memset(r->cache, 0, sizeof r->cache);
}

int dns_cache_count(const DnsResolver *r)
{
    int i, n = 0;
    uint64_t now;
    if (!r) return 0;
    now = dns_cache_now(r);
    for (i = 0; i < DNS_CACHE_MAX; i++) {
        if (!r->cache[i].used) continue;
        if (r->cache[i].expire_ms <= now) continue;
        n++;
    }
    return n;
}

/* The remaining lifetime of the entry a lookup for (name,type) would be
 * answered from, rounded up like dns_cache_lookup() does so a live entry
 * never reads as 0. A negative entry stored for every type (NXDOMAIN,
 * type 0) answers any type, exactly as the lookup does. */
int dns_cache_ttl(const DnsResolver *r, const char *name, uint16_t qtype)
{
    int i;
    uint64_t now;
    if (!r || !name) return 0;
    now = dns_cache_now(r);
    for (i = 0; i < DNS_CACHE_MAX; i++) {
        const DnsCacheEntry *e = &r->cache[i];
        uint64_t left, secs;
        if (!e->used) continue;
        if (!dns_name_eq(e->name, name)) continue;
        if (e->type != qtype && e->type != 0) continue;
        left = e->expire_ms > now ? e->expire_ms - now : 0;
        if (left == 0) continue;
        secs = (left + 999u) / 1000u;
        if (secs > 0x7FFFFFFFu) secs = 0x7FFFFFFFu;   /* fits the int return */
        return (int)secs;
    }
    return 0;
}

/* ============================================================
 *  7. Transport
 *
 *  The retry schedule is caller policy (DnsConfig), not protocol. The form
 *  is: every round tries every configured server once, and the timeout for
 *  round k is timeout_ms << k, capped at timeout_max_ms. Two servers, two
 *  attempts, 1000/8000 therefore produce 1000, 1000, 2000, 2000 ms. Each
 *  attempt is recorded in r->trace so the schedule is inspectable rather
 *  than asserted.
 * ============================================================ */
static int dns_backoff_ms(const DnsResolver *r, int round)
{
    long t = r->timeout_ms;
    int k;
    if (t <= 0) t = 1;
    for (k = 0; k < round; k++) {
        if (t >= r->timeout_max_ms / 2) { t = r->timeout_max_ms; break; }
        t *= 2;
    }
    if (r->timeout_max_ms > 0 && t > r->timeout_max_ms) t = r->timeout_max_ms;
    if (t > 3600000) t = 3600000;
    return (int)t;
}

static void dns_trace_add(DnsResolver *r, int server, int round, int tcp,
                          int edns, int result, int timeout_ms)
{
    DnsAttempt *t;
    if (r->ntrace >= DNS_MAX_TRACE) return;
    t = &r->trace[r->ntrace++];
    t->server = (uint8_t)server;
    t->round = (uint8_t)(round > 255 ? 255 : round);
    t->tcp = (uint8_t)(tcp ? 1 : 0);
    t->edns = (uint8_t)(edns ? 1 : 0);
    t->result = (int16_t)result;
    t->timeout_ms = (int32_t)timeout_ms;
}

/* One attempt against one server, including the TCP retry if the UDP reply
 * is truncated and the EDNS fallback if the server says FORMERR/NOTIMP.
 * Returns the DNS_E_* of the attempt (or DNS_OK). */
static int dns_try_server(DnsResolver *r, int si, int round, const char *name,
                          uint16_t type, uint16_t id, int edns, int timeout_ms,
                          DnsAnswer *out, uint32_t *neg_ttl)
{
    uint8_t q[DNS_HEADER_LEN + DNS_MAX_NAME + 4 + 11];
    uint8_t resp[DNS_MAX_MSG];
    size_t qlen = 0, rlen;
    int rc;

    rc = dns_build_query(q, sizeof q, id, name, type, edns, r->udp_size, &qlen);
    if (rc != DNS_OK) return rc;

    r->stats.queries++;
    rc = r->ops->udp_xchg(r->ops->ctx, &r->srv[si], q, qlen, resp, sizeof resp,
                          timeout_ms);
    dns_trace_add(r, si, round, 0, edns, rc > 0 ? DNS_OK : (rc ? rc : DNS_E_TIMEOUT),
                  timeout_ms);
    if (rc == 0) {                             /* nothing arrived */
        r->stats.timeouts++;                   /* same as an explicit timeout */
        return DNS_E_TIMEOUT;
    }
    if (rc < 0) {
        if (rc == DNS_E_TIMEOUT) r->stats.timeouts++;
        return rc;
    }
    /* RFC 1035 s4.2.1: TC means the reply did not fit in the transport's
     * limit, so ask again over TCP (s4.2.2 for the framing, which is the
     * ops' job). The ID is checked before that: a datagram with someone
     * else's ID must not be able to make this resolver open a connection.
     * A TCP failure is not fatal: the next server may answer. */
    if ((size_t)rc >= DNS_HEADER_LEN && dns_rd16(resp) == id &&
        (dns_rd16(resp + 2) & DNS_F_TC)) {
        int trc;
        r->stats.tc_retries++;
        trc = r->ops->tcp_xchg(r->ops->ctx, &r->srv[si], q, qlen, resp,
                               sizeof resp, timeout_ms);
        dns_trace_add(r, si, round, 1, edns,
                      trc > 0 ? DNS_OK : (trc ? trc : DNS_E_TIMEOUT), timeout_ms);
        if (trc <= 0) return trc < 0 ? trc : DNS_E_TCP;
        rc = trc;
        rlen = (size_t)rc;
        rc = dns_parse_reply(resp, rlen, id, name, type, out, neg_ttl);
        out->used_tcp = 1;
        return rc;
    }

    rc = dns_parse_reply(resp, (size_t)rc, id, name, type, out, neg_ttl);
    if (rc == DNS_E_ID) r->stats.id_mismatch++;
    else if (rc == DNS_E_WIRE || rc == DNS_E_COMPRESS || rc == DNS_E_OVERFLOW)
        r->stats.malformed++;
    else if (rc == DNS_E_RCODE) r->stats.rcode_errors++;
    return rc;
}

/* Ask the configured servers for one name. Returns DNS_OK with data, a
 * negative result the caller may cache, or the last transport error. */
static int dns_ask(DnsResolver *r, const char *name, uint16_t type,
                   DnsAnswer *out, uint32_t *neg_ttl)
{
    int total = r->nsrv * r->attempts;
    int i, last = DNS_E_NOSERVER;

    if (r->nsrv <= 0) return DNS_E_NOSERVER;
    if (r->attempts <= 0) return DNS_E_ARG;

    r->now = r->ops->now_ms(r->ops->ctx);

    for (i = 0; i < total; i++) {
        int si = i % r->nsrv;
        int round = i / r->nsrv;
        int tmo = dns_backoff_ms(r, round);
        int rc;
        /* A fresh ID per attempt: a late reply to an earlier attempt then
         * cannot be mistaken for the answer to this one. The counter is
         * seeded from the clock in dns_resolver_init(); see the note there
         * about what that does and does not defend against. */
        uint16_t id;
        r->next_id = (uint16_t)(r->next_id + 1);
        id = r->next_id;

        rc = dns_try_server(r, si, round, name, type, id, r->edns ? 1 : 0,
                            tmo, out, neg_ttl);

        /* RFC 6891 s6.2.2/7: a server that has never heard of EDNS0 answers
         * FORMERR (or NOTIMP). One retry without the OPT RR tells us whether
         * the name can be resolved by that server at all. */
        if (rc == DNS_E_RCODE && r->edns && out->rcode != DNS_RCODE_BADVERS &&
            (out->rcode == DNS_RCODE_FORMERR || out->rcode == DNS_RCODE_NOTIMP)) {
            r->stats.edns_fallbacks++;
            /* A fresh ID here too: the retry is a new attempt, and a late
             * reply to the first one must not be taken for its answer. */
            r->next_id = (uint16_t)(r->next_id + 1);
            rc = dns_try_server(r, si, round, name, type, r->next_id, 0, tmo,
                                out, neg_ttl);
        }

        if (rc == DNS_OK || rc == DNS_E_NXDOMAIN || rc == DNS_E_NODATA) {
            out->qtype = type;
            return rc;
        }
        /* SERVFAIL, REFUSED, a mismatched ID or a corrupt message: this
         * server could not answer this time. Try the next one. */
        last = rc;
    }
    return last;
}

/* ============================================================
 *  8. dns_resolve()
 * ============================================================ */
static int dns_hosts_lookup(const DnsResolver *r, const char *name, uint16_t qtype,
                            DnsAnswer *out);

int dns_resolve(DnsResolver *r, const char *name, uint16_t qtype, DnsAnswer *out)
{
    char qn[DNS_MAX_NAME];
    char cur[DNS_MAX_NAME];
    char visited[DNS_MAX_CNAME_DEPTH + 2][DNS_MAX_NAME];
    int nvis = 0, depth = 0, rc;
    uint32_t chain_ttl;
    size_t qlen, k;
    uint8_t probe[DNS_HEADER_LEN + DNS_MAX_NAME + 4];
    size_t probe_len = 0;

    if (!r || !r->ops || !out || !name) return DNS_E_ARG;
    if (!r->ops->udp_xchg || !r->ops->now_ms) return DNS_E_ARG;
    /* "example.com." and "example.com" are the same name: the root label is
     * implicit in the presentation form, and dns_put_name() has always
     * dropped one trailing dot from the query. Normalise it once, here,
     * before anything uses the name, so the hosts key, the cache key, the
     * comparison against wire owner names and out->name all agree. */
    qlen = strlen(name);
    if (qlen && name[qlen - 1] == '.') {
        /* ".." and "a.." stay invalid: only the root label is implicit. */
        if (qlen >= 2 && name[qlen - 2] == '.') return DNS_E_BADNAME;
        qlen--;
    }
    if (qlen + 1 > sizeof qn) return DNS_E_BADNAME;
    for (k = 0; k < qlen; k++) qn[k] = (char)dns_lower((unsigned char)name[k]);
    qn[qlen] = 0;

    memset(out, 0, sizeof *out);
    memcpy(out->name, qn, strlen(qn) + 1);
    out->qtype = qtype;

    /* Validate the name before anything else, so the result of a lookup is
     * the same whether or not a cache or hosts entry would have answered. */
    rc = dns_put_name(probe, sizeof probe, &probe_len, qn);
    if (rc != DNS_OK) return rc;

    r->now = r->ops->now_ms(r->ops->ctx);
    r->ntrace = 0;

    /* /etc/hosts first: it is the local administrator's override, and it is
     * not cached, so an edit takes effect on the next lookup. A machine with
     * no nameserver at all still resolves the names in this file, so the
     * DNS_E_NOSERVER check comes after it (and after the cache). */
    if ((qtype == DNS_T_A || qtype == DNS_T_AAAA) &&
        dns_hosts_lookup(r, qn, qtype, out) == DNS_OK) {
        out->name[0] = 0;
        memcpy(out->name, qn, strlen(qn) + 1);
        out->qtype = qtype;
        r->stats.hosts_hits++;
        return DNS_OK;
    }

    rc = dns_cache_lookup(r, qn, qtype, out);
    if (rc != DNS_E_NOCACHE) {
        if (rc == DNS_OK) out->qtype = qtype;
        return rc;
    }

    if (!r->nsrv) return DNS_E_NOSERVER;

    memcpy(cur, qn, strlen(qn) + 1);
    memcpy(visited[nvis++], cur, strlen(cur) + 1);

    /* RFC 1034 s5.3.3: when the chain is answered by more than one message
     * the answer to the name that was asked is only as durable as the least
     * durable link in it, so the TTLs of every message on the chain are
     * combined with the final one. 0xFFFFFFFF means "no link seen yet". */
    chain_ttl = 0xFFFFFFFFu;

    for (;;) {
        DnsAnswer a;
        uint32_t neg = 0;
        uint32_t ttl;

        memset(&a, 0, sizeof a);
        rc = dns_ask(r, cur, qtype, &a, &neg);

        if (rc == DNS_OK) {
            /* The last message's own minimum, and the least durable link of
             * any message the chain passed through before it. */
            ttl = a.ttl < chain_ttl ? a.ttl : chain_ttl;
            memcpy(out->name, qn, strlen(qn) + 1);
            memcpy(out->cname, a.cname, DNS_MAX_NAME);
            memcpy(out->ptr, a.ptr, DNS_MAX_PTR);
            out->naddr = a.naddr;
            out->ttl = ttl;
            out->rcode = a.rcode;
            out->used_tcp = a.used_tcp;
            out->qtype = qtype;
            memcpy(out->addr, a.addr, sizeof a.addr);
            /* RFC 1034 s5.3.3: the answer to the name that was asked is
             * stored with the minimum TTL of the chain that led to it. TTL 0
             * is refused by dns_cache_store (RFC 2181 s8). */
            dns_cache_store(r, qn, qtype, 0, 0, ttl, &a);
            return DNS_OK;
        }

        if (rc == DNS_E_NXDOMAIN) {
            /* RFC 2308 s2.1: NXDOMAIN is cached for the name and every type.
             * The SOA that timed it covers the name the reply ended at, so
             * when a CNAME inside this message moved us to another name the
             * entry belongs to that name, not to the alias: the alias is only
             * as durable as its own link TTL, and a later change to it must
             * not stay hidden behind the target's negative entry. `moved` is
             * a name comparison, not a test of a.cname[0]: a CNAME to the
             * root is a legal move to the name "". */
            int moved = !dns_name_eq(a.cname, cur);
            if (neg) dns_cache_store(r, moved ? a.cname : cur, 0, 1,
                                     DNS_RCODE_NXDOMAIN, neg, &a);
            memcpy(out->cname, a.cname, DNS_MAX_NAME);
            out->rcode = DNS_RCODE_NXDOMAIN;
            /* RFC 2308 s5 times the negative answer; the alias is only good
             * for as long as the shortest link that led to it. */
            out->ttl = neg < chain_ttl ? neg : chain_ttl;
            if (moved && a.ttl < out->ttl) out->ttl = a.ttl;
            return DNS_E_NXDOMAIN;
        }

        if (rc == DNS_E_NODATA) {
            /* Same rule as NXDOMAIN, including a CNAME to the root. */
            int moved = !dns_name_eq(a.cname, cur);
            /* A CNAME in the reply with no data for the target means the
             * chain continued past this message - unless the reply also
             * carried the SOA that times the *final* name (RFC 2308 s2.2),
             * in which case the negative answer is already complete for that
             * name and a second query would only risk replacing it with a
             * timeout. */
            if (moved && !neg) {
                int seen = 0, j;
                for (j = 0; j < nvis; j++)
                    if (dns_name_eq(visited[j], a.cname)) seen = 1;
                if (seen) return DNS_E_CYCLE;
                if (depth >= DNS_MAX_CNAME_DEPTH) return DNS_E_DEPTH;
                /* Keep the TTL of the link we are about to leave behind. */
                if (a.ttl < chain_ttl) chain_ttl = a.ttl;
                depth++;
                memcpy(cur, a.cname, strlen(a.cname) + 1);
                if (nvis < (int)(sizeof visited / sizeof visited[0]))
                    memcpy(visited[nvis++], cur, strlen(cur) + 1);
                continue;
            }
            /* RFC 2308 s2.1: NODATA is cached for this name and this type
             * only, and only when an SOA said how long (neg != 0). */
            if (neg) dns_cache_store(r, moved ? a.cname : cur, qtype, 1,
                                     DNS_RCODE_NOERROR, neg, &a);
            memcpy(out->cname, a.cname, DNS_MAX_NAME);
            out->rcode = DNS_RCODE_NOERROR;
            out->ttl = neg < chain_ttl ? neg : chain_ttl;
            if (moved && a.ttl < out->ttl) out->ttl = a.ttl;
            return DNS_E_NODATA;
        }

        /* No usable answer from any server. */
        memcpy(out->cname, a.cname, DNS_MAX_NAME);
        out->rcode = a.rcode;
        return rc;
    }
}

int dns_lookup_a(DnsResolver *r, const char *name, DnsAnswer *out)
{
    return dns_resolve(r, name, DNS_T_A, out);
}

int dns_lookup_aaaa(DnsResolver *r, const char *name, DnsAnswer *out)
{
    return dns_resolve(r, name, DNS_T_AAAA, out);
}

int dns_lookup_ptr(DnsResolver *r, uint16_t family, const uint8_t addr[16],
                   DnsAnswer *out)
{
    char rev[DNS_MAX_NAME];
    int rc;
    if (!addr) return DNS_E_ARG;
    rc = dns_ptr_name(family, addr, rev, sizeof rev);
    if (rc != DNS_OK) return rc;
    return dns_resolve(r, rev, DNS_T_PTR, out);
}

/* ============================================================
 *  9. Configuration files
 * ============================================================ */

/* Grab the next whitespace-delimited token from [p,end), returning 0 when
 * there is none. The token is not copied and is not NUL terminated. */
static int dns_token(const char **pp, const char *end, const char **tok, size_t *len)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    if (p >= end || *p == '\n') { *pp = p; return 0; }
    *tok = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    *len = (size_t)(p - *tok);
    *pp = p;
    return 1;
}

static int dns_token_addr(const char *tok, size_t len, DnsAddr *out)
{
    char buf[64];
    if (len == 0 || len >= sizeof buf) return DNS_E_BADNAME;
    memcpy(buf, tok, len);
    buf[len] = 0;
    return dns_addr_pton(buf, out);
}

/* Walk the lines of a text buffer. On each call [*line, *le) is the next
 * line with any '#' comment removed, and *pp is left at the first octet of
 * the following line. The cursor always advances (a line that is only a
 * comment, an empty line, and the newline of the previous line all make
 * progress), which is the property that keeps a parser from spinning.
 * Returns 0 at end of buffer. */
static int dns_next_line(const char **pp, const char *end,
                         const char **line, const char **le)
{
    const char *p = *pp, *c;
    if (p >= end) return 0;
    *line = p;
    while (p < end && *p != '\n') p++;
    *le = p;                          /* the newline, or end of buffer */
    for (c = *line; c < *le; c++) {
        if (*c == '#') { *le = c; break; }
    }
    if (p < end) p++;                 /* step over the newline */
    *pp = p;
    return 1;
}

/* /etc/hosts: "address name [name ...]" per line, '#' starts a comment.
 *
 * A hosts file has no TTL field, so an answer taken from it carries TTL 0
 * and is never cached. That is deliberate: TTL 0 means "this answer, now"
 * (RFC 2181 s8), and it keeps the file authoritative from one lookup to the
 * next instead of a stale copy outliving an edit.
 */
static int dns_hosts_lookup(const DnsResolver *r, const char *name, uint16_t qtype,
                            DnsAnswer *out)
{
    const char *p, *end;
    int found = 0;

    if (!r || !out || !name) return DNS_E_ARG;
    if (qtype != DNS_T_A && qtype != DNS_T_AAAA) return DNS_E_NOCACHE;
    if (!r->hosts_loaded || r->hosts_len == 0) return DNS_E_NOCACHE;
    p = r->hosts;
    end = r->hosts + r->hosts_len;

    while (p < end) {
        const char *line, *le, *lp;
        const char *tok;
        size_t len;
        DnsAddr a;

        if (!dns_next_line(&p, end, &line, &le)) break;
        lp = line;
        if (!dns_token(&lp, le, &tok, &len)) continue;
        if (dns_token_addr(tok, len, &a) != DNS_OK) continue;
        while (dns_token(&lp, le, &tok, &len)) {
            if (!dns_token_eq(tok, len, name)) continue;
            if (qtype == DNS_T_A && a.family != DNS_AF_INET) continue;
            if (qtype == DNS_T_AAAA && a.family != DNS_AF_INET6) continue;
            if (out->naddr < DNS_MAX_ADDRS) {
                out->addr[out->naddr++] = a;
                found = 1;
            }
        }
    }
    if (!found) return DNS_E_NOCACHE;
    out->ttl = 0;
    out->rcode = DNS_RCODE_NOERROR;
    out->from_hosts = 1;
    if (!dns_name_copy_lower(out->cname, sizeof out->cname, name)) return DNS_E_BADNAME;
    return DNS_OK;
}

int dns_parse_hosts(DnsResolver *r, const char *text, size_t len)
{
    const char *p, *end;
    if (!r || (!text && len)) return DNS_E_ARG;
    /* Validate every address up front, so a typo is reported instead of
     * being quietly skipped and the name then going to the network. */
    p = text; end = text + len;
    while (p < end) {
        const char *line, *le, *lp;
        const char *tok;
        size_t tl;
        DnsAddr a;
        if (!dns_next_line(&p, end, &line, &le)) break;
        lp = line;
        if (!dns_token(&lp, le, &tok, &tl)) continue;
        if (dns_token_addr(tok, tl, &a) != DNS_OK) return DNS_E_WIRE;
    }
    if (len >= sizeof r->hosts) return DNS_E_OVERFLOW;
    if (len) memcpy(r->hosts, text, len);
    r->hosts[len] = 0;
    r->hosts_len = len;
    r->hosts_loaded = 1;
    return DNS_OK;
}

int dns_hosts_reload(DnsResolver *r)
{
    size_t n = 0;
    int rc;
    if (!r || !r->ops || !r->ops->read_file) return DNS_E_ARG;
    r->hosts_loaded = 0;
    r->hosts_len = 0;
    rc = r->ops->read_file(r->ops->ctx, "/etc/hosts", r->hosts, sizeof r->hosts, &n);
    if (rc != DNS_OK) return rc;
    if (n >= sizeof r->hosts) n = sizeof r->hosts - 1;
    return dns_parse_hosts(r, r->hosts, n);
}

/* resolv.conf. Only the directives this resolver can act on are read:
 * "nameserver <address>", "options timeout:<seconds>" (the file counts
 * seconds, DnsConfig counts milliseconds) and "options attempts:<n>". Every
 * other keyword is skipped, which is what the format expects a reader to do
 * with what it does not understand. */
static long dns_dec(const char *s, size_t n, int *ok)
{
    long v = 0;
    size_t k;
    *ok = 0;
    if (n == 0 || n > 6) return 0;
    for (k = 0; k < n; k++) {
        if (s[k] < '0' || s[k] > '9') return 0;
        v = v * 10 + (s[k] - '0');
    }
    *ok = 1;
    return v;
}

int dns_parse_resolv_conf(const char *text, size_t len, DnsResolvConf *out)
{
    const char *p, *end;

    if (!text || !out) return DNS_E_ARG;
    memset(out, 0, sizeof *out);
    p = text;
    end = text + len;

    while (p < end) {
        const char *line, *le, *lp;
        const char *tok;
        size_t tl;
        if (!dns_next_line(&p, end, &line, &le)) break;
        lp = line;
        if (!dns_token(&lp, le, &tok, &tl)) continue;
        if (dns_token_eq(tok, tl, "nameserver")) {
            DnsAddr a;
            if (!dns_token(&lp, le, &tok, &tl)) continue;
            if (out->nsrv >= DNS_MAX_SERVERS) continue;
            if (dns_token_addr(tok, tl, &a) != DNS_OK) continue;
            out->servers[out->nsrv++] = a;
        } else if (dns_token_eq(tok, tl, "options")) {
            while (dns_token(&lp, le, &tok, &tl)) {
                if (tl > 8 && dns_token_eq(tok, 8, "timeout:")) {
                    int ok = 0;
                    long v = dns_dec(tok + 8, tl - 8, &ok);
                    if (ok && v > 0 && v <= 3600) out->timeout_ms = (int)(v * 1000);
                } else if (tl > 9 && dns_token_eq(tok, 9, "attempts:")) {
                    int ok = 0;
                    long v = dns_dec(tok + 9, tl - 9, &ok);
                    if (ok && v > 0 && v <= 10) out->attempts = (int)v;
                }
            }
        }
    }
    return DNS_OK;
}

/* ============================================================
 *  Initialisation
 * ============================================================ */
void dns_default_config(DnsConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->nsrv = 0;
    cfg->servers = NULL;
    cfg->attempts = 1;
    cfg->timeout_ms = 1000;
    cfg->timeout_max_ms = 8000;
    cfg->udp_size = DNS_MAX_MSG;
    cfg->edns = 1;
}

int dns_resolver_init(DnsResolver *r, const DnsOps *ops, const DnsConfig *cfg)
{
    int i;
    if (!r || !ops || !cfg) return DNS_E_ARG;
    if (cfg->nsrv < 0 || cfg->nsrv > DNS_MAX_SERVERS) return DNS_E_ARG;
    if (cfg->nsrv > 0 && !cfg->servers) return DNS_E_ARG;
    if (cfg->attempts < 1) return DNS_E_ARG;
    if (cfg->timeout_ms <= 0) return DNS_E_ARG;
    if (cfg->edns && cfg->udp_size != 0 && cfg->udp_size < DNS_MIN_UDP)
        return DNS_E_ARG;

    memset(r, 0, sizeof *r);
    r->ops = ops;
    r->nsrv = cfg->nsrv;
    for (i = 0; i < cfg->nsrv; i++) r->srv[i] = cfg->servers[i];
    r->attempts = cfg->attempts;
    r->timeout_ms = cfg->timeout_ms > 3600000 ? 3600000 : cfg->timeout_ms;
    r->timeout_max_ms = cfg->timeout_max_ms > 0 ? cfg->timeout_max_ms : r->timeout_ms;
    if (r->timeout_max_ms < r->timeout_ms) r->timeout_max_ms = r->timeout_ms;
    r->udp_size = cfg->udp_size ? cfg->udp_size : DNS_MAX_MSG;
    r->edns = cfg->edns ? 1 : 0;
    if (r->udp_size < DNS_MIN_UDP) r->udp_size = DNS_MIN_UDP;

    r->now = ops->now_ms ? ops->now_ms(ops->ctx) : 0;
    /* The ID is a counter, not a random number: this resolver has no entropy
     * source it can trust in a freestanding build, and pretending otherwise
     * would be worse than saying it. The defence against off-path spoofing is
     * the ops' connect() (the kernel drops datagrams from the wrong source)
     * plus the ID check; DNSSEC is not implemented. Seeding the counter from
     * the clock at least stops two processes starting at the same value. */
    r->next_id = (uint16_t)(r->now ^ 0x5A5Au);

    if (ops->read_file) (void)dns_hosts_reload(r);
    return DNS_OK;
}

/* ============================================================
 *  10. POSIX DnsOps
 *
 *  The only code here that has a file descriptor. Everything is bounded by
 *  the caller's timeout, and every wait is a poll(), never a spin.
 * ============================================================ */
#if DNS_ENABLE_POSIX_OPS

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

static int64_t dns_ps_mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static uint64_t dns_ps_now_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)dns_ps_mono_ms();
}

static int dns_ps_fill(const DnsAddr *a, struct sockaddr_storage *ss, socklen_t *slen)
{
    memset(ss, 0, sizeof *ss);
    if (a->family == DNS_AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(a->port ? a->port : DNS_PORT);
        memcpy(&sin->sin_addr, a->bytes, 4);
        *slen = sizeof *sin;
        return 0;
    }
    if (a->family == DNS_AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(a->port ? a->port : DNS_PORT);
        memcpy(&sin6->sin6_addr, a->bytes, 16);
        *slen = sizeof *sin6;
        return 0;
    }
    return -1;
}

/* Wait for `events` on fd until the deadline. 1 = ready, 0 = timed out,
 * -1 = error. */
static int dns_ps_wait(int fd, short events, int64_t deadline)
{
    struct pollfd p;
    for (;;) {
        int64_t left = deadline - dns_ps_mono_ms();
        int pr;
        if (left <= 0) return 0;
        if (left > 3600000) left = 3600000;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        pr = poll(&p, 1, (int)left);
        if (pr == 0) return 0;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return 1;
    }
}

static int dns_ps_udp_xchg(void *ctx, const DnsAddr *srv, const uint8_t *q,
                           size_t qlen, uint8_t *resp, size_t rcap, int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t slen = 0;
    int fd, rc = DNS_E_NET;
    ssize_t n;

    (void)ctx;
    if (dns_ps_fill(srv, &ss, &slen) != 0) return DNS_E_ARG;
    fd = socket(ss.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) return DNS_E_NET;

    /* connect() on a UDP socket makes the kernel deliver only datagrams from
     * this peer, and turns an ICMP port-unreachable into a recv() error
     * instead of a silent timeout. */
    if (connect(fd, (struct sockaddr *)&ss, slen) != 0) { close(fd); return DNS_E_NET; }
    if (send(fd, q, qlen, 0) != (ssize_t)qlen) { close(fd); return DNS_E_NET; }
    {
        int w = dns_ps_wait(fd, POLLIN, dns_ps_mono_ms() + timeout_ms);
        if (w != 1) { close(fd); return w == 0 ? DNS_E_TIMEOUT : DNS_E_NET; }
    }
    n = recv(fd, resp, rcap, 0);
    if (n > 0) rc = (int)n;
    close(fd);
    return rc;
}

static int dns_ps_write_full(int fd, const uint8_t *buf, size_t len, int64_t deadline)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, buf + off, len - off, 0);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (dns_ps_wait(fd, POLLOUT, deadline) != 1) return -1;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int dns_ps_read_full(int fd, uint8_t *buf, size_t len, int64_t deadline)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n == 0) return -1;                       /* peer closed */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (dns_ps_wait(fd, POLLIN, deadline) != 1) return -1;
            continue;
        }
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* RFC 1035 s4.2.2: "a two byte length field ... the message". The prefix is
 * added here on the way out and stripped on the way in, so the resolver core
 * sees the same message bytes on both transports. */
static int dns_ps_tcp_xchg(void *ctx, const DnsAddr *srv, const uint8_t *q,
                           size_t qlen, uint8_t *resp, size_t rcap, int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t slen = 0;
    int fd, rc = DNS_E_NET, flags;
    int64_t deadline;
    uint8_t lenbuf[2];
    size_t want;

    (void)ctx;
    if (dns_ps_fill(srv, &ss, &slen) != 0) return DNS_E_ARG;
    if (qlen == 0 || qlen > 65535) return DNS_E_ARG;
    fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return DNS_E_NET;
    deadline = dns_ps_mono_ms() + timeout_ms;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { close(fd); return DNS_E_NET; }

    if (connect(fd, (struct sockaddr *)&ss, slen) != 0 && errno != EINPROGRESS) {
        close(fd);
        return DNS_E_NET;
    }
    {
        int w = dns_ps_wait(fd, POLLOUT, deadline);
        if (w != 1) { close(fd); return w == 0 ? DNS_E_TIMEOUT : DNS_E_NET; }
    }
    {
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
            close(fd);
            return DNS_E_NET;
        }
    }

    lenbuf[0] = (uint8_t)(qlen >> 8);
    lenbuf[1] = (uint8_t)(qlen & 0xFF);
    if (dns_ps_write_full(fd, lenbuf, 2, deadline) != 0) { close(fd); return DNS_E_NET; }
    if (dns_ps_write_full(fd, q, qlen, deadline) != 0) { close(fd); return DNS_E_NET; }

    if (dns_ps_read_full(fd, lenbuf, 2, deadline) != 0) { close(fd); return DNS_E_NET; }
    want = ((size_t)lenbuf[0] << 8) | (size_t)lenbuf[1];
    if (want == 0 || want > rcap) { close(fd); return DNS_E_OVERFLOW; }
    if (dns_ps_read_full(fd, resp, want, deadline) != 0) { close(fd); return DNS_E_NET; }
    rc = (int)want;
    close(fd);
    return rc;
}

static int dns_ps_read_file(void *ctx, const char *path, char *buf, size_t cap,
                            size_t *out_len)
{
    FILE *f;
    size_t n;
    int bad;
    (void)ctx;
    if (!path || !buf || cap == 0) return DNS_E_ARG;
    f = fopen(path, "rb");
    if (!f) return DNS_E_ARG;
    n = fread(buf, 1, cap - 1, f);
    bad = ferror(f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return bad ? DNS_E_ARG : DNS_OK;
}

static const DnsOps dns_posix_ops = {
    NULL,
    dns_ps_udp_xchg,
    dns_ps_tcp_xchg,
    dns_ps_now_ms,
    dns_ps_read_file
};

const DnsOps *dns_ops_posix(void) { return &dns_posix_ops; }
int dns_posix_available(void) { return 1; }

#else /* !DNS_ENABLE_POSIX_OPS */

const DnsOps *dns_ops_posix(void) { return NULL; }
int dns_posix_available(void) { return 0; }

#endif
