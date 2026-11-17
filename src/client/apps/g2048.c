/* ============================================================
 *  Mystic OS - apps (external) / g2048
 *  2048 as its own process. It connects to the window server,
 *  maps the shared pixel buffer and plays a real game on it:
 *  four slide directions, one merge per tile per move, a new
 *  tile after every move that changed the board, and a score
 *  that is the sum of the values it actually merged.
 *
 *  Two random tiles start the board; the randomness comes from
 *  /dev/urandom. If that device cannot be read the footer says
 *  so and the tiles come from the real monotonic clock instead.
 *
 *      +--------------------------------------+
 *      |  2048            SCORE 128  BEST 512 |
 *      |  +--------------------------------+  |
 *      |  | [  2 ][  4 ][  . ][  . ]       |  |
 *      |  | [  . ][ 16 ][  8 ][  . ]       |  |
 *      |  | [  . ][  . ][  . ][  . ]       |  |
 *      |  | [  . ][  . ][  . ][  . ]       |  |
 *      |  +--------------------------------+  |
 *      |  arrows move   r restart   q quit    |
 *      |  moves 7   largest 16               |
 *      +--------------------------------------+
 *
 *  Only a board that actually changed is redrawn, and only a move
 *  that actually changed the board spawns a tile.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIN_W 380
#define WIN_H 420
#define MARGIN 16          /* window padding, shrunk on a small window */

#define N  4
#define NN (N * N)

/* how long a freshly spawned tile spends growing/fading in */
#define ANIM_NS 130000000ull

/* evdev key codes: the numbers the window server forwards verbatim */
#define KEY_Q     16
#define KEY_R     19
#define KEY_UP    103
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_DOWN  108

#define COL_BG_TOP    RGB(0x0E, 0x13, 0x1C)
#define COL_BG_BOT    RGB(0x16, 0x1E, 0x2B)
#define COL_TITLE     RGB(0xE8, 0xEC, 0xF6)
#define COL_TEXT      RGB(0xE2, 0xE8, 0xF2)
#define COL_DIM       RGB(0x8A, 0x93, 0xA6)
#define COL_FAINT     RGB(0x5E, 0x6B, 0x82)
#define COL_ERROR     RGB(0xFF, 0x8A, 0x7A)
#define COL_BOARD     RGB(0x12, 0x18, 0x22)
#define COL_BOARD_EDGE RGB(0x25, 0x2F, 0x3E)
#define COL_SLOT      RGB(0x1B, 0x23, 0x30)
#define COL_BOX       RGB(0x17, 0x1F, 0x2B)
#define COL_BOX_EDGE  RGB(0x28, 0x33, 0x44)
#define COL_PANEL     RGB(0x1A, 0x22, 0x2E)
#define COL_HOT       RGB(0xC2, 0x3C, 0x22)
#define COL_HOT_TEXT  RGB(0xFF, 0xC9, 0xB8)
#define COL_TILE_TEXT RGB(0xF6, 0xF8, 0xFB)

/* ------------------------------------------------------------
 *  The board and the numbers on it.
 * ------------------------------------------------------------ */
static int   board[NN];        /* 0 = empty, otherwise 2,4,8,...      */
static u64   spawned_at[NN];   /* CLOCK_MONOTONIC ns of the one tile that
                                * last spawned, 0 for every other cell    */
static long  score;            /* sum of every merged value this game   */
static long  best;             /* best score of this process' session   */
static int   moves;            /* moves that changed the board          */
static int   over;             /* no direction can move: game over      */

/* ---- pure rules: no sockets, no clock, no randomness ---------------- */
/* Everything between this banner and the matching "end of pure rules"
 * line is plain integer code, so the same source can be compiled into a
 * host test harness and the rules proved without a screen. */

#define DIR_LEFT  0
#define DIR_RIGHT 1
#define DIR_UP    2
#define DIR_DOWN  3

/* Slide one line of N cells towards index 0 and merge equal neighbours
 * once. in[] is the line as it is, out[] the line after the move.
 * Each merge adds its value to *add, which is exactly how 2048 scores.
 * Returns 1 only when the line changed, which is the only reason the
 * caller is allowed to spawn a new tile. */
static int line_slide(const int in[N], int out[N], long *add) {
    int pack[N];
    int n = 0;
    for (int i = 0; i < N; i++)
        if (in[i]) pack[n++] = in[i];          /* drop the holes first */

    int res[N];
    int k = 0;
    for (int i = 0; i < n; ) {
        if (i + 1 < n && pack[i] == pack[i + 1]) {
            res[k++] = pack[i] * 2;
            if (add) *add += (long)pack[i] * 2;
            i += 2;                            /* merged tiles cannot merge again */
        } else {
            res[k++] = pack[i++];
        }
    }
    while (k < N) res[k++] = 0;

    int changed = 0;
    for (int i = 0; i < N; i++) {
        out[i] = res[i];
        if (out[i] != in[i]) changed = 1;
    }
    return changed;
}

/* Which (row,col) is step i of line l when sliding towards dir?
 * Step 0 is the cell the tiles are pushed against, so for DIR_LEFT
 * line l is row l read left to right, and for DIR_UP line l is
 * column l read top to bottom. */
static void slide_cell(int dir, int l, int i, int *r, int *c) {
    switch (dir) {
    case DIR_LEFT:  *r = l;         *c = i;         break;
    case DIR_RIGHT: *r = l;         *c = N - 1 - i; break;
    case DIR_UP:    *r = i;         *c = l;         break;
    default:        *r = N - 1 - i; *c = l;         break;   /* DIR_DOWN */
    }
}

/* Slide and merge the whole board one way. The board is written in
 * place; *add collects the score of every merge. Returns 1 when at
 * least one line changed. A board that cannot move is left exactly
 * as it was, which is why a no-op move needs no copy. */
static int board_move(int *b, int dir, long *add) {
    int changed = 0;
    long gained = 0;
    for (int l = 0; l < N; l++) {
        int in[N], out[N];
        for (int i = 0; i < N; i++) {
            int r, c;
            slide_cell(dir, l, i, &r, &c);
            in[i] = b[r * N + c];
        }
        if (line_slide(in, out, &gained)) changed = 1;
        for (int i = 0; i < N; i++) {
            int r, c;
            slide_cell(dir, l, i, &r, &c);
            b[r * N + c] = out[i];
        }
    }
    if (add) *add = gained;
    return changed;
}

/* Game over is a fact about the board, not a guess: there is a move
 * left if and only if one of the four directions changes something.
 * Tried on a copy so the real board is not touched. */
static int can_move(const int *b) {
    for (int d = 0; d < 4; d++) {
        int t[N * N];
        long add = 0;
        for (int i = 0; i < N * N; i++) t[i] = b[i];
        if (board_move(t, d, &add)) return 1;
    }
    return 0;
}
/* ---- end of pure rules ---------------------------------------------- */

/* ------------------------------------------------------------ time */

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* ------------------------------------------------------------ random */

static int   rng_fd = -1;
static unsigned char rng_pool[32];
static int   rng_have, rng_pos;
static u64   fallback_state;
static char  rand_err[96];      /* empty while /dev/urandom works */

static void rng_init(void) {
    rng_fd = open("/dev/urandom", O_RDONLY);
    if (rng_fd < 0)
        snprintf(rand_err, sizeof(rand_err), "/dev/urandom: %s", strerror(errno));
    fallback_state = now_ns() ^ ((u64)getpid() << 17);
}

/* Refill the byte pool from the real device. Returns 0 (and leaves a
 * reason in rand_err) when the device is gone. */
static int rng_fill(void) {
    if (rng_fd < 0) return 0;
    rng_pos = 0;
    rng_have = 0;
    while (rng_have < (int)sizeof(rng_pool)) {
        ssize_t r = read(rng_fd, rng_pool + rng_have,
                         sizeof(rng_pool) - (size_t)rng_have);
        if (r > 0) { rng_have += (int)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        snprintf(rand_err, sizeof(rand_err), "/dev/urandom: %s",
                 r == 0 ? "short read" : strerror(errno));
        close(rng_fd);
        rng_fd = -1;
        rng_have = 0;
        return 0;
    }
    return 1;
}

static unsigned rng_next(void) {
    if (rng_pos >= rng_have && !rng_fill()) {
        /* The device is unusable: fall back to the monotonic clock,
         * which is still real time, just a weaker source. The footer
         * keeps saying why. */
        fallback_state = fallback_state * 6364136223846793005ull
                       + 1442695040888963407ull;
        return (unsigned)(fallback_state >> 33);
    }
    return rng_pool[rng_pos++];
}

/* ------------------------------------------------------------ game */

/* Put a 2 (nine times in ten) or a 4 on a random empty cell. */
static int spawn_tile(void) {
    int empty[NN];
    int n = 0;
    for (int i = 0; i < NN; i++)
        if (!board[i]) empty[n++] = i;
    if (!n) return 0;

    int cell = empty[rng_next() % (unsigned)n];
    board[cell] = (rng_next() % 10u == 0u) ? 4 : 2;
    spawned_at[cell] = now_ns();
    return 1;
}

static void new_game(void) {
    for (int i = 0; i < NN; i++) {
        board[i] = 0;
        spawned_at[i] = 0;
    }
    score = 0;
    moves = 0;
    over = 0;
    spawn_tile();
    spawn_tile();
}

/* Apply a move, but only when it does something. Returns 1 when the
 * board changed: that is when a tile spawns and a redraw is due. */
static int try_move(int dir) {
    long add = 0;
    if (!board_move(board, dir, &add)) return 0;   /* a no-op is not a move */
    score += add;
    if (score > best) best = score;
    moves++;
    /* Every tile moved, so any spawn animation from the previous move is
     * over: clearing the stamps stops a merged tile from popping again. */
    for (int i = 0; i < NN; i++) spawned_at[i] = 0;
    spawn_tile();
    over = !can_move(board);                       /* a spawn can end the game */
    return 1;
}

/* How much of the spawn animation is left: 1 at birth, 0 once done. */
static float pop_at(int i, u64 now) {
    if (!spawned_at[i]) return 0.0f;
    u64 age = now - spawned_at[i];
    if (age >= ANIM_NS) return 0.0f;
    return 1.0f - (float)age / (float)ANIM_NS;
}

static int anim_live(u64 now) {
    for (int i = 0; i < NN; i++)
        if (spawned_at[i] && now - spawned_at[i] < ANIM_NS) return 1;
    return 0;
}

/* ------------------------------------------------------------ colour */

/* Tile colour by value: 2 is a cool slate and the palette gets warmer
 * as the numbers grow; the last entry covers 4096 and everything above.
 * Indexed by log2(value)-1. */
static u32 tile_color(int v) {
    static const u32 cols[12] = {
        RGB(0x2B, 0x36, 0x4C),   /* 2     */
        RGB(0x2F, 0x42, 0x5E),   /* 4     */
        RGB(0x2C, 0x55, 0x64),   /* 8     */
        RGB(0x2D, 0x66, 0x5A),   /* 16    */
        RGB(0x4C, 0x70, 0x3B),   /* 32    */
        RGB(0x76, 0x6F, 0x2C),   /* 64    */
        RGB(0x93, 0x5F, 0x25),   /* 128   */
        RGB(0xAA, 0x4B, 0x21),   /* 256   */
        RGB(0xBD, 0x39, 0x1F),   /* 512   */
        RGB(0xCE, 0x28, 0x24),   /* 1024  */
        RGB(0xDE, 0x1B, 0x33),   /* 2048  */
        RGB(0xEF, 0x2F, 0x5B)    /* 4096+ */
    };
    int i = 0;
    while (v > 2 && i < 11) { v >>= 1; i++; }
    return cols[i];
}

/* ------------------------------------------------------------ layout */

typedef struct {
    int bx, by, bw, bh;   /* the board panel, in window pixels */
    int cell, gap;
    int hdr_y, foot_y, hint_y;
} Layout;

/* Every band is measured from the fonts that are really loaded, so a
 * non-1.0 ui scale (which font_get() already applied) cannot push the
 * header into the board or the board into the footer. */
typedef struct {
    Font *title;   /* the big text: header and GAME OVER */
    Font *label;   /* SCORE / BEST captions           */
    Font *value;   /* the score numbers               */
    Font *hint;    /* footer lines                    */
    int   head_h;  /* measured height of a score box  */
    int   line_h;  /* measured height of a footer line */
} Fonts;

static void fonts_measure(Fonts *F) {
    int sh = (int)(font_height(F->label) + font_height(F->value) + 8.0f);
    int lh = (int)(font_height(F->hint) + 0.5f);
    F->head_h = sh < 30 ? 30 : sh;
    F->line_h = lh < 12 ? 12 : lh;
}

static void layout_for(int vw, int vh, const Fonts *F, Layout *L) {
    int m = MARGIN;
    if (vw < 6 * m) m = vw / 6;
    if (vh < 6 * m) m = vh / 6;
    if (m < 4) m = 4;

    int head = F->head_h + 10;              /* score boxes plus air */
    int foot = 2 * F->line_h + 4;           /* two footer lines      */

    L->hdr_y  = m;
    L->foot_y = vh - m - foot;
    L->hint_y = L->foot_y + F->line_h + 2;

    int avail_w = vw - 2 * m;
    int avail_h = L->foot_y - 8 - (m + head);
    if (avail_w < 8) avail_w = 8;
    if (avail_h < 8) avail_h = 8;

    int side = avail_w < avail_h ? avail_w : avail_h;
    if (side > 360) side = 360;             /* no giant tiles on a huge window */

    int gap = side / 24;
    if (gap < 3) gap = 3;
    int cell = (side - (N + 1) * gap) / N;
    if (cell < 4) cell = 4;

    L->gap  = gap;
    L->cell = cell;
    L->bw   = N * cell + (N + 1) * gap;
    L->bh   = L->bw;
    L->bx   = (vw - L->bw) / 2;
    if (L->bx < 0) L->bx = 0;
    L->by   = m + head + (avail_h - L->bh) / 2;
    if (L->by < m + head) L->by = m + head;
}

/* ------------------------------------------------------------ draw */

/* The largest of a few nominal sizes whose *measured* text fits, so a
 * 4-digit value is never clipped and a longer one shrinks instead of
 * vanishing. NULL when even the smallest still spills. */
static Font *fit_font(int face, const char *txt, float max_w, float max_h) {
    static const int sizes[8] = { 30, 26, 22, 18, 15, 12, 10, 8 };
    for (int i = 0; i < 8; i++) {
        Font *f = font_get(face, (float)sizes[i]);
        if (!f) continue;
        if (font_width(f, txt) <= max_w && font_height(f) <= max_h) return f;
    }
    return NULL;
}

static Font *fit_value_font(const char *txt, float max_w, float max_h) {
    return fit_font(FONT_SEMIBOLD, txt, max_w, max_h);
}

static void draw_stat(Surface *s, const Fonts *F, int x, int y, int w,
                      const char *label, long v) {
    int h = F->head_h;
    if (x < 0) { w += x; x = 0; }
    if (w < 16 || h < 12) return;

    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, 8,
                 COL_BOX, COL_BOX_EDGE, 1.0f);

    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", v);

    float lh = font_height(F->label);
    float lw = font_width(F->label, label);
    if (lw <= (float)w - 12.0f)
        font_draw(s, F->label, (float)x + ((float)w - lw) * 0.5f,
                  (float)y + 3.0f, label, COL_DIM);

    Font *vf = F->value;
    float nw = font_width(vf, buf);
    if (nw > (float)w - 10.0f) {
        float room = (float)h - lh - 8.0f;
        vf = fit_value_font(buf, (float)w - 10.0f, room > 6.0f ? room : 6.0f);
        if (!vf) return;
        nw = font_width(vf, buf);
    }
    font_draw(s, vf, (float)x + ((float)w - nw) * 0.5f,
              (float)y + lh + 4.0f, buf, COL_TEXT);
}

static void draw_tile(Surface *s, int x, int y, int cell, int v, float pop) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);

    /* A tile that just spawned grows from 94% to 100% while it fades
     * in; every older tile is drawn at full size and full alpha. */
    float inset = (float)cell * 0.06f * pop;
    float tx = (float)x + inset;
    float ty = (float)y + inset;
    float ts = (float)cell - inset * 2.0f;

    u32 fill = tile_color(v);
    if (pop > 0.0f) fill = WITH_A(fill, 150 + (int)(105.0f * (1.0f - pop)));
    gfx_rrect_aa(s, tx, ty, ts, ts, ts * 0.16f, fill, 0, 0);

    Font *f = fit_value_font(buf, ts - 8.0f, ts - 6.0f);
    if (!f) return;
    float tw = font_width(f, buf), th = font_height(f);
    font_draw(s, f, tx + (ts - tw) * 0.5f, ty + (ts - th) * 0.5f,
              buf, COL_TILE_TEXT);
}

static void draw(MClient *c, int vw, int vh, const Layout *L,
                 const Fonts *F, u64 now) {
    if (vw <= 0 || vh <= 0) return;
    Surface s = { c->px, vw, vh, c->stride };

    int m = L->hdr_y;
    gfx_gradient(&s, 0, 0, vw, vh, COL_BG_TOP, COL_BG_BOT);

    /* ---- header: SCORE and BEST boxes on the right, title if it fits */
    int box_gap = 8;
    int box_w = 82;
    if (box_w * 2 + box_gap > vw - 2 * m) box_w = (vw - 2 * m - box_gap) / 2;
    int sx = vw - m - box_w * 2 - box_gap;
    draw_stat(&s, F, sx, m + 2, box_w, "SCORE", score);
    draw_stat(&s, F, sx + box_w + box_gap, m + 2, box_w, "BEST", best);
    float tw = font_width(F->title, "2048");
    if ((float)m + tw <= (float)(sx - 8))
        font_draw(&s, F->title, (float)m, (float)(m + 6), "2048", COL_TITLE);

    /* ---- the board: one rounded panel, sixteen rounded cells */
    gfx_rrect_aa(&s, (float)L->bx, (float)L->by, (float)L->bw, (float)L->bh,
                 12.0f, COL_BOARD, COL_BOARD_EDGE, 1.0f);
    for (int r = 0; r < N; r++) {
        for (int col = 0; col < N; col++) {
            int x = L->bx + L->gap + col * (L->cell + L->gap);
            int y = L->by + L->gap + r * (L->cell + L->gap);
            /* clip: never touch a pixel outside the window we were given */
            if (x < 0 || y < 0 || x + L->cell > vw || y + L->cell > vh) continue;
            gfx_rrect_aa(&s, (float)x, (float)y, (float)L->cell, (float)L->cell,
                         (float)L->cell * 0.16f, COL_SLOT, 0, 0);
            int v = board[r * N + col];
            if (!v) continue;
            draw_tile(&s, x, y, L->cell, v, pop_at(r * N + col, now));
        }
    }

    /* ---- game over: dim the board and say how to start again */
    if (over) {
        surf_blend(&s, L->bx, L->by, L->bw, L->bh, WITH_A(COL_BG_TOP, 200));
        int pw = L->bw - 40, ph = 96;
        if (pw > 260) pw = 260;
        if (pw < 24) pw = 24;      /* a small board still gets a real panel */
        if (pw > L->bw) pw = L->bw;
        if (ph > L->bh) ph = L->bh;
        if (pw < 1) pw = 1;
        if (ph < 1) ph = 1;
        int px = L->bx + (L->bw - pw) / 2;
        int py = L->by + (L->bh - ph) / 2;
        if (px < L->bx) px = L->bx;
        if (py < L->by) py = L->by;
        gfx_rrect_aa(&s, (float)px, (float)py, (float)pw, (float)ph, 12.0f,
                     COL_PANEL, COL_HOT, 1.0f);
        Font *gt = fit_value_font("GAME OVER", (float)pw - 16.0f, 34.0f);
        if (gt && ph >= 52) {                 /* only when the line fits the panel */
            float gw = font_width(gt, "GAME OVER");
            font_draw(&s, gt, (float)px + ((float)pw - gw) * 0.5f,
                      (float)py + 14.0f, "GAME OVER", COL_HOT_TEXT);
        }
        Font *ht = fit_font(FONT_REGULAR, "press r to restart",
                            (float)pw - 16.0f, 20.0f);
        if (ht && ph >= 86) {
            float hw = font_width(ht, "press r to restart");
            font_draw(&s, ht, (float)px + ((float)pw - hw) * 0.5f,
                      (float)py + 62.0f, "press r to restart", COL_TEXT);
        }
    }

    /* ---- footer: keys, then the numbers that are actually true */
    if (rand_err[0]) {
        font_draw(&s, F->hint, (float)m, (float)L->foot_y, rand_err, COL_ERROR);
    } else {
        font_draw(&s, F->hint, (float)m, (float)L->foot_y,
                  "arrows move   r restart   q quit", COL_DIM);
    }

    int largest = 0;
    for (int i = 0; i < NN; i++)
        if (board[i] > largest) largest = board[i];
    char info[64];
    snprintf(info, sizeof(info), "moves %d   largest %d", moves, largest);
    font_draw(&s, F->hint, (float)m, (float)L->hint_y, info, COL_FAINT);
}

/* ------------------------------------------------------------ main */

int main(void) {
    MClient c;
    if (mc_open(&c, "g2048", "2048", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "g2048: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                        : "/run/mystic/wm.sock");
        return 1;
    }

    font_init("/usr/share/fonts");
    ui_scale_load("/etc/mystic/ui.conf");   /* the system's text scale */
    Fonts F;
    F.title = font_get(FONT_SEMIBOLD, 22);
    F.label = font_get(FONT_REGULAR, 9);
    F.value = font_get(FONT_SEMIBOLD, 13);
    F.hint  = font_get(FONT_REGULAR, 10);
    if (!F.title || !F.label || !F.value || !F.hint) {
        fprintf(stderr, "g2048: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }
    fonts_measure(&F);

    /* the shm was mapped for exactly this size: a RESIZE may only give
     * us less than that, never more */
    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;

    rng_init();
    new_game();

    int dirty = 1, quit = 0;
    while (!quit) {
        Layout L;
        layout_for(vw, vh, &F, &L);

        if (dirty) {
            draw(&c, vw, vh, &L, &F, now_ns());
            mc_present(&c);
            dirty = 0;
        }

        int got = 0;
        MEvent ev;
        while (!quit && mc_poll(&c, &ev)) {
            got = 1;
            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;

            case MC_RESIZE:
                vw = ev.w < cap_w ? ev.w : cap_w;
                vh = ev.h < cap_h ? ev.h : cap_h;
                if (vw < 1) vw = 1;
                if (vh < 1) vh = 1;
                dirty = 1;
                break;

            /* Keys are handled here only. The server also forwards the
             * printable form of every key as TEXT, and restarting twice
             * for one 'r' would throw away the game the player just
             * started, so MC_TEXT is deliberately ignored. */
            case MC_KEY: {
                if (!ev.press) break;
                int dir = -1;
                if (ev.key == KEY_Q) { quit = 1; break; }
                if (ev.key == KEY_R) { new_game(); dirty = 1; break; }
                if (ev.key == KEY_LEFT)  dir = DIR_LEFT;
                if (ev.key == KEY_RIGHT) dir = DIR_RIGHT;
                if (ev.key == KEY_UP)    dir = DIR_UP;
                if (ev.key == KEY_DOWN)  dir = DIR_DOWN;
                if (dir >= 0 && try_move(dir)) dirty = 1;
                break;
            }

            default:
                break;
            }
        }
        if (quit) break;

        /* Redraw while a spawn animation is running; otherwise sleep
         * until something happens. Nothing here polls a frame clock. */
        u64 now = now_ns();
        if (anim_live(now)) {
            dirty = 1;
            usleep(16 * 1000);
        } else if (!got) {
            usleep(40 * 1000);
        }
    }

    mc_close(&c);
    return 0;
}
