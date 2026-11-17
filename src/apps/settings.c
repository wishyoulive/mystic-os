/* ============================================================
 *  Mystic OS - apps/settings.c
 *  Real settings: shows real hardware facts and lists the real
 *  theme files. Choosing a theme writes /etc/mystic/current-theme,
 *  which the desktop watches - that is how the change takes effect.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THEMES 12

/* ------------------------------------------------------------
 * Every vertical metric below is derived from the font that draws in
 * it, at draw time. font_get() applies the global text scale (see
 * src/font.c:125), so the theme rows and the section strides grow and
 * shrink with the glyphs instead of overlapping them. There is no
 * colour, key or behaviour change here - only the geometry that has to
 * follow the text. settings_click() calls row_h_of() with the same
 * font the painter uses, so a click lands on the row it hit.
 * ------------------------------------------------------------ */
typedef struct {
    char  file[64];
    char  name[64];
    float y;              /* where its row was drawn, for hit testing */
} ThemeRow;

typedef struct {
    ThemeRow rows[MAX_THEMES];
    int      count;
    int      sel;
    int      w, h;
    int      scroll;
    char     status[64];  /* real result of the last apply_theme() */
} Settings;

/* One list row: the row's own font line box plus 4 px of leading. */
static float row_h_of(Font *fr) { return font_height(fr) + 4.0f; }

/* One ordinary text line: line box plus 2 px. */
static float line_h_of(Font *f) { return font_height(f) + 2.0f; }

/* One section heading, including the gap under it. */
static float head_h_of(Font *fh) { return font_height(fh) + 4.0f; }

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

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const ThemeRow *)a)->file, ((const ThemeRow *)b)->file);
}

static void scan_themes(Settings *s) {
    s->count = 0;
    DIR *d = opendir("/etc/mystic/themes");
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
        r->y = -1.0f;                 /* not on screen until draw lays it out */

        char path[320], line[256];
        snprintf(path, sizeof(path), "/etc/mystic/themes/%s", e->d_name);
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

static void settings_init(App *a, int w, int h) {
    Settings *s = calloc(1, sizeof(Settings));
    if (!s) return;
    s->w = w;
    s->h = h;
    scan_themes(s);
    a->state = s;
}

static void settings_free(App *a) {
    free(a->state);
    a->state = NULL;
}

/* The desktop re-reads /etc/mystic/current-theme once a second and matches
 * the first line against Theme::name - the "name =" field inside the theme
 * file (main.c:1178-1186, docs/ARCHITECTURE.md 3.6), not its file name.
 * This used to write ThemeRow::file ("1-obsidian.theme"), which matched no
 * theme, so choosing one did nothing. Write the name, and report the real
 * result: fopen/fprintf/fflush/fclose were all unchecked before. */
static void apply_theme(Settings *s) {
    if (s->sel < 0 || s->sel >= s->count) return;
    const ThemeRow *r = &s->rows[s->sel];
    FILE *f = fopen("/etc/mystic/current-theme", "w");
    if (!f) {
        snprintf(s->status, sizeof(s->status), "not saved: %s", strerror(errno));
        return;
    }
    errno = 0;
    int ok = fprintf(f, "%s\n", r->name) > 0;
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok)
        snprintf(s->status, sizeof(s->status), "theme: %s", r->name);
    else
        snprintf(s->status, sizeof(s->status), "not saved: %s",
                 strerror(errno ? errno : EIO));
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
    if (!fh || !fr) return;

    const float rh = row_h_of(fr);          /* the one row height: draw and click */

    float py = (float)y + 6;
    char line[256];

    font_draw(s, fh, (float)x + 6, py, "Display", th->text);
    py += head_h_of(fh);
    read_file("/sys/class/graphics/fb0/virtual_size", line, sizeof(line));
    char pix[96];
    snprintf(pix, sizeof(pix), "  %s pixels", line);
    font_draw(s, fr, (float)x + 6, py, pix, th->text_dim);
    py += line_h_of(fr) + 4.0f;

    font_draw(s, fh, (float)x + 6, py, "Memory", th->text);
    py += head_h_of(fh);
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
    py += line_h_of(fr) + 8.0f;

    font_draw(s, fh, (float)x + 6, py, "Theme", th->text);
    py += head_h_of(fh);

    /* The list scrolls inside the height that is really left. The System
     * block is pinned to the bottom and drawn only when a whole theme row
     * still fits above it; the old code drew every row, then System, then
     * the hint at fixed offsets, so with more themes than fit (or a window
     * dragged to its 280x160 minimum) they were painted over the frame and
     * the desktop. SYS_H is now the sum of the real font lines it holds:
     * gap 8 + title + kernel + uptime + hint, each its own font's height. */
    const float SYS_H = 8.0f + head_h_of(fh) + (line_h_of(fr) + 1.0f) * 2.0f
                        + line_h_of(fd);
    float theme_bottom = (float)(y + h) - 6.0f;
    int have_sys = (theme_bottom - py) >= (SYS_H + rh);
    if (have_sys) theme_bottom -= SYS_H;

    int vis = (int)((theme_bottom - py) / rh);
    if (vis < 0) vis = 0;
    if (vis > st->count) vis = st->count;

    /* keep the selected row inside the visible window */
    if (st->count > 0) {
        if (st->sel < 0) st->sel = 0;
        if (st->sel >= st->count) st->sel = st->count - 1;
        if (st->sel < st->scroll) st->scroll = st->sel;
        if (st->sel >= st->scroll + vis) st->scroll = st->sel - vis + 1;
        if (st->scroll > st->count - vis) st->scroll = st->count - vis;
        if (st->scroll < 0) st->scroll = 0;
    } else {
        st->scroll = 0;
    }

    /* Rows that are not on screen are not hittable: clicks arrive in
     * app-local coordinates and y < 0 cannot be one. */
    for (int i = 0; i < st->count; i++) st->rows[i].y = -1.0f;
    for (int i = 0; i < vis; i++) {
        int idx = st->scroll + i;
        if (idx >= st->count) break;
        ThemeRow *r = &st->rows[idx];
        r->y = py - (float)y;         /* local, because that is what clicks get */
        int selected = (idx == st->sel);
        if (selected)
            gfx_rrect_aa(s, (float)x + 4, py - 2, (float)w - 8, rh, 4.0f,
                         color_alpha(th->accent, 0.20f), 0, 0);
        snprintf(line, sizeof(line), "%d   %s", idx + 1, r->name);
        font_draw(s, fr, (float)x + 10, py, line, selected ? th->text : th->text_dim);
        py += rh;
    }

    if (have_sys) {
        py += 8;
        font_draw(s, fh, (float)x + 6, py, "System", th->text);
        py += head_h_of(fh);
        read_file("/proc/sys/kernel/osrelease", line, sizeof(line));
        char buf[160];
        snprintf(buf, sizeof(buf), "  kernel %s", line);
        font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
        py += line_h_of(fr) + 1.0f;
        read_file("/proc/uptime", line, sizeof(line));
        snprintf(buf, sizeof(buf), "  up %d s", atoi(line));
        font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
        py += line_h_of(fr) + 1.0f;
    }
    if (py + line_h_of(fd) <= (float)(y + h))
        font_draw(s, fd, (float)x + 6, py,
                  st->status[0] ? st->status : "press 1-9 or click to switch theme",
                  st->status[0] ? color_alpha(th->accent, 0.80f)
                                : color_alpha(th->text, 0.40f));
}

static void settings_key(App *a, const char *bytes, int n) {
    Settings *s = a->state;
    if (!s) return;
    for (int i = 0; i < n; i++) {
        char c = bytes[i];
        if (c >= '1' && c <= '9') {
            int idx = c - '1';
            if (idx < s->count) { s->sel = idx; apply_theme(s); }
        } else if (c == 'j') {
            if (s->sel + 1 < s->count) { s->sel++; apply_theme(s); }
        } else if (c == 'k') {
            if (s->sel > 0) { s->sel--; apply_theme(s); }
        }
    }
}

static void settings_click(App *a, int x, int y, int buttons) {
    Settings *s = a->state;
    (void)x; (void)buttons;
    if (!s) return;
    /* the same font, hence the same row height, the painter used */
    Font *fr = font_get(FONT_REGULAR, 11);
    if (!fr) return;
    float rh = row_h_of(fr);
    for (int i = 0; i < s->count; i++) {
        float ry = s->rows[i].y;
        if (ry < 0.0f) continue;      /* scrolled out of view: not hittable */
        if ((float)y >= ry - 2.0f && (float)y < ry + rh) {
            s->sel = i;
            apply_theme(s);
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
