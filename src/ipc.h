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
 *    app  -> wm :  HELLO <id> <title...> <w> <h>
 *    wm   -> app:  OK <shm-path> <stride>
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
 *  The shared memory holds w*h pixels, 32-bit 0xAARRGGBB, stride
 *  pixels per row (stride is given in OK because we round it up).
 * ============================================================ */
#ifndef MYSTIC_IPC_H
#define MYSTIC_IPC_H

#include "mystic.h"

#define WM_SOCKET     "/run/mystic/wm.sock"
#define WM_MAX_CLIENTS 8
#define WM_MAX_APPS    32
#define WM_APP_DIR     "/usr/share/mystic/apps"

/* one connected app */
typedef struct {
    int   used;
    int   fd;
    char  rx[4096];          /* partial line buffer */
    int   rxlen;

    int   w, h, stride;
    char  shm_path[96];
    int   shm_fd;
    u32  *px;

    char  id[32];
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
    int  net;          /* manifest says 'network = host': keep the host netns,
                        * and give the jail a resolv.conf so lookups work */
} WmApp;

int  wms_start(void);                              /* listen on WM_SOCKET */
void wms_poll(WmClient *c, int n);                  /* accept + read */
void wms_notify_key(WmClient *c, int key, int mods, int press);
void wms_notify_text(WmClient *c, const char *utf8);
void wms_notify_mouse(WmClient *c, int x, int y, int buttons);
void wms_notify_resize(WmClient *c, int w, int h);
void wms_notify_close(WmClient *c);
void wms_shutdown_client(WmClient *c);

int  wms_scan_apps(WmApp *out, int max);            /* read WM_APP_DIR */
int  wms_launch(const WmApp *app, WmClient *c, int slot); /* fork/exec */

#endif /* MYSTIC_IPC_H */
