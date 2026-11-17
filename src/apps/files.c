/* ============================================================
 *  Mystic OS - apps/files.c
 *  A plain directory list: the path, then one row per entry -
 *  directories first (trailing '/'), then files with their size on
 *  the right. j/k or Up/Down move, Enter opens, Backspace goes up.
 *  Clicking a row selects it; clicking the selected row opens it.
 *
 *  Row and header heights come from the font metrics at draw time,
 *  so the global text scale grows rows and bands together and they
 *  cannot overlap. The click handler divides by the same values.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 400
#define NAME_LEN    256
#define PATH_LEN    512
#define ROW_GAP  4.0f      /* breathing room added to the glyph box */
#define HEAD_GAP 8.0f

typedef struct { char name[NAME_LEN]; long size; unsigned char is_dir; } Entry;

typedef struct {
    char   path[PATH_LEN];
    char   error[128];           /* strerror(errno) when opendir failed */
    Entry *ent;
    int    count, sel, scroll;   /* scroll = first visible row */
    int    w, h;
} Files;

/* Geometry from the fonts actually used; font_get() applies the scale. */
static Font *list_font(void) { return font_get(FONT_REGULAR, 11); }
static Font *path_font(void) { return font_get(FONT_MEDIUM, 12); }

static int row_h(Font *fr) {
    int v = (int)(font_height(fr) + ROW_GAP);
    return v > 0 ? v : 1;
}
static int head_h(Font *fp) {
    int v = (int)(font_height(fp) + HEAD_GAP);
    return v > 0 ? v : 1;
}

/* directories first, then plain name order */
static int entry_before(const Entry *a, const Entry *b) {
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir;
    return strcmp(a->name, b->name) < 0;
}

static void sort_entries(Entry *e, int n) {
    for (int i = 1; i < n; i++) {
        Entry key = e[i];
        int j = i - 1;
        while (j >= 0 && entry_before(&key, &e[j])) { e[j + 1] = e[j]; j--; }
        e[j + 1] = key;
    }
}

/* stat() follows symlinks, so a link to a directory reports S_ISDIR and
 * opens like a real directory. */
static void load(Files *f) {
    f->count = f->sel = f->scroll = 0;
    f->error[0] = 0;
    DIR *d = opendir(f->path);
    if (!d) {
        snprintf(f->error, sizeof(f->error), "%s", strerror(errno));
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && f->count < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char full[PATH_LEN + NAME_LEN + 2];
        if (snprintf(full, sizeof(full), "%s/%s", f->path, de->d_name)
                >= (int)sizeof(full)) continue;   /* no truncated lookups */

        struct stat st;
        if (stat(full, &st) < 0) continue;        /* vanished: skip it */

        Entry *e = &f->ent[f->count];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        e->size   = (long)st.st_size;
        f->count++;
    }
    closedir(d);
    sort_entries(f->ent, f->count);
}

static void files_init(App *a, int w, int h) {
    Files *f = calloc(1, sizeof(Files));
    if (!f) return;
    f->ent = calloc(MAX_ENTRIES, sizeof(Entry));
    if (!f->ent) { free(f); return; }
    f->w = w;
    f->h = h;
    snprintf(f->path, sizeof(f->path), "/");
    load(f);
    a->state = f;
}

static void files_free(App *a) {
    Files *f = a->state;
    if (!f) return;
    a->state = NULL;
    free(f->ent);
    free(f);
}

static void human_size(long size, char *out, size_t n) {
    if (size < 1024) snprintf(out, n, "%ld B", size);
    else if (size < 1024 * 1024) snprintf(out, n, "%.0f K", size / 1024.0);
    else snprintf(out, n, "%.1f M", size / (1024.0 * 1024.0));
}

/* draw inside maxw pixels, ending in ".." when it has to be cut */
static void draw_clipped(Surface *s, Font *fr, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!text || !text[0] || maxw < 4.0f) return;
    char buf[PATH_LEN + 8];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 3) len = sizeof(buf) - 3;
    memcpy(buf, text, len);
    buf[len] = 0;
    if (font_width(fr, buf) <= maxw) { font_draw(s, fr, x, y, buf, color); return; }
    float budget = maxw - font_width(fr, "..");
    size_t cut = len;
    while (cut > 0 && font_width(fr, buf) > budget) buf[--cut] = 0;
    if (cut == 0) return;
    memcpy(buf + cut, "..", 3);
    font_draw(s, fr, x, y, buf, color);
}

static void files_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Files *f = a->state;
    if (!f) return;
    f->w = w; f->h = h;
    Font *fp = path_font();
    Font *fr = list_font();
    if (!fp || !fr || w < 16 || h < 4) return;
    const int hh = head_h(fp);
    const int rh = row_h(fr);

    /* header: the current path, with a rule under it */
    if (h >= hh) {
        float ty = (float)y + (float)(hh - (int)font_height(fp)) * 0.5f;
        if (ty < (float)y) ty = (float)y;
        draw_clipped(s, fp, (float)x + 6.0f, ty, (float)w - 12.0f, f->path, th->text);
        if (hh >= 2 && w > 12)
            surf_blend(s, x + 6, y + hh - 1, w - 12, 1, th->panel_border);
    }

    /* on failure: one line, the real strerror(errno), and nothing else */
    if (f->error[0]) {
        if (h >= hh + rh)
            draw_clipped(s, fr, (float)x + 6.0f, (float)y + (float)hh,
                         (float)w - 12.0f, f->error, th->accent);
        return;
    }

    int rows = (h - hh) / rh;               /* whole rows under the header */
    if (rows < 1) return;

    /* keep the selection on screen */
    if (f->sel < 0) f->sel = 0;
    if (f->sel >= f->count) f->sel = f->count > 0 ? f->count - 1 : 0;
    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + rows) f->scroll = f->sel - rows + 1;
    if (f->scroll > f->count - rows) f->scroll = f->count - rows;
    if (f->scroll < 0) f->scroll = 0;

    float text_off = (float)(rh - (int)font_height(fr)) * 0.5f;
    if (text_off < 0.0f) text_off = 0.0f;
    for (int i = 0; i < rows; i++) {
        int idx = f->scroll + i;
        if (idx >= f->count) break;
        Entry *e = &f->ent[idx];
        int sel = (idx == f->sel);
        float ry = (float)y + (float)hh + (float)i * (float)rh;
        if (sel)
            gfx_rrect_aa(s, (float)x + 2.0f, ry + 1.0f, (float)w - 4.0f,
                         (float)(rh - 2), 4.0f, color_alpha(th->accent, 0.20f), 0, 0);

        float right = (float)(x + w) - 6.0f;      /* size column, files only */
        if (!e->is_dir) {
            char sz[24];
            human_size(e->size, sz, sizeof(sz));
            float sw = font_width(fr, sz);
            if (right - sw > (float)x + 40.0f) {
                font_draw(s, fr, right - sw, ry + text_off, sz,
                          color_alpha(th->text, 0.55f));
                right -= sw + 8.0f;
            }
        }

        char name[NAME_LEN + 2];
        snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
        draw_clipped(s, fr, (float)x + 6.0f, ry + text_off, right - (float)x - 6.0f,
                     name, sel ? th->text : th->text_dim);
    }
}

static void activate(Files *f) {
    if (f->sel < 0 || f->sel >= f->count) return;
    Entry *e = &f->ent[f->sel];
    if (!e->is_dir) return;

    char next[PATH_LEN];
    int n = !strcmp(f->path, "/")
          ? snprintf(next, sizeof(next), "/%s", e->name)
          : snprintf(next, sizeof(next), "%s/%s", f->path, e->name);
    if (n < 0 || n >= (int)sizeof(next)) return;   /* no truncated path */
    snprintf(f->path, sizeof(f->path), "%s", next);
    load(f);
}

static void go_up(Files *f) {
    if (!strcmp(f->path, "/")) return;
    char *slash = strrchr(f->path, '/');
    if (!slash || slash == f->path) snprintf(f->path, sizeof(f->path), "/");
    else *slash = 0;
    load(f);
}

static void files_key(App *a, const char *bytes, int n) {
    Files *f = a->state;
    if (!f) return;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == 0x1B) {                          /* arrow key escape run */
            if (i + 2 < n && bytes[i + 1] == '[') {
                if (bytes[i + 2] == 'A' && f->sel > 0) f->sel--;
                if (bytes[i + 2] == 'B' && f->sel + 1 < f->count) f->sel++;
                i += 2;
            }
            continue;
        }
        switch (c) {
            case 'j': if (f->sel + 1 < f->count) f->sel++; break;
            case 'k': if (f->sel > 0) f->sel--; break;
            case '\n':
            case '\r': activate(f); break;
            case '\b':
            case 0x7F: go_up(f); break;
            default: break;
        }
    }
}

static void files_click(App *a, int x, int y, int buttons) {
    Files *f = a->state;
    (void)x; (void)buttons;
    if (!f) return;

    Font *fp = path_font();
    Font *fr = list_font();
    if (!fp || !fr) return;
    const int hh = head_h(fp);
    const int rh = row_h(fr);
    if (y < hh) return;                           /* the header owns no rows */

    int rows = (f->h - hh) / rh;
    if (rows < 1) return;
    int row = (y - hh) / rh;                      /* y >= hh: no rounding trap */
    if (row >= rows) return;
    int idx = f->scroll + row;
    if (idx < 0 || idx >= f->count) return;

    if (idx == f->sel) activate(f);
    else f->sel = idx;
}

static void files_resize(App *a, int w, int h) {
    Files *f = a->state;
    if (f) { f->w = w; f->h = h; }
}

const AppVT app_vt_files = {
    .id = "files", .title = "Files",
    .init = files_init, .draw = files_draw, .key = files_key,
    .click = files_click, .resize = files_resize, .free = files_free,
};
