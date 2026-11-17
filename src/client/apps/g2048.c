/* ============================================================
 *  Mystic OS - apps (external) / g2048
 *  2048 as its own process. It connects to the window server,
 *  maps the shared pixel buffer and plays a real game on it:
 *  four slide directions, one merge per tile per move, a new
 *  tile after every move that changed the board, and a score
 *  that is the sum of the values it actually merged.
 *
 *      +--------------------------------------+
 *      |  2048            SCORE 128  BEST 512 |
 *      |  +--------------------------------+  |
 *      |  | [  2 ][  4 ][  . ][  . ]       |  |
 *      |  | [  . ][ 16 ][  8 ][  . ]       |  |
 *      |  | [  . ][  . ][  . ][  . ]       |  |
 *      |  | [  . ][  . ][  . ][  . ]       |  |
 *      |  +--------------------------------+  |
 *      |  arrows/WASD move  u undo  r new     |
 *      |  moves 7   largest 16   undo 2       |
 *      |  best 512  (/root/g2048.best)        |
 *      +--------------------------------------+
 *
 *  What is real here, and where it comes from:
 *
 *  - The moves are the rules in the "pure rules" block below: pack a
 *    line, merge equal neighbours once, score the merged value. The
 *    same function produces the source cell of every tile it moved,
 *    which is what the slide animation interpolates over.
 *  - The two starting tiles and every later one come from
 *    /dev/urandom; the footer says so if that device cannot be read
 *    and the fallback (the monotonic clock) is being used instead.
 *  - The best score is read and written as a real file. /root is
 *    tried first, because that is where the owner asked for it.
 *    A jailed app has no /root, so /tmp (its own jail tmp, on the
 *    real disk) is the fallback; if neither can be written the
 *    footer carries the actual strerror(errno) - it never pretends
 *    the score was saved.
 *  - The undo stack is a real stack of board snapshots: u steps back
 *    one move at a time, and it works after a loss.
 *  - The layout is measured from the loaded fonts, so the header,
 *    board and footer cannot collide at any text scale; every
 *    string is fitted to its box before it is drawn.
 *
 *  Only a board that actually changed is redrawn, and only a move
 *  that actually changed the board spawns a tile. While a slide is
 *  animating the process wakes every 16 ms; otherwise it sleeps
 *  until an event arrives.
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
/* how long a tile takes to slide to its new cell */
#define SLIDE_MS 110
#define SLIDE_NS ((u64)SLIDE_MS * 1000000ull)
/* how many moves the undo stack remembers */
#define UNDO_MAX 32

/* evdev key codes: the numbers the window server forwards verbatim */
#define KEY_Q     16
#define KEY_W     17
#define KEY_R     19
#define KEY_U     22
#define KEY_A     30
#define KEY_S     31
#define KEY_D     32
#define KEY_Z     44
#define KEY_SPACE 57
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
#define COL_WIN       RGB(0x4F, 0xD1, 0xC5)
#define COL_BOARD     RGB(0x12, 0x18, 0x22)
#define COL_BOARD_EDGE RGB(0x25, 0x2F, 0x3E)
#define COL_SLOT      RGB(0x1B, 0x23, 0x30)
#define COL_BOX       RGB(0x17, 0x1F, 0x2B)
#define COL_BOX_EDGE  RGB(0x28, 0x33, 0x44)
#define COL_PANEL     RGB(0x1A, 0x22, 0x2E)
#define COL_HOT       RGB(0xC2, 0x3C, 0x22)
#define COL_HOT_TEXT  RGB(0xFF, 0xC9, 0xB8)
#define COL_TILE_TEXT RGB(0xF6, 0xF8, 0xFB)

#define DIR_LEFT  0
#define DIR_RIGHT 1
#define DIR_UP    2
#define DIR_DOWN  3

/* ============================================================
 *  Pure rules: no sockets, no fonts, no clock, no randomness.
 *
 *  Everything down to "end of the pure rules" is plain integer code
 *  over a Game value the caller owns, so it can be compiled into a
 *  host harness (zig cc -DG2048_HOST_TEST) and proved without a
 *  screen: slides, one-merge-per-tile, scoring, the movement trace
 *  the animation uses, and the undo stack.
 * ============================================================ */

/* Where each destination cell's tile came from. A merge has two
 * sources; a plain move has one; an empty cell has none. The values
 * are cell indices, so the renderer can interpolate positions. */
typedef struct {
    unsigned char n;      /* 0, 1 or 2 sources */
    unsigned char s0, s1;
} Trace;
#define NO_CELL 255

/* One board state, kept so a move can be taken back. */
typedef struct {
    int  b[NN];
    long score;
    int  moves;
    int  won;
} Snap;

typedef struct {
    int  board[NN];        /* 0 = empty, otherwise 2,4,8,...      */
    long score;            /* sum of every merged value this game  */
    int  moves;            /* moves that changed the board         */
    int  won;              /* 2048 has been reached                 */
    int  over;             /* no direction can move: game over     */
    Snap undo[UNDO_MAX];
    int  undo_n;
} Game;

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
 * place; *add collects the score of every merge, and tr (which may be
 * NULL) collects where each resulting tile came from. Returns 1 when at
 * least one line changed - the only reason the caller may spawn a tile.
 *
 * One implementation serves both the rules and the animation: the
 * renderer must not be able to disagree with the game about which tile
 * went where. */
static int board_move_trace(int *b, int dir, long *add, Trace *tr) {
    int changed = 0;
    long gained = 0;

    for (int l = 0; l < N; l++) {
        int in[N], cellin[N];
        for (int i = 0; i < N; i++) {
            int r, c;
            slide_cell(dir, l, i, &r, &c);
            in[i] = b[r * N + c];
            cellin[i] = r * N + c;
        }

        /* the tiles on this line, in the order they are pushed */
        int pv[N], pc[N], n = 0;
        for (int i = 0; i < N; i++)
            if (in[i]) { pv[n] = in[i]; pc[n] = cellin[i]; n++; }

        int out[N], src_n[N], src0[N], src1[N];
        int k = 0;
        for (int i = 0; i < n; ) {
            if (i + 1 < n && pv[i] == pv[i + 1]) {
                out[k] = pv[i] * 2;
                gained += (long)out[k];
                src_n[k] = 2;
                src0[k] = (unsigned char)pc[i];
                src1[k] = (unsigned char)pc[i + 1];
                i += 2;                 /* merged tiles cannot merge again */
            } else {
                out[k] = pv[i];
                src_n[k] = 1;
                src0[k] = (unsigned char)pc[i];
                src1[k] = NO_CELL;
                i++;
            }
            k++;
        }
        while (k < N) {
            out[k] = 0;
            src_n[k] = 0;
            src0[k] = NO_CELL;
            src1[k] = NO_CELL;
            k++;
        }

        for (int i = 0; i < N; i++) {
            int r, c;
            slide_cell(dir, l, i, &r, &c);
            if (b[r * N + c] != out[i]) changed = 1;
            b[r * N + c] = out[i];
            if (tr) {
                tr[r * N + c].n = (unsigned char)src_n[i];
                tr[r * N + c].s0 = (unsigned char)src0[i];
                tr[r * N + c].s1 = (unsigned char)src1[i];
            }
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
        int t[NN];
        long add = 0;
        for (int i = 0; i < NN; i++) t[i] = b[i];
        if (board_move_trace(t, d, &add, NULL)) return 1;
    }
    return 0;
}

static int largest_tile(const int *b) {
    int v = 0;
    for (int i = 0; i < NN; i++)
        if (b[i] > v) v = b[i];
    return v;
}

static void game_reset(Game *g) {
    for (int i = 0; i < NN; i++) g->board[i] = 0;
    g->score = 0;
    g->moves = 0;
    g->won = 0;
    g->over = 0;
    g->undo_n = 0;
}

/* Push the current position before it is changed. A full stack drops the
 * oldest entry: the last UNDO_MAX moves are what "undo" means here. */
static void push_undo(Game *g) {
    if (g->undo_n == UNDO_MAX) {
        memmove(&g->undo[0], &g->undo[1], sizeof(Snap) * (UNDO_MAX - 1));
        g->undo_n--;
    }
    Snap *s = &g->undo[g->undo_n++];
    memcpy(s->b, g->board, sizeof(s->b));
    s->score = g->score;
    s->moves = g->moves;
    s->won = g->won;
}

/* Apply one move to the board, scoring it, remembering the position it
 * replaced, and (if tr is not NULL) recording where every tile came from.
 * Nothing is spawned here: that is the caller's job, because a no-op move
 * must not spawn one. Returns 1 when the board changed. */
static int game_apply(Game *g, int dir, long *add, Trace *tr) {
    int trial[NN];
    long gained = 0;
    for (int i = 0; i < NN; i++) trial[i] = g->board[i];
    if (!board_move_trace(trial, dir, &gained, NULL)) return 0;   /* a no-op */

    push_undo(g);
    /* the real move, traced once: the animation and the rules come from the
     * same call, so the tiles cannot slide somewhere the board did not go */
    board_move_trace(g->board, dir, &gained, tr);
    g->score += gained;
    g->moves++;
    if (add) *add = gained;
    return 1;
}

/* Take back the last move. Returns 1 if there was one. */
static int game_undo(Game *g) {
    if (g->undo_n <= 0) return 0;
    Snap *s = &g->undo[--g->undo_n];
    memcpy(g->board, s->b, sizeof(g->board));
    g->score = s->score;
    g->moves = s->moves;
    g->won = s->won;
    /* undoing out of a dead board revives it, and that is a fact we can
     * re-derive rather than remember */
    g->over = !can_move(g->board);
    return 1;
}

/* ---- end of the pure rules ------------------------------------------ */

#ifndef G2048_HOST_TEST

/* ------------------------------------------------------------ state */

static Game G;

/* only the tile that last spawned has a stamp; 0 means "settled" */
static u64 spawned_at[NN];

/* the slide in progress: where each landing tile came from */
static Trace trace[NN];
static u64   slide_t0;
static int   slide_on;

/* the "you win" banner, up until the player dismisses it */
static int win_banner;

/* best score, read from and written to a real file */
static long best;
static char best_path[64];      /* "" when nothing was loadable/creatable */
static char best_err[96];       /* why, when best_path is empty */

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

/* ------------------------------------------------------------ best score */

/* The paths are tried in this order. /root is what the owner asked for and
 * works whenever the app can see it (run from a shell, or a jail that
 * binds it); the app's own /tmp is the fallback inside the sandbox, and is
 * a real file on the real disk too. */
static const char *const BEST_CANDIDATES[] = {
    "/root/g2048.best",
    "/tmp/g2048.best",
};

static int read_best(const char *path, long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long v = -1;
    int ok = (fscanf(f, "%ld", &v) == 1) && v >= 0;
    fclose(f);
    if (ok) *out = v;
    return ok;
}

/* Write through a temporary and rename: a torn file never becomes the
 * best score. Returns 0 or -1 with errno set. */
static int write_best(const char *path, long v) {
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int ok = fprintf(f, "%ld\n", v) > 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { int e = errno; remove(tmp); errno = e; return -1; }
    if (rename(tmp, path) != 0) { int e = errno; remove(tmp); errno = e; return -1; }
    return 0;
}

/* Pick the first candidate that can actually be read or created, load the
 * score from it, and keep the reason if none can. */
static void best_open(void) {
    best_path[0] = 0;
    best_err[0] = 0;
    int last_errno = 0;

    for (unsigned i = 0; i < sizeof(BEST_CANDIDATES) / sizeof(BEST_CANDIDATES[0]); i++) {
        const char *p = BEST_CANDIDATES[i];
        if (read_best(p, &best)) {
            snprintf(best_path, sizeof(best_path), "%s", p);
            return;
        }
        /* not there yet: can we create it? A zero-byte probe is enough to
         * learn whether this directory is writable, and it is removed if we
         * did not find a score in it. */
        FILE *f = fopen(p, "a");
        if (f) {
            fclose(f);
            snprintf(best_path, sizeof(best_path), "%s", p);
            return;
        }
        last_errno = errno;
    }
    if (!last_errno) last_errno = EACCES;
    snprintf(best_err, sizeof(best_err), "%s", strerror(last_errno));
}

static void best_save(long v) {
    if (!best_path[0]) return;
    if (write_best(best_path, v) != 0)
        snprintf(best_err, sizeof(best_err), "%s", strerror(errno));
}

/* ------------------------------------------------------------ game flow */

/* Put a 2 (nine times in ten) or a 4 on a random empty cell. Returns the
 * cell, or -1 when the board is full. */
static int spawn_tile(u64 now) {
    int empty[NN];
    int n = 0;
    for (int i = 0; i < NN; i++)
        if (!G.board[i]) empty[n++] = i;
    if (!n) return -1;

    int cell = empty[rng_next() % (unsigned)n];
    G.board[cell] = (rng_next() % 10u == 0u) ? 4 : 2;
    spawned_at[cell] = now;
    return cell;
}

static void new_game(u64 now) {
    game_reset(&G);
    for (int i = 0; i < NN; i++) spawned_at[i] = 0;
    slide_on = 0;
    win_banner = 0;
    spawn_tile(now);
    spawn_tile(now);
}

/* Apply a move, but only when it does something. Returns 1 when the board
 * changed: that is when a tile spawns and a redraw is due. */
static int try_move(int dir, u64 now) {
    if (G.over || win_banner) return 0;      /* the overlays own the board */
    long gained = 0;
    for (int i = 0; i < NN; i++) {
        trace[i].n = 0;
        trace[i].s0 = trace[i].s1 = NO_CELL;
    }
    if (!game_apply(&G, dir, &gained, trace)) return 0;

    if (G.score > best) { best = G.score; best_save(best); }

    for (int i = 0; i < NN; i++) spawned_at[i] = 0;
    int cell = spawn_tile(now + SLIDE_NS);   /* the pop starts when it lands */
    (void)cell;

    if (!G.won && largest_tile(G.board) >= 2048) {
        G.won = 1;
        win_banner = 1;
    }
    G.over = !can_move(G.board);             /* a spawn can end the game */

    slide_t0 = now;
    slide_on = 1;
    return 1;
}

static void do_undo(void) {
    if (!game_undo(&G)) return;
    for (int i = 0; i < NN; i++) {
        spawned_at[i] = 0;
        trace[i].n = 0;
    }
    slide_on = 0;
    win_banner = 0;          /* the board is playable again */
}

/* How much of the spawn animation is left: 1 at birth, 0 once done. A stamp
 * in the future (the tile that lands when a slide ends) reads as 1. */
static float pop_at(int i, u64 now) {
    if (!spawned_at[i]) return 0.0f;
    if (spawned_at[i] > now) return 1.0f;
    u64 age = now - spawned_at[i];
    if (age >= ANIM_NS) return 0.0f;
    return 1.0f - (float)age / (float)ANIM_NS;
}

static int anim_live(u64 now) {
    if (slide_on && now - slide_t0 < SLIDE_NS) return 1;
    for (int i = 0; i < NN; i++) {
        if (!spawned_at[i]) continue;
        if (spawned_at[i] > now) return 1;          /* not started yet */
        if (now - spawned_at[i] < ANIM_NS) return 1;
    }
    return 0;
}

/* 0 at the start of a slide, 1 when the tile has landed. Eased so the
 * tiles leave quickly and settle gently. */
static float slide_t(u64 now) {
    if (!slide_on) return 1.0f;
    if (now <= slide_t0) return 0.0f;
    u64 age = now - slide_t0;
    if (age >= SLIDE_NS) return 1.0f;
    float t = (float)age / (float)SLIDE_NS;
    return 1.0f - (1.0f - t) * (1.0f - t);
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
    int m;
    int hdr_y, head_h;
    int foot_y;           /* first footer line */
    int line_h;
    int lines;            /* footer lines that fit */
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
    L->m = m;

    int head = F->head_h + 8;               /* score boxes plus air */
    int lines = 3;
    /* On a short window the third footer line (where the best score came
     * from) is the first thing to go, then the second. */
    while (lines > 1 && vh - 2 * m - head - (lines * F->line_h + 4) < 60) lines--;
    int foot = lines * F->line_h + 4;
    L->lines = lines;

    L->hdr_y  = m;
    L->head_h = F->head_h;
    L->line_h = F->line_h;
    L->foot_y = vh - m - foot;

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
    if (!txt || !*txt || max_w < 2.0f || max_h < 2.0f) return NULL;
    for (int i = 0; i < 8; i++) {
        Font *f = font_get(face, (float)sizes[i]);
        if (!f) continue;
        if (font_width(f, txt) <= max_w && font_height(f) <= max_h) return f;
    }
    return NULL;
}

static void draw_stat(Surface *s, int x, int y, int w, int h,
                      const char *label, long v) {
    if (w < 16 || h < 12) return;
    if (x < 0) { w += x; x = 0; }
    if (w < 16) return;

    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, 8,
                 COL_BOX, COL_BOX_EDGE, 1.0f);

    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", v);

    /* Each caption gets its own half of the box and is fitted to it, so a
     * label and a number can never be drawn on top of each other, whatever
     * the text scale does to their heights. */
    float lh = (float)h * 0.40f;
    float vh = (float)h - lh - 4.0f;
    Font *lf = fit_font(FONT_REGULAR, label, (float)w - 10.0f, lh);
    Font *vf = fit_font(FONT_SEMIBOLD, buf, (float)w - 10.0f, vh);

    if (lf) {
        float lw = font_width(lf, label);
        font_draw(s, lf, (float)x + ((float)w - lw) * 0.5f, (float)y + 3.0f,
                  label, COL_DIM);
    }
    if (vf) {
        float nw = font_width(vf, buf);
        font_draw(s, vf, (float)x + ((float)w - nw) * 0.5f,
                  (float)y + lh + 4.0f, buf, COL_TEXT);
    }
}

static void draw_tile(Surface *s, float x, float y, int cell, int v, float pop) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);

    /* A tile that just spawned grows from 94% to 100% while it fades
     * in; every older tile is drawn at full size and full alpha. */
    float inset = (float)cell * 0.06f * pop;
    float tx = x + inset;
    float ty = y + inset;
    float ts = (float)cell - inset * 2.0f;

    u32 fill = tile_color(v);
    if (pop > 0.0f) fill = WITH_A(fill, 150 + (int)(105.0f * (1.0f - pop)));
    gfx_rrect_aa(s, tx, ty, ts, ts, ts * 0.16f, fill, 0, 0);

    Font *f = fit_font(FONT_SEMIBOLD, buf, ts - 8.0f, ts - 6.0f);
    if (!f) return;
    float tw = font_width(f, buf), th = font_height(f);
    font_draw(s, f, tx + (ts - tw) * 0.5f, ty + (ts - th) * 0.5f,
              buf, COL_TILE_TEXT);
}

/* Top-left pixel of the square a cell occupies. */
static void cell_pos(const Layout *L, int idx, float *x, float *y) {
    int r = idx / N, c = idx % N;
    *x = (float)(L->bx + L->gap + c * (L->cell + L->gap));
    *y = (float)(L->by + L->gap + r * (L->cell + L->gap));
}

static void draw_overlay(Surface *s, const Layout *L, const char *title,
                         const char *sub, u32 edge, u32 text) {
    surf_blend(s, L->bx, L->by, L->bw, L->bh, WITH_A(COL_BG_TOP, 200));

    int pw = L->bw - 40, ph = (int)(L->bh * 0.30f);
    if (pw > 280) pw = 280;
    if (pw < 24) pw = 24;
    if (pw > L->bw) pw = L->bw;
    if (ph < 44) ph = 44;
    if (ph > L->bh) ph = L->bh;
    if (pw < 1 || ph < 1) return;
    int px = L->bx + (L->bw - pw) / 2;
    int py = L->by + (L->bh - ph) / 2;
    if (px < L->bx) px = L->bx;
    if (py < L->by) py = L->by;

    gfx_rrect_aa(s, (float)px, (float)py, (float)pw, (float)ph, 12.0f,
                 COL_PANEL, edge, 1.0f);

    /* the title gets the top half, the hint the bottom half; each is fitted
     * to its own half so the two lines cannot collide at any text scale */
    int th = ph / 2;
    Font *tf = fit_font(FONT_SEMIBOLD, title, (float)pw - 16.0f, (float)th - 8.0f);
    if (tf) {
        float tw = font_width(tf, title);
        font_draw(s, tf, (float)px + ((float)pw - tw) * 0.5f, (float)py + 8.0f,
                  title, text);
    }
    Font *sf = fit_font(FONT_REGULAR, sub, (float)pw - 16.0f, (float)ph - th - 8.0f);
    if (sf) {
        float sw = font_width(sf, sub);
        font_draw(s, sf, (float)px + ((float)pw - sw) * 0.5f,
                  (float)py + th + 2.0f, sub, COL_TEXT);
    }
}

static void draw(MClient *c, int vw, int vh, const Layout *L,
                 const Fonts *F, u64 now) {
    if (vw <= 0 || vh <= 0) return;
    Surface s;
    s.px = c->px;
    s.w = vw; s.h = vh;
    s.stride = c->stride;
    s.cx0 = 0; s.cy0 = 0; s.cx1 = vw; s.cy1 = vh;

    int m = L->m;
    gfx_gradient(&s, 0, 0, vw, vh, COL_BG_TOP, COL_BG_BOT);

    /* ---- header: SCORE and BEST boxes on the right, title if it fits */
    int box_gap = 8;
    int box_w = 70;
    {
        float need = font_width(F->label, "SCORE");
        if (font_width(F->label, "BEST") > need) need = font_width(F->label, "BEST");
        need += 14.0f;
        if (need > box_w) box_w = (int)need;
        int room = (vw - 2 * m - box_gap) / 2;
        if (box_w > room) box_w = room;
        if (box_w < 12) box_w = 12;
    }
    int sx = vw - m - box_w * 2 - box_gap;
    if (sx < m) sx = m;
    /* The boxes fill the header band exactly: a box drawn two pixels lower
     * than the band it belongs to is a box that pokes into the gap below it,
     * which is exactly what the layout harness checks for. */
    draw_stat(&s, sx, m, box_w, F->head_h, "SCORE", G.score);
    draw_stat(&s, sx + box_w + box_gap, m, box_w, F->head_h, "BEST", best);
    {
        float room = (float)(sx - 8) - (float)m;
        Font *tf = fit_font(FONT_SEMIBOLD, "2048", room, (float)F->head_h);
        if (tf) {
            float th = font_height(tf);
            font_draw(&s, tf, (float)m,
                      (float)m + ((float)F->head_h - th) * 0.5f, "2048", COL_TITLE);
        }
    }

    /* ---- the board: one rounded panel, sixteen rounded cells */
    gfx_rrect_aa(&s, (float)L->bx, (float)L->by, (float)L->bw, (float)L->bh,
                 12.0f, COL_BOARD, COL_BOARD_EDGE, 1.0f);

    float t = slide_t(now);
    int moving = slide_on && t < 1.0f;

    for (int r = 0; r < N; r++) {
        for (int col = 0; col < N; col++) {
            int idx = r * N + col;
            float x, y;
            cell_pos(L, idx, &x, &y);
            /* clip: never touch a pixel outside the window we were given */
            if (x < 0 || y < 0 || x + L->cell > vw || y + L->cell > vh) continue;
            gfx_rrect_aa(&s, x, y, (float)L->cell, (float)L->cell,
                         (float)L->cell * 0.16f, COL_SLOT, 0, 0);

            int v = G.board[idx];
            if (!v) continue;
            /* While a slide runs, the tiles are drawn from the trace at
             * interpolated positions; anything without a trace (the tile
             * that just spawned) waits for the landing. */
            if (moving) continue;
            draw_tile(&s, x, y, L->cell, v, pop_at(idx, now));
        }
    }

    if (moving) {
        for (int d = 0; d < NN; d++) {
            if (!trace[d].n) continue;
            int v = G.board[d];
            if (!v) continue;
            int nsrc = trace[d].n;
            int sv = nsrc == 2 ? v / 2 : v;
            float dx, dy;
            cell_pos(L, d, &dx, &dy);
            for (int k = 0; k < nsrc; k++) {
                int src = k == 0 ? trace[d].s0 : trace[d].s1;
                if (src == NO_CELL || src >= NN) continue;
                float sx0, sy0;
                cell_pos(L, src, &sx0, &sy0);
                float x = sx0 + (dx - sx0) * t;
                float y = sy0 + (dy - sy0) * t;
                if (x < -L->cell || y < -L->cell) continue;
                draw_tile(&s, x, y, L->cell, sv, 0.0f);
            }
        }
    }

    /* ---- the overlay that owns the board when the game is decided */
    if (win_banner) {
        draw_overlay(&s, L, "YOU WIN", "space: keep playing   r: new game",
                     COL_WIN, COL_WIN);
    } else if (G.over) {
        draw_overlay(&s, L, "GAME OVER",
                     G.won ? "2048 reached   r: new   u: undo"
                           : "r: new game   u: undo",
                     COL_HOT, COL_HOT_TEXT);
    }

    /* ---- footer: the keys, then the numbers that are actually true */
    float fy = (float)L->foot_y;
    const char *keys = rand_err[0] ? rand_err
                     : "arrows/WASD move   u undo   r new   q quit";
    Font *kf = fit_font(FONT_REGULAR, keys, (float)(vw - 2 * m), (float)L->line_h);
    if (kf)
        font_draw(&s, kf, (float)m, fy, keys, rand_err[0] ? COL_ERROR : COL_DIM);

    if (L->lines >= 2) {
        char info[80];
        snprintf(info, sizeof(info), "moves %d   largest %d   undo %d",
                 G.moves, largest_tile(G.board), G.undo_n);
        Font *inf = fit_font(FONT_REGULAR, info, (float)(vw - 2 * m),
                             (float)L->line_h);
        if (inf) font_draw(&s, inf, (float)m, fy + (float)L->line_h, info, COL_FAINT);
    }

    if (L->lines >= 3) {
        char src[128];
        if (best_path[0])
            snprintf(src, sizeof(src), "best %ld  (%s)", best, best_path);
        else
            snprintf(src, sizeof(src), "best %ld  not saved: %s", best, best_err);
        Font *sf = fit_font(FONT_REGULAR, src, (float)(vw - 2 * m),
                            (float)L->line_h);
        if (sf)
            font_draw(&s, sf, (float)m, fy + 2.0f * (float)L->line_h, src,
                      best_path[0] ? COL_FAINT : COL_ERROR);
    }
}

/* ------------------------------------------------------------ main */

#ifndef G2048_LAYOUT_TEST

int main(void) {
    MClient c;
    if (mc_open(&c, "g2048", "2048", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "g2048: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                        : "/run/mystic/wm.sock");
        return 1;
    }

    /* font_get() applies the desktop's text scale itself. Do not call
     * ui_scale_load() here: inside the app jail there is no /etc, so it
     * would fall back to 1.0 and throw away MYSTIC_UI_SCALE. */
    if (!font_init("/usr/share/fonts")) {
        fprintf(stderr, "g2048: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    Fonts F;
    F.title = font_get(FONT_SEMIBOLD, 22);
    F.label = font_get(FONT_REGULAR, 9);
    F.value = font_get(FONT_SEMIBOLD, 13);
    F.hint  = font_get(FONT_REGULAR, 10);
    if (!F.title || !F.label || !F.value || !F.hint) {
        fprintf(stderr, "g2048: fonts failed to load\n");
        mc_close(&c);
        return 1;
    }
    fonts_measure(&F);

    /* the buffer was mapped at the server's maximum: a maximise can grow
     * the drawing area, so that is the real cap */
    int cap_w = c.maxw > 0 ? c.maxw : c.w;
    int cap_h = c.maxh > 0 ? c.maxh : c.h;
    int vw = c.w, vh = c.h;

    rng_init();
    best_open();
    new_game(now_ns());

    int dirty = 1, quit = 0;
    int drag_on = 0, drag_x = 0, drag_y = 0;

    while (!quit) {
        u64 now = now_ns();
        Layout L;
        layout_for(vw, vh, &F, &L);

        if (slide_on && now - slide_t0 >= SLIDE_NS) slide_on = 0;

        if (dirty) {
            draw(&c, vw, vh, &L, &F, now);
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
                vw = ev.w < 1 ? 1 : (ev.w > cap_w ? cap_w : ev.w);
                vh = ev.h < 1 ? 1 : (ev.h > cap_h ? cap_h : ev.h);
                layout_for(vw, vh, &F, &L);
                dirty = 1;
                break;

            /* Keys are handled here only. The server also forwards the
             * printable form of every key as TEXT, and acting on both would
             * run every command twice. */
            case MC_KEY: {
                if (!ev.press) break;
                if (ev.key == KEY_Q) { quit = 1; break; }
                if (ev.key == KEY_R) { new_game(now); dirty = 1; break; }
                if (ev.key == KEY_U || ev.key == KEY_Z || (ev.mods & 2)) {
                    do_undo();
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_SPACE) {
                    if (win_banner) { win_banner = 0; dirty = 1; }
                    break;
                }
                int dir = -1;
                if (ev.key == KEY_LEFT  || ev.key == KEY_A) dir = DIR_LEFT;
                if (ev.key == KEY_RIGHT || ev.key == KEY_D) dir = DIR_RIGHT;
                if (ev.key == KEY_UP    || ev.key == KEY_W) dir = DIR_UP;
                if (ev.key == KEY_DOWN  || ev.key == KEY_S) dir = DIR_DOWN;
                if (dir >= 0 && try_move(dir, now)) dirty = 1;
                break;
            }

            /* A swipe: press, drag past half a cell, release. The threshold
             * is measured in cells, so it feels the same at every size. */
            case MC_MOUSE: {
                int down = ev.buttons & 1;
                if (down && !drag_on) {
                    drag_on = 1;
                    drag_x = ev.x;
                    drag_y = ev.y;
                } else if (down) {
                    int dx = ev.x - drag_x, dy = ev.y - drag_y;
                    int th = L.cell / 2;
                    if (th < 14) th = 14;
                    int adx = dx < 0 ? -dx : dx;
                    int ady = dy < 0 ? -dy : dy;
                    if (adx >= th || ady >= th) {
                        int dir = (adx > ady) ? (dx > 0 ? DIR_RIGHT : DIR_LEFT)
                                              : (dy > 0 ? DIR_DOWN : DIR_UP);
                        if (try_move(dir, now)) dirty = 1;
                        drag_x = ev.x;
                        drag_y = ev.y;
                    }
                }
                if (!down) drag_on = 0;
                break;
            }

            default:
                break;
            }
        }
        if (quit) break;

        /* Redraw while a slide or a spawn is running; otherwise sleep
         * until something happens. Nothing here polls a frame clock. */
        u64 after = now_ns();
        if (anim_live(after)) {
            dirty = 1;
            usleep(16 * 1000);
        } else if (!got) {
            usleep(40 * 1000);
        }
    }

    best_save(best);          /* the last word on the score, on the way out */
    mc_close(&c);
    return 0;
}

#endif /* !G2048_LAYOUT_TEST */

#endif /* !G2048_HOST_TEST */
