#define _GNU_SOURCE        /* unshare(), setgroups(), CLONE_* : musl
                            * only exposes them when this precedes
                            * every include, ours included. */
/* ============================================================
 *  Mystic OS - wmsrv.c
 *  The server half of the display protocol: a listening socket,
 *  one shared-memory buffer per app, and line-oriented messages.
 * ============================================================ */
#include "ipc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int listen_fd = -1;
static int shm_seq = 0;

/* ---------------------------------------------------------- helpers */
static void sendf(WmClient *c, const char *fmt, ...) {
    if (!c || c->fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    /* a short write on a socket is possible; a chatty error is better
     * than a silent one */
    ssize_t w = write(c->fd, buf, (size_t)n);
    (void)w;
}

static int make_shm(WmClient *c) {
    snprintf(c->shm_path, sizeof(c->shm_path), "/dev/shm/mystic-%d-%d", (int)getpid(), ++shm_seq);
    unlink(c->shm_path);
    int fd = open(c->shm_path, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) return -1;

    size_t bytes = (size_t)c->stride * (size_t)c->h * 4;
    if (ftruncate(fd, (off_t)bytes) < 0) { close(fd); unlink(c->shm_path); return -1; }

    c->px = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c->px == MAP_FAILED) { close(fd); unlink(c->shm_path); c->px = NULL; return -1; }
    c->shm_fd = fd;
    /* start black so an app that has not drawn yet is not garbage */
    memset(c->px, 0, bytes);
    /* the app now runs as uid 1000 inside its jail and still has to draw */
    if (chown(c->shm_path, 1000, 1000) != 0)
        fprintf(stderr, "wms: chown shm failed: %s\n", strerror(errno));
    return 0;
}

static void free_shm(WmClient *c) {
    if (c->px && c->px != MAP_FAILED) {
        size_t bytes = (size_t)c->stride * (size_t)c->h * 4;
        munmap(c->px, bytes);
    }
    c->px = NULL;
    if (c->shm_fd >= 0) close(c->shm_fd);
    c->shm_fd = -1;
    if (c->shm_path[0]) unlink(c->shm_path);
    c->shm_path[0] = 0;
}

/* ---------------------------------------------------------- start */
int wms_start(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "%s", WM_SOCKET);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    unlink(WM_SOCKET);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    listen(listen_fd, 8);
    /* connect() needs write permission on the socket inode, and apps now run as
     * uid 1000. This is a single-tenant system - every local process is one of
     * our sandboxed apps - so the socket is connectable, while what is behind
     * it (the filesystem) is not: see the jail. */
    chmod(WM_SOCKET, 0666);
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    fprintf(stderr, "wms: listening on %s\n", WM_SOCKET);
    return 0;
}

/* ---------------------------------------------------------- accept */
static WmClient *slot_free(WmClient *cl, int n) {
    for (int i = 0; i < n; i++) if (!cl[i].used) return &cl[i];
    return NULL;
}

static void handle_line(WmClient *c, char *line) {
    if (!strncmp(line, "HELLO ", 6)) {
        char title[80] = "app";
        int w = 400, h = 300;
        /* HELLO <id> <w> <h> <title...> */
        char id[32] = "app";
        if (sscanf(line + 6, "%31s %d %d %79[^\n]", id, &w, &h, title) < 3) {
            sendf(c, "ERR bad hello\n");
            return;
        }
        if (w < 80) w = 80;
        if (h < 60) h = 60;
        if (w > 1600) w = 1600;
        if (h > 1200) h = 1200;

        /* A second HELLO on the same connection replaces the window. Release
         * the previous buffer first: make_shm overwrites shm_path/shm_fd/px,
         * so without this the old mapping, its fd and its /dev/shm file leak
         * for the lifetime of the server. */
        if (c->shm_fd >= 0 || c->px || c->shm_path[0]) {
            fprintf(stderr, "wms: %s sent HELLO twice, replacing its buffer\n",
                    c->id[0] ? c->id : "client");
            free_shm(c);
        }

        snprintf(c->id, sizeof(c->id), "%s", id);
        snprintf(c->title, sizeof(c->title), "%s", title);
        c->w = w;
        c->h = h;
        c->stride = w;                       /* keep it simple and explicit */

        if (make_shm(c) < 0) {
            sendf(c, "ERR no shm\n");
            /* nothing to draw into: drop the connection deliberately rather
             * than leaving a client that can never present a frame */
            fprintf(stderr, "wms: %s could not get a buffer, disconnecting\n", c->id);
            wms_shutdown_client(c);
            return;
        }
        sendf(c, "OK %s %d\n", c->shm_path, c->stride);
        c->want_frame = 1;
        fprintf(stderr, "wms: %s connected %dx%d\n", c->id, c->w, c->h);
        return;
    }
    if (!strncmp(line, "FRAME", 5)) { c->want_frame = 1; return; }
    if (!strncmp(line, "TITLE ", 6)) {
        snprintf(c->title, sizeof(c->title), "%s", line + 6);
        return;
    }
    if (!strncmp(line, "CLOSE", 5)) { wms_shutdown_client(c); return; }
    if (!strncmp(line, "EVENT ", 6)) {
        /* apps may report their own status; we only log it */
        fprintf(stderr, "wms: %s says %s\n", c->id, line + 6);
        return;
    }
}

static void read_client(WmClient *c) {
    for (;;) {
        size_t space = sizeof(c->rx) - (size_t)c->rxlen - 1;
        if (space == 0) {
            /* The buffer is full and the loop above consumed every complete
             * line in it, so what is left is one line longer than rx[]. Never
             * read 0 bytes here (read()==0 means EOF) and never let rxlen run
             * past the array: reject the line explicitly and drop the client,
             * logging why. */
            fprintf(stderr, "wms: %s sent a line longer than %d bytes, disconnecting\n",
                    c->id[0] ? c->id : "client", (int)sizeof(c->rx) - 1);
            sendf(c, "ERR line too long\n");
            /* Swallow what the app is still sending before closing: closing a
             * socket that still has unread input makes the peer's kernel reset
             * the connection, which would throw away the ERR we just sent. */
            for (int i = 0; i < 16; i++) {
                char sink[256];
                ssize_t d = read(c->fd, sink, sizeof(sink));
                if (d <= 0) break;
            }
            wms_shutdown_client(c);
            return;
        }
        ssize_t n = read(c->fd, c->rx + c->rxlen, space);
        if (n > 0) {
            c->rxlen += (int)n;
            c->rx[c->rxlen] = 0;
            char *start = c->rx;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = 0;
                handle_line(c, start);
                if (!c->used) return;
                start = nl + 1;
            }
            int rest = (int)(c->rx + c->rxlen - start);
            if (rest > 0) memmove(c->rx, start, (size_t)rest);
            c->rxlen = rest;
            c->rx[rest] = 0;          /* the tail is a partial line, keep it */
            continue;
        }
        if (n == 0) { wms_shutdown_client(c); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        wms_shutdown_client(c);
        return;
    }
}

/* Called every frame with the client table. Accepts new connections and
 * reads whatever is pending. Never blocks. */
void wms_poll(WmClient *cl, int n) {
    if (listen_fd < 0) return;

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) break;

        WmClient *c = slot_free(cl, n);
        if (!c) { close(fd); continue; }
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->fd = fd;
        c->shm_fd = -1;
        c->win = -1;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fprintf(stderr, "wms: connection accepted\n");
    }

    for (int i = 0; i < n; i++)
        if (cl[i].used) read_client(&cl[i]);
}

/* ---------------------------------------------------------- notify */
void wms_notify_key(WmClient *c, int key, int mods, int press) {
    sendf(c, "KEY %d %d %d\n", key, mods, press);
}
void wms_notify_text(WmClient *c, const char *utf8) {
    /* The event stream is line oriented, so a newline inside the text would end
     * the message early: Enter reached every sandboxed app as an EMPTY text
     * event and so never did anything. Line breakers travel as 0x01. */
    char buf[512];
    size_t o = 0;
    for (const char *p = utf8 ? utf8 : ""; *p && o + 1 < sizeof(buf); p++)
        buf[o++] = (*p == 10 || *p == 13) ? 1 : *p;
    buf[o] = 0;
    sendf(c, "TEXT %s\n", buf);
}
void wms_notify_mouse(WmClient *c, int x, int y, int buttons) {
    sendf(c, "MOUSE %d %d %d\n", x, y, buttons);
}
void wms_notify_resize(WmClient *c, int w, int h) {
    sendf(c, "RESIZE %d %d\n", w, h);
}
void wms_notify_close(WmClient *c) {
    sendf(c, "CLOSE\n");
}

void wms_shutdown_client(WmClient *c) {
    if (!c || !c->used) return;
    free_shm(c);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->used = 0;
    c->win = -1;
}

/* ---------------------------------------------------------- apps */
static int cmp_app(const void *a, const void *b) {
    return strcmp(((const WmApp *)a)->id, ((const WmApp *)b)->id);
}

int wms_scan_apps(WmApp *out, int max) {
    DIR *d = opendir(WM_APP_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/manifest", WM_APP_DIR, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        WmApp *a = &out[n];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", e->d_name);

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = line, *v = eq + 1;
            while (*k == ' ') k++;
            while (*v == ' ') v++;
            char *ke = k + strlen(k);
            while (ke > k && ke[-1] == ' ') *--ke = 0;
            if (!strcmp(k, "name")) snprintf(a->name, sizeof(a->name), "%s", v);
            else if (!strcmp(k, "desc")) snprintf(a->desc, sizeof(a->desc), "%s", v);
            else if (!strcmp(k, "exec")) snprintf(a->exe, sizeof(a->exe), "%s", v);
            else if (!strcmp(k, "network") && !strcmp(v, "host")) a->net = 1;
        }
        fclose(f);
        if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
        if (!a->exe[0]) snprintf(a->exe, sizeof(a->exe), "bin");
        n++;
    }
    closedir(d);
    /* readdir order is arbitrary; sort so the launcher is stable */
    qsort(out, (size_t)n, sizeof(WmApp), cmp_app);
    return n;
}

/* ---------------------------------------------------------- sandbox
 * Every app runs in its own mount/network/IPC namespace, chrooted into a jail
 * that contains only its own binary, a private /tmp, /dev/null and the window
 * socket. It runs as uid 1000, so even if it escaped the chroot it could not
 * read /root (root-owned, mode 0700).
 *
 * This is what stops a malicious app from being ransomware: it never sees the
 * user's files to encrypt. The kernel enforces it (namespaces + DAC), not us.
 */
#define SANDBOX_UID 1000
#define SANDBOX_GID 1000

/* mkdir -p inside the jail, for the bind targets */
static void mkdirp_for(const char *jail, const char *sub) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s", jail, sub);
    for (char *p = buf + strlen(jail) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void sandbox_child(const char *exe, const char *id, int net) {
    char jail[192], path[256];
    snprintf(jail, sizeof(jail), "/run/mystic/jail/%s", id);

    /* Nothing above stderr belongs to the app. Without this it inherits our
     * root-opened /dev/fb0 (read AND write the whole screen, forge a lock
     * screen) and the evdev keyboard (the unlock PIN in cleartext), plus the
     * listener and every other client's socket and shm buffer. Namespaces hide
     * paths; they do not take away file descriptors. */
    for (int fd = 3; fd < 1024; fd++) close(fd);

    /* An app that declares 'network = host' keeps the real namespace: a
     * browser in an empty netns can only reach loopback, which is exactly the
     * 'name lookup timed out' it used to report. Everything else stays blind. */
    int ns = CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | (net ? 0 : CLONE_NEWNET);
    if (unshare(ns) == 0) {
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

        /* mkdir(2) does not create parents: /run/mystic/jail must exist
         * before /run/mystic/jail/<id>, or every bind below fails ENOENT. */
        mkdir("/run/mystic/jail", 0755);
        if (mkdir(jail, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "wms: sandbox jail mkdir %s failed: %s", jail, strerror(errno));
            fputc(10, stderr);
        }
        struct stat jst;
        if (stat(jail, &jst) != 0) {
            fprintf(stderr, "wms: sandbox jail %s does not exist after mkdir: %s", jail, strerror(errno));
            fputc(10, stderr);
        }
        snprintf(path, sizeof(path), "%s/bin", jail);        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/dev", jail);        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/tmp", jail);        mkdir(path, 0777);
        snprintf(path, sizeof(path), "%s/run", jail);        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/run/mystic", jail); mkdir(path, 0755);

        snprintf(path, sizeof(path), "%s/bin/app", jail);
        int fd = open(path, O_CREAT | O_RDWR, 0755);
        if (fd >= 0) close(fd);
        if (mount(exe, path, NULL, MS_BIND, NULL) < 0)
            fprintf(stderr, "wms: sandbox bind exe failed: %s\n", strerror(errno));

        snprintf(path, sizeof(path), "%s/run/mystic/wm.sock", jail);
        fd = open(path, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) close(fd);
        mount(WM_SOCKET, path, NULL, MS_BIND, NULL);

        if (net) {
            /* getaddrinfo needs these two; ro, and only when asked for */
            snprintf(path, sizeof(path), "%s/etc", jail);   mkdir(path, 0755);
            const char *cf[] = { "/etc/resolv.conf", "/etc/hosts" };
            for (int ci = 0; ci < 2; ci++) {
                snprintf(path, sizeof(path), "%s%s", jail, cf[ci]);
                fd = open(path, O_CREAT | O_RDWR, 0644);
                if (fd >= 0) close(fd);
                if (mount(cf[ci], path, NULL, MS_BIND | MS_RDONLY, NULL) < 0)
                    fprintf(stderr, "wms: sandbox bind %s failed: %s\n", cf[ci], strerror(errno));
            }
        }

        snprintf(path, sizeof(path), "%s/dev/null", jail);
        fd = open(path, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) close(fd);
        mount("/dev/null", path, NULL, MS_BIND, NULL);

        /* An app needs exactly four more things to be useful:
         *   /usr/share/fonts  text (read-only)
         *   /dev/shm          its own pixel buffer, owned by SANDBOX_UID
         *   /proc             sysmon/monitor read it; the rest ignore it
         * Nothing else is visible, so there is no path to /root from in here. */
        snprintf(path, sizeof(path), "%s/usr/share/fonts", jail);
        mkdirp_for(jail, "/usr/share/fonts");
        if (mount("/usr/share/fonts", path, NULL, MS_BIND | MS_RDONLY, NULL) < 0)
            fprintf(stderr, "wms: sandbox fonts bind failed: %s\n", strerror(errno));

        snprintf(path, sizeof(path), "%s/dev/shm", jail);
        mkdirp_for(jail, "/dev/shm");
        mount("/dev/shm", path, NULL, MS_BIND, NULL);

        /* seeding RNG is a legitimate need (2048, paint's palette); this is
         * read-only randomness, not a channel to anything */
        snprintf(path, sizeof(path), "%s/dev/urandom", jail);
        mkdirp_for(jail, "/dev");
        int ufd = open(path, O_CREAT | O_RDWR, 0666);
        if (ufd >= 0) close(ufd);
        mount("/dev/urandom", path, NULL, MS_BIND | MS_RDONLY, NULL);

        snprintf(path, sizeof(path), "%s/proc", jail);
        mkdirp_for(jail, "/proc");
        mount("/proc", path, NULL, MS_BIND, NULL);

        if (chroot(jail) == 0) {
            chdir("/");
            setgroups(0, NULL);
            if (setgid(SANDBOX_GID) != 0 || setuid(SANDBOX_UID) != 0) {
                fprintf(stderr, "wms: sandbox uid drop failed: %s; refusing to run\n",
                        strerror(errno));
                _exit(126);
            }
        } else {
            fprintf(stderr, "wms: chroot failed: %s; refusing to run\n", strerror(errno));
            _exit(124);
        }
    } else {
        /* fail closed: an app that cannot be isolated must not run at all,
         * rather than quietly running as root with the whole filesystem */
        fprintf(stderr, "wms: unshare failed (%s); refusing to run %s\n",
                strerror(errno), id);
        _exit(125);
    }

    setenv("MYSTIC_SOCKET", "/run/mystic/wm.sock", 1);
    setenv("MYSTIC_APP_ID", id, 1);
    setenv("HOME", "/tmp", 1);
    setenv("TMPDIR", "/tmp", 1);
}

int wms_launch(const WmApp *app, WmClient *cl, int slot) {
    if (!app || slot < 0) return -1;
    char exe[512];
    snprintf(exe, sizeof(exe), "%s/%s/%s", WM_APP_DIR, app->id, app->exe);

    /* exec= comes from a manifest, which is untrusted input: a bare filename
     * only. ".." or a slash would let a bundle point at /bin/sh, and the old
     * unconditional host fallback would then run it as root. */
    if (!app->exe[0] || strchr(app->exe, '/') || strstr(app->exe, "..")) {
        fprintf(stderr, "wms: refusing bundle %s: unsafe exec=%.40s\n", app->id, app->exe);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        sandbox_child(exe, app->id, app->net);
        execl("/bin/app", "app", (char *)NULL);   /* inside the jail, or not at all */
        fprintf(stderr, "wms: cannot exec the jailed %s\n", app->id);
        _exit(127);
    }
    (void)cl;
    return (int)pid;
}
