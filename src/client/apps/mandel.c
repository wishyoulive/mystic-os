/* ============================================================
 *  Mystic OS - apps (external) / mandel
 *
 *  An interactive Mandelbrot explorer that runs as its own
 *  process. It connects to the window server, is handed a shared
 *  pixel buffer, and then does all of the work itself on the CPU:
 *  nothing here is compiled into the desktop and the desktop never
 *  sees a complex number.
 *
 *  The picture is computed, not stored:
 *
 *      z(0) = 0 ;  z(n+1) = z(n)^2 + c        c = the pixel
 *      n counts up to MAX_ITER; |z| > 2 means "escaped"
 *
 *  A pixel that never escapes is taken to be inside the set and is
 *  painted near-black. Everything else is coloured by how long it
 *  took to escape. The raw count is an integer, which would give
 *  visible stripes, so the fractional part is recovered from the
 *  last |z| with the usual smoothing identity
 *
 *      nu = n + 1 - log2(log2(|z|))
 *
 *  nu then addresses a 256-entry sine palette, interpolated between
 *  entries so the ramp is continuous rather than 256 flat bands.
 *  The address is sqrt(nu)*32 rather than nu itself because escape
 *  counts pile up at the low end: on the default view 77% of the
 *  pixels escape before n = 8 and only 0.7% get past 32. Colour by
 *  nu directly and those 77% all land in the first tenth of the
 *  ramp and the picture is one flat wash. The square root spreads
 *  the crowded end across the palette and still puts nu = 64 (the
 *  cap) at the top of it.
 *
 *  Rendering is progressive. A whole 420x274 frame is ~115k pixels
 *  at up to 64 iterations each; doing it in one go would freeze the
 *  window for a noticeable moment. Instead one band of BAND_ROWS
 *  rows is computed per loop turn, the events are drained in between,
 *  and the frame is presented again so the picture visibly fills in
 *  from the top with a percentage over it. Only the rows that exist
 *  are copied out of the buffer, so a half-finished frame never shows
 *  stale pixels from an older view.
 *
 *  The clock is real: clock_gettime(CLOCK_MONOTONIC) around the whole
 *  render, and the footer shows that duration for the last frame that
 *  actually completed - not an estimate.
 *
 *  State is fixed size: one 512x384 pixel buffer and a handful of
 *  doubles. Nothing is allocated while running, so an MC_RESIZE only
 *  recomputes the aspect ratio and throws the current frame away; the
 *  pixels the server mapped can never grow, so the client area is
 *  clamped to what mc_open() was given.
 *
 *  Keys:  r  reset the view        q  quit        (closing quits too)
 *  Mouse: left click zooms in 2x centred on the point clicked,
 *         right click zooms out 2x, same anchor.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WIN_W 420
#define WIN_H 320

/* The escape-time cap. Fixed: the footer reports it, and the zoom
 * limits below are chosen so that 64 iterations still resolves the
 * detail at the deepest zoom instead of painting a black blob. */
#define MAX_ITER 64

/* rows of the picture computed per loop turn */
#define BAND_ROWS 16

/* the persistent render buffer. The window is 420x320, so this has
 * headroom for any resize the server can legitimately ask for. */
#define IMG_MAX_W 512
#define IMG_MAX_H 384

/* Footer geometry. Lato at 10px measures 14.00px tall (8.23 ascent,
 * 5.77 descent), so three lines need 42px; 46 leaves two above and two
 * below. At 42 the descenders of the last line were cut off by the
 * bottom edge of the window, which is why these are separate numbers
 * and not a guess. */
#define FOOT_H  46
#define FOOT_L1  2
#define FOOT_L2 16
#define FOOT_L3 30

/* The starting view: centred a little left of the origin so the
 * whole set sits comfortably, 2.6 imaginary units tall. 420x274 of
 * aspect 1.53 then spans about 4.0 units horizontally. */
#define BASE_CRE  (-0.6)
#define BASE_CIM  (0.0)
#define BASE_SPAN (2.6)

/* Zoom limits. 2.6 / 0.00127 is about 2000x in, and that is as far as
 * 64 iterations can honestly go: at 4000x only a third of the frame
 * still escapes and at 10000x all but 2% of it collapses into "inside",
 * i.e. the picture would be a black rectangle rather than the set. The
 * zoom stops while the picture is still true. 13 units out is 0.2x,
 * enough to see the set in context without it shrinking to a dot. */
#define MIN_SPAN (0.00127)
#define MAX_SPAN (13.0)

/* evdev key codes (input-event-codes.h): KEY_Q == 16, KEY_R == 19.
 * The server forwards raw codes, the same ones paint.c switches on. */
#define KEYC_Q 16
#define KEYC_R 19

#define TAU 6.283185307179586

/* ------------------------------------------------------------ colours */
static const u32 C_VOID   = RGB(0x08, 0x0A, 0x10);   /* not yet rendered */
static const u32 C_PANEL  = RGB(0x0B, 0x10, 0x18);   /* footer           */
static const u32 C_RULE   = RGB(0x1E, 0x27, 0x36);   /* footer hairline  */
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_INSIDE = RGB(0x06, 0x04, 0x0C);   /* inside the set   */

/* ------------------------------------------------------------ palette
 * 256 colours, built once from three sine phases and a luminance
 * ramp. The phases are a third of a turn apart, so the whole hue
 * circle is walked exactly once, and they are offset by 0.58 so the
 * dark end comes out the deep blue the rest of the system uses
 * instead of the olive green that phase zero lands on. Luminance
 * climbs as t^0.7 over the same t, so the far-outside escapes fade
 * into the background and the crowded bands near the boundary are
 * the warm, bright ones. 0.75 gamma on the way to 8 bits because a
 * straight scale greys the mid tones out.
 */
#define PAL_N 256
static u32 PAL[PAL_N];

static void palette_build(void) {
    for (int i = 0; i < PAL_N; i++) {
        double t = (double)i / (double)PAL_N;      /* 0 .. 1 across the ramp */
        double r = 0.5 + 0.5 * sin(TAU * (t + 0.58));
        double g = 0.5 + 0.5 * sin(TAU * (t + 0.75));
        double b = 0.5 + 0.5 * sin(TAU * (t + 0.92));
        double lum = pow(t, 0.7);                  /* nothing at t=0 */
        r = pow(r * lum, 0.75);
        g = pow(g * lum, 0.75);
        b = pow(b * lum, 0.75);
        int ir = (int)(255.0 * r + 0.5);
        int ig = (int)(255.0 * g + 0.5);
        int ib = (int)(255.0 * b + 0.5);
        if (ir > 255) ir = 255;
        if (ig > 255) ig = 255;
        if (ib > 255) ib = 255;
        PAL[i] = RGB(ir, ig, ib);
    }
}

/* ------------------------------------------------------------ state */
typedef struct {
    int w, h;              /* client area, never past what was mapped  */
    int cap_w, cap_h;      /* the extent mc_open() actually mapped     */
    int fw, fh;            /* the picture rect, at 0,0                 */
    int footer_y;

    double cre, cim;       /* view centre, double precision            */
    double span;           /* vertical extent in imaginary units       */

    u32    img[IMG_MAX_W * IMG_MAX_H];

    int    rows_done;      /* rows of img that belong to this view     */
    int    rendering;      /* a frame is part-way through              */
    int    have_frame;     /* at least one frame has completed         */
    struct timespec t0;    /* start of the render in progress          */
    double last_ms;        /* duration of the last completed render    */

    int    prev_buttons;   /* so a held button zooms once, not per px  */
} Mandel;

/* one window, so one state block, all of it fixed size */
static Mandel M;

/* ------------------------------------------------------------ algorithm */

/* Iterate z <- z^2 + c until it escapes or we run out of iterations.
 * Returns the smoothed escape count, or a negative number when the
 * point stayed bounded (i.e. it is inside the set). */
static double escape_time(double cr, double ci) {
    double zr = 0.0, zi = 0.0;
    int n;

    for (n = 0; n < MAX_ITER; n++) {
        double zr2 = zr * zr;
        double zi2 = zi * zi;
        if (zr2 + zi2 > 4.0) break;          /* |z| > 2: escaped */
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
    }
    if (n >= MAX_ITER) return -1.0;

    /* zr,zi are the pair that broke out, so |z| > 2 and log2(|z|) > 1.
     * log2(log2(|z|)) == log2(0.5 * log2(|z|^2)). */
    double mag2 = zr * zr + zi * zi;
    double lg = 0.5 * log2(mag2);
    return (double)n + 1.0 - log2(lg);
}

/* nu -> colour, interpolating between palette entries so the bands
 * are a continuous ramp instead of 256 steps. sqrt(nu)*32 maps the
 * whole range 0..MAX_ITER onto 0..PAL_N and pushes the crowd at the
 * low end out across the palette instead of into its first tenth. */
static u32 shade(double nu) {
    if (nu < 0.0) return C_INSIDE;

    double f = sqrt(nu) * 32.0;
    int i0 = (int)f;
    if (i0 < 0) i0 = 0;
    if (i0 > PAL_N - 1) i0 = PAL_N - 1;
    int i1 = (i0 < PAL_N - 1) ? i0 + 1 : PAL_N - 1;
    double fr = f - (double)i0;
    if (fr < 0.0) fr = 0.0;
    if (fr > 1.0) fr = 1.0;
    return color_mix(PAL[i0], PAL[i1], (float)fr);
}

/* Compute rows [y0,y1) of the current view into img. Pixel centres,
 * not corners: that shifts the mapping by half a pixel and keeps the
 * view symmetrical about its centre as it doubles. */
static void render_rows(int y0, int y1) {
    double aspect = (double)M.fw / (double)M.fh;
    double span_re = M.span * aspect;
    double re0 = M.cre - span_re * 0.5;          /* left edge          */
    double im1 = M.cim + M.span * 0.5;           /* top edge           */
    double dre = span_re / (double)M.fw;
    double dim = M.span / (double)M.fh;

    for (int y = y0; y < y1; y++) {
        double ci = im1 - ((double)y + 0.5) * dim;
        u32 *row = M.img + (size_t)y * IMG_MAX_W;
        for (int x = 0; x < M.fw; x++) {
            double cr = re0 + ((double)x + 0.5) * dre;
            row[x] = shade(escape_time(cr, ci));
        }
    }
}

/* ------------------------------------------------------------ view */

static void clamp_view(void) {
    if (M.span < MIN_SPAN) M.span = MIN_SPAN;
    if (M.span > MAX_SPAN) M.span = MAX_SPAN;
    /* a safety net: zooming about a click can only ever put the centre
     * inside the view, but zooming out around the edge of the plane
     * could otherwise walk it away from the set entirely */
    if (M.cre < -2.5) M.cre = -2.5;
    if (M.cre >  1.5) M.cre =  1.5;
    if (M.cim < -1.5) M.cim = -1.5;
    if (M.cim >  1.5) M.cim =  1.5;
}

static void layout(void) {
    /* a client area taller than the buffer plus a footer would leave a
     * dead strip between the picture and the footer, so cap it here */
    if (M.h > IMG_MAX_H + FOOT_H) M.h = IMG_MAX_H + FOOT_H;

    M.fw = M.w;
    M.fh = M.h - FOOT_H;
    M.footer_y = M.h - FOOT_H;
    if (M.footer_y < 0) M.footer_y = 0;
    if (M.fh < 0) M.fh = 0;
    /* the buffer is what it is; never index past it */
    if (M.fw > IMG_MAX_W) M.fw = IMG_MAX_W;
    if (M.fh > IMG_MAX_H) M.fh = IMG_MAX_H;
}

static void reset_view(void) {
    M.cre = BASE_CRE;
    M.cim = BASE_CIM;
    M.span = BASE_SPAN;
}

/* Throw the current frame away and start the clock on a new one. */
static void render_start(void) {
    M.rows_done = 0;
    M.rendering = 1;
    clock_gettime(CLOCK_MONOTONIC, &M.t0);
}

/* Stop the clock: this is the number the footer reports. */
static void render_finish(void) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    M.last_ms = (double)(t1.tv_sec - M.t0.tv_sec) * 1000.0
              + (double)(t1.tv_nsec - M.t0.tv_nsec) / 1000000.0;
    if (M.last_ms < 0.0) M.last_ms = 0.0;
    M.rendering = 0;
    M.have_frame = 1;
}

/* ------------------------------------------------------------ input */

/* Zoom 2x about the pixel that was clicked: that pixel's complex
 * coordinate becomes the new centre. A right click at the centre of
 * the window therefore undoes a left click made there. */
static void zoom_at(int px, int py, int in) {
    if (M.fw <= 0 || M.fh <= 0) return;
    if ((unsigned)px >= (unsigned)M.fw || (unsigned)py >= (unsigned)M.fh) return;

    double aspect = (double)M.fw / (double)M.fh;
    double span_re = M.span * aspect;
    double re = M.cre + (((double)px + 0.5) / (double)M.fw - 0.5) * span_re;
    double im = M.cim + (0.5 - ((double)py + 0.5) / (double)M.fh) * M.span;

    M.span = in ? (M.span * 0.5) : (M.span * 2.0);
    M.cre = re;
    M.cim = im;
    clamp_view();
    render_start();
}

static void handle_mouse(int x, int y, int buttons) {
    /* a press is a 0 -> 1 edge; the server keeps sending MOUSE while
     * the button is held, and without the edge test a single click
     * would zoom once per pixel of held-button motion */
    int press_l = (buttons & 1) && !(M.prev_buttons & 1);
    int press_r = (buttons & 2) && !(M.prev_buttons & 2);
    M.prev_buttons = buttons;

    if (press_l) zoom_at(x, y, 1);
    else if (press_r) zoom_at(x, y, 0);
}

/* ------------------------------------------------------------ drawing */

static void draw(MClient *c, Font *fmed, Font *freg) {
    int w = M.w, h = M.h;
    if (w <= 0 || h <= 0) return;

    /* the surface is the client area only, so every gfx_/surf_ call
     * below is clipped to the window - nothing can bleed out of it */
    Surface s = { c->px, w, h, c->stride };

    surf_fill(&s, 0, 0, w, h, C_VOID);

    /* the rows that exist for this view; a partial frame shows only
     * what has actually been computed, never an older picture */
    if (M.fw > 0 && M.fh > 0 && M.rows_done > 0) {
        int rows = M.rows_done < M.fh ? M.rows_done : M.fh;   /* never past the picture */
        Surface im = { M.img, IMG_MAX_W, IMG_MAX_H, IMG_MAX_W };
        surf_push(&s, 0, 0, &im, 0, 0, M.fw, rows);
    }

    int pct = 100;
    if (M.rendering) {
        pct = (M.fh > 0) ? (int)((long)M.rows_done * 100 / M.fh) : 0;
        if (pct > 100) pct = 100;
    } else if (!M.have_frame) {
        pct = 0;
    }

    /* while a frame is arriving: a chip in the corner and a bar along
     * the bottom edge of the picture, both inside the picture rect */
    if (M.rendering && M.fh > 0) {
        char chip[32];
        snprintf(chip, sizeof(chip), "rendering %d%%", pct);

        float ch = font_height(freg) + 4.0f;
        float cw = font_width(freg, chip) + 14.0f;
        if (cw < 98.0f) cw = 98.0f;
        gfx_rrect_aa(&s, 6, 6, cw, ch, 4, C_PANEL, C_ACCENT, 1.0f);
        font_draw(&s, freg, 13, 8, chip, C_TEXT);

        int bar_h = 3;
        int bar_y = M.fh - bar_h;
        if (bar_y >= 0 && M.fw > 0) {
            surf_fill(&s, 0, bar_y, M.fw, bar_h, RGB(0x10, 0x16, 0x20));
            int fill = (int)((long)M.fw * M.rows_done / M.fh);
            if (fill > 0) surf_fill(&s, 0, bar_y, fill, bar_h, C_ACCENT);
        }
    }

    /* footer */
    int fy = M.footer_y;
    surf_fill(&s, 0, fy, w, h - fy, C_PANEL);
    surf_fill(&s, 0, fy, w, 1, C_RULE);

    char line[128];
    char tim[32];

    if (M.have_frame) snprintf(tim, sizeof(tim), "%.2f ms", M.last_ms);
    else              snprintf(tim, sizeof(tim), "--");

    snprintf(line, sizeof(line), "re %+.9f   im %+.9f   zoom %.2fx",
             M.cre, M.cim, BASE_SPAN / M.span);
    font_draw(&s, fmed, 8, (float)(fy + FOOT_L1), line, C_TEXT);

    snprintf(line, sizeof(line), "iter cap %d   last frame %s   %d%%",
             MAX_ITER, tim, pct);
    font_draw(&s, freg, 8, (float)(fy + FOOT_L2), line, C_DIM);

    snprintf(line, sizeof(line), "click zoom in   right-click zoom out   r reset   q quit");
    font_draw(&s, freg, 8, (float)(fy + FOOT_L3), line, C_DIM);
}

/* ------------------------------------------------------------ main */

int main(void) {
    MClient c;
    if (mc_open(&c, "mandel", "Mandelbrot", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "mandel: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    font_init("/usr/share/fonts");
    Font *fmed = font_get(FONT_MEDIUM, 10);
    Font *freg = font_get(FONT_REGULAR, 10);
    if (!fmed || !freg) {
        fprintf(stderr, "mandel: no usable fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    palette_build();

    /* the extent that was actually mapped: a RESIZE may shrink the
     * area we draw into but it can never grow past this */
    M.cap_w = c.w;
    M.cap_h = c.h;
    M.w = c.w;
    M.h = c.h;
    layout();
    reset_view();
    render_start();

    int quit = 0;
    while (!quit) {
        /* Drain input first so a click is never stuck behind a band. */
        MEvent ev;
        while (mc_poll(&c, &ev)) {
            if (ev.type == MC_CLOSE) { quit = 1; break; }

            if (ev.type == MC_KEY) {
                if (!ev.press) continue;
                if (ev.key == KEYC_Q) quit = 1;
                else if (ev.key == KEYC_R) { reset_view(); render_start(); }
                continue;
            }
            if (ev.type == MC_TEXT) {
                for (const char *p = ev.text; *p && !quit; p++) {
                    if (*p == 'q' || *p == 'Q') quit = 1;
                    else if (*p == 'r' || *p == 'R') { reset_view(); render_start(); }
                }
                continue;
            }
            if (ev.type == MC_MOUSE) {
                handle_mouse(ev.x, ev.y, ev.buttons);
                continue;
            }
            if (ev.type == MC_RESIZE) {
                int nw = ev.w < M.cap_w ? ev.w : M.cap_w;
                int nh = ev.h < M.cap_h ? ev.h : M.cap_h;
                if (nw < 1) nw = 1;
                if (nh < 1) nh = 1;
                if (nw != M.w || nh != M.h) {
                    M.w = nw;
                    M.h = nh;
                    /* nothing reallocated: the buffer is fixed size, and
                     * the aspect ratio has moved, so the frame in flight
                     * is wrong and gets rendered again at the new size */
                    layout();
                    render_start();
                }
                continue;
            }
        }
        if (quit) break;

        /* One band, then straight back to the top of the loop: that is
         * what keeps the window answering clicks while it fills in. */
        if (M.rendering && M.fw > 0 && M.fh > 0) {
            int y0 = M.rows_done;
            int y1 = y0 + BAND_ROWS;
            if (y1 > M.fh) y1 = M.fh;
            if (y1 < y0) y1 = y0;

            if (y1 > y0) render_rows(y0, y1);
            M.rows_done = y1;
            if (y1 >= M.fh) render_finish();

            draw(&c, fmed, freg);
            mc_present(&c);
            continue;
        }

        if (M.rendering) {
            /* a window too short to hold a picture: stop pretending */
            render_finish();
            draw(&c, fmed, freg);
            mc_present(&c);
            continue;
        }

        /* nothing to compute: sleep until the server has something.
         * The timeout is only so a half-open connection cannot hang us. */
        struct pollfd p = { c.fd, POLLIN, 0 };
        int r = poll(&p, 1, 500);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    mc_close(&c);
    return 0;
}
