/* ============================================================
 *  Mystic OS - font.c
 *  Real TrueType text: we rasterise actual glyphs with
 *  stb_truetype and blend the coverage ourselves, so text is
 *  antialiased and crisp at any size.
 * ============================================================ */
#include "mystic.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GLYPHS 2048

/* 40 bytes exactly: the pointer first so the ints after it pack without
 * padding. 2048 of these are walked on every cache probe, so the layout is
 * worth this much. */
typedef struct {
    u8   *bitmap;
    int   cp;
    int   w, h;
    int   xoff, yoff;
    float advance;
    int   used;
    unsigned last_use;   /* Font.use_clock value of the last hit: the LRU key */
} Glyph;

struct Font {
    int           face;
    float         size;
    stbtt_fontinfo info;
    u8            *data;
    float          scale;
    float          ascent, descent;
    Glyph          cache[MAX_GLYPHS];
    unsigned       use_clock;   /* monotonic per face: bumped on every access */
    struct Font   *next;
};

static const char *FONT_FILES[FONT_COUNT] = {
    "Lato-Regular.ttf", "Lato-Bold.ttf", "Lato-Black.ttf",
    "JetBrainsMono-Regular.ttf"
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

    /* one place applies the global text scale: every caller asks for a
     * nominal size and gets the scaled one back, so no layout can forget. */
    size *= ui_scale_get();
    size = (float)((int)(size * 2.0f + 0.5f)) / 2.0f;
    if (size < 5.0f) size = 5.0f;
    if (size > 96.0f) size = 96.0f;

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

/* One monotonic counter per face, bumped on every cache access. It only ever
 * orders glyphs against each other, so on the 4-billionth access - where an
 * unsigned would wrap and the ordering would collapse - halving every stored
 * stamp keeps the relative order and buys another 4 billion. */
static unsigned next_use(Font *f) {
    if (f->use_clock >= UINT_MAX - 1) {
        for (int i = 0; i < MAX_GLYPHS; i++) f->cache[i].last_use >>= 1;
        f->use_clock = UINT_MAX / 2;
    }
    return ++f->use_clock;
}

static Glyph *glyph_get(Font *f, int cp) {
    const unsigned start = (unsigned)cp % MAX_GLYPHS;
    Glyph *empty = NULL;

    /* The table is open-addressed, but eviction can free a slot in the middle
     * of another codepoint's probe run, so a lookup must never stop at the
     * first empty slot: it walks the table and compares codepoints, which is
     * always correct wherever the entry ended up. A hit stops at the match,
     * so the common path stays short; only a miss pays for the full walk. */
    for (unsigned i = 0; i < MAX_GLYPHS; i++) {
        Glyph *g = &f->cache[(start + i) % MAX_GLYPHS];
        if (!g->used) {
            if (!empty) empty = g;
            continue;
        }
        if (g->cp == cp) { g->last_use = next_use(f); return g; }
    }

    Glyph *slot = empty;
    if (!slot) {
        /* Full: evict the least recently used glyph and hand its slot to the
         * new one. Without this the 2048 entries were permanent, so every
         * codepoint first seen after the table filled stayed invisible. */
        slot = &f->cache[0];
        for (int i = 1; i < MAX_GLYPHS; i++)
            if (f->cache[i].last_use < slot->last_use) slot = &f->cache[i];
        free(slot->bitmap);      /* the evicted bitmap must not leak */
        slot->bitmap = NULL;
        slot->used = 0;
    }

    glyph_rasterise(f, slot, cp);
    slot->last_use = next_use(f);
    return slot;
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
