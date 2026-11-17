/* ============================================================
 *  Mystic OS - apps (external) / clock
 *  A real separate process: it connects to the window server,
 *  gets shared memory, draws a live clock into it, and reacts to
 *  keys. Nothing here is compiled into the desktop.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void draw_clock(MClient *c, Font *fh, Font *fd, int ticks) {
    Surface s = { c->px, c->w, c->h, c->stride };

    /* background: a slow vertical gradient so it is obviously alive */
    u32 top = RGB(0x0D, 0x14, 0x22);
    u32 bottom = RGB(0x18, 0x24, 0x3A);
    gfx_gradient(&s, 0, 0, c->w, c->h, top, bottom);

    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);

    char big[16];
    snprintf(big, sizeof(big), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    float bw = font_width(fh, big);
    font_draw(&s, fh, (c->w - bw) / 2.0f, c->h * 0.22f, big, RGB(0xE8, 0xEC, 0xF6));

    char date[64];
    snprintf(date, sizeof(date), "%04d-%02d-%02d  UTC", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    float dw = font_width(fd, date);
    font_draw(&s, fd, (c->w - dw) / 2.0f, c->h * 0.62f, date, RGB(0x7A, 0xA2, 0xF7));

    /* a bar that moves every second: proof this is redrawing live */
    float frac = tmv.tm_sec / 60.0f;
    int bar_w = (int)((c->w - 40) * frac);
    gfx_rrect_aa(&s, 20, (float)c->h - 34, (float)c->w - 40, 8, 4, RGB(0x22, 0x2C, 0x40), 0, 0);
    if (bar_w > 2)
        gfx_rrect_aa(&s, 20, (float)c->h - 34, (float)bar_w, 8, 4, RGB(0x4F, 0xD1, 0xC5), 0, 0);

    char hint[64];
    snprintf(hint, sizeof(hint), "process %d   frames %d   q quits", (int)getpid(), ticks);
    font_draw(&s, fd, 20, 12, hint, RGB(0x8A, 0x93, 0xA6));
}

int main(void) {
    MClient c;
    if (mc_open(&c, "clock", "Clock", 380, 240) < 0) {
        fprintf(stderr, "clock: cannot connect to the window server\n");
        return 1;
    }

    font_init("/usr/share/fonts");
    Font *fh = font_get(FONT_SEMIBOLD, 38);
    Font *fd = font_get(FONT_REGULAR, 11);
    if (!fh || !fd) {
        fprintf(stderr, "clock: no fonts\n");
        return 1;
    }

    int ticks = 0;
    for (;;) {
        draw_clock(&c, fh, fd, ticks++);
        mc_present(&c);

        /* wait up to a second for an event, then redraw anyway */
        MEvent ev;
        for (int i = 0; i < 4; i++) {
            if (!mc_poll(&c, &ev)) { usleep(250 * 1000); continue; }
            if (ev.type == MC_CLOSE) { mc_close(&c); return 0; }
            if (ev.type == MC_KEY && ev.press && ev.key == 16) {   /* 'q' */
                mc_close(&c);
                return 0;
            }
        }
    }
}
