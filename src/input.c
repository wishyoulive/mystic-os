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
