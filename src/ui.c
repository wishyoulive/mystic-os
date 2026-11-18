/* ============================================================
 *  Mystic OS - ui.c
 *  One global text scale, read from /etc/mystic/ui.conf.
 *
 *  Everything that has a size derives it from this number: font sizes in
 *  font_get(), and every row/band/icon metric in the desktop. That is why
 *  changing it cannot overlap anything - nothing is hardcoded to 15px.
 *
 *  The file stores a scale RELATIVE to the size the desktop ships at; that
 *  shipped size is UI_BASE below. font_get() and ui_px() must therefore both
 *  multiply by the ABSOLUTE value, ui_scale_get(). ui_px() used to multiply
 *  by the bare relative number, so every box in every app was authored 1.4x
 *  too small while the glyphs inside it were drawn at the full size - a
 *  21.7 px line in a 19 px pill. The band metrics at the bottom of this file
 *  are the belt to that braces: a bar or a tile is now the line it holds plus
 *  one step of air, so it cannot be smaller than its text at any scale.
 * ============================================================ */
#include "mystic.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float ui_scale = 1.0f;      /* RELATIVE: 1.0 = the shipped size */

/* The number in ui.conf is RELATIVE to the size the desktop ships at. That
 * shipped size is the one that turned out to be readable in use, which the
 * old scale called 1.4 - so 1.0 in the file now means what 1.4 used to mean,
 * and a fresh install is never the tiny default. Every row in Settings keeps
 * its label: 1.0x is "Normal", and Normal is readable. */
#define UI_BASE 1.4f

/* The shipped range, as the relative value. ui_scale_load() and
 * ui_scale_set_effective() must both end up inside it, or the window server
 * and an app in its jail would disagree about what "1.6" means. */
#define UI_REL_MIN 0.8f
#define UI_REL_MAX 1.6f

static float clamp_rel(float rel) {
    if (!isfinite(rel)) return 1.0f;
    if (rel < UI_REL_MIN) rel = UI_REL_MIN;
    if (rel > UI_REL_MAX) rel = UI_REL_MAX;
    return rel;
}

float ui_scale_get(void) { return ui_scale * UI_BASE; }

/* ui.conf written before UI_BASE existed stored an ABSOLUTE scale, where
 * 1.4 meant what 1.0 means now. Reading such a file as relative would double
 * the bump (1.4 -> 1.96), so a legacy file is converted once and rewritten
 * with the marker line. Honest and idempotent: the marker is what makes a
 * second boot a no-op. The value written is the CLAMPED relative one, so a
 * legacy 2.24 ("Huge") lands on 1.6 instead of being clamped to 1.6 before
 * the division and losing a whole step. */
static void migrate_legacy(const char *path, float rel) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "baseline = %.1f\nscale = %.1f\n", (double)UI_BASE,
            (double)clamp_rel(rel));
    fclose(f);
}

void ui_scale_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { ui_scale = 1.0f; return; }
    char line[256];
    int marker = 0, got = 0;
    float v = 1.0f;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "baseline", 8)) { marker = 1; continue; }
        if (strncmp(line, "scale", 5)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        float parsed = (float)atof(eq + 1);
        /* NaN passes both comparisons, which would make every metric 1px
         * and every font 5px: reject it explicitly. */
        if (!isfinite(parsed)) continue;
        v = parsed;
        got = 1;
        break;
    }
    fclose(f);
    /* a file with no usable value keeps the size that is already in force */
    if (!got) return;

    float rel = marker ? v : v / UI_BASE;
    if (!marker) migrate_legacy(path, rel);
    ui_scale = clamp_rel(rel);
}

/* a scaled pixel metric, rounded so 1px borders stay 1px.
 * The ABSOLUTE scale, the same one font_get() applies: a box authored as
 * ui_px(40) and the 13 pt line inside it both follow UI_BASE. */
int ui_px(int base) {
    int v = (int)((float)base * ui_scale_get() + 0.5f);
    return v < 1 ? 1 : v;
}

/* An app runs in its own process (and its own jail, with no /etc/mystic),
 * so it cannot read ui.conf. The window server knows the scale and exports
 * the ABSOLUTE number in the environment; dividing it by UI_BASE here is the
 * exact inverse of ui_scale_get(), so ui_px() and font_get() inside the jail
 * produce the same pixels as on the desktop - including the clamps, which is
 * why the division happens before them. */
void ui_scale_set_effective(float v) {
    if (!isfinite(v) || v <= 0.0f) return;
    ui_scale = clamp_rel(v / UI_BASE);
}

/* ------------------------------------------------------------ bands
 *
 * A band is the line it holds plus `pad` units of air above and below, so a
 * bar, a pill or a tile can never be smaller than the text inside it. `pad`
 * is in ui_px() units like every other gap, and the caller's font size is the
 * one that band actually draws with - there is no second copy of the type
 * scale down here.
 *
 * These live in this file (not in the desktop) so a host harness can measure
 * them by linking the real ui.c and font.c: every number below is a function
 * of the scale, not a pixel someone guessed.
 */
int ui_line_h(int face, float size) {
    Font *f = font_get(face, size);
    int h = (int)(font_height(f) + 0.5f);
    return h < 1 ? 1 : h;
}

int ui_band_h(int face, float size, int pad) {
    return ui_line_h(face, size) + 2 * ui_px(pad);
}

/* The three bars of the desktop. The title bar draws a 13 pt semibold-ish
 * line, the top bar the same line, and the dock's hover label 11 pt. */
int ui_title_h(void)  { return ui_band_h(FONT_MEDIUM, 13, 5); }
int ui_topbar_h(void) { return ui_band_h(FONT_MEDIUM, 13, 3); }
int ui_tip_h(void)    { return ui_band_h(FONT_MEDIUM, 11, 4); }

/* A window may not be smaller than one line of its own title plus a content
 * box: src/main.c's resize clamps and its initial sizes both read these, so
 * a window created small is still big enough to draw into. */
int ui_win_min_w(void) { return ui_px(240); }
int ui_win_min_h(void) { return ui_title_h() + ui_px(160); }

/* The dock is icons, not text: these are the shipped design sizes, and the
 * hover pill above them is the text-bearing box (ui_tip_h). */
int ui_dock_icon(void) { return ui_px(40); }
int ui_dock_gap(void)  { return ui_px(16); }
int ui_dock_pad(void)  { return ui_px(12); }

/* ------------------------------------------------------------ launcher
 *
 * The app grid is the one surface whose size depends on the screen as well as
 * on the scale: 18 tiles at 1.6 do not fit in six rows of a 1280x800 screen.
 * Columns are therefore chosen from the available area, one cell size is
 * derived from both directions, and the floor is the label line plus a
 * minimum icon - never a hardcoded pixel height that the text can outgrow.
 */
#define UI_LAUNCHER_GAP      13   /* design px between cells */
#define UI_LAUNCHER_PAD      12   /* design px, panel inset  */
#define UI_LAUNCHER_ICON_MIN 20   /* design px in a squashed cell */
#define UI_LAUNCHER_TILE_W   128  /* design px, the shipped tile */
#define UI_LAUNCHER_TILE_H   118

static int launcher_cell_pad(void) { return ui_px(8); }

/* label line + the smallest icon worth drawing + the cell's own padding */
int ui_launcher_min_h(void) {
    return ui_line_h(FONT_MEDIUM, 12) + ui_px(3) +
           ui_px(UI_LAUNCHER_ICON_MIN) + 2 * launcher_cell_pad();
}

/* the width that lets a label be drawn whole */
int ui_launcher_min_w(int label_w) {
    return label_w + 2 * launcher_cell_pad();
}

/* One place computes the grid; the panel, the per-tile rectangle and the
 * keyboard column count all come from it, so draw and hit-test cannot drift. */
static void launcher_grid(int items, int sw, int sh, int label_w,
                          int *ocols, int *orows, int *ocw, int *och,
                          int *ogx, int *ogy) {
    if (items < 1) items = 1;
    const int gap = ui_px(UI_LAUNCHER_GAP);
    const int pad = ui_px(UI_LAUNCHER_PAD);
    /* the line under the panel, plus the gap above it and the screen's own
     * margin, so the hint is never the thing that falls off the bottom */
    const int hint = ui_line_h(FONT_REGULAR, 10) + ui_px(7) + ui_px(3);
    int avail_w = sw - 2 * pad - 2 * ui_px(8);
    int avail_h = sh - ui_topbar_h() - 2 * pad - hint;
    if (avail_w < ui_px(40)) avail_w = ui_px(40);
    if (avail_h < ui_px(40)) avail_h = ui_px(40);

    int need_w = ui_launcher_min_w(label_w);
    int need_h = ui_launcher_min_h();
    if (need_w < need_h) need_w = need_h;   /* no sliver-thin cells */

    /* the column count that makes the cells closest to square */
    int aspect = (int)(sqrt((double)items * (double)avail_w / (double)avail_h) + 0.5);
    if (aspect < 3) aspect = 3;
    if (aspect > items) aspect = items;

    int best = 0, best_d = 0, bw = 0, bh = 0;
    for (int c = 3; c <= items && c <= 8; c++) {
        int r = (items + c - 1) / c;
        int cw = (avail_w - (c - 1) * gap) / c;
        int ch = (avail_h - (r - 1) * gap) / r;
        if (cw < need_w || ch < need_h) continue;
        int d = c > aspect ? c - aspect : aspect - c;
        if (!best || d < best_d) { best = c; best_d = d; bw = cw; bh = ch; }
    }
    if (!best) {
        /* Not one column count gives every cell its full label: use the
         * aspect grid and let the caller fit the label into the cell it has.
         * Truncating with an ellipsis is the only outcome that never clips a
         * glyph and never lets a tile reach outside the panel. */
        best = aspect;
        if (best > items) best = items;
        if (best < 3) best = 3;
        int r = (items + best - 1) / best;
        bw = (avail_w - (best - 1) * gap) / best;
        bh = (avail_h - (r - 1) * gap) / r;
        if (bw < ui_px(24)) bw = ui_px(24);
        if (bh < ui_px(24)) bh = ui_px(24);
    }
    if (bw > ui_px(UI_LAUNCHER_TILE_W)) bw = ui_px(UI_LAUNCHER_TILE_W);
    if (bh > ui_px(UI_LAUNCHER_TILE_H)) bh = ui_px(UI_LAUNCHER_TILE_H);

    int rows = (items + best - 1) / best;
    int gw = best * bw + (best - 1) * gap;
    int gh = rows * bh + (rows - 1) * gap;
    int gx = (sw - gw) / 2;
    /* centre the grid in the band below the top bar, leaving the panel's own
     * pad on both sides; the panel then starts exactly at the bar when the
     * grid fills the band instead of overlapping it */
    int gy = ui_topbar_h() + pad + (avail_h - gh) / 2;
    /* even when the grid is taller than the band (a screen smaller than the
     * minimum grid needs), the first row starts below the bar: a cell that
     * hangs off the bottom is clipped by the surface, a cell over the bar
     * would be hidden by it */
    if (gy < ui_topbar_h() + pad) gy = ui_topbar_h() + pad;
    *ocols = best; *orows = rows; *ocw = bw; *och = bh; *ogx = gx; *ogy = gy;
}

int ui_launcher_cols(int items, int sw, int sh, int label_w) {
    int c, r, cw, ch, gx, gy;
    launcher_grid(items, sw, sh, label_w, &c, &r, &cw, &ch, &gx, &gy);
    return c;
}

void ui_launcher_geom(int items, int sw, int sh, int label_w, int idx,
                      int *x, int *y, int *w, int *h) {
    int cols, rows, cw, ch, gx, gy;
    launcher_grid(items, sw, sh, label_w, &cols, &rows, &cw, &ch, &gx, &gy);
    if (idx < 0) idx = 0;
    if (idx >= items) idx = items - 1;
    int r = idx / cols, c = idx % cols;
    int gap = ui_px(UI_LAUNCHER_GAP);
    *x = gx + c * (cw + gap);
    *y = gy + r * (ch + gap);
    *w = cw;
    *h = ch;
}

/* The panel that holds the grid: the cell rectangle plus one pad all round. */
void ui_launcher_panel(int items, int sw, int sh, int label_w,
                       int *x, int *y, int *w, int *h) {
    int cols, rows, cw, ch, gx, gy;
    launcher_grid(items, sw, sh, label_w, &cols, &rows, &cw, &ch, &gx, &gy);
    int pad = ui_px(UI_LAUNCHER_PAD);
    int gap = ui_px(UI_LAUNCHER_GAP);
    int gw = cols * cw + (cols - 1) * gap;
    int gh = rows * ch + (rows - 1) * gap;
    *x = gx - pad;
    *y = gy - pad;
    *w = gw + 2 * pad;
    *h = gh + 2 * pad;
    /* never let the panel leave the screen, even if a caller passed a size
     * smaller than the grid: the cells are clipped by the surface anyway */
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > sw) *w = sw - *x;
    if (*y + *h > sh) *h = sh - *y;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
