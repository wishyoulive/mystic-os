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
 *        4    Black Hole  yes   - traced Schwarzschild lens + disk
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

#endif /* MYSTIC_WALLPAPER_H */
