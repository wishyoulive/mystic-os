/* ============================================================
 *  Mystic OS - term.c
 *  A terminal: a real pty, a real shell, and a VT100-ish screen.
 *
 *  We fork a pty pair, run /bin/sh on the slave side, and read the
 *  bytes it writes. Those bytes are text plus escape sequences; this
 *  file turns them into a grid of cells we can draw.
 * ============================================================ */
#include "mystic.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <pty.h>

#define TERM_MAX_COLS 400
#define TERM_MAX_ROWS 200

/* No CSI parameter may exceed this. Every cursor coordinate is clamped to
 * the screen, so a larger number can never mean anything, and capping it
 * here keeps (cursor + delta) well inside int - no sequence can wrap. */
#define TERM_PARAM_MAX 1000000
/* An OSC string longer than this is abandoned. Title sequences are short,
 * and a stream that never sends BEL/ST must not swallow output forever. */
#define TERM_OSC_MAX 512

typedef struct {
    u32 ch;
    u8  fg, bg, bold;
} Cell;

#define HIST_ROWS 600      /* lines of scrollback per terminal */

struct Term {
    int   fd;
    pid_t pid;
    int   pidfd;        /* pidfd_open() handle for pid: names the process
                         * instance, not a number (-1 when unavailable) */
    int   cols, rows;

    Cell *grid;          /* rows * cols */
    Cell *hist;          /* ring buffer of lines that scrolled off the top */
    int   hist_head;     /* next slot to write in hist */
    int   hist_count;    /* valid lines in hist */
    int   scroll;        /* lines scrolled back from the live screen */
    int   cx, cy;        /* cursor; always inside the grid */
    int   wrap;          /* last column printed: wrap before the next glyph */
    u8    fg, bg, bold;

    /* UTF-8 decoder: bytes of a multi-byte character are accumulated here
     * and become one cell when the sequence completes (or U+FFFD). */
    u32   u8_cp;
    int   u8_need;       /* continuation bytes still expected, 0 = idle */
    u32   u8_min;        /* smallest legal codepoint for this length */

    /* parser state */
    int   state;         /* 0 normal, 1 ESC, 2 CSI, 3 OSC, 4 OSC+ESC, 5 charset */
    char  params[64];
    int   plen;
    int   csi_priv;      /* '?', '>', '<', '=' prefix: parse, then ignore */
    int   csi_over;      /* parameter buffer overflowed: drop the sequence */
    int   osc_len;       /* bytes seen in the current OSC string */

    int   alive;
    int   dirty;
    int   scroll_off;    /* reserved for scrollback */
};

/* ---------------------------------------------------------- palette */
static const u32 PALETTE[16] = {
    0xFF1A1D23u, 0xFFE06C75u, 0xFF98C379u, 0xFFE5C07Bu,
    0xFF61AFEFu, 0xFFC678DDu, 0xFF56B6C2u, 0xFFABB2BFu,
    0xFF4B5263u, 0xFFFF7B86u, 0xFFB5E890u, 0xFFFFD68Au,
    0xFF82C7FFu, 0xFFE29BF0u, 0xFF7FE3EFu, 0xFFFFFFFFu
};

static u32 cell_fg(const Cell *c) {
    if (c->fg < 16) return PALETTE[c->fg];
    return 0xFFDDDDDDu;
}
static u32 cell_bg(const Cell *c) {
    if (c->bg < 16) return PALETTE[c->bg];
    return 0xFF10131Au;
}

/* ---------------------------------------------------------- screen */
static Cell *cell_at(Term *t, int x, int y) {
    if (x < 0 || x >= t->cols || y < 0 || y >= t->rows) return NULL;
    return &t->grid[(size_t)y * t->cols + x];
}

static void clear_cells(Term *t, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            Cell *c = cell_at(t, x, y);
            if (c) { c->ch = ' '; c->fg = t->fg; c->bg = t->bg; c->bold = 0; }
        }
}

static void hist_push(Term *t, const Cell *row) {
    if (!t->hist) return;
    memcpy(t->hist + (size_t)t->hist_head * t->cols, row, (size_t)t->cols * sizeof(Cell));
    t->hist_head = (t->hist_head + 1) % HIST_ROWS;
    if (t->hist_count < HIST_ROWS) t->hist_count++;
}

static void scroll_up(Term *t) {
    hist_push(t, t->grid);
    memmove(t->grid, t->grid + t->cols, (size_t)(t->rows - 1) * t->cols * sizeof(Cell));
    Cell *last = t->grid + (size_t)(t->rows - 1) * t->cols;
    for (int x = 0; x < t->cols; x++) {
        last[x].ch = ' ';
        last[x].fg = t->fg;
        last[x].bg = t->bg;
        last[x].bold = 0;
    }
}

static void newline(Term *t) {
    t->wrap = 0;
    t->cy++;
    if (t->cy >= t->rows) { t->cy = t->rows - 1; scroll_up(t); }
}

/* The cursor is only ever moved through these two functions, so it cannot
 * leave [0,cols) x [0,rows) no matter what a program prints. The long
 * arithmetic and the clamp together make every move saturating. */
static void cursor_set(Term *t, int col, int row) {
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    t->cx = col;
    t->cy = row;
    t->wrap = 0;
}

static void cursor_move(Term *t, int dx, int dy) {
    long nx = (long)t->cx + (long)dx;
    long ny = (long)t->cy + (long)dy;
    if (nx < 0) nx = 0;
    else if (nx >= t->cols) nx = t->cols - 1;
    if (ny < 0) ny = 0;
    else if (ny >= t->rows) ny = t->rows - 1;
    t->cx = (int)nx;
    t->cy = (int)ny;
    t->wrap = 0;
}

static void put_char(Term *t, u32 ch) {
    /* A glyph written into the last column parks the cursor there and wraps
     * on the next glyph rather than running past the end of the row. */
    if (t->wrap) { t->cx = 0; newline(t); }
    Cell *c = cell_at(t, t->cx, t->cy);
    if (c) {
        c->ch = ch;
        c->fg = t->fg;
        c->bg = t->bg;
        c->bold = t->bold;
    }
    if (t->cx + 1 < t->cols) t->cx++;
    else                     t->wrap = 1;
    t->dirty = 1;
}

/* Blank the screen, home the cursor, drop attributes and any half-received
 * UTF-8 character. Shared by the public term_clear_screen() and RIS (ESC c). */
static void screen_reset(Term *t) {
    t->fg = 7;
    t->bg = 0;
    t->bold = 0;
    t->cx = 0;
    t->cy = 0;
    t->wrap = 0;
    t->u8_cp = 0;
    t->u8_need = 0;
    t->u8_min = 0;
    t->scroll = 0;
    clear_cells(t, 0, 0, t->cols - 1, t->rows - 1);
    t->dirty = 1;
}

/* Public API (mystic.h): a full reset of the visible screen. Defined here so
 * that RIS below can call it - this function is now reached from the wire
 * (ESC c), which is what makes it live code rather than dead code. */
void term_clear_screen(Term *t) {
    if (!t) return;
    screen_reset(t);
}

/* ---------------------------------------------------------- parser */

/* A streaming reader over the collected CSI parameter bytes. Parameters are
 * separated by ';', an empty parameter means 0 (ECMA-48). -1 means the list
 * has ended. Digits accumulate with saturation, so a 60-digit parameter
 * pins at TERM_PARAM_MAX instead of overflowing an int. */
typedef struct {
    const char *s;
    int len;
    int pos;            /* -1 once the list is exhausted */
} ParamCursor;

static int param_next(ParamCursor *p) {
    if (p->pos < 0) return -1;
    int v = 0;
    while (p->pos < p->len && p->s[p->pos] != ';') {
        char c = p->s[p->pos++];
        if (c >= '0' && c <= '9' && v < TERM_PARAM_MAX) {
            v = v * 10 + (c - '0');
            if (v > TERM_PARAM_MAX) v = TERM_PARAM_MAX;
        }
    }
    if (p->pos < p->len) p->pos++;      /* step over the ';' */
    else                 p->pos = -1;   /* that was the last parameter */
    return v;
}

/* The screen has 16 colours, so a 256-colour or truecolour SGR has to be
 * approximated: pick the nearest palette entry by squared RGB distance. */
static u8 nearest16(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    int best = 0;
    long best_d = -1;
    for (int i = 0; i < 16; i++) {
        int pr = (int)GET_R(PALETTE[i]);
        int pg = (int)GET_G(PALETTE[i]);
        int pb = (int)GET_B(PALETTE[i]);
        long d = (long)(r - pr) * (r - pr) +
                 (long)(g - pg) * (g - pg) +
                 (long)(b - pb) * (b - pb);
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return (u8)best;
}

static u8 colour256(int idx) {
    if (idx < 0) return 7;
    if (idx < 16) return (u8)idx;
    if (idx < 232) {                    /* 6x6x6 colour cube */
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        int n = idx - 16;
        return nearest16(lv[(n / 36) % 6], lv[(n / 6) % 6], lv[n % 6]);
    }
    if (idx < 256) {                    /* 24-step grey ramp */
        int v = 8 + (idx - 232) * 10;
        return nearest16(v, v, v);
    }
    return 7;
}

static void sgr(Term *t) {
    /* Parameters are consumed one at a time so that the arguments of an
     * extended colour (38/48/58) are read as arguments, not as codes: a 0
     * component can no longer be mistaken for a full reset, and colour
     * codes beyond the 16th parameter are still honoured. */
    ParamCursor p = { t->params, t->plen, 0 };
    int v;
    while ((v = param_next(&p)) >= 0) {
        if (v == 0) { t->fg = 7; t->bg = 0; t->bold = 0; }
        else if (v == 1) t->bold = 1;
        else if (v == 22) t->bold = 0;
        else if (v >= 30 && v <= 37) t->fg = (u8)(v - 30);
        else if (v == 39) t->fg = 7;
        else if (v >= 40 && v <= 47) t->bg = (u8)(v - 40);
        else if (v == 49) t->bg = 0;
        else if (v >= 90 && v <= 97) t->fg = (u8)(v - 90 + 8);
        else if (v >= 100 && v <= 107) t->bg = (u8)(v - 100 + 8);
        else if (v == 38 || v == 48 || v == 58) {
            int is_fg = (v == 38);
            int mode = param_next(&p);
            if (mode == 5) {
                int idx = param_next(&p);
                if (idx < 0) break;
                u8 c = colour256(idx);
                if (is_fg) t->fg = c;
                else if (v == 48) t->bg = c;
            } else if (mode == 2) {
                int r = param_next(&p);
                int g = param_next(&p);
                int b = param_next(&p);
                if (r < 0 || g < 0 || b < 0) break;
                u8 c = nearest16(r, g, b);
                if (is_fg) t->fg = c;
                else if (v == 48) t->bg = c;
            } else {
                break;      /* unknown form: the rest of the SGR is unusable */
            }
        }
    }
}

static void csi_dispatch(Term *t, char final, int priv) {
    t->dirty = 1;
    if (priv) return;       /* ESC[?25h and friends: no cursor, no colour */

    ParamCursor p = { t->params, t->plen, 0 };
    int p1 = param_next(&p);
    int p2 = param_next(&p);
    if (p1 < 0) p1 = 0;
    if (p2 < 0) p2 = 0;

    switch (final) {
        case 'H': case 'f': {
            int row = (p1 > 0 ? p1 : 1) - 1;
            int col = (p2 > 0 ? p2 : 1) - 1;
            cursor_set(t, col, row);
            break;
        }
        case 'A': cursor_move(t, 0, -(p1 > 0 ? p1 : 1)); break;
        case 'B': cursor_move(t, 0,  (p1 > 0 ? p1 : 1)); break;
        case 'C': cursor_move(t,  (p1 > 0 ? p1 : 1), 0); break;
        case 'D': cursor_move(t, -(p1 > 0 ? p1 : 1), 0); break;
        case 'G': cursor_set(t, (p1 > 0 ? p1 : 1) - 1, t->cy); break;
        case 'd': cursor_set(t, t->cx, (p1 > 0 ? p1 : 1) - 1); break;
        case 'J': {
            int mode = p1;
            if (mode == 2 || mode == 3) {
                clear_cells(t, 0, 0, t->cols - 1, t->rows - 1);
                if (mode == 3) {        /* 3 also drops the scrollback */
                    t->hist_head = 0;
                    t->hist_count = 0;
                    t->scroll = 0;
                }
            } else if (mode == 0) {
                clear_cells(t, t->cx, t->cy, t->cols - 1, t->cy);
                if (t->cy + 1 < t->rows)
                    clear_cells(t, 0, t->cy + 1, t->cols - 1, t->rows - 1);
            } else if (mode == 1) {
                /* Erase from the top of the screen to the cursor: whole rows
                 * above the cursor, then 0..cx of the cursor row. On row 0
                 * that is only the cells left of the cursor, not the row. */
                if (t->cy > 0) clear_cells(t, 0, 0, t->cols - 1, t->cy - 1);
                clear_cells(t, 0, t->cy, t->cx, t->cy);
            }
            break;
        }
        case 'K': {
            int mode = p1;
            if (mode == 0) clear_cells(t, t->cx, t->cy, t->cols - 1, t->cy);
            else if (mode == 1) clear_cells(t, 0, t->cy, t->cx, t->cy);
            else if (mode == 2) clear_cells(t, 0, t->cy, t->cols - 1, t->cy);
            break;
        }
        case 'm': sgr(t); break;
        case 'h': case 'l': break;      /* modes: ignore */
        default: break;
    }
}

/* Decode one byte of UTF-8 into cells. An overlong form, a surrogate, a
 * codepoint above U+10FFFF or a broken/incomplete sequence becomes U+FFFD;
 * the decoder always realigns on the offending byte. */
static void utf8_byte(Term *t, unsigned char b) {
again:
    if (t->u8_need > 0) {
        if ((b & 0xC0) == 0x80) {
            t->u8_cp = (t->u8_cp << 6) | (u32)(b & 0x3F);
            if (--t->u8_need == 0) {
                u32 cp = t->u8_cp;
                int bad = (cp < t->u8_min) || (cp > 0x10FFFFu) ||
                          (cp >= 0xD800u && cp <= 0xDFFFu);
                t->u8_cp = 0;
                t->u8_min = 0;
                put_char(t, bad ? 0xFFFDu : cp);
            }
            return;
        }
        /* Not a continuation byte: the sequence is broken. Emit one
         * replacement char and reinterpret this byte from the start. */
        t->u8_cp = 0;
        t->u8_need = 0;
        t->u8_min = 0;
        put_char(t, 0xFFFDu);
        goto again;
    }

    if (b < 0x80) { put_char(t, b); return; }
    if (b >= 0xC2 && b <= 0xDF)      { t->u8_need = 1; t->u8_cp = b & 0x1Fu; t->u8_min = 0x80; }
    else if (b >= 0xE0 && b <= 0xEF) { t->u8_need = 2; t->u8_cp = b & 0x0Fu; t->u8_min = 0x800; }
    else if (b >= 0xF0 && b <= 0xF4) { t->u8_need = 3; t->u8_cp = b & 0x07u; t->u8_min = 0x10000; }
    else put_char(t, 0xFFFDu);   /* 0x80..0xC1 and 0xF5..0xFF are never lead bytes */
}

static void csi_param_byte(Term *t, char c) {
    if (t->plen < (int)sizeof(t->params) - 1) t->params[t->plen++] = c;
    else t->csi_over = 1;       /* truncating mid-number would run a
                                 * different sequence: drop it instead */
}

static void feed_byte(Term *t, unsigned char b) {
    switch (t->state) {
        case 0:
            if (b >= 0x80) { utf8_byte(t, b); break; }
            if (t->u8_need) {       /* an ASCII byte cut a multi-byte char short */
                t->u8_cp = 0;
                t->u8_need = 0;
                t->u8_min = 0;
                put_char(t, 0xFFFDu);
            }
            if (b == 0x1B) { t->state = 1; break; }
            if (b == 0x0D) { t->cx = 0; t->wrap = 0; break; }
            if (b == 0x0A) { newline(t); break; }
            if (b == 0x08) { t->wrap = 0; if (t->cx > 0) t->cx--; break; }
            if (b == 0x09) {
                t->wrap = 0;
                t->cx = (t->cx + 8) & ~7;
                if (t->cx >= t->cols) t->cx = t->cols - 1;
                break;
            }
            if (b == 0x07) break;
            if (b < 32) break;
            put_char(t, b);
            break;

        case 1:                     /* after ESC */
            if (b == '[') {
                t->state = 2;
                t->plen = 0;
                t->params[0] = 0;
                t->csi_priv = 0;
                t->csi_over = 0;
            } else if (b == ']') {
                t->state = 3;
                t->osc_len = 0;
            } else if (b == 'c') {  /* RIS: a full reset, through the public
                                     * term_clear_screen() entry point */
                term_clear_screen(t);
                t->state = 0;
            } else if (b == '(' || b == ')') {
                t->state = 5;       /* charset select: swallow one byte */
            } else {
                t->state = 0;
            }
            break;

        case 2:                     /* inside CSI */
            if (b >= '0' && b <= '9') { csi_param_byte(t, (char)b); }
            else if (b == ';') { csi_param_byte(t, ';'); }
            else if (b == '?' || b == '>' || b == '<' || b == '=') {
                if (t->plen == 0) t->csi_priv = 1;
            } else if (b >= 0x20 && b <= 0x2F) {
                /* intermediate byte (e.g. the '$' of DECRQM): ignore */
            } else if (b >= 0x40 && b <= 0x7E) {
                t->params[t->plen] = 0;
                if (!t->csi_over) csi_dispatch(t, (char)b, t->csi_priv);
                t->state = 0;
            } else if (b == 0x1B) {
                t->state = 1;       /* abandoned CSI */
            } else if (b < 0x20) {
                t->state = 0;       /* control byte aborts the sequence */
            }
            break;

        case 3:                     /* OSC ... ends at BEL, ST or a control */
            if (b == 0x07) { t->state = 0; }
            else if (b == 0x1B) { t->state = 4; }
            else if (b < 0x20) { t->state = 0; }   /* CAN/SUB/newline: abort */
            else if (++t->osc_len > TERM_OSC_MAX) { t->state = 0; }
            break;

        case 4:                     /* ESC seen inside an OSC: want '\\' (ST) */
            if (b == '\\') { t->state = 0; }
            else if (b == 0x1B) { /* still waiting for the ST byte */ }
            else if (b == 0x07 || b < 0x20) { t->state = 0; }
            else { t->state = 3; }  /* not ST: back into the OSC string */
            break;

        case 5:                     /* one byte after ESC ( or ESC ) */
            t->state = 0;
            break;
    }
}

/* ---------------------------------------------------------- api */
Term *term_create(int cols, int rows, const char *shell) {
    if (cols < 10) cols = 10;
    if (rows < 4) rows = 4;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    Term *t = calloc(1, sizeof(Term));
    if (!t) return NULL;
    t->fd = -1;
    t->pidfd = -1;
    t->cols = cols;
    t->rows = rows;
    t->fg = 7;
    t->bg = 0;
    t->alive = 1;
    t->dirty = 1;
    t->grid = calloc((size_t)cols * rows, sizeof(Cell));
    t->hist = calloc((size_t)cols * HIST_ROWS, sizeof(Cell));
    if (!t->grid || !t->hist) { free(t->grid); free(t->hist); free(t); return NULL; }
    clear_cells(t, 0, 0, cols - 1, rows - 1);

    struct winsize ws = { 0 };
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid == 0) {
        /* child: a real shell on a real pty */
        setenv("TERM", "xterm-256color", 1);
        setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1);
        setenv("HOME", "/root", 1);
        setenv("PS1", "\\w # ", 1);
        execl(shell, shell, "-i", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { free(t->grid); free(t->hist); free(t); return NULL; }

    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    t->fd = master;
    t->pid = pid;
    /* A pidfd names this process instance rather than a number, so hanging the
     * shell up can never reach a process that later reused the pid. Optional:
     * kernels before 5.3 have no pidfd_open, and term_destroy then falls back
     * to the waitpid probe + kill(2). */
    t->pidfd = (int)syscall(SYS_pidfd_open, (long)pid, 0L);
    return t;
}

int term_fd(Term *t) { return t ? t->fd : -1; }
int term_alive(Term *t) { return t && t->alive; }
int term_cols(Term *t) { return t ? t->cols : 0; }
int term_rows(Term *t) { return t ? t->rows : 0; }

/* Returns the number of bytes consumed. Reading is capped per call so a
 * process that floods the pty (`yes`, `cat /dev/urandom`) cannot starve the
 * compositor: at 64 KiB per frame the desktop still paints, and the rest of
 * the output is read on the next frame. */
int term_pump(Term *t) {
    if (!t || !t->alive) return 0;
    char buf[2048];
    long budget = 65536;
    int total = 0;
    while (budget > 0) {
        ssize_t n = read(t->fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) feed_byte(t, (unsigned char)buf[i]);
            total += (int)n;
            budget -= n;
            continue;
        }
        if (n == 0) { t->alive = 0; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        t->alive = 0;
        break;
    }
    if (!t->alive) {
        /* Reap once. A pid that has been reaped here (or by main()'s
         * waitpid(-1) loop) must be forgotten, so term_destroy never signals
         * a pid that has since been recycled: ECHILD means it is already
         * somebody else's problem. */
        int status;
        pid_t r;
        do { r = waitpid(t->pid, &status, WNOHANG); } while (r < 0 && errno == EINTR);
        if (r == t->pid || r < 0) t->pid = -1;
    }
    t->dirty = t->dirty || total > 0;
    return total;
}

void term_key(Term *t, const char *bytes, int n) {
    if (!t || !t->alive || n <= 0) return;
    ssize_t r = write(t->fd, bytes, (size_t)n);
    (void)r;
}

void term_scroll(Term *t, int lines) {
    if (!t) return;
    t->scroll += lines;
    if (t->scroll > t->hist_count) t->scroll = t->hist_count;
    if (t->scroll < 0) t->scroll = 0;
    t->dirty = 1;
}

int term_scroll_offset(Term *t) { return t ? t->scroll : 0; }

/* Close the pty, hang up the shell, reap it, and free the grid, the
 * scrollback and the struct. The only correct way to throw a terminal away:
 * close(2) alone leaks 426 KiB (88x20 grid + 88x600 scrollback) and leaves the
 * shell running (or its zombie behind when a background job holds the slave).
 *
 * Before signalling anything we ask the kernel whether the pid is still ours.
 * main() runs waitpid(-1, ..., WNOHANG) every frame (main.c:1148), and
 * term_pump reaps too, so by the time a window is closed an exited shell has
 * usually been reaped already and t->pid may name a recycled process - a blind
 * kill() would then HUP an unrelated one. close(master) has already delivered
 * SIGHUP to the terminal's foreground group, so nothing is lost by skipping
 * the kill when the probe says the pid is not ours any more.
 * Reaping is bounded: a shell that ignores SIGHUP is SIGKILLed after ~100 ms
 * and waited on for at most another ~100 ms, so closing a window can never
 * wedge the desktop. */
/* Hang the shell up / kill it. When a pidfd is open the signal is delivered
 * through the handle, which is bound to *this* process instance: even if the
 * pid has been recycled (main() reaps with waitpid(-1) every frame, so a dead
 * shell's number is free again immediately) the signal goes to nobody rather
 * than to the new owner of the number. Without a pidfd we fall back to kill(2)
 * on a number we have just probed. */
static void term_signal(Term *t, int sig) {
    if (t->pidfd >= 0)
        syscall(SYS_pidfd_send_signal, (long)t->pidfd, (long)sig, (void *)0, 0L);
    else
        kill(t->pid, sig);
}

void term_destroy(Term *t) {
    if (!t) return;

    if (t->fd >= 0) { close(t->fd); t->fd = -1; }

    if (t->pid > 0) {
        /* Is the pid still ours? ECHILD means somebody already reaped it (and
         * the number may now name any child of ours), the pid itself means it
         * was a zombie and is reaped now, 0 means it is still running. Only a
         * 0 leaves the kill(2) fallback enabled - the pidfd path is safe either
         * way because it never looks the process up by number. */
        int status;
        pid_t probe;
        do { probe = waitpid(t->pid, &status, WNOHANG); } while (probe < 0 && errno == EINTR);
        if (probe < 0 || probe == t->pid) t->pid = -1;
    }

    if (t->pid > 0) {
        int status;
        int reaped = 0;
        term_signal(t, SIGHUP);
        for (int i = 0; i < 20 && !reaped; i++) {
            pid_t r = waitpid(t->pid, &status, WNOHANG);
            if (r == t->pid || r < 0) { reaped = 1; break; }  /* r < 0: ECHILD */
            struct timespec ts = { 0, 5 * 1000 * 1000 };      /* 5 ms */
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            term_signal(t, SIGKILL);
            for (int i = 0; i < 20; i++) {
                pid_t r = waitpid(t->pid, &status, WNOHANG);
                if (r == t->pid || r < 0) break;
                struct timespec ts = { 0, 5 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
        }
        t->pid = -1;
    }

    if (t->pidfd >= 0) { close(t->pidfd); t->pidfd = -1; }

    t->alive = 0;
    free(t->grid);
    free(t->hist);
    t->grid = NULL;
    t->hist = NULL;
    free(t);
}

/* Draw the grid into a rectangle. Caller owns the frame. */
void term_draw(Term *t, Surface *s, int x, int y, int w, int h,
               Font *fr, float cw, float ch) {
    if (!t) return;
    int cols = (int)(w / cw);
    int rows = (int)(h / ch);
    if (cols > t->cols) cols = t->cols;
    if (rows > t->rows) rows = t->rows;

    surf_fill(s, x, y, w, h, PALETTE[0]);

    int first_line = t->hist_count - t->scroll;   /* line index shown at the top */

    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int line = first_line + ry;
            Cell *c = NULL;
            if (line < 0) continue;
            if (line < t->hist_count) {
                int idx = (t->hist_head - t->hist_count + line + HIST_ROWS * 2) % HIST_ROWS;
                c = &t->hist[(size_t)idx * t->cols + rx];
            } else {
                int gy = line - t->hist_count;
                if (gy < t->rows) c = cell_at(t, rx, gy);
            }
            if (!c) continue;
            float px = x + rx * cw;
            float py = y + ry * ch;
            u32 bg = cell_bg(c);
            if (bg != PALETTE[0]) surf_fill(s, (int)px, (int)py, (int)cw + 1, (int)ch + 1, bg);
            if (c->ch && c->ch != ' ') {
                /* Re-encode the codepoint we decoded in feed_byte. 1, 2, 3 and
                 * 4 byte forms; a cell can never hold a surrogate or
                 * something above U+10FFFF, but render U+FFFD if it does. */
                char utf[8];
                int n = 0;
                u32 ch2 = c->ch;
                if (ch2 > 0x10FFFFu || (ch2 >= 0xD800u && ch2 <= 0xDFFFu))
                    ch2 = 0xFFFDu;
                if (ch2 < 0x80) {
                    utf[n++] = (char)ch2;
                } else if (ch2 < 0x800) {
                    utf[n++] = (char)(0xC0 | (ch2 >> 6));
                    utf[n++] = (char)(0x80 | (ch2 & 0x3F));
                } else if (ch2 < 0x10000) {
                    utf[n++] = (char)(0xE0 | (ch2 >> 12));
                    utf[n++] = (char)(0x80 | ((ch2 >> 6) & 0x3F));
                    utf[n++] = (char)(0x80 | (ch2 & 0x3F));
                } else {
                    utf[n++] = (char)(0xF0 | (ch2 >> 18));
                    utf[n++] = (char)(0x80 | ((ch2 >> 12) & 0x3F));
                    utf[n++] = (char)(0x80 | ((ch2 >> 6) & 0x3F));
                    utf[n++] = (char)(0x80 | (ch2 & 0x3F));
                }
                utf[n] = 0;
                font_draw(s, fr, px + 1, py + 1, utf, cell_fg(c));
            }
        }
    }

    if (t->scroll > 0) {
        char ind[48];
        snprintf(ind, sizeof(ind), "  scrolled back %d lines  ", t->scroll);
        float iw = font_width(fr, ind);
        gfx_rrect_aa(s, (float)x + (w - iw) / 2 - 8, (float)(y + h) - 20, iw + 16, 16, 8,
                     0xCC000000u, 0, 0);
        font_draw(s, fr, (float)x + (w - iw) / 2, (float)(y + h) - 17, ind, 0xFFCCCCCCu);
    }

    /* cursor */
    if (t->scroll == 0 && t->alive && t->cx < cols && t->cy < rows) {
        float px = x + t->cx * cw;
        float py = y + t->cy * ch;
        surf_blend(s, (int)px, (int)py + (int)ch - 2, (int)cw, 2, 0xFFBBBBBBu);
    }
}
