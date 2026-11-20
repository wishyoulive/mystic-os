/* ============================================================
 *  Mystic OS - net/dns.h
 *
 *  A stub resolver written from RFC 1034 and RFC 1035, with
 *  RFC 3596 (AAAA), RFC 2308 (negative caching) and RFC 6891
 *  (EDNS0). No third-party code, no libc resolver, no hidden
 *  syscalls: every byte that goes on the wire is built here and
 *  every byte that comes back is parsed here.
 *
 *  WHY AN OPS STRUCT
 *
 *  The resolver core does no I/O of its own. Sockets, the clock
 *  and reading /etc/hosts all go through DnsOps. That is not a
 *  testing shortcut: it means the same code runs in the OS (with
 *  dns_ops_posix(), the last function in dns.c) and in a host
 *  harness that answers with canned RFC 1035 bytes. The core is
 *  portable C11 with no headers beyond the C standard library.
 *
 *  WHAT IS IMPLEMENTED, AND WHERE IT COMES FROM
 *
 *    message format          RFC 1035 s4.1: 12 octet header, QD/AN/NS/AR
 *                            counts, 32 bit TTL, 16 bit RDLENGTH.
 *    name compression        RFC 1035 s4.1.4: a pointer is 0xC0 | 14 bit
 *                            offset, and it "points to a prior occurrence",
 *                            so a legal pointer always points backwards.
 *                            That rule is what makes a compression loop
 *                            impossible to follow: the core rejects any
 *                            pointer whose target is not strictly less than
 *                            the pointer's own offset, and additionally
 *                            caps the number of pointer hops.
 *    name length             RFC 1035 s2.3.4: 255 octets of wire name and
 *                            63 octets per label. Enforced on read too, so
 *                            a decompression bomb cannot overrun the buffer.
 *    UDP                     RFC 1035 s4.2.1, plus EDNS0 (RFC 6891): the
 *                            request carries an OPT pseudo-RR (type 41,
 *                            root owner name, CLASS = the requestor's UDP
 *                            payload size, TTL = extended RCODE/version/flags)
 *                            so a 4096 octet reply is legal. RFC 6891 s6.2.5
 *                            says a requestor that advertises less than 512
 *                            is treated as if it had said 512.
 *    truncation              RFC 1035 s4.2.1: TC set means "retry over TCP".
 *                            RFC 1035 s4.2.2: TCP carries a two octet length
 *                            prefix on each message.
 *    EDNS0 fallback          RFC 6891 s6.2.2/7: a server that answers
 *                            FORMERR (or NOTIMP) to an EDNS query may not
 *                            know EDNS at all, so the query is retried once
 *                            without the OPT record before moving on.
 *    CNAME                   RFC 1034 s4.3.2 / RFC 1035 s3.3.1: follow the
 *                            chain within the response first, then ask again
 *                            for the target, up to a depth cap. The TTL of
 *                            the answer is the smallest TTL along the chain
 *                            (RFC 2181 s5.2: an RRset with mixed TTLs is
 *                            treated as having the minimum; RFC 1034 s5.3.3
 *                            keeps the chain's minimum when caching).
 *    TTL                     RFC 2181 s5.1: a TTL with the top bit set is
 *                            treated as zero, so it cannot wrap into a
 *                            short-lived entry that looks long-lived.
 *    negative caching        RFC 2308 s2.1 and s5: NXDOMAIN is cached for
 *                            the name and every type, NODATA only for the
 *                            type asked, and the lifetime is the smaller of
 *                            the SOA TTL and the SOA MINIMUM field
 *                            (MINIMUM itself is defined in RFC 1035 s3.3.13).
 *                            An SOA only counts when its owner is the name
 *                            the chain ended at or a zone above it (RFC 2308
 *                            s2.2), and the entry is keyed to that name, not
 *                            to an alias that pointed at it. With no such SOA
 *                            there is nothing to time the entry with, so
 *                            nothing is cached (RFC 2308 s5).
 *    PTR                     RFC 1035 s3.5 (in-addr.arpa) and RFC 3596 s2.5
 *                            (ip6.arpa, one nibble per label).
 *    AAAA                    RFC 3596 s2.1: type 28, 16 octet RDATA.
 *
 *  WHAT IS POLICY, NOT PROTOCOL
 *
 *  The RFCs do not say how long to wait, how many times to ask, or how big
 *  a cache should be, so those are fields of DnsConfig and the caller picks
 *  them. The only numbers fixed here are protocol limits and capacities,
 *  each marked as such.
 *
 *  WHAT THIS IS NOT
 *
 *    - No DNSSEC (no RRSIG/NSEC validation), so a forged reply is not
 *      detected beyond the ID check. Do not treat an answer as
 *      authenticated; this is a stub resolver, not a validating one.
 *    - No DNS-over-TLS/HTTPS, no mDNS, no dynamic update, no zone transfer.
 *    - No search domains or ndots processing: dns_resolve() takes a name and
 *      asks for exactly that name. Appending a search suffix is a caller
 *      policy (see dns_resolve_search()) and is only done when the caller
 *      asks for it.
 *    - No IDNA/punycode. Bytes are passed through; only ASCII A-Z are
 *      folded, and only for comparisons and for the query (RFC 4343:
 *      DNS names are case insensitive).
 *    - The cache is a fixed array inside DnsResolver (about 35 KiB with the
 *      defaults below), not a heap. Eviction is deterministic: the entry
 *      that expires first is reused, ties going to the lowest slot, so a
 *      harness can predict it.
 *
 *  THREADING
 *
 *  One DnsResolver per thread. There are no locks and no globals; the
 *  posix ops singleton is the only shared object and it holds no state.
 * ============================================================ */
#ifndef MYSTIC_NET_DNS_H
#define MYSTIC_NET_DNS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Protocol constants (RFC 1035, RFC 3596, RFC 6891)
 * ------------------------------------------------------------ */

/* RFC 1035 s2.3.4: labels are at most 63 octets, and a name is at most 255
 * octets of wire data (including the length octets and the root label). The
 * extra byte here is the C string terminator, not wire data. */
#define DNS_LABEL_MAX       63
#define DNS_MAX_NAME        256

/* RFC 1035 s4.1.1: the fixed header is 12 octets. */
#define DNS_HEADER_LEN      12

/* The EDNS0 payload size this resolver advertises. RFC 6891 does not name a
 * required value; its Appendix A.1 example request uses 4096, which is also
 * the largest reply a single unfragmented IPv4 UDP datagram carries without
 * IP options on a 1500 octet MTU path. 512 is the RFC 1035 s4.2.1 limit that
 * applies when EDNS0 is absent. */
#define DNS_MAX_MSG         4096
#define DNS_MIN_UDP         512

/* Types (RFC 1035 s3.2.2/3.2.3, RFC 3596 s2.1) and the OPT pseudo-type
 * (RFC 6891 s6.1.1). */
#define DNS_T_A             1
#define DNS_T_NS            2
#define DNS_T_CNAME         5
#define DNS_T_SOA           6
#define DNS_T_PTR           12
#define DNS_T_AAAA          28
#define DNS_T_OPT           41

/* Classes (RFC 1035 s3.2.4/3.2.5). */
#define DNS_C_IN            1

/* RCODEs (RFC 1035 s4.1.1, RFC 2136, RFC 6891 s9). */
#define DNS_RCODE_NOERROR   0
#define DNS_RCODE_FORMERR   1
#define DNS_RCODE_SERVFAIL  2
#define DNS_RCODE_NXDOMAIN  3
#define DNS_RCODE_NOTIMP    4
#define DNS_RCODE_REFUSED   5
#define DNS_RCODE_BADVERS   16   /* EDNS version not implemented */

/* Header flag bits (RFC 1035 s4.1.1). */
#define DNS_F_QR            0x8000
#define DNS_F_OPCODE        0x7800
#define DNS_F_AA            0x0400
#define DNS_F_TC            0x0200
#define DNS_F_RD            0x0100
#define DNS_F_RA            0x0080

/* EDNS0: the TTL field of the OPT RR is split into extended RCODE (8 bits),
 * version (8 bits) and flags (16 bits) - RFC 6891 s6.1.3. Version 0 is the
 * only one defined, and the DO bit is 0x8000 (RFC 6891 s6.1.4). */
#define DNS_EDNS_VERSION    0
#define DNS_EDNS_DO         0x8000

/* Address families used in DnsAddr. These are the values of the resolver's
 * own naming, not AF_INET/AF_INET6, so the core stays free of <sys/socket.h>;
 * dns_ops_posix() maps them. */
#define DNS_AF_INET         1
#define DNS_AF_INET6        2

/* ------------------------------------------------------------
 *  Capacities. These are limits of this implementation, not protocol
 *  numbers: they bound the memory a resolver owns.
 * ------------------------------------------------------------ */
#define DNS_MAX_SERVERS     8    /* nameservers kept from resolv.conf       */
#define DNS_MAX_ADDRS       16   /* addresses kept from one answer          */
#define DNS_CACHE_MAX       32   /* positive + negative cache entries       */
#define DNS_MAX_TRACE       16   /* attempt records kept for diagnostics    */
#define DNS_HOSTS_MAX       8192 /* bytes of /etc/hosts kept in the resolver */

/* How far a CNAME chain may be followed. RFC 1034 s4.3.2 gives an example
 * algorithm with no fixed cap; a cap is mandatory in practice because the
 * chain is attacker-controlled. 8 is this implementation's choice and is
 * reported as DNS_E_DEPTH, never silently ignored. */
#define DNS_MAX_CNAME_DEPTH 8

/* Compression pointer hops allowed while decompressing one name. The
 * strictly-backwards rule already guarantees termination (each hop reduces
 * the offset, so at most 16384 hops are possible); this cap makes a legal
 * but absurd chain fail fast, and is checked in addition to the offset rule.
 * It is a policy number. */
#define DNS_MAX_POINTERS    64

/* ------------------------------------------------------------
 *  Error codes. Every function returns one of these (or DNS_OK), so a
 *  caller can tell "the name does not exist" from "the wire was corrupt"
 *  from "nobody answered".
 * ------------------------------------------------------------ */
enum {
    DNS_OK           = 0,
    DNS_E_ARG        = -1,   /* bad argument                              */
    DNS_E_BADNAME    = -2,   /* name cannot be encoded (label/length)     */
    DNS_E_WIRE       = -3,   /* reply is malformed (lengths, counts)      */
    DNS_E_COMPRESS   = -4,   /* bad compression pointer, or a loop        */
    DNS_E_ID         = -5,   /* reply ID does not match the query         */
    DNS_E_RCODE      = -6,   /* server answered with a non-zero RCODE     */
    DNS_E_NXDOMAIN   = -7,   /* RFC 2308: the name does not exist         */
    DNS_E_NODATA     = -8,   /* the name exists, this type has no data    */
    DNS_E_DEPTH      = -9,   /* CNAME chain deeper than the cap           */
    DNS_E_CYCLE      = -10,  /* CNAME chain revisits a name               */
    DNS_E_TIMEOUT    = -11,  /* no reply within the timeout               */
    DNS_E_NET        = -12,  /* transport error (socket, connect, send)   */
    DNS_E_OVERFLOW   = -13,  /* a buffer was too small                    */
    DNS_E_TCP        = -14,  /* a TCP retry was needed and it failed      */
    DNS_E_NOCACHE    = -15,  /* internal: lookup missed (never returned   */
                             /* from dns_resolve, used by the cache)      */
    DNS_E_NOSERVER   = -16   /* no usable nameserver is configured        */
};

/* ------------------------------------------------------------
 *  Wire types
 * ------------------------------------------------------------ */

/* An IPv4 or IPv6 address. For a nameserver, `port` is meaningful (host byte
 * order); for an answer it is zero. `bytes` is 4 octets of address for
 * DNS_AF_INET and 16 for DNS_AF_INET6, in network order, which is also the
 * order the A/AAAA RDATA arrives in, so a copy is all a resolver needs. */
typedef struct {
    uint16_t family;      /* DNS_AF_INET / DNS_AF_INET6             */
    uint16_t port;        /* host order; 0 means DNS_PORT where used */
    uint8_t  bytes[16];
} DnsAddr;

#define DNS_PORT 53

/* A PTR target is a 255 octet name like everything else, named separately
 * because it is a name in its own right rather than a query name. */
#define DNS_MAX_PTR         DNS_MAX_NAME

/* One resolved answer. TTL is the lifetime in seconds of the whole answer:
 * the minimum over the RRset and over every CNAME followed to reach it. */
typedef struct {
    int      naddr;                  /* entries used in addr[]            */
    uint32_t ttl;                    /* seconds; see above                */
    uint16_t qtype;                  /* the type that was asked for       */
    uint16_t rcode;                  /* RCODE of the reply, 0 on success  */
    uint8_t  from_hosts;             /* 1 = answered from /etc/hosts      */
    uint8_t  from_cache;             /* 1 = answered from the cache       */
    uint8_t  used_tcp;               /* 1 = the UDP reply was truncated   */
    char     name[DNS_MAX_NAME];     /* the name asked, lowercased        */
    char     cname[DNS_MAX_NAME];    /* final canonical name; == name if
                                      * no CNAME was followed            */
    char     ptr[DNS_MAX_PTR];       /* PTR target, when qtype == PTR;
                                      * "" is the root                  */
    DnsAddr  addr[DNS_MAX_ADDRS];
} DnsAnswer;

/* ------------------------------------------------------------
 *  The platform seam: sockets, clock, and one file read.
 *
 *  udp_xchg / tcp_xchg send one query and return one reply.
 *      > 0        number of reply bytes written to `resp`
 *      0          nothing arrived (treated as a timeout)
 *      DNS_E_*    negative, passed back to the caller
 *  `timeout_ms` is the total budget for that exchange.
 *
 *  For TCP the implementation owns the two octet length prefix of RFC 1035
 *  s4.2.2 in both directions; dns_ops_posix() shows exactly how.
 *
 *  now_ms must be monotonic-ish and never go backwards; it is only used to
 *  stamp cache entries. read_file is optional: when it is NULL, /etc/hosts
 *  is simply not consulted and dns_hosts_reload() returns DNS_E_ARG.
 * ------------------------------------------------------------ */
typedef struct DnsOps {
    void *ctx;

    int (*udp_xchg)(void *ctx, const DnsAddr *srv, const uint8_t *q, size_t qlen,
                    uint8_t *resp, size_t rcap, int timeout_ms);
    int (*tcp_xchg)(void *ctx, const DnsAddr *srv, const uint8_t *q, size_t qlen,
                    uint8_t *resp, size_t rcap, int timeout_ms);
    uint64_t (*now_ms)(void *ctx);
    /* Read a whole text file. Returns 0 and sets *out_len on success; the
     * buffer is NUL terminated by the implementation. */
    int (*read_file)(void *ctx, const char *path, char *buf, size_t cap,
                     size_t *out_len);
} DnsOps;

/* ------------------------------------------------------------
 *  Configuration and state
 * ------------------------------------------------------------ */

typedef struct {
    const DnsAddr *servers;   /* array of nsrv nameservers                */
    int      nsrv;
    int      attempts;        /* tries per server (>= 1)                  */
    int      timeout_ms;      /* first round timeout, > 0                 */
    int      timeout_max_ms;  /* ceiling for the exponential growth       */
    uint16_t udp_size;        /* advertised EDNS0 payload; 0 -> 4096      */
    int      edns;            /* 0 disables EDNS0 entirely                */
} DnsConfig;

typedef struct {
    uint8_t  used;
    uint8_t  neg;             /* 1 = negative entry (NXDOMAIN or NODATA)  */
    uint8_t  naddr;
    uint16_t type;            /* qtype, or 0 for "any type" (NXDOMAIN)    */
    uint16_t rcode;           /* RCODE stored with a negative entry       */
    uint64_t expire_ms;
    char     name[DNS_MAX_NAME];
    char     cname[DNS_MAX_NAME];
    char     ptr[DNS_MAX_PTR];
    DnsAddr  addr[DNS_MAX_ADDRS];
} DnsCacheEntry;

/* One recorded network attempt. The harness reads this to prove the backoff
 * schedule and the server order; a running system can show it to a user
 * asking "why is this name slow". */
typedef struct {
    uint8_t  server;          /* index into srv[]                         */
    uint8_t  round;           /* index of the pass over all servers       */
    uint8_t  tcp;             /* 1 = this attempt used TCP                */
    uint8_t  edns;            /* 1 = the query carried an OPT RR          */
    int16_t  result;          /* DNS_OK or the DNS_E_* it failed with     */
    int32_t  timeout_ms;
} DnsAttempt;

typedef struct {
    uint64_t queries;         /* queries actually put on the wire         */
    uint64_t cache_hits;
    uint64_t neg_hits;
    uint64_t hosts_hits;
    uint64_t tc_retries;
    uint64_t edns_fallbacks;
    uint64_t timeouts;
    uint64_t id_mismatch;
    uint64_t rcode_errors;
    uint64_t malformed;
    uint64_t stored_positive;
    uint64_t stored_negative;
    uint64_t refused_ttl_zero; /* answers with TTL 0 are not cached       */
} DnsStats;

typedef struct {
    const DnsOps *ops;
    DnsAddr  srv[DNS_MAX_SERVERS];
    int      nsrv;
    int      attempts;
    int      timeout_ms;
    int      timeout_max_ms;
    uint16_t udp_size;
    int      edns;
    uint16_t next_id;
    uint64_t now;             /* last value read from ops->now_ms         */

    DnsCacheEntry cache[DNS_CACHE_MAX];
    DnsStats stats;
    DnsAttempt trace[DNS_MAX_TRACE];
    int      ntrace;

    char     hosts[DNS_HOSTS_MAX];
    size_t   hosts_len;
    int      hosts_loaded;
} DnsResolver;

/* ------------------------------------------------------------
 *  API
 * ------------------------------------------------------------ */

/* Fill `cfg` with this implementation's defaults: no servers, one attempt,
 * a 1000 ms first round, 8000 ms ceiling, EDNS0 on with 4096. The timing
 * values are policy, not protocol; override them. */
void dns_default_config(DnsConfig *cfg);

/* Copy the configuration into the resolver and clear the cache and counters.
 * Every pointer in cfg is copied, so cfg may be a temporary. */
int dns_resolver_init(DnsResolver *r, const DnsOps *ops, const DnsConfig *cfg);

/* Resolve `name` for `qtype` (DNS_T_A, DNS_T_AAAA, DNS_T_PTR, or any other
 * type). A single trailing dot is optional and is removed first, so
 * "example.com." and "example.com" are the same lookup, the same cache key
 * and the same out->name. On DNS_OK, `out` is filled and out->ttl is the
 * number of seconds the answer is good for. For A and AAAA the addresses are
 * in out->addr; for PTR the target name is in out->ptr; for any other type
 * DNS_OK means the RRset exists (that is all this resolver claims: it does
 * not copy records it has no representation for) and out->ttl is still set.
 * On any negative result `out` may still carry the RCODE, the canonical name
 * and the TTL that was learned, which is useful in a log line. */
int dns_resolve(DnsResolver *r, const char *name, uint16_t qtype, DnsAnswer *out);

/* Convenience wrappers. dns_lookup_ptr takes the address in network order
 * (4 or 16 octets) and builds the in-addr.arpa / ip6.arpa name first. */
int dns_lookup_a(DnsResolver *r, const char *name, DnsAnswer *out);
int dns_lookup_aaaa(DnsResolver *r, const char *name, DnsAnswer *out);
int dns_lookup_ptr(DnsResolver *r, uint16_t family, const uint8_t addr[16],
                   DnsAnswer *out);

/* Read and parse /etc/hosts through ops->read_file. Called by
 * dns_resolver_init() when read_file is present; call it again after the
 * file changes. Returns DNS_OK, or DNS_E_ARG without read_file. */
int dns_hosts_reload(DnsResolver *r);

/* Parse hosts text that is already in memory. Lines are
 * "address name [name ...]", '#' starts a comment, blank lines are skipped.
 * An unparseable address makes the whole call fail with DNS_E_WIRE so a
 * typo is not silently ignored; out-of-range names are DNS_E_BADNAME. */
int dns_parse_hosts(DnsResolver *r, const char *text, size_t len);

/* What resolv.conf said. Servers are copied out; a timeout/attempts of 0
 * means the file did not say, so the caller keeps its own default. */
typedef struct {
    DnsAddr servers[DNS_MAX_SERVERS];
    int     nsrv;
    int     timeout_ms;   /* "options timeout:<seconds>" * 1000, or 0 */
    int     attempts;     /* "options attempts:<n>", or 0            */
} DnsResolvConf;

/* Parse resolv.conf text: "nameserver <address>", "options timeout:<seconds>"
 * and "options attempts:<n>". Other keywords are ignored, as the format
 * allows a reader to do. Ports other than 53 are not expressible in
 * resolv.conf, so DnsAddr.port is left 0 (meaning DNS_PORT). */
int dns_parse_resolv_conf(const char *text, size_t len, DnsResolvConf *out);

/* Drop every cache entry. */
void dns_cache_flush(DnsResolver *r);

/* Number of live cache entries, and the remaining lifetime of the entry the
 * cache would answer `name`/`qtype` from (0 when there is no such entry).
 * These exist so a test can watch an entry expire without sleeping. The
 * lifetime is rounded up, so a live entry never reports 0. */
int dns_cache_count(const DnsResolver *r);
int dns_cache_ttl(const DnsResolver *r, const char *name, uint16_t qtype);

/* Address text <-> bytes. dns_addr_pton returns DNS_OK or DNS_E_BADNAME.
 * IPv4 refuses leading zeros ("010" is not 10 and not 8, so it is refused);
 * IPv6 accepts "::", at most one "::", 1..4 hex digits per group, and the
 * trailing dotted-quad form of RFC 4291 s2.2.3. dns_addr_ntop writes the
 * RFC 5952 s4 text form and returns `buf`. */
int dns_addr_pton(const char *text, DnsAddr *out);
const char *dns_addr_ntop(const DnsAddr *a, char *buf, size_t cap);

/* Build the reverse-lookup name for an address: "4.3.2.1.in-addr.arpa"
 * (RFC 1035 s3.5) or 32 reversed nibbles under "ip6.arpa" (RFC 3596 s2.5).
 * `addr` must hold 4 bytes for DNS_AF_INET and 16 for DNS_AF_INET6. */
int dns_ptr_name(uint16_t family, const uint8_t addr[16], char *out, size_t cap);

/* A human-readable reason for a DNS_E_* code, for a status line or a log. */
const char *dns_strerror(int err);

/* The POSIX implementation of DnsOps (real sendto/recvfrom/connect/read/
 * write, with poll() for the timeout). Returns a singleton whose ctx is
 * unused, or NULL when the file was built with DNS_ENABLE_POSIX_OPS=0. */
const DnsOps *dns_ops_posix(void);
/* 1 when dns_ops_posix() returns an implementation, 0 otherwise. */
int dns_posix_available(void);

#ifdef __cplusplus
}
#endif
#endif /* MYSTIC_NET_DNS_H */
