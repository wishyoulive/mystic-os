/* ============================================================
 *  Mystic OS - apps/sysinfo.c
 *
 *  The "System" panel: a read-only report of what this machine
 *  actually is. Every number on screen was read from the kernel on
 *  the frame it is shown, or from a directory listing that was taken
 *  within the last second. Nothing is invented, nothing is remembered
 *  from a previous boot, and a source that cannot be opened says so
 *  ("unreadable") instead of printing a zero that looks like a fact.
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
 *  Layout: one grid, derived from the fonts at draw time - the same
 *  grid the other apps use.
 *
 *      lab_x   every label, every section heading, the memory bar's left end
 *      val_r   every value, right-aligned, and the memory bar's right end
 *      row_h   one fact row: a font line box plus ui_px(8) of air
 *      head_h  one section heading: font line box plus ui_px(10)
 *
 *  A label is centred on its real ink box (ascent..descent) inside the
 *  row, so it stays centred at every text scale. Nothing here is a bare
 *  pixel height: the panel was built to survive the text-size setting,
 *  and the values all share one right edge because val_r is measured
 *  from the window edge, not from the longest string.
 *
 *  The list scrolls (wheel, j/k, arrows, PageUp/PageDown, Home/End)
 *  because the whole report does not fit in the 520x360 window the
 *  desktop opens it with - the window is not resized to fit the app.
 *  The scroll is clamped against the measured content height, and the
 *  rows are painted through a scissor surface that starts at the list's
 *  top, so a half-scrolled row is clipped instead of overwriting the
 *  header.
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

#define MAX_ROWS     48
#define MAX_WALK_DEPTH 8
#define MAX_WALK_FILES 4096
#define FACTS_TTL    1.0     /* seconds a "does not change per frame" fact lives */

/* ------------------------------------------------------------ rows */
enum { R_HEAD, R_ROW, R_BAR };

typedef struct {
    int   kind;
    char  label[64];
    char  value[160];
    float frac;              /* R_BAR: filled fraction, 0..1 */
} Row;

/* Things that do not change from frame to frame, so they are read once a
 * second instead of sixty times - a recursive directory walk per frame on a
 * read-only panel would be waste, not accuracy. */
typedef struct {
    char  kernel[64];   int kernel_ok;
    char  host[64];     int host_ok;
    char  cpu[96];      int cpu_ok;
    int   cores;        int cores_ok;
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

typedef struct {
    int   w, h;
    float scroll;        /* pixels the list is scrolled down from the top */
    float view_h;        /* list viewport, measured by the last draw       */
    float content_h;     /* total height of every row, measured there too  */
    float row_h;         /* one fact row, for the wheel and the keys       */
    Facts facts;
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

/* Trim a name so it fits a column, with ".." when it had to be cut: the value
 * column only has so much room and a path may not overrun the label. */
static void fit_text(Font *f, const char *src, char *out, size_t cap, float maxw) {
    snprintf(out, cap, "%s", src ? src : "");
    if (maxw <= 0.0f) { out[0] = 0; return; }
    if (font_width(f, out) <= maxw) return;
    size_t n = strlen(src ? src : "");
    while (n > 0) {
        n--;
        snprintf(out, cap, "%.*s..", (int)n, src);
        if (font_width(f, out) <= maxw) return;
    }
    out[0] = 0;
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
    if (d > 0)      snprintf(out, cap, "%ldd %02ldh %02ldm", d, hh, mm);
    else if (hh > 0) snprintf(out, cap, "%ldh %02ldm %02lds", hh, mm, ss);
    else            snprintf(out, cap, "%ldm %02lds", mm, ss);
}

/* ------------------------------------------------------------ /proc readers */

/* meminfo is several values from one file, so it is read in one pass. The
 * ok bits are per value: a kernel without MemAvailable must say so for that
 * row only, not replace the whole panel with a placeholder. */
enum { MEM_TOTAL = 1, MEM_FREE = 2, MEM_AVAIL = 4, MEM_CACHED = 8,
       MEM_SWAP = 16, MEM_SWAP_FREE = 32 };
typedef struct {
    long total, free_kb, avail, cached, swap_total, swap_free;
    int  ok;
} Mem;

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

/* Only the cheap half is re-read per frame; the rest lives for one second. */
static void facts_refresh(Facts *f, int force) {
    double now = mono_now();
    if (!force && f->at > 0.0 && now >= f->at && now - f->at < FACTS_TTL) return;
    facts_read(f);
}

/* ------------------------------------------------------------ geometry */

/* font_draw() takes y as the ascender line and puts the baseline font_ascent()
 * below it (src/font.c:229); ascent+descent is the nominal size while
 * font_height() is size*1.4, so the ink box is font_height/1.4 tall. Centring
 * that box - not font_height() - is what keeps a label in the middle of its row
 * at every text scale. */
static float ink_h(Font *f) { return font_height(f) / 1.4f; }
static float base_y(Font *f, float top, float band) {
    return top + (band - ink_h(f)) * 0.5f + font_ascent(f);
}
static float top_y(Font *f, float base) { return base - font_ascent(f); }

typedef struct {
    float m, gap;        /* margins */
    float top;           /* list top, inside the window */
    float row_h;         /* one fact row */
    float head_h;        /* one section heading */
    float bar_h;         /* memory bar thickness */
    float foot_h;        /* legend band at the bottom */
    float lab_x, val_r;  /* the two columns */
} Geo;

static Geo geo_of(int x, int w, Font *fh, Font *fr, Font *fd) {
    Geo g;
    g.m      = (float)ui_px(12);
    g.gap    = (float)ui_px(8);
    g.top    = (float)ui_px(8);
    g.row_h  = font_height(fr) + (float)ui_px(8);
    g.head_h = font_height(fh) + (float)ui_px(10);
    g.bar_h  = font_height(fr) * 0.45f;
    if (g.bar_h < 3.0f) g.bar_h = 3.0f;
    g.foot_h = font_height(fd) + (float)ui_px(6);
    g.lab_x  = (float)x + g.m;
    g.val_r  = (float)(x + w) - g.m - font_width(fd, "  ");   /* room for the bar */
    return g;
}

/* Row height, one place for the painter and the measure pass. */
static float row_height(const Geo *g, const Row *r) {
    switch (r->kind) {
        case R_HEAD: return g->head_h;
        case R_BAR:  return g->bar_h + (float)ui_px(10);
        default:     return g->row_h;
    }
}

/* One fact row: label on the left column, value ending on the right one.
 * A value never takes more than half the row, so the label stays readable in
 * the smallest window; the label gets whatever is left. */
static void draw_row(Surface *s, Font *fr, const Geo *g,
                     float ry, const char *label, const char *value,
                     u32 lc, u32 vc) {
    char raw[192], vb[192], nb[192];
    float vw = 0.0f;
    /* fit from a separate buffer: snprintf() with src == dst is undefined, and
     * fit_text() formats into the destination it was given */
    snprintf(raw, sizeof(raw), "%s", value ? value : "");
    vb[0] = 0;
    if (raw[0]) {
        fit_text(fr, raw, vb, sizeof(vb), (g->val_r - g->lab_x) * 0.55f);
        if (vb[0]) vw = font_width(fr, vb) + g->gap;
    }
    fit_text(fr, label, nb, sizeof(nb), g->val_r - vw - g->lab_x);
    float base = base_y(fr, ry, g->row_h);
    font_draw(s, fr, g->lab_x, top_y(fr, base), nb, lc);
    if (vb[0])
        font_draw(s, fr, g->val_r - font_width(fr, vb), top_y(fr, base), vb, vc);
}

/* A section heading: small caps in the accent, on its own taller band, with a
 * hairline under it that runs from the label column to the value column. */
static void draw_head(Surface *s, Font *fd, const Geo *g, float ry, const char *text,
                      const Theme *th) {
    char nb[96];
    fit_text(fd, text, nb, sizeof(nb), g->val_r - g->lab_x);
    float base = base_y(fd, ry, g->head_h);
    font_draw(s, fd, g->lab_x, top_y(fd, base), nb, color_alpha(th->accent, 0.85f));
    float line_y = ry + g->head_h - 1.0f;
    gfx_line(s, g->lab_x, line_y, g->val_r, line_y, 1.0f, color_alpha(th->text, 0.12f));
}

/* ------------------------------------------------------------ row builder */

static void add_head(Row *rows, int *n, const char *label) {
    if (*n >= MAX_ROWS) return;
    Row *r = &rows[(*n)++];
    memset(r, 0, sizeof(*r));
    r->kind = R_HEAD;
    snprintf(r->label, sizeof(r->label), "%s", label);
}

static void add_row(Row *rows, int *n, const char *label, const char *value) {
    if (*n >= MAX_ROWS) return;
    Row *r = &rows[(*n)++];
    memset(r, 0, sizeof(*r));
    r->kind = R_ROW;
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
}

static void add_bar(Row *rows, int *n, float frac) {
    if (*n >= MAX_ROWS) return;
    Row *r = &rows[(*n)++];
    memset(r, 0, sizeof(*r));
    r->kind = R_BAR;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    r->frac = frac;
}

/* Build the report. Everything volatile is read here, on this frame. */
static void build_rows(Row *rows, int *n, const Facts *f) {
    *n = 0;
    char v[160], a[64], b[64];
    long online = sysconf(_SC_NPROCESSORS_ONLN);

    add_head(rows, n, "SYSTEM");
    add_row(rows, n, "Kernel", f->kernel);
    add_row(rows, n, "Host", f->host);
    double up = -1.0;
    if (uptime_read(&up) != 0) up = -1.0;
    fmt_uptime(v, sizeof(v), up);
    add_row(rows, n, "Uptime", v);

    add_head(rows, n, "CPU");
    add_row(rows, n, "Processor", f->cpu);
    if (f->cores <= 0)
        snprintf(v, sizeof(v), "unreadable");
    else if (online > 0)
        snprintf(v, sizeof(v), "%d logical, %ld online", f->cores, online);
    else
        /* sysconf() failed: the logical count is still a real reading, the
         * online count is not, so it is reported as missing rather than copied
         * from the logical count. */
        snprintf(v, sizeof(v), "%d logical, online unreadable", f->cores);
    add_row(rows, n, "Cores", v);
    double l1, l5, l15;
    int run = -1, threads = -1;
    if (loadavg_read(&l1, &l5, &l15, &run, &threads) == 0)
        snprintf(v, sizeof(v), "%.2f  %.2f  %.2f", l1, l5, l15);
    else
        snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Load 1/5/15", v);
    if (run >= 0 && threads >= 0)
        snprintf(v, sizeof(v), "%d running, %d threads", run, threads);
    else
        snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Tasks", v);

    add_head(rows, n, "MEMORY");
    Mem m;
    mem_read(&m);
    long used = (m.ok & (MEM_TOTAL | MEM_AVAIL)) ? m.total - m.avail : -1;
    if (used < 0) used = -1;
    if (used >= 0) {
        fmt_kb(a, sizeof(a), used);
        fmt_kb(b, sizeof(b), m.total);
        snprintf(v, sizeof(v), "%s of %s", a, b);
    } else {
        snprintf(v, sizeof(v), "unreadable");
    }
    add_row(rows, n, "In use", v);
    add_bar(rows, n, (m.ok & MEM_TOTAL) && m.total > 0 && used >= 0
                          ? (float)used / (float)m.total : 0.0f);
    if (m.ok & MEM_FREE) fmt_kb(v, sizeof(v), m.free_kb); else snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Free", v);
    if (m.ok & MEM_AVAIL) fmt_kb(v, sizeof(v), m.avail); else snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Available", v);
    if (m.ok & MEM_CACHED) fmt_kb(v, sizeof(v), m.cached); else snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Cached", v);
    if ((m.ok & (MEM_SWAP | MEM_SWAP_FREE)) != (MEM_SWAP | MEM_SWAP_FREE))
        snprintf(v, sizeof(v), "unreadable");
    else if (m.swap_total <= 0) snprintf(v, sizeof(v), "none");
    else {
        long swap_used = m.swap_total - m.swap_free;
        if (swap_used < 0) swap_used = 0;
        fmt_kb(a, sizeof(a), swap_used);
        fmt_kb(b, sizeof(b), m.swap_total);
        snprintf(v, sizeof(v), "%s of %s", a, b);
    }
    add_row(rows, n, "Swap", v);

    add_head(rows, n, "STORAGE");
    add_row(rows, n, "Mount", f->fs_mnt);
    add_row(rows, n, "Device", f->fs_dev);
    add_row(rows, n, "Filesystem", f->fs_type);
    struct statvfs vfs;
    int vfs_ok = statvfs(SI_DISK, &vfs) == 0;
    unsigned long long frs = vfs_ok ? (unsigned long long)vfs.f_frsize : 0;
    if (vfs_ok && frs == 0) frs = (unsigned long long)vfs.f_bsize;
    if (vfs_ok && frs > 0) {
        unsigned long long total = (unsigned long long)vfs.f_blocks * frs;
        unsigned long long freeb = (unsigned long long)vfs.f_bfree * frs;
        unsigned long long usedb = total > freeb ? total - freeb : 0;
        fmt_bytes(a, sizeof(a), usedb);
        fmt_bytes(b, sizeof(b), total);
        int pct = total ? (int)(usedb * 100ULL / total) : 0;
        snprintf(v, sizeof(v), "%s of %s (%d%%)", a, b, pct);
        add_row(rows, n, "Disk used", v);
        fmt_bytes(v, sizeof(v), (unsigned long long)vfs.f_bavail * frs);
        add_row(rows, n, "Disk free", v);
    } else {
        add_row(rows, n, "Disk used", "unreadable");
        add_row(rows, n, "Disk free", "unreadable");
    }

    add_head(rows, n, "DISPLAY");
    add_row(rows, n, "Framebuffer", f->fb);

    add_head(rows, n, "PACKAGES");
    add_row(rows, n, "Root", f->apps_root);
    if (f->bundles_ok) snprintf(v, sizeof(v), "%d", f->bundles);
    else               snprintf(v, sizeof(v), "unreadable");
    add_row(rows, n, "Bundles", v);
    if (f->apps_ok) {
        fmt_bytes(v, sizeof(v), f->apps_bytes);
        snprintf(a, sizeof(a), "%s", v);
        snprintf(v, sizeof(v), "%s (%d files)", a, f->apps_files);
    } else {
        snprintf(v, sizeof(v), "unreadable");
    }
    add_row(rows, n, "Size", v);
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

    Font *fh = font_get(FONT_MEDIUM, 13);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr || !fd) return;

    facts_refresh(&st->facts, 0);

    Row rows[MAX_ROWS];
    int n = 0;
    build_rows(rows, &n, &st->facts);

    const Geo g = geo_of(x, w, fh, fr, fd);

    /* One measure pass; the scroll clamp, the painter and the key handler all
     * read these numbers, so none of them can disagree about the content. The
     * legend band is only reserved when a row still fits above it - in a very
     * short window the list gets the space instead of an empty strip. */
    float total = 0.0f;
    for (int i = 0; i < n; i++) total += row_height(&g, &rows[i]);
    int foot_ok = (float)h - g.top - g.foot_h >= g.row_h;
    float view_h = (float)h - g.top - (foot_ok ? g.foot_h : 0.0f);
    if (view_h < 0.0f) view_h = 0.0f;
    st->content_h = total;
    st->view_h = view_h;
    st->row_h = g.row_h;

    float max_scroll = total - view_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0.0f) st->scroll = 0.0f;

    Surface clip = rect_clip(s, x, y, w, h);
    s = &clip;

    /* The list is its own surface whose first row of pixels is the list's top
     * edge: every draw below is in list coordinates, so a row that is half
     * scrolled out is clipped by the surface's own bounds instead of painting
     * over the header. */
    Surface list = clip;
    int list_top = y + (int)(g.top + 0.5f);
    int list_h = (int)(view_h + 0.5f);
    if (list_h < 0) list_h = 0;
    if (list_h > 0 && list_top >= 0 && list_top < clip.h) {
        list.px = clip.px + (size_t)list_top * clip.stride;
        list.h = list_h < clip.h - list_top ? list_h : clip.h - list_top;
    } else {
        list.h = 0;
    }

    float ry = -st->scroll;
    for (int i = 0; i < n && list.h > 0; i++) {
        Row *r = &rows[i];
        float rh = row_height(&g, r);
        if (ry + rh >= 0.0f && ry <= (float)list.h) {
            switch (r->kind) {
                case R_HEAD:
                    draw_head(&list, fd, &g, ry, r->label, th);
                    break;
                case R_ROW:
                    draw_row(&list, fr, &g, ry, r->label, r->value,
                             color_alpha(th->text, 0.85f),
                             color_alpha(th->text, 0.55f));
                    break;
                case R_BAR: {
                    float by = ry + (row_height(&g, r) - g.bar_h) * 0.5f;
                    float bw = g.val_r - g.lab_x;
                    if (bw > 1.0f) {
                        gfx_rrect_aa(&list, g.lab_x, by, bw, g.bar_h, g.bar_h * 0.5f,
                                     color_alpha(th->text, 0.10f), 0, 0);
                        if (r->frac > 0.005f)
                            gfx_rrect_aa(&list, g.lab_x, by, bw * r->frac, g.bar_h,
                                         g.bar_h * 0.5f, th->accent, 0, 0);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        ry += rh;
    }

    /* Scroll indicator: where the view is inside the content, on the window's
     * own right edge (outside val_r, so it never touches a value). */
    if (total > view_h + 0.5f && view_h > 1.0f) {
        float bar_w = (float)ui_px(3);
        float track_x = (float)(x + w) - (float)g.m * 0.5f;
        float track_h = view_h;
        float thumb_h = track_h * (view_h / total);
        if (thumb_h < ui_px(18)) thumb_h = (float)ui_px(18);
        if (thumb_h > track_h) thumb_h = track_h;
        float thumb_y = y + g.top + (track_h - thumb_h) * (st->scroll / (total - view_h));
        surf_blend(s, (int)track_x, (int)(y + g.top), (int)bar_w, (int)track_h,
                   color_alpha(th->text, 0.08f));
        surf_blend(s, (int)track_x, (int)thumb_y, (int)bar_w, (int)thumb_h,
                   color_alpha(th->accent, 0.75f));
    }

    /* Footer: what the keys do, and where these numbers come from. It lives on
     * the window, not in the list, so it is never scrolled away. */
    if (foot_ok) {
        char legend[160];
        snprintf(legend, sizeof(legend), "wheel / j k / PgUp PgDn scroll - r rereads /proc");
        char nb[160];
        fit_text(fd, legend, nb, sizeof(nb), (float)w - g.m * 2.0f);
        float ly = (float)(y + h) - g.foot_h;
        surf_blend(s, x, (int)ly, w, (int)g.foot_h, color_alpha(th->text, 0.05f));
        font_draw(s, fd, g.lab_x, top_y(fd, base_y(fd, ly, g.foot_h)), nb,
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
        else if (c == 'r') facts_refresh(&st->facts, 1);
    }
}

static void sysinfo_wheel(App *a, int notches) {
    SysInfo *st = a->state;
    if (!st || !notches) return;
    scroll_by(st, -(float)notches * 3.0f * st->row_h);
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
    .resize = sysinfo_resize,
    .wheel = sysinfo_wheel,
    .free = sysinfo_free,
};
