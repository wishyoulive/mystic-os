/* ============================================================
 *  Mystic OS - net/cookie.h
 *
 *  An RFC 6265 cookie store: parse a Set-Cookie header, apply the
 *  storage model, emit the Cookie header, and save/load the whole
 *  jar as text.
 *
 *  Every rule below is quoted from a document, not invented:
 *
 *    RFC 6265   HTTP State Management Mechanism
 *      S4.1.1  Set-Cookie syntax and the cookie-octet grammar
 *      S4.1.2  Cookie header syntax
 *      S5.1.1  cookie-date parsing: the rfc1123-date, rfc850-date and
 *              asctime-date forms of RFC 2616 S3.3.1, tokenised by the
 *              date-token grammar and scanned by the flag algorithm
 *      S5.1.2  canonicalized host names
 *      S5.1.3  domain-match
 *      S5.1.4  the default-path algorithm and path-match
 *      S5.2    the Set-Cookie parsing algorithm and the attribute
 *              algorithms of S5.2.1-S5.2.6
 *      S5.3    the storage model: Max-Age before Expires, domain
 *              versus host-only, the default path, the
 *              (name, domain, path) replacement rule, creation-time
 *              inheritance, and the eviction duties
 *      S5.4    the Cookie header: selection, ordering by path length
 *              then creation-time, and serialisation
 *      S6.1    implementation limits (4096 bytes per cookie, 50
 *              cookies per domain, 3000 cookies in total)
 *
 *    RFC 1035  S2.3.4   labels of at most 63 octets, names of at most
 *                       255 octets.  Used as the host/domain bound.
 *    RFC 2616  S3.3.1   the three HTTP-date productions that
 *                       RFC 6265 S5.1.1 adopts by reference.
 *    RFC 5890 / RFC 3492  IDNA A-labels.  NOT implemented here; see
 *                       "WHAT THIS MODULE DOES NOT DO" below.
 *    draft-ietf-httpbis-rfc6265bis  SameSite.  RFC 6265 has no
 *                       SameSite attribute at all, so the three
 *                       SameSite rules in this file are quoted from the
 *                       bis draft and marked as such at every use.
 *
 *  WHAT THIS MODULE DOES NOT DO, AND WHY
 *
 *  - No public-suffix list.  RFC 6265 S5.3 step 3 rejects a Domain
 *    attribute that "is a public suffix", but only "if the user agent
 *    is configured to reject public suffixes"; the check needs the PSL,
 *    which is not part of any RFC and is not in this tree.  So
 *    "Domain=com" from "example.com" is stored, exactly as S5.1.3's
 *    domain-match algorithm says it must be.  Do not add a guess here:
 *    a wrong suffix table is worse than a documented absence.
 *  - No IDNA.  S5.1.2 canonicalises a host by converting each
 *    non-ASCII label to an A-label (RFC 5890 S2.3).  This module works
 *    on ASCII: a host or Domain containing a byte outside the ASCII
 *    set it accepts fails with COOKIE_EHOST instead of being mangled.
 *    net/url.c's url_host_to_ascii() is where the Punycode encoder
 *    lives (RFC 3492); a caller that accepts international host names
 *    canonicalises with that first and hands the A-label here.
 *  - No SameSite request policy.  The jar stores the attribute and
 *    reports it; whether a cross-site request may carry a Lax or
 *    Strict cookie depends on the request's site context, which the
 *    jar does not have.  The one SameSite rule that is a storage rule
 *    (None requires Secure) is enforced, because it decides whether
 *    the cookie is stored at all.
 *  - No clock of its own.  Every function that needs "now" takes it as
 *    an int64_t of seconds since the Unix epoch.  That is not a testing
 *    hook bolted on: the kernel, a unit test and the desktop all have
 *    different clock sources, and Max-Age arithmetic must be
 *    reproducible.  Nothing here calls time() or reads a global.
 *
 *  STORAGE POLICY, NOT PROTOCOL
 *
 *  RFC 6265 S6.1 says a general-use user agent SHOULD support at least
 *  4096 bytes per cookie, at least 50 cookies per domain and at least
 *  3000 cookies in total.  Those are SHOULD-level minima for a browser;
 *  this jar is a fixed-size object that a kernel or a desktop app can
 *  put on the stack or in a static, so the numbers below are a policy
 *  choice, stated here so a caller can see and change them:
 *
 *      COOKIE_PER_DOMAIN_MAX  50    - the RFC's own per-domain number
 *      COOKIE_JAR_SLOTS       64    - total slots (compile-time; build
 *                                     with -DCOOKIE_JAR_SLOTS=3000 to
 *                                     reach the S6.1 total)
 *      COOKIE_MAX_NAME       128    - bytes, one Set-Cookie name
 *      COOKIE_MAX_VALUE     2048    - bytes, one Set-Cookie value
 *      COOKIE_MAX_DOMAIN     256    - RFC 1035 S2.3.4 (255 + NUL)
 *      COOKIE_MAX_PATH      1024    - bytes, one cookie path
 *
 *  Overflowing a stored field's bound is COOKIE_ETOOLONG, never a
 *  truncated cookie: a cookie whose name is silently cut in half is a
 *  different cookie.  That covers the name, the value, the Path
 *  attribute and every output buffer.  A Domain attribute has no bound
 *  of its own to overflow: it is matched, not stored, so a Domain that
 *  is not a canonical host - including one longer than RFC 1035
 *  S2.3.4's 255 octets - makes the domain-match fail and is reported as
 *  COOKIE_EDOMAIN (S5.3 step 4.1 rejects the cookie).

 *  The 4096-byte S6.1 figure is the sum of name, value and attributes;
 *  the four string bounds above hold at most 127 + 2047 + 255 + 1023 =
 *  3452 bytes (each bound includes its NUL), so a cookie this jar
 *  accepts also fits the RFC's per-cookie minimum.
 *
 *  THE TEXT FORM
 *
 *  cookie_jar_serialise() writes a canonical text file:
 *
 *      mystic-cookie-jar 1
 *      seq <next-creation>
 *      C <creation> <expiry> <last-access> <flags> <samesite> <name>
 *        <domain> <path> <value>
 *
 *  with a TAB between the fields of a C record and a LF after every
 *  line.  Records appear in slot order, which is insertion order, so
 *  the same jar always produces the same bytes.
 *
 *  It round-trips byte for byte, and both halves of that are exact
 *  because the encoder validates before it writes.  For every jar
 *  cookie_jar_serialise() accepts,
 *
 *      deserialise(serialise(j)) == j
 *
 *  (same count, same sequence number, same entries in the same order,
 *  every field equal); and for every text t cookie_jar_deserialise()
 *  accepts,
 *
 *      serialise(deserialise(t)) == t
 *
 *  byte for byte.  A jar the text form cannot represent - a duplicate
 *  creation-time, more than COOKIE_PER_DOMAIN_MAX cookies on one domain,
 *  SameSite=None without Secure, a byte in a field with no spelling
 *  (a C0 control, HTAB, DEL or NUL), an empty name, a relative path, a
 *  domain that is not canonical, an unknown flag bit, a persistent bit
 *  that disagrees with the expiry, or a creation-time outside
 *  [1, next_creation) - is COOKIE_ESERIAL from cookie_jar_serialise(),
 *  not a file.  Encoder and decoder therefore accept exactly the same
 *  set of jars, and a build whose bounds differ from the writer's
 *  refuses the file rather than reinterpreting it (which is what the
 *  version line is for).
 *
 *  The decoder accepts only the bytes the encoder would emit:
 *
 *    - a byte of a name, domain, path or value is either printable
 *      ASCII 0x20-0x7E other than '%', or a '%' followed by two
 *      UPPERCASE hex digits naming '%' or a byte 0x80-0xFF.  Nothing
 *      else is accepted, so there is exactly one spelling for a jar;
 *    - a control byte or DEL has no spelling at all, in either
 *      direction.  RFC 6265 S4.1.1's cookie-octet production excludes
 *      every one of them, so a Set-Cookie carrying one is not a
 *      set-cookie-string; this module rejects such a header as an extra
 *      restriction of the kind S5 allows, and refuses to store or save
 *      a field that holds one.  (S5.2's own parse algorithm is more
 *      permissive than the grammar and does not check for CTLs.)
 *    - a NUL byte has no spelling either: it cannot live in a
 *      NUL-terminated field, and S4.1.1 excludes it;
 *    - flags and samesite are validated against the value range, the
 *      persistent bit must agree with the expiry field, the domain
 *      must already be canonical, the path must start with '/', the
 *      (name, domain, path) key must be unique, and the sequence
 *      number must be greater than every creation-time in the file;
 *    - creation-times must be unique (they order equal-length paths),
 *      and no domain may hold more than COOKIE_PER_DOMAIN_MAX cookies
 *      (S6.1), which is what the store path enforces.
 *
 *  A file that breaks any of those rules is COOKIE_ESERIAL (or
 *  COOKIE_EVERSION for a different header line) and the jar is left
 *  empty, never half-loaded.  A jar that breaks them is COOKIE_ESERIAL
 *  from cookie_jar_serialise(), so it is never written.
 * ============================================================ */
#ifndef MYSTIC_NET_COOKIE_H
#define MYSTIC_NET_COOKIE_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------
 *  Storage bounds.  See "STORAGE POLICY, NOT PROTOCOL" above.
 * ------------------------------------------------------------ */
#ifndef COOKIE_JAR_SLOTS
#define COOKIE_JAR_SLOTS        64
#endif
#define COOKIE_PER_DOMAIN_MAX   50     /* RFC 6265 S6.1               */

#define COOKIE_MAX_NAME        128
#define COOKIE_MAX_VALUE      2048
#define COOKIE_MAX_DOMAIN      256     /* RFC 1035 S2.3.4: 255 + NUL  */
#define COOKIE_MAX_PATH       1024

#if COOKIE_JAR_SLOTS < 1
#error "COOKIE_JAR_SLOTS must be at least 1"
#endif
#if COOKIE_PER_DOMAIN_MAX < 1
#error "COOKIE_PER_DOMAIN_MAX must be at least 1"
#endif

/* The worst case Cookie header for a full jar: every cookie's name and
 * value, "=", and "; ".  A caller that cannot afford this on the stack
 * passes a smaller buffer and gets COOKIE_ETOOLONG plus the exact size
 * in *need (see cookie_jar_header). */
#define COOKIE_HEADER_MAX \
    ((size_t)COOKIE_JAR_SLOTS * (COOKIE_MAX_NAME + COOKIE_MAX_VALUE + 3) + 1)

/* The two sentinel expiry values.
 *
 *   COOKIE_TIME_EARLIEST  RFC 6265 S5.2.2: a Max-Age of zero or less
 *                         sets "the earliest representable date and
 *                         time".  It is also what an already-expired
 *                         Expires date produces, and storing such a
 *                         cookie deletes the one it replaces (S5.3).
 *   COOKIE_TIME_SESSION   the latest representable date, which is the
 *                         expiry-time S5.3 assigns when neither Max-Age
 *                         nor Expires is present.  A cookie with this
 *                         expiry is a session cookie and has the
 *                         persistent flag clear.
 *
 * COOKIE_TIME_LATEST_PERSISTENT is INT64_MAX - 1 rather than INT64_MAX
 * so that the two are distinguishable: a persistent cookie can never
 * carry the session sentinel, which is what makes the text form's
 * "persistent bit agrees with the expiry" check possible.
 */
#define COOKIE_TIME_EARLIEST          INT64_MIN
#define COOKIE_TIME_SESSION           INT64_MAX
#define COOKIE_TIME_LATEST_PERSISTENT (INT64_MAX - 1)

/* ------------------------------------------------------------
 *  SameSite (draft-ietf-httpbis-rfc6265bis S5.2.7).  RFC 6265 has no
 *  SameSite attribute; the values are the draft's three keywords.
 *  COOKIE_SAMESITE_ABSENT means the attribute was not present, which
 *  the draft calls the "Default" behaviour.  The jar records the
 *  keyword and lets the request layer decide what "Default" means.
 * ------------------------------------------------------------ */
enum {
    COOKIE_SAMESITE_ABSENT = 0,
    COOKIE_SAMESITE_NONE   = 1,
    COOKIE_SAMESITE_LAX    = 2,
    COOKIE_SAMESITE_STRICT = 3
};

/* CookieEntry.flags bits.  Only these four bits exist; a text record
 * with any other bit set is corrupt (COOKIE_ESERIAL). */
#define COOKIE_F_HOST_ONLY  0x01u   /* S5.3 step 4.2: no Domain attribute */
#define COOKIE_F_SECURE     0x02u   /* S5.2.5                            */
#define COOKIE_F_HTTP_ONLY  0x04u   /* S5.2.6                            */
#define COOKIE_F_PERSISTENT 0x08u   /* S5.3 step 1: has an expiry date    */

/* ------------------------------------------------------------
 *  Return codes.  Zero is success; every negative value names exactly
 *  one reason and cookie_strerror() spells it out.
 *
 *  RFC 6265's "ignore the cookie entirely" is a negative code here,
 *  because a caller that logs policy rejections wants to see them.
 *  In every such case the jar is unchanged.
 * ------------------------------------------------------------ */
enum {
    COOKIE_OK        =   0,
    COOKIE_EINVAL    =  -1,   /* NULL argument, or a NULL out buffer     */
    COOKIE_EHEADER   =  -2,   /* not parseable as a Set-Cookie value:
                                 no '=' (S5.2), or a CTL character, which
                                 S4.1.1's grammar excludes and this jar
                                 rejects as an extra restriction (S5)     */
    COOKIE_ENAME     =  -3,   /* empty cookie-name (RFC 6265 S5.2)       */
    COOKIE_ETOOLONG  =  -4,   /* a field or an output buffer is too small */
    COOKIE_EHOST     =  -5,   /* request host is not a canonical host    */
    COOKIE_EDOMAIN   =  -6,   /* Domain does not domain-match the host,
                                 or is malformed (S5.3 step 4.1)         */
    COOKIE_EDATE     =  -7,   /* cookie_parse_date: not a cookie-date    */
    COOKIE_EINSECURE =  -8,   /* SameSite=None without Secure
                                 (6265bis S5.4.7)                        */
    COOKIE_ENOSPACE  =  -9,   /* no slot, and eviction could not free one */
    COOKIE_ESERIAL   = -10,   /* the text store is corrupt               */
    COOKIE_EVERSION  = -11    /* the text store has another version      */
};

/* ------------------------------------------------------------
 *  One stored cookie.  All strings are NUL-terminated and already
 *  canonical (domain lowercased without the leading dot, path
 *  absolute).  `creation` is not a clock reading: it is a strictly
 *  increasing sequence number drawn from CookieJar.next_creation.
 *
 *  RFC 6265 S5.4 step 2 orders equal-length paths by "creation-time".
 *  Creation-time in the RFC is a date, and two cookies stored in the
 *  same second would tie, leaving the order undefined.  A monotonic
 *  sequence is the same ordering at finer resolution: it agrees with
 *  the RFC whenever the dates differ, and it is deterministic when
 *  they do not.  `last_access` is a real clock reading: S5.3 sets it to
 *  the current date and time when the cookie is stored (not to zero),
 *  and S5.4 step 3 updates it whenever the cookie goes into a Cookie
 *  header.  It is the RFC's eviction tie-break (S5.3's removal order),
 *  so it has to be meaningful even for a cookie that was never sent.
 * ------------------------------------------------------------ */
typedef struct {
    char     name[COOKIE_MAX_NAME];
    char     value[COOKIE_MAX_VALUE];
    char     domain[COOKIE_MAX_DOMAIN];
    char     path[COOKIE_MAX_PATH];
    int64_t  expiry;        /* COOKIE_TIME_SESSION, or a Unix second  */
    int64_t  creation;      /* sequence number, >= 1                  */
    int64_t  last_access;   /* Unix second of the store or last send   */
    uint8_t  flags;         /* COOKIE_F_*                             */
    uint8_t  samesite;      /* COOKIE_SAMESITE_*                      */
} CookieEntry;

/* ------------------------------------------------------------
 *  The jar.  `count` slots are in use, slots [0, count) are in
 *  insertion order, and the rest are zeroed.  The struct has no
 *  pointers, so it can be copied with = and saved to disk as is.
 * ------------------------------------------------------------ */
typedef struct {
    CookieEntry slot[COOKIE_JAR_SLOTS];
    int         count;
    int64_t     next_creation;
} CookieJar;

/* ------------------------------------------------------------
 *  Whole-jar operations
 * ------------------------------------------------------------ */

/* Zero the jar and set the sequence to 1.  cookie_jar_init() is the way
 * to create a jar.  The store, header and purge paths also cope with an
 * all-zero struct (they treat next_creation < 1 as 1), but
 * cookie_jar_serialise() refuses one with COOKIE_ESERIAL, because the
 * text form has no spelling for a sequence number below 1 and inventing
 * one would break the round-trip. */
void cookie_jar_init(CookieJar *jar);
void cookie_jar_clear(CookieJar *jar);

int  cookie_jar_count(const CookieJar *jar);

/* The cookie at `index` in insertion order, or NULL.  Expiry is judged
 * only by the calls that are given `now` (store, header, purge): those
 * are the ones S5.3's eviction duty can act on, so an expired cookie may
 * still be visible here until one of them runs.  Treat a NULL return as
 * "no such cookie" and a non-NULL one as "stored", not as "live". */
const CookieEntry *cookie_jar_at(const CookieJar *jar, int index);

/* Index of the cookie with this (name, domain, path) key, or -1.
 * That triple is the key of RFC 6265 S5.3's replacement rule; host_only
 * is not part of it, so a Domain cookie replaces a host-only cookie with
 * the same name and path, exactly as the RFC says. */
int cookie_jar_find(const CookieJar *jar, const char *name,
                    const char *domain, const char *path);

/* Remove the cookie with this key.  Returns 1 when one was removed,
 * 0 when the jar had no such cookie. */
int cookie_jar_remove(CookieJar *jar, const char *name,
                      const char *domain, const char *path);

/* RFC 6265 S5.3: "The user agent MUST evict all expired cookies from
 * the cookie store if, at any time, an expired cookie exists".  Returns
 * the number evicted, or COOKIE_EINVAL. */
int cookie_jar_purge_expired(CookieJar *jar, int64_t now);

/* ------------------------------------------------------------
 *  Receiving a cookie
 * ------------------------------------------------------------ */

/* Parse one Set-Cookie header value and store it, as RFC 6265 S5.2
 * (parse) followed by S5.3 (storage) prescribe.
 *
 *   set_cookie    the field value only: "SID=31d4d96e407aad42; Path=/"
 *                 (no "Set-Cookie:" prefix, no CRLF)
 *   host          the request host, as the URL layer produced it:
 *                 lowercased, no trailing dot, no brackets.  It is
 *                 canonicalised again here (S5.1.2) so a caller that
 *                 passes "Example.COM." still gets the same cookie.
 *   request_path  the request-uri's path, or "" when it has none.
 *                 Only used for S5.1.4's default-path.
 *   now           Unix seconds.
 *
 * Returns COOKIE_OK when the cookie was stored (or when an expired one
 * deleted its predecessor), or one of the COOKIE_E* codes with the jar
 * untouched.  A Set-Cookie whose Expires cannot be parsed is not an
 * error: S5.2.1 says to ignore that attribute, so the cookie is stored
 * as a session cookie - it does not delete the cookie it replaces.
 *
 * A name, value or Path that holds a byte with no spelling in the text
 * form (a C0 control, HTAB, DEL) is COOKIE_EHEADER: S4.1.1's
 * cookie-octet admits no control byte and no whitespace, and storing
 * such a field would mean a cookie that could never be saved back.
 *
 * Two Domain readings are worth naming because the RFC states them in
 * different places:
 *   - a leading dot is dropped (S5.2.3) and the cookie becomes a domain
 *     cookie;
 *   - a trailing dot is NOT dropped.  The note in S4.1.2.3 (semantics
 *     prose, marked non-normative) says it "will cause the user agent to
 *     ignore the attribute", but the normative client algorithm is
 *     otherwise: S5.2.3 leaves the dot, S5.1.3 cannot domain-match it
 *     against a canonical host, and S5.3 step 4.1 rejects the cookie.
 *     This module follows the algorithms and reports COOKIE_EDOMAIN; a
 *     malformed Domain is never silently downgraded to host-only.
 *
 * The creation-time is a sequence number (see CookieEntry) and the
 * last-access-time is set to `now` (S5.3), so a stored cookie that is
 * never sent still has a meaningful last access.
 */
int cookie_jar_store(CookieJar *jar, const char *set_cookie,
                     const char *host, const char *request_path,
                     int64_t now);

/* ------------------------------------------------------------
 *  Sending cookies
 * ------------------------------------------------------------ */

/* Build the Cookie header value for one request: RFC 6265 S5.4 step 1
 * (host-only must match exactly, domain cookies must domain-match, the
 * path must path-match, Secure needs `secure`, HttpOnly needs
 * `is_http`), step 2 (longer paths first, then earlier creation), step
 * 3 (update last-access), step 4 (name=value, "; " between cookies).
 *
 * `secure` is 1 when the request-uri's scheme denotes a secure protocol
 * (S5.4 leaves that notion to the user agent; https and wss are the
 * two this system has).  `is_http` is 0 for a "non-HTTP" API, which
 * excludes HttpOnly cookies.
 *
 * The header value is written to `out` NUL-terminated, and *need (when
 * non-NULL) receives the number of bytes including the NUL, so a caller
 * can size a buffer exactly:
 *
 *     size_t need;
 *     cookie_jar_header(jar, h, p, s, 1, now, NULL, 0, &need);
 *     char *buf = malloc(need);
 *     cookie_jar_header(jar, h, p, s, 1, now, buf, need, NULL);
 *
 * When the buffer is too small the result is COOKIE_ETOOLONG, out[0] is
 * NUL (when cap > 0) and *need is the size that would have been needed.
 * An empty jar produces "" and COOKIE_OK.
 *
 * This function mutates the jar: S5.4 step 3's last-access update, and
 * the S5.3 duty to evict expired cookies first.  Pass a copy if that
 * matters.
 */
int cookie_jar_header(CookieJar *jar, const char *host, const char *request_path,
                      int secure, int is_http, int64_t now,
                      char *out, size_t cap, size_t *need);

/* ------------------------------------------------------------
 *  Saving and loading the jar as text (see "THE TEXT FORM")
 * ------------------------------------------------------------ */

/* Canonical text for the whole jar.  *need (when non-NULL) receives
 * the byte count including the NUL, on success and on COOKIE_ETOOLONG
 * alike.  out[0] is NUL when cap > 0 and the result is an error.
 *
 * A jar that the text form cannot represent is COOKIE_ESERIAL, with
 * out[0] NUL and *need 0: the list of refusals is in "THE TEXT FORM"
 * above.  cookie_jar_store() only ever builds jars that pass, so this
 * is a guard against a hand-built one, and it is what makes
 * deserialise(serialise(j)) == j hold for every jar this function
 * accepts. */
int cookie_jar_serialise(const CookieJar *jar, char *out, size_t cap,
                         size_t *need);

/* Parse text produced by cookie_jar_serialise().  On success the jar
 * holds exactly the saved cookies, with their creation order, creation
 * sequence and last-access times, so a subsequent Cookie header is
 * byte-identical to the one the saved jar would have produced.
 *
 * On failure the jar is left EMPTY (never half-loaded) and the result
 * is COOKIE_ESERIAL, COOKIE_EVERSION or COOKIE_EINVAL.  `len` excludes
 * any terminating NUL; use strlen() for a NUL-terminated file.
 */
int cookie_jar_deserialise(CookieJar *jar, const char *text, size_t len);

/* ------------------------------------------------------------
 *  The RFC 6265 subcomponent algorithms, exposed because they are
 *  testable on their own and because a request layer may want them.
 * ------------------------------------------------------------ */

/* S5.1.1 cookie-date -> Unix seconds.  Accepts the three forms of
 * RFC 2616 S3.3.1 by applying the tokeniser and the flag algorithm,
 * so the format name does not matter: "Sun, 06 Nov 1994 08:49:37 GMT",
 * "Sunday, 06-Nov-94 08:49:37 GMT" and "Sun Nov  6 08:49:37 1994" all
 * give 784111777.  Returns COOKIE_OK or COOKIE_EDATE.
 *
 * Faithful to the letter of the algorithm, including two consequences
 * worth knowing:
 *   - the final step requires the date to exist, so 31 April, 30
 *     February and 29 February 1900 are COOKIE_EDATE.  They are not
 *     rolled over into the next month, and they are not clamped: an
 *     Expires that names a day the month does not have is ignored by
 *     S5.2.1 like any other unparseable date;
 *   - the month production is the three-letter form, so "January" is
 *     not a month token and the whole date fails.
 */
int cookie_parse_date(const char *text, size_t len, int64_t *out);

/* S5.1.2, the ASCII part: lowercase, drop one trailing dot, and enforce
 * RFC 1035 S2.3.4's lengths (a label of 1..63 octets, a name of at most
 * 255).  The accepted bytes are letters, digits, '-', '.', '_' and ':';
 * anything else - including every non-ASCII byte and every CTL - is
 * COOKIE_EHOST.
 *
 * The character rules are deliberately wider than RFC 1035 S2.3.1's
 * "preferred name syntax": an underscore (which real SRV-style names
 * carry) and a leading or trailing hyphen are accepted, because
 * S5.1.2 canonicalises a host by lowercasing and joining labels and
 * imposes no character grammar of its own.  Rejecting them would break
 * hosts that resolve; accepting them cannot make two different hosts
 * compare equal, since the comparison is still exact or label-wise.
 *
 * A host containing ':' is treated as an IP literal and only lowercased
 * (its labels are not domain labels).  Returns COOKIE_OK, COOKIE_EHOST
 * or COOKIE_ETOOLONG (or COOKIE_EINVAL).  Non-ASCII input is
 * COOKIE_EHOST: see the IDNA note at the top of this file. */
int cookie_canonical_host(const char *host, char *out, size_t cap);

/* S5.1.3 domain-match, on already-canonical strings.  Returns 1 or 0.
 * The leading dot of a Domain attribute is removed by the caller
 * (S5.2.3); this function is the algorithm itself, so
 * cookie_domain_match(".example.com", "example.com") is 0.
 *
 * The suffix branch also requires the host to be a name, not an IP
 * address, so cookie_domain_match("0.0.1", "127.0.0.1") is 0 while
 * cookie_domain_match("1.2.3.4", "1.2.3.4") is 1 (the identical
 * branch carries no such condition, which is what lets a host-only
 * cookie on an IP literal work). */
int cookie_domain_match(const char *domain, const char *host);

/* S5.1.4 path-match.  Returns 1 or 0. */
int cookie_path_match(const char *cookie_path, const char *request_path);

/* S5.1.4 default-path: the cookie path to use when the Path attribute
 * is absent, or when its last occurrence was empty or relative
 * (S5.2.4).  "" and "foo" give "/", "/foo" gives "/", "/foo/"
 * gives "/foo", "/foo/bar" gives "/foo". */
int cookie_default_path(const char *request_path, char *out, size_t cap);

/* "None" / "Lax" / "Strict" / "Absent", for logs and for a text store
 * a human has to read. */
const char *cookie_samesite_name(int samesite);

/* One sentence for each COOKIE_E* code.  Never NULL. */
const char *cookie_strerror(int err);

#endif /* MYSTIC_NET_COOKIE_H */
