/* ============================================================
 *  Mystic OS - theme.c
 *
 *  A theme is a plain text file, one "key = value" per line:
 *
 *      name    = Obsidian
 *      bg_top  = #0B0F1A
 *      accent  = #7AA2F7
 *      corner  = 14
 *
 *  Every key is optional. A file that leaves one out, or whose value
 *  does not parse, keeps the built-in default for that key, and the
 *  parser says which file and which key on stderr. A typo therefore
 *  never silently changes the look of the desktop, and a half-written
 *  theme can never leave text at an unreadable colour.
 *
 *  Colours are #RRGGBB or #AARRGGBB (the '#' is optional, "0x" works
 *  too). A '#' at the start of a value is a colour; a '#' after white
 *  space is a comment. Numbers are decimal.
 *
 *  docs/THEMES.md has the full key set, the contrast targets every
 *  shipped theme is measured against, and how to add one.
 * ============================================================ */
#include "mystic.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* theme_load_all() reads at most this many directory entries, and fills at
 * most THEME_MAX of them. A caller whose array is smaller is told exactly
 * how many files it dropped, instead of losing them silently. */
#define THEME_SCAN_MAX 256
#define THEME_MAX      32
#define THEME_LINE_MAX 512
#define THEME_WARN_MAX 32      /* per file: a corrupt file cannot flood stderr */

typedef struct {
    const char *path;
    int         line;          /* 0 when the problem is the file, not a line */
    int         warned;
} Src;

/* Every diagnostic goes through here so it always names the file, the line
 * when there is one, and the key. */
static void twarn_at(Src *s, const char *key, const char *fmt, ...) {
    if (s->warned > THEME_WARN_MAX) return;
    if (s->warned == THEME_WARN_MAX) {
        s->warned++;
        fprintf(stderr, "theme: %s: too many problems in this file, "
                        "further warnings suppressed\n", s->path);
        return;
    }
    s->warned++;
    if (s->line > 0) fprintf(stderr, "theme: %s:%d: ", s->path, s->line);
    else             fprintf(stderr, "theme: %s: ", s->path);
    if (key && key[0]) fprintf(stderr, "key '%s': ", key);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void defaults(Theme *t) {
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "Default");
    t->bg_top = RGB(0x0B, 0x0F, 0x1A);
    t->bg_bottom = RGB(0x14, 0x1A, 0x2B);
    t->accent = RGB(0x7A, 0xA2, 0xF7);
    t->icon_accent = t->accent;          /* a theme with no second colour still
                                          * gets a highlight of its own */
    t->icon_style = ICON_STYLE_DUOTONE;
    t->anim = ANIM_PULSE;
    t->glow = RGB(0x3D, 0x59, 0xA0);
    t->glow_strength = 0.55f;
    t->panel = WITH_A(RGB(0x1A, 0x20, 0x33), 0x8C);
    t->panel_border = WITH_A(RGB(0xFF, 0xFF, 0xFF), 0x1A);
    t->text = RGB(0xE8, 0xEC, 0xF6);
    t->text_dim = WITH_A(RGB(0xE8, 0xEC, 0xF6), 0x99);
    t->window_bg = RGB(0x14, 0x19, 0x28);        /* opaque: see theme_load  */
    t->window_border = WITH_A(RGB(0xFF, 0xFF, 0xFF), 0x22);
    t->corner = 14.0f;
    t->blur = 1.0f;
    t->anim_speed = 1.0f;                /* icons pulse gently by default */
    t->anim_amp = 0.6f;
    t->font_scale = 1.0f;
    t->font_ui[0] = 0;                   /* 0 = keep whatever font.c chose  */
    t->font_mono[0] = 0;
    t->dark = 1;
}

/* ---------------------------------------------------------- values ---- */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The whole token has to be hex: "#0B0F1A;" is an error, not a colour, and
 * neither is "#12345". Six digits are RRGGBB (opaque), eight are AARRGGBB. */
static int parse_color(const char *v, u32 *out) {
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '#') v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    int digits = 0;
    while (hexval((unsigned char)v[digits]) >= 0) digits++;
    const char *p = v + digits;
    while (*p == ' ' || *p == '\t') p++;
    if (*p || (digits != 6 && digits != 8)) return 0;
    u32 c = (u32)strtoul(v, NULL, 16);
    *out = (digits == 8) ? c : WITH_A(c, 0xFF);
    return 1;
}

static int parse_float(const char *v, float *out) {
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return 0;
    *out = f;
    return 1;
}

static int parse_flag(const char *v, int *out) {
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end || (n != 0 && n != 1)) return 0;
    *out = (int)n;
    return 1;
}

static void set_color(Src *s, const char *key, const char *v, u32 *dst) {
    u32 c;
    if (parse_color(v, &c)) { *dst = c; return; }
    twarn_at(s, key, "malformed colour '%s' (want #RRGGBB or #AARRGGBB), "
                     "keeping #%08X", v, *dst);
}

static void set_num(Src *s, const char *key, const char *v, float *dst,
                    float lo, float hi) {
    float f;
    if (!parse_float(v, &f)) {
        twarn_at(s, key, "malformed number '%s', keeping %g", v, (double)*dst);
        return;
    }
    if (f != f) {                                   /* NaN: strtof("nan") */
        twarn_at(s, key, "not a number, keeping %g", (double)*dst);
        return;
    }
    if (f < lo || f > hi) {
        float c = (f < lo) ? lo : hi;
        twarn_at(s, key, "%g is outside %g..%g, using %g",
                 (double)f, (double)lo, (double)hi, (double)c);
        f = c;
    }
    *dst = f;
}

static void set_flag(Src *s, const char *key, const char *v, int *dst) {
    int n;
    if (parse_flag(v, &n)) { *dst = n; return; }
    twarn_at(s, key, "malformed flag '%s' (want 0 or 1), keeping %d", v, *dst);
}

/* ------------------------------------------------------ named values ---- */
static const char *const anim_names[] = {
    "none", "pulse", "glow", "drift", "orbit", "wave", "spin"
};
static const char *const style_names[] = {
    "solid", "outline", "duotone", "rounded", "sharp", "pixel"
};

static void set_anim(Src *s, const char *key, const char *v, AnimKind *dst) {
    for (int i = 0; i < ANIM_COUNT; i++)
        if (!strcmp(v, anim_names[i])) { *dst = (AnimKind)i; return; }
    twarn_at(s, key, "unknown animation '%s' (want %s), keeping '%s'", v,
             "none|pulse|glow|drift|orbit|wave|spin", anim_names[*dst]);
}

static void set_style(Src *s, const char *key, const char *v, IconStyle *dst) {
    for (int i = 0; i < ICON_STYLE_COUNT; i++)
        if (!strcmp(v, style_names[i])) { *dst = (IconStyle)i; return; }
    twarn_at(s, key, "unknown icon style '%s' (want %s), keeping '%s'", v,
             "solid|outline|duotone|rounded|sharp|pixel", style_names[*dst]);
}

static void set_text(Src *s, const char *key, const char *v, char *dst, size_t cap) {
    if (!v[0]) {
        twarn_at(s, key, "empty value, keeping '%s'", dst[0] ? dst : "the default");
        return;
    }
    if (strlen(v) >= cap)
        twarn_at(s, key, "longer than %d bytes, truncated", (int)cap - 1);
    snprintf(dst, cap, "%s", v);
}

/* ------------------------------------------------------------ keys ---- */

enum {
    K_NAME, K_DARK, K_BG_TOP, K_BG_BOTTOM, K_ACCENT, K_ICON_ACCENT,
    K_ICON_STYLE, K_ANIM, K_GLOW, K_GLOW_STRENGTH, K_PANEL, K_PANEL_BORDER,
    K_TEXT, K_TEXT_DIM, K_WINDOW_BG, K_WINDOW_BORDER, K_CORNER, K_BLUR,
    K_ANIM_SPEED, K_ANIM_AMP, K_FONT_UI, K_FONT_MONO, K_FONT_SCALE, K_COUNT
};

static const char *const key_name[K_COUNT] = {
    "name",         "dark",        "bg_top",       "bg_bottom",
    "accent",       "icon_accent", "icon_style",   "anim",
    "glow",         "glow_strength", "panel",      "panel_border",
    "text",         "text_dim",    "window_bg",    "window_border",
    "corner",       "blur",        "anim_speed",   "anim_amp",
    "font_ui",      "font_mono",   "font_scale"
};

/* The keys a theme is expected to set. The rest have defaults that are safe
 * on any background (the glow, the two border alphas, the corner radius, the
 * blur strength, and the icon pair - icon_accent falls back to accent and the
 * icon pulse to 1.0). These nine decide whether the theme is readable, so a
 * missing one is worth a warning even though the default still works. */
static const unsigned REQUIRED =
    (1u << K_NAME) | (1u << K_DARK) | (1u << K_BG_TOP) | (1u << K_BG_BOTTOM) |
    (1u << K_ACCENT) | (1u << K_PANEL) | (1u << K_TEXT) | (1u << K_TEXT_DIM) |
    (1u << K_WINDOW_BG);

int theme_load(Theme *t, const char *path) {
    if (!t) return -1;
    defaults(t);
    if (!path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    Src src = { path, 0, 0 };
    unsigned seen = 0;
    int first = 1;
    char line[THEME_LINE_MAX];

    while (fgets(line, sizeof line, f)) {
        src.line++;
        size_t len = strlen(line);

        if (first) {                       /* a UTF-8 BOM from a text editor */
            first = 0;
            if (len >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                memmove(line, line + 3, len - 2);
            len = strlen(line);
        }

        /* fgets() split an over-long line into two lines, and the second half
         * would parse as junk. Throw the rest of the line away instead. */
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(f)) {
            twarn_at(&src, NULL, "line longer than %d bytes, ignored",
                     THEME_LINE_MAX - 1);
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
            continue;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;            /* blank or comment */

        char *eq = strchr(p, '=');
        if (!eq) {
            twarn_at(&src, NULL, "no '=' - expected 'key = value', line ignored");
            continue;
        }
        *eq = 0;

        char *key = p;
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = 0;

        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        /* A '#' after white space starts a comment; a '#' at the start of the
         * value is the colour prefix, so "accent = #FF00FF # magenta" works. */
        for (char *q = val; *q; q++) {
            if (*q == '#' && q > val && (q[-1] == ' ' || q[-1] == '\t')) {
                *q = 0;
                break;
            }
        }
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = 0;

        int k = -1;
        for (int i = 0; i < K_COUNT; i++)
            if (!strcmp(key, key_name[i])) { k = i; break; }
        if (k < 0) {
            twarn_at(&src, NULL, "unknown key '%s', ignored (see docs/THEMES.md)",
                     key);
            continue;
        }
        if (seen & (1u << k))
            twarn_at(&src, key, "appears more than once, the last value wins");
        seen |= 1u << k;

        switch (k) {
        case K_NAME:
            if (!val[0]) {
                twarn_at(&src, key, "empty name, keeping '%s'", t->name);
            } else {
                if (strlen(val) >= sizeof t->name)
                    twarn_at(&src, key, "name longer than %d bytes, truncated",
                             (int)sizeof t->name - 1);
                snprintf(t->name, sizeof t->name, "%s", val);
            }
            break;
        case K_DARK:          set_flag(&src, key, val, &t->dark);          break;
        case K_BG_TOP:        set_color(&src, key, val, &t->bg_top);       break;
        case K_BG_BOTTOM:     set_color(&src, key, val, &t->bg_bottom);    break;
        case K_ACCENT:        set_color(&src, key, val, &t->accent);        break;
        case K_ICON_STYLE:    set_style(&src, key, val, &t->icon_style);     break;
        case K_ANIM:          set_anim(&src, key, val, &t->anim);            break;
        case K_ICON_ACCENT:
            /* A malformed highlight is not "no highlight": drop the seen bit
             * so the after-the-loop rule resolves it to this file's accent,
             * whatever order the two keys are written in. */
            if (!parse_color(val, &t->icon_accent)) {
                twarn_at(&src, key, "malformed colour '%s' (want #RRGGBB or "
                         "#AARRGGBB), using the theme's accent", val);
                seen &= ~(1u << K_ICON_ACCENT);
            }
            break;
        case K_GLOW:          set_color(&src, key, val, &t->glow);         break;
        case K_GLOW_STRENGTH:
            set_num(&src, key, val, &t->glow_strength, 0.0f, 1.0f);
            break;
        case K_PANEL:         set_color(&src, key, val, &t->panel);        break;
        case K_PANEL_BORDER:  set_color(&src, key, val, &t->panel_border); break;
        case K_TEXT:          set_color(&src, key, val, &t->text);         break;
        case K_TEXT_DIM:      set_color(&src, key, val, &t->text_dim);     break;
        case K_WINDOW_BG:     set_color(&src, key, val, &t->window_bg);    break;
        case K_WINDOW_BORDER: set_color(&src, key, val, &t->window_border);break;
        case K_CORNER:        set_num(&src, key, val, &t->corner, 0.0f, 48.0f); break;
        case K_BLUR:          set_num(&src, key, val, &t->blur, 0.0f, 1.0f);    break;
        /* 0 means a still desktop; the upper end is a multiplier, not a
         * frame rate, and 3 is as fast as the small sine is pleasant. */
        case K_ANIM_SPEED:    set_num(&src, key, val, &t->anim_speed, 0.0f, 3.0f); break;
        case K_ANIM_AMP:      set_num(&src, key, val, &t->anim_amp, 0.0f, 1.0f);   break;
        case K_FONT_UI:       set_text(&src, key, val, t->font_ui, sizeof t->font_ui); break;
        case K_FONT_MONO:     set_text(&src, key, val, t->font_mono, sizeof t->font_mono); break;
        case K_FONT_SCALE:    set_num(&src, key, val, &t->font_scale, 0.8f, 1.6f);   break;
        default: break;
        }
    }

    int read_error = ferror(f);
    fclose(f);
    if (read_error) twarn_at(&src, NULL, "read error before end of file");

    src.line = 0;
    for (int i = 0; i < K_COUNT; i++) {
        if (!(REQUIRED & (1u << i)) || (seen & (1u << i))) continue;
        twarn_at(&src, key_name[i], "missing, keeping the built-in default");
    }

    /* icon_accent defaults to the theme's own accent rather than to a fixed
     * colour, and it is resolved *after* the loop so the order of the two keys
     * in the file cannot change the result. */
    if (!(seen & (1u << K_ICON_ACCENT))) t->icon_accent = t->accent;

    /* Two independent ways to say "nothing moves": speed 0 and amplitude 0.
     * Normalise both to ANIM_NONE so the frame-request path has one test. */
    if (t->anim_speed <= 0.0f || t->anim_amp <= 0.0f) t->anim = ANIM_NONE;

    /* ----------------------------------------------------------------
     * A window body is the surface an app's pixels and text are drawn on,
     * and the desktop paints it over a blurred copy of whatever is behind
     * the window. A body with alpha lets that blur bleed through, and that
     * is the haze: low-amplitude wallpaper detail sitting on top of the
     * body colour, different under every window, softening every text edge
     * on top of it. It also makes the in-window blur expensive for nothing.
     *
     * So the body is always opaque. Only the alpha is forced - a theme
     * still chooses the colour. The surfaces that are meant to be glass
     * (the top bar, the launcher, the lock panel) are drawn from `panel`
     * and keep their alpha, because they sit on the wallpaper rather than
     * under a window's text.
     * ---------------------------------------------------------------- */
    t->window_bg = WITH_A(t->window_bg, 0xFF);
    return 0;
}

/* -------------------------------------------------- icons and motion --
 * Both of these are theme policy, so they live next to the values they read
 * rather than in the desktop: the host test links this file and measures the
 * same numbers the compositor draws.
 */
static float srgb_lin(u32 v) {
    float c = (float)v / 255.0f;
    return (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* WCAG relative luminance: "the brighter of the two colours" is a measured
 * statement, not whichever channel happens to be largest. */
static float rel_luma(u32 c) {
    return 0.2126f * srgb_lin(GET_R(c)) + 0.7152f * srgb_lin(GET_G(c)) +
           0.0722f * srgb_lin(GET_B(c));
}

/* The two colours an icon is drawn in. An ordinary icon keeps the caller's
 * base colour for the body and the theme's icon_accent for the highlight. A
 * focused or hovered icon is drawn in the theme's two icon colours with the
 * roles assigned by measured luminance: the dimmer colour is the body and the
 * brighter one the highlight. That is the whole fix for the active state -
 * the old pair (body = accent, highlight = "the brighter of the two") made both
 * roles the same colour on every theme whose accent is the brighter one, so a
 * focused icon came out as a one-colour silhouette on eight of the twelve
 * shipped themes. When a theme sets accent == icon_accent the icon stays
 * monochrome on purpose; that is the theme's choice, not a lost highlight. */
IconPair theme_icon_pair(const Theme *t, u32 base, int active) {
    IconPair p;
    p.body = base;
    p.hi = base;
    if (!t) return p;
    if (!active) {
        p.hi = t->icon_accent;
        return p;
    }
    if (rel_luma(t->accent) >= rel_luma(t->icon_accent)) {
        p.body = t->icon_accent;    /* the dimmer half */
        p.hi = t->accent;           /* the brighter half */
    } else {
        p.body = t->accent;
        p.hi = t->icon_accent;
    }
    return p;
}

u32 theme_icon_highlight(const Theme *t, int active) {
    if (!t) return 0u;              /* no theme, no colour */
    return theme_icon_pair(t, 0u, active).hi;
}

float theme_icon_pulse_scale(const Theme *t, double now_sec) {
    if (!t || t->anim == ANIM_NONE ||
        t->anim_speed <= 0.0f || t->anim_amp <= 0.0f) return 1.0f;
    double phase = now_sec * (double)t->anim_speed *
                   (6.283185307179586 * (double)THEME_PULSE_HZ);
    /* Fold before the float cast: the monotonic clock can be days old, and a
     * phase of 10^6 radians held in a float carries quantisation a 3.5 %
     * amplitude would turn into a visible stutter. */
    phase = fmod(phase, 6.283185307179586);
    if (phase < 0.0) phase += 6.283185307179586;
    float p = 0.5f + 0.5f * sinf((float)phase);        /* 0 .. 1 */
    return 1.0f - THEME_PULSE_DEPTH * t->anim_amp * (1.0f - p);
}

/* Font roles: a theme may choose them, but the user's fonts.conf wins. */
int theme_apply_fonts(const Theme *t, int conf_ui_set, int conf_mono_set) {
    if (!t) return 0;
    int applied = 0;
    if (!conf_ui_set && t->font_ui[0]) {
        if (font_set_ui_family(t->font_ui) == 0) applied |= 1;
    }
    if (!conf_mono_set && t->font_mono[0]) {
        if (font_set_mono_family(t->font_mono) == 0) applied |= 2;
    }
    return applied;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Read every *.theme in a directory, in filename order so the numbering in
 * the filenames is the order the user sees (and the order Ctrl+1..9 uses).
 * A file that cannot be opened is reported and skipped; the slots after it
 * are filled by the next file, so one unreadable theme does not blank the
 * rest of the list. */
int theme_load_all(Theme *list, int max, const char *dir) {
    if (!list || !dir || max < 1) return 0;
    if (max > THEME_MAX) {
        fprintf(stderr, "theme: %s: asked for %d themes, the hard ceiling is "
                        "%d\n", dir, max, THEME_MAX);
        max = THEME_MAX;
    }

    DIR *d = opendir(dir);
    if (!d) return 0;

    char *names[THEME_SCAN_MAX];
    int count = 0, too_many = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, ".theme")) continue;
        if (count == THEME_SCAN_MAX) { too_many = 1; break; }
        names[count] = strdup(e->d_name);
        if (!names[count]) break;          /* out of memory: keep what we have */
        count++;
    }
    closedir(d);

    if (count == 0) return 0;
    if (too_many)
        fprintf(stderr, "theme: %s: more than %d theme files, the rest were "
                        "ignored\n", dir, THEME_SCAN_MAX);

    qsort(names, (size_t)count, sizeof names[0], cmp_str);

    int n = 0, skipped = 0;
    char lost[4][64];                  /* first few names, for the warning */
    int lost_shown = 0;
    for (int i = 0; i < count; i++) {
        if (n >= max) {
            if (lost_shown < 4) {
                snprintf(lost[lost_shown], sizeof lost[0], "%s", names[i]);
                lost_shown++;
            }
            skipped++;
            free(names[i]);
            continue;
        }

        char path[700];
        int len = snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (len < 0 || (size_t)len >= sizeof path) {
            fprintf(stderr, "theme: %s/%s: path too long, skipped\n",
                    dir, names[i]);
            free(names[i]);
            continue;
        }
        int rc = theme_load(&list[n], path);
        if (rc == 0) {
            n++;
        } else {
            int err = errno;
            fprintf(stderr, "theme: %s: cannot read (%s), skipped\n",
                    path, strerror(err));
        }
        free(names[i]);
    }

    if (skipped > 0) {
        fprintf(stderr, "theme: %s: %d of %d theme file(s) not loaded - the "
                        "caller only has %d slots (raise the Theme array, see "
                        "docs/THEMES.md):", dir, skipped, count, max);
        for (int i = 0; i < lost_shown; i++) fprintf(stderr, " %s", lost[i]);
        if (skipped > lost_shown) fprintf(stderr, " and %d more", skipped - lost_shown);
        fputc('\n', stderr);
    }
    return n;
}
