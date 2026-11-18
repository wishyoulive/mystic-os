/* ============================================================
 *  Mystic OS - apps/sysinfo.c
 *
 *  The "System" panel: a read-only report of what this machine
 *  actually is. Every number on screen is a real read: from a file in
 *  /proc or /sys, from sysconf(), from statvfs(), or from an lstat()
 *  walk of the app directory. Nothing is invented, nothing is remembered
 *  from a previous boot, and a source that cannot be opened says so
 *  ("unreadable") instead of printing a zero that looks like a fact.
 *
 *  SAMPLING. The panel reads at two rates, both one second:
 *
 *    - the static facts (kernel, host, CPU model, mount, framebuffer,
 *      bundle walk) are read once a second (FACTS_TTL) - a recursive
 *      directory walk per frame would be waste, not accuracy;
 *    - the values that do move (uptime, load average, meminfo, free
 *      space) used to be read on every frame. They are now cached the
 *      same way (LIVE_TTL, 1 s): /proc/meminfo cannot change faster
 *      than the panel can be read, and four opens per redraw is the
 *      compositor's frame rate, not the kernel's. A cached value is
 *      the last value actually read from that file, at most one second
 *      old; 'r' forces both caches to refresh immediately. A resize
 *      does not, because the window size cannot change a fact and the
 *      desktop calls resize() on every mouse move while an edge is
 *      dragged.
 *
 *  Sources, all real:
 *
 *      /proc/sys/kernel/osrelease     kernel release
 *      /proc/sys/kernel/hostname      host name
 *      /proc/uptime                   seconds since boot
 *      /proc/cpuinfo                  "model name" / "Processor", and one
 *                                     "processor" line per logical CPU
 *      sysconf(_SC_NPROCESSORS_ONLN)  CPUs actually online
 *      /proc/loadavg                  1/5/15 minute averages + nrun/nthreads
 *      /proc/meminfo                  MemTotal/MemFree/MemAvailable/Cached,
 *                                     SwapTotal/SwapFree (kB)
 *      statvfs("/root")               blocks, free blocks, fragment size
 *      /proc/mounts                   device + fs type of /root's mount
 *      /sys/class/graphics/fb0/...    virtual_size, bits_per_pixel
 *      WM_APP_DIR                     bundles (a subdir with a manifest) and
 *                                     the total size of every regular file
 *
 *  LAYOUT - one card per topic, the same visual language as Monitor
 *  (src/apps/monitor.c) and System Monitor (src/client/apps/sysmon.c):
 *
 *      a rounded panel          the theme's panel fill and border
 *      a title line             uppercase, accent, hairline underneath
 *      label/value rows         every value ends on one column
 *      no history strip        this panel is the static facts panel; it
 *                              keeps no samples, so it draws no graph
 *
 *  Cards flow into 1-3 columns of equal width, each new card under the
 *  shortest column, so nothing overlaps at any window size. The grid is
 *  measured from the real fonts (font_height/font_ascent) and ui_px(), so
 *  the text scale changes the cards instead of breaking them. The recipe
 *  is in the CARDS block below, copied byte for byte from monitor.c; the
 *  copies exist because sysinfo/monitor are linked into the desktop while
 *  sysmon is a separate client process built from src/client - a shared
 *  header would have to be added to both build systems.
 *
 *  The report scrolls (wheel, j/k, arrows, PageUp/PageDown, Home/End)
 *  because it does not fit the 520x360 window the desktop opens it with.
 *  Rows are painted through a surface that starts at the list's top edge,
 *  so a half-scrolled card is clipped instead of overwriting the footer.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifndef WM_APP_DIR
#define WM_APP_DIR "/usr/share/mystic/apps"
#endif

/* Every path the app reads is a macro so a host harness can run it against a
 * fixture tree; the shipped build uses the real ones. */
#ifndef SI_PROC
#define SI_PROC "/proc"
#endif
#ifndef SI_FB_SIZE
#define SI_FB_SIZE "/sys/class/graphics/fb0/virtual_size"
#endif
#ifndef SI_FB_BPP
#define SI_FB_BPP "/sys/class/graphics/fb0/bits_per_pixel"
#endif
#ifndef SI_APPS
#define SI_APPS WM_APP_DIR
#endif
#ifndef SI_DISK
#define SI_DISK "/root"
#endif

#define MAX_CARDS      8
#define MAX_CARD_ROWS  8
#define MAX_WALK_DEPTH 8
#define MAX_WALK_FILES 4096
#define FACTS_TTL      1.0     /* seconds a "does not change per frame" fact lives */
#define LIVE_TTL       1.0     /* seconds a volatile reading (uptime, load,
                                * meminfo, free space) lives; one second is
                                * finer than the panel can be read and coarse
                                * enough that a redraw does no I/O          */

/* ------------------------------------------------------------
 *  CARDS - one visual language for sysinfo, monitor and sysmon.
 *
 *  The frame, title, row and margin code below is identical in
 *  src/apps/monitor.c; sysmon.c carries the same block plus the
 *  sparkline painter. Change one and change all three.
 * ------------------------------------------------------------ */
typedef struct { float x, y, w, h; } CrdRect;

typedef struct {
    u32 panel, border, text, dim, accent, warn;
    float corner;
} CrdStyle;

typedef struct {
    float pad;        /* air between the card edge and its content      */
    float gap;        /* air between bands, and between label and value */
    float title_h;    /* the title line box (text centred inside)       */
    float row_h;      /* one label/value row                            */
    float line_h;     /* one single-line band: filter, column headings  */
    float spark_h;    /* a history strip: legend line + plot            */
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

/* Height of a card with `rows` rows and optionally a history strip. The
 * painter never lays anything past this, so the rect always contains exactly
 * the bands that were asked for. */
static float crd_height(const CrdMetrics *m, int rows, int spark) {
    float h = m->pad * 2.0f + m->title_h + m->gap + (float)rows * m->row_h;
    if (spark) h += m->gap + m->spark_h;
    return h;
}

/* Fit text into maxw with ".." when it has to be cut. The source is copied
 * first, because callers pass the same buffer for label and destination and
 * snprintf() with src == dst is undefined. */
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

/* The two strings one row will draw, after the column fit. Both the pain and
 * the layout test call this, so "it fits" is measured on the drawn strings. */
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

/* y of row i's band inside a card. */
static float crd_row_y(const CrdRect *c, const CrdMetrics *m, int i) {
    return c->y + m->pad + m->title_h + m->gap + (float)i * m->row_h;
}

/* The panel, its title and the hairline under the title. `meta` is optional
 * right-aligned text on the title line (a count, or "delta 0.42 s"); it
 * is dropped rather than overlapped when the card is too narrow for both. */
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
         * scale, so a narrow card still holds the title and the note together.
         * It is dropped rather than overlapped if even that fails. */
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

/* One label/value row: label left, value right-aligned on the card's value
 * column. Nothing is drawn longer than the row, and the label keeps at least
 * a gap away from the value because crd_row_text() fits both to that. */
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

/* ------------------------------------------------------------ report */
typedef struct {
    char label[64];
    char value[160];
} CardRowData;

typedef struct {
    const char *title;
    CardRowData row[MAX_CARD_ROWS];
    int nrows;
} CardData;

typedef struct {
    CardData card[MAX_CARDS];
    int n;
} Report;

typedef struct {
    int   n, cols;
    CrdRect card[MAX_CARDS];
    float content_h;
} CardGrid;

static void card_row_add(CardData *c, const char *label, const char *value) {
    if (c->nrows >= MAX_CARD_ROWS) return;
    CardRowData *r = &c->row[c->nrows++];
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
}

static CardData *card_new(Report *rep, const char *title) {
    if (rep->n >= MAX_CARDS) return NULL;
    CardData *c = &rep->card[rep->n++];
    memset(c, 0, sizeof(*c));
    c->title = title;
    return c;
}

/* Things that do not change from frame to frame, so they are read once a
 * second instead of sixty times - a recursive directory walk per frame on a
 * read-only panel would be waste, not accuracy. */
typedef struct {
    char  kernel[64];   int kernel_ok;
    char  host[64];     int host_ok;
    char  cpu[96];      int cpu_ok;
    int   cores;
    char  fs_dev[64];
    char  fs_type[32];
    char  fs_mnt[64];   int fs_ok;
    char  fb[64];       int fb_ok;
    char  apps_root[64];
    int   bundles;      int bundles_ok;
    unsigned long long apps_bytes;
    int   apps_files;   int apps_ok;
    double at;          /* CLOCK_MONOTONIC of the last refresh */
} Facts;

/* meminfo is several values from one file, so it is read in one pass. The
 * ok bits are per value: a kernel without MemAvailable must say so for that
 * row only, not replace the whole panel with a placeholder. */
enum { MEM_TOTAL = 1, MEM_FREE = 2, MEM_AVAIL = 4, MEM_CACHED = 8,
       MEM_SWAP = 16, MEM_SWAP_FREE = 32 };
typedef struct {
    long total, free_kb, avail, cached, swap_total, swap_free;
    int  ok;
} Mem;

/* The volatile half: real readings, just not on every frame. `at` is the
 * CLOCK_MONOTONIC time of the read that filled the struct. */
typedef struct {
    double uptime;      int uptime_ok;
    double l1, l5, l15; int run, threads; int load_ok;
    Mem    mem;
    struct statvfs vfs; int vfs_ok;
    double at;
} Live;

typedef struct {
    int   w, h;
    float scroll;        /* pixels the cards are scrolled up from the top  */
    float view_h;        /* list viewport, measured by the last draw       */
    float content_h;     /* total height of the card grid, measured there  */
    float row_h;         /* one fact row, for the wheel and the keys       */
    Facts facts;
    Live  live;
} SysInfo;

static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------ file readers */

/* Read one line of a file into out (trailing CR/LF stripped). Returns 0 on
 * success, -1 when the file cannot be opened or holds nothing: the caller
 * draws "unreadable" rather than a zero. */
static int slurp(const char *path, char *out, size_t cap) {
    if (!out || cap < 2) return -1;
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)cap, f)) { fclose(f); out[0] = 0; return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return out[0] ? 0 : -1;
}

static void trim_left(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/* ------------------------------------------------------------ formatting */

static void fmt_kb(char *out, size_t cap, long kb) {
    if (kb < 0) { snprintf(out, cap, "unreadable"); return; }
    if (kb < 1024)
        snprintf(out, cap, "%ld kB", kb);
    else if (kb < 1024L * 1024L)
        snprintf(out, cap, "%ld.%01ld MB", kb / 1024, (kb % 1024) * 10 / 1024);
    else
        snprintf(out, cap, "%ld.%01ld GB", kb / (1024L * 1024L),
                 (kb / 1024 % 1024) * 10 / 1024);
}

static void fmt_bytes(char *out, size_t cap, unsigned long long b) {
    if (b < 1024ULL)
        snprintf(out, cap, "%llu B", b);
    else if (b < 1024ULL * 1024ULL)
        snprintf(out, cap, "%llu.%01llu kB", b / 1024ULL, (b % 1024ULL) * 10ULL / 1024ULL);
    else if (b < 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, cap, "%llu.%01llu MB", b / (1024ULL * 1024ULL),
                 (b / 1024ULL % 1024ULL) * 10ULL / 1024ULL);
    else
        snprintf(out, cap, "%llu.%01llu GB", b / (1024ULL * 1024ULL * 1024ULL),
                 (b / (1024ULL * 1024ULL) % 1024ULL) * 10ULL / 1024ULL);
}

static void fmt_uptime(char *out, size_t cap, double secs) {
    if (secs < 0.0) { snprintf(out, cap, "unreadable"); return; }
    long s = (long)secs;
    long d = s / 86400; s %= 86400;
    long hh = s / 3600; s %= 3600;
    long mm = s / 60;
    long ss = s % 60;
    if (d > 0)       snprintf(out, cap, "%ldd %02ldh %02ldm", d, hh, mm);
    else if (hh > 0) snprintf(out, cap, "%ldh %02ldm %02lds", hh, mm, ss);
    else             snprintf(out, cap, "%ldm %02lds", mm, ss);
}

/* ------------------------------------------------------------ /proc readers */

static void mem_read(Mem *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen(SI_PROC "/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9)) {
            m->total = strtol(line + 9, NULL, 10); m->ok |= MEM_TOTAL;
        } else if (!strncmp(line, "MemFree:", 8)) {
            m->free_kb = strtol(line + 8, NULL, 10); m->ok |= MEM_FREE;
        } else if (!strncmp(line, "MemAvailable:", 13)) {
            m->avail = strtol(line + 13, NULL, 10); m->ok |= MEM_AVAIL;
        } else if (!strncmp(line, "Cached:", 7)) {
            m->cached = strtol(line + 7, NULL, 10); m->ok |= MEM_CACHED;
        } else if (!strncmp(line, "SwapTotal:", 10)) {
            m->swap_total = strtol(line + 10, NULL, 10); m->ok |= MEM_SWAP;
        } else if (!strncmp(line, "SwapFree:", 9)) {
            m->swap_free = strtol(line + 9, NULL, 10); m->ok |= MEM_SWAP_FREE;
        }
        /* No early break: the order of keys in /proc/meminfo is not part of any
         * contract, and stopping as soon as a subset is present is exactly how
         * SwapFree used to be skipped (SwapFree comes after SwapTotal), which
         * made every machine with swap look 100% swapped. The file is short;
         * reading all of it cannot get the order wrong. */
    }
    fclose(f);
}

static int loadavg_read(double *l1, double *l5, double *l15, int *run, int *threads) {
    FILE *f = fopen(SI_PROC "/loadavg", "r");
    if (!f) return -1;
    int r = fscanf(f, "%lf %lf %lf %d/%d", l1, l5, l15, run, threads);
    fclose(f);
    return r == 5 ? 0 : -1;
}

static int uptime_read(double *secs) {
    FILE *f = fopen(SI_PROC "/uptime", "r");
    if (!f) return -1;
    int r = fscanf(f, "%lf", secs);
    fclose(f);
    return r == 1 ? 0 : -1;
}

/* cpuinfo: one "processor" line per logical CPU, and the model name. x86 says
 * "model name", arm says "Processor", ppc "cpu", and s390 "vendor_id"-less
 * machines may only have "machine" - each is a real key some kernel emits. */
static void cpuinfo_read(char *model, size_t mcap, int *cores, int *model_ok) {
    *cores = 0;
    *model_ok = 0;
    model[0] = 0;
    FILE *f = fopen(SI_PROC "/cpuinfo", "r");
    if (!f) return;
    static const char *KEYS[] = { "model name", "Processor", "Hardware", "cpu model", "cpu" };
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "processor", 9)) { (*cores)++; continue; }
        if (*model_ok) continue;
        for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
            size_t kl = strlen(KEYS[k]);
            if (strncmp(line, KEYS[k], kl) != 0) continue;
            char *c = strchr(line, ':');
            if (!c) continue;
            char *v = c + 1;
            trim_left(v);
            size_t n = strlen(v);
            while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
            if (!v[0]) continue;
            snprintf(model, mcap, "%s", v);
            *model_ok = 1;
            break;
        }
    }
    fclose(f);
}

/* The mount that contains `path`: the longest mount point in /proc/mounts
 * that is a path prefix of it. Reading the table the kernel exports is more
 * honest than mapping statfs magic numbers to a name this file would have to
 * hardcode. */
static int fs_of(const char *path, char *dev, size_t devsz, char *type, size_t typesz,
                 char *mnt, size_t mntsz) {
    FILE *f = fopen(SI_PROC "/mounts", "r");
    if (!f) return -1;
    char line[512];
    size_t best = 0;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char d[128], m[256], t[64];
        if (sscanf(line, "%127s %255s %63s", d, m, t) != 3) continue;
        size_t ml = strlen(m);
        if (ml > strlen(path)) continue;
        if (strncmp(path, m, ml) != 0) continue;
        /* "/roo" must not match "/root", but "/" always ends on a boundary */
        if (ml && path[ml] != 0 && path[ml] != '/' && m[ml - 1] != '/') continue;
        if (found && ml < best) continue;
        best = ml;
        snprintf(dev, devsz, "%s", d);
        snprintf(type, typesz, "%s", t);
        snprintf(mnt, mntsz, "%s", m);
        found = 1;
    }
    fclose(f);
    return found ? 0 : -1;
}

/* ------------------------------------------------------------ app bundles */

typedef struct {
    unsigned long long bytes;
    int files;
} Tree;

static void tree_walk(const char *path, int depth, Tree *t) {
    if (depth > MAX_WALK_DEPTH || t->files >= MAX_WALK_FILES) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (t->files >= MAX_WALK_FILES) break;
        char p[512];
        if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p)) continue;
        struct stat sb;
        if (lstat(p, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            tree_walk(p, depth + 1, t);
        } else if (S_ISREG(sb.st_mode)) {
            t->files++;
            t->bytes += (unsigned long long)sb.st_size;
        }
        /* symlinks are neither followed nor counted: a bundle can point out of
         * the root, and following it would report another directory's size */
    }
    closedir(d);
}

/* A bundle is a top-level directory under the apps root that holds a real
 * "manifest" file - the same rule the store and the window server use. */
static void apps_scan(Facts *f) {
    f->apps_ok = 0;
    f->bundles_ok = 0;
    f->apps_bytes = 0;
    f->apps_files = 0;
    f->bundles = 0;
    DIR *d = opendir(SI_APPS);
    if (!d) return;
    f->bundles_ok = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[512];
        if (snprintf(p, sizeof(p), "%s/%s", SI_APPS, e->d_name) >= (int)sizeof(p)) continue;
        struct stat sb;
        if (lstat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        char man[512];
        if (snprintf(man, sizeof(man), "%s/manifest", p) >= (int)sizeof(man)) continue;
        struct stat mb;
        if (lstat(man, &mb) == 0 && S_ISREG(mb.st_mode)) f->bundles++;
    }
    closedir(d);
    Tree t = { 0, 0 };
    tree_walk(SI_APPS, 0, &t);
    f->apps_bytes = t.bytes;
    f->apps_files = t.files;
    f->apps_ok = 1;
}

/* ------------------------------------------------------------ facts */

static void facts_read(Facts *f) {
    memset(f, 0, sizeof(*f));
    f->kernel_ok = slurp(SI_PROC "/sys/kernel/osrelease", f->kernel, sizeof(f->kernel)) == 0;
    if (!f->kernel_ok) snprintf(f->kernel, sizeof(f->kernel), "unreadable");
    f->host_ok = slurp(SI_PROC "/sys/kernel/hostname", f->host, sizeof(f->host)) == 0;
    if (!f->host_ok) snprintf(f->host, sizeof(f->host), "unreadable");
    cpuinfo_read(f->cpu, sizeof(f->cpu), &f->cores, &f->cpu_ok);
    if (!f->cpu_ok) snprintf(f->cpu, sizeof(f->cpu), "unreadable");
    f->fs_ok = fs_of(SI_DISK, f->fs_dev, sizeof(f->fs_dev), f->fs_type, sizeof(f->fs_type),
                     f->fs_mnt, sizeof(f->fs_mnt)) == 0;
    if (!f->fs_ok) {
        /* /proc/mounts could not be read, so none of the three is known - the
         * mount POINT included, even though SI_DISK is the path we asked about:
         * printing "/root" as a "Mount" would be a claim we did not read. */
        snprintf(f->fs_dev, sizeof(f->fs_dev), "unreadable");
        snprintf(f->fs_type, sizeof(f->fs_type), "unreadable");
        snprintf(f->fs_mnt, sizeof(f->fs_mnt), "unreadable");
    }
    char size[32] = { 0 }, bpp[16] = { 0 };
    int have_size = slurp(SI_FB_SIZE, size, sizeof(size)) == 0;
    int have_bpp = slurp(SI_FB_BPP, bpp, sizeof(bpp)) == 0;
    if (have_size) {
        for (char *p = size; *p; p++) if (*p == ',') *p = 'x';
        if (have_bpp) snprintf(f->fb, sizeof(f->fb), "%s, %s bpp", size, bpp);
        else          snprintf(f->fb, sizeof(f->fb), "%s", size);
        f->fb_ok = 1;
    } else {
        snprintf(f->fb, sizeof(f->fb), "unreadable");
    }
    snprintf(f->apps_root, sizeof(f->apps_root), "%s", SI_APPS);
    apps_scan(f);
    f->at = mono_now();
}

/* Only the "does not change per frame" half is re-read once a second. */
static void facts_refresh(Facts *f, int force) {
    double now = mono_now();
    if (!force && f->at > 0.0 && now >= f->at && now - f->at < FACTS_TTL) return;
    facts_read(f);
}

/* The volatile half: four real reads (uptime, loadavg, meminfo, statvfs).
 * Every one keeps its own ok flag, so a source that fails still says
 * "unreadable" for its row after the value is cached. */
static void live_read(Live *l) {
    memset(l, 0, sizeof(*l));
    l->uptime_ok = uptime_read(&l->uptime) == 0;
    if (!l->uptime_ok) l->uptime = -1.0;
    l->load_ok = loadavg_read(&l->l1, &l->l5, &l->l15, &l->run, &l->threads) == 0;
    if (!l->load_ok) { l->run = -1; l->threads = -1; }
    mem_read(&l->mem);
    l->vfs_ok = statvfs(SI_DISK, &l->vfs) == 0;
    l->at = mono_now();
}

/* Same TTL rule as the facts: a failed or backwards clock refreshes rather
 * than trust an unbounded age. */
static void live_refresh(Live *l, int force) {
    double now = mono_now();
    if (!force && l->at > 0.0 && now >= l->at && now - l->at < LIVE_TTL) return;
    live_read(l);
}

/* ------------------------------------------------------------ report builder */

/* Build the cards. Every value comes from the two caches, which hold the
 * last real read of each source (facts: 1 s TTL, live: 1 s TTL). */
static void report_build(Report *rep, const Facts *f, const Live *l) {
    memset(rep, 0, sizeof(*rep));
    char v[160], a[64], b[64];
    long online = sysconf(_SC_NPROCESSORS_ONLN);

    CardData *c = card_new(rep, "SYSTEM");
    if (c) {
        card_row_add(c, "Host", f->host);
        card_row_add(c, "Kernel", f->kernel);
        double up = l->uptime_ok ? l->uptime : -1.0;
        fmt_uptime(v, sizeof(v), up);
        card_row_add(c, "Uptime", v);
        card_row_add(c, "Framebuffer", f->fb);
    }

    c = card_new(rep, "CPU");
    if (c) {
        card_row_add(c, "Processor", f->cpu);
        if (f->cores <= 0)
            snprintf(v, sizeof(v), "unreadable");
        else if (online > 0)
            snprintf(v, sizeof(v), "%d logical, %ld online", f->cores, online);
        else
            /* sysconf() failed: the logical count is still a real reading, the
             * online count is not, so it is reported as missing rather than copied
             * from the logical count. */
            snprintf(v, sizeof(v), "%d logical, online unreadable", f->cores);
        card_row_add(c, "Cores", v);
        double l1, l5, l15;
        int run = -1, threads = -1;
        if (l->load_ok) {
            l1 = l->l1; l5 = l->l5; l15 = l->l15;
            run = l->run; threads = l->threads;
            snprintf(v, sizeof(v), "%.2f  %.2f  %.2f", l1, l5, l15);
        } else {
            snprintf(v, sizeof(v), "unreadable");
        }
        card_row_add(c, "Load 1/5/15", v);
        if (run >= 0 && threads >= 0)
            snprintf(v, sizeof(v), "%d running, %d threads", run, threads);
        else
            snprintf(v, sizeof(v), "unreadable");
        card_row_add(c, "Tasks", v);
    }

    c = card_new(rep, "MEMORY");
    if (c) {
        const Mem *m = &l->mem;
        long used = (m->ok & (MEM_TOTAL | MEM_AVAIL)) ? m->total - m->avail : -1;
        if (used < 0) used = -1;
        if (used >= 0) {
            fmt_kb(a, sizeof(a), used);
            fmt_kb(b, sizeof(b), m->total);
            snprintf(v, sizeof(v), "%s of %s", a, b);
        } else {
            snprintf(v, sizeof(v), "unreadable");
        }
        card_row_add(c, "In use", v);
        if (m->ok & MEM_FREE) fmt_kb(v, sizeof(v), m->free_kb); else snprintf(v, sizeof(v), "unreadable");
        card_row_add(c, "Free", v);
        if (m->ok & MEM_AVAIL) fmt_kb(v, sizeof(v), m->avail); else snprintf(v, sizeof(v), "unreadable");
        card_row_add(c, "Available", v);
        if (m->ok & MEM_CACHED) fmt_kb(v, sizeof(v), m->cached); else snprintf(v, sizeof(v), "unreadable");
        card_row_add(c, "Cached", v);
        if ((m->ok & (MEM_SWAP | MEM_SWAP_FREE)) != (MEM_SWAP | MEM_SWAP_FREE))
            snprintf(v, sizeof(v), "unreadable");
        else if (m->swap_total <= 0) snprintf(v, sizeof(v), "none");
        else {
            long swap_used = m->swap_total - m->swap_free;
            if (swap_used < 0) swap_used = 0;
            fmt_kb(a, sizeof(a), swap_used);
            fmt_kb(b, sizeof(b), m->swap_total);
            snprintf(v, sizeof(v), "%s of %s", a, b);
        }
        card_row_add(c, "Swap", v);
    }

    c = card_new(rep, "STORAGE");
    if (c) {
        card_row_add(c, "Mount", f->fs_mnt);
        card_row_add(c, "Device", f->fs_dev);
        card_row_add(c, "Filesystem", f->fs_type);
        struct statvfs vfs;
        int vfs_ok = l->vfs_ok;
        if (vfs_ok) vfs = l->vfs;
        else memset(&vfs, 0, sizeof(vfs));
        unsigned long long frs = vfs_ok ? (unsigned long long)vfs.f_frsize : 0;
        if (vfs_ok && frs == 0) frs = (unsigned long long)vfs.f_bsize;
        if (vfs_ok && frs > 0) {
            unsigned long long total = (unsigned long long)vfs.f_blocks * frs;
            unsigned long long freeb = (unsigned long long)vfs.f_bfree * frs;
            unsigned long long usedb = total > freeb ? total - freeb : 0;
            fmt_bytes(a, sizeof(a), usedb);
            fmt_bytes(b, sizeof(b), total);
            int pct = total ? (int)(usedb * 100ULL / total) : 0;
            snprintf(v, sizeof(v), "%s / %s (%d%%)", a, b, pct);
            card_row_add(c, "Used", v);
            fmt_bytes(v, sizeof(v), (unsigned long long)vfs.f_bavail * frs);
            card_row_add(c, "Free", v);
        } else {
            card_row_add(c, "Used", "unreadable");
            card_row_add(c, "Free", "unreadable");
        }
    }

    c = card_new(rep, "PACKAGES");
    if (c) {
        card_row_add(c, "Root", f->apps_root);
        if (f->bundles_ok) snprintf(v, sizeof(v), "%d", f->bundles);
        else               snprintf(v, sizeof(v), "unreadable");
        card_row_add(c, "Bundles", v);
        if (f->apps_ok) {
            fmt_bytes(v, sizeof(v), f->apps_bytes);
            snprintf(a, sizeof(a), "%s", v);
            snprintf(v, sizeof(v), "%s (%d files)", a, f->apps_files);
        } else {
            snprintf(v, sizeof(v), "unreadable");
        }
        card_row_add(c, "Size", v);
    }
}

/* ------------------------------------------------------------ card grid */

/* Cards flow into columns: 1 on a narrow window, up to 3 when the width can
 * hold three cards at a readable width. Each card goes under the shortest
 * column, which is what keeps the grid balanced - and, because a card is only
 * ever placed below the last one in its column, is what makes overlap
 * impossible by construction.
 *
 * Coordinates are window-local: x starts at the margin, the first card's top
 * is the top margin. content_h includes the bottom margin. */
static CardGrid card_grid(int w, int h, const CrdMetrics *m,
                          const int *rows, int n) {
    CardGrid g;
    memset(&g, 0, sizeof(g));
    (void)h;
    float margin = (float)ui_px(10);
    float gap = (float)ui_px(10);
    int cols = (int)(((float)w - 2.0f * margin + gap) / ((float)ui_px(240) + gap));
    if (cols < 1) cols = 1;
    if (cols > 3) cols = 3;
    if (n > 0 && cols > n) cols = n;
    float colw = ((float)w - 2.0f * margin - (float)(cols - 1) * gap) / (float)cols;
    if (colw < 1.0f) colw = 1.0f;
    g.cols = cols;
    g.n = n;
    float col_y[3] = { margin, margin, margin };
    float maxy = margin;
    for (int i = 0; i < n; i++) {
        int ci = 0;
        for (int k = 1; k < cols; k++) if (col_y[k] < col_y[ci]) ci = k;
        float ch = crd_height(m, rows[i], 0);
        g.card[i].x = margin + (float)ci * (colw + gap);
        g.card[i].y = col_y[ci];
        g.card[i].w = colw;
        g.card[i].h = ch;
        col_y[ci] += ch + gap;
        if (col_y[ci] - gap > maxy) maxy = col_y[ci] - gap;
    }
    g.content_h = maxy + margin;
    return g;
}

/* The footer is only reserved when a card row still fits above it: in a very
 * short window the cards get the space instead of an empty strip. */
static float foot_height(const CrdMetrics *m, Font *fd, int h) {
    float fh = font_height(fd) + (float)ui_px(6);
    if ((float)h - 2.0f * (float)ui_px(10) - fh < crd_height(m, 1, 0)) return 0.0f;
    return fh;
}

/* ------------------------------------------------------------ AppVT */

static void sysinfo_init(App *a, int w, int h) {
    SysInfo *s = calloc(1, sizeof(SysInfo));
    if (!s) return;
    s->w = w;
    s->h = h;
    a->state = s;
}

static void sysinfo_free(App *a) {
    free(a->state);
    a->state = NULL;
}

/* Clip to the rectangle we were given. The window manager hands over the whole
 * scene surface, so keeping pixels inside the app rect is the app's job. */
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
    st->w = w;
    st->h = h;

    Font *ft = font_get(FONT_SEMIBOLD, 12); /* card title: same face and size as
                                              monitor.c and sysmon.c */
    Font *fr = font_get(FONT_REGULAR, 11);  /* label/value  */
    Font *fd = font_get(FONT_REGULAR, 10);  /* footer       */
    if (!ft || !fr || !fd) return;

    facts_refresh(&st->facts, 0);
    live_refresh(&st->live, 0);
    Report rep;
    report_build(&rep, &st->facts, &st->live);
    if (rep.n < 1) return;

    const CrdMetrics m = crd_metrics(ft, fr, fd);
    const CrdStyle sty = crd_style(th);
    const float foot_h = foot_height(&m, fd, h);
    float view_h = (float)h - foot_h;
    if (view_h < 0.0f) view_h = 0.0f;

    int rows[MAX_CARDS];
    for (int i = 0; i < rep.n && i < MAX_CARDS; i++) rows[i] = rep.card[i].nrows;
    CardGrid g = card_grid(w, h, &m, rows, rep.n);
    st->content_h = g.content_h;
    st->view_h = view_h;
    st->row_h = m.row_h;

    float max_scroll = g.content_h - view_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0.0f) st->scroll = 0.0f;

    Surface clip = rect_clip(s, x, y, w, h);
    s = &clip;

    /* The card surface's first pixel row is the list's top edge: cards are
     * drawn with y relative to it, so a half-scrolled card is clipped by the
     * surface bounds instead of painting over the footer. */
    Surface list = clip;
    int list_top = y;
    int list_h = (int)(view_h + 0.5f);
    if (list_h < 0) list_h = 0;
    if (list_h > 0 && list_top >= 0 && list_top < clip.h) {
        list.px = clip.px + (size_t)list_top * clip.stride;
        list.h = list_h < clip.h - list_top ? list_h : clip.h - list_top;
        list.cx0 = list.cy0 = list.cx1 = list.cy1 = 0;   /* bounds are the clip */
    } else {
        list.h = 0;
    }

    for (int i = 0; i < g.n; i++) {
        CrdRect c = g.card[i];
        c.x += (float)x;
        c.y -= st->scroll;
        if (c.y > (float)list.h || c.y + c.h < 0.0f) continue;
        crd_begin(&list, &c, &m, &sty, ft, fd, rep.card[i].title, NULL, 0);
        for (int r = 0; r < rep.card[i].nrows; r++)
            crd_row(&list, &c, &m, &sty, fr, r,
                    rep.card[i].row[r].label, rep.card[i].row[r].value);
    }

    /* Scroll indicator: where the view is inside the content, on the window's
     * own right edge (outside the cards, so it never touches a value). */
    if (g.content_h > view_h + 0.5f && view_h > 1.0f) {
        float bar_w = (float)ui_px(3);
        float track_x = (float)(x + w) - (float)ui_px(4);
        float track_h = view_h - (float)ui_px(10);
        if (track_h > 4.0f) {
            float thumb_h = track_h * (view_h / g.content_h);
            if (thumb_h < (float)ui_px(18)) thumb_h = (float)ui_px(18);
            if (thumb_h > track_h) thumb_h = track_h;
            float thumb_y = (float)y + (float)ui_px(5) +
                            (track_h - thumb_h) * (st->scroll / (g.content_h - view_h));
            surf_blend(s, (int)track_x, y + ui_px(5), (int)bar_w, (int)track_h,
                       color_alpha(th->text, 0.08f));
            surf_blend(s, (int)track_x, (int)thumb_y, (int)bar_w, (int)thumb_h,
                       color_alpha(th->accent, 0.75f));
        }
    }

    /* Footer: what the keys do, and where these numbers come from. It lives
     * on the window, not in the scrolled list, so it is never scrolled away. */
    if (foot_h > 0.0f) {
        char legend[160], nb[160];
        snprintf(legend, sizeof(legend),
                 "wheel / j k / PgUp PgDn scroll    r rereads /proc");
        crd_fit(fd, legend, nb, sizeof(nb), (float)w - 2.0f * (float)ui_px(12));
        float ly = (float)(y + h) - foot_h;
        surf_blend(s, x, (int)ly, w, (int)foot_h, color_alpha(th->text, 0.05f));
        font_draw(s, fd, (float)x + (float)ui_px(12),
                  crd_top(fd, crd_base(fd, ly, foot_h)), nb,
                  color_alpha(th->text, 0.45f));
    }
}

/* One place that moves the view: every key and the wheel end up here, so the
 * clamp can never be forgotten by one of them. */
static void scroll_by(SysInfo *st, float dy) {
    st->scroll += dy;
    float max_scroll = st->content_h - st->view_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0.0f) st->scroll = 0.0f;
}

static void sysinfo_key(App *a, const char *bytes, int n) {
    SysInfo *st = a->state;
    if (!st || !bytes) return;
    float page = st->view_h > st->row_h ? st->view_h - st->row_h : st->row_h;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == 0x1B && i + 2 < n && (bytes[i + 1] == '[' || bytes[i + 1] == 'O')) {
            char f = bytes[i + 2];
            if (f == 'A')      scroll_by(st, -st->row_h);
            else if (f == 'B') scroll_by(st,  st->row_h);
            else if (f == 'H') st->scroll = 0.0f;
            else if (f == 'F') scroll_by(st,  st->content_h);
            else if (f == '5') scroll_by(st, -page);
            else if (f == '6') scroll_by(st,  page);
            i += 2;
            continue;
        }
        if (c == 'j')      scroll_by(st,  st->row_h);
        else if (c == 'k') scroll_by(st, -st->row_h);
        else if (c == 'g') st->scroll = 0.0f;
        else if (c == 'G') scroll_by(st,  st->content_h);
        else if (c == 'r') {
            /* Explicit reload: the requirement was for the files to be read
             * again, so both caches are refilled now. */
            facts_refresh(&st->facts, 1);
            live_refresh(&st->live, 1);
        }
    }
}

static void sysinfo_wheel(App *a, int notches) {
    SysInfo *st = a->state;
    if (!st || !notches) return;
    scroll_by(st, -(float)notches * 3.0f * st->row_h);
}

static void sysinfo_resize(App *a, int w, int h) {
    SysInfo *s = a->state;
    if (s) {
        /* No cache invalidation here on purpose: a window size cannot change
         * a fact or a meminfo reading, and the desktop calls resize() on
         * every mouse move while an edge is dragged - refilling the 78-call
         * facts sweep there would put it back on a per-frame path. */
        s->w = w;
        s->h = h;
    }
}

const AppVT app_vt_sysinfo = {
    .id = "sysinfo",
    .title = "System",
    .init = sysinfo_init,
    .draw = sysinfo_draw,
    .key = sysinfo_key,
    .resize = sysinfo_resize,
    .wheel = sysinfo_wheel,
    .free = sysinfo_free,
};
