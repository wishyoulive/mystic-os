/* ============================================================
 *  Mystic OS - mystic-desktop
 *
 *  The desktop and the window manager. Everything on screen is
 *  drawn by the CPU from real measurements: resolution from fb0,
 *  memory and processes from /proc, input from evdev.
 *
 *  Keys:   1 2 3   switch theme
 *          Esc     close focused window
 *  Mouse:  drag a title bar to move a window, click a dock icon
 *          to launch an app, click a window's red dot to close it.
 * ============================================================ */
#include <signal.h>
#include <sys/stat.h>

#include "mystic.h"
#include "ipc.h"
#include "wallpaper.h"
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define THEME_DIR "/etc/mystic/themes"
#define FONT_DIR  "/usr/share/fonts"
#define MAX_WINDOWS 10

/* Every metric that has to follow the text scale. Nothing below is a
 * hardcoded pixel height, which is what made scaling unsafe before. */
static int title_h(void) { return ui_px(40); }      /* window title bar */
static int topbar_h(void) { return ui_px(34); }     /* screen top bar   */

/* Traffic-light diameter. Fixed on purpose: only its position follows the
 * scale. Draw and click must use the same centre, or the target drifts. */
#define DOT_SZ 10
/* F12: the dots, the title text and the click targets all hang off this one
 * vertical centre, so nothing drifts when the text scale changes the bar. */
static int traffic_y(int bar_h) { return (bar_h - DOT_SZ) / 2; }
/* A bundle used to be a letter in a box ("C", "F", "2"), which is why the
 * launcher looked unfinished. Each known app now has the same kind of drawn
 * icon the built-ins have; anything unknown still falls back to its initial. */
static void icon_terminal(Surface*,float,float,float,u32);
static void icon_folder(Surface*,float,float,float,u32);
static void icon_gear(Surface*,float,float,float,u32);
static void icon_clock(Surface*,float,float,float,u32);
static void icon_paint(Surface*,float,float,float,u32);
static void icon_life(Surface*,float,float,float,u32);
static void icon_mandel(Surface*,float,float,float,u32);
static void icon_sysmon(Surface*,float,float,float,u32);
static void icon_calc(Surface*,float,float,float,u32);
static void icon_grid(Surface*,float,float,float,u32);
static void icon_bars(Surface*,float,float,float,u32);
static void icon_pencil(Surface*,float,float,float,u32);
static void icon_web(Surface*,float,float,float,u32);
static void icon_2048(Surface*,float,float,float,u32);
static void icon_sysinfo(Surface*,float,float,float,u32);
typedef void (*IconFn)(Surface *, float, float, float, u32);
static IconFn icon_for_bundle(const char *id) {
    if (!id)                       return NULL;
    if (!strcmp(id, "clock"))      return icon_clock;
    if (!strcmp(id, "files"))      return icon_folder;
    if (!strcmp(id, "g2048"))      return icon_2048;
    if (!strcmp(id, "life"))       return icon_life;
    if (!strcmp(id, "mandel"))     return icon_mandel;
    if (!strcmp(id, "paint"))      return icon_paint;
    if (!strcmp(id, "sysmon"))     return icon_sysmon;
    if (!strcmp(id, "web"))        return icon_web;
    if (!strcmp(id, "sysinfo"))    return icon_sysinfo;
    if (!strcmp(id, "settings"))   return icon_gear;
    if (!strcmp(id, "monitor"))    return icon_bars;
    if (!strcmp(id, "notes"))      return icon_pencil;
    if (!strcmp(id, "store"))      return icon_grid;
    if (!strcmp(id, "calc"))       return icon_calc;
    return NULL;
}

/* The dock is as wide as the windows in it. Ten windows at Huge text were
 * wider than an 800px screen, and the icons painted off the left edge could
 * not be clicked at all. dock_rect() lowers this percentage until the bar
 * fits; draw and hit-test both read it, so they cannot disagree. */
static int dock_pct = 100;

static int dock_isz(void) { return ui_px(40) * dock_pct / 100; }  /* taskbar icon */
static int dock_gap(void) { return ui_px(16) * dock_pct / 100; }
static int dock_pad(void) { return ui_px(12); }

static Display disp;
static Surface screen;
static Surface scene;
static Input   input;
static Theme   themes[16];   /* ten are shipped; the array used to hold 8 */
static int     theme_count, theme_index;

/* ------------------------------------------------------------ windows */
enum { WIN_TERM = 0, WIN_APP, WIN_CLIENT };

typedef struct {
    int   used;
    int   kind;
    char  title[64];
    int   x, y, w, h;
    App  *app;
    Term *term;
    int   client;        /* index into clients[] when kind == WIN_CLIENT */
    int   hidden;        /* minimised: still alive, not drawn, not hit-tested */
    int   maxed;         /* maximised by double-click */
    int   restore_x, restore_y, restore_w, restore_h;
    int   z;
    unsigned gen;        /* slot generation: bumped whenever the slot changes hands */
} Window;

static Window wins[MAX_WINDOWS];
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
static unsigned drag_gen;         /* generation of the window being dragged   */
static int    resize_win = -1;
static unsigned resize_gen;       /* generation of the window being resized   */
/* A compositor that dies silently leaves a frozen picture that looks like a
 * hang. Say which signal and where, so the serial console records it. */
static void on_fatal(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    char msg[160];
    int n = snprintf(msg, sizeof(msg),
                     "mystic: FATAL signal %d at %p - the desktop is gone\n",
                     sig, si ? si->si_addr : (void *)0);
    if (n > 0) { ssize_t w = write(2, msg, (size_t)n); (void)w; }
    _exit(1);
}

static void install_fatal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

static int    launcher_open = 0;
static int    wall_index = 0;
static double wall_t0 = 0.0;
static double wall_last = 0.0;
static int    launcher_sel = 0;

static WmClient clients[WM_MAX_CLIENTS];
static WmApp   bundles[WM_MAX_APPS];
static int     bundle_count = 0;

/* lock screen state */
static int    locked = 0;
static char   pin[64];
static int    pin_len = 0;
static char   lock_msg[96] = "";
static int    mouse_prev;

/* ------------------------------------------------------------ helpers */
static long mem_field(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char k[64], unit[16];
    long v;
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3)
        if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return 0;
}

static void clock_text(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* ------------------------------------------------------------ icons */
static void icon_terminal(Surface *s, float x, float y, float sz, u32 fg) {
    float pad = sz * 0.26f;
    gfx_line(s, x + pad, y + pad * 1.2f, x + pad + sz * 0.18f, y + sz * 0.5f, sz * 0.09f, fg);
    gfx_line(s, x + pad + sz * 0.18f, y + sz * 0.5f, x + pad, y + sz - pad * 1.2f, sz * 0.09f, fg);
    gfx_line(s, x + sz * 0.48f, y + sz - pad, x + sz - pad, y + sz - pad, sz * 0.09f, fg);
}

static void icon_folder(Surface *s, float x, float y, float sz, u32 fg) {
    float w = sz * 0.62f, h = sz * 0.44f;
    float bx = x + (sz - w) * 0.5f, by = y + (sz - h) * 0.5f + sz * 0.04f;
    gfx_rrect(s, bx - sz * 0.02f, by - sz * 0.14f, w * 0.42f, sz * 0.12f, sz * 0.02f, fg);
    gfx_rrect_aa(s, bx, by, w, h, sz * 0.06f, fg, 0, 0);
}

static void icon_gear(Surface *s, float x, float y, float sz, u32 fg) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 3.14159265f / 4.0f;
        gfx_line(s, cx + cosf(a) * sz * 0.24f, cy + sinf(a) * sz * 0.24f,
                    cx + cosf(a) * sz * 0.40f, cy + sinf(a) * sz * 0.40f, sz * 0.10f, fg);
    }
    gfx_rrect_aa(s, cx - sz * 0.19f, cy - sz * 0.19f, sz * 0.38f, sz * 0.38f, sz * 0.19f, 0, fg, sz * 0.08f);
}

static void icon_clock(Surface *s, float x, float y, float sz, u32 fg) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.36f;
    gfx_rrect_aa(s, cx - r, cy - r, r * 2, r * 2, r, 0, fg, sz * 0.07f);
    gfx_line(s, cx, cy, cx, cy - r * 0.62f, sz * 0.07f, fg);
    gfx_line(s, cx, cy, cx + r * 0.52f, cy + r * 0.18f, sz * 0.07f, fg);
}

static void icon_paint(Surface *s, float x, float y, float sz, u32 fg) {
    float px = x + sz * 0.30f, py = y + sz * 0.72f;
    gfx_line(s, px, py, x + sz * 0.74f, y + sz * 0.26f, sz * 0.13f, fg);
    gfx_rrect_aa(s, x + sz * 0.18f, y + sz * 0.62f, sz * 0.16f, sz * 0.16f, sz * 0.08f, fg, 0, 0);
}

static void icon_life(Surface *s, float x, float y, float sz, u32 fg) {
    float c = sz * 0.11f, g = sz * 0.17f;
    float ox = x + (sz - (2 * g + c)) / 2, oy = y + (sz - (2 * g + c)) / 2;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++)
            if ((r * 3 + col) % 4 != 1)
                gfx_rrect(s, ox + col * g, oy + r * g, c, c, c * 0.35f, fg);
}

static void icon_mandel(Surface *s, float x, float y, float sz, u32 fg) {
    float cx = x + sz * 0.42f, cy = y + sz * 0.5f;
    for (int i = 0; i < 3; i++) {
        float r = sz * (0.10f + i * 0.10f);
        gfx_rrect_aa(s, cx - r, cy - r, r * 2, r * 2, r, 0, fg, sz * 0.055f);
    }
    gfx_rrect_aa(s, cx + sz * 0.22f, cy - sz * 0.06f, sz * 0.12f, sz * 0.12f, sz * 0.06f, fg, 0, 0);
}

static void icon_sysmon(Surface *s, float x, float y, float sz, u32 fg) {
    gfx_line(s, x + sz * 0.18f, y + sz * 0.66f, x + sz * 0.36f, y + sz * 0.42f, sz * 0.07f, fg);
    gfx_line(s, x + sz * 0.36f, y + sz * 0.42f, x + sz * 0.54f, y + sz * 0.56f, sz * 0.07f, fg);
    gfx_line(s, x + sz * 0.54f, y + sz * 0.56f, x + sz * 0.82f, y + sz * 0.30f, sz * 0.07f, fg);
}

static void icon_calc(Surface *s, float x, float y, float sz, u32 fg) {
    gfx_rrect_aa(s, x + sz * 0.18f, y + sz * 0.10f, sz * 0.64f, sz * 0.80f, sz * 0.10f,
                 0, fg, sz * 0.055f);
    gfx_rrect(s, x + sz * 0.27f, y + sz * 0.19f, sz * 0.46f, sz * 0.16f, sz * 0.03f, fg);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            gfx_rrect(s, x + sz * (0.27f + c * 0.17f), y + sz * (0.44f + r * 0.15f),
                      sz * 0.11f, sz * 0.10f, sz * 0.02f, fg);
}

static void icon_grid(Surface *s, float x, float y, float sz, u32 fg) {
    float q = sz * 0.30f, gap = sz * 0.12f;
    float ox = x + (sz - 2 * q - gap) / 2, oy = y + (sz - 2 * q - gap) / 2;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 2; c++)
            gfx_rrect(s, ox + c * (q + gap), oy + r * (q + gap), q, q, q * 0.22f, fg);
}

static void icon_web(Surface *s, float x, float y, float sz, u32 fg) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.36f;
    gfx_rrect_aa(s, cx - r, cy - r, r * 2, r * 2, r, 0, fg, sz * 0.065f);
    gfx_line(s, cx - r, cy, cx + r, cy, sz * 0.055f, fg);          /* equator */
    gfx_line(s, cx, cy - r, cx, cy + r, sz * 0.055f, fg);          /* meridian */
    gfx_line(s, cx - r * 0.62f, cy - r * 0.72f, cx + r * 0.62f, cy - r * 0.72f, sz * 0.05f, fg);
    gfx_line(s, cx - r * 0.62f, cy + r * 0.72f, cx + r * 0.62f, cy + r * 0.72f, sz * 0.05f, fg);
}

static void icon_sysinfo(Surface *s, float x, float y, float sz, u32 fg) {
    /* an info "i" in a rounded panel: the System app is a read-out, not a gear */
    gfx_rrect_aa(s, x + sz * 0.18f, y + sz * 0.12f, sz * 0.64f, sz * 0.76f, sz * 0.10f,
                 0, fg, sz * 0.055f);
    float cx = x + sz * 0.5f;
    gfx_rrect(s, cx - sz * 0.035f, y + sz * 0.26f, sz * 0.07f, sz * 0.10f, sz * 0.02f, fg);
    gfx_rrect(s, cx - sz * 0.035f, y + sz * 0.42f, sz * 0.07f, sz * 0.24f, sz * 0.02f, fg);
}

static void icon_2048(Surface *s, float x, float y, float sz, u32 fg) {
    float q = sz * 0.27f;
    float ox = x + (sz - 2 * q - sz * 0.10f) / 2, oy = y + (sz - 2 * q - sz * 0.10f) / 2;
    gfx_rrect(s, ox, oy, q, q, sz * 0.06f, fg);
    gfx_rrect(s, ox + q + sz * 0.10f, oy, q, q, sz * 0.06f, fg);
    gfx_rrect(s, ox, oy + q + sz * 0.10f, q, q, sz * 0.06f, fg);
    gfx_rrect(s, ox + q + sz * 0.10f, oy + q + sz * 0.10f, q, q, sz * 0.06f, fg);
}

static void icon_bars(Surface *s, float x, float y, float sz, u32 fg) {
    for (int i = 0; i < 3; i++) {
        float hh = sz * (0.22f + i * 0.16f);
        gfx_rrect(s, x + sz * (0.24f + i * 0.20f), y + sz - sz * 0.22f - hh, sz * 0.10f, hh, sz * 0.03f, fg);
    }
}

static void icon_pencil(Surface *s, float x, float y, float sz, u32 fg) {
    float pts[8] = {
        x + sz * 0.26f, y + sz * 0.74f,
        x + sz * 0.70f, y + sz * 0.30f,
        x + sz * 0.60f, y + sz * 0.20f,
        x + sz * 0.16f, y + sz * 0.64f,
    };
    gfx_polygon(s, pts, 4, fg);
    gfx_line(s, x + sz * 0.16f, y + sz * 0.64f, x + sz * 0.20f, y + sz * 0.80f, sz * 0.05f, fg);
}

typedef void (*IconFn)(Surface *, float, float, float, u32);

/* One table decides an app's icon, wherever it is drawn. There used to be a
 * second table here for the dock, and it disagreed: web and g2048 fell through
 * to a gear, so the same app had two different icons. */
static IconFn icon_for_id(const char *id, int is_term) {
    if (is_term) return icon_terminal;
    IconFn fn = icon_for_bundle(id);
    return fn ? fn : icon_gear;
}

static IconFn icon_for_window(Window *w) {
    if (w->kind == WIN_TERM) return icon_terminal;
    if (w->kind == WIN_CLIENT && w->client >= 0 && w->client < WM_MAX_CLIENTS)
        return icon_for_id(clients[w->client].id, 0);
    if (w->app && w->app->vt && w->app->vt->id) return icon_for_id(w->app->vt->id, 0);
    return icon_gear;
}

/* ------------------------------------------------------------ window mgmt */
static Window *win_top(void) {
    Window *best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].hidden && (!best || wins[i].z > best->z))
            best = &wins[i];
    return best;
}

static void win_focus(Window *w) {
    if (!w) return;
    w->z = ++z_top;
}

/* One place that tells a window's owner its content size changed: apps get a
 * resize callback, external clients a RESIZE message, and terminals recompute
 * their pty grid (without this the shell kept wrapping at the old width). */
static void window_resized(Window *w) {
    if (!w || !w->used) return;
    int cw = w->w - 2;
    int chh = w->h - title_h() - 2;
    if (cw < 40) cw = 40;
    if (chh < 30) chh = 30;
    if (w->term) {
        term_resize_px(w->term, cw, chh);
    } else if (w->kind == WIN_CLIENT && w->client >= 0) {
        wms_notify_resize(&clients[w->client], cw, chh);
    } else if (w->app && w->app->vt && w->app->vt->resize) {
        w->app->vt->resize(w->app, cw, chh);
    }
}

/* F9: a drag stores a slot index, and slots are reused. win_reset bumps the
 * slot's generation every time it changes hands, so a drag or a resize that
 * outlives its window is dropped instead of moving whatever moved in. */
static void win_reset(Window *w) {
    unsigned g = w->gen;
    memset(w, 0, sizeof(*w));
    w->gen = g + 1;
}

/* Settings live in the RAM rootfs. Whenever one changes, mirror it onto the
 * ext4 disk so it survives a reboot; stage/init:restore_settings copies them
 * back at boot. Failure here is not fatal (a read-only disk still runs). */
static void persist_settings(void) {
    static const char *names[] = { "ui.conf", "wallpaper", "current-theme" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char src[128], dst[128], buf[512];
        snprintf(src, sizeof(src), "/etc/mystic/%s", names[i]);
        snprintf(dst, sizeof(dst), "/root/mystic-%s", names[i]);
        FILE *in = fopen(src, "rb");
        if (!in) continue;
        size_t n = fread(buf, 1, sizeof(buf), in);
        fclose(in);
        FILE *out = fopen(dst, "wb");
        if (!out) continue;
        if (fwrite(buf, 1, n, out) != n) { fclose(out); continue; }
        fflush(out);
        fsync(fileno(out));
        fclose(out);
    }
}

/* An in-desktop app cannot fork, so it asks the desktop to do privileged work
 * by writing a request file - the same pattern as launch.request. The package
 * manager is a shell script, so this is a fork+exec of /bin/sh on a command
 * built from a VALIDATED name: no way to smuggle in a second command. */
static int pkg_name_ok(const char *n) {
    if (!n[0] || strlen(n) > 32) return 0;
    for (const char *c = n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'))
            return 0;
    return 1;
}

static void run_pkg(const char *verb, const char *name) {
    if (!pkg_name_ok(name)) {
        fprintf(stderr, "pkg: refusing suspicious package name\n");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* sh -c CMD A B does not append A and B to CMD: they become $0 and
         * $1, so the old form ran a bare "pkg" and printed the package list
         * instead of installing anything. Build the whole command instead;
         * both words are already restricted to [a-z0-9-] by pkg_name_ok. */
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "pkg %s %s", verb, name);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    fprintf(stderr, "pkg: %s %s started as pid %d\n", verb, name, (int)pid);
}

/* read a one-word request file, remove it, return 1 if present */
static int take_request(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    out[0] = 0;
    if (fgets(out, (int)outsz, f)) {
        char *nl = strchr(out, 10);
        if (nl) *nl = 0;
    }
    fclose(f);
    remove(path);
    return out[0] ? 1 : 0;
}

static void win_close(Window *w) {
    if (!w || !w->used) return;
    if (w->term) {
        /* term_destroy closes the pty, reaps the shell and frees the grid and
         * the 600-line scrollback ring. Freeing the struct here instead leaked
         * 426 KB and one zombie per closed terminal. */
        term_destroy(w->term);
        w->term = NULL;
    }
    if (w->app) app_free(w->app);
    if (w->kind == WIN_CLIENT && w->client >= 0 && w->client < WM_MAX_CLIENTS) {
        wms_notify_close(&clients[w->client]);
        wms_shutdown_client(&clients[w->client]);
    }
    win_reset(w);
}

static Window *win_open(int kind, const char *app_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (w->used) continue;
        win_reset(w);
        w->used = 1;
        w->kind = kind;
        w->z = ++z_top;

        if (kind == WIN_TERM) {
            w->w = ui_px(640); w->h = ui_px(380);
            w->x = 60 + (i % 3) * 40;
            w->y = 70 + (i % 3) * 40;
            Font *fmc = font_get(FONT_MONO, 12);
            float cw = font_width(fmc, "M");
            float ch = font_height(fmc);
            if (cw < 4.0f) cw = 4.0f;
            if (ch < 8.0f) ch = 8.0f;
            int cols = (int)((w->w - 24) / cw);
            int rows = (int)((w->h - 56) / ch);
            w->term = term_create(cols, rows, "/bin/sh");
            snprintf(w->title, sizeof(w->title), "Terminal");
            if (!w->term) { win_reset(w); return NULL; }
        } else {
            w->w = ui_px(520); w->h = ui_px(360);
            w->x = 140 + (i % 3) * 40;
            w->y = 110 + (i % 3) * 40;
            w->app = app_new(app_id, w->w - 2, w->h - 40 - 2);
            if (!w->app) { win_reset(w); return NULL; }
            snprintf(w->title, sizeof(w->title), "%s", app_title(w->app));
        }
        if (w->x + w->w > disp.w) w->x = disp.w - w->w - 20;
        if (w->y + w->h > disp.h) w->y = disp.h - w->h - 90;
        return w;
    }
    return NULL;
}

/* ------------------------------------------------------------ dock */

static int taskbar_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i].used) n++;
    return n;
}

static Window *taskbar_at(int n) {
    int k = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && k++ == n) return &wins[i];
    return NULL;
}

/* Taskbar geometry. The bar is a list of open windows, not a launcher - the
 * eye in the top bar is the launcher. With nothing open there is no bar. */
static int dock_rect(int *dx, int *dy, int *dw, int *dh) {
    int n = taskbar_count();
    if (n <= 0) { *dx = *dy = *dw = *dh = 0; dock_pct = 100; return 0; }
    const int pad = dock_pad();
    dock_pct = 100;
    int isz = dock_isz(), gap = dock_gap();
    int w = n * isz + (n - 1) * gap + pad * 2;
    while (w > disp.w - 8 && dock_pct > 40) {      /* shrink until it fits */
        dock_pct -= 5;
        isz = dock_isz(); gap = dock_gap();
        w = n * isz + (n - 1) * gap + pad * 2;
    }
    int h = isz + pad * 2;
    *dw = w; *dh = h;
    *dx = (disp.w - w) / 2;
    if (*dx < 4) *dx = 4;
    *dy = disp.h - h - 22;
    return 1;
}

static void draw_launcher(Surface *s, const Theme *th);
static int  launcher_items(void);
static void draw_lock_screen(Surface *s, const Theme *th);

/* ------------------------------------------------------------ start button
 * The Mystic mark: an eye. Two circular arcs make the lens (vesica), the
 * iris is a real circle, and pressing it blinks - the lid closes in 90 ms
 * and opens in 170 ms, which is roughly how a human eye does it.
 *
 * Everything is drawn from geometry: no glyph, no image, crisp at any size.
 */
#define EYE_CX   19
#define EYE_CY   17
#define EYE_RX   12.0f
#define EYE_RY   7.6f
#define EYE_HIT_X 4
#define EYE_HIT_Y 3
#define EYE_HIT_W 30
#define EYE_HIT_H 28

static double blink_t0 = -1.0;      /* monotonic seconds, -1 = not blinking */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* coverage of a soft-edged disc at (0,0) radius r, for pixel (px,py) */
static float disc_cov(float px, float py, float r) {
    float d = sqrtf(px * px + py * py);
    float cov = r - d + 0.5f;
    if (cov < 0.0f) return 0.0f;
    if (cov > 1.0f) return 1.0f;
    return cov;
}

static void blend_px(Surface *s, int x, int y, u32 color, float a) {
    if (a <= 0.004f) return;
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    if (a > 1.0f) a = 1.0f;
    u32 *p = &s->px[(size_t)y * s->stride + x];
    u32 d = *p;
    float inv = 1.0f - a;
    *p = 0xFF000000u
       | ((u32)(GET_R(color) * a + GET_R(d) * inv) << 16)
       | ((u32)(GET_G(color) * a + GET_G(d) * inv) << 8)
       |  (u32)(GET_B(color) * a + GET_B(d) * inv);
}

static void draw_eye(Surface *s, const Theme *th, float open) {
    int hover = (input.mouse_x >= EYE_HIT_X && input.mouse_x < EYE_HIT_X + EYE_HIT_W &&
                 input.mouse_y >= EYE_HIT_Y && input.mouse_y < EYE_HIT_Y + EYE_HIT_H);
    int active = hover || launcher_open;

    u32 ring  = active ? th->accent : color_mix(th->bg_bottom, 0xFF000000u, 0.55f);
    u32 iris  = th->accent;
    u32 pupil = color_mix(th->accent, 0xFF000000u, 0.82f);
    u32 sclera = color_mix(th->text, 0xFFFFFFFFu, 0.80f);

    float ry = EYE_RY * open;
    if (ry < 0.6f) ry = 0.6f;

    /* vesica: radius and centre offset that give this half-width/half-height */
    float R = (EYE_RX * EYE_RX + ry * ry) / (2.0f * ry);
    float d = R - ry;
    float iris_r = EYE_RY * open * 0.86f;
    if (iris_r > EYE_RX * 0.62f) iris_r = EYE_RX * 0.62f;
    if (iris_r < 1.2f) iris_r = 1.2f;

    int x0 = EYE_CX - (int)EYE_RX - 2, x1 = EYE_CX + (int)EYE_RX + 3;
    int y0 = EYE_CY - (int)EYE_RY - 3,  y1 = EYE_CY + (int)EYE_RY + 4;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - EYE_CX;
            float py = (float)y + 0.5f - EYE_CY;

            /* inside both circles -> inside the lens */
            float d1 = sqrtf(px * px + (py - d) * (py - d));
            float d2 = sqrtf(px * px + (py + d) * (py + d));
            float lens = (R - d1 < R - d2 ? R - d1 : R - d2) + 0.5f;
            if (lens <= 0.0f) continue;
            if (lens > 1.0f) lens = 1.0f;

            /* the white of the eye */
            blend_px(s, x, y, sclera, lens * 0.97f);

            /* iris, then pupil, then a highlight - all clipped by the lid */
            float ic = disc_cov(px, py, iris_r);
            if (ic > 0.0f) blend_px(s, x, y, iris, ic * lens);

            float pc = disc_cov(px, py, iris_r * 0.58f);
            if (pc > 0.0f) blend_px(s, x, y, pupil, pc * lens);

            float hx = px + iris_r * 0.36f, hy = py + iris_r * 0.40f;
            float hc = disc_cov(hx, hy, iris_r * 0.19f);
            if (hc > 0.0f) blend_px(s, x, y, 0xFFFFFFFFu, hc * lens * 0.95f);

            /* lid outline: only where the lens coverage is partial, i.e. the
             * boundary. (Using 1-lens here paints the whole interior - that
             * mistake turned the eye into a black blob.) */
            if (lens < 0.58f)
                blend_px(s, x, y, ring, (0.58f - lens) * 2.0f);
        }
    }
}

static void draw_start_button(Surface *s, const Theme *th) {
    double t = now_sec();
    float open = 1.0f;

    if (blink_t0 >= 0.0) {
        double e = t - blink_t0;
        if (e < 0.09) {
            open = 1.0f - (float)(e / 0.09) * 0.94f;          /* close */
        } else if (e < 0.26) {
            open = 0.06f + (float)((e - 0.09) / 0.17) * 0.94f;  /* open */
        } else {
            blink_t0 = -1.0;
            open = 1.0f;
        }
    }
    draw_eye(s, th, open);
}

/* ------------------------------------------------------------ compose */

/* F10: font_draw paints whatever it is handed and has no clip rectangle, so a
 * long window title ran across the frame, over the clock and into the next
 * window. Fit the string into `avail` pixels by truncating on UTF-8 codepoint
 * boundaries and marking the cut with an ellipsis. The stored title is never
 * touched - the caller passes a scratch buffer. */
static void text_fit(Font *f, const char *src, char *dst, size_t dstsz, float avail) {
    if (!dst || dstsz < 4) return;
    dst[0] = '\0';
    if (!f || !src) return;
    if (font_width(f, src) <= avail) { snprintf(dst, dstsz, "%s", src); return; }

    const char *ell = "\xE2\x80\xA6";             /* U+2026 HORIZONTAL ELLIPSIS */
    float ew = font_width(f, ell);
    /* a face without U+2026 would draw nothing, leaving a silent cut */
    if (ew <= 0.0f) { ell = "..."; ew = font_width(f, ell); }
    size_t elen = strlen(ell);
    float room = avail - ew;
    /* the marker itself does not fit: draw nothing rather than overflow */
    if (room < 0.0f || dstsz <= elen + 1) return;

    size_t out = 0;
    float w = 0.0f;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        int len = 1;
        unsigned char c = *p;
        if ((c & 0xE0) == 0xC0 && p[1]) len = 2;
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) len = 3;
        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) len = 4;
        if (out + (size_t)len + elen >= dstsz) break;
        char one[5];
        memcpy(one, p, (size_t)len);
        one[len] = '\0';
        float adv = font_width(f, one);
        if (w + adv > room) break;
        memcpy(dst + out, p, (size_t)len);
        out += (size_t)len;
        w += adv;
        p += len;
    }
    memcpy(dst + out, ell, elen);
    out += elen;
    dst[out] = '\0';
}

static void draw_desktop(Surface *s, const Theme *th) {
    int W = s->w;
    (void)(s->h);

    /* the background is a wallpaper: a pure function of theme + index + time,
     * so the animated ones move and the static ones cost nothing */
    wallpaper_draw(s, th, wall_index, now_sec() - wall_t0);

    /* ---------------- top bar ---------------- */
    const int bar = topbar_h();
    if (th->blur > 0) gfx_blur_region(s, 0, 0, W, bar, (int)(18 * th->blur));
    surf_blend(s, 0, 0, W, bar, color_alpha(th->panel, 0.55f));
    surf_blend(s, 0, bar, W, 1, th->panel_border);

    Font *f13 = font_get(FONT_MEDIUM, 13);
    Font *f11 = font_get(FONT_REGULAR, 11);

    /* F12: the bar grows with the text scale, so no baseline may be a magic
     * number. Centre every item on the bar from its own font metrics. */
    const float y13 = ((float)bar - font_height(f13)) * 0.5f;
    const float y11 = ((float)bar - font_height(f11)) * 0.5f;

    draw_start_button(s, th);
    float brand_w = font_width(f13, "Mystic");
    font_draw(s, f13, 46, y13, "Mystic", th->text);

    char right[128];
    char clock[16];
    clock_text(clock, sizeof(clock));
    long mem = mem_field("MemTotal:");
    long avail = mem_field("MemAvailable:");
    long used = mem - avail;
    if (used < 0) used = 0;
    snprintf(right, sizeof(right), "%s      %ld MB used of %ld MB", clock, used / 1024, mem / 1024);
    float rw = font_width(f11, right);
    font_draw(s, f11, (float)W - rw - 16, y11, right, th->text_dim);

    Window *top = win_top();
    if (top) {
        /* F10: clip by truncation to the band that is actually free, between
         * the brand label and the clock block. A symmetric band keeps the
         * title on the screen centre; on a narrow screen at a large scale the
         * band is lopsided, so use what is left rather than draw nothing.
         * Before this a long title painted straight over both. */
        const float pad = 12.0f;
        float band_lo = 46.0f + brand_w + pad;
        float band_hi = (float)W - rw - 16.0f - pad;
        float mid = (float)W * 0.5f;
        float half = mid - band_lo;
        if (band_hi - mid < half) half = band_hi - mid;
        float centre = mid, wmax = half * 2.0f;
        if (half <= 0.0f) { centre = (band_lo + band_hi) * 0.5f; wmax = band_hi - band_lo; }
        if (wmax > 0.0f) {
            char shown[96];
            text_fit(f11, top->title, shown, sizeof(shown), wmax);
            float tw = font_width(f11, shown);
            font_draw(s, f11, centre - tw * 0.5f, y11, shown, th->text_dim);
        }
    }

    /* ---------------- windows, drawn back to front ---------------- */
    /* draw in ascending z order */
    int last_z = -1;
    for (int n = 0; n < MAX_WINDOWS; n++) {
        Window *cur = NULL;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].used || wins[i].hidden) continue;
            if (wins[i].z <= last_z) continue;
            if (!cur || wins[i].z < cur->z) cur = &wins[i];
        }
        if (!cur) break;
        last_z = cur->z;

        float cx = th->corner;
        gfx_shadow(s, (float)cur->x, (float)cur->y, (float)cur->w, (float)cur->h, cx, 24, 0.5f);
        /* No per-window backdrop blur: the window body is opaque (theme.c
         * forces the alpha off), so blurring what is underneath could never be
         * seen. It was 1.0-4.3 ms of dead work per window. */
        gfx_rrect_aa(s, (float)cur->x, (float)cur->y, (float)cur->w, (float)cur->h, cx,
                     th->window_bg, th->window_border, 1.0f);

        const int tb = title_h();
        surf_blend(s, cur->x + 1, cur->y + 1, cur->w - 2, tb, color_alpha(th->panel, 0.30f));
        surf_blend(s, cur->x + 1, cur->y + tb, cur->w - 2, 1, th->panel_border);

        /* traffic lights: red closes. F12: the dots sit on the same vertical
         * centre as the title and the click targets (traffic_y) instead of a
         * fixed y+15 that fell out of the bar as it grew. */
        const int dot_y = traffic_y(tb);
        gfx_rrect_aa(s, (float)cur->x + 14, (float)cur->y + dot_y, DOT_SZ, DOT_SZ, DOT_SZ * 0.5f,
                     0xFFFF5F57u, 0, 0);
        gfx_rrect_aa(s, (float)cur->x + 32, (float)cur->y + dot_y, DOT_SZ, DOT_SZ, DOT_SZ * 0.5f,
                     0xFFFEBC2Eu, 0, 0);
        gfx_rrect_aa(s, (float)cur->x + 50, (float)cur->y + dot_y, DOT_SZ, DOT_SZ, DOT_SZ * 0.5f,
                     0xFF28C840u, 0, 0);

        /* F10: a title wider than the bar was painted outside the window, over
         * the frame and the window behind it. Truncate to the bar.
         * F12: centre it on the bar from the real font height. */
        {
            char shown[96];
            text_fit(f13, cur->title, shown, sizeof(shown),
                     (float)cur->w - 72.0f - 12.0f);
            font_draw(s, f13, (float)cur->x + 72,
                      (float)cur->y + ((float)tb - font_height(f13)) * 0.5f,
                      shown, th->text);
        }

        /* resize grip */
        for (int g = 0; g < 3; g++) {
            float gx = (float)(cur->x + cur->w - 6 - g * 5);
            float gy = (float)(cur->y + cur->h - 6);
            gfx_line(s, gx - 4, gy, gx, gy - 4, 1.4f, color_alpha(th->text, 0.30f));
        }

        int cx0 = cur->x + 1, cy0 = cur->y + tb + 1;
        int cw0 = cur->w - 2, ch0 = cur->h - tb - 2;

        if (cur->kind == WIN_TERM && cur->term) {
            /* a terminal needs a monospace face: with a proportional font the
             * columns drift and 'ls' output stops lining up at larger scales */
            Font *fm = font_get(FONT_MONO, 12);
            float cw_cell = font_width(fm, "M");
            float ch_cell = font_height(fm);
            if (cw_cell < 4.0f) cw_cell = 4.0f;
            if (ch_cell < 8.0f) ch_cell = 8.0f;
            term_draw(cur->term, s, cx0, cy0, cw0, ch0, fm, cw_cell, ch_cell);
        } else if (cur->app) {
            /* an app draws into the whole surface, so give it the window as
             * its clip: a layout bug in one app must not paint over the
             * wallpaper or over the window underneath */
            surf_clip(s, cx0, cy0, cw0, ch0);
            app_draw(cur->app, s, cx0, cy0, cw0, ch0, th);
            surf_unclip(s);
        } else if (cur->kind == WIN_CLIENT && cur->client >= 0) {
            WmClient *cl = &clients[cur->client];
            /* An app that died - or never connected - used to leave an empty
             * rectangle, which looks like the whole desktop is broken. Say so
             * instead: the frame is drawn, the message explains it. */
            if (!cl->used || !cl->px) {
                Font *fm = font_get(FONT_MEDIUM, 13);
                const char *msg = "this app has stopped - open it again";
                float mw = font_width(fm, msg);
                font_draw(s, fm, (float)cx0 + ((float)cw0 - mw) * 0.5f,
                          (float)cy0 + (float)ch0 * 0.5f - font_height(fm),
                          msg, color_alpha(th->text, 0.65f));
            }
            if (cl->used && cl->px) {
                /* Clip to the client's buffer AND to the screen. Without the
                 * screen clamp a window dragged above the top edge (resize or
                 * a snap) writes past the end of the surface - a real crash. */
                int cw1 = cw0 < cl->w ? cw0 : cl->w;
                int ch1 = ch0 < cl->h ? ch0 : cl->h;
                /* Shift the *source* when the destination starts off-screen.
                 * Clamping only the right/bottom edges still wrote before the
                 * buffer for a window placed at a negative origin. */
                int sx0 = (cx0 < 0) ? -cx0 : 0;
                int sy0 = (cy0 < 0) ? -cy0 : 0;
                int dx = cx0 + sx0;
                int dy = cy0 + sy0;
                if (cw1 > cl->w - sx0) cw1 = cl->w - sx0;
                if (ch1 > cl->h - sy0) ch1 = cl->h - sy0;
                if (dx + cw1 > s->w) cw1 = s->w - dx;
                if (dy + ch1 > s->h) ch1 = s->h - dy;
                if (cw1 > 0 && ch1 > 0) {
                    for (int row = 0; row < ch1; row++)
                        memcpy(s->px + (size_t)(dy + row) * s->stride + dx,
                               cl->px + (size_t)(sy0 + row) * cl->stride + sx0,
                               (size_t)cw1 * 4);
                }
            }
        }
    }

    /* ---------------- taskbar: open windows, with dock magnification ---- */
    {
        int dx, dy, dw, dh;
        if (dock_rect(&dx, &dy, &dw, &dh)) {
            float dr = (float)dh * 0.32f;
            gfx_shadow(s, (float)dx, (float)dy, (float)dw, (float)dh, dr, 18, 0.5f);
            if (th->blur > 0) gfx_blur_region(s, dx, dy, dw, dh, (int)(20 * th->blur));
            gfx_rrect_aa(s, (float)dx, (float)dy, (float)dw, (float)dh, dr,
                         th->panel, th->panel_border, 1.0f);

            const int isz = dock_isz(), gap = dock_gap(), pad = dock_pad();
            int n = taskbar_count();
            Window *focused = win_top();
            int near_bar = (input.mouse_y > dy - 110 && input.mouse_y < dy + dh + 20);

            for (int i = 0; i < n; i++) {
                Window *w = taskbar_at(i);
                if (!w) continue;
                float bx = (float)(dx + pad + i * (isz + gap));
                float by = (float)(dy + pad);
                float cx0 = bx + isz * 0.5f;

                /* Dock magnification: icons grow near the pointer and sink
                 * back. Gaussian on the distance, so neighbours scale too. */
                float scale = 1.0f;
                if (near_bar) {
                    float d = (float)input.mouse_x - cx0;
                    float t = d / 62.0f;
                    scale = 1.0f + 0.45f * expf(-t * t * 0.5f);
                }
                float sz = isz * scale;
                float ix = cx0 - sz * 0.5f;
                float iy = by + (isz - sz) * 0.55f;

                int hovered = (input.mouse_x >= bx && input.mouse_x < bx + isz &&
                               input.mouse_y >= dy && input.mouse_y < dy + dh);
                int is_focus = (w == focused);
                u32 bg = is_focus ? color_alpha(th->accent, 0.28f)
                                  : (hovered ? color_alpha(th->accent, 0.18f)
                                             : color_alpha(th->text, 0.07f));
                gfx_rrect_aa(s, ix, iy, sz, sz, 11.0f * scale, bg, 0, 0);

                IconFn fn = icon_for_window(w);
                u32 fg = w->hidden ? color_alpha(th->text, 0.45f)
                                   : (is_focus || hovered ? th->accent : th->text);
                fn(s, ix, iy, sz, fg);

                if (is_focus)
                    gfx_rrect_aa(s, cx0 - 7, (float)(dy + dh) - 5, 14, 3, 1.5f, th->accent, 0, 0);

                if (hovered) {
                    Font *ft = font_get(FONT_MEDIUM, 11);
                    float lw = font_width(ft, w->title);
                    float tx = cx0 - lw / 2 - 7;
                    float ty = (float)dy - 26;
                    if (tx < 4) tx = 4;
                    if (tx + lw + 14 > (float)s->w) tx = (float)s->w - lw - 18;
                    gfx_rrect_aa(s, tx, ty, lw + 14, 19, 9,
                                 color_alpha(th->window_bg, 0.96f), th->panel_border, 1.0f);
                    font_draw(s, ft, tx + 7, ty + 4, w->title, th->text);
                }
            }
        }
    }

    if (launcher_open) {
        int total = launcher_items();
        if (launcher_sel >= total) launcher_sel = total > 0 ? total - 1 : 0;
        if (launcher_sel < 0) launcher_sel = 0;
        draw_launcher(s, th);
    }
    if (locked) draw_lock_screen(s, th);
}

/* ------------------------------------------------------------ launcher
 * Super opens a grid of every app that exists. Tiles are the same icons
 * as the dock, so there is one place that defines what an app looks like.
 */
typedef struct {
    const char *app_id;      /* NULL = terminal */
    const char *label;
    void (*icon)(Surface *, float, float, float, u32);
} LauncherItem;

static const LauncherItem LAUNCHER[] = {
    { NULL,       "Terminal", icon_terminal },
    { "files",    "Files",    icon_folder },
    { "settings", "Settings", icon_gear },
    { "monitor",  "Monitor",  icon_bars },
    { "notes",    "Notes",    icon_pencil },
    { "store",    "Apps",     icon_grid },
    { "calc",     "Calculator", icon_calc },
    { "sysinfo",  "System",   icon_gear },
};
#define LAUNCHER_N ((int)(sizeof(LAUNCHER) / sizeof(LAUNCHER[0])))

static int launcher_items(void) { return LAUNCHER_N + bundle_count; }

static void launcher_geom(int idx, int *tx, int *ty, int *tw, int *th) {
    const int cols = 3, tile = ui_px(128), gap = ui_px(18);
    int rows = (launcher_items() + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int gw = cols * tile + (cols - 1) * gap;
    /* Tiles shrink so the grid fits: with 14 apps at 1.6x a fixed 118px tile
     * put the first row's icons above the top of the screen. */
    int avail = disp.h - ui_px(96);
    int grid_h = (avail - (rows - 1) * gap) / rows;
    if (grid_h > ui_px(118)) grid_h = ui_px(118);
    if (grid_h < ui_px(56)) grid_h = ui_px(56);
    int gh = rows * grid_h + (rows - 1) * gap;
    int gx = (disp.w - gw) / 2;
    int gy = (disp.h - gh) / 2;
    if (gy < ui_px(44)) gy = ui_px(44);
    int r = idx / cols, c = idx % cols;
    *tx = gx + c * (tile + gap);
    *ty = gy + r * (grid_h + gap);
    *tw = tile;
    *th = grid_h;
}

static void draw_launcher(Surface *s, const Theme *th) {
    /* dim everything behind */
    surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u);

    const int cols = 3, tile = ui_px(128), gap = ui_px(18);
    int rows = (launcher_items() + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int gw = cols * tile + (cols - 1) * gap;
    int avail = s->h - ui_px(96);
    int grid_h = (avail - (rows - 1) * gap) / rows;
    if (grid_h > ui_px(118)) grid_h = ui_px(118);
    if (grid_h < ui_px(56)) grid_h = ui_px(56);
    int gh = rows * grid_h + (rows - 1) * gap;
    int px = (s->w - gw) / 2 - 24;
    int py = (s->h - gh) / 2 - 24;
    int pw = gw + 48, ph = gh + 48;

    gfx_shadow(s, (float)px, (float)py, (float)pw, (float)ph, 22, 30, 0.55f);
    if (th->blur > 0) gfx_blur_region(s, px, py, pw, ph, (int)(22 * th->blur));
    gfx_rrect_aa(s, (float)px, (float)py, (float)pw, (float)ph, 22,
                 color_alpha(th->window_bg, 0.97f), th->window_border, 1.0f);

    Font *f12 = font_get(FONT_MEDIUM, 12);
    Font *fd = font_get(FONT_REGULAR, 10);

    for (int i = 0; i < launcher_items(); i++) {
        int tx, ty, tw, th2;
        launcher_geom(i, &tx, &ty, &tw, &th2);
        int hovered = (input.mouse_x >= tx && input.mouse_x < tx + tw &&
                       input.mouse_y >= ty && input.mouse_y < ty + th2);
        int selected = launcher_open && (i == launcher_sel);
        u32 bg = (hovered || selected) ? color_alpha(th->accent, selected ? 0.30f : 0.22f)
                                       : color_alpha(th->text, 0.06f);
        gfx_rrect_aa(s, (float)tx, (float)ty, (float)tw, (float)th2, 14.0f, bg,
                     selected ? th->accent : 0, selected ? 1.5f : 0.0f);

        /* icon and label share the tile: the icon shrinks so the label always
         * fits underneath, which is what broke at 1.6x */
        Font *fl = font_get(FONT_MEDIUM, 12);
        float label_h = font_height(fl) + 4.0f;
        float isz = (float)grid_h - label_h - 12.0f;
        if (isz > (float)ui_px(52)) isz = (float)ui_px(52);
        if (isz < 12.0f) isz = 12.0f;
        const char *label;
        if (i < LAUNCHER_N) {
            LAUNCHER[i].icon(s, (float)tx + (tw - isz) / 2,
                             (float)ty + 6 + (grid_h - label_h - 12.0f - isz) / 2, isz,
                             hovered ? th->accent : th->text);
            label = LAUNCHER[i].label;
        } else {
            /* an installed bundle: a real program from /usr/share/mystic/apps */
            int b = i - LAUNCHER_N;
            label = (b >= 0 && b < bundle_count) ? bundles[b].name : "?";
            /* an installed app gets its own drawn icon; the initial is only
             * the fallback for a bundle this build does not know */
            u32 tile_bg = hovered ? color_alpha(th->accent, 0.34f)
                                  : color_alpha(th->text, 0.10f);
            gfx_rrect_aa(s, (float)tx + (tw - isz) / 2, (float)ty + 18, isz, isz, 12,
                         tile_bg, 0, 0);
            IconFn fn = (b >= 0 && b < bundle_count) ? icon_for_bundle(bundles[b].id) : NULL;
            float ix = (float)tx + (tw - isz) / 2, iy = (float)ty + 18;
            if (fn) {
                fn(s, ix + isz * 0.16f, iy + isz * 0.16f, isz * 0.68f,
                   hovered ? th->accent : th->text);
            } else {
                char initial[2] = { (char)(label && label[0] ? label[0] : '?'), 0 };
                Font *fi = font_get(FONT_SEMIBOLD, 26);
                float iw2 = font_width(fi, initial);
                font_draw(s, fi, (float)tx + (tw - iw2) / 2, (float)ty + 28, initial,
                          hovered ? th->accent : th->text);
            }
        }

        float lw = font_width(f12, label);
        font_draw(s, f12, (float)tx + (tw - lw) / 2,
                  (float)ty + grid_h - label_h - 2, label,
                  hovered ? th->text : th->text_dim);
    }

    float hw = font_width(fd, "Super or Esc closes");
    font_draw(s, fd, (float)(s->w - hw) / 2, (float)(py + ph + 10),
              "Super or Esc closes", color_alpha(th->text, 0.55f));
}

/* ------------------------------------------------------------ lock screen
 * Ctrl+Alt+L locks. The password is checked against /etc/mystic/passwd,
 * which holds SHA-256 iterated over salt||password, 20000 rounds.
 */
static void draw_lock_screen(Surface *s, const Theme *th) {
    if (th->blur > 0) gfx_blur_region(s, 0, 0, s->w, s->h, 40);
    surf_blend(s, 0, 0, s->w, s->h, 0xB0000000u);

    int pw = ui_px(380), ph = ui_px(210);
    int px = (s->w - pw) / 2, py = (s->h - ph) / 2;

    gfx_shadow(s, (float)px, (float)py, (float)pw, (float)ph, 20, 26, 0.6f);
    gfx_rrect_aa(s, (float)px, (float)py, (float)pw, (float)ph, 20,
                 th->window_bg, th->window_border, 1.0f);

    Font *fh = font_get(FONT_MEDIUM, 16);
    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);

    char clock[16] = {0};
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);

    float hw = font_width(fh, clock);
    font_draw(s, fh, (float)px + (pw - hw) / 2, (float)py + 26, clock, th->text);

    float lw = font_width(fr, "Mystic OS is locked");
    font_draw(s, fr, (float)px + (pw - lw) / 2, (float)py + 58, "Mystic OS is locked",
              th->text_dim);

    /* password field, one dot per character */
    int fw = pw - 80, fh2 = 38;
    int fx = px + 40, fy = py + 92;
    gfx_rrect_aa(s, (float)fx, (float)fy, (float)fw, (float)fh2, 9,
                 color_alpha(th->text, 0.08f), th->window_border, 1.0f);
    float dx = (float)fx + 14;
    for (int i = 0; i < pin_len && i < 24; i++) {
        gfx_rrect_aa(s, dx, (float)fy + 15, 8, 8, 4, th->text, 0, 0);
        dx += 14;
    }

    if (lock_msg[0]) {
        float mw = font_width(fd, lock_msg);
        font_draw(s, fd, (float)px + (pw - mw) / 2, (float)py + 142, lock_msg, th->accent);
    }
    font_draw(s, fd, (float)px + 40, (float)py + 170, "Enter to unlock", color_alpha(th->text, 0.45f));
}

static void try_unlock(void) {
    char line[256] = {0};
    FILE *f = fopen("/etc/mystic/passwd", "r");
    if (f) {
        if (!fgets(line, sizeof(line), f)) line[0] = 0;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        fclose(f);
    }
    pin[pin_len < (int)sizeof(pin) ? pin_len : (int)sizeof(pin) - 1] = 0;
    if (!line[0]) {
        /* fail open only when there is no password file at all: deleting it
         * used to make the lock screen unrecoverable without a reboot */
        if (pin_len > 0) { locked = 0; pin_len = 0; pin[0] = 0; }
        else snprintf(lock_msg, sizeof(lock_msg), "no password file - any input unlocks");
        return;
    }
    if (mystic_check_password(line, pin)) {
        locked = 0;
        snprintf(lock_msg, sizeof(lock_msg), "%s", "");
    } else {
        snprintf(lock_msg, sizeof(lock_msg), "wrong password");
    }
    pin_len = 0;
    pin[0] = 0;
}

/* ------------------------------------------------------------ cursor */
/* Copy one rectangle between surfaces. The cursor is drawn straight onto the
 * framebuffer copy, so moving it means putting back what was underneath -
 * which we have: `scene` is the composited desktop without the cursor. */
static void copy_rect(Surface *dst, const Surface *src,
                      int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst->w) w = dst->w - x;
    if (y + h > dst->h) h = dst->h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        memcpy(dst->px + (size_t)row * dst->stride + x,
               src->px + (size_t)row * src->stride + x,
               (size_t)w * sizeof(u32));
}

static void draw_cursor_at(Surface *s, int x, int y) {
    static const float shape[] = {
        0, 0,   0, 17,   4.2f, 12.6f,   7.4f, 19.6f,   10.6f, 18.2f,
        7.4f, 11.2f,   12.4f, 11.2f
    };
    float pts[14];
    for (int i = 0; i < 14; i += 2) {
        pts[i]     = shape[i]     + (float)x;
        pts[i + 1] = shape[i + 1] + (float)y;
    }
    float outline[14];
    for (int i = 0; i < 14; i++) outline[i] = pts[i] + 1.2f;
    gfx_polygon(s, outline, 7, RGB(0, 0, 0));
    gfx_polygon(s, pts, 7, RGB(255, 255, 255));
}

/* ------------------------------------------------------------ input routing */
static void key_bytes_for(int code, char *out, int *n) {
    *n = 0;
    switch (code) {
        case 103: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'A'; return;   /* up */
        case 108: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'B'; return;   /* down */
        case 106: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'C'; return;   /* right */
        case 105: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'D'; return;   /* left */
        case 102: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'H'; return;   /* home */
        case 107: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = 'F'; return;   /* end */
        case 104: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = '5'; out[(*n)++] = '~'; return;
        case 109: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = '6'; out[(*n)++] = '~'; return;
        case 111: out[(*n)++] = 0x1B; out[(*n)++] = '['; out[(*n)++] = '3'; out[(*n)++] = '~'; return;
        default: return;
    }
}

static int taskbar_click(int mx, int my) {
    int dx, dy, dw, dh;
    if (!dock_rect(&dx, &dy, &dw, &dh)) return 0;
    if (my < dy || my >= dy + dh) return 0;
    const int isz = dock_isz(), gap = dock_gap(), pad = dock_pad();
    int n = taskbar_count();
    for (int i = 0; i < n; i++) {
        int bx = dx + pad + i * (isz + gap);
        if (mx < bx || mx >= bx + isz) continue;
        Window *w = taskbar_at(i);
        if (!w) continue;
        if (w->hidden) { w->hidden = 0; win_focus(w); }   /* restore */
        else if (w == win_top()) { w->hidden = 1; }       /* put away */
        else { win_focus(w); }                            /* focus */
        return 1;
    }
    return 0;
}

/* Maximise/restore in one place: the yellow dot and the title-bar
 * double-click must do exactly the same thing, or one of them drifts. */
static void win_toggle_max(Window *w) {
    if (!w->maxed) {
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
        int top_ = topbar_h();
        int bdx, bdy, bdw, bdh;
        int reserved = dock_rect(&bdx, &bdy, &bdw, &bdh)
                     ? (disp.h - bdy + ui_px(12)) : ui_px(16);
        w->x = 0; w->y = top_;
        w->w = disp.w; w->h = disp.h - top_ - reserved;
        w->maxed = 1;
    } else {
        w->x = w->restore_x; w->y = w->restore_y;
        w->w = w->restore_w; w->h = w->restore_h;
        w->maxed = 0;
    }
    window_resized(w);
}

        static int handle_click(void) {
    int mx = input.mouse_x, my = input.mouse_y;

    /* The taskbar is drawn above the windows, so it must be hit first:
     * otherwise a window dragged over the bar swallows its clicks. */
    if (taskbar_click(mx, my)) return 1;

    /* the start button is above everything else in the top bar */
    if (mx >= EYE_HIT_X && mx < EYE_HIT_X + EYE_HIT_W &&
        my >= EYE_HIT_Y && my < EYE_HIT_Y + EYE_HIT_H) {
        blink_t0 = now_sec();          /* it blinks on every press */
        launcher_open = !launcher_open;
        return 1;
    }

    if (launcher_open) {
        for (int i = 0; i < launcher_items(); i++) {
            int tx, ty, tw, th2;
            launcher_geom(i, &tx, &ty, &tw, &th2);
            if (mx < tx || mx >= tx + tw || my < ty || my >= ty + th2) continue;
            if (i < LAUNCHER_N) {
                if (LAUNCHER[i].app_id) win_open(WIN_APP, LAUNCHER[i].app_id);
                else                    win_open(WIN_TERM, NULL);
            } else {
                int b = i - LAUNCHER_N;
                if (b >= 0 && b < bundle_count) {
                    int pid = wms_launch(&bundles[b], clients, b);
                    fprintf(stderr, "wms: launched %s pid %d", bundles[b].id, pid);
                    fprintf(stderr, "\n");
                }
            }
            launcher_open = 0;
            return 1;
        }
        launcher_open = 0;                  /* click outside closes */
        return 1;
    }

    /* topmost window under the pointer */
    Window *hit = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (!w->used || w->hidden) continue;
        if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h) continue;
        if (!hit || w->z > hit->z) hit = w;
    }
    if (hit) {
        win_focus(hit);
        int tb = title_h();
        int lx = mx - hit->x, ly = my - hit->y;
        /* F12: red closes, yellow minimises - only on the dots themselves.
         * The band is derived from the same centre the dots are drawn on, so
         * at scale 1.6 the target still sits on the dot instead of the fixed
         * 13..27 band the dots had already left. */
int dot_y = traffic_y(tb);
        /* red closes, yellow maximises, green minimises - the order they are
         * painted in. The green dot had no handler at all and yellow
         * minimised, so "maximise" and "minimise" were both wrong. */
        if (ly >= dot_y - 2 && ly < dot_y + DOT_SZ + 2 && lx >= 14 && lx < 24) {
            win_close(hit);
            return 1;
        }
        if (ly >= dot_y - 2 && ly < dot_y + DOT_SZ + 2 && lx >= 32 && lx < 42) {
            win_toggle_max(hit);
            return 1;
        }
        if (ly >= dot_y - 2 && ly < dot_y + DOT_SZ + 2 && lx >= 50 && lx < 60) {
            hit->hidden = 1;
            return 1;
        }
        /* bottom-right corner resizes */
        if (lx >= hit->w - 18 && ly >= hit->h - 18) {
            resize_win = (int)(hit - wins);
            resize_gen = hit->gen;          /* F9: remember which window, not just the slot */
            return 1;
        }
        /* double-click on the title bar toggles maximise */
        if (ly < tb && input.clicked2) {
            win_toggle_max(hit);
            return 1;
        }

        /* title bar = drag, but only with the left button: a right click on
         * the bar is a context menu request, not a drag */
        if (ly < tb && (input.buttons & 1)) {
            drag_win = (int)(hit - wins);
            drag_gen = hit->gen;            /* F9: remember which window, not just the slot */
            drag_dx = lx;
            drag_dy = ly;
            return 1;
        }
        /* body */
        if (hit->app) {
            app_click(hit->app, lx - 1, ly - tb - 1, input.buttons);
        } else if (hit->kind == WIN_CLIENT && hit->client >= 0) {
            WmClient *cl = &clients[hit->client];
            if (cl->used) wms_notify_mouse(cl, lx - 1, ly - tb - 1, input.buttons);
        }
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------ main */
int main(void) {
    install_fatal_handlers();
    wms_set_screen(disp.w, disp.h);   /* client buffers are screen sized */
    if (disp_open(&disp, "/dev/fb0") < 0) return 1;
    /* the text scale must be known before the first font is requested */
    ui_scale_load("/etc/mystic/ui.conf");
    {
        FILE *wf = fopen("/etc/mystic/wallpaper", "r");
        if (wf) {
            char name[64] = {0};
            if (fgets(name, sizeof(name), wf)) {
                char *nl = strchr(name, 10);
                if (nl) *nl = 0;
                int idx = wallpaper_from_name(name);
                if (idx >= 0) wall_index = idx;
            }
            fclose(wf);
        }
        wall_t0 = now_sec();
    }
    font_init(FONT_DIR);
    fprintf(stderr, "ui: scale %.2f", ui_scale_get());
    fputc(10, stderr);

    screen.w = disp.w; screen.h = disp.h; screen.stride = disp.w;
    screen.px = calloc((size_t)screen.w * screen.h, 4);
    scene = screen;
    scene.px = calloc((size_t)screen.w * screen.h, 4);
    if (!screen.px || !scene.px) { fprintf(stderr, "out of memory\n"); return 1; }

    theme_count = theme_load_all(themes, 16, THEME_DIR);
    if (theme_count == 0) {
        Theme t;
        theme_load(&t, "/nonexistent");
        themes[0] = t;
        theme_count = 1;
    }

    if (input_open(&input) < 0) fprintf(stderr, "no input devices\n");

    if (wms_start() < 0) fprintf(stderr, "window server failed to start\n");
    bundle_count = wms_scan_apps(bundles, WM_MAX_APPS);
    fprintf(stderr, "wms: %d installed app bundle(s)\n", bundle_count);

    int quit = 0, dirty = 1;
    int cur_x = -1, cur_y = -1;      /* where the cursor is painted right now */
    long last_tick = 0;

    while (!quit) {
        input_frame(&input);
        input_poll(&input);

        if (input.mouse_x < 0) input.mouse_x = 0;
        if (input.mouse_y < 0) input.mouse_y = 0;
        if (input.mouse_x >= disp.w) input.mouse_x = disp.w - 1;
        if (input.mouse_y >= disp.h) input.mouse_y = disp.h - 1;

        if (input.ctrl && input.alt && input.key == 38) {   /* Ctrl+Alt+L */
            locked = 1;
            pin_len = 0;
            pin[0] = 0;
            lock_msg[0] = 0;
            dirty = 1;
            /* the 'l' of the shortcut itself arrives as a control character in
             * this same frame - drop it or it becomes the first password byte */
            input.text_len = 0;
            input.key = 0;
        }

        if (locked) {
            for (int i = 0; i < input.text_len; i++) {
                unsigned char c = (unsigned char)input.text[i];
                if (c < 32) continue;      /* Enter/Backspace arrive here too - ignore */
                if (pin_len < (int)sizeof(pin) - 1) pin[pin_len++] = (char)c;
            }
            if (input.key == 14 && pin_len > 0) pin[--pin_len] = 0;   /* Backspace */
            if (input.key == 28) try_unlock();                        /* Enter */
            if (!locked) {
                /* The Enter that unlocked must not reach whatever window is
                 * focused: previously it fell through and executed an empty
                 * command in the terminal / inserted a newline in Notes. */
                input.text_len = 0;
                input.key = 0;
                input.nkeys = 0;
            }

            /* Redraw only when something changed. The lock screen is a
             * full-screen blur (~7 ms measured), and it used to run at 60 Hz
             * for a clock that changes once a minute. */
            static long last_min = -1;
            long minute = (long)time(NULL) / 60;
            if (minute != last_min) { last_min = minute; dirty = 1; }
            if (input.text_len || input.key) dirty = 1;

            if (locked) {
                if (dirty) {
                    draw_desktop(&scene, &themes[theme_index]);
                    surf_copy(&screen, &scene);
                    draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
                    disp_present(&disp, &screen);
                    dirty = 0;
                }
                /* the lock screen must not stall the world behind it:
                 * clients would block on their sockets and exited children
                 * would stay zombies for the whole lock period */
                wms_poll(clients, WM_MAX_CLIENTS);
                while (waitpid(-1, NULL, WNOHANG) > 0) { }
                for (int wi = 0; wi < MAX_WINDOWS; wi++)
                    if (wins[wi].used && wins[wi].term) term_pump(wins[wi].term);

                struct pollfd lp = { 0, 0, 0 };
                poll(&lp, 1, 16);
                continue;
            }
        }

        /* Theme switching is Ctrl+1..9. Plain digits must reach the focused
         * app: with a calculator open, typing 2342 used to change the theme
         * instead of entering a number. */
        if (input.ctrl) {
            for (int i = 0; i < input.text_len; i++) {
                char c = input.text[i];
                if (c >= '1' && c <= '9') {
                    int idx = c - '1';
                    if (idx < theme_count) { theme_index = idx; dirty = 1; }
                    input.text[i] = 0;      /* consumed */
                }
            }
        }

        /* Every key pressed this frame, in order. Handling only the last one
         * collapsed fast bursts: five arrow presses registered as one. */
        for (int ki = 0; ki < input.nkeys; ki++) {
            int k = input.keys[ki];

            if (input.ctrl && input.alt && k == 38) {        /* Ctrl+Alt+L */
                locked = 1;
                pin_len = 0;
                pin[0] = 0;
                lock_msg[0] = 0;
                input.text_len = 0;      /* the 'l' arrives as a control char */
                dirty = 1;
                continue;
            }

            if (k == 125) {                  /* Super toggles the launcher */
                launcher_open = !launcher_open;
                dirty = 1;
                continue;
            }

            if (launcher_open) {
                if (k == 1) { launcher_open = 0; dirty = 1; continue; }
                if (k == 105 || k == 106 || k == 103 || k == 108) {
                    int cols = 3, total = launcher_items(), step = 0;
                    if (k == 105) step = -1;
                    if (k == 106) step = 1;
                    if (k == 103) step = -cols;
                    if (k == 108) step = cols;
                    int n2 = launcher_sel + step;
                    if (n2 >= 0 && n2 < total) launcher_sel = n2;
                    else if (step == -1 && launcher_sel == 0) launcher_sel = total - 1;
                    else if (step == 1 && launcher_sel == total - 1) launcher_sel = 0;
                    dirty = 1;
                    continue;
                }
                if (k == 28) {               /* Enter launches the selection */
                    /* Enter arrives twice in one frame: as a key here and as
                     * the '
' character in the text queue. The launcher
                     * consumes the key, so the text must go too - otherwise
                     * the app it just opened receives a newline and activates
                     * its own selected row (opening the launcher and starting
                     * an app with one press). */
                    input.text_len = 0;
                    input.text[0] = 0;
                    int idx = launcher_sel;
                    if (idx < LAUNCHER_N) {
                        if (LAUNCHER[idx].app_id) win_open(WIN_APP, LAUNCHER[idx].app_id);
                        else                      win_open(WIN_TERM, NULL);
                    } else {
                        int b = idx - LAUNCHER_N;
                        if (b >= 0 && b < bundle_count) {
                            int pid = wms_launch(&bundles[b], clients, b);
                            fprintf(stderr, "wms: launch %s pid %d", bundles[b].id, pid);
                            fputc(10, stderr);
                        }
                    }
                    launcher_open = 0;
                    dirty = 1;
                    continue;
                }
            }

            if (input.alt && k == 15 && z_top > 0) {           /* Alt+Tab */
                Window *cur = win_top(), *next = NULL;
                if (cur) {
                    int best = -1;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        if (!wins[i].used || wins[i].hidden || &wins[i] == cur) continue;
                        if (wins[i].z > best) { best = wins[i].z; next = &wins[i]; }
                    }
                }
                if (next) { win_focus(next); dirty = 1; }
                continue;
            }

            if (launcher_open) {
                /* modal overlay: nothing may leak to the window behind it */
                dirty = 1;
                continue;
            }

            switch (k) {                     /* F1..F5 launch */
                case 59: win_open(WIN_TERM, NULL); dirty = 1; continue;
                case 60: win_open(WIN_APP, "files"); dirty = 1; continue;
                case 61: win_open(WIN_APP, "settings"); dirty = 1; continue;
                case 62: win_open(WIN_APP, "monitor"); dirty = 1; continue;
                case 63: win_open(WIN_APP, "notes"); dirty = 1; continue;
                default: break;
            }

            if (k == 1) {                    /* Esc closes the top window */
                Window *t = win_top();
                if (t) { win_close(t); dirty = 1; }
                continue;
            }

            /* the focused window gets the key */
            Window *top = win_top();
            if (!top) continue;
            if (top->term) {
                char buf[64];
                int n = 0;
                if (input.shift && (k == 104 || k == 109)) {
                    term_scroll(top->term, k == 104 ? 8 : -8);
                } else {
                    key_bytes_for(k, buf, &n);
                    if (n) term_key(top->term, buf, n);
                }
            } else if (top->kind == WIN_CLIENT && top->client >= 0) {
                WmClient *cl = &clients[top->client];
                int mods = (input.shift ? 1 : 0) | (input.ctrl ? 2 : 0) | (input.alt ? 4 : 0);
                wms_notify_key(cl, k, mods, 1);
                dirty = 1;
            } else if (top->app) {
                char kb[64];
                int kn = 0;
                key_bytes_for(k, kb, &kn);
                if (kn) { app_key(top->app, kb, kn); dirty = 1; }
            }
        }

        /* releases: the protocol carries a press flag that was always 1, so a
         * client could never tell when a key went back up */
        {
            Window *top = win_top();
            if (top && top->kind == WIN_CLIENT && top->client >= 0 && input.nkeys_up) {
                WmClient *cl = &clients[top->client];
                int mods = (input.shift ? 1 : 0) | (input.ctrl ? 2 : 0) | (input.alt ? 4 : 0);
                for (int i = 0; i < input.nkeys_up; i++)
                    wms_notify_key(cl, input.keys_up[i], mods, 0);
            }
        }

        /* text is delivered once per frame, not per key */
        {
            Window *top = launcher_open ? NULL : win_top();
            if (top && input.text_len) {
                if (top->term) {
                    term_scroll(top->term, -(term_scroll_offset(top->term)));
                    term_key(top->term, input.text, input.text_len);
                } else if (top->kind == WIN_CLIENT && top->client >= 0) {
                    wms_notify_text(&clients[top->client], input.text);
                    dirty = 1;
                } else if (top->app) {
                    app_key(top->app, input.text, input.text_len);
                    dirty = 1;
                }
            }
        }

        /* mouse */
        if (input.wheel) {
            /* The wheel goes to whatever the pointer is over - any window,
             * not just terminals, and not only the focused one. */
            Window *under = NULL;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                Window *w = &wins[i];
                if (!w->used || w->hidden) continue;
                if (input.mouse_x < w->x || input.mouse_x >= w->x + w->w) continue;
                if (input.mouse_y < w->y || input.mouse_y >= w->y + w->h) continue;
                if (!under || w->z > under->z) under = w;
            }
            if (under) {
                if (under->term) {
                    term_scroll(under->term, input.wheel * 3);
                    dirty = 1;
                } else if (under->kind == WIN_CLIENT && under->client >= 0 &&
                           clients[under->client].used) {
                    wms_notify_wheel(&clients[under->client], input.wheel);
                    dirty = 1;
                } else if (under->app) {
                    app_wheel(under->app, input.wheel);
                    dirty = 1;
                }
            }
        }
        /* bit 1 is the left button, bit 2 the right (src/input.c maps
         * BTN_RIGHT to it). Only the left button used to be dispatched, so a
         * right click did nothing anywhere in the system. */
        if (input.clicked & 3) { if (handle_click()) dirty = 1; }
        if (input.released & 1) {
            if (drag_win >= 0 && wins[drag_win].used && wins[drag_win].gen == drag_gen) {
                Window *w = &wins[drag_win];
                const int bar = topbar_h();
                if (w->y <= bar + 6) {                       /* top edge: full width */
                    w->x = 0; w->y = bar; w->w = disp.w; w->h = disp.h - bar - 90;
                } else if (w->x <= 8) {                      /* left edge: left half */
                    w->x = 0; w->y = bar; w->w = disp.w / 2; w->h = disp.h - bar - 90;
                } else if (w->x + w->w >= disp.w - 8) {      /* right edge: right half */
                    w->w = disp.w / 2; w->x = disp.w - w->w;
                    w->y = bar; w->h = disp.h - bar - 90;
                }
                window_resized(w);
                dirty = 1;
            }
            drag_win = -1;
            resize_win = -1;
        }
        if ((input.buttons & 1) && resize_win >= 0) {
            Window *w = &wins[resize_win];
            /* F9: the window closed and the slot was reused - not our window */
            if (!w->used || w->gen != resize_gen) { resize_win = -1; }
            else {
                int nw = input.mouse_x - w->x + 1;
                int nh = input.mouse_y - w->y + 1;
                /* Minimums that leave a client app room to draw. The client
                 * area is h - title bar - 2, and the browser needs about 124
                 * px at the default scale (196 at the largest) before it has a
                 * header, a page and a status bar. At 160 px tall the content
                 * was 118, so a window squeezed to the old minimum rendered
                 * nothing at all. */
                if (nw < 320) nw = 320;
                if (nh < ui_px(200) + title_h()) nh = ui_px(200) + title_h();
                if (nw != w->w || nh != w->h) {
                    w->w = nw;
                    w->h = nh;
                    window_resized(w);
                    dirty = 1;
                }
            }
        }
        if ((input.buttons & 1) && drag_win >= 0) {
            Window *w = &wins[drag_win];
            /* F9: the window closed and the slot was reused - not our window */
            if (!w->used || w->gen != drag_gen) { drag_win = -1; }
            else {
                int nx = input.mouse_x - drag_dx, ny = input.mouse_y - drag_dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 34;
                if (nx + w->w > disp.w) nx = disp.w - w->w;
                if (ny + w->h > disp.h) ny = disp.h - w->h;
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; dirty = 1; }
            }
        }
        (void)mouse_prev;

        /* launched apps exit without anyone noticing otherwise: one zombie
         * per launch for the whole session */
        while (waitpid(-1, NULL, WNOHANG) > 0) { }

        /* window server: accept connections, bind windows, mark frames dirty */
        wms_poll(clients, WM_MAX_CLIENTS);

        /* A client that exited leaves its slot free; the window that was bound
         * to that slot must close with it, or the next client to take the slot
         * renders into the ghost and closing the ghost kills the newcomer. */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window *w = &wins[i];
            if (!w->used || w->kind != WIN_CLIENT) continue;
            if (w->client < 0 || w->client >= WM_MAX_CLIENTS) continue;
            if (!clients[w->client].used) {
                w->client = -1;          /* stop it from killing a reused slot */
                win_close(w);
                dirty = 1;
            }
        }
        for (int i = 0; i < WM_MAX_CLIENTS; i++) {
            WmClient *cl = &clients[i];
            if (!cl->used) continue;
            if (cl->win < 0 && cl->px) {
                Window *w = NULL;
                for (int k = 0; k < MAX_WINDOWS; k++) if (!wins[k].used) { w = &wins[k]; break; }
                if (w) {
                    win_reset(w);
                    w->used = 1;
                    w->kind = WIN_CLIENT;
                    w->client = i;
                    w->w = cl->w + 2;
                    w->h = cl->h + title_h() + 2;
                    w->x = 120 + (i % 4) * 36;
                    w->y = 90 + (i % 4) * 36;
                    if (w->x + w->w > disp.w) w->x = disp.w - w->w - 8;
                    if (w->y + w->h > disp.h) w->y = disp.h - w->h - 100;
                    snprintf(w->title, sizeof(w->title), "%s", cl->title);
                    w->z = ++z_top;
                    cl->win = (int)(w - wins);
                    dirty = 1;
                }
            } else if (cl->win >= 0) {
                Window *w = &wins[cl->win];
                if (w->used) {
                    snprintf(w->title, sizeof(w->title), "%s", cl->title);
                    if (cl->want_frame) { cl->want_frame = 0; dirty = 1; }
                }
            }
        }

        /* terminals produce output asynchronously */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (wins[i].used && wins[i].term) {
                /* Only repaint when the shell actually produced output. Setting
                 * dirty unconditionally meant an open terminal forced a full
                 * 1280x800 recompose (blur included) every single frame. */
                if (term_pump(wins[i].term) > 0) dirty = 1;
                if (!term_alive(wins[i].term)) { win_close(&wins[i]); }
            }
        }

        /* an external app needs motion, not just the press: without this a
         * drag draws one dot (paint) and sliders never move */
        {
            Window *ct = win_top();
            if (ct && ct->kind == WIN_CLIENT && ct->client >= 0) {
                WmClient *cl = &clients[ct->client];
                int inside = (input.mouse_x >= ct->x && input.mouse_x < ct->x + ct->w &&
                              input.mouse_y >= ct->y && input.mouse_y < ct->y + ct->h);
                if (cl->used && inside &&
                    (input.buttons || (input.mouse_dx || input.mouse_dy) || input.released)) {
                    wms_notify_mouse(cl, input.mouse_x - ct->x - 1,
                                     input.mouse_y - ct->y - title_h() - 1, input.buttons);
                    dirty = 1;
                }
            }
        }

        /* keep repainting while the eye is blinking */
        if (blink_t0 >= 0.0) dirty = 1;

        /* animated wallpapers need repaints; a static one must never force
         * them or the CPU would never idle */
        if (wallpaper_animated(wall_index)) {
            double nowt = now_sec();
            if (nowt - wall_last > 0.08) { wall_last = nowt; dirty = 1; }
        }

        long now = (long)time(NULL);
        if (now != last_tick) {
            last_tick = now;
            dirty = 1;
            /* Settings can change these at runtime: re-read both files once a
             * second (a whole-file read of two small files is cheap). */
            {
                float before = ui_scale_get();
                ui_scale_load("/etc/mystic/ui.conf");
                if (ui_scale_get() != before) {
                    fprintf(stderr, "ui: scale %.2f -> %.2f", before, ui_scale_get());
                    fputc(10, stderr);
                    dirty = 1;
                    persist_settings();
                }

                FILE *wf = fopen("/etc/mystic/wallpaper", "r");
                if (wf) {
                    char wn[64] = {0};
                    if (fgets(wn, sizeof(wn), wf)) {
                        char *nl = strchr(wn, 10);
                        if (nl) *nl = 0;
                        int idx = wallpaper_from_name(wn);
                        if (idx >= 0 && idx != wall_index) { wall_index = idx; dirty = 1; persist_settings(); }
                    }
                    fclose(wf);
                }
            }

            /* Store -> desktop: install a package, or refresh the repo index */
            {
                char want[64];
                if (take_request("/run/mystic/install.request", want, sizeof(want)))
                    run_pkg("install", want);
                /* first run: the Store needs an index to show anything, and
                 * asking the user to press Refresh before they have ever seen
                 * the list is a dead end. Fetch it once, in the background. */
                static int index_tried;
                if (!index_tried) {
                    index_tried = 1;
                    struct stat ist;
                    if (stat("/run/mystic/repo.index", &ist) != 0) {
                        pid_t pid = fork();
                        if (pid == 0) {
                            execl("/bin/sh", "sh", "-c",
                                  "wget -q -T 10 -O /run/mystic/repo.index "
                                  "http://10.0.2.2:8000/index.tsv", (char *)NULL);
                            _exit(127);
                        }
                    }
                }

                if (take_request("/run/mystic/repo.refresh", want, sizeof(want))) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execl("/bin/sh", "sh", "-c",
                              "wget -q -T 10 -O /run/mystic/repo.index "
                              "http://10.0.2.2:8000/index.tsv", (char *)NULL);
                        _exit(127);
                    }
                }
            }

            /* another app can ask for a theme by writing this file */
            FILE *tf = fopen("/etc/mystic/current-theme", "r");            if (tf) {
                char want[64] = {0};
                if (fgets(want, sizeof(want), tf)) {
                    char *nl = strchr(want, '\n');
                    if (nl) *nl = 0;
                    for (int i = 0; i < theme_count; i++)
                        if (!strcmp(want, themes[i].name)) { theme_index = i; persist_settings(); }
                }
                fclose(tf);
            }

            /* the Apps window cannot fork by itself; it writes the bundle id
             * here and the desktop does the launch (same trick as the theme). */
            FILE *lr = fopen("/run/mystic/launch.request", "r");
            if (lr) {
                char want[64] = {0};
                if (fgets(want, sizeof(want), lr)) {
                    char *nl = strchr(want, 10);
                    if (nl) *nl = 0;
                    for (int b = 0; b < bundle_count; b++) {
                        if (!strcmp(want, bundles[b].id)) {
                            int pid = wms_launch(&bundles[b], clients, b);
                            fprintf(stderr, "wms: launcher request -> %s pid %d\n", bundles[b].id, pid);
                            break;
                        }
                    }
                }
                fclose(lr);
                remove("/run/mystic/launch.request");
            }
        }
        /* A mouse move used to set dirty, which recomposed the whole desktop -
         * every window's shadow and blur - for every single motion event.
         * That was the stutter the user felt when moving the pointer. Now the
         * cursor alone is redrawn, unless the pointer is over chrome that
         * highlights (the dock magnifies, the top bar reacts). */
        int over_chrome = input.mouse_y < topbar_h();
        {
            int bdx, bdy, bdw, bdh;
            if (dock_rect(&bdx, &bdy, &bdw, &bdh) &&
                input.mouse_y >= bdy - ui_px(24))
                over_chrome = 1;
        }
        int cursor_moved = (input.mouse_x != cur_x || input.mouse_y != cur_y);
        if ((input.mouse_dx || input.mouse_dy) && over_chrome) dirty = 1;

        if (dirty) {
            draw_desktop(&scene, &themes[theme_index]);
            surf_copy(&screen, &scene);
            draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
            disp_present(&disp, &screen);
            dirty = 0;
            cur_x = input.mouse_x;
            cur_y = input.mouse_y;
        } else if (cursor_moved) {
            /* put back the 24x26 box the old cursor covered, then draw the
             * new one. No blur, no shadows, no full-screen copy. */
            if (cur_x >= 0)
                copy_rect(&screen, &scene, cur_x - 2, cur_y - 2, 26, 28);
            draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
            if (cur_x >= 0) {
                int ux = cur_x < input.mouse_x ? cur_x : input.mouse_x;
                int uy = cur_y < input.mouse_y ? cur_y : input.mouse_y;
                int uw = (cur_x < input.mouse_x ? input.mouse_x - cur_x : cur_x - input.mouse_x) + 28;
                int uh = (cur_y < input.mouse_y ? input.mouse_y - cur_y : cur_y - input.mouse_y) + 30;
                disp_present_rect(&disp, &screen, ux - 2, uy - 2, uw, uh);
            } else {
                disp_present(&disp, &screen);
            }
            cur_x = input.mouse_x;
            cur_y = input.mouse_y;
        }

        struct pollfd pfd[2];
        pfd[0].fd = 0; pfd[0].events = 0; pfd[0].revents = 0;
        poll(pfd, 1, 16);
    }
    return 0;
}
