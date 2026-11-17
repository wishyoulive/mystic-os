/* ============================================================
 *  Mystic OS - gfx.c
 *  All drawing is done by the CPU, on purpose: no GPU drivers,
 *  so this runs on anything from a 2008 laptop to a VM.
 *
 *  Signed-distance shapes give us smooth edges without a GPU:
 *  for every pixel we ask "how far is this pixel from the shape",
 *  and use that distance as coverage.
 * ============================================================ */
#include "mystic.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------- colour */
u32 color_mix(u32 a, u32 b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    float ia = 1.0f - t;
    int r = (int)(GET_R(a) * ia + GET_R(b) * t + 0.5f);
    int g = (int)(GET_G(a) * ia + GET_G(b) * t + 0.5f);
    int bl = (int)(GET_B(a) * ia + GET_B(b) * t + 0.5f);
    int al = (int)(GET_A(a) * ia + GET_A(b) * t + 0.5f);
    return ((u32)al << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

u32 color_alpha(u32 c, float a) {
    int al = (int)(GET_A(c) * a + 0.5f);
    if (al > 255) al = 255;
    if (al < 0) al = 0;
    return WITH_A(c, al);
}

/* ---------------------------------------------------------- surface */
void surf_fill(Surface *s, int x, int y, int w, int h, u32 c) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w;
    int y1 = y + h > s->h ? s->h : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = s->px + (size_t)yy * s->stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

/* alpha blend a solid colour over a rectangle */
void surf_blend(Surface *s, int x, int y, int w, int h, u32 c) {
    int a = GET_A(c);
    if (a == 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w;
    int y1 = y + h > s->h ? s->h : y + h;
    if (a == 255) { surf_fill(s, x0, y0, x1 - x0, y1 - y0, c | 0xFF000000u); return; }
    int r = GET_R(c), g = GET_G(c), b = GET_B(c);
    int ia = 255 - a;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = s->px + (size_t)yy * s->stride;
        for (int xx = x0; xx < x1; xx++) {
            u32 d = row[xx];
            int dr = GET_R(d), dg = GET_G(d), db = GET_B(d);
            row[xx] = 0xFF000000u
                    | (((u32)((r * a + dr * ia) / 255)) << 16)
                    | (((u32)((g * a + dg * ia) / 255)) << 8)
                    |  ((u32)((b * a + db * ia) / 255));
        }
    }
}

void surf_copy(Surface *dst, Surface *src) {
    int w = dst->w < src->w ? dst->w : src->w;
    int h = dst->h < src->h ? dst->h : src->h;
    for (int y = 0; y < h; y++)
        memcpy(dst->px + (size_t)y * dst->stride, src->px + (size_t)y * src->stride,
               (size_t)w * 4);
}

void surf_push(Surface *dst, int dx, int dy, Surface *src, int sx, int sy, int w, int h) {
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        memcpy(dst->px + (size_t)(dy + y) * dst->stride + dx,
               src->px + (size_t)(sy + y) * src->stride + sx, (size_t)w * 4);
}

/* ---------------------------------------------------------- gradients */
void gfx_gradient(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w;
    int y1 = y + h > s->h ? s->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        float t = (h > 1) ? (float)(yy - y) / (float)(h - 1) : 0.0f;
        u32 c = color_mix(top, bottom, t) | 0xFF000000u;
        u32 *row = s->px + (size_t)yy * s->stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

/* a soft round light, like light bleeding through glass */
void gfx_glow(Surface *s, float cx, float cy, float radius, u32 color, float strength) {
    int x0 = (int)(cx - radius), x1 = (int)(cx + radius);
    int y0 = (int)(cy - radius), y1 = (int)(cy + radius);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    float r2 = radius * radius;
    int cr = GET_R(color), cg = GET_G(color), cb = GET_B(color);
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = s->px + (size_t)yy * s->stride;
        float dy = (float)yy - cy;
        for (int xx = x0; xx < x1; xx++) {
            float dx = (float)xx - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            float t = 1.0f - d2 / r2;
            float a = t * t * strength;
            if (a <= 0.002f) continue;
            if (a > 1.0f) a = 1.0f;
            u32 d = row[xx];
            int ia = (int)((1.0f - a) * 255.0f);
            int aa = 255 - ia;
            row[xx] = 0xFF000000u
                    | ((u32)((cr * aa + GET_R(d) * ia) / 255) << 16)
                    | ((u32)((cg * aa + GET_G(d) * ia) / 255) << 8)
                    |  (u32)((cb * aa + GET_B(d) * ia) / 255);
        }
    }
}

/* distance from a point to a rounded rectangle */
static float rrect_dist(float px, float py, float x, float y, float w, float h, float r) {
    float halfw = w * 0.5f, halfh = h * 0.5f;
    if (r > halfw) r = halfw;
    if (r > halfh) r = halfh;
    float cx = x + halfw, cy = y + halfh;
    float qx = fabsf(px - cx) - (halfw - r);
    float qy = fabsf(py - cy) - (halfh - r);
    float ax = qx > 0 ? qx : 0.0f;
    float ay = qy > 0 ? qy : 0.0f;
    float outside = sqrtf(ax * ax + ay * ay);
    float inside = (qx > qy ? qx : qy);
    if (inside > 0) inside = 0;
    return outside + inside - r;
}

void gfx_rrect(Surface *s, float x, float y, float w, float h, float radius, u32 color) {
    gfx_rrect_aa(s, x, y, w, h, radius, color, 0, 0);
}

/* filled rounded rect with a 1px-ish border and antialiased edges */
void gfx_rrect_aa(Surface *s, float x, float y, float w, float h, float radius,
                  u32 fill, u32 border, float border_w) {
    int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + w) + 1;
    int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + h) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;

    int fr = GET_R(fill), fg = GET_G(fill), fb = GET_B(fill), fa = GET_A(fill);
    int br = GET_R(border), bg = GET_G(border), bb = GET_B(border), ba = GET_A(border);

    for (int yy = y0; yy < y1; yy++) {
        u32 *row = s->px + (size_t)yy * s->stride;
        float py = (float)yy + 0.5f;
        for (int xx = x0; xx < x1; xx++) {
            float px = (float)xx + 0.5f;
            float d = rrect_dist(px, py, x, y, w, h, radius);
            float cov = 0.5f - d;                 /* antialiased coverage */
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float a = (fa / 255.0f) * cov;
            float brd = 0.0f;
            if (border_w > 0.0f && ba > 0) {
                /* coverage of the border band: inside edge minus outside */
                float inner = rrect_dist(px, py, x + border_w, y + border_w,
                                         w - 2 * border_w, h - 2 * border_w,
                                         radius - border_w > 0 ? radius - border_w : 0);
                float bd = 0.5f - inner;
                if (bd > 1.0f) bd = 1.0f;
                if (bd < 0.0f) bd = 0.0f;
                brd = (cov - bd) * (ba / 255.0f);
                a = (fa / 255.0f) * bd;
            }
            u32 dst = row[xx];
            int dr = GET_R(dst), dg = GET_G(dst), db = GET_B(dst);
            if (brd > 0.0001f) {
                dr = (int)(br * brd + dr * (1 - brd));
                dg = (int)(bg * brd + dg * (1 - brd));
                db = (int)(bb * brd + db * (1 - brd));
            }
            if (a > 0.0001f) {
                dr = (int)(fr * a + dr * (1 - a));
                dg = (int)(fg * a + dg * (1 - a));
                db = (int)(fb * a + db * (1 - a));
            }
            row[xx] = 0xFF000000u | ((u32)dr << 16) | ((u32)dg << 8) | (u32)db;
        }
    }
}

/* ---------------------------------------------------------- scratch
 * This is a single-threaded compositor, so scratch memory lives at file
 * scope and simply keeps the largest size it has ever needed. Every repaint
 * used to calloc and free a few megabytes per visible window; now the first
 * frame allocates and every later frame reuses the same pages, which are
 * already resident (a fresh calloc of 5 MB also costs the page faults that
 * come with touching brand new pages). Nothing here is thread safe, and
 * nothing here needs to be.
 */
static void *scratch_grow(void **buf, size_t *cap, size_t bytes) {
    if (bytes <= *cap) return *buf;
    size_t ncap = *cap ? *cap : 64 * 1024;
    while (ncap < bytes) {
        if (ncap > (size_t)-1 / 2) { ncap = bytes; break; }
        ncap *= 2;
    }
    void *p = realloc(*buf, ncap);
    if (!p) return NULL;              /* realloc keeps the old buffer intact */
    *buf = p;
    *cap = ncap;
    return p;
}

static u32   *shadow_mask;                       /* gfx_shadow's scratch surface */
static size_t shadow_mask_cap;                   /* bytes */

static float *blur_small, *blur_tmp;             /* gfx_blur_region's work buffers */
static size_t blur_small_cap, blur_tmp_cap;      /* bytes */

/* ---------------------------------------------------------- blur
 * Separable box blur, run on a 4x-downsampled copy. Three passes of a
 * box approximates a gaussian well enough that nobody can tell, and it
 * costs a fraction of the CPU. This is what makes the glass read as
 * glass: the backdrop is visibly, correctly blurred - not a flat tint.
 *
 * ch is how many colour channels are carried: 3 for a normal surface blur,
 * 1 for the shadow mask (whose red, green and blue are the same number) so
 * that a shadow pays a third of the arithmetic and a third of the memory.
 * Each channel is blurred completely independently, so the 3-channel maths
 * is unchanged, value for value.
 */
static void blur_run(Surface *s, int rx, int ry, int rw, int rh, int radius, int ch) {
    if (rw <= 2 || rh <= 2 || radius <= 0) return;
    int x0 = rx < 0 ? 0 : rx, y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw > s->w ? s->w : rx + rw;
    int y1 = ry + rh > s->h ? s->h : ry + rh;
    if (x1 <= x0 || y1 <= y0) return;

    const int F = 4;                     /* downsample factor */
    int w = x1 - x0, h = y1 - y0;
    int sw = (w + F - 1) / F, sh = (h + F - 1) / F;
    if (sw < 1 || sh < 1) return;

    size_t cells = (size_t)sw * (size_t)sh;
    if (cells / (size_t)sh != (size_t)sw) return;
    if (cells > (size_t)-1 / (sizeof(float) * 3)) return;
    size_t need = cells * (size_t)ch * sizeof(float);

    float *small = scratch_grow((void **)&blur_small, &blur_small_cap, need);
    float *tmp = scratch_grow((void **)&blur_tmp, &blur_tmp_cap, need);
    if (!small || !tmp) return;          /* out of memory: draw nothing, as before */

    /* downsample (box average) */
    if (ch == 3) {
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                int cnt = 0, sr = 0, sg = 0, sb = 0;
                for (int yy = 0; yy < F; yy++) {
                    int sy = y0 + y * F + yy;
                    if (sy >= y1) break;
                    u32 *row = s->px + (size_t)sy * s->stride;
                    for (int xx = 0; xx < F; xx++) {
                        int sx = x0 + x * F + xx;
                        if (sx >= x1) break;
                        u32 c = row[sx];
                        sr += GET_R(c); sg += GET_G(c); sb += GET_B(c);
                        cnt++;
                    }
                }
                if (cnt < 1) cnt = 1;
                float *o = small + ((size_t)y * sw + x) * 3;
                o[0] = (float)sr / cnt;
                o[1] = (float)sg / cnt;
                o[2] = (float)sb / cnt;
            }
        }
    } else {
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                int cnt = 0, sr = 0;
                for (int yy = 0; yy < F; yy++) {
                    int sy = y0 + y * F + yy;
                    if (sy >= y1) break;
                    u32 *row = s->px + (size_t)sy * s->stride;
                    for (int xx = 0; xx < F; xx++) {
                        int sx = x0 + x * F + xx;
                        if (sx >= x1) break;
                        sr += GET_R(row[sx]);
                        cnt++;
                    }
                }
                if (cnt < 1) cnt = 1;
                small[(size_t)y * sw + x] = (float)sr / cnt;
            }
        }
    }

    int r = radius / F;
    if (r < 1) r = 1;

    /* horizontal box, then vertical box, three times */
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                float acc[3] = {0, 0, 0};
                int n = 0;
                for (int k = -r; k <= r; k++) {
                    int sx = x + k;
                    if (sx < 0) sx = 0;
                    if (sx >= sw) sx = sw - 1;
                    const float *i = small + ((size_t)y * sw + sx) * ch;
                    for (int c = 0; c < ch; c++) acc[c] += i[c];
                    n++;
                }
                float *o = tmp + ((size_t)y * sw + x) * ch;
                for (int c = 0; c < ch; c++) o[c] = acc[c] / n;
            }
        }
        memcpy(small, tmp, cells * (size_t)ch * sizeof(float));
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                float acc[3] = {0, 0, 0};
                int n = 0;
                for (int k = -r; k <= r; k++) {
                    int sy = y + k;
                    if (sy < 0) sy = 0;
                    if (sy >= sh) sy = sh - 1;
                    const float *i = small + ((size_t)sy * sw + x) * ch;
                    for (int c = 0; c < ch; c++) acc[c] += i[c];
                    n++;
                }
                float *o = tmp + ((size_t)y * sw + x) * ch;
                for (int c = 0; c < ch; c++) o[c] = acc[c] / n;
            }
        }
        memcpy(small, tmp, cells * (size_t)ch * sizeof(float));
    }

    /* upsample (nearest, the blur hides it) */
    if (ch == 3) {
        for (int y = 0; y < h; y++) {
            u32 *row = s->px + (size_t)(y0 + y) * s->stride;
            int sy = y / F; if (sy >= sh) sy = sh - 1;
            for (int x = 0; x < w; x++) {
                int sx = x / F; if (sx >= sw) sx = sw - 1;
                size_t i = ((size_t)sy * sw + sx) * 3;
                int r8 = (int)(small[i + 0] + 0.5f);
                int g8 = (int)(small[i + 1] + 0.5f);
                int b8 = (int)(small[i + 2] + 0.5f);
                if (r8 > 255) r8 = 255;
                if (g8 > 255) g8 = 255;
                if (b8 > 255) b8 = 255;
                row[x0 + x] = 0xFF000000u | ((u32)r8 << 16) | ((u32)g8 << 8) | (u32)b8;
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            u32 *row = s->px + (size_t)(y0 + y) * s->stride;
            int sy = y / F; if (sy >= sh) sy = sh - 1;
            for (int x = 0; x < w; x++) {
                int sx = x / F; if (sx >= sw) sx = sw - 1;
                int v = (int)(small[(size_t)sy * sw + sx] + 0.5f);
                if (v > 255) v = 255;
                row[x0 + x] = 0xFF000000u | ((u32)v << 16) | ((u32)v << 8) | (u32)v;
            }
        }
    }
}

void gfx_blur_region(Surface *s, int rx, int ry, int rw, int rh, int radius) {
    blur_run(s, rx, ry, rw, rh, radius, 3);
}

/* shadow = a blurred dark rounded rect, drawn underneath */
void gfx_shadow(Surface *s, float x, float y, float w, float h, float radius,
                float spread, float alpha) {
    int pad = (int)(spread * 3);
    int rx = (int)x - pad, ry = (int)y - pad;
    int rw = (int)w + pad * 2, rh = (int)h + pad * 2;
    if (rw <= 0 || rh <= 0 || s->w <= 0 || s->h <= 0) return;

    /* Only the part of that rectangle that can reach a pixel of the
     * destination has to exist. The blur inside the temp reaches at most
     * 3*spread pixels (three box passes on a 4x-downsampled copy), so the
     * temp is clipped to the destination grown by pad plus slack, and the
     * clip is kept on whole F=4 cells so the downsample grid - and every
     * blurred value with it - stays exactly where it was. For a window
     * that is on screen the clip does not bite at all, because the temp
     * only ever overhangs the destination by pad. */
    const int F = 4;
    int margin = pad + 4 * F;
    int tx = rx, ty = ry, tw = rw, th = rh;
    if (tx < -margin) {
        int k = (-margin - tx + F - 1) / F;      /* whole cells only */
        tx += k * F;
    }
    if (ty < -margin) {
        int k = (-margin - ty + F - 1) / F;
        ty += k * F;
    }
    if (tx + tw > s->w + margin) {
        int keep = s->w + margin - tx;
        keep -= keep % F;
        tw = keep;
    }
    if (ty + th > s->h + margin) {
        int keep = s->h + margin - ty;
        keep -= keep % F;
        th = keep;
    }
    if (tw <= 0 || th <= 0) return;              /* never touches the destination */

    size_t need = (size_t)tw * (size_t)th;
    if (need / (size_t)th != (size_t)tw) return;
    if (need > (size_t)-1 / 4) return;
    u32 *mask = scratch_grow((void **)&shadow_mask, &shadow_mask_cap, need * 4);
    if (!mask) return;
    /* gfx_rrect_aa only writes pixels the shape covers, so the rest has to be
     * cleared first: the blur reads it as "no shadow". Only the part that is
     * used is cleared, never the whole capacity. */
    memset(mask, 0, need * 4);

    /* Paint a white rounded rect on the scratch surface: the blur of a white
     * shape is the shadow mask we read back from the red channel. */
    Surface tmp;
    tmp.px = mask; tmp.w = tw; tmp.h = th; tmp.stride = tw;
    gfx_rrect_aa(&tmp, (float)((int)x - tx), (float)((int)y - ty), w, h, radius,
                 0xFFFFFFFFu, 0, 0);
    blur_run(&tmp, 0, 0, tw, th, (int)spread, 1);

    for (int yy = 0; yy < th; yy++) {
        int dy = ty + yy;
        if ((unsigned)dy >= (unsigned)s->h) continue;
        u32 *drow = s->px + (size_t)dy * s->stride;
        u32 *srow = mask + (size_t)yy * tw;
        for (int xx = 0; xx < tw; xx++) {
            int dx = tx + xx;
            if ((unsigned)dx >= (unsigned)s->w) continue;
            int a = GET_R(srow[xx]);
            if (a <= 0) continue;
            u32 d = drow[dx];
            int ia = 255 - (int)(a * alpha);
            drow[dx] = 0xFF000000u
                     | ((u32)((GET_R(d) * ia) / 255) << 16)
                     | ((u32)((GET_G(d) * ia) / 255) << 8)
                     |  (u32)((GET_B(d) * ia) / 255);
        }
    }
}

void gfx_line(Surface *s, float x0, float y0, float x1, float y1, float width, u32 color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    float pts[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    gfx_polygon(s, pts, 4, color);
}

/* scanline fill. 4 sub-scanlines per row give vertical antialiasing,
 * horizontal edges get partial coverage per pixel. */
void gfx_polygon(Surface *s, const float *pts, int n, u32 color) {
    if (n < 3) return;
    float miny = pts[1], maxy = pts[1];
    for (int i = 1; i < n; i++) {
        float y = pts[i * 2 + 1];
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;

    const int SS = 4;
    float a = GET_A(color) / 255.0f;
    int cr = GET_R(color), cg = GET_G(color), cb = GET_B(color);
    if (a <= 0.0f) return;

    for (int y = y0; y < y1; y++) {
        u32 *row = s->px + (size_t)y * s->stride;
        for (int ss = 0; ss < SS; ss++) {
            float sy = (float)y + (ss + 0.5f) / SS;
            float xs[64];
            int cnt = 0;
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                float ay = pts[i * 2 + 1], by = pts[j * 2 + 1];
                if ((sy >= ay && sy < by) || (sy >= by && sy < ay)) {
                    float t = (sy - ay) / (by - ay);
                    if (cnt < 64) xs[cnt++] = pts[i * 2] + t * (pts[j * 2] - pts[i * 2]);
                }
            }
            for (int i = 1; i < cnt; i++) {          /* insertion sort */
                float v = xs[i];
                int k = i - 1;
                while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
                xs[k + 1] = v;
            }
            float weight = a / SS;
            for (int i = 0; i + 1 < cnt; i += 2) {
                int sx0 = (int)floorf(xs[i]), sx1 = (int)ceilf(xs[i + 1]);
                if (sx0 < 0) sx0 = 0;
                if (sx1 > s->w) sx1 = s->w;
                for (int x = sx0; x < sx1; x++) {
                    float l = (float)x > xs[i] ? (float)x : xs[i];
                    float r = (float)(x + 1) < xs[i + 1] ? (float)(x + 1) : xs[i + 1];
                    float cov = (r - l);
                    if (cov <= 0.0f) continue;
                    cov *= weight;
                    if (cov > 1.0f) cov = 1.0f;
                    u32 d = row[x];
                    float inv = 1.0f - cov;
                    row[x] = 0xFF000000u
                           | ((u32)(cr * cov + GET_R(d) * inv) << 16)
                           | ((u32)(cg * cov + GET_G(d) * inv) << 8)
                           |  (u32)(cb * cov + GET_B(d) * inv);
                }
            }
        }
    }
}
