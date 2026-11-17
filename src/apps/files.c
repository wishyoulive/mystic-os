/* ============================================================
 *  Mystic OS - apps/files.c
 *  A file browser. Every row is a real entry from readdir();
 *  sizes and times come from lstat()/stat(), link targets from
 *  readlink(), and the footer's free space from statvfs().
 *
 *  Keys:  j/k or Up/Down  move        Enter  descend
 *         Home/End         first/last  Backspace  go up
 *         s                cycle sort (name -> size -> date)
 *         .                show/hide dotfiles
 *         g/G, Ctrl+A/E    first/last when only text reaches us
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 400
#define ROW_H 15.0f
#define NAME_LEN 256
#define LINK_LEN 160

/* header band, footer band - rows live between them */
#define HEAD_H 30
#define FOOT_H 18

enum { SORT_NAME = 0, SORT_SIZE, SORT_MTIME, SORT_COUNT };

typedef struct {
    char          name[NAME_LEN];
    char          target[LINK_LEN];  /* readlink() result, empty if not a link */
    long          size;              /* st_size, or the target's size */
    long          mtime;             /* st_mtime, epoch seconds */
    unsigned char is_dir;
    unsigned char is_link;
} Entry;

typedef struct {
    char   path[256];
    char   error[128];
    Entry *ent;
    int    count;
    int    sel;
    int    scroll;
    int    sort;      /* SORT_* */
    int    hidden;    /* draw dotfiles */
    int    w, h;
} Files;

/* ------------------------------------------------------------
 * Ordering: directories first in every mode (so the browser can
 * always descend), then the sort key, then the name as tiebreak.
 * ------------------------------------------------------------ */
static int entry_before(const Entry *a, const Entry *b, int mode) {
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir;
    if (mode == SORT_SIZE && a->size != b->size) return a->size > b->size;
    if (mode == SORT_MTIME && a->mtime != b->mtime) return a->mtime > b->mtime;
    return strcmp(a->name, b->name) < 0;
}

static void sort_entries(Entry *e, int n, int mode) {
    for (int i = 1; i < n; i++) {
        Entry key = e[i];
        int j = i - 1;
        while (j >= 0 && entry_before(&key, &e[j], mode)) { e[j + 1] = e[j]; j--; }
        e[j + 1] = key;
    }
}

static int index_of(Files *f, const char *name) {
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->ent[i].name, name)) return i;
    return -1;
}

/* Keep the same entry selected across a reload or a re-sort. */
static void restore_selection(Files *f, const char *keep) {
    if (keep && keep[0]) {
        int i = index_of(f, keep);
        if (i >= 0) { f->sel = i; return; }
    }
    if (f->sel < 0) f->sel = 0;
    if (f->sel >= f->count) f->sel = f->count > 0 ? f->count - 1 : 0;
}

/* ------------------------------------------------------------
 * Directory scan: one lstat() per entry, a readlink() for links.
 * ------------------------------------------------------------ */
static void load(Files *f) {
    char keep[NAME_LEN];
    keep[0] = 0;
    if (f->sel >= 0 && f->sel < f->count)
        snprintf(keep, sizeof(keep), "%s", f->ent[f->sel].name);

    f->count = 0;
    f->error[0] = 0;

    DIR *d = opendir(f->path);
    if (!d) {
        snprintf(f->error, sizeof(f->error), "%s: %s", f->path, strerror(errno));
        f->sel = 0;
        f->scroll = 0;
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && f->count < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && !f->hidden) continue;

        char full[600];
        snprintf(full, sizeof(full), "%s/%s", f->path, de->d_name);

        struct stat lst;
        if (lstat(full, &lst) < 0) continue;   /* entry vanished: skip it */

        Entry *ent = &f->ent[f->count];
        memset(ent, 0, sizeof(*ent));
        snprintf(ent->name, sizeof(ent->name), "%s", de->d_name);
        ent->is_link = S_ISLNK(lst.st_mode) ? 1 : 0;
        ent->is_dir  = S_ISDIR(lst.st_mode) ? 1 : 0;
        ent->size    = (long)lst.st_size;
        ent->mtime   = (long)lst.st_mtime;

        if (ent->is_link) {
            ssize_t tl = readlink(full, ent->target, sizeof(ent->target) - 1);
            ent->target[tl > 0 ? (size_t)tl : 0] = 0;
            /* A live link reports its target's type, size and time,
             * so you can still descend through a link to a directory. */
            struct stat st;
            if (stat(full, &st) == 0) {
                ent->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
                ent->size   = (long)st.st_size;
                ent->mtime  = (long)st.st_mtime;
            }
        }
        f->count++;
    }
    closedir(d);

    sort_entries(f->ent, f->count, f->sort);
    restore_selection(f, keep);
}

static void files_init(App *a, int w, int h) {
    Files *f = calloc(1, sizeof(Files));
    if (!f) return;
    f->ent = calloc(MAX_ENTRIES, sizeof(Entry));
    if (!f->ent) { free(f); return; }
    f->w = w; f->h = h;
    f->sort = SORT_NAME;
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

/* ------------------------------------------------------------
 * Small formatters
 * ------------------------------------------------------------ */
static void human_size(long size, char *out, size_t n) {
    if (size < 1024)             snprintf(out, n, "%ld B", size);
    else if (size < 1024 * 1024) snprintf(out, n, "%.0f K", size / 1024.0);
    else                         snprintf(out, n, "%.1f M", size / (1024.0 * 1024.0));
}

static void fmt_mtime(long t, char *out, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tt, &tm)) { snprintf(out, n, "-- --"); return; }
    snprintf(out, n, "%02d-%02d %02d:%02d",
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* Free space on the filesystem that holds /root, in whole MB. */
static void free_space(char *out, size_t n) {
    struct statvfs vfs;
    if (statvfs("/root", &vfs) < 0 && statvfs("/", &vfs) < 0) {
        snprintf(out, n, "free: ?");
        return;
    }
    unsigned long long unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    unsigned long long bytes = (unsigned long long)vfs.f_bavail * unit;
    snprintf(out, n, "%llu MB free", bytes / (1024ull * 1024ull));
}

static const char *sort_label(int mode) {
    switch (mode) {
        case SORT_SIZE:  return "sorted by size";
        case SORT_MTIME: return "sorted by date";
        default:         return "sorted by name";
    }
}

/* Draw text inside maxw pixels, ending in ".." when it has to be cut. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!text || !text[0] || maxw < 4.0f) return;

    char buf[320];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 4) len = sizeof(buf) - 4;
    memcpy(buf, text, len);
    buf[len] = 0;

    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }

    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0) {
        buf[cut] = 0;
        if (font_width(f, buf) <= budget) break;
        cut--;
    }
    if (cut == 0) return;
    buf[cut] = '.';
    buf[cut + 1] = '.';
    buf[cut + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* How many rows fit between the header rule and the footer. */
static int row_count(const Files *f) {
    if (f->h < HEAD_H + FOOT_H + (int)ROW_H) return 0;
    return (f->h - HEAD_H - FOOT_H) / (int)ROW_H;
}

/* ------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------ */
static void files_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Files *f = a->state;
    if (!f) return;
    f->w = w; f->h = h;

    Font *fp = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    if (!fp || !fr) return;
    if (w < 24 || h < HEAD_H) return;

    /* header: path on the left, current sort order on the right */
    const char *label = sort_label(f->sort);
    float lw = font_width(fr, label);
    float path_w = (float)w - 8.0f;
    if (w >= 160) {
        font_draw(s, fr, (float)(x + w) - lw - 4.0f, (float)y + 5.0f, label,
                  color_alpha(th->text, 0.45f));
        path_w = (float)w - lw - 12.0f;
    }
    draw_clipped(s, fp, (float)x + 4.0f, (float)y + 4.0f, path_w, f->path, th->text);
    surf_blend(s, x + 4, y + 22, w - 8, 1, th->panel_border);

    int rows = row_count(f);

    if (f->error[0]) {
        if (rows > 0)
            draw_clipped(s, fr, (float)x + 4.0f, (float)y + (float)HEAD_H,
                         (float)w - 8.0f, f->error, th->accent);
    } else {
        /* keep the selection visible */
        if (rows > 0) {
            if (f->sel < f->scroll) f->scroll = f->sel;
            if (f->sel >= f->scroll + rows) f->scroll = f->sel - rows + 1;
            int max_scroll = f->count - rows;
            if (max_scroll < 0) max_scroll = 0;
            if (f->scroll > max_scroll) f->scroll = max_scroll;
            if (f->scroll < 0) f->scroll = 0;
        }

        for (int i = 0; i < rows; i++) {
            int idx = f->scroll + i;
            if (idx >= f->count) break;
            Entry *e = &f->ent[idx];
            int sel = (idx == f->sel);
            float ry = (float)y + (float)HEAD_H + i * ROW_H;

            if (sel)
                gfx_rrect_aa(s, (float)x + 2.0f, ry - 1.0f, (float)w - 4.0f, ROW_H,
                             4.0f, color_alpha(th->accent, 0.20f), 0, 0);

            /* right hand columns, dropped when the window gets narrow */
            float right = (float)(x + w) - 8.0f;
            if (!e->is_dir) {
                char sz[24];
                human_size(e->size, sz, sizeof(sz));
                float sw = font_width(fr, sz);
                font_draw(s, fr, right - sw, ry, sz, color_alpha(th->text, 0.55f));
                right -= sw + 8.0f;
            }
            if (w >= 280) {
                char dt[24];
                fmt_mtime(e->mtime, dt, sizeof(dt));
                float dw = font_width(fr, dt);
                if (right - dw > (float)x + 90.0f) {
                    font_draw(s, fr, right - dw, ry, dt, color_alpha(th->text, 0.40f));
                    right -= dw + 8.0f;
                }
            }

            /* name, with '@' for a symlink and '/' for a directory */
            char name[NAME_LEN + 4];
            snprintf(name, sizeof(name), "%s%s%s", e->name,
                     e->is_link ? "@" : "", e->is_dir ? "/" : "");
            float name_x = (float)x + 6.0f;
            float room = right - name_x;
            draw_clipped(s, fr, name_x, ry, room, name,
                         sel ? th->text : th->text_dim);

            /* the readlink() target, dimmed, when there is room for it */
            if (e->is_link && e->target[0]) {
                float nw = font_width(fr, name);
                if (nw + 20.0f < room) {
                    char tgt[LINK_LEN + 8];
                    snprintf(tgt, sizeof(tgt), " -> %s", e->target);
                    draw_clipped(s, fr, name_x + nw, ry, room - nw, tgt,
                                 color_alpha(th->text, 0.40f));
                }
            }
        }
    }

    /* footer: entry count on the left, free space on the right */
    if (h < HEAD_H + FOOT_H) return;
    char freebuf[48];
    free_space(freebuf, sizeof(freebuf));
    float fw = font_width(fr, freebuf);
    float fy = (float)(y + h) - 16.0f;

    if (fw + 12.0f < (float)w) {
        font_draw(s, fr, (float)(x + w) - fw - 6.0f, fy, freebuf,
                  color_alpha(th->text, 0.45f));
        char foot[80];
        snprintf(foot, sizeof(foot), "%d entr%s%s%s", f->count,
                 f->count == 1 ? "y" : "ies",
                 f->hidden ? ", dotfiles" : "",
                 f->count >= MAX_ENTRIES ? " (max)" : "");
        draw_clipped(s, fr, (float)x + 4.0f, fy, (float)w - fw - 16.0f, foot,
                     color_alpha(th->text, 0.45f));
    } else {
        draw_clipped(s, fr, (float)x + 4.0f, fy, (float)w - 8.0f, freebuf,
                     color_alpha(th->text, 0.45f));
    }
}

/* ------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------ */
static void activate(Files *f) {
    if (f->sel < 0 || f->sel >= f->count) return;
    Entry *e = &f->ent[f->sel];
    if (!e->is_dir) return;

    char next[256];
    int n = !strcmp(f->path, "/")
          ? snprintf(next, sizeof(next), "/%s", e->name)
          : snprintf(next, sizeof(next), "%s/%s", f->path, e->name);
    if (n < 0 || n >= (int)sizeof(next)) {
        snprintf(f->error, sizeof(f->error), "%s: path too long", f->path);
        return;
    }
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

static void cycle_sort(Files *f) {
    char keep[NAME_LEN];
    keep[0] = 0;
    if (f->sel >= 0 && f->sel < f->count)
        snprintf(keep, sizeof(keep), "%s", f->ent[f->sel].name);

    f->sort = (f->sort + 1) % SORT_COUNT;
    sort_entries(f->ent, f->count, f->sort);
    restore_selection(f, keep);
}

static void files_key(App *a, const char *bytes, int n) {
    Files *f = a->state;
    if (!f) return;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B) {                       /* arrow / Home / End runs */
            if (i + 2 < n && bytes[i + 1] == '[') {
                switch (bytes[i + 2]) {
                    case 'A': if (f->sel > 0) f->sel--; break;
                    case 'B': if (f->sel + 1 < f->count) f->sel++; break;
                    case 'H': f->sel = 0; break;
                    case 'F': f->sel = f->count > 0 ? f->count - 1 : 0; break;
                    default: break;
                }
                i += 2;
            }
            continue;
        }

        switch (c) {
            case 'j': if (f->sel + 1 < f->count) f->sel++; break;
            case 'k': if (f->sel > 0) f->sel--; break;
            case 'g': f->sel = 0; break;                       /* Home alias */
            case 'G': f->sel = f->count > 0 ? f->count - 1 : 0; break;
            case 0x01: f->sel = 0; break;                      /* Ctrl+A */
            case 0x05: f->sel = f->count > 0 ? f->count - 1 : 0; break;  /* Ctrl+E */
            case 's': cycle_sort(f); break;
            case '.': f->hidden = !f->hidden; load(f); break;
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

    int rows = row_count(f);
    if (rows < 1) return;

    int row = (int)(((float)y - (float)HEAD_H) / ROW_H);
    if (row < 0 || row >= rows) return;
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
