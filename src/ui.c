/* ============================================================
 *  Mystic OS - ui.c
 *  One global text scale, read from /etc/mystic/ui.conf.
 *
 *  Everything that has a size derives it from this number: font sizes in
 *  font_get(), and every row/band/icon metric in the desktop. That is why
 *  changing it cannot overlap anything - nothing is hardcoded to 15px.
 * ============================================================ */
#include "mystic.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float ui_scale = 1.0f;

float ui_scale_get(void) { return ui_scale; }

void ui_scale_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { ui_scale = 1.0f; return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "scale", 5)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        float v = (float)atof(eq + 1);
        /* NaN passes both comparisons, which would make every metric 1px
         * and every font 5px: reject it explicitly. */
        if (!isfinite(v)) continue;
        if (v < 0.8f) v = 0.8f;
        if (v > 1.6f) v = 1.6f;
        ui_scale = v;
        break;
    }
    fclose(f);
}

/* a scaled pixel metric, rounded so 1px borders stay 1px */
int ui_px(int base) {
    int v = (int)(base * ui_scale + 0.5f);
    return v < 1 ? 1 : v;
}
