/* ============================================================
 *  Mystic OS - font.c
 *  Real TrueType text: we rasterise actual glyphs with
 *  stb_truetype and blend the coverage ourselves, so text is
 *  antialiased and crisp at any size.
 * ============================================================ */
#include "mystic.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GLYPHS 2048

typedef struct {
    int   cp;
    int   w, h;
    int   xoff, yoff;
    float advance;
    u8   *bitmap;
    int   used;
} Glyph;

struct Font {
    int           face;
    float         size;
    stbtt_fontinfo info;
    u8            *data;
    float          scale;
    float          ascent, descent;
    Glyph          cache[MAX_GLYPHS];
    struct Font   *next;
};

static const char *FONT_FILES[FONT_COUNT] = {
    "Lato-Regular.ttf", "Lato-Bold.ttf", "Lato-Black.ttf"
};

static Font *fonts;
static char  font_dir[512];
static int   font_ready;

int font_init(const char *dir) {
    snprintf(font_dir, sizeof(font_dir), "%s", dir);
    font_ready = 1;
    return 0;
}

float font_height(Font *f) { return f ? f->size * 1.4f : 0.0f; }
float font_ascent(Font *f) { return f ? f->ascent : 0.0f; }

static void glyph_rasterise(Font *f, Glyph *g, int cp) {
    int glyph_index = stbtt_FindGlyphIndex(&f->info, cp);
    float scale = f->scale;

    int advance_width, left_bearing;
    stbtt_GetGlyphHMetrics(&f->info, glyph_index, &advance_width, &left_bearing);
    g->advance = advance_width * scale;

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&f->info, glyph_index, scale, scale, &x0, &y0, &x1, &y1);
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    g->cp = cp;
    g->used = 1;

    if (g->w <= 0 || g->h <= 0) { g->bitmap = NULL; return; }
    if (g->w > 512 || g->h > 512) { g->w = g->h = 0; return; }
    g->bitmap = malloc((size_t)g->w * g->h);
    if (!g->bitmap) { g->w = g->h = 0; return; }
    stbtt_MakeGlyphBitmap(&f->info, g->bitmap, g->w, g->h, g->w, scale, scale, glyph_index);
}

static Font *load_one(int face, float size) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", font_dir, FONT_FILES[face]);

    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "font: cannot open %s\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return NULL; }

    u8 *data = malloc((size_t)len);
    if (!data) { fclose(fp); return NULL; }
    if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    Font *f = calloc(1, sizeof(Font));
    if (!f) { free(data); return NULL; }
    f->size = size;
    f->data = data;

    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, data, offset)) {
        fprintf(stderr, "font: %s is not a usable TrueType file\n", path);
        free(data);
        free(f);
        return NULL;
    }
    f->scale = stbtt_ScaleForPixelHeight(&f->info, size);
    int a, d, lg;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &lg);
    f->ascent = a * f->scale;
    f->descent = d * f->scale;
    return f;
}

Font *font_get(int face, float size) {
    if (!font_ready || face < 0 || face >= FONT_COUNT) return NULL;

    for (Font *f = fonts; f; f = f->next)
        if (f->face == face && fabsf(f->size - size) < 0.01f)
            return f;

    Font *f = load_one(face, size);
    if (!f) return NULL;
    f->face = face;
    f->next = fonts;
    fonts = f;
    return f;
}

static Glyph *glyph_get(Font *f, int cp) {
    int slot = cp % MAX_GLYPHS;
    for (int i = 0; i < MAX_GLYPHS; i++) {
        Glyph *g = &f->cache[(slot + i) % MAX_GLYPHS];
        if (g->used) {
            if (g->cp == cp) return g;
            continue;
        }
        glyph_rasterise(f, g, cp);
        return g;
    }
    return NULL;
}

static int utf8_next(const unsigned char *p, int *len) {
    int c = p[0];
    *len = 1;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && p[1]) { *len = 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *len = 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *len = 4;
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    return c;
}

float font_width(Font *f, const char *text) {
    if (!f || !text) return 0;
    float w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        int len;
        int cp = utf8_next(p, &len);
        Glyph *g = glyph_get(f, cp);
        if (g) w += g->advance;
        p += len;
    }
    return w;
}

float font_draw(Surface *s, Font *f, float x, float y, const char *text, u32 color) {
    if (!f || !text) return x;
    float pen = x;
    float base = y + f->ascent;
    float cr = (float)GET_R(color), cg = (float)GET_G(color), cb = (float)GET_B(color);
    float ca = GET_A(color) / 255.0f;

    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        int len;
        int cp = utf8_next(p, &len);
        Glyph *g = glyph_get(f, cp);
        if (!g) { p += len; continue; }

        if (g->bitmap) {
            int gx = (int)(pen + g->xoff);
            int gy = (int)(base + g->yoff);
            for (int yy = 0; yy < g->h; yy++) {
                int dy = gy + yy;
                if ((unsigned)dy >= (unsigned)s->h) continue;
                u32 *row = s->px + (size_t)dy * s->stride;
                const u8 *src = g->bitmap + (size_t)yy * g->w;
                for (int xx = 0; xx < g->w; xx++) {
                    int dx = gx + xx;
                    if ((unsigned)dx >= (unsigned)s->w) continue;
                    float cov = (src[xx] / 255.0f) * ca;
                    if (cov <= 0.004f) continue;
                    u32 d = row[dx];
                    float inv = 1.0f - cov;
                    row[dx] = 0xFF000000u
                            | ((u32)(cr * cov + GET_R(d) * inv) << 16)
                            | ((u32)(cg * cov + GET_G(d) * inv) << 8)
                            |  (u32)(cb * cov + GET_B(d) * inv);
                }
            }
        }
        pen += g->advance;
        p += len;
    }
    return pen;
}
