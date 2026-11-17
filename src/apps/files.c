/* ============================================================
 *  Mystic OS - apps/files.c
 *  A file browser. Every row is a real entry from readdir();
 *  every size is a real st_size from stat().
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ENTRIES 400
#define ROW_H 15.0f

typedef struct {
    char name[64];
    int  is_dir;
    long size;
} Entry;

typedef struct {
    char    path[256];
    char    error[128];
    Entry  *ent;
    int     count;
    int     sel;
    int     scroll;
    int     w, h;
} Files;

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;   /* dirs first */
    return strcmp(x->name, y->name);
}

static void load(Files *f) {
    f->count = 0;
    f->sel = 0;
    f->scroll = 0;
    f->error[0] = 0;

    DIR *d = opendir(f->path);
    if (!d) {
        snprintf(f->error, sizeof(f->error), "%s: %s", f->path, strerror(errno));
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && f->count < MAX_ENTRIES) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", f->path, e->d_name);
        struct stat st;
        if (stat(full, &st) < 0) continue;

        Entry *ent = &f->ent[f->count];
        snprintf(ent->name, sizeof(ent->name), "%s", e->d_name);
        ent->is_dir = S_ISDIR(st.st_mode);
        ent->size = (long)st.st_size;
        f->count++;
    }
    closedir(d);
    qsort(f->ent, (size_t)f->count, sizeof(Entry), cmp_entry);
}

static void files_init(App *a, int w, int h) {
    Files *f = calloc(1, sizeof(Files));
    if (!f) return;
    f->ent = calloc(MAX_ENTRIES, sizeof(Entry));
    if (!f->ent) { free(f); return; }
    f->w = w; f->h = h;
    snprintf(f->path, sizeof(f->path), "/");
    load(f);
    a->state = f;
}

static void files_free(App *a) {
    Files *f = a->state;
    if (!f) return;
    free(f->ent);
    free(f);
    a->state = NULL;
}

static void human_size(long size, char *out, size_t n) {
    if (size < 1024)           snprintf(out, n, "%ld B", size);
    else if (size < 1024 * 1024) snprintf(out, n, "%.0f K", size / 1024.0);
    else                        snprintf(out, n, "%.1f M", size / (1024.0 * 1024.0));
}

static void files_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Files *f = a->state;
    if (!f) return;
    f->w = w; f->h = h;

    Font *fp = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    if (!fp || !fr) return;

    /* path */
    font_draw(s, fp, (float)x + 4, (float)y + 4, f->path, th->text);
    surf_blend(s, x + 4, y + 22, w - 8, 1, th->panel_border);

    if (f->error[0]) {
        font_draw(s, fr, (float)x + 4, (float)y + 30, f->error, th->accent);
        return;
    }

    int rows = (int)((h - 30) / ROW_H);
    if (rows < 1) rows = 1;

    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + rows) f->scroll = f->sel - rows + 1;
    if (f->scroll < 0) f->scroll = 0;

    for (int i = 0; i < rows; i++) {
        int idx = f->scroll + i;
        if (idx >= f->count) break;
        Entry *e = &f->ent[idx];
        float ry = (float)y + 30 + i * ROW_H;

        if (idx == f->sel) {
            gfx_rrect_aa(s, (float)x + 2, ry - 1, (float)w - 4, ROW_H, 4.0f,
                         color_alpha(th->accent, 0.20f), 0, 0);
        }

        char name[80];
        if (e->is_dir) snprintf(name, sizeof(name), "%s/", e->name);
        else           snprintf(name, sizeof(name), "%s", e->name);

        font_draw(s, fr, (float)x + 6, ry, name, idx == f->sel ? th->text : th->text_dim);

        if (!e->is_dir) {
            char sz[24];
            human_size(e->size, sz, sizeof(sz));
            float sw = font_width(fr, sz);
            font_draw(s, fr, (float)(x + w) - sw - 8, ry, sz, color_alpha(th->text, 0.55f));
        }
    }

    /* footer */
    char foot[64];
    snprintf(foot, sizeof(foot), "%d entries%s", f->count,
             f->count >= MAX_ENTRIES ? " (truncated)" : "");
    font_draw(s, fr, (float)x + 4, (float)(y + h) - 16, foot, color_alpha(th->text, 0.45f));
}

static void activate(Files *f) {
    if (f->sel < 0 || f->sel >= f->count) return;
    if (!f->ent[f->sel].is_dir) return;
    char next[256];
    if (!strcmp(f->path, "/")) snprintf(next, sizeof(next), "/%s", f->ent[f->sel].name);
    else snprintf(next, sizeof(next), "%s/%s", f->path, f->ent[f->sel].name);
    snprintf(f->path, sizeof(f->path), "%s", next);
    load(f);
}

static void go_up(Files *f) {
    if (!strcmp(f->path, "/")) return;
    char *slash = strrchr(f->path, '/');
    if (!slash || slash == f->path) snprintf(f->path, sizeof(f->path), "/");
    else { *slash = 0; }
    load(f);
}

static void files_key(App *a, const char *bytes, int n) {
    Files *f = a->state;
    if (!f) return;
    for (int i = 0; i < n; i++) {
        char c = bytes[i];
        if (c == 'j') { if (f->sel + 1 < f->count) f->sel++; }
        else if (c == 'k') { if (f->sel > 0) f->sel--; }
        else if (c == '\n') activate(f);
        else if (c == '\b') go_up(f);
        else if (c == 0x1B && i + 2 < n && bytes[i + 1] == '[') {
            if (bytes[i + 2] == 'B') { if (f->sel + 1 < f->count) f->sel++; }
            if (bytes[i + 2] == 'A') { if (f->sel > 0) f->sel--; }
            i += 2;
        }
    }
}

static void files_click(App *a, int x, int y, int buttons) {
    Files *f = a->state;
    (void)x; (void)buttons;
    if (!f) return;
    int row = (int)((y - 30) / ROW_H);
    if (row < 0) return;
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
    .id = "files",
    .title = "Files",
    .init = files_init,
    .draw = files_draw,
    .key = files_key,
    .click = files_click,
    .resize = files_resize,
    .free = files_free,
};
