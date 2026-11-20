/* wmgeom.c - host test for the window-resize arithmetic in src/wmgeom.h.
 *
 *   zig cc -Isrc -Wall -Wextra -o wmgeom tools/tests/wmgeom.c && ./wmgeom
 *
 * Covers all eight edges, both minimum-size clamps, the top-bar boundary and
 * the no-op case. The compositor calls the same function the edge cases here
 * call, so a regression in the drag math fails this test before it reaches a
 * screen.
 */
#include <stdio.h>
#include <string.h>
#include "../../src/wmgeom.h"

static int fails;
static void ck(const char *what, WmRect got, WmRect want)
{
    if (memcmp(&got, &want, sizeof got) != 0) {
        printf("FAIL %-44s got %d,%d %dx%d  want %d,%d %dx%d\n", what,
               got.x, got.y, got.w, got.h, want.x, want.y, want.w, want.h);
        fails++;
    } else {
        printf("ok   %-44s %d,%d %dx%d\n", what, got.x, got.y, got.w, got.h);
    }
}

int main(void)
{
    WmRect r0 = { 100, 100, 500, 400 }, g;
    const int MW = 200, MH = 120, MINY = 30;

    wm_resize_apply(&r0, 2, 100, 0, MW, MH, MINY, &g);   ck("east +100",            g, (WmRect){100,100,600,400});
    wm_resize_apply(&r0, 2, -50, 0, MW, MH, MINY, &g);   ck("east -50",             g, (WmRect){100,100,450,400});
    wm_resize_apply(&r0, 1, 100, 0, MW, MH, MINY, &g);   ck("west +100",            g, (WmRect){200,100,400,400});
    wm_resize_apply(&r0, 1, -100, 0, MW, MH, MINY, &g);  ck("west -100",            g, (WmRect){0,100,600,400});
    wm_resize_apply(&r0, 8, 0, 100, MW, MH, MINY, &g);   ck("south +100",           g, (WmRect){100,100,500,500});
    wm_resize_apply(&r0, 4, 0, 100, MW, MH, MINY, &g);   ck("north +100",           g, (WmRect){100,200,500,300});
    wm_resize_apply(&r0, 6, 80, 60, MW, MH, MINY, &g);   ck("north-east corner",    g, (WmRect){100,160,580,340});
    wm_resize_apply(&r0, 9, 80, 60, MW, MH, MINY, &g);   ck("south-west corner",    g, (WmRect){180,100,420,460});
    wm_resize_apply(&r0, 1, 400, 0, MW, MH, MINY, &g);   ck("west past minimum",    g, (WmRect){400,100,200,400});
    wm_resize_apply(&r0, 4, 0, 400, MW, MH, MINY, &g);   ck("north past minimum",   g, (WmRect){100,380,500,120});
    wm_resize_apply(&r0, 1, 600, 0, MW, MH, MINY, &g);   ck("west far past minimum",g, (WmRect){400,100,200,400});
    wm_resize_apply(&r0, 4, 0, 1000, MW, MH, MINY, &g);  ck("north far past min",   g, (WmRect){100,380,500,120});
    wm_resize_apply(&r0, 8, 0, -1000, MW, MH, MINY, &g); ck("south to nothing",     g, (WmRect){100,100,500,120});
    wm_resize_apply(&r0, 2, -1000, 0, MW, MH, MINY, &g); ck("east to nothing",      g, (WmRect){100,100,200,400});
    wm_resize_apply(&r0, 0, 50, 50, MW, MH, MINY, &g);   ck("no edge: unchanged",   g, (WmRect){100,100,500,400});

    WmRect top = { 100, 50, 500, 400 };
    wm_resize_apply(&top, 4, 0, -60, MW, MH, MINY, &g);
    ck("north into the top bar", g, (WmRect){100,30,500,420});

    printf("\n%d checks, %d failures\n", 16, fails);
    return fails ? 1 : 0;
}
