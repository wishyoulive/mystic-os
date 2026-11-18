/* ============================================================
 *  Mystic OS - apps/monitor.c
 *
 *  The live process table. Its role is narrower than the other two
 *  system windows on purpose: System (apps/sysinfo.c) is the static
 *  facts panel and System Monitor (client/apps/sysmon.c) is the
 *  separate-process dashboard. This one is the table, as large as the
 *  window allows, and it is built from the same card, the same columns
 *  and the same keys as the table in sysmon.c.
 *
 *  Everything is read from /proc:
 *
 *    /proc/<pid>/stat     field 2 (comm), field 3 (state),
 *                         fields 14/15 (utime, stime)
 *    /proc/<pid>/statm    resident set size
 *
 *  The table is resampled at most four times a second (MON_TTL_S):
 *  the whole /proc scan runs only when the last real sample is older
 *  than 250 ms. Four hertz is live for a process table (a human cannot
 *  read a row changing faster, and a 250 ms window is 25 jiffies at
 *  HZ=100, enough to measure CPU% honestly), while a scan is one
 *  opendir plus two file reads per pid - at 120 pids that cost belongs
 *  to the sample, not to every frame the compositor asks for. Between
 *  samples the rows are the values from that last real read, never a
 *  guess: a number on screen is at most 250 ms old. A sort, a filter
 *  change or a kill marks the sample stale so the next frame rescans;
 *  a resize does not, because the window size cannot change /proc and
 *  the desktop calls resize() on every mouse move while a window edge
 *  is being dragged - invalidating there would put the scan back on a
 *  per-frame path.
 *
 *  CPU% is a DELTA and the card says so on its title line: the jiffies
 *  a process burned between two samples divided by the wall time
 *  between those samples, measured with CLOCK_MONOTONIC. It is a
 *  percentage of one core, so a threaded process can legitimately read
 *  above 100%. A process that appears after a sample has no previous
 *  ticks yet and reads "-" (unknown), never a 0.0% that looks like a
 *  measurement; the displayed sample interval is the real
 *  measured gap, not the requested one.
 *
 *  Columns, left to right: PID, NAME, STATE, CPU%, MEM. Every number is
 *  right-aligned on its own column edge, and each column is as wide as
 *  the widest string it has to hold, measured from the real font. At a
 *  window too narrow for all five, STATE then CPU are dropped (and the
 *  heading with them) instead of letting two columns overlap.
 *
 *  Keys and mouse, identical to sysmon.c:
 *
 *      1..5 or a click on a heading    sort by that column
 *      s                               next column
 *      S                               reverse the order
 *      f or /                          edit the filter (pid or name)
 *      c                               clear the filter
 *      Enter                           leave the filter box
 *      j k / arrows / PageUp PageDown  move the selection
 *      K                               SIGTERM, then SIGKILL after 2 s
 *      wheel                           scroll three rows
 *
 *  Kill refuses pid 1 and this process, reports kill(2) errors with
 *  strerror, and after the grace period sends SIGKILL only if the pid
 *  is still alive. The message replaces the "N of M" counter for five
 *  seconds; nothing here is a second copy of a number.
 *
 *  The card language (frame, title hairline, label/value rows, margins)
 *  is copied from src/apps/sysinfo.c; sysmon.c carries the same block.
 *  See the CARDS comment there for why the three files share the text
 *  instead of a header.
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

#define MAX_PROC     256
#define MON_TTL_S    0.25     /* process-table sample lifetime, 4 Hz    */
#define MON_MIN_DT_S 0.05     /* a forced rescan sooner than this keeps
                               * the previous CPU%, so a jiffy-quantised
                               * 0% or 400% is never published           */
#define KILL_GRACE_S 2.0      /* SIGTERM -> SIGKILL */

#ifndef MON_PROC
#define MON_PROC "/proc"
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif

/* ------------------------------------------------------------
 *  CARDS - one visual language for sysinfo, monitor and sysmon.
 *  Frame, title, row and margin code identical in apps/sysinfo.c.
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

static CrdStyle crd_style(const Theme *th) {
    CrdStyle st;
    st.panel  = th->panel;
    st.border = th->panel_border;
    st.text   = th->text;
    st.dim    = th->text_dim;
    st.accent = th->accent;
    st.warn   = RGB(0xE0, 0x6C, 0x5C);
    st.corner = th->corner * 0.55f;
    if (st.corner < 3.0f) st.corner = 3.0f;
    return st;
}

/* ------------------------------------------------------------
 *  Process rows - the same struct, columns, sort and filter as sysmon.
 * ------------------------------------------------------------ */
typedef struct {
    int    pid;
    char   comm[32];
    char   state;      /* /proc/<pid>/stat field 3        */
    long   mem_kb;     /* statm field 2 x the real page   */
    double cpu;        /* % of one core, delta since the sample */
} Proc;

enum { S_PID, S_NAME, S_STATE, S_CPU, S_MEM, S_N };

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

/* Lay out the table inside a card. Every column edge is measured from the
 * widest string the column can hold, so a heading and the values under it
 * share one edge at every text scale. Columns that do not fit are dropped
 * from the right, heading included, rather than overlapped. */
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

    /* Bands: title, filter, column headings, rows. A band is only used when a
     * whole row still fits under it, so a short window loses the filter line
     * before it loses the data. */
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

/* A copy of the layout translated into the scene surface for painting. Clicks
 * arrive in window-local coordinates and use the untranslated copy. */
static ProcCard proc_offset(ProcCard p, float dx, float dy) {
    p.card.x += dx;  p.card.y += dy;
    p.filter.x += dx; p.filter.y += dy;
    p.head.x += dx;   p.head.y += dy;
    p.body_y += dy;
    p.pid_r += dx; p.name_x += dx; p.name_end += dx;
    p.state_l += dx; p.state_r += dx;
    p.cpu_l += dx; p.cpu_r += dx;
    p.mem_l += dx; p.mem_r += dx;
    return p;
}

static int proc_col_at(const ProcCard *p, int x) {
    float centers[S_N];
    float best = 1e30f;
    int col = S_NAME;
    centers[S_PID] = (p->name_x - (float)ui_px(6) + p->pid_r) * 0.5f;
    centers[S_NAME] = (p->name_x + p->name_end) * 0.5f;
    centers[S_STATE] = (p->state_l + p->state_r) * 0.5f;
    centers[S_CPU] = (p->cpu_l + p->cpu_r) * 0.5f;
    centers[S_MEM] = (p->mem_l + p->mem_r) * 0.5f;
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

/* ------------------------------------------------------------
 *  Sorting and filtering - identical to sysmon.c
 * ------------------------------------------------------------ */
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
    if (r == 0) r = a->pid - b->pid;    /* pid breaks every tie */
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

/* ------------------------------------------------------------
 *  Painting the table
 * ------------------------------------------------------------ */
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
    const char *note;         /* replaces the N-of-M counter when set */
    int         note_warn;
} ProcView;

static void proc_sort_mark(Surface *s, float x, float y, int dir, u32 col) {
    float pts[6];
    float w = 7.0f, h = 6.0f;
    if (dir > 0) {                       /* ascending: up */
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

    /* filter line: the box on the left, the count (or a message) on the right */
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

    /* column headings, on the same edges the values end on */
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
        /* the sort arrow, beside the heading it orders */
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

    /* rows */
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

/* ------------------------------------------------------------
 *  /proc readers
 * ------------------------------------------------------------ */

static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Field 2 is (comm), which may contain spaces and ')', so the fields are
 * counted from the LAST ')'; field 3 is the single state letter. Returns 0 on
 * success. */
static int parse_stat(const char *buf, char *name, int namesz, char *state,
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

    char *p = close_p + 1;
    while (*p == ' ') p++;
    if (state) *state = *p ? *p : '?';
    if (!ticks) return 0;

    unsigned long long utime = 0, stime = 0;
    int field = 2;                 /* field 2 is the name just parsed */
    while (*p && field < 15) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (++field == 3) {        /* the state letter */
            while (*p && *p != ' ') p++;
            continue;
        }
        char *end;
        unsigned long long val = strtoull(p, &end, 10);
        if (end == p) {            /* not a number: step over the token */
            while (*p && *p != ' ') p++;
            continue;
        }
        if (field == 14) utime = val;
        else if (field == 15) stime = val;
        p = end;
    }
    *ticks = utime + stime;
    return 0;
}

static int stat_sample(int pid, char *name, int namesz, char *state,
                       unsigned long long *ticks) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), MON_PROC "/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return parse_stat(buf, name, namesz, state, ticks);
}

/* -1 means the file could not be read, which the table draws as "-": a
 * process whose statm is unreadable must not claim to use 0.0 MB. */
static long statm_kb(int pid, long page_kb) {
    char path[64];
    snprintf(path, sizeof(path), MON_PROC "/%d/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long size_pages = 0, rss_pages = 0;
    int got = fscanf(f, "%ld %ld", &size_pages, &rss_pages);
    fclose(f);
    if (got != 2) return -1;
    return rss_pages > 0 ? rss_pages * page_kb : 0;
}

/* ------------------------------------------------------------
 *  Monitor state and sampling
 * ------------------------------------------------------------ */
typedef struct {
    int                pid;
    unsigned long long ticks;
    double             cpu;
} Sample;

typedef struct {
    Proc   procs[MAX_PROC];
    int    nproc;
    int    seen;                   /* pids in /proc, including ones not stored */
    Sample snap[MAX_PROC];
    int    snap_count;
    double snap_t;                 /* CLOCK_MONOTONIC time of the sample */
    double sample_dt;              /* measured wall time between samples */
    int    view[MAX_PROC];
    int    nview;
    int    sort_key, sort_dir;
    char   filter[32];
    int    filter_edit;
    int    sel_pid;
    int    scroll;
    int    rows;                   /* visible rows from the last layout */
    int    w, h;
    int    stale;                  /* a user action asked for a fresh scan */
    char   msg[96];
    double msg_at;
    int    msg_ok;
    int    pending_pid;
    double pending_at;
} Monitor;

static int find_sample(const Sample *arr, int n, int pid) {
    for (int i = 0; i < n; i++) if (arr[i].pid == pid) return i;
    return -1;
}

/* One /proc walk. The TTL is checked here, before the first syscall, so a
 * draw inside the sample window performs ZERO file-system calls and the
 * table keeps showing the values read by the last walk. `force` comes from
 * a user action that can change the data (sort, filter, kill); it may run
 * the walk early but never fabricates a value - every field still comes
 * from the /proc files read in this call. */
static void scan(Monitor *m, int force) {
    double t = mono_now();

    /* A monotonic timestamp of 0 means clock_gettime failed, and t < snap_t
     * means the clock moved backwards: in both cases refresh rather than
     * trust the age of the last sample. */
    if (!force && m->snap_t > 0.0 && t >= m->snap_t &&
        t - m->snap_t < MON_TTL_S)
        return;

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;

    /* With no previous sample there is no interval to divide by; using the
     * raw monotonic reading would print "delta over 12345.60 s" on the first
     * frame. dt = 0 makes the card say it needs a second sample instead. */
    double dt = (m->snap_t > 0.0 && t > m->snap_t) ? t - m->snap_t : 0.0;

    m->nproc = 0;
    m->seen = 0;
    int nn = 0;

    DIR *d = opendir(MON_PROC);
    if (!d) {
        /* /proc is unreachable. Drop the baseline (no CPU% may be computed
         * across the gap) and move the timestamp so the retry happens at the
         * TTL instead of once per frame. */
        m->snap_count = 0;
        m->snap_t = t;
        m->sample_dt = 0.0;
        return;
    }

    static Sample next[MAX_PROC];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        m->seen++;                     /* every pid, stored or not */
        if (m->nproc >= MAX_PROC) continue;   /* list full: keep counting */

        char name[64] = "?";
        char state = '?';
        unsigned long long ticks = 0;
        if (stat_sample(pid, name, (int)sizeof(name), &state, &ticks) != 0)
            continue;                  /* the process died mid-scan */

        long kb = statm_kb(pid, page_kb);

        double cpu = -1.0;             /* no previous sample yet */
        int old = find_sample(m->snap, m->snap_count, pid);
        if (old >= 0 && dt >= MON_MIN_DT_S && ticks >= m->snap[old].ticks) {
            double dtick = (double)(ticks - m->snap[old].ticks);
            cpu = 100.0 * (dtick / (double)hz) / dt;
        } else if (old >= 0) {
            /* Too soon after the previous sample to divide: republish the %
             * that was measured over a real interval instead of a jiffy-
             * quantised one. */
            cpu = m->snap[old].cpu;
        }
        /* cpu stays -1 when there is no previous sample or the counters
         * went backwards; the table draws "-" for that, never a 0.0%
         * that looks like a measurement. */
        if (cpu > 999.0) cpu = 999.0;  /* keep the column readable */

        Proc *p = &m->procs[m->nproc++];
        p->pid = pid;
        snprintf(p->comm, sizeof(p->comm), "%s", name);
        p->state = state;
        p->mem_kb = kb;
        p->cpu = cpu;

        if (nn < MAX_PROC) {
            next[nn].pid = pid;
            next[nn].ticks = ticks;
            next[nn].cpu = cpu;
            nn++;
        }
    }
    closedir(d);

    for (int i = 0; i < nn; i++) m->snap[i] = next[i];
    m->snap_count = nn;
    m->snap_t = t;
    /* When a forced scan comes too soon to divide, the CPU% on screen is the
     * one measured over the previous real interval, so keep saying THAT
     * interval on the title line: printing the too-short dt would attach the
     * old number to a window it was not measured over. */
    if (dt == 0.0 || dt >= MON_MIN_DT_S) m->sample_dt = dt;
}

/* The events that can change what the table should show. Navigation alone is
 * not one of them: moving the cursor cannot change a process, and the sample
 * is never more than 250 ms old. */
static void scan_invalidate(Monitor *m) {
    m->stale = 1;
}

/* ------------------------------------------------------------
 *  Actions
 * ------------------------------------------------------------ */
static int proc_alive(int pid) {
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static void set_msg(Monitor *m, const char *text, int ok) {
    snprintf(m->msg, sizeof(m->msg), "%s", text);
    m->msg_at = mono_now();
    m->msg_ok = ok;
}

static void kill_selected(Monitor *m) {
    if (m->sel_pid <= 0) { set_msg(m, "select a process first", 0); return; }
    int pid = m->sel_pid;
    const Proc *p = NULL;
    for (int i = 0; i < m->nproc; i++) if (m->procs[i].pid == pid) { p = &m->procs[i]; break; }
    char name[32] = "?";
    if (p) snprintf(name, sizeof(name), "%s", p->comm);
    char buf[96];

    if (pid <= 1) {
        snprintf(buf, sizeof(buf), "refused: pid %d is init", pid);
        set_msg(m, buf, 0);
    } else if (pid == (int)getpid()) {
        snprintf(buf, sizeof(buf), "refused: pid %d is me", pid);
        set_msg(m, buf, 0);
    } else if (kill(pid, SIGTERM) == 0) {
        snprintf(buf, sizeof(buf), "SIGTERM -> %d %s", pid, name);
        set_msg(m, buf, 1);
        scan_invalidate(m);        /* the target may be gone next frame */
        m->pending_pid = pid;
        m->pending_at = mono_now() + KILL_GRACE_S;
    } else {
        snprintf(buf, sizeof(buf), "SIGTERM to %d failed: %s", pid, strerror(errno));
        set_msg(m, buf, 0);
    }
}

/* Called from draw, so the escalation happens while the window is up. */
static void check_pending(Monitor *m) {
    if (m->pending_pid <= 0) return;
    if (mono_now() < m->pending_at) return;
    int pid = m->pending_pid;
    m->pending_pid = 0;
    scan_invalidate(m);            /* the target may die from this signal */
    char buf[96];
    if (!proc_alive(pid)) {
        snprintf(buf, sizeof(buf), "pid %d exited after SIGTERM", pid);
        set_msg(m, buf, 1);
    } else if (kill(pid, SIGKILL) == 0) {
        snprintf(buf, sizeof(buf), "pid %d ignored SIGTERM: SIGKILL sent", pid);
        set_msg(m, buf, 1);
    } else {
        snprintf(buf, sizeof(buf), "SIGKILL to %d failed: %s", pid, strerror(errno));
        set_msg(m, buf, 0);
    }
}

/* ------------------------------------------------------------
 *  Layout of the window: one card and the key legend under it
 * ------------------------------------------------------------ */
typedef struct {
    ProcCard pc;
    float    margin;
    int      legend;
    float    legend_h;
} MonLayout;

static MonLayout monitor_layout(int w, int h, const CrdMetrics *m,
                                Font *fr, Font *fs) {
    MonLayout L;
    memset(&L, 0, sizeof(L));
    L.margin = (float)ui_px(10);
    L.legend_h = font_height(fs) + (float)ui_px(4);
    float avail = (float)h - 2.0f * L.margin;
    L.legend = (avail - L.legend_h) >= crd_height(m, 1, 0);
    if (L.legend) avail -= L.legend_h;
    if (avail < 1.0f) avail = 1.0f;
    CrdRect card = { L.margin, L.margin, (float)w - 2.0f * L.margin, avail };
    if (card.w < 1.0f) card.w = 1.0f;
    L.pc = proc_card_layout(&card, m, fr, fs);
    return L;
}

/* ------------------------------------------------------------
 *  App callbacks
 * ------------------------------------------------------------ */
static void monitor_init(App *a, int w, int h) {
    Monitor *m = calloc(1, sizeof(Monitor));
    if (!m) return;
    m->w = w;
    m->h = h;
    m->sort_key = S_CPU;
    m->sort_dir = -1;
    a->state = m;
}

static void monitor_free(App *a) {
    free(a->state);
    a->state = NULL;
}

/* Keep the selection on screen. Called after every move and after a filter
 * change, so the cursor and the scroll can never disagree. */
static void reveal_selection(Monitor *m) {
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0) return;
    if (sel < m->scroll) m->scroll = sel;
    if (m->rows > 0 && sel >= m->scroll + m->rows) m->scroll = sel - m->rows + 1;
    if (m->rows > 0 && m->scroll > m->nview - m->rows) m->scroll = m->nview - m->rows;
    if (m->scroll < 0) m->scroll = 0;
}

static void monitor_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Monitor *m = a->state;
    if (!m) return;
    m->w = w;
    m->h = h;

    Font *ft = font_get(FONT_SEMIBOLD, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fs = font_get(FONT_REGULAR, 10);
    if (!ft || !fr || !fs) return;

    check_pending(m);
    scan(m, m->stale);
    m->stale = 0;
    m->nview = proc_view_build(m->procs, m->nproc, m->sort_key, m->sort_dir,
                               m->filter, m->view);
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (m->nview > 0 && sel < 0) {
        m->sel_pid = m->procs[m->view[0]].pid;
        m->scroll = 0;
    }

    CrdMetrics cm = crd_metrics(ft, fr, fs);
    CrdStyle sty = crd_style(th);
    MonLayout L = monitor_layout(w, h, &cm, fr, fs);
    m->rows = L.pc.rows;
    reveal_selection(m);
    if (m->scroll > m->nview - L.pc.rows) m->scroll = m->nview - L.pc.rows;
    if (m->scroll < 0) m->scroll = 0;

    Surface clip = *s;
    int clip_r = x + w, clip_b = y + h;
    if (clip_r < 0) clip_r = 0;
    if (clip_b < 0) clip_b = 0;
    if (clip_r < clip.w) clip.w = clip_r;
    if (clip_b < clip.h) clip.h = clip_b;
    s = &clip;

    ProcView pv;
    memset(&pv, 0, sizeof(pv));
    pv.procs = m->procs;
    pv.view = m->view;
    pv.nview = m->nview;
    pv.ntotal = m->nproc;
    pv.scroll = m->scroll;
    pv.sel_pid = m->sel_pid;
    pv.sort_key = m->sort_key;
    pv.sort_dir = m->sort_dir;
    snprintf(pv.filter, sizeof(pv.filter), "%s", m->filter);
    pv.filter_edit = m->filter_edit;

    /* The card's title line carries the interval the CPU% delta was measured
     * over. "delta" is the word that matters: every CPU% here is a change
     * between two samples, not a total. Short enough for 320 px at 160%. */
    char meta[64];
    if (m->sample_dt > 0.0)
        snprintf(meta, sizeof(meta), "delta %.2f s", m->sample_dt);
    else
        snprintf(meta, sizeof(meta), "no delta yet");
    char note[96];
    note[0] = 0;
    if (m->msg[0] && mono_now() - m->msg_at < 5.0) {
        snprintf(note, sizeof(note), "%s", m->msg);
        pv.note_warn = !m->msg_ok;
    } else if (m->msg[0]) {
        m->msg[0] = 0;
    } else if (m->seen > m->nproc) {
        snprintf(note, sizeof(note), "list full: %d in /proc", m->seen);
        pv.note_warn = 1;
    }
    pv.note = note;

    ProcCard pc = proc_offset(L.pc, (float)x, (float)y);
    proc_draw(s, &pc, &cm, &sty, ft, fr, fs, &pv, "PROCESSES", meta);

    if (L.legend) {
        float ly = (float)(y + h) - L.legend_h;
        char legend[160], out[160];
        snprintf(legend, sizeof(legend),
                 "1-5 or click: sort    S: reverse    f: filter    j/k: move    K: kill");
        crd_fit(fs, legend, out, sizeof(out), (float)w - 2.0f * L.margin);
        font_draw(s, fs, (float)x + L.margin,
                  crd_top(fs, crd_base(fs, ly, L.legend_h)), out,
                  color_alpha(th->text, 0.40f));
    }
}

/* Move the cursor by delta rows in the visible order. */
static void move_sel(Monitor *m, int delta) {
    if (m->nview <= 0) return;
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0) sel = 0;
    sel += delta;
    if (sel < 0) sel = 0;
    if (sel >= m->nview) sel = m->nview - 1;
    m->sel_pid = m->procs[m->view[sel]].pid;
    reveal_selection(m);
}

static void resort(Monitor *m) {
    int keep = m->sel_pid;
    scan_invalidate(m);            /* order the freshest real sample */
    m->nview = proc_view_build(m->procs, m->nproc, m->sort_key, m->sort_dir,
                               m->filter, m->view);
    int sel = proc_view_index(m->procs, m->view, m->nview, keep);
    if (sel >= 0) m->sel_pid = keep;
    else if (m->nview > 0) m->sel_pid = m->procs[m->view[0]].pid;
    m->scroll = 0;
    reveal_selection(m);
}

static void set_sort(Monitor *m, int key) {
    if (m->sort_key == key) m->sort_dir = -m->sort_dir;
    else {
        m->sort_key = key;
        m->sort_dir = (key == S_CPU || key == S_MEM) ? -1 : 1;
    }
    resort(m);
}

static void filter_changed(Monitor *m) {
    scan_invalidate(m);            /* the surviving rows should be current */
    m->nview = proc_view_build(m->procs, m->nproc, m->sort_key, m->sort_dir,
                               m->filter, m->view);
    int sel = proc_view_index(m->procs, m->view, m->nview, m->sel_pid);
    if (sel < 0 && m->nview > 0) m->sel_pid = m->procs[m->view[0]].pid;
    m->scroll = 0;
}

static void monitor_key(App *a, const char *bytes, int n) {
    Monitor *m = a->state;
    if (!m || !bytes) return;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 2 < n && (bytes[i + 1] == '[' || bytes[i + 1] == 'O')) {
            char f = bytes[i + 2];
            if (f == 'A')      move_sel(m, -1);
            else if (f == 'B') move_sel(m,  1);
            else if (f == 'H') move_sel(m, -m->nview);
            else if (f == 'F') move_sel(m,  m->nview);
            else if (f == '5') move_sel(m, -m->rows);
            else if (f == '6') move_sel(m,  m->rows);
            i += 2;
            continue;
        }

        if (m->filter_edit) {
            if (c == 0x1B) {
                m->filter_edit = 0;
            } else if (c == '\b' || c == 0x7F) {
                size_t len = strlen(m->filter);
                if (len) m->filter[len - 1] = 0;
                filter_changed(m);
            } else if (c == '\n') {
                m->filter_edit = 0;
            } else if (c >= 32 && c != 127) {
                size_t len = strlen(m->filter);
                if (len + 1 < sizeof(m->filter)) {
                    m->filter[len] = (char)c;
                    m->filter[len + 1] = 0;
                }
                filter_changed(m);
            }
            continue;
        }

        if (c == 'j')      move_sel(m, 1);
        else if (c == 'k') move_sel(m, -1);
        else if (c == 'f' || c == '/') m->filter_edit = 1;
        else if (c == 'c') { m->filter[0] = 0; filter_changed(m); }
        else if (c == 's') set_sort(m, (m->sort_key + 1) % S_N);
        else if (c == 'S') { m->sort_dir = -m->sort_dir; resort(m); }
        else if (c >= '1' && c < '1' + S_N) set_sort(m, c - '1');
        else if (c == 'K') kill_selected(m);
        else if (c == 'g') move_sel(m, -m->nview);
        else if (c == 'G') move_sel(m,  m->nview);
    }
}

static void monitor_click(App *a, int x, int y, int buttons) {
    Monitor *m = a->state;
    if (!m || !(buttons & 1)) return;

    Font *ft = font_get(FONT_SEMIBOLD, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fs = font_get(FONT_REGULAR, 10);
    if (!ft || !fr || !fs) return;

    CrdMetrics cm = crd_metrics(ft, fr, fs);
    MonLayout L = monitor_layout(m->w, m->h, &cm, fr, fs);
    const ProcCard *pc = &L.pc;

    if (pc->show_filter && crd_hit(&pc->filter, x, y)) {
        m->filter_edit = 1;
        return;
    }
    if (pc->show_head && crd_hit(&pc->head, x, y)) {
        set_sort(m, proc_col_at(pc, x));
        return;
    }
    if (y >= (int)pc->body_y) {
        int i = (int)(((float)y - pc->body_y) / pc->row_h);
        if (i >= 0 && i < pc->rows) {
            int vi = m->scroll + i;
            if (vi >= 0 && vi < m->nview)
                m->sel_pid = m->procs[m->view[vi]].pid;
        }
    }
}

static void monitor_resize(App *a, int w, int h) {
    Monitor *m = a->state;
    if (m) {
        /* Deliberately NOT scan_invalidate(): the window size cannot change
         * /proc, and the desktop calls resize() on every mouse move while an
         * edge is dragged. The 250 ms TTL still bounds the age of the sample. */
        m->w = w;
        m->h = h;
    }
}

static void monitor_wheel(App *a, int notches) {
    Monitor *m = a->state;
    if (!m || !notches) return;
    m->scroll -= notches * 3;
    if (m->scroll > m->nview - m->rows) m->scroll = m->nview - m->rows;
    if (m->scroll < 0) m->scroll = 0;
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
