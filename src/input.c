/* ============================================================
 *  Mystic OS - input.c
 *  Mouse and keyboard, straight from the kernel's event devices.
 *
 *  Every axis, button and key code here is a real evdev code; there
 *  is no simulation. We only need to know which /dev/input/eventN is
 *  a mouse and which is a keyboard, and that we ask the device.
 * ============================================================ */
#include "mystic.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
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

        int is_mouse = has_bit(fd, EV_REL, REL_X) || has_bit(fd, EV_ABS, ABS_X);
        int is_key   = has_bit(fd, EV_KEY, KEY_A) || has_bit(fd, EV_KEY, KEY_SPACE);
        int kept = 0;

        if (is_mouse && nmouse < MAX_POINTERS) {
            mouse_fd[nmouse++] = fd;
            if (!strcmp(mouse_name, "none"))
                snprintf(mouse_name, sizeof(mouse_name), "%s", name);
            fprintf(stderr, "input: pointer  %s (%s)\n", path, name);
            kept = 1;
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
    fprintf(stderr, "input: mouse=%s keyboard=%s (%d pointer, %d keyboard)\n",
            mouse_name, key_name, nmouse, nkeybd);
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

static void handle_event(Input *in, struct input_event *ev, int *shift, int *ctrl) {
    switch (ev->type) {
        case EV_SYN:
            /* end of one kernel report: settle the wheel */
            if (ev->code == SYN_REPORT) wheel_report(in);
            break;
        case EV_REL:
            if (ev->code == REL_X) { in->mouse_dx += ev->value; in->mouse_x += ev->value; }
            if (ev->code == REL_Y) { in->mouse_dy += ev->value; in->mouse_y += ev->value; }
            if (ev->code == REL_WHEEL)             wheel_raw += ev->value;
            else if (ev->code == REL_WHEEL_HI_RES) { wheel_hi += ev->value; wheel_have_hi = 1; }
            break;
        case EV_ABS:
            if (ev->code == ABS_X) in->mouse_x = ev->value;
            if (ev->code == ABS_Y) in->mouse_y = ev->value;
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
    int any = 0;
    struct input_event ev;

    for (int i = 0; i < nmouse; i++) {
        while (read(mouse_fd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift, &ctrl);
            any = 1;
        }
        /* Settle the wheel for THIS device before reading the next one: the
         * pending raw/hi-res figures belong to the stream just drained, and a
         * device that vanished mid-report never sends its SYN_REPORT. */
        wheel_report(in);
    }
    for (int i = 0; i < nkeybd; i++) {
        /* A combo device sits in both lists; draining it twice would replay
         * nothing (the second read is EAGAIN) but it is clearer to skip it. */
        int already = 0;
        for (int j = 0; j < nmouse; j++)
            if (mouse_fd[j] == key_fd[i]) already = 1;
        if (already) continue;
        while (read(key_fd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift, &ctrl);
            any = 1;
        }
        wheel_report(in);
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
}
