/* ============================================================
 *  Mystic OS - clipbridge.h
 *
 *  The clipboard, as a sandboxed app sees it.
 *
 *  The Browser and the Viewer run chrooted, as uid 1000, with a jail
 *  that contains their payload, their bundle, /tmp, the fonts and the
 *  window socket - and nothing else. They cannot link src/clipboard.c
 *  (it would give them a second, private copy of the history) and they
 *  cannot reach /root. So the desktop binds ONE socket into their jail:
 *
 *      /run/mystic/clipboard.sock      (inside the jail)
 *
 *  and serves it in-process with the real library. The protocol on that
 *  socket is exactly the daemon's (see src/clipboard.h, "SOCKET
 *  PROTOCOL"), which is what makes this file honest: the same client
 *  code works against clipd when the app is run outside the desktop
 *  (a developer's shell, a host test) and against the desktop's bridge
 *  inside it. Same path, same bytes, two servers.
 *
 *  Only text is implemented here because text is what the page and the
 *  document can select. A file list or an image has no meaning to a
 *  renderer that owns no filesystem.
 *
 *  Every function returns 0 or a negative errno-style code; the caller
 *  says so on screen rather than pretending the copy worked.
 * ============================================================ */
#ifndef MYSTIC_CLIPBRIDGE_H
#define MYSTIC_CLIPBRIDGE_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clipboard.h"          /* CLIP_SOCKET_PATH, CLIP_MAX_TEXT */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <afunix.h>
typedef SOCKET cbr_sock;
#  define CBR_BAD INVALID_SOCKET
#  define cbr_close closesocket
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
typedef int cbr_sock;
#  define CBR_BAD (-1)
#  define cbr_close close
#endif

typedef struct {
    cbr_sock fd;
} ClipBridge;

static inline void cbr_net_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0) done = 1;
    }
#endif
}

/* Open a connection to the clipboard endpoint. Returns 0 or -errno. */
static inline int cbr_open(ClipBridge *b, const char *path) {
    b->fd = CBR_BAD;
    cbr_net_init();
    b->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (b->fd == CBR_BAD) return -errno ? -errno : -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) { cbr_close(b->fd); b->fd = CBR_BAD; return -EINVAL; }
    memcpy(a.sun_path, path, strlen(path) + 1);
    if (connect(b->fd, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0) {
        int e = errno;
        cbr_close(b->fd);
        b->fd = CBR_BAD;
        return -(e ? e : 1);
    }
    return 0;
}

static inline void cbr_close_conn(ClipBridge *b) {
    if (b->fd != CBR_BAD) { cbr_close(b->fd); b->fd = CBR_BAD; }
}

static inline int cbr_send_all(cbr_sock fd, const void *p, size_t n) {
    const char *c = p;
    while (n > 0) {
        ssize_t w = send(fd, c, (int)(n > 0x40000000u ? 0x40000000u : n), 0);
        if (w > 0) { c += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static inline int cbr_recv_all(cbr_sock fd, void *p, size_t n) {
    char *c = p;
    while (n > 0) {
        ssize_t r = recv(fd, c, (int)(n > 0x40000000u ? 0x40000000u : n), 0);
        if (r > 0) { c += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static inline int cbr_recv_line(cbr_sock fd, char *out, int cap) {
    int n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (c == '\n') { out[n] = 0; return n; }
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* Put UTF-8 text on the clipboard. 0 on success, negative on failure. */
static inline int clip_bridge_set_text(const char *text, int len) {
    if (!text || len <= 0 || len > (int)CLIP_MAX_TEXT) return -EINVAL;
    ClipBridge b;
    int rc = cbr_open(&b, CLIP_SOCKET_PATH);
    if (rc != 0) return rc;
    char head[64];
    int n = snprintf(head, sizeof(head), "SET TEXT %d\n", len);
    if (cbr_send_all(b.fd, head, (size_t)n) != 0 ||
        cbr_send_all(b.fd, text, (size_t)len) != 0) {
        cbr_close_conn(&b);
        return -EIO;
    }
    char line[256];
    rc = cbr_recv_line(b.fd, line, sizeof(line));
    cbr_close_conn(&b);
    if (rc < 0) return -EIO;
    if (strncmp(line, "OK ", 3) != 0) return -EPROTO;
    return 0;
}

/* Get the newest text entry. On success *out is a malloc'ed NUL-terminated
 * string (caller frees) and *len its byte count; -ENOENT when the clipboard
 * holds no text. */
static inline int clip_bridge_get_text(char **out, int *len) {
    if (out) *out = NULL;
    if (len) *len = 0;
    ClipBridge b;
    int rc = cbr_open(&b, CLIP_SOCKET_PATH);
    if (rc != 0) return rc;
    if (cbr_send_all(b.fd, "GET TEXT\n", 9) != 0) { cbr_close_conn(&b); return -EIO; }
    char line[512];
    rc = cbr_recv_line(b.fd, line, sizeof(line));
    if (rc < 0) { cbr_close_conn(&b); return -EIO; }
    if (strncmp(line, "OK GET ", 7) != 0) {
        cbr_close_conn(&b);
        return strstr(line, "ERR -4") ? -ENOENT : -EPROTO;
    }
    /* OK GET TEXT <imgkind> <id> <pinned> <stamp> <w> <h> <count> <len> */
    char type[16];
    int kind = 0, pinned = 0, w = 0, h = 0, count = 0, plen = -1;
    unsigned long long id = 0, stamp = 0;
    int got = sscanf(line, "OK GET %15s %d %llu %d %llu %d %d %d %d",
                     type, &kind, &id, &pinned, &stamp, &w, &h, &count, &plen);
    if (got < 9 || strcmp(type, "TEXT") != 0 || plen < 0 || plen > (int)CLIP_MAX_TEXT) {
        cbr_close_conn(&b);
        return -EPROTO;
    }
    if (plen > (int)CLIP_MAX_TEXT) { cbr_close_conn(&b); return -EPROTO; }
    char *buf = malloc((size_t)plen + 1);
    if (!buf) { cbr_close_conn(&b); return -ENOMEM; }
    if (cbr_recv_all(b.fd, buf, (size_t)plen) != 0) {
        free(buf);
        cbr_close_conn(&b);
        return -EIO;
    }
    buf[plen] = 0;
    cbr_close_conn(&b);
    if (out) *out = buf; else free(buf);
    if (len) *len = plen;
    return 0;
}

/* errno-style code to a sentence for a status line. */
static inline const char *clip_bridge_strerror(int rc) {
    switch (rc) {
        case 0:       return "ok";
        case -ENOENT: return "the clipboard holds no text";
        case -EINVAL: return "nothing to copy";
        case -EPROTO: return "the clipboard service did not answer correctly";
        case -EIO:    return "the clipboard service closed the connection";
        case -ENOMEM: return "out of memory";
        default:      return strerror(rc < 0 ? -rc : rc);
    }
}

#endif /* MYSTIC_CLIPBRIDGE_H */
