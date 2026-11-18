/* ============================================================
 *  Mystic OS - apps/notes.c
 *
 *  Notes is a real multi-document editor, not one buffer over one
 *  file. Every document is a regular file in /root/notes, and every
 *  list, size, timestamp and save state on screen comes from a
 *  syscall on that directory - nothing is cached from a previous
 *  boot and nothing is invented.
 *
 *  WHAT IT DOES, AND THE CALLS BEHIND IT
 *
 *    list        mkdir("/root/notes", 0700) once, then opendir(),
 *                readdir(), lstat() per entry; only S_ISREG files
 *                are listed, so a symlink can never point the editor
 *                outside the notes directory. Sizes are st_size and
 *                stamps st_mtime.
 *    open        open(O_RDONLY) + read() into the fixed 64 KB buffer.
 *                st_size from stat() tells a complete load from one
 *                that hit the cap.
 *    create      open(path, O_WRONLY|O_CREAT|O_EXCL, 0600).
 *    delete      unlink(path), after an explicit in-app confirmation.
 *    rename      rename(old, new), after saving unsaved edits, and only
 *                when lstat() says the target name is free - rename(2)
 *                would otherwise replace another note silently.
 *    save        open(path.tmp, O_WRONLY|O_CREAT|O_TRUNC), write(),
 *                fsync(), close(), rename() over path - and fsync of
 *                the directory so the rename is durable. A failure at
 *                any step leaves the previous file intact and the
 *                buffer dirty, with the kernel's strerror(errno) on
 *                screen.
 *    autosave    the same save, four seconds after the buffer first
 *                became dirty, run from draw() (main.c repaints at
 *                least once a second for the taskbar clock). After a
 *                save the footer shows "saved HH:MM:SS" from
 *                localtime_r(time(NULL)).
 *    search      opendir()/readdir() again, then open()+read() each
 *                note into a second fixed buffer and a case-folding
 *                substring scan. The note being edited is scanned from
 *                the live buffer, so unsaved text is searched too. The
 *                filter box highlights the match inside each result,
 *                and the same needle is painted behind the matching
 *                bytes in the editor.
 *    find/repl   a scan of the in-memory buffer; the replacement is a
 *                memmove inside the same 64 KB block, never a new
 *                allocation. Ctrl+R replaces every occurrence it can
 *                fit and reports the real count.
 *    counts      words/chars/lines computed from the buffer on every
 *                frame (chars counts UTF-8 code points, not continuation
 *                bytes). The painter decodes the same sequences, so a
 *                multi-byte glyph is drawn, measured and wrapped as one
 *                glyph; the caret itself stays byte-addressed.
 *    preview     Ctrl+T renders the buffer as markdown-ish text:
 *                headings, bullets, numbered lists, quotes, fences,
 *                **bold**, `code` and [links](...) are parsed out of
 *                the real bytes; nothing is a canned stub.
 *
 *  The editor keeps the caret behaviour that was verified: the caret
 *  is painted after every glyph, one ink-box tall, on the glyph pen -
 *  the x the next glyph is drawn from, not over its ink. All geometry -
 *  margin, gutter, gap, row height, sidebar, buttons, footer - is
 *  derived from the fonts and ui_px() once per frame in
 *  layout_build()/grid_of(), so the whole app grows and shrinks with the
 *  global text scale and the click handler, the painter and the scroll
 *  window cannot drift.
 *
 *  DISK METADATA CACHE. The stat() of the open document and the stat()
 *  of each visible sidebar row are real reads, but a file's size and
 *  mtime cannot change unless this app or another process writes it, so
 *  re-reading them on every frame only measures how often the compositor
 *  redraws. They are refreshed at most once a second (NOTES_STAT_TTL),
 *  and immediately when something here does change the disk or the set
 *  of rows on screen: a save, a document open, a create/rename/delete
 *  (docs_scan), a filter change (search_all) or a scroll that reveals
 *  new rows marks the cache stale. A resize does not - the window size
 *  cannot change a file, and the desktop calls resize() on every mouse
 *  move while an edge is dragged. Between refreshes the list shows the
 *  sizes from the last real stat of those rows, so a value is never
 *  invented and never older than the sweep that last covered it.
 *
 *  The wheel scrolls the list when the pointer is over the sidebar (the
 *  last pointer position the desktop reported) and the text otherwise;
 *  the list view keeps its selection on screen instead of running off.
 *
 *  The surface the desktop hands an app is the whole screen, with the
 *  window clip in screen coordinates. draw() rebases both into window
 *  space before painting: copying the surface alone would clip the
 *  top-left of every window that is not at 0,0.
 *
 *  There is no unbounded allocation: the two 64 KB blocks are fixed,
 *  the document table is capped at MAX_DOCS and the result table at
 *  MAX_HITS. A file larger than the buffer is never silently
 *  truncated on disk: autosave is off for it and Ctrl+S asks first,
 *  with the real byte counts.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NOTES_DIR     "/root/notes"
#define NOTES_LEGACY  "/root/notes.txt"
#define NOTES_MAX     65536
#define NAME_LEN      96
#define PATH_LEN      512
#define MAX_DOCS      128
#define MAX_HITS      160
#define SNIP_LEN      120
#define AUTOSAVE_SECS 4
#define NOTES_STAT_TTL 1.0  /* seconds a stat() result is reused; a file's
                             * size/mtime only changes when someone writes it,
                             * so per-frame stats measure redraws, not truth */
#define MD_TOK_MAX    192
#define ERR_COLOR     0xFFFF7B72u
#define OK_COLOR      0xFF7ED6A5u
#define LINE_SEP(c)   ((c) == ' ' || (c) == '\t' || (c) == '\n' || \
                       (c) == '\r' || (c) == '\f' || (c) == '\v')

enum { FOCUS_EDIT = 0, FOCUS_LIST, FOCUS_FILTER };
enum { PROMPT_NONE = 0, PROMPT_NEW, PROMPT_RENAME, PROMPT_DELETE };

typedef struct {
    char   name[NAME_LEN];
    long   size;
    time_t mtime;
    int    cur;        /* caret byte offset when last open          */
    int    top;        /* first line when last open                 */
    int    top_row;    /* first wrapped row inside top              */
    int    pv_off;     /* preview scroll when last open             */
} Doc;

typedef struct {
    char   file[NAME_LEN];
    long   off;        /* byte offset of the match in that file     */
    int    len;        /* needle length                             */
    int    line;       /* 1-based line number of the match          */
    int    snip_off;   /* match offset inside snip, -1 when clipped */
    int    snip_len;
    char   snip[SNIP_LEN];
} Hit;

/* The disk-metadata cache: one monotonic timestamp and one flag, not a copy
 * of the data - the sizes stay in Doc, where the list already reads them. */
typedef struct {
    double at;      /* CLOCK_MONOTONIC of the last stat sweep, 0 = none */
    int    stale;   /* something in this app changed the disk / the view */
} NotesFsCache;

typedef struct {
    char  *buf;                 /* fixed 64 KB: the open document     */
    char  *scan;                /* fixed 64 KB: search / replace work */
    int    len, cur;
    int    w, h;
    int    dirty;
    NotesFsCache fsc;
    int    last_dtop, last_htop; /* first visible rows of the last draw      */
    int    saved_here;
    time_t saved_at;
    time_t disk_mtime;
    long   disk_len;            /* st_size, -1 when absent            */
    int    top, top_row;
    int    fitted_w, fitted_h;
    int    pcol;
    char   err[200];            /* last load/save failure, real text  */
    int    err_no;
    int    err_is_save;
    int    partial;             /* buffer holds only part of the file */
    int    confirm;             /* warned once about overwriting      */

    Doc    docs[MAX_DOCS];
    int    ndocs, docs_more;
    int    dsel, dtop;
    char   dir_err[200];
    char   path[PATH_LEN];
    int    cur_doc;             /* index into docs, -1 when none      */

    int    focus;
    char   filter[128];
    int    filter_len;
    Hit    hits[MAX_HITS];
    int    nhits, hits_more, hsel, htop;

    char   find[128];
    int    find_len;
    char   repl[128];
    int    repl_len;
    int    find_open;
    int    find_field;          /* 0 = find box, 1 = replace box      */

    int    prompt;
    int    prompt_del;
    char   prompt_buf[NAME_LEN];
    int    prompt_len;

    int    view_preview;
    int    pv_off;

    time_t dirty_since;
    time_t last_attempt;
    char   status[220];
    time_t status_at;
    int    status_err;

    int    esc, esc_param, esc_digits, esc_mod;

    /* Last pointer position the desktop reported with a click. The AppVT
     * wheel event carries no position, so this is the only honest way to
     * know which pane the wheel is over. */
    int    ptr_x, ptr_y, ptr_valid;
} Notes;

/* ------------------------------------------------------------
 *  Small helpers
 * ------------------------------------------------------------ */
static void ensure_visible(Notes *n, Font *fr, Font *fd);
static void prompt_delete(Notes *n);
static void search_all(Notes *n);

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float ink_h(Font *f) { return f ? font_height(f) / 1.4f : 0.0f; }

/* y font_draw() needs so the glyph ink is centred in a band. */
static float ink_y(Font *f, float band_top, float band_h) {
    float ink = ink_h(f);
    if (ink > band_h) ink = band_h;
    if (ink < 0.0f) ink = 0.0f;
    return band_top + (band_h - ink) * 0.5f;
}

static void clip_text(char *out, size_t cap, Font *f, const char *in, float maxw) {
    if (cap == 0) return;
    out[0] = 0;
    if (!f || !in || maxw <= 0.0f) return;
    if (font_width(f, in) <= maxw) {
        snprintf(out, cap, "%s", in);
        return;
    }
    size_t len = strlen(in);
    if (len > 200) len = 200;
    char tmp[224];
    while (len > 0) {
        len--;
        /* never cut inside a UTF-8 codepoint: the whole point of the
         * helper is that the result is still drawable text */
        while (len > 0 && ((unsigned char)in[len] & 0xC0) == 0x80) len--;
        snprintf(tmp, sizeof(tmp), "%.*s..", (int)len, in);
        if (font_width(f, tmp) <= maxw) {
            snprintf(out, cap, "%s", tmp);
            return;
        }
    }
}

static void draw_fit(Surface *s, Font *f, float x, float y, float maxw,
                     const char *text, u32 color) {
    char cut[224];
    clip_text(cut, sizeof(cut), f, text, maxw);
    font_draw(s, f, x, y, cut, color);
}

/* Draw a run of text clipped to maxw and return the pen it ended at. */
static float draw_run(Surface *s, Font *f, float x, float y, float maxw,
                      const char *text, u32 color) {
    char cut[224];
    clip_text(cut, sizeof(cut), f, text, maxw > 0.0f ? maxw : 0.0f);
    return font_draw(s, f, x, y, cut, color);
}

static void fmt_clock(time_t t, char *out, size_t cap) {
    struct tm tm;
    if (t != 0 && localtime_r(&t, &tm) && strftime(out, cap, "%H:%M:%S", &tm) > 0)
        return;
    snprintf(out, cap, "--:--:--");
}

static void fmt_size(long sz, char *out, size_t cap) {
    if (sz < 0)                  snprintf(out, cap, "-");
    else if (sz < 1024)          snprintf(out, cap, "%ld B", sz);
    else if (sz < 1024L * 1024)  snprintf(out, cap, "%ld.%ld K",
                                           sz / 1024, (sz % 1024) * 10 / 1024);
    else                         snprintf(out, cap, "%ld.%ld M",
                                           sz / (1024L * 1024),
                                           (sz % (1024L * 1024)) * 10 / (1024L * 1024));
}

/* ------------------------------------------------------------
 *  Lines in the fixed buffer
 * ------------------------------------------------------------ */
static int line_start_at(const Notes *n, int pos) {
    if (pos > n->len) pos = n->len;
    if (pos < 0) pos = 0;
    while (pos > 0 && n->buf[pos - 1] != '\n') pos--;
    return pos;
}

static int line_end_at(const Notes *n, int pos) {
    if (pos > n->len) pos = n->len;
    if (pos < 0) pos = 0;
    while (pos < n->len && n->buf[pos] != '\n') pos++;
    return pos;
}

static int line_index_at(const Notes *n, int pos) {
    if (pos > n->len) pos = n->len;
    if (pos < 0) pos = 0;
    int line = 0;
    for (int i = 0; i < pos; i++)
        if (n->buf[i] == '\n') line++;
    return line;
}

static int line_count(const Notes *n) {
    int lines = 1;
    for (int i = 0; i < n->len; i++)
        if (n->buf[i] == '\n') lines++;
    return lines;
}

static int line_start_of_index(const Notes *n, int line) {
    if (line <= 0) return 0;
    int pos = 0;
    while (line > 0) {
        pos = line_end_at(n, pos);
        if (pos < n->len) pos++;
        line--;
        if (pos >= n->len) return n->len;
    }
    return pos;
}

/* Copy one UTF-8 code point starting at i into out and return its byte
 * length (1 for a byte that is not part of a valid sequence, so bad bytes
 * still show up as themselves instead of vanishing). Drawing, measuring
 * and hit-testing all go through this, so a two-byte accent is one glyph
 * wide everywhere and wraps at the same place it is painted. */
static int utf8_seq(const Notes *n, int i, int end, char *out) {
    int need = 0;
    if (i < 0 || i >= n->len) { out[0] = 0; return 0; }
    unsigned char b = (unsigned char)n->buf[i];
    if (b >= 0xC2 && b <= 0xDF) need = 1;
    else if (b >= 0xE0 && b <= 0xEF) need = 2;
    else if (b >= 0xF0 && b <= 0xF4) need = 3;
    if (need > 0 && i + need < end) {
        for (int k = 1; k <= need; k++)
            if (((unsigned char)n->buf[i + k] & 0xC0) != 0x80) { need = 0; break; }
    } else {
        need = 0;
    }
    int len = need + 1;
    memcpy(out, n->buf + i, (size_t)len);
    out[len] = 0;
    return len;
}

static int rows_of_line(const Notes *n, Font *fr, int start, int end, float avail) {
    float px = 0.0f;
    int   rows = 1;
    char  one[8];
    if (end > n->len) end = n->len;
    for (int i = start; i < end; i++) {
        int gl = utf8_seq(n, i, end, one);
        float cw = font_width(fr, one);
        if (px + cw > avail && px > 0.0f) { px = 0.0f; rows++; }
        px += cw;
        i += gl - 1;
    }
    return rows;
}

/* Case folding that does not depend on the locale. */
static int fold_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (int)c + 32 : (int)c;
}

static int ci_match(const char *hay, int hlen, int at, const char *needle, int nlen) {
    if (!needle || nlen <= 0 || at < 0 || at + nlen > hlen) return 0;
    for (int i = 0; i < nlen; i++)
        if (fold_byte((unsigned char)hay[at + i]) != fold_byte((unsigned char)needle[i]))
            return 0;
    return 1;
}

/* ------------------------------------------------------------
 *  Frame geometry: one Layout and one Grid, shared by the painter,
 *  the click handler, the scroll window and the footer.
 * ------------------------------------------------------------ */
typedef struct {
    float ox, oy;       /* top-left of the text band (app-local)    */
    float w, h;
    float m;            /* page margin inside the editor pane       */
    float pad_t;        /* space above the first text row           */
    float num_w;        /* line-number column width                 */
    float gap;          /* gutter rule .. first glyph               */
    float text_x, text_r, avail;
    float lh;
    int   vis;
} Grid;

typedef struct {
    int w, h, pad, sbw;

    int hdr_y, hdr_h;
    int flt_x, flt_y, flt_w, flt_h;
    int cx_x, cx_w;
    int list_y, list_h, row_h, hit_h;
    int btn_y, btn_h;
    int bnew_x, bnew_w, bren_x, bren_w, bdel_x, bdel_w;
    int status_y, status_h, hint_y, hint_h;

    int ex, ew, eh, em;
    int find_h;
    int ff_x, ff_y, ff_w, ff_h;
    int rf_x, rf_y, rf_w, rf_h;
    int foot_h;
    int band_x, band_y, band_w, band_h;

    Grid g;
} Layout;

static void grid_of(const Notes *n, Font *fr, Font *fd, const Layout *L, Grid *g) {
    memset(g, 0, sizeof(*g));
    g->ox = (float)L->band_x;
    g->oy = (float)L->band_y;
    g->w  = (float)L->band_w;
    g->h  = (float)L->band_h;

    int lines = line_count(n);
    int digits = 1;
    while (lines >= 10) { lines /= 10; digits++; }
    if (digits < 2) digits = 2;

    float m = (float)ui_px(10);
    if ((float)L->band_w < 2.0f * m + (float)ui_px(40))
        m = (float)ui_px(4);
    g->m     = m;
    g->pad_t = (float)ui_px(6);
    g->gap   = (float)ui_px(8);
    g->lh    = font_height(fr) + (float)ui_px(4);
    g->num_w = (float)digits * font_width(fd, "0");
    g->text_x = g->ox + m + g->num_w + g->gap;
    g->text_r = g->ox + g->w - m;
    g->avail  = g->text_r - g->text_x;

    if (g->avail < (float)ui_px(28)) {   /* no room for a gutter */
        g->num_w  = 0.0f;
        g->gap    = 0.0f;
        g->text_x = g->ox + (float)ui_px(4);
        g->avail  = g->text_r - g->text_x;
    }
    if (g->avail < (float)ui_px(12))
        g->avail = (float)ui_px(12);

    g->vis = (int)((g->h - g->pad_t) / g->lh);
    if (g->vis < 1) g->vis = 1;
}

static void layout_build(Notes *n, Layout *L, Font *fr, Font *fd) {
    memset(L, 0, sizeof(*L));
    L->w = n->w;
    L->h = n->h;

    int pad = ui_px(8);
    L->pad = pad;

    int sbw = (n->w * 34) / 100;
    int minw = ui_px(132), maxw = ui_px(268);
    if (n->w < ui_px(420)) minw = ui_px(100);
    if (sbw < minw) sbw = minw;
    if (sbw > maxw) sbw = maxw;
    if (sbw > (n->w * 2) / 3) sbw = n->w / 2;
    if (sbw < ui_px(64)) sbw = ui_px(64);
    if (sbw > n->w - ui_px(64)) sbw = n->w - ui_px(64);
    if (sbw < 1) sbw = 1;
    L->sbw = sbw;

    int inner_x = pad;
    int inner_w = sbw - 2 * pad;
    if (inner_w < ui_px(40)) {
        inner_x = ui_px(3);
        inner_w = sbw - 2 * inner_x;
    }
    if (inner_w < 1) inner_w = 1;

    L->hdr_y = pad;
    L->hdr_h = (int)(font_height(fr) + 0.5f) + ui_px(4);

    L->flt_x = inner_x;
    L->flt_y = L->hdr_y + L->hdr_h;
    L->flt_w = inner_w;
    L->flt_h = (int)(font_height(fr) + 0.5f) + ui_px(10);
    if (L->flt_h < ui_px(18)) L->flt_h = ui_px(18);
    L->cx_w = ui_px(14);
    L->cx_x = L->flt_x + L->flt_w - L->cx_w - ui_px(2);

    L->row_h = (int)(font_height(fr) + 0.5f) + ui_px(6);
    L->hit_h = 2 * (int)(font_height(fd) + 0.5f) + ui_px(8);
    if (L->hit_h < ui_px(26)) L->hit_h = ui_px(26);

    int one = (int)(font_height(fd) + 0.5f);
    L->status_h = one;
    L->hint_h = 2 * one + ui_px(4);
    L->hint_y = n->h - pad - L->hint_h;
    L->status_y = L->hint_y - ui_px(2) - L->status_h;

    L->btn_h = one + ui_px(8);
    if (L->btn_h < ui_px(18)) L->btn_h = ui_px(18);
    L->btn_y = L->status_y - ui_px(4) - L->btn_h;

    int gap = ui_px(4);
    int bw = (inner_w - 2 * gap) / 3;
    if (bw < 1) bw = 1;
    L->bnew_x = inner_x;
    L->bnew_w = bw;
    L->bren_x = inner_x + bw + gap;
    L->bren_w = bw;
    L->bdel_x = inner_x + 2 * (bw + gap);
    L->bdel_w = inner_x + inner_w - L->bdel_x;
    if (L->bdel_w < 1) L->bdel_w = 1;

    L->list_y = L->flt_y + L->flt_h + ui_px(4);
    L->list_h = L->btn_y - ui_px(6) - L->list_y;
    if (L->list_h < 0) L->list_h = 0;

    L->ex = sbw;
    L->ew = n->w - sbw;
    if (L->ew < 1) L->ew = 1;
    L->eh = n->h;
    int em = ui_px(10);
    if (L->ew < em * 2 + ui_px(24)) em = ui_px(4);
    if (em > L->ew / 2) em = L->ew / 2;
    if (em < 1) em = 1;
    L->em = em;

    L->foot_h = 2 * one + ui_px(8);

    if (n->find_open) {
        int row = (int)(font_height(fr) + 0.5f) + ui_px(8);
        int lab_w1 = (int)font_width(fd, "find") + ui_px(6);
        int lab_w2 = (int)font_width(fd, "with") + ui_px(6);
        int avail = L->ew - 2 * em;
        int fy = ui_px(6);
        if (avail < ui_px(300)) {
            L->find_h = 2 * row + ui_px(4);
            L->ff_x = L->ex + em + lab_w1;
            L->ff_y = fy;
            L->ff_w = avail - lab_w1;
            L->ff_h = row;
            L->rf_x = L->ex + em + lab_w2;
            L->rf_y = fy + row + ui_px(4);
            L->rf_w = avail - lab_w2;
            L->rf_h = row;
        } else {
            L->find_h = row;
            int g2 = ui_px(10);
            int w1 = (avail - lab_w1 - lab_w2 - g2) / 2;
            if (w1 < ui_px(40)) w1 = ui_px(40);
            L->ff_x = L->ex + em + lab_w1;
            L->ff_y = fy;
            L->ff_w = w1;
            L->ff_h = row;
            L->rf_x = L->ff_x + w1 + g2 + lab_w2;
            L->rf_y = fy;
            L->rf_w = L->ex + L->ew - em - L->rf_x;
            if (L->rf_w < ui_px(40)) L->rf_w = ui_px(40);
            L->rf_h = row;
        }
        if (L->ff_w < 2) L->ff_w = 2;
        if (L->rf_w < 2) L->rf_w = 2;
    }

    L->band_x = L->ex + em;
    L->band_w = L->ew - 2 * em;
    if (L->band_w < ui_px(20)) L->band_w = ui_px(20);
    L->band_y = ui_px(6) + L->find_h + (n->find_open ? ui_px(4) : 0);
    L->band_h = L->eh - L->band_y - L->foot_h;
    if (L->band_h < 0) L->band_h = 0;

    grid_of(n, fr, fd, L, &L->g);
}

/* Number of document rows / hit rows the sidebar list can show. */
static int doc_rows_visible(const Layout *L) {
    int rows = L->row_h > 0 ? L->list_h / L->row_h : 0;
    return rows < 1 ? 1 : rows;
}
static int hit_rows_visible(const Layout *L) {
    int rows = L->hit_h > 0 ? L->list_h / L->hit_h : 0;
    return rows < 1 ? 1 : rows;
}

static void list_clamp(Notes *n, const Layout *L) {
    int rows = doc_rows_visible(L);
    if (n->ndocs < 1) { n->dsel = 0; n->dtop = 0; }
    else {
        n->dsel = clampi(n->dsel, 0, n->ndocs - 1);
        if (n->dtop > n->dsel) n->dtop = n->dsel;
        if (n->dsel >= n->dtop + rows) n->dtop = n->dsel - rows + 1;
        if (n->dtop > n->ndocs - rows) n->dtop = n->ndocs - rows;
        if (n->dtop < 0) n->dtop = 0;
    }
    int hrows = hit_rows_visible(L);
    if (n->nhits < 1) { n->hsel = 0; n->htop = 0; }
    else {
        n->hsel = clampi(n->hsel, 0, n->nhits - 1);
        if (n->htop > n->hsel) n->htop = n->hsel;
        if (n->hsel >= n->htop + hrows) n->htop = n->hsel - hrows + 1;
        if (n->htop > n->nhits - hrows) n->htop = n->nhits - hrows;
        if (n->htop < 0) n->htop = 0;
    }
}

/* ------------------------------------------------------------
 *  Status and error text
 * ------------------------------------------------------------ */
static void set_status(Notes *n, int is_err, const char *msg) {
    snprintf(n->status, sizeof(n->status), "%s", msg);
    n->status_at = time(NULL);
    n->status_err = is_err;
}

static void set_errno_status(Notes *n, const char *what, int e) {
    char b[220];
    snprintf(b, sizeof(b), "%s: %s (errno %d)", what, strerror(e), e);
    set_status(n, 1, b);
}

static void note_save_error(Notes *n, const char *op, int e) {
    snprintf(n->err, sizeof(n->err), "%s: %s (errno %d)", op, strerror(e), e);
    n->err_no = e;
    n->err_is_save = 1;
}

static void mark_dirty(Notes *n) {
    if (!n->dirty) n->dirty_since = time(NULL);
    n->dirty = 1;
    n->confirm = 0;
}

/* ------------------------------------------------------------
 *  File primitives
 * ------------------------------------------------------------ */
static int write_all(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) { errno = EIO; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* Read at most cap bytes. Returns bytes read, or -1 if open() failed
 * (errno is then still the open failure). A short read leaves *err set
 * to the read/close errno and still returns what was read. */
static int read_file(const char *path, char *dst, int cap, long *size_out, int *err) {
    *err = 0;
    if (size_out) *size_out = -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    int got = 0;
    while (got < cap) {
        ssize_t r = read(fd, dst + got, (size_t)(cap - got));
        if (r < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            break;
        }
        if (r == 0) break;
        got += (int)r;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && size_out) *size_out = (long)st.st_size;
    if (close(fd) != 0 && *err == 0) *err = errno;
    return got;
}

/* Copy a file byte for byte (used once, to import the old single-file
 * notes.txt into the new directory). Returns bytes copied or -1. */
static long copy_stream(const char *src, const char *dst, int *err) {
    *err = 0;
    int in = open(src, O_RDONLY);
    if (in < 0) { *err = errno; return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) { *err = errno; close(in); return -1; }

    char tmp[4096];
    long total = 0;
    for (;;) {
        ssize_t r = read(in, tmp, sizeof(tmp));
        if (r < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            break;
        }
        if (r == 0) break;
        if (write_all(out, tmp, (size_t)r) != 0) { *err = errno; break; }
        total += (long)r;
    }
    if (*err == 0 && fsync(out) != 0) *err = errno;
    if (close(out) != 0 && *err == 0) *err = errno;
    if (close(in) != 0 && *err == 0) *err = errno;
    if (*err != 0) { unlink(dst); return -1; }
    return total;
}

static int doc_path(const Notes *n, const char *name, char *out, size_t cap) {
    (void)n;
    int k = snprintf(out, cap, "%s/%s", NOTES_DIR, name);
    return k > 0 && (size_t)k < cap;
}

static int doc_index_of(const Notes *n, const char *name) {
    for (int i = 0; i < n->ndocs; i++)
        if (!strcmp(n->docs[i].name, name)) return i;
    return -1;
}

static int name_ok(const char *s) {
    size_t len = strlen(s);
    if (len < 1 || len >= NAME_LEN) return 0;
    if (s[0] == '.') return 0;
    if (!strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c < 32 || c == 127) return 0;
    }
    return 1;
}

static void default_note_name(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    char b[16];
    if (!localtime_r(&t, &tm) || strftime(b, sizeof(b), "%H%M%S", &tm) == 0)
        snprintf(b, sizeof(b), "%ld", (long)t);
    snprintf(out, cap, "note-%s.txt", b);
}

/* ------------------------------------------------------------
 *  Disk-metadata cache (see NOTES_STAT_TTL above)
 * ------------------------------------------------------------ */
static double notes_mono(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Ask for a stat sweep on the next frame. Called only by code that has just
 * changed the disk or the set of visible rows. */
static void docs_stat_stale(Notes *n) {
    n->fsc.stale = 1;
}

/* Returns 1 when this frame should stat. A failed clock refreshes rather
 * than trust an unbounded age; a backwards clock refreshes too. */
static int docs_stat_due(Notes *n) {
    double now = notes_mono();
    if (n->fsc.stale || n->fsc.at <= 0.0 || now <= 0.0 || now < n->fsc.at ||
        now - n->fsc.at >= NOTES_STAT_TTL) {
        n->fsc.at = now > 0.0 ? now : 0.0;
        n->fsc.stale = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------
 *  The document list: a real directory scan
 * ------------------------------------------------------------ */
static void docs_scan(Notes *n) {
    char keep[NAME_LEN];
    int  have = (n->cur_doc >= 0 && n->cur_doc < n->ndocs);
    /* The directory may have gained or lost files; the next draw must not
     * serve row sizes from before this scan. */
    docs_stat_stale(n);
    keep[0] = 0;
    if (have) snprintf(keep, sizeof(keep), "%s", n->docs[n->cur_doc].name);

    n->ndocs = 0;
    n->docs_more = 0;
    n->dir_err[0] = 0;

    DIR *d = opendir(NOTES_DIR);
    if (!d) {
        int e = errno;
        snprintf(n->dir_err, sizeof(n->dir_err), "%s: %s (errno %d)",
                 NOTES_DIR, strerror(e), e);
        n->cur_doc = -1;
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.') continue;
        if (strlen(de->d_name) >= NAME_LEN) continue;
        if (n->ndocs >= MAX_DOCS) { n->docs_more = 1; break; }

        char path[PATH_LEN];
        if (!doc_path(n, de->d_name, path, sizeof(path))) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;     /* never follow a link */

        Doc *doc = &n->docs[n->ndocs++];
        memset(doc, 0, sizeof(*doc));
        snprintf(doc->name, sizeof(doc->name), "%s", de->d_name);
        doc->size = (long)st.st_size;
        doc->mtime = st.st_mtime;
    }
    closedir(d);

    /* alphabetical, insertion sort: the table is tiny and this file
     * avoids qsort's function-pointer indirection */
    for (int i = 1; i < n->ndocs; i++) {
        Doc key = n->docs[i];
        int j = i - 1;
        while (j >= 0 && strcmp(n->docs[j].name, key.name) > 0) {
            n->docs[j + 1] = n->docs[j];
            j--;
        }
        n->docs[j + 1] = key;
    }

    n->cur_doc = have ? doc_index_of(n, keep) : -1;
    if (n->dsel >= n->ndocs) n->dsel = n->ndocs > 0 ? n->ndocs - 1 : 0;
    if (n->dsel < 0) n->dsel = 0;
}

/* ------------------------------------------------------------
 *  Loading and saving one document
 * ------------------------------------------------------------ */
static int doc_load(Notes *n, int idx) {
    char path[PATH_LEN];
    if (idx < 0 || idx >= n->ndocs) return 0;
    if (!doc_path(n, n->docs[idx].name, path, sizeof(path))) {
        snprintf(n->err, sizeof(n->err), "%s", "document path too long");
        n->err_is_save = 0;
        return 0;
    }

    int e = 0;
    long size = -1;
    int got = read_file(path, n->buf, NOTES_MAX - 1, &size, &e);
    if (got < 0) {
        int oe = errno;
        snprintf(n->err, sizeof(n->err), "open %s: %s (errno %d)",
                 path, strerror(oe), oe);
        n->err_no = oe;
        n->err_is_save = 0;
        return 0;
    }

    n->len = got;
    n->buf[n->len] = 0;
    n->err[0] = 0;
    n->err_no = 0;
    n->err_is_save = 0;
    n->partial = 0;
    n->confirm = 0;
    n->dirty = 0;
    n->saved_here = 0;

    struct stat st;
    if (stat(path, &st) == 0) {
        n->disk_len = (long)st.st_size;
        n->disk_mtime = st.st_mtime;
    } else {
        n->disk_len = size;
        n->disk_mtime = 0;
    }
    n->saved_at = n->disk_mtime;

    if (e != 0) {
        snprintf(n->err, sizeof(n->err), "read %s: %s (errno %d)",
                 path, strerror(e), e);
        n->err_no = e;
        n->err_is_save = 0;
        n->partial = 1;
        n->dirty = 1;
    } else if (size > got) {
        snprintf(n->err, sizeof(n->err),
                 "disk has %ld bytes, loaded %d - too big to autosave", size, got);
        n->partial = 1;
    }

    Doc *doc = &n->docs[idx];
    n->cur = clampi(doc->cur, 0, n->len);
    n->pcol = n->cur - line_start_at(n, n->cur);
    n->top = doc->top;
    n->top_row = doc->top_row;
    n->pv_off = doc->pv_off;
    if (n->top < 0) n->top = 0;
    if (n->top_row < 0) n->top_row = 0;
    if (n->pv_off < 0) n->pv_off = 0;
    n->find_open = 0;
    snprintf(n->path, sizeof(n->path), "%s", path);
    docs_stat_stale(n);       /* the list row for this file just changed */
    return 1;
}

static int doc_save(Notes *n, int autosave) {
    n->last_attempt = time(NULL);

    if (n->cur_doc < 0 || n->path[0] == 0) {
        if (!autosave) {
            snprintf(n->err, sizeof(n->err), "%s", "no document to save");
            n->err_is_save = 1;
            n->err_no = 0;
        }
        return 0;
    }

    /* A partial buffer must never overwrite the rest of the file by
     * accident. Autosave simply does nothing; Ctrl+S warns first. */
    if (n->partial) {
        if (autosave) return 0;
        struct stat st;
        if (stat(n->path, &st) == 0 && (long)st.st_size > (long)n->len) {
            if (!n->confirm) {
                n->confirm = 1;
                snprintf(n->err, sizeof(n->err),
                         "disk has %ld bytes, loaded %d - Ctrl+S again to overwrite",
                         (long)st.st_size, n->len);
                n->err_is_save = 1;
                n->err_no = 0;
                n->dirty = 1;
                return 0;
            }
        } else {
            n->partial = 0;
        }
    }

    char tmp[PATH_LEN + 8];
    int k = snprintf(tmp, sizeof(tmp), "%s.tmp", n->path);
    if (k < 0 || (size_t)k >= sizeof(tmp)) {
        snprintf(n->err, sizeof(n->err), "%s", "temp path too long");
        n->err_is_save = 1;
        n->err_no = 0;
        n->dirty = 1;
        return 0;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        note_save_error(n, "open temp", errno);
        n->dirty = 1;
        return 0;
    }
    if (n->len > 0 && write_all(fd, n->buf, (size_t)n->len) != 0) {
        int e = errno;
        note_save_error(n, "write temp", e);
        close(fd);
        unlink(tmp);
        n->dirty = 1;
        return 0;
    }
    if (fsync(fd) != 0) {
        int e = errno;
        note_save_error(n, "fsync temp", e);
        close(fd);
        unlink(tmp);
        n->dirty = 1;
        return 0;
    }
    if (close(fd) != 0) {
        int e = errno;
        note_save_error(n, "close temp", e);
        unlink(tmp);
        n->dirty = 1;
        return 0;
    }
    if (rename(tmp, n->path) != 0) {
        int e = errno;
        note_save_error(n, "rename", e);
        unlink(tmp);
        n->dirty = 1;
        return 0;
    }

    /* Make the rename durable too; a failure here does not undo a
     * successful rename and must not be reported as a data loss. */
    int dfd = open(NOTES_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    n->dirty = 0;
    n->partial = 0;
    n->confirm = 0;
    n->saved_here = 1;
    n->saved_at = time(NULL);
    n->err[0] = 0;
    n->err_no = 0;
    n->err_is_save = 0;

    struct stat st;
    if (stat(n->path, &st) == 0) {
        n->disk_len = (long)st.st_size;
        n->disk_mtime = st.st_mtime;
        if (n->cur_doc >= 0 && n->cur_doc < n->ndocs) {
            n->docs[n->cur_doc].size = (long)st.st_size;
            n->docs[n->cur_doc].mtime = st.st_mtime;
        }
    }
    docs_stat_stale(n);       /* a save rewrites size and mtime */
    return 1;
}

static void doc_stash_view(Notes *n) {
    if (n->cur_doc < 0 || n->cur_doc >= n->ndocs) return;
    Doc *d = &n->docs[n->cur_doc];
    d->cur = n->cur;
    d->top = n->top;
    d->top_row = n->top_row;
    d->pv_off = n->pv_off;
}

/* Save the current document, then load idx. A failed save keeps the
 * current buffer and the switch does not happen. */
static int doc_open(Notes *n, int idx) {
    if (idx < 0 || idx >= n->ndocs) return 0;
    if (idx == n->cur_doc) {
        n->dsel = idx;
        n->focus = FOCUS_EDIT;
        n->view_preview = 0;
        return 1;
    }

    if (n->cur_doc >= 0) {
        doc_stash_view(n);
        if (n->dirty && !doc_save(n, 0)) {
            set_status(n, 1, "switch cancelled: current note was not saved");
            return 0;
        }
    }
    if (!doc_load(n, idx)) {
        n->dsel = n->cur_doc >= 0 ? n->cur_doc : 0;
        return 0;
    }
    n->cur_doc = idx;
    n->dsel = idx;
    n->focus = FOCUS_EDIT;
    n->view_preview = 0;
    ensure_visible(n, font_get(FONT_REGULAR, 12), font_get(FONT_REGULAR, 10));
    return 1;
}

/* ------------------------------------------------------------
 *  Caret movement and the scroll window
 * ------------------------------------------------------------ */
static void note_pcol(Notes *n) {
    n->pcol = n->cur - line_start_at(n, n->cur);
}

static void caret_home_line(Notes *n) {
    n->cur = line_start_at(n, n->cur);
    n->pcol = 0;
}

static void caret_end_line(Notes *n) {
    n->cur = line_end_at(n, n->cur);
    note_pcol(n);
}

static void caret_left(Notes *n) {
    if (n->cur > 0) n->cur--;
    note_pcol(n);
}

static void caret_right(Notes *n) {
    if (n->cur < n->len) n->cur++;
    note_pcol(n);
}

static void caret_vertical(Notes *n, int dir) {
    int line = line_index_at(n, n->cur);
    int total = line_count(n);
    int target = line + dir;
    if (target < 0) { n->cur = 0; n->pcol = 0; return; }
    if (target >= total) { n->cur = n->len; note_pcol(n); return; }
    int start = line_start_of_index(n, target);
    int end = line_end_at(n, start);
    int col = n->pcol;
    if (col > end - start) col = end - start;
    n->cur = start + col;
}

static int caret_row_from_top(Notes *n, Font *fr, const Grid *g, int *rows_top_line) {
    int caret_line = line_index_at(n, n->cur);
    if (caret_line < n->top) return -1;

    int pos = line_start_of_index(n, n->top);
    int end = line_end_at(n, pos);
    int top_rows = rows_of_line(n, fr, pos, end, g->avail);
    if (rows_top_line) *rows_top_line = top_rows;

    int rows = 0;
    int line = n->top;
    while (line < caret_line) {
        rows += rows_of_line(n, fr, pos, end, g->avail);
        pos = end < n->len ? end + 1 : end;
        end = line_end_at(n, pos);
        line++;
    }
    rows += rows_of_line(n, fr, pos, n->cur, g->avail) - 1;
    return rows - n->top_row;
}

static void ensure_visible(Notes *n, Font *fr, Font *fd) {
    if (!fr || !fd || n->view_preview) return;

    Layout L;
    layout_build(n, &L, fr, fd);
    Grid *g = &L.g;

    int lines = line_count(n);
    if (n->top > lines - 1) { n->top = lines - 1; n->top_row = 0; }
    if (n->top < 0) { n->top = 0; n->top_row = 0; }
    if (n->top_row < 0) n->top_row = 0;

    int vis = g->vis;
    int caret_line = line_index_at(n, n->cur);

    if (caret_line < n->top) {
        n->top = caret_line;
        n->top_row = 0;
    }

    int top_rows = 1;
    int row = caret_row_from_top(n, fr, g, &top_rows);

    if (row < 0) {
        n->top_row += row;
        if (n->top_row < 0) n->top_row = 0;
        row = caret_row_from_top(n, fr, g, &top_rows);
        if (row < 0) row = 0;
    }
    if (row >= vis)
        n->top_row += row - vis + 1;

    int guard = 0;
    while (n->top_row > 0 && n->top_row >= top_rows && n->top < caret_line) {
        n->top_row -= top_rows;
        n->top++;
        int s = line_start_of_index(n, n->top);
        top_rows = rows_of_line(n, fr, s, line_end_at(n, s), g->avail);
        if (++guard > 100000) break;
    }
    if (n->top_row >= top_rows) n->top_row = top_rows - 1;
    if (n->top_row < 0) n->top_row = 0;
}

static void caret_page(Notes *n, Font *fr, Font *fd, int dir) {
    if (!fr || !fd) return;
    Layout L;
    layout_build(n, &L, fr, fd);
    Grid *g = &L.g;

    int lines = line_count(n);
    int vis = g->vis;
    int line = line_index_at(n, n->cur);
    int target = line + dir * vis;
    if (target < 0) target = 0;
    if (target > lines - 1) target = lines - 1;

    int max_top = lines - vis;
    if (max_top < 0) max_top = 0;
    n->top += dir * vis;
    if (dir > 0 && n->top > max_top) n->top = max_top;
    if (n->top < 0) n->top = 0;
    n->top_row = 0;

    int start = line_start_of_index(n, target);
    int end = line_end_at(n, start);
    int col = n->pcol;
    if (col > end - start) col = end - start;
    n->cur = start + col;
    ensure_visible(n, fr, fd);
}

/* ------------------------------------------------------------
 *  Editing in the fixed buffer
 * ------------------------------------------------------------ */
static void insert(Notes *n, char c) {
    if (n->len >= NOTES_MAX - 1) return;
    memmove(n->buf + n->cur + 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->buf[n->cur] = c;
    n->cur++;
    n->len++;
    n->buf[n->len] = 0;
    mark_dirty(n);
    note_pcol(n);
}

static void erase_one(Notes *n) {
    if (n->cur <= 0) return;
    memmove(n->buf + n->cur - 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->cur--;
    n->len--;
    n->buf[n->len] = 0;
    mark_dirty(n);
    note_pcol(n);
}

static void erase_forward(Notes *n) {
    if (n->cur >= n->len) return;
    memmove(n->buf + n->cur, n->buf + n->cur + 1, (size_t)(n->len - n->cur - 1));
    n->len--;
    n->buf[n->len] = 0;
    mark_dirty(n);
    note_pcol(n);
}

/* Find the next occurrence at or after from, wrapping once. */
static int find_next_in(Notes *n, int from) {
    if (n->find_len <= 0) return 0;
    int len = n->find_len;
    if (from < 0) from = 0;
    if (from > n->len) from = n->len;

    for (int i = from; i + len <= n->len; i++)
        if (ci_match(n->buf, n->len, i, n->find, len)) {
            n->cur = i;
            note_pcol(n);
            return 1;
        }
    for (int i = 0; i + len <= n->len && i < from; i++)
        if (ci_match(n->buf, n->len, i, n->find, len)) {
            n->cur = i;
            note_pcol(n);
            return 1;
        }
    return 0;
}

static int replace_current(Notes *n) {
    if (n->find_len <= 0) return 0;
    if (!ci_match(n->buf, n->len, n->cur, n->find, n->find_len)) return 0;
    int newlen = n->len - n->find_len + n->repl_len;
    if (newlen > NOTES_MAX - 1) {
        set_status(n, 1, "replace: buffer is full");
        return 0;
    }
    memmove(n->buf + n->cur + n->repl_len, n->buf + n->cur + n->find_len,
            (size_t)(n->len - n->cur - n->find_len));
    memcpy(n->buf + n->cur, n->repl, (size_t)n->repl_len);
    n->len = newlen;
    n->buf[n->len] = 0;
    n->cur += n->repl_len;
    mark_dirty(n);
    note_pcol(n);
    return 1;
}

static void replace_all(Notes *n) {
    if (n->find_len <= 0) { set_status(n, 1, "replace all: find text is empty"); return; }

    int total = 0;
    for (int i = 0; i + n->find_len <= n->len; ) {
        if (ci_match(n->buf, n->len, i, n->find, n->find_len)) {
            total++;
            i += n->find_len;
        } else {
            i++;
        }
    }
    if (total == 0) { set_status(n, 1, "replace all: no matches"); return; }

    int done = total;
    int diff = n->repl_len - n->find_len;
    if (diff > 0) {
        long room = (long)(NOTES_MAX - 1) - (long)n->len;
        long can = room / (long)diff;
        if (can < 0) can = 0;
        if ((long)done > can) done = (int)can;
    }
    if (done <= 0) { set_status(n, 1, "replace all: buffer is full"); return; }

    int i = 0, o = 0, n_done = 0;
    while (i < n->len) {
        if (n_done < done && i + n->find_len <= n->len &&
            ci_match(n->buf, n->len, i, n->find, n->find_len)) {
            memcpy(n->scan + o, n->repl, (size_t)n->repl_len);
            o += n->repl_len;
            i += n->find_len;
            n_done++;
        } else {
            n->scan[o++] = n->buf[i++];
        }
    }
    memcpy(n->buf, n->scan, (size_t)o);
    n->len = o;
    n->buf[n->len] = 0;
    n->cur = 0;
    n->pcol = 0;
    mark_dirty(n);

    char b[160];
    if (n_done < total)
        snprintf(b, sizeof(b), "replaced %d of %d - buffer is full", n_done, total);
    else
        snprintf(b, sizeof(b), "replaced %d occurrence%s", n_done, n_done == 1 ? "" : "s");
    set_status(n, n_done < total, b);
}

/* Word / character / line counts from the buffer. Characters are UTF-8
 * code points, not bytes, so a pasted accent counts as one. */
static void text_counts(const Notes *n, int *words, int *chars, int *lines) {
    int w = 0, c = 0, l = 1, inw = 0;
    for (int i = 0; i < n->len; i++) {
        unsigned char b = (unsigned char)n->buf[i];
        if ((b & 0xC0) != 0x80) c++;
        if (b == '\n') l++;
        if (LINE_SEP((char)b)) inw = 0;
        else if (!inw) { inw = 1; w++; }
    }
    *words = w;
    *chars = c;
    *lines = l;
}

/* ------------------------------------------------------------
 *  Search across every note in /root/notes
 * ------------------------------------------------------------ */
static void add_hit(Notes *n, const char *file, long off, int line,
                    const char *linebuf, int line_len, int match_col) {
    if (n->nhits >= MAX_HITS) { n->hits_more = 1; return; }

    Hit *h = &n->hits[n->nhits++];
    memset(h, 0, sizeof(*h));
    snprintf(h->file, sizeof(h->file), "%s", file);
    h->off = off;
    h->len = n->filter_len;
    h->line = line;

    /* Keep the match early in the snippet so the highlight is visible
     * in a narrow sidebar even when the source line is very long. */
    int start = 0;
    if (match_col > 24) start = match_col - 8;
    int take = line_len - start;
    if (take > SNIP_LEN - 1) take = SNIP_LEN - 1;
    if (take < 0) take = 0;
    memcpy(h->snip, linebuf + start, (size_t)take);
    h->snip[take] = 0;
    h->snip_off = match_col - start;
    h->snip_len = n->filter_len;
    if (h->snip_off < 0 || h->snip_off + h->snip_len > take) {
        h->snip_off = -1;
        h->snip_len = 0;
    }
}

/* Scan one block of bytes for the filter. It is used both for the disk
 * copy of a note and for the live buffer of the note being edited, so a
 * search always sees exactly what is on screen - unsaved text included. */
static void scan_block(Notes *n, const char *name, const char *data, int len) {
    int line = 1;
    int line_start = 0;
    for (int i = 0; i < len; ) {
        if (data[i] == '\n') {
            line++;
            i++;
            line_start = i;
            continue;
        }
        if (ci_match(data, len, i, n->filter, n->filter_len)) {
            int le = i;
            while (le < len && data[le] != '\n') le++;
            add_hit(n, name, (long)i, line, data + line_start, le - line_start,
                    i - line_start);
            i += n->filter_len;
            if (n->nhits >= MAX_HITS) { n->hits_more = 1; return; }
        } else {
            i++;
        }
    }
}

static void search_all(Notes *n) {
    n->nhits = 0;
    n->hits_more = 0;
    n->hsel = 0;
    n->htop = 0;
    /* The filter changed which rows are visible (clearing it too), so the
     * next draw re-stats the rows it is about to show. */
    docs_stat_stale(n);
    if (n->filter_len <= 0) return;

    for (int di = 0; di < n->ndocs && n->nhits < MAX_HITS; di++) {
        if (di == n->cur_doc && n->dirty) {
            scan_block(n, n->docs[di].name, n->buf, n->len);
            continue;
        }

        char path[PATH_LEN];
        if (!doc_path(n, n->docs[di].name, path, sizeof(path))) continue;

        int e = 0;
        long size = -1;
        int got = read_file(path, n->scan, NOTES_MAX - 1, &size, &e);
        if (got < 0) continue;         /* vanished between scan and open */
        scan_block(n, n->docs[di].name, n->scan, got);
    }
}

static void open_hit(Notes *n, int idx) {
    if (idx < 0 || idx >= n->nhits) return;
    char file[NAME_LEN];
    long off = n->hits[idx].off;
    int line = n->hits[idx].line;
    snprintf(file, sizeof(file), "%s", n->hits[idx].file);

    int di = doc_index_of(n, file);
    if (di < 0) {
        docs_scan(n);
        di = doc_index_of(n, file);
        if (di < 0) {
            char b[180];
            snprintf(b, sizeof(b), "%s is gone (errno %d)", file, ENOENT);
            set_status(n, 1, b);
            search_all(n);
            return;
        }
    }
    if (!doc_open(n, di)) return;
    n->view_preview = 0;        /* a hit is opened to be edited */
    n->cur = clampi((int)off, 0, n->len);
    n->pcol = n->cur - line_start_at(n, n->cur);
    /* The disk may have changed since the scan, or the hit may have come
     * from the live buffer: put the caret on a real match rather than on
     * a byte offset that no longer points at one. */
    if (n->filter_len > 0 &&
        !ci_match(n->buf, n->len, n->cur, n->filter, n->filter_len)) {
        if (!find_next_in(n, n->cur)) find_next_in(n, 0);
    }
    n->focus = FOCUS_EDIT;
    n->find_open = 0;
    ensure_visible(n, font_get(FONT_REGULAR, 12), font_get(FONT_REGULAR, 10));
    char b[200];
    snprintf(b, sizeof(b), "opened %s:%d", file, line);
    set_status(n, 0, b);
}

/* ------------------------------------------------------------
 *  Document create / delete / rename
 * ------------------------------------------------------------ */
static int doc_create(Notes *n, const char *name) {
    if (!name_ok(name)) {
        set_status(n, 1, "name must be 1-95 chars, no '/' and no leading '.'");
        return 0;
    }
    if (n->cur_doc >= 0 && n->dirty && !doc_save(n, 0)) {
        set_status(n, 1, "create cancelled: current note was not saved");
        return 0;
    }

    char path[PATH_LEN];
    if (!doc_path(n, name, path, sizeof(path))) {
        set_status(n, 1, "create: path too long");
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        set_errno_status(n, "create", errno);
        return 0;
    }
    if (close(fd) != 0) {
        int e = errno;
        unlink(path);
        set_errno_status(n, "create close", e);
        return 0;
    }

    docs_scan(n);
    search_all(n);
    int idx = doc_index_of(n, name);
    if (idx < 0) {
        set_status(n, 1, "created, but the directory scan did not list it");
        return 0;
    }
    if (!doc_open(n, idx)) return 0;
    n->view_preview = 0;        /* a new note starts in the editor */
    char b[200];
    snprintf(b, sizeof(b), "created %s", name);
    set_status(n, 0, b);
    return 1;
}

static int doc_delete(Notes *n, int idx) {
    if (idx < 0 || idx >= n->ndocs) return 0;
    char name[NAME_LEN];
    char path[PATH_LEN];
    snprintf(name, sizeof(name), "%s", n->docs[idx].name);
    if (!doc_path(n, name, path, sizeof(path))) {
        set_status(n, 1, "delete: path too long");
        return 0;
    }
    if (unlink(path) != 0) {
        set_errno_status(n, "delete", errno);
        return 0;
    }

    int was_cur = (idx == n->cur_doc);
    if (was_cur) {
        /* The file is gone on purpose; do not let a later switch save
         * the deleted buffer back into a new file with the same name. */
        n->cur_doc = -1;
        n->path[0] = 0;
        n->dirty = 0;
        n->partial = 0;
        n->len = 0;
        n->buf[0] = 0;
        n->cur = 0;
    }

    docs_scan(n);
    search_all(n);

    if (was_cur) {
        if (n->ndocs > 0) {
            int next = idx < n->ndocs ? idx : n->ndocs - 1;
            if (next < 0) next = 0;
            doc_open(n, next);
        } else {
            /* Never leave the user without a document to type into. */
            char fresh[NAME_LEN];
            default_note_name(fresh, sizeof(fresh));
            char path[PATH_LEN];
            int fd = -1;
            if (doc_path(n, fresh, path, sizeof(path)))
                fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd < 0) {
                set_errno_status(n, "create replacement note", errno);
            } else {
                close(fd);
            }
            docs_scan(n);
            search_all(n);
            int ni = doc_index_of(n, fresh);
            if (ni >= 0) doc_open(n, ni);
        }
    } else {
        if (n->dsel >= n->ndocs) n->dsel = n->ndocs > 0 ? n->ndocs - 1 : 0;
    }

    char b[200];
    snprintf(b, sizeof(b), "deleted %s", name);
    set_status(n, 0, b);
    return 1;
}

static int doc_rename(Notes *n, int idx, const char *newname) {
    if (idx < 0 || idx >= n->ndocs) return 0;
    if (!name_ok(newname)) {
        set_status(n, 1, "name must be 1-95 chars, no '/' and no leading '.'");
        return 0;
    }
    char oldname[NAME_LEN];
    snprintf(oldname, sizeof(oldname), "%s", n->docs[idx].name);
    if (!strcmp(oldname, newname)) return 1;

    int was_cur = (idx == n->cur_doc);
    if (was_cur && n->dirty && !doc_save(n, 0)) {
        set_status(n, 1, "rename cancelled: current note was not saved");
        return 0;
    }

    char oldpath[PATH_LEN], newpath[PATH_LEN];
    if (!doc_path(n, oldname, oldpath, sizeof(oldpath)) ||
        !doc_path(n, newname, newpath, sizeof(newpath))) {
        set_status(n, 1, "rename: path too long");
        return 0;
    }
    /* rename(2) replaces the target silently. In a notes directory that is
     * a lost note, so refuse instead: the real lstat() decides, and the
     * message names the collision. */
    struct stat clash;
    if (lstat(newpath, &clash) == 0) {
        set_status(n, 1, "rename: a note with that name already exists");
        return 0;
    }
    if (rename(oldpath, newpath) != 0) {
        set_errno_status(n, "rename", errno);
        return 0;
    }

    docs_scan(n);
    int ni = doc_index_of(n, newname);
    if (was_cur && ni >= 0) {
        n->cur_doc = ni;
        snprintf(n->path, sizeof(n->path), "%s", newpath);
    }
    if (ni >= 0) n->dsel = ni;
    search_all(n);

    char b[220];
    snprintf(b, sizeof(b), "renamed %s to %s", oldname, newname);
    set_status(n, 0, b);
    return 1;
}

/* ------------------------------------------------------------
 *  Autosave
 * ------------------------------------------------------------ */
static void autosave_tick(Notes *n) {
    if (!n->dirty || n->partial || n->cur_doc < 0 || n->path[0] == 0) return;
    time_t now = time(NULL);
    if (n->dirty_since == 0 || now < n->dirty_since) n->dirty_since = now;
    if (now - n->dirty_since < AUTOSAVE_SECS) return;
    if (n->last_attempt != 0 && now - n->last_attempt < AUTOSAVE_SECS) return;
    if (doc_save(n, 1) && n->filter_len > 0)
        search_all(n);      /* the search results now match the disk */
}

/* ------------------------------------------------------------
 *  Buttons and fields
 * ------------------------------------------------------------ */
static void draw_button(Surface *s, const Theme *th, Font *f,
                        int x, int y, int w, int h, const char *label, u32 text_col,
                        int enabled) {
    if (w <= 1 || h <= 1) return;
    u32 fill = color_mix(th->window_bg, 0xFF000000u, 0.06f);
    u32 bord = enabled ? color_alpha(th->panel_border, 0.9f)
                       : color_alpha(th->panel_border, 0.35f);
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h,
                 (float)ui_px(5), fill, bord, 1.0f);
    float inner = (float)w - 2.0f * (float)ui_px(4);
    char cut[80];
    clip_text(cut, sizeof(cut), f, label, inner);
    float tw = font_width(f, cut);
    float tx = (float)x + ((float)w - tw) * 0.5f;
    float ty = ink_y(f, (float)y, (float)h);
    font_draw(s, f, tx, ty, cut, text_col);
}

/* A text field: box, clipped text, caret bar at the pen after the last
 * visible glyph. Used by the find/replace bar and the modal prompt. */
static void draw_field(Surface *s, const Theme *th, Font *f,
                       int x, int y, int w, int h, const char *text,
                       const char *placeholder, int focused, u32 text_col) {
    if (w <= 2 || h <= 2) return;
    u32 fill = color_mix(th->window_bg, 0xFF000000u, 0.12f);
    u32 bord = focused ? th->accent : color_alpha(th->panel_border, 0.7f);
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h,
                 (float)ui_px(4), fill, bord, 1.0f);

    float pad = (float)ui_px(5);
    float maxw = (float)w - 2.0f * pad;
    if (maxw < 1.0f) maxw = 1.0f;
    const char *shown = (text && text[0]) ? text : placeholder;
    u32 col = (text && text[0]) ? text_col : color_alpha(th->text, 0.38f);
    char cut[224];
    clip_text(cut, sizeof(cut), f, shown ? shown : "", maxw);
    float ty = ink_y(f, (float)y, (float)h);
    float pen = font_draw(s, f, (float)x + pad, ty, cut, col);

    if (focused) {
        float cx = pen;
        float right = (float)(x + w) - pad;
        if (cx > right - 1.0f) cx = right - 1.0f;
        surf_blend(s, (int)cx, y + ui_px(3), ui_px(1), h - 2 * ui_px(3), th->accent);
    }
}

/* ------------------------------------------------------------
 *  Escape sequences
 * ------------------------------------------------------------ */
static void preview_scroll(Notes *n, int px) {
    n->pv_off += px;
    if (n->pv_off < 0) n->pv_off = 0;
}

static void esc_action(Notes *n, int param, int final, Font *fr, Font *fd) {
    if (n->prompt) return;

    if (n->view_preview) {
        int line = (int)(font_height(fr) * 1.6f) + ui_px(2);
        switch (final) {
            case 'A': preview_scroll(n, -line); break;
            case 'B': preview_scroll(n, +line); break;
            case 'H': n->pv_off = 0; break;
            case 'F': n->pv_off += 1000000; break;
            case '~':
                if (param == 5) preview_scroll(n, -(int)(ui_px(220)));
                else if (param == 6) preview_scroll(n, +(int)(ui_px(220)));
                break;
            default: break;
        }
        return;
    }

    if (n->focus == FOCUS_LIST) {
        switch (final) {
            case 'A': if (n->filter_len) n->hsel--; else n->dsel--; break;
            case 'B': if (n->filter_len) n->hsel++; else n->dsel++; break;
            case 'H': if (n->filter_len) n->hsel = 0; else n->dsel = 0; break;
            case 'F': if (n->filter_len) n->hsel = n->nhits - 1;
                      else n->dsel = n->ndocs - 1; break;
            case '~':
                if (param == 3) prompt_delete(n);
                else if (param == 5) { if (n->filter_len) n->hsel -= 5; else n->dsel -= 5; }
                else if (param == 6) { if (n->filter_len) n->hsel += 5; else n->dsel += 5; }
                break;
            default: break;
        }
        return;
    }

    if (n->focus == FOCUS_FILTER) {
        switch (final) {
            case 'A': n->hsel--; break;
            case 'B': n->hsel++; break;
            case 'H': n->hsel = 0; break;
            case 'F': n->hsel = n->nhits - 1; break;
            case '~':
                if (param == 5) n->hsel -= 5;
                else if (param == 6) n->hsel += 5;
                break;
            default: break;
        }
        return;
    }

    switch (final) {
        case 'A': caret_vertical(n, -1); break;
        case 'B': caret_vertical(n, +1); break;
        case 'C': caret_right(n); break;
        case 'D': caret_left(n); break;
        case 'H': caret_home_line(n); break;
        case 'F': caret_end_line(n); break;
        case '~':
            switch (param) {
                case 1: case 7: caret_home_line(n); break;
                case 4: case 8: caret_end_line(n); break;
                case 3: erase_forward(n); break;
                case 5: caret_page(n, fr, fd, -1); break;
                case 6: caret_page(n, fr, fd, +1); break;
                default: break;
            }
            break;
        default: break;
    }
    ensure_visible(n, fr, fd);
}

static int esc_feed(Notes *n, unsigned char c, Font *fr, Font *fd) {
    switch (n->esc) {
        case 1:
            if (c == '[') {
                n->esc = 2; n->esc_param = 0; n->esc_digits = 0; n->esc_mod = 0;
                return 1;
            }
            if (c == 'O') { n->esc = 3; return 1; }
            n->esc = 0;
            if (c == 0x1B) { n->esc = 1; return 1; }
            return 0;
        case 2:
            if (c >= '0' && c <= '9') {
                if (!n->esc_mod && n->esc_digits < 4) {
                    n->esc_param = n->esc_param * 10 + (c - '0');
                    n->esc_digits++;
                }
                return 1;
            }
            if (c == ';') { n->esc_mod = 1; return 1; }
            n->esc = 0;
            esc_action(n, n->esc_digits ? n->esc_param : 0, c, fr, fd);
            return 1;
        case 3:
            n->esc = 0;
            esc_action(n, 0, c, fr, fd);
            return 1;
        default:
            n->esc = 0;
            return 0;
    }
}

/* ------------------------------------------------------------
 *  Markdown-ish preview (real parsing of the buffer)
 * ------------------------------------------------------------ */
typedef struct {
    Surface     *s;
    const Theme *th;
    Font        *fr, *fb, *fd, *fm;
    int          left, right;
    float        top, bottom, off;
    int          draw;
} MdCtx;

typedef struct {
    char          txt[56];
    unsigned char style;   /* 0 reg, 1 bold, 2 code, 3 link, 4 italic */
    float         w, sep;
} MdTok;

static Font *md_font(int style, Font *body, Font *bold, Font *mono) {
    if (style == 1) return bold;
    if (style == 2) return mono;
    return body;
}

static void md_emit(MdTok *tk, int *nt, int max, const char *word, int wl,
                    int style, Font *body, Font *bold, Font *mono,
                    float spacew, float *spaces) {
    if (wl <= 0 || *nt >= max) return;
    MdTok *t = &tk[*nt];
    int n = wl;
    if (n > (int)sizeof(t->txt) - 1) n = (int)sizeof(t->txt) - 1;
    memcpy(t->txt, word, (size_t)n);
    t->txt[n] = 0;
    t->style = (unsigned char)style;
    t->w = font_width(md_font(style, body, bold, mono), t->txt);
    t->sep = (*spaces) * spacew;
    *spaces = 0.0f;
    (*nt)++;
}

static int md_tokens(Notes *n, int s, int e, Font *body, Font *bold, Font *mono,
                     MdTok *tk, int max, float spacew) {
    int nt = 0, wl = 0, style = 0;
    float spaces = 0.0f;
    char word[64];

    for (int i = s; i < e; i++) {
        unsigned char ch = (unsigned char)n->buf[i];

        if (ch == '*' && i + 1 < e && n->buf[i + 1] == '*') {
            if (wl) { md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces); wl = 0; }
            style = (style == 1) ? 0 : 1;
            i++;
            continue;
        }
        if (ch == '*') {
            if (wl) { md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces); wl = 0; }
            style = (style == 4) ? 0 : 4;
            continue;
        }
        if (ch == '`') {
            if (wl) { md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces); wl = 0; }
            style = (style == 2) ? 0 : 2;
            continue;
        }
        if (ch == '[') {
            int j = i + 1;
            while (j < e && n->buf[j] != ']') j++;
            if (j < e && j + 1 < e && n->buf[j + 1] == '(') {
                int k = j + 2;
                while (k < e && n->buf[k] != ')') k++;
                if (k < e) {
                    if (wl) { md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces); wl = 0; }
                    int ws = i + 1;
                    for (int q = i + 1; q <= j; q++) {
                        if (q == j || n->buf[q] == ' ') {
                            if (q > ws)
                                md_emit(tk, &nt, max, n->buf + ws, q - ws, 3,
                                        body, bold, mono, spacew, &spaces);
                            if (q < j) spaces += 1.0f;
                            ws = q + 1;
                        }
                    }
                    i = k;
                    continue;
                }
            }
        }
        if (ch == ' ' || ch == '\t') {
            if (wl) { md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces); wl = 0; }
            spaces += 1.0f;
            continue;
        }
        if (ch == '\r') continue;
        if (wl >= (int)sizeof(word) - 1) {
            md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces);
            wl = 0;
        }
        word[wl++] = (char)ch;
    }
    if (wl) md_emit(tk, &nt, max, word, wl, style, body, bold, mono, spacew, &spaces);
    return nt;
}

static int md_visible(const MdCtx *c, float y, float h) {
    if (!c->draw || !c->s) return 0;
    float sy = c->top - c->off + y;
    return sy + h > c->top && sy < c->bottom;
}

static float md_sy(const MdCtx *c, float y) {
    return c->top - c->off + y;
}

/* Lay out the whole buffer. With c->draw == 0 this only measures the
 * total height; with it set, the same walk paints the visible part.
 * Returns the total height in pixels. */
static float md_pass(Notes *n, MdCtx *c) {
    float y = 0.0f;
    int in_code = 0;
    int pos = 0;

    while (pos <= n->len) {
        int end = pos;
        while (end < n->len && n->buf[end] != '\n') end++;
        int e = end;
        while (e > pos && n->buf[e - 1] == '\r') e--;
        int next_pos = (end < n->len) ? end + 1 : n->len + 1;

        int lead = pos;
        while (lead < e && (n->buf[lead] == ' ' || n->buf[lead] == '\t')) lead++;

        int fence = (lead + 2 < e && n->buf[lead] == '`' &&
                     n->buf[lead + 1] == '`' && n->buf[lead + 2] == '`');
        if (fence) {
            in_code = !in_code;
            y += (float)ui_px(2);
            pos = next_pos;
            continue;
        }

        if (in_code) {
            Font *fm = c->fm;
            float lh = font_height(fm) + (float)ui_px(2);
            float cx = (float)c->left + (float)ui_px(8);
            float cr = (float)c->right - (float)ui_px(8);
            if (cr < cx + (float)ui_px(8)) cr = cx + (float)ui_px(8);

            int rows = 1;
            float pen = cx;
            for (int i = pos; i < e; i++) {
                char one[2] = { n->buf[i], 0 };
                float cw = font_width(fm, one);
                if (pen + cw > cr && pen > cx) { pen = cx; rows++; }
                pen += cw;
            }

            float band_h = (float)rows * lh;
            int fy = (int)(md_sy(c, y) + 0.5f);
            int fh = (int)(band_h + 0.5f);
            if (c->draw && c->s) {
                if (fy < c->top) { fh -= (int)(c->top - (float)fy); fy = (int)c->top; }
                if ((float)fy + (float)fh > c->bottom) fh = (int)(c->bottom - (float)fy);
                if (fh > 0)
                    surf_blend(c->s, c->left, fy, c->right - c->left, fh,
                               color_alpha(c->th->panel_border, 0.10f));
            }

            pen = cx;
            float row_y = y;
            for (int i = pos; i < e; i++) {
                char one[2] = { n->buf[i], 0 };
                float cw = font_width(fm, one);
                if (pen + cw > cr && pen > cx) { pen = cx; row_y += lh; }
                if (md_visible(c, row_y, lh)) {
                    float ty = md_sy(c, row_y) + (lh - ink_h(fm)) * 0.5f;
                    font_draw(c->s, fm, pen, ty, one, c->th->text);
                }
                pen += cw;
            }
            y += band_h;
            pos = next_pos;
            continue;
        }

        int start = lead;
        int indent = 0;
        int bullet = 0, ordered = 0, quote = 0;
        char numtxt[16] = { 0 };
        Font *body = c->fr, *bold = c->fb;
        float base_lh = font_height(c->fr);
        int heading = 0;

        int hlevel = 0;
        while (hlevel < 6 && pos + hlevel < e && n->buf[pos + hlevel] == '#') hlevel++;
        if (hlevel > 0 && pos + hlevel < e && n->buf[pos + hlevel] == ' ') {
            heading = hlevel;
            start = pos + hlevel + 1;
            body = font_get(FONT_SEMIBOLD, 11.0f + (float)(7 - hlevel));
            if (!body) body = c->fb;
            bold = body;
            base_lh = font_height(body);
        } else {
            start = lead;
            if (start + 1 < e &&
                (n->buf[start] == '-' || n->buf[start] == '*' || n->buf[start] == '+') &&
                n->buf[start + 1] == ' ') {
                bullet = 1;
                start += 2;
                indent += ui_px(16);
            } else {
                int q = start;
                while (q < e && q - start < 5 && n->buf[q] >= '0' && n->buf[q] <= '9') q++;
                if (q > start && q < e && n->buf[q] == '.' && q + 1 < e && n->buf[q + 1] == ' ') {
                    ordered = 1;
                    int nl = q - start;
                    if (nl > 4) nl = 4;
                    memcpy(numtxt, n->buf + start, (size_t)nl);
                    numtxt[nl] = 0;
                    start = q + 2;
                    indent += (int)font_width(c->fd, numtxt) + ui_px(12);
                }
            }
        }

        while (start < e && n->buf[start] == '>') {
            int adv = 1;
            if (start + 1 < e && n->buf[start + 1] == ' ') adv = 2;
            quote++;
            start += adv;
            indent += ui_px(10);
        }
        while (start < e && (n->buf[start] == ' ' || n->buf[start] == '\t')) start++;

        if (start >= e) {
            y += font_height(c->fr) * 0.55f;
            pos = next_pos;
            continue;
        }

        MdTok tk[MD_TOK_MAX];
        float spacew = font_width(body, " ");
        int nt = md_tokens(n, start, e, body, bold, c->fm, tk, MD_TOK_MAX, spacew);
        if (nt <= 0) {
            y += base_lh * 0.55f;
            pos = next_pos;
            continue;
        }

        float lh = base_lh;
        for (int k = 0; k < nt; k++) {
            float fh = font_height(md_font(tk[k].style, body, bold, c->fm));
            if (fh > lh) lh = fh;
        }
        lh += (float)ui_px(3);

        float content_x = (float)c->left + (float)indent + (float)ui_px(4);
        float right = (float)c->right;
        u32 col = quote ? color_alpha(c->th->text, 0.68f) : c->th->text;

        if (md_visible(c, y, lh)) {
            for (int q = 0; q < quote; q++)
                surf_blend(c->s, c->left + q * ui_px(10),
                           (int)md_sy(c, y) + ui_px(2), ui_px(2),
                           (int)lh - ui_px(4), color_alpha(c->th->accent, 0.55f));
            if (bullet)
                gfx_rrect(c->s, (float)c->left + (float)ui_px(5),
                          md_sy(c, y) + lh * 0.5f - (float)ui_px(2),
                          (float)ui_px(4), (float)ui_px(4),
                          (float)ui_px(2), color_alpha(c->th->accent, 0.85f));
            if (ordered) {
                float ty = md_sy(c, y) + (lh - ink_h(c->fd)) * 0.5f;
                font_draw(c->s, c->fd, (float)c->left + (float)ui_px(4), ty,
                          numtxt, color_alpha(c->th->text, 0.45f));
            }
        }

        float pen = content_x;
        for (int k = 0; k < nt; k++) {
            float lead2 = (pen > content_x) ? tk[k].sep : 0.0f;
            if (pen + lead2 + tk[k].w > right && pen > content_x) {
                y += lh;
                pen = content_x;
                lead2 = 0.0f;
            }
            /* A token can be wider than the whole column (a URL, a hash, a
             * path: md_emit() caps a word at 55 bytes, not at the column).
             * The old test only wrapped when the line was already non-empty,
             * so that token was painted straight past the right margin, the
             * scrollbar and the band. Fit it to the room that is left. */
            float room = right - pen - lead2;
            if (tk[k].w > room) {
                if (md_visible(c, y, lh)) {
                    Font *f = md_font(tk[k].style, body, bold, c->fm);
                    float ty = md_sy(c, y) + (lh - ink_h(f)) * 0.5f;
                    draw_fit(c->s, f, pen + lead2, ty,
                             room > 0.0f ? room : 0.0f, tk[k].txt,
                             tk[k].style == 3 ? c->th->accent : col);
                }
                pen = right;
                continue;
            }
            if (md_visible(c, y, lh)) {
                Font *f = md_font(tk[k].style, body, bold, c->fm);
                float ty = md_sy(c, y) + (lh - ink_h(f)) * 0.5f;
                if (tk[k].style == 2)
                    surf_blend(c->s, (int)(pen + lead2),
                               (int)(md_sy(c, y) + (lh - font_height(f)) * 0.5f),
                               (int)tk[k].w + ui_px(2), (int)font_height(f),
                               color_alpha(c->th->panel_border, 0.14f));
                font_draw(c->s, f, pen + lead2, ty, tk[k].txt,
                          tk[k].style == 3 ? c->th->accent : col);
                if (tk[k].style == 3)
                    surf_blend(c->s, (int)(pen + lead2),
                               (int)(ty + ink_h(f) + 1.0f),
                               (int)tk[k].w, 1, c->th->accent);
            }
            pen += lead2 + tk[k].w;
        }
        (void)heading;
        y += lh;
        pos = next_pos;
    }
    return y;
}

/* ------------------------------------------------------------
 *  Drawing: sidebar
 * ------------------------------------------------------------ */
static void draw_hit_row(Notes *n, Surface *s, const Layout *L, Font *fd,
                         const Theme *th, int i, int ry) {
    Hit *h = &n->hits[i];
    int x = L->flt_x + ui_px(4);
    int right = L->flt_x + L->flt_w - ui_px(2);
    int one = (int)(font_height(fd) + 0.5f);

    if (i == n->hsel)
        surf_blend(s, L->flt_x, ry, L->flt_w, L->hit_h,
                   color_alpha(th->accent, 0.17f));

    char head[NAME_LEN + 16];
    snprintf(head, sizeof(head), "%s:%d", h->file, h->line);
    draw_fit(s, fd, (float)x, (float)(ry + ui_px(2)),
             (float)(right - x), head,
             i == n->hsel ? th->text : color_alpha(th->text, 0.62f));

    float ty = (float)(ry + one + ui_px(4));
    if (h->snip_off >= 0) {
        char before[SNIP_LEN], match[SNIP_LEN], after[SNIP_LEN];
        int bo = h->snip_off;
        int ml = h->snip_len;
        if (bo > (int)sizeof(before) - 1) bo = (int)sizeof(before) - 1;
        memcpy(before, h->snip, (size_t)bo);
        before[bo] = 0;
        if (ml > (int)sizeof(match) - 1) ml = (int)sizeof(match) - 1;
        memcpy(match, h->snip + h->snip_off, (size_t)ml);
        match[ml] = 0;
        snprintf(after, sizeof(after), "%s", h->snip + h->snip_off + h->snip_len);

        float pen = draw_run(s, fd, (float)x, ty, (float)(right - x), before,
                             color_alpha(th->text, 0.55f));
        char mcut[SNIP_LEN];
        clip_text(mcut, sizeof(mcut), fd, match, (float)right - pen);
        float mw = font_width(fd, mcut);
        if (mw > 0.0f)
            surf_blend(s, (int)pen, (int)(ty - ui_px(1)), (int)mw,
                       (int)(font_height(fd) + ui_px(2)),
                       color_alpha(th->accent, 0.35f));
        pen = font_draw(s, fd, pen, ty, mcut, th->text);
        draw_fit(s, fd, pen, ty, (float)right - pen, after,
                 color_alpha(th->text, 0.55f));
    } else {
        draw_fit(s, fd, (float)x, ty, (float)(right - x), h->snip,
                 color_alpha(th->text, 0.55f));
    }
}

static void draw_sidebar(Notes *n, Surface *s, const Layout *L,
                         Font *fr, Font *fb, Font *fd, const Theme *th,
                         int stat_now) {
    surf_fill(s, 0, 0, L->sbw, n->h, color_mix(th->window_bg, 0xFF000000u, 0.10f));
    surf_blend(s, L->sbw, 0, 1, n->h, color_alpha(th->panel_border, 0.65f));

    /* header */
    draw_fit(s, fb, (float)L->flt_x, ink_y(fb, (float)L->hdr_y, (float)L->hdr_h),
             (float)L->flt_w - (float)ui_px(40), "Documents", th->text);
    char count[32];
    if (n->filter_len)
        snprintf(count, sizeof(count), "%d%s/%d", n->nhits,
                 n->hits_more ? "+" : "", n->ndocs);
    else
        snprintf(count, sizeof(count), "%d%s", n->ndocs,
                 n->docs_more ? "+" : "");
    float cw = font_width(fd, count);
    font_draw(s, fd, (float)(L->flt_x + L->flt_w) - cw,
              ink_y(fd, (float)L->hdr_y, (float)L->hdr_h),
              count, color_alpha(th->text, 0.45f));

    /* filter box */
    u32 flt_border = (n->focus == FOCUS_FILTER) ? th->accent
                                                : color_alpha(th->panel_border, 0.7f);
    gfx_rrect_aa(s, (float)L->flt_x, (float)L->flt_y, (float)L->flt_w,
                 (float)L->flt_h, (float)ui_px(5),
                 color_mix(th->window_bg, 0xFF000000u, 0.15f), flt_border, 1.0f);
    float fpad = (float)ui_px(5);
    char shown[160];
    if (n->filter_len) snprintf(shown, sizeof(shown), "%s", n->filter);
    else               snprintf(shown, sizeof(shown), "%s", "Search all notes");
    float fmaxw = (float)L->flt_w - 2.0f * fpad - (n->filter_len ? (float)L->cx_w : 0.0f);
    char cut[224];
    clip_text(cut, sizeof(cut), fr, shown, fmaxw);
    float ty = ink_y(fr, (float)L->flt_y, (float)L->flt_h);
    float pen = font_draw(s, fr, (float)L->flt_x + fpad, ty, cut,
                          n->filter_len ? th->text : color_alpha(th->text, 0.35f));
    if (n->focus == FOCUS_FILTER) {
        float cx = pen;
        if (cx > (float)(L->cx_x - ui_px(3))) cx = (float)(L->cx_x - ui_px(3));
        surf_blend(s, (int)cx, L->flt_y + ui_px(4), ui_px(1),
                   L->flt_h - 2 * ui_px(4), th->accent);
    }
    if (n->filter_len) {
        float cx = (float)(L->cx_x + L->cx_w / 2) - ui_px(1);
        float cy = (float)(L->flt_y + L->flt_h / 2);
        gfx_line(s, cx - 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f, 1.0f,
                 color_alpha(th->text, 0.55f));
        gfx_line(s, cx - 3.0f, cy + 3.0f, cx + 3.0f, cy - 3.0f, 1.0f,
                 color_alpha(th->text, 0.55f));
    }

    /* list */
    int list_r = L->flt_x + L->flt_w;
    int list_b = L->list_y + L->list_h;
    if (L->list_h > 0) {
        if (n->filter_len) {
            if (n->nhits == 0) {
                char b[220];
                snprintf(b, sizeof(b), "No matches for \"%s\"", n->filter);
                draw_fit(s, fd, (float)L->flt_x, (float)L->list_y + (float)ui_px(2),
                         (float)L->flt_w, b, color_alpha(th->text, 0.45f));
            } else {
                for (int i = n->htop; i < n->nhits; i++) {
                    int ry = L->list_y + (i - n->htop) * L->hit_h;
                    if (ry + L->hit_h > list_b) break;
                    draw_hit_row(n, s, L, fd, th, i, ry);
                }
            }
        } else if (n->dir_err[0]) {
            draw_fit(s, fd, (float)L->flt_x, (float)L->list_y + (float)ui_px(2),
                     (float)L->flt_w, n->dir_err, ERR_COLOR);
        } else if (n->ndocs == 0) {
            draw_fit(s, fd, (float)L->flt_x, (float)L->list_y + (float)ui_px(2),
                     (float)L->flt_w, "No notes yet. Ctrl+N creates one.",
                     color_alpha(th->text, 0.45f));
        } else {
            for (int i = n->dtop; i < n->ndocs; i++) {
                int ry = L->list_y + (i - n->dtop) * L->row_h;
                if (ry + L->row_h > list_b) break;
                int cur = (i == n->cur_doc);
                int sel = (i == n->dsel && n->focus == FOCUS_LIST);
                if (cur || sel)
                    surf_blend(s, L->flt_x, ry, L->flt_w, L->row_h,
                               color_alpha(th->accent, cur ? 0.10f : 0.18f));
                if (cur)
                    surf_blend(s, L->flt_x, ry, ui_px(2), L->row_h, th->accent);
                int tx = L->flt_x + ui_px(6);
                int right = list_r - ui_px(2);

                /* Visible rows are re-stat'd when the metadata cache is due
                 * (once a second, or after a save/open/filter change), so an
                 * external write is picked up without a stat per frame. */
                if (stat_now) {
                    char dpath[PATH_LEN];
                    struct stat dst;
                    if (doc_path(n, n->docs[i].name, dpath, sizeof(dpath)) &&
                        stat(dpath, &dst) == 0) {
                        n->docs[i].size = (long)dst.st_size;
                        n->docs[i].mtime = dst.st_mtime;
                    }
                }

                char sz[32];
                fmt_size(n->docs[i].size, sz, sizeof(sz));
                float sw = font_width(fd, sz);
                font_draw(s, fd, (float)(right) - sw,
                          ink_y(fd, (float)ry, (float)L->row_h), sz,
                          color_alpha(th->text, cur ? 0.62f : 0.40f));

                char nm[NAME_LEN + 4];
                if (cur && n->dirty) snprintf(nm, sizeof(nm), "*%s", n->docs[i].name);
                else                 snprintf(nm, sizeof(nm), "%s", n->docs[i].name);
                draw_fit(s, fr, (float)tx, ink_y(fr, (float)ry, (float)L->row_h),
                         (float)(right - tx) - sw - (float)ui_px(6), nm,
                         cur ? th->text : color_alpha(th->text, 0.78f));
            }
        }
    }

    /* buttons */
    draw_button(s, th, fd, L->bnew_x, L->btn_y, L->bnew_w, L->btn_h,
                "New", th->text, 1);
    draw_button(s, th, fd, L->bren_x, L->btn_y, L->bren_w, L->btn_h,
                "Rename", th->text, n->ndocs > 0);
    draw_button(s, th, fd, L->bdel_x, L->btn_y, L->bdel_w, L->btn_h,
                "Delete", ERR_COLOR, n->ndocs > 0);

    /* status + key hints */
    const char *msg = n->status[0] ? n->status : "Ready";
    draw_fit(s, fd, (float)L->flt_x, (float)L->status_y, (float)L->flt_w, msg,
             n->status_err ? ERR_COLOR : color_alpha(th->text, 0.55f));

    const char *h0, *h1;
    if (n->focus == FOCUS_FILTER) {
        h0 = "Enter opens the match";
        h1 = "Up/Down pick - Ctrl+Q clears";
    } else if (n->focus == FOCUS_LIST) {
        h0 = "Enter open - Ctrl+R rename";
        h1 = "Ctrl+X delete - Ctrl+G search";
    } else {
        h0 = "Ctrl+G search - Ctrl+L list";
        h1 = "Ctrl+N new - Ctrl+S save";
    }
    float lineh = font_height(fd);
    draw_fit(s, fd, (float)L->flt_x, (float)L->hint_y, (float)L->flt_w, h0,
             color_alpha(th->text, 0.38f));
    draw_fit(s, fd, (float)L->flt_x, (float)L->hint_y + lineh, (float)L->flt_w, h1,
             color_alpha(th->text, 0.38f));
}

/* ------------------------------------------------------------
 *  Drawing: find/replace bar
 * ------------------------------------------------------------ */
static void draw_find_bar(Notes *n, Surface *s, const Layout *L, Font *fd,
                          const Theme *th) {
    if (!n->find_open) return;
    surf_blend(s, L->ex + 1, ui_px(4), L->ew - 2, L->find_h + ui_px(2),
               color_alpha(th->panel_border, 0.07f));

    float ty1 = ink_y(fd, (float)L->ff_y, (float)L->ff_h);
    font_draw(s, fd, (float)(L->ex + L->em), ty1, "find",
              color_alpha(th->text, 0.55f));
    draw_field(s, th, fd, L->ff_x, L->ff_y, L->ff_w, L->ff_h,
               n->find, "text to find", n->find_field == 0, th->text);

    float lw2 = font_width(fd, "with") + (float)ui_px(6);
    font_draw(s, fd, (float)L->rf_x - lw2, ink_y(fd, (float)L->rf_y, (float)L->rf_h),
              "with", color_alpha(th->text, 0.55f));
    draw_field(s, th, fd, L->rf_x, L->rf_y, L->rf_w, L->rf_h,
               n->repl, "replacement", n->find_field == 1, th->text);
}

/* ------------------------------------------------------------
 *  Drawing: footer
 * ------------------------------------------------------------ */
static void draw_footer(Notes *n, Surface *s, const Layout *L, Font *fd,
                        const Theme *th) {
    int x = L->ex + L->em;
    int w = L->ew - 2 * L->em;
    if (w < 2) return;
    float top = (float)(L->band_y + L->band_h);
    surf_blend(s, x, (int)top, w, 1, color_alpha(th->panel_border, 0.55f));

    int words, chars, lines;
    text_counts(n, &words, &chars, &lines);

    char l1[320];
    if (n->err[0]) {
        snprintf(l1, sizeof(l1), "%s%s",
                 n->err_is_save ? "not saved - " : "warning - ", n->err);
    } else if (n->disk_len < 0) {
        snprintf(l1, sizeof(l1), "buf %d B - no file on disk", n->len);
    } else if (n->partial && n->disk_len > (long)n->len) {
        snprintf(l1, sizeof(l1), "buf %d B - disk %ld B - partial load, autosave off",
                 n->len, n->disk_len);
    } else if (n->dirty) {
        time_t now = time(NULL);
        long wait = (long)AUTOSAVE_SECS - (long)(now - n->dirty_since);
        if (wait < 1) wait = 1;
        snprintf(l1, sizeof(l1), "buf %d B - disk %ld B - unsaved, autosave in %lds",
                 n->len, n->disk_len, wait);
    } else if (n->disk_len != (long)n->len) {
        snprintf(l1, sizeof(l1), "buf %d B - disk %ld B - unsaved (size differs)",
                 n->len, n->disk_len);
    } else {
        char clockbuf[16];
        fmt_clock(n->saved_here ? n->saved_at : n->disk_mtime, clockbuf, sizeof(clockbuf));
        snprintf(l1, sizeof(l1), "buf %d B - disk %ld B - %s %s",
                 n->len, n->disk_len, n->saved_here ? "saved" : "written", clockbuf);
    }

    char l2[260];
    const char *hint;
    if (n->find_open)      hint = "Enter next - Tab field - Ctrl+R replace all - Ctrl+Q close";
    else if (n->view_preview) hint = "Ctrl+T back to the editor - Up/Down scrolls";
    else                   hint = "Ctrl+F find - Ctrl+T preview - Ctrl+G search";
    snprintf(l2, sizeof(l2), "%d words - %d chars - %d lines - %s",
             words, chars, lines, hint);

    float lh = (float)L->foot_h * 0.5f;
    float y1 = top + (lh - ink_h(fd)) * 0.5f;
    float y2 = top + lh + (lh - ink_h(fd)) * 0.5f;
    draw_fit(s, fd, (float)x, y1, (float)w, l1,
             n->err[0] ? ERR_COLOR : color_alpha(th->text, 0.60f));
    draw_fit(s, fd, (float)x, y2, (float)w, l2, color_alpha(th->text, 0.40f));
}

/* ------------------------------------------------------------
 *  Drawing: preview
 * ------------------------------------------------------------ */
static void draw_preview(Notes *n, Surface *s, const Layout *L,
                         Font *fr, Font *fb, Font *fd, Font *fm, const Theme *th) {
    MdCtx mc;
    mc.s = s;
    mc.th = th;
    mc.fr = fr;
    mc.fb = fb;
    mc.fd = fd;
    mc.fm = fm;
    mc.left = L->band_x + ui_px(4);
    mc.right = L->band_x + L->band_w - ui_px(4);
    if (mc.right < mc.left + ui_px(20)) mc.right = mc.left + ui_px(20);
    mc.top = (float)L->band_y;
    mc.bottom = (float)(L->band_y + L->band_h);
    mc.off = 0.0f;
    mc.draw = 0;

    float total = md_pass(n, &mc);
    float vis = mc.bottom - mc.top;
    float maxoff = total - vis;
    if (maxoff < 0.0f) maxoff = 0.0f;
    if (n->pv_off < 0) n->pv_off = 0;
    if ((float)n->pv_off > maxoff) n->pv_off = (int)maxoff;

    mc.off = (float)n->pv_off;
    mc.draw = 1;
    md_pass(n, &mc);

    if (total > vis && vis > 1.0f) {
        float th_h = vis * vis / total;
        int thumb = (int)th_h;
        if (thumb < ui_px(14)) thumb = ui_px(14);
        if ((float)thumb > vis) thumb = (int)vis;
        int maxoffi = (int)maxoff;
        int tpos = maxoffi > 0 ? (int)(((long)(L->band_h - thumb) * n->pv_off) / maxoffi) : 0;
        int bx = L->ex + L->ew - ui_px(5);
        surf_blend(s, bx, L->band_y, ui_px(3), L->band_h,
                   color_alpha(th->panel_border, 0.18f));
        surf_blend(s, bx, L->band_y + tpos, ui_px(3), thumb,
                   color_alpha(th->accent, 0.55f));
    }
}

/* ------------------------------------------------------------
 *  Drawing: the editor
 * ------------------------------------------------------------ */
static void draw_editor(Notes *n, Surface *s, const Layout *L,
                        Font *fr, Font *fd, const Theme *th) {
    surf_fill(s, L->ex, 0, L->ew, n->h, color_mix(th->window_bg, 0xFF000000u, 0.22f));
    surf_blend(s, L->ex, 0, 1, n->h, color_alpha(th->panel_border, 0.6f));

    draw_find_bar(n, s, L, fd, th);

    if (n->view_preview) {
        draw_preview(n, s, L, fr, font_get(FONT_SEMIBOLD, 12), fd,
                     font_get(FONT_MONO, 11), th);
        draw_footer(n, s, L, fd, th);
        return;
    }

    const Grid *g = &L->g;
    int lines = line_count(n);
    if (n->top > lines - 1) { n->top = lines - 1; n->top_row = 0; }
    if (n->top < 0) { n->top = 0; n->top_row = 0; }
    if (n->top_row < 0) n->top_row = 0;

    int caret_line = line_index_at(n, n->cur);
    int caret_w = ui_px(1);
    float ink = ink_h(fr);

    if (g->num_w > 0.0f) {
        float rule_x = g->text_x - g->gap * 0.5f;
        surf_blend(s, (int)g->ox, (int)g->oy, (int)(rule_x - g->ox), (int)g->h,
                   color_alpha(th->panel_border, 0.10f));
        surf_blend(s, (int)rule_x, (int)g->oy, 1, (int)g->h,
                   color_alpha(th->panel_border, 0.55f));
    }

    const char *needle = NULL;
    int nlen = 0;
    if (n->find_open && n->find_len > 0) { needle = n->find; nlen = n->find_len; }
    else if (n->filter_len > 0) { needle = n->filter; nlen = n->filter_len; }

    int pos = line_start_of_index(n, n->top);
    int line = n->top;
    int row = 0;
    int base_row = n->top_row;
    int last_row = base_row + g->vis;
    float px = g->text_x;
    int caret_painted = 0;
    float caret_x = 0.0f, caret_top = 0.0f;

    while (line < lines && row < last_row) {
        int end = line_end_at(n, pos);
        int hl_until = -1;

        if (g->num_w > 0.0f) {
            int num_row = row < base_row ? base_row : row;
            if (num_row < last_row) {
                char num[16];
                snprintf(num, sizeof(num), "%d", line + 1);
                float band = g->oy + g->pad_t + (float)(num_row - base_row) * g->lh;
                /* Share the text's baseline (the old divider behaviour):
                 * centre the regular font's ink, then put the smaller
                 * digit font on the same baseline. */
                float nb = ink_y(fr, band, g->lh) + font_ascent(fr) - font_ascent(fd);
                float num_r = g->ox + g->m + g->num_w;
                u32 num_col = (line == caret_line) ? color_alpha(th->text, 0.95f)
                                                   : color_alpha(th->text, 0.38f);
                font_draw(s, fd, num_r - font_width(fd, num), nb, num, num_col);
            }
        }

        for (int i = pos; i < end && row < last_row; i++) {
            char glyph[8];
            int gl = utf8_seq(n, i, end, glyph);
            float cw = font_width(fr, glyph);
            if (px + cw > g->text_r && px > g->text_x) {
                px = g->text_x;
                row++;
                if (row >= last_row) break;
            }
            float band = g->oy + g->pad_t + (float)(row - base_row) * g->lh;
            if (row >= base_row) {
                /* The caret is byte-addressed; a position inside a multi-byte
                 * glyph belongs to the glyph it is part of. */
                if (n->cur >= i && n->cur < i + gl) {
                    caret_x = px;
                    caret_top = band + (g->lh - ink) * 0.5f;
                    caret_painted = 1;
                }

                if (needle) {
                    if (i >= hl_until) {
                        hl_until = (i + nlen <= end &&
                                    ci_match(n->buf, n->len, i, needle, nlen))
                                   ? i + nlen : -1;
                    }
                    if (hl_until > i) {
                        int hx0 = (int)(px + 0.5f);
                        int hx1 = (int)(px + cw + 0.5f);
                        if (hx1 <= hx0) hx1 = hx0 + 1;
                        if (hx1 > (int)g->text_r) hx1 = (int)g->text_r;
                        int cur_match = (n->find_open && n->cur >= i && n->cur < hl_until);
                        if (hx1 > hx0)
                            surf_blend(s, hx0, (int)band, hx1 - hx0, (int)g->lh,
                                       color_alpha(th->accent, cur_match ? 0.42f : 0.20f));
                    }
                }

                font_draw(s, fr, px, ink_y(fr, band, g->lh), glyph, th->text);
            }
            px += cw;
            i += gl - 1;
        }

        if (n->cur == end && row >= base_row && row < last_row) {
            float band = g->oy + g->pad_t + (float)(row - base_row) * g->lh;
            caret_x = px;
            caret_top = band + (g->lh - ink) * 0.5f;
            caret_painted = 1;
        }

        pos = end < n->len ? end + 1 : end;
        line++;
        if (pos >= n->len && line >= lines) break;
        px = g->text_x;
        row++;
    }

    /* The caret, last, one ink box tall, on the glyph pen: the x the
     * next glyph would be drawn from. Painting it after every glyph is
     * what keeps a glyph from covering it when it sits mid-word. */
    if (caret_painted)
        surf_blend(s, (int)(caret_x + 0.5f), (int)(caret_top + 0.5f),
                   caret_w, (int)(ink + 0.5f), th->accent);

    draw_footer(n, s, L, fd, th);
}

/* ------------------------------------------------------------
 *  Drawing: modal prompts
 * ------------------------------------------------------------ */
typedef struct {
    int x, y, w, h;
    int fx, fy, fw, fh;
    int ok_x, ok_y, ok_w, ok_h;
    int cx, cy, cw, ch;
} Modal;

static void modal_geom(const Notes *n, Font *fr, Font *fd, Modal *m) {
    int row = (int)(font_height(fr) + 0.5f);
    int fh = row + ui_px(12);
    int bh = (int)(font_height(fd) + 0.5f) + ui_px(10);
    int pad = ui_px(14);

    int w = n->w - 2 * ui_px(20);
    int maxw = ui_px(430);
    if (w > maxw) w = maxw;
    if (w < ui_px(180)) w = n->w - 2 * ui_px(6);
    if (w < ui_px(120)) w = n->w;
    if (w < 1) w = 1;

    int one = (int)(font_height(fd) + 0.5f);
    int h = pad + row + ui_px(8) + fh + ui_px(10) + one + ui_px(6) + bh + pad;
    int maxh = n->h - 2 * ui_px(10);
    if (maxh < bh + fh + ui_px(30)) maxh = bh + fh + ui_px(30);
    if (h > maxh) h = maxh;
    if (h < 1) h = 1;

    m->x = (n->w - w) / 2;
    m->y = (n->h - h) / 2;
    if (m->x < 0) m->x = 0;
    if (m->y < 0) m->y = 0;
    m->w = w;
    m->h = h;

    m->fx = m->x + pad;
    m->fy = m->y + pad + row + ui_px(8);
    m->fw = w - 2 * pad;
    m->fh = fh;

    int gap = ui_px(8);
    int bw = (w - 2 * pad - gap) / 2;
    if (bw < ui_px(24)) bw = w / 2;
    if (bw < 1) bw = 1;
    m->ok_x = m->x + pad;
    m->ok_y = m->y + h - pad - bh;
    m->ok_w = bw;
    m->ok_h = bh;
    m->cx = m->ok_x + bw + gap;
    m->cy = m->ok_y;
    m->cw = w - 2 * pad - bw - gap;
    if (m->cw < 1) m->cw = 1;
}

static void draw_modal(Notes *n, Surface *s, Font *fr, Font *fd, const Theme *th) {
    if (!n->prompt) return;
    Modal m;
    modal_geom(n, fr, fd, &m);

    surf_blend(s, 0, 0, n->w, n->h, 0x99000000u);
    gfx_rrect_aa(s, (float)m.x, (float)m.y, (float)m.w, (float)m.h,
                 (float)ui_px(10), color_mix(th->window_bg, 0xFF000000u, 0.03f),
                 color_alpha(th->panel_border, 0.9f), 1.0f);

    const char *title = n->prompt == PROMPT_NEW ? "New note" :
                        n->prompt == PROMPT_RENAME ? "Rename note" : "Delete note";
    draw_fit(s, fr, (float)(m.x + ui_px(14)),
             ink_y(fr, (float)(m.y + ui_px(14)), font_height(fr)),
             (float)m.w - 2.0f * (float)ui_px(14), title, th->text);

    if (n->prompt == PROMPT_DELETE) {
        char b[NAME_LEN + 40];
        snprintf(b, sizeof(b), "Delete \"%s\" from /root/notes?", n->prompt_buf);
        draw_fit(s, fd, (float)m.fx, ink_y(fd, (float)m.fy, (float)m.fh),
                 (float)m.fw, b, ERR_COLOR);
    } else {
        char placeholder[NAME_LEN + 30];
        if (n->prompt == PROMPT_NEW) {
            char def[NAME_LEN];
            default_note_name(def, sizeof(def));
            snprintf(placeholder, sizeof(placeholder), "%s", def);
        } else {
            snprintf(placeholder, sizeof(placeholder), "%s", "new name");
        }
        draw_field(s, th, fr, m.fx, m.fy, m.fw, m.fh,
                   n->prompt_buf, placeholder, 1, th->text);
    }

    const char *ok = n->prompt == PROMPT_DELETE ? "Delete" :
                     n->prompt == PROMPT_RENAME ? "Rename" : "Create";
    draw_button(s, th, fd, m.ok_x, m.ok_y, m.ok_w, m.ok_h, ok,
                n->prompt == PROMPT_DELETE ? ERR_COLOR : th->text, 1);
    draw_button(s, th, fd, m.cx, m.cy, m.cw, m.ch, "Cancel",
                color_alpha(th->text, 0.7f), 1);

    int hint_y = m.ok_y - (int)font_height(fd) - ui_px(4);
    if (hint_y > m.fy + m.fh + ui_px(2))
        draw_fit(s, fd, (float)m.fx, (float)hint_y, (float)m.fw,
                 "Enter confirms - Ctrl+Q cancels - Ctrl+U clears",
                 color_alpha(th->text, 0.40f));
}

/* ------------------------------------------------------------
 *  Prompt actions
 * ------------------------------------------------------------ */
static void prompt_new(Notes *n) {
    n->prompt = PROMPT_NEW;
    n->prompt_del = -1;
    n->prompt_len = 0;
    n->prompt_buf[0] = 0;
    n->esc = 0;
}

/* The document the list is pointing at: a search hit's file when the
 * results are showing, otherwise the selected row / open document. */
static int selected_doc_index(const Notes *n) {
    if (n->filter_len && n->hsel >= 0 && n->hsel < n->nhits) {
        int di = doc_index_of(n, n->hits[n->hsel].file);
        if (di >= 0) return di;
    }
    if (n->focus == FOCUS_LIST) return n->dsel;
    return n->cur_doc;
}

static void prompt_rename(Notes *n) {
    int idx = selected_doc_index(n);
    if (idx < 0 || idx >= n->ndocs) {
        set_status(n, 1, "rename: no document selected");
        return;
    }
    n->prompt = PROMPT_RENAME;
    n->prompt_del = idx;
    snprintf(n->prompt_buf, sizeof(n->prompt_buf), "%s", n->docs[idx].name);
    n->prompt_len = (int)strlen(n->prompt_buf);
    n->esc = 0;
}

static void prompt_delete(Notes *n) {
    int idx = selected_doc_index(n);
    if (idx < 0 || idx >= n->ndocs) {
        set_status(n, 1, "delete: no document selected");
        return;
    }
    n->prompt = PROMPT_DELETE;
    n->prompt_del = idx;
    snprintf(n->prompt_buf, sizeof(n->prompt_buf), "%s", n->docs[idx].name);
    n->prompt_len = (int)strlen(n->prompt_buf);
    n->esc = 0;
}

static void prompt_cancel(Notes *n) {
    n->prompt = PROMPT_NONE;
    n->prompt_del = -1;
    n->prompt_len = 0;
    n->prompt_buf[0] = 0;
}

static void prompt_key(Notes *n, unsigned char c) {
    if (c == 0x11) { prompt_cancel(n); return; }

    if (n->prompt == PROMPT_DELETE) {
        if (c == 10 || c == 13) {
            int idx = n->prompt_del;
            n->prompt = PROMPT_NONE;
            doc_delete(n, idx);
        }
        return;
    }

    if (c == 10 || c == 13) {
        char name[NAME_LEN];
        if (n->prompt_len > 0) snprintf(name, sizeof(name), "%s", n->prompt_buf);
        else if (n->prompt == PROMPT_NEW) default_note_name(name, sizeof(name));
        else { set_status(n, 1, "rename: name is empty"); return; }

        int ok;
        if (n->prompt == PROMPT_NEW) ok = doc_create(n, name);
        else                         ok = doc_rename(n, n->prompt_del, name);
        if (ok) prompt_cancel(n);
        return;
    }
    if (c == 0x15 || c == 0x01) {          /* Ctrl+U / Ctrl+A: clear */
        n->prompt_len = 0;
        n->prompt_buf[0] = 0;
        return;
    }
    if (c == 8 || c == 127) {
        if (n->prompt_len > 0) n->prompt_buf[--n->prompt_len] = 0;
        return;
    }
    if (c >= 32 && c != 127 && n->prompt_len < NAME_LEN - 1) {
        n->prompt_buf[n->prompt_len++] = (char)c;
        n->prompt_buf[n->prompt_len] = 0;
    }
}

/* ------------------------------------------------------------
 *  Key handling: fields
 * ------------------------------------------------------------ */
static void filter_key(Notes *n, unsigned char c) {
    if (c == 10 || c == 13) {
        if (n->nhits > 0) open_hit(n, n->hsel);
        else set_status(n, 1, "nothing to open");
        return;
    }
    if (c == 9) { n->focus = FOCUS_LIST; return; }
    if (c == 0x15) {
        n->filter_len = 0;
        n->filter[0] = 0;
        search_all(n);
        return;
    }
    if (c == 8 || c == 127) {
        if (n->filter_len > 0) {
            n->filter_len--;
            n->filter[n->filter_len] = 0;
            search_all(n);
        }
        return;
    }
    if (c >= 32 && n->filter_len < (int)sizeof(n->filter) - 1) {
        n->filter[n->filter_len++] = (char)c;
        n->filter[n->filter_len] = 0;
        search_all(n);
    }
}

static void find_key(Notes *n, unsigned char c, Font *fr, Font *fd) {
    if (c == 9) { n->find_field = n->find_field ? 0 : 1; return; }

    if (c == 0x12) {                 /* Ctrl+R: replace all */
        replace_all(n);
        return;
    }
    if (c == 0x15) {                 /* Ctrl+U: clear this field */
        if (n->find_field == 0) { n->find_len = 0; n->find[0] = 0; }
        else                    { n->repl_len = 0; n->repl[0] = 0; }
        return;
    }
    if (c == 10 || c == 13) {
        if (n->find_field == 1) {
            if (!replace_current(n)) {
                if (n->find_len == 0) set_status(n, 1, "find: type something first");
                else if (!find_next_in(n, n->cur + 1)) set_status(n, 1, "find: no match");
            } else {
                find_next_in(n, n->cur);
            }
        } else {
            if (n->find_len == 0) { set_status(n, 1, "find: type something first"); return; }
            int from = n->cur;
            if (ci_match(n->buf, n->len, n->cur, n->find, n->find_len))
                from = n->cur + n->find_len;
            if (!find_next_in(n, from)) set_status(n, 1, "find: no match");
        }
        ensure_visible(n, fr, fd);
        return;
    }
    if (c == 8 || c == 127) {
        if (n->find_field == 0) {
            if (n->find_len > 0) n->find[--n->find_len] = 0;
        } else {
            if (n->repl_len > 0) n->repl[--n->repl_len] = 0;
        }
        return;
    }
    if (c >= 32) {
        if (n->find_field == 0 && n->find_len < (int)sizeof(n->find) - 1) {
            n->find[n->find_len++] = (char)c;
            n->find[n->find_len] = 0;
        } else if (n->find_field == 1 && n->repl_len < (int)sizeof(n->repl) - 1) {
            n->repl[n->repl_len++] = (char)c;
            n->repl[n->repl_len] = 0;
        }
    }
}

static void list_key(Notes *n, unsigned char c) {
    if (c == 10 || c == 13) {
        if (n->filter_len) open_hit(n, n->hsel);
        else if (n->ndocs > 0) doc_open(n, n->dsel);
        return;
    }
    if (c == 'j') {
        if (n->filter_len) n->hsel++;
        else n->dsel++;
        return;
    }
    if (c == 'k') {
        if (n->filter_len) n->hsel--;
        else n->dsel--;
        return;
    }
}

static void preview_key(Notes *n, unsigned char c, Font *fr) {
    int line = (int)(font_height(fr) * 1.7f);
    if (c == 'j') preview_scroll(n, line);
    else if (c == 'k') preview_scroll(n, -line);
}

/* ------------------------------------------------------------
 *  Key handling: the editor
 * ------------------------------------------------------------ */
static void editor_key(Notes *n, unsigned char c, Font *fr, Font *fd) {
    if (c == 8 || c == 127) {
        erase_one(n);
    } else if (c == 10 || c == 13) {
        insert(n, '\n');
    } else if (c == 9) {
        insert(n, ' ');
        insert(n, ' ');
    } else if (c == 0x04) {
        erase_forward(n);
    } else if (c == 0x01) {
        caret_home_line(n);
    } else if (c == 0x05) {
        caret_end_line(n);
    } else if (c == 0x0E) {
        caret_vertical(n, +1);
    } else if (c == 0x10) {
        caret_vertical(n, -1);
    } else if (c >= 32) {
        insert(n, (char)c);
    }
    ensure_visible(n, fr, fd);
}

/* One place decides what Ctrl+Q backs out of. */
static void global_cancel(Notes *n) {
    if (n->prompt) { prompt_cancel(n); return; }
    if (n->find_open) { n->find_open = 0; return; }
    if (n->focus == FOCUS_FILTER || n->filter_len > 0) {
        n->filter_len = 0;
        n->filter[0] = 0;
        n->hits_more = 0;
        n->nhits = 0;
        n->hsel = 0;
        n->htop = 0;
        n->focus = FOCUS_EDIT;
        return;
    }
    if (n->focus == FOCUS_LIST) { n->focus = FOCUS_EDIT; return; }
    if (n->view_preview) { n->view_preview = 0; return; }
}

static void notes_key(App *a, const char *bytes, int count) {
    Notes *n = a->state;
    if (!n) return;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);

    for (int i = 0; i < count; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (n->esc && esc_feed(n, c, fr, fd)) continue;
        if (c == 0x1B) { n->esc = 1; continue; }

        if (n->prompt) { prompt_key(n, c); continue; }

        if (c == 0x13) {                        /* Ctrl+S: save now */
            if (doc_save(n, 0)) {
                char b[80];
                char clockbuf[16];
                fmt_clock(n->saved_at, clockbuf, sizeof(clockbuf));
                snprintf(b, sizeof(b), "saved %s", clockbuf);
                set_status(n, 0, b);
                if (n->filter_len > 0) search_all(n);
            } else {
                set_status(n, 1, "save failed - buffer kept");
            }
            continue;
        }
        if (c == 0x11) { global_cancel(n); continue; }

        if (c == 0x14 && !n->find_open) {       /* Ctrl+T: preview */
            n->view_preview = !n->view_preview;
            if (!n->view_preview) ensure_visible(n, fr, fd);
            continue;
        }
        if (c == 0x0C) {                        /* Ctrl+L: document list */
            n->focus = FOCUS_LIST;
            n->find_open = 0;
            if (n->filter_len) n->hsel = clampi(n->hsel, 0, n->nhits - 1);
            else n->dsel = n->cur_doc >= 0 ? n->cur_doc : n->dsel;
            continue;
        }
        if (c == 0x07) {                        /* Ctrl+G: filter box */
            n->focus = FOCUS_FILTER;
            n->find_open = 0;
            if (n->filter_len) search_all(n);
            continue;
        }
        /* Ctrl+F opens the find bar from any pane: a user who just
         * searched the directory should not have to click the text first. */
        if (c == 0x06) {
            n->find_open = 1;
            n->find_field = 0;
            n->view_preview = 0;
            n->focus = FOCUS_EDIT;
            ensure_visible(n, fr, fd);
            continue;
        }

        /* Document commands work from every pane, so they cannot be
         * lost by not clicking the list first. In the find bar Ctrl+R
         * keeps its other meaning: replace all. */
        if (c == 0x0E) { prompt_new(n); continue; }
        if (c == 0x12 && !(n->find_open && n->focus == FOCUS_EDIT)) {
            prompt_rename(n);
            continue;
        }
        if (c == 0x18 && !(n->find_open && n->focus == FOCUS_EDIT)) {
            prompt_delete(n);
            continue;
        }

        if (n->focus == FOCUS_FILTER) { filter_key(n, c); continue; }
        if (n->focus == FOCUS_LIST)   { list_key(n, c); continue; }

        if (n->find_open) {
            if (c != 0x13) n->confirm = 0;
            find_key(n, c, fr, fd);
            continue;
        }
        if (n->view_preview) { preview_key(n, c, fr); continue; }

        if (c != 0x13) n->confirm = 0;
        editor_key(n, c, fr, fd);
    }
}

/* ------------------------------------------------------------
 *  Mouse
 * ------------------------------------------------------------ */
static void click_text(Notes *n, const Layout *L, Font *fr, Font *fd,
                       int x, int y) {
    const Grid *g = &L->g;
    float band = ((float)y - g->oy - g->pad_t) / g->lh;
    int row = (int)band;
    if (band < 0.0f || row < 0 || row >= g->vis) return;

    int lines = line_count(n);
    int want = n->top_row + row;
    int pos = line_start_of_index(n, n->top);
    int line = n->top;
    int r = 0;
    float px = g->text_x;
    int best = -1;

    while (line < lines && r <= want) {
        int end = line_end_at(n, pos);
        for (int i = pos; i <= end; ) {
            if (i == end) {
                if (r == want) best = end;
                break;
            }
            char glyph[8];
            int gl = utf8_seq(n, i, end, glyph);
            float cw = font_width(fr, glyph);
            if (px + cw > g->text_x + g->avail && px > g->text_x) {
                if (r == want) { best = i; break; }
                px = g->text_x;
                r++;
                if (r > want) break;
            }
            if (r == want && (float)x < px + cw * 0.5f) { best = i; break; }
            px += cw;
            i += gl > 0 ? gl : 1;
        }
        if (best >= 0 || r > want) break;
        pos = end < n->len ? end + 1 : end;
        line++;
        px = g->text_x;
        r++;
    }

    if (best >= 0) {
        n->cur = best > n->len ? n->len : best;
        note_pcol(n);
        n->find_open = 0;
        ensure_visible(n, fr, fd);
    }
}

static void click_sidebar(Notes *n, const Layout *L, int x, int y) {
    int in_filter = (x >= L->flt_x && x < L->flt_x + L->flt_w &&
                     y >= L->flt_y && y < L->flt_y + L->flt_h);
    if (in_filter) {
        if (n->filter_len && x >= L->cx_x) {
            n->filter_len = 0;
            n->filter[0] = 0;
            search_all(n);
        }
        n->focus = FOCUS_FILTER;
        return;
    }

    if (y >= L->btn_y && y < L->btn_y + L->btn_h) {
        if (x >= L->bnew_x && x < L->bnew_x + L->bnew_w) { prompt_new(n); return; }
        if (x >= L->bren_x && x < L->bren_x + L->bren_w) { prompt_rename(n); return; }
        if (x >= L->bdel_x && x < L->bdel_x + L->bdel_w) { prompt_delete(n); return; }
    }

    if (y < L->list_y || y >= L->list_y + L->list_h) return;

    if (n->filter_len) {
        int idx = n->htop + (y - L->list_y) / L->hit_h;
        if (idx < 0 || idx >= n->nhits) return;
        n->hsel = idx;
        open_hit(n, idx);
    } else {
        int idx = n->dtop + (y - L->list_y) / L->row_h;
        if (idx < 0 || idx >= n->ndocs) return;
        n->dsel = idx;
        n->focus = FOCUS_LIST;
        doc_open(n, idx);
    }
}

static void modal_click(Notes *n, int x, int y, Font *fr, Font *fd) {
    Modal m;
    modal_geom(n, fr, fd, &m);

    int in_ok = (x >= m.ok_x && x < m.ok_x + m.ok_w &&
                 y >= m.ok_y && y < m.ok_y + m.ok_h);
    int in_cancel = (x >= m.cx && x < m.cx + m.cw &&
                     y >= m.cy && y < m.cy + m.ch);

    if (in_cancel) { prompt_cancel(n); return; }
    if (!in_ok) return;

    if (n->prompt == PROMPT_DELETE) {
        int idx = n->prompt_del;
        n->prompt = PROMPT_NONE;
        doc_delete(n, idx);
        return;
    }
    char name[NAME_LEN];
    if (n->prompt_len > 0) snprintf(name, sizeof(name), "%s", n->prompt_buf);
    else if (n->prompt == PROMPT_NEW) default_note_name(name, sizeof(name));
    else return;
    int ok = n->prompt == PROMPT_NEW ? doc_create(n, name)
                                     : doc_rename(n, n->prompt_del, name);
    if (ok) prompt_cancel(n);
}

static void notes_click(App *a, int x, int y, int buttons) {
    Notes *n = a->state;
    if (!n) return;
    /* Remember where the desktop last put the pointer: the wheel event has
     * no position of its own, and this is the only honest record of which
     * pane the user is pointing at. */
    n->ptr_x = x;
    n->ptr_y = y;
    n->ptr_valid = 1;
    if (!(buttons & 1)) return;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fd) return;

    if (n->prompt) { modal_click(n, x, y, fr, fd); return; }

    Layout L;
    layout_build(n, &L, fr, fd);
    list_clamp(n, &L);

    if (x < L.sbw) { click_sidebar(n, &L, x, y); return; }

    if (n->find_open) {
        if (x >= L.ff_x && x < L.ff_x + L.ff_w &&
            y >= L.ff_y && y < L.ff_y + L.ff_h) {
            n->find_field = 0;
            n->focus = FOCUS_EDIT;
            return;
        }
        if (x >= L.rf_x && x < L.rf_x + L.rf_w &&
            y >= L.rf_y && y < L.rf_y + L.rf_h) {
            n->find_field = 1;
            n->focus = FOCUS_EDIT;
            return;
        }
    }

    if (n->view_preview) { n->focus = FOCUS_EDIT; return; }

    if (y >= L.band_y && y < L.band_y + L.band_h) {
        n->focus = FOCUS_EDIT;
        click_text(n, &L, fr, fd, x, y);
    }
}

/* ------------------------------------------------------------
 *  Wheel and resize
 * ------------------------------------------------------------ */
/* Scroll whichever list is showing by three rows a notch (positive = up),
 * keeping the selected row inside the view. The selection is pulled to
 * the edge rather than left behind it, so what the wheel reveals is what
 * Enter acts on. */
static void list_scroll(Notes *n, int rows, int notches) {
    if (rows < 1) rows = 1;
    int step = notches * 3;

    if (n->filter_len > 0) {
        int max = n->nhits - rows;
        if (max < 0) max = 0;
        n->htop -= step;
        if (n->htop > max) n->htop = max;
        if (n->htop < 0) n->htop = 0;
        if (n->nhits < 1) { n->hsel = 0; n->htop = 0; return; }
        if (n->hsel < n->htop) n->hsel = n->htop;
        if (n->hsel > n->htop + rows - 1) n->hsel = n->htop + rows - 1;
        if (n->hsel > n->nhits - 1) n->hsel = n->nhits - 1;
        return;
    }

    int max = n->ndocs - rows;
    if (max < 0) max = 0;
    n->dtop -= step;
    if (n->dtop > max) n->dtop = max;
    if (n->dtop < 0) n->dtop = 0;
    if (n->ndocs < 1) { n->dsel = 0; n->dtop = 0; return; }
    if (n->dsel < n->dtop) n->dsel = n->dtop;
    if (n->dsel > n->dtop + rows - 1) n->dsel = n->dtop + rows - 1;
    if (n->dsel > n->ndocs - 1) n->dsel = n->ndocs - 1;
}

static void notes_wheel(App *a, int notches) {
    Notes *n = a->state;
    if (!n || !notches || n->prompt) return;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fd) return;
    Layout L;
    layout_build(n, &L, fr, fd);

    /* The wheel event carries no position. When the pointer was last seen
     * over the sidebar, that is the list; otherwise the pane with the
     * keyboard - the list when it has focus, the text when it does not. */
    int over_side = n->ptr_valid ? (n->ptr_x >= 0 && n->ptr_x < L.sbw)
                                 : (n->focus == FOCUS_LIST || n->focus == FOCUS_FILTER);
    if (over_side) {
        int rows = n->filter_len ? hit_rows_visible(&L) : doc_rows_visible(&L);
        list_scroll(n, rows, notches);
        return;
    }
    if (n->view_preview) {
        preview_scroll(n, -notches * ui_px(48));
        return;
    }

    int lines = line_count(n);
    int max_top = lines - L.g.vis;
    if (max_top < 0) max_top = 0;
    int top = n->top - notches * 3;
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;
    n->top = top;
    n->top_row = 0;
}

static void notes_resize(App *a, int w, int h) {
    Notes *n = a->state;
    if (!n) return;
    n->w = w;
    n->h = h;
    n->fitted_w = -1;
    n->fitted_h = -1;
    /* The remembered pointer is in the old window's coordinates. */
    n->ptr_valid = 0;
    ensure_visible(n, font_get(FONT_REGULAR, 12), font_get(FONT_REGULAR, 10));
}

/* ------------------------------------------------------------
 *  App lifecycle
 * ------------------------------------------------------------ */
static int import_legacy(Notes *n) {
    struct stat st;
    if (stat(NOTES_LEGACY, &st) != 0 || !S_ISREG(st.st_mode)) return 0;

    char dst[PATH_LEN];
    if (!doc_path(n, "notes.txt", dst, sizeof(dst))) return 0;
    int e = 0;
    long got = copy_stream(NOTES_LEGACY, dst, &e);
    if (got < 0) {
        char b[220];
        snprintf(b, sizeof(b), "import %s: %s (errno %d)",
                 NOTES_LEGACY, strerror(e), e);
        set_status(n, 1, b);
        return 0;
    }
    char b[200];
    snprintf(b, sizeof(b), "imported /root/notes.txt (%ld bytes)", got);
    set_status(n, 0, b);
    return 1;
}

static void notes_init(App *a, int w, int h) {
    Notes *n = calloc(1, sizeof(Notes));
    if (!n) return;
    n->buf = calloc(NOTES_MAX, 1);
    n->scan = calloc(NOTES_MAX, 1);
    if (!n->buf || !n->scan) {
        free(n->buf);
        free(n->scan);
        free(n);
        return;
    }
    n->w = w;
    n->h = h;
    n->disk_len = -1;
    n->cur_doc = -1;
    n->focus = FOCUS_EDIT;
    n->find_field = 0;
    n->fitted_w = -1;
    n->fitted_h = -1;

    if (mkdir(NOTES_DIR, 0700) != 0 && errno != EEXIST) {
        int e = errno;
        snprintf(n->dir_err, sizeof(n->dir_err), "mkdir %s: %s (errno %d)",
                 NOTES_DIR, strerror(e), e);
        set_errno_status(n, "mkdir /root/notes", e);
    }

    docs_scan(n);

    if (n->ndocs == 0 && n->dir_err[0] == 0 && import_legacy(n))
        docs_scan(n);

    if (n->ndocs == 0 && n->dir_err[0] == 0) {
        char fresh[NAME_LEN];
        default_note_name(fresh, sizeof(fresh));
        char path[PATH_LEN];
        if (doc_path(n, fresh, path, sizeof(path))) {
            int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) close(fd);
            docs_scan(n);
        }
    }

    if (n->ndocs > 0) doc_open(n, 0);
    n->dsel = n->cur_doc >= 0 ? n->cur_doc : 0;
    if (!n->status[0]) set_status(n, 0, "Ready");
    a->state = n;
}

static void notes_free(App *a) {
    Notes *n = a->state;
    if (!n) return;
    /* Best effort on close: the buffer is about to disappear. */
    if (n->dirty && !n->partial && n->cur_doc >= 0) doc_save(n, 0);
    free(n->buf);
    free(n->scan);
    free(n);
    a->state = NULL;
}

/* ------------------------------------------------------------
 *  Draw
 * ------------------------------------------------------------ */
static void notes_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    if (!s || w <= 0 || h <= 0) return;
    Notes *n = a->state;
    if (!n) return;
    n->w = w;
    n->h = h;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fb = font_get(FONT_SEMIBOLD, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fb || !fd) return;

    autosave_tick(n);

    Layout L;
    layout_build(n, &L, fr, fd);
    list_clamp(n, &L);

    /* A row that has just scrolled into view has not been stat'd by any
     * sweep yet. Ask for one instead of showing a size from an older
     * directory scan; scrolling is a user action, so the cost is bounded by
     * the gesture and not by the frame rate. */
    if (n->dtop != n->last_dtop || n->htop != n->last_htop) {
        docs_stat_stale(n);
        n->last_dtop = n->dtop;
        n->last_htop = n->htop;
    }

    /* The document's size and mtime, re-read when the metadata cache is due.
     * The values are still the real st_size/st_mtime of the real file - they
     * are just no more than a second old, instead of one stat per frame. */
    int stat_now = docs_stat_due(n);
    if (stat_now && n->cur_doc >= 0 && n->path[0]) {
        struct stat st;
        if (stat(n->path, &st) == 0) {
            n->disk_len = (long)st.st_size;
            n->disk_mtime = st.st_mtime;
            if (n->cur_doc < n->ndocs) {
                n->docs[n->cur_doc].size = (long)st.st_size;
                n->docs[n->cur_doc].mtime = st.st_mtime;
            }
        } else {
            n->disk_len = -1;
        }
    }

    if (n->fitted_w != w || n->fitted_h != h) {
        ensure_visible(n, fr, fd);
        n->fitted_w = w;
        n->fitted_h = h;
    }

    /* Every painter below works in app-local coordinates (0,0 is the
     * window's content origin). The desktop hands over the whole screen
     * surface with the window as a clip rectangle in *screen* coordinates,
     * so the surface must be rebased and the clip translated, or a window
     * at (140,110) would have its top-left clipped away. Both extents are
     * clamped to the real surface so a window that pokes off the screen
     * cannot write past the framebuffer. */
    if (x < 0 || y < 0) return;
    int vw = w, vh = h;
    if (vw > s->w - x) vw = s->w - x;
    if (vh > s->h - y) vh = s->h - y;
    if (vw <= 0 || vh <= 0) return;

    Surface view = *s;
    view.px = s->px + (size_t)y * (size_t)s->stride + (size_t)x;
    view.w = vw;
    view.h = vh;
    if (s->cx1 > s->cx0) {
        int cx0 = s->cx0 - x, cy0 = s->cy0 - y;
        int cx1 = s->cx1 - x, cy1 = s->cy1 - y;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > vw) cx1 = vw;
        if (cy1 > vh) cy1 = vh;
        if (cx1 <= cx0 || cy1 <= cy0) return;
        surf_clip(&view, cx0, cy0, cx1 - cx0, cy1 - cy0);
    } else {
        surf_unclip(&view);
    }

    surf_fill(&view, 0, 0, w, h, color_mix(th->window_bg, 0xFF000000u, 0.18f));

    draw_sidebar(n, &view, &L, fr, fb, fd, th, stat_now);
    draw_editor(n, &view, &L, fr, fd, th);
    draw_modal(n, &view, fr, fd, th);
}

/* The AppVT is unchanged: the desktop still asks for "notes" and gets
 * the same table. All the new behaviour lives in the state above. */
const AppVT app_vt_notes = {
    .id = "notes",
    .title = "Notes",
    .init = notes_init,
    .draw = notes_draw,
    .key = notes_key,
    .click = notes_click,
    .resize = notes_resize,
    .wheel = notes_wheel,
    .free = notes_free,
};
