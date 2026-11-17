/* ============================================================
 *  Mystic OS - apps/settings.c
 *
 *  Settings is one window with seven pages. Every page is a real
 *  control over the running system, and every value on it is read
 *  from the file or the syscall named beside it - nothing here is
 *  remembered, invented or rounded from a guess.
 *
 *  Pages, and exactly what each one reads and writes:
 *
 *      Text size   values 0.8 .. 1.6, the range ui_scale_load()
 *                  accepts (src/ui.c:58-59). 1.0 is the size the
 *                  desktop ships at (UI_BASE 1.4 in src/ui.c:23).
 *                  apply_size() writes "baseline = 1.4" and
 *                  "scale = <v>" to /etc/mystic/ui.conf.  The marker
 *                  matters: ui_scale_load() treats a file without it
 *                  as an old absolute scale and divides by 1.4
 *                  (src/ui.c:27-39), so a bare "scale = 1.2" was
 *                  silently converted to 0.9.  With the marker the
 *                  chosen value is the value the desktop uses.
 *
 *      Theme       every *.theme in /etc/mystic/themes, parsed with
 *                  theme_load() - the very function main.c uses - so
 *                  the swatch fill, bottom colour, accent border and
 *                  text bar are the colours in the real file.  The
 *                  list is rescanned every frame, so editing a theme
 *                  file is visible on the next repaint.  The "now"
 *                  row is the Theme the desktop passes to this draw
 *                  call, so even the Ctrl+1..9 shortcut (which never
 *                  writes the file, src/main.c:1901-1911) is shown
 *                  truthfully.  Enter writes the file's "name ="
 *                  value to /etc/mystic/current-theme, which main.c
 *                  matches against Theme::name (src/main.c:2332-2343).
 *                  The desktop loads its array once at boot
 *                  (src/main.c:1812), so a theme file ADDED to the
 *                  directory while it runs is listed here but could
 *                  not be applied until a restart; an EDITED file's
 *                  colours do show up on the next frame.
 *
 *      Wallpaper   every wallpaper wallpaper_name() implements, by the
 *                  name that function returns (Dusk, Mesh, Waves,
 *                  Orbits, Black Hole, Nebula), so a wallpaper added
 *                  to src/wallpaper.c gets a row here without this
 *                  file changing.  Enter writes the name to
 *                  /etc/mystic/wallpaper, re-read by main.c once a
 *                  second (src/main.c:2284-2291).
 *
 *      Users       changes the lock-screen password.  It asks for the
 *                  current one and then twice for the new one, checks
 *                  the current one with mystic_check_password() -
 *                  the same call try_unlock() makes (src/main.c:1192)
 *                  - generates a 16-byte salt from /dev/urandom,
 *                  hashes with mystic_make_password_line() at the
 *                  20000 rounds the shipped file uses
 *                  (stage/etc/mystic/passwd), writes the line through
 *                  a temp file and rename() to /etc/mystic/passwd,
 *                  then reads the file back and verifies the new
 *                  password against it.  Nothing typed is echoed:
 *                  the field draws one '*' per byte, never the byte.
 *                  The last result stays on the page once the prompt
 *                  closes, so a change, a refusal and a cancel are all
 *                  visible after the fact.
 *
 *                  /etc/mystic lives in the RAM rootfs, so the new
 *                  line survives until reboot only if something
 *                  mirrors it to the disk: stage/init:restore_settings
 *                  copies /root/mystic-<name> back for the three names
 *                  it lists (ui.conf, wallpaper, current-theme).
 *                  Adding "passwd" to that list, and to
 *                  persist_settings() in src/main.c, is what makes a
 *                  password change permanent; this file writes the
 *                  real file the lock screen reads (src/main.c:1177).
 *
 *      Network     the interface of the default route from
 *                  /proc/net/route, its real IPv4 address from
 *                  SIOCGIFADDR, the gateway decoded from the route's
 *                  little-endian hex field, operstate from
 *                  /sys/class/net/<if>/operstate, and the nameserver
 *                  lines from /etc/resolv.conf.  A source that cannot
 *                  be read says why (strerror) instead of showing
 *                  zeros.
 *
 *      Storage     statvfs(2) on / and /root: f_blocks * f_frsize,
 *                  f_bfree * f_frsize, and the real percentage.
 *
 *      System      the framebuffer geometry, memory, kernel release
 *                  and uptime, from /sys/class/graphics/fb0,
 *                  /proc/meminfo, /proc/sys/kernel/osrelease and
 *                  /proc/uptime.
 *
 *  LOOK - one header with the app's name and no action, then the seven
 *  pages either as a left rail (when the window is tall enough to show
 *  all of them without scrolling) or as a single page bar with two
 *  arrows.  A page is one column of rows: the label on the left, the
 *  value right-aligned on the one right edge, and a mark gutter that
 *  holds the theme's real colours or a tick on the row in effect.  The
 *  one footer line says what Enter will do, or where the facts came
 *  from, or the result of the last write.  Every padding and gap is a
 *  multiple of the 8 px unit in SP(); the only three fonts are the
 *  shared title/body/small faces; every row is ui_row_h() tall and
 *  centred by ui_top(), so the text scale cannot make rows overlap.
 *  Columns are dropped by measurement when a window is too narrow.
 *
 *  Input:
 *      Up/Down, j/k        move the selection on a page with controls
 *      Left/Right, Tab     change page
 *      Enter               apply the selected row (or submit a password)
 *      digits 1..9         apply that row of Text size / Theme / Wallpaper
 *      wheel               move the selection (a page of facts scrolls)
 *      Ctrl+C              cancel a password prompt (Esc is consumed by
 *                          main.c before an app sees it)
 *      click a page        change page (rail, arrow or the page name)
 *      click a row         select and apply it
 *      click Cancel/Submit in the password prompt
 *  ============================================================ */
#include "mystic.h"
#include "wallpaper.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* The files the desktop watches. Macros so a host harness can run this app
 * against a scratch directory; the desktop build uses the real paths. */
#ifndef MYSTIC_THEME_DIR
#define MYSTIC_THEME_DIR "/etc/mystic/themes"
#endif
#ifndef MYSTIC_UI_CONF
#define MYSTIC_UI_CONF "/etc/mystic/ui.conf"
#endif
#ifndef MYSTIC_WALLPAPER_FILE
#define MYSTIC_WALLPAPER_FILE "/etc/mystic/wallpaper"
#endif
#ifndef MYSTIC_CURRENT_THEME
#define MYSTIC_CURRENT_THEME "/etc/mystic/current-theme"
#endif
#ifndef MYSTIC_PASSWD
#define MYSTIC_PASSWD "/etc/mystic/passwd"
#endif
#ifndef MYSTIC_RESOLV
#define MYSTIC_RESOLV "/etc/resolv.conf"
#endif
/* The kernel's own two sources are guarded the same way, so a host probe can
 * drive the real parsers with a captured /proc/net/route and a scratch sysfs
 * tree. The desktop build has no definition of these and so reads the real
 * kernel, exactly like the MYSTIC_* paths above. */
#ifndef MYSTIC_ROUTE
#define MYSTIC_ROUTE "/proc/net/route"
#endif
#ifndef MYSTIC_SYS_NET
#define MYSTIC_SYS_NET "/sys/class/net"
#endif

/* src/main.c loads at most this many theme files: `static Theme themes[16]`
 * filled by theme_load_all(themes, 16, THEME_DIR) (src/main.c:105,1812).
 * Listing more than the desktop can load would offer rows that cannot be
 * applied, so these two numbers are the same number. Twelve themes ship. */
#define MAX_THEMES 16

/* The five values ui_scale_load() accepts after its clamp (src/ui.c:58-59).
 * 1.0 is relative to the shipped readable size, UI_BASE = 1.4 (src/ui.c:23),
 * which is the marker value written below. */
#define SIZE_COUNT 5
static const char  *SIZE_LABEL[SIZE_COUNT] = { "Small", "Normal", "Large", "Larger", "Huge" };
static const float  SIZE_VALUE[SIZE_COUNT] = { 0.8f, 1.0f, 1.2f, 1.4f, 1.6f };
#define SETTINGS_BASELINE 1.4f

/* The lock screen's pin[64] (src/main.c:167) holds at most 63 bytes, and
 * mystic_hash_password() also truncates at 64, so accepting more here would
 * create a password the lock screen could never match. */
#define PW_MAX     63
#define PW_ROUNDS  20000        /* rounds in stage/etc/mystic/passwd */

#define MAX_PAGE_ROWS 40

enum { TAB_TEXT, TAB_THEME, TAB_WALL, TAB_USERS, TAB_NET, TAB_STORAGE,
       TAB_SYSTEM, TAB_COUNT };
static const char *TAB_LABEL[TAB_COUNT] = {
    "Text", "Theme", "Wallpaper", "Users", "Network", "Storage", "System"
};

typedef struct {
    char  file[64];
    Theme theme;                /* parsed by theme_load(): the real colours */
} ThemeRow;

/* One row of whichever page is showing. Built from real data each frame.
 * id >= 0 marks a selectable picker row (Enter applies it); id == -1 is a
 * fact row, which has a value but no action. */
typedef struct {
    int         id;
    char        label[192];
    char        value[224];
    int         current;        /* this row is what is in effect on disk */
    const Theme *swatch;        /* non-NULL: draw this theme's colours here */
} PRow;

typedef struct {
    ThemeRow themes[MAX_THEMES];
    int      theme_count;
    int      theme_errno;       /* errno from opendir(MYSTIC_THEME_DIR) */
    int      tab;
    int      sel;
    int      w, h;
    int      scroll;            /* first visible body row (whole rows only) */
    int      px, py;            /* last pointer position reported to us */
    char     status[192];       /* real result of the last write */
    int      status_err;

    /* password flow: 0 idle, 1 old, 2 new, 3 repeat */
    int      pw_stage;
    int      pw_len;
    char     pw_buf[PW_MAX + 1];
    char     pw_first[PW_MAX + 1];
    char     pw_msg[192];
    int      pw_msg_err;

} Settings;

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


/* Copy src into out, shortened with ".." until it fits maxw. */
static void fit_text(Font *f, const char *src, char *out, size_t cap, float maxw) {
    /* src and out are allowed to be the same buffer (callers shorten in
     * place), so copy first: snprintf with overlapping source and
     * destination is undefined behaviour. */
    char tmp[512];
    if (cap == 0) return;
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    if (maxw <= 0.0f) { out[0] = 0; return; }
    snprintf(out, cap, "%s", tmp);
    if (font_width(f, out) <= maxw) return;
    size_t n = strlen(tmp);
    while (n > 0) {
        n--;
        snprintf(out, cap, "%.*s..", (int)n, tmp);
        if (font_width(f, out) <= maxw) return;
    }
    out[0] = 0;
}

/* Draw text clipped to maxw pixels, ending in ".." when it does not fit. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    char buf[512];
    if (!text || !text[0] || maxw < 4.0f) return;
    snprintf(buf, sizeof(buf), "%s", text);
    if (font_width(f, buf) <= maxw + 0.5f) {
        font_draw(s, f, x, y, buf, color);
        return;
    }
    size_t n = strlen(buf);
    while (n > 0) {
        buf[n] = 0;
        if (font_width(f, buf) + font_width(f, "..") <= maxw) break;
        n--;
    }
    snprintf(buf + n, sizeof(buf) - n, "..");
    font_draw(s, f, x, y, buf, color);
}

/* Best effort: the password buffers are cleared before the page moves on. */
static void secure_zero(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* The page on one grid. The navigation is a rail down the left when the
 * window is tall enough to show all seven pages without scrolling, and a
 * single page bar with two arrows when it is not - never a cramped strip
 * of wrapped tabs.
 *
 * Every x is one of three edges:
 *
 *      body_x        the content edge (the page-name column, or the
 *                    window margin in bar mode)
 *      label_x       the left edge of every label
 *      val_r         the right column: every value, right-aligned
 *
 * and every y is one of the bands, all built from the fonts and SP().
 */
typedef struct {
    UiFont f;

    int m, gap;
    int header_h, footer_h;

    int mode;                   /* 0 = rail, 1 = page bar */
    int rail_x, rail_y, rail_w;

    int bar_y, bar_h;
    int prev_x, next_x, arrow_w, name_x, name_w;

    int body_x, body_r, body_y, body_bot;
    int rows, row_h, row_dy, small_dy;

    /* the row grid: mark gutter, label, value */
    int gut, mark_sz, sw_w, sw_h;
    int label_x, val_r;

    int radius, rule, bar_w2, footer_dy;
} Layout;

/* Does every page name fit down the left side without scrolling? */
static int rail_rows_fit(int body_h, int row_h) {
    return body_h - 2 * SP(1) >= TAB_COUNT * row_h;
}

static int layout_settings(Layout *L, const Settings *st, int w, int h) {
    memset(L, 0, sizeof(*L));
    if (w < 1 || h < 1) return 0;

    L->f = ui_fonts();
    if (!L->f.title || !L->f.body || !L->f.small) return 0;

    L->m = SP(2);
    L->gap = SP(1);
    L->rule = ui_px(1);
    L->radius = SP(1);
    L->bar_w2 = ui_px(3);
    L->row_h = ui_row_h(L->f.body);
    L->row_dy = ui_top(L->f.body, L->row_h);
    L->small_dy = ui_top(L->f.small, L->row_h);

    L->header_h = ui_ceilf(font_height(L->f.title)) + 2 * SP(1);
    L->footer_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);
    if (L->header_h + L->footer_h + L->row_h > h) L->footer_h = 0;
    if (L->header_h + L->row_h > h) L->header_h = h;

    int top = L->header_h;
    int bottom = h - L->footer_h;
    if (bottom < top) bottom = top;

    L->footer_dy = L->footer_h > 0 ? ui_top(L->f.small, L->footer_h) : 0;

    /* one navigation column, chosen by what actually fits */
    L->mode = 1;
    if (rail_rows_fit(bottom - top, L->row_h) && w >= SP(40)) {
        int widest = 0;
        for (int i = 0; i < TAB_COUNT; i++) {
            int tw = ui_ceilf(font_width(L->f.body, TAB_LABEL[i]));
            if (tw > widest) widest = tw;
        }
        L->mode = 0;
        L->rail_w = widest + 2 * SP(2);
        if (L->rail_w > w - 2 * L->m - SP(20))
            L->rail_w = w - 2 * L->m - SP(20);
        if (L->rail_w < SP(8)) L->rail_w = SP(8);
        L->rail_x = L->m;
        L->rail_y = top + SP(1);
        L->body_x = L->rail_x + L->rail_w + SP(2);
        L->body_y = top + SP(1);
    } else {
        L->bar_h = L->row_h;
        L->bar_y = top + SP(1);
        L->arrow_w = SP(4);
        L->prev_x = L->m;
        L->next_x = w - L->m - L->arrow_w;
        L->name_x = L->prev_x + L->arrow_w + L->gap;
        L->name_w = L->next_x - L->gap - L->name_x;
        if (L->name_w < SP(6)) L->name_w = SP(6);
        L->body_x = L->m;
        L->body_y = L->bar_y + L->bar_h + SP(1);
    }
    L->body_r = w - L->m;
    L->body_bot = bottom - SP(1);
    if (L->body_bot < L->body_y) L->body_bot = L->body_y;

    /* the mark gutter only exists on pages that carry a mark */
    L->mark_sz = SP(2);
    L->sw_h = L->row_h - SP(1);
    if (L->sw_h > SP(3)) L->sw_h = SP(3);
    L->sw_w = SP(6);
    L->gut = 0;
    if (st->tab == TAB_THEME) L->gut = L->sw_w + L->gap;
    else if (st->tab == TAB_TEXT || st->tab == TAB_WALL) L->gut = L->mark_sz + L->gap;
    L->label_x = L->body_x + L->gut;
    L->val_r = L->body_r;

    {
        int avail = L->body_bot - L->body_y;
        L->rows = avail > 0 ? avail / L->row_h : 0;
        if (L->rows > MAX_PAGE_ROWS) L->rows = MAX_PAGE_ROWS;
    }
    return 1;
}

/* the rail entry (page i) */
static int rail_rect(const Layout *L, int i, int *bx, int *by, int *bw, int *bh) {
    if (L->mode != 0 || i < 0 || i >= TAB_COUNT) return 0;
    *bx = L->rail_x;
    *by = L->rail_y + i * L->row_h;
    *bw = L->rail_w;
    *bh = L->row_h;
    if (*by + *bh > L->rail_y + L->row_h * TAB_COUNT) return 0;
    return 1;
}

/* the page bar's two arrows */
static int arrow_rect(const Layout *L, int which, int *bx, int *by, int *bw, int *bh) {
    if (L->mode != 1) return 0;
    *bx = which ? L->next_x : L->prev_x;
    *by = L->bar_y;
    *bw = *bh = L->arrow_w;
    return 1;
}

/* visible body row i: the same rectangle the painter uses */
static int page_row_rect(const Layout *L, int i, int *ry, int *rh) {
    if (i < 0 || i >= L->rows || L->row_h <= 0) return 0;
    *ry = L->body_y + i * L->row_h;
    *rh = L->row_h;
    return (*ry + *rh <= L->body_bot);
}

/* the inverse: which visible body row holds cy, or -1 */
static int page_row_at(const Layout *L, int cy) {
    if (L->row_h <= 0 || cy < L->body_y || cy >= L->body_bot) return -1;
    int i = (cy - L->body_y) / L->row_h;
    int ry, rh;
    if (!page_row_rect(L, i, &ry, &rh)) return -1;
    return i;
}

/* The two buttons of the password prompt, in the footer band. Both the
 * painter and the click handler call this, so they cannot disagree. */
static int pw_btn_rect(const Layout *L, int w, int which,
                       int *bx, int *by, int *bw, int *bh) {
    if (L->footer_h <= 0) return 0;
    int h = ui_ceilf(font_height(L->f.small)) + SP(1);
    if (h > L->footer_h) h = L->footer_h;
    const char *lab = which ? "Submit" : "Cancel";
    int wd = ui_ceilf(font_width(L->f.small, lab)) + 2 * SP(2);
    *bw = wd;
    *bh = h;
    *by = -(L->footer_h - h) / 2;              /* relative to the footer top */
    if (which) *bx = w - L->m - wd;
    else       *bx = w - L->m - wd - SP(1) - wd;
    if (*bx < L->m) *bx = L->m;
    return 1;
}

/* ---- painting ------------------------------------------------------------ */

/* A theme swatch: the file's own bg_top as the rounded body, its bg_bottom as
 * the strip inside the lower half, its accent as the border and its text
 * colour as the short bar. Nothing is mixed with the current theme except
 * the alpha, so a porcelain swatch stays pale on an obsidian window. */
static void swatch_draw(Surface *s, float x, float y, float w, float h,
                        const Theme *t) {
    if (!t || w <= 1.0f || h <= 1.0f) return;
    float r = h * 0.30f;
    gfx_rrect_aa(s, x, y, w, h, r, t->bg_top, color_alpha(t->accent, 0.95f), 1.0f);

    float sh = h * 0.40f;
    float sx = x + 1.5f, sy = y + h - sh - 1.0f, sw = w - 3.0f;
    if (sw > 2.0f && sh > 1.0f)
        gfx_rrect_aa(s, sx, sy, sw, sh, r * 0.55f, t->bg_bottom, 0, 0);

    float tw = w * 0.34f, th = h * 0.15f;
    if (tw > 1.0f && th >= 1.0f)
        gfx_rrect_aa(s, x + w * 0.16f, y + h * 0.24f, tw, th, th * 0.5f,
                     color_alpha(t->text, 0.90f), 0, 0);
}

static void tick_draw(Surface *s, float x, float y, float sz, u32 c) {
    gfx_line(s, x, y + sz * 0.52f, x + sz * 0.38f, y + sz * 0.88f, sz * 0.16f, c);
    gfx_line(s, x + sz * 0.38f, y + sz * 0.88f, x + sz, y + sz * 0.10f, sz * 0.16f, c);
}

/* One body row: the label on its left column, the value right-aligned on
 * the one right edge. A theme row draws that theme's real colours in the
 * mark gutter; the current row of a picker draws a tick there. Hover and
 * selection are the same rectangle with a different colour. */
static void page_row_draw(Surface *s, const Layout *L, const Theme *th,
                          const PRow *r, int ry, int selectable, int sel) {
    if (sel)
        ui_pill(s, L->body_x, ry, L->body_r - L->body_x, L->row_h,
                (float)L->radius, color_alpha(th->accent, 0.18f));

    if (r->swatch) {
        float sy = (float)ry + ((float)L->row_h - (float)L->sw_h) * 0.5f;
        swatch_draw(s, (float)L->body_x, sy, (float)L->sw_w, (float)L->sw_h,
                    r->swatch);
    } else if (r->current && L->gut > 0) {
        float sy = (float)ry + ((float)L->row_h - (float)L->mark_sz) * 0.5f;
        tick_draw(s, (float)L->body_x + (float)SP(1), sy, (float)L->mark_sz,
                  th->accent);
    }

    char vb[256];
    snprintf(vb, sizeof(vb), "%s", r->value);
    float vw = 0.0f;
    if (vb[0]) {
        fit_text(L->f.small, vb, vb, sizeof(vb),
                 (float)(L->val_r - L->label_x) * 0.62f);
        if (vb[0]) vw = font_width(L->f.small, vb) + (float)L->gap;
    }

    char nb[256];
    fit_text(L->f.body, r->label, nb, sizeof(nb),
             (float)L->val_r - vw - (float)L->label_x);
    font_draw(s, L->f.body, (float)L->label_x, (float)(ry + L->row_dy), nb,
              selectable && sel ? th->text : color_alpha(th->text, 0.86f));
    if (vb[0])
        ui_text_right(s, L->f.small, L->val_r, ry + L->small_dy, vb,
                      r->current ? color_alpha(th->accent, 0.95f)
                                 : color_alpha(th->text, 0.55f));
}

/* ---- reading the facts back off the disk --------------------------------- */

/* The first line of a small file, newline stripped.
 * 1 = a non-empty line was read, 0 = missing or empty. */
static int first_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)n, f)) { fclose(f); return 0; }
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    nl = strchr(out, '\r');
    if (nl) *nl = 0;
    fclose(f);
    return out[0] != 0;
}

/* One line, with the caller told why when it cannot be read. read_file() is
 * for facts where the reason does not matter to the label. */
static int read_passwd(char *out, size_t n, int *err) {
    *err = 0;
    out[0] = 0;
    FILE *f = fopen(MYSTIC_PASSWD, "r");
    if (!f) { *err = errno ? errno : EIO; return -1; }
    if (!fgets(out, (int)n, f)) { fclose(f); return 0; }
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    fclose(f);
    return out[0] ? 1 : 0;
}

/* One line or a one-word failure, for System's facts. */
static void read_file(const char *path, char *out, size_t n) {
    out[0] = 0;
    if (!first_line(path, out, n)) snprintf(out, n, "%s", "unavailable");
}

/* One line at a time: a key such as "HugePages_Total: 0" must not stop the
 * scan before MemAvailable. */
static long mem_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    size_t klen = strlen(key);
    char line[256];
    long v = -1;                    /* -1: the key is not there */
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        v = strtol(line + klen, NULL, 10);
        break;
    }
    fclose(f);
    return v;
}

/* The scale the desktop is really using. This is ui_scale_load()'s own
 * algorithm (src/ui.c:44-68): the clamp, and the legacy conversion when the
 * "baseline" marker is absent. Returns 0 when there is no usable setting. */
static int read_scale(float *out) {
    *out = 1.0f;
    FILE *f = fopen(MYSTIC_UI_CONF, "r");
    if (!f) return 0;
    char line[256];
    int marker = 0, got = 0;
    float v = 1.0f;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "baseline", 8)) { marker = 1; continue; }
        if (strncmp(line, "scale", 5)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        float x = (float)atof(eq + 1);
        if (!isfinite(x)) continue;              /* NaN passes comparisons */
        if (x < 0.8f) x = 0.8f;
        if (x > 1.6f) x = 1.6f;
        v = x;
        got = 1;
        break;
    }
    fclose(f);
    if (!got) return 0;
    if (!marker) {
        v /= SETTINGS_BASELINE;
        if (v < 0.8f) v = 0.8f;
        if (v > 1.6f) v = 1.6f;
    }
    *out = v;
    return 1;
}

/* The wallpaper on disk. -2 = no file (the desktop keeps its default, 0),
 * -1 = a name no wallpaper matches, >= 0 = that wallpaper. */
static int read_wallpaper(void) {
    char line[128];
    if (!first_line(MYSTIC_WALLPAPER_FILE, line, sizeof(line))) return -2;
    return wallpaper_from_name(line);
}

/* ---- writing the facts back to the disk ---------------------------------- */

/* One small file, written whole: open, write, flush, close - every step
 * checked, and the caller gets the errno of whichever step failed. */
static int write_text(const char *path, const char *text, int *err) {
    *err = 0;
    FILE *f = fopen(path, "w");
    if (!f) { *err = errno ? errno : EIO; return 0; }
    errno = 0;
    int e = fputs(text, f) == EOF ? (errno ? errno : EIO) : 0;
    if (!e && fflush(f) != 0) e = errno ? errno : EIO;
    if (fclose(f) != 0 && !e) e = errno ? errno : EIO;
    *err = e;
    return e == 0;
}

static void apply_theme(Settings *s, int i) {
    if (i < 0 || i >= s->theme_count) return;
    const char *name = s->themes[i].theme.name;
    char text[160], err_txt[160];
    snprintf(text, sizeof(text), "%s\n", name);
    int err = 0;
    if (write_text(MYSTIC_CURRENT_THEME, text, &err)) {
        snprintf(s->status, sizeof(s->status), "theme: %s", name);
        s->status_err = 0;
    } else {
        snprintf(err_txt, sizeof(err_txt), "%s", strerror(err));
        snprintf(s->status, sizeof(s->status), "not applied: %s", err_txt);
        s->status_err = 1;
    }
}

static void apply_size(Settings *s, int i) {
    if (i < 0 || i >= SIZE_COUNT) return;
    char text[128], err_txt[160];
    /* The marker is part of the format: without it ui.c converts the file as
     * a legacy absolute scale and 1.2 would be used as 0.9. */
    snprintf(text, sizeof(text), "baseline = %.1f\nscale = %.1f\n",
             SETTINGS_BASELINE, SIZE_VALUE[i]);
    int err = 0;
    if (write_text(MYSTIC_UI_CONF, text, &err)) {
        snprintf(s->status, sizeof(s->status), "text size: %s (%.1fx)",
                 SIZE_LABEL[i], SIZE_VALUE[i]);
        s->status_err = 0;
    } else {
        snprintf(err_txt, sizeof(err_txt), "%s", strerror(err));
        snprintf(s->status, sizeof(s->status), "not saved: %s", err_txt);
        s->status_err = 1;
    }
}

static void apply_wallpaper(Settings *s, int i) {
    const char *name = wallpaper_name(i);
    if (!name) return;
    char text[128], err_txt[160];
    snprintf(text, sizeof(text), "%s\n", name);
    int err = 0;
    if (write_text(MYSTIC_WALLPAPER_FILE, text, &err)) {
        snprintf(s->status, sizeof(s->status), "wallpaper: %s", name);
        s->status_err = 0;
    } else {
        snprintf(err_txt, sizeof(err_txt), "%s", strerror(err));
        snprintf(s->status, sizeof(s->status), "not saved: %s", err_txt);
        s->status_err = 1;
    }
}

/* ---- the theme list ------------------------------------------------------ */

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Read every *.theme the desktop can load, in the same order: filename order
 * (theme_load_all sorts first, src/theme.c:363). theme_load() supplies the
 * real colours and the "name =" field, so the swatch and the applied name can
 * never disagree. Rescanned on every draw: the list is the disk. */
static void scan_themes(Settings *s) {
    char names[64][64];
    int nf = 0, err = 0;

    DIR *d = opendir(MYSTIC_THEME_DIR);
    if (!d) {
        err = errno ? errno : EIO;
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nf < 64) {
            size_t len = strlen(e->d_name);
            if (len < 7 || strcmp(e->d_name + len - 6, ".theme")) continue;
            if (len >= sizeof(names[0])) continue;
            snprintf(names[nf], sizeof(names[0]), "%s", e->d_name);
            nf++;
        }
        closedir(d);
    }
    qsort(names, (size_t)nf, sizeof(names[0]), cmp_names);

    int n = 0;
    if (nf > 0) {
        for (int i = 0; i < nf && n < MAX_THEMES; i++) {
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", MYSTIC_THEME_DIR, names[i]);
            Theme t;
            if (theme_load(&t, path) != 0) continue;   /* desktop skips it too */
            snprintf(s->themes[n].file, sizeof(s->themes[n].file), "%s", names[i]);
            s->themes[n].theme = t;
            n++;
        }
    }
    s->theme_count = n;
    s->theme_errno = err;
}

/* The theme the window is being drawn with is the Theme* the desktop hands
 * to settings_draw(); that is the live value, so it is used for the "now"
 * row instead of the current-theme file. That matters because Ctrl+1..9
 * changes theme_index without writing the file (src/main.c:1901-1911). */

/* ---- the password -------------------------------------------------------- */

/* 16 real random bytes from the kernel's pool. */
static int random_salt(u8 *out, int n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, (size_t)(n - got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            errno = e;
            return 0;
        }
        if (r == 0) { close(fd); errno = EIO; return 0; }
        got += (int)r;
    }
    close(fd);
    return 1;
}

/* Replace /etc/mystic/passwd atomically: a temp file in the same directory,
 * written and flushed, then rename(2). A power cut leaves either the old
 * line or the new one, never a truncated hash the lock screen cannot parse. */
static int write_passwd(const char *line, int *err) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.new", MYSTIC_PASSWD);
    *err = 0;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *err = errno ? errno : EIO; return 0; }

    size_t len = strlen(line), off = 0;
    while (off < len) {
        ssize_t w = write(fd, line + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            break;
        }
        if (w == 0) { *err = EIO; break; }
        off += (size_t)w;
    }
    if (!*err && fchmod(fd, 0600) != 0) *err = errno;
    if (!*err && fsync(fd) != 0) *err = errno;
    if (close(fd) != 0 && !*err) *err = errno;

    if (!*err && rename(tmp, MYSTIC_PASSWD) != 0) *err = errno;
    if (*err) unlink(tmp);
    return *err == 0;
}

/* Nothing typed is shown: one '*' per byte, and only a count-neutral
 * ellipsis past 24. That is a mask, not the password. */
static void pw_mask(const char *buf, int len, char *out, size_t n) {
    int k = len > 24 ? 24 : len;
    size_t p = 0;
    for (int i = 0; i < k && p + 1 < n; i++) out[p++] = '*';
    if (len > k && p + 4 < n) { out[p++] = '.'; out[p++] = '.'; out[p++] = '.'; }
    out[p] = 0;
    if (!out[0]) snprintf(out, n, "%s", "(empty)");
}

static const char *pw_prompt(const Settings *s) {
    switch (s->pw_stage) {
        case 1: return "Current password";
        case 2: return "New password";
        case 3: return "Repeat new password";
        default: return "";
    }
}

static void pw_set_msg(Settings *s, int err, const char *text) {
    snprintf(s->pw_msg, sizeof(s->pw_msg), "%s", text);
    s->pw_msg_err = err;
}

static void pw_clear(Settings *s) {
    s->pw_len = 0;
    s->pw_buf[0] = 0;
    secure_zero(s->pw_first, sizeof(s->pw_first));
    s->pw_stage = 0;
}

static void pw_cancel(Settings *s) {
    pw_clear(s);
    pw_set_msg(s, 0, "Cancelled - the password is unchanged.");
}

static void pw_start(Settings *s) {
    char line[256];
    int err = 0;
    int r = read_passwd(line, sizeof(line), &err);

    s->pw_len = 0;
    s->pw_buf[0] = 0;
    secure_zero(s->pw_first, sizeof(s->pw_first));
    s->pw_msg[0] = 0;
    s->pw_msg_err = 0;

    if (r < 0 && err != ENOENT) {
        snprintf(s->status, sizeof(s->status),
                 "cannot read %s: %s", MYSTIC_PASSWD, strerror(err));
        s->status_err = 1;
        return;
    }
    if (r <= 0) {
        /* No file or an empty one: try_unlock() fails open in that state
         * (src/main.c:1183-1188), so there is no old password to ask for. */
        s->pw_stage = 2;
        pw_set_msg(s, 0, "No password is set yet; choose one and type it twice.");
    } else {
        s->pw_stage = 1;
        pw_set_msg(s, 0, "Type the current lock-screen password.");
    }
}

static void pw_finish(Settings *s) {
    char rec[320];
    u8 salt[16];
    char line[256];

    if (!random_salt(salt, (int)sizeof(salt))) {
        int e = errno ? errno : EIO;
        pw_set_msg(s, 1, "New password not written: /dev/urandom failed.");
        snprintf(s->status, sizeof(s->status), "not saved: %s", strerror(e));
        s->status_err = 1;
        pw_clear(s);
        return;
    }

    mystic_make_password_line(salt, (int)sizeof(salt), s->pw_buf, PW_ROUNDS,
                              line, sizeof(line));
    snprintf(rec, sizeof(rec), "%s\n", line);

    int err = 0;
    if (!write_passwd(rec, &err)) {
        char why[96];
        snprintf(why, sizeof(why), "%s", strerror(err));
        pw_set_msg(s, 1, "New password not written - the old one still works.");
        snprintf(s->status, sizeof(s->status), "not saved: %s", why);
        s->status_err = 1;
        pw_clear(s);
        return;
    }

    /* Read the file back and check the new password against it with the very
     * function the lock screen calls. This is the proof that what was written
     * is a line try_unlock() will accept, not merely a successful write(). */
    char check[256];
    int rerr = 0;
    int r = read_passwd(check, sizeof(check), &rerr);
    if (r > 0 && mystic_check_password(check, s->pw_buf)) {
        pw_set_msg(s, 0, "Password changed. The file was read back and verified.");
        snprintf(s->status, sizeof(s->status),
                 "lock-screen password changed and verified");
        s->status_err = 0;
    } else {
        pw_set_msg(s, 1, "The file was written but did not verify - check it.");
        snprintf(s->status, sizeof(s->status),
                 "password written but read-back verification failed");
        s->status_err = 1;
    }
    pw_clear(s);
}

static void pw_enter(Settings *s) {
    if (s->pw_stage == 1) {
        char line[256];
        int err = 0;
        int r = read_passwd(line, sizeof(line), &err);
        if (r == 0) {
            /* the file is now empty: the lock fails open, so there is no
             * current password to verify. */
            s->pw_stage = 2;
            pw_set_msg(s, 0, "No password is set now; choose a new one.");
            s->pw_len = 0;
            s->pw_buf[0] = 0;
            return;
        }
        if (r < 0 && err == ENOENT) {
            /* the file disappeared while the prompt was open: try_unlock()
             * fails open in that state, so there is no old password left to
             * check. Move on to choosing the new one. */
            s->pw_stage = 2;
            pw_set_msg(s, 0, "No password is set now; choose a new one.");
            s->pw_len = 0;
            s->pw_buf[0] = 0;
            return;
        }
        if (r < 0) {
            snprintf(s->status, sizeof(s->status),
                     "cannot read %s: %s", MYSTIC_PASSWD, strerror(err));
            s->status_err = 1;
            return;
        }
        if (!mystic_check_password(line, s->pw_buf)) {
            pw_set_msg(s, 1, "That is not the current password.");
            s->pw_len = 0;
            s->pw_buf[0] = 0;
            return;
        }
        s->pw_stage = 2;
        pw_set_msg(s, 0, "Current password accepted. Type the new password.");
        s->pw_len = 0;
        s->pw_buf[0] = 0;
        return;
    }
    if (s->pw_stage == 2) {
        if (s->pw_len <= 0) {
            pw_set_msg(s, 1, "The password cannot be empty.");
            return;
        }
        snprintf(s->pw_first, sizeof(s->pw_first), "%s", s->pw_buf);
        s->pw_stage = 3;
        pw_set_msg(s, 0, "Type it again to confirm.");
        s->pw_len = 0;
        s->pw_buf[0] = 0;
        return;
    }
    if (s->pw_stage == 3) {
        if (strcmp(s->pw_buf, s->pw_first) != 0) {
            pw_set_msg(s, 1, "The two entries do not match. Start the new one again.");
            secure_zero(s->pw_first, sizeof(s->pw_first));
            s->pw_stage = 2;
            s->pw_len = 0;
            s->pw_buf[0] = 0;
            return;
        }
        pw_finish(s);
    }
}

/* ---- network ------------------------------------------------------------- */

typedef struct {
    char ifname[32];
    char state[32];
    char ip[96];
    char gw[96];
    char dns[192];
} NetInfo;

/* /proc/net/route prints the gateway as a host-order 32-bit value in hex, so
 * 0202A8C0 is 192.168.2.2. */
static void hex_ip(unsigned long v, char *out, size_t n) {
    snprintf(out, n, "%lu.%lu.%lu.%lu",
             v & 0xffUL, (v >> 8) & 0xffUL, (v >> 16) & 0xffUL, (v >> 24) & 0xffUL);
}

/* The default route: destination 0 and mask 0. Returns 1 and fills the
 * interface + gateway, or 0 and leaves errno-of-the-open in *err. */
static int route_default(char *ifname, size_t ifn, char *gw, size_t gwn, int *err) {
    *err = 0;
    ifname[0] = 0;
    gw[0] = 0;

    FILE *f = fopen(MYSTIC_ROUTE, "r");
    if (!f) { *err = errno ? errno : EIO; return 0; }

    char line[512];
    if (!fgets(line, sizeof(line), f)) { /* header, if any */ }
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char ifc[32];
        unsigned long dest = 0, gate = 0, flags = 0, mask = 0;
        int n = sscanf(line, "%31s %lx %lx %lx %*s %*s %*s %lx",
                       ifc, &dest, &gate, &flags, &mask);
        if (n < 4) continue;
        if (dest != 0) continue;
        if (n >= 5 && mask != 0) continue;
        /* Only a route with RTF_GATEWAY (0x0002) has a real next hop in the
         * Gateway column; a gateway-less default route prints 0 there. */
        if (gate && !(flags & 0x0002UL)) gate = 0;
        snprintf(ifname, ifn, "%s", ifc);
        if (gate) hex_ip(gate, gw, gwn);
        else snprintf(gw, gwn, "%s", "none");
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

/* A real interface when there is no default route: the first one in sysfs
 * that is not loopback, or loopback if that is all there is. */
static int first_iface(char *out, size_t n) {
    DIR *d = opendir(MYSTIC_SYS_NET);
    if (!d) return 0;
    struct dirent *e;
    char lo[32] = {0};
    int have_lo = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= n) continue;
        if (!strcmp(e->d_name, "lo")) {
            snprintf(lo, sizeof(lo), "%s", e->d_name);
            have_lo = 1;
            continue;
        }
        snprintf(out, n, "%s", e->d_name);
        closedir(d);
        return 1;
    }
    closedir(d);
    if (have_lo) { snprintf(out, n, "%s", lo); return 1; }
    return 0;
}

static void iface_state(const char *ifname, char *out, size_t n) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s/operstate", MYSTIC_SYS_NET, ifname);
    if (!first_line(path, out, n)) snprintf(out, n, "%s", "unknown");
}

/* The interface's own IPv4 address, from the kernel. */
static int iface_ipv4(const char *ifname, char *out, size_t n, int *err) {
    *err = 0;
    out[0] = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { *err = errno; return 0; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        *err = errno;
        close(fd);
        return 0;
    }
    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    const char *p = inet_ntop(AF_INET, &sin.sin_addr, out, (socklen_t)n);
    close(fd);
    if (!p) { *err = EINVAL; return 0; }
    return 1;
}

static void read_dns(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(MYSTIC_RESOLV, "r");
    if (!f) {
        int e = errno ? errno : ENOENT;
        snprintf(out, n, "no %s (%s)", MYSTIC_RESOLV, strerror(e));
        return;
    }
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < 2) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10) != 0) continue;
        p += 10;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ' && *end != '\t') end++;
        *end = 0;
        if (!*p) continue;

        size_t used = strlen(out);
        if (used) {
            if (used + 2 >= n) break;
            memcpy(out + used, ", ", 2);
            used += 2;
            out[used] = 0;
        }
        size_t pl = strlen(p);
        if (used + pl + 1 > n) break;
        memcpy(out + used, p, pl + 1);
        count++;
    }
    fclose(f);
    if (!out[0]) snprintf(out, n, "%s", "none in resolv.conf");
}

static void read_net(NetInfo *ni) {
    memset(ni, 0, sizeof(*ni));

    int rerr = 0;
    int have_route = route_default(ni->ifname, sizeof(ni->ifname),
                                   ni->gw, sizeof(ni->gw), &rerr);
    if (!have_route) {
        ni->ifname[0] = 0;
        if (!first_iface(ni->ifname, sizeof(ni->ifname))) {
            if (rerr) snprintf(ni->ifname, sizeof(ni->ifname), "%s", "unavailable");
            else      snprintf(ni->ifname, sizeof(ni->ifname), "%s", "none");
        }
        if (rerr) snprintf(ni->gw, sizeof(ni->gw), "unavailable (%s)", strerror(rerr));
        else      snprintf(ni->gw, sizeof(ni->gw), "%s", "none (no default route)");
    }

    if (!ni->ifname[0] || !strcmp(ni->ifname, "none") ||
        !strcmp(ni->ifname, "unavailable")) {
        snprintf(ni->state, sizeof(ni->state), "%s", "unknown");
        snprintf(ni->ip, sizeof(ni->ip), "%s", "no interface");
    } else {
        iface_state(ni->ifname, ni->state, sizeof(ni->state));
        int ierr = 0;
        if (!iface_ipv4(ni->ifname, ni->ip, sizeof(ni->ip), &ierr)) {
            if (ierr == EADDRNOTAVAIL)
                snprintf(ni->ip, sizeof(ni->ip), "%s", "no IPv4 address");
            else
                snprintf(ni->ip, sizeof(ni->ip), "unavailable (%s)", strerror(ierr));
        }
    }

    read_dns(ni->dns, sizeof(ni->dns));
}

/* ---- storage ------------------------------------------------------------- */

static void fmt_bytes(unsigned long long b, char *out, size_t n) {
    if (b >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, n, "%.1f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024ULL * 1024ULL)
        snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else if (b >= 1024ULL)
        snprintf(out, n, "%.1f KB", (double)b / 1024.0);
    else
        snprintf(out, n, "%llu B", b);
}

/* statvfs(2), the real thing. f_frsize is the fragment size the numbers are
 * in; f_bsize is the fallback every statvfs implementation provides. */
static void usage_line(const char *path, char *out, size_t n) {
    struct statvfs v;
    if (statvfs(path, &v) != 0) {
        snprintf(out, n, "unavailable (%s)", strerror(errno));
        return;
    }
    unsigned long long frs = v.f_frsize ? v.f_frsize : v.f_bsize;
    unsigned long long total = (unsigned long long)v.f_blocks * frs;
    unsigned long long freeb = (unsigned long long)v.f_bfree * frs;
    unsigned long long used = total > freeb ? total - freeb : 0;

    if (total == 0) { snprintf(out, n, "%s", "0 blocks reported"); return; }

    char a[40], b[40];
    fmt_bytes(used, a, sizeof(a));
    fmt_bytes(total, b, sizeof(b));
    int pct = (int)((double)used * 100.0 / (double)total);
    snprintf(out, n, "%s of %s used (%d%%)", a, b, pct);
}

/* ---- the page content ---------------------------------------------------- */

static int page_add(PRow *r, int n, int max, int id,
                    const char *label, const char *value, int current,
                    const Theme *swatch) {
    if (n >= max) return n;
    PRow *p = &r[n];
    p->id = id;
    p->current = current;
    p->swatch = swatch;
    snprintf(p->label, sizeof(p->label), "%s", label ? label : "");
    snprintf(p->value, sizeof(p->value), "%s", value ? value : "");
    return n + 1;
}

static void fmt_uptime(long s, char *out, size_t n) {
    if (s < 0) s = 0;
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (d)      snprintf(out, n, "%ldd %ldh %ldm", d, h, m);
    else if (h) snprintf(out, n, "%ldh %ldm", h, m);
    else if (m) snprintf(out, n, "%ldm %lds", m, sec);
    else        snprintf(out, n, "%lds", sec);
}

/* Build the rows of the current page from real data. Every frame starts
 * from the files, so an external edit, a failed write, or a missing source
 * is what the page shows. A row with id >= 0 is selectable and applies on
 * Enter; a fact row has id -1 and no selection at all. */
static int build_page(Settings *st, const Theme *live, PRow *rows, int max) {
    int n = 0;
    char v[224];

    switch (st->tab) {
    case TAB_TEXT: {
        float now = 1.0f;
        int have = read_scale(&now);
        if (!have) now = 1.0f;
        for (int i = 0; i < SIZE_COUNT; i++) {
            int cur = fabsf(now - SIZE_VALUE[i]) < 0.005f;
            snprintf(v, sizeof(v), "%.1fx", (double)SIZE_VALUE[i]);
            n = page_add(rows, n, max, i, SIZE_LABEL[i], v, cur, NULL);
        }
        break;
    }
    case TAB_THEME: {
        int cur = -1;
        for (int i = 0; i < st->theme_count; i++)
            if (live && live->name[0] && !strcmp(live->name, st->themes[i].theme.name))
                cur = i;
        for (int i = 0; i < st->theme_count; i++) {
            int is_cur = (i == cur);
            n = page_add(rows, n, max, i, st->themes[i].theme.name, "",
                         is_cur, &st->themes[i].theme);
        }
        break;
    }
    case TAB_WALL: {
        int dw = read_wallpaper();
        int cur = dw >= 0 ? dw : (dw == -2 ? 0 : -1);
        for (int i = 0; i < WALLPAPER_COUNT; i++) {
            const char *nm = wallpaper_name(i);
            n = page_add(rows, n, max, i, nm ? nm : "?", "", i == cur, NULL);
        }
        break;
    }
    case TAB_USERS: {
        if (st->pw_stage == 0) {
            n = page_add(rows, n, max, 0, "Change password", "", 0, NULL);
        } else {
            char mask[96];
            pw_mask(st->pw_buf, st->pw_len, mask, sizeof(mask));
            n = page_add(rows, n, max, -1, pw_prompt(st), mask, 0, NULL);
        }
        break;
    }
    case TAB_NET: {
        NetInfo ni;
        read_net(&ni);
        if (!strcmp(ni.ifname, "none") || !strcmp(ni.ifname, "unavailable"))
            snprintf(v, sizeof(v), "%s", ni.ifname);
        else
            snprintf(v, sizeof(v), "%s (%s)", ni.ifname, ni.state);
        n = page_add(rows, n, max, -1, "Interface", v, 0, NULL);
        n = page_add(rows, n, max, -1, "IPv4 address", ni.ip, 0, NULL);
        n = page_add(rows, n, max, -1, "Gateway", ni.gw, 0, NULL);
        n = page_add(rows, n, max, -1, "DNS", ni.dns, 0, NULL);
        break;
    }
    case TAB_STORAGE: {
        usage_line("/", v, sizeof(v));
        n = page_add(rows, n, max, -1, "/", v, 0, NULL);
        usage_line("/root", v, sizeof(v));
        n = page_add(rows, n, max, -1, "/root", v, 0, NULL);
        break;
    }
    case TAB_SYSTEM:
    default: {
        read_file("/sys/class/graphics/fb0/virtual_size", v, sizeof(v));
        n = page_add(rows, n, max, -1, "Display", v, 0, NULL);

        long total = mem_kb("MemTotal:"), avail = mem_kb("MemAvailable:");
        if (total > 0 && avail >= 0) {
            long used = total - avail;
            if (used < 0) used = 0;
            snprintf(v, sizeof(v), "%ld MB of %ld MB", used / 1024, total / 1024);
        } else {
            snprintf(v, sizeof(v), "%s", "unavailable");
        }
        n = page_add(rows, n, max, -1, "Memory", v, 0, NULL);

        read_file("/proc/sys/kernel/osrelease", v, sizeof(v));
        n = page_add(rows, n, max, -1, "Kernel", v, 0, NULL);

        {
            char up[64];
            if (first_line("/proc/uptime", v, sizeof(v)))
                fmt_uptime((long)atol(v), up, sizeof(up));
            else
                snprintf(up, sizeof(up), "%s", "unavailable");
            n = page_add(rows, n, max, -1, "Uptime", up, 0, NULL);
        }
        break;
    }
    }
    return n;
}

static int tab_rows(const Settings *s) {
    switch (s->tab) {
        case TAB_TEXT:  return SIZE_COUNT;
        case TAB_THEME: return s->theme_count;
        case TAB_WALL:  return WALLPAPER_COUNT;
        case TAB_USERS: return s->pw_stage ? 0 : 1;
        default:        return 0;
    }
}

/* One line for the empty case - never a blank pane. */
static const char *page_empty(const Settings *s, char *buf, size_t n) {
    if (s->tab != TAB_THEME) return NULL;
    if (s->theme_errno)
        snprintf(buf, n, "Cannot read %s: %s", MYSTIC_THEME_DIR,
                 strerror(s->theme_errno));
    else
        snprintf(buf, n, "No .theme files in %s", MYSTIC_THEME_DIR);
    return buf;
}

/* One footer line: what Enter will do, or where the values come from. */
static const char *page_hint(const Settings *s, const Theme *live, char *buf, size_t n) {
    switch (s->tab) {
        case TAB_TEXT:
            return "Enter applies - writes /etc/mystic/ui.conf";
        case TAB_THEME:
            if (live && live->name[0]) {
                int found = 0;
                for (int i = 0; i < s->theme_count && !found; i++)
                    if (!strcmp(live->name, s->themes[i].theme.name)) found = 1;
                if (!found) {
                    snprintf(buf, n, "In use: %s - no theme file has that name",
                             live->name);
                    return buf;
                }
            }
            return "Enter applies - writes /etc/mystic/current-theme";
        case TAB_WALL:
            return "Enter applies - writes /etc/mystic/wallpaper";
        case TAB_USERS:
            return s->pw_stage
                 ? "Enter submits - Ctrl+C cancels - nothing is echoed"
                 : "Enter starts - the current password is checked first";
        case TAB_NET:
            return "Read now: /proc/net/route, SIOCGIFADDR, /etc/resolv.conf";
        case TAB_STORAGE:
            return "Read now: statvfs(2) on / and /root";
        default:
            return "Read now: /proc and /sys";
    }
}

/* ---- navigation ---------------------------------------------------------- */

static void nav_arrow(Surface *s, int dir, float x, float y, float sz, u32 c) {
    if (dir < 0) {
        gfx_line(s, x + sz * 0.26f, y + sz * 0.5f, x + sz * 0.70f, y + sz * 0.5f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.28f, y + sz * 0.5f,
                         x + sz * 0.64f, y + sz * 0.28f,
                         x + sz * 0.64f, y + sz * 0.72f };
        gfx_polygon(s, pts, 3, c);
    } else {
        gfx_line(s, x + sz * 0.30f, y + sz * 0.5f, x + sz * 0.74f, y + sz * 0.5f,
                 sz * 0.12f, c);
        float pts[6] = { x + sz * 0.72f, y + sz * 0.5f,
                         x + sz * 0.36f, y + sz * 0.28f,
                         x + sz * 0.36f, y + sz * 0.72f };
        gfx_polygon(s, pts, 3, c);
    }
}

/* The rail (all seven names, the current one in an accent pill) or the
 * page bar (one name, two arrows). Both are built from Layout rectangles
 * the click handler also uses. */
static void nav_draw(Surface *s, int x, int y, const Layout *L,
                     const Settings *st, const Theme *th) {
    if (L->mode == 0) {
        for (int i = 0; i < TAB_COUNT; i++) {
            int bx, by, bw, bh;
            if (!rail_rect(L, i, &bx, &by, &bw, &bh)) continue;
            int on = (i == st->tab);
            if (on)
                ui_pill(s, x + bx, y + by, bw, bh, (float)L->radius,
                        color_alpha(th->accent, 0.16f));
            font_draw(s, L->f.body, (float)(x + bx + SP(2)),
                      (float)(y + by) + (float)ui_top(L->f.body, bh),
                      TAB_LABEL[i],
                      on ? th->text : color_alpha(th->text, 0.58f));
        }
        return;
    }

    for (int which = 0; which < 2; which++) {
        int bx, by, bw, bh;
        if (!arrow_rect(L, which, &bx, &by, &bw, &bh)) continue;
        int hot = st->px >= bx && st->px < bx + bw &&
                  st->py >= by && st->py < by + bh;
        if (hot)
            ui_pill(s, x + bx, y + by, bw, bh, (float)L->radius,
                    color_alpha(th->accent, 0.18f));
        int sz = L->arrow_w - 2 * SP(1);
        nav_arrow(s, which ? 1 : -1,
                  (float)(x + bx) + (float)SP(1),
                  (float)(y + by) + (float)SP(1),
                  (float)sz, color_alpha(th->text, hot ? 0.95f : 0.55f));
    }
    font_draw(s, L->f.body, (float)(x + L->name_x),
              (float)(y + L->bar_y) + (float)ui_top(L->f.body, L->bar_h),
              TAB_LABEL[st->tab], th->text);
}

/* ---- AppVT --------------------------------------------------------------- */

static void settings_init(App *a, int w, int h) {
    Settings *s = calloc(1, sizeof(Settings));
    if (!s) return;
    s->w = w;
    s->h = h;
    s->tab = TAB_TEXT;
    s->px = s->py = -1;
    scan_themes(s);
    /* start with the row that matches the scale really in effect */
    {
        float now = 1.0f;
        read_scale(&now);
        int best = 0;
        for (int i = 1; i < SIZE_COUNT; i++)
            if (fabsf(SIZE_VALUE[i] - now) < fabsf(SIZE_VALUE[best] - now)) best = i;
        s->sel = best;
    }
    a->state = s;
}

static void settings_free(App *a) {
    Settings *s = a->state;
    if (s) {
        secure_zero(s->pw_buf, sizeof(s->pw_buf));
        secure_zero(s->pw_first, sizeof(s->pw_first));
    }
    free(s);
    a->state = NULL;
}

/* Clip to the rectangle we were given. */
static Surface rect_clip(Surface *s, int x, int y, int w, int h) {
    Surface c = *s;
    int right = x + w, bottom = y + h;
    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;
    if (right < c.w) c.w = right;
    if (bottom < c.h) c.h = bottom;
    return c;
}

static void settings_draw(App *a, Surface *s, int x, int y, int w, int h,
                          const Theme *th) {
    Settings *st = a->state;
    if (!st) return;
    st->w = w;
    st->h = h;
    if (st->tab < 0 || st->tab >= TAB_COUNT) st->tab = TAB_TEXT;

    Surface clip = rect_clip(s, x, y, w, h);
    s = &clip;

    /* the theme list is the disk: pick up a file edited since the last frame */
    scan_themes(st);

    Layout L;
    if (!layout_settings(&L, st, w, h)) return;

    /* ---- header: the app's name, no action ---- */
    surf_blend(s, x, y, w, L.header_h, th->panel);
    font_draw(s, L.f.title, (float)(x + L.m), (float)(y + ui_top(L.f.title, L.header_h)),
              "Settings", th->text);
    surf_blend(s, x, y + L.header_h - L.rule, w, L.rule,
               color_alpha(th->panel_border, 0.80f));

    nav_draw(s, x, y, &L, st, th);

    /* ---- one measure pass: the whole page ---- */
    PRow rows[MAX_PAGE_ROWS];
    int nr = build_page(st, th, rows, MAX_PAGE_ROWS);

    int sel_n = tab_rows(st);
    if (sel_n <= 0) st->sel = 0;
    else {
        if (st->sel < 0) st->sel = 0;
        if (st->sel >= sel_n) st->sel = sel_n - 1;
    }

    int max_scroll = nr - L.rows;
    if (max_scroll < 0) max_scroll = 0;
    if (sel_n > 0 && st->sel >= 0) {
        if (st->sel < st->scroll) st->scroll = st->sel;
        if (st->sel >= st->scroll + L.rows) st->scroll = st->sel - L.rows + 1;
    }
    if (st->scroll < 0) st->scroll = 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;

    /* ---- rows ---- */
    if (nr == 0) {
        char buf[256];
        const char *msg = page_empty(st, buf, sizeof(buf));
        if (msg)
            draw_clipped(s, L.f.body, (float)(x + L.label_x),
                         (float)(y + L.body_y + L.row_dy),
                         (float)(L.val_r - L.label_x), msg, th->text_dim);
    } else {
        for (int i = 0; i < L.rows; i++) {
            int idx = st->scroll + i;
            if (idx >= nr) break;
            int ryt, rh;
            if (!page_row_rect(&L, i, &ryt, &rh)) break;
            int selectable = rows[idx].id >= 0;
            int sel = selectable && rows[idx].id == st->sel;
            page_row_draw(s, &L, th, &rows[idx], y + ryt, selectable, sel);
        }
    }

    /* ---- a map of the page, in the right margin ---- */
    if (nr > L.rows && L.rows > 0) {
        int band = L.body_bot - L.body_y;
        int bx = x + w - L.bar_w2;
        if (band > 0) {
            int thumb = band * L.rows / nr;
            if (thumb < SP(2)) thumb = SP(2);
            if (thumb > band) thumb = band;
            int span = band - thumb;
            int off = max_scroll > 0 ? span * st->scroll / max_scroll : 0;
            ui_pill(s, bx, y + L.body_y, L.bar_w2, band,
                    (float)L.bar_w2 * 0.5f, color_alpha(th->text, 0.06f));
            ui_pill(s, bx, y + L.body_y + off, L.bar_w2, thumb,
                    (float)L.bar_w2 * 0.5f, color_alpha(th->accent, 0.55f));
        }
    }

    /* ---- one status line: the last real result, else this page's hint ---- */
    if (L.footer_h > 0) {
        int fy = y + h - L.footer_h;
        surf_blend(s, x, fy, w, L.footer_h, th->panel);
        surf_blend(s, x, fy, w, L.rule, th->panel_border);

        char buf[256];
        const char *line;
        u32 col;
        if (st->pw_stage && st->pw_msg[0]) {
            line = st->pw_msg;
            col = st->pw_msg_err ? color_mix(th->accent, RGB(255, 92, 92), 0.65f)
                                 : color_alpha(th->accent, 0.85f);
        } else if (st->status[0]) {
            line = st->status;
            col = st->status_err ? color_mix(th->accent, RGB(255, 92, 92), 0.65f)
                                 : color_alpha(th->accent, 0.85f);
        } else if (st->pw_msg[0] && !st->pw_stage) {
            line = st->pw_msg;
            col = st->pw_msg_err ? color_mix(th->accent, RGB(255, 92, 92), 0.65f)
                                 : color_alpha(th->accent, 0.85f);
        } else {
            line = page_hint(st, th, buf, sizeof(buf));
            col = color_alpha(th->text, 0.45f);
        }

        int text_r = x + w - L.m;
        if (st->pw_stage) {
            int bx, by, bw, bh;
            if (pw_btn_rect(&L, w, 0, &bx, &by, &bw, &bh)) {
                text_r = x + bx - L.gap;
                ui_button(s, x + bx, fy + by, bw, bh, (float)L.radius,
                          "Cancel", L.f.small, 0, th);
            }
            if (pw_btn_rect(&L, w, 1, &bx, &by, &bw, &bh)) {
                int hot = st->px >= bx && st->px < bx + bw &&
                          st->py >= fy + by && st->py < fy + by + bh;
                ui_button(s, x + bx, fy + by, bw, bh, (float)L.radius,
                          "Submit", L.f.small, hot, th);
            }
        }
        draw_clipped(s, L.f.small, (float)(x + L.m),
                     (float)(fy + L.footer_dy), (float)(text_r - (x + L.m)),
                     line, col);
    }
}

/* ---- input --------------------------------------------------------------- */

static void set_tab(Settings *s, int t) {
    if (t < 0) t = TAB_COUNT - 1;
    if (t >= TAB_COUNT) t = 0;
    if (t == s->tab) return;
    if (s->pw_stage) pw_cancel(s);
    s->tab = t;
    s->sel = 0;
    s->scroll = 0;
}

static void move_sel(Settings *s, int delta) {
    int n = tab_rows(s);
    if (n <= 0) return;
    s->sel += delta;
    if (s->sel < 0) s->sel = 0;
    if (s->sel >= n) s->sel = n - 1;
}

static void activate(Settings *s) {
    switch (s->tab) {
        case TAB_TEXT:
            apply_size(s, s->sel);
            break;
        case TAB_THEME:
            apply_theme(s, s->sel);
            break;
        case TAB_WALL:
            apply_wallpaper(s, s->sel);
            break;
        case TAB_USERS:
            if (s->pw_stage == 0) pw_start(s);
            break;
        default:
            break;
    }
}

/* A digit picks the nth row of the page's own short list. */
static void pick_digit(Settings *s, int idx) {
    switch (s->tab) {
        case TAB_TEXT:
            if (idx < SIZE_COUNT) { s->sel = idx; apply_size(s, idx); }
            break;
        case TAB_THEME:
            if (idx < s->theme_count) { s->sel = idx; apply_theme(s, idx); }
            break;
        case TAB_WALL:
            if (idx < WALLPAPER_COUNT) { s->sel = idx; apply_wallpaper(s, idx); }
            break;
        default:
            break;
    }
}

/* Length of an escape sequence (arrow, Home/End, PageUp/PageDown) or 0 if
 * this byte does not start one. */
static int esc_len(const char *b, int i, int n) {
    if ((unsigned char)b[i] != 0x1b) return 0;
    if (i + 2 < n && b[i + 1] == '[') {
        if (b[i + 2] >= '0' && b[i + 2] <= '9')
            return (i + 3 < n && b[i + 3] == '~') ? 4 : 3;
        return 3;
    }
    return 1;
}

/* Scroll a page of facts by whole rows, clamped to what there is. */
static void scroll_rows(Settings *s, int delta) {
    PRow rows[MAX_PAGE_ROWS];
    int nr = build_page(s, NULL, rows, MAX_PAGE_ROWS);
    Layout L;
    int visible = 0;
    if (layout_settings(&L, s, s->w, s->h)) visible = L.rows;
    int maxs = nr - visible;
    if (maxs < 0) maxs = 0;
    s->scroll += delta;
    if (s->scroll < 0) s->scroll = 0;
    if (s->scroll > maxs) s->scroll = maxs;
}

static void settings_key(App *a, const char *bytes, int n) {
    Settings *s = a->state;
    if (!s || !bytes) return;

    for (int i = 0; i < n; ) {
        unsigned char c = (unsigned char)bytes[i];

        /* While a password is being entered every byte is a secret. */
        if (s->pw_stage) {
            int el = esc_len(bytes, i, n);
            if (el) { i += el; continue; }
            i++;
            if (c == '\r' || c == '\n') { pw_enter(s); continue; }
            if (c == '\b' || c == 0x7f) {
                if (s->pw_len > 0) s->pw_buf[--s->pw_len] = 0;
                continue;
            }
            if (c == 3) { pw_cancel(s); continue; }
            if (c == '\t') continue;
            if (c >= 0x20 && s->pw_len < PW_MAX) {
                s->pw_buf[s->pw_len++] = (char)c;
                s->pw_buf[s->pw_len] = 0;
            }
            continue;
        }

        int el = esc_len(bytes, i, n);
        if (el >= 3) {
            char f = bytes[i + 2];
            Layout L;
            int page = 1;
            if (layout_settings(&L, s, s->w, s->h) && L.rows > 1) page = L.rows - 1;
            if (f == 'A') {
                if (tab_rows(s) > 0) move_sel(s, -1);
                else scroll_rows(s, -1);
            } else if (f == 'B') {
                if (tab_rows(s) > 0) move_sel(s, +1);
                else scroll_rows(s, +1);
            } else if (f == 'C') {
                set_tab(s, s->tab + 1);
            } else if (f == 'D') {
                set_tab(s, s->tab - 1);
            } else if (f == '5') {
                if (tab_rows(s) > 0) move_sel(s, -page);
                else scroll_rows(s, -page);
            } else if (f == '6') {
                if (tab_rows(s) > 0) move_sel(s, +page);
                else scroll_rows(s, +page);
            } else if (f == 'H') {
                if (tab_rows(s) > 0) s->sel = 0;
                else s->scroll = 0;
            } else if (f == 'F') {
                if (tab_rows(s) > 0) s->sel = tab_rows(s) - 1;
            }
            i += el;
            continue;
        }
        if (el == 1) { i += 1; continue; }

        i++;
        if (c == '\t') { set_tab(s, s->tab + 1); continue; }
        if (c == '\r' || c == '\n') { activate(s); continue; }
        if (c == 'k') { move_sel(s, -1); continue; }
        if (c == 'j') { move_sel(s, +1); continue; }
        if (c >= '1' && c <= '9') { pick_digit(s, c - '1'); continue; }
    }
}

static void settings_click(App *a, int x, int y, int buttons) {
    Settings *s = a->state;
    if (!s) return;
    if (buttons && !(buttons & 1)) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px = x;
    s->py = y;

    Layout L;
    if (!layout_settings(&L, s, s->w, s->h)) return;

    /* navigation first: it is above and beside the body */
    for (int i = 0; i < TAB_COUNT; i++) {
        int bx, by, bw, bh;
        if (!rail_rect(&L, i, &bx, &by, &bw, &bh)) continue;
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            set_tab(s, i);
            return;
        }
    }
    for (int which = 0; which < 2; which++) {
        int bx, by, bw, bh;
        if (!arrow_rect(&L, which, &bx, &by, &bw, &bh)) continue;
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            set_tab(s, which ? s->tab + 1 : s->tab - 1);
            return;
        }
    }
    if (L.mode == 1 && x >= L.name_x && x < L.name_x + L.name_w &&
        y >= L.bar_y && y < L.bar_y + L.bar_h) {
        set_tab(s, s->tab + 1);
        return;
    }

    /* the password prompt's two buttons */
    if (s->pw_stage) {
        for (int which = 0; which < 2; which++) {
            int bx, by, bw, bh;
            if (!pw_btn_rect(&L, s->w, which, &bx, &by, &bw, &bh)) continue;
            int fy = s->h - L.footer_h;
            if (x >= bx && x < bx + bw && y >= fy + by && y < fy + by + bh) {
                if (which) pw_enter(s);
                else pw_cancel(s);
                return;
            }
        }
    }

    /* a body row: the same rectangle the painter used */
    int vis = page_row_at(&L, y);
    if (vis < 0) return;
    PRow rows[MAX_PAGE_ROWS];
    int nr = build_page(s, NULL, rows, MAX_PAGE_ROWS);
    int idx = s->scroll + vis;
    if (idx < 0 || idx >= nr) return;
    if (rows[idx].id < 0) return;              /* a fact row has no action */
    s->sel = rows[idx].id;
    activate(s);
}

static void settings_resize(App *a, int w, int h) {
    Settings *s = a->state;
    if (s) { s->w = w; s->h = h; }
}

/* The wheel walks the selection a few rows a notch, like every other list
 * in the desktop; a page of facts scrolls instead of moving a selection it
 * does not have. */
static void settings_wheel(App *a, int notches) {
    Settings *s = a->state;
    if (!s || !notches) return;
    if (tab_rows(s) > 0) move_sel(s, -notches * 2);
    else scroll_rows(s, -notches * 2);
}

const AppVT app_vt_settings = {
    .id = "settings",
    .title = "Settings",
    .init = settings_init,
    .draw = settings_draw,
    .key = settings_key,
    .click = settings_click,
    .resize = settings_resize,
    .wheel = settings_wheel,
    .free = settings_free,
};
