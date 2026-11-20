/* ============================================================
 *  Mystic OS - apps (external) / vlcplay
 *
 *  The video player: libVLC decodes, the frames land in memory this
 *  process owns, and that memory is the Mystic window. There is no X11
 *  and no Wayland here, so the library's normal video outputs have
 *  nowhere to draw. libVLC 3's "vmem" output is the supported way to
 *  ask for decoded pictures as pixels instead of as a window, and this
 *  file is that client.
 *
 *      vlcplay /root/media/clip.mp4
 *
 *  WHAT IS REAL, AND WHAT THE WINDOW SHOWS
 *  ---------------------------------------
 *  Every picture on screen came out of libVLC's decoder: vmem calls our
 *  lock() with a plane pointer, libVLC's converter copies the decoded
 *  picture into it with picture_CopyPixels(), unlock() says it is
 *  complete, and display() says it is due. Before the first picture the
 *  body is not a fake frame - it is a message saying no picture has
 *  arrived. If libVLC reports an error, the text is
 *  libvlc_errmsg()'s own string, copied immediately.
 *
 *  If libVLC is not installed the window is still drawn, and it says the
 *  one thing the user can act on, in package-manager words:
 *
 *      libVLC is not installed - install it with: pkg install vlc
 *
 *  plus the real loader error (dlopen) or the name of the symbol that was
 *  missing. That is the same sentence `pkg` prints (tools/fetch-vlc.py's
 *  package description and src/client/apps/vlcplayer.c use it too), not
 *  a second phrasing that could drift.
 *
 *  THE PIXEL FORMAT: WHY THERE IS NO CHANNEL CONVERSION
 *  ---------------------------------------------------
 *  libvlc_video_set_format(mp, "RV32", w, h, pitch) makes vmem.c set
 *
 *      rmask = 0xff0000, gmask = 0x00ff00, bmask = 0x0000ff
 *
 *  (modules/video_output/vmem.c, Open()), so a pixel is 0x00RRGGBB in a
 *  uint32_t and the bytes in memory are B, G, R, X. A Mystic colour is
 *  0xAARRGGBB (src/mystic.h), whose bytes are B, G, R, A; the
 *  compositor copies the low 24 bits and never reads the top byte. The
 *  three colour bytes are therefore already in the right order, and
 *  surf_blit() copies u32 values without touching a channel.
 *
 *  WHY THE PICTURE IS SCALED TO THE BODY, AND WHY THE LETTERBOX IS OURS
 *  ------------------------------------------------------------------
 *  vmem with the simple format API gives the display exactly the size we
 *  asked for, and libVLC's converter STRETCHES the decoded picture to
 *  fill it. That is not a guess: with the real libVLC 3.0.24 on the
 *  host, a 96x64 AVI asked for as 320x180 filled all 320 columns of the
 *  middle row (0 black columns), and a 320x176 H.264 file asked for as
 *  320x180 did the same. libvlc_video_get_size() still reported the
 *  files' own 96x64 and 320x176.
 *
 *  So the buffer contains the whole picture, anisotropically stretched,
 *  and the correct display is the whole buffer mapped into a rectangle
 *  whose aspect is the video's own (get_size) - that mapping restores
 *  square pixels exactly, because the net scale becomes equal in x and
 *  y. The rest of the body is filled with the letterbox colour. The
 *  decode buffer is the body size at open time (960x540 for the default
 *  window), so a 16:9 video needs no scaling in this process at all;
 *  other aspects get one nearest-neighbour resize per frame, and a
 *  window resize scales from the same buffer.
 *
 *  get_size() is libVLC's answer for the video track's size, and for
 *  both measured clips it was the picture's own pixel size. The simple
 *  vmem format API carries no sample-aspect-ratio field, so the fit uses
 *  those pixel dimensions: content whose pixels are not square is shown
 *  at its coded aspect, not at a SAR-corrected one. That is the one
 *  geometric limitation of this path, stated here rather than papered
 *  over.
 *
 *  THREADING: ONE BUFFER, TWO OWNERS, NEVER AT THE SAME TIME
 *  --------------------------------------------------------
 *  libVLC calls lock/unlock/display on its video output thread while the
 *  window is drawn on the main thread. The surface has one mutex:
 *
 *    lock()      waits until the main thread is not reading, sets busy,
 *                and hands libVLC the only plane pointer;
 *    unlock()    clears busy, counts a complete picture, wakes waiters;
 *    display()   counts a presentation (the playback clock said it is due);
 *    blit()      waits for busy to clear, sets busy so the next lock()
 *                waits, copies/scales the picture, clears busy.
 *
 *  No libVLC thread ever writes the window and the main thread never
 *  reads a half-written picture. The main thread only copies when the
 *  frame counter changed (or after a resize), not once per poll.
 *
 *  LOADING THE LIBRARY: THE EXACT PATH LIST
 *  ---------------------------------------
 *  No VLC headers exist in the image and libvlc is not linked: every
 *  function is resolved by name with dlsym(). The candidates are tried
 *  in this order, and the first that loads wins:
 *
 *    1. $MYSTIC_VLC_LIB                     explicit override (tests)
 *    2. $MYSTIC_BUNDLE/lib/libvlc.so.5      the app's own bundle. wmsrv
 *       binds the bundle read-only at /app and exports MYSTIC_BUNDLE=/app
 *       (src/wmsrv.c), so this is the copy that belongs to this app, not
 *       some other VLC that happens to be on the system.
 *    3. /app/lib/libvlc.so.5                the same bind, if the variable
 *       is missing (an older server, or a direct chroot start)
 *    4. /root/apps/vlc/lib/libvlc.so.5      the installed package on the
 *       real rootfs - reachable when the app runs outside a jail
 *    5. <dir of this executable>/../lib/libvlc.so.5   a bundle started
 *       directly (the tile's exec is bin/..., the closure is lib/...)
 *    6. libvlc.so.5                         the loader's own search:
 *       LD_LIBRARY_PATH (wmsrv sets /app/usr/lib:/app/lib inside the
 *       jail) and the system search path
 *
 *  libvlc.so.5's DT_NEEDED includes libvlccore.so.9. When the library is
 *  loaded from an absolute path, its directory's libvlccore.so.9 is
 *  loaded first with RTLD_GLOBAL, so the dependency is satisfied even if
 *  LD_LIBRARY_PATH does not name the bundle (musl reads that variable
 *  only at startup, so setenv() here could not help). VLC_PLUGIN_PATH is
 *  set from the same directory when the caller has not set it - the jail
 *  already exports it, and a direct run outside the jail needs it.
 *
 *  BUILD
 *  -----
 *  The compile check is a single -c (static or not - the file compiles
 *  both ways). A working binary must be DYNAMIC musl, because a static
 *  musl program's dlopen() is a stub that fails with "Dynamic loading
 *  not supported" - and when that happens this app shows that exact
 *  string plus the pkg line, so it is never a silent blank window.
 *
 *      zig cc -target x86_64-linux-musl -Os -dynamic \
 *             -Wl,-rpath,'$ORIGIN/../lib' -Wl,-dynamic-linker=/lib/ld-musl-x86_64.so.1 \
 *             -Isrc -Isrc/client -o vlcplay \
 *             src/client/apps/vlcplay.c src/client/mystic_client.c \
 *             src/gfx.c src/font.c src/ui.c -lm -pthread -ldl
 *
 *  (-dynamic is not optional: zig's musl target defaults to a static
 *  link. The dynamic link then carries PT_INTERP
 *  /lib/ld-musl-x86_64.so.1 and NEEDED libc.so, which is what makes
 *  dlopen() work at all. The rpath makes a direct exec find the closure
 *  next to the binary; inside the jail the loader and LD_LIBRARY_PATH do
 *  the same job.)
 *
 *  The bundle must carry the loader and the closure beside it:
 *
 *      bin/vlcplay                dynamic, PT_INTERP /lib/ld-musl-...
 *      lib/ld-musl-x86_64.so.1    the loader, if the jail has no /lib
 *      lib/libvlc.so.5 ...        libvlc + libvlccore (+ the rest)
 *      lib/vlc/plugins/...        demuxers, decoders, the vmem output
 *
 *  Keys: space play/pause, Left/Right seek 5 s, Up/Down volume, q or Esc
 *  quit, a click on the picture toggles playback.
 * ============================================================ */

#include "../vlcmin.h"           /* src/client/vlcmin.h - the ABI slice */
#include "../mystic_client.h"    /* window, events, mc_present()        */
#include "../../mystic.h"        /* Surface, RGB, fonts                 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
   /* wingdi.h defines RGB(r,g,b) as a COLORREF, which would silently
    * replace src/mystic.h's 0xAARRGGBB macro in a host-proof build. */
#  define NOGDI
#  include <windows.h>
#  undef NOGDI
#else
#  include <dlfcn.h>
#endif

/* The one sentence the package manager also prints. It is a macro so the
 * host harness can compare the bytes the window draws, and so there is
 * no second phrasing to drift. */
#define VLC_MISSING_MSG \
    "libVLC is not installed - install it with: pkg install vlc"
#define VLC_NEED_FILE_MSG \
    "give a media file: Player <path>"

/* Linux evdev codes, the numbers src/input.c forwards (the same values
 * the other apps in src/apps use). */
#define KEY_ESC    1
#define KEY_Q     16
#define KEY_SPACE 57
#define KEY_UP    103
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_DOWN  108

/* ------------------------------------------------------------ layout */

#define WIN_W  960
#define WIN_H  640
#define HEAD_H 36
#define FOOT_H 64
#define BODY_MIN_H 48

#define SEEK_MS 5000
#define VOL_STEP 5

/* Colours: the same quiet dark surface the other client apps use. */
static const u32 C_BG     = RGB(0x0E, 0x13, 0x1B);
static const u32 C_HEAD   = RGB(0x14, 0x1B, 0x26);
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x93, 0xA0, 0xB4);
static const u32 C_FAINT  = RGB(0x5A, 0x66, 0x78);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_WARN   = RGB(0xE2, 0x93, 0x7E);
static const u32 C_LINE   = RGB(0x23, 0x2D, 0x3C);
static const u32 C_BAR    = RGB(0x05, 0x07, 0x0A);  /* letterbox */
static const u32 C_TRACK  = RGB(0x1C, 0x25, 0x33);
static const u32 C_PANEL  = RGB(0x11, 0x17, 0x21);

/* ============================================================
 *  1. libVLC, resolved by name
 * ============================================================ */

#if defined(_WIN32)
typedef HMODULE VlcLib;

static VlcLib vlc_lib_open(const char *path) {
    /* A full path needs LOAD_WITH_ALTERED_SEARCH_PATH so libvlccore.dll
     * beside it is found; a bare name must use the default search. */
    if (strchr(path, '\\') || strchr(path, '/'))
        return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    return LoadLibraryA(path);
}
static void *vlc_lib_sym(VlcLib h, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(h, name);
}
static void vlc_lib_close(VlcLib h) {
    if (h) FreeLibrary(h);
}
static void vlc_lib_error(char *out, size_t n) {
    snprintf(out, n, "LoadLibrary failed (Windows error %lu)",
             (unsigned long)GetLastError());
}
#else
typedef void *VlcLib;

static VlcLib vlc_lib_open(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *vlc_lib_sym(VlcLib h, const char *name) {
    return dlsym(h, name);
}
static void vlc_lib_close(VlcLib h) {
    if (h) dlclose(h);
}
static void vlc_lib_error(char *out, size_t n) {
    const char *e = dlerror();               /* reading it clears it */
    snprintf(out, n, "%s", e && *e ? e : "dlopen failed without dlerror() text");
}
#endif

/* One table, __typeof__ of the prototype in vlcmin.h: a typo in a
 * signature is a compile error, not a crash. */
typedef struct {
#define VLC_MIN_FIELD(sym, field) __typeof__(&(sym)) field;
    VLC_MIN_FUNCTIONS(VLC_MIN_FIELD)
#undef VLC_MIN_FIELD
} VlcApi;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    uint32_t       *fb;        /* dec_w * dec_h, RV32 */
    int             w, h;      /* the dimensions libVLC was told to fill */
    int             pitch;     /* bytes per row */
    int             busy;      /* one side owns fb */
    unsigned        frames;    /* complete pictures (unlock ran) */
    unsigned        displays;  /* presentations (display ran) */
} VlcSurface;

typedef struct {
    VlcLib   lib;
    VlcApi   api;
    int      lib_ok;           /* the library and every symbol loaded */
    char     lib_path[256];    /* the candidate that loaded */
    char     lib_detail[320];  /* exact dlerror / missing symbol */
    char     lib_tried[2][256];/* first and last candidate, for the card */

    libvlc_instance_t     *inst;
    libvlc_media_t        *media;
    libvlc_media_player_t *mp;
    char     media_path[512];
    int      started;          /* play() was accepted */
    int      paused;           /* our last set_pause() request */
    int      inst_failed;      /* libvlc_new() failed: the library loaded */
    int      vol_init;         /* the 100% startup volume was applied      */
    int      symbol_missing;   /* libvlc.so.5 loaded but lacks a function  */
    char     last_err[192];    /* libvlc_errmsg() at a failure, copied */

    VlcSurface surf;
    int dec_w, dec_h;
    int vid_w, vid_h;          /* libvlc_video_get_size, 0 until known */
    int vol;
    int vol_known;             /* libVLC accepted a volume query */
    int audio_dead;            /* set_volume returned < 0: no output */
} VlcPlayer;

/* Copy the first loader error verbatim, then bind every symbol. */
static int vlc_bind(VlcPlayer *p, char *missing, size_t mn) {
    _Static_assert(sizeof(void *) == sizeof(p->api.new_instance),
                   "function pointers and void* must be the same size");
#define VLC_MIN_BIND(sym, field)                                        \
    do {                                                                \
        void *fn_ = vlc_lib_sym(p->lib, #sym);                          \
        if (!fn_) {                                                     \
            snprintf(missing, mn, "%s", #sym);                          \
            return 0;                                                   \
        }                                                               \
        memcpy(&p->api.field, &fn_, sizeof(p->api.field));              \
    } while (0);
    VLC_MIN_FUNCTIONS(VLC_MIN_BIND)
#undef VLC_MIN_BIND
    return 1;
}

#if !defined(_WIN32)
/* The directory part of an absolute path, without libgen.h's licence to
 * modify its argument. */
static void dir_of(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/');
    if (!slash) { out[0] = 0; return; }
    size_t len = (size_t)(slash - path);
    if (len >= n) len = n - 1;
    memcpy(out, path, len);
    out[len] = 0;
}
#endif

/* libvlc.so.5 needs libvlccore.so.9. Load the sibling first so the
 * dependency is satisfied by soname even when LD_LIBRARY_PATH (which
 * musl read at process start) does not name this directory. */
static void vlc_preload_core(const char *libpath) {
#if !defined(_WIN32)
    char dir[256], core[320];
    if (!strchr(libpath, '/')) return;
    dir_of(libpath, dir, sizeof(dir));
    if (!dir[0]) return;
    snprintf(core, sizeof(core), "%s/libvlccore.so.9", dir);
    /* Best effort: if this fails, libvlc's own dlopen may still succeed
     * through RUNPATH or LD_LIBRARY_PATH, and if it does not, the error
     * text it produces is the one shown. */
    (void)dlopen(core, RTLD_NOW | RTLD_GLOBAL);
#endif
}

/* Point VLC at the bundle's plugins when the caller has not. The module
 * bank reads VLC_PLUGIN_PATH with getenv() when the instance is created,
 * so setting it here works (unlike the loader's LD_LIBRARY_PATH). */
static void vlc_set_plugin_path(const char *libpath) {
#if !defined(_WIN32)
    char dir[256], plugins[320];
    const char *have = getenv("VLC_PLUGIN_PATH");
    if (have && *have) return;
    if (!strchr(libpath, '/')) return;
    dir_of(libpath, dir, sizeof(dir));       /* <bundle>/lib */
    if (!dir[0]) return;
    snprintf(plugins, sizeof(plugins), "%s/vlc/plugins", dir);
    if (access(plugins, R_OK) == 0)
        setenv("VLC_PLUGIN_PATH", plugins, 1);
#endif
}

/* The ordered candidate list documented at the top of the file. */
static int vlc_candidates(char out[][256], int max) {
    int n = 0;
    const char *env = getenv("MYSTIC_VLC_LIB");
    if (env && *env && n < max) snprintf(out[n++], 256, "%s", env);
#if defined(_WIN32)
    if (n < max) snprintf(out[n++], 256, "%s", "libvlc.dll");
#else
    const char *bundle = getenv("MYSTIC_BUNDLE");
    if (bundle && *bundle && n < max)
        snprintf(out[n++], 256, "%s/lib/%s", bundle, VLC_MIN_SONAME);
    if (n < max) snprintf(out[n++], 256, "/app/lib/%s", VLC_MIN_SONAME);
    if (n < max)
        snprintf(out[n++], 256, "/root/apps/vlc/lib/%s", VLC_MIN_SONAME);
    {
        /* <dir of this executable>/../lib - a bundle run directly. */
        char exe[256];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len > 0) {
            char dir[256];
            exe[len] = 0;
            dir_of(exe, dir, sizeof(dir));
            if (dir[0] && n < max)
                snprintf(out[n++], 256, "%s/../lib/%s", dir, VLC_MIN_SONAME);
        }
    }
    if (n < max) snprintf(out[n++], 256, "%s", VLC_MIN_SONAME);
#endif
    return n;
}

/* Load libvlc and bind every symbol. 0 on success; on failure lib_ok is
 * 0 and lib_detail holds the real reason, byte for byte. */
static int vlc_load(VlcPlayer *p) {
    char paths[8][256];
    int n = vlc_candidates(paths, 8);
    char why[256] = "";

    p->lib_ok = 0;
    p->lib_detail[0] = 0;
    p->lib_path[0] = 0;
    p->lib_tried[0][0] = 0;
    p->lib_tried[1][0] = 0;

    if (n > 0) snprintf(p->lib_tried[0], sizeof(p->lib_tried[0]), "%s", paths[0]);
    if (n > 0) snprintf(p->lib_tried[1], sizeof(p->lib_tried[1]), "%s", paths[n - 1]);

    for (int i = 0; i < n; i++) {
        VlcLib h;
        char missing[96];

        vlc_preload_core(paths[i]);
        h = vlc_lib_open(paths[i]);
        if (!h) {
            vlc_lib_error(why, sizeof(why));
            continue;                       /* keep the last real error */
        }
        p->lib = h;
        snprintf(p->lib_path, sizeof(p->lib_path), "%s", paths[i]);
        if (!vlc_bind(p, missing, sizeof(missing))) {
            p->symbol_missing = 1;
            snprintf(p->lib_detail, sizeof(p->lib_detail),
                     "%s does not export %s", paths[i], missing);
            vlc_lib_close(p->lib);
            p->lib = NULL;
            p->lib_path[0] = 0;
            return -1;
        }
        p->lib_ok = 1;
        vlc_set_plugin_path(paths[i]);
        return 0;
    }

    snprintf(p->lib_detail, sizeof(p->lib_detail), "%s",
             why[0] ? why : "no candidate path could be tried");
    return -1;
}

/* ============================================================
 *  2. The frame surface and the vmem callbacks
 * ============================================================ */

static int surf_init(VlcSurface *s, int w, int h) {
    memset(s, 0, sizeof(*s));
    if (w < 2 || h < 2) return -1;
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    s->fb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!s->fb) return -1;
    if (pthread_mutex_init(&s->m, NULL) != 0) { free(s->fb); s->fb = NULL; return -1; }
    if (pthread_cond_init(&s->cv, NULL) != 0) {
        pthread_mutex_destroy(&s->m);
        free(s->fb);
        s->fb = NULL;
        return -1;
    }
    return 0;
}

static void surf_free(VlcSurface *s) {
    if (!s) return;
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->m);
    free(s->fb);
    s->fb = NULL;
}

static void *cb_lock(void *opaque, void **planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    pthread_mutex_lock(&s->m);
    while (s->busy) pthread_cond_wait(&s->cv, &s->m);
    s->busy = 1;                            /* blit() will wait now */
    pthread_mutex_unlock(&s->m);
    if (planes) planes[0] = s->fb;
    return s->fb;
}

static void cb_unlock(void *opaque, void *picture, void *const *planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    (void)picture;
    (void)planes;
    pthread_mutex_lock(&s->m);
    s->busy = 0;
    s->frames++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}

static void cb_display(void *opaque, void *picture) {
    VlcSurface *s = (VlcSurface *)opaque;
    (void)picture;
    pthread_mutex_lock(&s->m);
    s->displays++;
    pthread_mutex_unlock(&s->m);
}

static unsigned surf_frames(VlcSurface *s) {
    unsigned n;
    pthread_mutex_lock(&s->m);
    n = s->frames;
    pthread_mutex_unlock(&s->m);
    return n;
}

/* Copy the newest complete picture into the (dx,dy,dw,dh) rectangle of
 * dst. The source is always the whole buffer: libVLC stretched the whole
 * picture into it (see the top of the file), so mapping all of it to a
 * rectangle of the video's aspect restores square pixels. Returns the
 * frame count that was copied, or 0 when no picture exists yet. */
static unsigned surf_blit(VlcSurface *s, uint32_t *dst, int dstride,
                          int dx, int dy, int dw, int dh) {
    unsigned seq;
    int sw, sh;

    if (!s || !s->fb || !dst || dw <= 0 || dh <= 0) return 0;

    pthread_mutex_lock(&s->m);
    while (s->busy) pthread_cond_wait(&s->cv, &s->m);
    if (s->frames == 0) {
        pthread_mutex_unlock(&s->m);
        return 0;
    }
    s->busy = 1;                            /* the next lock() waits */
    seq = s->frames;
    sw = s->w;
    sh = s->h;
    pthread_mutex_unlock(&s->m);

    if (dw == sw && dh == sh) {
        for (int y = 0; y < sh; y++)
            memcpy(dst + (size_t)(dy + y) * (size_t)dstride + dx,
                   s->fb + (size_t)y * (size_t)sw, (size_t)sw * 4u);
    } else {
        uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / (uint64_t)dw);
        for (int y = 0; y < dh; y++) {
            int sy = (int)(((uint64_t)y * (uint64_t)sh) / (uint64_t)dh);
            const uint32_t *srow = s->fb + (size_t)sy * (size_t)sw;
            uint32_t *drow = dst + (size_t)(dy + y) * (size_t)dstride + dx;
            uint32_t fx = 0;
            for (int x = 0; x < dw; x++) {
                drow[x] = srow[fx >> 16];
                fx += xstep;
            }
        }
    }

    pthread_mutex_lock(&s->m);
    s->busy = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    return seq;
}

/* Aspect-preserving fit of vw x vh into bw x bh, integer arithmetic.
 * The degenerate cases (size not known yet) fill the body. */
static void fit_rect(int vw, int vh, int bw, int bh,
                     int *ox, int *oy, int *ow, int *oh) {
    if (vw <= 0 || vh <= 0 || bw <= 0 || bh <= 0) {
        *ox = 0; *oy = 0; *ow = bw; *oh = bh;
        return;
    }
    if ((int64_t)bw * (int64_t)vh <= (int64_t)bh * (int64_t)vw) {
        int h = (int)(((int64_t)bw * (int64_t)vh) / (int64_t)vw);
        if (h < 1) h = 1;
        *ox = 0; *oy = (bh - h) / 2; *ow = bw; *oh = h;
    } else {
        int w = (int)(((int64_t)bh * (int64_t)vw) / (int64_t)vh);
        if (w < 1) w = 1;
        *ox = (bw - w) / 2; *oy = 0; *ow = w; *oh = bh;
    }
}

/* ============================================================
 *  3. The player lifecycle
 * ============================================================ */

/* A fresh libvlc player reports the audio output's own idea of the volume
 * (0 on a device that has not been used yet). Ask for the normal 100% once
 * at play, and once more when the first picture arrives, because the audio
 * output may only exist by then. After that a failure is real: there is no
 * audio output. */
static void vp_start_volume(VlcPlayer *p, int final) {
    if (!p->mp || p->vol_init || p->audio_dead) return;
    if (p->api.audio_set_volume(p->mp, 100) >= 0) {
        /* The setter's 0 is libVLC accepting the value; that accepted
         * value is what the footer shows. The getter is not used: in the
         * host proof (VLC 3.0.24) set_volume(100) returned 0 and
         * get_volume() answered 5, and showing a second, contradicting
         * number would be worse than the one libVLC accepted. */
        p->vol = 100;
        p->vol_known = 1;
        p->vol_init = 1;
    } else if (final) {
        p->audio_dead = 1;
        p->vol_init = 1;
    }
}

static void save_err(VlcPlayer *p, const char *what) {
    const char *e = p->api.errmsg ? p->api.errmsg() : NULL;
    snprintf(p->last_err, sizeof(p->last_err), "%s: %s", what,
             e && *e ? e : "libVLC gave no error text");
}

/* Open the library, create the instance, and - if a path was given -
 * build the media and the player and start it. Returns 0 when the window
 * has something real to show (even if that something is "libVLC is
 * missing" or "give me a file"); -1 only when memory or the surface
 * failed. */
static int vp_open(VlcPlayer *p, const char *media_path,
                   int dec_w, int dec_h) {
    memset(p, 0, sizeof(*p));

    if (surf_init(&p->surf, dec_w, dec_h) != 0) return -1;
    p->dec_w = dec_w;
    p->dec_h = dec_h;

    vlc_load(p);

    if (p->lib_ok && media_path && *media_path) {
        static const char *const opts[] = {
            "--no-video-title-show",   /* VLC's OSD must not land in the frame */
            "--avcodec-hw=none",       /* a GPU frame cannot be handed to vmem */
        };
        libvlc_instance_t *inst;

        inst = p->api.new_instance(2, opts);
        if (!inst) {
            /* The library is there, so the pkg sentence would be a lie.
             * Keep lib_ok and let the card show libvlc_errmsg() instead. */
            save_err(p, "libvlc_new() failed");
            p->inst_failed = 1;
            return 0;
        }
        p->inst = inst;

        snprintf(p->media_path, sizeof(p->media_path), "%s", media_path);
        p->media = p->api.media_new_path(inst, media_path);
        if (!p->media) {
            save_err(p, "libvlc_media_new_path() failed");
            return 0;
        }
        p->mp = p->api.player_new(inst);
        if (!p->mp) {
            save_err(p, "libvlc_media_player_new() failed");
            return 0;
        }
        p->api.player_set_media(p->mp, p->media);

        /* The format is fixed before play and is the only vmem contract:
         * RV32 pixels, the body's size, a tightly packed pitch. */
        p->api.video_set_callbacks(p->mp, cb_lock, cb_unlock, cb_display,
                                   &p->surf);
        p->api.video_set_format(p->mp, "RV32", (unsigned)dec_w,
                                (unsigned)dec_h, (unsigned)dec_w * 4u);

        if (p->api.player_play(p->mp) != 0) {
            save_err(p, "libvlc_media_player_play() failed");
            return 0;
        }
        p->started = 1;
        /* Not final: the audio output is created with the video output,
         * so a failure here may only mean "not yet". */
        vp_start_volume(p, 0);
    }
    return 0;
}

static void vp_close(VlcPlayer *p) {
    if (!p) return;
    if (p->mp) {
        p->api.player_stop(p->mp);
        p->api.player_release(p->mp);
        p->mp = NULL;
    }
    if (p->media) {
        p->api.media_release(p->media);
        p->media = NULL;
    }
    if (p->inst) {
        p->api.release(p->inst);
        p->inst = NULL;
    }
    if (p->lib) {
        vlc_lib_close(p->lib);
        p->lib = NULL;
    }
    surf_free(&p->surf);
}

/* Ask libVLC for the video's own size. Called by the main thread only,
 * and only while a picture exists. */
static void vp_refresh_video_size(VlcPlayer *p) {
    unsigned w = 0, h = 0;
    if (!p->mp || !p->api.video_get_size) return;
    if (p->api.video_get_size(p->mp, 0, &w, &h) == 0 && w >= 2 && h >= 2) {
        p->vid_w = (int)w;
        p->vid_h = (int)h;
    }
}

static void vp_toggle_pause(VlcPlayer *p) {
    libvlc_state_t st;
    if (!p->mp) return;
    st = p->api.player_get_state(p->mp);
    if (st == libvlc_Ended || st == libvlc_Stopped || st == libvlc_Error) {
        p->api.player_set_time(p->mp, 0);
        if (p->api.player_play(p->mp) != 0) {
            save_err(p, "restart failed");
            return;
        }
        p->started = 1;
        p->paused = 0;
        return;
    }
    p->paused = !p->paused;
    p->api.player_set_pause(p->mp, p->paused);
}

static void vp_seek(VlcPlayer *p, int64_t delta_ms) {
    int64_t now, len, to;
    if (!p->mp || !p->started) return;
    now = p->api.player_get_time(p->mp);
    len = p->api.player_get_length(p->mp);
    if (now < 0) now = 0;
    to = now + delta_ms;
    if (to < 0) to = 0;
    if (len > 0 && to > len) to = len;
    p->api.player_set_time(p->mp, to);
}

static void vp_volume(VlcPlayer *p, int delta) {
    int v;
    if (!p->mp || p->audio_dead) return;
    v = p->vol_known ? p->vol : 100;
    v += delta;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    if (p->api.audio_set_volume(p->mp, v) < 0) {
        /* No audio output exists (a jail with no /dev/snd and no ALSA
         * plugin). Say it once; do not retry every key press. */
        p->audio_dead = 1;
        return;
    }
    p->vol = v;
    p->vol_known = 1;
}

/* ============================================================
 *  4. Drawing
 * ============================================================ */

typedef struct {
    Font *title;     /* 15 semibold - the window name */
    Font *body;      /* 11 regular - the status row, card text */
    Font *small;     /* 9  medium  - the second row, card detail */
    Font *mono;      /* 11 mono    - time codes */
} Faces;

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    Rect head, body, foot;
} Layout;

typedef struct {
    MClient   c;
    Surface   s;
    Faces     f;
    VlcPlayer pl;

    int  win_w, win_h;          /* current drawing area */

    int  body_mode;             /* 0 empty, 1 picture, 2 card */
    int  card_state;            /* libvlc_state_t the card was drawn for */
    unsigned drawn_seq;         /* frame count already on screen */
    Rect drawn_video;           /* destination rect of that picture */
    int  force_body;            /* geometry or mode changed */

    long long last_hud_ms;
    int  quit;
    int  was_down;
} PlayerApp;

/* ---- small helpers ------------------------------------------ */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static size_t utf8_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;                       /* stray continuation byte: step one */
}

static void txt(Surface *s, Font *f, int x, int y, const char *t, u32 col) {
    if (f && t && *t) font_draw(s, f, (float)x, (float)y, t, col);
}

static void txt_right(Surface *s, Font *f, int right, int y,
                      const char *t, u32 col) {
    if (!f || !t || !*t) return;
    int x = right - (int)(font_width(f, t) + 0.5f);
    font_draw(s, f, (float)x, (float)y, t, col);
}

/* Fit text into maxw by dropping whole UTF-8 sequences and appending
 * "..." - never by cutting a character in half. */
static void elide(char *out, size_t cap, const char *in, Font *f, int maxw) {
    float ell;
    size_t o = 0;

    if (!out || cap < 5) return;
    out[0] = 0;
    if (!in) return;
    if (!f || maxw <= 0) return;
    if (font_width(f, in) <= (float)maxw) {
        snprintf(out, cap, "%s", in);
        return;
    }
    ell = font_width(f, "...");
    for (size_t i = 0; in[i]; ) {
        size_t step = utf8_len(in + i);
        if (o + step + 1 >= cap) break;
        memcpy(out + o, in + i, step);
        out[o + step] = 0;
        if (font_width(f, out) + ell > (float)maxw) {
            out[o] = 0;
            break;
        }
        o += step;
        i += step;
    }
    snprintf(out + o, cap - o, "...");
}

static const char *leaf(const char *path) {
    const char *slash;
    if (!path) return "";
    slash = strrchr(path, '/');
#if defined(_WIN32)
    {
        const char *bs = strrchr(path, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
    }
#endif
    return slash ? slash + 1 : path;
}

/* Longest prefix of s[0..n) that fits maxw, cut at a UTF-8 boundary. */
static size_t fit_prefix(Font *f, const char *s, size_t n, int maxw) {
    char probe[640];
    size_t used = 0, i = 0;

    while (i < n) {
        size_t step = utf8_len(s + i);
        if (i + step > n) break;
        if (used + step >= sizeof(probe)) break;
        memcpy(probe, s, used);
        memcpy(probe + used, s + i, step);
        probe[used + step] = 0;
        if (font_width(f, probe) > (float)maxw) break;
        used += step;
        i += step;
    }
    return used;
}

/* Greedy word wrap: words are moved whole to the next line, and a word
 * wider than the column is cut at character boundaries. Returns the
 * number of lines actually drawn (maxlines cap). */
static int wrap_text(Surface *s, Font *f, int x, int y, int maxw, int lineh,
                     int maxlines, const char *text, u32 col) {
    char line[640];
    size_t ln = 0;
    int drawn = 0;
    const char *p = text;

    if (!f || !text || maxlines <= 0 || maxw <= 0) return 0;
    line[0] = 0;

    while (*p && drawn < maxlines) {
        const char *word;
        size_t wl;
        int placed = 0;

        while (*p == ' ') p++;
        if (!*p) break;
        word = p;
        while (*p && *p != ' ') p++;
        wl = (size_t)(p - word);

        while (!placed && drawn < maxlines) {
            if (ln == 0) {
                size_t take = fit_prefix(f, word, wl, maxw);
                if (take == 0) {
                    take = utf8_len(word);
                    if (take > wl) take = wl;
                }
                if (take > sizeof(line) - 2) take = sizeof(line) - 2;
                memcpy(line, word, take);
                line[take] = 0;
                ln = take;
                word += take;
                wl -= take;
                if (wl == 0) { placed = 1; break; }
                txt(s, f, x, y + drawn * lineh, line, col);
                drawn++;
                line[0] = 0;
                ln = 0;
            } else {
                char cand[1300];
                if (ln + 1 + wl < sizeof(cand)) {
                    memcpy(cand, line, ln);
                    cand[ln] = ' ';
                    memcpy(cand + ln + 1, word, wl);
                    cand[ln + 1 + wl] = 0;
                    if (font_width(f, cand) <= (float)maxw) {
                        memcpy(line, cand, ln + 1 + wl + 1);
                        ln += 1 + wl;
                        placed = 1;
                        break;
                    }
                }
                /* The word does not fit after this line: draw the line and
                 * retry the word at the left edge. */
                txt(s, f, x, y + drawn * lineh, line, col);
                drawn++;
                line[0] = 0;
                ln = 0;
            }
        }
    }

    if (ln && drawn < maxlines) {
        txt(s, f, x, y + drawn * lineh, line, col);
        drawn++;
    }
    return drawn;
}

static void layout_window(int w, int h, Layout *L) {
    int foot_y = h - FOOT_H;
    if (foot_y < HEAD_H + BODY_MIN_H) foot_y = HEAD_H + BODY_MIN_H;
    if (foot_y > h) foot_y = h;

    L->head.x = 0; L->head.y = 0; L->head.w = w;
    L->head.h = imin(HEAD_H, h);
    L->body.x = 0; L->body.y = HEAD_H; L->body.w = w;
    L->body.h = imax(0, foot_y - HEAD_H);
    L->foot.x = 0; L->foot.y = foot_y; L->foot.w = w;
    L->foot.h = imax(0, h - foot_y);
}

static const char *state_name(libvlc_state_t st) {
    switch (st) {
    case libvlc_NothingSpecial: return "idle";
    case libvlc_Opening:        return "opening";
    case libvlc_Buffering:      return "buffering";
    case libvlc_Playing:        return "playing";
    case libvlc_Paused:         return "paused";
    case libvlc_Stopped:        return "stopped";
    case libvlc_Ended:          return "ended";
    case libvlc_Error:          return "error";
    default:                    return "unknown";
    }
}

static u32 state_colour(libvlc_state_t st) {
    if (st == libvlc_Error) return C_WARN;
    if (st == libvlc_Ended || st == libvlc_Stopped) return C_DIM;
    return C_ACCENT;
}

/* "m:ss", or "h:mm:ss" past an hour. Negative/unknown becomes "--:--". */
static void fmt_time(char *out, size_t n, int64_t ms) {
    int64_t sec, h, m, s;
    if (ms < 0) { snprintf(out, n, "--:--"); return; }
    sec = ms / 1000;
    h = sec / 3600;
    m = (sec / 60) % 60;
    s = sec % 60;
    if (h > 0)
        snprintf(out, n, "%lld:%02lld:%02lld", (long long)h,
                 (long long)m, (long long)s);
    else
        snprintf(out, n, "%lld:%02lld", (long long)m, (long long)s);
}

/* ---- the three bands ---------------------------------------- */

static void draw_header(PlayerApp *a, Layout *L) {
    Font *t = a->f.title, *s = a->f.small;
    int title_y = L->head.y + (L->head.h - (int)font_height(t)) / 2;
    int name_right = L->head.w - 14;
    char name[128];

    surf_fill(&a->s, L->head.x, L->head.y, L->head.w, L->head.h, C_HEAD);
    txt(&a->s, t, 14, title_y, "Player", C_TEXT);

    {
        const char *what = a->pl.media_path[0] ? leaf(a->pl.media_path) : "";
        elide(name, sizeof(name), what, s, L->head.w / 2);
    }
    if (name[0])
        txt_right(&a->s, s, name_right,
                  L->head.y + (L->head.h - (int)font_height(s)) / 2,
                  name, C_DIM);

    surf_fill(&a->s, 0, L->head.y + L->head.h - 1, L->head.w, 1, C_LINE);
}

/* The card: one centred column, used for every honest failure. */
static void draw_card(PlayerApp *a, Layout *L) {
    Rect b = L->body;
    Font *t = a->f.title, *bdy = a->f.body, *sm = a->f.small;
    libvlc_state_t st = a->pl.mp ? a->pl.api.player_get_state(a->pl.mp)
                                 : libvlc_NothingSpecial;
    int x = b.x + 40;
    int w = b.w - 80;
    int lh = (int)(font_height(bdy) + 5);
    int y;

    surf_fill(&a->s, b.x, b.y, b.w, b.h, C_BG);

    if (!a->pl.lib_ok) {
        /* Title, then the package manager's own sentence, then the real
         * loader reason, then where we looked. */
        y = b.y + b.h / 2 - 96;
        txt(&a->s, t, x, y,
            a->pl.symbol_missing ? "This libVLC cannot be used"
                                 : "libVLC is not installed", C_WARN);
        y += (int)font_height(t) + 10;
        wrap_text(&a->s, bdy, x, y, w, lh, 2, VLC_MISSING_MSG, C_TEXT);
        y += lh * 2 + 8;
        {
            int dlh = (int)(font_height(sm) + 4);
            int dlines = wrap_text(&a->s, sm, x, y, w, dlh, 4,
                                   a->pl.lib_detail[0] ? a->pl.lib_detail
                                                       : "dlopen() returned no detail",
                                   C_DIM);
            y += dlines * dlh + 6;
        }
        {
            char looked[320];
            snprintf(looked, sizeof(looked), "looked in %s",
                     a->pl.lib_tried[1][0] ? a->pl.lib_tried[1] : "(no path)");
            if (a->pl.lib_tried[0][0]) {
                char first[320];
                snprintf(first, sizeof(first), "first tried %s",
                         a->pl.lib_tried[0]);
                txt(&a->s, sm, x, y, first, C_FAINT);
                y += (int)font_height(sm) + 4;
            }
            wrap_text(&a->s, sm, x, y, w, (int)(font_height(sm) + 4), 2,
                      looked, C_FAINT);
        }
    } else if (a->pl.inst_failed) {
        y = b.y + b.h / 2 - 70;
        txt(&a->s, t, x, y, "libVLC failed to start", C_WARN);
        y += (int)font_height(t) + 10;
        {
            int nl = wrap_text(&a->s, bdy, x, y, w, lh, 3,
                               a->pl.last_err[0] ? a->pl.last_err
                                                 : "libvlc_new() returned NULL",
                               C_TEXT);
            y += nl * lh + 6;
        }
        {
            char line[256];
            snprintf(line, sizeof(line), "libVLC %s loaded from %s",
                     a->pl.api.version ? a->pl.api.version() : "(unknown)",
                     a->pl.lib_path[0] ? a->pl.lib_path : "(unknown)");
            wrap_text(&a->s, sm, x, y, w, (int)(font_height(sm) + 4), 3,
                      line, C_DIM);
        }
    } else if (!a->pl.media_path[0]) {
        y = b.y + b.h / 2 - 60;
        txt(&a->s, t, x, y, "No media file", C_TEXT);
        y += (int)font_height(t) + 10;
        wrap_text(&a->s, bdy, x, y, w, lh, 2, VLC_NEED_FILE_MSG, C_TEXT);
        y += lh * 2 + 6;
        {
            char line[256];
            snprintf(line, sizeof(line), "libVLC %s loaded from %s",
                     a->pl.api.version ? a->pl.api.version() : "(unknown)",
                     a->pl.lib_path[0] ? a->pl.lib_path : "(unknown)");
            wrap_text(&a->s, sm, x, y, w, (int)(font_height(sm) + 4), 3,
                      line, C_DIM);
        }
    } else {
        /* Media given, library loaded, but no picture yet (or an error). */
        char line[640];
        const char *err = a->pl.last_err[0] ? a->pl.last_err
                       : (a->pl.api.errmsg ? a->pl.api.errmsg() : NULL);
        int lines = 0;
        /* Ended/Stopped with no picture is VLC 3's answer for a file it
         * could not open (a missing path lands here, not in libvlc_Error).
         * Saying "Opening" forever would be a lie. */
        int dead = (st == libvlc_Error || st == libvlc_Ended ||
                    st == libvlc_Stopped);

        y = b.y + b.h / 2 - 70;
        if (dead) {
            txt(&a->s, t, x, y, "libVLC did not play this file", C_WARN);
        } else if (a->pl.started) {
            char shown[640];
            snprintf(shown, sizeof(shown), "Opening %s", leaf(a->pl.media_path));
            elide(line, sizeof(line), shown, t, w);
            txt(&a->s, t, x, y, line, C_TEXT);
        } else {
            txt(&a->s, t, x, y, "Nothing is playing", C_TEXT);
        }
        y += (int)font_height(t) + 10;

        snprintf(line, sizeof(line),
                 "state: %s   video: %s", state_name(st),
                 a->pl.started ? "no picture decoded yet" : "not started");
        wrap_text(&a->s, bdy, x, y, w, lh, 2, line, C_DIM);
        y += lh * 2;

        if (err && *err) {
            lines = wrap_text(&a->s, sm, x, y, w,
                              (int)(font_height(sm) + 4), 4, err, C_WARN);
            y += lines * (int)(font_height(sm) + 4);
        } else if (dead) {
            lines = wrap_text(&a->s, sm, x, y, w,
                              (int)(font_height(sm) + 4), 3,
                              "libVLC reached that state without an error "
                              "message: the path may be missing or unreadable, "
                              "or the file is not something it can open.",
                              C_DIM);
            y += lines * (int)(font_height(sm) + 4);
        }
        if (a->pl.started && !dead) {
            wrap_text(&a->s, sm, x, y, w, (int)(font_height(sm) + 4), 2,
                      "If this file has no video track, there is nothing to "
                      "show; the sound, if any, still plays.", C_FAINT);
        }
    }

    txt(&a->s, sm, x, b.y + b.h - 24, "q quits.", C_FAINT);
    a->body_mode = 2;
    a->card_state = (int)st;
}

static void draw_video(PlayerApp *a, Layout *L, int full) {
    Rect b = L->body;
    Rect v;
    unsigned seq = 0;
    int copied = 0;

    fit_rect(a->pl.vid_w, a->pl.vid_h, b.w, b.h, &v.x, &v.y, &v.w, &v.h);
    v.x += b.x;
    v.y += b.y;

    if (full || a->body_mode != 1 ||
        v.x != a->drawn_video.x || v.y != a->drawn_video.y ||
        v.w != a->drawn_video.w || v.h != a->drawn_video.h) {
        /* Bars first: every body pixel is written by us, so a resize or a
         * change of aspect cannot leave stale rows behind. */
        surf_fill(&a->s, b.x, b.y, b.w, b.h, C_BAR);
        seq = surf_blit(&a->pl.surf, a->s.px, a->s.stride,
                        v.x, v.y, v.w, v.h);
        copied = seq != 0;
    } else if (a->drawn_seq != surf_frames(&a->pl.surf)) {
        seq = surf_blit(&a->pl.surf, a->s.px, a->s.stride,
                        v.x, v.y, v.w, v.h);
        copied = seq != 0;
    }

    if (copied) {
        a->drawn_seq = seq;
        a->drawn_video = v;
        a->body_mode = 1;
    } else if (a->body_mode != 1) {
        /* frames exist but the copy failed: do not claim a picture */
        draw_card(a, L);
        return;
    }
    (void)full;
}

static void draw_footer(PlayerApp *a, Layout *L) {
    Font *f = a->f.body, *sm = a->f.small;
    Rect r = L->foot;
    libvlc_state_t st = libvlc_NothingSpecial;
    int64_t pos = -1, len = -1;
    char tpos[32] = "--:--", tlen[32] = "--:--", line[256];
    int x = 14;
    int row1 = r.y + 5;
    int bar_y = r.y + 25;
    int row2 = r.y + 34;

    surf_fill(&a->s, r.x, r.y, r.w, r.h, C_PANEL);
    surf_fill(&a->s, 0, r.y, r.w, 1, C_LINE);

    if (a->pl.mp) {
        st = a->pl.api.player_get_state(a->pl.mp);
        pos = a->pl.api.player_get_time(a->pl.mp);
        len = a->pl.api.player_get_length(a->pl.mp);
    } else if (!a->pl.lib_ok) {
        st = libvlc_Error;
    }
    fmt_time(tpos, sizeof(tpos), pos);
    if (len > 0) fmt_time(tlen, sizeof(tlen), len);

    txt(&a->s, f, x, row1, state_name(st), state_colour(st));
    x += (int)(font_width(f, state_name(st)) + 0.5f) + 10;

    snprintf(line, sizeof(line), "%s / %s", tpos, len > 0 ? tlen : "--:--");
    txt(&a->s, a->f.mono, x, row1, line, C_TEXT);
    x += (int)(font_width(a->f.mono, line) + 0.5f) + 14;

    if (a->pl.vid_w > 0 && a->pl.vid_h > 0)
        snprintf(line, sizeof(line), "%dx%d", a->pl.vid_w, a->pl.vid_h);
    else
        snprintf(line, sizeof(line), "%dx%d buffer", a->pl.dec_w, a->pl.dec_h);
    txt(&a->s, sm, x, row1 + 3, line, C_DIM);

    /* Right side: volume, audio state, libVLC version. */
    {
        char right[192];
        size_t off = 0;
        right[0] = 0;
        if (a->pl.mp) {
            if (a->pl.audio_dead)
                off += (size_t)snprintf(right + off, sizeof(right) - off,
                                        "no audio output");
            else if (a->pl.vol_known)
                off += (size_t)snprintf(right + off, sizeof(right) - off,
                                        "vol %d", a->pl.vol);
            else
                off += (size_t)snprintf(right + off, sizeof(right) - off,
                                        "vol n/a");
        }
        if (off >= sizeof(right) - 1) off = sizeof(right) - 1;
        if (a->pl.lib_ok) {
            long long v = (long long)off;
            snprintf(right + off, sizeof(right) - off,
                     "%slibVLC %s", v ? "   " : "",
                     a->pl.api.version ? a->pl.api.version() : "?");
        }
        txt_right(&a->s, sm, r.w - 14, row1 + 3, right,
                  a->pl.audio_dead ? C_WARN : C_DIM);
    }

    /* Progress: position over duration; an unknown duration leaves the
     * track empty rather than inventing a fill. */
    surf_fill(&a->s, 14, bar_y, r.w - 28, 3, C_TRACK);
    if (pos >= 0 && len > 0) {
        int64_t filled = (int64_t)(r.w - 28) * pos / len;
        if (filled > r.w - 28) filled = r.w - 28;
        if (filled > 0)
            surf_fill(&a->s, 14, bar_y, (int)filled, 3, C_ACCENT);
    }

    snprintf(line, sizeof(line),
             "space play/pause   left/right %d s   up/down volume   q quit",
             SEEK_MS / 1000);
    txt(&a->s, sm, 14, row2, line, C_FAINT);
}

static void draw_all(PlayerApp *a) {
    Layout L;
    layout_window(a->win_w, a->win_h, &L);

    surf_clip(&a->s, 0, 0, a->win_w, a->win_h);
    surf_fill(&a->s, 0, 0, a->win_w, a->win_h, C_BG);
    draw_header(a, &L);
    a->body_mode = 0;
    a->force_body = 1;
    if (surf_frames(&a->pl.surf) > 0)
        draw_video(a, &L, 1);
    else
        draw_card(a, &L);
    draw_footer(a, &L);
    a->force_body = 0;               /* this pass painted the body */
    mc_present(&a->c);
}

static void draw_footer_only(PlayerApp *a) {
    Layout L;
    layout_window(a->win_w, a->win_h, &L);
    draw_footer(a, &L);
}

/* Returns 1 when the body was redrawn (the caller sends one FRAME). */
static int draw_body_if_needed(PlayerApp *a) {
    Layout L;
    layout_window(a->win_w, a->win_h, &L);
    if (surf_frames(&a->pl.surf) > 0) {
        if (a->force_body || a->drawn_seq != surf_frames(&a->pl.surf)) {
            draw_video(a, &L, a->force_body);
            a->force_body = 0;
            return 1;
        }
    } else {
        /* No picture yet: redraw the card when libVLC's state changes, so
         * "opening" turns into the real error or ended state on screen. */
        libvlc_state_t st = a->pl.mp ? a->pl.api.player_get_state(a->pl.mp)
                                     : libvlc_NothingSpecial;
        if (a->body_mode != 2 || a->card_state != (int)st) {
            draw_card(a, &L);
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 *  5. The window
 * ============================================================ */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

static void on_key(PlayerApp *a, const MEvent *ev) {
    if (!ev->press) return;
    switch (ev->key) {
    case KEY_Q:
    case KEY_ESC:
        a->quit = 1;
        break;
    case KEY_SPACE:
        vp_toggle_pause(&a->pl);
        a->last_hud_ms = 0;
        break;
    case KEY_LEFT:
        vp_seek(&a->pl, -SEEK_MS);
        a->last_hud_ms = 0;
        break;
    case KEY_RIGHT:
        vp_seek(&a->pl, SEEK_MS);
        a->last_hud_ms = 0;
        break;
    case KEY_UP:
        vp_volume(&a->pl, VOL_STEP);
        a->last_hud_ms = 0;
        break;
    case KEY_DOWN:
        vp_volume(&a->pl, -VOL_STEP);
        a->last_hud_ms = 0;
        break;
    default:
        break;
    }
}

static void on_mouse(PlayerApp *a, const MEvent *ev, Layout *L) {
    int down = ev->buttons & 1;
    int in_body = ev->x >= L->body.x && ev->x < L->body.x + L->body.w &&
                  ev->y >= L->body.y && ev->y < L->body.y + L->body.h;
    if (down && !a->was_down && in_body) {
        vp_toggle_pause(&a->pl);
        a->last_hud_ms = 0;
    }
    a->was_down = down;
}

#ifndef VLCPLAY_HOST_TEST
int main(int argc, char **argv) {
    PlayerApp a;
    const char *media = NULL;
    int dec_w, dec_h;
    long long last_size_ms = 0;

    memset(&a, 0, sizeof(a));

    if (mc_open(&a.c, "vlc", "Player", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "vlcplay: cannot connect to the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                       : "/run/mystic/wm.sock");
        return 1;
    }
    if (font_init("/usr/share/fonts") != 0) {
        fprintf(stderr, "vlcplay: no fonts in /usr/share/fonts\n");
        mc_close(&a.c);
        return 1;
    }
    a.f.title = font_get(FONT_SEMIBOLD, 15);
    a.f.body  = font_get(FONT_REGULAR,  11);
    a.f.small = font_get(FONT_MEDIUM,    9);
    a.f.mono  = font_get(FONT_MONO,     11);
    if (!a.f.title || !a.f.body || !a.f.small) {
        fprintf(stderr, "vlcplay: fonts failed to load\n");
        mc_close(&a.c);
        return 1;
    }

    a.win_w = a.c.maxw > 0 ? imin(WIN_W, a.c.maxw) : WIN_W;
    a.win_h = a.c.maxh > 0 ? imin(WIN_H, a.c.maxh) : WIN_H;
    a.s.px = a.c.px;
    a.s.w = a.win_w;
    a.s.h = a.win_h;
    a.s.stride = a.c.stride;

    if (argc > 1 && argv[1] && argv[1][0]) media = argv[1];

    /* Decode at the body size: a 16:9 file then needs no scaling here at
     * all. The width is a multiple of 8 so the pitch is 32-byte aligned,
     * which is what vmem's header asks for. */
    {
        Layout L;
        layout_window(a.win_w, a.win_h, &L);
        dec_w = (L.body.w / 8) * 8;
        if (dec_w < 16) dec_w = 16;
        if (dec_w > 1920) dec_w = 1920;
        dec_h = L.body.h & ~1;
        if (dec_h < 16) dec_h = 16;
        if (dec_h > 1080) dec_h = 1080;
    }

    if (vp_open(&a.pl, media, dec_w, dec_h) != 0) {
        fprintf(stderr, "vlcplay: cannot allocate the %dx%d frame surface\n",
                dec_w, dec_h);
        mc_close(&a.c);
        return 1;
    }
    if (!a.pl.lib_ok) {
        fprintf(stderr, "vlcplay: %s\n", VLC_MISSING_MSG);
        fprintf(stderr, "vlcplay: %s\n",
                a.pl.lib_detail[0] ? a.pl.lib_detail : "libVLC could not be loaded");
    }

    draw_all(&a);
    a.last_hud_ms = now_ms();

    while (!a.quit) {
        Layout L;
        MEvent ev;
        long long now;
        int want_footer = 0;

        layout_window(a.win_w, a.win_h, &L);

        while (mc_poll(&a.c, &ev)) {
            switch (ev.type) {
            case MC_CLOSE:
                a.quit = 1;
                break;
            case MC_RESIZE:
                a.win_w = ev.w < 1 ? 1 : imin(ev.w, a.c.maxw > 0 ? a.c.maxw : ev.w);
                a.win_h = ev.h < 1 ? 1 : imin(ev.h, a.c.maxh > 0 ? a.c.maxh : ev.h);
                a.s.w = a.win_w;
                a.s.h = a.win_h;
                draw_all(&a);
                want_footer = 0;
                break;
            case MC_KEY:
                on_key(&a, &ev);
                want_footer = 1;
                break;
            case MC_MOUSE:
                on_mouse(&a, &ev, &L);
                break;
            default:
                break;
            }
        }
        if (a.quit) break;

        now = now_ms();

        if (a.pl.mp) {
            /* Live facts: the video's own size once a picture exists, and
             * the time/state row a few times a second. */
            if (surf_frames(&a.pl.surf) > 0 &&
                (a.pl.vid_w == 0 || now - last_size_ms >= 1000)) {
                int oldw = a.pl.vid_w, oldh = a.pl.vid_h;
                vp_refresh_video_size(&a.pl);
                if (a.pl.vid_w != oldw || a.pl.vid_h != oldh)
                    a.force_body = 1;   /* the letterbox geometry changed */
                last_size_ms = now;
            }
            if (!a.pl.vol_init && a.pl.started &&
                surf_frames(&a.pl.surf) > 0) {
                vp_start_volume(&a.pl, 1); /* the audio output exists now */
                want_footer = 1;
            }
            if (a.pl.started &&
                now - a.last_hud_ms >= (a.pl.paused ? 250 : 100)) {
                want_footer = 1;
                a.last_hud_ms = now;
            }
        }

        /* One FRAME per pass, whichever bands changed. */
        if (want_footer) draw_footer_only(&a);
        if (draw_body_if_needed(&a) || want_footer) mc_present(&a.c);

        /* Sleep for the next frame, or lazily when nothing is moving. */
        if (a.pl.mp && a.pl.started) {
            libvlc_state_t st = a.pl.api.player_get_state(a.pl.mp);
            if (st == libvlc_Playing || st == libvlc_Opening ||
                st == libvlc_Buffering)
                usleep(8 * 1000);
            else
                usleep(80 * 1000);
        } else {
            usleep(100 * 1000);
        }
    }

    vp_close(&a.pl);
    mc_close(&a.c);
    return 0;
}
#endif /* !VLCPLAY_HOST_TEST */
