/* ============================================================
 *  Mystic OS - shortcuts.h
 *
 *  Every desktop key binding in one table, and one function that
 *  turns a key event into an action.
 *
 *  Why a table: the desktop grew a switch per feature - a block for
 *  Ctrl+Alt+L, another for Alt+Tab, another for F1..F5, another for
 *  Esc - so nobody could answer "what keys does this thing have?"
 *  without reading the whole event loop, and no two features could
 *  agree on what a modifier meant. Here a binding is one line:
 *
 *      ROW(SC_ACT_LOCK, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
 *          "Lock screen", SC_KEY_L)
 *
 *  main.c calls shortcuts_dispatch() once per key press, before it
 *  hands anything to a window, and uses the return value to decide
 *  whether the key was consumed. The action itself still lives in
 *  main.c (it owns the windows). This module has no globals and
 *  allocates nothing. The dispatcher needs only <string.h>; the panel
 *  below it draws with the gfx/font API that every other surface uses.
 *  Both parts run on the host: .work/keys/harness.c drives the
 *  dispatcher, and .work/keys/render.c links these sources with gfx.c,
 *  font.c, ui.c and theme.c and renders the panel to a PPM.
 *
 *  Matching is exact. A row matches when
 *    - mods equals the row's modifiers bit for bit (so Ctrl+M and
 *      Ctrl+Shift+M are different bindings, not a prefix match),
 *    - the key code is one of the row's keys,
 *    - and the row's context (cond) is present in the caller's
 *      context, or the row is unconditional.
 *  At most one row may match any (key, mods, cond) - the host test
 *  enumerates every combination and fails if that is not true.
 * ============================================================ */
#ifndef MYSTIC_SHORTCUTS_H
#define MYSTIC_SHORTCUTS_H

/* Surface, Theme, font_* and ui_px() - the panel below draws with them. */
#include "mystic.h"

/* ------------------------------------------------------------
 *  evdev key codes
 *
 *  These are the kernel's KEY_* values from linux/input-event-codes.h.
 *  They are repeated as an enum because the table has to build and be
 *  tested on the host too, where the Linux headers are not present.
 *  The evdev key ABI is frozen, and .work/keys/check_codes.c compiles
 *  one static assertion per code against <linux/input.h>, so a wrong
 *  number cannot pass unnoticed.
 * ------------------------------------------------------------ */
enum {
    SC_KEY_ESC   = 1,
    SC_KEY_1     = 2,
    SC_KEY_2     = 3,
    SC_KEY_3     = 4,
    SC_KEY_4     = 5,
    SC_KEY_5     = 6,
    SC_KEY_6     = 7,
    SC_KEY_7     = 8,
    SC_KEY_8     = 9,
    SC_KEY_9     = 10,
    SC_KEY_0     = 11,
    SC_KEY_MINUS = 12,
    SC_KEY_EQUAL = 13,
    SC_KEY_TAB   = 15,
    SC_KEY_Q     = 16,
    SC_KEY_W     = 17,
    SC_KEY_E     = 18,
    SC_KEY_R     = 19,
    SC_KEY_T     = 20,
    SC_KEY_I     = 23,
    SC_KEY_ENTER = 28,
    SC_KEY_L     = 38,
    SC_KEY_GRAVE = 41,
    SC_KEY_B     = 48,
    SC_KEY_N     = 49,
    SC_KEY_M     = 50,
    SC_KEY_SLASH = 53,
    SC_KEY_SPACE = 57,
    SC_KEY_F1    = 59,
    SC_KEY_F2    = 60,
    SC_KEY_F3    = 61,
    SC_KEY_F4    = 62,
    SC_KEY_F5    = 63,
    SC_KEY_F11   = 87,
    SC_KEY_SYSRQ = 99,
    SC_KEY_UP    = 103,
    SC_KEY_LEFT  = 105,
    SC_KEY_RIGHT = 106,
    SC_KEY_DOWN  = 108,
    SC_KEY_META  = 125
};

/* ------------------------------------------------------------
 *  Modifiers
 *
 *  Bit for bit, exactly as held. Super is the Meta key: the desktop
 *  tracks it across frames because input.c reports it as an ordinary
 *  key press and release.
 * ------------------------------------------------------------ */
#define SC_MOD_CTRL  0x01u
#define SC_MOD_ALT   0x02u
#define SC_MOD_SHIFT 0x04u
#define SC_MOD_SUPER 0x08u

/* ------------------------------------------------------------
 *  Context
 *
 *  A binding may only make sense in one situation. F1 opens a
 *  terminal when there is a window to come back to, and the shortcut
 *  list when there is not, so the two rows carry different contexts
 *  instead of one row guessing.
 * ------------------------------------------------------------ */
#define SC_WHEN_NO_WINDOW 0x01u   /* nothing focused: the desktop itself */
#define SC_WHEN_WINDOW    0x02u   /* a window is focused                */

/* ------------------------------------------------------------
 *  Actions
 *
 *  What a binding does. The dispatcher never performs the action -
 *  main.c does, because it owns the windows, the theme and the
 *  framebuffer - so this list is the contract between the table and
 *  the switch that implements it. Adding a binding without adding
 *  its case is a compiler warning (the switch has no default) and a
 *  runtime message, not a silently dead key.
 * ------------------------------------------------------------ */
enum {
    SC_ACT_NONE = 0,
    /* window */
    SC_ACT_WIN_CLOSE,
    SC_ACT_WIN_MIN,
    SC_ACT_WIN_MAX,
    SC_ACT_WIN_FULL,
    SC_ACT_WIN_RESTORE,
    SC_ACT_WIN_NEXT,
    SC_ACT_WIN_PREV,
    SC_ACT_WIN_SAME_APP,
    SC_ACT_WIN_MOVE,
    SC_ACT_WIN_RESIZE,
    SC_ACT_WIN_SNAP,
    /* desktop */
    SC_ACT_LAUNCHER,
    SC_ACT_LOCK,
    SC_ACT_THEME_NEXT,
    SC_ACT_THEME_SET,
    SC_ACT_WALLPAPER,
    SC_ACT_TEXT_UP,
    SC_ACT_TEXT_DOWN,
    SC_ACT_TEXT_RESET,
    SC_ACT_DND,
    SC_ACT_SHOT,
    /* apps */
    SC_ACT_TERMINAL,
    SC_ACT_FILES,
    SC_ACT_SETTINGS,
    SC_ACT_MONITOR,
    SC_ACT_NOTES,
    SC_ACT_BROWSER,
    /* help */
    SC_ACT_HELP,
    SC_ACT_COUNT
};

/* Category order is the order the overlay lists them in. */
enum {
    SC_CAT_WINDOW = 0,
    SC_CAT_DESKTOP,
    SC_CAT_APPS,
    SC_CAT_HELP,
    SC_CAT_COUNT
};

/* A row can bind several keys to one action (Ctrl+W and Ctrl+Q both
 * close; the four arrows all move) - nine is what the theme digits
 * need, which is the widest row in the table. */
#define SC_MAX_KEYS 9

/* Row flags. SC_ALWAYS is for the one binding that acted through a modal
 * overlay before there was a table: Alt+Tab switched windows even with the
 * launcher open, and it still does. Everything else waits for the overlay
 * to close, which is what "modal" means. */
#define SC_ALWAYS 0x01u

typedef struct {
    unsigned char  id;            /* SC_ACT_*                          */
    unsigned char  cat;           /* SC_CAT_*                          */
    unsigned char  nkeys;         /* entries used in keys[]            */
    unsigned char  flags;         /* SC_ALWAYS, or 0                   */
    unsigned short mods;          /* SC_MOD_* required, exact          */
    unsigned short cond;          /* SC_WHEN_* required, 0 = any       */
    int            keys[SC_MAX_KEYS];
    const char    *desc;          /* one line, shown in the overlay    */
} Shortcut;

/* The host's action. `ctx` is the caller's pointer (main.c passes NULL,
 * the host test passes a recorder), `key` is which of the row's keys was
 * pressed (the arrow bindings need it) and `mods` is what was held. */
typedef void (*ShortcutFn)(void *ctx, int id, int key, unsigned mods);

typedef struct {
    ShortcutFn action;
    void       *ctx;
} ShortcutActions;

/* ------------------------------------------------------------
 *  Dispatch
 *
 *  Returns 1 when the key belongs to a binding (the caller must not
 *  pass it on to a window) and 0 when it does not. The action is
 *  called with the row's id before this returns.
 * ------------------------------------------------------------ */
int shortcuts_dispatch(const ShortcutActions *actions, int key,
                       unsigned mods, unsigned cond);

/* The row that owns this event, or NULL. Used by the dispatcher and by
 * the host test that proves no two rows claim the same event. */
const Shortcut *shortcuts_find(int key, unsigned mods, unsigned cond);

/* ------------------------------------------------------------
 *  Reading the table (the overlay draws from these)
 * ------------------------------------------------------------ */
int shortcuts_count(void);
const Shortcut *shortcuts_at(int index);
const char *shortcuts_category_name(int cat);

/* "Ctrl+Alt+W", "Ctrl+Alt+1..9", "Super / Super+Space". Generated from
 * the row's own keys and modifiers, so the documented chord cannot
 * drift away from the binding it describes. Returns the length that
 * was written, like snprintf. */
int shortcuts_chord(const Shortcut *s, char *out, int cap);
const char *shortcuts_key_name(int key);

/* ------------------------------------------------------------
 *  The panel
 *
 *  Draws every binding, grouped by category, into the middle of the
 *  surface: a dimmed backdrop, a rounded panel in the theme's colours,
 *  one to three columns (as many as the list needs and the screen can
 *  carry), rows measured from the font and spacing from ui_px().
 *
 *  `top_inset` is the desktop's top bar height, so the panel never
 *  covers it. The whole panel is clipped to itself, which is the same
 *  discipline an app window gets: a long string cannot paint outside
 *  the surface it belongs to.
 *
 *  It lives here rather than in main.c so the list that is drawn and
 *  the list that is dispatched can only ever be the same list - and so
 *  this can be rendered and compared against the code on the host.
 * ------------------------------------------------------------ */
void shortcuts_draw_panel(Surface *s, const Theme *th, int top_inset);

#endif /* MYSTIC_SHORTCUTS_H */
