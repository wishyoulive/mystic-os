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
 *  The same grid every row already used is still the only layout
 *  mechanism: Grid::row_h, head_h and line_h are built from the font
 *  line box plus ui_px() leading, and every label is centred on its
 *  real ink box (ascent..descent) inside its band.  The tab strip is
 *  measured from the heading font the same way.  There is no bare
 *  pixel height in this file, so the text scale cannot make rows
 *  overlap: they grow with the fonts that fill them.  A note that
 *  names several files wraps onto whole extra line_h bands instead of
 *  being shortened with "..", so the source of every value stays
 *  readable at every scale.
 *
 *  Input:
 *      Up/Down, j/k        move the selection on a page with controls
 *      Left/Right, Tab     change page
 *      Enter               apply the selected row (or submit a password)
 *      digits 1..9         apply that row of Text size / Theme / Wallpaper
 *      wheel               move the selection (a page of facts scrolls)
 *      Ctrl+C              cancel a password prompt (Esc is consumed by
 *                          main.c before an app sees it)
 *      click a tab         change page
 *      click a row         select and apply it
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

enum { K_HEAD, K_ROW, K_NOTE, K_GAP };

typedef struct {
    char  file[64];
    Theme theme;                /* parsed by theme_load(): the real colours */
} ThemeRow;

/* One row of whichever page is showing. Built from real data each frame. */
typedef struct {
    int         kind;           /* K_HEAD, K_ROW, K_NOTE or K_GAP */
    int         id;             /* selectable ordinal, -1 for a fact/heading */
    char        label[192];
    char        value[224];
    int         current;        /* this row is what is in effect on disk */
    const Theme *swatch;        /* non-NULL: draw this theme's colours here */
    int         lines;          /* K_NOTE only: bands it needs, from rows_measure */
} PRow;

typedef struct {
    ThemeRow themes[MAX_THEMES];
    int      theme_count;
    int      theme_errno;       /* errno from opendir(MYSTIC_THEME_DIR) */
    int      tab;
    int      sel;
    int      w, h;
    float    scroll;            /* pixels the page body is scrolled down */
    char     status[192];       /* real result of the last write */
    int      status_err;

    /* password flow: 0 idle, 1 old, 2 new, 3 repeat */
    int      pw_stage;
    int      pw_len;
    char     pw_buf[PW_MAX + 1];
    char     pw_first[PW_MAX + 1];
    char     pw_msg[192];
    int      pw_msg_err;

    /* Hit boxes, in window-local coordinates, painted by the last frame.
     * A click is tested against what was drawn, never against a second
     * guess at the layout. */
    float    tab_x[TAB_COUNT], tab_y[TAB_COUNT], tab_w[TAB_COUNT], tab_h;
    float    hit_y[MAX_PAGE_ROWS], hit_yb[MAX_PAGE_ROWS];
} Settings;

/* ---- one grid, every metric from the fonts and ui_px() ------------------ */

/* font_draw() takes y as the ascender line and puts the baseline
 * font_ascent() below it (src/font.c:1316). stbtt_ScaleForPixelHeight() makes
 * ascent + descent exactly the nominal size while font_height() is
 * size * 1.4, so the glyphs reach font_height/1.4 below y. Centring THAT box
 * is what puts a label in the middle of its row at every text scale. */
static float ink_h(Font *f) { return font_height(f) / 1.4f; }

static float base_y(Font *f, float top, float band_h) {
    return top + (band_h - ink_h(f)) * 0.5f + font_ascent(f);
}
static float top_y(Font *f, float base) { return base - font_ascent(f); }

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

/* Best effort: the password buffers are cleared before the page moves on.
 * volatile keeps the stores from being folded away at -Os. */
static void secure_zero(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* The page on one grid. Every x is one of three edges:
 *
 *      lab_x                the left column: every label, heading, note
 *      val_r                the right column: every value, right-aligned
 *      x + m .. lab_x - gap the ordinal or theme-swatch gutter
 *
 * and every y is one of four bands: row_h, head_h, line_h and gap. The
 * heights come from the font that fills them, the margins from ui_px(), so
 * the grid holds at every text scale and nothing is a bare pixel count. */
typedef struct {
    float m;        /* page margin, inside the window's own edge       */
    float gap;      /* air between blocks, and gutter before the label */
    float gut;      /* ordinal / swatch column                          */
    float row_h;    /* one list row / one fact row                     */
    float head_h;   /* one page heading band                           */
    float line_h;   /* one small band (note, status)                   */
    float lab_x;    /* left edge of every label and heading            */
    float val_r;    /* right edge of every value                       */
    float sw_w;     /* theme swatch width                              */
    float sw_h;     /* theme swatch height                             */
} Grid;

static Grid grid_of(int x, int w, Font *fh, Font *fr, Font *fd) {
    Grid g;
    g.m      = (float)ui_px(12);
    g.gap    = (float)ui_px(8);
    g.row_h  = font_height(fr) + (float)ui_px(8);
    g.head_h = font_height(fh) + (float)ui_px(10);
    g.line_h = font_height(fr) + (float)ui_px(6);
    g.sw_w   = font_height(fr) * 1.8f;
    g.sw_h   = font_height(fr) * 0.62f;
    g.gut    = font_width(fd, "00");
    if (g.sw_w > g.gut) g.gut = g.sw_w;
    g.lab_x  = (float)x + g.m + g.gut + g.gap;
    g.val_r  = (float)(x + w) - g.m;
    return g;
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

/* Every list/fact row in this window goes through this function. The
 * highlight is the full width of the window, the value ends at val_r, and a
 * theme row draws its swatch where every other row draws its ordinal. */
static void row_draw(Surface *s, Font *fr, Font *fd, const Grid *g,
                     int x, int w, float ry, const char *num,
                     const char *name, const char *value,
                     int sel, int cur, const Theme *th,
                     const Theme *swatch, float vfrac) {
    char nb[256], vb[256];
    float vw = 0.0f;

    snprintf(vb, sizeof(vb), "%s", value ? value : "");
    if (vb[0]) {
        /* the value column never takes more than vfrac of the row, so the
         * label stays readable in a narrow window */
        fit_text(fr, vb, vb, sizeof(vb), (g->val_r - g->lab_x) * vfrac);
        if (vb[0]) vw = font_width(fr, vb) + g->gap;
    }
    fit_text(fr, name, nb, sizeof(nb), g->val_r - vw - g->lab_x);

    if (sel)
        surf_blend(s, x, (int)(ry + 0.5f), w, (int)(g->row_h + 0.5f),
                   color_alpha(th->accent, 0.18f));

    if (swatch) {
        float sy = ry + (g->row_h - g->sw_h) * 0.5f;
        swatch_draw(s, (float)x + g->m, sy, g->sw_w, g->sw_h, swatch);
    } else if (num && num[0]) {
        float nw = font_width(fd, num);
        font_draw(s, fd, g->lab_x - g->gap - nw,
                  top_y(fd, base_y(fd, ry, g->row_h)), num,
                  color_alpha(th->text, 0.42f));
    }

    font_draw(s, fr, g->lab_x, top_y(fr, base_y(fr, ry, g->row_h)), nb,
              sel ? th->text : color_alpha(th->text, 0.85f));
    if (vb[0])
        font_draw(s, fr, g->val_r - font_width(fr, vb),
                  top_y(fr, base_y(fr, ry, g->row_h)), vb,
                  cur ? th->accent : color_alpha(th->text, 0.72f));
}

/* A page heading: the title on the left column, the live value (what the
 * desktop is using now) on the value column's right edge, one baseline. */
static void head_draw(Surface *s, Font *fh, Font *fr, const Grid *g,
                      float ry, const char *title, const char *value,
                      const Theme *th) {
    char vb[256];
    float base = base_y(fh, ry, g->head_h);
    font_draw(s, fh, g->lab_x, top_y(fh, base), title, th->text);
    if (!value || !value[0]) return;
    snprintf(vb, sizeof(vb), "%s", value);
    fit_text(fr, vb, vb, sizeof(vb),
             g->val_r - g->lab_x - font_width(fh, title) - g->gap);
    if (vb[0])
        font_draw(s, fr, g->val_r - font_width(fr, vb), top_y(fr, base), vb,
                  th->text_dim);
}

/* A small explanatory line, wrapped onto as many font-derived lines as it
 * needs. A note that names four files does not fit one row of a 518 px window,
 * and shortening it with ".." throws away exactly the part that says where
 * the value came from, so the note is split at spaces instead and its band
 * grows by whole line_h rows. The grid is still the grid: line_h comes from
 * the font, nothing here is a bare pixel count. */

/* Split `text` into at most four lines, each of which fits maxw, and return
 * how many lines were produced. The last line is shortened with ".." only
 * when a single word is wider than the whole column. */
static int note_split(const char *text, Font *fd, float maxw,
                      char out[4][384]) {
    char b[384];
    snprintf(b, sizeof(b), "%s", text ? text : "");
    int nl = 0;
    char *p = b;
    while (*p && nl < 4) {
        char line[384] = {0};
        while (*p == ' ') p++;
        while (*p) {
            char *e = p;
            while (*e && *e != ' ') e++;
            int wl = (int)(e - p);
            char word[192];
            if (wl > (int)sizeof(word) - 1) wl = (int)sizeof(word) - 1;
            memcpy(word, p, (size_t)wl);
            word[wl] = 0;
            char trial[384];
            if (line[0]) snprintf(trial, sizeof(trial), "%s %s", line, word);
            else         snprintf(trial, sizeof(trial), "%s", word);
            /* a word that does not fit starts the next line, unless it is the
             * first word of this one - then it is shortened, never dropped */
            if (font_width(fd, trial) > maxw && line[0]) break;
            snprintf(line, sizeof(line), "%s", trial);
            p = e;
            while (*p == ' ') p++;
        }
        if (!line[0]) break;
        fit_text(fd, line, out[nl], 384, maxw);
        nl++;
    }
    if (nl == 0) { out[0][0] = 0; nl = 1; }
    return nl;
}

/* The number of bands each note needs, filled in once per frame before the
 * page's offsets are summed. */
static void rows_measure(PRow *rows, int nr, Font *fd, const Grid *g) {
    float maxw = g->val_r - g->lab_x;
    for (int i = 0; i < nr; i++) {
        rows[i].lines = 0;
        if (rows[i].kind != K_NOTE) continue;
        char tmp[4][384];
        int n = note_split(rows[i].label, fd, maxw, tmp);
        rows[i].lines = n < 1 ? 1 : n;
    }
}

static void note_draw(Surface *s, Font *fd, const Grid *g, float ry,
                      const char *text, int lines, const Theme *th) {
    char out[4][384];
    int nl = note_split(text, fd, g->val_r - g->lab_x, out);
    if (lines > 0 && nl > lines) nl = lines;
    for (int i = 0; i < nl; i++) {
        if (!out[i][0]) continue;
        float y = ry + (float)i * g->line_h;
        font_draw(s, fd, g->lab_x, top_y(fd, base_y(fd, y, g->line_h)), out[i],
                  color_alpha(th->text, 0.52f));
    }
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

static int page_add(PRow *r, int n, int max, int kind, int id,
                    const char *label, const char *value, int current,
                    const Theme *swatch) {
    if (n >= max) return n;
    PRow *p = &r[n];
    p->kind = kind;
    p->id = id;
    p->current = current;
    p->swatch = swatch;
    p->lines = 0;
    snprintf(p->label, sizeof(p->label), "%s", label ? label : "");
    snprintf(p->value, sizeof(p->value), "%s", value ? value : "");
    return n + 1;
}

static float band_h(const PRow *r, const Grid *g) {
    switch (r->kind) {
        case K_HEAD: return g->head_h;
        case K_ROW:  return g->row_h;
        case K_NOTE: return g->line_h * (float)(r->lines > 0 ? r->lines : 1);
        default:     return g->gap;
    }
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
 * is what the page shows. */
static int build_page(Settings *st, const Theme *live, PRow *rows, int max) {
    int n = 0;
    char v[224];

    switch (st->tab) {
    case TAB_TEXT: {
        float now = 1.0f;
        int have = read_scale(&now);
        if (have) snprintf(v, sizeof(v), "now %.1fx", now);
        else      snprintf(v, sizeof(v), "now 1.0x (no ui.conf)");
        n = page_add(rows, n, max, K_HEAD, -1, "Text size", v, 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "1.0x is the shipped readable size - the baseline every "
                     "label and row is measured from.", "", 0, NULL);
        for (int i = 0; i < SIZE_COUNT; i++) {
            int cur = fabsf(now - SIZE_VALUE[i]) < 0.005f;
            snprintf(v, sizeof(v), "%.1fx%s", SIZE_VALUE[i], cur ? " now" : "");
            n = page_add(rows, n, max, K_ROW, i, SIZE_LABEL[i], v, cur, NULL);
        }
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Writes baseline = 1.4 and scale = <value> to "
                     "/etc/mystic/ui.conf; main.c re-reads it once a second.",
                     "", 0, NULL);
        break;
    }
    case TAB_THEME: {
        int cur = -1;
        for (int i = 0; i < st->theme_count; i++)
            if (live && !strcmp(live->name, st->themes[i].theme.name)) cur = i;
        if (st->theme_count == 0) {
            if (live && live->name[0])
                snprintf(v, sizeof(v), "now %s (no theme files)", live->name);
            else
                snprintf(v, sizeof(v), "%s", "none installed");
        } else if (live && live->name[0]) {
            snprintf(v, sizeof(v), "now %s", live->name);
        } else {
            snprintf(v, sizeof(v), "%s", "now: unknown");
        }
        n = page_add(rows, n, max, K_HEAD, -1, "Theme", v, 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Swatch = that file's bg_top, bg_bottom, accent and text, "
                     "read now.", "", 0, NULL);
        if (st->theme_count == 0) {
            if (st->theme_errno)
                snprintf(v, sizeof(v), "Cannot read %s: %s", MYSTIC_THEME_DIR,
                         strerror(st->theme_errno));
            else
                snprintf(v, sizeof(v), "No *%s file in %s loaded.", ".theme",
                         MYSTIC_THEME_DIR);
            n = page_add(rows, n, max, K_NOTE, -1, v, "", 0, NULL);
            n = page_add(rows, n, max, K_NOTE, -1,
                         "No theme file could be read now; the desktop keeps "
                         "the theme it already has.", "", 0, NULL);
        }
        for (int i = 0; i < st->theme_count; i++) {
            int is_cur = (i == cur);
            n = page_add(rows, n, max, K_ROW, i, st->themes[i].theme.name,
                         is_cur ? "now" : "", is_cur, &st->themes[i].theme);
        }
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Enter writes the theme's own name to "
                     "/etc/mystic/current-theme.", "", 0, NULL);
        break;
    }
    case TAB_WALL: {
        int dw = read_wallpaper();
        int cur = dw >= 0 ? dw : (dw == -2 ? 0 : -1);
        if (cur >= 0)
            snprintf(v, sizeof(v), "now %s%s", wallpaper_name(cur),
                     dw == -2 ? " (default)" : "");
        else
            snprintf(v, sizeof(v), "%s", "now: unknown name");
        n = page_add(rows, n, max, K_HEAD, -1, "Wallpaper", v, 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Every wallpaper wallpaper.c paints, by the name it "
                     "answers to. Enter applies one.", "", 0, NULL);
        for (int i = 0; i < WALLPAPER_COUNT; i++) {
            const char *nm = wallpaper_name(i);
            int is_cur = (i == cur);
            n = page_add(rows, n, max, K_ROW, i, nm ? nm : "?",
                         is_cur ? "now" : "", is_cur, NULL);
        }
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Enter writes the name to /etc/mystic/wallpaper; the "
                     "desktop re-reads it within a second.", "", 0, NULL);
        break;
    }
    case TAB_USERS: {
        n = page_add(rows, n, max, K_HEAD, -1, "Lock-screen password",
                     st->pw_stage ? "in progress" : "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "The lock screen verifies /etc/mystic/passwd: SHA-256 "
                     "over salt||password, 20000 rounds.", "", 0, NULL);
        if (st->pw_stage == 0) {
            n = page_add(rows, n, max, K_ROW, 0, "Change password", "Enter",
                         0, NULL);
            n = page_add(rows, n, max, K_NOTE, -1,
                         "Enter starts; the current password is checked "
                         "before a new one is accepted.", "", 0, NULL);
            /* The prompt is gone but its result is not: a change, a refusal
             * and a cancel each leave a line here, so the outcome of the
             * action is readable after the fact instead of only in the
             * status band. */
            if (st->pw_msg[0])
                n = page_add(rows, n, max, K_NOTE, -1, st->pw_msg, "", 0, NULL);
        } else {
            char mask[64];
            pw_mask(st->pw_buf, st->pw_len, mask, sizeof(mask));
            n = page_add(rows, n, max, K_ROW, -1, pw_prompt(st), mask, 0, NULL);
            if (st->pw_msg[0]) {
                snprintf(v, sizeof(v), "%s", st->pw_msg);
                n = page_add(rows, n, max, K_NOTE, -1, v, "", 0, NULL);
            }
            n = page_add(rows, n, max, K_NOTE, -1,
                         "Enter submits; Ctrl+C cancels. Nothing you type is "
                         "shown.", "", 0, NULL);
            if (st->pw_msg_err) {
                /* the message above already names the reason; this line is
                 * only here to keep the error visible in the status line */
                n = page_add(rows, n, max, K_NOTE, -1,
                             "The password has not been changed.", "", 0, NULL);
            }
        }
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "The new line is written to a temp file, fsync'd, then "
                     "renamed over the real file, and read back to verify.",
                     "", 0, NULL);
        break;
    }
    case TAB_NET: {
        NetInfo ni;
        read_net(&ni);
        n = page_add(rows, n, max, K_HEAD, -1, "Network", "", 0, NULL);
        if (!strcmp(ni.ifname, "none") || !strcmp(ni.ifname, "unavailable"))
            snprintf(v, sizeof(v), "%s", ni.ifname);
        else
            snprintf(v, sizeof(v), "%s (%s)", ni.ifname, ni.state);
        n = page_add(rows, n, max, K_ROW, -1, "Interface", v, 0, NULL);
        n = page_add(rows, n, max, K_ROW, -1, "IPv4 address", ni.ip, 0, NULL);
        n = page_add(rows, n, max, K_ROW, -1, "Gateway", ni.gw, 0, NULL);
        n = page_add(rows, n, max, K_ROW, -1, "DNS", ni.dns, 0, NULL);
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Sources: /proc/net/route, SIOCGIFADDR on the interface, "
                     "/sys/class/net/<if>/operstate, /etc/resolv.conf.",
                     "", 0, NULL);
        break;
    }
    case TAB_STORAGE: {
        n = page_add(rows, n, max, K_HEAD, -1, "Storage", "", 0, NULL);
        usage_line("/", v, sizeof(v));
        n = page_add(rows, n, max, K_ROW, -1, "/", v, 0, NULL);
        usage_line("/root", v, sizeof(v));
        n = page_add(rows, n, max, K_ROW, -1, "/root", v, 0, NULL);
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "statvfs(2) on each path: f_blocks and f_bfree times "
                     "f_frsize, read this frame.", "", 0, NULL);
        break;
    }
    case TAB_SYSTEM:
    default: {
        n = page_add(rows, n, max, K_HEAD, -1, "System", "", 0, NULL);
        read_file("/sys/class/graphics/fb0/virtual_size", v, sizeof(v));
        n = page_add(rows, n, max, K_ROW, -1, "Display", v, 0, NULL);

        long total = mem_kb("MemTotal:"), avail = mem_kb("MemAvailable:");
        if (total > 0 && avail >= 0) {
            long used = total - avail;
            if (used < 0) used = 0;
            snprintf(v, sizeof(v), "%ld MB of %ld MB", used / 1024, total / 1024);
        } else {
            snprintf(v, sizeof(v), "%s", "unavailable");
        }
        n = page_add(rows, n, max, K_ROW, -1, "Memory", v, 0, NULL);

        read_file("/proc/sys/kernel/osrelease", v, sizeof(v));
        n = page_add(rows, n, max, K_ROW, -1, "Kernel", v, 0, NULL);

        {
            char up[64];
            if (first_line("/proc/uptime", v, sizeof(v)))
                fmt_uptime((long)atol(v), up, sizeof(up));
            else
                snprintf(up, sizeof(up), "%s", "unavailable");
            n = page_add(rows, n, max, K_ROW, -1, "Uptime", up, 0, NULL);
        }
        n = page_add(rows, n, max, K_GAP, -1, "", "", 0, NULL);
        n = page_add(rows, n, max, K_NOTE, -1,
                     "Sources: /sys/class/graphics/fb0/virtual_size, "
                     "/proc/meminfo, /proc/sys/kernel/osrelease, /proc/uptime.",
                     "", 0, NULL);
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

static const char *tab_hint(const Settings *s) {
    switch (s->tab) {
        case TAB_TEXT:
            return "Enter applies; the desktop re-reads ui.conf within a second.";
        case TAB_THEME:
            return "Enter applies; each swatch is read from its real theme file.";
        case TAB_WALL:
            return "Enter applies; the desktop re-reads wallpaper within a second.";
        case TAB_USERS:
            return s->pw_stage
                 ? "Ctrl+C cancels. Nothing you type is shown."
                 : "Enter starts. The current password is checked first "
                   "when one is set.";
        case TAB_NET:
            return "Read now from /proc/net/route, the interface and resolv.conf.";
        case TAB_STORAGE:
            return "Read now with statvfs(2) on / and /root.";
        default:
            return "Read now from /proc and /sys.";
    }
}

/* ---- the tab strip ------------------------------------------------------- */

/* Tab labels are laid out left to right and wrap by measurement, never by a
 * fixed count: at a narrow window or a larger text scale the strip becomes
 * two rows and the body moves down. Each pill's painted rectangle is recorded
 * for settings_click(). */
static float tabs_draw(Surface *s, Font *fh, const Grid *g, int x, int y,
                       int w, Settings *st, const Theme *th) {
    float pad = (float)ui_px(6);
    float gap = (float)ui_px(4);
    float th_h = font_height(fh) + (float)ui_px(8);
    float left = (float)x + g->m;
    float right = (float)(x + w) - g->m;
    float tx = left, ty = (float)y + g->m * 0.5f;

    st->tab_h = th_h;
    for (int i = 0; i < TAB_COUNT; i++) {
        const char *lab = TAB_LABEL[i];
        float tw = font_width(fh, lab) + pad * 2.0f;
        if (tx > left && tx + tw > right) {
            tx = left;
            ty += th_h + gap;
        }
        if (tw > right - left) tw = right - left;
        if (tw < 1.0f) tw = 1.0f;

        int on = (i == st->tab);
        if (on)
            gfx_rrect_aa(s, tx, ty, tw, th_h, th_h * 0.5f,
                         color_alpha(th->accent, 0.16f),
                         color_alpha(th->accent, 0.75f), 1.0f);

        char shown[40];
        fit_text(fh, lab, shown, sizeof(shown), tw - pad * 2.0f);
        font_draw(s, fh, tx + pad, top_y(fh, base_y(fh, ty, th_h)), shown,
                  on ? th->text : color_alpha(th->text, 0.52f));

        st->tab_x[i] = tx - (float)x;
        st->tab_y[i] = ty - (float)y;
        st->tab_w[i] = tw;
        tx += tw + gap;
    }
    return ty + th_h - (float)y;
}

/* ---- AppVT --------------------------------------------------------------- */

static void settings_init(App *a, int w, int h) {
    Settings *s = calloc(1, sizeof(Settings));
    if (!s) return;
    s->w = w;
    s->h = h;
    s->tab = TAB_TEXT;
    /* -1 is "no row here": 0.0f would look like a real hit box at the top */
    for (int i = 0; i < MAX_PAGE_ROWS; i++) {
        s->hit_y[i] = -1.0f;
        s->hit_yb[i] = -1.0f;
    }
    scan_themes(s);
    /* start with the row that matches the scale really in effect, not with
     * Small; every "now" marker is still re-read from the file when drawn */
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

/* Clip to the rectangle we were given. The window manager hands over the
 * whole scene surface, so keeping pixels inside the app rect is the app's
 * job; negative x/y is rejected by every drawing call's bounds check. */
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

    Font *fh = font_get(FONT_MEDIUM, 13);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr || !fd) return;

    const Grid g = grid_of(x, w, fh, fr, fd);

    /* the theme list is the disk: pick up a file edited since the last frame */
    scan_themes(st);

    for (int i = 0; i < MAX_PAGE_ROWS; i++)
        st->hit_y[i] = st->hit_yb[i] = -1.0f;

    /* ---- tab strip ---- */
    float tabs_h = tabs_draw(s, fh, &g, x, y, w, st, th);

    /* ---- body and status bands ---- */
    float body_top = (float)y + tabs_h + g.gap;
    float status_y = (float)(y + h) - g.m - g.line_h;
    int have_status = status_y > body_top;
    float body_bottom = have_status ? status_y - g.gap * 0.5f
                                    : (float)(y + h) - g.m;
    float body_h = body_bottom - body_top;
    if (body_h < 0.0f) body_h = 0.0f;

    /* ---- one measure pass: the whole page ---- */
    PRow rows[MAX_PAGE_ROWS];
    float off[MAX_PAGE_ROWS];
    int nr = build_page(st, th, rows, MAX_PAGE_ROWS);
    rows_measure(rows, nr, fd, &g);          /* how many lines each note takes */
    float cy = 0.0f;
    for (int i = 0; i < nr; i++) {
        off[i] = cy;
        cy += band_h(&rows[i], &g);
    }
    const float content_h = cy;

    int sel_n = 0;
    for (int i = 0; i < nr; i++)
        if (rows[i].kind == K_ROW && rows[i].id >= 0) sel_n++;

    if (sel_n <= 0) {
        st->sel = 0;
    } else {
        if (st->sel < 0) st->sel = 0;
        if (st->sel >= sel_n) st->sel = sel_n - 1;
    }

    float max_scroll = content_h - body_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;

    if (sel_n > 0) {
        float sel_top = -1.0f, sel_bot = 0.0f;
        for (int i = 0; i < nr; i++) {
            if (rows[i].kind != K_ROW || rows[i].id != st->sel) continue;
            sel_top = off[i];
            sel_bot = off[i] + g.row_h;
            break;
        }
        if (sel_top >= 0.0f) {
            if (sel_top - st->scroll < 0.0f) st->scroll = sel_top;
            if (sel_bot - st->scroll > body_h) st->scroll = sel_bot - body_h;
        }
    }
    if (st->scroll < 0.0f) st->scroll = 0.0f;
    if (st->scroll > max_scroll) st->scroll = max_scroll;

    /* The body is its own surface whose first row is the body's top edge, so
     * a row scrolled past it is rejected by the same bounds check every other
     * draw uses - it cannot creep over the tabs or the status line. */
    int btop = (int)body_top;
    Surface bs = *s;
    if (btop > 0 && btop < bs.h) {
        bs.px += (size_t)btop * (size_t)bs.stride;
        bs.h -= btop;
    } else if (btop >= bs.h) {
        bs.h = 0;
    }
    if (bs.h > (int)body_h) bs.h = (int)body_h;
    if (bs.h < 0) bs.h = 0;

    for (int i = 0; i < nr; i++) {
        float ry = off[i] - st->scroll;
        float bh = band_h(&rows[i], &g);

        if (rows[i].kind == K_ROW && rows[i].id >= 0) {
            float top = ry, bot = ry + g.row_h;
            if (top < 0.0f) top = 0.0f;
            if (bot > body_h) bot = body_h;
            if (bot > top) {
                st->hit_y[rows[i].id] = (float)(btop - y) + top;
                st->hit_yb[rows[i].id] = (float)(btop - y) + bot;
            }
        }

        if (ry + bh <= 0.0f || ry >= body_h) continue;

        switch (rows[i].kind) {
            case K_HEAD:
                head_draw(&bs, fh, fr, &g, ry, rows[i].label, rows[i].value, th);
                break;
            case K_ROW: {
                char num[8];
                num[0] = 0;
                if (rows[i].id >= 0 && !rows[i].swatch)
                    snprintf(num, sizeof(num), "%d", rows[i].id + 1);
                int sel = rows[i].id >= 0 && rows[i].id == st->sel;
                float vfrac = rows[i].id >= 0 ? 0.50f : 0.62f;
                row_draw(&bs, fr, fd, &g, x, w, ry, num, rows[i].label,
                         rows[i].value, sel, rows[i].current, th,
                         rows[i].swatch, vfrac);
                break;
            }
            case K_NOTE:
                note_draw(&bs, fd, &g, ry, rows[i].label, rows[i].lines, th);
                break;
            default:
                break;
        }
    }

    /* A scroll bar in the page's right margin, clear of the value column, and
     * only when the page really continues past an edge. The old pair of
     * triangles sat on the first and last row's value text; a thumb in the
     * margin cannot collide with a label and also says how far down the page
     * you are. Track and thumb are sized from ui_px(), so they scale with the
     * text like every other margin here. */
    if (content_h > body_h + 0.5f && body_h > g.line_h * 0.5f) {
        float tw = (float)ui_px(4);
        if (tw < 1.0f) tw = 1.0f;
        float tx = (float)(x + w) - g.m * 0.5f - tw * 0.5f;
        float ty = body_top + (float)ui_px(2);
        float track_h = body_h - (float)ui_px(4);
        if (track_h > tw) {
            gfx_rrect_aa(s, tx, ty, tw, track_h, tw * 0.5f,
                         color_alpha(th->text, 0.16f), 0, 0);
            float frac = body_h / content_h;
            float thumb = track_h * frac;
            if (thumb < tw * 2.0f) thumb = tw * 2.0f;
            if (thumb > track_h) thumb = track_h;
            float t = st->scroll / (content_h - body_h);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float thumb_y = ty + (track_h - thumb) * t;
            gfx_rrect_aa(s, tx, thumb_y, tw, thumb, tw * 0.5f,
                         color_alpha(th->text, 0.50f), 0, 0);
        }
    }

    /* ---- status line: the last real result, or this page's honest hint ---- */
    if (have_status) {
        char hint[384];
        snprintf(hint, sizeof(hint), "%s",
                 st->status[0] ? st->status : tab_hint(st));
        fit_text(fd, hint, hint, sizeof(hint), g.val_r - g.lab_x);
        u32 col = st->status[0]
                ? (st->status_err
                       ? color_mix(th->accent, RGB(255, 92, 92), 0.65f)
                       : th->accent)
                : color_alpha(th->text, 0.42f);
        font_draw(s, fd, g.lab_x, top_y(fd, base_y(fd, status_y, g.line_h)),
                  hint, col);
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
    s->scroll = 0.0f;
}

static void move_sel(Settings *s, int delta) {
    int n = tab_rows(s);
    if (n <= 0) return;
    s->sel += delta;
    if (s->sel < 0) s->sel = 0;
    if (s->sel >= n) s->sel = n - 1;
}

/* Row height at a key event uses the same font the painter used, so page
 * scrolling and rows cannot disagree. */
static float key_row_h(void) {
    Font *fr = font_get(FONT_REGULAR, 11);
    return fr ? font_height(fr) + (float)ui_px(8) : 0.0f;
}

static void scroll_by(Settings *s, float dy) {
    s->scroll += dy;
    if (s->scroll < 0.0f) s->scroll = 0.0f;
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

/* A digit picks the nth row of the page's own short list - 1..5 for the text
 * sizes, 1..12 for the themes, 1..6 for the wallpapers. */
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

static void settings_key(App *a, const char *bytes, int n) {
    Settings *s = a->state;
    if (!s || !bytes) return;

    for (int i = 0; i < n; ) {
        unsigned char c = (unsigned char)bytes[i];

        /* While a password is being entered every byte is a secret. */
        if (s->pw_stage) {
            int el = esc_len(bytes, i, n);
            if (el) { i += el; continue; }          /* arrows are not password bytes */
            i++;
            if (c == '\r' || c == '\n') { pw_enter(s); continue; }
            if (c == '\b' || c == 0x7f) {
                if (s->pw_len > 0) s->pw_buf[--s->pw_len] = 0;
                continue;
            }
            if (c == 3) { pw_cancel(s); continue; } /* Ctrl+C */
            if (c == '\t') continue;
            if (c >= 0x20 && s->pw_len < PW_MAX) {
                s->pw_buf[s->pw_len++] = (char)c;
                s->pw_buf[s->pw_len] = 0;
            }
            continue;
        }

        /* arrows arrive as CSI sequences from src/main.c:key_bytes_for() */
        int el = esc_len(bytes, i, n);
        if (el >= 3) {
            char f = bytes[i + 2];
            float rh = key_row_h();
            if (f == 'A') {
                if (tab_rows(s) > 0) move_sel(s, -1);
                else scroll_by(s, -rh);
            } else if (f == 'B') {
                if (tab_rows(s) > 0) move_sel(s, +1);
                else scroll_by(s, rh);
            } else if (f == 'C') {
                set_tab(s, s->tab + 1);
            } else if (f == 'D') {
                set_tab(s, s->tab - 1);
            } else if (f == '5') {                  /* PageUp */
                if (tab_rows(s) > 0) move_sel(s, -5);
                else scroll_by(s, -rh * 4.0f);
            } else if (f == '6') {                  /* PageDown */
                if (tab_rows(s) > 0) move_sel(s, +5);
                else scroll_by(s, rh * 4.0f);
            } else if (f == 'H') {                  /* Home */
                if (tab_rows(s) > 0) s->sel = 0;
                else s->scroll = 0.0f;
            } else if (f == 'F') {                  /* End */
                if (tab_rows(s) > 0) s->sel = tab_rows(s) - 1;
            }
            i += el;
            continue;
        }
        if (el == 1) {                              /* a bare Esc: ignore */
            i += 1;
            continue;
        }

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
    (void)buttons;
    if (!s) return;

    for (int i = 0; i < TAB_COUNT; i++) {
        float tx = s->tab_x[i], ty = s->tab_y[i], tw = s->tab_w[i];
        if (tw <= 0.0f) continue;
        if ((float)x >= tx && (float)x < tx + tw &&
            (float)y >= ty && (float)y < ty + s->tab_h) {
            set_tab(s, i);
            return;
        }
    }

    int n = tab_rows(s);
    if (n > MAX_PAGE_ROWS) n = MAX_PAGE_ROWS;
    for (int i = 0; i < n; i++) {
        float ry = s->hit_y[i], yb = s->hit_yb[i];
        if (ry < 0.0f || yb <= ry) continue;   /* not on screen, not hittable */
        if ((float)y >= ry && (float)y < yb) {
            s->sel = i;
            activate(s);
            return;
        }
    }
}

static void settings_resize(App *a, int w, int h) {
    Settings *s = a->state;
    if (s) { s->w = w; s->h = h; }
}

/* The wheel walks the selection three rows a notch, like every other list in
 * the desktop; a page of facts scrolls instead of moving a selection it does
 * not have. */
static void settings_wheel(App *a, int notches) {
    Settings *s = a->state;
    if (!s || !notches) return;
    if (tab_rows(s) > 0) move_sel(s, -notches * 3);
    else scroll_by(s, -(float)notches * 3.0f * key_row_h());
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
