/* ============================================================
 *  Mystic OS - fb.c
 *  The screen, as the kernel offers it: /dev/fb0.
 *
 *  We map the framebuffer once and copy our finished frame into it.
 *  fb0 is what exist on old machines (vesafb/efifb/simpledrm) and is
 *  also what QEMU's bochs-drm provides, so one code path covers both.
 * ============================================================ */
#include "mystic.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Tell the kernel to stop drawing its text console on this VT.
 * Without this the console keeps painting a cursor block over our pixels. */
static void claim_console(void) {
    int fd = open("/dev/tty0", O_RDWR);
    if (fd < 0) return;
    ioctl(fd, KDSETMODE, KD_GRAPHICS);
    close(fd);
}

int disp_open(Display *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->fd = open(path, O_RDWR);
    if (d->fd < 0) { perror("open /dev/fb0"); return -1; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vi) < 0) { perror("FBIOGET_VSCREENINFO"); return -1; }
    if (ioctl(d->fd, FBIOGET_FSCREENINFO, &fi) < 0) { perror("FBIOGET_FSCREENINFO"); return -1; }

    d->w = vi.xres;
    d->h = vi.yres;
    d->bpp = vi.bits_per_pixel;
    d->stride = fi.line_length / (vi.bits_per_pixel / 8);

    size_t bytes = (size_t)fi.line_length * (size_t)vi.yres;
    d->mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->mem == MAP_FAILED) { perror("mmap fb"); return -1; }

    claim_console();
    fprintf(stderr, "display: %dx%d %dbpp stride=%d\n", d->w, d->h, d->bpp, d->stride);
    return 0;
}

void disp_present(Display *d, Surface *s) {
    disp_present_rect(d, s, 0, 0, s->w, s->h);
}

/* Push only one rectangle to the screen. Moving the pointer used to copy all
 * 1280x800 pixels because there was no way to say "just this box"; the cursor
 * needs about 26x28 of them. Costs are proportional to what changed. */
void disp_present_rect(Display *d, Surface *s, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > d->w) w = d->w - x;
    if (y + h > d->h) h = d->h - y;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        memcpy(d->mem + (size_t)row * d->stride + x,
               s->px + (size_t)row * s->stride + x,
               (size_t)w * 4);
}
