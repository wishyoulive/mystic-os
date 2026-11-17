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

#define MAX_FDS 8

static double last_t = -1.0;
static int    last_x, last_y;

static int  mouse_fd = -1;
static int  key_fd = -1;
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
    for (int i = 0; i < MAX_FDS; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

        int is_mouse = has_bit(fd, EV_REL, REL_X) || has_bit(fd, EV_ABS, ABS_X);
        int is_key   = has_bit(fd, EV_KEY, KEY_A) || has_bit(fd, EV_KEY, KEY_SPACE);

        if (is_mouse && mouse_fd < 0) {
            mouse_fd = fd;
            snprintf(mouse_name, sizeof(mouse_name), "%s", name);
            fprintf(stderr, "input: mouse    %s\n", name);
        } else if (is_key && key_fd < 0) {
            key_fd = fd;
            snprintf(key_name, sizeof(key_name), "%s", name);
            fprintf(stderr, "input: keyboard %s\n", name);
        } else {
            fprintf(stderr, "input: ignoring %s (%s)\n", path, name);
            close(fd);
        }
    }
    fprintf(stderr, "input: mouse=%s keyboard=%s\n", mouse_name, key_name);
    return (mouse_fd >= 0 || key_fd >= 0) ? 0 : -1;
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
};

static int key_to_char(int code, int shift) {
    switch (code) {
        case KEY_SPACE:     return ' ';
        case KEY_ENTER:     return '\n';
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB:       return '\t';
        default: break;
    }
    for (size_t i = 0; i < sizeof(KEYMAP) / sizeof(KEYMAP[0]); i++)
        if (KEYMAP[i].code == code)
            return (unsigned char)(shift ? KEYMAP[i].hi : KEYMAP[i].lo);
    return 0;
}

/* The character a key produces, after the Ctrl+letter -> control-code rule
 * (Ctrl+S is 0x13). Used for both the release edge and autorepeat. */
static int key_char(int code, int shift, int ctrl) {
    int ch = key_to_char(code, shift);
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
    if (key_to_char(code, 0) || key_to_char(code, 1)) return 1;
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

static void handle_event(Input *in, struct input_event *ev, int *shift, int *ctrl) {
    switch (ev->type) {
        case EV_REL:
            if (ev->code == REL_X) { in->mouse_dx += ev->value; in->mouse_x += ev->value; }
            if (ev->code == REL_Y) { in->mouse_dy += ev->value; in->mouse_y += ev->value; }
            if (ev->code == REL_WHEEL) in->wheel += ev->value;
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
            } else {
                /* evdev values: 1 press, 2 kernel autorepeat (already
                 * rate-limited by the input device), 0 release. */
                if (ev->value == 2 && !key_repeats(ev->code)) break;
                if (ev->value == 1 || ev->value == 2) {
                    in->key = ev->code;
                    push_key(in->keys, &in->nkeys,
                             (int)(sizeof(in->keys) / sizeof(in->keys[0])), ev->code);
                    /* A repeat is a press as far as keys[] is concerned, so
                     * holding an arrow keeps walking. This layer hands the
                     * printable character over on the release edge, so a
                     * repeat must carry it too or holding Backspace would
                     * still delete exactly one character. */
                    {
                        /* the character belongs to the PRESS edge: emitting it
                         * on release meant nothing echoed until the key came
                         * up, and the modifier was sampled at release time so
                         * Shift down, A down, Shift up, A up gave 'a' not 'A' */
                        int ch = key_char(ev->code, *shift, *ctrl);
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

    if (mouse_fd >= 0) {
        while (read(mouse_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift, &ctrl);
            any = 1;
        }
    }
    if (key_fd >= 0) {
        while (read(key_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift, &ctrl);
            any = 1;
        }
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
