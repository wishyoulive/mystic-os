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
    /* Optional clip rectangle. cx1 <= cx0 means "no clip". An app is given
     * its own window rect as the clip, so a bug in its layout cannot paint
     * over the wallpaper or the neighbouring windows. */
    int  cx0, cy0, cx1, cy1;
} Surface;

static inline void surf_clip(Surface *s, int x, int y, int w, int h) {
    s->cx0 = x; s->cy0 = y; s->cx1 = x + w; s->cy1 = y + h;
}
static inline void surf_unclip(Surface *s) { s->cx0 = s->cy0 = s->cx1 = s->cy1 = 0; }

static inline void surf_set(Surface *s, int x, int y, u32 c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) {
        if (s->cx1 > s->cx0 &&
            (x < s->cx0 || x >= s->cx1 || y < s->cy0 || y >= s->cy1)) return;
        s->px[(size_t)y * s->stride + x] = c;
    }
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

enum { FONT_REGULAR, FONT_MEDIUM, FONT_SEMIBOLD, FONT_MONO, FONT_COUNT };

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
    int   clicked2;         /* a second press within 350ms and 6px */
    int   wheel;
    int   key;              /* last key code, 0 if none */
    int   keys[16];         /* every key pressed this frame, in order */
    int   keys_up[16];      /* releases this frame, so apps can see them */
    int   nkeys_up;
    int   nkeys;
    int   shift;            /* shift held on the last key event */
    int   alt;              /* alt held on the last key event */
    int   ctrl;             /* ctrl held on the last key event */
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
void disp_present_rect(Display *d, Surface *s, int x, int y, int w, int h);

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


/* ------------------------------------------------------------
 * Apps
 *
 * An app is a rectangle of pixels plus event handlers. It draws
 * only inside the area it is given, and gets clicks in its own
 * local coordinates. That is the whole contract - which is what
 * lets an app be written without knowing anything about the
 * window manager, the theme or the framebuffer.
 * ------------------------------------------------------------ */
typedef struct App App;

typedef struct {
    const char *id;
    const char *title;
    void (*init)(App *a, int w, int h);
    void (*draw)(App *a, Surface *s, int x, int y, int w, int h, const Theme *th);
    void (*key)(App *a, const char *bytes, int n);
    void (*click)(App *a, int x, int y, int buttons);
    void (*resize)(App *a, int w, int h);
    void (*wheel)(App *a, int notches);   /* positive scrolls up */
    void (*free)(App *a);
} AppVT;

struct App {
    const AppVT *vt;
    void *state;      /* app private data */
};

App *app_new(const char *id, int w, int h);
void app_free(App *a);
void app_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th);
void app_key(App *a, const char *bytes, int n);
void app_wheel(App *a, int notches);
void app_click(App *a, int x, int y, int buttons);
const char *app_title(App *a);

/* each app file provides one of these; undefined ones are simply not built */
#ifdef WITH_FILES
extern const AppVT app_vt_files;
#endif
#ifdef WITH_SETTINGS
extern const AppVT app_vt_settings;
#endif
#ifdef WITH_MONITOR
extern const AppVT app_vt_monitor;
#endif
#ifdef WITH_NOTES
extern const AppVT app_vt_notes;
#endif
#ifdef WITH_STORE
extern const AppVT app_vt_store;
#endif
#ifdef WITH_CALC
extern const AppVT app_vt_calc;
#endif
extern const AppVT app_vt_sysinfo;

/* ------------------------------------------------------------
 * Terminal
 * ------------------------------------------------------------ */
typedef struct Term Term;
Term *term_create(int cols, int rows, const char *shell);
void  term_destroy(Term *t);

/* global text scale (0.8 .. 1.6), from /etc/mystic/ui.conf */
float ui_scale_get(void);
void  ui_scale_load(const char *path);
int   ui_px(int base);            /* a size scaled by ui_scale, min 1 */   /* close the pty, reap the shell, free the grid */
void ui_scale_set_effective(float v);   /* client side: scale from the WM */
int   term_fd(Term *t);
int   term_alive(Term *t);
/* Rebuild the grid and scrollback at a new size, keeping the top-left content,
 * and ioctl(TIOCSWINSZ) the pty so the shell reflows (SIGWINCH). */
void  term_resize(Term *t, int cols, int rows);
/* The same, from the pixel rectangle the terminal is drawn into. */
void  term_resize_px(Term *t, int w, int h);
int   term_pump(Term *t);   /* bytes consumed; capped per call */
void  term_key(Term *t, const char *bytes, int n);
void  term_clear_screen(Term *t);
void  term_draw(Term *t, Surface *s, int x, int y, int w, int h,
                Font *fr, float cw, float ch);
void  term_scroll(Term *t, int lines);   /* positive = back in history */
int   term_scroll_offset(Term *t);

/* ------------------------------------------------------------ */
/* ------------------------------------------------------------
 * SHA-256 and the password file
 * ------------------------------------------------------------ */
void sha256(const void *data, size_t len, u8 out[32]);
void mystic_hash_password(const u8 *salt, int salt_len, const char *password,
                          int rounds, u8 out[32]);
int  mystic_check_password(const char *line, const char *password);
void mystic_make_password_line(const u8 *salt, int salt_len, const char *password,
                               int rounds, char *out, size_t outsz);

#endif /* MYSTIC_H */
