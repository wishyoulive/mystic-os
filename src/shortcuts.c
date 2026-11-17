/* ============================================================
 *  Mystic OS - shortcuts.c
 *
 *  The binding table, the dispatcher, the cancel chain and the panel.
 *  Read shortcuts.h first: it explains the matching rule, the modal
 *  rule and the action contract.
 *
 *  Nothing in the dispatcher touches the screen, the filesystem or a
 *  window: it is a pure function from (key, modifiers, context, modals)
 *  to a row of the table, plus the host callback that main.c supplies.
 *  That is what makes it testable without a framebuffer, and it is why
 *  the overlay can be drawn from the same rows the dispatcher uses -
 *  one table, one answer to "what does this key do?".
 * ============================================================ */
#include "shortcuts.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ the rows
 *
 * One row per binding. Several keys may share a row when they are the
 * same binding with the same modifiers (Ctrl+W and Ctrl+Q close), and
 * the same action appears again only where the chord differs in kind
 * (F11 and Super+F both toggle full screen).
 *
 * `code` is the binding's own name and must be unique across the whole
 * table: two rows may share an action, but no two rows are the same
 * binding. The host test checks it, and it is what a future settings
 * screen would persist a rebind under.
 *
 * The categories are in SC_CAT_* order and the overlay relies on that,
 * so a new row belongs inside its block, not at the end of the file.
 */
#define ROW_F(code, id, cat, flags, mods, cond, desc, ...)                 \
    { (code), (unsigned char)(id), (unsigned char)(cat),                   \
      (unsigned char)(sizeof((int[]){ __VA_ARGS__ }) / sizeof(int)),       \
      (unsigned char)(flags),                                              \
      (unsigned short)(mods), (unsigned short)(cond), { __VA_ARGS__ },    \
      (desc) }
/* an ordinary binding */
#define ROW(code, id, cat, mods, cond, desc, ...)                          \
    ROW_F((code), (id), (cat), 0, (mods), (cond), (desc), __VA_ARGS__)
/* one that also works while a desktop modal is up (see SC_ALWAYS) */
#define ROW_ALWAYS(code, id, cat, mods, cond, desc, ...)                   \
    ROW_F((code), (id), (cat), SC_ALWAYS, (mods), (cond), (desc), __VA_ARGS__)

static const Shortcut TABLE[] = {
    /* ------------------------------------------------ window */
    ROW("win.close", SC_ACT_WIN_CLOSE, SC_CAT_WINDOW, SC_MOD_CTRL, 0,
        "Close window", SC_KEY_W, SC_KEY_Q),
    ROW("win.min", SC_ACT_WIN_MIN, SC_CAT_WINDOW, SC_MOD_CTRL, 0,
        "Minimise window", SC_KEY_M),
    ROW("win.max", SC_ACT_WIN_MAX, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Maximise / restore", SC_KEY_M),
    ROW("win.full", SC_ACT_WIN_FULL, SC_CAT_WINDOW, 0, 0,
        "Full screen", SC_KEY_F11),
    ROW("win.full.f", SC_ACT_WIN_FULL, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Full screen", SC_KEY_F),
    ROW("win.restore", SC_ACT_WIN_RESTORE, SC_CAT_WINDOW,
        SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Restore minimised", SC_KEY_R),
    /* Alt+Tab is the one pair that acted through the launcher overlay
     * before this table existed; SC_ALWAYS keeps that true. */
    ROW_ALWAYS("win.next", SC_ACT_WIN_NEXT, SC_CAT_WINDOW, SC_MOD_ALT, 0,
               "Next window", SC_KEY_TAB),
    ROW_ALWAYS("win.prev", SC_ACT_WIN_PREV, SC_CAT_WINDOW,
               SC_MOD_ALT | SC_MOD_SHIFT, 0,
               "Previous window", SC_KEY_TAB),
    ROW("win.same", SC_ACT_WIN_SAME_APP, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Cycle app windows", SC_KEY_GRAVE),
    ROW("win.move", SC_ACT_WIN_MOVE, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Move window", SC_KEY_LEFT, SC_KEY_RIGHT,
        SC_KEY_UP, SC_KEY_DOWN),
    ROW("win.resize", SC_ACT_WIN_RESIZE, SC_CAT_WINDOW,
        SC_MOD_CTRL | SC_MOD_ALT | SC_MOD_SHIFT, 0,
        "Resize window", SC_KEY_LEFT, SC_KEY_RIGHT,
        SC_KEY_UP, SC_KEY_DOWN),
    ROW("win.snap", SC_ACT_WIN_SNAP, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Snap left / right / up", SC_KEY_LEFT, SC_KEY_RIGHT, SC_KEY_UP),
    ROW("win.edge", SC_ACT_WIN_EDGE, SC_CAT_WINDOW,
        SC_MOD_SUPER | SC_MOD_SHIFT, 0,
        "Snap to edge", SC_KEY_LEFT, SC_KEY_RIGHT,
        SC_KEY_UP, SC_KEY_DOWN),
    ROW("win.grow", SC_ACT_WIN_GROW, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Grow window", SC_KEY_EQUAL),
    ROW("win.shrink", SC_ACT_WIN_SHRINK, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Shrink window", SC_KEY_MINUS),
    ROW("win.centre", SC_ACT_WIN_CENTRE, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Centre window", SC_KEY_C),
    ROW("win.raise", SC_ACT_WIN_RAISE, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Raise / lower window", SC_KEY_R),
    ROW("win.ontop", SC_ACT_WIN_ONTOP, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Always on top", SC_KEY_T),
    ROW("win.focus", SC_ACT_WIN_FOCUS, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Focus window by dock number", SC_KEY_1, SC_KEY_2, SC_KEY_3,
        SC_KEY_4, SC_KEY_5, SC_KEY_6, SC_KEY_7, SC_KEY_8, SC_KEY_9),
    ROW("win.minall", SC_ACT_WIN_MIN_ALL, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Minimise all windows", SC_KEY_D),
    ROW("win.restoreall", SC_ACT_WIN_RESTORE_ALL, SC_CAT_WINDOW,
        SC_MOD_SUPER | SC_MOD_SHIFT, 0,
        "Restore all windows", SC_KEY_D),
    ROW("win.closeall", SC_ACT_WIN_CLOSE_ALL, SC_CAT_WINDOW,
        SC_MOD_SUPER | SC_MOD_SHIFT, 0,
        "Close all windows", SC_KEY_W),

    /* ------------------------------------------------ desktop */
    ROW("desk.launcher", SC_ACT_LAUNCHER, SC_CAT_DESKTOP, SC_MOD_SUPER, 0,
        "Launcher (Super)", SC_KEY_META, SC_KEY_SPACE),
    ROW("desk.lock", SC_ACT_LOCK, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Lock screen", SC_KEY_L),
    ROW("desk.theme.next", SC_ACT_THEME_NEXT, SC_CAT_DESKTOP,
        SC_MOD_CTRL | SC_MOD_ALT, 0, "Next theme", SC_KEY_T),
    ROW("desk.theme.prev", SC_ACT_THEME_PREV, SC_CAT_DESKTOP,
        SC_MOD_CTRL | SC_MOD_ALT | SC_MOD_SHIFT, 0,
        "Previous theme", SC_KEY_T),
    ROW("desk.theme.set", SC_ACT_THEME_SET, SC_CAT_DESKTOP,
        SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Theme by number", SC_KEY_1, SC_KEY_2, SC_KEY_3,
        SC_KEY_4, SC_KEY_5, SC_KEY_6, SC_KEY_7, SC_KEY_8, SC_KEY_9),
    ROW("desk.wall.next", SC_ACT_WALLPAPER_NEXT, SC_CAT_DESKTOP,
        SC_MOD_CTRL | SC_MOD_ALT, 0, "Next wallpaper", SC_KEY_W),
    ROW("desk.wall.prev", SC_ACT_WALLPAPER_PREV, SC_CAT_DESKTOP,
        SC_MOD_CTRL | SC_MOD_ALT | SC_MOD_SHIFT, 0,
        "Previous wallpaper", SC_KEY_W),
    ROW("desk.text.up", SC_ACT_TEXT_UP, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text larger", SC_KEY_EQUAL),
    ROW("desk.text.down", SC_ACT_TEXT_DOWN, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text smaller", SC_KEY_MINUS),
    ROW("desk.text.reset", SC_ACT_TEXT_RESET, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text normal", SC_KEY_0),
    ROW("desk.dnd", SC_ACT_DND, SC_CAT_DESKTOP, SC_MOD_SUPER, 0,
        "Do not disturb", SC_KEY_N),
    ROW("desk.shot", SC_ACT_SHOT, SC_CAT_DESKTOP, 0, 0,
        "Screenshot screen to /root", SC_KEY_SYSRQ),
    ROW("desk.shot.win", SC_ACT_SHOT_WINDOW, SC_CAT_DESKTOP, SC_MOD_ALT, 0,
        "Screenshot focused window", SC_KEY_SYSRQ),

    /* ------------------------------------------------ apps */
    ROW("app.term", SC_ACT_TERMINAL, SC_CAT_APPS, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Terminal", SC_KEY_ENTER),
    /* F1 opens a terminal, except with no window at all - then it opens
     * the shortcut list. The two rows are told apart by their context,
     * not by the dispatcher guessing from the key. */
    ROW("app.term.f1", SC_ACT_TERMINAL, SC_CAT_APPS, 0, SC_WHEN_WINDOW,
        "Terminal", SC_KEY_F1),
    ROW("app.files", SC_ACT_FILES, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Files", SC_KEY_E),
    ROW("app.files.f2", SC_ACT_FILES, SC_CAT_APPS, 0, 0,
        "Files", SC_KEY_F2),
    ROW("app.settings", SC_ACT_SETTINGS, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Settings", SC_KEY_I),
    ROW("app.settings.f3", SC_ACT_SETTINGS, SC_CAT_APPS, 0, 0,
        "Settings", SC_KEY_F3),
    ROW("app.monitor", SC_ACT_MONITOR, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Monitor", SC_KEY_M),
    ROW("app.monitor.f4", SC_ACT_MONITOR, SC_CAT_APPS, 0, 0,
        "Monitor", SC_KEY_F4),
    ROW("app.notes", SC_ACT_NOTES, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Notes", SC_KEY_O),
    ROW("app.notes.f5", SC_ACT_NOTES, SC_CAT_APPS, 0, 0,
        "Notes", SC_KEY_F5),
    ROW("app.store", SC_ACT_STORE, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "App store", SC_KEY_S),
    ROW("app.store.f7", SC_ACT_STORE, SC_CAT_APPS, 0, 0,
        "App store", SC_KEY_F7),
    ROW("app.calc", SC_ACT_CALC, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Calculator", SC_KEY_K),
    ROW("app.calc.f6", SC_ACT_CALC, SC_CAT_APPS, 0, 0,
        "Calculator", SC_KEY_F6),
    ROW("app.browser", SC_ACT_BROWSER, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Browser", SC_KEY_B),
    ROW("app.browser.f8", SC_ACT_BROWSER, SC_CAT_APPS, 0, 0,
        "Browser", SC_KEY_F8),
    ROW("app.sysinfo", SC_ACT_SYSINFO, SC_CAT_APPS, 0, 0,
        "System info", SC_KEY_F9),

    /* ------------------------------------------------ browser */
    ROW("web.reload", SC_ACT_BROWSER_RELOAD, SC_CAT_BROWSER, SC_MOD_CTRL, 0,
        "Reload page", SC_KEY_R),
    ROW("web.addr", SC_ACT_BROWSER_ADDR, SC_CAT_BROWSER, SC_MOD_ALT, 0,
        "Focus address bar", SC_KEY_D),

    /* ------------------------------------------------ terminal */
    ROW("term.clear", SC_ACT_TERM_CLEAR, SC_CAT_TERMINAL, SC_MOD_CTRL, 0,
        "Clear scrollback", SC_KEY_L),
    ROW("term.copy", SC_ACT_TERM_COPY, SC_CAT_TERMINAL,
        SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Copy typed line", SC_KEY_C),
    ROW("term.paste", SC_ACT_TERM_PASTE, SC_CAT_TERMINAL,
        SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Paste clipboard", SC_KEY_V),
    ROW("term.font.up", SC_ACT_TERM_FONT_UP, SC_CAT_TERMINAL,
        SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Bigger terminal font", SC_KEY_EQUAL),
    ROW("term.font.down", SC_ACT_TERM_FONT_DOWN, SC_CAT_TERMINAL,
        SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Smaller terminal font", SC_KEY_MINUS),

    /* ------------------------------------------------ help */
    ROW("help.panel", SC_ACT_HELP, SC_CAT_HELP, SC_MOD_SUPER, 0,
        "This shortcut list", SC_KEY_SLASH),
    ROW("help.panel.f1", SC_ACT_HELP, SC_CAT_HELP, 0, SC_WHEN_NO_WINDOW,
        "This shortcut list", SC_KEY_F1),
    /* Esc never closes a window. It is the last row for a reason: every
     * other binding is an operation, this one undoes whatever is open. */
    ROW_ALWAYS("help.cancel", SC_ACT_CANCEL, SC_CAT_HELP, 0, 0,
               "Cancel / dismiss", SC_KEY_ESC)
};

#define TABLE_N ((int)(sizeof(TABLE) / sizeof(TABLE[0])))

int shortcuts_count(void) { return TABLE_N; }

const Shortcut *shortcuts_at(int index) {
    if (index < 0 || index >= TABLE_N) return NULL;
    return &TABLE[index];
}

static const char *const CATEGORY_NAME[SC_CAT_COUNT] = {
    "Window", "Desktop", "Apps", "Browser", "Terminal", "Help"
};

const char *shortcuts_category_name(int cat) {
    if (cat < 0 || cat >= SC_CAT_COUNT) return "Other";
    return CATEGORY_NAME[cat];
}

/* ------------------------------------------------------------ dispatch */

const Shortcut *shortcuts_find(int key, unsigned mods, unsigned cond,
                               unsigned modals) {
    for (int i = 0; i < TABLE_N; i++) {
        const Shortcut *s = &TABLE[i];
        /* a desktop overlay owns the keyboard: only the rows that are
         * explicitly allowed through may act while one is up */
        if (modals && !(s->flags & SC_ALWAYS)) continue;
        if ((unsigned)s->mods != mods) continue;
        if (s->cond && !(s->cond & cond)) continue;
        for (int k = 0; k < (int)s->nkeys; k++)
            if (s->keys[k] == key) return s;
    }
    return NULL;
}

int shortcuts_dispatch(const ShortcutActions *actions, int key,
                       unsigned mods, unsigned cond, unsigned modals) {
    const Shortcut *s = shortcuts_find(key, mods, cond, modals);
    if (!s) return 0;
    if (actions && actions->action)
        actions->action(actions->ctx, s->id, key, mods);
    return 1;
}

/* ------------------------------------------------------------ cancel */

int shortcuts_cancel_target(unsigned modals) {
    /* The order of these tests is the priority documented in the header.
     * A drag outranks a resize because both cannot be active at once, and
     * outranks the app because the title bar already has the pointer. */
    if (modals & SC_MODAL_HELP)     return SC_MODAL_HELP;
    if (modals & SC_MODAL_LAUNCHER) return SC_MODAL_LAUNCHER;
    if (modals & SC_MODAL_MENU)     return SC_MODAL_MENU;
    if (modals & SC_MODAL_DRAG)     return SC_MODAL_DRAG;
    if (modals & SC_MODAL_RESIZE)   return SC_MODAL_RESIZE;
    if (modals & SC_MODAL_APP)      return SC_MODAL_APP;
    return 0;
}

const char *shortcuts_cancel_name(int target) {
    switch (target) {
    case SC_MODAL_HELP:     return "shortcuts";
    case SC_MODAL_LAUNCHER: return "launcher";
    case SC_MODAL_MENU:     return "menu";
    case SC_MODAL_DRAG:     return "move";
    case SC_MODAL_RESIZE:   return "resize";
    case SC_MODAL_APP:      return "app";
    default:                return NULL;
    }
}

/* ------------------------------------------------------------ names
 *
 * Designated initialisers, so the name of a key sits at the key's own
 * number and a reader can check it against the evdev list without
 * counting rows. Anything not named here is not in the table either,
 * and reports "?" rather than an invented label.
 */
static const char *const KEY_NAME[128] = {
    [SC_KEY_ESC]    = "Esc",
    [SC_KEY_1]      = "1",
    [SC_KEY_2]      = "2",
    [SC_KEY_3]      = "3",
    [SC_KEY_4]      = "4",
    [SC_KEY_5]      = "5",
    [SC_KEY_6]      = "6",
    [SC_KEY_7]      = "7",
    [SC_KEY_8]      = "8",
    [SC_KEY_9]      = "9",
    [SC_KEY_0]      = "0",
    [SC_KEY_MINUS]  = "-",
    [SC_KEY_EQUAL]  = "=",
    [SC_KEY_TAB]    = "Tab",
    [SC_KEY_Q]      = "Q",
    [SC_KEY_W]      = "W",
    [SC_KEY_E]      = "E",
    [SC_KEY_R]      = "R",
    [SC_KEY_T]      = "T",
    [SC_KEY_I]      = "I",
    [SC_KEY_O]      = "O",
    [SC_KEY_ENTER]  = "Enter",
    [SC_KEY_D]      = "D",
    [SC_KEY_F]      = "F",
    [SC_KEY_K]      = "K",
    [SC_KEY_L]      = "L",
    [SC_KEY_S]      = "S",
    [SC_KEY_GRAVE]  = "`",
    [SC_KEY_C]      = "C",
    [SC_KEY_V]      = "V",
    [SC_KEY_B]      = "B",
    [SC_KEY_N]      = "N",
    [SC_KEY_M]      = "M",
    [SC_KEY_SLASH]  = "/",
    [SC_KEY_SPACE]  = "Space",
    [SC_KEY_F1]     = "F1",
    [SC_KEY_F2]     = "F2",
    [SC_KEY_F3]     = "F3",
    [SC_KEY_F4]     = "F4",
    [SC_KEY_F5]     = "F5",
    [SC_KEY_F6]     = "F6",
    [SC_KEY_F7]     = "F7",
    [SC_KEY_F8]     = "F8",
    [SC_KEY_F9]     = "F9",
    [SC_KEY_F11]    = "F11",
    [SC_KEY_SYSRQ]  = "Print",
    [SC_KEY_HOME]   = "Home",
    [SC_KEY_UP]     = "\xE2\x86\x91",       /* up arrow       */
    [SC_KEY_PAGEUP] = "PgUp",
    [SC_KEY_LEFT]   = "\xE2\x86\x90",       /* left arrow     */
    [SC_KEY_RIGHT]  = "\xE2\x86\x92",       /* right arrow    */
    [SC_KEY_END]    = "End",
    [SC_KEY_DOWN]   = "\xE2\x86\x93",       /* down arrow     */
    [SC_KEY_PAGEDOWN] = "PgDn",
    [SC_KEY_META]   = "Super"
};

const char *shortcuts_key_name(int key) {
    if (key > 0 && key < (int)(sizeof(KEY_NAME) / sizeof(KEY_NAME[0])) && KEY_NAME[key])
        return KEY_NAME[key];
    return "?";
}

/* One chord, as text: "Ctrl+Alt+W", "Ctrl+Alt+1..9", "Super / Super+Space".
 *
 * Generated from the row instead of stored beside it. A hand-written
 * label is the classic way for documentation to start lying: change a
 * key code and the label keeps describing the old one. */
int shortcuts_chord(const Shortcut *s, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!s || s->nkeys == 0) return 0;

    char buf[192];
    int n = 0;
#define PUT(ch)   do { if (n < (int)sizeof(buf) - 1) buf[n++] = (char)(ch); } while (0)
#define PUTS(str) do { const char *p_ = (str); while (*p_) PUT(*p_++); } while (0)

    /* the modifier prefix, in the order a person says it */
    char pre[32];
    int pn = 0;
    static const struct { unsigned bit; const char *name; } MODS[] = {
        /* the order a person says it, with the desktop's own modifier first:
         * "Super+Shift+D", not "Shift+Super+D" */
        { SC_MOD_SUPER, "Super" }, { SC_MOD_CTRL, "Ctrl" },
        { SC_MOD_ALT, "Alt" }, { SC_MOD_SHIFT, "Shift" }
    };
    for (unsigned i = 0; i < sizeof(MODS) / sizeof(MODS[0]); i++) {
        if (!(s->mods & MODS[i].bit)) continue;
        if (pn && pn < (int)sizeof(pre) - 1) pre[pn++] = '+';
        for (const char *c = MODS[i].name; *c && pn < (int)sizeof(pre) - 1; c++)
            pre[pn++] = *c;
    }
    pre[pn] = 0;

    /* A run of consecutive digits is a range, not nine lines: the theme
     * row binds 1..9 and "Ctrl+Alt+1..9" is how a person writes that. */
    int range = s->nkeys >= 4 && s->keys[0] >= SC_KEY_1 && s->keys[0] <= SC_KEY_9;
    for (int i = 1; range && i < (int)s->nkeys; i++)
        if (s->keys[i] != s->keys[0] + i) range = 0;

    /* a row of arrows is one chord; a row of letters is a list of chords */
    int all_arrows = s->nkeys > 1;
    for (int i = 0; all_arrows && i < (int)s->nkeys; i++) {
        int k = s->keys[i];
        if (k != SC_KEY_UP && k != SC_KEY_DOWN && k != SC_KEY_LEFT && k != SC_KEY_RIGHT)
            all_arrows = 0;
    }

    if (range) {
        if (pn) { PUTS(pre); PUT('+'); }
        PUTS(shortcuts_key_name(s->keys[0]));
        PUTS("..");
        PUTS(shortcuts_key_name(s->keys[s->nkeys - 1]));
    } else if (s->nkeys > 3 || all_arrows) {
        /* arrows read better as one chord with a key each than as four
         * identical chords: "Ctrl+Alt+←→↑↓". No spaces: at the largest text
         * scale the row has to fit three columns, and the four separators
         * were worth a truncated description. */
        if (pn) { PUTS(pre); PUT('+'); }
        for (int i = 0; i < (int)s->nkeys; i++)
            PUTS(shortcuts_key_name(s->keys[i]));
    } else {
        for (int i = 0; i < (int)s->nkeys; i++) {
            if (i) PUTS(" / ");
            /* the Super key is the modifier, so "Super+Super" must not
             * happen: a bare meta key is just "Super" */
            if (pn && s->keys[i] != SC_KEY_META) { PUTS(pre); PUT('+'); }
            PUTS(shortcuts_key_name(s->keys[i]));
        }
    }
#undef PUT
#undef PUTS

    int len = n < cap - 1 ? n : cap - 1;
    memcpy(out, buf, (size_t)len);
    out[len] = 0;
    return len;
}

/* ============================================================
 *  The panel
 *
 *  Drawn from the same rows the dispatcher matches, so the list cannot
 *  describe a key that does nothing. Nothing here is a fixed pixel: the
 *  row height comes from the font, the padding from ui_px(), and the
 *  number of columns and pages from how much room the list needs.
 *
 *  The layout is a pure measurement (shortcuts_panel_layout) plus a
 *  painter. The host test measures every page at several screen sizes
 *  and text scales and requires overflow == 0, so "50+ rows still fit"
 *  is a measured claim, not a hope.
 * ============================================================ */

/* Cut a string to `avail` pixels, ending in an ellipsis. Walks UTF-8 so a
 * multi-byte character (the arrow glyphs) is never split in half. */
static void panel_fit(Font *f, const char *src, char *dst, size_t cap, float avail) {
    if (!dst || cap < 4 || !f || !src) return;
    dst[0] = 0;
    if (font_width(f, src) <= avail) { snprintf(dst, cap, "%s", src); return; }
    const char *ell = "\xE2\x80\xA6";
    float ew = font_width(f, ell), room = avail - ew;
    if (ew <= 0.0f) { ell = "..."; ew = font_width(f, ell); room = avail - ew; }
    size_t elen = strlen(ell);
    if (room < 0.0f || cap <= elen + 1) return;
    size_t out = 0;
    float w = 0.0f;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        int len = 1;
        unsigned char c = *p;
        if ((c & 0xE0) == 0xC0 && p[1]) len = 2;
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) len = 3;
        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) len = 4;
        if (out + (size_t)len + elen >= cap) break;
        char one[5];
        memcpy(one, p, (size_t)len);
        one[len] = '\0';
        float adv = font_width(f, one);
        if (w + adv > room) break;
        memcpy(dst + out, p, (size_t)len);
        out += (size_t)len;
        w += adv;
        p += len;
    }
    memcpy(dst + out, ell, elen);
    out += elen;
    dst[out] = 0;
}

enum {
    SC_PANEL_MAX_ITEMS = 256,
    SC_PANEL_MAX_COLS  = 3,
    SC_PANEL_MAX_PAGES = 12,
    SC_PANEL_MAX_ROWS  = 320
};

/* A column of items. `repeat_cat` >= 0 means the column begins in the
 * middle of that category (the list was split for height), so the heading
 * is repeated at the top: a block of rows without its heading reads like a
 * different category. */
typedef struct { int s, e; int repeat_cat; } PanelCol;
typedef struct { int ncols; PanelCol col[SC_PANEL_MAX_COLS]; } PanelPage;

typedef struct {
    Font *ft, *fc, *fr;
    float row_h, cat_h;
    int head_h, foot_h;
    int pad, gap;
    int cols;
    int px, py, pw, ph;
    int body_top, body_bottom;
    int items;
    int item_row[SC_PANEL_MAX_ITEMS];
    int item_cat[SC_PANEL_MAX_ITEMS];
    PanelPage page[SC_PANEL_MAX_PAGES];
    int npages;
    int rows_on_page;
    int overflow;
} PanelLayout;

/* Fill `page[]` with the items that fit, in reading order. Returns the
 * number of pages written and sets *consumed to the first item not placed
 * (== items when every row made it). A column never ends on a category
 * heading, and every column holds at least one item, so this terminates. */
static int panel_paginate(const int *item_row, const int *item_cat, int items,
                          int cols, float body_h, float row_h, float cat_h,
                          PanelPage *page, int *consumed) {
    int i = 0, p = 0;
    while (i < items && p < SC_PANEL_MAX_PAGES) {
        PanelPage *pg = &page[p];
        pg->ncols = 0;
        while (pg->ncols < cols && i < items) {
            int start = i;
            /* a continuation column repeats its category heading, and that
             * heading takes the same vertical room as a real one */
            float h = (item_row[start] < 0) ? 0.0f : cat_h;
            while (i < items) {
                float ih = item_row[i] < 0 ? cat_h : row_h;
                if (h + ih > body_h && i > start) break;
                h += ih;
                i++;
            }
            /* a heading as the last line of a column labels nothing: move
             * it to the next column (and never empty this one) */
            while (i > start + 1 && item_row[i - 1] < 0) i--;
            if (i == start) i = start + 1;      /* always make progress */
            pg->col[pg->ncols].s = start;
            pg->col[pg->ncols].e = i;
            pg->col[pg->ncols].repeat_cat =
                (item_row[start] < 0) ? -1 : item_cat[start];
            pg->ncols++;
        }
        if (pg->ncols == 0) break;
        p++;
    }
    *consumed = i;
    return p;
}

/* Measure the panel for one page. Returns 0 when the fonts are missing
 * (the desktop then draws nothing rather than inventing a layout). */
static int panel_build(Surface *s, int top_inset, int page_no, PanelLayout *L) {
    if (!s || !L) return 0;
    memset(L, 0, sizeof(*L));

    Font *ft = font_get(FONT_SEMIBOLD, 16);
    Font *fc = font_get(FONT_MEDIUM, 11);
    if (!ft || !fc) return 0;

    /* a category heading, then every binding in it, in table order */
    const int n = shortcuts_count();
    for (int c = 0; c < SC_CAT_COUNT && L->items < SC_PANEL_MAX_ITEMS; c++) {
        int head = 1;
        for (int i = 0; i < n && L->items < SC_PANEL_MAX_ITEMS; i++) {
            const Shortcut *sc = shortcuts_at(i);
            if ((int)sc->cat != c) continue;
            if (head) {
                L->item_row[L->items] = -1;
                L->item_cat[L->items] = c;
                L->items++;
                head = 0;
            }
            L->item_row[L->items] = i;
            L->item_cat[L->items] = c;
            L->items++;
        }
    }
    if (L->items == 0) return 0;

    L->ft = ft;
    L->fc = fc;
    const int outer = ui_px(24);
    const int pad = ui_px(20);
    const int gap = ui_px(22);
    L->pad = pad;
    L->gap = gap;
    L->head_h = (int)(font_height(ft) + (float)ui_px(16));
    L->foot_h = (int)(font_height(fc) + (float)ui_px(16));

    int avail_w = s->w - 2 * outer;
    if (avail_w < ui_px(200)) avail_w = s->w - ui_px(8);
    if (avail_w < ui_px(120)) avail_w = ui_px(120);
    int avail_h = s->h - top_inset - L->head_h - L->foot_h - ui_px(16);
    if (avail_h < ui_px(40)) avail_h = ui_px(40);

    /* Fit, in steps: the row face starts at 12 and only shrinks when the
     * list would need too many pages. A narrower screen costs columns
     * first (readability), then pages. */
    static const int ROW_SIZE[4] = { 12, 11, 10, 9 };
    Font *fr = NULL;
    int chosen = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        fr = font_get(FONT_REGULAR, ROW_SIZE[attempt]);
        if (!fr) return 0;
        float row_h = font_height(fr) + (float)ui_px(4);
        float cat_h = font_height(fc) + (float)ui_px(9);

        /* the widest chord sets the narrowest column that still leaves a
         * description next to it */
        float chord_max = 0.0f, row_need = 0.0f;
        for (int i = 0; i < L->items; i++) {
            if (L->item_row[i] < 0) continue;
            const Shortcut *sc = shortcuts_at(L->item_row[i]);
            char chord[192];
            shortcuts_chord(sc, chord, (int)sizeof(chord));
            float cw = font_width(fr, chord);
            float dw = font_width(fr, sc->desc);
            if (cw > chord_max) chord_max = cw;
            if (dw + (float)ui_px(10) + cw > row_need)
                row_need = dw + (float)ui_px(10) + cw;
        }
        int min_col = (int)(chord_max + (float)ui_px(96));
        if (min_col < ui_px(150)) min_col = ui_px(150);
        int col_need = (int)row_need;
        if (col_need < min_col) col_need = min_col;
        if (col_need > avail_w) col_need = avail_w;

        int cols = SC_PANEL_MAX_COLS;
        while (cols > 1 && avail_w < cols * col_need + (cols - 1) * gap) cols--;

        PanelPage page[SC_PANEL_MAX_PAGES];
        int consumed = 0;
        int npages = panel_paginate(L->item_row, L->item_cat, L->items, cols,
                                    (float)avail_h, row_h, cat_h, page,
                                    &consumed);

        /* 12px over three pages, 11 over four, and so on; the smallest
         * size is accepted whatever it needs, because at that point the
         * screen is the limit and the footer reports the extra pages. */
        int limit = 3 + attempt;
        L->fr = fr;
        L->row_h = row_h;
        L->cat_h = cat_h;
        L->cols = cols;
        L->npages = npages;
        memcpy(L->page, page, sizeof(page));
        L->overflow = 0;
        for (int i = consumed; i < L->items; i++)
            if (L->item_row[i] >= 0) L->overflow++;
        chosen = (npages <= limit) || attempt == 3;
        if (chosen) break;
    }
    if (!chosen || !L->fr) return 0;

    /* The page's column body: the tallest column on any page, so the
     * panel does not jump when the page changes. */
    float tallest = 0.0f;
    for (int p = 0; p < L->npages; p++) {
        for (int c = 0; c < L->page[p].ncols; c++) {
            float h = 0.0f;
            if (L->page[p].col[c].repeat_cat >= 0) h += L->cat_h;
            for (int i = L->page[p].col[c].s; i < L->page[p].col[c].e; i++)
                h += L->item_row[i] < 0 ? L->cat_h : L->row_h;
            if (h > tallest) tallest = h;
        }
    }
    int body_need = (int)(tallest + 0.999f);
    if (body_need > avail_h) body_need = avail_h;

    /* width: as wide as the columns need, not as wide as the screen */
    int col_need = (int)((float)avail_w / (float)L->cols);
    {
        float need = 0.0f;
        for (int i = 0; i < L->items; i++) {
            if (L->item_row[i] < 0) continue;
            const Shortcut *sc = shortcuts_at(L->item_row[i]);
            char chord[192];
            shortcuts_chord(sc, chord, (int)sizeof(chord));
            float w2 = font_width(L->fr, sc->desc) + (float)ui_px(10) +
                       font_width(L->fr, chord);
            if (w2 > need) need = w2;
        }
        int want = (int)(need + (float)ui_px(24));
        int per_col = want;
        if (per_col < ui_px(150)) per_col = ui_px(150);
        if (per_col > col_need) per_col = col_need;
        col_need = per_col;
    }
    L->pw = L->cols * col_need + (L->cols - 1) * L->gap + 2 * L->pad;
    if (L->pw > s->w - ui_px(8)) L->pw = s->w - ui_px(8);
    if (L->pw < ui_px(180)) L->pw = ui_px(180);
    L->ph = L->head_h + L->foot_h + body_need + ui_px(4);
    if (L->ph > s->h - top_inset - ui_px(8))
        L->ph = s->h - top_inset - ui_px(8);
    L->px = (s->w - L->pw) / 2;
    L->py = top_inset + (s->h - top_inset - L->ph) / 2;
    if (L->py < top_inset) L->py = top_inset;

    if (page_no < 0) page_no = 0;
    if (page_no >= L->npages) page_no = L->npages - 1;
    L->body_top = L->py + L->head_h;
    L->body_bottom = L->py + L->ph - L->foot_h;
    L->rows_on_page = 0;
    for (int c = 0; c < L->page[page_no].ncols; c++)
        for (int i = L->page[page_no].col[c].s; i < L->page[page_no].col[c].e; i++)
            if (L->item_row[i] >= 0) L->rows_on_page++;
    return page_no + 1;
}

ShortcutsPanel shortcuts_panel_layout(Surface *s, int top_inset, int page) {
    ShortcutsPanel out;
    memset(&out, 0, sizeof(out));
    PanelLayout L;
    int used = panel_build(s, top_inset, page, &L);
    if (used <= 0) return out;
    out.pages = L.npages;
    out.page = used - 1;
    out.cols = L.cols;
    out.px = L.px;
    out.py = L.py;
    out.pw = L.pw;
    out.ph = L.ph;
    out.body_top = L.body_top;
    out.body_bottom = L.body_bottom;
    out.rows = L.rows_on_page;
    out.overflow = L.overflow;
    return out;
}

int shortcuts_draw_panel(Surface *s, const Theme *th, int top_inset, int page) {
    if (!s || !th) return 1;
    PanelLayout L;
    int used = panel_build(s, top_inset, page, &L);
    if (used <= 0) return 1;
    const int cur = used - 1;

    /* dim what is behind, then the panel itself */
    surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u);
    gfx_shadow(s, (float)L.px, (float)L.py, (float)L.pw, (float)L.ph, 22, 32, 0.6f);
    if (th->blur > 0) gfx_blur_region(s, L.px, L.py, L.pw, L.ph, (int)(22 * th->blur));
    gfx_rrect_aa(s, (float)L.px, (float)L.py, (float)L.pw, (float)L.ph, 22,
                 color_alpha(th->window_bg, 0.98f), th->window_border, 1.0f);

    font_draw(s, L.ft, (float)(L.px + L.pad), (float)(L.py + ui_px(13)),
              "Keyboard shortcuts", th->text);
    {
        char count[64];
        snprintf(count, sizeof(count), "%d bindings  -  %d groups",
                 shortcuts_count(), SC_CAT_COUNT);
        const char *hint = "Esc closes";
        float cw = font_width(L.fc, count);
        float hint_w = font_width(L.fc, hint);
        float right = (float)(L.px + L.pw - L.pad);
        float left_after_title = (float)(L.px + L.pad) +
                                 font_width(L.ft, "Keyboard shortcuts") +
                                 (float)ui_px(12);
        /* right to left: the count, then the hint if both fit beside the
         * title; otherwise only the count, fitted to what is left */
        if (right - cw >= left_after_title) {
            font_draw(s, L.fc, right - cw, (float)(L.py + ui_px(18)),
                      count, th->text_dim);
            if (right - cw - (float)ui_px(16) - hint_w >= left_after_title)
                font_draw(s, L.fc, right - cw - (float)ui_px(16) - hint_w,
                          (float)(L.py + ui_px(18)), hint, th->text_dim);
        } else {
            char fitted[64];
            panel_fit(L.fc, count, fitted, sizeof(fitted),
                      right - left_after_title);
            if (fitted[0])
                font_draw(s, L.fc, right - font_width(L.fc, fitted),
                          (float)(L.py + ui_px(18)), fitted, th->text_dim);
        }
    }
    surf_blend(s, L.px + L.pad, L.py + L.head_h - ui_px(6),
               L.pw - L.pad * 2, 1, color_alpha(th->panel_border, 0.9f));

    /* The body is clipped for everything drawn with the gfx helpers. Text is
     * not clipped by the surface (font_draw writes pixels directly), so the
     * row loop stops at the same boundary instead of relying on it: a row is
     * drawn whole or not at all, never through the footer. */
    surf_clip(s, L.px + L.pad, L.body_top, L.pw - L.pad * 2,
              L.body_bottom - L.body_top);

    const float col_w = (float)(L.pw - 2 * L.pad -
                                (L.cols - 1) * L.gap) / (float)L.cols;
    for (int c = 0; c < L.page[cur].ncols; c++) {
        float x = (float)(L.px + L.pad + c * (col_w + (float)L.gap));
        float y = (float)L.body_top;
        /* a column that starts mid-category repeats its heading */
        if (L.page[cur].col[c].repeat_cat >= 0) {
            font_draw(s, L.fc, x, y + (float)ui_px(3),
                      shortcuts_category_name(L.page[cur].col[c].repeat_cat),
                      th->accent);
            y += L.cat_h;
        }
        for (int i = L.page[cur].col[c].s; i < L.page[cur].col[c].e; i++) {
            if (L.item_row[i] < 0) {
                if (y + L.cat_h > (float)L.body_bottom + 1.0f) { y += L.cat_h; continue; }
                font_draw(s, L.fc, x, y + (float)ui_px(3),
                          shortcuts_category_name(L.item_cat[i]), th->accent);
                y += L.cat_h;
                continue;
            }
            if (y + L.row_h > (float)L.body_bottom + 1.0f) { y += L.row_h; continue; }
            const Shortcut *sc = shortcuts_at(L.item_row[i]);
            char chord[192], label[192];
            shortcuts_chord(sc, chord, (int)sizeof(chord));
            float chord_w = font_width(L.fr, chord);
            float cap = col_w - (float)ui_px(84);
            if (cap < col_w * 0.50f) cap = col_w * 0.50f;
            if (chord_w > cap) {
                panel_fit(L.fr, chord, label, sizeof(label), cap);
                snprintf(chord, sizeof(chord), "%s", label);
                chord_w = font_width(L.fr, chord);
            }
            panel_fit(L.fr, sc->desc, label, sizeof(label),
                      col_w - chord_w - (float)ui_px(10));
            font_draw(s, L.fr, x, y + (float)ui_px(3), label, th->text);
            font_draw(s, L.fr, x + col_w - chord_w, y + (float)ui_px(3),
                      chord, th->text_dim);
            y += L.row_h;
        }
    }
    surf_unclip(s);

    /* the footer says where we are and how to leave; when a page counter
     * exists, PgUp/PgDn is the other half of the instruction */
    char foot[160], shown[160];
    if (L.npages > 1)
        snprintf(foot, sizeof(foot), "Page %d/%d  -  PgUp / PgDn  -  Esc closes",
                 cur + 1, L.npages);
    else
        snprintf(foot, sizeof(foot), "%s", "Esc closes");
    panel_fit(L.fc, foot, shown, sizeof(shown), (float)(L.pw - 2 * L.pad));
    float fw = font_width(L.fc, shown);
    font_draw(s, L.fc, (float)L.px + (float)L.pw * 0.5f - fw * 0.5f,
              (float)(L.py + L.ph) - font_height(L.fc) - (float)ui_px(8),
              shown, color_alpha(th->text, 0.55f));
    return L.npages;
}
