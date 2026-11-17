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
 * A Schwarzschild black hole seen nearly edge-on, with a thin accretion
 * disk traced through its own gravitational field. This is the one
 * wallpaper that is computed rather than painted:
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
 *  - Geometry: a pixel at sky offset (X, Y) fixes the photon's orbital
 *    plane; its line of nodes with the disk plane gives the angle psi0 at
 *    which the ray starts, and therefore the ray crosses the disk plane at
 *    sweep psi0, psi0 + pi and psi0 + 2pi. Each crossing is one table
 *    lookup: the direct image (the near side of the disk, including the
 *    part that passes in front of the shadow), the lensed image of the far
 *    side that arcs over and under the shadow, and the photon ring.
 *
 *    The disk itself is an ellipse on screen with aspect cos(yaw), where
 *    yaw is the angle between the line of sight and the disk axis, so an
 *    inclined view really foreshortens: at yaw -> 0 the disk opens into a
 *    full annulus, at yaw -> pi/2 it collapses to a line. The same yaw
 *    drives the Doppler term sin(yaw) * cos(azimuth).
 *
 *  - Animation: the disk is emissive, and its pattern is advected by the
 *    local Keplerian angular velocity omega(r) = sqrt(0.5/r^3), scaled by
 *    WP_BH_RATE. The inner rings therefore visibly outrun the rim - the
 *    differential rotation is what makes it read as orbiting matter and not
 *    as a rotating picture. The photon ring and the secondary (lensed)
 *    image carry a smaller, slower shimmer. All of it is a function of the
 *    `seconds` argument, which is why wallpaper_animated(4) is 1.
 *
 *  - Interaction: wallpaper_blackhole_view() and _spin() below are the two
 *    knobs the desktop can drive from input; see wallpaper.h for the exact
 *    drag and wheel mapping. The view is a real change of projection, not
 *    a scale.
 *
 *  - Relativistic beaming: a circular orbit at r moves at
 *    beta = sqrt(0.5/(r-1)), its line-of-sight component is
 *    cos(azimuth) * sin(yaw), and the observed flux is scaled by delta^3
 *    with delta = sqrt(1-1/r) / (1 - beta_los) (gravitational redshift and
 *    Doppler together). The factor is clamped to 9x so the receding side
 *    stays visible instead of going black.
 *
 *  - The shadow (b < 3*sqrt(3)/2) has no background: no stars, no ring on
 *    the far side. Background stars are sampled at the deflected sky
 *    position, so they bunch into arcs at the critical curve, and a soft
 *    photon-ring term sits just outside it.
 *
 *  - Cost: the sky - gradient, lensed stars, grain, the shadow - does not
 *    depend on time or on the view, so it is rasterised once per size and
 *    sky colour: a full-resolution copy (the desktop blits it and then
 *    draws the moving part over it) plus a trace-resolution copy stored as
 *    10:10:10 before the tone curve so the disk can add light to it. Per
 *    frame only the disk ellipse and the photon-ring circle are recomputed,
 *    and only those rows are upscaled from the 1/q trace. See wp_bh_draw().
 *
 *  The cached geodesic table is a pure function of constants (no theme, no
 *  time, no resolution), so the picture stays a pure function of its
 *  arguments.
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
#define WP_BH_NC    160                    /* disk palette entries       */
#define WP_BH_RATE  2.2f                   /* Keplerian phase per second */
#define WP_BH_RMAX  1.25f                  /* photon ring outer cut, x BC*/
#define WP_BH_SPAN  (WP_BH_RMAX / WP_BH_DSC) /* ring radius in S units   */

/* The view and the disk phase, driven by the desktop through the two
 * functions at the bottom of this section. Single-threaded compositor, so
 * plain floats are enough. */
static float bh_yaw  = WALLPAPER_BH_TILT_DEF;
static float bh_spin = 0.0f;

static struct {
    int   built;
    float sweep[WP_BH_NB];                 /* sweep to escape, radians   */
    unsigned short r[WP_BH_NB][WP_BH_NA];  /* crossing radius * 64       */
    int   sinq[256];
    u8    tone[1024];                      /* display curve, 0..1023     */

    /* still sky, keyed by size and the two sky colours */
    int   cw, ch, cq;                      /* trace size and 1/q         */
    u32   key_top, key_bot;
    u32  *bg;                              /* W*H, tone mapped           */
    u32  *pre;                             /* cw*ch, packed 10:10:10     */
    u32  *canvas;                          /* cw*ch, the moving part     */
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

static inline float wp_bh_sweep(float b) {
    float t = wp_bh_bidx(b);
    if (t < 0.0f) return 0.0f;
    if (t >= (float)(WP_BH_NB - 1)) return (float)WP_BH_PI;
    int i = (int)t;
    float ft = t - (float)i;
    return bhgeo.sweep[i] + (bhgeo.sweep[i + 1] - bhgeo.sweep[i]) * ft;
}

/* ---- the still sky ------------------------------------------------
 * Everything that does not move: the gradient, the lensed stars, the
 * shadow and the grain. Packed 10:10:10 so the disk can add light to it
 * before the display curve. Pure: used both for the cached copies and for
 * the no-cache fallback.
 */
static inline u32 wp_bh_pack(int R, int G, int B) {
    if (R < 0) R = 0; if (R > 1023) R = 1023;
    if (G < 0) G = 0; if (G > 1023) G = 1023;
    if (B < 0) B = 0; if (B > 1023) B = 1023;
    return (u32)R | ((u32)G << 10) | ((u32)B << 20);
}

static inline u32 wp_bh_sky(float dx, float dy, float rho, float b,
                            int bgr, int bgg, int bgb,
                            float invca, float invra, u32 grain) {
    int R = bgr, G = bgg, B = bgb;
    if (b < WP_BH_BC) {
        /* inside the shadow: no disk, no stars, nothing comes back */
        R = 2; G = 2; B = 4;
    } else {
        /* background stars, at the deflected position */
        float defl = wp_bh_sweep(b) - (float)WP_BH_PI;
        if (defl < 0.0f) defl = 0.0f;
        float wrp = defl * 0.22f / (rho + 1e-6f);   /* one divide, both axes */
        int star = wp_star_at(dx + dx * wrp, dy + dy * wrp,
                              invca, 0x5EEDu, 15, 225, invra);
        R += star; G += star; B += star;
    }
    /* a little grain, so the smooth areas have some texture */
    int nz = (int)(grain & 15u) - 7;
    R += nz >> 2; G += nz >> 2; B += nz >> 2;
    return wp_bh_pack(R, G, B);
}

/* Build (or keep) the two sky copies. Keyed by the size and the two colours
 * the sky is made of, so a theme switch rebuilds it and nothing else does. */
static void wp_bh_cache_build(int W, int H, const Theme *th) {
    if (!bhgeo.built) wp_bh_build();

    int q = wp_q(W, H);
    int tw = (W + q - 1) / q, thh = (H + q - 1) / q;
    if (bhgeo.bg && bhgeo.pre && bhgeo.canvas &&
        bhgeo.cw == tw && bhgeo.ch == thh && bhgeo.cq == q &&
        bhgeo.key_top == th->bg_top && bhgeo.key_bot == th->bg_bottom) return;

    free(bhgeo.bg); free(bhgeo.pre); free(bhgeo.canvas);
    bhgeo.bg = NULL; bhgeo.pre = NULL; bhgeo.canvas = NULL;
    bhgeo.cw = bhgeo.ch = bhgeo.cq = 0;
    bhgeo.key_top = bhgeo.key_bot = 0;

    u32 *bg  = (u32 *)malloc((size_t)W * (size_t)H * 4);
    u32 *pre = (u32 *)malloc((size_t)tw * (size_t)thh * 4);
    u32 *can = (u32 *)malloc((size_t)tw * (size_t)thh * 4);
    if (!bg || !pre || !can) { free(bg); free(pre); free(can); return; }

    bhgeo.bg = bg; bhgeo.pre = pre; bhgeo.canvas = can;
    bhgeo.cw = tw; bhgeo.ch = thh; bhgeo.cq = q;
    bhgeo.key_top = th->bg_top; bhgeo.key_bot = th->bg_bottom;

    const float St = 0.5f * (float)(tw < thh ? tw : thh);
    const float cxt = (float)tw * 0.545f, cyt = (float)thh * 0.470f;
    /* one star size for both copies: the trace copy is what gets upscaled,
       so measuring the stars on it keeps the two pictures consistent */
    const float invca = 1.0f / 0.021f;
    const float invra = (St * 0.021f) / 0.80f;

    u32 sky_top = wp_opaque(color_mix(th->bg_top, RGB(0, 0, 0), 0.62f));
    u32 sky_bot = wp_opaque(color_mix(th->bg_bottom, RGB(0, 0, 0), 0.48f));

    for (int y = 0; y < thh; y++) {
        const float dy = ((float)y + 0.5f - cyt) / St;
        int ty = (thh > 1) ? (y * 256 + (thh - 1) / 2) / (thh - 1) : 0;
        if (ty > 256) ty = 256;
        const int bgr = wp_lerp8((int)GET_R(sky_top), (int)GET_R(sky_bot), ty);
        const int bgg = wp_lerp8((int)GET_G(sky_top), (int)GET_G(sky_bot), ty);
        const int bgb = wp_lerp8((int)GET_B(sky_top), (int)GET_B(sky_bot), ty);
        u32 *row = pre + (size_t)y * tw;
        for (int x = 0; x < tw; x++) {
            const float dx = ((float)x + 0.5f - cxt) / St;
            const float rho = sqrtf(dx * dx + dy * dy);
            const float b = rho * (WP_BH_BC * WP_BH_DSC);
            row[x] = wp_bh_sky(dx, dy, rho, b, bgr, bgg, bgb, invca, invra,
                               wp_h32((u32)x, (u32)y, 0x51C7u));
        }
    }

    /* The full-resolution copy is the same trace sky upscaled, not a second
     * rasterisation: that way the moving band pasted over it later lands on
     * exactly the pixels it would have had, and no seam shows where the
     * upscale stops. `can` is free until the first frame, so it is the
     * tone-mapped source here. */
    const size_t n = (size_t)tw * (size_t)thh;
    for (size_t i = 0; i < n; i++) {
        u32 sk = pre[i];
        can[i] = 0xFF000000u
                  | ((u32)bhgeo.tone[sk & 1023u] << 16)
                  | ((u32)bhgeo.tone[(sk >> 10) & 1023u] << 8)
                  | (u32)bhgeo.tone[(sk >> 20) & 1023u];
    }
    if (q == 1) {
        memcpy(bg, can, n * 4);
    } else {
        Surface ds, ss;
        memset(&ds, 0, sizeof ds);
        memset(&ss, 0, sizeof ss);
        ds.px = bg; ds.w = W; ds.h = H; ds.stride = W;
        ss.px = can; ss.w = tw; ss.h = thh; ss.stride = tw;
        surf_unclip(&ds);
        wp_upscale(&ds, can, tw, thh, q);
    }
}

/* Everything the disk needs that is constant across one frame. */
typedef struct {
    float ci, si, ci2, invci, lspan;
    u32   lut[WP_BH_NC];
    float bl[WP_BH_NC];      /* orbital speed, c                  */
    float gr[WP_BH_NC];      /* sqrt(1 - 1/r), gravitational part  */
    float rot[WP_BH_NC];     /* Keplerian phase at this instant    */
    float sl[WP_BH_NC];      /* spiral pitch, radians              */
    float flick[WP_BH_NC];   /* slow brightness breathing          */
    float arc[WP_BH_NC];     /* shimmer of the lensed image        */
    u32   hot;
    float ring_amp;
    int   ring_ph;
} BhPal;

static void wp_bh_view_init(BhPal *v, const Theme *th, double seconds) {
    const float t = (float)((seconds > 0.0) ? seconds : 0.0);
    const float tf = t * WP_BH_RATE;

    v->ci = cosf(bh_yaw);
    v->si = sinf(bh_yaw);
    v->ci2 = v->ci * v->ci;
    v->invci = 1.0f / v->ci;
    v->lspan = WP_BH_ROUT - WP_BH_RIN;

    u32 hot  = wp_opaque(color_mix(th->accent, RGB(0xFF, 0xF4, 0xDE), 0.62f));
    u32 warm = wp_opaque(th->accent);
    u32 cool = wp_opaque(color_mix(th->accent, th->glow, 0.62f));
    u32 rim  = wp_opaque(color_mix(th->glow, th->bg_bottom, 0.45f));
    v->hot = hot;

    for (int i = 0; i < WP_BH_NC; i++) {
        float tt = (float)i / (float)(WP_BH_NC - 1);
        float r = WP_BH_RIN + v->lspan * tt;
        /* r^-1 emissivity, with a soft inner lip and a soft outer rim so
           the disk does not end in a hard edge */
        float env = 1.65f * (WP_BH_RIN / r);
        float lip = (r < WP_BH_RIN * 1.22f) ? (r - WP_BH_RIN) / (0.22f * WP_BH_RIN) : 1.0f;
        if (lip < 0.0f) lip = 0.0f;
        float rimf = (tt > 0.80f) ? (1.0f - (tt - 0.80f) / 0.20f) : 1.0f;
        if (rimf < 0.0f) rimf = 0.0f;
        env *= lip * rimf * rimf;

        /* a narrow bright inner edge: the hottest, fastest material sits at
           the ISCO and that is what the eye reads as "the disk" */
        if (tt < 0.18f) env *= 1.0f + 0.55f * (1.0f - tt / 0.18f);

        u32 c;
        if (tt < 0.26f)      c = wp_lerp_color(hot,  warm, (int)(tt / 0.26f * 256.0f));
        else if (tt < 0.68f) c = wp_lerp_color(warm, cool, (int)((tt - 0.26f) / 0.42f * 256.0f));
        else                 c = wp_lerp_color(cool, rim,  (int)((tt - 0.68f) / 0.32f * 256.0f));

        int e8 = (int)(env * 255.0f + 0.5f);
        if (e8 > 255) e8 = 255;                 /* RGB() does not clamp, and   */
        if (e8 < 0) e8 = 0;                     /* an overflow would carry into */
        v->lut[i] = RGB((u32)(GET_R(c) * e8 / 255),/* the next channel: this is */
                        (u32)(GET_G(c) * e8 / 255),/* what turned the disk rainbow */
                        (u32)(GET_B(c) * e8 / 255));

        float beta = sqrtf(0.5f / (r - 1.0f));
        if (beta > 0.75f) beta = 0.75f;
        v->bl[i] = beta;
        v->gr[i] = sqrtf(1.0f - 1.0f / r);
        v->rot[i] = sqrtf(0.5f / (r * r * r)) * tf + bh_spin;
        v->sl[i] = 2.4f * logf(r / WP_BH_RIN);

        /* turbulence and lensing shimmer: slow, and only a few percent, so
           the disk keeps its shape while it moves */
        v->flick[i] = 1.0f + 0.055f * sinf(t * 1.90f + r * 1.7f);
        v->arc[i]   = 1.0f + 0.050f * sinf(t * 1.45f + r * 0.9f);
    }
    v->ring_amp = 1.0f + 0.030f * sinf(t * 1.10f);
    v->ring_ph  = (int)(t * 2.4f * 10430.378f);
}

/* Add the disk and the photon ring at one pixel. (dx, dy) are in units of
 * S = half the short side, the same units the geometry above uses. */
static inline void wp_bh_light(const BhPal *v, float dx, float dy,
                               int *R, int *G, int *B) {
    const float rho = sqrtf(dx * dx + dy * dy);
    const float b = rho * (WP_BH_BC * WP_BH_DSC);
    const float az = wp_atan2f(dy, dx * v->ci);
    /* the disk can only appear inside the projected ellipse of its outer
       radius; the ring circle is a separate, much smaller footprint */
    const float rmax = WP_BH_ROUT / (WP_BH_BC * WP_BH_DSC);
    const float ex = dx, ey = dy * v->invci;

    if (ex * ex + ey * ey <= rmax * rmax) {
        const float qn = sqrtf(dx * dx * v->ci2 + dy * dy);
        /* the line-of-sight component of the orbital velocity:
           cos(azimuth) sin(yaw) */
        const float bm = (qn > 1e-9f) ? (dx * v->ci * v->si / qn) : 0.0f;
        /* psi is the sweep at which this ray crosses the disk plane */
        const float psi = wp_atan2f(v->ci * rho, v->si * dy);
        const float t   = wp_bh_bidx(b);
        const float s0  = psi * ((float)WP_BH_NA / (float)WP_BH_DMAX) - 0.5f;
        int   j0 = (int)s0;
        const float fs = s0 - (float)j0;
        /* the third crossing only exists within a few percent of the
           critical curve, so it is gated on b and skipped for most pixels */
        const int nc = (b < WP_BH_BC * 1.25f) ? 3 : 2;
        int side = 1;

        for (int k = 0; k < nc; k++, j0 += WP_BH_NS, side = -side) {
            float r = wp_bh_radius(t, j0, fs);
            if (r < WP_BH_RIN || r > WP_BH_ROUT) continue;
            int li = (int)((r - WP_BH_RIN) * ((float)(WP_BH_NC - 1) / v->lspan));
            if (li < 0) li = 0;
            if (li > WP_BH_NC - 1) li = WP_BH_NC - 1;

            /* delta = sqrt(1-1/r) / (1 - beta_los), flux ~ delta^3 */
            float dop = 1.0f - bm * (float)side * v->bl[li];
            if (dop < 0.10f) dop = 0.10f;
            float gain = v->gr[li] / dop;
            gain = gain * gain * gain;
            if (gain > 4.5f) gain = 4.5f;

            /* trailing spiral: two, four and eight arm harmonics on a
               Keplerian shear, so the inner disk runs ahead of the rim */
            float ph = az + v->sl[li] + v->rot[li] + (side < 0 ? (float)WP_BH_PI : 0.0f);
            int ph16 = (int)(ph * 10430.378f);
            int h2 = bhgeo.sinq[(ph16 >> 7) & 255];
            int h4 = bhgeo.sinq[(ph16 >> 6) & 255];
            int h8 = bhgeo.sinq[(ph16 >> 5) & 255];
            int pat = 128 + ((h2 - 128) >> 1) + ((h4 - 128) >> 2) + ((h8 - 128) >> 3);
            float f = v->flick[li] * (side < 0 ? v->arc[li] : 1.0f);
            int inten = (int)(gain * (float)pat * f * 2.0f);

            u32 c = v->lut[li];
            *R += ((int)GET_R(c) * inten) >> 8;
            *G += ((int)GET_G(c) * inten) >> 8;
            *B += ((int)GET_B(c) * inten) >> 8;
        }
    }

    /* ---- photon ring: a thin bright halo just outside the critical curve,
       where every crossing piles up. A slow three-lobed shimmer runs round
       it so it is alive without changing its radius. */
    if (b >= WP_BH_BC && b < WP_BH_BC * WP_BH_RMAX) {
        float tt = (b - WP_BH_BC) / (WP_BH_BC * 0.055f);
        float gr = 1.0f / (1.0f + tt * tt);
        gr *= gr;
        int az16 = (int)(az * 10430.378f);
        int shim = bhgeo.sinq[(((az16 * 3) >> 8) + v->ring_ph) & 255];
        float rm = v->ring_amp + (float)(shim - 128) * (0.05f / 128.0f);
        int gi = (int)(gr * 185.0f * rm);
        *R += ((int)GET_R(v->hot) * gi) >> 8;
        *G += ((int)GET_G(v->hot) * gi) >> 8;
        *B += ((int)GET_B(v->hot) * gi) >> 8;
    }
}

/* Bilinear 1/q upscale of one destination row segment, on the same sampling
 * grid wp_upscale() uses (so the moving part lines up with the sky it is
 * pasted over). */
static void wp_up_row(u32 *dstrow, const u32 *src, int sw, int sh, int q,
                      int py, int x0, int x1, int ymin, int ymax) {
    const int step = 256 / q;
    const int maxx = (sw - 1) * 256;
    const int lastx = (sw > 1) ? sw - 2 : 0;
    const int lasty = (sh > 1) ? sh - 2 : 0;
    int sy = ((2 * py + 1) * 128) / q - 128;
    if (sy < 0) sy = 0;
    if (sy > (sh - 1) * 256) sy = (sh - 1) * 256;
    int iy = sy >> 8;
    if (iy < ymin) iy = ymin;
    if (iy > ymax - 1) iy = ymax - 1;
    if (iy > lasty) iy = lasty;
    int fy = sy & 255, ify = 256 - fy;
    const u32 *r0 = src + (size_t)iy * sw;
    const u32 *r1 = src + (size_t)((iy + 1 <= lasty) ? iy + 1 : iy) * sw;
    int sx = ((2 * x0 + 1) * 128) / q - 128;

    for (int x = x0; x <= x1; x++, sx += step) {
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
        dstrow[x] = 0xFF000000u | ((u32)rr << 16) | ((u32)rg << 8) | (u32)rb;
    }
}

/* No-cache path: only taken when the 5 MB of sky copies cannot be had.
 * Correct, just slower - it recomputes the sky per pixel. */
static void wp_bh_uncached(Surface *s, const Theme *th, const BhPal *v) {
    const int W = s->w, H = s->h;
    const float S = 0.5f * (float)(W < H ? W : H);
    const float cx = (float)W * 0.545f, cy = (float)H * 0.470f;
    const float invca = 1.0f / 0.021f;
    const float invra = (S * 0.021f) / 0.80f;
    u32 sky_top = wp_opaque(color_mix(th->bg_top, RGB(0, 0, 0), 0.62f));
    u32 sky_bot = wp_opaque(color_mix(th->bg_bottom, RGB(0, 0, 0), 0.48f));

    for (int y = 0; y < H; y++) {
        const float dy = ((float)y + 0.5f - cy) / S;
        int ty = (H > 1) ? (y * 256 + (H - 1) / 2) / (H - 1) : 0;
        if (ty > 256) ty = 256;
        const int bgr = wp_lerp8((int)GET_R(sky_top), (int)GET_R(sky_bot), ty);
        const int bgg = wp_lerp8((int)GET_G(sky_top), (int)GET_G(sky_bot), ty);
        const int bgb = wp_lerp8((int)GET_B(sky_top), (int)GET_B(sky_bot), ty);
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) {
            const float dx = ((float)x + 0.5f - cx) / S;
            const float rho = sqrtf(dx * dx + dy * dy);
            const float b = rho * (WP_BH_BC * WP_BH_DSC);
            u32 sk = wp_bh_sky(dx, dy, rho, b, bgr, bgg, bgb, invca, invra,
                               wp_h32((u32)x, (u32)y, 0x51C7u));
            int R = (int)(sk & 1023u);
            int G = (int)((sk >> 10) & 1023u);
            int B = (int)((sk >> 20) & 1023u);
            wp_bh_light(v, dx, dy, &R, &G, &B);
            if (R < 0) R = 0; if (R > 1023) R = 1023;
            if (G < 0) G = 0; if (G > 1023) G = 1023;
            if (B < 0) B = 0; if (B > 1023) B = 1023;
            row[x] = 0xFF000000u
                   | ((u32)bhgeo.tone[R] << 16)
                   | ((u32)bhgeo.tone[G] << 8)
                   | (u32)bhgeo.tone[B];
        }
    }
}

/* Paint the black hole. The still sky comes from the cache, the disk and
 * the ring are recomputed only where they can reach, and those rows are the
 * only ones upscaled. Above WP_TRACE_MAX pixels the moving part is traced at
 * 1/q resolution; it is a smooth picture and the cost has to stay flat. */
static void wp_draw_blackhole(Surface *s, const Theme *th, double seconds) {
    const int W = s->w, H = s->h;
    BhPal v;
    wp_bh_view_init(&v, th, seconds);
    wp_bh_cache_build(W, H, th);

    if (!bhgeo.bg || !bhgeo.pre || !bhgeo.canvas) {
        wp_bh_uncached(s, th, &v);
        return;
    }

    /* 1. the still sky. The compositor owns the framebuffer and repaints
     *    the whole background every frame, so this copy is the wallpaper. */
    if (s->stride == W) {
        memcpy(s->px, bhgeo.bg, (size_t)W * (size_t)H * 4);
    } else {
        for (int y = 0; y < H; y++)
            memcpy(s->px + (size_t)y * s->stride, bhgeo.bg + (size_t)y * W,
                   (size_t)W * 4);
    }

    /* 2. the moving part, at trace resolution. Its footprint is the disk
     *    ellipse plus the photon-ring circle; both are inside a rectangle
     *    whose half width is the larger of the two, so every canvas row in
     *    the band is filled edge to edge and the upscale never reads stale
     *    pixels. */
    const int q = bhgeo.cq, tw = bhgeo.cw, thh = bhgeo.ch;
    const float St = 0.5f * (float)(tw < thh ? tw : thh);
    const float cxt = (float)tw * 0.545f, cyt = (float)thh * 0.470f;
    const float Rmax = WP_BH_ROUT / (WP_BH_BC * WP_BH_DSC);   /* S units */
    const float invSt = 1.0f / St;
    const float invci = 1.0f / v.ci;
    const float Rmax2 = Rmax * Rmax;
    const float span2 = WP_BH_SPAN * WP_BH_SPAN;
    /* the band the moving part can reach: the disk ellipse is wide and
     * short (its half height is cos(yaw) * Rmax), the photon ring is a
     * small circle, so the union is a rectangle across the disk's width */
    const float yhalf = (v.ci * Rmax > WP_BH_SPAN ? v.ci * Rmax : WP_BH_SPAN) * St;
    int ylo = (int)(cyt - yhalf) - 2, yhi = (int)(cyt + yhalf) + 2;
    if (ylo < 0) ylo = 0;
    if (yhi > thh - 1) yhi = thh - 1;
    int x0 = (int)(cxt - Rmax * St) - 2, x1 = (int)(cxt + Rmax * St) + 2;
    if (x0 < 0) x0 = 0;
    if (x1 > tw - 1) x1 = tw - 1;

    for (int y = ylo; y <= yhi; y++) {
        const float dy = ((float)y + 0.5f - cyt) * invSt;
        const float dy2 = dy * dy;
        u32 *row = bhgeo.canvas + (size_t)y * tw;
        for (int x = x0; x <= x1; x++) {
            const float dx = ((float)x + 0.5f - cxt) * invSt;
            u32 sk = bhgeo.pre[(size_t)y * tw + x];
            int R = (int)(sk & 1023u);
            int G = (int)((sk >> 10) & 1023u);
            int B = (int)((sk >> 20) & 1023u);
            /* cheap gate before the two atan2: is this pixel on the disk
             * ellipse, or inside the photon-ring circle? */
            float ei = dy * invci;
            if (dx * dx + ei * ei <= Rmax2 || dx * dx + dy2 <= span2)
                wp_bh_light(&v, dx, dy, &R, &G, &B);
            if (R < 0) R = 0; if (R > 1023) R = 1023;
            if (G < 0) G = 0; if (G > 1023) G = 1023;
            if (B < 0) B = 0; if (B > 1023) B = 1023;
            row[x] = 0xFF000000u
                   | ((u32)bhgeo.tone[R] << 16)
                   | ((u32)bhgeo.tone[G] << 8)
                   | (u32)bhgeo.tone[B];
        }
    }

    /* 3. paste the rows the moving part touched back over the still sky */
    int py0 = ylo * q - q, py1 = (yhi + 1) * q + q;
    if (py0 < 0) py0 = 0;
    if (py1 > H - 1) py1 = H - 1;
    int dx0 = x0 * q - q - 2, dx1 = (x1 + 1) * q + q + 2;
    if (dx0 < 0) dx0 = 0;
    if (dx1 > W - 1) dx1 = W - 1;
    for (int py = py0; py <= py1; py++)
        wp_up_row(s->px + (size_t)py * s->stride, bhgeo.canvas, tw, thh, q,
                  py, dx0, dx1, ylo, yhi + 1);
}

/* ---- the two knobs the desktop drives ---------------------------- */

void wallpaper_blackhole_view(float delta) {
    if (!(delta == delta)) return;                 /* ignore NaN */
    float y = bh_yaw + delta;
    if (y < WALLPAPER_BH_TILT_MIN) y = WALLPAPER_BH_TILT_MIN;
    if (y > WALLPAPER_BH_TILT_MAX) y = WALLPAPER_BH_TILT_MAX;
    bh_yaw = y;
}

void wallpaper_blackhole_spin(float delta) {
    if (!(delta == delta)) return;
    bh_spin = fmodf(bh_spin + delta, 6.28318530718f);
    if (bh_spin < 0.0f) bh_spin += 6.28318530718f;
}

void wallpaper_blackhole_reset(void) {
    bh_yaw = WALLPAPER_BH_TILT_DEF;
    bh_spin = 0.0f;
}

float wallpaper_blackhole_tilt(void) {
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
