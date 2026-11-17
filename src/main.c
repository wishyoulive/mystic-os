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
enum { WIN_TERM = 0, WIN_APP };

typedef struct {
    int   used;
    int   kind;
    char  title[64];
    int   x, y, w, h;
    App  *app;
    Term *term;
    int   z;
} Window;

static Window wins[MAX_WINDOWS];
static int    z_top;
static int    drag_win = -1, drag_dx, drag_dy;
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

        int cx0 = cur->x + 1, cy0 = cur->y + tb + 1;
        int cw0 = cur->w - 2, ch0 = cur->h - tb - 3;

        if (cur->kind == WIN_TERM && cur->term) {
            Font *fm = font_get(FONT_REGULAR, 12);
            term_draw(cur->term, s, cx0, cy0, cw0, ch0, fm, 7.0f, 16.0f);
        } else if (cur->app) {
            app_draw(cur->app, s, cx0, cy0, cw0, ch0, th);
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
    }
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

    int quit = 0, dirty = 1;
    long last_tick = 0;

    while (!quit) {
        input_frame(&input);
        input_poll(&input);

        if (input.mouse_x < 0) input.mouse_x = 0;
        if (input.mouse_y < 0) input.mouse_y = 0;
        if (input.mouse_x >= disp.w) input.mouse_x = disp.w - 1;
        if (input.mouse_y >= disp.h) input.mouse_y = disp.h - 1;

        /* global keys */
        for (int i = 0; i < input.text_len; i++) {
            char c = input.text[i];
            if (c >= '1' && c <= '9') {
                int idx = c - '1';
                if (idx < theme_count) { theme_index = idx; dirty = 1; }
            }
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
                key_bytes(buf, &n);
                if (n) term_key(top->term, buf, n);
                if (input.text_len) term_key(top->term, input.text, input.text_len);
            } else if (top->app) {
                if (input.text_len) app_key(top->app, input.text, input.text_len);
                if (input.key == 14) app_key(top->app, "\b", 1);   /* Backspace */
                if (input.key == 28) app_key(top->app, "\n", 1);   /* Enter */
            }
        }

        /* mouse */
        if (input.clicked & 1) { if (handle_click()) dirty = 1; }
        if (input.released & 1) drag_win = -1;
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
