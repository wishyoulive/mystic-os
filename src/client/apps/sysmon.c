/* ============================================================
 *  Mystic OS - apps (external) / sysmon
 *
 *  A live system dashboard that runs as its own process. It
 *  connects to the window server, is given a 420x300 shared pixel
 *  buffer, and repaints it once a second. Nothing here is compiled
 *  into the desktop, and every number on screen was read out of a
 *  kernel-generated file a moment earlier:
 *
 *    /proc/cpuinfo        "processor" lines        -> CPU count
 *    /proc/loadavg        fields 1, 2, 3          -> load average
 *    /proc/meminfo        MemTotal, MemAvailable  -> memory bar + text
 *    /proc/uptime         first field             -> Nd HH:MM:SS
 *    /proc/<pid>/statm    field 2 (resident pages)-> top 3 by RSS
 *    /proc/<pid>/comm                            -> their names
 *
 *  The load history is a fixed 60-slot ring inside this process, one
 *  sample per repaint, so the sparkline covers the last 60 seconds.
 *  Nothing is allocated while running: the ring, the top-three table
 *  and every text buffer are fixed size.
 *
 *  Keys: q quits. Closing the window quits too.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W    420
#define WIN_H    300

#define SPARK_N   60      /* sparkline samples, one per second */
#define TOP_N      3      /* processes listed by resident memory */

/* left/right margin inside the client area */
#define MX 9

/* vertical layout of the dashboard, in client coordinates */
#define L_HEAD     8      /* "SYSTEM MONITOR" */
#define L_RULE1   25
#define L_LOAD_LB 30      /* "LOAD AVERAGE" + cpu / proc counts */
#define L_LOAD_BG 43      /* the big 1-minute number */
#define L_PLOT0   80      /* sparkline top */
#define L_PLOT1  120      /* sparkline baseline */
#define L_MEM_LB 128      /* "MEMORY" + "X MB used of Y MB" */
#define L_MEM_BR 141      /* memory bar */
#define L_MEM_BH   8
#define L_UP_LB  156      /* "UPTIME" + the formatted value */
#define L_RULE2  172
#define L_TOP_HD 179      /* "TOP MEMORY" */
#define L_TOP_COL 196     /* PID NAME RSS column headings */
#define L_ROW0   210      /* first process row */
#define ROW_H     16

/* palette: the same cold dashboard blue the clock app uses, so the
 * two external apps look like one system */
static const u32 C_BG_TOP = RGB(0x0D, 0x14, 0x22);
static const u32 C_BG_BOT = RGB(0x18, 0x24, 0x3A);
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_MEM    = RGB(0x7A, 0xA2, 0xF7);
static const u32 C_WARN   = RGB(0xE0, 0x6C, 0x5C);

typedef struct {
    int  pid;
    char comm[28];
    long kb;                 /* resident set, kB */
} TopProc;

typedef struct {
    /* one snapshot, all of it freshly read from /proc */
    int    ncpu;
    int    nproc;
    double load[3];
    long   total_kb;
    long   avail_kb;
    double uptime;
    TopProc top[TOP_N];
    int    top_n;

    /* the load-average ring: hist[hist_head] is the next slot to write */
    float  hist[SPARK_N];
    int    hist_head;
    int    hist_len;
    int    samples;

    long   page_kb;          /* kB per resident page, from sysconf */
    int    w, h;             /* client area we are drawing into */
} SysMon;

typedef struct {
    Font *head;              /* section headings */
    Font *big;               /* the 1-minute load number */
    Font *body;              /* values */
    Font *small;             /* labels and the legend */
} Fonts;

/* ------------------------------------------------------------
 *  /proc readers
 * ------------------------------------------------------------ */

/* CPU count: one "processor" line per logical CPU. The number drawn is
 * literally how many such lines we read. */
static int read_ncpu(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "processor", 9)) n++;
    fclose(f);
    return n;
}

/* /proc/loadavg is "0.42 0.31 0.20 1/57 12345" */
static void read_loadavg(SysMon *m) {
    m->load[0] = m->load[1] = m->load[2] = 0.0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double a = 0.0, b = 0.0, c = 0.0;
    if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) {
        m->load[0] = a;
        m->load[1] = b;
        m->load[2] = c;
    }
    fclose(f);
}

/* MemTotal and MemAvailable are in kB. used = total - available, which
 * is what the kernel itself reports to userspace as used memory. */
static void read_meminfo(SysMon *m) {
    m->total_kb = 0;
    m->avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))
            m->total_kb = strtol(line + 9, NULL, 10);
        else if (!strncmp(line, "MemAvailable:", 13))
            m->avail_kb = strtol(line + 13, NULL, 10);
        if (m->total_kb > 0 && m->avail_kb > 0) break;
    }
    fclose(f);
}

static double read_uptime(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return 0.0;
    double up = 0.0;
    if (fscanf(f, "%lf", &up) != 1) up = 0.0;
    fclose(f);
    return up > 0.0 ? up : 0.0;
}

/* drop a pid from the table, compacting the tail */
static void top_remove(SysMon *m, int pid) {
    for (int i = 0; i < m->top_n; i++) {
        if (m->top[i].pid != pid) continue;
        for (int k = i; k + 1 < m->top_n; k++) m->top[k] = m->top[k + 1];
        m->top_n--;
        return;
    }
}

/* Offer one process to the descending-by-RSS table; pid ascending breaks
 * ties so the list does not shuffle between samples. The pid is removed
 * first, so offering the same pid twice can never produce two rows. */
static void top_offer(SysMon *m, int pid, const char *comm, long kb) {
    top_remove(m, pid);

    int at = m->top_n;
    for (int i = 0; i < m->top_n; i++) {
        if (kb > m->top[i].kb || (kb == m->top[i].kb && pid < m->top[i].pid)) {
            at = i;
            break;
        }
    }
    if (at >= TOP_N) return;                 /* the table is full and we lose */
    if (m->top_n < TOP_N) m->top_n++;
    for (int i = m->top_n - 1; i > at; i--) m->top[i] = m->top[i - 1];
    m->top[at].pid = pid;
    m->top[at].kb = kb;
    snprintf(m->top[at].comm, sizeof(m->top[at].comm), "%s", comm);
}

/* Walk /proc once. Every numeric entry is a live pid; statm field 2 is
 * the resident page count, so it is the number the kernel has for that
 * process right now, not a cached figure. A process that exits between
 * readdir and open simply disappears from the sample. */
static void scan_procs(SysMon *m) {
    m->nproc = 0;
    m->top_n = 0;
    DIR *d = opendir("/proc");
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;

        char path[64];
        long size_pages = 0, rss_pages = -1;
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;                    /* it died mid-scan */
        if (fscanf(f, "%ld %ld", &size_pages, &rss_pages) != 2) rss_pages = -1;
        fclose(f);
        if (rss_pages < 0) continue;

        char comm[28];
        snprintf(comm, sizeof(comm), "?");
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        f = fopen(path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f)) {
                size_t n = strlen(comm);
                while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r'))
                    comm[--n] = 0;
            }
            fclose(f);
        }

        m->nproc++;
        top_offer(m, pid, comm, rss_pages * m->page_kb);
    }
    closedir(d);
}

/* one full sample: everything the frame shows, plus one sparkline point */
static void collect(SysMon *m) {
    m->ncpu = read_ncpu();
    read_loadavg(m);
    read_meminfo(m);
    m->uptime = read_uptime();
    scan_procs(m);

    m->hist[m->hist_head] = (float)m->load[0];
    m->hist_head = (m->hist_head + 1) % SPARK_N;
    if (m->hist_len < SPARK_N) m->hist_len++;
    m->samples++;
}

/* ------------------------------------------------------------
 *  Text helpers
 * ------------------------------------------------------------ */
static void text_right(Surface *s, Font *f, int xr, float y, const char *t, u32 col) {
    font_draw(s, f, (float)xr - font_width(f, t), y, t, col);
}

/* cut bytes off the end until the string fits, so a long comm never
 * runs into the numbers beside it */
static void fit(char *t, Font *f, float maxw) {
    size_t n = strlen(t);
    while (n > 1 && font_width(f, t) > maxw) t[--n] = 0;
}

static void fmt_uptime(double secs, char *out, size_t n) {
    unsigned long s  = (unsigned long)(secs > 0.0 ? secs : 0.0);
    unsigned long d  = s / 86400UL;
    unsigned long hh = (s % 86400UL) / 3600UL;
    unsigned long mm = (s % 3600UL) / 60UL;
    unsigned long ss = s % 60UL;
    if (d)
        snprintf(out, n, "%lud %02lu:%02lu:%02lu", d, hh, mm, ss);
    else
        snprintf(out, n, "%02lu:%02lu:%02lu", hh, mm, ss);
}

/* ------------------------------------------------------------
 *  Drawing
 * ------------------------------------------------------------ */
static void draw_spark(SysMon *m, Surface *s, const Fonts *f, int w) {
    int px0 = MX, px1 = w - MX;
    int py0 = L_PLOT0, py1 = L_PLOT1;
    if (px1 - px0 < 20 || py1 - py0 < 8) return;

    float pw = (float)(px1 - px0);
    float ph = (float)(py1 - py0);
    float step = pw / (float)(SPARK_N - 1);

    /* scale to the largest sample we are showing, but never below 1.0
     * so an idle machine does not look like it is thrashing */
    float vmax = 1.0f;
    for (int i = 0; i < m->hist_len; i++) {
        int idx = (m->hist_head - m->hist_len + i + SPARK_N) % SPARK_N;
        if (m->hist[idx] > vmax) vmax = m->hist[idx];
    }

    char scale[32];
    snprintf(scale, sizeof(scale), "scale %.2f", (double)vmax);
    text_right(s, f->small, px1, (float)py0 - 16.0f, scale, color_alpha(C_TEXT, 0.45f));

    /* the floor of the plot, and a hairline at the scale maximum */
    surf_blend(s, px0, py1, px1 - px0, 1, color_alpha(C_TEXT, 0.16f));
    surf_blend(s, px0, py0, px1 - px0, 1, color_alpha(C_TEXT, 0.06f));

    float lastx = (float)px0, lasty = (float)py1;
    float prevx = 0.0f, prevy = 0.0f;
    int have_prev = 0;

    for (int i = 0; i < m->hist_len; i++) {
        int idx = (m->hist_head - m->hist_len + i + SPARK_N) % SPARK_N;
        float v = m->hist[idx];
        if (v < 0.0f) v = 0.0f;
        float x = (float)px0 + (float)i * step;
        float y = (float)py1 - (v / vmax) * ph;
        if (y < (float)py0) y = (float)py0;
        if (y > (float)py1) y = (float)py1;
        if (have_prev) gfx_line(s, prevx, prevy, x, y, 1.5f, C_ACCENT);
        prevx = x;
        prevy = y;
        lastx = x;
        lasty = y;
        have_prev = 1;
    }

    /* the newest sample gets a dot, so a single-point history still
     * shows something real instead of an empty box */
    if (m->hist_len > 0)
        gfx_rrect_aa(s, lastx - 2.0f, lasty - 2.0f, 4.0f, 4.0f, 2.0f, C_ACCENT, 0, 0);
}

static void draw_frame(SysMon *m, Surface *s, const Fonts *f) {
    int w = m->w, h = m->h;
    if (w < 120 || h < 150) return;          /* too small to lay out */

    gfx_gradient(s, 0, 0, w, h, C_BG_TOP, C_BG_BOT);

    const u32 rule = color_alpha(C_TEXT, 0.10f);
    const u32 label = color_alpha(C_TEXT, 0.50f);

    /* ---- header ---- */
    font_draw(s, f->head, MX, L_HEAD, "SYSTEM MONITOR", C_TEXT);
    char buf[96];
    snprintf(buf, sizeof(buf), "pid %d", (int)getpid());
    if (font_width(f->head, "SYSTEM MONITOR") + font_width(f->small, buf) + 24.0f < (float)w)
        text_right(s, f->small, w - MX, L_HEAD + 3, buf, C_DIM);
    surf_blend(s, MX, L_RULE1, w - 2 * MX, 1, rule);

    /* ---- load average ---- */
    font_draw(s, f->small, MX, L_LOAD_LB, "LOAD AVERAGE", label);
    /* the CPU count is the number of "processor" lines we actually
     * counted in /proc/cpuinfo, and nproc the pids we could sample */
    snprintf(buf, sizeof(buf), "%d cpu    %d proc", m->ncpu, m->nproc);
    text_right(s, f->small, w - MX, L_LOAD_LB, buf, label);

    char big[16];
    snprintf(big, sizeof(big), "%.2f", m->load[0]);
    font_draw(s, f->big, MX, L_LOAD_BG, big, C_ACCENT);
    float bw = font_width(f->big, big);
    font_draw(s, f->small, (float)MX + bw + 8.0f, (float)L_LOAD_BG + 9.0f,
              "1 min", C_DIM);

    snprintf(buf, sizeof(buf), "5m %.2f    15m %.2f", m->load[1], m->load[2]);
    text_right(s, f->small, w - MX, (float)L_LOAD_BG + 4.0f, buf, C_DIM);

    draw_spark(m, s, f, w);

    /* ---- memory ---- */
    long used_kb = m->total_kb - m->avail_kb;
    if (used_kb < 0) used_kb = 0;
    float frac = m->total_kb > 0 ? (float)used_kb / (float)m->total_kb : 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    font_draw(s, f->small, MX, L_MEM_LB, "MEMORY", label);
    if (m->total_kb > 0) {
        snprintf(buf, sizeof(buf), "%ld MB used of %ld MB   %d%%",
                 used_kb / 1024, m->total_kb / 1024,
                 (int)(frac * 100.0f + 0.5f));
        text_right(s, f->body, w - MX, L_MEM_LB - 1, buf, C_TEXT);
    }

    gfx_rrect_aa(s, (float)MX, (float)L_MEM_BR, (float)(w - 2 * MX),
                 (float)L_MEM_BH, (float)L_MEM_BH * 0.5f,
                 color_alpha(C_TEXT, 0.10f), 0, 0);
    float fill = (float)(w - 2 * MX) * frac;
    if (fill > 3.0f)
        gfx_rrect_aa(s, (float)MX, (float)L_MEM_BR, fill, (float)L_MEM_BH,
                     (float)L_MEM_BH * 0.5f,
                     frac > 0.85f ? C_WARN : C_MEM, 0, 0);

    /* ---- uptime ---- */
    char up[32];
    fmt_uptime(m->uptime, up, sizeof(up));
    font_draw(s, f->small, MX, L_UP_LB, "UPTIME", label);
    text_right(s, f->body, w - MX, L_UP_LB - 1, up, C_TEXT);

    /* ---- top three by resident set ---- */
    surf_blend(s, MX, L_RULE2, w - 2 * MX, 1, rule);
    font_draw(s, f->head, MX, L_TOP_HD, "TOP MEMORY", C_TEXT);
    snprintf(buf, sizeof(buf), "resident set, %d procs", m->nproc);
    if (font_width(f->head, "TOP MEMORY") + font_width(f->small, buf) + 24.0f < (float)w)
        text_right(s, f->small, w - MX, L_TOP_HD + 3, buf, label);

    float name_x = (float)MX + 46.0f;
    float mem_r  = (float)(w - MX);
    font_draw(s, f->small, MX, L_TOP_COL, "PID", color_alpha(C_TEXT, 0.40f));
    font_draw(s, f->small, name_x, L_TOP_COL, "NAME", color_alpha(C_TEXT, 0.40f));
    text_right(s, f->small, (int)mem_r, L_TOP_COL, "RESIDENT",
               color_alpha(C_TEXT, 0.40f));

    long top_kb = m->top_n > 0 && m->top[0].kb > 0 ? m->top[0].kb : 1;
    float name_max = (mem_r - 62.0f) - name_x;
    if (name_max < 30.0f) name_max = 30.0f;

    for (int i = 0; i < m->top_n; i++) {
        float ry = (float)L_ROW0 + (float)i * (float)ROW_H;
        if (ry + (float)ROW_H > (float)h - 30.0f) break;

        /* a share bar behind the row: this process against the largest,
         * stopping short of the MB column so the number stays on flat
         * background */
        float share = (float)((double)m->top[i].kb / (double)top_kb);
        if (share > 1.0f) share = 1.0f;
        float bar_x0 = (float)MX + 42.0f;
        float barw = (mem_r - 62.0f - bar_x0) * share;
        if (barw > 2.0f)
            gfx_rrect_aa(s, bar_x0, ry - 1.0f, barw, (float)ROW_H - 2.0f,
                         3.0f, color_alpha(C_ACCENT, 0.13f), 0, 0);

        snprintf(buf, sizeof(buf), "%d", m->top[i].pid);
        font_draw(s, f->body, MX, ry, buf, C_DIM);

        char nm[28];
        snprintf(nm, sizeof(nm), "%s", m->top[i].comm);
        fit(nm, f->body, name_max);
        font_draw(s, f->body, name_x, ry, nm, C_TEXT);

        snprintf(buf, sizeof(buf), "%.1f MB", (double)m->top[i].kb / 1024.0);
        text_right(s, f->body, (int)mem_r, ry, buf, C_TEXT);
    }

    /* ---- legend ---- */
    float fy = (float)h - 22.0f;
    if (fy > (float)L_ROW0 + (float)TOP_N * (float)ROW_H) {
        surf_blend(s, MX, (int)fy - 7, w - 2 * MX, 1, rule);
        font_draw(s, f->small, MX, fy, "q quits", C_DIM);
        snprintf(buf, sizeof(buf), "1 s sample    %d taken", m->samples);
        text_right(s, f->small, w - MX, fy, buf, color_alpha(C_TEXT, 0.40f));
    }
}

/* ------------------------------------------------------------
 *  Main loop
 * ------------------------------------------------------------ */
int main(void) {
    MClient c;
    if (mc_open(&c, "sysmon", "System Monitor", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "sysmon: cannot connect to the window server\n");
        return 1;
    }

    font_init("/usr/share/fonts");
    Fonts f;
    f.head  = font_get(FONT_MEDIUM, 12);
    f.big   = font_get(FONT_SEMIBOLD, 22);
    f.body  = font_get(FONT_REGULAR, 11);
    f.small = font_get(FONT_REGULAR, 10);
    if (!f.head || !f.big || !f.body || !f.small) {
        fprintf(stderr, "sysmon: no fonts\n");
        mc_close(&c);
        return 1;
    }

    SysMon m;
    memset(&m, 0, sizeof(m));
    m.page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (m.page_kb <= 0) m.page_kb = 4;
    m.w = c.w;
    m.h = c.h;

    /* we draw straight into the shared buffer we were handed; the
     * Surface clips every primitive to the window for us */
    Surface s = { c.px, c.w, c.h, c.stride };

    for (;;) {
        collect(&m);
        draw_frame(&m, &s, &f);
        mc_present(&c);

        /* sit out the rest of the second, but react to input at once */
        MEvent ev;
        int resized = 0;
        for (int i = 0; i < 20 && !resized; i++) {
            while (mc_poll(&c, &ev)) {
                if (ev.type == MC_CLOSE) { mc_close(&c); return 0; }
                if (ev.type == MC_KEY && ev.press && ev.key == 16) {  /* q */
                    mc_close(&c);
                    return 0;
                }
                if (ev.type == MC_TEXT && strchr(ev.text, 'q')) {
                    mc_close(&c);
                    return 0;
                }
                if (ev.type == MC_RESIZE) {
                    /* the mapping we were given cannot grow, so only a
                     * shrink is honoured; anything larger is ignored */
                    int nw = ev.w > 0 && ev.w <= s.w ? ev.w : m.w;
                    int nh = ev.h > 0 && ev.h <= s.h ? ev.h : m.h;
                    if (nw != m.w || nh != m.h) {
                        m.w = nw;
                        m.h = nh;
                        resized = 1;   /* repaint now, not a second late */
                    }
                }
            }
            if (!resized) usleep(50 * 1000);
        }
    }
}
