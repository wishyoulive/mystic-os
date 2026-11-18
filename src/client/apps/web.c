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
 *  It renders a real CSS subset: colours, text alignment, line height,
 *  and block boxes with margins, padding, top and bottom borders,
 *  backgrounds and centred max-width columns (src/css.c). It has no
 *  JavaScript and no layout beyond ordinary block flow - no floats, flex
 *  or grid - and the type scale is the built-in h1..h6 one rather than an
 *  author font-size rule; that is the honest description of what is
 *  implemented.
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
#include "../../css.h"
/* Img is embedded in PageImg even in the WEB_HOST_TEST build, where img.c
 * is not compiled in; without this the bare -DWEB_HOST_TEST translation
 * unit does not know the type. */
#include "../../img.h"

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
#include "../../img.c"
#include "../../css.c"
#endif

#ifndef WEB_HOST_TEST
#include <signal.h>
#include <unistd.h>
/* The clipboard as a sandboxed app sees it: one socket the desktop binds into
 * this process's jail. Header-only, because a bundle is built from its own .c
 * file and cannot add a library to the link line. */
#include "clipbridge.h"
#ifndef WEB_DRAW_ONLY            /* the socket layer's headers */
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
#endif

#if defined(WEB_HOST_TEST)
/* This switch builds a library, not a program: a harness #includes this
 * file and calls the fetch/parser/layout code itself. Compiled on its own
 * with -c, nothing calls those statics yet, so the unused warnings would
 * only say that no harness is attached. Silenced for this switch alone;
 * the OS build and WEB_DRAW_ONLY keep every warning on. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#endif

/* ------------------------------------------------------------
 * Limits. Every buffer in this file is one of these, and every
 * write into one is bounds checked - the network is not trusted.
 * ------------------------------------------------------------ */
#define WEB_RAW_MAX     (512 * 1024)  /* response bytes we will hold   */
#define WEB_IMG_MAX     (384 * 1024)  /* largest image we will decode  */
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
#define WEB_MAX_BLOCKS  2048          /* block boxes per document      */
#define WEB_BLOCK_DEPTH 64            /* nesting depth we can cascade  */

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

/* The document's vertical rhythm, in design pixels. Block boxes take
 * their margins from the cascade (the built-in sheet gives p 12 px and
 * every heading 16 px); GAP_BLOCK_D is the fallback for a block tag the
 * sheet says nothing about, so <div> keeps the spacing readers know.
 * Layout applies the text scale when it places a line, so a heading
 * keeps its proportions when the desktop is zoomed. */
#define GAP_BLOCK_D       6   /* fallback margin for an unstyled block */
#define GAP_LI_D          2   /* list items follow each other closely  */
#define GAP_AFTER_HEAD_D 10   /* space below the help screen's title   */

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
#define K_A       30
#define K_C       46
#define K_V       47

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
#define AF_IMAGE   8     /* an inline <img> box: text is empty    */
#define AF_ALT    16     /* the alt text, laid out only if the    */
                         /* image could not be fetched/decoded    */
#define AF_HARD   32     /* a real newline (pre) or blank line    */

typedef struct {
    char text[WEB_ATOM_TEXT];
    u8   level;                      /* 0 body, 1..6 heading          */
    u8   flags;                      /* AF_*                          */
    u8   gap;                        /* px of space above, with AF_BREAK */
    /* The resolved run style. css.c computes these per element; carrying
     * them on the atom is what lets one paragraph contain bold, italic,
     * underlined and struck words instead of one face for the block. */
    u16  weight;                     /* 100..900; >=600 -> semibold   */
    u8   italic;                     /* synthesised by shear (no face)*/
    u8   decoration;                 /* CSS_TD_* bits                 */
    u8   ws;                         /* CSS_WS_* at this run          */
    u8   mono;                       /* pre/code/kbd/samp/tt          */
    u16  block;                      /* enclosing block, 0xFFFF none  */
    int16_t link;                    /* index into Doc.link, -1 none  */
    int16_t img;                     /* index into Doc.img, -1 none   */
} Atom;

typedef struct {
    u16 atom;                        /* index into Doc.atom           */
    u16 start;                       /* first byte inside atom->text  */
    u16 len;                         /* byte count                    */
    u16 x;                           /* page px, from the document edge */
    u16 w;                           /* measured width in px          */
} Seg;

typedef struct {
    u32 first;                       /* first Seg                    */
    u32 y;                           /* px from the top of the document */
    u16 n;                           /* Seg count                    */
    u16 h;                           /* line box height in px        */
    u16 th;                          /* tallest face on the line     */
    u16 gap;                         /* space above the line         */
    u16 block;                       /* block whose column it sits in */
    u16 x0, x1;                      /* the column this line was wrapped in */
} Line;

/* One block box: the part of CSS that is real vertical space, a real
 * background and a real rule. The parser records the metrics in design
 * pixels and the atom range; doc_layout resolves the column (width and
 * max-width against the window) and fills the y/line geometry; the
 * painter draws the background and the two borders. Margins collapse
 * between boxes the way CSS says they do: max(previous bottom, next top). */
typedef struct {
    u16 atom_start, atom_end;        /* [start, end) in Doc.atom      */
    u16 line_start, line_end;        /* [start, end) in Doc.line      */
    u16 parent;                      /* enclosing block, 0xFFFF none  */

    int16_t margin_top, margin_bottom; /* design px                   */
    int16_t pad_top, pad_bottom;       /* design px                   */
    /* margin-left/padding-left and their right twin are one column's
     * inset: the box starts this far in and wraps that much earlier. */
    u16 inset_left, inset_right;     /* design px, margin + padding   */
    u16 width, max_width;            /* design px; 0 = auto/none      */
    u16 width_pct, max_width_pct;    /* 0..1000; 0 = unset            */

    u16 box_x0, box_x1;              /* the block's column, page px   */
    u32 box_y0, box_y1;              /* its border box, doc px        */
    u32 content_y0, content_y1;      /* its content box, doc px       */

    u32 background;                  /* 0x00000000 = none             */
    u32 color;                       /* resolved text colour          */
    u32 border_top_color, border_bottom_color;
    u8  border_top_w, border_bottom_w;
    u8  border_top_style, border_bottom_style;
    u8  align;                       /* CSS_ALIGN_*                   */
    u8  level;                       /* 0..6, the type face           */
    u16 line_height;                 /* design px; 0 = face height    */
    u8  has_top;                     /* box_y0 already fixed          */
    u8  is_body;                     /* <body>: its background is the canvas */
    u8  has_color;                   /* the page (or an ancestor) set colour */
    u16 indent;                      /* design px from ul/ol/blockquote/dd */
} Block;

typedef struct {
    char url[WEB_PATH_MAX];
} Link;

/* Every stylesheet the document references, in document order: either a
 * <style> element's text (an offset into the collected buffer) or an
 * external <link rel=stylesheet> still to be fetched. The browser walks
 * this list after the first parse pass, so a sheet that appears after a
 * <style> in the source also cascades after it - which is what "later
 * wins" means for two rules of equal specificity. */
typedef struct {
    int  is_link;                   /* 1: fetch url; 0: css[off..off+len) */
    int  off, len;                  /* <style> bytes in the text buffer   */
    char url[WEB_PATH_MAX];         /* <link href>, already absolute      */
    char media[32];                 /* <link media=...>                   */
} CssSrc;

/* WEB_MAX_CSSSRC bounds one document's stylesheet references. A real site
 * links two or three; eight is the cap the browser states in its footer
 * when it stops. */
#define WEB_MAX_CSSSRC   8
#define WEB_CSS_BYTES    (256 * 1024)   /* total fetched sheet bytes      */

#define WEB_MAX_IMGS 8

/* One <img> the page referenced. The URL is resolved against the document
 * base; the pixels arrive later, after the body has been fetched. */
typedef struct {
    char  url[WEB_PATH_MAX];
    char  alt[96];
    Img   pic;          /* decoded; pic.px == NULL until it is fetched   */
    int   tried;        /* fetch attempted - do not retry every frame    */
} PageImg;

typedef struct {
    Atom atom[WEB_MAX_ATOMS];
    int  natom;
    Seg  seg[WEB_MAX_SEGS];
    int  nseg;
    Line line[WEB_MAX_LINES];
    int  nline;
    Block block[WEB_MAX_BLOCKS];
    int  nblock;
    Link link[WEB_MAX_LINKS];
    int  nlink;
    CssSrc csssrc[WEB_MAX_CSSSRC];
    int  ncsssrc;
    u8   css_capped;                 /* a sheet reference was dropped  */
    u32  doc_h;
    int  truncated;                  /* a cap was hit; say so in the footer */
    PageImg img[WEB_MAX_IMGS];
    int  nimg;
} Doc;

/* How layout measures text. The app passes the real Font objects in;
 * the host harness passes the same font_width() with real Lato files,
 * so the wrap points it reports are the real ones. */
typedef struct {
    void *ctx;
    float (*text_w)(void *ctx, const Atom *a, const char *s, int n);
    float (*line_h)(void *ctx, const Atom *a);
    /* Ink a run paints past its upright advance. A synthesised italic is
     * sheared, so its glyph tops lean right of the advance font_width()
     * reports; layout has to reserve that or the next word is touched.
     * NULL or 0 for an upright run. */
    float (*overhang)(void *ctx, const Atom *a);
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
    long base = 0;
    /* A server may send one or more 1xx interim responses before the real
     * one (100 Continue, 103 Early Hints). Each has its own header block;
     * skip it and parse the next, instead of reading the interim block as
     * if it were the final response. */
    for (;;) {
        hdr_end = -1;
        sep_len = 4;
        for (long i = base; i + 3 < rawlen; i++) {
            if (raw[i] == '\r' && raw[i + 1] == '\n' &&
                raw[i + 2] == '\r' && raw[i + 3] == '\n') {
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
        if (hdr_end - base < 5 || strncmp(raw + base, "HTTP/", 5)) return -1;

        long sp = base + 5;
        while (sp < hdr_end && raw[sp] != ' ') sp++;
        if (sp >= hdr_end) return -1;
        r->status = atoi(raw + sp + 1);
        if (r->status < 100 || r->status > 599) return -1;
        if (r->status >= 100 && r->status < 200) {
            base = hdr_end + sep_len;
            if (base >= rawlen) return -1;
            memset(r, 0, sizeof(*r));
            r->content_length = -1;
            continue;
        }
        break;
    }

    /* position just past the status line: the first header */
    long pos = base;
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
    int   tls_chain_ok;
    char  tls_version[16];
    char  tls_cipher[48];
    char  tls_cn[128];
    char  tls_subject[200];
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
        f->tls_chain_ok = 0;
        f->tls_version[0] = 0;
        f->tls_cipher[0] = 0;
        f->tls_cn[0] = 0;
        f->tls_subject[0] = 0;
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
            f->elapsed_ms += now_ms() - t0;
            snprintf(f->error, sizeof(f->error), "%s", err[0] ? err : "cannot connect");
            return;
        }

        /* Take the session facts now: the connection is closed below. */
        const TlsInfo *ti = tr->tls ? tr->tls(st) : NULL;
        if (ti) {
            f->tls = 1;
            f->tls_chain_ok = ti->chain_ok;
            snprintf(f->tls_version, sizeof(f->tls_version), "%s", ti->version);
            snprintf(f->tls_cipher, sizeof(f->tls_cipher), "%s", ti->cipher);
            snprintf(f->tls_cn, sizeof(f->tls_cn), "%s", ti->cn);
            snprintf(f->tls_subject, sizeof(f->tls_subject), "%s", ti->subject);
            snprintf(f->tls_expiry, sizeof(f->tls_expiry), "%s", ti->not_after);
        }
        if (tr->send(st, req, (long)strlen(req), err, sizeof(err)) < 0) {
            tr->close(st);
            f->elapsed_ms += now_ms() - t0;
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
                f->elapsed_ms += now_ms() - t0;
                f->raw_bytes += total;
                snprintf(f->error, sizeof(f->error), "%s", err[0] ? err : "cannot read the response");
                return;
            }
            if (n == 0) break;
            total += n;
        }
        tr->close(st);

        /* Every leg of a redirect, and every read, is added to the same
         * totals: the footer describes the whole load, not the last
         * response. */
        f->elapsed_ms += now_ms() - t0;
        f->raw_bytes += total;
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
    char src[WEB_PATH_MAX];      /* <img src>  */
    char alt[96];
    /* These are the attribute values, not selector tokens: a real page
     * carries several classes ("mw-content-ltr mw-parser-output"), so the
     * buffers are wider than CSS_CLASS_MAX/CSS_ID_MAX on purpose. */
    char cls[96];
    char id[48];
    char rel[32];                /* <link rel=stylesheet>          */
    char media[32];              /* <link media=screen>            */
    char style[256];             /* the inline style="..." element */
    int  align;                  /* align= hint: 0/1/2/3 = left/center/right/justify */
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
            } else if (!strcmp(an, "src") && vl < (long)sizeof(t->src)) {
                memcpy(t->src, s + vs, (size_t)vl);
                t->src[vl] = 0;
            } else if (!strcmp(an, "class") && vl < (long)sizeof(t->cls)) {
                memcpy(t->cls, s + vs, (size_t)vl);
                t->cls[vl] = 0;
            } else if (!strcmp(an, "id") && vl < (long)sizeof(t->id)) {
                memcpy(t->id, s + vs, (size_t)vl);
                t->id[vl] = 0;
            } else if (!strcmp(an, "rel") && vl < (long)sizeof(t->rel)) {
                memcpy(t->rel, s + vs, (size_t)vl);
                t->rel[vl] = 0;
            } else if (!strcmp(an, "media") && vl < (long)sizeof(t->media)) {
                memcpy(t->media, s + vs, (size_t)vl);
                t->media[vl] = 0;
            } else if (!strcmp(an, "style") && vl < (long)sizeof(t->style)) {
                /* The inline style is applied to the element's resolved
                 * style in html_elem_push(), as the author's last word. */
                memcpy(t->style, s + vs, (size_t)vl);
                t->style[vl] = 0;
            } else if (!strcmp(an, "align")) {
                if (ci_eq(s + vs, vl, "center")) t->align = 1;
                else if (ci_eq(s + vs, vl, "right")) t->align = 2;
                else if (ci_eq(s + vs, vl, "justify")) t->align = 3;
            }
        }
    }
}

typedef struct {
    char   tag[CSS_TAG_MAX];
    char   cls[96];
    char   id[48];
    unsigned hidden;             /* display:none here or on an ancestor */
    CssNode node;                /* style resolved with its ancestors */
} HtmlElem;

typedef struct {
    char tag[CSS_TAG_MAX];
    int  block;                  /* index into Doc.block, -1 if capped */
    int  elem;                   /* index into Html.elem, -1 if capped */
    int  ordinal;                /* li counter, for ol/ul elements     */
} HtmlOpen;

typedef struct {
    Doc        *d;
    const Url  *base;
    char       *title;
    size_t      title_cap;
    int         title_len;
    int         latin1;
    int         in_title;
    int         skip;            /* inside <script>/<style>       */
    int         in_style;        /* collecting a <style> body     */
    const CssSheet *sheet;       /* the page's cascade            */
    char       *css;             /* where that text is collected  */
    size_t      css_cap;
    size_t     *css_len;
    int         level;           /* 0 body, 1..6 heading          */
    int         in_link;         /* link index, -1 when outside   */
    int         pending_break;
    int         pending_gap;
    int         pending_space;
    int         glue_next;       /* the next atom continues a split run */
    int         pre_seen;        /* a <pre> line has content      */
    int         pre_break;       /* a hard break was emitted      */
    int         pre_col;         /* character column in a pre line,
                                  * so a tab reaches the next CSS
                                  * tab stop (tab-size: 8)        */
    char        word[WEB_ATOM_TEXT];
    int         wlen;
    int         overflow;        /* a cap was hit                 */
    /* display:none. `hidden` is the number of open elements whose own
     * resolved display is none plus every hidden ancestor: a subtree is
     * hidden by the topmost element that says so, and every descendant
     * inherits that without a rule of its own. Text, pictures, link
     * markers and block boxes inside it are not created at all. */
    int         hidden;
    int         style_off;       /* css offset where the open <style> began */
    int         style_open;      /* <style> body currently being collected */
    HtmlElem    elem[WEB_BLOCK_DEPTH];
    int         nelem;
    HtmlOpen    open[WEB_BLOCK_DEPTH];
    int         nopen;
} Html;

static int level_for_px(int px);   /* defined with the other style helpers */

static int is_heading_tag(const char *n) {
    return n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2];
}

/* The innermost enclosing block box. Inline elements now live on the same
 * open stack (that is what lets one word be bold and the next regular),
 * so "the current block" is the nearest entry that owns one. */
static int html_block_cur(const Html *h) {
    for (int i = h->nopen - 1; i >= 0; i--)
        if (h->open[i].block >= 0) return h->open[i].block;
    return -1;
}

static const CssStyle *html_top_style(const Html *h) {
    if (h->nelem <= 0) return NULL;
    return &h->elem[h->nelem - 1].node.style;
}

/* Is the innermost open element inside a display:none subtree? Text and
 * boxes inside one are never created, which is what makes
 * `display:none` hide a whole nav block instead of one line of it. */
static int html_hidden(const Html *h) {
    return h->nelem > 0 && h->elem[h->nelem - 1].hidden;
}

static int html_white_space(const Html *h) {
    const CssStyle *cs = html_top_style(h);
    return cs ? cs->white_space : CSS_WS_NORMAL;
}

/* white-space values that keep literal spaces and tabs (pre, pre-wrap). */
static int html_keeps_spaces(const Html *h) {
    int ws = html_white_space(h);
    return ws == CSS_WS_PRE || ws == CSS_WS_PRE_WRAP;
}

/* white-space values that keep newlines (pre, pre-wrap, pre-line). */
static int html_keeps_lines(const Html *h) {
    int ws = html_white_space(h);
    return ws == CSS_WS_PRE || ws == CSS_WS_PRE_WRAP || ws == CSS_WS_PRE_LINE;
}

/* Fill one atom. Style is read from the innermost open element, so a run
 * created inside <b><i> carries both facts to the painter. A zero-length
 * atom is a box, not text, and only two flags make one: a hard line break
 * (AF_BREAK) and an inline picture (AF_IMAGE). Treating the image box as
 * empty text dropped it, so a picture in the middle of a line had no box
 * at the <img> position and its alt text was printed even though the
 * pixels had been decoded. */
static void html_atom(Html *h, const char *text, long len, int flags,
                      int link, int img) {
    Doc *d = h->d;
    if (len < 0) return;
    if (html_hidden(h)) return;              /* display:none subtree */
    if (len == 0 && !(flags & (AF_BREAK | AF_IMAGE))) return;
    if (len >= WEB_ATOM_TEXT) len = WEB_ATOM_TEXT - 1;
    if (d->natom >= WEB_MAX_ATOMS) { h->overflow = 1; d->truncated = 1; return; }
    Atom *a = &d->atom[d->natom++];
    if (len > 0) memcpy(a->text, text, (size_t)len);
    a->text[len] = 0;

    /* The innermost block owns the type size; the innermost element owns
     * weight, slant, decoration, white-space and the monospace face. */
    int bi = html_block_cur(h);
    a->level = (u8)(bi >= 0 ? d->block[bi].level : (h->level ? h->level : 0));
    const CssStyle *cs = html_top_style(h);
    int w = cs ? cs->weight : CSS_WEIGHT_NORMAL;
    if (w < 100) w = 100;
    if (w > 900) w = 900;
    /* Atom.weight is a u16 because CSS weights are 100..900: narrowing
     * this through a u8 truncated 400 to 144 and 700 to 188, so every
     * run failed font_for_atom()'s >= 600 test and bold never existed. */
    a->weight = (u16)w;
    a->italic = (u8)(cs && cs->italic);
    /* css.c resolves text-decoration for one element and *assigns* it, so
     * a nested element that declares its own bit erases the outer one:
     * `<u><s>x</s></u>` came out line-through only, and a link inside <s>
     * lost the strike. CSS does not inherit text-decoration; it
     * propagates the ancestor's decoration into the inline subtree and no
     * descendant can cancel it. Union the open element chain here, where
     * the run that will be painted is formed. */
    int dec = cs ? cs->decoration : 0;
    for (int i = 0; i < h->nopen; i++) {
        int ei = h->open[i].elem;
        if (ei >= 0) dec |= h->elem[ei].node.style.decoration;
    }
    a->decoration = (u8)dec;
    a->ws = (u8)(cs ? cs->white_space : CSS_WS_NORMAL);
    a->mono = (u8)(cs && cs->monospace);
    a->flags = (u8)flags;
    if (h->glue_next) {
        a->flags |= AF_GLUE;
        h->glue_next = 0;
    }
    int gap = 0;
    if (flags & AF_BREAK) {
        gap = h->pending_gap;
        if (gap > 255) gap = 255;
    }
    a->gap = (u8)gap;
    a->block = (u16)(bi < 0 ? 0xFFFF : bi);
    a->link = (int16_t)link;
    a->img = (int16_t)img;
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
    html_atom(h, h->word, h->wlen, flags, h->in_link, -1);
    h->wlen = 0;
    h->pending_break = 0;
    h->pending_gap = 0;
    h->pending_space = 0;
}

/* Flush the pending word at an opening inline tag without eating the
 * space that came before the tag: "foo <b>bar" must still read with one
 * space, and the word after </b> must not be glued to the one before. */
static void html_flush_before_inline(Html *h) {
    int space = h->pending_space;
    html_end_word(h, 0);
    if (space) h->pending_space = 1;
}

static void html_put(Html *h, const char *bytes, long len) {
    if (html_hidden(h)) return;              /* display:none subtree */
    for (long i = 0; i < len; i++) {
        if (h->wlen >= WEB_ATOM_TEXT - 2) {
            /* one word too long for one atom: continue it in the next,
             * glued so no space appears where the split happened */
            h->word[h->wlen] = 0;
            int flags = 0;
            if (h->pending_break) flags |= AF_BREAK;
            html_atom(h, h->word, h->wlen, flags, h->in_link, -1);
            h->wlen = 0;
            h->pending_break = 0;
            h->pending_gap = 0;
            h->pending_space = 0;
            h->glue_next = 1;
            h->word[h->wlen++] = bytes[i];
            continue;
        }
        h->word[h->wlen++] = bytes[i];
    }
}

static void html_break(Html *h, int gap) {
    html_end_word(h, 0);
    h->pending_break = 1;
    h->pre_col = 0;              /* whatever starts now is column 0 */
    if (gap > h->pending_gap) h->pending_gap = gap;
}

/* A real newline inside a pre block: an atom, not a pending flag, so a
 * blank line is distinguishable from a line that has not started. */
static void html_hard_break(Html *h) {
    html_atom(h, "", 0, AF_BREAK | AF_HARD, -1, -1);
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
    html_atom(h, text, len, flags, link, -1);
    h->pending_break = 0;
    h->pending_gap = 0;
    h->pending_space = 0;
}

/* Text between tags: decode entities, collapse whitespace, transcode
 * Latin-1 when the server said so, and preserve it all inside pre. */
static void html_text(Html *h, const char *s, long n) {
    if (h->skip) {
        /* A <style> body IS the stylesheet: keep it (bounded), and keep
         * dropping <script> bodies. */
        if (h->in_style && h->css && h->css_len) {
            size_t room = h->css_cap - *h->css_len - 1;
            size_t take = (size_t)n < room ? (size_t)n : room;
            if (take) {
                memcpy(h->css + *h->css_len, s, take);
                *h->css_len += take;
                h->css[*h->css_len] = 0;
            }
        }
        return;
    }

    const int keep_spaces = html_keeps_spaces(h);
    const int keep_lines = html_keeps_lines(h);

    long i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        if (c == '&') {
            char dec[8];
            long used = 0, outlen = 0;
            if (entity_decode(s + i, n - i, dec, sizeof(dec), &used, &outlen)) {
                if (h->in_title) html_title_put(h, dec, outlen);
                else if (outlen == 1 && dec[0] == ' ') {
                    if (keep_spaces) {
                        html_put(h, " ", 1);        /* &nbsp; is a real space */
                        h->pre_seen = 1;
                        h->pre_col++;
                    } else {
                        html_end_word(h, 0);        /* &nbsp; ends the word */
                        h->pending_space = 1;
                    }
                } else {
                    html_put(h, dec, outlen);
                    if (keep_spaces) { h->pre_seen = 1; h->pre_col++; }
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
            if (h->in_title) {
                html_title_put(h, " ", 1);
                i++;
                continue;
            }
            if (c == '\n' || c == '\r') {
                if (keep_lines) {
                    if (c == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
                    h->pre_col = 0;          /* a new line starts at column 0 */
                    if (h->pre_seen || h->wlen > 0) {
                        html_end_word(h, 0);
                        html_hard_break(h);
                        h->pre_seen = 0;
                        h->pre_break = 1;
                    } else if (h->pre_break) {
                        /* the previous newline ended a line, so this one is
                         * a blank line and must keep its height */
                        html_hard_break(h);
                    } else {
                        /* the newline right after <pre> is not a line */
                        h->pre_break = 1;
                    }
                    i++;
                    continue;
                }
                html_end_word(h, 0);
                h->pending_space = 1;
                i++;
                continue;
            }
            if (keep_spaces) {
                /* white-space: pre keeps every space, and a tab advances to
                 * the next tab stop. CSS's initial tab-size is 8 spaces, so
                 * one column counter is enough in the monospace face a pre
                 * block is set in; it is counted in characters, which is what
                 * tab-size means. */
                int count = 1;
                if (c == '\t') {
                    count = 8 - (h->pre_col % 8);
                    if (count <= 0) count = 8;
                }
                for (int k = 0; k < count; k++) html_put(h, " ", 1);
                h->pre_seen = 1;
                h->pre_col += count;
                i++;
                continue;
            }
            html_end_word(h, 0);                /* end the word, then */
            h->pending_space = 1;               /* space the next one */
            i++;
            continue;
        }

        if (c < 0x20) { i++; continue; }

        if (c < 0x80) {
            if (h->in_title) html_title_put(h, (const char *)&c, 1);
            else html_put(h, (const char *)&c, 1);
            i++;
            if (keep_spaces) { h->pre_seen = 1; h->pre_col++; }
            continue;
        }

        if (h->latin1) {
            char u[2];
            u[0] = (char)(0xC0 | (c >> 6));
            u[1] = (char)(0x80 | (c & 0x3F));
            if (h->in_title) html_title_put(h, u, 2);
            else html_put(h, u, 2);
            i++;
            if (keep_spaces) { h->pre_seen = 1; h->pre_col++; }
            continue;
        }

        int len = utf8_len(c);
        if (len > n - i) len = (int)(n - i);
        if (h->in_title) html_title_put(h, s + i, len);
        else html_put(h, s + i, len);
        i += len;
        if (keep_spaces) { h->pre_seen = 1; h->pre_col++; }
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

/* Elements with no end tag. They never go on the open stack. */
static int is_void_tag(const char *n) {
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; voids[i]; i++)
        if (!strcmp(n, voids[i])) return 1;
    return 0;
}

/* Does a <link media> / <style media> value apply to a screen? Same
 * policy as css.c's @media: media TYPES only, and a query that narrows on
 * a viewport feature is not one a text browser can honour, so it is not
 * applied. */
static int media_ok(const char *m) {
    /* One policy, shared with css.c's @media evaluator: a <link media>
     * is the same question as an @media block. */
    return css_media_matches(m);
}

/* Record one stylesheet reference in document order. A <style> element
 * passes the byte range its body occupies in the collected text; a <link>
 * passes the absolute URL. Returns 1 if the reference was kept. */
static int html_add_csssrc(Html *h, int is_link, int off, int len,
                           const char *url, const char *media) {
    Doc *d = h->d;
    if (d->ncsssrc >= WEB_MAX_CSSSRC) { d->css_capped = 1; return 0; }
    CssSrc *s = &d->csssrc[d->ncsssrc++];
    s->is_link = is_link;
    s->off = off;
    s->len = len;
    s->url[0] = 0;
    s->media[0] = 0;
    if (url) snprintf(s->url, sizeof(s->url), "%s", url);
    if (media) snprintf(s->media, sizeof(s->media), "%s", media);
    return 1;
}

/* <link rel=stylesheet href=...>. The href is resolved against the page
 * it came from by url_resolve(), the resolver the rest of the document
 * uses (relative, absolute and //host/x.css all land on a real address);
 * the bytes are fetched after the first parse pass, because the browser
 * has one connection and the document is what names them. */
static void html_add_stylesheet(Html *h, const Tag *t) {
    if (!t->href[0]) return;
    /* A stylesheet is a resource, not content: a <link> inside <head> -
     * which the built-in sheet hides with display:none - still has to be
     * fetched. Only the media it applies to can exclude it. */
    /* rel is a space-separated token list: "stylesheet", "alternate
     * stylesheet", "preload". Anything without the word is not a sheet. */
    if (!ci_has(t->rel, (long)strlen(t->rel), "stylesheet")) return;
    if (ci_has(t->rel, (long)strlen(t->rel), "alternate")) return;
    if (!media_ok(t->media)) return;

    char abs[WEB_PATH_MAX];
    char err[64];
    err[0] = 0;
    url_resolve(h->base, t->href, abs, sizeof(abs), err, sizeof(err));
    if (!abs[0]) return;
    html_add_csssrc(h, 1, 0, 0, abs, t->media);
}

static void html_add_link(Html *h, const Tag *t) {
    if (h->in_link >= 0) return;              /* nested <a>: keep the outer */
    if (html_hidden(h)) { h->in_link = -1; return; }
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

/* The marker for one list item. Decimal, the alphabets and the roman
 * numerals are all in css.h's list-style-type; the three bullet shapes
 * are the ones the built-in sheet asks for. */
static void list_marker(int style, int n, char *out, size_t cap) {
    static const char *roman[][2] = {
        { "M", "1000" }, { "CM", "900" }, { "D", "500" }, { "CD", "400" },
        { "C", "100" }, { "XC", "90" }, { "L", "50" }, { "XL", "40" },
        { "X", "10" }, { "IX", "9" }, { "V", "5" }, { "IV", "4" },
        { "I", "1" }, { NULL, NULL }
    };
    static const int rval[] = { 1000, 900, 500, 400, 100, 90, 50, 40,
                                10, 9, 5, 4, 1 };
    out[0] = 0;
    switch (style) {
        case CSS_LIST_DISC:   snprintf(out, cap, "\xE2\x80\xA2"); break;
        case CSS_LIST_CIRCLE: snprintf(out, cap, "\xE2\x88\x98"); break;
        case CSS_LIST_SQUARE: snprintf(out, cap, "\xE2\x96\xAA"); break;
        case CSS_LIST_DECIMAL: snprintf(out, cap, "%d.", n); break;
        case CSS_LIST_LOWER_ALPHA:
        case CSS_LIST_UPPER_ALPHA: {
            char a[16];
            int k = 0, v = n > 0 ? n : 1;
            while (v > 0 && k < (int)sizeof(a) - 1) {
                a[k++] = (char)('a' + (v - 1) % 26);
                v = (v - 1) / 26;
            }
            char b[20];
            int j = 0;
            while (k > 0) b[j++] = a[--k];
            b[j++] = '.';
            b[j] = 0;
            if (style == CSS_LIST_UPPER_ALPHA)
                for (int i = 0; b[i]; i++)
                    if (b[i] >= 'a' && b[i] <= 'z') b[i] = (char)(b[i] - 'a' + 'A');
            snprintf(out, cap, "%s", b);
            break;
        }
        case CSS_LIST_LOWER_ROMAN:
        case CSS_LIST_UPPER_ROMAN: {
            char b[32];
            size_t o = 0;
            int v = n > 0 ? n : 1;
            if (v > 3999) v = 3999;
            for (int i = 0; rval[i] && o + 6 < sizeof(b); i++) {
                while (v >= rval[i]) {
                    size_t l = strlen(roman[i][0]);
                    memcpy(b + o, roman[i][0], l);
                    o += l;
                    v -= rval[i];
                }
            }
            b[o++] = '.';
            b[o] = 0;
            if (style == CSS_LIST_UPPER_ROMAN)
                for (size_t i = 0; b[i]; i++)
                    if (b[i] >= 'a' && b[i] <= 'z') b[i] = (char)(b[i] - 'a' + 'A');
            snprintf(out, cap, "%s", b);
            break;
        }
        default: break;
    }
}

/* Open one element: always a style node, plus a block box when the tag
 * asks for one. Inline elements share the same stack, which is what makes
 * `foo<b>bar</b>baz` carry two different weights to the atom level. */
static void html_elem_push(Html *h, const Tag *t, int make_block, int level) {
    Doc *d = h->d;
    int ei = -1, bi = -1;

    if (h->nopen < WEB_BLOCK_DEPTH && h->nelem < WEB_BLOCK_DEPTH) {
        ei = h->nelem++;
        HtmlElem *e = &h->elem[ei];
        snprintf(e->tag, sizeof e->tag, "%s", t->name);
        snprintf(e->cls, sizeof e->cls, "%s", t->cls);
        snprintf(e->id, sizeof e->id, "%s", t->id);

        unsigned attrs = 0;
        if (t->align == 1) attrs |= CSS_ATTR_CENTER;
        else if (t->align == 2) attrs |= CSS_ATTR_RIGHT;
        else if (t->align == 3) attrs |= CSS_ATTR_JUSTIFY;
        const CssNode *up = ei > 0 ? &h->elem[ei - 1].node : NULL;
        css_node_init(&e->node, e->tag, e->cls[0] ? e->cls : NULL,
                      e->id[0] ? e->id : NULL, attrs, up, h->sheet);
        /* The author's last word on this element is its own style="..."
         * attribute: it beats every sheet rule, and the descendants that
         * inherit from this element see the result. */
        if (t->style[0])
            css_inline_for(&e->node.style, t->style, strlen(t->style),
                           up ? &up->style : NULL);
        e->hidden = (unsigned)(ei > 0 ? h->elem[ei - 1].hidden : 0);
        if (e->node.style.display == CSS_DISPLAY_NONE) e->hidden = 1;
    }

    const CssStyle *cs = ei >= 0 ? &h->elem[ei].node.style
                                 : html_top_style(h);

    /* display:none hides the whole subtree, so a hidden element does not
     * open a box either: nothing inside it can produce a line. */
    if (make_block && html_hidden(h)) bi = -1;
    /* `display:block` on a tag that is inline by default really is a
     * block box - nav links written as <a> are the usual case. */
    else if (!make_block && cs && (cs->display == CSS_DISPLAY_BLOCK ||
                                   cs->display == CSS_DISPLAY_LIST_ITEM))
        make_block = 1;

    if (make_block && d->nblock < WEB_MAX_BLOCKS) {
        bi = d->nblock++;
        Block *b = &d->block[bi];
        memset(b, 0, sizeof *b);
        b->atom_start = (u16)d->natom;
        b->atom_end = (u16)d->natom;
        b->line_start = 0xFFFF;
        b->line_end = 0xFFFF;
        int parent_block = html_block_cur(h);
        b->parent = (u16)(parent_block >= 0 ? parent_block : 0xFFFF);
        /* A block tag with no margin in the sheet keeps the spacing
         * the reader is used to (GAP_BLOCK_D); a sheet that says 0
         * gets 0, because CSS_SET_MARGIN_* is then set. */
        b->margin_top = (int16_t)((cs && (cs->set & CSS_SET_MARGIN_TOP)) ? cs->margin_top : GAP_BLOCK_D);
        b->margin_bottom = (int16_t)((cs && (cs->set & CSS_SET_MARGIN_BOTTOM)) ? cs->margin_bottom : GAP_BLOCK_D);
        b->pad_top = (int16_t)(cs ? cs->pad_top : 0);
        b->pad_bottom = (int16_t)(cs ? cs->pad_bottom : 0);
        /* The horizontal half of the box: margin-left and padding-left
         * (and their right twins) become the column's insets. Nothing is
         * invented for a border: this renderer draws the top and bottom
         * rules, and a vertical rule would need a second axis. */
        int il = cs ? cs->margin_left + cs->pad_left : 0;
        int ir = cs ? cs->margin_right + cs->pad_right : 0;
        if (il < 0) il = 0;
        if (ir < 0) ir = 0;
        b->inset_left = (u16)(il > 10000 ? 10000 : il);
        b->inset_right = (u16)(ir > 10000 ? 10000 : ir);
        b->width = (u16)(!cs || cs->width < 0 ? 0 : cs->width > 10000 ? 10000 : cs->width);
        b->max_width = (u16)(!cs || cs->max_width < 0 ? 0 : cs->max_width > 10000 ? 10000 : cs->max_width);
        b->width_pct = (u16)(!cs || cs->width_pct < 0 ? 0 : cs->width_pct);
        b->max_width_pct = (u16)(!cs || cs->max_width_pct < 0 ? 0 : cs->max_width_pct);
        b->background = cs ? cs->background : 0;
        b->color = cs ? cs->color : 0;
        b->has_color = (u8)((cs && (cs->set & CSS_SET_COLOR)) ||
                            (parent_block >= 0 && d->block[parent_block].has_color));
        b->border_top_w = (u8)(cs && cs->border_top_w > 0 ? cs->border_top_w : 0);
        b->border_bottom_w = (u8)(cs && cs->border_bottom_w > 0 ? cs->border_bottom_w : 0);
        b->border_top_style = (u8)(cs ? cs->border_top_style : CSS_BORDER_NONE);
        b->border_bottom_style = (u8)(cs ? cs->border_bottom_style : CSS_BORDER_NONE);
        b->border_top_color = cs ? cs->border_top_color : 0;
        b->border_bottom_color = cs ? cs->border_bottom_color : 0;
        b->align = (u8)(cs ? cs->align : CSS_ALIGN_LEFT);
        b->is_body = (u8)(!strcmp(t->name, "body"));
        b->level = (u8)(level > 0 ? level : level_for_px(cs ? cs->font_size : CSS_BASE_FONT_SIZE));
        b->line_height = (u16)(!cs || cs->line_height > 60000 ? 60000 :
                               cs->line_height < 0 ? 0 : cs->line_height);
        b->indent = (u16)(!cs || cs->indent < 0 ? 0 : cs->indent > 10000 ? 10000 : cs->indent);
    }

    if (ei >= 0) {
        HtmlOpen *o = &h->open[h->nopen++];
        snprintf(o->tag, sizeof o->tag, "%s", t->name);
        o->block = bi;
        o->elem = ei;
        o->ordinal = 0;
    } else if (bi >= 0) {
        /* No room to track it: the box is real but empty. */
        d->block[bi].atom_end = (u16)d->natom;
    }

    if (make_block) {
        h->level = level;
        h->pending_break = 1;
        h->pending_gap = 0;
        h->pending_space = 0;
        h->pre_seen = 0;
        h->pre_break = 0;
        h->pre_col = 0;
    }
}

/* Close the named element. Mismatched markup (“<b><div></b></div>”)
 * closes the inner elements too, so the stack can never leak a run into
 * the next page. An inline close never starts a new line; it only ends
 * the current word so the two sides can keep different faces. */
static void html_elem_close(Html *h, const char *tag, int is_block) {
    /* Flush the last word while the closing element still supplies its
     * style; a block close must also end its word before its atom_end is
     * stamped, or the last word of every heading lands in the next block. */
    html_end_word(h, 0);
    int k = -1;
    for (int i = h->nopen - 1; i >= 0; i--)
        if (!strcmp(h->open[i].tag, tag)) { k = i; break; }
    if (k < 0) {
        if (is_block) {
            h->pending_break = 1;
            h->pending_gap = 0;
        }
        return;
    }
    int had_block = 0;
    while (h->nopen > k) {
        HtmlOpen *o = &h->open[--h->nopen];
        if (o->block >= 0) {
            Block *b = &h->d->block[o->block];
            b->atom_end = (u16)h->d->natom;
            had_block = 1;
        }
        if (o->elem >= 0) h->nelem = o->elem;
    }
    /* An inline tag that the cascade turned into a block box still ends
     * like a block: the next text starts on a new line. */
    if (is_block || had_block) {
        h->pending_break = 1;
        h->pending_gap = 0;
        h->pending_space = 0;
    }
    if (h->nopen == 0) h->level = 0;
}

static void html_block_open(Html *h, const Tag *t, int level) {
    html_end_word(h, 0);
    html_elem_push(h, t, 1, level);
}

static void html_block_close(Html *h, const char *tag) {
    html_elem_close(h, tag, 1);
}

/* The enclosing list element and its resolved style, or NULL. */
static HtmlOpen *html_list_open(Html *h) {
    for (int i = h->nopen - 1; i >= 0; i--) {
        const char *n = h->open[i].tag;
        if (!strcmp(n, "ul") || !strcmp(n, "ol") ||
            !strcmp(n, "menu") || !strcmp(n, "dir"))
            return &h->open[i];
    }
    return NULL;
}

static void html_tag(Html *h, const Tag *t) {
    if (!t->name[0]) return;                  /* comment, doctype, PI */
    const char *n = t->name;

    if (t->closing) {
        if (!strcmp(n, "title")) {
            h->in_title = 0;
            html_elem_close(h, "title", 0);
            return;
        }
        if (!strcmp(n, "script") || !strcmp(n, "style")) {
            /* The <style> body has been collected by html_text(); record
             * where it sits in the buffer so the cascade can read it in
             * document order, after the sheets that preceded it. */
            if (!strcmp(n, "style") && h->style_open && h->css && h->css_len) {
                int len = (int)*h->css_len - h->style_off;
                if (len > 0) html_add_csssrc(h, 0, h->style_off, len, NULL, NULL);
            }
            h->style_open = 0;
            h->skip = 0;
            h->in_style = 0;
            html_elem_close(h, n, 0);
            return;
        }
        if (!strcmp(n, "a")) {
            if (h->in_link >= 0) {
                char num[16];
                int idx = h->in_link;
                h->in_link = -1;
                html_end_word(h, 0);
                html_elem_close(h, "a", 0);
                snprintf(num, sizeof(num), "[%d]", idx + 1);
                html_atom_word(h, num, (long)strlen(num), idx, 1);
                /* no space is forced after the marker: "lists</a>,"
                 * has to read "lists[3],", while "</a> word" still
                 * gets its space from the whitespace in the page */
                h->pending_space = 0;
            } else {
                html_elem_close(h, "a", 0);
            }
            return;
        }
        if (!strcmp(n, "li")) {
            html_end_word(h, 0);
            html_elem_close(h, "li", 0);
            html_break(h, GAP_LI_D);
            return;
        }
        if (!strcmp(n, "td") || !strcmp(n, "th")) {
            html_end_word(h, 0);
            html_elem_close(h, n, 0);
            h->pending_space = 1;
            return;
        }
        if (is_heading_tag(n)) {
            html_block_close(h, n);
            h->level = 0;
            return;
        }
        if (is_block_tag(n)) { html_block_close(h, n); return; }
        html_elem_close(h, n, 0);
        return;
    }

    /* open tags */
    if (h->in_title && (!strcmp(n, "body") || is_block_tag(n) || is_heading_tag(n))) {
        h->in_title = 0;
        html_elem_close(h, "title", 0);
    }
    if (!strcmp(n, "title")) {
        html_elem_push(h, t, 0, 0);
        h->in_title = 1;
        return;
    }
    if (!strcmp(n, "script") || !strcmp(n, "style")) {
        html_elem_push(h, t, 0, 0);
        h->skip = 1;
        /* A print-only <style> body is not part of this document's
         * cascade, so it is not collected at all. */
        h->in_style = !strcmp(n, "style") && media_ok(t->media);
        h->style_open = h->in_style;
        h->style_off = (h->style_open && h->css_len) ? (int)*h->css_len : 0;
        return;
    }
    if (!strcmp(n, "link")) {
        html_add_stylesheet(h, t);
        return;
    }
    if (!strcmp(n, "a")) {
        html_flush_before_inline(h);
        html_elem_push(h, t, 0, 0);
        html_add_link(h, t);
        return;
    }
    if (!strcmp(n, "li")) {
        /* The marker comes from the enclosing list's resolved
         * list-style-type: decimal really counts 1. 2. 3., and the
         * ul/ol indent is what lays the nesting out. */
        html_end_word(h, 0);
        html_elem_push(h, t, 0, 0);
        html_break(h, GAP_LI_D);

        HtmlOpen *lo = html_list_open(h);
        int ordinal = 0;
        if (lo) ordinal = ++lo->ordinal;
        const CssStyle *cs = html_top_style(h);
        int ls = cs ? cs->list_style : CSS_LIST_DISC;
        char mark[32];
        list_marker(ls, ordinal, mark, sizeof(mark));
        if (mark[0]) {
            html_atom(h, mark, (long)strlen(mark), AF_BREAK, -1, -1);
            h->pending_break = 0;
            h->pending_gap = 0;
            h->pending_space = 1;
        } else {
            h->pending_break = 1;
            h->pending_space = 0;
        }
        return;
    }
    if (is_heading_tag(n)) {
        html_block_open(h, t, n[1] - '0');
        return;
    }
    if (!strcmp(n, "br")) {
        /* In a preformatted block a <br> is a real line break, exactly like
         * a newline: emitting an atom, not only a pending flag, is what
         * makes <br><br> leave the blank line box between the two breaks.
         * In ordinary text the pending-break path is enough. */
        if (html_keeps_lines(h)) {
            html_end_word(h, 0);
            html_hard_break(h);
            h->pending_break = 1;
            h->pending_space = 0;
            h->pre_seen = 0;
            h->pre_break = 1;
            h->pre_col = 0;
        } else {
            html_break(h, 0);
        }
        return;
    }
    if (!strcmp(n, "td") || !strcmp(n, "th")) {
        html_end_word(h, 0);
        html_elem_push(h, t, 0, 0);
        h->pending_space = 1;
        return;
    }
    if (!strcmp(n, "img")) {
        /* The picture is an inline box at exactly this atom. Its alt text
         * is laid out only when the image cannot be fetched or decoded;
         * an image that is drawn suppresses that atom in doc_layout(), so
         * the words never appear twice and never stand in for a picture
         * that is really there. */
        html_end_word(h, 0);
        int img = -1;
        if (t->src[0] && h->d->nimg < WEB_MAX_IMGS && !html_hidden(h)) {
            PageImg *pi = &h->d->img[h->d->nimg];
            snprintf(pi->url, sizeof(pi->url), "%s", t->src);
            snprintf(pi->alt, sizeof(pi->alt), "%s", t->alt);
            pi->pic.px = NULL;
            pi->tried = 0;
            img = h->d->nimg++;
        }
        int flags = 0;
        if (h->pending_break) flags |= AF_BREAK;
        else if (!h->pending_space) flags |= AF_GLUE;
        html_atom(h, "", 0, flags | AF_IMAGE, h->in_link, img);
        h->pending_break = 0;
        h->pending_gap = 0;
        h->pending_space = 0;

        /* The alternative text exists whenever there is a picture the
         * reader might have expected, including one the fixed image table
         * had no room for (img < 0): layout suppresses this atom when the
         * picture is really drawn, so it only ever stands in for a
         * missing picture, and it is emitted once. */
        if (t->src[0] || t->alt[0]) {
            char alt[128];
            if (t->alt[0]) snprintf(alt, sizeof(alt), "[%s]", t->alt);
            else snprintf(alt, sizeof(alt), "[image]");
            /* The alternative text takes the picture's own place in the
             * flow: glued when the picture was glued to the word before
             * it, spaced when it was spaced. Forcing AF_GLUE here printed
             * "broken[picture]" instead of "broken [picture]". */
            int afl = AF_ALT | (flags & AF_GLUE);
            html_atom(h, alt, (long)strlen(alt), afl, h->in_link, img);
            h->pending_space = 0;
        }
        return;
    }
    if (!strcmp(n, "hr")) {
        /* A void element: its box opens and closes at this one point, so
         * the built-in border-top really becomes a rule with space around
         * it instead of six pixels of nothing. */
        html_block_open(h, t, 0);
        html_block_close(h, "hr");
        return;
    }
    if (is_block_tag(n)) { html_block_open(h, t, 0); return; }
    if (is_void_tag(n)) return;
    /* Any other element is an inline box. Flushing the current word on
     * the boundary is what keeps its style from leaking into the text on
     * the other side of the tag. */
    html_flush_before_inline(h);
    html_elem_push(h, t, 0, 0);
}

/* Turn a body of HTML into atoms. `title` receives the <title> text
 * (possibly empty). Returns the number of links found. */
static int html_build(Doc *d, const char *s, long n, const Url *base,
                      int latin1, char *title, size_t title_cap,
                      char *css, size_t css_cap, size_t *css_len,
                      const CssSheet *sheet) {
    d->natom = 0;
    d->nseg = 0;
    d->nline = 0;
    d->nblock = 0;
    d->nlink = 0;
    d->nimg = 0;
    d->ncsssrc = 0;
    d->css_capped = 0;
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
    h.sheet = sheet;
    h.css = css;
    h.css_cap = css_cap;
    h.css_len = css_len;
    if (css && css_cap) { css[0] = 0; *css_len = 0; }

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
    /* Close whatever the page forgot to close. An unclosed <div> must not
     * swallow the rest of the document, and its box ends at the last atom
     * like every other box. */
    while (h.nopen > 0) {
        char tag[CSS_TAG_MAX];
        snprintf(tag, sizeof(tag), "%s", h.open[h.nopen - 1].tag);
        html_elem_close(&h, tag, is_block_tag(tag));
    }
    while (h.title_len > 0 && title[h.title_len - 1] == ' ') h.title_len--;
    title[h.title_len] = 0;
    return d->nlink;
}

/* ============================================================
 *  Layout: atoms -> lines of placed segments
 * ============================================================ */

/* Bytes of s that fit in `avail` px, always at least one character so
 * a word wider than the line makes progress instead of looping. */
static int fit_prefix(const WebMetrics *m, const Atom *a, const char *s, int n, float avail) {
    float w = 0;
    int i = 0;
    while (i < n) {
        int cl = utf8_len((unsigned char)s[i]);
        if (cl > n - i) cl = n - i;
        float cw = m->text_w(m->ctx, a, s + i, cl);
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

/* The drawn size of a picture inside a column `avail` px wide. The
 * painter and the layout both call this, so the box that was reserved and
 * the pixels that are drawn can never disagree. */
static void img_fit(const PageImg *pi, float avail, int *ww, int *hh) {
    int w = pi->pic.w, h = pi->pic.h;
    int maxw = (int)avail;
    if (maxw < 1) maxw = 1;
    if (w <= 0 || h <= 0 || !pi->pic.px) { *ww = 0; *hh = 0; return; }
    if (w > maxw) {
        h = (int)((long)h * maxw / w);
        w = maxw;
    }
    if (h < 1) h = 1;
    *ww = w;
    *hh = h;
}

typedef struct {
    Doc *d;
    const WebMetrics *m;
    float page_w;                  /* full content width of the window */
    float width;                   /* current column width             */
    float x;                       /* current column left, page px     */
    u32 y;
    int first_seg;
    int segs_in_line;
    float pen;
    int height;                    /* tallest face on the line         */
    int gap;
    int stack[WEB_BLOCK_DEPTH];    /* open blocks, innermost last      */
    int nopen;
    int next_block;                /* next Doc.block to open           */
    int pending_margin;            /* design px waiting to be applied  */
    int hard_pending;              /* the previous atom was a hard \n  */
    int skip_alt;                  /* the next AF_ALT is already drawn */
} Layout;

/* One placed run. `a` supplies the face, the weight, the slant and the
 * decoration; the segment is stored in page coordinates, which is what
 * the painter, the link hit test and text-align all need. */
static void lay_emit(Layout *L, const Atom *a, int ai, int start, int len,
                     float x, float w) {
    Doc *d = L->d;
    if (d->nseg >= WEB_MAX_SEGS) { d->truncated = 1; return; }
    Seg *s = &d->seg[d->nseg];
    s->atom = (u16)ai;
    s->start = (u16)start;
    s->len = (u16)len;
    s->x = (u16)(L->x + x + 0.5f);
    s->w = (u16)(w + 0.5f);
    if (L->first_seg < 0) L->first_seg = d->nseg;
    d->nseg++;
    L->segs_in_line++;
    /* An image contributes its own box height; its "face" is the picture. */
    if (!(a->flags & AF_IMAGE)) {
        int lh = (int)(L->m->line_h(L->m->ctx, a) + 0.5f);
        if (lh > L->height) L->height = lh;
    }
}

/* A design gap in screen pixels. This is the only place where the
 * document's vertical rhythm meets the desktop's text scale, so a scale
 * change re-spaces the next layout without rebuilding the document. */
static int layout_gap(int design) {
    if (design <= 0) return 0;
    return ui_px(design);
}

/* Fix the tops of every box that is still waiting for its first line.
 * Called from both flush paths, so an empty line (a blank line inside a
 * <pre>) still anchors the boxes around it in the right place. */
static void lay_anchor_boxes(Layout *L) {
    Doc *d = L->d;
    if (L->pending_margin > 0) {
        L->y += (u32)layout_gap(L->pending_margin);
        L->pending_margin = 0;
    }
    for (int k = 0; k < L->nopen; k++) {
        Block *ab = &d->block[L->stack[k]];
        if (!ab->has_top) {
            ab->box_y0 = ab->content_y0 = L->y;
            ab->has_top = 1;
        }
    }
}

static void lay_flush(Layout *L) {
    Doc *d = L->d;
    if (L->first_seg >= 0 && L->segs_in_line > 0 && d->nline < WEB_MAX_LINES) {
        int bi = L->nopen > 0 ? L->stack[L->nopen - 1] : -1;
        Block *b = bi >= 0 ? &d->block[bi] : NULL;

        /* A block with no top border or padding fixes its top at the first
         * line it really produces, so its margin and its background start
         * in one place. Every still-deferred ancestor gets the same y:
         * that is a margin collapsing through the boxes. */
        lay_anchor_boxes(L);

        /* text-align shifts the finished line inside the block's column,
         * never the wrapping: a centred line is wrapped at the same width
         * as a left one, which is how CSS defines it. Justified lines are
         * spread after the whole document, where the last line of each
         * block is known. */
        float used = 0.0f;
        for (int j = 0; j < L->segs_in_line; j++) {
            Seg *sg = &d->seg[L->first_seg + j];
            float right = (float)sg->x + (float)sg->w;
            if (right > used) used = right;
        }
        float used_in_col = used - L->x;
        if (used_in_col < 0.0f) used_in_col = 0.0f;
        int align = b ? b->align : CSS_ALIGN_LEFT;
        float off = 0.0f;
        if (align == CSS_ALIGN_CENTER) off = (L->width - used_in_col) * 0.5f;
        else if (align == CSS_ALIGN_RIGHT) off = L->width - used_in_col;
        if (off > 0.5f) {
            for (int j = 0; j < L->segs_in_line; j++)
                d->seg[L->first_seg + j].x =
                    (u16)((float)d->seg[L->first_seg + j].x + off + 0.5f);
        }

        int h = L->height > 0 ? L->height : 12;
        if (b && b->line_height > 0) {
            /* line-height opens the line box up; it never makes it shorter
             * than the face, because overlapping glyphs are not a text
             * browser's idea of tight leading. */
            int blh = layout_gap(b->line_height);
            if (blh > h) h = blh;
        }

        Line *ln = &d->line[d->nline++];
        ln->first = (u32)L->first_seg;
        ln->n = (u16)L->segs_in_line;
        ln->th = (u16)(L->height > 0 ? L->height : 12);
        ln->h = (u16)h;
        ln->block = (u16)(bi < 0 ? 0xFFFF : bi);
        ln->x0 = (u16)(L->x + 0.5f);
        ln->x1 = (u16)(L->x + L->width + 0.5f);
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

/* An empty line box, which is what a blank line inside <pre> is. It has
 * no segments, so the painter draws nothing, but it takes the height of
 * the face and keeps the vertical rhythm - the difference between
 * "a\n\nb" and "a\nb". */
static void lay_empty_line(Layout *L, const Atom *a) {
    Doc *d = L->d;
    if (d->nline >= WEB_MAX_LINES) { d->truncated = 1; return; }
    lay_anchor_boxes(L);
    int bi = L->nopen > 0 ? L->stack[L->nopen - 1] : -1;
    int h = (int)(L->m->line_h(L->m->ctx, a) + 0.5f);
    if (h < 1) h = 12;
    Line *ln = &d->line[d->nline++];
    ln->first = (u32)d->nseg;
    ln->n = 0;
    ln->th = (u16)h;
    ln->h = (u16)h;
    ln->block = (u16)(bi < 0 ? 0xFFFF : bi);
    ln->x0 = (u16)(L->x + 0.5f);
    ln->x1 = (u16)(L->x + L->width + 0.5f);
    ln->gap = 0;
    ln->y = L->y;
    L->y += (u32)h;
}

/* Enter a box: resolve its column and add its top margin, border and
 * padding to the flow. Width and max-width are CSS lengths (design pixels
 * here); a percentage is against the parent's content width. Any block
 * narrower than its parent is centred in it. `indent` is the CSS-derived
 * nesting step of ul/ol/blockquote/dd; it narrows the column from the
 * left, which is what makes a nested list visibly nested. */
static void lay_open_block(Layout *L, int bi) {
    Doc *d = L->d;
    Block *b = &d->block[bi];
    lay_flush(L);
    if (b->line_start == 0xFFFF) b->line_start = (u16)d->nline;

    if (b->margin_top > L->pending_margin) L->pending_margin = b->margin_top;

    float parent_w = L->width > 1.0f ? L->width : 1.0f;
    float parent_x = L->x;
    float w = (float)layout_gap(b->width);
    if (b->width_pct) w = parent_w * (float)b->width_pct / 100.0f;
    if (w <= 0.0f) w = parent_w;
    if (b->max_width_pct) {
        float mw = parent_w * (float)b->max_width_pct / 100.0f;
        if (w > mw) w = mw;
    } else if (b->max_width) {
        float mw = (float)layout_gap(b->max_width);
        if (w > mw) w = mw;
    }
    if (w < 1.0f) w = 1.0f;
    if (w > parent_w) w = parent_w;      /* one column, never a sideways scroll */
    float x = parent_x;
    if (w < parent_w) x = parent_x + (parent_w - w) * 0.5f;

    /* The list/quote indent is inside the block's own column, and it is
     * the step this box adds to its parent's, not the whole inherited
     * value. css.c inherits `indent`, so a <p> inside an <li> carries the
     * list's 24 px as well; applying the inherited number again put that
     * paragraph 24 px past the bullet, and a nested <ul> (24 inherited +
     * 24 of its own) at 72 px instead of 48. The column already starts
     * where the parent's indent put it. */
    int parent_ind = L->nopen > 0 ? (int)d->block[L->stack[L->nopen - 1]].indent : 0;
    float ind = (float)layout_gap((int)b->indent > parent_ind ?
                                  (int)b->indent - parent_ind : 0);
    if (ind > 0.0f) {
        x += ind;
        w -= ind;
        if (w < 1.0f) w = 1.0f;
    }

    /* margin-left + padding-left and their right twins: the horizontal
     * half of the CSS box, as an inset of the one column this renderer
     * has. A box that asks for more inset than there is room for keeps a
     * readable minimum rather than collapsing to zero width. */
    float l = (float)layout_gap((int)b->inset_left);
    float r = (float)layout_gap((int)b->inset_right);
    if (l < 0.0f) l = 0.0f;
    if (r < 0.0f) r = 0.0f;
    if (l + r > w - 16.0f) {
        float slack = w > 16.0f ? w - 16.0f : 0.0f;
        float sum = l + r;
        if (sum > 0.0f) { l = l * slack / sum; r = r * slack / sum; }
    }
    if (l > 0.0f || r > 0.0f) {
        x += l;
        w -= l + r;
        if (w < 1.0f) w = 1.0f;
    }

    b->box_x0 = (u16)(x + 0.5f);
    b->box_x1 = (u16)(x + w + 0.5f);
    b->has_top = 0;

    if (L->nopen >= WEB_BLOCK_DEPTH) return;   /* cannot happen; the parser caps */
    L->stack[L->nopen++] = bi;
    L->x = x;
    L->width = w;

    if (b->border_top_w > 0 || b->pad_top > 0) {
        /* A border or padding cannot collapse, so the top is fixed now. */
        if (L->pending_margin > 0) {
            L->y += (u32)layout_gap(L->pending_margin);
            L->pending_margin = 0;
        }
        b->box_y0 = L->y;
        b->content_y0 = L->y + (u32)layout_gap(b->border_top_w) +
                        (u32)layout_gap(b->pad_top);
        L->y = b->content_y0;
        b->has_top = 1;
    }
}

/* Leave a box: close its content, add bottom padding and border, then
 * leave its margin pending for the next box (adjacent margins collapse to
 * the larger one, which is what keeps paragraph rhythm even). */
static void lay_close_block(Layout *L, int bi) {
    Doc *d = L->d;
    Block *b = &d->block[bi];
    lay_flush(L);

    if (!b->has_top && b->pad_bottom == 0 && b->border_bottom_w == 0) {
        /* An empty box with no drawn part: its margins collapse through. */
        if (b->margin_bottom > L->pending_margin) L->pending_margin = b->margin_bottom;
        b->box_y0 = b->box_y1 = b->content_y0 = b->content_y1 = L->y;
        b->line_end = (u16)d->nline;
        if (L->nopen > 0 && L->stack[L->nopen - 1] == bi) L->nopen--;
        if (L->nopen > 0) {
            Block *p = &d->block[L->stack[L->nopen - 1]];
            L->x = p->box_x0;
            L->width = (float)(p->box_x1 - p->box_x0);
        } else {
            L->x = 0.0f;
            L->width = L->page_w;
        }
        return;
    }

    if (!b->has_top) {
        /* Empty, but it has a bottom padding or border to place. */
        if (L->pending_margin > 0) {
            L->y += (u32)layout_gap(L->pending_margin);
            L->pending_margin = 0;
        }
        b->box_y0 = b->content_y0 = L->y;
        b->has_top = 1;
    }

    b->content_y1 = L->y;
    L->y += (u32)layout_gap(b->pad_bottom) + (u32)layout_gap(b->border_bottom_w);
    b->box_y1 = L->y;
    if (b->margin_bottom > L->pending_margin) L->pending_margin = b->margin_bottom;
    b->line_end = (u16)d->nline;

    if (L->nopen > 0 && L->stack[L->nopen - 1] == bi) L->nopen--;
    if (L->nopen > 0) {
        Block *p = &d->block[L->stack[L->nopen - 1]];
        L->x = p->box_x0;
        L->width = (float)(p->box_x1 - p->box_x0);
    } else {
        L->x = 0.0f;
        L->width = L->page_w;
    }
}

/* Spread a justified line to both edges. CSS justifies every line of a
 * block except its last one, which is why this runs after the document is
 * laid out and block->line_end is known. */
static void lay_justify(Doc *d) {
    for (int i = 0; i < d->nline; i++) {
        Line *ln = &d->line[i];
        if (ln->n < 2 || ln->block == 0xFFFF) continue;
        Block *b = &d->block[ln->block];
        if (b->align != CSS_ALIGN_JUSTIFY) continue;
        if (i + 1 >= (int)b->line_end) continue;      /* last line of the block */
        int used = 0;
        int preserved = 0;
        for (int j = 0; j < (int)ln->n; j++) {
            Seg *sg = &d->seg[ln->first + j];
            int right = (int)sg->x + (int)sg->w;
            if (right > used) used = right;
            /* A preserved line (white-space: pre) ends at a real newline,
             * which CSS never justifies, and stretching it would destroy
             * the indentation that is the reason the author used pre. */
            if (d->atom[sg->atom].ws == CSS_WS_PRE) preserved = 1;
        }
        if (preserved) continue;
        int avail = (int)ln->x1 - (int)ln->x0;
        int extra = avail - (used - (int)ln->x0);
        if (extra <= 1) continue;
        int gaps = (int)ln->n - 1;
        for (int j = 1; j < (int)ln->n; j++) {
            Seg *sg = &d->seg[ln->first + j];
            sg->x = (u16)((int)sg->x + extra * j / gaps);
        }
    }
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
    L.page_w = (float)width;
    L.width = (float)width;
    L.first_seg = -1;

    for (int ai = 0; ai <= d->natom; ai++) {
        /* 1. close the boxes that end at this atom, innermost first */
        while (L.nopen > 0 && d->block[L.stack[L.nopen - 1]].atom_end <= ai)
            lay_close_block(&L, L.stack[L.nopen - 1]);

        /* 2. open the boxes that start here, in document order. An empty
         * box (an <hr>, or a <div> with nothing in it) opens and closes at
         * this same point, so its border and padding still happen. */
        while (L.next_block < d->nblock &&
               d->block[L.next_block].atom_start <= ai) {
            int bi = L.next_block++;
            lay_open_block(&L, bi);
            while (L.nopen > 0 && d->block[L.stack[L.nopen - 1]].atom_end <= ai)
                lay_close_block(&L, L.stack[L.nopen - 1]);
        }

        if (ai >= d->natom) break;

        const Atom *a = &d->atom[ai];
        int len = (int)strlen(a->text);

        /* The alt text of an image that was drawn inline has already had
         * its place taken by the picture; never print it twice. The flag
         * covers exactly one atom: an alt atom is emitted directly after
         * its picture, so any other atom means the picture had no alt (or
         * the atom cap dropped it) and a later broken image must still be
         * able to print its own. */
        if ((a->flags & AF_ALT) && L.skip_alt) {
            L.skip_alt = 0;
            continue;
        }
        L.skip_alt = 0;

        if (a->flags & AF_BREAK) {
            int had = L.segs_in_line > 0;
            lay_flush(&L);
            L.gap = a->gap;
            if (a->flags & AF_HARD) {
                /* A hard break with no text before it is a blank line, not
                 * just the end of the previous one. HTML strips the newline
                 * directly after <pre>, so the first hard break of a pre
                 * block can already be a blank line and must keep its box. */
                if (!had) lay_empty_line(&L, a);
                L.hard_pending = 1;
            }
            /* An empty text atom is only a line break. An image box has
             * no text either, but it is a real inline box: an <img> that
             * starts a paragraph carries AF_BREAK, and skipping it here
             * left a hole where the picture should be and printed the alt
             * text instead. */
            if (len <= 0 && !(a->flags & AF_IMAGE)) continue;
        }
        L.hard_pending = 0;

        if (a->flags & AF_IMAGE) {
            if (a->img >= 0 && a->img < d->nimg &&
                d->img[a->img].pic.px && d->img[a->img].pic.w > 0) {
                int ww = 0, hh = 0;
                img_fit(&d->img[a->img], L.width, &ww, &hh);
                if (ww > 0 && hh > 0) {
                    float sp = (L.segs_in_line > 0 && !(a->flags & AF_GLUE))
                               ? m->text_w(m->ctx, a, " ", 1) : 0.0f;
                    if (L.segs_in_line > 0 && L.pen + sp + (float)ww > L.width)
                        lay_flush(&L);
                    lay_emit(&L, a, ai, 0, 0, L.pen + sp, (float)ww);
                    L.pen += sp + (float)ww;
                    if (hh > L.height) L.height = hh;
                    L.skip_alt = 1;
                }
            }
            continue;                    /* broken: the AF_ALT atom follows */
        }

        if (len <= 0) continue;

        /* white-space: pre and nowrap mean one thing to a text renderer:
         * do not break this run because the column ended. The painter
         * clips whatever passes the right edge; nothing is invented or
         * reordered. pre-wrap is not in that list: it keeps the spaces
         * and the newlines but still wraps, so a long preserved line
         * stays readable instead of being cut off at the column edge. */
        int nowrap = (a->ws == CSS_WS_NOWRAP || a->ws == CSS_WS_PRE);

        /* A slanted run leans past its upright advance. Reserve that ink
         * on the line, once per atom, so "italic upright" keeps a gap
         * instead of the italic tail landing on the next word. */
        float lean = m->overhang ? m->overhang(m->ctx, a) : 0.0f;

        float space = m->text_w(m->ctx, a, " ", 1);
        int start = 0;
        while (start < len) {
            float wfull = m->text_w(m->ctx, a, a->text + start, len - start) + lean;
            float sp = (L.segs_in_line > 0 && !(a->flags & AF_GLUE)) ? space : 0.0f;

            if (nowrap || L.pen + sp + wfull <= L.width) {
                lay_emit(&L, a, ai, start, len - start, L.pen + sp, wfull);
                L.pen += sp + wfull;
                break;
            }
            if (L.segs_in_line > 0) {
                lay_flush(&L);
                continue;                    /* try again on a fresh line */
            }
            /* A word wider than the whole line: this is the one case
             * where breaking a word is the only honest thing to do. */
            int k = fit_prefix(m, a, a->text + start, len - start,
                               L.width - lean > 1.0f ? L.width - lean : 1.0f);
            float kw = m->text_w(m->ctx, a, a->text + start, k) + lean;
            lay_emit(&L, a, ai, start, k, 0.0f, kw);
            lay_flush(&L);
            start += k;
        }
    }
    lay_flush(&L);
    lay_justify(d);
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
/* Map an absolute CSS font-size onto the renderer's level table. The table
 * is what every measurement and every glyph uses, so a page asking for 30px
 * headings gets them through the same code path as <h1>. */
static int level_for_px(int px) {
    if (px >= 22) return 1;      /* 24 */
    if (px >= 19) return 2;      /* 20 */
    if (px >= 16) return 3;      /* 17 */
    if (px >= 14) return 4;      /* 15 */
    if (px >= 13) return 5;      /* 13 */
    return 0;                    /* 12 */
}

static int style_size(int level) {
    static const int sizes[7] = { 12, 24, 20, 17, 15, 13, 12 };
    if (level < 0 || level > 6) level = 0;
    return sizes[level];
}

/* The face a run is set in. There are four installed roles - regular,
 * medium, semibold and mono - and no italic file at all, which is why
 * italic is synthesised by shear in draw_run() below instead of being
 * faked by picking another upright family. The weight threshold is the
 * one css.h documents: 600 and up is the semibold face. */
static Font *font_for_atom(const Atom *a) {
    float size = (float)style_size(a ? a->level : 0);
    if (a && a->mono) return font_get(FONT_MONO, size);
    return font_get((a && a->weight >= 600) ? FONT_SEMIBOLD : FONT_REGULAR, size);
}

/* tan(12 deg): the slant of a synthesised oblique. The installed set has
 * no italic file at all, so draw_italic() shifts every scanline of the
 * upright glyphs right by this fraction of its height above the baseline.
 * The same number is what layout reserves as a slanted run's overhang. */
#define ITALIC_SHEAR 0.2126f

/* How far the ink of a sheared run reaches past its advance. One lean per
 * atom, not per character: font_width() already measures the upright text,
 * and the shear moves the rightmost glyph's top by this much. */
static float real_overhang(void *ctx, const Atom *a) {
    (void)ctx;
    if (!a || !a->italic) return 0.0f;
    Font *f = font_for_atom(a);
    return f ? ITALIC_SHEAR * font_height(f) : 0.0f;
}

/* Metrics handed to layout: the real font_width of the real face, with
 * the same face selection the painter will use for the glyphs. */
static float real_text_w(void *ctx, const Atom *a, const char *s, int n) {
    Font *f = font_for_atom(a);
    if (!f || n <= 0) return 0;
    char buf[WEB_ATOM_TEXT + 8];
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    (void)ctx;
    return font_width(f, buf);
}

static float real_line_h(void *ctx, const Atom *a) {
    Font *f = font_for_atom(a);
    (void)ctx;
    return f ? font_height(f) : 16.0f;
}

static const WebMetrics real_metrics = { NULL, real_text_w, real_line_h,
                                        real_overhang };

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
    CssSheet  sheet;           /* the page's own CSS, in document order  */
    int       css_ready;
    u32       col_text;        /* resolved from the sheet, not invented   */
    u32       col_link;
    u32       col_head;
    /* Every <style> body that was collected, plus the room the fetched
     * sheets are parsed from. WEB_CSS_BYTES is the same cap the fetched
     * total is held to, so the buffer can always hold what is accepted. */
    char      css_raw[WEB_CSS_BYTES];
    /* What really arrived, for the footer: sheets parsed, bytes fetched,
     * and whether a cap stopped anything. */
    int       css_sheets;
    long      css_bytes;
    int       css_capped;

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
    char tls_subject[200];
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

    /* Text selection over the rendered page. A position is (line, segment,
     * byte) in the document the painter walks; byte offsets inside a segment
     * are what make the highlight land between glyphs instead of between
     * words. Both ends are stored unordered. */
    int  sel;                  /* 1 when a range exists */
    int  sl, ss, so;           /* start line / segment / offset */
    int  el, es, eo;           /* end, exclusive of eo */
    int  sel_drag;             /* the press became a selection */
    int  sel_moved;            /* the pointer really moved since the press */
    int  link_hit;             /* a link under the press, followed on release */
    int  press_x, press_y;     /* where the press landed, page px */

    /* One short line of feedback ("copied 214 bytes"), drawn over the page
     * for a moment. Time is CLOCK_MONOTONIC milliseconds. */
    char note[96];
    long note_at;
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

/* Length of the longest prefix of buf[0..len) that does not end inside a
 * UTF-8 codepoint. Error text carries host names, which may be non-ASCII. */
static size_t draw_utf8_trim(const char *buf, size_t len) {
    if (len == 0) return 0;
    size_t i = len;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return len;
    unsigned char c = (unsigned char)buf[i - 1];
    int need = 1;
    if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) need = 3;
    else if (c >= 0xF0 && c <= 0xF4) need = 4;
    if (len - (i - 1) >= (size_t)need) return len;
    return i - 1;
}

/* Draw text, cut with ".." when it cannot fit in maxw pixels. This is the
 * only single-line entry point: every error, hint and caption is measured. */
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
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;   /* room for the dots */
    while (len > 0) {
        len = draw_utf8_trim(buf, len);
        buf[len] = 0;
        if (font_width(f, buf) + dots <= maxw) break;
        if (len == 0) break;
        len--;
    }
    if (len == 0) return;
    buf[len] = '.';
    buf[len + 1] = '.';
    buf[len + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* Bytes of one word that fit in maxw, always at least one whole
 * character so the caller that loops makes progress. */
static int wrap_prefix(Font *f, const char *s, int n, float maxw) {
    float w = 0;
    int i = 0;
    while (i < n) {
        int cl = utf8_len((unsigned char)s[i]);
        if (cl > n - i) cl = n - i;
        char one[8];
        if (cl >= (int)sizeof(one)) cl = (int)sizeof(one) - 1;
        memcpy(one, s + i, (size_t)cl);
        one[cl] = 0;
        float cw = font_width(f, one);
        if (i > 0 && w + cw > maxw) break;
        w += cw;
        i += cl;
    }
    if (i == 0) {
        i = utf8_len((unsigned char)s[0]);
        if (i > n) i = n;
    }
    return i;
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

        /* One word can be wider than the whole column - a URL or host name
         * in an error message is the usual case. Break it by measurement
         * whichever word of the line it is: committing it and painting it
         * at full length runs it across the window (the case the earlier
         * "first word only" test still missed was "cannot connect to
         * <long-url>", where the URL is the second word). */
        char word[512];
        int wc = wl;
        if (wc > (int)sizeof(word) - 1) wc = (int)sizeof(word) - 1;
        memcpy(word, w, (size_t)wc);
        word[wc] = 0;
        if (wc < wl || font_width(f, word) > maxw) {
            if (olen > 0) {
                font_draw(v, f, x, y + (float)line * lh, out, color);
                line++;
                olen = 0;
                out[0] = 0;
            }
            int off = 0;
            while (off < wl && line < maxlines) {
                int k = wrap_prefix(f, w + off, wl - off, maxw);
                if (k <= 0) break;
                char piece[512];
                int pc = k;
                if (pc > (int)sizeof(piece) - 1) pc = (int)sizeof(piece) - 1;
                memcpy(piece, w + off, (size_t)pc);
                piece[pc] = 0;
                font_draw(v, f, x, y + (float)line * lh, piece, color);
                line++;
                off += k;
            }
            continue;
        }

        char probe[512];
        int n = 0;
        if (olen > 0) {
            if (olen > (int)sizeof(probe) - 2) {
                /* No room for the space that joins the next word: draw this
                 * line and start the next one, rather than writing past
                 * probe[]. */
                font_draw(v, f, x, y + (float)line * lh, out, color);
                line++;
                olen = 0;
                out[0] = 0;
            } else {
                memcpy(probe, out, (size_t)olen);
                n = olen;
                probe[n++] = ' ';
            }
        }
        int copy = wl;
        if (n + copy >= (int)sizeof(probe)) copy = (int)sizeof(probe) - n - 1;
        if (copy < 0) copy = 0;
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

/* ------------------------------------------------------------ styled text
 * The CSS cascade gives every run a weight, a slant and a decoration.
 * The installed font set has regular, medium, semibold and mono files and
 * no italic file at all, so a declared italic is synthesised the way a
 * typesetter's oblique is: each scanline of the upright glyphs is shifted
 * right by an amount proportional to its height above the baseline. The
 * scratch surface below is the only allocation this costs, it is reused
 * for every run in every frame, and if it cannot be allocated the run is
 * drawn upright rather than vanishing. */

static u32   *ital_buf;
static size_t ital_cap;

static int ital_reserve(size_t n) {
    if (n <= ital_cap) return 1;
    u32 *p = (u32 *)realloc(ital_buf, n * sizeof *p);
    if (!p) return 0;
    ital_buf = p;
    ital_cap = n;
    return 1;
}

/* Composite one pixel with coverage `cov` (the same arithmetic font_draw
 * uses for a glyph, and the one the old image band was missing). */
static void blend_cov(Surface *s, int x, int y, u32 col, float cov) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    if (s->cx1 > s->cx0 &&
        (x < s->cx0 || x >= s->cx1 || y < s->cy0 || y >= s->cy1)) return;
    if (cov <= 0.004f) return;
    u32 *row = s->px + (size_t)y * s->stride;
    if (cov >= 0.996f) { row[x] = col | 0xFF000000u; return; }
    u32 d = row[x];
    float inv = 1.0f - cov;
    row[x] = 0xFF000000u
           | ((u32)(GET_R(col) * cov + GET_R(d) * inv) << 16)
           | ((u32)(GET_G(col) * cov + GET_G(d) * inv) << 8)
           |  (u32)(GET_B(col) * cov + GET_B(d) * inv);
}

static void draw_italic(Surface *v, Font *f, float x, float y,
                        const char *text, u32 col) {
    float fw = font_width(f, text);
    int h = (int)(font_height(f) + 2.0f);
    int w = (int)(fw + ITALIC_SHEAR * (float)h + 4.0f);
    if (w <= 0 || h <= 0 || !ital_reserve((size_t)w * (size_t)h)) {
        font_draw(v, f, x, y, text, col);
        return;
    }
    memset(ital_buf, 0, (size_t)w * (size_t)h * sizeof *ital_buf);
    Surface t;
    t.px = ital_buf;
    t.w = w;
    t.h = h;
    t.stride = w;
    surf_unclip(&t);
    /* White on a transparent buffer: the red channel is then exactly the
     * glyph coverage, independent of the colour the run is drawn in. */
    font_draw(&t, f, 2.0f, 1.0f, text, 0xFFFFFFFFu);
    int ox = (int)x - 2, oy = (int)y - 1;
    for (int row = 0; row < h; row++) {
        int shift = (int)(ITALIC_SHEAR * (float)(h - 1 - row) + 0.5f);
        const u32 *sr = ital_buf + (size_t)row * w;
        for (int cx = 0; cx < w; cx++) {
            u8 cov = GET_R(sr[cx]);
            if (!cov) continue;
            blend_cov(v, ox + cx + shift, oy + row, col, (float)cov / 255.0f);
        }
    }
}

static void draw_run(Surface *v, Font *f, const Atom *a, const char *text,
                     float x, float y, u32 col) {
    if (a->italic) draw_italic(v, f, x, y, text, col);
    else font_draw(v, f, x, y, text, col);
}

/* underline / line-through / overline, per placed segment. A link is
 * underlined only when its cascade says so - the built-in sheet gives
 * `a` an underline, and `a { text-decoration: none }` removes it. */
static void draw_decoration(Surface *v, Font *f, const Atom *a, const Seg *sg,
                            int ty, u32 col) {
    if (!a->decoration) return;
    int th = ui_px(1);
    if (th < 1) th = 1;
    int asc = (int)font_ascent(f);
    /* A rule runs along the baseline, so it spans the upright advance of
     * the segment. Seg.w also carries the slant's overhang (layout had to
     * reserve it); that ink is at the top of the glyphs, not at the rule,
     * so take it off here or an italic link gets a rule past its last
     * letter. */
    int rw = (int)sg->w;
    if (a->italic) {
        int lean = (int)(ITALIC_SHEAR * font_height(f) + 0.5f);
        rw -= lean;
        if (rw < 1) rw = 1;
    }
    u32 ink = color_alpha(col, 0.85f);
    if (a->decoration & CSS_TD_UNDERLINE) {
        int uy = ty + asc + ui_px(2);
        surf_fill(v, sg->x, uy, rw, th, ink);
    }
    if (a->decoration & CSS_TD_LINE_THROUGH) {
        /* Line-through sits through the MIDDLE of the x-height, not at its
         * top: Lato's x-height is about 0.5 em and font_height() is
         * size*1.4, so 0.5*sqrt-ish lands at ~0.196*font_height above
         * the baseline. The old 0.32 factor put the rule on the first
         * x-height row, which reads as an overline on a word with no
         * ascenders ("minimum"). */
        int sy = ty + asc - (int)(font_height(f) * 0.196f);
        surf_fill(v, sg->x, sy, rw, th, ink);
    }
    if (a->decoration & CSS_TD_OVERLINE) {
        surf_fill(v, sg->x, ty + 1, rw, th, ink);
    }
}

/* One picture at the box the segment reserved for it. The source is
 * sampled with its own pixel grid; alpha is honoured, so a PNG with
 * transparency does not turn into a black rectangle. */
static void draw_img_seg(Surface *v, const PageImg *pi, int x, int y,
                         int ww, int hh) {
    if (!pi->pic.px || ww <= 0 || hh <= 0) return;
    for (int row = 0; row < hh; row++) {
        int dy = y + row;
        if ((unsigned)dy >= (unsigned)v->h) continue;
        int sy = row * pi->pic.h / hh;
        const u32 *srow = pi->pic.px + (size_t)sy * pi->pic.w;
        for (int col = 0; col < ww; col++) {
            int dx = x + col;
            if ((unsigned)dx >= (unsigned)v->w) continue;
            int sx = col * pi->pic.w / ww;
            u32 sp = srow[sx];
            float a = (float)GET_A(sp) / 255.0f;
            if (a <= 0.004f) continue;
            blend_cov(v, dx, dy, sp, a);
        }
    }
}

/* One CSS border, drawn as what it says: solid fills the band, dashed and
 * dotted are real gaps, and a double border is two lines inside the width
 * the author declared instead of a thicker solid one. */
static void draw_rule(Surface *v, int x, int y, int w, int h, int style, u32 col) {
    if (w <= 0 || h <= 0) return;
    if (style == CSS_BORDER_DOTTED) {
        int step = h * 2;
        if (step < 2) step = 2;
        for (int dx = 0; dx < w; dx += step)
            surf_blend(v, x + dx, y, h, h, col);
        return;
    }
    if (style == CSS_BORDER_DASHED) {
        int dash = h * 3;
        if (dash < 3) dash = 3;
        int step = dash + dash / 2;
        for (int dx = 0; dx < w; dx += step) {
            int sad = w - dx;
            surf_blend(v, x + dx, y, sad < dash ? sad : dash, h, col);
        }
        return;
    }
    if (style == CSS_BORDER_DOUBLE && h >= 2) {
        surf_blend(v, x, y, w, 1, col);
        surf_blend(v, x, y + h - 1, w, 1, col);
        return;
    }
    surf_blend(v, x, y, w, h, col);
}

/* Every block's background and its top/bottom rules. Painted before the
 * text, in document order, so a child box sits on its parent's background
 * exactly as it does in CSS. Boxes with nothing to paint cost one branch. */
static void draw_page_boxes(Surface *v, Web *w) {
    Doc *d = &w->doc;
    /* CSS propagates a body background to the canvas when the root does not
     * set one. Painting it over the whole page view is what makes a page
     * that says `body { background: #fff }` look like a page and not like a
     * white strip with the desktop showing at its edges. */
    for (int i = 0; i < d->nblock; i++) {
        Block *b = &d->block[i];
        if (b->is_body && (b->background >> 24)) {
            surf_blend(v, 0, 0, v->w, v->h, b->background);
            break;
        }
    }
    for (int i = 0; i < d->nblock; i++) {
        Block *b = &d->block[i];
        if (b->box_x1 <= b->box_x0) continue;
        int top = (int)b->box_y0 - w->scroll;
        int bot = (int)b->box_y1 - w->scroll;
        if (bot <= 0 || top >= v->h) continue;
        int cw = (int)b->box_x1 - (int)b->box_x0;

        if (b->background >> 24)
            surf_blend(v, b->box_x0, top, cw, bot - top, b->background);
        if (b->border_top_w > 0 && b->border_top_style != CSS_BORDER_NONE)
            draw_rule(v, b->box_x0, top, cw, ui_px(b->border_top_w),
                      b->border_top_style, b->border_top_color);
        if (b->border_bottom_w > 0 && b->border_bottom_style != CSS_BORDER_NONE)
            draw_rule(v, b->box_x0, bot - ui_px(b->border_bottom_w), cw,
                      ui_px(b->border_bottom_w), b->border_bottom_style,
                      b->border_bottom_color);
    }
}

/* The selection, resolved for the painter. The model itself lives with the
 * input handlers further down; these two queries are all the painter needs. */
static int web_measure(const Atom *a, const Seg *sg, int upto);
static int web_sel_span(const Web *w, int line, int seg, int *b0, int *b1);

static void draw_page_lines(Surface *v, Web *w) {
    Doc *d = &w->doc;
    draw_page_boxes(v, w);
    if (d->nline <= 0) return;

    int first = line_at(d, (u32)w->scroll);
    for (int i = first; i < d->nline; i++) {
        Line *ln = &d->line[i];
        int ly = (int)ln->y - w->scroll;
        if (ly >= v->h) break;
        if (ly + (int)ln->h <= 0) continue;
        /* Half-leading: a line-height larger than the face puts the same
         * amount of space above the text as below it. */
        int ty = ly + ((int)ln->h - (int)ln->th) / 2;

        for (int j = 0; j < (int)ln->n; j++) {
            Seg *sg = &d->seg[ln->first + j];
            Atom *a = &d->atom[sg->atom];

            if (a->flags & AF_IMAGE) {
                if (a->img >= 0 && a->img < d->nimg) {
                    PageImg *pi = &d->img[a->img];
                    int ww = (int)sg->w;
                    if (pi->pic.px && pi->pic.w > 0 && ww > 0) {
                        int hh = (int)((long)pi->pic.h * ww / pi->pic.w);
                        int iy = ly + ((int)ln->h - hh) / 2;
                        draw_img_seg(v, pi, (int)sg->x, iy, ww, hh);
                    }
                }
                continue;
            }
            if (sg->len == 0) continue;

            Font *f = font_for_atom(a);
            if (!f) continue;
            int len = sg->len;
            char tmp[WEB_ATOM_TEXT + 4];
            if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
            memcpy(tmp, a->text + sg->start, (size_t)len);
            tmp[len] = 0;

            /* The page's own CSS decides the colour when a rule set one on
             * this block or an ancestor; otherwise the theme defaults stand,
             * which is what keeps an unstyled page readable on the dark
             * desktop instead of turning it black on black. */
            const Block *blk = (a->block != 0xFFFF && a->block < (u16)d->nblock)
                               ? &d->block[a->block] : NULL;
            u32 col;
            if (a->link >= 0) col = w->col_link ? w->col_link
                                                : color_for_link(a->flags & AF_LINKNUM);
            else if (blk && blk->has_color) col = blk->color;
            else if (a->level) col = w->col_head ? w->col_head : C_HEAD;
            else col = w->col_text ? w->col_text : C_TEXT;

            /* The selection sits behind the glyphs of the very run that is
             * about to be drawn, and is measured with the same face, so the
             * highlight starts and ends between the letters the user picked. */
            {
                int b0 = 0, b1 = 0;
                if (web_sel_span(w, i, (int)(ln->first + j), &b0, &b1)) {
                    int hx0 = (int)sg->x + web_measure(a, sg, b0);
                    int hx1 = (int)sg->x + web_measure(a, sg, b1);
                    int band = ly + ((int)ln->h - (int)ln->th) / 2;
                    if (hx1 <= hx0) hx1 = hx0 + 1;
                    surf_blend(v, hx0, band, hx1 - hx0, (int)ln->th,
                               color_alpha(0xFF4FD1C5u, 0.32f));
                }
            }
            draw_run(v, f, a, tmp, (float)sg->x, (float)ty, col);
            draw_decoration(v, f, a, sg, ty, col);
        }
    }

    /* One short line of feedback ("copied 214 bytes") for two and a half
     * seconds, in the corner of the page it is about: a copy with no visible
     * answer looks like a copy that did nothing. */
    if (w->note[0]) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now = (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
        if (now - w->note_at > 2500) {
            w->note[0] = 0;
        } else {
            Font *fd = field_font();
            if (fd) {
                int pad = ui_px(6);
                int tw = (int)(font_width(fd, w->note) + 0.5f);
                int th = (int)(font_height(fd) + 0.5f);
                int bw = tw + 2 * pad, bh = th + pad;
                int bx = v->w - bw - ui_px(8);
                int by = v->h - bh - ui_px(8);
                if (bx < ui_px(4)) { bx = ui_px(4); bw = v->w - 2 * ui_px(4); }
                if (bw < 1) bw = 1;
                if (bh < 1) bh = 1;
                if (by < 0) by = 0;
                gfx_rrect_aa(v, (float)bx, (float)by, (float)bw, (float)bh,
                             (float)ui_px(3), 0xE61A1D23u, 0, 0);
                draw_fit(v, fd, (float)(bx + pad), (float)(by + pad / 2),
                         (float)(bw - 2 * pad), w->note, C_TEXT);
            }
            w->dirty = 1;
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
    else {
        /* body_bytes is this document; raw_bytes/elapsed_ms are totals
         * across every redirect leg, every stylesheet and every image
         * fetch. Saying which is which is the only honest way to print
         * one line. The stylesheet count and size are what really
         * arrived through <link rel=stylesheet>, and `cap` is printed only
         * when one of the two caps actually stopped a sheet. */
        char css[48];
        css[0] = 0;
        if (w->css_sheets > 0 || w->css_capped)
            snprintf(css, sizeof(css), "  css %d sheet%s %ldK%s",
                     w->css_sheets, w->css_sheets == 1 ? "" : "s",
                     (w->css_bytes + 1023) / 1024,
                     w->css_capped ? " (cap)" : "");
        snprintf(left, sizeof(left), "HTTP %d  %ld bytes total  %ld ms total%s%s",
                 w->status, w->raw_bytes, w->elapsed_ms, css,
                 (w->truncated || w->doc.truncated) ? "  (truncated)" : "");
    }

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
        /* Never print the requested hostname as if it were the certificate
         * subject. The leaf either has a CN, a real subject string, or only
         * SANs - in which case the handshake still proved the name, so
         * that is exactly what the line says. */
        char cert[256];
        if (w->tls_cn[0]) snprintf(cert, sizeof(cert), "CN=%s", w->tls_cn);
        else if (w->tls_subject[0]) snprintf(cert, sizeof(cert), "%s", w->tls_subject);
        else snprintf(cert, sizeof(cert), "matched by SAN");
        char tls[WEB_ERR_MAX + 96];
        snprintf(tls, sizeof(tls), "%s  %s  %s  expires %s",
                 w->tls_version[0] ? w->tls_version : "TLS",
                 w->tls_cipher[0] ? w->tls_cipher : "?",
                 cert,
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
    /* A Surface carries an optional clip rectangle; "cx1 <= cx0" means
     * none. Leaving it uninitialised is undefined behaviour, and a stack
     * word that happens to look like a clip made the whole page vanish. */
    surf_unclip(&s);

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
        /* draw_start(), draw_page_lines() and the painter all draw into
         * this view; its own clip is exactly its own rectangle. */
        surf_clip(&v, 0, 0, cw, ch);

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
    w->doc.nblock = 0;
    w->doc.nlink = 0;
    w->doc.doc_h = 0;
    w->doc.truncated = 0;
    /* Images belonged to the page that is being discarded. They were left in
     * place, so the next site inherited them - a logo from a previously
     * visited page appeared under a different site - and their pixels were
     * never freed either. */
    for (int i = 0; i < w->doc.nimg; i++) {
        img_free(&w->doc.img[i].pic);
        w->doc.img[i].tried = 0;
        w->doc.img[i].url[0] = 0;
        w->doc.img[i].alt[0] = 0;
    }
    w->doc.nimg = 0;
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
    w->tls_subject[0] = 0;
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

/* An <img src> resolved against the page it came from. Absolute URLs pass
 * through; "/x" and "x" both land on the page's own host. */
static void img_url(const Url *base, const char *src, char *out, size_t n) {
    if (strstr(src, "://")) { snprintf(out, n, "%s", src); return; }
    /* Keep the port. The first version dropped it, so every image on a page
     * served from a non-default port was requested from port 80 and failed. */
    char hostport[WEB_PATH_MAX];
    if (base->port[0] &&
        !((!strcmp(base->scheme, "http") && !strcmp(base->port, "80")) ||
          (!strcmp(base->scheme, "https") && !strcmp(base->port, "443")))) {
        snprintf(hostport, sizeof(hostport), "%s:%s", base->host, base->port);
    } else {
        snprintf(hostport, sizeof(hostport), "%s", base->host);
    }
    if (src[0] == '/') {
        snprintf(out, n, "%s://%s%s", base->scheme, hostport, src);
        return;
    }
    const char *slash = strrchr(base->path, '/');
    char dir[WEB_PATH_MAX];
    if (slash) {
        size_t k = (size_t)(slash - base->path) + 1;
        if (k >= sizeof(dir)) k = sizeof(dir) - 1;
        memcpy(dir, base->path, k); dir[k] = 0;
    } else {
        snprintf(dir, sizeof(dir), "/");
    }
    snprintf(out, n, "%s://%s%s%s", base->scheme, hostport, dir, src);
}

/* What the stylesheet says the page's default text, link and heading
 * colours are. The per-block work (backgrounds, borders, widths, align,
 * line-height) happens in html_build()/doc_layout() against the same
 * resolved style; floats, flex and grid are not things this text renderer
 * can honour, and pretending would be worse than saying so. Falls back to
 * the built-in palette when the page sets nothing. */
static void css_apply(Web *w) {
    CssStyle body = css_for(&w->sheet, "body", NULL, NULL, NULL);
    CssStyle link = css_for(&w->sheet, "a", NULL, NULL, NULL);
    CssStyle head = css_for(&w->sheet, "h1", NULL, NULL, NULL);
    w->col_text = (body.set & CSS_SET_COLOR) ? body.color : C_TEXT;
    w->col_link = (link.set & CSS_SET_COLOR) ? link.color : C_LINK;
    w->col_head = (head.set & CSS_SET_COLOR) ? head.color : C_HEAD;
}

/* Build the page's cascade: every stylesheet the document referenced, in
 * document order, inline <style> bodies and fetched .css files in one
 * sheet. `src` is the document the first parse pass produced, whose
 * csssrc[] list names them in source order.
 *
 * This is the step that made CSS "not work on real sites": a real site
 * puts its rules in <link rel=stylesheet> files, and there was no <link>
 * handler at all, so the sheet was always empty. The two caps are the
 * browser's own (WEB_MAX_CSSSRC references, WEB_CSS_BYTES fetched bytes)
 * and both are reported, not silently applied.
 *
 * The transport is a parameter for the same reason load_images() takes
 * one: a host harness runs this exact function over captured bytes. */
static void load_stylesheets(Web *w, const Doc *src, const WebTransport *tr) {
    static char scratch[WEB_RAW_MAX];

    css_sheet_init(&w->sheet);
    w->css_sheets = 0;
    w->css_bytes = 0;
    w->css_capped = src->css_capped ? 1 : 0;

    for (int i = 0; i < src->ncsssrc; i++) {
        const CssSrc *s = &src->csssrc[i];
        if (!s->is_link) {
            if (s->len > 0 && s->off >= 0 &&
                (size_t)s->off + (size_t)s->len <= sizeof(w->css_raw)) {
                css_parse(w->css_raw + s->off, (size_t)s->len, &w->sheet);
                w->css_sheets++;
            }
            continue;
        }
        if (w->css_capped || w->css_bytes >= WEB_CSS_BYTES ||
            w->css_sheets >= WEB_MAX_CSSSRC) {
            w->css_capped = 1;
            continue;
        }
        Url u;
        char err[128];
        err[0] = 0;
        if (url_parse(s->url, &u, err, sizeof(err)) != 0) continue;
        if (!url_fetchable(&u, err, sizeof(err))) continue;

        Fetch f;
        web_fetch(&f, tr, &u, scratch);
        /* The footer's totals are the page's load: a stylesheet is part of
         * it, exactly like an image. */
        w->raw_bytes += f.raw_bytes;
        w->elapsed_ms += f.elapsed_ms;
        /* A 404 body is not a stylesheet: only a 2xx answer with bytes is
         * parsed. The request itself is still counted in raw_bytes above,
         * because it really crossed the wire. */
        if (!f.ok || f.status < 200 || f.status >= 300) continue;
        if (!f.body || f.body_len <= 0) continue;

        long room = WEB_CSS_BYTES - w->css_bytes;
        long take = f.body_len < room ? f.body_len : room;
        if (take < f.body_len) w->css_capped = 1;
        if (take <= 0) { w->css_capped = 1; continue; }
        css_parse(f.body, (size_t)take, &w->sheet);
        w->css_bytes += take;
        w->css_sheets++;
    }
    w->css_ready = 1;
}

/* Fetch and decode the images the page referenced. Called once per load,
 * after the body is in: the browser has exactly one connection in flight at
 * a time, so this is the only place it can happen. A failure is recorded on
 * that image and never stops the page from rendering.
 *
 * The transport is a parameter, not a global: the accounting below is the
 * only place where a picture's bytes and milliseconds reach the footer, and
 * a host harness has to be able to run this very function over a transport
 * of its own to prove that it does. */
static void load_images(Web *w, const WebTransport *tr) {
    char abs[WEB_PATH_MAX];
    /* web_fetch() is allowed to fill WEB_RAW_MAX bytes, so the buffer must
     * be that large; it used to be WEB_IMG_MAX, which let a large response
     * run past the end of the scratch buffer. */
    static char scratch[WEB_RAW_MAX];
    for (int i = 0; i < w->doc.nimg; i++) {
        PageImg *pi = &w->doc.img[i];
        if (pi->tried) continue;
        pi->tried = 1;
        img_url(&w->cur, pi->url, abs, sizeof(abs));
        Url u;
        char err[128] = {0};
        if (url_parse(abs, &u, err, sizeof(err)) != 0) continue;
        if (!url_fetchable(&u, err, sizeof(err))) continue;
        Fetch f;
        web_fetch(&f, tr, &u, scratch);
        /* The footer totals include the picture fetches: a 125 ms page
         * plus three images is not a 0 ms load. */
        w->raw_bytes += f.raw_bytes;
        w->elapsed_ms += f.elapsed_ms;
        if (!f.ok || !f.body || f.body_len <= 0) continue;
        if (f.body_len > WEB_IMG_MAX) continue;   /* decoder cap, stated */
        char derr[128] = {0};
        if (img_decode((const unsigned char *)f.body, (size_t)f.body_len,
                       &pi->pic, derr, sizeof(derr)) != 0) {
            pi->pic.px = NULL;
        }
    }
}


#ifndef WEB_DRAW_ONLY            /* nothing below runs without a socket */

/* Load one address. The only part of the page path that needs a real
 * socket: everything above (the request, the totals, the parser, the
 * images) takes its transport as an argument. */
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

    /* The page that is about to be replaced owns the old pictures: free
     * them before the document is rebuilt, or a logo from the previous
     * site would sit in this page's image table with live pixels. */
    web_clear_page(w);
    /* web_clear_page() zeroed the counters; the page's real measurements
     * still belong to this load. */
    w->raw_bytes = f.raw_bytes;
    w->elapsed_ms = f.elapsed_ms;
    w->truncated = f.truncated;

    w->cur = u;
    snprintf(w->url, sizeof(w->url), "%s", f.url);
    snprintf(w->edit, sizeof(w->edit), "%s", f.url);
    /* The padlock is earned by a verified chain, not merely by a session
     * object existing. */
    w->tls_ok = f.tls && f.tls_chain_ok;
    snprintf(w->tls_version, sizeof(w->tls_version), "%s", f.tls_version);
    snprintf(w->tls_cipher, sizeof(w->tls_cipher), "%s", f.tls_cipher);
    snprintf(w->tls_cn, sizeof(w->tls_cn), "%s", f.tls_cn);
    snprintf(w->tls_subject, sizeof(w->tls_subject), "%s", f.tls_subject);
    snprintf(w->tls_expiry, sizeof(w->tls_expiry), "%s", f.tls_expiry);
    w->status = f.status;
    w->body_bytes = f.body_bytes;
    w->truncated = f.truncated || (f.body_len >= WEB_RAW_MAX - 1);
    snprintf(w->charset, sizeof(w->charset), "%s", f.charset);

    char title[WEB_TITLE_MAX];
    /* Two passes, because the stylesheets live in <head> and the block
     * tags in <body> need their size and margin as they are met. The first
     * pass collects the sheet references only - every <style> body and
     * every <link rel=stylesheet href> in document order; load_stylesheets()
     * then fetches the external ones and builds one cascade; the second
     * pass lays the document out with that cascade. */
    size_t css_len = 0;
    Doc scratch;
    html_build(&scratch, f.body, f.body_len, &w->cur, f.latin1, title, sizeof(title),
               w->css_raw, sizeof(w->css_raw), &css_len, NULL);
    load_stylesheets(w, &scratch, &net_transport);
    css_apply(w);
    html_build(&w->doc, f.body, f.body_len, &w->cur, f.latin1, title, sizeof(title),
               w->css_raw, sizeof(w->css_raw), &css_len, &w->sheet);
    snprintf(w->title, sizeof(w->title), "%s", title);

    w->scroll = 0;
    w->laid_w = 0;                 /* force a fresh layout at this width */
    w->state = ST_PAGE;
    /* the old page's selection is not this page's text */
    w->sel = 0;
    w->sel_drag = 0;
    w->link_hit = -1;
    load_images(w, &net_transport);
}


#endif /* !WEB_DRAW_ONLY */         /* web_load: the only part that needs sockets */

/* ------------------------------------------------------------ input */

/* The input handling is deliberately outside the WEB_DRAW_ONLY guard: it
 * touches no socket, and a host harness can drive the wheel, the keys and
 * the clicks against a real rendered frame. That is how the scroll behaviour
 * below is proved without a window. */

static int scroll_step(void) {
    Font *f = font_get(FONT_REGULAR, (float)style_size(0));
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

/* ------------------------------------------------------------ selection
 *
 * The page is atoms, segments and lines; a selection is two positions in that
 * structure, so it costs no copy of the text and it survives a repaint at a
 * different width (the layout moves, the offsets do not). Copying walks the
 * same segments the painter draws and reconstructs the plain text: spaces
 * where the layout left a gap, a newline at the end of each line.
 *
 * Everything here is compiled in the WEB_DRAW_ONLY host build too, so a
 * harness can press, drag and copy against real rendered pages.
 * ------------------------------------------------------------ */

static int web_pos_cmp(int l1, int s1, int o1, int l2, int s2, int o2) {
    if (l1 != l2) return l1 < l2 ? -1 : 1;
    if (s1 != s2) return s1 < s2 ? -1 : 1;
    if (o1 != o2) return o1 < o2 ? -1 : 1;
    return 0;
}

/* Where inside one segment a page x lands. Measured with the face the run is
 * painted in, on real prefixes, and only at UTF-8 boundaries. */
static int web_off_in_seg(const Doc *d, const Seg *sg, int dx) {
    const Atom *a = &d->atom[sg->atom];
    if (sg->len == 0) return 0;
    Font *f = font_for_atom(a);
    if (!f) return 0;
    char tmp[WEB_ATOM_TEXT + 4];
    int len = sg->len;
    if (len > WEB_ATOM_TEXT) len = WEB_ATOM_TEXT;
    memcpy(tmp, a->text + sg->start, (size_t)len);
    tmp[len] = 0;
    float prev = 0.0f;
    int i = 0;
    while (i < len) {
        int gl = utf8_len((unsigned char)tmp[i]);
        if (gl < 1 || i + gl > len) break;
        char save = tmp[i + gl];
        tmp[i + gl] = 0;
        float pre = font_width(f, tmp);
        tmp[i + gl] = save;
        if ((float)dx < (prev + pre) * 0.5f) return i;
        prev = pre;
        i += gl;
    }
    return len;
}

/* The document position under a page-space point. Clamped to the document,
 * so a drag past the edge selects up to the edge rather than nowhere. */
static void web_pos_at(Web *w, int lx, int ly, int *line, int *seg, int *off) {
    Doc *d = &w->doc;
    *line = 0; *seg = 0; *off = 0;
    if (d->nline <= 0) return;
    int y = ly + w->scroll;
    int li = d->nline - 1;
    for (int i = 0; i < d->nline; i++) {
        int top = (int)d->line[i].y;
        int bot = top + (int)d->line[i].h;
        if (y < bot) { li = i; break; }
    }
    if (y < (int)d->line[0].y) li = 0;
    Line *ln = &d->line[li];
    int n = (int)ln->n;
    *line = li;
    if (n <= 0) { *seg = (int)ln->first; *off = 0; return; }
    for (int j = 0; j < n; j++) {
        Seg *sg = &d->seg[ln->first + j];
        int x0 = (int)sg->x, x1 = x0 + (int)sg->w;
        if (lx < x0) { *seg = (int)(ln->first + j); *off = 0; return; }
        if (lx <= x1) {
            *seg = (int)(ln->first + j);
            *off = web_off_in_seg(d, sg, lx - x0);
            return;
        }
    }
    Seg *last = &d->seg[ln->first + n - 1];
    *seg = (int)(ln->first + n - 1);
    *off = (int)last->len;
}

/* Point the selection at one position (both ends, i.e. an empty range). */
static void web_sel_set(Web *w, int line, int seg, int off) {
    w->sel = 1;
    w->sl = w->el = line;
    w->ss = w->es = seg;
    w->so = w->eo = off;
    w->dirty = 1;
}

/* Begin a press: remember where it landed, so a drag can tell movement from
 * a click and a click can still follow a link. */
static void web_sel_press(Web *w, int lx, int ly) {
    int l, sg, o;
    web_pos_at(w, lx, ly, &l, &sg, &o);
    web_sel_set(w, l, sg, o);
    w->sel_drag = 1;
    w->sel_moved = 0;
    w->press_x = lx;
    w->press_y = ly;
    w->link_hit = link_at(w, lx, ly);
}

static void web_sel_extend(Web *w, int lx, int ly) {
    int l, sg, o;
    if (!w->sel) return;
    if (lx != w->press_x || ly != w->press_y) w->sel_moved = 1;
    web_pos_at(w, lx, ly, &l, &sg, &o);
    if (l == w->el && sg == w->es && o == w->eo) return;
    w->el = l;
    w->es = sg;
    w->eo = o;
    w->dirty = 1;
}

static void web_sel_clear(Web *w) {
    if (!w->sel) return;
    w->sel = 0;
    w->dirty = 1;
}

/* The whole document, first text run to last. */
static void web_sel_all(Web *w) {
    Doc *d = &w->doc;
    if (d->nline <= 0) { web_sel_clear(w); return; }
    int l0 = 0, s0 = (int)d->line[0].first, o0 = 0;
    for (int i = 0; i < d->nline; i++) {
        Line *ln = &d->line[i];
        if (ln->n > 0) { l0 = i; s0 = (int)ln->first; break; }
    }
    int l1 = d->nline - 1, s1 = (int)(d->line[l1].first + (d->line[l1].n ? d->line[l1].n - 1 : 0));
    int o1 = d->nseg > s1 ? (int)d->seg[s1].len : 0;
    w->sel = 1;
    w->sl = l0; w->ss = s0; w->so = o0;
    w->el = l1; w->es = s1; w->eo = o1;
    w->dirty = 1;
}

/* The selected text, plain UTF-8, malloc'ed. NULL when the range is empty. */
static char *web_sel_text(Web *w, int *out_len) {
    if (out_len) *out_len = 0;
    Doc *d = &w->doc;
    if (!w->sel || d->nline <= 0) return NULL;
    int l0 = w->sl, s0 = w->ss, o0 = w->so;
    int l1 = w->el, s1 = w->es, o1 = w->eo;
    if (web_pos_cmp(l0, s0, o0, l1, s1, o1) > 0) {
        int tl = l0, ts = s0, to = o0;
        l0 = l1; s0 = s1; o0 = o1;
        l1 = tl; s1 = ts; o1 = to;
    }
    if (l0 < 0) l0 = 0;
    if (l1 >= d->nline) l1 = d->nline - 1;
    if (l1 < l0) return NULL;

    size_t cap = (size_t)(l1 - l0 + 1) * 2u;
    for (int line = l0; line <= l1; line++) {
        Line *ln = &d->line[line];
        int first = (line == l0 && s0 > (int)ln->first) ? s0 : (int)ln->first;
        int last = (line == l1 && s1 < (int)(ln->first + ln->n))
                   ? s1 : (int)(ln->first + ln->n) - 1;
        for (int si = first; si <= last && si < d->nseg; si++)
            cap += (size_t)d->seg[si].len + 2u;
    }
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    int wrote = 0;

    for (int line = l0; line <= l1; line++) {
        Line *ln = &d->line[line];
        int first = (line == l0 && s0 > (int)ln->first) ? s0 : (int)ln->first;
        int last = (line == l1 && s1 < (int)(ln->first + ln->n))
                   ? s1 : (int)(ln->first + ln->n) - 1;
        if (last >= (int)d->nseg) last = (int)d->nseg - 1;
        int prev_right = -1;
        int wrote_line = 0;
        for (int si = first; si <= last; si++) {
            Seg *sg = &d->seg[si];
            const Atom *a = &d->atom[sg->atom];
            if (a->flags & AF_IMAGE) continue;
            int x0 = (int)sg->x;
            int x1 = x0 + (int)sg->w;
            int b0 = (si == s0) ? o0 : 0;
            int b1 = (si == s1) ? o1 : (int)sg->len;
            if (b0 < 0) b0 = 0;
            if (b1 > (int)sg->len) b1 = (int)sg->len;
            if (b1 <= b0) { prev_right = x1; continue; }
            /* A gap between two runs is a space, however CSS produced it. */
            if (prev_right >= 0 && x0 > prev_right + 1 && o + 1 < cap) {
                out[o++] = ' ';
            }
            if (!wrote_line && wrote) {
                if (o + 1 < cap) out[o++] = '\n';
            }
            wrote_line = 1;
            wrote = 1;
            memcpy(out + o, a->text + sg->start + b0, (size_t)(b1 - b0));
            o += (size_t)(b1 - b0);
            prev_right = x1;
        }
        if (!wrote_line && wrote) {
            /* an empty line inside the range is a real blank line */
            if (o + 1 < cap) out[o++] = '\n';
        }
    }
    while (o > 0 && out[o - 1] == '\n') o--;
    out[o] = 0;
    if (o == 0) { free(out); return NULL; }
    if (out_len) *out_len = (int)o;
    return out;
}

/* How wide the first `upto` bytes of a segment are in its own face. */
static int web_measure(const Atom *a, const Seg *sg, int upto) {
    Font *f = font_for_atom(a);
    if (!f || upto <= 0) return 0;
    if (upto > (int)sg->len) upto = (int)sg->len;
    char tmp[WEB_ATOM_TEXT + 4];
    memcpy(tmp, a->text + sg->start, (size_t)upto);
    tmp[upto] = 0;
    return (int)(font_width(f, tmp) + 0.5f);
}

/* The part of segment `seg` on line `line` that the selection covers, as
 * byte offsets inside the segment. Returns 0 when that segment is outside
 * the range. */
static int web_sel_span(const Web *w, int line, int seg, int *b0, int *b1) {
    const Doc *d = &w->doc;
    if (!w->sel) return 0;
    int l0 = w->sl, s0 = w->ss, o0 = w->so;
    int l1 = w->el, s1 = w->es, o1 = w->eo;
    if (web_pos_cmp(l0, s0, o0, l1, s1, o1) > 0) {
        int tl = l0, ts = s0, to = o0;
        l0 = l1; s0 = s1; o0 = o1;
        l1 = tl; s1 = ts; o1 = to;
    }
    if (line < l0 || line > l1) return 0;
    if (seg < s0 || seg > s1) return 0;
    *b0 = (seg == s0) ? o0 : 0;
    *b1 = (seg == s1) ? o1 : (int)d->seg[seg].len;
    if (*b0 < 0) *b0 = 0;
    if (*b1 > (int)d->seg[seg].len) *b1 = (int)d->seg[seg].len;
    return *b1 > *b0;
}

static void web_note(Web *w, const char *text) {
    snprintf(w->note, sizeof(w->note), "%s", text);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    w->note_at = (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
    w->dirty = 1;
}

/* Ctrl+C: put the selection on the clipboard through the bridge. The bytes
 * that cross are the plain text web_sel_text() built - never the atoms. */
static void web_copy(Web *w) {
    int len = 0;
    char *text = web_sel_text(w, &len);
    if (!text) { web_note(w, "no text is selected"); return; }
    int rc = clip_bridge_set_text(text, len);
    free(text);
    char msg[96];
    if (rc == 0) snprintf(msg, sizeof(msg), "copied %d bytes", len);
    else         snprintf(msg, sizeof(msg), "copy failed: %s", clip_bridge_strerror(rc));
    web_note(w, msg);
}

/* Ctrl+V in the address bar: append the clipboard's text, with line breaks
 * turned into spaces (a URL field is one line). */
static void web_paste_address(Web *w) {
    char *text = NULL;
    int len = 0;
    int rc = clip_bridge_get_text(&text, &len);
    if (rc != 0) { web_note(w, "paste: the clipboard holds no text"); return; }
    char *one = malloc((size_t)len + 1);
    if (!one) { free(text); web_note(w, "paste: out of memory"); return; }
    int o = 0;
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if ((unsigned char)c < 0x20) continue;
        one[o++] = c;
    }
    one[o] = 0;
    edit_put(w, one);
    free(one);
    free(text);
    web_note(w, "pasted into the address bar");
}

static void handle_key(Web *w, int key, int mods) {
    /* Clipboard chords first: they must work whether or not the address bar
     * has the focus, because the page is what the user is looking at. */
    if (mods & 2) {
        if (key == K_C) { web_copy(w); return; }            /* Ctrl+C */
        if (key == K_A) { web_sel_all(w); return; }         /* Ctrl+A */
        if (key == K_V) {                                   /* Ctrl+V */
            if (w->edit_focus) web_paste_address(w);
            return;
        }
    }
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

    /* Release: the desktop sends it so a drag can end. A press that never
     * moved is a click, and a click on a link follows it; a drag selects. */
    if (!(buttons & 1)) {
        if (prev & 1) {
            if (w->sel_drag && !w->sel_moved && w->link_hit >= 0)
                follow_link(w, w->link_hit);
            w->sel_drag = 0;
            w->link_hit = -1;
            w->dirty = 1;
        }
        return;
    }

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

    /* A press in the page takes the keyboard away from the address bar (so
     * Ctrl+C copies the page, not the URL) and starts a selection. The drag
     * itself arrives as further MOUSE events with the button still down. */
    w->edit_focus = 0;
    if (prev & 1) {
        web_sel_extend(w, lx, ly);
        return;
    }
    web_sel_press(w, lx, ly);
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

    if (!font_get(FONT_REGULAR, (float)style_size(0))) {
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
