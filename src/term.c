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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <pty.h>

#define TERM_MAX_COLS 400
#define TERM_MAX_ROWS 200

typedef struct {
    u32 ch;
    u8  fg, bg, bold;
} Cell;

#define HIST_ROWS 600      /* lines of scrollback per terminal */

struct Term {
    int   fd;
    pid_t pid;
    int   cols, rows;

    Cell *grid;          /* rows * cols */
    Cell *hist;          /* ring buffer of lines that scrolled off the top */
    int   hist_head;     /* next slot to write in hist */
    int   hist_count;    /* valid lines in hist */
    int   scroll;        /* lines scrolled back from the live screen */
    int   cx, cy;        /* cursor */
    u8    fg, bg, bold;

    /* parser state */
    int   state;         /* 0 normal, 1 ESC, 2 CSI, 3 OSC */
    char  params[64];
    int   plen;

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
    t->cy++;
    if (t->cy >= t->rows) { t->cy = t->rows - 1; scroll_up(t); }
}

static void put_char(Term *t, u32 ch) {
    if (t->cx >= t->cols) { t->cx = 0; newline(t); }
    Cell *c = cell_at(t, t->cx, t->cy);
    if (c) {
        c->ch = ch;
        c->fg = t->fg;
        c->bg = t->bg;
        c->bold = t->bold;
    }
    t->cx++;
    t->dirty = 1;
}

/* ---------------------------------------------------------- parser */
static void sgr(Term *t) {
    /* params like "0;1;32" */
    int vals[16] = {0};
    int n = 0;
    int cur = 0, have = 0;
    for (int i = 0; i <= t->plen; i++) {
        char c = (i < t->plen) ? t->params[i] : ';';
        if (c == ';') {
            if (n < 16) vals[n++] = have ? cur : 0;
            cur = 0;
            have = 0;
        } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have = 1;
        }
    }
    if (n == 0) { vals[0] = 0; n = 1; }

    for (int i = 0; i < n; i++) {
        int v = vals[i];
        if (v == 0) { t->fg = 7; t->bg = 0; t->bold = 0; }
        else if (v == 1) t->bold = 1;
        else if (v == 22) t->bold = 0;
        else if ((v >= 30 && v <= 37)) t->fg = (u8)(v - 30);
        else if (v == 39) t->fg = 7;
        else if ((v >= 40 && v <= 47)) t->bg = (u8)(v - 40);
        else if (v == 49) t->bg = 0;
        else if ((v >= 90 && v <= 97)) t->fg = (u8)(v - 90 + 8);
        else if ((v >= 100 && v <= 107)) t->bg = (u8)(v - 100 + 8);
        else if (v == 38 || v == 48) {
            /* 256-colour / truecolour - we take the simple 256 path */
            int is_fg = (v == 38);
            i++;
            if (i < n && vals[i] == 5 && i + 1 < n) {
                int idx = vals[i + 1];
                i++;
                if (is_fg) t->fg = (idx < 16) ? (u8)idx : 7;
                else       t->bg = (idx < 16) ? (u8)idx : 0;
            }
        }
    }
}

static void csi_dispatch(Term *t, char final) {
    int p1 = 0, p2 = 0;
    char *s = t->params;
    if (t->plen) p1 = atoi(s);
    char *semi = strchr(s, ';');
    if (semi) p2 = atoi(semi + 1);

    switch (final) {
        case 'H': case 'f': {
            int row = (p1 > 0 ? p1 : 1) - 1;
            int col = (p2 > 0 ? p2 : 1) - 1;
            if (row < 0) row = 0;
            if (row >= t->rows) row = t->rows - 1;
            if (col < 0) col = 0;
            if (col >= t->cols) col = t->cols - 1;
            t->cx = col; t->cy = row;
            break;
        }
        case 'A': t->cy -= (p1 > 0 ? p1 : 1); if (t->cy < 0) t->cy = 0; break;
        case 'B': t->cy += (p1 > 0 ? p1 : 1); if (t->cy >= t->rows) t->cy = t->rows - 1; break;
        case 'C': t->cx += (p1 > 0 ? p1 : 1); if (t->cx >= t->cols) t->cx = t->cols - 1; break;
        case 'D': t->cx -= (p1 > 0 ? p1 : 1); if (t->cx < 0) t->cx = 0; break;
        case 'J': {
            int mode = p1;
            if (mode == 2 || mode == 3) clear_cells(t, 0, 0, t->cols - 1, t->rows - 1);
            else if (mode == 0) {
                clear_cells(t, t->cx, t->cy, t->cols - 1, t->cy);
                if (t->cy + 1 < t->rows) clear_cells(t, 0, t->cy + 1, t->cols - 1, t->rows - 1);
            } else if (mode == 1) {
                clear_cells(t, 0, 0, t->cols - 1, t->cy - 1 > 0 ? t->cy - 1 : 0);
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
    t->dirty = 1;
}

static void feed_byte(Term *t, unsigned char b) {
    switch (t->state) {
        case 0:
            if (b == 0x1B) { t->state = 1; break; }
            if (b == 0x0D) { t->cx = 0; break; }
            if (b == 0x0A) { newline(t); break; }
            if (b == 0x08) { if (t->cx > 0) t->cx--; break; }
            if (b == 0x09) { t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) t->cx = t->cols - 1; break; }
            if (b == 0x07) break;
            if (b < 32) break;
            put_char(t, b);
            break;

        case 1:                     /* after ESC */
            if (b == '[') { t->state = 2; t->plen = 0; t->params[0] = 0; }
            else if (b == ']') { t->state = 3; }
            else if (b == '(' || b == ')') { t->state = 1; }   /* charset select: swallow */
            else t->state = 0;
            break;

        case 2:                     /* inside CSI */
            if ((b >= '0' && b <= '9') || b == ';' || b == '?' || b == ' ') {
                if (t->plen < (int)sizeof(t->params) - 1) {
                    if (b != '?') t->params[t->plen++] = (char)b;
                }
            } else {
                t->params[t->plen] = 0;
                csi_dispatch(t, (char)b);
                t->state = 0;
            }
            break;

        case 3:                     /* OSC ... terminated by BEL or ST */
            if (b == 0x07) t->state = 0;
            else if (b == 0x1B) t->state = 1;
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
    if (pid < 0) { free(t->grid); free(t); return NULL; }

    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    t->fd = master;
    t->pid = pid;
    return t;
}

int term_fd(Term *t) { return t ? t->fd : -1; }
int term_alive(Term *t) { return t && t->alive; }
int term_cols(Term *t) { return t ? t->cols : 0; }
int term_rows(Term *t) { return t ? t->rows : 0; }

void term_pump(Term *t) {
    if (!t || !t->alive) return;
    char buf[2048];
    for (;;) {
        ssize_t n = read(t->fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) feed_byte(t, (unsigned char)buf[i]);
            continue;
        }
        if (n == 0) { t->alive = 0; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        t->alive = 0;
        break;
    }
    if (!t->alive) {
        int status;
        waitpid(t->pid, &status, WNOHANG);
    }
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

void term_clear_screen(Term *t) {
    if (!t) return;
    clear_cells(t, 0, 0, t->cols - 1, t->rows - 1);
    t->cx = t->cy = 0;
    t->dirty = 1;
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
                char utf[8];
                int n = 0;
                u32 ch2 = c->ch;
                if (ch2 < 0x80) utf[n++] = (char)ch2;
                else if (ch2 < 0x800) {
                    utf[n++] = (char)(0xC0 | (ch2 >> 6));
                    utf[n++] = (char)(0x80 | (ch2 & 0x3F));
                } else {
                    utf[n++] = (char)(0xE0 | (ch2 >> 12));
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
