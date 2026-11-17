/* ============================================================
 *  Mystic OS - apps (external) / web
 *
 *  A real, small browser. It is an ordinary process like the other
 *  apps: it connects to the window server, owns a shared pixel
 *  buffer, and draws into it. Nothing here is compiled into the
 *  desktop.
 *
 *  What it actually does, in order:
 *
 *    1. parse the address you typed (scheme, host, port, path)
 *    2. getaddrinfo() the host, connect() with a bounded wait,
 *       send a real HTTP/1.1 GET, read the response into a byte
 *       buffer with a hard cap
 *    3. split the headers off the body, honour Content-Length,
 *       decode chunked transfer-encoding, follow one redirect
 *    4. turn the HTML into a list of laid-out text lines: tags
 *       are dropped but structure is kept (blocks break lines,
 *       h1..h6 get their own sizes, <li> gets a bullet, entities
 *       are decoded, whitespace collapses, <a href> is recorded)
 *    5. wrap those words to the window width with the *real*
 *       font metrics (font_width), never mid-word unless a word
 *       is wider than the whole line
 *    6. draw the lines, scroll them, and follow links you click
 *
 *  HTTPS is real. http:// and https:// take the same path through the
 *  HTTP code: only the transport differs. https:// runs over a TLS 1.2
 *  session (src/client/tls.c) with SNI, chain validation against the
 *  Mozilla roots in /etc/ssl/certs/ca-bundle.crt, a host-name check and
 *  an expiry check. A bare host name typed in the address bar means
 *  https://, exactly as it does in a modern browser. The title row shows
 *  a lock and the scheme, and the status bar shows the negotiated TLS
 *  version, cipher suite, certificate CN and expiry date.
 *
 *  It still has no JavaScript, no CSS and no images - it renders text,
 *  which is the honest description of what is implemented.
 *
 *  Keys
 *      Tab          focus / leave the address bar
 *      Enter        load what is in the address bar
 *      Backspace    back (in the page) / delete (in the address bar)
 *      j k  Up Down scroll a line
 *      PgUp PgDn    scroll a page (Space also pages down)
 *      Home End     top / bottom
 *      Ctrl+R       reload
 *      q            quit (when the address bar is not focused)
 *  Mouse
 *      click the bar to edit, click a numbered link to follow it,
 *      click the scroll bar to jump, and the wheel scrolls the page
 *      three lines per notch (positive = up, as the server sends it)
 *
 *  Every number shown at runtime is measured: byte counts come from
 *  the socket, status from the server's status line, milliseconds
 *  from clock_gettime(CLOCK_MONOTONIC).
 *
 *  Two switches exist for host harnesses, and nothing else:
 *
 *    WEB_HOST_TEST   builds only what decides *what the text says*
 *                    (address parsing, HTTP response parsing, the
 *                    fetch loop, HTML to text, layout), without the
 *                    window and without sockets.
 *    WEB_DRAW_ONLY   keeps the window, the drawing and the input
 *                    handling but drops the socket layer, web_load()
 *                    and main(), so the real window can be rendered to
 *                    an image on a host that has no sockets.
 *
 *  Neither is used by the OS build.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"
#include "../tls.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(WEB_HOST_TEST) && !defined(WEB_DRAW_ONLY)
/* The TLS implementation is a single translation unit (see tls.c). It is
 * pulled in only where the socket layer exists, so the host test builds
 * stay socket-free. */
#include "../tls.c"
#endif

#ifndef WEB_HOST_TEST
#include <signal.h>
#include <unistd.h>
#ifndef WEB_DRAW_ONLY            /* the socket layer's headers */
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
#endif

/* ------------------------------------------------------------
 * Limits. Every buffer in this file is one of these, and every
 * write into one is bounds checked - the network is not trusted.
 * ------------------------------------------------------------ */
#define WEB_RAW_MAX     (512 * 1024)  /* response bytes we will hold   */
#define WEB_PATH_MAX    768           /* an absolute address           */
#define WEB_HOST_MAX    256
#define WEB_TITLE_MAX   160
#define WEB_ERR_MAX     180
#define WEB_CHARSET_MAX 24
#define WEB_HIST_MAX    24

#define WEB_MAX_ATOMS   12000         /* words + link numbers          */
#define WEB_ATOM_TEXT   112           /* bytes in one atom             */
#define WEB_MAX_SEGS    30000         /* placed text runs              */
#define WEB_MAX_LINES   16000
#define WEB_MAX_LINKS   256

#define WEB_CONNECT_MS  6000          /* bounded connect()             */
#define WEB_IO_SECS     10            /* bounded send()/recv()         */

/* Window bands, in *design* pixels. Nothing here is used raw: every one
 * of them goes through ui_px() at the moment it is needed, and the two
 * bands that hold text are then measured from the fonts that sit in them
 * (see title_row_h()/field_h()/foot_h()). None of it is cached in a
 * static, so nothing here can go stale when the desktop's text scale
 * changes under the app. */
#define HEAD_GAP_D  6   /* space under the address field, in the band    */
#define ROW_PAD_D   4   /* above the title row and between the rows     */
#define ADDR_PAD_D  5   /* padding inside the address field             */
#define FOOT_PAD_D  4   /* padding in the status bar                    */
#define CGAP_D      4   /* gap between the header band and the page     */
#define SBAR_D      8   /* gutter on the right holding the scroll bar   */
#define MX_D       24   /* the one left/right margin of the whole window */
#define BADGE_H_D  16   /* design height of the scheme badge           */
#define BADGE_PAD_D 7   /* its horizontal padding                       */

/* The document's vertical rhythm, in design pixels. The parser stores
 * these on the atoms; layout applies the text scale when it places a
 * line, so a heading keeps its proportions when the desktop is zoomed.
 * One value above and one below every heading is what puts them on the
 * same grid, instead of the 10/7 px split the tags used to ask for. */
#define GAP_BLOCK_D       6   /* <p>, <div>, <ul> ...                 */
#define GAP_LI_D          2   /* list items follow each other closely */
#define GAP_HEAD_D       16   /* space above any heading              */
#define GAP_AFTER_HEAD_D 10   /* space below any heading              */

/* One wheel notch. Body lines, so it follows the text scale with the
 * fonts instead of being a fixed pixel distance. */
#define WHEEL_LINES 3

/* Palette. A client app owns its pixels, so it carries its own
 * colours, picked to sit next to the desktop theme. */
#define C_BG_TOP  RGB(0x0B, 0x11, 0x1C)
#define C_BG_BOT  RGB(0x14, 0x1D, 0x2D)
#define C_TEXT    RGB(0xC6, 0xCE, 0xDC)
#define C_HEAD    RGB(0xE9, 0xEE, 0xF8)
#define C_LINK    RGB(0x6F, 0xB0, 0xFF)
#define C_LINKNUM RGB(0x51, 0x6D, 0x9E)
#define C_DIM     RGB(0x86, 0x90, 0xA4)
#define C_FAINT   RGB(0x5A, 0x65, 0x7A)
#define C_ERR     RGB(0xE5, 0x7A, 0x7A)
#define C_ACCENT  RGB(0x4F, 0xD1, 0xC5)
#define C_FIELD   RGB(0x07, 0x0C, 0x15)
#define C_EDGE    RGB(0x27, 0x33, 0x48)

/* evdev key codes: the window server forwards the raw code from the
 * kernel, so these values are the ABI and not an invention of ours.
 * (Characters arrive separately, in TEXT, already translated.) */
#define K_UP      103
#define K_DOWN    108
#define K_PAGEUP  104
#define K_PAGEDOWN 109
#define K_HOME    102
#define K_END     107
#define K_R       19

/* ------------------------------------------------------------
 * An address.
 * ------------------------------------------------------------ */
typedef struct {
    char scheme[8];                  /* "http" or "https"        */
    char host[WEB_HOST_MAX];
    char port[8];
    char path[WEB_PATH_MAX];         /* always starts with '/'   */
    int  https;
} Url;

/* ------------------------------------------------------------
 * A parsed HTTP response.
 * ------------------------------------------------------------ */
typedef struct {
    int  status;
    int  chunked;
    int  short_body;                 /* Content-Length promised more    */
    long content_length;             /* -1 when the header is absent */
    char location[WEB_PATH_MAX];
    char charset[WEB_CHARSET_MAX];
} HttpResponse;

/* ------------------------------------------------------------
 * Rendered text.
 *
 * Html text becomes Atoms (words), a width-independent description
 * of the document. doc_layout() turns atoms into Lines of Segs for
 * one particular pixel width, and that is what gets drawn. So a
 * resize re-lays-out without re-fetching or re-parsing.
 * ------------------------------------------------------------ */
#define AF_GLUE    1     /* sits tight against the previous atom  */
#define AF_BREAK   2     /* starts a new line                     */
#define AF_LINKNUM 4     /* a "[3]" link marker: drawn dim        */

typedef struct {
    char text[WEB_ATOM_TEXT];
    u8   level;                      /* 0 body, 1..6 heading          */
    u8   flags;                      /* AF_*                          */
    u8   gap;                        /* px of space above, with AF_BREAK */
    int16_t link;                    /* index into Doc.link, -1 none  */
} Atom;

typedef struct {
    u16 atom;                        /* index into Doc.atom           */
    u16 start;                       /* first byte inside atom->text  */
    u16 len;                         /* byte count                    */
    u16 x;                           /* px from the line's left edge  */
    u16 w;                           /* measured width in px          */
} Seg;

typedef struct {
    u32 first;                       /* first Seg                    */
    u32 y;                           /* px from the top of the document */
    u16 n;                           /* Seg count                    */
    u16 h;                           /* line height in px            */
    u16 gap;                         /* space above the line         */
} Line;

typedef struct {
    char url[WEB_PATH_MAX];
} Link;

typedef struct {
    Atom atom[WEB_MAX_ATOMS];
    int  natom;
    Seg  seg[WEB_MAX_SEGS];
    int  nseg;
    Line line[WEB_MAX_LINES];
    int  nline;
    Link link[WEB_MAX_LINKS];
    int  nlink;
    u32  doc_h;
    int  truncated;                  /* a cap was hit; say so in the footer */
} Doc;

/* How layout measures text. The app passes the real Font objects in;
 * the host harness passes the same font_width() with real Lato files,
 * so the wrap points it reports are the real ones. */
typedef struct {
    void *ctx;
    float (*text_w)(void *ctx, int level, const char *s, int n);
    float (*line_h)(void *ctx, int level);
} WebMetrics;

/* ------------------------------------------------------------
 * Small helpers.
 * ------------------------------------------------------------ */
static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int lower(int c) { return tolower((unsigned char)c); }

static int ci_eq(const char *a, long alen, const char *b) {
    long i;
    for (i = 0; i < alen; i++) {
        if (!b[i]) return 0;
        if (lower(a[i]) != lower(b[i])) return 0;
    }
    return b[i] == 0;
}

/* does hay[0..hlen) contain needle, ignoring case? */
static int ci_has(const char *hay, long hlen, const char *needle) {
    long nlen = (long)strlen(needle);
    if (nlen == 0 || hlen < nlen) return 0;
    for (long i = 0; i + nlen <= hlen; i++) {
        long k = 0;
        while (k < nlen && lower(hay[i + k]) == lower(needle[k])) k++;
        if (k == nlen) return 1;
    }
    return 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* ============================================================
 *  Addresses
 * ============================================================ */

/* Build "scheme://host[:port]path" from a parsed address. */
static void url_make(const Url *u, char *out, size_t osz) {
    const char *defport = u->https ? "443" : "80";
    if (!strcmp(u->port, defport))
        snprintf(out, osz, "%s://%s%s", u->scheme, u->host, u->path);
    else
        snprintf(out, osz, "%s://%s:%s%s", u->scheme, u->host, u->port, u->path);
}

/* Collapse "//", "." and ".." in a path, in place. Real pages link
 * with "../x", and following those must not send junk to the server. */
static void path_clean(char *p) {
    char out[WEB_PATH_MAX];
    size_t o = 0;
    const char *s = p;

    while (*s) {
        while (*s == '/') s++;
        const char *seg = s;
        while (*s && *s != '/') s++;
        size_t len = (size_t)(s - seg);
        if (len == 0) continue;
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            continue;
        }
        if (o + len + 2 >= sizeof(out)) break;
        out[o++] = '/';
        memcpy(out + o, seg, len);
        o += len;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    snprintf(p, WEB_PATH_MAX, "%s", out);
}

/* Parse what the user typed. Returns 0 and fills u, or -1 with a
 * message a person can act on. */
static int url_parse(const char *in, Url *u, char *err, size_t esz) {
    memset(u, 0, sizeof(*u));
    /* A bare host name means https, the way a modern browser reads it.
     * An explicit http:// switches to port 80 and plain HTTP. */
    snprintf(u->scheme, sizeof(u->scheme), "https");
    u->https = 1;
    snprintf(u->port, sizeof(u->port), "443");
    snprintf(u->path, sizeof(u->path), "/");

    const char *p = in;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        snprintf(err, esz, "the address bar is empty - Tab to it and type one");
        return -1;
    }

    const char *sep = strstr(p, "://");
    if (sep && sep - p < 8) {
        char sc[8];
        int n = (int)(sep - p);
        for (int i = 0; i < n; i++) sc[i] = (char)lower(p[i]);
        sc[n] = 0;
        if (!strcmp(sc, "http")) {
            u->https = 0;
            snprintf(u->port, sizeof(u->port), "80");
        } else if (!strcmp(sc, "https")) {
            u->https = 1;
            snprintf(u->port, sizeof(u->port), "443");
        } else {
            snprintf(err, esz, "%s:// is not supported - this browser speaks http:// and https://", sc);
            return -1;
        }
        snprintf(u->scheme, sizeof(u->scheme), "%s", sc);
        p = sep + 3;
    } else {
        /* No scheme. Accept "host[:port][/path]", but refuse anything
         * that looks like a different protocol rather than guessing. */
        const char *colon = strchr(p, ':');
        const char *slash = strchr(p, '/');
        if (colon && (!slash || colon < slash)) {
            int digits = 1;
            for (const char *q = colon + 1; *q && *q != '/'; q++)
                if (!isdigit((unsigned char)*q)) { digits = 0; break; }
            if (!digits) {
                snprintf(err, esz, "unsupported address - use http:// or https://");
                return -1;
            }
        }
    }

    const char *auth = p;
    const char *end = auth;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;

    /* user:pass@host - the credentials are not shown anywhere, so drop them */
    const char *at = NULL;
    for (const char *q = auth; q < end; q++)
        if (*q == '@') at = q;
    if (at) auth = at + 1;

    if (auth >= end) {
        snprintf(err, esz, "no host in the address");
        return -1;
    }
    if (*auth == '[') {
        snprintf(err, esz, "IPv6 literals are not supported - use a name or an IPv4 address");
        return -1;
    }

    const char *colon = NULL;
    for (const char *q = auth; q < end; q++)
        if (*q == ':') colon = q;
    const char *hostend = colon ? colon : end;
    if (hostend == auth) {
        snprintf(err, esz, "no host in the address");
        return -1;
    }
    if ((size_t)(hostend - auth) >= sizeof(u->host)) {
        snprintf(err, esz, "host name is too long (%d bytes)", (int)(hostend - auth));
        return -1;
    }
    memcpy(u->host, auth, (size_t)(hostend - auth));
    u->host[hostend - auth] = 0;
    for (const char *q = auth; q < hostend; q++) {
        unsigned char c = (unsigned char)*q;
        if (c <= 0x20 || c >= 0x7F) {
            snprintf(err, esz, "bad host name in the address");
            return -1;
        }
    }

    if (colon) {
        size_t plen = (size_t)(end - colon - 1);
        if (plen == 0 || plen >= sizeof(u->port)) {
            snprintf(err, esz, "bad port number");
            return -1;
        }
        for (size_t i = 0; i < plen; i++) {
            if (!isdigit((unsigned char)colon[1 + i])) {
                snprintf(err, esz, "bad port number");
                return -1;
            }
        }
        memcpy(u->port, colon + 1, plen);
        u->port[plen] = 0;
    }

    if (*end == 0 || *end == '#') {
        snprintf(u->path, sizeof(u->path), "/");
    } else if (*end == '?') {
        size_t n = 1;
        u->path[0] = '/';
        for (size_t i = 0; end[i] && n + 4 < sizeof(u->path); i++) {
            unsigned char c = (unsigned char)end[i];
            if (c <= 0x20 || c >= 0x7F) {
                snprintf(u->path + n, sizeof(u->path) - n, "%%%02X", c);
                n += 3;
            } else {
                u->path[n++] = (char)c;
            }
        }
        u->path[n] = 0;
    } else {
        /* Copy the path, encoding anything that cannot appear on a
         * request line (spaces, control bytes) as %XX. A raw space
         * would cut the request line in half. */
        size_t n = 0;
        if (*end != '/') u->path[n++] = '/';
        for (size_t i = 0; end[i] && end[i] != '#' && n + 4 < sizeof(u->path); i++) {
            unsigned char c = (unsigned char)end[i];
            if (c <= 0x20 || c >= 0x7F) {
                snprintf(u->path + n, sizeof(u->path) - n, "%%%02X", c);
                n += 3;
            } else {
                u->path[n++] = (char)c;
            }
        }
        u->path[n] = 0;
    }
    return 0;
}

/* The one rule about what this browser may fetch: http and https, and
 * nothing else. https is a real fetch through src/client/tls.c. Used by
 * the UI, by the loader and by the fetch loop, so none of them can
 * disagree about what is reachable. */
static int url_fetchable(const Url *u, char *err, size_t esz) {
    if (u->https || !strcmp(u->scheme, "https") || !strcmp(u->scheme, "http"))
        return 1;
    snprintf(err, esz, "%s:// is not supported - this browser speaks http:// and https://",
             u->scheme);
    return 0;
}

/* Resolve a link or a Location: header against the page it came from.
 * Returns 0 with out[0]==0 when the target is not something we can
 * fetch (fragment, mailto:, javascript:) - the caller just ignores it. */
static int url_resolve(const Url *base, const char *href, char *out, size_t osz,
                       char *err, size_t esz) {
    out[0] = 0;
    const char *p = href;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p || *p == '#') return 0;

    if (!strncmp(p, "//", 2)) {
        snprintf(out, osz, "%s:%s", base->scheme, p);
        return 0;
    }
    const char *sep = strstr(p, "://");
    if (sep && sep - p < 8) {
        char sc[8];
        int n = (int)(sep - p);
        for (int i = 0; i < n; i++) sc[i] = (char)lower(p[i]);
        sc[n] = 0;
        if (!strcmp(sc, "http") || !strcmp(sc, "https")) {
            snprintf(out, osz, "%s", p);
            return 0;
        }
        return 0;                       /* mailto:, ftp:, ... - not for us */
    }
    /* A "scheme:" we do not speak (mailto:, javascript:, ftp:) is
     * ignored rather than turned into a bogus hostname. A port is
     * digits, so "example.com:8080/x" still resolves. */
    const char *colon = strchr(p, ':');
    const char *first_slash = strchr(p, '/');
    if (colon && (!first_slash || colon < first_slash)) {
        int schemeish = (colon > p);
        for (const char *q = p; q < colon; q++)
            if (!isalpha((unsigned char)*q) && *q != '+' && *q != '-' && *q != '.')
                schemeish = 0;
        int digits = 1;
        for (const char *q = colon + 1; *q && *q != '/'; q++)
            if (!isdigit((unsigned char)*q)) { digits = 0; break; }
        if (schemeish && !digits) return 0;
    }

    char authority[WEB_HOST_MAX + 16];
    const char *defport = base->https ? "443" : "80";
    if (!strcmp(base->port, defport))
        snprintf(authority, sizeof(authority), "%s://%s", base->scheme, base->host);
    else
        snprintf(authority, sizeof(authority), "%s://%s:%s", base->scheme, base->host, base->port);

    char path[WEB_PATH_MAX];
    if (p[0] == '/') {
        snprintf(path, sizeof(path), "%s", p);
    } else {
        /* the directory part of the current path */
        const char *dir_end = strrchr(base->path, '/');
        int dlen = dir_end ? (int)(dir_end - base->path) + 1 : 1;
        if (dlen >= (int)sizeof(path)) dlen = (int)sizeof(path) - 1;
        memcpy(path, base->path, (size_t)dlen);
        path[dlen] = 0;
        size_t have = (size_t)dlen;
        size_t i = 0;
        while (p[i] && have + 1 < sizeof(path)) path[have++] = p[i++];
        path[have] = 0;
    }
    path_clean(path);
    snprintf(out, osz, "%s%s", authority, path);
    (void)err;
    (void)esz;
    return 0;
}

/* ============================================================
 *  HTTP responses
 * ============================================================ */

/* Decode chunked transfer-encoding in place. Returns the decoded
 * length. Chunk extensions ("1a;foo=bar") and trailers are skipped;
 * a truncated transfer keeps whatever arrived, because showing the
 * first 500 KB of a page beats showing nothing.
 *
 * Writing always trails reading (out <= in), so doing it in place is
 * safe and costs no second buffer. */
static long dechunk(char *p, long n) {
    long in = 0, out = 0;
    while (in < n) {
        long le = in;
        while (le < n && p[le] != '\n') le++;
        if (le >= n) break;

        long i = in;
        long size = 0;
        int digits = 0;
        while (i < le && p[i] != ';') {
            int c = (unsigned char)p[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            if (size > (0x7FFFFFFF - v) / 16) { size = 0x7FFFFFFF; break; }
            size = size * 16 + v;
            digits = 1;
            i++;
        }
        if (!digits) break;

        in = le + 1;
        if (size == 0) break;                       /* last chunk */
        if (in + size > n) size = n - in;           /* truncated on the wire */
        if (size > 0) memmove(p + out, p + in, (size_t)size);
        out += size;
        in += size;
        if (in + 1 < n && p[in] == '\r' && p[in + 1] == '\n') in += 2;
        else if (in < n && p[in] == '\n') in += 1;
    }
    return out;
}

/* Split a raw response. Returns 0 and fills r/body/blen, or -1 when
 * the bytes are not an HTTP response at all. */
static int http_parse(char *raw, long rawlen, HttpResponse *r, char **body, long *blen) {
    memset(r, 0, sizeof(*r));
    r->content_length = -1;

    long hdr_end = -1;
    long sep_len = 4;
    for (long i = 0; i + 3 < rawlen; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            hdr_end = i;
            sep_len = 4;
            break;
        }
        if (raw[i] == '\n' && raw[i + 1] == '\n') {
            hdr_end = i;
            sep_len = 2;
            break;
        }
    }
    if (hdr_end < 0) return -1;
    if (rawlen < 5 || strncmp(raw, "HTTP/", 5)) return -1;

    long sp = 5;
    while (sp < hdr_end && raw[sp] != ' ') sp++;
    if (sp >= hdr_end) return -1;
    r->status = atoi(raw + sp + 1);
    if (r->status < 100 || r->status > 599) return -1;

    /* position just past the status line: the first header */
    long pos = 0;
    while (pos < hdr_end && raw[pos] != '\n') pos++;
    pos++;

    while (pos < hdr_end) {
        long le = pos;
        while (le < hdr_end && raw[le] != '\n') le++;
        long len = le - pos;
        if (len > 0 && raw[pos + len - 1] == '\r') len--;
        const char *line = raw + pos;

        long c = 0;
        while (c < len && line[c] != ':') c++;
        if (c < len) {
            long v = c + 1;
            while (v < len && (line[v] == ' ' || line[v] == '\t')) v++;
            long vlen = len - v;
            if (ci_eq(line, c, "content-length")) {
                long total = 0;
                int ok = 0;
                for (long i = v; i < len; i++) {
                    if (!isdigit((unsigned char)line[i])) break;
                    if (total > (0x7FFFFFFF - 9) / 10) { total = 0x7FFFFFFF; break; }
                    total = total * 10 + (line[i] - '0');
                    ok = 1;
                }
                if (ok) r->content_length = total;
            } else if (ci_eq(line, c, "transfer-encoding")) {
                if (ci_has(line + v, vlen, "chunked")) r->chunked = 1;
            } else if (ci_eq(line, c, "location")) {
                long n = vlen;
                if (n >= (long)sizeof(r->location)) n = (long)sizeof(r->location) - 1;
                memcpy(r->location, line + v, (size_t)n);
                r->location[n] = 0;
            } else if (ci_eq(line, c, "content-type")) {
                /* charset=... decides whether high bytes are Latin-1 */
                for (long i = v; i + 8 <= len; i++) {
                    if (ci_has(line + i, 8, "charset=")) {
                        long q = i + 8;
                        long e = q;
                        while (e < len && line[e] != ';' && line[e] != ' ' && line[e] != '\r') e++;
                        long n = e - q;
                        if (n > 0 && n < (long)sizeof(r->charset)) {
                            for (long k = 0; k < n; k++)
                                r->charset[k] = (char)lower(line[q + k]);
                            r->charset[n] = 0;
                        }
                        break;
                    }
                }
            }
        }
        pos = le + 1;
    }

    char *b = raw + hdr_end + sep_len;
    long bl = rawlen - (hdr_end + sep_len);
    if (bl < 0) bl = 0;
    if (r->chunked) {
        bl = dechunk(b, bl);
    } else if (r->content_length >= 0) {
        if (bl > r->content_length) {
            bl = r->content_length;  /* the server may keep the socket open */
        } else if (bl < r->content_length) {
            /* The peer closed before sending what it promised. The bytes
             * that did arrive are real, but the document is not whole, so
             * the caller must say so instead of passing it off as a page. */
            r->short_body = 1;
        }
    }
    *body = b;
    *blen = bl;
    return 0;
}

/* ============================================================
 *  The transport
 *
 *  Four socket-shaped operations plus one question ("is this
 *  connection private?"). There is exactly one implementation, over
 *  a TCP socket, and it picks plain HTTP or TLS 1.2 from the scheme
 *  of the address it is handed: https goes through src/client/tls.c,
 *  which refuses to complete the handshake unless the chain, the name
 *  and the dates all check out. Everything above this struct - the
 *  request, the header parser, Content-Length, chunked, redirects -
 *  is therefore identical for http:// and https://.
 * ============================================================ */
typedef struct WebStream WebStream;

typedef struct {
    WebStream *(*open)(const char *host, const char *port, int https,
                       char *err, size_t esz);
    int   (*send)(WebStream *s, const char *buf, long len, char *err, size_t esz);
    long  (*recv)(WebStream *s, char *buf, long cap, char *err, size_t esz);
    void  (*close)(WebStream *s);
    /* Facts from the handshake, or NULL when the stream is plain HTTP. */
    const TlsInfo *(*tls)(WebStream *s);
} WebTransport;

#if !defined(WEB_HOST_TEST) && !defined(WEB_DRAW_ONLY)
struct WebStream {
    int fd;
    TlsConn *tls;                /* NULL: the bytes are plain HTTP      */
};

/* An errno a person can read. */
static const char *net_reason(int e) {
    switch (e) {
        case ECONNREFUSED: return "connection refused";
        case ETIMEDOUT:    return "connection timed out";
        case ENETUNREACH:  return "network unreachable";
        case EHOSTUNREACH: return "host unreachable";
        case ECONNRESET:   return "connection reset by the server";
        case EPIPE:        return "connection closed by the server";
        case EAGAIN:       return "timed out waiting for the server";
        default:           return NULL;
    }
}

static void put_net_error(char *err, size_t esz, const char *what, int e) {
    const char *why = net_reason(e);
    if (why) snprintf(err, esz, "%s: %s", what, why);
    else     snprintf(err, esz, "%s: %s", what, strerror(e));
}

static WebStream *tcp_open(const char *host, const char *port, char *err, size_t esz) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        if (rc == EAI_NONAME || rc == EAI_NODATA)
            snprintf(err, esz, "no such host: %s", host);
        else if (rc == EAI_AGAIN)
            snprintf(err, esz, "name lookup timed out: %s", host);
        else if (rc == EAI_SYSTEM)
            put_net_error(err, esz, host, errno);
        else if (rc == EAI_MEMORY)
            snprintf(err, esz, "out of memory looking up %s", host);
        else
            snprintf(err, esz, "cannot look up %s: %s", host, gai_strerror(rc));
        return NULL;
    }

    int fd = -1;
    int last = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last = errno; continue; }

        /* A bounded connect: a host that never answers must not freeze
         * the window forever. */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            int pr = poll(&p, 1, WEB_CONNECT_MS);
            if (pr > 0) {
                int so = 0;
                socklen_t sl = sizeof(so);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) < 0) so = errno;
                if (so == 0) r = 0;
                else { errno = so; r = -1; }
            } else {
                errno = (pr == 0) ? ETIMEDOUT : errno;
                r = -1;
            }
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, flags);
            struct timeval tv;
            tv.tv_sec = WEB_IO_SECS;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            last = 0;
            break;
        }
        last = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        put_net_error(err, esz, host, last ? last : ECONNREFUSED);
        return NULL;
    }
    WebStream *s = calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        snprintf(err, esz, "out of memory");
        return NULL;
    }
    s->fd = fd;
    return s;
}

/* ------------------------------------------------------------
 * TLS
 *
 * tls.c never touches a socket: it asks these two functions for
 * bytes and for room to put bytes. Both are one syscall, retried on
 * EINTR, because the socket already carries SO_RCVTIMEO/SO_SNDTIMEO
 * from tcp_open() - a server that stops talking cannot wedge the
 * window forever.
 * ------------------------------------------------------------ */
static long tls_bio_read(void *ctx, void *buf, size_t len) {
    WebStream *s = ctx;
    for (;;) {
        ssize_t n = recv(s->fd, buf, len, 0);
        if (n >= 0) return (long)n;
        if (errno == EINTR) continue;
        return -1;
    }
}

static long tls_bio_write(void *ctx, const void *buf, size_t len) {
    WebStream *s = ctx;
    for (;;) {
        ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return (long)n;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Open the transport the address asks for. For https the handshake
 * runs to completion here, so a bad certificate is reported before a
 * single byte of the request is written. */
static WebStream *net_open(const char *host, const char *port, int https,
                           char *err, size_t esz) {
    WebStream *s = tcp_open(host, port, err, esz);
    if (!s) return NULL;
    if (!https) return s;

    char terr[WEB_ERR_MAX];
    terr[0] = 0;
    s->tls = tls_connect(s, tls_bio_read, tls_bio_write, host, terr, sizeof(terr));
    if (!s->tls) {
        snprintf(err, esz, "%s", terr[0] ? terr : "TLS handshake failed");
        close(s->fd);
        free(s);
        return NULL;
    }
    return s;
}

static int net_send(WebStream *s, const char *buf, long len, char *err, size_t esz) {
    if (s->tls) {
        char terr[WEB_ERR_MAX];
        terr[0] = 0;
        if (tls_write(s->tls, buf, (size_t)len, terr, sizeof(terr)) != 0) {
            snprintf(err, esz, "%s", terr[0] ? terr : "cannot send over TLS");
            return -1;
        }
        return 0;
    }
    long off = 0;
    while (off < len) {
        ssize_t n = send(s->fd, buf + off, (size_t)(len - off), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            put_net_error(err, esz, "cannot send the request", errno);
            return -1;
        }
        off += (long)n;
    }
    return 0;
}

static long net_recv(WebStream *s, char *buf, long cap, char *err, size_t esz) {
    if (s->tls) {
        char terr[WEB_ERR_MAX];
        terr[0] = 0;
        long n = tls_read(s->tls, buf, (size_t)cap, terr, sizeof(terr));
        if (n < 0) {
            snprintf(err, esz, "%s", terr[0] ? terr : "cannot read over TLS");
            return -1;
        }
        return n;                     /* 0 is the peer's close_notify */
    }
    for (;;) {
        ssize_t n = recv(s->fd, buf, (size_t)cap, 0);
        if (n > 0) return (long)n;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        put_net_error(err, esz, "reading the response", errno);
        return -1;
    }
}

static void net_close(WebStream *s) {
    if (!s) return;
    if (s->tls) tls_close(s->tls);    /* sends close_notify, best effort */
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static const TlsInfo *net_tlsinfo(WebStream *s) {
    return (s && s->tls) ? tls_info(s->tls) : NULL;
}

static const WebTransport net_transport = {
    net_open, net_send, net_recv, net_close, net_tlsinfo
};
#endif /* sockets */

/* ============================================================
 *  Fetching
 * ============================================================ */
static int build_request(char *out, size_t cap, const Url *u) {
    char host_hdr[WEB_HOST_MAX + 16];
    /* The Host header carries a port only when it is not the scheme's
     * default: 80 for http, 443 for https. Same rule for both. */
    const char *defport = u->https ? "443" : "80";
    if (!strcmp(u->port, defport))
        snprintf(host_hdr, sizeof(host_hdr), "%s", u->host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%s", u->host, u->port);

    int n = snprintf(out, cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: mystic-web/1.0 (Mystic OS)\r\n"
        "Accept: text/html,text/plain;q=0.9,*/*;q=0.5\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        u->path, host_hdr);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Result of one navigation attempt. */
typedef struct {
    int   ok;
    char  error[WEB_ERR_MAX];
    int   status;
    long  raw_bytes;
    long  body_bytes;
    long  elapsed_ms;
    int   truncated;
    int   latin1;
    char  charset[WEB_CHARSET_MAX];
    char  url[WEB_PATH_MAX];
    char *body;                 /* points inside the caller's raw buffer */
    long  body_len;
    /* What TLS said, copied out of the session before it is closed.
     * tls is 0 for plain http, where these strings stay empty. */
    int   tls;
    char  tls_version[16];
    char  tls_cipher[48];
    char  tls_cn[128];
    char  tls_expiry[32];
} Fetch;

/* Fetch a page, following at most one redirect. `raw` holds the bytes
 * and must be WEB_RAW_MAX; the body pointer in the result points into
 * it and stays valid until the next fetch. */
static void web_fetch(Fetch *f, const WebTransport *tr, Url *u, char *raw) {
    memset(f, 0, sizeof(*f));
    f->status = 0;

    if (!url_fetchable(u, f->error, sizeof(f->error))) return;

    for (int attempt = 0; attempt <= 1; attempt++) {
        /* Every attempt opens a new connection, so nothing about the
         * previous one survives it: an https -> http redirect must not be
         * drawn with the padlock earned by the page that sent it. */
        f->tls = 0;
        f->tls_version[0] = 0;
        f->tls_cipher[0] = 0;
        f->tls_cn[0] = 0;
        f->tls_expiry[0] = 0;
        f->truncated = 0;
        f->status = 0;
        f->url[0] = 0;

        char req[1024];
        if (build_request(req, sizeof(req), u) < 0) {
            snprintf(f->error, sizeof(f->error), "the address is too long");
            return;
        }

        char err[WEB_ERR_MAX];
        err[0] = 0;
        long t0 = now_ms();

        WebStream *st = tr->open(u->host, u->port, u->https, err, sizeof(err));
        if (!st) {
            snprintf(f->error, sizeof(f->error), "%s", err[0] ? err : "cannot connect");
            return;
        }

        /* Take the session facts now: the connection is closed below. */
        const TlsInfo *ti = tr->tls ? tr->tls(st) : NULL;
        if (ti) {
            f->tls = 1;
            snprintf(f->tls_version, sizeof(f->tls_version), "%s", ti->version);
            snprintf(f->tls_cipher, sizeof(f->tls_cipher), "%s", ti->cipher);
            snprintf(f->tls_cn, sizeof(f->tls_cn), "%s", ti->cn);
            snprintf(f->tls_expiry, sizeof(f->tls_expiry), "%s", ti->not_after);
        }
        if (tr->send(st, req, (long)strlen(req), err, sizeof(err)) < 0) {
            tr->close(st);
            snprintf(f->error, sizeof(f->error), "%s", err[0] ? err : "cannot send the request");
            return;
        }

        long total = 0;
        int truncated = 0;
        for (;;) {
            if (total >= WEB_RAW_MAX - 1) { truncated = 1; break; }
            long n = tr->recv(st, raw + total, WEB_RAW_MAX - 1 - total, err, sizeof(err));
            if (n < 0) {
                tr->close(st);
                snprintf(f->error, sizeof(f->error), "%s", err[0] ? err : "cannot read the response");
                return;
            }
            if (n == 0) break;
            total += n;
        }
        tr->close(st);

        f->elapsed_ms = now_ms() - t0;
        f->raw_bytes = total;
        f->truncated = truncated;
        raw[total] = 0;

        if (total == 0) {
            snprintf(f->error, sizeof(f->error), "the server sent nothing and closed");
            return;
        }

        HttpResponse hr;
        char *body = NULL;
        long blen = 0;
        if (http_parse(raw, total, &hr, &body, &blen) != 0) {
            snprintf(f->error, sizeof(f->error),
                     "%s sent %ld bytes that are not an HTTP response",
                     u->host, f->raw_bytes);
            return;
        }

        f->status = hr.status;
        f->body = body;
        f->body_len = blen;
        f->body_bytes = blen;
        if (hr.short_body) f->truncated = 1;
        snprintf(f->charset, sizeof(f->charset), "%s", hr.charset);
        f->latin1 = (hr.charset[0] &&
                     (!strcmp(hr.charset, "iso-8859-1") || !strcmp(hr.charset, "latin1") ||
                      !strcmp(hr.charset, "iso8859-1") || !strcmp(hr.charset, "windows-1252")));

        /* one redirect, as promised */
        if ((hr.status == 301 || hr.status == 302 || hr.status == 303 ||
             hr.status == 307 || hr.status == 308) &&
            hr.location[0] && attempt == 0) {
            char next[WEB_PATH_MAX];
            char rerr[WEB_ERR_MAX];
            rerr[0] = 0;
            url_resolve(u, hr.location, next, sizeof(next), rerr, sizeof(rerr));
            if (next[0]) {
                Url nu;
                if (url_parse(next, &nu, rerr, sizeof(rerr)) != 0) {
                    snprintf(f->error, sizeof(f->error), "%s", rerr);
                    return;
                }
                if (!url_fetchable(&nu, f->error, sizeof(f->error))) return;
                *u = nu;
                f->status = 0;
                continue;
            }
        }

        url_make(u, f->url, sizeof(f->url));
        f->ok = 1;
        return;
    }
}

/* ============================================================
 *  HTML -> text
 * ============================================================ */

/* Named entities we decode. Unknown ones are printed as written,
 * which is what a reader can still understand. */
static const struct { const char *name; const char *utf8; } ENTITIES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " }, { "shy", "" },
    { "mdash", "\xE2\x80\x94" }, { "ndash", "\xE2\x80\x93" },
    { "hellip", "\xE2\x80\xA6" }, { "bull", "\xE2\x80\xA2" },
    { "middot", "\xC2\xB7" }, { "deg", "\xC2\xB0" },
    { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" },
    { "trade", "\xE2\x84\xA2" }, { "euro", "\xE2\x82\xAC" },
    { "laquo", "\xC2\xAB" }, { "raquo", "\xC2\xBB" },
    { "ldquo", "\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" },
    { "lsquo", "\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
    { "plusmn", "\xC2\xB1" }, { "times", "\xC3\x97" },
    { "divide", "\xC3\xB7" }, { "frac12", "\xC2\xBD" },
    { NULL, NULL }
};

static int utf8_encode(long cp, char *out) {
    if (cp < 0 || cp > 0x10FFFF) return 0;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one entity at s[0]=='&'. Returns 1 when it was one, with
 * *used = bytes consumed and *outlen = bytes written to out. */
static int entity_decode(const char *s, long n, char *out, size_t osz,
                         long *used, long *outlen) {
    *used = 0;
    *outlen = 0;
    if (n < 3 || s[0] != '&') return 0;

    long semi = -1;
    for (long i = 1; i < n && i < 12; i++) {
        if (s[i] == ';') { semi = i; break; }
        if (s[i] == ' ' || s[i] == '&' || s[i] == '<') break;
    }
    if (semi < 2) return 0;

    const char *name = s + 1;
    long nlen = semi - 1;

    if (name[0] == '#') {
        long cp = 0;
        int base = 10, i = 1;
        if (nlen > 1 && (name[1] == 'x' || name[1] == 'X')) { base = 16; i = 2; }
        if (i >= nlen) return 0;
        for (; i < nlen; i++) {
            int c = (unsigned char)name[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return 0;
            cp = cp * base + v;
            if (cp > 0x10FFFF) return 0;
        }
        if (cp == 0) return 0;
        int len = utf8_encode(cp, out);
        if (len <= 0 || (size_t)len >= osz) return 0;
        *used = semi + 1;
        *outlen = len;
        return 1;
    }

    for (int i = 0; ENTITIES[i].name; i++) {
        if (ci_eq(name, nlen, ENTITIES[i].name)) {
            size_t l = strlen(ENTITIES[i].utf8);
            if (l >= osz) return 0;
            memcpy(out, ENTITIES[i].utf8, l);
            *used = semi + 1;
            *outlen = (long)l;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int  closing;
    char name[16];
    char href[WEB_PATH_MAX];
    char alt[96];
} Tag;

/* Parse one tag starting at s[0]=='<'. Returns 1 with *used set, or 0
 * when this is not a tag (in which case the '<' is text). */
static int parse_tag(const char *s, long n, long *used, Tag *t) {
    memset(t, 0, sizeof(*t));
    *used = 0;
    if (n < 2 || s[0] != '<') return 0;

    if (s[1] == '!') {
        long j = 2;
        int comment = (n >= 4 && s[2] == '-' && s[3] == '-');
        if (comment) {
            j = 4;
            while (j + 2 < n && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
            if (j + 2 >= n) return 0;
            *used = j + 3;
        } else {
            while (j < n && s[j] != '>') j++;
            if (j >= n) return 0;
            *used = j + 1;
        }
        t->name[0] = 0;
        return 1;
    }
    if (s[1] == '?') {
        long j = 2;
        while (j < n && s[j] != '>') j++;
        if (j >= n) return 0;
        *used = j + 1;
        t->name[0] = 0;
        return 1;
    }

    long i = 1;
    if (s[i] == '/') { t->closing = 1; i++; }

    long ns = i;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || s[i] == ':')) i++;
    long nl = i - ns;
    if (nl <= 0 || nl >= (long)sizeof(t->name)) return 0;
    for (long k = 0; k < nl; k++) t->name[k] = (char)lower(s[ns + k]);
    t->name[nl] = 0;

    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i >= n) return 0;
        if (s[i] == '>') { *used = i + 1; return 1; }
        if (s[i] == '/') { i++; continue; }

        long as = i;
        while (i < n && s[i] != '=' && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '\r' && s[i] != '\n' && s[i] != '>' && s[i] != '/') i++;
        long al = i - as;
        char an[16];
        an[0] = 0;
        if (al > 0 && al < (long)sizeof(an)) {
            for (long k = 0; k < al; k++) an[k] = (char)lower(s[as + k]);
            an[al] = 0;
        }

        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            long vs = 0, vl = 0;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                vs = i;
                while (i < n && s[i] != q) i++;
                vl = i - vs;
                if (i < n) i++;
            } else {
                vs = i;
                while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
                       s[i] != '\n' && s[i] != '>') i++;
                vl = i - vs;
            }
            if (!strcmp(an, "href") && vl < (long)sizeof(t->href)) {
                memcpy(t->href, s + vs, (size_t)vl);
                t->href[vl] = 0;
            } else if (!strcmp(an, "alt") && vl < (long)sizeof(t->alt)) {
                memcpy(t->alt, s + vs, (size_t)vl);
                t->alt[vl] = 0;
            }
        }
    }
}

typedef struct {
    Doc        *d;
    const Url  *base;
    char       *title;
    size_t      title_cap;
    int         title_len;
    int         latin1;
    int         in_title;
    int         skip;            /* inside <script>/<style>       */
    int         level;           /* 0 body, 1..6 heading          */
    int         prev_level;      /* level of the last atom emitted; its
                                  * AF_BREAK decides the gap below it */
    int         in_link;         /* link index, -1 when outside   */
    int         pending_break;
    int         pending_gap;
    int         pending_space;
    char        word[WEB_ATOM_TEXT];
    int         wlen;
    int         overflow;        /* a cap was hit                 */
} Html;

static void html_atom(Html *h, const char *text, long len, int flags, int link) {
    Doc *d = h->d;
    if (len <= 0) return;
    if (len >= WEB_ATOM_TEXT) len = WEB_ATOM_TEXT - 1;
    if (d->natom >= WEB_MAX_ATOMS) { h->overflow = 1; d->truncated = 1; return; }
    Atom *a = &d->atom[d->natom++];
    memcpy(a->text, text, (size_t)len);
    a->text[len] = 0;
    a->level = (u8)h->level;
    a->flags = (u8)flags;
    int gap = 0;
    if (flags & AF_BREAK) {
        gap = h->pending_gap;
        /* A heading is followed by its own space, never by whatever the
         * next block happened to ask for: that is what makes the space
         * below every heading the same and puts them on one grid. */
        if (h->prev_level && gap < GAP_AFTER_HEAD_D) gap = GAP_AFTER_HEAD_D;
        if (gap > 255) gap = 255;
    }
    a->gap = (u8)gap;
    a->link = (int16_t)link;
    h->prev_level = h->level;
}

/* Ending a word is where the break/space state is spent. */
static void html_end_word(Html *h, int glued) {
    if (h->wlen <= 0) return;
    h->word[h->wlen] = 0;
    int flags = 0;
    if (h->pending_break) flags |= AF_BREAK;
    /* no whitespace before this word means it is glued to the previous
     * one: "lists</a>," must not put a space before the comma */
    else if (glued || !h->pending_space) flags |= AF_GLUE;
    html_atom(h, h->word, h->wlen, flags, h->in_link);
    h->wlen = 0;
    h->pending_break = 0;
    h->pending_gap = 0;
    h->pending_space = 0;
}

static void html_put(Html *h, const char *bytes, long len) {
    for (long i = 0; i < len; i++) {
        if (h->wlen >= WEB_ATOM_TEXT - 2) {
            /* one word too long for one atom: continue it in the next,
             * glued so no space appears where the split happened */
            h->word[h->wlen] = 0;
            int flags = 0;
            if (h->pending_break) flags |= AF_BREAK;
            html_atom(h, h->word, h->wlen, flags, h->in_link);
            h->wlen = 0;
            h->pending_break = 0;
            h->pending_gap = 0;
            h->pending_space = 0;
            h->word[h->wlen++] = bytes[i];
            continue;
        }
        h->word[h->wlen++] = bytes[i];
    }
}

static void html_break(Html *h, int gap) {
    html_end_word(h, 0);
    h->pending_break = 1;
    if (gap > h->pending_gap) h->pending_gap = gap;
}

/* <title> text. The text scanner calls this once per character, so it
 * must not trim here: trimming on every call ate the spaces between the
 * words of every title. html_build trims the finished title. */
static void html_title_put(Html *h, const char *bytes, long len) {
    for (long i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (h->title_len > 0 && h->title[h->title_len - 1] != ' ')
                h->title[h->title_len++] = ' ';
        } else if (h->title_len + 1 < (int)h->title_cap) {
            h->title[h->title_len++] = (char)c;
        }
        if (h->title_len < (int)h->title_cap) h->title[h->title_len] = 0;
    }
}

/* Push a complete word-atom, spending the glue/break state on it. Used
 * for text that is not built up in the word buffer. */
static void html_atom_word(Html *h, const char *text, long len, int link, int glue) {
    int flags = 0;
    if (h->pending_break) flags |= AF_BREAK;
    else if (glue || !h->pending_space) flags |= AF_GLUE;
    html_atom(h, text, len, flags, link);
    h->pending_break = 0;
    h->pending_gap = 0;
    h->pending_space = 0;
}

/* Text between tags: decode entities, collapse whitespace, transcode
 * Latin-1 when the server said so. */
static void html_text(Html *h, const char *s, long n) {
    if (h->skip) return;

    long i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        if (c == '&') {
            char dec[8];
            long used = 0, outlen = 0;
            if (entity_decode(s + i, n - i, dec, sizeof(dec), &used, &outlen)) {
                if (h->in_title) html_title_put(h, dec, outlen);
                else if (outlen == 1 && dec[0] == ' ') {
                    html_end_word(h, 0);        /* &nbsp; ends the word */
                    h->pending_space = 1;
                } else {
                    html_put(h, dec, outlen);
                }
                i += used;
                continue;
            }
            if (h->in_title) html_title_put(h, "&", 1);
            else html_put(h, "&", 1);
            i++;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (h->in_title) html_title_put(h, " ", 1);
            else {
                html_end_word(h, 0);            /* end the word, then */
                h->pending_space = 1;           /* space the next one */
            }
            i++;
            continue;
        }

        if (c < 0x20) { i++; continue; }

        if (c < 0x80) {
            if (h->in_title) html_title_put(h, (const char *)&c, 1);
            else html_put(h, (const char *)&c, 1);
            i++;
            continue;
        }

        if (h->latin1) {
            char u[2];
            u[0] = (char)(0xC0 | (c >> 6));
            u[1] = (char)(0x80 | (c & 0x3F));
            if (h->in_title) html_title_put(h, u, 2);
            else html_put(h, u, 2);
            i++;
            continue;
        }

        int len = utf8_len(c);
        if (len > n - i) len = (int)(n - i);
        if (h->in_title) html_title_put(h, s + i, len);
        else html_put(h, s + i, len);
        i += len;
    }
}

static int is_block_tag(const char *n) {
    static const char *blocks[] = {
        "p", "div", "table", "tr", "ul", "ol", "dl", "dd", "dt", "pre",
        "blockquote", "section", "article", "header", "footer", "nav",
        "main", "aside", "form", "fieldset", "figure", "figcaption",
        "hr", "address", "center", "body", "menu", "dir", "listing",
        "xmp", "noscript", "details", "summary", NULL
    };
    for (int i = 0; blocks[i]; i++)
        if (!strcmp(n, blocks[i])) return 1;
    return 0;
}

static void html_add_link(Html *h, const Tag *t) {
    if (h->in_link >= 0) return;              /* nested <a>: keep the outer */
    if (h->d->nlink >= WEB_MAX_LINKS) { h->in_link = -1; return; }
    if (!t->href[0]) { h->in_link = -1; return; }

    char abs[WEB_PATH_MAX];
    char err[64];
    err[0] = 0;
    url_resolve(h->base, t->href, abs, sizeof(abs), err, sizeof(err));
    if (!abs[0]) { h->in_link = -1; return; }   /* not a fetchable link */

    int idx = h->d->nlink++;
    snprintf(h->d->link[idx].url, sizeof(h->d->link[idx].url), "%s", abs);
    h->in_link = idx;
}

static void html_tag(Html *h, const Tag *t) {
    if (!t->name[0]) return;                  /* comment, doctype, PI */
    const char *n = t->name;

    if (t->closing) {
        if (!strcmp(n, "title")) {
            h->in_title = 0;
            return;
        }
        if (!strcmp(n, "script") || !strcmp(n, "style")) { h->skip = 0; return; }
        if (!strcmp(n, "a")) {
            if (h->in_link >= 0) {
                char num[16];
                int idx = h->in_link;
                h->in_link = -1;
                snprintf(num, sizeof(num), "[%d]", idx + 1);
                html_end_word(h, 0);
                html_atom_word(h, num, (long)strlen(num), idx, 1);
                /* no space is forced after the marker: "lists</a>,"
                 * has to read "lists[3],", while "</a> word" still
                 * gets its space from the whitespace in the page */
                h->pending_space = 0;
            }
            return;
        }
        if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
            /* The space under a heading comes from GAP_AFTER_HEAD_D in
             * html_atom(), which remembers that the previous line was a
             * heading. Asking for a block gap here would only be a
             * second, competing number. */
            html_break(h, 0);       /* flush the heading while it is still one */
            h->level = 0;
            return;
        }
        if (!strcmp(n, "li")) { html_break(h, GAP_LI_D); return; }
        if (!strcmp(n, "p")) { html_break(h, GAP_BLOCK_D); return; }
        if (!strcmp(n, "td") || !strcmp(n, "th")) { html_end_word(h, 0); h->pending_space = 1; return; }
        if (is_block_tag(n)) { html_break(h, GAP_BLOCK_D); return; }
    }

    /* open tags */
    if (h->in_title && (!strcmp(n, "body") || is_block_tag(n))) h->in_title = 0;
    if (!strcmp(n, "title")) { h->in_title = 1; return; }
    if (!strcmp(n, "script") || !strcmp(n, "style")) { h->skip = 1; return; }
    if (!strcmp(n, "a")) { html_end_word(h, 0); html_add_link(h, t); return; }
    if (!strcmp(n, "li")) {
        html_break(h, GAP_LI_D);
        html_end_word(h, 0);
        h->pending_break = 1;
        html_atom(h, "\xE2\x80\xA2", 3, AF_BREAK, -1);   /* a real bullet */
        h->pending_break = 0;
        h->pending_gap = 0;
        h->pending_space = 1;
        return;
    }
    if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
        h->level = n[1] - '0';
        /* one gap above every heading, whatever its level: h1 and h6 sit
         * on the same grid and only their font size differs */
        html_break(h, GAP_HEAD_D);
        return;
    }
    if (!strcmp(n, "br")) { html_break(h, 0); return; }
    if (!strcmp(n, "p")) { html_break(h, GAP_BLOCK_D); return; }
    if (!strcmp(n, "td") || !strcmp(n, "th")) {
        html_end_word(h, 0);
        h->pending_space = 1;
        return;
    }
    if (!strcmp(n, "img")) {
        char alt[128];
        if (t->alt[0]) snprintf(alt, sizeof(alt), "[%s]", t->alt);
        else snprintf(alt, sizeof(alt), "[image]");
        html_end_word(h, 0);
        html_atom_word(h, alt, (long)strlen(alt), h->in_link, 0);
        return;
    }
    if (is_block_tag(n)) { html_break(h, GAP_BLOCK_D); return; }
}

/* Turn a body of HTML into atoms. `title` receives the <title> text
 * (possibly empty). Returns the number of links found. */
static int html_build(Doc *d, const char *s, long n, const Url *base,
                      int latin1, char *title, size_t title_cap) {
    d->natom = 0;
    d->nseg = 0;
    d->nline = 0;
    d->nlink = 0;
    d->doc_h = 0;
    d->truncated = 0;

    title[0] = 0;

    Html h;
    memset(&h, 0, sizeof(h));
    h.d = d;
    h.base = base;
    h.title = title;
    h.title_cap = title_cap;
    h.latin1 = latin1;
    h.in_link = -1;

    long i = 0;
    while (i < n && !h.overflow) {
        if (s[i] == '<') {
            Tag t;
            long used = 0;
            if (parse_tag(s + i, n - i, &used, &t)) {
                /* Only the tags that really separate words end one - they
                 * call html_break() or html_end_word() themselves. An
                 * inline tag must not: "2026-09-27</time>." is one word. */
                html_tag(&h, &t);
                i += used;
                continue;
            }
            html_text(&h, "<", 1);
            i++;
            continue;
        }
        long start = i;
        while (i < n && s[i] != '<') i++;
        html_text(&h, s + start, i - start);
    }
    html_end_word(&h, 0);
    while (h.title_len > 0 && title[h.title_len - 1] == ' ') h.title_len--;
    title[h.title_len] = 0;
    return d->nlink;
}

/* ============================================================
 *  Layout: atoms -> lines of placed segments
 * ============================================================ */

/* Bytes of s that fit in `avail` px, always at least one character so
 * a word wider than the line makes progress instead of looping. */
static int fit_prefix(const WebMetrics *m, int level, const char *s, int n, float avail) {
    float w = 0;
    int i = 0;
    while (i < n) {
        int cl = utf8_len((unsigned char)s[i]);
        if (cl > n - i) cl = n - i;
        float cw = m->text_w(m->ctx, level, s + i, cl);
        if (i > 0 && w + cw > avail) break;
        w += cw;
        i += cl;
    }
    if (i == 0) {
        i = utf8_len((unsigned char)s[0]);
        if (i > n) i = n;
    }
    return i;
}

typedef struct {
    Doc *d;
    const WebMetrics *m;
    float width;
    u32 y;
    int first_seg;
    int segs_in_line;
    float pen;
    int height;
    int gap;
} Layout;

static void lay_emit(Layout *L, int ai, int start, int len, float x, float w, int level) {
    Doc *d = L->d;
    if (d->nseg >= WEB_MAX_SEGS) { d->truncated = 1; return; }
    Seg *s = &d->seg[d->nseg];
    s->atom = (u16)ai;
    s->start = (u16)start;
    s->len = (u16)len;
    s->x = (u16)(x + 0.5f);
    s->w = (u16)(w + 0.5f);
    if (L->first_seg < 0) L->first_seg = d->nseg;
    d->nseg++;
    L->segs_in_line++;
    int lh = (int)(L->m->line_h(L->m->ctx, level) + 0.5f);
    if (lh > L->height) L->height = lh;
}

/* A design gap in screen pixels. This is the only place where the
 * document's vertical rhythm meets the desktop's text scale, so a scale
 * change re-spaces the next layout without rebuilding the document. */
static int layout_gap(int design) {
    if (design <= 0) return 0;
    return ui_px(design);
}

static void lay_flush(Layout *L) {
    Doc *d = L->d;
    if (L->first_seg >= 0 && L->segs_in_line > 0 && d->nline < WEB_MAX_LINES) {
        Line *ln = &d->line[d->nline++];
        ln->first = (u32)L->first_seg;
        ln->n = (u16)L->segs_in_line;
        ln->h = (u16)(L->height > 0 ? L->height : 12);
        /* The document starts flush with the content rectangle: a heading
         * gap at the very top is not a margin, it is an offset. */
        ln->gap = (u16)(d->nline == 1 ? 0 : layout_gap(L->gap));
        L->y += ln->gap;
        ln->y = L->y;
        L->y += ln->h;
    } else if (L->segs_in_line > 0) {
        d->truncated = 1;
    }
    L->first_seg = -1;
    L->segs_in_line = 0;
    L->pen = 0;
    L->height = 0;
    L->gap = 0;
}

static void doc_layout(Doc *d, const WebMetrics *m, int width) {
    d->nseg = 0;
    d->nline = 0;
    d->doc_h = 0;
    if (width < 40) width = 40;

    Layout L;
    memset(&L, 0, sizeof(L));
    L.d = d;
    L.m = m;
    L.width = (float)width;
    L.first_seg = -1;

    for (int ai = 0; ai < d->natom; ai++) {
        const Atom *a = &d->atom[ai];
        int level = a->level;
        int len = (int)strlen(a->text);
        if (len <= 0) continue;

        if (a->flags & AF_BREAK) {
            lay_flush(&L);
            L.gap = a->gap;
        }

        float space = m->text_w(m->ctx, level, " ", 1);
        int start = 0;
        while (start < len) {
            float wfull = m->text_w(m->ctx, level, a->text + start, len - start);
            float sp = (L.segs_in_line > 0 && !(a->flags & AF_GLUE)) ? space : 0.0f;

            if (L.pen + sp + wfull <= L.width) {
                lay_emit(&L, ai, start, len - start, L.pen + sp, wfull, level);
                L.pen += sp + wfull;
                break;
            }
            if (L.segs_in_line > 0) {
                lay_flush(&L);
                continue;                    /* try again on a fresh line */
            }
            /* A word wider than the whole line: this is the one case
             * where breaking a word is the only honest thing to do. */
            int k = fit_prefix(m, level, a->text + start, len - start, L.width);
            float kw = m->text_w(m->ctx, level, a->text + start, k);
            lay_emit(&L, ai, start, k, 0.0f, kw, level);
            lay_flush(&L);
            start += k;
        }
    }
    lay_flush(&L);
    d->doc_h = L.y;
}

/* The line whose box contains y (or the last one above it). */
static int line_at(const Doc *d, u32 y) {
    int lo = 0, hi = d->nline - 1, best = 0;
    if (d->nline <= 0) return 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (d->line[mid].y <= y) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

/* ============================================================
 *  Typography
 *
 *  The document's own sizes, and the measure functions layout uses.
 *  These are the real Font objects: wrapping is decided by
 *  font_width() of the same Lato faces that draw the glyphs, so a
 *  line that fits here fits on screen.
 * ============================================================ */
static int style_size(int level) {
    static const int sizes[7] = { 12, 24, 20, 17, 15, 13, 12 };
    if (level < 0 || level > 6) level = 0;
    return sizes[level];
}

static int style_face(int level) {
    return level ? FONT_SEMIBOLD : FONT_REGULAR;
}

static Font *font_for(int level) {
    return font_get(style_face(level), (float)style_size(level));
}

/* Metrics handed to layout: the real font_width of the real face. */
static float real_text_w(void *ctx, int level, const char *s, int n) {
    Font *f = font_for(level);
    if (!f || n <= 0) return 0;
    char buf[WEB_ATOM_TEXT + 8];
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    (void)ctx;
    return font_width(f, buf);
}

static float real_line_h(void *ctx, int level) {
    Font *f = font_for(level);
    (void)ctx;
    return f ? font_height(f) : 16.0f;
}

static const WebMetrics real_metrics = { NULL, real_text_w, real_line_h };

/* ============================================================
 *  The app itself: window, drawing, input.
 *
 *  Everything below needs the client SDK, so it is compiled out of a
 *  host test build (which still links font.c to get real metrics).
 * ============================================================ */
#ifndef WEB_HOST_TEST

enum { ST_START, ST_LOADING, ST_PAGE, ST_ERROR };

typedef struct {
    MClient *c;

    char raw[WEB_RAW_MAX];
    Doc  doc;

    Url  cur;
    char url[WEB_PATH_MAX];       /* the address actually on screen    */
    char pending[WEB_PATH_MAX];   /* the address being loaded          */
    char title[WEB_TITLE_MAX];
    int  status;
    long raw_bytes, body_bytes;
    long elapsed_ms;
    int  truncated;
    char charset[WEB_CHARSET_MAX];

    int  state;
    char error[WEB_ERR_MAX];

    /* Set from the handshake of the page on screen, so the header can
     * show a lock and the status bar can name the session. All zero for
     * a plain http page. */
    int  tls_ok;
    char tls_version[16];
    char tls_cipher[48];
    char tls_cn[128];
    char tls_expiry[32];

    int  scroll, max_scroll;
    int  content_h;               /* how tall what is on screen really is */
    int  view_w, view_h, laid_w;

    char edit[WEB_PATH_MAX];      /* the address bar shares the URL limit */
    int  edit_focus;
    int  caret_on;

    char hist[WEB_HIST_MAX][WEB_PATH_MAX];
    int  hist_n, hist_i;

    int  w, h, maxw, maxh;
    int  dirty, last_buttons;
} Web;

/* ------------------------------------------------------------ geometry
 * One set of numbers for the whole window. The ones that hold text are
 * measured from the very fonts that go in them (font_height of the real
 * Font object, which font_get() has already scaled), and everything else
 * is a design size pushed through ui_px() when it is needed. Nothing is
 * stored, so the header, the page and the status bar re-derive themselves
 * on every frame and cannot disagree with each other.
 * ------------------------------------------------------------ */
static Font *title_font(void) { return font_get(FONT_MEDIUM, 12); }
static Font *field_font(void) { return font_get(FONT_REGULAR, 11); }

/* The height of a line of `f`, or a sane design fallback before the fonts
 * are loaded. Rounded up, never truncated: a band must not be shorter
 * than the text in it. */
static int text_h(Font *f, int fallback) {
    float h = f ? font_height(f) : 0.0f;
    if (h < 1.0f) return fallback;
    return (int)(h + 0.999f);
}

static int page_mx(void) { return ui_px(MX_D); }
static int sbar_w(void)  { return ui_px(SBAR_D); }

static int title_row_h(void) { return text_h(title_font(), 17); }
static int field_h(void)     { return text_h(field_font(), 16) + 2 * ui_px(ADDR_PAD_D); }
static int field_y(void)     { return ui_px(ROW_PAD_D) + title_row_h() + ui_px(ROW_PAD_D); }
static int head_h(void)      { return field_y() + field_h() + ui_px(HEAD_GAP_D); }

/* How many lines the status bar needs. A page that came over a verified TLS
 * session gets a second line for the session facts; the page gives up
 * exactly that much height, because page_rect() asks this same question. */
static int foot_lines(const Web *w) {
    return (w && w->state == ST_PAGE && w->tls_ok) ? 2 : 1;
}

static int foot_h(const Web *w) {
    return foot_lines(w) * text_h(field_font(), 16) + 2 * ui_px(FOOT_PAD_D);
}

/* A surface with no pixels at all. font_draw() clips every glyph against
 * s->w / s->h, so drawing a screen into this measures its height with the
 * very code that paints it - one description, never two that can drift.
 * The width is set per measurement (word wrapping needs the real measure);
 * the height stays 0 so nothing can be written. Nothing here is const
 * because the draw functions take a Surface *. */
static Surface SINK = { NULL, 0, 0, 0, 0, 0, 0, 0 };

typedef struct { int x, y, w, h; } PageRect;

/* Can the header band fit? The window manager lets an external client be
 * resized down to a 280x160 outer box (src/main.c:1645), whose client area
 * is 118 px at the default scale and 94 px at the largest one - so this can
 * be false, and render() has to say so instead of freezing on the last
 * frame. */
static int header_fits(const Web *w) { return w->h >= head_h(); }

/* The status bar is drawn only when a real page is left in front of it: in
 * a short window the page wins, because a status line over an empty page
 * explains nothing. 0 means "not drawn", and page_rect() honours it, so a
 * click can never land in a footer that is not there. */
static int footer_px(const Web *w) {
    int body = w->h - (head_h() + ui_px(CGAP_D));
    if (body < foot_h(w) + ui_px(12)) return 0;
    return foot_h(w);
}

/* The rectangle the document lives in, between the header band and the
 * status bar, inset by the one content margin. render() draws here and
 * handle_mouse() tests clicks here; both call this, so a click can never
 * address a rectangle that was drawn somewhere else. h == 0 means there is
 * no page pixel to draw at all. */
static PageRect page_rect(const Web *w) {
    PageRect r;
    r.x = page_mx();
    r.y = head_h() + ui_px(CGAP_D);
    r.w = w->w - 2 * page_mx();
    r.h = w->h - r.y - footer_px(w);
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    /* never describe pixels we do not own */
    if (r.x > w->w) { r.x = w->w; r.w = 0; }
    if (r.x + r.w > w->w) r.w = w->w - r.x;
    if (r.y > w->h) { r.y = w->h; r.h = 0; }
    if (r.y + r.h > w->h) r.h = w->h - r.y;
    return r;
}

/* The address field, in window pixels. draw_header() paints it and
 * handle_mouse() hit-tests it through this one function. */
static int addr_field_rect(const Web *w, float *fx, float *fy,
                           float *fw, float *fh) {
    float x = (float)page_mx();
    float width = (float)(w->w - 2 * page_mx());
    if (width < 40.0f) return 0;
    *fx = x;
    *fy = (float)field_y();
    *fw = width;
    *fh = (float)field_h();
    return 1;
}

/* Draw text, cut with ".." when it cannot fit in maxw pixels. */
static void draw_fit(Surface *s, Font *f, float x, float y, float maxw,
                     const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 6.0f) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }
    float dots = font_width(f, "..");
    size_t len = strlen(buf);
    while (len > 0) {
        buf[len] = 0;
        if (font_width(f, buf) + dots <= maxw) break;
        len--;
    }
    if (len == 0) return;
    buf[len] = '.';
    buf[len + 1] = '.';
    buf[len + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* Word-wrap a plain string (error text, hints) into a rectangle.
 * Returns the height used. */
static int draw_wrapped(Surface *v, Font *f, float x, float y, float maxw,
                        const char *text, u32 color, int maxlines) {
    if (!f || !text || maxw < 20.0f) return 0;
    float lh = font_height(f);
    int line = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);

    char out[512];
    int olen = 0;
    out[0] = 0;
    const char *p = buf;
    while (*p && line < maxlines) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        int wl = (int)(p - w);
        if (wl == 0) break;

        char probe[512];
        int n = 0;
        if (olen > 0) {
            memcpy(probe, out, (size_t)olen);
            n = olen;
            probe[n++] = ' ';
        }
        int copy = wl;
        if (n + copy >= (int)sizeof(probe)) copy = (int)sizeof(probe) - n - 1;
        memcpy(probe + n, w, (size_t)copy);
        n += copy;
        probe[n] = 0;

        if (olen > 0 && font_width(f, probe) > maxw) {
            font_draw(v, f, x, y + (float)line * lh, out, color);
            line++;
            memcpy(out, w, (size_t)copy);
            olen = copy;
            out[olen] = 0;
        } else {
            memcpy(out, probe, (size_t)n + 1);
            olen = n;
        }
    }
    if (olen > 0 && line < maxlines) {
        font_draw(v, f, x, y + (float)line * lh, out, color);
        line++;
    }
    return (int)((float)line * lh);
}

static u32 color_for_link(int isnum) {
    return isnum ? C_LINKNUM : C_LINK;
}

static void draw_page_lines(Surface *v, Web *w) {
    Doc *d = &w->doc;
    if (d->nline <= 0) return;

    int first = line_at(d, (u32)w->scroll);
    for (int i = first; i < d->nline; i++) {
        Line *ln = &d->line[i];
        int ly = (int)ln->y - w->scroll;
        if (ly >= v->h) break;
        if (ly + (int)ln->h <= 0) continue;

        for (int j = 0; j < (int)ln->n; j++) {
            Seg *sg = &d->seg[ln->first + j];
            Atom *a = &d->atom[sg->atom];
            Font *f = font_for(a->level);
            if (!f) continue;
            int len = sg->len;
            char tmp[WEB_ATOM_TEXT + 4];
            if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
            memcpy(tmp, a->text + sg->start, (size_t)len);
            tmp[len] = 0;

            u32 col;
            if (a->link >= 0) col = color_for_link(a->flags & AF_LINKNUM);
            else if (a->level) col = C_HEAD;
            else col = C_TEXT;

            font_draw(v, f, (float)sg->x, (float)ly, tmp, col);

            if (a->link >= 0) {
                int uy = ly + (int)font_ascent(f) + ui_px(2);
                surf_fill(v, sg->x, uy, (int)sg->w, 1, color_alpha(col, 0.45f));
            }
        }
    }
}

/* A padlock the height of a line of text, drawn from two rounded
 * rectangles. The shackle is a ring, the body a filled block that covers
 * the ring's lower half, so the two read as one lock. `secure` closes the
 * shackle; without it the ring is lifted and pushed right, which is the
 * open padlock every browser uses for a page that is not private. */
static void draw_lock(Surface *s, float x, float y, float h, u32 col, int secure) {
    if (h < 5.0f) return;
    float w = h * 0.72f;
    float bw = h * 0.15f;
    if (bw < 1.0f) bw = 1.0f;
    float bodyh = h * 0.52f;

    float ringw = w - 2.0f * bw;
    float ringh = h * 0.58f;
    float ringx = x + bw + (secure ? 0.0f : w * 0.22f);
    float ringy = y + bw + (secure ? 0.0f : -h * 0.10f);
    gfx_rrect_aa(s, ringx, ringy, ringw, ringh, ringw * 0.5f, 0, col, bw);

    gfx_rrect_aa(s, x, y + h - bodyh, w, bodyh, h * 0.15f, col, 0, 0);
}

static void draw_header(Surface *s, Web *w) {
    Font *ft = title_font();
    Font *fd = field_font();
    if (!ft || !fd) return;

    const int mx = page_mx();
    const char *title = w->title[0] ? w->title : (w->url[0] ? w->url : "Browser");

    /* The badge states the scheme of the page that is actually on screen,
     * and only then: with no page there is nothing to make a claim about,
     * and "https" is earned by the session, not by what the address says. */
    int secure = (w->state == ST_PAGE) && w->tls_ok;
    int badge_show = (w->state == ST_PAGE);
    const char *badge = secure ? "https" : "http";
    u32 badge_fg = secure ? C_ACCENT : C_DIM;

    /* The title row is one band of exactly title_row_h(): the title text
     * lives at its top, the badge is centred in it, and the address field
     * starts below it. No two of those numbers can overlap. */
    int row_y = ui_px(ROW_PAD_D);
    int row_h = title_row_h();
    float bh = (float)ui_px(BADGE_H_D);
    if (bh > (float)row_h) bh = (float)row_h;
    float bp = (float)ui_px(BADGE_PAD_D);
    float badge_w = badge_show ? font_width(fd, badge) + 2.0f * bp : 0.0f;
    float badge_x = (float)(w->w - mx) - badge_w;
    float badge_y = (float)row_y + ((float)row_h - bh) * 0.5f;

    /* the badge is the right end of the title row, so the title is cut to
     * whatever is left of it - on the same margins as the document */
    float title_max = badge_x - (float)mx - (float)ui_px(12);
    if (title_max < (float)ui_px(24)) title_max = (float)(w->w - 2 * mx);
    draw_fit(s, ft, (float)mx, (float)row_y, title_max, title, C_TEXT);
    if (badge_show && badge_x > (float)mx + (float)ui_px(24)) {
        gfx_rrect_aa(s, badge_x, badge_y, badge_w, bh, bh * 0.5f,
                     WITH_A(badge_fg, 0x22), WITH_A(badge_fg, 0x55), 1.0f);
        font_draw(s, fd, badge_x + bp,
                  badge_y + (bh - font_height(fd)) * 0.5f, badge, badge_fg);
    }

    /* address bar */
    float fx, fy, fw, fh;
    if (!addr_field_rect(w, &fx, &fy, &fw, &fh)) return;
    u32 border = w->edit_focus ? WITH_A(C_ACCENT, 0xAA) : C_EDGE;
    gfx_rrect_aa(s, fx, fy, fw, fh, (float)ui_px(5), C_FIELD, border, 1.0f);

    float pad = (float)ui_px(8);
    float ty = fy + (fh - font_height(fd)) * 0.5f;   /* centred in the field */
    const char *text = w->edit[0] ? w->edit : "";
    float inner = fw - 2.0f * pad;

    /* The lock sits at the field's left edge and describes the page on
     * screen - closed and accent for a verified TLS session, open and grey
     * otherwise. The address text keeps the scheme ("https://...") itself,
     * so the two together are the lock and the scheme in the bar. */
    float lh = font_height(fd) * 0.86f;
    float lw = lh * 0.72f;
    float lgap = (float)ui_px(6);
    float lockoff = 0.0f;
    if (lh >= 6.0f && inner > lw + lgap + (float)ui_px(48)) {
        draw_lock(s, fx + pad, fy + (fh - lh) * 0.5f, lh,
                  secure ? C_ACCENT : C_DIM, secure);
        lockoff = lw + lgap;
        inner -= lockoff;
    }
    float base = fx + pad + lockoff;

    if (!text[0] && !w->edit_focus) {
        draw_fit(s, fd, base, ty, inner,
                 "Tab, then type an address - https:// is the default", C_FAINT);
    } else {
        /* While typing, the end of the address is what matters, so when
         * it is longer than the field we show its tail. Walking back from
         * the end costs one glyph measurement per character, which keeps
         * a long address cheap to draw. */
        const char *show = text;
        if (font_width(fd, text) > inner) {
            int n = (int)strlen(text);
            int i = n;
            float w = 0;
            while (i > 0) {
                int j = i - 1;
                while (j > 0 && ((unsigned char)text[j] & 0xC0) == 0x80) j--;
                char one[8];
                int len = i - j;
                if (len < (int)sizeof(one)) {
                    memcpy(one, text + j, (size_t)len);
                    one[len] = 0;
                    float cw = font_width(fd, one);
                    if (w + cw > inner - 8.0f) break;
                    w += cw;
                }
                i = j;
            }
            show = text + i;
            font_draw(s, fd, base, ty, "<", C_FAINT);
        }
        float tx0 = base + (show != text ? (float)ui_px(7) : 0.0f);
        font_draw(s, fd, tx0, ty, show[0] ? show : "<",
                  show[0] ? C_TEXT : C_FAINT);
        if (w->edit_focus && w->caret_on) {
            float tw = font_width(fd, show);
            surf_fill(s, (int)(tx0 + tw) + 1, (int)ty, 1,
                      (int)font_height(fd), C_ACCENT);
        }
    }
}

static void draw_footer(Surface *s, Web *w) {
    Font *fd = field_font();
    if (!fd) return;
    const int bar = footer_px(w);
    if (!bar) return;                     /* no room: the page keeps the space */

    const int mx = page_mx();

    /* the hairline and both ends of the status line sit on exactly the
     * margins the document uses, so the window has one gutter, not two */
    surf_blend(s, mx, w->h - bar, w->w - 2 * mx, 1, color_alpha(C_TEXT, 0.10f));

    char left[WEB_ERR_MAX + 64];
    if (w->state == ST_LOADING)
        snprintf(left, sizeof(left), "loading %s ...", w->pending);
    else if (w->state == ST_ERROR)
        snprintf(left, sizeof(left), "%s", w->error);
    else if (w->state == ST_START)
        snprintf(left, sizeof(left), "ready - http and https, no scripts");
    else
        snprintf(left, sizeof(left), "HTTP %d  %ld bytes  %ld ms%s",
                 w->status, w->body_bytes, w->elapsed_ms,
                 (w->truncated || w->doc.truncated) ? "  (truncated)" : "");

    char right[64];
    if (w->doc.nline > 0 && w->state == ST_PAGE) {
        int cl = line_at(&w->doc, (u32)w->scroll) + 1;
        if (cl > w->doc.nline) cl = w->doc.nline;
        snprintf(right, sizeof(right), "line %d/%d", cl, w->doc.nline);
    } else {
        snprintf(right, sizeof(right), "%ld bytes", w->raw_bytes);
    }

    /* Two lines only for a verified TLS page: the session facts sit above
     * the HTTP facts, and both are read from the handshake of the page on
     * screen - version, cipher, leaf CN and the real expiry date. */
    const int lh = text_h(fd, 16);
    const int padb = ui_px(FOOT_PAD_D);
    const int bottom_top = w->h - padb - lh;
    float fy = (float)bottom_top + ((float)lh - font_height(fd)) * 0.5f;

    if (foot_lines(w) == 2) {
        char tls[WEB_ERR_MAX + 96];
        snprintf(tls, sizeof(tls), "%s  %s  CN=%s  expires %s",
                 w->tls_version[0] ? w->tls_version : "TLS",
                 w->tls_cipher[0] ? w->tls_cipher : "?",
                 w->tls_cn[0] ? w->tls_cn : w->cur.host,
                 w->tls_expiry[0] ? w->tls_expiry : "(unknown)");
        int top = bottom_top - lh;
        float ty = (float)top + ((float)lh - font_height(fd)) * 0.5f;
        draw_fit(s, fd, (float)mx, ty, (float)(w->w - 2 * mx), tls, C_ACCENT);
    }

    float rw = font_width(fd, right);
    u32 lc = C_ACCENT;
    if (w->state == ST_ERROR) lc = C_ERR;
    else if (w->state != ST_LOADING) lc = color_alpha(C_TEXT, 0.55f);
    draw_fit(s, fd, (float)mx, fy,
             (float)(w->w - 2 * mx) - rw - (float)ui_px(12), left, lc);
    font_draw(s, fd, (float)(w->w - mx) - rw, fy, right,
              color_alpha(C_TEXT, 0.40f));
}

static void draw_scrollbar(Surface *v, Web *w) {
    if (w->content_h <= 0 || w->view_h <= 0) return;
    if (w->content_h <= w->view_h) return;

    int barw = ui_px(3);
    int minthumb = ui_px(12);
    int track_h = v->h;
    int thumb = (int)((float)track_h * (float)w->view_h / (float)w->content_h);
    if (thumb < minthumb) thumb = minthumb;
    if (thumb > track_h) thumb = track_h;
    int span = track_h - thumb;
    int off = 0;
    if (w->max_scroll > 0) off = span * w->scroll / w->max_scroll;
    gfx_rrect_aa(v, (float)(v->w - ui_px(6)), (float)off, (float)barw,
                 (float)thumb, (float)barw * 0.5f, WITH_A(C_TEXT, 0x55), 0, 0);
}

/* Not even the header band fits (the WM allows a 280x160 outer box, whose
 * client area is 118 px at the default scale and 94 px at the largest, while
 * the header alone needs 70 and 111). Say so instead of leaving the last
 * frame frozen with no explanation. The wording leads with the part that
 * always fits, and the numbers go on a second line only when that line
 * really fits: a truncated number is worse than none. */
static void draw_too_small(Surface *s, Web *w) {
    Font *f = font_get(FONT_REGULAR, 9);       /* the smallest face we have */
    if (!f) f = field_font();
    if (!f) return;

    int mx = page_mx();
    if (2 * mx > w->w - 8) mx = 4;
    float avail = (float)(w->w - 2 * mx);
    int lh = (int)font_height(f);
    int y = ui_px(ROW_PAD_D);

    font_draw(s, f, (float)mx, (float)y, "Window too small", C_DIM);
    y += lh + ui_px(2);

    char msg[96];
    snprintf(msg, sizeof(msg), "%d px tall, %d needed", w->h, head_h());
    if (y + lh <= w->h && font_width(f, msg) <= avail)
        font_draw(s, f, (float)mx, (float)y, msg, C_FAINT);
}

/* The help screen. It returns the y just past its last line, which is what
 * render() turns into max_scroll: the help scrolls like a page, so a hint
 * is never merely unreachable on a short window. `yoff` shifts everything,
 * and font_draw() clips whatever leaves the content rectangle. */
static int draw_start(Surface *v, Web *w, int yoff) {
    Font *fb = font_get(FONT_SEMIBOLD, 20);
    Font *fd = font_get(FONT_REGULAR, 12);
    Font *fm = font_get(FONT_MONO, 11);
    if (!fb || !fd || !fm) return yoff;

    (void)w;
    float x = 0.0f;                    /* the view is already inset */
    int y = yoff;

    font_draw(v, fb, x, (float)y, "Browser", C_HEAD);
    y += (int)font_height(fb) + ui_px(GAP_BLOCK_D);

    static const char *keys[][2] = {
        { "Tab",          "focus the address bar (Tab again to leave it)" },
        { "Enter",        "load what you typed" },
        { "Backspace",    "back, when the address bar is not focused" },
        { "j k  Up Down", "scroll a line" },
        { "PgUp PgDn",    "scroll a page (Space too)" },
        { "Wheel",        "scroll three lines per notch" },
        { "Home End",     "top or bottom of the page" },
        { "Ctrl+R",       "reload" },
        { "q",            "quit, when the address bar is not focused" },
        { "click",        "links are numbered [1] - click one to follow it" },
    };
    float col = (float)ui_px(116);
    int kh = (int)font_height(fm);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        font_draw(v, fm, x, (float)y, keys[i][0], C_ACCENT);
        /* cut with ".." rather than sliced by the content edge: at a large
         * text scale a long hint is wider than a narrow window */
        draw_fit(v, fd, x + col, (float)y, (float)v->w - col, keys[i][1], C_DIM);
        y += kh + ui_px(2);
    }

    /* one example, not a directory: "Try: <address>" on a single line */
    y += ui_px(GAP_AFTER_HEAD_D);
    float tw = font_width(fd, "Try: ");
    font_draw(v, fd, x, (float)y, "Try:", C_DIM);
    draw_fit(v, fm, x + tw, (float)y, (float)v->w - tw, "https://example.com/", C_LINK);
    y += (int)font_height(fd) + ui_px(GAP_BLOCK_D);

    /* Say what https actually costs and checks, in the app itself, so the
     * guarantee is not buried in a source comment. */
    y += draw_wrapped(v, fd, x, (float)y, (float)v->w,
                      "https:// is verified against the system CA bundle: the chain up "
                      "to a trusted root, the host name in the certificate and its "
                      "validity dates. A page that fails any of those checks is not "
                      "rendered - the status bar names the check that failed.",
                      C_DIM, 5);
    return y;
}

/* The error screen, same contract as draw_start(). */
static int draw_error(Surface *v, Web *w, int yoff) {
    Font *fb = font_get(FONT_SEMIBOLD, 16);
    Font *fd = font_get(FONT_REGULAR, 12);
    if (!fb || !fd) return yoff;

    float x = 0.0f;                    /* the view is already inset */
    float mw = (float)v->w;
    int y = yoff;
    font_draw(v, fb, x, (float)y, "This page could not be loaded", C_ERR);
    y += (int)font_height(fb) + ui_px(GAP_BLOCK_D);
    y += draw_wrapped(v, fd, x, (float)y, mw, w->error, C_TEXT, 5);
    y += ui_px(GAP_BLOCK_D);
    if (w->pending[0]) {
        y += draw_wrapped(v, fd, x, (float)y, mw, w->pending, C_DIM, 3);
        y += ui_px(GAP_BLOCK_D);      /* only when there was a line to space */
    }
    y += draw_wrapped(v, fd, x, (float)y, mw,
                      "Backspace goes back, Tab lets you type another address.",
                      C_FAINT, 2);
    return y;
}

static void render(Web *w) {
    MClient *c = w->c;
    if (!c || !c->px) return;

    Surface s;
    s.px = c->px;
    s.w = w->w;
    s.h = w->h;
    s.stride = c->stride;

    /* Always paint something. The WM can shrink an external window to a
     * 280x160 box (118 px of client at the default scale, 94 px at the
     * largest); the bands are dropped in order of what a reader needs
     * least - status bar, then page detail, then the header - and below
     * the header there is one line that says why. */
    gfx_gradient(&s, 0, 0, w->w, w->h, C_BG_TOP, C_BG_BOT);
    if (!header_fits(w)) { draw_too_small(&s, w); return; }
    draw_header(&s, w);

    PageRect r = page_rect(w);
    int cx = r.x, cy = r.y, cw = r.w, ch = r.h;

    w->view_w = cw;
    w->view_h = ch;

    /* The page wraps in the space left over by the scroll bar. */
    int tw = cw - sbar_w();
    if (tw < 40) tw = 40;
    if (tw != w->laid_w) {
        doc_layout(&w->doc, &real_metrics, tw);
        w->laid_w = tw;
    }

    /* How tall what is on screen really is. The two procedural screens are
     * measured by drawing them into the pixel-less SINK, so their scroll
     * range comes from the same code that paints them. The sink carries the
     * real width, because the error text word-wraps: measuring it at width 0
     * would count one line and leave the rest unreachable. */
    SINK.w = cw;
    SINK.h = 0;
    if (w->state == ST_START)      w->content_h = draw_start(&SINK, w, 0);
    else if (w->state == ST_ERROR) w->content_h = draw_error(&SINK, w, 0);
    else                           w->content_h = (int)w->doc.doc_h;

    w->max_scroll = w->content_h - ch;
    if (w->max_scroll < 0) w->max_scroll = 0;
    if (w->scroll > w->max_scroll) w->scroll = w->max_scroll;
    if (w->scroll < 0) w->scroll = 0;

    /* A view Surface that starts at the content origin: everything drawn
     * into it is clipped to the content rectangle, exactly, so a screen can
     * never paint over the header or the status bar. When the window is too
     * short for a page there is no such rectangle (ch == 0) and nothing is
     * drawn into it - the header and footer still are. */
    if (cw > 0 && ch > 0) {
        Surface v;
        v.px = s.px + (size_t)cy * (size_t)s.stride + (size_t)cx;
        v.w = cw;
        v.h = ch;
        v.stride = s.stride;

        if (w->state == ST_START)      draw_start(&v, w, -w->scroll);
        else if (w->state == ST_ERROR) draw_error(&v, w, -w->scroll);
        else                           draw_page_lines(&v, w);

        draw_scrollbar(&v, w);
    }
    draw_footer(&s, w);
}

/* ------------------------------------------------------------ loading */

static void web_clear_page(Web *w) {
    w->doc.natom = 0;
    w->doc.nseg = 0;
    w->doc.nline = 0;
    w->doc.nlink = 0;
    w->doc.doc_h = 0;
    w->doc.truncated = 0;
    w->title[0] = 0;
    w->status = 0;
    w->raw_bytes = 0;
    w->body_bytes = 0;
    w->elapsed_ms = 0;
    w->truncated = 0;
    w->scroll = 0;
    w->max_scroll = 0;
    /* There is no page on screen any more, so there is no session to
     * describe: the lock and the TLS line go with it. */
    w->tls_ok = 0;
    w->tls_version[0] = 0;
    w->tls_cipher[0] = 0;
    w->tls_cn[0] = 0;
    w->tls_expiry[0] = 0;
}

static void hist_push(Web *w, const char *url) {
    if (w->hist_i >= 0 && w->hist_i < w->hist_n &&
        !strcmp(w->hist[w->hist_i], url) && w->hist_i == w->hist_n - 1)
        return;                                  /* reloading: same entry */
    if (w->hist_i < w->hist_n - 1) w->hist_n = w->hist_i + 1;   /* drop forward */
    if (w->hist_n >= WEB_HIST_MAX) {
        memmove(w->hist[0], w->hist[1], sizeof(w->hist[0]) * (WEB_HIST_MAX - 1));
        w->hist_n = WEB_HIST_MAX - 1;
    }
    snprintf(w->hist[w->hist_n], WEB_PATH_MAX, "%s", url);
    w->hist_n++;
    w->hist_i = w->hist_n - 1;
}

/* Start loading an address. The caller renders first (so the word
 * "loading" is really on screen) and then calls web_load(). */
static void web_go(Web *w, const char *addr, int push_history) {
    Url u;
    char err[WEB_ERR_MAX];
    err[0] = 0;

    if (url_parse(addr, &u, err, sizeof(err)) != 0) {
        web_clear_page(w);
        snprintf(w->pending, sizeof(w->pending), "%s", addr);
        snprintf(w->url, sizeof(w->url), "%s", addr);
        snprintf(w->error, sizeof(w->error), "%s", err);
        w->state = ST_ERROR;
        return;
    }
    if (!url_fetchable(&u, err, sizeof(err))) {
        web_clear_page(w);
        snprintf(w->pending, sizeof(w->pending), "%s", addr);
        snprintf(w->url, sizeof(w->url), "%s", addr);
        snprintf(w->error, sizeof(w->error), "%s", err);
        w->state = ST_ERROR;
        return;
    }
    url_make(&u, w->pending, sizeof(w->pending));
    snprintf(w->edit, sizeof(w->edit), "%s", w->pending);
    if (push_history) hist_push(w, w->pending);
    w->state = ST_LOADING;
}

#ifndef WEB_DRAW_ONLY

static void web_load(Web *w) {
    char addr[WEB_PATH_MAX];
    snprintf(addr, sizeof(addr), "%s", w->pending);

    Url u;
    char err[WEB_ERR_MAX];
    err[0] = 0;
    if (url_parse(addr, &u, err, sizeof(err)) != 0) {
        web_clear_page(w);
        snprintf(w->error, sizeof(w->error), "%s", err);
        w->state = ST_ERROR;
        return;
    }
    if (!url_fetchable(&u, err, sizeof(err))) {
        web_clear_page(w);
        snprintf(w->error, sizeof(w->error), "%s", err);
        w->state = ST_ERROR;
        return;
    }

    Fetch f;
    web_fetch(&f, &net_transport, &u, w->raw);

    w->raw_bytes = f.raw_bytes;
    w->elapsed_ms = f.elapsed_ms;
    w->truncated = f.truncated;

    if (!f.ok) {
        web_clear_page(w);
        snprintf(w->url, sizeof(w->url), "%s", addr);
        snprintf(w->error, sizeof(w->error), "%s", f.error);
        /* keep the count and the clock: they are real measurements of
         * a failed attempt and the footer should not pretend otherwise */
        w->raw_bytes = f.raw_bytes;
        w->elapsed_ms = f.elapsed_ms;
        w->state = ST_ERROR;
        return;
    }

    w->cur = u;
    snprintf(w->url, sizeof(w->url), "%s", f.url);
    snprintf(w->edit, sizeof(w->edit), "%s", f.url);
    w->tls_ok = f.tls;
    snprintf(w->tls_version, sizeof(w->tls_version), "%s", f.tls_version);
    snprintf(w->tls_cipher, sizeof(w->tls_cipher), "%s", f.tls_cipher);
    snprintf(w->tls_cn, sizeof(w->tls_cn), "%s", f.tls_cn);
    snprintf(w->tls_expiry, sizeof(w->tls_expiry), "%s", f.tls_expiry);
    w->status = f.status;
    w->body_bytes = f.body_bytes;
    w->truncated = f.truncated || (f.body_len >= WEB_RAW_MAX - 1);
    snprintf(w->charset, sizeof(w->charset), "%s", f.charset);

    char title[WEB_TITLE_MAX];
    html_build(&w->doc, f.body, f.body_len, &w->cur, f.latin1, title, sizeof(title));
    snprintf(w->title, sizeof(w->title), "%s", title);

    w->scroll = 0;
    w->laid_w = 0;                 /* force a fresh layout at this width */
    w->state = ST_PAGE;
}

#endif /* !WEB_DRAW_ONLY */         /* web_load: the only part that needs sockets */

/* ------------------------------------------------------------ input */

/* The input handling is deliberately outside the WEB_DRAW_ONLY guard: it
 * touches no socket, and a host harness can drive the wheel, the keys and
 * the clicks against a real rendered frame. That is how the scroll behaviour
 * below is proved without a window. */

static int scroll_step(void) {
    Font *f = font_for(0);
    int h = f ? (int)font_height(f) : 16;
    return h < 8 ? 8 : h;
}

static void scroll_to(Web *w, int y) {
    if (y < 0) y = 0;
    if (y > w->max_scroll) y = w->max_scroll;
    if (y != w->scroll) {
        w->scroll = y;
        w->dirty = 1;
    }
}

static void go_back(Web *w) {
    if (w->hist_i <= 0) return;
    w->hist_i--;
    snprintf(w->pending, sizeof(w->pending), "%s", w->hist[w->hist_i]);
    snprintf(w->edit, sizeof(w->edit), "%s", w->hist[w->hist_i]);
    w->state = ST_LOADING;
    w->dirty = 1;
}

static void follow_link(Web *w, int idx) {
    if (idx < 0 || idx >= w->doc.nlink) return;
    const char *target = w->doc.link[idx].url;
    if (!target[0]) return;
    web_go(w, target, 1);       /* web_go puts the target in the bar */
    w->dirty = 1;
}

static void edit_backspace(Web *w) {
    /* step back one UTF-8 sequence, not one byte */
    int n = (int)strlen(w->edit);
    if (n <= 0) return;
    int i = n - 1;
    while (i > 0 && ((unsigned char)w->edit[i] & 0xC0) == 0x80) i--;
    w->edit[i] = 0;
    w->dirty = 1;
}

static void edit_put(Web *w, const char *t) {
    int n = (int)strlen(w->edit);
    for (int i = 0; t[i]; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x20 || c == 0x7F) continue;
        if (n + 2 >= (int)sizeof(w->edit)) break;
        w->edit[n++] = (char)c;
    }
    w->edit[n] = 0;
    w->dirty = 1;
}

static void submit_address(Web *w) {
    if (!w->edit[0]) {
        snprintf(w->error, sizeof(w->error),
                 "the address bar is empty - type something like https://example.com/");
        w->state = ST_ERROR;
        w->dirty = 1;
        return;
    }
    web_go(w, w->edit, 1);
    w->dirty = 1;
}

static int link_at(Web *w, int lx, int ly) {
    Doc *d = &w->doc;
    if (d->nline <= 0) return -1;
    int first = line_at(d, (u32)w->scroll);
    for (int i = first; i < d->nline; i++) {
        Line *ln = &d->line[i];
        int y = (int)ln->y - w->scroll;
        if (y >= w->view_h) break;
        if (ly < y || ly >= y + (int)ln->h) continue;
        for (int j = 0; j < (int)ln->n; j++) {
            Seg *sg = &d->seg[ln->first + j];
            if (d->atom[sg->atom].link < 0) continue;
            if (lx >= (int)sg->x && lx < (int)sg->x + (int)sg->w)
                return d->atom[sg->atom].link;
        }
    }
    return -1;
}

static void handle_key(Web *w, int key, int mods) {
    if (key == K_R && (mods & 2)) {                 /* Ctrl+R: reload */
        if (w->url[0]) {
            snprintf(w->pending, sizeof(w->pending), "%s", w->url);
            w->state = ST_LOADING;
            w->dirty = 1;
        }
        return;
    }
    /* scroll_to() clamps to max_scroll, which is 0 when there is nothing
     * below the fold, so the keys work on every screen and do nothing
     * where there is nothing to scroll. */
    switch (key) {
        case K_UP:       scroll_to(w, w->scroll - scroll_step()); break;
        case K_DOWN:     scroll_to(w, w->scroll + scroll_step()); break;
        case K_PAGEUP:   scroll_to(w, w->scroll - w->view_h * 9 / 10); break;
        case K_PAGEDOWN: scroll_to(w, w->scroll + w->view_h * 9 / 10); break;
        case K_HOME:     scroll_to(w, 0); break;
        case K_END:      scroll_to(w, w->max_scroll); break;
        default: break;
    }
}

static int handle_text(Web *w, const char *t) {
    for (int i = 0; t[i]; i++) {
        unsigned char c = (unsigned char)t[i];

        if (c == '\t') {                            /* Tab: focus the bar */
            w->edit_focus = !w->edit_focus;
            w->caret_on = 1;
            w->dirty = 1;
            continue;
        }
        if (w->edit_focus) {
            if (c == '\n' || c == '\r') { submit_address(w); continue; }
            if (c == '\b' || c == 0x7F) { edit_backspace(w); continue; }
            char one[2];
            one[0] = (char)c;
            one[1] = 0;
            edit_put(w, one);
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (w->doc.natom == 0 && w->state != ST_LOADING) {
                /* Enter on an empty page: go to the bar, like a browser */
                w->edit_focus = 1;
                w->dirty = 1;
            }
            continue;
        }
        if (c == '\b' || c == 0x7F) { go_back(w); continue; }
        switch (c) {
            case 'j': scroll_to(w, w->scroll + scroll_step()); break;
            case 'k': scroll_to(w, w->scroll - scroll_step()); break;
            case ' ': scroll_to(w, w->scroll + w->view_h * 9 / 10); break;
            case 'q': return 1;
            default: break;
        }
    }
    return 0;
}

static void handle_mouse(Web *w, int x, int y, int buttons) {
    int prev = w->last_buttons;
    w->last_buttons = buttons;
    if (!(buttons & 1)) return;
    if (prev & 1) return;                     /* only the press edge acts */

    /* In a window too short for the header there is nothing to click: the
     * single line render() draws there is a message, not a control. */
    if (!header_fits(w)) return;

    float fx, fy, fw, fh;
    if (addr_field_rect(w, &fx, &fy, &fw, &fh) &&
        x >= (int)fx && x < (int)(fx + fw) && y >= (int)fy && y < (int)(fy + fh)) {
        w->edit_focus = 1;
        w->caret_on = 1;
        w->dirty = 1;
        return;
    }

    PageRect r = page_rect(w);
    if (r.w <= 0 || r.h <= 0) return;           /* no page was drawn */
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return;

    int lx = x - r.x, ly = y - r.y;

    if (lx >= r.w - sbar_w()) {               /* the scroll bar */
        if (w->content_h > r.h) {
            int track = r.h;
            float frac = (float)ly / (float)track;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            scroll_to(w, (int)(frac * (float)w->max_scroll));
        }
        return;
    }

    int link = link_at(w, lx, ly);
    if (link >= 0) follow_link(w, link);
}

/* The wheel, in notches, positive when the user scrolled up. One notch is
 * WHEEL_LINES body lines - a few, so a page is crossed in a handful of
 * flicks - and scroll_to() clamps every request to [0, max_scroll] as it
 * was last measured by render(), so this can never scroll into empty
 * space or mark the frame dirty without having moved something. */
static void handle_wheel(Web *w, int notches) {
    if (notches == 0) return;
    scroll_to(w, w->scroll - notches * scroll_step() * WHEEL_LINES);
}

/* Last resort: if we cannot even allocate our state, say so on screen
 * instead of dying silently. */
#ifndef WEB_DRAW_ONLY
static void render_fatal(MClient *c, const char *what, const char *why) {
    if (!c->px) return;
    Surface s;
    s.px = c->px;
    s.w = c->w;
    s.h = c->h;
    s.stride = c->stride;
    gfx_gradient(&s, 0, 0, c->w, c->h, RGB(0x1D, 0x10, 0x14), RGB(0x2B, 0x16, 0x1B));
    Font *fr = font_get(FONT_REGULAR, 12);
    if (!fr) return;
    draw_fit(&s, fr, (float)page_mx(), (float)page_mx(),
             (float)c->w - 2.0f * (float)page_mx(), what, C_ERR);
    draw_fit(&s, fr, (float)page_mx(), (float)(page_mx() + foot_h(NULL)),
             (float)c->w - 2.0f * (float)page_mx(), why, C_TEXT);
    mc_present(c);
}

int main(int argc, char **argv) {
    MClient c;
    if (mc_open(&c, "web", "Browser", 640, 460) < 0) {
        fprintf(stderr, "web: cannot connect to the window server\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    font_init("/usr/share/fonts");

    if (!font_for(0)) {
        render_fatal(&c, "web: no fonts", "install the Lato TrueType files in /usr/share/fonts");
        usleep(1500 * 1000);
        mc_close(&c);
        return 1;
    }

    Web *w = calloc(1, sizeof(*w));
    if (!w) {
        int saved = errno;
        fprintf(stderr, "web: out of memory: %s\n", strerror(saved));
        render_fatal(&c, "web: cannot start - out of memory", strerror(saved));
        usleep(1500 * 1000);
        mc_close(&c);
        return 1;
    }

    w->c = &c;
    w->w = c.w;
    w->h = c.h;
    w->maxw = c.stride;
    w->maxh = c.h;
    w->state = ST_START;
    w->hist_i = -1;
    w->caret_on = 1;
    w->dirty = 1;

    /* 'web http://example.com' from a terminal starts on that page. It also
     * makes the whole load path testable without clicking anything. */
    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(w->edit, sizeof(w->edit), "%s", argv[1]);
        web_go(w, argv[1], 1);
    }

    int running = 1;
    while (running) {
        if (w->dirty) {
            render(w);
            mc_present(&c);
            w->dirty = 0;
        }
        if (w->state == ST_LOADING) {
            web_load(w);          /* blocking, bounded by the socket timeouts */
            w->dirty = 1;
            continue;
        }

        struct pollfd p;
        p.fd = c.fd;
        p.events = POLLIN;
        p.revents = 0;
        int timeout = w->edit_focus ? 450 : 1000;
        int r = poll(&p, 1, timeout);
        if (r == 0) {
            if (w->edit_focus) {          /* blink the caret */
                w->caret_on = !w->caret_on;
                w->dirty = 1;
            }
            continue;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        MEvent ev;
        while (mc_poll(&c, &ev)) {
            if (ev.type == MC_CLOSE) { running = 0; break; }
            if (ev.type == MC_TEXT) {
                if (handle_text(w, ev.text)) { running = 0; break; }
                w->dirty = 1;
            } else if (ev.type == MC_KEY) {
                if (ev.press) {
                    handle_key(w, ev.key, ev.mods);
                    w->dirty = 1;
                }
            } else if (ev.type == MC_MOUSE) {
                handle_mouse(w, ev.x, ev.y, ev.buttons);
            } else if (ev.type == MC_WHEEL) {
                /* positive = up, as the server sends it */
                handle_wheel(w, ev.wheel);
            } else if (ev.type == MC_RESIZE) {
                /* the mapping cannot grow; clamp so a resize can never
                 * make us write outside the pixels we own */
                int nw = ev.w < w->maxw ? ev.w : w->maxw;
                int nh = ev.h < w->maxh ? ev.h : w->maxh;
                if (nw > 0 && nh > 0) {
                    w->w = nw;
                    w->h = nh;
                    w->dirty = 1;
                }
            }
        }
    }

    mc_close(&c);
    free(w);
    return 0;
}

#endif /* !WEB_DRAW_ONLY */      /* render_fatal + main */

#endif /* !WEB_HOST_TEST */
