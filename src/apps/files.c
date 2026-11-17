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
 *  font_get() in src/font.c - grows the toolbar, the breadcrumb
 *  line, the rows and the footer together. layout_build() is the one
 *  and only source of those numbers: one Layout holds the outer
 *  padding, every inter-column gap, the row height, the icon size,
 *  the corner radius and the right edge of each column. files_draw()
 *  paints from it and files_click()/files_wheel() hit-test and scroll
 *  with it, so a click always lands on the row that was painted and
 *  the wheel moves the list the pointer is over. Nothing here is a
 *  fixed 15 px, and no text is placed with a guessed +2: each line is
 *  centred in its band from font_ascent() (see line_top()).
 *
 *  ICONS are drawn geometry, in the same style as the desktop
 *  launcher's icon_folder()/icon_pencil() in src/main.c - no images.
 *  A 20x20 box (ui_px() grows it with the text scale) holds one of
 *  nine marks, chosen once per entry from the real name and st_mode:
 *
 *    folder with a tab          a directory
 *    page, folded corner        a plain file (and the unknown type)
 *    page + play triangle       executable, mode & 0111
 *    page + mountain and sun    .png .jpg .jpeg .ppm .bmp .gif
 *    page + gear                config: .conf .cfg .ini
 *    page + palette disc        a theme: .theme
 *    page + zip teeth           .zip .gz .tgz .tar .xz .bz2
 *    page + ruled lines         .txt .md .log
 *    page + arrow               a symlink (dimmed when it dangles)
 *
 *  The toolbar is five real buttons (Back, Up, Home, Refresh, the sort
 *  cycle) drawn as rounded rects with an icon, a label when the width
 *  allows it, a hover fill on the last pointer position the desktop
 *  reported, and a pressed fill for the frame the click causes. It is a
 *  frame, not an animation: the desktop repaints from events (src/main.c
 *  sets `dirty` from input, not from a timer) so an app cannot keep a
 *  highlight alive for a while and then clear it; the press therefore
 *  ends on the next repaint after PRESS_S, and because the pointer is
 *  still on that button the button falls back to its hover fill instead
 *  of staying lit. The breadcrumb line is clickable per segment and the
 *  status line carries the item count, the selected entry's name, its
 *  real size and the free space statvfs() reports for the directory.
 *  Each breadcrumb segment's hover pill and click span come from one
 *  function, crumb_pill(), which clips the span to the padded band - so a
 *  folder name wider than the band is cut at the padding, never painted
 *  over the desktop, and its chevron cannot cross the edge either.
 *
 *  The listing is opendir()/readdir()/lstat(), the size and stamp are the
 *  real st_size/st_mtime, the free-space figure is statvfs() on the
 *  directory being shown. Names are kept up to the 255 bytes ext4 allows.
 *
 *  SYMLINKS are shown as what they stand for: stat() supplies the target's
 *  size and mtime (so a link to a regular file does not inherit the 0777
 *  that lstat() reports for a link), readlink() names the target in the
 *  footer, and a link to a directory opens like one. The icon is the page
 *  with an arrow either way - a link is a pointer, not the thing it points
 *  at - and a dangling link draws that page dimmer and is marked
 *  "(broken link)". If opendir() fails the real strerror(errno) is drawn
 *  instead of rows.
 *
 *  KEYS   j/k, Up/Down   move          Enter      open (dir descends,
 *         Home/End       first/last               a file says so)
 *         PageUp/PageDn  one page      Backspace  parent
 *         s              cycle sort    r          rescan
 *         b              back in history  h       home ("/")
 *  MOUSE  a click selects a row; a second click on the same row within
 *         450 ms (measured with CLOCK_MONOTONIC) opens it; the toolbar
 *         buttons and every breadcrumb segment are clickable.
 *  WHEEL  the window's wheel hook scrolls the list by WHEEL_ROWS rows a
 *         notch (positive = up, toward the first entry). The highlighted
 *         row is pulled to the nearest edge when the view moves past it,
 *         so the selection can never be scrolled out of sight.
 *
 *  COLUMNS the modified-time column is dropped under 380 px of window (the
 *         brief's literal number) and, regardless of width, whenever the
 *         name column would fall under ui_px(90) - that fit test is what
 *         actually decides, so a 480 px window at 1.4x keeps its dates.
 *
 *  AppVT hands an app the pointer position only together with a click,
 *  so the toolbar highlight marks the button under the last pointer
 *  position the app was told about - it is a real position, never a
 *  guess about where the mouse might be now.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 512
#define NAME_LEN    256   /* ext4 allows 255 bytes; a shorter buffer hides entries */
#define LINK_LEN    96    /* as much of a symlink target as the footer can use */
#define PATH_LEN    512
#define HIST_MAX    24
#define MAX_CRUMBS  16

/* sort modes, cycled by the toolbar button and by 's' */
enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_TIME = 2, SORT_MODES = 3 };

/* toolbar buttons, in drawing order; TB_SORT sits on the right edge */
enum { TB_BACK, TB_UP, TB_HOME, TB_REFRESH, TB_SORT, TB_COUNT };

/* the words on those buttons (TB_SORT shows the mode it is set to) */
static const char *const TB_LABEL[TB_COUNT] = { "Back", "Up", "Home", "Refresh", "Name" };

/* Which drawn icon an entry gets - decided once per entry, at scan time,
 * from the real name and st_mode (see kind_of()). Nine marks: folder, plain
 * page, executable, image, config, theme, archive, text, symlink. */
enum { IC_FOLDER, IC_FILE, IC_EXEC, IC_IMAGE, IC_CONF, IC_ARCHIVE, IC_TEXT, IC_LINK,
       IC_THEME };

/* how long a toolbar button stays visibly pressed after a real click */
#define PRESS_S 0.14

/* Rows the wheel moves per notch. A count, not a pixel size: the wheel
 * moves whole entries, so it follows the text scale for free. */
#define WHEEL_ROWS 3

typedef struct {
    char          name[NAME_LEN];
    char          target[LINK_LEN];  /* readlink() of a symlink, empty otherwise */
    long long     size;
    long long     mtime;
    unsigned      mode;
    unsigned char is_dir;
    unsigned char is_link;
    unsigned char link_ok;           /* stat() of the link target succeeded */
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
    int content_l, content_r;   /* the padded band every band is clipped to */
    int rule;                   /* hairline thickness, ui_px(1) */
    int toolbar_h, crumb_h, footer_h;
    int row_h, icon_sz, rows;
    int list_y, list_bot;
    int btn_x[TB_COUNT], btn_w[TB_COUNT];
    int tb_labels;      /* the four navigation buttons carry their names */
    int tb_sort_label;  /* the sort button carries the current mode */

    /* insets and corner radii, all ui_px(): the row highlight is inset
     * from the list edges by row_xpad/row_ypad, buttons and highlights
     * share `radius`, and the scrollbar lives in the right padding. */
    int radius, row_xpad, row_ypad, press_dy;
    int bar_w, bar_pad, thumb_min;

    /* the breadcrumb band: the app's folder mark, the chevron cell between
     * two segments, and the hover pill's air around a segment. */
    int crumb_icon, crumb_chev, crumb_hpad, crumb_vpad, chev_sz, chev_pad,
        crumb_igap;

    /* text tops inside their own band, from line_top(): the row, the
     * breadcrumb, the footer and the toolbar labels. */
    int row_text_dy, crumb_text_dy, footer_text_dy, tb_text_dy, tb_icon_dy;

    /* one column grid: name on the left, size and stamp right-aligned on
     * the same two edges in every row. */
    int name_x, name_w;
    int size_right, size_col;
    int time_right, time_col;
    int show_time, show_size;

    /* footer: the gap after the item count, before the free-space figure
     * and the smallest middle column worth drawing. */
    int foot_gap, foot_colgap, foot_mininfo;
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
    int    pressed;               /* toolbar button pressed last, -1 = none */
    double pressed_t;             /* CLOCK_MONOTONIC of that press */
    double last_click_t;          /* CLOCK_MONOTONIC of the last row click */
    int    last_click_idx;
    int    last_click_x, last_click_y;
    char   hist[HIST_MAX][PATH_LEN];
    int    hist_n;
    unsigned long long free_bytes;
    int    has_free;
} Files;

static int iround(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }
/* A box that must *contain* a string rounds up: rounding a text width down by
 * half a pixel is enough for draw_clipped() to cut the last letter. */
static int iceil(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* ------------------------------------------------------------
 *  Vertical centring that is not a guess
 *
 *  font_draw() takes the TOP of a line and puts the baseline exactly
 *  font_ascent() below it. The ink of that line runs from `ascent`
 *  above the baseline to `descent` below it, and stbtt's
 *  ScaleForPixelHeight makes ascent + descent equal the nominal size
 *  - which is font_height()/1.4, because src/font.c draws in a 1.4*size
 *  line box (src/font.c:60,119). So the ink box is exactly
 *  font_height()/1.4 tall and centring it in a band of `band` pixels is
 *
 *      y = (band - font_height(f)/1.4) / 2
 *
 *  measured from the band's top. Caps and descenders are then equally
 *  inside the row; the old (band - font_height())/2 guess sat about a
 *  tenth of a line high because it centred the leading too. The other
 *  apps in this tree use the same rule, so no two windows disagree
 *  about what "centred" means. Every band here uses this function.
 * ------------------------------------------------------------ */
static int line_top(Font *f, int band) {
    int ink;
    if (!f || band <= 0) return 0;
    ink = iround(font_height(f) / 1.4f);
    if (ink >= band) return 0;
    return (band - ink) / 2;
}

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

/* Extension tables. A NULL ends each list, so has_ext() needs no count to
 * get out of step with the data. A .theme is a palette rather than a set of
 * switches, so it keeps its own disc and does not wear the gear that
 * .conf/.cfg/.ini wear - that distinction was already in this file (and in
 * the files-rich brief), and collapsing it would be a regression. */
static const char *const EXT_CONF[]    = { ".conf", ".cfg", ".ini", NULL };
static const char *const EXT_IMAGE[]   = { ".png", ".jpg", ".jpeg", ".ppm", ".bmp", ".gif", NULL };
static const char *const EXT_ARCHIVE[] = { ".zip", ".gz", ".tgz", ".tar", ".xz", ".bz2", NULL };
static const char *const EXT_TEXT[]    = { ".txt", ".md", ".log", NULL };

static int has_ext(const char *name, const char *const *list) {
    for (const char *const *p = list; *p; p++)
        if (ends_with_ci(name, *p)) return 1;
    return 0;
}

/* A symlink is decided first: it is a pointer, not the thing it points at,
 * so it keeps its own arrow icon even when the target is a directory (the
 * trailing '/' and the status line still say the target is a folder). A
 * dangling link has no mode of its own to speak of and must never inherit
 * the 0777 that lstat() reports for a link. Then the name decides the
 * config/image/archive/text families, then the execute bit, then plain. */
static int kind_of(const Entry *e) {
    if (e->is_link) return IC_LINK;
    if (e->is_dir)  return IC_FOLDER;
    if (ends_with_ci(e->name, ".theme")) return IC_THEME;   /* a palette */
    if (has_ext(e->name, EXT_CONF))    return IC_CONF;      /* the gear */
    if (has_ext(e->name, EXT_IMAGE))   return IC_IMAGE;
    if (has_ext(e->name, EXT_ARCHIVE)) return IC_ARCHIVE;
    if (has_ext(e->name, EXT_TEXT))    return IC_TEXT;
    if ((e->mode & 0111u) != 0)        return IC_EXEC;
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

/* draw inside maxw pixels, ending in ".." when it has to be cut.
 * The half-pixel tolerance matters: a column width is computed as
 * (x0 + width) - x0, which in floating point can come out a few
 * thousandths of a pixel smaller than the width it started from - enough
 * to chop the last letter off a label that fits perfectly. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 4.0f) return;
    char buf[PATH_LEN + 8];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 3) len = sizeof(buf) - 3;
    memcpy(buf, text, len);
    buf[len] = 0;
    if (font_width(f, buf) <= maxw + 0.5f) { font_draw(s, f, x, y, buf, color); return; }
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
    L->content_l = L->pad;
    L->content_r = w - L->pad;
    if (L->content_r < L->content_l) L->content_r = L->content_l;
    L->rule = ui_px(1);
    L->radius = ui_px(5);
    L->row_xpad = L->pad / 2;
    L->row_ypad = ui_px(1);
    L->press_dy = ui_px(1);
    L->bar_w = ui_px(3);
    L->bar_pad = ui_px(2);
    L->thumb_min = ui_px(10);
    L->crumb_chev = ui_px(12);
    L->chev_sz = ui_px(7);
    L->chev_pad = ui_px(2);
    L->crumb_hpad = ui_px(3);
    L->crumb_vpad = ui_px(2);
    L->crumb_icon = ui_px(14);
    L->crumb_igap = ui_px(4);
    L->foot_gap = ui_px(14);
    L->foot_colgap = ui_px(8);
    L->foot_mininfo = ui_px(20);

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

    /* Every line's top inside its own band, from the face that draws it. */
    L->row_text_dy = line_top(L->fl, L->row_h);
    L->tb_text_dy = line_top(L->ft, L->btn);
    L->tb_icon_dy = (L->btn - L->icw) / 2;
    if (L->tb_icon_dy < 0) L->tb_icon_dy = 0;

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

    L->crumb_text_dy = line_top(L->fc, L->crumb_h);
    L->footer_text_dy = line_top(L->ff, L->footer_h);

    L->list_y = L->toolbar_h + L->crumb_h + L->pad / 2;
    L->list_bot = h - L->footer_h;
    if (L->list_bot < L->list_y) L->list_bot = L->list_y;
    int avail = L->list_bot - L->list_y;
    L->rows = avail > 0 ? avail / L->row_h : 0;
    if (L->rows > MAX_ENTRIES) L->rows = MAX_ENTRIES;

    /* Toolbar buttons: icon + label when the width allows it, icons only
     * when it does not. Both layouts are tried with real font widths. */
    {
        int nat[TB_COUNT];
        /* the sort button must hold whichever mode label it will show, so
         * the box is measured against the widest of the three */
        int sort_w = 0;
        for (int m = 0; m < SORT_MODES; m++) {
            int sw = iceil(font_width(L->ft, sort_label(m)));
            if (sw > sort_w) sort_w = sw;
        }
        long total = L->pad;
        for (int i = 0; i < TB_COUNT; i++) {
            const char *lab = (i == TB_SORT) ? "Refresh" : TB_LABEL[i];  /* widest */
            nat[i] = L->pad + L->icw + L->pad + iceil(font_width(L->ft, lab)) + L->pad;
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
        L->size_col = iceil(font_width(L->fl, "0000.0 MB"));
        L->time_col = iceil(font_width(L->fl, "0000-00-00 00:00"));
        L->name_x = L->pad + L->icon_sz + L->pad;
        L->size_right = right;
        L->time_right = right;

        /* The brief's rule: no modified-time column under 380 px of window.
         * 380 is a literal window width - the columns it holds are already
         * measured from the scaled font - so it is not scaled again here.
         * Whether the column actually fits is decided below by measuring the
         * name column against ui_px(90); that test is the real guard, and it
         * keeps the dates in a 480 px window at 1.4x where they still fit. */
        L->show_time = (w >= 380);
        if (L->show_time)
            L->size_right = L->time_right - L->time_col - L->pad;
        L->name_w = L->size_right - L->size_col - L->gap - L->name_x;
        if (L->show_time && L->name_w < ui_px(90)) {
            L->show_time = 0;
            L->size_right = right;
            L->name_w = L->size_right - L->size_col - L->gap - L->name_x;
        }
        /* in a window too narrow for a size column the names take it all */
        L->show_size = (L->name_w >= ui_px(90));
        if (!L->show_size) {
            L->size_right = right;
            L->name_w = right - L->name_x;
        }
        if (L->name_w < ui_px(16)) L->name_w = ui_px(16);
    }
    return 1;
}

/* ------------------------------------------------------------
 *  The handful of derived positions the painter and the hit-tester
 *  must agree on. They live here, next to layout_build(), and there is
 *  exactly one copy of each: draw_toolbar()/draw_list() paint from
 *  tb_rect()/row_rect() and files_click() hit-tests with tb_btn_at()/
 *  list_row_at(), which are built on the same two functions. So a click
 *  always lands on the thing that was painted - at any text scale.
 * ------------------------------------------------------------ */

/* vertical offset of the toolbar button row inside the toolbar band */
static int tb_y(const Layout *L) {
    int y = (L->toolbar_h - L->btn) / 2;
    return y < 0 ? 0 : y;
}

/* The rectangle of toolbar button i, app-local. 0 when the button did
 * not fit and was therefore not drawn (a button that is not painted is
 * not clickable either). */
static int tb_rect(const Layout *L, int i, int *bx, int *by, int *bw, int *bh) {
    if (i < 0 || i >= TB_COUNT || L->btn_w[i] <= 0 || L->btn <= 0) return 0;
    *bx = L->btn_x[i];
    *by = tb_y(L);
    *bw = L->btn_w[i];
    *bh = L->btn;
    return 1;
}

/* which button is under (cx, cy), or -1 */
static int tb_btn_at(const Layout *L, int cx, int cy) {
    for (int i = 0; i < TB_COUNT; i++) {
        int bx, by, bw, bh;
        if (!tb_rect(L, i, &bx, &by, &bw, &bh)) continue;
        if (cx >= bx && cx < bx + bw && cy >= by && cy < by + bh) return i;
    }
    return -1;
}

/* The rectangle of visible list row i (0 = topmost visible row), and
 * whether that row is fully inside the list band. */
static int row_rect(const Layout *L, int i, int *ry, int *rh) {
    if (i < 0 || i >= L->rows || L->row_h <= 0) return 0;
    *ry = L->list_y + i * L->row_h;
    *rh = L->row_h;
    return (*ry + *rh <= L->list_bot);
}

/* The inverse of row_rect(): which visible row holds cy, or -1. The
 * division is exact because row_rect() only ever multiplies. */
static int list_row_at(const Layout *L, int cy) {
    if (L->row_h <= 0 || cy < L->list_y || cy >= L->list_bot) return -1;
    int i = (cy - L->list_y) / L->row_h;
    int ry, rh;
    if (!row_rect(L, i, &ry, &rh)) return -1;
    return i;
}

/* a button is live when the action it performs would do something */
static int tb_enabled(const Files *f, int i) {
    if (i == TB_BACK) return f->hist_n > 0;
    if (i == TB_UP)   return strcmp(f->path, "/") != 0;
    return 1;
}

/* ------------------------------------------------------------
 *  Two coordinate spaces, and the rule that keeps them apart
 *
 *  AppVT.draw() gets the app's ORIGIN on the compositor's surface
 *  (src/main.c: app_draw(cur->app, s, cx0, cy0, ...)) and paints with
 *  x + local. AppVT.click() gets APP-LOCAL coordinates (app_click(hit->app,
 *  lx - 1, ly - tb - 1, ...)), so the pointer the app stores is app-local.
 *
 *  Every rectangle in here - btn_x/tb_rect(), list_y/row_rect(), the crumb
 *  spans from crumb_build() - is app-local as well. So a hit test compares
 *  the stored pointer DIRECTLY against those rectangles and the draw origin
 *  never enters it. Mixing the two (pointer vs x + rect) only looks right
 *  while the window happens to sit at 0,0: it kills hover as soon as the
 *  window is dragged right, and it lets a click on a row light up the
 *  breadcrumb band above it. The host harness renders the app at (101,61)
 *  to keep this from regressing.
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 *  Breadcrumbs: built from the real path, laid out to fit w
 * ------------------------------------------------------------ */
static int crumb_build(const Files *f, const Layout *L, Crumb *out, int *dots) {
    Crumb all[MAX_CRUMBS];
    int n = 0;

    *dots = 0;
    if (f->path[0] != '/') return 0;

    all[n].target = 1; all[n].ls = 0; all[n].ll = 0; n++;      /* "/" */
    int i = 1;
    while (f->path[i]) {
        int s = i;
        while (f->path[i] && f->path[i] != '/') i++;
        if (n == MAX_CRUMBS) {
            /* The window is full. Forget the oldest segment and keep going,
             * so the last one is always the folder we are actually in - and
             * say so with the ellipsis, because the first visible segment is
             * no longer the root. */
            memmove(&all[0], &all[1], sizeof(all[0]) * (MAX_CRUMBS - 1));
            n = MAX_CRUMBS - 1;
            *dots = 1;
        }
        all[n].target = i;
        all[n].ls = s;
        all[n].ll = i - s;
        n++;
        if (f->path[i] == '/') i++;
    }

    float chev = (float)L->crumb_chev;
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

    /* The app's own folder mark owns the first crumb_icon pixels of the
     * band; the path starts after it (and after the ellipsis when leading
     * segments had to go). Everything is measured inside the padded band
     * (content_l .. content_r), so nothing can leave the window. */
    float avail = (float)(L->content_r - L->content_l - L->crumb_icon - L->crumb_igap);
    int first = 0;
    float dots_w = font_width(L->fc, "...") + (float)L->gap;
    /* Drop whole leading segments until the line fits *with* the ellipsis
     * that says so. The ellipsis is never dropped on its own: a breadcrumb
     * that starts part way down without a mark would claim the path starts
     * where it does not. The last segment (the folder we are in) always
     * survives, and is clipped only if it alone is wider than the line. */
    while (total + dots_w > avail && first < n - 1) {
        total -= wid[first];
        if (first + 1 < n) total -= chev;
        first++;
        *dots = 1;
    }

    float x = (float)L->content_l + (float)L->crumb_icon + (float)L->crumb_igap
            + (*dots ? dots_w : 0.0f);
    int m = 0;
    for (int k = first; k < n; k++) {
        float x1 = x + wid[k];
        /* A single component wider than the whole band is clipped here, not
         * just in the painter: the span the click test and the hover pill
         * use must stop at the padded edge as well, or a long folder name
         * would paint its pill over the desktop. */
        if (x > (float)L->content_r) x = (float)L->content_r;
        if (x1 > (float)L->content_r) x1 = (float)L->content_r;
        out[m].x0 = x;
        out[m].x1 = x1;
        out[m].target = all[k].target;
        out[m].ls = all[k].ls;
        out[m].ll = all[k].ll;
        x = x1;
        if (k + 1 < n) x += chev;
        m++;
    }
    return m;
}

/* ------------------------------------------------------------
 *  One crumb's hover pill and click span, clipped to the padded band.
 *  The painter fills exactly this rectangle (draw_crumbs()) and the
 *  hit-test compares the pointer with exactly this rectangle
 *  (files_click()), so a path component wider than the breadcrumb band
 *  can neither paint nor capture a single pixel outside the window - and
 *  the two sides cannot drift apart, because there is only one copy.
 * ------------------------------------------------------------ */
static void crumb_pill(const Layout *L, const Crumb *c, float *px, float *pw) {
    float x0 = c->x0 - (float)L->crumb_hpad;
    float x1 = c->x1 + (float)L->crumb_hpad;
    if (x0 < (float)L->content_l) x0 = (float)L->content_l;
    if (x1 > (float)L->content_r) x1 = (float)L->content_r;
    if (x1 < x0) x1 = x0;
    *px = x0;
    *pw = x1 - x0;
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

        int is_link = S_ISLNK(st.st_mode) ? 1 : 0;
        int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        struct stat tgt;
        int have_tgt = 0;
        char target[LINK_LEN];
        target[0] = 0;

        if (is_link) {
            /* A symlink is a stand-in for something else, so every number we
             * show for it has to be the target's: lstat() would give the
             * length of the path it contains and mode 0777, which would make
             * every symlink look like an executable of a few bytes. */
            have_tgt = (stat(full, &tgt) == 0);
            if (have_tgt && S_ISDIR(tgt.st_mode)) is_dir = 1;
            long r = (long)readlink(full, target, sizeof(target) - 1);
            if (r < 0) target[0] = 0;
            else target[r] = 0;
        }
        const struct stat *shown = (is_link && have_tgt) ? &tgt : &st;

        Entry *e = &f->ent[f->count];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        snprintf(e->target, sizeof(e->target), "%s", target);
        e->is_dir  = (unsigned char)is_dir;
        e->is_link = (unsigned char)is_link;
        e->link_ok = (unsigned char)(is_link ? have_tgt : 0);
        e->size    = (long long)shown->st_size;
        e->mtime   = (long long)shown->st_mtime;
        e->mode    = (unsigned)shown->st_mode;
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

/* 20x20 nominal box (ui_px() and the row size it, so it grows with the
 * text scale) holding a folder, or a page with a folded corner and a mark
 * for the kind of file. The mark is what tells the nine kinds apart at a
 * glance; the page is the same for all of them so the list still reads as
 * one list. */

/* gear - the launcher's icon_gear() idea, fattened: six thick teeth and a
 * thick ring, which is the only way that shape survives an 8 px radius. */
static void mark_gear(Surface *s, float cx, float cy, float r, u32 c) {
    float w = r * 0.50f;             /* a fat ring and fat teeth survive 8 px */
    for (int i = 0; i < 6; i++) {
        float a = (float)i * 3.14159265f / 3.0f;
        gfx_line(s, cx + cosf(a) * r * 0.50f, cy + sinf(a) * r * 0.50f,
                    cx + cosf(a) * r,        cy + sinf(a) * r, w, c);
    }
    gfx_rrect_aa(s, cx - r * 0.48f, cy - r * 0.48f, r * 0.96f, r * 0.96f,
                 r * 0.48f, 0, c, w);
}

/* A zipper: a vertical band with teeth alternating left and right, so the
 * mark cannot be mistaken for the ruled lines of a text file. */
static void mark_zip(Surface *s, float x, float y, float w, float h, u32 band) {
    float zw = w * 0.24f;
    float zx = x + (w - zw) * 0.5f;
    gfx_rrect(s, zx, y, zw, h, zw * 0.35f, band);
    for (int i = 0; i < 4; i++) {
        float ty = y + h * (0.04f + (float)i * 0.25f);
        float tw = w * 0.30f;
        float tx = (i & 1) ? zx + zw * 0.55f : zx + zw * 0.45f - tw;
        gfx_rrect(s, tx, ty, tw, h * 0.11f, h * 0.055f, band);
    }
}

/* ruled lines, the last one short, like a page of writing */
static void mark_rules(Surface *s, float x, float y, float w, float h, u32 c) {
    float lh = h * 0.11f;
    if (lh < 1.0f) lh = 1.0f;
    for (int i = 0; i < 4; i++) {
        float lw = (i == 3) ? w * 0.46f : w * 0.82f;
        gfx_rrect(s, x, y + h * (0.05f + (float)i * 0.27f), lw, lh, lh * 0.5f, c);
    }
}

/* a small arrow pointing up and to the right: "this is a pointer" */
static void mark_arrow(Surface *s, float x, float y, float w, float h, u32 c) {
    float th = h * 0.19f;
    if (th < 1.0f) th = 1.0f;
    gfx_line(s, x + w * 0.18f, y + h * 0.80f, x + w * 0.66f, y + h * 0.32f, th, c);
    float head[6] = { x + w * 0.90f, y + h * 0.08f,
                      x + w * 0.94f, y + h * 0.56f,
                      x + w * 0.42f, y + h * 0.12f };
    gfx_polygon(s, head, 3, c);
}

/* The folder mark, in the launcher's geometric style: a tab body plus the
 * body itself. Used for every directory row and as this window's own mark
 * at the head of the breadcrumb band. */
static void draw_folder(Surface *s, float x, float y, float sz, u32 fg) {
    float w = sz * 0.90f, h = sz * 0.60f;
    float bx = x + (sz - w) * 0.5f, by = y + (sz - h) * 0.5f + sz * 0.07f;
    gfx_rrect(s, bx, by - sz * 0.15f, w * 0.46f, sz * 0.18f, sz * 0.04f, fg);
    gfx_rrect_aa(s, bx, by, w, h, sz * 0.10f, fg, 0, 0);
}

static void draw_entry_icon(Surface *s, const Entry *e, float x, float y, float sz,
                            int sel, const Theme *th) {
    if (sz < 6.0f) return;

    /* a dangling link has no target: draw it, but plainly */
    int broken = (e->kind == IC_LINK) && !e->link_ok;

    if (e->kind == IC_FOLDER) {
        draw_folder(s, x, y, sz, color_alpha(th->accent, sel ? 0.98f : 0.80f));
        return;
    }

    float w = sz * 0.62f, h = sz * 0.82f, fold = w * 0.42f;
    float px = x + (sz - w) * 0.5f, py = y + (sz - h) * 0.5f;
    u32 body = color_alpha(th->text, sel ? 0.94f : (broken ? 0.38f : 0.72f));
    u32 flap = color_alpha(th->text, sel ? 0.52f : (broken ? 0.22f : 0.38f));

    float page[10] = { px, py, px + w - fold, py, px + w, py + fold,
                       px + w, py + h, px, py + h };
    gfx_polygon(s, page, 5, body);
    float tri[6] = { px + w - fold, py, px + w, py + fold, px + w - fold, py + fold };
    gfx_polygon(s, tri, 3, flap);

    u32 mark = color_alpha(th->accent, sel ? 0.98f : (broken ? 0.50f : 0.85f));
    /* the mark band: below the fold, inside the page's edges, with a
     * margin so nothing spills over the silhouette */
    float my = py + fold + (h - fold) * 0.14f;
    float mh = (h - fold) * 0.70f;
    float mx = px + w * 0.15f, mw = w * 0.70f;

    switch (e->kind) {
    case IC_CONF:
        mark_gear(s, px + w * 0.5f, my + mh * 0.5f, mh * 0.60f, mark);
        break;
    case IC_THEME: {
        /* a palette is one filled disc: no teeth, no notches, no lines, so
         * it cannot be read as the gear a .conf gets */
        float r = mh * 0.46f;
        gfx_rrect(s, px + w * 0.5f - r, my + mh * 0.5f - r,
                  r * 2.0f, r * 2.0f, r, mark);
        break;
    }
    case IC_IMAGE: {
        float mt[6] = { mx, my + mh,
                        mx + mw * 0.44f, my + mh * 0.18f,
                        mx + mw, my + mh };
        gfx_polygon(s, mt, 3, mark);
        float r = mh * 0.17f;
        gfx_rrect(s, mx + mw * 0.62f, my + mh * 0.02f, r * 2.0f, r * 2.0f, r,
                  color_alpha(th->accent, broken ? 0.40f : 0.70f));
        break;
    }
    case IC_ARCHIVE:
        mark_zip(s, mx, my, mw, mh, mark);
        break;
    case IC_TEXT:
        mark_rules(s, mx, my, mw, mh, mark);
        break;
    case IC_EXEC: {
        float tp[6] = { mx + mw * 0.14f, my,
                        mx + mw * 0.14f, my + mh,
                        mx + mw * 0.88f, my + mh * 0.5f };
        gfx_polygon(s, tp, 3, mark);
        break;
    }
    case IC_LINK:
        mark_arrow(s, mx, my, mw, mh, mark);
        break;
    default:
        /* IC_FILE: a blank page is the honest icon for "I do not know" */
        break;
    }
}

/* ------------------------------------------------------------
 *  Drawing
 * ------------------------------------------------------------ */
/* One toolbar button: rounded rect, a real 1 px border, the icon, and the
 * label when the layout found room for it. Three states are visible:
 *   hover    - the last pointer position the desktop reported is inside
 *   pressed  - the frame(s) drawn within PRESS_S of the click; the first
 *              repaint after that window shows hover instead (the desktop
 *              repaints from events, not from a timer, so an app cannot
 *              hold a highlight for a while and then clear it - but the
 *              pointer is still on the button, so it never looks stuck lit)
 *   disabled - the action would do nothing (no history / already at /)
 * The pressed button's contents shift down one pixel and the fill and
 * border go to full accent, which is the whole vocabulary a button needs. */
static void draw_toolbar(Files *f, Surface *s, int x, int y, int w,
                         const Layout *L, const Theme *th) {
    if (w <= 0 || L->toolbar_h <= 0) return;

    surf_blend(s, x, y, w, L->toolbar_h, th->panel);
    if (L->toolbar_h > L->rule)
        surf_blend(s, x, y + L->toolbar_h - L->rule, w, L->rule, th->panel_border);

    double now = mono_now();

    for (int i = 0; i < TB_COUNT; i++) {
        int bx, by, bw, bh;
        if (!tb_rect(L, i, &bx, &by, &bw, &bh)) continue;

        int enabled = tb_enabled(f, i);
        int hov = f->px >= bx && f->px < bx + bw &&
                  f->py >= by && f->py < by + bh;
        int down = hov && enabled && f->pressed == i &&
                   (now - f->pressed_t) >= 0.0 && (now - f->pressed_t) < PRESS_S;

        u32 fill, border;
        if (!enabled) {
            fill   = color_alpha(th->text, 0.025f);
            border = color_alpha(th->panel_border, 0.55f);
        } else if (down) {
            fill   = color_alpha(th->accent, 0.42f);
            border = color_alpha(th->accent, 0.90f);
        } else if (hov) {
            fill   = color_alpha(th->accent, 0.24f);
            border = color_alpha(th->accent, 0.60f);
        } else {
            fill   = color_alpha(th->text, 0.085f);
            border = color_alpha(th->panel_border, 0.95f);
        }

        gfx_rrect_aa(s, (float)(x + bx), (float)(y + by), (float)bw, (float)bh,
                     (float)L->radius, fill, border, 1.0f);

        u32 fg = !enabled ? color_alpha(th->text, 0.25f)
               : down    ? th->text
               : hov     ? th->text
                         : color_alpha(th->text, 0.85f);

        /* pressed: the icon and the word sit one pixel lower, inside the
         * same button - the pixel the finger would have pushed */
        int dy = down ? L->press_dy : 0;
        float ix = (float)(x + bx) + (float)L->pad;
        float iy = (float)(y + by) + (float)L->tb_icon_dy + (float)dy;
        tb_icon(s, i, ix, iy, (float)L->icw, fg);

        if (i == TB_SORT ? L->tb_sort_label : L->tb_labels) {
            const char *lab = (i == TB_SORT) ? sort_label(f->sort) : TB_LABEL[i];
            float ly = (float)(y + by) + (float)L->tb_text_dy + (float)dy;
            float lx = ix + (float)L->icw + (float)L->pad;
            float maxw = (float)(x + bx + bw) - lx - (float)L->pad;
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
    int n = crumb_build(f, L, cr, &dots);

    float ty = (float)cy + (float)L->crumb_text_dy;

    /* the window's own mark: a folder at the head of the path */
    draw_folder(s, (float)(x + L->pad),
                (float)cy + ((float)L->crumb_h - (float)L->crumb_icon) * 0.5f,
                (float)L->crumb_icon, color_alpha(th->accent, 0.70f));

    if (dots)
        draw_clipped(s, L->fc,
                     (float)(x + L->content_l + L->crumb_icon + L->crumb_igap),
                     ty,
                     (float)(L->content_r - L->content_l - L->crumb_icon -
                             L->crumb_igap),
                     "...", color_alpha(th->text, 0.45f));

    for (int i = 0; i < n; i++) {
        char lab[NAME_LEN];
        int len = cr[i].ll;
        if (len >= (int)sizeof(lab)) len = (int)sizeof(lab) - 1;
        if (len > 0) memcpy(lab, f->path + cr[i].ls, (size_t)len);
        lab[len] = 0;
        const char *text = len ? lab : "/";

        /* local pointer vs the pill crumb_pill() computes - see the
         * coordinate-space note above layout_build(); the draw origin stays
         * out of this. The pill is clipped to the padded band, so hovering
         * a path component wider than the band cannot paint outside the
         * window. */
        float pill_x, pill_w;
        crumb_pill(L, &cr[i], &pill_x, &pill_w);
        int hov = f->py >= L->toolbar_h && f->py < L->toolbar_h + L->crumb_h &&
                  pill_w > 0.0f &&
                  (float)f->px >= pill_x && (float)f->px < pill_x + pill_w;
        int last = (i == n - 1);

        if (hov)
            gfx_rrect(s, (float)x + pill_x,
                      (float)cy + (float)L->crumb_vpad,
                      pill_w, (float)(L->crumb_h - 2 * L->crumb_vpad),
                      (float)L->radius, color_alpha(th->accent, 0.18f));

        u32 col = last ? th->text : (hov ? th->text : color_alpha(th->text, 0.62f));
        draw_clipped(s, L->fc, (float)x + cr[i].x0, ty,
                     cr[i].x1 - cr[i].x0, text, col);

        if (i + 1 < n) {
            /* the arrow lives inside the cell crumb_chev reserved for it,
             * with a pixel of air so it never touches a segment. It cannot
             * cross the right edge: a clipped last span leaves its chevron
             * at the band edge, inside the padded band. */
            float sz = (float)L->chev_sz;
            float cxp = (float)x + cr[i].x1 + (float)L->chev_pad;
            if (cxp + sz > (float)x + L->content_r) cxp = (float)x + L->content_r - sz;
            draw_chevron(s, cxp,
                         (float)cy + ((float)L->crumb_h - sz) * 0.5f, sz,
                         color_alpha(th->text, 0.30f));
        }
    }
    if (L->crumb_h > L->rule)
        surf_blend(s, x + L->content_l, cy + L->crumb_h - L->rule,
                   L->content_r - L->content_l, L->rule,
                   color_alpha(th->panel_border, 0.6f));
}

static void draw_list(Files *f, Surface *s, int x, int y, int w,
                      const Layout *L, const Theme *th) {
    int top = y + L->list_y;

    if (f->error[0]) {
        draw_clipped(s, L->fl, (float)(x + L->pad), (float)(top + L->row_text_dy),
                     (float)(w - 2 * L->pad), f->error, th->accent);
        return;
    }
    if (f->count == 0) {
        draw_clipped(s, L->fl, (float)(x + L->pad), (float)(top + L->row_text_dy),
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
        int ryt, rh;
        /* row_rect() is the same function files_click() hit-tests through:
         * the row painted here is the row a click on it lands on */
        if (!row_rect(L, i, &ryt, &rh)) break;
        int idx = f->scroll + i;
        if (idx >= f->count) break;
        float ry = (float)(y + ryt);

        Entry *e = &f->ent[idx];
        int sel = (idx == f->sel);

        if (sel) {
            gfx_rrect_aa(s, (float)x + (float)L->row_xpad, ry + (float)L->row_ypad,
                         (float)(w - 2 * L->row_xpad),
                         (float)(rh - 2 * L->row_ypad), (float)L->radius,
                         color_alpha(th->accent, 0.20f), 0, 0);
        } else {
            /* hover: the last pointer position the desktop reported sits on
             * this row. A tint, never a selection - selecting is the click.
             * f->px/f->py and row_rect() are both app-local. */
            int hov = f->px >= 0 && f->px < w && f->py >= ryt && f->py < ryt + rh;
            if (hov)
                gfx_rrect_aa(s, (float)x + (float)L->row_xpad,
                             ry + (float)L->row_ypad,
                             (float)(w - 2 * L->row_xpad),
                             (float)(rh - 2 * L->row_ypad), (float)L->radius,
                             color_alpha(th->text, 0.05f), 0, 0);
        }

        float ty = ry + (float)L->row_text_dy;
        draw_entry_icon(s, e, (float)(x + L->pad),
                        ry + (float)(rh - L->icon_sz) * 0.5f,
                        (float)L->icon_sz, sel, th);

        /* name, with a trailing '/' on directories (a symlink to a folder
         * gets one too: it opens like a folder) */
        char name[NAME_LEN + 2];
        snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
        draw_clipped(s, L->fl, (float)(x + L->name_x), ty, (float)L->name_w, name,
                     sel ? th->text : color_alpha(th->text, 0.72f));

        if (L->show_size && !e->is_dir) {
            char sz[24];
            human_size(e->size, sz, sizeof(sz));
            float sw = font_width(L->fl, sz);
            font_draw(s, L->fl, (float)(x + L->size_right) - sw, ty, sz,
                      color_alpha(th->text, sel ? 0.85f : 0.50f));
        }

        if (L->show_time) {
            char ts[32];
            fmt_mtime(e->mtime, ts, sizeof(ts));
            float tw = font_width(L->fl, ts);
            font_draw(s, L->fl, (float)(x + L->time_right) - tw, ty, ts,
                      color_alpha(th->text, sel ? 0.62f : 0.36f));
        }
    }

    /* Scrollbar: the thumb's height and position are the real
     * rows/scroll/count, so it is a map of the list, not decoration. It
     * lives in the right padding so it can never cross a column. */
    if (L->rows > 0 && f->count > L->rows) {
        int bar_w = L->bar_w;
        int band = L->list_bot - L->list_y;
        int bx = x + w - bar_w - L->bar_pad;
        if (bx > x) {
            int thumb = band * L->rows / f->count;
            if (thumb < L->thumb_min) thumb = L->thumb_min;
            if (thumb > band) thumb = band;
            int span = band - thumb;
            int maxs = f->count - L->rows;
            int off = maxs > 0 ? span * f->scroll / maxs : 0;
            gfx_rrect(s, (float)bx, (float)(y + L->list_y), (float)bar_w,
                      (float)band, (float)bar_w * 0.5f,
                      color_alpha(th->text, 0.06f));
            gfx_rrect(s, (float)bx, (float)(y + L->list_y + off), (float)bar_w,
                      (float)thumb, (float)bar_w * 0.5f,
                      color_alpha(th->accent, 0.55f));
        }
    }
}

/* One line about the selected entry: its name, the size stat() reported -
 * for a directory too, because ext4 gives a directory a real st_size - and
 * the kind word. A message from the last action owns the line when there is
 * one. Split out of draw_footer() so the host harness can assert on the
 * exact words the owner reads instead of a second copy of this snprintf. */
static void footer_info(const Files *f, char *out, size_t n) {
    out[0] = 0;
    if (f->status[0]) { snprintf(out, n, "%s", f->status); return; }
    if (f->sel < 0 || f->sel >= f->count) return;

    const Entry *e = &f->ent[f->sel];
    char sz[24];
    human_size(e->size, sz, sizeof(sz));
    const char *tgt = e->target[0] ? e->target : "?";

    if (e->is_link && !e->link_ok)
        /* nothing honest to report but the dead pointer itself: the link's
         * own size is the length of the text inside it, not a file size */
        snprintf(out, n, "%s -> %s (broken link)", e->name, tgt);
    else if (e->is_link && e->is_dir)
        snprintf(out, n, "%s/ -> %s  %s  folder", e->name, tgt, sz);
    else if (e->is_link)
        snprintf(out, n, "%s -> %s  %s", e->name, tgt, sz);
    else if (e->is_dir)
        snprintf(out, n, "%s/  %s  folder", e->name, sz);
    else
        snprintf(out, n, "%s  %s", e->name, sz);
}

static void draw_footer(Files *f, Surface *s, int x, int y, int w,
                        const Layout *L, const Theme *th) {
    if (L->footer_h <= 0) return;
    int fy = y + L->list_bot;
    surf_blend(s, x, fy, w, L->footer_h, th->panel);
    if (L->footer_h > L->rule)
        surf_blend(s, x, fy, w, L->rule, th->panel_border);

    float ty = (float)fy + (float)L->footer_text_dy;

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

    float minfo = (float)(x + L->pad) + lw + (float)L->foot_gap;
    float maxw = (float)(x + w - L->pad) - rw - (float)L->foot_colgap - minfo;
    if (maxw < (float)L->foot_mininfo) return;

    char info[NAME_LEN + LINK_LEN + 64];
    footer_info(f, info, sizeof(info));
    u32 col = f->status[0] ? th->accent : color_alpha(th->text, 0.55f);
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

    /* toolbar: tb_btn_at() walks the rectangles tb_rect() handed the
     * painter, so a click can only land on a button that was drawn. */
    {
        int hit = tb_btn_at(&L, cx, cy);
        if (hit >= 0) {
            f->pressed = hit;
            f->pressed_t = mono_now();
            toolbar_action(f, hit);
            return;
        }
    }

    /* breadcrumbs */
    int cy0 = L.toolbar_h;
    if (cy >= cy0 && cy < cy0 + L.crumb_h) {
        Crumb cr[MAX_CRUMBS];
        int dots = 0;
        int n = crumb_build(f, &L, cr, &dots);
        for (int i = 0; i < n; i++) {
            float pill_x, pill_w;
            crumb_pill(&L, &cr[i], &pill_x, &pill_w);
            /* the same clamped span the painter filled - a click in the
             * right gutter is not this crumb */
            if (pill_w <= 0.0f || (float)cx < pill_x ||
                (float)cx >= pill_x + pill_w) continue;
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

    /* rows: list_row_at() is the inverse of the row_rect() the painter
     * used, so the row under the pointer is the row that was drawn there */
    {
        int row = list_row_at(&L, cy);
        if (row < 0) return;
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

/* The window's wheel hook (AppVT.wheel). Positive notches scroll up, as
 * the desktop delivers them. The view moves under the pointer by whole
 * rows; the highlighted row is pulled to the nearest edge when the view
 * passes it, so the selection is always on screen. Like an arrow key,
 * the wheel only moves - it never opens the row it lands on. */
static void files_wheel(App *a, int notches) {
    Files *f = a->state;
    if (!f || notches == 0 || f->count <= 0) return;

    Layout L;
    if (!layout_build(&L, f->w, f->h) || L.rows <= 0) return;

    int maxs = f->count - L.rows;
    if (maxs < 0) maxs = 0;
    int want = f->scroll - notches * WHEEL_ROWS;
    if (want < 0) want = 0;
    if (want > maxs) want = maxs;
    if (want == f->scroll) return;

    f->scroll = want;
    if (f->sel < f->scroll) f->sel = f->scroll;
    if (f->sel >= f->scroll + L.rows) f->sel = f->scroll + L.rows - 1;
    f->status[0] = 0;
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
    f->pressed = -1;
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
    .click = files_click, .resize = files_resize,
    .wheel = files_wheel, .free = files_free,
};
