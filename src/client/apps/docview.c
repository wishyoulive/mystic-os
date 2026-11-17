/* ============================================================
 *  Mystic OS - apps (external) / docview
 *
 *  A document window for the text inside Office files. It is not a
 *  word processor: it shows the characters and the structure that
 *  src/office.c extracted - headings in a larger bold face, body
 *  text wrapped to the window, tables as aligned columns with
 *  wrapping cells - and it says plainly, in the window, what it
 *  cannot read.
 *
 *  Keys:  j / Down      scroll one line
 *         k / Up        back one line
 *         Space/PgDn    page down      b / PgUp   page up
 *         g / Home      top            G / End    bottom
 *         r             structured text <-> raw text
 *         i             document information (format, file, limits)
 *         q             quit
 *  Mouse: the wheel scrolls; dragging the page moves it.
 *
 *  The reader lives in src/office.c. It is compiled into this app
 *  by including it below: tools/mkapp.sh links a fixed object list,
 *  and office.c has no main and no global state, so this keeps the
 *  bundle buildable without changing the tool.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"
#include "../../office.c"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DOCVIEW_HOST_TEST
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Linux input event codes, as the window server forwards them. */
#define DV_KEY_ESC      1
#define DV_KEY_Q       16
#define DV_KEY_R       19
#define DV_KEY_I       23
#define DV_KEY_B       48
#define DV_KEY_G       34
#define DV_KEY_HOME   102
#define DV_KEY_UP     103
#define DV_KEY_PAGEUP 104
#define DV_KEY_LEFT   105
#define DV_KEY_RIGHT  106
#define DV_KEY_END    107
#define DV_KEY_DOWN   108
#define DV_KEY_PAGEDN 109

/* Palette: the app owns its pixels, so it carries its own colours,
 * chosen to sit next to the desktop without depending on its theme. */
#define C_BG_TOP RGB(0x0D, 0x12, 0x1C)
#define C_BG_BOT RGB(0x14, 0x1B, 0x28)
#define C_TEXT   RGB(0xE6, 0xEB, 0xF5)
#define C_HEAD   RGB(0xF4, 0xF7, 0xFF)
#define C_DIM    RGB(0x9A, 0xA6, 0xBC)
#define C_FAINT  RGB(0x6B, 0x77, 0x8C)
#define C_ACCENT RGB(0x79, 0xA6, 0xF7)
#define C_RULE   RGB(0x28, 0x32, 0x45)
#define C_ERR    RGB(0xE8, 0x8A, 0x8A)
#define C_PANEL  RGB(0x0A, 0x0E, 0x16)
#define C_ALT    RGB(0x18, 0x20, 0x2F)

#define DV_SCROLL_STEP 26      /* one j/k, before ui_px() */
#define DV_WHEEL_LINES 3       /* notches -> lines */

enum { IT_TEXT = 0, IT_ROW = 1 };

/* One laid-out line of the document. Offsets point into doc.text (or
 * doc.plain in raw mode); keeping them as offsets means the arena can
 * move without invalidating the layout. */
typedef struct {
    unsigned char kind;
    unsigned char style;
    unsigned char first;       /* first line of its block (bullet marker) */
    unsigned char clipped;
    int block;
    int row;
    int table;                 /* index into Viewer.tables, -1 = none */
    int y, h;
    int x;
    uint32_t off, len;
} DvLine;

typedef struct {
    int n;
    int w[64];
    int total;
    int overflow;              /* wider than the window: clipped */
    int block;
} DvTable;

#define DV_MAX_TABLES 16

typedef struct {
    OfficeDoc doc;
    int loaded;                /* a document was parsed */
    int raw;                   /* plain text view */
    int show_info;
    char err[320];
    char path[512];

    DvLine *lines;
    int nlines, cap;
    int y;                     /* layout cursor, content pixels */
    int content_h;
    int scroll;

    DvTable tables[DV_MAX_TABLES];
    int ntables;

    int w, h, maxw, maxh;
    int body_top, body_h;
    int last_buttons, last_y, dragging;
    int table_wide, cell_clipped;
    int oom;
} Viewer;

/* ------------------------------------------------------------ text */

static int dv_min(int a, int b) { return a < b ? a : b; }
static int dv_max(int a, int b) { return a > b ? a : b; }
static int dv_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int dv_margin(void) { return ui_px(26); }

static int dv_u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static Font *dv_font(int style, int bold, int italic) {
    int face = FONT_REGULAR;
    float size = 12.0f;
    switch (style) {
    case OFFICE_STYLE_TITLE:  face = FONT_SEMIBOLD; size = 25.0f; break;
    case OFFICE_STYLE_H1:     face = FONT_SEMIBOLD; size = 21.0f; break;
    case OFFICE_STYLE_H2:     face = FONT_SEMIBOLD; size = 16.0f; break;
    case OFFICE_STYLE_H3:     face = FONT_SEMIBOLD; size = 13.5f; break;
    case OFFICE_STYLE_QUOTE:  size = 12.0f; break;
    default:
        face = bold ? FONT_SEMIBOLD : (italic ? FONT_MEDIUM : FONT_REGULAR);
        size = 12.0f;
        break;
    }
    Font *f = font_get(face, size);
    if (!f) f = font_get(FONT_REGULAR, size);
    return f;
}

static int dv_line_h(Font *f) {
    return (int)(font_height(f) + (float)ui_px(4) + 0.5f);
}

/* Width of base[off..off+len) without copying: the arena is ours and
 * writable, so a NUL is placed and put back. */
static float dv_width_mut(char *base, uint32_t off, uint32_t len, Font *f) {
    if (!base || len == 0 || !f) return 0;
    char saved = base[off + len];
    base[off + len] = 0;
    float w = font_width(f, base + off);
    base[off + len] = saved;
    return w;
}

/* Width of a paragraph range, switching face at every run boundary. */
static float dv_measure_block(Viewer *v, const OfficeBlock *b,
                              uint32_t abs_off, uint32_t len) {
    if (len == 0) return 0;
    if (b->nruns <= 0)
        return dv_width_mut(v->doc.text, abs_off, len,
                            dv_font(b->style, b->bold, b->italic));

    float total = 0;
    uint32_t pos = 0;
    int ri = 0;
    while (pos < len) {
        uint32_t at = abs_off + pos;
        while (ri < b->nruns && b->runs[ri].off + b->runs[ri].len <= at) ri++;
        int bold = b->bold, italic = b->italic;
        uint32_t seg = len - pos;
        if (ri < b->nruns) {
            if (at >= b->runs[ri].off) {
                bold = b->runs[ri].bold;
                italic = b->runs[ri].italic;
                uint32_t avail = b->runs[ri].off + b->runs[ri].len - at;
                if (avail < seg) seg = avail;
            } else {
                uint32_t avail = b->runs[ri].off - at;
                if (avail < seg) seg = avail;
            }
        }
        if (seg == 0) seg = 1;
        total += dv_width_mut(v->doc.text, at, seg, dv_font(b->style, bold, italic));
        pos += seg;
    }
    return total;
}

static void dv_draw_block(Viewer *v, Surface *s, const OfficeBlock *b,
                          uint32_t abs_off, uint32_t len, float x, float y, u32 color) {
    if (len == 0) return;
    if (b->nruns <= 0) {
        Font *f = dv_font(b->style, b->bold, b->italic);
        char saved = v->doc.text[abs_off + len];
        v->doc.text[abs_off + len] = 0;
        font_draw(s, f, x, y, v->doc.text + abs_off, color);
        v->doc.text[abs_off + len] = saved;
        return;
    }
    uint32_t pos = 0;
    int ri = 0;
    while (pos < len) {
        uint32_t at = abs_off + pos;
        while (ri < b->nruns && b->runs[ri].off + b->runs[ri].len <= at) ri++;
        int bold = b->bold, italic = b->italic;
        uint32_t seg = len - pos;
        if (ri < b->nruns) {
            if (at >= b->runs[ri].off) {
                bold = b->runs[ri].bold;
                italic = b->runs[ri].italic;
                uint32_t avail = b->runs[ri].off + b->runs[ri].len - at;
                if (avail < seg) seg = avail;
            } else {
                uint32_t avail = b->runs[ri].off - at;
                if (avail < seg) seg = avail;
            }
        }
        if (seg == 0) seg = 1;
        Font *f = dv_font(b->style, bold, italic);
        char saved = v->doc.text[at + seg];
        v->doc.text[at + seg] = 0;
        font_draw(s, f, x, y, v->doc.text + at, color);
        v->doc.text[at + seg] = saved;
        x += dv_width_mut(v->doc.text, at, seg, f);
        pos += seg;
    }
}

/* ------------------------------------------------------- line wrapping */

/* One wrapped line out of a mutable buffer, the algorithm used by table
 * cells and by the raw-text view: words break at spaces, a word wider
 * than the line is cut on a UTF-8 boundary. Returns 0 at the end. */
static int dv_wrap_next(char *s, int n, Font *f, int maxw,
                        int *cur, int *start, int *len, int *clipped) {
    int pos = *cur;
    while (pos < n && s[pos] == ' ') pos++;
    if (pos >= n) { *cur = n; return 0; }

    int ls = pos, le = pos;
    float w = 0;
    while (pos < n) {
        if (s[pos] == '\n') { pos++; break; }
        if (s[pos] == ' ') { pos++; continue; }
        int ts = pos, te = pos;
        while (te < n && s[te] != ' ' && s[te] != '\n') te++;
        pos = te;
        float tw = dv_width_mut(s, (uint32_t)ts, (uint32_t)(te - ts), f);
        float gap = (le > ls) ? dv_width_mut(s, (uint32_t)le, (uint32_t)(ts - le), f) : 0.0f;
        if ((le == ls) ? (tw <= (float)maxw) : (w + gap + tw <= (float)maxw)) {
            w += gap + tw;
            le = te;
        } else if (le == ls) {
            int cut = ts;
            float cw = 0;
            while (cut < te) {
                int cl = dv_u8_len((unsigned char)s[cut]);
                if (cut + cl > te) cl = te - cut;
                float g = dv_width_mut(s, (uint32_t)cut, (uint32_t)cl, f);
                if (cw + g > (float)maxw && cut > ts) break;
                cw += g;
                cut += cl;
            }
            if (cut == ts) cut = ts + dv_u8_len((unsigned char)s[ts]);
            le = cut;
            pos = cut;
            if (clipped) *clipped = 1;
            break;
        } else {
            pos = ts;
            break;
        }
    }
    if (le == ls && pos < n) pos++;
    *cur = pos;
    *start = ls;
    *len = le - ls;
    return le > ls;
}

static int dv_wrap_count(const char *text, Font *f, int maxw, int *clipped) {
    char buf[1024];
    int n = (int)strlen(text);
    if (n > (int)sizeof(buf) - 1) { n = (int)sizeof(buf) - 1; if (clipped) *clipped = 1; }
    memcpy(buf, text, (size_t)n);
    buf[n] = 0;
    int cur = 0, start = 0, len = 0, lines = 0;
    while (lines < 64 && dv_wrap_next(buf, n, f, maxw, &cur, &start, &len, clipped))
        lines++;
    return lines < 1 ? 1 : lines;
}

/* Greedy fit for a single-line label. */
static void dv_draw_fit(Surface *s, Font *f, float x, float y, float maxw,
                        const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 4) return;
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", text);
    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }
    float dots = font_width(f, "..");
    size_t n = strlen(buf);
    while (n > 0) {
        buf[n] = 0;
        if (font_width(f, buf) + dots <= maxw) break;
        n--;
    }
    if (n == 0) return;
    buf[n] = '.';
    buf[n + 1] = '.';
    buf[n + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* ------------------------------------------------------------ layout */

static int dv_space_before(int style) {
    switch (style) {
    case OFFICE_STYLE_TITLE: return ui_px(10);
    case OFFICE_STYLE_H1:    return ui_px(20);
    case OFFICE_STYLE_H2:    return ui_px(16);
    case OFFICE_STYLE_H3:    return ui_px(12);
    case OFFICE_STYLE_BULLET: return ui_px(3);
    default:                 return ui_px(9);
    }
}

static int dv_space_after(int style) {
    switch (style) {
    case OFFICE_STYLE_TITLE: return ui_px(8);
    case OFFICE_STYLE_H1:    return ui_px(6);
    case OFFICE_STYLE_H2:    return ui_px(4);
    default:                 return 0;
    }
}

static DvLine *dv_add_line(Viewer *v, int kind, int block, int row, int table,
                           int y, int h, int x, uint32_t off, uint32_t len,
                           int style, int first, int clipped) {
    if (v->nlines >= 200000) { v->oom = 1; return NULL; }
    if (v->nlines == v->cap) {
        int cap = v->cap ? v->cap * 2 : 256;
        DvLine *p = realloc(v->lines, (size_t)cap * sizeof(*p));
        if (!p) { v->oom = 1; return NULL; }
        v->lines = p;
        v->cap = cap;
    }
    DvLine *l = &v->lines[v->nlines++];
    memset(l, 0, sizeof(*l));
    l->kind = (unsigned char)kind;
    l->block = block;
    l->row = row;
    l->table = table;
    l->y = y;
    l->h = h;
    l->x = x;
    l->off = off;
    l->len = len;
    l->style = (unsigned char)style;
    l->first = (unsigned char)(first ? 1 : 0);
    l->clipped = (unsigned char)(clipped ? 1 : 0);
    return l;
}

static uint32_t dv_hard_cut(Viewer *v, const OfficeBlock *b,
                            uint32_t ts, uint32_t te, int maxw) {
    uint32_t cut = ts;
    float w = 0;
    while (cut < te) {
        uint32_t cl = (uint32_t)dv_u8_len((unsigned char)v->doc.text[cut]);
        if (cut + cl > te) cl = te - cut;
        float cw = dv_measure_block(v, b, cut, cl);
        if (w + cw > (float)maxw && cut > ts) break;
        w += cw;
        cut += cl;
    }
    if (cut == ts) {
        cut += (uint32_t)dv_u8_len((unsigned char)v->doc.text[ts]);
        if (cut > te) cut = te;
    }
    return cut;
}

static void dv_layout_para(Viewer *v, int bi) {
    const OfficeBlock *b = &v->doc.blocks[bi];
    if (b->para.len == 0) return;

    Font *f = dv_font(b->style, b->bold, b->italic);
    int lh = dv_line_h(f);
    int x = dv_margin() + b->level * ui_px(16) +
            (b->style == OFFICE_STYLE_BULLET ? ui_px(15) : 0);
    int maxw = v->w - dv_margin() - x;
    if (maxw < ui_px(80)) maxw = ui_px(80);

    v->y += dv_space_before(b->style);

    const char *s = v->doc.text + b->para.off;
    uint32_t n = b->para.len;
    uint32_t pos = 0;
    int first = 1;

    while (pos < n) {
        while (pos < n && s[pos] == ' ') pos++;
        if (pos >= n) break;

        uint32_t ls = pos, le = pos;
        float w = 0;
        int done = 0;
        while (pos < n && !done) {
            if (s[pos] == '\n') { pos++; done = 1; break; }
            if (s[pos] == ' ') { pos++; continue; }
            uint32_t ts = pos, te;
            while (pos < n && s[pos] != ' ' && s[pos] != '\n') pos++;
            te = pos;
            float tw = dv_measure_block(v, b, b->para.off + ts, te - ts);
            float gap = (le > ls) ? dv_measure_block(v, b, b->para.off + le, ts - le) : 0.0f;
            if ((le == ls) ? (tw <= (float)maxw) : (w + gap + tw <= (float)maxw)) {
                w += gap + tw;
                le = te;
            } else if (le == ls) {
                uint32_t cut = dv_hard_cut(v, b, b->para.off + ts, b->para.off + te, maxw);
                uint32_t rel = cut - b->para.off;
                if (rel <= ts) rel = ts + (uint32_t)dv_u8_len((unsigned char)s[ts]);
                if (rel > n) rel = n;
                le = rel;
                pos = rel;
                done = 1;
            } else {
                pos = ts;
                done = 1;
            }
        }
        if (le > ls) {
            dv_add_line(v, IT_TEXT, bi, -1, -1, v->y, lh, x,
                        b->para.off + ls, le - ls, b->style, first, 0);
            v->y += lh;
            first = 0;
        } else if (pos == ls && pos < n) {
            pos++;
        }
        if (v->oom) return;
    }
    v->y += dv_space_after(b->style);
}

static void dv_table_geom(Viewer *v, const OfficeBlock *b, DvTable *g) {
    memset(g, 0, sizeof(*g));
    int n = b->cols > 64 ? 64 : b->cols;
    g->n = n;
    g->block = -1;
    int pad = ui_px(6);
    int minw = ui_px(30) + 2 * pad;
    int avail = dv_max(v->w - 2 * dv_margin(), minw);
    int nat_cap = dv_max(avail / 2, minw);

    int total = 0;
    int nat[64];
    for (int c = 0; c < n; c++) {
        int cw = minw;
        for (int r = 0; r < b->rows; r++) {
            int bold = b->cell_bold ? b->cell_bold[(size_t)r * (size_t)b->cols + (size_t)c] : 0;
            Font *fc = dv_font(OFFICE_STYLE_BODY, bold, 0);
            const char *t = office_cell_text(&v->doc, b, r, c);
            int w = (int)font_width(fc, t) + 2 * pad + 2;
            if (w > nat_cap) w = nat_cap;
            if (w > cw) cw = w;
        }
        nat[c] = cw;
        total += cw;
    }

    if (n == 1) {
        g->w[0] = avail;
    } else if (total <= avail) {
        float scale = (float)avail / (float)total;
        int used = 0;
        for (int c = 0; c < n; c++) {
            g->w[c] = (int)((float)nat[c] * scale);
            if (g->w[c] < minw) g->w[c] = minw;
            used += g->w[c];
        }
        for (int c = 0; used < avail; c = (c + 1) % n) { g->w[c]++; used++; }
        for (int c = n - 1; used > avail && c >= 0; c--)
            if (g->w[c] > minw) { g->w[c]--; used--; }
    } else {
        int min_total = n * minw;
        if (min_total >= avail) {
            for (int c = 0; c < n; c++) g->w[c] = minw;
            g->overflow = 1;
        } else {
            float scale = (float)(avail - min_total) / (float)(total - min_total);
            int used = 0;
            for (int c = 0; c < n; c++) {
                g->w[c] = minw + (int)((float)(nat[c] - minw) * scale);
                used += g->w[c];
            }
            for (int c = 0; used < avail && c < n; c = (c + 1) % n) { g->w[c]++; used++; }
        }
    }
    g->total = 0;
    for (int c = 0; c < n; c++) g->total += g->w[c];
    if (g->total > avail) g->overflow = 1;
    g->block = 0;
}

static void dv_layout_table(Viewer *v, int bi) {
    const OfficeBlock *b = &v->doc.blocks[bi];
    v->y += ui_px(12);

    DvTable g;
    dv_table_geom(v, b, &g);
    int ti = -1;
    if (v->ntables < DV_MAX_TABLES) {
        ti = v->ntables;
        v->tables[ti] = g;
        v->ntables++;
    } else {
        v->table_wide = 1;
    }
    if (g.overflow) v->table_wide = 1;

    int pad = ui_px(6);
    int vpad = ui_px(5);
    int lh = dv_line_h(dv_font(OFFICE_STYLE_BODY, 0, 0));

    for (int r = 0; r < b->rows; r++) {
        int lines = 1;
        for (int c = 0; c < g.n; c++) {
            int bold = b->cell_bold ? b->cell_bold[(size_t)r * (size_t)b->cols + (size_t)c] : 0;
            Font *fc = dv_font(OFFICE_STYLE_BODY, bold, 0);
            int clipped = 0;
            int cl = dv_wrap_count(office_cell_text(&v->doc, b, r, c), fc,
                                   dv_max(g.w[c] - 2 * pad, ui_px(12)), &clipped);
            if (clipped) v->cell_clipped = 1;
            if (cl > lines) lines = cl;
        }
        if (lines > 24) { lines = 24; v->cell_clipped = 1; }
        int h = lines * lh + 2 * vpad;
        dv_add_line(v, IT_ROW, bi, r, ti, v->y, h, 0, 0, 0,
                    OFFICE_STYLE_BODY, r == 0, 0);
        v->y += h;
        if (v->oom) return;
    }
    v->y += ui_px(16);
}

static void dv_layout_raw(Viewer *v) {
    Font *f = font_get(FONT_MONO, 10.5f);
    if (!f) f = font_get(FONT_REGULAR, 11.0f);
    int lh = dv_line_h(f);
    int x = dv_margin();
    int maxw = v->w - 2 * dv_margin();
    if (maxw < ui_px(80)) maxw = ui_px(80);

    v->y = ui_px(4);
    const char *src = v->doc.plain ? v->doc.plain : "";
    int n = (int)strlen(src);
    int pos = 0;
    while (pos < n) {
        /* one physical line at a time, then wrapped to the width */
        int eol = pos;
        while (eol < n && src[eol] != '\n') eol++;
        int cur = pos, start = 0, len = 0;
        while (dv_wrap_next((char *)src + pos, eol - pos, f, maxw, &cur, &start, &len, NULL)) {
            dv_add_line(v, IT_TEXT, -1, -1, -1, v->y, lh, x,
                        (uint32_t)(pos + start), (uint32_t)len, OFFICE_STYLE_BODY, 0, 0);
            v->y += lh;
            if (v->oom) return;
        }
        pos = eol + 1;
    }
    v->y += ui_px(10);
}

static void dv_layout(Viewer *v) {
    const int head = ui_px(58);
    const int foot = ui_px(30);
    v->body_top = head;
    v->body_h = v->h - head - foot;
    if (v->body_h < ui_px(40)) v->body_h = ui_px(40);

    v->nlines = 0;
    v->ntables = 0;
    v->y = 0;
    v->table_wide = 0;
    v->cell_clipped = 0;
    v->oom = 0;

    if (v->loaded && v->doc.nblocks > 0) {
        if (v->raw) {
            dv_layout_raw(v);
        } else {
            v->y = ui_px(6);
            for (int i = 0; i < v->doc.nblocks && !v->oom; i++) {
                const OfficeBlock *b = &v->doc.blocks[i];
                if (b->kind == OFFICE_BLOCK_PARA) dv_layout_para(v, i);
                else dv_layout_table(v, i);
            }
            v->y += ui_px(24);
        }
    } else {
        v->y = 0;
    }
    v->content_h = v->y;
    if (v->content_h < v->body_h) v->content_h = v->body_h;
}

static void dv_clamp_scroll(Viewer *v) {
    int maxscroll = v->content_h - v->body_h;
    if (maxscroll < 0) maxscroll = 0;
    v->scroll = dv_clamp(v->scroll, 0, maxscroll);
}

static void dv_scroll_by(Viewer *v, int px) {
    v->scroll += px;
    dv_clamp_scroll(v);
}

/* ------------------------------------------------------------ drawing */

static void dv_draw_wrapped(Surface *s, Font *f, int x, int y, int maxw,
                            int lh, const char *text, u32 color, int maxlines) {
    char buf[2048];
    int n = (int)strlen(text);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, text, (size_t)n);
    buf[n] = 0;

    int cur = 0, start = 0, len = 0, lines = 0;
    while (lines < maxlines &&
           dv_wrap_next(buf, n, f, maxw, &cur, &start, &len, NULL)) {
        char saved = buf[start + len];
        buf[start + len] = 0;
        font_draw(s, f, (float)x, (float)(y + lines * lh), buf + start, color);
        buf[start + len] = saved;
        lines++;
    }
}

static void dv_draw_para_line(Viewer *v, Surface *s, const DvLine *l, int y) {
    const OfficeBlock *b = &v->doc.blocks[l->block];
    u32 color = C_TEXT;
    if (b->style == OFFICE_STYLE_H1 || b->style == OFFICE_STYLE_H2 ||
        b->style == OFFICE_STYLE_H3 || b->style == OFFICE_STYLE_TITLE)
        color = C_HEAD;
    else if (b->style == OFFICE_STYLE_QUOTE)
        color = C_DIM;

    if (l->first && b->style == OFFICE_STYLE_BULLET) {
        Font *fb = dv_font(OFFICE_STYLE_BODY, 0, 0);
        font_draw(s, fb, (float)(l->x - ui_px(14)), (float)y, "\xE2\x80\xA2", C_ACCENT);
    }
    dv_draw_block(v, s, b, l->off, l->len, (float)l->x, (float)y, color);
}

static void dv_draw_raw_line(Viewer *v, Surface *s, const DvLine *l, int y) {
    char *base = v->doc.plain;
    if (!base) return;
    Font *f = font_get(FONT_MONO, 10.5f);
    if (!f) f = font_get(FONT_REGULAR, 11.0f);
    char saved = base[l->off + l->len];
    base[l->off + l->len] = 0;
    font_draw(s, f, (float)l->x, (float)y, base + l->off, C_TEXT);
    base[l->off + l->len] = saved;
}

static void dv_draw_table_row(Viewer *v, Surface *s, const DvLine *l, int y) {
    const OfficeBlock *b = &v->doc.blocks[l->block];
    DvTable local;
    const DvTable *g;
    if (l->table >= 0 && l->table < v->ntables) {
        g = &v->tables[l->table];
    } else {
        dv_table_geom(v, b, &local);
        g = &local;
    }
    int pad = ui_px(6);
    int vpad = ui_px(5);
    Font *fb = dv_font(OFFICE_STYLE_BODY, 0, 0);
    int lh = dv_line_h(fb);

    int any_bold = 0;
    for (int c = 0; c < g->n; c++)
        if (b->cell_bold && b->cell_bold[(size_t)l->row * (size_t)b->cols + (size_t)c])
            any_bold = 1;
    if (l->row == 0 && any_bold)
        surf_fill(s, dv_margin(), y, g->total, l->h, C_ALT);
    else if (l->row & 1)
        surf_fill(s, dv_margin(), y, g->total, l->h, RGB(0x11, 0x18, 0x24));

    int x = dv_margin();
    for (int c = 0; c < g->n; c++) {
        const char *text = office_cell_text(&v->doc, b, l->row, c);
        int bold = b->cell_bold ? b->cell_bold[(size_t)l->row * (size_t)b->cols + (size_t)c] : 0;
        Font *fc = dv_font(OFFICE_STYLE_BODY, bold, 0);
        int cw = g->w[c] - 2 * pad;
        if (cw < 2) cw = 2;

        char buf[1024];
        int n = (int)strlen(text);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        memcpy(buf, text, (size_t)n);
        buf[n] = 0;
        int cur = 0, start = 0, len = 0, line = 0;
        while (line * lh < l->h - 2 * vpad &&
               dv_wrap_next(buf, n, fc, cw, &cur, &start, &len, NULL)) {
            char saved = buf[start + len];
            buf[start + len] = 0;
            /* a cell gets its own surface so a long word cannot paint
             * over the next column */
            Surface cell = { .px = s->px + (size_t)(y + vpad + line * lh) * (size_t)s->stride +
                                   (size_t)(x + pad),
                             .w = cw, .h = l->h - 2 * vpad, .stride = s->stride };
            font_draw(&cell, fc, 0, 0, buf + start,
                      bold ? C_HEAD : C_TEXT);
            buf[start + len] = saved;
            line++;
        }

        x += g->w[c];
        surf_fill(s, x - 1, y, 1, l->h, C_RULE);
    }
    surf_fill(s, dv_margin(), y + l->h - 1, g->total, 1, C_RULE);
}

static void dv_draw_info(Viewer *v, Surface *body) {
    int pw = dv_min(v->w - 2 * ui_px(40), ui_px(560));
    int ph = dv_min(body->h - 2 * ui_px(14), ui_px(300));
    if (pw < ui_px(200) || ph < ui_px(120)) return;
    int px = (v->w - pw) / 2;
    int py = (body->h - ph) / 2;

    Surface p = { .px = body->px + (size_t)py * (size_t)body->stride + (size_t)px,
                  .w = pw, .h = ph, .stride = body->stride };
    surf_fill(&p, 0, 0, pw, ph, C_PANEL);
    surf_fill(&p, 0, 0, pw, 1, C_RULE);
    surf_fill(&p, 0, ph - 1, pw, 1, C_RULE);
    surf_fill(&p, 0, 0, 1, ph, C_RULE);
    surf_fill(&p, pw - 1, 0, 1, ph, C_RULE);

    Font *fh = dv_font(OFFICE_STYLE_H3, 0, 0);
    Font *fl = dv_font(OFFICE_STYLE_BODY, 0, 0);
    Font *fm = font_get(FONT_REGULAR, 10.0f);
    int x = ui_px(16);
    int wmax = pw - 2 * x;
    int y = ui_px(12);
    font_draw(&p, fh, (float)x, (float)y, "Document information", C_HEAD);
    y += dv_line_h(fh) + ui_px(4);

    char line[512];
    snprintf(line, sizeof(line), "File: %s", v->path[0] ? v->path : "(none)");
    dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_TEXT);
    y += dv_line_h(fl);

    snprintf(line, sizeof(line), "Format: %s", v->doc.format_name);
    dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_TEXT);
    y += dv_line_h(fl);

    if (v->doc.title[0]) {
        snprintf(line, sizeof(line), "Title: %s", v->doc.title);
        dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_TEXT);
        y += dv_line_h(fl);
    }
    if (v->doc.author[0]) {
        snprintf(line, sizeof(line), "Author: %s", v->doc.author);
        dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_TEXT);
        y += dv_line_h(fl);
    }

    int tables = 0;
    for (int i = 0; i < v->doc.nblocks; i++)
        if (v->doc.blocks[i].kind == OFFICE_BLOCK_TABLE) tables++;
    snprintf(line, sizeof(line), "Blocks: %d   Tables: %d   Sheets: %d",
             v->doc.nblocks, tables, v->doc.nsheets);
    dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_DIM);
    y += dv_line_h(fl) + ui_px(6);

    if (v->doc.message[0]) {
        font_draw(&p, fm, (float)x, (float)y, "What this reader does and does not read:",
                  C_ACCENT);
        y += dv_line_h(fm) + ui_px(2);
        int avail = ph - y - ui_px(28);
        int lines = avail / dv_line_h(fm);
        if (lines < 1) lines = 1;
        if (lines > 12) lines = 12;
        dv_draw_wrapped(&p, fm, x, y, wmax, dv_line_h(fm), v->doc.message, C_DIM, lines);
    }
    font_draw(&p, fm, (float)x, (float)(ph - dv_line_h(fm) - ui_px(6)),
              "i closes this panel", C_FAINT);
}

static void dv_draw_header(Viewer *v, Surface *s) {
    int x = dv_margin();
    Font *ft = font_get(FONT_SEMIBOLD, 14.5f);
    Font *fs = font_get(FONT_REGULAR, 10.0f);
    int right = ui_px(26);

    const char *title = v->loaded ? v->doc.title : "Document viewer";
    if (!title || !title[0]) title = v->path[0] ? v->path : "Document viewer";
    dv_draw_fit(s, ft, (float)x, (float)ui_px(8),
                (float)(v->w - 2 * x - right), title, C_HEAD);

    char sub[400];
    if (v->loaded) {
        if (v->doc.author[0])
            snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", v->doc.format_name, v->doc.author);
        else
            snprintf(sub, sizeof(sub), "%s", v->doc.format_name);
    } else {
        snprintf(sub, sizeof(sub), "No document loaded");
    }
    dv_draw_fit(s, fs, (float)x, (float)ui_px(31),
                (float)(v->w - 2 * x - right), sub, C_DIM);

    const char *mode = v->loaded ? (v->raw ? "RAW TEXT" : "STRUCTURED") : "";
    float mw = font_width(fs, mode);
    font_draw(s, fs, (float)(v->w - dv_margin() - mw), (float)ui_px(8), mode, C_ACCENT);

    surf_fill(s, 0, ui_px(58) - 1, v->w, 1, C_RULE);
}

static void dv_draw_footer(Viewer *v, Surface *s) {
    int y = v->h - ui_px(30);
    surf_fill(s, 0, y, v->w, 1, C_RULE);
    Font *f = font_get(FONT_REGULAR, 9.5f);
    if (!f) return;
    float tx = (float)dv_margin();
    float ty = (float)(y + ui_px(9));

    if (v->table_wide || v->cell_clipped) {
        font_draw(s, f, tx, ty, "wide table clipped  \xC2\xB7  r shows raw text", C_ACCENT);
    } else {
        font_draw(s, f, tx, ty,
                  "j/k or wheel scroll  \xC2\xB7  r raw text  \xC2\xB7  i info  \xC2\xB7  q quit",
                  C_FAINT);
    }

    int maxscroll = v->content_h - v->body_h;
    int pct = maxscroll > 0 ? (v->scroll * 100) / maxscroll : 0;
    char pos[32];
    snprintf(pos, sizeof(pos), "%d%%", pct);
    float pw = font_width(f, pos);
    font_draw(s, f, (float)(v->w - dv_margin()) - pw, ty, pos, C_DIM);

    if (maxscroll > 0) {
        int track_h = v->body_h - ui_px(8);
        int thumb_h = (int)((long)track_h * v->body_h / v->content_h);
        if (thumb_h < ui_px(20)) thumb_h = ui_px(20);
        if (thumb_h > track_h) thumb_h = track_h;
        int thumb_y = v->body_top + ui_px(4) +
                      (int)((long)(track_h - thumb_h) * v->scroll / maxscroll);
        surf_fill(s, v->w - ui_px(6), thumb_y, ui_px(3), thumb_h, RGB(0x39, 0x46, 0x60));
    }
}

static void dv_draw_message(Viewer *v, Surface *s, const char *heading,
                            const char *body, const char *sub, u32 accent) {
    int x = dv_margin();
    int y = ui_px(54);
    Font *fh = font_get(FONT_SEMIBOLD, 18.0f);
    Font *fb = font_get(FONT_REGULAR, 12.0f);
    Font *fs = font_get(FONT_REGULAR, 10.0f);
    if (!fh || !fb || !fs) return;

    dv_draw_fit(s, fh, (float)x, (float)y, (float)(v->w - 2 * x), heading, accent);
    y += dv_line_h(fh) + ui_px(10);
    dv_draw_wrapped(s, fb, x, y, v->w - 2 * x, dv_line_h(fb), body, C_TEXT, 16);
    if (sub) {
        int sy = v->h - ui_px(46);
        surf_fill(s, 0, sy - ui_px(8), v->w, 1, C_RULE);
        dv_draw_fit(s, fs, (float)x, (float)sy, (float)(v->w - 2 * x), sub, C_FAINT);
    }
}

static void dv_render(Viewer *v, MClient *c) {
    Surface s = { .px = c->px, .w = v->w, .h = v->h, .stride = c->stride };
    gfx_gradient(&s, 0, 0, v->w, v->h, C_BG_TOP, C_BG_BOT);

    if (!v->loaded) {
        char body[900];
        snprintf(body, sizeof(body),
                 "%s\n\nSupported files: .docx, .xlsx, .pptx, .odt, .ods, .odp, and the "
                 "legacy .doc/.xls/.ppt (detected and named, not decoded).\n\n"
                 "Looked in /root/Documents, /root, $HOME/Documents and $HOME. "
                 "Start docview with a path, or set MYSTIC_DOC=/path/to/file, to open one.",
                 v->err[0] ? v->err : "No document was named and none was found.");
        dv_draw_message(v, &s, "No Office document to show", body,
                        "q quits", C_ERR);
        return;
    }

    if (v->doc.nblocks == 0) {
        dv_draw_message(v, &s, v->doc.format_name, v->doc.message,
                        "q quits  \xC2\xB7  this window shows text only", C_ERR);
        return;
    }

    dv_draw_header(v, &s);

    int body_h = v->body_h;
    int body_top = v->body_top;
    if (body_h < 1) return;

    Surface body = { .px = c->px + (size_t)body_top * (size_t)c->stride,
                     .w = v->w, .h = body_h, .stride = c->stride };

    if (v->show_info) {
        dv_draw_info(v, &body);
        dv_draw_footer(v, &s);
        return;
    }

    for (int i = 0; i < v->nlines; i++) {
        const DvLine *l = &v->lines[i];
        int y = l->y - v->scroll;
        if (y + l->h <= 0) continue;
        if (y >= body_h) break;
        if (l->kind == IT_TEXT) {
            if (v->raw) dv_draw_raw_line(v, &body, l, y);
            else dv_draw_para_line(v, &body, l, y);
        } else if (l->kind == IT_ROW) {
            dv_draw_table_row(v, &body, l, y);
        }
    }

    dv_draw_footer(v, &s);
}

/* ----------------------------------------------------------- documents */

#ifndef DOCVIEW_HOST_TEST
static int dv_supported(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ext[] = {
        ".docx", ".xlsx", ".pptx", ".odt", ".ods", ".odp",
        ".doc", ".xls", ".ppt", ".rtf", NULL
    };
    for (int i = 0; ext[i]; i++) {
        size_t n = strlen(ext[i]);
        size_t m = strlen(dot);
        if (n != m) continue;
        int same = 1;
        for (size_t k = 0; k < n; k++)
            if (tolower((unsigned char)dot[k]) != ext[i][k]) { same = 0; break; }
        if (same) return 1;
    }
    return 0;
}

/* The first supported file in a directory, chosen by name so the same
 * boot shows the same document. Real directory listings, no fixtures. */
static int dv_find_in(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char best[256];
    best[0] = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!dv_supported(de->d_name)) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (best[0] == 0 || strcmp(de->d_name, best) < 0)
            snprintf(best, sizeof(best), "%s", de->d_name);
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, cap, "%s/%s", dir, best);
    return 1;
}

static int dv_resolve(const char *arg, char *out, size_t cap) {
    if (arg && arg[0]) {
        struct stat st;
        if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", arg);
            return 1;
        }
    }
    const char *home = getenv("HOME");
    char dirs[6][320];
    int n = 0;
    snprintf(dirs[n++], sizeof(dirs[0]), "/root/Documents");
    snprintf(dirs[n++], sizeof(dirs[0]), "/root");
    if (home && home[0]) {
        snprintf(dirs[n++], sizeof(dirs[0]), "%s/Documents", home);
        snprintf(dirs[n++], sizeof(dirs[0]), "%s", home);
    }
    for (int i = 0; i < n; i++)
        if (dv_find_in(dirs[i], out, cap)) return 1;
    out[0] = 0;
    return 0;
}

#endif /* DOCVIEW_HOST_TEST: the host renderer supplies its own document */

static void dv_free(Viewer *v) {
    if (!v) return;
    office_free(&v->doc);
    free(v->lines);
    v->lines = NULL;
}

/* --------------------------------------------------------------- main */

#ifndef DOCVIEW_HOST_TEST
int main(int argc, char **argv) {
    MClient c;
    if (mc_open(&c, "docview", "Document", 780, 620) < 0) {
        fprintf(stderr, "docview: cannot connect to the window server\n");
        return 1;
    }
    if (font_init("/usr/share/fonts") != 0)
        fprintf(stderr, "docview: no fonts found; text cannot be drawn\n");

    Viewer *v = calloc(1, sizeof(*v));
    if (!v) {
        fprintf(stderr, "docview: out of memory\n");
        usleep(1500 * 1000);
        mc_close(&c);
        return 1;
    }
    v->maxw = c.stride;
    v->maxh = c.h;
    v->w = c.w;
    v->h = c.h;

    const char *arg = argc > 1 ? argv[1] : getenv("MYSTIC_DOC");
    char path[512];
    if (dv_resolve(arg, path, sizeof(path))) {
        snprintf(v->path, sizeof(v->path), "%s", path);
        if (office_open(&v->doc, path, v->err, sizeof(v->err)) == 0)
            v->loaded = 1;
        else
            v->loaded = 0;
    } else {
        snprintf(v->err, sizeof(v->err),
                 "%s", arg && arg[0] ? arg : "");
        v->loaded = 0;
    }
    dv_layout(v);

    int dirty = 1;
    for (;;) {
        if (dirty) {
            dv_render(v, &c);
            mc_present(&c);
            dirty = 0;
        }

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
            if (ev.type == MC_CLOSE) { quit = 1; break; }

            if (ev.type == MC_TEXT) {
                for (const char *t = ev.text; *t; t++) {
                    switch (*t) {
                    case 'q': quit = 1; break;
                    case 'r':
                        if (v->loaded && v->doc.nblocks > 0) {
                            int maxs = v->content_h - v->body_h;
                            int ratio = maxs > 0 ? (v->scroll * 1000) / maxs : 0;
                            v->raw = !v->raw;
                            dv_layout(v);
                            maxs = v->content_h - v->body_h;
                            v->scroll = maxs > 0 ? maxs * ratio / 1000 : 0;
                            dv_clamp_scroll(v);
                        }
                        break;
                    case 'i':
                        if (v->loaded) v->show_info = !v->show_info;
                        break;
                    case 'j':
                        if (!v->show_info) dv_scroll_by(v, ui_px(DV_SCROLL_STEP));
                        break;
                    case 'k':
                        if (!v->show_info) dv_scroll_by(v, -ui_px(DV_SCROLL_STEP));
                        break;
                    case 'g':
                        v->scroll = 0;
                        break;
                    case 'G':
                        v->scroll = v->content_h;
                        dv_clamp_scroll(v);
                        break;
                    case ' ':
                        if (!v->show_info)
                            dv_scroll_by(v, (v->body_h * 9) / 10);
                        break;
                    case 'b':
                        if (!v->show_info)
                            dv_scroll_by(v, -(v->body_h * 9) / 10);
                        break;
                    default: break;
                    }
                    if (quit) break;
                }
                dirty = 1;
            } else if (ev.type == MC_KEY) {
                if (ev.press) {
                    int step = ui_px(DV_SCROLL_STEP);
                    switch (ev.key) {
                    case DV_KEY_DOWN:  if (!v->show_info) dv_scroll_by(v, step); break;
                    case DV_KEY_UP:    if (!v->show_info) dv_scroll_by(v, -step); break;
                    case DV_KEY_PAGEDN: if (!v->show_info) dv_scroll_by(v, (v->body_h * 9) / 10); break;
                    case DV_KEY_PAGEUP: if (!v->show_info) dv_scroll_by(v, -(v->body_h * 9) / 10); break;
                    case DV_KEY_HOME:  v->scroll = 0; break;
                    case DV_KEY_END:   v->scroll = v->content_h; dv_clamp_scroll(v); break;
                    case DV_KEY_ESC:
                        if (v->show_info) v->show_info = 0;
                        else quit = 1;
                        break;
                    default: break;
                    }
                }
                dirty = 1;
            } else if (ev.type == MC_WHEEL) {
                if (!v->show_info)
                    dv_scroll_by(v, -ev.wheel * ui_px(DV_SCROLL_STEP) * DV_WHEEL_LINES);
                dirty = 1;
            } else if (ev.type == MC_MOUSE) {
                int down = ev.buttons & 1;
                if (down && !(v->last_buttons & 1)) {
                    v->dragging = 1;
                    v->last_y = ev.y;
                } else if (!down) {
                    v->dragging = 0;
                } else if (v->dragging && ev.y != v->last_y) {
                    dv_scroll_by(v, v->last_y - ev.y);
                    v->last_y = ev.y;
                }
                v->last_buttons = ev.buttons;
                dirty = 1;
            } else if (ev.type == MC_RESIZE) {
                int nw = ev.w < v->maxw ? ev.w : v->maxw;
                int nh = ev.h < v->maxh ? ev.h : v->maxh;
                if (nw > 0 && nh > 0) {
                    int maxs = v->content_h - v->body_h;
                    int ratio = maxs > 0 ? (v->scroll * 1000) / maxs : 0;
                    v->w = nw;
                    v->h = nh;
                    dv_layout(v);
                    maxs = v->content_h - v->body_h;
                    v->scroll = maxs > 0 ? maxs * ratio / 1000 : 0;
                    dv_clamp_scroll(v);
                }
                dirty = 1;
            }
        }
        if (quit) break;
    }

    dv_free(v);
    free(v);
    mc_close(&c);
    return 0;
}
#endif /* DOCVIEW_HOST_TEST */
