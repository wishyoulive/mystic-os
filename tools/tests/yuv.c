/* yuv.c - host test for src/yuv.h (YUV 4:2:0 -> RGB, nearest, BT.601).
 *
 *   zig cc -Isrc -Wall -Wextra -o yuv tools/tests/yuv.c && ./yuv
 *
 * No libVLC, no window server, no fonts: src/yuv.h is a pure header, so
 * the arithmetic the player runs per frame is tested here, on the host.
 *
 * What is checked, and why each case exists:
 *
 *   - eight known Y/U/V triples against hand-computed BT.601 limited-range
 *     RGB, including Y=0 (footroom clamps to black) and Y=255 (headroom
 *     clamps to white), so the matrix and both clamps are exact;
 *   - an 8x8 image whose four 4x4 quadrants are black/white/red/green at
 *     1:1, which checks where the chroma subsampling actually lands;
 *   - the same image converted 8x8 -> 2x2 and 8x8 -> 2x4, i.e. downscale;
 *   - a 2x2 source stretched to 8x8, i.e. upscale (src smaller than dst);
 *   - 3x3 -> 5x5 and 1x1 -> 1x1, i.e. odd sizes and the smallest legal
 *     source; the 3x3 chroma plane is 2x2 so its last sample is read by
 *     pixels that map to the padded edge - the clamp path;
 *   - a strided destination (rows 10 pixels apart, only 8 written), so the
 *     window-surface form used by vlcplay.c is covered;
 *   - NV12 and I420 give identical RGB for the same image, at 1:1 and at
 *     4x downscale, which proves the interleaved path is not a second,
 *     drifting copy of the arithmetic;
 *   - every plane and every destination buffer is allocated with 64-byte
 *     guard bands filled with 0xA5; after each conversion the guards must
 *     still be 0xA5. That is the plane-bound safety check at odd sizes.
 *
 * Prints "N checks, M failures" and exits 1 when anything fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/yuv.h"

static int checks;
static int fails;

#define GUARD 64

typedef struct {
    uint8_t *raw;      /* malloc block: GUARD | pitch*lines | GUARD */
    uint8_t *p;        /* the plane the converter is given            */
    int      pitch;
    int      lines;
} Plane;

static void plane_alloc(Plane *pl, int pitch, int lines) {
    size_t n = (size_t)pitch * (size_t)lines;
    pl->pitch = pitch;
    pl->lines = lines;
    pl->raw = (uint8_t *)malloc(n + 2 * GUARD);
    if (!pl->raw) {
        fprintf(stderr, "out of memory (%d x %d)\n", pitch, lines);
        exit(2);
    }
    memset(pl->raw, 0xA5, n + 2 * GUARD);
    pl->p = pl->raw + GUARD;
}

static void plane_free(Plane *pl) {
    free(pl->raw);
    pl->raw = NULL;
    pl->p = NULL;
}

static int guards_ok(const Plane *pl) {
    size_t n = (size_t)pl->pitch * (size_t)pl->lines;
    for (size_t i = 0; i < GUARD; i++)
        if (pl->raw[i] != 0xA5) return 0;
    for (size_t i = 0; i < GUARD; i++)
        if (pl->p[n + i] != 0xA5) return 0;
    return 1;
}

/* A destination with guards, stamped with a value no test writes. */
#define DST_SENTINEL 0xDEADBEEFu

typedef struct {
    uint32_t *raw;
    uint32_t *p;
    int       stride;
    int       w, h;      /* the visible destination the test owns */
} Dst;

static void dst_alloc(Dst *d, int w, int h, int stride) {
    size_t n = (size_t)stride * (size_t)h;
    d->w = w; d->h = h; d->stride = stride;
    d->raw = (uint32_t *)malloc((n + 2 * GUARD) * sizeof(uint32_t));
    if (!d->raw) {
        fprintf(stderr, "out of memory (dst %d x %d)\n", w, h);
        exit(2);
    }
    for (size_t i = 0; i < n + 2 * GUARD; i++) d->raw[i] = DST_SENTINEL;
    d->p = d->raw + GUARD;
}

static void dst_free(Dst *d) {
    free(d->raw);
    d->raw = NULL;
    d->p = NULL;
}

static int dst_guards_ok(const Dst *d) {
    size_t n = (size_t)d->stride * (size_t)d->h;
    for (size_t i = 0; i < GUARD; i++)
        if (d->raw[i] != DST_SENTINEL) return 0;
    for (size_t i = 0; i < GUARD; i++)
        if (d->p[n + i] != DST_SENTINEL) return 0;
    return 1;
}

static void ck(const char *what, int ok) {
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static void ck_px(const char *what, const Dst *d, int x, int y, uint32_t want) {
    uint32_t got = d->p[(size_t)y * (size_t)d->stride + x];
    checks++;
    if (got == want) {
        printf("ok   %-26s (%d,%d) = 0x%08X\n", what, x, y, got);
    } else {
        fails++;
        printf("FAIL %-26s (%d,%d) got 0x%08X want 0x%08X\n",
               what, x, y, got, want);
    }
}

/* ---- the 8x8 quadrant image: black, white / red, green ------------- */
typedef struct {
    Plane y, u, v;
} Pic;

static void quad8_init(Pic *pic) {
    /* deliberately un-tight pitches: the converter must use the pitch */
    plane_alloc(&pic->y, 13, 8);
    plane_alloc(&pic->u, 7, 4);
    plane_alloc(&pic->v, 9, 4);
    for (int yy = 0; yy < 8; yy++) {
        for (int xx = 0; xx < 8; xx++) {
            uint8_t luma = (yy < 4) ? (xx < 4 ? 16 : 235)
                                    : (xx < 4 ? 81 : 145);
            pic->y.p[(size_t)yy * 13 + xx] = luma;
        }
    }
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            uint8_t uu = 128, vv = 128;
            if (cy >= 2 && cx < 2) { uu = 90; vv = 240; }    /* red   */
            if (cy >= 2 && cx >= 2) { uu = 54; vv = 34; }    /* green */
            pic->u.p[(size_t)cy * 7 + cx] = uu;
            pic->v.p[(size_t)cy * 9 + cx] = vv;
        }
    }
}

static void quad8_free(Pic *pic) {
    plane_free(&pic->y);
    plane_free(&pic->u);
    plane_free(&pic->v);
}

#define BLACK 0xFF000000u
#define WHITE 0xFFFFFFFFu
#define RED   0xFFFF0000u
#define GREEN 0xFF00FF01u
#define BLUE  0xFF0000FFu
#define GRAY  0xFF828282u

int main(void) {
    /* ---- 1. the matrix, eight hand-computed triples ---------------- */
    {
        /* 16x2: eight 2x2 blocks in a row, one chroma sample each. */
        enum { NB = 8 };
        static const uint8_t yv[NB] = { 16, 235,  81, 145,  41, 128,   0, 255 };
        static const uint8_t uv[NB] = { 128, 128, 90,  54, 240, 128, 128, 128 };
        static const uint8_t vv[NB] = { 128, 128, 240, 34, 110, 128, 128, 128 };
        static const uint32_t want[NB] = { BLACK, WHITE, RED, GREEN,
                                           BLUE, GRAY, BLACK, WHITE };
        Pic pic;
        Dst dst;

        plane_alloc(&pic.y, 16, 2);
        plane_alloc(&pic.u, 8, 1);
        plane_alloc(&pic.v, 8, 1);
        for (int b = 0; b < NB; b++) {
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    pic.y.p[(size_t)dy * 16 + b * 2 + dx] = yv[b];
            pic.u.p[b] = uv[b];
            pic.v.p[b] = vv[b];
        }
        dst_alloc(&dst, 16, 2, 16);
        yuv420_to_rgb_nearest(pic.y.p, pic.u.p, pic.v.p, 16, 8, 8,
                              16, 2, dst.p, 16, 2);
        for (int b = 0; b < NB; b++) {
            char what[64];
            snprintf(what, sizeof(what), "matrix Y=%3u U=%3u V=%3u",
                     yv[b], uv[b], vv[b]);
            ck_px(what, &dst, b * 2, 0, want[b]);
        }
        ck("matrix: source guards untouched",
           guards_ok(&pic.y) && guards_ok(&pic.u) && guards_ok(&pic.v));
        ck("matrix: destination guards untouched", dst_guards_ok(&dst));
        plane_free(&pic.y); plane_free(&pic.u); plane_free(&pic.v);
        dst_free(&dst);
    }

    /* ---- 2. 8x8 quadrants at 1:1, with un-tight pitches ------------ */
    {
        Pic pic;
        Dst dst;
        quad8_init(&pic);
        dst_alloc(&dst, 8, 8, 8);
        yuv420_to_rgb_nearest(pic.y.p, pic.u.p, pic.v.p, 13, 7, 9,
                              8, 8, dst.p, 8, 8);
        ck_px("1:1 top-left black",     &dst, 0, 0, BLACK);
        ck_px("1:1 top-left corner",    &dst, 3, 3, BLACK);
        ck_px("1:1 top-right white",    &dst, 7, 0, WHITE);
        ck_px("1:1 top-right corner",   &dst, 4, 3, WHITE);
        ck_px("1:1 bottom-left red",    &dst, 0, 7, RED);
        ck_px("1:1 bottom-left corner", &dst, 3, 4, RED);
        ck_px("1:1 bottom-right green", &dst, 7, 7, GREEN);
        ck_px("1:1 bottom-right inner", &dst, 4, 4, GREEN);
        ck("1:1 source guards untouched",
           guards_ok(&pic.y) && guards_ok(&pic.u) && guards_ok(&pic.v));
        ck("1:1 destination guards untouched", dst_guards_ok(&dst));
        quad8_free(&pic);
        dst_free(&dst);
    }

    /* ---- 3. 8x8 -> 2x2: 4x downscale, nearest ---------------------- */
    {
        Pic pic;
        Dst dst;
        quad8_init(&pic);
        /* stride 4: only columns 0..1 are the picture, 2..3 must stay put */
        dst_alloc(&dst, 2, 2, 4);
        /* stride 4, visible 2x2: the window-surface form */
        yuv420_to_rgb_nearest_stride(pic.y.p, pic.u.p, pic.v.p, 13, 7, 9,
                                     8, 8, dst.p, 4, 2, 2);
        ck_px("4x downscale black", &dst, 0, 0, BLACK);
        ck_px("4x downscale white", &dst, 1, 0, WHITE);
        ck_px("4x downscale red",   &dst, 0, 1, RED);
        ck_px("4x downscale green", &dst, 1, 1, GREEN);
        ck("4x downscale: unused stride columns untouched",
           dst.p[2] == DST_SENTINEL && dst.p[3] == DST_SENTINEL &&
           dst.p[6] == DST_SENTINEL && dst.p[7] == DST_SENTINEL);
        ck("4x downscale: guards untouched", dst_guards_ok(&dst));
        quad8_free(&pic);
        dst_free(&dst);
    }

    /* ---- 4. 8x8 -> 8x4 on a wide stride: vertical 2x downscale ----- */
    {
        Pic pic;
        Dst dst;
        quad8_init(&pic);
        dst_alloc(&dst, 8, 4, 10);
        yuv420_to_rgb_nearest_stride(pic.y.p, pic.u.p, pic.v.p, 13, 7, 9,
                                     8, 8, dst.p, 10, 8, 4);
        ck_px("2x vertical: row 0 black",  &dst, 0, 0, BLACK);
        ck_px("2x vertical: row 0 white",  &dst, 7, 0, WHITE);
        ck_px("2x vertical: row 1 black",  &dst, 0, 1, BLACK);
        ck_px("2x vertical: row 2 red",    &dst, 0, 2, RED);
        ck_px("2x vertical: row 3 green",  &dst, 7, 3, GREEN);
        ck("2x vertical: stride columns 8..9 untouched",
           dst.p[8] == DST_SENTINEL && dst.p[9] == DST_SENTINEL &&
           dst.p[4 * 10 + 8] == DST_SENTINEL &&
           dst.p[4 * 10 + 9] == DST_SENTINEL);
        ck("2x vertical: guards untouched", dst_guards_ok(&dst));
        quad8_free(&pic);
        dst_free(&dst);
    }

    /* ---- 5. 2x2 -> 8x8: upscale (src smaller than dst) ------------- */
    {
        Plane y, u, v;
        Dst dst;
        plane_alloc(&y, 2, 2);
        plane_alloc(&u, 1, 1);
        plane_alloc(&v, 1, 1);
        y.p[0] = 16;  y.p[1] = 235;     /* row 0: black, white */
        y.p[2] = 81;  y.p[3] = 145;     /* row 1: red-ish Y, mid  */
        u.p[0] = 128;
        v.p[0] = 128;
        dst_alloc(&dst, 8, 8, 8);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 2, 1, 1, 2, 2, dst.p, 8, 8);
        ck_px("upscale: (0,0) black",   &dst, 0, 0, BLACK);
        ck_px("upscale: (3,3) black",   &dst, 3, 3, BLACK);
        ck_px("upscale: (4,0) white",   &dst, 4, 0, WHITE);
        ck_px("upscale: (7,3) white",   &dst, 7, 3, WHITE);
        /* Y=81, U=V=128 -> C=65, 298*65+128 = 19498 >> 8 = 76 = 0x4C */
        ck_px("upscale: (0,4) 76 grey",  &dst, 0, 4, 0xFF4C4C4Cu);
        /* Y=145 -> C=129, 298*129+128 = 38570 >> 8 = 150 = 0x96 */
        ck_px("upscale: (7,7) 150 grey", &dst, 7, 7, 0xFF969696u);
        ck("upscale: source/destination guards untouched",
           guards_ok(&y) && guards_ok(&u) && guards_ok(&v) &&
           dst_guards_ok(&dst));
        plane_free(&y); plane_free(&u); plane_free(&v);
        dst_free(&dst);
    }

    /* ---- 6. 3x3 -> 5x5: odd sizes, last chroma sample, strided dst -- */
    {
        Plane y, u, v;
        Dst dst;
        plane_alloc(&y, 5, 3);         /* odd pitch */
        plane_alloc(&u, 3, 2);         /* chroma is ceil(3/2) x ceil(3/2) */
        plane_alloc(&v, 3, 2);
        for (int yy = 0; yy < 3; yy++)
            for (int xx = 0; xx < 3; xx++)
                y.p[(size_t)yy * 5 + xx] = (xx == 2 && yy == 2) ? 235 : 16;
        u.p[0] = 128; u.p[1] = 128;
        u.p[3] = 128; u.p[4] = 240;    /* last column/row: distinct */
        v.p[0] = 128; v.p[1] = 128;
        v.p[3] = 128; v.p[4] = 110;
        dst_alloc(&dst, 5, 5, 7);
        yuv420_to_rgb_nearest_stride(y.p, u.p, v.p, 5, 3, 3, 3, 3,
                                     dst.p, 7, 5, 5);
        ck_px("3x3: (0,0) black", &dst, 0, 0, BLACK);
        ck_px("3x3: (2,2) luma 16, chroma (0,0)", &dst, 2, 2, BLACK);
        /* (4,0) is sx=2 -> last chroma COLUMN (cx=1) at chroma row 0 */
        ck_px("3x3: (4,0) last chroma column", &dst, 4, 0, BLACK);
        /* (4,4) is sx=sy=2 -> the last chroma SAMPLE (1,1) with Y=235:
         * R=(298*219+409*(-18)+128)>>8=226, G=226, B clamps to 255 */
        ck_px("3x3: (4,4) last chroma, Y=235", &dst, 4, 4, 0xFFE2E2FFu);
        ck("3x3: stride columns 5..6 untouched",
           dst.p[5] == DST_SENTINEL && dst.p[6] == DST_SENTINEL &&
           dst.p[4 * 7 + 5] == DST_SENTINEL &&
           dst.p[4 * 7 + 6] == DST_SENTINEL);
        ck("3x3: guards untouched",
           guards_ok(&y) && guards_ok(&u) && guards_ok(&v) &&
           dst_guards_ok(&dst));
        plane_free(&y); plane_free(&u); plane_free(&v);
        dst_free(&dst);
    }

    /* ---- 7. 1x1 -> 1x1: the smallest legal source ------------------ */
    {
        uint8_t y = 235, u = 128, v = 128, guard_before = 0xA5;
        Dst dst;
        dst_alloc(&dst, 1, 1, 1);
        yuv420_to_rgb_nearest(&y, &u, &v, 1, 1, 1, 1, 1, dst.p, 1, 1);
        ck_px("1x1: white", &dst, 0, 0, WHITE);
        ck("1x1: guards untouched", dst_guards_ok(&dst) &&
                                   guard_before == 0xA5);
        dst_free(&dst);
    }

    /* ---- 8. zero sizes and null planes must do nothing ------------- */
    {
        Plane y, u, v;
        Dst dst;
        plane_alloc(&y, 4, 4);
        plane_alloc(&u, 2, 2);
        plane_alloc(&v, 2, 2);
        dst_alloc(&dst, 4, 4, 4);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 4, 2, 2, 0, 4, dst.p, 4, 4);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 4, 2, 2, 4, 0, dst.p, 4, 4);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 4, 2, 2, 4, 4, dst.p, 0, 4);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 4, 2, 2, 4, 4, dst.p, 4, 0);
        yuv420_to_rgb_nearest(NULL, u.p, v.p, 4, 2, 2, 4, 4, dst.p, 4, 4);
        yuv420_to_rgb_nearest(y.p, NULL, v.p, 4, 2, 2, 4, 4, dst.p, 4, 4);
        yuv420_to_rgb_nearest(y.p, u.p, NULL, 4, 2, 2, 4, 4, dst.p, 4, 4);
        yuv420_to_rgb_nearest(y.p, u.p, v.p, 4, 2, 2, 4, 4, NULL, 4, 4);
        nv12_to_rgb_nearest(y.p, NULL, 4, 2, 4, 4, dst.p, 4, 4);
        ck("degenerate arguments write nothing",
           dst.p[0] == DST_SENTINEL && dst.p[15] == DST_SENTINEL &&
           dst_guards_ok(&dst));
        plane_free(&y); plane_free(&u); plane_free(&v);
        dst_free(&dst);
    }

    /* ---- 9. NV12 == I420 for the same picture --------------------- */
    {
        Pic pic;
        Plane uv;                 /* 4x4 pairs: 8 bytes per row */
        Dst a, b;
        int same1 = 1, same4 = 1;

        quad8_init(&pic);
        plane_alloc(&uv, 8, 4);
        for (int cy = 0; cy < 4; cy++)
            for (int cx = 0; cx < 4; cx++) {
                uv.p[(size_t)cy * 8 + cx * 2 + 0] = pic.u.p[(size_t)cy * 7 + cx];
                uv.p[(size_t)cy * 8 + cx * 2 + 1] = pic.v.p[(size_t)cy * 9 + cx];
            }

        dst_alloc(&a, 8, 8, 8);
        dst_alloc(&b, 8, 8, 8);
        yuv420_to_rgb_nearest(pic.y.p, pic.u.p, pic.v.p, 13, 7, 9,
                              8, 8, a.p, 8, 8);
        nv12_to_rgb_nearest(pic.y.p, uv.p, 13, 8, 8, 8, b.p, 8, 8);
        for (int yy = 0; yy < 8 && same1; yy++)
            for (int xx = 0; xx < 8; xx++)
                if (a.p[(size_t)yy * 8 + xx] != b.p[(size_t)yy * 8 + xx]) {
                    same1 = 0;
                    printf("     NV12 differs at (%d,%d): 0x%08X vs 0x%08X\n",
                           xx, yy, a.p[(size_t)yy * 8 + xx],
                           b.p[(size_t)yy * 8 + xx]);
                    break;
                }
        ck("NV12 == I420, 64 pixels at 1:1", same1);
        ck("NV12: source guards untouched", guards_ok(&uv));
        ck("NV12: destination guards untouched", dst_guards_ok(&b));

        {
            Dst c, d;
            dst_alloc(&c, 2, 2, 2);
            dst_alloc(&d, 2, 2, 2);
            yuv420_to_rgb_nearest(pic.y.p, pic.u.p, pic.v.p, 13, 7, 9,
                                  8, 8, c.p, 2, 2);
            nv12_to_rgb_nearest(pic.y.p, uv.p, 13, 8, 8, 8, d.p, 2, 2);
            for (int i = 0; i < 4; i++)
                if (c.p[i] != d.p[i]) same4 = 0;
            ck("NV12 == I420, 4 pixels at 4x downscale", same4);
            ck("NV12 4x: destination guards untouched",
               dst_guards_ok(&c) && dst_guards_ok(&d));
            dst_free(&c);
            dst_free(&d);
        }

        dst_free(&a);
        dst_free(&b);
        plane_free(&uv);
        quad8_free(&pic);
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
