/* ============================================================
 *  Mystic OS - apps/notes.c
 *  A text editor that writes to the disk, so notes survive a
 *  reboot. /root is a real filesystem, so the bytes really land
 *  on a device and not in a variable.
 *
 *  The buffer is a fixed 64 KB block: no allocation ever grows
 *  with the file. On top of that buffer:
 *
 *    - a line-number gutter, numbered by counting '\n';
 *    - a scroll window (first visible line, plus the first visible
 *      wrapped row inside that line) that keeps the caret on screen;
 *    - arrow / PageUp / PageDown / Home / End editing, driven either
 *      by the usual xterm escape sequences or, for windows that only
 *      forward text, by the Ctrl chords;
 *    - a footer that reports the real st_size of the file next to the
 *      buffer length and says "unsaved" when the two disagree.
 *
 *  Ctrl+S writes the whole 64 KB buffer into /root/notes.txt.tmp,
 *  fsyncs that file, closes it and rename()s it over /root/notes.txt.
 *  Until the rename succeeds the file that is already on the disk is
 *  untouched, so a full disk, a read-only /root or a short write can
 *  never eat the notes that were there. Every call is checked; when
 *  one fails the buffer stays dirty and the footer shows the kernel's
 *  own strerror() text instead of claiming a save.
 *
 *  A file larger than the buffer is only ever overwritten on purpose:
 *  the first Ctrl+S warns with the real byte counts and a second one,
 *  with no edit in between, goes ahead.
 * ============================================================ */
#include "mystic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define NOTES_MAX   65536
#define LINE_H      16.0f
#define TOP_PAD     6      /* space above the first text row        */
#define FOOT_H      26     /* footer band, rule plus one 10 px line */
#define TEXT_GAP    8      /* gap between gutter rule and glyphs    */
#define GUTTER_PAD  6      /* gutter's own left inset               */
#define ERR_COLOR   0xFFFF7B72u  /* footer text when a save failed   */

typedef struct {
    char  *buf;
    int    len;
    int    cur;        /* caret: byte offset in buf, 0..len          */
    int    w, h;
    int    dirty;
    int    saved_here; /* we wrote the file this session            */
    time_t saved_at;   /* clock at that write                       */
    time_t disk_mtime; /* st_mtime of the file as last stat'd       */
    long   disk_len;   /* st_size of the file, -1 when absent        */
    int    top;        /* index of the first visible logical line    */
    int    top_row;    /* first visible wrapped row inside that line */
    int    fitted_w, fitted_h;  /* size top/top_row were last fitted for */
    int    pcol;       /* preferred byte column, for up/down         */
    /* What the last save or load actually did. err[] empty == no
     * failure; otherwise it holds the real strerror() text and the
     * footer shows it instead of "saved". */
    char   err[128];
    int    partial;    /* the buffer is only part of the file on disk */
    int    confirm;    /* warned once; the next Ctrl+S may overwrite  */
    /* Escape-sequence reader. An escape can be split over two key
     * events, so the state lives in the app, not on the stack. */
    int    esc;        /* 0 idle, 1 after ESC, 2 in CSI, 3 after ESC O */
    int    esc_param;
    int    esc_digits;
    int    esc_mod;    /* inside the second (modifier) parameter     */
} Notes;

static const char *NOTES_PATH = "/root/notes.txt";

/* ------------------------------------------------------------
 *  Buffer geometry
 * ------------------------------------------------------------ */

/* Byte offset of the first byte of the line containing pos. */
static int line_start_at(const Notes *n, int pos) {
    if (pos > n->len) pos = n->len;
    if (pos < 0) pos = 0;
    while (pos > 0 && n->buf[pos - 1] != '\n') pos--;
    return pos;
}

/* Byte offset of the newline that ends pos's line (or len). */
static int line_end_at(const Notes *n, int pos) {
    if (pos > n->len) pos = n->len;
    if (pos < 0) pos = 0;
    while (pos < n->len && n->buf[pos] != '\n') pos++;
    return pos;
}

/* 0-based logical line index of pos: the number of newlines before it. */
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

/* Byte offset where logical line `line` starts. */
static int line_start_of_index(const Notes *n, int line) {
    if (line <= 0) return 0;
    int pos = 0;
    while (line > 0) {
        pos = line_end_at(n, pos);
        if (pos < n->len) pos++;        /* step over the newline */
        line--;
        if (pos >= n->len) return n->len;
    }
    return pos;
}

/* How many rows one logical line occupies once wrapped at width avail. */
static int rows_of_line(const Notes *n, Font *fr, int start, int end, float avail) {
    float px = 0;
    int   rows = 1;
    char  one[2] = { 0, 0 };
    if (end > n->len) end = n->len;
    for (int i = start; i < end; i++) {
        one[0] = n->buf[i];
        float cw = font_width(fr, one);
        if (px + cw > avail && px > 0) { px = 0; rows++; }
        px += cw;
    }
    return rows;
}

/* Width of the number gutter. Fixed while the line count keeps the
 * same number of digits. */
static float gutter_width_px(const Notes *n, Font *fd) {
    int lines = line_count(n);
    int digits = 1;
    while (lines >= 10) { lines /= 10; digits++; }
    if (digits < 2) digits = 2;
    return (float)GUTTER_PAD + (float)digits * font_width(fd, "0") + (float)TEXT_GAP;
}

/* Width available to text after the gutter. */
static float text_avail_px(const Notes *n, Font *fd) {
    float avail = (float)n->w - gutter_width_px(n, fd) - (float)TEXT_GAP;
    return avail < 24.0f ? 24.0f : avail;
}

/* Shorten s in place, with a trailing "..", until it fits maxw. A
 * strerror() message can be long and the window can be narrow: text
 * must never spill out of the app's rect. */
static void fit_footer(char *s, size_t cap, Font *fd, float maxw) {
    if (font_width(fd, s) <= maxw) return;
    size_t len = strlen(s);
    char cut[192];
    while (len > 0) {
        len--;
        snprintf(cut, sizeof(cut), "%.*s..", (int)len, s);
        if (font_width(fd, cut) <= maxw) {
            snprintf(s, cap, "%s", cut);
            return;
        }
    }
    s[0] = 0;
}

/* Rows of text that fit between the top padding and the footer. */
static int visible_rows(const Notes *n) {
    int rows = (int)((float)(n->h - FOOT_H) / LINE_H);
    return rows < 1 ? 1 : rows;
}

/* Row of the caret measured from the top of the visible band, or -1
 * if the caret sits above it. *rows_top_line gets the height of the
 * top line, so the caller can normalise top_row. */
static int caret_row_from_top(Notes *n, Font *fr, float avail, int *rows_top_line) {
    int caret_line = line_index_at(n, n->cur);
    if (caret_line < n->top) return -1;

    int pos = line_start_of_index(n, n->top);
    int end = line_end_at(n, pos);
    int top_rows = rows_of_line(n, fr, pos, end, avail);
    if (rows_top_line) *rows_top_line = top_rows;

    int rows = 0;
    int line = n->top;
    while (line < caret_line) {
        rows += rows_of_line(n, fr, pos, end, avail);
        pos = end < n->len ? end + 1 : end;
        end = line_end_at(n, pos);
        line++;
    }
    rows += rows_of_line(n, fr, pos, n->cur, avail) - 1;
    return rows - n->top_row;
}

/* ------------------------------------------------------------
 *  Caret movement
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

/* Move a line up (dir < 0) or down, keeping the preferred column. */
static void caret_vertical(Notes *n, int dir) {
    int line = line_index_at(n, n->cur);
    int total = line_count(n);
    int target = line + dir;
    if (target < 0) {
        n->cur = 0;
        n->pcol = 0;
        return;
    }
    if (target >= total) {
        n->cur = n->len;
        note_pcol(n);
        return;
    }
    int start = line_start_of_index(n, target);
    int end = line_end_at(n, start);
    int col = n->pcol;
    if (col > end - start) col = end - start;
    n->cur = start + col;
}

/* Keep the caret inside the visible band by moving the window:
 * n->top is the first visible line, n->top_row the first visible
 * wrapped row inside it. */
static void ensure_visible(Notes *n, Font *fr, Font *fd) {
    if (!fr || !fd) return;

    int lines = line_count(n);
    if (n->top > lines - 1) { n->top = lines - 1; n->top_row = 0; }
    if (n->top < 0) { n->top = 0; n->top_row = 0; }
    if (n->top_row < 0) n->top_row = 0;

    float avail = text_avail_px(n, fd);
    int   vis = visible_rows(n);
    int   caret_line = line_index_at(n, n->cur);

    if (caret_line < n->top) {         /* caret above: pull the window up */
        n->top = caret_line;
        n->top_row = 0;
    }

    int top_rows = 1;
    int row = caret_row_from_top(n, fr, avail, &top_rows);

    if (row < 0) {                     /* caret row scrolled above the band */
        n->top_row += row;
        if (n->top_row < 0) n->top_row = 0;
        row = caret_row_from_top(n, fr, avail, &top_rows);
        if (row < 0) row = 0;
    }
    if (row >= vis)                    /* caret row below the band */
        n->top_row += row - vis + 1;

    /* Normalise: whole lines leave the top instead of an impossible row. */
    int guard = 0;
    while (n->top_row > 0 && n->top_row >= top_rows && n->top < caret_line) {
        n->top_row -= top_rows;
        n->top++;
        int s = line_start_of_index(n, n->top);
        top_rows = rows_of_line(n, fr, s, line_end_at(n, s), avail);
        if (++guard > 100000) break;
    }
    if (n->top_row >= top_rows) n->top_row = top_rows - 1;
    if (n->top_row < 0) n->top_row = 0;
}

/* PageUp (dir < 0) / PageDown: move the window a screenful of lines
 * and take the caret with it, so the two never disagree. */
static void caret_page(Notes *n, Font *fr, Font *fd, int dir) {
    int lines = line_count(n);
    int vis = visible_rows(n);
    int line = line_index_at(n, n->cur);
    int target = line + dir * vis;
    if (target < 0) target = 0;
    if (target > lines - 1) target = lines - 1;

    /* A page moves the window, but never past the last page: on a file
     * shorter than a screen the view stays anchored at the top. PageUp
     * is never pulled forwards by this clamp. */
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
 *  Editing
 * ------------------------------------------------------------ */

static void insert(Notes *n, char c) {
    if (n->len >= NOTES_MAX - 1) return;
    memmove(n->buf + n->cur + 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->buf[n->cur] = c;
    n->cur++;
    n->len++;
    n->dirty = 1;
    note_pcol(n);
}

static void erase_one(Notes *n) {
    if (n->cur <= 0) return;
    memmove(n->buf + n->cur - 1, n->buf + n->cur, (size_t)(n->len - n->cur));
    n->cur--;
    n->len--;
    n->dirty = 1;
    note_pcol(n);
}

static void erase_forward(Notes *n) {
    if (n->cur >= n->len) return;
    memmove(n->buf + n->cur, n->buf + n->cur + 1, (size_t)(n->len - n->cur - 1));
    n->len--;
    n->dirty = 1;
    note_pcol(n);
}

/* Record a failed operation. errno must still be the failing call's. */
static void save_error(Notes *n, const char *op) {
    int e = errno;
    snprintf(n->err, sizeof(n->err), "%s: %s", op, strerror(e));
}

/* Replace NOTES_PATH atomically: write the whole buffer to a sibling
 * temp file, flush it all the way to the device, close it, and only
 * then rename() it over the real path. rename() inside one directory
 * is atomic, so every failure below leaves the old notes.txt intact
 * and there is never a moment when the file is half written. On any
 * failure the buffer stays dirty and err[] carries the real errno. */
static void save(Notes *n) {
    char   tmp[128];
    int    fd = -1;
    size_t off = 0;

    n->err[0] = 0;

    /* The buffer holds only the first NOTES_MAX-1 bytes of a bigger
     * file: writing it back would delete the rest. Say so with the
     * real sizes first, and only overwrite on an explicit second
     * Ctrl+S (any other keypress clears the confirmation). */
    if (n->partial) {
        struct stat st;
        if (stat(NOTES_PATH, &st) == 0 && (long)st.st_size > (long)n->len) {
            if (!n->confirm) {
                n->confirm = 1;
                snprintf(n->err, sizeof(n->err),
                         "disk has %ld bytes, loaded %d - Ctrl+S again to overwrite",
                         (long)st.st_size, n->len);
                n->dirty = 1;
                return;
            }
        } else {
            n->partial = 0;     /* the file shrank: the buffer is whole */
        }
    }

    /* The temp sits in the same directory as the target, which is what
     * makes the rename atomic (a different filesystem would not be). */
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", NOTES_PATH) >= (int)sizeof(tmp)) {
        snprintf(n->err, sizeof(n->err), "save: temp path too long");
        n->dirty = 1;
        return;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        save_error(n, "open .tmp");
        n->dirty = 1;
        return;
    }

    while (off < (size_t)n->len) {
        ssize_t w = write(fd, n->buf + off, (size_t)n->len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            save_error(n, "write .tmp");
            close(fd);
            unlink(tmp);
            n->dirty = 1;
            return;
        }
        if (w == 0) {           /* no progress, no errno: a real failure */
            snprintf(n->err, sizeof(n->err),
                     "write .tmp: stopped after %ld of %d bytes",
                     (long)off, n->len);
            close(fd);
            unlink(tmp);
            n->dirty = 1;
            return;
        }
        off += (size_t)w;
    }

    /* Without this the bytes sit in the page cache. Pull the plug and
     * they are gone - which is exactly what happened the first time
     * this was tested. */
    if (fsync(fd) != 0) {
        save_error(n, "fsync .tmp");
        close(fd);
        unlink(tmp);
        n->dirty = 1;
        return;
    }
    if (close(fd) != 0) {       /* close reports deferred write errors */
        save_error(n, "close .tmp");
        unlink(tmp);
        n->dirty = 1;
        return;
    }

    if (rename(tmp, NOTES_PATH) != 0) {
        save_error(n, "rename");
        unlink(tmp);
        n->dirty = 1;
        return;
    }

    /* Only now, after the successful rename, is the save a fact. */
    n->dirty = 0;
    n->partial = 0;
    n->confirm = 0;
    n->saved_here = 1;
    n->saved_at = time(NULL);

    /* A stat failure here cannot undo the rename; draw() re-reads the
     * real size every frame, so the footer corrects itself. */
    struct stat st;
    n->disk_len = (stat(NOTES_PATH, &st) == 0) ? (long)st.st_size : -1;
    if (n->disk_len >= 0) n->disk_mtime = st.st_mtime;
}

/* ------------------------------------------------------------
 *  Escape sequences: ESC [ A .. ESC [ 6 ~
 * ------------------------------------------------------------ */

static void esc_dispatch(Notes *n, int param, int final, Font *fr, Font *fd) {
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
        default:
            break;
    }
    ensure_visible(n, fr, fd);
}

/* Feed one byte to the escape reader. Returns 1 when the byte belonged
 * to a sequence (and so must not be typed), 0 when the reader gave up
 * and the byte is ordinary text. */
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
                /* Digits after a ';' are a modifier (Shift/Ctrl/Alt):
                 * skipped, so ESC [ 5 ; 2 ~ is still a plain PageUp. */
                if (!n->esc_mod && n->esc_digits < 4) {
                    n->esc_param = n->esc_param * 10 + (c - '0');
                    n->esc_digits++;
                }
                return 1;
            }
            if (c == ';') { n->esc_mod = 1; return 1; }
            n->esc = 0;
            esc_dispatch(n, n->esc_digits ? n->esc_param : 0, c, fr, fd);
            return 1;
        case 3:
            n->esc = 0;
            esc_dispatch(n, 0, c, fr, fd);
            return 1;
        default:
            n->esc = 0;
            return 0;
    }
}

/* ------------------------------------------------------------
 *  App lifecycle
 * ------------------------------------------------------------ */

static void notes_init(App *a, int w, int h) {
    Notes *n = calloc(1, sizeof(Notes));
    if (!n) return;
    n->buf = calloc(NOTES_MAX, 1);
    if (!n->buf) { free(n); return; }
    n->w = w;
    n->h = h;
    n->disk_len = -1;

    /* The size on disk is needed before the read: it is the only way
     * to tell a complete load from one that hit the 64 KB cap. */
    struct stat st;
    int have_st = (stat(NOTES_PATH, &st) == 0) ? 1 : 0;

    FILE *f = fopen(NOTES_PATH, "r");
    if (f) {
        size_t got = fread(n->buf, 1, NOTES_MAX - 1, f);
        int    bad = ferror(f) ? 1 : 0;
        int    rerr = errno;
        fclose(f);
        n->len = (int)got;
        n->cur = 0;
        if (bad) {
            /* A partial read is not the file. Never let a later Ctrl+S
             * turn a half-read buffer into the whole file. */
            snprintf(n->err, sizeof(n->err), "read: %s", strerror(rerr));
            n->partial = 1;
            n->dirty = 1;
        } else if (have_st && (long)st.st_size > (long)got) {
            snprintf(n->err, sizeof(n->err),
                     "disk has %ld bytes, loaded %d - Ctrl+S overwrites",
                     (long)st.st_size, (int)got);
            n->partial = 1;
            n->dirty = (got == 0) ? 1 : 0;
        }
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
        /* ENOENT is simply "no notes yet". Anything else means a file
         * is there and we could not read it, so say so and treat the
         * buffer as partial: Ctrl+S will ask before replacing it. */
        if (errno != ENOENT) {
            save_error(n, NOTES_PATH);
            n->partial = 1;
        }
    }
    n->top = 0;
    n->top_row = 0;
    n->pcol = 0;
    n->esc = 0;
    n->saved_here = 0;

    if (have_st) {
        n->disk_len = (long)st.st_size;
        n->disk_mtime = st.st_mtime;
    } else {
        n->disk_len = -1;
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

static void notes_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Notes *n = a->state;
    if (!n) return;
    n->w = w;
    n->h = h;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fd) return;

    /* The real file, every frame. This is the only source of truth for
     * what is on the disk; it is not cached from the last save. */
    struct stat st;
    if (stat(NOTES_PATH, &st) == 0) {
        n->disk_len = (long)st.st_size;
        n->disk_mtime = st.st_mtime;
    } else {
        n->disk_len = -1;
    }

    surf_fill(s, x, y, w, h, color_mix(th->window_bg, 0xFF000000u, 0.25f));

    /* The window may have been resized since the last keystroke: re-fit
     * the scroll window before painting, or a shorter/wider window would
     * hide the caret or leave top_row past the real row count. */
    if (n->fitted_w != w || n->fitted_h != h) {
        ensure_visible(n, fr, fd);
        n->fitted_w = w;
        n->fitted_h = h;
    }

    int   lines = line_count(n);
    float gutter = gutter_width_px(n, fd);
    float avail = text_avail_px(n, fd);
    int   vis = visible_rows(n);
    int   body_h = h - FOOT_H > 0 ? h - FOOT_H : 0;

    if (n->top > lines - 1) { n->top = lines - 1; n->top_row = 0; }
    if (n->top < 0) { n->top = 0; n->top_row = 0; }
    if (n->top_row < 0) n->top_row = 0;

    /* Gutter tint and its rule, so the numbers read as a margin. */
    surf_blend(s, x, y, (int)gutter, body_h, color_alpha(th->panel_border, 0.10f));
    surf_blend(s, x + (int)gutter - 1, y, 1, body_h, color_alpha(th->panel_border, 0.55f));

    int   caret_line = line_index_at(n, n->cur);
    int   pos = line_start_of_index(n, n->top);
    int   line = n->top;
    int   row = 0;
    int   base = n->top_row;
    int   last_row = base + vis;
    float left = (float)x + gutter + (float)TEXT_GAP;
    float px = left;
    char  one[2] = { 0, 0 };

    while (line < lines && row < last_row) {
        int end = line_end_at(n, pos);

        /* The line number rides the first visible row of the line: if
         * the line started above the band, that is the band's top row. */
        int num_row = row < base ? base : row;
        if (num_row < last_row) {
            char num[16];
            snprintf(num, sizeof(num), "%d", line + 1);
            float num_x = (float)x + gutter - (float)TEXT_GAP - font_width(fd, num);
            float num_y = (float)y + (float)TOP_PAD + (float)(num_row - base) * LINE_H
                        + (LINE_H - font_height(fd)) * 0.5f;
            u32 num_col = (line == caret_line) ? color_alpha(th->text, 0.95f)
                                               : color_alpha(th->text, 0.38f);
            font_draw(s, fd, num_x, num_y, num, num_col);
        }

        for (int i = pos; i < end && row < last_row; i++) {
            int at_caret = (i == n->cur);
            one[0] = n->buf[i];
            float cw = font_width(fr, one);
            if (px + cw > left + avail && px > left) {
                px = left;
                row++;
                if (row >= last_row) break;
            }
            float py = (float)y + (float)TOP_PAD + (float)(row - base) * LINE_H;
            if (row >= base) {
                if (at_caret) surf_blend(s, (int)px, (int)py, 1, (int)LINE_H, th->accent);
                font_draw(s, fr, px, py, one, th->text);
            }
            px += cw;
        }

        /* Caret parked just before the newline, or at the end of the buffer. */
        if (n->cur == end && row >= base && row < last_row) {
            float py = (float)y + (float)TOP_PAD + (float)(row - base) * LINE_H;
            surf_blend(s, (int)px, (int)py, 1, (int)LINE_H, th->accent);
        }

        pos = end < n->len ? end + 1 : end;
        line++;
        if (pos >= n->len && line >= lines) break;
        px = left;
        row++;
    }

    /* Footer: rule, then buffer length against the size on disk. */
    surf_blend(s, x + 4, (int)(y + h) - 20, w - 8, 1, th->panel_border);

    char foot[192];
    int unsaved = n->dirty || (n->disk_len >= 0 && n->disk_len != (long)n->len);
    if (n->err[0]) {
        /* A real failure with a real errno: say that, never "saved". */
        snprintf(foot, sizeof(foot), "not saved - %s", n->err);
    } else if (n->disk_len < 0) {
        snprintf(foot, sizeof(foot), "%d bytes buf  -  no file on disk  -  Ctrl+S saves", n->len);
    } else if (unsaved) {
        snprintf(foot, sizeof(foot), "%d bytes buf  -  %ld bytes on disk  -  unsaved",
                 n->len, n->disk_len);
    } else {
        long ago;
        if (n->saved_here) {
            /* This session wrote it: measure from our own save. */
            ago = (long)(time(NULL) - n->saved_at);
            snprintf(foot, sizeof(foot), "%d bytes buf  -  %ld bytes on disk  -  saved %ld s ago",
                     n->len, n->disk_len, ago < 0 ? 0L : ago);
        } else {
            /* Never saved here: the age comes from the file's own mtime. */
            ago = (long)(time(NULL) - n->disk_mtime);
            snprintf(foot, sizeof(foot), "%d bytes buf  -  %ld bytes on disk  -  written %ld s ago",
                     n->len, n->disk_len, ago < 0 ? 0L : ago);
        }
    }
    if (!n->err[0] && font_width(fd, foot) > (float)(w - 16)) {   /* narrow window: shorten */
        if (n->disk_len < 0)
            snprintf(foot, sizeof(foot), "%d b buf  -  no disk file", n->len);
        else
            snprintf(foot, sizeof(foot), "%d b buf  -  %ld b disk%s",
                     n->len, n->disk_len, unsaved ? "  -  unsaved" : "");
    }
    fit_footer(foot, sizeof(foot), fd, (float)(w - 16));
    font_draw(s, fd, (float)x + 8, (float)(y + h) - 16, foot,
              n->err[0] ? ERR_COLOR : color_alpha(th->text, 0.55f));
}

static void notes_key(App *a, const char *bytes, int count) {
    Notes *n = a->state;
    if (!n) return;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);

    for (int i = 0; i < count; i++) {
        unsigned char c = (unsigned char)bytes[i];
        int code = (int)c;

        if (n->esc && esc_feed(n, c, fr, fd)) continue;
        if (code == 0x1B) { n->esc = 1; continue; }

        /* One keypress that is not Ctrl+S cancels an "overwrite the
         * bigger file?" warning: the user has touched the document
         * since the warning, so it has to be earned again. This sits
         * above the edit path on purpose - an edit dropped because the
         * buffer is full still counts as an attempt to change it. */
        if (code != 0x13) n->confirm = 0;

        if (code == 0x13) {                        /* Ctrl+S */
            save(n);
        } else if (code == 0x08 || code == 0x7F) { /* Backspace */
            erase_one(n);
        } else if (code == 10 || code == 13) {     /* Enter */
            insert(n, 10);
        } else if (code == 9) {                    /* Tab */
            insert(n, ' ');
            insert(n, ' ');
        } else if (code == 0x10) {                 /* Ctrl+P: line up */
            caret_vertical(n, -1);
        } else if (code == 0x0E) {                 /* Ctrl+N: line down */
            caret_vertical(n, +1);
        } else if (code == 0x02) {                 /* Ctrl+B: page up */
            caret_page(n, fr, fd, -1);
        } else if (code == 0x06) {                 /* Ctrl+F: page down */
            caret_page(n, fr, fd, +1);
        } else if (code == 0x01) {                 /* Ctrl+A: line start */
            caret_home_line(n);
        } else if (code == 0x05) {                 /* Ctrl+E: line end */
            caret_end_line(n);
        } else if (code == 0x04) {                 /* Ctrl+D: delete forward */
            erase_forward(n);
        } else if (code >= 32 && code < 127) {
            insert(n, (char)code);
        }
        ensure_visible(n, fr, fd);
    }
}

/* Click in the text: put the caret on the clicked row at the clicked
 * byte, using the same wrap arithmetic the painter uses. */
static void notes_click(App *a, int x, int y, int buttons) {
    Notes *n = a->state;
    if (!n || !(buttons & 1)) return;

    Font *fr = font_get(FONT_REGULAR, 12);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fr || !fd) return;

    int band = (int)(((float)y - (float)TOP_PAD) / LINE_H);
    if (band < 0 || band >= visible_rows(n)) return;   /* outside the text */

    float avail = text_avail_px(n, fd);
    float left = gutter_width_px(n, fd) + (float)TEXT_GAP;
    int   lines = line_count(n);
    int   want = n->top_row + band;    /* row measured from the top line */

    int pos = line_start_of_index(n, n->top);
    int line = n->top;
    int r = 0;
    float px = left;
    int best = -1;

    while (line < lines && r <= want) {
        int end = line_end_at(n, pos);
        for (int i = pos; i <= end; i++) {
            if (i == end) {                    /* end-of-line position */
                if (r == want) best = end;
                break;
            }
            char one[2] = { n->buf[i], 0 };
            float cw = font_width(fr, one);
            if (px + cw > left + avail && px > left) {
                if (r == want) { best = i; break; }     /* row ends at a wrap */
                px = left;
                r++;
                if (r > want) break;
            }
            if (r == want && x < px + cw * 0.5f) { best = i; break; }
            px += cw;
        }
        if (best >= 0 || r > want) break;
        pos = end < n->len ? end + 1 : end;
        line++;
        px = left;
        r++;
    }

    if (best >= 0) {
        n->cur = best > n->len ? n->len : best;
        note_pcol(n);
        ensure_visible(n, fr, fd);
    }
}

static void notes_resize(App *a, int w, int h) {
    Notes *n = a->state;
    if (!n) return;
    n->w = w;
    n->h = h;
    /* A resize changes the row budget: re-fit the scroll window at once,
     * not on the next keystroke, or the caret can sit outside the band. */
    ensure_visible(n, font_get(FONT_REGULAR, 12), font_get(FONT_REGULAR, 10));
    n->fitted_w = w;
    n->fitted_h = h;
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
