/* ============================================================
 *  Mystic OS - apps/files.c
 *  "Files": a real file manager, not a list of names.
 *
 *  What is on screen, top to bottom:
 *
 *    Files                                        [New folder]
 *    <- ->   /  ›  usr  ›  share  ›  fonts
 *    [icon] name................................  12.4 KB  2024-11-17 10:59
 *    ...
 *    N items                                              free 1.8 GB
 *
 *  Every band is derived from the font that draws in it (see
 *  layout_build()), so the global text scale - applied inside
 *  font_get() in src/font.c - grows the header, the path band, the rows
 *  and the footer together. layout_build() is the one and only source of
 *  those numbers: one Layout holds the outer inset, every gap, the row
 *  height, the icon size, the corner radius and the right edge of each
 *  column. files_draw() paints from it and files_click()/files_wheel()
 *  hit-test and scroll with it, so a click always lands on the row that
 *  was painted. Each line is centred in its band from ui_top(), never
 *  with a guessed baseline. The three fonts are the shared faces in the
 *  ONE VISUAL LANGUAGE block: semibold 15 title, regular 11 body,
 *  medium 9 small.
 *
 *  ICONS are drawn geometry, in the same style as the desktop
 *  launcher's icon_folder()/icon_pencil() in src/main.c - no images.
 *  An SP(2) box (capped to the row height) holds one of nine marks,
 *  chosen once per entry from the real name and st_mode:
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
 *  The window is four bands on one grid (see Layout): a header with the
 *  app's name and exactly one action (New folder), a path band with back,
 *  up and the real path as clickable crumbs, the list, and one status
 *  line. Every padding and gap is a multiple of the 8 px unit in SP();
 *  the three fonts are the shared title/body/small faces, and every row
 *  is ui_row_h() tall. Hover and selection are the same rectangle with a
 *  different colour, so nothing moves when the pointer does.
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
 *         450 ms (measured with CLOCK_MONOTONIC) opens it; the header
 *         action, Back, Up and every breadcrumb segment are clickable.
 *  WHEEL  the window's wheel hook scrolls the list by WHEEL_ROWS rows a
 *         notch (positive = up, toward the first entry). The highlighted
 *         row is pulled to the nearest edge when the view moves past it,
 *         so the selection can never be scrolled out of sight.
 *
 *  COLUMNS every value ends on the one right edge. The modified-time
 *         column is dropped first and the size column second, each when
 *         the remaining name column would be narrower than SP(8) - so a
 *         narrow window keeps the name and loses dates, never the other
 *         way round, and two columns can never overlap.
 *
 *  AppVT hands an app the pointer position only together with a click,
 *  so the hover mark follows the last pointer position the app was told
 *  about - it is a real position, never a guess about where the mouse
 *  might be now.
 *
 *  ------------------------------------------------------------
 *  RIGHT CLICK (the reason this file grew)
 *
 *  src/input.c maps BTN_RIGHT to bit 2 of `buttons` and src/main.c now
 *  dispatches a right press to the window body, so AppVT.click() sees 2.
 *  A right click on a row opens a real menu: Open, Open with..., Rename,
 *  Copy, Cut, Paste, Delete, New folder, Properties. A right click on
 *  empty space (or the footer) opens: New folder, Paste, Refresh,
 *  Sort by (Name/Size/Modified), Show hidden, and Undo delete when the
 *  last delete can still be undone. Every item performs the syscall it
 *  names - each handler below is the one that does it. A menu is measured
 *  from the fonts before it is placed, then clamped to the window rect
 *  (menu_measure/menu_place), so it can never paint on the desktop
 *  outside the window; if the window is too short for every row the box is
 *  capped and the bands that do not fit are cut (zero height, unreachable),
 *  so the painted menu, the hit test and the keyboard walk the same set of
 *  items. Up/Down/Home/End move the highlight, Right/Left open and close a
 *  submenu, Enter activates, Esc and a click elsewhere dismiss. A click on
 *  the menu's own padding or on the rule between two items is swallowed:
 *  it dismisses nothing and activates nothing, the way a real menu behaves.
 *  A press outside the rect the app was given (the desktop forwards a right
 *  press on a title bar as a negative y) belongs to the desktop and is
 *  ignored, so the list never opens a menu above its own window.
 *
 *  The menu has no hover highlight from the mouse: the AppVT only reports
 *  the pointer together with a press, so the item under the opening click
 *  gets the highlight and the arrow keys move it after that. That is the
 *  honest amount of pointer information the app is given.
 *
 *  CLIPBOARD  copy/cut store the real paths of the selection; paste
 *  copies files with open/read/write/fchmod/utimensat and directories
 *  with mkdir + recursion (symlinks with readlink/symlink), then reports
 *  the first real strerror(errno) if anything failed. Cut pastes with
 *  rename(2) and falls back to copy + remove across filesystems.
 *
 *  DELETE is two-step: a confirmation panel, then each path is renamed
 *  into /root/.local/share/mystic/trash (created with mkdir -p), which is
 *  what makes Ctrl+Z / "Undo delete" able to put it back with rename(2).
 *  A path on another filesystem is copied into the trash and removed from
 *  its old place, so it is undoable too.
 *
 *  MULTI-SELECTION  the cursor (f->sel) and the marked set are separate.
 *  A click carries left/right/middle plus the modifier bits the desktop
 *  ORs in (0x100 shift, 0x200 ctrl, src/main.c's app_click call), so
 *  Ctrl+click adds or removes one row and Shift+click marks a range from
 *  the anchor; middle click toggles a row, Space toggles the cursor row,
 *  Ctrl+A marks everything, and 'v' enters a range mode in which j/k
 *  extend the range (the keyboard equivalent of Shift+click). The footer
 *  prints the real count. Every marked row is painted with the accent
 *  pill, and the cursor row with a stronger one - see draw_list().
 *
 *  RENAME / NEW FOLDER edit in place: the row itself turns into a text
 *  field (Enter commits with rename(2)/mkdir(2), Esc cancels); errors
 *  keep the field open and print strerror(errno).
 *
 *  PROPERTIES is a panel of lstat(2) fields: size, mode in octal plus the
 *  rwx string, owner and group names from getpwuid_r/getgrgid_r (numeric
 *  when the lookup fails), mtime, inode, blocks and link count. Open on a
 *  regular file reads up to 4 KiB and shows a clipped preview (open/read/
 *  close); a directory opens instead.
 * ============================================================ */
#include "mystic.h"
#include "clipboard.h"
#include "filetypes.h"
#include "i18n.h"      /* font_cell_width / UTF-8 helpers, shared with font.c */

#ifndef OPEN_REQ
#define OPEN_REQ "/run/mystic/open.request"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <math.h>
#include <pwd.h>
#include <stdarg.h>
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
#define CLIP_MAX    64     /* paths one copy/cut can hold */
#define UNDO_MAX    32     /* the last delete batch, restorable with rename */
#define MENU_MAX    16
#define PREVIEW_MAX 4096
#define COPY_BUF    32768
#define DEPTH_MAX   24     /* recursion cap for copy/remove (real dirs are shallow) */

/* Where a delete puts things. Overridable so a host harness can point it at
 * a scratch tree instead of the real /root; the OS build uses this path. */
#ifndef TRASH_DIR
#define TRASH_DIR    "/root/.local/share/mystic/trash"
#endif

/* Where app bundles and the launch request live. Guarded the same way as
 * TRASH_DIR so a host harness can point them at a scratch tree. */
#ifndef APP_DIR_ONE
#define APP_DIR_ONE  "/usr/share/mystic/apps"
#endif
#ifndef APP_DIR_TWO
#define APP_DIR_TWO  "/root/apps"
#endif
#ifndef LAUNCH_REQ
#define LAUNCH_REQ   "/run/mystic/launch.request"
#endif

/* the buttons mask, as src/input.c builds it, plus the modifier bits the
 * desktop ORs into it at the app_click() call site (src/main.c). Before
 * that the bits never arrived, so Ctrl+click and Shift+click could not
 * work in the shipped desktop even though this file handled them; the
 * keyboard and middle-click paths below stay, because they are the ones
 * that work over the client IPC protocol too. */
#define BTN_LEFT    1
#define BTN_RIGHT   2
#define BTN_MIDDLE  4
#define MOD_SHIFT   0x100
#define MOD_CTRL    0x200

/* sort modes, cycled by the toolbar button and by 's' */
enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_TIME = 2, SORT_MODES = 3 };

/* Which drawn icon an entry gets - decided once per entry, at scan time,
 * from the real name and st_mode (see kind_of()). Nine marks: folder, plain
 * page, executable, image, config, theme, archive, text, symlink. */
enum { IC_FOLDER, IC_FILE, IC_EXEC, IC_IMAGE, IC_CONF, IC_ARCHIVE, IC_TEXT, IC_LINK,
       IC_THEME };

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

/* ============================================================
 *  ONE VISUAL LANGUAGE
 *
 *  This block is identical, byte for byte, in files.c, store.c and
 *  settings.c: the owner asked for one desktop, not three
 *  improvisations. Change it in all three at once.
 *
 *  SPACING. One unit, 8 px, scaled by ui_px() so it follows the global
 *  text scale. Every padding and every gap in this window is SP(n);
 *  there is no other kind of space.
 *
 *  TYPE. Exactly three sizes, and the same three faces in all three
 *  windows: semibold 15 is a title, regular 11 is body text, medium 9
 *  is small print. No other font_get() call is allowed.
 *
 *  ROWS. Every list row is ui_row_h(): the body line box plus one unit
 *  of leading. Hover and selection are the same rectangle with a
 *  different colour, so a row never grows, shrinks or gains a border
 *  when the pointer moves over it.
 * ============================================================ */
#define UI_UNIT 8
static int SP(int n) { return ui_px(n * UI_UNIT); }

typedef struct { Font *title, *body, *small; } UiFont;

static UiFont ui_fonts(void) {
    UiFont f;
    f.title = font_get(FONT_SEMIBOLD, 15);
    f.body  = font_get(FONT_REGULAR,  11);
    f.small = font_get(FONT_MEDIUM,    9);
    return f;
}

/* The ink of one line is font_height()/1.4 tall (src/font.c:119), and
 * centring that box - not the line box, which includes leading - is what
 * puts caps and descenders equally inside a band. No guessed baseline. */
static int ui_top(Font *f, int band) {
    int ink;
    if (!f || band <= 0) return 0;
    ink = (int)(font_height(f) / 1.4f + 0.5f);
    if (ink >= band) return 0;
    return (band - ink) / 2;
}

static int ui_ceilf(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* the one row height: body line box + one spacing unit */
static int ui_row_h(Font *body) {
    int h = ui_ceilf(font_height(body)) + SP(1);
    return h < SP(3) ? SP(3) : h;
}

/* A filled rounded rectangle: the only shape hover and selection use. */
static void ui_pill(Surface *s, int x, int y, int w, int h, float r, u32 c) {
    if (w <= 0 || h <= 0) return;
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, r, c, 0, 0);
}

/* A value on the shared right edge. */
static void ui_text_right(Surface *s, Font *f, int right, int y, const char *t, u32 c) {
    if (!f || !t || !t[0]) return;
    font_draw(s, f, (float)right - font_width(f, t), (float)y, t, c);
}

static void text_fit(char *buf, size_t cap, Font *f, const char *text, float maxw);

/* Same edge, but fitted to the column it owns: a size from a file (or a
 * GB-scale number) can be wider than the stand-in the column was sized
 * from, and it must be cut rather than painted over the stamp/name cell. */
static void ui_text_right_fit(Surface *s, Font *f, int right, int y, float maxw,
                              const char *t, u32 c) {
    char buf[64];
    text_fit(buf, sizeof(buf), f, t, maxw);
    if (!buf[0]) return;
    font_draw(s, f, (float)right - font_width(f, buf), (float)y, buf, c);
}

/* One button: the header action and the dialog buttons share it, so a
 * button looks the same wherever it appears. */
static void ui_button(Surface *s, int x, int y, int w, int h, float r,
                      const char *label, Font *f, int primary, const Theme *th) {
    u32 fill = primary ? color_alpha(th->accent, 0.30f)
                       : color_alpha(th->text, 0.07f);
    u32 bord = primary ? color_alpha(th->accent, 0.85f)
                       : color_alpha(th->panel_border, 0.85f);
    if (w <= 0 || h <= 0 || !f) return;
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, r, fill, bord, 1.0f);
    float tw = font_width(f, label);
    font_draw(s, f, (float)x + ((float)w - tw) * 0.5f,
              (float)y + (float)ui_top(f, h), label,
              primary ? th->text : color_alpha(th->text, 0.88f));
}

/* Geometry for one draw, in app-local coordinates (0,0 = top-left of the
 * rect the app was given). Built from the fonts above, never stored as a
 * constant, and rebuilt identically by the click handler. */
typedef struct {
    UiFont f;                   /* the only three fonts in this window */

    int m, gap;                 /* SP(2) outer inset, SP(1) between things */
    int header_h, path_h, footer_h;
    int row_h, icon_sz, rows;
    int list_y, list_bot;

    /* header: the app name, and one action button right-aligned on the
     * same margin as the values in the rows */
    int title_x, title_dy;
    int act_x, act_y, act_w, act_h, act_label;

    /* path bar: back, up, then the real path, all on the row grid */
    int nav_sz, back_x, up_x, path_x, path_r, path_dy;
    int pill_y, pill_h;

    /* one row grid: name on the left, values right-aligned on one edge */
    int name_x, name_w, row_dy, small_dy;
    int size_right, size_col;
    int time_right, time_col;
    int show_time, show_size;

    int row_xpad, row_ypad;
    int footer_dy;
    int radius, rule, bar_w, bar_pad, thumb_min;
    int chev_sz, chev_cell;
} Layout;


/* ------------------------------------------------------------
 *  The clipboard: real paths, captured at Copy/Cut time. Nothing here
 *  borrows the desktop's state; paste acts on these strings alone.
 * ------------------------------------------------------------ */
typedef struct {
    char          path[PATH_LEN];
    char          name[NAME_LEN];
    unsigned char is_dir;
} ClipItem;

typedef struct {
    ClipItem item[CLIP_MAX];
    int      n;
    int      cut;          /* 1 = move on paste, 0 = copy */
} Clipboard;

/* ------------------------------------------------------------
 *  One context-menu entry. y0/y1 are filled by menu_measure(), so the
 *  painter and the hit test walk exactly the same rectangles.
 * ------------------------------------------------------------ */
typedef struct {
    char label[64];
    char shortcut[16];
    char arg[40];          /* launch id for MA_LAUNCH */
    int  action;
    int  y0, y1;           /* item band, relative to the menu top */
    unsigned char sep_before, enabled, checked, submenu;
} MenuItem;

typedef struct {
    int      open;
    int      x, y, w, h;   /* placed, app-local, guaranteed inside the window */
    int      focus;        /* keyboard highlight, -1 = none */
    int      row;          /* list index this menu was opened on, -1 = empty space */
    int      n;
    int      pending_sep;
    /* metrics fixed by menu_measure() and used by the painter and hit test */
    int      pad, check_w, arrow_w, row_h, sep_h;
    MenuItem item[MENU_MAX];
} Menu;

/* What a context menu can do. Every one of these ends in a real call. */
enum {
    MA_NONE = 0, MA_OPEN, MA_OPENWITH, MA_RENAME, MA_COPY, MA_CUT, MA_PASTE,
    MA_DELETE, MA_NEWFOLDER, MA_PROPS, MA_REFRESH, MA_SORT_NAME, MA_SORT_SIZE,
    MA_SORT_TIME, MA_SHOW_HIDDEN, MA_UNDO, MA_CLOSE, MA_LAUNCH
};

/* ------------------------------------------------------------
 *  Inline text field: one row of the list becomes editable for Rename,
 *  and a floating row appears at the top of the list for New folder.
 * ------------------------------------------------------------ */
enum { TI_NONE = 0, TI_RENAME, TI_MKDIR };

typedef struct {
    int  mode;
    int  row;              /* TI_RENAME: list index being renamed */
    char buf[NAME_LEN];
    int  len;
    int  caret;
} TextInput;

/* one undo record: where the delete put it, and where it came from */
typedef struct {
    char trash[PATH_LEN];
    char orig[PATH_LEN];
} Undo;

/* the lstat(2) fields the Properties panel prints */
typedef struct {
    char          name[NAME_LEN];
    unsigned      type;        /* S_IFMT of st_mode */
    unsigned      mode;        /* st_mode */
    unsigned long long size, blocks, inode, mtime, nlink;
    unsigned      uid, gid;
    int           has_target;
    char          target[LINK_LEN];
} PropInfo;

enum { DLG_NONE = 0, DLG_DELETE, DLG_PROPS, DLG_PREVIEW };

typedef struct {
    char   path[PATH_LEN];
    char   error[128];            /* strerror(errno) from a failed opendir */
    char   status[200];           /* one-line answer to the last action */
    Entry *ent;                   /* MAX_ENTRIES, calloc'd once */
    int    count, sel, scroll;
    int    truncated;             /* more than MAX_ENTRIES entries */
    int    sort;
    int    show_hidden;           /* dotfiles in the listing */
    int    w, h;
    int    px, py;                /* last pointer position reported to us */
    /* Keyboard focus. The list owns the keys by default; Tab hands them to the
     * two navigation buttons and then to the header action, so everything a
     * mouse can reach has a key path and the focused control wears a visible
     * ring (draw_focus()). 0 = list, 1 = back, 2 = up, 3 = header action. */
    int    focus;
    int    kb;                    /* a key moved the focus: show the list ring */
    double last_click_t;          /* CLOCK_MONOTONIC of the last row click */
    int    last_click_idx;
    int    last_click_x, last_click_y;
    char   hist[HIST_MAX][PATH_LEN];
    int    hist_n;
    unsigned long long free_bytes;
    int    has_free;

    /* selection: marks parallel to ent[], the cursor, and the range anchor */
    unsigned char *marked;
    int    nmarked;
    int    anchor;
    int    mark_mode;             /* 'v': j/k extend the marked range */

    Clipboard clip;
    Menu      menu;               /* row or empty-space menu */
    Menu      sub;                /* Open with... / Sort by... */
    TextInput ti;
    int       dlg;                /* DLG_* */
    char      dlg_title[96];
    char      dlg_msg[200];
    int       delete_count;       /* items the open confirmation refers to */
    int       prop_scroll;        /* first Properties line drawn */
    PropInfo  prop;
    char      preview[PREVIEW_MAX + 1];
    int       preview_len, preview_binary, preview_scroll;
    Undo      undo[UNDO_MAX];
    int       undo_n;
} Files;

/* A box that must *contain* a string rounds up: rounding a text width down by
 * half a pixel is enough for draw_clipped() to cut the last letter. */
static int iceil(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* Shorten a buffer that snprintf() filled so it ends on a character boundary.
 * A name longer than the field it was copied into used to leave half a
 * codepoint at the end, which the font then drew as one replacement glyph. */
static void cut_utf8(char *s) {
    if (!s) return;
    size_t n = strlen(s), k = font_utf8_boundary(s, n);
    if (k < n) s[k] = 0;
}


static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Any navigation or action that changes what is on screen closes every
 * overlay: a menu anchored to a row that no longer exists, or an editor for
 * a name that just disappeared, would otherwise act on stale indices. */
static void overlays_close(Files *f) {
    f->menu.open = 0;
    f->sub.open = 0;
    f->ti.mode = TI_NONE;
    f->dlg = DLG_NONE;
}

static void statusf(Files *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->status, sizeof(f->status), fmt, ap);
    va_end(ap);
    cut_utf8(f->status);
}

/* Report the last syscall failure with the real string the kernel returned. */
static void status_errno(Files *f, const char *what, const char *path) {
    int e = errno;
    const char *why = (e != 0) ? strerror(e) : "failed";
    if (path && path[0])
        statusf(f, "%s %s: %s", what, path, why);
    else
        statusf(f, "%s: %s", what, why);
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
    /* The 75-format registry (src/filetypes.c) is the authority when it knows
     * the name: one table decides a type everywhere in the OS. The old
     * extension lists below stay as the fallback for anything it misses. */
    {
        const FileType *t = filetype_for(e->name);
        if (t && t->kind != FT_KIND_UNKNOWN) {
            switch (t->kind) {
                case FT_KIND_IMAGE:      return IC_IMAGE;
                case FT_KIND_VIDEO:      return IC_IMAGE;
                case FT_KIND_AUDIO:      return IC_IMAGE;
                case FT_KIND_TEXT:       return IC_TEXT;
                case FT_KIND_DOCUMENT:   return IC_TEXT;
                case FT_KIND_ARCHIVE:    return IC_ARCHIVE;
                case FT_KIND_CODE:       return IC_TEXT;
                case FT_KIND_DATA:       return IC_FILE;
                case FT_KIND_EXECUTABLE: return IC_EXEC;
                default: break;
            }
        }
    }
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

/* @utf8-fit-begin  (the host harness in .work/i18n compiles exactly this text) */
/* draw inside maxw pixels, ending in ".." when it has to be cut.
 * The half-pixel tolerance matters: a column width is computed as
 * (x0 + width) - x0, which in floating point can come out a few
 * thousandths of a pixel smaller than the width it started from - enough
 * to chop the last letter off a label that fits perfectly.
 *
 * IMPORTANT: every cut lands on a CODEPOINT boundary. The loop used to
 * shorten the string one byte at a time, so a Cyrillic or Japanese name cut
 * mid-character left a bare lead byte before the dots; font_draw() then read
 * that byte plus the first '.' as one 2-byte sequence, drew a wrong glyph and
 * ate the dots, and the rest of the line shifted with it. font_utf8_prev()
 * steps back whole characters, so the string handed to the font is always
 * valid UTF-8. */
static void text_fit(char *buf, size_t cap, Font *f, const char *text, float maxw) {
    if (cap < 4) { if (cap) buf[0] = 0; return; }
    buf[0] = 0;
    if (!f || !text || !text[0] || maxw < 4.0f) return;
    size_t len = strlen(text);
    if (len >= cap - 3) len = font_utf8_boundary(text, cap - 3);
    memcpy(buf, text, len);
    buf[len] = 0;
    if (font_width(f, buf) <= maxw + 0.5f) return;
    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0 && font_width(f, buf) > budget) {
        cut = font_utf8_prev(buf, cut);
        buf[cut] = 0;
    }
    if (cut == 0) { buf[0] = 0; return; }
    memcpy(buf + cut, "..", 3);
    buf[cut + 2] = 0;
}

/* @utf8-fit-end */

static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    char buf[PATH_LEN + 8];
    text_fit(buf, sizeof(buf), f, text, maxw);
    if (buf[0]) font_draw(s, f, x, y, buf, color);
}

/* ------------------------------------------------------------
 *  Layout - the single source of every vertical and horizontal number
 * ------------------------------------------------------------ */
static int layout_build(Layout *L, int w, int h) {
    memset(L, 0, sizeof(*L));
    if (w < 1 || h < 1) return 0;

    L->f = ui_fonts();
    if (!L->f.title || !L->f.body || !L->f.small) return 0;

    L->m = SP(2);
    L->gap = SP(1);
    L->rule = ui_px(1);
    L->radius = SP(1);
    L->bar_w = ui_px(3);
    L->bar_pad = 0;                  /* the bar lives in the right margin */
    L->thumb_min = SP(2);
    L->chev_sz = SP(1);
    L->chev_cell = SP(2);
    L->row_xpad = L->m;
    L->row_ypad = 0;

    /* one row height for the whole window, from the body font */
    L->row_h = ui_row_h(L->f.body);
    L->icon_sz = SP(2);
    if (L->icon_sz > L->row_h - SP(1)) L->icon_sz = L->row_h - SP(1);
    if (L->icon_sz < SP(1)) L->icon_sz = SP(1);

    L->header_h = ui_ceilf(font_height(L->f.title)) + 2 * SP(1);
    L->path_h = ui_ceilf(font_height(L->f.body)) + 2 * SP(1);
    L->footer_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);

    /* A short window gives up the footer, then the path bar. The header is
     * the app's name and its one action, so it is the last to go. */
    if (L->header_h + L->path_h + L->footer_h + L->row_h > h) L->footer_h = 0;
    if (L->header_h + L->path_h + L->row_h > h) L->path_h = 0;
    if (L->header_h + L->row_h > h) L->header_h = h;

    /* header: title on the left, one action right-aligned on the margin */
    L->title_x = L->m;
    L->title_dy = ui_top(L->f.title, L->header_h);
    L->act_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);
    if (L->act_h > L->header_h - SP(1)) L->act_h = L->header_h - SP(1);
    if (L->act_h < SP(2)) L->act_h = SP(2);
    L->act_y = (L->header_h - L->act_h) / 2;
    {
        int labw = ui_ceilf(font_width(L->f.small, "New folder"));
        int full = labw + 2 * SP(1);
        int room = w - 2 * L->m - ui_ceilf(font_width(L->f.title, "Files"))
                 - 2 * L->gap;
        L->act_label = (full <= room);
        L->act_w = L->act_label ? full : L->act_h;
        if (L->act_w > w - 2 * L->m) L->act_w = w - 2 * L->m;
        L->act_x = w - L->m - L->act_w;
        if (L->act_x < L->m) L->act_x = L->m;
    }

    /* path bar: two navigation targets, then the real path */
    L->nav_sz = SP(3);
    if (L->nav_sz > L->path_h) L->nav_sz = L->path_h;
    L->back_x = L->m;
    L->up_x = L->back_x + L->nav_sz + L->gap;
    L->path_x = L->up_x + L->nav_sz + L->gap;
    L->path_r = w - L->m;
    if (L->path_x > L->path_r) L->path_x = L->path_r;
    L->path_dy = ui_top(L->f.body, L->path_h);
    L->pill_h = ui_ceilf(font_height(L->f.body));
    L->pill_y = (L->path_h - L->pill_h) / 2;
    if (L->pill_y < 0) L->pill_y = 0;

    /* bands: one unit of air between the path rule and the first row */
    L->list_y = L->header_h + L->path_h + (L->path_h > 0 ? SP(1) : 0);
    L->list_bot = h - L->footer_h;
    if (L->list_bot < L->list_y) L->list_bot = L->list_y;
    {
        int avail = L->list_bot - L->list_y;
        L->rows = avail > 0 ? avail / L->row_h : 0;
        if (L->rows > MAX_ENTRIES) L->rows = MAX_ENTRIES;
    }
    L->row_dy = ui_top(L->f.body, L->row_h);
    L->small_dy = ui_top(L->f.small, L->row_h);
    L->footer_dy = L->footer_h > 0 ? ui_top(L->f.small, L->footer_h) : 0;

    /* columns: every value ends on the one right edge, w - m */
    {
        int right = w - L->m;
        L->size_col = ui_ceilf(font_width(L->f.small, "999.9 MB"));
        if (L->size_col > SP(12)) L->size_col = SP(12);
        /* the stamp is drawn as "YYYY-MM-DD HH:MM" (fmt_mtime), so the
         * column is measured from that real string, not a short stand-in */
        L->time_col = ui_ceilf(font_width(L->f.small, "0000-00-00 00:00"));
        L->name_x = L->m + L->icon_sz + L->gap;
        L->size_right = right;
        L->time_right = right;
        L->show_time = (right - L->name_x >=
                        L->time_col + L->size_col + 3 * L->gap + SP(14));
        if (L->show_time)
            L->size_right = L->time_right - L->time_col - 2 * L->gap;
        L->name_w = L->size_right - L->size_col - L->gap - L->name_x;
        L->show_size = (L->name_w >= SP(10));
        if (!L->show_size) {
            /* no size column: the name still stops before the time column,
             * so dropping it can never make the two painted cells collide */
            if (!L->show_time) L->size_right = right;
            L->name_w = L->size_right - L->name_x;
        }
        if (L->name_w < SP(2)) L->name_w = SP(2);
    }
    return 1;
}

/* ------------------------------------------------------------
 *  The rectangles the painter and the hit-tester must agree on.
 *  There is exactly one copy of each: the draw code and files_click()
 *  both call these, so a click always lands on what was painted.
 * ------------------------------------------------------------ */

/* the header's one action button */
static int act_rect(const Layout *L, int *bx, int *by, int *bw, int *bh) {
    if (L->act_w <= 0 || L->act_h <= 0) return 0;
    *bx = L->act_x; *by = L->act_y; *bw = L->act_w; *bh = L->act_h;
    return 1;
}

/* back (0) and up (1) in the path bar */
static int nav_rect(const Layout *L, int which, int *bx, int *by, int *bw, int *bh) {
    if (L->path_h <= 0 || L->nav_sz <= 0) return 0;
    *bx = which ? L->up_x : L->back_x;
    *by = (L->path_h - L->nav_sz) / 2;
    *bw = *bh = L->nav_sz;
    return 1;
}

static int nav_at(const Layout *L, int cx, int cy) {
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        if (!nav_rect(L, i, &bx, &by, &bw, &bh)) continue;
        int py = L->header_h;
        if (cx >= bx && cx < bx + bw && cy >= py + by && cy < py + by + bh) return i;
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

/* ------------------------------------------------------------
 *  Keyboard focus: who owns the keys, and where the ring goes
 *
 *  Every control a mouse can hit already had a key path, but not one
 *  a user could SEE: Tab moved nothing, so the only thing a keyboard
 *  user could tell was where the list cursor was. The focus now walks
 *  list -> back -> up -> header action, and the control holding it is
 *  drawn with a ring (draw_focus()).
 * ------------------------------------------------------------ */
#define FOCUS_LIST   0
#define FOCUS_BACK   1
#define FOCUS_UP     2
#define FOCUS_ACTION 3

/* The rectangle the ring is drawn around, app-local. Returns 0 when the
 * control is not on screen in this layout (the path band is dropped in a very
 * short window). */
static int focus_rect(const Files *f, const Layout *L, int which,
                      int *rx, int *ry, int *rw, int *rh) {
    if (which == FOCUS_BACK || which == FOCUS_UP) {
        if (!nav_rect(L, which == FOCUS_BACK ? 0 : 1, rx, ry, rw, rh)) return 0;
        *ry += L->header_h;          /* nav_rect() is relative to the path band */
        return 1;
    }
    if (which == FOCUS_ACTION) return act_rect(L, rx, ry, rw, rh);
    /* the list: the ring is the selected row, which is where Enter acts */
    if (!row_rect(L, f->sel - f->scroll, ry, rh)) return 0;
    *rx = L->row_xpad;
    *rw = f->w - 2 * L->row_xpad;
    return 1;
}

/* Is this control present? The path band (and with it back/up) is the first
 * thing a short window gives up, so Tab has to skip what is not there. */
static int focus_exists(const Layout *L, int which) {
    if (which == FOCUS_BACK || which == FOCUS_UP) return L->path_h > 0 && L->nav_sz > 0;
    if (which == FOCUS_ACTION) return L->act_w > 0 && L->act_h > 0;
    return 1;
}

static int focus_next(const Layout *L, int from, int step) {
    for (int i = 0; i < 4; i++) {
        from = (from + step + 4) % 4;
        if (focus_exists(L, from)) return from;
    }
    return FOCUS_LIST;
}

/* The visible focus indicator: two device pixels of the theme's accent drawn
 * just outside the focused control. A colour change alone is not enough for a
 * user who cannot see the palette; the accent is the brightest colour in every
 * shipped theme (docs/THEMES.md measures it against the body colour), so the
 * ring is the highest-contrast mark the app can make. The list keeps its
 * selection pill and only gains the ring once a key has moved the focus, so a
 * mouse user's screen does not change. */
static void draw_focus(Surface *s, int x, int y, const Files *f,
                       const Layout *L, const Theme *th) {
    if (f->focus == FOCUS_LIST && !f->kb) return;
    int rx, ry, rw, rh;
    if (!focus_rect(f, L, f->focus, &rx, &ry, &rw, &rh)) return;
    if (rw <= 0 || rh <= 0) return;
    float pad = (float)L->rule;
    gfx_rrect_aa(s, (float)(x + rx) - pad, (float)(y + ry) - pad,
                 (float)rw + 2.0f * pad, (float)rh + 2.0f * pad,
                 (float)L->radius + pad, 0u,
                 color_alpha(th->accent, 0.90f), 2.0f);
}

/* ------------------------------------------------------------
 *  Two coordinate spaces, and the rule that keeps them apart
 *
 *  AppVT.draw() gets the app's ORIGIN on the compositor's surface
 *  (src/main.c: app_draw(cur->app, s, cx0, cy0, ...)) and paints with
 *  x + local. AppVT.click() gets APP-LOCAL coordinates (app_click(hit->app,
 *  lx - 1, ly - tb - 1, ...)), so the pointer the app stores is app-local.
 *
 *  Every rectangle in here - act_rect(), nav_rect(), row_rect(), the crumb
 *  spans from crumb_build() - is app-local as well. So a hit test compares
 *  the stored pointer DIRECTLY against those rectangles and the draw origin
 *  never enters it. Mixing the two (pointer vs x + rect) only looks right
 *  while the window happens to sit at 0,0: it kills hover as soon as the
 *  window is dragged right, and it lets a click on a row light up the
 *  breadcrumb band above it.
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 *  Breadcrumbs: built from the real path, laid out to fit the path bar
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

    float chev = (float)L->chev_cell;
    float wid[MAX_CRUMBS];
    float total = 0.0f;
    for (int k = 0; k < n; k++) {
        char lab[NAME_LEN];
        size_t len = (size_t)all[k].ll;
        if (len >= sizeof(lab)) {
            /* A segment longer than the buffer has to be cut on a character
             * boundary: a byte cut split a Cyrillic or Japanese folder name
             * and the breadcrumb drew a replacement glyph for the half of it
             * that survived. */
            memcpy(lab, f->path + all[k].ls, sizeof(lab) - 1);
            lab[sizeof(lab) - 1] = 0;
            len = font_utf8_boundary(lab, sizeof(lab) - 1);
        } else {
            memcpy(lab, f->path + all[k].ls, len);
        }
        lab[len] = 0;
        wid[k] = font_width(L->f.body, len ? lab : "/");
        total += wid[k];
        if (k) total += chev;
    }

    /* Everything is measured inside the path bar (path_x .. path_r), so
     * nothing can leave the window. Leading segments are dropped whole
     * until the line fits *with* the ellipsis that says so; the last
     * segment (the folder we are in) always survives. */
    float avail = (float)(L->path_r - L->path_x);
    int first = 0;
    float dots_w = font_width(L->f.body, "...") + (float)L->gap;
    while (total + dots_w > avail && first < n - 1) {
        total -= wid[first];
        if (first + 1 < n) total -= chev;
        first++;
        *dots = 1;
    }

    float x = (float)L->path_x + (*dots ? dots_w : 0.0f);
    int m = 0;
    for (int k = first; k < n; k++) {
        float x1 = x + wid[k];
        if (x > (float)L->path_r) x = (float)L->path_r;
        if (x1 > (float)L->path_r) x1 = (float)L->path_r;
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

/* One crumb's hover pill and click span, clipped to the path bar. The
 * painter fills exactly this rectangle and the hit-test compares the
 * pointer with exactly this rectangle, so the two cannot drift apart. */
static void crumb_pill(const Layout *L, const Crumb *c, float *px, float *pw) {
    float x0 = c->x0 - (float)L->gap;
    float x1 = c->x1 + (float)L->gap;
    if (x0 < (float)L->path_x) x0 = (float)L->path_x;
    if (x1 > (float)L->path_r) x1 = (float)L->path_r;
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

/* ------------------------------------------------------------
 *  Multi-selection
 *
 *  The cursor (f->sel) says where the keyboard is; the marks say what an
 *  action acts on. They are deliberately not the same thing: a click on a
 *  row that is already marked moves the cursor without dropping the set,
 *  which is what lets a set be assembled without a modifier key - the
 *  AppVT carries only the three buttons, not Ctrl or Shift (see the note
 *  in the file header). Everything that operates on "the selection" goes
 *  through selection_paths(), so the menu, the clipboard and Delete can
 *  never disagree about it.
 * ------------------------------------------------------------ */
static void marks_reset(Files *f) {
    if (f->marked) memset(f->marked, 0, (size_t)(f->count > 0 ? f->count : 1));
    f->nmarked = 0;
    f->anchor = -1;
    f->mark_mode = 0;
}

static int mark_at(const Files *f, int i) {
    return f->marked && i >= 0 && i < f->count && f->marked[i];
}

static void mark_set(Files *f, int i, int on) {
    if (!f->marked || i < 0 || i >= f->count) return;
    unsigned char want = (unsigned char)(on ? 1 : 0);
    if (f->marked[i] == want) return;
    f->marked[i] = want;
    if (on) f->nmarked++;
    else if (f->nmarked > 0) f->nmarked--;
}

static void mark_all(Files *f) {
    for (int i = 0; i < f->count; i++) mark_set(f, i, 1);
    if (f->count > 0) f->anchor = 0;
}

static void mark_range(Files *f, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = 0; i < f->count; i++) mark_set(f, i, i >= a && i <= b);
    f->anchor = a;
}

static void mark_toggle(Files *f, int i) {
    if (i < 0 || i >= f->count) return;
    mark_set(f, i, !mark_at(f, i));
    f->anchor = i;
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
        /* dotfiles are real entries and are listed on request; the two
         * pseudo-entries above never are */
        if (de->d_name[0] == '.' && !f->show_hidden) continue;
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
    /* the marks are indices into ent[]; a rescan may have reordered or
     * removed entries, so they cannot survive it */
    marks_reset(f);
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
    overlays_close(f);
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

/* Set a sort mode by name, keeping the cursor on the same entry. Used by
 * 's', the toolbar's sort button and the Sort by submenu, so all three go
 * through one implementation. */
static void sort_set(Files *f, int mode) {
    char keep[NAME_LEN];
    keep[0] = 0;
    if (f->sel >= 0 && f->sel < f->count)
        snprintf(keep, sizeof(keep), "%s", f->ent[f->sel].name);
    f->sort = mode;
    sort_entries(f);
    f->sel = 0;
    f->scroll = 0;
    marks_reset(f);      /* the indices mean different entries after a sort */
    if (keep[0])
        for (int i = 0; i < f->count; i++)
            if (!strcmp(f->ent[i].name, keep)) { f->sel = i; break; }
    statusf(f, "sorted by %s", sort_label(f->sort));
}

static void cycle_sort(Files *f) {
    sort_set(f, (f->sort + 1) % SORT_MODES);
}

static void preview_open(Files *f, int row);

static void join_path(const char *dir, const char *name, char *out);

static void open_selected(Files *f) {
    if (f->sel < 0 || f->sel >= f->count) {
        snprintf(f->status, sizeof(f->status), "nothing selected");
        return;
    }
    Entry *e = &f->ent[f->sel];
    if (!e->is_dir) {
        /* A directory descends; a FILE opens in whatever app the type
         * registry chose for it. Files cannot fork, so it leaves the path in
         * a request file and the desktop does the launch - the app is picked
         * on that side from src/filetypes.c, not by this process. */
        char full[PATH_LEN];
        join_path(f->path, e->name, full);
        if (!full[0]) { snprintf(f->status, sizeof(f->status), "path too long"); return; }
        FILE *fp = fopen(OPEN_REQ, "w");
        if (!fp) {
            /* no request channel: fall back to the in-window preview */
            preview_open(f, f->sel);
            return;
        }
        int wrote = fprintf(fp, "%s\n", full);
        int bad = (fclose(fp) != 0 || wrote < 0);
        if (bad) {
            status_errno(f, "cannot write", OPEN_REQ);
            preview_open(f, f->sel);
            return;
        }
        const FileType *t = filetype_for(e->name);
        snprintf(f->status, sizeof(f->status), "opening %s with %s", e->name,
                 t ? filetype_app(t) : "viewer");
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
 *  Paths, copies, deletes, previews, properties
 *
 *  Everything in this block is a real filesystem operation. The helpers
 *  return 0/-1 and leave errno from the failing call alone, so the caller
 *  can put the kernel's own strerror() in the status line; none of them
 *  invent a reason.
 * ------------------------------------------------------------ */
static void join_path(const char *dir, const char *name, char *out) {
    int n;
    if (!dir[0] || !strcmp(dir, "/")) n = snprintf(out, PATH_LEN, "/%s", name);
    else n = snprintf(out, PATH_LEN, "%s/%s", dir, name);
    if (n < 0 || n >= PATH_LEN) out[0] = 0;   /* signals the overflow to the caller */
}

static void entry_path(const Files *f, int row, char *out) {
    out[0] = 0;
    if (row < 0 || row >= f->count) return;
    join_path(f->path, f->ent[row].name, out);
}

/* The paths an action applies to. When the row the menu was opened on is
 * part of the marked set, the whole set is the selection; a row outside it
 * (or a keyboard action, row < 0) means just the cursor when nothing is
 * marked. */
static int selection_paths(Files *f, int row, char out[][PATH_LEN], int max) {
    int n = 0;
    int use_marks = (f->nmarked > 0) && (row < 0 || mark_at(f, row));
    if (use_marks) {
        for (int i = 0; i < f->count && n < max; i++)
            if (mark_at(f, i)) {
                join_path(f->path, f->ent[i].name, out[n]);
                if (out[n][0]) n++;
            }
        return n;
    }
    if (row < 0) row = f->sel;
    if (row >= 0 && row < f->count && n < max) {
        join_path(f->path, f->ent[row].name, out[n]);
        if (out[n][0]) n++;
    }
    return n;
}

static int selection_count(Files *f, int row) {
    if (f->nmarked > 0 && (row < 0 || mark_at(f, row))) return f->nmarked;
    if (row < 0) row = f->sel;
    return (row >= 0 && row < f->count) ? 1 : 0;
}

static int path_exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

/* dir/name, or the first "name copy", "name copy 2" ... that does not
 * exist yet. lstat() decides - never a guess about what is there. */
static void unique_path(const char *dir, const char *name, char *out) {
    join_path(dir, name, out);
    if (!out[0] || !path_exists(out)) return;
    for (int n = 1; n < 10000; n++) {
        char base[NAME_LEN + 32];
        if (n == 1) snprintf(base, sizeof(base), "%s copy", name);
        else snprintf(base, sizeof(base), "%s copy %d", name, n);
        join_path(dir, base, out);
        if (!out[0]) return;
        if (!path_exists(out)) return;
    }
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_LEN];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || n >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int remove_tree(const char *path, int depth) {
    if (depth > DEPTH_MAX) { errno = ELOOP; return -1; }
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0, e = 0;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[PATH_LEN];
        join_path(path, de->d_name, child);
        if (!child[0]) { rc = -1; errno = ENAMETOOLONG; break; }
        if (remove_tree(child, depth + 1) != 0) { rc = -1; break; }
    }
    e = errno;
    closedir(d);
    if (rc == 0 && rmdir(path) != 0) rc = -1;
    if (rc != 0) errno = e;
    return rc;
}

static int copy_file_data(const char *src, const char *dst, const struct stat *st) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char *buf = malloc(COPY_BUF);
    if (!buf) { int e = ENOMEM; close(in); close(out); unlink(dst); errno = e; return -1; }
    int rc = 0;
    for (;;) {
        ssize_t r = read(in, buf, COPY_BUF);
        if (r < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc != 0) break;
    }
    free(buf);
    if (rc == 0 && fchown(out, st->st_uid, st->st_gid) != 0 && errno != EPERM) rc = -1;
    if (rc == 0 && fchmod(out, st->st_mode & 07777) != 0) rc = -1;
    if (close(out) != 0 && rc == 0) rc = -1;
    if (close(in) != 0 && rc == 0) rc = -1;
    if (rc != 0) { int e = errno; unlink(dst); errno = e; return -1; }
    struct timespec ts[2];
    ts[0] = st->st_atim;
    ts[1] = st->st_mtim;
    (void)utimensat(AT_FDCWD, dst, ts, 0);
    return 0;
}

static int copy_tree(const char *src, const char *dst, int depth) {
    if (depth > DEPTH_MAX) { errno = ELOOP; return -1; }
    struct stat st;
    if (lstat(src, &st) != 0) return -1;

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_LEN];
        ssize_t r = readlink(src, tgt, sizeof(tgt) - 1);
        if (r < 0) return -1;
        tgt[r] = 0;
        return symlink(tgt, dst);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, (st.st_mode & 0777) | 0700) != 0) return -1;
        DIR *d = opendir(src);
        if (!d) { int e = errno; rmdir(dst); errno = e; return -1; }
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s2[PATH_LEN], d2[PATH_LEN];
            join_path(src, de->d_name, s2);
            join_path(dst, de->d_name, d2);
            if (!s2[0] || !d2[0]) { errno = ENAMETOOLONG; rc = -1; break; }
            if (copy_tree(s2, d2, depth + 1) != 0) { rc = -1; break; }
        }
        int e = errno;
        closedir(d);
        if (rc == 0) {
            (void)chmod(dst, st.st_mode & 07777);
            struct timespec ts[2];
            ts[0] = st.st_atim;
            ts[1] = st.st_mtim;
            (void)utimensat(AT_FDCWD, dst, ts, 0);
        } else {
            /* never leave a half-copied tree that looks complete */
            remove_tree(dst, depth + 1);
            errno = e;
        }
        return rc;
    }
    if (S_ISREG(st.st_mode)) return copy_file_data(src, dst, &st);
    errno = EOPNOTSUPP;   /* fifos/devices/sockets are not copied */
    return -1;
}

/* ------------------------------------------------------------
 *  The trash: delete is rename(2) into a real directory, which is what
 *  makes the undo a rename back. mkdir_p() creates the parents because
 *  mkdir(2) does not.
 * ------------------------------------------------------------ */
static int trash_ready(void) {
    return mkdir_p(TRASH_DIR, 0700);
}

static void trash_name(const char *path, char *out) {
    struct timespec ts;
    long long ns = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        ns = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < 1000; i++) {
        int n = snprintf(out, PATH_LEN, "%s/%lld-%d-%s", TRASH_DIR, ns, i, base);
        if (n < 0 || n >= PATH_LEN) { out[0] = 0; return; }
        if (!path_exists(out)) return;
    }
    int n = snprintf(out, PATH_LEN, "%s/%ld-%lld-%s", TRASH_DIR, (long)getpid(), ns, base);
    if (n < 0 || n >= PATH_LEN) out[0] = 0;
}

/* Move one path into the trash and record how to bring it back. A path on
 * another filesystem cannot be renamed, so it is copied there and removed
 * from where it was; the undo record is the same either way. */
static int trash_one(Files *f, const char *path) {
    if (f->undo_n >= UNDO_MAX) { errno = ENOSPC; return -1; }
    if (trash_ready() != 0) return -1;
    char dst[PATH_LEN];
    trash_name(path, dst);
    if (!dst[0]) { errno = ENAMETOOLONG; return -1; }

    if (rename(path, dst) != 0) {
        if (errno != EXDEV) return -1;
        if (copy_tree(path, dst, 0) != 0) return -1;
        if (remove_tree(path, 0) != 0) {
            int e = errno;
            remove_tree(dst, 0);
            errno = e;
            return -1;
        }
    }
    snprintf(f->undo[f->undo_n].trash, PATH_LEN, "%s", dst);
    snprintf(f->undo[f->undo_n].orig, PATH_LEN, "%s", path);
    f->undo_n++;
    return 0;
}

/* ------------------------------------------------------------
 *  Properties: the numbers come straight out of lstat(2).
 * ------------------------------------------------------------ */
static const char *type_name(unsigned mode) {
    if (S_ISDIR(mode))  return "directory";
    if (S_ISLNK(mode))  return "symlink";
    if (S_ISREG(mode))  return "regular file";
    if (S_ISCHR(mode))  return "character device";
    if (S_ISBLK(mode))  return "block device";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    return "unknown";
}

static void mode_string(unsigned mode, char out[11]) {
    out[0] = S_ISDIR(mode)  ? 'd' : S_ISLNK(mode) ? 'l' :
             S_ISCHR(mode)  ? 'c' : S_ISBLK(mode) ? 'b' :
             S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & (1u << (8 - i))) ? rwx[i] : '-';
    out[10] = 0;
}

static void fmt_mtime_full(long long mtime, char *out, size_t n) {
    time_t t = (time_t)mtime;
    struct tm tmv;
    if (!localtime_r(&t, &tmv) || strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        snprintf(out, n, "--");
}

static int prop_fill(const char *path, PropInfo *p) {
    memset(p, 0, sizeof(*p));
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    const char *base = strrchr(path, '/');
    snprintf(p->name, sizeof(p->name), "%s", base ? base + 1 : path);
    p->type  = (unsigned)(st.st_mode & S_IFMT);
    p->mode  = (unsigned)st.st_mode;
    p->size  = (unsigned long long)st.st_size;
    p->blocks = (unsigned long long)st.st_blocks;
    p->inode = (unsigned long long)st.st_ino;
    p->mtime = (unsigned long long)st.st_mtime;
    p->nlink = (unsigned long long)st.st_nlink;
    p->uid   = (unsigned)st.st_uid;
    p->gid   = (unsigned)st.st_gid;
    if (S_ISLNK(st.st_mode)) {
        p->has_target = (stat(path, &st) == 0);
        ssize_t r = readlink(path, p->target, sizeof(p->target) - 1);
        if (r >= 0) p->target[r] = 0;
        else p->target[0] = 0;
    }
    return 0;
}

/* The exact lines the Properties panel prints, built once so the panel's
 * height and its text cannot disagree. */
static int prop_lines(const Files *f, char lines[][200], int max) {
    const PropInfo *p = &f->prop;
    int n = 0;
    char sz[32], mode[11], mt[32], owner[80], group[80];
    human_size((long long)p->size, sz, sizeof(sz));
    mode_string(p->mode, mode);
    fmt_mtime_full((long long)p->mtime, mt, sizeof(mt));

    struct passwd pw, *pres = NULL;
    char pbuf[1024];
    if (getpwuid_r(p->uid, &pw, pbuf, sizeof(pbuf), &pres) == 0 && pres)
        snprintf(owner, sizeof(owner), "%s (%u)", pw.pw_name, p->uid);
    else
        snprintf(owner, sizeof(owner), "%u", p->uid);

    struct group gr, *gres = NULL;
    char gbuf[1024];
    if (getgrgid_r(p->gid, &gr, gbuf, sizeof(gbuf), &gres) == 0 && gres)
        snprintf(group, sizeof(group), "%s (%u)", gr.gr_name, p->gid);
    else
        snprintf(group, sizeof(group), "%u", p->gid);

    if (n < max) snprintf(lines[n++], 200, "Type: %s", type_name(p->mode));
    if (n < max) snprintf(lines[n++], 200, "Size: %llu bytes (%s)", p->size, sz);
    if (n < max) snprintf(lines[n++], 200, "Mode: %04o  %s", p->mode & 07777u, mode);
    if (n < max) snprintf(lines[n++], 200, "Owner: %s", owner);
    if (n < max) snprintf(lines[n++], 200, "Group: %s", group);
    if (n < max) snprintf(lines[n++], 200, "Modified: %s", mt);
    if (n < max) snprintf(lines[n++], 200, "Inode: %llu", p->inode);
    if (n < max) snprintf(lines[n++], 200, "Blocks: %llu (%llu bytes on disk)",
                          p->blocks, p->blocks * 512ULL);
    if (n < max) snprintf(lines[n++], 200, "Links: %llu", p->nlink);
    if (p->has_target && n < max)
        snprintf(lines[n++], 200, "Target: %s", p->target[0] ? p->target : "?");
    return n;
}

/* ------------------------------------------------------------
 *  Preview: read-only, max 4 KiB, clipped into the panel.
 * ------------------------------------------------------------ */
static int preview_line_count(const Files *f) {
    int n = 1;
    for (int i = 0; i < f->preview_len; i++)
        if (f->preview[i] == '\n') n++;
    if (n > 400) n = 400;
    return n;
}

static void preview_open(Files *f, int row) {
    if (row < 0 || row >= f->count) return;
    Entry *e = &f->ent[row];
    char full[PATH_LEN];
    entry_path(f, row, full);
    if (!full[0]) { statusf(f, "path too long: %s", e->name); return; }
    if (e->is_dir) { nav_to(f, full, 1); return; }

    int fd = open(full, O_RDONLY);
    if (fd < 0) { status_errno(f, "open:", e->name); return; }
    ssize_t got = read(fd, f->preview, PREVIEW_MAX);
    if (got < 0) { int err = errno; close(fd); errno = err; status_errno(f, "read:", e->name); return; }
    if (close(fd) != 0) { status_errno(f, "close:", e->name); return; }

    f->preview_len = (int)got;
    f->preview[got] = 0;
    f->preview_binary = 0;
    for (int i = 0; i < got; i++)
        if ((unsigned char)f->preview[i] == 0) { f->preview_binary = 1; break; }
    f->preview_scroll = 0;

    overlays_close(f);
    snprintf(f->dlg_title, sizeof(f->dlg_title), "%s", e->name);
    cut_utf8(f->dlg_title);
    snprintf(f->dlg_msg, sizeof(f->dlg_msg), "%d bytes%s", (int)got,
             e->size > (long long)got ? " (first 4096 shown)" : "");
    f->dlg = DLG_PREVIEW;
    statusf(f, "opened %s read-only", e->name);
}

/* ------------------------------------------------------------
 *  Clipboard actions
 * ------------------------------------------------------------ */
static void clip_capture(Files *f, int row, int cut) {
    char (*paths)[PATH_LEN] = malloc(sizeof(char[PATH_LEN]) * CLIP_MAX);
    if (!paths) { errno = ENOMEM; status_errno(f, "copy", NULL); return; }
    int n = selection_paths(f, row, paths, CLIP_MAX);
    if (n <= 0) {
        free(paths);
        statusf(f, "nothing selected");
        return;
    }
    f->clip.n = 0;
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        snprintf(f->clip.item[i].path, PATH_LEN, "%s", paths[i]);
        snprintf(f->clip.item[i].name, NAME_LEN, "%s", base);
        f->clip.n++;
    }
    f->clip.cut = cut;
    statusf(f, "%s %d item%s", cut ? "cut" : "copied", n, n == 1 ? "" : "s");
    free(paths);
}

static void do_copy(Files *f, int row) { clip_capture(f, row, 0); }
static void do_cut(Files *f, int row)  { clip_capture(f, row, 1); }

static void do_paste(Files *f, int row) {
    if (f->clip.n <= 0) { statusf(f, "clipboard is empty"); return; }

    char dest[PATH_LEN];
    if (row >= 0 && row < f->count && f->ent[row].is_dir) entry_path(f, row, dest);
    else snprintf(dest, PATH_LEN, "%s", f->path);
    if (!dest[0]) { statusf(f, "path too long"); return; }

    int ok = 0, fail = 0, first_err = 0;
    char first_name[NAME_LEN] = "";
    for (int i = 0; i < f->clip.n; i++) {
        char dst[PATH_LEN];
        unique_path(dest, f->clip.item[i].name, dst);
        if (!dst[0]) { if (!fail) { first_err = ENAMETOOLONG; snprintf(first_name, sizeof(first_name), "%s", f->clip.item[i].name); } fail++; continue; }

        /* A directory must not be pasted inside itself: copying would recurse
         * through the directory being created, and a rename would move it
         * under its own child. Refuse with the kernel's EINVAL. */
        {
            size_t sl = strlen(f->clip.item[i].path);
            if (sl > 0 && strncmp(dst, f->clip.item[i].path, sl) == 0 &&
                (dst[sl] == '/' || dst[sl] == 0)) {
                if (!fail) {
                    first_err = EINVAL;
                    snprintf(first_name, sizeof(first_name), "%s", f->clip.item[i].name);
                }
                fail++;
                continue;
            }
        }

        int rc;
        if (f->clip.cut) {
            rc = rename(f->clip.item[i].path, dst);
            if (rc != 0 && errno == EXDEV) {
                rc = copy_tree(f->clip.item[i].path, dst, 0);
                if (rc == 0 && remove_tree(f->clip.item[i].path, 0) != 0) {
                    int e = errno;
                    remove_tree(dst, 0);
                    errno = e;
                    rc = -1;
                }
            }
        } else {
            rc = copy_tree(f->clip.item[i].path, dst, 0);
        }
        if (rc == 0) ok++;
        else {
            if (!fail) {
                first_err = errno;
                snprintf(first_name, sizeof(first_name), "%s", f->clip.item[i].name);
            }
            fail++;
        }
    }

    if (f->clip.cut && fail == 0) f->clip.n = 0;
    rescan(f);
    if (fail)
        statusf(f, "pasted %d, %d failed - %s: %s", ok, fail, first_name, strerror(first_err));
    else
        statusf(f, "pasted %d item%s", ok, ok == 1 ? "" : "s");
}

/* ------------------------------------------------------------
 *  Delete: ask, then trash, then rescan. undo_delete() reverses the last
 *  batch with rename(2).
 * ------------------------------------------------------------ */
static void delete_confirm(Files *f) {
    char (*paths)[PATH_LEN] = malloc(sizeof(char[PATH_LEN]) * CLIP_MAX);
    if (!paths) { errno = ENOMEM; status_errno(f, "delete", NULL); f->dlg = DLG_NONE; return; }
    int n = selection_paths(f, f->sel, paths, CLIP_MAX);
    if (n <= 0) { free(paths); f->dlg = DLG_NONE; statusf(f, "nothing selected"); return; }

    f->undo_n = 0;
    int ok = 0, fail = 0, first_err = 0;
    char first_name[NAME_LEN] = "";
    for (int i = 0; i < n; i++) {
        if (trash_one(f, paths[i]) == 0) ok++;
        else {
            if (!fail) {
                first_err = errno;
                const char *base = strrchr(paths[i], '/');
                snprintf(first_name, sizeof(first_name), "%s", base ? base + 1 : paths[i]);
            }
            fail++;
        }
    }
    free(paths);
    f->dlg = DLG_NONE;
    rescan(f);
    if (fail)
        statusf(f, "deleted %d, %d failed - %s: %s", ok, fail, first_name, strerror(first_err));
    else
        statusf(f, "moved %d item%s to the trash (Ctrl+Z restores)", ok, ok == 1 ? "" : "s");
}

static void delete_ask(Files *f) {
    int n = selection_count(f, f->sel);
    if (n <= 0) { statusf(f, "nothing selected"); return; }
    overlays_close(f);
    f->delete_count = n;
    snprintf(f->dlg_title, sizeof(f->dlg_title), "Delete");
    snprintf(f->dlg_msg, sizeof(f->dlg_msg), "The last delete can be undone with Ctrl+Z.");
    f->dlg = DLG_DELETE;
}

static void undo_delete(Files *f) {
    if (f->undo_n <= 0) { statusf(f, "nothing to undo"); return; }
    int ok = 0, fail = 0, first_err = 0;
    char first[PATH_LEN] = "";
    for (int i = f->undo_n - 1; i >= 0; i--) {
        if (rename(f->undo[i].trash, f->undo[i].orig) == 0) ok++;
        else {
            if (!fail) {
                first_err = errno;
                snprintf(first, sizeof(first), "%s", f->undo[i].orig);
            }
            fail++;
        }
    }
    f->undo_n = 0;
    rescan(f);
    if (fail) {
        const char *base = strrchr(first, '/');
        statusf(f, "restored %d, %d failed - %s: %s", ok, fail,
                base ? base + 1 : first, strerror(first_err));
    } else {
        statusf(f, "restored %d item%s", ok, ok == 1 ? "" : "s");
    }
}

/* ------------------------------------------------------------
 *  Inline rename / new folder
 * ------------------------------------------------------------ */
static int name_valid(Files *f, const char *name) {
    if (!name[0]) { statusf(f, "a name cannot be empty"); return 0; }
    if (!strcmp(name, ".") || !strcmp(name, "..")) { statusf(f, "\"%s\" is reserved", name); return 0; }
    if (strchr(name, '/')) { statusf(f, "a name cannot contain /"); return 0; }
    if (strlen(name) >= NAME_LEN) { statusf(f, "name is too long"); return 0; }
    return 1;
}

static void ti_open_rename(Files *f, int row) {
    if (row < 0 || row >= f->count) return;
    overlays_close(f);
    f->ti.mode = TI_RENAME;
    f->ti.row = row;
    snprintf(f->ti.buf, sizeof(f->ti.buf), "%s", f->ent[row].name);
    f->ti.len = (int)strlen(f->ti.buf);
    f->ti.caret = f->ti.len;
    statusf(f, "rename %s - Enter commits, Esc cancels", f->ent[row].name);
}

static void ti_open_mkdir(Files *f) {
    overlays_close(f);
    f->ti.mode = TI_MKDIR;
    f->ti.row = -1;
    f->ti.buf[0] = 0;
    f->ti.len = 0;
    f->ti.caret = 0;
    statusf(f, "new name in %s - a name with an extension makes a file, a bare name a folder (Enter creates, Esc cancels)", f->path);
}

static void ti_cancel(Files *f) {
    if (f->ti.mode == TI_NONE) return;
    f->ti.mode = TI_NONE;
    statusf(f, "edit cancelled");
}

static void ti_select_name(Files *f, const char *want) {
    f->sel = 0;
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->ent[i].name, want)) { f->sel = i; mark_set(f, i, 1); f->anchor = i; break; }
}

static void ti_commit(Files *f) {
    char want[NAME_LEN];
    if (!name_valid(f, f->ti.buf)) return;
    snprintf(want, sizeof(want), "%s", f->ti.buf);

    if (f->ti.mode == TI_RENAME) {
        if (f->ti.row < 0 || f->ti.row >= f->count) { f->ti.mode = TI_NONE; return; }
        char old[PATH_LEN], newp[PATH_LEN];
        join_path(f->path, f->ent[f->ti.row].name, old);
        join_path(f->path, want, newp);
        if (!old[0] || !newp[0]) { statusf(f, "path too long"); return; }
        if (!strcmp(old, newp)) { f->ti.mode = TI_NONE; statusf(f, "name unchanged"); return; }
        /* rename(2) would silently replace an existing file; a file manager
         * must not lose a name to a typo. */
        if (path_exists(newp)) { statusf(f, "already exists: %s", want); return; }
        if (rename(old, newp) != 0) { status_errno(f, "rename:", want); return; }
        f->ti.mode = TI_NONE;
        rescan(f);
        ti_select_name(f, want);
        statusf(f, "renamed to %s", want);
        return;
    }

    if (f->ti.mode == TI_MKDIR) {
        char full[PATH_LEN];
        join_path(f->path, want, full);
        if (!full[0]) { statusf(f, "path too long"); return; }
        /* A name carrying an extension creates a FILE of that format, an
         * O_CREAT|O_EXCL empty file - never a clobbered one. A bare name is
         * still a folder. The type registry then gives it the right icon. */
        const char *dot = strrchr(want, '.');
        if (dot && dot != want && dot[1]) {
            int fd = open(full, O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) { status_errno(f, "create:", want); return; }
            close(fd);
            const FileType *t = filetype_for(want);
            f->ti.mode = TI_NONE;
            rescan(f);
            ti_select_name(f, want);
            statusf(f, "created %s (%s)", want,
                    t ? filetype_label(t) : "empty file");
            return;
        }
        if (mkdir(full, 0755) != 0) { status_errno(f, "mkdir:", want); return; }
        f->ti.mode = TI_NONE;
        rescan(f);
        ti_select_name(f, want);
        statusf(f, "created %s", want);
    }
}

/* The text field's own key handling. It runs before every navigation key,
 * so typing 'j' into a name inserts a j instead of moving the cursor. */
static void ti_key(Files *f, const char *bytes, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            int j = i + 2, arg = 0, have_arg = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') { arg = arg * 10 + (bytes[j] - '0'); have_arg = 1; j++; }
            if (j >= n) break;
            char fin = bytes[j];
            if (fin == 'D') { if (f->ti.caret > 0) f->ti.caret--; }
            else if (fin == 'C') { if (f->ti.caret < f->ti.len) f->ti.caret++; }
            else if (fin == 'H') f->ti.caret = 0;
            else if (fin == 'F') f->ti.caret = f->ti.len;
            else if (fin == '~' && have_arg && arg == 3) {
                if (f->ti.caret < f->ti.len) {
                    memmove(f->ti.buf + f->ti.caret, f->ti.buf + f->ti.caret + 1,
                            (size_t)(f->ti.len - f->ti.caret));
                    f->ti.len--;
                    f->ti.buf[f->ti.len] = 0;
                }
            }
            i = j;
            continue;
        }
        if (c == 0x1B) { ti_cancel(f); return; }
        if (c == '\n' || c == '\r') { ti_commit(f); return; }
        if (c == 0x16) {
            /* Ctrl+V in the name field: the clipboard's text, inserted at the
             * caret. Clipboard.c is linked into this desktop, so Files reads
             * it directly - no socket, no jail. Line breaks and NULs are not
             * name bytes and are dropped. */
            ClipEntry *e = clip_get_current(CLIP_TYPE_TEXT);
            if (!e || e->type != CLIP_TYPE_TEXT || !e->text) {
                clip_entry_free(e);
                statusf(f, "paste: the clipboard holds no text");
                continue;
            }
            int put = 0;
            for (uint32_t k = 0; k < e->text_len; k++) {
                unsigned char ch = (unsigned char)e->text[k];
                if (ch < 0x20 || ch == 0x7F) continue;
                if (f->ti.len >= NAME_LEN - 1) break;
                memmove(f->ti.buf + f->ti.caret + 1, f->ti.buf + f->ti.caret,
                        (size_t)(f->ti.len - f->ti.caret + 1));
                f->ti.buf[f->ti.caret] = (char)ch;
                f->ti.caret++;
                f->ti.len++;
                put++;
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "pasted %d byte%s", put, put == 1 ? "" : "s");
            statusf(f, "%s", msg);
            clip_entry_free(e);
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (f->ti.caret > 0 && f->ti.len > 0) {
                memmove(f->ti.buf + f->ti.caret - 1, f->ti.buf + f->ti.caret,
                        (size_t)(f->ti.len - f->ti.caret));
                f->ti.caret--;
                f->ti.len--;
                f->ti.buf[f->ti.len] = 0;
            }
            continue;
        }
        if (c < 0x20) continue;            /* other control keys: not name bytes */
        if (c == 0x7F) continue;
        if (f->ti.len >= NAME_LEN - 1) continue;
        memmove(f->ti.buf + f->ti.caret + 1, f->ti.buf + f->ti.caret,
                (size_t)(f->ti.len - f->ti.caret + 1));
        f->ti.buf[f->ti.caret] = (char)c;
        f->ti.caret++;
        f->ti.len++;
    }
}

/* ------------------------------------------------------------
 *  Menus
 *
 *  A menu is built (labels, shortcuts, enabled/checked state), measured
 *  from the font, then placed with a clamp that guarantees every pixel of
 *  the box and of its shadow is inside the app rect. Items are drawn and
 *  hit-tested through the same y0/y1 bands, so keyboard focus, the pointer
 *  and the painter always agree.
 * ------------------------------------------------------------ */
static MenuItem *menu_add(Menu *m, const char *label, const char *shortcut,
                          int action, int enabled) {
    if (m->n >= MENU_MAX) return NULL;
    MenuItem *it = &m->item[m->n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->shortcut, sizeof(it->shortcut), "%s", shortcut ? shortcut : "");
    it->action = action;
    it->enabled = (unsigned char)(enabled ? 1 : 0);
    if (m->pending_sep && m->n > 1) it->sep_before = 1;
    m->pending_sep = 0;
    return it;
}

static void menu_sep(Menu *m) { m->pending_sep = 1; }

static void menu_build_row(Files *f, Menu *m, int row) {
    (void)row;
    menu_add(m, "Open", "Enter", MA_OPEN, 1);
    menu_add(m, "Open with...", "", MA_OPENWITH, 1)->submenu = 1;
    menu_sep(m);
    menu_add(m, "Rename", "m", MA_RENAME, 1);
    menu_add(m, "Copy", "Ctrl+C", MA_COPY, 1);
    menu_add(m, "Cut", "Ctrl+X", MA_CUT, 1);
    menu_add(m, "Paste", "Ctrl+V", MA_PASTE, f->clip.n > 0);
    menu_add(m, "Delete", "Del", MA_DELETE, 1);
    menu_sep(m);
    menu_add(m, "New folder", "n", MA_NEWFOLDER, 1);
    menu_add(m, "Properties", "i", MA_PROPS, 1);
}

static void menu_build_bg(Files *f, Menu *m) {
    menu_add(m, "New folder", "n", MA_NEWFOLDER, 1);
    menu_add(m, "Paste", "Ctrl+V", MA_PASTE, f->clip.n > 0);
    menu_add(m, "Refresh", "r", MA_REFRESH, 1);
    menu_sep(m);
    menu_add(m, "Sort by", "s", MA_SORT_NAME, 1)->submenu = 1;
    MenuItem *it = menu_add(m, "Show hidden", "H", MA_SHOW_HIDDEN, 1);
    it->checked = (unsigned char)(f->show_hidden ? 1 : 0);
    if (f->undo_n > 0) {
        menu_sep(m);
        menu_add(m, "Undo delete", "Ctrl+Z", MA_UNDO, 1);
    }
}

/* Sort by is a submenu with a check on the active mode. */
static void menu_build_sort(Files *f, Menu *m) {
    MenuItem *it;
    it = menu_add(m, "Name", "", MA_SORT_NAME, 1);
    it->checked = (unsigned char)(f->sort == SORT_NAME ? 1 : 0);
    it = menu_add(m, "Size", "", MA_SORT_SIZE, 1);
    it->checked = (unsigned char)(f->sort == SORT_SIZE ? 1 : 0);
    it = menu_add(m, "Modified", "", MA_SORT_TIME, 1);
    it->checked = (unsigned char)(f->sort == SORT_TIME ? 1 : 0);
}

/* Read the app bundles that really exist (a directory with a manifest),
 * exactly like src/wmsrv.c and src/apps/store.c do. */
static int scan_bundles(char ids[][40], char names[][64], int max) {
    int n = 0;
    const char *const dirs[2] = { APP_DIR_ONE, APP_DIR_TWO };
    for (int d = 0; d < 2 && n < max; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && n < max) {
            if (de->d_name[0] == '.' || strlen(de->d_name) >= 40) continue;
            char man[PATH_LEN];
            int mn = snprintf(man, sizeof(man), "%s/%s/manifest", dirs[d], de->d_name);
            if (mn < 0 || mn >= (int)sizeof(man)) continue;
            FILE *mf = fopen(man, "r");
            if (!mf) continue;           /* not an app bundle */
            char name[64] = "";
            char line[256];
            while (fgets(line, sizeof(line), mf)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "name", 4) != 0) continue;
                p += 4;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '=') continue;
                p++;
                while (*p == ' ' || *p == '\t') p++;
                char *e = p + strlen(p);
                while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
                snprintf(name, sizeof(name), "%s", p);
                break;
            }
            fclose(mf);
            if (!name[0]) snprintf(name, sizeof(name), "%s", de->d_name);
            snprintf(ids[n], 40, "%s", de->d_name);
            snprintf(names[n], 64, "%s", name);
            n++;
        }
        closedir(dp);
    }
    return n;
}

static void menu_build_openwith(Files *f, Menu *m) {
    (void)f;
    char ids[8][40], names[8][64];
    int n = scan_bundles(ids, names, 8);
    if (n == 0) {
        menu_add(m, "No app bundles found", "", MA_CLOSE, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        MenuItem *it = menu_add(m, names[i], "", MA_LAUNCH, 1);
        if (it) snprintf(it->arg, sizeof(it->arg), "%s", ids[i]);
    }
    menu_sep(m);
    menu_add(m, "Cancel", "Esc", MA_CLOSE, 1);
}

/* The launch protocol is one bundle id per line; the desktop does the fork.
 * Validate the id so a corrupt manifest cannot inject a second line. */
static void launch_bundle(Files *f, const char *id) {
    size_t n = strlen(id);
    int ok = (n > 0 && n < 32);
    for (size_t i = 0; ok && i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) ok = 0;
    }
    if (!ok) { statusf(f, "refusing an invalid app id"); return; }
    FILE *fp = fopen(LAUNCH_REQ, "w");
    if (!fp) { status_errno(f, "cannot write", LAUNCH_REQ); return; }
    int wrote = fprintf(fp, "%s\n", id);
    if (fclose(fp) != 0 || wrote < 0) { status_errno(f, "cannot write", LAUNCH_REQ); return; }
    statusf(f, "asked the desktop to launch %s (a launch request carries no file name)", id);
}

static int menu_measure(Menu *m, int win_w, int win_h) {
    Font *f = ui_fonts().body;
    if (!f || m->n <= 0 || m->n > MENU_MAX) return 0;

    int vpad = SP(1), gap = SP(2);
    int fh = iceil(font_height(f));
    if (fh < 1) fh = 1;
    m->pad = SP(1);
    m->row_h = ui_row_h(f);
    m->sep_h = SP(1);

    int label_w = 0, short_w = 0, has_check = 0, has_sub = 0, seps = 0;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->item[i];
        int w = iceil(font_width(f, it->label));
        if (w > label_w) label_w = w;
        if (it->shortcut[0]) {
            w = iceil(font_width(f, it->shortcut));
            if (w > short_w) short_w = w;
        }
        if (it->checked) has_check = 1;
        if (it->submenu) has_sub = 1;
        if (it->sep_before && i > 0) seps++;
    }
    m->check_w = has_check ? SP(2) : 0;
    m->arrow_w = has_sub ? SP(2) : 0;

    int margin = SP(1);
    int maxw = win_w - 2 * margin;
    int maxh = win_h - 2 * margin;
    if (maxw < SP(5) || maxh < SP(5)) return 0;

    int w = 2 * m->pad + m->check_w + label_w +
            (short_w ? gap + short_w : 0) + m->arrow_w;
    if (w > maxw) w = maxw;
    if (w < SP(16) && SP(16) <= maxw) w = SP(16);

    int h = 2 * vpad + m->n * m->row_h + seps * m->sep_h;
    if (h > maxh) {
        int avail = maxh - 2 * vpad - seps * m->sep_h;
        int fit = avail / m->n;
        if (fit < m->row_h) m->row_h = fit;
        if (m->row_h < fh) m->row_h = fh;      /* the glyphs need their line box */
        h = 2 * vpad + m->n * m->row_h + seps * m->sep_h;
    }

    int yy = vpad;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->item[i];
        if (it->sep_before && i > 0) yy += m->sep_h;
        it->y0 = yy;
        it->y1 = yy + m->row_h;
        yy += m->row_h;
    }
    m->w = w;
    m->h = yy + vpad;

    /* A window that is too short for the whole list cannot hold the menu
     * either. The row height shrinks first (above), but the glyphs need
     * their line box, so the box may still be taller than the window allows:
     * cap it here and cut the bands that would fall outside. A cut item keeps
     * a zero-height band, which menu_hit(), menu_focus_move() and the painter
     * all skip - so what is painted, what the keyboard can reach and what a
     * click can land on stay the same set. */
    if (m->h > maxh) {
        m->h = maxh;
        int bottom = m->h - vpad;
        for (int i = 0; i < m->n; i++) {
            MenuItem *it = &m->item[i];
            if (it->y0 >= bottom) {
                it->y0 = it->y1 = 0;
                it->enabled = 0;        /* unreachable: not drawn, not hit */
                continue;
            }
            if (it->y1 > bottom) it->y1 = bottom;
        }
    }
    return 1;
}

static int menu_place(Menu *m, int wx, int wy, int win_w, int win_h) {
    int margin = SP(1);
    if (m->w > win_w - 2 * margin) m->w = win_w - 2 * margin;
    if (m->h > win_h - 2 * margin) m->h = win_h - 2 * margin;
    if (m->w < 1 || m->h < 1) return 0;
    m->x = wx;
    m->y = wy;
    if (m->x + m->w > win_w - margin) m->x = win_w - margin - m->w;
    if (m->x < margin) m->x = margin;
    if (m->y + m->h > win_h - margin) m->y = win_h - margin - m->h;
    if (m->y < margin) m->y = margin;
    return 1;
}

static int menu_hit(const Menu *m, int cx, int cy) {
    if (!m->open) return -1;
    if (cx < m->x || cx >= m->x + m->w || cy < m->y || cy >= m->y + m->h) return -1;
    for (int i = 0; i < m->n; i++) {
        int y0 = m->y + m->item[i].y0;
        int y1 = m->y + m->item[i].y1;
        if (y1 <= y0) continue;          /* cut by menu_measure(): no band */
        if (cy >= y0 && cy < y1) return i;
    }
    return -1;
}

static int menu_inside(const Menu *m, int cx, int cy) {
    return m->open && cx >= m->x && cx < m->x + m->w && cy >= m->y && cy < m->y + m->h;
}

static void menu_focus_move(Menu *m, int dir) {
    if (m->n <= 0) return;
    int i = m->focus;
    for (int step = 0; step < m->n; step++) {
        i += dir;
        if (i < 0) i = m->n - 1;
        if (i >= m->n) i = 0;
        if (m->item[i].enabled) { m->focus = i; return; }
    }
}

static void menu_focus_edge(Menu *m, int last) {
    if (last) {
        for (int i = m->n - 1; i >= 0; i--)
            if (m->item[i].enabled) { m->focus = i; return; }
    } else {
        for (int i = 0; i < m->n; i++)
            if (m->item[i].enabled) { m->focus = i; return; }
    }
}

static void props_open(Files *f, int row) {
    if (row < 0 || row >= f->count) { statusf(f, "nothing selected"); return; }
    char full[PATH_LEN];
    entry_path(f, row, full);
    if (!full[0]) { statusf(f, "path too long: %s", f->ent[row].name); return; }
    if (prop_fill(full, &f->prop) != 0) { status_errno(f, "stat:", f->ent[row].name); return; }
    overlays_close(f);
    f->prop_scroll = 0;
    snprintf(f->dlg_title, sizeof(f->dlg_title), "%s", f->ent[row].name);
    cut_utf8(f->dlg_title);
    snprintf(f->dlg_msg, sizeof(f->dlg_msg), "%s", full);
    cut_utf8(f->dlg_msg);
    f->dlg = DLG_PROPS;
}

static void submenu_open(Files *f, Menu *parent, int i) {
    Menu *s = &f->sub;
    memset(s, 0, sizeof(*s));
    int action = parent->item[i].action;
    if (action == MA_OPENWITH) menu_build_openwith(f, s);
    else if (action == MA_SORT_NAME) menu_build_sort(f, s);
    else return;
    if (!menu_measure(s, f->w, f->h)) return;

    int x = parent->x + parent->w;
    int y = parent->y + parent->item[i].y0;
    if (x + s->w > f->w - SP(1)) x = parent->x - s->w;
    if (!menu_place(s, x, y, f->w, f->h)) return;
    s->open = 1;
    s->focus = -1;
    s->row = parent->row;
}

static void menu_activate(Files *f, Menu *m, int i) {
    if (i < 0 || i >= m->n || !m->item[i].enabled) return;
    MenuItem *it = &m->item[i];
    int row = f->menu.row;

    if (it->submenu) { submenu_open(f, m, i); return; }

    f->menu.open = 0;
    f->sub.open = 0;
    switch (it->action) {
        case MA_OPEN:
            if (row >= 0) f->sel = row;
            open_selected(f);
            break;
        case MA_RENAME:
            ti_open_rename(f, row >= 0 ? row : f->sel);
            break;
        case MA_COPY:
            do_copy(f, row);
            break;
        case MA_CUT:
            do_cut(f, row);
            break;
        case MA_PASTE:
            do_paste(f, row);
            break;
        case MA_DELETE:
            delete_ask(f);
            break;
        case MA_NEWFOLDER:
            ti_open_mkdir(f);
            break;
        case MA_PROPS:
            props_open(f, row >= 0 ? row : f->sel);
            break;
        case MA_REFRESH:
            rescan(f);
            statusf(f, "refreshed %s", f->path);
            break;
        case MA_SORT_NAME:
            sort_set(f, SORT_NAME);
            break;
        case MA_SORT_SIZE:
            sort_set(f, SORT_SIZE);
            break;
        case MA_SORT_TIME:
            sort_set(f, SORT_TIME);
            break;
        case MA_SHOW_HIDDEN:
            f->show_hidden = !f->show_hidden;
            rescan(f);
            statusf(f, "%s hidden files", f->show_hidden ? "showing" : "hiding");
            break;
        case MA_UNDO:
            undo_delete(f);
            break;
        case MA_LAUNCH:
            launch_bundle(f, it->arg);
            break;
        case MA_CLOSE:
        default:
            break;
    }
}

static void menu_open_row(Files *f, int row, int cx, int cy) {
    Menu *m = &f->menu;
    memset(m, 0, sizeof(*m));
    menu_build_row(f, m, row);
    if (!menu_measure(m, f->w, f->h)) return;
    int x = cx;
    int y = cy;
    if (y + m->h > f->h - SP(1)) y = cy - m->h;    /* flip above the pointer */
    if (!menu_place(m, x, y, f->w, f->h)) return;
    m->open = 1;
    m->row = row;
    m->focus = menu_hit(m, cx, cy);
}

static void menu_open_bg(Files *f, int cx, int cy) {
    Menu *m = &f->menu;
    memset(m, 0, sizeof(*m));
    menu_build_bg(f, m);
    if (!menu_measure(m, f->w, f->h)) return;
    int x = cx;
    int y = cy;
    if (y + m->h > f->h - SP(1)) y = cy - m->h;
    if (!menu_place(m, x, y, f->w, f->h)) return;
    m->open = 1;
    m->row = -1;
    m->focus = menu_hit(m, cx, cy);
}

static void menu_key(Files *f, const char *bytes, int n) {
    Menu *m = f->sub.open ? &f->sub : &f->menu;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            int j = i + 2, arg = 0, have_arg = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') { arg = arg * 10 + (bytes[j] - '0'); have_arg = 1; j++; }
            if (j >= n) break;
            char fin = bytes[j];
            if (fin == 'A') menu_focus_move(m, -1);
            else if (fin == 'B') menu_focus_move(m, 1);
            else if (fin == 'H') menu_focus_edge(m, 0);
            else if (fin == 'F') menu_focus_edge(m, 1);
            else if (fin == 'C') {
                if (m->focus >= 0 && m->item[m->focus].submenu)
                    submenu_open(f, m, m->focus);
            } else if (fin == 'D') {
                if (f->sub.open) f->sub.open = 0;
                else f->menu.open = 0;
            } else if (fin == '~' && have_arg && arg == 5) menu_focus_move(m, -3);
            else if (fin == '~' && have_arg && arg == 6) menu_focus_move(m, 3);
            i = j;
            continue;
        }
        if (c == 0x1B) {
            if (f->sub.open) f->sub.open = 0;
            else f->menu.open = 0;
            return;
        }
        if (c == '\n' || c == '\r') {
            if (m->focus >= 0) menu_activate(f, m, m->focus);
            else if (m->n > 0) { menu_focus_edge(m, 0); menu_activate(f, m, m->focus); }
            return;
        }
        if (c == 'j') menu_focus_move(m, 1);
        else if (c == 'k') menu_focus_move(m, -1);
    }
}

/* ------------------------------------------------------------
 *  Dialog panels: Properties, Delete confirmation, Preview
 * ------------------------------------------------------------ */
typedef struct {
    int x, y, w, h;
    int pad, vpad, line_h, title_y, msg_y, body_y, body_h;
    int b1x, b1y, b1w, b1h, b2x, b2y, b2w, b2h;
    int has_b1, cols;
} DlgGeom;

static int dialog_lines(const Files *f, char lines[][200], int max) {
    int n = 0;
    if (f->dlg == DLG_DELETE) {
        if (n < max) snprintf(lines[n++], 200, "Move %d item%s to the trash?",
                              f->delete_count, f->delete_count == 1 ? "" : "s");
        if (n < max) snprintf(lines[n++], 200, "Trash: %s", TRASH_DIR);
        if (n < max) snprintf(lines[n++], 200, "Enter confirms, Esc cancels. Ctrl+Z undoes.");
        return n;
    }
    if (f->dlg == DLG_PROPS) return prop_lines(f, lines, max);
    return 0;
}

/* ------------------------------------------------------------
 *  Dialog panels: Properties, Delete confirmation, Preview
 *  Same three fonts, same spacing unit, same centring as the window.
 * ------------------------------------------------------------ */
static int dialog_geom(const Files *f, DlgGeom *g) {
    UiFont u = ui_fonts();
    Font *ft = u.title, *fs = u.small;
    if (!ft || !fs || f->dlg == DLG_NONE) return 0;
    memset(g, 0, sizeof(*g));

    int margin = SP(2);
    int pad = SP(2);
    int th = ui_ceilf(font_height(ft));
    int fh = ui_ceilf(font_height(fs));
    g->pad = pad;
    g->vpad = SP(1);
    g->line_h = ui_row_h(fs);

    char lines[12][200];
    int nl = dialog_lines(f, lines, 12);
    if (f->dlg == DLG_PREVIEW) {
        nl = preview_line_count(f);
        if (nl > 16) nl = 16;
    }

    int maxw = f->w - 2 * margin;
    int maxh = f->h - 2 * margin;
    if (maxw < SP(30) || maxh < SP(24)) return 0;

    int w = SP(56);
    if (w > maxw) w = maxw;

    int btn_h = fh + 2 * SP(1);
    int msg_h = f->dlg_msg[0] ? g->line_h : 0;
    int fixed = pad + th + SP(1) + msg_h + SP(1) + btn_h + pad;

    g->cols = (f->dlg == DLG_PROPS && w >= SP(44)) ? 2 : 1;
    int body_lines = (nl + g->cols - 1) / g->cols;
    int body_h = body_lines * g->line_h;
    if (fixed + body_h > maxh) {
        body_h = maxh - fixed;
        if (body_h < g->line_h) body_h = g->line_h;
    }
    int h = fixed + body_h;
    if (h > maxh) h = maxh;

    g->w = w;
    g->h = h;
    g->x = (f->w - w) / 2;
    g->y = (f->h - h) / 2;
    if (g->x < margin) g->x = margin;
    if (g->y < margin) g->y = margin;

    g->title_y = g->y + pad;
    g->msg_y = g->title_y + th + SP(1);
    g->body_y = g->msg_y + msg_h + SP(1);
    g->body_h = body_h;

    int by = g->y + g->h - pad - btn_h;
    const char *close_lab = (f->dlg == DLG_DELETE) ? "Cancel" : "Close";
    int cw = ui_ceilf(font_width(fs, close_lab)) + 2 * SP(2);
    g->b2w = cw;
    g->b2h = btn_h;
    g->b2x = g->x + g->w - pad - cw;
    g->b2y = by;

    if (f->dlg == DLG_DELETE) {
        const char *ok = "Delete";
        int ow = ui_ceilf(font_width(fs, ok)) + 2 * SP(2);
        g->b1w = ow;
        g->b1h = btn_h;
        g->b1x = g->b2x - SP(1) - ow;
        g->b1y = by;
        g->has_b1 = 1;
    }
    return 1;
}

/* one line of preview text, control bytes turned into spaces so the font
 * cannot draw a missing glyph box where a tab or an escape was */
static void draw_preview_line(Surface *s, Font *f, float x, float y, float maxw,
                              const char *src, int maxlen, u32 color) {
    char buf[220];
    int n = maxlen;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        buf[i] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    buf[n] = 0;
    draw_clipped(s, f, x, y, maxw, buf, color);
}

static void draw_preview_body(Files *f, Surface *s, int x, int y, const DlgGeom *g,
                              Font *fs, const Theme *th) {
    if (f->preview_binary) {
        draw_clipped(s, fs, (float)(x + g->x + g->pad), (float)(y + g->body_y),
                     (float)(g->w - 2 * g->pad),
                     "binary file - no text preview", color_alpha(th->text, 0.60f));
        return;
    }

    int visible = g->body_h / g->line_h;
    if (visible < 1) visible = 1;
    int start = f->preview_scroll;
    int line = 0, pos = 0;
    while (pos < f->preview_len && line < start) {
        if (f->preview[pos] == '\n') line++;
        pos++;
    }
    for (int i = 0; i < visible; i++) {
        int end = pos;
        while (end < f->preview_len && f->preview[end] != '\n') end++;
        float ly = (float)(y + g->body_y + i * g->line_h) +
                   (float)ui_top(fs, g->line_h);
        draw_preview_line(s, fs, (float)(x + g->x + g->pad), ly,
                          (float)(g->w - 2 * g->pad), f->preview + pos, end - pos,
                          color_alpha(th->text, 0.82f));
        if (end >= f->preview_len) break;
        pos = end + 1;
    }
}

static void draw_dialog(Files *f, Surface *s, int x, int y, const Theme *th) {
    DlgGeom g;
    if (!dialog_geom(f, &g)) return;
    UiFont u = ui_fonts();
    Font *ft = u.title, *fs = u.small;
    if (!ft || !fs) return;

    /* modal dim, exactly the app rect - never a pixel more */
    surf_blend(s, x, y, f->w, f->h, 0x66000000u);
    gfx_shadow(s, (float)(x + g.x), (float)(y + g.y), (float)g.w, (float)g.h,
               (float)g.pad, (float)SP(1), 0.50f);
    gfx_rrect_aa(s, (float)(x + g.x), (float)(y + g.y), (float)g.w, (float)g.h,
                 (float)g.pad, th->window_bg,
                 color_alpha(th->panel_border, 0.95f), 1.0f);

    draw_clipped(s, ft, (float)(x + g.x + g.pad), (float)(y + g.title_y),
                 (float)(g.w - 2 * g.pad), f->dlg_title, th->text);
    if (f->dlg_msg[0])
        draw_clipped(s, fs, (float)(x + g.x + g.pad),
                     (float)(y + g.msg_y) + (float)ui_top(fs, g.line_h),
                     (float)(g.w - 2 * g.pad), f->dlg_msg,
                     color_alpha(th->text, 0.55f));

    int rule_y = y + g.body_y - SP(1) / 2;
    surf_blend(s, x + g.x + g.pad, rule_y, g.w - 2 * g.pad, ui_px(1),
               color_alpha(th->panel_border, 0.75f));

    if (f->dlg == DLG_PREVIEW) {
        draw_preview_body(f, s, x, y, &g, fs, th);
    } else {
        char lines[12][200];
        int nl = dialog_lines(f, lines, 12);
        int per_col = (nl + g.cols - 1) / g.cols;
        int colgap = SP(2);
        int colw = (g.w - 2 * g.pad - (g.cols - 1) * colgap) / g.cols;
        int rows = g.body_h / g.line_h;
        int first = (f->dlg == DLG_PROPS && g.cols == 1) ? f->prop_scroll : 0;
        if (first >= nl) first = 0;
        for (int i = first; i < nl; i++) {
            int col = per_col > 0 ? i / per_col : 0;
            int row = per_col > 0 ? i % per_col : i;
            if (col >= g.cols || row >= rows) continue;
            float lx = (float)(x + g.x + g.pad + col * (colw + colgap));
            float ly = (float)(y + g.body_y + row * g.line_h) +
                       (float)ui_top(fs, g.line_h);
            draw_clipped(s, fs, lx, ly, (float)colw, lines[i],
                         color_alpha(th->text, 0.86f));
        }
    }

    if (g.has_b1)
        ui_button(s, x + g.b1x, y + g.b1y, g.b1w, g.b1h, (float)g.pad,
                  "Delete", fs, 1, th);
    ui_button(s, x + g.b2x, y + g.b2y, g.b2w, g.b2h, (float)g.pad,
              f->dlg == DLG_DELETE ? "Cancel" : "Close", fs, 0, th);
}

static void dlg_close(Files *f) {
    int was_delete = (f->dlg == DLG_DELETE);
    f->dlg = DLG_NONE;
    if (was_delete) statusf(f, "delete cancelled");
}

static void dlg_click(Files *f, int cx, int cy, int buttons) {
    (void)buttons;
    DlgGeom g;
    if (!dialog_geom(f, &g)) { f->dlg = DLG_NONE; return; }
    int in_b2 = cx >= g.b2x && cx < g.b2x + g.b2w &&
                cy >= g.b2y && cy < g.b2y + g.b2h;
    int in_b1 = g.has_b1 && cx >= g.b1x && cx < g.b1x + g.b1w &&
                cy >= g.b1y && cy < g.b1y + g.b1h;
    int outside = cx < g.x || cx >= g.x + g.w || cy < g.y || cy >= g.y + g.h;

    if (f->dlg == DLG_DELETE && in_b1) { delete_confirm(f); return; }
    if (in_b2 || outside) dlg_close(f);
}

/* Scrolling inside a panel: Properties lines and the preview body. */
static void dlg_scroll(Files *f, const DlgGeom *g, int delta) {
    int rows = g->body_h / g->line_h;
    if (rows < 1) rows = 1;
    if (f->dlg == DLG_PREVIEW) {
        int maxs = preview_line_count(f) - rows;
        if (maxs < 0) maxs = 0;
        f->preview_scroll += delta;
        if (f->preview_scroll < 0) f->preview_scroll = 0;
        if (f->preview_scroll > maxs) f->preview_scroll = maxs;
    } else if (f->dlg == DLG_PROPS) {
        char lines[12][200];
        int nl = dialog_lines(f, lines, 12);
        int maxs = (g->cols == 1) ? nl - rows : 0;
        if (maxs < 0) maxs = 0;
        f->prop_scroll += delta;
        if (f->prop_scroll < 0) f->prop_scroll = 0;
        if (f->prop_scroll > maxs) f->prop_scroll = maxs;
    }
}

static void dlg_key(Files *f, const char *bytes, int n) {
    DlgGeom g;
    int have = dialog_geom(f, &g);

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            int j = i + 2, arg = 0, have_arg = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') { arg = arg * 10 + (bytes[j] - '0'); have_arg = 1; j++; }
            if (j >= n) break;
            char fin = bytes[j];
            if (have && (f->dlg == DLG_PREVIEW || f->dlg == DLG_PROPS)) {
                int rows = g.body_h / g.line_h;
                if (rows < 1) rows = 1;
                if (fin == 'A') dlg_scroll(f, &g, -1);
                else if (fin == 'B') dlg_scroll(f, &g, 1);
                else if (fin == 'H') dlg_scroll(f, &g, -10000);
                else if (fin == 'F') dlg_scroll(f, &g, 10000);
                else if (fin == '~' && have_arg && arg == 5) dlg_scroll(f, &g, -rows);
                else if (fin == '~' && have_arg && arg == 6) dlg_scroll(f, &g, rows);
            }
            i = j;
            continue;
        }
        if (c == 0x1B) { dlg_close(f); return; }
        if (c == '\n' || c == '\r') {
            if (f->dlg == DLG_DELETE) delete_confirm(f);
            else dlg_close(f);
            return;
        }
        if (have && (f->dlg == DLG_PREVIEW || f->dlg == DLG_PROPS)) {
            if (c == 'j') dlg_scroll(f, &g, 1);
            else if (c == 'k') dlg_scroll(f, &g, -1);
        }
        if (c == 'q') { dlg_close(f); return; }
    }
}

/* ------------------------------------------------------------
 *  Menus: the same spacing unit and the same three fonts
 * ------------------------------------------------------------ */
static void draw_chevron(Surface *s, float x, float y, float sz, u32 c) {
    gfx_line(s, x, y + sz * 0.18f, x + sz * 0.55f, y + sz * 0.5f, sz * 0.16f, c);
    gfx_line(s, x + sz * 0.55f, y + sz * 0.5f, x, y + sz * 0.82f, sz * 0.16f, c);
}

static void draw_tick(Surface *s, float x, float y, float sz, u32 c) {
    gfx_line(s, x, y + sz * 0.52f, x + sz * 0.38f, y + sz * 0.88f, sz * 0.15f, c);
    gfx_line(s, x + sz * 0.38f, y + sz * 0.88f, x + sz, y + sz * 0.10f, sz * 0.15f, c);
}

static void draw_menu(Surface *s, int x, int y, const Menu *m, const Theme *th) {
    if (!m->open) return;
    Font *f = ui_fonts().body;
    if (!f || m->w < 4 || m->h < 4) return;

    int mx = x + m->x, my = y + m->y;
    float radius = (float)SP(1);
    gfx_shadow(s, (float)mx, (float)my, (float)m->w, (float)m->h,
               radius, (float)SP(1), 0.45f);
    gfx_rrect_aa(s, (float)mx, (float)my, (float)m->w, (float)m->h,
                 radius, th->window_bg,
                 color_alpha(th->panel_border, 0.95f), 1.0f);

    for (int i = 0; i < m->n; i++) {
        const MenuItem *it = &m->item[i];
        int iy = my + it->y0;
        int ih = it->y1 - it->y0;
        if (ih <= 0) continue;
        if (it->sep_before && i > 0) {
            int sy = iy - m->sep_h / 2;
            surf_blend(s, mx + m->pad / 2, sy, m->w - m->pad, ui_px(1),
                       color_alpha(th->panel_border, 0.75f));
        }
        if (iy >= my + m->h) break;

        if (m->focus == i && it->enabled)
            ui_pill(s, mx + SP(1) / 2, iy + 1, m->w - SP(1), ih - 2,
                    (float)SP(1), color_alpha(th->accent, 0.24f));

        int ty = iy + ui_top(f, ih);
        int lx = mx + m->pad + m->check_w;
        float maxw = (float)(mx + m->w - m->pad) - (float)lx;
        if (m->arrow_w) maxw -= (float)m->arrow_w;
        if (it->shortcut[0]) {
            float sw = font_width(f, it->shortcut);
            ui_text_right(s, f, mx + m->w - m->pad - m->arrow_w, ty,
                          it->shortcut, color_alpha(th->text, 0.42f));
            maxw -= sw + (float)SP(1);
        }
        if (it->checked) {
            float sz = (float)SP(2);
            draw_tick(s, (float)(mx + m->pad), (float)iy + ((float)ih - sz) * 0.5f,
                      sz, th->accent);
        }
        u32 col = it->enabled ? (m->focus == i ? th->text : color_alpha(th->text, 0.86f))
                              : color_alpha(th->text, 0.30f);
        draw_clipped(s, f, (float)lx, (float)ty, maxw, it->label, col);
        if (it->submenu) {
            float sz = (float)SP(2);
            float cxp = (float)(mx + m->w - m->pad - sz);
            draw_chevron(s, cxp, (float)iy + ((float)ih - sz) * 0.5f, sz,
                         color_alpha(th->text, 0.55f));
        }
    }
}

/* ------------------------------------------------------------
 *  The inline editor, drawn inside the row it edits
 * ------------------------------------------------------------ */
static void draw_inline_input(Files *f, Surface *s, int x, int box_x, int ry,
                              int rh, int box_w, const Layout *L, const Theme *th) {
    Font *fnt = L->f.body;
    if (!fnt || box_w < SP(3)) return;
    float bx = (float)(x + box_x);
    float by = (float)ry + (float)SP(1) / 2;
    float bh = (float)(rh - SP(1));
    gfx_rrect_aa(s, bx, by, (float)box_w, bh, (float)L->radius,
                 th->window_bg, color_alpha(th->accent, 0.90f), 1.0f);

    /* Keep the caret visible: walk the start of the string forward until
     * the text before the caret fits in the box. */
    int start = 0;
    char tmp[NAME_LEN];
    for (;;) {
        int len = f->ti.caret - start;
        if (len <= 0) break;
        if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
        memcpy(tmp, f->ti.buf + start, (size_t)len);
        tmp[len] = 0;
        if (font_width(fnt, tmp) <= (float)box_w - (float)SP(2)) break;
        start++;
    }

    float tx = bx + (float)SP(1);
    float ty = (float)ry + (float)L->row_dy;
    int vis = f->ti.len - start;
    if (vis < 0) vis = 0;
    if (vis >= (int)sizeof(tmp)) vis = (int)sizeof(tmp) - 1;
    memcpy(tmp, f->ti.buf + start, (size_t)vis);
    tmp[vis] = 0;
    draw_clipped(s, fnt, tx, ty, (float)box_w - (float)SP(2), tmp, th->text);

    int caret = f->ti.caret - start;
    if (caret < 0) caret = 0;
    if (caret > vis) caret = vis;
    memcpy(tmp, f->ti.buf + start, (size_t)caret);
    tmp[caret] = 0;
    float cxp = tx + font_width(fnt, tmp);
    float caret_h = (float)rh - (float)SP(2);
    if (caret_h < 4.0f) caret_h = 4.0f;
    gfx_line(s, cxp, (float)ry + (float)SP(1), cxp,
             (float)ry + (float)SP(1) + caret_h, 1.4f, th->accent);
}

/* ------------------------------------------------------------
 *  Icons - drawn geometry, the launcher's style (src/main.c)
 * ------------------------------------------------------------ */
static void nav_icon(Surface *s, int which, float x, float y, float sz, u32 c) {
    if (which == 0) {                       /* back: an arrow to the left */
        gfx_line(s, x + sz * 0.20f, y + sz * 0.5f, x + sz * 0.64f, y + sz * 0.5f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.22f, y + sz * 0.5f,
                         x + sz * 0.60f, y + sz * 0.26f,
                         x + sz * 0.60f, y + sz * 0.74f };
        gfx_polygon(s, pts, 3, c);
    } else {                                /* up: an arrow to the parent */
        gfx_line(s, x + sz * 0.5f, y + sz * 0.40f, x + sz * 0.5f, y + sz * 0.78f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.5f, y + sz * 0.18f,
                         x + sz * 0.74f, y + sz * 0.50f,
                         x + sz * 0.26f, y + sz * 0.50f };
        gfx_polygon(s, pts, 3, c);
    }
}

/* gear - the launcher's icon_gear() idea, fattened: six thick teeth and a
 * thick ring, which is the only way that shape survives an 8 px radius. */
static void mark_gear(Surface *s, float cx, float cy, float r, u32 c) {
    float w = r * 0.50f;
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
 * body itself. Used for every directory row. */
static void draw_folder(Surface *s, float x, float y, float sz, u32 fg) {
    float w = sz * 0.90f, h = sz * 0.60f;
    float bx = x + (sz - w) * 0.5f, by = y + (sz - h) * 0.5f + sz * 0.07f;
    gfx_rrect(s, bx, by - sz * 0.15f, w * 0.46f, sz * 0.18f, sz * 0.04f, fg);
    gfx_rrect_aa(s, bx, by, w, h, sz * 0.10f, fg, 0, 0);
}

static void draw_entry_icon(Surface *s, const Entry *e, float x, float y, float sz,
                            int sel, const Theme *th) {
    if (sz < 6.0f) return;

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
    float my = py + fold + (h - fold) * 0.14f;
    float mh = (h - fold) * 0.70f;
    float mx = px + w * 0.15f, mw = w * 0.70f;

    switch (e->kind) {
    case IC_CONF:
        mark_gear(s, px + w * 0.5f, my + mh * 0.5f, mh * 0.60f, mark);
        break;
    case IC_THEME: {
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
        break;
    }
}

/* a plus mark for the header's one action */
static void draw_plus(Surface *s, float x, float y, float sz, u32 c) {
    float t = sz * 0.14f;
    if (t < 1.0f) t = 1.0f;
    gfx_rrect(s, x, y + sz * 0.5f - t * 0.5f, sz, t, t * 0.5f, c);
    gfx_rrect(s, x + sz * 0.5f - t * 0.5f, y, t, sz, t * 0.5f, c);
}

/* ------------------------------------------------------------
 *  Drawing
 * ------------------------------------------------------------ */

/* One header band: the app's name on the left and exactly one action on
 * the right, on the same margin as the values in the rows. */
static void draw_header(Files *f, Surface *s, int x, int y, int w,
                        const Layout *L, const Theme *th) {
    if (L->header_h <= 0) return;
    surf_blend(s, x, y, w, L->header_h, th->panel);
    font_draw(s, L->f.title, (float)(x + L->title_x), (float)(y + L->title_dy),
              "Files", th->text);

    int bx, by, bw, bh;
    if (act_rect(L, &bx, &by, &bw, &bh)) {
        int hot = f->px >= bx && f->px < bx + bw && f->py >= by && f->py < by + bh;
        const char *lab = L->act_label ? "New folder" : 0;
        gfx_rrect_aa(s, (float)(x + bx), (float)(y + by), (float)bw, (float)bh,
                     (float)L->radius,
                     hot ? color_alpha(th->accent, 0.28f)
                         : color_alpha(th->text, 0.07f),
                     hot ? color_alpha(th->accent, 0.85f)
                         : color_alpha(th->panel_border, 0.85f), 1.0f);
        u32 fg = hot ? th->text : color_alpha(th->text, 0.86f);
        if (lab) {
            float tw = font_width(L->f.small, lab);
            font_draw(s, L->f.small,
                      (float)(x + bx) + ((float)bw - tw) * 0.5f,
                      (float)(y + by) + (float)ui_top(L->f.small, bh), lab, fg);
        } else {
            float sz = (float)SP(2);
            draw_plus(s, (float)(x + bx) + ((float)bw - sz) * 0.5f,
                      (float)(y + by) + ((float)bh - sz) * 0.5f, sz, fg);
        }
    }
    surf_blend(s, x, y + L->header_h - L->rule, w, L->rule,
               color_alpha(th->panel_border, 0.80f));
}

/* The path band: back, up, then the real path as crumbs. */
static void draw_path(Files *f, Surface *s, int x, int y, int w,
                      const Layout *L, const Theme *th) {
    if (L->path_h <= 0) return;
    int py = y + L->header_h;

    for (int which = 0; which < 2; which++) {
        int bx, by, bw, bh;
        if (!nav_rect(L, which, &bx, &by, &bw, &bh)) continue;
        int hot = f->px >= bx && f->px < bx + bw && f->py >= by && f->py < by + bh;
        if (hot)
            ui_pill(s, x + bx, py + by, bw, bh, (float)L->radius,
                    color_alpha(th->accent, 0.20f));
        nav_icon(s, which, (float)(x + bx) + (float)SP(1) * 0.5f,
                 (float)(py + by) + (float)SP(1) * 0.5f,
                 (float)(bw - SP(1)), color_alpha(th->text, hot ? 0.95f : 0.62f));
    }

    Crumb cr[MAX_CRUMBS];
    int dots = 0;
    int n = crumb_build(f, L, cr, &dots);
    int ty = py + L->path_dy;

    if (dots)
        draw_clipped(s, L->f.body, (float)(x + L->path_x), (float)ty,
                     (float)(L->path_r - L->path_x), "...",
                     color_alpha(th->text, 0.45f));

    for (int i = 0; i < n; i++) {
        char lab[NAME_LEN];
        int len = cr[i].ll;
        if (len >= (int)sizeof(lab)) len = (int)sizeof(lab) - 1;
        if (len > 0) memcpy(lab, f->path + cr[i].ls, (size_t)len);
        lab[len] = 0;
        const char *text = len ? lab : "/";

        float pill_x, pill_w;
        crumb_pill(L, &cr[i], &pill_x, &pill_w);
        int last = (i == n - 1);
        int hov = f->py >= py && f->py < py + L->path_h && pill_w > 0.0f &&
                  (float)f->px >= pill_x && (float)f->px < pill_x + pill_w;
        if (hov)
            ui_pill(s, (int)((float)x + pill_x), py + L->pill_y,
                    (int)pill_w, L->pill_h, (float)L->radius,
                    color_alpha(th->accent, 0.18f));

        u32 col = (last || hov) ? th->text : color_alpha(th->text, 0.62f);
        draw_clipped(s, L->f.body, (float)x + cr[i].x0, (float)ty,
                     cr[i].x1 - cr[i].x0, text, col);

        if (i + 1 < n) {
            float sz = (float)L->chev_sz;
            float cxp = (float)x + cr[i].x1 +
                        (float)(L->chev_cell - L->chev_sz) * 0.5f;
            if (cxp + sz > (float)(x + L->path_r)) cxp = (float)(x + L->path_r) - sz;
            draw_chevron(s, cxp, (float)py + ((float)L->path_h - sz) * 0.5f, sz,
                         color_alpha(th->text, 0.30f));
        }
    }
    surf_blend(s, x + L->m, py + L->path_h - L->rule, w - 2 * L->m, L->rule,
               color_alpha(th->panel_border, 0.55f));
}

static void draw_list(Files *f, Surface *s, int x, int y, int w,
                      const Layout *L, const Theme *th) {
    int top = y + L->list_y;

    if (f->error[0]) {
        draw_clipped(s, L->f.body, (float)(x + L->m),
                     (float)(top + L->row_dy), (float)(w - 2 * L->m),
                     f->error, th->accent);
        return;
    }
    if (f->count == 0) {
        draw_clipped(s, L->f.body, (float)(x + L->m),
                     (float)(top + L->row_dy), (float)(w - 2 * L->m),
                     "This folder is empty", th->text_dim);
        if (f->ti.mode == TI_MKDIR && L->rows > 0) {
            draw_inline_input(f, s, x, L->name_x, top, L->row_h, L->name_w, L, th);
        }
        return;
    }

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
        if (!row_rect(L, i, &ryt, &rh)) break;
        int idx = f->scroll + i;
        if (idx >= f->count) break;
        float ry = (float)(y + ryt);

        Entry *e = &f->ent[idx];
        int sel = (idx == f->sel);
        int marked = mark_at(f, idx);
        int hov = f->px >= 0 && f->px < w && f->py >= ryt && f->py < ryt + rh;

        /* hover and selection are the same rectangle, different colour */
        if (marked || sel)
            ui_pill(s, x + L->row_xpad, (int)ry + L->row_ypad,
                    w - 2 * L->row_xpad, rh - 2 * L->row_ypad, (float)L->radius,
                    color_alpha(th->accent, marked ? (sel ? 0.34f : 0.26f) : 0.18f));
        else if (hov)
            ui_pill(s, x + L->row_xpad, (int)ry + L->row_ypad,
                    w - 2 * L->row_xpad, rh - 2 * L->row_ypad, (float)L->radius,
                    color_alpha(th->text, 0.05f));

        draw_entry_icon(s, e, (float)(x + L->m),
                        ry + (float)(rh - L->icon_sz) * 0.5f,
                        (float)L->icon_sz, sel, th);

        if (f->ti.mode == TI_RENAME && f->ti.row == idx) {
            draw_inline_input(f, s, x, L->name_x, (int)ry, rh, L->name_w, L, th);
        } else {
            char name[NAME_LEN + 2];
            snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
            draw_clipped(s, L->f.body, (float)(x + L->name_x), ry + (float)L->row_dy,
                         (float)L->name_w, name,
                         marked || sel ? th->text : color_alpha(th->text, 0.88f));
        }

        if (L->show_size && !e->is_dir) {
            char sz[32];
            human_size(e->size, sz, sizeof(sz));
            ui_text_right_fit(s, L->f.small, x + L->size_right,
                              (int)ry + L->small_dy, (float)L->size_col, sz,
                              color_alpha(th->text, sel ? 0.85f : 0.52f));
        }
        if (L->show_time) {
            char ts[32];
            fmt_mtime(e->mtime, ts, sizeof(ts));
            ui_text_right(s, L->f.small, x + L->time_right,
                          (int)ry + L->small_dy, ts,
                          color_alpha(th->text, sel ? 0.66f : 0.42f));
        }
    }

    /* New folder edits in a floating row at the top of the list. */
    if (f->ti.mode == TI_MKDIR && L->rows > 0) {
        int ryt = y + L->list_y;
        ui_pill(s, x + L->row_xpad, ryt + L->row_ypad, w - 2 * L->row_xpad,
                L->row_h - 2 * L->row_ypad, (float)L->radius,
                color_alpha(th->accent, 0.20f));
        draw_folder(s, (float)(x + L->m),
                    (float)ryt + (float)(L->row_h - L->icon_sz) * 0.5f,
                    (float)L->icon_sz, color_alpha(th->accent, 0.85f));
        draw_inline_input(f, s, x, L->name_x, ryt, L->row_h, L->name_w, L, th);
    }

    /* Scrollbar: a map of the list, in the right margin. */
    if (L->rows > 0 && f->count > L->rows) {
        int band = L->list_bot - L->list_y;
        int bx = x + w - L->bar_w - L->bar_pad;
        if (bx > x && band > 0) {
            int thumb = band * L->rows / f->count;
            if (thumb < L->thumb_min) thumb = L->thumb_min;
            if (thumb > band) thumb = band;
            int span = band - thumb;
            int maxs = f->count - L->rows;
            int off = maxs > 0 ? span * f->scroll / maxs : 0;
            ui_pill(s, bx, y + L->list_y, L->bar_w, band,
                    (float)L->bar_w * 0.5f, color_alpha(th->text, 0.06f));
            ui_pill(s, bx, y + L->list_y + off, L->bar_w, thumb,
                    (float)L->bar_w * 0.5f, color_alpha(th->accent, 0.55f));
        }
    }
}

/* One line: what happened (or how many items), and the free space. The
 * selected entry's size is already in its row, so it is not repeated. */
static void draw_footer(Files *f, Surface *s, int x, int y, int w,
                        const Layout *L, const Theme *th) {
    if (L->footer_h <= 0) return;
    int fy = y + L->list_bot;
    surf_blend(s, x, fy, w, L->footer_h, th->panel);
    if (L->footer_h > L->rule)
        surf_blend(s, x, fy, w, L->rule, th->panel_border);

    int ty = fy + L->footer_dy;

    char right[48];
    right[0] = 0;
    if (f->has_free) {
        char fs[32];
        human_size((long long)f->free_bytes, fs, sizeof(fs));
        snprintf(right, sizeof(right), "free %s", fs);
        ui_text_right(s, L->f.small, x + w - L->m, ty, right,
                      color_alpha(th->text, 0.46f));
    }

    char left[128];
    if (f->status[0]) snprintf(left, sizeof(left), "%s", f->status);
    else if (f->nmarked > 1)
        snprintf(left, sizeof(left), "%d of %d%s selected", f->nmarked,
                 f->count, f->truncated ? "+" : "");
    else
        snprintf(left, sizeof(left), "%d%s item%s", f->count,
                 f->truncated ? "+" : "",
                 (f->count == 1 && !f->truncated) ? "" : "s");

    int maxw = w - 2 * L->m;
    if (right[0]) maxw -= ui_ceilf(font_width(L->f.small, right)) + L->gap;
    if (maxw < SP(4)) maxw = SP(4);
    draw_clipped(s, L->f.small, (float)(x + L->m), (float)ty, (float)maxw, left,
                 f->status[0] ? th->accent : color_alpha(th->text, 0.55f));
}

static void files_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Files *f = a->state;
    if (!f || !th) return;
    f->w = w;
    f->h = h;

    Layout L;
    if (!layout_build(&L, w, h)) return;

    draw_header(f, s, x, y, w, &L, th);
    draw_path(f, s, x, y, w, &L, th);
    draw_list(f, s, x, y, w, &L, th);
    draw_footer(f, s, x, y, w, &L, th);
    draw_focus(s, x, y, f, &L, th);

    if (f->menu.open) draw_menu(s, x, y, &f->menu, th);
    if (f->sub.open)  draw_menu(s, x, y, &f->sub, th);
    if (f->dlg != DLG_NONE) draw_dialog(f, s, x, y, th);
}

/* ------------------------------------------------------------
 *  Input
 * ------------------------------------------------------------ */
static void files_click(App *a, int cx, int cy, int buttons) {
    Files *f = a->state;
    if (!f) return;
    if (cx < 0 || cy < 0 || (f->w > 0 && cx >= f->w) || (f->h > 0 && cy >= f->h)) return;
    f->px = cx;
    f->py = cy;
    /* a click puts the keyboard focus back where it landed: the list, or the
     * button that was pressed */
    f->focus = FOCUS_LIST;

    int right  = (buttons & BTN_RIGHT) != 0;
    int middle = (buttons & BTN_MIDDLE) != 0;
    int shift  = (buttons & MOD_SHIFT) != 0;
    int ctrl   = (buttons & MOD_CTRL) != 0;

    Layout L;
    int have = layout_build(&L, f->w, f->h);

    /* 1. a dialog is modal: it owns every click while it is open */
    if (f->dlg != DLG_NONE) { dlg_click(f, cx, cy, buttons); return; }

    /* 2. the inline editor: a click inside the field keeps it, a click
     * anywhere else cancels it and is consumed */
    if (f->ti.mode != TI_NONE) {
        if (have) {
            if (f->ti.mode == TI_RENAME) {
                int vis = f->ti.row - f->scroll, ryt, rh;
                if (row_rect(&L, vis, &ryt, &rh) && cy >= ryt && cy < ryt + rh &&
                    cx >= L.name_x && cx < L.name_x + L.name_w) return;
            } else {
                if (cy >= L.list_y && cy < L.list_y + L.row_h &&
                    cx >= L.name_x && cx < L.name_x + L.name_w) return;
            }
        }
        ti_cancel(f);
        return;
    }

    /* 3. an open menu owns the click. */
    if (f->sub.open) {
        int hit = menu_hit(&f->sub, cx, cy);
        if (hit >= 0) { menu_activate(f, &f->sub, hit); return; }
        if (menu_inside(&f->sub, cx, cy)) return;
        if (menu_inside(&f->menu, cx, cy)) { f->sub.open = 0; return; }
        f->sub.open = 0;
        f->menu.open = 0;
        if (!right) return;
    } else if (f->menu.open) {
        int hit = menu_hit(&f->menu, cx, cy);
        if (hit >= 0) { menu_activate(f, &f->menu, hit); return; }
        if (menu_inside(&f->menu, cx, cy)) return;
        f->menu.open = 0;
        if (!right) return;
    }

    if (!have) return;

    /* the header's one action, and the two navigation buttons */
    if (!right && !middle) {
        int bx, by, bw, bh;
        if (act_rect(&L, &bx, &by, &bw, &bh) &&
            cx >= bx && cx < bx + bw && cy >= by && cy < by + bh) {
            ti_open_mkdir(f);
            return;
        }
        int nav = nav_at(&L, cx, cy);
        if (nav == 0) { nav_back(f); return; }
        if (nav == 1) { nav_up(f); return; }

        if (L.path_h > 0 && cy >= L.header_h && cy < L.header_h + L.path_h) {
            Crumb cr[MAX_CRUMBS];
            int dots = 0;
            int n = crumb_build(f, &L, cr, &dots);
            for (int i = n - 1; i >= 0; i--) {
                float pill_x, pill_w;
                crumb_pill(&L, &cr[i], &pill_x, &pill_w);
                if (pill_w <= 0.0f || (float)cx < pill_x ||
                    (float)cx >= pill_x + pill_w) continue;
                char target[PATH_LEN];
                int t = cr[i].target;
                if (t < 1) t = 1;
                if (t >= (int)sizeof(target)) t = (int)sizeof(target) - 1;
                memcpy(target, f->path, (size_t)t);
                target[t] = 0;
                nav_to(f, target, 1);
                return;
            }
            return;
        }
    }

    /* rows: list_row_at() is the inverse of the row_rect() the painter used */
    int row = list_row_at(&L, cy);
    int idx = (row >= 0) ? f->scroll + row : -1;
    if (idx >= f->count) idx = -1;

    if (right) {
        f->last_click_idx = -1;
        if (idx >= 0) {
            f->sel = idx;
            if (!mark_at(f, idx)) {
                marks_reset(f);
                mark_set(f, idx, 1);
                f->anchor = idx;
            }
            f->status[0] = 0;
            menu_open_row(f, idx, cx, cy);
        } else {
            menu_open_bg(f, cx, cy);
        }
        return;
    }

    if (middle) {
        if (idx >= 0) {
            f->sel = idx;
            if (!f->mark_mode) mark_toggle(f, idx);
            else mark_range(f, f->anchor >= 0 ? f->anchor : idx, idx);
            f->status[0] = 0;
        }
        return;
    }

    if (idx < 0) {
        marks_reset(f);
        f->last_click_idx = -1;
        f->status[0] = 0;
        return;
    }

    double t = mono_now();
    int twice = !ctrl && !shift && !f->mark_mode &&
                (idx == f->last_click_idx) &&
                (t - f->last_click_t) <= 0.45 &&
                (cx - f->last_click_x) > -8 && (cx - f->last_click_x) < 8 &&
                (cy - f->last_click_y) > -8 && (cy - f->last_click_y) < 8;

    f->sel = idx;
    f->status[0] = 0;

    if (ctrl) {
        mark_toggle(f, idx);
    } else if (shift || f->mark_mode) {
        mark_range(f, f->anchor >= 0 ? f->anchor : idx, idx);
    } else if (f->nmarked > 1 && mark_at(f, idx)) {
        f->anchor = idx;
    } else {
        marks_reset(f);
        mark_set(f, idx, 1);
        f->anchor = idx;
    }

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

/* Movement that respects the range mode: with 'v' on, moving extends the
 * marked range from the anchor (the keyboard equivalent of Shift+click);
 * with it off, a plain move collapses a previous set. */
static void cursor_move(Files *f, int delta) {
    if (f->mark_mode) {
        int before = f->sel;
        move_sel(f, delta);
        if (f->anchor < 0) f->anchor = before;
        mark_range(f, f->anchor, f->sel);
    } else {
        if (f->nmarked > 0) marks_reset(f);
        move_sel(f, delta);
    }
}

static void cursor_home(Files *f, int last) {
    if (f->count <= 0) return;
    int s = last ? f->count - 1 : 0;
    if (f->mark_mode) {
        if (f->anchor < 0) f->anchor = f->sel;
        f->sel = s;
        mark_range(f, f->anchor, f->sel);
    } else {
        if (f->nmarked > 0) marks_reset(f);
        f->sel = s;
    }
    f->status[0] = 0;
}

static void files_key(App *a, const char *bytes, int n) {
    Files *f = a->state;
    if (!f || !bytes || n <= 0) return;

    /* modal layers first: the editor, then a dialog, then a menu */
    if (f->dlg != DLG_NONE) { dlg_key(f, bytes, n); return; }
    if (f->ti.mode != TI_NONE) { ti_key(f, bytes, n); return; }
    if (f->menu.open) { menu_key(f, bytes, n); return; }

    Layout L;
    int have = layout_build(&L, f->w, f->h);
    int page = (have && L.rows > 1) ? L.rows - 1 : 1;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            int j = i + 2, arg = 0, have_arg = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') {
                arg = arg * 10 + (bytes[j] - '0');
                have_arg = 1;
                j++;
            }
            if (j >= n) break;
            char fin = bytes[j];
            if (fin == 'A')      cursor_move(f, -1);
            else if (fin == 'B') cursor_move(f, 1);
            else if (fin == 'C') open_selected(f);
            else if (fin == 'D') nav_up(f);
            else if (fin == 'H') cursor_home(f, 0);
            else if (fin == 'F') cursor_home(f, 1);
            else if (fin == 'Z') { f->kb = 1; f->focus = focus_next(&L, f->focus, -1); }
            else if (fin == '~' && have_arg) {
                if (arg == 5) cursor_move(f, -page);
                else if (arg == 6) cursor_move(f, page);
                else if (arg == 3) delete_ask(f);
                else if (arg == 1 || arg == 7) cursor_home(f, 0);
                else if (arg == 4 || arg == 8) cursor_home(f, 1);
            }
            i = j;
            continue;
        }
        if (c == 0x1B) {
            if (f->focus != FOCUS_LIST) { f->focus = FOCUS_LIST; }
            else if (f->mark_mode) { f->mark_mode = 0; statusf(f, "range mode off"); }
            else if (f->nmarked > 0) { marks_reset(f); statusf(f, "selection cleared"); }
            continue;
        }

        /* Tab walks the controls. It has to be handled before the switch on
         * characters: a Tab byte is 0x09 and used to do nothing at all, which
         * left the two navigation buttons and the header action reachable only
         * with a mouse. */
        if (c == 0x09) {
            f->kb = 1;
            f->focus = focus_next(&L, f->focus, +1);
            continue;
        }

        switch (c) {
            case 'j': cursor_move(f, 1); break;
            case 'k': cursor_move(f, -1); break;
            case '\n':
            case '\r':
                /* Enter activates whatever holds the focus */
                if (f->focus == FOCUS_BACK)        nav_back(f);
                else if (f->focus == FOCUS_UP)     nav_up(f);
                else if (f->focus == FOCUS_ACTION) ti_open_mkdir(f);
                else                               open_selected(f);
                break;
            case '\b':
            case 0x7F: nav_up(f); break;
            case ' ': {
                if (f->focus != FOCUS_LIST) {
                    if (f->focus == FOCUS_BACK)        nav_back(f);
                    else if (f->focus == FOCUS_UP)     nav_up(f);
                    else if (f->focus == FOCUS_ACTION) ti_open_mkdir(f);
                    break;
                }
                mark_toggle(f, f->sel);
                statusf(f, "%d selected", f->nmarked);
                break;
            }
            case 'v':
                f->mark_mode = !f->mark_mode;
                if (f->mark_mode) {
                    f->anchor = f->sel;
                    mark_set(f, f->sel, 1);
                    statusf(f, "range mode: j/k extend, v or Esc ends");
                } else {
                    statusf(f, "range mode off");
                }
                break;
            case 's': cycle_sort(f); break;
            case 'r': rescan(f); statusf(f, "refreshed %s", f->path); break;
            case 'b': nav_back(f); break;
            case 'h': nav_to(f, "/", 1); break;
            case 'n': ti_open_mkdir(f); break;
            case 'm': ti_open_rename(f, f->sel); break;
            case 'd': delete_ask(f); break;
            case 'i': props_open(f, f->sel); break;
            case 'H':
                f->show_hidden = !f->show_hidden;
                rescan(f);
                statusf(f, "%s hidden files", f->show_hidden ? "showing" : "hiding");
                break;
            case 0x01: mark_all(f); statusf(f, "%d selected", f->nmarked); break;
            case 0x03: do_copy(f, f->sel); break;
            case 0x18: do_cut(f, f->sel); break;
            case 0x16: do_paste(f, f->sel); break;
            case 0x1A: undo_delete(f); break;
            default: break;
        }
    }
}

/* The window's wheel hook (AppVT.wheel). Positive notches scroll up, as
 * the desktop delivers them. The view moves under the pointer by whole
 * rows; the highlighted row is pulled to the nearest edge when the view
 * passes it, so the selection is always on screen. */
static void files_wheel(App *a, int notches) {
    Files *f = a->state;
    if (!f || notches == 0 || f->count <= 0) return;
    if (f->dlg != DLG_NONE || f->menu.open || f->ti.mode != TI_NONE) return;

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
    f->marked = calloc(MAX_ENTRIES, 1);
    if (!f->ent || !f->marked) { free(f->ent); free(f->marked); free(f); return; }

    f->w = w > 0 ? w : 0;
    f->h = h > 0 ? h : 0;
    f->px = f->py = -1;
    f->last_click_idx = -1;
    f->anchor = -1;
    f->sort = SORT_NAME;
    snprintf(f->path, sizeof(f->path), "/");
    scan(f);
    if (f->count > 0) { f->sel = 0; mark_set(f, 0, 1); f->anchor = 0; }
    a->state = f;
}

static void files_free(App *a) {
    Files *f = a->state;
    if (!f) return;
    a->state = NULL;
    free(f->ent);
    free(f->marked);
    free(f);
}

const AppVT app_vt_files = {
    .id = "files", .title = "Files",
    .init = files_init, .draw = files_draw, .key = files_key,
    .click = files_click, .resize = files_resize,
    .wheel = files_wheel, .free = files_free,
};
