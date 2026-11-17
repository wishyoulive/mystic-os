/* ============================================================
 *  Mystic OS - apps/sysinfo.c
 *  Reference app: reads /proc and draws what it finds.
 *  Kept simple on purpose - it doubles as the example other apps
 *  are written from.
 * ============================================================ */
#include "mystic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int   w, h;
    float scroll;
} SysInfo;

static void sysinfo_init(App *a, int w, int h) {
    SysInfo *s = calloc(1, sizeof(SysInfo));
    if (!s) return;                 /* app_draw/app_click see state == NULL */
    s->w = w; s->h = h;
    a->state = s;
}

static void sysinfo_free(App *a) {
    free(a->state);
    a->state = NULL;
}

/* One line at a time. The old token loop required three fields per line
 * (key, number, unit) and stopped at the first line that had fewer, so any
 * key below a two-field entry such as "HugePages_Total: 0" read as 0. */
static long read_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    size_t klen = strlen(key);
    char line[256];
    long v = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        v = strtol(line + klen, NULL, 10);
        break;
    }
    fclose(f);
    return v;
}

/* Clip to the rectangle we were given. The window manager hands over the
 * whole scene surface (main.c:434), so keeping pixels inside the app rect
 * is the app's job. Coordinates below stay absolute; shortening the
 * surface to the rect's right/bottom edge makes the bounds check inside
 * every gfx_ and font_draw call reject anything past it, and negative x/y
 * is rejected there too. */
static Surface rect_clip(Surface *s, int x, int y, int w, int h) {
    Surface c = *s;
    int right = x + w, bottom = y + h;
    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;
    if (right < c.w) c.w = right;
    if (bottom < c.h) c.h = bottom;
    return c;
}

static void sysinfo_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    SysInfo *st = a->state;
    if (!st) return;
    st->w = w; st->h = h;

    Surface clip = rect_clip(s, x, y, w, h);
    s = &clip;

    Font *fh = font_get(FONT_MEDIUM, 17);
    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 11);
    if (!fh || !fr) return;

    float py = (float)y + 8;

    long total = read_kb("MemTotal:");
    long avail = read_kb("MemAvailable:");
    long used = total - avail;
    if (used < 0) used = 0;

    /* memory bar */
    font_draw(s, fh, (float)x + 4, py, "Memory", th->text);
    py += 26;
    float bw = (float)w - 8;
    if (bw < 1.0f) bw = 1.0f;
    gfx_rrect_aa(s, (float)x + 4, py, bw, 8, 4, color_alpha(th->text, 0.10f), 0, 0);
    float frac = total > 0 ? (float)used / (float)total : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac > 0.01f)
        gfx_rrect_aa(s, (float)x + 4, py, bw * frac, 8, 4, th->accent, 0, 0);
    py += 16;
    char line[128];
    snprintf(line, sizeof(line), "%ld MB used of %ld MB", used / 1024, total / 1024);
    font_draw(s, fr, (float)x + 4, py, line, th->text_dim);
    py += 28;

    /* uptime */
    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int mins = (int)(up / 60), secs = (int)up % 60;
    snprintf(line, sizeof(line), "Up %d min %d s", mins, secs);
    font_draw(s, fr, (float)x + 4, py, line, th->text);
    py += 24;

    /* load average */
    double l1 = 0, l5 = 0, l15 = 0;
    f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) != 3) { l1 = l5 = l15 = 0; } fclose(f); }
    snprintf(line, sizeof(line), "Load %.2f  %.2f  %.2f", l1, l5, l15);
    font_draw(s, fr, (float)x + 4, py, line, th->text);
    py += 28;

    font_draw(s, fh, (float)x + 4, py, "Processes", th->text);
    py += 24;

    /* A few real processes, from /proc/<pid>/comm. Stop when the rect is
     * full instead of running the rows over the window frame: the old code
     * drew all six and then did nothing in a `if (py > y + h) ;` stub. */
    int shown = 0;
    for (int pid = 1; pid < 200 && shown < 6; pid++) {
        if (py + 14.0f > (float)(y + h)) break;     /* no room for a row */
        char path[64], name[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE *p = fopen(path, "r");
        if (!p) continue;
        if (fgets(name, sizeof(name), p)) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = 0;
            snprintf(line, sizeof(line), "%5d  %s", pid, name);
            font_draw(s, fd, (float)x + 4, py, line, th->text_dim);
            py += 16;
            shown++;
        }
        fclose(p);
    }
}

static void sysinfo_click(App *a, int x, int y, int buttons) {
    (void)a; (void)x; (void)y; (void)buttons;
}

static void sysinfo_key(App *a, const char *bytes, int n) {
    (void)a; (void)bytes; (void)n;
}

static void sysinfo_resize(App *a, int w, int h) {
    SysInfo *s = a->state;
    if (s) { s->w = w; s->h = h; }
}

const AppVT app_vt_sysinfo = {
    .id = "sysinfo",
    .title = "System",
    .init = sysinfo_init,
    .draw = sysinfo_draw,
    .key = sysinfo_key,
    .click = sysinfo_click,
    .resize = sysinfo_resize,
    .free = sysinfo_free,
};
