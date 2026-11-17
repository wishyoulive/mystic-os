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

/* The number in ui.conf is RELATIVE to the size the desktop ships at. That
 * shipped size is the one that turned out to be readable in use, which the
 * old scale called 1.4 - so 1.0 in the file now means what 1.4 used to mean,
 * and a fresh install is never the tiny default. Every row in Settings keeps
 * its label: 1.0x is "Normal", and Normal is readable. */
#define UI_BASE 1.4f

float ui_scale_get(void) { return ui_scale * UI_BASE; }

/* ui.conf written before UI_BASE existed stored an ABSOLUTE scale, where
 * 1.4 meant what 1.0 means now. Reading such a file as relative would double
 * the bump (1.4 -> 1.96), so a legacy file is converted once and rewritten
 * with the marker line. Honest and idempotent: the marker is what makes a
 * second boot a no-op. */
static int migrate_legacy(const char *path, float v, int have_marker) {
    if (have_marker) return 0;
    float rel = v / UI_BASE;
    if (rel < 0.8f) rel = 0.8f;
    if (rel > 1.6f) rel = 1.6f;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "baseline = %.1f\nscale = %.1f\n", (double)UI_BASE, (double)rel);
    fclose(f);
    return 1;
}

void ui_scale_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { ui_scale = 1.0f; return; }
    char line[256];
    int marker = 0, got = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "baseline", 8)) { marker = 1; continue; }
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
        got = 1;
        break;
    }
    fclose(f);
    if (got && migrate_legacy(path, ui_scale, marker)) ui_scale /= UI_BASE;
    if (ui_scale < 0.8f) ui_scale = 0.8f;
    if (ui_scale > 1.6f) ui_scale = 1.6f;
}

/* a scaled pixel metric, rounded so 1px borders stay 1px */
int ui_px(int base) {
    int v = (int)(base * ui_scale + 0.5f);
    return v < 1 ? 1 : v;
}
