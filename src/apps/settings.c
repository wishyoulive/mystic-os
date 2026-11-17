/* ============================================================
 *  Mystic OS - apps/settings.c
 *  Real settings: shows real hardware facts and lists the real
 *  theme files. Choosing a theme writes /etc/mystic/current-theme,
 *  which the desktop watches - that is how the change takes effect.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THEMES 12
#define ROW_H 18.0f

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
} Settings;

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

static long mem_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char k[64], unit[16];
    long v;
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3)
        if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return 0;
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

        ThemeRow *r = &s->rows[s->count];
        snprintf(r->file, sizeof(r->file), "%s", e->d_name);
        snprintf(r->name, sizeof(r->name), "%s", e->d_name);   /* fallback */

        char path[256], line[256];
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

static void apply_theme(Settings *s) {
    if (s->sel < 0 || s->sel >= s->count) return;
    FILE *f = fopen("/etc/mystic/current-theme", "w");
    if (!f) return;
    fprintf(f, "%s\n", s->rows[s->sel].file);
    fclose(f);
}

static void settings_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Settings *st = a->state;
    if (!st) return;
    st->w = w;
    st->h = h;

    Font *fh = font_get(FONT_MEDIUM, 13);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fh || !fr) return;

    float py = (float)y + 6;
    char line[256];

    font_draw(s, fh, (float)x + 6, py, "Display", th->text);
    py += 20;
    read_file("/sys/class/graphics/fb0/virtual_size", line, sizeof(line));
    char pix[96];
    snprintf(pix, sizeof(pix), "  %s pixels", line);
    font_draw(s, fr, (float)x + 6, py, pix, th->text_dim);
    py += 22;

    font_draw(s, fh, (float)x + 6, py, "Memory", th->text);
    py += 20;
    long total = mem_kb("MemTotal:"), avail = mem_kb("MemAvailable:");
    long used = total - avail;
    if (used < 0) used = 0;
    float frac = total > 0 ? (float)used / (float)total : 0;
    gfx_rrect_aa(s, (float)x + 6, py, (float)w - 12, 7, 3.5f, color_alpha(th->text, 0.10f), 0, 0);
    if (frac > 0.01f)
        gfx_rrect_aa(s, (float)x + 6, py, ((float)w - 12) * frac, 7, 3.5f, th->accent, 0, 0);
    py += 12;
    snprintf(line, sizeof(line), "  %ld MB used of %ld MB", used / 1024, total / 1024);
    font_draw(s, fr, (float)x + 6, py, line, th->text_dim);
    py += 24;

    font_draw(s, fh, (float)x + 6, py, "Theme", th->text);
    py += 20;

    for (int i = 0; i < st->count; i++) {
        ThemeRow *r = &st->rows[i];
        r->y = py;
        int selected = (i == st->sel);
        if (selected)
            gfx_rrect_aa(s, (float)x + 4, py - 2, (float)w - 8, ROW_H, 4.0f,
                         color_alpha(th->accent, 0.20f), 0, 0);
        snprintf(line, sizeof(line), "%d   %s", i + 1, r->name);
        font_draw(s, fr, (float)x + 10, py, line, selected ? th->text : th->text_dim);
        py += ROW_H;
    }

    py += 8;
    font_draw(s, fh, (float)x + 6, py, "System", th->text);
    py += 20;
    read_file("/proc/sys/kernel/osrelease", line, sizeof(line));
    char buf[160];
    snprintf(buf, sizeof(buf), "  kernel %s", line);
    font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
    py += 18;
    read_file("/proc/uptime", line, sizeof(line));
    snprintf(buf, sizeof(buf), "  up %d s", atoi(line));
    font_draw(s, fr, (float)x + 6, py, buf, th->text_dim);
    py += 18;
    font_draw(s, fd, (float)x + 6, py, "  press 1-9 or click to switch theme",
              color_alpha(th->text, 0.40f));
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
    for (int i = 0; i < s->count; i++) {
        if ((float)y >= s->rows[i].y - 2 && (float)y < s->rows[i].y + ROW_H) {
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
