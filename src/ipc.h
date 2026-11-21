/* ============================================================
 *  Mystic OS - ipc.h
 *  The display protocol: how a separate process puts pixels on
 *  our screen and receives input.
 *
 *  This is deliberately small and text-based on the control
 *  channel, with the pixels in shared memory. You can watch it
 *  with `nc` and it never needs a specification document longer
 *  than this header.
 *
 *  Wire format, one connection per app, line terminated by \n:
 *
 *    app  -> wm :  HELLO <id> <w> <h> <title...>
 *    wm   -> app:  OK <shm-path> <stride> <maxw> <maxh>
 *    wm   -> app:  ERR <reason>
 *
 *    app  -> wm :  FRAME                     (shared memory is ready)
 *    app  -> wm :  TITLE <text>
 *    app  -> wm :  CLOSE
 *
 *    wm   -> app:  KEY <keycode> <mods> <press>
 *    wm   -> app:  TEXT <utf8>
 *    wm   -> app:  MOUSE <x> <y> <buttons>
 *    wm   -> app:  RESIZE <w> <h>
 *    wm   -> app:  CLOSE
 *
 *  mods is a bitmask: 1 shift, 2 ctrl, 4 alt.
 *  buttons is a bitmask: 1 left, 2 right, 4 middle.
 *
 *  The shared memory is maxw * maxh pixels, 32-bit 0xAARRGGBB, stride pixels
 *  per row. Both numbers come back in OK because the server allocates the
 *  whole screen (a window can grow, up to that bound) and <w> <h> are only the
 *  size the app wants to open at: the app must map stride * maxh * 4 bytes and
 *  never draw outside it. <shm-path> is the path as the APP sees it.
 *
 *  What a launched app finds inside its sandbox: its payload running as
 *  /bin/app, its own bundle mounted read-only at /app (MYSTIC_BUNDLE), its
 *  own appdata as /root, its own size-limited tmpfs as /tmp and as /dev/shm,
 *  the window socket, read-only fonts, /dev/null, /dev/urandom and a procfs.
 *  MYSTIC_APP_ID and MYSTIC_UI_SCALE carry the bundle id and the desktop's
 *  text scale.
 *
 *  Two things about that sandbox belong to the protocol and so live here:
 *
 *   - The app runs as its own uid, allocated per bundle from a small pool
 *     (2000..2000+WM_MAX_CLIENTS-1). The kernel tells the server which uid a
 *     connection really belongs to (SO_PEERCRED), which is why the buffer the
 *     server hands out in OK can be opened only by that app. See
 *     docs/SECURITY.md.
 *   - /dev/shm inside the jail is a per-app tmpfs, so the <shm-path> in OK is
 *     the path as the APP sees it (/dev/shm/...), not a host path.
 * ============================================================ */
#ifndef MYSTIC_IPC_H
#define MYSTIC_IPC_H

#include "mystic.h"

#define WM_SOCKET     "/run/mystic/wm.sock"
#define WM_MAX_CLIENTS 8
#define WM_MAX_APPS    32

/* The two places a bundle can live. WM_APP_DIR is the image's own copy: read
 * only in spirit, replaced only by booting a new image. WM_APP_DIR_RW is where
 * pkg unpacks what the user installs, and /root is the only place that survives
 * a reboot. A launcher tile therefore has to remember WHICH of the two it came
 * from - exec'ing everything from WM_APP_DIR is what made every installed
 * bundle a listed, clickable, permanently dead tile. */
#define WM_APP_DIR     "/usr/share/mystic/apps"
#define WM_APP_DIR_RW  "/root/apps"

/* Request files in the run dir. pkg writes one after it changes /root/apps;
 * the desktop polls for it once a second, rescans, and removes it. Nothing else
 * is needed to see a new bundle without a reboot. */
#define WM_RUN_DIR     "/run/mystic"
#define WM_RESCAN_LEAF "rescan.request"
#define WM_LAUNCH_LEAF "launch.error"
#define WM_RESCAN_REQ  WM_RUN_DIR "/" WM_RESCAN_LEAF
#define WM_LAUNCH_ERR  WM_RUN_DIR "/" WM_LAUNCH_LEAF

/* one connected app */
typedef struct {
    int   used;
    int   fd;
    char  rx[4096];          /* partial line buffer */
    int   rxlen;

    int   w, h, stride;
    int   maxw, maxh;      /* the buffer is this big; w/h is the window */
    char  shm_path[96];    /* the path the APP opens: /dev/shm/... */
    char  shm_host[512];   /* the same file as this server reaches it */
    int   shm_fd;
    u32  *px;

    char  id[32];
    char  jail_id[32];     /* the bundle this connection was launched as; for a
                            * jailed client this comes from the launch, not from
                            * HELLO, so a lying HELLO cannot aim the buffer at
                            * another app's /dev/shm */
    int   uid;             /* SO_PEERCRED: the uid the app really runs as */
    int   jailed;          /* 1 = uid is one of the sandbox uids (and the app
                            * really is inside a jail) */
    long  pid;             /* SO_PEERCRED: the app's pid, for the uid pool */
    char  title[80];
    int   win;               /* index into wins[], or -1 */
    int   want_frame;
} WmClient;

/* what an app bundle looks like on disk:  <dir>/manifest , <dir>/bin */
typedef struct {
    char id[32];
    char name[64];
    char desc[160];
    char exe[128];
    char root[64];     /* the root this manifest was found under: WM_APP_DIR or
                        * WM_APP_DIR_RW. The payload is <root>/<id>/<exe>, and
                        * wms_launch() execs exactly that path. */
    int  from_image;   /* 1 = shipped bundle, 0 = installed under /root/apps */
    int  audio;        /* manifest says 'audio = yes': bind /dev/snd */
    int  net;          /* manifest says 'network = host': keep the host netns,
                        * and give the jail a resolv.conf so lookups work */
    int  procs;        /* manifest says 'procs = yes': the app reads every
                        * process, so it gets the host /proc read-only and NO
                        * pid namespace. Every other app gets its own pid
                        * namespace and a fresh procfs. The trade is named,
                        * one manifest line, and documented in
                        * docs/SECURITY.md; sysmon is the only app that asks. */
    int  no_chroot;    /* manifest says 'chroot = no': keep the mount, pid,
                        * net and ipc namespaces, the uid drop and the private
                        * /dev/shm and /tmp, but do not chroot. A third-party
                        * runtime that insists on the real /lib, /etc and
                        * /usr/share (a browser engine does) cannot start in a
                        * chroot holding only its own bundle. Stated in the
                        * manifest, and recorded in docs/SECURITY.md. */
} WmApp;

/* Tell the server how big the screen is, so a client buffer can be allocated
 * at full size: a window that is maximised needs somewhere to grow into. */
void wms_set_screen(int w, int h);
int  wms_start(void);                              /* listen on WM_SOCKET */
void wms_poll(WmClient *c, int n);                  /* accept + read */
void wms_notify_key(WmClient *c, int key, int mods, int press);
void wms_notify_text(WmClient *c, const char *utf8);
void wms_notify_mouse(WmClient *c, int x, int y, int buttons);
void wms_notify_wheel(WmClient *c, int notches);
void wms_notify_resize(WmClient *c, int w, int h);
void wms_notify_close(WmClient *c);
void wms_shutdown_client(WmClient *c);

/* Read every bundle from both roots into `out` and return the count. Safe to
 * call again at any time: it re-reads the disk and overwrites `out`. */
int  wms_scan_apps(WmApp *out, int max);

/* Rescan only when something asked for it (pkg, after an install or a removal,
 * writes WM_RESCAN_REQ). Returns 1 when `out`/`count` were rewritten - redraw the
 * launcher - and 0 when no request was waiting (one stat() per frame in that
 * case). The request is removed before the scan, so the next frame is quiet. */
int  wms_apps_poll_rescan(WmApp *out, int *count, int max);

/* The file the launcher must exec for this bundle: <root>/<id>/<exe>, proven
 * with stat() to be a regular executable. Returns 0, or -1 with the kernel's own
 * reason in `why` (strerror(errno) from the syscall that failed). */
int  wms_bundle_payload(const WmApp *app, char *out, size_t cap,
                        char *why, size_t whyn);

/* The last launch that failed, and why, as one line: "<id>: <reason> (<path>)".
 * Returns 1 and copies it (clearing it) if there was one, 0 otherwise. The same
 * text is written to WM_LAUNCH_ERR, so a launcher that does not poll this still
 * leaves the reason on disk instead of a pid that never produced a window. */
int  wms_take_launch_error(char *out, int cap);

int  wms_launch(const WmApp *app, WmClient *c, int slot); /* fork/exec */
/* Same, but the app receives `path` as argv[1]: this is how Files opens
 * a document in the app the type registry chose for it. */
int  wms_launch_path(const WmApp *app, WmClient *c, int slot, const char *path);

#endif /* MYSTIC_IPC_H */
