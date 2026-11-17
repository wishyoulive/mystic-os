/* ============================================================
 *  Mystic OS - shortcuts.c
 *
 *  The binding table and the dispatcher. Read shortcuts.h first: it
 *  explains the matching rule and the action contract.
 *
 *  Nothing in this file touches the screen, the filesystem or a
 *  window: it is a pure function from (key, modifiers, context) to a
 *  row of the table, plus the host callback that main.c supplies.
 *  That is what makes it testable without a framebuffer, and it is
 *  why the overlay can be drawn from the same rows the dispatcher
 *  uses - one table, one answer to "what does this key do?".
 * ============================================================ */
#include "shortcuts.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ the rows
 *
 * One row per action. Several keys may share a row when they are the
 * same action (Ctrl+W and Ctrl+Q close; the four arrows move), and the
 * same action may need a second row when a different modifier is the
 * only sensible chord (F1 and Super+E both open Files).
 *
 * The categories are in SC_CAT_* order and the overlay relies on that,
 * so a new row belongs inside its block, not at the end of the file.
 */
#define ROW_F(id, cat, flags, mods, cond, desc, ...)                       \
    { (unsigned char)(id), (unsigned char)(cat),                           \
      (unsigned char)(sizeof((int[]){ __VA_ARGS__ }) / sizeof(int)),       \
      (unsigned char)(flags),                                              \
      (unsigned short)(mods), (unsigned short)(cond), { __VA_ARGS__ },    \
      (desc) }
/* an ordinary binding */
#define ROW(id, cat, mods, cond, desc, ...)                                \
    ROW_F((id), (cat), 0, (mods), (cond), (desc), __VA_ARGS__)
/* one that also works while a modal overlay is up (see SC_ALWAYS) */
#define ROW_ALWAYS(id, cat, mods, cond, desc, ...)                         \
    ROW_F((id), (cat), SC_ALWAYS, (mods), (cond), (desc), __VA_ARGS__)

static const Shortcut TABLE[] = {
    /* ------------------------------------------------ window */
    ROW(SC_ACT_WIN_CLOSE, SC_CAT_WINDOW, SC_MOD_CTRL, 0,
        "Close window", SC_KEY_W, SC_KEY_Q),
    ROW(SC_ACT_WIN_CLOSE, SC_CAT_WINDOW, 0, 0,
        "Close window", SC_KEY_ESC),
    ROW(SC_ACT_WIN_MIN, SC_CAT_WINDOW, SC_MOD_CTRL, 0,
        "Minimise window", SC_KEY_M),
    ROW(SC_ACT_WIN_MAX, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Maximise window", SC_KEY_M),
    /* F11 is bound once, here: full screen covers the top bar too, so
     * listing it as a second maximise chord would make one press mean
     * two things. Ctrl+Shift+M maximises; F11 takes the whole panel. */
    ROW(SC_ACT_WIN_FULL, SC_CAT_WINDOW, 0, 0,
        "Full screen", SC_KEY_F11),
    ROW(SC_ACT_WIN_RESTORE, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_SHIFT, 0,
        "Restore / un-minimise", SC_KEY_R),
    /* Alt+Tab is the one binding that acted through the launcher overlay
     * before this table existed; SC_ALWAYS keeps that true. */
    ROW_ALWAYS(SC_ACT_WIN_NEXT, SC_CAT_WINDOW, SC_MOD_ALT, 0,
               "Next window", SC_KEY_TAB),
    ROW_ALWAYS(SC_ACT_WIN_PREV, SC_CAT_WINDOW, SC_MOD_ALT | SC_MOD_SHIFT, 0,
               "Previous window", SC_KEY_TAB),
    ROW(SC_ACT_WIN_SAME_APP, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Cycle app windows", SC_KEY_GRAVE),
    ROW(SC_ACT_WIN_MOVE, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Move window", SC_KEY_LEFT, SC_KEY_RIGHT,
        SC_KEY_UP, SC_KEY_DOWN),
    ROW(SC_ACT_WIN_RESIZE, SC_CAT_WINDOW, SC_MOD_CTRL | SC_MOD_ALT | SC_MOD_SHIFT, 0,
        "Resize window", SC_KEY_LEFT, SC_KEY_RIGHT,
        SC_KEY_UP, SC_KEY_DOWN),
    ROW(SC_ACT_WIN_SNAP, SC_CAT_WINDOW, SC_MOD_SUPER, 0,
        "Snap left / right / up", SC_KEY_LEFT, SC_KEY_RIGHT, SC_KEY_UP),

    /* ------------------------------------------------ desktop */
    ROW(SC_ACT_LAUNCHER, SC_CAT_DESKTOP, SC_MOD_SUPER, 0,
        "Launcher (Super)", SC_KEY_META, SC_KEY_SPACE),
    ROW(SC_ACT_LOCK, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Lock screen", SC_KEY_L),
    ROW(SC_ACT_THEME_NEXT, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Next theme", SC_KEY_T),
    ROW(SC_ACT_THEME_SET, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Theme by number", SC_KEY_1, SC_KEY_2, SC_KEY_3,
        SC_KEY_4, SC_KEY_5, SC_KEY_6, SC_KEY_7, SC_KEY_8, SC_KEY_9),
    ROW(SC_ACT_WALLPAPER, SC_CAT_DESKTOP, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Next wallpaper", SC_KEY_W),
    ROW(SC_ACT_TEXT_UP, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text larger", SC_KEY_EQUAL),
    ROW(SC_ACT_TEXT_DOWN, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text smaller", SC_KEY_MINUS),
    ROW(SC_ACT_TEXT_RESET, SC_CAT_DESKTOP, SC_MOD_CTRL, 0,
        "Text normal", SC_KEY_0),
    ROW(SC_ACT_DND, SC_CAT_DESKTOP, SC_MOD_SUPER, 0,
        "Mute notifications", SC_KEY_N),
    ROW(SC_ACT_SHOT, SC_CAT_DESKTOP, 0, 0,
        "Screenshot to /root", SC_KEY_SYSRQ),

    /* ------------------------------------------------ apps */
    ROW(SC_ACT_TERMINAL, SC_CAT_APPS, SC_MOD_CTRL | SC_MOD_ALT, 0,
        "Terminal", SC_KEY_ENTER),
    /* F1 opens a terminal, except with no window at all - then it opens
     * the shortcut list. The two rows are told apart by their context,
     * not by the dispatcher guessing from the key. */
    ROW(SC_ACT_TERMINAL, SC_CAT_APPS, 0, SC_WHEN_WINDOW,
        "Terminal", SC_KEY_F1),
    ROW(SC_ACT_FILES, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Files", SC_KEY_E),
    ROW(SC_ACT_FILES, SC_CAT_APPS, 0, 0,
        "Files", SC_KEY_F2),
    ROW(SC_ACT_SETTINGS, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Settings", SC_KEY_I),
    ROW(SC_ACT_SETTINGS, SC_CAT_APPS, 0, 0,
        "Settings", SC_KEY_F3),
    ROW(SC_ACT_MONITOR, SC_CAT_APPS, 0, 0,
        "Monitor", SC_KEY_F4),
    ROW(SC_ACT_NOTES, SC_CAT_APPS, 0, 0,
        "Notes", SC_KEY_F5),
    ROW(SC_ACT_BROWSER, SC_CAT_APPS, SC_MOD_SUPER, 0,
        "Browser", SC_KEY_B),

    /* ------------------------------------------------ help */
    ROW(SC_ACT_HELP, SC_CAT_HELP, SC_MOD_SUPER, 0,
        "This shortcut list", SC_KEY_SLASH),
    ROW(SC_ACT_HELP, SC_CAT_HELP, 0, SC_WHEN_NO_WINDOW,
        "This shortcut list", SC_KEY_F1)
};

#define TABLE_N ((int)(sizeof(TABLE) / sizeof(TABLE[0])))

int shortcuts_count(void) { return TABLE_N; }

const Shortcut *shortcuts_at(int index) {
    if (index < 0 || index >= TABLE_N) return NULL;
    return &TABLE[index];
}

static const char *const CATEGORY_NAME[SC_CAT_COUNT] = {
    "Window", "Desktop", "Apps", "Help"
};

const char *shortcuts_category_name(int cat) {
    if (cat < 0 || cat >= SC_CAT_COUNT) return "Other";
    return CATEGORY_NAME[cat];
}

/* ------------------------------------------------------------ dispatch */

const Shortcut *shortcuts_find(int key, unsigned mods, unsigned cond) {
    for (int i = 0; i < TABLE_N; i++) {
        const Shortcut *s = &TABLE[i];
        if ((unsigned)s->mods != mods) continue;
        if (s->cond && !(s->cond & cond)) continue;
        for (int k = 0; k < (int)s->nkeys; k++)
            if (s->keys[k] == key) return s;
    }
    return NULL;
}

int shortcuts_dispatch(const ShortcutActions *actions, int key,
                       unsigned mods, unsigned cond) {
    const Shortcut *s = shortcuts_find(key, mods, cond);
    if (!s) return 0;
    if (actions && actions->action)
        actions->action(actions->ctx, s->id, key, mods);
    return 1;
}

/* ------------------------------------------------------------ names
 *
 * Designated initialisers, so the name of a key sits at the key's own
 * number and a reader can check it against the evdev list without
 * counting rows. Anything not named here is not in the table either,
 * and reports "?" rather than an invented label.
 */
static const char *const KEY_NAME[128] = {
    [SC_KEY_ESC]   = "Esc",
    [SC_KEY_1]     = "1",
    [SC_KEY_2]     = "2",
    [SC_KEY_3]     = "3",
    [SC_KEY_4]     = "4",
    [SC_KEY_5]     = "5",
    [SC_KEY_6]     = "6",
    [SC_KEY_7]     = "7",
    [SC_KEY_8]     = "8",
    [SC_KEY_9]     = "9",
    [SC_KEY_0]     = "0",
    [SC_KEY_MINUS] = "-",
    [SC_KEY_EQUAL] = "=",
    [SC_KEY_TAB]   = "Tab",
    [SC_KEY_Q]     = "Q",
    [SC_KEY_W]     = "W",
    [SC_KEY_E]     = "E",
    [SC_KEY_R]     = "R",
    [SC_KEY_T]     = "T",
    [SC_KEY_I]     = "I",
    [SC_KEY_ENTER] = "Enter",
    [SC_KEY_L]     = "L",
    [SC_KEY_GRAVE] = "`",
    [SC_KEY_B]     = "B",
    [SC_KEY_N]     = "N",
    [SC_KEY_M]     = "M",
    [SC_KEY_SLASH] = "/",
    [SC_KEY_SPACE] = "Space",
    [SC_KEY_F1]    = "F1",
    [SC_KEY_F2]    = "F2",
    [SC_KEY_F3]    = "F3",
    [SC_KEY_F4]    = "F4",
    [SC_KEY_F5]    = "F5",
    [SC_KEY_F11]   = "F11",
    [SC_KEY_SYSRQ] = "Print",
    [SC_KEY_UP]    = "\xE2\x86\x91",       /* up arrow    */
    [SC_KEY_LEFT]  = "\xE2\x86\x90",       /* left arrow  */
    [SC_KEY_RIGHT] = "\xE2\x86\x92",       /* right arrow */
    [SC_KEY_DOWN]  = "\xE2\x86\x93",       /* down arrow  */
    [SC_KEY_META]  = "Super"
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
        { SC_MOD_CTRL, "Ctrl" }, { SC_MOD_ALT, "Alt" },
        { SC_MOD_SHIFT, "Shift" }, { SC_MOD_SUPER, "Super" }
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
 *  number of columns from how much room the list actually needs.
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

void shortcuts_draw_panel(Surface *s, const Theme *th, int top_inset) {
    if (!s || !th) return;
    Font *ft = font_get(FONT_SEMIBOLD, 16);
    Font *fc = font_get(FONT_MEDIUM, 11);
    if (!ft || !fc) return;

    enum { MAX_ITEMS = 160, MAX_COLS = 3 };
    int item_row[MAX_ITEMS], item_cat[MAX_ITEMS], items = 0;
    const int n = shortcuts_count();

    /* a category heading, then every binding in it, in table order */
    for (int c = 0; c < SC_CAT_COUNT && items < MAX_ITEMS; c++) {
        int head = 1;
        for (int i = 0; i < n && items < MAX_ITEMS; i++) {
            const Shortcut *sc = shortcuts_at(i);
            if ((int)sc->cat != c) continue;
            if (head) { item_row[items] = -1; item_cat[items] = c; items++; head = 0; }
            if (items < MAX_ITEMS) { item_row[items] = i; item_cat[items] = c; items++; }
        }
    }
    if (items == 0) return;

    const int pad = ui_px(22);
    const int gap = ui_px(24);
    const int head_h = (int)(font_height(ft) + (float)ui_px(16));
    const int foot_h = (int)(font_height(fc) + (float)ui_px(16));
    int avail_w = s->w - 2 * ui_px(32);
    if (avail_w < ui_px(240)) avail_w = s->w - ui_px(16);
    int avail_h = s->h - top_inset - head_h - foot_h - 2 * ui_px(8);
    if (avail_h < ui_px(80)) avail_h = ui_px(80);

    /* Fit, in steps: the row face starts at 12 and only shrinks when the
     * list could not be shown whole. A column is never narrower than its
     * longest chord plus room for a description, so shrinking the face is
     * what buys the extra column that makes the height fit. */
    static const int ROW_SIZE[3] = { 12, 11, 10 };
    Font *fr = NULL;
    float row_h = 0.0f, cat_h = 0.0f, total = 0.0f;
    int cols = 1, col_w = avail_w;
    for (int attempt = 0; attempt < 3; attempt++) {
        fr = font_get(FONT_REGULAR, ROW_SIZE[attempt]);
        if (!fr) return;
        row_h = font_height(fr) + (float)ui_px(4);
        cat_h = font_height(fc) + (float)ui_px(9);
        total = 0.0f;
        float chord_max = 0.0f;
        for (int i = 0; i < items; i++) {
            if (item_row[i] < 0) { total += cat_h; continue; }
            total += row_h;
            char c[192];
            shortcuts_chord(shortcuts_at(item_row[i]), c, (int)sizeof(c));
            float cw = font_width(fr, c);
            if (cw > chord_max) chord_max = cw;
        }
        int min_col = (int)(chord_max + (float)ui_px(90));
        if (min_col < ui_px(150)) min_col = ui_px(150);

        cols = 1;
        while (cols < MAX_COLS && total / (float)cols > (float)avail_h) cols++;
        col_w = (avail_w - (cols - 1) * gap) / cols;
        while (cols > 1 && col_w < min_col) {
            cols--;
            col_w = (avail_w - (cols - 1) * gap) / cols;
        }
        if (total / (float)cols <= (float)avail_h) break;
    }
    if (col_w < ui_px(120)) col_w = ui_px(120);

    /* where each column starts - never with a category heading as the last
     * line of a column, which would label nothing */
    int bounds[MAX_COLS + 1];
    bounds[0] = 0;
    bounds[cols] = items;
    for (int c = 1; c < cols; c++) {
        int cut = (items * c) / cols;
        while (cut > bounds[c - 1] && cut < items && item_row[cut - 1] < 0) cut++;
        bounds[c] = cut;
    }

    /* Whole categories per column read better than a column that begins in
     * the middle of one, so use that split whenever it needs no extra
     * column. It is what makes each heading label everything under it. */
    {
        int first[SC_CAT_COUNT], count[SC_CAT_COUNT];
        for (int c = 0; c < SC_CAT_COUNT; c++) { first[c] = -1; count[c] = 0; }
        for (int i = 0; i < items; i++) {
            if (item_row[i] < 0) first[item_cat[i]] = i;
            else                 count[item_cat[i]]++;
        }
        int breaks[MAX_COLS];
        int nbreaks = 0, have = 0;
        float h = 0.0f;
        for (int c = 0; c < SC_CAT_COUNT; c++) {
            if (first[c] < 0) continue;
            float ch = cat_h + (float)count[c] * row_h;
            if (have && nbreaks < MAX_COLS - 1 && h + ch > (float)avail_h) {
                breaks[nbreaks++] = first[c];
                h = 0.0f;
            }
            h += ch;
            have = 1;
        }
        int cat_cols = nbreaks + 1;
        if (cat_cols <= cols) {
            /* only when it stays readable: a heading-aligned split that
             * leaves the last column half empty looks like a mistake, so
             * keep the balanced split in that case */
            float lo = 1e9f, hi = 0.0f;
            int start = 0;
            for (int b = 0; b <= nbreaks; b++) {
                int end = (b < nbreaks) ? breaks[b] : items;
                float h2 = 0.0f;
                for (int i = start; i < end; i++) h2 += item_row[i] < 0 ? cat_h : row_h;
                if (h2 < lo) lo = h2;
                if (h2 > hi) hi = h2;
                start = end;
            }
            if (lo < hi * 0.65f) { cat_cols = cols + 1; }   /* rejected */
        }
        if (cat_cols <= cols) {
            cols = cat_cols;
            bounds[0] = 0;
            for (int b = 0; b < nbreaks; b++) bounds[b + 1] = breaks[b];
            bounds[cols] = items;
            col_w = (avail_w - (cols - 1) * gap) / cols;
            if (col_w < ui_px(120)) col_w = ui_px(120);
        }
    }

    /* The panel is as wide as the table needs, not as wide as the screen:
     * with equal columns, the widest "description + chord" pair sets it. On
     * a small text scale that turns a full-width sheet into a tidy block
     * instead of two columns with a lake of empty pixels between them. */
    {
        float natural = 0.0f;
        for (int i = 0; i < items; i++) {
            if (item_row[i] < 0) continue;
            const Shortcut *sc = shortcuts_at(item_row[i]);
            char chord[192];
            shortcuts_chord(sc, chord, (int)sizeof(chord));
            float w2 = font_width(fr, sc->desc) + (float)ui_px(10) + font_width(fr, chord);
            if (w2 > natural) natural = w2;
        }
        int want = (int)(natural + (float)ui_px(26));
        if (want < ui_px(180)) want = ui_px(180);
        if (want < col_w) col_w = want;
    }

    float col_h[MAX_COLS] = { 0 };
    for (int c = 0; c < cols; c++)
        for (int i = bounds[c]; i < bounds[c + 1]; i++)
            col_h[c] += item_row[i] < 0 ? cat_h : row_h;
    float tallest = 0.0f;
    for (int c = 0; c < cols; c++) if (col_h[c] > tallest) tallest = col_h[c];

    /* round the body up to a whole pixel: a column that needs 571.4px needs
     * 572, and truncating it is what silently dropped the last row */
    int body_need = (int)(tallest + 0.999f);
    int pw = cols * col_w + (cols - 1) * gap + pad * 2;
    int ph = head_h + foot_h + body_need;
    if (pw > s->w - ui_px(8)) pw = s->w - ui_px(8);
    /* at least one row and the footer, then the screen wins: on a surface
     * too small for even that, the panel keeps its header and the row guard
     * drops the rest instead of painting past the edge */
    if (ph < head_h + foot_h + (int)row_h) ph = head_h + foot_h + (int)row_h;
    if (ph > s->h - top_inset - ui_px(8)) ph = s->h - top_inset - ui_px(8);
    int px = (s->w - pw) / 2;
    int py = top_inset + ui_px(4) + (s->h - top_inset - ui_px(4) - ph) / 2;
    if (py < top_inset) py = top_inset;

    /* dim what is behind, then the panel itself */
    surf_blend(s, 0, 0, s->w, s->h, 0xAA000000u);
    gfx_shadow(s, (float)px, (float)py, (float)pw, (float)ph, 22, 32, 0.6f);
    if (th->blur > 0) gfx_blur_region(s, px, py, pw, ph, (int)(22 * th->blur));
    gfx_rrect_aa(s, (float)px, (float)py, (float)pw, (float)ph, 22,
                 color_alpha(th->window_bg, 0.98f), th->window_border, 1.0f);

    font_draw(s, ft, (float)(px + pad), (float)(py + ui_px(13)),
              "Keyboard shortcuts", th->text);
    float title_w = font_width(ft, "Keyboard shortcuts");
    float hint_x = (float)(px + pw - pad) - font_width(fc, "Esc closes");
    if (hint_x > (float)(px + pad) + title_w + (float)ui_px(12))
        font_draw(s, fc, hint_x, (float)(py + ui_px(18)), "Esc closes", th->text_dim);
    surf_blend(s, px + pad, py + head_h - ui_px(6), pw - pad * 2, 1,
               color_alpha(th->panel_border, 0.9f));

    /* The body is clipped for everything drawn with the gfx helpers. Text is
     * not clipped by the surface (font_draw writes pixels directly), so the
     * row loop stops at the same boundary instead of relying on it: a row is
     * drawn whole or not at all, never through the footer. */
    const float body_top = (float)(py + head_h);
    const float body_bottom = (float)(py + ph - foot_h);
    surf_clip(s, px + pad, py + head_h, pw - pad * 2, ph - head_h - foot_h);

    int shown = 0, hidden = 0;
    for (int c = 0; c < cols; c++) {
        float x = (float)(px + pad + c * (col_w + gap));
        float y = body_top;
        for (int i = bounds[c]; i < bounds[c + 1]; i++) {
            if (item_row[i] < 0) {
                if (y + cat_h > body_bottom + 1.0f) { y += cat_h; continue; }
                font_draw(s, fc, x, y + (float)ui_px(3),
                          shortcuts_category_name(item_cat[i]), th->accent);
                y += cat_h;
                continue;
            }
            if (y + row_h > body_bottom + 1.0f) { y += row_h; hidden++; continue; }
            const Shortcut *sc = shortcuts_at(item_row[i]);
            char chord[192], label[192];
            shortcuts_chord(sc, chord, (int)sizeof(chord));
            float chord_w = font_width(fr, chord);
            float cap = (float)col_w - (float)ui_px(90);
            if (cap < (float)col_w * 0.55f) cap = (float)col_w * 0.55f;
            if (chord_w > cap) {
                panel_fit(fr, chord, label, sizeof(label), cap);
                snprintf(chord, sizeof(chord), "%s", label);
                chord_w = font_width(fr, chord);
            }
            panel_fit(fr, sc->desc, label, sizeof(label),
                      (float)col_w - chord_w - (float)ui_px(10));
            font_draw(s, fr, x, y + (float)ui_px(3), label, th->text);
            font_draw(s, fr, x + (float)col_w - chord_w, y + (float)ui_px(3),
                      chord, th->text_dim);
            y += row_h;
            shown++;
        }
    }
    surf_unclip(s);

    /* the footer says how to close it, and admits it when this text size on
     * this screen cannot show every row */
    char foot[128], foot_shown[128];
    if (hidden > 0)
        snprintf(foot, sizeof(foot), "Super+/ or Esc closes  -  %d more: Ctrl+- for smaller text",
                 hidden);
    else
        snprintf(foot, sizeof(foot), "%s", "Super+/ or Esc closes");
    (void)shown;
    /* text is not clipped by the surface, so it is fitted here: even the
     * footer must stay inside the panel at the largest text scale */
    panel_fit(fc, foot, foot_shown, sizeof(foot_shown), (float)(pw - 2 * pad));
    float fw = font_width(fc, foot_shown);
    font_draw(s, fc, (float)px + (float)pw * 0.5f - fw * 0.5f,
              (float)(py + ph) - font_height(fc) - (float)ui_px(8),
              foot_shown, color_alpha(th->text, 0.55f));
}
