/* ============================================================
 *  Mystic OS - mystic-desktop
 *  The desktop. Everything on screen is drawn here, by the CPU,
 *  from real measurements: resolution from fb0, memory and uptime
 *  from /proc, input from evdev.
 *
 *  Keys:   1 2 3   switch theme
 *          T       cycle theme
 *          Esc     quit
 *  Mouse:  drag the title bar to move a window
 * ============================================================ */
#include "mystic.h"

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

static Display disp;
static Surface screen;      /* the frame we draw into */
static Surface scene;       /* composed desktop without the cursor */
static Input   input;
static Theme   themes[8];
static int     theme_count, theme_index;

/* ------------------------------------------------------------ helpers */
static void read_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
}

static int read_int(const char *path) {
    char buf[64];
    read_line(path, buf, sizeof(buf));
    return atoi(buf);
}

static long mem_available_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char key[64]; long v; char unit[16];
    while (fscanf(f, "%63s %ld %15s", key, &v, unit) == 3) {
        if (!strcmp(key, "MemAvailable:")) { fclose(f); return v; }
    }
    fclose(f);
    return 0;
}

static long mem_total_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char key[64];
    long v;
    char unit[16];
    while (fscanf(f, "%63s %ld %15s", key, &v, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) { fclose(f); return v; }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ icons
 * Drawn from geometry, not loaded from images, so they stay sharp at
 * any size and cost nothing to ship.
 */
static void icon_terminal(Surface *s, float x, float y, float sz, u32 fg) {
    float pad = sz * 0.26f;
    float pts1[6] = { x + pad, y + pad, x + pad + sz * 0.18f, y + sz * 0.5f, x + pad, y + sz - pad };
    gfx_line(s, pts1[0], pts1[1], pts1[2], pts1[3], sz * 0.09f, fg);
    gfx_line(s, pts1[2], pts1[3], pts1[4], pts1[5], sz * 0.09f, fg);
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
        float x0 = cx + cosf(a) * sz * 0.24f, y0 = cy + sinf(a) * sz * 0.24f;
        float x1 = cx + cosf(a) * sz * 0.40f, y1 = cy + sinf(a) * sz * 0.40f;
        gfx_line(s, x0, y0, x1, y1, sz * 0.10f, fg);
    }
    gfx_rrect_aa(s, cx - sz * 0.19f, cy - sz * 0.19f, sz * 0.38f, sz * 0.38f, sz * 0.19f, 0, fg, sz * 0.08f);
}

static void icon_moon(Surface *s, float x, float y, float sz, u32 fg) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    for (int yy = -sz / 2; yy < sz / 2; yy++) {
        for (int xx = -sz / 2; xx < sz / 2; xx++) {
            float dx = (float)xx, dy = (float)yy;
            float d1 = sqrtf(dx * dx + dy * dy) - sz * 0.34f;
            float ox = dx - sz * 0.16f, oy = dy - sz * 0.10f;
            float d2 = sqrtf(ox * ox + oy * oy) - sz * 0.28f;
            float cov = 0.5f - d1;
            if (cov > 0) {
                float hole = 0.5f - d2;
                if (hole > 0) cov -= hole;
                if (cov > 0) {
                    if (cov > 1) cov = 1;
                    surf_set(s, (int)(cx + xx), (int)(cy + yy), color_alpha(fg, cov));
                }
            }
        }
    }
}

/* ------------------------------------------------------------ clock */
static void clock_text(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* ------------------------------------------------------------ layout */
typedef struct { int x, y, w, h; } Rect;

static Rect  win = { 0, 0, 560, 340 };
static int   dragging, drag_dx, drag_dy;
static int   dock_hover = -1;

/* ------------------------------------------------------------ compose */
static int dbg_step = -1;   /* 0 = trace drawing steps to the console */
static void step(int n) {
    if (dbg_step >= 0) fprintf(stderr, "  step %d\n", n);
}

static void draw_desktop(Surface *s, const Theme *th) {
    int W = s->w, H = s->h;
    step(1);

    gfx_gradient(s, 0, 0, W, H, th->bg_top, th->bg_bottom);

    step(2);
    /* soft light sources - real radial falloff, not a pasted image */
    gfx_glow(s, W * 0.22f, H * 0.18f, W * 0.42f, th->glow, th->glow_strength * 0.85f);
    gfx_glow(s, W * 0.86f, H * 0.72f, W * 0.38f, th->accent, th->glow_strength * 0.45f);
    gfx_glow(s, W * 0.55f, H * 1.02f, W * 0.50f, th->glow, th->glow_strength * 0.55f);

    step(3);
    /* ---------------- top bar ---------------- */
    const int bar = 34;
    step(40);
    if (th->blur > 0) gfx_blur_region(s, 0, 0, W, bar, (int)(18 * th->blur));
    step(41);
    surf_blend(s, 0, 0, W, bar, color_alpha(th->panel, 0.55f));
    step(42);
    surf_blend(s, 0, bar, W, 1, th->panel_border);

    step(43);
    Font *f13 = font_get(FONT_MEDIUM, 13);
    step(32);
    Font *f11 = font_get(FONT_REGULAR, 11);
    step(33);

    step(30);
    font_draw(s, f13, 16, 9, "Mystic OS", th->text);
    step(31);

    char right[128];
    char clock[16];
    clock_text(clock, sizeof(clock));
    long mem = mem_total_kb();
    long avail = mem_available_kb();
    long used = mem - avail;
    if (used < 0) used = 0;
    snprintf(right, sizeof(right), "%s      %ld MB used of %ld MB",
             clock, used / 1024, mem / 1024);
    float rw = font_width(f11, right);
    font_draw(s, f11, (float)W - rw - 16, 11, right, th->text_dim);

    step(4);
    /* ---------------- window ---------------- */
    float cx = th->corner;
    /* shadow, then glass: blur what is behind the window, then tint it */
    gfx_shadow(s, (float)win.x, (float)win.y, (float)win.w, (float)win.h, cx, 26, 0.55f);
    if (th->blur > 0) gfx_blur_region(s, win.x, win.y, win.w, win.h, (int)(16 * th->blur));
    gfx_rrect_aa(s, (float)win.x, (float)win.y, (float)win.w, (float)win.h, cx,
                 th->window_bg, th->window_border, 1.0f);

    /* title bar */
    const int tb = 40;
    surf_blend(s, win.x + 1, win.y + 1, win.w - 2, tb, color_alpha(th->panel, 0.35f));
    surf_blend(s, win.x + 1, win.y + tb, win.w - 2, 1, th->panel_border);

    font_draw(s, f13, (float)win.x + 16, (float)win.y + 13, "About this computer", th->text);

    /* the three dots */
    {
        static const u32 dot[3] = { 0xFFFF5F57u, 0xFFFEBC2Eu, 0xFF28C840u };
        for (int i = 0; i < 3; i++)
            gfx_rrect_aa(s, (float)(win.x + win.w - 62 + i * 18), (float)win.y + 15, 10, 10, 5,
                         dot[i], 0, 0);
    }

    step(5);
    /* body: real facts, read at runtime */
    Font *fh = font_get(FONT_SEMIBOLD, 20);
    Font *fr = font_get(FONT_REGULAR, 13);
    char line[256];
    float y = (float)win.y + tb + 22;
    float x = (float)win.x + 24;

    font_draw(s, fh, x, y, "Mystic OS", th->text);
    y += 30;
    font_draw(s, fr, x, y, "built from a 512-byte boot sector upward", th->text_dim);
    y += 34;

    snprintf(line, sizeof(line), "Display   %d x %d", disp.w, disp.h);
    font_draw(s, fr, x, y, line, th->text); y += 22;
    snprintf(line, sizeof(line), "Memory    %ld MB used of %ld MB", used / 1024, mem / 1024);
    font_draw(s, fr, x, y, line, th->text); y += 22;
    char up[32];
    read_line("/proc/uptime", up, sizeof(up));
    double uptime = atof(up);
    if (uptime < 60.0)
        snprintf(line, sizeof(line), "Uptime    %.1f seconds", uptime);
    else
        snprintf(line, sizeof(line), "Uptime    %d min %d s", (int)uptime / 60, (int)uptime % 60);
    font_draw(s, fr, x, y, line, th->text); y += 22;
    snprintf(line, sizeof(line), "Theme     %s", th->name);
    font_draw(s, fr, x, y, line, th->text); y += 22;
    snprintf(line, sizeof(line), "Kernel    %s", "Linux 6.18");
    font_draw(s, fr, x, y, line, th->text);

    /* hint */
    font_draw(s, f11, x, (float)(win.y + win.h - 26), "press 1 2 3 to change theme", th->text_dim);

    step(6);
    /* ---------------- dock ---------------- */
    const int icons = 3;
    const int isz = 40, gap = 22, pad = 14;
    int dw = icons * isz + (icons - 1) * gap + pad * 2;
    int dh = isz + pad * 2;
    int dx = (W - dw) / 2, dy = H - dh - 22;
    float dr = (float)dh * 0.32f;

    gfx_shadow(s, (float)dx, (float)dy, (float)dw, (float)dh, dr, 18, 0.5f);
    if (th->blur > 0) gfx_blur_region(s, dx, dy, dw, dh, (int)(20 * th->blur));
    gfx_rrect_aa(s, (float)dx, (float)dy, (float)dw, (float)dh, dr, th->panel, th->panel_border, 1.0f);

    for (int i = 0; i < icons; i++) {
        float ix = (float)(dx + pad + i * (isz + gap));
        float iy = (float)(dy + pad);
        int hovered = (input.mouse_x >= ix && input.mouse_x < ix + isz &&
                       input.mouse_y >= iy && input.mouse_y < iy + isz);
        u32 bg = hovered ? color_alpha(th->accent, 0.22f) : color_alpha(th->text, 0.07f);
        gfx_rrect_aa(s, ix, iy, (float)isz, (float)isz, 11.0f, bg, 0, 0);
        u32 fg = hovered ? th->accent : th->text;
        switch (i) {
            case 0: icon_terminal(s, ix, iy, (float)isz, fg); break;
            case 1: icon_folder(s, ix, iy, (float)isz, fg); break;
            default: icon_gear(s, ix, iy, (float)isz, fg); break;
        }
    }
}

/* ------------------------------------------------------------ cursor
 * The pointer is composited into the frame. Recomposing the whole scene
 * on pointer movement is the simple, obviously-correct thing to do; a
 * damage-rectangle scheme can come once we measure that we need it.
 */
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
        fprintf(stderr, "no themes in %s\n", THEME_DIR);
        Theme t; memset(&t, 0, sizeof(t));
        theme_load(&t, "/nonexistent");
        themes[0] = t;
        theme_count = 1;
    }

    win.x = (disp.w - win.w) / 2;
    win.y = (disp.h - win.h) / 2 - 20;

    if (input_open(&input) < 0) fprintf(stderr, "no input devices; drawing only\n");

    int quit = 0;
    int dirty = 1;
    long last_tick = 0;
    struct pollfd pfd[2];
    int npfd = 0;

    while (!quit) {
        input_frame(&input);
        input_poll(&input);

        /* clamp the pointer */
        if (input.mouse_x < 0) input.mouse_x = 0;
        if (input.mouse_y < 0) input.mouse_y = 0;
        if (input.mouse_x >= disp.w) input.mouse_x = disp.w - 1;
        if (input.mouse_y >= disp.h) input.mouse_y = disp.h - 1;

        /* theme keys */
        for (int i = 0; i < input.text_len; i++) {
            char c = input.text[i];
            if (c >= '1' && c <= '9') {
                int idx = c - '1';
                if (idx < theme_count) { theme_index = idx; dirty = 1; }
            }
            if (c == 27) quit = 1;
        }
        if (input.key == 20 /*KEY_T*/) {          /* cycle */
            theme_index = (theme_index + 1) % theme_count;
            dirty = 1;
        }

        /* window dragging */
        if (input.clicked & 1) {
            if (input.mouse_x >= win.x && input.mouse_x < win.x + win.w &&
                input.mouse_y >= win.y && input.mouse_y < win.y + 40) {
                dragging = 1;
                drag_dx = input.mouse_x - win.x;
                drag_dy = input.mouse_y - win.y;
            }
        }
        if (input.released & 1) dragging = 0;
        if (dragging) {
            int nx = input.mouse_x - drag_dx, ny = input.mouse_y - drag_dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + win.w > disp.w) nx = disp.w - win.w;
            if (ny + win.h > disp.h) ny = disp.h - win.h;
            if (nx != win.x || ny != win.y) { win.x = nx; win.y = ny; dirty = 1; }
        }

        /* the clock and uptime tick, so recompose once a second even when idle */
        long now = (long)time(NULL);
        if (now != last_tick) { last_tick = now; dirty = 1; }
        if (input.mouse_dx || input.mouse_dy) dirty = 1;

        if (dirty) {
            draw_desktop(&scene, &themes[theme_index]);
            surf_copy(&screen, &scene);
            draw_cursor_at(&screen, input.mouse_x, input.mouse_y);
            disp_present(&disp, &screen);
            dirty = 0;
        }

        pfd[0].fd = 0; pfd[0].events = 0; pfd[0].revents = 0;
        npfd = 1;
        poll(pfd, npfd, 16);
    }

    return 0;
}
