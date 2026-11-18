/* ============================================================
 *  Mystic OS - wallpaper.h
 *  The desktop background, as six procedural paintings.
 *
 *  Nothing here touches the filesystem: a wallpaper is a pure
 *  function of the theme, an index and (for the animated ones)
 *  the time we are handed. That means the desktop can paint the
 *  background once and then sit still for the static ones, and
 *  repaint only the animated ones at its own frame rate.
 *
 *      index  name        animated
 *        0    Dusk        no    - vertical gradient + 3 soft glows
 *        1    Mesh        no    - four-corner mesh + soft vignette
 *        2    Waves       yes   - layered sine bands drifting sideways
 *        3    Orbits      yes   - rings and discs orbiting the centre
 *        4    Black Hole  yes   - traced Schwarzschild lens + disk,
 *                                Keplerian rotation, drag/wheel view
 *        5    Nebula      no    - warped fbm clouds, core and stars
 * ============================================================ */
#ifndef MYSTIC_WALLPAPER_H
#define MYSTIC_WALLPAPER_H

#include "mystic.h"

#define WALLPAPER_COUNT 6

/* "Dusk", "Mesh", "Waves", "Orbits", "Black Hole", "Nebula"; NULL when
 * index is out of range. */
const char *wallpaper_name(int index);

/* 0..WALLPAPER_COUNT-1, or -1 when the name is unknown.
 * Matching is case-insensitive and ignores surrounding whitespace
 * (spaces, tabs, CR, LF), so a name read straight out of a config file
 * works without trimming. */
int wallpaper_from_name(const char *name);

/* 1 when the wallpaper uses `seconds` (so the caller should keep
 * redrawing), 0 when it is static (paint once and idle). */
int wallpaper_animated(int index);

/* Paint the whole of `s`. `seconds` only matters for animated
 * wallpapers; pass 0.0 when you have no clock. An out-of-range index
 * is clamped to the first wallpaper, so the screen is never left
 * unpainted. Never draws outside s->w x s->h. */
void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds);

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
 *   runs it backwards. The accumulation wraps at one hour.
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
