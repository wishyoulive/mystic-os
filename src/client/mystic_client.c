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

/* read one line, blocking; returns length or -1 */
static int read_line(int fd, char *out, int max) {
    int n = 0;
    while (n < max - 1) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ch == '\n') { out[n] = 0; return n; }
        out[n++] = ch;
    }
    out[n] = 0;
    return n;
}

int mc_open(MClient *c, const char *id, const char *title, int w, int h) {
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
    if (write_all(c->fd, hello, strlen(hello)) < 0) return -1;

    char line[512];
    if (read_line(c->fd, line, sizeof(line)) < 0) return -1;
    if (strncmp(line, "OK ", 3)) {
        fprintf(stderr, "mystic: server refused: %s\n", line);
        return -1;
    }

    char shm[128];
    int stride = w;
    if (sscanf(line + 3, "%127s %d", shm, &stride) < 1) return -1;

    snprintf(c->shm, sizeof(c->shm), "%s", shm);
    c->stride = stride;

    int fd = open(shm, O_RDWR);
    if (fd < 0) { perror("open shm"); return -1; }
    size_t bytes = (size_t)stride * (size_t)h * 4;
    c->px = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (c->px == MAP_FAILED) { perror("mmap shm"); c->px = NULL; return -1; }

    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

void mc_present(MClient *c) {
    if (!c || c->fd < 0) return;
    write_all(c->fd, "FRAME\n", 6);
}

int mc_poll(MClient *c, MEvent *ev) {
    if (!c || c->fd < 0) return 0;

    for (;;) {
        ssize_t n = read(c->fd, c->rx + c->rxlen, sizeof(c->rx) - (size_t)c->rxlen - 1);
        if (n > 0) {
            c->rxlen += (int)n;
            c->rx[c->rxlen] = 0;
        } else if (n == 0) {
            ev->type = MC_CLOSE;
            return 1;
        } else {
            break;
        }
    }

    char *nl = strchr(c->rx, '\n');
    if (!nl) return 0;
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
        snprintf(ev->text, sizeof(ev->text), "%s", line + 5);
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
