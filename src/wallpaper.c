/* ============================================================
 *  Mystic OS - wallpaper.c
 *  Six backgrounds, painted by the CPU, straight into the
 *  framebuffer surface. No image files, no GPU.
 *
 *  Every pixel is a pure function of:
 *      - the theme (so all six work with every theme),
 *      - the wallpaper index,
 *      - `seconds`, and only for the animated ones.
 *
 *  Cost is what makes this usable on old hardware, so the rules
 *  here are: no transcendental function inside a per-pixel loop
 *  unless it is amortised over a table (a 256 entry sine table does
 *  the sin work for Waves and for the Black Hole's spiral arms, one
 *  sqrt per row does it for Orbits and the Dusk lights, and the
 *  Black Hole's geodesics are integrated once and cached), and the
 *  float divide gfx_glow does per pixel becomes a multiply by a
 *  precomputed reciprocal plus a 128 step alpha ramp.
 *
 *  Measured at 1280x800 with the shipping flags (-Os), process CPU
 *  time, best of 30 draws (this host was running three other builds,
 *  so the four originals read about twice their idle figures here):
 *
 *      Dusk 5.2 ms   Mesh 3.1 ms   Waves 3.1 ms   Orbits 0.5 ms
 *      Black Hole 16 ms          Nebula 48 ms
 *
 *  So the black hole costs about 3x Dusk and the nebula about 9x. Only
 *  the black hole is ever on the animation path (the desktop repaints
 *  animated backgrounds at 12.5 Hz, which is ~20% of one core); the
 *  nebula reports wallpaper_animated() == 0 and is painted once.
 *
 *  Both new ones also cap how many pixels they trace: above 360k pixels
 *  they are computed at 1/q resolution and scaled up bilinearly, because
 *  both are low-pass pictures and the cost has to stay flat as screens
 *  grow. See wp_q() / wp_upscale().
 * ============================================================ */
#include "wallpaper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* one wave period, in 16.16 fixed point over a 256 entry sine table */
#define WP_SINE_N     256
#define WP_SINE_MASK  (WP_SINE_N - 1)
#define WP_PHASE_MASK ((WP_SINE_N << 16) - 1)

/* ------------------------------------------------------------ names */

static const char *const WP_NAMES[WALLPAPER_COUNT] = {
    "Dusk", "Mesh", "Waves", "Orbits", "Black Hole", "Nebula"
};

const char *wallpaper_name(int index) {
    if (index < 0 || index >= WALLPAPER_COUNT) return NULL;
    return WP_NAMES[index];
}

int wallpaper_animated(int index) {
    /* Dusk, Mesh and Nebula are still pictures: the desktop can paint them
       once and then leave the framebuffer alone. Waves, Orbits and the
       Black Hole use `seconds`, so they have to be repainted - the black
       hole turns its disk, which is the whole point of it. */
    return (index == 2 || index == 3 || index == 4) ? 1 : 0;
}

static int wp_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* case-insensitive compare of a user string against a lowercase literal,
 * ignoring leading and trailing whitespace. CR and LF count as whitespace
 * because the name comes from a config file (main.c reads
 * /etc/mystic/wallpaper and only strips the newline, so a CRLF checkout
 * would otherwise hand us "Mesh\r"). */
static int wp_name_is(const char *user, const char *literal) {
    while (*user == ' ' || *user == '\t' || *user == '\r' || *user == '\n') user++;
    size_t i = 0;
    for (; literal[i]; i++) {
        if (wp_lower((unsigned char)user[i]) != literal[i]) return 0;
    }
    for (; user[i]; i++) {
        if (user[i] != ' ' && user[i] != '\t' && user[i] != '\r' && user[i] != '\n')
            return 0;
    }
    return 1;
}

int wallpaper_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        /* the literals are lowercase copies of WP_NAMES, so the compare
         * stays allocation free and case-insensitive */
        char low[16];
        size_t n = strlen(WP_NAMES[i]);
        if (n >= sizeof(low)) continue;
        for (size_t k = 0; k <= n; k++) low[k] = (char)wp_lower((unsigned char)WP_NAMES[i][k]);
        if (wp_name_is(name, low)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------ pixel helpers */

static inline u32 wp_opaque(u32 c) { return c | 0xFF000000u; }

static inline int wp_lerp8(int a, int b, int t) {          /* t: 0..256 */
    return (a * (256 - t) + b * t) >> 8;
}

static inline u32 wp_lerp_color(u32 a, u32 b, int t) {
    return 0xFF000000u
         | ((u32)wp_lerp8(GET_R(a), GET_R(b), t) << 16)
         | ((u32)wp_lerp8(GET_G(a), GET_G(b), t) << 8)
         |  (u32)wp_lerp8(GET_B(a), GET_B(b), t);
}

/* one pixel of `color` at `alpha` (0..255) over what is already there */
static inline void wp_blend(Surface *s, int x, int y, u32 color, int alpha) {
    if (alpha <= 0) return;
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    u32 *p = s->px + (size_t)y * s->stride + x;
    if (alpha >= 255) { *p = wp_opaque(color); return; }
    u32 d = *p;
    int ia = 255 - alpha;
    int r = (int)((GET_R(color) * (u32)alpha + GET_R(d) * (u32)ia) / 255u);
    int g = (int)((GET_G(color) * (u32)alpha + GET_G(d) * (u32)ia) / 255u);
    int b = (int)((GET_B(color) * (u32)alpha + GET_B(d) * (u32)ia) / 255u);
    *p = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* ------------------------------------------------------------ 0. Dusk
 * The original desktop: a vertical gradient with three soft lights on it.
 * Same falloff as gfx_glow (a = t*t*strength with t = 1 - d2/r2) but the
 * per-pixel float divide becomes a multiply by a precomputed reciprocal
 * plus a 128 step ramp, and each row walks only the span the circle
 * actually covers, which is what keeps three screen sized lights inside a
 * couple of milliseconds.
 */
static void wp_glow(Surface *s, float cx, float cy, float radius, u32 color, float strength) {
    if (!(radius > 0.0f) || !(strength > 0.0f)) return;

    /* 128 steps of alpha instead of gfx_glow's continuous falloff: the
       table costs nothing (built once per light) and halves the
       quantisation, so no rings show up on a dark gradient. */
    enum { LEVELS = 128 };
    int ramp[LEVELS + 1];
    for (int i = 0; i <= LEVELS; i++) {
        float t = 1.0f - (float)i / (float)LEVELS;
        float a = t * t * strength;
        if (a > 1.0f) a = 1.0f;
        ramp[i] = (int)(a * 255.0f + 0.5f);
    }
    float r2 = radius * radius;
    float scale = (float)LEVELS / r2;

    int y0 = (int)floorf(cy - radius), y1 = (int)ceilf(cy + radius);
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;

    int cr = GET_R(color), cg = GET_G(color), cb = GET_B(color);

    for (int y = y0; y < y1; y++) {
        float dy = (float)y - cy;
        float dy2 = dy * dy;
        if (dy2 >= r2) continue;                 /* this row misses the light */

        /* The light is a circle, so a row only covers one span of it. One
           sqrt per row (never per pixel) replaces a test on every pixel of
           the bounding box, and the corners - 21% of the box - are skipped. */
        float span = sqrtf(r2 - dy2);
        int xa = (int)floorf(cx - span), xb = (int)ceilf(cx + span);
        if (xa < 0) xa = 0;
        if (xb > s->w) xb = s->w;
        if (xb <= xa) continue;

        int iy = (int)(dy2 * scale);             /* per-row half of the index */
        u32 *row = s->px + (size_t)y * s->stride;

        for (int x = xa; x < xb; x++) {
            /* per-pixel square, not a finite difference: the add-chain
               version (dxx += dstep; dstep += 2) is 2x SLOWER here because
               it serialises on a 2-add dependency, while this form lets the
               out-of-order core overlap iterations */
            float fdx = (float)x - cx;
            int idx = (int)(fdx * fdx * scale) + iy;
            if (idx > LEVELS) continue;
            int a = ramp[idx];
            if (a <= 0) continue;
            /* d*(1-a) + c*a with a in 0..255, rounded; all terms stay
               non-negative so the shift is well defined, and the divide by
               255 that surf_blend pays for is folded into the shift */
            u32 d = row[x];
            u32 ia = (u32)(256 - a);
            int ir = (int)((GET_R(d) * ia + (u32)cr * (u32)a + 128u) >> 8);
            int ig = (int)((GET_G(d) * ia + (u32)cg * (u32)a + 128u) >> 8);
            int ib = (int)((GET_B(d) * ia + (u32)cb * (u32)a + 128u) >> 8);
            row[x] = 0xFF000000u | ((u32)ir << 16) | ((u32)ig << 8) | (u32)ib;
        }
    }
}

static void wp_draw_dusk(Surface *s, const Theme *th) {
    int W = s->w, H = s->h;
    gfx_gradient(s, 0, 0, W, H, th->bg_top, th->bg_bottom);

    /* The three lights of the original desktop, in the same places and at
       the same strengths. Measured against main.c's gfx_gradient + three
       gfx_glow calls over all six themes: max channel difference 4/255 and
       mean below 0.8/255 - the same picture - at 1.4x to 1.9x the speed. */
    wp_glow(s, (float)W * 0.22f, (float)H * 0.18f, (float)W * 0.42f,
            th->glow, th->glow_strength * 0.85f);
    wp_glow(s, (float)W * 0.86f, (float)H * 0.72f, (float)W * 0.38f,
            th->accent, th->glow_strength * 0.45f);
    wp_glow(s, (float)W * 0.55f, (float)H * 1.02f, (float)W * 0.50f,
            th->glow, th->glow_strength * 0.55f);
}

/* ------------------------------------------------------------ 1. Mesh
 * A two-axis gradient mesh: four corner colours, bilinear between them,
 * with a soft oval vignette on top. Every corner is a mix of the theme's
 * own background with one of its two light sources, so a dark theme can
 * only produce dark corners and a light theme only light ones.
 */
static void wp_draw_mesh(Surface *s, const Theme *th) {
    int W = s->w, H = s->h;

    float in_top    = th->dark ? 0.42f : 0.24f;      /* accent in the top-right */
    float in_bottom = th->dark ? 0.55f : 0.40f;      /* glow   in the bottom-left */
    float in_br     = th->dark ? 0.26f : 0.15f;      /* accent in the bottom-right */

    u32 tl = wp_opaque(th->bg_top);
    u32 tr = wp_opaque(color_mix(th->bg_top, th->accent, in_top));
    u32 bl = wp_opaque(color_mix(th->bg_bottom, th->glow, in_bottom));
    u32 br = wp_opaque(color_mix(th->bg_bottom, th->accent, in_br));

    /* vignette depth out of 256, applied on both axes and multiplied, so
       it reads as a soft oval instead of a cross */
    int vig = th->dark ? 44 : 20;
    /* 8.24 reciprocal across the row: w8 = (x * step) >> 24 is exactly 0 at
       x = 0 and exactly 256 at x = W-1, monotone in between and never off
       by more than one level of 256. The obvious 8.8 accumulator drifts by
       +2 levels mid-row and then pins its last ~19 columns at 256 (a flat
       band at the right edge); rounding the reciprocal up is what makes the
       end points exact without a per-pixel divide. */
    long long step_x = (W > 1) ? (((256LL << 24) + (W - 2)) / (long long)(W - 1)) : 0;

    for (int y = 0; y < H; y++) {
        int ty = (H > 1) ? ((y * 256 + (H - 1) / 2) / (H - 1)) : 0;
        if (ty > 256) ty = 256;
        u32 le = wp_lerp_color(tl, bl, ty);
        u32 re = wp_lerp_color(tr, br, ty);

        int yn = (H > 1) ? ((y * 512) / (H - 1)) - 256 : 0;    /* -256..256 */
        int vy = 256 - ((vig * ((yn * yn) >> 8)) >> 8);
        if (vy < 0) vy = 0;

        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) {
            int w8 = (int)(((long long)x * step_x) >> 24);   /* 0..256 */
            if (w8 > 256) w8 = 256;
            int xn = 2 * w8 - 256;                      /* -256..256 */
            int vx = 256 - ((vig * ((xn * xn) >> 8)) >> 8);
            int f = (vx * vy) >> 8;                     /* 0..256 */
            int r = wp_lerp8(GET_R(le), GET_R(re), w8);
            int g = wp_lerp8(GET_G(le), GET_G(re), w8);
            int b = wp_lerp8(GET_B(le), GET_B(re), w8);
            row[x] = 0xFF000000u
                   | ((u32)((r * f) >> 8) << 16)
                   | ((u32)((g * f) >> 8) << 8)
                   |  (u32)((b * f) >> 8);
        }
    }
}

/* ------------------------------------------------------------ 2. Waves
 * Layered sine bands running across the screen. The band phase depends on y
 * (that is the band spacing) and is pushed up and down by a slow horizontal
 * sine, which is what makes the bands undulate instead of lying flat. A
 * second, finer band train is woven in so the result has depth.
 *
 * Everything is a 16.16 phase accumulator over a 256 entry sine table: the
 * inner loop is adds, a shift, a table index and an integer lerp - no sin()
 * and no division per pixel. Both the swell and the bands drift with
 * `seconds`, so the picture really moves.
 */
static void wp_draw_waves(Surface *s, const Theme *th, double seconds) {
    int W = s->w, H = s->h;

    gfx_gradient(s, 0, 0, W, H, th->bg_top, th->bg_bottom);

    /* the crest colour: the theme's two light sources, pulled back towards
       the background so the bands stay in the theme's own key */
    u32 crest = wp_opaque(color_mix(color_mix(th->glow, th->accent, 0.40f),
                                    th->bg_bottom, th->dark ? 0.25f : 0.45f));

    int sint[WP_SINE_N];
    for (int i = 0; i < WP_SINE_N; i++) {
        float v = 127.5f + 127.5f * sinf(6.28318530718f * (float)i / (float)WP_SINE_N);
        int q = (int)(v + 0.5f);
        sint[i] = q < 0 ? 0 : (q > 255 ? 255 : q);
    }

    /* how far a crest reaches into the water (0..255). Glow-heavy themes
       get slightly stronger bands, within bounds, so every theme reads. */
    float gs = th->glow_strength;
    if (!(gs > 0.20f)) gs = 0.20f;
    if (gs > 1.20f) gs = 1.20f;
    int amp = (int)((th->dark ? 138.0f : 108.0f) * gs);
    if (amp > 210) amp = 210;

    int shape[WP_SINE_N];
    for (int i = 0; i < WP_SINE_N; i++) {
        float t = (float)i / 255.0f;
        shape[i] = (int)(t * t * (3.0f - 2.0f * t) * (float)amp + 0.5f);
    }

    double period = (double)(WP_SINE_N << 16);        /* one full cycle */
    double t = (seconds > 0.0) ? seconds : 0.0;
    if (t > 1.0e7) t = 1.0e7;                         /* keep the fixed point sane */

    /* 1.5 swell wavelengths across the surface, drifting sideways */
    int step_x  = (int)(period * 1.5 / (double)W);
    int drift_x = (int)((long long)(period * 0.045 * t) & WP_PHASE_MASK);
    /* five bands down the surface, the finer train has nine */
    int row_a = (int)(period * 5.0 / (double)H);
    int row_b = (int)(period * 9.0 / (double)H);
    int drift_a = (int)((long long)(-period * 0.020 * t) & WP_PHASE_MASK);
    int drift_b = (int)((long long)(period * 0.013 * t) & WP_PHASE_MASK);
    /* how far the swell lifts a band: a third of a band period per sine unit */
    int lift_a = (int)(period * 0.33 / 128.0);
    int lift_b = (int)(period * 0.16 / 128.0);

    for (int y = 0; y < H; y++) {
        float ty = (H > 1) ? (float)y / (float)(H - 1) : 0.0f;
        u32 base = wp_opaque(color_mix(th->bg_top, th->bg_bottom, ty));

        /* the per-row part of each band phase, plus the bias that keeps the
           swell offset positive before it is masked into range */
        int base_a = (int)(((long long)drift_a + (long long)row_a * (long long)y
                            + (WP_SINE_N << 16)) & WP_PHASE_MASK);
        int base_b = (int)(((long long)drift_b + (long long)row_b * (long long)y
                            + (WP_SINE_N << 16)) & WP_PHASE_MASK);
        int xa = drift_x;
        u32 *row = s->px + (size_t)y * s->stride;

        for (int x = 0; x < W; x++) {
            int m = sint[(xa >> 16) & WP_SINE_MASK] - 128;   /* -128..127 swell */
            xa = (xa + step_x) & WP_PHASE_MASK;
            int va = sint[(((base_a + m * lift_a) & WP_PHASE_MASK) >> 16) & WP_SINE_MASK];
            int vb = sint[(((base_b + m * lift_b) & WP_PHASE_MASK) >> 16) & WP_SINE_MASK];
            row[x] = wp_lerp_color(base, crest, shape[(va * 3 + vb) >> 2]);
        }
    }
}

/* ------------------------------------------------------------ 3. Orbits
 * Three large concentric rings and two discs slowly going round the middle
 * of the surface, in the accent colour. The rings breathe and the discs
 * orbit, both from `seconds`.
 */

/* A ring of radius r: for each row the ring only touches two short spans,
 * so we compute those from the two circle equations instead of walking the
 * whole chord - that is the difference between ~12k pixels and ~300k. */
static void wp_ring(Surface *s, float cx, float cy, float r, float thickness,
                    u32 color, int alpha) {
    if (!(r > 1.0f) || thickness <= 0.0f || alpha <= 0) return;
    if (alpha > 255) alpha = 255;

    float out = r + thickness * 1.6f;
    float out2 = out * out;
    float r2 = r * r;
    float band2 = 2.0f * r * thickness + thickness * thickness;
    float inv = 1.0f / band2;

    int y0 = (int)floorf(cy - out), y1 = (int)ceilf(cy + out);
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;

    for (int y = y0; y < y1; y++) {
        float dy = ((float)y + 0.5f) - cy;
        float dy2 = dy * dy;
        if (dy2 >= out2) continue;
        float span = sqrtf(out2 - dy2);

        /* the ring covers at most two spans on this row: left of the hole
           and right of it, or the single chord when the row misses the hole */
        float hole = (dy2 < r2) ? sqrtf(r2 - dy2) : -1.0f;
        int spans = (hole < 0.0f) ? 1 : 2;
        for (int side = 0; side < spans; side++) {
            float lo, hi;
            if (hole < 0.0f)  { lo = -span; hi =  span; }
            else if (side == 0) { lo = -span; hi = -hole; }
            else                { lo =  hole; hi =  span; }
            int x0 = (int)ceilf(cx + lo);
            int x1 = (int)floorf(cx + hi);
            if (x0 < 0) x0 = 0;
            if (x1 > s->w - 1) x1 = s->w - 1;
            for (int x = x0; x <= x1; x++) {
                float dx = ((float)x + 0.5f) - cx;
                float d2 = dx * dx + dy2;
                float d = d2 - r2;
                if (d < 0.0f) d = -d;
                float cov = 1.0f - d * inv;
                if (cov <= 0.0f) continue;
                int a = (int)(cov * cov * (float)alpha + 0.5f);
                wp_blend(s, x, y, color, a);
            }
        }
    }
}

/* a soft round spot */
static void wp_disc(Surface *s, float cx, float cy, float radius, u32 color, int alpha) {
    if (!(radius > 0.0f) || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    int x0 = (int)floorf(cx - radius), x1 = (int)ceilf(cx + radius);
    int y0 = (int)floorf(cy - radius), y1 = (int)ceilf(cy + radius);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    float inv = 1.0f / (radius * radius);

    for (int y = y0; y < y1; y++) {
        float dy = ((float)y + 0.5f) - cy;
        float dy2 = dy * dy;
        for (int x = x0; x < x1; x++) {
            float dx = ((float)x + 0.5f) - cx;
            float t = 1.0f - (dx * dx + dy2) * inv;
            if (t <= 0.0f) continue;
            int a = (int)(t * t * (float)alpha + 0.5f);
            wp_blend(s, x, y, color, a);
        }
    }
}

static void wp_draw_orbits(Surface *s, const Theme *th, double seconds) {
    int W = s->w, H = s->h;

    gfx_gradient(s, 0, 0, W, H, th->bg_top, th->bg_bottom);

    double t = (seconds > 0.0) ? seconds : 0.0;
    float half = (float)(W < H ? W : H) * 0.5f;
    float cx = (float)W * 0.5f, cy = (float)H * 0.5f;

    /* accent for the shape, pushed towards the theme's glow on a dark theme.
       On a light theme that glow is white, which would make the rings
       brighter than the paper and effectively invisible, so there they are
       pulled the other way, towards the theme's own background. */
    u32 ring_col = wp_opaque(th->dark ? color_mix(th->accent, th->glow, 0.45f)
                                      : color_mix(th->accent, th->bg_bottom, 0.18f));
    u32 disc_col = wp_opaque(th->dark ? color_mix(th->accent, th->glow, 0.20f)
                                      : th->accent);
    u32 core_col = wp_opaque(th->dark ? th->glow
                                      : color_mix(th->accent, th->bg_bottom, 0.10f));
    int ring_alpha = th->dark ? 170 : 150;
    int disc_alpha = th->dark ? 165 : 150;

    for (int i = 0; i < 3; i++) {
        double phase = (t * (0.035 + 0.011 * (double)i) + 0.19 * (double)i) * 6.28318530718;
        float breathe = 1.0f + 0.030f * (float)sin(phase);
        float r = half * (0.46f + 0.17f * (float)i) * breathe;
        wp_ring(s, cx, cy, r, 3.0f + 2.2f * (float)i, ring_col, ring_alpha - i * 20);
    }

    /* two discs going round, squashed vertically so the path reads as an
       orbit seen at a slight angle rather than a flat circle */
    for (int i = 0; i < 2; i++) {
        double a = (t * (0.045 - 0.018 * (double)i) + 0.35 * (double)i) * 6.28318530718;
        /* keep both discs inside the innermost ring, so a disc never sits on
           top of a ring and reads as a smudge */
        float orbit = half * (0.24f + 0.12f * (float)i);
        float dx = cos(a) * orbit;
        float dy = sin(a) * orbit * 0.72f;
        wp_disc(s, cx + dx, cy + dy, half * (0.070f + 0.025f * (float)i),
                disc_col, disc_alpha - i * 40);
    }

    /* a small core, so the rings have something to be centred on */
    wp_disc(s, cx, cy, half * 0.045f, core_col, th->dark ? 190 : 150);
}

/* ------------------------------------------------------ shared noise
 * A deterministic 32 bit hash. The star fields and the nebula grain are
 * sampled in normalised coordinates through this, never with rand(), so
 * the sky is the same picture on every boot and at every resolution.
 */
static inline u32 wp_h32(u32 x, u32 y, u32 seed) {
    u32 h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/* floor() without a libm call: on a baseline x86-64 build floorf() is a
 * call into libm (no SSE4.1 roundsd), which costs more than everything else
 * the star field does per pixel. */
static inline int wp_ifloor(float v) {
    int i = (int)v;
    return i - (v < (float)i);
}

/* atan2 with one divide and three branches; worst error 0.0032 rad, which
 * is 0.2% of the range we index a geodesic table with. */
static inline float wp_atan2f(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    int swap = (ay > ax);
    float t = swap ? ax / (ay + 1e-20f) : ay / (ax + 1e-20f);
    float a = 0.78539816f * t + 0.27323954f * t * (1.0f - t);
    if (swap) a = 1.57079633f - a;
    if (x < 0.0f) a = 3.14159265f - a;
    if (y < 0.0f) a = -a;
    return a;
}

/* Brightness of one star from one grid layer, at the normalised sky
 * position (ux, uy). `cell` is the grid pitch and `rad2` the squared star
 * radius, both in the same units. Pure: it draws nothing, so the black
 * hole can decide how much of the star survives the disk in front of it. */
static inline int wp_star_at(float ux, float uy, float inv_cell, u32 seed,
                             int thresh, int peak, float inv_rad2) {
    float gx = ux * inv_cell, gy = uy * inv_cell;
    int ix = wp_ifloor(gx), iy = wp_ifloor(gy);
    u32 h = wp_h32((u32)(ix + 65536), (u32)(iy + 65536), seed);
    if ((int)(h & 0xFFu) >= thresh) return 0;      /* most cells are empty */

    u32 h2 = wp_h32(h, (u32)iy * 2654435761u, seed ^ 0x9E3779B9u);
    float jx = 0.25f + 0.5f * (float)((h >> 8) & 0xFFu) * (1.0f / 255.0f);
    float jy = 0.25f + 0.5f * (float)((h >> 16) & 0xFFu) * (1.0f / 255.0f);
    float px = (gx - (float)ix) - jx;
    float py = (gy - (float)iy) - jy;
    float d2 = (px * px + py * py) * inv_rad2;
    if (d2 >= 1.0f) return 0;
    float t = 1.0f - d2;
    /* 40%..100% of `peak`, so the field is not a uniform dust sheet */
    float mag = 0.40f + 0.60f * (float)(h2 & 0xFFu) * (1.0f / 255.0f);
    return (int)((float)peak * mag * t * t + 0.5f);
}

/* --------------------------------------------------- render resolution
 * The black hole and the nebula are the two backgrounds whose per-pixel cost
 * is measured in hundreds of cycles rather than tens. Both are low-pass by
 * nature - a smooth accretion disk, a cloud of gas - so above a threshold
 * they are traced at 1/q resolution and scaled up bilinearly. That keeps the
 * per-frame cost flat as the screen grows, and on a 1280x800 display the only
 * visible difference is a slightly softer photon ring.
 */
#define WP_TRACE_MAX 360000

static int wp_q(int w, int h) {
    int q = 1;
    while (q < 4 && (w / q) * (h / q) > WP_TRACE_MAX) q++;
    return q;
}

/* Bilinear upscale of a q-times smaller image into dst, in 8.8 fixed point:
 * one add per pixel for the position, four loads, and integer lerps, because
 * this runs over the whole screen and must stay cheaper than a fill. */
static void wp_upscale(Surface *dst, const u32 *src, int sw, int sh, int q) {
    const int step = 256 / q;
    const int maxx = (sw - 1) * 256, maxy = (sh - 1) * 256;
    const int lastx = (sw > 1) ? sw - 2 : 0, lasty = (sh > 1) ? sh - 2 : 0;

    for (int y = 0; y < dst->h; y++) {
        int sy = ((2 * y + 1) * 128) / q - 128;
        if (sy < 0) sy = 0;
        if (sy > maxy) sy = maxy;
        int iy = sy >> 8;
        if (iy > lasty) iy = lasty;
        int fy = sy & 255, ify = 256 - fy;
        const u32 *r0 = src + (size_t)iy * sw;
        const u32 *r1 = src + (size_t)((iy + 1 <= lasty) ? iy + 1 : iy) * sw;
        u32 *out = dst->px + (size_t)y * dst->stride;
        int sx = 128 / q - 128;

        for (int x = 0; x < dst->w; x++, sx += step) {
            int v = sx;
            if (v < 0) v = 0;
            if (v > maxx) v = maxx;
            int ix = v >> 8;
            if (ix > lastx) ix = lastx;
            int fx = v & 255, ifx = 256 - fx;
            u32 a = r0[ix], b = r0[ix + 1], c = r1[ix], d = r1[ix + 1];
            int ar = ((int)GET_R(a) * ifx + (int)GET_R(b) * fx) >> 8;
            int ag = ((int)GET_G(a) * ifx + (int)GET_G(b) * fx) >> 8;
            int ab = ((int)GET_B(a) * ifx + (int)GET_B(b) * fx) >> 8;
            int br = ((int)GET_R(c) * ifx + (int)GET_R(d) * fx) >> 8;
            int bg = ((int)GET_G(c) * ifx + (int)GET_G(d) * fx) >> 8;
            int bb = ((int)GET_B(c) * ifx + (int)GET_B(d) * fx) >> 8;
            int rr = (ar * ify + br * fy) >> 8;
            int rg = (ag * ify + bg * fy) >> 8;
            int rb = (ab * ify + bb * fy) >> 8;
            out[x] = 0xFF000000u | ((u32)rr << 16) | ((u32)rg << 8) | (u32)rb;
        }
    }
}

/* Run `body` into a 1/q scratch surface when q > 1. Declared here so both of
 * the expensive painters share one allocation and one fallback path. */
static u32 *wp_scratch(int w, int h, int q, int *ow, int *oh) {
    *ow = (w + q - 1) / q;
    *oh = (h + q - 1) / q;
    if (q <= 1) return NULL;
    return malloc((size_t)(*ow) * (size_t)(*oh) * sizeof(u32));
}

/* ------------------------------------------------------------ 4. Black Hole
 * A Schwarzschild black hole at an adjustable inclination, with a thin
 * accretion disk traced through its own gravitational field. This is the
 * one wallpaper whose moving parts are computed rather than painted.
 *
 *  - A photon passing the hole at impact parameter b obeys
 *
 *        d2u/dphi2 + u = 1.5 u^2 ,      u = 1/r,   rs = 1
 *
 *    (the Schwarzschild null geodesic in the equatorial plane). That ODE is
 *    integrated once, with RK4, for 288 impact parameters, and the radius at
 *    every pi/224 of sweep is cached in a table. Two photon invariants fall
 *    straight out of the same table: the capture cross section (rays that
 *    run into the horizon) and the total sweep to escape (which is what
 *    lenses the star field).
 *
 *  - Geometry: the screen x axis is the line of nodes, so a disk radius r
 *    appears at screen (r cos(theta), cos(yaw) r sin(theta)): the minor axis
 *    is really foreshortened by cos(yaw), and the Doppler lever arm is
 *    sin(yaw) cos(theta). yaw is the angle between the line of sight and the
 *    disk axis: 82 degrees by default (nearly edge-on), 10 degrees opens it
 *    into a broad annulus, 88 degrees closes it to a thin line. Pixels that
 *    are outside the projected disk can still hold a lensed image of its far
 *    side, so the disk is evaluated over the whole frame from one camera -
 *    there is no clipped rectangle and therefore no rectangular seam.
 *
 *  - Animation: the pattern is advected by the local Keplerian angular
 *    velocity omega(r) = sqrt(0.5/r^3) times WP_BH_RATE, so the inner rings
 *    visibly outrun the rim. That differential rotation is what makes the
 *    disk read as orbiting matter instead of a spinning picture. The
 *    photon ring and the lensed (secondary) image carry a few percent of
 *    slow shimmer. Everything is a function of `seconds`, which is why
 *    wallpaper_animated(4) is 1.
 *
 *  - Interaction: wallpaper_blackhole_view() and _spin() at the bottom of
 *    this section are the two knobs the desktop drives from input; the
 *    exact drag/wheel mapping is documented in wallpaper.h.
 *
 *  - Relativistic beaming: a circular orbit at r moves at
 *    beta = sqrt(0.5/(r-1)), its line-of-sight component is
 *    sin(yaw) cos(theta), and the observed flux is scaled by delta^3 with
 *    delta = sqrt(1-1/r) / (1 - beta_los) (gravitational redshift and
 *    Doppler together). The factor is clamped so the receding side stays
 *    visible instead of going black.
 *
 *  - The shadow (b < 3*sqrt(3)/2) is black: no disk, no stars. The star
 *    field is a deterministic, position-independent layer sampled behind
 *    the hole; it is the only cached picture. A soft photon-ring term sits
 *    just outside the critical curve and shimmers slowly.
 *
 *  - Cost: the star field is the only cached picture. It depends on the
 *    surface size and nothing else - not on the theme, the time or the view
 *    - so a theme switch, a drag and every animation frame keep it. The
 *    geodesic table, the display curve and a 128-entry support bound are
 *    built once: sup[psi] is the largest impact parameter whose crossing at
 *    that sweep can still be inside the disk, measured from the same table
 *    with a margin. That keeps the per-frame disk work to the pixels the
 *    disk can actually reach, and because sup is a strict superset of the
 *    lit region it cannot clip an arc or leave a seam. Above WP_TRACE_MAX
 *    pixels the whole frame (sky, stars, disk, ring) is evaluated at 1/q
 *    resolution and scaled up; the trace still samples every pixel of that
 *    layer through the same camera, and the upscale is one pass over the
 *    whole surface, never a pasted sub-rectangle.
 */

#define WP_BH_NB0   160                    /* b samples, BC .. WP_BH_BM  */
#define WP_BH_NB1   128                    /* b samples, WP_BH_BM .. max */
#define WP_BH_NB    (WP_BH_NB0 + WP_BH_NB1)
#define WP_BH_BM    4.5f                   /* where the b grid coarsens   */
#define WP_BH_NA    225                    /* sweep samples per b, /3    */
#define WP_BH_BC    2.59807621f            /* 3*sqrt(3)/2 rs: shadow edge*/
#define WP_BH_RIN   3.0f                   /* ISCO, rs: disk inner edge  */
#define WP_BH_ROUT  18.0f                  /* disk outer edge, rs        */
#define WP_BH_BMAX  46.0f
#define WP_BH_PI    3.14159265358979
#define WP_BH_DMAX  9.42477796076938       /* 3*pi of sweep, radians     */
#define WP_BH_DSC   4.0                    /* screen radius of the shadow */
#define WP_BH_K0    ((float)WP_BH_NB0 / (WP_BH_BM - WP_BH_BC))
#define WP_BH_K1    ((float)WP_BH_NB1 / (WP_BH_BMAX - WP_BH_BM))
#define WP_BH_NS    (WP_BH_NA / 3)         /* samples per pi of sweep    */
#define WP_BH_SUPN  128                    /* support-bound table entries */
#define WP_BH_NC    160                    /* disk palette entries       */
#define WP_BH_RATE  1.8f                   /* Keplerian phase per second */
#define WP_BH_RMAX  1.30f                  /* photon ring outer cut, x BC*/
#define WP_BH_TAU   6.28318530718f

/* The view and the disk clock. Only the single-threaded compositor touches
 * these, and only through the four functions at the bottom of the section. */
static float bh_yaw  = WALLPAPER_BH_YAW_DEF;
static double bh_spin = 0.0;              /* seconds of disk time       */

static struct {
    int   built;
    float sweep[WP_BH_NB];                 /* sweep to escape, radians   */
    unsigned short r[WP_BH_NB][WP_BH_NA];  /* crossing radius * 64       */
    int   sinq[256];
    int   sup_built;
    float sup[WP_BH_SUPN];                 /* max b with a lit crossing   */
    u8    tone[1024];                      /* display curve, 0..1023     */
    u32   hot;                             /* palette's hottest colour   */

    /* palette, rebuilt only when the theme's three colours change */
    int   pal_built;
    u32   pal_accent, pal_glow, pal_bottom;
    u32   lut[WP_BH_NC];
    float bl[WP_BH_NC];                    /* orbital speed, c           */
    float gl[WP_BH_NC];                    /* sqrt(1 - 1/r)              */
    float om[WP_BH_NC];                    /* Keplerian omega, rad/s     */
    float sl[WP_BH_NC];                    /* spiral pitch, radians      */

    /* star field: the one cached picture, keyed by size only */
    int   star_w, star_h;
    u8   *star;

    /* a fixed 64x64 grain tile: deterministic, position independent */
    int   grain_built;
    signed char grain[64 * 64];

    /* one reusable trace surface, so a frame allocates nothing */
    int   scr_w, scr_h;
    u32  *scratch;

    /* one reusable row for the separable q = 2 upscale */
    int   up_w;
    u32  *uprow;
} bhgeo;

/* One ray: integrate the null geodesic from r = 4000 inwards and store the
 * radius (x64, 0 = "no crossing") at the centre of each sweep bin. Returns
 * the total sweep to escape, or 0 when the ray is captured. */
static double wp_bh_trace(double b, unsigned short *out) {
    const double dphi = WP_BH_DMAX / (double)WP_BH_NA;
    const double h = dphi * 0.25;
    double u = 1.0 / 4000.0, v, phi = 0.0;
    int next = 0, escaped = 0;

    for (int i = 0; i < WP_BH_NA; i++) out[i] = 0;

    double q = 1.0 / (b * b) - u * u + u * u * u;
    if (!(q > 0.0)) return 0.0;
    v = sqrt(q);

    while (next < WP_BH_NA && phi < WP_BH_DMAX) {
        double k1u = v,                 k1v = -u + 1.5 * u * u;
        double ua = u + 0.5 * h * k1u,  va = v + 0.5 * h * k1v;
        double k2u = va,                k2v = -ua + 1.5 * ua * ua;
        double ub = u + 0.5 * h * k2u,  vb = v + 0.5 * h * k2v;
        double k3u = vb,                k3v = -ub + 1.5 * ub * ub;
        double uc = u + h * k3u,        vc = v + h * k3v;
        double k4u = vc,                k4v = -uc + 1.5 * uc * uc;
        double un = u + h * (k1u + 2.0 * k2u + 2.0 * k3u + k4u) / 6.0;
        double vn = v + h * (k1v + 2.0 * k2v + 2.0 * k3v + k4v) / 6.0;
        double pn = phi + h;

        /* record every bin centre this step walked past */
        while (next < WP_BH_NA && (double)(next) * dphi + 0.5 * dphi <= pn) {
            double tc = (double)next * dphi + 0.5 * dphi;
            double f = (h > 0.0) ? (tc - phi) / h : 0.0;
            double uc2 = u + (un - u) * f;
            double rr = (uc2 > 0.0) ? 1.0 / uc2 : 0.0;
            if (rr > 1023.0) rr = 1023.0;
            out[next] = (unsigned short)(rr * 64.0 + 0.5);
            next++;
        }

        u = un; v = vn; phi = pn;
        if (!(u == u) || !(v == v)) return 0.0;          /* NaN: give up   */
        if (u > 1.0) return 0.0;                         /* swallowed      */
        if (u < 1.0 / 3000.0 && v < 0.0) {               /* escaped        */
            escaped = 1;
            break;
        }
    }
    /* Past the escape there is no crossing at that sweep, and the honest
     * value is an infinite radius: the crossing runs off to infinity as the
     * sweep approaches the escape angle. Filling those bins with a huge
     * radius makes the bilinear blend climb out of the disk instead of
     * sweeping through every colour in the palette. */
    if (escaped) {
        while (next < WP_BH_NA) out[next++] = 65535;
    }
    return (next > 0) ? phi : 0.0;
}

static void wp_bh_build(void) {
    for (int i = 0; i < WP_BH_NB; i++) {
        double b;
        if (i < WP_BH_NB0) b = (double)WP_BH_BC + (double)i / (double)WP_BH_K0;
        else               b = (double)WP_BH_BM + (double)(i - WP_BH_NB0) / (double)WP_BH_K1;
        bhgeo.sweep[i] = (float)wp_bh_trace(b, bhgeo.r[i]);
    }
    for (int i = 0; i < 256; i++) {
        float s = 127.5f + 127.5f * sinf(6.28318530718f * (float)i / 256.0f);
        bhgeo.sinq[i] = (int)(s + 0.5f);
    }
    /* display curve: linear to 128, then a soft shoulder, so the sky keeps
     * its exact values and the disk rolls off instead of clipping */
    for (int i = 0; i < 1024; i++) {
        if (i <= 128) bhgeo.tone[i] = (u8)i;
        else {
            float d = (float)(i - 128) * (1.0f / 127.0f);
            bhgeo.tone[i] = (u8)(128.0f + 127.0f * (d / (1.0f + d)) + 0.5f);
        }
    }
    bhgeo.built = 1;
}

/* b -> table coordinate in samples. The grid is uniform, so the index is a
 * multiply and a compare, but it is split at WP_BH_BM: 0.012 rs steps
 * through the strong field where the photon ring lives, 0.32 rs steps
 * outside it, where the image is smooth and interpolation absorbs the
 * difference. */
static inline float wp_bh_bidx(float b) {
    if (b < WP_BH_BM) return (b - WP_BH_BC) * WP_BH_K0;
    return (float)WP_BH_NB0 + (b - WP_BH_BM) * WP_BH_K1;
}

/* crossing radius at impact parameter b after `dlt` radians of sweep;
 * 0 when that crossing does not exist (captured, or already escaped) */
static inline float wp_bh_radius(float t, int j, float fs) {
    if (t < 0.0f || t >= (float)(WP_BH_NB - 1)) return 0.0f;
    if (j < 0 || j >= WP_BH_NA - 1) return 0.0f;
    int i = (int)t;
    float ft = t - (float)i;
    float a = (float)bhgeo.r[i][j]     + ((float)bhgeo.r[i][j + 1]     - (float)bhgeo.r[i][j])     * fs;
    float c = (float)bhgeo.r[i + 1][j] + ((float)bhgeo.r[i + 1][j + 1] - (float)bhgeo.r[i + 1][j]) * fs;
    return (a + (c - a) * ft) * (1.0f / 64.0f);
}

/* The largest impact parameter whose crossing at sweep psi can still be
 * inside [RIN, ROUT]. This is a property of the geodesic table alone: it
 * does not depend on the theme, the clock or the view, so it is built once.
 * The 0.15 rs margin makes the bound a strict superset of the lit region,
 * which is what lets it be used as a culling gate without clipping (and
 * therefore without a seam) at its edge. */
static void wp_bh_support(void) {
    for (int i = 0; i < WP_BH_SUPN; i++) {
        float psi = ((float)i + 0.5f) * ((float)WP_BH_PI / (float)WP_BH_SUPN);
        float s0 = psi * ((float)WP_BH_NA / (float)WP_BH_DMAX) - 0.5f;
        int   j0 = (int)s0;
        float fs = s0 - (float)j0;
        float bmax = 0.0f;
        for (float b = WP_BH_BC; b <= WP_BH_ROUT; b += 0.02f) {
            float t = wp_bh_bidx(b);
            float r1 = wp_bh_radius(t, j0, fs);
            float r2 = wp_bh_radius(t, j0 + WP_BH_NS, fs);
            float r3 = wp_bh_radius(t, j0 + 2 * WP_BH_NS, fs);
            if ((r1 >= WP_BH_RIN && r1 <= WP_BH_ROUT) ||
                (r2 >= WP_BH_RIN && r2 <= WP_BH_ROUT) ||
                (r3 >= WP_BH_RIN && r3 <= WP_BH_ROUT)) bmax = b;
        }
        bhgeo.sup[i] = bmax + 0.15f;
    }
    /* one-entry dilation: the table is indexed at 1.4 degree resolution, and
     * a ray between two entries must use the wider of them so the gate can
     * never cut the support it is meant to bound */
    for (int i = 0; i < WP_BH_SUPN; i++) {
        float m = bhgeo.sup[i];
        if (i > 0 && bhgeo.sup[i - 1] > m) m = bhgeo.sup[i - 1];
        if (i < WP_BH_SUPN - 1 && bhgeo.sup[i + 1] > m) m = bhgeo.sup[i + 1];
        bhgeo.sup[i] = m;
    }
    bhgeo.sup_built = 1;
}

/* ---- the disk palette, keyed by the theme's three colours -------------
 * The colours and the radial physics do not change from frame to frame, so
 * this is built on a theme change and reused. Only the phase arrays are
 * refreshed per frame (in wp_bh_paint). */
static void wp_bh_palette(const Theme *th) {
    if (bhgeo.pal_built &&
        bhgeo.pal_accent == th->accent &&
        bhgeo.pal_glow   == th->glow &&
        bhgeo.pal_bottom == th->bg_bottom) return;

    const float lspan = WP_BH_ROUT - WP_BH_RIN;
    u32 hot  = wp_opaque(color_mix(th->accent, RGB(0xFF, 0xF4, 0xDE), 0.62f));
    u32 warm = wp_opaque(th->accent);
    u32 cool = wp_opaque(color_mix(th->accent, th->glow, 0.62f));
    u32 rim  = wp_opaque(color_mix(th->glow, th->bg_bottom, 0.45f));
    bhgeo.hot = hot;

    for (int i = 0; i < WP_BH_NC; i++) {
        float t = (float)i / (float)(WP_BH_NC - 1);
        float r = WP_BH_RIN + lspan * t;
        /* r^-1 emissivity, with a soft inner lip and a soft outer rim so
           the disk does not end in a hard edge */
        float env = 1.65f * (WP_BH_RIN / r);
        float lip = (r < WP_BH_RIN * 1.22f) ? (r - WP_BH_RIN) / (0.22f * WP_BH_RIN) : 1.0f;
        if (lip < 0.0f) lip = 0.0f;
        float rimf = (t > 0.80f) ? (1.0f - (t - 0.80f) / 0.20f) : 1.0f;
        if (rimf < 0.0f) rimf = 0.0f;
        env *= lip * rimf * rimf;

        /* a narrow bright inner edge: the hottest, fastest material sits at
           the ISCO and that is what the eye reads as "the disk" */
        if (t < 0.18f) env *= 1.0f + 0.55f * (1.0f - t / 0.18f);

        u32 c;
        if (t < 0.26f)      c = wp_lerp_color(hot,  warm, (int)(t / 0.26f * 256.0f));
        else if (t < 0.68f) c = wp_lerp_color(warm, cool, (int)((t - 0.26f) / 0.42f * 256.0f));
        else                c = wp_lerp_color(cool, rim,  (int)((t - 0.68f) / 0.32f * 256.0f));

        int e8 = (int)(env * 255.0f + 0.5f);
        if (e8 > 255) e8 = 255;                 /* RGB() does not clamp, and   */
        if (e8 < 0) e8 = 0;                     /* an overflow would carry into */
        bhgeo.lut[i] = RGB((u32)(GET_R(c) * e8 / 255),/* the next channel: this is */
                            (u32)(GET_G(c) * e8 / 255),/* what turned the disk rainbow */
                            (u32)(GET_B(c) * e8 / 255));

        float beta = sqrtf(0.5f / (r - 1.0f));
        if (beta > 0.75f) beta = 0.75f;
        bhgeo.bl[i] = beta;                     /* orbital speed, c        */
        bhgeo.gl[i] = sqrtf(1.0f - 1.0f / r);    /* gravitational redshift  */
        bhgeo.om[i] = sqrtf(0.5f / (r * r * r)); /* Keplerian omega         */
        bhgeo.sl[i] = 2.4f * logf(r / WP_BH_RIN);/* spiral pitch, radians   */
    }
    bhgeo.pal_accent = th->accent;
    bhgeo.pal_glow   = th->glow;
    bhgeo.pal_bottom = th->bg_bottom;
    bhgeo.pal_built  = 1;
}

/* ---- the star field ---------------------------------------------------
 * A deterministic 32 bit hash places the stars, so the same sky appears at
 * every resolution and on every boot. The layer is independent of the
 * theme, the clock and the view; it is the only picture this wallpaper
 * caches. Stars are not warped: a radial warp turns a point star into a
 * long scratch near the critical curve, and scratches are exactly the
 * speckle this background must not have. The photon ring carries the
 * strong-field light instead.
 */
static inline int wp_bh_star_at(float dx, float dy, float invca, float invra) {
    return wp_star_at(dx, dy, invca, 0x5EEDu, 15, 225, invra);
}

static void wp_bh_stars(int w, int h) {
    if (bhgeo.star && bhgeo.star_w == w && bhgeo.star_h == h) return;
    free(bhgeo.star);
    bhgeo.star = NULL;
    bhgeo.star_w = bhgeo.star_h = 0;
    u8 *st = (u8 *)malloc((size_t)w * (size_t)h);
    if (!st) return;                            /* fallback computes them */

    const float S = 0.5f * (float)(w < h ? w : h);
    const float cx = (float)w * 0.545f, cy = (float)h * 0.470f;
    const float invS = 1.0f / S;
    const float invca = 1.0f / 0.021f;
    const float invra = (S * 0.021f) / 0.80f;

    for (int y = 0; y < h; y++) {
        const float dy = ((float)y + 0.5f - cy) * invS;
        u8 *row = st + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const float dx = ((float)x + 0.5f - cx) * invS;
            row[x] = (u8)wp_bh_star_at(dx, dy, invca, invra);
        }
    }
    bhgeo.star = st;
    bhgeo.star_w = w;
    bhgeo.star_h = h;
}

static void wp_bh_grain(void) {
    if (bhgeo.grain_built) return;
    for (int i = 0; i < 64 * 64; i++) {
        u32 h = wp_h32((u32)(i & 63), (u32)(i >> 6), 0x51C7u);
        int n = (int)(h & 7u) - 3;              /* -3 .. 4 */
        if (n < -1) n = -1;
        if (n > 1) n = 1;
        bhgeo.grain[i] = (signed char)n;
    }
    bhgeo.grain_built = 1;
}

/* Paint the whole black hole at the surface's own resolution. Every pixel
 * goes through the same camera: no region is carved out for the disk. */
static void wp_bh_paint(Surface *s, const Theme *th, double seconds) {
    const int   W = s->w, H = s->h;
    const float S  = 0.5f * (float)(W < H ? W : H);      /* px per unit    */
    /* the hole sits a little off centre, which reads better as a desktop */
    const float cx = (float)W * 0.545f, cy = (float)H * 0.470f;
    const float ci = cosf(bh_yaw), si = sinf(bh_yaw);
    const float ci2 = ci * ci;

    if (!bhgeo.built) wp_bh_build();
    if (!bhgeo.sup_built) wp_bh_support();
    wp_bh_palette(th);
    wp_bh_stars(W, H);
    wp_bh_grain();

    const float t = (float)((seconds > 0.0) ? seconds : 0.0);
    const double tf = (double)t * (double)WP_BH_RATE + bh_spin;
    const float lspan = WP_BH_ROUT - WP_BH_RIN;

    /* per-radius, per-frame: the Keplerian clock and the two slow shimmer
       terms. Both are smooth in r, so they cannot draw rings. */
    float rot[WP_BH_NC], flick[WP_BH_NC], arc[WP_BH_NC];
    for (int i = 0; i < WP_BH_NC; i++) {
        float r = WP_BH_RIN + lspan * (float)i / (float)(WP_BH_NC - 1);
        rot[i] = fmodf((float)((double)bhgeo.om[i] * tf), WP_BH_TAU);
        flick[i] = 1.0f + 0.045f * sinf(t * 1.55f + r * 1.10f);
        arc[i]   = 1.0f + 0.060f * sinf(t * 1.05f + r * 0.55f + 1.2f);
    }
    const float ring_amp = 1.0f + 0.055f * sinf(t * 0.90f);
    const int   ring_ph  = (int)(t * 1.70f * 10430.378f);

    /* ---- display curve and sky ---- */
    const u8 *tone = bhgeo.tone;
    u32 sky_top = wp_opaque(color_mix(th->bg_top, RGB(0, 0, 0), 0.62f));
    u32 sky_bot = wp_opaque(color_mix(th->bg_bottom, RGB(0, 0, 0), 0.48f));

    const float invS = 1.0f / S;
    const float invca = 1.0f / 0.021f;
    const float invra = (S * 0.021f) / 0.80f;
    const float K = WP_BH_BC * WP_BH_DSC;
    /* Broad outer bound: no crossing can put light on a pixel whose impact
     * parameter exceeds the outermost crossing radius. This is a radial
     * test, not a projected ellipse: lensing moves a disk point away from
     * its flat projection, so an ellipse gate would clip real light and
     * leave a hard edge. Inside this circle, sup[psi] narrows the gate to
     * the actual reach of the disk at that sweep. */
    const float rdisk2 = (WP_BH_ROUT / K) * (WP_BH_ROUT / K);

    for (int y = 0; y < H; y++) {
        const float dy = ((float)y + 0.5f - cy) * invS;
        const float dy2 = dy * dy;
        int ty = (H > 1) ? (y * 256 + (H - 1) / 2) / (H - 1) : 0;
        if (ty > 256) ty = 256;
        const int bgr = wp_lerp8((int)GET_R(sky_top), (int)GET_R(sky_bot), ty);
        const int bgg = wp_lerp8((int)GET_G(sky_top), (int)GET_G(sky_bot), ty);
        const int bgb = wp_lerp8((int)GET_B(sky_top), (int)GET_B(sky_bot), ty);
        u32 *row = s->px + (size_t)y * s->stride;
        const u8 *srow = bhgeo.star ? bhgeo.star + (size_t)y * (size_t)W : NULL;

        for (int x = 0; x < W; x++) {
            const float dx = ((float)x + 0.5f - cx) * invS;
            const float dx2 = dx * dx;
            int R = bgr, G = bgg, B = bgb;
            int star = srow ? srow[x] : wp_bh_star_at(dx, dy, invca, invra);

            if (dx2 + dy2 <= rdisk2) {
                const float rho = sqrtf(dx2 + dy2);
                const float b = rho * K;

                if (b < WP_BH_BC) {
                    /* inside the shadow: no disk, no stars, nothing comes back */
                    R = 2; G = 2; B = 4;
                    star = 0;
                } else {
                    /* The sweep where this ray crosses the disk plane. It is
                     * a function of the screen angle only, so the same value
                     * also indexes the support bound below. */
                    const float psi = wp_atan2f(ci * rho, si * dy);
                    int pi = (int)(psi * ((float)WP_BH_SUPN / (float)WP_BH_PI));
                    if (pi < 0) pi = 0;
                    if (pi > WP_BH_SUPN - 1) pi = WP_BH_SUPN - 1;

                    /* sup[] is the largest impact parameter whose crossing at
                     * this psi can still be inside the disk. It is built once
                     * from the same geodesic table, with a margin, so it can
                     * only ever be wider than the lit region - never a clip
                     * and never a hard edge. */
                    if (b <= bhgeo.sup[pi]) {
                        const float az = wp_atan2f(dy, dx * ci);
                        /* the line-of-sight component of the orbital velocity,
                           exactly: the pixel's major-axis offset over its
                           projected radius, times sin(yaw) */
                        const float qz = sqrtf(dx2 * ci2 + dy2);
                        const float bm = (qz > 1e-9f) ? (dx * ci * si / qz) : 0.0f;
                        const float tt = wp_bh_bidx(b);
                        const float s0 = psi * ((float)WP_BH_NA / (float)WP_BH_DMAX) - 0.5f;
                        int   j0 = (int)s0;
                        const float fs = s0 - (float)j0;
                        /* the third crossing only exists within a few percent
                           of the critical curve, and is skipped elsewhere */
                        const int nc = (b < WP_BH_BC * 1.25f) ? 3 : 2;
                        int side = 1;

                        for (int k = 0; k < nc; k++, j0 += WP_BH_NS, side = -side) {
                            float r = wp_bh_radius(tt, j0, fs);
                            if (r < WP_BH_RIN || r > WP_BH_ROUT) continue;
                            int li = (int)((r - WP_BH_RIN) * ((float)(WP_BH_NC - 1) / lspan));
                            if (li < 0) li = 0;
                            if (li > WP_BH_NC - 1) li = WP_BH_NC - 1;

                            /* delta = sqrt(1-1/r) / (1 - beta_los), flux ~ delta^3 */
                            float dop = 1.0f - bm * (float)side * bhgeo.bl[li];
                            if (dop < 0.10f) dop = 0.10f;
                            float gain = bhgeo.gl[li] / dop;
                            gain = gain * gain * gain;
                            if (gain > 5.0f) gain = 5.0f;

                            /* trailing spiral: 2, 4 and 8 arm harmonics on the
                               Keplerian shear, so the inner disk runs ahead of
                               the rim instead of turning as one rigid plate */
                            float ph = az + bhgeo.sl[li] + rot[li]
                                       + (side < 0 ? (float)WP_BH_PI : 0.0f);
                            int ph16 = (int)(ph * 10430.378f);
                            int h2 = bhgeo.sinq[(ph16 >> 7) & 255];
                            int h4 = bhgeo.sinq[(ph16 >> 6) & 255];
                            int h8 = bhgeo.sinq[(ph16 >> 5) & 255];
                            int pat = 128 + ((h2 - 128) >> 1)
                                          + ((h4 - 128) >> 2)
                                          + ((h8 - 128) >> 3);
                            float f = flick[li] * (side < 0 ? arc[li] : 1.0f);
                            int inten = (int)(gain * (float)pat * f * 2.0f);

                            u32 c = bhgeo.lut[li];
                            R += ((int)GET_R(c) * inten) >> 8;
                            G += ((int)GET_G(c) * inten) >> 8;
                            B += ((int)GET_B(c) * inten) >> 8;
                        }
                    }

                    /* ---- photon ring: a thin bright halo just outside the
                       critical curve, where every crossing piles up. A slow
                       three-lobed shimmer runs round it without moving it. */
                    if (b < WP_BH_BC * WP_BH_RMAX) {
                        float q = (b - WP_BH_BC) / (WP_BH_BC * 0.055f);
                        float gr = 1.0f / (1.0f + q * q);
                        gr *= gr;
                        const float az = wp_atan2f(dy, dx * ci);
                        int az16 = (int)(az * 10430.378f);
                        int shim = bhgeo.sinq[(((az16 * 3) >> 8) + ring_ph) & 255];
                        float rm = ring_amp + (float)(shim - 128) * (0.035f / 128.0f);
                        int gi = (int)(gr * 185.0f * rm);
                        R += ((int)GET_R(bhgeo.hot) * gi) >> 8;
                        G += ((int)GET_G(bhgeo.hot) * gi) >> 8;
                        B += ((int)GET_B(bhgeo.hot) * gi) >> 8;
                    }
                }
            }
            /* stars sit behind the disk, not on it: fade them out by how
               much light the crossings already put on this pixel */
            if (star > 0) {
                int lit = R - bgr;
                if (G - bgg > lit) lit = G - bgg;
                if (B - bgb > lit) lit = B - bgb;
                if (lit >= 190) star = 0;
                else if (lit > 0) star -= (star * lit) / 190;
                R += star; G += star; B += star;
            }

            /* a little grain, so the smooth areas have some texture */
            int nz = bhgeo.grain[(((y & 63) << 6) | (x & 63))];
            R += nz; G += nz; B += nz;
            if (R < 0) R = 0; if (R > 1023) R = 1023;
            if (G < 0) G = 0; if (G > 1023) G = 1023;
            if (B < 0) B = 0; if (B > 1023) B = 1023;
            row[x] = 0xFF000000u | ((u32)tone[R] << 16) | ((u32)tone[G] << 8)
                   | (u32)tone[B];
        }
    }
}

/* One reusable trace surface. This is a working buffer, not a picture
 * cache: it is overwritten from the first pixel of every frame. */
static u32 *wp_bh_scratch(int w, int h) {
    if (bhgeo.scratch && bhgeo.scr_w == w && bhgeo.scr_h == h)
        return bhgeo.scratch;
    free(bhgeo.scratch);
    bhgeo.scratch = NULL;
    bhgeo.scr_w = bhgeo.scr_h = 0;
    u32 *buf = (u32 *)malloc((size_t)w * (size_t)h * sizeof(u32));
    if (!buf) return NULL;
    bhgeo.scratch = buf;
    bhgeo.scr_w = w;
    bhgeo.scr_h = h;
    return buf;
}

/* Weighted mix of two packed RGB pixels with the only weights a q = 2
 * bilinear tap uses: 1/4 and 3/4. The x3 is a shift and an add, no
 * multiply, and the row weights are hoisted out of the pixel loop so the
 * hot loops are branch-free. The R/B lanes stay 16 bits apart and the
 * largest sum is 1020, so no lane can carry into the next. */
static inline u32 wp_bh_m1(u32 a, u32 b) {
    u32 ra = a & 0x00FF00FFu, rb = b & 0x00FF00FFu;
    u32 ga = a & 0x0000FF00u, gb = b & 0x0000FF00u;
    u32 srb = ra + (ra << 1) + rb + 0x00020002u;
    u32 sg  = ga + (ga << 1) + gb + 0x00000200u;
    return 0xFF000000u | (((srb >> 2) & 0xFFu) << 16)
                       | (((sg  >> 10) & 0xFFu) << 8)
                       |  ((srb >> 18) & 0xFFu);
}
static inline u32 wp_bh_m3(u32 a, u32 b) {
    u32 ra = a & 0x00FF00FFu, rb = b & 0x00FF00FFu;
    u32 ga = a & 0x0000FF00u, gb = b & 0x0000FF00u;
    u32 srb = rb + (rb << 1) + ra + 0x00020002u;
    u32 sg  = gb + (gb << 1) + ga + 0x00000200u;
    return 0xFF000000u | (((srb >> 2) & 0xFFu) << 16)
                       | (((sg  >> 10) & 0xFFu) << 8)
                       |  ((srb >> 18) & 0xFFu);
}

/* q = 2 upscale of the complete trace surface, separable so the vertical
 * blend is done once per source column and reused by both destination
 * columns. Same sampling grid and weights as wp_upscale(). */
static void wp_upscale2(Surface *dst, const u32 *src, int sw, int sh) {
    if (bhgeo.up_w != sw || !bhgeo.uprow) {
        free(bhgeo.uprow);
        bhgeo.uprow = (u32 *)malloc((size_t)sw * sizeof(u32));
        bhgeo.up_w = bhgeo.uprow ? sw : 0;
    }
    if (!bhgeo.uprow) { wp_upscale(dst, src, sw, sh, 2); return; }

    u32 *vrow = bhgeo.uprow;
    const int maxy = (sh - 1) * 256;
    int lasty = (sh > 1) ? sh - 2 : 0;
    const int lastx = (sw > 1) ? sw - 2 : 0;

    for (int y = 0; y < dst->h; y++) {
        int sy = 128 * y - 64;
        if (sy < 0) sy = 0;
        if (sy > maxy) sy = maxy;
        int iy = sy >> 8;
        if (iy > lasty) iy = lasty;
        int fy = sy & 255;
        int wv = (fy == 64) ? 1 : (fy == 192) ? 3 : 0;
        const u32 *r0 = src + (size_t)iy * sw;
        const u32 *r1 = src + (size_t)((iy < lasty) ? iy + 1 : iy) * sw;

        if (wv == 1) {
            for (int x = 0; x < sw; x++) vrow[x] = wp_bh_m1(r0[x], r1[x]);
        } else if (wv == 3) {
            for (int x = 0; x < sw; x++) vrow[x] = wp_bh_m3(r0[x], r1[x]);
        } else {
            memcpy(vrow, r0, (size_t)sw * sizeof(u32));
        }

        u32 *out = dst->px + (size_t)y * dst->stride;
        out[0] = vrow[0];
        if (sw > 1) {
            int x = 1;
            for (; x + 1 < dst->w; x += 2) {
                int ix = (x - 1) >> 1;               /* odd x: 1/4 */
                int i2 = x >> 1;                     /* even x+1: 3/4 */
                if (ix > lastx) out[x] = vrow[sw - 1];
                else            out[x] = wp_bh_m1(vrow[ix], vrow[ix + 1]);
                if (i2 > lastx) out[x + 1] = vrow[sw - 1];
                else            out[x + 1] = wp_bh_m3(vrow[i2], vrow[i2 + 1]);
            }
            if (x < dst->w) {
                int ix = (x - 1) >> 1;
                out[x] = (ix > lastx) ? vrow[sw - 1] : wp_bh_m1(vrow[ix], vrow[ix + 1]);
            }
        } else {
            for (int x = 1; x < dst->w; x++) out[x] = vrow[0];
        }
    }
}

/* Paint the black hole. Above WP_TRACE_MAX pixels the whole frame is traced
 * at 1/q resolution and scaled up, so the cost stays flat as screens grow.
 * The trace covers the complete surface - the disk is not clipped to a
 * rectangle - and the upscale is a single bilinear pass, which is why there
 * is no seam where a "moving band" would otherwise meet a cached sky. */
static void wp_draw_blackhole(Surface *s, const Theme *th, double seconds) {
    int q = wp_q(s->w, s->h);
    int sw = (s->w + q - 1) / q, sh = (s->h + q - 1) / q;
    u32 *buf = wp_bh_scratch(sw, sh);
    if (!buf) { wp_bh_paint(s, th, seconds); return; }

    Surface tmp;
    tmp.px = buf; tmp.w = sw; tmp.h = sh; tmp.stride = sw;
    tmp.cx0 = tmp.cy0 = tmp.cx1 = tmp.cy1 = 0;
    wp_bh_paint(&tmp, th, seconds);
    if (q == 2) wp_upscale2(s, buf, sw, sh);
    else        wp_upscale(s, buf, sw, sh, q);
}

/* ---- the two knobs the desktop drives --------------------------------
 * Both are state changes only: the next wallpaper_draw() picks them up.
 * The clamps are in wallpaper.h, next to the documented drag/wheel map. */

void wallpaper_blackhole_view(float delta) {
    if (!(delta == delta)) return;                 /* ignore NaN */
    float y = bh_yaw + delta;
    if (y < WALLPAPER_BH_YAW_MIN) y = WALLPAPER_BH_YAW_MIN;
    if (y > WALLPAPER_BH_YAW_MAX) y = WALLPAPER_BH_YAW_MAX;
    bh_yaw = y;
}

void wallpaper_blackhole_spin(float delta) {
    if (!(delta == delta)) return;
    /* A double keeps the clock exact for any realistic amount of wheel
     * input. The per-radius phase is reduced modulo 2*pi in the paint, so
     * there is nothing to wrap here and therefore no jump in the pattern. */
    bh_spin += (double)delta;
}

void wallpaper_blackhole_reset(void) {
    bh_yaw  = WALLPAPER_BH_YAW_DEF;
    bh_spin = 0.0f;
}

float wallpaper_blackhole_yaw(void) {
    return bh_yaw;
}

/* ------------------------------------------------------------ 5. Nebula
 * Value-noise fbm, domain warped twice, in the theme's own two light
 * colours, with a soft core and a star field. Static: the desktop paints
 * it once. The noise is sampled in normalised coordinates, so the same
 * clouds appear at every resolution.
 */
static inline float wp_vnoise(float x, float y, u32 seed) {
    int ix = wp_ifloor(x), iy = wp_ifloor(y);
    float tx = x - (float)ix, ty = y - (float)iy;
    float sx = tx * tx * (3.0f - 2.0f * tx);
    float sy = ty * ty * (3.0f - 2.0f * ty);

    float a = (float)(wp_h32((u32)ix, (u32)iy, seed) & 0xFFFFFFu) * (1.0f / 16777215.0f);
    float b = (float)(wp_h32((u32)ix + 1u, (u32)iy, seed) & 0xFFFFFFu) * (1.0f / 16777215.0f);
    float c = (float)(wp_h32((u32)ix, (u32)iy + 1u, seed) & 0xFFFFFFu) * (1.0f / 16777215.0f);
    float d = (float)(wp_h32((u32)ix + 1u, (u32)iy + 1u, seed) & 0xFFFFFFu) * (1.0f / 16777215.0f);

    float ab = a + (b - a) * sx;
    float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}

static inline float wp_fbm(float x, float y, int oct, u32 seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < oct; i++) {
        sum += amp * wp_vnoise(x, y, seed + (u32)i * 131u);
        norm += amp;
        /* rotate by 37 degrees and double: no axis-aligned grain */
        float nx = 2.02f * (0.7986f * x - 0.6018f * y) + 11.3f;
        float ny = 2.02f * (0.6018f * x + 0.7986f * y) - 7.1f;
        x = nx; y = ny;
        amp *= 0.55f;
    }
    return sum / (norm > 0.0f ? norm : 1.0f);
}

static void wp_nebula_paint(Surface *s, const Theme *th, double seconds) {
    const int   W = s->w, H = s->h;
    const float S = 0.5f * (float)(W < H ? W : H);
    const float cx = (float)W * 0.42f, cy = (float)H * 0.58f;
    const float amp = th->dark ? 1.0f : 0.35f;

    u32 sky_top = wp_opaque(color_mix(th->bg_top, RGB(0, 0, 0), th->dark ? 0.55f : 0.05f));
    u32 sky_bot = wp_opaque(color_mix(th->bg_bottom, RGB(0, 0, 0), th->dark ? 0.35f : 0.02f));
    gfx_gradient(s, 0, 0, W, H, sky_top, sky_bot);

    const int cr = (int)GET_R(th->accent), cg = (int)GET_G(th->accent), cb = (int)GET_B(th->accent);
    const int dr = (int)GET_R(th->glow),   dg = (int)GET_G(th->glow),   db = (int)GET_B(th->glow);

    const float invS = 1.0f / S;
    for (int y = 0; y < H; y++) {
        const float dy = ((float)y + 0.5f - cy) * invS;
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) {
            const float dx = ((float)x + 0.5f - cx) * invS;
            const float u = dx * 2.15f, v = dy * 2.15f;

            /* two warped fbm fields: the first one is the density, the
               second one decides which of the two hues the cloud takes */
            float q = wp_fbm(u, v, 2, 0x4E0Bu);
            float f1 = wp_fbm(u + 2.10f * q - 3.4f, v + 2.10f * q + 5.7f, 4, 0x9E37u);
            float f2 = wp_fbm(u - 1.70f * q + 8.1f, v + 1.30f * q - 2.6f, 2, 0xB529u);

            /* density: a soft threshold, squared so the wisps are thin and
               most of the sky stays empty; the low frequency field q decides
               which half of the picture the cloud belongs to, so there are
               dark regions instead of an even marble */
            float d = (f1 - 0.40f) * 2.30f;
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            d = d * d * (3.0f - 2.0f * d);
            float reg = 0.35f + 1.30f * q * q;
            float m = d * d * amp * reg;

            /* hue: 0 -> glow (cool), 1 -> accent (hot) */
            float hh = (f2 - 0.30f) * 2.2f;
            if (hh < 0.0f) hh = 0.0f;
            if (hh > 1.0f) hh = 1.0f;

            int R = wp_lerp8(dr, cr, (int)(hh * 256.0f));
            int G = wp_lerp8(dg, cg, (int)(hh * 256.0f));
            int B = wp_lerp8(db, cb, (int)(hh * 256.0f));

            u32 base = row[x];
            int br = (int)GET_R(base), bg = (int)GET_G(base), bb = (int)GET_B(base);
            /* screen blend of a coloured light on a dark sky: the cloud adds
               light, it does not paint over the background */
            R = br + (int)((float)R * m * 1.45f);
            G = bg + (int)((float)G * m * 1.45f);
            B = bb + (int)((float)B * m * 1.45f);

            /* a soft bright core, dust-lit from inside */
            float cd2 = dx * dx + dy * dy;
            float core = 1.0f / (1.0f + cd2 * 26.0f);
            core *= core;
            int ci8 = (int)(core * 150.0f * amp);
            R += ci8; G += ci8; B += (ci8 * 9) / 8;

            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            row[x] = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
    }

    (void)seconds;
}

/* Stars are drawn at full resolution, after the cloud has been scaled up, so
 * they stay points instead of blobs. */
static void wp_draw_nebula(Surface *s, const Theme *th) {
    const int   W = s->w, H = s->h;
    const float S = 0.5f * (float)(W < H ? W : H);
    const float cx = (float)W * 0.42f, cy = (float)H * 0.58f;
    const float invS = 1.0f / S;
    const float invca = 1.0f / 0.028f, invcb = 1.0f / 0.012f;
    const float invra = (S * 0.028f) / 0.85f, invrb = (S * 0.012f) / 0.70f;

    int q = wp_q(W, H);
    int sw = W, sh = H;
    u32 *buf = wp_scratch(W, H, q, &sw, &sh);
    if (!buf) {
        wp_nebula_paint(s, th, 0.0);
    } else {
        Surface tmp;
        tmp.px = buf; tmp.w = sw; tmp.h = sh; tmp.stride = sw;
        tmp.cx0 = tmp.cy0 = tmp.cx1 = tmp.cy1 = 0;
        wp_nebula_paint(&tmp, th, 0.0);
        wp_upscale(s, buf, sw, sh, q);
        free(buf);
    }

    for (int y = 0; y < H; y++) {
        const float dy = ((float)y + 0.5f - cy) * invS;
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) {
            const float dx = ((float)x + 0.5f - cx) * invS;
            int sa = wp_star_at(dx, dy, invca, 0x5EEDu, 15, 225, invra);
            int sb = wp_star_at(dx, dy, invcb, 0x51A2u, 30, 80,  invrb);
            int sv = sa + sb;
            if (!sv) continue;
            u32 c = row[x];
            int R = (int)GET_R(c) + sv, G = (int)GET_G(c) + (sv * 245) / 256,
                B = (int)GET_B(c) + (sv * 235) / 256;
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            row[x] = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
    }
}

/* ------------------------------------------------------------ public */

void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds) {
    if (!s || !s->px || s->w <= 0 || s->h <= 0) return;
    if (!th) {                                  /* never leave the screen dirty */
        surf_fill(s, 0, 0, s->w, s->h, RGB(0, 0, 0));
        return;
    }
    if (index < 0 || index >= WALLPAPER_COUNT) index = 0;

    switch (index) {
        case 1: wp_draw_mesh(s, th);                  break;
        case 2: wp_draw_waves(s, th, seconds);        break;
        case 3: wp_draw_orbits(s, th, seconds);       break;
        case 4: wp_draw_blackhole(s, th, seconds);    break;
        case 5: wp_draw_nebula(s, th);                break;
        case 0:
        default: wp_draw_dusk(s, th);                 break;
    }
}
