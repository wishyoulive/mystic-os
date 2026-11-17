/* ============================================================
 *  Mystic OS - apps (external) / paint
 *  A separate process: it connects to the window server, gets a
 *  shared pixel buffer, and keeps its own canvas between frames.
 *  Mouse drags are joined with a Bresenham line so a fast stroke
 *  is a stroke and not a row of dots.
 *
 *  Layout of the window (380x280 as asked for, but everything is
 *  computed from the size the server actually gave us):
 *
 *      +--------------------------------------+
 *      |  8,8   canvas (<= 360 x 220)         |
 *      +--------------------------------------+
 *      |  [1] [2] [3] [4] [5] [6]   ink name  |
 *      |  1-6 ink   c clear   q quit   pen x,y|
 *      +--------------------------------------+
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define WIN_W 380
#define WIN_H 280

#define CANVAS_W 360
#define CANVAS_H 220

#define MARGIN  8
#define GAP     6
#define PAL_H   22
#define PAL_N   6
#define SW_W    24
#define SW_GAP  6

/* The empty sheet, one step below the desktop background so the
 * canvas reads as a recessed area rather than a hole in the window. */
#define CANVAS_BG RGB(0x0E, 0x14, 0x1C)

/* The six inks. Light on purpose: the sheet is dark. */
static const u32 PALETTE[PAL_N] = {
    RGB(0xE8, 0xEC, 0xF6),   /* 1 bone   */
    RGB(0xFF, 0x6B, 0x5B),   /* 2 rust   */
    RGB(0xF5, 0xC5, 0x42),   /* 3 amber  */
    RGB(0x4F, 0xD1, 0xC5),   /* 4 teal   */
    RGB(0x7A, 0xA2, 0xF7),   /* 5 blue   */
    RGB(0xB9, 0x8B, 0xF7),   /* 6 violet */
};
static const char *PAL_NAME[PAL_N] = {
    "bone", "rust", "amber", "teal", "blue", "violet"
};

/* The canvas itself: a fixed buffer that outlives every frame. */
static u32 canvas[CANVAS_W * CANVAS_H];

/* Where everything lands this frame. */
typedef struct {
    int cx, cy, cw, ch;          /* canvas rect, in window pixels */
    int sw_x[PAL_N], sw_y, sw_w, sw_h;
    int hint_y;
} Layout;

/* ------------------------------------------------------------ canvas */

static int canvas_poke(int x, int y, u32 col) {
    if ((unsigned)x >= (unsigned)CANVAS_W || (unsigned)y >= (unsigned)CANVAS_H) return 0;
    u32 *p = &canvas[(size_t)y * CANVAS_W + x];
    if (*p == col) return 0;
    *p = col;
    return 1;
}

/* A round 5px pen: every offset with dx*dx + dy*dy <= 5. */
static int canvas_dot(int x, int y, u32 col) {
    int changed = 0;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            if (dx * dx + dy * dy > 5) continue;
            changed += canvas_poke(x + dx, y + dy, col);
        }
    return changed;
}

/* Bresenham, so two mouse samples 40px apart still form a stroke. */
static int canvas_line(int x0, int y0, int x1, int y1, u32 col) {
    int changed = 0;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;

    for (;;) {
        changed += canvas_dot(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
    return changed;
}

static int canvas_clear(void) {
    int changed = 0;
    for (size_t i = 0; i < (size_t)CANVAS_W * CANVAS_H; i++) {
        if (canvas[i] != CANVAS_BG) { canvas[i] = CANVAS_BG; changed++; }
    }
    return changed;
}

/* ------------------------------------------------------------ layout */

static void layout_for(int vw, int vh, Layout *L) {
    int cw = vw - 2 * MARGIN;
    int ch = vh - MARGIN - GAP - PAL_H - GAP - 14 - 4;   /* 14 = hint line */

    if (cw > CANVAS_W) cw = CANVAS_W;
    if (ch > CANVAS_H) ch = CANVAS_H;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;

    L->cx = MARGIN;
    L->cy = MARGIN;
    L->cw = cw;
    L->ch = ch;

    L->sw_w = SW_W;
    L->sw_h = PAL_H;
    L->sw_y = MARGIN + ch + GAP;
    for (int i = 0; i < PAL_N; i++)
        L->sw_x[i] = MARGIN + i * (SW_W + SW_GAP);

    L->hint_y = L->sw_y + PAL_H + GAP;
}

/* ------------------------------------------------------------ drawing */

static void paint_draw(MClient *c, int vw, int vh, const Layout *L,
                       Font *num, Font *hint, int sel,
                       int have_mouse, int mcx, int mcy) {
    if (vw <= 0 || vh <= 0) return;

    Surface s = { c->px, vw, vh, c->stride };

    /* the window itself */
    surf_fill(&s, 0, 0, vw, vh, RGB(0x12, 0x16, 0x1F));

    /* the sheet, with a 2px frame so the edge is legible */
    if (L->cw > 0 && L->ch > 0) {
        gfx_rrect_aa(&s, (float)L->cx - 2, (float)L->cy - 2,
                     (float)L->cw + 4, (float)L->ch + 4, 3,
                     0, RGB(0x22, 0x2B, 0x3A), 2.0f);
        Surface cv = { canvas, CANVAS_W, CANVAS_H, CANVAS_W };
        surf_push(&s, L->cx, L->cy, &cv, 0, 0, L->cw, L->ch);
    }

    /* swatches; the chosen ink gets a pale ring around it */
    int nlab_h = (int)font_height(num);
    for (int i = 0; i < PAL_N; i++) {
        float x = (float)L->sw_x[i], y = (float)L->sw_y;
        if (x >= (float)vw) break;
        if (i == sel)
            gfx_rrect_aa(&s, x - 2, y - 2, (float)L->sw_w + 4, (float)L->sw_h + 4, 7,
                         color_mix(PALETTE[i], RGB(0xF2, 0xF5, 0xFA), 0.55f), 0, 0);
        gfx_rrect_aa(&s, x, y, (float)L->sw_w, (float)L->sw_h, 5, PALETTE[i], 0, 0);

        char digit[2] = { (char)('1' + i), 0 };
        float dw = font_width(num, digit);
        font_draw(&s, num, x + ((float)L->sw_w - dw) * 0.5f,
                  y + ((float)L->sw_h - (float)nlab_h) * 0.5f,
                  digit, RGB(0x12, 0x16, 0x1F));
    }

    /* which ink is loaded, named */
    float name_x = (float)(L->sw_x[PAL_N - 1] + L->sw_w) + 16;
    float name_w = font_width(num, PAL_NAME[sel]);
    if (name_x + name_w <= (float)vw - MARGIN)
        font_draw(&s, num, name_x,
                  (float)L->sw_y + ((float)L->sw_h - (float)nlab_h) * 0.5f,
                  PAL_NAME[sel], PALETTE[sel]);

    /* hint line, and the pen position when the pointer is on the sheet */
    const char *keys = "1-6 ink    c clear    q quit";
    font_draw(&s, hint, (float)MARGIN, (float)L->hint_y, keys, RGB(0x8A, 0x93, 0xA6));

    if (have_mouse) {
        char pos[32];
        snprintf(pos, sizeof(pos), "pen %d,%d", mcx, mcy);
        float pw = font_width(hint, pos);
        float limit = (float)MARGIN + font_width(hint, keys) + 12;
        if ((float)vw - MARGIN - pw > limit)
            font_draw(&s, hint, (float)vw - MARGIN - pw, (float)L->hint_y,
                      pos, RGB(0x5E, 0x6B, 0x82));
    }
}

/* ------------------------------------------------------------ main */

int main(void) {
    MClient c;
    if (mc_open(&c, "paint", "Paint", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "paint: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    font_init("/usr/share/fonts");
    Font *num  = font_get(FONT_SEMIBOLD, 10);
    Font *hint = font_get(FONT_REGULAR, 11);
    if (!num || !hint) {
        fprintf(stderr, "paint: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    /* the shared buffer's real extent; a RESIZE can only shrink the
     * area we draw into, never grow past what the server mapped */
    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;

    canvas_clear();

    Layout L;
    int sel = 0;
    int drawing = 0, last_x = 0, last_y = 0;
    int was_down = 0;
    int have_mouse = 0, mcx = 0, mcy = 0;
    int dirty = 1;

    for (;;) {
        layout_for(vw, vh, &L);

        if (dirty) {
            paint_draw(&c, vw, vh, &L, num, hint, sel, have_mouse, mcx, mcy);
            mc_present(&c);
            dirty = 0;
        }

        int got = 0;
        MEvent ev;
        while (mc_poll(&c, &ev)) {
            int quit = 0;
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

            case MC_KEY:
                if (!ev.press) break;
                if (ev.key >= 2 && ev.key <= 7) {            /* KEY_1 .. KEY_6 */
                    if (sel != ev.key - 2) { sel = ev.key - 2; dirty = 1; }
                } else if (ev.key == 46) {                   /* KEY_C */
                    if (canvas_clear() > 0) dirty = 1;
                } else if (ev.key == 16) {                   /* KEY_Q */
                    quit = 1;
                }
                break;

            case MC_TEXT:
                for (const char *p = ev.text; *p && !quit; p++) {
                    if (*p >= '1' && *p <= '6') {
                        if (sel != *p - '1') { sel = *p - '1'; dirty = 1; }
                    } else if (*p == 'c' || *p == 'C') {
                        if (canvas_clear() > 0) dirty = 1;
                    } else if (*p == 'q' || *p == 'Q') {
                        quit = 1;
                    }
                }
                break;

            case MC_MOUSE: {
                int x = ev.x - L.cx, y = ev.y - L.cy;
                int down = ev.buttons & 1;

                if (!have_mouse || x != mcx || y != mcy) {
                    have_mouse = 1;
                    mcx = x;
                    mcy = y;
                    dirty = 1;
                }

                if (down && x >= 0 && y >= 0 && x < L.cw && y < L.ch) {
                    int changed = drawing ? canvas_line(last_x, last_y, x, y, PALETTE[sel])
                                          : canvas_dot(x, y, PALETTE[sel]);
                    if (changed) dirty = 1;
                    last_x = x;
                    last_y = y;
                    drawing = 1;
                } else {
                    drawing = 0;
                    if (down && !was_down) {              /* a press on a swatch */
                        for (int i = 0; i < PAL_N; i++) {
                            if (ev.x >= L.sw_x[i] && ev.x < L.sw_x[i] + L.sw_w &&
                                ev.y >= L.sw_y && ev.y < L.sw_y + L.sw_h) {
                                if (sel != i) { sel = i; dirty = 1; }
                                break;
                            }
                        }
                    }
                }
                was_down = down;
                break;
            }

            default:
                break;
            }

            if (quit) {
                mc_close(&c);
                return 0;
            }
        }

        if (!got && !dirty) usleep(30 * 1000);
    }
}
