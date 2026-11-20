/* ============================================================
 *  Mystic OS - net/url.c
 *  RFC 3986 URI parsing, normalisation and reference resolution,
 *  plus RFC 3492 Punycode for host labels.
 *
 *  The file is one pass per job, in this order:
 *
 *     1. octet classes              RFC 3986 S2.2, S2.3
 *     2. UTF-8                      RFC 3629 S4
 *     3. Punycode                   RFC 3492 S5, S6.3
 *     4. percent-encoding           RFC 3986 S2.1, S6.2.2
 *     5. hosts: reg-name/IP-literal RFC 3986 S3.2.2
 *     6. the Appendix B splitter    RFC 3986 S3, Appendix B
 *     7. remove_dot_segments        RFC 3986 S5.2.4
 *     8. parse, resolve, serialise  RFC 3986 S5.2, S5.3
 *     9. query strings
 *
 *  No allocation, no globals, no errno.  See url.h for the contract.
 * ============================================================ */
#include "url.h"

#include <string.h>

/* ------------------------------------------------------------
 *  A bounded output cursor.
 *
 *  Every writer in this file goes through one, so no path can write
 *  past its buffer and no path can return a string that is not
 *  NUL-terminated.  The first overflow sticks: URL_ETOOLONG is
 *  reported even if later characters would have fitted, because a
 *  half-written URL is not a URL.
 * ------------------------------------------------------------ */
typedef struct {
    char  *p;
    size_t cap;
    size_t n;
    int    err;
} Out;

static void out_init(Out *o, char *p, size_t cap) {
    o->p = p; o->cap = cap; o->n = 0;
    o->err = (cap == 0) ? URL_ETOOLONG : URL_OK;
}

static void out_ch(Out *o, char c) {
    if (o->err) return;
    if (o->n + 1 >= o->cap) { o->err = URL_ETOOLONG; return; }
    o->p[o->n++] = c;
}

static void out_mem(Out *o, const char *s, size_t n) {
    if (o->err) return;
    if (o->n + n + 1 > o->cap) { o->err = URL_ETOOLONG; return; }
    if (n) memcpy(o->p + o->n, s, n);
    o->n += n;
}

static void out_str(Out *o, const char *s) {
    out_mem(o, s, strlen(s));
}

/* NUL-terminate and report.  On error the buffer is left as "" so a
 * caller that ignores the return value cannot read stale bytes. */
static int out_end(Out *o) {
    if (o->cap == 0) return URL_ETOOLONG;
    if (o->err) { o->p[0] = 0; return o->err; }
    o->p[o->n] = 0;
    return URL_OK;
}

/* Same guarantee for a writer that gives up in the middle: the buffer
 * is "" and no partial component is left behind. */
static int out_fail(Out *o, int err) {
    if (o->cap) o->p[0] = 0;
    return err;
}

static int field_set(char *dst, size_t cap, const char *src, size_t n) {
    if (n + 1 > cap) return URL_ETOOLONG;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
    return URL_OK;
}

/* ------------------------------------------------------------
 *  1. Octet classes - RFC 3986 S2.2 (reserved) and S2.3 (unreserved).
 * ------------------------------------------------------------ */
#define HEXDIG_UPPER "0123456789ABCDEF"   /* S6.2.2.1: uppercase on output */

static int ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int ascii_hex(unsigned char c) {
    return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"   (S2.3) */
static int is_unreserved(unsigned char c) {
    return ascii_alpha(c) || ascii_digit(c) ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

/* sub-delims = "!" / "$" / "&" / "'" / "(" / ")" /
 *              "*" / "+" / "," / ";" / "="            (S2.2) */
static int is_subdelim(unsigned char c) {
    switch (c) {
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=':
        return 1;
    default:
        return 0;
    }
}

/* The grammar productions this file has to reproduce, as bit masks.
 * '%' is never "allowed raw": a '%' always introduces a triplet and
 * is validated by norm_component(). */
#define CM_UNRESERVED 0x0001u
#define CM_SUBDELIM   0x0002u
#define CM_COLON      0x0004u
#define CM_AT         0x0008u
#define CM_SLASH      0x0010u
#define CM_QUESTION   0x0020u

static int class_ok(unsigned char c, unsigned mask) {
    if ((mask & CM_UNRESERVED) && is_unreserved(c)) return 1;
    if ((mask & CM_SUBDELIM)   && is_subdelim(c))   return 1;
    if ((mask & CM_COLON)      && c == ':')         return 1;
    if ((mask & CM_AT)         && c == '@')         return 1;
    if ((mask & CM_SLASH)      && c == '/')         return 1;
    if ((mask & CM_QUESTION)   && c == '?')         return 1;
    return 0;
}

/* pchar = unreserved / pct-encoded / sub-delims / ":" / "@"  (S3.3) */
#define MASK_PATH   (CM_UNRESERVED | CM_SUBDELIM | CM_COLON | CM_AT | CM_SLASH)
#define MASK_QUERY  (MASK_PATH | CM_QUESTION)
#define MASK_USER   (CM_UNRESERVED | CM_SUBDELIM | CM_COLON)

/* ------------------------------------------------------------
 *  2. UTF-8 - RFC 3629 S4.  Rejects everything S4 calls invalid:
 *  a truncated sequence, a stray continuation, an overlong form, an
 *  encoded surrogate, and anything above U+10FFFF.
 * ------------------------------------------------------------ */
static int utf8_next(const unsigned char *s, size_t n, size_t *i, uint32_t *cp) {
    unsigned char c = s[*i];
    uint32_t v;
    size_t need, k;

    if (c < 0x80) { *cp = c; (*i)++; return 1; }
    if (c >= 0xC2 && c <= 0xDF)      { need = 1; v = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; v = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; v = c & 0x07u; }
    else return 0;                     /* continuation byte, C0, C1, F5..FF */
    if (*i + need >= n) return 0;      /* not enough continuation bytes    */
    for (k = 1; k <= need; k++) {
        unsigned char cc = s[*i + k];
        if ((cc & 0xC0u) != 0x80u) return 0;
        v = (v << 6) | (uint32_t)(cc & 0x3Fu);
    }
    if (need == 2 && v < 0x800u)     return 0;   /* overlong */
    if (need == 3 && v < 0x10000u)   return 0;   /* overlong */
    if (v > 0x10FFFFu)               return 0;
    if (v >= 0xD800u && v <= 0xDFFFu) return 0;  /* surrogate */
    *cp = v;
    *i += need + 1;
    return 1;
}

/* ------------------------------------------------------------
 *  3. Punycode - RFC 3492.
 *
 *  S5 parameters, exactly:
 *     base 36, tmin 1, tmax 26, skew 38, damp 700,
 *     initial_bias 72, initial_n 0x80, delimiter 0x2D,
 *     and the sample implementation's overflow ceiling 2^31-1.
 *  S6.3 is transcribed step for step, so the S7.1 sample strings in
 *  the harness come out byte-identical.
 * ------------------------------------------------------------ */
#define PUNY_BASE           36
#define PUNY_TMIN            1
#define PUNY_TMAX           26
#define PUNY_SKEW           38
#define PUNY_DAMP          700
#define PUNY_INITIAL_BIAS   72
#define PUNY_INITIAL_N    0x80u
#define PUNY_DELIM        0x2Du
#define PUNY_MAXINT 0x7FFFFFFFu

/* RFC 3492 S3.4: digit 0..25 is 'a'..'z', 26..35 is '0'..'9'.
 * A-label output is lowercase, so only the lowercase half is needed. */
static char puny_digit(uint32_t d) {
    return (d < 26u) ? (char)('a' + d) : (char)('0' + (d - 26u));
}

/* RFC 3492 S6.1 bias adaptation. */
static uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, int first) {
    uint32_t k = 0;

    delta = first ? delta / PUNY_DAMP : delta / 2u;
    delta += delta / numpoints;
    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2u) {
        delta /= PUNY_BASE - PUNY_TMIN;
        k += PUNY_BASE;
    }
    return k + (((PUNY_BASE - PUNY_TMIN + 1u) * delta) / (delta + PUNY_SKEW));
}

int url_punycode_encode(const uint32_t *cp, size_t n, char *out, size_t osz,
                        size_t *outlen) {
    Out o;
    uint32_t nval = PUNY_INITIAL_N;
    uint32_t delta = 0, bias = PUNY_INITIAL_BIAS;
    size_t i, b = 0, h;
    int rc;

    if (outlen) *outlen = 0;
    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;   /* cannot hold even "" */
    out[0] = 0;
    if (!cp) return URL_EINVAL;
    out_init(&o, out, osz);

    for (i = 0; i < n; i++)
        if (cp[i] < 0x80u) b++;

    h = b;
    if (b > 0) {
        for (i = 0; i < n; i++)
            if (cp[i] < 0x80u) out_ch(&o, (char)cp[i]);
        out_ch(&o, (char)PUNY_DELIM);
    }

    while (h < n) {
        uint32_t m = 0xFFFFFFFFu;

        for (i = 0; i < n; i++)
            if (cp[i] >= nval && cp[i] < m) m = cp[i];
        if (m == 0xFFFFFFFFu) { o.err = URL_EHOST; goto done; }

        /* delta += (m - nval) * (h + 1), the S6.3 overflow check */
        if ((m - nval) > (PUNY_MAXINT - delta) / (uint32_t)(h + 1)) {
            o.err = URL_EHOST;
            goto done;
        }
        delta += (m - nval) * (uint32_t)(h + 1);
        nval = m;

        for (i = 0; i < n; i++) {
            if (cp[i] < nval) {
                if (delta == PUNY_MAXINT) { o.err = URL_EHOST; goto done; }
                delta++;
            }
            if (cp[i] == nval) {
                uint32_t q = delta;
                uint32_t k;

                for (k = PUNY_BASE; ; k += PUNY_BASE) {
                    uint32_t t;
                    if (k <= bias + PUNY_TMIN)      t = PUNY_TMIN;
                    else if (k >= bias + PUNY_TMAX) t = PUNY_TMAX;
                    else                             t = k - bias;
                    if (q < t) break;
                    out_ch(&o, puny_digit(t + (q - t) % (PUNY_BASE - t)));
                    q = (q - t) / (PUNY_BASE - t);
                }
                out_ch(&o, puny_digit(q));
                bias = puny_adapt(delta, (uint32_t)(h + 1), h == b);
                delta = 0;
                h++;
            }
        }
        if (delta == PUNY_MAXINT) { o.err = URL_EHOST; goto done; }
        delta++;
        if (nval == 0xFFFFFFFFu) { o.err = URL_EHOST; goto done; }
        nval++;
    }

done:
    rc = out_end(&o);
    if (outlen) *outlen = (rc == URL_OK) ? o.n : 0;
    return rc;
}

/* ------------------------------------------------------------
 *  4. Percent-encoding - RFC 3986 S2.1, S6.2.2.
 * ------------------------------------------------------------ */
static void out_pct(Out *o, unsigned char c) {
    out_ch(o, '%');
    out_ch(o, HEXDIG_UPPER[c >> 4]);
    out_ch(o, HEXDIG_UPPER[c & 15u]);
}

int url_pct_encode(const char *in, size_t inlen, const char *keep,
                   char *out, size_t osz, size_t *outlen) {
    Out o;
    size_t i = 0;
    int rc;

    if (outlen) *outlen = 0;
    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;   /* cannot hold even "" */
    out[0] = 0;
    if (!in && inlen) return URL_EINVAL;
    out_init(&o, out, osz);

    while (i < inlen) {
        unsigned char c = (unsigned char)in[i];

        if (c >= 0x80) {
            /* S2.5: a URI carries non-ASCII as UTF-8 octets, each
             * percent-encoded.  Refuse bytes that are not UTF-8. */
            size_t k = i;
            uint32_t cp;
            if (!utf8_next((const unsigned char *)in, inlen, &k, &cp)) {
                o.err = URL_EUTF8;
                break;
            }
            while (i < k) { out_pct(&o, (unsigned char)in[i]); i++; }
            continue;
        }
        if (c == '%' && !(keep && strchr(keep, '%'))) {
            /* '%' is always a triplet introducer in this layer;
             * url_pct_encode() escapes it so the output re-parses. */
            out_pct(&o, c);
            i++;
            continue;
        }
        if (is_unreserved(c) || (keep && c != 0 && strchr(keep, c))) {
            out_ch(&o, (char)c);
            i++;
            continue;
        }
        out_pct(&o, c);
        i++;
    }

    rc = out_end(&o);
    if (outlen) *outlen = (rc == URL_OK) ? o.n : 0;
    return rc;
}

static int encode_with_mask(const char *in, size_t inlen, unsigned mask,
                            char *out, size_t osz, size_t *outlen) {
    Out o;
    size_t i = 0;
    int rc;

    if (outlen) *outlen = 0;
    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;   /* cannot hold even "" */
    out[0] = 0;
    if (!in && inlen) return URL_EINVAL;
    out_init(&o, out, osz);
    while (i < inlen) {
        unsigned char c = (unsigned char)in[i];

        if (c >= 0x80) {
            size_t k = i;
            uint32_t cp;
            if (!utf8_next((const unsigned char *)in, inlen, &k, &cp)) {
                o.err = URL_EUTF8;
                break;
            }
            while (i < k) { out_pct(&o, (unsigned char)in[i]); i++; }
            continue;
        }
        if (class_ok(c, mask)) out_ch(&o, (char)c);
        else                   out_pct(&o, c);
        i++;
    }
    rc = out_end(&o);
    if (outlen) *outlen = (rc == URL_OK) ? o.n : 0;
    return rc;
}

int url_encode_path(const char *in, size_t inlen, char *out, size_t osz,
                    size_t *outlen) {
    return encode_with_mask(in, inlen, MASK_PATH, out, osz, outlen);
}
int url_encode_query(const char *in, size_t inlen, char *out, size_t osz,
                     size_t *outlen) {
    return encode_with_mask(in, inlen, MASK_QUERY, out, osz, outlen);
}
int url_encode_fragment(const char *in, size_t inlen, char *out, size_t osz,
                        size_t *outlen) {
    return encode_with_mask(in, inlen, MASK_QUERY, out, osz, outlen);
}
int url_encode_userinfo(const char *in, size_t inlen, char *out, size_t osz,
                        size_t *outlen) {
    return encode_with_mask(in, inlen, MASK_USER, out, osz, outlen);
}

int url_pct_decode(const char *in, size_t inlen, char *out, size_t osz,
                   size_t *outlen) {
    Out o;
    size_t i = 0;
    int rc;

    if (outlen) *outlen = 0;
    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;   /* cannot hold even "" */
    out[0] = 0;
    if (!in && inlen) return URL_EINVAL;
    out_init(&o, out, osz);
    while (i < inlen) {
        unsigned char c = (unsigned char)in[i];

        if (c == '%') {
            if (i + 2 >= inlen || !ascii_hex((unsigned char)in[i + 1]) ||
                !ascii_hex((unsigned char)in[i + 2])) {
                o.err = URL_EBADPCT;
                break;
            }
            out_ch(&o, (char)((hex_val((unsigned char)in[i + 1]) << 4) |
                              hex_val((unsigned char)in[i + 2])));
            i += 3;
            continue;
        }
        out_ch(&o, (char)c);
        i++;
    }
    rc = out_end(&o);
    if (outlen) *outlen = (rc == URL_OK) ? o.n : 0;
    return rc;
}

/* Normalise one component in place-free style (RFC 3986 S6.2.2):
 *
 *   - "%XX" whose octet is unreserved becomes that character
 *     (S6.2.2.2, "decoding percent-encoded octets of unreserved
 *     characters is safe");
 *   - every other "%XX" is rewritten with UPPERCASE hex (S6.2.2.1);
 *   - a raw octet the component grammar allows is copied;
 *   - a raw octet it does not allow is percent-encoded, which is the
 *     only way to keep the result a URI;
 *   - raw non-ASCII must be valid UTF-8 (RFC 3629) and is encoded.
 *
 * A backslash is not in any RFC 3986 production, so it takes the
 * percent-encoding path here ("\\" becomes %5C).  ref_parse() never
 * lets a raw one reach this function: it is an error there.
 */
static int norm_component(const char *in, size_t len, unsigned mask,
                          char *out, size_t osz) {
    Out o;
    size_t i = 0;

    if ((!in && len) || !out) return URL_EINVAL;
    out_init(&o, out, osz);
    while (i < len) {
        unsigned char c = (unsigned char)in[i];

        if (c == '%') {
            unsigned v;
            if (i + 2 >= len || !ascii_hex((unsigned char)in[i + 1]) ||
                !ascii_hex((unsigned char)in[i + 2]))
                return out_fail(&o, URL_EBADPCT);
            v = (unsigned)((hex_val((unsigned char)in[i + 1]) << 4) |
                           hex_val((unsigned char)in[i + 2]));
            if (is_unreserved((unsigned char)v)) out_ch(&o, (char)v);
            else                                out_pct(&o, (unsigned char)v);
            i += 3;
            continue;
        }
        if (c >= 0x80) {
            size_t k = i;
            uint32_t cp;
            if (!utf8_next((const unsigned char *)in, len, &k, &cp))
                return out_fail(&o, URL_EUTF8);
            while (i < k) { out_pct(&o, (unsigned char)in[i]); i++; }
            continue;
        }
        if (class_ok(c, mask)) out_ch(&o, (char)c);
        else                   out_pct(&o, c);
        i++;
    }
    return out_end(&o);
}

/* ------------------------------------------------------------
 *  5. Hosts - RFC 3986 S3.2.2.
 * ------------------------------------------------------------ */

/* dec-octet = DIGIT / %x31-39 DIGIT / "1" 2DIGIT /
 *             "2" %x30-34 DIGIT / "25" %x30-35
 * i.e. 0..255, and no leading zero unless the octet is "0" itself. */
static int ipv4_octets(const char *s, size_t n, int *count) {
    size_t i = 0;
    int groups = 0;

    if (n == 0) return 0;
    while (i < n) {
        size_t start = i;
        unsigned v = 0;

        while (i < n && s[i] != '.') i++;
        if (i - start == 0 || i - start > 3) return 0;
        if (i - start > 1 && s[start] == '0') return 0;   /* leading zero */
        for (size_t k = start; k < i; k++) {
            if (!ascii_digit((unsigned char)s[k])) return 0;
            v = v * 10u + (unsigned)(s[k] - '0');
        }
        if (v > 255u) return 0;
        groups++;
        if (i < n) {
            i++;                                   /* the '.' */
            if (i == n) return 0;                  /* trailing dot */
        }
    }
    if (count) *count = groups;
    return 1;
}

/* One run of ':'-separated elements, no "::" allowed inside.
 * Returns the number of 16-bit groups, or -1 on a syntax error.
 * A dotted quad in the last position counts as two groups (RFC 3986
 * S3.2.2: ls32 = ( h16 ":" h16 ) / IPv4address ). */
static int ipv6_run(const char *s, size_t n, int allow_v4) {
    size_t i = 0;
    int groups = 0;

    if (n == 0) return 0;
    while (i < n) {
        size_t start = i;

        while (i < n && s[i] != ':') i++;
        if (i == start) return -1;                 /* empty element */
        if (allow_v4 && memchr(s + start, '.', i - start)) {
            int quad = 0;
            if (i != n) return -1;                 /* IPv4 must come last */
            if (!ipv4_octets(s + start, i - start, &quad)) return -1;
            if (quad != 4) return -1;              /* IPv4address = 4 octets */
            groups += 2;
        } else {
            if (i - start > 4) return -1;
            for (size_t k = start; k < i; k++)
                if (!ascii_hex((unsigned char)s[k])) return -1;
            groups++;
        }
        if (i < n) {
            i++;                                   /* the ':' */
            if (i == n) return -1;                 /* trailing single ':' */
        }
    }
    return groups;
}

/* IPv6address / IPvFuture, without the brackets.  RFC 3986 S3.2.2.
 * The "::" form may stand for one or more zero groups, so the two
 * runs must total fewer than eight; without "::" they must total
 * exactly eight (RFC 4291 S2.2). */
static int ip_literal_valid(const char *s, size_t n) {
    size_t dc = 0;
    int have_dc = 0, left, right;

    /* IPvFuture = "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" ) */
    if (n > 0 && (s[0] == 'v' || s[0] == 'V')) {
        size_t i = 1, digits = 0, k = 0;
        while (i < n && ascii_hex((unsigned char)s[i])) { i++; digits++; }
        if (digits == 0 || i >= n || s[i] != '.') return 0;
        i++;
        for (k = i; k < n; k++) {
            unsigned char c = (unsigned char)s[k];
            if (!(is_unreserved(c) || is_subdelim(c) || c == ':')) return 0;
        }
        return k > i;
    }

    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == ':' && s[i + 1] == ':') {
            if (have_dc) return 0;                /* "::" appears twice */
            have_dc = 1;
            dc = i;
        }
    }
    if (!have_dc) return ipv6_run(s, n, 1) == 8;

    left  = ipv6_run(s, dc, 0);
    right = ipv6_run(s + dc + 2, n - dc - 2, 1);
    if (left < 0 || right < 0) return 0;
    return left + right < 8;
}

/* Percent-decode a reg-name and turn it into its ASCII form:
 * lowercase, labels split on '.', non-ASCII labels punycoded with
 * RFC 3492.  Length limits are RFC 1035 S2.3.4 (63 per label, 255
 * octets of name in wire form, so 253 characters in presentation).
 *
 * A single trailing dot is the root label (RFC 1035 S3.1) and is
 * dropped; the caller that needs an FQDN adds it back.  Interior
 * empty labels are an error: "a..b" cannot be resolved. */
static int host_reg_name(const char *in, size_t n, char *out, size_t osz) {
    char dec[URL_MAX_HOST];
    size_t dn = 0, i, start, labels = 0;
    Out o;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (n >= sizeof dec) return URL_ETOOLONG;
    for (i = 0; i < n; i++) {
        if (in[i] == '%') {
            if (i + 2 >= n || !ascii_hex((unsigned char)in[i + 1]) ||
                !ascii_hex((unsigned char)in[i + 2]))
                return URL_EBADPCT;
            dec[dn++] = (char)((hex_val((unsigned char)in[i + 1]) << 4) |
                               hex_val((unsigned char)in[i + 2]));
            i += 2;
        } else {
            dec[dn++] = in[i];
        }
    }
    if (dn == 0) return URL_EHOST;
    if (dn > 1 && dec[dn - 1] == '.') dn--;       /* root label */

    out_init(&o, out, osz);
    start = 0;
    while (start < dn) {
        size_t end = start, len;
        int nonascii = 0;

        while (end < dn && dec[end] != '.') end++;
        len = end - start;
        if (len == 0) return out_fail(&o, URL_EHOST);   /* "a..b" */

        for (size_t k = start; k < end; k++) {
            unsigned char c = (unsigned char)dec[k];
            if (c >= 0x80) { nonascii = 1; continue; }
            /* reg-name = *( unreserved / pct-encoded / sub-delims ) */
            if (!(is_unreserved(c) || is_subdelim(c)))
                return out_fail(&o, URL_EHOST);
        }

        if (labels++) out_ch(&o, '.');
        if (nonascii) {
            uint32_t cp[URL_MAX_HOST];
            size_t nc = 0, k = start;
            char enc[URL_MAX_HOST];
            size_t en = 0;
            int rc;

            while (k < end) {
                if (nc >= sizeof cp / sizeof cp[0])
                    return out_fail(&o, URL_EHOST);
                if (!utf8_next((const unsigned char *)dec, end, &k, &cp[nc]))
                    return out_fail(&o, URL_EUTF8);
                /* Punycode copies the basic code points verbatim, so
                 * lowercase them first (S6.2.2.3 + lowercase A-label). */
                if (cp[nc] >= 'A' && cp[nc] <= 'Z') cp[nc] += 32u;
                nc++;
            }
            rc = url_punycode_encode(cp, nc, enc, sizeof enc, &en);
            if (rc != URL_OK) return out_fail(&o, rc);
            /* RFC 5890 S2.3.4 / RFC 1035 S2.3.4: an A-label is 63 max */
            if (4 + en > 63) return out_fail(&o, URL_EHOST);
            out_mem(&o, "xn--", 4);
            out_mem(&o, enc, en);
        } else {
            if (len > 63) return out_fail(&o, URL_EHOST);  /* RFC 1035 */
            for (size_t k = start; k < end; k++) {
                unsigned char c = (unsigned char)dec[k];
                out_ch(&o, (char)((c >= 'A' && c <= 'Z') ? c + 32u : c));
            }
        }
        start = end + 1;
    }

    {
        int rc = out_end(&o);
        if (rc != URL_OK) return rc;
    }
    if (strlen(out) > 253) return out_fail(&o, URL_EHOST);
    return URL_OK;
}

/* Validate one IP-literal - the text between the brackets, or a bare
 * literal - and store it without brackets, hex digits lowercased
 * (RFC 3986 S6.2.2.3).  The caller has already made out[0] = 0, so an
 * error leaves a legal empty string. */
static int ip_literal_store(const char *s, size_t n, char *out, size_t osz) {
    if (n == 0) return URL_EIPV6;
    if (n + 1 > osz) return URL_ETOOLONG;
    if (!ip_literal_valid(s, n)) return URL_EIPV6;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32u : c);
    }
    out[n] = 0;
    return URL_OK;
}

int url_host_to_ascii(const char *host, char *out, size_t osz) {
    size_t n;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!host) return URL_EINVAL;
    n = strlen(host);
    if (n == 0) return URL_EHOST;

    if (host[0] == '[') {
        size_t close = 0;

        while (close < n && host[close] != ']') close++;
        if (close + 1 != n || close < 2) return URL_EIPV6;
        return ip_literal_store(host + 1, close - 1, out, osz);
    }
    /* RFC 3986 S3.2.2: a reg-name cannot contain ':', so a bare
     * literal is recognisable by its colon.  This is the form a
     * parsed Url stores in Url.host, and accepting it is what lets
     * url_build() rebuild a URL from its own components.  An
     * IPvFuture without a colon is indistinguishable from a reg-name
     * and must be passed in brackets. */
    if (memchr(host, ':', n) != NULL)
        return ip_literal_store(host, n, out, osz);
    return host_reg_name(host, n, out, osz);
}

/* ------------------------------------------------------------
 *  6. Splitting a reference - RFC 3986 Appendix B, made strict.
 *
 *     ^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?
 *
 *  Instead of the regex's greediness we scan once, left to right,
 *  and keep offsets into the caller's string.  Nothing is copied,
 *  so this cannot overflow.  There is no second separator: unlike
 *  the URL Standard, a backslash is not a slash here (RFC 3986 has
 *  no backslash in any production, and ref_parse() refuses it).
 * ------------------------------------------------------------ */
typedef struct {
    size_t scheme_at, scheme_n;   int has_scheme;
    size_t auth_at,   auth_n;     int has_authority;
    size_t path_at,   path_n;
    size_t query_at,  query_n;    int has_query;
    size_t frag_at,   frag_n;     int has_fragment;
} Parts;

/* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )   RFC 3986 S3.1 */
static int scheme_syntax_ok(const char *s, size_t n) {
    if (n == 0 || !ascii_alpha((unsigned char)s[0])) return 0;
    for (size_t i = 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(ascii_alpha(c) || ascii_digit(c) ||
              c == '+' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static void split_reference(const char *s, size_t n, Parts *p) {
    size_t i = 0, c = 0;

    memset(p, 0, sizeof *p);

    /* Appendix B requires one or more characters before the ':';
     * "http://a/b/c" + ":x" is a relative path, not a scheme. */
    while (c < n && s[c] != ':' && s[c] != '/' && s[c] != '?' && s[c] != '#')
        c++;
    if (c > 0 && c < n && s[c] == ':') {
        p->has_scheme = 1;
        p->scheme_at = 0;
        p->scheme_n = c;
        i = c + 1;
    }

    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        p->has_authority = 1;
        i += 2;
        p->auth_at = i;
        while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#')
            i++;
        p->auth_n = i - p->auth_at;
    }

    p->path_at = i;
    while (i < n && s[i] != '?' && s[i] != '#') i++;
    p->path_n = i - p->path_at;

    if (i < n && s[i] == '?') {
        p->has_query = 1;
        i++;
        p->query_at = i;
        while (i < n && s[i] != '#') i++;
        p->query_n = i - p->query_at;
    }
    if (i < n && s[i] == '#') {
        p->has_fragment = 1;
        i++;
        p->frag_at = i;
        p->frag_n = n - i;
    }
}

/* ------------------------------------------------------------
 *  7. remove_dot_segments - RFC 3986 S5.2.4, step A..E.
 *
 *  The output never grows past the input, so a buffer the size of the
 *  input is always enough.  "Above the root" ("/../x") is clamped to
 *  the root, which is what the algorithm's step C does by dropping
 *  the last output segment from an empty output.
 * ------------------------------------------------------------ */
static void out_pop_segment(char *buf, size_t *o) {
    while (*o > 0 && buf[*o - 1] != '/') (*o)--;
    if (*o > 0) (*o)--;
}

static int remove_dot_segments(const char *in, char *out, size_t osz) {
    size_t n, i = 0, o = 0;

    if (!in || !out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    n = strlen(in);

    while (i < n) {
        size_t rem = n - i;

        if (rem >= 3 && in[i] == '.' && in[i + 1] == '.' && in[i + 2] == '/') {
            i += 3;                                        /* A: "../"  */
        } else if (rem >= 2 && in[i] == '.' && in[i + 1] == '/') {
            i += 2;                                        /* A: "./"   */
        } else if (rem >= 3 && in[i] == '/' && in[i + 1] == '.' &&
                   in[i + 2] == '/') {
            i += 2;                                        /* B: "/./"  */
        } else if (rem == 2 && in[i] == '/' && in[i + 1] == '.') {
            /* B: "/." becomes "/", and that slash belongs to the
             * output: "g/." is "g/", not "g". */
            i += 2;
            if (o + 1 + 1 > osz) return URL_ETOOLONG;
            out[o++] = '/';
        } else if (rem >= 4 && in[i] == '/' && in[i + 1] == '.' &&
                   in[i + 2] == '.' && in[i + 3] == '/') {
            i += 3;                                        /* C: "/../" */
            out_pop_segment(out, &o);
        } else if (rem == 3 && in[i] == '/' && in[i + 1] == '.' &&
                   in[i + 2] == '.') {
            /* C: "/.." becomes "/" after the last segment is dropped. */
            i += 2;
            out_pop_segment(out, &o);
            if (o + 1 + 1 > osz) return URL_ETOOLONG;
            out[o++] = '/';
        } else if (rem == 1 && in[i] == '.') {
            i += 1;                                        /* D: "."    */
        } else if (rem == 2 && in[i] == '.' && in[i + 1] == '.') {
            i += 2;                                        /* D: ".."   */
        } else {
            size_t start = i, len;
            if (in[i] == '/') i++;                         /* E */
            while (i < n && in[i] != '/') i++;
            len = i - start;
            if (o + len + 1 > osz) return URL_ETOOLONG;
            memcpy(out + o, in + start, len);
            o += len;
        }
    }
    out[o] = 0;
    return URL_OK;
}

/* ------------------------------------------------------------
 *  8. Schemes and their ports
 * ------------------------------------------------------------ */
int url_scheme_is_special(const char *scheme) {
    return scheme && (strcmp(scheme, "ftp") == 0 ||
                      strcmp(scheme, "file") == 0 ||
                      strcmp(scheme, "http") == 0 ||
                      strcmp(scheme, "https") == 0 ||
                      strcmp(scheme, "ws") == 0 ||
                      strcmp(scheme, "wss") == 0);
}

unsigned url_default_port(const char *scheme) {
    if (!scheme) return 0;
    if (strcmp(scheme, "http") == 0)  return 80;   /* RFC 9110 S4.2.2 */
    if (strcmp(scheme, "https") == 0) return 443;  /* RFC 9110 S4.2.4 */
    if (strcmp(scheme, "ws") == 0)    return 80;   /* RFC 6455 S3     */
    if (strcmp(scheme, "wss") == 0)   return 443;  /* RFC 6455 S3     */
    if (strcmp(scheme, "ftp") == 0)   return 21;   /* RFC 1738 S3.2   */
    return 0;                                      /* file: and others */
}

/* ------------------------------------------------------------
 *  Authority: [ userinfo "@" ] host [ ":" port ]   RFC 3986 S3.2
 * ------------------------------------------------------------ */
static int parse_port(const char *s, size_t n, const char *scheme,
                      char *out, size_t osz) {
    unsigned v = 0;
    unsigned d;
    char tmp[URL_MAX_PORT];
    size_t k = 0;

    if (!out) return URL_EINVAL;
    out[0] = 0;
    if (n == 0) return URL_OK;             /* S3.2.3: port = *DIGIT, empty ok */
    /* S3.2.3 is "port = *DIGIT" and S6.2.3 makes leading zeros
     * insignificant (they are dropped from the stored value), so
     * ":0000000000080" is port 80, not an error.  Only the numeric
     * value has to fit the 16-bit transport bound. */
    while (n > 1 && s[0] == '0') { s++; n--; }
    if (n > 5) return URL_EPORT;           /* 65535 has five digits */

    for (size_t i = 0; i < n; i++) {
        if (!ascii_digit((unsigned char)s[i])) return URL_EPORT;
        v = v * 10u + (unsigned)(s[i] - '0');
        /* The 16-bit bound is the transport's: a TCP port is a
         * 16-bit quantity (RFC 9293 S3.1), so 65535 is the ceiling. */
        if (v > 65535u) return URL_EPORT;
    }
    /* S6.2.3: "the default port ... may be elided".  Only a scheme
     * with a DEFINED default qualifies: url_default_port() returns 0
     * for "no default", and that is not the port value 0, so
     * "custom://h:0" and "file://h:0" keep their port.  A reference
     * that has no scheme yet passes scheme "" and is elided later by
     * elide_default_port(), once the base's scheme is known. */
    d = url_default_port(scheme);
    if (d != 0 && v == d) return URL_OK;
    do {
        tmp[k++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && k < sizeof tmp);
    if (v != 0) return URL_ETOOLONG;
    if (k + 1 > osz) return URL_ETOOLONG;
    for (size_t i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    out[k] = 0;
    return URL_OK;
}

/* `a` is the authority with no "//", `n` its length. */
static int parse_authority(const char *a, size_t n, const char *scheme, Url *u) {
    size_t at = n, colon = n, i;
    int rc;

    for (i = 0; i < n; i++)
        if (a[i] == '@') at = i;          /* the LAST '@' delimits S3.2.1 */

    if (at != n) {
        u->has_userinfo = 1;
        rc = norm_component(a, at, MASK_USER,
                            u->userinfo, sizeof u->userinfo);
        if (rc != URL_OK) return rc;
        a += at + 1;
        n -= at + 1;
    }

    if (n > 0 && a[0] == '[') {                       /* IP-literal S3.2.2 */
        size_t close = 1;

        while (close < n && a[close] != ']') close++;
        if (close == n) return URL_EIPV6;
        if (!ip_literal_valid(a + 1, close - 1)) return URL_EIPV6;
        u->is_ipv6 = 1;
        if (close - 1 + 1 > sizeof u->host) return URL_ETOOLONG;
        for (i = 0; i < close - 1; i++) {
            unsigned char c = (unsigned char)a[i + 1];
            /* S6.2.2.3: hex digits in a host are case-insensitive and
             * "should be normalized to lowercase". */
            u->host[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32u : c);
        }
        u->host[close - 1] = 0;
        if (close + 1 < n) {
            if (a[close + 1] != ':') return URL_EHOST;
            return parse_port(a + close + 2, n - close - 2, scheme,
                              u->port, sizeof u->port);
        }
        if (close + 1 != n) return URL_EHOST;
        u->port[0] = 0;
        return URL_OK;
    }

    if (n == 0) {
        /* RFC 8089 S2: file: allows an empty host (it means the local
         * machine).  Every other scheme needs a name to connect to. */
        if (strcmp(scheme, "file") != 0) return URL_EHOST;
        u->host[0] = 0;
        u->port[0] = 0;
        return URL_OK;
    }

    colon = n;          /* n just shrank past the userinfo, so reset this */
    for (i = 0; i < n; i++)
        if (a[i] == ':') { colon = i; break; }   /* ':' cannot be in a reg-name */

    if (colon == 0) return URL_EHOST;                /* ":80" with no host */
    if (colon == n) {
        rc = host_reg_name(a, n, u->host, sizeof u->host);
        if (rc != URL_OK) return rc;
        u->port[0] = 0;
        return URL_OK;
    }
    rc = host_reg_name(a, colon, u->host, sizeof u->host);
    if (rc != URL_OK) return rc;
    if (colon + 1 < n)
        return parse_port(a + colon + 1, n - colon - 1, scheme,
                          u->port, sizeof u->port);
    u->port[0] = 0;
    return URL_OK;
}

/* ------------------------------------------------------------
 *  Parsing one reference into components.  Dot segments are NOT
 *  removed here: S5.2.2 decides where they are removed, and a
 *  relative reference such as ".." must survive until the merge.
 * ------------------------------------------------------------ */
static int ref_parse(const char *text, size_t n, Url *u) {
    Parts p;
    char tmp[URL_MAX_PATH];
    int rc;

    if (!text || !u) return URL_EINVAL;
    memset(u, 0, sizeof *u);

    /* RFC 3986 S2: a URI is built from a limited set of characters.
     * A raw control or a raw space is not among them, and neither is
     * DEL, so reject rather than repair.  A backslash is not in any
     * production either: silently reading it as '/' (the URL
     * Standard's compat rule) gives one resource two spellings and
     * is the classic URL-confusion bug.  Data that really is a
     * backslash travels as %5C. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c <= 0x20u || c == 0x7Fu || c == '\\') return URL_EBADCHAR;
    }

    split_reference(text, n, &p);

    if (p.has_scheme) {
        if (!scheme_syntax_ok(text + p.scheme_at, p.scheme_n))
            return URL_ESCHEME;
        if (p.scheme_n + 1 > sizeof u->scheme) return URL_ETOOLONG;
        for (size_t i = 0; i < p.scheme_n; i++) {
            unsigned char c = (unsigned char)text[p.scheme_at + i];
            u->scheme[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32u : c);
        }
        u->scheme[p.scheme_n] = 0;
        u->has_scheme = 1;
    }

    if (p.has_authority) {
        u->has_authority = 1;
        rc = parse_authority(text + p.auth_at, p.auth_n,
                             u->has_scheme ? u->scheme : "", u);
        if (rc != URL_OK) return rc;
    }

    rc = norm_component(text + p.path_at, p.path_n, MASK_PATH,
                        tmp, sizeof tmp);
    if (rc != URL_OK) return rc;
    rc = field_set(u->path, sizeof u->path, tmp, strlen(tmp));
    if (rc != URL_OK) return rc;

    if (p.has_query) {
        u->has_query = 1;
        rc = norm_component(text + p.query_at, p.query_n, MASK_QUERY,
                            u->query, sizeof u->query);
        if (rc != URL_OK) return rc;
    }
    if (p.has_fragment) {
        u->has_fragment = 1;
        rc = norm_component(text + p.frag_at, p.frag_n, MASK_QUERY,
                            u->fragment, sizeof u->fragment);
        if (rc != URL_OK) return rc;
    }
    return URL_OK;
}

int url_parse(Url *out, const char *text) {
    int rc;

    if (!out || !text) return URL_EINVAL;
    rc = ref_parse(text, strlen(text), out);
    if (rc != URL_OK) goto fail;
    if (!out->has_scheme) {                   /* an absolute URI needs one */
        rc = URL_ESCHEME;
        goto fail;
    }
    /* Normalisation: dot segments go (S5.2.4, applied by S6.2.3). */
    {
        char tmp[URL_MAX_PATH];
        rc = remove_dot_segments(out->path, tmp, sizeof tmp);
        if (rc != URL_OK) goto fail;
        rc = field_set(out->path, sizeof out->path, tmp, strlen(tmp));
        if (rc != URL_OK) goto fail;
    }
    return URL_OK;

fail:
    /* No partial Url: a caller that ignores the code sees an empty one. */
    memset(out, 0, sizeof *out);
    return rc;
}

/* ------------------------------------------------------------
 *  Reference resolution - RFC 3986 S5.2.1, S5.2.2, S5.2.3, S5.2.4
 * ------------------------------------------------------------ */
static int merge_paths(const Url *base, const char *refpath,
                       char *out, size_t osz) {
    Out o;

    out_init(&o, out, osz);
    if (base->has_authority && base->path[0] == 0) {
        out_ch(&o, '/');                      /* S5.2.3 first case */
    } else {
        size_t k = strlen(base->path);
        while (k > 0 && base->path[k - 1] != '/') k--;
        out_mem(&o, base->path, k);
    }
    out_str(&o, refpath);
    return out_end(&o);
}

/* Copy the authority fields of `src` to `dst`. */
static void copy_auth(Url *dst, const Url *src) {
    dst->has_authority = src->has_authority;
    dst->has_userinfo  = src->has_userinfo;
    dst->is_ipv6       = src->is_ipv6;
    if (src->has_authority) {
        memcpy(dst->userinfo, src->userinfo, sizeof dst->userinfo);
        memcpy(dst->host, src->host, sizeof dst->host);
        memcpy(dst->port, src->port, sizeof dst->port);
    }
}

/* RFC 3986 S6.2.3: a port equal to the scheme's DEFINED default is
 * written as absent.  ref_parse() can apply this only when the
 * reference carries its own scheme; a network-path reference such as
 * "//g:80/x" has no scheme yet, so it is applied again once the base's
 * scheme is known. */
static void elide_default_port(Url *u) {
    unsigned v = 0;
    unsigned d;

    if (!u->port[0]) return;
    for (const char *p = u->port; *p; p++) v = v * 10u + (unsigned)(*p - '0');
    d = url_default_port(u->scheme);
    /* S6.2.3 permits dropping the port only when it equals the
     * scheme's DEFINED default.  url_default_port() returns 0 for "no
     * default", which is not the same as port 0: "//g:0/x" resolved
     * against http keeps its ":0". */
    if (d != 0 && v == d) u->port[0] = 0;
}

int url_resolve(Url *out, const Url *base, const char *ref) {
    Url r, t;
    char path[URL_MAX_PATH];
    int rc;

    if (!out || !base || !ref) return URL_EINVAL;
    if (!base->has_scheme) return URL_ESCHEME;

    rc = ref_parse(ref, strlen(ref), &r);
    if (rc != URL_OK) return rc;

    memset(&t, 0, sizeof t);

    if (r.has_scheme) {
        /* S5.2.2 strict: "http:g" stays "http:g". */
        memcpy(t.scheme, r.scheme, sizeof t.scheme);
        t.has_scheme = 1;
        copy_auth(&t, &r);
        rc = remove_dot_segments(r.path, t.path, sizeof t.path);
        if (rc != URL_OK) return rc;
        t.has_query = r.has_query;
        if (r.has_query) memcpy(t.query, r.query, sizeof t.query);
    } else {
        if (r.has_authority) {
            copy_auth(&t, &r);
            rc = remove_dot_segments(r.path, t.path, sizeof t.path);
            if (rc != URL_OK) return rc;
            t.has_query = r.has_query;
            if (r.has_query) memcpy(t.query, r.query, sizeof t.query);
        } else {
            if (r.path[0] == 0) {
                memcpy(t.path, base->path, sizeof t.path);
                if (r.has_query) {
                    t.has_query = 1;
                    memcpy(t.query, r.query, sizeof t.query);
                } else if (base->has_query) {
                    t.has_query = 1;
                    memcpy(t.query, base->query, sizeof t.query);
                }
            } else {
                if (r.path[0] == '/') {
                    rc = remove_dot_segments(r.path, t.path, sizeof t.path);
                } else {
                    rc = merge_paths(base, r.path, path, sizeof path);
                    if (rc == URL_OK)
                        rc = remove_dot_segments(path, t.path, sizeof t.path);
                }
                if (rc != URL_OK) return rc;
                t.has_query = r.has_query;
                if (r.has_query) memcpy(t.query, r.query, sizeof t.query);
            }
            copy_auth(&t, base);
        }
        memcpy(t.scheme, base->scheme, sizeof t.scheme);
        t.has_scheme = 1;
    }

    t.has_fragment = r.has_fragment;
    if (r.has_fragment) memcpy(t.fragment, r.fragment, sizeof t.fragment);

    elide_default_port(&t);
    *out = t;
    return URL_OK;
}

/* ------------------------------------------------------------
 *  Serialising - RFC 3986 S5.3
 *
 *      scheme ":" [ "//" authority ] path [ "?" query ] [ "#" fragment ]
 *
 *  The authority is not added when the path is empty, because S5.3
 *  does not: "http://g" stays "http://g" (S5.4.1).
 * ------------------------------------------------------------ */
int url_to_string(const Url *u, char *out, size_t osz) {
    Out o;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!u) return URL_EINVAL;
    out_init(&o, out, osz);

    if (u->has_scheme) {
        out_str(&o, u->scheme);
        out_ch(&o, ':');
    }
    if (u->has_authority) {
        out_mem(&o, "//", 2);
        if (u->has_userinfo) {
            out_str(&o, u->userinfo);
            out_ch(&o, '@');
        }
        if (u->is_ipv6) {
            out_ch(&o, '[');
            out_str(&o, u->host);
            out_ch(&o, ']');
        } else {
            out_str(&o, u->host);
        }
        if (u->port[0]) {
            out_ch(&o, ':');
            out_str(&o, u->port);
        }
    }
    out_str(&o, u->path);
    if (u->has_query) {
        out_ch(&o, '?');
        out_str(&o, u->query);
    }
    if (u->has_fragment) {
        out_ch(&o, '#');
        out_str(&o, u->fragment);
    }
    return out_end(&o);
}

int url_build(char *out, size_t osz, const char *scheme, const char *host,
              unsigned port, const char *path, const char *query) {
    Url u;
    char norm_path[URL_MAX_PATH];
    char scheme_lc[URL_MAX_SCHEME];
    size_t sn;
    int rc;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!scheme || !host) return URL_EINVAL;
    /* The contract says absolute or empty.  "h" + "x" would serialise
     * as "http://hx", a different URL, so refuse instead of guessing. */
    if (path && path[0] && path[0] != '/') return URL_EARG;

    memset(&u, 0, sizeof u);
    sn = strlen(scheme);
    if (sn == 0 || sn + 1 > sizeof scheme_lc) return URL_ESCHEME;
    if (!scheme_syntax_ok(scheme, sn)) return URL_ESCHEME;
    for (size_t i = 0; i < sn; i++) {
        unsigned char c = (unsigned char)scheme[i];
        scheme_lc[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32u : c);
    }
    scheme_lc[sn] = 0;
    memcpy(u.scheme, scheme_lc, sn + 1);
    u.has_scheme = 1;
    u.has_authority = 1;

    rc = url_host_to_ascii(host, u.host, sizeof u.host);
    if (rc != URL_OK) return rc;
    /* Bracketed input is an IP-literal by construction; a bare host is
     * one when the validated form contains ':' (a reg-name cannot).
     * This is what makes url_build(u.scheme, u.host, ...) able to
     * rebuild a parsed IPv6 URL from its own components. */
    u.is_ipv6 = (host[0] == '[') || (strchr(u.host, ':') != NULL);

    {
        char portbuf[URL_MAX_PORT];
        unsigned v = port ? port : url_default_port(scheme_lc);
        size_t k = 0, j;

        if (v > 65535u) return URL_EPORT;
        if (v != url_default_port(scheme_lc) && v != 0) {
            char rev[URL_MAX_PORT];
            do { rev[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
            for (j = 0; j < k; j++) portbuf[j] = rev[k - 1 - j];
            portbuf[k] = 0;
        } else {
            portbuf[0] = 0;
        }
        /* Only the bytes that were written: copying the whole array
         * would put uninitialised stack bytes into a struct that is
         * fixed-size and may be handed to a caller as-is. */
        memcpy(u.port, portbuf, k + 1);
    }

    {
        const char *p = path ? path : "";
        rc = norm_component(p, strlen(p), MASK_PATH,
                            norm_path, sizeof norm_path);
        if (rc != URL_OK) return rc;
        rc = remove_dot_segments(norm_path, u.path, sizeof u.path);
        if (rc != URL_OK) return rc;
    }
    if (query) {
        u.has_query = 1;
        rc = norm_component(query, strlen(query), MASK_QUERY,
                            u.query, sizeof u.query);
        if (rc != URL_OK) return rc;
    }
    return url_to_string(&u, out, osz);
}

/* ------------------------------------------------------------
 *  Origin - RFC 6454 S4, S5, S6.2.
 *
 *  The port compared is the EFFECTIVE one: an elided default and an
 *  explicit default are the same origin, which is why url_parse()
 *  elides it in the first place.
 * ------------------------------------------------------------ */
static unsigned effective_port(const Url *u) {
    if (!u) return 0;
    if (u->port[0]) {
        unsigned v = 0;
        for (const char *p = u->port; *p; p++) v = v * 10u + (unsigned)(*p - '0');
        return v;
    }
    return url_default_port(u->scheme);
}

int url_origin(const Url *u, char *out, size_t osz) {
    Out o;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!u) return URL_EINVAL;
    if (!u->has_scheme || !u->has_authority) return URL_ENOAUTH;
    out_init(&o, out, osz);
    out_str(&o, u->scheme);
    out_mem(&o, "://", 3);
    if (u->is_ipv6) {
        out_ch(&o, '[');
        out_str(&o, u->host);
        out_ch(&o, ']');
    } else {
        out_str(&o, u->host);
    }
    if (u->port[0]) {
        out_ch(&o, ':');
        out_str(&o, u->port);
    }
    return out_end(&o);
}

int url_same_origin(const Url *a, const Url *b) {
    if (!a || !b) return 0;
    if (!a->has_scheme || !b->has_scheme) return 0;
    if (!a->has_authority || !b->has_authority) return 0;
    if (strcmp(a->scheme, b->scheme) != 0) return 0;
    if (strcmp(a->host, b->host) != 0) return 0;
    return effective_port(a) == effective_port(b);
}

int url_request_target(const Url *u, char *out, size_t osz) {
    Out o;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!u) return URL_EINVAL;
    out_init(&o, out, osz);
    /* RFC 9112 S3.2.1: origin-form is absolute-path [ "?" query ],
     * and an empty path is sent as "/". */
    if (u->path[0] == 0) out_ch(&o, '/');
    else                 out_str(&o, u->path);
    if (u->has_query) {
        out_ch(&o, '?');
        out_str(&o, u->query);
    }
    return out_end(&o);
}

/* ------------------------------------------------------------
 *  9. Query strings
 * ------------------------------------------------------------ */
static int query_add_decoded(UrlQuery *q, const char *name, size_t nn,
                             const char *value, size_t vn) {
    UrlParam *p;

    if (!q) return URL_EINVAL;
    if (q->count >= URL_MAX_PARAMS) return URL_EBADQUERY;
    p = &q->item[q->count];
    if (nn + 1 > sizeof p->name || vn + 1 > sizeof p->value)
        return URL_ETOOLONG;
    if (nn) memcpy(p->name, name, nn);
    p->name[nn] = 0;
    if (vn) memcpy(p->value, value, vn);
    p->value[vn] = 0;
    q->count++;
    return URL_OK;
}

/* `plus_is_space` is the one difference between the RFC view and the
 * HTML form view (HTML Standard, urlencoded parser, step 1).
 *
 * An empty sequence ("a=1&", "&&") is skipped, as that parser's
 * step 4.1 does: only a sequence with contents can name a parameter,
 * and keeping "" would make url_query_build() emit a trailing '&'.
 * A sequence of just "=" is not empty and is kept (empty name and
 * empty value), which is what the same parser does. */
static int query_parse_impl(UrlQuery *q, const char *query, int plus_is_space) {
    size_t n, i = 0;

    if (!q || !query) return URL_EINVAL;
    q->count = 0;
    n = strlen(query);
    if (n == 0) return URL_OK;

    while (i <= n) {
        size_t end = i, eq, k;
        char name[URL_MAX_VALUE];
        char value[URL_MAX_VALUE];
        size_t nn = 0, vn = 0;
        int rc;

        while (end < n && query[end] != '&') end++;
        if (end == i) {                     /* empty sequence */
            if (end >= n) break;
            i = end + 1;
            continue;
        }
        eq = i;
        while (eq < end && query[eq] != '=') eq++;

        /* Decode in place-safe temporaries; a decoded NUL is refused
         * because the list stores C strings. */
        for (k = i; k < eq; k++) {
            char c = query[k];
            if (c == '+') c = plus_is_space ? ' ' : '+';
            if (c == '%') {
                if (k + 2 >= eq || !ascii_hex((unsigned char)query[k + 1]) ||
                    !ascii_hex((unsigned char)query[k + 2]))
                    return URL_EBADPCT;
                c = (char)((hex_val((unsigned char)query[k + 1]) << 4) |
                           hex_val((unsigned char)query[k + 2]));
                k += 2;
            }
            if (c == 0) return URL_EBADQUERY;
            if (nn + 1 >= sizeof name) return URL_ETOOLONG;
            name[nn++] = c;
        }
        if (eq < end) {
            for (k = eq + 1; k < end; k++) {
                char c = query[k];
                if (c == '+') c = plus_is_space ? ' ' : '+';
                if (c == '%') {
                    if (k + 2 >= end ||
                        !ascii_hex((unsigned char)query[k + 1]) ||
                        !ascii_hex((unsigned char)query[k + 2]))
                        return URL_EBADPCT;
                    c = (char)((hex_val((unsigned char)query[k + 1]) << 4) |
                               hex_val((unsigned char)query[k + 2]));
                    k += 2;
                }
                if (c == 0) return URL_EBADQUERY;
                if (vn + 1 >= sizeof value) return URL_ETOOLONG;
                value[vn++] = c;
            }
        }
        rc = query_add_decoded(q, name, nn, value, vn);
        if (rc != URL_OK) return rc;

        if (end >= n) break;
        i = end + 1;
    }
    return URL_OK;
}

int url_query_parse(UrlQuery *q, const char *query) {
    return query_parse_impl(q, query, 0);
}

int url_query_parse_form(UrlQuery *q, const char *query) {
    return query_parse_impl(q, query, 1);
}

const char *url_query_get(const UrlQuery *q, const char *name) {
    if (!q || !name) return NULL;
    for (int i = 0; i < q->count; i++)
        if (strcmp(q->item[i].name, name) == 0) return q->item[i].value;
    return NULL;
}

int url_query_count(const UrlQuery *q, const char *name) {
    int n = 0;

    if (!q || !name) return 0;
    for (int i = 0; i < q->count; i++)
        if (strcmp(q->item[i].name, name) == 0) n++;
    return n;
}

int url_query_add(UrlQuery *q, const char *name, const char *value) {
    if (!q || !name || !value) return URL_EINVAL;
    return query_add_decoded(q, name, strlen(name), value, strlen(value));
}

int url_query_build(const UrlQuery *q, char *out, size_t osz) {
    Out o;

    if (!out) return URL_EINVAL;
    if (osz == 0) return URL_ETOOLONG;
    out[0] = 0;
    if (!q) return URL_EINVAL;
    out_init(&o, out, osz);
    for (int i = 0; i < q->count; i++) {
        /* A value is at most URL_MAX_VALUE-1 octets and a name at most
         * URL_MAX_NAME-1, each encoding to 3x its length; size the
         * scratch for the larger one, not for their sum. */
        char enc[3 * URL_MAX_VALUE];
        size_t en = 0;
        int rc;

        if (i) out_ch(&o, '&');
        rc = url_pct_encode(q->item[i].name, strlen(q->item[i].name), NULL,
                            enc, sizeof enc, &en);
        if (rc != URL_OK) return out_fail(&o, rc);
        out_mem(&o, enc, en);
        out_ch(&o, '=');
        rc = url_pct_encode(q->item[i].value, strlen(q->item[i].value), NULL,
                            enc, sizeof enc, &en);
        if (rc != URL_OK) return out_fail(&o, rc);
        out_mem(&o, enc, en);
    }
    return out_end(&o);
}

/* ------------------------------------------------------------
 *  Diagnostics
 * ------------------------------------------------------------ */
const char *url_strerror(int err) {
    switch (err) {
    case URL_OK:        return "ok";
    case URL_EINVAL:    return "invalid argument";
    case URL_ETOOLONG:  return "component too long";
    case URL_ESCHEME:   return "missing or malformed scheme";
    case URL_EBADCHAR:  return "control character, space or backslash in URL";
    case URL_EBADPCT:   return "malformed percent-escape";
    case URL_EPORT:     return "port is not 0..65535";
    case URL_EHOST:     return "invalid host name";
    case URL_EIPV6:     return "invalid IPv6 / IPvFuture literal";
    case URL_EUTF8:     return "invalid UTF-8";
    case URL_ENOAUTH:   return "operation needs scheme and host";
    case URL_EBADQUERY: return "query list full or contains NUL";
    case URL_EARG:      return "argument out of range";
    default:            return "unknown URL error";
    }
}
