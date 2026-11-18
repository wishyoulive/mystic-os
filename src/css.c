/* ============================================================
 *  Mystic OS - css.c
 *  A real, small CSS engine: parse, cascade, inherit.
 *
 *  This file is the whole implementation of css.h. It has no
 *  dependency beyond <string.h>: no malloc, no locale, no file I/O,
 *  no globals that change after the built-in sheet is parsed once.
 *
 *  The shape of the parser is deliberately boring: a cursor over the
 *  bytes, whitespace and comments skipped in exactly one place, a
 *  rule prelude scanned up to its '{', a declaration block read to
 *  its '}', and a recovery rule that consumes the broken construct
 *  and carries on with the next one. Nothing throws the rest of the
 *  sheet away because one rule is wrong - a browser that stops at the
 *  first malformed rule renders an unstyled page, which is worse than
 *  rendering a page with one rule missing.
 *
 *  RECOVERY, PRECISELY
 *
 *    - a rule with no '{' at all is dropped (or, at EOF, the bytes
 *      are a rule with no block: dropped);
 *    - a selector outside the subset drops that one selector; the
 *      other selectors of the same comma group still get the block;
 *    - a declaration with no ':' or an empty value is dropped, and
 *      the reader resynchronises at the next ';' or '}';
 *    - a declaration with a property or a value we cannot use is
 *      dropped, and the next declaration is read normally;
 *    - an at-rule is skipped as a unit, including a balanced block
 *      and any strings or comments inside it.
 *
 *  Every one of those is counted in sheet->stats and the first is
 *  kept as a line-numbered sentence, so a bad stylesheet is visible
 *  without being fatal.
 * ============================================================ */
#include "css.h"

#include <string.h>

/* ------------------------------------------------------------
 *  Small ASCII helpers. The C locale is never consulted: CSS is
 *  ASCII-keyword based and a Turkish locale must not change what
 *  "ITALIC" means.
 * ------------------------------------------------------------ */
static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int tok_prefix(const char *s, size_t n, const char *lit)
{
    size_t i = 0;
    while (lit[i]) {
        if (i >= n || ascii_lower((unsigned char)s[i]) != ascii_lower((unsigned char)lit[i]))
            return 0;
        i++;
    }
    return 1;
}

static int tok_eq(const char *s, size_t n, const char *lit)
{
    return tok_prefix(s, n, lit) && lit[n] == 0;
}

static int is_name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           c == '-' || c == '_' || c >= 0x80;
}

/* ------------------------------------------------------------
 *  The CSS colour names, all 148 of CSS Color 4, plus transparent
 *  handled by the parser. Stored as 0xRRGGBB; alpha is added at the
 *  lookup, because every named colour is opaque.
 * ------------------------------------------------------------ */
typedef struct { const char *name; u32 rgb; } CssNamed;

static const CssNamed kNamed[] = {
    { "aliceblue",            0xF0F8FF }, { "antiquewhite",         0xFAEBD7 },
    { "aqua",                 0x00FFFF }, { "aquamarine",           0x7FFFD4 },
    { "azure",                0xF0FFFF }, { "beige",                0xF5F5DC },
    { "bisque",               0xFFE4C4 }, { "black",                0x000000 },
    { "blanchedalmond",       0xFFEBCD }, { "blue",                 0x0000FF },
    { "blueviolet",           0x8A2BE2 }, { "brown",                0xA52A2A },
    { "burlywood",            0xDEB887 }, { "cadetblue",            0x5F9EA0 },
    { "chartreuse",           0x7FFF00 }, { "chocolate",            0xD2691E },
    { "coral",                0xFF7F50 }, { "cornflowerblue",       0x6495ED },
    { "cornsilk",             0xFFF8DC }, { "crimson",              0xDC143C },
    { "cyan",                 0x00FFFF }, { "darkblue",             0x00008B },
    { "darkcyan",             0x008B8B }, { "darkgoldenrod",        0xB8860B },
    { "darkgray",             0xA9A9A9 }, { "darkgreen",            0x006400 },
    { "darkgrey",             0xA9A9A9 }, { "darkkhaki",            0xBDB76B },
    { "darkmagenta",          0x8B008B }, { "darkolivegreen",       0x556B2F },
    { "darkorange",           0xFF8C00 }, { "darkorchid",           0x9932CC },
    { "darkred",              0x8B0000 }, { "darksalmon",           0xE9967A },
    { "darkseagreen",         0x8FBC8F }, { "darkslateblue",        0x483D8B },
    { "darkslategray",        0x2F4F4F }, { "darkslategrey",        0x2F4F4F },
    { "darkturquoise",        0x00CED1 }, { "darkviolet",           0x9400D3 },
    { "deeppink",             0xFF1493 }, { "deepskyblue",          0x00BFFF },
    { "dimgray",              0x696969 }, { "dimgrey",              0x696969 },
    { "dodgerblue",           0x1E90FF }, { "firebrick",            0xB22222 },
    { "floralwhite",          0xFFFAF0 }, { "forestgreen",          0x228B22 },
    { "fuchsia",              0xFF00FF }, { "gainsboro",            0xDCDCDC },
    { "ghostwhite",           0xF8F8FF }, { "gold",                 0xFFD700 },
    { "goldenrod",            0xDAA520 }, { "gray",                 0x808080 },
    { "green",                0x008000 }, { "greenyellow",          0xADFF2F },
    { "grey",                 0x808080 }, { "honeydew",             0xF0FFF0 },
    { "hotpink",              0xFF69B4 }, { "indianred",            0xCD5C5C },
    { "indigo",               0x4B0082 }, { "ivory",                0xFFFFF0 },
    { "khaki",                0xF0E68C }, { "lavender",             0xE6E6FA },
    { "lavenderblush",        0xFFF0F5 }, { "lawngreen",            0x7CFC00 },
    { "lemonchiffon",         0xFFFACD }, { "lightblue",            0xADD8E6 },
    { "lightcoral",           0xF08080 }, { "lightcyan",            0xE0FFFF },
    { "lightgoldenrodyellow", 0xFAFAD2 }, { "lightgray",            0xD3D3D3 },
    { "lightgreen",           0x90EE90 }, { "lightgrey",            0xD3D3D3 },
    { "lightpink",            0xFFB6C1 }, { "lightsalmon",          0xFFA07A },
    { "lightseagreen",        0x20B2AA }, { "lightskyblue",         0x87CEFA },
    { "lightslategray",       0x778899 }, { "lightslategrey",       0x778899 },
    { "lightsteelblue",       0xB0C4DE }, { "lightyellow",          0xFFFFE0 },
    { "lime",                 0x00FF00 }, { "limegreen",            0x32CD32 },
    { "linen",                0xFAF0E6 }, { "magenta",              0xFF00FF },
    { "maroon",               0x800000 }, { "mediumaquamarine",     0x66CDAA },
    { "mediumblue",           0x0000CD }, { "mediumorchid",         0xBA55D3 },
    { "mediumpurple",         0x9370DB }, { "mediumseagreen",       0x3CB371 },
    { "mediumslateblue",      0x7B68EE }, { "mediumspringgreen",    0x00FA9A },
    { "mediumturquoise",      0x48D1CC }, { "mediumvioletred",      0xC71585 },
    { "midnightblue",         0x191970 }, { "mintcream",            0xF5FFFA },
    { "mistyrose",            0xFFE4E1 }, { "moccasin",             0xFFE4B5 },
    { "navajowhite",          0xFFDEAD }, { "navy",                 0x000080 },
    { "oldlace",              0xFDF5E6 }, { "olive",                0x808000 },
    { "olivedrab",            0x6B8E23 }, { "orange",               0xFFA500 },
    { "orangered",            0xFF4500 }, { "orchid",               0xDA70D6 },
    { "palegoldenrod",        0xEEE8AA }, { "palegreen",            0x98FB98 },
    { "paleturquoise",        0xAFEEEE }, { "palevioletred",        0xDB7093 },
    { "papayawhip",           0xFFEFD5 }, { "peachpuff",            0xFFDAB9 },
    { "peru",                 0xCD853F }, { "pink",                 0xFFC0CB },
    { "plum",                 0xDDA0DD }, { "powderblue",           0xB0E0E6 },
    { "purple",               0x800080 }, { "rebeccapurple",        0x663399 },
    { "red",                  0xFF0000 }, { "rosybrown",            0xBC8F8F },
    { "royalblue",            0x4169E1 }, { "saddlebrown",          0x8B4513 },
    { "salmon",               0xFA8072 }, { "sandybrown",           0xF4A460 },
    { "seagreen",             0x2E8B57 }, { "seashell",             0xFFF5EE },
    { "sienna",               0xA0522D }, { "silver",               0xC0C0C0 },
    { "skyblue",              0x87CEEB }, { "slateblue",            0x6A5ACD },
    { "slategray",            0x708090 }, { "slategrey",            0x708090 },
    { "snow",                 0xFFFAFA }, { "springgreen",          0x00FF7F },
    { "steelblue",            0x4682B4 }, { "tan",                  0xD2B48C },
    { "teal",                 0x008080 }, { "thistle",              0xD8BFD8 },
    { "tomato",               0xFF6347 }, { "turquoise",            0x40E0D0 },
    { "violet",               0xEE82EE }, { "wheat",                0xF5DEB3 },
    { "white",                0xFFFFFF }, { "whitesmoke",           0xF5F5F5 },
    { "yellow",               0xFFFF00 }, { "yellowgreen",          0x9ACD32 }
};

/* ------------------------------------------------------------
 *  Colour parsing: #rgb #rgba #rrggbb #rrggbbaa, rgb()/rgba(),
 *  names, transparent. The result is 0xAARRGGBB, the OS word.
 * ------------------------------------------------------------ */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_comp(const char **pp, const char *end, float *val, int *pct)
{
    const char *p = *pp;
    while (p < end && is_space((unsigned char)*p)) p++;
    int digits = 0;
    float v = 0.0f;
    while (p < end && is_digit((unsigned char)*p)) { v = v * 10.0f + (float)(*p - '0'); p++; digits++; }
    if (p < end && *p == '.') {
        p++;
        float f = 0.1f;
        while (p < end && is_digit((unsigned char)*p)) { v += (float)(*p - '0') * f; f *= 0.1f; p++; digits++; }
    }
    if (!digits) return 0;
    *pct = 0;
    if (p < end && *p == '%') { *pct = 1; p++; }
    *val = v;
    *pp = p;
    return 1;
}

static int clamp255(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (int)(v + 0.5f);
}

int css_color(const char *text, size_t len, u32 *out)
{
    if (!text || !out) return 0;
    while (len && is_space((unsigned char)text[0])) { text++; len--; }
    while (len && is_space((unsigned char)text[len - 1])) len--;
    if (!len) return 0;

    if (text[0] == '#') {
        if (len != 4 && len != 5 && len != 7 && len != 9) return 0;
        unsigned v[8];
        size_t k;
        for (k = 1; k < len; k++) {
            int h = hexval((unsigned char)text[k]);
            if (h < 0) return 0;
            v[k - 1] = (unsigned)h;
        }
        u32 r, g, b, a = 0xFF;
        if (len == 4) {
            r = (u32)(v[0] * 17); g = (u32)(v[1] * 17); b = (u32)(v[2] * 17);
        } else if (len == 5) {
            r = (u32)(v[0] * 17); g = (u32)(v[1] * 17); b = (u32)(v[2] * 17);
            a = (u32)(v[3] * 17);
        } else if (len == 7) {
            r = (u32)(v[0] * 16 + v[1]); g = (u32)(v[2] * 16 + v[3]);
            b = (u32)(v[4] * 16 + v[5]);
        } else {
            r = (u32)(v[0] * 16 + v[1]); g = (u32)(v[2] * 16 + v[3]);
            b = (u32)(v[4] * 16 + v[5]); a = (u32)(v[6] * 16 + v[7]);
        }
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return 1;
    }

    if (len > 5 && (tok_prefix(text, len, "rgb(") || tok_prefix(text, len, "rgba("))) {
        int is_rgba = tok_prefix(text, len, "rgba(");
        const char *p = text + (is_rgba ? 5 : 4);
        const char *end = text + len;
        if (end <= p || end[-1] != ')') return 0;
        end--;
        float v[4];
        int pct[4];
        int n = 0;
        for (;;) {
            if (n >= 4) return 0;
            if (!parse_comp(&p, end, &v[n], &pct[n])) return 0;
            n++;
            while (p < end && is_space((unsigned char)*p)) p++;
            if (p < end && *p == ',') { p++; continue; }
            break;
        }
        while (p < end && is_space((unsigned char)*p)) p++;
        if (p != end) return 0;
        if (n != 3 && !(is_rgba && n == 4)) return 0;
        u32 c;
        int r = pct[0] ? clamp255(v[0] * 255.0f / 100.0f) : clamp255(v[0]);
        int g = pct[1] ? clamp255(v[1] * 255.0f / 100.0f) : clamp255(v[1]);
        int b = pct[2] ? clamp255(v[2] * 255.0f / 100.0f) : clamp255(v[2]);
        int a = 0xFF;
        if (n == 4) a = pct[3] ? clamp255(v[3] * 255.0f / 100.0f)
                               : (int)(v[3] * 255.0f + 0.5f);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        c = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        *out = c;
        return 1;
    }

    if (tok_eq(text, len, "transparent")) { *out = 0; return 1; }

    {
        char name[32];
        size_t i;
        if (len >= sizeof name) return 0;
        for (i = 0; i < len; i++) name[i] = (char)ascii_lower((unsigned char)text[i]);
        name[len] = 0;
        for (i = 0; i < sizeof kNamed / sizeof kNamed[0]; i++) {
            if (!strcmp(name, kNamed[i].name)) {
                *out = 0xFF000000u | kNamed[i].rgb;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------
 *  Lengths and integers. Whole string must be consumed.
 * ------------------------------------------------------------ */
static int parse_length(const char *s, int *out)
{
    const char *p = s;
    int sign = 1;
    float v = 0.0f;
    int digits = 0, is_pt = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    while (is_digit((unsigned char)*p)) { v = v * 10.0f + (float)(*p - '0'); p++; digits++; }
    if (*p == '.') {
        float f = 0.1f;
        p++;
        while (is_digit((unsigned char)*p)) { v += (float)(*p - '0') * f; f *= 0.1f; p++; digits++; }
    }
    if (!digits) return 0;
    if (p[0] == 'p' && p[1] == 'x') p += 2;
    else if (p[0] == 'p' && p[1] == 't') { is_pt = 1; p += 2; }
    else if (p[0] && p[0] != ' ') return 0;      /* em, %, rem: not in the subset */
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;
    if (is_pt) v = v * 4.0f / 3.0f;
    v = sign * v;
    if (v > 10000.0f) v = 10000.0f;
    if (v < -10000.0f) v = -10000.0f;
    *out = (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
    return 1;
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    const char *p = s;
    int sign = 1;
    long v = 0;
    int digits = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    while (is_digit((unsigned char)*p)) { v = v * 10 + (*p - '0'); if (v > 1000000) v = 1000000; p++; digits++; }
    while (*p == ' ' || *p == '\t') p++;
    if (!digits || *p) return 0;
    v *= sign;
    if (v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

/* A bare percentage, 0..1000: "60%". Used by width/max-width, where a
 * percentage has to wait for the renderer to know its column, and by
 * line-height, where it is relative to the element's own font size. */
static int parse_percent(const char *s, int *out)
{
    const char *p = s;
    float v = 0.0f;
    int digits = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (is_digit((unsigned char)*p)) { v = v * 10.0f + (float)(*p - '0'); p++; digits++; }
    if (*p == '.') {
        float f = 0.1f;
        p++;
        while (is_digit((unsigned char)*p)) { v += (float)(*p - '0') * f; f *= 0.1f; p++; digits++; }
    }
    if (!digits) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '%') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;
    if (v < 0.0f) v = 0.0f;
    if (v > 1000.0f) v = 1000.0f;
    *out = (int)(v + 0.5f);
    return 1;
}

/* Defined with the declaration values below; the box and font-size
 * shorthands need it before then. */
static int next_word(const char **pp, char *out, int cap);

/* A unitless number: "1.5". line-height is the only property that takes
 * one, and there it means a multiple of the element's font size. */
static int parse_multiplier(const char *s, int *pct)
{
    const char *p = s;
    float v = 0.0f;
    int digits = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (is_digit((unsigned char)*p)) { v = v * 10.0f + (float)(*p - '0'); p++; digits++; }
    if (*p == '.') {
        float f = 0.1f;
        p++;
        while (is_digit((unsigned char)*p)) { v += (float)(*p - '0') * f; f *= 0.1f; p++; digits++; }
    }
    if (!digits) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;                       /* a unit means it is not a multiplier */
    v *= 100.0f;
    if (v < 1.0f) v = 1.0f;
    if (v > 1000.0f) v = 1000.0f;
    *pct = (int)(v + 0.5f);
    return 1;
}

/* "1.2rem" -> 120, i.e. the percentage parse_percent() gives for
 * "120%", but measured against the root element instead of the parent.
 * Returns 0 when the suffix is not rem. */
/* A bare number in hundredths: "1.25" -> 125, "60" -> 6000. Unlike
 * parse_multiplier() this is for lengths, not line-height percentages, so
 * 60em (a 960 px column) must survive: the ceiling is 1000em. */
static int parse_number_100(const char *s, int *out)
{
    const char *p = s;
    float v = 0.0f;
    int digits = 0;
    while (*p == ' ' || *p == '	') p++;
    while (is_digit((unsigned char)*p)) { v = v * 10.0f + (float)(*p - '0'); p++; digits++; }
    if (*p == '.') {
        float f = 0.1f;
        p++;
        while (is_digit((unsigned char)*p)) { v += (float)(*p - '0') * f; f *= 0.1f; p++; digits++; }
    }
    while (*p == ' ' || *p == '	') p++;
    if (!digits || *p) return 0;
    v *= 100.0f;
    if (v > 100000.0f) v = 100000.0f;
    if (v < -100000.0f) v = -100000.0f;
    *out = (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
    return 1;
}

/* "1.2rem" -> 120, i.e. the percentage parse_percent() gives for
 * "120%", but measured against the root element instead of the parent.
 * Returns 0 when the suffix is not rem. */
static int parse_rem(const char *s, int *pct)
{
    char buf[40];
    size_t n = 0;
    const char *p = s;
    while (*p == ' ' || *p == '	') p++;
    while (*p && n + 1 < sizeof buf) buf[n++] = *p++;
    if (n < 4) return 0;
    if (buf[n - 3] != 'r' || buf[n - 2] != 'e' || buf[n - 1] != 'm') return 0;
    buf[n - 3] = 0;
    return parse_number_100(buf, pct);
}

/* "1.2em" -> 120: em is the parent's own size, which is what the
 * percentage form means for font-size. Returns 0 when the unit is not em. */
static int parse_em(const char *s, int *pct)
{
    char buf[40];
    size_t n = 0;
    const char *p = s;
    while (*p == ' ' || *p == '	') p++;
    while (*p && n + 1 < sizeof buf) buf[n++] = *p++;
    if (n < 3) return 0;
    if (buf[n - 2] != 'e' || buf[n - 1] != 'm') return 0;
    buf[n - 2] = 0;
    return parse_number_100(buf, pct);
}

/* font-size: an absolute length, em/% of the parent, rem of the root, or
 * one of the absolute keywords. The relative forms are stored as a
 * percentage here and resolved by css_for(), which is the only place that
 * knows the parent's resolved size. */
static int apply_font_size(CssStyle *st, const char *val)
{
    static const struct { const char *name; int pct; } kSizes[] = {
        { "xx-small", 60 }, { "x-small", 75 }, { "small", 89 },
        { "medium", 100 }, { "large", 120 }, { "x-large", 150 },
        { "xx-large", 200 }, { "xxx-large", 250 }
    };
    int px = 0, pct = 0;

    for (size_t i = 0; i < sizeof kSizes / sizeof kSizes[0]; i++) {
        if (!ieq(val, kSizes[i].name)) continue;
        st->set &= ~(CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REM);
        st->font_size_pct = kSizes[i].pct;
        st->set |= CSS_SET_FONT_SIZE_REL;
        return 1;
    }
    if (ieq(val, "smaller") || ieq(val, "larger")) {
        st->set &= ~(CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REM);
        st->font_size_pct = ieq(val, "smaller") ? 83 : 120;
        st->set |= CSS_SET_FONT_SIZE_REL;
        return 1;
    }
    if (parse_rem(val, &pct)) {
        st->set &= ~(CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REL);
        st->font_size_pct = pct;
        st->set |= CSS_SET_FONT_SIZE_REM;
        return 1;
    }
    if (parse_em(val, &pct) || parse_percent(val, &pct)) {
        st->set &= ~(CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REM);
        st->font_size_pct = pct;
        st->set |= CSS_SET_FONT_SIZE_REL;
        return 1;
    }
    if (parse_length(val, &px) && px > 0) {
        st->set &= ~(CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM);
        st->font_size = px;
        st->set |= CSS_SET_FONT_SIZE;
        return 1;
    }
    return 0;
}

/* display. Only none / block / list-item change a text renderer's
 * output; the rest are recognised so that a real stylesheet's
 * `display:table` is not a parse error, and are treated as inline. */
static int parse_display(const char *val, int *out)
{
    if (ieq(val, "none")) *out = CSS_DISPLAY_NONE;
    else if (ieq(val, "block")) *out = CSS_DISPLAY_BLOCK;
    else if (ieq(val, "list-item")) *out = CSS_DISPLAY_LIST_ITEM;
    else if (ieq(val, "inline")) *out = CSS_DISPLAY_INLINE;
    else if (ieq(val, "inline-block")) *out = CSS_DISPLAY_INLINE_BLOCK;
    else if (ieq(val, "table")) *out = CSS_DISPLAY_TABLE;
    else if (ieq(val, "table-row")) *out = CSS_DISPLAY_TABLE_ROW;
    else if (ieq(val, "table-cell")) *out = CSS_DISPLAY_TABLE_CELL;
    /* A flow container is a block with one axis: flex and grid lay their
     * children out as a block box in this renderer, which is what a
     * single column actually is. Saying so is better than dropping the
     * container entirely. */
    else if (ieq(val, "flex") || ieq(val, "grid") ||
             ieq(val, "flow-root") || ieq(val, "table-caption"))
        *out = CSS_DISPLAY_BLOCK;
    else if (ieq(val, "inline-flex") || ieq(val, "inline-grid") ||
             ieq(val, "contents"))
        *out = CSS_DISPLAY_INLINE;
    else return 0;
    return 1;
}

/* A box length: px, pt, or em. Returns 1 with *out set and *rel saying
 * which unit it is: 0 for an absolute px value, 1 when *out is in
 * hundredths of an em and needs the element's font size. `rem` is left
 * to font-size alone, where the root size is reachable. */
static int parse_len(const char *s, int *out, int *rel)
{
    *rel = 0;
    if (parse_length(s, out)) return 1;
    if (parse_em(s, out)) { *rel = 1; return 1; }
    return 0;
}

/* One to four values, in CSS's expansion:
 *   1 value  -> all four sides
 *   2 values -> top+bottom, left+right
 *   3 values -> top, left+right, bottom
 *   4 values -> top, right, bottom, left
 * `auto` is 0: a text column has nothing to distribute a free-space
 * margin into, and centring is what width/max-width already do. */
static int apply_box_shorthand(CssStyle *st, const char *val,
                               int *top, unsigned btop, unsigned rtop,
                               int *right, unsigned bright, unsigned rright,
                               int *bottom, unsigned bbottom, unsigned rbottom,
                               int *left, unsigned bleft, unsigned rleft,
                               int allow_negative)
{
    int v[4], rel[4];
    int n = 0;
    const char *p = val;
    char tok[32];
    while (next_word(&p, tok, sizeof tok)) {
        int px = 0, r = 0;
        if (!ieq(tok, "auto") && !parse_len(tok, &px, &r)) return 0;
        if (!allow_negative && px < 0) px = 0;
        if (n >= 4) return 0;                 /* CSS takes at most four */
        rel[n] = r;
        v[n++] = px;
    }
    if (n == 0) return 0;
    int t, r, b, l, rt = 0, rr = 0, rb = 0, rl = 0;
    if (n == 1)      { t = r = b = l = v[0]; rt = rr = rb = rl = rel[0]; }
    else if (n == 2) { t = b = v[0]; r = l = v[1];
                       rt = rb = rel[0]; rr = rl = rel[1]; }
    else if (n == 3) { t = v[0]; r = l = v[1]; b = v[2];
                       rt = rel[0]; rr = rl = rel[1]; rb = rel[2]; }
    else             { t = v[0]; r = v[1]; b = v[2]; l = v[3];
                       rt = rel[0]; rr = rel[1]; rb = rel[2]; rl = rel[3]; }
    *top = t; *right = r; *bottom = b; *left = l;
    st->set |= btop | bright | bbottom | bleft;
    st->rel = (st->rel & ~(rtop | rright | rbottom | rleft)) |
              (rt ? rtop : 0) | (rr ? rright : 0) |
              (rb ? rbottom : 0) | (rl ? rleft : 0);
    return 1;
}

/* The first token of a value that is a colour, or 0. `background`
 * carries an image, a position and a repeat mode as well as a colour, so
 * the whole-value parser is the wrong tool for it. */
static int first_color(const char *val, u32 *out)
{
    const char *p = val;
    char tok[64];
    while (next_word(&p, tok, sizeof tok)) {
        if (css_color(tok, strlen(tok), out)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------
 *  The cursor.
 * ------------------------------------------------------------ */
typedef struct {
    const char *s;
    size_t n, i;
    int line;
} Cur;

static int cpeek(const Cur *c)
{
    return c->i < c->n ? (unsigned char)c->s[c->i] : -1;
}

static void cadv(Cur *c)
{
    if (c->i >= c->n) return;
    if (c->s[c->i] == '\n') c->line++;
    c->i++;
}

static void cskip_ws(Cur *c)
{
    for (;;) {
        int ch = cpeek(c);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v') {
            cadv(c);
            continue;
        }
        if (ch == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') {
            cadv(c);
            cadv(c);
            while (c->i < c->n) {
                if (c->s[c->i] == '*' && c->i + 1 < c->n && c->s[c->i + 1] == '/') {
                    cadv(c);
                    cadv(c);
                    break;
                }
                cadv(c);
            }
            continue;
        }
        break;
    }
}

static void skip_string(Cur *c, int quote)
{
    cadv(c);                       /* the opening quote */
    while (c->i < c->n) {
        int ch = cpeek(c);
        cadv(c);
        if (ch == '\\' && c->i < c->n) { cadv(c); continue; }
        if (ch == quote) return;
    }
}

/* The '{' has already been consumed. */
static void skip_balanced(Cur *c)
{
    int depth = 1;
    while (c->i < c->n) {
        int ch = cpeek(c);
        if (ch == '"' || ch == '\'') { skip_string(c, ch); continue; }
        if (ch == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') { cskip_ws(c); continue; }
        cadv(c);
        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) return;
        }
    }
}

/* How deep @media blocks may nest before the rest is skipped. Real sheets
 * never go past two; a hostile one must not recurse without end. */
#define CSS_MEDIA_DEPTH 3

/* ------------------------------------------------------------
 *  Statistics. One line, the first problem, with its real line.
 * ------------------------------------------------------------ */
static char *put_uint(char *p, int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_str(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static void note(CssSheet *sh, int line, const char *what, const char *detail)
{
    if (!sh) return;
    sh->stats.ignored++;
    if (sh->stats.first_problem[0]) return;
    char *p = sh->stats.first_problem;
    char *end = p + sizeof sh->stats.first_problem - 1;
    p = put_str(p, end, "line ");
    p = put_uint(p, line);
    p = put_str(p, end, ": ");
    p = put_str(p, end, what);
    if (detail && *detail) {
        p = put_str(p, end, " '");
        p = put_str(p, end, detail);
        p = put_str(p, end, "'");
    }
    *p = 0;
}

/* ------------------------------------------------------------
 *  Selectors.
 *
 *  A prelude is split on top-level commas first, then each piece is
 *  parsed on its own. That way `h1, h2:hover, p` keeps h1 and p and
 *  only loses the middle selector.
 * ------------------------------------------------------------ */
static int read_name(Cur *c, char *dst, int cap, int lower)
{
    int n = 0, over = 0;
    for (;;) {
        int ch = cpeek(c);
        if (ch < 0 || !is_name_char(ch)) break;
        if (n < cap - 1) dst[n++] = (char)(lower ? ascii_lower(ch) : ch);
        else over = 1;
        cadv(c);
    }
    dst[n] = 0;
    return n > 0 && !over;
}

static int parse_selector(Cur *c, CssSelector *sel)
{
    sel->nparts = 0;
    sel->specificity = 0;

    for (;;) {
        cskip_ws(c);
        int ch = cpeek(c);
        if (ch < 0 || c->i >= c->n) break;
        if (sel->nparts >= CSS_MAX_SEL_PARTS) return 0;

        CssCompound *cp = &sel->part[sel->nparts];
        memset(cp, 0, sizeof *cp);

        int got = 0, star = 0;
        for (;;) {
            ch = cpeek(c);
            if (ch < 0) break;
            if (ch == '*') {
                if (star) return 0;
                cadv(c);
                star = 1;
                got = 1;
                continue;
            }
            if (ch == '.') {
                if (cp->cls[0]) return 0;
                cadv(c);
                if (!read_name(c, cp->cls, CSS_CLASS_MAX, 0)) return 0;
                got = 1;
                continue;
            }
            if (ch == '#') {
                if (cp->id[0]) return 0;
                cadv(c);
                if (!read_name(c, cp->id, CSS_ID_MAX, 0)) return 0;
                got = 1;
                continue;
            }
            if (is_name_char(ch) && !cp->tag[0] && !star) {
                if (!read_name(c, cp->tag, CSS_TAG_MAX, 1)) return 0;
                got = 1;
                continue;
            }
            break;
        }
        if (!got) return 0;

        /* Whatever follows the compound without whitespace must be a
           separator we understand, or the selector is outside the subset
           (':hover', '>', '[attr]', '+', '~', ...). A comment is allowed
           because cskip_ws() eats it. */
        ch = cpeek(c);
        if (ch >= 0 && !is_space(ch) && ch != ',' && ch != '{' && ch != '/') return 0;

        if (cp->id[0]) sel->specificity += 10000;
        if (cp->cls[0]) sel->specificity += 100;
        if (cp->tag[0]) sel->specificity += 1;
        sel->nparts++;
    }
    return sel->nparts > 0;
}

static size_t next_comma(const char *s, size_t i, size_t end)
{
    int quote = 0;
    while (i < end) {
        int ch = (unsigned char)s[i];
        if (quote) {
            if (ch == '\\' && i + 1 < end) { i += 2; continue; }
            if (ch == quote) quote = 0;
            i++;
            continue;
        }
        if (ch == '"' || ch == '\'') { quote = ch; i++; continue; }
        if (ch == '/' && i + 1 < end && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < end && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < end) ? i + 2 : end;
            continue;
        }
        if (ch == ',') return i;
        i++;
    }
    return end;
}

/* ------------------------------------------------------------
 *  Declaration values.
 * ------------------------------------------------------------ */
static int next_word(const char **pp, char *out, int cap)
{
    const char *p = *pp;
    while (*p && is_space((unsigned char)*p)) p++;
    if (!*p) { *pp = p; return 0; }
    int n = 0;
    while (*p && !is_space((unsigned char)*p)) {
        if (n < cap - 1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

/* Read up to ';' or '}' at paren depth 0, strip comments, collapse
   whitespace, keep quoted text intact. Returns the length, or -1 if the
   value did not fit. */
static int scan_value(Cur *c, char *buf, int cap)
{
    int n = 0, over = 0, quote = 0, depth = 0;
    for (;;) {
        int ch = cpeek(c);
        if (ch < 0) break;
        if (quote) {
            cadv(c);
            if (n < cap - 1) buf[n++] = (char)ch; else over = 1;
            if (ch == '\\' && c->i < c->n) {
                ch = cpeek(c);
                cadv(c);
                if (n < cap - 1) buf[n++] = (char)ch; else over = 1;
            } else if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            cadv(c);
            if (n < cap - 1) buf[n++] = (char)ch; else over = 1;
            continue;
        }
        if (ch == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') {
            cskip_ws(c);
            if (n > 0 && n < cap - 1) buf[n++] = ' ';
            else if (n >= cap - 1) over = 1;
            continue;
        }
        if (ch == '(') { depth++; cadv(c); if (n < cap - 1) buf[n++] = '('; else over = 1; continue; }
        if (ch == ')') { if (depth > 0) depth--; cadv(c); if (n < cap - 1) buf[n++] = ')'; else over = 1; continue; }
        if (depth == 0 && (ch == ';' || ch == '}')) break;
        cadv(c);
        if (is_space(ch)) {
            if (n > 0 && buf[n - 1] != ' ') { if (n < cap - 1) buf[n++] = ' '; else over = 1; }
            continue;
        }
        if (n < cap - 1) buf[n++] = (char)ch; else over = 1;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    return over ? -1 : n;
}

static int border_style(const char *word, int *out)
{
    if (ieq(word, "none") || ieq(word, "hidden")) { *out = CSS_BORDER_NONE; return 1; }
    if (ieq(word, "solid")) { *out = CSS_BORDER_SOLID; return 1; }
    if (ieq(word, "dashed")) { *out = CSS_BORDER_DASHED; return 1; }
    if (ieq(word, "dotted")) { *out = CSS_BORDER_DOTTED; return 1; }
    if (ieq(word, "double")) { *out = CSS_BORDER_DOUBLE; return 1; }
    /* The 3D styles have no 3D to draw on: a text renderer shows the
       same single line, so they map to solid rather than vanishing. */
    if (ieq(word, "groove") || ieq(word, "ridge") ||
        ieq(word, "inset") || ieq(word, "outset")) { *out = CSS_BORDER_SOLID; return 1; }
    return 0;
}

static int parse_border_width(const char *word, int *out)
{
    if (ieq(word, "thin")) { *out = 1; return 1; }
    if (ieq(word, "medium")) { *out = 3; return 1; }
    if (ieq(word, "thick")) { *out = 5; return 1; }
    if (parse_length(word, out)) return *out >= 0;
    return 0;
}

static int apply_border_shorthand(CssStyle *st, const char *val, int top)
{
    int w = 3, style = CSS_BORDER_NONE, have_c = 0;
    u32 col = 0;
    const char *p = val;
    char tok[40];
    int any = 0;
    while (next_word(&p, tok, sizeof tok)) {
        u32 c;
        any = 1;
        if (parse_border_width(tok, &w)) continue;
        if (border_style(tok, &style)) continue;
        if (css_color(tok, strlen(tok), &c)) { col = c; have_c = 1; continue; }
        return 0;
    }
    if (!any) return 0;
    if (top) {
        st->border_top_w = w;
        st->set |= CSS_SET_BORDER_TOP_W;
        st->border_top_style = style;
        st->set |= CSS_SET_BORDER_TOP_S;
        if (have_c) { st->border_top_color = col; st->set |= CSS_SET_BORDER_TOP_C; }
    } else {
        st->border_bottom_w = w;
        st->set |= CSS_SET_BORDER_BOTTOM_W;
        st->border_bottom_style = style;
        st->set |= CSS_SET_BORDER_BOTTOM_S;
        if (have_c) { st->border_bottom_color = col; st->set |= CSS_SET_BORDER_BOTTOM_C; }
    }
    return 1;
}

static int parse_list_style(const char *val, int *out)
{
    if (ieq(val, "none")) *out = CSS_LIST_NONE;
    else if (ieq(val, "disc")) *out = CSS_LIST_DISC;
    else if (ieq(val, "circle")) *out = CSS_LIST_CIRCLE;
    else if (ieq(val, "square")) *out = CSS_LIST_SQUARE;
    else if (ieq(val, "decimal")) *out = CSS_LIST_DECIMAL;
    else if (ieq(val, "lower-alpha") || ieq(val, "lower-latin")) *out = CSS_LIST_LOWER_ALPHA;
    else if (ieq(val, "upper-alpha") || ieq(val, "upper-latin")) *out = CSS_LIST_UPPER_ALPHA;
    else if (ieq(val, "lower-roman")) *out = CSS_LIST_LOWER_ROMAN;
    else if (ieq(val, "upper-roman")) *out = CSS_LIST_UPPER_ROMAN;
    else return 0;
    return 1;
}

static int parse_white_space(const char *val, int *out)
{
    if (ieq(val, "normal")) *out = CSS_WS_NORMAL;
    else if (ieq(val, "pre")) *out = CSS_WS_PRE;
    else if (ieq(val, "nowrap")) *out = CSS_WS_NOWRAP;
    else if (ieq(val, "pre-wrap")) *out = CSS_WS_PRE_WRAP;
    else if (ieq(val, "pre-line")) *out = CSS_WS_PRE_LINE;
    else return 0;
    return 1;
}

/* Apply one declaration. Returns 1 if it took effect, 0 if it was
   dropped (and counted), so css_inline() can report a number. */
static int decl_apply(CssStyle *st, const char *name, const char *val,
                      int line, CssSheet *sh)
{
    u32 c;
    int n;

    if (ieq(name, "color")) {
        if (ieq(val, "currentcolor")) return 1;    /* means: keep the inherited colour */
        if (!css_color(val, strlen(val), &c)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->color = c;
        st->set |= CSS_SET_COLOR;
        return 1;
    }
    if (ieq(name, "background-color")) {
        if (!css_color(val, strlen(val), &c)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->background = c;
        st->set |= CSS_SET_BACKGROUND;
        return 1;
    }
    if (ieq(name, "background")) {
        /* `background: #fff url(x.png) no-repeat center` - the colour is
         * one token of many, and a page that writes it this way is
         * asking for the same fill as background-color. The first token
         * that parses as a colour is that colour. */
        if (!first_color(val, &c)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->background = c;
        st->set |= CSS_SET_BACKGROUND;
        return 1;
    }
    if (ieq(name, "display")) {
        if (!parse_display(val, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->display = n;
        st->set |= CSS_SET_DISPLAY;
        return 1;
    }
    if (ieq(name, "font-size")) {
        if (!apply_font_size(st, val)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        return 1;
    }
    if (ieq(name, "font-weight")) {
        if (ieq(val, "normal")) n = CSS_WEIGHT_NORMAL;
        else if (ieq(val, "bold")) n = CSS_WEIGHT_BOLD;
        else if (ieq(val, "bolder")) n = CSS_WEIGHT_BOLDER;
        else if (ieq(val, "lighter")) n = CSS_WEIGHT_LIGHTER;
        else if (!parse_int(val, 1, 1000, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->weight = n;
        st->set |= CSS_SET_WEIGHT;
        return 1;
    }
    if (ieq(name, "font-style")) {
        if (ieq(val, "normal")) n = 0;
        else if (ieq(val, "italic") || ieq(val, "oblique")) n = 1;
        else {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->italic = n;
        st->set |= CSS_SET_ITALIC;
        return 1;
    }
    if (ieq(name, "text-decoration") || ieq(name, "text-decoration-line")) {
        const char *p = val;
        char tok[24];
        int d = 0, any = 0;
        while (next_word(&p, tok, sizeof tok)) {
            any = 1;
            if (ieq(tok, "none")) d = 0;
            else if (ieq(tok, "underline")) d |= CSS_TD_UNDERLINE;
            else if (ieq(tok, "line-through")) d |= CSS_TD_LINE_THROUGH;
            else if (ieq(tok, "overline")) d |= CSS_TD_OVERLINE;
            else if (ieq(tok, "blink")) { /* accepted, never drawn */ }
            else {
                if (sh) sh->stats.bad_values++;
                note(sh, line, "bad value for", name);
                return 0;
            }
        }
        if (!any) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->decoration = d;
        st->set |= CSS_SET_DECORATION;
        return 1;
    }
    if (ieq(name, "text-align")) {
        if (ieq(val, "left") || ieq(val, "start")) n = CSS_ALIGN_LEFT;
        else if (ieq(val, "center")) n = CSS_ALIGN_CENTER;
        else if (ieq(val, "right") || ieq(val, "end")) n = CSS_ALIGN_RIGHT;
        else if (ieq(val, "justify")) n = CSS_ALIGN_JUSTIFY;
        else {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->align = n;
        st->set |= CSS_SET_ALIGN;
        return 1;
    }
    if (ieq(name, "margin-top") || ieq(name, "margin-bottom")) {
        int rel = 0;
        if (!parse_len(val, &n, &rel)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        int top = ieq(name, "margin-top");
        st->rel = (st->rel & ~(top ? CSS_RL_MARGIN_TOP : CSS_RL_MARGIN_BOTTOM)) |
                  (rel ? (top ? CSS_RL_MARGIN_TOP : CSS_RL_MARGIN_BOTTOM) : 0);
        if (top) { st->margin_top = n; st->set |= CSS_SET_MARGIN_TOP; }
        else { st->margin_bottom = n; st->set |= CSS_SET_MARGIN_BOTTOM; }
        return 1;
    }
    if (ieq(name, "margin-left") || ieq(name, "margin-right")) {
        int rel = 0;
        if (ieq(val, "auto")) {
            n = 0;                     /* one column: no free space to take */
        } else if (!parse_len(val, &n, &rel)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        int left = ieq(name, "margin-left");
        st->rel = (st->rel & ~(left ? CSS_RL_MARGIN_LEFT : CSS_RL_MARGIN_RIGHT)) |
                  (rel ? (left ? CSS_RL_MARGIN_LEFT : CSS_RL_MARGIN_RIGHT) : 0);
        if (left) { st->margin_left = n; st->set |= CSS_SET_MARGIN_LEFT; }
        else { st->margin_right = n; st->set |= CSS_SET_MARGIN_RIGHT; }
        return 1;
    }
    if (ieq(name, "margin")) {
        if (!apply_box_shorthand(st, val,
                                 &st->margin_top, CSS_SET_MARGIN_TOP, CSS_RL_MARGIN_TOP,
                                 &st->margin_right, CSS_SET_MARGIN_RIGHT, CSS_RL_MARGIN_RIGHT,
                                 &st->margin_bottom, CSS_SET_MARGIN_BOTTOM, CSS_RL_MARGIN_BOTTOM,
                                 &st->margin_left, CSS_SET_MARGIN_LEFT, CSS_RL_MARGIN_LEFT,
                                 1)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        return 1;
    }
    if (ieq(name, "padding-top") || ieq(name, "padding-bottom")) {
        int rel = 0;
        if (!parse_len(val, &n, &rel) || n < 0) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        int top = ieq(name, "padding-top");
        st->rel = (st->rel & ~(top ? CSS_RL_PAD_TOP : CSS_RL_PAD_BOTTOM)) |
                  (rel ? (top ? CSS_RL_PAD_TOP : CSS_RL_PAD_BOTTOM) : 0);
        if (top) { st->pad_top = n; st->set |= CSS_SET_PAD_TOP; }
        else { st->pad_bottom = n; st->set |= CSS_SET_PAD_BOTTOM; }
        return 1;
    }
    if (ieq(name, "padding-left") || ieq(name, "padding-right")) {
        int rel = 0;
        if (!parse_len(val, &n, &rel) || n < 0) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        int left = ieq(name, "padding-left");
        st->rel = (st->rel & ~(left ? CSS_RL_PAD_LEFT : CSS_RL_PAD_RIGHT)) |
                  (rel ? (left ? CSS_RL_PAD_LEFT : CSS_RL_PAD_RIGHT) : 0);
        if (left) { st->pad_left = n; st->set |= CSS_SET_PAD_LEFT; }
        else { st->pad_right = n; st->set |= CSS_SET_PAD_RIGHT; }
        return 1;
    }
    if (ieq(name, "padding")) {
        if (!apply_box_shorthand(st, val,
                                 &st->pad_top, CSS_SET_PAD_TOP, CSS_RL_PAD_TOP,
                                 &st->pad_right, CSS_SET_PAD_RIGHT, CSS_RL_PAD_RIGHT,
                                 &st->pad_bottom, CSS_SET_PAD_BOTTOM, CSS_RL_PAD_BOTTOM,
                                 &st->pad_left, CSS_SET_PAD_LEFT, CSS_RL_PAD_LEFT,
                                 0)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        return 1;
    }
    if (ieq(name, "border")) {
        /* This renderer draws a block's top and bottom rule, so a plain
         * `border: 1px solid x` is those two. */
        if (!apply_border_shorthand(st, val, 1) ||
            !apply_border_shorthand(st, val, 0)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        return 1;
    }
    if (ieq(name, "border-top") || ieq(name, "border-bottom")) {
        if (!apply_border_shorthand(st, val, name[7] == 't')) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        return 1;
    }
    if (ieq(name, "border-top-width") || ieq(name, "border-bottom-width")) {
        if (!parse_border_width(val, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        if (name[7] == 't') { st->border_top_w = n; st->set |= CSS_SET_BORDER_TOP_W; }
        else { st->border_bottom_w = n; st->set |= CSS_SET_BORDER_BOTTOM_W; }
        return 1;
    }
    if (ieq(name, "border-top-style") || ieq(name, "border-bottom-style")) {
        if (!border_style(val, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        if (name[7] == 't') { st->border_top_style = n; st->set |= CSS_SET_BORDER_TOP_S; }
        else { st->border_bottom_style = n; st->set |= CSS_SET_BORDER_BOTTOM_S; }
        return 1;
    }
    if (ieq(name, "border-top-color") || ieq(name, "border-bottom-color")) {
        if (ieq(val, "currentcolor")) return 1;
        if (!css_color(val, strlen(val), &c)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        if (name[7] == 't') { st->border_top_color = c; st->set |= CSS_SET_BORDER_TOP_C; }
        else { st->border_bottom_color = c; st->set |= CSS_SET_BORDER_BOTTOM_C; }
        return 1;
    }
    if (ieq(name, "width") || ieq(name, "max-width")) {
        int px = 0, pct = 0, rel = 0;
        int is_max = (name[0] == 'm');
        if (is_max && ieq(val, "none")) {
            /* none is a real value: no maximum, whatever an ancestor said */
            st->max_width = 0;
            st->max_width_pct = 0;
            st->rel &= ~CSS_RL_MAX_WIDTH;
            st->set |= CSS_SET_MAX_WIDTH;
            return 1;
        }
        if (!is_max && ieq(val, "auto")) {
            st->width = 0;
            st->width_pct = 0;
            st->rel &= ~CSS_RL_WIDTH;
            st->set |= CSS_SET_WIDTH;
            return 1;
        }
        if (parse_percent(val, &pct)) { px = 0; }
        else if (!parse_len(val, &px, &rel) || px < 0) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        if (is_max) {
            st->max_width = px;
            st->max_width_pct = pct;
            st->rel = (st->rel & ~CSS_RL_MAX_WIDTH) | (rel ? CSS_RL_MAX_WIDTH : 0);
            st->set |= CSS_SET_MAX_WIDTH;
        } else {
            st->width = px;
            st->width_pct = pct;
            st->rel = (st->rel & ~CSS_RL_WIDTH) | (rel ? CSS_RL_WIDTH : 0);
            st->set |= CSS_SET_WIDTH;
        }
        return 1;
    }
    if (ieq(name, "line-height")) {
        int px = 0, pct = 0;
        if (ieq(val, "normal")) {
            px = 0;
            pct = 0;
        } else if (parse_percent(val, &pct)) {
            px = 0;
            if (pct == 0) pct = 1;
        } else if (parse_multiplier(val, &pct)) {
            px = 0;
        } else if (parse_length(val, &px)) {
            pct = 0;
            if (px < 1) px = 1;
        } else {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->line_height = px;
        st->line_height_pct = pct;
        st->set |= CSS_SET_LINE_HEIGHT;
        return 1;
    }
    if (ieq(name, "list-style-type")) {
        if (!parse_list_style(val, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->list_style = n;
        st->set |= CSS_SET_LIST_STYLE;
        return 1;
    }
    if (ieq(name, "white-space")) {
        if (!parse_white_space(val, &n)) {
            if (sh) sh->stats.bad_values++;
            note(sh, line, "bad value for", name);
            return 0;
        }
        st->white_space = n;
        st->set |= CSS_SET_WHITE_SPACE;
        return 1;
    }

    if (sh) sh->stats.unknown_props++;
    note(sh, line, "unknown property", name);
    return 0;
}

/* ------------------------------------------------------------
 *  Declarations and blocks.
 *  parse_declaration returns 1 if the declaration took effect, 0 if it
 *  was consumed but rejected, -1 at '}' or EOF (nothing consumed).
 * ------------------------------------------------------------ */
static int parse_declaration(Cur *c, CssStyle *props, CssSheet *sh)
{
    cskip_ws(c);
    int ch = cpeek(c);
    if (ch < 0 || ch == '}') return -1;
    if (ch == ';') { cadv(c); return 0; }
    if (ch == '{') {
        cadv(c);
        skip_balanced(c);
        if (sh) sh->stats.bad_decls++;
        note(sh, c->line, "nested block in a declaration", NULL);
        return 0;
    }

    int line = c->line;
    char name[32];
    int nl = 0, over = 0;
    while (c->i < c->n) {
        ch = cpeek(c);
        if (ch == ':' || ch == ';' || ch == '}' || ch == '{') break;
        if (!is_space(ch)) {
            if (nl < (int)sizeof name - 1) name[nl++] = (char)ch;
            else over = 1;
        }
        cadv(c);
    }
    name[nl] = 0;

    ch = cpeek(c);
    if (ch != ':') {
        if (sh) sh->stats.bad_decls++;
        note(sh, line, "declaration without a colon", name);
        if (ch == ';') cadv(c);
        return 0;
    }
    cadv(c);                                  /* ':' */

    char val[128];
    int vl = scan_value(c, val, (int)sizeof val);
    if (cpeek(c) == ';') cadv(c);

    if (over || nl == 0 || vl <= 0) {
        if (sh) sh->stats.bad_decls++;
        note(sh, line, "malformed declaration", name);
        return 0;
    }
    return decl_apply(props, name, val, line, sh);
}

static void parse_block(Cur *c, CssStyle *props, CssSheet *sh)
{
    for (;;) {
        int r = parse_declaration(c, props, sh);
        if (r < 0) {
            if (cpeek(c) == '}') cadv(c);
            return;
        }
    }
}

/* ------------------------------------------------------------
 *  Rules and the sheet.
 * ------------------------------------------------------------ */
#define CSS_MAX_GROUP 16

static void parse_rule(Cur *c, CssSheet *sh)
{
    size_t pbegin = c->i;
    size_t pend = c->i;
    int pline = c->line;

    for (;;) {
        int ch = cpeek(c);
        if (ch < 0) {
            sh->stats.bad_rules++;
            note(sh, pline, "rule without a block", NULL);
            return;
        }
        if (ch == '{') { pend = c->i; cadv(c); break; }
        if (ch == '}') {
            cadv(c);
            sh->stats.bad_rules++;
            note(sh, pline, "unexpected", "}");
            return;
        }
        if (ch == '"' || ch == '\'') { skip_string(c, ch); continue; }
        if (ch == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') { cskip_ws(c); continue; }
        cadv(c);
    }

    CssSelector group[CSS_MAX_GROUP];
    int ng = 0;

    size_t i = pbegin;
    while (i < pend) {
        size_t j = next_comma(c->s, i, pend);
        Cur sc;
        sc.s = c->s;
        sc.n = j;
        sc.i = i;
        sc.line = pline;
        CssSelector sel;
        cskip_ws(&sc);
        if (parse_selector(&sc, &sel)) {
            cskip_ws(&sc);
            if (sc.i >= sc.n) {
                if (ng < CSS_MAX_GROUP) group[ng++] = sel;
                else { sh->stats.truncated++; note(sh, pline, "selector group too long", NULL); }
            } else {
                sh->stats.bad_selectors++;
                note(sh, pline, "selector outside the subset", NULL);
            }
        } else {
            sh->stats.bad_selectors++;
            note(sh, pline, "selector outside the subset", NULL);
        }
        i = (j < pend) ? j + 1 : pend;
    }
    if (ng == 0) {
        sh->stats.bad_rules++;
        note(sh, pline, "rule with no usable selector", NULL);
    }

    CssStyle props;
    memset(&props, 0, sizeof props);
    parse_block(c, &props, sh);

    for (int k = 0; k < ng; k++) {
        if (sh->nrules >= CSS_MAX_RULES) {
            sh->stats.truncated++;
            note(sh, pline, "rule list full, dropped", NULL);
            break;
        }
        CssRule *r = &sh->rule[sh->nrules++];
        r->sel = group[k];
        r->props = props;
    }
}

void css_sheet_init(CssSheet *sheet)
{
    if (!sheet) return;
    memset(sheet, 0, sizeof *sheet);
}

/* ------------------------------------------------------------
 *  @media
 *
 *  A text browser can answer one media question - what kind of medium
 *  is this - and it answers it from the query's TYPE, never by guessing
 *  at a viewport it does not have. `screen`, `all` and `not print`
 *  match. `print` does not. A query that narrows on a feature
 *  (`(max-width: 600px)`, `(prefers-color-scheme: dark)`) cannot be
 *  evaluated against a column of text, so it does not match and is
 *  counted: a mobile-first sheet's phone rules must not be applied to
 *  the desktop page the reader is looking at.
 * ------------------------------------------------------------ */
static int media_query_matches(const char *q)
{
    if (!q[0]) return 1;                       /* `@media {` = all      */
    for (const char *p = q; *p; p++)
        if (*p == '(') return 0;               /* a feature: not ours   */

    const char *p = q;
    char tok[32];
    int negated = 0, yes = 0, typed = 0;
    while (next_word(&p, tok, sizeof tok)) {
        if (ieq(tok, "not")) { negated = 1; continue; }
        if (ieq(tok, "only") || ieq(tok, "and")) continue;
        if (ieq(tok, "screen") || ieq(tok, "all")) { yes = 1; typed = 1; continue; }
        if (ieq(tok, "print") || ieq(tok, "speech") || ieq(tok, "tv") ||
            ieq(tok, "braille") || ieq(tok, "handheld") ||
            ieq(tok, "projection") || ieq(tok, "tty") ||
            ieq(tok, "embossed")) { typed = 1; continue; }
        return 0;                              /* a type we do not know */
    }
    if (negated) return !yes;
    return yes || !typed;
}

/* A whole comma-separated media query list. */
static int media_matches(const char *list, size_t len)
{
    size_t i = 0;
    while (i <= len) {
        size_t j = i;
        while (j < len && list[j] != ',') j++;
        char q[96];
        size_t a = i, b = j, n = 0;
        while (a < b && is_space((unsigned char)list[a])) a++;
        while (b > a && is_space((unsigned char)list[b - 1])) b--;
        for (size_t k = a; k < b && n + 1 < sizeof q; k++) q[n++] = list[k];
        q[n] = 0;
        if (media_query_matches(q)) return 1;
        if (j >= len) break;
        i = j + 1;
    }
    return 0;
}

int css_media_matches(const char *list)
{
    if (!list || !list[0]) return 1;
    return media_matches(list, strlen(list));
}

static void parse_rule_list(Cur *c, CssSheet *sh, int depth);

/* The '@' has already been consumed. Everything except a screen @media
 * is skipped as a unit, exactly as before. */
static void parse_at_rule(Cur *c, CssSheet *sh, int depth)
{
    char name[20];
    if (!read_name(c, name, (int)sizeof name, 1)) name[0] = 0;

    char pre[96];
    int pl = 0, have_block = 0;
    for (;;) {
        int ch = cpeek(c);
        if (ch < 0) break;
        if (ch == ';') { cadv(c); break; }
        if (ch == '{') { cadv(c); have_block = 1; break; }
        if (ch == '"' || ch == '\'') { skip_string(c, ch); continue; }
        if (ch == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') { cskip_ws(c); continue; }
        if (is_space(ch)) {
            if (pl > 0 && pre[pl - 1] != ' ' && pl < (int)sizeof pre - 1)
                pre[pl++] = ' ';
        } else if (pl < (int)sizeof pre - 1) {
            pre[pl++] = (char)ch;
        }
        cadv(c);
    }
    pre[pl] = 0;
    while (pl > 0 && pre[pl - 1] == ' ') pre[--pl] = 0;

    sh->stats.at_rules++;
    if (!have_block) return;                   /* @import, @charset: done */

    if (ieq(name, "media") && depth < CSS_MEDIA_DEPTH) {
        if (!media_matches(pre, (size_t)pl)) {
            sh->stats.media_skipped++;
            skip_balanced(c);
            return;
        }
        sh->stats.media_applied++;
        parse_rule_list(c, sh, depth + 1);
        return;
    }
    skip_balanced(c);
}

/* Parse rules until the closing '}' of the enclosing block (consumed) or
 * the end of the sheet. `depth` is how many @media blocks enclose this
 * one; at depth 0 a stray '}' is an error, inside a block it is the end. */
static void parse_rule_list(Cur *c, CssSheet *sh, int depth)
{
    for (;;) {
        cskip_ws(c);
        int ch = cpeek(c);
        if (ch < 0) return;
        if (ch == '}') {
            cadv(c);
            if (depth == 0) {
                sh->stats.bad_rules++;
                note(sh, c->line, "unexpected", "}");
                continue;
            }
            return;
        }
        if (ch == '@') {
            cadv(c);
            parse_at_rule(c, sh, depth);
            continue;
        }
        parse_rule(c, sh);
    }
}

int css_parse(const char *text, size_t len, CssSheet *sheet)
{
    if (!text || !sheet) return -1;
    /* A UTF-8 BOM is legal at the top of a fetched stylesheet and would
       otherwise turn the first selector into an unknown name. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }

    int before = sheet->nrules;
    Cur c;
    c.s = text;
    c.n = len;
    c.i = 0;
    c.line = 1;

    parse_rule_list(&c, sheet, 0);
    return sheet->nrules - before;
}

/* ------------------------------------------------------------
 *  The built-in HTML sheet.
 *
 *  Every rule here is real CSS read by the parser above, applied at UA
 *  origin: any author rule, even a bare tag selector, beats all of it.
 *
 *  Sizes are web.c's document sizes (src/client/apps/web.c:1822):
 *  12 px body, 24/20/17/15/13/12 for h1..h6. All six headings share one
 *  vertical margin - 16 px above and 16 px below - because the browser
 *  puts every heading on one grid and lets only the type size tell them
 *  apart. The earlier per-level em factors (0.67, 0.83, 1, 1.33, 1.67,
 *  2.33) made h6 float 12 px further from its neighbour than h1; that
 *  is a typographic decision the renderer does not make, so the built-in
 *  sheet states the one value instead of hiding it in six rules.
 *
 *  Margins are the element's own; collapsing two adjacent margins is a
 *  layout decision and is left to the renderer.
 * ------------------------------------------------------------ */
static const char kDefaultCss[] =
"body { margin-top: 0; margin-bottom: 0; }\n"
/* HTML's own hidden elements. This is real UA CSS, and it matters for
 * real pages: <head> carries <meta>, <link> and <style> children whose
 * text would otherwise be laid out as a paragraph. */
"head, script, style, title, base, meta, link, param { display: none; }\n"
"p { margin-top: 12px; margin-bottom: 12px; }\n"
"h1, h2, h3, h4, h5, h6 { font-weight: bold; margin-top: 16px; margin-bottom: 16px; }\n"
"ul, ol, menu, dir { margin-top: 12px; margin-bottom: 12px; }\n"
"ul { list-style-type: disc; }\n"
"ol { list-style-type: decimal; }\n"
"ul ul { list-style-type: circle; }\n"
"ul ul ul { list-style-type: square; }\n"
"blockquote { margin-top: 12px; margin-bottom: 12px; }\n"
"pre { margin-top: 12px; margin-bottom: 12px; white-space: pre; }\n"
"a { color: #0000EE; text-decoration: underline; }\n"
"u, ins { text-decoration: underline; }\n"
"s, strike, del { text-decoration: line-through; }\n"
"b, strong { font-weight: bold; }\n"
"i, em, cite, var, dfn, address { font-style: italic; }\n"
"th { font-weight: bold; text-align: center; }\n"
"caption { text-align: center; }\n"
"center { text-align: center; }\n"
"nobr { white-space: nowrap; }\n"
"hr { margin-top: 6px; margin-bottom: 6px;"
" border-top: 1px solid #999999; border-bottom: 0; }\n";

/* The three facts the property subset cannot express. Size 0 means
 * "do not change the inherited size", which is what inline elements
 * want: <code> inside <h2> stays 20 px, it only changes face. */
typedef struct {
    const char *tag;
    int size;
    int indent;
    int mono;
} CssTagDefault;

static const CssTagDefault kTagDefaults[] = {
    /* `body` is deliberately absent: its size is the initial one, so it
     * inherits whatever <html> resolved to (html { font-size: 16px } on a
     * real page must reach the text, not be overwritten by a 12 px UA
     * fact). css_style_init() already starts at CSS_BASE_FONT_SIZE. */
    { "h1",         24,                 0,  0 },
    { "h2",         20,                 0,  0 },
    { "h3",         17,                 0,  0 },
    { "h4",         15,                 0,  0 },
    { "h5",         13,                 0,  0 },
    { "h6",         12,                 0,  0 },
    { "pre",        0,                  0,  1 },
    { "code",       0,                  0,  1 },
    { "kbd",        0,                  0,  1 },
    { "samp",       0,                  0,  1 },
    { "tt",         0,                  0,  1 },
    { "ul",         0,                  24, 0 },
    { "ol",         0,                  24, 0 },
    { "menu",       0,                  24, 0 },
    { "dir",        0,                  24, 0 },
    { "blockquote", 0,                  24, 0 },
    { "dd",         0,                  24, 0 }
};

static const CssTagDefault *tag_default(const char *tag)
{
    if (!tag) return NULL;
    for (size_t i = 0; i < sizeof kTagDefaults / sizeof kTagDefaults[0]; i++) {
        const char *a = kTagDefaults[i].tag;
        const char *b = tag;
        while (*a && *b && *a == ascii_lower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return &kTagDefaults[i];
    }
    return NULL;
}

static CssSheet g_defaults;
static int g_defaults_ready;

const CssSheet *css_defaults(void)
{
    if (!g_defaults_ready) {
        css_sheet_init(&g_defaults);
        css_parse(kDefaultCss, sizeof kDefaultCss - 1, &g_defaults);
        g_defaults_ready = 1;
    }
    return &g_defaults;
}

/* ------------------------------------------------------------
 *  Matching and the cascade.
 * ------------------------------------------------------------ */
static int class_has(const char *classes, const char *want)
{
    if (!classes || !*classes) return 0;
    size_t wl = strlen(want);
    const char *p = classes;
    while (*p) {
        while (*p && is_space((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        if ((size_t)(p - s) == wl && !memcmp(s, want, wl)) return 1;
    }
    return 0;
}

static int tag_is(const char *sel_tag, const char *tag)
{
    while (*sel_tag && *tag) {
        if (*sel_tag != ascii_lower((unsigned char)*tag)) return 0;
        sel_tag++;
        tag++;
    }
    return *sel_tag == 0 && *tag == 0;
}

static int compound_match(const CssCompound *cp, const char *tag,
                          const char *classes, const char *id)
{
    if (cp->id[0]) {
        if (!id || strcmp(cp->id, id)) return 0;
    }
    if (cp->cls[0]) {
        if (!class_has(classes, cp->cls)) return 0;
    }
    if (cp->tag[0]) {
        if (!tag || !tag_is(cp->tag, tag)) return 0;
    }
    return 1;
}

static int sel_match(const CssSelector *sel, const char *tag,
                     const char *classes, const char *id, const CssNode *up)
{
    if (!compound_match(&sel->part[sel->nparts - 1], tag, classes, id)) return 0;
    const CssNode *n = up;
    for (int i = sel->nparts - 2; i >= 0; i--) {
        int found = 0;
        while (n) {
            if (compound_match(&sel->part[i], n->tag, n->classes, n->id)) {
                n = n->up;
                found = 1;
                break;
            }
            n = n->up;
        }
        if (!found) return 0;
    }
    return 1;
}

/* Property slots, one per cascaded field. Font size, monospace and
 * indent are not here: only the built-in tag table sets them, so there
 * is nothing to cascade. */
enum {
    P_COLOR, P_BG, P_WEIGHT, P_ITALIC, P_DECOR, P_ALIGN,
    P_MT, P_MB, P_ML, P_MR, P_PT, P_PB, P_PL, P_PR,
    P_BTW, P_BTS, P_BTC, P_BBW, P_BBS, P_BBC,
    P_LIST, P_WS, P_WIDTH, P_MAXW, P_LH, P_DISPLAY, P_FS, P_COUNT
};

#define CSS_SPEC_UA   (-1000000)
#define CSS_SPEC_ATTR 0

static void cascade(CssStyle *dst, const CssStyle *src, int spec, int *best)
{
#define TAKE(bit, idx) ((src->set & (bit)) && spec >= best[idx])
    if (TAKE(CSS_SET_COLOR, P_COLOR)) {
        dst->color = src->color; best[P_COLOR] = spec; dst->set |= CSS_SET_COLOR;
    }
    if (TAKE(CSS_SET_BACKGROUND, P_BG)) {
        dst->background = src->background; best[P_BG] = spec; dst->set |= CSS_SET_BACKGROUND;
    }
    if (src->set & CSS_SET_WEIGHT) {
        if (spec >= best[P_WEIGHT]) {
            int w = src->weight;
            if (w == CSS_WEIGHT_BOLDER) w = dst->weight < 600 ? 700 : 900;
            else if (w == CSS_WEIGHT_LIGHTER) w = dst->weight >= 600 ? 400 : 100;
            dst->weight = w;
            best[P_WEIGHT] = spec;
            dst->set |= CSS_SET_WEIGHT;
        }
    }
    if (TAKE(CSS_SET_ITALIC, P_ITALIC)) {
        dst->italic = src->italic; best[P_ITALIC] = spec; dst->set |= CSS_SET_ITALIC;
    }
    if (TAKE(CSS_SET_DECORATION, P_DECOR)) {
        dst->decoration = src->decoration; best[P_DECOR] = spec; dst->set |= CSS_SET_DECORATION;
    }
    if (TAKE(CSS_SET_ALIGN, P_ALIGN)) {
        dst->align = src->align; best[P_ALIGN] = spec; dst->set |= CSS_SET_ALIGN;
    }
    /* A relative length keeps its CSS_RL_* bit with the value it belongs
     * to: taking a declaration must take its unit as well, or an em value
     * from a lower-priority rule would be read as px. */
    if (TAKE(CSS_SET_MARGIN_TOP, P_MT)) {
        dst->margin_top = src->margin_top; best[P_MT] = spec; dst->set |= CSS_SET_MARGIN_TOP;
        dst->rel = (dst->rel & ~CSS_RL_MARGIN_TOP) | (src->rel & CSS_RL_MARGIN_TOP);
    }
    if (TAKE(CSS_SET_MARGIN_BOTTOM, P_MB)) {
        dst->margin_bottom = src->margin_bottom; best[P_MB] = spec; dst->set |= CSS_SET_MARGIN_BOTTOM;
        dst->rel = (dst->rel & ~CSS_RL_MARGIN_BOTTOM) | (src->rel & CSS_RL_MARGIN_BOTTOM);
    }
    if (TAKE(CSS_SET_MARGIN_LEFT, P_ML)) {
        dst->margin_left = src->margin_left; best[P_ML] = spec; dst->set |= CSS_SET_MARGIN_LEFT;
        dst->rel = (dst->rel & ~CSS_RL_MARGIN_LEFT) | (src->rel & CSS_RL_MARGIN_LEFT);
    }
    if (TAKE(CSS_SET_MARGIN_RIGHT, P_MR)) {
        dst->margin_right = src->margin_right; best[P_MR] = spec; dst->set |= CSS_SET_MARGIN_RIGHT;
        dst->rel = (dst->rel & ~CSS_RL_MARGIN_RIGHT) | (src->rel & CSS_RL_MARGIN_RIGHT);
    }
    if (TAKE(CSS_SET_PAD_TOP, P_PT)) {
        dst->pad_top = src->pad_top; best[P_PT] = spec; dst->set |= CSS_SET_PAD_TOP;
        dst->rel = (dst->rel & ~CSS_RL_PAD_TOP) | (src->rel & CSS_RL_PAD_TOP);
    }
    if (TAKE(CSS_SET_PAD_BOTTOM, P_PB)) {
        dst->pad_bottom = src->pad_bottom; best[P_PB] = spec; dst->set |= CSS_SET_PAD_BOTTOM;
        dst->rel = (dst->rel & ~CSS_RL_PAD_BOTTOM) | (src->rel & CSS_RL_PAD_BOTTOM);
    }
    if (TAKE(CSS_SET_PAD_LEFT, P_PL)) {
        dst->pad_left = src->pad_left; best[P_PL] = spec; dst->set |= CSS_SET_PAD_LEFT;
        dst->rel = (dst->rel & ~CSS_RL_PAD_LEFT) | (src->rel & CSS_RL_PAD_LEFT);
    }
    if (TAKE(CSS_SET_PAD_RIGHT, P_PR)) {
        dst->pad_right = src->pad_right; best[P_PR] = spec; dst->set |= CSS_SET_PAD_RIGHT;
        dst->rel = (dst->rel & ~CSS_RL_PAD_RIGHT) | (src->rel & CSS_RL_PAD_RIGHT);
    }

    if (TAKE(CSS_SET_DISPLAY, P_DISPLAY)) {
        dst->display = src->display; best[P_DISPLAY] = spec; dst->set |= CSS_SET_DISPLAY;
    }
    /* font-size is one property with three sources; the winner clears the
     * other two so css_for() resolves the one that was written last. */
    if ((src->set & (CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM)) &&
        spec >= best[P_FS]) {
        dst->set &= ~(CSS_SET_FONT_SIZE | CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM);
        if (src->set & CSS_SET_FONT_SIZE) {
            dst->font_size = src->font_size;
            dst->set |= CSS_SET_FONT_SIZE;
        } else {
            dst->font_size_pct = src->font_size_pct;
            dst->set |= (src->set & CSS_SET_FONT_SIZE_REM) ? CSS_SET_FONT_SIZE_REM
                                                          : CSS_SET_FONT_SIZE_REL;
        }
        best[P_FS] = spec;
    }
    if (TAKE(CSS_SET_BORDER_TOP_W, P_BTW)) {
        dst->border_top_w = src->border_top_w; best[P_BTW] = spec; dst->set |= CSS_SET_BORDER_TOP_W;
    }
    if (TAKE(CSS_SET_BORDER_TOP_S, P_BTS)) {
        dst->border_top_style = src->border_top_style; best[P_BTS] = spec; dst->set |= CSS_SET_BORDER_TOP_S;
    }
    if (TAKE(CSS_SET_BORDER_TOP_C, P_BTC)) {
        dst->border_top_color = src->border_top_color; best[P_BTC] = spec; dst->set |= CSS_SET_BORDER_TOP_C;
    }
    if (TAKE(CSS_SET_BORDER_BOTTOM_W, P_BBW)) {
        dst->border_bottom_w = src->border_bottom_w; best[P_BBW] = spec; dst->set |= CSS_SET_BORDER_BOTTOM_W;
    }
    if (TAKE(CSS_SET_BORDER_BOTTOM_S, P_BBS)) {
        dst->border_bottom_style = src->border_bottom_style; best[P_BBS] = spec; dst->set |= CSS_SET_BORDER_BOTTOM_S;
    }
    if (TAKE(CSS_SET_BORDER_BOTTOM_C, P_BBC)) {
        dst->border_bottom_color = src->border_bottom_color; best[P_BBC] = spec; dst->set |= CSS_SET_BORDER_BOTTOM_C;
    }
    if (TAKE(CSS_SET_LIST_STYLE, P_LIST)) {
        dst->list_style = src->list_style; best[P_LIST] = spec; dst->set |= CSS_SET_LIST_STYLE;
    }
    if (TAKE(CSS_SET_WHITE_SPACE, P_WS)) {
        dst->white_space = src->white_space; best[P_WS] = spec; dst->set |= CSS_SET_WHITE_SPACE;
    }
    if (TAKE(CSS_SET_WIDTH, P_WIDTH)) {
        dst->width = src->width; dst->width_pct = src->width_pct;
        best[P_WIDTH] = spec; dst->set |= CSS_SET_WIDTH;
        dst->rel = (dst->rel & ~CSS_RL_WIDTH) | (src->rel & CSS_RL_WIDTH);
    }
    if (TAKE(CSS_SET_MAX_WIDTH, P_MAXW)) {
        dst->max_width = src->max_width; dst->max_width_pct = src->max_width_pct;
        best[P_MAXW] = spec; dst->set |= CSS_SET_MAX_WIDTH;
        dst->rel = (dst->rel & ~CSS_RL_MAX_WIDTH) | (src->rel & CSS_RL_MAX_WIDTH);
    }
    if (TAKE(CSS_SET_LINE_HEIGHT, P_LH)) {
        dst->line_height = src->line_height; dst->line_height_pct = src->line_height_pct;
        best[P_LH] = spec; dst->set |= CSS_SET_LINE_HEIGHT;
    }
#undef TAKE
}

static void cascade_sheet(CssStyle *out, const CssSheet *sh, const char *tag,
                          const char *classes, const char *id,
                          const CssNode *st, int base, int *best)
{
    if (!sh) return;
    const CssNode *up = st ? st->up : NULL;
    for (int i = 0; i < sh->nrules; i++) {
        const CssRule *r = &sh->rule[i];
        if (!sel_match(&r->sel, tag, classes, id, up)) continue;
        cascade(out, &r->props, base + r->sel.specificity, best);
    }
}

static void apply_attrs(CssStyle *out, unsigned attrs, int *best)
{
    if (attrs & CSS_ATTR_BOLD) {
        if (CSS_SPEC_ATTR >= best[P_WEIGHT]) {
            out->weight = CSS_WEIGHT_BOLD;
            out->set |= CSS_SET_WEIGHT;
            best[P_WEIGHT] = CSS_SPEC_ATTR;
        }
    }
    if (attrs & CSS_ATTR_ITALIC) {
        if (CSS_SPEC_ATTR >= best[P_ITALIC]) {
            out->italic = 1;
            out->set |= CSS_SET_ITALIC;
            best[P_ITALIC] = CSS_SPEC_ATTR;
        }
    }
    if (attrs & CSS_ATTR_MONO) {
        out->monospace = 1;
        out->set |= CSS_SET_MONO;
    }
    if (attrs & CSS_ATTR_PRE) {
        if (CSS_SPEC_ATTR >= best[P_WS]) {
            out->white_space = CSS_WS_PRE;
            out->set |= CSS_SET_WHITE_SPACE;
            best[P_WS] = CSS_SPEC_ATTR;
        }
        out->monospace = 1;
        out->set |= CSS_SET_MONO;
    }
    if (attrs & CSS_ATTR_NOWRAP) {
        if (CSS_SPEC_ATTR >= best[P_WS]) {
            out->white_space = CSS_WS_NOWRAP;
            out->set |= CSS_SET_WHITE_SPACE;
            best[P_WS] = CSS_SPEC_ATTR;
        }
    }
    if (attrs & CSS_ATTR_UNDERLINE) {
        if (CSS_SPEC_ATTR >= best[P_DECOR]) {
            out->decoration |= CSS_TD_UNDERLINE;
            out->set |= CSS_SET_DECORATION;
            best[P_DECOR] = CSS_SPEC_ATTR;
        }
    }
    if (attrs & CSS_ATTR_STRIKE) {
        if (CSS_SPEC_ATTR >= best[P_DECOR]) {
            out->decoration |= CSS_TD_LINE_THROUGH;
            out->set |= CSS_SET_DECORATION;
            best[P_DECOR] = CSS_SPEC_ATTR;
        }
    }
    /* One bit can be set by more than one alignment hint; the order is
       fixed so the answer never depends on the caller's bit order. */
    int align = -1;
    if (attrs & CSS_ATTR_JUSTIFY) align = CSS_ALIGN_JUSTIFY;
    if (attrs & CSS_ATTR_CENTER) align = CSS_ALIGN_CENTER;
    if (attrs & CSS_ATTR_RIGHT) align = CSS_ALIGN_RIGHT;
    if (align >= 0 && CSS_SPEC_ATTR >= best[P_ALIGN]) {
        out->align = align;
        out->set |= CSS_SET_ALIGN;
        best[P_ALIGN] = CSS_SPEC_ATTR;
    }
}

void css_style_init(CssStyle *style)
{
    if (!style) return;
    memset(style, 0, sizeof *style);
    style->color = CSS_INITIAL_COLOR;
    style->font_size = CSS_BASE_FONT_SIZE;
    style->weight = CSS_WEIGHT_NORMAL;
    style->align = CSS_ALIGN_LEFT;
    style->list_style = CSS_LIST_NONE;
    style->white_space = CSS_WS_NORMAL;
    style->border_top_style = CSS_BORDER_NONE;
    style->border_bottom_style = CSS_BORDER_NONE;
}

CssStyle css_for(const CssSheet *sheet, const char *tag, const char *class_attr,
                 const char *id_attr, const CssNode *st)
{
    CssStyle out;
    css_style_init(&out);

    /* 1. Inheritance. Only the text properties travel; margins,
       padding, borders and background are the element's own. */
    const CssStyle *parent = (st && st->up) ? &st->up->style : NULL;
    if (parent) {
        out.color = parent->color;
        out.font_size = parent->font_size;
        out.weight = parent->weight;
        out.italic = parent->italic;
        out.decoration = parent->decoration;
        out.align = parent->align;
        out.list_style = parent->list_style;
        out.white_space = parent->white_space;
        out.monospace = parent->monospace;
        out.indent = parent->indent;
        /* line-height inherits: a px value as px, a multiplier as a
         * multiplier, so a heading under a 1.5 body gets 1.5 of its own
         * larger size rather than the body's absolute number. */
        out.line_height = parent->line_height;
        out.line_height_pct = parent->line_height_pct;
    }

    /* 2. The parts of the document defaults that are presentation
       facts rather than CSS properties, then the built-in sheet. */
    const CssTagDefault *td = tag_default(tag);
    if (td) {
        if (td->size) {
            out.font_size = td->size;
            out.set |= CSS_SET_FONT_SIZE;
        }
        if (td->indent) {
            out.indent += td->indent;
            out.set |= CSS_SET_INDENT;
        }
        if (td->mono) {
            out.monospace = 1;
            out.set |= CSS_SET_MONO;
        }
    }

    int best[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) best[i] = -2000000000;

    cascade_sheet(&out, css_defaults(), tag, class_attr, id_attr, st, CSS_SPEC_UA, best);

    /* 3. Presentational hints sit between the UA sheet and the author. */
    apply_attrs(&out, st ? st->attrs : 0, best);

    /* 4. The author's sheet, by specificity, later wins on a tie. */
    cascade_sheet(&out, sheet, tag, class_attr, id_attr, st, 1, best);

    /* Finalise. A border colour that was never written is currentColor,
       and a border that has no width is not a border at all. */
    if (!(out.set & CSS_SET_BORDER_TOP_C)) out.border_top_color = out.color;
    if (!(out.set & CSS_SET_BORDER_BOTTOM_C)) out.border_bottom_color = out.color;
    if (out.pad_top < 0) out.pad_top = 0;
    if (out.pad_bottom < 0) out.pad_bottom = 0;
    if (out.pad_left < 0) out.pad_left = 0;
    if (out.pad_right < 0) out.pad_right = 0;
    if (out.border_top_w < 0) out.border_top_w = 0;
    if (out.border_bottom_w < 0) out.border_bottom_w = 0;
    if (out.font_size <= 0) out.font_size = CSS_BASE_FONT_SIZE;

    /* em / % are relative to the parent's resolved size, rem to the root
     * element's. This is the one place that can see both. */
    if (out.set & (CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM)) {
        int base = CSS_BASE_FONT_SIZE;
        if (out.set & CSS_SET_FONT_SIZE_REL) {
            if (parent && parent->font_size > 0) base = parent->font_size;
        } else {
            const CssNode *r = st;
            while (r && r->up) r = r->up;
            if (r && r->style.font_size > 0) base = r->style.font_size;
        }
        out.font_size = (base * out.font_size_pct + 50) / 100;
        out.set &= ~(CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM);
        out.set |= CSS_SET_FONT_SIZE;
    }
    if (out.font_size < 4) out.font_size = 4;
    if (out.font_size > 400) out.font_size = 400;

    /* em lengths: multiplied by this element's own resolved font size,
     * which is exactly what CSS says `margin: 1em` means. Done here, after
     * the font size is final, so a rule that sets both is not read in the
     * wrong order. */
    if (out.rel) {
        int fs = out.font_size;
#define RESOLVE_EM(field, bit)                                                          if (out.rel & (bit)) {                                                              int v = out.field;                                                              out.field = (v * fs + (v < 0 ? -50 : 50)) / 100;                            }
        RESOLVE_EM(margin_top,    CSS_RL_MARGIN_TOP)
        RESOLVE_EM(margin_right,  CSS_RL_MARGIN_RIGHT)
        RESOLVE_EM(margin_bottom, CSS_RL_MARGIN_BOTTOM)
        RESOLVE_EM(margin_left,   CSS_RL_MARGIN_LEFT)
        RESOLVE_EM(pad_top,       CSS_RL_PAD_TOP)
        RESOLVE_EM(pad_right,     CSS_RL_PAD_RIGHT)
        RESOLVE_EM(pad_bottom,    CSS_RL_PAD_BOTTOM)
        RESOLVE_EM(pad_left,      CSS_RL_PAD_LEFT)
        RESOLVE_EM(width,         CSS_RL_WIDTH)
        RESOLVE_EM(max_width,     CSS_RL_MAX_WIDTH)
#undef RESOLVE_EM
        out.rel = 0;
    }

    if (out.weight < 100) out.weight = 100;
    if (out.weight > 900) out.weight = 900;
    if (out.line_height_pct > 0)
        out.line_height = (out.font_size * out.line_height_pct + 50) / 100;
    if (out.line_height < 0) out.line_height = 0;

    return out;
}

void css_node_init(CssNode *n, const char *tag, const char *class_attr,
                   const char *id_attr, unsigned attrs,
                   const CssNode *parent, const CssSheet *sheet)
{
    if (!n) return;
    n->tag = tag;
    n->classes = class_attr;
    n->id = id_attr;
    n->attrs = attrs;
    n->up = parent;
    n->style = css_for(sheet, tag, class_attr, id_attr, n);
}

/* ------------------------------------------------------------
 *  Inline style="..." attributes.
 * ------------------------------------------------------------ */
int css_inline(CssStyle *style, const char *decls, size_t len)
{
    if (!style) return -1;
    if (!decls || len == 0) return 0;
    Cur c;
    c.s = decls;
    c.n = len;
    c.i = 0;
    c.line = 1;
    int applied = 0;
    for (;;) {
        int r = parse_declaration(&c, style, NULL);
        if (r < 0) break;
        if (r > 0) applied++;
    }
    if (style->line_height_pct > 0)
        style->line_height = (style->font_size * style->line_height_pct + 50) / 100;
    return applied;
}

void css_resolve_relative(CssStyle *style, int parent_font_size,
                          int root_font_size)
{
    if (!style) return;
    if (style->set & (CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM)) {
        int base = (style->set & CSS_SET_FONT_SIZE_REM) ? root_font_size
                                                        : parent_font_size;
        if (base <= 0) base = CSS_BASE_FONT_SIZE;
        style->font_size = (base * style->font_size_pct + 50) / 100;
        style->set |= CSS_SET_FONT_SIZE;
        style->set &= ~(CSS_SET_FONT_SIZE_REL | CSS_SET_FONT_SIZE_REM);
    }
    if (style->font_size < 4) style->font_size = 4;
    if (style->font_size > 400) style->font_size = 400;
    if (style->line_height_pct > 0)
        style->line_height = (style->font_size * style->line_height_pct + 50) / 100;
    if (style->line_height < 0) style->line_height = 0;
}

int css_inline_for(CssStyle *style, const char *decls, size_t len,
                   const CssStyle *parent)
{
    if (!style) return -1;
    int n = css_inline(style, decls, len);
    if (n < 0) return n;
    int root = CSS_BASE_FONT_SIZE;
    if (parent && (parent->set & CSS_SET_FONT_SIZE) && parent->font_size > 0)
        root = parent->font_size;
    css_resolve_relative(style, parent && parent->font_size > 0 ? parent->font_size
                                                               : CSS_BASE_FONT_SIZE,
                         root);
    return n;
}
