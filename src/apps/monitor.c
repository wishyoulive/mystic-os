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
 *  is why the kill takes the shift.
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
#define ROW_H    15.0f
#define BODY_Y   62      /* first row, local to the app */
#define FOOT_H   15      /* key legend at the bottom */
#define SAMPLE_S 0.35    /* shortest gap between two CPU samples */

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
     * surface (main.c:434). The rows already derive from h, so this only
     * bites when the rect is shorter than BODY_Y + FOOT_H, where rows is
     * clamped to 1 and that row would otherwise land past the bottom edge. */
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
    int rows = (int)((h - BODY_Y - FOOT_H) / ROW_H);
    if (rows < 1) rows = 1;
    m->rows = rows;

    if (m->sel >= m->count) m->sel = m->count - 1;
    if (m->sel < 0) m->sel = 0;
    if (m->sel < m->scroll) m->scroll = m->sel;
    if (m->sel >= m->scroll + rows) m->scroll = m->sel - rows + 1;
    if (m->scroll > m->count - rows) m->scroll = m->count - rows;
    if (m->scroll < 0) m->scroll = 0;

    /* ---- header: sort mode, process and thread totals ---- */
    char head[64], right[96];
    snprintf(head, sizeof(head), "sort: %s", sort_name(m->sort));
    font_draw(s, fh, (float)x + 6, (float)y + 4, head, th->text);

    /* The list holds at most MAX_PROC; say so instead of silently showing
     * a smaller "N proc" than /proc really has. */
    if (m->seen > m->count)
        snprintf(right, sizeof(right), "%d/%d proc   %d thr",
                 m->count, m->seen, threads);
    else
        snprintf(right, sizeof(right), "%d proc   %d thr", m->count, threads);
    float rw = font_width(fd, right);
    if (font_width(fh, head) + rw + 28.0f < (float)w)
        font_draw(s, fd, (float)(x + w) - rw - 6, (float)y + 7, right,
                  color_alpha(th->text, 0.55f));

    surf_blend(s, x + 4, y + 21, w - 8, 1, th->panel_border);

    /* ---- memory bar ---- */
    float frac = total_kb > 0 ? (float)used_kb / (float)total_kb : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    gfx_rrect_aa(s, (float)x + 4, (float)y + 25, (float)w - 8, 6, 3,
                 color_alpha(th->text, 0.10f), 0, 0);
    if (frac > 0.01f)
        gfx_rrect_aa(s, (float)x + 4, (float)y + 25, ((float)w - 8) * frac, 6, 3,
                     th->accent, 0, 0);

    /* ---- second header line: load, uptime, memory, last action ---- */
    char left[96];
    snprintf(left, sizeof(left), "load %.2f   up %s", l1, upbuf);
    font_draw(s, fd, (float)x + 6, (float)y + 35, left, th->text_dim);

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
        if (font_width(fd, left) + nw + 28.0f < (float)w)
            font_draw(s, fd, (float)(x + w) - nw - 6, (float)y + 35, note, note_col);
    }

    /* ---- column headings ---- */
    float top = (float)y + 50;
    float pid_x = (float)x + 6;
    float name_x = (float)x + 56;
    float cpu_r = (float)(x + w) - 78;
    float mem_r = (float)(x + w) - 8;

    font_draw(s, fd, pid_x, top, "PID", color_alpha(th->text, 0.45f));
    font_draw(s, fd, name_x, top, "NAME", color_alpha(th->text, 0.45f));
    const char *chcpu = "CPU", *chmem = "MEM";
    font_draw(s, fd, cpu_r - font_width(fd, chcpu), top, chcpu,
              color_alpha(th->text, 0.45f));
    font_draw(s, fd, mem_r - font_width(fd, chmem), top, chmem,
              color_alpha(th->text, 0.45f));

    /* ---- rows ---- */
    float name_max = (float)(w - 56) - 84.0f;
    if (name_max < 40.0f) name_max = 40.0f;

    double vmax = 1.0;
    for (int i = 0; i < m->count; i++) {
        double v = m->sort == SORT_CPU ? m->procs[i].cpu : (double)m->procs[i].kb;
        if (v > vmax) vmax = v;
    }

    float bar_w = (float)w - 15.0f;   /* x+9 .. x+w-6, clear of the cursor mark */
    for (int i = 0; i < rows; i++) {
        int idx = m->scroll + i;
        if (idx >= m->count) break;
        Proc *p = &m->procs[idx];
        float ry = (float)(y + BODY_Y) + (float)i * ROW_H;
        int sel = (idx == m->sel);

        if (sel) {
            gfx_rrect_aa(s, (float)x + 3, ry - 1, (float)w - 6, ROW_H - 1, 3,
                         color_alpha(th->accent, 0.22f),
                         color_alpha(th->accent, 0.70f), 1.0f);
            /* solid marker, so the cursor cannot be mistaken for the bar of
             * whichever row happens to hold the largest share */
            surf_blend(s, x + 4, (int)ry, 3, (int)(ROW_H - 3),
                       color_alpha(th->accent, 0.85f));
        }

        /* share bar of whatever the current sort keys on */
        double v = m->sort == SORT_CPU ? p->cpu : (double)p->kb;
        float bw = bar_w * (float)(v / vmax);
        if (bw > 2.0f)
            gfx_rrect_aa(s, (float)x + 9, ry + 2, bw, ROW_H - 5, 2,
                         color_alpha(th->accent, 0.13f), 0, 0);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", p->pid);
        font_draw(s, fr, pid_x, ry, buf, sel ? th->text : th->text_dim);

        char nm[32];
        snprintf(nm, sizeof(nm), "%s", p->name);
        while (strlen(nm) > 3 && font_width(fr, nm) > name_max)
            nm[strlen(nm) - 1] = 0;
        font_draw(s, fr, name_x, ry, nm, th->text);

        snprintf(buf, sizeof(buf), "%.1f%%", p->cpu);
        font_draw(s, fr, cpu_r - font_width(fr, buf), ry, buf,
                  sel ? th->text : th->text_dim);

        snprintf(buf, sizeof(buf), "%.1f MB", p->kb / 1024.0);
        font_draw(s, fr, mem_r - font_width(fr, buf), ry, buf,
                  sel ? th->text : th->text_dim);
    }

    /* ---- key legend ---- */
    if (h - FOOT_H > BODY_Y) {
        font_draw(s, fd, (float)x + 6, (float)(y + h) - 12,
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
    int rows = m->rows > 0 ? m->rows : 1;
    int local = y - BODY_Y;
    if (local < 0) return;
    int i = local / (int)ROW_H;
    if (i >= rows) return;
    int idx = m->scroll + i;
    if (idx >= 0 && idx < m->count) m->sel = idx;
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
