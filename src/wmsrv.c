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
#include <time.h>
#include <unistd.h>

static int listen_fd = -1;
static int shm_seq = 0;

/* The reason the last launch failed, kept for the launcher to pick up with
 * wms_take_launch_error(). Empty when the last launch worked. */
static char last_launch_error[192];

/* ---------------------------------------------------------- paths
 * The run dir and the two bundle roots are constants in the shipped system.
 * MYSTIC_RUN_DIR and MYSTIC_APP_ROOTS ("<image>:<installed>") let the host
 * harness point the real scanner at a fixture tree - the same test hook pkg has
 * with MYSTIC_APPS. Only the owner of this process can set its environment, so
 * neither is a way in; a desktop with them unset behaves exactly as before.
 */
static const char *run_dir(void) {
    const char *e = getenv("MYSTIC_RUN_DIR");
    return (e && e[0]) ? e : WM_RUN_DIR;
}

static void run_file(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", run_dir(), leaf);
}

static void app_roots(char *image, size_t ni, char *installed, size_t nw) {
    snprintf(image, ni, "%s", WM_APP_DIR);
    snprintf(installed, nw, "%s", WM_APP_DIR_RW);
    const char *e = getenv("MYSTIC_APP_ROOTS");
    if (!e || !e[0]) return;
    const char *colon = strchr(e, ':');
    if (!colon) return;
    size_t l = (size_t)(colon - e);
    if (l > 0 && l < ni) { memcpy(image, e, l); image[l] = 0; }
    if (colon[1] && strlen(colon + 1) < nw)
        snprintf(installed, nw, "%s", colon + 1);
}

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

static int screen_w = 1280, screen_h = 800;

void wms_set_screen(int w, int h) {
    if (w > 0 && h > 0) { screen_w = w; screen_h = h; }
}

static int make_shm(WmClient *c) {
    snprintf(c->shm_path, sizeof(c->shm_path), "/dev/shm/mystic-%d-%d", (int)getpid(), ++shm_seq);
    unlink(c->shm_path);
    int fd = open(c->shm_path, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) return -1;

    size_t bytes = (size_t)c->stride * (size_t)c->maxh * 4;
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
        size_t bytes = (size_t)c->stride * (size_t)c->maxh * 4;
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
        /* The buffer is allocated for the whole screen, not the size the app
         * asked to open at: otherwise a client can never grow, and maximising
         * its window just draws the same small picture in a bigger frame. */
        c->maxw = w > screen_w ? w : screen_w;
        c->maxh = h > screen_h ? h : screen_h;
        c->stride = c->maxw;                 /* keep it simple and explicit */

        if (make_shm(c) < 0) {
            sendf(c, "ERR no shm\n");
            /* nothing to draw into: drop the connection deliberately rather
             * than leaving a client that can never present a frame */
            fprintf(stderr, "wms: %s could not get a buffer, disconnecting\n", c->id);
            wms_shutdown_client(c);
            return;
        }
        sendf(c, "OK %s %d %d %d\n", c->shm_path, c->stride, c->maxw, c->maxh);
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
void wms_notify_wheel(WmClient *c, int notches) {
    /* A wheel has no position of its own: the client already knows where its
     * pointer is from the MOUSE events. One number, positive = up. */
    sendf(c, "WHEEL %d\n", notches);
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

/* Two roots, on purpose: the image's own bundles, and /root/apps where pkg
 * unpacks what the user installed - /root is the only place that survives a
 * reboot. Scanning just the first meant a package installed from the Store
 * could never be launched; and because the launcher then exec'd everything from
 * WM_APP_DIR, the tiles it did draw for installed bundles were dead. Each
 * WmApp now remembers its root and wms_launch() uses it. */
static int scan_root(WmApp *out, int n, int max, const char *root, int from_image) {
    if (strlen(root) >= sizeof(out[0].root)) {
        fprintf(stderr, "wms: app root %.80s is too long to remember; skipped\n", root);
        return n;
    }
    DIR *d = opendir(root);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        /* the id becomes a path component twice (manifest, payload) */
        if (strchr(e->d_name, '/')) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/manifest", root, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        WmApp *a = &out[n];
        memset(a, 0, sizeof(*a));
        a->from_image = from_image;
        snprintf(a->id, sizeof(a->id), "%s", e->d_name);
        snprintf(a->root, sizeof(a->root), "%s", root);

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
            else if (!strcmp(k, "audio") && !strcmp(v, "yes")) a->audio = 1;
        }
        fclose(f);
        if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
        if (!a->exe[0]) snprintf(a->exe, sizeof(a->exe), "bin");
        n++;
    }
    closedir(d);
    return n;
}

int wms_bundle_payload(const WmApp *app, char *out, size_t cap,
                       char *why, size_t whyn) {
    if (why && whyn) why[0] = 0;
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!app) return -1;

    /* exec= and the directory name both come off the disk, so both are
     * untrusted: a bare file name and a bare directory name, or nothing. A
     * '..' or a slash here would point the launcher at a file of the bundle's
     * choosing instead of its own payload. */
    if (!app->id[0] || strchr(app->id, '/') || strstr(app->id, "..")) {
        if (why && whyn) snprintf(why, whyn, "unusable bundle id '%.31s'", app->id);
        return -1;
    }
    if (!app->exe[0] || strchr(app->exe, '/') || strstr(app->exe, "..")) {
        if (why && whyn) snprintf(why, whyn, "unsafe exec '%.31s'", app->exe);
        return -1;
    }

    const char *root = app->root[0] ? app->root : WM_APP_DIR;
    int k = snprintf(out, cap, "%s/%s/%s", root, app->id, app->exe);
    if (k < 0 || (size_t)k >= cap) {
        if (why && whyn) snprintf(why, whyn, "path longer than %d bytes", (int)cap - 1);
        out[0] = 0;
        return -1;
    }

    /* The three ways a bundle is dead: nothing there, not a file, not
     * executable. Each one is reported with the kernel's own words, because
     * "it did not start" is not a diagnosis. */
    struct stat st;
    if (stat(out, &st) != 0) {
        if (why && whyn) snprintf(why, whyn, "%s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        if (why && whyn) snprintf(why, whyn, "not a regular file");
        return -1;
    }
    if (access(out, X_OK) != 0) {
        if (why && whyn) snprintf(why, whyn, "%s", strerror(errno));
        return -1;
    }
    return 0;
}

/* 1 when the payload is there and executable. Used to decide which of two
 * bundles with the same id is worth a tile. */
static int payload_ok(const WmApp *a) {
    char path[512], why[96];
    return wms_bundle_payload(a, path, sizeof(path), why, sizeof(why)) == 0;
}

/* Two roots can hold the same id, and exactly one of them gets the tile. Two
 * rules, both of which shipping needs:
 *
 *   - a bundle whose payload is missing or not executable cannot start, so it
 *     must not shadow one that can. A half-finished install then leaves the
 *     shipped app working instead of turning a good tile into a dead one.
 *
 *   - when both can start, the installed copy wins. /usr/share is the image: it
 *     cannot be upgraded without reflashing, while /root/apps is only there
 *     because the user installed it on purpose. This is what makes the 'vlc'
 *     package replace the shipped vlcplayer stub instead of leaving a second
 *     tile for one program.
 *
 * A tie (neither works) keeps the image's copy, so the error names the bundle
 * that shipped with the OS. The decision is symmetric in the argument order
 * because qsort is not stable: whichever copy comes first, the same one wins. */
static int merge_dups(WmApp *out, int n) {
    int keep = 0;
    for (int i = 0; i < n; i++) {
        int at = -1;
        for (int j = 0; j < keep; j++)
            if (!strcmp(out[j].id, out[i].id)) { at = j; break; }
        if (at < 0) {
            if (keep != i) out[keep] = out[i];
            keep++;
            continue;
        }
        int cand_ok = payload_ok(&out[i]);
        int kept_ok = payload_ok(&out[at]);
        int take;
        const char *why;
        if (cand_ok != kept_ok) {
            /* exactly one can start: it gets the tile, whatever order it came in */
            take = cand_ok;
            why = take ? "the other copy has no runnable payload"
                       : "this copy has no runnable payload";
        } else if (cand_ok) {
            /* both run: the installed copy wins, so a package can replace a
             * stub the image shipped */
            take = !out[i].from_image && out[at].from_image;
            why = "both run, and the installed copy wins";
        } else {
            /* neither runs: keep the shipped bundle, so the error names it */
            take = out[i].from_image && !out[at].from_image;
            why = "neither copy runs; keeping the shipped one";
        }
        if (take) {
            fprintf(stderr, "wms: bundle %s: launching %.120s, not %.120s (%s)\n",
                    out[i].id, out[i].root, out[at].root, why);
            out[at] = out[i];
        } else {
            fprintf(stderr, "wms: bundle %s: launching %.120s, not %.120s (%s)\n",
                    out[at].id, out[at].root, out[i].root, why);
        }
    }
    return keep;
}

int wms_scan_apps(WmApp *out, int max) {
    if (!out || max <= 0) return 0;
    char image[256], installed[256];
    app_roots(image, sizeof(image), installed, sizeof(installed));
    int n = scan_root(out, 0, max, image, 1);
    n = scan_root(out, n, max, installed, 0);
    if (n == max)
        fprintf(stderr, "wms: app list is full (%d); a bundle may be missing\n", max);

    /* readdir order is arbitrary; sort so the launcher is stable. */
    qsort(out, (size_t)n, sizeof(WmApp), cmp_app);
    return merge_dups(out, n);
}

int wms_apps_poll_rescan(WmApp *out, int *count, int max) {
    if (!out || max <= 0) return 0;

    char req[256];
    run_file(req, sizeof(req), WM_RESCAN_LEAF);
    struct stat st;
    if (stat(req, &st) != 0) return 0;

    /* Take the request before scanning: pkg writes it after the bundle is in
     * place, and a request we cannot remove must not turn into a rescan (and a
     * few hundred stat() calls) on every frame. Remember which one we could not
     * delete instead. */
    static long long stubborn_ino = -1, stubborn_mtime = -1;
    if (unlink(req) != 0 && errno != ENOENT) {
        if ((long long)st.st_ino == stubborn_ino && (long long)st.st_mtime == stubborn_mtime)
            return 0;
        stubborn_ino = (long long)st.st_ino;
        stubborn_mtime = (long long)st.st_mtime;
        fprintf(stderr, "wms: cannot remove %s (%s); scanning anyway\n", req, strerror(errno));
    } else {
        stubborn_ino = -1;
        stubborn_mtime = -1;
    }

    int n = wms_scan_apps(out, max);
    if (count) *count = n;
    fprintf(stderr, "wms: rescan: %d bundle(s)\n", n);
    return 1;
}

/* ---------------------------------------------------------- launch errors
 * A tile that does not start used to fail three syscalls deep in a forked
 * child and leave nothing behind but a pid. Every failure now becomes one line
 * that says which bundle, which path and what the kernel said - logged, handed
 * to the launcher through wms_take_launch_error(), and written to
 * WM_LAUNCH_ERR so it survives even a launcher that never asks.
 */
static void launch_failed(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_launch_error, sizeof(last_launch_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "wms: launch failed: %s\n", last_launch_error);

    char path[256];
    run_file(path, sizeof(path), WM_LAUNCH_LEAF);
    mkdir(run_dir(), 0755);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "wms: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", last_launch_error);
    fclose(f);
}

int wms_take_launch_error(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!last_launch_error[0]) { out[0] = 0; return 0; }
    snprintf(out, (size_t)cap, "%s", last_launch_error);
    last_launch_error[0] = 0;
    return 1;
}

/* ---------------------------------------------------------- sandbox
 * Every app runs in its own mount/network/IPC namespace, chrooted into a jail
 * that contains its own payload, its own bundle (read-only, at /app), a private
 * /tmp, /dev/null and the window socket. It runs as uid 1000, so even if it
 * escaped the chroot it could not read /root (root-owned, mode 0700).
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

/* One line from the child, before it becomes the app: what failed, on what
 * path, and what the kernel said. The write end is close-on-exec, so the parent
 * treats "the pipe closed without a word" as proof that exec worked. */
#define LAUNCH_MSG_MAX 256

static void child_report(int fd, const char *what, const char *path) {
    if (fd < 0) return;
    int err = errno;                  /* before anything else can clobber it */
    char buf[LAUNCH_MSG_MAX];
    int n = snprintf(buf, sizeof(buf), "%s %.160s: %s", what, path ? path : "",
                     strerror(err));
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
}

static void sandbox_child(const char *exe, const char *id, int net, int audio,
                          const char *arg, int errfd) {
    char jail[192], path[256];
    snprintf(jail, sizeof(jail), "/run/mystic/jail/%s", id);

    /* Nothing above stderr belongs to the app. Without this it inherits our
     * root-opened /dev/fb0 (read AND write the whole screen, forge a lock
     * screen) and the evdev keyboard (the unlock PIN in cleartext), plus the
     * listener and every other client's socket and shm buffer. Namespaces hide
     * paths; they do not take away file descriptors. errfd is the one exception:
     * it is how this child tells the desktop why it never became an app. */
    for (int fd = 3; fd < 1024; fd++) if (fd != errfd) close(fd);

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
        if (mount(exe, path, NULL, MS_BIND, NULL) < 0) {
            /* This is the failure that used to be invisible: the tile was there,
             * the click did nothing, and 127 scrolled past on a console nobody
             * reads. Report it and stop - exec'ing an unmounted /bin/app would
             * only add a second, less useful error. The report goes first so it
             * carries the errno the syscall actually set, not the one fprintf
             * left behind. */
            child_report(errfd, "cannot mount the payload", exe);
            fprintf(stderr, "wms: sandbox bind %.160s -> %s failed: %s\n",
                    exe, path, strerror(errno));
            _exit(123);
        }

        /* A payload is not always the whole app: pkg installs a tree, and a
         * package can ship a runtime beside its executable (libVLC's shared
         * objects, a browser's engine, data files). Binding only the executable
         * left such an app unable to see its own files, so the bundle directory
         * is mounted read-only at /app and MYSTIC_BUNDLE says where. Only that
         * one directory is added - not /root, not the rest of
         * /usr/share/mystic/apps. */
        {
            char bundle_dir[512];
            snprintf(bundle_dir, sizeof(bundle_dir), "%s", exe);
            char *last = strrchr(bundle_dir, '/');
            if (last) *last = 0;                       /* <root>/<id> */
            snprintf(path, sizeof(path), "%s/app", jail);
            mkdir(path, 0755);
            if (mount(bundle_dir, path, NULL, MS_BIND | MS_RDONLY, NULL) < 0)
                fprintf(stderr, "wms: sandbox bundle bind %.200s failed: %s\n",
                        bundle_dir, strerror(errno));
        }

        snprintf(path, sizeof(path), "%s/run/mystic/wm.sock", jail);
        fd = open(path, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) close(fd);
        mount(WM_SOCKET, path, NULL, MS_BIND, NULL);

        if (net) {
            /* getaddrinfo needs these two; ro, and only when asked for */
            snprintf(path, sizeof(path), "%s/etc", jail);   mkdir(path, 0755);
            /* the CA bundle lives two levels down; mkdir(2) makes one level */
            snprintf(path, sizeof(path), "%s/etc/ssl", jail);       mkdir(path, 0755);
            snprintf(path, sizeof(path), "%s/etc/ssl/certs", jail); mkdir(path, 0755);
            const char *cf[] = { "/etc/resolv.conf", "/etc/hosts",
                                 "/etc/ssl/certs/ca-bundle.crt" };
            for (int ci = 0; ci < (int)(sizeof(cf) / sizeof(cf[0])); ci++) {
                snprintf(path, sizeof(path), "%s%s", jail, cf[ci]);
                fd = open(path, O_CREAT | O_RDWR, 0644);
                if (fd >= 0) close(fd);
                if (mount(cf[ci], path, NULL, MS_BIND | MS_RDONLY, NULL) < 0)
                    fprintf(stderr, "wms: sandbox bind %s failed: %s\n", cf[ci], strerror(errno));
            }
        }

        /* Each app gets its OWN writable /root. Without this a jailed app
         * cannot save anything: /root is not in the jail and the jail root is
         * root-owned, so Paint could not write /root/Pictures and 2048 could
         * not keep its best score. Mounted as the app's /root, it is still
         * isolated from every other app and from the real one. */
        {
            char home[256];
            mkdir("/root/appdata", 0755);
            snprintf(home, sizeof(home), "/root/appdata/%s", id);
            mkdir(home, 0755);
            if (chown(home, 1000, 1000) != 0)
                fprintf(stderr, "wms: appdata chown %s failed: %s\n", home, strerror(errno));
            snprintf(path, sizeof(path), "%s/root", jail); mkdir(path, 0755);
            if (mount(home, path, NULL, MS_BIND, NULL) < 0)
                fprintf(stderr, "wms: appdata bind %s failed: %s\n", home, strerror(errno));
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
        {
            /* an app with no fonts is a blank window: say what the jail sees */
            DIR *fd2 = opendir(path);
            int nf = 0;
            if (fd2) {
                struct dirent *de2;
                while ((de2 = readdir(fd2))) if (de2->d_name[0] != '.') nf++;
                closedir(fd2);
            } else {
                fprintf(stderr, "wms: jail fonts opendir failed: %s\n", strerror(errno));
            }
            fprintf(stderr, "wms: jail fonts for %s: %d entries\n", id, nf);
        }

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

        /* The document the user asked to open. The desktop validated the
         * path; here it is bound READ-ONLY, on its own, into the jail at the
         * same path. So the app can read exactly the file it was asked to
         * show - not the directory it lives in, not anything beside it, and
         * it cannot write to it. */
        if (arg && arg[0] == '/' && !strstr(arg, "..")) {
            char dirpart[512];
            snprintf(dirpart, sizeof(dirpart), "%s", arg);
            char *slash2 = strrchr(dirpart, '/');
            if (slash2) {
                *slash2 = 0;                       /* parent path, no name */
                char jaildir[768];
                snprintf(jaildir, sizeof(jaildir), "%s%s", jail, dirpart);
                mkdirp_for(jail, dirpart);
                int dfd = open(jaildir, O_RDONLY | O_DIRECTORY);
                if (dfd >= 0) close(dfd);
                char filepath[768];
                snprintf(filepath, sizeof(filepath), "%s%s", jail, arg);
                int ffd = open(filepath, O_CREAT | O_RDONLY, 0444);
                if (ffd >= 0) close(ffd);
                if (mount(arg, filepath, NULL, MS_BIND | MS_RDONLY, NULL) < 0)
                    fprintf(stderr, "wms: cannot bind %.120s into the jail: %s\n",
                            arg, strerror(errno));
            }
        }

        /* Audio: libVLC's ALSA output opens /dev/snd/pcmC0D0p. Without this
         * bind a video plays silently and the player has to say so. */
        if (audio) {
            snprintf(path, sizeof(path), "%s/dev/snd", jail);
            mkdirp_for(jail, "/dev/snd");
            if (mount("/dev/snd", path, NULL, MS_BIND, NULL) < 0)
                fprintf(stderr, "wms: sandbox /dev/snd bind failed: %s\n", strerror(errno));
        }

        snprintf(path, sizeof(path), "%s/proc", jail);
        mkdirp_for(jail, "/proc");
        mount("/proc", path, NULL, MS_BIND, NULL);

        if (chroot(jail) == 0) {
            chdir("/");
            setgroups(0, NULL);
            if (setgid(SANDBOX_GID) != 0 || setuid(SANDBOX_UID) != 0) {
                child_report(errfd, "cannot drop to uid 1000 in", jail);
                fprintf(stderr, "wms: sandbox uid drop failed: %s; refusing to run\n",
                        strerror(errno));
                _exit(126);
            }
        } else {
            child_report(errfd, "cannot chroot into", jail);
            fprintf(stderr, "wms: chroot failed: %s; refusing to run\n", strerror(errno));
            _exit(124);
        }
    } else {
        /* fail closed: an app that cannot be isolated must not run at all,
         * rather than quietly running as root with the whole filesystem */
        child_report(errfd, "cannot unshare namespaces for", id);
        fprintf(stderr, "wms: unshare failed (%s); refusing to run %s\n",
                strerror(errno), id);
        _exit(125);
    }

    setenv("MYSTIC_SOCKET", "/run/mystic/wm.sock", 1);
    setenv("MYSTIC_APP_ID", id, 1);
    {
        /* the text scale is a desktop setting; a client app cannot read
         * /etc/mystic from inside its jail, so it travels in the env */
        char sc[32];
        snprintf(sc, sizeof(sc), "%.3f", (double)ui_scale_get());
        setenv("MYSTIC_UI_SCALE", sc, 1);
    }
    setenv("HOME", "/tmp", 1);
    setenv("TMPDIR", "/tmp", 1);
    /* the bundle's own tree, if the app needs more than its payload */
    setenv("MYSTIC_BUNDLE", "/app", 1);
    setenv("LD_LIBRARY_PATH", "/app/usr/lib:/app/lib", 1);
}

static int wms_launch_arg(const WmApp *app, WmClient *cl, int slot, const char *arg) {
    if (!app || slot < 0) return -1;

    /* The payload comes from the root the manifest was found under, never from
     * a hardcoded one: a pkg-installed bundle lives in /root/apps and exec'ing
     * <WM_APP_DIR>/<id> for it hit a path that does not exist, which is why
     * every installed tile was clickable and dead. The path is also proven to
     * be a regular executable before a fork is spent on it, so the failure is
     * one honest line instead of a pid that never opens a window. */
    char exe[512], why[128];
    if (wms_bundle_payload(app, exe, sizeof(exe), why, sizeof(why)) != 0) {
        if (exe[0]) launch_failed("%s: %s (%s)", app->id[0] ? app->id : "?", why, exe);
        else        launch_failed("%s: %s", app->id[0] ? app->id : "?", why);
        return -1;
    }

    int ep[2];
    if (pipe(ep) != 0) {
        launch_failed("%s: pipe: %s", app->id, strerror(errno));
        return -1;
    }
    /* The write end dies with the exec, so the parent can tell "running" from
     * "never ran" without polling /proc or waiting for a window. */
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);
    fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        launch_failed("%s: fork: %s", app->id, strerror(errno));
        close(ep[0]);
        close(ep[1]);
        return -1;
    }
    if (pid == 0) {
        close(ep[0]);
        sandbox_child(exe, app->id, app->net, app->audio, arg, ep[1]);
        if (arg && arg[0]) {
            /* an app that was asked to open a document gets the path as
             * argv[1]; the path was validated by the desktop, and the app
             * still cannot see anything outside its own jail */
            execl("/bin/app", "app", arg, (char *)NULL);
        } else {
            execl("/bin/app", "app", (char *)NULL);   /* inside the jail, or not at all */
        }
        /* exec failed: /bin/app is not an executable (a 0-byte placeholder, a
         * library, a wrong architecture). The kernel says which. */
        child_report(ep[1], "cannot exec", exe);
        fprintf(stderr, "wms: cannot exec the jailed %s: %s\n", app->id, strerror(errno));
        _exit(127);
    }

    close(ep[1]);
    /* Wait for one of two answers: the pipe closes (exec happened, the app is
     * running) or it carries a sentence (it did not). Bounded, because a mount
     * that hangs must not freeze the desktop's frame loop. */
    char msg[LAUNCH_MSG_MAX + 1];
    int got = 0, timed_out = 0;
    for (int i = 0; i < 2000 && got < (int)sizeof(msg) - 1; i++) {
        ssize_t r = read(ep[0], msg + got, sizeof(msg) - 1 - (size_t)got);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;                       /* closed: nothing went wrong */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (i == 1999) { timed_out = 1; break; }
        struct timespec ts = { 0, 1000000 };     /* 1 ms */
        nanosleep(&ts, NULL);
    }
    close(ep[0]);
    msg[got] = 0;

    if (got > 0) {
        for (int i = 0; i < got; i++) if (msg[i] == 10 || msg[i] == 13) msg[i] = ' ';
        launch_failed("%s: %s", app->id, msg);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        return -1;
    }
    if (timed_out)
        fprintf(stderr, "wms: %s has not exec'd after 2 s; leaving it alone\n", app->id);
    (void)cl;
    return (int)pid;
}
/* Two entry points, one implementation: the launcher and the store use the
 * plain one; Files passes a document path so the app can open it. */
int wms_launch(const WmApp *app, WmClient *c, int slot) {
    return wms_launch_arg(app, c, slot, NULL);
}

int wms_launch_path(const WmApp *app, WmClient *c, int slot, const char *path) {
    return wms_launch_arg(app, c, slot, path);
}

