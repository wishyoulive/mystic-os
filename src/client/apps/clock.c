/* ============================================================
 *  Mystic OS - apps (external) / clock
 *
 *  A clock, a countdown timer, a stopwatch with laps, and a
 *  once-a-day alarm in one process. It is a normal program: it
 *  connects to the window server, maps a shared pixel buffer and
 *  draws into it. Nothing here is compiled into the desktop.
 *
 *      +--------------------------------------------------+
 *      | [Clock] [Timer] [Stopwatch] [Alarm]              |  tabs
 *      |                     .-''''-.                     |
 *      |        14:32:07   .'   12   '.                   |
 *      |      2026-11-17   |  9  o  3  |                  |
 *      |           UTC     '.   6    .'                    |
 *      |                     '-....-'                     |
 *      | 1-4: tabs   q: quit                              |  hint
 *      +--------------------------------------------------+
 *
 *  Three rules hold this file together.
 *
 *  1. The state below is the whole program. No rectangle, no font
 *     and no pixel size is stored in it, so an MC_RESIZE only
 *     changes the numbers used to place things - every count keeps
 *     running across a resize, a maximise and a restore.
 *
 *  2. The countdown and the stopwatch are computed from
 *     CLOCK_MONOTONIC as (duration - used - (now - started)), never
 *     by subtracting from a counter once per frame. A slow frame, a
 *     missed wakeup or a resize cannot make either of them drift;
 *     they are a function of the clock, not of how often we looked
 *     at it. The alarm is the one thing that has to follow the wall
 *     clock, and it fires when the requested time of day is crossed
 *     - on a 24-hour circle, so arming at 23:50 for 07:00 works.
 *
 *  3. Every band is laid out on a grid derived from the fonts that
 *     are actually loaded (one cell = one small-text row), and every
 *     string is measured before it is drawn, so the layout is the
 *     same lattice at text scale 0.8 and at 1.6. A string that does
 *     not fit its box gets a smaller face; the box itself never
 *     moves into a neighbour.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIN_W 420
#define WIN_H 380

#define TAB_COUNT 4
#define LAP_MAX   64

/* How long "time up" and the alarm banner flash for. */
#define FLASH_MS 2600L

/* pi*2, written out: <math.h> only defines M_PI when a feature macro is
 * requested and the app is compiled without any. */
#define TAU 6.28318530717958647692f

/* evdev key codes, forwarded verbatim by the window server. */
#define KEY_TAB   15
#define KEY_Q     16
#define KEY_1      2
#define KEY_2      3
#define KEY_3      4
#define KEY_4      5
#define KEY_R     19
#define KEY_L     38
#define KEY_SPACE 57
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_UP    103
#define KEY_DOWN  108

/* ------------------------------------------------------------
 *  Pure arithmetic. No clock, no pixels, no allocation.
 *
 *  Everything down to "end of the pure arithmetic" is plain
 *  integer code over values the caller supplies, which is what
 *  lets a host harness advance a fake monotonic clock and check
 *  the countdown without a screen:
 *
 *      zig cc -DCLOCK_HOST_TEST -I src .work/clockgame/clock_harness.c
 *
 *  The harness includes this file; the guard keeps main() and the
 *  whole drawing half out of it.
 * ------------------------------------------------------------ */

static long clamp_l(long v, long lo, long hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Milliseconds between two CLOCK_MONOTONIC readings, in ns. A zero start
 * ("never started") and a clock that appears to go backwards both mean
 * nothing has elapsed. The cap keeps the long arithmetic below far away
 * from an overflow even if the process sat in a debugger for a month. */
static long span_ms(u64 from_ns, u64 to_ns) {
    if (!from_ns || to_ns <= from_ns) return 0;
    u64 d = (to_ns - from_ns) / 1000000ull;
    if (d > 1000000000ull) d = 1000000000ull;      /* ~11.5 days */
    return (long)d;
}

/* What is left of a countdown. The state is the duration, the time
 * already spent, and when the current run started - there is no
 * "remaining" field that gets decremented, so nothing can accumulate a
 * rounding error, however irregular the redraws are. */
static long timer_left_ms(long dur_ms, long used_ms, u64 start_ns,
                          int running, u64 now_ns) {
    long left = dur_ms - used_ms;
    if (running) left -= span_ms(start_ns, now_ns);
    return left < 0 ? 0 : left;
}

/* Stopwatch: banked time plus whatever the current run has added. */
static long sw_elapsed_ms(long acc_ms, u64 start_ns, int running, u64 now_ns) {
    long v = acc_ms;
    if (running) v += span_ms(start_ns, now_ns);
    return v < 0 ? 0 : v;
}

/* Seconds since local midnight. */
static long tod_sec(const struct tm *tm) {
    return (long)tm->tm_hour * 3600L + (long)tm->tm_min * 60L + (long)tm->tm_sec;
}

/* Did the wall clock pass target_sec between two readings of the time of
 * day? All three are seconds since midnight, so the question is asked on a
 * 24-hour circle: an alarm armed at 23:50 for 07:00 must ring at 07:00
 * rather than be skipped because the numbers wrapped. prev_sec < 0 means
 * "first reading", where only an exact hit counts (we cannot claim to have
 * seen a crossing we were not there for). */
static int tod_passed(long prev_sec, long now_sec, long target_sec) {
    if (prev_sec < 0) return now_sec == target_sec;
    long step = (now_sec - prev_sec + 86400L) % 86400L;   /* seconds advanced */
    long dist = (target_sec - prev_sec + 86400L) % 86400L;
    if (step == 0) return dist == 0;
    return dist > 0 && dist <= step;
}

/* MM:SS, rounded down: a 5:00 countdown reads 00:01 for its last whole
 * second and only reads 00:00 when it has actually expired. */
static void fmt_mmss(char *out, size_t n, long ms) {
    long s = ms / 1000;
    if (s < 0) s = 0;
    snprintf(out, n, "%02ld:%02ld", s / 60, s % 60);
}

/* HH:MM:SS. */
static void fmt_hms(char *out, size_t n, long sec) {
    if (sec < 0) sec = 0;
    snprintf(out, n, "%02ld:%02ld:%02ld", sec / 3600, (sec / 60) % 60, sec % 60);
}

/* Stopwatch: MM:SS.cc while it is under an hour, then H:MM:SS. Centiseconds
 * are the finest thing a 30 Hz redraw can honestly show. */
static void fmt_sw(char *out, size_t n, long ms) {
    long cs = ms / 10;
    if (cs < 0) cs = 0;
    long h = cs / 360000, m = (cs / 6000) % 60, s = (cs / 100) % 60;
    if (h > 0) snprintf(out, n, "%ld:%02ld:%02ld", h, m, s);
    else       snprintf(out, n, "%02ld:%02ld.%02ld", m, s, cs % 100);
}

/* "HH:MM" for a time of day. */
static void fmt_tod(char *out, size_t n, int h, int m) {
    snprintf(out, n, "%02d:%02d", clamp_i(h, 0, 23), clamp_i(m, 0, 59));
}

/* How long until the alarm: the real difference, never a guess. */
static void fmt_until(char *out, size_t n, long sec) {
    if (sec < 0) sec = 0;
    long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    if (h > 0)      snprintf(out, n, "in %ldh %02ldm", h, m);
    else if (m > 0) snprintf(out, n, "in %ldm %02lds", m, s);
    else            snprintf(out, n, "in %lds", s);
}

/* ---- end of the pure arithmetic ------------------------------------ */

#ifndef CLOCK_HOST_TEST

/* ------------------------------------------------------------ colour */

#define COL_BG_TOP RGB(0x0D, 0x13, 0x1F)
#define COL_BG_BOT RGB(0x15, 0x1E, 0x2D)
#define COL_TEXT   RGB(0xE8, 0xEC, 0xF6)
#define COL_DIM    RGB(0x8A, 0x93, 0xA6)
#define COL_FAINT  RGB(0x5E, 0x6B, 0x82)
#define COL_ACCENT RGB(0x4F, 0xD1, 0xC5)
#define COL_BLUE   RGB(0x7A, 0xA2, 0xF7)
#define COL_WARN   RGB(0xF5, 0xC5, 0x42)
#define COL_HOT    RGB(0xFF, 0x6B, 0x5B)
#define COL_HOT_TX RGB(0xFF, 0xC9, 0xB8)
#define COL_PANEL  RGB(0x16, 0x1E, 0x2B)
#define COL_SUNK   RGB(0x10, 0x17, 0x23)
#define COL_EDGE   RGB(0x27, 0x32, 0x45)
#define COL_EDGE2  RGB(0x38, 0x46, 0x5E)

/* ------------------------------------------------------------ state */

/* The four tabs. The index is stored, not a name or a position. */
enum { TAB_CLOCK, TAB_TIMER, TAB_STOPWATCH, TAB_ALARM };

typedef struct {
    int  tab;                 /* which tab is showing */
    int  mx, my;              /* pointer, for hover only */

    /* ---- timer */
    long t_dur_ms;            /* the duration the user set            */
    long t_used_ms;           /* time spent by previous runs          */
    u64  t_start_ns;          /* CLOCK_MONOTONIC at the start of this run */
    int  t_run;
    int  t_field;             /* 0 minutes, 1 seconds                 */
    int  t_done;              /* reached zero and has not been reset  */
    u64  t_flash_until;

    /* ---- stopwatch */
    long w_acc_ms;            /* banked time                          */
    u64  w_start_ns;
    int  w_run;
    int  w_laps;
    long w_lap_split[LAP_MAX];/* each lap on its own                   */
    long w_lap_total[LAP_MAX];/* the running total when it was taken   */

    /* ---- alarm */
    int  a_h, a_m;
    int  a_field;             /* 0 hours, 1 minutes                   */
    int  a_armed;
    int  a_rings;             /* it fired; the banner is up           */
    long a_prev_sec;          /* last seconds-of-day we saw, -1 = none */
    u64  a_flash_until;
} ClockState;

/* ------------------------------------------------------------ geometry */

typedef struct { int x, y, w, h; } Rect;

static int rect_has(Rect r) { return r.w > 0 && r.h > 0; }

static int rect_hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* The grid: one cell is one row of small text plus its leading, and it is
 * measured from the font that is really loaded (which already has the
 * desktop's text scale applied). Every band below is a whole number of
 * cells, so the layout is the same lattice at 0.8 and at 1.6. */
typedef struct {
    Font *small;     /* hints, dates, lap rows */
    int   cell_h;
    int   cell_w;
} Chrome;

typedef struct {
    Rect tabs, content, hint;
    int  tabw;       /* one tab cell */
    int  gap;
} Layout;

/* Everything a tab's drawing and its hit test need, computed once from the
 * content rectangle so the two can never disagree. */
typedef struct {
    Rect field[2];       /* the two setter boxes */
    Rect minus[2], plus[2];
    Rect big;            /* the counter / the digital time */
    Rect line2;          /* date, or the alarm status line */
    Rect btn[3];
    int  btn_n;
    Rect list;           /* lap list */
    int  rows;           /* lap rows that fit */
} TabUI;

static const char *TAB_NAME[TAB_COUNT] = { "Clock", "Timer", "Stopwatch", "Alarm" };

static void chrome_init(Chrome *C) {
    C->small = font_get(FONT_REGULAR, 10);
    C->cell_h = C->small ? (int)(font_height(C->small) + 3.0f) : 14;
    if (C->cell_h < 11) C->cell_h = 11;
    C->cell_w = C->small ? (int)(font_width(C->small, "0") + 0.5f) : 6;
    if (C->cell_w < 4) C->cell_w = 4;
}

/* The largest of these nominal sizes whose measured text fits the box.
 * font_get() applies the text scale, so the table is a set of candidates in
 * nominal points and the chosen one is whatever the scale made of them.
 * Iterating a fixed table (instead of computing a size from the box) keeps
 * the number of fonts the app ever asks for bounded. NULL means even the
 * smallest spills, and the caller then does not draw rather than overlap. */
static Font *fit_font_box(int face, const char *txt, float max_w, float max_h) {
    static const int sizes[9] = { 46, 40, 34, 28, 22, 17, 13, 10, 8 };
    if (!txt || !*txt || max_w < 2.0f || max_h < 2.0f) return NULL;
    for (int i = 0; i < 9; i++) {
        Font *f = font_get(face, (float)sizes[i]);
        if (!f) continue;
        if (font_width(f, txt) <= max_w && font_height(f) <= max_h) return f;
    }
    return NULL;
}

/* Draw a string centred in a box, at the largest size that fits. */
static void text_box(Surface *s, int face, Rect r, const char *txt, u32 col) {
    Font *f = fit_font_box(face, txt, (float)r.w, (float)r.h);
    if (!f) return;
    float w = font_width(f, txt);
    float x = (float)r.x + ((float)r.w - w) * 0.5f;
    float y = (float)r.y + ((float)r.h - font_height(f)) * 0.5f;
    font_draw(s, f, x, y, txt, col);
}

/* A specific font, centred / left / right in a box. */
static void font_box(Surface *s, Font *f, Rect r, const char *txt, u32 col, int align) {
    if (!f || !txt) return;
    float w = font_width(f, txt);
    float x = (float)r.x;
    if (align == 1) x = (float)r.x + ((float)r.w - w) * 0.5f;
    if (align == 2) x = (float)r.x + (float)r.w - w;
    float y = (float)r.y + ((float)r.h - font_height(f)) * 0.5f;
    font_draw(s, f, x, y, txt, col);
}

/* Tab labels are pills: the same cell width each, with the label sized to
 * the longest one so "Stopwatch" never pushes into "Alarm" at any scale. */
static Font *choose_tab_font(int tabw) {
    static const int sizes[7] = { 13, 12, 11, 10, 9, 8, 7 };
    for (int i = 0; i < 7; i++) {
        Font *f = font_get(FONT_SEMIBOLD, (float)sizes[i]);
        if (!f) continue;
        float need = 0.0f;
        for (int k = 0; k < TAB_COUNT; k++) {
            float w = font_width(f, TAB_NAME[k]);
            if (w > need) need = w;
        }
        if (need <= (float)tabw - 8.0f) return f;
    }
    return font_get(FONT_SEMIBOLD, 7.0f);
}

static void layout_window(int vw, int vh, const Chrome *C, Layout *L) {
    int pad = C->cell_h / 2;
    if (pad < 4) pad = 4;
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;

    L->gap = gap;
    L->tabs.x = pad;
    L->tabs.y = pad;
    L->tabs.w = vw - 2 * pad;
    L->tabs.h = C->cell_h + C->cell_h / 2;
    if (L->tabs.w < 4) L->tabs.w = 4;
    if (L->tabs.h < 4) L->tabs.h = 4;

    L->tabw = (L->tabs.w - (TAB_COUNT - 1) * gap) / TAB_COUNT;
    if (L->tabw < 8) L->tabw = 8;

    L->hint.x = pad;
    L->hint.y = vh - pad - C->cell_h;
    L->hint.w = vw - 2 * pad;
    L->hint.h = C->cell_h;
    if (L->hint.w < 0) L->hint.w = 0;

    L->content.x = pad;
    L->content.y = L->tabs.y + L->tabs.h + gap;
    L->content.w = vw - 2 * pad;
    L->content.h = L->hint.y - gap - L->content.y;
    if (L->content.w < 0) L->content.w = 0;
    if (L->content.h < 0) L->content.h = 0;
}

static Rect tab_rect(const Layout *L, int i) {
    Rect r;
    r.x = L->tabs.x + i * (L->tabw + L->gap);
    r.y = L->tabs.y;
    r.w = L->tabw;
    r.h = L->tabs.h;
    return r;
}

/* Two setter boxes with a [-] and a [+] under each. Returns the height
 * used, so the caller can give the rest to the counter. */
static int geo_fields(Rect c, const Chrome *C, TabUI *u) {
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;
    int bw = (c.w - gap) / 2;
    if (bw < 8) bw = 8;
    int fh = 2 * C->cell_h;
    if (fh > c.h / 2) fh = c.h / 2;
    if (fh < C->cell_h) fh = c.h < C->cell_h ? c.h : C->cell_h;

    for (int i = 0; i < 2; i++) {
        u->field[i].x = c.x + i * (bw + gap);
        u->field[i].y = c.y;
        u->field[i].w = bw;
        u->field[i].h = fh;

        int hw = (bw - gap) / 2;
        if (hw < 6) hw = 6;
        u->minus[i].x = u->field[i].x;
        u->minus[i].y = c.y + fh + gap;
        u->minus[i].w = hw;
        u->minus[i].h = C->cell_h;
        u->plus[i].x = u->field[i].x + hw + gap;
        u->plus[i].y = u->minus[i].y;
        u->plus[i].w = hw;
        u->plus[i].h = C->cell_h;
    }
    return fh + gap + C->cell_h;
}

static void geo_timer(Rect c, const Chrome *C, TabUI *u) {
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;
    int used = geo_fields(c, C, u);

    int bh = C->cell_h + C->cell_h / 2;
    int bw = (c.w - gap) / 2;
    if (bw < 8) bw = 8;
    u->btn_n = 2;
    for (int i = 0; i < 2; i++) {
        u->btn[i].x = c.x + i * (bw + gap);
        u->btn[i].y = c.y + c.h - bh;
        u->btn[i].w = bw;
        u->btn[i].h = bh;
    }

    int by = c.y + used + gap;
    int bh2 = (c.y + c.h - bh - gap) - by;
    if (bh2 < 0) bh2 = 0;
    u->big.x = c.x;
    u->big.y = by;
    u->big.w = c.w;
    u->big.h = bh2;
}

static void geo_stopwatch(Rect c, const Chrome *C, TabUI *u) {
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;

    int bh = C->cell_h + C->cell_h / 2;
    int bw = (c.w - 2 * gap) / 3;
    if (bw < 8) bw = 8;
    u->btn_n = 3;
    for (int i = 0; i < 3; i++) {
        u->btn[i].x = c.x + i * (bw + gap);
        u->btn[i].y = c.y + c.h - bh;
        u->btn[i].w = bw;
        u->btn[i].h = bh;
    }

    int cy = c.y;
    int ch = 3 * C->cell_h;
    if (ch > c.h) ch = c.h;
    u->big.x = c.x;
    u->big.y = cy;
    u->big.w = c.w;
    u->big.h = ch;

    int ly = cy + ch + gap;
    int lh = (c.y + c.h - bh - gap) - ly;
    if (lh < 0) lh = 0;
    u->list.x = c.x;
    u->list.y = ly;
    u->list.w = c.w;
    u->list.h = lh;
    u->rows = lh / C->cell_h;
    if (u->rows > LAP_MAX) u->rows = LAP_MAX;
}

static void geo_alarm(Rect c, const Chrome *C, TabUI *u) {
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;
    int used = geo_fields(c, C, u);

    int bh = C->cell_h + C->cell_h / 2;
    int bw = (c.w - gap) / 2;
    if (bw < 8) bw = 8;
    u->btn_n = 2;
    for (int i = 0; i < 2; i++) {
        u->btn[i].x = c.x + i * (bw + gap);
        u->btn[i].y = c.y + used + gap;
        u->btn[i].w = bw;
        u->btn[i].h = bh;
    }

    int sy = u->btn[0].y + bh + gap;
    int sh = (c.y + c.h) - sy;
    if (sh < 0) sh = 0;
    u->big.x = c.x;              /* the status text lives here */
    u->big.y = sy;
    u->big.w = c.w;
    u->big.h = sh;
    u->line2 = u->big;
}

static void geo_clock(Rect c, const Chrome *C, TabUI *u) {
    int gap = C->cell_h / 3;
    if (gap < 3) gap = 3;

    int dateh = C->cell_h;
    int digith = 2 * C->cell_h + gap;
    int side = c.w < c.h - dateh - digith - 2 * gap
             ? c.w : c.h - dateh - digith - 2 * gap;
    if (side > 300) side = 300;

    u->big.x = c.x;
    u->big.y = c.y + c.h - dateh - digith;
    u->big.w = c.w;
    u->big.h = digith;
    u->line2.x = c.x;
    u->line2.y = c.y + c.h - dateh;
    u->line2.w = c.w;
    u->line2.h = dateh;

    if (side >= 56) {
        u->field[0].x = c.x + (c.w - side) / 2;
        u->field[0].y = c.y;
        u->field[0].w = side;
        u->field[0].h = side;
    } else {
        u->field[0].x = c.x;
        u->field[0].y = c.y;
        u->field[0].w = 0;
        u->field[0].h = 0;
    }
}

static void tab_geometry(int tab, Rect c, const Chrome *C, TabUI *u) {
    memset(u, 0, sizeof(*u));
    if (!rect_has(c)) return;
    switch (tab) {
    case TAB_CLOCK:     geo_clock(c, C, u); break;
    case TAB_TIMER:     geo_timer(c, C, u); break;
    case TAB_STOPWATCH: geo_stopwatch(c, C, u); break;
    default:            geo_alarm(c, C, u); break;
    }
}

/* ------------------------------------------------------------ actions */

static void timer_toggle(ClockState *K, u64 now) {
    if (K->t_run) {
        /* pause: bank exactly what the monotonic clock says has gone */
        K->t_used_ms = K->t_dur_ms -
                       timer_left_ms(K->t_dur_ms, K->t_used_ms, K->t_start_ns, 1, now);
        K->t_start_ns = 0;
        K->t_run = 0;
    } else {
        if (K->t_dur_ms <= 0) return;
        if (K->t_done || K->t_used_ms >= K->t_dur_ms) K->t_used_ms = 0;
        K->t_done = 0;
        K->t_start_ns = now;
        K->t_run = 1;
    }
}

static void timer_reset(ClockState *K) {
    K->t_used_ms = 0;
    K->t_start_ns = 0;
    K->t_run = 0;
    K->t_done = 0;
    K->t_flash_until = 0;
}

/* Change the set duration by delta seconds, keeping the field in range and
 * never letting the elapsed part exceed the new duration. */
static void timer_adjust(ClockState *K, int field, int delta) {
    long dur = K->t_dur_ms / 1000;
    long mins = dur / 60, secs = dur % 60;
    if (field == 0) mins = clamp_l(mins + delta, 0, 99);
    else            secs = clamp_l(secs + delta, 0, 59);
    K->t_dur_ms = (mins * 60 + secs) * 1000;
    if (K->t_used_ms > K->t_dur_ms) K->t_used_ms = K->t_dur_ms;
    if (K->t_done) { K->t_done = 0; K->t_flash_until = 0; }
}

static void sw_toggle(ClockState *K, u64 now) {
    if (K->w_run) {
        K->w_acc_ms = sw_elapsed_ms(K->w_acc_ms, K->w_start_ns, 1, now);
        K->w_start_ns = 0;
        K->w_run = 0;
    } else {
        K->w_start_ns = now;
        K->w_run = 1;
    }
}

static void sw_reset(ClockState *K) {
    K->w_acc_ms = 0;
    K->w_start_ns = 0;
    K->w_run = 0;
    K->w_laps = 0;
}

static void sw_lap(ClockState *K, u64 now) {
    if (!K->w_run) return;
    if (K->w_laps >= LAP_MAX) return;          /* the list has a real limit */
    long total = sw_elapsed_ms(K->w_acc_ms, K->w_start_ns, 1, now);
    long prev = K->w_laps ? K->w_lap_total[K->w_laps - 1] : 0;
    if (total <= prev) return;                 /* a lap must have happened */
    K->w_lap_total[K->w_laps] = total;
    K->w_lap_split[K->w_laps] = total - prev;
    K->w_laps++;
}

static void alarm_adjust(ClockState *K, int field, int delta, long now_sec) {
    if (field == 0) K->a_h = clamp_i(K->a_h + delta, 0, 23);
    else            K->a_m = clamp_i(K->a_m + delta, 0, 59);
    /* A new target must not look like one we just crossed. */
    K->a_prev_sec = now_sec;
}

/* ------------------------------------------------------------ tick */

/* Advance every count from the real monotonic reading and the real wall
 * clock. This is the only place either of them is interpreted. */
static void tick(ClockState *K, u64 now, long sec_of_day) {
    if (K->t_run) {
        if (timer_left_ms(K->t_dur_ms, K->t_used_ms, K->t_start_ns, 1, now) <= 0) {
            K->t_used_ms = K->t_dur_ms;        /* pin it at zero */
            K->t_start_ns = 0;
            K->t_run = 0;
            K->t_done = 1;
            K->t_flash_until = now + (u64)FLASH_MS * 1000000ull;
        }
    }
    if (K->a_armed) {
        long target = (long)K->a_h * 3600L + (long)K->a_m * 60L;
        if (tod_passed(K->a_prev_sec, sec_of_day, target)) {
            K->a_armed = 0;                    /* the alarm rings once */
            K->a_rings = 1;
            K->a_flash_until = now + (u64)FLASH_MS * 1000000ull;
        }
    }
    K->a_prev_sec = sec_of_day;
}

/* The 4 Hz flash phase, 1 while a banner should be lit. */
static int flash_on(u64 now, u64 until) {
    if (!until || now >= until) return 0;
    return (int)((now / 250000000ull) & 1ull);
}

/* Seconds still to go before the armed alarm: the real distance on the
 * 24-hour circle from the wall clock to the target. */
static long alarm_left_s(const ClockState *K, long sec_of_day) {
    long target = (long)K->a_h * 3600L + (long)K->a_m * 60L;
    return (target - sec_of_day + 86400L) % 86400L;
}

/* ------------------------------------------------------------ drawing */

typedef struct {
    Surface *s;
    const Chrome *C;
    const ClockState *K;
    const Layout *L;
    const TabUI *u;
    int mx, my;
    u64 now;
} DC;

static void draw_pill(Surface *s, Rect r, int on, int hot, u32 accent) {
    if (!rect_has(r)) return;
    u32 fill = on ? WITH_A(accent, 46) : (hot ? RGB(0x1B, 0x25, 0x34) : COL_PANEL);
    u32 edge = on ? accent : (hot ? COL_EDGE2 : COL_EDGE);
    gfx_rrect_aa(s, (float)r.x, (float)r.y, (float)r.w, (float)r.h,
                 (float)r.h * 0.5f, fill, edge, 1.0f);
}

/* A field box: sunk, with the selected one outlined in the accent. */
static void draw_box(Surface *s, Rect r, int on, int hot) {
    if (!rect_has(r)) return;
    u32 edge = on ? COL_ACCENT : (hot ? COL_EDGE2 : COL_EDGE);
    gfx_rrect_aa(s, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 7.0f,
                 COL_SUNK, edge, on ? 1.6f : 1.0f);
}

static void draw_tabs(DC *d) {
    Font *tf = choose_tab_font(d->L->tabw);
    for (int i = 0; i < TAB_COUNT; i++) {
        Rect r = tab_rect(d->L, i);
        int on = (i == d->K->tab);
        int hot = rect_hit(r, d->mx, d->my);
        /* a tab whose thing is happening (timer done, alarm ringing)
         * pulses in the warning colour */
        int event = 0;
        if (i == TAB_TIMER && d->K->t_done)
            event = flash_on(d->now, d->K->t_flash_until);
        if (i == TAB_ALARM && d->K->a_rings)
            event = flash_on(d->now, d->K->a_flash_until);
        draw_pill(d->s, r, on, hot, event ? COL_HOT : COL_ACCENT);
        u32 col = on ? COL_TEXT : (event ? COL_HOT_TX : COL_DIM);
        font_box(d->s, tf, r, TAB_NAME[i], col, 1);
    }
}

static void draw_dial(Surface *s, Rect r, int hh, int mm, float ss, u32 accent) {
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    int rad = (r.w < r.h ? r.w : r.h) / 2;
    if (rad < 24) return;                    /* smaller than a clock face */

    float fx = (float)cx, fy = (float)cy;
    gfx_rrect_aa(s, fx - (float)rad, fy - (float)rad,
                 (float)rad * 2.0f, (float)rad * 2.0f, (float)rad,
                 COL_SUNK, COL_EDGE, 1.0f);

    for (int i = 0; i < 60; i++) {
        float a = (float)i * (TAU / 60.0f);
        float sn = sinf(a), cs = cosf(a);
        int major = (i % 5) == 0;
        float r0 = (float)rad * (major ? 0.80f : 0.87f);
        float r1 = (float)rad * 0.93f;
        gfx_line(s, fx + sn * r0, fy - cs * r0, fx + sn * r1, fy - cs * r1,
                 major ? 2.0f : 1.0f, major ? COL_DIM : COL_FAINT);
    }

    float ah = ((float)(hh % 12) + (float)mm / 60.0f) * (TAU / 12.0f);
    float am = (float)mm * (TAU / 60.0f) + ss * (TAU / 3600.0f);
    float as = ss * (TAU / 60.0f);
    float rh = (float)rad * 0.46f, rm = (float)rad * 0.70f, rs = (float)rad * 0.80f;
    gfx_line(s, fx, fy, fx + sinf(ah) * rh, fy - cosf(ah) * rh, 5.0f, COL_TEXT);
    gfx_line(s, fx, fy, fx + sinf(am) * rm, fy - cosf(am) * rm, 3.0f, COL_TEXT);
    gfx_line(s, fx, fy, fx + sinf(as) * rs, fy - cosf(as) * rs, 1.5f, accent);
    gfx_rrect_aa(s, fx - 4.0f, fy - 4.0f, 8.0f, 8.0f, 4.0f, accent, 0, 0);
}

/* The tabs share one clock reading: taken once per frame, passed in. */
typedef struct {
    int  hh, mm;
    float ss;
    long sec_of_day;
    char date[48];
    char digital[16];
    char zone[8];
} Wall;

static void read_wall(Wall *w) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    struct tm lv, gv;
    localtime_r(&t, &lv);
    gmtime_r(&t, &gv);

    w->hh = lv.tm_hour;
    w->mm = lv.tm_min;
    w->ss = (float)lv.tm_sec + (float)ts.tv_nsec / 1e9f;
    w->sec_of_day = tod_sec(&lv);
    fmt_hms(w->digital, sizeof(w->digital), w->sec_of_day);
    snprintf(w->date, sizeof(w->date), "%04d-%02d-%02d",
             lv.tm_year + 1900, lv.tm_mon + 1, lv.tm_mday);
    /* A chroot has no /etc/localtime, so local == UTC there. Say which one
     * it is from the two real readings instead of guessing a zone name. */
    int same = lv.tm_year == gv.tm_year && lv.tm_yday == gv.tm_yday &&
               lv.tm_hour == gv.tm_hour && lv.tm_min == gv.tm_min;
    snprintf(w->zone, sizeof(w->zone), "%s", same ? "UTC" : "local");
}

static void draw_clock_tab(DC *d, const Wall *w) {
    Surface *s = d->s;
    Rect dial = d->u->field[0];
    if (rect_has(dial))
        draw_dial(s, dial, w->hh, w->mm, w->ss, COL_ACCENT);

    text_box(s, FONT_MONO, d->u->big, w->digital, COL_TEXT);

    char line[64];
    snprintf(line, sizeof(line), "%s  %s", w->date, w->zone);
    text_box(s, FONT_REGULAR, d->u->line2, line, COL_BLUE);
}

/* The remaining time, or the banner that says it is over. */
static void draw_counter(DC *d, Rect r, const char *txt, int done, u64 until) {
    if (!rect_has(r)) return;
    if (done) {
        int lit = flash_on(d->now, until);
        gfx_rrect_aa(d->s, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 10.0f,
                     lit ? WITH_A(COL_HOT, 40) : WITH_A(COL_HOT, 18), COL_HOT, 1.2f);
        text_box(d->s, FONT_MONO, r, "TIME UP", lit ? COL_HOT_TX : COL_HOT);
        return;
    }
    text_box(d->s, FONT_MONO, r, txt, COL_TEXT);
}

static void draw_timer_tab(DC *d) {
    const ClockState *K = d->K;
    Surface *s = d->s;
    char buf[24];

    long mins = K->t_dur_ms / 60000;
    long secs = (K->t_dur_ms / 1000) % 60;
    for (int i = 0; i < 2; i++) {
        draw_box(s, d->u->field[i], K->t_field == i && !K->t_run,
                 rect_hit(d->u->field[i], d->mx, d->my));
        snprintf(buf, sizeof(buf), "%02ld", i == 0 ? mins : secs);
        text_box(s, FONT_MONO, d->u->field[i], buf,
                 K->t_field == i ? COL_TEXT : COL_DIM);

        draw_pill(s, d->u->minus[i], 0, rect_hit(d->u->minus[i], d->mx, d->my), COL_ACCENT);
        font_box(s, d->C->small, d->u->minus[i], "-", COL_DIM, 1);
        draw_pill(s, d->u->plus[i], 0, rect_hit(d->u->plus[i], d->mx, d->my), COL_ACCENT);
        font_box(s, d->C->small, d->u->plus[i], "+", COL_DIM, 1);
    }

    long left = timer_left_ms(K->t_dur_ms, K->t_used_ms, K->t_start_ns, K->t_run, d->now);
    fmt_mmss(buf, sizeof(buf), left);
    draw_counter(d, d->u->big, buf, K->t_done, K->t_flash_until);

    draw_pill(s, d->u->btn[0], K->t_run || K->t_done,
              rect_hit(d->u->btn[0], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[0],
             K->t_run ? "pause" : (K->t_done ? "again" : "start"), COL_TEXT, 1);
    draw_pill(s, d->u->btn[1], 0, rect_hit(d->u->btn[1], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[1], "reset", COL_TEXT, 1);
}

static void draw_stopwatch_tab(DC *d) {
    const ClockState *K = d->K;
    Surface *s = d->s;
    char buf[24];

    long ms = sw_elapsed_ms(K->w_acc_ms, K->w_start_ns, K->w_run, d->now);
    fmt_sw(buf, sizeof(buf), ms);
    text_box(s, FONT_MONO, d->u->big, buf, COL_TEXT);

    draw_pill(s, d->u->btn[0], K->w_run, rect_hit(d->u->btn[0], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[0], K->w_run ? "stop" : "start", COL_TEXT, 1);
    draw_pill(s, d->u->btn[1], 0, rect_hit(d->u->btn[1], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[1], "lap", COL_TEXT, 1);
    draw_pill(s, d->u->btn[2], 0, rect_hit(d->u->btn[2], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[2], "reset", COL_TEXT, 1);

    /* The lap list: newest first, as many rows as the box really has. */
    if (!rect_has(d->u->list)) return;
    if (!K->w_laps) {
        font_box(s, d->C->small, d->u->list, "no laps yet", COL_FAINT, 1);
        return;
    }
    int row_h = d->C->cell_h;
    for (int i = 0; i < d->u->rows; i++) {
        int idx = K->w_laps - 1 - i;
        if (idx < 0) break;
        Rect r;
        r.x = d->u->list.x;
        r.y = d->u->list.y + i * row_h;
        r.w = d->u->list.w;
        r.h = row_h;
        if (r.y + r.h > d->u->list.y + d->u->list.h) break;

        char no[16], sp[24], tt[24];
        snprintf(no, sizeof(no), "%d", idx + 1);
        fmt_sw(sp, sizeof(sp), K->w_lap_split[idx]);
        fmt_sw(tt, sizeof(tt), K->w_lap_total[idx]);

        font_box(s, d->C->small, r, no, COL_FAINT, 0);
        /* split in the middle, total on the right: both measured, both
         * inside their own half of the row */
        Rect mid = r, right = r;
        mid.x += (int)font_width(d->C->small, "00") + 6;
        mid.w = r.w / 2 - 8;
        right.w = r.w / 2 - 4;
        right.x = r.x + r.w - right.w;
        text_box(s, FONT_MONO, mid, sp, COL_DIM);
        text_box(s, FONT_MONO, right, tt, COL_TEXT);
    }
}

static void draw_alarm_tab(DC *d, const Wall *w) {
    const ClockState *K = d->K;
    Surface *s = d->s;
    char buf[24];

    for (int i = 0; i < 2; i++) {
        draw_box(s, d->u->field[i], K->a_field == i,
                 rect_hit(d->u->field[i], d->mx, d->my));
        snprintf(buf, sizeof(buf), "%02d", i == 0 ? K->a_h : K->a_m);
        text_box(s, FONT_MONO, d->u->field[i], buf,
                 K->a_field == i ? COL_TEXT : COL_DIM);

        draw_pill(s, d->u->minus[i], 0, rect_hit(d->u->minus[i], d->mx, d->my), COL_ACCENT);
        font_box(s, d->C->small, d->u->minus[i], "-", COL_DIM, 1);
        draw_pill(s, d->u->plus[i], 0, rect_hit(d->u->plus[i], d->mx, d->my), COL_ACCENT);
        font_box(s, d->C->small, d->u->plus[i], "+", COL_DIM, 1);
    }

    draw_pill(s, d->u->btn[0], K->a_armed, rect_hit(d->u->btn[0], d->mx, d->my),
              K->a_armed ? COL_WARN : COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[0], K->a_armed ? "disarm" : "arm",
             K->a_armed ? COL_WARN : COL_TEXT, 1);
    draw_pill(s, d->u->btn[1], 0, rect_hit(d->u->btn[1], d->mx, d->my), COL_ACCENT);
    font_box(s, d->C->small, d->u->btn[1], "now", COL_TEXT, 1);

    char status[64];
    if (K->a_rings) {
        fmt_tod(buf, sizeof(buf), K->a_h, K->a_m);
        snprintf(status, sizeof(status), "ALARM  %s  -  space", buf);
        text_box(s, FONT_SEMIBOLD, d->u->line2, status,
                 flash_on(d->now, K->a_flash_until) ? COL_HOT_TX : COL_HOT);
        return;
    }
    if (K->a_armed) {
        char until[32];
        fmt_until(until, sizeof(until), alarm_left_s(K, w->sec_of_day));
        snprintf(status, sizeof(status), "armed  %s", until);
        text_box(s, FONT_SEMIBOLD, d->u->big, status, COL_WARN);
        return;
    }
    text_box(s, FONT_REGULAR, d->u->big, "not armed", COL_FAINT);
}

/* The banner for an alarm that fired while another tab was showing. */
static void draw_banner(DC *d) {
    const ClockState *K = d->K;
    if (!K->a_rings) return;
    if (K->tab == TAB_ALARM) return;          /* its own tab already says it */
    Rect c = d->L->content;
    int h = 3 * d->C->cell_h;
    if (h > c.h) h = c.h;
    if (h < d->C->cell_h) return;
    Rect r;
    r.x = c.x;
    r.y = c.y + (c.h - h) / 2;
    r.w = c.w;
    r.h = h;
    int lit = flash_on(d->now, K->a_flash_until);
    gfx_rrect_aa(d->s, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 10.0f,
                 WITH_A(COL_HOT, lit ? 60 : 30), COL_HOT, 1.4f);
    char buf[32], line[64];
    fmt_tod(buf, sizeof(buf), K->a_h, K->a_m);
    snprintf(line, sizeof(line), "ALARM %s", buf);
    Rect top = r, bot = r;
    top.h = r.h / 2;
    bot.y = r.y + r.h / 2;
    bot.h = r.h - r.h / 2;
    text_box(d->s, FONT_SEMIBOLD, top, line, lit ? COL_HOT_TX : COL_HOT);
    text_box(d->s, FONT_REGULAR, bot, "space to dismiss", COL_TEXT);
}

static void draw_hint(DC *d) {
    const ClockState *K = d->K;
    const char *hint;
    switch (K->tab) {
    case TAB_CLOCK:     hint = "1-2-3-4: tabs    q: quit"; break;
    case TAB_TIMER:     hint = "arrows: field/value   space: start   r: reset"; break;
    case TAB_STOPWATCH: hint = "space: start/stop   l: lap   r: reset"; break;
    default:            hint = "arrows: field/value   space: arm/disarm"; break;
    }
    if (!rect_has(d->L->hint)) return;

    Font *f = fit_font_box(FONT_REGULAR, hint, (float)d->L->hint.w,
                           (float)d->L->hint.h);
    if (!f) {
        /* Nothing fits: use the smallest face and let the window edge clip
         * it. A hint that is cut off is better than a hint that is gone. */
        f = font_get(FONT_REGULAR, 8);
        if (!f) return;
    }
    font_box(d->s, f, d->L->hint, hint, COL_FAINT, 0);

    if (K->tab == TAB_STOPWATCH && K->w_laps) {
        char n[24];
        snprintf(n, sizeof(n), "laps %d", K->w_laps);
        font_box(d->s, f, d->L->hint, n,
                 K->w_laps >= LAP_MAX ? COL_WARN : COL_DIM, 2);
    }
}

static void draw_all(MClient *c, int vw, int vh, ClockState *K, const Chrome *C,
                     const Wall *w, u64 now) {
    Surface s = { c->px, vw, vh, c->stride };
    if (vw <= 0 || vh <= 0) return;

    int lit = flash_on(now, K->t_flash_until) || flash_on(now, K->a_flash_until);
    u32 top = lit ? color_mix(COL_BG_TOP, COL_HOT, 0.18f) : COL_BG_TOP;
    u32 bot = lit ? color_mix(COL_BG_BOT, COL_HOT, 0.10f) : COL_BG_BOT;
    gfx_gradient(&s, 0, 0, vw, vh, top, bot);

    Layout L;
    layout_window(vw, vh, C, &L);
    TabUI u;
    tab_geometry(K->tab, L.content, C, &u);

    DC d;
    d.s = &s; d.C = C; d.K = K; d.L = &L; d.u = &u;
    d.mx = K->mx; d.my = K->my; d.now = now;

    draw_tabs(&d);
    switch (K->tab) {
    case TAB_CLOCK:     draw_clock_tab(&d, w); break;
    case TAB_TIMER:     draw_timer_tab(&d); break;
    case TAB_STOPWATCH: draw_stopwatch_tab(&d); break;
    default:            draw_alarm_tab(&d, w); break;
    }
    draw_banner(&d);
    draw_hint(&d);
}

/* ------------------------------------------------------------ input */

static void tab_click(ClockState *K, const Layout *L, int x, int y) {
    for (int i = 0; i < TAB_COUNT; i++)
        if (rect_hit(tab_rect(L, i), x, y)) K->tab = i;
}

static void timer_click(ClockState *K, const TabUI *u, int x, int y, u64 now) {
    for (int i = 0; i < 2; i++) {
        if (rect_hit(u->field[i], x, y)) K->t_field = i;
        if (rect_hit(u->minus[i], x, y)) { K->t_field = i; timer_adjust(K, i, -1); }
        if (rect_hit(u->plus[i], x, y))  { K->t_field = i; timer_adjust(K, i, +1); }
    }
    if (rect_hit(u->btn[0], x, y)) timer_toggle(K, now);
    if (rect_hit(u->btn[1], x, y)) timer_reset(K);
}

static void sw_click(ClockState *K, const TabUI *u, int x, int y, u64 now) {
    if (rect_hit(u->btn[0], x, y)) sw_toggle(K, now);
    if (rect_hit(u->btn[1], x, y)) sw_lap(K, now);
    if (rect_hit(u->btn[2], x, y)) sw_reset(K);
}

static void alarm_click(ClockState *K, const TabUI *u, int x, int y, long now_sec) {
    for (int i = 0; i < 2; i++) {
        if (rect_hit(u->field[i], x, y)) K->a_field = i;
        if (rect_hit(u->minus[i], x, y)) { K->a_field = i; alarm_adjust(K, i, -1, now_sec); }
        if (rect_hit(u->plus[i], x, y))  { K->a_field = i; alarm_adjust(K, i, +1, now_sec); }
    }
    if (rect_hit(u->btn[0], x, y)) {
        K->a_armed = !K->a_armed;
        K->a_rings = 0;
        K->a_prev_sec = now_sec;
    }
    if (rect_hit(u->btn[1], x, y)) {
        /* "now": copy the real current time of day into the fields */
        K->a_h = (int)(now_sec / 3600);
        K->a_m = (int)((now_sec / 60) % 60);
        K->a_prev_sec = now_sec;
    }
}

/* ------------------------------------------------------------ main */

static u64 now_ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

int main(void) {
    MClient c;
    if (mc_open(&c, "clock", "Clock", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "clock: cannot connect to the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET")
                                        : "/run/mystic/wm.sock");
        return 1;
    }

    /* font_get() applies the text scale itself. It must NOT be re-read from
     * /etc/mystic/ui.conf here: inside the app jail there is no /etc at all,
     * and ui_scale_load() would fall back to 1.0 and throw away the scale the
     * window server passed in MYSTIC_UI_SCALE. */
    if (!font_init("/usr/share/fonts")) {
        fprintf(stderr, "clock: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    Chrome C;
    chrome_init(&C);
    if (!C.small) {
        fprintf(stderr, "clock: fonts failed to load\n");
        mc_close(&c);
        return 1;
    }

    ClockState K;
    memset(&K, 0, sizeof(K));
    K.tab = TAB_CLOCK;
    K.t_dur_ms = 5 * 60 * 1000;        /* a real 5:00 to start from */
    K.a_h = 7;
    K.a_m = 0;
    K.a_prev_sec = -1;
    K.mx = -1;
    K.my = -1;

    /* The buffer was mapped at the server's maximum, so a maximise can grow
     * the drawing area. Capping at the size we asked for left two thirds of
     * a maximised window showing stale pixels. */
    int cap_w = c.maxw > 0 ? c.maxw : c.w;
    int cap_h = c.maxh > 0 ? c.maxh : c.h;
    int vw = c.w, vh = c.h;

    int quit = 0;
    int have_sig = 0;
    long last_sig[6] = { 0 };
    int dirty = 1;
    /* Outside the loop on purpose: while the left button stays down the
     * motion events must keep being a drag, not become new clicks. */
    int was_down = 0;

    while (!quit) {
        u64 now = now_ns_monotonic();
        Wall w;
        read_wall(&w);
        tick(&K, now, w.sec_of_day);

        /* Redraw only when something that is on screen changed. The
         * signature is built from the same values the drawing uses, so a
         * running count cannot be shown stale and a quiet window costs
         * nothing. */
        long sig[6];
        sig[0] = K.tab;
        sig[1] = (K.tab == TAB_CLOCK) ? w.sec_of_day
               : (K.tab == TAB_TIMER) ? timer_left_ms(K.t_dur_ms, K.t_used_ms,
                                                      K.t_start_ns, K.t_run, now) / 100
               : (K.tab == TAB_STOPWATCH) ? sw_elapsed_ms(K.w_acc_ms, K.w_start_ns,
                                                          K.w_run, now) / 10
               : (long)K.a_armed * 100000L + alarm_left_s(&K, w.sec_of_day) / 60;
        sig[2] = flash_on(now, K.t_flash_until) + 2 * flash_on(now, K.a_flash_until);
        sig[3] = K.t_done + 2 * K.t_run + 4 * K.w_run + 8 * K.a_rings +
                 16 * K.w_laps + 32 * K.t_field + 64 * K.a_field;
        sig[4] = K.t_dur_ms / 1000;
        sig[5] = (long)K.a_h * 60 + K.a_m;

        int same = have_sig;
        for (int i = 0; i < 6 && same; i++)
            if (sig[i] != last_sig[i]) same = 0;
        if (!same) {
            for (int i = 0; i < 6; i++) last_sig[i] = sig[i];
            have_sig = 1;
            dirty = 1;
        }

        if (dirty) {
            draw_all(&c, vw, vh, &K, &C, &w, now);
            mc_present(&c);
            dirty = 0;
        }

        MEvent ev;
        while (mc_poll(&c, &ev)) {
            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;

            case MC_RESIZE:
                vw = ev.w < 1 ? 1 : (ev.w > cap_w ? cap_w : ev.w);
                vh = ev.h < 1 ? 1 : (ev.h > cap_h ? cap_h : ev.h);
                dirty = 1;                 /* nothing in K depends on this */
                break;

            case MC_KEY:
                if (!ev.press) break;
                if (ev.key == KEY_Q) { quit = 1; break; }
                if (ev.key >= KEY_1 && ev.key < KEY_1 + TAB_COUNT) {
                    K.tab = ev.key - KEY_1;
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_TAB) {
                    K.tab = (K.tab + 1) % TAB_COUNT;
                    dirty = 1;
                    break;
                }
                /* A fired alarm takes the first key press, whatever it was,
                 * so it cannot be missed and cannot leak into a control. */
                if (K.a_rings && ev.key != KEY_SPACE) {
                    K.a_rings = 0;
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_SPACE) {
                    if (K.a_rings) K.a_rings = 0;
                    else if (K.tab == TAB_TIMER) timer_toggle(&K, now);
                    else if (K.tab == TAB_STOPWATCH) sw_toggle(&K, now);
                    else if (K.tab == TAB_ALARM) {
                        K.a_armed = !K.a_armed;
                        K.a_prev_sec = w.sec_of_day;
                    }
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_R) {
                    if (K.tab == TAB_TIMER) timer_reset(&K);
                    else if (K.tab == TAB_STOPWATCH) sw_reset(&K);
                    else if (K.tab == TAB_ALARM) {
                        K.a_h = (int)(w.sec_of_day / 3600);
                        K.a_m = (int)((w.sec_of_day / 60) % 60);
                        K.a_prev_sec = w.sec_of_day;
                    }
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_L) {
                    if (K.tab == TAB_STOPWATCH) sw_lap(&K, now);
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT) {
                    int d = ev.key == KEY_RIGHT ? 1 : -1;
                    if (K.tab == TAB_TIMER) K.t_field = (K.t_field + d + 2) % 2;
                    if (K.tab == TAB_ALARM) K.a_field = (K.a_field + d + 2) % 2;
                    dirty = 1;
                    break;
                }
                if (ev.key == KEY_UP || ev.key == KEY_DOWN) {
                    int d = ev.key == KEY_UP ? 1 : -1;
                    if (ev.mods & 1) d *= 5;          /* shift: bigger steps */
                    if (K.tab == TAB_TIMER) timer_adjust(&K, K.t_field, d);
                    if (K.tab == TAB_ALARM) alarm_adjust(&K, K.a_field, d, w.sec_of_day);
                    dirty = 1;
                    break;
                }
                break;

            case MC_MOUSE:
                K.mx = ev.x;
                K.my = ev.y;
                dirty = 1;                     /* hover highlight */
                if ((ev.buttons & 1) && !was_down) {
                    Layout L;
                    layout_window(vw, vh, &C, &L);
                    TabUI u;
                    tab_geometry(K.tab, L.content, &C, &u);
                    tab_click(&K, &L, ev.x, ev.y);
                    if (K.tab == TAB_TIMER) timer_click(&K, &u, ev.x, ev.y, now);
                    if (K.tab == TAB_STOPWATCH) sw_click(&K, &u, ev.x, ev.y, now);
                    if (K.tab == TAB_ALARM) alarm_click(&K, &u, ev.x, ev.y, w.sec_of_day);
                    /* a click anywhere else dismisses a ringing alarm */
                    if (K.tab != TAB_ALARM) K.a_rings = 0;
                }
                was_down = ev.buttons & 1;
                break;

            default:
                break;
            }
        }
        if (quit) break;

        /* Wake up often enough for the fastest thing that is running, and
         * lazily when nothing is. There is no frame clock here. */
        int delay = 150;
        if (K.t_run || K.w_run) delay = 30;
        else if (flash_on(now, K.t_flash_until) || flash_on(now, K.a_flash_until) ||
                 now < K.t_flash_until || now < K.a_flash_until) delay = 60;
        else if (K.tab == TAB_CLOCK) delay = 120;
        usleep((useconds_t)delay * 1000);
    }

    mc_close(&c);
    return 0;
}

#endif /* !CLOCK_HOST_TEST */
