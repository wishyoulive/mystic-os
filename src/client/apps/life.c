/* ============================================================
 *  Mystic OS - apps (external) / life
 *  Conway's Game of Life as its own process. It connects to the
 *  window server, gets a shared pixel buffer, and runs a real
 *  cellular automaton in it: one bit per cell, one generation
 *  per timer tick, timed with clock_gettime(CLOCK_MONOTONIC).
 *
 *  Nothing here is compiled into the desktop and nothing here is
 *  a picture of a game of life: the grid is a bitset that is
 *  actually evolved, seeded from /dev/urandom, and the population
 *  and generation counters are counted from that bitset.
 *
 *  Layout (everything derives from the size the server gave us):
 *
 *      +--------------------------------------+
 *      |  Conway's Game of Life     running   |
 *      |  +--------------------------------+  |
 *      |  |  the grid, one rounded rect    |  |
 *      |  |  per live cell                 |  |
 *      |  +--------------------------------+  |
 *      |  gen 412  pop 318  fps 10.0  66x40   |
 *      |  space pause  s step  r reseed ...   |
 *      +--------------------------------------+
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

#define WIN_W 420
#define WIN_H 320

/* Preferred cell pitch in window pixels. The real pitch is derived
 * from the size the server gave us (see layout_for); these two maxima
 * are the largest grid the window could ever ask for, and the bitsets
 * below are sized for them. */
#define CELL 6
#define MAX_COLS 128
#define MAX_ROWS 96
#define NBUF ((MAX_COLS * MAX_ROWS + 7) / 8)   /* 1536 bytes: 1 bit/cell */

/* Cells per random byte: a byte < DENSITY is alive, so ~30% of the
 * board starts alive, which is the density that gives Life room to
 * move instead of instantly collapsing or freezing. */
#define DENSITY 77

/* One generation every 100 ms -> about 10 a second. */
#define TICK_NS 100000000ull

#define MARGIN 12

/* Colours: the sheet is darker than the window so the live cells
 * read as light on a recessed panel. */
#define COL_WIN_TOP    RGB(0x12, 0x18, 0x22)
#define COL_WIN_BOT    RGB(0x0B, 0x0F, 0x16)
#define COL_GRID       RGB(0x07, 0x0A, 0x10)
#define COL_GRID_EDGE  RGB(0x22, 0x2B, 0x3A)
#define COL_CELL       RGB(0x5F, 0xE3, 0xC6)
#define COL_TITLE      RGB(0xE8, 0xEC, 0xF6)
#define COL_RUNNING    RGB(0x4F, 0xD1, 0xC5)
#define COL_PAUSED     RGB(0xF5, 0xC5, 0x42)
#define COL_DIM        RGB(0x8A, 0x93, 0xA6)
#define COL_FAINT      RGB(0x5E, 0x6B, 0x82)
#define COL_ERROR      RGB(0xFF, 0x8A, 0x7A)

/* ------------------------------------------------------------ state */

/* Two bitsets: the board, and the board being computed. Swapping
 * them costs a memcpy of at most 1536 bytes. */
static u8 cells[NBUF];
static u8 scratch[NBUF];

/* One random byte per cell when seeding; 12288 bytes worst case. */
static u8 random_bytes[MAX_COLS * MAX_ROWS];

static int gcols, grows;

typedef struct {
    long gen;
    int  pop;
    int  paused;
    double fps;
    char err[80];        /* /dev/urandom failure, empty when fine */
} State;

typedef struct {
    int gx, gy, gw, gh;  /* the grid's pixel rect inside the window */
    int pitch;           /* pixels per cell, chosen from the window size */
    int title_y, foot_y, hint_y;
} Layout;

/* ------------------------------------------------------------ time */

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* ------------------------------------------------------------ bits */

static inline int bit_get(const u8 *b, int cols, int x, int y) {
    int i = y * cols + x;
    return (b[i >> 3] >> (i & 7)) & 1;
}

static inline void bit_set(u8 *b, int cols, int x, int y, int v) {
    int i = y * cols + x;
    if (v) b[i >> 3] |= (u8)(1u << (i & 7));
    else   b[i >> 3] &= (u8)~(1u << (i & 7));
}

static int board_pop(const u8 *b) {
    int n = 0;
    for (int y = 0; y < grows; y++)
        for (int x = 0; x < gcols; x++)
            n += bit_get(b, gcols, x, y);
    return n;
}

/* ------------------------------------------------------------ seeding */

/* Read the real random device into random_bytes[]. Returns 0 and
 * fills err on failure; the board is untouched until the read has
 * actually succeeded, so a failed reseed leaves the old board up. */
static int read_random(int n, char *err, size_t errsz) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "urandom: %s", strerror(errno));
        return 0;
    }
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, random_bytes + got, (size_t)(n - got));
        if (r > 0) { got += (int)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        snprintf(err, errsz, "urandom: %s", r == 0 ? "short read" : strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static int board_seed(char *err, size_t errsz) {
    int n = gcols * grows;
    if (!read_random(n, err, errsz)) return -1;

    memset(cells, 0, sizeof(cells));
    int pop = 0;
    for (int y = 0; y < grows; y++) {
        for (int x = 0; x < gcols; x++) {
            int alive = random_bytes[y * gcols + x] < DENSITY;
            bit_set(cells, gcols, x, y, alive);
            pop += alive;
        }
    }
    return pop;
}

/* ------------------------------------------------------------ evolve */

/* One generation, on a torus: the edges wrap, which keeps a glider
 * travelling for ever instead of dying on the wall. Returns the new
 * population, counted while the cells are written. */
static int evolve(void) {
    int pop = 0;
    for (int y = 0; y < grows; y++) {
        int ym = (y == 0) ? grows - 1 : y - 1;
        int yp = (y == grows - 1) ? 0 : y + 1;
        for (int x = 0; x < gcols; x++) {
            int xm = (x == 0) ? gcols - 1 : x - 1;
            int xp = (x == gcols - 1) ? 0 : x + 1;

            int n = bit_get(cells, gcols, xm, ym) + bit_get(cells, gcols, x, ym)
                  + bit_get(cells, gcols, xp, ym) + bit_get(cells, gcols, xm, y)
                  + bit_get(cells, gcols, xp, y)  + bit_get(cells, gcols, xm, yp)
                  + bit_get(cells, gcols, x, yp)  + bit_get(cells, gcols, xp, yp);

            int alive = bit_get(cells, gcols, x, y);
            int born = alive ? (n == 2 || n == 3) : (n == 3);
            bit_set(scratch, gcols, x, y, born);
            pop += born;
        }
    }
    memcpy(cells, scratch, sizeof(cells));
    return pop;
}

/* ------------------------------------------------------------ resize */

/* A RESIZE can arrive when the window is dragged/restored. The bits
 * are indexed by y*cols+x, so the old board cannot simply be reused:
 * copy the part that still fits, top-left aligned, into a clean
 * board of the new size. */
static int board_resize(int cols, int rows) {
    int oc = gcols, orows = grows;
    u8 saved[NBUF];
    memcpy(saved, cells, sizeof(saved));

    memset(cells, 0, sizeof(cells));
    gcols = cols;
    grows = rows;

    int c = cols < oc ? cols : oc;
    int r = rows < orows ? rows : orows;
    for (int y = 0; y < r; y++)
        for (int x = 0; x < c; x++)
            bit_set(cells, gcols, x, y, bit_get(saved, oc, x, y));

    return board_pop(cells);
}

/* ------------------------------------------------------------ layout */

static void layout_for(int vw, int vh, Layout *L, int *cols, int *rows) {
    L->title_y = MARGIN;
    L->foot_y  = vh - MARGIN - 18;
    L->hint_y  = vh - MARGIN - 2;

    int top = MARGIN + 26;
    int bot = L->foot_y - 10;
    if (bot < top + CELL) bot = top + CELL;

    int avail_w = vw - 2 * MARGIN;
    int avail_h = bot - top;
    if (avail_w > vw) avail_w = vw;
    if (avail_h > vh) avail_h = vh;
    if (avail_w < 3) avail_w = 3;
    if (avail_h < 3) avail_h = 3;

    /* The cell size comes from the window, not from a constant: start
     * at 6px, shrink towards 3 when the window is too narrow for a
     * usable board, and grow when the window is big enough that the
     * 128x96 limit would otherwise leave the grid stranded in the
     * middle of a wide screen. */
    int pitch = CELL;
    while (pitch > 3 && avail_w / pitch < 24) pitch--;
    while (pitch < 16 && (avail_w / pitch > MAX_COLS || avail_h / pitch > MAX_ROWS))
        pitch++;

    int c = avail_w / pitch;
    int r = avail_h / pitch;

    /* The band between the header and the footer can be taller than a
     * very short window; the grid must still land inside the pixels we
     * were actually given, so cap it by the window itself. */
    if (c * pitch > vw && vw >= pitch) c = vw / pitch;
    if (r * pitch > vh && vh >= pitch) r = vh / pitch;

    if (c > MAX_COLS) c = MAX_COLS;
    if (r > MAX_ROWS) r = MAX_ROWS;
    if (c < 1) c = 1;
    if (r < 1) r = 1;

    L->pitch = pitch;
    L->gw = c * pitch;
    L->gh = r * pitch;
    L->gx = (vw - L->gw) / 2;
    L->gy = top + (avail_h - L->gh) / 2;
    if (L->gx + L->gw > vw) L->gx = vw - L->gw;
    if (L->gy + L->gh > vh) L->gy = vh - L->gh;
    if (L->gx < 0) L->gx = 0;
    if (L->gy < 0) L->gy = 0;

    *cols = c;
    *rows = r;
}

/* ------------------------------------------------------------ draw */

static void draw(MClient *c, int vw, int vh, const Layout *L,
                 Font *ft, Font *fb, Font *fs, const State *st) {
    if (vw <= 0 || vh <= 0) return;
    Surface s = { c->px, vw, vh, c->stride };

    gfx_gradient(&s, 0, 0, vw, vh, COL_WIN_TOP, COL_WIN_BOT);

    /* the panel the automaton lives on; this fill *is* the dead cells */
    if (L->gw > 0 && L->gh > 0) {
        surf_fill(&s, L->gx, L->gy, L->gw, L->gh, COL_GRID);
        gfx_rrect_aa(&s, (float)L->gx - 1, (float)L->gy - 1,
                     (float)L->gw + 2, (float)L->gh + 2, 5,
                     0, COL_GRID_EDGE, 1.0f);

        /* one filled rounded rect per live cell */
        int p = L->pitch;
        for (int y = 0; y < grows; y++) {
            float cy = (float)(L->gy + y * p) + 0.6f;
            if (cy > (float)L->gy + (float)L->gh) break;
            for (int x = 0; x < gcols; x++) {
                if (!bit_get(cells, gcols, x, y)) continue;
                float cx = (float)(L->gx + x * p) + 0.6f;
                gfx_rrect_aa(&s, cx, cy,
                             (float)p - 1.2f, (float)p - 1.2f, (float)p * 0.28f,
                             COL_CELL, 0, 0);
            }
        }
    }

    /* header */
    font_draw(&s, ft, (float)MARGIN, (float)L->title_y,
              "Conway's Game of Life", COL_TITLE);

    const char *mode = st->paused ? "paused" : "running";
    u32 mode_col = st->paused ? COL_PAUSED : COL_RUNNING;
    float mw = font_width(fb, mode);
    if ((float)MARGIN + mw < (float)vw)
        font_draw(&s, fb, (float)vw - MARGIN - mw,
                  (float)L->title_y + (font_height(ft) - font_height(fb)) * 0.5f,
                  mode, mode_col);

    /* footer: the three numbers are all measured, never guessed */
    char line[128];
    snprintf(line, sizeof(line), "gen %ld   pop %d   fps %.1f",
             st->gen, st->pop, st->fps);
    font_draw(&s, fb, (float)MARGIN, (float)L->foot_y, line, COL_DIM);

    char dims[32];
    snprintf(dims, sizeof(dims), "%dx%d cells", gcols, grows);
    float dw = font_width(fs, dims);
    float limit = (float)MARGIN + font_width(fb, line) + 12;
    if ((float)vw - MARGIN - dw > limit)
        font_draw(&s, fs, (float)vw - MARGIN - dw,
                  (float)L->foot_y + (font_height(fb) - font_height(fs)),
                  dims, COL_FAINT);

    /* hint line, or the real reason seeding failed */
    if (st->err[0]) {
        font_draw(&s, fs, (float)MARGIN, (float)L->hint_y, st->err, COL_ERROR);
    } else {
        font_draw(&s, fs, (float)MARGIN, (float)L->hint_y,
                  "space pause   s step   r reseed   click a cell   q quit",
                  COL_DIM);
    }
}

/* ------------------------------------------------------------ main */

int main(void) {
    MClient c;
    if (mc_open(&c, "life", "Life", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "life: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    font_init("/usr/share/fonts");
    Font *ft = font_get(FONT_SEMIBOLD, 13);
    Font *fb = font_get(FONT_REGULAR, 11);
    Font *fs = font_get(FONT_REGULAR, 10);
    if (!ft || !fb || !fs) {
        fprintf(stderr, "life: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    /* the shm was mapped for exactly this size; a RESIZE may only
     * give us less, never more */
    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;

    Layout L;
    int cols = 1, rows = 1;
    layout_for(vw, vh, &L, &cols, &rows);
    gcols = cols;
    grows = rows;

    State st;
    memset(&st, 0, sizeof(st));
    memset(cells, 0, sizeof(cells));
    memset(scratch, 0, sizeof(scratch));

    int seeded = board_seed(st.err, sizeof(st.err));
    st.pop = seeded > 0 ? seeded : 0;
    if (seeded < 0) st.paused = 1;   /* nothing to run; show the reason */

    int dirty = 1, quit = 0, was_down = 0;
    unsigned long frames = 0;
    u64 tick_t = now_ns();
    u64 fps_t0 = tick_t;

    for (;;) {
        MEvent ev;
        while (!quit && mc_poll(&c, &ev)) {
            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;

            case MC_RESIZE:
                vw = ev.w < cap_w ? ev.w : cap_w;
                vh = ev.h < cap_h ? ev.h : cap_h;
                if (vw < 32) vw = 32;
                if (vh < 32) vh = 32;
                layout_for(vw, vh, &L, &cols, &rows);
                if (cols != gcols || rows != grows) {
                    st.pop = board_resize(cols, rows);
                    dirty = 1;
                }
                break;

            /* Keys are handled here only. The server also forwards the
             * printable form of every key as TEXT in the same frame, and
             * a pause toggle that ran twice would be no toggle at all. */
            case MC_KEY:
                if (!ev.press) break;
                switch (ev.key) {
                case 57:                                  /* KEY_SPACE */
                    st.paused = !st.paused;
                    tick_t = now_ns();
                    dirty = 1;
                    break;
                case 19: {                                /* KEY_R */
                    int n = board_seed(st.err, sizeof(st.err));
                    if (n >= 0) {
                        st.pop = n;
                        st.gen = 0;
                        tick_t = now_ns();
                    }
                    dirty = 1;
                    break;
                }
                case 31:                                  /* KEY_S */
                    if (st.paused && !st.err[0]) {
                        st.pop = evolve();
                        st.gen++;
                        dirty = 1;
                    }
                    break;
                case 16:                                  /* KEY_Q */
                    quit = 1;
                    break;
                default:
                    break;
                }
                break;

            case MC_MOUSE: {
                int down = ev.buttons & 1;
                if (down && !was_down) {
                    int gx = (ev.x - L.gx) / L.pitch;
                    int gy = (ev.y - L.gy) / L.pitch;
                    if (ev.x >= L.gx && ev.y >= L.gy &&
                        gx >= 0 && gx < gcols && gy >= 0 && gy < grows) {
                        int alive = bit_get(cells, gcols, gx, gy);
                        bit_set(cells, gcols, gx, gy, !alive);
                        st.pop = board_pop(cells);
                        dirty = 1;
                    }
                }
                was_down = down;
                break;
            }

            default:
                break;
            }
        }
        if (quit) {
            mc_close(&c);
            return 0;
        }

        u64 t = now_ns();

        /* one generation per tick, never a burst to catch up */
        if (!st.paused && t - tick_t >= TICK_NS) {
            st.pop = evolve();
            st.gen++;
            tick_t = t;
            dirty = 1;
        }

        /* a real frame counter, read back from the clock */
        if (t - fps_t0 >= 1000000000ull) {
            st.fps = (double)frames * 1e9 / (double)(t - fps_t0);
            frames = 0;
            fps_t0 = t;
            dirty = 1;                 /* the number on screen is stale */
        }

        if (dirty) {
            draw(&c, vw, vh, &L, ft, fb, fs, &st);
            mc_present(&c);
            frames++;
            dirty = 0;

            /* just drew: look for input immediately */
            continue;
        }

        /* idle: wake at the next tick, or soon, so keys feel live */
        u64 t2 = now_ns();
        u64 due = (st.paused || st.err[0]) ? t2 + 2000000ull : tick_t + TICK_NS;
        u64 wait = due > t2 ? due - t2 : 0;
        if (wait > 8000000ull) wait = 8000000ull;
        if (wait < 1000000ull) wait = 1000000ull;
        usleep((useconds_t)(wait / 1000u));
    }
}
