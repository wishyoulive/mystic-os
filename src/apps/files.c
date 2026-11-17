/* ============================================================
 *  Mystic OS - apps/files.c
 *  "Files": a real file manager, not a list of names.
 *
 *  What is on screen, top to bottom:
 *
 *    [Back] [Up] [Home] [Refresh]                      [sort: Name]
 *    / › usr › share › fonts
 *    [icon] name................................  12.4 KB  2024-11-17 10:59
 *    ...
 *    N items      fonts.conf  2.1 KB            free 1.8 GB
 *
 *  Every band is derived from the font that draws in it (see
 *  layout_build()), so the global text scale - applied inside
 *  font_get(), src/font.c:125 - grows the toolbar, the breadcrumb
 *  line, the rows and the footer together. layout_build() is the one
 *  and only source of those numbers: files_draw() paints with it and
 *  files_click() hit-tests with it, so a click always lands on the row
 *  that was painted. Nothing here is a fixed 15 px.
 *
 *  ICONS are drawn geometry, in the same style as the desktop
 *  launcher's icon_folder()/icon_pencil() in src/main.c - no images:
 *  a folder with a tab, a page with a folded corner, and a mark on the
 *  page for the kind of file (a dot for .theme, text rules for .conf,
 *  a play triangle for an executable, a mountain and a sun for an
 *  image). The kind is decided from the real name and st_mode.
 *
 *  The listing is opendir()/readdir()/lstat(), the byte and inode
 *  counts are the real st_size/st_mtime, the free-space figure is
 *  statvfs() on the directory being shown, and a symlink that points
 *  at a directory resolves through stat() so it opens like one. If
 *  opendir() fails the real strerror(errno) is drawn instead of rows.
 *
 *  KEYS   j/k, Up/Down   move          Enter      open (dir descends,
 *         Home/End       first/last               a file says so)
 *         PageUp/PageDn  one page      Backspace  parent
 *         s              cycle sort    r          rescan
 *         b              back in history  h       home ("/")
 *  MOUSE  a click selects a row; a second click on the same row within
 *         450 ms (measured with CLOCK_MONOTONIC) opens it; the toolbar
 *         buttons and every breadcrumb segment are clickable.
 *
 *  AppVT hands an app the pointer position only together with a click,
 *  so the toolbar highlight marks the button under the last pointer
 *  position the app was told about - it is a real position, never a
 *  guess about where the mouse might be now.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 512
#define NAME_LEN    160
#define PATH_LEN    512
#define HIST_MAX    24
#define MAX_CRUMBS  16

/* sort modes, cycled by the toolbar button and by 's' */
enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_TIME = 2, SORT_MODES = 3 };

/* toolbar buttons, in drawing order; TB_SORT sits on the right edge */
enum { TB_BACK, TB_UP, TB_HOME, TB_REFRESH, TB_SORT, TB_COUNT };

/* what mark the icon carries - decided once per entry, at scan time */
enum { IC_FOLDER, IC_FILE, IC_THEME, IC_CONF, IC_EXEC, IC_IMAGE };

typedef struct {
    char          name[NAME_LEN];
    long long     size;
    long long     mtime;
    unsigned      mode;
    unsigned char is_dir;
    unsigned char is_link;
    unsigned char kind;
} Entry;

/* One breadcrumb segment: a clickable x range and the prefix of the
 * path it jumps to. ll == 0 means the root, drawn as "/". */
typedef struct {
    float x0, x1;
    int   target;      /* strlen of the path prefix, i.e. where to cut */
    int   ls, ll;      /* label: f->path[ls .. ls+ll) */
} Crumb;

/* Geometry for one draw, in app-local coordinates (0,0 = top-left of
 * the rect the app was given). Built from the fonts, never stored as a
 * constant, and rebuilt identically by the click handler. */
typedef struct {
    Font *fl;          /* list rows          */
    Font *fc;          /* breadcrumbs        */
    Font *ff;          /* footer             */
    Font *ft;          /* toolbar            */

    int pad, gap, icw, btn;
    int toolbar_h, crumb_h, footer_h;
    int row_h, icon_sz, rows, text_off;
    int list_y, list_bot;
    int btn_x[TB_COUNT], btn_w[TB_COUNT];
    int tb_labels;      /* the four navigation buttons carry their names */
    int tb_sort_label;  /* the sort button carries the current mode */

    int name_x, name_w;
    int size_x, size_col;
    int time_x, time_col;
    int show_time, show_size;
} Layout;

typedef struct {
    char   path[PATH_LEN];
    char   error[128];            /* strerror(errno) from a failed opendir */
    char   status[160];           /* one-line answer to the last action */
    Entry *ent;                   /* MAX_ENTRIES, calloc'd once */
    int    count, sel, scroll;
    int    truncated;             /* more than MAX_ENTRIES entries */
    int    sort;
    int    w, h;
    int    px, py;                /* last pointer position reported to us */
    double last_click_t;          /* CLOCK_MONOTONIC of the last row click */
    int    last_click_idx;
    int    last_click_x, last_click_y;
    char   hist[HIST_MAX][PATH_LEN];
    int    hist_n;
    unsigned long long free_bytes;
    int    has_free;
} Files;

static int iround(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }

static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *sort_label(int mode) {
    if (mode == SORT_SIZE) return "Size";
    if (mode == SORT_TIME) return "Time";
    return "Name";
}

/* ------------------------------------------------------------
 *  Names -> icon kind
 * ------------------------------------------------------------ */
static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m == 0 || n < m) return 0;
    const char *a = s + (n - m);
    for (size_t i = 0; i < m; i++) {
        char c = a[i], d = suffix[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
        if (c != d) return 0;
    }
    return 1;
}

/* Content first (theme/conf/image), then the execute bit, then plain. */
static int kind_of(const Entry *e) {
    if (e->is_dir) return IC_FOLDER;
    if (ends_with_ci(e->name, ".theme")) return IC_THEME;
    if (ends_with_ci(e->name, ".conf"))  return IC_CONF;
    if (ends_with_ci(e->name, ".png") || ends_with_ci(e->name, ".jpg") ||
        ends_with_ci(e->name, ".jpeg") || ends_with_ci(e->name, ".ppm") ||
        ends_with_ci(e->name, ".bmp") || ends_with_ci(e->name, ".gif"))
        return IC_IMAGE;
    if ((e->mode & 0111u) != 0) return IC_EXEC;
    return IC_FILE;
}

/* ------------------------------------------------------------
 *  Formatting
 * ------------------------------------------------------------ */
static void human_size(long long size, char *out, size_t n) {
    if (size < 0) { snprintf(out, n, "--"); return; }
    if (size < 1024) snprintf(out, n, "%lld B", size);
    else if (size < 1024LL * 1024) snprintf(out, n, "%.0f KB", (double)size / 1024.0);
    else if (size < 1024LL * 1024 * 1024)
        snprintf(out, n, "%.1f MB", (double)size / (1024.0 * 1024.0));
    else snprintf(out, n, "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
}

/* "2024-11-17 10:59" - localtime, so the stamp matches what the user
 * sees in any other program on the machine. */
static void fmt_mtime(long long mtime, char *out, size_t n) {
    time_t t = (time_t)mtime;
    struct tm tmv;
    if (!localtime_r(&t, &tmv) || strftime(out, n, "%Y-%m-%d %H:%M", &tmv) == 0)
        snprintf(out, n, "--");
}

/* draw inside maxw pixels, ending in ".." when it has to be cut */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 4.0f) return;
    char buf[PATH_LEN + 8];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 3) len = sizeof(buf) - 3;
    memcpy(buf, text, len);
    buf[len] = 0;
    if (font_width(f, buf) <= maxw) { font_draw(s, f, x, y, buf, color); return; }
    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0 && font_width(f, buf) > budget) buf[--cut] = 0;
    if (cut == 0) return;
    memcpy(buf + cut, "..", 3);
    font_draw(s, f, x, y, buf, color);
}

/* ------------------------------------------------------------
 *  Layout - the single source of every vertical and horizontal number
 * ------------------------------------------------------------ */
static int layout_build(Layout *L, int w, int h) {
    memset(L, 0, sizeof(*L));
    if (w < 1 || h < 1) return 0;

    L->fl = font_get(FONT_REGULAR, 11);
    L->fc = font_get(FONT_MEDIUM, 11);
    L->ff = font_get(FONT_REGULAR, 10);
    L->ft = font_get(FONT_MEDIUM, 10);
    if (!L->fl || !L->fc || !L->ff || !L->ft) return 0;

    int fh_l = iround(font_height(L->fl)); if (fh_l < 1) fh_l = 1;
    int fh_c = iround(font_height(L->fc)); if (fh_c < 1) fh_c = 1;
    int fh_f = iround(font_height(L->ff)); if (fh_f < 1) fh_f = 1;
    int fh_t = iround(font_height(L->ft)); if (fh_t < 1) fh_t = 1;

    L->pad = ui_px(6);
    L->gap = ui_px(4);

    /* An icon button is one glyph line plus padding on every side. */
    L->icw = fh_t + ui_px(2);
    if (L->icw < ui_px(13)) L->icw = ui_px(13);
    L->btn = L->icw + 2 * L->pad;

    /* A row is the glyph box plus leading; the icon lives in the row, so a
     * row can never be shorter than the icon needs, and both grow with the
     * text scale because font_height() already has it folded in. */
    L->row_h = fh_l + ui_px(9);
    if (L->row_h < ui_px(22)) L->row_h = ui_px(22);
    /* The icon is 20x20 at 100% text scale and ui_px() grows it with the
     * scale, exactly like the row does; it shrinks only if the row it has
     * to live in is shorter than that. */
    L->icon_sz = ui_px(20);
    if (L->icon_sz > L->row_h - ui_px(4)) L->icon_sz = L->row_h - ui_px(4);
    if (L->icon_sz < ui_px(12)) L->icon_sz = ui_px(12);
    L->text_off = (L->row_h - fh_l) / 2;
    if (L->text_off < 0) L->text_off = 0;

    /* Vertical bands. A band is only kept if it and one whole row still fit
     * in the window: the status line goes first, then the breadcrumbs, so a
     * very short window loses decoration instead of painting outside its
     * rect. */
    L->toolbar_h = L->btn + L->pad;
    L->crumb_h = fh_c + L->pad;
    L->footer_h = fh_f + L->pad;
    if (L->toolbar_h + L->crumb_h + L->footer_h + L->row_h > h) L->footer_h = 0;
    if (L->toolbar_h + L->crumb_h + L->row_h > h) L->crumb_h = 0;
    if (L->toolbar_h + L->row_h > h) L->toolbar_h = h;

    L->list_y = L->toolbar_h + L->crumb_h + L->pad / 2;
    L->list_bot = h - L->footer_h;
    if (L->list_bot < L->list_y) L->list_bot = L->list_y;
    int avail = L->list_bot - L->list_y;
    L->rows = avail > 0 ? avail / L->row_h : 0;
    if (L->rows > MAX_ENTRIES) L->rows = MAX_ENTRIES;

    /* Toolbar buttons: icon + label when the width allows it, icons only
     * when it does not. Both layouts are tried with real font widths. */
    {
        static const char *LABEL[TB_COUNT] = { "Back", "Up", "Home", "Refresh", "Name" };
        int nat[TB_COUNT];
        /* the sort button must hold whichever mode label it will show, so
         * the box is measured against the widest of the three */
        int sort_w = 0;
        for (int m = 0; m < SORT_MODES; m++) {
            int sw = iround(font_width(L->ft, sort_label(m)));
            if (sw > sort_w) sort_w = sw;
        }
        long total = L->pad;
        for (int i = 0; i < TB_COUNT; i++) {
            const char *lab = (i == TB_SORT) ? "Refresh" : LABEL[i];  /* widest */
            nat[i] = L->pad + L->icw + L->pad + iround(font_width(L->ft, lab)) + L->pad;
            total += nat[i] + L->gap;
        }
        L->tb_labels = (total <= (long)w);
        int bx = L->pad;
        for (int i = 0; i < TB_COUNT; i++) {
            if (i == TB_SORT) continue;
            int bw = L->tb_labels ? nat[i] : L->btn;
            /* a button that would not fit is not drawn and not clickable */
            if (bx + bw > w - L->pad) { L->btn_w[i] = 0; continue; }
            L->btn_x[i] = bx;
            L->btn_w[i] = bw;
            bx += bw + L->gap;
        }
        {
            /* The mode is named at every width where the button fits at
             * all - an icon alone would hide which sort is active. */
            int sw = L->pad + L->icw + L->pad + sort_w + L->pad;
            L->tb_sort_label = 1;
            if (sw > w - 2 * L->pad) {
                sw = L->btn;
                L->tb_sort_label = 0;
            }
            L->btn_w[TB_SORT] = sw;
            L->btn_x[TB_SORT] = w - L->pad - sw;
            /* the sort button gives way rather than overlapping the group */
            if (L->btn_x[TB_SORT] < bx) {
                L->btn_x[TB_SORT] = bx;
                if (bx + sw > w - L->pad) L->btn_w[TB_SORT] = 0;
            }
            if (L->btn_x[TB_SORT] < 0) L->btn_w[TB_SORT] = 0;
        }
    }

    /* Columns. Each is measured from a fixed sample string - the widest
     * string that column can ever hold - so the columns never jitter and
     * never overflow into each other as the contents change. */
    {
        int right = w - L->pad;
        L->size_col = iround(font_width(L->fl, "0000.0 MB"));
        L->time_col = iround(font_width(L->fl, "0000-00-00 00:00"));
        L->name_x = L->pad + L->icon_sz + L->pad;
        L->size_x = right - L->size_col;
        L->time_x = right - L->time_col;

        /* the modified-time column is dropped below the nominal 380 px
         * design width - expressed through ui_px() so the rule holds at
         * every text scale - and whenever it would squeeze the names */
        L->show_time = (w >= ui_px(380));
        if (L->show_time) L->size_x = L->time_x - L->pad - L->size_col;
        L->name_w = L->size_x - L->gap - L->name_x;
        if (L->show_time && L->name_w < ui_px(90)) {
            L->show_time = 0;
            L->size_x = right - L->size_col;
            L->name_w = L->size_x - L->gap - L->name_x;
        }
        /* in a window too narrow for a size column the names take it all */
        L->show_size = (L->name_w >= ui_px(90));
        if (!L->show_size) L->name_w = right - L->name_x;
        if (L->name_w < ui_px(16)) L->name_w = ui_px(16);
    }
    return 1;
}

/* ------------------------------------------------------------
 *  Breadcrumbs: built from the real path, laid out to fit w
 * ------------------------------------------------------------ */
static int crumb_build(const Files *f, const Layout *L, int w, Crumb *out, int *dots) {
    Crumb all[MAX_CRUMBS];
    int n = 0;

    *dots = 0;
    if (f->path[0] != '/') return 0;

    all[n].target = 1; all[n].ls = 0; all[n].ll = 0; n++;      /* "/" */
    int i = 1;
    while (f->path[i] && n < MAX_CRUMBS) {
        int s = i;
        while (f->path[i] && f->path[i] != '/') i++;
        all[n].target = i;
        all[n].ls = s;
        all[n].ll = i - s;
        n++;
        if (f->path[i] == '/') i++;
    }

    float chev = (float)ui_px(9);
    float wid[MAX_CRUMBS];
    float total = 0.0f;
    for (int k = 0; k < n; k++) {
        char lab[NAME_LEN];
        int len = all[k].ll;
        if (len >= (int)sizeof(lab)) len = (int)sizeof(lab) - 1;
        if (len > 0) memcpy(lab, f->path + all[k].ls, (size_t)len);
        lab[len] = 0;
        wid[k] = font_width(L->fc, len ? lab : "/");
        total += wid[k];
        if (k) total += chev;
    }

    float avail = (float)(w - 2 * L->pad);
    int first = 0;
    float dots_w = font_width(L->fc, "...") + (float)L->gap;
    while (total > avail && first < n - 1) {
        total -= wid[first];
        if (first + 1 < n) total -= chev;
        first++;
        *dots = 1;
    }
    if (total + (*dots ? dots_w : 0.0f) > avail) { *dots = 0; }

    float x = (float)L->pad + (*dots ? dots_w : 0.0f);
    int m = 0;
    for (int k = first; k < n; k++) {
        out[m].x0 = x;
        out[m].x1 = x + wid[k];
        out[m].target = all[k].target;
        out[m].ls = all[k].ls;
        out[m].ll = all[k].ll;
        x += wid[k];
        if (k + 1 < n) x += chev;
        m++;
    }
    return m;
}

/* ------------------------------------------------------------
 *  Reading the directory
 * ------------------------------------------------------------ */
static int cmp_entry(const Files *f, const Entry *a, const Entry *b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;      /* dirs first */
    if (f->sort == SORT_SIZE && a->size != b->size) return a->size > b->size ? -1 : 1;
    if (f->sort == SORT_TIME && a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void sort_entries(Files *f) {
    for (int i = 1; i < f->count; i++) {
        Entry key = f->ent[i];
        int j = i - 1;
        while (j >= 0 && cmp_entry(f, &key, &f->ent[j]) < 0) {
            f->ent[j + 1] = f->ent[j];
            j--;
        }
        f->ent[j + 1] = key;
    }
}

static void free_space_scan(Files *f) {
    struct statvfs vfs;
    f->has_free = 0;
    f->free_bytes = 0;
    if (statvfs(f->path, &vfs) != 0) return;
    unsigned long long unit = vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                                           : (unsigned long long)vfs.f_bsize;
    f->free_bytes = unit * (unsigned long long)vfs.f_bavail;
    f->has_free = 1;
}

static void scan(Files *f) {
    f->count = 0;
    f->truncated = 0;
    f->error[0] = 0;

    /* Free space belongs to the directory, not to a successful listing:
     * ask even when the listing is about to fail, so the footer never
     * shows a stale figure from the folder we came from. */
    free_space_scan(f);

    DIR *d = opendir(f->path);
    if (!d) {
        snprintf(f->error, sizeof(f->error), "%s", strerror(errno));
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (strlen(de->d_name) >= NAME_LEN) continue;   /* would not round-trip */
        if (f->count >= MAX_ENTRIES) { f->truncated = 1; break; }

        char full[PATH_LEN + NAME_LEN + 2];
        int n = snprintf(full, sizeof(full), "%s/%s", f->path, de->d_name);
        if (n < 0 || n >= (int)sizeof(full)) continue;   /* never a truncated lookup */

        struct stat st;
        if (lstat(full, &st) < 0) continue;              /* vanished: skip it */

        int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        int is_link = S_ISLNK(st.st_mode) ? 1 : 0;
        if (is_link) {
            struct stat t;
            if (stat(full, &t) == 0 && S_ISDIR(t.st_mode)) is_dir = 1;
        }

        Entry *e = &f->ent[f->count];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir  = (unsigned char)is_dir;
        e->is_link = (unsigned char)is_link;
        e->size    = (long long)st.st_size;
        e->mtime   = (long long)st.st_mtime;
        e->mode    = (unsigned)st.st_mode;
        e->kind    = (unsigned char)kind_of(e);
        f->count++;
    }
    closedir(d);
    sort_entries(f);
}

/* ------------------------------------------------------------
 *  Navigation
 * ------------------------------------------------------------ */
static void hist_push(Files *f, const char *p) {
    if (f->hist_n > 0 && !strcmp(f->hist[f->hist_n - 1], p)) return;
    if (f->hist_n == HIST_MAX) {
        memmove(f->hist[0], f->hist[1], sizeof(f->hist[0]) * (HIST_MAX - 1));
        f->hist_n = HIST_MAX - 1;
    }
    snprintf(f->hist[f->hist_n], PATH_LEN, "%s", p);
    f->hist_n++;
}

static int hist_pop(Files *f, char *out) {
    if (f->hist_n <= 0) return 0;
    f->hist_n--;
    snprintf(out, PATH_LEN, "%s", f->hist[f->hist_n]);
    return 1;
}

/* Load a directory. "to" is copied first so callers may pass a slice
 * of f->path; trailing slashes are trimmed, and the result is verified
 * to fit before it becomes the current path. */
static void nav_to(Files *f, const char *to, int push) {
    char next[PATH_LEN];
    int n = snprintf(next, sizeof(next), "%s", to);
    if (n < 0 || n >= (int)sizeof(next)) {
        snprintf(f->status, sizeof(f->status), "path too long");
        return;
    }
    size_t len = strlen(next);
    while (len > 1 && next[len - 1] == '/') next[--len] = 0;
    if (len == 0) snprintf(next, sizeof(next), "/");

    if (push && strcmp(next, f->path) != 0) hist_push(f, f->path);
    snprintf(f->path, sizeof(f->path), "%s", next);
    f->status[0] = 0;
    f->sel = f->scroll = 0;
    f->last_click_idx = -1;
    scan(f);
}

static void nav_up(Files *f) {
    if (!strcmp(f->path, "/")) { snprintf(f->status, sizeof(f->status), "already at /"); return; }
    char parent[PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", f->path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) snprintf(parent, sizeof(parent), "/");
    else *slash = 0;
    nav_to(f, parent, 1);
}

static void nav_back(Files *f) {
    char prev[PATH_LEN];
    if (!hist_pop(f, prev)) { snprintf(f->status, sizeof(f->status), "no history"); return; }
    nav_to(f, prev, 0);
}

/* Rescan in place, keeping the highlighted name selected when it is
 * still there - a refresh should not move the cursor. */
static void rescan(Files *f) {
    char want[NAME_LEN];
    want[0] = 0;
    if (f->sel >= 0 && f->sel < f->count)
        snprintf(want, sizeof(want), "%s", f->ent[f->sel].name);
    scan(f);
    f->sel = 0;
    f->scroll = 0;
    if (want[0]) {
        for (int i = 0; i < f->count; i++)
            if (!strcmp(f->ent[i].name, want)) { f->sel = i; break; }
    }
    f->status[0] = 0;
}

static void cycle_sort(Files *f) {
    char keep[NAME_LEN];
    keep[0] = 0;
    if (f->sel >= 0 && f->sel < f->count)
        snprintf(keep, sizeof(keep), "%s", f->ent[f->sel].name);
    f->sort = (f->sort + 1) % SORT_MODES;
    sort_entries(f);
    f->sel = 0;
    f->scroll = 0;
    if (keep[0])
        for (int i = 0; i < f->count; i++)
            if (!strcmp(f->ent[i].name, keep)) { f->sel = i; break; }
    snprintf(f->status, sizeof(f->status), "sorted by %s", sort_label(f->sort));
}

static void open_selected(Files *f) {
    if (f->sel < 0 || f->sel >= f->count) {
        snprintf(f->status, sizeof(f->status), "nothing selected");
        return;
    }
    Entry *e = &f->ent[f->sel];
    if (!e->is_dir) {
        /* Files does not launch programs yet; say so instead of failing
         * silently or pretending to open something. */
        snprintf(f->status, sizeof(f->status), "cannot open files yet: %s", e->name);
        return;
    }
    char next[PATH_LEN];
    int n = !strcmp(f->path, "/")
          ? snprintf(next, sizeof(next), "/%s", e->name)
          : snprintf(next, sizeof(next), "%s/%s", f->path, e->name);
    if (n < 0 || n >= (int)sizeof(next)) {
        snprintf(f->status, sizeof(f->status), "path too long");
        return;
    }
    nav_to(f, next, 1);
}

static void move_sel(Files *f, int delta) {
    if (f->count <= 0) return;
    int s = f->sel + delta;
    if (s < 0) s = 0;
    if (s > f->count - 1) s = f->count - 1;
    f->sel = s;
    f->status[0] = 0;
}

/* ------------------------------------------------------------
 *  Icons - drawn geometry, the launcher's style (src/main.c)
 * ------------------------------------------------------------ */
static void draw_chevron(Surface *s, float x, float y, float sz, u32 c) {
    gfx_line(s, x, y + sz * 0.18f, x + sz * 0.55f, y + sz * 0.5f, sz * 0.16f, c);
    gfx_line(s, x + sz * 0.55f, y + sz * 0.5f, x, y + sz * 0.82f, sz * 0.16f, c);
}

static void tb_icon(Surface *s, int kind, float x, float y, float sz, u32 c) {
    if (kind == TB_BACK) {
        gfx_line(s, x + sz * 0.22f, y + sz * 0.5f, x + sz * 0.62f, y + sz * 0.5f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.24f, y + sz * 0.5f,
                         x + sz * 0.62f, y + sz * 0.24f,
                         x + sz * 0.62f, y + sz * 0.76f };
        gfx_polygon(s, pts, 3, c);
    } else if (kind == TB_UP) {
        gfx_line(s, x + sz * 0.5f, y + sz * 0.44f, x + sz * 0.5f, y + sz * 0.80f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.5f, y + sz * 0.14f,
                         x + sz * 0.76f, y + sz * 0.50f,
                         x + sz * 0.24f, y + sz * 0.50f };
        gfx_polygon(s, pts, 3, c);
    } else if (kind == TB_HOME) {
        float roof[6] = { x + sz * 0.5f, y + sz * 0.12f,
                          x + sz * 0.90f, y + sz * 0.52f,
                          x + sz * 0.10f, y + sz * 0.52f };
        gfx_polygon(s, roof, 3, c);
        gfx_rrect_aa(s, x + sz * 0.26f, y + sz * 0.48f, sz * 0.48f, sz * 0.36f,
                     sz * 0.05f, c, 0, 0);
    } else if (kind == TB_REFRESH) {
        float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.32f;
        gfx_rrect_aa(s, cx - r, cy - r, r * 2.0f, r * 2.0f, r, 0, c, sz * 0.11f);
        float head[6] = { cx + r * 0.10f, cy - r * 1.30f,
                          cx + r * 1.30f, cy - r * 0.55f,
                          cx + r * 0.10f, cy - r * 0.25f };
        gfx_polygon(s, head, 3, c);
    } else { /* TB_SORT: three rules, longest first */
        for (int i = 0; i < 3; i++) {
            float lw = sz * (0.66f - i * 0.18f);
            gfx_rrect(s, x + sz * 0.16f, y + sz * (0.26f + i * 0.22f), lw, sz * 0.10f,
                      sz * 0.05f, c);
        }
    }
}

/* 20x20 nominal box (scaled with the row) holding either a folder or a
 * page with a folded corner and a mark for the kind of file. */
static void draw_entry_icon(Surface *s, const Entry *e, float x, float y, float sz,
                            int sel, const Theme *th) {
    if (sz < 6.0f) return;

    if (e->kind == IC_FOLDER) {
        u32 fg = color_alpha(th->accent, sel ? 0.98f : 0.80f);
        float w = sz * 0.90f, h = sz * 0.60f;
        float bx = x + (sz - w) * 0.5f, by = y + (sz - h) * 0.5f + sz * 0.07f;
        gfx_rrect(s, bx, by - sz * 0.15f, w * 0.46f, sz * 0.18f, sz * 0.04f, fg);
        gfx_rrect_aa(s, bx, by, w, h, sz * 0.10f, fg, 0, 0);
        return;
    }

    float w = sz * 0.62f, h = sz * 0.82f, fold = w * 0.42f;
    float px = x + (sz - w) * 0.5f, py = y + (sz - h) * 0.5f;
    u32 body = color_alpha(th->text, sel ? 0.94f : 0.72f);
    u32 flap = color_alpha(th->text, sel ? 0.52f : 0.38f);

    float page[10] = { px, py, px + w - fold, py, px + w, py + fold,
                       px + w, py + h, px, py + h };
    gfx_polygon(s, page, 5, body);
    float tri[6] = { px + w - fold, py, px + w, py + fold, px + w - fold, py + fold };
    gfx_polygon(s, tri, 3, flap);

    u32 mark = color_alpha(th->accent, sel ? 0.98f : 0.85f);
    float my = py + fold + (h - fold) * 0.16f;
    float mh = (h - fold) * 0.64f;
    switch (e->kind) {
    case IC_THEME: {
        float r = mh * 0.30f, cx = px + w * 0.5f, cy = my + mh * 0.5f;
        gfx_rrect(s, cx - r, cy - r, r * 2.0f, r * 2.0f, r, mark);
        break;
    }
    case IC_CONF:
        for (int i = 0; i < 3; i++) {
            float lw = w * (i == 2 ? 0.36f : 0.60f);
            gfx_rrect(s, px + w * 0.20f, my + (float)i * mh * 0.32f, lw,
                      mh * 0.15f, mh * 0.075f, color_alpha(th->text_dim, 0.85f));
        }
        break;
    case IC_EXEC: {
        float tp[6] = { px + w * 0.30f, my,
                        px + w * 0.30f, my + mh,
                        px + w * 0.78f, my + mh * 0.5f };
        gfx_polygon(s, tp, 3, mark);
        break;
    }
    case IC_IMAGE: {
        float mt[6] = { px + w * 0.20f, my + mh,
                        px + w * 0.50f, my + mh * 0.24f,
                        px + w * 0.80f, my + mh };
        gfx_polygon(s, mt, 3, mark);
        float r = mh * 0.16f;
        gfx_rrect(s, px + w * 0.60f, my + mh * 0.06f, r * 2.0f, r * 2.0f, r,
                  color_alpha(th->accent, 0.70f));
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------
 *  Drawing
 * ------------------------------------------------------------ */
static void draw_toolbar(Files *f, Surface *s, int x, int y, int w,
                         const Layout *L, const Theme *th) {
    if (w <= 0 || L->toolbar_h <= 0) return;

    surf_blend(s, x, y, w, L->toolbar_h, th->panel);
    if (L->toolbar_h > 1) surf_blend(s, x, y + L->toolbar_h - 1, w, 1, th->panel_border);

    int by = (L->toolbar_h - L->btn) / 2;
    if (by < 0) by = 0;

    for (int i = 0; i < TB_COUNT; i++) {
        int bw = L->btn_w[i];
        if (bw <= 0) continue;
        int bx = L->btn_x[i];
        int enabled = 1;
        if (i == TB_BACK && f->hist_n <= 0) enabled = 0;
        if (i == TB_UP && !strcmp(f->path, "/")) enabled = 0;
        int hov = enabled && f->px >= bx && f->px < bx + bw &&
                  f->py >= by && f->py < by + L->btn;

        u32 fill = hov ? color_alpha(th->accent, 0.26f)
                       : color_alpha(th->text, 0.07f);
        if (!enabled) fill = color_alpha(th->text, 0.025f);
        u32 border = hov ? color_alpha(th->accent, 0.55f)
                         : color_alpha(th->panel_border, 0.85f);
        gfx_rrect_aa(s, (float)(x + bx), (float)(y + by), (float)bw,
                     (float)L->btn, (float)ui_px(5), fill, border, 1.0f);

        u32 fg = enabled ? (hov ? th->text : color_alpha(th->text, 0.85f))
                         : color_alpha(th->text, 0.25f);
        float iy = (float)(y + by) + (float)(L->btn - L->icw) * 0.5f;
        float ix = (float)(x + bx) + (float)L->pad;
        tb_icon(s, i, ix, iy, (float)L->icw, fg);

        if (i == TB_SORT ? L->tb_sort_label : L->tb_labels) {
            const char *lab = (i == TB_SORT) ? sort_label(f->sort) : NULL;
            if (!lab) {
                static const char *NAMES[TB_COUNT] = { "Back", "Up", "Home", "Refresh", "Name" };
                lab = NAMES[i];
            }
            float ly = (float)(y + by) + ((float)L->btn - font_height(L->ft)) * 0.5f;
            float lx = ix + (float)L->icw + (float)L->pad;
            float maxw = (float)(bx + bw) - (lx - (float)x) - (float)L->pad;
            draw_clipped(s, L->ft, lx, ly, maxw, lab, fg);
        }
    }
}

static void draw_crumbs(Files *f, Surface *s, int x, int y, int w,
                        const Layout *L, const Theme *th) {
    if (L->crumb_h <= 0) return;
    int cy = y + L->toolbar_h;
    Crumb cr[MAX_CRUMBS];
    int dots = 0;
    int n = crumb_build(f, L, w, cr, &dots);

    float ty = (float)cy + ((float)L->crumb_h - font_height(L->fc)) * 0.5f;
    if (ty < (float)cy) ty = (float)cy;

    if (dots)
        draw_clipped(s, L->fc, (float)(x + L->pad), ty,
                     (float)(w - 2 * L->pad), "...", color_alpha(th->text, 0.45f));

    for (int i = 0; i < n; i++) {
        char lab[NAME_LEN];
        int len = cr[i].ll;
        if (len >= (int)sizeof(lab)) len = (int)sizeof(lab) - 1;
        if (len > 0) memcpy(lab, f->path + cr[i].ls, (size_t)len);
        lab[len] = 0;
        const char *text = len ? lab : "/";

        int hov = f->py >= cy && f->py < cy + L->crumb_h &&
                  (float)f->px >= cr[i].x0 - 2.0f && (float)f->px < cr[i].x1 + 2.0f;
        int last = (i == n - 1);

        if (hov)
            gfx_rrect(s, (float)x + cr[i].x0 - 3.0f, (float)cy + 2.0f,
                      (cr[i].x1 - cr[i].x0) + 6.0f, (float)L->crumb_h - 4.0f,
                      (float)ui_px(4), color_alpha(th->accent, 0.18f));

        u32 col = last ? th->text : (hov ? th->text : color_alpha(th->text, 0.62f));
        /* never paint past the right padding, whatever the path is */
        float room = (float)(x + w - L->pad) - ((float)x + cr[i].x0);
        if (room > cr[i].x1 - cr[i].x0) room = cr[i].x1 - cr[i].x0;
        draw_clipped(s, L->fc, (float)x + cr[i].x0, ty, room, text, col);

        if (i + 1 < n)
            draw_chevron(s, (float)x + cr[i].x1 + (float)ui_px(2),
                         (float)cy + ((float)L->crumb_h - (float)ui_px(9)) * 0.5f,
                         (float)ui_px(9), color_alpha(th->text, 0.30f));
    }
    if (L->crumb_h > 0)
        surf_blend(s, x + L->pad, cy + L->crumb_h - 1, w - 2 * L->pad, 1,
                   color_alpha(th->panel_border, 0.6f));
}

static void draw_list(Files *f, Surface *s, int x, int y, int w,
                      const Layout *L, const Theme *th) {
    float top = (float)(y + L->list_y);
    float availh = (float)(L->list_bot - L->list_y);

    if (f->error[0]) {
        draw_clipped(s, L->fl, (float)(x + L->pad), top + (float)L->text_off,
                     (float)(w - 2 * L->pad), f->error, th->accent);
        return;
    }
    if (f->count == 0) {
        draw_clipped(s, L->fl, (float)(x + L->pad), top + (float)L->text_off,
                     (float)(w - 2 * L->pad), "this folder is empty", th->text_dim);
        return;
    }

    /* keep the selection inside the scroll window */
    if (f->sel < 0) f->sel = 0;
    if (f->sel > f->count - 1) f->sel = f->count - 1;
    if (L->rows > 0) {
        if (f->sel < f->scroll) f->scroll = f->sel;
        if (f->sel >= f->scroll + L->rows) f->scroll = f->sel - L->rows + 1;
        int maxs = f->count - L->rows;
        if (maxs < 0) maxs = 0;
        if (f->scroll > maxs) f->scroll = maxs;
    }
    if (f->scroll < 0) f->scroll = 0;

    for (int i = 0; i < L->rows; i++) {
        int idx = f->scroll + i;
        if (idx >= f->count) break;
        float ry = top + (float)i * (float)L->row_h;
        if (ry + (float)L->row_h > top + availh + 0.5f) break;

        Entry *e = &f->ent[idx];
        int sel = (idx == f->sel);

        if (sel)
            gfx_rrect_aa(s, (float)x + (float)(L->pad / 2), ry + 1.0f,
                         (float)(w - L->pad), (float)(L->row_h - 2),
                         (float)ui_px(4),
                         color_alpha(th->accent, 0.20f), 0, 0);

        float ty = ry + (float)L->text_off;
        draw_entry_icon(s, e, (float)(x + L->pad),
                        ry + (float)(L->row_h - L->icon_sz) * 0.5f,
                        (float)L->icon_sz, sel, th);

        /* name, with a trailing '/' on directories */
        char name[NAME_LEN + 2];
        snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
        draw_clipped(s, L->fl, (float)(x + L->name_x), ty, (float)L->name_w, name,
                     sel ? th->text : color_alpha(th->text, 0.72f));

        if (L->show_size && !e->is_dir) {
            char sz[24];
            human_size(e->size, sz, sizeof(sz));
            float sw = font_width(L->fl, sz);
            font_draw(s, L->fl,
                      (float)(x + L->size_x) + (float)L->size_col - sw, ty, sz,
                      color_alpha(th->text, sel ? 0.85f : 0.50f));
        }

        if (L->show_time) {
            char ts[32];
            fmt_mtime(e->mtime, ts, sizeof(ts));
            float tw = font_width(L->fl, ts);
            font_draw(s, L->fl,
                      (float)(x + L->time_x) + (float)L->time_col - tw, ty, ts,
                      color_alpha(th->text, sel ? 0.62f : 0.36f));
        }
    }
}

static void draw_footer(Files *f, Surface *s, int x, int y, int w,
                        const Layout *L, const Theme *th) {
    if (L->footer_h <= 0) return;
    int fy = y + L->list_bot;
    surf_blend(s, x, fy, w, L->footer_h, th->panel);
    if (L->footer_h > 1) surf_blend(s, x, fy, w, 1, th->panel_border);

    float ty = (float)fy + ((float)L->footer_h - font_height(L->ff)) * 0.5f;

    char left[48];
    snprintf(left, sizeof(left), "%d%s item%s", f->count, f->truncated ? "+" : "",
             f->count == 1 && !f->truncated ? "" : "s");
    font_draw(s, L->ff, (float)(x + L->pad), ty, left, color_alpha(th->text, 0.60f));
    float lw = font_width(L->ff, left);

    float rw = 0.0f;
    if (f->has_free) {
        char right[48], fs[32];
        human_size((long long)f->free_bytes, fs, sizeof(fs));
        snprintf(right, sizeof(right), "free %s", fs);
        rw = font_width(L->ff, right);
        font_draw(s, L->ff, (float)(x + w - L->pad) - rw, ty, right,
                  color_alpha(th->text, 0.50f));
    }

    float minfo = (float)(x + L->pad) + lw + (float)ui_px(14);
    float maxw = (float)(x + w - L->pad) - rw - (float)ui_px(8) - minfo;
    if (maxw < (float)ui_px(20)) return;

    char info[NAME_LEN + 48];
    u32 col = color_alpha(th->text, 0.55f);
    if (f->status[0]) {
        snprintf(info, sizeof(info), "%s", f->status);
        col = th->accent;
    } else if (f->sel >= 0 && f->sel < f->count) {
        Entry *e = &f->ent[f->sel];
        if (e->is_dir)
            snprintf(info, sizeof(info), "%s/  folder", e->name);
        else {
            char sz[24];
            human_size(e->size, sz, sizeof(sz));
            snprintf(info, sizeof(info), "%s  %s", e->name, sz);
        }
    } else {
        info[0] = 0;
    }
    draw_clipped(s, L->ff, minfo, ty, maxw, info, col);
}

static void files_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Files *f = a->state;
    if (!f || !th) return;
    f->w = w;
    f->h = h;

    Layout L;
    if (!layout_build(&L, w, h)) return;     /* no fonts yet: draw nothing */

    draw_toolbar(f, s, x, y, w, &L, th);
    draw_crumbs(f, s, x, y, w, &L, th);
    draw_list(f, s, x, y, w, &L, th);
    draw_footer(f, s, x, y, w, &L, th);
}

/* ------------------------------------------------------------
 *  Input
 * ------------------------------------------------------------ */
static void toolbar_action(Files *f, int which) {
    switch (which) {
        case TB_BACK:    nav_back(f); break;
        case TB_UP:      nav_up(f); break;
        case TB_HOME:    nav_to(f, "/", 1); break;
        case TB_REFRESH: rescan(f); break;
        case TB_SORT:    cycle_sort(f); break;
        default: break;
    }
}

static void files_click(App *a, int cx, int cy, int buttons) {
    Files *f = a->state;
    (void)buttons;
    if (!f) return;
    f->px = cx;
    f->py = cy;

    Layout L;
    if (!layout_build(&L, f->w, f->h)) return;

    /* toolbar */
    int by = (L.toolbar_h - L.btn) / 2;
    if (by < 0) by = 0;
    if (cy >= by && cy < by + L.btn) {
        for (int i = 0; i < TB_COUNT; i++) {
            if (L.btn_w[i] <= 0) continue;
            if (cx >= L.btn_x[i] && cx < L.btn_x[i] + L.btn_w[i]) {
                toolbar_action(f, i);
                return;
            }
        }
    }

    /* breadcrumbs */
    int cy0 = L.toolbar_h;
    if (cy >= cy0 && cy < cy0 + L.crumb_h) {
        Crumb cr[MAX_CRUMBS];
        int dots = 0;
        int n = crumb_build(f, &L, f->w, cr, &dots);
        for (int i = 0; i < n; i++) {
            if ((float)cx < cr[i].x0 - 2.0f || (float)cx >= cr[i].x1 + 2.0f) continue;
            char target[PATH_LEN];
            int t = cr[i].target;
            if (t < 1) t = 1;
            if (t >= (int)sizeof(target)) t = (int)sizeof(target) - 1;
            memcpy(target, f->path, (size_t)t);
            target[t] = 0;
            /* never hand nav_to() a pointer into f->path it will overwrite */
            nav_to(f, target, 1);
            return;
        }
        return;
    }

    /* rows */
    if (cy < L.list_y || cy >= L.list_bot) return;
    if (L.row_h <= 0) return;
    int row = (cy - L.list_y) / L.row_h;
    if (row < 0 || row >= L.rows) return;
    int idx = f->scroll + row;
    if (idx < 0 || idx >= f->count) return;

    double t = mono_now();
    int twice = (idx == f->last_click_idx) &&
                (t - f->last_click_t) <= 0.45 &&
                (cx - f->last_click_x) > -8 && (cx - f->last_click_x) < 8 &&
                (cy - f->last_click_y) > -8 && (cy - f->last_click_y) < 8;

    f->sel = idx;
    f->status[0] = 0;
    if (twice) {
        f->last_click_idx = -1;
        open_selected(f);
    } else {
        f->last_click_idx = idx;
        f->last_click_t = t;
        f->last_click_x = cx;
        f->last_click_y = cy;
    }
}

static void files_key(App *a, const char *bytes, int n) {
    Files *f = a->state;
    if (!f || !bytes || n <= 0) return;

    Layout L;
    int have = layout_build(&L, f->w, f->h);
    int page = (have && L.rows > 1) ? L.rows - 1 : 1;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            /* CSI: digits then a final byte */
            int j = i + 2, arg = 0, have_arg = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') {
                arg = arg * 10 + (bytes[j] - '0');
                have_arg = 1;
                j++;
            }
            if (j >= n) break;                 /* truncated run: do not guess */
            char fin = bytes[j];
            if (fin == 'A')      move_sel(f, -1);
            else if (fin == 'B') move_sel(f, 1);
            else if (fin == 'C') open_selected(f);
            else if (fin == 'D') nav_up(f);
            else if (fin == 'H') { f->sel = 0; f->status[0] = 0; }
            else if (fin == 'F') { if (f->count) f->sel = f->count - 1; f->status[0] = 0; }
            else if (fin == '~' && have_arg) {
                if (arg == 5) move_sel(f, -page);
                else if (arg == 6) move_sel(f, page);
                else if (arg == 1 || arg == 7) { f->sel = 0; f->status[0] = 0; }
                else if (arg == 4 || arg == 8) {
                    if (f->count) f->sel = f->count - 1;
                    f->status[0] = 0;
                }
            }
            i = j;
            continue;
        }
        if (c == 0x1B) { i++; continue; }      /* lone escape: the desktop owns it */

        switch (c) {
            case 'j': move_sel(f, 1); break;
            case 'k': move_sel(f, -1); break;
            case '\n':
            case '\r': open_selected(f); break;
            case '\b':
            case 0x7F: nav_up(f); break;
            case 's': cycle_sort(f); break;
            case 'r': rescan(f); break;
            case 'b': nav_back(f); break;
            case 'h': nav_to(f, "/", 1); break;
            default: break;
        }
    }
}

static void files_resize(App *a, int w, int h) {
    Files *f = a->state;
    if (!f) return;
    f->w = w > 0 ? w : 0;
    f->h = h > 0 ? h : 0;
}

/* ------------------------------------------------------------ lifecycle */
static void files_init(App *a, int w, int h) {
    Files *f = calloc(1, sizeof(Files));
    if (!f) return;
    f->ent = calloc(MAX_ENTRIES, sizeof(Entry));
    if (!f->ent) { free(f); return; }

    f->w = w > 0 ? w : 0;
    f->h = h > 0 ? h : 0;
    f->px = f->py = -1;
    f->last_click_idx = -1;
    f->sort = SORT_NAME;
    snprintf(f->path, sizeof(f->path), "/");
    scan(f);
    a->state = f;
}

static void files_free(App *a) {
    Files *f = a->state;
    if (!f) return;
    a->state = NULL;
    free(f->ent);
    free(f);
}

const AppVT app_vt_files = {
    .id = "files", .title = "Files",
    .init = files_init, .draw = files_draw, .key = files_key,
    .click = files_click, .resize = files_resize, .free = files_free,
};
