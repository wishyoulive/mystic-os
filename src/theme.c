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

/* ------------------------------------------------------------ keys ---- */

enum {
    K_NAME, K_DARK, K_BG_TOP, K_BG_BOTTOM, K_ACCENT, K_GLOW,
    K_GLOW_STRENGTH, K_PANEL, K_PANEL_BORDER, K_TEXT, K_TEXT_DIM,
    K_WINDOW_BG, K_WINDOW_BORDER, K_CORNER, K_BLUR, K_COUNT
};

static const char *const key_name[K_COUNT] = {
    "name",         "dark",        "bg_top",       "bg_bottom",
    "accent",       "glow",        "glow_strength","panel",
    "panel_border", "text",        "text_dim",     "window_bg",
    "window_border","corner",      "blur"
};

/* The keys a theme is expected to set. The rest have defaults that are safe
 * on any background (the glow, the two border alphas, the corner radius and
 * the blur strength). These nine decide whether the theme is readable, so a
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
        case K_ACCENT:        set_color(&src, key, val, &t->accent);       break;
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
