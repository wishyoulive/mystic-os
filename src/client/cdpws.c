/* ============================================================
 *  Mystic OS - client/cdpws.c
 *
 *  The DevTools transport that does not use a pipe: TCP to 127.0.0.1 and
 *  RFC 6455 WebSocket framing.  See cdpws.h for the API and the reasons.
 *
 *  WHAT IS IMPLEMENTED HERE
 *  ------------------------
 *  SHA-1 and base64 (only what the handshake needs), the RFC 6455 opening
 *  handshake with a caller-supplied key and a fail-closed accept check, the
 *  frame writer (client masking, all three length forms), the frame reader
 *  (continuation, ping/pong, close, 64-bit lengths, UTF-8 checks), message
 *  reassembly, the strict DevToolsActivePort reader, a JSON field scanner
 *  for /json/version, and an HTTP GET on the same transport.
 *
 *  Every byte that is not a syscall moves through CdpwsTransport, so the
 *  whole client runs against a fake socket.  The POSIX socket transport at
 *  the bottom is the only part that needs a kernel.
 *
 *  WHAT WAS RUN, AND HOW
 *  ---------------------
 *  [compile] zig cc -target x86_64-linux-musl -Os -static -Isrc -Wall
 *            -Wextra -c src/client/cdpws.c is silent (the report quotes
 *            the command and its empty output).
 *  [harness] .work/cdp1/harness.c drives this file over a scripted fake
 *            transport: the RFC 6455 example key, a response fragmented
 *            across three frames with a Ping in the middle, a wrong and a
 *            missing Sec-WebSocket-Accept, a Close answered with a Close,
 *            masked-client-frame verification, DevToolsActivePort
 *            accept/reject cases, /json/version extraction and the HTTP
 *            GET.  It also runs the same handshake and frames over a real
 *            loopback TCP socket.
 *  [not]     no browser was started (QEMU is not available to workers) and
 *            the POSIX connect() could not execute on the Windows host that
 *            runs the harness; it is compiled, not observed.
 * ============================================================ */
#include "cdpws.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CDPWS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define CDPWS_IN_CAP 16384u

/* ============================================================
 *  1. Result codes as sentences
 * ============================================================ */
const char *cdpws_strerror(int rc) {
    switch (rc) {
    case CDPWS_OK:             return "ok";
    case CDPWS_ERR_IO:         return "transport read, write or connect failed";
    case CDPWS_ERR_TIMEOUT:    return "the connect or I/O deadline expired";
    case CDPWS_ERR_CLOSED:     return "the connection is closed";
    case CDPWS_ERR_PROTOCOL:   return "the peer violated RFC 6455 or HTTP";
    case CDPWS_ERR_HANDSHAKE:  return "the upgrade response did not satisfy RFC 6455 section 4.1";
    case CDPWS_ERR_TOO_BIG:    return "a message or header exceeded its limit";
    case CDPWS_ERR_ARG:        return "invalid argument";
    case CDPWS_ERR_NOMEM:      return "out of memory";
    case CDPWS_ERR_HTTP:       return "the HTTP response was not usable";
    case CDPWS_ERR_NOTFOUND:   return "the JSON field is absent";
    case CDPWS_ERR_UTF8:       return "a text payload is not valid UTF-8";
    case CDPWS_ERR_RANDOM:     return "no masking key could be produced";
    case CDPWS_ERR_NOSPACE:    return "the caller's buffer is too small";
    default:                   return "unknown CDPWS error";
    }
}

static int err_write(char *err, size_t esz, const char *fmt, ...) {
    va_list ap;
    if (!err || esz == 0) return 0;
    va_start(ap, fmt);
    vsnprintf(err, esz, fmt, ap);
    va_end(ap);
    return 0;
}

/* ============================================================
 *  2. SHA-1 (FIPS 180-1).  Only used on the 60-byte handshake text, so
 *     this is the plain one-block-at-a-time form.
 * ============================================================ */
typedef struct {
    uint32_t      h[5];
    uint64_t      bits;
    unsigned char buf[64];
    size_t        used;
} Sha1;

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(Sha1 *s, const unsigned char *p) {
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->bits = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    s->bits += (uint64_t)n * 8u;
    while (n > 0) {
        size_t room = 64 - s->used;
        size_t take = n < room ? n : room;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_final(Sha1 *s, unsigned char out[20]) {
    uint64_t bits = s->bits;
    unsigned char pad[72];
    size_t padlen;
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    padlen = (s->used < 56) ? (56 - s->used) : (120 - s->used);
    sha1_update(s, pad, padlen);
    for (i = 0; i < 8; i++) pad[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(s, pad, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

/* ============================================================
 *  3. base64 (RFC 4648, with padding).  Only the encoder is needed: the
 *     client makes Sec-WebSocket-Key and Sec-WebSocket-Accept, never
 *     decodes either.
 * ============================================================ */
static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const unsigned char *in, size_t n, char *out, size_t cap) {
    size_t need;
    size_t i, o = 0;
    if (!out) return CDPWS_ERR_ARG;
    need = 4 * ((n + 2) / 3);
    if (cap < need + 1) return CDPWS_ERR_NOSPACE;
    for (i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int two = (i + 1 < n);
        int three = (i + 2 < n);
        if (two)   v |= (uint32_t)in[i + 1] << 8;
        if (three) v |= (uint32_t)in[i + 2];
        out[o++] = b64_tab[(v >> 18) & 63];
        out[o++] = b64_tab[(v >> 12) & 63];
        out[o++] = two   ? b64_tab[(v >> 6) & 63] : '=';
        out[o++] = three ? b64_tab[v & 63]        : '=';
    }
    out[o] = '\0';
    return CDPWS_OK;
}

int cdpws_key_to_b64(const unsigned char key[CDPWS_KEY_LEN],
                     char out[CDPWS_KEY_B64_LEN + 1]) {
    if (!key || !out) return CDPWS_ERR_ARG;
    return b64_encode(key, CDPWS_KEY_LEN, out, CDPWS_KEY_B64_LEN + 1);
}

int cdpws_accept_key(const char *key_b64, char out[CDPWS_ACCEPT_BUF]) {
    char text[CDPWS_KEY_B64_LEN + sizeof(CDPWS_GUID)];
    unsigned char digest[20];
    Sha1 s;
    size_t klen;
    if (!key_b64 || !out) return CDPWS_ERR_ARG;
    klen = strlen(key_b64);
    if (klen != CDPWS_KEY_B64_LEN) return CDPWS_ERR_ARG;
    memcpy(text, key_b64, klen);
    memcpy(text + klen, CDPWS_GUID, sizeof(CDPWS_GUID));  /* includes NUL */
    sha1_init(&s);
    sha1_update(&s, text, klen + sizeof(CDPWS_GUID) - 1);
    sha1_final(&s, digest);
    return b64_encode(digest, sizeof digest, out, CDPWS_ACCEPT_BUF);
}

/* ============================================================
 *  4. UTF-8 (RFC 3629).  A text frame that is not valid UTF-8 is a
 *     protocol error in RFC 6455 §5.6 and must fail the connection.
 * ============================================================ */
static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
            continue;
        }
        if (c >= 0xE0 && c <= 0xEF) {
            unsigned char lo = 0x80, hi = 0xBF;
            if (c == 0xE0) lo = 0xA0;      /* no overlong */
            if (c == 0xED) hi = 0x9F;      /* no surrogates */
            if (i + 2 >= n) return 0;
            if (s[i + 1] < lo || s[i + 1] > hi) return 0;
            if ((s[i + 2] & 0xC0) != 0x80) return 0;
            i += 3;
            continue;
        }
        if (c >= 0xF0 && c <= 0xF4) {
            unsigned char lo = 0x90, hi = 0xBF;
            if (c == 0xF0) lo = 0x90;      /* no overlong */
            if (c == 0xF4) hi = 0x8F;      /* <= U+10FFFF */
            if (i + 3 >= n) return 0;
            if (s[i + 1] < lo || s[i + 1] > hi) return 0;
            if ((s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) return 0;
            i += 4;
            continue;
        }
        return 0;
    }
    return 1;
}

/* ============================================================
 *  5. Random
 * ============================================================ */
#ifndef _WIN32
int cdpws_random_bytes(void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t off = 0;
    int fd;
    if (!buf && len) return -1;
#ifdef O_CLOEXEC
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
#endif
        fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t r = read(fd, p + off, len - off);
        if (r > 0) { off += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
#endif

/* ============================================================
 *  6. DevToolsActivePort
 * ============================================================ */
int cdpws_parse_active_port(const char *text, size_t len,
                            int *port, char *path, size_t path_cap,
                            char *err, size_t esz) {
    size_t nl, end, pstart, pend;
    size_t i;
    unsigned value = 0;
    if (err && esz) err[0] = '\0';
    if (!text || !port || !path || path_cap == 0) {
        err_write(err, esz, "invalid argument");
        return CDPWS_ERR_ARG;
    }

    /* line 1: the port */
    nl = 0;
    while (nl < len && text[nl] != '\n') nl++;
    if (nl == len) {
        err_write(err, esz, "the file has no second line (path)");
        return CDPWS_ERR_ARG;
    }
    end = nl;
    while (end > 0 && text[end - 1] == '\r') end--;
    if (end == 0) {
        err_write(err, esz, "the first line (port) is empty");
        return CDPWS_ERR_ARG;
    }
    if (end > 5) {
        err_write(err, esz, "the port is longer than five digits");
        return CDPWS_ERR_ARG;
    }
    for (i = 0; i < end; i++) {
        if (text[i] < '0' || text[i] > '9') {
            err_write(err, esz, "the port is not decimal (byte 0x%02x)",
                      (unsigned)(unsigned char)text[i]);
            return CDPWS_ERR_ARG;
        }
        value = value * 10u + (unsigned)(text[i] - '0');
    }
    if (value < 1 || value > 65535u) {
        err_write(err, esz, "the port %u is outside 1..65535", value);
        return CDPWS_ERR_ARG;
    }

    /* line 2: the WebSocket path */
    pstart = nl + 1;
    pend = pstart;
    while (pend < len && text[pend] != '\n') pend++;
    while (pend > pstart && text[pend - 1] == '\r') pend--;
    if (pend == pstart) {
        err_write(err, esz, "the second line (WebSocket path) is empty");
        return CDPWS_ERR_ARG;
    }
    if (text[pstart] != '/') {
        err_write(err, esz, "the WebSocket path does not start with '/'");
        return CDPWS_ERR_ARG;
    }
    for (i = pstart; i < pend; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c <= 0x20 || c >= 0x7F) {
            err_write(err, esz, "the WebSocket path contains a space or control byte");
            return CDPWS_ERR_ARG;
        }
    }
    if (pend - pstart + 1 > path_cap) {
        err_write(err, esz, "the WebSocket path needs %u bytes, the buffer has %u",
                  (unsigned)(pend - pstart + 1), (unsigned)path_cap);
        return CDPWS_ERR_NOSPACE;
    }
    /* nothing but blank lines may follow */
    for (i = pend; i < len; i++) {
        if (text[i] != '\n' && text[i] != '\r') {
            err_write(err, esz, "the file has a third, unexpected line");
            return CDPWS_ERR_ARG;
        }
    }
    memcpy(path, text + pstart, pend - pstart);
    path[pend - pstart] = '\0';
    *port = (int)value;
    return CDPWS_OK;
}

int cdpws_read_active_port(const char *file, int *port,
                           char *path, size_t path_cap,
                           char *err, size_t esz) {
    FILE *f;
    char buf[4096];
    size_t n;
    int rc;
    if (err && esz) err[0] = '\0';
    if (!file || !port || !path || path_cap == 0) {
        err_write(err, esz, "invalid argument");
        return CDPWS_ERR_ARG;
    }
    f = fopen(file, "rb");
    if (!f) {
        err_write(err, esz, "cannot open %s: %s", file, strerror(errno));
        return CDPWS_ERR_IO;
    }
    n = fread(buf, 1, sizeof buf, f);
    if (ferror(f)) {
        err_write(err, esz, "cannot read %s: %s", file, strerror(errno));
        fclose(f);
        return CDPWS_ERR_IO;
    }
    if (n == sizeof buf && fgetc(f) != EOF) {
        err_write(err, esz, "%s is larger than %u bytes", file,
                  (unsigned)sizeof buf);
        fclose(f);
        return CDPWS_ERR_TOO_BIG;
    }
    fclose(f);
    rc = cdpws_parse_active_port(buf, n, port, path, path_cap, err, esz);
    return rc;
}

/* ============================================================
 *  7. JSON string scanner
 *
 *     Not a tree parser: it walks the document, consumes whole strings
 *     (so a key name inside another string's value cannot match), checks
 *     that a candidate key is followed by ':', and decodes the one string
 *     value asked for.
 * ============================================================ */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int json_put(char *out, size_t cap, size_t *o, unsigned cp) {
    if (*o + 1 >= cap) return CDPWS_ERR_NOSPACE;
    if (cp < 0x80) {
        out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o + 2 >= cap) return CDPWS_ERR_NOSPACE;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*o + 3 >= cap) return CDPWS_ERR_NOSPACE;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*o + 4 >= cap) return CDPWS_ERR_NOSPACE;
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
    out[*o] = '\0';
    return CDPWS_OK;
}

/* Parse one JSON string starting at *i (which must be '"').  On success
 * *i is just past the closing quote and, when out is not NULL, the value
 * is decoded into out (including \" and \uXXXX). */
static int json_scan_string(const char *s, size_t n, size_t *i,
                            char *out, size_t cap) {
    size_t k = *i, o = 0;
    if (k >= n || s[k] != '"') return CDPWS_ERR_PROTOCOL;
    k++;
    if (out && cap) out[0] = '\0';
    while (k < n) {
        unsigned char c = (unsigned char)s[k];
        if (c == '"') {
            *i = k + 1;
            return CDPWS_OK;
        }
        if (c < 0x20) return CDPWS_ERR_PROTOCOL;
        if (c != '\\') {
            if (out) {
                if (o + 1 >= cap) return CDPWS_ERR_NOSPACE;
                out[o++] = (char)c;
                out[o] = '\0';
            }
            k++;
            continue;
        }
        if (k + 1 >= n) return CDPWS_ERR_PROTOCOL;
        {
            unsigned char e = (unsigned char)s[k + 1];
            unsigned cp;
            int rc;
            k += 2;
            switch (e) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                int h0, h1, h2, h3;
                if (k + 4 > n) return CDPWS_ERR_PROTOCOL;
                h0 = hexval((unsigned char)s[k]);
                h1 = hexval((unsigned char)s[k + 1]);
                h2 = hexval((unsigned char)s[k + 2]);
                h3 = hexval((unsigned char)s[k + 3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    return CDPWS_ERR_PROTOCOL;
                cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                k += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    int g0, g1, g2, g3;
                    /* a high surrogate must be followed by \uDC00..\uDFFF */
                    if (k + 6 > n || s[k] != '\\' || s[k + 1] != 'u') {
                        cp = 0xFFFD;
                        break;
                    }
                    g0 = hexval((unsigned char)s[k + 2]);
                    g1 = hexval((unsigned char)s[k + 3]);
                    g2 = hexval((unsigned char)s[k + 4]);
                    g3 = hexval((unsigned char)s[k + 5]);
                    if (g0 < 0 || g1 < 0 || g2 < 0 || g3 < 0)
                        return CDPWS_ERR_PROTOCOL;
                    {
                        unsigned lo = (unsigned)((g0 << 12) | (g1 << 8) |
                                                 (g2 << 4) | g3);
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            cp = 0xFFFD;
                            break;
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        k += 6;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;       /* lone low surrogate */
                }
                break;
            }
            default:
                return CDPWS_ERR_PROTOCOL;
            }
            if (out) {
                rc = json_put(out, cap, &o, cp);
                if (rc != CDPWS_OK) return rc;
            }
        }
    }
    return CDPWS_ERR_PROTOCOL;         /* unterminated string */
}

static void json_skip_ws(const char *s, size_t n, size_t *i) {
    while (*i < n) {
        char c = s[*i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') (*i)++;
        else break;
    }
}

int cdpws_json_string(const char *json, size_t len, const char *field,
                      char *out, size_t cap) {
    size_t i = 0;
    size_t flen;
    if (!json || !field || !out || cap == 0) return CDPWS_ERR_ARG;
    flen = strlen(field);
    out[0] = '\0';
    while (i < len) {
        if (json[i] != '"') { i++; continue; }
        {
            size_t key_start = i;
            char key[128];
            size_t k = i;
            int rc = json_scan_string(json, len, &k, key, sizeof key);
            if (rc == CDPWS_ERR_NOSPACE) {
                /* a key longer than any field we look for: skip it */
                size_t skip = key_start;
                if (json_scan_string(json, len, &skip, NULL, 0) != CDPWS_OK)
                    return CDPWS_ERR_PROTOCOL;
                i = skip;
                continue;
            }
            if (rc != CDPWS_OK) return CDPWS_ERR_PROTOCOL;
            {
                size_t after = k;
                json_skip_ws(json, len, &after);
                if (after < len && json[after] == ':') {
                    after++;
                    json_skip_ws(json, len, &after);
                    if (strlen(key) == flen && memcmp(key, field, flen) == 0) {
                        if (after >= len || json[after] != '"')
                            return CDPWS_ERR_NOTFOUND;
                        return json_scan_string(json, len, &after, out, cap);
                    }
                    i = after;
                    continue;
                }
            }
            i = k;
        }
    }
    return CDPWS_ERR_NOTFOUND;
}

int cdpws_ws_url_path(const char *url, char *path, size_t cap) {
    size_t n, i, start, k;
    if (!url || !path || cap == 0) return CDPWS_ERR_ARG;
    n = strlen(url);
    if (n < 5) return CDPWS_ERR_ARG;
    if (!((url[0] == 'w' || url[0] == 'W') &&
          (url[1] == 's' || url[1] == 'S') && url[2] == ':' &&
          url[3] == '/' && url[4] == '/'))
        return CDPWS_ERR_ARG;              /* wss:// is not offered locally */
    start = 5;
    i = start;
    while (i < n && url[i] != '/' && url[i] != '?' && url[i] != '#') i++;
    if (i == start) return CDPWS_ERR_ARG;  /* empty authority */
    if (i == n || url[i] != '/') return CDPWS_ERR_ARG;  /* no path */
    for (k = i; k < n; k++) {
        unsigned char c = (unsigned char)url[k];
        if (c <= 0x20 || c >= 0x7F || c == '#') return CDPWS_ERR_ARG;
    }
    if (n - i + 1 > cap) return CDPWS_ERR_NOSPACE;
    memcpy(path, url + i, n - i);
    path[n - i] = '\0';
    for (k = 0; k + i < n; k++)
        if (path[k] == '?') { path[k] = '\0'; break; }
    return CDPWS_OK;
}

/* ============================================================
 *  8. HTTP/1.x helpers
 * ============================================================ */
static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_equal(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return 0;
    return 1;
}

static int http_status(const char *head, size_t n, int *status) {
    int v;
    if (n < 12 || memcmp(head, "HTTP/1.", 7) != 0) return CDPWS_ERR_PROTOCOL;
    if (head[7] != '0' && head[7] != '1') return CDPWS_ERR_PROTOCOL;
    if (head[8] != ' ') return CDPWS_ERR_PROTOCOL;
    if (head[9] < '0' || head[9] > '9' || head[10] < '0' || head[10] > '9' ||
        head[11] < '0' || head[11] > '9')
        return CDPWS_ERR_PROTOCOL;
    v = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');
    *status = v;
    return CDPWS_OK;
}

/* 1 = found, 0 = absent, <0 = malformed header block.  Values are trimmed. */
static int http_header(const char *head, size_t n, const char *name,
                       const char **val, size_t *vlen) {
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < n && head[i] != '\n') i++;
    if (i < n) i++;
    while (i < n) {
        size_t eol = i;
        size_t end, j, name_end, vstart, vend;
        while (eol < n && head[eol] != '\n') eol++;
        end = eol;
        if (end > i && head[end - 1] == '\r') end--;
        if (end == i) return 0;            /* blank line: headers are done */
        j = i;
        while (j < end && head[j] != ':') j++;
        if (j >= end) return CDPWS_ERR_PROTOCOL;
        name_end = j;
        while (name_end > i && (head[name_end - 1] == ' ' ||
                                head[name_end - 1] == '\t')) name_end--;
        if (name_end - i == nl && ci_equal(head + i, name, nl)) {
            vstart = j + 1;
            while (vstart < end && (head[vstart] == ' ' || head[vstart] == '\t'))
                vstart++;
            vend = end;
            while (vend > vstart && (head[vend - 1] == ' ' ||
                                     head[vend - 1] == '\t')) vend--;
            *val = head + vstart;
            *vlen = vend - vstart;
            return 1;
        }
        i = eol + 1;
    }
    return 0;
}

/* Does a comma-separated header value list contain `token`? */
static int http_token(const char *val, size_t vlen, const char *token) {
    size_t tl = strlen(token);
    size_t i = 0;
    while (i < vlen) {
        size_t start, end;
        while (i < vlen && (val[i] == ',' || val[i] == ' ' || val[i] == '\t'))
            i++;
        start = i;
        while (i < vlen && val[i] != ',') i++;
        end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;
        if (end - start == tl && ci_equal(val + start, token, tl)) return 1;
    }
    return 0;
}

/* Find the end of an HTTP head in buf; 0 if not complete yet. */
static size_t http_head_end(const char *buf, size_t n) {
    size_t i;
    for (i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

/* Strict decimal Content-Length.  Returns 0 on success. */
static int http_content_length(const char *v, size_t n, size_t *out) {
    size_t i, val = 0;
    if (n == 0 || n > 18) return CDPWS_ERR_HTTP;
    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return CDPWS_ERR_HTTP;
        val = val * 10 + (size_t)(v[i] - '0');
        if (val > ((size_t)1 << 40)) return CDPWS_ERR_TOO_BIG;
    }
    *out = val;
    return CDPWS_OK;
}

static int header_safe(const char *s) {
    size_t i;
    if (!s) return 0;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7F) return 0;
    }
    return 1;
}

/* ============================================================
 *  9. The connection object
 * ============================================================ */
enum { CDPWS_ST_CLOSED = 0, CDPWS_ST_OPEN = 1, CDPWS_ST_CLOSING = 2 };

struct Cdpws {
    CdpwsTransport tr;
    int            state;
    int            close_sent;
    int            close_recv;
    int            failed;
    unsigned       pings;
    unsigned char *in;
    size_t         in_cap;
    size_t         in_len;
    unsigned char *msg;          /* assembly and delivery buffer */
    size_t         msg_cap;
    size_t         msg_len;
    int            frag_opcode;  /* 0 when no message is being assembled */
    size_t         max_message;
    CdpwsRandFn    rand;
    void          *rand_ctx;
    int            err_code;
    char           err[CDPWS_ERR_MAX];
};

const char *cdpws_last_error(const Cdpws *c) {
    return c ? c->err : "no connection";
}

int cdpws_last_error_code(const Cdpws *c) {
    return c ? c->err_code : CDPWS_ERR_ARG;
}

static int failf(Cdpws *c, int rc, const char *fmt, ...) {
    va_list ap;
    c->err_code = rc;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    return rc;
}

static int buf_reserve(unsigned char **p, size_t *cap, size_t need) {
    size_t ncap;
    unsigned char *np;
    if (need <= *cap) return CDPWS_OK;
    ncap = *cap ? *cap : 256;
    while (ncap < need) {
        if (ncap > ((size_t)-1) / 2) { ncap = need; break; }
        ncap *= 2;
    }
    np = (unsigned char *)realloc(*p, ncap);
    if (!np) return CDPWS_ERR_NOMEM;
    *p = np;
    *cap = ncap;
    return CDPWS_OK;
}

static int write_all(Cdpws *c, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;
    while (off < len) {
        long w = c->tr.write(c->tr.ctx, p + off, len - off);
        if (w > 0) {
            if ((size_t)w > len - off)
                return failf(c, CDPWS_ERR_IO,
                    "the transport claimed to write more bytes than it was given");
            off += (size_t)w;
            continue;
        }
        c->failed = 1;
        if (w == 0) return failf(c, CDPWS_ERR_IO, "the transport wrote 0 bytes");
        if ((int)w == CDPWS_ERR_TIMEOUT)
            return failf(c, CDPWS_ERR_TIMEOUT, "the write deadline expired");
        return failf(c, CDPWS_ERR_IO, "the transport write failed (%ld)", w);
    }
    return CDPWS_OK;
}

static int random_fill(Cdpws *c, void *buf, size_t len) {
    if (c->rand) {
        if (c->rand(c->rand_ctx, buf, len) != 0)
            return failf(c, CDPWS_ERR_RANDOM, "the random source failed");
        return CDPWS_OK;
    }
#ifndef _WIN32
    if (cdpws_random_bytes(buf, len) != 0)
        return failf(c, CDPWS_ERR_RANDOM, "cannot read /dev/urandom");
    return CDPWS_OK;
#else
    (void)buf; (void)len;
    return failf(c, CDPWS_ERR_RANDOM, "no random source was supplied");
#endif
}

/* ============================================================
 *  10. Frame writer
 * ============================================================ */
static int frame_send(Cdpws *c, int opcode, const void *data, size_t len) {
    unsigned char hdr[14];
    unsigned char key[4];
    unsigned char tmp[4096];
    const unsigned char *p = (const unsigned char *)data;
    size_t hl = 0, off = 0;
    int rc;

    if (len && !data) return failf(c, CDPWS_ERR_ARG, "null payload");
    hdr[hl++] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[hl++] = (unsigned char)(0x80 | (unsigned char)len);
    } else if (len <= 65535) {
        hdr[hl++] = 0x80 | 126;
        hdr[hl++] = (unsigned char)(len >> 8);
        hdr[hl++] = (unsigned char)len;
    } else {
        int i;
        hdr[hl++] = 0x80 | 127;
        for (i = 7; i >= 0; i--)
            hdr[hl++] = (unsigned char)((uint64_t)len >> (i * 8));
    }
    rc = random_fill(c, key, sizeof key);
    if (rc != CDPWS_OK) return rc;
    memcpy(hdr + hl, key, sizeof key);
    hl += sizeof key;
    rc = write_all(c, hdr, hl);
    if (rc != CDPWS_OK) return rc;
    while (off < len) {
        size_t n = len - off;
        size_t i;
        if (n > sizeof tmp) n = sizeof tmp;
        for (i = 0; i < n; i++)
            tmp[i] = (unsigned char)(p[off + i] ^ key[(off + i) & 3]);
        rc = write_all(c, tmp, n);
        if (rc != CDPWS_OK) return rc;
        off += n;
    }
    return CDPWS_OK;
}

static int close_code_ok(int code) {
    if (code >= 1000 && code <= 1003) return 1;
    if (code >= 1007 && code <= 1011) return 1;
    if (code >= 3000 && code <= 4999) return 1;
    return 0;
}

static int send_close_code(Cdpws *c, int code) {
    unsigned char p[2];
    size_t n = 0;
    int rc;
    if (c->close_sent) return CDPWS_OK;
    if (code) {
        p[0] = (unsigned char)(code >> 8);
        p[1] = (unsigned char)(code & 0xFF);
        n = 2;
    }
    rc = frame_send(c, CDPWS_OP_CLOSE, p, n);
    c->close_sent = 1;
    if (c->state == CDPWS_ST_OPEN) c->state = CDPWS_ST_CLOSING;
    return rc;
}

/* Record a failure; when the RFC requires the connection to be failed, send
 * the matching Close first (1002 protocol, 1007 UTF-8, 1009 too big). */
static int close_fail(Cdpws *c, int rc, const char *fmt, ...) {
    va_list ap;
    int code = 0;
    c->err_code = rc;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    if (rc == CDPWS_ERR_PROTOCOL) code = 1002;
    else if (rc == CDPWS_ERR_UTF8) code = 1007;
    else if (rc == CDPWS_ERR_TOO_BIG) code = 1009;
    if (code && c->state == CDPWS_ST_OPEN) (void)send_close_code(c, code);
    return rc;
}

/* ============================================================
 *  11. Frame reader and reassembly
 * ============================================================ */
typedef struct {
    int                  opcode;
    int                  fin;
    size_t               header_len;
    const unsigned char *payload;
    size_t               len;
} Frame;

/* 1 = a whole frame is buffered, 0 = need more bytes, <0 = the frame is
 * illegal and the connection has been failed. */
static int frame_parse(Cdpws *c, Frame *f) {
    const unsigned char *p = c->in;
    size_t n = c->in_len;
    size_t len, off;
    int b0, b1, op, fin;

    if (n < 2) return 0;
    b0 = p[0];
    b1 = p[1];
    if (b0 & 0x70) return close_fail(c, CDPWS_ERR_PROTOCOL,
        "a reserved bit is set (no extension was negotiated)");
    op = b0 & 0x0F;
    if (!(op == CDPWS_OP_CONT || op == CDPWS_OP_TEXT || op == CDPWS_OP_BINARY ||
          op == CDPWS_OP_CLOSE || op == CDPWS_OP_PING || op == CDPWS_OP_PONG))
        return close_fail(c, CDPWS_ERR_PROTOCOL, "opcode 0x%x is not defined", op);
    if (b1 & 0x80) return close_fail(c, CDPWS_ERR_PROTOCOL,
        "a server frame is masked (RFC 6455 section 5.1 forbids it)");
    fin = (b0 & 0x80) != 0;
    len = (size_t)(b1 & 0x7F);
    off = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = ((size_t)p[2] << 8) | (size_t)p[3];
        off = 4;
        if (len < 126) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a 16-bit length was used for a payload that fits in 7 bits");
    } else if (len == 127) {
        uint64_t v = 0;
        int i;
        if (n < 10) return 0;
        if (p[2] & 0x80) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a 64-bit length has its most significant bit set");
        for (i = 0; i < 8; i++) v = (v << 8) | (uint64_t)p[2 + i];
        if (v > (uint64_t)SIZE_MAX) return close_fail(c, CDPWS_ERR_TOO_BIG,
            "the payload length does not fit in size_t");
        len = (size_t)v;
        off = 10;
        if (len <= 65535) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a 64-bit length was used for a payload that fits in 16 bits");
    }
    if (op >= 8) {
        if (!fin) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a control frame is fragmented");
        if (len > 125) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a control frame payload is %u bytes (the limit is 125)",
            (unsigned)len);
    }
    if (op < 8 && len > c->max_message) return close_fail(c, CDPWS_ERR_TOO_BIG,
        "a frame of %u bytes exceeds max_message (%u)",
        (unsigned)len, (unsigned)c->max_message);
    if (n < off) return 0;
    if (n - off < len) return 0;
    f->opcode = op;
    f->fin = fin;
    f->header_len = off;
    f->payload = p + off;
    f->len = len;
    return 1;
}

static int msg_append(Cdpws *c, const unsigned char *p, size_t n) {
    int rc;
    if (n > c->max_message - c->msg_len)
        return close_fail(c, CDPWS_ERR_TOO_BIG,
            "the reassembled message exceeds max_message (%u)",
            (unsigned)c->max_message);
    rc = buf_reserve(&c->msg, &c->msg_cap, c->msg_len + n + 1);
    if (rc != CDPWS_OK) return failf(c, rc, "out of memory for a message");
    if (n) memcpy(c->msg + c->msg_len, p, n);
    c->msg_len += n;
    c->msg[c->msg_len] = '\0';
    return CDPWS_OK;
}

/* 1 = a message is in *m, 0 = the frame was handled and produced none. */
static int handle_frame(Cdpws *c, const Frame *f, CdpwsMessage *m) {
    int rc;
    if (f->opcode == CDPWS_OP_PING) {
        /* RFC 6455 §5.5.3: answer with a Pong and the same payload.  A Ping
         * may arrive between fragments; it never disturbs the assembly. */
        if (c->pings != (unsigned)-1) c->pings++;
        return frame_send(c, CDPWS_OP_PONG, f->payload, f->len);
    }
    if (f->opcode == CDPWS_OP_PONG) return 0;
    if (f->opcode == CDPWS_OP_CLOSE) {
        int code = 0;
        if (f->len == 1) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a Close frame has a 1-byte payload");
        if (f->len >= 2) {
            code = ((int)f->payload[0] << 8) | (int)f->payload[1];
            if (!close_code_ok(code)) return close_fail(c, CDPWS_ERR_PROTOCOL,
                "Close status %d may not appear on the wire", code);
            if (!utf8_valid(f->payload + 2, f->len - 2))
                return close_fail(c, CDPWS_ERR_UTF8,
                    "a Close reason is not valid UTF-8");
        }
        if (!c->close_sent) {
            rc = send_close_code(c, code);
            if (rc != CDPWS_OK) return rc;
        }
        c->close_recv = 1;
        c->state = CDPWS_ST_CLOSED;
        rc = buf_reserve(&c->msg, &c->msg_cap, f->len + 1);
        if (rc != CDPWS_OK) return failf(c, rc, "out of memory for a message");
        if (f->len) memcpy(c->msg, f->payload, f->len);
        c->msg_len = f->len;
        c->msg[c->msg_len] = '\0';
        m->opcode = CDPWS_OP_CLOSE;
        m->len = (int)f->len;
        m->code = code;
        m->data = c->msg;
        return 1;
    }
    if (f->opcode == CDPWS_OP_CONT) {
        int op;
        if (!c->frag_opcode) return close_fail(c, CDPWS_ERR_PROTOCOL,
            "a continuation frame arrived with no message in progress");
        op = c->frag_opcode;
        rc = msg_append(c, f->payload, f->len);
        if (rc != CDPWS_OK) return rc;
        if (!f->fin) return 0;
        if (op == CDPWS_OP_TEXT && !utf8_valid(c->msg, c->msg_len))
            return close_fail(c, CDPWS_ERR_UTF8,
                "a fragmented text message is not valid UTF-8");
        c->frag_opcode = 0;
        if (c->msg_len > (size_t)0x7FFFFFFF)
            return close_fail(c, CDPWS_ERR_TOO_BIG, "the message does not fit in int");
        m->opcode = op;
        m->len = (int)c->msg_len;
        m->code = 0;
        m->data = c->msg;
        return 1;
    }
    /* TEXT or BINARY */
    if (c->frag_opcode) return close_fail(c, CDPWS_ERR_PROTOCOL,
        "a new data frame started inside a fragmented message");
    if (!f->fin) {
        c->frag_opcode = f->opcode;
        c->msg_len = 0;
        return msg_append(c, f->payload, f->len);
    }
    if (f->opcode == CDPWS_OP_TEXT && !utf8_valid(f->payload, f->len))
        return close_fail(c, CDPWS_ERR_UTF8, "a text frame is not valid UTF-8");
    c->msg_len = 0;
    rc = msg_append(c, f->payload, f->len);
    if (rc != CDPWS_OK) return rc;
    if (c->msg_len > (size_t)0x7FFFFFFF)
        return close_fail(c, CDPWS_ERR_TOO_BIG, "the message does not fit in int");
    m->opcode = f->opcode;
    m->len = (int)c->msg_len;
    m->code = 0;
    m->data = c->msg;
    return 1;
}

int cdpws_recv(Cdpws *c, CdpwsMessage *m) {
    if (!c || !m) return CDPWS_ERR_ARG;
    if (c->close_recv) return CDPWS_ERR_CLOSED;
    if (c->failed) return c->err_code ? c->err_code : CDPWS_ERR_IO;
    for (;;) {
        Frame f;
        int r = frame_parse(c, &f);
        if (r < 0) { c->failed = 1; return r; }
        if (r == 1) {
            int rc;
            size_t used = f.header_len + f.len;
            rc = handle_frame(c, &f, m);
            memmove(c->in, c->in + used, c->in_len - used);
            c->in_len -= used;
            if (rc < 0) { c->failed = 1; return rc; }
            if (rc == 1) return 1;
            continue;
        }
        if (c->in_len == c->in_cap) {
            size_t want = c->in_cap ? c->in_cap * 2 : CDPWS_IN_CAP;
            if (want > c->max_message + 14) want = c->max_message + 14;
            if (want <= c->in_cap)
                return close_fail(c, CDPWS_ERR_TOO_BIG,
                    "the incoming buffer cannot grow past %u bytes",
                    (unsigned)c->in_cap);
            if (buf_reserve(&c->in, &c->in_cap, want) != CDPWS_OK)
                return failf(c, CDPWS_ERR_NOMEM, "out of memory for input");
        }
        {
            long got = c->tr.read(c->tr.ctx, c->in + c->in_len,
                                  c->in_cap - c->in_len);
            if (got == CDPWS_IO_AGAIN) return 0;
            if (got == 0) {
                c->failed = 1;
                return failf(c, CDPWS_ERR_CLOSED,
                    "the peer closed the TCP connection without a Close frame");
            }
            if (got < 0) {
                /* A read deadline is not a protocol failure: a caller with an
                 * event loop sets a short I/O timeout and treats 0 exactly
                 * like "nothing complete yet".  The connection stays usable. */
                if ((int)got == CDPWS_ERR_TIMEOUT) return 0;
                c->failed = 1;
                return failf(c, CDPWS_ERR_IO, "the transport read failed (%ld)", got);
            }
            if ((size_t)got > c->in_cap - c->in_len)
                return failf(c, CDPWS_ERR_IO,
                    "the transport claimed to read more bytes than its buffer");
            c->in_len += (size_t)got;
        }
    }
}

/* ============================================================
 *  12. Sending
 * ============================================================ */
static int sendable(Cdpws *c) {
    if (!c) return CDPWS_ERR_ARG;
    if (c->failed) return c->err_code ? c->err_code : CDPWS_ERR_IO;
    if (c->state != CDPWS_ST_OPEN || c->close_sent)
        return failf(c, CDPWS_ERR_CLOSED, "the connection is not open");
    return CDPWS_OK;
}

int cdpws_send_text(Cdpws *c, const void *data, size_t len) {
    int rc = sendable(c);
    if (rc != CDPWS_OK) return rc;
    if (len && !data) return failf(c, CDPWS_ERR_ARG, "null text payload");
    if (!utf8_valid((const unsigned char *)data, len))
        return failf(c, CDPWS_ERR_UTF8, "the text to send is not valid UTF-8");
    rc = frame_send(c, CDPWS_OP_TEXT, data, len);
    if (rc != CDPWS_OK) c->failed = 1;
    return rc;
}

int cdpws_send_json(Cdpws *c, const char *json) {
    if (!json) return CDPWS_ERR_ARG;
    return cdpws_send_text(c, json, strlen(json));
}

int cdpws_send_ping(Cdpws *c, const void *data, size_t len) {
    int rc = sendable(c);
    if (rc != CDPWS_OK) return rc;
    if (len > 125) return failf(c, CDPWS_ERR_ARG,
                                "a Ping payload is limited to 125 bytes");
    if (len && !data) return failf(c, CDPWS_ERR_ARG, "null ping payload");
    rc = frame_send(c, CDPWS_OP_PING, data, len);
    if (rc != CDPWS_OK) c->failed = 1;
    return rc;
}

int cdpws_close(Cdpws *c, int code, const char *reason) {
    unsigned char p[125];
    size_t rl = reason ? strlen(reason) : 0;
    size_t n = 0;
    int rc;
    if (!c) return CDPWS_ERR_ARG;
    if (c->close_sent || c->state == CDPWS_ST_CLOSED) return CDPWS_OK;
    if (code != 0 && !close_code_ok(code))
        return failf(c, CDPWS_ERR_ARG, "Close status %d may not be sent", code);
    if (code == 0 && rl)
        return failf(c, CDPWS_ERR_ARG, "a Close reason needs a status code");
    if (rl > 123) return failf(c, CDPWS_ERR_ARG,
                               "a Close reason is limited to 123 bytes");
    if (rl && !utf8_valid((const unsigned char *)reason, rl))
        return failf(c, CDPWS_ERR_UTF8, "the Close reason is not valid UTF-8");
    if (code) {
        p[n++] = (unsigned char)(code >> 8);
        p[n++] = (unsigned char)(code & 0xFF);
    }
    if (rl) { memcpy(p + n, reason, rl); n += rl; }
    rc = frame_send(c, CDPWS_OP_CLOSE, p, n);
    c->close_sent = 1;
    if (c->state == CDPWS_ST_OPEN) c->state = CDPWS_ST_CLOSING;
    if (rc != CDPWS_OK) { c->failed = 1; return rc; }
    return CDPWS_OK;
}

int cdpws_is_open(const Cdpws *c) {
    return c && c->state == CDPWS_ST_OPEN && !c->close_sent && !c->failed;
}

unsigned cdpws_pings(const Cdpws *c) {
    return c ? c->pings : 0;
}

/* ============================================================
 *  13. HTTP GET on the same transport
 * ============================================================ */
int cdpws_http_get(const CdpwsTransport *tr, const char *host, int port,
                   const char *path, char *body, size_t cap,
                   size_t *body_len, char *err, size_t esz) {
    char req[1600];
    char head[CDPWS_HEAD_MAX];
    size_t got = 0, head_end = 0, have, written = 0, i;
    int rc, status = 0, rl;
    const char *val;
    size_t vlen, clen = 0;
    int have_clen = 0;

    if (err && esz) err[0] = '\0';
    if (!tr || !tr->connect || !tr->read || !tr->write || !tr->close) {
        err_write(err, esz, "the transport needs connect/read/write/close");
        return CDPWS_ERR_ARG;
    }
    if (!path || path[0] != '/' || !body || cap == 0 || !body_len) {
        err_write(err, esz, "invalid argument");
        return CDPWS_ERR_ARG;
    }
    if (port < 1 || port > 65535) {
        err_write(err, esz, "the port %d is outside 1..65535", port);
        return CDPWS_ERR_ARG;
    }
    if (!host || !host[0]) host = "127.0.0.1";
    if (!header_safe(host) || !header_safe(path)) {
        err_write(err, esz, "the request host or path contains a space or control byte");
        return CDPWS_ERR_ARG;
    }

    rl = snprintf(req, sizeof req,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Accept: application/json\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  path, host, port);
    if (rl < 0 || (size_t)rl >= sizeof req) {
        err_write(err, esz, "the request line does not fit");
        return CDPWS_ERR_ARG;
    }

    rc = tr->connect(tr->ctx, host, port, 10000);
    if (rc != CDPWS_OK) {
        err_write(err, esz, "connect to %s:%d: %s", host, port, cdpws_strerror(rc));
        tr->close(tr->ctx);
        return rc;
    }
    for (i = 0; i < (size_t)rl; ) {
        long w = tr->write(tr->ctx, req + i, (size_t)rl - i);
        if (w > 0) { i += (size_t)w; continue; }
        rc = (w == CDPWS_ERR_TIMEOUT) ? CDPWS_ERR_TIMEOUT : CDPWS_ERR_IO;
        err_write(err, esz, "writing the GET request: %s", cdpws_strerror(rc));
        tr->close(tr->ctx);
        return rc;
    }
    for (;;) {
        long r;
        head_end = http_head_end(head, got);
        if (head_end) break;
        if (got == sizeof head) {
            err_write(err, esz, "the HTTP response header is larger than %u bytes",
                      (unsigned)sizeof head);
            tr->close(tr->ctx);
            return CDPWS_ERR_TOO_BIG;
        }
        r = tr->read(tr->ctx, head + got, sizeof head - got);
        if (r > 0) {
            if ((size_t)r > sizeof head - got) {
                err_write(err, esz, "the transport read past the header buffer");
                tr->close(tr->ctx);
                return CDPWS_ERR_IO;
            }
            got += (size_t)r;
            continue;
        }
        if (r == CDPWS_IO_AGAIN) {
            err_write(err, esz, "the transport had no header bytes (a read must "
                                "be able to wait during an HTTP exchange)");
            tr->close(tr->ctx);
            return CDPWS_ERR_TIMEOUT;
        }
        if (r == 0) {
            err_write(err, esz, "the server closed before the HTTP header ended");
            tr->close(tr->ctx);
            return CDPWS_ERR_HTTP;
        }
        rc = (int)r;
        err_write(err, esz, "reading the HTTP header: %s", cdpws_strerror(rc));
        tr->close(tr->ctx);
        return rc;
    }

    rc = http_status(head, head_end, &status);
    if (rc != CDPWS_OK) {
        err_write(err, esz, "the HTTP status line is malformed");
        tr->close(tr->ctx);
        return CDPWS_ERR_HTTP;
    }
    if (status != 200) {
        err_write(err, esz, "GET %s answered %d, not 200", path, status);
        tr->close(tr->ctx);
        return CDPWS_ERR_HTTP;
    }
    switch (http_header(head, head_end, "Transfer-Encoding", &val, &vlen)) {
    case 0: break;
    case 1:
        err_write(err, esz, "the response uses Transfer-Encoding, which this "
                            "client does not implement");
        tr->close(tr->ctx);
        return CDPWS_ERR_HTTP;
    default:
        err_write(err, esz, "the HTTP headers are malformed");
        tr->close(tr->ctx);
        return CDPWS_ERR_HTTP;
    }
    switch (http_header(head, head_end, "Content-Length", &val, &vlen)) {
    case 1:
        rc = http_content_length(val, vlen, &clen);
        if (rc != CDPWS_OK) {
            err_write(err, esz, "Content-Length is not a usable decimal number");
            tr->close(tr->ctx);
            return rc;
        }
        have_clen = 1;
        break;
    case 0:
        break;
    default:
        err_write(err, esz, "the HTTP headers are malformed");
        tr->close(tr->ctx);
        return CDPWS_ERR_HTTP;
    }

    have = got - head_end;
    if (have_clen) {
        if (clen + 1 > cap) {
            err_write(err, esz, "the body is %u bytes, the buffer is %u",
                      (unsigned)clen, (unsigned)cap);
            tr->close(tr->ctx);
            return CDPWS_ERR_NOSPACE;
        }
        if (have > clen) {
            err_write(err, esz, "the server sent more than its Content-Length");
            tr->close(tr->ctx);
            return CDPWS_ERR_HTTP;
        }
        if (have) {
            memcpy(body, head + head_end, have);
            written = have;
        }
        while (written < clen) {
            long r = tr->read(tr->ctx, body + written, clen - written);
            if (r > 0) {
                if ((size_t)r > clen - written) {
                    err_write(err, esz, "the server sent more than its Content-Length");
                    tr->close(tr->ctx);
                    return CDPWS_ERR_HTTP;
                }
                written += (size_t)r;
                continue;
            }
            if (r == 0) {
                err_write(err, esz, "the server closed after %u of %u body bytes",
                          (unsigned)written, (unsigned)clen);
                tr->close(tr->ctx);
                return CDPWS_ERR_HTTP;
            }
            if (r == CDPWS_IO_AGAIN) {
                err_write(err, esz, "the transport had no body bytes");
                tr->close(tr->ctx);
                return CDPWS_ERR_TIMEOUT;
            }
            rc = (int)r;
            err_write(err, esz, "reading the HTTP body: %s", cdpws_strerror(rc));
            tr->close(tr->ctx);
            return rc;
        }
    } else {
        if (have > cap - 1) {
            err_write(err, esz, "the body does not fit in %u bytes", (unsigned)cap);
            tr->close(tr->ctx);
            return CDPWS_ERR_NOSPACE;
        }
        if (have) {
            memcpy(body, head + head_end, have);
            written = have;
        }
        for (;;) {
            long r;
            if (written == cap - 1) {
                /* the limit is reached: only EOF can make this a complete
                 * body, so probe with a single byte */
                char probe;
                r = tr->read(tr->ctx, &probe, 1);
                if (r == 0) break;
                if (r > 0) {
                    err_write(err, esz, "the body does not fit in %u bytes",
                              (unsigned)cap);
                    tr->close(tr->ctx);
                    return CDPWS_ERR_NOSPACE;
                }
            } else {
                r = tr->read(tr->ctx, body + written, cap - 1 - written);
                if (r > 0) { written += (size_t)r; continue; }
                if (r == 0) break;
            }
            if (r == CDPWS_IO_AGAIN) {
                err_write(err, esz, "the transport had no body bytes");
                tr->close(tr->ctx);
                return CDPWS_ERR_TIMEOUT;
            }
            rc = (int)r;
            err_write(err, esz, "reading the HTTP body: %s", cdpws_strerror(rc));
            tr->close(tr->ctx);
            return rc;
        }
    }
    body[written] = '\0';
    *body_len = written;
    tr->close(tr->ctx);
    return CDPWS_OK;
}

/* ============================================================
 *  14. The opening handshake
 * ============================================================ */
/* Read until the response head is complete, leaving the bytes (and any
 * frames that arrived with them) in c->in. */
static int read_upgrade_head(Cdpws *c) {
    for (;;) {
        size_t he = http_head_end((const char *)c->in, c->in_len);
        long r;
        if (he) return (int)he;
        if (c->in_len == c->in_cap)
            return failf(c, CDPWS_ERR_TOO_BIG,
                "the upgrade response header is larger than %u bytes",
                (unsigned)c->in_cap);
        r = c->tr.read(c->tr.ctx, c->in + c->in_len, c->in_cap - c->in_len);
        if (r > 0) {
            if ((size_t)r > c->in_cap - c->in_len)
                return failf(c, CDPWS_ERR_IO,
                    "the transport claimed to read more bytes than its buffer");
            c->in_len += (size_t)r;
            continue;
        }
        if (r == 0) return failf(c, CDPWS_ERR_CLOSED,
            "the browser closed the connection before the upgrade response");
        if (r == CDPWS_IO_AGAIN) return failf(c, CDPWS_ERR_TIMEOUT,
            "the transport had no upgrade bytes (a read must be able to wait "
            "for the response during cdpws_open)");
        if ((int)r == CDPWS_ERR_TIMEOUT)
            return failf(c, CDPWS_ERR_TIMEOUT, "the upgrade response deadline expired");
        return failf(c, CDPWS_ERR_IO, "reading the upgrade response failed (%ld)", r);
    }
}

static int check_upgrade(Cdpws *c, size_t head_len, const char *key_b64) {
    char expected[CDPWS_ACCEPT_BUF];
    const char *val;
    size_t vlen;
    int status = 0;
    int rc;

    rc = http_status((const char *)c->in, head_len, &status);
    if (rc != CDPWS_OK) return failf(c, CDPWS_ERR_HANDSHAKE,
        "the upgrade response has no HTTP status line");
    if (status != 101) return failf(c, CDPWS_ERR_HANDSHAKE,
        "the upgrade answered %d, not 101", status);
    switch (http_header((const char *)c->in, head_len, "Upgrade", &val, &vlen)) {
    case 1:
        if (!http_token(val, vlen, "websocket"))
            return failf(c, CDPWS_ERR_HANDSHAKE,
                "the Upgrade header does not name websocket");
        break;
    case 0:
        return failf(c, CDPWS_ERR_HANDSHAKE, "the response has no Upgrade header");
    default:
        return failf(c, CDPWS_ERR_HANDSHAKE, "the response headers are malformed");
    }
    switch (http_header((const char *)c->in, head_len, "Connection", &val, &vlen)) {
    case 1:
        if (!http_token(val, vlen, "upgrade"))
            return failf(c, CDPWS_ERR_HANDSHAKE,
                "the Connection header does not contain Upgrade");
        break;
    case 0:
        return failf(c, CDPWS_ERR_HANDSHAKE, "the response has no Connection header");
    default:
        return failf(c, CDPWS_ERR_HANDSHAKE, "the response headers are malformed");
    }
    switch (http_header((const char *)c->in, head_len,
                        "Sec-WebSocket-Accept", &val, &vlen)) {
    case 1:
        break;
    case 0:
        return failf(c, CDPWS_ERR_HANDSHAKE,
            "the response has no Sec-WebSocket-Accept (fail closed)");
    default:
        return failf(c, CDPWS_ERR_HANDSHAKE, "the response headers are malformed");
    }
    rc = cdpws_accept_key(key_b64, expected);
    if (rc != CDPWS_OK) return failf(c, CDPWS_ERR_HANDSHAKE,
        "cannot compute the expected Sec-WebSocket-Accept");
    if (vlen != CDPWS_ACCEPT_LEN || memcmp(val, expected, CDPWS_ACCEPT_LEN) != 0)
        return failf(c, CDPWS_ERR_HANDSHAKE,
            "Sec-WebSocket-Accept is %.*s, expected %s",
            (int)(vlen > 40 ? 40 : vlen), val, expected);
    /* We offered neither, so a server must not select either (§4.1). */
    switch (http_header((const char *)c->in, head_len,
                        "Sec-WebSocket-Extensions", &val, &vlen)) {
    case 0: break;
    case 1: return failf(c, CDPWS_ERR_HANDSHAKE,
        "the server selected an extension that was not offered");
    default: return failf(c, CDPWS_ERR_HANDSHAKE, "the response headers are malformed");
    }
    switch (http_header((const char *)c->in, head_len,
                        "Sec-WebSocket-Protocol", &val, &vlen)) {
    case 0: break;
    case 1: return failf(c, CDPWS_ERR_HANDSHAKE,
        "the server selected a subprotocol that was not offered");
    default: return failf(c, CDPWS_ERR_HANDSHAKE, "the response headers are malformed");
    }
    return CDPWS_OK;
}

Cdpws *cdpws_open(const CdpwsTransport *tr, const CdpwsOpenOpts *o,
                  char *err, size_t esz) {
    char local[CDPWS_ERR_MAX];
    unsigned char key[CDPWS_KEY_LEN];
    char key_b64[CDPWS_KEY_B64_LEN + 1];
    char req[2048];
    const char *host;
    int rl, rc, head_len;
    size_t off;
    Cdpws *c;

    if (!err || esz == 0) { err = local; esz = sizeof local; }
    err[0] = '\0';
    if (!tr || !tr->connect || !tr->read || !tr->write) {
        err_write(err, esz, "the transport needs connect/read/write");
        return NULL;
    }
    if (!o || !o->path) {
        err_write(err, esz, "cdpws_open needs a path");
        return NULL;
    }
    if (o->port < 1 || o->port > 65535) {
        err_write(err, esz, "the port %d is outside 1..65535", o->port);
        return NULL;
    }
    if (o->path[0] != '/' || !header_safe(o->path)) {
        err_write(err, esz, "the WebSocket path must start with '/' and contain "
                            "only printable ASCII");
        return NULL;
    }
    host = o->host ? o->host : "127.0.0.1";
    if (!header_safe(host)) {
        err_write(err, esz, "the host is not printable ASCII");
        return NULL;
    }
    if (o->origin && !header_safe(o->origin)) {
        err_write(err, esz, "the origin is not printable ASCII");
        return NULL;
    }

    c = (Cdpws *)calloc(1, sizeof *c);
    if (!c) {
        err_write(err, esz, "out of memory");
        return NULL;
    }
    c->tr = *tr;
    c->max_message = o->max_message ? o->max_message : CDPWS_DEFAULT_MAX_MESSAGE;
    c->rand = o->rand;
    c->rand_ctx = o->rand_ctx;
    c->in_cap = CDPWS_IN_CAP;
    c->in = (unsigned char *)malloc(c->in_cap);
    if (!c->in) {
        err_write(err, esz, "out of memory");
        free(c);
        return NULL;
    }

    if (o->key) {
        memcpy(key, o->key, sizeof key);
    } else {
        rc = random_fill(c, key, sizeof key);
        if (rc != CDPWS_OK) {
            err_write(err, esz, "%s", c->err);
            free(c->in);
            free(c);
            return NULL;
        }
    }
    rc = cdpws_key_to_b64(key, key_b64);
    if (rc != CDPWS_OK) {
        err_write(err, esz, "%s", cdpws_strerror(rc));
        free(c->in);
        free(c);
        return NULL;
    }

    rl = snprintf(req, sizeof req,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "%s%s"
                  "\r\n",
                  o->path, host, o->port, key_b64,
                  o->origin ? "Origin: " : "", o->origin ? o->origin : "");
    if (rl < 0 || (size_t)rl >= sizeof req) {
        err_write(err, esz, "the upgrade request does not fit");
        free(c->in);
        free(c);
        return NULL;
    }

    rc = c->tr.connect(c->tr.ctx, host, o->port,
                       o->timeout_ms > 0 ? o->timeout_ms : 10000);
    if (rc != CDPWS_OK) {
        err_write(err, esz, "connect to %s:%d: %s", host, o->port,
                  cdpws_strerror(rc));
        if (c->tr.close) c->tr.close(c->tr.ctx);
        free(c->in);
        free(c);
        return NULL;
    }
    for (off = 0; off < (size_t)rl; ) {
        long w = c->tr.write(c->tr.ctx, req + off, (size_t)rl - off);
        if (w > 0) { off += (size_t)w; continue; }
        err_write(err, esz, "writing the upgrade request: %s",
                  cdpws_strerror(w == CDPWS_ERR_TIMEOUT ? CDPWS_ERR_TIMEOUT
                                                       : CDPWS_ERR_IO));
        if (c->tr.close) c->tr.close(c->tr.ctx);
        free(c->in);
        free(c);
        return NULL;
    }
    head_len = read_upgrade_head(c);
    if (head_len < 0) {
        err_write(err, esz, "%s", c->err);
        if (c->tr.close) c->tr.close(c->tr.ctx);
        free(c->in);
        free(c);
        return NULL;
    }
    rc = check_upgrade(c, (size_t)head_len, key_b64);
    if (rc != CDPWS_OK) {
        err_write(err, esz, "%s", c->err);
        if (c->tr.close) c->tr.close(c->tr.ctx);
        free(c->in);
        free(c);
        return NULL;
    }
    /* bytes after the head are already frames (Chromium sends none before
     * the first command, but a peer is allowed to) */
    memmove(c->in, c->in + head_len, c->in_len - (size_t)head_len);
    c->in_len -= (size_t)head_len;
    c->state = CDPWS_ST_OPEN;
    return c;
}

void cdpws_free(Cdpws *c) {
    if (!c) return;
    if (c->state == CDPWS_ST_OPEN && !c->close_sent)
        (void)cdpws_close(c, 1000, NULL);
    if (c->tr.close) c->tr.close(c->tr.ctx);
    free(c->in);
    free(c->msg);
    free(c);
}

/* ============================================================
 *  15. The POSIX transport
 * ============================================================ */
#ifndef _WIN32
static int net_connect(void *ctx, const char *host, int port, int timeout_ms) {
    CdpwsNet *n = (CdpwsNet *)ctx;
    struct sockaddr_in sa;
    struct pollfd p;
    int fd, flags, rc, so, pr;
    socklen_t sl;

    if (!n) return CDPWS_ERR_ARG;
    if (n->fd >= 0) { close(n->fd); n->fd = -1; }
    if (!host || strcmp(host, "127.0.0.1") != 0) return CDPWS_ERR_ARG;
    if (port < 1 || port > 65535) return CDPWS_ERR_ARG;
    if (timeout_ms <= 0) timeout_ms = 10000;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return CDPWS_ERR_IO;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return CDPWS_ERR_IO;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc != 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            close(fd);
            return CDPWS_ERR_IO;
        }
        p.fd = fd;
        p.events = POLLOUT;
        p.revents = 0;
        do { pr = poll(&p, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
        if (pr == 0) { close(fd); return CDPWS_ERR_TIMEOUT; }
        if (pr < 0)  { close(fd); return CDPWS_ERR_IO; }
        so = 0;
        sl = sizeof so;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so != 0) {
            close(fd);
            return so == ETIMEDOUT ? CDPWS_ERR_TIMEOUT : CDPWS_ERR_IO;
        }
    }
    n->fd = fd;
    return CDPWS_OK;
}

static long net_read(void *ctx, void *buf, size_t len) {
    CdpwsNet *n = (CdpwsNet *)ctx;
    int t;
    if (!n || n->fd < 0 || (!buf && len)) return CDPWS_ERR_ARG;
    if (len == 0) return 0;
    t = n->io_timeout_ms > 0 ? n->io_timeout_ms : 10000;
    for (;;) {
        ssize_t r = read(n->fd, buf, len);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p;
            int pr;
            p.fd = n->fd;
            p.events = POLLIN;
            p.revents = 0;
            do { pr = poll(&p, 1, t); } while (pr < 0 && errno == EINTR);
            if (pr == 0) return CDPWS_ERR_TIMEOUT;
            if (pr < 0)  return CDPWS_ERR_IO;
            continue;
        }
        return CDPWS_ERR_IO;
    }
}

static long net_write(void *ctx, const void *buf, size_t len) {
    CdpwsNet *n = (CdpwsNet *)ctx;
    int t;
    if (!n || n->fd < 0 || (!buf && len)) return CDPWS_ERR_ARG;
    if (len == 0) return 0;
    t = n->io_timeout_ms > 0 ? n->io_timeout_ms : 10000;
    for (;;) {
        ssize_t w = write(n->fd, buf, len);
        if (w >= 0) return (long)w;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p;
            int pr;
            p.fd = n->fd;
            p.events = POLLOUT;
            p.revents = 0;
            do { pr = poll(&p, 1, t); } while (pr < 0 && errno == EINTR);
            if (pr == 0) return CDPWS_ERR_TIMEOUT;
            if (pr < 0)  return CDPWS_ERR_IO;
            continue;
        }
        return CDPWS_ERR_IO;
    }
}

static void net_close(void *ctx) {
    cdpws_net_close((CdpwsNet *)ctx);
}

CdpwsTransport cdpws_net_transport(CdpwsNet *net) {
    CdpwsTransport tr;
    tr.ctx = net;
    tr.connect = net_connect;
    tr.read = net_read;
    tr.write = net_write;
    tr.close = net_close;
    return tr;
}

void cdpws_net_close(CdpwsNet *net) {
    if (!net) return;
    if (net->fd >= 0) {
        close(net->fd);
        net->fd = -1;
    }
}
#endif /* !_WIN32 */
