/* ============================================================
 *  Mystic OS - fb.c
 *  The screen, as the kernel offers it: /dev/fb0.
 *
 *  We map the framebuffer once and copy our finished frame into it.
 *  fb0 is what exist on old machines (vesafb/efifb/simpledrm) and is
 *  also what QEMU's bochs-drm provides, so one code path covers both.
 *
 *  On top of that copy there is now a display layer, because a panel
 *  is not a design size. Three numbers matter and they are kept
 *  apart:
 *
 *    fb_w x fb_h    the device, right now (var_screeninfo, and the
 *                   mode the driver actually ended up in)
 *    w x h          the logical size the compositor draws into. Native
 *                   by default; disp_set_logical_size() picks another,
 *                   so a 1920x1080 panel can still show the 1280x800
 *                   design at 1.35x instead of leaving it in a corner.
 *                   It is also a user choice: Settings' canvas rows (and
 *                   `logical=` in display.conf) set it, which is the one
 *                   display setting that changes the picture on a panel
 *                   that reports a single mode - every QEMU fbdev.
 *    modes[]        every size the device reported in
 *                   /sys/class/graphics/fbN/modes, native first. If that
 *                   file is empty we keep the native size and nothing
 *                   else - a mode is never invented here.
 *
 *  disp_present() maps the logical surface onto the panel: fit (aspect
 *  kept, black bars), stretch (fills, aspect broken), integer (whole
 *  factor, nearest neighbour) or 1:1 (centred, cropped when larger).
 *  integer and 1:1 are nearest neighbour - they are exact, and a pixel
 *  must stay a pixel. fit/stretch average the source pixels a
 *  destination pixel covers (a box filter), which is the one honest
 *  thing to do with a fractional scale.
 *
 *  disp_present_rect() keeps the old behaviour exactly when nothing is
 *  scaled (a memcpy per row, which is what the cursor and the window
 *  server use); under a scale mode it maps the same logical rectangle
 *  through the blit, so a partial update still lands in the right place.
 * ============================================================ */
#include "mystic.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Tell the kernel to stop drawing its text console on this VT.
 * Without this the console keeps painting a cursor block over our pixels. */
static void claim_console(void) {
    int fd = open("/dev/tty0", O_RDWR);
    if (fd < 0) return;
    ioctl(fd, KDSETMODE, KD_GRAPHICS);
    close(fd);
}

/* C division truncates towards zero; centring a cropped frame needs a floor
 * so an odd difference lands one pixel left/up, not one pixel right/down. */
static int div_floor(int a, int b) {
    int q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}

static double ts_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------
 *  The modes the device reports
 *
 *  fbmon writes these lines into /sys/class/graphics/fbN/modes:
 *
 *      U:1920x1080p-60
 *      D:1280x720p-50
 *
 *  U/D is the "user"/"driver default" flag and is irrelevant to us; the
 *  rest is width x height, i(nterlaced) or p(rogressive), then -refresh.
 *  A driver may also write a bare 1920x1080. Anything that does not parse
 *  is skipped, never guessed at.
 * ------------------------------------------------------------ */
static int parse_mode_line(const char *line, DispMode *m) {
    const char *p = line;
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    if (!(*p >= '0' && *p <= '9')) return 0;
    /* Accumulate in long and stop on absurd values: a sysfs file is text we
     * did not write, and an int that wraps around is not a resolution. */
    long w = 0, h = 0, refresh = 0;
    while (*p >= '0' && *p <= '9') {
        w = w * 10 + (*p - '0');
        if (w > 100000) return 0;
        p++;
    }
    if (*p != 'x' && *p != 'X') return 0;
    p++;
    if (!(*p >= '0' && *p <= '9')) return 0;
    while (*p >= '0' && *p <= '9') {
        h = h * 10 + (*p - '0');
        if (h > 100000) return 0;
        p++;
    }
    if (*p == 'i' || *p == 'I' || *p == 'p' || *p == 'P') p++;
    if (*p == '-') {
        p++;
        while (*p >= '0' && *p <= '9') {
            refresh = refresh * 10 + (*p - '0');
            if (refresh > 100000) return 0;
            p++;
        }
    }
    if (w < 1 || h < 1 || w > 16384 || h > 16384) return 0;
    m->w = (int)w;
    m->h = (int)h;
    m->refresh = (int)refresh;
    m->native = 0;
    m->from_sysfs = 1;
    return 1;
}

static int read_text(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

int disp_modes_read(const char *sysdir, DispMode *out, int max,
                    int native_w, int native_h);
/* The same parse with two extra facts: how many parseable lines the file had
 * (duplicates included) and how many distinct sizes it named that are not the
 * native one. Either out pointer may be NULL. */
static int disp_modes_read_ex(const char *sysdir, DispMode *out, int max,
                              int native_w, int native_h,
                              int *named_out, int *extra_out);

int disp_modes_read(const char *sysdir, DispMode *out, int max,
                    int native_w, int native_h) {
    return disp_modes_read_ex(sysdir, out, max, native_w, native_h, NULL, NULL);
}

static int disp_modes_read_ex(const char *sysdir, DispMode *out, int max,
                              int native_w, int native_h,
                              int *named_out, int *extra_out) {
    if (named_out) *named_out = 0;
    if (extra_out) *extra_out = 0;
    if (!out || max < 1) return 0;
    int n = 0;
    int named = 0, extra = 0;

    /* The native size always comes first: it is the one mode we can prove
     * the device is in, because the kernel is in it. */
    if (native_w > 0 && native_h > 0) {
        out[n].w = native_w;
        out[n].h = native_h;
        out[n].refresh = 0;
        out[n].native = 1;
        out[n].from_sysfs = 0;
        n++;
    }

    /* Nothing below names a mode, so a caller that got no file back must be
     * able to see "named nothing" (0) rather than inherit the native count. */
    if (!sysdir || !*sysdir) {
        if (named_out) *named_out = 0;
        if (extra_out) *extra_out = 0;
        return n;
    }

    char path[192];
    char text[2048];

    /* virtual_size is "xres,yres" - the framebuffer's current virtual
     * resolution. It is the same size as the display on every driver we
     * care about, but a driver with panning can make it taller, so it is
     * a cross-check for the native entry, not a mode of its own. */
    snprintf(path, sizeof(path), "%s/virtual_size", sysdir);
    if (read_text(path, text, sizeof(text)) >= 0) {
        int vw = 0, vh = 0;
        if (sscanf(text, "%d,%d", &vw, &vh) == 2 && vw > 0 && vh > 0 && n > 0) {
            if (!(vw == out[0].w && vh == out[0].h))
                fprintf(stderr, "display: virtual_size %dx%d differs from "
                                "the visible %dx%d\n", vw, vh, out[0].w, out[0].h);
        }
    }

    snprintf(path, sizeof(path), "%s/modes", sysdir);
    if (read_text(path, text, sizeof(text)) <= 0) {
        if (named_out) *named_out = 0;
        if (extra_out) *extra_out = 0;
        return n;
    }

    char *line = text;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        DispMode m;
        memset(&m, 0, sizeof(m));
        if (parse_mode_line(line, &m)) {
            named++;          /* a line the device wrote, whatever it says */
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (out[i].w == m.w && out[i].h == m.h) {
                    /* The file naming the native size is still the device
                     * speaking. Without this, a panel whose only reported
                     * mode is the one the kernel is already in would be
                     * described as "reported no modes" - which is false.
                     * Its refresh is device text too, so keep it. */
                    if (out[i].native) {
                        out[i].from_sysfs = 1;
                        if (m.refresh > 0) out[i].refresh = m.refresh;
                    }
                    dup = 1;
                    break;
                }
            if (!dup) {
                if (!m.native && !(native_w > 0 && native_h > 0 &&
                                   m.w == native_w && m.h == native_h))
                    extra++;  /* a size other than the one in effect */
                if (n < max) out[n++] = m;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (named_out) *named_out = named;
    if (extra_out) *extra_out = extra;
    return n;
}

/* Where sysfs is mounted. A chroot, an initramfs and the host harness all
 * need to point this somewhere else, and hard-coding /sys would make the mode
 * list untestable (and wrong in a chroot). */
static const char *sysfs_root(void) {
    const char *r = getenv("MYSTIC_SYSFS");
    return r ? r : "";
}

int disp_scan_modes(Display *d) {
    if (!d) return 0;
    int native_w = d->fb_w, native_h = d->fb_h;
    int from_sysfs = 0, named = 0, extra = 0;

    d->nmodes = 0;
    d->modes_from_sysfs = 0;
    d->modes_named = 0;
    d->modes_extra = 0;

    /* /dev/fb0 -> fb0 -> /sys/class/graphics/fb0. A device node with any
     * other name has no sysfs entry we can honestly claim, so it keeps the
     * native size alone. */
    const char *base = strrchr(d->path, '/');
    base = base ? base + 1 : d->path;
    if (strncmp(base, "fb", 2) == 0 && base[2]) {
        char dir[192];
        snprintf(dir, sizeof(dir), "%s/sys/class/graphics/%s", sysfs_root(), base);
        d->nmodes = disp_modes_read_ex(dir, d->modes, DISP_MAX_MODES,
                                       native_w, native_h, &named, &extra);
        for (int i = 0; i < d->nmodes; i++) if (d->modes[i].from_sysfs) from_sysfs++;
    }
    if (d->nmodes == 0 && native_w > 0 && native_h > 0) {
        d->nmodes = disp_modes_read_ex(NULL, d->modes, DISP_MAX_MODES,
                                       native_w, native_h, &named, &extra);
        from_sysfs = 0;
        named = extra = 0;
    }

    d->modes_from_sysfs = from_sysfs;
    d->modes_named = named;
    d->modes_extra = extra;
    d->mode_index = 0;
    for (int i = 0; i < d->nmodes; i++)
        if (d->modes[i].native) { d->mode_index = i; break; }
    return d->nmodes;
}

int disp_mode_count(const Display *d) { return d ? d->nmodes : 0; }

const DispMode *disp_mode_at(const Display *d, int i) {
    if (!d || i < 0 || i >= d->nmodes) return NULL;
    return &d->modes[i];
}

int disp_mode_current(const Display *d) { return d ? d->mode_index : -1; }

int disp_modes_reported(const Display *d) {
    return d && d->modes_from_sysfs > 0;
}

DispModeSource disp_modes_source(const Display *d) {
    if (!d || d->modes_named <= 0) return DISP_MODES_NONE;
    return d->modes_extra > 0 ? DISP_MODES_EXTRA : DISP_MODES_NATIVE_ONLY;
}

int disp_modes_named(const Display *d) { return d ? d->modes_named : 0; }
int disp_modes_extra(const Display *d) { return d ? d->modes_extra : 0; }

/* ------------------------------------------------------------
 *  Geometry. No I/O: one definition of every scale mode, used by
 *  present(), the cursor's inverse mapping and the host harness.
 * ------------------------------------------------------------ */
DispGeom disp_layout(DispScale m, int src_w, int src_h, int dst_w, int dst_h) {
    DispGeom g;
    memset(&g, 0, sizeof(g));
    g.mode = m;
    g.bw = dst_w > 0 ? dst_w : 1;
    g.bh = dst_h > 0 ? dst_h : 1;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return g;

    switch (m) {
    case DISP_STRETCH:
        g.bw = dst_w;
        g.bh = dst_h;
        g.integer = 0;
        break;
    case DISP_INTEGER: {
        int kx = dst_w / src_w;
        int ky = dst_h / src_h;
        int k = kx < ky ? kx : ky;
        if (k < 1) k = 1;              /* larger than the panel: cropped */
        g.integer = k;
        g.bw = src_w * k;
        g.bh = src_h * k;
        break;
    }
    case DISP_ONE_TO_ONE:
        g.integer = 1;
        g.bw = src_w;
        g.bh = src_h;
        break;
    case DISP_FIT:
    default: {
        double sx = (double)dst_w / (double)src_w;
        double sy = (double)dst_h / (double)src_h;
        double s = sx < sy ? sx : sy;
        g.bw = (int)((double)src_w * s + 0.5);
        g.bh = (int)((double)src_h * s + 0.5);
        if (g.bw < 1) g.bw = 1;
        if (g.bh < 1) g.bh = 1;
        if (g.bw > dst_w) g.bw = dst_w;
        if (g.bh > dst_h) g.bh = dst_h;
        g.integer = 0;
        break;
    }
    }

    g.bx = div_floor(dst_w - g.bw, 2);
    g.by = div_floor(dst_h - g.bh, 2);
    g.bars = (g.bw < dst_w || g.bh < dst_h);
    g.crop = (g.bw > dst_w || g.bh > dst_h);
    return g;
}

int disp_map_point(const DispGeom *g, int src_w, int src_h, int *x, int *y) {
    if (!g || !x || !y || src_w <= 0 || src_h <= 0) return 0;
    int px = *x - g->bx;
    int py = *y - g->by;
    int inside = (px >= 0 && px < g->bw && py >= 0 && py < g->bh);
    int sx, sy;
    if (g->integer > 0) {
        int k = g->integer;
        sx = div_floor(px, k);
        sy = div_floor(py, k);
    } else {
        if (g->bw <= 0 || g->bh <= 0) { *x = 0; *y = 0; return 0; }
        sx = (int)(((long long)px * src_w) / g->bw);
        sy = (int)(((long long)py * src_h) / g->bh);
    }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx > src_w - 1) sx = src_w - 1;
    if (sy > src_h - 1) sy = src_h - 1;
    *x = sx;
    *y = sy;
    return inside;
}

int disp_map_input_at(Display *d, int *x, int *y) {
    if (!d || !x || !y) return 0;
    /* Geometry only: the inverse is a property of the sizes and the scale,
     * not of whether a mapping happened to succeed. A dead screen must not
     * turn the pointer into raw framebuffer coordinates. */
    int sw = d->src_w > 0 ? d->src_w : d->w;
    int sh = d->src_h > 0 ? d->src_h : d->h;
    if (sw <= 0 || sh <= 0 || d->fb_w <= 0 || d->fb_h <= 0) return 0;
    DispGeom g = disp_layout(d->scale, sw, sh, d->fb_w, d->fb_h);
    return disp_map_point(&g, sw, sh, x, y);
}

/* The one-panel shortcut: disp_open() remembers the display it mapped, so the
 * desktop can invert "framebuffer pointer -> logical point" without reaching
 * for the struct. */
static Display *g_disp;

int disp_map_input(int *x, int *y) {
    return disp_map_input_at(g_disp, x, y);
}

/* ------------------------------------------------------------
 *  The blit
 * ------------------------------------------------------------ */
static int clip_region(const Surface *dst, const DispGeom *g,
                       int x, int y, int w, int h,
                       int *x0, int *y0, int *x1, int *y1) {
    if (w <= 0 || h <= 0) return 0;
    int ax = x, ay = y;
    int bx = x + w, by = y + h;
    int rx = g->bx + g->bw, ry = g->by + g->bh;
    if (ax < g->bx) ax = g->bx;
    if (ay < g->by) ay = g->by;
    if (bx > rx) bx = rx;
    if (by > ry) by = ry;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx > dst->w) bx = dst->w;
    if (by > dst->h) by = dst->h;
    if (dst->cx1 > dst->cx0) {
        if (ax < dst->cx0) ax = dst->cx0;
        if (ay < dst->cy0) ay = dst->cy0;
        if (bx > dst->cx1) bx = dst->cx1;
        if (by > dst->cy1) by = dst->cy1;
    }
    if (bx <= ax || by <= ay) return 0;
    *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by;
    return 1;
}

/* Copy a row of pixels 32 bytes at a time. A constant-size memcpy is a
 * single unaligned vector move, so this does not depend on the libc's
 * memcpy being vectorised - msvcrt's is not, and a per-row memcpy of 7.6 KB
 * measured slower than a pixel loop because of it. */
static void copy_row(u32 *dst, const u32 *src, int n) {
    while (n >= 8) {
        memcpy(dst, src, 32);
        dst += 8;
        src += 8;
        n -= 8;
    }
    while (n-- > 0) *dst++ = *src++;
}

/* Nearest neighbour: one source pixel becomes a k x k block, untouched. A row
 * of k copies is a run, and k == 1 is a row copy, which is what the 1:1 path
 * has always done. */
static void blit_nearest(Surface *dst, const Surface *src, const DispGeom *g,
                         int x0, int y0, int x1, int y1) {
    int k = g->integer > 0 ? g->integer : 1;
    for (int Y = y0; Y < y1; Y++) {
        int sy = div_floor(Y - g->by, k);
        if (sy < 0) sy = 0;
        if (sy >= src->h) sy = src->h - 1;
        const u32 *srow = src->px + (size_t)sy * (size_t)src->stride;
        u32 *drow = dst->px + (size_t)Y * (size_t)dst->stride;
        if (k == 1) {
            int sx = x0 - g->bx;
            if (sx < 0) sx = 0;
            int n = x1 - x0;
            if (sx + n > src->w) n = src->w - sx;
            if (n > 0) copy_row(drow + x0, srow + sx, n);
            continue;
        }
        int X = x0;
        while (X < x1) {
            int sx = div_floor(X - g->bx, k);
            if (sx < 0) sx = 0;
            if (sx >= src->w) break;
            int run_end = g->bx + (sx + 1) * k;
            if (run_end > x1) run_end = x1;
            if (run_end <= X) run_end = X + 1;
            u32 c = srow[sx];
            for (; X < run_end; X++) drow[X] = c;
        }
    }
}

/* One axis of the box filter: destination index t owns source cells
 * [a, b), or the single cell under its centre when that range is empty. */
static void box_range(int t, int s, int d, int *a, int *b) {
    int lo = (int)(((long long)t * s) / d);
    int hi = (int)(((long long)(t + 1) * s) / d);
    if (hi <= lo) {
        lo = (int)(((long long)t * s + s / 2) / d);
        hi = lo + 1;
    }
    if (lo < 0) lo = 0;
    if (hi > s) hi = s;
    *a = lo;
    *b = hi;
}

static u32 cell_avg(const Surface *src, int xa, int xb, int ya, int yb) {
    u64 r = 0, g = 0, b = 0, a = 0;
    for (int sy = ya; sy < yb; sy++) {
        const u32 *p = src->px + (size_t)sy * (size_t)src->stride;
        for (int sx = xa; sx < xb; sx++) {
            u32 c = p[sx];
            r += c & 0xFF0000u;
            g += c & 0x00FF00u;
            b += c & 0x0000FFu;
            a += c >> 24;
        }
    }
    u32 n = (u32)((xb - xa) * (yb - ya));
    if (!n) return 0;
    return (u32)((a / n) << 24) | ((u32)(r / n) & 0xFF0000u) |
           ((u32)(g / n) & 0x00FF00u) | ((u32)(b / n) & 0x0000FFu);
}

/* Box filter. The destination columns tile the source exactly: column t owns
 * [floor(t*sw/bw), floor((t+1)*sw/bw)) - every source pixel lands in exactly
 * one destination pixel, nothing is counted twice. When there is no room for
 * a whole source pixel under the destination pixel (upscaling) it takes the
 * one under its centre; there is nothing to average.
 *
 * The ranges are worked out once per blit (with 64-bit division), not once
 * per pixel: at 1920x1080 the naive version spent 27 ms a frame in divides. */
static void blit_box(Surface *dst, const Surface *src, const DispGeom *g,
                     int x0, int y0, int x1, int y1) {
    const int sw = src->w, sh = src->h;
    const int bw = g->bw, bh = g->bh;
    int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return;

    int *mem = malloc(sizeof(int) * (size_t)(2 * rw + 2 * rh));
    if (!mem) {
        /* No scratch: still draw, just with the division per pixel. A black
         * screen would be a worse answer than a slow one. */
        for (int Y = y0; Y < y1; Y++) {
            int ya, yb;
            box_range(Y - g->by, sh, bh, &ya, &yb);
            u32 *drow = dst->px + (size_t)Y * (size_t)dst->stride;
            for (int X = x0; X < x1; X++) {
                int xa, xb;
                box_range(X - g->bx, sw, bw, &xa, &xb);
                if (xb > xa && yb > ya) drow[X] = cell_avg(src, xa, xb, ya, yb);
            }
        }
        return;
    }
    int *cx0 = mem;
    int *cx1 = mem + rw;
    int *cy0 = mem + 2 * rw;
    int *cy1 = mem + 2 * rw + rh;
    int all_one = 1;   /* every destination column takes one source column */

    for (int i = 0; i < rw; i++) {
        box_range(x0 + i - g->bx, sw, bw, &cx0[i], &cx1[i]);
        if (cx1[i] - cx0[i] != 1) all_one = 0;
    }
    for (int j = 0; j < rh; j++)
        box_range(y0 + j - g->by, sh, bh, &cy0[j], &cy1[j]);

    for (int j = 0; j < rh; j++) {
        u32 *drow = dst->px + (size_t)(y0 + j) * (size_t)dst->stride + x0;
        int ya = cy0[j], yb = cy1[j];
        const u32 *srow = src->px + (size_t)ya * (size_t)src->stride;
        int ny = yb - ya;
        if (ny == 1 && all_one) {
            /* Upscaling by a fraction: no averaging anywhere in this row. */
            for (int i = 0; i < rw; i++) drow[i] = srow[cx0[i]];
            continue;
        }
        for (int i = 0; i < rw; i++) {
            int xa = cx0[i], xb = cx1[i];
            int nx = xb - xa;
            if (nx <= 0 || ny <= 0) continue;
            if (ny == 1 && nx == 1) { drow[i] = srow[xa]; continue; }
            /* Shrinking: average the cells this pixel owns. Masks instead of
             * per-channel shifts, 64-bit sums so nothing overflows. */
            u64 r = 0, g2 = 0, b2 = 0, a2 = 0;
            if (ny == 1) {
                for (int sx = xa; sx < xb; sx++) {
                    u32 c = srow[sx];
                    r += c & 0xFF0000u; g2 += c & 0x00FF00u;
                    b2 += c & 0x0000FFu; a2 += c >> 24;
                }
            } else {
                for (int sy = ya; sy < yb; sy++) {
                    const u32 *p = src->px + (size_t)sy * (size_t)src->stride;
                    for (int sx = xa; sx < xb; sx++) {
                        u32 c = p[sx];
                        r += c & 0xFF0000u; g2 += c & 0x00FF00u;
                        b2 += c & 0x0000FFu; a2 += c >> 24;
                    }
                }
            }
            u32 n = (u32)(nx * ny);
            drow[i] = (u32)((a2 / n) << 24) | ((u32)(r / n) & 0xFF0000u) |
                      ((u32)(g2 / n) & 0x00FF00u) | ((u32)(b2 / n) & 0x0000FFu);
        }
    }
    free(mem);
}

void disp_blit(Surface *dst, const Surface *src, const DispGeom *g,
               int x, int y, int w, int h) {
    if (!dst || !src || !g || !dst->px || !src->px) return;
    if (src->w <= 0 || src->h <= 0 || dst->w <= 0 || dst->h <= 0) return;
    if (g->bw <= 0 || g->bh <= 0) return;
    int x0, y0, x1, y1;
    if (!clip_region(dst, g, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    if (g->integer > 0) blit_nearest(dst, src, g, x0, y0, x1, y1);
    else                blit_box(dst, src, g, x0, y0, x1, y1);
}

/* ------------------------------------------------------------
 *  Opening the device
 * ------------------------------------------------------------ */
int disp_open(Display *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->fd = open(path, O_RDWR);
    if (d->fd < 0) { perror("open /dev/fb0"); return -1; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0) { perror("FBIOGET_VSCREENINFO"); return -1; }
    if (ioctl(d->fd, FBIOGET_FSCREENINFO, &fi) < 0) { perror("FBIOGET_FSCREENINFO"); return -1; }
    if (vi.bits_per_pixel < 8 || (vi.bits_per_pixel % 8) != 0) {
        fprintf(stderr, "display: %u bpp is not a packed pixel format\n",
                (unsigned)vi.bits_per_pixel);
        return -1;
    }
    if (vi.xres == 0 || vi.yres == 0 || fi.line_length == 0) {
        fprintf(stderr, "display: the framebuffer reports no size\n");
        return -1;
    }

    d->fb_w = (int)vi.xres;
    d->fb_h = (int)vi.yres;
    d->bpp = (int)vi.bits_per_pixel;
    d->fb_stride = (int)(fi.line_length / (vi.bits_per_pixel / 8));
    if (d->fb_stride < d->fb_w) {
        fprintf(stderr, "display: stride %d is shorter than %d pixels\n",
                d->fb_stride, d->fb_w);
        if (d->mem) munmap(d->mem, d->fb_bytes);
        d->mem = NULL;
        d->fb_bytes = 0;
        return -1;
    }
    d->w = d->fb_w;
    d->h = d->fb_h;
    d->stride = d->fb_stride;
    d->scale = DISP_ONE_TO_ONE;      /* the behaviour every caller had */
    snprintf(d->path, sizeof(d->path), "%s", path);

    d->fb_bytes = (size_t)fi.line_length * (size_t)vi.yres;
    d->mem = mmap(NULL, d->fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->mem == MAP_FAILED) { d->mem = NULL; perror("mmap fb"); return -1; }

    claim_console();
    g_disp = d;

    disp_scan_modes(d);
    d->geom = disp_layout(d->scale, d->w, d->h, d->fb_w, d->fb_h);
    d->src_w = d->w;
    d->src_h = d->h;

    fprintf(stderr, "display: %dx%d %dbpp stride=%d\n", d->fb_w, d->fb_h, d->bpp, d->fb_stride);
    if (disp_modes_source(d) == DISP_MODES_EXTRA) {
        fprintf(stderr, "display: %d mode(s) from %s/sys/class/graphics: ",
                d->nmodes, sysfs_root());
        for (int i = 0; i < d->nmodes; i++)
            fprintf(stderr, "%s%dx%d%s", i ? " " : "", d->modes[i].w, d->modes[i].h,
                    d->modes[i].native ? "*" : "");
        fputc(10, stderr);
    } else if (disp_modes_source(d) == DISP_MODES_NATIVE_ONLY) {
        fprintf(stderr, "display: the device named only its current size, "
                        "%dx%d; no other mode can be requested\n", d->fb_w, d->fb_h);
    } else {
        fprintf(stderr, "display: the device named no modes; native %dx%d only\n",
                d->fb_w, d->fb_h);
    }

    if (getenv("MYSTIC_DISP_BENCH"))
        disp_bench_report(d->w, d->h, d->fb_w, d->fb_h);
    return 0;
}

int disp_set_mode(Display *d, int i) {
    if (!d || d->fd < 0 || i < 0 || i >= d->nmodes) { errno = EINVAL; return -1; }
    DispMode *m = &d->modes[i];
    if (m->w == d->fb_w && m->h == d->fb_h) { d->mode_index = i; return 0; }

    struct fb_var_screeninfo vi;
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0) return -1;
    vi.xres = (u32)m->w;
    vi.yres = (u32)m->h;
    vi.xres_virtual = (u32)m->w;
    vi.yres_virtual = (u32)m->h;
    vi.xoffset = 0;
    vi.yoffset = 0;
    vi.activate = FB_ACTIVATE_NOW;
    if (ioctl(d->fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
        perror("FBIOPUT_VSCREENINFO");       /* the old mapping is untouched */
        return -1;
    }

    struct fb_fix_screeninfo fi;
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(d->fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        /* The driver already accepted the mode, so the mapping we still
         * hold may no longer describe the panel. Writing through it would
         * run past its end; stop drawing instead and keep the errno. */
        int e = errno ? errno : EIO;
        fprintf(stderr, "display: the driver accepted %dx%d but its geometry "
                        "cannot be read back (%s); drawing stopped\n",
                m->w, m->h, strerror(e));
        if (d->mem) munmap(d->mem, d->fb_bytes);
        d->mem = NULL;
        d->fb_bytes = 0;
        errno = e;
        return -1;
    }
    if (vi.xres == 0 || vi.yres == 0 || fi.line_length == 0 ||
        vi.bits_per_pixel < 8 || (vi.bits_per_pixel % 8) != 0) {
        errno = EIO;
        fprintf(stderr, "display: the driver accepted the mode but reports no "
                        "usable geometry\n");
        return -1;
    }
    size_t bytes = (size_t)fi.line_length * (size_t)vi.yres;
    u32 *nm = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (nm == MAP_FAILED) {
        /* The driver took the mode but the new size cannot be mapped. Writing
         * through the mapping we still hold would run off its end, so stop
         * drawing instead: the screen freezes, nothing is corrupted. */
        if (d->mem) munmap(d->mem, d->fb_bytes);
        d->mem = NULL;
        d->fb_bytes = 0;
        d->fb_w = (int)vi.xres;
        d->fb_h = (int)vi.yres;
        d->bpp = (int)vi.bits_per_pixel;
        d->fb_stride = (int)(fi.line_length / (vi.bits_per_pixel / 8));
        d->stride = d->fb_stride;
        perror("mmap fb (new mode)");
        return -1;
    }
    if ((int)(fi.line_length / (vi.bits_per_pixel / 8)) < (int)vi.xres) {
        munmap(nm, bytes);
        if (d->mem) munmap(d->mem, d->fb_bytes);
        d->mem = NULL;
        d->fb_bytes = 0;
        errno = EIO;
        fprintf(stderr, "display: the driver reports a stride shorter than the mode\n");
        return -1;
    }

    int followed_native = (d->w == d->fb_w && d->h == d->fb_h);
    if (d->mem) munmap(d->mem, d->fb_bytes);
    d->mem = nm;
    d->fb_bytes = bytes;
    d->fb_w = (int)vi.xres;
    d->fb_h = (int)vi.yres;
    d->bpp = (int)vi.bits_per_pixel;
    d->fb_stride = (int)(fi.line_length / (vi.bits_per_pixel / 8));
    d->stride = d->fb_stride;
    if (followed_native) { d->w = d->fb_w; d->h = d->fb_h; }
    d->src_w = d->w;
    d->src_h = d->h;
    d->mode_index = -1;
    for (int k = 0; k < d->nmodes; k++) {
        d->modes[k].native = (d->modes[k].w == d->fb_w && d->modes[k].h == d->fb_h);
        if (d->modes[k].native && d->mode_index < 0) d->mode_index = k;
    }
    if (d->mode_index < 0 && d->nmodes < DISP_MAX_MODES) {
        /* The driver gave a size the list did not name. Record what it
         * reported - that is still the device speaking, not us. */
        DispMode *e = &d->modes[d->nmodes];
        e->w = d->fb_w;
        e->h = d->fb_h;
        e->refresh = 0;
        e->native = 1;
        e->from_sysfs = 0;
        d->mode_index = d->nmodes++;
    }
    if (d->mode_index < 0) d->mode_index = 0;
    d->geom = disp_layout(d->scale, d->w, d->h, d->fb_w, d->fb_h);
    fprintf(stderr, "display: mode %dx%d (%d bpp)\n", d->fb_w, d->fb_h, d->bpp);
    return 0;
}

/* ------------------------------------------------------------
 *  Logical size and scale mode
 * ------------------------------------------------------------ */
void disp_set_logical_size(Display *d, int w, int h) {
    if (!d) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    d->w = w;
    d->h = h;
    /* The inverse mapper reads src_w/src_h, and it runs before the next
     * present(). Leaving them on the previous frame's size meant the first
     * click after a canvas change was mapped through the old blit. */
    d->src_w = w;
    d->src_h = h;
    d->geom = disp_layout(d->scale, d->w, d->h, d->fb_w, d->fb_h);
}

void disp_logical_size(const Display *d, int *w, int *h) {
    if (!d) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = d->w;
    if (h) *h = d->h;
}

void disp_set_scale(Display *d, DispScale m) {
    if (!d) return;
    if ((int)m < 0 || m >= DISP_SCALE_COUNT) m = DISP_ONE_TO_ONE;
    d->scale = m;
    d->geom = disp_layout(d->scale, d->w, d->h, d->fb_w, d->fb_h);
}

DispScale disp_get_scale(const Display *d) {
    return d ? d->scale : DISP_ONE_TO_ONE;
}

double disp_present_ms(const Display *d) {
    return d ? d->last_ms : 0.0;
}

const char *disp_scale_name(DispScale m) {
    switch (m) {
    case DISP_FIT:        return "fit";
    case DISP_STRETCH:    return "stretch";
    case DISP_INTEGER:    return "integer";
    case DISP_ONE_TO_ONE: return "1:1";
    default:              return "?";
    }
}

/* ------------------------------------------------------------
 *  Presenting
 * ------------------------------------------------------------ */
/* Clear one rectangle to black. The same code clears the letterbox in
 * disp_present() and is timed by disp_bench(), so the reported frame cost
 * includes the bars instead of quietly leaving them out. */
static void surf_fill_rect(Surface *dst, int x, int y, int w, int h) {
    if (!dst || !dst->px || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst->w) w = dst->w - x;
    if (y + h > dst->h) h = dst->h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = y; r < y + h; r++)
        memset(dst->px + (size_t)r * (size_t)dst->stride + x, 0,
               (size_t)w * sizeof(u32));
}

static void fb_fill_rect(Display *d, int x, int y, int w, int h) {
    Surface dst;
    dst.px = d->mem;
    dst.w = d->fb_w;
    dst.h = d->fb_h;
    dst.stride = d->fb_stride;
    surf_unclip(&dst);
    surf_fill_rect(&dst, x, y, w, h);
}

void disp_present(Display *d, Surface *s) {
    if (!d || !d->mem || !s || !s->px || s->w <= 0 || s->h <= 0) return;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    d->src_w = s->w;
    d->src_h = s->h;
    d->geom = disp_layout(d->scale, s->w, s->h, d->fb_w, d->fb_h);
    const DispGeom *g = &d->geom;

    /* The bars are black and they are cleared here, so whatever a previous
     * frame (or the kernel console) left there never shows through. */
    if (g->by > 0) fb_fill_rect(d, 0, 0, d->fb_w, g->by);
    if (g->by + g->bh < d->fb_h)
        fb_fill_rect(d, 0, g->by + g->bh, d->fb_w, d->fb_h - (g->by + g->bh));
    if (g->bx > 0) fb_fill_rect(d, 0, g->by, g->bx, g->bh);
    if (g->bx + g->bw < d->fb_w)
        fb_fill_rect(d, g->bx + g->bw, g->by, d->fb_w - (g->bx + g->bw), g->bh);

    Surface dst;
    dst.px = d->mem;
    dst.w = d->fb_w;
    dst.h = d->fb_h;
    dst.stride = d->fb_stride;
    surf_unclip(&dst);
    disp_blit(&dst, s, g, g->bx, g->by, g->bw, g->bh);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    d->last_ms = ts_ms(&t0, &t1);
}

/* Push only one rectangle to the screen. Moving the pointer used to copy all
 * 1280x800 pixels because there was no way to say "just this box"; the cursor
 * needs about 26x28 of them. Costs are proportional to what changed.
 *
 * With no scaling this is exactly the memcpy it has always been. With a scale
 * mode the same logical rectangle is mapped through the blit, so a partial
 * update (the cursor) still lands where the pointer is. */
void disp_present_rect(Display *d, Surface *s, int x, int y, int w, int h) {
    if (!d || !d->mem || !s || !s->px) return;
    if (s->w <= 0 || s->h <= 0) return;

    if (!(d->scale == DISP_ONE_TO_ONE && s->w == d->fb_w && s->h == d->fb_h)) {
        if (s->w != d->src_w || s->h != d->src_h || d->geom.bw <= 0) {
            d->src_w = s->w;
            d->src_h = s->h;
            d->geom = disp_layout(d->scale, s->w, s->h, d->fb_w, d->fb_h);
        }
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > s->w) w = s->w - x;
        if (y + h > s->h) h = s->h - y;
        if (w <= 0 || h <= 0) return;
        const DispGeom *g = &d->geom;
        int dx0, dy0, dx1, dy1;
        if (g->integer > 0) {
            dx0 = g->bx + x * g->integer;
            dx1 = g->bx + (x + w) * g->integer;
            dy0 = g->by + y * g->integer;
            dy1 = g->by + (y + h) * g->integer;
        } else {
            dx0 = g->bx + (int)(((long long)x * g->bw) / s->w);
            dx1 = g->bx + (int)((((long long)(x + w) * g->bw) + s->w - 1) / s->w);
            dy0 = g->by + (int)(((long long)y * g->bh) / s->h);
            dy1 = g->by + (int)((((long long)(y + h) * g->bh) + s->h - 1) / s->h);
        }
        Surface dst;
        dst.px = d->mem;
        dst.w = d->fb_w;
        dst.h = d->fb_h;
        dst.stride = d->fb_stride;
        surf_unclip(&dst);
        disp_blit(&dst, s, g, dx0, dy0, dx1 - dx0, dy1 - dy0);
        return;
    }

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > d->fb_w) w = d->fb_w - x;
    if (y + h > d->fb_h) h = d->fb_h - y;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        memcpy(d->mem + (size_t)row * (size_t)d->fb_stride + x,
               s->px + (size_t)row * (size_t)s->stride + x,
               (size_t)w * sizeof(u32));
}

/* ------------------------------------------------------------
 *  What a frame costs
 *
 *  Measured, not estimated: the bar clear and disp_blit() that every
 *  disp_present() runs, on scratch surfaces of the given sizes, best of
 *  `reps` frames after one untimed warm-up. The bar phase and the blit
 *  phase are timed separately inside the same iteration, and the iteration
 *  reported is the one with the lowest total, so blit + bars == total for
 *  the numbers printed. A bench that could not allocate says so instead of
 *  printing a number.
 * ------------------------------------------------------------ */
double disp_bench_parts(int src_w, int src_h, int dst_w, int dst_h, DispScale m,
                        int reps, double *blit_ms, double *bars_ms) {
    if (blit_ms) *blit_ms = -1;
    if (bars_ms) *bars_ms = -1;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return -1;
    if (reps < 1) reps = 1;

    Surface src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.w = src_w; src.h = src_h; src.stride = src_w;
    dst.w = dst_w; dst.h = dst_h; dst.stride = dst_w;
    src.px = malloc((size_t)src_w * (size_t)src_h * sizeof(u32));
    dst.px = malloc((size_t)dst_w * (size_t)dst_h * sizeof(u32));
    if (!src.px || !dst.px) { free(src.px); free(dst.px); return -1; }
    surf_unclip(&src);
    surf_unclip(&dst);

    for (int y = 0; y < src_h; y++)
        for (int x = 0; x < src_w; x++) {
            int r = src_w > 1 ? x * 255 / (src_w - 1) : 0;
            int ggg = src_h > 1 ? y * 255 / (src_h - 1) : 0;
            int b = ((x ^ y) & 16) ? 0xF0 : 0x20;
            src.px[(size_t)y * (size_t)src_w + x] = RGB(r, ggg, b);
        }
    memset(dst.px, 0, (size_t)dst_w * (size_t)dst_h * sizeof(u32));

    DispGeom g = disp_layout(m, src_w, src_h, dst_w, dst_h);
    double best = -1, best_blit = -1, best_bars = -1;
    struct timespec a, b, c;
    /* r == -1 is an untimed warm-up: the first touch of the scratch pages and
     * the first pass over a branch is not what the next frame will cost. */
    for (int r = -1; r < reps; r++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        /* exactly what disp_present() clears before the blit: a fit or a
         * centred frame leaves bars, and leaving them out of the measurement
         * would report a cost the panel does not actually pay. */
        if (g.by > 0) surf_fill_rect(&dst, 0, 0, dst_w, g.by);
        if (g.by + g.bh < dst_h)
            surf_fill_rect(&dst, 0, g.by + g.bh, dst_w, dst_h - (g.by + g.bh));
        if (g.bx > 0) surf_fill_rect(&dst, 0, g.by, g.bx, g.bh);
        if (g.bx + g.bw < dst_w)
            surf_fill_rect(&dst, g.bx + g.bw, g.by, dst_w - (g.bx + g.bw), g.bh);
        clock_gettime(CLOCK_MONOTONIC, &b);
        disp_blit(&dst, &src, &g, g.bx, g.by, g.bw, g.bh);
        clock_gettime(CLOCK_MONOTONIC, &c);
        if (r < 0) continue;
        double bars = ts_ms(&a, &b);
        double blit = ts_ms(&b, &c);
        double total = bars + blit;
        if (best < 0 || total < best) {
            best = total;
            best_bars = bars;
            best_blit = blit;
        }
    }
    free(src.px);
    free(dst.px);
    if (blit_ms) *blit_ms = best_blit;
    if (bars_ms) *bars_ms = best_bars;
    return best;
}

double disp_bench(int src_w, int src_h, int dst_w, int dst_h, DispScale m, int reps) {
    return disp_bench_parts(src_w, src_h, dst_w, dst_h, m, reps, NULL, NULL);
}

void disp_bench_report(int src_w, int src_h, int dst_w, int dst_h) {
    fprintf(stderr, "disp cost: %dx%d -> %dx%d, best of 7 frames, measured here\n",
            src_w, src_h, dst_w, dst_h);
    fprintf(stderr, "  (each frame = black bars + the scaled blit, the same two "
                    "phases disp_present runs)\n");
    for (int i = 0; i < DISP_SCALE_COUNT; i++) {
        DispScale m = (DispScale)i;
        double blit = -1, bars = -1;
        double ms = disp_bench_parts(src_w, src_h, dst_w, dst_h, m, 7, &blit, &bars);
        if (ms < 0)
            fprintf(stderr, "  %-8s unavailable (out of memory)\n", disp_scale_name(m));
        else
            fprintf(stderr, "  %-8s %.2f ms/frame = blit %.2f + bars %.2f\n",
                    disp_scale_name(m), ms, blit, bars);
    }
}
