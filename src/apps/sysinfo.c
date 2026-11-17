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
    s->w = w; s->h = h;
    a->state = s;
}

static void sysinfo_free(App *a) {
    free(a->state);
    a->state = NULL;
}

static long read_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char k[64], unit[16];
    long v;
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3)
        if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return 0;
}

static void sysinfo_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    SysInfo *st = a->state;
    if (!st) return;
    st->w = w; st->h = h;

    Font *fh = font_get(FONT_MEDIUM, 17);
    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 11);
    if (!fh || !fr) return;

    float py = (float)y + 8;

    long total = read_kb("MemTotal:");
    long avail = read_kb("MemAvailable:");
    long used = total - avail;

    /* memory bar */
    font_draw(s, fh, (float)x + 4, py, "Memory", th->text);
    py += 26;
    float bw = (float)w - 8;
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

    /* a few real processes, from /proc/<pid>/comm */
    int shown = 0;
    for (int pid = 1; pid < 200 && shown < 6; pid++) {
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
    if (py > (float)(y + h) - 6)
        ; /* clipping is the window manager's job */
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
