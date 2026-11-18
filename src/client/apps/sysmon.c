/* ============================================================
 *  Mystic OS - apps (external) / sysmon
 *
 *  A system dashboard that runs as its own process. It is the same
 *  feature as Monitor (src/apps/monitor.c), seen from outside the
 *  desktop: the process table uses the same columns, the same sort
 *  keys, the same filter keys and the same card geometry, so moving
 *  between the two windows does not mean learning a second table.
 *
 *  Everything it draws was read from the kernel moments earlier:
 *
 *    /proc/cpuinfo        "processor" lines                 -> cpu count
 *    /proc/loadavg        the three load averages           -> numbers + graph
 *    /proc/meminfo        MemTotal / MemAvailable           -> numbers + graph
 *    /proc/uptime         first field                       -> uptime
 *    /proc/<pid>/stat     fields 2, 3, 14, 15               -> comm, state,
 *                                                              utime+stime
 *    /proc/<pid>/statm    fields 1, 2                       -> resident set
 *    /proc/net/dev        per-interface rx/tx byte counters -> totals + rates
 *
 *  Every value that is a DELTA says so on screen:
 *
 *    CPU%    the jiffies between two samples divided by the measured wall
 *            time between those samples (CLOCK_MONOTONIC), as a percentage
 *            of one core, so a threaded process can read above 100%. The
 *            process card's title says "delta 0.98 s" with the interval
 *            that was actually measured; before the second sample it
 *            says "no delta yet" instead of inventing a zero.
 *    NET     the byte counters are cumulative; the rates beside them are
 *            (now - previous) / measured seconds, and the network card's
 *            title says "rates delta 1.00 s".
 *
 *  Memory is statm field 2 (resident pages) times the real page size.
 *
 *  Kill is the same key and the same path as Monitor: K sends SIGTERM to
 *  the selected pid immediately (no confirmation step, so the two
 *  windows cannot diverge), SIGKILL follows after the grace period if it
 *  is still alive, pids 1 and this process are refused, and kill(2)
 *  errors are reported with strerror.
 *
 *  The graphs are ring buffers inside this process: one point per one
 *  second sample, RING_N slots (three minutes). A ring is only advanced
 *  when its source file could be read, so a missing /proc/loadavg holds
 *  the old history instead of drawing invented zeros.
 *
 *  Mouse: click a column heading to sort, the filter line to type, a row
 *  to select. Wheel scrolls. Keys are listed on the window's bottom line.
 *
 *  Colours are the Obsidian theme's (stage/etc/mystic/themes/
 *  1-obsidian.theme), the theme a fresh image boots with, so this window
 *  and the two in-desktop system windows read as one family.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ sizes */

#define WIN_W 760
#define WIN_H 580

#define MAX_PROC 384
#define RING_N   180          /* three minutes at one sample per second */
#define MAX_IF   6

#define KILL_GRACE_MS 2000.0  /* SIGTERM -> SIGKILL */

/* ------------------------------------------------------------ colours */

static const u32 C_BG_TOP = RGB(0x0B, 0x0F, 0x1A);   /* Obsidian bg_top    */
static const u32 C_BG_BOT = RGB(0x16, 0x1C, 0x2E);   /* Obsidian bg_bottom */
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);   /* Obsidian text      */
static const u32 C_DIM    = WITH_A(RGB(0xE8, 0xEC, 0xF6), 0x99);
static const u32 C_ACCENT = RGB(0x7A, 0xA2, 0xF7);   /* Obsidian accent    */
static const u32 C_WARN   = RGB(0xE0, 0x6C, 0x5C);
static const u32 C_PANEL  = WITH_A(RGB(0x16, 0x1C, 0x2B), 0x8C);
static const u32 C_BORDER = WITH_A(RGB(0xFF, 0xFF, 0xFF), 0x1A);

#ifndef SIGKILL
#  define SIGKILL 9
#endif

/* The OS build reads the real /proc. The host harness points the same
 * parsers at a fixture tree it wrote itself, so nothing in the shipped
 * object changes. */
#ifdef SYSMON_HOST_TEST
#  ifndef SYSMON_PROC_ROOT
#    define SYSMON_PROC_ROOT ".work/sysui/fake/proc"
#  endif
#  define PROC_ROOT SYSMON_PROC_ROOT
#else
#  define PROC_ROOT "/proc"
#endif

/* ------------------------------------------------------------ data */

typedef struct {
    int    pid;
    char   comm[32];
    char   state;                      /* /proc/<pid>/stat field 3 */
    unsigned long long utime, stime;   /* jiffies from the stat    */
    long   mem_kb;                     /* statm field 2 x page     */
    double cpu;                        /* % of one core, delta; <0 unknown */
} Proc;

typedef struct {
    char name[16];
    unsigned long long rx, tx;         /* cumulative byte counters  */
    double rx_rate, tx_rate;           /* bytes/second, real delta  */
} Iface;

typedef struct {
    int    ncpu;
    int    nproc;
    int    truncated;                  /* MAX_PROC rows and more pids existed */
    int    hz;                         /* sysconf(_SC_CLK_TCK) */
    long   page_kb;
    int    load_ok, mem_ok, net_ok;
    double load[3];
    long   total_kb, avail_kb;
    double uptime;                     /* <0 when /proc/uptime is unreadable */
    double dt_ms;                      /* measured gap between samples */
    int    samples;

    Proc   procs[MAX_PROC];
    Iface  ifs[MAX_IF];
    int    nif;

    /* one ring per graph: a sample is only appended when its file was read */
    float  hist_load[RING_N];
    int    load_head, load_len;
    float  hist_mem[RING_N];
    int    mem_head, mem_len;

    /* table */
    int    view[MAX_PROC];
    int    nview;
    int    sort_key, sort_dir;
    char   filter[32];
    int    filter_edit;
    int    sel_pid;
    int    scroll, rows;

    /* actions */
    int    pending_pid;
    double pending_at;
    char   msg[160];
    double msg_at;
} SysMon;

enum { S_PID, S_NAME, S_STATE, S_CPU, S_MEM, S_N };
enum { K_SYS, K_MEM, K_NET };

/* ------------------------------------------------------------ clock and kill */

#ifdef SYSMON_HOST_TEST
static double g_host_ms;
static double mono_ms(void) { return g_host_ms; }
static void host_advance(double ms) { g_host_ms += ms; }
/* kill(2) does not exist on the Windows host; this records the signal and
 * reports success, so the SIGTERM -> SIGKILL state machine is exercised end
 * to end. pid 999999 reports ESRCH, like a process that is already gone. */
static int g_kill_n;
static int g_kill_pid[16];
static int g_kill_sig[16];
static int host_kill(int pid, int sig) {
    if (sig != 0 && g_kill_n < 16) {   /* sig 0 is only a liveness probe */
        g_kill_pid[g_kill_n] = pid;
        g_kill_sig[g_kill_n] = sig;
        g_kill_n++;
    }
    if (pid == 999999) { errno = ESRCH; return -1; }
    return 0;
}
#  define kill host_kill
#else
static double mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

/* ------------------------------------------------------------ /proc */

static int read_ncpu(void) {
    FILE *f = fopen(PROC_ROOT "/cpuinfo", "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "processor", 9)) n++;
    fclose(f);
    return n;
}

/* Returns 1 when the three averages were read, 0 otherwise. */
static int read_loadavg(SysMon *m) {
    m->load[0] = m->load[1] = m->load[2] = 0.0;
    FILE *f = fopen(PROC_ROOT "/loadavg", "r");
    if (!f) return 0;
    double a = 0.0, b = 0.0, c = 0.0;
    int got = fscanf(f, "%lf %lf %lf", &a, &b, &c);
    fclose(f);
    if (got != 3) return 0;
    m->load[0] = a;
    m->load[1] = b;
    m->load[2] = c;
    return 1;
}

/* MemAvailable arrived in 3.14; older kernels only have MemFree. Returns 1
 * only when a used/total pair can actually be computed. */
static int read_meminfo(SysMon *m) {
    m->total_kb = 0;
    m->avail_kb = 0;
    long free_kb = 0;
    int have_avail = 0, have_free = 0;
    FILE *f = fopen(PROC_ROOT "/meminfo", "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))
            m->total_kb = strtol(line + 9, NULL, 10);
        else if (!strncmp(line, "MemAvailable:", 13)) {
            m->avail_kb = strtol(line + 13, NULL, 10);
            have_avail = 1;
        } else if (!strncmp(line, "MemFree:", 8)) {
            free_kb = strtol(line + 8, NULL, 10);
            have_free = 1;
        }
    }
    fclose(f);
    if (!have_avail && have_free) m->avail_kb = free_kb;
    return m->total_kb > 0 && (have_avail || have_free);
}

/* <0 when the file cannot be read: a zero here would be a claim. */
static double read_uptime(void) {
    FILE *f = fopen(PROC_ROOT "/uptime", "r");
    if (!f) return -1.0;
    double up = 0.0;
    int got = fscanf(f, "%lf", &up);
    fclose(f);
    if (got != 1 || up < 0.0) return -1.0;
    return up;
}

static char *next_field(char **sp) {
    char *s = *sp;
    while (*s == ' ') s++;
    char *t = s;
    while (*s && *s != ' ') s++;
    if (*s) { *s = 0; s++; }
    *sp = s;
    return t;
}

/* /proc/<pid>/stat: the command may contain spaces and ')' so the last ')'
 * on the line is the one that closes it; every field after it is a number or
 * a state letter. Fills comm, state, utime, stime. */
static int read_stat(int pid, Proc *p) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *rp = strrchr(buf, ')');
    char *lp = strchr(buf, '(');
    if (!rp || !lp || lp > rp) return -1;

    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof(p->comm)) cl = sizeof(p->comm) - 1;
    memcpy(p->comm, lp + 1, cl);
    p->comm[cl] = 0;

    char *sp = rp + 1;
    char *tok[13];
    for (int i = 0; i < 13; i++) tok[i] = next_field(&sp);
    if (!tok[0][0] || !tok[11][0] || !tok[12][0]) return -1;
    p->state = tok[0][0];                       /* field 3  */
    p->utime = strtoull(tok[11], NULL, 10);     /* field 14 */
    p->stime = strtoull(tok[12], NULL, 10);     /* field 15 */
    return 0;
}

/* statm is "size resident shared text lib data dt" in pages. A pid whose
 * statm cannot be read keeps mem_kb = -1, which draws as "-": a live process
 * is not dropped from the table just because one file is missing. */
static int read_statm(int pid, long *rss_pages) {
    char path[64];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long size_pages = 0, rss = 0;
    int got = fscanf(f, "%ld %ld", &size_pages, &rss);
    fclose(f);
    if (got != 2) return -1;
    *rss_pages = rss > 0 ? rss : 0;
    return 0;
}

static const Proc *find_pid(const Proc *list, int n, int pid) {
    for (int i = 0; i < n; i++)
        if (list[i].pid == pid) return &list[i];
    return NULL;
}

/* ------------------------------------------------------------ /proc/net/dev */

static Iface *find_if(Iface *list, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].name, name)) return &list[i];
    return NULL;
}

static void read_net(SysMon *m, double dt_ms) {
    Iface next[MAX_IF];
    int n = 0;
    memset(next, 0, sizeof(next));

    FILE *f = fopen(PROC_ROOT "/net/dev", "r");
    m->net_ok = f != NULL;
    if (!f) { m->nif = 0; return; }

    char line[512];
    int header = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, ':')) { header++; continue; }
        if (header < 2) continue;
        char *colon = strchr(line, ':');
        char name[16];
        size_t nl = (size_t)(colon - line);
        while (nl > 0 && (line[nl - 1] == ' ' || line[nl - 1] == '\t')) nl--;
        size_t st = 0;
        while (st < nl && (line[st] == ' ' || line[st] == '\t')) st++;
        if (nl - st >= sizeof(name)) continue;
        memcpy(name, line + st, nl - st);
        name[nl - st] = 0;
        if (!name[0]) continue;

        unsigned long long v[16];
        char *sp = colon + 1;
        int got = 0;
        for (int i = 0; i < 16; i++) {
            char *t = next_field(&sp);
            if (!t[0]) break;
            v[i] = strtoull(t, NULL, 10);
            got++;
        }
        if (got < 16) continue;
        if (n >= MAX_IF) break;

        snprintf(next[n].name, sizeof(next[n].name), "%s", name);
        next[n].rx = v[0];       /* rx bytes */
        next[n].tx = v[8];       /* tx bytes */
        Iface *old = find_if(m->ifs, m->nif, name);
        if (old && dt_ms > 1.0) {
            double secs = dt_ms / 1000.0;
            if (next[n].rx >= old->rx)
                next[n].rx_rate = (double)(next[n].rx - old->rx) / secs;
            if (next[n].tx >= old->tx)
                next[n].tx_rate = (double)(next[n].tx - old->tx) / secs;
        }
        n++;
    }
    fclose(f);
    memcpy(m->ifs, next, sizeof(Iface) * (size_t)n);
    m->nif = n;
}

/* ------------------------------------------------------------ sample */

/* Read every pid and compute its CPU delta against the previous table.
 * dt_ms is the measured wall time since the previous sample. */
static void sample(SysMon *m, double dt_ms) {
    m->dt_ms = dt_ms;
    m->load_ok = read_loadavg(m);
    m->mem_ok = read_meminfo(m);
    m->uptime = read_uptime();
    m->ncpu = read_ncpu();
    read_net(m, dt_ms);

    static Proc next[MAX_PROC];
    int n = 0;
    m->truncated = 0;
    double dt_s = dt_ms / 1000.0;
    DIR *d = opendir(PROC_ROOT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            int pid = atoi(e->d_name);
            if (pid <= 0) continue;
            if (n >= MAX_PROC) { m->truncated = 1; break; }

            Proc tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.pid = pid;
            tmp.cpu = -1.0;                       /* unknown until a delta */
            if (read_stat(pid, &tmp) != 0) continue;      /* it died mid-scan */
            long rss = 0;
            tmp.mem_kb = read_statm(pid, &rss) == 0 ? rss * m->page_kb : -1;

            const Proc *old = find_pid(m->procs, m->nproc, pid);
            if (old && dt_s > 0.0005) {
                unsigned long long a = old->utime + old->stime;
                unsigned long long b = tmp.utime + tmp.stime;
                if (b >= a) {
                    double pct = (double)(b - a) * 100.0 /
                                 ((double)m->hz * dt_s);
                    if (pct > 999.0) pct = 999.0;
                    tmp.cpu = pct;
                } else {
                    tmp.cpu = 0.0;                /* counters went backwards */
                }
            }
            next[n++] = tmp;
        }
        closedir(d);
    }
    memcpy(m->procs, next, sizeof(Proc) * (size_t)n);
    m->nproc = n;

    /* One point per graph, and only when that graph's source was read. */
    if (m->load_ok) {
        m->hist_load[m->load_head] = (float)m->load[0];
        m->load_head = (m->load_head + 1) % RING_N;
        if (m->load_len < RING_N) m->load_len++;
    }
    if (m->mem_ok) {
        long used = m->total_kb - m->avail_kb;
        if (used < 0) used = 0;
        m->hist_mem[m->mem_head] = m->total_kb > 0
            ? (float)((double)used / (double)m->total_kb) : 0.0f;
        m->mem_head = (m->mem_head + 1) % RING_N;
        if (m->mem_len < RING_N) m->mem_len++;
    }
    m->samples++;
}

/* ------------------------------------------------------------
 *  CARDS - one visual language for sysinfo, monitor and sysmon.
 *  The frame, title, row and margin code is identical in
 *  src/apps/sysinfo.c and src/apps/monitor.c; this file also carries the
 *  sparkline painter, because only this window has history to draw.
 * ------------------------------------------------------------ */
typedef struct { float x, y, w, h; } CrdRect;

typedef struct {
    u32 panel, border, text, dim, accent, warn;
    float corner;
} CrdStyle;

typedef struct {
    float pad;
    float gap;
    float title_h;
    float row_h;
    float line_h;
    float spark_h;
} CrdMetrics;

static float crd_ink(Font *f) { return font_height(f) / 1.4f; }
static float crd_base(Font *f, float top, float band) {
    return top + (band - crd_ink(f)) * 0.5f + font_ascent(f);
}
static float crd_top(Font *f, float base) { return base - font_ascent(f); }

static CrdMetrics crd_metrics(Font *ft, Font *fr, Font *fs) {
    CrdMetrics m;
    m.pad     = (float)ui_px(10);
    m.gap     = (float)ui_px(6);
    m.title_h = font_height(ft) + (float)ui_px(6);
    m.row_h   = font_height(fr) + (float)ui_px(6);
    m.line_h  = font_height(fs) + (float)ui_px(4);
    m.spark_h = font_height(fs) + (float)ui_px(4) + font_height(fs) * 1.6f;
    return m;
}

static float crd_height(const CrdMetrics *m, int rows, int spark) {
    float h = m->pad * 2.0f + m->title_h + m->gap + (float)rows * m->row_h;
    if (spark) h += m->gap + m->spark_h;
    return h;
}

static int crd_hit(const CrdRect *r, int px, int py) {
    return (float)px >= r->x && (float)py >= r->y &&
           (float)px < r->x + r->w && (float)py < r->y + r->h;
}

static void crd_fit(Font *f, const char *text, char *out, size_t cap, float maxw) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", text ? text : "");
    snprintf(out, cap, "%s", tmp);
    if (maxw <= 0.0f) { out[0] = 0; return; }
    if (font_width(f, out) <= maxw) return;
    size_t n = strlen(tmp);
    while (n > 0) {
        n--;
        /* never split a UTF-8 sequence: back up to its first byte, then cut
         * with a real ellipsis (U+2026), which Lato carries */
        while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) n--;
        snprintf(out, cap, "%.*s…", (int)n, tmp);
        if (font_width(f, out) <= maxw) return;
    }
    out[0] = 0;
}

static void crd_row_text(Font *fr, float inner_w, float gap,
                         const char *label, const char *value,
                         char *lb, size_t lcap, char *vb, size_t vcap) {
    /* The value gets what the label does not need, with a floor at 40% so a
     * long label can never squeeze a number down to nothing. Then the label
     * is fitted into the remainder, which is what keeps the two apart. */
    float lw = font_width(fr, label ? label : "");
    float maxv = inner_w - lw - gap;
    if (maxv < inner_w * 0.40f) maxv = inner_w * 0.40f;
    if (maxv < 0.0f) maxv = 0.0f;
    crd_fit(fr, value, vb, vcap, maxv);
    float vw = vb[0] ? font_width(fr, vb) + gap : 0.0f;
    crd_fit(fr, label, lb, lcap, inner_w - vw);
}

static float crd_row_y(const CrdRect *c, const CrdMetrics *m, int i) {
    return c->y + m->pad + m->title_h + m->gap + (float)i * m->row_h;
}

static void crd_begin(Surface *s, const CrdRect *c, const CrdMetrics *m,
                      const CrdStyle *st, Font *ft, Font *fs,
                      const char *title, const char *meta, int meta_warn) {
    float r = st->corner;
    float lim = (c->w < c->h ? c->w : c->h) * 0.5f;
    if (r > lim) r = lim;
    if (r < 0.0f) r = 0.0f;
    gfx_rrect_aa(s, c->x, c->y, c->w, c->h, r, st->panel, st->border, 1.0f);
    float base = crd_base(ft, c->y + m->pad, m->title_h);
    font_draw(s, ft, c->x + m->pad, crd_top(ft, base), title,
              color_alpha(st->accent, 0.92f));
    if (meta && meta[0]) {
        /* The note beside the title is the SMALL face, one step down the type
         * scale, so a 320 px card still holds both "PROCESSES" and the delta
         * interval. It is dropped rather than overlapped if even that fails. */
        float mw = font_width(fs, meta);
        if (mw + m->pad * 2.0f + font_width(ft, title) <= c->w) {
            float mb = crd_base(fs, c->y + m->pad, m->title_h);
            font_draw(s, fs, c->x + c->w - m->pad - mw, crd_top(fs, mb), meta,
                      meta_warn ? color_alpha(st->warn, 0.95f)
                                : color_alpha(st->text, 0.45f));
        }
    }
    float ry = c->y + m->pad + m->title_h;
    surf_blend(s, (int)(c->x + m->pad), (int)ry,
               (int)(c->w - 2.0f * m->pad), 1, color_alpha(st->text, 0.10f));
}

static void crd_row(Surface *s, const CrdRect *c, const CrdMetrics *m,
                    const CrdStyle *st, Font *fr, int i,
                    const char *label, const char *value) {
    float ys = crd_row_y(c, m, i);
    float inner = c->w - 2.0f * m->pad;
    char lb[128], vb[192];
    crd_row_text(fr, inner, m->gap, label, value, lb, sizeof(lb), vb, sizeof(vb));
    float base = crd_base(fr, ys, m->row_h);
    font_draw(s, fr, c->x + m->pad, crd_top(fr, base), lb,
              color_alpha(st->text, 0.62f));
    if (vb[0])
        font_draw(s, fr, c->x + c->w - m->pad - font_width(fr, vb),
                  crd_top(fr, base), vb, st->text);
}

/* A crisp 2 px polyline: gfx_line is antialiased, and a flat graph at 63%
 * coverage reads as a smudge, while a sparkline wants to be a hard line. */
static void crd_spark_line(Surface *s, int x0, int y0, int x1, int y1, u32 col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        surf_set(s, x0, y0, col);
        surf_set(s, x0, y0 + 1, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* The history strip at the foot of a card: a caption, a floor line, a faint
 * top line and the line itself. No fill: a filled area behind a sparkline
 * reads as a bar repeating the number above it. */
static void crd_spark(Surface *s, const CrdRect *c, const CrdMetrics *m,
                      Font *fs, const char *legend, u32 legend_col,
                      const float *hist, int head, int len, int cap,
                      float vmax_fixed, u32 col) {
    float top = c->y + c->h - m->pad - m->spark_h;
    float x0 = c->x + m->pad;
    float x1 = c->x + c->w - m->pad;
    float gy0 = top + font_height(fs) + (float)ui_px(2);
    float gy1 = c->y + c->h - m->pad;
    if (legend && legend[0]) {
        char cap[64];
        crd_fit(fs, legend, cap, sizeof(cap), x1 - x0);
        font_draw(s, fs, x0, top, cap, legend_col);
    }
    if (x1 - x0 < 8.0f || gy1 - gy0 < 4.0f) return;
    surf_blend(s, (int)x0, (int)gy1, (int)(x1 - x0), 1, color_alpha(col, 0.38f));
    surf_blend(s, (int)x0, (int)gy0, (int)(x1 - x0), 1, color_alpha(col, 0.12f));
    if (len <= 0 || cap <= 1) return;

    float vmax = vmax_fixed;
    if (vmax <= 0.0f) {
        vmax = 0.0f;
        for (int i = 0; i < len; i++) {
            float v = hist[(head - len + i + cap) % cap];
            if (v > vmax) vmax = v;
        }
    }
    if (vmax < 0.0001f) vmax = 1.0f;

    float plotw = x1 - x0;
    float step = plotw / (float)(cap - 1);
    float prevx = 0.0f, prevy = 0.0f;
    int have = 0;
    for (int i = 0; i < len; i++) {
        int idx = (head - len + i + cap) % cap;
        float v = hist[idx];
        if (v < 0.0f) v = 0.0f;
        if (v > vmax) v = vmax;
        /* newest sample on the right edge, history growing leftwards */
        float px = x1 - (float)(len - 1 - i) * step;
        float py = gy1 - (v / vmax) * (gy1 - gy0);
        if (py < gy0) py = gy0;
        if (py > gy1 - 1.0f) py = gy1 - 1.0f;
        if (have)
            crd_spark_line(s, (int)(prevx + 0.5f), (int)(prevy + 0.5f),
                           (int)(px + 0.5f), (int)(py + 0.5f), col);
        prevx = px;
        prevy = py;
        have = 1;
        if (i == len - 1)
            surf_fill(s, (int)px - 1, (int)py - 1, 3, 3, col);
    }
}

static CrdStyle crd_style(void) {
    CrdStyle st;
    st.panel  = C_PANEL;
    st.border = C_BORDER;
    st.text   = C_TEXT;
    st.dim    = C_DIM;
    st.accent = C_ACCENT;
    st.warn   = C_WARN;
    st.corner = 7.7f;   /* 14 x 0.55, the Obsidian theme's card radius */
    return st;
}

/* ------------------------------------------------------------
 *  Process table - the same struct, columns, sort and filter as
 *  src/apps/monitor.c.
 * ------------------------------------------------------------ */
typedef struct {
    CrdRect card;
    int   show_filter, show_head;
    CrdRect filter, head;
    float body_y, row_h;
    int   rows;
    int   have_state, have_cpu, have_mem;
    float pid_r, name_x, name_end;
    float state_l, state_r, cpu_l, cpu_r, mem_l, mem_r;
    float name_max;
} ProcCard;

static ProcCard proc_card_layout(const CrdRect *card, const CrdMetrics *m,
                                 Font *fr, Font *fs) {
    ProcCard p;
    memset(&p, 0, sizeof(p));
    p.card = *card;
    p.row_h = m->row_h;

    float inner_l = card->x + m->pad;
    float inner_r = card->x + card->w - m->pad;
    float gap = m->gap;
    float min_name = (float)ui_px(24);

    float mem_w = font_width(fr, "9999.9 GB") + gap;
    float cpu_w = font_width(fr, "999.9%") + gap;
    float st_w  = font_width(fs, "STATE") + gap;
    float pid_w = font_width(fr, "9999999");   /* pid_max is 7 digits */

    p.pid_r = inner_l + pid_w;
    p.name_x = p.pid_r + gap;
    p.mem_r = inner_r;
    p.mem_l = p.mem_r - mem_w;
    p.cpu_r = p.mem_l;
    p.cpu_l = p.cpu_r - cpu_w;
    p.state_r = p.cpu_l;
    p.state_l = p.state_r - st_w;

    /* Drop columns from the right until the name has min_name pixels of its
     * own. Every *_l above is the LEFT EDGE OF THE VALUES, not of the heading:
     * a heading is narrower than its values, and the name must never run
     * under a value. */
    p.have_mem = (p.mem_l - p.name_x) >= min_name;
    if (!p.have_mem) {
        p.mem_l = p.mem_r = inner_r;
        p.cpu_l = p.cpu_r = inner_r;
        p.state_l = p.state_r = inner_r;
    }
    p.have_cpu = p.have_mem && (p.cpu_l - p.name_x) >= min_name;
    if (!p.have_cpu) {
        p.cpu_l = p.cpu_r = p.mem_l;
        p.state_l = p.state_r = p.mem_l;
    }
    p.have_state = p.have_cpu && (p.state_l - p.name_x) >= min_name;
    if (!p.have_state) p.state_l = p.state_r = p.cpu_l;

    p.name_end = p.have_state ? p.state_l
               : p.have_cpu   ? p.cpu_l
               : p.have_mem   ? p.mem_l
                              : inner_r;
    p.name_max = p.name_end - p.name_x;

    float y = card->y + m->pad + m->title_h + m->gap;
    float bottom = card->y + card->h - m->pad;
    int fit_filter = (bottom - y - m->line_h - m->line_h) >= m->row_h;
    int fit_head = (bottom - y - m->line_h) >= m->row_h;
    p.show_filter = fit_filter;
    if (p.show_filter) {
        p.filter = (CrdRect){ inner_l, y, inner_r - inner_l, m->line_h };
        y += m->line_h;
    }
    p.show_head = fit_head;
    if (p.show_head) {
        p.head = (CrdRect){ inner_l, y, inner_r - inner_l, m->line_h };
        y += m->line_h;
    }
    p.body_y = y;
    float rf = (bottom - y) / m->row_h;
    p.rows = (int)rf;
    if (p.rows < 0) p.rows = 0;
    return p;
}

static int proc_col_at(const ProcCard *p, int x) {
    float centers[S_N];
    float best = 1e30f;
    int col = S_NAME;
    centers[S_PID]   = (p->name_x - (float)ui_px(6) + p->pid_r) * 0.5f;
    centers[S_NAME]  = (p->name_x + p->name_end) * 0.5f;
    centers[S_STATE] = (p->state_l + p->state_r) * 0.5f;
    centers[S_CPU]   = (p->cpu_l + p->cpu_r) * 0.5f;
    centers[S_MEM]   = (p->mem_l + p->mem_r) * 0.5f;
    for (int i = 0; i < S_N; i++) {
        if (i == S_STATE && !p->have_state) continue;
        if (i == S_CPU && !p->have_cpu) continue;
        if (i == S_MEM && !p->have_mem) continue;
        float d = (float)x - centers[i];
        if (d < 0.0f) d = -d;
        if (d < best) { best = d; col = i; }
    }
    return col;
}

static const Proc *g_sort_procs;
static int g_sort_key;
static int g_sort_dir;

static int ci_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (int)(unsigned char)*a;
        int cb = (int)(unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int proc_cmp(const void *pa, const void *pb) {
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const Proc *a = &g_sort_procs[ia];
    const Proc *b = &g_sort_procs[ib];
    int r = 0;
    switch (g_sort_key) {
    case S_PID:   r = a->pid - b->pid; break;
    case S_NAME:  r = ci_cmp(a->comm, b->comm); break;
    case S_STATE: r = (int)(unsigned char)a->state - (int)(unsigned char)b->state; break;
    case S_CPU:   r = a->cpu > b->cpu ? 1 : (a->cpu < b->cpu ? -1 : 0); break;
    case S_MEM:   r = a->mem_kb > b->mem_kb ? 1 : (a->mem_kb < b->mem_kb ? -1 : 0); break;
    default: break;
    }
    if (r == 0) r = a->pid - b->pid;
    return g_sort_dir > 0 ? r : -r;
}

static int ci_contains(const char *hay, const char *needle) {
    size_t fl = strlen(needle);
    if (!fl) return 1;
    for (const char *q = hay; *q; q++) {
        size_t i = 0;
        while (i < fl && q[i]) {
            int a = (int)(unsigned char)q[i];
            int b = (int)(unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            i++;
        }
        if (i == fl) return 1;
    }
    return 0;
}

static int proc_match(const Proc *p, const char *f) {
    if (!f[0]) return 1;
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", p->pid);
    if (strstr(pid, f)) return 1;
    return ci_contains(p->comm, f);
}

static int proc_view_build(const Proc *procs, int n, int key, int dir,
                           const char *filter, int *view) {
    int vn = 0;
    for (int i = 0; i < n && vn < MAX_PROC; i++)
        if (proc_match(&procs[i], filter)) view[vn++] = i;
    g_sort_procs = procs;
    g_sort_key = key;
    g_sort_dir = dir;
    if (vn > 1) qsort(view, (size_t)vn, sizeof(int), proc_cmp);
    return vn;
}

static int proc_view_index(const Proc *procs, const int *view, int vn, int pid) {
    for (int i = 0; i < vn; i++) if (procs[view[i]].pid == pid) return i;
    return -1;
}

typedef struct {
    const Proc *procs;
    const int  *view;
    int         nview;
    int         ntotal;
    int         scroll;
    int         sel_pid;
    int         sort_key, sort_dir;
    char        filter[32];
    int         filter_edit;
    const char *note;
    int         note_warn;
} ProcView;

static void proc_sort_mark(Surface *s, float x, float y, int dir, u32 col) {
    float pts[6];
    float w = 7.0f, h = 6.0f;
    if (dir > 0) {
        pts[0] = x;         pts[1] = y + h;
        pts[2] = x + w;     pts[3] = y + h;
        pts[4] = x + w / 2; pts[5] = y;
    } else {
        pts[0] = x;         pts[1] = y;
        pts[2] = x + w;     pts[3] = y;
        pts[4] = x + w / 2; pts[5] = y + h;
    }
    gfx_polygon(s, pts, 3, col);
}

/* RSS for the table: one formatter in both tables, so the same process reads
 * the same string in either window. MB below a gigabyte, GB above it; the MEM
 * column is measured from "9999.9 GB", so even a huge process still fits. */
static void proc_fmt_mem(long kb, char *out, size_t n) {
    if (kb < 0) snprintf(out, n, "-");
    else if (kb < 1024) snprintf(out, n, "%ld kB", kb);
    else if (kb < 1024L * 1024L) snprintf(out, n, "%.1f MB", (double)kb / 1024.0);
    else snprintf(out, n, "%.1f GB", (double)kb / (1024.0 * 1024.0));
}

static void proc_draw(Surface *s, const ProcCard *p, const CrdMetrics *m,
                      const CrdStyle *st, Font *ft, Font *fr, Font *fs,
                      const ProcView *v, const char *title, const char *meta) {
    crd_begin(s, &p->card, m, st, ft, fs, title, meta, 0);

    u32 label = color_alpha(st->text, 0.45f);
    u32 dim = color_alpha(st->text, 0.62f);

    if (p->show_filter) {
        float base = crd_base(fs, p->filter.y, p->filter.h);
        char line[96], out[128];
        if (v->filter[0]) snprintf(line, sizeof(line), "/ %s", v->filter);
        else              snprintf(line, sizeof(line), "/ filter   (f)");
        /* Both ends of this line are measured and fitted, so a long kill
         * message cannot push the count past the card edge or over the
         * filter text. */
        char rtxt[128], rout[128];
        rtxt[0] = 0;
        if (v->note && v->note[0]) snprintf(rtxt, sizeof(rtxt), "%s", v->note);
        else snprintf(rtxt, sizeof(rtxt), "%d of %d", v->nview, v->ntotal);
        crd_fit(fs, rtxt, rout, sizeof(rout), p->filter.w * 0.62f);
        float rightw = rout[0] ? font_width(fs, rout) + m->gap : 0.0f;
        crd_fit(fs, line, out, sizeof(out), p->filter.w - rightw);
        font_draw(s, fs, p->filter.x, crd_top(fs, base), out,
                  v->filter_edit ? color_alpha(st->accent, 0.95f)
                                 : color_alpha(st->text, 0.55f));
        if (v->filter_edit) {
            float cx = p->filter.x + font_width(fs, out) + 2.0f;
            surf_fill(s, (int)cx, (int)(p->filter.y + 2.0f), 1,
                      (int)(p->filter.h - 4.0f), color_alpha(st->accent, 0.95f));
        }
        if (rightw > 0.0f)
            font_draw(s, fs, p->filter.x + p->filter.w - rightw, crd_top(fs, base),
                      rout, v->note && v->note[0]
                                ? (v->note_warn ? color_alpha(st->warn, 0.95f)
                                                : color_alpha(st->accent, 0.85f))
                                : dim);
    }

    if (p->show_head) {
        float base = crd_base(fs, p->head.y, p->head.h);
        float ty = crd_top(fs, base);
        char nmb[8];
        crd_fit(fs, "NAME", nmb, sizeof(nmb), p->name_max);
        font_draw(s, fs, p->name_x, ty, nmb, label);
        if (p->have_state) {
            const char *t = "STATE";
            font_draw(s, fs, p->state_r - font_width(fs, t), ty, t, label);
        }
        if (p->have_cpu) {
            const char *t = "CPU%";
            font_draw(s, fs, p->cpu_r - font_width(fs, t), ty, t, label);
        }
        if (p->have_mem) {
            const char *t = "MEM";
            font_draw(s, fs, p->mem_r - font_width(fs, t), ty, t, label);
        }
        {
            const char *t = "PID";
            font_draw(s, fs, p->pid_r - font_width(fs, t), ty, t, label);
        }
        for (int i = 0; i < S_N; i++) {
            if (i == S_STATE && !p->have_state) continue;
            if (i == S_CPU && !p->have_cpu) continue;
            if (i == S_MEM && !p->have_mem) continue;
            if (i != v->sort_key) continue;
            float mx = (i == S_PID)   ? p->pid_r - font_width(fs, "PID") - 10.0f
                     : (i == S_NAME)  ? p->name_x + font_width(fs, "NAME") + 4.0f
                     : (i == S_STATE) ? p->state_r - font_width(fs, "STATE") - 10.0f
                     : (i == S_CPU)   ? p->cpu_r - font_width(fs, "CPU%") - 10.0f
                                      : p->mem_r - font_width(fs, "MEM") - 10.0f;
            proc_sort_mark(s, mx, p->head.y + 3.0f, v->sort_dir, st->accent);
        }
        surf_blend(s, (int)p->head.x, (int)(p->head.y + p->head.h),
                   (int)p->head.w, 1, color_alpha(st->text, 0.08f));
    }

    if (v->nview == 0) {
        if (p->rows > 0) {
            /* the empty state is one line, and it is measured like every other
             * string: at a narrow window it is cut with the same ellipsis */
            char eb[64];
            crd_fit(fr, v->filter[0] ? "no process matches the filter"
                                     : "no process in /proc",
                    eb, sizeof(eb), p->card.w - 2.0f * m->pad);
            float base = crd_base(fr, p->body_y, p->row_h);
            font_draw(s, fr, p->card.x + m->pad, crd_top(fr, base), eb,
                      color_alpha(st->text, 0.40f));
        }
    } else {
        int sel = proc_view_index(v->procs, v->view, v->nview, v->sel_pid);
        for (int i = 0; i < p->rows; i++) {
            int vi = v->scroll + i;
            if (vi >= v->nview) break;
            const Proc *pr = &v->procs[v->view[vi]];
            float ry = p->body_y + (float)i * p->row_h;
            if (vi == sel) {
                surf_blend(s, (int)(p->card.x + 1.0f), (int)ry,
                           (int)(p->card.w - 2.0f), (int)p->row_h,
                           color_alpha(st->accent, 0.16f));
                surf_blend(s, (int)(p->card.x + 1.0f), (int)ry,
                           2, (int)p->row_h, color_alpha(st->accent, 0.85f));
            }
            float base = crd_base(fr, ry, p->row_h);
            float ty = crd_top(fr, base);
            char buf[64], nm[64];

            snprintf(buf, sizeof(buf), "%d", pr->pid);
            font_draw(s, fr, p->pid_r - font_width(fr, buf), ty, buf,
                      color_alpha(st->text, 0.55f));

            crd_fit(fr, pr->comm, nm, sizeof(nm), p->name_max);
            font_draw(s, fr, p->name_x, ty, nm, st->text);

            if (p->have_state) {
                char sb[2] = { pr->state ? pr->state : '?', 0 };
                font_draw(s, fr, p->state_r - font_width(fr, sb), ty, sb,
                          pr->state == 'R' ? color_alpha(st->accent, 0.95f)
                                           : color_alpha(st->text, 0.55f));
            }
            if (p->have_cpu) {
                if (pr->cpu < 0.0) {
                    font_draw(s, fr, p->cpu_r - font_width(fr, "-"), ty, "-",
                              color_alpha(st->text, 0.40f));
                } else {
                    snprintf(buf, sizeof(buf), "%.1f%%", pr->cpu);
                    font_draw(s, fr, p->cpu_r - font_width(fr, buf), ty, buf,
                              pr->cpu >= 50.0 ? color_alpha(st->warn, 0.95f)
                                              : color_alpha(st->text, 0.85f));
                }
            }
            if (p->have_mem) {
                if (pr->mem_kb < 0) {
                    font_draw(s, fr, p->mem_r - font_width(fr, "-"), ty, "-",
                              color_alpha(st->text, 0.40f));
                } else {
                    proc_fmt_mem(pr->mem_kb, buf, sizeof(buf));
                    font_draw(s, fr, p->mem_r - font_width(fr, buf), ty, buf,
                              color_alpha(st->text, 0.85f));
                }
            }
        }
    }
}

/* ------------------------------------------------------------ formatting */

static void fmt_bytes(double v, char *out, size_t n) {
    if (v < 1024.0) snprintf(out, n, "%.0f B", v);
    else if (v < 1024.0 * 1024.0) snprintf(out, n, "%.1f kB", v / 1024.0);
    else if (v < 1024.0 * 1024.0 * 1024.0) snprintf(out, n, "%.1f MB", v / (1024.0 * 1024.0));
    else snprintf(out, n, "%.1f GB", v / (1024.0 * 1024.0 * 1024.0));
}

static void fmt_uptime(double secs, char *out, size_t n) {
    if (secs < 0.0) { snprintf(out, n, "unreadable"); return; }
    unsigned long s = (unsigned long)secs;
    unsigned long d  = s / 86400UL;
    unsigned long hh = (s % 86400UL) / 3600UL;
    unsigned long mm = (s % 3600UL) / 60UL;
    unsigned long ss = s % 60UL;
    if (d) snprintf(out, n, "%lud %02lu:%02lu", d, hh, mm);
    else   snprintf(out, n, "%02lu:%02lu:%02lu", hh, mm, ss);
}

/* ------------------------------------------------------------ layout */

typedef struct {
    Font *head, *body, *small;
} Fonts;

typedef struct {
    int   nsum;
    int   kind[3];
    CrdRect sum[3];
    ProcCard pc;
    float margin, gap, legend_h;
    int   legend;
    int   split;
} SysLayout;

/* Cards down the left column, the table on the right. The split only happens
 * when the window is wide enough in LOGICAL pixels (ui_px(560)) and a card
 * can still hold its longest label and value without truncating both; at the
 * largest text scale 760 device pixels is a narrow window, so the table gets
 * the whole width instead. Summary cards are offered by priority and dropped
 * from the end (network, then memory, then system) until they fit - the table
 * keeps its full height either way. */
static SysLayout sysmon_layout(int w, int h, const SysMon *m,
                               const CrdMetrics *cm, Font *fr, Font *fs,
                               const int *want_kind, const int *want_rows,
                               const int *want_spark, int nwant) {
    SysLayout L;
    memset(&L, 0, sizeof(L));
    L.margin = (float)ui_px(10);
    L.gap = (float)ui_px(8);
    L.legend_h = font_height(fs) + (float)ui_px(4);

    float avail = (float)h - 2.0f * L.margin;
    float min_card = cm->pad * 2.0f + cm->title_h + cm->gap + cm->line_h + cm->row_h;
    L.legend = (avail - L.legend_h) >= min_card;
    if (L.legend) avail -= L.legend_h;
    if (avail < 1.0f) avail = 1.0f;

    /* The card stack and the table share the width only when BOTH can be
     * read: the cards need their longest label/value pair, and the table
     * needs enough room to keep all five columns above the minimum name
     * width. ui_px() now follows the absolute shipped scale, so the default
     * 760 px window still gets its dashboard (one column would hide the
     * tables' own reason for being separate from Monitor). */
    float left_w = 0.0f;
    L.split = 0;
    if (w >= ui_px(420)) {
        left_w = (float)ui_px(230);
        float cap = ((float)w - 2.0f * L.margin) * 0.42f;
        if (left_w > cap) left_w = cap;
        float inner = left_w - 2.0f * cm->pad;
        float need = font_width(fr, "0000.0 MB") + font_width(fr, "Load 1/5/15");
        float table_w = (float)w - 2.0f * L.margin - L.gap - left_w;
        if (left_w >= (float)ui_px(200) && inner >= need &&
            table_w >= (float)ui_px(250))
            L.split = 1;
    }

    if (L.split) {
        float stack = 0.0f;
        int n = 0;
        for (int i = 0; i < nwant && i < 3; i++) {
            float ch = crd_height(cm, want_rows[i], want_spark[i]);
            if (stack + ch + (n ? L.gap : 0.0f) > avail) break;
            stack += ch + (n ? L.gap : 0.0f);
            L.kind[n] = want_kind[i];
            L.sum[n] = (CrdRect){ L.margin, L.margin + (stack - ch), left_w, ch };
            n++;
        }
        L.nsum = n;
    }

    CrdRect card;
    if (L.nsum > 0) {
        card.x = L.margin + left_w + L.gap;
        card.w = (float)w - L.margin - card.x;
    } else {
        card.x = L.margin;
        card.w = (float)w - 2.0f * L.margin;
    }
    card.y = L.margin;
    card.h = avail;
    if (card.w < 1.0f) card.w = 1.0f;
    if (card.h < 1.0f) card.h = 1.0f;
    L.pc = proc_card_layout(&card, cm, fr, fs);

    return L;
}

/* Number of rows the network card needs for the interfaces it will show. */
static int net_card_rows(const SysMon *m) {
    int shown = m->nif < 2 ? m->nif : 2;
    if (shown < 0) shown = 0;
    return shown * 2 + (m->nif > shown ? 1 : 0);
}

/* The one place the dashboard's card set is chosen. The harness calls this
 * too, so the sizes it proves are the sizes the window draws. */
static SysLayout sysmon_layout_for(const SysMon *m, int w, int h,
                                   const Fonts *F, const CrdMetrics *cm) {
    int want_kind[3], want_rows[3], want_spark[3];
    int nwant = 0;
    want_kind[nwant] = K_SYS; want_rows[nwant] = 3; want_spark[nwant] = 1; nwant++;
    want_kind[nwant] = K_MEM; want_rows[nwant] = 2; want_spark[nwant] = 1; nwant++;
    if (m->nif > 0) {
        want_kind[nwant] = K_NET;
        want_rows[nwant] = net_card_rows(m);
        want_spark[nwant] = 0;
        nwant++;
    }
    return sysmon_layout(w, h, m, cm, F->body, F->small,
                         want_kind, want_rows, want_spark, nwant);
}

/* ------------------------------------------------------------ summary cards */

static void draw_sys_card(Surface *s, const CrdRect *c, const CrdMetrics *cm,
                          const CrdStyle *st, Font *ft, Font *fr, Font *fs,
                          const SysMon *m) {
    crd_begin(s, c, cm, st, ft, fs, "SYSTEM", NULL, 0);
    char v[96];
    if (m->load_ok)
        snprintf(v, sizeof(v), "%.2f %.2f %.2f", m->load[0], m->load[1], m->load[2]);
    else
        snprintf(v, sizeof(v), "unreadable");
    crd_row(s, c, cm, st, fr, 0, "Load 1/5/15", v);
    fmt_uptime(m->uptime, v, sizeof(v));
    crd_row(s, c, cm, st, fr, 1, "Uptime", v);
    if (m->ncpu > 0) snprintf(v, sizeof(v), "%d", m->ncpu);
    else             snprintf(v, sizeof(v), "unreadable");
    crd_row(s, c, cm, st, fr, 2, "CPUs", v);
    char legend[64];
    if (m->load_ok)
        snprintf(legend, sizeof(legend), "load 1m, last %d samples", m->load_len);
    else
        snprintf(legend, sizeof(legend), "load: no history");
    crd_spark(s, c, cm, fs, legend, color_alpha(st->text, 0.40f),
              m->hist_load, m->load_head, m->load_len, RING_N, 0.0f, st->accent);
}

static void draw_mem_card(Surface *s, const CrdRect *c, const CrdMetrics *cm,
                          const CrdStyle *st, Font *ft, Font *fr, Font *fs,
                          const SysMon *m) {
    crd_begin(s, c, cm, st, ft, fs, "MEMORY", NULL, 0);
    char used[32], total[32], v[96];
    if (m->mem_ok) {
        long u = m->total_kb - m->avail_kb;
        if (u < 0) u = 0;
        fmt_bytes((double)u * 1024.0, used, sizeof(used));
        fmt_bytes((double)m->total_kb * 1024.0, total, sizeof(total));
        snprintf(v, sizeof(v), "%s of %s", used, total);
        crd_row(s, c, cm, st, fr, 0, "In use", v);
        fmt_bytes((double)m->avail_kb * 1024.0, v, sizeof(v));
        crd_row(s, c, cm, st, fr, 1, "Available", v);
    } else {
        crd_row(s, c, cm, st, fr, 0, "In use", "unreadable");
        crd_row(s, c, cm, st, fr, 1, "Available", "unreadable");
    }
    char legend[64];
    if (m->mem_ok)
        snprintf(legend, sizeof(legend), "used, last %d samples", m->mem_len);
    else
        snprintf(legend, sizeof(legend), "memory: no history");
    crd_spark(s, c, cm, fs, legend, color_alpha(st->text, 0.40f),
              m->hist_mem, m->mem_head, m->mem_len, RING_N, 1.0f, st->accent);
}

static void draw_net_card(Surface *s, const CrdRect *c, const CrdMetrics *cm,
                          const CrdStyle *st, Font *ft, Font *fr, Font *fs,
                          const SysMon *m) {
    char meta[64];
    if (m->dt_ms > 0.5)
        snprintf(meta, sizeof(meta), "rates delta %.2f s", m->dt_ms / 1000.0);
    else
        snprintf(meta, sizeof(meta), "no delta yet");
    crd_begin(s, c, cm, st, ft, fs, "NETWORK", meta, 0);
    int shown = m->nif < 2 ? m->nif : 2;
    int row = 0;
    char label[32], v[96], tot[24], rate[24];
    for (int i = 0; i < shown; i++) {
        const Iface *f = &m->ifs[i];
        fmt_bytes((double)f->rx, tot, sizeof(tot));
        fmt_bytes(f->rx_rate, rate, sizeof(rate));
        snprintf(label, sizeof(label), "%s rx", f->name);
        snprintf(v, sizeof(v), "%s  %s/s", tot, rate);
        crd_row(s, c, cm, st, fr, row++, label, v);

        fmt_bytes((double)f->tx, tot, sizeof(tot));
        fmt_bytes(f->tx_rate, rate, sizeof(rate));
        snprintf(label, sizeof(label), "%s tx", f->name);
        snprintf(v, sizeof(v), "%s  %s/s", tot, rate);
        crd_row(s, c, cm, st, fr, row++, label, v);
    }
    if (m->nif > shown) {
        snprintf(v, sizeof(v), "+%d interfaces", m->nif - shown);
        crd_row(s, c, cm, st, fr, row++, "other", v);
    }
}

/* ------------------------------------------------------------ frame */

static void draw_frame(SysMon *m, Surface *s, const SysLayout *L,
                       const Fonts *F, const ProcView *pv, int w, int h) {
    gfx_gradient(s, 0, 0, w, h, C_BG_TOP, C_BG_BOT);
    CrdStyle sty = crd_style();
    CrdMetrics cm = crd_metrics(F->head, F->body, F->small);

    for (int i = 0; i < L->nsum; i++) {
        switch (L->kind[i]) {
        case K_SYS: draw_sys_card(s, &L->sum[i], &cm, &sty, F->head, F->body, F->small, m); break;
        case K_MEM: draw_mem_card(s, &L->sum[i], &cm, &sty, F->head, F->body, F->small, m); break;
        case K_NET: draw_net_card(s, &L->sum[i], &cm, &sty, F->head, F->body, F->small, m); break;
        default: break;
        }
    }

    /* The card's title line carries the interval the CPU% delta was measured
     * over. "delta" is the word that matters: every CPU% here is a change
     * between two samples, not a total. Short enough for 320 px at 160%. */
    char meta[64];
    if (m->dt_ms > 0.5)
        snprintf(meta, sizeof(meta), "delta %.2f s", m->dt_ms / 1000.0);
    else
        snprintf(meta, sizeof(meta), "no delta yet");
    proc_draw(s, &L->pc, &cm, &sty, F->head, F->body, F->small, pv, "PROCESSES", meta);

    if (L->legend) {
        char legend[192], out[192];
        snprintf(legend, sizeof(legend),
                 "1-5 or click: sort    S: reverse    f: filter    j/k: move    K: kill");
        /* q only exists in this process, and only gets a hint when the line
         * has room for it; the shared keys read exactly like Monitor's. */
        if (font_width(F->small, legend) + font_width(F->small, "    q: quit") <=
            (float)w - 2.0f * L->margin)
            snprintf(legend + strlen(legend), sizeof(legend) - strlen(legend),
                     "    q: quit");
        crd_fit(F->small, legend, out, sizeof(out), (float)w - 2.0f * L->margin);
        float ly = (float)h - L->legend_h;
        font_draw(s, F->small, L->margin,
                  crd_top(F->small, crd_base(F->small, ly, L->legend_h)), out,
                  color_alpha(C_TEXT, 0.40f));
    }

}

/* ------------------------------------------------------------ actions */

static void set_msg(SysMon *m, const char *text) {
    snprintf(m->msg, sizeof(m->msg), "%s", text);
    m->msg_at = mono_ms();
}

static int proc_alive(int pid) {
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static void do_kill(SysMon *m) {
    if (m->sel_pid <= 0) { set_msg(m, "select a process first"); return; }
    int pid = m->sel_pid;
    const Proc *p = NULL;
    for (int i = 0; i < m->nproc; i++)
        if (m->procs[i].pid == pid) { p = &m->procs[i]; break; }
    char name[32] = "?";
    if (p) snprintf(name, sizeof(name), "%s", p->comm);
    char buf[160];

    if (pid <= 1) {
        snprintf(buf, sizeof(buf), "refused: pid %d is init", pid);
        set_msg(m, buf);
    } else if (pid == (int)getpid()) {
        snprintf(buf, sizeof(buf), "refused: pid %d is me", pid);
        set_msg(m, buf);
    } else if (kill(pid, SIGTERM) == 0) {
        snprintf(buf, sizeof(buf), "SIGTERM -> %d %s", pid, name);
        set_msg(m, buf);
        m->pending_pid = pid;
        m->pending_at = mono_ms() + KILL_GRACE_MS;
    } else {
        snprintf(buf, sizeof(buf), "SIGTERM to %d failed: %s", pid, strerror(errno));
        set_msg(m, buf);
    }
}

static void check_pending(SysMon *m) {
    if (m->pending_pid <= 0) return;
    if (mono_ms() < m->pending_at) return;
    int pid = m->pending_pid;
    m->pending_pid = 0;
    char buf[160];
    if (!proc_alive(pid)) {
        snprintf(buf, sizeof(buf), "pid %d exited after SIGTERM", pid);
    } else if (kill(pid, SIGKILL) == 0) {
        snprintf(buf, sizeof(buf), "pid %d ignored SIGTERM: SIGKILL sent", pid);
    } else {
        snprintf(buf, sizeof(buf), "SIGKILL to %d failed: %s", pid, strerror(errno));
    }
    set_msg(m, buf);
}

/* ------------------------------------------------------------ view helpers */

static void refresh_view(SysMon *m) {
    m->nview = proc_view_build(m->procs, m->nproc, m->sort_key, m->sort_dir,
                               m->filter, m->view);
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0 && m->nview > 0) {
        m->sel_pid = m->procs[m->view[0]].pid;
        m->scroll = 0;
    }
}

static void reveal_selection(SysMon *m) {
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0) return;
    if (sel < m->scroll) m->scroll = sel;
    if (m->rows > 0 && sel >= m->scroll + m->rows) m->scroll = sel - m->rows + 1;
    if (m->rows > 0 && m->scroll > m->nview - m->rows) m->scroll = m->nview - m->rows;
    if (m->scroll < 0) m->scroll = 0;
}

static void move_sel(SysMon *m, int delta) {
    if (m->nview <= 0) return;
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0) sel = 0;
    sel += delta;
    if (sel < 0) sel = 0;
    if (sel >= m->nview) sel = m->nview - 1;
    m->sel_pid = m->procs[m->view[sel]].pid;
    reveal_selection(m);
}

static void set_sort(SysMon *m, int key) {
    if (m->sort_key == key) m->sort_dir = -m->sort_dir;
    else {
        m->sort_key = key;
        m->sort_dir = (key == S_CPU || key == S_MEM) ? -1 : 1;
    }
    m->scroll = 0;
    refresh_view(m);
    reveal_selection(m);
}

/* ------------------------------------------------------------ main */

#ifndef SYSMON_HOST_TEST

int main(void) {
    MClient c;
    if (mc_open(&c, "sysmon", "System Monitor", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "sysmon: cannot connect to the window server\n");
        return 1;
    }

    if (font_init("/usr/share/fonts") != 0) {
        fprintf(stderr, "sysmon: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }
    Fonts F;
    F.head  = font_get(FONT_SEMIBOLD, 12);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_REGULAR, 10);
    if (!F.head || !F.body || !F.small) {
        fprintf(stderr, "sysmon: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    static SysMon m;
    memset(&m, 0, sizeof(m));
    m.hz = (int)sysconf(_SC_CLK_TCK);
    if (m.hz <= 0) m.hz = 100;
    m.page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (m.page_kb <= 0) m.page_kb = 4;
    m.sort_key = S_CPU;
    m.sort_dir = -1;
    m.sel_pid = 0;

    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;

    double last_ms = mono_ms();
    sample(&m, 0.0);
    refresh_view(&m);

    double next_sample = mono_ms() + 1000.0;
    int dirty = 1;
    int was_down = 0;

    for (;;) {
        double now = mono_ms();

        check_pending(&m);
        if (now >= next_sample) {
            sample(&m, now - last_ms);
            last_ms = now;
            refresh_view(&m);
            next_sample = now + 1000.0;
            dirty = 1;
        }
        if (m.msg[0] && m.msg_at > 0.0 && now - m.msg_at > 5000.0) {
            m.msg[0] = 0;
            dirty = 1;
        }

        CrdMetrics cm = crd_metrics(F.head, F.body, F.small);
        SysLayout L = sysmon_layout_for(&m, vw, vh, &F, &cm);
        m.rows = L.pc.rows;
        reveal_selection(&m);
        if (m.scroll > m.nview - L.pc.rows) m.scroll = m.nview - L.pc.rows;
        if (m.scroll < 0) m.scroll = 0;

        if (dirty) {
            ProcView pv;
            memset(&pv, 0, sizeof(pv));
            pv.procs = m.procs;
            pv.view = m.view;
            pv.nview = m.nview;
            pv.ntotal = m.nproc;
            pv.scroll = m.scroll;
            pv.sel_pid = m.sel_pid;
            pv.sort_key = m.sort_key;
            pv.sort_dir = m.sort_dir;
            snprintf(pv.filter, sizeof(pv.filter), "%s", m.filter);
            pv.filter_edit = m.filter_edit;
            char note[160];
            note[0] = 0;
            if (m.msg[0] && mono_ms() - m.msg_at < 5000.0)
                snprintf(note, sizeof(note), "%s", m.msg);
            else if (m.truncated)
                snprintf(note, sizeof(note), "list full: %d in /proc", m.nproc);
            pv.note = note;
            pv.note_warn = m.truncated;

            Surface s = { .px = c.px, .w = vw, .h = vh, .stride = c.stride };
            draw_frame(&m, &s, &L, &F, &pv, vw, vh);
            mc_present(&c);
            dirty = 0;
        }

        int got = 0;
        MEvent ev;
        while (mc_poll(&c, &ev)) {
            got = 1;
            int quit = 0;

            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;

            case MC_RESIZE:
                vw = ev.w < cap_w ? ev.w : cap_w;
                vh = ev.h < cap_h ? ev.h : cap_h;
                if (vw < 1) vw = 1;
                if (vh < 1) vh = 1;
                dirty = 1;
                break;

            case MC_TEXT: {
                int filter_dirty = 0;
                for (const char *p = ev.text; *p; p++) {
                    char ch = *p;
                    if (m.filter_edit) {
                        size_t n = strlen(m.filter);
                        if (n + 1 < sizeof(m.filter) &&
                            (unsigned char)ch >= 32 && ch != 127) {
                            m.filter[n] = ch;
                            m.filter[n + 1] = 0;
                            filter_dirty = 1;
                        }
                    } else if (ch == '/') {
                        m.filter_edit = 1;
                    } else if (ch == 'f') {
                        m.filter_edit = 1;
                    } else if (ch == 'c') {
                        m.filter[0] = 0;
                        filter_dirty = 1;
                    } else if (ch == 'q' || ch == 'Q') {
                        quit = 1;
                    } else if (ch == 'K') {
                        do_kill(&m);
                    } else if (ch == 's') {
                        set_sort(&m, (m.sort_key + 1) % S_N);
                    } else if (ch == 'S') {
                        m.sort_dir = -m.sort_dir;
                        m.scroll = 0;
                    } else if (ch >= '1' && ch < '1' + S_N) {
                        set_sort(&m, ch - '1');
                    }
                }
                /* a changed filter starts the list at its first match, the
                 * same rule monitor.c's filter_changed() uses */
                if (filter_dirty) m.scroll = 0;
                refresh_view(&m);
                reveal_selection(&m);
                dirty = 1;
                break;
            }

            case MC_KEY: {
                if (!ev.press) break;
                if (m.filter_edit) {
                    if (ev.key == 1) {
                        m.filter_edit = 0;
                        if (!m.filter[0]) refresh_view(&m);
                        dirty = 1;
                    } else if (ev.key == 28) {
                        m.filter_edit = 0;
                        dirty = 1;
                    } else if (ev.key == 14) {
                        size_t n = strlen(m.filter);
                        if (n) m.filter[n - 1] = 0;
                        refresh_view(&m);
                        m.scroll = 0;
                        dirty = 1;
                    }
                    break;
                }
                switch (ev.key) {
                case 103: move_sel(&m, -1); dirty = 1; break;
                case 108: move_sel(&m,  1); dirty = 1; break;
                case 104: move_sel(&m, -L.pc.rows); dirty = 1; break;
                case 109: move_sel(&m,  L.pc.rows); dirty = 1; break;
                case 102: move_sel(&m, -m.nview); dirty = 1; break;
                case 107: move_sel(&m,  m.nview); dirty = 1; break;
                default: break;
                }
                break;
            }

            case MC_WHEEL: {
                m.scroll -= ev.wheel * 3;
                if (m.scroll > m.nview - L.pc.rows) m.scroll = m.nview - L.pc.rows;
                if (m.scroll < 0) m.scroll = 0;
                dirty = 1;
                break;
            }

            case MC_MOUSE: {
                int x = ev.x, y = ev.y;
                int down = ev.buttons & 1;
                if (down && !was_down) {
                    if (L.pc.show_filter && crd_hit(&L.pc.filter, x, y)) {
                        m.filter_edit = 1;
                        dirty = 1;
                    } else if (L.pc.show_head && crd_hit(&L.pc.head, x, y)) {
                        set_sort(&m, proc_col_at(&L.pc, x));
                        dirty = 1;
                    } else if (y >= (int)L.pc.body_y) {
                        int r = (int)(((float)y - L.pc.body_y) / L.pc.row_h);
                        int vi = m.scroll + r;
                        if (r >= 0 && r < L.pc.rows && vi >= 0 && vi < m.nview) {
                            m.sel_pid = m.procs[m.view[vi]].pid;
                            dirty = 1;
                        }
                    }
                }
                was_down = down;
                break;
            }

            default:
                break;
            }
            if (quit) {
                mc_close(&c);
                return 0;
            }
        }

        if (!got && !dirty) usleep(20 * 1000);
    }
}

#else /* SYSMON_HOST_TEST */

/* The host harness lives in .work/systemui; this file keeps only the clock
 * and kill stand-ins above. Nothing here is compiled into the OS app. */

#endif /* SYSMON_HOST_TEST */
