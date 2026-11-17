/* ============================================================
 *  Mystic OS - apps (external) / files
 *  A file browser that is an ordinary process: it connects to
 *  the window server, owns a shared pixel buffer and draws into
 *  it. Nothing here is compiled into the desktop.
 *
 *  Every row is a real entry from readdir() with its type and
 *  size from lstat(). No synthetic rows, no padding.
 *
 *  Keys:  j / Down      next          k / Up      previous
 *         Enter         descend       Backspace   go up
 *         Home / End    first / last  PgUp/PgDn   page
 *         q             quit
 *  Mouse: click a row to select it, click the selected row again
 *         to descend into it.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 300          /* the listing cap; the platform is small */
#define NAME_LEN    256
#define PATH_LEN    512
#define ERR_LEN     200

/* bands, in window-local pixels */
#define HEAD_H 30
#define FOOT_H 20
#define ROW_H  16

/* Palette. A client app gets no Theme: it owns its pixels, so it
 * carries its own colours, chosen to sit next to the desktop. */
#define BG_TOP   RGB(0x0E, 0x15, 0x24)
#define BG_BOT   RGB(0x17, 0x21, 0x35)
#define C_TEXT   RGB(0xE8, 0xEC, 0xF6)
#define C_DIR    RGB(0x7A, 0xA2, 0xF7)
#define C_ERROR  RGB(0xE5, 0x7A, 0x7A)
#define C_ACCENT RGB(0x4F, 0xD1, 0xC5)

typedef struct {
    char          name[NAME_LEN];
    long          size;      /* st_size straight from lstat() */
    unsigned char is_dir;
} Entry;

typedef struct {
    char    path[PATH_LEN];
    Entry   ent[MAX_ENTRIES];   /* fixed: the cap above, never grown */
    int     count;
    int     sel;                /* index into ent[], -1 when empty */
    int     top;                /* first visible row */
    int     w, h;               /* current drawable size */
    int     maxw, maxh;         /* size of the shared mapping we were given */
    int     error;              /* 1 when error_text holds a real errno message */
    char    error_text[ERR_LEN];
    int     last_buttons;       /* previous MOUSE button mask */
} Browser;

/* ------------------------------------------------------------ text */

/* Draw text, truncating with ".." when it cannot fit in maxw pixels. */
static void draw_fit(Surface *s, Font *f, float x, float y, float maxw,
                     const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 4.0f) return;

    char buf[NAME_LEN + 4];
    snprintf(buf, sizeof(buf), "%s", text);
    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }

    float dots = font_width(f, "..");
    size_t len = strlen(buf);
    while (len > 0) {
        buf[len] = 0;
        if (font_width(f, buf) + dots <= maxw) break;
        len--;
    }
    if (len == 0) return;
    buf[len] = '.';
    buf[len + 1] = '.';
    buf[len + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* B / K / M, from the byte count lstat() reported. */
static void human_size(long bytes, char *out, size_t n) {
    if (bytes < 1024)
        snprintf(out, n, "%ld B", bytes);
    else if (bytes < 1024L * 1024L)
        snprintf(out, n, "%.1f K", bytes / 1024.0);
    else
        snprintf(out, n, "%.1f M", bytes / (1024.0 * 1024.0));
}

/* ------------------------------------------------------- the listing */

/* Alphabetical, case-insensitive first so "Makefile" and "mystic"
 * sort next to each other; byte order breaks the remaining ties. */
static int name_cmp(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        i++;
    }
    if (a[i] == b[i]) return strcmp(a, b);
    return a[i] ? 1 : -1;
}

static int entry_before(const Entry *a, const Entry *b) {
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir;   /* directories first */
    return name_cmp(a->name, b->name) < 0;
}

static void sort_entries(Entry *e, int n) {
    for (int i = 1; i < n; i++) {
        Entry key = e[i];
        int j = i - 1;
        while (j >= 0 && entry_before(&key, &e[j])) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

static void select_name(Browser *b, const char *keep) {
    b->sel = b->count > 0 ? 0 : -1;
    if (keep && keep[0]) {
        for (int i = 0; i < b->count; i++)
            if (!strcmp(b->ent[i].name, keep)) { b->sel = i; break; }
    }
    b->top = 0;
}

/* Read b->path with opendir/readdir/lstat. Keep `keep` selected if it
 * still exists. Any failure is reported with the real strerror(). */
static void load(Browser *b, const char *keep) {
    b->count = 0;
    b->error = 0;
    b->error_text[0] = 0;

    DIR *d = opendir(b->path);
    if (!d) {
        snprintf(b->error_text, sizeof(b->error_text), "%s: %s", b->path, strerror(errno));
        b->error = 1;
        b->sel = -1;
        b->top = 0;
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (b->count >= MAX_ENTRIES) break;

        char full[PATH_LEN + NAME_LEN + 2];
        int n = snprintf(full, sizeof(full), "%s%s%s", b->path,
                         strcmp(b->path, "/") ? "/" : "", de->d_name);
        if (n < 0 || n >= (int)sizeof(full)) continue;      /* name longer than a path */

        struct stat st;
        if (lstat(full, &st) < 0) continue;                 /* entry vanished mid-scan */

        Entry *e = &b->ent[b->count];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        e->size = (long)st.st_size;
        b->count++;
    }
    closedir(d);

    sort_entries(b->ent, b->count);
    select_name(b, keep);
}

/* --------------------------------------------------------- movement */

static int rows_for(int h) {
    int avail = h - HEAD_H - FOOT_H;
    if (avail < ROW_H) return 0;
    return avail / ROW_H;
}

/* Rows that can be drawn, and rows currently rendered. */
static int visible_rows(const Browser *b) {
    int rows = rows_for(b->h);
    int left = b->count - b->top;
    if (left < 0) left = 0;
    return rows < left ? rows : left;
}

static void scroll_to_sel(Browser *b) {
    int rows = rows_for(b->h);
    if (rows < 1 || b->count < 1) { b->top = 0; return; }
    if (b->sel < 0) b->sel = 0;
    if (b->sel >= b->count) b->sel = b->count - 1;

    if (b->sel < b->top) b->top = b->sel;
    if (b->sel >= b->top + rows) b->top = b->sel - rows + 1;

    int max_top = b->count - rows;
    if (max_top < 0) max_top = 0;
    if (b->top > max_top) b->top = max_top;
    if (b->top < 0) b->top = 0;
}

static void move_sel(Browser *b, int delta) {
    if (b->count < 1) return;
    if (b->sel < 0) b->sel = 0;
    int s = b->sel + delta;
    if (s < 0) s = 0;
    if (s >= b->count) s = b->count - 1;
    b->sel = s;
    scroll_to_sel(b);
}

static void descend(Browser *b) {
    if (b->sel < 0 || b->sel >= b->count) return;
    if (!b->ent[b->sel].is_dir) return;

    char next[PATH_LEN];
    int n = snprintf(next, sizeof(next), "%s%s%s", b->path,
                     strcmp(b->path, "/") ? "/" : "", b->ent[b->sel].name);
    if (n < 0 || n >= (int)sizeof(next)) return;

    snprintf(b->path, sizeof(b->path), "%s", next);
    load(b, NULL);          /* a failure here leaves the error on screen */
}

static void go_up(Browser *b) {
    if (!strcmp(b->path, "/")) return;

    /* Remember the directory we are leaving so it stays selected. */
    char keep[NAME_LEN];
    const char *base = strrchr(b->path, '/');
    snprintf(keep, sizeof(keep), "%s", base ? base + 1 : b->path);

    char *slash = strrchr(b->path, '/');
    if (!slash || slash == b->path) snprintf(b->path, sizeof(b->path), "/");
    else *slash = 0;
    load(b, keep);
}

/* ------------------------------------------------------------ input */

/* TEXT carries printable bytes, '\n' for Enter, '\b' for Backspace and
 * the WM's escape runs for the arrow/Home/End/Page keys. */
static int handle_text(Browser *b, const char *t) {
    for (int i = 0; t[i]; i++) {
        unsigned char ch = (unsigned char)t[i];

        if (ch == 0x1B) {                       /* CSI: ESC [ <final> */
            if (t[i + 1] == '[' && t[i + 2]) {
                switch (t[i + 2]) {
                    case 'A': move_sel(b, -1); break;
                    case 'B': move_sel(b, +1); break;
                    case 'H': if (b->count) { b->sel = 0; scroll_to_sel(b); } break;
                    case 'F': if (b->count) { b->sel = b->count - 1; scroll_to_sel(b); } break;
                    case '5': move_sel(b, -rows_for(b->h)); break;   /* PgUp */
                    case '6': move_sel(b, +rows_for(b->h)); break;   /* PgDn */
                    default: break;
                }
                i += 2;
                continue;
            }
            continue;                           /* lone ESC: nothing to do */
        }

        if (ch == '\n' || ch == '\r') { descend(b); continue; }
        if (ch == '\b' || ch == 0x7F) { go_up(b); continue; }

        switch (ch) {
            case 'j': move_sel(b, +1); break;
            case 'k': move_sel(b, -1); break;
            case 'q': return 1;
            default: break;
        }
    }
    return 0;
}

/* Click: select the row under the pointer, or descend when the row is
 * already the selection. Only the press edge acts, so holding the
 * button does not keep descending. */
static void handle_mouse(Browser *b, int x, int y, int buttons) {
    int prev = b->last_buttons;
    b->last_buttons = buttons;
    if (!(buttons & 1)) return;
    if (prev & 1) return;
    if (x < 0 || x >= b->w) return;
    if (y < HEAD_H || y >= b->h - FOOT_H) return;

    int row = (y - HEAD_H) / ROW_H;
    if (row < 0 || row >= rows_for(b->h)) return;
    int idx = b->top + row;
    if (idx < 0 || idx >= b->count) return;

    if (idx == b->sel) descend(b);
    else b->sel = idx;
}

/* ----------------------------------------------------------- drawing */

static void render(MClient *c, Browser *b) {
    if (b->w < 40 || b->h < HEAD_H + FOOT_H) return;

    Surface s = { c->px, b->w, b->h, c->stride };
    gfx_gradient(&s, 0, 0, b->w, b->h, BG_TOP, BG_BOT);

    Font *fp = font_get(FONT_MEDIUM, 12);     /* path / header */
    Font *fr = font_get(FONT_REGULAR, 11);    /* rows / footer */
    if (!fp || !fr) return;

    /* header: where we are */
    draw_fit(&s, fp, 10.0f, 7.0f, (float)b->w - 20.0f, b->path, C_TEXT);
    surf_blend(&s, 10, HEAD_H - 3, b->w - 20, 1, color_alpha(C_TEXT, 0.10f));

    int rows = rows_for(b->h);
    if (rows > 0) {
        scroll_to_sel(b);

        /* The list gets its own Surface view, so every row - and every
         * glyph in it - is clipped to the band between header and
         * footer instead of spilling over them. */
        Surface list = { c->px + (size_t)HEAD_H * (size_t)c->stride,
                         b->w, rows * ROW_H, c->stride };

        if (b->error) {
            draw_fit(&list, fr, 10.0f, 3.0f, (float)b->w - 20.0f, b->error_text, C_ERROR);
            draw_fit(&list, fr, 10.0f, 3.0f + ROW_H, (float)b->w - 20.0f,
                     "Backspace goes up", color_alpha(C_TEXT, 0.40f));
        } else if (b->count == 0) {
            draw_fit(&list, fr, 10.0f, 3.0f, (float)b->w - 20.0f,
                     "empty directory", color_alpha(C_TEXT, 0.40f));
        }

        for (int i = 0; i < rows; i++) {
            int idx = b->top + i;
            if (idx >= b->count) break;
            const Entry *e = &b->ent[idx];
            int selected = (idx == b->sel);
            float ry = (float)(i * ROW_H);
            float ty = ry + 2.0f;

            if (selected)
                gfx_rrect_aa(&list, 6.0f, ry + 1.0f, (float)b->w - 12.0f,
                             (float)ROW_H - 2.0f, 4.0f,
                             color_alpha(C_ACCENT, 0.20f), 0, 0);

            /* size, right-aligned - a real st_size, in B/K/M */
            char size[24];
            human_size(e->size, size, sizeof(size));
            float sw = font_width(fr, size);
            float right = (float)b->w - 10.0f;
            u32 size_col = selected ? color_alpha(C_TEXT, 0.80f)
                                    : color_alpha(C_TEXT, e->is_dir ? 0.28f : 0.45f);
            font_draw(&list, fr, right - sw, ty, size, size_col);
            right -= sw + 12.0f;

            /* name, '/' appended for directories */
            char name[NAME_LEN + 4];
            snprintf(name, sizeof(name), "%s%s", e->name, e->is_dir ? "/" : "");
            u32 name_col;
            if (selected)    name_col = C_TEXT;
            else if (e->is_dir) name_col = C_DIR;
            else             name_col = color_alpha(C_TEXT, 0.72f);
            draw_fit(&list, fr, 10.0f, ty, right - 10.0f, name, name_col);
        }
    }

    /* footer: how many entries, and how many of them are not on screen */
    if (b->h >= HEAD_H + FOOT_H) {
        surf_blend(&s, 10, b->h - FOOT_H, b->w - 20, 1, color_alpha(C_TEXT, 0.10f));
        float fy = (float)(b->h - FOOT_H) + 3.0f;

        int shown = visible_rows(b);
        int hidden = b->count - shown;
        if (hidden < 0) hidden = 0;

        char left[64], right[48];
        snprintf(left, sizeof(left), "%d entr%s%s", b->count,
                 b->count == 1 ? "y" : "ies",
                 b->count >= MAX_ENTRIES ? " (limit)" : "");
        snprintf(right, sizeof(right), "%d off-screen", hidden);

        float rw = font_width(fr, right);
        draw_fit(&s, fr, 10.0f, fy, (float)b->w - rw - 26.0f, left,
                 color_alpha(C_TEXT, 0.50f));
        font_draw(&s, fr, (float)b->w - 10.0f - rw, fy, right,
                  color_alpha(C_TEXT, 0.50f));
    }
}

/* Last resort: if we cannot even allocate the browser, say so on the
 * screen with the real errno text instead of dying silently. */
static void render_fatal(MClient *c, const char *what, const char *why) {
    if (!c->px) return;
    Surface s = { c->px, c->w, c->h, c->stride };
    gfx_gradient(&s, 0, 0, c->w, c->h, RGB(0x1D, 0x10, 0x14), RGB(0x2B, 0x16, 0x1B));
    Font *fr = font_get(FONT_REGULAR, 12);
    if (!fr) return;
    draw_fit(&s, fr, 12.0f, 12.0f, (float)c->w - 24.0f, what, C_ERROR);
    draw_fit(&s, fr, 12.0f, 34.0f, (float)c->w - 24.0f, why, C_TEXT);
    mc_present(c);
}

/* -------------------------------------------------------------- main */

int main(void) {
    MClient c;
    if (mc_open(&c, "files", "Files", 480, 340) < 0) {
        fprintf(stderr, "files: cannot connect to the window server\n");
        return 1;
    }

    font_init("/usr/share/fonts");

    Browser *b = calloc(1, sizeof(*b));
    if (!b) {
        int saved = errno;
        fprintf(stderr, "files: out of memory: %s\n", strerror(saved));
        render_fatal(&c, "files: cannot start - out of memory", strerror(saved));
        usleep(1500 * 1000);            /* leave the message on screen */
        mc_close(&c);
        return 1;
    }

    b->maxw = c.stride;                 /* the mapping is stride*height pixels */
    b->maxh = c.h;
    b->w = c.w;
    b->h = c.h;
    b->sel = -1;
    b->last_buttons = 0;
    snprintf(b->path, sizeof(b->path), "/");
    load(b, NULL);

    int dirty = 1;
    for (;;) {
        if (dirty) {
            render(&c, b);
            mc_present(&c);
            dirty = 0;
        }

        /* Nothing to animate: sleep until the server has something for us.
         * The timeout only exists so a dropped connection cannot hang us. */
        struct pollfd p = { c.fd, POLLIN, 0 };
        int r = poll(&p, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        MEvent ev;
        int quit = 0;
        while (mc_poll(&c, &ev)) {
            dirty = 1;
            if (ev.type == MC_CLOSE) { quit = 1; break; }
            if (ev.type == MC_TEXT) {
                if (handle_text(b, ev.text)) { quit = 1; break; }
            } else if (ev.type == MC_MOUSE) {
                handle_mouse(b, ev.x, ev.y, ev.buttons);
            } else if (ev.type == MC_RESIZE) {
                /* The pixels we mapped cannot grow; clamp to the mapping
                 * so a resize can never make us write past it. */
                int nw = ev.w < b->maxw ? ev.w : b->maxw;
                int nh = ev.h < b->maxh ? ev.h : b->maxh;
                if (nw > 0 && nh > 0) { b->w = nw; b->h = nh; }
            }
        }
        if (quit) break;
    }

    mc_close(&c);
    free(b);
    return 0;
}
