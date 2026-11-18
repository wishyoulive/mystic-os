/* ============================================================
 *  Mystic OS - wallpaper.h
 *  The desktop background: eight procedural paintings, or one
 *  picture decoded from a file on disk.
 *
 *  A procedural wallpaper is a pure function of the theme, an
 *  index and (for the animated ones) the time we are handed. That
 *  means the desktop can paint the background once and then sit
 *  still for the static ones, and repaint only the animated ones at
 *  its own frame rate.
 *
 *      index  name        animated
 *        0    Dusk        no    - vertical gradient + 3 soft glows
 *        1    Mesh        no    - four-corner mesh + soft vignette
 *        2    Waves       yes   - layered sine bands drifting sideways
 *        3    Orbits      yes   - rings and discs orbiting the centre
 *        4    Black Hole  yes   - traced Schwarzschild lens + disk,
 *                                Keplerian rotation, drag/wheel view
 *        5    Nebula      no    - warped fbm clouds, core and stars
 *        6    Synthwave   yes   - deep purple sky, banded sun, a
 *                                perspective neon grid whose lines
 *                                scroll toward the viewer
 *        7    Dunes       no    - five noise-driven warm ridges, a low
 *                                sun and a star / dust field
 *
 *  The ninth choice is not an index into that list:
 *
 *      WALLPAPER_IMAGE  "Custom image" - a PNG, JPEG, BMP or PNM read
 *                       from disk once, cover-scaled to the screen
 *                       and cached, then re-blitted.
 *
 *  The custom image is the one wallpaper that does touch the
 *  filesystem, and it is deliberately narrow: the module reads the
 *  picture when the setting changes (and once for a new screen
 *  size), never per frame. The setting itself lives in
 *  /etc/mystic/wallpaper as one line:
 *
 *      image:/path/to/picture.png     a custom image
 *      Waves                          a wallpaper name
 *
 *  wallpaper_from_name() understands both forms, so a caller that
 *  already reads that file (src/main.c does) needs no new code: a
 *  name resolves to its index, "image:" resolves to WALLPAPER_IMAGE,
 *  and wallpaper_draw() then draws the picture instead of a
 *  procedural background. Nothing else in the header changes shape.
 *
 *  COST, measured at 1280x800 with the shipping flags (-Os), best of
 *  30 draws on the host: Dusk 5.1 ms, Mesh 3.5 ms, Waves 3.7 ms,
 *  Orbits 0.5 ms, Black Hole 6.5 ms, Nebula 44 ms, Synthwave 2.9 ms,
 *  Dunes 3.3 ms. A cached custom image costs a full-screen memcpy
 *  (0.34 ms) per frame and no decode at all.
 * ============================================================ */
#ifndef MYSTIC_WALLPAPER_H
#define MYSTIC_WALLPAPER_H

#include "mystic.h"

/* The procedural wallpapers. WALLPAPER_IMAGE is a pseudo-index, not a
 * ninth painting: it names the custom image choice, which is why the
 * Settings list carries WALLPAPER_COUNT + 1 rows. */
#define WALLPAPER_COUNT 8
#define WALLPAPER_IMAGE WALLPAPER_COUNT

/* "Dusk", "Mesh", "Waves", "Orbits", "Black Hole", "Nebula",
 * "Synthwave", "Dunes"; "Custom image" for WALLPAPER_IMAGE; NULL when
 * index is out of range. */
const char *wallpaper_name(int index);

/* A wallpaper index 0..WALLPAPER_COUNT-1, or -1 when the line names no
 * wallpaper. Matching is case-insensitive and ignores surrounding
 * whitespace (spaces, tabs, CR, LF), so a name read straight out of a
 * config file works without trimming.
 *
 * A line starting with "image:" (any case) returns WALLPAPER_IMAGE.
 * Only the prefix is examined - the path is read later, from the
 * setting file itself - so a path longer than the caller's line buffer
 * still resolves to the right choice. */
int wallpaper_from_name(const char *name);

/* 1 when the wallpaper uses `seconds` (so the caller should keep
 * redrawing), 0 when it is static (paint once and idle). Waves, Orbits,
 * the Black Hole and Synthwave read the clock. A custom image answers 1
 * while a picture is active: the desktop's static-surface cache is
 * keyed on the wallpaper index, so a replaced picture at the same index
 * would otherwise be restored from the cache instead of being redrawn.
 * The repaint is a cached blit, never a decode. */
int wallpaper_animated(int index);

/* Paint the whole of `s`. `seconds` only matters for animated
 * wallpapers; pass 0.0 when you have no clock. An out-of-range index
 * is clamped to the first wallpaper, so the screen is never left
 * unpainted. Never draws outside s->w x s->h.
 *
 * When a custom image is active this draws that picture instead of the
 * procedural background named by `index` (the setting is the truth, and
 * the desktop's index catches up with it within a second). */
void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds);

/* ------------------------- Custom image -------------------------
 * wallpaper_set_image() reads `path`, decodes it with src/img.c
 * (PNG, JPEG, BMP, PNM - the same decoder the viewer and browser use)
 * and, on the next draw, cover-scales it to the screen: the aspect
 * ratio is preserved and the overflow is cropped, never stretched.
 * The screen-size copy is cached; a 40-megabyte photo is decoded once,
 * not per frame.
 *
 * Returns 0 on success. On failure it returns -1, changes nothing, and
 * writes the real reason into err: strerror(errno) for a file that
 * cannot be read, or the decoder's own sentence for bytes that are not
 * a picture ("PNG: bad IHDR CRC ..."), preceded by the path.
 *
 * wallpaper_clear_image() drops the picture and its cache; the next
 * draw paints the procedural wallpaper again.
 * wallpaper_image_active() is 1 while a decoded picture is the
 * background - it is false before the first successful set, after a
 * clear, and when a restore failed, so a caller can tell a picture from
 * a fallback.
 * wallpaper_image_path() is the active path ("" when none).
 * wallpaper_image_generation() counts changes: a caller that caches
 * anything keyed on the wallpaper can include it in its key.
 *
 * wallpaper_image_size() fills any non-NULL output with the decoded
 * source size and the cached screen-size copy; it returns 1 when a
 * picture is active. dst_w/dst_h stay 0 until the first draw, because
 * only the draw knows the surface it must fill.
 *
 * An image with an alpha channel is composited over black, so a
 * transparent wallpaper is a black one rather than whatever was in the
 * framebuffer last frame.                                   */
int  wallpaper_set_image(const char *path, char *err, size_t esz);
void wallpaper_clear_image(void);
int  wallpaper_image_active(void);
const char *wallpaper_image_path(void);
int  wallpaper_image_generation(void);
int  wallpaper_image_size(int *src_w, int *src_h, int *dst_w, int *dst_h);

/* ---------------------- Black Hole: view and spin ----------------------
 * The black hole is the only wallpaper with a view. These two knobs are
 * state changes only - they do not repaint anything - so the desktop can
 * call them from its input path and let the next wallpaper_draw() pick the
 * new state up. Calling them while another wallpaper is selected is
 * harmless: the state is remembered and used when index 4 is drawn.
 *
 * DRAG (yaw):
 *     wallpaper_blackhole_view(input.mouse_dy * WALLPAPER_BH_YAW_PER_PX);
 *   Positive dy (pointer moving down) tilts toward edge-on; dragging up
 *   opens the disk toward face-on. One pixel is WALLPAPER_BH_YAW_PER_PX
 *   radians, so a 300 px drag covers the whole range. If your drag axis is
 *   horizontal, pass dx instead - the function only takes the delta.
 *   The view is clamped to [WALLPAPER_BH_YAW_MIN, WALLPAPER_BH_YAW_MAX];
 *   the default is WALLPAPER_BH_YAW_DEF. The disk is really projected at
 *   cos(yaw) on its minor axis, so this is a change of camera, not a
 *   scale of a circle.
 *
 * WHEEL (spin):
 *     wallpaper_blackhole_spin(input.wheel * WALLPAPER_BH_SPIN_PER_NOTCH);
 *   Positive notches advance the disk. The argument is seconds of disk
 *   time, so each radius keeps its own Keplerian speed and the wheel
 *   shears the material rather than rotating it rigidly. Negative delta
 *   runs it backwards. The clock is kept in double precision and is
 *   reduced per radius modulo 2*pi, so there is no wrap point to jump at.
 *
 * wallpaper_blackhole_reset() restores the 82-degree default view and zero
 * spin; wallpaper_blackhole_yaw() returns the current view in radians. */
#define WALLPAPER_BH_YAW_DEF        1.43116999f  /* 82 deg, near edge-on   */
#define WALLPAPER_BH_YAW_MIN        0.17453293f  /* 10 deg, near face-on   */
#define WALLPAPER_BH_YAW_MAX        1.53588974f  /* 88 deg, thin edge-on   */
#define WALLPAPER_BH_YAW_PER_PX     0.0045f      /* rad per pixel dragged  */
#define WALLPAPER_BH_SPIN_PER_NOTCH 0.35f        /* s of disk time / notch */

void wallpaper_blackhole_view(float delta_radians);
void wallpaper_blackhole_spin(float delta_seconds);
void wallpaper_blackhole_reset(void);
float wallpaper_blackhole_yaw(void);

#endif /* MYSTIC_WALLPAPER_H */
