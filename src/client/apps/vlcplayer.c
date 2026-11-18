/* ============================================================
 *  Mystic OS - apps (external) / vlcplayer
 *
 *  A real video player: libVLC decodes, the vmem video output copies each
 *  decoded picture into memory this process owns, and that memory is the
 *  Mystic window. Nothing here draws an invented frame: if no picture has
 *  arrived the window says so, and if libVLC reports an error the exact
 *  libvlc_errmsg() string is shown.
 *
 *  Usage (argv[1] is the media path):
 *
 *      Player <media file>     (after: pkg install vlc)
 *
 *  THE FILES IN THE BUNDLE, AND WHY THERE ARE THREE OF THEM
 *  -------------------------------------------------------
 *  The bundle id is `vlc` everywhere: stage/usr/share/mystic/apps/vlc in
 *  the image, /root/apps/vlc after an install, the Store row `vlc`, the
 *  window's app id and the manifest all say the same word. There used to
 *  be a second id (`vlcplayer`) and a payload that could not load its own
 *  library; both are gone.
 *
 *      bin                      the static player: this file compiled with
 *                               -DVP_BOOT=1, no PT_INTERP, no DT_NEEDED.
 *                               It is the manifest's 'exec =', and it must
 *                               be a BARE FILE NAME: src/wmsrv.c
 *                               (wms_launch_arg) rejects any exec with a '/'
 *                               or '..' in it ("unsafe exec"), so
 *                               'exec = libexec/vlcplayer' is a layout this
 *                               OS cannot launch. libexec/ is internal: the
 *                               launcher execs it through the bundled loader.
 *                               The IMAGE has /lib/ld-musl-x86_64.so.1 from
 *                               its Alpine base rootfs, but the jail is a
 *                               chroot that binds only /bin/app, /app (this
 *                               bundle), the fonts, /dev bits and /proc - no
 *                               /lib. So inside the jail PT_INTERP cannot
 *                               resolve and the static launcher is the only
 *                               file the kernel can start.
 *      libexec/vlcplayer        the player: dynamic musl, PT_INTERP
 *                               /lib/ld-musl-x86_64.so.1, RUNPATH
 *                               $ORIGIN/../lib. dlopen()s libvlc.so.5.
 *      lib/ld-musl-x86_64.so.1  the musl 1.2.6 loader, copied out of
 *                               Alpine's musl-1.2.6-r3.apk (670,312 bytes,
 *                               mode 0755). musl's loader is itself a
 *                               program that takes --library-path
 *                               (ldso/dynlink.c, __dls3), so bin execs:
 *
 *                                 lib/ld-musl-x86_64.so.1 \
 *                                   --library-path <bundle>/lib \
 *                                   <bundle>/libexec/vlcplayer <args...>
 *
 *      lib/libvlc.so.5, libvlccore.so.9, ...   the real closure
 *      lib/vlc/plugins/...                     the plugins a profile needs
 *
 *  NO WMSRV CHANGE IS NEEDED for this to start. The window server already
 *  binds the bundle directory read-only at /app and exports
 *  MYSTIC_BUNDLE=/app, LD_LIBRARY_PATH=/app/usr/lib:/app/lib and
 *  HOME=/tmp; bin reads MYSTIC_BUNDLE first, then falls back to
 *  /proc/self/exe, and passes --library-path plus
 *  VLC_PLUGIN_PATH=<bundle>/lib/vlc/plugins itself. The player's RUNPATH
 *  ($ORIGIN/../lib) is the third, independent way to find the same files.
 *
 *  In the IMAGE (no libvlc: the playback closure is 100+ MB and is not
 *  staged) this same source is compiled statically and the window says
 *  exactly that. It is a placeholder, not a fake player - a static musl
 *  executable cannot dlopen at all, which the next paragraph proves.
 *
 *  WHY THE PLAYER IS NOT STATIC, PROVEN BY BUILDING BOTH WAYS
 *  ---------------------------------------------------------
 *  tools/fetch-vlc.py links it twice and reads the result instead of
 *  trusting a guess:
 *
 *    static + libvlc:  zig cc -target x86_64-linux-musl -static ...
 *                      ... -Lbuild/vlc-root/usr/lib vlcplayer.c -lvlc
 *                      -> error: unable to find static system library
 *                         "vlc" using strategy no_fallback. searched paths:
 *                           build/vlc-root/usr/lib\libvlc.a
 *                      (the real output of that link in .work/f5-vlc; the
 *                      closure ships libvlc.so.5 only, and VLC's own module
 *                      loader is a dlopen() user even if a libvlc.a existed)
 *    static - libvlc:  builds (this is the image placeholder), but
 *                      dlopen() is musl's weak stub, whose whole body is
 *                      __dl_seterr("Dynamic loading not supported")
 *                      (zig's libc/musl/src/ldso/dlopen.c). That string
 *                      is present in the static binary's bytes.
 *    dynamic:          builds, PT_INTERP /lib/ld-musl-x86_64.so.1,
 *                      DT_NEEDED libc.so only, RUNPATH $ORIGIN/../lib.
 *                      libvlc.so.5 is dlopen()ed at run time, so the
 *                      package stays relocatable and -lvlc never
 *                      appears on a link line.
 *
 *  So: dynamic player + the loader it needs, both inside the bundle. The
 *  exact symbols it resolves are listed in VpApi below and bound by name
 *  in vp_bind() - nothing is declared and left unresolved.
 *
 *  RV32 IS OUR PIXEL FORMAT - the claim, checked against both sources
 *  ------------------------------------------------------------------
 *  VLC 3.0.23 modules/video_output/vmem.c, Open(), for VLC_CODEC_RGB32:
 *      rmask = 0xff0000, gmask = 0x00ff00, bmask = 0x0000ff
 *  and the same three masks in src/misc/es_format.c video_format_FixRgb().
 *  A pixel's colour is therefore the low 24 bits 0xRRGGBB; the fourth
 *  byte is not part of the mask. On little-endian the bytes in memory are
 *  B, G, R, X.
 *
 *  src/mystic.h defines colours as 0xAARRGGBB; on little-endian those
 *  bytes are B, G, R, A. The compositor memcpy's each row and ignores the
 *  top byte (src/main.c, WIN_CLIENT branch). And the converter VLC
 *  actually runs for our test clip, modules/video_chroma/rv32.c, writes:
 *
 *      *(p_dst++) = p_src[2];   B
 *      *(p_dst++) = p_src[1];   G
 *      *(p_dst++) = p_src[0];   R
 *      *(p_dst++) = 0xff;       (Alpha)
 *
 *  so its output is 0xFFRRGGBB - the same u32 as an opaque Mystic colour,
 *  byte for byte. If some other converter leaves 0x00 in that byte the
 *  three colour bytes are still identical and the compositor still never
 *  reads the fourth. vp_blit() copies u32 values without touching any
 *  channel, which is exactly that fact and why there is no conversion
 *  code here. The proof run in docs/MULTIMEDIA.md reads back 0xFFRRGGBB
 *  and the exact B,G,R values of a generated AVI frame.
 *
 *  vmem IS A COPY, AND THAT IS THE DESIGN
 *  --------------------------------------
 *  vmem.c Prepare() does picture_CopyPixels(locked, pic) into the planes
 *  returned by our lock callback. So the planes are the frame surface:
 *  VLC's own internal copy is the only copy into it. We give it a
 *  MAXW x MAXH u32 surface (8.0 MB at 1920x1080) and scale from there into
 *  the window on the main thread, with a mutex that stops VLC writing
 *  while we read. Asking VLC for the window size instead and letting its
 *  chroma converter scale would need libswscale (~0.7 MB extra) and the
 *  i420_rgb converters refuse non-integer ratios; a nearest-neighbour
 *  scale here is ~30 lines and has no extra dependency.
 *
 *  Audio: libVLC's default audio output is left alone only when BOTH of
 *  the two things it needs exist: a PCM node under /dev/snd (the sandbox
 *  binds /dev/snd when the manifest says 'audio = yes'), and an ALSA
 *  output plugin in the bundle. The shipped closure has audio output
 *  plugins replaced by libadummy only - fetch-vlc.py's profile does not
 *  include libalsa_plugin.so - so on a machine WITH a sound card the
 *  honest answer is still "decoded, not played". The player tests both
 *  at startup, passes --aout=dummy once (never per frame or per media)
 *  when either is missing, and says which one in one line on stderr and
 *  permanently in the status bar. Volume still goes through
 *  libvlc_audio_set_volume(), which is VLC's software volume.
 *
 *  Hardware decoding is refused with --avcodec-hw=none (and the same option
 *  on the media). A vmem picture lives in system memory; a D3D11VA/VAAPI
 *  frame lives in the GPU, so hardware decode could never hand us a picture.
 *  VLC 3 still *probes* the hardware output formats before the software one,
 *  which on the Windows proof host left the vmem surface at a padded
 *  320x194 for a 320x176 file - vp_visible_size() asks libVLC for the real
 *  size and vp_blit() crops to it, so the padding is never presented.
 *
 *  Keys: space play/pause, Left/Right seek -/+10 s, Up/Down volume,
 *        s stop, f fit/fill scaling, q or Esc quit, click toggles play.
 * ============================================================ */

/* The two sentences the no-argument window can show, in ONE place.
 *
 * The window server starts a bundle with no argv, so the first run of the
 * Player tile is always this card. It must say which fact is true:
 *
 *   - libvlc.so.5 is nowhere this build looks: the image bundle is the
 *     static placeholder, so the fix is the package.
 *   - libVLC is there: the tile just needs a media file, and it says how.
 *
 * Both strings live here so there is no second copy to drift, and the
 * host harness compiles this file with -DVLC_TILE_MSG_TEST to print the
 * exact bytes the window draws (nothing else of the file is built then). */
#define VP_MSG_NO_LIBVLC \
    "libVLC is not installed - install it with: pkg install vlc"
#define VP_MSG_NEED_FILE \
    "give a media file: Player <path>"

/* vp_tile_hint() exists where a card can be drawn: the app build and the
 * harness build. The static launcher and the host decode test have no
 * window, so they do not carry it (and must not carry an unused static). */
#if defined(VLC_TILE_MSG_TEST) || \
    (!defined(VLC_HOST_TEST) && !(defined(VP_BOOT) && VP_BOOT))
static const char *vp_tile_hint(int have_file, int lib_ok) {
    if (have_file) return (const char *)0;  /* a real error is shown instead */
    return lib_ok ? VP_MSG_NEED_FILE : VP_MSG_NO_LIBVLC;
}
#endif

#if defined(VLC_TILE_MSG_TEST)
/* The harness build contains only vp_tile_hint() and its two strings. */
#else

/* VLC_HOST_TEST compiles only the libVLC/vmem core below, with no Mystic
 * client and no main(), so .work/vlc/host.c can include this exact file and
 * decode a real file on the host with the same callbacks the player uses.
 *
 * VP_BOOT=1 compiles the static launcher instead: no libVLC table, no vmem
 * surface, no window - just the bundle lookup and the exec of the bundled
 * loader (section 5). It is the only part of this file that runs in a jail
 * whose chroot has no /lib, so it must stay free of any dynamic
 * dependency. */
#ifdef VLC_HOST_TEST
#  define VP_APP 0
#else
#  define VP_APP 1
#endif

#if defined(VP_BOOT) && VP_BOOT
#  undef VP_APP
#  define VP_APP 0
#  define VP_BOOTING 1
#else
#  define VP_BOOTING 0
#endif

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
typedef HMODULE VpLibHandle;
#else
#  include <dlfcn.h>
typedef void *VpLibHandle;
#endif

#if VP_APP || VP_BOOTING
#  include <errno.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#if VP_APP
#  include "../mystic_client.h"
#  include "../../mystic.h"

#  include <dirent.h>
#  include <time.h>
#endif

/* Everything from here to the end of section 3 talks to libVLC. The static
 * launcher (VP_BOOTING) must not contain any of it: it cannot dlopen, and a
 * smaller binary is the whole point of that build. */
#if !VP_BOOTING

/* ============================================================
 *  1. libVLC 3 ABI, declared here on purpose
 *
 *  The build host has no vlc-dev headers and the compile check is a
 *  single -c with -Isrc, so the prototypes are transcribed from
 *  Alpine's vlc-dev 3.0.23 /usr/include/vlc/libvlc*.h. The struct
 *  layout of libvlc_media_track_t is copied field for field; only
 *  i_type and i_codec are read, but the compiler needs the right
 *  offsets to find them.
 * ============================================================ */

typedef int64_t VpTime;                 /* libvlc_time_t */

typedef struct libvlc_instance_t libvlc_instance_t;
typedef struct libvlc_media_t libvlc_media_t;
typedef struct libvlc_media_player_t libvlc_media_player_t;

/* libvlc_media_track_t, libvlc_media.h: enum is int, union holds pointers. */
typedef struct VpTrack {
    uint32_t i_codec;                   /* VLC fourcc, e.g. 'h264' */
    uint32_t i_original_fourcc;
    int      i_id;
    int      i_type;                    /* libvlc_track_video == 1 */
    int      i_profile;
    int      i_level;
    union {
        void *audio;
        void *video;
        void *subtitle;
    } u;
    unsigned i_bitrate;
    char    *psz_language;
    char    *psz_description;
} VpTrack;

/* The vmem callback prototypes, libvlc_media_player.h lines 307-377. */
typedef void *(*VpLockCb)(void *opaque, void **planes);
typedef void (*VpUnlockCb)(void *opaque, void *picture, void *const *planes);
typedef void (*VpDisplayCb)(void *opaque, void *picture);
typedef unsigned (*VpSetupCb)(void **opaque, char *chroma, unsigned *width,
                              unsigned *height, unsigned *pitches,
                              unsigned *lines);
typedef void (*VpCleanupCb)(void *opaque);

typedef struct VpApi {
    libvlc_instance_t *(*new_instance)(int argc, const char *const *argv);
    void (*release_instance)(libvlc_instance_t *);
    const char *(*errmsg)(void);
    const char *(*version)(void);

    libvlc_media_t *(*media_new_path)(libvlc_instance_t *, const char *);
    void (*media_add_option)(libvlc_media_t *, const char *);
    void (*media_release)(libvlc_media_t *);
    VpTime (*media_get_duration)(libvlc_media_t *);
    unsigned (*media_tracks_get)(libvlc_media_t *, VpTrack ***);
    void (*media_tracks_release)(VpTrack **, unsigned);
    const char *(*media_codec_description)(int, uint32_t);

    libvlc_media_player_t *(*player_new_from_media)(libvlc_media_t *);
    void (*player_release)(libvlc_media_player_t *);
    int (*player_play)(libvlc_media_player_t *);
    void (*player_set_pause)(libvlc_media_player_t *, int);
    void (*player_stop)(libvlc_media_player_t *);
    VpTime (*player_get_time)(libvlc_media_player_t *);
    void (*player_set_time)(libvlc_media_player_t *, VpTime);
    VpTime (*player_get_length)(libvlc_media_player_t *);
    int (*player_get_state)(libvlc_media_player_t *);
    int (*player_is_playing)(libvlc_media_player_t *);

    int (*audio_set_volume)(libvlc_media_player_t *, int);
    int (*audio_get_volume)(libvlc_media_player_t *);

    int (*video_get_size)(libvlc_media_player_t *, unsigned,
                          unsigned *, unsigned *);

    void (*video_set_callbacks)(libvlc_media_player_t *, VpLockCb, VpUnlockCb,
                                VpDisplayCb, void *);
    void (*video_set_format_callbacks)(libvlc_media_player_t *, VpSetupCb,
                                       VpCleanupCb);
} VpApi;

typedef struct VpPlayer {
    VpLibHandle lib;
    VpApi       api;
    libvlc_instance_t     *vlc;
    libvlc_media_t        *media;
    libvlc_media_player_t *mp;
    char missing[64];       /* first symbol that was not found */
} VpPlayer;

#if defined(_WIN32)
static VpLibHandle vp_lib_open(const char *path) {
    /* LOAD_WITH_ALTERED_SEARCH_PATH makes libvlc.dll find libvlccore.dll
     * beside it; LoadLibraryA would look only in the app directory/PATH. */
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *vp_lib_sym(VpLibHandle h, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(h, name);
}
static void vp_lib_close(VpLibHandle h) {
    if (h) FreeLibrary(h);
}
static void vp_lib_error(char *out, size_t n) {
    snprintf(out, n, "LoadLibrary failed (Windows error %lu)",
             (unsigned long)GetLastError());
}
#else
static VpLibHandle vp_lib_open(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *vp_lib_sym(VpLibHandle h, const char *name) {
    return dlsym(h, name);
}
static void vp_lib_close(VpLibHandle h) {
    if (h) dlclose(h);
}
static void vp_lib_error(char *out, size_t n) {
    const char *e = dlerror();
    snprintf(out, n, "%s", e ? e : "dlopen failed without dlerror() text");
}
#endif

/* Bind every symbol in VpApi. memcpy is the ISO-clean way to move a void*
 * into a function pointer; on x86-64 both are 8 bytes and the two targets
 * this builds for (Linux x86-64, Windows x64) are both LP64. */
static int vp_bind(VpApi *a, VpLibHandle h, char *missing, size_t mn) {
#define VP_FN(field, sym)                                                 \
    do {                                                                  \
        void *p_ = vp_lib_sym(h, sym);                                    \
        if (!p_) {                                                        \
            snprintf(missing, mn, "%s", sym);                             \
            return 0;                                                     \
        }                                                                 \
        memcpy(&a->field, &p_, sizeof(a->field));                         \
    } while (0)

    VP_FN(new_instance,           "libvlc_new");
    VP_FN(release_instance,       "libvlc_release");
    VP_FN(errmsg,                 "libvlc_errmsg");
    VP_FN(version,                "libvlc_get_version");
    VP_FN(media_new_path,         "libvlc_media_new_path");
    VP_FN(media_add_option,       "libvlc_media_add_option");
    VP_FN(media_release,          "libvlc_media_release");
    VP_FN(media_get_duration,     "libvlc_media_get_duration");
    VP_FN(media_tracks_get,       "libvlc_media_tracks_get");
    VP_FN(media_tracks_release,   "libvlc_media_tracks_release");
    VP_FN(media_codec_description, "libvlc_media_get_codec_description");
    VP_FN(player_new_from_media,  "libvlc_media_player_new_from_media");
    VP_FN(player_release,         "libvlc_media_player_release");
    VP_FN(player_play,            "libvlc_media_player_play");
    VP_FN(player_set_pause,       "libvlc_media_player_set_pause");
    VP_FN(player_stop,            "libvlc_media_player_stop");
    VP_FN(player_get_time,        "libvlc_media_player_get_time");
    VP_FN(player_set_time,        "libvlc_media_player_set_time");
    VP_FN(player_get_length,      "libvlc_media_player_get_length");
    VP_FN(player_get_state,       "libvlc_media_player_get_state");
    VP_FN(player_is_playing,      "libvlc_media_player_is_playing");
    VP_FN(audio_set_volume,       "libvlc_audio_set_volume");
    VP_FN(audio_get_volume,       "libvlc_audio_get_volume");
    VP_FN(video_get_size,         "libvlc_video_get_size");
    VP_FN(video_set_callbacks,    "libvlc_video_set_callbacks");
    VP_FN(video_set_format_callbacks, "libvlc_video_set_format_callbacks");
#undef VP_FN
    return 1;
}

/* ============================================================
 *  2. The vmem surface
 *
 *  One frame surface plus a mutex. VLC's lock callback waits until the
 *  main thread has finished reading, sets busy, and points planes[0] at
 *  fb. The unlock callback clears busy and marks a frame ready; display
 *  (called on the playback clock) counts presentations. vp_blit() runs on
 *  the main thread, waits for busy to clear, scales into the window while
 *  holding busy, then releases. No VLC thread ever writes the window.
 * ============================================================ */

#define VP_MAXW 1920
#define VP_MAXH 1080

typedef struct VpVmem {
    uint32_t *fb;              /* VP_MAXW * VP_MAXH, u32 RV32 */
    int       maxw, maxh;

    int       w, h;            /* dimensions VLC asked for, after setup() */
    int       visw, vish;      /* visible picture inside those, from VLC */
    int       pitch;           /* bytes per row VLC writes */
    int       too_big;         /* decoder output did not fit the surface */

    int       ready;           /* a decoded picture is waiting */
    int       busy;            /* 1 while either side owns fb */

    unsigned  frames;          /* pictures VLC copied into fb */
    unsigned  displays;        /* display callbacks VLC made */
    unsigned  setups;          /* format callbacks */

    pthread_mutex_t m;
    pthread_cond_t  cv;
} VpVmem;

static int vp_vmem_init(VpVmem *v, int maxw, int maxh) {
    memset(v, 0, sizeof(*v));
    if (maxw <= 0 || maxh <= 0) return -1;
    v->maxw = maxw;
    v->maxh = maxh;
    /* 4 bytes per pixel, and the multiplication cannot overflow: both
     * factors are bounded by the two constants above. */
    v->fb = (uint32_t *)calloc((size_t)maxw * (size_t)maxh, sizeof(uint32_t));
    if (!v->fb) return -1;
    if (pthread_mutex_init(&v->m, NULL) != 0) {
        free(v->fb);
        v->fb = NULL;
        return -1;
    }
    if (pthread_cond_init(&v->cv, NULL) != 0) {
        pthread_mutex_destroy(&v->m);
        free(v->fb);
        v->fb = NULL;
        return -1;
    }
    return 0;
}

static void vp_vmem_free(VpVmem *v) {
    if (!v) return;
    pthread_cond_destroy(&v->cv);
    pthread_mutex_destroy(&v->m);
    free(v->fb);
    v->fb = NULL;
}

/* planes[0] is the frame surface. The header asks for 32-byte-aligned
 * planes and recommends 32-byte pitches; calloc gives at least 16-byte
 * alignment and maxw is a multiple of 8, so the pitch below is too. */
static void *vp_cb_lock(void *opaque, void **planes) {
    VpVmem *v = (VpVmem *)opaque;
    pthread_mutex_lock(&v->m);
    while (v->busy) pthread_cond_wait(&v->cv, &v->m);
    v->busy = 1;
    pthread_mutex_unlock(&v->m);
    planes[0] = v->fb;
    return v;
}

static void vp_cb_unlock(void *opaque, void *picture, void *const *planes) {
    VpVmem *v = (VpVmem *)opaque;
    (void)picture;
    (void)planes;
    pthread_mutex_lock(&v->m);
    v->busy = 0;
    v->ready = 1;
    v->frames++;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->m);
}

static void vp_cb_display(void *opaque, void *picture) {
    VpVmem *v = (VpVmem *)opaque;
    (void)picture;
    pthread_mutex_lock(&v->m);
    v->displays++;
    pthread_mutex_unlock(&v->m);
}

/* vmem.c calls this with the decoder/filter output format. Returning the
 * same width and height asks for no scaling; only the chroma changes to
 * RV32. The return value is "number of pictures", and vmem treats 0 as
 * failure - so a video larger than our surface fails loudly instead of
 * being silently cropped. */
static unsigned vp_cb_setup(void **opaque, char *chroma, unsigned *width,
                            unsigned *height, unsigned *pitches,
                            unsigned *lines) {
    VpVmem *v = (VpVmem *)*opaque;
    unsigned w = *width, h = *height;

#if defined(VP_TRACE)
    fprintf(stderr, "vlcplayer: vmem setup %u asks for %ux%u (chroma %.4s)\n",
            v ? v->setups : 0u, w, h, chroma);
#endif
    if (!v || !v->fb) return 0;
    if (w < 2 || h < 2 || w > (unsigned)v->maxw || h > (unsigned)v->maxh) {
        v->too_big = 1;
        return 0;
    }
    /* RV32: 0x00RRGGBB. See the header comment for the masks. */
    memcpy(chroma, "RV32", 4);
    *width = w;
    *height = h;
    pitches[0] = ((w + 7u) & ~7u) * 4u;     /* >= w*4, multiple of 32 */
    lines[0] = h;

    pthread_mutex_lock(&v->m);
    v->w = (int)w;
    v->h = (int)h;
    v->pitch = (int)pitches[0];
    /* a new format invalidates the visible rectangle the previous one had:
     * VLC asks again for each candidate it probes, and a stale crop from the
     * hardware attempt would be wrong for the software format that wins. */
    v->visw = 0;
    v->vish = 0;
    v->setups++;
    pthread_mutex_unlock(&v->m);
    return 1;
}

static void vp_cb_cleanup(void *opaque) {
    (void)opaque;
}

/* The visible rectangle inside the vmem surface. vmem.c uses
 * video_format_ApplyRotation(vd->fmt) and hands us i_width/i_height, and it
 * then sets i_visible_width = i_width - so the two agree for a plain frame.
 * They do NOT always agree: a hardware-tried decoder (or a file whose coded
 * size is padded to a macroblock edge) reaches vmem with the padded height,
 * e.g. 320x194 for a 320x176 H.264 file. libvlc_video_get_size() is the
 * number VLC itself would show in its own UI, so the blit is cropped to it
 * instead of presenting padding rows as picture. 0 keeps the surface size. */
static void vp_visible_size(VpPlayer *p, VpVmem *v) {
    unsigned w = 0, h = 0;
    int visw = 0, vish = 0;

    if (!p || !p->mp || !p->api.video_get_size) return;
    if (p->api.video_get_size(p->mp, 0, &w, &h) == 0 && w >= 2 && h >= 2) {
        visw = (int)w;
        vish = (int)h;
    }
    pthread_mutex_lock(&v->m);
    if (visw > 0 && vish > 0 && visw <= v->w && vish <= v->h) {
        v->visw = visw;
        v->vish = vish;
    }
    pthread_mutex_unlock(&v->m);
}

static unsigned vp_frame_count(VpVmem *v) {
    unsigned n;
    pthread_mutex_lock(&v->m);
    n = v->frames;
    pthread_mutex_unlock(&v->m);
    return n;
}

/* Fill an axis-aligned rectangle of the destination; used for letterbox
 * bars so every window pixel is written by us, never left stale. */
static void vp_fill_rect(uint32_t *dst, int dw, int dh, int dstride,
                         int x, int y, int w, int h, uint32_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dw) w = dw - x;
    if (y + h > dh) h = dh - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t *row = dst + (size_t)yy * (size_t)dstride + x;
        for (int xx = 0; xx < w; xx++) row[xx] = colour;
    }
}

/* Blit the newest decoded picture into dst (window pixels), letterboxed
 * when fit=1 and stretched to the whole window when fit=0. Returns 1 if a
 * picture existed. The scale is nearest-neighbour in 16.16 fixed point;
 * the 1:1 path is a row memcpy, which is the only path a same-size video
 * takes. All channels are copied as-is: RV32 and Mystic pixels share the
 * low 24 bits, and the compositor ignores the top byte (see the top of
 * this file). */
static int vp_blit(VpVmem *v, uint32_t *dst, int dw, int dh, int dstride,
                   int fit) {
    int sw, sh, spitch;
    uint32_t *src;

    if (!v || !v->fb || !dst || dw <= 0 || dh <= 0 || dstride < dw) return 0;

    pthread_mutex_lock(&v->m);
    while (v->busy) pthread_cond_wait(&v->cv, &v->m);
    if (v->w <= 0 || v->h <= 0) {
        pthread_mutex_unlock(&v->m);
        return 0;
    }
    sw = v->w;
    sh = v->h;
    if (v->visw >= 2 && v->visw <= sw) sw = v->visw;
    if (v->vish >= 2 && v->vish <= sh) sh = v->vish;
    spitch = v->pitch;
    src = v->fb;
    v->busy = 1;               /* VLC's lock waits while we read fb */
    v->ready   = 0;            /* this presentation consumes the picture */
    pthread_mutex_unlock(&v->m);

    /* Destination rectangle. Integer arithmetic only: for a 1920-wide
     * source and a 960-wide window this is exact, and for odd sizes it
     * rounds down and leaves at most one black column. */
    int tw = dw, th = dh, ox = 0, oy = 0;
    if (fit) {
        if ((int64_t)sw * dh >= (int64_t)sh * dw) {
            tw = dw;
            th = (int)(((int64_t)sh * dw) / sw);
        } else {
            th = dh;
            tw = (int)(((int64_t)sw * dh) / sh);
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        ox = (dw - tw) / 2;
        oy = (dh - th) / 2;
    }

    if (ox > 0)    vp_fill_rect(dst, dw, dh, dstride, 0, 0, ox, dh, 0xFF000000u);
    if (ox + tw < dw)
        vp_fill_rect(dst, dw, dh, dstride, ox + tw, 0, dw - (ox + tw), dh, 0xFF000000u);
    if (oy > 0)    vp_fill_rect(dst, dw, dh, dstride, 0, 0, dw, oy, 0xFF000000u);
    if (oy + th < dh)
        vp_fill_rect(dst, dw, dh, dstride, 0, oy + th, dw, dh - (oy + th), 0xFF000000u);

    if (tw == sw && th == sh) {
        for (int y = 0; y < sh; y++) {
            memcpy(dst + (size_t)(oy + y) * (size_t)dstride + ox,
                   (const uint8_t *)src + (size_t)y * (size_t)spitch,
                   (size_t)sw * 4u);
        }
    } else {
        uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / (uint64_t)tw);
        for (int y = 0; y < th; y++) {
            int sy = (int)(((uint64_t)y * (uint64_t)sh) / (uint64_t)th);
            const uint32_t *srow =
                (const uint32_t *)((const uint8_t *)src + (size_t)sy * (size_t)spitch);
            uint32_t *drow = dst + (size_t)(oy + y) * (size_t)dstride + ox;
            uint32_t fx = 0;
            for (int x = 0; x < tw; x++) {
                drow[x] = srow[fx >> 16];
                fx += xstep;
            }
        }
    }

    pthread_mutex_lock(&v->m);
    v->busy = 0;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->m);
    return 1;
}

/* ============================================================
 *  3. Player lifecycle
 * ============================================================ */

static int vp_player_open(VpPlayer *p, VpVmem *v, const char *libpath,
                          const char *path, int no_audio,
                          char *err, size_t errn) {
    const char *args[4];
    int nargs = 0;

    memset(p, 0, sizeof(*p));
    if (err && errn) err[0] = 0;
    if (!libpath || !*libpath) libpath = "libvlc.so.5";

    p->lib = vp_lib_open(libpath);
    if (!p->lib) {
        char why[256];
        vp_lib_error(why, sizeof(why));
        snprintf(err, errn, "cannot load %s: %s", libpath, why);
        return -1;
    }
    if (!vp_bind(&p->api, p->lib, p->missing, sizeof(p->missing))) {
        snprintf(err, errn, "%s does not export %s (libVLC 3 is required)",
                 libpath, p->missing);
        vp_lib_close(p->lib);
        p->lib = NULL;
        return -1;
    }

    /* Only options that exist in libVLC 3. --no-video-title-show keeps its
     * OSD out of our surface. --avcodec-hw=none is not a preference: vmem
     * pictures are system memory, so a hardware decoder's GPU frame could
     * never be handed to us. --aout=dummy is added exactly once, and only
     * when there is no PCM node or no ALSA plugin to write through. */
    args[nargs++] = "--no-video-title-show";
    args[nargs++] = "--avcodec-hw=none";
    if (no_audio) args[nargs++] = "--aout=dummy";

    p->vlc = p->api.new_instance(nargs, args);
    if (!p->vlc) {
        const char *e = p->api.errmsg();
        snprintf(err, errn, "libvlc_new() failed: %s",
                 e && *e ? e : "no libvlc_errmsg() detail");
        vp_lib_close(p->lib);
        p->lib = NULL;
        return -1;
    }

    p->media = p->api.media_new_path(p->vlc, path);
    if (!p->media) {
        const char *e = p->api.errmsg();
        snprintf(err, errn, "libvlc_media_new_path(%s) failed: %s", path,
                 e && *e ? e : "no libvlc_errmsg() detail");
        p->api.release_instance(p->vlc);
        p->vlc = NULL;
        vp_lib_close(p->lib);
        p->lib = NULL;
        return -1;
    }

    /* Hardware decoding is impossible with vmem: its pictures are system
     * memory, and a DXVA/VAAPI frame is a GPU surface that vmem.c's
     * picture_CopyPixels() cannot copy into planes[0]. The instance option
     * above refuses it - on the proof host VLC's log changes from "Using
     * D3D11VA ... for hardware decoding" to "looking for hw decoder module
     * matching \"none\" / no hw decoder modules matched". This per-media
     * copy of the same rule keeps it attached to the media even if the
     * instance is reused.
     *
     * What it does NOT do is stop VLC 3 from probing the two hardware output
     * formats first: vmem setup() is still called three times on a GPU host,
     * with chroma DX11 and DXA9 before I420, and the third call is the one
     * that sticks (at a padded 320x194 for a 320x176 file). That is why
     * vp_visible_size() exists and why vp_blit() crops: the padding is VLC's,
     * not the file's. */
    p->api.media_add_option(p->media, ":avcodec-hw=none");

    p->mp = p->api.player_new_from_media(p->media);
    if (!p->mp) {
        const char *e = p->api.errmsg();
        snprintf(err, errn, "libvlc_media_player_new_from_media() failed: %s",
                 e && *e ? e : "no libvlc_errmsg() detail");
        p->api.media_release(p->media);
        p->api.release_instance(p->vlc);
        p->media = NULL;
        p->vlc = NULL;
        vp_lib_close(p->lib);
        p->lib = NULL;
        return -1;
    }

    /* libvlc_video_set_callbacks() also does var_SetString(mp,"vout","vmem")
     * in lib/media_player.c, so no --vout option is needed. */
    p->api.video_set_callbacks(p->mp, vp_cb_lock, vp_cb_unlock,
                               vp_cb_display, v);
    p->api.video_set_format_callbacks(p->mp, vp_cb_setup, vp_cb_cleanup);
    return 0;
}

static void vp_player_close(VpPlayer *p) {
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
    if (p->vlc) {
        p->api.release_instance(p->vlc);
        p->vlc = NULL;
    }
    if (p->lib) {
        vp_lib_close(p->lib);
        p->lib = NULL;
    }
}

/* The codec VLC itself reports for the first video track. The description
 * table is static, so copying it into out is enough. Returns 1 if a video
 * track existed. */
static int vp_query_codec(VpPlayer *p, char *out, size_t n) {
    VpTrack **tracks = NULL;
    unsigned count;
    int found = 0;

    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!p || !p->mp || !p->media) return 0;

    count = p->api.media_tracks_get(p->media, &tracks);
    if (!tracks || count == 0) return 0;

    for (unsigned i = 0; i < count; i++) {
        if (!tracks[i] || tracks[i]->i_type != 1) continue;   /* video */
        const char *desc = p->api.media_codec_description(1, tracks[i]->i_codec);
        if (desc && *desc) {
            snprintf(out, n, "%s", desc);
        } else {
            uint32_t c = tracks[i]->i_codec;
            char fourcc[5];
            fourcc[0] = (char)(c & 0xFF);
            fourcc[1] = (char)((c >> 8) & 0xFF);
            fourcc[2] = (char)((c >> 16) & 0xFF);
            fourcc[3] = (char)((c >> 24) & 0xFF);
            fourcc[4] = 0;
            for (int k = 0; k < 4; k++)
                if (fourcc[k] < 32 || fourcc[k] > 126) fourcc[k] = '.';
            snprintf(out, n, "%s (fourcc)", fourcc);
        }
        found = 1;
        break;
    }
    p->api.media_tracks_release(tracks, count);
    return found;
}

#endif /* !VP_BOOTING */

/* ============================================================
 *  4. The Mystic window application
 * ============================================================ */
#if VP_APP

#define VP_WIN_W 960
#define VP_WIN_H 600

/* evdev codes, forwarded verbatim by the window server. */
#define KEY_ESC   1
#define KEY_Q    16
#define KEY_S    31
#define KEY_F    33
#define KEY_SPACE 57
#define KEY_UP   103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108

#define VP_SEEK_MS 10000

typedef struct VpApp {
    VpPlayer pl;
    VpVmem   vmem;

    char   path[PATH_MAX];
    char   name[256];
    char   err[512];
    char   codec[96];
    char   libver[64];
    int    have_file;
    int    player_ok;
    int    lib_ok;           /* libvlc.so.5 exists where this build looks */
    int    font_ok;

    int    fit;              /* 1 = letterbox, 0 = stretch */
    int    vol;
    int    audio_device;     /* /dev/snd/pcm* exists */
    int    audio_plugin;     /* an ALSA output plugin exists for libVLC */
    int    no_audio_once;    /* we already said it */
    int    queried_codec;
    long   codec_try_at;
    int    last_state;
    VpTime pos, len;
    int    vw, vh;
    unsigned last_frames;
} VpApp;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void fmt_time(char *b, size_t n, VpTime ms) {
    if (ms < 0) {
        snprintf(b, n, "--:--");
        return;
    }
    int64_t s = ms / 1000;
    if (s >= 3600)
        snprintf(b, n, "%lld:%02lld:%02lld", (long long)(s / 3600),
                 (long long)((s / 60) % 60), (long long)(s % 60));
    else
        snprintf(b, n, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
}

static const char *state_name(int s) {
    switch (s) {
    case 0: return "idle";
    case 1: return "opening";
    case 2: return "buffering";
    case 3: return "playing";
    case 4: return "paused";
    case 5: return "stopped";
    case 6: return "ended";
    case 7: return "error";
    default: return "unknown";
    }
}

/* Does a PCM node exist? The image stages no sound driver and QEMU is
 * started without a sound device, so this is normally false; the player
 * then forces --aout=dummy once instead of letting ALSA fail per media. */
static int audio_device_present(void) {
    DIR *d = opendir("/dev/snd");
    int found = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strncmp(e->d_name, "pcmC", 4)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Canonical path of this executable, or "" if /proc is unavailable. */
static void self_dir(char *out, size_t n) {
    char exe[PATH_MAX];
    ssize_t k;
    out[0] = 0;
    k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) return;
    exe[k] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = 0;
    snprintf(out, n, "%s", exe);
}

/* The bundle directory. $MYSTIC_BUNDLE wins when the window server set it
 * (wmsrv binds that directory read-only at /app and exports the variable),
 * then the directory above .../libexec, then the directory the executable
 * is in. The static launcher in section 5 uses the same rule, so both entry
 * points agree on where lib/, libexec/ and the plugins are. Returns 0 when
 * /proc is unavailable and the environment says nothing. */
static int bundle_dir(char *out, size_t n) {
    char dir[PATH_MAX];
    struct stat st;
    const char *env = getenv("MYSTIC_BUNDLE");

    if (out == NULL || n == 0) return 0;
    out[0] = 0;
    if (env && *env && stat(env, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    self_dir(dir, sizeof(dir));
    if (!dir[0]) return 0;
    char *slash = strrchr(dir, '/');
    if (slash && !strcmp(slash + 1, "libexec")) *slash = 0;
    snprintf(out, n, "%s", dir);
    return 1;
}

/* There is deliberately no re-exec here any more. musl reads
 * LD_LIBRARY_PATH before main(), and this program is started either by the
 * static launcher (which passes --library-path=<bundle>/lib to the bundled
 * loader) or by the kernel with RUNPATH $ORIGIN/../lib baked in, so both
 * routes already give dlopen() the right search path. A re-exec would have
 * to exec /proc/self/exe, which inside the jail names a file whose
 * PT_INTERP (/lib/ld-musl-x86_64.so.1) does not exist there. */

/* Where to load libvlc from. $VLC_LIBRARY wins (a test hook, and honest
 * for a user with a nonstandard install); then the bundle's lib/ (the
 * package layout), then the image's /usr/lib; then the bare soname, which
 * lets the loader's --library-path / RUNPATH search finish the job. */
static void find_libvlc(char *out, size_t n) {
    char dir[PATH_MAX], probe[PATH_MAX];
    const char *env = getenv("VLC_LIBRARY");

    if (env && *env) {
        snprintf(out, n, "%s", env);
        return;
    }
    if (bundle_dir(dir, sizeof(dir))) {
        snprintf(probe, sizeof(probe), "%s/lib/libvlc.so.5", dir);
        if (access(probe, R_OK) == 0) {
            snprintf(out, n, "%s", probe);
            return;
        }
        /* an image install may put libvlc beside the executable instead */
        snprintf(probe, sizeof(probe), "%s/libvlc.so.5", dir);
        if (access(probe, R_OK) == 0) {
            snprintf(out, n, "%s", probe);
            return;
        }
    }
    if (access("/usr/lib/libvlc.so.5", R_OK) == 0) {
        snprintf(out, n, "/usr/lib/libvlc.so.5");
        return;
    }
    snprintf(out, n, "libvlc.so.5");
}

/* Does libVLC have an audio output it can actually open? Two things are
 * needed and they are independent: a PCM node (audio_device_present()) and
 * the plugin that opens it. VLC 3 on Linux reaches ALSA only through
 * vlc/plugins/audio_output/libalsa_plugin.so, and the shipped closure in
 * this repository replaces the audio outputs with libadummy only (see
 * tools/fetch-vlc.py's plugin profile). Saying "default output" when the
 * plugin is absent would be a lie the user hears as silence, so the bundle
 * check is explicit - and a system install is accepted too, because a
 * distribution's own vlc package really does ship it (Alpine keeps the file
 * in vlc-3.0.23-r4.apk; that package is not in this bundle's closure). */
static int audio_plugin_present(void) {
    static const char *const rel =
        "lib/vlc/plugins/audio_output/libalsa_plugin.so";
    char dir[PATH_MAX], probe[PATH_MAX];

    if (bundle_dir(dir, sizeof(dir)) && dir[0]) {
        snprintf(probe, sizeof(probe), "%s/%s", dir, rel);
        if (access(probe, R_OK) == 0) return 1;
    }
    if (access("/usr/lib/vlc/plugins/audio_output/libalsa_plugin.so", R_OK) == 0)
        return 1;
    if (access("/usr/lib/x86_64-linux-gnu/vlc/plugins/audio_output/"
               "libalsa_plugin.so", R_OK) == 0)
        return 1;
    return 0;
}

/* Is libVLC actually on this disk? The no-argument card must not say "not
 * installed" when the package is present, and must not ask for a media file
 * on an image that has no libVLC at all. The test is the same find_libvlc()
 * the player uses, so the sentence matches what would happen if a file were
 * given (a bare soname that only a system loader cache could resolve counts
 * as absent here - that is the honest answer inside this jail). */
static int libvlc_present(void) {
    char lib[PATH_MAX];
    find_libvlc(lib, sizeof(lib));
    return lib[0] == '/' && access(lib, R_OK) == 0;
}

/* Draw text clipped to maxw, with an ellipsis when it had to be cut. The
 * cut is made on a UTF-8 codepoint boundary, and the ellipsis itself is
 * measured, so the result is never wider than maxw. */
static void draw_clipped(Surface *s, Font *f, int x, int y, int maxw,
                         const char *text, u32 colour) {
    char tmp[512], cut[520];
    if (!f || !text || maxw <= 0) return;
    snprintf(tmp, sizeof(tmp), "%s", text);
    if (font_width(f, tmp) <= (float)maxw) {
        font_draw(s, f, (float)x, (float)y, tmp, colour);
        return;
    }
    size_t len = strlen(tmp);
    while (len > 0) {
        len--;
        while (len > 0 && (((unsigned char)tmp[len]) & 0xC0) == 0x80) len--;
        tmp[len] = 0;
        snprintf(cut, sizeof(cut), "%s...", tmp);
        if (font_width(f, cut) <= (float)maxw) {
            font_draw(s, f, (float)x, (float)y, cut, colour);
            return;
        }
    }
    /* not even one glyph plus the ellipsis fits: leave the line empty */
}

/* Draw a paragraph wrapped at spaces, clipped to maxw. Returns the y after
 * the last line. Used for the real libvlc error string, which can be long. */
static int draw_para(Surface *s, Font *f, int x, int y, int maxw, int maxlines,
                     const char *text, u32 colour) {
    char line[512];
    size_t ll = 0;
    int lines = 0;
    const char *p = text;

    if (!f || !text) return y;
    while (*p && lines < maxlines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *w = p;
        while (*p && *p != ' ') p++;
        size_t wl = (size_t)(p - w);
        if (wl > sizeof(line) - 2) wl = sizeof(line) - 2;   /* one huge token */
        if (ll + (ll ? 1 : 0) + wl >= sizeof(line)) {
            line[ll] = 0;
            draw_clipped(s, f, x, y, maxw, line, colour);
            y += (int)font_height(f) + 2;
            lines++;
            ll = 0;
            if (lines >= maxlines) break;
        }
        if (ll) line[ll++] = ' ';
        memcpy(line + ll, w, wl);
        ll += wl;
    }
    if (ll && lines < maxlines) {
        line[ll] = 0;
        draw_clipped(s, f, x, y, maxw, line, colour);
        y += (int)font_height(f) + 2;
    }
    return y;
}

static void draw_error_screen(MClient *c, VpApp *a, Font *f, u32 bg) {
    Surface s;
    memset(&s, 0, sizeof(s));
    s.px = c->px;
    s.w = c->w;
    s.h = c->h;
    s.stride = c->stride;
    surf_clip(&s, 0, 0, c->w, c->h);
    surf_fill(&s, 0, 0, c->w, c->h, bg);

    Font *title = font_get(FONT_SEMIBOLD, 17);
    Font *body = font_get(FONT_REGULAR, 11);
    int x = 34, y = 30;
    int maxw = c->w - 68;

    /* With no argv - which is how the window server starts the tile - this is
     * not an error screen. It states which fact is true: libVLC is missing
     * (the image bundle is the static placeholder), or libVLC is there and the
     * Player needs a file. Both sentences come from vp_tile_hint(), the one
     * definition the host harness prints verbatim. Nothing here invents a
     * path: the old card printed Player <media file> on a
     * machine that had neither, and never mentioned the missing library. */
    const char *hint = vp_tile_hint(a->have_file, a->lib_ok);

    if (hint) {
        font_draw(&s, title, (float)x, (float)y,
                  a->lib_ok ? "VLC Player" : "libVLC is not installed",
                  a->lib_ok ? RGB(235, 240, 248) : RGB(255, 150, 150));
        y += (int)font_height(title) + 14;
        y = draw_para(&s, body, x, y, maxw, 3, hint,
                      a->lib_ok ? RGB(238, 243, 250) : RGB(255, 196, 196));
        y += 8;
        if (!a->lib_ok) {
            y = draw_para(&s, body, x, y, maxw, 6,
                          "This bundle is the image's placeholder: it is a static "
                          "musl program and cannot dlopen, so it could not load "
                          "libVLC even if one were present. The package supplies "
                          "the dynamic player, the loader and the whole closure "
                          "inside the bundle.",
                          RGB(150, 160, 175));
        } else {
            y = draw_para(&s, body, x, y, maxw, 4,
                          "Open a media file from Files, or start the Player with "
                          "the file's path as the first argument.",
                          RGB(150, 160, 175));
        }
        y += 12;
        font_draw(&s, body, (float)x, (float)y, "q quits.", RGB(150, 160, 175));
        mc_present(c);
        (void)f;
        (void)bg;
        return;
    }

    font_draw(&s, title, (float)x, (float)y, "Video could not start", RGB(255, 150, 150));
    y += (int)font_height(title) + 12;

    draw_clipped(&s, body, x, y, maxw, a->path, RGB(235, 240, 248));
    y += (int)font_height(body) + 10;
    if (a->err[0]) y = draw_para(&s, body, x, y, maxw, 9, a->err, RGB(255, 190, 190));

    y += 14;
    font_draw(&s, body, (float)x, (float)y,
              "q quits. The full libvlc error text is above, verbatim.",
              RGB(150, 160, 175));
    mc_present(c);
    (void)bg;
}

static void draw_overlay(Surface *s, VpApp *a, Font *fs, Font *fb) {
    int pad = 12;
    int lh = (int)font_height(fs) + 4;
    int hint_h = (int)font_height(fs) + 4;
    int err_h = a->err[0] ? lh : 0;
    int barh = lh * 3 + hint_h + err_h + 14;
    if (barh > s->h - 20) barh = s->h - 20;
    if (barh < lh) barh = lh;
    int y0 = s->h - barh;

    /* Solid, not alpha-blended: this is drawn once per decoded frame, and
     * 77k per-pixel divides 30 times a second is not free on QEMU. */
    surf_fill(s, 0, y0, s->w, barh, RGB(9, 11, 16));
    surf_fill(s, 0, y0, s->w, 1, RGB(78, 92, 114));

    char pos[32], len[32], size[32], line[512];
    fmt_time(pos, sizeof(pos), a->pos);
    fmt_time(len, sizeof(len), a->len);
    if (a->vw > 0 && a->vh > 0) snprintf(size, sizeof(size), "%dx%d", a->vw, a->vh);
    else snprintf(size, sizeof(size), "size pending");

    int y = y0 + 8;
    const char *st = a->player_ok ? state_name(a->last_state) : "no libvlc";
    snprintf(line, sizeof(line), "%s   [%s]", a->name[0] ? a->name : "no media", st);
    draw_clipped(s, fb, pad, y, s->w - pad * 2, line, RGB(238, 243, 250));
    y += lh;

    snprintf(line, sizeof(line), "%s / %s   %s   %s   vol %d",
             pos, len, size,
             a->codec[0] ? a->codec : "codec not reported yet",
             a->vol);
    draw_clipped(s, fs, pad, y, s->w - pad * 2, line, RGB(178, 190, 205));
    y += lh;

    if (a->err[0]) {
        draw_clipped(s, fs, pad, y, s->w - pad * 2, a->err, RGB(255, 150, 150));
        y += lh;
    }

    const char *audio_txt;
    int audio_ok = a->audio_device && a->audio_plugin;
    if (audio_ok)              audio_txt = "default output (ALSA)";
    else if (!a->audio_device) audio_txt = "none (no /dev/snd PCM node)";
    else                       audio_txt = "decoded, not played (no ALSA plugin in this bundle)";
    snprintf(line, sizeof(line), "scale %s   audio %s   libVLC %s",
             a->fit ? "fit" : "fill", audio_txt,
             a->libver[0] ? a->libver : "unreported");
    draw_clipped(s, fs, pad, y, s->w - pad * 2, line,
                 audio_ok ? RGB(150, 165, 180) : RGB(235, 185, 120));
    y += lh;

    draw_clipped(s, fs, pad, y, s->w - pad * 2,
                 "space play/pause   left/right 10s   up/down volume   "
                 "s stop   f fit/fill   q quit",
                 RGB(135, 148, 168));

    /* progress, drawn last so a one-pixel line is exact */
    if (a->len > 0 && a->pos >= 0) {
        int64_t frac = (int64_t)a->pos * (int64_t)s->w / (int64_t)a->len;
        if (frac < 0) frac = 0;
        if (frac > s->w) frac = s->w;
        surf_fill(s, 0, s->h - 3, (int)frac, 3, RGB(92, 165, 255));
    }
}

/* Read everything the status line needs from libVLC, then blit the latest
 * decoded picture and draw the overlay. All window writes happen here on
 * the main thread. */
static void render(MClient *c, VpApp *a, Font *fs, Font *fb, int force_frame) {
    Surface s;
    memset(&s, 0, sizeof(s));
    s.px = c->px;
    s.w = c->w;
    s.h = c->h;
    s.stride = c->stride;
    surf_clip(&s, 0, 0, c->w, c->h);

    int got = 0;
    if (a->player_ok && force_frame)
        got = vp_blit(&a->vmem, c->px, c->w, c->h, c->stride, a->fit);

    if (!got) {
        /* no decoded picture: an honest black window with the status on it,
         * never a fake frame */
        surf_fill(&s, 0, 0, c->w, c->h, RGB(6, 7, 10));
        if (a->player_ok) {
            Font *f = font_get(FONT_REGULAR, 13);
            font_draw(&s, f, 30, 30, "no picture decoded (the file may have no video track)",
                      RGB(150, 160, 175));
        }
    }
    if (a->err[0] && !a->player_ok) {
        draw_error_screen(c, a, fs, RGB(8, 9, 13));
        return;
    }
    draw_overlay(&s, a, fs, fb);
    mc_present(c);
}

static void refresh(VpApp *a) {
    if (!a->player_ok) return;
    VpApi *api = &a->pl.api;

    /* Ask VLC for the visible size before the status line reads it, so the
     * bar and the blit can never disagree about how much of the surface is
     * picture. */
    vp_visible_size(&a->pl, &a->vmem);

    a->last_state = api->player_get_state(a->pl.mp);
    a->pos = api->player_get_time(a->pl.mp);
    a->len = api->player_get_length(a->pl.mp);
    if (a->len <= 0) a->len = api->media_get_duration(a->pl.media);
    if (a->len <= 0) a->len = -1;       /* VLC does not know: show --:-- */

    int v = api->audio_get_volume(a->pl.mp);
    if (v >= 0) a->vol = v;

    pthread_mutex_lock(&a->vmem.m);
    a->vw = a->vmem.visw > 0 ? a->vmem.visw : a->vmem.w;
    a->vh = a->vmem.vish > 0 ? a->vmem.vish : a->vmem.h;
    pthread_mutex_unlock(&a->vmem.m);

    if (!a->queried_codec && a->last_state >= 3) {
        long t = now_ms();
        if (t >= a->codec_try_at) {
            a->queried_codec = vp_query_codec(&a->pl, a->codec, sizeof(a->codec));
            a->codec_try_at = t + 500;      /* retry while the ES is set up */
        }
    }

    if (a->last_state == 7 && !a->err[0]) {
        const char *e = api->errmsg();
        snprintf(a->err, sizeof(a->err), "libVLC reported Error: %s",
                 e && *e ? e : "libvlc_errmsg() has no detail");
    }
}

static void set_volume(VpApp *a, int delta) {
    if (!a->player_ok) return;
    int v = a->vol + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    a->vol = v;
    a->pl.api.audio_set_volume(a->pl.mp, v);
}

static void do_seek(VpApp *a, VpTime delta) {
    if (!a->player_ok) return;
    VpTime t = a->pl.api.player_get_time(a->pl.mp);
    if (t < 0) t = 0;
    VpTime n = t + delta;
    if (n < 0) n = 0;
    if (a->len > 0 && n > a->len - 1) n = a->len - 1;
    a->pl.api.player_set_time(a->pl.mp, n);
    a->pos = n;
}

static void toggle_pause(VpApp *a) {
    if (!a->player_ok) return;
    VpApi *api = &a->pl.api;
    int st = api->player_get_state(a->pl.mp);
    if (st == 3) {
        api->player_set_pause(a->pl.mp, 1);
    } else if (st == 4) {
        api->player_set_pause(a->pl.mp, 0);
    } else {
        /* stopped/ended/error: (re)start from the current position */
        if (st == 6 || st == 7) api->player_set_time(a->pl.mp, 0);
        if (api->player_play(a->pl.mp) != 0) {
            const char *e = api->errmsg();
            snprintf(a->err, sizeof(a->err), "libvlc_media_player_play() failed: %s",
                     e && *e ? e : "no libvlc_errmsg() detail");
        }
    }
}

/* Turn the two failures a user can actually cause into a sentence that says
 * what to do, keeping the loader's own words at the end. Anything else is
 * passed through untouched - that is the honest libVLC error text, and the
 * screen shows it verbatim. */
static void explain_open_error(char *out, size_t n, const char *raw) {
    if (!out || n == 0) return;
    if (!raw) raw = "";
    if (strstr(raw, "Dynamic loading not supported")) {
        snprintf(out, n,
                 "This is a static build. musl's dlopen() is a stub whose "
                 "whole answer is 'Dynamic loading not supported', so a "
                 "static executable cannot load libVLC's shared objects. "
                 "The playable build ships in the vlc package: run "
                 "pkg install vlc, then open the Player tile or "
                 "/root/apps/vlc/bin <file>.\n\n%s", raw);
        return;
    }
    if (strstr(raw, "cannot load") && strstr(raw, "libvlc")) {
        snprintf(out, n,
                 "libVLC is not installed where this build looks. The vlc "
                 "package carries libvlc.so.5, libvlccore.so.9 and the "
                 "plugins: run pkg install vlc.\n\n%s", raw);
        return;
    }
    snprintf(out, n, "%s", raw);
}

int main(int argc, char **argv) {
    MClient c;
    VpApp a;
    Font *fs, *fb;
    long next_status;
    int quit = 0, dirty = 1;
    int prev_buttons = 0;
    unsigned prev_frames = 0;

    memset(&a, 0, sizeof(a));
    a.fit = 1;
    a.vol = 100;
    a.last_state = 0;
    a.pos = -1;
    a.len = -1;
    a.codec_try_at = 0;

    a.have_file = argc > 1 && argv[1] && argv[1][0];
    if (a.have_file) {
        snprintf(a.path, sizeof(a.path), "%s", argv[1]);
        const char *slash = strrchr(a.path, '/');
        snprintf(a.name, sizeof(a.name), "%s", slash ? slash + 1 : a.path);
    }

    if (mc_open(&c, "vlc", "VLC Player", VP_WIN_W, VP_WIN_H) < 0) {
        fprintf(stderr, "vlcplayer: cannot connect to the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                        : "/run/mystic/wm.sock");
        return 1;
    }
    if (font_init("/usr/share/fonts") != 0) {
        fprintf(stderr, "vlcplayer: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }
    a.font_ok = 1;
    fs = font_get(FONT_REGULAR, 10);
    fb = font_get(FONT_SEMIBOLD, 12);
    if (!fs || !fb) {
        fprintf(stderr, "vlcplayer: fonts did not load\n");
        mc_close(&c);
        return 1;
    }

    a.audio_device = audio_device_present();
    a.audio_plugin = audio_plugin_present();
    if ((!a.audio_device || !a.audio_plugin) && !a.no_audio_once) {
        if (!a.audio_device)
            fprintf(stderr, "vlcplayer: no /dev/snd PCM node - libVLC will use "
                            "--aout=dummy (decoding audio, not playing it)\n");
        else
            fprintf(stderr, "vlcplayer: /dev/snd has a PCM node but this bundle "
                            "has no ALSA output plugin (libalsa_plugin.so); "
                            "passing --aout=dummy once\n");
        a.no_audio_once = 1;
    }

    /* For the no-argument card: it must say "install the package" only when
     * libVLC really is absent, and ask for a file when it is present. */
    a.lib_ok = libvlc_present();

    if (a.have_file) {
        char lib[PATH_MAX], err[512];
        find_libvlc(lib, sizeof(lib));
        /* Readable before libVLC is asked. libVLC 3 answers a missing path by
         * ending the media with libvlc_errmsg() empty, which would leave the
         * window saying "no picture decoded" - true, and useless. The kernel's
         * own errno says which file and why, and inside this jail the common
         * cause is that the window server did not bind the file (it is not in
         * the same directory the app was started with), so the path in the
         * message is the one that failed. */
        if (access(a.path, R_OK) != 0) {
            snprintf(a.err, sizeof(a.err), "cannot open %s: %s", a.path,
                     strerror(errno));
        } else if (vp_vmem_init(&a.vmem, VP_MAXW, VP_MAXH) != 0) {
            snprintf(a.err, sizeof(a.err), "out of memory for a %dx%d frame surface",
                     VP_MAXW, VP_MAXH);
        } else if (vp_player_open(&a.pl, &a.vmem, lib, a.path,
                                  !(a.audio_device && a.audio_plugin),
                                  err, sizeof(err)) != 0) {
            explain_open_error(a.err, sizeof(a.err), err);
        } else {
            a.player_ok = 1;
            const char *lv = a.pl.api.version();
            snprintf(a.libver, sizeof(a.libver), "%s", lv ? lv : "unknown");
            int v = a.pl.api.audio_get_volume(a.pl.mp);
            if (v >= 0) a.vol = v;
            if (a.pl.api.player_play(a.pl.mp) != 0) {
                const char *e = a.pl.api.errmsg();
                snprintf(a.err, sizeof(a.err),
                         "libvlc_media_player_play() failed: %s",
                         e && *e ? e : "no libvlc_errmsg() detail");
            }
        }
    }

    next_status = now_ms();
    while (!quit) {
        MEvent ev;
        int refreshed = 0;
        while (mc_poll(&c, &ev)) {
            dirty = 1;
            if (ev.type == MC_CLOSE) {
                quit = 1;
            } else if (ev.type == MC_RESIZE) {
                if (ev.w > 0 && ev.h > 0 && ev.w <= c.maxw && ev.h <= c.maxh) {
                    c.w = ev.w;
                    c.h = ev.h;
                }
            } else if (ev.type == MC_KEY && ev.press) {
                switch (ev.key) {
                case KEY_Q:
                case KEY_ESC:
                    quit = 1;
                    break;
                case KEY_SPACE:
                    toggle_pause(&a);
                    break;
                case KEY_LEFT:
                    do_seek(&a, -VP_SEEK_MS);
                    break;
                case KEY_RIGHT:
                    do_seek(&a, VP_SEEK_MS);
                    break;
                case KEY_UP:
                    set_volume(&a, 5);
                    break;
                case KEY_DOWN:
                    set_volume(&a, -5);
                    break;
                case KEY_S:
                    if (a.player_ok) {
                        a.pl.api.player_stop(a.pl.mp);
                        a.pos = 0;
                    }
                    break;
                case KEY_F:
                    a.fit = !a.fit;
                    break;
                default:
                    break;
                }
            } else if (ev.type == MC_MOUSE) {
                if ((ev.buttons & 1) && !(prev_buttons & 1)) toggle_pause(&a);
                prev_buttons = ev.buttons;
            }
        }

        long now = now_ms();
        if (now >= next_status) {
            refresh(&a);
            next_status = now + 200;
            refreshed = 1;
        }

        unsigned frames = a.player_ok ? vp_frame_count(&a.vmem) : 0;
        if (frames != prev_frames || dirty || refreshed) {
            if (a.have_file) {
                render(&c, &a, fs, fb, a.player_ok);
            } else {
                draw_error_screen(&c, &a, fs, RGB(8, 9, 13));
            }
            prev_frames = frames;
            dirty = 0;
        }

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 12L * 1000L * 1000L;
        nanosleep(&ts, NULL);
    }

    if (a.player_ok) vp_player_close(&a.pl);
    if (a.vmem.fb) vp_vmem_free(&a.vmem);
    mc_close(&c);
    return 0;
}

#endif /* VP_APP */

/* ============================================================
 *  5. The static launcher (VP_BOOT=1)
 *
 *  This is the only file in the bundle the kernel can exec inside a jail
 *  whose chroot has no /lib: it is static, so it has no PT_INTERP and no
 *  DT_NEEDED. (The image itself has /lib/ld-musl-x86_64.so.1 from the
 *  Alpine base; the jail does not, because wmsrv binds only the payload,
 *  the bundle, the fonts, /dev bits and /proc.)
 *  It finds the bundle and replaces itself with the bundled musl loader
 *  running the dynamic player. Nothing here touches libvlc, so when the
 *  closure is missing the message is the truth ("pkg install vlc")
 *  instead of a window that pretends to be a player.
 *
 *  musl's loader accepts being run as a program and consumes options
 *  before the program path (ldso/dynlink.c, __dls3: --list, --library-path,
 *  --preload, --argv0), which is what makes a self-contained bundle
 *  possible without a system /lib.
 * ============================================================ */
#if VP_BOOTING

#define VP_BOOT_LOADER "lib/ld-musl-x86_64.so.1"
#define VP_BOOT_PLAYER "libexec/vlcplayer"
#define VP_BOOT_LIBVLC "lib/libvlc.so.5"

static int vp_file_ok(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int vp_dir_ok(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* setenv() is POSIX; the host test is a Windows binary, where the libc
 * spelling is _putenv("K=V"). Only the boot section needs this. */
static int vp_setenv(const char *k, const char *v) {
#if defined(_WIN32)
    char buf[PATH_MAX * 2 + 64];
    if ((size_t)snprintf(buf, sizeof(buf), "%s=%s", k, v) >= sizeof(buf))
        return -1;
    return _putenv(buf);
#else
    return setenv(k, v, 1);
#endif
}

/* Full path of the running launcher, or "" when /proc is not mounted.
 * VP_TEST_SELF is the host test hook: it lets the Windows harness exercise
 * the same path arithmetic with a POSIX-shaped path. */
static void vp_self(char *out, size_t n) {
    char exe[PATH_MAX];
    ssize_t k;
    if (out == NULL || n == 0) return;
    out[0] = 0;
#ifdef VP_BOOT_TEST
    {
        const char *fake = getenv("VP_TEST_SELF");
        if (fake && *fake) {
            snprintf(out, n, "%s", fake);
            return;
        }
    }
#endif
#if defined(_WIN32)
    /* There is no /proc here; the test supplies VP_TEST_SELF and the real
     * launcher is only ever built for Linux. */
    (void)exe;
    (void)k;
#else
    k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) return;
    exe[k] = 0;
    snprintf(out, n, "%s", exe);
#endif
}

/* A directory is the bundle when it holds the player or its libvlc. */
static int vp_is_bundle(const char *dir) {
    char probe[PATH_MAX];
    if (!vp_dir_ok(dir)) return 0;
    snprintf(probe, sizeof(probe), "%s/" VP_BOOT_PLAYER, dir);
    if (vp_file_ok(probe)) return 1;
    snprintf(probe, sizeof(probe), "%s/" VP_BOOT_LIBVLC, dir);
    return vp_file_ok(probe);
}

/* Strip a trailing /libexec, so both ".../vlc/bin" and a bin symlink that
 * resolves to ".../vlc/libexec/vlcplayer" name the bundle root. */
static void vp_strip_libexec(char *dir) {
    char *slash = strrchr(dir, '/');
    if (slash && !strcmp(slash + 1, "libexec")) *slash = 0;
}

/* Find the bundle: $MYSTIC_BUNDLE (wmsrv exports it as /app), then the
 * directory of /proc/self/exe, then the directory of argv[0], then the jail
 * convention /app. Returns 1 when a directory that looks like a bundle was
 * found; returns 0 with the best existing candidate in out otherwise, so
 * the caller's error message can name a real path. */
static int vp_find_bundle(char *out, size_t n, int argc, char **argv) {
    char cand[PATH_MAX], exe[PATH_MAX];
    const char *env = getenv("MYSTIC_BUNDLE");

    if (out == NULL || n == 0) return 0;
    out[0] = 0;
    if (env && *env && vp_is_bundle(env)) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    vp_self(exe, sizeof(exe));
    if (exe[0]) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(cand, sizeof(cand), "%s", exe);
            vp_strip_libexec(cand);
            if (vp_is_bundle(cand)) {
                snprintf(out, n, "%s", cand);
                return 1;
            }
        }
    }
    if (argc > 0 && argv && argv[0]) {
        const char *a = argv[0];
        const char *slash = strrchr(a, '/');
        if (slash && (size_t)(slash - a) < sizeof(cand)) {
            memcpy(cand, a, (size_t)(slash - a));
            cand[slash - a] = 0;
            vp_strip_libexec(cand);
            if (vp_is_bundle(cand)) {
                snprintf(out, n, "%s", cand);
                return 1;
            }
        }
    }
    if (vp_is_bundle("/app")) {
        snprintf(out, n, "/app");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char bundle[PATH_MAX], loader[PATH_MAX], player[PATH_MAX];
    char lib[PATH_MAX], plugins[PATH_MAX], libvlc[PATH_MAX];
    char ld[PATH_MAX * 2];
    const char *old;
    char **av;
    int i, k;

    if (!vp_find_bundle(bundle, sizeof(bundle), argc, argv)) {
        char self[PATH_MAX] = "";
        vp_self(self, sizeof(self));
        fprintf(stderr,
                "vlc: cannot find a bundle with " VP_BOOT_PLAYER ".\n"
                "     MYSTIC_BUNDLE=%s\n"
                "     /proc/self/exe=%s\n"
                "     argv[0]=%s   /app=%s\n"
                "     On Mystic OS the window server exports MYSTIC_BUNDLE=/app.\n",
                getenv("MYSTIC_BUNDLE") ? getenv("MYSTIC_BUNDLE") : "(unset)",
                self[0] ? self : "(unreadable)",
                (argc > 0 && argv && argv[0]) ? argv[0] : "(none)",
                vp_is_bundle("/app") ? "a bundle"
                    : (vp_dir_ok("/app") ? "exists but is not a bundle" : "missing"));
        return 127;
    }
    snprintf(loader,  sizeof(loader),  "%s/" VP_BOOT_LOADER, bundle);
    snprintf(player,  sizeof(player),  "%s/" VP_BOOT_PLAYER, bundle);
    snprintf(libvlc,  sizeof(libvlc),  "%s/" VP_BOOT_LIBVLC, bundle);
    snprintf(lib,     sizeof(lib),     "%s/lib", bundle);
    snprintf(plugins, sizeof(plugins), "%s/lib/vlc/plugins", bundle);

    /* Order matters for the message: a bundle with no closure is the case a
     * user can fix, and it must not be reported as a broken executable. */
    if (!vp_file_ok(libvlc)) {
        fprintf(stderr,
                "vlc: libVLC is not installed (%s is missing).\n"
                "     On Mystic OS: pkg install vlc\n"
                "     The package carries libvlc.so.5, libvlccore.so.9 and the\n"
                "     plugins this player needs; this bundle has %s.\n",
                libvlc, bundle);
        return 127;
    }
    if (!vp_file_ok(player)) {
        fprintf(stderr,
                "vlc: %s is missing - the bundle is incomplete.\n"
                "     Reinstall it: pkg install --force vlc\n", player);
        return 127;
    }
    if (!vp_file_ok(loader)) {
        /* The image has /lib/ld-musl-x86_64.so.1 from its Alpine base, even
         * though the jail normally does not bind it. Prefer the bundle's own
         * copy, but do not refuse to start when the system has one. */
        const char *sysloader = "/lib/ld-musl-x86_64.so.1";
        if (vp_file_ok(sysloader)) {
            snprintf(loader, sizeof(loader), "%s", sysloader);
        } else {
            fprintf(stderr,
                    "vlc: no dynamic loader: %s is missing and so is %s.\n"
                    "     Reinstall it: pkg install --force vlc\n",
                    loader, sysloader);
            return 127;
        }
    }

    old = getenv("LD_LIBRARY_PATH");
    if (old && *old) snprintf(ld, sizeof(ld), "%s:%s", lib, old);
    else             snprintf(ld, sizeof(ld), "%s", lib);
    vp_setenv("LD_LIBRARY_PATH", ld);
    vp_setenv("VLC_PLUGIN_PATH", plugins);
    /* Informational: the player no longer re-execs. It documents that this
     * process already came through the launcher. */
    vp_setenv("MYSTIC_VLC_LDPATH", "1");

    /* loader --library-path <bundle>/lib <bundle>/libexec/vlcplayer args... */
    av = (char **)calloc((size_t)(argc > 0 ? argc : 1) + 4, sizeof(*av));
    if (!av) {
        fprintf(stderr, "vlc: out of memory building the loader command\n");
        return 127;
    }
    k = 0;
    av[k++] = loader;
    av[k++] = (char *)"--library-path";
    av[k++] = lib;
    av[k++] = player;
    for (i = 1; i < argc; i++) av[k++] = argv[i];
    av[k] = NULL;

#ifdef VP_BOOT_TEST
    printf("bundle=%s\nloader=%s\nplayer=%s\nlibvlc=%s\nlib=%s\nplugins=%s\n",
           bundle, loader, player, libvlc, lib, plugins);
    printf("LD_LIBRARY_PATH=%s\nVLC_PLUGIN_PATH=%s\n",
           getenv("LD_LIBRARY_PATH"), getenv("VLC_PLUGIN_PATH"));
    for (i = 0; i < k; i++) printf("argv[%d]=%s\n", i, av[i]);
    free(av);
    return 0;
#else
    execv(loader, av);
    fprintf(stderr, "vlc: cannot run %s: %s\n", loader, strerror(errno));
    free(av);
    return 127;
#endif
}

#endif /* VP_BOOTING */

#endif /* !VLC_TILE_MSG_TEST */
