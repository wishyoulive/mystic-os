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
 *      n counts up to max_iter; |z| > 2 means "escaped"
 *
 *  A pixel that never escapes is taken to be inside the set and is
 *  painted near-black. Everything else is coloured by how long it
 *  took to escape. The raw count is an integer, which would give
 *  visible stripes, so the fractional part is recovered from the
 *  last |z| with the usual smoothing identity
 *
 *      nu = n + 1 - log2(log2(|z|))
 *
 *  ------------------------------------------------------------ why it
 *  used to stop, and what replaced it
 *
 *  A plain double holds 53 bits. Near the set (|c| < 2) the spacing
 *  of representable doubles is 2^-52 * |c|, i.e. about 1e-16. Once a
 *  pixel is narrower than that, neighbouring pixels round to the same
 *  c, every one of them iterates the same orbit, and the picture turns
 *  into flat blocks - not because the maths stopped, but because the
 *  *input* stopped being representable. The old version papered over
 *  this with a hard zoom floor (span >= 0.00127, about 2000x) so the
 *  wall was never reached. That floor is gone.
 *
 *  Instead the view is carried as double-double: an unevaluated sum
 *
 *      x = hi + lo ,   |lo| <= ulp(hi)/2
 *
 *  which holds 31-32 significant decimal digits (two doubles). The
 *  building blocks are the classic error-free transformations
 *
 *      two_sum  (Knuth)    a + b = s + e   exactly
 *      two_prod (Dekker)   a * b = p + e   exactly
 *
 *  and dd_add / dd_mul / dd_scale renormalise so the invariant above
 *  holds again. All of it is plain IEEE double arithmetic - no libm
 *  call, no fma dependency, no __int128 - so it behaves identically on
 *  the musl/x86-64 target. A complex step costs ~40 flops.
 *
 *  Where the two representations are used:
 *
 *    view centre     dd, always. Panning and zooming therefore never
 *                    drop what has already been resolved: a zoom
 *                    multiplies only the *span* (a plain double, a
 *                    purely relative quantity, so its 1e-16 relative
 *                    error is a scale error and not a position error)
 *                    while the centre moves by dd_add(dd, dd).
 *
 *    pixel c         dd: c = centre + offset, via dd_add. The offset is
 *                    dd too (pixel size times an exact pixel index), so
 *                    the grid keeps 32 digits even though the centre is
 *                    ~0.7: that is what lets a zoom about a pixel keep
 *                    that pixel fixed instead of creeping by 1e-16 of a
 *                    window per notch.
 *
 *    iteration       two paths. Above DD_SWITCH (span 4e-11, where a
 *                    pixel is 1e-13 wide and a double still resolves
 *                    1000 of them) the fast plain-double loop runs.
 *                    Below it, z <- z^2 + c runs with z and c as
 *                    hi+lo, giving ~1e-32 absolute resolution at
 *                    |c| ~ 1 - sixteen more orders of magnitude than
 *                    a double. The dd path is per pixel: there is no
 *                    reference orbit, so there are no glitches and no
 *                    rebasing to get wrong, and it is actually fewer
 *                    operations than perturbation (two dd multiplies
 *                    per step instead of three).
 *
 *  The remaining limit is arithmetic, not zoom: two doubles hold 32
 *  digits, so the app stops at span = 1e-28 (about 2.6e28x) where a
 *  pixel is still ~20 times wider than the last digit dd can hold.
 *  Asking for more is refused and the footer says "precision limit"
 *  instead of quietly painting blocks. More limbs would go further
 *  (three doubles = 48 digits) with the same dd_add/dd_mul shape.
 *
 *  ------------------------------------------------------------ detail
 *  Iterations grow with depth, which is what makes new filaments keep
 *  appearing instead of the deep image going flat black:
 *
 *      L = log10(zoom)
 *      max_iter = 200 + 60*L + 25*L*L,   clamped to 200 .. 20000
 *
 *  Every factor of ten in magnification buys 60 more iterations, and the
 *  quadratic term buys more where the measured need is not linear: at a
 *  boundary point in Seahorse Valley the escape counts are ~1.2k at 1e-10
 *  and ~3k at 1e-14, so a 60*L line alone (825 / 1065 there) paints those
 *  frames black. 60*L + 25*L^2 gives 3.5k / 6.2k at the same depths, and
 *  it reaches the 20000 cap at the arithmetic floor instead of four
 *  hundred decades past it. The old fixed cap of 64 is why 2000x already
 *  looked mushy.
 *
 *  Because the cap is no longer fixed, the colour address cannot be
 *  either: the old sqrt(nu)*32 ramp assumed nu <= 64 and would saturate
 *  to one flat colour for every deep pixel. The ramp is now
 *
 *      address = log2(1 + nu) * 42.5,  mirrored every 256 entries
 *
 *  which is scale-free: it puts the base view's nu 0..63 across the
 *  whole palette exactly as before, keeps cycling (a colour cycle per
 *  64x of iteration count) as the cap grows, and the mirror makes the
 *  seam at the wrap invisible.
 *
 *  ------------------------------------------------------------ drawing
 *  Rendering is two passes so the window is never frozen:
 *
 *    1. coarse - one sample per 4x4 block, replicated over the block.
 *                1/16 of the work, so a deep view appears immediately.
 *    2. fine   - every pixel, a band of rows per loop turn, overwriting
 *                the coarse blocks from the top down. The band height
 *                comes from the work a row costs (~4M pixel-iterations,
 *                ~0.1s deep) and bottoms out at one row, so the worst
 *                case is 420 px x 20000 iter x 24.5ns = 0.21s: that is
 *                what a turn of the renderer can make input wait.
 *
 *  Input is drained between bands, so a click during a long deep render
 *  is never stuck behind it, and when nothing is being rendered the app
 *  sleeps in poll() - no busy loop. Only a view change starts a frame,
 *  so an idle window costs nothing.
 *
 *  State is fixed size: one 512x384 pixel buffer and a handful of
 *  doubles, so nothing is allocated while running. The pixels the server
 *  mapped can grow (MClient.maxw/maxh - it allocates a whole screen and
 *  sends the maxima), so an MC_RESIZE can make the window bigger; the
 *  client area is clamped to that mapping and to the fixed buffer, and
 *  only the footer and the picture split of it are recomputed.
 *
 *  The footer is derived from the loaded fonts (metrics()), not from a
 *  number picked for a 10px font: font_get() applies the desktop text
 *  scale, so the line box is 19.6px at scale 1.0 and 31.4px at the 1.6
 *  Settings allows. The footer keeps 3 lines while they leave 32px of
 *  picture, then 2, then 1, then none, and every line is built from a
 *  ladder of shorter forms so nothing is ever clipped at the edge.
 *
 *  The clock is real: clock_gettime(CLOCK_MONOTONIC) around the whole
 *  render (coarse + fine), and the footer shows that duration for the
 *  last frame that actually completed - not an estimate.
 *
 *  Keys:  + / =  zoom in (1.25x)      - / _  zoom out (1.25x)
 *         (keypad +/- are the same keys: they arrive as text, and the
 *         evdev code is deliberately not handled a second time)
 *         arrows pan,  r reset,  q quit
 *  Mouse: wheel zooms about the pointer (smooth, 1.25x per notch),
 *         left click centres on the point and zooms in 2x,
 *         right click zooms out 2x keeping the point under the pointer.
 *         Clicks on the footer are ignored: it is a readout, not a
 *         control.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIN_W 420
#define WIN_H 320

/* rows of the fine picture computed per loop turn at shallow depth */
#define BAND_ROWS 16
/* sample rows of the coarse pass computed per loop turn (4 px each) */
#define COARSE_BAND 8
/* the coarse pass samples one pixel per COARSE_STEP x COARSE_STEP block */
#define COARSE_STEP 4

/* the persistent render buffer. The window is 420x320, so this has
 * headroom for any resize the server can legitimately ask for. */
#define IMG_MAX_W 512
#define IMG_MAX_H 384

/* Footer: the height and the line tops are derived from the fonts that
 * are actually loaded (metrics() below), not from a number picked for a
 * 10px font. font_get(face, 10) is multiplied by the desktop text scale,
 * so the regular line box is 19.6px at scale 1.0 and 31.4px at the 1.6
 * Settings allows; a fixed 46px footer with 14px steps overlapped those
 * lines and pushed the third one out of a 320px window. Three lines are
 * kept while they leave MIN_PIC_H of picture, then two, then one, then
 * none, so the window stays usable at any height and any text scale. */
#define MIN_PIC_H 32
#define FOOT_MAX_LINES 3

/* The starting view: centred a little left of the origin so the
 * whole set sits comfortably, 2.6 imaginary units tall. 420x274 of
 * aspect 1.53 then spans about 4.0 units horizontally. */
#define BASE_CRE  (-0.6)
#define BASE_CIM  (0.0)
#define BASE_SPAN (2.6)

/* Where the iteration switches from plain double to double-double.
 * A 420-pixel-wide frame of this span has 1e-13 per pixel: a double
 * still resolves 1000 distinct values inside one pixel there, and below
 * it that margin is gone. */
#define DD_SWITCH (4.0e-11)

/* The floor is the arithmetic's, not a chosen zoom limit: dd holds 32
 * digits, so at span 1e-28 a pixel is 2.4e-31 wide and the last digit
 * dd can represent is 1.2e-32 - still ~20 counts of headroom per pixel.
 * Pushing past that would start drawing quantised blocks, so the app
 * refuses and says so. */
#define MIN_SPAN (1.0e-28)

/* 13 units out is 0.2x: enough to see the set in context without it
 * shrinking to a dot. */
#define MAX_SPAN (13.0)

/* Iteration schedule and the escape-count colour slope.
 * 200 + 60*L + 25*L^2, L = log10(zoom); see the measurement note in the
 * file header for why the linear term alone is not enough. */
#define ITER_BASE  200.0
#define ITER_SLOPE  60.0
#define ITER_CURVE  25.0
#define ITER_CAP 20000.0
#define SHADE_SLOPE 42.5

/* evdev keycodes, the same ones the other client apps use. The keypad's
 * +/- are deliberately absent: they arrive as "+"/"-" text too, and
 * handling both would zoom twice per press (see handle_key). */
#define KEY_UP       103
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_DOWN     108

#define TAU 6.283185307179586

/* ------------------------------------------------------------ colours */
static const u32 C_VOID   = RGB(0x08, 0x0A, 0x10);   /* not yet rendered */
static const u32 C_PANEL  = RGB(0x0B, 0x10, 0x18);   /* footer           */
static const u32 C_RULE   = RGB(0x1E, 0x27, 0x36);   /* footer hairline  */
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_WARN   = RGB(0xE8, 0xB3, 0x4F);
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

/* ------------------------------------------------------------ dd math
 *
 * A DD is hi+lo with |lo| <= ulp(hi)/2. two_sum and two_prod are exact
 * in IEEE double, so nothing below ever needs a wider type.
 */
typedef struct { double hi, lo; } DD;

/* Knuth: a + b = s + e exactly, for any a,b. */
static inline void two_sum(double a, double b, double *s, double *e) {
    double t = a + b;
    double bb = t - a;
    *s = t;
    *e = (a - (t - bb)) + (b - bb);
}

/* Dekker/Veltkamp split product: a * b = p + e exactly. Splitting with
 * 2^27+1 keeps both halves inside 26 bits, so their products are exact. */
static inline void two_prod(double a, double b, double *p, double *e) {
    const double SPLIT = 134217729.0;             /* 2^27 + 1 */
    double t = a * b;
    double ca = SPLIT * a, cb = SPLIT * b;
    double ah = ca - (ca - a), al = a - ah;
    double bh = cb - (cb - b), bl = b - bh;
    *p = t;
    *e = ((ah * bh - t) + ah * bl + al * bh) + al * bl;
}

/* s + e with |s| >= |e|: the cheap renormalisation. */
static inline DD dd_pack(double s, double e) {
    DD r;
    r.hi = s + e;
    r.lo = e - (r.hi - s);
    return r;
}

static inline DD dd_make(double v) {
    DD r;
    r.hi = v;
    r.lo = 0.0;
    return r;
}

static inline DD dd_add(DD a, DD b) {
    double s, e;
    two_sum(a.hi, b.hi, &s, &e);
    e += a.lo + b.lo;
    return dd_pack(s, e);
}

static inline DD dd_sub(DD a, DD b) {
    DD nb;
    nb.hi = -b.hi;
    nb.lo = -b.lo;
    return dd_add(a, nb);
}

static inline DD dd_mul(DD a, DD b) {
    double p, e;
    two_prod(a.hi, b.hi, &p, &e);
    e += a.hi * b.lo + a.lo * b.hi;
    return dd_pack(p, e);
}

/* multiply by a plain double: one product instead of three */
static inline DD dd_scale(DD a, double k) {
    double p, e;
    two_prod(a.hi, k, &p, &e);
    e += a.lo * k;
    return dd_pack(p, e);
}

/* ------------------------------------------------------------ state */
typedef struct {
    int w, h;              /* client area, never past the mapping      */
    int maxw, maxh;        /* MClient.maxw/maxh: the mapping can grow  */
    int fw, fh;            /* the picture rect, at 0,0                 */
    int footer_y;

    /* footer metrics, all derived from the loaded fonts in metrics() */
    int line_h, pad, gutter;   /* line box, vertical air, side margin  */
    int flines;            /* footer lines that fit: 0 .. 3            */
    int foot_h;
    int line_y[FOOT_MAX_LINES];

    DD     cre, cim;       /* view centre, 32 digits                   */
    double span;           /* vertical extent in imaginary units       */
    int    max_iter;       /* for this view: 200 .. 20000              */
    int    deep;           /* this frame uses the dd iteration         */
    int    at_limit;       /* last zoom was refused by the dd floor    */

    u32    img[IMG_MAX_W * IMG_MAX_H];

    int    pass;           /* 0 = coarse, 1 = fine, 2 = idle           */
    int    rows_done;      /* coarse sample rows / fine rows done      */
    int    samples_y;      /* coarse sample rows in total              */
    int    rendering;      /* a frame is part-way through              */
    int    have_frame;     /* at least one frame has completed         */
    int    dirty;          /* state changed but no new frame to render */
    struct timespec t0;    /* start of the frame in progress           */
    double last_ms;        /* duration of the last completed frame     */

    int    prev_buttons;   /* so a held button zooms once, not per px  */
    int    mouse_x, mouse_y;   /* last pointer position in the window  */
    int    have_mouse;
} Mandel;

/* one window, so one state block, all of it fixed size */
static Mandel M;

/* ------------------------------------------------------------ algorithm
 * max_iter from the view width: 200 at 1x, +60 per decade of zoom, plus
 * 25*L^2 so the deep frames where the escape counts run to thousands get
 * the budget they need; never more than 20000, never fewer than 200. */
static int iter_for_span(double span) {
    double zoom = BASE_SPAN / span;
    double L, it;
    if (!(zoom >= 1.0)) zoom = 1.0;
    L = log10(zoom);
    it = ITER_BASE + ITER_SLOPE * L + ITER_CURVE * L * L;
    if (it > ITER_CAP) it = ITER_CAP;
    if (it < ITER_BASE) it = ITER_BASE;
    return (int)(it + 0.5);
}

/* The plain-double iteration, used while a double still resolves a
 * pixel. Returns the smoothed escape count, or a negative number when
 * the point stayed bounded (i.e. it is inside the set). */
static double escape_d(double cr, double ci, int max_iter) {
    double zr = 0.0, zi = 0.0;
    int n;

    for (n = 0; n < max_iter; n++) {
        double zr2 = zr * zr;
        double zi2 = zi * zi;
        if (zr2 + zi2 > 4.0) break;          /* |z| > 2: escaped */
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
    }
    if (n >= max_iter) return -1.0;

    /* zr,zi are the pair that broke out, so |z| > 2 and log2(|z|) > 1.
     * log2(log2(|z|)) == log2(0.5 * log2(|z|^2)). */
    double mag2 = zr * zr + zi * zi;
    double lg = 0.5 * log2(mag2);
    return (double)n + 1.0 - log2(lg);
}

/* The same iteration carried as hi+lo, so c's 32nd digit survives into
 * z instead of being rounded away at the first addition. z^2 is formed
 * as (zr+zi)(zr-zi) and 2*zr*zi: two dd multiplies per step. The escape
 * test reads the hi parts only, which is a 1e-32 relative readout of a
 * threshold, i.e. exact for any pixel that is not within 1e-32 of the
 * boundary circle. */
static double escape_dd(DD cr, DD ci, int max_iter) {
    DD zr = dd_make(0.0), zi = dd_make(0.0);
    int n;

    for (n = 0; n < max_iter; n++) {
        if (zr.hi * zr.hi + zi.hi * zi.hi > 4.0) break;
        DD sum = dd_add(zr, zi);
        DD dif = dd_sub(zr, zi);
        DD re2 = dd_mul(sum, dif);           /* zr*zr - zi*zi */
        DD im2 = dd_mul(zr, zi);
        im2 = dd_add(im2, im2);              /* 2*zr*zi       */
        zr = dd_add(re2, cr);
        zi = dd_add(im2, ci);
    }
    if (n >= max_iter) return -1.0;

    double mag2 = zr.hi * zr.hi + zi.hi * zi.hi;
    double lg = 0.5 * log2(mag2);
    return (double)n + 1.0 - log2(lg);
}

/* nu -> colour. log2(1+nu) is scale free, so the colours do not shift
 * when max_iter changes; times SHADE_SLOPE it walks the 256 entries for
 * nu 0..63 and then keeps cycling, mirrored so the wrap has no seam. */
static u32 shade(double nu) {
    if (nu < 0.0) return C_INSIDE;

    double f = log2(1.0 + nu) * SHADE_SLOPE;
    f = fmod(f, (double)(2 * PAL_N));
    if (f < 0.0) f += (double)(2 * PAL_N);
    if (f > (double)(PAL_N - 1)) f = (double)(2 * PAL_N - 1) - f;

    int i0 = (int)f;
    if (i0 < 0) i0 = 0;
    if (i0 > PAL_N - 1) i0 = PAL_N - 1;
    int i1 = (i0 < PAL_N - 1) ? i0 + 1 : PAL_N - 1;
    double fr = f - (double)i0;
    if (fr < 0.0) fr = 0.0;
    if (fr > 1.0) fr = 1.0;
    return color_mix(PAL[i0], PAL[i1], (float)fr);
}

/* one pixel's iteration: whichever path this frame is using */
static u32 pixel_colour(DD cr, DD ci) {
    double nu = M.deep ? escape_dd(cr, ci, M.max_iter)
                       : escape_d(cr.hi, ci.hi, M.max_iter);
    return shade(nu);
}

/* ------------------------------------------------------------ geometry
 * Size of one pixel, as hi+lo. Carrying the *pixel size* in dd and not
 * just the centre is what makes zooming reversible: the anchor maths in
 * zoom_step() is exact only if the offset of pixel k scales by exactly
 * the ratio the centre is scaled by. With a double pixel size that
 * ratio carries a 1-ulp error, so a point being zoomed into creeps by
 * ~1e-16 of a window per notch and is thousands of pixels off after a
 * few hundred notches. In dd the same creep is ~1e-32. The 1/fw factor
 * is a constant, so its own rounding is a uniform scale error that
 * cancels in the ratio - the pixel grid may be a hair wider than the
 * window, but it does not drift. */
static void pixel_size(DD *dx, DD *dy) {
    double aspect = (double)M.fw / (double)M.fh;
    *dx = dd_scale(dd_make(M.span * aspect), 1.0 / (double)M.fw);
    *dy = dd_scale(dd_make(M.span), 1.0 / (double)M.fh);
}

/* The complex point at the centre of pixel (x,y), as hi+lo. */
static void pixel_point(int x, int y, DD *re, DD *im) {
    DD dx, dy;
    pixel_size(&dx, &dy);
    *re = dd_add(M.cre, dd_scale(dx, (double)x + 0.5 - 0.5 * (double)M.fw));
    *im = dd_add(M.cim, dd_scale(dy, 0.5 * (double)M.fh - ((double)y + 0.5)));
}

/* A safety net, not a pan limit: the base view spans re [-2.588, 1.388]
 * and im [-1.3, 1.3], so this box contains every pixel of it: a click
 * anywhere in the starting picture is honoured exactly, and the box only
 * ever bends the centre after the user has zoomed out past the starting
 * framing, where it stops the view sailing off into empty plane. */
static void clamp_centre(void) {
    if (M.cre.hi < -2.7) M.cre = dd_make(-2.7);
    if (M.cre.hi >  1.7) M.cre = dd_make( 1.7);
    if (M.cim.hi < -1.7) M.cim = dd_make(-1.7);
    if (M.cim.hi >  1.7) M.cim = dd_make( 1.7);
}

/* The fonts are owned by main() and are the only truth about how tall a
 * line is: font_get() applies the desktop text scale, so asking for a
 * nominal 10 can hand back a 22.4px font. layout() runs on every resize
 * and needs the same answer, hence these two pointers. */
static Font *g_fmed;
static Font *g_freg;

/* Footer metrics from the real fonts. line_h is one line box, ceil'd so
 * two consecutive lines can never touch; the number of lines then falls
 * back 3 -> 2 -> 1 -> 0 until MIN_PIC_H of picture is left, which keeps
 * every glyph inside the window instead of drawing a line at y = h. */
static void metrics(void) {
    float reg_h = g_freg ? font_height(g_freg) : 14.0f;
    if (reg_h < 6.0f) reg_h = 6.0f;
    M.line_h = (int)(reg_h + 0.999f);
    M.pad = M.line_h / 4;
    if (M.pad < 2) M.pad = 2;
    M.gutter = ui_px(6);
    if (M.gutter < 3) M.gutter = 3;

    M.flines = 0;
    for (int n = FOOT_MAX_LINES; n >= 1; n--) {
        int need = n * M.line_h + 2 * M.pad;
        if (M.h - need >= MIN_PIC_H) { M.flines = n; break; }
    }
    M.foot_h = M.flines ? M.flines * M.line_h + 2 * M.pad : 0;
    if (M.foot_h >= M.h) {          /* nothing left to draw a picture in */
        M.flines = 0;
        M.foot_h = 0;
    }
    for (int i = 0; i < FOOT_MAX_LINES; i++)
        M.line_y[i] = M.footer_y + M.pad + i * M.line_h;
}

/* Clamp the client area to what can actually be drawn: the mapping that
 * mc_open() was given (MClient.maxw/maxh - the server allocates the whole
 * screen, so a RESIZE may grow the client), and this app's own fixed
 * 512x384 picture buffer. */
static void layout(void) {
    if (M.w > M.maxw) M.w = M.maxw;
    if (M.h > M.maxh) M.h = M.maxh;
    if (M.w > IMG_MAX_W) M.w = IMG_MAX_W;
    if (M.w < 1) M.w = 1;
    if (M.h < 1) M.h = 1;

    M.footer_y = M.h;
    metrics();                       /* foot_h for this height / scale */
    if (M.h > IMG_MAX_H + M.foot_h) M.h = IMG_MAX_H + M.foot_h;
    if (M.h < 1) M.h = 1;
    metrics();                       /* h may have just been clamped    */

    M.footer_y = M.h - M.foot_h;
    if (M.footer_y < 0) M.footer_y = 0;
    M.fw = M.w;
    M.fh = M.footer_y;               /* the picture starts at 0,0       */
    if (M.fh > IMG_MAX_H) M.fh = IMG_MAX_H;
    if (M.fh < 0) M.fh = 0;

    M.samples_y = (M.fh + COARSE_STEP - 1) / COARSE_STEP;
    if (M.samples_y < 0) M.samples_y = 0;
    /* the line tops depend on footer_y, so they are computed last */
    for (int i = 0; i < FOOT_MAX_LINES; i++)
        M.line_y[i] = M.footer_y + M.pad + i * M.line_h;
}

static void reset_view(void) {
    M.cre = dd_make(BASE_CRE);
    M.cim = dd_make(BASE_CIM);
    M.span = BASE_SPAN;
    M.at_limit = 0;
}

/* Throw the current frame away and start the clock on a new one. The
 * iteration budget and the arithmetic path are decided once per view,
 * so a frame is always consistent with itself. */
static void render_start(void) {
    M.max_iter = iter_for_span(M.span);
    M.deep = (M.span < DD_SWITCH) ? 1 : 0;
    M.pass = 0;
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
    M.pass = 2;
    M.have_frame = 1;
}

/* ------------------------------------------------------------ zoom
 * Zooming scales the span and moves the centre in dd. When the factor
 * is applied about a pixel, that pixel's point stays put:
 *
 *      centre' = p + (centre - p) * factor
 *
 * which is one dd_sub, one dd_scale and one dd_add - so the anchor keeps
 * all 32 digits too, and repeated zooms about the same point converge
 * instead of drifting. */
static void zoom_step(int px, int py, double factor) {
    if (M.fw <= 0 || M.fh <= 0 || !(factor > 0.0)) return;

    double nspan = M.span * factor;
    int limited = 0;
    if (nspan < MIN_SPAN) { nspan = MIN_SPAN; limited = 1; }
    if (nspan > MAX_SPAN) nspan = MAX_SPAN;

    if (limited && M.span <= MIN_SPAN) {   /* already at the floor */
        M.at_limit = 1;
        M.dirty = 1;                       /* the footer has news    */
        return;
    }
    double f = nspan / M.span;
    if (f == 1.0) return;

    if (px < 0) px = 0;
    if (px > M.fw - 1) px = M.fw - 1;
    if (py < 0) py = 0;
    if (py > M.fh - 1) py = M.fh - 1;

    DD pr, pi;
    pixel_point(px, py, &pr, &pi);
    M.cre = dd_add(pr, dd_scale(dd_sub(M.cre, pr), f));
    M.cim = dd_add(pi, dd_scale(dd_sub(M.cim, pi), f));
    M.span = nspan;
    M.at_limit = limited;
    clamp_centre();
    render_start();
}

/* Left click: put the clicked point in the middle and zoom in 2x. */
static void click_centre(int px, int py) {
    if (M.fw <= 0 || M.fh <= 0) return;
    if (px < 0) px = 0;
    if (px > M.fw - 1) px = M.fw - 1;
    if (py < 0) py = 0;
    if (py > M.fh - 1) py = M.fh - 1;

    DD pr, pi;
    pixel_point(px, py, &pr, &pi);
    M.cre = pr;
    M.cim = pi;

    double nspan = M.span * 0.5;
    if (nspan < MIN_SPAN) { nspan = MIN_SPAN; M.at_limit = 1; }
    else M.at_limit = 0;
    if (nspan > MAX_SPAN) nspan = MAX_SPAN;
    M.span = nspan;
    clamp_centre();
    render_start();
}

/* Pan by a fraction of the frame, in dd, so the centre keeps its low
 * bits when the step itself is 1e-29 wide. */
static void pan_step(int dx, int dy) {
    if (M.fw <= 0 || M.fh <= 0 || (dx == 0 && dy == 0)) return;
    double aspect = (double)M.fw / (double)M.fh;
    if (dx) M.cre = dd_add(M.cre, dd_make((double)dx * 0.12 * M.span * aspect));
    if (dy) M.cim = dd_add(M.cim, dd_make((double)-dy * 0.12 * M.span));
    M.at_limit = 0;      /* the limit notice belongs to the zoom, not the view */
    clamp_centre();
    render_start();
}

/* The wheel and the +/- keys zoom about the pointer when it is over the
 * picture, and about the centre otherwise - a pointer resting on the
 * footer readouts must not turn a wheel notch into a zoom anchored in the
 * last picture row. */
static void zoom_anchor(double factor) {
    int px = M.fw / 2, py = M.fh / 2;
    if (M.have_mouse && M.mouse_y >= 0 && M.mouse_y < M.fh &&
        M.mouse_x >= 0 && M.mouse_x < M.fw) {
        px = M.mouse_x;
        py = M.mouse_y;
    }
    zoom_step(px, py, factor);
}

/* ------------------------------------------------------------ render */
static void render_fine_row(int y) {
    DD dx, dy;
    pixel_size(&dx, &dy);

    DD ci = dd_add(M.cim, dd_scale(dy, 0.5 * (double)M.fh - ((double)y + 0.5)));
    u32 *row = M.img + (size_t)y * IMG_MAX_W;

    for (int x = 0; x < M.fw; x++) {
        DD cr = dd_add(M.cre, dd_scale(dx, (double)x + 0.5 - 0.5 * (double)M.fw));
        row[x] = pixel_colour(cr, ci);
    }
}

/* One coarse sample row: iterate the middle pixel of each 4x4 block and
 * replicate it over the block. Cheap enough that a deep view appears at
 * once; the fine pass then replaces it from the top down. */
static void render_coarse_row(int sy) {
    int ys = sy * COARSE_STEP;
    int ye = ys + COARSE_STEP;
    if (ys >= M.fh) return;
    if (ye > M.fh) ye = M.fh;
    int yc = ys + (ye - ys) / 2;

    for (int xs = 0; xs < M.fw; xs += COARSE_STEP) {
        int xe = xs + COARSE_STEP;
        if (xe > M.fw) xe = M.fw;
        int xc = xs + (xe - xs) / 2;

        DD cr, ci;
        pixel_point(xc, yc, &cr, &ci);
        u32 col = pixel_colour(cr, ci);

        for (int y = ys; y < ye; y++) {
            u32 *row = M.img + (size_t)y * IMG_MAX_W;
            for (int x = xs; x < xe; x++) row[x] = col;
        }
    }
}

/* One turn of the renderer: a coarse band, or a fine band, or the end. */
static void render_band(void) {
    if (M.fw <= 0 || M.fh <= 0) { render_finish(); return; }

    if (M.pass == 0) {
        /* The coarse pass has its own work unit - one iteration per 4x4
         * block - but the same budget, or at the 20000-iteration cap a
         * single coarse band is ~0.6s of input latency on its own. */
        long samples_x = (M.fw + COARSE_STEP - 1) / COARSE_STEP;
        long cwork = samples_x * (long)M.max_iter;
        int cband = (cwork > 0) ? (int)(4000000L / cwork) : COARSE_BAND;
        if (cband < 1) cband = 1;
        if (cband > COARSE_BAND) cband = COARSE_BAND;

        int sy = M.rows_done;
        int sy1 = sy + cband;
        if (sy1 > M.samples_y) sy1 = M.samples_y;
        for (; sy < sy1; sy++) render_coarse_row(sy);
        M.rows_done = sy;
        if (sy >= M.samples_y) {              /* coarse done: refine */
            M.pass = 1;
            M.rows_done = 0;
        }
        return;
    }

    int y = M.rows_done;
    /* A band is what input waits behind, so its height comes from the work
     * a row actually costs. Measured on this target: 24.5ns per dd
     * iteration, 2.5ns per double iteration. At the 20000-iteration cap one
     * row is already 420 * 20000 * 24.5ns = 0.21s, which is the finest
     * granularity there is; below that the band is sized to ~4M
     * pixel-iterations (~0.1s deep, ~0.01s shallow) and never more than
     * BAND_ROWS. Worst case therefore: 0.21s of input latency, one row. */
    long row_work = (long)M.fw * (long)M.max_iter;
    int band = (row_work > 0) ? (int)(4000000L / row_work) : BAND_ROWS;
    if (band < 1) band = 1;
    if (band > BAND_ROWS) band = BAND_ROWS;
    int y1 = y + band;
    if (y1 > M.fh) y1 = M.fh;
    for (; y < y1; y++) render_fine_row(y);
    M.rows_done = y;
    if (y >= M.fh) render_finish();
}

/* ------------------------------------------------------------ drawing */
/* "hi" plus the residual in exponent form. The residual only exists
 * once the view is deep, so a shallow view prints two clean numbers and
 * a deep one prints exactly the digits the dd path is using. */
static void coord_text(char *buf, size_t n, DD v, int with_lo) {
    if (with_lo && v.lo != 0.0) snprintf(buf, n, "%.16g %+.1e", v.hi, v.lo);
    else                        snprintf(buf, n, "%.16g", v.hi);
}

/* Draw text clipped to xmax, with ".." if it had to be cut. Every string
 * that reaches the surface goes through this or through footer_line(),
 * so a wider face or a narrower window can never paint half a glyph
 * against the edge. */
static void text_fit(Surface *s, Font *f, float x, float y, float xmax,
                     const char *t, u32 col) {
    if (!t || !*t || !f) return;
    if (x + font_width(f, t) <= xmax) { font_draw(s, f, x, y, t, col); return; }

    char tmp[192];
    size_t len = strlen(t);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, t, len);
    tmp[len] = 0;
    while (len > 0 && x + font_width(f, tmp) > xmax) tmp[--len] = 0;
    if (len >= 2) { tmp[len - 2] = '.'; tmp[len - 1] = '.'; }
    font_draw(s, f, x, y, tmp, col);
}

/* The time column: ms while a frame is quick, seconds once it is not, so
 * the longest number the footer has to show stays short. */
static void time_text(char *buf, size_t n) {
    if (!M.have_frame)             snprintf(buf, n, "--");
    else if (M.last_ms >= 10000.0) snprintf(buf, n, "%.1f s", M.last_ms / 1000.0);
    else                           snprintf(buf, n, "%.1f ms", M.last_ms);
}

/* Footer line `i`: 0 = the centre, 1 = depth / iterations / arithmetic /
 * frame time, 2 = the controls. Each is built from a ladder of shorter
 * variants and handed back as soon as one measures inside `avail`, which
 * is the real fix for "the last character is clipped at the floor state":
 * at ui scale 1.6 the regular face is 2.24x the width a nominal 10px font
 * would give, so the long sentence does not fit 420px there and something
 * has to go - in this order: the parenthesised depth, then the frame time,
 * then the arithmetic tag, then whole words. */
static const char *footer_line(int i, Font *f, float avail,
                               char *buf, size_t n) {
    char tim[24];
    time_text(tim, sizeof tim);

    double zoom = BASE_SPAN / M.span;
    double depth = log10(zoom);
    const char *prec = M.deep ? "dd" : "dbl";

    if (i == 0) {
        char a[64], b[64];
        for (int v = 0; v < 6; v++) {
            if (v == 0) {
                coord_text(a, sizeof a, M.cre, 1);
                coord_text(b, sizeof b, M.cim, 1);
                snprintf(buf, n, "re %s   im %s", a, b);
            } else if (v == 1) {
                coord_text(a, sizeof a, M.cre, 0);
                coord_text(b, sizeof b, M.cim, 0);
                snprintf(buf, n, "re %s   im %s", a, b);
            } else if (v == 2) {
                snprintf(buf, n, "re %.10g   im %.10g", M.cre.hi, M.cim.hi);
            } else if (v == 3) {
                snprintf(buf, n, "re %.6g  im %.6g", M.cre.hi, M.cim.hi);
            } else if (v == 4) {
                snprintf(buf, n, "%.5g %.5g", M.cre.hi, M.cim.hi);
            } else {
                snprintf(buf, n, "%.4g", M.cre.hi);
            }
            if (font_width(f, buf) <= avail) break;
        }
        return buf;
    }

    if (i == 1) {
        for (int v = 0; v < 6; v++) {
            if (v == 0)
                snprintf(buf, n, "zoom %.3gx   depth 10^%.2f   iter %d   %s   %s",
                         zoom, depth, M.max_iter, prec, tim);
            else if (v == 1)
                snprintf(buf, n, "zoom %.3gx   iter %d   %s   %s",
                         zoom, M.max_iter, prec, tim);
            else if (v == 2)
                snprintf(buf, n, "zoom %.3gx   iter %d   %s", zoom, M.max_iter, prec);
            else if (v == 3)
                snprintf(buf, n, "%.3gx   iter %d   %s", zoom, M.max_iter, prec);
            else if (v == 4)
                snprintf(buf, n, "iter %d  %s", M.max_iter, prec);
            else
                snprintf(buf, n, "iter %d", M.max_iter);
            if (font_width(f, buf) <= avail) break;
        }
        return buf;
    }

    for (int v = 0; v < 7; v++) {
        if (v == 0)
            snprintf(buf, n, "wheel or +/-  zoom   click centre   arrows pan   r reset   q quit");
        else if (v == 1)
            snprintf(buf, n, "wheel/+- zoom   click centre   arrows pan   r reset");
        else if (v == 2)
            snprintf(buf, n, "wheel/+- zoom   click centre   r reset   q quit");
        else if (v == 3)
            snprintf(buf, n, "wheel/+- zoom   r reset   q quit");
        else if (v == 4)
            snprintf(buf, n, "wheel zoom   r reset   q quit");
        else if (v == 5)
            snprintf(buf, n, "r reset   q quit");
        else
            snprintf(buf, n, "q quit");
        if (font_width(f, buf) <= avail) break;
    }
    return buf;
}

/* A chip in the top-left corner of the picture: render progress while a
 * frame is arriving, or the precision-limit notice when a zoom was
 * refused. Both stay inside the picture rect and are width-fitted, so the
 * limit notice cannot be clipped away at any text scale. */
static void draw_chip(Surface *s, Font *f, const char *text, u32 border) {
    if (!f || M.fw < 96 || M.fh < M.line_h) return;

    float tw = font_width(f, text);
    float pad = (float)(M.pad + 2);
    float x = (float)M.gutter / 2.0f;
    float y = (float)M.gutter / 2.0f;
    float cw = tw + 2.0f * pad;
    float ch = font_height(f) + pad;
    float maxw = (float)M.fw - x - 2.0f;
    if (cw > maxw) cw = maxw;
    if (ch > (float)M.fh - y) ch = (float)M.fh - y;
    if (cw < 24.0f || ch < 8.0f) return;

    gfx_rrect_aa(s, x, y, cw, ch, (float)M.pad, C_PANEL, border, 1.0f);
    text_fit(s, f, x + pad, y + (ch - font_height(f)) / 2.0f, x + cw - pad,
             text, C_TEXT);
}

static void draw(MClient *c, Font *fmed, Font *freg) {
    int w = M.w, h = M.h;
    if (w <= 0 || h <= 0) return;

    /* the surface is the client area only, so every gfx_/surf_ call
     * below is clipped to the window - nothing can bleed out of it */
    Surface s = { c->px, w, h, c->stride };

    surf_fill(&s, 0, 0, w, h, C_VOID);

    /* While the coarse pass runs only the sampled rows exist; from the
     * moment it finishes the buffer holds a complete picture (blocky,
     * then refined from the top), so there is never a half-old image. */
    if (M.fw > 0 && M.fh > 0) {
        int rows;
        if (M.pass == 0) {
            long r = (long)M.rows_done * COARSE_STEP;
            rows = (r > M.fh) ? M.fh : (int)r;
        } else if (M.pass == 1 || M.have_frame) {
            rows = M.fh;
        } else {
            rows = 0;
        }
        if (rows > 0) {
            Surface im = { M.img, IMG_MAX_W, IMG_MAX_H, IMG_MAX_W };
            surf_push(&s, 0, 0, &im, 0, 0, M.fw, rows);
        }
    }

    int pct = 100;
    if (M.rendering) {
        if (M.pass == 0) {
            pct = (M.samples_y > 0) ? (M.rows_done * 6 / M.samples_y) : 0;
        } else {
            pct = (M.fh > 0) ? (6 + M.rows_done * 94 / M.fh) : 100;
        }
        if (pct > 100) pct = 100;
    } else if (!M.have_frame) {
        pct = 0;
    }

    if (M.fh > 0) {
        char chip[48];
        if (M.rendering) {
            snprintf(chip, sizeof(chip), "%s %d%%",
                     M.pass == 0 ? "coarse" : "fine", pct);
            draw_chip(&s, freg, chip, C_ACCENT);

            /* the bar is the honest overall (coarse+fine) progress, not
             * just the band that happens to be running */
            int bar_h = ui_px(3);
            int bar_y = M.fh - bar_h;
            if (bar_y >= 0) {
                surf_fill(&s, 0, bar_y, M.fw, bar_h, RGB(0x10, 0x16, 0x20));
                int fill = M.fw * pct / 100;
                if (fill > 0) surf_fill(&s, 0, bar_y, fill, bar_h, C_ACCENT);
            }
        } else if (M.at_limit) {
            draw_chip(&s, freg, "precision limit", C_WARN);
        }
    }

    /* footer: the band and its hairline, then the lines that fit */
    int fy = M.footer_y;
    if (fy < h) surf_fill(&s, 0, fy, w, h - fy, C_PANEL);
    if (fy > 0 && fy < h) surf_fill(&s, 0, fy, w, 1, C_RULE);

    if (M.flines > 0) {
        static const int SLOT3[3] = { 0, 1, 2 };
        static const int SLOT2[2] = { 1, 2 };
        static const int SLOT1[1] = { 1 };
        const int *slot = (M.flines == 3) ? SLOT3 : (M.flines == 2) ? SLOT2 : SLOT1;

        char buf[192];
        float x0 = (float)M.gutter;
        float avail = (float)w - 2.0f * x0;
        if (avail < 16.0f) avail = 16.0f;      /* text_fit truncates below */

        for (int i = 0; i < M.flines; i++) {
            int id = slot[i];
            Font *f = (id == 0) ? fmed : freg;
            const char *t = footer_line(id, f, avail, buf, sizeof buf);
            text_fit(&s, f, x0, (float)M.line_y[i], (float)w - x0, t,
                     (id == 0) ? C_TEXT : C_DIM);
        }
    }
}

/* ------------------------------------------------------------ input */
static void handle_mouse(int x, int y, int buttons) {
    /* a press is a 0 -> 1 edge; the server keeps sending MOUSE while
     * the button is held, and without the edge test a single click
     * would zoom once per pixel of held-button motion */
    int press_l = (buttons & 1) && !(M.prev_buttons & 1);
    int press_r = (buttons & 2) && !(M.prev_buttons & 2);
    M.prev_buttons = buttons;

    M.mouse_x = x;
    M.mouse_y = y;
    M.have_mouse = 1;

    /* the footer is read-only: a click on the readouts or the help row
     * must not zoom into whatever pixel happens to be behind it */
    if (y >= M.fh || y < 0) return;

    if (press_l)      click_centre(x, y);
    else if (press_r) zoom_step(x, y, 2.0);
}

static void handle_wheel(int notches) {
    if (notches == 0) return;
    /* one notch is 1.25x, and a burst is capped so a fast wheel cannot
     * jump a factor of a million in one frame */
    if (notches >  8) notches =  8;
    if (notches < -8) notches = -8;
    zoom_anchor(pow(0.8, (double)notches));
}

static void handle_key(int key, int press) {
    if (!press) return;
    /* The numeric keypad is NOT handled here on purpose: input.c maps
     * KEY_KPPLUS/KEY_KPMINUS to "+"/"-" text as well, and the server
     * sends both events, so handling the keycode too made one keypad
     * press zoom 0.8^2 = 1.56x while the main row zoomed 1.25x. Every
     * zoom control is exactly 1.25x per press now (left click is the
     * documented 2x click-to-centre). */
    switch (key) {
    case KEY_UP:    pan_step(0, -1); break;
    case KEY_DOWN:  pan_step(0,  1); break;
    case KEY_LEFT:  pan_step(-1, 0); break;
    case KEY_RIGHT: pan_step( 1, 0); break;
    default: break;
    }
}

static void handle_text(const char *text, int *quit) {
    for (const char *p = text; *p && !*quit; p++) {
        switch (*p) {
        case 'q': case 'Q': *quit = 1; break;
        case 'r': case 'R': reset_view(); render_start(); break;
        case '+': case '=': zoom_anchor(0.8);  break;
        case '-': case '_': zoom_anchor(1.25); break;
        default: break;
        }
    }
}

static void handle_resize(int nw, int nh) {
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == M.w && nh == M.h) return;

    int ofw = M.fw, ofh = M.fh, ofl = M.flines;
    M.w = nw;
    M.h = nh;
    /* layout() clamps to MClient.maxw/maxh and to the fixed 512x384
     * picture buffer, and recomputes the footer for the new height and
     * text scale. Nothing is reallocated: the mapping only ever grows
     * inside what mc_open() was told about. */
    layout();
    if (M.fw != ofw || M.fh != ofh || M.flines != ofl) render_start();
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

    /* the fonts are the only truth about how tall a line is, and layout()
     * needs them on every resize */
    g_fmed = fmed;
    g_freg = freg;

    /* the client area may grow, but only inside what mc_open() mapped
     * (the server allocates a whole screen and reports the maxima) and
     * this app's fixed picture buffer */
    M.maxw = c.maxw;
    M.maxh = c.maxh;
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
            switch (ev.type) {
            case MC_CLOSE:  quit = 1; break;
            case MC_TEXT:   handle_text(ev.text, &quit); break;
            case MC_KEY:    handle_key(ev.key, ev.press); break;
            case MC_MOUSE:  handle_mouse(ev.x, ev.y, ev.buttons); break;
            case MC_WHEEL:  handle_wheel(ev.wheel); break;
            case MC_RESIZE: handle_resize(ev.w, ev.h); break;
            default: break;
            }
            if (quit) break;
        }
        if (quit) break;

        /* One band, then straight back to the top of the loop: that is
         * what keeps the window answering clicks while it fills in. */
        if (M.rendering) {
            render_band();
            draw(&c, fmed, freg);
            mc_present(&c);
            continue;
        }

        /* a refused zoom changes only the footer: repaint once */
        if (M.dirty) {
            M.dirty = 0;
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
