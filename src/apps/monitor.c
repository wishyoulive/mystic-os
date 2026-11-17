/* ============================================================
 *  Mystic OS - apps/monitor.c
 *  Live process monitor. Everything is re-read from /proc on every
 *  frame: names from /proc/<pid>/comm, memory from /proc/<pid>/statm.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC 256
#define ROW_H 15.0f

typedef struct {
    int  pid;
    char name[32];
    long kb;
} Proc;

typedef struct {
    Proc *procs;
    int   count;
    int   scroll;
    int   w, h;
    long  total_kb;
} Monitor;

static int cmp_proc(const void *a, const void *b) {
    const Proc *x = a, *y = b;
    if (x->kb != y->kb) return (y->kb > x->kb) - (y->kb < x->kb);
    return x->pid - y->pid;
}

static long meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char k[64], unit[16];
    long v;
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3)
        if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return 0;
}

static void scan(Monitor *m) {
    m->count = 0;
    DIR *d = opendir("/proc");
    if (!d) return;

    long page = sysconf(_SC_PAGESIZE) / 1024;
    if (page <= 0) page = 4;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && m->count < MAX_PROC) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;

        char path[64], name[64] = "?";
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof(name), f)) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = 0;
        }
        fclose(f);

        long kb = 0;
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        f = fopen(path, "r");
        if (f) {
            long size_pages = 0, rss_pages = 0;
            if (fscanf(f, "%ld %ld", &size_pages, &rss_pages) == 2)
                kb = rss_pages * page;
            fclose(f);
        }

        Proc *p = &m->procs[m->count++];
        p->pid = pid;
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->kb = kb;
    }
    closedir(d);

    qsort(m->procs, (size_t)m->count, sizeof(Proc), cmp_proc);
}

static void monitor_init(App *a, int w, int h) {
    Monitor *m = calloc(1, sizeof(Monitor));
    if (!m) return;
    m->procs = calloc(MAX_PROC, sizeof(Proc));
    if (!m->procs) { free(m); return; }
    m->w = w;
    m->h = h;
    a->state = m;
}

static void monitor_free(App *a) {
    Monitor *m = a->state;
    if (!m) return;
    free(m->procs);
    free(m);
    a->state = NULL;
}

static void monitor_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Monitor *m = a->state;
    if (!m) return;
    m->w = w;
    m->h = h;
    scan(m);

    Font *fh = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr) return;

    /* header: real load average and memory */
    double l1 = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &l1) != 1) l1 = 0; fclose(f); }
    long total = meminfo("MemTotal:");
    long avail = meminfo("MemAvailable:");
    long used = total - avail;
    if (used < 0) used = 0;

    char line[128];
    snprintf(line, sizeof(line), "load %.2f    %ld MB of %ld MB    %d processes",
             l1, used / 1024, total / 1024, m->count);
    font_draw(s, fh, (float)x + 4, (float)y + 4, line, th->text);
    surf_blend(s, x + 4, y + 22, w - 8, 1, th->panel_border);

    /* memory bar */
    float frac = total > 0 ? (float)used / (float)total : 0.0f;
    if (frac > 1) frac = 1;
    gfx_rrect_aa(s, (float)x + 4, (float)y + 27, (float)w - 8, 6, 3,
                 color_alpha(th->text, 0.10f), 0, 0);
    if (frac > 0.01f)
        gfx_rrect_aa(s, (float)x + 4, (float)y + 27, ((float)w - 8) * frac, 6, 3,
                     th->accent, 0, 0);

    /* column headings */
    float top = (float)y + 40;
    font_draw(s, fd, (float)x + 6,  top, "PID",  color_alpha(th->text, 0.45f));
    font_draw(s, fd, (float)x + 60, top, "NAME", color_alpha(th->text, 0.45f));
    font_draw(s, fd, (float)x + w - 70, top, "MEMORY", color_alpha(th->text, 0.45f));

    int rows = (int)((h - 60) / ROW_H);
    if (rows < 1) rows = 1;
    if (m->scroll > m->count - rows) m->scroll = m->count - rows;
    if (m->scroll < 0) m->scroll = 0;

    long maxkb = 1;
    for (int i = 0; i < m->count; i++) if (m->procs[i].kb > maxkb) maxkb = m->procs[i].kb;

    for (int i = 0; i < rows; i++) {
        int idx = m->scroll + i;
        if (idx >= m->count) break;
        Proc *p = &m->procs[idx];
        float ry = top + 16 + i * ROW_H;

        /* share bar */
        float bw = ((float)w - 8) * ((float)p->kb / (float)maxkb);
        if (bw > 1)
            gfx_rrect_aa(s, (float)x + 4, ry + 1, bw, ROW_H - 3, 3,
                         color_alpha(th->accent, 0.18f), 0, 0);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", p->pid);
        font_draw(s, fr, (float)x + 6, ry, buf, th->text_dim);

        /* truncate the name to the space available */
        char nm[32];
        snprintf(nm, sizeof(nm), "%s", p->name);
        while (font_width(fr, nm) > (float)(w - 140) && strlen(nm) > 3)
            nm[strlen(nm) - 1] = 0;
        font_draw(s, fr, (float)x + 60, ry, nm, th->text);

        snprintf(buf, sizeof(buf), "%.1f MB", p->kb / 1024.0);
        float sw = font_width(fr, buf);
        font_draw(s, fr, (float)(x + w) - sw - 6, ry, buf, th->text_dim);
    }
}

static void monitor_key(App *a, const char *bytes, int n) {
    Monitor *m = a->state;
    if (!m) return;
    for (int i = 0; i < n; i++) {
        if (bytes[i] == 'j') m->scroll++;
        else if (bytes[i] == 'k') { if (m->scroll > 0) m->scroll--; }
    }
}

static void monitor_click(App *a, int x, int y, int buttons) {
    (void)a; (void)x; (void)y; (void)buttons;
}

static void monitor_resize(App *a, int w, int h) {
    Monitor *m = a->state;
    if (m) { m->w = w; m->h = h; }
}

const AppVT app_vt_monitor = {
    .id = "monitor",
    .title = "Monitor",
    .init = monitor_init,
    .draw = monitor_draw,
    .key = monitor_key,
    .click = monitor_click,
    .resize = monitor_resize,
    .free = monitor_free,
};
