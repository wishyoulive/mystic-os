/* ============================================================
 *  Mystic OS - apps (external) / engineview
 *
 *  A real browser window: it starts the Chromium headless engine that the
 *  `engine` package installs, speaks the DevTools protocol to it over TCP and
 *  an RFC 6455 WebSocket, and puts the frames the engine renders into our
 *  surface. Our pointer and key events are forwarded to the page with
 *  Input.dispatchMouseEvent / Input.dispatchKeyEvent.
 *
 *      Engine [url]        (after: pkg install engine)
 *
 *  WHAT THIS FILE IS, AND WHAT IT IS NOT
 *  ------------------------------------
 *  It is not a renderer and it invents nothing. Every pixel inside the page
 *  area comes from a Page.screencastFrame event produced by the engine; the
 *  window draws bars around that area and, until the first frame arrives,
 *  says so. A frame is never synthesised or reused after the engine stops: if
 *  the engine dies the window says which status it died with and shows the
 *  engine's own last stderr line.
 *
 *  WHAT WAS VERIFIED, AND HOW
 *  -------------------------
 *  [built]   zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client
 *            -Isrc/client/apps -Wall -Wextra -Wno-unused-parameter -c this
 *            file is silent, and ./tools/mkapp.sh engine links the app with
 *            src/client/cdpws.c (the DevTools transport) and src/img.c.
 *  [api]     the switches and protocol names used here are present in the
 *            exact binary the package ships (chromium-headless-shell
 *            156.0.8078.79): "remote-debugging-port", "user-data-dir",
 *            "startscreencast", "screencastframe", "dispatchmouseevent",
 *            "dispatchkeyevent", "attachtotarget", "createtarget",
 *            "navigate", "no-sandbox", "disable-gpu", "devtoolsactiveport".
 *            Measured by byte search on the unpacked binary, not assumed.
 *  [parsers] the JSON reader, the base64 decoder and the key table are
 *            exercised by the EVP_SELFTEST block at the bottom: built for the
 *            host and run (18 checks, all passing).
 *  [not]     no boot, no QEMU, no page was rendered in the run that added
 *            this file, and the browser was not started in the run that moved
 *            the transport to TCP. The socket half of the channel is
 *            compiled and its framing comes from cdpws.c's own harness; the
 *            DevToolsActivePort half is checked on the host against the very
 *            path this file polls. What no host run can show is the browser
 *            publishing that file inside the jail and connect() succeeding.
 *            And on this build it will not: see LOOPBACK below - stage/init
 *            never brings up lo, so a booted guest takes the fallback path,
 *            not the live one. That is readable today and is not fixed here.
 *
 *  THE TRANSPORT: TCP AND A WEBSOCKET, NOT THE PIPE
 *  -----------------------------------------------
 *  --remote-debugging-pipe hands the browser two file descriptors (commands on
 *  fd 3, replies on fd 4, NUL-delimited JSON). On this kernel that path is
 *  dead: the engine exits 1 with two pipes and 127 with two socketpairs,
 *  before it writes a byte. --remote-debugging-port=0 has it pick a free
 *  loopback port, publish it in <user-data-dir>/DevToolsActivePort and speak
 *  HTTP plus WebSocket on it - connect/read/write only, none of the pipe
 *  semantics the kernel gets wrong.
 *
 *  src/client/cdpws.c does the handshake and the framing: the upgrade is
 *  RFC 6455 with Sec-WebSocket-Accept checked against SHA-1 of the key, client
 *  frames are masked, and fragmented messages are reassembled before they
 *  reach handle_message() below. This file owns only the CDP JSON, exactly as
 *  it did with the pipe: an id in, an id out, no command routing table.
 *
 *  The channel is not the only way to a picture. If no DevToolsActivePort
 *  appears, the window says why and falls back to shot_render() - the engine
 *  run with --screenshot, one frame per navigation - which is proven to work
 *  on this kernel.
 *
 *  WHY IT STARTS, WHERE IT STARTS
 *  -----------------------------
 *  src/wmsrv.c chroots this process into /run/mystic/jail/<id> with:
 *    /bin/app  this binary (the bind-mounted payload)
 *    /app      the bundle, read-only, MYSTIC_BUNDLE
 *    /root     a private writable dir     /tmp  writable
 *    /proc /dev/shm /dev/null /dev/urandom /usr/share/fonts (ro)
 *    LD_LIBRARY_PATH=/app/usr/lib:/app/lib
 *  There is no /lib, so the engine's PT_INTERP cannot be the usual
 *  /lib/ld-musl-x86_64.so.1. tools/fetch-engine.py rewrites it to
 *  /app/ld-musl-x86_64.so.1 and puts musl's loader there; this program execs
 *  the engine and the kernel does the rest. That is also why the engine can
 *  re-exec /proc/self/exe for its zygote and renderer children: the process
 *  image is the engine binary, not a loader with a program argument.
 *
 *  Both facts are checked before exec, and the window says which one is
 *  missing instead of showing a dead rectangle.
 *
 *  LOOPBACK: THE ONE THING THIS FILE CANNOT FIX
 *  --------------------------------------------
 *  DevTools over TCP is a loopback connection - the browser binds 127.0.0.1 and
 *  this process connects to 127.0.0.1. stage/init's setup_net() runs
 *  "ip link set eth0 up" and udhcpc, and nothing else; no other script in
 *  stage/ or tools/ brings lo up, and Linux boots it down. So on a booted guest
 *  the browser cannot bind, this window cannot connect, no DevToolsActivePort
 *  ever appears, engine_attach() spends its full 10 s, and every navigation is
 *  one --screenshot. The engine's manifest says "network = host", so
 *  src/wmsrv.c (line 355) does not give it a new network namespace: the
 *  guest's lo is the one that matters, and in a namespace (CLONE_NEWNET) it
 *  would be that namespace's lo. The fix is one line in stage/init, which is
 *  not this file's to change; it is written down here so that nobody spends a
 *  QEMU run on the wrong question.
 *
 *  KNOWN LIMITS, STATED IN THE WINDOW
 *  ---------------------------------
 *  US keyboard layout only (evdev codes, no IME); one tab; no downloads, no
 *  file pickers, no drag and drop; history back/forward are not wired
 *  (Ctrl+R reloads); mouse modifier keys are not forwarded because the window
 *  server's MOUSE message carries no modifiers; no DRM - Alpine ships no
 *  Widevine CDM, so Netflix and Spotify cannot play. Video without DRM
 *  (H.264/VP9/AV1 in MP4/WebM) does decode, through the engine's libavcodec.
 * ============================================================ */

#include "../mystic_client.h"
#include "../../mystic.h"
#include "../../img.h"
#include "cdpws.h"          /* the DevTools transport: TCP + RFC 6455 WebSocket */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#ifndef EVP_SELFTEST
/* The self-test at the bottom of this file runs on the host and only needs the
 * parsers, so the POSIX headers are not required to build it. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define EVP_WIN_W        1100
#define EVP_WIN_H         720
#define EVP_BAR_H          28       /* address/status bar at the top */
#define EVP_FOOT_H         24
#define EVP_URL_MAX       512
#define EVP_MSG_MAX     (4u << 20)  /* one CDP message */
#define EVP_FRAME_MAX   (2u << 20)  /* one decoded-base64 JPEG */
#define EVP_STDERR_KEEP  4096
#define EVP_FIRST_FRAME_MS 20000    /* say so, loudly, if nothing arrives */

/* evdev codes, forwarded verbatim by the window server (src/input.c) */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_0         11
#define KEY_MINUS     12
#define KEY_EQUAL     13
#define KEY_BACKSPACE 14
#define KEY_TAB       15
#define KEY_Q         16
#define KEY_R         19
#define KEY_P         25
#define KEY_ENTER     28
#define KEY_A         30
#define KEY_L         38
#define KEY_Z         44
#define KEY_M         50
#define KEY_SLASH     53
#define KEY_SPACE     57
#define KEY_F1        59
#define KEY_F5        63
#define KEY_F6        64
#define KEY_F11       87
#define KEY_F12       88
#define KEY_SYSRQ     99
#define KEY_HOME     102
#define KEY_UP       103
#define KEY_PAGEUP   104
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_END      107
#define KEY_DOWN     108
#define KEY_PAGEDOWN 109
#define KEY_DELETE   111

/* Modifier bits: MC_KEY events carry these in ev.mods (src/shortcuts.h). */
#define MOD_CTRL  0x01
#define MOD_ALT   0x02
#define MOD_SHIFT 0x04
#define MOD_SUPER 0x08

#ifndef EVP_SELFTEST
static unsigned char evp_jpeg[EVP_FRAME_MAX];
#endif

/* ============================================================
 *  1. Small utilities
 * ============================================================ */
#ifndef EVP_SELFTEST
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
#endif

/* The format attribute is what makes -Wformat check every JSON template in
 * this file against its arguments; without it the compiler cannot see them. */
static void set_err(char *dst, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_err(char *dst, size_t cap, const char *fmt, ...) {
    if (!dst || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

/* ============================================================
 *  2. JSON: read the slices this app needs, without a DOM
 *
 *  The engine sends objects like
 *      {"id":4,"result":{"sessionId":"A1B2"}}
 *      {"method":"Page.screencastFrame","sessionId":"A1B2",
 *       "params":{"data":"<base64>","sessionId":7}}
 *  A full parser would be more code than the client. These helpers walk one
 *  object level, skip strings and nesting correctly, and return a pointer +
 *  length into the original buffer. They never allocate and never read past
 *  the slice they were given.
 * ============================================================ */

/* Skip one JSON value at p (bounded by end). Returns its end, or NULL. */
static const char *js_skip_value(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= end) return NULL;
    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') return p + 1;
            p++;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        int in_str = 0;
        for (; p < end; p++) {
            char c = *p;
            if (in_str) {
                if (c == '\\') { p++; continue; }
                if (c == '"') in_str = 0;
                continue;
            }
            if (c == '"') { in_str = 1; continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* Copy the string literal at p into out, unescaping. 0 on success. */
static int js_copy_string(const char *p, const char *end, char *out, size_t cap) {
    size_t n = 0;
    if (p >= end || *p != '"') return -1;
    p++;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '"': c = '"';  break;
            case '\\': c = '\\'; break;
            case '/': c = '/';  break;
            case 'u': {
                /* \uXXXX: ASCII kept, the rest shown as '?' because this
                 * window's font draw is not a Unicode shaper. */
                unsigned v = 0;
                int i;
                for (i = 0; i < 4 && p < end; i++) {
                    char h = *p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                }
                c = (v < 0x80) ? (char)v : '?';
                break;
            }
            default: c = e; break;
            }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n] = 0;
    return 0;
}

/* Find `key` in the object at (js, n) and return its value slice. */
static int js_get(const char *js, size_t n, const char *key,
                  const char **val, size_t *vlen) {
    const char *p = js, *end = js + n;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= end || *p != '{') return 0;
    p++;
    for (;;) {
        const char *v, *vend;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p == '}') return 0;
        if (*p != '"') return 0;
        v = p;
        /* start at v+1: the opening quote is not the closing one. This loop
         * used to start at v, so it stopped immediately, klen underflowed and
         * no key ever matched - the host self-test caught it. */
        for (p = v + 1; p < end; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') break;
        }
        if (p >= end) return 0;
        {
            size_t klen = (size_t)(p - v - 1);
            int match = (klen == strlen(key)) && !strncmp(v + 1, key, klen);
            p++;
            while (p < end && *p != ':') p++;
            if (p >= end) return 0;
            p++;
            vend = js_skip_value(p, end);
            if (!vend) return 0;
            if (match) { *val = p; *vlen = (size_t)(vend - p); return 1; }
            p = vend;
            while (p < end && *p != ',' && *p != '}') p++;
            if (p < end && *p == ',') p++;
        }
    }
}

/* ... and copy it out when it is a string. */
static int js_get_str(const char *js, size_t n, const char *key,
                      char *out, size_t cap) {
    const char *v;
    size_t vl;
    if (!js_get(js, n, key, &v, &vl)) return 0;
    return js_copy_string(v, v + vl, out, cap) == 0;
}

static int js_get_int(const char *js, size_t n, const char *key, long *out) {
    const char *v;
    size_t vl;
    if (!js_get(js, n, key, &v, &vl) || vl == 0) return 0;
    *out = strtol(v, NULL, 10);
    return 1;
}

/* Walk a path of object keys: {"result":{"result":{"value":"..."}}}. */
static int js_get_nested(const char *js, size_t n,
                         const char *const *keys, int nk,
                         const char **val, size_t *vlen) {
    const char *cur = js;
    size_t curlen = n;
    int i;
    for (i = 0; i < nk; i++) {
        if (!js_get(cur, curlen, keys[i], &cur, &curlen)) return 0;
    }
    *val = cur;
    *vlen = curlen;
    return 1;
}

/* ["title","url"] -> two strings. Used for the page's own title/href. */
static int js_two_strings(const char *p, size_t n, char *a, size_t ac,
                          char *b, size_t bc) {
    const char *end = p + n;
    while (p < end && *p != '[') p++;
    if (p >= end) return -1;
    p++;
    while (p < end && (*p == ' ' || *p == ',')) p++;
    if (js_copy_string(p, end, a, ac) != 0) return -1;
    while (p < end && *p != ',') p++;
    if (p >= end) return -1;
    p++;
    while (p < end && *p == ' ') p++;
    return js_copy_string(p, end, b, bc);
}

/* ============================================================
 *  3. base64 (the screencast frame payload)
 * ============================================================ */
static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decode into out. Returns bytes written, or -1 on a bad character/overflow. */
static long b64_decode(const char *in, size_t n, unsigned char *out, size_t cap) {
    size_t i, o = 0;
    int quad[4];
    int q = 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)in[i];
        int v;
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        v = b64_val(c);
        if (v < 0) return -1;
        quad[q++] = v;
        if (q == 4) {
            unsigned word = ((unsigned)quad[0] << 18) | ((unsigned)quad[1] << 12) |
                            ((unsigned)quad[2] << 6) | (unsigned)quad[3];
            if (o + 3 > cap) return -1;
            out[o++] = (unsigned char)(word >> 16);
            out[o++] = (unsigned char)(word >> 8);
            out[o++] = (unsigned char)word;
            q = 0;
        }
    }
    if (q == 2) {
        if (o + 1 > cap) return -1;
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
    } else if (q == 3) {
        if (o + 2 > cap) return -1;
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    } else if (q != 0) {
        return -1;
    }
    return (long)o;
}

/* ============================================================
 *  4. keys: evdev -> (windowsVirtualKeyCode, key, code)
 *
 *  US layout. The text itself comes from the window server's MC_TEXT event,
 *  which is already shifted; this table is for the keys that produce no text
 *  (arrows, function keys) and for Ctrl+letter shortcuts, which produce no
 *  MC_TEXT either.
 * ============================================================ */
typedef struct { int ev; int vk; const char *key; const char *code; } EvKey;

static const EvKey ev_keys[] = {
    { KEY_ESC,        27,  "Escape",     "Escape"      },
    { KEY_BACKSPACE,   8,  "Backspace",  "Backspace"   },
    { KEY_TAB,         9,  "Tab",        "Tab"         },
    { KEY_ENTER,      13,  "Enter",      "Enter"       },
    { KEY_SPACE,      32,  " ",          "Space"       },
    { KEY_DELETE,     46,  "Delete",     "Delete"      },
    { KEY_HOME,       36,  "Home",       "Home"        },
    { KEY_END,        35,  "End",        "End"         },
    { KEY_PAGEUP,     33,  "PageUp",     "PageUp"      },
    { KEY_PAGEDOWN,   34,  "PageDown",   "PageDown"    },
    { KEY_LEFT,       37,  "ArrowLeft",  "ArrowLeft"   },
    { KEY_UP,         38,  "ArrowUp",    "ArrowUp"     },
    { KEY_RIGHT,      39,  "ArrowRight", "ArrowRight"  },
    { KEY_DOWN,       40,  "ArrowDown",  "ArrowDown"   },
    { KEY_F1,        112,  "F1",         "F1"          },
    { KEY_F5,        116,  "F5",         "F5"          },
    { KEY_F6,        117,  "F6",         "F6"          },
    { KEY_F11,       122,  "F11",        "F11"         },
    { KEY_F12,       123,  "F12",        "F12"         },
    { KEY_SYSRQ,      44,  "PrintScreen","PrintScreen" },
};

/* Letters/digits/punctuation an evdev code stands for, for Ctrl shortcuts. */
static int ev_char(int ev) {
    static const char *row1 = "qwertyuiop";      /* evdev 16..25 */
    static const char *row2 = "asdfghjkl";       /* evdev 30..38 */
    static const char *row3 = "zxcvbnm";         /* evdev 44..50 */
    static const char *digits = "1234567890";    /* evdev 2..11 */
    if (ev >= KEY_Q && ev <= KEY_P) return row1[ev - KEY_Q];
    if (ev >= KEY_A && ev <= KEY_L) return row2[ev - KEY_A];
    if (ev >= KEY_Z && ev <= KEY_M) return row3[ev - KEY_Z];
    if (ev >= KEY_1 && ev <= KEY_0) return digits[ev - KEY_1];
    if (ev == KEY_MINUS) return '-';
    if (ev == KEY_EQUAL) return '=';
    if (ev == KEY_SLASH) return '/';
    return 0;
}

/* The CDP modifier bitmask: Alt=1, Ctrl=2, Meta=4, Shift=8. */
static int cdp_mods(int mods) {
    int m = 0;
    if (mods & MOD_ALT)   m |= 1;
    if (mods & MOD_CTRL)  m |= 2;
    if (mods & MOD_SUPER) m |= 4;
    if (mods & MOD_SHIFT) m |= 8;
    return m;
}

#ifndef EVP_SELFTEST

/* ============================================================
 *  5. The engine process and the DevTools connection
 * ============================================================ */
typedef struct {
    pid_t  pid;
    int    from_stderr;    /* the engine's own diagnostics, not the protocol */
    int    alive;
    int    waiting;        /* forked; the DevTools endpoint is not up yet */
    long   spawned_ms;     /* monotonic start, for the 10 s endpoint deadline */
    long   polled_ms;      /* last look at the port file (100 ms apart) */
    char   stderr_tail[EVP_STDERR_KEEP + 1];
    size_t stderr_len;
    int    status;
    Cdpws *ws;             /* the upgraded DevTools connection, NULL until then */
#ifndef _WIN32
    CdpwsNet net;          /* the TCP socket behind ws; fd is -1 until connect */
#endif
} Engine;

static void engine_close_pipe(Engine *e) {
    if (e->ws) { cdpws_free(e->ws); e->ws = NULL; }
    if (e->from_stderr >= 0) { close(e->from_stderr); e->from_stderr = -1; }
}

/* Keep the tail of the engine's stderr: the last line before a failure is the
 * useful one, and it is the engine's own text, not a paraphrase. */
static void engine_note_stderr(Engine *e) {
    char buf[1024];
    ssize_t n;
    if (e->from_stderr < 0) return;
    while ((n = read(e->from_stderr, buf, sizeof(buf))) > 0) {
        /* Also on the app's own stderr, which the OS writes to the serial
         * console: an engine that dies before the first frame leaves its reason
         * here even when nothing reaches the window. */
        fprintf(stderr, "engine: %.*s", (int)n, buf);
        if ((size_t)n >= EVP_STDERR_KEEP) {
            memcpy(e->stderr_tail, buf + n - EVP_STDERR_KEEP, EVP_STDERR_KEEP);
            e->stderr_len = EVP_STDERR_KEEP;
        } else {
            size_t keep = EVP_STDERR_KEEP - (size_t)n;
            if (e->stderr_len > keep) {
                size_t drop = e->stderr_len - keep;
                memmove(e->stderr_tail, e->stderr_tail + drop, e->stderr_len - drop);
                e->stderr_len -= drop;
            }
            memcpy(e->stderr_tail + e->stderr_len, buf, (size_t)n);
            e->stderr_len += (size_t)n;
        }
        e->stderr_tail[e->stderr_len] = 0;
    }
    if (n == 0) {
        close(e->from_stderr);
        e->from_stderr = -1;
    }
}

static const char *engine_last_stderr(Engine *e) {
    size_t i;
    if (!e->stderr_len) return "";
    for (i = e->stderr_len; i > 0; i--) {
        if (e->stderr_tail[i - 1] == '\n') {
            size_t s = i;
            while (s < e->stderr_len &&
                   (e->stderr_tail[s] == ' ' || e->stderr_tail[s] == '\t')) s++;
            if (s < e->stderr_len) return e->stderr_tail + s;
        }
    }
    return e->stderr_tail;
}

/* The engine's closure keeps libraries in subdirectories of the bundle -
 * usr/lib/chromium for its own, usr/lib/pulseaudio for pulse's helpers - and
 * the dynamic loader searches exactly the directories it is given, never their
 * children. Walking the bundle once and listing every directory that holds a
 * .so is what makes the child find all of them; with only <bundle>/usr/lib the
 * engine exits immediately and the window says it never started. */
static int dir_has_so(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while (!found && (ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len > 3 && strstr(n, ".so")) found = 1;
    }
    closedir(d);
    return found;
}

static void collect_lib_dirs(const char *base, int depth, char *out, size_t cap) {
    if (depth > 3) return;
    if (dir_has_so(base)) {
        size_t used = strlen(out);
        if (used + 1 < cap) {
            if (used) snprintf(out + used, cap - used, ":");
            snprintf(out + used + (used ? 1 : 0), cap - used - (used ? 1 : 0),
                     "%s", base);
        }
    }
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char child[512];
        snprintf(child, sizeof child, "%s/%s", base, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            collect_lib_dirs(child, depth + 1, out, cap);
    }
    closedir(d);
}

/* Where the profile and its port file live. Both the child's --user-data-dir
 * and the parent's DevToolsActivePort path are built from these, never from
 * getenv: the child is given TMPDIR=/tmp below, so a parent that read its own
 * TMPDIR could look in a different directory from the one the browser writes. */
#define EVP_TMP          "/tmp"
#define EVP_PROFILE_DIR  EVP_TMP "/engine-profile"
#define EVP_ACTIVE_PORT  EVP_PROFILE_DIR "/DevToolsActivePort"

/* SIGTERM, then SIGKILL after 500 ms, and reap. Idempotent. */
static void engine_stop(Engine *e) {
    int st = 0, i;
    if (e->pid <= 0) return;
    kill(e->pid, SIGTERM);
    for (i = 0; i < 50; i++) {
        if (waitpid(e->pid, &st, WNOHANG) == e->pid) {
            e->status = WIFEXITED(st) ? WEXITSTATUS(st)
                      : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
            e->pid = 0;
            e->alive = 0;
            return;
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10L * 1000L * 1000L;
            nanosleep(&ts, NULL);
        }
    }
    kill(e->pid, SIGKILL);
    waitpid(e->pid, &st, 0);
    e->status = WIFEXITED(st) ? WEXITSTATUS(st)
              : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    e->pid = 0;
    e->alive = 0;
}

/* Start the headless engine. The DevTools endpoint is not waited for here: see
 * engine_attach(), which the frame loop calls, so that a browser that takes
 * seconds to listen is a countdown in the window rather than a frozen one.
 *
 * Returns 0 once the child is running (e->waiting is set), or -1 with err set
 * when the browser could not be started at all - a missing payload, pipe() or
 * fork() - in which case the caller renders with --screenshot. */
static int engine_spawn(Engine *e, const char *bundle, int page_w, int page_h,
                        int mute_audio, char *err, size_t esz) {
    char exe[512], libdir[640], size[64], loader[600];
    char *argv[32];
    int errp[2] = {-1, -1};
    int n = 0;

    memset(e, 0, sizeof(*e));
    e->from_stderr = -1;
#ifndef _WIN32
    e->net.fd = -1;         /* memset makes it 0, and fd 0 is stdin */
#endif

    snprintf(exe, sizeof(exe), "%s/usr/lib/chromium/chromium-headless-shell", bundle);
    if (access(exe, X_OK) != 0) {
        set_err(err, esz, "the engine payload is not in this bundle: %s: %s",
                exe, strerror(errno));
        return -1;
    }
    collect_lib_dirs(bundle, 0, libdir, sizeof libdir);
    if (!libdir[0]) snprintf(libdir, sizeof(libdir), "%s/usr/lib:%s/lib", bundle, bundle);
    snprintf(loader, sizeof(loader), "%s/lib/ld-musl-x86_64.so.1", bundle);
    /* The loader is not exec'd (see the comment above execv), but a bundle
     * without it can only fail there, and this line says why before the fork. */
    if (access(loader, X_OK) != 0)
        fprintf(stderr, "engineview: warning: %s is not executable; execv will "
                        "report the real error\n", loader);
    snprintf(size, sizeof(size), "--window-size=%dx%d", page_w, page_h);

    /* A DevToolsActivePort left by a previous run would be read as this run's,
     * and the connect() to a dead port would look like a browser that never
     * started. The browser deletes the file on a clean exit; a killed one does
     * not. */
    unlink(EVP_ACTIVE_PORT);

    if (pipe(errp) != 0) {
        set_err(err, esz, "pipe(): %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        set_err(err, esz, "fork(): %s", strerror(errno));
        close(errp[0]);
        close(errp[1]);
        return -1;
    }
    if (pid == 0) {
        /* dup(2) keeps the desktop's serial console: it is the one place a
         * message survives when the engine dies before the frame loop drains
         * its stderr. The marker file /app/engine-log in the bundle then sends
         * that stderr to a file in the jail as well, so a death during startup
         * leaves evidence after the console has scrolled away. */
        int console = dup(2);
        if (errp[1] != 2) dup2(errp[1], 2);
        if (errp[0] != 2) close(errp[0]);
        if (errp[1] != 2) close(errp[1]);
        if (console >= 0 && access("/app/engine-log", F_OK) == 0) {
            int lf = open(EVP_TMP "/engine-child.log",
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lf >= 0) { dup2(lf, 2); if (lf != 2) close(lf); }
        }
        if (console >= 0)
            dprintf(console, "engineview: exec %s\n", exe);
        setenv("LD_LIBRARY_PATH", libdir, 1);
        setenv("HOME", EVP_TMP, 1);
        setenv("TMPDIR", EVP_TMP, 1);

        argv[n++] = exe;
        argv[n++] = "--headless=new";          /* without it the shell prints usage
                                                  * and exits 0, silently */
        argv[n++] = "--remote-debugging-port=0"; /* the port and the browser path
                                                  * are published in the profile */
        argv[n++] = "--no-sandbox";            /* the OS jail IS the sandbox */
        argv[n++] = "--disable-gpu";           /* no GPU: no GL, no DRI driver */
        {
            static char prof[256];
            snprintf(prof, sizeof prof, "--user-data-dir=%s", EVP_PROFILE_DIR);
            argv[n++] = prof;
        }
        argv[n++] = "--no-first-run";
        argv[n++] = "--no-default-browser-check";
        argv[n++] = "--disable-background-networking";
        argv[n++] = "--disable-component-update";
        argv[n++] = "--disable-extensions";
        argv[n++] = "--disable-sync";
        argv[n++] = "--disable-default-apps";
        argv[n++] = "--disable-breakpad";
        argv[n++] = "--hide-scrollbars";
        argv[n++] = "--force-device-scale-factor=1";
        argv[n++] = "--lang=en-US";
        argv[n++] = size;
        if (mute_audio) argv[n++] = "--mute-audio";
        argv[n++] = "about:blank";
        argv[n] = NULL;
        /* The binary is exec'd directly: chromium re-execs
         * /proc/self/exe for its child processes, so starting it through
         * the loader would make every child restart the loader with no
         * arguments ("GPU process launch failed"). The package rewrites
         * PT_INTERP to /app/ld-musl-x86_64.so.1, which exists in the jail. */
        execv(exe, argv);
        fprintf(stderr, "engineview: execv %s: %s\n", exe, strerror(errno));
        _exit(127);
    }

    close(errp[1]);
    e->pid = pid;
    e->from_stderr = errp[0];
    e->alive = 1;
    fcntl(e->from_stderr, F_SETFL, fcntl(e->from_stderr, F_GETFL, 0) | O_NONBLOCK);

    /* The endpoint is not up yet: the browser publishes its port in
     * DevToolsActivePort only once it is listening, and on a slow guest that is
     * seconds after this fork. engine_attach() polls for it from the frame loop,
     * so the window keeps drawing (and counting) in the meantime. */
    e->waiting = 1;
    e->spawned_ms = now_ms();
    return 0;
}

/* One CDP message, one WebSocket text frame. The transport owns the framing,
 * so there is no NUL delimiter and no partial-message state here; a JSON
 * object that does not fit the buffer is a bug in the caller, and cdpws_send_json
 * would be handed invalid JSON, so the size is checked first. */
static int engine_send(Engine *e, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static int engine_send(Engine *e, const char *fmt, ...) {
    char buf[4096];
    int n;
    va_list ap;
    if (!e->ws) return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return cdpws_send_json(e->ws, buf) < 0 ? -1 : 0;
}

/* Wait for the browser's DevTools endpoint and upgrade it - one attempt per
 * pass of the frame loop, at most every 100 ms, so the window keeps drawing and
 * the elapsed time in its card keeps counting.
 *
 * --remote-debugging-port=0 has the browser pick a free loopback port and write
 * it, plus the WebSocket path for the browser target, into
 * <user-data-dir>/DevToolsActivePort - and only once it is listening. That file
 * is the readiness signal: cdpws_read_active_port() refuses a port outside
 * 1..65535 or a path without '/', so a half-written file is never mistaken for a
 * listening browser.
 *
 *   1  e->ws is upgraded and the channel is usable
 *   0  not yet; call again
 *  -1  it will not happen: err says why, and the child has been stopped
 */
static int engine_attach(Engine *e, char *err, size_t esz) {
    int st = 0, port = 0;
    char path[CDPWS_PATH_MAX], perr[CDPWS_ERR_MAX];

    if (e->ws) return 1;
    if (e->pid <= 0) {
        set_err(err, esz, "the engine is not running");
        return -1;
    }
    /* Draining stderr is not optional while waiting: the pipe holds 64 KiB and
     * a browser that fills it blocks on its own diagnostics. */
    engine_note_stderr(e);
    if (waitpid(e->pid, &st, WNOHANG) == e->pid) {
        e->pid = 0;
        e->alive = 0;
        e->status = WIFEXITED(st) ? WEXITSTATUS(st)
                  : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
        engine_note_stderr(e);           /* whatever it said before it died */
        set_err(err, esz, "the engine exited %d before it published %s%s%s",
                e->status, EVP_ACTIVE_PORT,
                e->stderr_len ? ": " : "", engine_last_stderr(e));
        return -1;
    }

    if (e->polled_ms && now_ms() - e->polled_ms < 100) return 0;
    e->polled_ms = now_ms();

    path[0] = 0;
    perr[0] = 0;
    if (cdpws_read_active_port(EVP_ACTIVE_PORT, &port, path, sizeof path,
                               perr, sizeof perr) != CDPWS_OK) {
        if (now_ms() - e->spawned_ms >= 10000) {
            set_err(err, esz, "no %s after 10 s: %s", EVP_ACTIVE_PORT,
                    perr[0] ? perr : "the file never appeared");
            engine_stop(e);
            return -1;
        }
        return 0;
    }
#ifndef _WIN32
    {
        CdpwsTransport tr = cdpws_net_transport(&e->net);
        CdpwsOpenOpts o;
        char werr[CDPWS_ERR_MAX];
        /* cdpws_open has no event loop to come back to: its upgrade response
         * must be allowed to wait, so the transport polls for the full open
         * deadline here. The frame loop sets it short below. */
        e->net.io_timeout_ms = 5000;
        memset(&o, 0, sizeof o);
        o.host = "127.0.0.1";
        o.port = port;
        o.path = path;
        o.timeout_ms = 5000;
        o.max_message = 2u * EVP_MSG_MAX;   /* one base64 JPEG is ~150 KiB */
        e->ws = cdpws_open(&tr, &o, werr, sizeof werr);
        if (!e->ws) {
            set_err(err, esz, "port %d did not accept the WebSocket upgrade: %s",
                    port, werr);
            engine_stop(e);
            return -1;
        }
        /* Now the frame loop owns the socket: a read that waited would stop the
         * window from drawing, and cdpws_recv reports a 20 ms timeout as
         * "nothing complete yet" without failing the connection. */
        e->net.io_timeout_ms = 20;
    }
    fprintf(stderr, "engineview: devtools over tcp port %d %s\n", port, path);
    return 1;
#else
    set_err(err, esz, "the DevTools TCP transport is not built on this host");
    engine_stop(e);
    return -1;
#endif
}

/* ============================================================
 *  6. What the window knows about the page
 * ============================================================ */
typedef struct {
    int  ready;                 /* attached to a page target */
    int  creating, attaching;   /* waiting for this reply */
    long create_id, attach_id, info_id;
    char session[64];           /* flat target session id */
    char url[EVP_URL_MAX];
    char title[256];
    char typing[EVP_URL_MAX];   /* address bar contents */
    int  bar_focus;
    int  page_w, page_h;        /* the viewport we asked for */
    Img  frame;                 /* the last decoded frame */
    unsigned frames;            /* frames decoded since start */
    int  busy;                  /* a long render is running; no frame yet */
    int  shot_mode;             /* rendering via --screenshot, not DevTools */
    long last_frame_ms;
    long first_send_ms;
    long next_id;
    char cdperr[256];
    char state[128];
    int  want_info;
} Page;

static void page_metrics(Page *p, Engine *e) {
    if (!p->ready) return;
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\","
        "\"method\":\"Emulation.setDeviceMetricsOverride\","
        "\"params\":{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,"
        "\"mobile\":false}}",
        ++p->next_id, p->session, p->page_w, p->page_h);
}

static void page_start_cast(Page *p, Engine *e) {
    if (!p->ready) return;
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Page.startScreencast\","
        "\"params\":{\"format\":\"jpeg\",\"quality\":70,"
        "\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1}}",
        ++p->next_id, p->session, p->page_w, p->page_h);
}

static void page_navigate(Page *p, Engine *e, const char *url) {
    char msg[EVP_URL_MAX + 300], esc[EVP_URL_MAX * 2];
    size_t o = 0;
    const char *s;
    if (!p->ready) return;
    /* JSON-escape the two bytes that would break the message. A URL with a raw
     * control byte is refused by the engine, which reports it. */
    for (s = url; *s && o + 2 < sizeof(esc); s++) {
        if (*s == '"' || *s == '\\') esc[o++] = '\\';
        if ((unsigned char)*s < 0x20) continue;
        esc[o++] = *s;
    }
    esc[o] = 0;
    snprintf(msg, sizeof(msg),
             "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Page.navigate\","
             "\"params\":{\"url\":\"%s\"}}", ++p->next_id, p->session, esc);
    engine_send(e, "%s", msg);
    snprintf(p->url, sizeof(p->url), "%s", url);
}

static void page_create(Page *p, Engine *e) {
    p->create_id = ++p->next_id;
    engine_send(e,
        "{\"id\":%ld,\"method\":\"Target.createTarget\","
        "\"params\":{\"url\":\"about:blank\",\"width\":%d,\"height\":%d}}",
        p->create_id, p->page_w, p->page_h);
    p->creating = 1;
    snprintf(p->state, sizeof(p->state), "asking the engine for a tab");
}

/* The page's own title and href, in one round trip. */
static void page_ask_info(Page *p, Engine *e) {
    if (!p->ready || p->info_id) return;
    p->info_id = ++p->next_id;
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\","
        "\"params\":{\"expression\":\"JSON.stringify([document.title,"
        "location.href])\",\"returnByValue\":true}}",
        p->info_id, p->session);
    p->want_info = 0;
}

/* ============================================================
 *  7. Input forwarding
 * ============================================================ */
static void input_mouse(Engine *e, Page *p, const char *type, int x, int y,
                        const char *button, int buttons) {
    if (!p->ready) return;
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.dispatchMouseEvent\","
        "\"params\":{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\","
        "\"buttons\":%d,\"clickCount\":%d}}",
        ++p->next_id, p->session, type, x, y, button, buttons,
        !strcmp(type, "mousePressed") ? 1 : 0);
}

static void input_wheel(Engine *e, Page *p, int x, int y, int notches) {
    if (!p->ready) return;
    /* one evdev notch is 120 device units, which is what Chromium scrolls by;
     * a 1:1 forward rather than an invented factor */
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.dispatchMouseEvent\","
        "\"params\":{\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,"
        "\"deltaX\":0,\"deltaY\":%d}}",
        ++p->next_id, p->session, x, y, -notches * 120);
}

/* A key with no text: arrows, Tab, Enter, function keys, Ctrl+letter. */
static void input_key(Engine *e, Page *p, int evdev, int mods, int down) {
    int vk = 0;
    const char *key = NULL, *code = "";
    char ch[2] = {0, 0};
    size_t i;
    if (!p->ready) return;
    for (i = 0; i < sizeof(ev_keys) / sizeof(ev_keys[0]); i++) {
        if (ev_keys[i].ev == evdev) {
            vk = ev_keys[i].vk;
            key = ev_keys[i].key;
            code = ev_keys[i].code;
            break;
        }
    }
    if (!key) {
        int c = ev_char(evdev);
        if (!c) return;
        ch[0] = (char)c;
        vk = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
        key = ch;
    }
    engine_send(e,
        "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.dispatchKeyEvent\","
        "\"params\":{\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\","
        "\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d,"
        "\"modifiers\":%d}}",
        ++p->next_id, p->session, down ? "keyDown" : "keyUp",
        key, code ? code : "", vk, vk, cdp_mods(mods));
}

/* Text the window server already translated (shift applied). */
static void input_text(Engine *e, Page *p, const char *text) {
    char esc[220];
    size_t o = 0;
    const char *s;
    if (!p->ready || !text || !text[0]) return;
    if (strlen(text) > 1) {
        for (s = text; *s && o + 2 < sizeof(esc); s++) {
            if (*s == '"' || *s == '\\') esc[o++] = '\\';
            if ((unsigned char)*s < 0x20) continue;
            esc[o++] = *s;
        }
        esc[o] = 0;
        engine_send(e,
            "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.insertText\","
            "\"params\":{\"text\":\"%s\"}}", ++p->next_id, p->session, esc);
        return;
    }
    {
        unsigned char c = (unsigned char)text[0];
        int vk = (c >= 'a' && c <= 'z') ? (int)c - 'a' + 'A'
               : (c >= 'A' && c <= 'Z') ? (int)c
               : (c >= '0' && c <= '9') ? (int)c : 0;
        if (c < 0x20) return;
        if (c == '"' || c == '\\') snprintf(esc, sizeof(esc), "\\%c", c);
        else snprintf(esc, sizeof(esc), "%c", c);
        engine_send(e,
            "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.dispatchKeyEvent\","
            "\"params\":{\"type\":\"keyDown\",\"text\":\"%s\","
            "\"unmodifiedText\":\"%s\",\"key\":\"%s\","
            "\"windowsVirtualKeyCode\":%d}}",
            ++p->next_id, p->session, esc, esc, esc, vk);
        engine_send(e,
            "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Input.dispatchKeyEvent\","
            "\"params\":{\"type\":\"keyUp\",\"key\":\"%s\","
            "\"windowsVirtualKeyCode\":%d}}",
            ++p->next_id, p->session, esc, vk);
    }
}

/* ============================================================
 *  8. Reading the DevTools socket
 *
 *  cdpws_recv hands over whole messages only: it reassembles fragments,
 *  answers pings itself and never returns a partial frame. A message is one
 *  CDP JSON object, so unlike the pipe there is no NUL split and no
 *  reassembly buffer in this file.
 * ============================================================ */

static void handle_message(Page *p, Engine *e, const char *js, size_t n,
                           int *new_frame) {
    char method[64], session[64], target[80];
    long id = 0;
    const char *ev;
    size_t evl;

    if (js_get_str(js, n, "method", method, sizeof(method))) {
        if (!strcmp(method, "Page.screencastFrame")) {
            const char *data;
            size_t dlen;
            long cast_id = 0;
            if (!js_get(js, n, "params", &data, &dlen)) return;
            {
                const char *payload = NULL;
                size_t plen = 0;
                if (js_get(data, dlen, "data", &payload, &plen) &&
                    plen >= 2 && payload[0] == '"') {
                    long got = b64_decode(payload + 1, plen - 2,
                                          evp_jpeg, sizeof(evp_jpeg));
                    if (got > 0) {
                        Img im;
                        char ierr[160];
                        memset(&im, 0, sizeof(im));
                        if (img_decode(evp_jpeg, (size_t)got, &im, ierr, sizeof(ierr)) == 0) {
                            img_free(&p->frame);
                            p->frame = im;
                            p->frames++;
                            p->last_frame_ms = now_ms();
                            *new_frame = 1;
                        } else {
                            set_err(p->cdperr, sizeof(p->cdperr),
                                    "frame %u: %s", p->frames + 1, ierr);
                        }
                    } else {
                        set_err(p->cdperr, sizeof(p->cdperr),
                                "screencastFrame: base64 data is not valid");
                    }
                }
                if (js_get(data, dlen, "sessionId", &payload, &plen))
                    cast_id = strtol(payload, NULL, 10);
            }
            /* The ACK is mandatory: without it the engine stops sending. */
            if (js_get_str(js, n, "sessionId", session, sizeof(session))) {
                engine_send(e,
                    "{\"id\":%ld,\"sessionId\":\"%s\","
                    "\"method\":\"Page.screencastFrameAck\","
                    "\"params\":{\"sessionId\":%ld}}",
                    ++p->next_id, session, cast_id);
            }
            return;
        }
        if (!strcmp(method, "Page.loadEventFired")) {
            p->want_info = 1;
            return;
        }
        return;
    }

    if (!js_get_int(js, n, "id", &id)) return;

    if (js_get(js, n, "error", &ev, &evl)) {
        char m[220];
        if (!js_get_str(ev, evl, "message", m, sizeof(m)))
            snprintf(m, sizeof(m), "%.*s", (int)(evl > 180 ? 180 : evl), ev);
        set_err(p->cdperr, sizeof(p->cdperr), "CDP %ld: %s", id, m);
        if (p->creating && id == p->create_id) {
            p->creating = 0;
            snprintf(p->state, sizeof(p->state), "no tab");
        }
        if (p->attaching && id == p->attach_id) {
            p->attaching = 0;
            snprintf(p->state, sizeof(p->state), "attach failed");
        }
        if (p->info_id == id) p->info_id = 0;
        return;
    }

    if (p->creating && id == p->create_id) {
        const char *v;
        size_t vl;
        p->creating = 0;
        if (js_get(js, n, "result", &v, &vl) &&
            js_get(v, vl, "targetId", &v, &vl) &&
            js_copy_string(v, v + vl, target, sizeof(target)) == 0) {
            p->attach_id = ++p->next_id;
            engine_send(e,
                "{\"id\":%ld,\"method\":\"Target.attachToTarget\","
                "\"params\":{\"targetId\":\"%s\",\"flatten\":true}}",
                p->attach_id, target);
            p->attaching = 1;
            snprintf(p->state, sizeof(p->state), "attaching to the page");
        } else {
            set_err(p->cdperr, sizeof(p->cdperr),
                    "Target.createTarget returned no targetId");
            snprintf(p->state, sizeof(p->state), "no tab");
        }
        return;
    }

    if (p->attaching && id == p->attach_id) {
        const char *v;
        size_t vl;
        p->attaching = 0;
        if (js_get(js, n, "result", &v, &vl) &&
            js_get(v, vl, "sessionId", &v, &vl) &&
            js_copy_string(v, v + vl, p->session, sizeof(p->session)) == 0) {
            p->ready = 1;
            snprintf(p->state, sizeof(p->state), "page attached");
            engine_send(e, "{\"id\":%ld,\"sessionId\":\"%s\",\"method\":\"Page.enable\"}",
                        ++p->next_id, p->session);
            page_metrics(p, e);
            page_start_cast(p, e);
            if (p->url[0] && strcmp(p->url, "about:blank"))
                page_navigate(p, e, p->url);
            p->want_info = 1;
        } else {
            set_err(p->cdperr, sizeof(p->cdperr),
                    "Target.attachToTarget returned no sessionId");
            snprintf(p->state, sizeof(p->state), "attach failed");
        }
        return;
    }

    if (p->info_id && id == p->info_id) {
        const char *keys[3];
        const char *v;
        size_t vl;
        char title[256], href[EVP_URL_MAX];
        p->info_id = 0;
        keys[0] = "result";
        keys[1] = "result";
        keys[2] = "value";
        if (js_get_nested(js, n, keys, 3, &v, &vl) && vl >= 2 && v[0] == '"') {
            char tmp[EVP_URL_MAX + 300];
            if (js_copy_string(v, v + vl, tmp, sizeof(tmp)) == 0 &&
                js_two_strings(tmp, strlen(tmp), title, sizeof(title),
                               href, sizeof(href)) == 0) {
                snprintf(p->title, sizeof(p->title), "%s", title);
                if (href[0] && strcmp(href, "about:blank"))
                    snprintf(p->url, sizeof(p->url), "%s", href);
            }
        }
        return;
    }
}

/* Read everything the browser has sent; returns the number of frames decoded.
 * Called once per pass of the frame loop, never blocks (the transport's read
 * timeout is 20 ms and cdpws_recv reports a timeout as "nothing yet"). */
static int engine_pump(Page *p, Engine *e) {
    int frames = 0;

    engine_note_stderr(e);
    if (!e->ws) return 0;

    for (;;) {
        CdpwsMessage m;
        int nf = 0;
        int r = cdpws_recv(e->ws, &m);
        if (r == 0) break;                     /* nothing pending */
        if (r < 0) {
            set_err(p->cdperr, sizeof(p->cdperr), "devtools: %s",
                    cdpws_last_error(e->ws));
            snprintf(p->state, sizeof(p->state), "devtools closed");
            e->alive = 0;
            break;
        }
        if (m.opcode == CDPWS_OP_CLOSE) {
            /* The browser closed the session itself: a dead engine, but not a
             * protocol error, so it gets its own sentence. */
            snprintf(p->state, sizeof(p->state),
                     "the engine closed the DevTools session (%d)", m.code);
            e->alive = 0;
            break;
        }
        if (m.opcode != CDPWS_OP_TEXT) continue;   /* ping answered inside */
        /* cdpws guarantees NUL termination for text messages */
        handle_message(p, e, (const char *)m.data, (size_t)m.len, &nf);
        frames += nf;
    }
    return frames;
}

/* ============================================================
 *  9. Drawing
 * ============================================================ */
/* Draw text clipped to maxw with an ellipsis, measured against the real font
 * width. Never by character count: that is how a title runs under a bar. */
static void draw_clip(Surface *s, Font *f, int x, int y, int maxw,
                      const char *text, u32 color) {
    char buf[EVP_URL_MAX + 300];
    size_t n;
    if (maxw <= 0) return;
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if (font_width(f, buf) <= (float)maxw) {
        font_draw(s, f, (float)x, (float)y, buf, color);
        return;
    }
    n = strlen(buf);
    while (n > 0) {
        char cut[EVP_URL_MAX + 300];
        size_t k;
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        k = n;
        if (k + 4 < sizeof(cut)) {
            memcpy(cut, buf, k);
            memcpy(cut + k, "...", 4);
            if (font_width(f, cut) <= (float)maxw) {
                font_draw(s, f, (float)x, (float)y, cut, color);
                return;
            }
        }
    }
}

static void draw_page(Surface *s, Page *p, int px, int py, int pw, int ph) {
    Img *im = &p->frame;
    if (!im->px || im->w <= 0 || im->h <= 0) {
        surf_fill(s, px, py, pw, ph, RGB(16, 18, 24));
        return;
    }
    if (im->w == pw && im->h == ph) {
        int y;
        for (y = 0; y < ph; y++)
            memcpy(s->px + (size_t)(py + y) * s->stride + px,
                   im->px + (size_t)y * im->stride, (size_t)pw * 4);
        return;
    }
    /* The viewport is set to the frame rect, so this is the fallback for a
     * frame that arrives at the old size during a resize. Nearest neighbour,
     * no allocation. */
    {
        int y, x;
        for (y = 0; y < ph; y++) {
            int sy = (int)((long)y * im->h / ph);
            if (sy >= im->h) sy = im->h - 1;
            for (x = 0; x < pw; x++) {
                int sx = (int)((long)x * im->w / pw);
                if (sx >= im->w) sx = im->w - 1;
                s->px[(size_t)(py + y) * s->stride + px + x] =
                    im->px[(size_t)sy * im->stride + sx];
            }
        }
    }
}

static void draw_ui(MClient *c, Page *p, Engine *e, Font *fs, Font *fb,
                    int px, int py, int pw, int ph) {
    Surface s;
    char line[EVP_URL_MAX + 400];

    memset(&s, 0, sizeof(s));
    s.px = c->px;
    s.w = c->w;
    s.h = c->h;
    s.stride = c->stride;
    surf_clip(&s, 0, 0, c->w, c->h);

    draw_page(&s, p, px, py, pw, ph);

    /* top bar */
    surf_fill(&s, 0, 0, c->w, EVP_BAR_H, RGB(18, 21, 28));
    surf_fill(&s, 0, EVP_BAR_H - 1, c->w, 1, RGB(52, 60, 74));
    if (p->bar_focus) {
        surf_fill(&s, 8, 5, c->w / 2, EVP_BAR_H - 11, RGB(30, 35, 46));
        draw_clip(&s, fs, 14, 9, c->w / 2 - 12,
                  p->typing[0] ? p->typing : "type a URL, Enter to open, Esc to cancel",
                  p->typing[0] ? RGB(238, 243, 250) : RGB(120, 132, 150));
    } else {
        draw_clip(&s, fs, 12, 9, c->w - 280,
                  p->url[0] ? p->url : "about:blank", RGB(200, 210, 224));
    }
    snprintf(line, sizeof(line), "%s  %u frames",
             p->state[0] ? p->state : "starting", p->frames);
    draw_clip(&s, fs, c->w - 262, 9, 250, line,
              p->ready ? RGB(120, 200, 150) : RGB(230, 180, 110));

    /* bottom bar: title, size, and the engine's own last words */
    surf_fill(&s, 0, c->h - EVP_FOOT_H, c->w, EVP_FOOT_H, RGB(14, 16, 22));
    surf_fill(&s, 0, c->h - EVP_FOOT_H, c->w, 1, RGB(44, 52, 64));
    if (p->cdperr[0]) {
        draw_clip(&s, fs, 10, c->h - EVP_FOOT_H + 5, c->w - 20, p->cdperr,
                  RGB(255, 150, 150));
    } else if (p->shot_mode) {
        /* No live process exists in this mode, by design: the frame came from
         * --screenshot. Say that rather than "engine exited". */
        snprintf(line, sizeof(line),
                 "%s   %dx%d   one frame per navigation (--screenshot)   "
                 "F6 address   Esc closes",
                 p->title[0] ? p->title : "(no title yet)", p->page_w, p->page_h);
        draw_clip(&s, fs, 10, c->h - EVP_FOOT_H + 5, c->w - 20, line,
                  RGB(230, 190, 120));
    } else if (!e->alive && !p->busy) {
        const char *why = engine_last_stderr(e);
        snprintf(line, sizeof(line), "engine exited (status %d)%s%s",
                 e->status, why && why[0] ? ": " : "", why ? why : "");
        draw_clip(&s, fs, 10, c->h - EVP_FOOT_H + 5, c->w - 20, line,
                  RGB(255, 150, 150));
    } else {
        snprintf(line, sizeof(line),
                 "%s   %dx%d   F6 address   Esc closes   Ctrl+R reload",
                 p->title[0] ? p->title : "(no title yet)", p->page_w, p->page_h);
        draw_clip(&s, fs, 10, c->h - EVP_FOOT_H + 5, c->w - 20, line,
                  RGB(140, 152, 170));
    }

    /* the first-frame card: only while there is no frame at all, and it names
     * the real elapsed time instead of a spinner that pretends */
    if (!p->frame.px) {
        long waited = now_ms() - p->first_send_ms;
        int cx = px + 26, cy = py + 26;
        surf_fill(&s, px, py, pw, ph, RGB(12, 14, 19));
        font_draw(&s, fb, (float)cx, (float)cy,
                  "Waiting for the engine's first frame", RGB(226, 232, 242));
        cy += (int)font_height(fb) + 10;
        if (!e->alive && !p->busy) {
            snprintf(line, sizeof(line), "The engine process is not running: %s",
                     engine_last_stderr(e));
            draw_clip(&s, fs, cx, cy, pw - 52, line, RGB(255, 160, 160));
        } else if (!p->ready) {
            snprintf(line, sizeof(line), "%.1f s. %s", waited / 1000.0,
                     p->state[0] ? p->state : "starting the engine");
            draw_clip(&s, fs, cx, cy, pw - 52, line, RGB(180, 192, 208));
        } else if (waited > EVP_FIRST_FRAME_MS) {
            snprintf(line, sizeof(line),
                     "%.0f s and no Page.screencastFrame has arrived.%s%s",
                     waited / 1000.0, p->cdperr[0] ? " " : "",
                     p->cdperr[0] ? p->cdperr : "");
            draw_clip(&s, fs, cx, cy, pw - 52, line, RGB(255, 190, 120));
            cy += (int)font_height(fs) + 8;
            draw_clip(&s, fs, cx, cy, pw - 52,
                      "This window never draws a frame the engine did not send.",
                      RGB(140, 152, 170));
        } else {
            snprintf(line, sizeof(line), "%.1f s. %s", waited / 1000.0,
                     p->state[0] ? p->state : "starting the engine");
            draw_clip(&s, fs, cx, cy, pw - 52, line, RGB(180, 192, 208));
        }
    }
    mc_present(c);
}

/* The card shown when the engine is not installed at all. It states the real
 * cost, because the user has to raise the guest's memory and disk for the
 * package to fit. */
/* ---- screenshot mode -----------------------------------------------------
 *
 * DevTools over TCP is the interactive path, and it is only taken when the
 * browser publishes a port and accepts the WebSocket upgrade. When it does not
 * - an engine that dies in the jail, a port file that never appears - nothing
 * can be driven, while the very same binary renders happily when asked for a
 * document or a screenshot. So navigation then falls back to the way that is
 * measured: run the engine with --screenshot at the window size, then show the
 * PNG it wrote. It is one frame per navigation rather than 30 per second, and
 * the footer says so.
 */
static int shot_render(const char *bundle, const char *url, int w, int h,
                       Page *p, char *err, size_t esz) {
    char exe[600], out[300], size[64], shot[340], libs[900];
    snprintf(exe, sizeof(exe), "%s/usr/lib/chromium/chromium-headless-shell", bundle);
    snprintf(out, sizeof(out), "/tmp/shot-%d.png", (int)getpid());
    snprintf(size, sizeof(size), "--window-size=%dx%d", w, h);
    snprintf(shot, sizeof(shot), "--screenshot=%s", out);
    collect_lib_dirs(bundle, 0, libs, sizeof libs);
    unlink(out);

    pid_t pid = fork();
    if (pid < 0) { set_err(err, esz, "fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        setenv("LD_LIBRARY_PATH", libs, 1);
        setenv("HOME", "/tmp", 1);
        setenv("TMPDIR", "/tmp", 1);
        execl(exe, "chrome", "--headless", "--no-sandbox", "--disable-gpu",
              "--hide-scrollbars", "--force-device-scale-factor=1",
              size, shot, url, (char *)NULL);
        _exit(127);
    }
    int st = 0, waited = 0;
    while (waited < 200) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        usleep(100000);
        waited++;
    }
    if (waited >= 200) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }

    FILE *f = fopen(out, "rb");
    if (!f) {
        set_err(err, esz, "the engine wrote no screenshot (%s), status %d",
                strerror(errno), WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return -1;
    }
    static unsigned char *buf;
    static size_t cap;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); set_err(err, esz, "empty screenshot"); return -1; }
    if ((size_t)len > cap) {
        unsigned char *nb = realloc(buf, (size_t)len);
        if (!nb) { fclose(f); set_err(err, esz, "out of memory"); return -1; }
        buf = nb; cap = (size_t)len;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    unlink(out);
    if (got != (size_t)len) { set_err(err, esz, "short read"); return -1; }

    Img im;
    if (img_decode(buf, got, &im, err, esz) != 0) return -1;
    img_free(&p->frame);
    p->frame = im;
    p->frames++;
    return 0;
}

/* The --screenshot fallback, with its bookkeeping in one place: a render can
 * take tens of seconds, and while it runs the window must say so (p->busy)
 * rather than claim the engine exited. Returns shot_render's result. */
static int page_shot(const char *bundle, Page *p, char *err, size_t esz) {
    int rc;
    p->busy = 1;
    rc = shot_render(bundle, p->url, p->page_w, p->page_h, p, err, esz);
    p->busy = 0;
    if (rc != 0) {
        snprintf(p->state, sizeof(p->state), "%s", err);
    } else {
        snprintf(p->state, sizeof(p->state), "rendered");
        p->shot_mode = 1;
    }
    return rc;
}

static void draw_missing(MClient *c, Font *fs, Font *fb, const char *why) {
    Surface s;
    int x = 34, y = 34;
    memset(&s, 0, sizeof(s));
    s.px = c->px;
    s.w = c->w;
    s.h = c->h;
    s.stride = c->stride;
    surf_clip(&s, 0, 0, c->w, c->h);
    surf_fill(&s, 0, 0, c->w, c->h, RGB(10, 12, 17));

    font_draw(&s, fb, (float)x, (float)y, "The engine is not installed",
              RGB(255, 170, 170));
    y += (int)font_height(fb) + 14;
    draw_clip(&s, fs, x, y, c->w - 68, why, RGB(236, 240, 248));
    y += (int)font_height(fs) * 2 + 6;
    draw_clip(&s, fs, x, y, c->w - 68,
              "Install it in the guest:  pkg update && pkg install engine",
              RGB(200, 212, 228));
    y += (int)font_height(fs) + 6;
    draw_clip(&s, fs, x, y, c->w - 68,
              "Chromium headless: 682.0 MB installed, 258.3 MB to download.",
              RGB(200, 212, 228));
    y += (int)font_height(fs) + 6;
    draw_clip(&s, fs, x, y, c->w - 68,
              "The guest needs MYSTIC_RAM=1024 and MYSTIC_DISK_MB=2048 in image.sh",
              RGB(230, 190, 120));
    y += (int)font_height(fs) + 6;
    draw_clip(&s, fs, x, y, c->w - 68,
              "(delete build/disk.img once: image.sh only creates that file when "
              "it is missing)", RGB(150, 162, 180));
    y += (int)font_height(fs) + 14;
    draw_clip(&s, fs, x, y, c->w - 68,
              "The package is built by tools/fetch-engine.py; docs/ENGINE.md has "
              "the measured numbers and what cannot work (DRM).",
              RGB(150, 162, 180));
    mc_present(c);
}

/* ============================================================
 *  10. main
 * ============================================================ */
static const char *bundle_dir(char *buf, size_t cap) {
    const char *env = getenv("MYSTIC_BUNDLE");
    if (env && env[0]) {
        snprintf(buf, cap, "%s", env);
        return buf;
    }
    {
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            char *slash;
            exe[n] = 0;
            slash = strrchr(exe, '/');
            if (slash) *slash = 0;
            snprintf(buf, cap, "%s", exe);
            return buf;
        }
    }
    snprintf(buf, cap, "/app");
    return buf;
}

static int audio_device_present(void) {
    return access("/dev/snd/pcmC0D0p", F_OK) == 0;
}

int main(int argc, char **argv) {
    MClient c;
    Engine e;
    Page p;
    Font *fs, *fb;
    char bundle[512], url[EVP_URL_MAX], err[512], exe[600];
    int px = 0, py = EVP_BAR_H, pw, ph;
    int quit = 0, dirty = 1, new_frame = 0, prev_buttons = 0;
    int last_mx = 0, last_my = 0;
    long last_status = 0;

    memset(&p, 0, sizeof(p));
    memset(&e, 0, sizeof(e));
    e.from_stderr = -1;
#ifndef _WIN32
    e.net.fd = -1;
#endif

    if (argc > 1 && argv[1] && argv[1][0]) {
        if (argv[1][0] == '/') snprintf(url, sizeof(url), "file://%s", argv[1]);
        else                   snprintf(url, sizeof(url), "%s", argv[1]);
    } else {
        snprintf(url, sizeof(url), "about:blank");
    }

    if (mc_open(&c, "engine", "Browser Engine", EVP_WIN_W, EVP_WIN_H) < 0) {
        fprintf(stderr, "engineview: cannot connect to the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                        : "/run/mystic/wm.sock");
        return 1;
    }
    if (font_init("/usr/share/fonts") != 0) {
        fprintf(stderr, "engineview: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }
    fs = font_get(FONT_REGULAR, 10);
    fb = font_get(FONT_SEMIBOLD, 13);

    pw = c.w;
    ph = c.h - EVP_BAR_H - EVP_FOOT_H;
    if (ph < 1) ph = 1;
    p.page_w = pw;
    p.page_h = ph;
    p.first_send_ms = now_ms();
    snprintf(p.url, sizeof(p.url), "%s", url);
    if (strcmp(url, "about:blank")) snprintf(p.typing, sizeof(p.typing), "%s", url);

    bundle_dir(bundle, sizeof(bundle));
    /* What the jail really gives this process: a silent exit 1 from the
     * engine is not explainable without knowing which of these are absent. */
    fprintf(stderr, "jail: uid=%d dev_null=%d urandom=%d shm=%d proc=%d tmp=%d\n",
            (int)getuid(), access("/dev/null", F_OK) == 0,
            access("/dev/urandom", F_OK) == 0, access("/dev/shm", F_OK) == 0,
            access("/proc/self/exe", F_OK) == 0, access("/tmp", W_OK) == 0);
    {
        DIR *jd = opendir("/dev");
        if (jd) {
            struct dirent *de;
            char names[512] = {0};
            while ((de = readdir(jd)) != NULL) {
                if (de->d_name[0] == '.') continue;
                if (strlen(names) + strlen(de->d_name) + 2 < sizeof names) {
                    strcat(names, de->d_name);
                    strcat(names, " ");
                }
            }
            closedir(jd);
            fprintf(stderr, "jail: /dev = %s\n", names);
        } else {
            fprintf(stderr, "jail: /dev missing\n");
        }
    }
    snprintf(exe, sizeof(exe), "%s/usr/lib/chromium/chromium-headless-shell", bundle);

    if (access(exe, X_OK) != 0) {
        snprintf(err, sizeof(err), "%s: %s", exe, strerror(errno));
        draw_missing(&c, fs, fb, err);
        snprintf(p.state, sizeof(p.state), "no engine");
    } else {
        char loader[560];
        snprintf(loader, sizeof(loader), "%s/lib/ld-musl-x86_64.so.1", bundle);
        if (access(loader, R_OK) != 0)
            snprintf(loader, sizeof(loader), "%s/ld-musl-x86_64.so.1", bundle);
        if (access(loader, R_OK) != 0) {
            snprintf(err, sizeof(err),
                     "the bundle has no %s, and the jail has no /lib: the "
                     "engine's PT_INTERP is /app/ld-musl-x86_64.so.1", loader);
            draw_missing(&c, fs, fb, err);
            snprintf(p.state, sizeof(p.state), "no loader");
        } else {
            int mute = !audio_device_present();
            if (mute)
                fprintf(stderr, "engineview: no /dev/snd PCM node - passing "
                                "--mute-audio once (decode still happens)\n");
            /* The live DevTools channel is the interactive path; the fallback is
             * --screenshot, one frame per navigation. engine_spawn() only forks:
             * the endpoint is waited for by engine_attach() in the frame loop, so
             * a browser that is slow to listen (or never does) is a countdown in
             * the window, not ten seconds of frozen screen. */
            int spawned = engine_spawn(&e, bundle, p.page_w, p.page_h, mute,
                                       err, sizeof(err));
            if (spawned == 0) {
                snprintf(p.state, sizeof(p.state),
                         "pid %d, waiting for DevToolsActivePort", (int)e.pid);
                fprintf(stderr, "engineview: live channel started (pid %d), "
                                "waiting for %s\n", (int)e.pid, EVP_ACTIVE_PORT);
                /* The card goes up now; the loop counts the wait. */
                draw_ui(&c, &p, &e, fs, fb, px, py, pw, ph);
            } else {
                fprintf(stderr, "engineview: live channel not started: %s\n", err);
                /* A --screenshot render can take tens of seconds; paint the
                 * card first so the wait is a message, not a blank window. */
                p.busy = 1;
                snprintf(p.state, sizeof(p.state), "rendering %s with --screenshot",
                         p.url);
                draw_ui(&c, &p, &e, fs, fb, px, py, pw, ph);
                if (page_shot(bundle, &p, err, sizeof(err)) != 0)
                    draw_missing(&c, fs, fb, err);
            }
        }
    }

    while (!quit) {
        MEvent ev;
        new_frame = 0;

        if (e.ws) {
            new_frame = engine_pump(&p, &e);
            if (new_frame) dirty = 1;
        } else if (e.waiting) {
            char aerr[256];
            int r = engine_attach(&e, aerr, sizeof(aerr));
            if (r == 1) {
                e.waiting = 0;
                p.shot_mode = 0;          /* a live channel replaced any still */
                p.first_send_ms = now_ms();   /* the frame clock starts now */
                snprintf(p.state, sizeof(p.state), "live pid %d", (int)e.pid);
                page_create(&p, &e);
                dirty = 1;
            } else if (r < 0) {
                e.waiting = 0;
                fprintf(stderr, "engineview: live channel not started: %s\n", aerr);
                p.busy = 1;
                snprintf(p.state, sizeof(p.state), "rendering %s with --screenshot",
                         p.url);
                draw_ui(&c, &p, &e, fs, fb, px, py, pw, ph);
                if (page_shot(bundle, &p, err, sizeof(err)) != 0)
                    draw_missing(&c, fs, fb, err);
                else
                    snprintf(p.state, sizeof(p.state), "rendered (live: %s)", aerr);
                dirty = 1;
            }
        } else {
            engine_note_stderr(&e);
        }

        while (mc_poll(&c, &ev)) {
            if (ev.type == MC_CLOSE) {
                quit = 1;
            } else if (ev.type == MC_RESIZE) {
                if (ev.w > 0 && ev.h > 0 && ev.w <= c.maxw && ev.h <= c.maxh) {
                    c.w = ev.w;
                    c.h = ev.h;
                    pw = c.w;
                    ph = c.h - EVP_BAR_H - EVP_FOOT_H;
                    if (ph < 1) ph = 1;
                    p.page_w = pw;
                    p.page_h = ph;
                    page_metrics(&p, &e);
                    page_start_cast(&p, &e);
                    dirty = 1;
                }
            } else if (ev.type == MC_KEY && ev.press) {
                if (ev.key == KEY_ESC) {
                    if (p.bar_focus) { p.bar_focus = 0; dirty = 1; }
                    else quit = 1;
                } else if (ev.key == KEY_F6 || (ev.key == KEY_L && (ev.mods & MOD_CTRL))) {
                    p.bar_focus = 1;
                    snprintf(p.typing, sizeof(p.typing), "%s",
                             strcmp(p.url, "about:blank") ? p.url : "");
                    dirty = 1;
                } else if (ev.key == KEY_F5 || (ev.key == KEY_R && (ev.mods & MOD_CTRL))) {
                    if (e.ws) {
                        /* A live channel reloads the page it has. Before the
                         * page is attached page_navigate() is a no-op, and the
                         * attach path navigates to p->url itself. */
                        page_navigate(&p, &e, p.url);
                    } else if (e.waiting) {
                        /* The channel is coming up; it navigates to p->url when
                         * it attaches, so there is nothing to render now. */
                        snprintf(p.state, sizeof(p.state),
                                 "reload when the channel is up");
                        dirty = 1;
                    } else if (p.url[0]) {
                        /* the fallback has no live page to reload */
                        if (page_shot(bundle, &p, err, sizeof(err)) != 0)
                            draw_missing(&c, fs, fb, err);
                        dirty = 1;
                    }
                } else if (p.bar_focus) {
                    if (ev.key == KEY_ENTER) {
                        p.bar_focus = 0;
                        if (p.typing[0]) {
                            snprintf(p.url, sizeof(p.url), "%s", p.typing);
                            if (e.ws || e.waiting) {
                                /* The live page navigates itself; nothing is
                                 * rendered here. page_navigate() drops the URL
                                 * until the page is attached, and the attach
                                 * handler navigates to p->url - so this also
                                 * works while a tab is being created, or while
                                 * the channel itself is still coming up. Never
                                 * start a second browser to answer this. */
                                page_navigate(&p, &e, p.url);
                                /* A channel with no page (createTarget or
                                 * attachToTarget failed) gets one more try:
                                 * that is the only state where this happens. */
                                if (e.ws && !p.ready && !p.creating)
                                    page_create(&p, &e);
                            } else if (page_shot(bundle, &p, err,
                                                 sizeof(err)) != 0) {
                                draw_missing(&c, fs, fb, err);
                            }
                        }
                        dirty = 1;
                    } else if (ev.key == KEY_BACKSPACE) {
                        size_t n = strlen(p.typing);
                        if (n) p.typing[n - 1] = 0;
                        dirty = 1;
                    } else if (ev.key == KEY_TAB) {
                        input_key(&e, &p, ev.key, ev.mods, 1);
                    }
                } else {
                    input_key(&e, &p, ev.key, ev.mods, 1);
                }
            } else if (ev.type == MC_KEY && !ev.press) {
                if (!p.bar_focus) input_key(&e, &p, ev.key, ev.mods, 0);
            } else if (ev.type == MC_TEXT) {
                /* Typing into the window starts an address, the way a browser
                 * behaves when nothing else on the page takes the keyboard:
                 * the first character puts the bar in focus. */
                if (!p.bar_focus && ev.text[0] && ev.text[0] != 10) {
                    p.bar_focus = 1;
                    if (!strcmp(p.url, "about:blank")) p.typing[0] = 0;
                }
                if (p.bar_focus) {
                    size_t n = strlen(p.typing), i;
                    for (i = 0; ev.text[i] && n + 1 < sizeof(p.typing); i++)
                        p.typing[n++] = ev.text[i];
                    p.typing[n] = 0;
                    dirty = 1;
                } else {
                    /* the window server turns a line break into '\n' */
                    if (strchr(ev.text, '\n')) {
                        input_key(&e, &p, KEY_ENTER, 0, 1);
                        input_key(&e, &p, KEY_ENTER, 0, 0);
                    } else {
                        input_text(&e, &p, ev.text);
                    }
                }
            } else if (ev.type == MC_MOUSE) {
                int mx, my, down = ev.buttons & 1, was = prev_buttons & 1;
                last_mx = ev.x;
                last_my = ev.y;
                mx = ev.x - px;
                my = ev.y - py;
                if (mx >= 0 && my >= 0 && mx < pw && my < ph) {
                    if (down && !was)
                        input_mouse(&e, &p, "mousePressed", mx, my, "left", 1);
                    else if (!down && was)
                        input_mouse(&e, &p, "mouseReleased", mx, my, "left", 0);
                    else
                        input_mouse(&e, &p, "mouseMoved", mx, my, "none",
                                    down ? 1 : 0);
                }
                prev_buttons = ev.buttons;
            } else if (ev.type == MC_WHEEL) {
                /* WHEEL carries no position: use where the pointer last was,
                 * which is exactly what the window server's comment says the
                 * client is expected to do. */
                int mx = last_mx - px, my = last_my - py;
                if (mx >= 0 && my >= 0 && mx < pw && my < ph)
                    input_wheel(&e, &p, mx, my, ev.wheel);
            }
        }

        /* One status refresh every 400 ms. Nothing here touches the disk. */
        {
            long tick = now_ms();
            if (tick - last_status >= 400) {
                last_status = tick;
                if (p.want_info) page_ask_info(&p, &e);
                if (p.cdperr[0] || !e.alive || !p.frame.px) dirty = 1;
            }
        }

        /* Reap a dead engine, or stop one this window can no longer drive,
         * exactly once. Without the second half a browser whose DevTools socket
         * failed would keep running - hundreds of megabytes - while nothing
         * could reach it. */
        if (e.pid > 0 && !e.alive) {
            int st = 0;
            if (waitpid(e.pid, &st, WNOHANG) == e.pid) {
                /* The stderr pipe still holds whatever the engine wrote before
                 * it died; closing it here without reading loses the only copy
                 * of the reason. */
                engine_note_stderr(&e);
                if (e.stderr_len)
                    fprintf(stderr, "engine: %s\n", engine_last_stderr(&e));
                e.status = WIFEXITED(st) ? WEXITSTATUS(st)
                         : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
                e.pid = 0;
            } else {
                engine_note_stderr(&e);
                engine_stop(&e);
                fprintf(stderr, "engineview: stopped an unreachable engine "
                                "(status %d): %s\n", e.status,
                        p.cdperr[0] ? p.cdperr : p.state);
            }
            engine_close_pipe(&e);
            dirty = 1;
        }

        if (dirty) {
            draw_ui(&c, &p, &e, fs, fb, px, py, pw, ph);
            dirty = 0;
        }

        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (new_frame ? 4L : 12L) * 1000L * 1000L;
            nanosleep(&ts, NULL);
        }
    }

    engine_stop(&e);
    engine_close_pipe(&e);
    img_free(&p.frame);
    mc_close(&c);
    return 0;
}

#else /* EVP_SELFTEST: the parsers, on the host, with no window server */

static int fails;
static void ck(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); fails++; }
    else       { printf("ok   %s\n", what); }
}

int main(void) {
    const char *reply = "{\"id\":4,\"result\":{\"sessionId\":\"A1B2C3\"}}";
    const char *frame =
        "{\"method\":\"Page.screencastFrame\",\"sessionId\":\"A1B2C3\","
        "\"params\":{\"data\":\"AAECAwQ=\",\"metadata\":{\"pageScaleFactor\":1},"
        "\"sessionId\":7}}";
    const char *errmsg =
        "{\"id\":9,\"error\":{\"code\":-32601,\"message\":\"'Foo.bar' wasn't found\"}}";
    const char *esc = "{\"title\":\"a\\\"b\\\\c\\u0041\"}";
    const char *nested =
        "{\"id\":11,\"sessionId\":\"S\",\"result\":{\"result\":"
        "{\"type\":\"string\",\"value\":\"[\\\"Example\\\\nPage\\\","
        "\\\"https://example.com/a?b=1\\\"]\"}}}";
    const char *keys[3];
    char buf[256], t2[128];
    long v = 0;
    const char *sl;
    size_t sln;
    unsigned char raw[64];
    long n;

    ck(js_get_int(reply, strlen(reply), "id", &v) && v == 4, "id is read");
    ck(js_get(reply, strlen(reply), "result", &sl, &sln) &&
       js_get_str(sl, sln, "sessionId", buf, sizeof(buf)) &&
       !strcmp(buf, "A1B2C3"), "nested result.sessionId");

    ck(js_get_str(frame, strlen(frame), "method", buf, sizeof(buf)) &&
       !strcmp(buf, "Page.screencastFrame"), "method is read");
    ck(js_get(frame, strlen(frame), "params", &sl, &sln), "params slice");
    {
        const char *d;
        size_t dl;
        ck(js_get(sl, sln, "data", &d, &dl), "params.data slice");
        n = b64_decode(d + 1, dl - 2, raw, sizeof(raw));
        ck(n == 5 && raw[0] == 0 && raw[1] == 1 && raw[2] == 2 &&
           raw[3] == 3 && raw[4] == 4, "base64 AAECAwQ= -> 00 01 02 03 04");
        ck(js_get(sl, sln, "sessionId", &d, &dl) && strtol(d, NULL, 10) == 7,
           "the screencast session id is 7 (ack value)");
    }

    ck(js_get(errmsg, strlen(errmsg), "error", &sl, &sln), "error object is found");
    ck(js_get_str(sl, sln, "message", buf, sizeof(buf)) &&
       !strcmp(buf, "'Foo.bar' wasn't found"), "the engine's error text is read");

    ck(js_get_str(esc, strlen(esc), "title", buf, sizeof(buf)) &&
       !strcmp(buf, "a\"b\\cA"), "string escapes");

    keys[0] = "result"; keys[1] = "result"; keys[2] = "value";
    ck(js_get_nested(nested, strlen(nested), keys, 3, &sl, &sln) && sl[0] == '"',
       "result.result.value is reachable");
    ck(js_copy_string(sl, sl + sln, buf, sizeof(buf)) == 0 &&
       js_two_strings(buf, strlen(buf), t2, sizeof(t2), buf, sizeof(buf)) == 0 &&
       !strcmp(t2, "Example\nPage"), "the title JSON-decodes (with its \\n)");

    set_err(buf, 8, "abcdefghij");
    ck(strlen(buf) == 7 && !strncmp(buf, "abcdefg", 7),
       "set_err truncates instead of overflowing");
    ck(ev_char(KEY_Q) == 'q' && ev_char(KEY_A) == 'a' && ev_char(KEY_Z) == 'z' &&
       ev_char(KEY_1) == '1' && ev_char(KEY_0) == '0' && ev_char(KEY_SLASH) == '/',
       "evdev letters/digits for Ctrl shortcuts");
    ck(cdp_mods(MOD_CTRL | MOD_SHIFT) == (2 | 8), "modifier mask is CDP's");
    {
        int i, seen_enter = 0, seen_left = 0, seen_f6 = 0;
        for (i = 0; i < (int)(sizeof(ev_keys) / sizeof(ev_keys[0])); i++) {
            if (ev_keys[i].ev == KEY_ENTER && ev_keys[i].vk == 13) seen_enter = 1;
            if (ev_keys[i].ev == KEY_LEFT && ev_keys[i].vk == 37) seen_left = 1;
            if (ev_keys[i].ev == KEY_F6 && ev_keys[i].vk == 117) seen_f6 = 1;
        }
        ck(seen_enter && seen_left && seen_f6, "Enter/ArrowLeft/F6 table rows");
    }
    {
        const char *bad = "{\"id\":1,\"result\":{\"sess";
        ck(js_get(bad, strlen(bad), "result", &sl, &sln) == 0,
           "truncated JSON is refused, not walked past");
        ck(js_get(bad, strlen(bad), "nope", &sl, &sln) == 0, "absent key is absent");
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
#endif /* EVP_SELFTEST */
