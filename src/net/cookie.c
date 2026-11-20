/* ============================================================
 *  Mystic OS - net/cookie.c
 *  RFC 6265 cookie parsing, storage and Cookie header generation,
 *  plus a canonical text form of the whole jar.
 *
 *  The file is one pass per job, in the order RFC 6265 uses:
 *
 *      1. octet classes and slices
 *      2. canonicalized host names      RFC 6265 S5.1.2
 *      3. domain-match                  RFC 6265 S5.1.3
 *      4. default-path and path-match   RFC 6265 S5.1.4
 *      5. cookie-date                   RFC 6265 S5.1.1
 *      6. Set-Cookie parsing            RFC 6265 S5.2
 *      7. the storage model             RFC 6265 S5.3
 *      8. the Cookie header             RFC 6265 S5.4
 *      9. the text form
 *     10. diagnostics
 *
 *  No allocation, no globals, no errno, no clock calls: "now" is always
 *  a parameter.  See cookie.h for the contract and for the list of
 *  things this module deliberately does not do (public suffixes, IDNA,
 *  SameSite request policy).
 * ============================================================ */
#include "cookie.h"

#include <string.h>

/* ============================================================
 *  1. Octet classes, slices, and a bounded output cursor
 * ============================================================ */

/* RFC 6265 S4.1.1's cookie-octet production is
 *     %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
 * which excludes every control character, so a Set-Cookie field that
 * contains one is not a set-cookie-string at all.  S5.2's parse
 * algorithm is deliberately more permissive than that grammar, so
 * rejecting such a field is an ADDITIONAL restriction of this jar, not a
 * step of S5.2: it is one of the "more restrictions" RFC 6265 S5 allows
 * a user agent to enforce.  (6265bis later folded a CTL abort into its
 * own parse algorithm, but nothing here depends on that document for
 * this rule.) */
static int is_ctl(unsigned char c) {
    return c <= 0x08 || (c >= 0x0A && c <= 0x1F) || c == 0x7F;
}

/* RFC 6265 S5.2: "Remove any leading or trailing WSP characters", and
 * S4.1.1's WSP is RFC 5234's SP / HTAB. */
static int is_wsp(unsigned char c) {
    return c == 0x20 || c == 0x09;
}

static int is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* An immutable view of part of a header.  Nothing in this file copies a
 * substring out just to look at it. */
typedef struct {
    const char *p;
    size_t      n;
} Slice;

static Slice slice_make(const char *p, size_t n) {
    Slice s;
    s.p = p;
    s.n = n;
    return s;
}

/* Does a field hold a byte the text form cannot spell?  A field byte has
 * to be printable ASCII or >= 0x80: the escape set is {'%', 0x80-0xFF},
 * so a C0 control, HTAB and DEL have no representation at all.  RFC 6265
 * S4.1.1's cookie-octet excludes them too (it admits no whitespace and no
 * control byte), so a set-cookie-string field holding one is already
 * invalid by the grammar; this jar refuses it instead of storing
 * something it could never write back. */
static int slice_has_unspellable(Slice s) {
    size_t i;
    for (i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c < 0x20 || c == 0x7F) return 1;
    }
    return 0;
}

static int cstr_has_unspellable(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) return 1;
    }
    return 0;
}

static Slice slice_trim(Slice s) {
    while (s.n > 0 && is_wsp((unsigned char)s.p[0])) { s.p++; s.n--; }
    while (s.n > 0 && is_wsp((unsigned char)s.p[s.n - 1])) s.n--;
    return s;
}

static int slice_eq_ci_lit(Slice s, const char *lit) {
    size_t i, n = strlen(lit);
    if (s.n != n) return 0;
    for (i = 0; i < n; i++)
        if (lower_ascii(s.p[i]) != lower_ascii(lit[i])) return 0;
    return 1;
}

/* Copy a slice into a fixed field, NUL-terminated.  A slice that does
 * not fit is COOKIE_ETOOLONG: see "STORAGE POLICY" in cookie.h for why
 * this is not a truncation. */
static int slice_copy(char *dst, size_t cap, Slice s) {
    if (s.n + 1 > cap) return COOKIE_ETOOLONG;
    if (s.n) memcpy(dst, s.p, s.n);
    dst[s.n] = 0;
    return COOKIE_OK;
}

/* ------------------------------------------------------------
 *  Output cursor.  Every writer goes through this, so no path can
 *  write past its buffer, no path can return a string that is not
 *  NUL-terminated, and *need is exact even when the buffer is too
 *  small: the byte count keeps rising after the first overflow.
 *
 *  cap == 0 means "measure only" (out may be NULL).  A caller that
 *  passes a real buffer and cap > 0 either gets the whole string or
 *  COOKIE_ETOOLONG with out[0] == 0.
 * ------------------------------------------------------------ */
typedef struct {
    char  *p;
    size_t cap;
    size_t n;
    int    err;
} Out;

static void out_init(Out *o, char *p, size_t cap) {
    o->p = p;
    o->cap = cap;
    o->n = 0;
    o->err = COOKIE_OK;
}

static void out_ch(Out *o, char c) {
    if (o->cap == 0) { o->n++; return; }
    if (o->n + 1 < o->cap) o->p[o->n] = c;
    else o->err = COOKIE_ETOOLONG;
    o->n++;
}

static void out_mem(Out *o, const char *s, size_t n) {
    if (o->cap == 0) { o->n += n; return; }
    if (o->n + n + 1 <= o->cap) {
        if (n) memcpy(o->p + o->n, s, n);
    } else {
        o->err = COOKIE_ETOOLONG;
    }
    o->n += n;
}

static void out_cstr(Out *o, const char *s) {
    out_mem(o, s, strlen(s));
}

/* Decimal, no stdio: INT64_MIN is written correctly through the
 * unsigned magnitude. */
static void out_i64(Out *o, int64_t v) {
    char buf[24];
    size_t n = 0;
    uint64_t u;
    if (v < 0) { out_ch(o, '-'); u = (uint64_t)0 - (uint64_t)v; }
    else u = (uint64_t)v;
    do {
        buf[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u != 0);
    while (n > 0) out_ch(o, buf[--n]);
}

/* The canonical escaping of the text form (cookie.h, "THE TEXT FORM"):
 * a byte is written raw when it is printable ASCII other than '%', and
 * as '%' plus two UPPERCASE hex digits when it is '%' or 0x80-0xFF.
 * Nothing else can arrive: jar_invariants_ok() has already refused a jar
 * whose fields hold a C0 control, HTAB, DEL or NUL.  If one ever did
 * reach here it would be written raw and the decoder would refuse the
 * file - a loud failure, never a silent substitution. */
static void out_escaped(Out *o, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; p++) {
        if (*p != '%' && *p < 0x80) {
            out_ch(o, (char)*p);
        } else {
            out_ch(o, '%');
            out_ch(o, hex[*p >> 4]);
            out_ch(o, hex[*p & 0x0F]);
        }
    }
}

static int out_finish(Out *o, size_t *need) {
    if (need) *need = o->n + 1;
    if (o->cap == 0) return COOKIE_OK;
    if (o->err != COOKIE_OK || o->n + 1 > o->cap) {
        o->p[0] = 0;
        return COOKIE_ETOOLONG;
    }
    o->p[o->n] = 0;
    return COOKIE_OK;
}

/* ============================================================
 *  2. Canonicalized host names - RFC 6265 S5.1.2
 *
 *  S5.1.2 is three steps: split into labels, convert each non-ASCII
 *  label to an A-label, then concatenate with ".".  Step 2 needs
 *  IDNA2008 (RFC 5890 S2.3) and the Unicode tables, which this system
 *  does not carry, so a non-ASCII byte is COOKIE_EHOST rather than a
 *  guess (cookie.h explains, and points at net/url.c's Punycode
 *  encoder for the caller that does have them).
 *
 *  What is left is steps 1 and 3 plus the length rules of RFC 1035
 *  S2.3.4 that make a label sequence a domain name at all: <= 63
 *  octets per label and <= 255 octets for the name.  One trailing dot
 *  is the absolute-name root of RFC 1035 S3.1 and is not part of the
 *  name, so it is dropped - which is also what makes "Example.COM."
 *  and "example.com" the same host, as S5.1.2 intends.
 *
 *  A string containing ':' is an IP literal, not a domain name; its
 *  labels are not domain labels, so only the lowercase pass and the
 *  length bound apply.
 * ============================================================ */
static int host_byte_ok(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_' || c == ':';
}

static int canon_host_n(const char *p, size_t n, char *out, size_t cap) {
    size_t i;

    if (!p || !out) return COOKIE_EINVAL;
    if (n == 0) return COOKIE_EHOST;
    if (n + 1 > cap) return COOKIE_ETOOLONG;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (!host_byte_ok(c)) return COOKIE_EHOST;
        out[i] = lower_ascii((char)c);
    }
    out[n] = 0;

    if (n > 1 && out[n - 1] == '.') {
        out[n - 1] = 0;
        n--;
    }
    if (n == 0) return COOKIE_EHOST;
    if (n > 255) return COOKIE_ETOOLONG;               /* RFC 1035 S2.3.4 */

    if (!memchr(out, ':', n)) {
        size_t label = 0;
        for (i = 0; i <= n; i++) {
            if (i == n || out[i] == '.') {
                if (label == 0 || label > 63) return COOKIE_EHOST;
                label = 0;
            } else {
                label++;
            }
        }
    }
    return COOKIE_OK;
}

int cookie_canonical_host(const char *host, char *out, size_t cap) {
    if (!host || !out) return COOKIE_EINVAL;
    return canon_host_n(host, strlen(host), out, cap);
}

/* ============================================================
 *  3. domain-match - RFC 6265 S5.1.3
 *
 *  "If the domain string and the string are identical, return true.
 *   Otherwise, if the domain string is a suffix of the string and the
 *   last character of the string that is not included in the domain
 *   string is a %x2E (".") character, return true.  Otherwise, return
 *   false."
 *
 *  The suffix branch has a third condition that is easy to drop and
 *  expensive to drop: "The string is a host name (i.e., not an IP
 *  address)."  Without it Domain=0.0.1, set from 10.0.0.1, would be
 *  sent to 127.0.0.1.
 *
 *  Both arguments must already be canonical (lowercase); this is the
 *  algorithm, not the attribute processing, so a leading dot is NOT
 *  stripped here - S5.2.3 does that, and the non-normative parenthetical
 *  in S4.1.2.3 ("a leading %x2E, if present, is ignored even though that
 *  character is not permitted") describes the result of the stripping
 *  rather than a rule of this algorithm.
 * ============================================================ */

/* Is `host` an address literal rather than a name?  S5.1.3 does not
 * define "IP address", so:
 *   - any host containing ':' is an IPv6 literal;
 *   - a host whose labels are all digits is an IPv4 address: the
 *     canonical dotted quad, and also a spelling such as 0127.0.0.1
 *     that RFC 3986 S3.2.2's dec-octet would reject but that no
 *     registrable name looks like either.
 * The second clause is stricter than the RFC's letter.  It can only
 * turn a match off, never on, so it can never widen a cookie. */
static int host_is_ip_address(const char *host) {
    size_t i, n, label = 0;
    int any_label = 0;

    if (!host) return 0;
    n = strlen(host);
    if (n == 0) return 0;
    if (memchr(host, ':', n)) return 1;              /* IPv6 literal */
    for (i = 0; i <= n; i++) {
        if (i == n || host[i] == '.') {
            if (label == 0) return 0;                /* empty label: a name */
            any_label = 1;
            label = 0;
            continue;
        }
        if (!is_digit((unsigned char)host[i])) return 0;
        label++;
    }
    return any_label;
}

int cookie_domain_match(const char *domain, const char *host) {
    size_t dl, hl;

    if (!domain || !host) return 0;
    dl = strlen(domain);
    hl = strlen(host);
    if (dl == 0 || hl == 0) return 0;
    /* The identical branch carries no host-name condition, which is what
     * lets a host-only cookie on an IP literal (domain == host) work. */
    if (dl == hl) return strcmp(domain, host) == 0;
    if (dl > hl) return 0;
    if (host_is_ip_address(host)) return 0;          /* S5.1.3, third condition */
    if (strcmp(host + (hl - dl), domain) != 0) return 0;
    return host[hl - dl - 1] == '.';
}

/* ============================================================
 *  4. Paths - RFC 6265 S5.1.4
 * ============================================================ */

/* S5.1.4's default-path, step for step:
 *   2. empty or not starting with "/"        -> "/"
 *   3. no more than one "/"                  -> "/"
 *   4. up to but not including the rightmost "/"
 */
int cookie_default_path(const char *request_path, char *out, size_t cap) {
    const char *p = request_path;
    size_t n, i, slashes = 0, cut;

    if (!request_path || !out) return COOKIE_EINVAL;
    if (cap < 2) return COOKIE_ETOOLONG;
    n = strlen(p);

    if (n == 0 || p[0] != '/') {                       /* step 2 */
        out[0] = '/';
        out[1] = 0;
        return COOKIE_OK;
    }
    for (i = 0; i < n; i++)
        if (p[i] == '/') slashes++;
    if (slashes <= 1) {                                /* step 3 */
        out[0] = '/';
        out[1] = 0;
        return COOKIE_OK;
    }
    cut = n;                                           /* step 4 */
    while (cut > 0 && p[cut - 1] != '/') cut--;
    if (cut == 0) cut = 1;                             /* unreachable: p[0] == '/' */
    if (cut + 1 > cap) return COOKIE_ETOOLONG;
    memcpy(out, p, cut - 1);
    out[cut - 1] = 0;
    return COOKIE_OK;
}

/* S5.1.4's path-match, step for step.  A zero-length cookie path cannot
 * occur (the default-path algorithm never produces one) but is handled
 * so that the function is total. */
int cookie_path_match(const char *cookie_path, const char *request_path) {
    size_t cl, rl;

    if (!cookie_path || !request_path) return 0;
    cl = strlen(cookie_path);
    rl = strlen(request_path);
    if (cl == 0) return 0;
    if (cl == rl) return strcmp(cookie_path, request_path) == 0;
    if (cl > rl) return 0;
    if (memcmp(cookie_path, request_path, cl) != 0) return 0;
    if (cookie_path[cl - 1] == '/') return 1;
    return request_path[cl] == '/';
}

/* ============================================================
 *  5. cookie-date - RFC 6265 S5.1.1
 *
 *  The three formats of RFC 2616 S3.3.1 that S5.1.1 has to accept:
 *
 *      Sun, 06 Nov 1994 08:49:37 GMT     rfc1123-date
 *      Sunday, 06-Nov-94 08:49:37 GMT    rfc850-date
 *      Sun Nov  6 08:49:37 1994          asctime-date
 *
 *  S5.1.1 does not parse those grammars directly.  It defines a
 *  delimiter set, splits on it, and scans the tokens looking for a
 *  time, a day-of-month, a month and a year, in any order and with
 *  anything else ignored - which is why all three forms work with one
 *  algorithm, and why "GMT" never has to be understood.
 * ============================================================ */

/* S5.1.1's delimiter production:
 *     delimiter = %x09 / %x20-2F / %x3B-40 / %x5B-60 / %x7B-7E
 * Note what is NOT a delimiter: ':' (0x3A) and '_' (0x5F), which is how
 * "08:49:37" stays one token. */
static int is_date_delim(unsigned char c) {
    return c == 0x09 ||
           (c >= 0x20 && c <= 0x2F) ||
           (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

/* time = hms-time ( non-digit *OCTET )
 * hms-time = time-field ":" time-field ":" time-field
 * time-field = 1*2DIGIT
 *
 * The trailing group is treated as optional, because S5.1.1's own
 * rfc1123-date example tokenises "08:49:37" with nothing after it;
 * requiring a non-digit there would reject the RFC's example.  A
 * trailing DIGIT is still rejected: "08:49:371" is not a time. */
static int tok_time(Slice t, int *h, int *mi, int *s) {
    int v[3];
    size_t i = 0;
    int f;

    for (f = 0; f < 3; f++) {
        size_t start = i;
        int val = 0;
        while (i < t.n && i - start < 2 && is_digit((unsigned char)t.p[i])) {
            val = val * 10 + (t.p[i] - '0');
            i++;
        }
        if (i == start) return 0;
        v[f] = val;
        if (f < 2) {
            if (i >= t.n || t.p[i] != ':') return 0;
            i++;
        }
    }
    if (i < t.n && is_digit((unsigned char)t.p[i])) return 0;
    *h = v[0];
    *mi = v[1];
    *s = v[2];
    return 1;
}

/* day-of-month = 1*2DIGIT ( non-digit *OCTET ) */
static int tok_day(Slice t, int *day) {
    size_t i = 0;
    int v = 0;
    while (i < t.n && i < 2 && is_digit((unsigned char)t.p[i])) {
        v = v * 10 + (t.p[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    if (i < t.n && is_digit((unsigned char)t.p[i])) return 0;
    *day = v;
    return 1;
}

/* month = "jan" / "feb" / ... / "dec"
 *
 * The production is the three-letter form and has no trailing group, so
 * "January" is not a month token.  That is the letter of S5.1.1; a date
 * that fails to parse costs a session cookie, not a wrong cookie. */
static int tok_month(Slice t, int *mon) {
    static const char *const name[12] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    int i;

    if (t.n != 3) return 0;
    for (i = 0; i < 12; i++) {
        int k;
        for (k = 0; k < 3; k++)
            if (lower_ascii(t.p[k]) != name[i][k]) break;
        if (k == 3) {
            *mon = i + 1;
            return 1;
        }
    }
    return 0;
}

/* year = 2*4DIGIT ( non-digit *OCTET ) */
static int tok_year(Slice t, int *yr) {
    size_t i = 0;
    int v = 0;
    while (i < t.n && i < 4 && is_digit((unsigned char)t.p[i])) {
        v = v * 10 + (t.p[i] - '0');
        i++;
    }
    if (i < 2) return 0;
    if (i < t.n && is_digit((unsigned char)t.p[i])) return 0;
    *yr = v;
    return 1;
}

static int is_leap(int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* The length of a Gregorian month.  S5.1.1's last step rejects a
 * day-of-month that the month does not have, so this is what stops
 * "31 Apr" and "29 Feb 1900" from being normalised into the next
 * month instead of failing. */
static int days_in_month(int64_t year, int mon) {
    static const int len[12] = {31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31};
    if (mon == 2 && is_leap(year)) return 29;
    return len[mon - 1];
}

/* Proleptic Gregorian date to Unix seconds, by counting whole years and
 * then whole months.  No magic constants: 86400 is a day, 3600 an hour,
 * 60 a minute, and the leap rule is the Gregorian one.  S5.1.1 allows
 * any year from 1601 to 9999, so the loop is at most 8030 iterations.
 *
 * The caller has already established that the date exists (see
 * cookie_parse_date), so a day that the month does not have cannot
 * arrive here. */
static int64_t date_to_unix(int year, int mon, int day, int h, int mi, int s) {
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181,
                                212, 243, 273, 304, 334};
    int64_t days = 0;
    int y;

    for (y = 1970; y < year; y++) days += is_leap(y) ? 366 : 365;
    for (y = year; y < 1970; y++) days -= is_leap(y) ? 366 : 365;
    days += cum[mon - 1] + ((mon > 2 && is_leap(year)) ? 1 : 0) + (day - 1);
    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)s;
}

int cookie_parse_date(const char *text, size_t len, int64_t *out) {
    int have_time = 0, have_day = 0, have_month = 0, have_year = 0;
    int h = 0, mi = 0, s = 0, day = 0, mon = 0, year = 0;
    size_t i = 0;

    if (!text || !out) return COOKIE_EINVAL;

    while (i < len) {
        size_t start;
        Slice t;

        while (i < len && is_date_delim((unsigned char)text[i])) i++;
        start = i;
        while (i < len && !is_date_delim((unsigned char)text[i])) i++;
        if (i == start) break;
        t = slice_make(text + start, i - start);

        if (!have_time && tok_time(t, &h, &mi, &s)) { have_time = 1; continue; }
        if (!have_day && tok_day(t, &day)) { have_day = 1; continue; }
        if (!have_month && tok_month(t, &mon)) { have_month = 1; continue; }
        if (!have_year && tok_year(t, &year)) { have_year = 1; continue; }
    }

    if (!have_time || !have_day || !have_month || !have_year) return COOKIE_EDATE;

    /* steps 3 and 4 */
    if (year >= 70 && year <= 99) year += 1900;
    else if (year >= 0 && year <= 69) year += 2000;

    /* steps 5 to 10 */
    if (day < 1 || day > 31) return COOKIE_EDATE;
    if (year < 1601) return COOKIE_EDATE;
    if (mon < 1 || mon > 12) return COOKIE_EDATE;
    if (h > 23) return COOKIE_EDATE;
    if (mi > 59) return COOKIE_EDATE;
    if (s > 59) return COOKIE_EDATE;

    /* S5.1.1's last step: the parsed-cookie-date must be "the date whose
     * day-of-month, month, year, hour, minute, and second ... have the
     * values denoted by the date-tokens", and "if no such date exists,
     * abort these steps and fail to parse the cookie-date".  The checks
     * above bound the day at 31; this one rejects 31 April, 30 February
     * and 29 February 1900 instead of normalising them. */
    if (day > days_in_month(year, mon)) return COOKIE_EDATE;

    *out = date_to_unix(year, mon, day, h, mi, s);
    return COOKIE_OK;
}

/* ============================================================
 *  6. Set-Cookie parsing - RFC 6265 S5.2
 * ============================================================ */

typedef struct {
    char     name[COOKIE_MAX_NAME];
    char     value[COOKIE_MAX_VALUE];
    char     domain[COOKIE_MAX_DOMAIN];
    char     path[COOKIE_MAX_PATH];
    int      have_expires;
    int64_t  expires;
    int      have_max_age;
    int64_t  max_age;
    int      have_domain;
    int      domain_ok;        /* domain passed S5.1.2 canonicalisation */
    int      have_path;
    int      path_default;     /* the last Path was empty/relative (S5.2.4) */
    uint8_t  flags;
    uint8_t  samesite;
} SetCookie;

/* S5.2.2: "If the first character of the attribute-value is not a DIGIT
 * or a '-' character, ignore the cookie-av.  If the remainder of
 * attribute-value contains a non-DIGIT character, ignore the
 * cookie-av."  A bare "-" has no digits at all, so it is not a number
 * and the attribute is ignored too.
 *
 * The value is saturated, not wrapped: RFC 6265 lets the server send
 * any number of digits, and an expiry that wraps into the past would
 * silently delete the cookie. */
static int parse_max_age(Slice v, int64_t *out) {
    uint64_t limit, val = 0;
    size_t i = 0;
    int neg = 0, digits = 0;

    if (v.n == 0) return 0;
    if (v.p[0] == '-') { neg = 1; i = 1; }
    limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;

    for (; i < v.n; i++) {
        if (!is_digit((unsigned char)v.p[i])) return 0;
        if (val > limit / 10u) {
            val = limit;                    /* saturate */
        } else {
            val = val * 10u + (uint64_t)(v.p[i] - '0');
            if (val > limit) val = limit;
        }
        digits++;
    }
    if (digits == 0) return 0;

    if (neg) {
        if (val == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
        else *out = -(int64_t)val;
    } else {
        *out = (int64_t)val;
    }
    return 1;
}

/* One cookie-av: name and value, already split and trimmed.  Returns
 * COOKIE_OK, or COOKIE_ETOOLONG when a value cannot be represented. */
static int parse_av(SetCookie *a, Slice name, Slice value) {
    int64_t t;

    if (name.n == 0) return COOKIE_OK;              /* empty attribute-name */

    if (slice_eq_ci_lit(name, "expires")) {         /* S5.2.1 */
        if (cookie_parse_date(value.p, value.n, &t) == COOKIE_OK) {
            a->have_expires = 1;
            a->expires = t;
        }
        /* An unparseable date is an ignored attribute, not an error:
         * the cookie then has no expiry and is a session cookie. */
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "max-age")) {         /* S5.2.2 */
        if (parse_max_age(value, &t)) {
            a->have_max_age = 1;
            a->max_age = t;
        }
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "domain")) {          /* S5.2.3 */
        Slice d = value;
        if (d.n == 0) return COOKIE_OK;             /* ignore the av */
        if (d.p[0] == '.') { d.p++; d.n--; }        /* S5.2.3 drops one leading dot */
        if (d.n == 0) return COOKIE_OK;
        a->have_domain = 1;
        a->domain_ok = 0;
        if (d.p[d.n - 1] == '.') {
            /* RFC 6265 S4.1.2.3 is semantics prose marked non-normative,
             * and its parenthetical says a trailing %x2E "will cause the
             * user agent to ignore the attribute".  The normative client
             * algorithm says otherwise: S5.2.3 leaves the dot in place,
             * S5.1.3 cannot domain-match "example.com." against the
             * canonical host "example.com", and S5.3 step 4.1 then
             * rejects the cookie.  Where the two disagree the algorithms
             * win, so this module stores nothing (COOKIE_EDOMAIN) rather
             * than silently downgrading the cookie to host-only. */
            return COOKIE_OK;
        }
        if (canon_host_n(d.p, d.n, a->domain, sizeof a->domain) == COOKIE_OK)
            a->domain_ok = 1;
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "path")) {            /* S5.2.4 */
        /* S5.2.4: "If the attribute-value is empty or if the first
         * character of the attribute-value is not %x2F ("/"), let
         * cookie-path be the default-path."  The attribute is still
         * appended, so an unusable Path overrides an earlier usable one:
         * the last Path attribute is the one S5.3 uses. */
        a->have_path = 1;
        a->path_default = 0;
        if (value.n == 0 || value.p[0] != '/') {
            a->path_default = 1;
            return COOKIE_OK;
        }
        if (slice_has_unspellable(value)) return COOKIE_EHEADER;
        if (slice_copy(a->path, sizeof a->path, value) != COOKIE_OK)
            return COOKIE_ETOOLONG;
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "secure")) {          /* S5.2.5 */
        a->flags |= COOKIE_F_SECURE;
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "httponly")) {        /* S5.2.6 */
        a->flags |= COOKIE_F_HTTP_ONLY;
        return COOKIE_OK;
    }
    if (slice_eq_ci_lit(name, "samesite")) {        /* 6265bis S5.2.7 */
        if (slice_eq_ci_lit(value, "none")) a->samesite = COOKIE_SAMESITE_NONE;
        else if (slice_eq_ci_lit(value, "lax")) a->samesite = COOKIE_SAMESITE_LAX;
        else if (slice_eq_ci_lit(value, "strict")) a->samesite = COOKIE_SAMESITE_STRICT;
        return COOKIE_OK;
    }
    return COOKIE_OK;                               /* unknown: ignore */
}

/* RFC 6265 S5.2: the name-value pair, then the attribute algorithm. */
static int parse_set_cookie(const char *header, SetCookie *a) {
    Slice h, nv, rest;
    size_t n, i, semi;
    int rc;

    memset(a, 0, sizeof *a);
    n = strlen(header);

    /* S4.1.1's grammar excludes every CTL, and this jar rejects such a
     * field outright (see is_ctl: an additional restriction, not S5.2) */
    for (i = 0; i < n; i++)
        if (is_ctl((unsigned char)header[i])) return COOKIE_EHEADER;

    /* S5.2: split at the first ';' */
    h = slice_make(header, n);
    semi = n;
    for (i = 0; i < n; i++)
        if (h.p[i] == ';') { semi = i; break; }
    nv = slice_make(h.p, semi);
    if (semi < n) rest = slice_make(h.p + semi + 1, n - semi - 1);
    else rest = slice_make(h.p + n, 0);

    /* S5.2: a name-value-pair without '=' is not a cookie */
    {
        size_t eq = nv.n;
        Slice name, value;
        for (i = 0; i < nv.n; i++)
            if (nv.p[i] == '=') { eq = i; break; }
        if (eq == nv.n) return COOKIE_EHEADER;

        /* S5.2: split at the first '=' */
        name = slice_trim(slice_make(nv.p, eq));
        value = slice_trim(slice_make(nv.p + eq + 1, nv.n - eq - 1));

        /* S5.2: leading and trailing WSP of both was removed by the trim */
        /* S5.2: an empty name is not a cookie */
        if (name.n == 0) return COOKIE_ENAME;

        /* A byte with no spelling in the text form (a C0 control, HTAB or
         * DEL) is not a cookie-octet either, so the field is not a
         * set-cookie-string; refusing it keeps "a stored cookie can
         * always be written back" true. */
        if (slice_has_unspellable(name) || slice_has_unspellable(value))
            return COOKIE_EHEADER;

        rc = slice_copy(a->name, sizeof a->name, name);
        if (rc != COOKIE_OK) return rc;
        rc = slice_copy(a->value, sizeof a->value, value);
        if (rc != COOKIE_OK) return rc;
    }

    /* The attribute loop: S5.2's algorithm, which discards a ';' and
     * then consumes up to the next one for every cookie-av.  An empty
     * cookie-av has an empty attribute-name and is ignored. */
    while (rest.n > 0) {
        Slice av, name, value;
        size_t k = 0, eq;

        while (k < rest.n && rest.p[k] != ';') k++;
        av = slice_make(rest.p, k);
        if (k < rest.n) { rest.p += k + 1; rest.n -= k + 1; }
        else { rest.n = 0; }

        eq = av.n;
        for (i = 0; i < av.n; i++)
            if (av.p[i] == '=') { eq = i; break; }
        name = slice_trim(slice_make(av.p, eq));
        value = (eq < av.n) ? slice_trim(slice_make(av.p + eq + 1, av.n - eq - 1))
                            : slice_make(av.p + av.n, 0);

        rc = parse_av(a, name, value);
        if (rc != COOKIE_OK) return rc;
    }
    return COOKIE_OK;
}

/* ============================================================
 *  7. The storage model - RFC 6265 S5.3
 * ============================================================ */

static int jar_n(const CookieJar *jar) {
    if (!jar || jar->count <= 0) return 0;
    return jar->count > COOKIE_JAR_SLOTS ? COOKIE_JAR_SLOTS : jar->count;
}

void cookie_jar_init(CookieJar *jar) {
    if (!jar) return;
    memset(jar, 0, sizeof *jar);
    jar->next_creation = 1;
}

void cookie_jar_clear(CookieJar *jar) {
    cookie_jar_init(jar);
}

int cookie_jar_count(const CookieJar *jar) {
    return jar_n(jar);
}

const CookieEntry *cookie_jar_at(const CookieJar *jar, int index) {
    if (!jar || index < 0 || index >= jar_n(jar)) return NULL;
    return &jar->slot[index];
}

static void jar_remove_at(CookieJar *jar, int i) {
    int n = jar_n(jar);
    if (i < 0 || i >= n) return;
    if (i + 1 < n) {
        memmove(&jar->slot[i], &jar->slot[i + 1],
                (size_t)(n - i - 1) * sizeof jar->slot[0]);
    }
    jar->count = n - 1;
    memset(&jar->slot[jar->count], 0, sizeof jar->slot[0]);
}

static int jar_find_key(const CookieJar *jar, const char *name,
                        const char *domain, const char *path) {
    int i, n = jar_n(jar);
    for (i = 0; i < n; i++) {
        const CookieEntry *e = &jar->slot[i];
        if (strcmp(e->name, name) == 0 &&
            strcmp(e->domain, domain) == 0 &&
            strcmp(e->path, path) == 0)
            return i;
    }
    return -1;
}

int cookie_jar_find(const CookieJar *jar, const char *name,
                    const char *domain, const char *path) {
    if (!jar || !name || !domain || !path) return -1;
    return jar_find_key(jar, name, domain, path);
}

int cookie_jar_remove(CookieJar *jar, const char *name,
                      const char *domain, const char *path) {
    int i;
    if (!jar || !name || !domain || !path) return 0;
    i = jar_find_key(jar, name, domain, path);
    if (i < 0) return 0;
    jar_remove_at(jar, i);
    return 1;
}

int cookie_jar_purge_expired(CookieJar *jar, int64_t now) {
    int i = 0, gone = 0;
    if (!jar) return COOKIE_EINVAL;
    while (i < jar_n(jar)) {
        if (jar->slot[i].expiry <= now) {
            jar_remove_at(jar, i);
            gone++;
        } else {
            i++;
        }
    }
    return gone;
}

/* True when a expires later than b; the tie-breaks make it a total
 * order, which is what keeps eviction deterministic. */
static int entry_later(const CookieEntry *a, const CookieEntry *b) {
    if (a->expiry != b->expiry) return a->expiry > b->expiry;
    if (a->last_access != b->last_access) return a->last_access > b->last_access;
    return a->creation > b->creation;
}

/* RFC 6265 S5.3 "removing excess cookies":
 *   1. expired cookies            - cookie_jar_purge_expired()
 *   2. cookies sharing an over-quota domain
 *   3. any cookie
 * and for equal priority, "the cookie with the earliest last-access
 * date".  This jar orders by expiry first, because that is the rule the
 * system asked for and the one a cache wants: the cookie that would
 * disappear soonest anyway is the cheapest to lose.  A session cookie's
 * expiry is COOKIE_TIME_SESSION, the latest representable date, so a
 * session cookie is only evicted when nothing persistent is left.
 *
 * `domain` restricts the search to one domain (NULL means any). */
static int jar_victim(const CookieJar *jar, const char *domain) {
    int i, best = -1, n = jar_n(jar);
    for (i = 0; i < n; i++) {
        if (domain && strcmp(jar->slot[i].domain, domain) != 0) continue;
        if (best < 0 || entry_later(&jar->slot[best], &jar->slot[i])) best = i;
    }
    return best;
}

/* S5.2.2: "the current date and time plus delta-seconds seconds", with
 * a zero or negative delta meaning the earliest representable date. */
static int64_t expiry_from_max_age(int64_t now, int64_t delta) {
    if (delta <= 0) return COOKIE_TIME_EARLIEST;
    if (now > COOKIE_TIME_LATEST_PERSISTENT - delta) return COOKIE_TIME_LATEST_PERSISTENT;
    if (now < COOKIE_TIME_EARLIEST + delta) return COOKIE_TIME_EARLIEST;
    return now + delta;
}

static void field_copy(char *dst, const char *src) {
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
}

int cookie_jar_store(CookieJar *jar, const char *set_cookie,
                     const char *host, const char *request_path,
                     int64_t now) {
    SetCookie sc;
    CookieEntry e;
    char chost[COOKIE_MAX_DOMAIN];
    char cpath[COOKIE_MAX_PATH];
    int rc, idx, i, dom_count;

    if (!jar || !set_cookie) return COOKIE_EINVAL;

    rc = cookie_canonical_host(host, chost, sizeof chost);
    if (rc != COOKIE_OK) return rc;

    rc = parse_set_cookie(set_cookie, &sc);
    if (rc != COOKIE_OK) return rc;

    /* 6265bis S5.4.7: a cookie with SameSite=None that is not Secure is
     * not stored at all.  This is the only SameSite rule that belongs to
     * storage; the request-side rules need site context the jar lacks. */
    if (sc.samesite == COOKIE_SAMESITE_NONE && !(sc.flags & COOKIE_F_SECURE))
        return COOKIE_EINSECURE;

    memset(&e, 0, sizeof e);
    e.flags = sc.flags;
    e.samesite = sc.samesite;
    field_copy(e.name, sc.name);
    field_copy(e.value, sc.value);
    /* S5.3: "Set the creation-time and the last-access-time to the
     * current date and time."  `creation` is a strictly increasing
     * sequence number instead of a date (cookie.h explains why);
     * last-access is a real clock reading, and it must start at the
     * store time, not at zero: a cookie that is stored and never sent
     * still has a last-access-time, and the eviction tie-break and the
     * text form both depend on it. */
    e.last_access = now;

    /* S5.3: Max-Age before Expires, and a session cookie only when
     * neither is present. */
    if (sc.have_max_age) e.expiry = expiry_from_max_age(now, sc.max_age);
    else if (sc.have_expires) e.expiry = sc.expires;
    else e.expiry = COOKIE_TIME_SESSION;
    if (e.expiry != COOKIE_TIME_SESSION) e.flags |= COOKIE_F_PERSISTENT;

    /* S5.3: the Domain attribute, or host-only. */
    if (sc.have_domain) {
        if (!sc.domain_ok) return COOKIE_EDOMAIN;
        if (!cookie_domain_match(sc.domain, chost)) return COOKIE_EDOMAIN;
        field_copy(e.domain, sc.domain);
    } else {
        e.flags |= COOKIE_F_HOST_ONLY;
        field_copy(e.domain, chost);
    }

    /* S5.2.4 with S5.3: the cookie path is the value of the LAST Path
     * attribute, and an empty or relative value means the request's
     * default-path (S5.1.4), not "keep the previous attribute". */
    if (sc.have_path && !sc.path_default) {
        field_copy(e.path, sc.path);
    } else {
        rc = cookie_default_path(request_path ? request_path : "", cpath, sizeof cpath);
        if (rc != COOKIE_OK) return rc;
        field_copy(e.path, cpath);
    }

    /* S5.3: "The user agent MUST evict all expired cookies ... if, at
     * any time, an expired cookie exists".  Doing it before the key
     * lookup below also means an expired predecessor cannot be mistaken
     * for a live one. */
    cookie_jar_purge_expired(jar, now);

    idx = jar_find_key(jar, e.name, e.domain, e.path);

    /* An already-expired cookie deletes the cookie it replaces and is
     * not stored (S5.2.2's earliest date, S5.3's eviction duty). */
    if (e.expiry <= now) {
        if (idx >= 0) jar_remove_at(jar, idx);
        return COOKIE_OK;
    }

    if (jar->next_creation < 1) jar->next_creation = 1;

    /* S5.3: a cookie with the same name, domain and path replaces the
     * old one and inherits its creation-time. */
    if (idx >= 0) {
        e.creation = jar->slot[idx].creation;
        jar_remove_at(jar, idx);
    } else {
        e.creation = jar->next_creation;
    }

    /* S6.1's per-domain number, enforced per S5.3's removal priority 2
     * before the global limit of priority 3. */
    dom_count = 0;
    for (i = 0; i < jar_n(jar); i++)
        if (strcmp(jar->slot[i].domain, e.domain) == 0) dom_count++;
    if (dom_count >= COOKIE_PER_DOMAIN_MAX) {
        int v = jar_victim(jar, e.domain);
        if (v >= 0) jar_remove_at(jar, v);
    }
    if (jar_n(jar) >= COOKIE_JAR_SLOTS) {
        int v = jar_victim(jar, NULL);
        if (v >= 0) jar_remove_at(jar, v);
    }
    i = jar_n(jar);
    if (i >= COOKIE_JAR_SLOTS) return COOKIE_ENOSPACE;
    jar->slot[i] = e;
    jar->count = i + 1;
    if (idx < 0) jar->next_creation++;
    return COOKIE_OK;
}

/* ============================================================
 *  8. The Cookie header - RFC 6265 S5.4
 * ============================================================ */

/* S5.4 step 2: "Cookies with longer paths are listed before cookies
 * with shorter paths.  Among cookies that have equal-length path
 * fields, cookies with earlier creation-times are listed before
 * cookies with later creation-times." */
static int entry_before(const CookieEntry *a, const CookieEntry *b) {
    size_t la = strlen(a->path), lb = strlen(b->path);
    if (la != lb) return la > lb;
    return a->creation < b->creation;
}

int cookie_jar_header(CookieJar *jar, const char *host, const char *request_path,
                      int secure, int is_http, int64_t now,
                      char *out, size_t cap, size_t *need) {
    char chost[COOKIE_MAX_DOMAIN];
    const char *rpath;
    int idx[COOKIE_JAR_SLOTS];
    int n = 0, i, j, k, rc;
    Out o;

    if (!jar) return COOKIE_EINVAL;
    if (cap > 0 && !out) return COOKIE_EINVAL;

    rc = cookie_canonical_host(host, chost, sizeof chost);
    if (rc != COOKIE_OK) return rc;
    rpath = (request_path && request_path[0]) ? request_path : "/";

    cookie_jar_purge_expired(jar, now);

    /* S5.4 step 1: host-only must match exactly, a domain cookie must
     * domain-match, then the path, the secure flag and the HTTP flag. */
    for (i = 0; i < jar_n(jar); i++) {
        const CookieEntry *e = &jar->slot[i];
        int host_ok;
        if (e->flags & COOKIE_F_HOST_ONLY) host_ok = strcmp(e->domain, chost) == 0;
        else host_ok = cookie_domain_match(e->domain, chost);
        if (!host_ok) continue;
        if (!cookie_path_match(e->path, rpath)) continue;
        if ((e->flags & COOKIE_F_SECURE) && !secure) continue;
        if ((e->flags & COOKIE_F_HTTP_ONLY) && !is_http) continue;
        idx[n++] = i;
    }

    /* S5.4 step 2, insertion sort: COOKIE_JAR_SLOTS is small and the
     * comparison is a total order, so this is stable and deterministic. */
    for (i = 1; i < n; i++) {
        int v = idx[i];
        for (j = i - 1; j >= 0 && entry_before(&jar->slot[v], &jar->slot[idx[j]]); j--)
            idx[j + 1] = idx[j];
        idx[j + 1] = v;
    }

    /* S5.4 steps 3 and 4 */
    out_init(&o, out, cap);
    for (k = 0; k < n; k++) {
        const CookieEntry *e = &jar->slot[idx[k]];
        if (k) out_cstr(&o, "; ");
        if (e->name[0]) {
            out_cstr(&o, e->name);
            out_ch(&o, '=');
        }
        out_cstr(&o, e->value);
        jar->slot[idx[k]].last_access = now;
    }
    return out_finish(&o, need);
}

/* ============================================================
 *  9. The text form
 *
 *  See cookie.h, "THE TEXT FORM", for the grammar and for why the
 *  decoder accepts only the exact bytes the encoder emits.
 * ============================================================ */

static const char k_magic[] = "mystic-cookie-jar ";
#define COOKIE_MAGIC_N (sizeof k_magic - 1)

/* Read one line, without its LF.  Returns 1 for a line, 0 at end of
 * input, -1 for a byte the canonical form cannot contain or for a final
 * line with no LF. */
static int read_line(const char *t, size_t len, size_t *pos, Slice *out) {
    size_t start = *pos, i = start;
    if (start >= len) return 0;
    while (i < len && t[i] != '\n') {
        unsigned char c = (unsigned char)t[i];
        if (c == 0x7F || (c < 0x20 && c != 0x09)) return -1;
        i++;
    }
    if (i >= len) return -1;
    *out = slice_make(t + start, i - start);
    *pos = i + 1;
    return 1;
}

/* TAB-separated fields of one line.  The last field is returned once
 * even when it is empty and even when the line has no TAB at all. */
typedef struct {
    const char *p;
    const char *end;
    int         done;
} Fields;

static int field_next(Fields *f, Slice *out) {
    const char *start;
    if (f->done) return 0;
    start = f->p;
    while (f->p < f->end && *f->p != '\t') f->p++;
    *out = slice_make(start, (size_t)(f->p - start));
    if (f->p == f->end) f->done = 1;
    else f->p++;
    return 1;
}

/* Strict decimal: optional '-', no leading zeros, no "-0", no '+', no
 * spaces.  That is a total function on the canonical text and rejects
 * every other spelling, which is what makes the round-trip exact. */
static int parse_i64(Slice s, int64_t *out) {
    uint64_t limit, v = 0;
    size_t i = 0;
    int neg = 0;

    if (s.n == 0) return 0;
    if (s.p[0] == '-') {
        neg = 1;
        i = 1;
        if (s.n == 1) return 0;
    }
    if (s.p[i] == '0' && (s.n - i > 1 || neg)) return 0;
    limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < s.n; i++) {
        if (!is_digit((unsigned char)s.p[i])) return 0;
        if (v > limit / 10u) return 0;
        v = v * 10u + (uint64_t)(s.p[i] - '0');
        if (v > limit) return 0;
    }
    if (neg) {
        if (v == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
        else *out = -(int64_t)v;
    } else {
        *out = (int64_t)v;
    }
    return 1;
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one field.  A raw byte must be printable ASCII other than '%',
 * and an escape must be '%' + two UPPERCASE hex digits naming a byte that
 * cannot be written raw: '%' itself or 0x80-0xFF.  A control byte, DEL
 * and every printable byte have no escaped spelling, so the decoder
 * accepts exactly the encoder's output and nothing else.  A NUL is in
 * that group too: it cannot live in a NUL-terminated field, and
 * RFC 6265 S4.1.1's cookie-octet excludes it. */
static int decode_field(Slice s, char *dst, size_t cap) {
    size_t i, o = 0;

    for (i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        unsigned char v;
        if (c == '%') {
            int hi, lo;
            if (i + 2 >= s.n) return COOKIE_ESERIAL;
            hi = hex_val((unsigned char)s.p[i + 1]);
            lo = hex_val((unsigned char)s.p[i + 2]);
            if (hi < 0 || lo < 0) return COOKIE_ESERIAL;
            v = (unsigned char)((hi << 4) | lo);
            /* only a byte that cannot be written raw may be escaped */
            if (!(v == '%' || v >= 0x80)) return COOKIE_ESERIAL;
            i += 2;
        } else if (c >= 0x20 && c <= 0x7E && c != '%') {
            v = c;
        } else {
            return COOKIE_ESERIAL;      /* raw CTL, DEL or high byte */
        }
        if (o + 1 >= cap) return COOKIE_ESERIAL;
        dst[o++] = (char)v;
    }
    dst[o] = 0;
    return COOKIE_OK;
}

/* One "C" record.  Everything the decoder can check, it checks. */
static int parse_record(Slice line, CookieEntry *e) {
    Fields f;
    Slice tag, s_creation, s_expiry, s_access, s_flags, s_samesite;
    Slice s_name, s_domain, s_path, s_value, extra;
    int64_t creation, expiry, access;
    int64_t flags, samesite;
    char canon[COOKIE_MAX_DOMAIN];
    int rc;

    f.p = line.p;
    f.end = line.p + line.n;
    f.done = 0;

    if (!field_next(&f, &tag) || tag.n != 1 || tag.p[0] != 'C') return COOKIE_ESERIAL;
    if (!field_next(&f, &s_creation)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_expiry)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_access)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_flags)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_samesite)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_name)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_domain)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_path)) return COOKIE_ESERIAL;
    if (!field_next(&f, &s_value)) return COOKIE_ESERIAL;
    if (field_next(&f, &extra)) return COOKIE_ESERIAL;   /* too many fields */

    if (!parse_i64(s_creation, &creation) || creation < 1) return COOKIE_ESERIAL;
    if (!parse_i64(s_expiry, &expiry)) return COOKIE_ESERIAL;
    if (!parse_i64(s_access, &access)) return COOKIE_ESERIAL;
    if (!parse_i64(s_flags, &flags) || flags < 0 || flags > 0x0F) return COOKIE_ESERIAL;
    if (!parse_i64(s_samesite, &samesite) || samesite < 0 ||
        samesite > COOKIE_SAMESITE_STRICT) return COOKIE_ESERIAL;

    memset(e, 0, sizeof *e);
    e->creation = creation;
    e->expiry = expiry;
    e->last_access = access;
    e->flags = (uint8_t)flags;
    e->samesite = (uint8_t)samesite;

    if (expiry != COOKIE_TIME_SESSION) {
        if (expiry > COOKIE_TIME_LATEST_PERSISTENT) return COOKIE_ESERIAL;
        if (!(e->flags & COOKIE_F_PERSISTENT)) return COOKIE_ESERIAL;
    } else {
        if (e->flags & COOKIE_F_PERSISTENT) return COOKIE_ESERIAL;
    }

    rc = decode_field(s_name, e->name, sizeof e->name);
    if (rc != COOKIE_OK) return COOKIE_ESERIAL;
    rc = decode_field(s_domain, e->domain, sizeof e->domain);
    if (rc != COOKIE_OK) return COOKIE_ESERIAL;
    rc = decode_field(s_path, e->path, sizeof e->path);
    if (rc != COOKIE_OK) return COOKIE_ESERIAL;
    rc = decode_field(s_value, e->value, sizeof e->value);
    if (rc != COOKIE_OK) return COOKIE_ESERIAL;

    /* The jar's invariants, so that matching and ordering stay defined. */
    if (e->name[0] == 0) return COOKIE_ESERIAL;
    if (e->path[0] != '/') return COOKIE_ESERIAL;
    if (cookie_canonical_host(e->domain, canon, sizeof canon) != COOKIE_OK)
        return COOKIE_ESERIAL;
    if (strcmp(canon, e->domain) != 0) return COOKIE_ESERIAL;
    return COOKIE_OK;
}

/* Is `jar` a state the text form can represent?  cookie_jar_serialise()
 * refuses a jar that is not, which is what makes
 *     deserialise(serialise(j)) == j
 * true for EVERY jar the encoder accepts: the encoder and the decoder
 * check the same list, so neither can produce a state the other refuses.
 * The list is the decoder's (cookie_jar_deserialise + parse_record) plus
 * the two field rules the decoder enforces while decoding. */
static int jar_invariants_ok(const CookieJar *jar) {
    int i, j, n;

    if (jar->count < 0 || jar->count > COOKIE_JAR_SLOTS) return 0;
    if (jar->next_creation < 1) return 0;   /* the file has no spelling for 0 */
    n = jar->count;

    for (i = 0; i < n; i++) {
        const CookieEntry *e = &jar->slot[i];
        char canon[COOKIE_MAX_DOMAIN];
        int same_domain = 0;

        if (e->name[0] == 0) return 0;
        if (e->path[0] != '/') return 0;
        if (e->flags & (uint8_t)~(COOKIE_F_HOST_ONLY | COOKIE_F_SECURE |
                                  COOKIE_F_HTTP_ONLY | COOKIE_F_PERSISTENT)) return 0;
        if (e->samesite > COOKIE_SAMESITE_STRICT) return 0;
        if (e->samesite == COOKIE_SAMESITE_NONE && !(e->flags & COOKIE_F_SECURE)) return 0;
        if (e->creation < 1 || e->creation >= jar->next_creation) return 0;
        if (e->expiry != COOKIE_TIME_SESSION) {
            if (e->expiry > COOKIE_TIME_LATEST_PERSISTENT) return 0;
            if (!(e->flags & COOKIE_F_PERSISTENT)) return 0;
        } else if (e->flags & COOKIE_F_PERSISTENT) {
            return 0;
        }
        if (cookie_canonical_host(e->domain, canon, sizeof canon) != COOKIE_OK)
            return 0;
        if (strcmp(canon, e->domain) != 0) return 0;
        if (cstr_has_unspellable(e->name) || cstr_has_unspellable(e->domain) ||
            cstr_has_unspellable(e->path) || cstr_has_unspellable(e->value)) return 0;

        for (j = 0; j < n; j++) {
            const CookieEntry *o = &jar->slot[j];
            if (o->creation == e->creation && j < i) return 0;
            if (j < i && strcmp(o->name, e->name) == 0 &&
                strcmp(o->domain, e->domain) == 0 &&
                strcmp(o->path, e->path) == 0) return 0;
            if (strcmp(o->domain, e->domain) == 0) same_domain++;
        }
        if (same_domain > COOKIE_PER_DOMAIN_MAX) return 0;
    }
    return 1;
}

int cookie_jar_serialise(const CookieJar *jar, char *out, size_t cap, size_t *need) {
    int i, n;
    Out o;

    if (!jar) return COOKIE_EINVAL;
    if (cap > 0 && !out) return COOKIE_EINVAL;
    if (!jar_invariants_ok(jar)) {
        if (cap > 0) out[0] = 0;
        if (need) *need = 0;
        return COOKIE_ESERIAL;
    }

    n = jar_n(jar);
    out_init(&o, out, cap);
    out_mem(&o, k_magic, COOKIE_MAGIC_N);
    out_ch(&o, '1');
    out_ch(&o, '\n');
    out_cstr(&o, "seq ");
    out_i64(&o, jar->next_creation);
    out_ch(&o, '\n');

    for (i = 0; i < n; i++) {
        const CookieEntry *e = &jar->slot[i];
        out_cstr(&o, "C\t");
        out_i64(&o, e->creation);
        out_ch(&o, '\t');
        out_i64(&o, e->expiry);
        out_ch(&o, '\t');
        out_i64(&o, e->last_access);
        out_ch(&o, '\t');
        out_i64(&o, (int64_t)e->flags);
        out_ch(&o, '\t');
        out_i64(&o, (int64_t)e->samesite);
        out_ch(&o, '\t');
        out_escaped(&o, e->name);
        out_ch(&o, '\t');
        out_escaped(&o, e->domain);
        out_ch(&o, '\t');
        out_escaped(&o, e->path);
        out_ch(&o, '\t');
        out_escaped(&o, e->value);
        out_ch(&o, '\n');
    }
    return out_finish(&o, need);
}

int cookie_jar_deserialise(CookieJar *jar, const char *text, size_t len) {
    Slice line;
    size_t pos = 0;
    int64_t seq = 0;
    int rc;

    if (!jar) return COOKIE_EINVAL;
    cookie_jar_clear(jar);
    if (!text) return COOKIE_EINVAL;

    /* header: "mystic-cookie-jar 1" */
    rc = read_line(text, len, &pos, &line);
    if (rc <= 0) return COOKIE_ESERIAL;
    if (line.n < COOKIE_MAGIC_N ||
        memcmp(line.p, k_magic, COOKIE_MAGIC_N) != 0) return COOKIE_ESERIAL;
    {
        Slice v = slice_make(line.p + COOKIE_MAGIC_N, line.n - COOKIE_MAGIC_N);
        if (v.n != 1 || v.p[0] != '1') return COOKIE_EVERSION;
    }

    /* sequence number */
    rc = read_line(text, len, &pos, &line);
    if (rc <= 0) return COOKIE_ESERIAL;
    if (line.n < 4 || memcmp(line.p, "seq ", 4) != 0) return COOKIE_ESERIAL;
    if (!parse_i64(slice_make(line.p + 4, line.n - 4), &seq) || seq < 1)
        return COOKIE_ESERIAL;

    /* records */
    while ((rc = read_line(text, len, &pos, &line)) == 1) {
        CookieEntry e;
        int j, same_domain = 0, dup_creation = 0;

        if (jar_n(jar) >= COOKIE_JAR_SLOTS) { cookie_jar_clear(jar); return COOKIE_ESERIAL; }
        if (parse_record(line, &e) != COOKIE_OK) { cookie_jar_clear(jar); return COOKIE_ESERIAL; }
        if (e.creation >= seq) { cookie_jar_clear(jar); return COOKIE_ESERIAL; }
        if (jar_find_key(jar, e.name, e.domain, e.path) >= 0) {
            cookie_jar_clear(jar);
            return COOKIE_ESERIAL;      /* the key must be unique */
        }
        /* The rest of what cookie_jar_store() guarantees, so that the
         * decoder accepts exactly the states the encoder can produce:
         *   - creation-time is unique (it orders equal-length paths);
         *   - a domain never holds more than COOKIE_PER_DOMAIN_MAX
         *     cookies (S6.1, and the store path evicts to enforce it);
         *   - SameSite=None always came with Secure (6265bis S5.4.7). */
        for (j = 0; j < jar_n(jar); j++) {
            if (jar->slot[j].creation == e.creation) dup_creation = 1;
            if (strcmp(jar->slot[j].domain, e.domain) == 0) same_domain++;
        }
        if (dup_creation || same_domain >= COOKIE_PER_DOMAIN_MAX) {
            cookie_jar_clear(jar);
            return COOKIE_ESERIAL;
        }
        if (e.samesite == COOKIE_SAMESITE_NONE && !(e.flags & COOKIE_F_SECURE)) {
            cookie_jar_clear(jar);
            return COOKIE_ESERIAL;
        }
        jar->slot[jar_n(jar)] = e;
        jar->count = jar_n(jar) + 1;
    }
    if (rc < 0) {
        cookie_jar_clear(jar);
        return COOKIE_ESERIAL;
    }
    jar->next_creation = seq;
    return COOKIE_OK;
}

/* ============================================================
 *  10. Diagnostics
 * ============================================================ */

const char *cookie_samesite_name(int samesite) {
    switch (samesite) {
    case COOKIE_SAMESITE_NONE:   return "None";
    case COOKIE_SAMESITE_LAX:    return "Lax";
    case COOKIE_SAMESITE_STRICT: return "Strict";
    default:                     return "Absent";
    }
}

const char *cookie_strerror(int err) {
    switch (err) {
    case COOKIE_OK:        return "ok";
    case COOKIE_EINVAL:    return "invalid argument";
    case COOKIE_EHEADER:   return "set-cookie value is not parseable (RFC 6265 S5.2)";
    case COOKIE_ENAME:     return "cookie name is empty (RFC 6265 S5.2)";
    case COOKIE_ETOOLONG:  return "a field or the output buffer is too small";
    case COOKIE_EHOST:     return "request host is not a canonical host (RFC 6265 S5.1.2)";
    case COOKIE_EDOMAIN:   return "Domain does not domain-match the host (RFC 6265 S5.3 step 4.1)";
    case COOKIE_EDATE:     return "not a cookie-date (RFC 6265 S5.1.1)";
    case COOKIE_EINSECURE: return "SameSite=None requires Secure (6265bis S5.4.7)";
    case COOKIE_ENOSPACE:  return "no free slot and nothing to evict";
    case COOKIE_ESERIAL:   return "cookie store text is corrupt";
    case COOKIE_EVERSION:  return "cookie store text has another version";
    default:               return "unknown cookie error";
    }
}
