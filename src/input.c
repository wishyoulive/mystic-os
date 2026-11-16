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
#include <unistd.h>

#define MAX_FDS 8

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

/* US keyboard, keycode -> character (lower case + shift) */
static int key_to_char(int code, int shift) {
    static const char plain[] = "1234567890-=  qwertyuiop[]  asdfghjkl;'`  zxcvbnm,./";
    static const char upper[] = "!@#$%^&*()_+  QWERTYUIOP{}  ASDFGHJKL:\"~  ZXCVBNM<>?";
    switch (code) {
        case KEY_SPACE: return ' ';
        case KEY_ENTER: return '\n';
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB: return '\t';
        default: break;
    }
    const int map[][2] = {
        {KEY_1,0},{KEY_2,1},{KEY_3,2},{KEY_4,3},{KEY_5,4},{KEY_6,5},{KEY_7,6},{KEY_8,7},
        {KEY_9,8},{KEY_0,9},{KEY_MINUS,10},{KEY_EQUAL,11},{KEY_Q,13},{KEY_W,14},{KEY_E,15},
        {KEY_R,16},{KEY_T,17},{KEY_Y,18},{KEY_U,19},{KEY_I,20},{KEY_O,21},{KEY_P,22},
        {KEY_LEFTBRACE,23},{KEY_RIGHTBRACE,24},{KEY_A,27},{KEY_S,28},{KEY_D,29},{KEY_F,30},
        {KEY_G,31},{KEY_H,32},{KEY_J,33},{KEY_K,34},{KEY_L,35},{KEY_SEMICOLON,36},
        {KEY_APOSTROPHE,37},{KEY_GRAVE,38},{KEY_BACKSLASH,39},{KEY_Z,41},{KEY_X,42},
        {KEY_C,43},{KEY_V,44},{KEY_B,45},{KEY_N,46},{KEY_M,47},{KEY_COMMA,48},
        {KEY_DOT,49},{KEY_SLASH,50},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i][0] == code) {
            int idx = map[i][1];
            return (unsigned char)(shift ? upper[idx] : plain[idx]);
        }
    return 0;
}

static void handle_event(Input *in, struct input_event *ev, int *shift) {
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
                if (ev->value == 1) { in->buttons |= bit; in->clicked |= bit; }
                else                { in->buttons &= ~bit; in->released |= bit; }
            } else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
                *shift = ev->value;
            } else {
                if (ev->value == 1) {
                    in->key = ev->code;
                    in->key_pressed = 1;
                    int ch = key_to_char(ev->code, *shift);
                    if (ch && in->text_len < (int)sizeof(in->text) - 1)
                        in->text[in->text_len++] = (char)ch;
                }
            }
            break;
        default:
            break;
    }
}

int input_poll(Input *in) {
    static int shift = 0;
    int any = 0;
    struct input_event ev;

    if (mouse_fd >= 0) {
        while (read(mouse_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift);
            any = 1;
        }
    }
    if (key_fd >= 0) {
        while (read(key_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(in, &ev, &shift);
            any = 1;
        }
    }
    in->text[in->text_len] = 0;
    return any;
}

void input_frame(Input *in) {
    in->clicked = 0;
    in->released = 0;
    in->wheel = 0;
    in->key = 0;
    in->key_pressed = 0;
    in->mouse_dx = 0;
    in->mouse_dy = 0;
    in->text_len = 0;
    in->text[0] = 0;
}
