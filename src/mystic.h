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
/* Same shadow, but the caller paints an opaque shape over the whole rect
 * right after: the covered interior is not blended (the corner ring is). */
void gfx_shadow_solid(Surface *s, float x, float y, float w, float h, float radius,
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
 *
 * Three sizes, and they are not the same thing:
 *   - the DEVICE size (fb_w x fb_h) is what the panel is doing right now;
 *   - the LOGICAL size (w x h) is the Surface the compositor draws into.
 *     It is the device size unless disp_set_logical_size() picked another,
 *     which is how a 1920x1080 panel gets a 1280x800 design;
 *   - modes[] is the list of sizes the device itself reported, native first.
 *     Nothing is ever added to it that /sys or the kernel did not say.
 *
 * disp_present() maps the logical surface onto the device in the chosen
 * DispScale mode. disp_present_rect() keeps the old dirty-rectangle copy
 * (the 1:1 case, a memcpy per row) so existing callers are unchanged.
 *
 * The logical size is also the user's choice, not only a policy: Settings
 * offers a canvas list, and on a device that reports a single mode (every
 * QEMU fbdev) that list is the only display setting that can change what is
 * on screen. It is stored in /etc/mystic/display.conf as `logical=WxH`, or
 * `logical=auto` for "the design size for this panel". The scale mode then
 * decides how the canvas lands on the panel: fit (bars), stretch, integer
 * (whole factor) or 1:1 (centred, cropped).
 * ------------------------------------------------------------ */

/* How a logical frame is mapped onto the framebuffer. */
typedef enum {
    DISP_FIT = 0,      /* whole frame, aspect kept, black bars      */
    DISP_STRETCH,      /* fills the panel, aspect broken            */
    DISP_INTEGER,      /* largest whole factor that fits, centred   */
    DISP_ONE_TO_ONE,   /* centred, cropped when it is larger        */
    DISP_SCALE_COUNT
} DispScale;

#define DISP_MAX_MODES 16

typedef struct {
    int w, h;          /* resolution, exactly as the device reported it */
    int refresh;       /* Hz from the mode string, 0 when it did not say */
    int native;        /* 1 = the framebuffer's current size */
    int from_sysfs;    /* 1 = /sys/class/graphics/fbN/modes named it; the
                        * native entry carries this too when the file's
                        * only line is the size the kernel is in */
} DispMode;

/* What the device's mode file actually said. "It named nothing" and "it
 * named only the size the panel is already in" are different facts: the
 * first means nobody can say what the panel can do, the second means the
 * panel told us it has nothing else. The Display page says which one. */
typedef enum {
    DISP_MODES_NONE = 0,     /* no file, an empty file, or nothing parsed */
    DISP_MODES_NATIVE_ONLY,  /* the file named only the size in effect */
    DISP_MODES_EXTRA         /* the file named at least one other size */
} DispModeSource;

/* Where one src_w x src_h frame lands on a dst_w x dst_h panel.
 * bx/by may be negative (1:1 on a smaller panel crops on both sides). */
typedef struct {
    DispScale mode;
    int bx, by, bw, bh;   /* the image rectangle, in framebuffer pixels */
    int integer;          /* nearest-neighbour factor; 0 = box filter */
    int bars;             /* 1 = black bars on at least one axis */
    int crop;             /* 1 = the frame is larger than its rectangle */
} DispGeom;

typedef struct {
    int  fd;
    u32 *mem;          /* the real screen, memory-mapped */
    /* device size: the panel right now */
    int  fb_w, fb_h;
    int  fb_stride;    /* pixels per row in framebuffer memory */
    int  bpp;
    size_t fb_bytes;   /* size of the mapping, for munmap() after a mode change */
    /* logical size the compositor draws into. These are the fields every
     * caller already uses for layout, and they equal fb_w/fb_h by default. */
    int  w, h;
    int  stride;       /* == fb_stride, kept for the old callers */

    char     path[64];        /* the device we opened, for logging */
    DispMode modes[DISP_MAX_MODES];
    int      nmodes;          /* >= 1 once disp_open() succeeded */
    int      mode_index;      /* which modes[] entry the panel is set to */
    int      modes_from_sysfs;/* how many entries the device named, the
                               * native one included (0 = it named none) */
    int      modes_named;     /* parseable lines in the mode file (any size,
                               * duplicates included; 0 = it said nothing) */
    int      modes_extra;     /* distinct sizes the file named that are not
                               * the native one (0 = native only or silent) */
    DispScale scale;
    DispGeom  geom;           /* placement used by the last present() */
    int       src_w, src_h;   /* size of the surface that was mapped */
    double    last_ms;        /* measured cost of that present() */
} Display;

int  disp_open(Display *d, const char *path);

/* Ask the device what it can do. Reads /sys/class/graphics/fbN/modes and
 * virtual_size for the device disp_open() opened; native size first, then
 * every distinct mode the file names. Returns d->nmodes (always >= 1 while
 * the display is open). The device always supplies the native entry even
 * when it reports nothing else. */
int  disp_scan_modes(Display *d);
/* The same parse, from a directory a caller names - so a test can prove it
 * against real kernel text without /dev/fb0. Returns the number of entries
 * written; entries[0] is the native size when native_w/native_h are > 0. */
int  disp_modes_read(const char *sysdir, DispMode *out, int max,
                     int native_w, int native_h);
int  disp_mode_count(const Display *d);
const DispMode *disp_mode_at(const Display *d, int i);
int  disp_mode_current(const Display *d);
/* 1 when the device's mode file named at least one mode; 0 means it said
 * nothing at all and the only size anyone knows is the framebuffer's
 * current one. Use disp_modes_source() to tell "named nothing" apart from
 * "named only the current size". */
int  disp_modes_reported(const Display *d);
/* The three states, from the same parse as modes[]. */
DispModeSource disp_modes_source(const Display *d);
/* How many parseable lines the file had, and how many distinct sizes other
 * than the native one it named. Both 0 when the file was absent or empty. */
int  disp_modes_named(const Display *d);
int  disp_modes_extra(const Display *d);

/* Ask the panel for modes[i]: FBIOPUT_VSCREENINFO, then re-read the geometry
 * and remap. Only xres/yres are requested; a mode's refresh is what the
 * device reported (DispMode.refresh), not something this call programs.
 * Returns 0 when the driver accepted it (with fb_w/fb_h set to
 * what the driver then reported, which may differ from the request), -1 with
 * errno kept when it refused - the old mapping is left untouched. */
int  disp_set_mode(Display *d, int i);

/* The logical size (the canvas): the surface the compositor draws, before
 * the blit maps it onto the panel. Defaults to the device size and follows
 * it across a mode change until someone picks a size. Takes effect on the
 * next disp_present(); the inverse mapping (disp_map_input_at) follows it
 * immediately, so the first click after a change is not one frame stale. */
void disp_set_logical_size(Display *d, int w, int h);
void disp_logical_size(const Display *d, int *w, int *h);

void      disp_set_scale(Display *d, DispScale m);
DispScale disp_get_scale(const Display *d);
const char *disp_scale_name(DispScale m);
/* Measured cost of the last disp_present() in milliseconds - the whole frame,
 * bar fills included - and 0 before the first one. */
double disp_present_ms(const Display *d);

/* Pure geometry: where a src frame lands on a dst panel. No I/O, so the
 * desktop, the harness and Settings all agree on one definition. */
DispGeom disp_layout(DispScale m, int src_w, int src_h, int dst_w, int dst_h);
/* The inverse: a framebuffer point back to a logical one. Writes clamped
 * coordinates in every case and returns 1 when the point was inside the
 * image, 0 when it fell in a black bar (it then maps to the nearest edge). */
int  disp_map_point(const DispGeom *g, int src_w, int src_h, int *x, int *y);
/* The same for the display's current logical size and scale - the geometry
 * the next disp_present() will use, which is the last one unless the size or
 * scale changed in between. */
int  disp_map_input_at(Display *d, int *x, int *y);
/* One panel is the normal case, so this is the same call against the display
 * disp_open() was last given. */
int  disp_map_input(int *x, int *y);

void disp_present(Display *d, Surface *s);   /* map the frame onto the panel */
void disp_present_rect(Display *d, Surface *s, int x, int y, int w, int h);

/* The blit on Surfaces, so the same code that drives the panel can be proven
 * on the host. Region is in destination pixels; it is clipped to the image
 * rectangle, the surface and its clip. */
void disp_blit(Surface *dst, const Surface *src, const DispGeom *g,
               int x, int y, int w, int h);

/* What a frame costs, measured here and now (clock_gettime). disp_bench()
 * returns the WHOLE present: the bar clear plus the blit, the same two
 * phases disp_present() runs, on scratch surfaces of the given sizes - ms
 * per frame, or -1 when the scratch surfaces could not be allocated.
 *
 * disp_bench_parts() measures the same run and also hands back the two
 * components (blit_ms, bars_ms) from the iteration whose total was best, so
 * neither number is an estimate and they add up: total = blit + bars. Pass
 * NULL for a component you do not want. This is what disp_bench_report()
 * prints, one line per scale mode. */
double disp_bench(int src_w, int src_h, int dst_w, int dst_h, DispScale m, int reps);
double disp_bench_parts(int src_w, int src_h, int dst_w, int dst_h, DispScale m,
                        int reps, double *blit_ms, double *bars_ms);
void   disp_bench_report(int src_w, int src_h, int dst_w, int dst_h);

/* ------------------------------------------------------------
 * Input (evdev)
 * ------------------------------------------------------------ */
int  input_open(Input *in);
int  input_poll(Input *in);        /* fills Input for this frame; returns 1 if anything happened */
void input_frame(Input *in);       /* clear per-frame fields */

/* ------------------------------------------------------------
 * Theme
 * ------------------------------------------------------------ */
/* How an icon is built, not just what colour it is. The desktop and the host
 * harness both switch on these values; the .theme files name them. */
typedef enum {
    ICON_STYLE_SOLID = 0,   /* filled marks, the theme's normal corners */
    ICON_STYLE_OUTLINE,     /* the same silhouette as a 1.5 px stroke */
    ICON_STYLE_DUOTONE,     /* filled marks, the reference geometry */
    ICON_STYLE_ROUNDED,     /* corner radius ~35% of the mark */
    ICON_STYLE_SHARP,       /* 0-1 px corners */
    ICON_STYLE_PIXEL,       /* visible 3 px grid, square ends */
    ICON_STYLE_COUNT
} IconStyle;
/* Style is geometry only. Every style still draws the deliberate highlight
 * part in Theme.icon_accent; a theme that wants no second hue sets both
 * colours to the same colour (Mono does that). */

/* Which extra chrome motion the theme adds on top of the sine breathe. Every
 * value except ANIM_NONE uses theme_icon_pulse_scale(); these select the
 * optional second motion. docs/THEMES.md says what each one does. */
typedef enum {
    ANIM_NONE = 0,          /* still desktop: no breathe, no frame request */
    ANIM_PULSE,             /* the breathe on its own (1-2 px shrink) */
    ANIM_GLOW,              /* soft halo around the active icon, plus breathe */
    ANIM_DRIFT,             /* backdrop light, advanced on the same frames */
    ANIM_ORBIT,             /* a dot circles the active icon, plus breathe */
    ANIM_WAVE,              /* the bars icon steps in sequence, plus breathe */
    ANIM_SPIN,              /* the gear icon's teeth rotate, plus breathe */
    ANIM_COUNT
} AnimKind;

typedef struct {
    char  name[64];
    u32   bg_top, bg_bottom;
    u32   accent;
    u32   icon_accent;      /* the highlight inside every icon (default accent) */
    IconStyle icon_style;   /* geometry of every icon */
    AnimKind  anim;         /* which animation this theme uses */
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
    float anim_speed;       /* animation multiplier 0..3, 0 = animations off */
    float anim_amp;         /* animation amplitude 0..1 */
    float font_scale;       /* per-theme text scale 0.8..1.6, on top of the user's */
    char  font_ui[48];      /* UI family name, empty = keep the installed default */
    char  font_mono[48];    /* monospace family name, empty = keep the default */
    int   dark;
} Theme;

int theme_load(Theme *t, const char *path);
int theme_load_all(Theme *list, int max, const char *dir);

/* Apply a theme's font roles, but only those the user's /etc/mystic/fonts.conf
 * does not set (conf_*_set are 1 when the file names that role). Returns a
 * bitmask: 1 = UI family applied, 2 = mono family applied. A family that is not
 * installed is refused by font.c with a line on stderr and the current face is
 * left in place - never a silent fallback. */
int theme_apply_fonts(const Theme *t, int conf_ui_set, int conf_mono_set);

/* Runtime icon colours and motion, so the desktop and a host test share one
 * definition instead of two formulas that can drift apart.
 *
 * theme_icon_pair() answers with the two colours an icon is drawn in. For an
 * ordinary icon the body is the caller's base colour and the highlight is
 * icon_accent; for a focused or hovered one both come from the theme's two
 * icon colours, the DIMMER as the body and the brighter as the highlight, so
 * the two roles cannot collapse into one colour - not even on a theme whose
 * accent is the brighter of the pair (Porcelain, Aurora, Sandstone, Carbon,
 * Eclipse, Mono, Neon, Rose). A monochrome icon is a theme whose accent and
 * icon_accent are equal; every shipped theme sets them apart.
 * theme_icon_highlight() is the highlight half alone.
 *
 * theme_icon_pulse_scale() is exactly 1.0 when anim is none, anim_speed is 0
 * or anim_amp is 0 (nothing moves and no frame is needed), otherwise a
 * shrink-only multiplier in
 * [1 - 0.035*amp, 1] derived from sin(now_sec * anim_speed * 2*pi*0.8). It
 * never grows an icon, so a pulsing icon cannot leave its tile or hit box. */
#define THEME_PULSE_HZ 0.8f
#define THEME_PULSE_DEPTH 0.035f
typedef struct { u32 body, hi; } IconPair;
float   theme_icon_pulse_scale(const Theme *t, double now_sec);
IconPair theme_icon_pair(const Theme *t, u32 base, int active);
u32     theme_icon_highlight(const Theme *t, int active);

/* font.c's runtime family API. It lives there, not here historically; these
 * declarations let the desktop, the theme loader and the Settings app use one
 * signature instead of each writing its own extern block (docs/FONTS.md). */
int  font_family_count(void);
const char *font_family_name(int index);
int  font_face_count(void);
int  font_role_family(int role, char *out, size_t cap);
int  font_set_ui_family(const char *family);
int  font_set_mono_family(const char *family);
int  font_set_role_family(int role, const char *family);
int  font_save_config(const char *path);
int  font_reload(void);


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
