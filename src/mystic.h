/* ============================================================
 *  Mystic OS - mystic.h
 *  Everything the desktop needs, in one place.
 * ============================================================ */
#ifndef MYSTIC_H
#define MYSTIC_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

/* ------------------------------------------------------------
 * Colour: 0xAARRGGBB. The framebuffer wants 0x00RRGGBB, which is
 * the same bytes in the same order on little-endian, so a colour
 * can be memcpy'd straight out.
 * ------------------------------------------------------------ */
#define RGB(r, g, b)      (0xFF000000u | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define WITH_A(c, a)      (((c) & 0x00FFFFFFu) | ((u32)(a) << 24))
#define GET_A(c)          ((u8)(((c) >> 24) & 0xFF))
#define GET_R(c)          ((u8)(((c) >> 16) & 0xFF))
#define GET_G(c)          ((u8)(((c) >> 8) & 0xFF))
#define GET_B(c)          ((u8)((c) & 0xFF))

u32 color_mix(u32 a, u32 b, float t);      /* t=0 -> a, t=1 -> b */
u32 color_alpha(u32 c, float a);           /* scale existing alpha */

/* ------------------------------------------------------------
 * Surface: a rectangle of pixels we draw into.
 * ------------------------------------------------------------ */
typedef struct {
    u32 *px;
    int  w, h;
    int  stride;      /* pixels per row */
} Surface;

static inline void surf_set(Surface *s, int x, int y, u32 c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h)
        s->px[(size_t)y * s->stride + x] = c;
}

void surf_fill(Surface *s, int x, int y, int w, int h, u32 c);
void surf_blend(Surface *s, int x, int y, int w, int h, u32 c);
void surf_copy(Surface *dst, Surface *src);
void surf_push(Surface *dst, int dx, int dy, Surface *src, int sx, int sy, int w, int h);

/* ------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------ */
void gfx_gradient(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_glow(Surface *s, float cx, float cy, float radius, u32 color, float strength);
void gfx_rrect(Surface *s, float x, float y, float w, float h, float radius, u32 color);
void gfx_rrect_aa(Surface *s, float x, float y, float w, float h, float radius,
                  u32 fill, u32 border, float border_w);
void gfx_shadow(Surface *s, float x, float y, float w, float h, float radius,
                float spread, float alpha);
void gfx_blur_region(Surface *s, int x, int y, int w, int h, int radius);
void gfx_line(Surface *s, float x0, float y0, float x1, float y1, float width, u32 color);
void gfx_polygon(Surface *s, const float *pts, int n, u32 color);

/* ------------------------------------------------------------
 * Text
 * ------------------------------------------------------------ */
typedef struct Font Font;

enum { FONT_REGULAR, FONT_MEDIUM, FONT_SEMIBOLD, FONT_COUNT };

int  font_init(const char *dir);                  /* dir holding the .ttf files */
Font *font_get(int face, float size);             /* cached per face+size */
float font_width(Font *f, const char *text);
float font_height(Font *f);
float font_ascent(Font *f);
float font_draw(Surface *s, Font *f, float x, float y, const char *text, u32 color);

/* ------------------------------------------------------------
 * Input (evdev)
 * ------------------------------------------------------------ */
typedef struct {
    int   mouse_x, mouse_y;
    int   mouse_dx, mouse_dy;
    int   buttons;          /* bit0 left, bit1 right, bit2 middle */
    int   clicked, released;
    int   wheel;
    int   key;              /* last key code, 0 if none */
    int   key_pressed;      /* 0/1 */
    char  text[32];         /* printable chars typed this frame */
    int   text_len;
} Input;

/* ------------------------------------------------------------
 * Display (/dev/fb0)
 * ------------------------------------------------------------ */
typedef struct {
    int  fd;
    u32 *mem;          /* the real screen, memory-mapped */
    int  w, h;
    int  stride;       /* pixels per row */
    int  bpp;
} Display;

int  disp_open(Display *d, const char *path);
void disp_present(Display *d, Surface *s);   /* copy a surface to the screen */

/* ------------------------------------------------------------
 * Input (evdev)
 * ------------------------------------------------------------ */
int  input_open(Input *in);
int  input_poll(Input *in);        /* fills Input for this frame; returns 1 if anything happened */
void input_frame(Input *in);       /* clear per-frame fields */

/* ------------------------------------------------------------
 * Theme
 * ------------------------------------------------------------ */
typedef struct {
    char  name[64];
    u32   bg_top, bg_bottom;
    u32   accent;
    u32   glow;
    float glow_strength;
    u32   panel;            /* panel fill (with alpha) */
    u32   panel_border;
    u32   text;
    u32   text_dim;
    u32   window_bg;
    u32   window_border;
    float corner;           /* corner radius */
    float blur;             /* backdrop blur strength 0..1 */
    int   dark;
} Theme;

int theme_load(Theme *t, const char *path);
int theme_load_all(Theme *list, int max, const char *dir);

#endif /* MYSTIC_H */
