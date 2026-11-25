/* setenv(), clock_gettime() and usleep() are POSIX, and musl hides them
 * under -std=c11 unless a feature-test macro asks for them. This must come
 * before any system header, so it is the first line of the file. */
#define _GNU_SOURCE 1

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
 *  lock() with plane pointers, picture_CopyPixels() copies the decoded
 *  picture into them, unlock() says it is complete, and display() says
 *  it is due. Before the first picture the
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
 *  THE PIXEL PATH: THE DECODER'S OWN PLANES, SAMPLED AT THE WINDOW
 *  ---------------------------------------------------------------
 *  The old code asked libVLC for RV32 at the window-body size with
 *  libvlc_video_set_format(mp, "RV32", 960, 540, 960*4). That makes
 *  libVLC's video-output thread convert and rescale every decoded
 *  picture to RGB before this process ever sees it: for 4K it touches
 *  8,294,400 source pixels per frame on one thread. The rate reported for
 *  the four-vCPU guest was 17 fps. This path takes that full-frame
 *  conversion off the vout thread and converts only the displayed pixels.
 *  On the host used to test it the old path was already at the source
 *  rate, so the end-to-end rate did not improve there: ten alternating
 *  8 s runs on a 23.976 fps 4K file gave median 22.9 fps (range
 *  20.5-23.6) for this path vs 23.7 (23.4-24.0) for RV32. The vout still
 *  copies whole planes (12.5 MB at 4K) and this path samples them, so a
 *  small median cost remains; the guest's gain is expected, not measured. What is measured is the work removed:
 *  libVLC no longer converts or rescales a frame at all.
 *
 *  This file now uses the setup/cleanup half of the vmem API:
 *
 *      video_set_callbacks(mp, lock, unlock, display, &surf);
 *      video_set_format_callbacks(mp, setup, cleanup);
 *
 *  modules/video_output/vmem.c calls setup(&opaque, chroma, &w, &h,
 *  pitches, lines) with the decoder's own format and picture size and
 *  honours the FOURCC written back. Answering I420 (or keeping the NV12
 *  the decoder proposed) at the same w/h means libVLC performs NO chroma
 *  conversion and NO scaling: vmem.c's Prepare() only picture_CopyPixels()
 *  the plane data into the buffers setup() allocated. The window thread
 *  then samples those planes straight into the window body at the
 *  displayed size - 518,400 reads for a 960x540 body instead of 8,294,400
 *  converts - using yuv420_to_rgb_nearest() from src/yuv.h. Only pixels
 *  that are actually presented are read, and the per-pixel work left the
 *  single video-output thread. The sampling measured 2.1-3.6 ms per frame
 *  at the shipping -Os on the test host (4K source, 960x540 body); its
 *  cost grows with the BODY, not the source, so a full-screen window is
 *  the case to watch (see surf_blit's note).
 *
 *  The sampling is nearest-neighbour ON PURPOSE: at a 4:1 downscale it
 *  picks every fourth source pixel, so fine detail can alias. A box
 *  filter or bilinear taps are a possible later refinement behind the
 *  same yuv.h interface; this is not passed off as one.
 *
 *  Colour is BT.601 limited range, integer only (the matrix and its
 *  limits are documented in src/yuv.h). libVLC 3's vmem API has no
 *  colour-space field, so a BT.709-tagged stream is shown through the
 *  same fixed matrix; that is stated rather than guessed from the
 *  resolution, and it is the honest limitation of this path.
 *
 *  The vmem contract is also alignment, and it is not advisory:
 *  libvlc_media_player.h says "The planes must be aligned on 32-bytes
 *  boundaries". surf_set_planes() over-allocates each block by 31 bytes,
 *  rounds the Y plane up inside it, and gives every plane a pitch that
 *  is a multiple of 32, so each row starts aligned too. It verifies that
 *  before telling libVLC anything and fails setup() (which vmem.c treats
 *  as "no pictures") instead of handing over a misaligned pointer.
 *
 *  WHY THE PICTURE IS FITTED, AND WHY THE LETTERBOX IS OURS
 *  --------------------------------------------------------
 *  The planes are the decoder's size, which can be taller than the
 *  visible picture (a coded height padded to a macroblock edge, e.g.
 *  320x194 for a 320x176 file). libvlc_video_get_size() is libVLC's own
 *  answer for the visible picture, so surf_blit() samples only that
 *  top-left rectangle and fit_rect() maps it into the body by its
 *  aspect. The rest of the body is filled with the letterbox colour.
 *
 *  Sample aspect ratio is still not carried by this API: content whose
 *  pixels are not square is shown at its coded aspect, not a
 *  SAR-corrected one. That is the one geometric limitation of this
 *  path, stated here rather than papered over.
 *
 *  THREADING: THREE PLANE SETS, TWO OWNERS, NEVER THE SAME SET
 *  -----------------------------------------------------------
 *  libVLC calls setup/lock/unlock/display on its video-output thread
 *  while the window is drawn on the main thread. The surface has one
 *  mutex and a ring of three plane sets:
 *
 *    setup()     installs the ring, freeing the previous sets after
 *                any reader has finished; called before the vout starts
 *                and again when the format changes;
 *    cleanup()   detaches and frees the ring, after the last unlock;
 *    lock()      takes the next set, waits only if the window thread is
 *                still reading that one, and hands out its three planes;
 *    unlock()    publishes the set as complete, counts the picture,
 *                wakes waiters;
 *    display()   counts a presentation (the playback clock said it is due);
 *    blit()      marks the newest complete set as being read, converts
 *                it into the window, and releases it.
 *
 *  The video-output thread therefore fills one set while the window
 *  thread converts another, and has a third in reserve. No libVLC thread
 *  ever writes the window, and the main thread never reads a half-written
 *  picture. The conversion deliberately runs in blit() on the main thread
 *  rather than inside unlock(): the video-output thread must not write the
 *  shared window buffer (the HUD is drawn there too), and a frame that is
 *  never presented then costs nothing to convert.
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
 *    5. <dir of this executable>/../lib/libvlc.so.5   a bundle run directly
 *       from the rootfs: this file sits in <bundle>/libexec/, so ../lib is
 *       <bundle>/lib. (Not a manifest exec path - wmsrv rejects exec values
 *       containing '/' - this is the launcher case described under BUILD.)
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
 *  string plus the pkg line, so it is never a silent blank window. (The
 *  stub is not folklore: the string is in this file's own -static link.)
 *
 *      MSYS_NO_PATHCONV=1 zig cc -target x86_64-linux-musl -Os -dynamic \
 *             -Wl,-rpath,'$ORIGIN/../lib' -Wl,-dynamic-linker=/lib/ld-musl-x86_64.so.1 \
 *             -Isrc -Isrc/client -o vlcplay \
 *             src/client/apps/vlcplay.c src/client/mystic_client.c \
 *             src/gfx.c src/font.c src/ui.c -lm -pthread -ldl
 *
 *  The SAME file builds the static launcher the manifest's `exec` needs, with
 *  no libVLC and no SDK sources at all (section 0 describes what it does):
 *
 *      zig cc -target x86_64-linux-musl -Os -static -DVLCPLAY_BOOT \
 *             -Isrc -Isrc/client -o bin src/client/apps/vlcplay.c
 *
 *  Both are compile-checked with the plain -c form and -Wall -Wextra.
 *
 *  MSYS_NO_PATHCONV=1 is not decoration. Built from git-bash on Windows
 *  without it, MSYS rewrites the absolute -dynamic-linker argument into a
 *  Windows path, and the ELF's PT_INTERP then names a file that exists on no
 *  Linux machine. Both results were read out of the program headers:
 *
 *      with    MSYS_NO_PATHCONV=1: PT_INTERP /lib/ld-musl-x86_64.so.1
 *      without                  : PT_INTERP C:/Users/<name>/.../lib/ld-musl-x86_64.so.1
 *
 *  and in both cases NEEDED libc.so, RUNPATH $ORIGIN/../lib. (-dynamic is
 *  not optional either: zig's musl target defaults to a static link.)
 *
 *  WHERE THIS BINARY CAN BE THE MANIFEST'S exec, AND WHERE IT CANNOT
 *  -----------------------------------------------------------------
 *  Not inside the wmsrv jail. src/wmsrv.c chroots each app into a directory
 *  built from binds - the payload at /bin/app, the bundle at /app, the
 *  socket, fonts, /dev nodes - and nothing else; there is no /lib in it. A
 *  dynamic payload's PT_INTERP is absolute, so that exec fails before main()
 *  runs. The package therefore needs a STATIC launcher as its `bin`, and this
 *  file builds that launcher: see section 0 with -DVLCPLAY_BOOT. The intended
 *  installed shape is
 *
 *      bin                      STATIC launcher (manifest exec = bin)
 *                               built from this file with -DVLCPLAY_BOOT
 *      libexec/vlcplay          this file, DYNAMIC (section BUILD above)
 *      lib/ld-musl-x86_64.so.1  the loader; the launcher execs it with
 *                               --library-path /app/lib and the player path
 *      lib/libvlc.so.5          the package's soname symlink
 *      lib/libvlccore.so.9      ditto
 *      lib/vlc/plugins/...      demuxers, decoders, the vmem output
 *
 *  That is the shape docs/MULTIMEDIA.md describes and the shape pkg's tar
 *  unpacker supports (symlink members included). It is NOT what
 *  build/repo/vlc.tar.gz contained when this revision was written: that tar's
 *  `bin` was a dynamic ELF with PT_INTERP /lib/ld-musl-x86_64.so.1 and it had
 *  no libexec/ member at all, so the tile could not start. That is a packaging
 *  bug outside this file - the two build commands are in section 0 and in
 *  BUILD above, and the launcher's path logic is tested in .work/p1/boottest.c.
 *
 *  Outside the jail - on the real rootfs, where /lib/ld-musl-x86_64.so.1
 *  exists, or with the closure reached through RUNPATH - the dynamic binary
 *  starts directly. Both entry paths end in the same main().
 *
 *  Keys: space play/pause (on an ended file: restart), Left/Right seek 5 s,
 *  Up/Down volume, q or Esc quit, a click on the picture toggles playback
 *  (and restarts an ended file). Both restart paths are measured in
 *  .work/p1/restart.c: set_time(0)+play() leaves VLC Ended, stop()+play()
 *  returns it to Playing with the clock and the decoded frames advancing.
 * ============================================================ */

#include "../vlcmin.h"           /* src/client/vlcmin.h - the ABI slice */
#include "../mystic_client.h"    /* window, events, mc_present()        */
#include "../../mystic.h"        /* Surface, RGB, fonts                 */
#include "../../yuv.h"           /* I420/NV12 -> 0xAARRGGBB, nearest    */

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
#define VLC_BAD_SYMBOL_MSG \
    "the library loaded but is missing symbols it must export - " \
    "reinstall it with: pkg install vlc"

/* Linux evdev codes, the numbers src/input.c forwards (the same values
 * the other apps in src/apps use). */
#define KEY_ESC    1
#define KEY_Q     16
#define KEY_SPACE 57
#define KEY_UP    103
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_DOWN  108
#define KEY_LEFTBRACKET  26   /* [ : slower */
#define KEY_RIGHTBRACKET 27   /* ] : faster */

/* ============================================================
 *  0. THE LAUNCHER BUILD (-DVLCPLAY_BOOT): THE STATIC `bin`
 *
 *  The manifest's `exec` is a bare file name and wmsrv execs it inside a
 *  jail whose only contents are binds: the payload at /bin/app, the bundle
 *  at /app, the socket, fonts, /dev nodes and the app's own /tmp and /root.
 *  There is NO /lib in that jail (src/wmsrv.c's sbx plan, and
 *  docs/MULTIMEDIA.md says the same), and a dynamic binary's PT_INTERP is an
 *  absolute path, so the kernel cannot start one there. The same file built
 *  with -DVLCPLAY_BOOT is a static launcher that provides the missing step:
 *
 *      /app/lib/ld-musl-x86_64.so.1 --library-path /app/lib \
 *          /app/libexec/vlcplay <args...>
 *
 *  It resolves the bundle root from MYSTIC_BUNDLE (wmsrv sets /app), then
 *  from /proc/self/exe (<bundle>/bin/<name> -> <bundle>), checks that the
 *  loader and the player are really there, exports LD_LIBRARY_PATH and
 *  VLC_PLUGIN_PATH when the caller did not, and execs. If anything is
 *  missing it says which path, on stderr, and exits 127 - a tile that dies
 *  silently is the failure this build exists to prevent.
 *
 *  Two build commands, both from this one file:
 *
 *      # static launcher, the manifest's exec
 *      zig cc -target x86_64-linux-musl -Os -static -DVLCPLAY_BOOT \
 *             -Isrc -Isrc/client -o bin src/client/apps/vlcplay.c
 *
 *      # the dynamic player, libexec/vlcplay (section BUILD below)
 * ============================================================ */
#if defined(VLCPLAY_BOOT)

#define BOOT_LOADER_REL "lib/ld-musl-x86_64.so.1"
#define BOOT_PLAYER_REL "libexec/vlcplay"

typedef struct {
    char root[512];
    char libdir[640];
    char loader[704];
    char player[704];
} BootPlan;

/* Why a root could not be used, so the caller can say something true. The
 * too-long and bad-env cases are NOT "unset": a 600-byte MYSTIC_BUNDLE used
 * to produce the sentence "MYSTIC_BUNDLE is unset", which was false. */
enum {
    BOOT_ROOT_TOO_LONG = -2,
    BOOT_ROOT_BAD_ENV  = -1,
    BOOT_ROOT_NONE     =  0,
    BOOT_ROOT_OK       =  1
};

/* Pure string work, no syscalls, so the host harness can test every layout:
 *
 *   1. $MYSTIC_BUNDLE                      wmsrv exports /app in the jail
 *   2. <...>/bin/<name>  -> <...>          the installed bundle layout
 *   3. dirname(<exe>)                      anything else
 *
 *  Returns BOOT_ROOT_OK with a root in `out`, or one of the reasons above. */
static int boot_pick_root(const char *env, const char *exe, char *out, size_t n) {
    const char *slash, *prev = NULL, *q;
    size_t rootlen;

    if (!out || n < 2) return BOOT_ROOT_TOO_LONG;
    out[0] = 0;
    if (env && *env) {
        if (env[0] != '/') return BOOT_ROOT_BAD_ENV;   /* relative: not a mount */
        if (strlen(env) >= n) return BOOT_ROOT_TOO_LONG;
        snprintf(out, n, "%s", env);
        return BOOT_ROOT_OK;
    }
    if (!exe || !*exe) return BOOT_ROOT_NONE;
    slash = strrchr(exe, '/');
    if (!slash) return BOOT_ROOT_NONE;

    /* <bundle>/bin/<name>: drop "/<name>" and the "/bin" before it */
    for (q = exe; q < slash; q++)
        if (*q == '/') prev = q;
    if (prev && prev > exe && (size_t)(slash - prev) == 4 &&
        memcmp(prev, "/bin", 4) == 0) {
        rootlen = (size_t)(prev - exe);
        if (rootlen >= n) return BOOT_ROOT_TOO_LONG;
        memcpy(out, exe, rootlen);
        out[rootlen] = 0;
        return BOOT_ROOT_OK;
    }

    /* dirname(exe); "/bin/app" gives "/bin" */
    rootlen = (size_t)(slash - exe);
    if (rootlen == 0) {
        out[0] = '/';
        out[1] = 0;
        return BOOT_ROOT_OK;
    }
    if (rootlen >= n) return BOOT_ROOT_TOO_LONG;
    memcpy(out, exe, rootlen);
    out[rootlen] = 0;
    return BOOT_ROOT_OK;
}

/* root -> the three absolute paths the exec needs. 0 when they do not fit. */
static int boot_plan(const char *root, BootPlan *p) {
    if (!root || !*root || !p) return 0;
    if ((size_t)snprintf(p->root, sizeof(p->root), "%s", root) >= sizeof(p->root))
        return 0;
    if ((size_t)snprintf(p->libdir, sizeof(p->libdir), "%s/lib", p->root) >=
        sizeof(p->libdir))
        return 0;
    if ((size_t)snprintf(p->loader, sizeof(p->loader), "%s/%s", p->root,
                         BOOT_LOADER_REL) >= sizeof(p->loader))
        return 0;
    if ((size_t)snprintf(p->player, sizeof(p->player), "%s/%s", p->root,
                         BOOT_PLAYER_REL) >= sizeof(p->player))
        return 0;
    return 1;
}

#if !defined(_WIN32)
int main(int argc, char **argv) {
    char exe[512], root[512], plugins[704];
    const char *env;
    BootPlan plan;
    char **args;
    int extra, i, rc;
    ssize_t n;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) exe[0] = 0; else exe[n] = 0;

    env = getenv("MYSTIC_BUNDLE");
    rc = boot_pick_root(env, exe, root, sizeof(root));
    if (rc == BOOT_ROOT_BAD_ENV) {
        fprintf(stderr, "vlcplay: MYSTIC_BUNDLE='%s' is not an absolute "
                        "path, so it cannot name the bundle mount\n", env);
        return 127;
    }
    if (rc == BOOT_ROOT_TOO_LONG) {
        fprintf(stderr, "vlcplay: %s is too long for the %u-byte bundle-root "
                        "buffer\n",
                (env && *env) ? "MYSTIC_BUNDLE" : "the executable path",
                (unsigned)sizeof(root));
        return 127;
    }
    if (rc == BOOT_ROOT_NONE) {
        fprintf(stderr, "vlcplay: cannot find the bundle root: MYSTIC_BUNDLE "
                        "is unset and /proc/self/exe did not resolve\n");
        return 127;
    }
    if (!boot_plan(root, &plan)) {
        fprintf(stderr, "vlcplay: the bundle path under %s is too long\n", root);
        return 127;
    }
    /* A server that does not export MYSTIC_BUNDLE leaves the executable at
     * /bin/app, so the root derived from /proc/self/exe is /bin - which is
     * not where a bundle lives. The bundle is mounted at /app by the sandbox
     * either way, so fall back to it when the first guess has no loader. */
    if (access(plan.loader, X_OK) != 0 && strcmp(plan.root, "/app") != 0) {
        BootPlan alt;
        if (boot_plan("/app", &alt) && access(alt.loader, X_OK) == 0) {
            plan = alt;
            snprintf(root, sizeof(root), "%s", plan.root);
        }
    }
    if (access(plan.loader, X_OK) != 0) {
        fprintf(stderr, "vlcplay: %s: %s - the bundle must carry the musl "
                        "loader at %s\n", plan.loader, strerror(errno),
                BOOT_LOADER_REL);
        return 127;
    }
    if (access(plan.player, X_OK) != 0) {
        fprintf(stderr, "vlcplay: %s: %s - the dynamic player must be at %s\n",
                plan.player, strerror(errno), BOOT_PLAYER_REL);
        return 127;
    }

    /* musl reads these when the loader starts, which is the very next exec,
     * so setting them here works. The jail already exports both. */
    setenv("LD_LIBRARY_PATH", plan.libdir, 0);
    if (!getenv("VLC_PLUGIN_PATH")) {
        snprintf(plugins, sizeof(plugins), "%s/vlc/plugins", plan.libdir);
        if (access(plugins, R_OK) == 0) setenv("VLC_PLUGIN_PATH", plugins, 1);
    }

    extra = argc > 1 ? argc - 1 : 0;
    args = (char **)calloc((size_t)extra + 5u, sizeof(char *));
    if (!args) {
        fprintf(stderr, "vlcplay: out of memory building the loader command\n");
        return 127;
    }
    args[0] = plan.loader;
    args[1] = (char *)"--library-path";
    args[2] = plan.libdir;
    args[3] = plan.player;
    for (i = 0; i < extra; i++) args[4 + i] = argv[1 + i];
    args[4 + extra] = NULL;

    execv(plan.loader, args);
    fprintf(stderr, "vlcplay: cannot exec %s: %s\n", plan.loader,
            strerror(errno));
    return 127;
}
#endif /* !_WIN32 */

#else  /* !VLCPLAY_BOOT: the player itself, from section 1 down */

/* ------------------------------------------------------------ layout */

#define WIN_W  960
#define WIN_H  640
#define HEAD_H 36
#define FOOT_H 64
#define BODY_MIN_H 48

#define SEEK_MS 5000
#define VOL_STEP 5

static long long now_ms(void);

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

/* libvlc_media_player.h - the setup/cleanup half of vmem. setup() may be
 * called several times (libVLC probes decoder formats), receives the
 * decoder's chroma and picture size, writes the FOURCC and plane layout it
 * wants, and returns the plane count; 0 means "no pictures". cleanup()
 * releases whatever setup() published through *opaque. */
typedef unsigned (*VlcFormatCb)(void **opaque, char *chroma, unsigned *width,
                                unsigned *height, unsigned *pitches,
                                unsigned *lines);
typedef void (*VlcCleanupCb)(void *opaque);

/* One plane set. libVLC writes one while the window thread reads the
 * other; `in_vlc`/`reading` are what keeps them apart. */
typedef struct {
    uint8_t        *planes[3];   /* Y,U,V for I420; Y,UV,NULL for NV12 */
    void           *mem;         /* malloc block the planes live in     */
    int             in_vlc;      /* libVLC is writing this set          */
    int             reading;     /* the window thread is reading it     */
    int             complete;    /* a whole picture has been written    */
    unsigned        seq;         /* s->frames when it was completed     */
} VlcPlanes;

/* Plane sets in the ring. libVLC writes one while the window thread
 * reads an older one; 3 means the vout is not held up by a conversion
 * unless the window thread has fallen two frames behind. */
#define VLC_SETS 3

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    VlcPlanes       set[VLC_SETS]; /* ring, see section 2              */
    int             write_idx;   /* the set lock() hands out next       */
    int             ready;       /* newest complete set, -1 when none   */
    int             w, h;        /* decoder (coded) size in pixels      */
    int             pitches[3];  /* bytes per row, multiple of 32       */
    int             lines[3];    /* rows per plane                      */
    int             nv12;        /* set[*].planes[1] is interleaved UV  */
    unsigned        frames;      /* complete pictures (unlock ran)      */
    long long       first_frame_ms; /* when the first picture arrived   */
    long long       rate_ms;     /* time of the last 100-frame report   */
    unsigned        rate_frames; /* frame count at that report          */
    unsigned        displays;    /* presentations (display ran)         */
    unsigned        lock_calls;  /* vmem lock() entries, for diagnosis  */

    /* The RV32 path: one 32-bit buffer, copied by the window thread. It is
     * the path that has always worked in the guest, and it stays available
     * (MYSTIC_VLC_PLANES=1 selects the plane path instead) because the plane
     * path could not be made to show a picture under the guest's libVLC
     * 3.0.23 - see the note above setup(). */
    int             rv32;
    uint32_t       *fb;          /* w*h pixels, aligned               */
    void           *fb_raw;
    int             pitch;
    int             busy;
} VlcSurface;

typedef struct {
    VlcLib   lib;
    VlcApi   api;
    /* The setup/cleanup entry point, which vlcmin.h's table does not
     * carry. libVLC 3 has exported it since 3.0 (docs/VLC.md's symbol
     * dump of the shipped 3.0.23 library lists it); it is bound by name
     * like everything else, so a library without it is reported by name
     * and never called. */
    unsigned (*video_set_format_callbacks)(libvlc_media_player_t *,
                                           VlcFormatCb, VlcCleanupCb);
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
    int vid_w, vid_h;          /* libvlc_video_get_size, 0 until known */
    int vol;                   /* what libVLC's getter last answered   */
    int vol_known;             /* a getter answer, not a guess        */
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
    {
        /* Not in VLC_MIN_FUNCTIONS: this player needs the setup/cleanup
         * variant to receive I420 planes, and a library that cannot
         * provide it cannot run this path. Report the name. */
        void *fn_ = vlc_lib_sym(p->lib, "libvlc_video_set_format_callbacks");
        if (!fn_) {
            snprintf(missing, mn, "%s", "libvlc_video_set_format_callbacks");
            return 0;
        }
        memcpy(&p->video_set_format_callbacks, &fn_,
               sizeof(p->video_set_format_callbacks));
    }
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
 *
 *  libVLC is given the decoder's own I420 (or NV12) planes at the
 *  decoder's own size, allocated here. vmem.c's Prepare() copies each
 *  decoded picture into them with picture_CopyPixels(); the window
 *  thread samples them into the window in surf_blit(). No RGB frame
 *  buffer and no window-sized rescale exist anywhere in this path.
 *
 *  There are THREE plane sets in a ring. libVLC fills one while the
 *  window thread converts the newest completed one, so the video-output
 *  thread does not wait for a conversion it is not part of. With one set
 *  every frame cost the vout a wait of one conversion; with two it still
 *  caught up with the window thread every other frame (20-21 fps against
 *  the 23.976 fps source in every run). Three sets removed that
 *  systematic stall. What remains is a 0.5-0.8 fps median difference
 *  from the larger plane copy, plus an occasional ~20.5 fps tail that
 *  both this path and RV32 show under load on the shared test host.
 *  The window thread still never reads a set libVLC is writing; if it
 *  falls two frames behind, lock() waits. Memory: three plane sets
 *  (37 MB for 4K, 9 MB for 1080p).
 * ============================================================ */

/* libvlc_media_player.h: "The planes must be aligned on 32-bytes
 * boundaries". musl's malloc returns 16-byte alignment, so each block is
 * over-allocated by 31 bytes and the Y plane is rounded up inside it.
 * Every pitch is rounded up to 32 bytes too, which is what makes every
 * ROW of every plane start on a 32-byte boundary. surf_set_planes()
 * checks both before the pointers are published and fails setup() (which
 * vmem.c reports as "no pictures") instead of handing libVLC a
 * misaligned buffer. */
#define VLC_MIN_PLANE_ALIGN 32u

/* Largest dimension accepted from a decoder. 8K (7680x4320) fits inside
 * this; 8192x8192 I420 is 100 MB per set, so a bogus header cannot ask
 * for an unbounded allocation. */
#define VLC_MAX_DIM 8192

static size_t align32(size_t n) {
    return (n + (size_t)VLC_MIN_PLANE_ALIGN - 1u) &
           ~((size_t)VLC_MIN_PLANE_ALIGN - 1u);
}

static int surf_init(VlcSurface *s) {
    memset(s, 0, sizeof(*s));
    s->ready = -1;
    if (pthread_mutex_init(&s->m, NULL) != 0) return -1;
    if (pthread_cond_init(&s->cv, NULL) != 0) {
        pthread_mutex_destroy(&s->m);
        return -1;
    }
    return 0;
}

/* Allocate one plane set for w x h. 0 on success. */
static int planes_alloc(VlcPlanes *ps, int w, int h, int nv12,
                        int yp, int up, int vp) {
    size_t chroma_h = (size_t)((h + 1) / 2);
    size_t bytes = (size_t)yp * (size_t)h + (size_t)up * chroma_h +
                   (nv12 ? 0u : (size_t)vp * chroma_h);
    unsigned char *raw;
    uint8_t *base;

    memset(ps, 0, sizeof(*ps));
    raw = (unsigned char *)malloc(bytes + (size_t)VLC_MIN_PLANE_ALIGN);
    if (!raw) return -1;
    base = (uint8_t *)(((uintptr_t)raw + (uintptr_t)VLC_MIN_PLANE_ALIGN - 1u) &
                       ~((uintptr_t)VLC_MIN_PLANE_ALIGN - 1u));

    /* The 32-byte contract, checked against the real addresses. The pitches
     * are multiples of 32 by construction, so the plane offsets (plane
     * sizes) are too; this proves it instead of trusting it. */
    if (((uintptr_t)base & (uintptr_t)(VLC_MIN_PLANE_ALIGN - 1u)) != 0 ||
        ((size_t)yp % VLC_MIN_PLANE_ALIGN) != 0 ||
        ((size_t)up % VLC_MIN_PLANE_ALIGN) != 0 ||
        ((size_t)vp % VLC_MIN_PLANE_ALIGN) != 0) {
        free(raw);
        return -1;
    }
    memset(base, 0, bytes);            /* black, never uninitialised memory */
    ps->mem = raw;
    ps->planes[0] = base;
    ps->planes[1] = base + (size_t)yp * (size_t)h;
    ps->planes[2] = nv12 ? NULL
                         : base + (size_t)yp * (size_t)h +
                                  (size_t)up * chroma_h;
    return 0;
}

/* Install a fresh pair of w x h plane sets: I420 (nv12=0, three planes) or
 * NV12 (nv12=1, Y plus interleaved UV). 0 on success, -1 when it cannot be
 * allocated or the alignment contract would be broken (nothing changes in
 * that case). The previous blocks are freed here, after any reader has
 * finished, so a format change cannot free memory the window thread or
 * libVLC is using. */
static int surf_set_planes(VlcSurface *s, int w, int h, int nv12) {
    size_t chroma_w, chroma_h;
    int yp, up, vp;
    VlcPlanes fresh[VLC_SETS];
    void *old[VLC_SETS];
    int k;

    if (!s || w < 2 || h < 2 || w > VLC_MAX_DIM || h > VLC_MAX_DIM) return -1;

    chroma_w = (size_t)((w + 1) / 2);
    chroma_h = (size_t)((h + 1) / 2);
    yp = (int)align32((size_t)w);
    up = (int)align32(nv12 ? chroma_w * 2u : chroma_w);
    vp = nv12 ? 0 : up;

    for (k = 0; k < VLC_SETS; k++) {
        if (planes_alloc(&fresh[k], w, h, nv12, yp, up, vp) != 0) {
            while (k > 0) free(fresh[--k].mem);
            return -1;
        }
    }

    pthread_mutex_lock(&s->m);
    for (k = 0; k < VLC_SETS; k++) {
        while (s->set[k].in_vlc || s->set[k].reading)
            pthread_cond_wait(&s->cv, &s->m);
        old[k] = s->set[k].mem;
        s->set[k] = fresh[k];
    }
    s->w = w;
    s->h = h;
    s->nv12 = nv12;
    s->pitches[0] = yp;
    s->pitches[1] = up;
    s->pitches[2] = vp;
    s->lines[0] = h;
    s->lines[1] = (int)chroma_h;
    s->lines[2] = nv12 ? 0 : (int)chroma_h;
    s->write_idx = 0;
    s->ready = -1;                     /* nothing in these sets yet */
    pthread_mutex_unlock(&s->m);

    for (k = 0; k < VLC_SETS; k++) free(old[k]);
    return 0;
}

static void surf_free(VlcSurface *s) {
    int k;
    if (!s) return;
    for (k = 0; k < VLC_SETS; k++) {
        free(s->set[k].mem);
        s->set[k].mem = NULL;
        s->set[k].planes[0] = NULL;
    }
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->m);
}

/* vmem.c: char chroma[5] holds the decoder's FOURCC; the callback writes
 * the one it wants and returns the number of planes. It is called once per
 * video output (and again if the format changes). */
static unsigned cb_setup(void **opaque, char *chroma, unsigned *width,
                         unsigned *height, unsigned *pitches,
                         unsigned *lines) {
    VlcSurface *s = opaque ? (VlcSurface *)*opaque : NULL;
    char offered[5];
    int w, h, nv12 = 0;

    if (!s || !chroma || !width || !height || !pitches || !lines) return 0;
    if (*width < 2 || *height < 2 ||
        *width > (unsigned)VLC_MAX_DIM || *height > (unsigned)VLC_MAX_DIM) {
        fprintf(stderr, "vlcplay: refusing a %ux%u vmem format\n",
                *width, *height);
        return 0;
    }
    w = (int)*width;
    h = (int)*height;
    memcpy(offered, chroma, 4);       /* what the decoder proposed */
    offered[4] = 0;

    /* Keep the decoder's own format and size when it is one we can sample
     * (8-bit 4:2:0). That is the whole optimisation: libVLC then performs
     * no chroma conversion and no scaling, it only copies planes. Anything
     * else - a hardware surface, 10-bit, 4:2:2 - is answered with I420 at
     * the same size, which libVLC converts to but never rescales.
     *
     * There is deliberately no "if I420 is refused" retry: this callback
     * CHOOSES the format, and returning 0 aborts the video output; libVLC 3
     * does not call setup() again with an alternative fourcc (vmem.c,
     * Open()). NV12 is supported because some decoders PROPOSE it - the
     * converter is tested in tools/tests/yuv.c - not via a second
     * negotiation. */
    if (memcmp(chroma, "I420", 4) == 0) {
        /* Ask for the OTHER 4:2:0 layout. When the requested format matches
         * the decoder's exactly, VLC enables direct rendering: the decoder
         * takes its frames from the video-output pool instead of its own.
         * With vmem that pool path fails here - libavcodec reports
         * "get_buffer() failed / thread_get_buffer() failed" and the vout
         * restarts in a loop - even for a 320x176 clip, and the same clip
         * played fine while the vout asked for RV32. Requesting NV12 against
         * an I420 decoder (or I420 against an NV12 one) is a format mismatch,
         * so direct rendering stays off, the decoder allocates its own
         * frames, and vmem does a chroma-only convert with no scaling. Both
         * layouts are what src/yuv.h samples, so the plane path stays. */
        nv12 = 0;                       /* keep the decoder's own layout */
    } else if (memcmp(chroma, "NV12", 4) == 0) {
        nv12 = 1;
    } else if (memcmp(chroma, "YV12", 4) == 0 || memcmp(chroma, "YU12", 4) == 0) {
        memcpy(chroma, "I420", 4);      /* same planes, U and V swapped */
        nv12 = 0;
    } else {
        /* A hardware surface (VAOP, VDV0, DXVA, VT..): a GPU decoder cannot
         * hand us CPU planes and there is no GPU in the guest, so refuse it
         * and let VLC pick a software decoder. */
        fprintf(stderr, "vlcplay: refusing vout format %.4s (not 8-bit 4:2:0, "
                        "probably a hardware surface); VLC will fall back"
                        "%s", chroma, "\n");
        return 0;
    }

    if (surf_set_planes(s, w, h, nv12) != 0) {
        fprintf(stderr, "vlcplay: cannot allocate %dx%d %s planes\n",
                w, h, nv12 ? "NV12" : "I420");
        return 0;
    }

    pthread_mutex_lock(&s->m);
    pitches[0] = (unsigned)s->pitches[0];
    pitches[1] = (unsigned)s->pitches[1];
    pitches[2] = nv12 ? 0u : (unsigned)s->pitches[2];
    lines[0] = (unsigned)s->lines[0];
    lines[1] = (unsigned)s->lines[1];
    lines[2] = nv12 ? 0u : (unsigned)s->lines[2];
    pthread_mutex_unlock(&s->m);

    /* vmem.c passes &sys->opaque to setup() and then hands the same
     * sys->opaque to lock/unlock/display, so publishing the surface here
     * is what the other callbacks receive. (It is also the pointer
     * video_set_callbacks() was given, so this is belt and braces.) */
    *opaque = s;
    fprintf(stderr, "vlcplay: vmem setup %dx%d, decoder offered %.4s, "
                    "using %s, pitches %u/%u/%u\n",
            w, h, offered, nv12 ? "NV12" : "I420",
            pitches[0], pitches[1], pitches[2]);
    return nv12 ? 2u : 3u;
}

static void cb_cleanup(void *opaque) {
    VlcSurface *s = (VlcSurface *)opaque;
    void *mem[VLC_SETS];
    int k;

    if (!s) return;
    pthread_mutex_lock(&s->m);
    for (k = 0; k < VLC_SETS; k++) {
        while (s->set[k].in_vlc || s->set[k].reading)
            pthread_cond_wait(&s->cv, &s->m);
        mem[k] = s->set[k].mem;
        s->set[k].mem = NULL;
        s->set[k].planes[0] = s->set[k].planes[1] = s->set[k].planes[2] = NULL;
        s->set[k].complete = 0;
    }
    s->ready = -1;
    s->w = s->h = 0;
    pthread_mutex_unlock(&s->m);
    for (k = 0; k < VLC_SETS; k++) free(mem[k]);
}

static void *cb_lock(void *opaque, void **planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    VlcPlanes *ps;
    int idx;

    if (!s) return NULL;
    pthread_mutex_lock(&s->m);
    if (!s->lock_calls) {
        fprintf(stderr, "vlcplay: vmem lock #1 (%dx%d %s)\n",
                s->w, s->h, s->nv12 ? "NV12" : "I420");
    }
    s->lock_calls++;
    idx = s->write_idx;
    ps = &s->set[idx];
    while (ps->in_vlc || ps->reading) pthread_cond_wait(&s->cv, &s->m);
    ps->in_vlc = 1;
    ps->complete = 0;
    s->write_idx = (s->write_idx + 1) % VLC_SETS;
    if (planes) {
        planes[0] = ps->planes[0];
        planes[1] = ps->planes[1];
        planes[2] = ps->planes[2];
    }
    pthread_mutex_unlock(&s->m);
    return ps;                         /* vmem hands this back to unlock() */
}

static void cb_unlock(void *opaque, void *picture, void *const *planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    VlcPlanes *ps = (VlcPlanes *)picture;
    (void)planes;
    if (!s) return;
    pthread_mutex_lock(&s->m);
    if (ps >= &s->set[0] && ps < &s->set[VLC_SETS]) {
        ps->in_vlc = 0;
        ps->complete = 1;
        if (!s->frames) {
            s->first_frame_ms = now_ms();
            s->rate_ms = s->first_frame_ms;
        }
        s->frames++;
        ps->seq = s->frames;
        s->ready = (int)(ps - &s->set[0]);
        if (s->frames == 1)
            fprintf(stderr, "vlcplay: vmem unlock #1 (%dx%d)\n", s->w, s->h);
    }
    /* The rate the decoder actually sustains is a number, not an opinion.
     * Two numbers, because the average since start hides a stall: the
     * instantaneous rate over the last hundred pictures, then the average.
     * (The old report showed only the average, which could keep reading
     * 17 fps for minutes after the real rate had changed.) */
    if (s->frames && (s->frames % 100) == 0) {
        long long t = now_ms();
        long long span = t - s->rate_ms;
        long long total = t - s->first_frame_ms;
        unsigned df = s->frames - s->rate_frames;
        if (span > 0 && total > 0)
            fprintf(stderr, "vlcplay: %u frames: %.1f fps over the last %u "
                            "frames, %.1f fps average since start\n",
                    s->frames, (double)df * 1000.0 / (double)span, df,
                    (double)s->frames * 1000.0 / (double)total);
        s->rate_ms = t;
        s->rate_frames = s->frames;
    }
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

/* The plane set's own size, for the footer label before libVLC has
 * answered video_get_size(). */
static void surf_size(VlcSurface *s, int *w, int *h) {
    pthread_mutex_lock(&s->m);
    *w = s->w;
    *h = s->h;
    pthread_mutex_unlock(&s->m);
}

/* The size to lay out and sample: libVLC's visible size when BOTH axes of
 * it fit inside the plane set, otherwise the plane set itself.
 *
 * video_get_size() is the TRACK's size. vmem's setup() receives the format
 * after rotation has been applied, so a 90-degree MP4 reports 1920x1080
 * from get_size while its planes are 1090x1920: one axis is larger than
 * the plane, and the fit-both test rejects the crop. Padding is always an
 * overshoot on the same axes (320x194 planes for a 320x176 picture,
 * 3840x2178 for 3840x2160), so the visible size is used and the padding
 * rows/columns are never sampled. This is the same "does the visible
 * rectangle fit inside the surface" rule src/client/apps/vlcplayer.c uses;
 * an aspect-ratio tolerance was tried here and rejected because the
 * bundled 320x176-in-320x194 clip is a 10% aspect difference, not 2%.
 * Measured with libVLC 3.0.24 on clip176.mp4, a tkhd-rotated MP4 and 4K. */
static void surf_display_size(VlcSurface *s, int vw, int vh,
                              int *w, int *h) {
    int pw, ph;
    surf_size(s, &pw, &ph);
    *w = pw;
    *h = ph;
    if (vw >= 2 && vh >= 2 && vw <= pw && vh <= ph) {
        *w = vw;
        *h = vh;
    }
}

/* Convert the newest complete picture into the (dx,dy,dw,dh) rectangle of
 * dst, sampling at destination resolution: for each of the dw*dh window
 * pixels the source pixel is picked by the scale factor and only that
 * pixel's Y/U/V are read. For a 4K source in a 960x540 body that is
 * 518,400 reads instead of 8,294,400 converted pixels - the entire reason
 * the old path's per-frame cost grew with the source size rather than with
 * the window.
 *
 * THE COST IS THE BODY, AND IT IS THE LIMIT TO KNOW. Measured with this
 * function at the shipping -Os on a 16-thread i5-1240P, from a 4K source
 * (ranges over repeated runs, because the machine throttles):
 *
 *     960x540   2.1-3.6 ms        1100x668  3.0-5.4 ms
 *     1280x700  3.5-5.9 ms        1920x980  7.9-13.0 ms
 *
 * At 60 fps the budget is 16.7 ms. A body up to the default desktop's
 * 1280x700 fits with room for the HUD; a full-screen 1920x980 window
 * (the panel-clamped maximum: 1920x1080 minus the 36 px header and 64 px
 * footer) spends roughly half to three quarters of the budget in this
 * scalar conversion alone. That is a real limit of sampling on the window
 * thread, and it is why the converter is a pure function in src/yuv.h:
 * it can be optimised (or given bilinear taps) without touching the vmem
 * plumbing.
 *
 * The set is marked `reading` while the conversion runs: libVLC's next
 * lock() takes one of the other sets in the ring, and waits only if it
 * catches up with this one.
 *
 * visw/vish crop the decoder's padded picture to libVLC's visible size
 * (0 means "use the whole plane set"). Returns the frame count that was
 * converted, or 0 when no picture exists yet. */
/* The rate the decoder actually sustains, reported by both paths. Two
 * numbers, because the average since start hides a stall: the instantaneous
 * rate over the last hundred pictures, then the average. */
static void report_rate(VlcSurface *s) {
    if (!s->frames || (s->frames % 100) != 0) return;
    {
        long long t = now_ms();
        long long span = t - s->rate_ms;
        long long total = t - s->first_frame_ms;
        unsigned df = s->frames - s->rate_frames;
        if (span > 0 && total > 0)
            fprintf(stderr, "vlcplay: %u frames: %.1f fps over the last %u, "
                            "%.1f fps average%s\n",
                    s->frames, (double)df * 1000.0 / (double)span, df,
                    (double)s->frames * 1000.0 / (double)total,
                    s->rv32 ? " (RV32)" : " (planes)");
        s->rate_ms = t;
        s->rate_frames = s->frames;
    }
}

/* ---- the RV32 path -------------------------------------------------------
 *
 * libVLC converts and scales each picture to RGB32 at the size we ask for and
 * this process copies rows into the window. The conversion happens on the
 * video-output thread, which is why a 4K file costs more here than the plane
 * path was supposed to cost - but it is the arrangement that works with the
 * guest's libVLC, and it measured 60.0 fps on a 1080p60 file and the file's
 * own 23.976 fps on 4K.
 */
static int surf_init_rv32(VlcSurface *s, int w, int h) {
    size_t bytes;
    if (w < 2 || h < 2) return -1;
    s->rv32 = 1;
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    s->fb_raw = malloc(bytes + VLC_MIN_PLANE_ALIGN);
    if (!s->fb_raw) return -1;
    s->fb = (uint32_t *)(((uintptr_t)s->fb_raw + VLC_MIN_PLANE_ALIGN - 1u) &
                         ~((uintptr_t)VLC_MIN_PLANE_ALIGN - 1u));
    memset(s->fb, 0, bytes);
    return 0;
}

static void *cb_lock_rv32(void *opaque, void **planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    pthread_mutex_lock(&s->m);
    while (s->busy) pthread_cond_wait(&s->cv, &s->m);
    s->busy = 1;
    pthread_mutex_unlock(&s->m);
    if (planes) planes[0] = s->fb;
    return s->fb;
}

static void cb_unlock_rv32(void *opaque, void *picture, void *const *planes) {
    VlcSurface *s = (VlcSurface *)opaque;
    (void)picture;
    (void)planes;
    pthread_mutex_lock(&s->m);
    s->busy = 0;
    if (!s->frames) {
        s->first_frame_ms = now_ms();
        s->rate_ms = s->first_frame_ms;
    }
    s->frames++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    report_rate(s);
}

static unsigned surf_blit_rv32(VlcSurface *s, uint32_t *dst, int dstride,
                               int dx, int dy, int dw, int dh) {
    unsigned seq;
    int sw, sh;
    if (!s->fb || dw <= 0 || dh <= 0) return 0;
    pthread_mutex_lock(&s->m);
    while (s->busy) pthread_cond_wait(&s->cv, &s->m);
    if (!s->frames) {
        pthread_mutex_unlock(&s->m);
        return 0;
    }
    s->busy = 1;
    seq = s->frames;
    sw = s->w;
    sh = s->h;
    pthread_mutex_unlock(&s->m);

    if (dw == sw && dh == sh) {
        int y;
        for (y = 0; y < sh; y++)
            memcpy(dst + (size_t)(dy + y) * (size_t)dstride + dx,
                   s->fb + (size_t)y * (size_t)sw, (size_t)sw * 4u);
    } else {
        uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / (uint64_t)dw);
        int x, y;
        for (y = 0; y < dh; y++) {
            int sy = (int)(((uint64_t)y * (uint64_t)sh) / (uint64_t)dh);
            const uint32_t *srow = s->fb + (size_t)sy * (size_t)sw;
            uint32_t *drow = dst + (size_t)(dy + y) * (size_t)dstride + dx;
            uint32_t fx = 0;
            for (x = 0; x < dw; x++) {
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

static unsigned surf_blit(VlcSurface *s, uint32_t *dst, int dstride,
                          int dx, int dy, int dw, int dh,
                          int visw, int vish) {
    unsigned seq;
    VlcPlanes *ps;
    uint8_t *yp, *up, *vp;
    if (s && s->rv32) return surf_blit_rv32(s, dst, dstride, dx, dy, dw, dh);
    int ypitch, upitch, vpitch, sw, sh, nv12;

    if (!s || !dst || dw <= 0 || dh <= 0 || dx < 0 || dy < 0) return 0;

    pthread_mutex_lock(&s->m);
    /* The set unlock() published is the newest complete picture. Wait until
     * libVLC has finished copying into it (it is writing at most one set),
     * then read it. With VLC_SETS sets in the ring the vout still has two
     * other sets for its next pictures, so this wait does not stall the
     * video-output thread; and returning the newest sequence number means
     * the app converts each frame once instead of chasing older sets. */
    while (s->ready >= 0 && s->set[s->ready].in_vlc)
        pthread_cond_wait(&s->cv, &s->m);
    if (s->ready < 0 || s->frames == 0 || !s->set[s->ready].planes[0] ||
        s->set[s->ready].in_vlc) {
        pthread_mutex_unlock(&s->m);
        return 0;
    }
    ps = &s->set[s->ready];
    ps->reading = 1;                   /* libVLC must not reuse this set */
    seq = ps->seq;
    yp = ps->planes[0];
    up = ps->planes[1];
    vp = ps->planes[2];
    ypitch = s->pitches[0];
    upitch = s->pitches[1];
    vpitch = s->pitches[2];
    sw = s->w;
    sh = s->h;
    nv12 = s->nv12;
    pthread_mutex_unlock(&s->m);

    if (visw > 0 && visw <= sw) sw = visw;
    if (vish > 0 && vish <= sh) sh = vish;

    if (nv12)
        nv12_to_rgb_nearest_stride(yp, up, ypitch, upitch, sw, sh,
                                   dst + (size_t)dy * (size_t)dstride +
                                         (size_t)dx,
                                   dstride, dw, dh);
    else
        yuv420_to_rgb_nearest_stride(yp, up, vp, ypitch, upitch, vpitch,
                                     sw, sh,
                                     dst + (size_t)dy * (size_t)dstride +
                                           (size_t)dx,
                                     dstride, dw, dh);

    pthread_mutex_lock(&s->m);
    ps->reading = 0;
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

/* The volume the footer shows is libVLC's own answer: the output's getter,
 * re-read on every status tick, so a change made anywhere (including a
 * stream that carries its own gain) is what appears. Measured with the real
 * library (host VLC 3.0.24, live audio output): set_volume(100/50/20/200/0)
 * returned 0 every time and get_volume() answered 100/50/20/200/0 once the
 * output had applied it - the same numbers, so the getter is a usable
 * source and the footer does not have to echo its own request. Two facts
 * that shape the code below: the getter answers -1 while no audio output
 * exists yet (libvlc_media_player.h documents that), and the output applies
 * a set asynchronously, so a getter call immediately after set_volume()
 * still reports the old value. Hence "vol n/a" until the first answer, and
 * the key handler showing its request for one tick before the truth lands. */
static void vp_refresh_volume(VlcPlayer *p) {
    int got;
    if (!p->mp || !p->api.audio_get_volume) return;
    got = p->api.audio_get_volume(p->mp);
    if (got < 0) return;              /* no output yet: no answer to show */
    p->vol = got;
    p->vol_known = 1;
}

/* A fresh libvlc player reports the audio output's own idea of the volume.
 * On a device nobody has used that is 0 - silent video - and libVLC also
 * remembers the volume it saved when a previous process exited. So ask for
 * the nominal 100% once at play, and once more when playback has actually
 * started, because the audio output may only exist by then. After that a
 * failure is real: there is no audio output.
 *
 * The read-back here can still be the output's pre-set value for one
 * moment (the output applies a set asynchronously - measured on the host:
 * a previous process had saved 37, so the first getter answer after the
 * set was 37 and the next status tick showed 100). That is why the footer
 * is refreshed on every tick instead of being written once: what it shows
 * is the output's answer, and the answer settles. */
static void vp_start_volume(VlcPlayer *p, int final) {
    if (!p->mp || p->vol_init || p->audio_dead) return;
    if (p->api.audio_set_volume(p->mp, 100) >= 0) {
        p->vol_init = 1;
        vp_refresh_volume(p);         /* show what the output holds */
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

/* The same words the card uses, on stderr, so a headless run and the window
 * never disagree. A library that loaded but lacks a symbol is NOT "not
 * installed": vlc_bind() names the symbol in lib_detail, and the sentence
 * here is the one the card draws (VLC_BAD_SYMBOL_MSG). Kept as a function so
 * the host harness can capture and compare the bytes instead of trusting a
 * second copy of the wording. */
static void report_load_failure(const VlcPlayer *p, FILE *out) {
    if (!p || !out || p->lib_ok) return;
    fprintf(out, "vlcplay: %s\n",
            p->symbol_missing ? VLC_BAD_SYMBOL_MSG : VLC_MISSING_MSG);
    fprintf(out, "vlcplay: %s\n",
            p->lib_detail[0] ? p->lib_detail : "libVLC could not be loaded");
}

/* Open the library, create the instance, and - if a path was given -
 * build the media and the player and start it. Returns 0 when the window
 * has something real to show (even if that something is "libVLC is
 * missing" or "give me a file"); -1 only when memory or the surface
 * failed. */
static int vp_open(VlcPlayer *p, const char *media_path,
                   int body_w, int body_h) {
    memset(p, 0, sizeof(*p));

    /* No pixel buffer is allocated here: the size and the chroma are not
     * known until libVLC calls setup() with the decoder's format. surf_init
     * only creates the lock that the callbacks and the window thread
     * share. */
    if (surf_init(&p->surf) != 0) return -1;

    vlc_load(p);

    if (p->lib_ok && media_path && *media_path) {
        static const char *const opts[] = {
            "--no-video-title-show",   /* VLC's OSD must not land in the frame */
            "--avcodec-hw=none",       /* a GPU frame cannot be handed to vmem */
            /* Direct rendering makes the decoder take its frames from the
             * video-output pool instead of allocating them. With vmem that
             * path fails in the guest: lavc_UpdateVideoFormat() returns
             * non-zero and libavcodec prints "get_buffer() failed /
             * thread_get_buffer() failed", so the vout restarts forever and
             * nothing is ever shown - even a 320x176 clip, and even when the
             * requested chroma matches the decoder's. Turning DR off makes
             * avcodec allocate its own frames (avcodec_default_get_buffer2)
             * and vmem copies them into the planes we hand it. */
            "--no-avcodec-dr",
        };
        libvlc_instance_t *inst;

        inst = p->api.new_instance(3, opts);
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

        /* vmem in the setup/cleanup form: libVLC hands us the decoder's
         * own planes and picture size, and setup() answers with the I420
         * (or NV12) layout the blit samples. Nothing here asks for RGB or
         * for a resize, so the video-output thread only copies plane
         * data. */
        /* Which display path. RV32 is the default: it is the one measured at
         * 60.0 fps on a 1080p60 file and at the source's own 23.976 fps on a
         * 4K file, and it is what the guest's libVLC 3.0.23 actually shows.
         * The plane path is kept and can be selected with
         * MYSTIC_VLC_PLANES=1; it could not be made to present a picture
         * under 3.0.23 (the vout is set up and then re-opened forever, with
         * no lock() call ever reaching us), while the same source measures
         * correctly against libVLC 3.0.24 on a developer machine. */
        {
            const char *want_planes = getenv("MYSTIC_VLC_PLANES");
            int planes = want_planes && *want_planes && *want_planes != '0';
            if (!planes) {
                p->surf.rv32 = 1;
                if (surf_init_rv32(&p->surf, body_w, body_h) != 0) return -1;
                p->api.video_set_callbacks(p->mp, cb_lock_rv32, cb_unlock_rv32,
                                           cb_display, &p->surf);
                p->api.video_set_format(p->mp, "RV32", (unsigned)body_w,
                                        (unsigned)body_h,
                                        (unsigned)body_w * 4u);
            } else {
                p->api.video_set_callbacks(p->mp, cb_lock, cb_unlock, cb_display,
                                           &p->surf);
                p->video_set_format_callbacks(p->mp, cb_setup, cb_cleanup);
            }
        }

        if (p->api.player_play(p->mp) != 0) {
            save_err(p, "libvlc_media_player_play() failed");
            return 0;
        }
        p->started = 1;
        /* Playback-rate hook, for measurement and for speed control:
         * MYSTIC_PLAYBACK_RATE=2.5 makes libVLC present from a 23.976 fps
         * 4K file at roughly 60 pictures a second, which is how throughput
         * above the source rate is shown without a 60 fps 4K file. */
        {
            const char *rs = getenv("MYSTIC_PLAYBACK_RATE");
            if (rs && *rs) {
                double rate = atof(rs);
                if (rate >= 0.10 && rate <= 8.0 && p->api.player_set_rate) {
                    if (p->api.player_set_rate(p->mp, (float)rate) == 0)
                        fprintf(stderr, "vlcplay: playback rate %.2f\n", rate);
                    else
                        fprintf(stderr, "vlcplay: playback rate %.2f refused\n", rate);
                }
            }
        }
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

/* Multiply the playback rate, clamped to libVLC's sane range, and say what
 * it became. Prints because the HUD is redrawn at most a few times a second
 * and the serial log is where a measurement is read from. */
static void vp_rate(VlcPlayer *p, float factor) {
    static float rate = 1.0f;
    if (!p->mp || !p->api.player_get_rate || !p->api.player_set_rate) return;
    rate = p->api.player_get_rate(p->mp);
    if (rate < 0.10f || rate > 8.0f) rate = 1.0f;
    rate *= factor;
    if (rate < 0.10f) rate = 0.10f;
    if (rate > 8.0f) rate = 8.0f;
    if (p->api.player_set_rate(p->mp, rate) == 0)
        fprintf(stderr, "vlcplay: playback rate %.2f\n", (double)rate);
    else
        fprintf(stderr, "vlcplay: playback rate %.2f refused\n", (double)rate);
}

static void vp_toggle_pause(VlcPlayer *p) {
    libvlc_state_t st;
    if (!p->mp) return;
    st = p->api.player_get_state(p->mp);
    if (st == libvlc_Ended || st == libvlc_Stopped || st == libvlc_Error) {
        /* Restart, measured rather than assumed. On an Ended player
         * set_time(0) + play() returns 0 and leaves the state Ended with the
         * clock at 0 - i.e. the key does nothing while claiming success. Only
         * stop() clears the input so the next play() starts a new one. With
         * the real libVLC 3.0.24 on the host:
         *
         *   set_time(0)+play(): 2 s WAV -> ended (time 0)
         *                       320x176 MP4 -> ended (time 0)
         *   stop()+play():      2 s WAV -> playing (1908 ms after 2 s)
         *                       320x176 MP4 -> playing (1751 ms after 2 s)
         *
         * The card promises that space or a click starts the file again, so
         * this branch has to keep that promise. A/B: .work/p1/restartprobe.c;
         * regression test (space and click, both media): .work/p1/restart.c */
        p->api.player_stop(p->mp);
        p->last_err[0] = 0;          /* the previous failure is history now */
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
    libvlc_state_t st;
    if (!p->mp || !p->started) return;
    /* Seeking a player that is not on a timeline is meaningless, and on
     * VLC 3 it is worse than meaningless: set_time() is accepted for a
     * media that never opened, and get_time() then reports the number we
     * asked for, so the footer would show a position the file never
     * played. Refuse instead of inventing a clock. */
    st = p->api.player_get_state(p->mp);
    if (st == libvlc_Ended || st == libvlc_Stopped || st == libvlc_Error ||
        st == libvlc_NothingSpecial)
        return;
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
    /* Show the new value at once so held/repeated keys accumulate from it,
     * then let the next status tick replace it with the output's own answer
     * (vp_refresh_volume). Measured on the host: the output applies a
     * volume change asynchronously, so an immediate getter call still
     * reports the previous value - reading back here would throw away the
     * key press. */
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
    int  card_played;           /* the card was drawn with len>0 + ended */
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
    int body_y = imin(HEAD_H, h);
    if (foot_y < HEAD_H + BODY_MIN_H) foot_y = HEAD_H + BODY_MIN_H;
    if (foot_y > h) foot_y = h;

    /* The three bands must tile the surface exactly, at every height:
     * head.y == 0, body.y == head.y + head.h, foot.y == body bottom and
     * foot.y + foot.h == h. body_y is clamped to h for a window that is
     * shorter than the header (a dragged-to-nothing window); pinning it to
     * HEAD_H instead would leave body.y above the surface and foot.h computed
     * from a negative height. .work/p1/layout.c checks all of it, 1..300. */
    L->head.x = 0; L->head.y = 0; L->head.w = w;
    L->head.h = imin(HEAD_H, h);
    L->body.x = 0; L->body.y = body_y; L->body.w = w;
    L->body.h = imax(0, foot_y - body_y);
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
    /* Whether the media played to its end; kept on the app so the render
     * loop can tell a card that was drawn before libVLC had parsed the
     * duration from one that already knows the answer. */
    int played_out = 0;

    surf_fill(&a->s, b.x, b.y, b.w, b.h, C_BG);

    if (!a->pl.lib_ok) {
        /* Title, then the sentence that is actually true, then the real
         * loader reason, then where we looked.
         *
         * "libVLC is not installed" is only true when nothing loaded. When
         * libvlc.so.5 DID load and a symbol was missing, the library is
         * present - a truncated or foreign build - and telling the user it
         * is not installed would be false. The reinstall advice stays the
         * same, in pkg's own words. */
        y = b.y + b.h / 2 - 96;
        txt(&a->s, t, x, y,
            a->pl.symbol_missing ? "This libVLC cannot be used"
                                 : "libVLC is not installed", C_WARN);
        y += (int)font_height(t) + 10;
        wrap_text(&a->s, bdy, x, y, w, lh, 2,
                  a->pl.symbol_missing ? VLC_BAD_SYMBOL_MSG
                                       : VLC_MISSING_MSG,
                  C_TEXT);
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
        /* Ended/Stopped with no picture means one of two different things,
         * and only libVLC's own clock can tell them apart:
         *
         *   ended with no duration -> it never played. A missing path lands
         *     here: VLC 3 reports Ended, and get_length() answers 0 because
         *     no demuxer ever opened the media.
         *   ended with a duration -> it played TO THE END. An audio-only file
         *     does exactly this, and calling that "did not play this file"
         *     would be a lie about a file that played.
         *
         * The DURATION is the signal, not the position: libVLC resets the
         * clock to 0 at end of stream and whenever a restart is requested, so
         * a zero position says nothing. A non-zero duration cannot be
         * invented for media that never opened - measured on the host: a
         * path that does not exist answers length=0, the 2 s WAV answers
         * length=2000 at the same state. */
        int64_t len = a->pl.mp ? a->pl.api.player_get_length(a->pl.mp) : -1;
        played_out = (st == libvlc_Ended && len > 0);
        int dead = (st == libvlc_Error || st == libvlc_Ended ||
                    st == libvlc_Stopped);

        y = b.y + b.h / 2 - 70;
        if (played_out) {
            txt(&a->s, t, x, y, "Playback ended", C_TEXT);
        } else if (dead) {
            txt(&a->s, t, x, y, "libVLC did not play this file", C_WARN);
        } else if (a->pl.started &&
                   (st == libvlc_Playing || st == libvlc_Paused)) {
            /* Playing with no picture yet. "Opening" would be stale once
             * the clock is running; "no video" would claim too much, so
             * say exactly what libVLC reports and nothing else. */
            char shown[640];
            snprintf(shown, sizeof(shown), "Playing %s - no picture",
                     leaf(a->pl.media_path));
            elide(line, sizeof(line), shown, t, w);
            txt(&a->s, t, x, y, line, C_TEXT);
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
                 !a->pl.started ? "not started"
                 : played_out ? "no picture decoded"
                              : "no picture decoded yet");
        wrap_text(&a->s, bdy, x, y, w, lh, 2, line, C_DIM);
        y += lh * 2;

        if (err && *err) {
            lines = wrap_text(&a->s, sm, x, y, w,
                              (int)(font_height(sm) + 4), 4, err, C_WARN);
            y += lines * (int)(font_height(sm) + 4);
        } else if (played_out) {
            lines = wrap_text(&a->s, sm, x, y, w,
                              (int)(font_height(sm) + 4), 3,
                              "libVLC played this file to its end (the clock "
                              "above is its own) but decoded no video track, "
                              "so there is no picture to show. Space or a "
                              "click starts it again.",
                              C_DIM);
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
    a->card_played = played_out;
}

static void draw_video(PlayerApp *a, Layout *L, int full) {
    Rect b = L->body;
    Rect v;
    unsigned seq = 0;
    int copied = 0;

    /* Layout invariant: layout_window() never lets the body reach past the
     * surface. foot_y = min(max(h - FOOT_H, HEAD_H + BODY_MIN_H), h), so
     * body.y + body.h == foot_y <= h always, and the rows this function
     * copies (which bypass surf_set()'s bounds check on purpose, because a
     * frame must not pay for a per-pixel clip test) stay inside the mapping.
     * .work/p1/layout.c checks that for every window height, so a change to
     * layout_window() cannot quietly turn this into an out-of-bounds write. */

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
                        v.x, v.y, v.w, v.h,
                        a->pl.vid_w, a->pl.vid_h);
        copied = seq != 0;
    } else if (a->drawn_seq != surf_frames(&a->pl.surf)) {
        seq = surf_blit(&a->pl.surf, a->s.px, a->s.stride,
                        v.x, v.y, v.w, v.h,
                        a->pl.vid_w, a->pl.vid_h);
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
    else {
        /* No track size yet: show the plane-set size libVLC's setup()
         * negotiated, which is the decoder's coded size. */
        int pw = 0, ph = 0;
        surf_size(&a->pl.surf, &pw, &ph);
        if (pw > 0 && ph > 0)
            snprintf(line, sizeof(line), "%dx%d buffer", pw, ph);
        else
            snprintf(line, sizeof(line), "--");
    }
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
         * "opening" turns into the real error or ended state on screen, and
         * also when the played-to-the-end verdict changes. That second test
         * matters: libVLC reports Ended first and only answers get_length()
         * once the input has been torn down, so a card drawn at the instant
         * of the transition can still see length=0. Without the refresh the
         * window would keep saying "did not play this file" about a file
         * that played, which is exactly the kind of false statement this app
         * must not make. */
        libvlc_state_t st = a->pl.mp ? a->pl.api.player_get_state(a->pl.mp)
                                     : libvlc_NothingSpecial;
        int played = 0;
        if (a->pl.mp && st == libvlc_Ended)
            played = a->pl.api.player_get_length(a->pl.mp) > 0;
        if (a->body_mode != 2 || a->card_state != (int)st ||
            a->card_played != played) {
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
    /* Playback speed. Not a test-only path: it is the speed control any
     * player has, and it is also how 4K60 throughput is demonstrated
     * without a 60 fps 4K file - 2.5x a 23.976 fps source makes libVLC
     * present about 60 pictures a second. */
    case KEY_RIGHTBRACKET:
        vp_rate(&a->pl, 1.25f);
        a->last_hud_ms = 0;
        break;
    case KEY_LEFTBRACKET:
        vp_rate(&a->pl, 1.0f / 1.25f);
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
    long long last_size_ms = 0;
    int body_w = 960, body_h = 540;

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

    /* The RV32 path asks libVLC for the body size, so the conversion is the
     * size of the window rather than of the source. The plane path ignores
     * these two and samples the decoder's own planes instead. */
    {
        Layout L;
        layout_window(a.win_w, a.win_h, &L);
        body_w = (L.body.w / 8) * 8;
        if (body_w < 16) body_w = 16;
        if (body_w > 1920) body_w = 1920;
        body_h = L.body.h & ~1;
        if (body_h < 16) body_h = 16;
        if (body_h > 1080) body_h = 1080;
    }

    if (vp_open(&a.pl, media, body_w, body_h) != 0) {
        fprintf(stderr, "vlcplay: cannot allocate the plane sets\n");
        mc_close(&a.c);
        return 1;
    }
    report_load_failure(&a.pl, stderr);

    draw_all(&a);
    a.last_hud_ms = now_ms();

    while (!a.quit) {
        Layout L;
        MEvent ev;
        long long now;
        libvlc_state_t st = libvlc_NothingSpecial;
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
            st = a.pl.api.player_get_state(a.pl.mp);

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
                (st == libvlc_Playing || st == libvlc_Paused)) {
                /* Playback really started, so the audio output exists now
                 * (or never will): this second attempt is final. A file
                 * with no video track has no frames but does have sound. */
                vp_start_volume(&a.pl, 1);
                want_footer = 1;
            }
            if (a.pl.started &&
                now - a.last_hud_ms >= (a.pl.paused ? 250 : 100)) {
                vp_refresh_volume(&a.pl);   /* the output's own number */
                want_footer = 1;
                a.last_hud_ms = now;
            }
        }

        /* One FRAME per pass, whichever bands changed. */
        if (want_footer) draw_footer_only(&a);
        if (draw_body_if_needed(&a) || want_footer) mc_present(&a.c);

        /* Sleep for the next frame, or lazily when nothing is moving. */
        if (a.pl.mp && a.pl.started) {
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

#endif /* !VLCPLAY_BOOT - end of the player build */
