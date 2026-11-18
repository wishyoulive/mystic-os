/* ============================================================
 *  Mystic OS - mystic-desktop
 *
 *  The desktop and the window manager. Everything on screen is
 *  drawn by the CPU from real measurements: resolution from fb0,
 *  memory and processes from /proc, input from evdev.
 *
 *  Keys:   every binding lives in src/shortcuts.c, and Super+/ (or F1
 *          with nothing open) draws the list of them. Esc is a cancel
 *          key - it dismisses the launcher, the shortcut panel, an open
 *          menu, a drag in progress or the focused app's own modal, and
 *          it never closes a window. Ctrl+W / Ctrl+Q close the window,
 *          Alt+Tab switches, Super opens the launcher, Ctrl+Alt+L locks.
 *  Mouse:  drag a title bar to move a window, click a dock icon
 *          to launch an app, click a window's red dot to close it.
 * ============================================================ */
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "mystic.h"
#include "filetypes.h"
#include "ipc.h"
#include "wallpaper.h"
#include "shortcuts.h"
#include "clipboard.h"
#include "notify.h"
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

/* Desktop layout metrics live in src/ui.c next to ui_px() and the scale,
 * because they are functions of the text they hold - not of a pixel count
 * someone guessed. They are declared here rather than in mystic.h, which is
 * the app ABI: an app does not own the desktop's title bar. */
int  ui_line_h(int face, float size);
int  ui_title_h(void);
int  ui_topbar_h(void);
int  ui_tip_h(void);
int  ui_win_min_w(void);
int  ui_win_min_h(void);
int  ui_dock_icon(void);
int  ui_dock_gap(void);
int  ui_dock_pad(void);
int  ui_launcher_cols(int items, int sw, int sh, int label_w);
void ui_launcher_geom(int items, int sw, int sh, int label_w, int idx,
                      int *x, int *y, int *w, int *h);
void ui_launcher_panel(int items, int sw, int sh, int label_w,
                       int *x, int *y, int *w, int *h);

/* The two bands every window is framed by. Each is measured from the line it
 * draws, so the 21.6 px tooltip line can no longer end up in a 19 px box at
 * any of the five settings. */
static int title_h(void) { return ui_title_h(); }    /* window title bar */
static int topbar_h(void) { return ui_topbar_h(); }  /* screen top bar   */

/* Traffic-light diameter. It follows the text scale like every other metric,
 * and the position and the click target are derived from the same numbers the
 * painter uses - the old fixed 10/14/32/50 left the targets behind once the
 * title bar started growing. */
static int dot_sz(void) { return ui_px(7); }        /* 10 px at the shipped size */
/* x of light i: 14, 32, 50 at the shipped size, one dot + air apart */
static int dot_x(int i) { return ui_px(10 + 13 * i); }
static int title_x(void) { return dot_x(2) + dot_sz() + ui_px(6); }
/* F12: the dots, the title text and the click targets all hang off this one
 * vertical centre, so nothing drifts when the text scale changes the bar. */
static int traffic_y(int bar_h) { return (bar_h - dot_sz()) / 2; }
/* A bundle used to be a letter in a box ("C", "F", "2"), which is why the
 * launcher looked unfinished. Each known app now has the same kind of drawn
 * icon the built-ins have; anything unknown still falls back to its initial. */
static void icon_terminal(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_folder(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_gear(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_clock(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_paint(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_life(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_mandel(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_sysmon(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_calc(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_grid(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_bars(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_pencil(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_web(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_viewer(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_docview(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_player(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_2048(Surface*,float,float,float,u32,u32,const Theme*,double,int);
static void icon_sysinfo(Surface*,float,float,float,u32,u32,const Theme*,double,int);
typedef void (*IconFn)(Surface *, float, float, float, u32, u32,
                       const Theme *, double, int);
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
    if (!strcmp(id, "viewer"))     return icon_viewer;
    if (!strcmp(id, "docview"))    return icon_docview;
    /* The id is 'vlc' everywhere now - the bundle directory, the manifest's
     * 'bundle =' line, the Store row, the index and mc_open() - so a window
     * and its launcher tile show the same icon. 'vlcplayer' is kept only so
     * a window started by a bundle from before the rename still gets it. */
    if (!strcmp(id, "vlcplayer"))  return icon_player;
    if (!strcmp(id, "vlc"))        return icon_player;
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

static int dock_isz(void) { return ui_dock_icon() * dock_pct / 100; }  /* taskbar icon */
static int dock_gap(void) { return ui_dock_gap() * dock_pct / 100; }
static int dock_pad(void) { return ui_dock_pad(); }

static Display disp;
static Surface screen;
static Surface scene;
static Input   input;
static Theme   themes[16];   /* twelve are shipped; the array used to hold 8 */
static int     theme_count, theme_index;

/* Settings is a built-in app in this process. It must inspect the display the
 * compositor has open - opening /dev/fb0 twice would map the panel twice - so
 * the Display lives here and the app asks for it. */
Display *desk_display(void) { return &disp; }

/* The UI was authored on a 1280x800 screen. A panel bigger than that gets the
 * same design, mapped by the display layer (so a 1920x1080 screen does not
 * shrink every label); a panel smaller than it gets a logical frame its own
 * size. The result is never larger than the panel, and it is the only size
 * the compositor allocates. */
#define DESIGN_W 1280
#define DESIGN_H 800
void desk_design_size(int fb_w, int fb_h, int *w, int *h) {
    int lw = fb_w < DESIGN_W ? fb_w : DESIGN_W;
    int lh = fb_h < DESIGN_H ? fb_h : DESIGN_H;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    *w = lw;
    *h = lh;
}

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
    int   maxed;         /* maximised by double-click; 2 full screen, 3 snapped */
    int   restore_x, restore_y, restore_w, restore_h;
    int   on_top;        /* always-on-top: ranks above every ordinary window */
    int   term_size;     /* FONT_MONO point size (scaled later by font_get) */
    char  input_line[192];  /* bytes typed into this terminal since Enter */
    int   input_len;
    int   z;
    unsigned gen;        /* slot generation: bumped whenever the slot changes hands */
} Window;

static Window wins[MAX_WINDOWS];
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
static int    drag_ox, drag_oy;   /* where the drag began, so Esc can put it back */
static unsigned drag_gen;         /* generation of the window being dragged   */
static int    resize_win = -1;
static int    resize_ow, resize_oh; /* the size before the resize handle grabbed */
static unsigned resize_gen;       /* generation of the window being resized   */

/* Desktop context menu (right click on empty wallpaper). It is a modal for
 * the same reason the launcher is: Esc and a click elsewhere dismiss it,
 * and no key reaches a window while it is up. */
static int    menu_open, menu_sel, menu_x, menu_y;

/* A one-line confirmation above the dock. `status_t0` is 0 when there is
 * nothing to say; the once-a-second tick clears it after a few seconds. */
static char   status_msg[192];
static double status_t0;
static void   set_status(const char *fmt, ...);
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
static double pulse_last = 0.0;
static double drift_last = 0.0;
static int    launcher_sel = 0;

/* Black Hole interaction (src/wallpaper.h).
 *
 *   wall_drag       a left-button drag that began on the wallpaper, not on a
 *                   window or the chrome; its mouse_dy is fed to the view knob
 *   wall_bh_index   the index of the Black Hole wallpaper, found once by name
 *                   from the same table the rotation reads - no magic 4
 *
 * Both knobs are state changes only, so the next wallpaper_draw() picks them
 * up; while another wallpaper is selected the pointer must not touch them. */
static int    wall_drag = 0;
static int    wall_bh_index = -1;

static int wall_view_active(void) {
    if (wall_bh_index < 0) wall_bh_index = wallpaper_from_name("Black Hole");
    return wall_bh_index >= 0 && wall_index == wall_bh_index;
}

static WmClient clients[WM_MAX_CLIENTS];
static WmApp   bundles[WM_MAX_APPS];
static int     bundle_count = 0;

/* lock screen state */
static int    locked = 0;
static char   pin[64];
static int    pin_len = 0;
static char   lock_msg[96] = "";
static int    mouse_prev;

/* Shortcut state that has to live across frames.
 *
 *   super_down  the Meta key is held. input.c reports it like any other
 *               key, so the desktop tracks it itself - without that,
 *               Super+Left could only ever open the launcher.
 *   super_lone  Super was pressed and nothing else was: that tap is the
 *               launcher, and a tap is only known on the release.
 *   help_open   the keyboard-shortcut panel is up.
 *   dnd_on      do-not-disturb, also mirrored to /etc/mystic/dnd.
 *   dirty       the frame needs recomposing. It is at file scope so the
 *               shortcut actions, which live outside main(), can ask for
 *               a repaint the same way every other piece of code does.
 */
static int super_down, super_lone;
static int help_open, help_page, dnd_on;
static int dirty = 1;

/* ------------------------------------------------------------ helpers */
/* /proc/meminfo is read on the paint path (every compose), so sample it at
 * most once a second and answer from the snapshot in between. Two fopens
 * and a formatted walk per frame made a pure-paint function block on I/O. */
static long   mem_total_kb, mem_avail_kb;
static time_t mem_sampled_at;

static void mem_sample(void) {
    time_t t = time(NULL);
    if (mem_sampled_at == t && mem_sampled_at != 0) return;
    mem_sampled_at = t;
    mem_total_kb = mem_avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char k[64], unit[16];
    long v;
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3) {
        if (!strcmp(k, "MemTotal:")) mem_total_kb = v;
        else if (!strcmp(k, "MemAvailable:")) { mem_avail_kb = v; break; }
    }
    fclose(f);
}

static void clock_text(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* ------------------------------------------------------------ icons */
/* The host test extracts the block between the two markers below and builds
 * it against the real src/gfx.c, so what it renders is this file's icon code.
 * Keep it self-contained: mystic.h and <math.h>, no desktop state. */
/* @icons-begin */
/* ------------------------------------------------------------------
 *  Icon geometry, theme style and animation.
 *
 *  The host harness extracts this block verbatim (build.py) and compiles it
 *  against the real src/gfx.c, so what it renders is this code. Keep it
 *  self-contained: mystic.h and <math.h>, no desktop state.
 *
 *  One icon function is one drawing; ic_* applies the theme's icon_style. The
 *  styles are geometry, not a colour change - every one of them still draws
 *  the deliberate highlight part. The highlight colour is icon_accent for an
 *  ordinary icon and the BRIGHTER of accent/icon_accent when the icon is
 *  active (focused, hovered or selected) - on Neon that is body #FF2BD6 with
 *  highlight #22F0FF:
 *    solid    - the marks filled, the theme's normal corners
 *    outline  - the same silhouette as a 1.5 px stroke, no fill
 *    duotone  - filled marks, secondary parts in icon_accent
 *    rounded  - duotone with every corner at ~35% of the mark
 *    sharp    - duotone with 0-1 px corners
 *    pixel    - duotone snapped to a visible 3 px grid, square ends
 * ------------------------------------------------------------------ */

typedef void (*IconFn)(Surface *, float, float, float, u32, u32,
                       const Theme *, double, int);

typedef struct {
    Surface   *s;
    const Theme *th;
    float      x, y, sz;
    u32        fg, hi;
    IconStyle  style;
    AnimKind   anim;
    float      speed, amp;
    double     now;
    int        active;
} IconDraw;

static IconDraw icon_ctx(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                         const Theme *th, double now, int active) {
    IconDraw c;
    c.s = s;
    c.th = th;
    c.x = x; c.y = y; c.sz = sz;
    c.fg = fg; c.hi = hi;
    c.style = th ? th->icon_style : ICON_STYLE_DUOTONE;
    c.anim = th ? th->anim : ANIM_NONE;
    c.speed = th ? th->anim_speed : 0.0f;
    c.amp = th ? th->anim_amp : 0.0f;
    c.now = now;
    c.active = active;
    if (c.speed <= 0.0f || c.amp <= 0.0f) c.anim = ANIM_NONE;
    return c;
}

/* Colour is not a style: every style draws the same two-tone icon, and the
 * style only changes its geometry. `second` selects the deliberate highlight
 * part (chevron, tab, hub, hands, nib, tallest bar, inner ring, peak,
 * meridian, inner square, play triangle, rules). Its colour is icon_accent for
 * an ordinary icon, and the brighter of accent/icon_accent when the icon is
 * active; a monochrome look comes from a theme whose two colours are equal (or
 * that omits icon_accent, which defaults to accent), not from throwing the
 * highlight away. */
static u32 ic_col(const IconDraw *c, int second) {
    return second ? c->hi : c->fg;
}

static float ic_radius(const IconDraw *c, float w, float h, float r) {
    switch (c->style) {
    case ICON_STYLE_ROUNDED: {
        float m = w < h ? w : h;
        return m * 0.35f;
    }
    case ICON_STYLE_SHARP:
        return r > 1.0f ? 1.0f : r;
    case ICON_STYLE_PIXEL:
        return 0.0f;
    default:
        return r;
    }
}

/* Snap to the pixel grid, measured from the icon's own origin so the grid is
 * stable when the icon is drawn at different places. */
static float ic_grid(const IconDraw *c, float v, float origin) {
    if (c->style != ICON_STYLE_PIXEL) return v;
    return origin + floorf((v - origin) / 3.0f + 0.5f) * 3.0f;
}

/* The one stroke width the outline style uses, and the pixel grid's stroke. */
#define IC_STROKE 1.5f
#define IC_PIXEL  3.0f

static void ic_rect(IconDraw *c, float x, float y, float w, float h,
                    float r, int second) {
    if (c->style == ICON_STYLE_PIXEL) {
        float xe = ic_grid(c, x + w, c->x);
        float ye = ic_grid(c, y + h, c->y);
        x = ic_grid(c, x, c->x);
        y = ic_grid(c, y, c->y);
        w = xe - x; h = ye - y;
        if (w < IC_PIXEL) w = IC_PIXEL;
        if (h < IC_PIXEL) h = IC_PIXEL;
    }
    float rr = ic_radius(c, w, h, r);
    if (c->style == ICON_STYLE_OUTLINE)
        gfx_rrect_aa(c->s, x, y, w, h, rr, 0, ic_col(c, second), IC_STROKE);
    else
        gfx_rrect(c->s, x, y, w, h, rr, ic_col(c, second));
}

/* A ring: the hollow shapes (clock face, globe, gear body, frames). */
static void ic_ring(IconDraw *c, float cx, float cy, float rad,
                    float width, int second) {
    if (c->style == ICON_STYLE_PIXEL) {
        cx = ic_grid(c, cx, c->x);
        cy = ic_grid(c, cy, c->y);
        rad = ic_grid(c, rad, 3.0f);
        if (rad < IC_PIXEL) rad = IC_PIXEL;
    }
    float d = rad * 2.0f;
    float rr = ic_radius(c, d, d, rad);
    float w = (c->style == ICON_STYLE_OUTLINE) ? IC_STROKE : width;
    if (c->style == ICON_STYLE_PIXEL) w = IC_PIXEL;
    gfx_rrect_aa(c->s, cx - rad, cy - rad, d, d, rr, 0, ic_col(c, second), w);
}

/* A framed rectangle: the page/frame/panel shapes that are not circles. */
static void ic_rect_ring(IconDraw *c, float x, float y, float w, float h,
                         float r, float width, int second) {
    if (c->style == ICON_STYLE_PIXEL) {
        float xe = ic_grid(c, x + w, c->x);
        float ye = ic_grid(c, y + h, c->y);
        x = ic_grid(c, x, c->x);
        y = ic_grid(c, y, c->y);
        w = xe - x; h = ye - y;
        if (w < IC_PIXEL) w = IC_PIXEL;
        if (h < IC_PIXEL) h = IC_PIXEL;
    }
    float rr = ic_radius(c, w, h, r);
    float wd = (c->style == ICON_STYLE_OUTLINE) ? IC_STROKE : width;
    if (c->style == ICON_STYLE_PIXEL) wd = IC_PIXEL;
    gfx_rrect_aa(c->s, x, y, w, h, rr, 0, ic_col(c, second), wd);
}

static void ic_disc(IconDraw *c, float cx, float cy, float rad, int second) {
    if (c->style == ICON_STYLE_PIXEL) {
        cx = ic_grid(c, cx, c->x);
        cy = ic_grid(c, cy, c->y);
        rad = ic_grid(c, rad, 3.0f);
        if (rad < IC_PIXEL) rad = IC_PIXEL;
    }
    if (c->style == ICON_STYLE_OUTLINE) {
        gfx_rrect_aa(c->s, cx - rad, cy - rad, rad * 2, rad * 2, rad,
                     0, ic_col(c, second), IC_STROKE);
    } else {
        gfx_rrect(c->s, cx - rad, cy - rad, rad * 2, rad * 2, rad,
                  ic_col(c, second));
    }
}

static void ic_line(IconDraw *c, float x0, float y0, float x1, float y1,
                    float width, int second) {
    if (c->style == ICON_STYLE_PIXEL) {
        x0 = ic_grid(c, x0, c->x); y0 = ic_grid(c, y0, c->y);
        x1 = ic_grid(c, x1, c->x); y1 = ic_grid(c, y1, c->y);
        width = IC_PIXEL;
    } else if (c->style == ICON_STYLE_OUTLINE) {
        width = IC_STROKE;
    }
    gfx_line(c->s, x0, y0, x1, y1, width, ic_col(c, second));
}

static void ic_poly(IconDraw *c, const float *pts, int n, int second) {
    if (n < 3) return;
    float tmp[24];
    if (n > 12) n = 12;
    if (c->style == ICON_STYLE_PIXEL) {
        for (int i = 0; i < n; i++) {
            tmp[i * 2]     = ic_grid(c, pts[i * 2], c->x);
            tmp[i * 2 + 1] = ic_grid(c, pts[i * 2 + 1], c->y);
        }
        pts = tmp;
    }
    if (c->style == ICON_STYLE_OUTLINE) {
        u32 col = ic_col(c, second);
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            gfx_line(c->s, pts[i * 2], pts[i * 2 + 1],
                     pts[j * 2], pts[j * 2 + 1], IC_STROKE, col);
        }
    } else {
        gfx_polygon(c->s, pts, n, ic_col(c, second));
    }
}

/* Whether the desktop has to keep drawing because a theme-animated item is
 * active. The three inputs are the only things allowed to open this gate: a
 * hovered dock icon, a hovered launcher tile, and the launcher's keyboard
 * selection (which marks a tile active without the pointer being on it).
 * Nothing else belongs here. In particular the start button's eye is not an
 * item: an earlier version let it hold the gate open for ANIM_ORBIT themes, so
 * Aurora and Nebula repainted at 30 fps with the pointer nowhere near an item.
 * A still theme (anim_speed = 0, anim_amp = 0 or anim = none) never gets past
 * the first test, so an idle desktop asks for no frames at all. */
static int anim_gate(AnimKind kind, float speed, float amp,
                     int dock_item, int tile, int selected_tile) {
    if (kind == ANIM_NONE || speed <= 0.0f || amp <= 0.0f) return 0;
    return dock_item || tile || selected_tile;
}

/* ------------------------------------------------------------ the icons */

static void icon_terminal(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                          const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float pad = sz * 0.26f;
    ic_line(&c, x + pad, y + pad * 1.2f, x + pad + sz * 0.18f, y + sz * 0.5f,
            sz * 0.09f, 1);
    ic_line(&c, x + pad + sz * 0.18f, y + sz * 0.5f, x + pad, y + sz - pad * 1.2f,
            sz * 0.09f, 1);
    ic_line(&c, x + sz * 0.48f, y + sz - pad, x + sz - pad, y + sz - pad,
            sz * 0.09f, 0);
}

static void icon_folder(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float w = sz * 0.62f, h = sz * 0.44f;
    float bx = x + (sz - w) * 0.5f, by = y + (sz - h) * 0.5f + sz * 0.04f;
    ic_rect(&c, bx - sz * 0.02f, by - sz * 0.14f, w * 0.42f, sz * 0.12f,
            sz * 0.02f, 1);
    ic_rect(&c, bx, by, w, h, sz * 0.06f, 0);
}

static void icon_gear(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    /* spin: every tooth is a quad, rotated together. The hub and the ring
     * stand still, so the icon still reads as a gear. */
    float spin = (c.anim == ANIM_SPIN && c.active)
                     ? (float)c.now * c.speed * 1.2f * c.amp : 0.0f;
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 3.14159265f / 4.0f + spin;
        float ca = cosf(a), sa = sinf(a);
        float r0 = sz * 0.24f, r1 = sz * 0.40f, hw = sz * 0.055f;
        float p0x = cx + ca * r0, p0y = cy + sa * r0;
        float p1x = cx + ca * r1, p1y = cy + sa * r1;
        float pts[8] = {
            p0x + sa * hw, p0y - ca * hw,
            p1x + sa * hw, p1y - ca * hw,
            p1x - sa * hw, p1y + ca * hw,
            p0x - sa * hw, p0y + ca * hw,
        };
        ic_poly(&c, pts, 4, 0);
    }
    ic_ring(&c, cx, cy, sz * 0.19f, sz * 0.08f, 0);
    ic_disc(&c, cx, cy, sz * 0.09f, 1);       /* hub: the second colour */
}

static void icon_clock(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                       const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.36f;
    ic_ring(&c, cx, cy, r, sz * 0.07f, 0);
    ic_line(&c, cx, cy, cx, cy - r * 0.62f, sz * 0.07f, 1);
    ic_line(&c, cx, cy, cx + r * 0.52f, cy + r * 0.18f, sz * 0.07f, 1);
}

static void icon_paint(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                       const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float px = x + sz * 0.30f, py = y + sz * 0.72f;
    ic_line(&c, px, py, x + sz * 0.74f, y + sz * 0.26f, sz * 0.13f, 0);
    ic_disc(&c, x + sz * 0.26f, y + sz * 0.70f, sz * 0.08f, 1);
}

static void icon_life(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float q = sz * 0.11f, g = sz * 0.17f;
    float ox = x + (sz - (2 * g + q)) / 2, oy = y + (sz - (2 * g + q)) / 2;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++)
            if ((r * 3 + col) % 4 != 1)
                ic_rect(&c, ox + col * g, oy + r * g, q, q, q * 0.35f, 0);
    ic_rect(&c, ox + g, oy + g, q, q, q * 0.35f, 1);   /* the live cell */
}

static void icon_mandel(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float cx = x + sz * 0.42f, cy = y + sz * 0.5f;
    for (int i = 0; i < 3; i++) {
        float r = sz * (0.10f + i * 0.10f);
        ic_ring(&c, cx, cy, r, sz * 0.055f, i == 0 ? 1 : 0);
    }
    ic_disc(&c, cx + sz * 0.28f, cy, sz * 0.06f, 0);
}

static void icon_sysmon(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    ic_line(&c, x + sz * 0.18f, y + sz * 0.66f, x + sz * 0.36f, y + sz * 0.42f,
            sz * 0.07f, 0);
    ic_line(&c, x + sz * 0.36f, y + sz * 0.42f, x + sz * 0.54f, y + sz * 0.56f,
            sz * 0.07f, 0);
    ic_line(&c, x + sz * 0.54f, y + sz * 0.56f, x + sz * 0.82f, y + sz * 0.30f,
            sz * 0.07f, 1);                        /* the peak */
}

static void icon_calc(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    ic_rect_ring(&c, x + sz * 0.18f, y + sz * 0.10f, sz * 0.64f, sz * 0.80f,
                 sz * 0.10f, sz * 0.055f, 0);
    ic_rect(&c, x + sz * 0.27f, y + sz * 0.19f, sz * 0.46f, sz * 0.16f,
            sz * 0.03f, 1);
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++)
            ic_rect(&c, x + sz * (0.27f + q * 0.17f),
                    y + sz * (0.44f + r * 0.15f), sz * 0.11f, sz * 0.10f,
                    sz * 0.02f, 0);
}

static void icon_grid(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float q = sz * 0.30f, gap = sz * 0.12f;
    float ox = x + (sz - 2 * q - gap) / 2, oy = y + (sz - 2 * q - gap) / 2;
    for (int r = 0; r < 2; r++)
        for (int col = 0; col < 2; col++)
            ic_rect(&c, ox + col * (q + gap), oy + r * (q + gap), q, q,
                    q * 0.22f, (r == 0 && col == 0) ? 1 : 0);
}

/* A page with a picture on it: the Viewer opens images and text. */
static void icon_viewer(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    ic_rect_ring(&c, x + sz * 0.16f, y + sz * 0.10f, sz * 0.68f, sz * 0.80f,
                 sz * 0.08f, sz * 0.055f, 0);
    ic_rect(&c, x + sz * 0.26f, y + sz * 0.20f, sz * 0.48f, sz * 0.34f,
            sz * 0.04f, 1);
    ic_rect(&c, x + sz * 0.26f, y + sz * 0.62f, sz * 0.48f, sz * 0.06f,
            sz * 0.02f, 0);
    ic_rect(&c, x + sz * 0.26f, y + sz * 0.72f, sz * 0.32f, sz * 0.06f,
            sz * 0.02f, 0);
}

/* A document with a folded corner: the Documents app. */
static void icon_docview(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                         const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    ic_rect_ring(&c, x + sz * 0.18f, y + sz * 0.10f, sz * 0.60f, sz * 0.80f,
                 sz * 0.07f, sz * 0.055f, 0);
    ic_line(&c, x + sz * 0.56f, y + sz * 0.10f, x + sz * 0.78f, y + sz * 0.32f,
            sz * 0.055f, 0);
    for (int i = 0; i < 3; i++)
        ic_rect(&c, x + sz * 0.28f, y + sz * (0.50f + (float)i * 0.12f),
                sz * (0.36f - (float)i * 0.06f), sz * 0.05f, sz * 0.02f, 1);
}

/* A play triangle in a disc: the video player. */
static void icon_player(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.34f;
    ic_ring(&c, cx, cy, r, sz * 0.06f, 0);
    float tri[6] = { cx - r * 0.28f, cy - r * 0.44f,
                     cx + r * 0.46f, cy,
                     cx - r * 0.28f, cy + r * 0.44f };
    ic_poly(&c, tri, 3, 1);
}

static void icon_web(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                     const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.36f;
    ic_ring(&c, cx, cy, r, sz * 0.065f, 0);
    ic_line(&c, cx - r, cy, cx + r, cy, sz * 0.055f, 0);          /* equator */
    ic_line(&c, cx, cy - r, cx, cy + r, sz * 0.055f, 1);          /* meridian */
    ic_line(&c, cx - r * 0.62f, cy - r * 0.72f,
            cx + r * 0.62f, cy - r * 0.72f, sz * 0.05f, 0);
    ic_line(&c, cx - r * 0.62f, cy + r * 0.72f,
            cx + r * 0.62f, cy + r * 0.72f, sz * 0.05f, 0);
}

static void icon_sysinfo(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                         const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    /* an info "i" in a rounded panel: the System app is a read-out, not a gear */
    ic_rect_ring(&c, x + sz * 0.18f, y + sz * 0.12f, sz * 0.64f, sz * 0.76f,
                 sz * 0.10f, sz * 0.055f, 0);
    float cx = x + sz * 0.5f;
    ic_rect(&c, cx - sz * 0.035f, y + sz * 0.26f, sz * 0.07f, sz * 0.10f,
            sz * 0.02f, 1);
    ic_rect(&c, cx - sz * 0.035f, y + sz * 0.42f, sz * 0.07f, sz * 0.24f,
            sz * 0.02f, 1);
}

static void icon_2048(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float q = sz * 0.27f;
    float ox = x + (sz - 2 * q - sz * 0.10f) / 2;
    float oy = y + (sz - 2 * q - sz * 0.10f) / 2;
    ic_rect(&c, ox, oy, q, q, sz * 0.06f, 1);
    ic_rect(&c, ox + q + sz * 0.10f, oy, q, q, sz * 0.06f, 0);
    ic_rect(&c, ox, oy + q + sz * 0.10f, q, q, sz * 0.06f, 0);
    ic_rect(&c, ox + q + sz * 0.10f, oy + q + sz * 0.10f, q, q,
            sz * 0.06f, 0);
}

static void icon_bars(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                      const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    /* wave: three bars, each a step behind the previous one. The tallest is
     * the second colour; the group never grows past the icon box. */
    float phase = (c.anim == ANIM_WAVE && c.active)
                      ? (float)c.now * c.speed * 6.2831853f * 0.6f : 0.0f;
    for (int i = 0; i < 3; i++) {
        float base = 0.22f + (float)i * 0.16f;
        if (c.anim == ANIM_WAVE && c.active)
            base += 0.08f * c.amp * sinf(phase + (float)i * 1.1f);
        if (base < 0.08f) base = 0.08f;
        if (base > 0.70f) base = 0.70f;
        float hh = sz * base;
        ic_rect(&c, x + sz * (0.24f + (float)i * 0.20f),
                y + sz - sz * 0.22f - hh, sz * 0.10f, hh, sz * 0.03f,
                i == 2 ? 1 : 0);
    }
}

static void icon_pencil(Surface *s, float x, float y, float sz, u32 fg, u32 hi,
                        const Theme *th, double now, int active) {
    IconDraw c = icon_ctx(s, x, y, sz, fg, hi, th, now, active);
    float pts[8] = {
        x + sz * 0.26f, y + sz * 0.74f,
        x + sz * 0.70f, y + sz * 0.30f,
        x + sz * 0.60f, y + sz * 0.20f,
        x + sz * 0.16f, y + sz * 0.64f,
    };
    ic_poly(&c, pts, 4, 0);
    ic_line(&c, x + sz * 0.16f, y + sz * 0.64f,
            x + sz * 0.20f, y + sz * 0.80f, sz * 0.05f, 1);   /* the nib */
}

/* ------------------------------------------------- animation dispatcher --
 * Every animation is derived from the theme's anim kind. The ones that move an
 * item act only on `active` (hovered dock icon, selected launcher tile); drift
 * is a background decoration and has its own function below. Sizes only ever
 * shrink, so nothing can leave its tile or hit box.
 */
static void icon_draw(IconFn fn, Surface *s, float x, float y, float sz,
                      u32 fg, u32 hi, const Theme *th, double now, int active) {
    if (!fn) return;
    AnimKind kind = th ? th->anim : ANIM_NONE;
    float amp = th ? th->anim_amp : 0.0f;
    float speed = th ? th->anim_speed : 0.0f;
    if (speed <= 0.0f || amp <= 0.0f) kind = ANIM_NONE;

    /* The breathe is the one motion every moving theme has. It shrinks
     * around the icon's centre, so it can never leave the tile or the dock
     * hit box; anim_style adds an optional second motion on top. */
    if (kind != ANIM_NONE && active) {
        float scale = theme_icon_pulse_scale(th, now);
        float ns = sz * scale;
        x += (sz - ns) * 0.5f;
        y += (sz - ns) * 0.5f;
        sz = ns;
    }
    if (kind == ANIM_GLOW && active && th) {
        float phase = (float)now * speed * 6.2831853f * 0.5f;
        float r = sz * (0.62f + 0.12f * sinf(phase)) * (0.6f + 0.4f * amp);
        gfx_glow(s, x + sz * 0.5f, y + sz * 0.5f, r,
                 color_alpha(th->glow, 0.55f), 0.55f * amp);
    }

    fn(s, x, y, sz, fg, hi, th, now, active);

    if (kind == ANIM_ORBIT && active) {
        float a = (float)now * speed * 1.5f;
        float rr = sz * 0.44f;
        float dr = sz * 0.035f;
        if (dr < 1.5f) dr = 1.5f;
        gfx_rrect(s, x + sz * 0.5f + cosf(a) * rr - dr,
                  y + sz * 0.5f + sinf(a) * rr - dr, dr * 2, dr * 2, dr, hi);
    }
}

/* ANIM_DRIFT is the one kind that moves the background, never text or hit
 * boxes: one soft light in the theme's glow colour wanders slowly. It is
 * called with the desktop surface before any window is drawn. */
static void anim_drift_backdrop(Surface *s, const Theme *th, double now) {
    if (!s || !th || th->anim != ANIM_DRIFT ||
        th->anim_speed <= 0.0f || th->anim_amp <= 0.0f) return;
    float amp = th->anim_amp;
    float t = (float)now * th->anim_speed;
    float minw = (float)(s->w < s->h ? s->w : s->h);
    float cx = (float)s->w * (0.5f + 0.30f * sinf(t * 0.11f));
    float cy = (float)s->h * (0.5f + 0.26f * cosf(t * 0.09f));
    float r = minw * (0.34f + 0.06f * sinf(t * 0.17f));
    gfx_glow(s, cx, cy, r, color_alpha(th->glow, 0.4f), 0.55f * amp);
}

/* @icons-end */

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

/* The stacking rank: always-on-top windows outrank ordinary ones whatever
 * their z is, and z orders within a layer. One function, so win_top(),
 * the painter, hit-testing and the focus ring cannot disagree about which
 * window is in front. */
static long win_rank(const Window *w) {
    return ((long)(w->on_top ? 1 : 0) << 30) + (long)w->z;
}

static Window *win_top(void) {
    Window *best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].hidden &&
            (!best || win_rank(&wins[i]) > win_rank(best)))
            best = &wins[i];
    return best;
}

/* Keyboard focus is not the same thing as the top of the stack any more:
 * an always-on-top window is drawn and hit-tested above ordinary ones, but
 * clicking an ordinary window must still send the keyboard there. The slot
 * is remembered with its generation, so a recycled slot is not mistaken
 * for the window that used to live in it. */
static int      focus_win = -1;
static unsigned focus_gen;

static Window *win_focused(void) {
    if (focus_win >= 0 && focus_win < MAX_WINDOWS) {
        Window *w = &wins[focus_win];
        if (w->used && !w->hidden && w->gen == focus_gen) return w;
    }
    return win_top();
}

static void win_focus(Window *w) {
    if (!w) return;
    w->z = ++z_top;
    focus_win = (int)(w - wins);
    focus_gen = w->gen;
}

/* One place that tells a window's owner its content size changed: apps get a
 * resize callback, external clients a RESIZE message, and terminals recompute
 * their pty grid (without this the shell kept wrapping at the old width). */
static void window_resized(Window *w) {
    if (!w || !w->used) return;
    int cw = w->w - 2;
    int chh = w->h - title_h() - 2;
    /* the floor is a scaled metric like the minimums it protects */
    if (cw < ui_px(29)) cw = ui_px(29);
    if (chh < ui_px(21)) chh = ui_px(21);
    if (w->term) {
        term_resize_px(w->term, cw, chh);
    } else if (w->kind == WIN_CLIENT && w->client >= 0) {
        wms_notify_resize(&clients[w->client], cw, chh);
    } else if (w->app && w->app->vt && w->app->vt->resize) {
        w->app->vt->resize(w->app, cw, chh);
    }
}

/* The display can change while the desktop runs: Settings asks the panel for
 * another mode, and the design size for that panel can be different. The two
 * compositor surfaces are reallocated here, the window server is told the new
 * logical screen, and every window is pulled back inside it. Returns 1 when
 * the size actually changed, so the caller only asks for a repaint then. */
static int desktop_reflow(int w, int h) {
    if (w < 1 || h < 1) return 0;
    if (w == screen.w && h == screen.h) return 0;
    if (!screen.px || !scene.px) return 0;

    u32 *np = calloc((size_t)w * (size_t)h, sizeof(u32));
    u32 *ns = calloc((size_t)w * (size_t)h, sizeof(u32));
    if (!np || !ns) {
        free(np);
        free(ns);
        fprintf(stderr, "display: out of memory for a %dx%d surface\n", w, h);
        return 0;
    }
    free(screen.px);
    free(scene.px);
    screen.px = np; screen.w = w; screen.h = h; screen.stride = w;
    scene = screen; scene.px = ns;

    wms_set_screen(w, h);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *win = &wins[i];
        if (!win->used) continue;
        if (win->maxed == 2) {              /* a full-screen window follows */
            win->x = 0;
            win->y = topbar_h();
            win->w = w;
            win->h = h - topbar_h();
            if (win->h < ui_win_min_h()) win->h = h;
        } else {
            int minw = ui_win_min_w(), minh = ui_win_min_h();
            if (win->w < minw) win->w = minw;
            if (win->h < minh) win->h = minh;
            if (win->w > w) win->w = w;
            if (win->h > h) win->h = h;
            if (win->x < 0) win->x = 0;
            if (win->y < 0) win->y = 0;
            if (win->x + win->w > w) win->x = w - win->w;
            if (win->y + win->h > h) win->y = h - win->h;
        }
        window_resized(win);
    }
    dock_pct = 100;
    dirty = 1;
    /* the inverse mapper reads these even before the next present() */
    disp.src_w = w;
    disp.src_h = h;
    fprintf(stderr, "display: logical surface %dx%d\n", w, h);
    return 1;
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
    static const char *names[] = { "ui.conf", "wallpaper", "current-theme",
                                   "display.conf" };
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
            /* 640x380 design px is 832x494 at 1.0 and 1434x851 at 1.6: the
             * largest text scale no longer fits it on an 800x1280 screen, so
             * the same screen clamps the app windows get apply here too. */
            if (w->w > screen.w - ui_px(40)) w->w = screen.w - ui_px(40);
            {
                int tmax = screen.h - topbar_h() - ui_px(90);
                if (w->h > tmax) w->h = tmax;
                if (w->h < ui_win_min_h()) w->h = ui_win_min_h();
            }
            w->x = ui_px(43) + (i % 3) * ui_px(29);
            w->y = ui_px(50) + (i % 3) * ui_px(29);
            w->term_size = 12;          /* Ctrl+Shift+= / - change it later */
            Font *fmc = font_get(FONT_MONO, w->term_size);
            float cw = font_width(fmc, "M");
            float ch = font_height(fmc);
            if (cw < 4.0f) cw = 4.0f;
            if (ch < 8.0f) ch = 8.0f;
            /* the grid the terminal will actually be drawn into: the window
             * minus its frame and title bar, not a guessed 24/56 px */
            int cols = (w->w - 2) / (int)cw;
            int rows = (w->h - title_h() - 2) / (int)ch;
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            w->term = term_create(cols, rows, "/bin/sh");
            snprintf(w->title, sizeof(w->title), "Terminal");
            if (!w->term) { win_reset(w); return NULL; }
        } else {
            /* The calculator is portrait: the owner asked for it to grow
             * downwards rather than sideways, so it opens tall and narrow.
             * Every other app keeps the wide default. The height is clamped to
             * the screen because ui_px(520) is 1165 px at the largest text
             * scale, which would push the title bar above the top edge. */
            if (app_id && !strcmp(app_id, "calc")) {
                w->w = ui_px(300);
                w->h = ui_px(520);
            } else {
                w->w = ui_px(520);
                w->h = ui_px(360);
            }
            {
                int maxh = screen.h - topbar_h() - ui_px(90);
                if (w->h > maxh) w->h = maxh;
                if (w->h < ui_win_min_h()) w->h = ui_win_min_h();
                if (w->w > screen.w - ui_px(40)) w->w = screen.w - ui_px(40);
                if (w->w < ui_win_min_w()) w->w = ui_win_min_w();
            }
            w->x = ui_px(100) + (i % 3) * ui_px(29);
            w->y = ui_px(79) + (i % 3) * ui_px(29);
            /* the app is handed its CONTENT box: the frame and the title bar
             * were a hardcoded 40, which is 26 px wrong at the largest text
             * scale - the app drew its last row underneath the title bar */
            w->app = app_new(app_id, w->w - 2, w->h - title_h() - 2);
            if (!w->app) { win_reset(w); return NULL; }
            snprintf(w->title, sizeof(w->title), "%s", app_title(w->app));
        }
        if (w->x + w->w > screen.w) w->x = screen.w - w->w - ui_px(14);
        if (w->y + w->h > screen.h) w->y = screen.h - w->h - ui_px(64);
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
    while (w > screen.w - ui_px(6) && dock_pct > 40) {   /* shrink until it fits */
        dock_pct -= 5;
        isz = dock_isz(); gap = dock_gap();
        w = n * isz + (n - 1) * gap + pad * 2;
    }
    int h = isz + pad * 2;
    *dw = w; *dh = h;
    *dx = (screen.w - w) / 2;
    if (*dx < ui_px(3)) *dx = ui_px(3);
    /* the bar's own gap from the bottom edge of the screen */
    *dy = screen.h - h - ui_px(16);
    return 1;
}

/* True over the top bar or the dock: the two bands that are not wallpaper.
 * The Black Hole's drag and wheel knobs only apply on the wallpaper itself,
 * so a press on the dock (between its icons) does not start a camera drag. */
static int over_dock(int mx, int my) {
    int dx, dy, dw, dh;
    if (!dock_rect(&dx, &dy, &dw, &dh)) return 0;
    return mx >= dx && mx < dx + dw && my >= dy && my < dy + dh;
}

static int over_chrome_at(int mx, int my) {
    return my < topbar_h() || over_dock(mx, my);
}

static void draw_launcher(Surface *s, const Theme *th);
static void draw_menu(Surface *s, const Theme *th);
static int  launcher_items(void);
static void draw_lock_screen(Surface *s, const Theme *th);

/* ------------------------------------------------------------ start button
 * The Mystic mark: an eye. Two circular arcs make the lens (vesica), the
 * iris is a real circle, and pressing it blinks - the lid closes in 90 ms
 * and opens in 170 ms, which is roughly how a human eye does it.
 *
 * Everything is drawn from geometry: no glyph, no image, crisp at any size.
 * The mark keeps its designed size (a logo is not text) and is centred on the
 * bar, which is what keeps it on the bar when the bar grows.
 */
#define EYE_CX   19
#define EYE_CY   17
#define EYE_RX   12.0f
#define EYE_RY   7.6f
#define EYE_HIT_X 4
#define EYE_HIT_Y 3
#define EYE_HIT_W 30
#define EYE_HIT_H 28

/* how far the whole mark is shifted down to sit on the middle of the bar */
static int eye_oy(void) { return (topbar_h() - EYE_HIT_H) / 2 - EYE_HIT_Y; }

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

static void draw_eye(Surface *s, const Theme *th, float open, int oy) {
    int hover = (input.mouse_x >= EYE_HIT_X && input.mouse_x < EYE_HIT_X + EYE_HIT_W &&
                 input.mouse_y >= EYE_HIT_Y + oy && input.mouse_y < EYE_HIT_Y + oy + EYE_HIT_H);
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
            blend_px(s, x, y + oy, sclera, lens * 0.97f);

            /* iris, then pupil, then a highlight - all clipped by the lid */
            float ic = disc_cov(px, py, iris_r);
            if (ic > 0.0f) blend_px(s, x, y + oy, iris, ic * lens);

            float pc = disc_cov(px, py, iris_r * 0.58f);
            if (pc > 0.0f) blend_px(s, x, y + oy, pupil, pc * lens);

            float hx = px + iris_r * 0.36f, hy = py + iris_r * 0.40f;
            float hc = disc_cov(hx, hy, iris_r * 0.19f);
            if (hc > 0.0f) blend_px(s, x, y + oy, 0xFFFFFFFFu, hc * lens * 0.95f);

            /* lid outline: only where the lens coverage is partial, i.e. the
             * boundary. (Using 1-lens here paints the whole interior - that
             * mistake turned the eye into a black blob.) */
            if (lens < 0.58f)
                blend_px(s, x, y + oy, ring, (0.58f - lens) * 2.0f);
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
    draw_eye(s, th, open, eye_oy());

    /* The eye does not animate on its own and nothing orbits it. It used to
     * grow a dot for ANIM_ORBIT themes while hovered or with the launcher up,
     * which forced a repaint every 33 ms on an otherwise idle desktop and made
     * the start button the only animated thing on screen. Orbit belongs to the
     * active dock/launcher icon (icon_draw draws it there); the eye blinks on
     * a press and is still otherwise. */
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
    /* A static wallpaper is evaluated per pixel - the black hole traces
     * every ray - and it was being recomputed on every frame, even though
     * nothing about it changes between theme switches. Cache it and blit, so
     * a frame costs a memcpy instead of a full re-render. Animated
     * wallpapers keep drawing every frame; that is the point of them. */
    {
        static Surface wall;
        static int wall_key = -1;
        int key = (th->name[0] ? (int)th->name[0] : 0) * 131 + wall_index * 7 +
                  s->w * 3 + s->h * 5 + (int)(ui_scale_get() * 100);
        int animated = wallpaper_animated(wall_index);
        if (!animated && wall.px && wall_key == key && wall.w == s->w && wall.h == s->h) {
            surf_copy(s, &wall);
        } else {
            wallpaper_draw(s, th, wall_index, now_sec() - wall_t0);
            if (!animated) {
                if (!wall.px || wall.w != s->w || wall.h != s->h) {
                    free(wall.px);
                    wall.w = s->w;
                    wall.h = s->h;
                    wall.stride = s->w;
                    wall.cx0 = wall.cy0 = wall.cx1 = wall.cy1 = 0;
                    wall.px = calloc((size_t)wall.w * wall.h, sizeof(u32));
                }
                if (wall.px) {
                    surf_copy(&wall, s);
                    wall_key = key;
                }
            }
        }
    }

    /* ANIM_DRIFT is background decoration: one soft light wandering behind
     * the windows, before any text or hit box exists. */
    anim_drift_backdrop(s, th, now_sec());

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
    font_draw(s, f13, (float)ui_px(33), y13, "Mystic", th->text);

    char right[128];
    char clock[16];
    clock_text(clock, sizeof(clock));
    long mem, avail;
    mem_sample();
    mem = mem_total_kb;
    avail = mem_avail_kb;
    long used = mem - avail;
    if (used < 0) used = 0;
    snprintf(right, sizeof(right), "%s%s      %ld MB used of %ld MB",
             dnd_on ? "DND  \xC2\xB7  " : "", clock, used / 1024, mem / 1024);
    float rw = font_width(f11, right);
    font_draw(s, f11, (float)W - rw - (float)ui_px(11), y11, right, th->text_dim);

    Window *top = win_focused();
    if (top) {
        /* F10: clip by truncation to the band that is actually free, between
         * the brand label and the clock block. A symmetric band keeps the
         * title on the screen centre; on a narrow screen at a large scale the
         * band is lopsided, so use what is left rather than draw nothing.
         * Before this a long title painted straight over both. */
        const float pad = (float)ui_px(9);
        float band_lo = (float)ui_px(33) + brand_w + pad;
        float band_hi = (float)W - rw - (float)ui_px(11) - pad;
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
    /* draw in ascending rank order: always-on-top last */
    long last_rank = -1;
    for (int n = 0; n < MAX_WINDOWS; n++) {
        Window *cur = NULL;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].used || wins[i].hidden) continue;
            long r = win_rank(&wins[i]);
            if (r <= last_rank) continue;
            if (!cur || r < win_rank(cur)) cur = &wins[i];
        }
        if (!cur) break;
        last_rank = win_rank(cur);

        float cx = th->corner;
        /* The window body below is opaque, so the shadow under it is
         * invisible: gfx_shadow_solid skips that interior (F20). */
        gfx_shadow_solid(s, (float)cur->x, (float)cur->y, (float)cur->w, (float)cur->h, cx, 24, 0.5f);
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
        const float dsz = (float)dot_sz();
        gfx_rrect_aa(s, (float)(cur->x + dot_x(0)), (float)cur->y + dot_y, dsz, dsz, dsz * 0.5f,
                     0xFFFF5F57u, 0, 0);
        gfx_rrect_aa(s, (float)(cur->x + dot_x(1)), (float)cur->y + dot_y, dsz, dsz, dsz * 0.5f,
                     0xFFFEBC2Eu, 0, 0);
        gfx_rrect_aa(s, (float)(cur->x + dot_x(2)), (float)cur->y + dot_y, dsz, dsz, dsz * 0.5f,
                     0xFF28C840u, 0, 0);

        /* F10: a title wider than the bar was painted outside the window, over
         * the frame and the window behind it. Truncate to the bar.
         * F12: centre it on the bar from the real font height. */
        {
            char shown[96];
            const int tx0 = title_x();
            text_fit(f13, cur->title, shown, sizeof(shown),
                     (float)cur->w - (float)tx0 - (float)ui_px(9));
            font_draw(s, f13, (float)cur->x + tx0,
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
            Font *fm = font_get(FONT_MONO, cur->term_size > 0 ? cur->term_size : 12);
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
            Window *focused = win_focused();
            /* the magnify neighbourhood is a distance, so it scales too;
             * the Gaussian radius below is the same unit (44 design px) */
            int near_bar = (input.mouse_y > dy - ui_px(79) &&
                            input.mouse_y < dy + dh + ui_px(14));

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
                    float t = d / (float)ui_px(44);
                    scale = 1.0f + 0.45f * expf(-t * t * 0.5f);
                }
                int hovered = (input.mouse_x >= bx && input.mouse_x < bx + isz &&
                               input.mouse_y >= dy && input.mouse_y < dy + dh);
                int is_focus = (w == focused);
                /* The tile keeps the magnified size; the icon inside it is
                 * what the theme animates, so a pulse can only shrink and
                 * never leave the tile (icon_draw applies it, centred). */
                float sz = isz * scale;
                float ix = cx0 - sz * 0.5f;
                float iy = by + (isz - sz) * 0.55f;
                u32 bg = is_focus ? color_alpha(th->accent, 0.28f)
                                  : (hovered ? color_alpha(th->accent, 0.18f)
                                             : color_alpha(th->text, 0.07f));
                gfx_rrect_aa(s, ix, iy, sz, sz, 11.0f * scale, bg, 0, 0);

                IconFn fn = icon_for_window(w);
                /* Two colours in every state. Hidden windows are faded as a
                 * pair; an ordinary icon keeps `text` as the body; a focused
                 * or hovered one is drawn in the theme's two icon colours,
                 * with the dimmer as the body and the brighter as the
                 * highlight (theme_icon_pair), so the active icon is never a
                 * one-colour silhouette again. */
                u32 fg, hi;
                if (w->hidden) {
                    fg = color_alpha(th->text, 0.45f);
                    hi = color_alpha(th->icon_accent, 0.45f);
                } else {
                    IconPair pair = theme_icon_pair(th, th->text,
                                                    is_focus || hovered);
                    fg = pair.body;
                    hi = pair.hi;
                }
                icon_draw(fn, s, ix, iy, sz, fg, hi, th, now_sec(), hovered);

                if (is_focus)
                    gfx_rrect_aa(s, cx0 - 7, (float)(dy + dh) - 5, 14, 3, 1.5f, th->accent, 0, 0);

                if (hovered) {
                    /* The hover label is a text box, so it is measured from
                     * its own line: the old fixed 19 px pill held a 21.7 px
                     * line at the shipped size and overflowed it. */
                    Font *ft = font_get(FONT_MEDIUM, 11);
                    const int tiph = ui_tip_h();
                    const float tpad = (float)ui_px(5);
                    char shown[64];
                    text_fit(ft, w->title, shown, sizeof(shown),
                             (float)s->w - 2.0f * tpad - (float)ui_px(8));
                    float lw = font_width(ft, shown);
                    float tx = cx0 - lw / 2.0f - tpad;
                    float ty = (float)dy - (float)tiph - (float)ui_px(5);
                    if (tx < (float)ui_px(3)) tx = (float)ui_px(3);
                    if (tx + lw + 2.0f * tpad > (float)s->w - (float)ui_px(3))
                        tx = (float)s->w - lw - 2.0f * tpad - (float)ui_px(3);
                    if (ty < (float)topbar_h() + (float)ui_px(2))
                        ty = (float)topbar_h() + (float)ui_px(2);
                    gfx_rrect_aa(s, tx, ty, lw + 2.0f * tpad, (float)tiph,
                                 (float)tiph * 0.5f,
                                 color_alpha(th->window_bg, 0.96f), th->panel_border, 1.0f);
                    font_draw(s, ft, tx + tpad,
                              ty + ((float)tiph - font_height(ft)) * 0.5f,
                              shown, th->text);
                }
            }
        }
    }

    /* the status line sits above the windows and below the overlays: a
     * confirmation must not be hidden by the thing that caused it, but the
     * shortcut panel owns the middle of the screen while it is up */
    if (status_t0 > 0 && status_msg[0]) {
        Font *fs = font_get(FONT_MEDIUM, 12);
        char shown[192];
        float maxw = (float)s->w - (float)ui_px(60);
        text_fit(fs, status_msg, shown, sizeof(shown), maxw);
        float tw = font_width(fs, shown);
        int bw = (int)tw + ui_px(28);
        int bh = (int)font_height(fs) + ui_px(14);
        int bx = (s->w - bw) / 2;
        /* clear the dock by its real height, the same reserve a maximised
         * window uses: the old ui_px(78) was tuned to the shrunken bar and
         * let the pill overlap the dock at 1.6 */
        int bdx, bdy, bdw, bdh;
        int reserved = dock_rect(&bdx, &bdy, &bdw, &bdh)
                     ? (s->h - bdy + ui_px(6)) : ui_px(16);
        int by = s->h - reserved - bh;
        if (by < topbar_h() + ui_px(8)) by = topbar_h() + ui_px(8);
        gfx_shadow(s, (float)bx, (float)by, (float)bw, (float)bh, 9, 12, 0.45f);
        gfx_rrect_aa(s, (float)bx, (float)by, (float)bw, (float)bh, 9,
                     color_alpha(th->window_bg, 0.96f), th->panel_border, 1.0f);
        font_draw(s, fs, (float)bx + ui_px(14),
                  (float)by + ((float)bh - font_height(fs)) * 0.5f,
                  shown, th->text);
    }

    if (menu_open) draw_menu(s, th);
    if (launcher_open) {
        int total = launcher_items();
        if (launcher_sel >= total) launcher_sel = total > 0 ? total - 1 : 0;
        if (launcher_sel < 0) launcher_sel = 0;
        draw_launcher(s, th);
    }
    /* the shortcut panel is above every window, and the lock screen is
     * above everything - it is a security boundary, not a decoration */
    if (help_open) shortcuts_draw_panel(s, th, topbar_h(), help_page);
    if (locked) draw_lock_screen(s, th);
}

/* ------------------------------------------------------------ launcher
 * Super opens a grid of every app that exists. Tiles are the same icons
 * as the dock, so there is one place that defines what an app looks like.
 */
typedef struct {
    const char *app_id;      /* NULL = terminal */
    const char *label;
} LauncherItem;

static const LauncherItem LAUNCHER[] = {
    { NULL,       "Terminal" },
    { "files",    "Files" },
    { "settings", "Settings" },
    { "monitor",  "Monitor" },
    { "notes",    "Notes" },
    { "store",    "Apps" },
    { "calc",     "Calculator" },
    { "sysinfo",  "System" },
};
#define LAUNCHER_N ((int)(sizeof(LAUNCHER) / sizeof(LAUNCHER[0])))

static int launcher_items(void) { return LAUNCHER_N + bundle_count; }

/* The icon is a function of the app id and nothing else. The launcher used to
 * carry its own table, which is why the System tile drew a gear while the same
 * app's dock icon was a chip: two lists, two answers. The unknown-id fallback
 * is icon_gear here too, exactly what icon_for_id() gives the dock. */
static IconFn launcher_icon(int idx) {
    if (idx < 0 || idx >= LAUNCHER_N) return NULL;
    if (!LAUNCHER[idx].app_id) return icon_terminal;
    IconFn fn = icon_for_bundle(LAUNCHER[idx].app_id);
    return fn ? fn : icon_gear;
}

/* Every cell is at least as wide as the widest label it has to draw, measured
 * with the face the grid draws it in. */
static int launcher_label_w(void) {
    Font *f = font_get(FONT_MEDIUM, 12);
    float w = 0.0f;
    for (int i = 0; i < LAUNCHER_N; i++) {
        float tw = font_width(f, LAUNCHER[i].label);
        if (tw > w) w = tw;
    }
    for (int b = 0; b < bundle_count; b++) {
        float tw = font_width(f, bundles[b].name);
        if (tw > w) w = tw;
    }
    return (int)(w + 0.5f);
}

/* One letter stands in for a bundle this build has no drawn icon for. Pick
 * the largest nominal size whose REAL line fits the box: at 1.6 a 26 pt
 * initial is 81 px tall and would paint outside its 63 px tile. */
static Font *initial_font(int box) {
    int pt = 26;
    while (pt > 6 && ui_line_h(FONT_SEMIBOLD, (float)pt) > box - 2 * ui_px(2))
        pt -= 2;
    return font_get(FONT_SEMIBOLD, (float)pt);
}

/* One grid for the panel, the tiles and the keyboard: ui.c owns the column
 * count (it depends on how many apps there are and how tall the screen is),
 * so a draw, a click and a Right-arrow cannot disagree about which cell is
 * which. */
static void launcher_geom(int idx, int *tx, int *ty, int *tw, int *th) {
    ui_launcher_geom(launcher_items(), screen.w, screen.h, launcher_label_w(),
                     idx, tx, ty, tw, th);
}

/* True while a theme-animated item is active: a hovered dock icon, a hovered
 * launcher tile, or the launcher's keyboard selection. Those are the only
 * three conditions anim_gate() accepts. A still desktop never asks for a frame
 * because of the pulse - and with anim_speed = 0 the gate closes at its first
 * test. The eye is not an item: hovering it asks for nothing, so an idle
 * Aurora or Nebula desktop repaints zero times for icon animation. The eye's
 * blink has its own schedule, set by a press, further up. */
static int anim_item_under_pointer(void) {
    const Theme *th = &themes[theme_index];
    int dock_item = 0, tile = 0, selected_tile = 0;
    if (launcher_open) {
        /* The selected tile is a launcher item even when the pointer is not
         * over it: keyboard navigation still marks it active, so it has to
         * breathe (and ask for frames) while the launcher is up. */
        int total = launcher_items();
        selected_tile = (launcher_sel >= 0 && launcher_sel < total);
        for (int i = 0; i < total && !tile; i++) {
            int tx, ty, tw, th2;
            launcher_geom(i, &tx, &ty, &tw, &th2);
            if (input.mouse_x >= tx && input.mouse_x < tx + tw &&
                input.mouse_y >= ty && input.mouse_y < ty + th2)
                tile = 1;
        }
    } else {
        int dx, dy, dw, dh;
        if (dock_rect(&dx, &dy, &dw, &dh)) {
            int isz = dock_isz();
            for (int i = 0; i < taskbar_count() && !dock_item; i++) {
                float bx = (float)(dx + dock_pad() + i * (isz + dock_gap()));
                if (input.mouse_x >= bx && input.mouse_x < bx + isz &&
                    input.mouse_y >= dy && input.mouse_y < dy + dh)
                    dock_item = 1;
            }
        }
    }
    return anim_gate(th->anim, th->anim_speed, th->anim_amp,
                     dock_item, tile, selected_tile);
}

static void draw_launcher(Surface *s, const Theme *th) {
    /* dim everything behind */
    surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u);

    const int total = launcher_items();
    const int label_w = launcher_label_w();
    int px, py, pw, ph;
    ui_launcher_panel(total, s->w, s->h, label_w, &px, &py, &pw, &ph);

    gfx_shadow(s, (float)px, (float)py, (float)pw, (float)ph, 22, 30, 0.55f);
    /* The panel body is opaque, so blurring the wallpaper underneath it was
     * invisible work - and it ran on every pointer move over the grid, which
     * is exactly the lag the owner reported on the Apps screen. */
    gfx_rrect_aa(s, (float)px, (float)py, (float)pw, (float)ph, 22,
                 color_alpha(th->window_bg, 0.97f), th->window_border, 1.0f);

    Font *f12 = font_get(FONT_MEDIUM, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    const float pad = (float)ui_px(8);
    const float label_h = font_height(f12);

    for (int i = 0; i < total; i++) {
        int tx, ty, tw, th2;
        launcher_geom(i, &tx, &ty, &tw, &th2);
        int hovered = (input.mouse_x >= tx && input.mouse_x < tx + tw &&
                       input.mouse_y >= ty && input.mouse_y < ty + th2);
        int selected = launcher_open && (i == launcher_sel);
        int active = hovered || selected;
        u32 bg = (hovered || selected) ? color_alpha(th->accent, selected ? 0.30f : 0.22f)
                                       : color_alpha(th->text, 0.06f);
        gfx_rrect_aa(s, (float)tx, (float)ty, (float)tw, (float)th2, 14.0f, bg,
                     selected ? th->accent : 0, selected ? 1.5f : 0.0f);

        /* The label owns the bottom line of the cell; the icon gets what is
         * left above it and shrinks to fit that, so a cell can always draw
         * both at every scale. */
        float label_top = (float)ty + (float)th2 - pad - label_h;
        float icon_room = label_top - (float)ty - 2.0f * pad;
        float isz = icon_room;
        if (isz > (float)ui_px(52)) isz = (float)ui_px(52);
        if (isz < 12.0f) isz = 12.0f;
        float ix = (float)tx + ((float)tw - isz) * 0.5f;
        float iy = (float)ty + pad + (icon_room - isz) * 0.5f;
        u32 fg = active ? th->accent : th->text;
        /* active tiles take the theme's two icon colours with the roles set by
         * luminance, so the body and the highlight are always different */
        IconPair pair = theme_icon_pair(th, th->text, active);
        u32 icon_body = pair.body, hi = pair.hi;
        const char *label;
        if (i < LAUNCHER_N) {
            label = LAUNCHER[i].label;
            IconFn fn = launcher_icon(i);
            icon_draw(fn, s, ix, iy, isz, icon_body, hi, th, now_sec(), active);
        } else {
            /* an installed bundle: a real program from /usr/share/mystic/apps */
            int b = i - LAUNCHER_N;
            label = (b >= 0 && b < bundle_count) ? bundles[b].name : "?";
            /* an installed app gets its own drawn icon; the initial is only
             * the fallback for a bundle this build does not know */
            u32 tile_bg = active ? color_alpha(th->accent, 0.34f)
                                 : color_alpha(th->text, 0.10f);
            gfx_rrect_aa(s, ix, iy, isz, isz, 12.0f, tile_bg, 0, 0);
            IconFn fn = (b >= 0 && b < bundle_count) ? icon_for_bundle(bundles[b].id) : NULL;
            if (fn) {
                icon_draw(fn, s, ix + isz * 0.16f, iy + isz * 0.16f,
                          isz * 0.68f, icon_body, hi, th, now_sec(), active);
            } else {
                char initial[2] = { (char)(label && label[0] ? label[0] : '?'), 0 };
                Font *fi = initial_font((int)isz);
                float iw2 = font_width(fi, initial);
                font_draw(s, fi, ix + (isz - iw2) * 0.5f,
                          iy + (isz - font_height(fi)) * 0.5f, initial, fg);
            }
        }

        /* fit the label to its cell: a bundle may be named anything, and a
         * name wider than the tile used to run into the neighbouring cell */
        char shown[64];
        text_fit(f12, label, shown, sizeof(shown), (float)tw - 2.0f * ui_px(4));
        float lw = font_width(f12, shown);
        font_draw(s, f12, (float)tx + ((float)tw - lw) * 0.5f, label_top, shown,
                  hovered ? th->text : th->text_dim);
    }

    float hw = font_width(fd, "Super or Esc closes");
    font_draw(s, fd, (float)(s->w - hw) / 2, (float)(py + ph + ui_px(7)),
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
    font_draw(s, fh, (float)px + (pw - hw) / 2, (float)py + ui_px(19), clock, th->text);

    float lw = font_width(fr, "Mystic OS is locked");
    font_draw(s, fr, (float)px + (pw - lw) / 2, (float)py + ui_px(41), "Mystic OS is locked",
              th->text_dim);

    /* password field, one dot per character; the field is a line plus air,
     * and its contents are placed from the same inset scale as the panel */
    int fw = pw - ui_px(57), fh2 = ui_line_h(FONT_MEDIUM, 12) + ui_px(8);
    int fx = px + ui_px(29), fy = py + ui_px(66);
    gfx_rrect_aa(s, (float)fx, (float)fy, (float)fw, (float)fh2, 9,
                 color_alpha(th->text, 0.08f), th->window_border, 1.0f);
    float dx = (float)fx + (float)ui_px(10);
    for (int i = 0; i < pin_len && i < 24; i++) {
        float dsz = (float)ui_px(6);
        gfx_rrect_aa(s, dx, (float)fy + ((float)fh2 - dsz) * 0.5f, dsz, dsz, dsz * 0.5f,
                     th->text, 0, 0);
        dx += (float)ui_px(10);
    }

    if (lock_msg[0]) {
        float mw = font_width(fd, lock_msg);
        font_draw(s, fd, (float)px + (pw - mw) / 2, (float)py + ui_px(101), lock_msg, th->accent);
    }
    font_draw(s, fd, (float)px + ui_px(29),
              (float)(py + ph) - (float)ui_px(11) - font_height(fd),
              "Enter to unlock", color_alpha(th->text, 0.45f));
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
        case SC_KEY_ESC:
            /* Esc is a cancel key, but an app that owns its own modal needs
             * the byte: cancel_action() forwards it through here */
            out[(*n)++] = 0x1B; return;
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
        else if (w == win_focused()) { w->hidden = 1; }   /* put away */
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
                     ? (screen.h - bdy + ui_px(12)) : ui_px(16);
        w->x = 0; w->y = top_;
        w->w = screen.w; w->h = screen.h - top_ - reserved;
        w->maxed = 1;
    } else {
        w->x = w->restore_x; w->y = w->restore_y;
        w->w = w->restore_w; w->h = w->restore_h;
        w->maxed = 0;
    }
    window_resized(w);
}

/* ============================================================
 *  Shortcut actions
 *
 *  src/shortcuts.c owns WHICH keys are bound; this owns what they do,
 *  because the windows, the theme, the wallpaper and the framebuffer all
 *  live in this file. Every case is a real operation: no action writes a
 *  placeholder, and a value that is not read back from the system is not
 *  invented here.
 * ============================================================ */

/* The area a maximised or snapped window may use: everything under the top
 * bar and above the dock. Same reserve as win_toggle_max(), so a snapped
 * window and a maximised one cannot disagree about where the bottom is. */
static void desk_area(int *x, int *y, int *w, int *h) {
    int bdx, bdy, bdw, bdh;
    int reserved = dock_rect(&bdx, &bdy, &bdw, &bdh)
                 ? (screen.h - bdy + ui_px(12)) : ui_px(16);
    int top = topbar_h();
    *x = 0; *y = top; *w = screen.w; *h = screen.h - top - reserved;
    if (*h < ui_px(160)) *h = screen.h - top;
}

/* ------------------------------------------------------------ status
 *
 * One line above the dock, for things whose only evidence is that they
 * happened: "Closed 3 windows", "Cancelled move", "Always on top on".
 * The text is not a notification (src/notify.c owns those) and does not
 * need acknowledging; the tick below clears it after a few seconds.
 */
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
    va_end(ap);
    status_t0 = now_sec();
    dirty = 1;
    fprintf(stderr, "status: %s\n", status_msg);
}

/* Which desktop overlays are up. This is what makes the keyboard modal
 * and what tells Esc what to dismiss. SC_MODAL_APP is deliberately not
 * here: a merely focused window must not stop ordinary shortcuts. */
static unsigned current_modals(void) {
    unsigned m = 0;
    if (help_open)          m |= SC_MODAL_HELP;
    if (launcher_open)      m |= SC_MODAL_LAUNCHER;
    if (menu_open)          m |= SC_MODAL_MENU;
    if (drag_win >= 0)      m |= SC_MODAL_DRAG;
    if (resize_win >= 0)    m |= SC_MODAL_RESIZE;
    return m;
}

/* The overlays that own the keyboard. A drag or a resize is a mouse capture:
 * it gives Esc something to cancel, but it does not stop Ctrl+W from
 * closing the window being dragged. */
static unsigned keyboard_modals(void) {
    return current_modals() &
           (SC_MODAL_HELP | SC_MODAL_LAUNCHER | SC_MODAL_MENU);
}

/* Esc reaches the focused window when nothing on the desktop is open: a
 * terminal needs it to leave insert mode, an app may have an inline edit,
 * a menu or a text selection of its own. This is the one place the cancel
 * chain hands the key on; it never closes anything. */
static void esc_to_window(Window *w) {
    if (!w || !w->used) return;
    if (w->term) {
        term_key(w->term, "\x1b", 1);
        dirty = 1;
        return;
    }
    if (w->kind == WIN_CLIENT && w->client >= 0 && w->client < WM_MAX_CLIENTS &&
        clients[w->client].used) {
        int mods = (input.shift ? 1 : 0) | (input.ctrl ? 2 : 0) | (input.alt ? 4 : 0);
        wms_notify_key(&clients[w->client], SC_KEY_ESC, mods, 1);
        dirty = 1;
        return;
    }
    if (w->app) {
        app_key(w->app, "\x1b", 1);
        dirty = 1;
    }
}

static void cancel_drag(void) {
    if (drag_win >= 0 && wins[drag_win].used && wins[drag_win].gen == drag_gen) {
        Window *w = &wins[drag_win];
        w->x = drag_ox;
        w->y = drag_oy;
    }
    drag_win = -1;
    dirty = 1;
}

static void cancel_resize(void) {
    if (resize_win >= 0 && wins[resize_win].used && wins[resize_win].gen == resize_gen) {
        Window *w = &wins[resize_win];
        w->w = resize_ow;
        w->h = resize_oh;
        window_resized(w);
    }
    resize_win = -1;
    dirty = 1;
}

/* Esc. The decision of WHAT to cancel is shortcuts_cancel_target(), which
 * the host test walks for every state; this is only the effect. There is
 * no window-close case here, by design: Esc cannot close a window. */
static void cancel_action(void) {
    unsigned mask = current_modals();
    if (win_focused()) mask |= SC_MODAL_APP;
    int target = shortcuts_cancel_target(mask);

    switch (target) {
    case SC_MODAL_HELP:
        help_open = 0;
        help_page = 0;
        break;
    case SC_MODAL_LAUNCHER:
        launcher_open = 0;
        break;
    case SC_MODAL_MENU:
        menu_open = 0;
        break;
    case SC_MODAL_DRAG:
        cancel_drag();
        break;
    case SC_MODAL_RESIZE:
        cancel_resize();
        break;
    case SC_MODAL_APP:
        /* an app's own modal, selection or inline edit owns the Escape */
        esc_to_window(win_focused());
        return;
    default:
        return;                     /* nothing is open: nothing happens */
    }
    set_status("Cancelled %s", shortcuts_cancel_name(target));
    dirty = 1;
}

/* ------------------------------------------------------------ window ops */

static void win_min_all(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used || wins[i].hidden) continue;
        wins[i].hidden = 1;
        n++;
    }
    Window *nx = win_top();
    if (nx) win_focus(nx);
    set_status("Minimised %d window%s", n, n == 1 ? "" : "s");
    dirty = 1;
}

static void win_restore_all(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used || !wins[i].hidden) continue;
        wins[i].hidden = 0;
        n++;
    }
    if (n) {
        Window *nx = win_top();
        if (nx) win_focus(nx);
    }
    set_status("Restored %d window%s", n, n == 1 ? "" : "s");
    dirty = 1;
}

static void win_close_all(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        win_close(&wins[i]);
        n++;
    }
    set_status("Closed %d window%s", n, n == 1 ? "" : "s");
    dirty = 1;
}

/* Super+1..9 follows the dock, because that is the order the owner sees:
 * the first icon is window 1. `idx` is 0-based. */
static void focus_window_n(int idx) {
    Window *w = taskbar_at(idx);
    if (!w) {
        set_status("No window %d", idx + 1);
        return;
    }
    if (w->hidden) w->hidden = 0;
    win_focus(w);
    dirty = 1;
}

/* Super+Shift+arrows: half the desk on each side. Unlike win_snap() this
 * also fills the top and bottom halves, which is what "edge" means. */
static void win_edge(Window *w, int key) {
    if (!w) return;
    if (!w->maxed) {
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
    }
    int x, y, ww, hh;
    desk_area(&x, &y, &ww, &hh);
    w->maxed = 3;
    if (key == SC_KEY_LEFT) {
        w->x = x; w->y = y; w->w = ww / 2; w->h = hh;
    } else if (key == SC_KEY_RIGHT) {
        w->w = ww / 2; w->x = x + ww - w->w;
        w->y = y; w->h = hh;
    } else if (key == SC_KEY_UP) {
        w->x = x; w->y = y; w->w = ww; w->h = hh / 2;
    } else {
        w->x = x; w->w = ww; w->h = hh / 2; w->y = y + hh - w->h;
    }
    window_resized(w);
    dirty = 1;
}

static void win_grow(Window *w, int dir) {
    if (!w) return;
    if (w->maxed) win_toggle_max(w);   /* grow the size it would restore to */
    int step = ui_px(24);
    int nw = w->w + dir * step * 2;
    int nh = w->h + dir * step;
    if (nw < ui_px(220)) nw = ui_px(220);
    if (nh < ui_px(150)) nh = ui_px(150);
    if (w->x + nw > screen.w) nw = screen.w - w->x;
    if (w->y + nh > screen.h) nh = screen.h - w->y;
    if (nw < ui_px(120)) nw = ui_px(120);
    if (nh < ui_px(100)) nh = ui_px(100);
    if (nw == w->w && nh == w->h) {
        set_status("Window is at its limit");
        return;
    }
    w->w = nw;
    w->h = nh;
    window_resized(w);
    dirty = 1;
}

static void win_centre(Window *w) {
    if (!w) return;
    if (w->maxed) win_toggle_max(w);
    int x, y, ww, hh;
    desk_area(&x, &y, &ww, &hh);
    w->x = x + (ww - w->w) / 2;
    w->y = y + (hh - w->h) / 2;
    if (w->x < 0) w->x = 0;
    if (w->y < topbar_h()) w->y = topbar_h();
    dirty = 1;
}

/* Super+R: the focused window goes to the front, or to the back when it is
 * already in front. Always-on-top windows keep their layer (see win_rank). */
static void win_raise_lower(Window *w) {
    if (!w) return;
    int zmin = 0, any = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        if (!any || wins[i].z < zmin) { zmin = wins[i].z; any = 1; }
    }
    if (w == win_top()) {
        w->z = zmin - 1;
        set_status("Lowered %s", w->title);
    } else {
        w->z = ++z_top;
        set_status("Raised %s", w->title);
    }
    dirty = 1;
}

static void win_toggle_ontop(Window *w) {
    if (!w) return;
    w->on_top = !w->on_top;
    if (w->on_top) w->z = ++z_top;
    set_status("Always on top %s: %s", w->on_top ? "on" : "off", w->title);
    dirty = 1;
}

/* Alt+Print: only the focused window's rectangle, cut from the composed
 * scene. Same P6 writer as take_screenshot(), but the header carries the
 * window's size, not the screen's. */
static int shot_next;              /* first screenshot number to try */
static void take_screenshot_window(void) {
    Window *w = win_focused();
    if (!w) { set_status("No window to capture"); return; }
    if (mkdir("/root/screenshots", 0755) != 0 && errno != EEXIST) {
        set_status("Screenshot: %s", strerror(errno));
        return;
    }
    char path[96];
    int slot = -1;
    for (int n = shot_next; n < 10000; n++) {
        struct stat st;
        snprintf(path, sizeof(path), "/root/screenshots/window-%04d.ppm", n);
        if (stat(path, &st) != 0) { slot = n; break; }
    }
    if (slot < 0) { set_status("Screenshot: no free name"); return; }
    shot_next = slot + 1;

    int x0 = w->x, y0 = w->y, ww = w->w, hh = w->h;
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hh += y0; y0 = 0; }
    if (x0 + ww > scene.w) ww = scene.w - x0;
    if (y0 + hh > scene.h) hh = scene.h - y0;
    if (ww <= 0 || hh <= 0) { set_status("Screenshot: window is off screen"); return; }

    FILE *f = fopen(path, "wb");
    if (!f) { set_status("Screenshot: %s", strerror(errno)); return; }
    unsigned char *row = malloc((size_t)ww * 3u);
    if (!row) { fclose(f); remove(path); set_status("Screenshot: out of memory"); return; }
    int ok = fprintf(f, "P6\n%d %d\n255\n", ww, hh) > 0;
    for (int y = 0; y < hh && ok; y++) {
        const u32 *src = scene.px + (size_t)(y0 + y) * (size_t)scene.stride + x0;
        for (int x = 0; x < ww; x++) {
            row[x * 3 + 0] = (unsigned char)((src[x] >> 16) & 0xFF);
            row[x * 3 + 1] = (unsigned char)((src[x] >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)(src[x] & 0xFF);
        }
        if (fwrite(row, 3, (size_t)ww, f) != (size_t)ww) ok = 0;
    }
    free(row);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(path);
        set_status("Screenshot failed");
        return;
    }
    set_status("Screenshot %s", path);
}

static void win_fullscreen(Window *w) {
    if (!w) return;
    if (w->maxed == 2) {            /* already full screen: give the size back */
        win_toggle_max(w);
        return;
    }
    if (!w->maxed) {                /* keep the windowed rect to return to */
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
    }
    w->x = 0; w->y = 0; w->w = screen.w; w->h = screen.h;
    w->maxed = 2;
    window_resized(w);
    dirty = 1;
}

static void win_restore_action(Window *w) {
    if (w && w->maxed) { win_toggle_max(w); return; }
    /* nothing maximised: bring back the most recently focused window that
     * was put away, which is what "restore" means when one is minimised */
    Window *back = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *o = &wins[i];
        if (!o->used || !o->hidden) continue;
        if (!back || win_rank(o) > win_rank(back)) back = o;
    }
    if (back) { back->hidden = 0; win_focus(back); dirty = 1; }
}

static void win_snap(Window *w, int dir) {
    if (!w) return;
    if (dir == 0) {                 /* Super+Up: maximise, or give the size back */
        if (w->maxed == 3) w->maxed = 0;
        win_toggle_max(w);
        dirty = 1;
        return;
    }
    if (!w->maxed) {                /* remember the windowed rect */
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
    }
    int x, y, ww, hh;
    desk_area(&x, &y, &ww, &hh);
    w->maxed = 3;                   /* snapped: win_toggle_max restores it */
    w->y = y; w->h = hh;
    if (dir < 0)      { w->x = x; w->w = ww / 2; }
    else              { w->w = ww / 2; w->x = x + ww - w->w; }
    window_resized(w);
    dirty = 1;
}

static void win_move_by(Window *w, int key) {
    if (!w) return;
    int step = ui_px(20);
    if (key == SC_KEY_LEFT)  w->x -= step;
    if (key == SC_KEY_RIGHT) w->x += step;
    if (key == SC_KEY_UP)    w->y -= step;
    if (key == SC_KEY_DOWN)  w->y += step;
    /* never push the title bar where it cannot be grabbed again */
    if (w->y < topbar_h()) w->y = topbar_h();
    if (w->y > screen.h - ui_px(40)) w->y = screen.h - ui_px(40);
    if (w->x + w->w < ui_px(80)) w->x = ui_px(80) - w->w;
    if (w->x > screen.w - ui_px(80)) w->x = screen.w - ui_px(80);
    dirty = 1;
}

static void win_resize_by(Window *w, int key) {
    if (!w) return;
    int step = ui_px(20);
    if (key == SC_KEY_LEFT)  w->w -= step;
    if (key == SC_KEY_RIGHT) w->w += step;
    if (key == SC_KEY_UP)    w->h -= step;
    if (key == SC_KEY_DOWN)  w->h += step;
    if (w->w < ui_px(220)) w->w = ui_px(220);
    if (w->h < ui_px(150)) w->h = ui_px(150);
    if (w->x + w->w > screen.w) w->w = screen.w - w->x;
    if (w->y + w->h > screen.h) w->h = screen.h - w->y;
    w->maxed = 0;                   /* a resized window is a window again */
    window_resized(w);
    dirty = 1;
}

/* Alt+Tab walks the z order: "next" is the window that was focused before
 * this one (what Alt+Tab always did here), "previous" starts from the other
 * end, so the two directions are not the same key. */
static void focus_cycle(int dir) {
    Window *cur = win_focused(), *pick = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (!w->used || w->hidden || w == cur) continue;
        if (!pick) { pick = w; continue; }
        if (dir > 0) { if (win_rank(w) > win_rank(pick)) pick = w; }
        else         { if (win_rank(w) < win_rank(pick)) pick = w; }
    }
    if (pick) { win_focus(pick); dirty = 1; }
}

/* A window's identity, for "cycle this app's windows": the bundle id for an
 * external app, the app id for a built-in one, "terminal" for a shell. */
static const char *win_identity(const Window *w) {
    if (w->kind == WIN_TERM) return "terminal";
    if (w->kind == WIN_CLIENT && w->client >= 0 && w->client < WM_MAX_CLIENTS &&
        clients[w->client].used)
        return clients[w->client].id;
    if (w->app && w->app->vt && w->app->vt->id) return w->app->vt->id;
    return "";
}

static void focus_same_app(void) {
    Window *cur = win_focused();
    if (!cur) return;
    const char *id = win_identity(cur);
    if (!id[0]) return;
    Window *below = NULL, *top = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (!w->used || w->hidden || w == cur) continue;
        if (strcmp(win_identity(w), id) != 0) continue;
        if (!top || win_rank(w) > win_rank(top)) top = w;
        if (win_rank(w) < win_rank(cur) &&
            (!below || win_rank(w) > win_rank(below))) below = w;
    }
    Window *pick = below ? below : top;      /* wrap round to the newest */
    if (pick) { win_focus(pick); dirty = 1; }
}

static void launch_bundle(const char *id) {
    for (int b = 0; b < bundle_count; b++) {
        if (strcmp(bundles[b].id, id) != 0) continue;
        int pid = wms_launch(&bundles[b], clients, b);
        fprintf(stderr, "wms: shortcut launch %s pid %d\n", id, pid);
        return;
    }
    fprintf(stderr, "shortcuts: no installed bundle '%s'\n", id);
}

/* Theme and wallpaper writes use the same files and the same format the
 * Settings app uses, so the desktop's once-a-second re-read and the app
 * cannot disagree about what is on screen. */
static void theme_runtime_apply(void);   /* fonts + text scale, defined below */

static void theme_apply(int idx) {
    if (idx < 0 || idx >= theme_count) return;
    theme_index = idx;
    theme_runtime_apply();
    FILE *f = fopen("/etc/mystic/current-theme", "w");
    if (f) { fprintf(f, "%s\n", themes[idx].name); fclose(f); }
    persist_settings();
    set_status("Theme: %s", themes[idx].name);
    dirty = 1;
}

static void wallpaper_apply(int idx) {
    if (idx < 0 || idx >= WALLPAPER_COUNT) return;
    wall_index = idx;
    const char *name = wallpaper_name(idx);
    if (name) {
        FILE *f = fopen("/etc/mystic/wallpaper", "w");
        if (f) { fprintf(f, "%s\n", name); fclose(f); }
        persist_settings();
        set_status("Wallpaper: %s", name);
    }
    wall_t0 = now_sec();
    dirty = 1;
}

/* ------------------------------------------------------------ terminal
 *
 * The terminal chords act on the terminal wherever it is in the stack:
 * they are chords for the terminal, and the focused window may well be
 * the browser whose page you are copying from.
 */
static Window *term_window(void) {
    Window *best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (!w->used || w->hidden || !w->term) continue;
        if (!best || win_rank(w) > win_rank(best)) best = w;
    }
    return best;
}

/* What the user has typed since the last Enter, kept so Ctrl+Shift+C has a
 * real selection-less copy to offer: the line itself. term.c owns the
 * grid and exposes no text accessor, so copying screen text would mean
 * inventing it here. */
static void term_note_input(Window *w, const char *bytes, int n) {
    if (!w) return;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '\r' || c == '\n') {
            w->input_len = 0;
            w->input_line[0] = 0;
            continue;
        }
        if (c == 0x7f || c == '\b') {
            if (w->input_len > 0) w->input_line[--w->input_len] = 0;
            continue;
        }
        if (c == 0x1b) continue;            /* arrows and function keys */
        if (c >= 0x20 && c < 0x7f &&
            w->input_len < (int)sizeof(w->input_line) - 1) {
            w->input_line[w->input_len++] = (char)c;
            w->input_line[w->input_len] = 0;
        }
    }
}

static void term_clear(void) {
    Window *w = term_window();
    if (!w) { set_status("No terminal open"); return; }
    /* the real Ctrl+L byte: the shell clears its screen and redraws the
     * prompt, so the terminal and the shell agree about what is on it */
    term_key(w->term, "\x0c", 1);
    w->input_len = 0;
    w->input_line[0] = 0;
    set_status("Terminal: cleared");
    dirty = 1;
}

static void term_copy(void) {
    Window *w = term_window();
    if (!w) { set_status("No terminal open"); return; }
    if (w->input_len <= 0) {
        set_status("Terminal: nothing typed to copy");
        return;
    }
    int rc = clip_set_text(w->input_line);
    if (rc == CLIP_OK)
        set_status("Copied terminal line (%d bytes)", w->input_len);
    else
        set_status("Clipboard: %s", clip_strerror(rc));
}

static void term_paste(void) {
    Window *w = term_window();
    if (!w) { set_status("No terminal open"); return; }
    ClipEntry *e = clip_get_current(CLIP_TYPE_TEXT);
    if (!e) {
        set_status("Clipboard: no text to paste");
        return;
    }
    if (e->type == CLIP_TYPE_TEXT && e->text && e->text_len) {
        term_scroll(w->term, -term_scroll_offset(w->term));
        term_key(w->term, e->text, (int)e->text_len);
        term_note_input(w, e->text, (int)e->text_len);
        set_status("Pasted %u bytes into the terminal", (unsigned)e->text_len);
        dirty = 1;
    } else {
        set_status("Clipboard: no text to paste");
    }
    clip_entry_free(e);
}

static void term_font(int dir) {
    Window *w = term_window();
    if (!w) { set_status("No terminal open"); return; }
    int size = w->term_size > 0 ? w->term_size : 12;
    size += dir;
    if (size < 8) size = 8;
    if (size > 28) size = 28;
    if (size == w->term_size) { set_status("Terminal font at its limit"); return; }
    w->term_size = size;
    window_resized(w);                   /* pty reflows on the next paint */
    set_status("Terminal font %d", size);
    dirty = 1;
}

/* ------------------------------------------------------------ browser */

static Window *browser_window(void) {
    Window *best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (!w->used || w->hidden) continue;
        if (w->kind != WIN_CLIENT || w->client < 0 || w->client >= WM_MAX_CLIENTS)
            continue;
        if (!clients[w->client].used) continue;
        if (strcmp(clients[w->client].id, "web") != 0) continue;
        if (!best || win_rank(w) > win_rank(best)) best = w;
    }
    return best;
}

static void browser_reload(void) {
    Window *w = browser_window();
    if (!w) { set_status("Browser is not open - Super+B starts it"); return; }
    WmClient *cl = &clients[w->client];
    wms_notify_key(cl, SC_KEY_R, 2, 1);   /* mods bit 1 = Ctrl */
    wms_notify_key(cl, SC_KEY_R, 2, 0);
    set_status("Browser: reload");
    dirty = 1;
}

static void browser_addr(void) {
    Window *w = browser_window();
    if (!w) { set_status("Browser is not open - Super+B starts it"); return; }
    /* web.c documents Tab as "focus / leave the address bar"; the window
     * server has no richer way to ask for the field itself */
    wms_notify_text(&clients[w->client], "\t");
    set_status("Browser: address bar");
    dirty = 1;
}

/* ============================================================
 *  Desktop context menu
 *
 *  Right click on the wallpaper. Small, real and modal: it makes the
 *  "an open menu" case of the Esc cancel chain an actual thing rather
 *  than a branch waiting for a menu that does not exist.
 * ============================================================ */
enum {
    MENU_TERM = 0,
    MENU_FILES,
    MENU_SETTINGS,
    MENU_SHORTCUTS,
    MENU_SEP,
    MENU_WALL,
    MENU_THEME,
    MENU_LOCK,
    MENU_COUNT
};

static const char *const MENU_LABEL[MENU_COUNT] = {
    "Open Terminal",
    "Open Files",
    "Open Settings",
    "Keyboard shortcuts",
    "",                       /* separator */
    "Next wallpaper",
    "Next theme",
    "Lock screen"
};

static int menu_item_h(void) {
    Font *f = font_get(FONT_REGULAR, 12);
    return (int)(font_height(f) + (float)ui_px(10));
}

static int menu_sep_h(void) { return ui_px(9); }
static int menu_pad(void) { return ui_px(10); }
static int menu_side(void) { return ui_px(14); }

static int menu_width(void) {
    Font *f = font_get(FONT_MEDIUM, 12);
    float w = 0.0f;
    for (int i = 0; i < MENU_COUNT; i++) {
        float tw = font_width(f, MENU_LABEL[i]);
        if (tw > w) w = tw;
    }
    return (int)w + 2 * menu_side();
}

static int menu_height(void) {
    int h = 2 * menu_pad();
    for (int i = 0; i < MENU_COUNT; i++)
        h += (i == MENU_SEP) ? menu_sep_h() : menu_item_h();
    return h;
}

/* The menu's top-left, clamped so it never runs off the screen. */
static void menu_origin(int *x, int *y) {
    int w = menu_width(), h = menu_height();
    *x = menu_x;
    *y = menu_y;
    if (*x + w > screen.w - 4) *x = screen.w - w - 4;
    if (*y + h > screen.h - 4) *y = screen.h - h - 4;
    if (*x < 4) *x = 4;
    if (*y < topbar_h()) *y = topbar_h();
}

static int menu_item_at(int mx, int my) {
    int x, y;
    menu_origin(&x, &y);
    int cy = y + menu_pad();
    int w = menu_width();
    for (int i = 0; i < MENU_COUNT; i++) {
        int h = (i == MENU_SEP) ? menu_sep_h() : menu_item_h();
        if (i != MENU_SEP && mx >= x && mx < x + w && my >= cy && my < cy + h)
            return i;
        cy += h;
    }
    return -1;
}

static void menu_run(int item) {
    switch (item) {
    case MENU_TERM:       win_open(WIN_TERM, NULL); break;
    case MENU_FILES:      win_open(WIN_APP, "files"); break;
    case MENU_SETTINGS:   win_open(WIN_APP, "settings"); break;
    case MENU_SHORTCUTS:  help_open = 1; help_page = 0; break;
    case MENU_WALL:       wallpaper_apply((wall_index + 1) % WALLPAPER_COUNT); break;
    case MENU_THEME:
        if (theme_count > 0) theme_apply((theme_index + 1) % theme_count);
        break;
    case MENU_LOCK:
        locked = 1;
        pin_len = 0;
        pin[0] = 0;
        lock_msg[0] = 0;
        break;
    default: break;
    }
    menu_open = 0;
    dirty = 1;
}

static void draw_menu(Surface *s, const Theme *th) {
    if (!menu_open) return;
    Font *f = font_get(FONT_MEDIUM, 12);
    int x, y;
    menu_origin(&x, &y);
    int w = menu_width(), h = menu_height();

    gfx_shadow(s, (float)x, (float)y, (float)w, (float)h, 12, 16, 0.55f);
    if (th->blur > 0) gfx_blur_region(s, x, y, w, h, (int)(12 * th->blur));
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, 10,
                 color_alpha(th->window_bg, 0.98f), th->window_border, 1.0f);

    int cy = y + menu_pad();
    int hover = menu_item_at(input.mouse_x, input.mouse_y);
    for (int i = 0; i < MENU_COUNT; i++) {
        if (i == MENU_SEP) {
            surf_blend(s, x + menu_side(), cy + menu_sep_h() / 2,
                       w - 2 * menu_side(), 1, color_alpha(th->panel_border, 0.9f));
            cy += menu_sep_h();
            continue;
        }
        int ih = menu_item_h();
        if (i == menu_sel || i == hover)
            gfx_rrect_aa(s, (float)(x + ui_px(4)), (float)cy,
                         (float)(w - ui_px(8)), (float)ih, 6,
                         color_alpha(th->accent, i == menu_sel ? 0.30f : 0.16f), 0, 0);
        font_draw(s, f, (float)(x + menu_side()),
                  (float)cy + ((float)ih - font_height(f)) * 0.5f,
                  MENU_LABEL[i], th->text);
        cy += ih;
    }
}

static int menu_click(int mx, int my, int buttons) {
    if (!menu_open) return 0;
    if (buttons & 1) {
        int item = menu_item_at(mx, my);
        if (item >= 0) { menu_run(item); return 1; }
    }
    menu_open = 0;
    dirty = 1;
    return 1;
}

static int menu_key(int k) {
    if (!menu_open) return 0;
    if (k == SC_KEY_DOWN) {
        do { menu_sel = (menu_sel + 1) % MENU_COUNT; } while (menu_sel == MENU_SEP);
        dirty = 1;
        return 1;
    }
    if (k == SC_KEY_UP) {
        do { menu_sel = (menu_sel + MENU_COUNT - 1) % MENU_COUNT; } while (menu_sel == MENU_SEP);
        dirty = 1;
        return 1;
    }
    if (k == SC_KEY_ENTER) {
        menu_run(menu_sel);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------- theme fonts and scale
 *
 * The theme owns two font roles and a text-scale multiplier, but the user
 * owns the last word: /etc/mystic/fonts.conf names a role and wins over the
 * theme for that role. The file is re-read once a second like ui.conf, so a
 * Settings click (font_set_role_family + font_save_config, see the report)
 * changes the desktop without a restart.
 */
#define FONTS_CONF "/etc/mystic/fonts.conf"

typedef struct {
    int  has_ui, has_mono;
    char ui[48], mono[48];
} FontConf;

static char *conf_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static void font_conf_read(FontConf *fc) {
    memset(fc, 0, sizeof *fc);
    FILE *f = fopen(FONTS_CONF, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = conf_trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = conf_trim(p);
        char *val = conf_trim(eq + 1);
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
                        (val[0] == '\'' && val[vl - 1] == '\''))) {
            val[vl - 1] = 0;
            val++;
        }
        if (!*val) continue;
        if (!strcmp(key, "ui")) {
            snprintf(fc->ui, sizeof fc->ui, "%s", val);
            fc->has_ui = 1;
        } else if (!strcmp(key, "mono")) {
            snprintf(fc->mono, sizeof fc->mono, "%s", val);
            fc->has_mono = 1;
        }
    }
    fclose(f);
}

static int font_conf_same(const FontConf *a, const FontConf *b) {
    return a->has_ui == b->has_ui && a->has_mono == b->has_mono &&
           !strcmp(a->ui, b->ui) && !strcmp(a->mono, b->mono);
}

static float theme_text_scale(void) {
    float s = themes[theme_index].font_scale;
    return (s >= 0.8f && s <= 1.6f) ? s : 1.0f;
}

/* font_scale is a multiplier on top of the user's ui.conf value. ui.c owns
 * the one global scale every layout reads; load the user's number first so a
 * theme switch can never compound the multiplier, then apply the theme's. */
static void apply_text_scale(void) {
    ui_scale_load("/etc/mystic/ui.conf");
    float user = ui_scale_get();
    float want = user * theme_text_scale();
    ui_scale_set_effective(want);
    if (fabsf(ui_scale_get() - want) > 0.01f)
        fprintf(stderr, "theme: font_scale %.2f x user %.2f exceeds the text "
                        "scale range, using %.2f\n",
                (double)theme_text_scale(), (double)user, (double)ui_scale_get());
}

/* Everything a theme contributes outside its colours: family roles (unless
 * fonts.conf names them) and the text-scale multiplier. */
static void theme_runtime_apply(void) {
    FontConf fc;
    font_conf_read(&fc);
    theme_apply_fonts(&themes[theme_index], fc.has_ui, fc.has_mono);
    apply_text_scale();
    dirty = 1;
}

/* ui.conf stores the scale RELATIVE to ui.c's baseline, and the same file
 * carries that baseline as its first line. Reading it back is what keeps
 * this file from hardcoding a third copy of the constant. */
static float ui_conf_baseline(void) {
    float base = 1.4f;              /* UI_BASE in src/ui.c: the shipped size */
    FILE *f = fopen("/etc/mystic/ui.conf", "r");
    if (!f) return base;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "baseline", 8)) continue;
        char *eq = strchr(line, '=');
        if (eq) {
            float v = (float)atof(eq + 1);
            if (v > 0.1f && v < 8.0f) base = v;
        }
        break;
    }
    fclose(f);
    return base;
}

/* The five shipped sizes are 0.8 .. 1.6 in steps of 0.2 relative to the
 * baseline, which is the same set the Settings app offers. Stepping through
 * them (instead of inventing a fine-grained number) keeps the file readable
 * and the Settings list in sync with what a chord just did. */
static void text_size_step(int dir) {
    float base = ui_conf_baseline();
    float cur = (ui_scale_get() / base) / theme_text_scale();
    int k = (int)((cur - 0.8f) / 0.2f + 0.5f);
    if (k < 0) k = 0;
    if (k > 4) k = 4;
    if (dir == 0) k = 1;                      /* Ctrl+0: back to 1.0x */
    else k += dir > 0 ? 1 : -1;
    if (k < 0) k = 0;
    if (k > 4) k = 4;
    float rel = 0.8f + 0.2f * (float)k;

    char text[96];
    snprintf(text, sizeof(text), "baseline = %.1f\nscale = %.1f\n",
             (double)base, (double)rel);
    FILE *f = fopen("/etc/mystic/ui.conf", "w");
    if (!f) {
        fprintf(stderr, "shortcuts: ui.conf: %s\n", strerror(errno));
        return;
    }
    int ok = fputs(text, f) != EOF;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "shortcuts: ui.conf not written\n"); return; }
    apply_text_scale();
    persist_settings();
    fprintf(stderr, "shortcuts: text scale %.1fx\n", (double)rel);
    dirty = 1;
}

/* Do not disturb. The notification service (src/notify.c) takes the same
 * one-line command over its documented socket, and the file is what a
 * service started after this press reads. With no service running the file
 * is still the honest record of the setting. */
static void dnd_toggle(void) {
    dnd_on = !dnd_on;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", NOTIFY_SOCKET);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            FILE *sf = fdopen(fd, "w");
            if (sf) {
                fd = -1;                 /* fclose owns it from here */
                fprintf(sf, "DND %d\n", dnd_on);
                fclose(sf);
            }
        }
        if (fd >= 0) close(fd);
    }
    FILE *f = fopen("/etc/mystic/dnd", "w");
    if (f) { fprintf(f, "%d\n", dnd_on); fclose(f); }
    fprintf(stderr, "shortcuts: do not disturb %s\n", dnd_on ? "on" : "off");
}

/* A screenshot of the desktop as it is on screen: `scene` is the composed
 * frame the framebuffer was last given, without the pointer. P6/PPM needs
 * no encoder and no dependency, and the pixels are the surface's own. */
static void take_screenshot(void) {
    if (mkdir("/root/screenshots", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "shortcuts: screenshot dir: %s\n", strerror(errno));
        return;
    }
    char path[96];
    int slot = -1;
    for (int n = shot_next; n < 10000; n++) {
        struct stat st;
        snprintf(path, sizeof(path), "/root/screenshots/shot-%04d.ppm", n);
        if (stat(path, &st) != 0) { slot = n; break; }   /* never overwrite */
    }
    if (slot < 0) { fprintf(stderr, "shortcuts: screenshot: no free name\n"); return; }
    shot_next = slot + 1;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "shortcuts: screenshot: %s\n", strerror(errno)); return; }
    unsigned char *row = malloc((size_t)scene.w * 3u);
    if (!row) { fclose(f); remove(path); return; }
    int ok = fprintf(f, "P6\n%d %d\n255\n", scene.w, scene.h) > 0;
    for (int y = 0; y < scene.h && ok; y++) {
        const u32 *src = scene.px + (size_t)y * (size_t)scene.stride;
        for (int x = 0; x < scene.w; x++) {
            row[x * 3 + 0] = (unsigned char)((src[x] >> 16) & 0xFF);
            row[x * 3 + 1] = (unsigned char)((src[x] >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)(src[x] & 0xFF);
        }
        if (fwrite(row, 3, (size_t)scene.w, f) != (size_t)scene.w) ok = 0;
    }
    free(row);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "shortcuts: screenshot failed\n");
        remove(path);
        return;
    }
    fprintf(stderr, "shortcuts: screenshot %s (%dx%d)\n", path, scene.w, scene.h);
}

static void shortcut_action(void *ctx, int id, int key, unsigned mods) {
    (void)ctx;
    (void)mods;
    Window *w = win_focused();

    switch (id) {
    case SC_ACT_WIN_CLOSE:   if (w) win_close(w); break;
    case SC_ACT_WIN_MIN:
        if (w) { w->hidden = 1; Window *nx = win_focused(); if (nx) win_focus(nx); }
        break;
    case SC_ACT_WIN_MAX:
        if (w) { if (w->maxed == 3) w->maxed = 0; win_toggle_max(w); }
        break;
    case SC_ACT_WIN_FULL:    win_fullscreen(w); break;
    case SC_ACT_WIN_RESTORE: win_restore_action(w); break;
    case SC_ACT_WIN_NEXT:    focus_cycle(1); break;
    case SC_ACT_WIN_PREV:    focus_cycle(-1); break;
    case SC_ACT_WIN_SAME_APP: focus_same_app(); break;
    case SC_ACT_WIN_MOVE:    win_move_by(w, key); break;
    case SC_ACT_WIN_RESIZE:  win_resize_by(w, key); break;
    case SC_ACT_WIN_SNAP:
        if (key == SC_KEY_LEFT)       win_snap(w, -1);
        else if (key == SC_KEY_RIGHT) win_snap(w, 1);
        else                          win_snap(w, 0);
        break;
    case SC_ACT_WIN_EDGE:    win_edge(w, key); break;
    case SC_ACT_WIN_GROW:    win_grow(w, 1); break;
    case SC_ACT_WIN_SHRINK:  win_grow(w, -1); break;
    case SC_ACT_WIN_CENTRE:  win_centre(w); break;
    case SC_ACT_WIN_RAISE:   win_raise_lower(w); break;
    case SC_ACT_WIN_ONTOP:   win_toggle_ontop(w); break;
    case SC_ACT_WIN_FOCUS:   focus_window_n(key - SC_KEY_1); break;
    case SC_ACT_WIN_MIN_ALL: win_min_all(); break;
    case SC_ACT_WIN_RESTORE_ALL: win_restore_all(); break;
    case SC_ACT_WIN_CLOSE_ALL:   win_close_all(); break;
    case SC_ACT_LAUNCHER:
        /* a bare Super press is half of a tap: the launcher is toggled when
         * the key comes back up, so Super+E can still open Files */
        if (key != SC_KEY_META) launcher_open = !launcher_open;
        break;
    case SC_ACT_LOCK:
        locked = 1;
        pin_len = 0;
        pin[0] = 0;
        lock_msg[0] = 0;
        break;
    case SC_ACT_THEME_NEXT:
        if (theme_count > 0) theme_apply((theme_index + 1) % theme_count);
        break;
    case SC_ACT_THEME_PREV:
        if (theme_count > 0)
            theme_apply((theme_index + theme_count - 1) % theme_count);
        break;
    case SC_ACT_THEME_SET:   theme_apply(key - SC_KEY_1); break;
    case SC_ACT_WALLPAPER_NEXT:
        wallpaper_apply((wall_index + 1) % WALLPAPER_COUNT);
        break;
    case SC_ACT_WALLPAPER_PREV:
        wallpaper_apply((wall_index + WALLPAPER_COUNT - 1) % WALLPAPER_COUNT);
        break;
    case SC_ACT_TEXT_UP:     text_size_step(1); break;
    case SC_ACT_TEXT_DOWN:   text_size_step(-1); break;
    case SC_ACT_TEXT_RESET:  text_size_step(0); break;
    case SC_ACT_DND:         dnd_toggle(); break;
    case SC_ACT_SHOT:        take_screenshot(); break;
    case SC_ACT_SHOT_WINDOW: take_screenshot_window(); break;
    case SC_ACT_TERMINAL:    win_open(WIN_TERM, NULL); break;
    case SC_ACT_FILES:       win_open(WIN_APP, "files"); break;
    case SC_ACT_SETTINGS:    win_open(WIN_APP, "settings"); break;
    case SC_ACT_MONITOR:     win_open(WIN_APP, "monitor"); break;
    case SC_ACT_NOTES:       win_open(WIN_APP, "notes"); break;
    case SC_ACT_STORE:       win_open(WIN_APP, "store"); break;
    case SC_ACT_CALC:        win_open(WIN_APP, "calc"); break;
    case SC_ACT_SYSINFO:     win_open(WIN_APP, "sysinfo"); break;
    case SC_ACT_BROWSER:     launch_bundle("web"); break;
    case SC_ACT_BROWSER_RELOAD: browser_reload(); break;
    case SC_ACT_BROWSER_ADDR:   browser_addr(); break;
    case SC_ACT_TERM_CLEAR:     term_clear(); break;
    case SC_ACT_TERM_COPY:      term_copy(); break;
    case SC_ACT_TERM_PASTE:     term_paste(); break;
    case SC_ACT_TERM_FONT_UP:   term_font(1); break;
    case SC_ACT_TERM_FONT_DOWN: term_font(-1); break;
    case SC_ACT_HELP:
        help_open = !help_open;
        if (help_open) help_page = 0;
        break;
    case SC_ACT_CANCEL:      cancel_action(); break;
    case SC_ACT_NONE:
    case SC_ACT_COUNT:
    default:
        /* a row without a case is a compiler warning above and a real
         * message here - never a key that silently does nothing */
        fprintf(stderr, "shortcuts: no action for id %d\n", id);
        break;
    }
}

static const ShortcutActions SHORTCUTS = { shortcut_action, NULL };

        static int handle_click(void) {
    int mx = input.mouse_x, my = input.mouse_y;

    /* the shortcut panel is modal: a click dismisses it and does nothing
     * else, so a half-seen key cannot be clicked "through" by accident */
    if (help_open) { help_open = 0; help_page = 0; dirty = 1; return 1; }

    /* the desktop menu is modal too: an item runs, anywhere else dismisses */
    if (menu_open) return menu_click(mx, my, input.buttons);

    /* The taskbar is drawn above the windows, so it must be hit first:
     * otherwise a window dragged over the bar swallows its clicks. */
    if (taskbar_click(mx, my)) return 1;

    /* the start button is above everything else in the top bar */
    if (mx >= EYE_HIT_X && mx < EYE_HIT_X + EYE_HIT_W &&
        my >= EYE_HIT_Y + eye_oy() && my < EYE_HIT_Y + eye_oy() + EYE_HIT_H) {
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
        if (!hit || win_rank(w) > win_rank(hit)) hit = w;
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
         * minimised, so "maximise" and "minimise" were both wrong.
         * The band and the x of each light come from dot_sz()/dot_x(), the
         * same functions the painter uses, so a target is always on its dot. */
        const int dsz = dot_sz(), slack = ui_px(1);
        const int in_band = (ly >= dot_y - slack && ly < dot_y + dsz + slack);
        if (in_band && lx >= dot_x(0) && lx < dot_x(0) + dsz) {
            win_close(hit);
            return 1;
        }
        if (in_band && lx >= dot_x(1) && lx < dot_x(1) + dsz) {
            win_toggle_max(hit);
            return 1;
        }
        if (in_band && lx >= dot_x(2) && lx < dot_x(2) + dsz) {
            hit->hidden = 1;
            return 1;
        }
        /* bottom-right corner resizes */
        if (lx >= hit->w - ui_px(13) && ly >= hit->h - ui_px(13)) {
            resize_win = (int)(hit - wins);
            resize_gen = hit->gen;          /* F9: remember which window, not just the slot */
            resize_ow = hit->w;
            resize_oh = hit->h;
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
            drag_ox = hit->x;               /* Esc puts it back here */
            drag_oy = hit->y;
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

    /* empty wallpaper: a right click opens the desktop menu there; a left
     * click on the desktop starts a Black Hole drag (and does nothing for the
     * other wallpapers, as before) */
    if (input.buttons & 2) {
        menu_open = 1;
        menu_sel = MENU_TERM;
        menu_x = mx;
        menu_y = my;
        dirty = 1;
        return 1;
    }
    if ((input.buttons & 1) && wall_view_active() && !over_chrome_at(mx, my)) {
        wall_drag = 1;
        dirty = 1;
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------ display choice
 *
 * /etc/mystic/display.conf is up to three lines:
 *     mode=1920x1080      (a mode the device itself reported)
 *     logical=1024x768    (the canvas; `auto` = the design size for the panel)
 *     scale=fit
 * It is read after disp_open() has asked the device for its mode list, so a
 * saved mode is only ever applied when this device reported it. A mode that
 * is not in the list, or that the driver refuses, is named on stderr and the
 * current mode is kept - never one the panel did not report. The canvas line
 * is independent of the mode list: it works on a device that reports one
 * mode, which is the only way a resolution choice can do something on QEMU.
 * /etc/mystic is a RAM rootfs, so Settings also writes
 * /root/mystic-display.conf and this restores that copy when the RAM one is
 * missing.
 */
#ifndef DISPLAY_CONF
#define DISPLAY_CONF "/etc/mystic/display.conf"
#endif
#ifndef DISPLAY_CONF_SAVED
#define DISPLAY_CONF_SAVED "/root/mystic-display.conf"
#endif

/* MYSTIC_DISPLAY_CONF_SECTION_BEGIN
 * The block below is main.c's whole display.conf reader. The host harness
 * extracts this exact text (with sed) and links it against a fake panel, so
 * the parsing and the fallback it proves are the shipping lines. */

static int display_scale_from_key(const char *s, DispScale *out) {
    if (!strcmp(s, "fit") || !strcmp(s, "Fit")) { *out = DISP_FIT; return 1; }
    if (!strcmp(s, "stretch") || !strcmp(s, "Stretch")) { *out = DISP_STRETCH; return 1; }
    if (!strcmp(s, "integer") || !strcmp(s, "Integer")) { *out = DISP_INTEGER; return 1; }
    if (!strcmp(s, "1:1") || !strcmp(s, "1to1")) { *out = DISP_ONE_TO_ONE; return 1; }
    return 0;
}

/* ------------------------------------------------------------ canvas
 *
 * The canvas is the logical size the compositor draws before the blit
 * maps it onto the panel. It is a user choice, not only a policy: on a
 * device that reports a single mode (every QEMU fbdev) it is the ONLY
 * display setting that can change the picture. Stored as
 * `logical=WxH`, or `logical=auto` (absent counts as auto) for "the
 * design size for this panel".
 *
 * The bounds are what the compositor allocates twice (screen + scene)
 * without pretending: below them no window can be placed, above them a
 * typo would ask for 64 MB surfaces. A size outside them is clamped and
 * the caller is told - never silently used.
 */
#define LOGICAL_MIN_W 320
#define LOGICAL_MIN_H 240
#define LOGICAL_MAX_W 4096
#define LOGICAL_MAX_H 4096

static int g_logical_auto = 1;                 /* follow the panel */
static int g_logical_w, g_logical_h;           /* only read when !auto */

int desk_logical_auto(void) { return g_logical_auto; }

/* Clamp a canvas pair in place; 1 when it was already inside the bounds. */
static int logical_fit(int *w, int *h) {
    int exact = 1;
    if (*w < LOGICAL_MIN_W) { *w = LOGICAL_MIN_W; exact = 0; }
    if (*h < LOGICAL_MIN_H) { *h = LOGICAL_MIN_H; exact = 0; }
    if (*w > LOGICAL_MAX_W) { *w = LOGICAL_MAX_W; exact = 0; }
    if (*h > LOGICAL_MAX_H) { *h = LOGICAL_MAX_H; exact = 0; }
    return exact;
}

/* Apply the remembered canvas to the display. Called at start and again
 * after a mode change, because a new panel size changes what "auto"
 * means. disp_set_logical_size() also moves the inverse mapper, so the
 * first click after this is mapped through the new blit. */
void desk_follow_mode(Display *d) {
    if (!d) return;
    int lw, lh;
    if (g_logical_auto) {
        desk_design_size(d->fb_w, d->fb_h, &lw, &lh);
    } else {
        lw = g_logical_w;
        lh = g_logical_h;
        logical_fit(&lw, &lh);
    }
    disp_set_logical_size(d, lw, lh);
}

/* Settings' canvas rows: (0,0) = follow the panel; any other pair is the
 * canvas. Applies now and remembers for the next mode change. Returns 1
 * when the pair was used as given, 0 when it had to be clamped. */
int desk_set_logical(Display *d, int w, int h) {
    if (!d) return 0;
    int exact = 1;
    if (w <= 0 || h <= 0) {
        g_logical_auto = 1;
    } else {
        int lw = w, lh = h;
        exact = logical_fit(&lw, &lh);
        g_logical_auto = 0;
        g_logical_w = lw;
        g_logical_h = lh;
    }
    desk_follow_mode(d);
    return exact;
}

static void display_restore_saved(void) {
    if (access(DISPLAY_CONF, R_OK) == 0) return;
    FILE *in = fopen(DISPLAY_CONF_SAVED, "rb");
    if (!in) return;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), in);
    int bad = ferror(in);
    fclose(in);
    if (bad) return;
    FILE *out = fopen(DISPLAY_CONF, "wb");
    if (!out) {
        fprintf(stderr, "display: cannot restore %s: %s\n", DISPLAY_CONF,
                strerror(errno ? errno : EIO));
        return;
    }
    int werr = 0;
    if (fwrite(buf, 1, n, out) != n) werr = errno ? errno : EIO;
    if (!werr && fflush(out) != 0) werr = errno ? errno : EIO;
    if (fclose(out) != 0 && !werr) werr = errno ? errno : EIO;
    if (werr) {
        fprintf(stderr, "display: cannot write %s: %s\n", DISPLAY_CONF,
                strerror(werr));
        return;
    }
    fprintf(stderr, "display: restored %s from %s\n", DISPLAY_CONF,
            DISPLAY_CONF_SAVED);
}

static void display_load_conf(Display *d) {
    FILE *f = fopen(DISPLAY_CONF, "r");
    if (!f) {
        fprintf(stderr, "display: no %s; using %dx%d, %s\n", DISPLAY_CONF,
                d->fb_w, d->fb_h, disp_scale_name(disp_get_scale(d)));
        return;
    }

    int mw = 0, mh = 0, have_mode = 0, have_scale = 0;
    DispScale want = disp_get_scale(d);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        if (sscanf(p, "mode=%dx%d", &mw, &mh) == 2 && mw > 0 && mh > 0) {
            have_mode = 1;
            continue;
        }
        if (!strncmp(p, "scale=", 6)) {
            char key[32] = {0};
            if (sscanf(p + 6, "%31s", key) == 1 && display_scale_from_key(key, &want)) {
                have_scale = 1;
            } else {
                fprintf(stderr, "display: %s names an unknown scale; keeping %s\n",
                        DISPLAY_CONF, disp_scale_name(disp_get_scale(d)));
            }
            continue;
        }
        if (!strncmp(p, "logical=", 8)) {
            char key[32] = {0};
            if (sscanf(p + 8, "%31s", key) != 1) continue;
            if (!strcmp(key, "auto")) {
                g_logical_auto = 1;
            } else {
                int lw = 0, lh = 0;
                if (sscanf(key, "%dx%d", &lw, &lh) == 2 && lw > 0 && lh > 0) {
                    int cw = lw, ch = lh;
                    if (!logical_fit(&cw, &ch))
                        fprintf(stderr, "display: canvas %dx%d is outside %dx%d..%dx%d; "
                                        "clamped to %dx%d\n", lw, lh,
                                LOGICAL_MIN_W, LOGICAL_MIN_H,
                                LOGICAL_MAX_W, LOGICAL_MAX_H, cw, ch);
                    g_logical_auto = 0;
                    g_logical_w = cw;
                    g_logical_h = ch;
                } else {
                    fprintf(stderr, "display: %s names an unreadable canvas \"%s\"; "
                                    "following the panel\n", DISPLAY_CONF, key);
                    g_logical_auto = 1;
                }
            }
        }
    }
    fclose(f);

    if (have_mode) {
        int idx = -1;
        for (int i = 0; i < d->nmodes; i++) {
            if (d->modes[i].w == mw && d->modes[i].h == mh) { idx = i; break; }
        }
        if (idx < 0) {
            fprintf(stderr,
                    "display: saved mode %dx%d is not offered by this device; "
                    "keeping %dx%d\n", mw, mh, d->fb_w, d->fb_h);
        } else {
            errno = 0;
            if (disp_set_mode(d, idx) != 0) {
                int e = errno ? errno : EIO;
                fprintf(stderr,
                        "display: saved mode %dx%d refused by the kernel: %s; "
                        "keeping %dx%d\n", mw, mh, strerror(e), d->fb_w, d->fb_h);
            } else {
                fprintf(stderr, "display: mode %dx%d\n", d->fb_w, d->fb_h);
            }
        }
    }
    if (have_scale) {
        disp_set_scale(d, want);
        fprintf(stderr, "display: scaling %s\n", disp_scale_name(disp_get_scale(d)));
    }
}

/* Pick the surface size after the mode is known, and tell the journal the
 * three numbers that matter: what is drawn, what the panel is, and how it is
 * mapped. The canvas is whatever display.conf chose (or the design size for
 * this panel when it said `logical=auto` or nothing). */
static void display_choose_logical(Display *d) {
    desk_follow_mode(d);
    fprintf(stderr, "display: %dx%d canvas on a %dx%d panel, %s%s\n",
            d->w, d->h, d->fb_w, d->fb_h, disp_scale_name(disp_get_scale(d)),
            g_logical_auto ? " (auto)" : "");
}
/* MYSTIC_DISPLAY_CONF_SECTION_END */

/* ------------------------------------------------------------ main */
int main(void) {
    install_fatal_handlers();
    if (disp_open(&disp, "/dev/fb0") < 0) return 1;
    /* Ask the device what it can do before anything asks for a mode. */
    disp_scan_modes(&disp);
    /* With no saved choice the design is fitted to the panel, not left in a
     * corner at 1:1: that is the behaviour this desktop is for. display.conf,
     * when it exists, overrides this below. */
    disp_set_scale(&disp, DISP_FIT);
    display_restore_saved();
    display_load_conf(&disp);
    display_choose_logical(&disp);

    int logical_w = disp.w, logical_h = disp.h;
    wms_set_screen(logical_w, logical_h);  /* client buffers are screen sized */
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
    /* do-not-disturb survives a restart: it is the same file Super+N writes,
     * and the top bar shows it when it is on */
    {
        FILE *df = fopen("/etc/mystic/dnd", "r");
        if (df) {
            int v = 0;
            if (fscanf(df, "%d", &v) == 1) dnd_on = v ? 1 : 0;
            fclose(df);
        }
    }
    font_init(FONT_DIR);
    fprintf(stderr, "ui: scale %.2f", ui_scale_get());
    fputc(10, stderr);

    screen.w = logical_w; screen.h = logical_h; screen.stride = logical_w;
    screen.px = calloc((size_t)screen.w * screen.h, 4);
    scene = screen;
    scene.px = calloc((size_t)screen.w * screen.h, 4);
    if (!screen.px || !scene.px) {
        /* A saved canvas can be larger than this machine can hold - a
         * hand-edited display.conf, or less RAM than last boot. Falling back
         * to the design size is the difference between a smaller desktop and
         * no desktop at all; the saved choice is left alone so a machine
         * with the memory keeps it. */
        free(screen.px);
        free(scene.px);
        int fw = 0, fh = 0;
        desk_design_size(disp.fb_w, disp.fb_h, &fw, &fh);
        fprintf(stderr, "display: no memory for a %dx%d canvas; using %dx%d\n",
                logical_w, logical_h, fw, fh);
        logical_w = fw;
        logical_h = fh;
        disp_set_logical_size(&disp, fw, fh);
        wms_set_screen(fw, fh);
        screen.w = fw; screen.h = fh; screen.stride = fw;
        screen.px = calloc((size_t)fw * (size_t)fh, 4);
        scene = screen;
        scene.px = calloc((size_t)fw * (size_t)fh, 4);
        if (!screen.px || !scene.px) { fprintf(stderr, "out of memory\n"); return 1; }
    }

    theme_count = theme_load_all(themes, 16, THEME_DIR);
    if (theme_count == 0) {
        Theme t;
        theme_load(&t, "/nonexistent");
        themes[0] = t;
        theme_count = 1;
    }
    /* fonts.conf has been read by font_init(); the theme's roles and scale
     * are applied on top of it once, before the first frame. */
    theme_runtime_apply();

    if (input_open(&input) < 0) fprintf(stderr, "no input devices\n");

    if (wms_start() < 0) fprintf(stderr, "window server failed to start\n");
    bundle_count = wms_scan_apps(bundles, WM_MAX_APPS);
    fprintf(stderr, "wms: %d installed app bundle(s)\n", bundle_count);

    int quit = 0;
    int cur_x = -1, cur_y = -1;      /* where the cursor is painted right now */
    long last_tick = 0;

    while (!quit) {
        /* Settings can ask the panel for another mode while this loop runs.
         * The core reports the logical size that mode needs; when it differs
         * from the surfaces, reallocate them and pull the windows inside. */
        {
            int lw = screen.w, lh = screen.h;
            disp_logical_size(&disp, &lw, &lh);
            if (lw > 0 && lh > 0 && (lw != screen.w || lh != screen.h)) {
                if (!desktop_reflow(lw, lh)) {
                    /* The canvas was asked for but its two surfaces could not
                     * be allocated. Keep drawing what exists and put the core
                     * back to the truth, so the page cannot show a size that
                     * is not on screen and the next frame does not ask again.
                     * The choice stays in display.conf, so a machine with the
                     * memory for it can still use it after a restart. */
                    fprintf(stderr, "display: %dx%d canvas does not fit; "
                                    "keeping %dx%d\n", lw, lh, screen.w, screen.h);
                    disp_set_logical_size(&disp, screen.w, screen.h);
                }
            }
        }
        /* The surface that will be presented is exactly the logical size.
         * Keeping the inverse mapper's source size in step here covers the
         * frame in which Settings changes the mode: the next click is already
         * mapped through the new blit, not the previous one. */
        disp.src_w = screen.w;
        disp.src_h = screen.h;

        input_frame(&input);
        input_poll(&input);

        /* evdev reports the panel's pixels; everything above this line lives
         * in the logical frame. Invert the same blit disp_present() uses, so
         * a click lands on the row under the pointer in every scale mode. */
        disp_map_input(&input.mouse_x, &input.mouse_y);
        if (input.mouse_dx || input.mouse_dy) {
            int bw = disp.geom.bw > 0 ? disp.geom.bw : screen.w;
            int bh = disp.geom.bh > 0 ? disp.geom.bh : screen.h;
            if (bw > 0 && bh > 0 && screen.w > 0 && screen.h > 0) {
                input.mouse_dx = (int)lroundf((float)input.mouse_dx *
                                              (float)screen.w / (float)bw);
                input.mouse_dy = (int)lroundf((float)input.mouse_dy *
                                              (float)screen.h / (float)bh);
            }
        }
        if (input.mouse_x < 0) input.mouse_x = 0;
        if (input.mouse_y < 0) input.mouse_y = 0;
        if (input.mouse_x >= screen.w) input.mouse_x = screen.w - 1;
        if (input.mouse_y >= screen.h) input.mouse_y = screen.h - 1;

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
                    if (idx < theme_count) { theme_index = idx; theme_runtime_apply(); }
                    input.text[i] = 0;      /* consumed */
                }
            }
        }

        /* Every key pressed this frame, in order. Handling only the last one
         * collapsed fast bursts: five arrow presses registered as one. */
        for (int ki = 0; ki < input.nkeys; ki++) {
            int k = input.keys[ki];

            /* Super is a modifier as well as the launcher key, and input.c
             * reports the Meta keys like any other key, so the desktop tracks
             * it: held across frames, and a tap (pressed with nothing else in
             * between) is the launcher on the RELEASE. Toggling on the press
             * instead would open the launcher and swallow Super+Left. */
            if (k == SC_KEY_META) { super_down = 1; super_lone = 1; }
            else if (super_down) super_lone = 0;

            /* What the dispatcher is asked with. Computed once so a binding
             * means the same thing wherever it is reached from. */
            const unsigned sc_mods = (input.shift ? SC_MOD_SHIFT : 0u) |
                                     (input.ctrl  ? SC_MOD_CTRL  : 0u) |
                                     (input.alt   ? SC_MOD_ALT   : 0u) |
                                     (super_down  ? SC_MOD_SUPER : 0u);
            const unsigned sc_cond = win_focused() ? SC_WHEN_WINDOW : SC_WHEN_NO_WINDOW;
            const unsigned sc_modals = keyboard_modals();

            /* One table decides every desktop binding (src/shortcuts.c). It
             * runs before every overlay and before the focused window gets
             * the key, and it is told which desktop modals are up: only the
             * rows marked SC_ALWAYS (Alt+Tab and Esc) act through one. A
             * key it does not know falls through exactly as before. */
            if (shortcuts_dispatch(&SHORTCUTS, k, sc_mods, sc_cond, sc_modals)) {
                /* a chord that also types a character (Ctrl+W is 0x17) must
                 * not type it into the window behind the action */
                if (sc_mods & (SC_MOD_CTRL | SC_MOD_ALT | SC_MOD_SUPER)) {
                    input.text_len = 0;
                    input.text[0] = 0;
                }
                dirty = 1;
                continue;
            }

            /* the shortcut panel is modal: PageUp/PageDown walk its pages,
             * F1 or the chord that opened it closes it, and nothing behind
             * it sees a key while it is up. Esc never reaches here - it is
             * an SC_ALWAYS row, so shortcuts_dispatch() already cancelled
             * the panel above. */
            if (help_open) {
                if (k == SC_KEY_PAGEUP || k == SC_KEY_UP) {
                    if (help_page > 0) help_page--;
                } else if (k == SC_KEY_PAGEDOWN || k == SC_KEY_DOWN) {
                    help_page++;        /* the panel layout clamps to its last */
                } else if (k == SC_KEY_HOME) {
                    help_page = 0;
                } else if (k == SC_KEY_F1 || (super_down && k == SC_KEY_SLASH)) {
                    help_open = 0;
                }
                input.text_len = 0;
                dirty = 1;
                continue;
            }

            /* the desktop menu is modal: arrows and Enter walk it, Esc was
             * handled above, and anything else types nothing */
            if (menu_open) {
                menu_key(k);
                input.text_len = 0;
                dirty = 1;
                continue;
            }

            if (launcher_open) {
                if (k == 105 || k == 106 || k == 103 || k == 108) {
                    int cols = ui_launcher_cols(launcher_items(), screen.w, screen.h,
                                                launcher_label_w());
                    int total = launcher_items(), step = 0;
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

                /* modal overlay: nothing may leak to the window behind it.
                 * Alt+Tab (an SC_ALWAYS row) already had its chance above. */
                input.text_len = 0;
                dirty = 1;
                continue;
            }

            /* the focused window gets the key */
            Window *top = win_focused();
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

        /* Super released: a tap that touched nothing else is the launcher.
         * Done here rather than on the press, which is what lets Super+Left,
         * Super+E and the rest keep their own meaning. */
        for (int i = 0; i < input.nkeys_up; i++) {
            if (input.keys_up[i] != SC_KEY_META) continue;
            if (super_down && super_lone) {
                launcher_open = !launcher_open;
                dirty = 1;
            }
            super_down = 0;
            super_lone = 0;
        }

        /* releases: the protocol carries a press flag that was always 1, so a
         * client could never tell when a key went back up */
        {
            Window *top = win_focused();
            if (top && top->kind == WIN_CLIENT && top->client >= 0 && input.nkeys_up) {
                WmClient *cl = &clients[top->client];
                int mods = (input.shift ? 1 : 0) | (input.ctrl ? 2 : 0) | (input.alt ? 4 : 0);
                for (int i = 0; i < input.nkeys_up; i++)
                    wms_notify_key(cl, input.keys_up[i], mods, 0);
            }
        }

        /* text is delivered once per frame, not per key */
        {
            /* a modal overlay owns the keyboard: nothing typed while the
             * launcher or the shortcut panel is up may reach the window
             * behind it */
            Window *top = (launcher_open || help_open || menu_open) ? NULL : win_focused();
            if (top && input.text_len) {
                if (top->term) {
                    term_scroll(top->term, -(term_scroll_offset(top->term)));
                    term_key(top->term, input.text, input.text_len);
                    term_note_input(top, input.text, input.text_len);
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
                if (!under || win_rank(w) > win_rank(under)) under = w;
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
            } else if (wall_view_active() &&
                       !over_chrome_at(input.mouse_x, input.mouse_y) &&
                       !launcher_open && !menu_open && !help_open) {
                /* over the wallpaper, and the wallpaper is a black hole: the
                 * wheel shears the disk. Wallpaper.h documents the unit
                 * (seconds of disk time per notch), so the knob keeps the
                 * Keplerian profile instead of rotating it rigidly. */
                wallpaper_blackhole_spin((float)input.wheel *
                                         WALLPAPER_BH_SPIN_PER_NOTCH);
                dirty = 1;
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
                const int edge = ui_px(6), foot = ui_px(64);
                if (w->y <= bar + ui_px(4)) {                /* top edge: full width */
                    w->x = 0; w->y = bar; w->w = screen.w; w->h = screen.h - bar - foot;
                } else if (w->x <= edge) {                   /* left edge: left half */
                    w->x = 0; w->y = bar; w->w = screen.w / 2; w->h = screen.h - bar - foot;
                } else if (w->x + w->w >= screen.w - edge) {   /* right edge: right half */
                    w->w = screen.w / 2; w->x = screen.w - w->w;
                    w->y = bar; w->h = screen.h - bar - foot;
                }
                window_resized(w);
                dirty = 1;
            }
            drag_win = -1;
            resize_win = -1;
            wall_drag = 0;
        }
        if ((input.buttons & 1) && resize_win >= 0) {
            Window *w = &wins[resize_win];
            /* F9: the window closed and the slot was reused - not our window */
            if (!w->used || w->gen != resize_gen) { resize_win = -1; }
            else {
                int nw = input.mouse_x - w->x + 1;
                int nh = input.mouse_y - w->y + 1;
                /* Minimums that leave a client app room to draw. Both the
                 * width and the height are the desktop's own minimum module:
                 * the resize clamps, the initial size and a snapped window
                 * read the same functions, so they cannot disagree. */
                if (nw < ui_win_min_w()) nw = ui_win_min_w();
                if (nh < ui_win_min_h()) nh = ui_win_min_h();
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
                /* a title bar cannot be dragged under the top bar; 34 was the
                 * bar height at the shipped scale and let go at 1.6 */
                if (ny < topbar_h()) ny = topbar_h();
                if (nx + w->w > screen.w) nx = screen.w - w->w;
                if (ny + w->h > screen.h) ny = screen.h - w->h;
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; dirty = 1; }
            }
        }
        /* A drag that began on the wallpaper is the Black Hole's camera:
         * the header documents the delta and the clamp, so this only feeds
         * the pointer's own movement in. If the wallpaper was changed mid
         * drag the knob is dropped rather than applied to another painting. */
        if (wall_drag) {
            if (!(input.buttons & 1) || !wall_view_active()) {
                wall_drag = 0;
            } else if (input.mouse_dy) {
                wallpaper_blackhole_view((float)input.mouse_dy *
                                         WALLPAPER_BH_YAW_PER_PX);
                dirty = 1;
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
                    w->x = ui_px(86) + (i % 4) * ui_px(26);
                    w->y = ui_px(64) + (i % 4) * ui_px(26);
                    if (w->x + w->w > screen.w) w->x = screen.w - w->w - ui_px(6);
                    if (w->y + w->h > screen.h) w->y = screen.h - w->h - ui_px(71);
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
            Window *ct = win_focused();
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

        /* The icon breathe is the one chrome animation the pointer drives.
         * It asks for frames only while an item is active; 30 fps is plenty
         * for a breathe of at most 3.5 % and keeps a hover as cheap as the dock
         * feedback that was already there. anim_speed = 0 never gets here. */
        int item_active = anim_item_under_pointer();
        if (item_active) {
            double pulse_now = now_sec();
            if (pulse_now - pulse_last >= 0.033) { pulse_last = pulse_now; dirty = 1; }
        }
        /* drift is background decoration, and it is held to the same gate: it
         * advances while an item is active, never on its own. With the pointer
         * elsewhere the desktop still requests no frames for chrome motion. */
        if (themes[theme_index].anim == ANIM_DRIFT && item_active) {
            double drift_now = now_sec();
            if (drift_now - drift_last >= 0.05) { drift_last = drift_now; dirty = 1; }
        }

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
            /* the status line is a confirmation, not a notification: it goes
             * away on its own after a couple of seconds */
            if (status_t0 > 0 && now_sec() - status_t0 > 2.5) {
                status_t0 = 0;
                status_msg[0] = 0;
            }
            /* Settings can change these at runtime: re-read both files once a
             * second (a whole-file read of two small files is cheap). */
            {
                float before = ui_scale_get();
                ui_scale_load("/etc/mystic/ui.conf");
                ui_scale_set_effective(ui_scale_get() * theme_text_scale());
                if (ui_scale_get() != before) {
                    fprintf(stderr, "ui: scale %.2f -> %.2f", before, ui_scale_get());
                    fputc(10, stderr);
                    dirty = 1;
                    persist_settings();
                }

                /* the user's font selector is live: a Settings click writes
                 * fonts.conf, and this re-read is what makes it visible */
                static FontConf last_fc;
                static int fc_ready;
                FontConf fc;
                font_conf_read(&fc);
                if (!fc_ready) { fc_ready = 1; last_fc = fc; }
                else if (!font_conf_same(&fc, &last_fc)) {
                    last_fc = fc;
                    font_reload();
                    theme_apply_fonts(&themes[theme_index], fc.has_ui, fc.has_mono);
                    dirty = 1;
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
                /* A package install changes /usr/share/mystic/apps, which is
                 * what the launcher and the dock enumerate. wmsrv rescans and
                 * says whether the list really changed; without this a fresh
                 * bundle stayed invisible until the next boot. */
                if (wms_apps_poll_rescan(bundles, &bundle_count, WM_MAX_APPS))
                    dirty = 1;
            }

            /* another app can ask for a theme by writing this file */
            FILE *tf = fopen("/etc/mystic/current-theme", "r");            if (tf) {
                char want[64] = {0};
                if (fgets(want, sizeof(want), tf)) {
                    char *nl = strchr(want, '\n');
                    if (nl) *nl = 0;
                    for (int i = 0; i < theme_count; i++)
                        if (!strcmp(want, themes[i].name)) {
                            theme_index = i;
                            theme_runtime_apply();
                            persist_settings();
                        }
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
            /* a launch that failed has a real reason (a missing binary, a
             * bad manifest) and it used to go nowhere: the tile just did
             * nothing. wmsrv keeps the last reason; show it as the status
             * confirmation instead of guessing one here. */
            {
                char lerr[192];
                if (wms_take_launch_error(lerr, sizeof(lerr)))
                    set_status("%s", lerr);
            }

            /* Files asks for a DOCUMENT to be opened. The app is chosen here
             * from the type registry, not by the app that asked, so a
             * sandboxed file manager cannot pick what runs: it only names a
             * path, and this side decides. */
            FILE *orp = fopen("/run/mystic/open.request", "r");
            if (orp) {
                char path[256] = {0};
                if (fgets(path, sizeof(path), orp)) {
                    char *nl = strchr(path, 10);
                    if (nl) *nl = 0;
                    const FileType *ft = filetype_for(path);
                    const char *app = ft ? filetype_app(ft) : "viewer";
                    int ok = path[0] == '/' && !strstr(path, "..");
                    if (!ok) {
                        fprintf(stderr, "open: refusing path %.60s\n", path);
                    } else {
                        for (int b = 0; ok && b < bundle_count; b++) {
                            if (!strcmp(app, bundles[b].id)) {
                                int pid = wms_launch_path(&bundles[b], clients, b, path);
                                fprintf(stderr, "open: %s -> %s pid %d\n", path, app, pid);
                                break;
                            }
                        }
                    }
                }
                fclose(orp);
                remove("/run/mystic/open.request");
            }
        }
        /* A mouse move used to set dirty, which recomposed the whole desktop -
         * every window's shadow and blur - for every single motion event.
         * That was the stutter the user felt when moving the pointer. Now the
         * cursor alone is redrawn, unless the pointer is over chrome that
         * highlights (the dock magnifies, the top bar reacts). */
        int over_chrome = input.mouse_y < topbar_h();
        if (launcher_open) {
            /* inside the panel, a one-pixel move must not recompose the
             * desktop: only a change of hovered tile changes what is drawn */
            static int last_tile = -2;
            int tile = -1;
            for (int i = 0; i < launcher_items(); i++) {
                int tx, ty, tw, th2;
                launcher_geom(i, &tx, &ty, &tw, &th2);
                if (input.mouse_x >= tx && input.mouse_x < tx + tw &&
                    input.mouse_y >= ty && input.mouse_y < ty + th2) { tile = i; break; }
            }
            if (tile != last_tile) { last_tile = tile; dirty = 1; }
            over_chrome = 0;
        }
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
             * new one. No blur, no shadows, no full-screen copy. The core
             * maps this rectangle through the same blit as the full frame,
             * so the cursor is in the right place in every scale mode. */
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
