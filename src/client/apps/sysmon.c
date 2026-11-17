/* ============================================================
 *  Mystic OS - apps (external) / sysmon
 *
 *  A system monitor that runs as its own process. Everything it
 *  draws was read from the kernel moments earlier:
 *
 *    /proc/cpuinfo        "processor" lines           -> cpu count
 *    /proc/loadavg        the three load averages     -> numbers + sparkline
 *    /proc/meminfo        MemTotal / MemAvailable     -> memory % + sparkline
 *    /proc/uptime         first field                 -> uptime
 *    /proc/<pid>/stat     fields 3, 14, 15            -> state, utime+stime
 *    /proc/<pid>/statm    fields 1, 2                 -> virtual / resident
 *    /proc/<pid>/comm     the process name
 *    /proc/net/dev        per-interface rx/tx bytes   -> totals and rates
 *
 *  CPU% is a DELTA, and the screen says so: for every pid we keep the
 *  utime+stime jiffies from the previous sample, subtract, and divide
 *  by the measured wall time between the two samples. A lifetime
 *  average would be a different (and much less useful) number. A
 *  process can legitimately show more than 100% on a multi-core box.
 *
 *  Memory is statm field 2 (resident pages) times the real page size,
 *  not a guess.
 *
 *  Kill sends SIGTERM; if the pid is still there two seconds later it
 *  sends SIGKILL. It refuses pid 1, pid 0 and this monitor's own pid.
 *  kill(2) errors are shown with strerror.
 *
 *  The two graphs are ring buffers inside this process: one sample per
 *  second, RING_N slots, so the window is the last three minutes.
 *
 *  Mouse: click a column heading to sort, click a row to select it,
 *  click the filter box (or press /) and type to filter. Wheel scrolls.
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
#define MAX_ROWS 60

#define KILL_GRACE_MS 2000.0  /* SIGTERM -> SIGKILL */

/* ------------------------------------------------------------ colours */

static const u32 C_BG_TOP = RGB(0x0D, 0x14, 0x22);
static const u32 C_BG_BOT = RGB(0x18, 0x24, 0x3A);
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_MEM    = RGB(0x7A, 0xA2, 0xF7);
static const u32 C_WARN   = RGB(0xE0, 0x6C, 0x5C);
static const u32 C_NET    = RGB(0x7B, 0xD8, 0x8F);
static const u32 C_PANEL  = RGB(0x12, 0x1B, 0x2B);

/* The OS build reads the real /proc. The host harness points the same
 * parsers at a fixture tree it wrote itself, so nothing in the shipped
 * object changes. */
#ifdef SYSMON_HOST_TEST
#  define PROC_ROOT ".work/paintmon/fake/proc"
#else
#  define PROC_ROOT "/proc"
#endif

#ifndef SIGKILL
#  define SIGKILL 9
#endif

/* ------------------------------------------------------------ data */

typedef struct {
    int  pid;
    char comm[24];
    char state;
    unsigned long long utime, stime;   /* jiffies from /proc/<pid>/stat */
    long size_pages;                   /* statm field 1 */
    long rss_pages;                    /* statm field 2 */
    long mem_kb;                       /* rss_pages * the real page size */
    int  cpu10;                        /* tenths of a percent, -1 if unknown */
} Proc;

typedef struct {
    char name[16];
    unsigned long long rx, tx;         /* byte counters, cumulative */
    double rx_rate, tx_rate;           /* bytes per second, from the delta */
} Iface;

typedef struct {
    /* one snapshot */
    int    ncpu;
    int    nproc;
    int    truncated;                  /* MAX_PROC rows and more pids existed */
    int    hz;                         /* sysconf(_SC_CLK_TCK) */
    long   page_kb;
    double load[3];
    long   total_kb, avail_kb;
    double uptime;
    double dt_ms;                      /* measured gap between samples */
    int    samples;

    Proc   procs[MAX_PROC];
    Iface  ifs[MAX_IF];
    int    nif;

    /* ring buffers: hist_head is the slot the next sample overwrites */
    float  hist_load[RING_N];
    float  hist_mem[RING_N];
    int    hist_head, hist_len;

    /* interaction */
    int    sort_key, sort_dir;         /* S_* and +1 ascending / -1 descending */
    char   filter[32];
    int    filter_edit;
    int    sel_pid;
    int    scroll;
    int    hover;                      /* row index under the pointer, -1 */
    int    dlg;                        /* kill confirmation is up */
    int    kill_pid;
    char   kill_comm[24];
    int    pending_pid;
    double pending_at;                 /* when SIGKILL is due */
    char   msg[160];
    double msg_at;
} SysMon;

enum { S_PID, S_NAME, S_STATE, S_CPU, S_MEM, S_N };

static const char *SORT_NAME[S_N] = { "pid", "name", "state", "cpu", "memory" };

/* ------------------------------------------------------------ clock */

#ifdef SYSMON_HOST_TEST
static double g_host_ms;
static double mono_ms(void) { return g_host_ms; }
static void host_advance(double ms) { g_host_ms += ms; }
/* kill(2) does not exist on the Windows host; this records the signal and
 * pretends the call succeeded, so the SIGTERM -> SIGKILL state machine is
 * still exercised end to end. pid 999999 reports ESRCH. */
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

static void read_loadavg(SysMon *m) {
    m->load[0] = m->load[1] = m->load[2] = 0.0;
    FILE *f = fopen(PROC_ROOT "/loadavg", "r");
    if (!f) return;
    double a = 0.0, b = 0.0, c = 0.0;
    if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) {
        m->load[0] = a;
        m->load[1] = b;
        m->load[2] = c;
    }
    fclose(f);
}

static void read_meminfo(SysMon *m) {
    m->total_kb = 0;
    m->avail_kb = 0;
    long free_kb = 0;
    FILE *f = fopen(PROC_ROOT "/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))
            m->total_kb = strtol(line + 9, NULL, 10);
        else if (!strncmp(line, "MemAvailable:", 13))
            m->avail_kb = strtol(line + 13, NULL, 10);
        else if (!strncmp(line, "MemFree:", 8))
            free_kb = strtol(line + 8, NULL, 10);
    }
    fclose(f);
    /* MemAvailable arrived in 3.14; older kernels only have MemFree */
    if (m->avail_kb == 0 && free_kb > 0) m->avail_kb = free_kb;
}

static double read_uptime(void) {
    FILE *f = fopen(PROC_ROOT "/uptime", "r");
    if (!f) return 0.0;
    double up = 0.0;
    if (fscanf(f, "%lf", &up) != 1) up = 0.0;
    fclose(f);
    return up > 0.0 ? up : 0.0;
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

/* /proc/<pid>/stat: the command may contain spaces and ')' so the last
 * ')' on the line is the one that closes it; every field after it is a
 * number or a state letter. */
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

/* statm is "size resident shared text lib data dt" in pages */
static int read_statm(int pid, long *size_pages, long *rss_pages) {
    char path[64];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long a = 0, b = 0;
    int got = fscanf(f, "%ld %ld", &a, &b);
    fclose(f);
    if (got != 2) return -1;
    *size_pages = a;
    *rss_pages = b;
    return 0;
}

static const Proc *find_pid(const SysMon *m, int pid) {
    for (int i = 0; i < m->nproc; i++)
        if (m->procs[i].pid == pid) return &m->procs[i];
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
    double dt = dt_ms;
    m->dt_ms = dt;

    m->ncpu = read_ncpu();
    read_loadavg(m);
    read_meminfo(m);
    m->uptime = read_uptime();
    read_net(m, dt);

    static Proc next[MAX_PROC];
    int n = 0;
    m->truncated = 0;
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
            tmp.cpu10 = -1;
            if (read_stat(pid, &tmp) != 0) continue;      /* it died mid-scan */
            if (read_statm(pid, &tmp.size_pages, &tmp.rss_pages) != 0) continue;
            tmp.mem_kb = tmp.rss_pages * m->page_kb;
            if (tmp.mem_kb < 0) tmp.mem_kb = 0;

            const Proc *old = m->samples > 0 ? find_pid(m, pid) : NULL;
            if (old && dt > 0.5) {
                unsigned long long a = old->utime + old->stime;
                unsigned long long b = tmp.utime + tmp.stime;
                if (b > a) {
                    double pct10 = (double)(b - a) * 1000000.0 /
                                   ((double)m->hz * dt);
                    double ceiling = 1000.0 * (double)(m->ncpu > 0 ? m->ncpu : 1);
                    if (pct10 > ceiling) pct10 = ceiling;
                    tmp.cpu10 = (int)(pct10 + 0.5);
                } else {
                    tmp.cpu10 = 0;
                }
            }
            next[n++] = tmp;
        }
        closedir(d);
    }
    memcpy(m->procs, next, sizeof(Proc) * (size_t)n);
    m->nproc = n;

    /* one point per graph, per second */
    m->hist_load[m->hist_head] = (float)m->load[0];
    long used = m->total_kb - m->avail_kb;
    if (used < 0) used = 0;
    m->hist_mem[m->hist_head] = m->total_kb > 0
        ? (float)((double)used / (double)m->total_kb) : 0.0f;
    m->hist_head = (m->hist_head + 1) % RING_N;
    if (m->hist_len < RING_N) m->hist_len++;

    m->samples++;
}

/* ------------------------------------------------------------ view */

static const SysMon *g_m;
static int g_key;
static int g_dir;

static int ci_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int cmp_proc(const void *pa, const void *pb) {
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const Proc *a = &g_m->procs[ia];
    const Proc *b = &g_m->procs[ib];
    int r = 0;
    switch (g_key) {
    case S_PID:
        r = a->pid - b->pid;
        break;
    case S_NAME:
        r = ci_cmp(a->comm, b->comm);
        break;
    case S_STATE:
        r = (int)a->state - (int)b->state;
        break;
    case S_CPU:
        r = a->cpu10 - b->cpu10;
        break;
    case S_MEM:
        r = a->mem_kb > b->mem_kb ? 1 : (a->mem_kb < b->mem_kb ? -1 : 0);
        break;
    default:
        break;
    }
    if (r == 0) r = a->pid - b->pid;
    return g_dir > 0 ? r : -r;
}

static int view_match(const Proc *p, const char *f) {
    if (!f[0]) return 1;
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", p->pid);
    if (strstr(pid, f)) return 1;
    size_t fl = strlen(f);
    for (const char *q = p->comm; *q; q++) {
        size_t i = 0;
        while (i < fl && q[i] &&
               tolower((unsigned char)q[i]) == tolower((unsigned char)f[i]))
            i++;
        if (i == fl) return 1;
        if (!q[1]) break;
    }
    return 0;
}

/* Rebuild the visible order: filter first, then sort. */
static int build_view(SysMon *m, int *view) {
    int n = 0;
    for (int i = 0; i < m->nproc && n < MAX_PROC; i++)
        if (view_match(&m->procs[i], m->filter)) view[n++] = i;
    g_m = m;
    g_key = m->sort_key;
    g_dir = m->sort_dir;
    if (n > 1) qsort(view, (size_t)n, sizeof(int), cmp_proc);
    return n;
}

static int view_index_of(const SysMon *m, const int *view, int n, int pid) {
    for (int i = 0; i < n; i++)
        if (m->procs[view[i]].pid == pid) return i;
    return -1;
}

/* ------------------------------------------------------------ text */

static void text_right(Surface *s, Font *f, int xr, float y, const char *t, u32 col) {
    font_draw(s, f, (float)xr - font_width(f, t), y, t, col);
}

static void fit(char *t, Font *f, float maxw) {
    size_t n = strlen(t);
    while (n > 1 && font_width(f, t) > maxw) {
        t[--n] = 0;
        while (n > 1 && ((u8)t[n] & 0xC0) == 0x80) t[--n] = 0;
    }
}

static void fmt_bytes(double v, char *out, size_t n) {
    if (v < 1024.0) snprintf(out, n, "%.0f B", v);
    else if (v < 1024.0 * 1024.0) snprintf(out, n, "%.1f kB", v / 1024.0);
    else if (v < 1024.0 * 1024.0 * 1024.0) snprintf(out, n, "%.1f MB", v / (1024.0 * 1024.0));
    else snprintf(out, n, "%.2f GB", v / (1024.0 * 1024.0 * 1024.0));
}

static void fmt_uptime(double secs, char *out, size_t n) {
    unsigned long s = (unsigned long)(secs > 0.0 ? secs : 0.0);
    unsigned long d = s / 86400UL;
    unsigned long hh = (s % 86400UL) / 3600UL;
    unsigned long mm = (s % 3600UL) / 60UL;
    unsigned long ss = s % 60UL;
    if (d) snprintf(out, n, "%lud %02lu:%02lu:%02lu", d, hh, mm, ss);
    else snprintf(out, n, "%02lu:%02lu:%02lu", hh, mm, ss);
}

/* ------------------------------------------------------------ layout */

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    int  vw, vh;
    Rect head;
    Rect spark_load, spark_mem;
    Rect net;
    Rect filter, kill_btn;
    Rect thead, rows;
    Rect dlg, dlg_ok, dlg_no;
    int  col[6];          /* column boundaries: 0..5 */
    int  row_h, rows_visible, rows_top, rows_bottom;
    int  y_foot[3];
} Layout;

typedef struct {
    Font *head, *body, *small, *mono;
} Fonts;

static void layout_build(int vw, int vh, const Fonts *F, Layout *L) {
    memset(L, 0, sizeof(*L));
    L->vw = vw;
    L->vh = vh;

    int gh = (int)font_height(F->head);
    int sh = (int)font_height(F->small);
    int bh = (int)font_height(F->body);
    if (gh < 1) gh = 1;
    if (sh < 1) sh = 1;
    if (bh < 1) bh = 1;

    L->head = (Rect){ 12, 8, vw - 24, gh };

    int y = 8 + gh + 4;
    int gap = 12;
    int pw = (vw - 24 - gap) / 2;
    L->spark_load = (Rect){ 12, y, pw, 74 };
    L->spark_mem  = (Rect){ 12 + pw + gap, y, pw, 74 };
    y += 74 + 8;

    L->net = (Rect){ 12, y, vw - 24, 3 * sh };
    y += 3 * sh + 4;

    L->filter = (Rect){ 12, y, vw - 24 - 96, sh + 4 };
    L->kill_btn = (Rect){ vw - 12 - 88, y, 88, sh + 4 };
    y += sh + 4 + 6;

    L->thead = (Rect){ 12, y, vw - 24, sh + 2 };
    y += sh + 2;

    int foot_top = vh - 3 * (sh + 1) - 6;
    L->rows_top = y;
    L->rows_bottom = foot_top - 4;
    L->rows = (Rect){ 12, L->rows_top, vw - 24, L->rows_bottom - L->rows_top };
    L->row_h = sh + 3;
    L->rows_visible = (L->rows_bottom - L->rows_top) / L->row_h;
    if (L->rows_visible < 1) L->rows_visible = 1;
    if (L->rows_visible > MAX_ROWS) L->rows_visible = MAX_ROWS;

    for (int i = 0; i < 3; i++) L->y_foot[i] = foot_top + i * (sh + 1);

    L->col[0] = 12;
    L->col[1] = 74;
    L->col[2] = vw - 300;
    L->col[3] = vw - 190;
    L->col[4] = vw - 100;
    L->col[5] = vw - 12;
    if (L->col[2] < L->col[1] + 40) L->col[2] = L->col[1] + 40;

    int dw = 420;
    if (dw > vw - 20) dw = vw - 20;
    int dh = 14 + gh + 6 + 2 * sh + 14 + 26 + 14;
    L->dlg = (Rect){ (vw - dw) / 2, (vh - dh) / 2, dw, dh };
    L->dlg_ok = (Rect){ L->dlg.x + 16, L->dlg.y + dh - 40, 180, 26 };
    L->dlg_no = (Rect){ L->dlg.x + dw - 136, L->dlg.y + dh - 40, 120, 26 };
}

static int rect_hit(const Rect *r, int x, int y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* ------------------------------------------------------------ drawing */

/* A crisp 2 px polyline straight into the surface: gfx_line is
 * antialiased and a flat graph at 63% coverage reads as a smudge, while a
 * sparkline wants to be a hard line. */
static void spark_line(Surface *s, int x0, int y0, int x1, int y1, u32 col) {
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

static void draw_spark(Surface *s, Font *f_small, const Rect *r,
                       const float *hist, int head, int len,
                       float vmax_fixed, const char *title, const char *now,
                       u32 col) {
    gfx_rrect_aa(s, (float)r->x, (float)r->y, (float)r->w, (float)r->h, 6.0f,
                 C_PANEL, color_alpha(C_TEXT, 0.10f), 1.0f);
    font_draw(s, f_small, (float)r->x + 9.0f, (float)r->y + 3.0f, title,
              color_alpha(C_TEXT, 0.55f));
    if (now && font_width(f_small, title) + font_width(f_small, now) + 26.0f < (float)r->w)
        text_right(s, f_small, r->x + r->w - 9, (float)r->y + 3.0f, now, col);

    int px0 = r->x + 9, px1 = r->x + r->w - 9;
    int py0 = r->y + (int)font_height(f_small) + 6;
    int py1 = r->y + r->h - 8;
    if (px1 - px0 < 20 || py1 - py0 < 10) return;

    float vmax = vmax_fixed;
    if (vmax <= 0.0f) {
        vmax = 0.0f;
        for (int i = 0; i < len; i++) {
            int idx = (head - len + i + RING_N) % RING_N;
            if (hist[idx] > vmax) vmax = hist[idx];
        }
        if (vmax < 1.0f) vmax = 1.0f;   /* idle is not thrashing */
    }

    /* two hairlines: the floor, and the top of the scale */
    surf_blend(s, px0, py1, px1 - px0, 1, color_alpha(C_TEXT, 0.16f));
    surf_blend(s, px0, py0, px1 - px0, 1, color_alpha(C_TEXT, 0.06f));

    float plotw = (float)(px1 - px0);
    float ph = (float)(py1 - py0);
    float step = plotw / (float)(RING_N - 1);
    float prevx = 0.0f, prevy = 0.0f;
    int have = 0;
    float lastx = (float)px1, lasty = (float)py1;

    for (int i = 0; i < len; i++) {
        int idx = (head - len + i + RING_N) % RING_N;
        float v = hist[idx];
        if (v < 0.0f) v = 0.0f;
        if (v > vmax) v = vmax;
        /* newest sample on the right edge, history grows leftwards */
        float x = (float)px1 - (float)(len - 1 - i) * step;
        float y = (float)py1 - (v / vmax) * ph;
        if (y < (float)py0) y = (float)py0;
        if (y > (float)(py1 - 1)) y = (float)(py1 - 1);
        if (have)
            spark_line(s, (int)(prevx + 0.5f), (int)(prevy + 0.5f),
                       (int)(x + 0.5f), (int)(y + 0.5f), col);
        prevx = x;
        prevy = y;
        lastx = x;
        lasty = y;
        have = 1;
    }
    if (have)
        surf_fill(s, (int)lastx - 1, (int)lasty - 1, 3, 3, col);
}

static void draw_sort_mark(Surface *s, int x, int y, int dir, u32 col) {
    float pts[6];
    if (dir > 0) {                        /* ascending: triangle pointing up */
        pts[0] = (float)x;      pts[1] = (float)y + 7.0f;
        pts[2] = (float)x + 8.0f; pts[3] = (float)y + 7.0f;
        pts[4] = (float)x + 4.0f; pts[5] = (float)y;
    } else {
        pts[0] = (float)x;      pts[1] = (float)y;
        pts[2] = (float)x + 8.0f; pts[3] = (float)y;
        pts[4] = (float)x + 4.0f; pts[5] = (float)y + 7.0f;
    }
    gfx_polygon(s, pts, 3, col);
}

static void draw_frame(SysMon *m, Surface *s, const Layout *L, const Fonts *F,
                       const int *view, int view_n) {
    int vw = L->vw, vh = L->vh;
    if (vw < 200 || vh < 200) return;

    gfx_gradient(s, 0, 0, vw, vh, C_BG_TOP, C_BG_BOT);
    const u32 rule = color_alpha(C_TEXT, 0.10f);
    const u32 label = color_alpha(C_TEXT, 0.50f);
    const u32 faint = color_alpha(C_TEXT, 0.35f);
    char buf[160];

    /* ---- header ---- */
    font_draw(s, F->head, 12.0f, 8.0f, "SYSTEM MONITOR", C_TEXT);
    {
        char up[32];
        fmt_uptime(m->uptime, up, sizeof(up));
        snprintf(buf, sizeof(buf), "pid %d    up %s    %d cpu    %d proc",
                 (int)getpid(), up, m->ncpu, m->nproc);
        text_right(s, F->small, vw - 12, 10.0f, buf, C_DIM);
    }
    surf_blend(s, 12, 8 + (int)font_height(F->head) + 1, vw - 24, 1, rule);

    /* ---- load ---- */
    {
        long used = m->total_kb - m->avail_kb;
        if (used < 0) used = 0;
        char now[80];
        snprintf(now, sizeof(now), "1m %.2f   5m %.2f   15m %.2f",
                 (double)m->load[0], (double)m->load[1], (double)m->load[2]);
        draw_spark(s, F->small, &L->spark_load, m->hist_load, m->hist_head,
                   m->hist_len, 0.0f, "LOAD AVERAGE (1 min)", now, C_ACCENT);

        char now2[96];
        double frac = m->total_kb > 0 ? (double)used / (double)m->total_kb : 0.0;
        snprintf(now2, sizeof(now2), "%.0f%%   %ld of %ld MB",
                 frac * 100.0, used / 1024, m->total_kb / 1024);
        draw_spark(s, F->small, &L->spark_mem, m->hist_mem, m->hist_head,
                   m->hist_len, 1.0f, "MEMORY USED", now2, C_MEM);
    }

    /* ---- network ---- */
    {
        int y = L->net.y;
        int shown = 0;
        for (int i = 0; i < m->nif && shown < 3; i++) {
            const Iface *f = &m->ifs[i];
            if (f->rx == 0 && f->tx == 0) continue;
            char rx[24], tx[24], rr[24], tr[24];
            fmt_bytes((double)f->rx, rx, sizeof(rx));
            fmt_bytes((double)f->tx, tx, sizeof(tx));
            fmt_bytes(f->rx_rate, rr, sizeof(rr));
            fmt_bytes(f->tx_rate, tr, sizeof(tr));
            font_draw(s, F->mono, 12.0f, (float)y, f->name, C_NET);
            snprintf(buf, sizeof(buf), "rx %s   tx %s", rx, tx);
            font_draw(s, F->small, 110.0f, (float)y, buf, C_TEXT);
            snprintf(buf, sizeof(buf), "down %s/s   up %s/s", rr, tr);
            text_right(s, F->small, vw - 12, (float)y, buf, C_DIM);
            y += (int)font_height(F->small);
            shown++;
        }
        if (!shown) {
            font_draw(s, F->small, 12.0f, (float)y,
                      "network: no counters in /proc/net/dev", faint);
        }
    }

    /* ---- filter row ---- */
    {
        int editing = m->filter_edit;
        gfx_rrect_aa(s, (float)L->filter.x, (float)L->filter.y,
                     (float)L->filter.w, (float)L->filter.h, 4.0f,
                     C_PANEL, editing ? C_ACCENT : color_alpha(C_TEXT, 0.14f), 1.0f);
        char line[96];
        if (m->filter[0])
            snprintf(line, sizeof(line), "/ %s", m->filter);
        else
            snprintf(line, sizeof(line), "/ filter by name or pid%s",
                     editing ? "" : "   (press / to type)");
        font_draw(s, F->small, (float)L->filter.x + 8.0f,
                  (float)L->filter.y + 2.0f, line, m->filter[0] ? C_TEXT : faint);
        if (editing) {
            float cx = (float)L->filter.x + 8.0f + font_width(F->small, line);
            surf_fill(s, (int)cx + 1, L->filter.y + 3, 2,
                      L->filter.h - 6, C_ACCENT);
        }
        snprintf(buf, sizeof(buf), "%d shown of %d%s", view_n, m->nproc,
                 m->truncated ? " (list full)" : "");
        text_right(s, F->small, L->filter.x + L->filter.w - 8,
                   (float)L->filter.y + 2.0f, buf, label);

        int can = m->sel_pid > 0;
        gfx_rrect_aa(s, (float)L->kill_btn.x, (float)L->kill_btn.y,
                     (float)L->kill_btn.w, (float)L->kill_btn.h, 4.0f,
                     can ? color_alpha(C_WARN, 0.20f) : C_PANEL,
                     can ? C_WARN : color_alpha(C_TEXT, 0.14f), 1.0f);
        font_draw(s, F->small, (float)L->kill_btn.x + 10.0f,
                  (float)L->kill_btn.y + 2.0f, "kill  (k)",
                  can ? C_WARN : faint);
    }

    /* ---- table header ---- */
    {
        const char *nm[5] = { "PID", "NAME", "STATE", "CPU%", "MEM" };
        int align_r[5] = { 0, 0, 0, 1, 1 };
        int y = L->thead.y;
        for (int i = 0; i < 5; i++) {
            int x = L->col[i];
            int limit = L->col[i + 1] - 6;
            if (align_r[i]) {
                text_right(s, F->small, limit, (float)y, nm[i],
                           m->sort_key == i ? C_ACCENT : label);
            } else {
                font_draw(s, F->small, (float)x, (float)y, nm[i],
                          m->sort_key == i ? C_ACCENT : label);
            }
            if (m->sort_key == i) {
                int mx = align_r[i] ? limit - (int)font_width(F->small, nm[i]) - 11
                                    : x + (int)font_width(F->small, nm[i]) + 5;
                draw_sort_mark(s, mx, y + 4, m->sort_dir, C_ACCENT);
            }
        }
        /* a line under the heading */
        surf_blend(s, 12, L->thead.y + L->thead.h, vw - 24, 1, rule);
    }

    /* ---- rows ---- */
    {
        int sel = view_index_of(m, view, view_n, m->sel_pid);
        surf_clip(s, L->rows.x, L->rows.y, L->rows.w, L->rows.h);
        int budget = L->rows_visible;
        for (int r = 0; r < budget; r++) {
            int vi = m->scroll + r;
            if (vi >= view_n) break;
            const Proc *p = &m->procs[view[vi]];
            int y = L->rows_top + r * L->row_h;
            if (y + L->row_h > L->rows_bottom) break;

            int rowid = vi;
            if (rowid == sel)
                surf_blend(s, L->rows.x, y, L->rows.w, L->row_h - 1,
                           color_alpha(C_ACCENT, 0.16f));
            else if (rowid == m->hover)
                surf_blend(s, L->rows.x, y, L->rows.w, L->row_h - 1,
                           color_alpha(C_TEXT, 0.05f));
            else if (r & 1)
                surf_blend(s, L->rows.x, y, L->rows.w, L->row_h - 1,
                           color_alpha(C_TEXT, 0.025f));

            float ty = (float)y + 1.0f;
            snprintf(buf, sizeof(buf), "%d", p->pid);
            font_draw(s, F->mono, (float)L->col[0], ty, buf, C_DIM);

            char name[28];
            snprintf(name, sizeof(name), "%s", p->comm);
            fit(name, F->small, (float)(L->col[2] - L->col[1] - 10));
            font_draw(s, F->small, (float)L->col[1], ty, name,
                      rowid == sel ? C_TEXT : color_alpha(C_TEXT, 0.85f));

            char st[2] = { p->state ? p->state : '?', 0 };
            font_draw(s, F->mono, (float)L->col[2], ty, st,
                      p->state == 'R' ? C_ACCENT :
                      (p->state == 'Z' ? C_WARN : C_DIM));

            if (p->cpu10 < 0)
                text_right(s, F->mono, L->col[4] - 6, ty, "-", faint);
            else {
                snprintf(buf, sizeof(buf), "%d.%d", p->cpu10 / 10, p->cpu10 % 10);
                text_right(s, F->mono, L->col[4] - 6, ty, buf,
                           p->cpu10 >= 500 ? C_WARN : C_TEXT);
            }

            if (p->mem_kb >= 1024)
                snprintf(buf, sizeof(buf), "%ld.%ld MB", p->mem_kb / 1024,
                         (p->mem_kb % 1024) * 10 / 1024);
            else
                snprintf(buf, sizeof(buf), "%ld kB", p->mem_kb);
            text_right(s, F->small, L->col[5] - 6, ty, buf, C_TEXT);
        }
        surf_unclip(s);
    }

    /* ---- footer ---- */
    {
        double dt = m->dt_ms > 0.0 ? m->dt_ms : 0.0;
        snprintf(buf, sizeof(buf),
                 "CPU%% is a DELTA since the previous sample (%.2f s), not a lifetime average; over 100%% means more than one core.",
                 dt / 1000.0);
        fit(buf, F->small, (float)(vw - 24));
        font_draw(s, F->small, 12.0f, (float)L->y_foot[0], buf, color_alpha(C_TEXT, 0.55f));

        snprintf(buf, sizeof(buf),
                 "MEM = statm field 2 x %ld kB pages   -   load /proc/loadavg   -   net /proc/net/dev   -   %d samples",
                 m->page_kb, m->samples);
        fit(buf, F->small, (float)(vw - 24));
        font_draw(s, F->small, 12.0f, (float)L->y_foot[1], buf, faint);

        snprintf(buf, sizeof(buf),
                 "arrows move   k kill   / filter   %s %s   q quit",
                 SORT_NAME[m->sort_key], m->sort_dir > 0 ? "asc" : "desc");
        font_draw(s, F->small, 12.0f, (float)L->y_foot[2], buf, faint);
        if (m->msg[0]) {
            /* keep the message clear of the key legend on its left */
            int room = vw - 24 - (int)font_width(F->small, buf) - 12;
            if (room > 40) {
                char m2[160];
                snprintf(m2, sizeof(m2), "%s", m->msg);
                fit(m2, F->small, (float)room);
                text_right(s, F->small, vw - 12, (float)L->y_foot[2], m2, C_ACCENT);
            }
        }
    }

    /* ---- the kill confirmation ---- */
    if (m->dlg) {
        surf_blend(s, 0, 0, vw, vh, color_alpha(RGB(0, 0, 0), 0.55f));
        gfx_rrect_aa(s, (float)L->dlg.x, (float)L->dlg.y,
                     (float)L->dlg.w, (float)L->dlg.h, 8.0f,
                     C_PANEL, C_WARN, 1.5f);
        font_draw(s, F->head, (float)L->dlg.x + 16.0f, (float)L->dlg.y + 14.0f,
                  "Kill this process?", C_TEXT);
        snprintf(buf, sizeof(buf), "pid %d   %s", m->kill_pid, m->kill_comm);
        font_draw(s, F->mono, (float)L->dlg.x + 16.0f,
                  (float)L->dlg.y + 14.0f + font_height(F->head) + 6.0f,
                  buf, C_TEXT);
        snprintf(buf, sizeof(buf),
                 "SIGTERM now; SIGKILL after %.1f s if it is still alive.",
                 KILL_GRACE_MS / 1000.0);
        fit(buf, F->small, (float)(L->dlg.w - 32));
        font_draw(s, F->small, (float)L->dlg.x + 16.0f,
                  (float)L->dlg.y + 14.0f + font_height(F->head) + 6.0f +
                      font_height(F->small) + 4.0f,
                  buf, C_DIM);
        gfx_rrect_aa(s, (float)L->dlg_ok.x, (float)L->dlg_ok.y,
                     (float)L->dlg_ok.w, (float)L->dlg_ok.h, 4.0f,
                     color_alpha(C_WARN, 0.22f), C_WARN, 1.0f);
        font_draw(s, F->body, (float)L->dlg_ok.x + 12.0f,
                  (float)L->dlg_ok.y + 3.0f, "Send SIGTERM", C_TEXT);
        gfx_rrect_aa(s, (float)L->dlg_no.x, (float)L->dlg_no.y,
                     (float)L->dlg_no.w, (float)L->dlg_no.h, 4.0f,
                     C_PANEL, color_alpha(C_TEXT, 0.20f), 1.0f);
        font_draw(s, F->body, (float)L->dlg_no.x + 12.0f,
                  (float)L->dlg_no.y + 3.0f, "Cancel", C_DIM);
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
    int pid = m->kill_pid;
    m->dlg = 0;
    char buf[160];
    if (pid <= 0) { set_msg(m, "no process selected"); return; }
    if (pid == 1) { set_msg(m, "refusing to kill pid 1 (init)"); return; }
    if (pid == (int)getpid()) { set_msg(m, "refusing to kill this monitor"); return; }
    if (kill(pid, SIGTERM) != 0) {
        snprintf(buf, sizeof(buf), "SIGTERM to %d failed: %s", pid, strerror(errno));
        set_msg(m, buf);
        return;
    }
    m->pending_pid = pid;
    m->pending_at = mono_ms() + KILL_GRACE_MS;
    snprintf(buf, sizeof(buf), "SIGTERM sent to %d (%s)", pid, m->kill_comm);
    set_msg(m, buf);
}

static int check_pending(SysMon *m) {
    if (m->pending_pid <= 0) return 0;
    if (mono_ms() < m->pending_at) return 0;
    int pid = m->pending_pid;
    m->pending_pid = 0;
    char buf[160];
    if (!proc_alive(pid)) {
        snprintf(buf, sizeof(buf), "pid %d exited after SIGTERM", pid);
        set_msg(m, buf);
        return 1;
    }
    if (kill(pid, SIGKILL) == 0)
        snprintf(buf, sizeof(buf), "pid %d ignored SIGTERM: SIGKILL sent", pid);
    else
        snprintf(buf, sizeof(buf), "SIGKILL to %d failed: %s", pid, strerror(errno));
    set_msg(m, buf);
    return 1;
}

static void open_kill_dialog(SysMon *m, const int *view, int view_n) {
    int i = view_index_of(m, view, view_n, m->sel_pid);
    if (i < 0) { set_msg(m, "select a process first"); return; }
    const Proc *p = &m->procs[view[i]];
    if (p->pid == 1) { set_msg(m, "refusing to kill pid 1 (init)"); return; }
    if (p->pid == (int)getpid()) { set_msg(m, "refusing to kill this monitor"); return; }
    m->kill_pid = p->pid;
    snprintf(m->kill_comm, sizeof(m->kill_comm), "%s", p->comm);
    m->dlg = 1;
}

/* ------------------------------------------------------------ main */

#ifndef SYSMON_HOST_TEST

int main(void) {
    MClient c;
    if (mc_open(&c, "sysmon", "System Monitor", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "sysmon: cannot connect to the window server\n");
        return 1;
    }

    font_init("/usr/share/fonts");
    Fonts F;
    F.head  = font_get(FONT_SEMIBOLD, 12);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_REGULAR, 10);
    F.mono  = font_get(FONT_MONO, 10);
    if (!F.head || !F.body || !F.small || !F.mono) {
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
    m.hover = -1;
    m.sel_pid = -1;
    m.msg_at = 0.0;

    static int view[MAX_PROC];
    int view_n = 0;

    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;
    Layout L;
    layout_build(vw, vh, &F, &L);

    double last_ms = mono_ms();
    sample(&m, 0.0);
    view_n = build_view(&m, view);
    if (view_n > 0) m.sel_pid = m.procs[view[0]].pid;

    double next_sample = mono_ms() + 1000.0;
    int dirty = 1;
    int was_down = 0;

    for (;;) {
        double now = mono_ms();

        if (now >= next_sample) {
            sample(&m, now - last_ms);
            last_ms = now;
            view_n = build_view(&m, view);
            next_sample = now + 1000.0;
            if (view_index_of(&m, view, view_n, m.sel_pid) < 0 && view_n > 0)
                m.sel_pid = m.procs[view[0]].pid;
            dirty = 1;
        }
        if (m.pending_pid > 0 && check_pending(&m)) dirty = 1;
        if (m.msg[0] && m.msg_at > 0.0 && now - m.msg_at > 8000.0) {
            m.msg[0] = 0;
            dirty = 1;
        }

        if (dirty) {
            layout_build(vw, vh, &F, &L);
            draw_frame(&m, &(Surface){ .px = c.px, .w = vw, .h = vh, .stride = c.stride },
                       &L, &F, view, view_n);
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
                layout_build(vw, vh, &F, &L);
                dirty = 1;
                break;

            case MC_TEXT: {
                if (m.dlg) {
                    if (strchr(ev.text, 'y') || strchr(ev.text, 'Y')) do_kill(&m);
                    else if (strchr(ev.text, 'n') || strchr(ev.text, 'N')) m.dlg = 0;
                    dirty = 1;
                    break;
                }
                for (const char *p = ev.text; *p; p++) {
                    char ch = *p;
                    if (m.filter_edit) {
                        size_t n = strlen(m.filter);
                        if (n + 1 < sizeof(m.filter) &&
                            (unsigned char)ch >= 32 && ch != 127) {
                            m.filter[n] = ch;
                            m.filter[n + 1] = 0;
                        }
                    } else if (ch == '/') {
                        m.filter_edit = 1;
                    } else if (ch == 'q' || ch == 'Q') {
                        quit = 1;
                    } else if (ch == 'k' || ch == 'K') {
                        open_kill_dialog(&m, view, view_n);
                    } else if ((unsigned char)ch >= 32 && ch != 127) {
                        /* "filter by typing": any other printable key starts
                         * the filter with that character */
                        m.filter_edit = 1;
                        m.filter[0] = ch;
                        m.filter[1] = 0;
                    }
                }
                /* filtering is live: rebuild the view on every keystroke */
                view_n = build_view(&m, view);
                m.scroll = 0;
                if (view_index_of(&m, view, view_n, m.sel_pid) < 0 && view_n > 0)
                    m.sel_pid = m.procs[view[0]].pid;
                dirty = 1;
                break;
            }

            case MC_KEY: {
                if (!ev.press) break;
                if (m.dlg) {
                    if (ev.key == 1) { m.dlg = 0; dirty = 1; }
                    else if (ev.key == 28) { do_kill(&m); dirty = 1; }
                    break;
                }
                if (m.filter_edit) {
                    if (ev.key == 1) {                 /* Esc leaves editing */
                        m.filter_edit = 0;
                        if (!m.filter[0]) view_n = build_view(&m, view);
                        dirty = 1;
                    } else if (ev.key == 28) {         /* Enter also leaves */
                        m.filter_edit = 0;
                        dirty = 1;
                    } else if (ev.key == 14) {         /* Backspace */
                        size_t n = strlen(m.filter);
                        if (n) m.filter[n - 1] = 0;
                        view_n = build_view(&m, view);
                        m.scroll = 0;
                        dirty = 1;
                    }
                    break;
                }
                int sel = view_index_of(&m, view, view_n, m.sel_pid);
                if (sel < 0) sel = 0;
                int moved = 0;
                switch (ev.key) {
                case 1:                                /* Esc clears the filter */
                    if (m.filter[0]) {
                        m.filter[0] = 0;
                        view_n = build_view(&m, view);
                        m.scroll = 0;
                        dirty = 1;
                    }
                    break;
                case 103: if (sel > 0) { sel--; moved = 1; } break;             /* up */
                case 108: if (sel + 1 < view_n) { sel++; moved = 1; } break;    /* down */
                case 104:                                                       /* page up */
                    sel -= L.rows_visible;
                    if (sel < 0) sel = 0;
                    moved = 1;
                    break;
                case 109:                                                       /* page down */
                    sel += L.rows_visible;
                    if (sel > view_n - 1) sel = view_n - 1;
                    if (sel < 0) sel = 0;
                    moved = 1;
                    break;
                case 102: sel = 0; moved = 1; break;                             /* home */
                case 107: sel = view_n - 1; moved = 1; break;                    /* end */
                case 28: open_kill_dialog(&m, view, view_n); dirty = 1; break;
                default: break;
                }
                if (moved && view_n > 0) {
                    if (sel < 0) sel = 0;
                    m.sel_pid = m.procs[view[sel]].pid;
                    if (sel < m.scroll) m.scroll = sel;
                    if (sel >= m.scroll + L.rows_visible)
                        m.scroll = sel - L.rows_visible + 1;
                    if (m.scroll > view_n - L.rows_visible)
                        m.scroll = view_n - L.rows_visible;
                    if (m.scroll < 0) m.scroll = 0;
                    dirty = 1;
                }
                break;
            }

            case MC_WHEEL: {
                if (m.dlg) break;
                m.scroll -= ev.wheel * 3;
                if (m.scroll > view_n - L.rows_visible)
                    m.scroll = view_n - L.rows_visible;
                if (m.scroll < 0) m.scroll = 0;
                dirty = 1;
                break;
            }

            case MC_MOUSE: {
                int x = ev.x, y = ev.y;
                int down = ev.buttons & 1;
                int over_rows = rect_hit(&L.rows, x, y);
                int hov = -1;
                if (over_rows) {
                    int r = (y - L.rows_top) / (L.row_h > 0 ? L.row_h : 1);
                    if (r >= 0 && r < L.rows_visible) hov = m.scroll + r;
                    if (hov >= view_n) hov = -1;
                }
                if (hov != m.hover) { m.hover = hov; dirty = 1; }

                if (down && !was_down) {
                    if (m.dlg) {
                        if (rect_hit(&L.dlg_ok, x, y)) do_kill(&m);
                        else if (rect_hit(&L.dlg_no, x, y)) m.dlg = 0;
                        dirty = 1;
                    } else if (rect_hit(&L.kill_btn, x, y)) {
                        open_kill_dialog(&m, view, view_n);
                        dirty = 1;
                    } else if (rect_hit(&L.filter, x, y)) {
                        m.filter_edit = 1;
                        dirty = 1;
                    } else if (rect_hit(&L.thead, x, y)) {
                        /* click a column heading: same column flips direction */
                        int col = -1;
                        for (int i = 0; i < 5; i++)
                            if (x >= L.col[i] && x < L.col[i + 1]) col = i;
                        if (col >= 0) {
                            if (m.sort_key == col) m.sort_dir = -m.sort_dir;
                            else {
                                m.sort_key = col;
                                m.sort_dir = (col == S_CPU || col == S_MEM) ? -1 : 1;
                            }
                            view_n = build_view(&m, view);
                            m.scroll = 0;
                            dirty = 1;
                        }
                    } else if (over_rows) {
                        int r = (y - L.rows_top) / (L.row_h > 0 ? L.row_h : 1);
                        int vi = m.scroll + r;
                        if (vi >= 0 && vi < view_n) {
                            m.sel_pid = m.procs[view[vi]].pid;
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

/* ============================================================
 *  Host-only self test (never compiled into the OS app).
 *  It builds a /proc fixture out of real files, points the same
 *  parsers at it, and checks the arithmetic and the state machines.
 * ============================================================ */

#ifdef _WIN32
#  include <direct.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  define host_mkdir(p) mkdir((p), 0755)
#endif

static int fails;
static int checks;

static void chk(int cond, const char *what) {
    checks++;
    printf("%-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static void wr(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); fails++; return; }
    fputs(text, f);
    fclose(f);
}

static void stat_file(int pid, const char *comm, char state,
                      unsigned long long utime, unsigned long long stime,
                      long rss_pages, long size_pages) {
    char path[128];
    char buf[640];
    snprintf(buf, sizeof(buf),
             "%d (%s) %c 1 100 100 0 -1 4194304 500 0 0 0 %llu %llu 0 0 20 0 1 0 500 0 250 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
             pid, comm, state, utime, stime);
    snprintf(path, sizeof(path), PROC_ROOT "/%d/stat", pid);
    wr(path, buf);
    snprintf(buf, sizeof(buf), "%ld %ld 100 10 0 50 0\n", size_pages, rss_pages);
    snprintf(path, sizeof(path), PROC_ROOT "/%d/statm", pid);
    wr(path, buf);
}

static const Proc *by_pid(SysMon *m, int pid) { return find_pid(m, pid); }

static void render_bmp(const char *path, const u32 *px, int w, int h) {
    int row = (w * 3 + 3) & ~3;
    size_t size = 54u + (size_t)row * (size_t)h;
    u8 *b = calloc(1, size);
    if (!b) return;
    b[0] = 'B'; b[1] = 'M';
    b[2] = (u8)size; b[3] = (u8)(size >> 8); b[4] = (u8)(size >> 16); b[5] = (u8)(size >> 24);
    b[10] = 54; b[14] = 40;
    b[18] = (u8)w; b[19] = (u8)(w >> 8); b[20] = (u8)(w >> 16); b[21] = (u8)(w >> 24);
    b[22] = (u8)h; b[23] = (u8)(h >> 8); b[24] = (u8)(h >> 16); b[25] = (u8)(h >> 24);
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < h; y++) {
        u8 *dst = b + 54u + (size_t)(h - 1 - y) * (size_t)row;
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * (size_t)w + (size_t)x];
            dst[x * 3 + 0] = GET_B(c);
            dst[x * 3 + 1] = GET_G(c);
            dst[x * 3 + 2] = GET_R(c);
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(b, 1, size, f); fclose(f); }
    free(b);
}

int sysmon_selftest(void);
int sysmon_render(const char *normal, const char *dialog);

int sysmon_selftest(void) {
    host_mkdir(".work/paintmon/fake");
    host_mkdir(".work/paintmon/fake/proc");
    host_mkdir(".work/paintmon/fake/proc/net");
    host_mkdir(PROC_ROOT "/1");
    host_mkdir(PROC_ROOT "/100");
    host_mkdir(PROC_ROOT "/200");
    host_mkdir(PROC_ROOT "/300");
    host_mkdir(PROC_ROOT "/400");

    wr(PROC_ROOT "/loadavg", "0.42 0.31 0.20 1/57 12345\n");
    wr(PROC_ROOT "/meminfo",
       "MemTotal:        8000000 kB\n"
       "MemFree:          500000 kB\n"
       "MemAvailable:    3000000 kB\n");
    wr(PROC_ROOT "/uptime", "123456.78 999.00\n");
    wr(PROC_ROOT "/cpuinfo",
       "processor\t: 0\nmodel name\t: fake\n"
       "processor\t: 1\nmodel name\t: fake\n"
       "processor\t: 2\nmodel name\t: fake\n"
       "processor\t: 3\nmodel name\t: fake\n");
    wr(PROC_ROOT "/net/dev",
       "Inter-|   Receive                                                |  Transmit\n"
       " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
       "    lo: 1000 1 0 0 0 0 0 0 2000 1 0 0 0 0 0 0\n"
       "  eth0: 5000000 10 0 0 0 0 0 0 7000000 20 0 0 0 0 0 0\n");

    stat_file(1, "init", 'S', 10, 5, 100, 1000);
    stat_file(100, "bash", 'S', 100, 20, 250, 1000);
    stat_file(200, "sys monitor", 'R', 5, 0, 500, 2000);
    stat_file(300, "odd)name", 'S', 1000, 0, 4000, 8000);
    stat_file(400, "defunct", 'Z', 0, 0, 0, 0);

    SysMon m;
    memset(&m, 0, sizeof(m));
    m.hz = 100;
    m.page_kb = 4;
    m.sort_key = S_CPU;
    m.sort_dir = -1;
    m.sel_pid = -1;

    sample(&m, 0.0);
    printf("sample 1: %d procs, %d cpus, load %.2f %.2f %.2f, net %d ifaces\n",
           m.nproc, m.ncpu, m.load[0], m.load[1], m.load[2], m.nif);
    chk(m.nproc == 5, "stat/sample: five pids read");
    chk(m.ncpu == 4, "cpuinfo: four processor lines");
    chk(m.load[0] > 0.419 && m.load[0] < 0.421, "loadavg: 1 min read");
    chk(m.load[2] > 0.199 && m.load[2] < 0.201, "loadavg: 15 min read");
    chk(m.total_kb == 8000000, "meminfo: MemTotal read");
    chk(m.avail_kb == 3000000, "meminfo: MemAvailable read");
    chk(m.uptime > 123456.7 && m.uptime < 123456.8, "uptime: read");
    chk(m.nif == 2, "net/dev: two interfaces");
    chk(!strcmp(m.ifs[1].name, "eth0") && m.ifs[1].rx == 5000000 && m.ifs[1].tx == 7000000,
        "net/dev: eth0 counters");
    {
        const Proc *p = by_pid(&m, 200);
        chk(p && !strcmp(p->comm, "sys monitor"), "stat: comm with a space");
        p = by_pid(&m, 300);
        chk(p && !strcmp(p->comm, "odd)name"), "stat: comm containing ')'");
        p = by_pid(&m, 400);
        chk(p && p->state == 'Z', "stat: zombie state letter");
        p = by_pid(&m, 100);
        chk(p && p->mem_kb == 1000, "statm: 250 pages x 4 kB = 1000 kB");
        chk(p && p->cpu10 == -1, "cpu: first sample has no delta");
    }
    chk(m.hist_len == 1, "ring: one sample after the first pass");

    /* ---- second sample, one second later, with new jiffies ---- */
    stat_file(100, "bash", 'S', 150, 20, 250, 1000);       /* +50 jiffies = 0.50 s */
    stat_file(300, "odd)name", 'S', 1100, 0, 4000, 8000);  /* +100 jiffies = 1.00 s */
    stat_file(200, "sys monitor", 'R', 5, 0, 500, 2000);   /* unchanged */
    wr(PROC_ROOT "/net/dev",
       "Inter-|   Receive                                                |  Transmit\n"
       " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
       "    lo: 1000 1 0 0 0 0 0 0 2000 1 0 0 0 0 0 0\n"
       "  eth0: 5002000 10 0 0 0 0 0 0 7010000 20 0 0 0 0 0 0\n");
    sample(&m, 1000.0);

    {
        const Proc *p = by_pid(&m, 100);
        printf("cpu: pid 100 +50 jiffies over 1.000 s at 100 Hz -> %d tenths of a percent\n",
               p ? p->cpu10 : -999);
        chk(p && p->cpu10 == 500, "cpu% = jiffies x 1000 / (hz x dt)  => 50.0%");
        p = by_pid(&m, 300);
        chk(p && p->cpu10 == 1000, "cpu% = 100.0% for one full second of cpu");
        p = by_pid(&m, 200);
        chk(p && p->cpu10 == 0, "cpu% = 0.0% for an unchanged process");
        chk(m.dt_ms == 1000.0, "sample: dt is the measured 1000 ms");
    }
    chk(m.hist_len == 2, "ring: two samples");
    chk(m.ifs[1].rx_rate > 1999.0 && m.ifs[1].rx_rate < 2001.0,
        "net rate: +2000 bytes in 1 s = 2000 B/s");
    chk(m.ifs[1].tx_rate > 9999.0 && m.ifs[1].tx_rate < 10001.0,
        "net rate: +10000 bytes in 1 s = 10000 B/s");

    static int view[MAX_PROC];
    int n = build_view(&m, view);
    chk(n == 5, "view: five rows");
    chk(m.procs[view[0]].pid == 300, "sort: cpu descending puts pid 300 first");
    m.sort_key = S_MEM;
    m.sort_dir = -1;
    n = build_view(&m, view);
    chk(m.procs[view[0]].pid == 300, "sort: memory descending puts the 16000 kB pid first");
    chk(m.procs[view[4]].pid == 400, "sort: the 0 kB zombie sorts last");
    m.sort_key = S_NAME;
    m.sort_dir = 1;
    n = build_view(&m, view);
    chk(!strcmp(m.procs[view[0]].comm, "bash"), "sort: name ascending starts at bash");
    m.sort_key = S_PID;
    m.sort_dir = 1;
    n = build_view(&m, view);
    chk(m.procs[view[0]].pid == 1, "sort: pid ascending starts at 1");

    snprintf(m.filter, sizeof(m.filter), "odd");
    n = build_view(&m, view);
    chk(n == 1 && m.procs[view[0]].pid == 300, "filter: 'odd' matches only odd)name");
    snprintf(m.filter, sizeof(m.filter), "20");
    n = build_view(&m, view);
    chk(n == 1 && m.procs[view[0]].pid == 200, "filter: '20' matches pid 200");
    snprintf(m.filter, sizeof(m.filter), "BASH");
    n = build_view(&m, view);
    chk(n == 1 && m.procs[view[0]].pid == 100, "filter: case-insensitive");
    snprintf(m.filter, sizeof(m.filter), "zzz");
    n = build_view(&m, view);
    chk(n == 0, "filter: no match gives no rows");
    m.filter[0] = 0;
    n = build_view(&m, view);

    /* ---- kill: refusal and the two signals ---- */
    g_kill_n = 0;
    m.kill_pid = 1;
    snprintf(m.kill_comm, sizeof(m.kill_comm), "init");
    do_kill(&m);
    printf("kill pid 1 -> %s\n", m.msg);
    chk(strstr(m.msg, "refusing") != NULL && g_kill_n == 0, "kill: pid 1 refused, no signal sent");
    m.kill_pid = (int)getpid();
    do_kill(&m);
    chk(strstr(m.msg, "refusing") != NULL && g_kill_n == 0, "kill: own pid refused");

    m.kill_pid = 300;
    snprintf(m.kill_comm, sizeof(m.kill_comm), "odd)name");
    do_kill(&m);
    printf("kill pid 300 -> %s\n", m.msg);
    chk(g_kill_n == 1 && g_kill_pid[0] == 300 && g_kill_sig[0] == SIGTERM,
        "kill: SIGTERM is the first signal");
    chk(m.pending_pid == 300, "kill: escalation is armed");
    check_pending(&m);
    chk(g_kill_n == 1, "kill: no SIGKILL before the grace period");
    host_advance(KILL_GRACE_MS + 100.0);
    check_pending(&m);
    printf("after %.1f s -> %s\n", KILL_GRACE_MS / 1000.0, m.msg);
    chk(g_kill_n == 2 && g_kill_pid[1] == 300 && g_kill_sig[1] == SIGKILL,
        "kill: SIGKILL after the grace period");
    chk(m.pending_pid == 0, "kill: pending cleared");

    m.kill_pid = 999999;
    snprintf(m.kill_comm, sizeof(m.kill_comm), "ghost");
    do_kill(&m);
    printf("kill 999999 -> %s\n", m.msg);
    chk(strstr(m.msg, "failed") != NULL && m.pending_pid == 0,
        "kill: a failed kill(2) is reported with strerror and not armed");

    printf("checks %d, failures %d\n", checks, fails);
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

int sysmon_render(const char *normal, const char *dialog) {
    ui_scale_set_effective(1.4f);
    if (font_init("assets/fonts") != 0) { printf("render: font_init failed\n"); return 2; }
    Fonts F;
    F.head  = font_get(FONT_SEMIBOLD, 12);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_REGULAR, 10);
    F.mono  = font_get(FONT_MONO, 10);
    if (!F.head || !F.body || !F.small || !F.mono) { printf("render: fonts\n"); return 2; }

    static SysMon m;
    memset(&m, 0, sizeof(m));
    m.hz = 100;
    m.page_kb = 4;
    m.sort_key = S_CPU;
    m.sort_dir = -1;
    m.hover = -1;
    m.sel_pid = -1;
    sample(&m, 0.0);

    /* a few more ring points so the graphs have a shape, every one of
     * them read back from the fixture files */
    for (int i = 0; i < 40; i++) {
        char la[64];
        double v = 0.30 + 0.02 * (double)((i * 7) % 11);
        snprintf(la, sizeof(la), "%.2f 0.31 0.20 1/57 12345\n", v);
        wr(PROC_ROOT "/loadavg", la);
        stat_file(100, "bash", 'S', (unsigned long long)(100 + i * 3), 20, 250, 1000);
        stat_file(300, "odd)name", 'S', (unsigned long long)(1000 + i * 9), 0, 4000, 8000);
        sample(&m, 1000.0);
    }
    m.sort_key = S_CPU;
    m.sort_dir = -1;
    static int view[MAX_PROC];
    int n = build_view(&m, view);
    if (n > 0) m.sel_pid = m.procs[view[0]].pid;
    m.filter[0] = 0;
    m.msg[0] = 0;

    int vw = WIN_W, vh = WIN_H;
    u32 *px = calloc((size_t)vw * (size_t)vh, sizeof(u32));
    if (!px) return 2;
    Layout L;
    layout_build(vw, vh, &F, &L);
    Surface s = { .px = px, .w = vw, .h = vh, .stride = vw };

    draw_frame(&m, &s, &L, &F, view, n);
    render_bmp(normal, px, vw, vh);
    printf("render          %s\n", normal);

    m.dlg = 1;
    m.kill_pid = 300;
    snprintf(m.kill_comm, sizeof(m.kill_comm), "odd)name");
    m.sel_pid = 300;
    draw_frame(&m, &s, &L, &F, view, n);
    render_bmp(dialog, px, vw, vh);
    printf("render          %s\n", dialog);

    free(px);
    return 0;
}

#endif /* SYSMON_HOST_TEST */
