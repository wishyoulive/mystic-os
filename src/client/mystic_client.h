/* ============================================================
 *  Mystic OS - client library (SDK)
 *  An ordinary program links this, connects to the window server
 *  and gets a shared pixel buffer plus an input queue.
 * ============================================================ */
#ifndef MYSTIC_CLIENT_H
#define MYSTIC_CLIENT_H

#include <stdint.h>

#define MC_KEY    1
#define MC_TEXT   2
#define MC_MOUSE  3
#define MC_RESIZE 4
#define MC_CLOSE  5
#define MC_WHEEL  6   /* x = notches, positive is up */

typedef struct {
    int  type;
    int  key, mods, press;
    char text[32];
    int  x, y, buttons, wheel;
    int  w, h;
} MEvent;

typedef struct {
    int   fd;
    int   w, h, stride;
    int   maxw, maxh;   /* the mapping can grow to this */
    uint32_t *px;
    char  shm[128];
    char  rx[2048];
    int   rxlen;
} MClient;

/* Returns 0 on success. Connects to $MYSTIC_SOCKET or the default. */
int  mc_open(MClient *c, const char *id, const char *title, int w, int h);
void mc_present(MClient *c);                 /* tell the WM the frame is ready */
int  mc_poll(MClient *c, MEvent *ev);        /* 1 if an event was read */
void mc_close(MClient *c);

/* a couple of helpers so demo apps do not each rewrite them */
void mc_fill(MClient *c, uint32_t color);
void mc_rect(MClient *c, int x, int y, int w, int h, uint32_t color);

#endif
