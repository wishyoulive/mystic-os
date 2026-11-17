/* ============================================================
 *  Mystic OS - client library (SDK) implementation
 *  Speaks the protocol in src/ipc.h. No dependencies beyond libc.
 * ============================================================ */
#include "mystic_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ui.c provides this; the SDK keeps no dependency on mystic.h, which drags
 * in the whole desktop header set */
void ui_scale_set_effective(float v);

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Read one '\n'-terminated line, blocking, into out (max bytes including the
 * terminator). Returns the length, or -1 on EOF, error, or a line that does
 * not fit in out. In the last case the remainder of the line is still
 * consumed, so a caller can never inherit a half-read greeting in the stream.
 * A short read is not an error: bytes are collected until the newline, which
 * is what makes a line that arrives in several pieces work. */
static int read_line(int fd, char *out, int max) {
    int n = 0, over = 0;
    if (!out || max < 2) return -1;
    for (;;) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 0) return -1;              /* EOF in the middle of a line */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ch == '\n') break;
        if (n < max - 1) out[n++] = ch;
        else over = 1;                      /* keep draining to the '\n'  */
    }
    out[n] = 0;
    return over ? -1 : n;
}

/* close the socket on any mc_open failure and report the error */
static int mc_open_fail(MClient *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    return -1;
}

int mc_open(MClient *c, const char *id, const char *title, int w, int h) {
    /* The desktop's text scale, exported by the window server: an app cannot
     * read ui.conf from inside its jail, and without this its fonts ignored
     * the setting entirely. */
    {
        const char *s = getenv("MYSTIC_UI_SCALE");
        if (s && *s) ui_scale_set_effective((float)atof(s));
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->w = w;
    c->h = h;

    const char *path = getenv("MYSTIC_SOCKET");
    if (!path) path = "/run/mystic/wm.sock";

    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    char hello[512];
    snprintf(hello, sizeof(hello), "HELLO %s %d %d %s\n", id, w, h, title);
    if (write_all(c->fd, hello, strlen(hello)) < 0) return mc_open_fail(c);

    char line[512];
    if (read_line(c->fd, line, sizeof(line)) < 0) return mc_open_fail(c);
    if (strncmp(line, "OK ", 3)) {
        fprintf(stderr, "mystic: server refused: %s\n", line);
        return mc_open_fail(c);
    }

    char shm[128];
    int stride = w;
    if (sscanf(line + 3, "%127s %d", shm, &stride) < 1) return mc_open_fail(c);

    /* the server allocates the buffer for the whole screen and tells us the
     * maximum; the window size is our own request, and RESIZE events move
     * the drawing area inside that mapping */
    int maxw = w, maxh = h;
    if (sscanf(line + 3, "%127s %d %d %d", shm, &stride, &maxw, &maxh) < 3) {
        maxw = stride > 0 ? stride : w;
        maxh = h;
    }
    if (maxw < w) maxw = w;
    if (maxh < h) maxh = h;
    c->maxw = maxw;
    c->maxh = maxh;
    snprintf(c->shm, sizeof(c->shm), "%s", shm);
    c->stride = stride;

    int fd = open(shm, O_RDWR);
    if (fd < 0) { perror("open shm"); return mc_open_fail(c); }
    size_t bytes = (size_t)stride * (size_t)maxh * 4;
    c->px = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c->px == MAP_FAILED) {
        perror("mmap shm");
        c->px = NULL;
        return mc_open_fail(c);
    }

    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

void mc_present(MClient *c) {
    if (!c || c->fd < 0) return;
    write_all(c->fd, "FRAME\n", 6);
}

int mc_poll(MClient *c, MEvent *ev) {
    if (!c || c->fd < 0) return 0;

    int eof = 0;
    for (;;) {
        /* Never ask the kernel for 0 bytes. read(fd, buf, 0) returns 0, which
         * is indistinguishable from EOF, so a full rx[] used to be reported as
         * MC_CLOSE while the server was still alive and talking. When there is
         * no room left we stop reading and let the caller consume a line; the
         * unread bytes stay in the socket for the next call. */
        size_t space = sizeof(c->rx) - (size_t)c->rxlen - 1;
        if (space == 0) break;
        ssize_t n = read(c->fd, c->rx + c->rxlen, space);
        if (n > 0) {
            c->rxlen += (int)n;
            c->rx[c->rxlen] = 0;
            continue;
        }
        if (n == 0) { eof = 1; break; }      /* genuine EOF */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        eof = 1;                             /* real error: the peer is gone */
        break;
    }

    char *nl = strchr(c->rx, '\n');
    if (!nl) {
        if (c->rxlen >= (int)sizeof(c->rx) - 1) {
            /* One line longer than rx[]: it can never be completed in this
             * buffer, so drop it instead of wedging the reader forever. The
             * server's longest line is "TEXT " plus 31 bytes, so this can only
             * be a broken peer. */
            c->rxlen = 0;
            c->rx[0] = 0;
            return 0;
        }
        if (eof) { ev->type = MC_CLOSE; return 1; }
        return 0;
    }
    *nl = 0;

    char line[256];
    snprintf(line, sizeof(line), "%s", c->rx);
    int rest = (int)(c->rx + c->rxlen - (nl + 1));
    memmove(c->rx, nl + 1, (size_t)rest);
    c->rxlen = rest;

    memset(ev, 0, sizeof(*ev));
    if (!strncmp(line, "KEY ", 4)) {
        ev->type = MC_KEY;
        sscanf(line + 4, "%d %d %d", &ev->key, &ev->mods, &ev->press);
        return 1;
    }
    if (!strncmp(line, "TEXT ", 5)) {
        ev->type = MC_TEXT;
        /* 0x01 stands for a line breaker: the stream is line oriented, so a
         * real newline inside the payload would cut the message short. */
        {
            size_t k = 0;
            for (const char *q = line + 5; *q && k + 1 < sizeof(ev->text); q++)
                ev->text[k++] = (*q == 1) ? 10 : *q;
            ev->text[k] = 0;
        }
        return 1;
    }
    if (!strncmp(line, "WHEEL ", 6)) {
        ev->type = MC_WHEEL;
        ev->wheel = atoi(line + 6);
        return 1;
    }
    if (!strncmp(line, "MOUSE ", 6)) {
        ev->type = MC_MOUSE;
        sscanf(line + 6, "%d %d %d", &ev->x, &ev->y, &ev->buttons);
        return 1;
    }
    if (!strncmp(line, "RESIZE ", 7)) {
        ev->type = MC_RESIZE;
        sscanf(line + 7, "%d %d", &ev->w, &ev->h);
        return 1;
    }
    if (!strncmp(line, "CLOSE", 5)) { ev->type = MC_CLOSE; return 1; }
    return 0;
}

void mc_close(MClient *c) {
    if (!c) return;
    if (c->fd >= 0) {
        write_all(c->fd, "CLOSE\n", 6);
        close(c->fd);
    }
    if (c->px && c->px != MAP_FAILED)
        munmap(c->px, (size_t)c->stride * (size_t)c->h * 4);
    c->px = NULL;
    c->fd = -1;
}

void mc_fill(MClient *c, uint32_t color) {
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->stride; x++)
            c->px[(size_t)y * c->stride + x] = color;
}

void mc_rect(MClient *c, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->stride) w = c->stride - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            c->px[(size_t)j * c->stride + i] = color;
}
