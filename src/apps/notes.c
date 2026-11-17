/* ============================================================
 *  Mystic OS - apps/notes.c
 *  A text editor that writes to the disk, so notes survive a
 *  reboot. /root is the FAT image. Ctrl+S saves.
 * ============================================================ */
#include "mystic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NOTES_MAX 65536
#define LINE_H 16.0f

typedef struct {
    char  *buf;
    int    len;
    int    cur;
    int    w, h;
    int    dirty;
    time_t saved_at;
    int    saves;
} Notes;

static const char *NOTES_PATH = "/root/notes.txt";

static void notes_init(App *a, int w, int h) {
    Notes *n = calloc(1, sizeof(Notes));
    if (!n) return;
    n->buf = calloc(NOTES_MAX, 1);
    if (!n->buf) { free(n); return; }
    n->w = w;
    n->h = h;

    FILE *f = fopen(NOTES_PATH, "r");
    if (f) {
        size_t got = fread(n->buf, 1, NOTES_MAX - 1, f);
        n->len = (int)got;
        n->cur = n->len;
        fclose(f);
    } else {
        const char *welcome =
            "Notes.\n\n"
            "Type here. Backspace works.\n"
            "Ctrl+S writes /root/notes.txt,\n"
            "and /root is a real disk, so it\n"
            "is still here after a reboot.\n";
        n->len = (int)strlen(welcome);
        if (n->len > NOTES_MAX - 1) n->len = NOTES_MAX - 1;
        memcpy(n->buf, welcome, (size_t)n->len);
        n->cur = n->len;
        n->dirty = 1;
    }
    a->state = n;
}

static void notes_free(App *a) {
    Notes *n = a->state;
    if (!n) return;
    free(n->buf);
    free(n);
    a->state = NULL;
}

static void save(Notes *n) {
    FILE *f = fopen(NOTES_PATH, "w");
    if (!f) return;
    fwrite(n->buf, 1, (size_t)n->len, f);
    fflush(f);
    /* Without this the bytes sit in the kernel's page cache. Pull the
     * plug and they are gone - which is exactly what happened the first
     * time this was tested. */
    fsync(fileno(f));
    fclose(f);
    n->dirty = 0;
    n->saved_at = time(NULL);
    n->saves++;
}

static void insert(Notes *n, char c) {
    if (n->len >= NOTES_MAX - 1) return;
    memmove(n->buf + n->cur + 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->buf[n->cur] = c;
    n->cur++;
    n->len++;
    n->dirty = 1;
}

static void erase_one(Notes *n) {
    if (n->cur <= 0) return;
    memmove(n->buf + n->cur - 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->cur--;
    n->len--;
    n->dirty = 1;
}

static void notes_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Notes *n = a->state;
    if (!n) return;
    n->w = w;
    n->h = h;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fd) return;

    surf_fill(s, x, y, w, h, color_mix(th->window_bg, 0xFF000000u, 0.25f));

    float avail = (float)w - 16.0f;
    int   max_lines = (int)((h - 26) / LINE_H);
    if (max_lines < 1) max_lines = 1;

    int   line = 0;
    float px = (float)x + 8;
    float py = (float)y + 6;
    char  one[2] = { 0, 0 };

    for (int i = 0; i < n->len && line < max_lines; i++) {
        char c = n->buf[i];
        if (c == 10) {                       /* newline */
            px = (float)x + 8;
            py += LINE_H;
            line++;
            continue;
        }
        one[0] = c;
        float cw = font_width(fr, one);
        if (px + cw > (float)x + 8 + avail) {
            px = (float)x + 8;
            py += LINE_H;
            line++;
            if (line >= max_lines) break;
        }
        if (i == n->cur)
            surf_blend(s, (int)px, (int)py, 1, (int)LINE_H, th->accent);
        font_draw(s, fr, px, py, one, th->text);
        px += cw;
    }
    if (n->cur >= n->len)
        surf_blend(s, (int)px, (int)py, 1, (int)LINE_H, th->accent);

    char foot[160];
    if (n->dirty) {
        snprintf(foot, sizeof(foot), "%d bytes  -  unsaved changes  -  Ctrl+S saves", n->len);
    } else if (n->saves == 0) {
        snprintf(foot, sizeof(foot), "%d bytes  -  loaded from disk", n->len);
    } else {
        long ago = (long)(time(NULL) - n->saved_at);
        snprintf(foot, sizeof(foot), "%d bytes  -  saved %ld s ago", n->len, ago);
    }
    surf_blend(s, x + 4, (int)(y + h) - 20, w - 8, 1, th->panel_border);
    font_draw(s, fd, (float)x + 8, (float)(y + h) - 16, foot, color_alpha(th->text, 0.55f));
}

static void notes_key(App *a, const char *bytes, int count) {
    Notes *n = a->state;
    if (!n) return;
    for (int i = 0; i < count; i++) {
        unsigned char c = (unsigned char)bytes[i];
        int code = (int)c;
        if (code == 0x13) {                   /* Ctrl+S */
            save(n);
        } else if (code == 0x08) {            /* Backspace */
            erase_one(n);
        } else if (code == 10 || code == 13) { /* Enter */
            insert(n, 10);
        } else if (code == 9) {               /* Tab */
            insert(n, ' ');
            insert(n, ' ');
        } else if (code >= 32 && code < 127) {
            insert(n, (char)code);
        }
    }
}

static void notes_click(App *a, int x, int y, int buttons) {
    (void)a; (void)x; (void)y; (void)buttons;
}

static void notes_resize(App *a, int w, int h) {
    Notes *n = a->state;
    if (n) { n->w = w; n->h = h; }
}

const AppVT app_vt_notes = {
    .id = "notes",
    .title = "Notes",
    .init = notes_init,
    .draw = notes_draw,
    .key = notes_key,
    .click = notes_click,
    .resize = notes_resize,
    .free = notes_free,
};
