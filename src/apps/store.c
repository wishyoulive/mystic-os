/* ============================================================
 *  Mystic OS - apps/store.c
 *  "Apps": a browser for the bundles in /usr/share/mystic/apps.
 *
 *  Every row is a real directory read by readdir() that holds a
 *  readable 'manifest'. The manifest is parsed exactly the way
 *  src/wmsrv.c parses it (name/desc/exec keys, one '=' per line),
 *  and the size shown for each bundle is the st_size of its real
 *  payload file, WM_APP_DIR/<id>/<exec>.
 *
 *  This window never forks anything. Marking an app and pressing
 *  Enter (or clicking its row) writes the app id, one line, to
 *  /run/mystic/launch.request - the desktop watches that file and
 *  does the fork/exec. That keeps process creation in one place.
 *
 *  Keys:  j/k or Up/Down  move      Enter   write the launch request
 *         Home/End         first/last         PageUp/PageDown  page
 *         g/G, Ctrl+A/E    first/last  r      rescan the directory
 * ============================================================ */
#include "mystic.h"
#include "ipc.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROW_H   16.0f
#define HEAD_H  26
#define FOOT_H  16
#define LINE_H  13
#define DESC_LINE 256
#define DESC_MAXLINES 3
#define DESC_WORDS 128   /* a 255-char line cannot hold more words than this */
#define LAUNCH_REQUEST "/run/mystic/launch.request"

typedef struct {
    char id[32];
    char name[64];
    char desc[DESC_LINE];
    char exe[128];
    long size;        /* st_size of WM_APP_DIR/<id>/<exe>, 0 if unreadable */
    int  have_size;
} StoreApp;

typedef struct {
    StoreApp *app;            /* WM_MAX_APPS entries, calloc'd once */
    int       count;
    int       sel;
    int       scroll;
    int       w, h;

    char      error[128];     /* why the directory could not be read */
    char      status[128];    /* result of the last launch request */

    char      dlines[DESC_MAXLINES][DESC_LINE];   /* wrapped description */
    int       dn;
    int       wrap_w;         /* width the wrap was computed for */
    char      wrap_id[32];    /* which app's desc the wrap holds; "" if none */
} Store;

/* ------------------------------------------------------------
 * Manifest parsing - identical rules to wms_scan_apps() in
 * src/wmsrv.c: strip the newline, split on the first '=', trim
 * spaces around the key, take the value verbatim.
 * ------------------------------------------------------------ */
static int read_manifest(const char *dir, StoreApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/manifest", dir, a->id);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;
        char *ke = k + strlen(k);
        while (ke > k && ke[-1] == ' ') *--ke = 0;
        if (!strcmp(k, "name"))      snprintf(a->name, sizeof(a->name), "%s", v);
        else if (!strcmp(k, "desc")) snprintf(a->desc, sizeof(a->desc), "%s", v);
        else if (!strcmp(k, "exec")) snprintf(a->exe,  sizeof(a->exe),  "%s", v);
    }
    fclose(f);

    if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
    if (!a->exe[0])  snprintf(a->exe,  sizeof(a->exe),  "bin");
    return 1;
}

/* The payload is a real file on disk; its size is what we show. */
static void probe_payload(StoreApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", WM_APP_DIR, a->id, a->exe);

    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        a->size = (long)st.st_size;
        a->have_size = 1;
    } else {
        a->size = 0;
        a->have_size = 0;
    }
}

static int app_before(const StoreApp *a, const StoreApp *b) {
    return strcmp(a->id, b->id) < 0;
}

static void sort_apps(StoreApp *e, int n) {
    for (int i = 1; i < n; i++) {
        StoreApp key = e[i];
        int j = i - 1;
        while (j >= 0 && app_before(&key, &e[j])) { e[j + 1] = e[j]; j--; }
        e[j + 1] = key;
    }
}

static int index_of(const Store *s, const char *id) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->app[i].id, id)) return i;
    return -1;
}

/* Re-read the bundle directory. Selection follows the same app id. */
static void scan(Store *s) {
    char keep[32];
    keep[0] = 0;
    if (s->sel >= 0 && s->sel < s->count)
        snprintf(keep, sizeof(keep), "%s", s->app[s->sel].id);

    s->count = 0;
    s->error[0] = 0;
    /* Whatever was wrapped describes the list as it was before this scan
     * (a bundle may have appeared, vanished, or had its desc = edited).
     * Throw the cache away so the band cannot show another app's text. */
    s->wrap_id[0] = 0;
    s->wrap_w = -1;
    s->dn = 0;

    DIR *d = opendir(WM_APP_DIR);
    if (!d) {
        snprintf(s->error, sizeof(s->error), "%s: %s", WM_APP_DIR, strerror(errno));
        s->sel = 0;
        s->scroll = 0;
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && s->count < WM_MAX_APPS) {
        if (de->d_name[0] == '.') continue;

        StoreApp *a = &s->app[s->count];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", de->d_name);
        if (!read_manifest(WM_APP_DIR, a)) continue;    /* no manifest: not an app */
        probe_payload(a);
        s->count++;
    }
    closedir(d);

    sort_apps(s->app, s->count);

    if (keep[0]) {
        int i = index_of(s, keep);
        if (i >= 0) { s->sel = i; return; }
    }
    if (s->sel >= s->count) s->sel = s->count > 0 ? s->count - 1 : 0;
    if (s->sel < 0) s->sel = 0;
}

static void store_init(App *a, int w, int h) {
    Store *s = calloc(1, sizeof(Store));
    if (!s) return;
    s->app = calloc(WM_MAX_APPS, sizeof(StoreApp));
    if (!s->app) { free(s); return; }
    s->w = w;
    s->h = h;
    s->wrap_w = -1;
    scan(s);
    a->state = s;
}

static void store_free(App *a) {
    Store *s = a->state;
    if (!s) return;
    free(s->app);
    free(s);
    a->state = NULL;
}

/* ------------------------------------------------------------
 * Geometry. draw() and click() must agree on where the rows are,
 * so both call this.
 * ------------------------------------------------------------ */
static void layout_of(int h, int *rows, int *band, int *nlines) {
    int nl = h >= 210 ? DESC_MAXLINES : h >= 172 ? 2 : h >= 134 ? 1 : 0;
    for (;;) {
        int b = nl > 0 ? nl * LINE_H + LINE_H + 8 : (h >= 96 ? LINE_H + 8 : 0);
        int r = (h - HEAD_H - FOOT_H - b) / (int)ROW_H;
        if (r >= 1 || nl == 0) {
            *rows = r < 0 ? 0 : r;
            *band = b;
            *nlines = nl;
            return;
        }
        nl--;
    }
}

/* ------------------------------------------------------------
 * Text helpers
 * ------------------------------------------------------------ */
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

/* Greedy word wrap: fills `out` with at most maxlines lines. The final
 * line gets the whole remainder and is clipped at draw time, so long
 * descriptions end in ".." instead of vanishing. */
static int wrap_desc(Font *fr, const char *text, float maxw,
                     char out[][DESC_LINE], int maxlines) {
    const char *wstart[DESC_WORDS];
    int         wlen[DESC_WORDS];
    int         nw = 0;

    const char *p = text;
    while (*p && nw < DESC_WORDS) {
        while (*p == ' ') p++;
        if (!*p) break;
        wstart[nw] = p;
        while (*p && *p != ' ') p++;
        wlen[nw] = (int)(p - wstart[nw]);
        nw++;
    }

    int nlines = 0;
    int i = 0;
    while (i < nw && nlines < maxlines) {
        if (nlines == maxlines - 1) {          /* last line: the remainder */
            int len = 0;
            out[nlines][0] = 0;
            while (i < nw && len + 1 < DESC_LINE) {
                if (len && len + 1 < DESC_LINE) out[nlines][len++] = ' ';
                for (int k = 0; k < wlen[i] && len + 1 < DESC_LINE; k++)
                    out[nlines][len++] = wstart[i][k];
                i++;
            }
            out[nlines][len] = 0;
            nlines++;
            break;
        }

        int len = 0;
        out[nlines][0] = 0;
        while (i < nw) {
            char cand[DESC_LINE];
            int cl = 0;
            if (len) { memcpy(cand, out[nlines], (size_t)len); cl = len; cand[cl++] = ' '; }

            int take = wlen[i];
            if (cl + take >= DESC_LINE) take = (int)sizeof(cand) - cl - 1;
            if (take < 0) take = 0;
            memcpy(cand + cl, wstart[i], (size_t)take);
            cl += take;
            cand[cl] = 0;

            if (font_width(fr, cand) > maxw) {
                if (len > 0) break;            /* word goes to the next line */
                /* A single word wider than the whole line: hard cut it
                 * and keep the rest of that word for the next line. */
                int keep = 0;
                char test[DESC_LINE];
                while (keep < take) {
                    int t2 = keep + 1;
                    memcpy(test, wstart[i], (size_t)t2);
                    test[t2] = 0;
                    if (font_width(fr, test) > maxw) break;
                    keep = t2;
                }
                if (keep == 0) keep = 1;
                memcpy(out[nlines], wstart[i], (size_t)keep);
                out[nlines][keep] = 0;
                wstart[i] += keep;
                wlen[i] -= keep;
                len = keep;
                break;
            }

            memcpy(out[nlines], cand, (size_t)cl + 1);
            len = cl;
            i++;
        }
        nlines++;
    }
    return nlines;
}

/* The cache is keyed on the selected app's id, not on its row number, and
 * scan() clears it - so a rescan can never leave another app's text in the
 * band, whether the list grew, shrank, or a desc = line was edited. */
static void refresh_wrap(Store *s, Font *fr, int w) {
    const char *id = (s->sel >= 0 && s->sel < s->count) ? s->app[s->sel].id : "";

    if (s->wrap_w == w && !strcmp(s->wrap_id, id)) return;
    snprintf(s->wrap_id, sizeof(s->wrap_id), "%s", id);
    s->wrap_w = w;
    s->dn = 0;
    if (!s->wrap_id[0]) return;
    s->dn = wrap_desc(fr, s->app[s->sel].desc, (float)w - 12.0f,
                      s->dlines, DESC_MAXLINES);
}

/* ------------------------------------------------------------
 * The launch request. The desktop owns the fork/exec; we only
 * hand it the id, one line, in the file it watches.
 * ------------------------------------------------------------ */
static void request_launch(Store *s) {
    if (s->sel < 0 || s->sel >= s->count) {
        snprintf(s->status, sizeof(s->status), "nothing selected");
        return;
    }
    StoreApp *a = &s->app[s->sel];

    FILE *f = fopen(LAUNCH_REQUEST, "w");
    if (!f) {
        snprintf(s->status, sizeof(s->status), "%s: %s",
                 LAUNCH_REQUEST, strerror(errno));
        return;
    }

    int wrote = fprintf(f, "%s\n", a->id);
    int err = 0;
    if (wrote < 0) err = errno;
    if (fflush(f) != 0 && !err) err = errno;
    if (ferror(f) && !err) err = EIO;
    if (fclose(f) != 0 && !err) err = errno;

    if (err)
        snprintf(s->status, sizeof(s->status), "%s: %s",
                 LAUNCH_REQUEST, strerror(err));
    else
        snprintf(s->status, sizeof(s->status), "asked the desktop to launch %s", a->id);
}

/* ------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------ */
static void store_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Store *st = a->state;
    if (!st) return;
    st->w = w;
    st->h = h;
    if (w < 40 || h < 40) return;

    Font *fp = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fp || !fr || !fd) return;

    /* header: what this window is, and how many bundles it found */
    font_draw(s, fp, (float)x + 6.0f, (float)y + 5.0f, "Apps", th->text);
    {
        char head[64];
        snprintf(head, sizeof(head), "%d bundle%s", st->count,
                 st->count == 1 ? "" : "s");
        float hw = font_width(fr, head);
        if (hw + 60.0f < (float)w)
            font_draw(s, fr, (float)(x + w) - hw - 6.0f, (float)y + 7.0f, head,
                      color_alpha(th->text, 0.45f));
    }
    surf_blend(s, x + 4, y + HEAD_H - 6, w - 8, 1, th->panel_border);

    int rows = 0, band = 0, nlines = 0;
    layout_of(h, &rows, &band, &nlines);

    if (st->error[0]) {
        draw_clipped(s, fr, (float)x + 6.0f, (float)y + (float)HEAD_H,
                     (float)w - 12.0f, st->error, th->accent);
    } else if (st->count == 0) {
        draw_clipped(s, fr, (float)x + 6.0f, (float)y + (float)HEAD_H,
                     (float)w - 12.0f, "no app bundles with a manifest here",
                     th->text_dim);
    } else {
        /* keep the highlighted row inside the scroll window */
        if (rows > 0) {
            if (st->sel < st->scroll) st->scroll = st->sel;
            if (st->sel >= st->scroll + rows) st->scroll = st->sel - rows + 1;
            int max_scroll = st->count - rows;
            if (max_scroll < 0) max_scroll = 0;
            if (st->scroll > max_scroll) st->scroll = max_scroll;
            if (st->scroll < 0) st->scroll = 0;
        }

        for (int i = 0; i < rows; i++) {
            int idx = st->scroll + i;
            if (idx >= st->count) break;
            StoreApp *e = &st->app[idx];
            int selected = (idx == st->sel);
            float ry = (float)y + (float)HEAD_H + (float)i * ROW_H;

            if (selected)
                gfx_rrect_aa(s, (float)x + 3.0f, ry - 1.0f, (float)w - 6.0f, ROW_H,
                             4.0f, color_alpha(th->accent, 0.20f), 0, 0);

            /* payload size, right aligned, from the real st_size */
            float right = (float)(x + w) - 6.0f;
            char sz[32];
            if (e->have_size)
                snprintf(sz, sizeof(sz), "%.1f KB", (double)e->size / 1024.0);
            else
                snprintf(sz, sizeof(sz), "no payload");
            float sw = font_width(fr, sz);
            if (sw + 90.0f < (float)w) {
                font_draw(s, fr, right - sw, ry, sz,
                          color_alpha(th->text, e->have_size ? 0.55f : 0.35f));
                right -= sw + 10.0f;
            }

            /* the exec name, when there is room for it */
            if (w >= 300) {
                float ew = font_width(fd, e->exe);
                if (right - ew > (float)x + 120.0f) {
                    font_draw(s, fd, right - ew, ry + 1.0f, e->exe,
                              color_alpha(th->text, 0.35f));
                    right -= ew + 10.0f;
                }
            }

            float name_x = (float)x + 8.0f;
            draw_clipped(s, fr, name_x, ry, right - name_x, e->name,
                         selected ? th->text : th->text_dim);
        }
    }

    /* description band: the selected manifest's 'desc =' line, wrapped */
    if (band > 0) {
        float by = (float)(y + h) - (float)FOOT_H - (float)band;
        surf_blend(s, x + 4, (int)by, w - 8, 1, th->panel_border);
        refresh_wrap(st, fr, w);

        if (st->count > 0 && st->sel >= 0 && st->sel < st->count) {
            int shown = st->dn;
            if (shown > nlines) shown = nlines;
            if (st->dn == 0 && nlines > 0)
                draw_clipped(s, fr, (float)x + 6.0f, by + 5.0f, (float)w - 12.0f,
                             "this manifest has no desc = line", th->text_dim);
            for (int i = 0; i < shown; i++)
                draw_clipped(s, fr, (float)x + 6.0f, by + 5.0f + (float)i * LINE_H,
                             (float)w - 12.0f, st->dlines[i],
                             color_alpha(th->text, 0.85f));

            /* the hint: this window asks, the desktop launches */
            float hy = (float)(y + h) - (float)FOOT_H - (float)LINE_H - 3.0f;
            char hint[96];
            snprintf(hint, sizeof(hint), "Enter or click: write %s", LAUNCH_REQUEST);
            draw_clipped(s, fd, (float)x + 6.0f, hy, (float)w - 12.0f, hint,
                         color_alpha(th->text, 0.40f));
        } else {
            float hy = (float)(y + h) - (float)FOOT_H - (float)LINE_H - 3.0f;
            char hint[96];
            snprintf(hint, sizeof(hint), "the desktop launches %s", LAUNCH_REQUEST);
            draw_clipped(s, fd, (float)x + 6.0f, hy, (float)w - 12.0f, hint,
                         color_alpha(th->text, 0.40f));
        }
    }

    /* footer: how many bundles, and the outcome of the last request */
    {
        float fy = (float)(y + h) - 14.0f;
        const char *tail = st->status[0]
                         ? st->status
                         : "the desktop launches what /run/mystic/launch.request names";
        float tw = font_width(fr, tail);
        if (tw + 70.0f < (float)w) {
            font_draw(s, fr, (float)(x + w) - tw - 6.0f, fy, tail,
                      color_alpha(st->status[0] ? th->accent : th->text, 0.45f));
            tw += 12.0f;
        } else {
            tw = 0.0f;
        }

        char foot[64];
        snprintf(foot, sizeof(foot), "%d app%s%s", st->count,
                 st->count == 1 ? "" : "s",
                 st->count >= WM_MAX_APPS ? " (max)" : "");
        draw_clipped(s, fr, (float)x + 6.0f, fy, (float)w - tw - 12.0f, foot,
                     color_alpha(th->text, 0.45f));
    }
}

/* ------------------------------------------------------------
 * Input
 * ------------------------------------------------------------ */
static void move_sel(Store *s, int delta) {
    if (s->count == 0) return;
    int v = s->sel + delta;
    if (v < 0) v = 0;
    if (v >= s->count) v = s->count - 1;
    s->sel = v;
}

static void store_key(App *a, const char *bytes, int n) {
    Store *s = a->state;
    if (!s) return;

    int rows = 0, band = 0, nlines = 0;
    layout_of(s->h, &rows, &band, &nlines);
    if (rows < 1) rows = 1;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B) {                       /* arrow / Home / End / Page runs */
            if (i + 2 < n && bytes[i + 1] == '[') {
                switch (bytes[i + 2]) {
                    case 'A': move_sel(s, -1); break;
                    case 'B': move_sel(s, +1); break;
                    case 'H': s->sel = 0; break;
                    case 'F': s->sel = s->count > 0 ? s->count - 1 : 0; break;
                    case '5':                            /* \x1b[5~ PageUp */
                        move_sel(s, -rows);
                        if (i + 3 < n) i++;
                        break;
                    case '6':                            /* \x1b[6~ PageDown */
                        move_sel(s, +rows);
                        if (i + 3 < n) i++;
                        break;
                    default: break;
                }
                i += 2;
            }
            continue;
        }

        switch (c) {
            case 'j': move_sel(s, +1); break;
            case 'k': move_sel(s, -1); break;
            case 'g': s->sel = 0; break;
            case 'G': s->sel = s->count > 0 ? s->count - 1 : 0; break;
            case 0x01: s->sel = 0; break;                       /* Ctrl+A */
            case 0x05: s->sel = s->count > 0 ? s->count - 1 : 0; break;  /* Ctrl+E */
            case 'r': scan(s); break;
            case '\n':
            case '\r': request_launch(s); break;
            default: break;
        }
    }
}

static void store_click(App *a, int x, int y, int buttons) {
    Store *s = a->state;
    (void)x;
    if (!s) return;
    if (buttons && !(buttons & 1)) return;      /* left button only */

    int rows = 0, band = 0, nlines = 0;
    layout_of(s->h, &rows, &band, &nlines);
    if (rows < 1) return;

    /* Only the row band owns rows. A click above the first row - the
     * "Apps" title, the "N bundles" count, or the rule under them - and a
     * click in the footer or the desc band below the list belong to no row
     * and must do nothing. This used to be `(int)(((float)y - HEAD_H) /
     * ROW_H)`: C truncates a negative float toward zero, so y in
     * [HEAD_H - ROW_H + 1, HEAD_H - 1] (here 11..25) became row 0 and the
     * `row < 0` guard could never fire - one click on the word "Apps"
     * selected the top bundle and wrote /run/mystic/launch.request, which
     * the desktop then fork/exec'd. Reject the bands first, then divide a
     * value that is known to be >= 0, so truncation and floor agree and no
     * negative or out-of-range index can be produced. */
    if (y < HEAD_H) return;                     /* header band */
    if (y >= s->h - FOOT_H) return;             /* footer band */

    int row = (y - HEAD_H) / (int)ROW_H;        /* y >= HEAD_H: exact */
    if (row < 0 || row >= rows) return;         /* past the list / desc band */
    int idx = s->scroll + row;
    if (idx < 0 || idx >= s->count) return;

    s->sel = idx;
    request_launch(s);
}

static void store_resize(App *a, int w, int h) {
    Store *s = a->state;
    if (!s) return;
    s->w = w;
    s->h = h;
    s->wrap_w = -1;              /* force a re-wrap for the new width */
}

const AppVT app_vt_store = {
    .id = "store",
    .title = "Apps",
    .init = store_init,
    .draw = store_draw,
    .key = store_key,
    .click = store_click,
    .resize = store_resize,
    .free = store_free,
};
