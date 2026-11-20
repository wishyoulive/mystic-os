/* ============================================================
 *  Mystic OS - net/url.h
 *  An RFC 3986 URI layer: parse, validate, normalise, resolve,
 *  percent-encode/decode, query strings, absolute-URL building.
 *
 *  Every rule here is taken from a document, not invented:
 *
 *    RFC 3986  URI Generic Syntax      - grammar (S2, S3), reference
 *                                        resolution (S5.2), normalisation
 *                                        and comparison (S6.2), the
 *                                        Appendix B parser.
 *    RFC 3492  Punycode                - S5 parameters, S6.3 encoder.
 *    RFC 3629  UTF-8                   - valid octet sequences only
 *                                        (no overlongs, no surrogates,
 *                                        nothing above U+10FFFF).
 *    RFC 1035  Domain names            - S2.3.4 label <= 63 octets,
 *                                        name <= 255 octets on the wire.
 *    RFC 6454  Web origin              - the scheme/host/port triple.
 *    RFC 9110  HTTP semantics          - S4.2.2/4.2.4 default ports.
 *    RFC 6455  WebSocket               - S3 default ports.
 *    RFC 1738  FTP                     - S3.2 default port.
 *    RFC 9112  HTTP/1.1                - S3.2.1 origin-form.
 *
 *  WHATWG URL, where this file deliberately does NOT follow it:
 *
 *    - There is no second separator: a raw backslash is REJECTED in
 *      every component, exactly as the RFC 3986 grammar requires (no
 *      production contains one).  The URL Standard reads a backslash
 *      as '/' for its special schemes; this parser does not, because
 *      one resource would then have two spellings, and
 *      "http://good.com\@evil.com/" is the classic URL-confusion bug
 *      (the standard sees host good.com, a naive split on '@' sees
 *      evil.com).  A backslash that is data travels as %5C and
 *      round-trips unchanged.
 *    - Raw controls and raw space are REJECTED, not stripped.  RFC
 *      3986 S2 has no room for them either; silently deleting a tab
 *      out of a host name hides a bug in the caller.
 *    - WHATWG fills in a "//" for "http:host"; RFC 3986 S5.4.2
 *      requires "http:g" to stay "http:g", so the strict rule stays.
 *    - No IDNA mapping/NFC pass.  We lowercase ASCII and punycode the
 *      non-ASCII labels (RFC 3492); full IDNA2008 (RFC 5891/5892)
 *      needs the Unicode tables, which this system does not carry.
 *      A decomposed accent therefore encodes differently from a
 *      precomposed one, exactly as the bytes differ.
 *
 *  STORAGE, NOT PROTOCOL
 *
 *  RFC 3986 imposes no maximum length.  The URL_MAX_* values below are
 *  this implementation's storage bounds: a Url is a fixed-size object
 *  with no allocation, so the whole desktop can hold one on the stack
 *  and a bad packet cannot ask for more memory.  Overflowing a bound
 *  returns URL_ETOOLONG instead of truncating - a truncated URL that
 *  still parses is worse than an error.
 *
 *  Every function is reentrant, allocates nothing, keeps no state and
 *  never touches errno.  Errors are the URL_E* codes and url_strerror()
 *  spells them out.
 *
 *  NO PARTIAL RESULTS
 *
 *  Every function that writes a string clears it (out[0] = 0) before
 *  it can fail - a NULL argument included - and sets *outlen to 0 when
 *  it takes one, so a caller that ignores the return value reads an
 *  empty string, never stale bytes.  The single exception is osz == 0:
 *  a zero-size buffer cannot hold even "", so that case returns
 *  URL_ETOOLONG and writes nothing.  The *outlen values are byte
 *  counts (a decoded component may contain a NUL), not C strings.
 * ============================================================ */
#ifndef MYSTIC_NET_URL_H
#define MYSTIC_NET_URL_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------
 *  Limits (storage policy - see the note above)
 * ------------------------------------------------------------ */
#define URL_MAX_SCHEME      32
#define URL_MAX_USERINFO   256
#define URL_MAX_HOST       256   /* RFC 1035 S2.3.4: 255 octets wire form */
#define URL_MAX_PORT         6   /* "65535" + NUL */
#define URL_MAX_PATH      2048
#define URL_MAX_QUERY     2048
#define URL_MAX_FRAGMENT  1024

#define URL_MAX_PARAMS      64
#define URL_MAX_NAME       128
#define URL_MAX_VALUE      512

/* ------------------------------------------------------------
 *  Error codes.  Zero is success, everything else is negative and
 *  names exactly one reason.
 * ------------------------------------------------------------ */
enum {
    URL_OK          =  0,
    URL_EINVAL      = -1,   /* NULL pointer                                 */
    URL_ETOOLONG    = -2,   /* a component does not fit its storage bound   */
                            /* (a zero-size output buffer is ETOOLONG too)  */
    URL_ESCHEME     = -3,   /* missing scheme, or not ALPHA *( ALPHA /      */
                            /* DIGIT / "+" / "-" / "." )   RFC 3986 S3.1   */
    URL_EBADCHAR    = -4,   /* raw control, space or backslash (S2)        */
    URL_EBADPCT     = -5,   /* '%' not followed by two HEXDIG              */
    URL_EPORT       = -6,   /* port not DIGIT, or above 65535              */
    URL_EHOST       = -7,   /* empty/oversized label, bad reg-name octet   */
    URL_EIPV6       = -8,   /* malformed IP-literal                        */
    URL_EUTF8       = -9,   /* not a valid RFC 3629 sequence               */
    URL_ENOAUTH     = -10,  /* operation needs an authority (host)         */
    URL_EBADQUERY   = -11,  /* query list full, or a decoded NUL           */
    URL_EARG        = -12   /* argument outside its contract               */
};

/* ------------------------------------------------------------
 *  A parsed, normalised URI reference.
 *
 *  All strings are NUL-terminated and already normalised:
 *    scheme    lowercased (RFC 3986 S6.2.2.3)
 *    host      lowercased, percent-decoded, non-ASCII labels punycoded
 *              ("xn--..."), no enclosing brackets
 *    port      decimal with no leading zeros, or "" when absent or
 *              equal to the scheme's DEFINED default (S6.2.3).  A
 *              scheme with no default (file, custom) keeps an explicit
 *              ":0", because "no default" is not the value 0.
 *    path      percent-normalised (S6.2.2.1/.2) and, after url_parse()
 *              or url_resolve(), dot-segment-free (S5.2.4)
 *    query     without the leading '?'
 *    fragment  without the leading '#'
 *
 *  A component's has_* flag, not its emptiness, says whether it was
 *  present: "http://a/?" has an empty query and "http://a/" has none,
 *  and RFC 3986 S6.2.3 keeps those distinct.
 * ------------------------------------------------------------ */
typedef struct {
    char scheme[URL_MAX_SCHEME];
    char userinfo[URL_MAX_USERINFO];
    char host[URL_MAX_HOST];
    char port[URL_MAX_PORT];
    char path[URL_MAX_PATH];
    char query[URL_MAX_QUERY];
    char fragment[URL_MAX_FRAGMENT];

    unsigned char has_scheme;
    unsigned char has_authority;
    unsigned char has_userinfo;
    unsigned char has_query;
    unsigned char has_fragment;
    unsigned char is_ipv6;        /* host is an IP-literal: re-add [] */
} Url;

/* ------------------------------------------------------------
 *  Parsing
 * ------------------------------------------------------------ */

/* Parse an absolute URI (a scheme is required).  Normalises every
 * component, removes dot segments and elides the default port.
 * Returns URL_OK, or one of the URL_E* codes; on error *out is zeroed,
 * never half-filled. */
int url_parse(Url *out, const char *text);

/* RFC 3986 S5.2: resolve the reference `ref` against the absolute
 * base `base`.  This is the algorithm of S5.2.1-S5.2.4 step for step,
 * including the merge of S5.2.3 and the empty-path query inheritance
 * of S5.2.2, so the S5.4 examples pass unchanged.  On error *out is
 * left untouched. */
int url_resolve(Url *out, const Url *base, const char *ref);

/* ------------------------------------------------------------
 *  Serialising and building
 * ------------------------------------------------------------ */

/* RFC 3986 S5.3 recomposition, verbatim: scheme ":" ["//" authority]
 * path ["?" query] ["#" fragment].  A URL with an authority and an
 * empty path stays "http://host" - S5.3 does not add a slash, and
 * RFC 3986 S5.4.1 expects exactly that.  Use url_request_target()
 * for the HTTP origin-form that does want the "/". */
int url_to_string(const Url *u, char *out, size_t osz);

/* Build an absolute URL from parts, normalising each one.
 * `port` 0 is the sentinel for "no port": the scheme's default applies
 * when it defines one, and nothing is written when it does not.  An
 * explicit ":0" therefore cannot be expressed through url_build (0 is
 * the sentinel, not a value); url_parse() keeps such a port and
 * url_to_string() reproduces it exactly.  `path` must be absolute or
 * empty (URL_EARG otherwise: "h" + "x" would read as "hx"); `query`
 * may be NULL for "no query".  `host` may be a name, a bracketed
 * IP-literal or a bare one; is_ipv6 is derived from the validated
 * form, so a Url's own scheme/host/port rebuild the same URL. */
int url_build(char *out, size_t osz, const char *scheme, const char *host,
              unsigned port, const char *path, const char *query);

/* RFC 6454 S6.2 serialised origin: "scheme://host[:port]", the port
 * only when it is not the scheme default.  Needs scheme + authority. */
int url_origin(const Url *u, char *out, size_t osz);

/* RFC 6454 S5: same scheme, same host, same effective port. */
int url_same_origin(const Url *a, const Url *b);

/* RFC 9112 S3.2.1 origin-form: path with "/" when empty, then
 * "?query" when a query is present.  This is what goes on the
 * request line, never the fragment (RFC 9110 S7.1). */
int url_request_target(const Url *u, char *out, size_t osz);

/* ------------------------------------------------------------
 *  Percent-encoding (RFC 3986 S2.1, S2.2, S2.3, S6.2.2)
 * ------------------------------------------------------------ */

/* Decode %XX triplets.  '+' is NOT a space here: RFC 3986 S3.4 gives
 * it no special meaning, the HTML form parser does (see
 * url_query_parse_form).  `outlen` may be NULL; when non-NULL it
 * receives the number of bytes written and is 0 on error (the result
 * may contain a NUL, so it is a byte count, not a C string). */
int url_pct_decode(const char *in, size_t inlen, char *out, size_t osz,
                   size_t *outlen);

/* Encode everything that is not unreserved (RFC 3986 S2.3) or in the
 * `keep` set (NUL-terminated, may be NULL), using UPPERCASE hex as
 * S6.2.2.1 recommends.  Non-ASCII input must be valid UTF-8; its
 * octets are encoded (S2.5 / RFC 3629). */
int url_pct_encode(const char *in, size_t inlen, const char *keep,
                   char *out, size_t osz, size_t *outlen);

/* The four component encoders, each keeping exactly what its grammar
 * production allows besides pchar:
 *   path      pchar / "/"                      RFC 3986 S3.3
 *   query     pchar / "/" / "?"                RFC 3986 S3.4
 *   fragment  pchar / "/" / "?"                RFC 3986 S3.5
 *   userinfo  unreserved / sub-delims / ":"    RFC 3986 S3.2.1
 * (`pchar` itself is unreserved / sub-delims / ":" / "@".) */
int url_encode_path(const char *in, size_t inlen, char *out, size_t osz,
                    size_t *outlen);
int url_encode_query(const char *in, size_t inlen, char *out, size_t osz,
                     size_t *outlen);
int url_encode_fragment(const char *in, size_t inlen, char *out, size_t osz,
                        size_t *outlen);
int url_encode_userinfo(const char *in, size_t inlen, char *out, size_t osz,
                        size_t *outlen);

/* ------------------------------------------------------------
 *  Hosts: RFC 3986 S3.2.2 + RFC 3492
 * ------------------------------------------------------------ */

/* RFC 3492 S6.3 encoder for one label: code points in, "xn--" body
 * out (no prefix, no dot).  `outlen` receives the byte count. */
int url_punycode_encode(const uint32_t *cp, size_t n, char *out, size_t osz,
                        size_t *outlen);

/* Textual host to an RFC 3986 host: lowercase, percent-decode,
 * punycode non-ASCII labels, validate every label.  An IP-literal
 * may be passed in brackets ("[::1]") or bare ("::1"); the brackets
 * are stripped either way, the literal is validated and its hex
 * digits are lowercased (S6.2.2.3).  A bare literal is recognised by
 * its ':', which no reg-name may contain (S3.2.2); an IPvFuture with
 * no colon is therefore indistinguishable from a reg-name and must
 * be passed in brackets.  Bare is the form stored in Url.host, so
 * url_build(u.scheme, u.host, ...) can rebuild a parsed IPv6 URL.
 * Returns URL_EHOST/URL_EIPV6/URL_EUTF8 rather than a host that
 * cannot be looked up. */
int url_host_to_ascii(const char *host, char *out, size_t osz);

/* ------------------------------------------------------------
 *  Query strings
 *
 *  RFC 3986 S3.4: the query is a sequence of pchar / "/" / "?" and
 *  neither '&' nor '=' has any meaning at that layer.  This module
 *  splits on '&' and then on the FIRST '=' because that is the
 *  convention every caller means; ';' is NOT a separator, because
 *  RFC 3986 does not make it one and treating it as one silently
 *  mangles "a=1;b=2".  Order and duplicates are preserved, and an
 *  empty sequence ("a=1&", "&&") is skipped as the HTML form parser
 *  skips it, so "a=1&" parses and rebuilds to "a=1".  A sequence of
 *  just "=" is not empty: it is the pair ("", "").
 *
 *  url_query_parse()      strict RFC view: '+' stays '+'
 *  url_query_parse_form() HTML form view: '+' becomes a space
 *                         (HTML Standard, application/x-www-form-
 *                         urlencoded parser, step 1)
 * ------------------------------------------------------------ */
typedef struct {
    char name[URL_MAX_NAME];
    char value[URL_MAX_VALUE];
} UrlParam;

typedef struct {
    UrlParam item[URL_MAX_PARAMS];
    int count;
} UrlQuery;

/* `query` is the substring after '?', or "" for an empty query.
 * Percent-escapes are decoded in both names and values. */
int url_query_parse(UrlQuery *q, const char *query);
int url_query_parse_form(UrlQuery *q, const char *query);

/* First value for `name`, or NULL.  url_query_count() counts every
 * occurrence, so a caller can see "a=1&a=2" is two, not one. */
const char *url_query_get(const UrlQuery *q, const char *name);
int url_query_count(const UrlQuery *q, const char *name);

/* Append one pair (encoded on output by url_query_build). */
int url_query_add(UrlQuery *q, const char *name, const char *value);

/* "a=1&b=hello%20world" - names and values encoded to unreserved.
 * A value can be URL_MAX_VALUE-1 octets, so the encoded form needs
 * up to three times that; a short `osz` returns URL_ETOOLONG and
 * leaves "" behind like every other writer here. */
int url_query_build(const UrlQuery *q, char *out, size_t osz);

/* ------------------------------------------------------------
 *  Diagnostics
 * ------------------------------------------------------------ */
const char *url_strerror(int err);

/* True when `scheme` is one of the URL Standard's special schemes
 * (ftp, file, http, https, ws, wss).  Parsing does NOT use this to
 * reinterpret a backslash - that is rejected (see the note at the
 * top) - but callers that need the URL Standard's classification
 * (default-port policy, proxy rules) can ask for it here.
 * `scheme` must already be lowercase. */
int url_scheme_is_special(const char *scheme);

/* The default port for a scheme, or 0 when it has none.
 * http/ws 80, https/wss 443 (RFC 9110 S4.2.2/4.2.4, RFC 6455 S3),
 * ftp 21 (RFC 1738 S3.2). */
unsigned url_default_port(const char *scheme);

#endif /* MYSTIC_NET_URL_H */
