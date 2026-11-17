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
#include "mystic.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define THEME_DIR "/etc/mystic/themes"
#define FONT_DIR  "/usr/share/fonts"
#define MAX_WINDOWS 10

static Display disp;
static Surface screen;
static Surface scene;
static Input   input;
static Theme   themes[8];
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
    int   z;
} Window;

static Window wins[MAX_WINDOWS];
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
static int    resize_win = -1;
static int    launcher_open = 0;

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

/* ------------------------------------------------------------ window mgmt */
static Window *win_top(void) {
    Window *best = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && (!best || wins[i].z > best->z)) best = &wins[i];
    return best;
}

static void win_focus(Window *w) {
    if (!w) return;
    w->z = ++z_top;
}

static void win_close(Window *w) {
    if (!w || !w->used) return;
    if (w->term) { close(term_fd(w->term)); free(w->term); }
    if (w->app) app_free(w->app);
    if (w->kind == WIN_CLIENT && w->client >= 0 && w->client < WM_MAX_CLIENTS) {
        wms_notify_close(&clients[w->client]);
        wms_shutdown_client(&clients[w->client]);
    }
    memset(w, 0, sizeof(*w));
}

static Window *win_open(int kind, const char *app_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &wins[i];
        if (w->used) continue;
        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->kind = kind;
        w->z = ++z_top;

        if (kind == WIN_TERM) {
            w->w = 640; w->h = 380;
            w->x = 60 + (i % 3) * 40;
            w->y = 70 + (i % 3) * 40;
            float cw = 7.0f, ch = 16.0f;
            int cols = (int)((w->w - 24) / cw);
            int rows = (int)((w->h - 56) / ch);
            w->term = term_create(cols, rows, "/bin/sh");
            snprintf(w->title, sizeof(w->title), "Terminal");
            if (!w->term) { memset(w, 0, sizeof(*w)); return NULL; }
        } else {
            w->w = 520; w->h = 360;
            w->x = 140 + (i % 3) * 40;
            w->y = 110 + (i % 3) * 40;
            w->app = app_new(app_id, w->w - 2, w->h - 40 - 2);
            if (!w->app) { memset(w, 0, sizeof(*w)); return NULL; }
            snprintf(w->title, sizeof(w->title), "%s", app_title(w->app));
        }
        if (w->x + w->w > disp.w) w->x = disp.w - w->w - 20;
        if (w->y + w->h > disp.h) w->y = disp.h - w->h - 90;
        return w;
    }
    return NULL;
}

/* ------------------------------------------------------------ dock */
typedef struct {
    const char *app_id;   /* NULL for terminal */
    const char *label;
    void (*icon)(Surface *, float, float, float, u32);
} DockItem;

static const DockItem DOCK[] = {
    { NULL,          "Terminal", icon_terminal },
    { "files",       "Files",    icon_folder },
    { "settings",    "Settings", icon_gear },
    { "monitor",     "Monitor",  icon_bars },
    { "notes",       "Notes",    icon_pencil },
};
#define DOCK_N ((int)(sizeof(DOCK) / sizeof(DOCK[0])))

static void dock_rect(int *dx, int *dy, int *dw, int *dh) {
    const int isz = 40, gap = 20, pad = 14;
    int w = DOCK_N * isz + (DOCK_N - 1) * gap + pad * 2;
    int h = isz + pad * 2;
    *dw = w; *dh = h;
    *dx = (disp.w - w) / 2;
    *dy = disp.h - h - 22;
}

static void draw_launcher(Surface *s, const Theme *th);
static void draw_lock_screen(Surface *s, const Theme *th);

/* ------------------------------------------------------------ compose */
static void draw_desktop(Surface *s, const Theme *th) {
    int W = s->w, H = s->h;

    gfx_gradient(s, 0, 0, W, H, th->bg_top, th->bg_bottom);
    gfx_glow(s, W * 0.22f, H * 0.18f, W * 0.42f, th->glow, th->glow_strength * 0.85f);
    gfx_glow(s, W * 0.86f, H * 0.72f, W * 0.38f, th->accent, th->glow_strength * 0.45f);
    gfx_glow(s, W * 0.55f, H * 1.02f, W * 0.50f, th->glow, th->glow_strength * 0.55f);

    /* ---------------- top bar ---------------- */
    const int bar = 34;
    if (th->blur > 0) gfx_blur_region(s, 0, 0, W, bar, (int)(18 * th->blur));
    surf_blend(s, 0, 0, W, bar, color_alpha(th->panel, 0.55f));
    surf_blend(s, 0, bar, W, 1, th->panel_border);

    Font *f13 = font_get(FONT_MEDIUM, 13);
    Font *f11 = font_get(FONT_REGULAR, 11);

    font_draw(s, f13, 16, 9, "Mystic OS", th->text);

    Window *top = win_top();
    if (top) {
        float tw = font_width(f11, top->title);
        font_draw(s, f11, (float)W * 0.5f - tw * 0.5f, 11, top->title, th->text_dim);
    }

    char right[128];
    char clock[16];
    clock_text(clock, sizeof(clock));
    long mem = mem_field("MemTotal:");
    long avail = mem_field("MemAvailable:");
    long used = mem - avail;
    if (used < 0) used = 0;
    snprintf(right, sizeof(right), "%s      %ld MB used of %ld MB", clock, used / 1024, mem / 1024);
    float rw = font_width(f11, right);
    font_draw(s, f11, (float)W - rw - 16, 11, right, th->text_dim);

    /* ---------------- windows, drawn back to front ---------------- */
    /* draw in ascending z order */
    int last_z = -1;
    for (int n = 0; n < MAX_WINDOWS; n++) {
        Window *cur = NULL;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].used) continue;
            if (wins[i].z <= last_z) continue;
            if (!cur || wins[i].z < cur->z) cur = &wins[i];
        }
        if (!cur) break;
        last_z = cur->z;

        float cx = th->corner;
        gfx_shadow(s, (float)cur->x, (float)cur->y, (float)cur->w, (float)cur->h, cx, 24, 0.5f);
        if (th->blur > 0) gfx_blur_region(s, cur->x, cur->y, cur->w, cur->h, (int)(14 * th->blur));
        gfx_rrect_aa(s, (float)cur->x, (float)cur->y, (float)cur->w, (float)cur->h, cx,
                     th->window_bg, th->window_border, 1.0f);

        const int tb = 40;
        surf_blend(s, cur->x + 1, cur->y + 1, cur->w - 2, tb, color_alpha(th->panel, 0.30f));
        surf_blend(s, cur->x + 1, cur->y + tb, cur->w - 2, 1, th->panel_border);

        /* traffic lights: red closes */
        gfx_rrect_aa(s, (float)cur->x + 14, (float)cur->y + 15, 10, 10, 5, 0xFFFF5F57u, 0, 0);
        gfx_rrect_aa(s, (float)cur->x + 32, (float)cur->y + 15, 10, 10, 5, 0xFFFEBC2Eu, 0, 0);
        gfx_rrect_aa(s, (float)cur->x + 50, (float)cur->y + 15, 10, 10, 5, 0xFF28C840u, 0, 0);

        font_draw(s, f13, (float)cur->x + 72, (float)cur->y + 13, cur->title, th->text);

        /* resize grip */
        for (int g = 0; g < 3; g++) {
            float gx = (float)(cur->x + cur->w - 6 - g * 5);
            float gy = (float)(cur->y + cur->h - 6);
            gfx_line(s, gx - 4, gy, gx, gy - 4, 1.4f, color_alpha(th->text, 0.30f));
        }

        int cx0 = cur->x + 1, cy0 = cur->y + tb + 1;
        int cw0 = cur->w - 2, ch0 = cur->h - tb - 3;

        if (cur->kind == WIN_TERM && cur->term) {
            Font *fm = font_get(FONT_REGULAR, 12);
            term_draw(cur->term, s, cx0, cy0, cw0, ch0, fm, 7.0f, 16.0f);
        } else if (cur->app) {
            app_draw(cur->app, s, cx0, cy0, cw0, ch0, th);
        } else if (cur->kind == WIN_CLIENT && cur->client >= 0) {
            WmClient *cl = &clients[cur->client];
            if (cl->used && cl->px) {
                int cw1 = cw0 < cl->w ? cw0 : cl->w;
                int ch1 = ch0 < cl->h ? ch0 : cl->h;
                for (int row = 0; row < ch1; row++)
                    memcpy(s->px + (size_t)(cy0 + row) * s->stride + cx0,
                           cl->px + (size_t)row * cl->stride, (size_t)cw1 * 4);
            }
        }
    }

    /* ---------------- dock ---------------- */
    int dx, dy, dw, dh;
    dock_rect(&dx, &dy, &dw, &dh);
    float dr = (float)dh * 0.32f;

    gfx_shadow(s, (float)dx, (float)dy, (float)dw, (float)dh, dr, 18, 0.5f);
    if (th->blur > 0) gfx_blur_region(s, dx, dy, dw, dh, (int)(20 * th->blur));
    gfx_rrect_aa(s, (float)dx, (float)dy, (float)dw, (float)dh, dr, th->panel, th->panel_border, 1.0f);

    const int isz = 40, gap = 20, pad = 14;
    for (int i = 0; i < DOCK_N; i++) {
        float ix = (float)(dx + pad + i * (isz + gap));
        float iy = (float)(dy + pad);
        int hovered = (input.mouse_x >= ix && input.mouse_x < ix + isz &&
                       input.mouse_y >= iy && input.mouse_y < iy + isz);
        u32 bg = hovered ? color_alpha(th->accent, 0.22f) : color_alpha(th->text, 0.07f);
        gfx_rrect_aa(s, ix, iy, (float)isz, (float)isz, 11.0f, bg, 0, 0);
        DOCK[i].icon(s, ix, iy, (float)isz, hovered ? th->accent : th->text);

        /* a dot under the icon when that app is open */
        int open = 0;
        for (int wi = 0; wi < MAX_WINDOWS; wi++) {
            if (!wins[wi].used) continue;
            if (DOCK[i].app_id) {
                if (wins[wi].app && wins[wi].app->vt && !strcmp(wins[wi].app->vt->id, DOCK[i].app_id))
                    open = 1;
            } else if (wins[wi].term) {
                open = 1;
            }
        }
        if (open)
            gfx_rrect_aa(s, ix + (float)isz / 2 - 2, iy + (float)isz + 3, 4, 4, 2, th->accent, 0, 0);
    }

    if (launcher_open) draw_launcher(s, th);
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
    { "sysinfo",  "System",   icon_gear },
};
#define LAUNCHER_N ((int)(sizeof(LAUNCHER) / sizeof(LAUNCHER[0])))

static int launcher_items(void) { return LAUNCHER_N + bundle_count; }

static void launcher_geom(int idx, int *tx, int *ty, int *tw, int *th) {
    const int cols = 3, tile = 128, gap = 18, grid_h = 118;
    int rows = (launcher_items() + cols - 1) / cols;
    int gw = cols * tile + (cols - 1) * gap;
    int gh = rows * grid_h + (rows - 1) * gap;
    int gx = (disp.w - gw) / 2;
    int gy = (disp.h - gh) / 2;
    int r = idx / cols, c = idx % cols;
    *tx = gx + c * (tile + gap);
    *ty = gy + r * (grid_h + gap);
    *tw = tile;
    *th = grid_h;
}

static void draw_launcher(Surface *s, const Theme *th) {
    /* dim everything behind */
    surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u);

    const int cols = 3, tile = 128, gap = 18, grid_h = 118;
    int rows = (launcher_items() + cols - 1) / cols;
    int gw = cols * tile + (cols - 1) * gap;
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
        u32 bg = hovered ? color_alpha(th->accent, 0.22f) : color_alpha(th->text, 0.06f);
        gfx_rrect_aa(s, (float)tx, (float)ty, (float)tw, (float)th2, 14.0f, bg, 0, 0);

        float isz = 52;
        const char *label;
        if (i < LAUNCHER_N) {
            LAUNCHER[i].icon(s, (float)tx + (tw - isz) / 2, (float)ty + 18, isz,
                             hovered ? th->accent : th->text);
            label = LAUNCHER[i].label;
        } else {
            /* an installed bundle: a real program from /usr/share/mystic/apps */
            int b = i - LAUNCHER_N;
            icon_gear(s, (float)tx + (tw - isz) / 2, (float)ty + 18, isz,
                      hovered ? th->accent : th->text);
            label = (b >= 0 && b < bundle_count) ? bundles[b].name : "?";
        }

        float lw = font_width(f12, label);
        font_draw(s, f12, (float)tx + (tw - lw) / 2, (float)ty + th2 - 30,
                  label, hovered ? th->text : th->text_dim);
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

    int pw = 380, ph = 210;
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
    if (line[0] && mystic_check_password(line, pin)) {
        locked = 0;
        snprintf(lock_msg, sizeof(lock_msg), "%s", "");
    } else {
        snprintf(lock_msg, sizeof(lock_msg), "wrong password");
    }
    pin_len = 0;
    pin[0] = 0;
}

/* ------------------------------------------------------------ cursor */
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
static void key_bytes(char *out, int *n) {
    *n = 0;
    switch (input.key) {
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

static int handle_click(void) {
    int mx = input.mouse_x, my = input.mouse_y;

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
        if (!w->used) continue;
        if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h) continue;
        if (!hit || w->z > hit->z) hit = w;
    }
    if (hit) {
        win_focus(hit);
        int tb = 40;
        int lx = mx - hit->x, ly = my - hit->y;
        /* close dot */
        if (ly < tb && lx >= 14 && lx < 24) { win_close(hit); return 1; }
        /* bottom-right corner resizes */
        if (lx >= hit->w - 18 && ly >= hit->h - 18) {
            resize_win = (int)(hit - wins);
            return 1;
        }
        /* title bar = drag */
        if (ly < tb) {
            drag_win = (int)(hit - wins);
            drag_dx = lx;
            drag_dy = ly;
            return 1;
        }
        /* body */
        if (hit->app) app_click(hit->app, lx - 1, ly - tb - 1, input.buttons);
        return 1;
    }

    /* dock */
    int dx, dy, dw, dh;
    dock_rect(&dx, &dy, &dw, &dh);
    const int isz = 40, gap = 20, pad = 14;
    if (my >= dy && my < dy + dh) {
        for (int i = 0; i < DOCK_N; i++) {
            int ix = dx + pad + i * (isz + gap);
            if (mx >= ix && mx < ix + isz) {
                if (DOCK[i].app_id) win_open(WIN_APP, DOCK[i].app_id);
                else                win_open(WIN_TERM, NULL);
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------ main */
int main(void) {
    if (disp_open(&disp, "/dev/fb0") < 0) return 1;
    font_init(FONT_DIR);

    screen.w = disp.w; screen.h = disp.h; screen.stride = disp.w;
    screen.px = calloc((size_t)screen.w * screen.h, 4);
    scene = screen;
    scene.px = calloc((size_t)screen.w * screen.h, 4);
    if (!screen.px || !scene.px) { fprintf(stderr, "out of memory\n"); return 1; }

    theme_count = theme_load_all(themes, 8, THEME_DIR);
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
            dirty = 1;
            if (locked) {
                if (dirty) {
                    draw_desktop(&scene, &themes[theme_index]);
                    surf_copy(&screen, &scene);
                    draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
                    disp_present(&disp, &screen);
                    dirty = 0;
                }
                struct pollfd lp = { 0, 0, 0 };
                poll(&lp, 1, 16);
                continue;
            }
        }

        /* global keys */
        for (int i = 0; i < input.text_len; i++) {
            char c = input.text[i];
            if (c >= '1' && c <= '9') {
                int idx = c - '1';
                if (idx < theme_count) { theme_index = idx; dirty = 1; }
            }
        }
        if (input.key == 125) {                 /* Super toggles the launcher */
            launcher_open = !launcher_open;
            dirty = 1;
        } else if (launcher_open && input.key == 1) {   /* Esc closes it */
            launcher_open = 0;
            dirty = 1;
        }

        /* Alt+Tab cycles the window stack */
        if (input.alt && input.key == 15 && z_top > 0) {          /* KEY_TAB */
            Window *cur = win_top();
            Window *next = NULL;
            if (cur) {
                int best = -1;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (!wins[i].used || &wins[i] == cur) continue;
                    if (wins[i].z > best) { best = wins[i].z; next = &wins[i]; }
                }
                if (!next) {                      /* only one window: nothing to cycle */
                    next = NULL;
                }
            }
            if (next) { win_focus(next); dirty = 1; }
        }

        /* F1..F5 launch, so apps are reachable without a mouse */
        switch (input.key) {
            case 59: win_open(WIN_TERM, NULL); dirty = 1; break;   /* F1 terminal */
            case 60: win_open(WIN_APP, "files"); dirty = 1; break;
            case 61: win_open(WIN_APP, "settings"); dirty = 1; break;
            case 62: win_open(WIN_APP, "monitor"); dirty = 1; break;
            case 63: win_open(WIN_APP, "notes"); dirty = 1; break;
            default: break;
        }

        if (input.key == 1) {                 /* Esc */
            Window *t = win_top();
            if (t) { win_close(t); dirty = 1; }
        }

        /* focused window gets keys */
        Window *top = win_top();
        if (top) {
            if (top->term) {
                char buf[64];
                int n = 0;
                if (input.shift && (input.key == 104 || input.key == 109)) {
                    term_scroll(top->term, input.key == 104 ? 8 : -8);
                    dirty = 1;
                } else {
                    key_bytes(buf, &n);
                    if (n) term_key(top->term, buf, n);
                }
                if (input.text_len) {
                    term_scroll(top->term, -(term_scroll_offset(top->term)));
                    term_key(top->term, input.text, input.text_len);
                }
            } else if (top->kind == WIN_CLIENT && top->client >= 0) {
                WmClient *cl = &clients[top->client];
                int mods = (input.shift ? 1 : 0) | (input.ctrl ? 2 : 0) | (input.alt ? 4 : 0);
                if (input.key) wms_notify_key(cl, input.key, mods, 1);
                if (input.text_len) wms_notify_text(cl, input.text);
                char kb[64]; int kn = 0;
                key_bytes(kb, &kn);
                if (kn) wms_notify_text(cl, kb);
                dirty = 1;
            } else if (top->app) {
                if (input.text_len) app_key(top->app, input.text, input.text_len);
                /* Arrows, Home/End, PageUp/Down and Delete must reach apps as
                 * escape sequences or none of their key handling can run.
                 * (Found by the files and notes workers during review; the
                 * Enter/Backspace forwarding that used to live here duplicated
                 * what input.text already carries.) */
                char kb[64];
                int kn = 0;
                key_bytes(kb, &kn);
                if (kn) app_key(top->app, kb, kn);
                if (input.text_len || kn) dirty = 1;   /* or the repaint lags a second */
            }
        }

        /* mouse */
        if (input.wheel) {
            Window *under = NULL;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                Window *w = &wins[i];
                if (!w->used || !w->term) continue;
                if (input.mouse_x < w->x || input.mouse_x >= w->x + w->w) continue;
                if (input.mouse_y < w->y || input.mouse_y >= w->y + w->h) continue;
                if (!under || w->z > under->z) under = w;
            }
            if (under) { term_scroll(under->term, input.wheel * 3); dirty = 1; }
        }
        if (input.clicked & 1) { if (handle_click()) dirty = 1; }
        if (input.released & 1) {
            if (drag_win >= 0 && wins[drag_win].used) {
                Window *w = &wins[drag_win];
                const int bar = 34;
                if (w->y <= bar + 6) {                       /* top edge: full width */
                    w->x = 0; w->y = bar; w->w = disp.w; w->h = disp.h - bar - 90;
                } else if (w->x <= 8) {                      /* left edge: left half */
                    w->x = 0; w->y = bar; w->w = disp.w / 2; w->h = disp.h - bar - 90;
                } else if (w->x + w->w >= disp.w - 8) {      /* right edge: right half */
                    w->w = disp.w / 2; w->x = disp.w - w->w;
                    w->y = bar; w->h = disp.h - bar - 90;
                }
                if (w->app && w->app->vt && w->app->vt->resize)
                    w->app->vt->resize(w->app, w->w - 2, w->h - 42);
                dirty = 1;
            }
            drag_win = -1;
            resize_win = -1;
        }
        if ((input.buttons & 1) && resize_win >= 0) {
            Window *w = &wins[resize_win];
            if (w->used) {
                int nw = input.mouse_x - w->x + 1;
                int nh = input.mouse_y - w->y + 1;
                if (nw < 280) nw = 280;
                if (nh < 160) nh = 160;
                if (nw != w->w || nh != w->h) {
                    w->w = nw;
                    w->h = nh;
                    if (w->app && w->app->vt && w->app->vt->resize)
                        w->app->vt->resize(w->app, w->w - 2, w->h - 42);
                    dirty = 1;
                }
            }
        }
        if ((input.buttons & 1) && drag_win >= 0) {
            Window *w = &wins[drag_win];
            if (w->used) {
                int nx = input.mouse_x - drag_dx, ny = input.mouse_y - drag_dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 34;
                if (nx + w->w > disp.w) nx = disp.w - w->w;
                if (ny + w->h > disp.h) ny = disp.h - w->h;
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; dirty = 1; }
            }
        }
        (void)mouse_prev;

        /* window server: accept connections, bind windows, mark frames dirty */
        wms_poll(clients, WM_MAX_CLIENTS);
        for (int i = 0; i < WM_MAX_CLIENTS; i++) {
            WmClient *cl = &clients[i];
            if (!cl->used) continue;
            if (cl->win < 0 && cl->px) {
                Window *w = NULL;
                for (int k = 0; k < MAX_WINDOWS; k++) if (!wins[k].used) { w = &wins[k]; break; }
                if (w) {
                    memset(w, 0, sizeof(*w));
                    w->used = 1;
                    w->kind = WIN_CLIENT;
                    w->client = i;
                    w->w = cl->w + 2;
                    w->h = cl->h + 42;
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
                term_pump(wins[i].term);
                dirty = 1;            /* keep it simple while terminals exist */
                if (!term_alive(wins[i].term)) { win_close(&wins[i]); }
            }
        }

        long now = (long)time(NULL);
        if (now != last_tick) {
            last_tick = now;
            dirty = 1;
            /* another app can ask for a theme by writing this file */
            FILE *tf = fopen("/etc/mystic/current-theme", "r");
            if (tf) {
                char want[64] = {0};
                if (fgets(want, sizeof(want), tf)) {
                    char *nl = strchr(want, '\n');
                    if (nl) *nl = 0;
                    for (int i = 0; i < theme_count; i++)
                        if (!strcmp(want, themes[i].name)) theme_index = i;
                }
                fclose(tf);
            }
        }
        if (input.mouse_dx || input.mouse_dy) dirty = 1;

        if (dirty) {
            draw_desktop(&scene, &themes[theme_index]);
            surf_copy(&screen, &scene);
            draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
            disp_present(&disp, &screen);
            dirty = 0;
        }

        struct pollfd pfd[2];
        pfd[0].fd = 0; pfd[0].events = 0; pfd[0].revents = 0;
        poll(pfd, 1, 16);
    }
    return 0;
}
