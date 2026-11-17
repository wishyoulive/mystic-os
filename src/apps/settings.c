/* ============================================================
 *  Mystic OS - apps/settings.c
 *
 *  What is on screen, top to bottom:
 *
 *      Display      the real framebuffer size
 *      Memory       the real MemTotal/MemAvailable from /proc
 *      Text size    the five scales ui_scale_load() accepts
 *      Wallpaper    every wallpaper wallpaper_name() knows
 *      Theme        every theme file in /etc/mystic/themes
 *      System       kernel release, uptime
 *      status       the real result of the last write
 *
 *  The two new controls sit directly under the facts because they are the
 *  ones you come here to change: at the 520x360 window the app opens with,
 *  five rows of themes used to push them past the bottom edge. The theme
 *  list is last because it is the longest list, and it scrolls exactly the
 *  way it always did.
 *
 *  The three lists are one flat list of rows, in that same order: `sel`
 *  walks Text size, then Wallpaper, then Theme. Up/Down (and j/k) move it,
 *  Enter applies the row under it, a click applies the row that was
 *  painted there, and the digits pick within whichever list the selection
 *  is in - 1..5 for a text size, 1..4 for a wallpaper, and 1..9 for a
 *  theme, which is what this app always did.
 *
 *  Applying means writing one of the three files the desktop watches:
 *
 *      /etc/mystic/current-theme      theme name, one line
 *      /etc/mystic/ui.conf            "scale = 1.2"
 *      /etc/mystic/wallpaper          wallpaper name, one line
 *
 *  src/main.c re-reads ui.conf and wallpaper once a second
 *  ("Settings can change these at runtime", main.c:1426) and
 *  current-theme in the same block, so a click takes effect within a
 *  second and never needs a restart.
 *
 *  Nothing here remembers what it wrote. Every frame goes back to the
 *  files and marks the row whose value is on disk with a "now", so the
 *  highlight is a fact about the disk - it follows an external edit, a
 *  failed write, or a file that was never there.
 *
 *  Every vertical metric is derived from the font that draws in it, at
 *  draw time (font_get() applies the global text scale, src/font.c:125).
 *  One measure pass per frame produces the row offsets, and the painter
 *  records the exact rectangle it painted for every row; settings_click()
 *  hit-tests those recorded rectangles and nothing else, so a click lands on
 *  the row that was drawn there - at any text scale. There is no fixed row
 *  height anywhere in this file.
 * ============================================================ */
#include "mystic.h"
#include "wallpaper.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THEMES 12

/* The files the desktop watches. These are macros so a host harness can run
 * this app against a scratch directory; the desktop build uses the real paths
 * (stage/etc/mystic) and needs no -D at all. */
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

/* The five choices in the Text size section. They span exactly the range
 * ui_scale_load() accepts (src/ui.c clamps to 0.8 .. 1.6), so every row
 * written here is a value the desktop will really use. */
#define SIZE_COUNT 5
static const char  *SIZE_LABEL[SIZE_COUNT] = { "Small", "Normal", "Large", "Larger", "Huge" };
static const float  SIZE_VALUE[SIZE_COUNT] = { 0.8f, 1.0f, 1.2f, 1.4f, 1.6f };

/* One flat list: text-size rows, then wallpaper rows, then theme rows. */
#define MAX_ROWS (MAX_THEMES + SIZE_COUNT + WALLPAPER_COUNT)

typedef struct {
    char file[64];
    char name[64];
} ThemeRow;

typedef struct {
    ThemeRow rows[MAX_THEMES];
    int      count;              /* themes found on disk */
    int      sel;                /* index into the flat row list */
    int      w, h;
    float    scroll;             /* pixels the list is scrolled down */
    char     status[96];         /* real result of the last write */
    /* window-local top and bottom of every flat row's painted rectangle, or
     * -1 when the list cannot show it. Written by the painter, read by the
     * click handler: the hit box is the rectangle that was drawn - clipped
     * with it at the list's edges - not a second guess at the layout. */
    float    row_y[MAX_ROWS];
    float    row_yb[MAX_ROWS];
} Settings;

/* ---- geometry: every metric comes from the font that fills it ------------- */

/* One list row: the row's own font line box plus 4 px of leading. */
static float row_h_of(Font *fr) { return font_height(fr) + 4.0f; }

/* One ordinary text line: line box plus 2 px. */
static float line_h_of(Font *f) { return font_height(f) + 2.0f; }

/* One section heading, including the gap under it. */
static float head_h_of(Font *fh) { return font_height(fh) + 4.0f; }

static int flat_count(const Settings *s) {
    return SIZE_COUNT + WALLPAPER_COUNT + s->count;
}

/* The flat list is in the order the sections are drawn: every Text size row,
 * then every Wallpaper row, then every Theme row. */
#define ROW_WALL(k)  (SIZE_COUNT + (k))
#define ROW_THEME(i) (SIZE_COUNT + WALLPAPER_COUNT + (i))

/* 0 = Theme, 1 = Text size, 2 = Wallpaper. */
static int section_of(int sel) {
    if (sel < SIZE_COUNT) return 1;
    if (sel < SIZE_COUNT + WALLPAPER_COUNT) return 2;
    return 0;
}

/* ---- reading the facts back off the disk --------------------------------- */

static void read_file(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(out, n, "unavailable"); return; }
    if (fgets(out, (int)n, f)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
    } else {
        snprintf(out, n, "empty");
    }
    fclose(f);
}

/* One line at a time. The old token loop required three fields per line
 * (key, number, unit) and stopped at the first line that had fewer, so any
 * key below a two-field entry such as "HugePages_Total: 0" read as 0. */
static long mem_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    size_t klen = strlen(key);
    char line[256];
    long v = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        v = strtol(line + klen, NULL, 10);
        break;
    }
    fclose(f);
    return v;
}

/* The scale the desktop is really using: same file, same key, same clamp and
 * same 1.0 fallback as ui_scale_load() in src/ui.c, read now instead of
 * remembered. Returns 0 when the file has no usable "scale" line (the desktop
 * then runs at 1.0), so the caller can say so rather than pretend. */
static int read_scale(float *out) {
    *out = 1.0f;
    FILE *f = fopen(MYSTIC_UI_CONF, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "scale", 5) != 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        float v = (float)atof(eq + 1);
        /* NaN passes both comparisons, which is why ui.c rejects it too */
        if (!isfinite(v)) continue;
        if (v < 0.8f) v = 0.8f;
        if (v > 1.6f) v = 1.6f;
        *out = v;
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

/* The wallpaper the desktop is really showing. wallpaper_from_name() ignores
 * case and surrounding whitespace, exactly as the desktop's own re-read does
 * (src/main.c:1439), so this cannot disagree with the screen.
 *   >= 0  that wallpaper index
 *   -1    the file holds a name that is not a wallpaper (desktop keeps its own)
 *   -2    no file at all - the desktop keeps its default, wallpaper 0 (Dusk) */
static int read_wallpaper(void) {
    FILE *f = fopen(MYSTIC_WALLPAPER_FILE, "r");
    if (!f) return -2;
    char line[128];
    int idx = -1;
    if (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        idx = wallpaper_from_name(line);
    }
    fclose(f);
    return idx;
}

/* ---- writing the facts back to the disk ---------------------------------- */

/* One small file, written whole: open, write, flush, close - every step
 * checked, and the caller gets the errno of whichever step failed, so the
 * status line can name the real reason instead of guessing. */
static int write_line(const char *path, const char *text, int *err) {
    *err = 0;
    FILE *f = fopen(path, "w");
    if (!f) { *err = errno ? errno : EIO; return 0; }
    errno = 0;
    int e = fprintf(f, "%s\n", text) > 0 ? 0 : (errno ? errno : EIO);
    if (!e && fflush(f) != 0) e = errno ? errno : EIO;
    if (fclose(f) != 0 && !e) e = errno ? errno : EIO;
    *err = e;
    return e == 0;
}

/* The desktop re-reads current-theme once a second and matches the first line
 * against Theme::name - the "name =" field inside the theme file (main.c:1178),
 * not its file name. Writing ThemeRow::file matched no theme and did nothing;
 * this writes the name and reports the real result. */
static void apply_theme(Settings *s, int i) {
    if (i < 0 || i >= s->count) return;
    const ThemeRow *r = &s->rows[i];
    int err = 0;
    if (write_line(MYSTIC_CURRENT_THEME, r->name, &err))
        snprintf(s->status, sizeof(s->status), "theme: %s", r->name);
    else
        snprintf(s->status, sizeof(s->status), "not saved: %s", strerror(err));
}

static void apply_size(Settings *s, int i) {
    if (i < 0 || i >= SIZE_COUNT) return;
    char val[32];
    snprintf(val, sizeof(val), "scale = %.1f", SIZE_VALUE[i]);
    int err = 0;
    if (write_line(MYSTIC_UI_CONF, val, &err))
        snprintf(s->status, sizeof(s->status), "text size: %s (%.1fx)",
                 SIZE_LABEL[i], SIZE_VALUE[i]);
    else
        snprintf(s->status, sizeof(s->status), "not saved: %s", strerror(err));
}

static void apply_wallpaper(Settings *s, int i) {
    const char *name = wallpaper_name(i);
    if (!name) return;
    int err = 0;
    if (write_line(MYSTIC_WALLPAPER_FILE, name, &err))
        snprintf(s->status, sizeof(s->status), "wallpaper: %s", name);
    else
        snprintf(s->status, sizeof(s->status), "not saved: %s", strerror(err));
}

/* Apply whatever row the selection is on. One place decides what a row means,
 * so a click, Enter and a digit cannot disagree about it. */
static void activate(Settings *s) {
    if (s->sel < 0) return;
    if (s->sel < SIZE_COUNT)
        apply_size(s, s->sel);
    else if (s->sel < SIZE_COUNT + WALLPAPER_COUNT)
        apply_wallpaper(s, s->sel - SIZE_COUNT);
    else
        apply_theme(s, s->sel - SIZE_COUNT - WALLPAPER_COUNT);
}

static void move_sel(Settings *s, int delta) {
    int n = flat_count(s);
    if (n <= 0) return;
    s->sel += delta;
    if (s->sel < 0) s->sel = 0;
    if (s->sel >= n) s->sel = n - 1;
}

/* ---- the theme list ------------------------------------------------------ */

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const ThemeRow *)a)->file, ((const ThemeRow *)b)->file);
}

static void scan_themes(Settings *s) {
    s->count = 0;
    DIR *d = opendir(MYSTIC_THEME_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s->count < MAX_THEMES) {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, ".theme")) continue;
        /* ThemeRow::file is what the sort order and the name fallback use, so
         * a name that does not fit would be stored truncated and the row
         * would read as a theme that does not exist. Skip it instead of
         * listing a row that cannot work. (A .theme entry is <= NAME_MAX.) */
        if (len >= sizeof(s->rows[0].file)) continue;

        ThemeRow *r = &s->rows[s->count];
        snprintf(r->file, sizeof(r->file), "%s", e->d_name);
        snprintf(r->name, sizeof(r->name), "%s", e->d_name);   /* fallback */

        char path[320], line[256];
        snprintf(path, sizeof(path), "%s/%s", MYSTIC_THEME_DIR, e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "name", 4)) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        char *v = eq + 1;
                        while (*v == ' ') v++;
                        char *nl = strchr(v, '\n');
                        if (nl) *nl = 0;
                        snprintf(r->name, sizeof(r->name), "%s", v);
                    }
                    break;
                }
            }
            fclose(f);
        }
        /* "name =" present but empty: keep the file name rather than write
         * an empty line, which no theme's name can ever match. */
        if (!r->name[0]) snprintf(r->name, sizeof(r->name), "%s", r->file);
        s->count++;
    }
    closedir(d);
    /* same order as the desktop: by filename, so "1 2 3" mean the same thing */
    qsort(s->rows, (size_t)s->count, sizeof(ThemeRow), cmp_name);
}

/* ---- AppVT --------------------------------------------------------------- */

static void settings_init(App *a, int w, int h) {
    Settings *s = calloc(1, sizeof(Settings));
    if (!s) return;
    s->w = w;
    s->h = h;
    /* -1 is "no row here": 0.0f would look like a real hit box at the top */
    for (int i = 0; i < MAX_ROWS; i++) {
        s->row_y[i] = -1.0f;
        s->row_yb[i] = -1.0f;
    }
    scan_themes(s);
    a->state = s;
}

static void settings_free(App *a) {
    free(a->state);
    a->state = NULL;
}

/* Clip to the rectangle we were given. The window manager hands over the
 * whole scene surface (main.c:434), so keeping pixels inside the app rect
 * is the app's job. Coordinates below stay absolute; shortening the
 * surface to the rect's right/bottom edge makes the bounds check inside
 * every gfx_ and font_draw call reject anything past it, and negative x/y
 * is rejected there too. */
static Surface rect_clip(Surface *s, int x, int y, int w, int h) {
    Surface c = *s;
    int right = x + w, bottom = y + h;
    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;
    if (right < c.w) c.w = right;
    if (bottom < c.h) c.h = bottom;
    return c;
}

static void settings_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Settings *st = a->state;
    if (!st) return;
    st->w = w;
    st->h = h;

    Surface clip = rect_clip(s, x, y, w, h);
    s = &clip;

    Font *fh = font_get(FONT_MEDIUM, 13);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr || !fd) return;

    const float rh = row_h_of(fr);          /* the one row height: draw and click */
    const float lh = line_h_of(fr);
    const float hh = head_h_of(fh);
    char line[256];

    /* the facts, re-read every frame: never what we last wrote */
    float cur_scale = 1.0f;
    int have_scale = read_scale(&cur_scale);
    int cur_wall = read_wallpaper();

    float py = (float)y + 6;

    /* ---- Display ---- */
    font_draw(s, fh, (float)x + 6, py, "Display", th->text);
    py += hh;
    read_file("/sys/class/graphics/fb0/virtual_size", line, sizeof(line));
    char pix[96];
    snprintf(pix, sizeof(pix), "  %s pixels", line);
    font_draw(s, fr, (float)x + 6, py, pix, th->text_dim);
    py += lh + 4.0f;

    /* ---- Memory ---- */
    font_draw(s, fh, (float)x + 6, py, "Memory", th->text);
    py += hh;
    long total = mem_kb("MemTotal:"), avail = mem_kb("MemAvailable:");
    long used = total - avail;
    if (used < 0) used = 0;
    float frac = total > 0 ? (float)used / (float)total : 0;
    float mw = (float)w - 12;
    if (mw < 1.0f) mw = 1.0f;
    gfx_rrect_aa(s, (float)x + 6, py, mw, 7, 3.5f, color_alpha(th->text, 0.10f), 0, 0);
    if (frac > 0.01f)
        gfx_rrect_aa(s, (float)x + 6, py, mw * frac, 7, 3.5f, th->accent, 0, 0);
    py += 12;                      /* the 7 px bar and the gap under it, not text */
    snprintf(line, sizeof(line), "  %ld MB used of %ld MB", used / 1024, total / 1024);
    font_draw(s, fr, (float)x + 6, py, line, th->text_dim);
    py += lh + 8.0f;

    /* ---- the scrolling list: Text size, Wallpaper, Theme ---- */
    int list_top = (int)(py + 0.5f);

    /* One measure pass. Offsets are relative to the top of the list, so they
     * do not depend on the window height or on the scroll position - the same
     * numbers serve the scroll clamp, the painter and the hit boxes. */
    int nt = st->count;
    int n = flat_count(st);
    if (n > MAX_ROWS) n = MAX_ROWS;
    float head[3] = { 0.0f, 0.0f, 0.0f };
    float row[MAX_ROWS];
    for (int i = 0; i < MAX_ROWS; i++) row[i] = 0.0f;
    float cy = 0.0f;
    head[0] = cy;                    cy += hh;
    for (int i = 0; i < SIZE_COUNT; i++)       { row[i] = cy; cy += rh; }
    cy += 8.0f;
    head[1] = cy;                    cy += hh;
    for (int i = 0; i < WALLPAPER_COUNT; i++)  { row[ROW_WALL(i)] = cy; cy += rh; }
    cy += 8.0f;
    head[2] = cy;                    cy += hh;
    for (int i = 0; i < nt && i < MAX_THEMES; i++) { row[ROW_THEME(i)] = cy; cy += rh; }
    const float content_h = cy;

    /* Bottom blocks are pinned to the bottom and only drawn when they really
     * fit, so at the 280x160 minimum the rows simply get fewer instead of
     * being painted over the frame and the desktop. */
    const float status_y  = (float)(y + h) - 6.0f - line_h_of(fd);
    const int   have_status = status_y >= (float)list_top;
    const float bottom_limit = have_status ? status_y - 4.0f : (float)(y + h) - 4.0f;
    const float sys_h = hh + (lh + 1.0f) * 2.0f;
    const float sys_top = bottom_limit - sys_h;
    const int   have_sys = (sys_top - 8.0f - (float)list_top) >= rh;
    const float list_bottom = have_sys ? sys_top - 8.0f : bottom_limit;
    float vis_h = list_bottom - (float)list_top;
    if (vis_h < 0.0f) vis_h = 0.0f;

    float max_scroll = content_h - vis_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;

    /* keep the selected row inside the visible window */
    if (n > 0) {
        if (st->sel < 0) st->sel = 0;
        if (st->sel >= n) st->sel = n - 1;
        float sy = row[st->sel];
        if (sy - st->scroll < 0.0f) st->scroll = sy;
        if (sy + rh - st->scroll > vis_h) st->scroll = sy + rh - vis_h;
    } else {
        st->scroll = 0.0f;
    }
    if (st->scroll < 0.0f) st->scroll = 0.0f;
    if (st->scroll > max_scroll) st->scroll = max_scroll;

    /* The list gets its own surface whose first row is the list's top edge, so
     * a row that scrolls past it is rejected by the same bounds check every
     * other draw already uses - it cannot creep over Display or Memory. */
    Surface ls = *s;
    if (list_top > 0 && list_top < ls.h) {
        ls.px += (size_t)list_top * (size_t)ls.stride;
        ls.h -= list_top;
    } else if (list_top >= ls.h) {
        ls.h = 0;
    }
    if ((int)vis_h < ls.h) ls.h = (int)vis_h;
    if (ls.h < 0) ls.h = 0;

    const float ly = -st->scroll;    /* list-local y of the content's top edge */

    /* headings, each with the fact it is about, read back from the file */
    char fact[80];
    font_draw(&ls, fh, (float)x + 6, ly + head[0], "Text size", th->text);
    if (have_scale) snprintf(fact, sizeof(fact), "now %.1fx", cur_scale);
    else            snprintf(fact, sizeof(fact), "now 1.0x (no setting)");
    font_draw(&ls, fr, (float)x + 12 + font_width(fh, "Text size"),
              ly + head[0], fact, th->text_dim);

    font_draw(&ls, fh, (float)x + 6, ly + head[1], "Wallpaper", th->text);
    if (cur_wall >= 0)       snprintf(fact, sizeof(fact), "now %s", wallpaper_name(cur_wall));
    else if (cur_wall == -2) snprintf(fact, sizeof(fact), "now Dusk (no setting)");
    else                     snprintf(fact, sizeof(fact), "now: unknown name");
    font_draw(&ls, fr, (float)x + 12 + font_width(fh, "Wallpaper"),
              ly + head[1], fact, th->text_dim);

    font_draw(&ls, fh, (float)x + 6, ly + head[2], "Theme", th->text);

    for (int i = 0; i < n; i++) {
        float ry = ly + row[i];
        /* the painted rectangle is (ry - 2) tall enough for rh, cut off at the
         * list's own top and bottom edges - the hit box is that same shape */
        float top = ry - 2.0f, bot = ry + rh - 2.0f;
        if (top < 0.0f) top = 0.0f;
        if (bot > vis_h) bot = vis_h;
        if (bot > top) {
            st->row_y[i]  = (float)(list_top - y) + top;
            st->row_yb[i] = (float)(list_top - y) + bot;
        } else {
            st->row_y[i] = st->row_yb[i] = -1.0f;
        }
        if (ry + rh <= 0.0f || ry >= vis_h) continue;

        int selected = (i == st->sel);
        int current = 0;
        char label[128];

        if (i < SIZE_COUNT) {
            int k = i;
            snprintf(label, sizeof(label), "%d   %-7s %.1fx",
                     k + 1, SIZE_LABEL[k], SIZE_VALUE[k]);
            current = fabsf(cur_scale - SIZE_VALUE[k]) < 0.005f;
        } else if (i < SIZE_COUNT + WALLPAPER_COUNT) {
            int k = i - SIZE_COUNT;
            const char *nm = wallpaper_name(k);
            snprintf(label, sizeof(label), "%d   %s", k + 1, nm ? nm : "?");
            current = (cur_wall == k);
        } else {
            int t = i - SIZE_COUNT - WALLPAPER_COUNT;
            snprintf(label, sizeof(label), "%d   %s", t + 1, st->rows[t].name);
        }

        if (selected)
            gfx_rrect_aa(&ls, (float)x + 4, ry - 2, (float)w - 8, rh, 4.0f,
                         color_alpha(th->accent, 0.20f), 0, 0);
        font_draw(&ls, fr, (float)x + 10, ry, label,
                  selected ? th->text : (current ? th->accent : th->text_dim));
        if (current) {               /* what the file says, right where it is */
            const char *mk = "now";
            float mk_w = font_width(fr, mk);
            /* stop short of the scroll triangles in the corner */
            font_draw(&ls, fr, (float)x + (float)w - 26.0f - mk_w, ry, mk,
                      color_alpha(th->accent, 0.85f));
        }
    }

    /* A section below the window is never a secret: a small triangle sits at
     * whichever edge the list continues past, in the font's own size. */
    if (vis_h > line_h_of(fd) * 1.5f) {
        const float t = font_height(fd) * 0.55f;
        float mx = (float)x + (float)w - 12.0f - t;
        if (st->scroll + vis_h < content_h - 0.5f) {
            float my = list_bottom - t - 5.0f;
            float dn[6] = { mx, my, mx + t, my, mx + t * 0.5f, my + t * 0.8f };
            gfx_polygon(s, dn, 3, color_alpha(th->text, 0.45f));
        }
        if (st->scroll > 0.5f) {
            float my = (float)list_top + 4.0f;
            float up[6] = { mx, my + t * 0.8f, mx + t, my + t * 0.8f, mx + t * 0.5f, my };
            gfx_polygon(s, up, 3, color_alpha(th->text, 0.45f));
        }
    }

    /* ---- System ---- */
    if (have_sys) {
        py = sys_top;
        font_draw(s, fh, (float)x + 6, py, "System", th->text);
        py += hh;
        read_file("/proc/sys/kernel/osrelease", line, sizeof(line));
        char buf[160];
        snprintf(buf, sizeof(buf), "  kernel %s", line);
        font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
        py += lh + 1.0f;
        read_file("/proc/uptime", line, sizeof(line));
        snprintf(buf, sizeof(buf), "  up %d s", atoi(line));
        font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
    }

    /* ---- status / hint ---- */
    if (have_status)
        font_draw(s, fd, (float)x + 6, status_y,
                  st->status[0] ? st->status
                                : "Up/Down + Enter, or click a row; digits pick in that list",
                  st->status[0] ? color_alpha(th->accent, 0.80f)
                                : color_alpha(th->text, 0.40f));
}

static void settings_key(App *a, const char *bytes, int n) {
    Settings *s = a->state;
    if (!s || !bytes) return;
    for (int i = 0; i < n; ) {
        unsigned char c = (unsigned char)bytes[i];

        /* arrows arrive as CSI sequences from src/main.c:key_bytes_for() */
        if (c == 0x1b && i + 2 < n && bytes[i + 1] == '[') {
            char f = bytes[i + 2];
            int adv = 3;
            if (f >= '0' && f <= '9' && i + 3 < n && bytes[i + 3] == '~') adv = 4;
            if (f == 'A') move_sel(s, -1);
            else if (f == 'B') move_sel(s, +1);
            i += adv;
            continue;
        }
        i++;

        if (c == '\r' || c == '\n') { activate(s); continue; }
        if (c == 'k') { move_sel(s, -1); continue; }
        if (c == 'j') { move_sel(s, +1); continue; }

        /* digits pick inside the list the selection is in - Text size takes
         * 1..5, Wallpaper 1..4, and the theme list keeps its old 1..9 meaning */
        if (c >= '1' && c <= '9') {
            int idx = c - '1';
            int sec = section_of(s->sel);
            if (sec == 0) {
                if (idx < s->count) { s->sel = ROW_THEME(idx); apply_theme(s, idx); }
            } else if (sec == 1) {
                if (idx < SIZE_COUNT) { s->sel = idx; apply_size(s, idx); }
            } else {
                if (idx < WALLPAPER_COUNT) {
                    s->sel = ROW_WALL(idx);
                    apply_wallpaper(s, idx);
                }
            }
        }
    }
}

static void settings_click(App *a, int x, int y, int buttons) {
    Settings *s = a->state;
    (void)x; (void)buttons;
    if (!s) return;
    int n = flat_count(s);
    if (n > MAX_ROWS) n = MAX_ROWS;
    for (int i = 0; i < n; i++) {
        float ry = s->row_y[i], yb = s->row_yb[i];
        if (ry < 0.0f || yb <= ry) continue;   /* not on screen, not hittable */
        /* exactly the rectangle the painter drew for this row, clipped with it
         * at the list's edges, so a click cannot land on a row it cannot see */
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

const AppVT app_vt_settings = {
    .id = "settings",
    .title = "Settings",
    .init = settings_init,
    .draw = settings_draw,
    .key = settings_key,
    .click = settings_click,
    .resize = settings_resize,
    .free = settings_free,
};
