/* wmgeom.h - window geometry arithmetic, kept out of the compositor.
 *
 * The compositor in main.c owns the window list, the surfaces and the input
 * state; what a resize of a given edge does to a rectangle is pure arithmetic
 * and lives here instead, so it can be tested exhaustively on the host without
 * a framebuffer or an evdev device.
 *
 * The edge bits are the same numbers the input code uses:
 *   1 west, 2 east, 4 north, 8 south - a corner is two bits at once.
 */
#ifndef MYSTIC_WMGEOM_H
#define MYSTIC_WMGEOM_H

typedef struct {
    int x, y, w, h;
} WmRect;

/* Apply a pointer delta to a window rectangle.
 *
 * r0    the rectangle before the drag began (never the live one: a resize that
 *       reads its own output drifts with every frame)
 * edge  the grabbed edges; 0 leaves the rectangle alone
 * dx,dy pointer movement since the press, in screen pixels
 * minw,minh  the smallest size a window may have
 * miny  the top of the usable area: a window may not be resized under the bar
 *
 * The dragged edge follows the pointer. The opposite edge stays where it was,
 * which is why dragging west or north moves the origin rather than only the
 * size. When a west or north drag reaches the minimum size, the origin stops
 * moving too - otherwise the window would keep sliding while its far edge
 * stayed pinned, and the pointer would separate from the edge it grabbed.
 */
static void wm_resize_apply(const WmRect *r0, int edge, int dx, int dy,
                            int minw, int minh, int miny, WmRect *out)
{
    WmRect r = *r0;

    if (edge & 2) r.w = r0->w + dx;                 /* east  */
    if (edge & 1) { r.w = r0->w - dx; r.x = r0->x + dx; }   /* west */
    if (edge & 8) r.h = r0->h + dy;                 /* south */
    if (edge & 4) { r.h = r0->h - dy; r.y = r0->y + dy; }   /* north */

    if (r.w < minw) {
        if (edge & 1) r.x = r0->x + r0->w - minw;
        r.w = minw;
    }
    if (r.h < minh) {
        if (edge & 4) r.y = r0->y + r0->h - minh;
        r.h = minh;
    }
    /* A window dragged past the left edge or above the bar keeps its size and
     * stops at the boundary: clamping the origin alone would change the width
     * the pointer is dragging, which feels like the handle slipping. */
    if (r.x < 0) {
        if (edge & 1) r.w -= -r.x;
        r.x = 0;
        if (r.w < minw) r.w = minw;
    }
    if (r.y < miny) {
        if (edge & 4) r.h -= miny - r.y;
        r.y = miny;
        if (r.h < minh) r.h = minh;
    }

    *out = r;
}

#endif
