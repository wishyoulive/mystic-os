/* ============================================================
 *  Mystic OS - input.c
 *  Mouse and keyboard, straight from the kernel's event devices.
 *
 *  Every axis, button and key code here is a real evdev code; there
 *  is no simulation. We only need to know which /dev/input/eventN is
 *  a mouse and which is a keyboard, and that we ask the device.
 * ============================================================ */
#include "mystic.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* The kernel gives evdev 32 minors (EVDEV_MINORS in drivers/input/evdev.c), so
 * event0..event31 is every device a machine can have. The old loop stopped at
 * event7 and silently closed everything above it: on a desktop whose first
 * seven nodes are the power button, the lid switch, the video bus and the four
 * HDA jack detectors - which is what an ordinary laptop looks like - a
 * keyboard enumerating as event8 or later was never opened, and a second
 * pointing device (a mouse next to a touchpad) never contributed a single
 * wheel event even though it was plugged in. */
#define MAX_EVDEV    32
#define MAX_POINTERS  8
#define MAX_KEYB      8

static double last_t = -1.0;
static int    last_x, last_y;

/* Every pointer and keyboard that was found. A device that is both - the
 * wireless keyboard/touchpad combos report REL_X and KEY_A on one node - goes
 * into both lists, because its keys and its wheel must both be delivered.
 * Keeping only the first of each class is what dropped whole devices. */
static int  mouse_fd[MAX_POINTERS];
static int  nmouse;
static int  key_fd[MAX_KEYB];
static int  nkeybd;
static int  led_fd = -1;   /* first keyboard opened for writing: Caps Lock LED */
static int  caps_lock;     /* Caps Lock, restored from the LED of the first keyboard */
static char mouse_name[64] = "none";
static char key_name[64] = "none";

static int has_bit(int fd, int ev_type, int code) {
    unsigned long bits[8] = {0};
    int len = (code / (int)(8 * sizeof(long))) + 1;
    if (len > 8) len = 8;
    if (ioctl(fd, EVIOCGBIT(ev_type, 0), bits) < 0) return 0;
    if (ioctl(fd, EVIOCGBIT(ev_type, len * (int)sizeof(long)), bits) < 0) return 0;
    int word = code / (8 * (int)sizeof(long));
    int bit = code % (8 * (int)sizeof(long));
    if (word >= 8) return 0;
    return (bits[word] >> bit) & 1;
}

/* ------------------------------------------------------------
 *  Touchpad gestures
 *
 *  A multitouch touchpad does not report motion, it reports CONTACTS. Under
 *  evdev's protocol B each contact owns a slot:
 *
 *      EV_ABS ABS_MT_SLOT          which slot the next values belong to
 *      EV_ABS ABS_MT_TRACKING_ID   a new id opens the slot, -1 closes it
 *      EV_ABS ABS_MT_POSITION_X/Y  that slot's position, in device units
 *
 *  and a packet ends at EV_SYN/SYN_REPORT. Everything a gesture needs is
 *  computed at the end of a packet, never per event: the second finger's
 *  position in the same packet may still be in flight when the first one is
 *  read.
 *
 *  The older reporting style has no slots at all: it says how many fingers
 *  are down with BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
 *  BTN_TOOL_QUADTAP and BTN_TOOL_QUINTTAP (the kernel's names, and its
 *  documented contact counts), with BTN_TOUCH saying a finger is on the pad.
 *  Those are EV_KEY codes - the BTN range lives inside EV_KEY - which is why
 *  they used to fall through this file's key branch and get queued as if the
 *  user had typed them. Both styles are accepted here and neither reaches
 *  keys[]. That style has a finger COUNT but no positions, so it can report
 *  how many fingers are down and cannot make a gesture out of them; the
 *  gestures themselves need protocol B's slots. A pad like that already gets
 *  its scrolling from the REL_WHEEL its driver emits, which this file has
 *  read all along.
 *
 *  The numbers below are POLICY, not measurements: they are the thresholds a
 *  gesture has to pass before it is allowed to do anything. Positions are
 *  delivered as pixels - this file already treats an absolute touch position
 *  (ABS_X) as a logical pixel - so a delta here is a distance on screen at
 *  1:1, and the desktop turns it into lines with its own font height.
 * ------------------------------------------------------------ */
#define MT_SLOTS      10     /* slots this parser tracks                      */

#define MT_TAP_SLOP     8    /* px a contact may wander and still be a tap    */
#define MT_TAP_MS     350    /* ms first contact -> last lift; the same window
                              * the double-click test above uses             */
#define MT_SCROLL_MIN   4    /* px of centroid travel to latch a scroll       */
#define MT_SWIPE_MIN   30    /* px to latch a three-finger swipe              */
#define MT_PINCH_MIN    4    /* px of gap change to latch a pinch             */
#define MT_PINCH_RATIO 0.04f /* ... or 4 % of the starting gap                */
#define MT_AXIS_LOCK  115    /* an axis is dominant past 1.15x the other      */

/* One contact. `fresh` marks a slot opened in the current packet: its first
 * position is the contact's origin, not a move from wherever the previous
 * finger left the slot, so its delta is zero by definition. */
typedef struct {
    int used;                /* a contact owns this slot right now. The
                              * tracking id itself is not kept: a value of 0
                              * is a legal id, so "empty" cannot be spelled
                              * with it, and every packet that matters (a new
                              * contact) is marked by `fresh` below.        */
    int x, y;                /* latest position                            */
    int px, py;              /* position at the previous packet            */
    int fresh;               /* opened since the previous packet            */
} MtSlot;

static struct {
    /* 0 until input_open() finds the touchpad; then its fd, or -1 when the
     * machine has none. Accepting MT codes while it is 0 is what lets a host
     * harness drive this parser directly, and fd 0 is never an evdev node:
     * input_open() assigns the touchpad's fd before any event is read. */
    int fd;
    int cur;                 /* ABS_MT_SLOT in force                        */
    MtSlot slot[MT_SLOTS];
    unsigned tool;           /* BTN_TOOL_* contact-count bits down          */

    /* the run of contacts currently on the pad */
    int  down;               /* contacts were down at the previous packet   */
    int  fingers;            /* how many of them                            */
    int  no_tap;             /* this run may not end as a tap: its fingers
                              * are the tail of a gesture, or of a tap that
                              * was already reported                         */
    long start_ms;           /* when the first one landed                    */
    int  sx, sy;             /* centroid then                               */
    int  lx, ly;             /* centroid at the previous packet             */
    int  travel;             /* furthest the centroid has been from (sx,sy) */
    int  dist0;              /* two-finger gap then, px                     */
    int  kind;               /* GESTURE_* latched, or GESTURE_NONE          */
    int  axis;               /* 1 = horizontal, 2 = vertical                */
    int  gid;                /* sequence number of the last latch           */
    float last_scale;        /* ratio to report on the release frame        */
} mt;

/* MT codes are only accepted from the touchpad - a second pointing device
 * must not be able to move a gesture that the first one started. */
static int mt_accept(int fd) {
    return mt.fd == 0 || fd == mt.fd;
}

/* Integer square root: no libm in the event path, and the answer must not
 * depend on the host's floating point. */
static int mt_isqrt(unsigned long long v) {
    unsigned long long r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (int)r;
}

static int mt_gap(int ax, int ay, int bx, int by) {
    long long dx = (long long)ax - bx, dy = (long long)ay - by;
    return mt_isqrt((unsigned long long)(dx * dx + dy * dy));
}

/* The kernel's contact-count codes, ordered by the number of fingers they
 * mean: BTN_TOOL_QUINTTAP is 0x148 while BTN_TOOL_DOUBLETAP is 0x14d, so the
 * numeric order of the codes says nothing about the number of fingers. The
 * bit mt.tool keeps for code i is (1u << i). */
static const int MT_TOOL[5] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                                BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP,
                                BTN_TOOL_QUINTTAP };

/* The classic report's finger count: the highest BTN_TOOL_* bit that is down
 * is the number of contacts (that is how the kernel documents them). */
static int mt_tool_fingers(void) {
    int n = 0;
    for (int i = 0; i < 5; i++)
        if (mt.tool & (1u << i)) n = i + 1;
    return n;
}

/* EV_KEY codes that belong to the touchpad rather than the keyboard. Returns
 * 1 when the code was consumed. */
static int mt_tool_event(int code, int value) {
    for (int i = 0; i < 5; i++) {
        if (code != MT_TOOL[i]) continue;
        if (value) mt.tool |= 1u << i;
        else       mt.tool &= ~(1u << i);
        return 1;
    }
    if (code == BTN_TOUCH) {
        /* "a finger is on the pad". The count bits mean nothing once it
         * lifts, so they go with it. */
        if (!value) mt.tool = 0;
        return 1;
    }
    return 0;
}

static long mt_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* A run of contacts starts: this is where the tap timer, the centroid the
 * deltas are measured from and the pinch ratio all begin, which is why a tap
 * produces no travel and a pinch ratio of exactly 1.0. */
static void mt_start(int n, int cx, int cy, int gap) {
    mt.down = 1;
    mt.fingers = n;
    mt.start_ms = mt_now_ms();
    mt.sx = cx;  mt.sy = cy;
    mt.lx = cx;  mt.ly = cy;
    mt.travel = 0;
    mt.dist0 = gap;
    mt.kind = GESTURE_NONE;
    mt.axis = 0;
    mt.last_scale = 1.0f;
}

/* A device that was unplugged mid-gesture leaves its contacts standing, and a
 * gesture whose device is gone can never end - which would leave the desktop
 * holding a text scale nobody can release. Forgetting them makes the next
 * packet report the release edge, and no_tap stops an unplug from looking like
 * a click. */
static void mt_forget(void) {
    for (int s = 0; s < MT_SLOTS; s++) mt.slot[s].used = 0;
    mt.tool = 0;
    mt.no_tap = 1;
}

/* The gesture in progress is over. It is named ONE more time, on the frame it
 * ends, so whoever acted on it can commit what it changed; after that every
 * field is zero and nothing fires again. */
static void mt_end(Input *in) {
    if (mt.kind == GESTURE_NONE) return;
    in->gesture = mt.kind;
    in->gesture_fingers = mt.fingers;
    in->gesture_scale = (mt.kind == GESTURE_PINCH) ? mt.last_scale : 0.0f;
    in->gesture_release = 1;
    mt.kind = GESTURE_NONE;
}

/* One kernel packet worth of contacts. Called at SYN_REPORT and once more
 * after draining a device, so a report that was cut off still settles instead
 * of waiting for a SYN that is not coming. */
static void mt_packet(Input *in) {
    int n = 0, sx = 0, sy = 0;
    int px[MT_SLOTS], py[MT_SLOTS], fdx[MT_SLOTS], fdy[MT_SLOTS];

    for (int s = 0; s < MT_SLOTS; s++) {
        MtSlot *sl = &mt.slot[s];
        if (!sl->used) continue;
        fdx[n] = sl->fresh ? 0 : sl->x - sl->px;
        fdy[n] = sl->fresh ? 0 : sl->y - sl->py;
        px[n] = sl->x; py[n] = sl->y;
        sx += sl->x; sy += sl->y;
        sl->px = sl->x; sl->py = sl->y; sl->fresh = 0;
        n++;
    }
    /* Protocol B is authoritative when it reports slots; the BTN_TOOL_* count
     * is what a pad without slots can say. */
    in->finger_n = n ? n : mt_tool_fingers();
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        in->finger_dx[i] = i < n ? fdx[i] : 0;
        in->finger_dy[i] = i < n ? fdy[i] : 0;
    }

    if (n == 0) {
        /* Every contact has lifted. */
        if (mt.kind != GESTURE_NONE) {
            mt_end(in);                          /* exactly one release edge */
        } else if (mt.down && !mt.no_tap) {
            long held = mt_now_ms() - mt.start_ms;
            if (mt.travel <= MT_TAP_SLOP && held <= MT_TAP_MS) {
                in->gesture_tap = 1;
                in->gesture_tap_fingers = mt.fingers;
            }
        }
        mt.no_tap = 0;
        mt.down = 0;
        mt.fingers = 0;
        mt.kind = GESTURE_NONE;
        mt.axis = 0;
        return;
    }

    int cx = sx / n, cy = sy / n;
    int gap = (n == 2) ? mt_gap(px[0], py[0], px[1], py[1]) : 0;

    if (!mt.down) { mt.no_tap = 0; mt_start(n, cx, cy, gap); return; }
    if (n != mt.fingers) {
        /* The contact count changed mid-run: a finger landed or lifted. What
         * was in progress ends here and the new count gets its own gesture, so
         * a three-finger swipe is not a scroll plus one finger.
         *
         * Two things about real hands live in this branch. People lift the
         * fingers of a two-finger tap a few milliseconds apart, so the tap is
         * reported HERE, at the first lift, with the number of fingers it
         * started with - waiting for the last finger would lose almost every
         * tap. And the fingers left over from a gesture that has just ended
         * are its tail, not a new tap: a three-finger swipe that ends with the
         * fingers coming up one at a time must not turn the last two into a
         * two-finger tap, which would open a context menu at the end of every
         * swipe. */
        if (mt.kind != GESTURE_NONE) {
            mt_end(in);
            mt.no_tap = 1;      /* the fingers left over are the tail       */
        } else if (n < mt.fingers) {
            if (!mt.no_tap) {
                long held = mt_now_ms() - mt.start_ms;
                if (mt.travel <= MT_TAP_SLOP && held <= MT_TAP_MS) {
                    in->gesture_tap = 1;
                    in->gesture_tap_fingers = mt.fingers;
                }
            }
            mt.no_tap = 1;      /* a tap is reported once                   */
        }
        /* n > mt.fingers with nothing latched is fingers still arriving, so
         * the run may yet be the tap it looks like: no_tap is left alone. */
        mt_start(n, cx, cy, gap);
        return;
    }

    int dx = cx - mt.lx, dy = cy - mt.ly;
    mt.lx = cx; mt.ly = cy;

    int ex = cx - mt.sx, ey = cy - mt.sy;
    if (ex < 0) ex = -ex;
    if (ey < 0) ey = -ey;
    if (ex > mt.travel) mt.travel = ex;
    if (ey > mt.travel) mt.travel = ey;

    int latched = 0;
    if (mt.kind == GESTURE_NONE) {
        if (n >= 3) {
            if (ex >= MT_SWIPE_MIN || ey >= MT_SWIPE_MIN) {
                mt.kind = GESTURE_SWIPE;
                mt.axis = (ex * 100 >= ey * MT_AXIS_LOCK) ? 1 :
                          (ey * 100 >= ex * MT_AXIS_LOCK) ? 2 : (ex > ey ? 1 : 2);
                latched = 1;
            }
        } else if (n == 2) {
            int change = gap - mt.dist0;
            if (change < 0) change = -change;
            int travel = ex > ey ? ex : ey;
            int pinch_px = (int)((float)mt.dist0 * MT_PINCH_RATIO + 0.5f);
            if (pinch_px < MT_PINCH_MIN) pinch_px = MT_PINCH_MIN;
            if (gap > 0 && mt.dist0 > 0 &&
                change >= pinch_px && change >= travel) {
                mt.kind = GESTURE_PINCH;
                latched = 1;
            } else if (travel >= MT_SCROLL_MIN) {
                mt.kind = GESTURE_SCROLL;
                mt.axis = (ex * 100 >= ey * MT_AXIS_LOCK) ? 1 :
                          (ey * 100 >= ex * MT_AXIS_LOCK) ? 2 : (ex > ey ? 1 : 2);
                latched = 1;
            }
        }
        /* one finger is the pointer, not a gesture */
        if (latched) { mt.gid++; in->gesture_id = mt.gid; }
    }

    switch (mt.kind) {
    case GESTURE_SCROLL:
        /* The travel up to the latch is delivered on the latch packet too, so
         * no part of a scroll is lost to the threshold. */
        if (latched) { in->gesture_dx += cx - mt.sx; in->gesture_dy += cy - mt.sy; }
        else         { in->gesture_dx += dx;          in->gesture_dy += dy; }
        /* Axis lock: only the axis the gesture latched on is delivered. The
         * other is consumed HERE, which is what stops a mostly-vertical
         * scroll from jittering a list sideways. */
        if (mt.axis == 1) in->gesture_dy = 0;
        else              in->gesture_dx = 0;
        break;
    case GESTURE_PINCH:
        if (mt.dist0 > 0 && gap > 0) {
            mt.last_scale = (float)gap / (float)mt.dist0;
            in->gesture_scale = mt.last_scale;
        } else {
            mt.last_scale = 1.0f;
            in->gesture_scale = 1.0f;
        }
        break;
    case GESTURE_SWIPE:
        if (latched) { in->gesture_dx += cx - mt.sx; in->gesture_dy += cy - mt.sy; }
        else         { in->gesture_dx += dx;          in->gesture_dy += dy; }
        break;
    default:
        return;                       /* nothing latched: no gesture to report */
    }
    in->gesture = mt.kind;
    in->gesture_fingers = mt.fingers;
}

void input_set_screen(Input *in, int w, int h) {
    in->scr_w = w;
    in->scr_h = h;
}

/* ---- scripted input -------------------------------------------------------
 *
 * MYSTIC_INPUT_TEST=<file> replays a text script instead of reading evdev:
 *
 *     abs 690 470     move the pointer to a screen position
 *     press           left button down
 *     release         left button up
 *     wheel 3         three notches up (negative scrolls down)
 *     key a           a key press and release, same names evdev uses
 *     wait 3          three frames with nothing happening
 *
 * One line is consumed per poll, so the compositor sees a sequence of frames
 * exactly as it would from a device. This exists because a synthetic pointer
 * driven through the hypervisor's PS/2 model drops and reorders events, which
 * makes a resize or a drag untestable; with a script the same interaction can
 * be reproduced deterministically, on any machine, in CI.
 */
static FILE *test_script;
static int   test_eof;
static int   test_wait;          /* frames still to skip before the next line */
static char  test_pending[512];  /* text that did not fit in one frame */
static int   test_pending_len, test_pending_off;

/* Hand the focused window as much of the pending text as one frame carries. */
static void test_pump(Input *in) {
    int room = (int)sizeof(in->text) - 1;
    int left = test_pending_len - test_pending_off;
    int n = left < room ? left : room;
    if (n <= 0) return;
    memcpy(in->text, test_pending + test_pending_off, (size_t)n);
    in->text_len = n;
    test_pending_off += n;
}

static void test_open(void) {
    /* init starts the desktop, so an environment variable set in a shell is not
     * visible here; the path is also read from a file the image can carry. */
    const char *path = getenv("MYSTIC_INPUT_TEST");
    if (!path || !*path) {
        static const char marker[] = "/run/mystic/input-script";
        if (access(marker, R_OK) == 0) path = marker;
        else return;
    }
    test_script = fopen(path, "r");
    if (!test_script) {
        fprintf(stderr, "input: MYSTIC_INPUT_TEST %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(stderr, "input: scripted device %s\n", path);
}

static void test_step(Input *in) {
    char line[192];
    if (!test_script || test_eof) return;
    if (test_wait > 0) { test_wait--; return; }
    if (test_pending_off < test_pending_len) { test_pump(in); return; }
    if (!fgets(line, sizeof line, test_script)) { test_eof = 1; return; }
    char what[24];
    int a = 0, b = 0;
    if (sscanf(line, "%23s %d %d", what, &a, &b) < 1) return;
    fprintf(stderr, "script: %s", line);
    if (!strcmp(what, "abs")) {
        in->mouse_x = a; in->mouse_y = b;
        in->mouse_dx += a; in->mouse_dy += b;
    } else if (!strcmp(what, "press")) {
        if (!(in->buttons & 1)) in->clicked = 1;
        in->buttons |= 1;
    } else if (!strcmp(what, "release")) {
        if (in->buttons & 1) in->released = 1;
        in->buttons &= ~1;
    } else if (!strcmp(what, "rpress")) {
        if (!(in->buttons & 2)) in->clicked = 1;
        in->buttons |= 2;
    } else if (!strcmp(what, "rrelease")) {
        if (in->buttons & 2) in->released = 1;
        in->buttons &= ~2;
    } else if (!strcmp(what, "wheel")) {
        in->wheel += a;
    } else if (!strcmp(what, "text")) {
        const char *t = strchr(line, ' ');
        if (!t) return;
        t++;
        size_t n = strlen(t);
        while (n && (t[n-1] == 10 || t[n-1] == 13)) n--;
        /* A line can be longer than one frame's text queue (a shell command
         * with a URL easily is), so the remainder is carried to the next
         * frames instead of being cut. */
        if (n >= sizeof(test_pending)) n = sizeof(test_pending) - 1;
        memcpy(test_pending, t, n);
        test_pending[n] = 0;
        test_pending_len = (int)n;
        test_pending_off = 0;
        test_pump(in);
        return;
    } else if (!strcmp(what, "enter")) {
        /* A window receives text, not keycodes: main.c delivers input.text to
         * the focused terminal or app, so Enter has to be the byte itself. */
        in->text[0] = 13;    /* carriage return */
        in->text_len = 1;
        in->key = 28;                 /* KEY_ENTER, for the shortcut table */
        in->keys[0] = 28;
        in->nkeys = 1;
    } else if (!strcmp(what, "wait")) {
        /* a is a count of frames */
        test_wait = a;
    }
}

int input_open(Input *in) {
    memset(in, 0, sizeof(*in));
    in->mouse_x = 640;
    in->mouse_y = 400;

    char path[64];
    for (int i = 0; i < MAX_EVDEV; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        /* Write access is only needed to light the Caps Lock LED; a device that
         * refuses it still works read-only, so fall back instead of dropping
         * the keyboard. */
        int fd = open(path, O_RDWR | O_NONBLOCK);
        int writable = fd >= 0;
        if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

        /* A touchpad may report nothing but ABS_MT_*: it is still the
         * pointing device, and it is the one that carries gestures. */
        int is_mt = has_bit(fd, EV_ABS, ABS_MT_SLOT) ||
                    has_bit(fd, EV_ABS, ABS_MT_POSITION_X) ||
                    has_bit(fd, EV_ABS, ABS_MT_POSITION_Y);
        int is_mouse = has_bit(fd, EV_REL, REL_X) || has_bit(fd, EV_ABS, ABS_X) || is_mt;
        int is_key   = has_bit(fd, EV_KEY, KEY_A) || has_bit(fd, EV_KEY, KEY_SPACE);
        int kept = 0;

        if (is_mouse && nmouse < MAX_POINTERS) {
            mouse_fd[nmouse++] = fd;
            /* The kernel reports the axis range for the device itself; a
             * tablet says 0..32767, a touchscreen says its panel size. */
            if (in->abs_max_x == 0 && has_bit(fd, EV_ABS, ABS_X)) {
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(ABS_X), &ai) >= 0 && ai.maximum > 0)
                    in->abs_max_x = ai.maximum;
            }
            if (in->abs_max_y == 0 && has_bit(fd, EV_ABS, ABS_Y)) {
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) >= 0 && ai.maximum > 0)
                    in->abs_max_y = ai.maximum;
            }
            if (!strcmp(mouse_name, "none"))
                snprintf(mouse_name, sizeof(mouse_name), "%s", name);
            fprintf(stderr, "input: pointer  %s (%s)\n", path, name);
            kept = 1;
        }
        /* The first multitouch device is the gesture source. Only the address
         * is remembered: a device that failed to be kept (no pointer slot
         * left) must not become one, or the parser would read a closed fd. */
        if (is_mt && kept && mt.fd == 0) {
            mt.fd = fd;
            fprintf(stderr, "input: touchpad %s (%s, multitouch)\n", path, name);
        }
        if (is_key && nkeybd < MAX_KEYB) {
            key_fd[nkeybd++] = fd;
            if (!strcmp(key_name, "none"))
                snprintf(key_name, sizeof(key_name), "%s", name);
            if (writable && led_fd < 0) led_fd = fd;
            if (nkeybd == 1) {
                /* Caps Lock may already be locked from before this process
                 * started. Its LED is the only state there is, so start from
                 * the lamp instead of assuming the key is off. */
                unsigned long leds = 0;
                if (ioctl(fd, EVIOCGLED(sizeof(leds)), &leds) >= 0)
                    caps_lock = (int)((leds >> LED_CAPSL) & 1ul);
            }
            fprintf(stderr, "input: keyboard %s (%s)\n", path, name);
            kept = 1;
        }
        if (!kept) {
            fprintf(stderr, "input: ignoring %s (%s)\n", path, name);
            close(fd);
        }
    }
    /* No touchpad was found: say so once, so every MT code from here on is
     * known not to be ours. On such a machine the desktop keeps the plain
     * mouse and wheel path it always had. */
    if (mt.fd == 0) mt.fd = -1;
    fprintf(stderr, "input: mouse=%s keyboard=%s (%d pointer, %d keyboard%s)\n",
            mouse_name, key_name, nmouse, nkeybd,
            mt.fd > 0 ? ", multitouch" : ", no touchpad");
    return (nmouse || nkeybd) ? 0 : -1;
}

/* US keyboard: keycode -> character, explicit so it can be read and
 * checked. (An offset table computed from row strings was off by one and
 * turned "ls" into "ka" - a real bug, caught by a real test.) */
static const struct { int code; char lo, hi; } KEYMAP[] = {
    {KEY_1,'1','!'},{KEY_2,'2','@'},{KEY_3,'3','#'},{KEY_4,'4','$'},
    {KEY_5,'5','%'},{KEY_6,'6','^'},{KEY_7,'7','&'},{KEY_8,'8','*'},
    {KEY_9,'9','('},{KEY_0,'0',')'},{KEY_MINUS,'-','_'},{KEY_EQUAL,'=','+'},
    {KEY_Q,'q','Q'},{KEY_W,'w','W'},{KEY_E,'e','E'},{KEY_R,'r','R'},
    {KEY_T,'t','T'},{KEY_Y,'y','Y'},{KEY_U,'u','U'},{KEY_I,'i','I'},
    {KEY_O,'o','O'},{KEY_P,'p','P'},{KEY_LEFTBRACE,'[','{'},{KEY_RIGHTBRACE,']','}'},
    {KEY_A,'a','A'},{KEY_S,'s','S'},{KEY_D,'d','D'},{KEY_F,'f','F'},
    {KEY_G,'g','G'},{KEY_H,'h','H'},{KEY_J,'j','J'},{KEY_K,'k','K'},
    {KEY_L,'l','L'},{KEY_SEMICOLON,';',':'},{KEY_APOSTROPHE,'\'','"'},
    {KEY_GRAVE,'`','~'},{KEY_BACKSLASH,'\\','|'},
    {KEY_Z,'z','Z'},{KEY_X,'x','X'},{KEY_C,'c','C'},
    {KEY_V,'v','V'},{KEY_B,'b','B'},{KEY_N,'n','N'},{KEY_M,'m','M'},
    {KEY_COMMA,',','<'},{KEY_DOT,'.','>'},{KEY_SLASH,'/','?'},
    /* The numeric keypad. Without these seventeen rows (KpEnter is handled in
     * key_to_char) the whole pad was dead: typing 1+1<Enter> on it produced
     * nothing at all, which is part of "a lot of keyboard keys not
     * recognised". A keypad does not change under Shift (NumLock is a
     * separate, kernel-side lock), so lo == hi. */
    {KEY_KP0,'0','0'},{KEY_KP1,'1','1'},{KEY_KP2,'2','2'},{KEY_KP3,'3','3'},
    {KEY_KP4,'4','4'},{KEY_KP5,'5','5'},{KEY_KP6,'6','6'},{KEY_KP7,'7','7'},
    {KEY_KP8,'8','8'},{KEY_KP9,'9','9'},{KEY_KPDOT,'.','.'},{KEY_KPCOMMA,',',','},
    {KEY_KPSLASH,'/','/'},{KEY_KPASTERISK,'*','*'},{KEY_KPMINUS,'-','-'},
    {KEY_KPPLUS,'+','+'},{KEY_KPEQUAL,'=','='},
    /* Key 86: the extra key between Left Shift and Z that only ISO keyboards
     * have. It is '<' and '>' in the kernel's own US keymap (defkeymap), and it
     * was unhandled, so on every ISO keyboard that key typed nothing. */
    {KEY_102ND,'<','>'},
};

/* Caps Lock is a lock over letters only: it never turns '1' into '!', and
 * Shift cancels it back to lower case. Every letter row above has a lower-case
 * `lo`, so that property is read straight off the table instead of being
 * duplicated in a second flag that could drift away from it. */
static int key_to_char(int code, int shift, int caps) {
    switch (code) {
        case KEY_SPACE:     return ' ';
        case KEY_ENTER:     return '\n';
        case KEY_KPENTER:   return '\n';
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB:       return '\t';
        default: break;
    }
    for (size_t i = 0; i < sizeof(KEYMAP) / sizeof(KEYMAP[0]); i++) {
        if (KEYMAP[i].code != code) continue;
        int upper = (KEYMAP[i].lo >= 'a' && KEYMAP[i].lo <= 'z')
                  ? (shift != caps)      /* a letter: Caps Lock and Shift cancel */
                  : shift;               /* a digit or a symbol: Shift only */
        return (unsigned char)(upper ? KEYMAP[i].hi : KEYMAP[i].lo);
    }
    return 0;
}

/* The character a key produces, after the Ctrl+letter -> control-code rule
 * (Ctrl+S is 0x13). Used for both the press and the autorepeat edge. */
static int key_char(int code, int shift, int ctrl, int caps) {
    int ch = key_to_char(code, shift, caps);
    if (ch && ctrl && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
        ch = (ch >= 'a' ? ch - 'a' : ch - 'A') + 1;
    return ch;
}

/* Which keys may repeat. The desktop's global shortcuts (Esc closes the top
 * window, Super toggles the launcher, F1..F5 open windows) are one-shot
 * actions in main.c that run for every entry in keys[], so letting those
 * repeat would close every window or open thirty terminals while a key is
 * held - real desktops suppress autorepeat for keybindings for exactly that
 * reason. The keys a person holds on purpose are the ones that repeat: the
 * character keys (include space, Enter and Backspace) and the editing and
 * navigation keys. */
static int key_repeats(int code) {
    if (key_to_char(code, 0, 0) || key_to_char(code, 1, 0)) return 1;
    switch (code) {
        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        case KEY_HOME: case KEY_END: case KEY_PAGEUP: case KEY_PAGEDOWN:
        case KEY_INSERT: case KEY_DELETE:
            return 1;
        default:
            return 0;
    }
}

/* Per-frame event queues are deliberately small; app code walks them every
 * frame. A burst larger than the buffer is therefore possible (a key held
 * down, a chord of arrows, a pasted-on-the-keyboard run of digits), and when
 * it happens we drop the OLDEST entry and append the new one. The queue is
 * consumed once per frame, so the events at the front are already stale by
 * the time the frame is drawn while the newest one is what the user is
 * reacting to - keeping the newest is the behaviour a person can feel. */
static void push_key(int *arr, int *n, int cap, int code) {
    if (*n >= cap) {
        memmove(arr, arr + 1, sizeof(*arr) * (size_t)(cap - 1));
        *n = cap - 1;
    }
    arr[(*n)++] = code;
}

/* Same choice for the character queue: keep the newest bytes. One byte is
 * reserved for the NUL that input_poll() writes. */
static void push_char(Input *in, char ch) {
    int cap = (int)sizeof(in->text) - 1;
    if (in->text_len >= cap) {
        memmove(in->text, in->text + 1, (size_t)(cap - 1));
        in->text_len = cap - 1;
    }
    in->text[in->text_len++] = ch;
}

/* ------------------------------------------------------------
 *  The wheel
 *
 *  evdev reports one physical detent as REL_WHEEL 1 and, on a device with a
 *  high-resolution wheel (touchpads, free-spinning and "smooth" mice), the
 *  SAME movement again as REL_WHEEL_HI_RES 120 - 120 units to the notch. Only
 *  REL_WHEEL was read here, so every high-resolution-only device scrolled
 *  nothing at all, and once both are read a device that sends both must not be
 *  counted twice or one notch becomes two.
 *
 *  So a report (everything up to the next SYN_REPORT) is the unit: if it
 *  carried hi-res units, they are the truth and the raw figure is that same
 *  motion rounded, so it is dropped; otherwise the raw notches are used. The
 *  sub-notch remainder of the hi-res total is carried into the next report, so
 *  three 40-unit reports are one notch and a slow roll is not swallowed.
 *
 *  Sign: evdev's REL_WHEEL is positive when the wheel turns away from the user
 *  (up), which is the sign this tree already uses everywhere - positive = up,
 *  in term_scroll() (+8 is Shift+PageUp), in app_wheel() and in the WM_WHEEL
 *  protocol ("positive = up"). No inversion here: inverting would turn the
 *  wheel upside down in every app at once.
 *
 *  REL_HWHEEL is deliberately not folded in: sideways wheel motion is not
 *  upward motion, and Input (src/mystic.h) has no field to carry it - one
 *  `int hwheel;` there plus a line here is the whole patch, and no app in this
 *  tree reads a horizontal wheel yet.
 * ------------------------------------------------------------ */
static int wheel_hi;        /* REL_WHEEL_HI_RES units in this report */
static int wheel_raw;       /* REL_WHEEL notches in this report */
static int wheel_have_hi;   /* the report carried hi-res units */
static int wheel_carry;     /* hi-res remainder not yet worth a notch */

static void wheel_report(Input *in) {
    if (wheel_have_hi) {
        wheel_carry += wheel_hi;
        int notches = wheel_carry / 120;    /* truncates towards 0, both signs */
        wheel_carry -= notches * 120;
        in->wheel += notches;
    } else {
        in->wheel += wheel_raw;
    }
    wheel_hi = 0;
    wheel_raw = 0;
    wheel_have_hi = 0;
}

/* Light the Caps Lock LED on the keyboard itself, so the key and its lamp
 * agree. An EV_LED event written to the evdev node is how the kernel is told
 * (the device must have been opened for writing; led_fd is -1 when it was
 * not). Failure is not fatal - a keyboard that cannot be written to still
 * types - so the result is ignored on purpose. */
static void caps_led(int on) {
    if (led_fd < 0) return;
    struct input_event led;
    memset(&led, 0, sizeof(led));
    led.type = EV_LED;
    led.code = LED_CAPSL;
    led.value = on ? 1 : 0;
    if (write(led_fd, &led, sizeof(led)) != (ssize_t)sizeof(led)) led_fd = -1;
}

static void handle_event(Input *in, int fd, struct input_event *ev,
                         int *shift, int *ctrl) {
    switch (ev->type) {
        case EV_SYN:
            /* end of one kernel report: settle the wheel and the contacts */
            if (ev->code == SYN_REPORT) {
                wheel_report(in);
                if (mt_accept(fd)) mt_packet(in);
            }
            break;
        case EV_REL:
            if (ev->code == REL_X) { in->mouse_dx += ev->value; in->mouse_x += ev->value; }
            if (ev->code == REL_Y) { in->mouse_dy += ev->value; in->mouse_y += ev->value; }
            if (ev->code == REL_WHEEL)             wheel_raw += ev->value;
            else if (ev->code == REL_WHEEL_HI_RES) { wheel_hi += ev->value; wheel_have_hi = 1; }
            break;
        case EV_ABS:
            if (ev->code == ABS_X) {
                in->mouse_x = (in->abs_max_x > 0 && in->scr_w > 0)
                            ? ev->value * in->scr_w / in->abs_max_x : ev->value;
                if (in->mouse_x >= in->scr_w && in->scr_w > 0) in->mouse_x = in->scr_w - 1;
            } else if (ev->code == ABS_Y) {
                in->mouse_y = (in->abs_max_y > 0 && in->scr_h > 0)
                            ? ev->value * in->scr_h / in->abs_max_y : ev->value;
                if (in->mouse_y >= in->scr_h && in->scr_h > 0) in->mouse_y = in->scr_h - 1;
            }
            else if (mt_accept(fd)) {
                /* Protocol B. A value is stored, not acted on: the packet is
                 * not complete until SYN_REPORT, and acting per event would
                 * compute a centroid from half a frame's contacts. */
                switch (ev->code) {
                case ABS_MT_SLOT:
                    mt.cur = ev->value;
                    if (mt.cur < 0) mt.cur = 0;
                    if (mt.cur >= MT_SLOTS) mt.cur = MT_SLOTS - 1;
                    break;
                case ABS_MT_TRACKING_ID:
                    /* -1 closes the slot; any other value is a new contact,
                     * which is what `fresh` tells the packet pass. */
                    if (ev->value < 0) {
                        mt.slot[mt.cur].used = 0;
                    } else {
                        mt.slot[mt.cur].used = 1;
                        mt.slot[mt.cur].fresh = 1;
                    }
                    break;
                case ABS_MT_POSITION_X: mt.slot[mt.cur].x = ev->value; break;
                case ABS_MT_POSITION_Y: mt.slot[mt.cur].y = ev->value; break;
                default: break;   /* pressure, touch size, orientation */
                }
            }
            break;
        case EV_KEY:
            if (ev->code == BTN_LEFT || ev->code == BTN_RIGHT || ev->code == BTN_MIDDLE) {
                int bit = 0;
                if (ev->code == BTN_LEFT) bit = 1;
                if (ev->code == BTN_RIGHT) bit = 2;
                if (ev->code == BTN_MIDDLE) bit = 4;
                if (ev->value == 1) {
                    in->buttons |= bit;
                    in->clicked |= bit;
                    if (bit == 1) {
                        /* double click: two presses close together in time and
                         * place, measured, not assumed */
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        double t = ts.tv_sec + ts.tv_nsec / 1e9;
                        int dx = in->mouse_x - last_x, dy = in->mouse_y - last_y;
                        if (dx < 0) dx = -dx;
                        if (dy < 0) dy = -dy;
                        if (t - last_t < 0.35 && dx < 6 && dy < 6) in->clicked2 = 1;
                        last_t = t;
                        last_x = in->mouse_x;
                        last_y = in->mouse_y;
                    }
                }
                else                { in->buttons &= ~bit; in->released |= bit; }
            } else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
                *shift = ev->value;
                in->shift = ev->value;
            } else if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
                *ctrl = ev->value;
                in->ctrl = ev->value;
            } else if (ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT) {
                in->alt = ev->value;
            } else if (mt_accept(fd) && ev->value != 2 &&
                       mt_tool_event(ev->code, ev->value)) {
                /* A touchpad's BTN_TOOL_* is a contact count or a tap signal,
                 * not something the user typed: consumed, never queued. */
                break;
            } else if (ev->code == KEY_CAPSLOCK) {
                /* A lock, not a character: it flips on the press edge, it is
                 * not put in keys[] (nothing may treat it as a shortcut) and
                 * its lamp is switched with it. Before this, Caps Lock was the
                 * one key on the board that did absolutely nothing. */
                if (ev->value == 1) {
                    caps_lock = !caps_lock;
                    caps_led(caps_lock);
                    in->key = ev->code;
                    in->key_pressed = 1;
                }
            } else {
                /* evdev values: 1 press, 2 kernel autorepeat (already
                 * rate-limited by the input device), 0 release. */
                if (ev->value == 2 && !key_repeats(ev->code)) break;
                if (ev->value == 1 || ev->value == 2) {
                    in->key = ev->code;
                    push_key(in->keys, &in->nkeys,
                             (int)(sizeof(in->keys) / sizeof(in->keys[0])), ev->code);
                    /* The character belongs to the PRESS edge: emitting it on
                     * release meant nothing echoed until the key came up, and
                     * sampling the modifier at release time turned
                     * Shift down, A down, Shift up, A up into 'a'. A kernel
                     * autorepeat (value 2) is a press and has to produce the
                     * same character as the press did, with the modifiers that
                     * are held now - that is what makes a held Shift+6 emit
                     * '^' over and over - so both values take this path. */
                    {
                        int ch = key_char(ev->code, *shift, *ctrl, caps_lock);
                        if (ch) push_char(in, (char)ch);
                        in->key_pressed = 1;
                    }
                } else if (ev->value == 0) {
                    /* releases matter: a client that never sees KEY ... 0
                     * thinks the key is still held */
                    push_key(in->keys_up, &in->nkeys_up,
                             (int)(sizeof(in->keys_up) / sizeof(in->keys_up[0])), ev->code);
                    in->shift = *shift;
                    in->ctrl = *ctrl;
                    in->key_pressed = 1;
                }
            }
            break;
        default:
            break;
    }
}

int input_poll(Input *in) {
    static int shift = 0, ctrl = 0;
    static int opened;
    int any = 0;
    struct input_event ev;

    if (!opened) { opened = 1; test_open(); }
    if (test_script && !test_eof) { test_step(in); any = 1; }

    for (int i = 0; i < nmouse; i++) {
        ssize_t got;
        /* EAGAIN is the normal end of the queue. ENODEV and EIO mean the
         * device was unplugged, which is the one way a finger can go on
         * being "down" for ever. */
        while ((got = read(mouse_fd[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            handle_event(in, mouse_fd[i], &ev, &shift, &ctrl);
            any = 1;
        }
        if (got < 0 && (errno == ENODEV || errno == EIO) &&
            mouse_fd[i] == mt.fd)
            mt_forget();
        /* Settle the wheel for THIS device before reading the next one: the
         * pending raw/hi-res figures belong to the stream just drained, and a
         * device that vanished mid-report never sends its SYN_REPORT. The
         * same is true of a half-delivered contact, so the packet is closed
         * here as well. */
        wheel_report(in);
        if (mt_accept(mouse_fd[i])) mt_packet(in);
    }
    for (int i = 0; i < nkeybd; i++) {
        /* A combo device sits in both lists; draining it twice would replay
         * nothing (the second read is EAGAIN) but it is clearer to skip it. */
        int already = 0;
        for (int j = 0; j < nmouse; j++)
            if (mouse_fd[j] == key_fd[i]) already = 1;
        if (already) continue;
        while (read(key_fd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, key_fd[i], &ev, &shift, &ctrl);
            any = 1;
        }
        wheel_report(in);
        if (mt_accept(key_fd[i])) mt_packet(in);
    }
    in->text[in->text_len] = 0;
    return any;
}

void input_frame(Input *in) {
    in->clicked = 0;
    in->released = 0;
    in->clicked2 = 0;
    in->wheel = 0;
    in->key = 0;
    in->key_pressed = 0;
    in->nkeys = 0;
    in->nkeys_up = 0;
    in->mouse_dx = 0;
    in->mouse_dy = 0;
    in->text_len = 0;
    in->text[0] = 0;
    /* Gesture OUTPUT is per frame; the gesture itself (mt.kind, mt.gid) lives
     * in the parser, so a slow frame does not restart what the fingers are
     * doing. gesture_id is not cleared: it only ever counts up. */
    in->gesture = GESTURE_NONE;
    in->gesture_fingers = 0;
    in->gesture_dx = 0;
    in->gesture_dy = 0;
    in->gesture_scale = 0.0f;
    in->gesture_release = 0;
    in->gesture_tap = 0;
    in->gesture_tap_fingers = 0;
    in->finger_n = 0;
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        in->finger_dx[i] = 0;
        in->finger_dy[i] = 0;
    }
}
