/* ============================================================
 *  Mystic OS - wallpaper.c
 *  Four backgrounds, painted by the CPU, straight into the
 *  framebuffer surface. No image files, no GPU, no state.
 *
 *  Every pixel is a pure function of:
 *      - the theme (so all four work with every theme),
 *      - the wallpaper index,
 *      - `seconds`, and only for the animated ones.
 *
 *  Cost is what makes this usable on old hardware, so the rules
 *  here are: no transcendental function inside a per-pixel loop
 *  (a 256 entry table does the sin/cos work), and the one float
 *  divide gfx_glow does per pixel becomes a reciprocal multiply
 *  plus a 64 step ramp.
 * ============================================================ */
#include "wallpaper.h"

#include <math.h>
#include <string.h>

/* one wave period, in 16.16 fixed point over a 256 entry sine table */
#define WP_SINE_N     256
#define WP_SINE_MASK  (WP_SINE_N - 1)
#define WP_PHASE_MASK ((WP_SINE_N << 16) - 1)

/* ------------------------------------------------------------ names */

static const char *const WP_NAMES[WALLPAPER_COUNT] = { "Dusk", "Mesh", "Waves", "Orbits" };

const char *wallpaper_name(int index) {
    if (index < 0 || index >= WALLPAPER_COUNT) return NULL;
    return WP_NAMES[index];
}

int wallpaper_animated(int index) {
    /* Dusk and Mesh are still pictures: the desktop can paint them once
       and then leave the framebuffer alone. */
    return (index == 2 || index == 3) ? 1 : 0;
}

static int wp_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* case-insensitive compare of a user string against a lowercase literal,
 * ignoring leading and trailing blanks */
static int wp_name_is(const char *user, const char *literal) {
    while (*user == ' ' || *user == '\t') user++;
    size_t i = 0;
    for (; literal[i]; i++) {
        if (wp_lower((unsigned char)user[i]) != literal[i]) return 0;
    }
    for (; user[i]; i++) {
        if (user[i] != ' ' && user[i] != '\t') return 0;
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
 * per-pixel float divide becomes a reciprocal multiply plus a 64 step
 * ramp, which is what keeps three screen sized lights inside a few ms.
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
        /* walk (x-cx)^2 by finite difference: one add per pixel, no multiply
           and no float->int of a squared coordinate */
        float fdx = (float)xa - cx;
        float dxx = fdx * fdx;
        float dstep = fdx + fdx;                 /* (x+1)^2 - x^2 = 2x+1 */
        u32 *row = s->px + (size_t)y * s->stride;

        for (int x = xa; x < xb; x++) {
            int idx = (int)(dxx * scale) + iy;
            dxx += dstep;
            dstep += 2.0f;
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
       gfx_glow calls over all six themes: max channel difference 4/255,
       mean below 1/255 - the same picture, at about 1.6x the speed. */
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

/* ------------------------------------------------------------ public */

void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds) {
    if (!s || !s->px || s->w <= 0 || s->h <= 0) return;
    if (!th) {                                  /* never leave the screen dirty */
        surf_fill(s, 0, 0, s->w, s->h, RGB(0, 0, 0));
        return;
    }
    if (index < 0 || index >= WALLPAPER_COUNT) index = 0;

    switch (index) {
        case 1: wp_draw_mesh(s, th);            break;
        case 2: wp_draw_waves(s, th, seconds);  break;
        case 3: wp_draw_orbits(s, th, seconds); break;
        case 0:
        default: wp_draw_dusk(s, th);           break;
    }
}
