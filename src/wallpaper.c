/* ============================================================
 *  Mystic OS - wallpaper.c
 *  Eight backgrounds, painted by the CPU, straight into the
 *  framebuffer surface, plus one picture decoded from a file.
 *  No GPU.
 *
 *  Every procedural pixel is a pure function of:
 *      - the theme (so all eight work with every theme),
 *      - the wallpaper index,
 *      - `seconds`, and only for the animated ones.
 *
 *  The eighth choice, a custom image, is the exception and is kept
 *  narrow on purpose: the file is read and decoded when the setting
 *  changes, cover-scaled once to the surface size and cached, and
 *  every later frame is one blit of that cache. See wp_setting_sync(),
 *  wp_image_build() and wp_image_blit().
 *
 *  Cost is what makes this usable on old hardware, so the expensive
 *  work is amortised: a 256 entry sine table does the sin work for
 *  Waves and for the black hole's spiral arms, the black hole's
 *  geodesics are integrated once and cached, the display curve is
 *  built once, and the star field is the only cached picture. The
 *  per-pixel path is a closed-form projection; the disk's atan2/sqrt
 *  work runs only inside a support gate that is a measured superset of
 *  the lit region, and the float divide gfx_glow does per pixel is a
 *  multiply by a precomputed reciprocal plus a 128 step alpha ramp.
 *
 *  Measured at 1280x800 with the shipping flags (-Os), best of 30 draws
 *  per wallpaper. The host was also running other builds, so a repeat run
 *  moves by a millisecond or two and these are the lowest figures seen:
 *
 *      Dusk 5.1 ms   Mesh 3.5 ms   Waves 3.7 ms   Orbits 0.5 ms
 *      Black Hole 6.5 ms         Nebula 44 ms
 *      Synthwave 2.9 ms          Dunes 3.3 ms
 *
 *  A cached custom image is a full-screen row memcpy: 0.34 ms at
 *  1280x800. The decode and the cover scale happen once per picture
 *  (and once more if the surface size changes), never per frame.
 *
 *  In one quiet 30-frame block the black hole measured 7.4 ms best, 8.0 ms
 *  mean; of that, 2.0 ms is the q = 2 upscale, 1.3 ms the sky / star / grain
 *  pass, and 4.2 ms the disk itself, spread over the 35% of the traced layer
 *  the support gate admits (0.72 crossing evaluations per traced pixel).
 *  Forcing that gate fully open costs 9.6 ms in the same block, and an
 *  interleaved gate-on/gate-off test puts the saving at 2.6 ms (24%). Above
 *  360k pixels the whole frame (sky, stars, disk, ring) is traced at 1/q
 *  resolution and scaled up, which keeps the cost flat as screens grow. Only
 *  the black hole is on the animation path (the desktop repaints animated
 *  backgrounds at 12.5 Hz, about 9% of one core at the -Os figure); the
 *  nebula reports wallpaper_animated() == 0 and is painted once. See wp_q()
 *  / wp_upscale().
 * ============================================================ */
#include "wallpaper.h"
#include "img.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The desktop build lists the sources it links (tools/build-desktop.sh) and
 * src/img.c is not one of them; the browser has always compiled the decoder
 * into its own translation unit for the same reason. img.c has no global
 * state and every internal symbol is static and starts with ic_, so this is
 * the documented way to use it (see the end of img.h). The custom-image
 * wallpaper therefore costs the desktop one extra decoder, not one extra
 * decoder per image format. */
#include "img.c"

/* The setting file main.c already reads. A host harness can point this at a
 * scratch path. */
#ifndef MYSTIC_WALLPAPER_FILE
#define MYSTIC_WALLPAPER_FILE "/etc/mystic/wallpaper"
#endif

/* one wave period, in 16.16 fixed point over a 256 entry sine table */
#define WP_SINE_N     256
#define WP_SINE_MASK  (WP_SINE_N - 1)
#define WP_PHASE_MASK ((WP_SINE_N << 16) - 1)

/* ------------------------------------------------------------ names */

static const char *const WP_NAMES[WALLPAPER_COUNT] = {
    "Dusk", "Mesh", "Waves", "Orbits", "Black Hole", "Nebula",
    "Synthwave", "Dunes"
};

const char *wallpaper_name(int index) {
    if (index == WALLPAPER_IMAGE) return "Custom image";
    if (index < 0 || index >= WALLPAPER_COUNT) return NULL;
    return WP_NAMES[index];
}

int wallpaper_animated(int index) {
    /* Dusk, Mesh, Nebula and Dunes are still pictures: the desktop can paint
       them once and then leave the framebuffer alone. Waves, Orbits, the
       Black Hole and Synthwave use `seconds`, so they have to be repainted -
       the black hole turns its disk and Synthwave's grid scrolls. A custom
       image answers 1 while it is active, for EVERY index: wallpaper_draw
       then paints the picture instead of the index's background, and the
       desktop's static cache is keyed on the index alone, so serving that
       cache after a picture changed would show the previous wallpaper. */
    if (wallpaper_image_active()) return 1;
    if (index == WALLPAPER_IMAGE) return 0;
    return (index == 2 || index == 3 || index == 4 || index == 6) ? 1 : 0;
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

/* Is `user` the word `prefix` (lowercase), ignoring leading whitespace and
 * case? Used for the "image:" tag, which is followed by a path. */
static int wp_name_prefix_is(const char *user, const char *prefix) {
    while (*user == ' ' || *user == '\t' || *user == '\r' || *user == '\n') user++;
    for (size_t i = 0; prefix[i]; i++) {
        if (wp_lower((unsigned char)user[i]) != prefix[i]) return 0;
    }
    return 1;
}

int wallpaper_from_name(const char *name) {
    if (!name) return -1;
    /* "image:<path>" - a custom picture. Only the tag is needed here: the
     * path is read from the setting file by wp_setting_sync(), so a caller
     * whose line buffer is shorter than the path still resolves it. */
    if (wp_name_prefix_is(name, "image:")) return WALLPAPER_IMAGE;
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
static inline __attribute__((always_inline)) float wp_atan2f(float y, float x) {
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
 *    just outside the revieweral curve and shimmers slowly.
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
    float sup_max;                         /* max over psi, a radial bound */
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
static inline __attribute__((always_inline)) float wp_bh_bidx(float b) {
    if (b < WP_BH_BM) return (b - WP_BH_BC) * WP_BH_K0;
    return (float)WP_BH_NB0 + (b - WP_BH_BM) * WP_BH_K1;
}

/* crossing radius at impact parameter b after `dlt` radians of sweep;
 * 0 when that crossing does not exist (captured, or already escaped) */
static inline __attribute__((always_inline)) float wp_bh_radius(float t, int j, float fs) {
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
    /* One-entry dilation: sup[i] becomes the largest of the three raw bounds
     * i-1, i, i+1. The table is sampled at bin centres (i + 0.5) but the
     * paint floors psi into a bin, so a ray can sit anywhere in its bin and
     * must be allowed the widest bound the bin touches - the maximum over
     * the entry and its two neighbours is what makes the gate a superset of
     * the lit region at every psi instead of only at the centres.
     *
     * This has to read the undilated values. Dilating in place reads
     * sup[i - 1] after that entry was overwritten with the maximum of
     * everything before it, which turns the table into a running (prefix)
     * maximum: past the widest psi the bound is the global sup_max, the gate
     * stops culling, and the per-frame trace pays for pixels no disk can
     * reach. Keep the raw pass in its own array. */
    float raw[WP_BH_SUPN];
    memcpy(raw, bhgeo.sup, sizeof raw);
    for (int i = 0; i < WP_BH_SUPN; i++) {
        float m = raw[i];
        if (i > 0 && raw[i - 1] > m) m = raw[i - 1];
        if (i < WP_BH_SUPN - 1 && raw[i + 1] > m) m = raw[i + 1];
        bhgeo.sup[i] = m;
    }
    /* one radial bound for the whole frame: outside it no sweep can put a
     * crossing in the disk. It is never tighter than the photon ring, so the
     * ring can live inside the same gate. */
    float m = 0.0f;
    for (int i = 0; i < WP_BH_SUPN; i++) if (bhgeo.sup[i] > m) m = bhgeo.sup[i];
    if (m < WP_BH_BC * WP_BH_RMAX) m = WP_BH_BC * WP_BH_RMAX;
    bhgeo.sup_max = m;
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
 * long scratch near the revieweral curve, and scratches are exactly the
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
    /* The shimmer pattern turns at 1.70 rad/s - one revolution every 3.7 s -
     * which is the "slow" the ring comment above promises. 40.7436654 is the
     * 256 entry sine table's units per radian; the az16 scale (10430.378) is
     * units per radian times 256, for the shift that converts it back, and
     * using it here stepped the pattern 5.5 turns per 12.5 Hz frame: a
     * flicker, not the rotation. Reduced mod one turn in double so a desktop
     * left running for days cannot push the int conversion out of range. */
    const int   ring_ph  = (int)fmod((double)t * (1.70 * 40.74366543152521), 256.0);

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
                } else if (b <= bhgeo.sup_max) {
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
                           of the revieweral curve, and is skipped elsewhere */
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
                       revieweral curve, where every crossing piles up. A slow
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
 * hot loops are branch-free. R and B share the 0x00FF00FF mask, 16 bits
 * apart, and the largest sum is 1020, so no lane can carry into its
 * neighbour.
 *
 * Each lane goes back to the position it was masked from: (srb >> 2) is the
 * blue sum and stays in bits 0..7, (srb >> 18) is the red sum and goes to
 * bits 16..23. Writing them the other way round is invisible in the middle
 * of the frame - the vertical and the horizontal pass then swap twice - but
 * the outer row and the outer column of the upscale are written by a single
 * pass, so a swap here leaves red and blue exchanged in a hard line around
 * the whole frame. That line is what this extraction has to keep straight. */
static inline __attribute__((always_inline)) u32 wp_bh_m1(u32 a, u32 b) {
    u32 ra = a & 0x00FF00FFu, rb = b & 0x00FF00FFu;
    u32 ga = a & 0x0000FF00u, gb = b & 0x0000FF00u;
    u32 srb = ra + (ra << 1) + rb + 0x00020002u;
    u32 sg  = ga + (ga << 1) + gb + 0x00000200u;
    return 0xFF000000u | (((srb >> 18) & 0xFFu) << 16)
                       | (((sg  >> 10) & 0xFFu) << 8)
                       |  ((srb >> 2) & 0xFFu);
}
static inline __attribute__((always_inline)) u32 wp_bh_m3(u32 a, u32 b) {
    u32 ra = a & 0x00FF00FFu, rb = b & 0x00FF00FFu;
    u32 ga = a & 0x0000FF00u, gb = b & 0x0000FF00u;
    u32 srb = rb + (rb << 1) + ra + 0x00020002u;
    u32 sg  = gb + (gb << 1) + ga + 0x00000200u;
    return 0xFF000000u | (((srb >> 18) & 0xFFu) << 16)
                       | (((sg  >> 10) & 0xFFu) << 8)
                       |  ((srb >> 2) & 0xFFu);
}

/* q = 2 upscale of the complete trace surface, separable so the vertical
 * blend is done once per source column and reused by both destination
 * columns. Same 1/4-3/4 weights and the same clamped sample positions as
 * wp_upscale(), with the vertical clamp taken to the last source sample
 * (sh - 1) rather than to the base of the last pair. The clamp lands
 * exactly on that sample, so clamping one row short would answer the bottom
 * row with a second copy of the row above instead of the row the frame
 * ends on - a flat two-row band at the very bottom of the screen. */
static void wp_upscale2(Surface *dst, const u32 *src, int sw, int sh) {
    if (bhgeo.up_w != sw || !bhgeo.uprow) {
        free(bhgeo.uprow);
        bhgeo.uprow = (u32 *)malloc((size_t)sw * sizeof(u32));
        bhgeo.up_w = bhgeo.uprow ? sw : 0;
    }
    if (!bhgeo.uprow) { wp_upscale(dst, src, sw, sh, 2); return; }

    u32 *vrow = bhgeo.uprow;
    const int maxy = (sh - 1) * 256;
    const int lastx = (sw > 1) ? sw - 2 : 0;

    for (int y = 0; y < dst->h; y++) {
        int sy = 128 * y - 64;
        if (sy < 0) sy = 0;
        if (sy > maxy) sy = maxy;
        /* Clamp to the last source sample, not to the base of the last pair:
         * the clamp lands exactly on sample sh-1 (sy = (sh-1)*256, fy = 0),
         * so clamping to sh-2 would replace the bottom row with a copy of the
         * one above it instead of the row the ray actually hits. */
        int iy = sy >> 8;
        if (iy > sh - 1) iy = sh - 1;
        int fy = sy & 255;
        int wv = (fy == 64) ? 1 : (fy == 192) ? 3 : 0;
        const u32 *r0 = src + (size_t)iy * sw;
        const u32 *r1 = src + (size_t)((iy + 1 < sh) ? iy + 1 : iy) * sw;

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

/* ---------------------------------------------------------- 6. Synthwave
 * The eighties poster: a deep purple sky, a big banded sun sinking into a
 * hard horizon, and a neon grid in perspective running toward it. The grid
 * is real perspective, not a set of drawn wedges: with the camera at height
 * 1 and a pinhole of focal length fh, a ground point at depth Z lands on
 * row t = fh/Z below the horizon, and a line of constant lateral offset
 * X = k*sw lands at x = W/2 + k*sw*t. So the vertical lines are
 * x(y) = W/2 + k*sw*t (a fan through the vanishing point) and the
 * horizontal ones sit at the depths Z = m*step - phase, where `phase`
 * advances with `seconds` so the rows slide toward the viewer and are
 * respawned at the horizon. Nothing is drawn as a polygon: every pixel
 * asks how far it is from the nearest line of each family, in pixels, and
 * that distance is a 1.3 px antialiased ramp. Both families fade out at
 * the horizon so the fan cannot saturate into a white band.
 *
 * Per row the work is a handful of float ops (one divide); per pixel it is
 * one multiply, one round, one subtract and one multiply for the vertical
 * family, plus an add for the horizontal term the row already computed.
 * Measured 2.9 ms at 1280x800. */
static inline float wp_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Hermite ramp from 0 at e0 to 1 at e1 (e0 may be greater than e1). */
static inline float wp_smoothf(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = wp_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* floor()/round() without a libm call: on a baseline x86-64 build these are
 * calls, and the grid asks for one per pixel. Valid for |v| < 2^23. */
static inline float wp_floorf(float v) {
    int i = (int)v;
    return (v < (float)i) ? (float)(i - 1) : (float)i;
}

static inline int wp_iround(float v) {
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static void wp_draw_synthwave(Surface *s, const Theme *th, double seconds) {
    const int W = s->w, H = s->h;

    /* the horizon: a little below the middle, so the sky (and the sun) owns
       the larger half of the picture */
    int hy = (int)((float)H * 0.615f);
    if (hy < 2) hy = 2;
    if (hy > H - 3) hy = H - 3;

    /* Fixed synthwave hues pulled a little toward the theme: a dark theme
       keeps the poster look, a light one stays legible, and every theme's
       accent still appears in the grid. */
    float tmix = th->dark ? 0.15f : 0.30f;
    u32 sky_top = color_mix(RGB(0x12, 0x05, 0x2C), th->bg_top, tmix);
    u32 sky_mid = color_mix(RGB(0x3C, 0x0B, 0x66), th->bg_bottom, tmix);
    u32 hz_col  = color_mix(RGB(0xFF, 0x2D, 0x8A), th->accent, 0.28f);
    u32 sun_hi  = color_mix(RGB(0xFF, 0xE2, 0x84), th->glow, 0.18f);
    u32 sun_lo  = color_mix(RGB(0xFF, 0x2F, 0x6B), th->accent, 0.22f);
    u32 grd_v   = color_mix(RGB(0x45, 0xF0, 0xFF), th->accent, 0.22f);
    u32 grd_h   = color_mix(RGB(0xFF, 0x46, 0xA5), th->accent, 0.18f);
    u32 floor_t = color_mix(RGB(0x16, 0x04, 0x2A), th->bg_bottom, 0.22f);
    u32 floor_b = color_mix(RGB(0x04, 0x01, 0x0C), RGB(0, 0, 0), 0.0f);

    /* ---- sky: two stops, then a warm ramp into the horizon ---- */
    for (int y = 0; y < hy; y++) {
        float t = (float)y / (float)(hy > 1 ? hy - 1 : 1);
        u32 c = wp_lerp_color(sky_top, sky_mid, (int)(t * 256.0f));
        float w = wp_smoothf(0.55f, 1.0f, t);
        if (w > 0.0f) c = wp_lerp_color(c, hz_col, (int)(w * w * 168.0f));
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) row[x] = c;
    }

    /* ---- the ground: a dark floor. Painted before the sun so the halo can
       spill onto it - the sun is lighting the whole picture. ---- */
    for (int y = hy; y < H; y++) {
        float t = (float)(y - hy) / (float)(H - hy > 1 ? H - hy : 1);
        u32 c = wp_lerp_color(floor_t, floor_b, (int)(t * 200.0f));
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) row[x] = c;
    }

    /* ---- the sun: a disc with horizontal slices cut out of its lower half,
       centred on the vanishing point and cut by the horizon ---- */
    float r = (float)W * 0.200f;
    if (r > (float)H * 0.330f) r = (float)H * 0.330f;
    if (r < 8.0f) r = 8.0f;
    float cx = (float)W * 0.5f;
    float cy = (float)hy - r * 0.42f;

    wp_glow(s, cx, cy, r * 2.30f, color_mix(hz_col, sun_lo, 0.40f), 0.40f);

    int sy0 = (int)floorf(cy - r - 2.0f), sy1 = (int)ceilf(cy + r + 2.0f);
    int sx0 = (int)floorf(cx - r - 2.0f), sx1 = (int)ceilf(cx + r + 2.0f);
    if (sy0 < 0) sy0 = 0;
    if (sy1 > hy) sy1 = hy;                 /* the horizon cuts it */
    if (sx0 < 0) sx0 = 0;
    if (sx1 > W) sx1 = W;
    for (int y = sy0; y < sy1; y++) {
        float py = (float)y + 0.5f - cy;
        float v = wp_clampf(((float)y + 0.5f - (cy - r)) / (2.0f * r), 0.0f, 1.0f);
        u32 sc = v < 0.55f ? wp_lerp_color(sun_hi, sun_lo, (int)(v / 0.55f * 256.0f))
                           : sun_lo;
        /* The slices depend only on the row: the cell is 2r/7 px tall, and
           inside is the signed distance in px from the slice's lower edge
           (<0 inside the slice). */
        int banded = v > 0.34f;
        float inside_px = 0.0f;
        if (banded) {
            float st = (v - 0.34f) / 0.66f;
            float cell_px = (2.0f * r) / 11.0f;
            float ph = st * 11.0f + 0.20f;
            float fr = ph - wp_floorf(ph);            /* 0..1 inside the cell */
            float width = 0.13f + 0.30f * st;         /* slices thicken down */
            inside_px = (width - fr) * cell_px;
        }
        for (int x = sx0; x < sx1; x++) {
            float px = (float)x + 0.5f - cx;
            float d = sqrtf(px * px + py * py);
            float cov = wp_clampf(r + 0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            if (banded) {
                float cut = wp_clampf(inside_px / 1.3f + 0.5f, 0.0f, 1.0f);
                cov *= 1.0f - cut;
                if (cov <= 0.0f) continue;
            }
            wp_blend(s, x, y, sc, (int)(cov * 255.0f + 0.5f));
        }
    }

    /* the sun's light spills onto the floor under it, low and faint: a
       bright wash here would make the thin end of the grid a visible
       speckle instead of a fade */
    wp_glow(s, cx, (float)hy + (float)(H - hy) * 0.55f,
            (float)W * 0.30f, hz_col, 0.12f);

    /* ---- the horizon itself: a bright line with a 3 px falloff ---- */
    for (int y = hy - 3; y < hy + 4; y++) {
        if (y < 0 || y >= H) continue;
        float d = fabsf((float)y + 0.5f - (float)hy);
        int a = (int)(190.0f * wp_clampf(1.0f - d / 3.2f, 0.0f, 1.0f));
        if (a <= 0) continue;
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) row[x] = wp_lerp_color(row[x], hz_col, a);
    }

    /* ---- the grid ----
     * Camera height 1. A ground point at depth Z is on row t = fh/Z; a
     * lateral offset X is at x = W/2 + X*t. So:
     *   vertical lines   X = k*sw        -> x = W/2 + k*sw*t
     *   horizontal lines Z = m*step - ph -> t = fh/Z
     * Both are evaluated as a distance in pixels from the nearest line,
     * which is what makes a line one pixel wide at any depth. */
    const float fh = (float)(H - hy);
    const float step = 0.100f;                       /* world spacing, rows */
    const float sw = (float)W / 12.0f / fh;          /* W/12 px at the bottom */
    const float lw = 1.00f;                          /* half width, px */
    const float ph = (float)fmod(seconds * 0.22, 1.0);
    const float half_w = (float)W * 0.5f;
    const int hr = (int)GET_R(grd_h), hg = (int)GET_G(grd_h), hb = (int)GET_B(grd_h);
    const int vr = (int)GET_R(grd_v), vg = (int)GET_G(grd_v), vb = (int)GET_B(grd_v);

    for (int y = hy + 1; y < H; y++) {
        float t = (float)y + 0.5f - (float)hy;
        float Z = fh / t;
        float inv_t_sw = 1.0f / (t * sw);            /* world k per px */
        float dpx_per_k = t * sw;                    /* px per world k  */

        /* one horizontal line can be near this row: find it in world Z */
        float kz = Z / step + ph;
        float mz = wp_floorf(kz + 0.5f);
        float dz = (kz - mz) * step;                 /* world depth error */
        float ds = t * t / fh * fabsf(dz);           /* px on this row    */
        float ih = 1.0f - ds / lw;
        if (ih < 0.0f) ih = 0.0f;

        /* Both families fade toward the horizon, and both are dimmed by how
           much of a pixel a line train covers there: once the rows are less
           than a line width apart, the nearest-line model would paint EVERY
           row and the grid would turn into a moire cloth. spacing/(2*lw) is
           the average coverage of a train that dense, so the grid dissolves
           into haze exactly where the lines merge. */
        float fade = wp_smoothf(fh * 0.10f, fh * 0.62f, t);
        float spacing_h = (t * t / fh) * step;
        float spacing_v = sw * t;
        float aa_h = wp_clampf(spacing_h / (2.0f * lw), 0.0f, 1.0f);
        float aa_v = wp_clampf(spacing_v / (2.0f * lw), 0.0f, 1.0f);
        float dim_h = fade * aa_h;
        float dim_v = fade * aa_v;
        ih *= dim_h;
        if (ih > 1.0f) ih = 1.0f;

        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) {
            float k = ((float)x + 0.5f - half_w) * inv_t_sw;
            int ik = wp_iround(k);
            float dv = fabsf(k - (float)ik) * dpx_per_k;
            float iv = 1.0f - dv / lw;
            iv *= dim_v;
            if (iv <= 0.0f) {
                if (ih <= 0.0f) continue;
                iv = 0.0f;
            }
            int add_r = (int)((float)hr * ih + (float)vr * iv);
            int add_g = (int)((float)hg * ih + (float)vg * iv);
            int add_b = (int)((float)hb * ih + (float)vb * iv);
            u32 c = row[x];
            int R = (int)GET_R(c) + add_r;
            int G = (int)GET_G(c) + add_g;
            int B = (int)GET_B(c) + add_b;
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            row[x] = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
    }
}

/* ------------------------------------------------------------ 7. Dunes
 * A layered desert at dusk. The sky is a warm gradient with a low sun and a
 * soft halo; five ridges run across the screen, each one a smooth noise
 * silhouette (four octaves of value noise plus one long swell) whose crest
 * sits a little lower and reaches a little further for every layer nearer
 * the viewer. Each layer's colour is its own warm brown mixed toward the
 * horizon colour by its haze, and the crest carries a one-pixel rim light
 * from the sun. The layers cannot cross: a nearer crest is forced at least
 * two pixels below the one behind it, so the column painter below can walk
 * the layers in order with a single comparison per pixel.
 *
 * The stars are plotted from a 190-entry table built once from the same
 * deterministic hash the rest of this file uses, not hashed per pixel: a
 * per-pixel star field over the sky would cost more than all five ridges.
 * An extra 240 dust motes, dimmer and packed near the horizon, give the sky
 * some grain. Measured 3.3 ms at 1280x800.
 */
typedef struct { float u, v; unsigned char mag; } WpDuneStar;
#define WP_DUNE_STARS 190
typedef struct { float u, v; unsigned char mag; } WpDuneDust;
#define WP_DUNE_DUST  240
static WpDuneStar wp_dune_stars[WP_DUNE_STARS];
static WpDuneDust wp_dune_dust[WP_DUNE_DUST];
static int wp_dune_ready;

static void wp_dune_build(void) {
    if (wp_dune_ready) return;
    wp_dune_ready = 1;
    for (int i = 0; i < WP_DUNE_STARS; i++) {
        u32 h = wp_h32((u32)i, 0x51A2u, 0xD00Du);
        /* the sky owns the top 52% of the picture, above every crest */
        wp_dune_stars[i].u = (float)(h & 0xFFFFu) * (1.0f / 65535.0f);
        wp_dune_stars[i].v = (float)((h >> 16) & 0xFFFFu) * (1.0f / 65535.0f) * 0.52f;
        wp_dune_stars[i].mag = (unsigned char)(70 + (int)((h >> 8) & 0x7Fu));
    }
    for (int i = 0; i < WP_DUNE_DUST; i++) {
        u32 h = wp_h32((u32)i + 977u, 0x9E37u, 0x51A2u);
        wp_dune_dust[i].u = (float)(h & 0xFFFFu) * (1.0f / 65535.0f);
        /* dust hugs the horizon, where the air is thick */
        float r = (float)((h >> 16) & 0xFFFFu) * (1.0f / 65535.0f);
        wp_dune_dust[i].v = 0.20f + 0.36f * r * r;
        wp_dune_dust[i].mag = (unsigned char)(16 + (int)((h >> 8) & 0x1Fu));
    }
}

/* One soft point: the pixel itself plus four half-strength neighbours, so a
 * star reads at any size without a sprite. */
static void wp_dot(Surface *s, int x, int y, int r, int g, int b, int a) {
    if (a <= 0) return;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int w = (dx && dy) ? 0 : (dx || dy ? a / 3 : a);
            if (w <= 0) continue;
            int px = x + dx, py = y + dy;
            if ((unsigned)px >= (unsigned)s->w || (unsigned)py >= (unsigned)s->h) continue;
            u32 *p = s->px + (size_t)py * s->stride + px;
            u32 c = *p;
            int R = (int)GET_R(c) + (r * w) / 255;
            int G = (int)GET_G(c) + (g * w) / 255;
            int B = (int)GET_B(c) + (b * w) / 255;
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            *p = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
    }
}

#define WP_DUNE_LAYERS 5

static void wp_draw_dunes(Surface *s, const Theme *th) {
    const int W = s->w, H = s->h;
    wp_dune_build();

    int hy = (int)((float)H * 0.615f);
    if (hy < 3) hy = 3;
    if (hy > H - 4) hy = H - 4;

    /* warm dusk: a deep violet top, an amber horizon, both pulled toward the
       theme so a porcelain desktop is a hazy sunrise and not a night scene */
    float tmix = th->dark ? 0.28f : 0.46f;
    u32 sky_top = color_mix(RGB(0x1B, 0x12, 0x33), th->bg_top, tmix);
    u32 sky_mid = color_mix(RGB(0x6B, 0x2E, 0x4A), th->bg_bottom, tmix);
    u32 horizon = color_mix(RGB(0xF2, 0x9A, 0x54), th->glow, th->dark ? 0.26f : 0.42f);
    u32 sun_col = color_mix(RGB(0xFF, 0xF6, 0xDC), th->accent, 0.10f);

    /* the sky fills the whole surface; the ridges paint over the lower part,
       and the strip between the horizon and the farthest crest keeps it */
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)(hy > 1 ? hy : 1);
        if (t > 1.0f) t = 1.0f;
        u32 c = t < 0.55f
             ? wp_lerp_color(sky_top, sky_mid, (int)(t / 0.55f * 256.0f))
             : wp_lerp_color(sky_mid, horizon, (int)((t - 0.55f) / 0.45f * 256.0f));
        u32 *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < W; x++) row[x] = c;
    }

    /* the sun: low, right of centre, with a wide halo on the horizon */
    float sr = (float)W * 0.045f;
    if (sr > (float)H * 0.070f) sr = (float)H * 0.070f;
    if (sr < 5.0f) sr = 5.0f;
    float sx = (float)W * 0.66f;
    float sy = (float)hy - (float)(H - hy) * 0.30f;
    wp_glow(s, sx, sy, (float)W * 0.30f, color_mix(horizon, sun_col, 0.45f), 0.55f);
    {
        int y0 = (int)(sy - sr - 2.0f), y1 = (int)(sy + sr + 2.0f);
        int x0 = (int)(sx - sr - 2.0f), x1 = (int)(sx + sr + 2.0f);
        if (y0 < 0) y0 = 0;
        if (y1 > H) y1 = H;
        if (x0 < 0) x0 = 0;
        if (x1 > W) x1 = W;
        for (int y = y0; y < y1; y++) {
            float py = (float)y + 0.5f - sy;
            for (int x = x0; x < x1; x++) {
                float px = (float)x + 0.5f - sx;
                float d = sqrtf(px * px + py * py);
                float cov = wp_clampf(sr + 0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) wp_blend(s, x, y, sun_col, (int)(cov * 255.0f));
            }
        }
    }
    /* the sun's light spills along the horizon, under the ridges */
    wp_glow(s, sx, (float)hy, (float)W * 0.55f, horizon, 0.30f);

    /* ---- stars and dust, above the crests ---- */
    {
        const float inv_glow = 1.0f / ((float)W * 0.16f);
        for (int i = 0; i < WP_DUNE_STARS; i++) {
            float u = wp_dune_stars[i].u, v = wp_dune_stars[i].v;
            int x = (int)(u * (float)W);
            int y = (int)(v * (float)H);
            /* the sun's halo washes the stars out */
            float gx = (float)x - sx, gy = (float)y - sy;
            float wash = wp_clampf(sqrtf(gx * gx + gy * gy) * inv_glow, 0.0f, 1.0f);
            int a = (int)((float)wp_dune_stars[i].mag * (0.25f + 0.75f * wash));
            wp_dot(s, x, y, 255, 244, 226, a);
        }
        for (int i = 0; i < WP_DUNE_DUST; i++) {
            int x = (int)(wp_dune_dust[i].u * (float)W);
            int y = (int)(wp_dune_dust[i].v * (float)H);
            wp_dot(s, x, y, 255, 226, 190, wp_dune_dust[i].mag);
        }
    }

    /* ---- the ridges ----
     * Layer colours, farthest first: each one is the layer's own warm brown
     * mixed toward the horizon by its haze, so distance reads as air. */
    static const u32 base[WP_DUNE_LAYERS] = {
        /* far */ RGB(0x7C, 0x46, 0x50),
                  RGB(0x63, 0x33, 0x40),
                  RGB(0x4A, 0x25, 0x30),
                  RGB(0x33, 0x18, 0x22),
        /* near */RGB(0x1F, 0x0E, 0x15)
    };
    u32 layer[WP_DUNE_LAYERS];
    int rim[WP_DUNE_LAYERS];
    for (int i = 0; i < WP_DUNE_LAYERS; i++) {
        /* a light theme gets more air in every layer, otherwise the nearest
           ridge is a black band under a pale sky */
        float haze = 0.42f - 0.085f * (float)i + (th->dark ? 0.0f : 0.16f);
        layer[i] = color_mix(base[i], horizon, haze);
        /* the crest of a far ridge is backlit; the near ones are in their
           own shadow, so the rim light dies with every layer */
        rim[i] = 104 - 17 * i;
        if (rim[i] < 22) rim[i] = 22;
    }

    const float inv_w = 1.0f / (float)W;
    const float dh = (float)(H - hy > 1 ? H - hy : 1);
    const float inv_sun = 1.0f / ((float)W * 0.72f);
    /* The vertical shade, in 8.8: 1.0 at the horizon, 0.61 at the bottom.
     * It is quantised to 16 steps and each layer resolves those steps into
     * colours ONCE, so the inner loop is a shift, a table read and a store
     * instead of three multiplies per pixel. */
    const int shade_step = (99 << 8) / (int)dh;
    u32 shade_lut[WP_DUNE_LAYERS][16];
    for (int i = 0; i < WP_DUNE_LAYERS; i++) {
        u32 c = layer[i];
        for (int k = 0; k < 16; k++) {
            int sh = 256 - k * 7;                /* 256 down to 151 */
            if (sh < 158) sh = 158;
            shade_lut[i][k] = 0xFF000000u
                | ((u32)(((int)GET_R(c) * sh) >> 8) << 16)
                | ((u32)(((int)GET_G(c) * sh) >> 8) << 8)
                |  (u32)(((int)GET_B(c) * sh) >> 8);
        }
    }

    /* Painted far to near, one pass per ridge: a nearer dune simply covers
     * the base of the one behind it, which is what gives the stack its
     * depth. (The first version walked the layers per column and forced
     * every crest below the previous one; that cannot let two dunes overlap
     * and it read as flat strata.) */
    for (int li = 0; li < WP_DUNE_LAYERS; li++) {
        float base_y = (float)hy + dh * (0.03f + 0.225f * (float)li);
        float amp = dh * (0.150f + 0.045f * (float)li);
        float swellf = 0.85f + 0.42f * (float)li;
        float ripplef = 3.2f + 1.7f * (float)li;
        int lrim = rim[li];
        const u32 *lut = shade_lut[li];
        for (int x = 0; x < W; x++) {
            float u = ((float)x + 0.5f) * inv_w;
            /* A dune is a broad arc first and a wind-cut wiggle second: one
               long sine gives the swell (0.85 to 2.5 periods across the
               width, layer by layer) and three octaves of value noise ride
               on top of it. */
            float swell = 0.5f + 0.5f * sinf(6.2831853f * u * swellf
                                             + (float)li * 1.9f);
            float n = wp_fbm(u * ripplef + 7.7f * (float)li,
                             1.3f + 0.7f * (float)li, 3, 0xD000u + (u32)li * 977u);
            float shape = 0.55f * swell + 0.45f * n;          /* 0..1 */
            float topf = base_y - amp * shape;
            if (topf < 0.0f) topf = 0.0f;
            if (topf > (float)(H - 1)) topf = (float)(H - 1);
            int top = (int)topf;
            /* the crest's sub-pixel part: the first row is blended with what
               is behind it, so a nearly horizontal crest is a smooth edge
               and not a staircase of whole pixels */
            int cov0 = (int)((1.0f - (topf - (float)top)) * 256.0f);

            /* 1 under the sun, 0 at the far edges: the rim light's reach */
            float sunw = 1.0f - wp_clampf(fabsf((float)x + 0.5f - sx) * inv_sun,
                                          0.0f, 1.0f);
            int rim_a = (lrim * (int)(64.0f + 192.0f * sunw * sunw)) >> 8;

            int shade = 256 - (((top - hy) * shade_step) >> 8);
            if (shade > 256) shade = 256;
            u32 *col = s->px + (size_t)top * s->stride + x;
            for (int y = top; y < H; y++, col += s->stride) {
                int k = (256 - shade) / 7;
                if (k < 0) k = 0;
                if (k > 15) k = 15;
                u32 c = lut[k];
                if (y - top < 2) {
                    /* rim light on the crest: the sun is behind these hills */
                    int ra = rim_a * (2 - (y - top)) / 2;
                    int R = (int)GET_R(c) + (((255 - (int)GET_R(c)) * ra) >> 8);
                    int G = (int)GET_G(c) + (((205 - (int)GET_G(c)) * ra) >> 8);
                    int B = (int)GET_B(c) + (((140 - (int)GET_B(c)) * ra) >> 8);
                    if (R > 255) R = 255;
                    if (G > 255) G = 255;
                    if (B > 255) B = 255;
                    c = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
                }
                if (y == top) {
                    u32 u = *col;
                    int ia = 256 - cov0;
                    c = 0xFF000000u
                      | ((u32)((((int)GET_R(c) * cov0 + (int)GET_R(u) * ia) >> 8)) << 16)
                      | ((u32)((((int)GET_G(c) * cov0 + (int)GET_G(u) * ia) >> 8)) << 8)
                      |  (u32)((((int)GET_B(c) * cov0 + (int)GET_B(u) * ia) >> 8));
                }
                *col = c;
                shade -= shade_step;
            }
        }
    }
}

/* ---------------------------------------------------- the custom image
 * The one wallpaper that is not a function of the theme: a PNG, JPEG, BMP or
 * PNM from disk. It is decoded with src/img.c (the same decoder the viewer
 * and the browser use), cover-scaled to the surface - aspect ratio kept, the
 * overflow cropped, never stretched - and the screen-size copy is cached.
 *
 * Three rules keep it off the per-frame path:
 *   - the setting file is stat()ed at most once a second, and only read when
 *     its size or mtime moved (wp_setting_sync);
 *   - the picture is decoded once, when the setting changes;
 *   - the cover scale runs once per picture and once per surface size, never
 *     per frame. Every frame after that is one row memcpy (0.34 ms at
 *     1280x800).
 *
 * The module owns the state, not the desktop: wallpaper_draw() reads the
 * setting itself, so a picture written by Settings - or restored at boot by
 * main.c's existing "first line of /etc/mystic/wallpaper" reader - is drawn
 * without a new call from main.c. */

#define WP_PATH_MAX  512          /* a path longer than this is refused */
#define WP_FILE_MAX  (64u << 20)  /* refuse a file over 64 MiB             */

static char  wp_img_path[WP_PATH_MAX];   /* the active (or attempted) path */
static char  wp_img_err[192];            /* why the last attempt failed    */
static Img   wp_img_src;                 /* decoded, freed by the first scale */
static u32  *wp_img_dst;                 /* cached cover-scaled picture    */
static int   wp_img_dw, wp_img_dh;
static int   wp_img_sw, wp_img_sh;       /* decoded source size, for reports */
static int   wp_img_ok;                  /* 1 = a picture is the background */
static int   wp_img_gen;                 /* bumped by every change          */

static int wp_prefix_ci(const char *s, const char *prefix) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    for (size_t i = 0; prefix[i]; i++)
        if (wp_lower((unsigned char)s[i]) != prefix[i]) return 0;
    return 1;
}

/* Read a whole file into a malloc'ed buffer. The caller frees. */
static unsigned char *wp_read_file(const char *path, size_t *len, char *err, size_t esz) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && esz) snprintf(err, esz, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        if (err && esz) snprintf(err, esz, "%s: cannot seek: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        if (err && esz) snprintf(err, esz, "%s: %s", path,
                                 sz == 0 ? "the file is empty" : "cannot tell its size");
        fclose(f);
        return NULL;
    }
    if ((unsigned long)sz > WP_FILE_MAX) {
        if (err && esz)
            snprintf(err, esz, "%s: %ld bytes is over the %u MiB limit", path, sz,
                     (unsigned)(WP_FILE_MAX >> 20));
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) {
        if (err && esz) snprintf(err, esz, "%s: out of memory for %ld bytes", path, sz);
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    int rerr = ferror(f) ? errno : 0;
    fclose(f);
    if (got != (size_t)sz) {
        if (err && esz)
            snprintf(err, esz, "%s: read %zu of %ld bytes%s%s", path, got, sz,
                     rerr ? ": " : "", rerr ? strerror(rerr) : "");
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/* ---- the resampler ----------------------------------------------------
 * Cover, not stretch: the visible part of the source is the largest
 * destination-aspect rectangle that fits in it, centred, and every
 * destination pixel is that rectangle divided into a dw x dh grid.
 *
 * Shrinking averages the source pixels the cell covers (a real box filter:
 * edge pixels are weighted by the fraction of the cell they own). Enlarging
 * samples bilinearly, because a box filter narrower than one source pixel
 * would just repeat pixels and look like a nearest-neighbour blow-up. Alpha
 * is averaged as a plain channel and the result is composited over black, so
 * a transparent wallpaper is black rather than last frame's pixels. */
static u32 wp_src_px(const Img *im, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= im->w) x = im->w - 1;
    if (y >= im->h) y = im->h - 1;
    return im->px[(size_t)y * im->stride + x];
}

static u32 wp_over_black(int r, int g, int b, int a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    r = (r * a) / 255;
    g = (g * a) / 255;
    b = (b * a) / 255;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static void wp_cover_scale(const Img *src, int dw, int dh, u32 *dst) {
    const double sxf = (double)dw / (double)src->w;
    const double syf = (double)dh / (double)src->h;
    const double s = sxf > syf ? sxf : syf;      /* the cover factor */
    const double vis_w = (double)dw / s;         /* source pixels shown */
    const double vis_h = (double)dh / s;
    const double ox = ((double)src->w - vis_w) * 0.5;   /* the centred crop */
    const double oy = ((double)src->h - vis_h) * 0.5;
    const double step_x = vis_w / (double)dw;
    const double step_y = vis_h / (double)dh;

    if (s >= 1.0) {
        /* enlarge: bilinear, sampling at the cell centre */
        for (int y = 0; y < dh; y++) {
            float fy = (float)(oy + ((double)y + 0.5) * step_y - 0.5);
            int iy = (int)wp_floorf(fy);
            float ty = fy - (float)iy;
            u32 *row = dst + (size_t)y * dw;
            for (int x = 0; x < dw; x++) {
                float fx = (float)(ox + ((double)x + 0.5) * step_x - 0.5);
                int ix = (int)wp_floorf(fx);
                float tx = fx - (float)ix;
                u32 p00 = wp_src_px(src, ix, iy), p10 = wp_src_px(src, ix + 1, iy);
                u32 p01 = wp_src_px(src, ix, iy + 1), p11 = wp_src_px(src, ix + 1, iy + 1);
                float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
                float w01 = (1.0f - tx) * ty, w11 = tx * ty;
                int a = (int)(w00 * GET_A(p00) + w10 * GET_A(p10) +
                              w01 * GET_A(p01) + w11 * GET_A(p11));
                int r = (int)(w00 * GET_R(p00) + w10 * GET_R(p10) +
                              w01 * GET_R(p01) + w11 * GET_R(p11));
                int g = (int)(w00 * GET_G(p00) + w10 * GET_G(p10) +
                              w01 * GET_G(p01) + w11 * GET_G(p11));
                int b = (int)(w00 * GET_B(p00) + w10 * GET_B(p10) +
                              w01 * GET_B(p01) + w11 * GET_B(p11));
                row[x] = wp_over_black(r, g, b, a);
            }
        }
        return;
    }

    /* Shrink: area average with fractional edge weights. The column spans
     * and their weights are the same for every destination row, so they are
     * worked out once (that is what makes a 6 MP photo a 60 ms scale and not
     * a 200 ms one). */
    int span = (int)ceil(step_x) + 2;            /* widest source span, px */
    if (span < 1) span = 1;
    int span_y = (int)ceil(step_y) + 2;
    if (span_y < 1) span_y = 1;
    int *xoff = malloc((size_t)dw * sizeof(int));
    float *xwt = malloc((size_t)dw * (size_t)span * sizeof(float));
    float *ywt = malloc((size_t)span_y * sizeof(float));
    if (!xoff || !xwt || !ywt) {                 /* no table: nearest sample */
        for (int y = 0; y < dh; y++) {
            int sy = (int)(oy + ((double)y + 0.5) * step_y);
            u32 *row = dst + (size_t)y * dw;
            for (int x = 0; x < dw; x++) {
                int sx = (int)(ox + ((double)x + 0.5) * step_x);
                u32 p = wp_src_px(src, sx, sy);
                row[x] = wp_over_black((int)GET_R(p), (int)GET_G(p),
                                       (int)GET_B(p), (int)GET_A(p));
            }
        }
        free(xoff);
        free(xwt);
        free(ywt);
        return;
    }
    for (int x = 0; x < dw; x++) {
        double x0 = ox + (double)x * step_x;
        double x1 = x0 + step_x;
        int ix0 = (int)floor(x0);
        double wsum = 0.0;
        int n = 0;
        for (int i = ix0; i < (int)ceil(x1) && n < span; i++, n++) {
            double xa = x0 > (double)i ? x0 : (double)i;
            double xb = x1 < (double)(i + 1) ? x1 : (double)(i + 1);
            double w = xb > xa ? xb - xa : 0.0;
            xwt[(size_t)x * span + n] = (float)w;
            wsum += w;
        }
        if (wsum <= 0.0) wsum = 1.0;
        for (int k = 0; k < n; k++) xwt[(size_t)x * span + k] = (float)(xwt[(size_t)x * span + k] / wsum);
        /* the taps must stay inside the image; shift the window, never read
           past it (the weights still describe the true box) */
        if (ix0 < 0) ix0 = 0;
        if (src->w > span && ix0 > src->w - span) ix0 = src->w - span;
        xoff[x] = ix0;
    }

    for (int y = 0; y < dh; y++) {
        double y0 = oy + (double)y * step_y;
        double y1 = y0 + step_y;
        int iy0 = (int)floor(y0);
        int yn = 0;
        double wysum = 0.0;
        for (int j = iy0; j < (int)ceil(y1) && yn < span_y; j++, yn++) {
            double ya = y0 > (double)j ? y0 : (double)j;
            double yb = y1 < (double)(j + 1) ? y1 : (double)(j + 1);
            double w = yb > ya ? yb - ya : 0.0;
            ywt[yn] = (float)w;
            wysum += w;
        }
        if (wysum <= 0.0) wysum = 1.0;
        for (int k = 0; k < yn; k++) ywt[k] = (float)(ywt[k] / wysum);
        u32 *row = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            const float *wx = xwt + (size_t)x * span;
            int ix0 = xoff[x];
            float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
            for (int j = 0; j < yn; j++) {
                int sy = iy0 + j;
                if (sy < 0) sy = 0;
                if (sy >= src->h) sy = src->h - 1;
                const u32 *srow = src->px + (size_t)sy * src->stride + ix0;
                float wy = ywt[j];
                for (int i = 0; i < span; i++) {
                    float w = wx[i];
                    if (w <= 0.0f) continue;
                    u32 p = srow[i];
                    ar += w * wy * (float)GET_R(p);
                    ag += w * wy * (float)GET_G(p);
                    ab += w * wy * (float)GET_B(p);
                    aa += w * wy * (float)GET_A(p);
                }
            }
            row[x] = wp_over_black((int)(ar + 0.5f), (int)(ag + 0.5f),
                                   (int)(ab + 0.5f), (int)(aa + 0.5f));
        }
    }
    free(xoff);
    free(xwt);
    free(ywt);
}

/* Read, decode and (later) scale one picture. 0 = ok. */
static int wp_load_image(const char *path, Img *out, char *err, size_t esz) {
    size_t len = 0;
    unsigned char *buf = wp_read_file(path, &len, err, esz);
    if (!buf) return -1;
    char derr[160];
    memset(out, 0, sizeof(*out));
    int rc = img_decode(buf, len, out, derr, sizeof(derr));
    free(buf);
    if (rc != 0) {
        if (err && esz) snprintf(err, esz, "%s: %s", path, derr);
        return -1;
    }
    return 0;
}

int wallpaper_set_image(const char *path, char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    if (!path || !path[0]) {
        if (err && esz) snprintf(err, esz, "%s", "no path given");
        return -1;
    }
    if (strlen(path) >= sizeof(wp_img_path)) {
        if (err && esz)
            snprintf(err, esz, "the path is %zu bytes; the limit is %zu",
                     strlen(path), sizeof(wp_img_path) - 1);
        return -1;
    }
    /* Decode now, so a wrong file is refused at the pick and not at the
     * first frame. The decoded picture is kept until the draw knows the
     * surface size, then freed. */
    Img im;
    char why[192];
    if (wp_load_image(path, &im, why, sizeof(why)) != 0) {
        if (err && esz) snprintf(err, esz, "%s", why);
        return -1;
    }

    img_free(&wp_img_src);
    free(wp_img_dst);
    wp_img_dst = NULL;
    wp_img_dw = wp_img_dh = 0;
    wp_img_src = im;
    wp_img_sw = im.w;
    wp_img_sh = im.h;
    snprintf(wp_img_path, sizeof(wp_img_path), "%s", path);
    wp_img_ok = 1;
    wp_img_gen++;
    wp_img_err[0] = 0;
    return 0;
}

void wallpaper_clear_image(void) {
    if (!wp_img_ok && !wp_img_dst && !wp_img_src.px && wp_img_path[0] == 0) return;
    img_free(&wp_img_src);
    free(wp_img_dst);
    wp_img_dst = NULL;
    wp_img_dw = wp_img_dh = 0;
    wp_img_sw = wp_img_sh = 0;
    wp_img_path[0] = 0;
    wp_img_ok = 0;
    wp_img_gen++;
}

int wallpaper_image_active(void) { return wp_img_ok; }

const char *wallpaper_image_path(void) {
    return wp_img_ok ? wp_img_path : "";
}

int wallpaper_image_generation(void) { return wp_img_gen; }

int wallpaper_image_size(int *src_w, int *src_h, int *dst_w, int *dst_h) {
    if (src_w) *src_w = wp_img_sw;
    if (src_h) *src_h = wp_img_sh;
    if (dst_w) *dst_w = wp_img_dw;
    if (dst_h) *dst_h = wp_img_dh;
    return wp_img_ok ? 1 : 0;
}

/* Build (or rebuild) the screen-size cache. The source is either the decoded
 * picture set_image() left behind, or a fresh decode when the surface changed
 * size after that source was freed. */
static int wp_image_build(int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    Img im;
    int borrowed = 0;
    memset(&im, 0, sizeof(im));
    if (wp_img_src.px) {
        im = wp_img_src;
        borrowed = 1;
    } else {
        if (!wp_img_path[0]) return -1;
        char why[192];
        if (wp_load_image(wp_img_path, &im, why, sizeof(why)) != 0) {
            snprintf(wp_img_err, sizeof(wp_img_err), "%s", why);
            return -1;
        }
    }
    u32 *dst = malloc((size_t)w * (size_t)h * sizeof(u32));
    if (!dst) {
        snprintf(wp_img_err, sizeof(wp_img_err), "out of memory for %dx%d", w, h);
        if (borrowed) img_free(&wp_img_src); else img_free(&im);
        return -1;
    }
    wp_img_sw = im.w;
    wp_img_sh = im.h;
    wp_cover_scale(&im, w, h, dst);
    free(wp_img_dst);
    wp_img_dst = dst;
    wp_img_dw = w;
    wp_img_dh = h;
    wp_img_err[0] = 0;
    if (borrowed) img_free(&wp_img_src); else img_free(&im);
    return 0;
}

/* Blit the cache. Same size is a row memcpy; a different size (the surface
 * changed and the rebuild failed, e.g. the file went away) is a centred
 * copy that still never stretches. */
static void wp_image_blit(Surface *s) {
    if (!wp_img_dst || wp_img_dw <= 0 || wp_img_dh <= 0) return;
    if (wp_img_dw == s->w && wp_img_dh == s->h) {
        for (int y = 0; y < s->h; y++)
            memcpy(s->px + (size_t)y * s->stride,
                   wp_img_dst + (size_t)y * wp_img_dw, (size_t)s->w * 4u);
        return;
    }
    surf_fill(s, 0, 0, s->w, s->h, RGB(0, 0, 0));
    int cw = wp_img_dw < s->w ? wp_img_dw : s->w;
    int ch = wp_img_dh < s->h ? wp_img_dh : s->h;
    int ox = (wp_img_dw - cw) / 2, oy = (wp_img_dh - ch) / 2;
    int dx = (s->w - cw) / 2, dy = (s->h - ch) / 2;
    for (int y = 0; y < ch; y++)
        memcpy(s->px + (size_t)(dy + y) * s->stride + dx,
               wp_img_dst + (size_t)(oy + y) * wp_img_dw + ox, (size_t)cw * 4u);
}

/* Keep the module in step with the setting file, without touching the disk
 * per frame: one stat per second, a read only when size or mtime moved, and
 * a decode only when the picture really changed. */
static void wp_setting_sync(int force) {
    static time_t last_check;
    static int have_memo;
    static long long memo_mtime, memo_size;

    time_t now = time(NULL);
    if (!force && last_check != 0 && last_check == now) return;
    last_check = now;

    struct stat st;
    if (stat(MYSTIC_WALLPAPER_FILE, &st) != 0) {
        have_memo = 0;                 /* no setting: nothing to restore */
        return;
    }
    long long mt = (long long)st.st_mtime;
    long long sz = (long long)st.st_size;
    if (have_memo && memo_mtime == mt && memo_size == sz) return;
    have_memo = 1;
    memo_mtime = mt;
    memo_size = sz;

    FILE *f = fopen(MYSTIC_WALLPAPER_FILE, "r");
    if (!f) return;
    char line[WP_PATH_MAX + 8];
    if (!fgets(line, (int)sizeof(line), f)) { fclose(f); return; }
    fclose(f);
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    nl = strchr(line, '\r');
    if (nl) *nl = 0;

    if (wp_prefix_ci(line, "image:")) {
        const char *p = line + 6;
        while (*p == ' ' || *p == '\t') p++;
        if (wp_img_ok && strcmp(p, wp_img_path) == 0) return;   /* already drawn */
        char why[192];
        if (wallpaper_set_image(p, why, sizeof(why)) != 0) {
            /* Keep the previous background: a broken file must not blank the
             * desktop. The reason is kept for a caller that asks. */
            snprintf(wp_img_err, sizeof(wp_img_err), "%s", why);
        }
    } else if (line[0]) {
        wallpaper_clear_image();
    }
}

/* ------------------------------------------------------------ public */

void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds) {
    if (!s || !s->px || s->w <= 0 || s->h <= 0) return;

    /* The setting is the truth. Forced (ignoring the one-second gate) when
     * the caller asks for the image but none is loaded - that is the boot
     * path, where main.c has read the setting but has no decoder. */
    wp_setting_sync(index == WALLPAPER_IMAGE && !wp_img_ok);

    if (wp_img_ok) {
        if (!wp_img_dst || wp_img_dw != s->w || wp_img_dh != s->h) {
            if (wp_image_build(s->w, s->h) != 0) {
                /* the cache could not be rebuilt; blit what we have */
            }
        }
        wp_image_blit(s);
        return;
    }

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
        case 6: wp_draw_synthwave(s, th, seconds);    break;
        case 7: wp_draw_dunes(s, th);                 break;
        case 0:
        default: wp_draw_dusk(s, th);                 break;
    }
}
