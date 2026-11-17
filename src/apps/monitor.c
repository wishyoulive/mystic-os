/* ============================================================
 *  Mystic OS - apps/monitor.c
 *  Live process monitor. Everything is re-read from /proc:
 *
 *    /proc/<pid>/stat     field 2 (comm), fields 14/15 (utime, stime)
 *    /proc/<pid>/statm    resident set size
 *    /proc/meminfo        MemTotal / MemAvailable
 *    /proc/loadavg        load average, and field 4 ("1/57") for threads
 *    /proc/uptime         system uptime, shown in the header
 *
 *  CPU use is a real delta: the jiffies a process burned between two
 *  samples divided by the wall time between those samples, measured
 *  with CLOCK_MONOTONIC. That makes it a percentage of one core, so a
 *  threaded process can legitimately read above 100%. A process that
 *  appears after a sample has no previous ticks yet and reads 0.0%.
 *
 *  Keys: j/k move the selection, s cycles memory -> cpu -> pid,
 *  shift+K (or x) sends SIGTERM to the selected pid. Plain 'k' stays
 *  the cursor move - that is the vim order the other apps use - which
 *  is why the kill takes the shift. The wheel walks the same cursor
 *  three rows a notch, so the list scrolls under it.
 *
 *  The whole window is one column grid: every line starts on the left
 *  margin, every number ends on the right edge of its own column, and each
 *  column is as wide as the widest string it has to hold, measured from the
 *  real font. The PID/CPU/MEM headings are right-aligned on exactly the
 *  edges their values end on, so the header cannot drift from the numbers
 *  below it at any text scale. Vertical bands come from font_height() /
 *  font_ascent() plus ui_px(); a label is centred on its own ink box.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROC 256
#define SAMPLE_S 0.35    /* shortest gap between two CPU samples */

/* ------------------------------------------------------------
 *  Geometry, derived from the fonts at draw time
 *
 *  font_draw() takes y as the ascender line and puts the baseline
 *  font_ascent() below it (src/font.c:229); stbtt_ScaleForPixelHeight()
 *  makes ascent + descent exactly the nominal size (src/font.c:119) while
 *  font_height() is size * 1.4, so one line of text covers font_height/1.4
 *  from y. Centring that ink box is what puts a label in the middle of its
 *  band at every text scale.
 *
 *  Every band below is a font line box plus ui_px() air, and every column
 *  edge is measured from the widest string that column has to hold, so the
 *  headings and the numbers under them share an edge at any scale. There is
 *  no fixed pixel height or column width left in this file.
 * ------------------------------------------------------------ */
static float ink_h(Font *f) { return font_height(f) / 1.4f; }

/* Baseline of one line centred in a band, and the y font_draw() needs for a
 * line on a given baseline. */
static float base_y(Font *f, float top, float band_h) {
    return top + (band_h - ink_h(f)) * 0.5f + font_ascent(f);
}
static float top_y(Font *f, float base) { return base - font_ascent(f); }

typedef struct {
    float title_y, title_h;   /* "sort: memory" band, from the top edge   */
    float rule_y;             /* the 1 px rule under the title            */
    float bar_y, bar_h;       /* the memory bar (a graphic)               */
    float sub_y, sub_h;       /* load / uptime / memory line              */
    float col_y, col_h;       /* PID NAME CPU MEM headings                */
    float body_y;             /* first process row, app-local             */
    float row_h;              /* one process row                          */
    float foot_h;             /* key legend band at the bottom            */
    float m;                  /* the one left/right margin                */
    float pid_r;              /* right edge of the PID column             */
    float name_x;             /* left edge of the NAME column             */
    float cpu_r;              /* right edge of the CPU column             */
    float mem_r;              /* right edge of the MEM column             */
    float mem_w, cpu_w;       /* widths the columns reserve               */
    int   have_cpu;           /* 0 when the window is too narrow for CPU  */
} Geo;

static Geo geo_of(int x, int w, Font *fh, Font *fr, Font *fd) {
    Geo g;
    g.m       = (float)ui_px(12);
    g.title_y = (float)ui_px(8);
    g.title_h = font_height(fh) + (float)ui_px(6);
    g.rule_y  = g.title_y + g.title_h;
    g.bar_h   = font_height(fr) * 0.45f;
    if (g.bar_h < 3.0f) g.bar_h = 3.0f;
    g.bar_y   = g.rule_y + (float)ui_px(6);
    g.sub_h   = font_height(fd) + (float)ui_px(4);
    g.sub_y   = g.bar_y + g.bar_h + (float)ui_px(4);
    g.col_h   = font_height(fd) + (float)ui_px(4);
    g.col_y   = g.sub_y + g.sub_h;
    g.row_h   = font_height(fr) + (float)ui_px(8);
    g.body_y  = g.col_y + g.col_h;
    g.foot_h  = font_height(fd) + (float)ui_px(6);

    /* column edges: each one measured from the widest string it holds, so a
     * bigger text scale widens the column instead of overlapping it */
    g.mem_w  = font_width(fr, "9999.9 MB");
    g.cpu_w  = font_width(fr, "999.9%");
    g.mem_r  = (float)(x + w) - g.m;
    g.pid_r  = (float)x + g.m + font_width(fr, "000000");
    g.name_x = g.pid_r + (float)ui_px(10);
    g.cpu_r  = g.mem_r - g.mem_w - (float)ui_px(10);
    g.have_cpu = (g.cpu_r - g.cpu_w - (float)ui_px(8)) - g.name_x >= (float)ui_px(28);
    if (!g.have_cpu) g.cpu_r = g.mem_r - g.mem_w - (float)ui_px(10);
    return g;
}

static int visible_rows(float h, const Geo *g) {
    int rows = (int)((h - g->body_y - g->foot_h) / g->row_h);
    return rows < 1 ? 1 : rows;
}

enum { SORT_MEM, SORT_CPU, SORT_PID };

typedef struct {
    int    pid;
    char   name[32];
    long   kb;
    double cpu;                    /* % of one core over the last sample */
} Proc;

/* One CPU sample: the tick count and the value we published from it. */
typedef struct {
    int                pid;
    unsigned long long ticks;      /* utime + stime, in clock ticks */
    double             cpu;
} Sample;

typedef struct {
    Proc   procs[MAX_PROC];
    Sample snap[MAX_PROC];         /* previous sample, read for deltas */
    Sample next[MAX_PROC];         /* the sample being taken now */
    int    count;
    int    seen;                   /* pids in /proc, including ones not stored */
    int    snap_count;
    double snap_t;                 /* CLOCK_MONOTONIC time of the sample */
    int    sort;
    int    sel;
    int    scroll;
    int    rows;
    int    w, h;
    char   msg[96];
    double msg_t;
    int    msg_ok;
} Monitor;

static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *sort_name(int mode) {
    if (mode == SORT_CPU) return "cpu";
    if (mode == SORT_PID) return "pid";
    return "memory";
}

/* ------------------------------------------------------------
 *  /proc readers
 * ------------------------------------------------------------ */

/* utime is field 14 and stime field 15 of /proc/<pid>/stat. Field 2
 * is (comm) and may itself contain spaces and brackets, so the fields
 * are counted from the last ')'. Returns 0 on success. */
static int parse_stat(const char *buf, char *name, int namesz,
                      unsigned long long *ticks) {
    char *open_p = strchr(buf, '(');
    char *close_p = strrchr(buf, ')');
    if (!open_p || !close_p || close_p <= open_p) return -1;

    if (name && namesz > 1) {
        size_t len = (size_t)(close_p - open_p - 1);
        if (len > (size_t)namesz - 1) len = (size_t)namesz - 1;
        memcpy(name, open_p + 1, len);
        name[len] = 0;
    }
    if (!ticks) return 0;

    unsigned long long utime = 0, stime = 0;
    int field = 2;                 /* field 2 is the name just parsed */
    char *p = close_p + 1;
    while (*p && field < 15) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (++field == 3) {        /* field 3 is the state letter */
            while (*p && *p != ' ') p++;
            continue;
        }
        char *end;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) {            /* not a number: step over the token */
            while (*p && *p != ' ') p++;
            continue;
        }
        if (field == 14) utime = v;
        else if (field == 15) stime = v;
        p = end;
    }
    *ticks = utime + stime;
    return 0;
}

static int stat_sample(int pid, char *name, int namesz, unsigned long long *ticks) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return parse_stat(buf, name, namesz, ticks);
}

static void meminfo_read(long *total_kb, long *avail_kb) {
    *total_kb = 0;
    *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))
            *total_kb = strtol(line + 9, NULL, 10);
        else if (!strncmp(line, "MemAvailable:", 13))
            *avail_kb = strtol(line + 13, NULL, 10);
        if (*total_kb && *avail_kb) break;
    }
    fclose(f);
}

/* field 4 of /proc/loadavg is "running/threads", e.g. "1/57". */
static void loadavg_read(double *l1, int *threads) {
    *l1 = 0.0;
    *threads = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        double a = 0, b = 0, c = 0;
        int thr = 0;
        /* field 4 is "running/threads"; we want the total */
        if (sscanf(line, "%lf %lf %lf %*d/%d", &a, &b, &c, &thr) == 4) {
            *l1 = a;
            *threads = thr;
        }
    }
    fclose(f);
}

static double uptime_read(void) {
    double up = 0.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return 0.0;
    if (fscanf(f, "%lf", &up) != 1) up = 0.0;
    fclose(f);
    return up;
}

static void fmt_uptime(double secs, char *out, size_t n) {
    unsigned long s = (unsigned long)(secs > 0.0 ? secs : 0.0);
    unsigned long d  = s / 86400UL;
    unsigned long hh = (s % 86400UL) / 3600UL;
    unsigned long mm = (s % 3600UL) / 60UL;
    unsigned long ss = s % 60UL;
    if (d)
        snprintf(out, n, "%lud %02lu:%02lu", d, hh, mm);
    else
        snprintf(out, n, "%02lu:%02lu:%02lu", hh, mm, ss);
}

static int find_sample(const Sample *arr, int n, int pid) {
    for (int i = 0; i < n; i++)
        if (arr[i].pid == pid) return i;
    return -1;
}

/* ------------------------------------------------------------
 *  Sorting
 * ------------------------------------------------------------ */
static int better(const Proc *x, const Proc *y, int mode) {
    if (mode == SORT_CPU) {
        if (x->cpu > y->cpu) return 1;
        if (x->cpu < y->cpu) return 0;
    } else if (mode == SORT_MEM) {
        if (x->kb > y->kb) return 1;
        if (x->kb < y->kb) return 0;
    }
    return x->pid < y->pid;        /* pid ascending breaks every tie */
}

static void sort_procs(Monitor *m) {
    for (int i = 1; i < m->count; i++) {
        Proc key = m->procs[i];
        int j = i - 1;
        while (j >= 0 && better(&key, &m->procs[j], m->sort)) {
            m->procs[j + 1] = m->procs[j];
            j--;
        }
        m->procs[j + 1] = key;
    }
}

/* ------------------------------------------------------------
 *  Scan
 * ------------------------------------------------------------ */
static void scan(Monitor *m) {
    double t = mono_now();
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    /* Resample when enough wall time has passed, otherwise republish
     * the value from the last sample - a burst of redraws must not
     * produce a jiffy-quantised 0% or 400%. */
    int resample = (m->snap_t <= 0.0) || (t - m->snap_t >= SAMPLE_S);
    double dt = t - m->snap_t;

    m->count = 0;
    m->seen = 0;
    int nn = 0;

    DIR *d = opendir("/proc");
    if (!d) return;

    long page = sysconf(_SC_PAGESIZE) / 1024;
    if (page <= 0) page = 4;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        m->seen++;                     /* every pid, stored or not */
        if (m->count >= MAX_PROC) continue;   /* list full: keep counting */

        char name[64] = "?";
        unsigned long long ticks = 0;
        if (stat_sample(pid, name, (int)sizeof(name), &ticks) != 0)
            continue;              /* the process died mid-scan */

        long kb = 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        FILE *f = fopen(path, "r");
        if (f) {
            long size_pages = 0, rss_pages = 0;
            if (fscanf(f, "%ld %ld", &size_pages, &rss_pages) == 2)
                kb = rss_pages * page;
            fclose(f);
        }

        double cpu = 0.0;
        int old = find_sample(m->snap, m->snap_count, pid);
        if (resample) {
            if (old >= 0 && dt > 0.001 && ticks >= m->snap[old].ticks) {
                double dtick = (double)(ticks - m->snap[old].ticks);
                cpu = 100.0 * (dtick / (double)hz) / dt;
            }
        } else if (old >= 0) {
            cpu = m->snap[old].cpu;
        }
        if (cpu < 0.0) cpu = 0.0;
        if (cpu > 999.0) cpu = 999.0;   /* keep the column readable */

        Proc *p = &m->procs[m->count++];
        p->pid = pid;
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->kb = kb;
        p->cpu = cpu;

        if (nn < MAX_PROC) {
            m->next[nn].pid = pid;
            m->next[nn].ticks = ticks;
            m->next[nn].cpu = cpu;
            nn++;
        }
    }
    closedir(d);

    if (resample) {
        for (int i = 0; i < nn; i++) m->snap[i] = m->next[i];
        m->snap_count = nn;
        m->snap_t = t;
    }
}

/* ------------------------------------------------------------
 *  Actions
 * ------------------------------------------------------------ */
static void kill_selected(Monitor *m) {
    if (m->sel < 0 || m->sel >= m->count) return;
    Proc *p = &m->procs[m->sel];
    int me = (int)getpid();

    if (p->pid <= 1) {
        snprintf(m->msg, sizeof(m->msg), "refused: pid %d is init", p->pid);
        m->msg_ok = 0;
    } else if (p->pid == me) {
        snprintf(m->msg, sizeof(m->msg), "refused: pid %d is me", p->pid);
        m->msg_ok = 0;
    } else if (kill(p->pid, SIGTERM) == 0) {
        snprintf(m->msg, sizeof(m->msg), "SIGTERM -> %d %s", p->pid, p->name);
        m->msg_ok = 1;
    } else {
        snprintf(m->msg, sizeof(m->msg), "SIGTERM to %d failed: %s",
                 p->pid, strerror(errno));
        m->msg_ok = 0;
    }
    m->msg_t = mono_now();
}

/* ------------------------------------------------------------
 *  App callbacks
 * ------------------------------------------------------------ */
static void monitor_init(App *a, int w, int h) {
    Monitor *m = calloc(1, sizeof(Monitor));
    if (!m) return;
    m->w = w;
    m->h = h;
    m->sort = SORT_MEM;
    a->state = m;
}

static void monitor_free(App *a) {
    free(a->state);
    a->state = NULL;
}

static void monitor_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Monitor *m = a->state;
    if (!m) return;
    m->w = w;
    m->h = h;

    /* Keep every pixel inside the rect: the WM hands over the whole scene
     * surface (main.c:434). The rows already derive from h and the fonts,
     * so this only bites when the rect is shorter than the header plus the
     * legend, where visible_rows() clamps to 1 and that row would otherwise
     * land past the bottom edge. */
    Surface clip = *s;
    int clip_r = x + w, clip_b = y + h;
    if (clip_r < 0) clip_r = 0;
    if (clip_b < 0) clip_b = 0;
    if (clip_r < clip.w) clip.w = clip_r;
    if (clip_b < clip.h) clip.h = clip_b;
    s = &clip;

    scan(m);
    sort_procs(m);

    Font *fh = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr || !fd) return;

    Geo geo = geo_of(x, w, fh, fr, fd);
    const float dx = (float)x, dy = (float)y;

    double l1;
    int threads;
    loadavg_read(&l1, &threads);

    long total_kb, avail_kb;
    meminfo_read(&total_kb, &avail_kb);
    long used_kb = total_kb - avail_kb;
    if (used_kb < 0) used_kb = 0;

    double up = uptime_read();
    char upbuf[32];
    fmt_uptime(up, upbuf, sizeof(upbuf));

    /* row window first: the cursor and scroll depend on it */
    int rows = visible_rows((float)h, &geo);
    m->rows = rows;

    if (m->sel >= m->count) m->sel = m->count - 1;
    if (m->sel < 0) m->sel = 0;
    if (m->sel < m->scroll) m->scroll = m->sel;
    if (m->sel >= m->scroll + rows) m->scroll = m->sel - rows + 1;
    if (m->scroll > m->count - rows) m->scroll = m->count - rows;
    if (m->scroll < 0) m->scroll = 0;

    /* ---- header: sort mode on the left, totals on the right ---- */
    char head[64], right[96];
    snprintf(head, sizeof(head), "sort: %s", sort_name(m->sort));
    font_draw(s, fh, dx + geo.m, top_y(fh, base_y(fh, dy + geo.title_y, geo.title_h)),
              head, th->text);

    /* The list holds at most MAX_PROC; say so instead of silently showing
     * a smaller "N proc" than /proc really has. */
    if (m->seen > m->count)
        snprintf(right, sizeof(right), "%d/%d proc   %d thr",
                 m->count, m->seen, threads);
    else
        snprintf(right, sizeof(right), "%d proc   %d thr", m->count, threads);
    {
        float rw = font_width(fd, right);
        if (font_width(fh, head) + rw + 4.0f * geo.m < (float)w)
            font_draw(s, fd, dx + (float)w - geo.m - rw,
                      top_y(fd, base_y(fd, dy + geo.title_y, geo.title_h)), right,
                      color_alpha(th->text, 0.55f));
    }

    surf_blend(s, x + (int)geo.m, (int)(dy + geo.rule_y),
               w - 2 * (int)geo.m, 1, th->panel_border);

    /* ---- memory bar: the same two margins as the text around it ---- */
    float frac = total_kb > 0 ? (float)used_kb / (float)total_kb : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    {
        float bx = dx + geo.m, bw = (float)w - 2.0f * geo.m;
        if (bw > 1.0f) {
            gfx_rrect_aa(s, bx, dy + geo.bar_y, bw, geo.bar_h, geo.bar_h * 0.5f,
                         color_alpha(th->text, 0.10f), 0, 0);
            if (frac > 0.01f)
                gfx_rrect_aa(s, bx, dy + geo.bar_y, bw * frac, geo.bar_h,
                             geo.bar_h * 0.5f, th->accent, 0, 0);
        }
    }

    /* ---- second header line: load, uptime, memory, last action ---- */
    char left[96];
    snprintf(left, sizeof(left), "load %.2f   up %s", l1, upbuf);
    font_draw(s, fd, dx + geo.m, top_y(fd, base_y(fd, dy + geo.sub_y, geo.sub_h)),
              left, th->text_dim);

    double now = mono_now();
    char note[96];
    note[0] = 0;
    u32 note_col = th->text_dim;
    if (m->msg[0] && now - m->msg_t < 5.0) {
        snprintf(note, sizeof(note), "%s", m->msg);
        note_col = m->msg_ok ? th->accent : color_alpha(th->text, 0.55f);
    } else if (m->msg[0]) {
        m->msg[0] = 0;
    } else if (total_kb > 0) {
        snprintf(note, sizeof(note), "mem %ld MB / %ld MB  %d%%",
                 used_kb / 1024, total_kb / 1024, (int)(frac * 100.0f + 0.5f));
    }
    if (note[0]) {
        float nw = font_width(fd, note);
        if (font_width(fd, left) + nw + 4.0f * geo.m < (float)w)
            font_draw(s, fd, dx + (float)w - geo.m - nw,
                      top_y(fd, base_y(fd, dy + geo.sub_y, geo.sub_h)), note, note_col);
    }

    /* ---- column headings, each on the edge its values right-align to ---- */
    {
        float chb = base_y(fd, dy + geo.col_y, geo.col_h);   /* shared baseline */
        float cy = top_y(fd, chb);
        const char *chpid = "PID", *chname = "NAME", *chcpu = "CPU", *chmem = "MEM";
        u32 hc = color_alpha(th->text, 0.45f);
        font_draw(s, fd, geo.pid_r - font_width(fd, chpid), cy, chpid, hc);
        font_draw(s, fd, geo.name_x, cy, chname, hc);
        if (geo.have_cpu)
            font_draw(s, fd, geo.cpu_r - font_width(fd, chcpu), cy, chcpu, hc);
        font_draw(s, fd, geo.mem_r - font_width(fd, chmem), cy, chmem, hc);
    }

    /* ---- rows ---- */
    float name_max = (geo.have_cpu ? geo.cpu_r - geo.cpu_w : geo.cpu_r)
                   - (float)ui_px(8) - geo.name_x;
    if (name_max < (float)ui_px(24)) name_max = (float)ui_px(24);

    double vmax = 1.0;
    for (int i = 0; i < m->count; i++) {
        double v = m->sort == SORT_CPU ? m->procs[i].cpu : (double)m->procs[i].kb;
        if (v > vmax) vmax = v;
    }

    float bar_x = dx + geo.m, bar_w = (float)w - 2.0f * geo.m;

    for (int i = 0; i < rows; i++) {
        int idx = m->scroll + i;
        if (idx >= m->count) break;
        Proc *p = &m->procs[idx];
        float ry = dy + geo.body_y + (float)i * geo.row_h;
        int sel = (idx == m->sel);
        float ty = top_y(fr, base_y(fr, ry, geo.row_h));

        /* the highlight is the full width of the window, and the cursor mark
         * sits in the same left margin every line of this window starts on */
        if (sel) {
            surf_blend(s, x, (int)(ry + 0.5f), w, (int)(geo.row_h + 0.5f),
                       color_alpha(th->accent, 0.20f));
            surf_blend(s, x, (int)(ry + 1.0f), (int)geo.m / 3 + 1,
                       (int)(geo.row_h - 1.5f), color_alpha(th->accent, 0.85f));
        }

        /* share bar of whatever the current sort keys on, behind the row */
        double v = m->sort == SORT_CPU ? p->cpu : (double)p->kb;
        float bw = bar_w * (float)(v / vmax);
        if (bw > 2.0f)
            gfx_rrect_aa(s, bar_x, ry + (float)ui_px(1), bw,
                         geo.row_h - 2.0f * (float)ui_px(1), 2,
                         color_alpha(th->accent, 0.13f), 0, 0);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", p->pid);
        font_draw(s, fr, geo.pid_r - font_width(fr, buf), ty, buf,
                  sel ? th->text : th->text_dim);

        char nm[32];
        snprintf(nm, sizeof(nm), "%s", p->name);
        while (strlen(nm) > 3 && font_width(fr, nm) > name_max)
            nm[strlen(nm) - 1] = 0;
        font_draw(s, fr, geo.name_x, ty, nm, th->text);

        if (geo.have_cpu) {
            snprintf(buf, sizeof(buf), "%.1f%%", p->cpu);
            font_draw(s, fr, geo.cpu_r - font_width(fr, buf), ty, buf,
                      sel ? th->text : th->text_dim);
        }

        snprintf(buf, sizeof(buf), "%.1f MB", p->kb / 1024.0);
        font_draw(s, fr, geo.mem_r - font_width(fr, buf), ty, buf,
                  sel ? th->text : th->text_dim);
    }

    /* ---- key legend, on the same margin as everything else ---- */
    if ((float)h - geo.foot_h > geo.body_y) {
        float fy = (float)h - geo.foot_h;
        font_draw(s, fd, dx + geo.m, top_y(fd, base_y(fd, dy + fy, geo.foot_h)),
                  "s sort   j/k move   shift+K kill", color_alpha(th->text, 0.40f));
    }
}

static void monitor_key(App *a, const char *bytes, int n) {
    Monitor *m = a->state;
    if (!m) return;
    for (int i = 0; i < n; i++) {
        char c = bytes[i];
        if (c == 'j') {
            if (m->sel + 1 < m->count) m->sel++;
        } else if (c == 'k') {
            if (m->sel > 0) m->sel--;
        } else if (c == 's') {
            /* re-sort now, not on the next draw, so the cursor stays on the
             * same process and a key pressed right after this one still
             * moves the cursor where the user expects */
            int keep = (m->sel >= 0 && m->sel < m->count)
                           ? m->procs[m->sel].pid : 0;
            m->sort = (m->sort + 1) % 3;
            sort_procs(m);
            if (keep > 0)
                for (int idx = 0; idx < m->count; idx++)
                    if (m->procs[idx].pid == keep) { m->sel = idx; break; }
        } else if (c == 'K' || c == 'x') {
            kill_selected(m);
        }
    }
}

static void monitor_click(App *a, int x, int y, int buttons) {
    Monitor *m = a->state;
    (void)x;
    if (!m || !(buttons & 1)) return;

    /* Same fonts, same geo_of()/visible_rows() as monitor_draw(): the row
     * that was painted under this y is the row this click selects. The click
     * arrives in app coordinates, so the column edges are computed for a rect
     * that starts at 0; only body_y and row_h matter for the hit test. */
    Font *fh = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr || !fd) return;

    Geo geo = geo_of(0, m->w, fh, fr, fd);
    int rows = visible_rows((float)m->h, &geo);

    float local = (float)y - geo.body_y;
    if (local < 0.0f) return;                  /* header / headings */
    int i = (int)(local / geo.row_h);
    if (i >= rows) return;                     /* legend / past the list */
    int idx = m->scroll + i;
    if (idx >= 0 && idx < m->count) m->sel = idx;
}

static void monitor_resize(App *a, int w, int h) {
    Monitor *m = a->state;
    if (m) { m->w = w; m->h = h; }
}

/* The wheel: three rows a notch, positive notches towards the top, the same
 * distance the terminal scrolls (main.c:1607). The cursor and the window move
 * together, which is what keeps monitor_click()'s row index honest. */
static void monitor_wheel(App *a, int notches) {
    Monitor *m = a->state;
    if (!m || !notches) return;
    int step = notches * 3;
    m->sel -= step;
    if (m->sel < 0) m->sel = 0;
    if (m->sel >= m->count) m->sel = m->count > 0 ? m->count - 1 : 0;
}

const AppVT app_vt_monitor = {
    .id = "monitor",
    .title = "Monitor",
    .init = monitor_init,
    .draw = monitor_draw,
    .key = monitor_key,
    .click = monitor_click,
    .resize = monitor_resize,
    .wheel = monitor_wheel,
    .free = monitor_free,
};
