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
 *
 *  PLAIN TEXT AND MARKDOWN ARE EDITABLE. With no file argument the
 *  window opens an empty buffer called "untitled" instead of hunting
 *  for a document to show; .txt/.md (and any file that decodes as
 *  UTF-8 text) can be typed into and saved with Ctrl+S. Office
 *  containers (.docx/.xlsx/.pptx/.odt) stay read-only: they are ZIP
 *  packages of XML, so editing one means rewriting the package, which
 *  this reader does not do. The footer says "read-only" on those.
 *
 *  Keys, editing:  arrows / Home / End move the caret (Up/Down keep a
 *         goal column), typing / Enter / Backspace / Delete edit,
 *         Tab inserts up to four spaces, Ctrl+S saves, Esc closes
 *         (twice when there are unsaved changes).
 *         Shift+arrows do NOT select: there is no selection yet, and
 *         the header says so rather than pretending.
 *  Mouse: the wheel scrolls; dragging the page moves it, never the caret.
 *
 *  The reader lives in src/office.c. It is compiled into this app
 *  by including it below: tools/mkapp.sh links a fixed object list,
 *  and office.c has no main and no global state, so this keeps the
 *  bundle buildable without changing the tool.
 * ============================================================ */
#ifdef DOCEDIT_TEST
/* tools/tests/docedit.c compiles this file with DOCEDIT_TEST defined: only
 * the editing core below is built - no window server, no fonts, no
 * filesystem - so the host test drives exactly the functions the app runs. */
#define DOCVIEW_HOST_TEST 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../office.h"
#else
#include "../mystic_client.h"
#include "../../mystic.h"
#include "../../office.c"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DOCVIEW_HOST_TEST
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif /* DOCEDIT_TEST */

/* ============================================================
 *  THE EDITING CORE
 *
 *  An editable document is one contiguous buffer of bytes with '\n'
 *  between its lines, and a caret that is a byte offset into that
 *  buffer. Every function in this section is a pure function of
 *  (buffer, position): no fonts, no window server, no filesystem, so
 *  tools/tests/docedit.c compiles this same code and asserts the exact
 *  resulting text. Wrapping and drawing need font metrics and live with
 *  the window code further down.
 *
 *  Laziness is not on offer: an edit is applied to the buffer and the
 *  layout is rebuilt from it. See dv_layout_edit for that choice.
 * ============================================================ */

/* A document, not a disk image. The cap is also what keeps a whole-buffer
 * re-layout affordable on every keystroke. */
#define DV_TEXT_MAX  (256 * 1024)
#define DV_TAB_WIDTH 4

typedef struct {
    char *p;      /* bytes, always NUL-terminated at p[n] */
    int   n;      /* bytes in use */
    int   cap;    /* bytes allocated */
} DvText;

static void dv_text_free(DvText *t)
{
    if (!t) return;
    free(t->p);
    t->p = NULL;
    t->n = 0;
    t->cap = 0;
}

/* Room for `need` bytes plus the NUL. 0 ok, -1 out of memory or too big. */
static int dv_text_reserve(DvText *t, int need)
{
    if (!t || need < 0 || need > DV_TEXT_MAX) return -1;
    if (need + 1 <= t->cap) return 0;
    int cap = t->cap > 0 ? t->cap : 256;
    while (cap < need + 1) cap *= 2;
    char *p = realloc(t->p, (size_t)cap);
    if (!p) return -1;
    t->p = p;
    t->cap = cap;
    return 0;
}

/* Replace the whole contents. */
static int dv_text_set(DvText *t, const char *src, int n)
{
    if (!t) return -1;
    if (n < 0) n = 0;
    if (dv_text_reserve(t, n) != 0) return -1;
    if (n > 0 && src) memcpy(t->p, src, (size_t)n);
    t->n = n;
    t->p[n] = 0;
    return 0;
}

/* Insert len bytes at `at` (clamped to the buffer). 0 ok, -1 refused. */
static int dv_text_insert(DvText *t, int at, const char *s, int len)
{
    if (!t || !s || len < 0) return -1;
    if (len == 0) return 0;
    if (t->n > DV_TEXT_MAX - len) return -1;
    if (at < 0) at = 0;
    if (at > t->n) at = t->n;
    if (dv_text_reserve(t, t->n + len) != 0) return -1;
    memmove(t->p + at + len, t->p + at, (size_t)(t->n - at));
    memcpy(t->p + at, s, (size_t)len);
    t->n += len;
    t->p[t->n] = 0;
    return 0;
}

/* Remove len bytes at `at`. 1 when something went, 0 when it was a no-op. */
static int dv_text_erase(DvText *t, int at, int len)
{
    if (!t || len <= 0) return 0;
    if (at < 0) at = 0;
    if (at > t->n) at = t->n;
    if (len > t->n - at) len = t->n - at;
    if (len <= 0) return 0;
    memmove(t->p + at, t->p + at + len, (size_t)(t->n - at - len));
    t->n -= len;
    t->p[t->n] = 0;
    return 1;
}

/* Physical lines: 1 for empty text, otherwise 1 + one per '\n'. */
static int dv_text_lines(const DvText *t)
{
    if (!t) return 0;
    int lines = 1;
    for (int i = 0; i < t->n; i++)
        if (t->p[i] == '\n') lines++;
    return lines;
}

/* ---- byte positions and UTF-8 boundaries ---- */

/* One code point to the left of pos. Never stops inside a sequence, so
 * deleting an accented letter deletes the whole letter, not its tail. */
static int dv_utf8_prev(const char *s, int pos)
{
    if (!s || pos <= 0) return 0;
    int i = pos - 1;
    int steps = 0;
    while (i > 0 && steps < 3 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
        steps++;
    }
    return i;
}

static int dv_utf8_next(const char *s, int n, int pos)
{
    if (!s || pos >= n) return n;
    int i = pos + 1;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

/* Start of the physical line holding pos. A pos on a '\n' answers with the
 * start of the line that newline ends, which is what the caret wants. */
static int dv_line_start(const DvText *t, int pos)
{
    if (!t || t->n <= 0) return 0;
    if (pos > t->n) pos = t->n;
    if (pos < 0) pos = 0;
    while (pos > 0 && t->p[pos - 1] != '\n') pos--;
    return pos;
}

/* The offset of the '\n' ending the line (or the buffer length). */
static int dv_line_end(const DvText *t, int pos)
{
    if (!t) return 0;
    if (pos > t->n) pos = t->n;
    if (pos < 0) pos = 0;
    while (pos < t->n && t->p[pos] != '\n') pos++;
    return pos;
}

/* ---- caret movement ---- */

static int dv_caret_left(const DvText *t, int caret)
{
    return t ? dv_utf8_prev(t->p, caret) : 0;
}

static int dv_caret_right(const DvText *t, int caret)
{
    return t ? dv_utf8_next(t->p, t->n, caret) : 0;
}

static int dv_caret_home(const DvText *t, int caret)
{
    return dv_line_start(t, caret);
}

static int dv_caret_end(const DvText *t, int caret)
{
    return t ? dv_line_end(t, caret) : 0;
}

/* Up and down remember the column they started from (goal, in bytes from the
 * line start) so a run of Down keys through short lines comes back out at the
 * column it went in at. */
static int dv_caret_up(const DvText *t, int caret, int *goal)
{
    if (!t) return 0;
    int ls = dv_line_start(t, caret);
    int col = (goal && *goal >= 0) ? *goal : caret - ls;
    if (goal) *goal = col;
    if (ls <= 0) return ls;
    int prev_end = ls - 1;                  /* the '\n' above */
    int prev_start = dv_line_start(t, prev_end);
    int pos = prev_start + col;
    if (pos > prev_end) pos = prev_end;
    if (pos < prev_start) pos = prev_start;
    while (pos > prev_start && pos < prev_end &&
           ((unsigned char)t->p[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static int dv_caret_down(const DvText *t, int caret, int *goal)
{
    if (!t) return 0;
    int ls = dv_line_start(t, caret);
    int le = dv_line_end(t, caret);
    int col = (goal && *goal >= 0) ? *goal : caret - ls;
    if (goal) *goal = col;
    if (le >= t->n) return caret;           /* last line */
    int next_start = le + 1;
    int next_end = dv_line_end(t, next_start);
    int pos = next_start + col;
    if (pos > next_end) pos = next_end;
    while (pos > next_start && pos < next_end &&
           ((unsigned char)t->p[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

/* ---- the edits themselves ---- */

/* Type a run of text at the caret; the caret ends up after it. */
static int dv_edit_insert(DvText *t, int *caret, const char *s, int len)
{
    if (!t) return 0;
    int at = caret ? *caret : 0;
    if (at < 0) at = 0;
    if (at > t->n) at = t->n;
    if (dv_text_insert(t, at, s, len) != 0) return 0;
    if (caret) *caret = at + len;
    return 1;
}

/* Enter: split the line at the caret. */
static int dv_edit_split(DvText *t, int *caret)
{
    return dv_edit_insert(t, caret, "\n", 1);
}

/* Join this line onto the one above: remove the '\n' just before the caret.
 * This is exactly what Backspace does at the start of a line. */
static int dv_edit_merge(DvText *t, int *caret)
{
    int at = caret ? *caret : 0;
    if (!t || at <= 0 || at > t->n) return 0;
    if (t->p[at - 1] != '\n') return 0;
    if (!dv_text_erase(t, at - 1, 1)) return 0;
    if (caret) *caret = at - 1;
    return 1;
}

/* Backspace: one code point before the caret, or a line join. */
static int dv_edit_backspace(DvText *t, int *caret)
{
    int at = caret ? *caret : 0;
    if (!t || at <= 0 || at > t->n) return 0;
    if (t->p[at - 1] == '\n') return dv_edit_merge(t, caret);
    int from = dv_utf8_prev(t->p, at);
    if (from >= at) from = at - 1;
    if (!dv_text_erase(t, from, at - from)) return 0;
    if (caret) *caret = from;
    return 1;
}

/* Delete: one code point after the caret, or a line join. The caret stays. */
static int dv_edit_delete(DvText *t, int *caret)
{
    int at = caret ? *caret : 0;
    if (!t || at < 0 || at >= t->n) return 0;
    int to = (t->p[at] == '\n') ? at + 1 : dv_utf8_next(t->p, t->n, at);
    if (to <= at) to = at + 1;
    if (to > t->n) to = t->n;
    return dv_text_erase(t, at, to - at);
}

/* How many spaces a Tab inserts: enough to reach the next stop on a
 * four-character grid. The font has no tab advance, so a literal '\t' would
 * be invisible; soft tabs are what a reader can see. */
static int dv_tab_spaces(const DvText *t, int caret)
{
    int ls = dv_line_start(t, caret);
    int col = caret - ls;
    int n = DV_TAB_WIDTH - (col % DV_TAB_WIDTH);
    if (n <= 0) n = DV_TAB_WIDTH;
    return n;
}

/* ---- wrapping one visual line ---- */

/* UTF-8 sequence length from its first byte, as the wrap needs it. */
static int dv_u8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* How wide a byte range is, in pixels. The caller supplies it so this file
 * can lay a line out without owning a font - tools/tests/docedit.c passes a
 * fixed width per character and asserts the exact break offsets. */
typedef float (*DvMeasure)(void *ud, char *s, int off, int len);

/* One visual line out of the physical line [ls, eol). Returns the offset just
 * past the last byte that fits in maxw - always strictly greater than ls for a
 * non-empty line, so a caller can never stall.
 *
 * Unlike the reader's paragraph wrapper this keeps every byte: leading and
 * repeated spaces are the user's text, not layout to be swallowed. A break is
 * preferred just after a space; a word wider than the body is cut on a UTF-8
 * boundary so no glyph is ever half drawn.
 *
 * The ranges of the visual lines therefore tile the physical line exactly,
 * with no byte owned by two lines and none skipped. The cost is that a space
 * which does not itself fit - "hello| world" with the break at the space -
 * begins the next line and draws as a few pixels of indent. The gain is that
 * every caret offset belongs to exactly one line, with no special case in
 * dv_draw_caret or in the caret-to-line search. */
static int dv_fit_span(char *s, int n, int ls, int eol, int maxw,
                       DvMeasure measure, void *ud)
{
    if (ls < 0) ls = 0;
    if (eol > n) eol = n;
    if (ls >= eol) return eol;
    if (!measure) return eol;

    float w = 0;
    int p = ls;
    while (p < eol) {
        int cl = dv_u8_len((unsigned char)s[p]);
        if (p + cl > eol) cl = eol - p;
        float cw = measure(ud, s, p, cl);
        if (w + cw > (float)maxw && p > ls) break;
        w += cw;
        p += cl;
    }
    if (p < eol) {
        int brk = -1;
        for (int q = p; q > ls; q--)
            if (s[q - 1] == ' ') { brk = q; break; }
        if (brk > ls) p = brk;
    }
    if (p <= ls) p = ls + 1;               /* one glyph wider than the body */
    if (p > eol) p = eol;
    return p;
}

/* ---- Markdown, only as far as it changes how a line looks ---- */

/* The style of one source line. The markers stay on screen because this is
 * the source you are editing; the face is what tells you what the line is. */
static int dv_md_line_style(const char *s, int len)
{
    int i = 0;
    if (!s) return OFFICE_STYLE_BODY;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= len) return OFFICE_STYLE_BODY;
    if (s[i] == '#') {
        int n = 0;
        while (i < len && s[i] == '#') { i++; n++; }
        if (n < 1 || n > 4) return OFFICE_STYLE_BODY;
        if (i < len && s[i] != ' ') return OFFICE_STYLE_BODY;
        if (n == 1) return OFFICE_STYLE_H1;
        if (n == 2) return OFFICE_STYLE_H2;
        return OFFICE_STYLE_H3;
    }
    if (s[i] == '>') return OFFICE_STYLE_QUOTE;
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') &&
        i + 1 < len && s[i + 1] == ' ')
        return OFFICE_STYLE_BULLET;
    return OFFICE_STYLE_BODY;
}

/* The title a Markdown file declares for itself: its leading '# ' line. */
static void dv_md_title(const DvText *t, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!t || t->n <= 0) return;
    int i = 0;
    while (i < t->n && t->p[i] == '#') i++;
    if (i < 1 || i > 3) return;
    if (i < t->n && t->p[i] != ' ') return;
    while (i < t->n && t->p[i] == ' ') i++;
    int k = 0;
    while (i < t->n && t->p[i] != '\n' && (size_t)k + 1 < cap) {
        out[k++] = t->p[i++];
    }
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '#')) k--;
    out[k] = 0;
}

/* ---- picking a name that does not exist yet ---- */

/* "does this path exist?" - the caller supplies it so the chooser can be
 * tested against a list instead of a filesystem. */
typedef int (*DvNameProbe)(void *ud, const char *path);

static int dv_join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    int need = (n > 0 && dir[n - 1] == '/')
                   ? snprintf(out, cap, "%s%s", dir, name)
                   : snprintf(out, cap, "%s/%s", dir, name);
    return (need < 0 || (size_t)need >= cap) ? -1 : 0;
}

/* base.ext, then base-2.ext, base-3.ext ... The first one the probe says is
 * free wins, so a save never overwrites an earlier one. */
static int dv_choose_save_name(DvNameProbe exists, void *ud,
                               const char *dir, const char *base,
                               const char *ext, char *out, size_t cap)
{
    if (!dir || !base || !ext || !out || cap == 0) return -1;
    out[0] = 0;
    for (int n = 1; n <= 9999; n++) {
        char name[128];
        if (n == 1) snprintf(name, sizeof(name), "%s%s", base, ext);
        else snprintf(name, sizeof(name), "%s-%d%s", base, n, ext);
        if (dv_join_path(out, cap, dir, name) != 0) return -1;
        if (!exists || !exists(ud, out)) return 0;
    }
    out[0] = 0;
    return -1;
}

/* Everything below needs the window server, the fonts and the filesystem;
 * tools/tests/docedit.c stops at the editing core above. */
#ifndef DOCEDIT_TEST

/* Linux input event codes, as the window server forwards them. */
#define DV_KEY_ESC      1
#define DV_KEY_Q       16
#define DV_KEY_S       31
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
#define DV_KEY_DELETE 111

/* ev.mods bits, as src/main.c sends them to a client window. */
#define DV_MOD_SHIFT 0x01
#define DV_MOD_CTRL  0x02
#define DV_MOD_ALT   0x04

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

    /* ---- the editable buffer (plain text and Markdown) ----
     * In editing mode this DvText IS the document: there is no second copy
     * of the text, and doc.text stays NULL. Office files keep using
     * doc.text/plain and stay read-only. */
    DvText etext;
    int editing;               /* the buffer can be typed into */
    int markdown;              /* style '#'/'-'/>' lines while editing */
    int caret;                 /* byte offset into etext */
    int goal_col;              /* column Up/Down aim for, -1 = derive */
    int caret_line;            /* visual line holding the caret */
    int dirty_doc;             /* edited since the last save */
    int quit_armed;            /* Esc seen once with unsaved changes */
    int status_err;            /* the footer note is an error */
    int had_path;              /* opened from a file: save back to it */
    char status[256];          /* "saved <path>", or why it failed */
    char save_path[512];       /* where Ctrl+S writes, empty until chosen */

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
static size_t dv_utf8_trim(const char *buf, size_t len) {
    if (len == 0) return 0;
    size_t i = len;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return len;
    unsigned char c = (unsigned char)buf[i - 1];
    int need = 1;
    if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) need = 3;
    else if (c >= 0xF0 && c <= 0xF4) need = 4;
    if (len - (i - 1) >= (size_t)need) return len;
    return i - 1;
}

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
        n = dv_utf8_trim(buf, n);
        buf[n] = 0;
        if (font_width(f, buf) + dots <= maxw) break;
        if (n == 0) break;
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

/* ---- the editable layout ----------------------------------------
 *
 * Re-layout policy: the WHOLE buffer, on every edit. Patching only the
 * changed region would mean recomputing the wrap of one physical line and
 * then shifting every later DvLine's y - the same work as laying out a
 * document this size, with a chance of leaving a stale line behind when a
 * word moves from one visual line to the next. Correctness first: a whole
 * pass cannot disagree with the buffer, and DV_TEXT_MAX keeps it small.
 */

/* Advance one visual line inside [ls, eol). Unlike the reader's paragraph
 * wrapper this keeps every byte: leading and repeated spaces are the user's
 * text, not layout to be swallowed. A break is preferred after a space; a
 * word wider than the body is cut on a UTF-8 boundary. */
/* The editor's measure: real font metrics for the line's own face. */
typedef struct { Font *f; } DvSpanFont;

static float dv_measure_font(void *ud, char *s, int off, int len)
{
    DvSpanFont *m = (DvSpanFont *)ud;
    return dv_width_mut(s, (uint32_t)off, (uint32_t)len, m->f);
}

/* Advance one visual line inside [ls, eol). */
static int dv_fit_line(char *s, int n, int ls, int eol, Font *f, int maxw)
{
    DvSpanFont m;
    m.f = f;
    return dv_fit_span(s, n, ls, eol, maxw, dv_measure_font, &m);
}

static void dv_layout_edit(Viewer *v) {
    if (!v->etext.p) return;
    int x = dv_margin();
    int maxw = v->w - x - ui_px(30);
    if (maxw < ui_px(40)) maxw = ui_px(40);

    int y = ui_px(6);
    int cur = 0;                            /* start of a physical line */
    v->caret_line = -1;

    for (;;) {
        int eol = cur;
        while (eol < v->etext.n && v->etext.p[eol] != '\n') eol++;

        int style = v->markdown ?
                    dv_md_line_style(v->etext.p + cur, eol - cur) : OFFICE_STYLE_BODY;
        Font *f = dv_font(style, 0, 0);
        int lh = dv_line_h(f);

        int at = cur;
        do {
            int le = dv_fit_line(v->etext.p, v->etext.n, at, eol, f, maxw);
            int idx = v->nlines;
            dv_add_line(v, IT_TEXT, -1, -1, -1, y, lh, x,
                        (uint32_t)at, (uint32_t)(le - at), style, 0, 0);
            if (v->oom) return;
            if (v->nlines == idx) return;
            /* the caret belongs to the last visual line that starts at or
             * before it, which also puts it after the final newline */
            if ((uint32_t)v->caret >= v->lines[idx].off) v->caret_line = idx;
            y += lh;
            at = le;
        } while (at < eol);

        if (cur >= v->etext.n) break;
        cur = eol + 1;
        if (cur > v->etext.n) break;
    }
    if (v->caret_line < 0 && v->nlines > 0) v->caret_line = 0;
    v->y = y + ui_px(12);
}

static void dv_scroll_by(Viewer *v, int px);
static void dv_clamp_scroll(Viewer *v);

/* Keep the caret's line on screen: that is the whole of "follow the typing". */
static void dv_caret_reveal(Viewer *v) {
    if (!v->editing || v->caret_line < 0 || v->caret_line >= v->nlines) return;
    const DvLine *l = &v->lines[v->caret_line];
    int top = l->y - v->scroll;
    if (top < 0) v->scroll = l->y;
    else if (top + l->h > v->body_h) v->scroll = l->y + l->h - v->body_h;
    dv_clamp_scroll(v);
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

    if (v->editing) {
        dv_layout_edit(v);
    } else if (v->loaded && v->doc.nblocks > 0) {
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
    /* A raw source line has no wrap width of its own; fit it to the body
     * band instead of letting it run under the scrollbar and out of the
     * window. */
    float maxw = (float)(v->w - l->x - ui_px(16));
    if (maxw < 4.0f) maxw = (float)v->w - (float)l->x;
    dv_draw_fit(s, f, (float)l->x, (float)y, maxw, base + l->off, C_TEXT);
    base[l->off + l->len] = saved;
}

/* One visual line of the editable buffer. The text is drawn exactly as it is
 * stored - markers and all - because this is the source you are editing. */
static void dv_draw_edit_line(Viewer *v, Surface *s, const DvLine *l, int y) {
    char *base = v->etext.p;
    if (!base) return;
    Font *f = dv_font(l->style, 0, 0);
    u32 color = C_TEXT;
    if (l->style == OFFICE_STYLE_TITLE || l->style == OFFICE_STYLE_H1 ||
        l->style == OFFICE_STYLE_H2 || l->style == OFFICE_STYLE_H3)
        color = C_HEAD;
    else if (l->style == OFFICE_STYLE_QUOTE)
        color = C_DIM;

    char saved = base[l->off + l->len];
    base[l->off + l->len] = 0;
    font_draw(s, f, (float)l->x, (float)y, base + l->off, color);
    base[l->off + l->len] = saved;
}

/* The caret is a bar measured from the same font metrics the line was drawn
 * with, so it sits between the glyphs it sits between in the buffer. */
static void dv_draw_caret(Viewer *v, Surface *s) {
    if (!v->editing || v->caret_line < 0 || v->caret_line >= v->nlines) return;
    const DvLine *l = &v->lines[v->caret_line];
    int y = l->y - v->scroll;
    if (y + l->h <= 0 || y >= v->body_h) return;
    Font *f = dv_font(l->style, 0, 0);
    float x = (float)l->x;
    uint32_t at = (uint32_t)v->caret;
    if (at > l->off) {
        uint32_t k = at - l->off;
        if (k > l->len) k = l->len;
        x += dv_width_mut(v->etext.p, l->off, k, f);
    }
    int h = dv_line_h(f) - ui_px(2);
    if (h < ui_px(8)) h = ui_px(8);
    surf_fill(s, (int)x, y + ui_px(1), dv_max(2, ui_px(2)), h, C_ACCENT);
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

    /* This panel is a read-only view of a read-only window: 'i' is a letter
     * you can type while editing, so it is not a command there. */
    snprintf(line, sizeof(line), "Blocks: %d   Tables: %d   Sheets: %d",
             v->doc.nblocks, tables, v->doc.nsheets);
    dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_DIM);
    y += dv_line_h(fl);
    snprintf(line, sizeof(line), "Editing: no - %s is shown read-only here",
             v->doc.format_name);
    dv_draw_fit(&p, fl, (float)x, (float)y, (float)wmax, line, C_ERR);
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

    const char *mode = v->editing ? "EDIT" :
                       (v->loaded ? (v->raw ? "READ-ONLY RAW" : "READ-ONLY") : "");
    float mw = font_width(fs, mode);
    float textw = (float)(v->w - 2 * x - right) - mw - ui_px(8);
    if (textw < ui_px(80)) textw = (float)(v->w - 2 * x - right);

    char title[280];
    const char *base;
    if (v->editing)
        base = v->doc.title[0] ? v->doc.title : "untitled";
    else if (v->loaded)
        base = v->doc.title[0] ? v->doc.title :
               (v->path[0] ? v->path : "Document viewer");
    else
        base = "Document viewer";
    snprintf(title, sizeof(title), "%s%s", base,
             (v->editing && v->dirty_doc) ? "  *" : "");
    dv_draw_fit(s, ft, (float)x, (float)ui_px(8), textw, title, C_HEAD);

    char sub[400];
    if (v->editing) {
        /* the header says what this window can and cannot do, including the
         * line-editing feature it does not have, and how big the document is */
        snprintf(sub, sizeof(sub),
                 "%s  \xC2\xB7  %d lines  \xC2\xB7  editing  \xC2\xB7  no shift-selection yet",
                 v->doc.format_name[0] ? v->doc.format_name : "Plain text",
                 dv_text_lines(&v->etext));
    } else if (v->loaded) {
        if (v->doc.author[0])
            snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s  \xC2\xB7  read-only",
                     v->doc.format_name, v->doc.author);
        else
            snprintf(sub, sizeof(sub), "%s  \xC2\xB7  read-only",
                     v->doc.format_name);
    } else {
        snprintf(sub, sizeof(sub), "No document loaded");
    }
    dv_draw_fit(s, fs, (float)x, (float)ui_px(31), textw, sub, C_DIM);

    font_draw(s, fs, (float)(v->w - dv_margin() - mw), (float)ui_px(8), mode,
              v->editing ? C_ACCENT : C_ERR);

    surf_fill(s, 0, ui_px(58) - 1, v->w, 1, C_RULE);
}

/* Right-aligned fit. For a path the tail is the informative part, so a
 * too-long string loses characters from the left, not the right. */
static void dv_draw_fit_tail(Surface *s, Font *f, float right, float y,
                             float maxw, const char *text, u32 color) {
    if (!f || !text || !text[0] || maxw < 8) return;
    float w = font_width(f, text);
    if (w <= maxw) {
        font_draw(s, f, right - w, y, text, color);
        return;
    }
    float dots = font_width(f, "..");
    const char *p = text;
    while (*p && font_width(f, p) + dots > maxw) {
        p++;
        while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
    }
    if (!*p) return;
    char buf[600];
    snprintf(buf, sizeof(buf), "..%s", p);
    w = font_width(f, buf);
    font_draw(s, f, right - w, y, buf, color);
}

static void dv_draw_footer(Viewer *v, Surface *s) {
    int y = v->h - ui_px(30);
    surf_fill(s, 0, y, v->w, 1, C_RULE);
    Font *f = font_get(FONT_REGULAR, 9.5f);
    if (!f) return;
    float tx = (float)dv_margin();
    float ty = (float)(y + ui_px(9));
    float hint_max = (float)(v->w - 2 * dv_margin() - ui_px(150));

    if (v->editing) {
        if (v->status[0])
            dv_draw_fit(s, f, tx, ty, hint_max, v->status,
                        v->status_err ? C_ERR : C_ACCENT);
        else
            dv_draw_fit(s, f, tx, ty, hint_max,
                        "type  \xC2\xB7  arrows  \xC2\xB7  Enter  \xC2\xB7  Ctrl+S saves  "
                        "\xC2\xB7  Esc quits", C_FAINT);
    } else if (v->table_wide || v->cell_clipped) {
        font_draw(s, f, tx, ty, "read-only", C_ERR);
        font_draw(s, f, tx + font_width(f, "read-only"), ty,
                  "  \xC2\xB7  wide table clipped  \xC2\xB7  r shows raw text", C_ACCENT);
    } else {
        if (v->loaded) {
            font_draw(s, f, tx, ty, "read-only", C_ERR);
            tx += font_width(f, "read-only");
            font_draw(s, f, tx, ty, "  \xC2\xB7  ", C_FAINT);
            tx += font_width(f, "  \xC2\xB7  ");
        }
        dv_draw_fit(s, f, tx, ty,
                    (float)(v->w - dv_margin() - ui_px(80)) - tx,
                    "j/k or wheel scroll  \xC2\xB7  r raw text  \xC2\xB7  i info  \xC2\xB7  q quit",
                    C_FAINT);
    }

    int maxscroll = v->content_h - v->body_h;
    int pct = maxscroll > 0 ? (v->scroll * 100) / maxscroll : 0;
    char pos[32];
    snprintf(pos, sizeof(pos), "%d%%", pct);
    float pw = font_width(f, pos);
    float rx = (float)(v->w - dv_margin()) - pw;
    font_draw(s, f, rx, ty, pos, C_DIM);

    if (v->editing) {
        /* the saved path lives here permanently, not just in the transient
         * "saved ..." note, so there is always an answer to "where is it?" */
        const char *path = v->save_path[0] ? v->save_path : "not saved yet";
        float avail = rx - tx - font_width(f, " ");
        if (avail > ui_px(60))
            dv_draw_fit_tail(s, f, rx - ui_px(24), ty, avail, path,
                             v->save_path[0] ? C_DIM : C_FAINT);
    }

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
                 "%s\n\n"
                 "Editable here: .txt and .md, and any file that decodes as UTF-8 "
                 "text (up to %d KiB). With no file argument this window opens an "
                 "empty untitled document instead of looking for one.\n\n"
                 "Read-only: .docx, .xlsx, .pptx, .odt, .ods and .odp, which are ZIP "
                 "packages of XML - editing one means rewriting the package, and "
                 "this reader does not do that - and the legacy .doc/.xls/.ppt, "
                 "which are OLE2 binaries it never decodes.",
                 v->err[0] ? v->err : "The file could not be opened.",
                 DV_TEXT_MAX / 1024);
        dv_draw_message(v, &s, "Nothing open in this window", body,
                        "q quits", C_ERR);
        return;
    }

    if (!v->editing && v->doc.nblocks == 0) {
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
            if (v->editing) dv_draw_edit_line(v, &body, l, y);
            else if (v->raw) dv_draw_raw_line(v, &body, l, y);
            else dv_draw_para_line(v, &body, l, y);
        } else if (l->kind == IT_ROW) {
            dv_draw_table_row(v, &body, l, y);
        }
    }
    if (v->editing) dv_draw_caret(v, &body);

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

/* A named file that exists and is a regular file. There is no directory
 * search any more: with no argument the window opens an empty document, which
 * is the whole point of being able to create one. */
static int dv_openable(const char *arg, char *out, size_t cap) {
    if (!arg || !arg[0]) return 0;
    struct stat st;
    if (stat(arg, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(out, cap, "%s", arg);
    return 1;
}

/* A byte stream is text when it has no NUL and decodes as UTF-8. Anything else
 * is refused by name instead of shown as mojibake. */
static int dv_is_text(const char *p, int n) {
    int i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        if (c == 0) return 0;
        if (c < 0x80) { i++; continue; }
        int need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        if (i + need >= n) return 0;
        for (int k = 1; k <= need; k++)
            if (((unsigned char)p[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}

static int dv_ext_is(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    size_t n = strlen(ext);
    if (strlen(dot) != n) return 0;
    for (size_t k = 0; k < n; k++)
        if (tolower((unsigned char)dot[k]) != ext[k]) return 0;
    return 1;
}

static int dv_md_path(const char *path) {
    return path && (dv_ext_is(path, ".md") || dv_ext_is(path, ".markdown"));
}

/* Load a text file into the editable buffer. Nothing is duplicated: the
 * DvText IS the document from here on. */
static int dv_load_text(Viewer *v, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "%s: cannot seek: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    long end = ftell(f);
    if (end < 0) {
        snprintf(err, errsz, "%s: cannot size: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if (end > DV_TEXT_MAX) {
        snprintf(err, errsz,
                 "%s is %ld bytes; editable documents are capped at %d KiB so "
                 "that a re-layout on every keystroke stays affordable",
                 path, end, DV_TEXT_MAX / 1024);
        fclose(f);
        return -1;
    }
    if (dv_text_reserve(&v->etext, (int)end) != 0) {
        snprintf(err, errsz, "%s: out of memory for %ld bytes", path, end);
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err, errsz, "%s: cannot rewind: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    size_t got = end ? fread(v->etext.p, 1, (size_t)end, f) : 0;
    int bad = ferror(f);
    fclose(f);
    if (got != (size_t)end) {
        snprintf(err, errsz, "%s: read %zu of %ld bytes%s", path, got, end,
                 bad ? " (I/O error)" : "");
        dv_text_free(&v->etext);
        return -1;
    }
    v->etext.p[got] = 0;
    v->etext.n = (int)got;
    if (!dv_is_text(v->etext.p, v->etext.n)) {
        dv_text_free(&v->etext);
        snprintf(err, errsz,
                 "%s is not UTF-8 text (it contains NUL or invalid bytes); only "
                 "plain text and Markdown are editable here", path);
        return -1;
    }
    return 0;
}

/* Fill in what the header, footer and info panel read. `path` may be NULL for
 * a brand new document, which has no path until the first save. */
static void dv_set_text_meta(Viewer *v, const char *path) {
    v->editing = 1;
    v->markdown = dv_md_path(path);
    snprintf(v->doc.format_name, sizeof(v->doc.format_name), "%s",
             v->markdown ? "Markdown" : "Plain text");
    v->doc.fmt = OFFICE_FMT_UNKNOWN;
    v->path[0] = 0;
    v->save_path[0] = 0;
    v->had_path = 0;

    if (path && path[0]) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(v->doc.title, sizeof(v->doc.title), "%s", base);
        snprintf(v->path, sizeof(v->path), "%s", path);
        snprintf(v->save_path, sizeof(v->save_path), "%s", path);
        v->had_path = 1;
    } else {
        snprintf(v->doc.title, sizeof(v->doc.title), "untitled");
    }
    if (v->markdown) {
        char md[192];
        dv_md_title(&v->etext, md, sizeof(md));
        if (md[0]) snprintf(v->doc.title, sizeof(v->doc.title), "%s", md);
    }
    v->caret = 0;
    v->goal_col = -1;
    v->loaded = 1;
}

static void dv_new_text(Viewer *v) {
    dv_text_free(&v->etext);
    dv_text_set(&v->etext, "", 0);
    dv_set_text_meta(v, NULL);
}

/* ---- saving ---- */

#define DV_SAVE_DIR "/root/Documents"

static int dv_probe_file(void *ud, const char *path) {
    (void)ud;
    struct stat st;
    return stat(path, &st) == 0;
}

static int dv_ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

/* Write the buffer to v->save_path, choosing a fresh name under
 * /root/Documents the first time an untitled document is saved. The write goes
 * to a temporary file that is renamed over the target, so a full disk leaves
 * the previous version of the file intact. */
static int dv_save(Viewer *v) {
    if (!v->editing) return -1;
    v->status_err = 0;

    if (!v->save_path[0]) {
        /* a new document: /root/Documents, and a name nothing else has */
        if (dv_ensure_dir(DV_SAVE_DIR) != 0) {
            snprintf(v->status, sizeof(v->status), "cannot create %s: %s",
                     DV_SAVE_DIR, strerror(errno));
            v->status_err = 1;
            fprintf(stderr, "docview: %s\n", v->status);
            return -1;
        }
        if (dv_choose_save_name(dv_probe_file, NULL, DV_SAVE_DIR, "untitled",
                                v->markdown ? ".md" : ".txt",
                                v->save_path, sizeof(v->save_path)) != 0) {
            snprintf(v->status, sizeof(v->status),
                     "no free name in %s (untitled.txt .. untitled-9999.txt all exist)",
                     DV_SAVE_DIR);
            v->status_err = 1;
            return -1;
        }
    } else {
        /* An opened file saves where it came from, so only ITS directory has
         * to exist. Creating /root/Documents here would make saving
         * /root/notes.txt fail whenever /root was read-only. */
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", v->save_path);
        char *slash = strrchr(dir, '/');
        if (!slash) snprintf(dir, sizeof(dir), ".");
        else if (slash == dir) dir[1] = 0;
        else *slash = 0;
        if (dv_ensure_dir(dir) != 0) {
            snprintf(v->status, sizeof(v->status), "cannot use %s: %s",
                     dir, strerror(errno));
            v->status_err = 1;
            fprintf(stderr, "docview: %s\n", v->status);
            return -1;
        }
    }

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", v->save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(v->status, sizeof(v->status), "cannot write %s: %s",
                 tmp, strerror(errno));
        v->status_err = 1;
        fprintf(stderr, "docview: %s\n", v->status);
        return -1;
    }
    size_t want = (size_t)v->etext.n;
    size_t wrote = want ? fwrite(v->etext.p, 1, want, f) : 0;
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad || wrote != want) {
        snprintf(v->status, sizeof(v->status), "save failed: %s", strerror(errno));
        v->status_err = 1;
        unlink(tmp);
        fprintf(stderr, "docview: %s\n", v->status);
        return -1;
    }
    if (rename(tmp, v->save_path) != 0) {
        snprintf(v->status, sizeof(v->status), "cannot replace %s: %s",
                 v->save_path, strerror(errno));
        v->status_err = 1;
        unlink(tmp);
        fprintf(stderr, "docview: %s\n", v->status);
        return -1;
    }

    v->dirty_doc = 0;
    v->quit_armed = 0;
    if (!v->had_path) {
        /* the document now has a name: the header stops saying "untitled" */
        const char *base = strrchr(v->save_path, '/');
        base = base ? base + 1 : v->save_path;
        snprintf(v->doc.title, sizeof(v->doc.title), "%s", base);
        snprintf(v->path, sizeof(v->path), "%s", v->save_path);
        v->had_path = 1;
    }
    snprintf(v->status, sizeof(v->status), "saved %s (%d bytes)",
             v->save_path, v->etext.n);
    fprintf(stderr, "docview: saved %s\n", v->save_path);
    return 0;
}

#endif /* DOCVIEW_HOST_TEST: the host renderer supplies its own document */

static void dv_free(Viewer *v) {
    if (!v) return;
    office_free(&v->doc);
    dv_text_free(&v->etext);
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

    /* argv[1] then MYSTIC_DOC name the file to open. With neither, this is a
     * new document: an empty untitled buffer you can type into and save. The
     * old behaviour - search four directories for something to show - is gone,
     * because it made "start a new document" impossible. */
    const char *want = (argc > 1 && argv[1][0]) ? argv[1] : getenv("MYSTIC_DOC");
    char path[512];
    if (!want || !want[0]) {
        dv_new_text(v);
    } else if (!dv_openable(want, path, sizeof(path))) {
        snprintf(v->err, sizeof(v->err), "%s: no such file", want);
        v->loaded = 0;
    } else if (dv_supported(path)) {
        /* an Office container: read-only, exactly as before */
        snprintf(v->path, sizeof(v->path), "%s", path);
        v->loaded = office_open(&v->doc, path, v->err, sizeof(v->err)) == 0;
    } else {
        /* anything else is offered as text if its bytes decode as text */
        snprintf(v->path, sizeof(v->path), "%s", path);
        if (dv_load_text(v, path, v->err, sizeof(v->err)) == 0)
            dv_set_text_meta(v, path);
        else
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
        int edited = 0;      /* the buffer changed: re-lay out once, below */
        int saved = 0;       /* Ctrl+S arrives as a key AND as 0x13: save once */
        while (mc_poll(&c, &ev)) {
            if (ev.type == MC_CLOSE) {
                /* the title-bar close is the same as Esc: do not drop unsaved
                 * text on the floor without saying so first */
                if (v->editing && v->dirty_doc && !v->quit_armed) {
                    v->quit_armed = 1;
                    v->status_err = 1;
                    snprintf(v->status, sizeof(v->status),
                             "unsaved changes - Ctrl+S saves, close again to discard them");
                    dirty = 1;
                    continue;
                }
                quit = 1;
                break;
            }

            if (ev.type == MC_TEXT) {
                for (const char *t = ev.text; *t; t++) {
                    unsigned char ch = (unsigned char)*t;

                    if (v->editing) {
                        /* Ctrl+S reaches us as 0x13 as well as a key event;
                         * either path saves, and a save is never typed. */
                        if (ch == 0x13) {
                            if (!saved) { dv_save(v); saved = 1; }
                        } else if (ch == '\n' || ch == '\r') {
                            if (dv_edit_split(&v->etext, &v->caret)) edited = 1;
                            v->goal_col = -1;
                        } else if (ch == '\b') {
                            if (dv_edit_backspace(&v->etext, &v->caret)) edited = 1;
                            v->goal_col = -1;
                        } else if (ch == '\t') {
                            int n = dv_tab_spaces(&v->etext, v->caret);
                            for (int k = 0; k < n; k++)
                                if (dv_edit_insert(&v->etext, &v->caret, " ", 1))
                                    edited = 1;
                            v->goal_col = -1;
                        } else if (ch >= 32 && ch != 127) {
                            if (dv_edit_insert(&v->etext, &v->caret,
                                               (const char *)t, 1))
                                edited = 1;
                            v->goal_col = -1;
                        }
                        /* other control bytes are not text */
                        continue;
                    }

                    switch (ch) {
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

                    if (v->editing) {
                        int ctrl = (ev.mods & DV_MOD_CTRL) != 0;
                        switch (ev.key) {
                        case DV_KEY_S:
                            if (ctrl && !saved) { dv_save(v); saved = 1; }
                            break;
                        case DV_KEY_LEFT:
                            if (!ctrl) { v->caret = dv_caret_left(&v->etext, v->caret);
                                         v->goal_col = -1; }
                            break;
                        case DV_KEY_RIGHT:
                            if (!ctrl) { v->caret = dv_caret_right(&v->etext, v->caret);
                                         v->goal_col = -1; }
                            break;
                        case DV_KEY_UP:
                            v->caret = dv_caret_up(&v->etext, v->caret, &v->goal_col);
                            break;
                        case DV_KEY_DOWN:
                            v->caret = dv_caret_down(&v->etext, v->caret, &v->goal_col);
                            break;
                        case DV_KEY_HOME:
                            v->caret = dv_caret_home(&v->etext, v->caret);
                            v->goal_col = -1;
                            break;
                        case DV_KEY_END:
                            v->caret = dv_caret_end(&v->etext, v->caret);
                            v->goal_col = -1;
                            break;
                        case DV_KEY_DELETE:
                            if (dv_edit_delete(&v->etext, &v->caret)) edited = 1;
                            v->goal_col = -1;
                            break;
                        case DV_KEY_PAGEDN:
                            dv_scroll_by(v, (v->body_h * 9) / 10);
                            break;
                        case DV_KEY_PAGEUP:
                            dv_scroll_by(v, -(v->body_h * 9) / 10);
                            break;
                        case DV_KEY_ESC:
                            if (v->dirty_doc && !v->quit_armed) {
                                v->quit_armed = 1;
                                v->status_err = 1;
                                snprintf(v->status, sizeof(v->status),
                                         "unsaved changes - Ctrl+S saves, Esc again discards them");
                            } else {
                                quit = 1;
                            }
                            break;
                        default: break;
                        }
                    } else {
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
                    if (v->editing) dv_caret_reveal(v);
                }
                dirty = 1;
            }
        }
        if (edited) {
            /* one whole-buffer re-layout per frame, not per character */
            dv_layout(v);
            dv_caret_reveal(v);
            v->dirty_doc = 1;
            v->quit_armed = 0;
            v->status[0] = 0;   /* the "saved ..." note is stale once you type */
            v->status_err = 0;
            dirty = 1;
        }
        if (quit) break;
    }

    dv_free(v);
    free(v);
    mc_close(&c);
    return 0;
}
#endif /* DOCVIEW_HOST_TEST */

#endif /* !DOCEDIT_TEST */
