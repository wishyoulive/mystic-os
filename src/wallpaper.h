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
 *                                Keplerian animation, draggable view
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

/* ---- the Black Hole's two knobs ----------------------------------
 *
 * These steer wallpaper 4 ("Black Hole") and nothing else. They may be
 * called while another wallpaper is showing - the state is simply kept
 * for the next time Black Hole is selected, and reset() puts it back to
 * the default view. The calls the desktop should make:
 *
 *   on drag:   wallpaper_blackhole_spin(input.mouse_dx * 0.010f);
 *              wallpaper_blackhole_view(input.mouse_dy * -0.006f);
 *   on wheel:  wallpaper_blackhole_view(input.wheel * 0.05f);
 *
 * `view` adds a yaw offset in radians, measured from the disk axis:
 * 0 is face on, PI/2 is exactly edge on. It is clamped to
 * [WALLPAPER_BH_TILT_MIN, WALLPAPER_BH_TILT_MAX]; the default is 82
 * degrees. The projected disk is an ellipse with aspect cos(yaw), and
 * the lensed arcs and the Doppler term follow the same angle, so the
 * disk really foreshortens rather than being scaled. Dragging up closes
 * the disk towards edge-on, dragging down opens it towards face-on.
 *
 * `spin` adds a phase offset in radians to the disk's own rotation and
 * wraps to one turn, so a horizontal drag pushes the pattern forwards or
 * backwards by hand. The animation keeps running on its own: `seconds`
 * drives the Keplerian rotation of the disk, with the inner rings faster
 * than the outer ones, plus the photon-ring and lensed-arc shimmer.
 *
 * `tilt` returns the current yaw in radians, for a slider or a HUD.
 */
#define WALLPAPER_BH_TILT_MIN  0.14f    /* ~8 deg, nearly face on  */
#define WALLPAPER_BH_TILT_MAX  1.5533f  /* ~89 deg, edge on        */
#define WALLPAPER_BH_TILT_DEF  1.4312f  /* ~82 deg, the default    */

void  wallpaper_blackhole_view(float delta);
void  wallpaper_blackhole_spin(float delta);
void  wallpaper_blackhole_reset(void);
float wallpaper_blackhole_tilt(void);

/* Paint the whole of `s`. `seconds` only matters for animated
 * wallpapers; pass 0.0 when you have no clock. An out-of-range index
 * is clamped to the first wallpaper, so the screen is never left
 * unpainted. Never draws outside s->w x s->h. */
void wallpaper_draw(Surface *s, const Theme *th, int index, double seconds);

#endif /* MYSTIC_WALLPAPER_H */
