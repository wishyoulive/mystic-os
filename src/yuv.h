/* ============================================================
 *  Mystic OS - yuv.h
 *
 *  YUV 4:2:0 -> 0xAARRGGBB, NEAREST NEIGHBOUR, INTEGER ONLY.
 *
 *  This exists for one reason: libVLC's "vmem" video output can hand
 *  the player the decoder's own I420 (or NV12) planes at the decoder's
 *  own size, and then the player converts only the pixels it actually
 *  displays. A 4K frame is 3840x2160 = 8,294,400 pixels; a 960x540
 *  window body is 518,400. Sampling at the destination reads the
 *  source 16 times less often than converting the whole frame, and
 *  the work happens on the window thread instead of in libVLC's single
 *  video-output thread. That is the whole point of this file.
 *
 *  NEAREST NEIGHBOUR IS A DELIBERATE TRADE, NOT A QUALITY CLAIM.
 *  At a 4:1 downscale it picks every fourth source pixel, so fine
 *  detail can alias. The alternative (a box filter or bilinear taps)
 *  costs 4 or more source reads per output pixel; it is a possible
 *  later refinement behind the same interface, not something this
 *  function pretends to do.
 *
 *  COLOUR: BT.601, LIMITED RANGE ("TV range"), the matrix every SD
 *  YUV 4:2:0 stream is defined in and the default libVLC uses when a
 *  stream does not say otherwise:
 *
 *      C = Y - 16            D = U - 128            E = V - 128
 *      R = (298*C + 409*E            + 128) >> 8
 *      G = (298*C - 100*D - 208*E    + 128) >> 8
 *      B = (298*C + 516*D            + 128) >> 8
 *
 *  All integer, no floating point per pixel, and the rounding term is
 *  +128 so the shift rounds to nearest. Y below 16 clamps to 0 (the
 *  footroom value), and each channel clamps to 0..255. These are the
 *  coefficients from ITU-R BT.601-7 Table 2 (the 8-bit form used by
 *  libavcodec's yuv2rgb and by every standard decoder).
 *
 *  A stream tagged BT.709 (most HD) or full range is NOT detected
 *  here: libVLC 3's vmem API has no colour-space field, so this fixed
 *  matrix is the honest, stated limitation of the path. A BT.709
 *  source is displayed with slightly different hues; the alternative
 *  would be guessing a matrix from the resolution, which is how
 *  players get colour wrong silently.
 *
 *  PURE FUNCTIONS. No libVLC, no malloc, no globals, no I/O: the host
 *  test tools/tests/yuv.c compiles this header alone and checks exact
 *  pixel values, upscaling, odd sizes and plane-bound safety.
 *
 *  Every function tolerates sw/sh/dw/dh of 0 (it does nothing), any
 *  positive pitch, and a source smaller or larger than the
 *  destination. Chroma reads are clamped to ceil(sw/2) x ceil(sh/2)
 *  and ceil(sh/2), so an odd width or height can never read past the
 *  last chroma row/column.
 * ============================================================ */
#ifndef MYSTIC_YUV_H
#define MYSTIC_YUV_H

#include <stdint.h>

/* 0xAARRGGBB, the Mystic surface format (src/mystic.h RGB()). */
#define YUV_A 0xFF000000u

static inline uint32_t yuv_pack(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return YUV_A | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* One BT.601 limited-range pixel from an 8-bit Y and 8-bit U/V pair.
 * Split out so the two plane layouts (I420, NV12) share the exact
 * arithmetic and cannot drift apart. */
static inline uint32_t yuv_pixel_bt601(int yy, int uu, int vv) {
    int c = yy - 16;
    int d = uu - 128;
    int e = vv - 128;
    int r, g, b;
    if (c < 0) c = 0;                    /* footroom: 0..15 is black */
    r = (298 * c + 409 * e + 128) >> 8;
    g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b = (298 * c + 516 * d + 128) >> 8;
    return yuv_pack(r, g, b);
}

/* The one conversion loop. `nv12` selects the chroma layout:
 *
 *   0: u and v are separate planes, one sample per 2x2 luma block;
 *   1: `u` points at the interleaved UV plane and `v` is unused
 *      (U at even byte offsets, V at odd ones).
 *
 * For every destination pixel (dx,dy):
 *
 *   sy = dy * sh / dh          (exact integer scale, 64-bit product)
 *   sx = dx * sw / dw          (16.16 fixed point, incremented per pixel)
 *   U  = plane[sx/2, sy/2]     (or UV[2*(sx/2), sy/2] for NV12)
 *
 * The destination row is dst + dy*dstride; `dstride` is in PIXELS, the
 * same unit as Surface.stride. No clipping is done here: the caller owns
 * the destination rectangle and must keep dy < dh within it. */
static inline void yuv420_to_rgb_nearest_impl(const uint8_t *y,
                                              const uint8_t *u,
                                              const uint8_t *v,
                                              int ypitch, int upitch,
                                              int vpitch,
                                              int sw, int sh,
                                              uint32_t *dst, int dstride,
                                              int dw, int dh, int nv12) {
    uint32_t xstep;
    int cw, ch;
    int yy;

    if (!y || !u || !dst) return;
    if (!nv12 && !v) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (ypitch <= 0 || upitch <= 0) return;
    if (!nv12 && vpitch <= 0) return;

    cw = (sw + 1) >> 1;                  /* chroma columns  */
    ch = (sh + 1) >> 1;                  /* chroma rows     */
    /* floor(sw*65536/dw); the integer add below is the per-pixel scale.
     * 64-bit so an 8K source cannot overflow the shift. */
    xstep = (uint32_t)(((uint64_t)(uint32_t)sw << 16) / (uint64_t)(uint32_t)dw);

    for (yy = 0; yy < dh; yy++) {
        int sy = (int)(((uint64_t)(uint32_t)yy * (uint64_t)(uint32_t)sh) /
                       (uint64_t)(uint32_t)dh);
        int cy;
        const uint8_t *yrow;
        const uint8_t *urow;
        const uint8_t *vrow = 0;
        uint32_t *drow;
        uint32_t fx = 0;
        int xx;

        if (sy >= sh) sy = sh - 1;       /* only on impossible rounding */
        cy = sy >> 1;
        if (cy >= ch) cy = ch - 1;       /* odd sh: clamp to last row */

        yrow = y + (size_t)sy * (size_t)ypitch;
        urow = u + (size_t)cy * (size_t)upitch;
        if (!nv12) vrow = v + (size_t)cy * (size_t)vpitch;
        drow = dst + (size_t)yy * (size_t)dstride;

        for (xx = 0; xx < dw; xx++) {
            int sx = (int)(fx >> 16);
            int cx;
            int uu, vv;

            if (sx >= sw) sx = sw - 1;   /* defensive: floor() can't go past */
            cx = sx >> 1;
            if (cx >= cw) cx = cw - 1;   /* odd sw: clamp to last column */

            if (nv12) {
                const uint8_t *p = urow + ((size_t)cx << 1);
                uu = p[0];
                vv = p[1];
            } else {
                uu = urow[cx];
                vv = vrow[cx];
            }
            drow[xx] = yuv_pixel_bt601(yrow[sx], uu, vv);
            fx += xstep;
        }
    }
}

/* I420 (Y, U, V planar) into a tightly packed destination. */
static inline void yuv420_to_rgb_nearest(const uint8_t *y, const uint8_t *u,
                                         const uint8_t *v, int ypitch,
                                         int upitch, int vpitch, int sw,
                                         int sh, uint32_t *dst, int dw,
                                         int dh) {
    yuv420_to_rgb_nearest_impl(y, u, v, ypitch, upitch, vpitch, sw, sh,
                               dst, dw, dw, dh, 0);
}

/* I420 into a destination whose rows are `dstride` pixels apart - the
 * window surface's stride, which is usually wider than the video rect. */
static inline void yuv420_to_rgb_nearest_stride(const uint8_t *y,
                                                const uint8_t *u,
                                                const uint8_t *v, int ypitch,
                                                int upitch, int vpitch,
                                                int sw, int sh,
                                                uint32_t *dst, int dstride,
                                                int dw, int dh) {
    yuv420_to_rgb_nearest_impl(y, u, v, ypitch, upitch, vpitch, sw, sh,
                               dst, dstride, dw, dh, 0);
}

/* NV12 (Y plane, then interleaved UV pairs) into a packed destination. */
static inline void nv12_to_rgb_nearest(const uint8_t *y, const uint8_t *uv,
                                       int ypitch, int uvpitch, int sw,
                                       int sh, uint32_t *dst, int dw,
                                       int dh) {
    yuv420_to_rgb_nearest_impl(y, uv, 0, ypitch, uvpitch, 0, sw, sh,
                               dst, dw, dw, dh, 1);
}

/* NV12 into a strided destination. */
static inline void nv12_to_rgb_nearest_stride(const uint8_t *y,
                                              const uint8_t *uv, int ypitch,
                                              int uvpitch, int sw, int sh,
                                              uint32_t *dst, int dstride,
                                              int dw, int dh) {
    yuv420_to_rgb_nearest_impl(y, uv, 0, ypitch, uvpitch, 0, sw, sh,
                               dst, dstride, dw, dh, 1);
}

#endif /* MYSTIC_YUV_H */
