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

    fprintf(stderr, "display: %dx%d %dbpp stride=%d\n", d->w, d->h, d->bpp, d->stride);
    return 0;
}

void disp_present(Display *d, Surface *s) {
    int w = s->w < d->w ? s->w : d->w;
    int h = s->h < d->h ? s->h : d->h;
    for (int y = 0; y < h; y++)
        memcpy(d->mem + (size_t)y * d->stride, s->px + (size_t)y * s->stride, (size_t)w * 4);
}
