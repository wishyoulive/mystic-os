/* ============================================================
 *  Mystic OS - apps/settings.c
 *
 *  Settings is one window with eight pages. Every page is a real
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
 *      Display     two independent choices, both applied in the
 *                  running session:
 *
 *                  Canvas - the logical size the compositor draws,
 *                  listed as Match panel / 1920x1080 / ... / 640x400.
 *                  This is the setting that works on a device that
 *                  reports a single mode (every QEMU fbdev): Enter
 *                  calls desk_set_logical() in src/main.c, which calls
 *                  disp_set_logical_size() and reallocates the
 *                  desktop's surfaces on the next frame.  Each row
 *                  shows the factor and bars the current scale mode
 *                  would produce for that canvas, computed by the
 *                  same disp_layout() the blit uses.
 *
 *                  Mode - every resolution the framebuffer driver
 *                  reported in /sys/class/graphics/fbN/modes, read by
 *                  disp_scan_modes() into Display::modes.  When the
 *                  device named no other size the page says which of
 *                  the two it is ("not named by the device" or
 *                  "named only the current size") instead of offering
 *                  a row that cannot do anything.  Enter calls
 *                  disp_set_mode(), which asks the driver with
 *                  FBIOPUT_VSCREENINFO; a refusal is shown with
 *                  errno's own words and the old mode, mapping and
 *                  display.conf are left alone.
 *
 *                  The four scale modes come from src/fb.c and decide
 *                  how the canvas lands on the panel; the preview is
 *                  the canvas, the native size, the blit disp_layout()
 *                  will use (factor and bars), and the cost the last
 *                  disp_present() measured with clock_gettime().  The
 *                  choice is written as "mode=<w>x<h>",
 *                  "logical=<w>x<h>|auto" and "scale=<name>" to
 *                  /etc/mystic/display.conf and to the disk copy
 *                  /root/mystic-display.conf; src/main.c reads it after
 *                  disp_open() and keeps the current mode when the
 *                  saved one is not offered.  Reset applies the native
 *                  mode, Fit and the automatic canvas, and says what
 *                  actually happened.
 *
 *      Theme       every *.theme in /etc/mystic/themes, parsed with
 *                  theme_load() - the very function main.c uses - so
 *                  the swatch fill, bottom colour, accent border and
 *                  text bar are the colours in the real file.  The
 *                  list is rescanned every frame, so editing a theme
 *                  file is visible on the next repaint.  The footer's
 *                  "now" fact is the Theme the desktop passes to this
 *                  draw call, so even the Ctrl+1..9 shortcut (which never
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
 *                  Orbits, Black Hole, Nebula, Synthwave, Dunes), so a
 *                  wallpaper added to src/wallpaper.c gets a row here
 *                  without this file changing.  Enter writes the name
 *                  to /etc/mystic/wallpaper, re-read by main.c once a
 *                  second.
 *
 *                  The last row is the custom image.  There is no file
 *                  chooser, so Enter on that row opens an inline path
 *                  field in the row itself - the same measured box,
 *                  scrolling text and caret Files uses for rename.
 *                  Typing a path and pressing Enter calls
 *                  wallpaper_set_image(path, err, ...), which reads the
 *                  file and DECODES it with src/img.c (PNG, JPEG, BMP,
 *                  PNM) before anything is written; a missing file, a
 *                  directory, an empty file or bytes that are not a
 *                  picture are refused with the real strerror or the
 *                  decoder's own sentence, the field stays open, and
 *                  the wallpaper already on screen is untouched.  On
 *                  success the line "image:/path/to/file.png" is
 *                  written to /etc/mystic/wallpaper and the row shows
 *                  the file's basename, so the list always says which
 *                  file is set.  An empty field removes the custom
 *                  image and goes back to Dusk.
 *
 *                  main.c needs no change for this format:
 *                  wallpaper_from_name() returns WALLPAPER_IMAGE for a
 *                  line that starts with "image:" without reading the
 *                  path, and wallpaper_draw() then reads the path from
 *                  the same file (once per second at most) and draws
 *                  the cached picture instead of the procedural one.
 *                  That is why the desktop shows the image within a
 *                  second of Enter, and immediately at boot.
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
 *                  /etc/mystic lives in the RAM rootfs, so the same
 *                  line is ALSO written atomically to the disk copy
 *                  /root/mystic-passwd (mode 0600) in the same call.
 *                  That is the name persist_settings() and
 *                  restore_settings() use for every other setting
 *                  (they read /root/mystic-<name>). Two one-word
 *                  changes outside this file make the boot restore
 *                  complete: add "passwd" to the loop in
 *                  stage/init:restore_settings (line 82) and to the
 *                  names[] array in src/main.c:531. Apply BOTH in the
 *                  same commit: main.c copies /etc/mystic/<name> to
 *                  /root/mystic-<name>, so without the boot restore it
 *                  would overwrite the good disk copy with the image's
 *                  default on the next theme or scale change. Until the
 *                  image is rebuilt with that change, settings_init()
 *                  re-adopts /root/mystic-passwd whenever it is newer
 *                  than the RAM file, so opening Settings repairs the
 *                  boot loss; the very first unlock after a reboot still
 *                  sees the image's file because init runs first.
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
 *  LOOK - one header with the app's name and no action, then the eight
 *  pages on one navigation band: every page name when they fit, otherwise
 *  the one page name with two arrows.  A page is one column of rows: the
 *  label on the left, the value right-aligned on the one right edge, and a
 *  mark gutter that holds the theme's real colours or a tick on the row in
 *  effect.  The one footer line says what Enter will do, or where the facts
 *  came from, or the result of the last write.  Every padding and gap is a
 *  multiple of the 8 px unit in SP(); the only three fonts are the shared
 *  title/body/small faces; every row is ui_row_h() tall and centred by
 *  ui_top(), so the text scale cannot make rows overlap.  Columns are
 *  dropped by measurement when a window is too narrow.
 *
 *  Input:
 *      Up/Down, j/k        move the selection on a page with controls
 *      Left/Right, Tab     change page
 *      Enter               apply the selected row (or submit a password)
 *      digits 1..9         apply that row of Text size / Theme / Wallpaper
 *      wheel               move the selection (a page of facts scrolls)
 *      Ctrl+C              cancel a password prompt (Esc is consumed by
 *                          main.c before an app sees it)
 *      click a page        change page (a tab, an arrow or the page name)
 *      click a row         select and apply it
 *      click Cancel/Submit in the password prompt
 *  ============================================================ */
#include "mystic.h"
#include "wallpaper.h"

#include <arpa/inet.h>
#include <ctype.h>
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
#include <time.h>
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
/* The disk copy of the same line. src/main.c:persist_settings() and
 * stage/init:restore_settings() use /root/mystic-<name> as the disk copy of
 * every setting; "passwd" is the one name that is not in their lists yet.
 * This file writes it directly so a change is durable immediately, and
 * re-adopts it at startup when the RAM file is older. See the Users note at
 * the top of this file for the two one-word changes that belong in init and
 * main.c. */
#ifndef MYSTIC_PASSWD_SAVED
#define MYSTIC_PASSWD_SAVED "/root/mystic-passwd"
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

/* The display choice, in the format src/main.c reads at start-up:
 *     mode=<w>x<h>
 *     scale=<fit|stretch|integer|1:1>
 * /etc/mystic is a RAM rootfs and stage/init restores ui.conf, wallpaper and
 * current-theme from /root/mystic-<name>, so the same file is written there
 * too; main.c falls back to that copy when the RAM one is missing. */
#ifndef MYSTIC_DISPLAY_CONF
#define MYSTIC_DISPLAY_CONF "/etc/mystic/display.conf"
#endif
#ifndef MYSTIC_DISPLAY_SAVED
#define MYSTIC_DISPLAY_SAVED "/root/mystic-display.conf"
#endif
/* MemAvailable lives here; a host probe points it at a fixture. */
#ifndef MYSTIC_MEMINFO
#define MYSTIC_MEMINFO "/proc/meminfo"
#endif

/* Settings is a built-in app: it runs in the desktop process, so it asks the
 * compositor for the Display it already has open instead of opening /dev/fb0
 * a second time (two mappings of one panel is a way to corrupt the screen).
 * Both are defined in src/main.c; a host probe supplies the same two. */
Display *desk_display(void);
/* The one policy for the size the compositor draws at: the shipped 1280x800
 * design on any panel at least that large, never larger than the panel. */
void desk_design_size(int fb_w, int fb_h, int *w, int *h);
/* The canvas (logical size). (0,0) means "follow the panel"; any other pair
 * is used as the drawing size. Applies now, remembers for the next mode
 * change, and returns 1 when the pair was used as given. */
int  desk_set_logical(Display *d, int w, int h);
/* Re-apply the remembered choice after the panel changed size. */
void desk_follow_mode(Display *d);
/* 1 when the canvas follows the panel (no explicit size was chosen). */
int  desk_logical_auto(void);

/* The four scale modes, in the order the page lists them. Everything that
 * has to name one goes through these two functions, so the label, the value
 * written to display.conf and the enum the core takes cannot drift apart. */
static const char *scale_label(DispScale m) {
    switch (m) {
        case DISP_FIT:        return "Fit";
        case DISP_STRETCH:    return "Stretch";
        case DISP_INTEGER:    return "Integer";
        case DISP_ONE_TO_ONE: return "1:1";
        default:              return "?";
    }
}

static const char *scale_key(DispScale m) {
    switch (m) {
        case DISP_FIT:        return "fit";
        case DISP_STRETCH:    return "stretch";
        case DISP_INTEGER:    return "integer";
        case DISP_ONE_TO_ONE: return "1:1";
        default:              return "fit";
    }
}

/* One line of plain English for each mode. It is the row's value, not a
 * second label, and it is short enough to survive the 320 px window. */
static const char *scale_hint(DispScale m) {
    switch (m) {
        case DISP_FIT:        return "whole screen, bars";
        case DISP_STRETCH:    return "fills, not square";
        case DISP_INTEGER:    return "whole pixels, bars";
        case DISP_ONE_TO_ONE: return "pixel for pixel";
        default:              return "";
    }
}

/* The canvases the page offers, largest first. These are design sizes, not
 * modes: every one is drawn at and then mapped onto the panel by the chosen
 * scale mode, so a device that reports a single mode still gets a real
 * choice. 1280x800 is the size the desktop was authored on, so it is the
 * one most panels should pick. The page adds "Match panel" above these. */
static const struct { int w, h; } CANVAS[] = {
    { 1920, 1080 },
    { 1600,  900 },
    { 1280,  800 },
    { 1024,  768 },
    {  960,  600 },
    {  800,  600 },
    {  640,  400 },
    {  512,  384 },   /* exactly 2x on a 1024x768 or 1280x800 panel, so
                       * Integer has a canvas where it differs from 1:1 */
};
#define CANVAS_COUNT ((int)(sizeof(CANVAS) / sizeof(CANVAS[0])))

/* A snapshot of the Display's public fields. They live in memory (the core
 * keeps them), so this is a read of a struct, not I/O: the page can ask once
 * per frame and still be exactly what the compositor will use. */
typedef struct {
    int      ok;
    int      n;             /* modes the device reports */
    int      mode_source;   /* DispModeSource: none / native only / extra */
    int      named;         /* parseable lines in the mode file */
    int      extra;         /* sizes other than the native one */
    int      cur, native;   /* indices into the Display's mode list */
    DispScale scale;
    int      lw, lh;        /* logical size */
    int      fw, fh;        /* framebuffer size right now */
    int      nw, nh;        /* native panel size */
    DispGeom geom;          /* the exact placement the core uses */
    double   ms;            /* measured cost of the last present */
    char     err[96];
} DispInfo;

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

/* The custom-image path field. src/wallpaper.c refuses a path at 512 bytes,
 * so the field can hold one and the line written to /etc/mystic/wallpaper is
 * "image:" + path + newline. */
#define IMG_PATH_MAX 512

/* The image row's label. One definition, because the row builder, the row
 * painter and the inline field all have to agree on which row is the path. */
#define IMG_ROW_LABEL "Custom image"

enum { TAB_TEXT, TAB_DISP, TAB_THEME, TAB_WALL, TAB_USERS, TAB_NET,
       TAB_STORAGE, TAB_SYSTEM, TAB_COUNT };
static const char *TAB_LABEL[TAB_COUNT] = {
    "Text", "Display", "Theme", "Wallpaper", "Users", "Network", "Storage",
    "System"
};

typedef struct {
    char  file[64];
    Theme theme;                /* parsed by theme_load(): the real colours */
} ThemeRow;

/* One row of whichever page is showing. Built from real data each frame.
 * id >= 0 marks a selectable picker row (Enter applies it); id == -1 is a
 * fact row, which has a value but no action. id is the row's ordinal among
 * the page's selectable rows, so sel can never land on a fact. act/arg say
 * what Enter does without re-parsing the row's text. */
typedef struct {
    int         id;
    char        label[192];
    char        value[224];
    int         current;        /* this row is what is in effect on disk */
    const Theme *swatch;        /* non-NULL: draw this theme's colours here */
    int         act;            /* ACT_*: what Enter does */
    int         arg;            /* mode index or DispScale */
} PRow;

enum { ACT_NONE = 0, ACT_MODE, ACT_SCALE, ACT_LOGICAL, ACT_RESET };

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
    time_t   themes_at;         /* last theme-directory scan (1 s TTL) */
    DispInfo disp;              /* display facts, read from the core's struct */

    /* password flow: 0 idle, 1 old, 2 new, 3 repeat */
    int      pw_stage;
    int      pw_len;
    char     pw_buf[PW_MAX + 1];
    char     pw_first[PW_MAX + 1];
    char     pw_msg[192];
    int      pw_msg_err;

    /* wallpaper path field: 1 while it is open. It owns the keyboard until
     * Enter or Ctrl+C, exactly like the password prompt. */
    int      img_stage;
    int      img_len;
    char     img_buf[IMG_PATH_MAX];
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


/* ONE ELLIPSIS. Every string in this window is measured against the box it
 * has to live in; when it does not fit, the tail becomes a single U+2026 and
 * nothing is ever painted outside its column (or its window). Returned in a
 * caller buffer, so the painter and the hit test see the same string. */
#define ELLIPSIS "\xE2\x80\xA6"

static void fit_ellipsis(Font *f, const char *src, char *out, size_t cap,
                         float maxw) {
    char tmp[512];
    if (!cap) return;
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    out[0] = 0;
    if (!f || maxw <= 0.0f) return;
    if (font_width(f, tmp) <= maxw) {
        snprintf(out, cap, "%s", tmp);
        return;
    }
    /* drop whole UTF-8 characters, never half of one */
    size_t n = strlen(tmp);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) n--;
        tmp[n] = 0;
        char trybuf[600];
        snprintf(trybuf, sizeof(trybuf), "%s" ELLIPSIS, tmp);
        if (font_width(f, trybuf) <= maxw) {
            snprintf(out, cap, "%s", trybuf);
            return;
        }
    }
    /* even the ellipsis alone is wider than the box: draw nothing rather
     * than overflow it */
}

/* Draw text fitted to maxw pixels, ending in one ellipsis when truncated. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    char buf[512];
    if (!text || !text[0] || maxw < 4.0f) return;
    fit_ellipsis(f, text, buf, sizeof(buf), maxw);
    if (buf[0]) font_draw(s, f, x, y, buf, color);
}

/* Best effort: the password buffers are cleared before the page moves on. */
static void secure_zero(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* The page on one grid, and the same four bands Files has:
 *
 *      header      the app's name, nothing else
 *      navigation  every page name when they fit, otherwise one name with
 *                  two arrows - never a rail and never a wrapped strip
 *      body        one column of rows: mark gutter, label, value
 *      footer      one line of facts
 *
 * Every x is one of three edges:
 *
 *      body_x        the window margin
 *      label_x       the left edge of every label (after the mark gutter)
 *      val_r         the right column: every value, right-aligned
 *
 * and every y is one of the bands, all built from the fonts and SP().
 */
typedef struct {
    UiFont f;

    int m, gap;
    int header_h, footer_h;

    int nav_mode;               /* 0 = all page names, 1 = arrows */
    int nav_y, nav_h;
    int tab_x[TAB_COUNT], tab_w[TAB_COUNT];
    int prev_x, next_x, arrow_w, name_x, name_w;

    int body_x, body_r, body_y, body_bot;
    int rows, row_h, row_dy, small_dy;

    /* the row grid: mark gutter, label, value */
    int gut, mark_sz, sw_w, sw_h;
    int label_x, val_r;

    int radius, rule, bar_w2, footer_dy;
} Layout;

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

    /* The navigation band is the body line box with one unit of air above
     * and below - the same band shape as the path bar in Files. A short
     * window compacts the band, then gives up the footer, and only then the
     * band itself: rows of the page are the last thing to go. */
    L->nav_h = ui_ceilf(font_height(L->f.body)) + 2 * SP(1);
    if (L->nav_h < L->row_h) L->nav_h = L->row_h;
    if (L->header_h + L->nav_h + SP(1) + L->footer_h + L->row_h > h)
        L->nav_h = ui_ceilf(font_height(L->f.body));
    if (L->header_h + L->nav_h + SP(1) + L->footer_h + L->row_h > h) L->footer_h = 0;
    if (L->header_h + L->nav_h + L->row_h > h)
        L->nav_h = h - L->header_h - L->row_h;
    if (L->nav_h < 0) L->nav_h = 0;
    if (L->header_h + L->row_h > h) L->header_h = h;

    int top = L->header_h;
    int bottom = h - L->footer_h;
    if (bottom < top) bottom = top;

    L->footer_dy = L->footer_h > 0 ? ui_top(L->f.small, L->footer_h) : 0;
    L->nav_y = top;

    /* all eight names, measured; use the band only when every one fits */
    int x = L->m;
    int total = L->m;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tw = ui_ceilf(font_width(L->f.body, TAB_LABEL[i])) + 2 * SP(1);
        L->tab_x[i] = x;
        L->tab_w[i] = tw;
        x += tw;
        total = x + L->m;
    }
    L->nav_mode = (L->nav_h > 0 && total <= w) ? 0 : 1;
    if (L->nav_mode == 1) {
        L->arrow_w = SP(4);
        if (L->arrow_w > L->nav_h) L->arrow_w = L->nav_h;
        if (L->arrow_w < SP(2)) L->arrow_w = SP(2);
        L->prev_x = L->m;
        L->next_x = w - L->m - L->arrow_w;
        L->name_x = L->prev_x + L->arrow_w + SP(2);
        L->name_w = L->next_x - L->gap - L->name_x;
        if (L->name_w < SP(4)) L->name_w = SP(4);
    }

    L->body_x = L->m;
    L->body_r = w - L->m;
    L->body_y = L->nav_y + L->nav_h + (L->nav_h > 0 ? SP(1) : 0);
    L->body_bot = bottom - SP(1);
    if (L->body_bot < L->body_y) L->body_bot = L->body_y;

    /* the mark gutter only exists on pages that carry a mark, and the label
     * always starts one unit after the mark so the two never touch */
    L->mark_sz = SP(2);
    L->sw_h = L->row_h - SP(1);
    if (L->sw_h > SP(3)) L->sw_h = SP(3);
    if (L->sw_h < SP(1)) L->sw_h = SP(1);
    L->sw_w = SP(5);
    L->gut = 0;
    if (st->tab == TAB_THEME) L->gut = L->sw_w + L->gap;
    else if (st->tab == TAB_TEXT || st->tab == TAB_WALL || st->tab == TAB_DISP)
        L->gut = SP(1) + L->mark_sz + L->gap;
    L->label_x = L->body_x + L->gut;
    L->val_r = L->body_r;

    {
        int avail = L->body_bot - L->body_y;
        L->rows = avail > 0 ? avail / L->row_h : 0;
        if (L->rows > MAX_PAGE_ROWS) L->rows = MAX_PAGE_ROWS;
    }
    return 1;
}

/* one page-name tab */
static int tab_rect(const Layout *L, int i, int *bx, int *by, int *bw, int *bh) {
    if (L->nav_mode != 0 || i < 0 || i >= TAB_COUNT || L->nav_h <= 0) return 0;
    *bx = L->tab_x[i];
    *by = L->nav_y;
    *bw = L->tab_w[i];
    *bh = L->nav_h;
    return 1;
}

/* the page bar's two arrows */
static int arrow_rect(const Layout *L, int which, int *bx, int *by, int *bw, int *bh) {
    if (L->nav_mode != 1 || L->nav_h <= 0) return 0;
    *bx = which ? L->next_x : L->prev_x;
    *by = L->nav_y;
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
    *by = (L->footer_h - h) / 2;               /* relative to the footer top */
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
                          const PRow *r, int ox, int ry, int selectable,
                          int sel, int hot) {
    /* ox is the window's origin on the compositor surface; every column in
     * Layout is app-local, so each x is ox + column. (The y is already the
     * absolute row top, computed by the caller from L->body_y.) */
    if (sel)
        ui_pill(s, ox + L->body_x, ry, L->body_r - L->body_x, L->row_h,
                (float)L->radius, color_alpha(th->accent, 0.18f));
    else if (selectable && hot)
        ui_pill(s, ox + L->body_x, ry, L->body_r - L->body_x, L->row_h,
                (float)L->radius, color_alpha(th->text, 0.05f));

    if (r->swatch) {
        float sy = (float)ry + ((float)L->row_h - (float)L->sw_h) * 0.5f;
        swatch_draw(s, (float)(ox + L->body_x), sy, (float)L->sw_w,
                    (float)L->sw_h, r->swatch);
    } else if (r->current && L->gut > 0) {
        float sy = (float)ry + ((float)L->row_h - (float)L->mark_sz) * 0.5f;
        tick_draw(s, (float)(ox + L->body_x + SP(1)), sy, (float)L->mark_sz,
                  color_alpha(th->accent, 0.92f));
    }

    char vb[256];
    snprintf(vb, sizeof(vb), "%s", r->value);
    float vw = 0.0f;
    if (vb[0]) {
        fit_ellipsis(L->f.small, vb, vb, sizeof(vb),
                 (float)(L->val_r - L->label_x) * 0.62f);
        if (vb[0]) vw = font_width(L->f.small, vb) + (float)L->gap;
    }

    char nb[256];
    fit_ellipsis(L->f.body, r->label, nb, sizeof(nb),
             (float)L->val_r - vw - (float)L->label_x);
    font_draw(s, L->f.body, (float)(ox + L->label_x),
              (float)(ry + L->row_dy), nb,
              sel ? th->text
                  : color_alpha(th->text, hot && selectable ? 0.95f : 0.86f));
    if (vb[0])
        ui_text_right(s, L->f.small, ox + L->val_r, ry + L->small_dy, vb,
                      r->current ? color_alpha(th->accent, 0.95f)
                                 : color_alpha(th->text, 0.55f));
}

/* ---- the custom-image path field -----------------------------------------
 * The same measured box, scrolling text and caret Files draws for a rename:
 * the text scrolls so the caret stays inside, and the caret is placed at the
 * glyph pen (font_width of the text before it), never at a guessed column.
 * It is drawn INSIDE the row it edits, so the page never jumps.
 */
static void img_field_draw(const Settings *st, Surface *s, const Layout *L,
                           const Theme *th, int ox, int ry, int rh) {
    Font *fnt = L->f.body;
    if (!fnt) return;
    float bx = (float)(ox + L->body_x);
    float bw = (float)(L->body_r - L->body_x);
    if (bw < (float)SP(4)) return;
    float by = (float)ry + (float)SP(1) / 2.0f;
    float bh = (float)(rh - SP(1));
    if (bh < (float)SP(2)) return;

    gfx_rrect_aa(s, bx, by, bw, bh, (float)L->radius, th->window_bg,
                 color_alpha(th->accent, 0.90f), 1.0f);

    float room = bw - (float)SP(2);
    int start = 0;
    char tmp[IMG_PATH_MAX];
    for (;;) {
        int len = st->img_len - start;
        if (len <= 0) break;
        if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
        memcpy(tmp, st->img_buf + start, (size_t)len);
        tmp[len] = 0;
        if (font_width(fnt, tmp) <= room) break;
        start++;
    }

    float tx = bx + (float)SP(1);
    float ty = (float)ry + (float)L->row_dy;
    int vis = st->img_len - start;
    if (vis < 0) vis = 0;
    if (vis >= (int)sizeof(tmp)) vis = (int)sizeof(tmp) - 1;
    memcpy(tmp, st->img_buf + start, (size_t)vis);
    tmp[vis] = 0;
    draw_clipped(s, fnt, tx, ty, room, tmp, th->text);

    int caret = st->img_len - start;
    if (caret < 0) caret = 0;
    if (caret > vis) caret = vis;
    memcpy(tmp, st->img_buf + start, (size_t)caret);
    tmp[caret] = 0;
    float cxp = tx + font_width(fnt, tmp);
    float caret_h = bh - (float)SP(1);
    if (caret_h < 4.0f) caret_h = 4.0f;
    gfx_line(s, cxp, by + (float)SP(1) / 2.0f, cxp,
             by + (float)SP(1) / 2.0f + caret_h, 1.4f, th->accent);
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
    FILE *f = fopen(MYSTIC_MEMINFO, "r");
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
 * "baseline" marker is absent. Returns 0 when there is no usable setting.
 *
 * The answer is cached for one second: the draw path asks for it more than
 * once per frame, and the file changes only when the user applies a size
 * (apply_size() drops the cache). */
static time_t scale_read_at;
static float  scale_read_val = 1.0f;
static int    scale_read_ok;

static void scale_cache_drop(void) { scale_read_at = 0; }

static int read_scale(float *out) {
    time_t t = time(NULL);
    if (scale_read_at != 0 && scale_read_at == t) {
        *out = scale_read_ok ? scale_read_val : 1.0f;
        return scale_read_ok;
    }

    float v = 1.0f;
    int marker = 0, got = 0;
    FILE *f = fopen(MYSTIC_UI_CONF, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "baseline", 8)) { marker = 1; continue; }
            if (strncmp(line, "scale", 5)) continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            float x = (float)atof(eq + 1);
            if (!isfinite(x)) continue;          /* NaN passes comparisons */
            if (x < 0.8f) x = 0.8f;
            if (x > 1.6f) x = 1.6f;
            v = x;
            got = 1;
            break;
        }
        fclose(f);
    }
    if (got && !marker) {
        v /= SETTINGS_BASELINE;
        if (v < 0.8f) v = 0.8f;
        if (v > 1.6f) v = 1.6f;
    }
    scale_read_at = t;
    scale_read_val = v;
    scale_read_ok = got;
    *out = v;
    return got;
}

/* The wallpaper on disk: the raw first line, cached for one second like the
 * scale (apply_*() drops the cache). The line is kept, not just the index,
 * because the Wallpaper page shows the custom image's file name. */
static char   wall_line[IMG_PATH_MAX + 8];
static int    wall_line_ok;
static time_t wall_read_at;

static int read_wallpaper_line(void) {
    time_t t = time(NULL);
    if (wall_read_at != 0 && wall_read_at == t) return wall_line_ok;
    wall_line_ok = first_line(MYSTIC_WALLPAPER_FILE, wall_line, sizeof(wall_line));
    wall_read_at = t;
    return wall_line_ok;
}

/* -2 = no file (the desktop keeps its default, 0), -1 = a line no wallpaper
 * or image matches, 0..WALLPAPER_COUNT-1 = that wallpaper, WALLPAPER_IMAGE =
 * a custom picture ("image:<path>"). */
static int read_wallpaper(void) {
    if (!read_wallpaper_line()) return -2;
    return wallpaper_from_name(wall_line);
}

/* The path of an "image:" line, "" otherwise. wallpaper_from_name() has
 * already established the tag; this skips it plus any spaces. */
static void read_wallpaper_image(char *out, size_t n) {
    out[0] = 0;
    if (!read_wallpaper_line()) return;
    if (wallpaper_from_name(wall_line) != WALLPAPER_IMAGE) return;
    const char *p = wall_line;
    while (*p == ' ' || *p == '	') p++;
    p += 6;                                    /* "image:" */
    while (*p == ' ' || *p == '	') p++;
    snprintf(out, n, "%s", p);
}

/* The last path component, for the list. "/" and "" give "". */
static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---- the display ---------------------------------------------------------- */

/* Defined with the other durable writes below; the display's own save path
 * needs it before that definition. */
static int write_atomic(const char *path, const char *data, size_t len,
                        mode_t mode, int *err);

/* Everything the Display page prints comes from here: the mode list the
 * kernel reported, the one in effect, the logical size the compositor draws
 * at, the exact blit the core will use, and the cost it measured. All of
 * them are fields of the display the compositor already holds, so this is a
 * struct read and the draw path never touches the device. */
static DispInfo *disp_info(Settings *s) {
    DispInfo *di = &s->disp;

    memset(di, 0, sizeof(*di));
    di->native = -1;

    Display *d = desk_display();
    if (!d) {
        snprintf(di->err, sizeof(di->err), "%s", "display not open");
        return di;
    }
    di->n = disp_mode_count(d);
    di->mode_source = (int)disp_modes_source(d);
    di->named = disp_modes_named(d);
    di->extra = disp_modes_extra(d);
    di->scale = disp_get_scale(d);
    di->lw = d->w;
    di->lh = d->h;
    di->fw = d->fb_w;
    di->fh = d->fb_h;
    di->ms = disp_present_ms(d);

    di->cur = disp_mode_current(d);
    /* modes[0] is the size the device was in when it was opened: the one
     * disp_scan_modes() can prove the kernel is in, and the list is never
     * reordered when a mode is set. It is therefore the native entry even
     * after disp_set_mode() re-flags the mode in effect. */
    di->native = di->n > 0 ? 0 : -1;
    if (di->cur < 0 || di->cur >= di->n) di->cur = -1;

    const DispMode *nm = di->native >= 0 ? disp_mode_at(d, di->native) : NULL;
    if (nm) {
        di->nw = nm->w;
        di->nh = nm->h;
    } else {
        di->nw = d->fb_w;
        di->nh = d->fb_h;
    }

    /* The placement the next present() will use. Pure arithmetic in the
     * core, so the preview cannot disagree with the blit. */
    di->geom = disp_layout(di->scale, di->lw, di->lh, di->fw, di->fh);
    /* The display is open, so the canvas and scale rows are usable even when
     * the device named no modes at all - which is exactly the QEMU case. */
    di->ok = 1;
    return di;
}

/* One short line for a mapping: the factor(s) and what is lost. The canvas
 * rows, the preview and the status line all call this, so a factor printed
 * in one place cannot disagree with the blit in another. `g` must be
 * disp_layout(scale, cw, ch, fw, fh). */
static void fmt_mapping(char *out, size_t n, const DispGeom *g,
                        int cw, int ch, int fw, int fh) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (!g || cw <= 0 || ch <= 0) return;
    double fx = (double)g->bw / (double)cw;
    double fy = (double)g->bh / (double)ch;
    char fac[32];
    if (fabs(fx - fy) < 0.005)
        snprintf(fac, sizeof(fac), "%.2fx", fx);
    else
        snprintf(fac, sizeof(fac), "%.2f x %.2f", fx, fy);

    if (g->crop) {
        int cropw = cw - g->bw, croph = ch - g->bh;
        if (cropw > 0 && croph > 0)
            snprintf(out, n, "%s, crops %dx%d", fac, cropw, croph);
        else if (croph > 0)
            snprintf(out, n, "%s, crops %d px tall", fac, croph);
        else
            snprintf(out, n, "%s, crops %d px wide", fac, cropw > 0 ? cropw : 0);
        return;
    }
    int bx = fw - g->bw, by = fh - g->bh;
    if (bx > 0 || by > 0)
        snprintf(out, n, "%s, bars %dx%d", fac, bx, by);
    else
        snprintf(out, n, "%s, fills panel", fac);
}

/* The choice, in the exact format src/main.c parses: mode=<w>x<h> and
 * scale=<name>. Written with the same temp+rename as the password file: a
 * power cut must not leave half a resolution behind. Returns 1 when both
 * copies landed, 0 otherwise; *why says which step failed. */
static int display_persist(Settings *s, Display *d, char *why, size_t cap) {
    const DispMode *m = disp_mode_at(d, disp_mode_current(d));
    if (!m) { snprintf(why, cap, "%s", "the core has no current mode"); return 0; }
    /* The canvas is independent of the mode: a panel that offers one mode
     * still restores the size the user drew at. "auto" is written as the
     * word, not the number it resolves to now, so it follows a new panel. */
    char canvas[24];
    if (desk_logical_auto())
        snprintf(canvas, sizeof(canvas), "%s", "auto");
    else
        snprintf(canvas, sizeof(canvas), "%dx%d", d->w, d->h);
    char text[160];
    snprintf(text, sizeof(text), "mode=%dx%d\nlogical=%s\nscale=%s\n",
             m->w, m->h, canvas, scale_key(disp_get_scale(d)));

    int err = 0;
    if (!write_atomic(MYSTIC_DISPLAY_CONF, text, strlen(text), 0644, &err)) {
        snprintf(why, cap, "not saved: %s: %s", MYSTIC_DISPLAY_CONF,
                 strerror(err ? err : EIO));
        return 0;
    }
    if (!write_atomic(MYSTIC_DISPLAY_SAVED, text, strlen(text), 0644, &err)) {
        snprintf(why, cap, "saved for this boot only: %s: %s",
                 MYSTIC_DISPLAY_SAVED, strerror(err ? err : EIO));
        return 0;
    }
    snprintf(why, cap, "saved in %s", MYSTIC_DISPLAY_CONF);
    return 1;
}

/* Enter on a resolution row. The core asks the driver with
 * FBIOPUT_VSCREENINFO and keeps the old mapping when it refuses, so a
 * refusal is reported with errno's own words and the previous mode stays in
 * effect - never a claim that a mode was set. */
static void disp_apply_mode(Settings *s, int idx) {
    Display *d = desk_display();
    if (!d) { snprintf(s->status, sizeof(s->status), "%s", "no display"); s->status_err = 1; return; }
    const DispMode *want = disp_mode_at(d, idx);
    if (!want) { snprintf(s->status, sizeof(s->status), "%s", "that mode is no longer listed"); s->status_err = 1; return; }
    int want_w = want->w, want_h = want->h, hz = want->refresh;

    errno = 0;
    int rc = disp_set_mode(d, idx);
    int e = errno;
    if (rc != 0) {
        /* The core reports -1 both for a driver refusal (old mapping kept)
         * and for a mode the driver took whose new framebuffer could not be
         * mapped (drawing stopped). Those are different outcomes; say which
         * one happened instead of calling both a refusal. */
        const DispMode *now = disp_mode_at(d, disp_mode_current(d));
        if (now && now->w == want_w && now->h == want_h && !d->mem) {
            snprintf(s->status, sizeof(s->status),
                     "panel switched to %dx%d but the new framebuffer could not be mapped: %s; drawing stopped",
                     want_w, want_h, strerror(e ? e : EIO));
        } else {
            snprintf(s->status, sizeof(s->status), "%dx%d refused: %s",
                     want_w, want_h, strerror(e ? e : EIO));
        }
        s->status_err = 1;
        return;
    }

    /* Prove it with the device, not with the return code: a driver that
     * changes something other than the request is a real failure to say. */
    const DispMode *now = disp_mode_at(d, disp_mode_current(d));
    if (!now || now->w != want_w || now->h != want_h) {
        snprintf(s->status, sizeof(s->status),
                 "%dx%d was accepted but the panel reports %dx%d",
                 want_w, want_h, now ? now->w : d->fb_w, now ? now->h : d->fb_h);
        s->status_err = 1;
        return;
    }

    /* A success with no mapping is still a dead screen: disp_set_mode()
     * returns 0 for a mode that is already in effect without remapping, and
     * an earlier failed map can have left d->mem NULL. Say so, do not save
     * a choice that cannot be drawn. */
    if (!d->mem) {
        snprintf(s->status, sizeof(s->status),
                 "%dx%d is set but the framebuffer is not mapped; drawing stopped",
                 want_w, want_h);
        s->status_err = 1;
        return;
    }

    /* Keep the canvas choice across a panel change: auto re-resolves for the
     * new panel, an explicit canvas stays exactly what the user picked. The
     * core applies it to the surfaces and to the inverse mapper. */
    desk_follow_mode(d);

    char map[64];
    DispGeom g = disp_layout(disp_get_scale(d), d->w, d->h, d->fb_w, d->fb_h);
    fmt_mapping(map, sizeof(map), &g, d->w, d->h, d->fb_w, d->fb_h);

    char why[160];
    int saved = display_persist(s, d, why, sizeof(why));
    char hzb[32];
    if (hz > 0) snprintf(hzb, sizeof(hzb), " at %d Hz", hz);
    else hzb[0] = 0;
    snprintf(s->status, sizeof(s->status), "%dx%d%s, canvas %dx%d %s; %s",
             want_w, want_h, hzb, d->w, d->h, map, why);
    s->status_err = saved ? 0 : 1;
}

/* A canvas costs two compositor surfaces (screen + scene) at 4 bytes per
 * pixel, and the machine must still be able to run: the shipped image is
 * started with 32 MB. Refuse a size here, with both real numbers, instead
 * of saving a choice the desktop would find it cannot allocate two frames
 * later - and would then try to restore at the next boot. MemAvailable is
 * the kernel's own figure; when /proc is absent (host probe) the core's
 * compile-time bounds are the only limit. */
#define CANVAS_RESERVE_KB (4 * 1024)
static int canvas_memory_ok(int w, int h, char *why, size_t cap) {
    if (w < 1 || h < 1) return 1;
    long avail = mem_kb("MemAvailable:");
    if (avail <= 0) return 1;                 /* no /proc/meminfo here */
    long need = (long)(((unsigned long long)w * (unsigned long long)h * 8ull) / 1024ull);
    if (need + CANVAS_RESERVE_KB <= avail) return 1;
    snprintf(why, cap,
             "canvas %dx%d needs %ld MB, %ld MB available (4 MB kept free)",
             w, h, (need + 1023) / 1024, avail / 1024);
    return 0;
}

/* Enter on a canvas row: the logical size the compositor draws before the
 * blit. This is pure state in the core (no driver call, so it cannot be
 * refused by the kernel), and the desktop reallocates its surfaces on the
 * next frame - the same session, no restart. The proof shown is the size
 * read back and the mapping the next present will use, both from the
 * Display struct. */
static void disp_apply_logical(Settings *s, int arg) {
    Display *d = desk_display();
    if (!d) { snprintf(s->status, sizeof(s->status), "%s", "no display"); s->status_err = 1; return; }
    int want_w = 0, want_h = 0;
    if (arg >= 0 && arg < CANVAS_COUNT) {
        want_w = CANVAS[arg].w;
        want_h = CANVAS[arg].h;
    }
    char memwhy[160];
    memwhy[0] = 0;
    if (want_w > 0 && !canvas_memory_ok(want_w, want_h, memwhy, sizeof(memwhy))) {
        snprintf(s->status, sizeof(s->status), "%s; not applied", memwhy);
        s->status_err = 1;
        return;
    }
    int exact = desk_set_logical(d, want_w, want_h);
    int got_w = d->w, got_h = d->h;
    /* A clamp is still an applied choice (the core used the nearest size it
     * can allocate), so it is saved - and said in the same line, because a
     * status that says "clamped" while nothing was written would be a lie. */
    DispGeom g = disp_layout(disp_get_scale(d), got_w, got_h, d->fb_w, d->fb_h);
    char map[64];
    fmt_mapping(map, sizeof(map), &g, got_w, got_h, d->fb_w, d->fb_h);

    char why[160];
    int saved = display_persist(s, d, why, sizeof(why));
    if (want_w > 0 && !exact)
        snprintf(s->status, sizeof(s->status),
                 "canvas %dx%d asked, %dx%d used (bounds), %s; %s",
                 want_w, want_h, got_w, got_h, map, why);
    else if (desk_logical_auto())
        snprintf(s->status, sizeof(s->status),
                 "canvas follows the panel: %dx%d, %s; %s", got_w, got_h, map, why);
    else
        snprintf(s->status, sizeof(s->status),
                 "canvas %dx%d, %s; %s", got_w, got_h, map, why);
    s->status_err = saved ? 0 : 1;
}

/* Enter on a scaling row. disp_set_scale() is pure state, so the only proof
 * is reading it back; the next frame is mapped with it. */
static void disp_apply_scale(Settings *s, DispScale want) {
    Display *d = desk_display();
    if (!d) { snprintf(s->status, sizeof(s->status), "%s", "no display"); s->status_err = 1; return; }
    disp_set_scale(d, want);
    DispScale got = disp_get_scale(d);
    if (got != want) {
        snprintf(s->status, sizeof(s->status), "%s refused by the core", scale_label(want));
        s->status_err = 1;
        return;
    }
    char why[160];
    int saved = display_persist(s, d, why, sizeof(why));
    DispGeom g = disp_layout(got, d->w, d->h, d->fb_w, d->fb_h);
    char map[64];
    fmt_mapping(map, sizeof(map), &g, d->w, d->h, d->fb_w, d->fb_h);
    snprintf(s->status, sizeof(s->status), "scaling: %s, %s; %s",
             scale_key(got), map, why);
    s->status_err = saved ? 0 : 1;
}

/* The two defaults, together: the panel's own mode and the scale that fits
 * it without inventing pixels. Said in that order so a refusal of the mode
 * leaves the scale untouched and the message names what failed. */
static void disp_apply_reset(Settings *s) {
    Display *d = desk_display();
    if (!d) { snprintf(s->status, sizeof(s->status), "%s", "no display"); s->status_err = 1; return; }
    /* modes[0]: the size the device was opened in, which disp_scan_modes()
     * puts first and disp_set_mode() never reorders. */
    const DispMode *nm = disp_mode_at(d, 0);
    if (!nm) {
        snprintf(s->status, sizeof(s->status), "%s", "the device did not name a native mode");
        s->status_err = 1;
        return;
    }
    int nat = 0;
    int nw = nm->w, nh = nm->h;

    errno = 0;
    int rc = disp_set_mode(d, nat);
    int e = errno;
    if (rc != 0) {
        snprintf(s->status, sizeof(s->status), "reset refused: %s",
                 strerror(e ? e : EIO));
        s->status_err = 1;
        return;
    }
    /* A reset that leaves no mapping is not a reset anyone can use. */
    if (!d->mem) {
        snprintf(s->status, sizeof(s->status),
                 "native %dx%d is set but the framebuffer is not mapped; drawing stopped",
                 nw, nh);
        s->status_err = 1;
        return;
    }

    /* The driver can accept the request and land somewhere else. Set the
     * scale and the logical surface from what it actually reported, and say
     * the difference rather than calling it a successful reset. */
    if (d->fb_w != nw || d->fb_h != nh) {
        disp_set_scale(d, DISP_FIT);
        desk_set_logical(d, 0, 0);
        snprintf(s->status, sizeof(s->status),
                 "reset changed the panel to %dx%d, not the native %dx%d; not saved",
                 d->fb_w, d->fb_h, nw, nh);
        s->status_err = 1;
        return;
    }
    disp_set_scale(d, DISP_FIT);
    desk_set_logical(d, 0, 0);

    char why[160];
    int saved = display_persist(s, d, why, sizeof(why));
    snprintf(s->status, sizeof(s->status),
             "reset to native: %dx%d, fit, canvas auto (%dx%d); %s",
             nw, nh, d->w, d->h, why);
    s->status_err = saved ? 0 : 1;
}


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
    scale_cache_drop();
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
    wall_read_at = 0;
    /* the name replaces any custom picture in this session too: the module
     * would otherwise keep drawing the picture until the file was re-read */
    wallpaper_clear_image();
    if (write_text(MYSTIC_WALLPAPER_FILE, text, &err)) {
        snprintf(s->status, sizeof(s->status), "wallpaper: %s", name);
        s->status_err = 0;
    } else {
        snprintf(err_txt, sizeof(err_txt), "%s", strerror(err));
        snprintf(s->status, sizeof(s->status), "not saved: %s", err_txt);
        s->status_err = 1;
    }
}

/* Apply a custom image: decode it first (wallpaper_set_image returns the
 * real reason when it cannot), and only then write the setting line. A
 * refused path leaves both the file and the picture on screen alone. */
static void apply_wallpaper_image(Settings *s, const char *path) {
    if (!path[0]) {
        /* an empty field means "no custom image": a real state, not a typo */
        apply_wallpaper(s, 0);
        snprintf(s->status, sizeof(s->status), "%s",
                 "custom image removed; wallpaper: Dusk");
        s->status_err = 0;
        return;
    }

    char err[192];
    if (wallpaper_set_image(path, err, sizeof(err)) != 0) {
        snprintf(s->status, sizeof(s->status), "%s", err);
        s->status_err = 1;
        return;                                 /* the field stays open */
    }

    char text[IMG_PATH_MAX + 16];
    snprintf(text, sizeof(text), "image:%s\n", path);
    int werr = 0;
    wall_read_at = 0;
    int sw = 0, sh = 0;
    wallpaper_image_size(&sw, &sh, NULL, NULL);
    if (write_text(MYSTIC_WALLPAPER_FILE, text, &werr)) {
        snprintf(s->status, sizeof(s->status), "wallpaper: image %s (%dx%d)",
                 path_basename(path), sw, sh);
        s->status_err = 0;
    } else {
        snprintf(s->status, sizeof(s->status),
                 "image applied but not saved: %s", strerror(werr));
        s->status_err = 1;
    }
}

/* Open the inline path field on the Wallpaper page. It starts with the file
 * that is set, so Enter alone re-applies it and Backspace can edit it. */
static void img_open(Settings *s) {
    char path[IMG_PATH_MAX];
    read_wallpaper_image(path, sizeof(path));
    snprintf(s->img_buf, sizeof(s->img_buf), "%s", path);
    s->img_len = (int)strlen(s->img_buf);
    s->img_stage = 1;
    s->status[0] = 0;
    s->status_err = 0;
}

static void img_cancel(Settings *s) {
    s->img_stage = 0;
    s->img_len = 0;
    s->img_buf[0] = 0;
    snprintf(s->status, sizeof(s->status), "%s", "path edit cancelled");
    s->status_err = 0;
}

static void img_commit(Settings *s) {
    apply_wallpaper_image(s, s->img_buf);
    if (s->status_err) return;                  /* refused: let the user fix it */
    s->img_stage = 0;
    s->img_len = 0;
    s->img_buf[0] = 0;
}

/* ---- the theme list ------------------------------------------------------ */

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Read every *.theme the desktop can load, in the same order: filename order
 * (theme_load_all sorts first, src/theme.c:363). theme_load() supplies the
 * real colours and the "name =" field, so the swatch and the applied name can
 * never disagree. Rescanned at most once a second: this is the disk, but a
 * directory and every theme file in it do not change between two frames. */
static void scan_themes(Settings *s) {
    time_t t = time(NULL);
    if (s->themes_at != 0 && s->themes_at == t) return;
    s->themes_at = t;

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

/* Write one small file atomically: a temp file in the same directory,
 * written, fsynced, then rename(2). A power cut leaves either the old bytes
 * or the new ones, never a truncated hash the lock screen cannot parse. */
static int write_atomic(const char *path, const char *data, size_t len,
                        mode_t mode, int *err) {
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (n < 0 || n >= (int)sizeof(tmp)) { *err = ENAMETOOLONG; return 0; }
    *err = 0;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { *err = errno ? errno : EIO; return 0; }

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            break;
        }
        if (w == 0) { *err = EIO; break; }
        off += (size_t)w;
    }
    if (!*err && fchmod(fd, mode) != 0) *err = errno;
    if (!*err && fsync(fd) != 0) *err = errno;
    if (close(fd) != 0 && !*err) *err = errno;

    if (!*err && rename(tmp, path) != 0) *err = errno;
    if (*err) unlink(tmp);
    return *err == 0;
}

/* The format mystic_check_password() parses: <salt-hex>:<rounds>:<hash-hex>.
 * This is used only to refuse to re-adopt a disk copy that is not a whole
 * line: try_unlock() fails OPEN on an unparseable file, so copying a
 * truncated fragment over the RAM file would silently remove the lock. */
static int passwd_line_ok(const char *line) {
    if (!line) return 0;
    const char *c1 = strchr(line, ':');
    if (!c1 || c1 == line) return 0;

    const char *p = c1 + 1;
    int rounds = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        rounds = rounds * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (!digits || rounds <= 0 || *p != ':') return 0;
    p++;

    int hashlen = 0;
    while (isxdigit((unsigned char)*p)) { hashlen++; p++; }
    if (*p || hashlen != 64) return 0;

    size_t saltlen = (size_t)(c1 - line);
    if (saltlen == 0 || saltlen > 64 || (saltlen & 1)) return 0;
    for (const char *q = line; q < c1; q++)
        if (!isxdigit((unsigned char)*q)) return 0;
    return 1;
}

/* Write the lock-screen line to the RAM file (what try_unlock reads) AND to
 * the disk copy that makes it survive a reboot. *err is the live write's
 * errno; *saved_err is the disk copy's (0 when durable). Returns 1 when the
 * lock screen's file was replaced, even if the disk copy failed: the caller
 * must then say "this boot only" instead of claiming permanence. */
static int write_passwd(const char *line, int *err, int *saved_err) {
    *saved_err = 0;
    size_t len = strlen(line);
    if (!write_atomic(MYSTIC_PASSWD, line, len, 0600, err)) return 0;
    if (!write_atomic(MYSTIC_PASSWD_SAVED, line, len, 0600, saved_err)) return 1;
    return 1;
}

/* After a reboot /etc/mystic is the RAM rootfs again and carries the image's
 * default line. When the disk copy is newer, put it back before anything can
 * read the RAM file. 1 = copied, 0 = nothing to do; *err says why not (0
 * when there was nothing to do). A fragment that does not parse is never
 * adopted. */
static int restore_passwd(int *err) {
    *err = 0;

    struct stat ds, ls;
    if (stat(MYSTIC_PASSWD_SAVED, &ds) != 0) {
        if (errno != ENOENT) *err = errno;
        return 0;
    }
    if (!S_ISREG(ds.st_mode)) return 0;

    int have_live = (stat(MYSTIC_PASSWD, &ls) == 0 && S_ISREG(ls.st_mode));
    if (have_live && ds.st_mtime <= ls.st_mtime) return 0;

    FILE *f = fopen(MYSTIC_PASSWD_SAVED, "rb");
    if (!f) { *err = errno ? errno : EIO; return 0; }
    char line[320];
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    int rerr = ferror(f) ? (errno ? errno : EIO) : 0;
    fclose(f);
    if (rerr) { *err = rerr; return 0; }

    line[n] = 0;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!passwd_line_ok(line)) { *err = EINVAL; return 0; }

    if (!write_atomic(MYSTIC_PASSWD, line, n, 0600, err)) return 0;
    return 1;
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

    int err = 0, saved_err = 0;
    if (!write_passwd(rec, &err, &saved_err)) {
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
        if (saved_err) {
            /* The lock screen already reads the new line, but the disk copy
             * that would carry it past a reboot did not land. Say so. */
            pw_set_msg(s, 1, "Password changed for this boot only: /root/mystic-passwd was not written.");
            snprintf(s->status, sizeof(s->status),
                     "changed and verified, but %s: %s",
                     MYSTIC_PASSWD_SAVED, strerror(saved_err));
            s->status_err = 1;
        } else {
            pw_set_msg(s, 0, "Password changed and saved to disk; the file was read back and verified.");
            snprintf(s->status, sizeof(s->status),
                     "lock-screen password changed, verified, saved to %s",
                     MYSTIC_PASSWD_SAVED);
            s->status_err = 0;
        }
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
    p->act = ACT_NONE;
    p->arg = 0;
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
    case TAB_DISP: {
        DispInfo *di = disp_info(st);
        int sid = 0;                     /* ordinal among the selectable rows */
        int auto_cur = desk_logical_auto();
        Display *disp = desk_display();

        /* Canvas first. It is the one choice that changes the picture on a
         * device that reports a single mode, and every row carries the
         * factor and bars the current scale mode would produce - computed by
         * the same disp_layout() the blit uses, so the row cannot promise
         * something the next frame will not do. */
        {
            int aw = 0, ah = 0;
            desk_design_size(di->fw, di->fh, &aw, &ah);
            DispGeom ag = disp_layout(di->scale, aw, ah, di->fw, di->fh);
            char am[64], av[80];
            fmt_mapping(am, sizeof(am), &ag, aw, ah, di->fw, di->fh);
            snprintf(av, sizeof(av), "%d x %d, %s", aw, ah, am);
            n = page_add(rows, n, max, sid++, "Match panel", av, auto_cur, NULL);
            if (n > 0) { rows[n - 1].act = ACT_LOGICAL; rows[n - 1].arg = -1; }
        }
        for (int i = 0; i < CANVAS_COUNT; i++) {
            DispGeom cg = disp_layout(di->scale, CANVAS[i].w, CANVAS[i].h,
                                      di->fw, di->fh);
            char cm[64], lbl[24];
            fmt_mapping(cm, sizeof(cm), &cg, CANVAS[i].w, CANVAS[i].h,
                        di->fw, di->fh);
            snprintf(lbl, sizeof(lbl), "%d x %d", CANVAS[i].w, CANVAS[i].h);
            int cur = !auto_cur && di->lw == CANVAS[i].w && di->lh == CANVAS[i].h;
            n = page_add(rows, n, max, sid++, lbl, cm, cur, NULL);
            if (n > 0) { rows[n - 1].act = ACT_LOGICAL; rows[n - 1].arg = i; }
        }

        /* The panel's own modes, only when the device named a size other
         * than the one in effect. A row that is already current cannot do
         * anything, so when there is no choice the page says which kind of
         * silence it has instead of offering a dead row. */
        if (di->n > 1) {
            /* The panel's own modes, prefixed so a size that is also a
             * canvas cannot be mistaken for it. Index 0 is the size the
             * device was opened in; it stays first even after a mode change
             * re-flags which one is native. */
            char pm[48];
            snprintf(pm, sizeof(pm), "%d sizes reported", di->n);
            n = page_add(rows, n, max, -1, "Panel modes", pm, 0, NULL);
            for (int i = 0; i < di->n; i++) {
                const DispMode *m = disp_mode_at(disp, i);
                int mw = m ? m->w : 0, mh = m ? m->h : 0;
                if (mw <= 0 || mh <= 0) continue;
                char lbl[48];
                snprintf(lbl, sizeof(lbl), "Panel %d x %d", mw, mh);
                const char *tag = (i == di->cur) ? "current"
                                : (i == di->native ? "native" : "");
                int id = sid++;
                n = page_add(rows, n, max, id, lbl, tag, i == di->cur, NULL);
                if (n > 0) { rows[n - 1].act = ACT_MODE; rows[n - 1].arg = i; }
            }
        } else {
            const char *why = di->mode_source == DISP_MODES_NATIVE_ONLY
                            ? "named only the current size"
                            : "not named by the device";
            n = page_add(rows, n, max, -1, "Panel modes", why, 0, NULL);
        }

        /* fit / stretch / integer / 1:1, each with one line saying what it
         * does. The value column is the explanation; the tick is the mode
         * that is in effect, so no label is repeated. */
        for (int m = 0; m < DISP_SCALE_COUNT; m++) {
            DispScale sc = (DispScale)m;
            int id = sid++;
            n = page_add(rows, n, max, id, scale_label(sc), scale_hint(sc),
                         m == (int)di->scale, NULL);
            if (n > 0) { rows[n - 1].act = ACT_SCALE; rows[n - 1].arg = m; }
        }

        /* The preview: four numbers, every one from the Display struct. */
        {
            char a[48], b[48], c[80], e[40];
            snprintf(a, sizeof(a), "%d x %d", di->lw, di->lh);
            snprintf(b, sizeof(b), "%d x %d", di->nw, di->nh);
            fmt_mapping(c, sizeof(c), &di->geom, di->lw, di->lh, di->fw, di->fh);

            if (di->ms > 0.0) snprintf(e, sizeof(e), "%.1f ms", di->ms);
            else              snprintf(e, sizeof(e), "%s", "not measured yet");

            n = page_add(rows, n, max, -1, "Canvas", a, 0, NULL);
            n = page_add(rows, n, max, -1, "Native", b, 0, NULL);
            n = page_add(rows, n, max, -1, "Mapping", c, 0, NULL);
            n = page_add(rows, n, max, -1, "Present", e, 0, NULL);
        }

        if (di->native >= 0) {
            char rv[48];
            int id = sid++;
            snprintf(rv, sizeof(rv), "%d x %d, fit", di->nw, di->nh);
            n = page_add(rows, n, max, id, "Reset to native", rv, 0, NULL);
            if (n > 0) rows[n - 1].act = ACT_RESET;
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
        /* the custom image: its value is the file's basename, so the list
         * says which file is set rather than just "custom" */
        char val[224];
        if (dw == WALLPAPER_IMAGE) {
            char path[IMG_PATH_MAX];
            read_wallpaper_image(path, sizeof(path));
            const char *b = path_basename(path);
            snprintf(val, sizeof(val), "%s", b[0] ? b : "no file");
        } else {
            snprintf(val, sizeof(val), "%s", "none - Enter to type a path");
        }
        n = page_add(rows, n, max, WALLPAPER_IMAGE, IMG_ROW_LABEL, val,
                     dw == WALLPAPER_IMAGE, NULL);
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
        /* The framebuffer's own geometry, distinct from the Display page's
         * logical/mode pair: /sys reports pixels the device holds. */
        read_file("/sys/class/graphics/fb0/virtual_size", v, sizeof(v));
        n = page_add(rows, n, max, -1, "Framebuffer", v, 0, NULL);

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

static int tab_rows(Settings *s) {
    switch (s->tab) {
        case TAB_TEXT:  return SIZE_COUNT;
        case TAB_DISP: {
            /* canvas rows (Match panel + CANVAS_COUNT), the panel modes only
             * when the device named another one, the four scale choices and
             * Reset. It must equal the number of id >= 0 rows build_page()
             * makes. */
            DispInfo *di = disp_info(s);
            int n = 1 + CANVAS_COUNT;
            if (di->n > 1) n += di->n;
            return n + DISP_SCALE_COUNT + (di->native >= 0 ? 1 : 0);
        }
        case TAB_THEME: return s->theme_count;
        case TAB_WALL:  return WALLPAPER_COUNT + 1;
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

/* One footer line of facts. The left cell says where the page's numbers
 * come from (or the result of the last real write); the right cell is the
 * one value the page is about: the scale in effect, the theme, the
 * wallpaper, whether a password is set. No instructions live here. */
static void page_facts(Settings *s, const Theme *live,
                       char *left, size_t ln, char *right, size_t rn) {
    left[0] = 0;
    right[0] = 0;

    switch (s->tab) {
    case TAB_TEXT: {
        float now = 1.0f;
        read_scale(&now);
        snprintf(right, rn, "now %.1fx", (double)now);
        snprintf(left, ln, "%d sizes, %.1fx to %.1fx", SIZE_COUNT,
                 (double)SIZE_VALUE[0], (double)SIZE_VALUE[SIZE_COUNT - 1]);
        break;
    }
    case TAB_DISP: {
        DispInfo *di = disp_info(s);
        if (di->ok) {
            if (di->ms > 0.0)
                snprintf(right, rn, "canvas %dx%d, %.1f ms", di->lw, di->lh, di->ms);
            else
                snprintf(right, rn, "canvas %dx%d", di->lw, di->lh);
            snprintf(left, ln, "%s", "canvas in RAM, panel via /sys; clock_gettime");
        } else {
            snprintf(left, ln, "%s", di->err[0] ? di->err : "display unavailable");
        }
        break;
    }
    case TAB_THEME:
        snprintf(left, ln, "%d themes in %s", s->theme_count, MYSTIC_THEME_DIR);
        if (live && live->name[0]) snprintf(right, rn, "now %s", live->name);
        break;
    case TAB_WALL: {
        snprintf(left, ln, "%d wallpapers + a custom image", WALLPAPER_COUNT);
        int wi = read_wallpaper();
        if (wi == WALLPAPER_IMAGE) {
            char path[IMG_PATH_MAX];
            read_wallpaper_image(path, sizeof(path));
            snprintf(right, rn, "now image %s", path_basename(path));
        } else if (wi >= 0) {
            snprintf(right, rn, "now %s", wallpaper_name(wi));
        } else if (wi == -2) {
            snprintf(right, rn, "%s", "now default");
        } else {
            snprintf(right, rn, "%s", "now unknown");
        }
        if (s->img_stage) {
            /* the field owns the page: the footer says how to leave it, and
               the right cell is cleared so a long hint is not squeezed */
            snprintf(left, ln, "%s",
                     "Enter applies, an empty path removes the image, Ctrl+C cancels");
            right[0] = 0;
        }
        break;
    }
    case TAB_USERS: {
        snprintf(left, ln, "%s", "/etc/mystic/passwd, saved to /root/mystic-passwd");
        char line[256];
        int err = 0;
        int r = read_passwd(line, sizeof(line), &err);
        if (r > 0) snprintf(right, rn, "%s", "set");
        else if (r == 0) snprintf(right, rn, "%s", "not set");
        else snprintf(right, rn, "%s", "unreadable");
        break;
    }
    case TAB_NET:
        snprintf(left, ln, "%s", "/proc/net/route + SIOCGIFADDR + resolv.conf");
        break;
    case TAB_STORAGE:
        snprintf(left, ln, "%s", "statvfs(2) on / and /root");
        break;
    default:
        snprintf(left, ln, "%s", "/proc and /sys");
        break;
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

/* The navigation band: all eight page names on one line, or one name with
 * two arrows when the names cannot fit. The current page is the same pill
 * a selected row uses; hover is the same pill in the row-hover colour. Both
 * are built from Layout rectangles the click handler also uses. */
static void nav_draw(Surface *s, int x, int y, const Layout *L,
                     const Settings *st, const Theme *th) {
    if (L->nav_h <= 0) return;

    if (L->nav_mode == 0) {
        for (int i = 0; i < TAB_COUNT; i++) {
            int bx, by, bw, bh;
            if (!tab_rect(L, i, &bx, &by, &bw, &bh)) continue;
            int on = (i == st->tab);
            int hot = st->px >= bx && st->px < bx + bw &&
                      st->py >= by && st->py < by + bh;
            if (on)
                ui_pill(s, x + bx, y + by, bw, bh, (float)L->radius,
                        color_alpha(th->accent, 0.18f));
            else if (hot)
                ui_pill(s, x + bx, y + by, bw, bh, (float)L->radius,
                        color_alpha(th->text, 0.05f));
            draw_clipped(s, L->f.body, (float)(x + bx + SP(1)),
                         (float)(y + by) + (float)ui_top(L->f.body, bh),
                         (float)(bw - 2 * SP(1)), TAB_LABEL[i],
                         on ? th->text
                            : color_alpha(th->text, hot ? 0.92f : 0.62f));
        }
    } else {
        for (int which = 0; which < 2; which++) {
            int bx, by, bw, bh;
            if (!arrow_rect(L, which, &bx, &by, &bw, &bh)) continue;
            int hot = st->px >= bx && st->px < bx + bw &&
                      st->py >= by && st->py < by + bh;
            if (hot)
                ui_pill(s, x + bx, y + by, bw, bh, (float)L->radius,
                        color_alpha(th->text, 0.05f));
            int sz = L->nav_h - 2 * SP(1);
            if (sz > SP(3)) sz = SP(3);
            if (sz < SP(2)) sz = SP(2);
            if (sz > L->arrow_w - SP(1)) sz = L->arrow_w - SP(1);
            if (sz < SP(1)) sz = SP(1);
            nav_arrow(s, which ? 1 : -1,
                      (float)(x + bx) + (float)(L->arrow_w - sz) * 0.5f,
                      (float)(y + by) + (float)(bh - sz) * 0.5f,
                      (float)sz, color_alpha(th->text, hot ? 0.95f : 0.55f));
        }
        draw_clipped(s, L->f.body, (float)(x + L->name_x),
                     (float)(y + L->nav_y) + (float)ui_top(L->f.body, L->nav_h),
                     (float)L->name_w, TAB_LABEL[st->tab], th->text);
    }

    /* one hairline closes the band, inset like the path rule in Files */
    surf_blend(s, x + L->m, y + L->nav_y + L->nav_h - L->rule,
               L->body_r - L->body_x, L->rule,
               color_alpha(th->panel_border, 0.55f));
}

/* ---- AppVT --------------------------------------------------------------- */

static void settings_init(App *a, int w, int h) {
    Settings *s = calloc(1, sizeof(Settings));
    if (!s) return;
    s->w = w;
    s->h = h;
    s->tab = TAB_TEXT;
    s->px = s->py = -1;

    /* /etc/mystic is RAM: after a reboot the image's default password is
     * there again. Adopt the newer disk copy before the Users page (or the
     * lock screen's next read) can use the stale one. */
    {
        int rerr = 0;
        int restored = restore_passwd(&rerr);
        if (rerr) {
            snprintf(s->status, sizeof(s->status),
                     "cannot re-adopt %s: %s", MYSTIC_PASSWD_SAVED, strerror(rerr));
            s->status_err = 1;
        } else if (restored) {
            snprintf(s->status, sizeof(s->status),
                     "restored the saved password from %s", MYSTIC_PASSWD_SAVED);
            s->status_err = 0;
        }
    }

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

    /* The selection is an ordinal among the selectable rows, and a page may
     * carry fact rows between them (the Display preview sits between the
     * scale choices and Reset). Find the row it names, then keep that row
     * visible; using the ordinal as a row index is what made the old page
     * stop scrolling short of its last row. */
    int sel_row = -1;
    if (sel_n > 0) {
        for (int i = 0; i < nr; i++)
            if (rows[i].id == st->sel) { sel_row = i; break; }
    }
    if (sel_row >= 0) {
        if (sel_row < st->scroll) st->scroll = sel_row;
        if (sel_row >= st->scroll + L.rows) st->scroll = sel_row - L.rows + 1;
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
            int hot = selectable && st->px >= L.body_x &&
                      st->px < L.body_r && st->py >= ryt &&
                      st->py < ryt + rh;
            page_row_draw(s, &L, th, &rows[idx], x, y + ryt,
                          selectable, sel, hot);
            if (st->img_stage && rows[idx].id == WALLPAPER_IMAGE)
                img_field_draw(st, s, &L, th, x, y + ryt, rh);
        }
    }

    /* ---- a map of the page, in the right margin ---- */
    if (nr > L.rows && L.rows > 0) {
        int band = L.body_bot - L.body_y;
        int bx = x + w - L.bar_w2 - SP(1);
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

    /* ---- one footer line of facts (and the password's two buttons) ---- */
    if (L.footer_h > 0) {
        int fy = y + h - L.footer_h;
        surf_blend(s, x, fy, w, L.footer_h, th->panel);
        surf_blend(s, x, fy, w, L.rule, th->panel_border);

        char fact[256], right[128];
        page_facts(st, th, fact, sizeof(fact), right, sizeof(right));
        const char *line = fact;
        u32 col = color_alpha(th->text, 0.45f);
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
        }
        /* the result line owns the footer; do not repeat the same value on
         * the right of it */
        if (line != fact) right[0] = 0;

        int text_r = x + w - L.m;
        if (st->pw_stage) {
            /* the prompt owns the footer: its message and two buttons */
            right[0] = 0;
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
        } else if (right[0]) {
            /* only draw the right fact when it leaves the left one room */
            float rw = font_width(L.f.small, right);
            if ((float)(text_r - (x + L.m)) >= rw + 2 * L.gap) {
                ui_text_right(s, L.f.small, text_r, fy + L.footer_dy, right,
                              color_alpha(th->text, 0.42f));
                text_r -= (int)rw + L.gap;
            }
        }
        int avail = text_r - (x + L.m);
        if (avail >= SP(4))
            draw_clipped(s, L.f.small, (float)(x + L.m),
                         (float)(fy + L.footer_dy), (float)avail, line, col);
    }
}

/* ---- input --------------------------------------------------------------- */

static void set_tab(Settings *s, int t) {
    if (t < 0) t = TAB_COUNT - 1;
    if (t >= TAB_COUNT) t = 0;
    if (t == s->tab) return;
    if (s->pw_stage) pw_cancel(s);
    if (s->img_stage) { s->img_stage = 0; s->img_len = 0; s->img_buf[0] = 0; }
    s->tab = t;
    s->sel = 0;
    s->scroll = 0;
    /* a result belongs to the page that produced it */
    s->status[0] = 0;
    s->status_err = 0;
    s->pw_msg[0] = 0;
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
        case TAB_DISP: {
            /* The page mixes mode rows, scale rows and Reset with fact rows,
             * so Enter is resolved against the row the selection names. */
            PRow rows[MAX_PAGE_ROWS];
            int nr = build_page(s, NULL, rows, MAX_PAGE_ROWS);
            for (int i = 0; i < nr; i++) {
                if (rows[i].id != s->sel || !rows[i].act) continue;
                switch (rows[i].act) {
                    case ACT_MODE:    disp_apply_mode(s, rows[i].arg); break;
                    case ACT_SCALE:   disp_apply_scale(s, (DispScale)rows[i].arg); break;
                    case ACT_LOGICAL: disp_apply_logical(s, rows[i].arg); break;
                    case ACT_RESET:   disp_apply_reset(s); break;
                    default: break;
                }
                break;
            }
            break;
        }
        case TAB_THEME:
            apply_theme(s, s->sel);
            break;
        case TAB_WALL:
            if (s->sel < WALLPAPER_COUNT) apply_wallpaper(s, s->sel);
            else img_open(s);
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
            else if (idx == WALLPAPER_IMAGE) { s->sel = idx; img_open(s); }
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

        /* The wallpaper path field owns the keyboard while it is open, the
         * same way the password prompt does. Esc is consumed by the desktop
         * before an app sees it, so Ctrl+C cancels here too. */
        if (s->img_stage) {
            int el = esc_len(bytes, i, n);
            if (el) { i += el; continue; }
            i++;
            if (c == '\r' || c == '\n') { img_commit(s); continue; }
            if (c == '\b' || c == 0x7f) {
                if (s->img_len > 0) s->img_buf[--s->img_len] = 0;
                continue;
            }
            if (c == 3) { img_cancel(s); continue; }
            if (c == '\t') continue;
            if (c >= 0x20 && s->img_len < IMG_PATH_MAX - 1) {
                s->img_buf[s->img_len++] = (char)c;
                s->img_buf[s->img_len] = 0;
            }
            continue;
        }

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
        if (!tab_rect(&L, i, &bx, &by, &bw, &bh)) continue;
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
    /* the path field is a modal inside the page: the tabs above still work
     * (set_tab() cancels the edit), a click anywhere below it does not */
    if (s->img_stage) return;

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
