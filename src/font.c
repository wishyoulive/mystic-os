/* ============================================================
 *  Mystic OS - font.c
 *
 *  Real TrueType/OpenType text: glyphs are rasterised from the
 *  font file itself with stb_truetype and blended by hand, so
 *  text is antialiased and crisp at any size.
 *
 *  Fonts are DISCOVERED, not hard-coded. At startup the loader
 *  scans for *.ttf / *.otf, in this order:
 *
 *      /root/fonts          the user's own fonts (highest priority)
 *      <font_init() dir>    normally /usr/share/fonts
 *      /usr/share/fonts     the faces the image ships
 *
 *  and builds a face list from the files that actually parse.
 *  The four drawing roles are then mapped by weight onto that
 *  list: the UI family supplies regular/medium/semibold and a
 *  monospaced family supplies the terminal face.
 *
 *  /etc/mystic/fonts.conf can name the family for the two roles:
 *
 *      ui   = Lato
 *      mono = JetBrains Mono
 *
 *  See docs/FONTS.md for the full syntax, the runtime calls a
 *  settings screen needs to switch a family without a restart,
 *  and the list of checks a font file has to pass before it is
 *  allowed near stb_truetype.
 * ============================================================ */
#include "mystic.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GLYPHS 2048
#define MAX_FACES  128
#define MAX_DIR_FILES 256
#define FONT_NAME_LEN 96
#define FONT_PATH_LEN 512
#define FONT_FILE_MAX (64L * 1024 * 1024)

#define FONT_CONF       "/etc/mystic/fonts.conf"
#define USER_FONT_DIR   "/root/fonts"
#define SYSTEM_FONT_DIR "/usr/share/fonts"

/* 40 bytes exactly: the pointer first so the ints after it pack without
 * padding. 2048 of these are walked on every cache probe, so the layout is
 * worth this much. */
typedef struct {
    u8   *bitmap;
    int   cp;
    int   w, h;
    int   xoff, yoff;
    float advance;
    int   used;
    int   src;           /* faces[] index that drew it; -1 = no face covers it */
    unsigned last_use;   /* Font.use_clock value of the last hit: the LRU key */
} Glyph;

/* One real font file. The bytes stay alive for the life of the process and
 * every Font of every size points into them: eight sizes of Lato keep one
 * copy of Lato, not eight. `info` is parsed once at scan time. */
typedef struct {
    char  family[FONT_NAME_LEN];   /* family name from the font's name table */
    char  path[FONT_PATH_LEN];
    u8   *data;
    long  len;
    stbtt_fontinfo info;
    int   weight;                  /* OS/2 usWeightClass, 100..900 */
    int   italic;
    int   mono;
} Face;

struct Font {
    int           face;      /* role: FONT_REGULAR .. FONT_MONO */
    int           source;    /* index into faces[] this role points at now */
    float         size;
    stbtt_fontinfo info;
    const u8      *data;     /* owned by faces[source], not by this Font */
    float          scale;
    float          ascent, descent;
    Glyph          cache[MAX_GLYPHS];
    unsigned       use_clock;   /* monotonic per face: bumped on every access */
    struct Font   *next;
};

static Face  faces[MAX_FACES];
static int   face_count;
static int   role_face[FONT_COUNT];           /* -1 when nothing is installed */
static char  role_family[FONT_COUNT][FONT_NAME_LEN];
static char  conf_ui[FONT_NAME_LEN];          /* "ui = ..." from fonts.conf */
static char  conf_mono[FONT_NAME_LEN];
static Font *fonts;
static char  font_dir[FONT_PATH_LEN];
static int   font_ready;

/* The filenames the image ships. They are only a tie-break for the default
 * choice when the user has not named a family: any .ttf or .otf file found
 * in the scan directories is a candidate for every role. */
static const char *DEFAULT_UI_FILES[] = {
    "Lato-Regular.ttf", "Lato-Book.ttf", "DejaVuSans.ttf", NULL
};
static const char *DEFAULT_MONO_FILES[] = {
    "JetBrainsMono-Regular.ttf", "DejaVuSansMono.ttf", "LiberationMono-Regular.ttf", NULL
};

/* ------------------------------------------------------------
 *  Small helpers
 * ------------------------------------------------------------ */
static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static char *trim_ws(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static unsigned be16(const u8 *p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static int be16s(const u8 *p) {
    return (int)(short)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int has_font_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *ext = name + n - 4;
    return ci_equal(ext, ".ttf") || ci_equal(ext, ".otf");
}

/* ------------------------------------------------------------
 *  Unicode: cell width, a strict UTF-8 walk, and what a face covers
 *
 *  How many terminal cells a codepoint occupies is a property of the
 *  CHARACTER, not of the face that happens to draw it, so the tables
 *  live here beside the other character data instead of inside the
 *  terminal. src/term.c asks font_cell_width() how far the cursor
 *  moves; src/apps/files.c and src/client/apps/viewer.c ask it how
 *  many columns a line of text really takes.
 *
 *  WIDE   the East Asian Wide and Fullwidth ranges (Unicode
 *         EastAsianWidth property W and F) and the emoji blocks that
 *         terminals draw two cells wide.
 *  ZERO   the combining marks (general categories Mn and Me), the
 *         zero-width space and joiner controls, and the Hangul Jamo
 *         medial/final ranges, which sit on the previous cell.
 *
 *  Both tables are the ones in Markus Kuhn's public-domain wcwidth()
 *  - which encode the Unicode data - narrowed to the ranges named
 *  above; a codepoint in neither table is one cell wide. Nothing here
 *  is guessed from a glyph's advance: a CJK ideograph is two cells
 *  even in a face that draws it narrower, because that is what the
 *  column grid in a terminal means.
 * ------------------------------------------------------------ */
/* @utf8-only-begin (pure Unicode/UTF-8 code; the Files render harness compiles
 * exactly this text, with no font files and no face list involved) */
typedef struct { u32 lo, hi; } CpRange;

static const CpRange CP_WIDE[] = {
    { 0x1100, 0x115F },   /* Hangul Jamo initial consonants  */
    { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC },
    { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE },
    { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F },
    { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB },
    { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE },
    { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 },
    { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
    { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 },
    { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 },
    { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 },
    { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 },
    { 0x2B55, 0x2B55 },
    { 0x2E80, 0x2E99 }, { 0x2E9B, 0x2EF3 }, { 0x2F00, 0x2FD5 },
    { 0x2FF0, 0x2FFB }, { 0x3000, 0x303E }, { 0x3041, 0x3096 },
    { 0x3099, 0x30FF }, { 0x3105, 0x312D }, { 0x3131, 0x318E },
    { 0x3190, 0x31BA }, { 0x31C0, 0x31E3 }, { 0x31F0, 0x321E },
    { 0x3220, 0x3247 }, { 0x3250, 0x32FE }, { 0x3300, 0x4DBF },
    { 0x4E00, 0xA48C }, { 0xA490, 0xA4C6 }, { 0xA960, 0xA97C },
    { 0xAC00, 0xD7A3 },   /* Hangul syllables               */
    { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 },
    { 0xFE54, 0xFE66 }, { 0xFE68, 0xFE6B }, { 0xFF01, 0xFF60 },
    { 0xFFE0, 0xFFE6 }, { 0x16FE0, 0x16FE0 }, { 0x17000, 0x187EC },
    { 0x18800, 0x18AF2 }, { 0x1B000, 0x1B001 },
    { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E },
    { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F202 }, { 0x1F210, 0x1F23B },
    { 0x1F240, 0x1F248 }, { 0x1F250, 0x1F251 }, { 0x1F300, 0x1F320 },
    { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 },
    { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 },
    { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 },
    { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E },
    { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 },
    { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 },
    { 0x1F6CC, 0x1F6CC }, { 0x1F6D0, 0x1F6D2 }, { 0x1F6EB, 0x1F6EC },
    { 0x1F6F4, 0x1F6F6 }, { 0x1F910, 0x1F91E }, { 0x1F920, 0x1F927 },
    { 0x1F930, 0x1F930 }, { 0x1F933, 0x1F93E }, { 0x1F940, 0x1F94B },
    { 0x1F950, 0x1F95E }, { 0x1F980, 0x1F991 }, { 0x1F9C0, 0x1F9C0 },
    { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD }
};

static const CpRange CP_ZERO[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
    { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 },
    { 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F },
    { 0x0670, 0x0670 }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 },
    { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0711, 0x0711 },
    { 0x0730, 0x074A }, { 0x07A6, 0x07B0 }, { 0x07EB, 0x07F3 },
    { 0x0816, 0x0819 }, { 0x081B, 0x0823 }, { 0x0825, 0x0827 },
    { 0x0829, 0x082D }, { 0x0859, 0x085B }, { 0x08E3, 0x0902 },
    { 0x093A, 0x093A }, { 0x093C, 0x093C }, { 0x0941, 0x0948 },
    { 0x094D, 0x094D }, { 0x0951, 0x0957 }, { 0x0962, 0x0963 },
    { 0x0981, 0x0981 }, { 0x09BC, 0x09BC }, { 0x09C1, 0x09C4 },
    { 0x09CD, 0x09CD }, { 0x09E2, 0x09E3 }, { 0x0A01, 0x0A02 },
    { 0x0A3C, 0x0A3C }, { 0x0A41, 0x0A42 }, { 0x0A47, 0x0A48 },
    { 0x0A4B, 0x0A4D }, { 0x0A51, 0x0A51 }, { 0x0A70, 0x0A71 },
    { 0x0A75, 0x0A75 }, { 0x0A81, 0x0A82 }, { 0x0ABC, 0x0ABC },
    { 0x0AC1, 0x0AC5 }, { 0x0AC7, 0x0AC8 }, { 0x0ACD, 0x0ACD },
    { 0x0AE2, 0x0AE3 }, { 0x0B01, 0x0B01 }, { 0x0B3C, 0x0B3C },
    { 0x0B3F, 0x0B3F }, { 0x0B41, 0x0B44 }, { 0x0B4D, 0x0B4D },
    { 0x0B56, 0x0B56 }, { 0x0B62, 0x0B63 }, { 0x0B82, 0x0B82 },
    { 0x0BC0, 0x0BC0 }, { 0x0BCD, 0x0BCD }, { 0x0C00, 0x0C00 },
    { 0x0C3E, 0x0C40 }, { 0x0C46, 0x0C48 }, { 0x0C4A, 0x0C4D },
    { 0x0C55, 0x0C56 }, { 0x0C62, 0x0C63 }, { 0x0C81, 0x0C81 },
    { 0x0CBC, 0x0CBC }, { 0x0CBF, 0x0CBF }, { 0x0CC6, 0x0CC6 },
    { 0x0CCC, 0x0CCD }, { 0x0CE2, 0x0CE3 }, { 0x0D01, 0x0D01 },
    { 0x0D41, 0x0D44 }, { 0x0D4D, 0x0D4D }, { 0x0D62, 0x0D63 },
    { 0x0DCA, 0x0DCA }, { 0x0DD2, 0x0DD4 }, { 0x0DD6, 0x0DD6 },
    { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
    { 0x0EB1, 0x0EB1 }, { 0x0EB4, 0x0EB9 }, { 0x0EBB, 0x0EBC },
    { 0x0EC8, 0x0ECD }, { 0x0F18, 0x0F19 }, { 0x0F35, 0x0F35 },
    { 0x0F37, 0x0F37 }, { 0x0F39, 0x0F39 }, { 0x0F71, 0x0F7E },
    { 0x0F80, 0x0F84 }, { 0x0F86, 0x0F87 }, { 0x0F8D, 0x0F97 },
    { 0x0F99, 0x0FBC }, { 0x0FC6, 0x0FC6 }, { 0x102D, 0x1030 },
    { 0x1032, 0x1037 }, { 0x1039, 0x103A }, { 0x103D, 0x103E },
    { 0x1058, 0x1059 }, { 0x105E, 0x1060 }, { 0x1071, 0x1074 },
    { 0x1082, 0x1082 }, { 0x1085, 0x1086 }, { 0x108D, 0x108D },
    { 0x109D, 0x109D }, { 0x1160, 0x11FF }, { 0x135D, 0x135F },
    { 0x1712, 0x1714 }, { 0x1732, 0x1734 }, { 0x1752, 0x1753 },
    { 0x1772, 0x1773 }, { 0x17B4, 0x17B5 }, { 0x17B7, 0x17BD },
    { 0x17C6, 0x17C6 }, { 0x17C9, 0x17D3 }, { 0x17DD, 0x17DD },
    { 0x180B, 0x180E }, { 0x18A9, 0x18A9 }, { 0x1920, 0x1922 },
    { 0x1927, 0x1928 }, { 0x1932, 0x1932 }, { 0x1939, 0x193B },
    { 0x1A17, 0x1A18 }, { 0x1A1B, 0x1A1B }, { 0x1A56, 0x1A56 },
    { 0x1A58, 0x1A5E }, { 0x1A60, 0x1A60 }, { 0x1A62, 0x1A62 },
    { 0x1A65, 0x1A6C }, { 0x1A73, 0x1A7C }, { 0x1A7F, 0x1A7F },
    { 0x1AB0, 0x1ABE }, { 0x1B00, 0x1B03 }, { 0x1B34, 0x1B34 },
    { 0x1B36, 0x1B3A }, { 0x1B3C, 0x1B3C }, { 0x1B42, 0x1B42 },
    { 0x1B6B, 0x1B73 }, { 0x1B80, 0x1B81 }, { 0x1BA2, 0x1BA5 },
    { 0x1BA8, 0x1BA9 }, { 0x1BAB, 0x1BAD }, { 0x1BE6, 0x1BE6 },
    { 0x1BE8, 0x1BE9 }, { 0x1BED, 0x1BED }, { 0x1BEF, 0x1BF1 },
    { 0x1C2C, 0x1C33 }, { 0x1C36, 0x1C37 }, { 0x1CD0, 0x1CD2 },
    { 0x1CD4, 0x1CE0 }, { 0x1CE2, 0x1CE8 }, { 0x1CED, 0x1CED },
    { 0x1CF4, 0x1CF4 }, { 0x1CF8, 0x1CF9 }, { 0x1DC0, 0x1DF5 },
    { 0x1DFC, 0x1DFF }, { 0x200B, 0x200F }, { 0x202A, 0x202E },
    { 0x2060, 0x2064 }, { 0x2066, 0x206F }, { 0x20D0, 0x20F0 },
    { 0x2CEF, 0x2CF1 }, { 0x2D7F, 0x2D7F }, { 0x2DE0, 0x2DFF },
    { 0x302A, 0x302D }, { 0x3099, 0x309A }, { 0xA66F, 0xA672 },
    { 0xA674, 0xA67D }, { 0xA69E, 0xA69F }, { 0xA6F0, 0xA6F1 },
    { 0xA802, 0xA802 }, { 0xA806, 0xA806 }, { 0xA80B, 0xA80B },
    { 0xA825, 0xA826 }, { 0xA8C4, 0xA8C5 }, { 0xA8E0, 0xA8F1 },
    { 0xA926, 0xA92D }, { 0xA947, 0xA951 }, { 0xA980, 0xA982 },
    { 0xA9B3, 0xA9B3 }, { 0xA9B6, 0xA9B9 }, { 0xA9BC, 0xA9BC },
    { 0xA9E5, 0xA9E5 }, { 0xAA29, 0xAA2E }, { 0xAA31, 0xAA32 },
    { 0xAA35, 0xAA36 }, { 0xAA43, 0xAA43 }, { 0xAA4C, 0xAA4C },
    { 0xAA7C, 0xAA7C }, { 0xAAB0, 0xAAB0 }, { 0xAAB2, 0xAAB4 },
    { 0xAAB7, 0xAAB8 }, { 0xAABE, 0xAABF }, { 0xAAC1, 0xAAC1 },
    { 0xAAEC, 0xAAED }, { 0xAAF6, 0xAAF6 }, { 0xABE5, 0xABE5 },
    { 0xABE8, 0xABE8 }, { 0xABED, 0xABED }, { 0xD7B0, 0xD7FF },
    { 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
    { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB }, { 0x101FD, 0x101FD },
    { 0x102E0, 0x102E0 }, { 0x10376, 0x1037A }, { 0x10A01, 0x10A03 },
    { 0x10A05, 0x10A06 }, { 0x10A0C, 0x10A0F }, { 0x10A38, 0x10A3A },
    { 0x10A3F, 0x10A3F }, { 0x10AE5, 0x10AE6 }, { 0x11001, 0x11001 },
    { 0x11038, 0x11046 }, { 0x1107F, 0x11081 }, { 0x110B3, 0x110B6 },
    { 0x110B9, 0x110BA }, { 0x11100, 0x11102 }, { 0x11127, 0x1112B },
    { 0x1112D, 0x11134 }, { 0x11173, 0x11173 }, { 0x11180, 0x11181 },
    { 0x111B6, 0x111BE }, { 0x111CA, 0x111CC }, { 0x1122F, 0x11231 },
    { 0x11234, 0x11234 }, { 0x11236, 0x11237 }, { 0x112DF, 0x112DF },
    { 0x112E3, 0x112EA }, { 0x11300, 0x11301 }, { 0x1133C, 0x1133C },
    { 0x11340, 0x11340 }, { 0x11366, 0x1136C }, { 0x11370, 0x11374 },
    { 0x114B3, 0x114B8 }, { 0x114BA, 0x114BA }, { 0x114BF, 0x114C0 },
    { 0x114C2, 0x114C3 }, { 0x115B2, 0x115B5 }, { 0x115BC, 0x115BD },
    { 0x115BF, 0x115C0 }, { 0x115DC, 0x115DD }, { 0x11633, 0x1163A },
    { 0x1163D, 0x1163D }, { 0x1163F, 0x11640 }, { 0x116AB, 0x116AB },
    { 0x116AD, 0x116AD }, { 0x116B0, 0x116B5 }, { 0x116B7, 0x116B7 },
    { 0x1171D, 0x1171F }, { 0x11722, 0x11725 }, { 0x11727, 0x1172B },
    { 0x16AF0, 0x16AF4 }, { 0x16B30, 0x16B36 }, { 0x16F8F, 0x16F92 },
    { 0x1BC9D, 0x1BC9E }, { 0x1D167, 0x1D169 }, { 0x1D17B, 0x1D182 },
    { 0x1D185, 0x1D18B }, { 0x1D1AA, 0x1D1AD }, { 0x1D242, 0x1D244 },
    { 0x1DA00, 0x1DA36 }, { 0x1DA3B, 0x1DA6C }, { 0x1DA75, 0x1DA75 },
    { 0x1DA84, 0x1DA84 }, { 0x1DA9B, 0x1DA9F }, { 0x1DAA1, 0x1DAAF },
    { 0x1E8D0, 0x1E8D6 },
};

/* Sorted lookup. The tables are small enough (a few hundred entries) that a
 * binary search costs about eight compares - less than the glyph lookup that
 * follows it, and it keeps the data as the one thing to check. */
static int cp_in(const CpRange *t, size_t n, u32 cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].lo) hi = mid;
        else if (cp > t[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int font_cell_width(u32 cp) {
    /* C0, DEL and C1 are not printable: a terminal has already acted on them,
     * and nothing they could occupy belongs in the grid. Everything below
     * U+0300 is one cell (the combining blocks all start at U+0300). */
    if (cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) return 0;
    if (cp < 0x0300u) return 1;
    if (cp_in(CP_ZERO, sizeof CP_ZERO / sizeof CP_ZERO[0], cp)) return 0;
    if (cp_in(CP_WIDE, sizeof CP_WIDE / sizeof CP_WIDE[0], cp)) return 2;
    return 1;
}

/* Decode one UTF-8 sequence from s[0..len). Returns the codepoint, or -1 when
 * the bytes are not a valid sequence (a stray continuation byte, an overlong
 * form, a surrogate, or a value above U+10FFFF); *bytes is always at least 1,
 * so every walk of a string advances and none can loop.
 *
 * The decoder in font_draw() used to accept the first byte of a 2-4 byte
 * sequence plus whatever followed it, which is how a name cut in the middle of
 * a Cyrillic letter made the dots after it vanish into one wrong glyph. */
int font_utf8_next(const char *s, size_t len, int *bytes) {
    const unsigned char *p = (const unsigned char *)s;
    if (bytes) *bytes = 1;
    if (!s || len == 0) return -1;
    unsigned c = p[0];
    if (c < 0x80u) return (int)c;

    int need;
    u32 cp, min;
    if (c >= 0xC2u && c <= 0xDFu)      { need = 1; cp = c & 0x1Fu; min = 0x80u; }
    else if (c >= 0xE0u && c <= 0xEFu) { need = 2; cp = c & 0x0Fu; min = 0x800u; }
    else if (c >= 0xF0u && c <= 0xF4u) { need = 3; cp = c & 0x07u; min = 0x10000u; }
    else return -1;

    if ((size_t)need + 1u > len) return -1;
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return -1;
        cp = (cp << 6) | (u32)(p[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return -1;
    if (bytes) *bytes = need + 1;
    return (int)cp;
}

/* The largest k <= n for which s[0..k) is a whole number of codepoints: the
 * cut a string may be shortened to without ever splitting a character. A
 * string that is already valid up to n comes back as n. */
size_t font_utf8_boundary(const char *s, size_t n) {
    if (!s) return 0;
    size_t total = strlen(s);
    if (n >= total) return total;
    size_t i = 0;
    while (i < n) {
        int step = 1;
        if (total - i >= 1u) font_utf8_next(s + i, total - i, &step);
        if (step < 1) step = 1;
        if (i + (size_t)step > n) break;      /* would split this character */
        i += (size_t)step;
    }
    return i;
}

/* The start of the codepoint that ends at n (n > 0): the previous cut a
 * shorten-by-one loop may take. */
size_t font_utf8_prev(const char *s, size_t n) {
    if (!s || n == 0) return 0;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0u) == 0x80u) i--;
    return i;
}
/* @utf8-only-end */

static void tag_str(const u8 *p, char out[5]) {
    for (int i = 0; i < 4; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    out[4] = 0;
}

/* UTF-8 append used when a font's name table is UTF-16BE. Returns 0 when the
 * buffer is full, which simply truncates the name. */
static int utf8_put(char *out, size_t cap, size_t *used, unsigned cp) {
    if (*used + 4 >= cap) return 0;
    if (cp < 0x80) {
        out[(*used)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*used)++] = (char)(0xC0 | (cp >> 6));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*used)++] = (char)(0xE0 | (cp >> 12));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*used)++] = (char)(0xF0 | (cp >> 18));
        out[(*used)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    }
    out[*used] = 0;
    return 1;
}

/* ------------------------------------------------------------
 *  Where a font file starts, and whether it is safe to parse
 *
 *  stb_truetype trusts a font's own table directory: it has no
 *  length argument, so a file truncated in the wrong place would
 *  make it read past the end of our buffer. Everything below is
 *  a bounds check done BEFORE stb_truetype gets the pointer:
 *  the sfnt directory, every table extent, the cmap subtables,
 *  hmtx/loca sizes, every TrueType glyph outline, and the CFF
 *  index chain. A file that fails is skipped with its path and
 *  the reason on stderr.
 * ------------------------------------------------------------ */
enum {
    T_CMAP, T_HEAD, T_HHEA, T_HMTX, T_MAXP, T_LOCA, T_GLYF,
    T_CFF, T_OS2, T_NAME, T_POST, T_COUNT
};

static const char TABLE_TAGS[T_COUNT][5] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "loca", "glyf",
    "CFF ", "OS/2", "name", "post"
};

typedef struct {
    int      present;
    unsigned off, len;
} Tab;

static int validate_name_table(const u8 *d, unsigned off, unsigned len,
                               char *why, size_t cap) {
    if (len < 6) {
        snprintf(why, cap, "the name table is only %u bytes", len);
        return -1;
    }
    unsigned count = be16(d + off + 2);
    unsigned str_off = be16(d + off + 4);
    if (6u + 12u * count > len) {
        snprintf(why, cap, "the name table has %u records that do not fit its %u bytes", count, len);
        return -1;
    }
    if (str_off > len) {
        snprintf(why, cap, "the name table string area starts at %u in a %u-byte table", str_off, len);
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        const u8 *rec = d + off + 6 + (long)i * 12;
        /* record: platform, encoding, language, nameID, length, offset */
        unsigned slen = be16(rec + 8);
        unsigned soff = be16(rec + 10);
        if ((unsigned long)str_off + soff + slen > len) {
            snprintf(why, cap, "name record %u points outside the name table", i);
            return -1;
        }
    }
    return 0;
}

static int validate_cmap_table(const u8 *d, unsigned off, unsigned len,
                               char *why, size_t cap) {
    if (len < 4) {
        snprintf(why, cap, "the cmap table is only %u bytes", len);
        return -1;
    }
    unsigned n = be16(d + off + 2);
    if (4u + 8u * n > len) {
        snprintf(why, cap, "the cmap table declares %u subtables that do not fit its %u bytes", n, len);
        return -1;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned sub = be32(d + off + 4 + (long)i * 8 + 4);
        if ((unsigned long)sub + 4 > len) {
            snprintf(why, cap, "cmap subtable %u starts outside the cmap table", i);
            return -1;
        }
        unsigned fmt = be16(d + off + sub);
        unsigned long need = 0;
        unsigned long slen = 0;
        if (fmt == 0 || fmt == 2 || fmt == 4 || fmt == 6) {
            need = 4;
            if ((unsigned long)sub + need > len) {
                snprintf(why, cap, "cmap format %u subtable %u is truncated", fmt, i);
                return -1;
            }
            slen = be16(d + off + sub + 2);
        } else if (fmt == 8 || fmt == 10 || fmt == 12 || fmt == 13) {
            need = 8;
            if ((unsigned long)sub + need > len) {
                snprintf(why, cap, "cmap format %u subtable %u is truncated", fmt, i);
                return -1;
            }
            slen = be32(d + off + sub + 4);
        } else {
            continue;   /* a format stb_truetype does not read */
        }
        if (slen < need || (unsigned long)sub + slen > len) {
            snprintf(why, cap, "cmap format %u subtable %u claims %lu bytes in a %u-byte table",
                     fmt, i, slen, len);
            return -1;
        }
    }
    return 0;
}

/* Every simple glyph's point data and every composite's component list must
 * fit inside the glyph's own outline. This is exactly the structure
 * stb_truetype walks when it builds a shape, so checking it here means its
 * reads cannot leave the file. */
static int validate_glyph_outline(const u8 *d, long glyf, unsigned start, unsigned end,
                                  unsigned index, char *why, size_t cap) {
    long g = glyf + (long)start;
    long stop = glyf + (long)end;
    long span = stop - g;
    if (span < 10) {
        snprintf(why, cap, "glyph %u is shorter than a glyph header", index);
        return -1;
    }
    int contours = be16s(d + g);
    long p = g + 10;
    if (contours > 0) {
        if ((long)contours * 2 + 12 > span) {
            snprintf(why, cap, "glyph %u has %d contours that do not fit its %ld bytes",
                     index, contours, span);
            return -1;
        }
        unsigned points = (unsigned)be16(d + g + 10 + (long)(contours - 1) * 2) + 1;
        p += (long)contours * 2;
        unsigned ilen = be16(d + p);
        p += 2;
        if (p + (long)ilen > stop) {
            snprintf(why, cap, "glyph %u instructions run past its outline", index);
            return -1;
        }
        p += ilen;
        /* stb_truetype reads all point flags first, then every x coordinate,
         * then every y coordinate. Validate the three regions in that order:
         * a per-point interleaved walk would consume coordinate bytes as if
         * they were flags. */
        long flags_start = p;
        for (unsigned i = 0; i < points; ) {
            if (p >= stop) {
                snprintf(why, cap, "glyph %u point flags run past its outline", index);
                return -1;
            }
            unsigned fl = d[p++];
            unsigned rep = 1;
            if (fl & 0x08) {
                if (p >= stop) {
                    snprintf(why, cap, "glyph %u repeat flag runs past its outline", index);
                    return -1;
                }
                rep += d[p++];
            }
            if (i + rep > points) {
                snprintf(why, cap, "glyph %u point repeat count exceeds its point count", index);
                return -1;
            }
            i += rep;
        }
        long flags_end = p;
        unsigned long xbytes = 0, ybytes = 0;
        p = flags_start;
        for (unsigned i = 0; i < points; ) {
            unsigned fl = d[p++];
            unsigned rep = 1;
            if (fl & 0x08) rep += d[p++];
            xbytes += (unsigned long)((fl & 0x02) ? 1u : ((fl & 0x10) ? 0u : 2u)) * rep;
            ybytes += (unsigned long)((fl & 0x04) ? 1u : ((fl & 0x20) ? 0u : 2u)) * rep;
            i += rep;
        }
        if ((unsigned long)(stop - flags_end) < xbytes + ybytes) {
            snprintf(why, cap, "glyph %u point coordinates run past its outline", index);
            return -1;
        }
    } else if (contours < 0) {
        for (int guard = 0;; guard++) {
            if (guard >= 512) {
                snprintf(why, cap, "glyph %u has too many composite components", index);
                return -1;
            }
            if (p + 4 > stop) {
                snprintf(why, cap, "glyph %u composite header runs past its outline", index);
                return -1;
            }
            unsigned flags = be16(d + p);
            p += 4;
            p += (flags & 0x0001) ? 4 : 2;           /* ARG_1_AND_2_ARE_WORDS */
            if (flags & 0x0008) p += 2;              /* WE_HAVE_A_SCALE      */
            else if (flags & 0x0040) p += 4;         /* X_AND_Y_SCALE        */
            else if (flags & 0x0080) p += 8;         /* TWO_BY_TWO           */
            if (p > stop) {
                snprintf(why, cap, "glyph %u composite arguments run past its outline", index);
                return -1;
            }
            if (!(flags & 0x0020)) break;            /* MORE_COMPONENTS      */
        }
    }
    return 0;
}

static unsigned long cff_offset(const u8 *p, unsigned n) {
    unsigned long v = 0;
    for (unsigned i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* One CFF INDEX; advances *pos past it. Count/offSize/offsets/data must all
 * fit inside the CFF table or the font is rejected. */
static int cff_index_ok(const u8 *d, long off, long end, long *pos) {
    long p = *pos;
    if (p + 2 > end) return -1;
    unsigned count = be16(d + p);
    p += 2;
    if (count == 0) { *pos = p; return 0; }
    if (p + 1 > end) return -1;
    unsigned off_size = d[p++];
    if (off_size < 1 || off_size > 4) return -1;
    unsigned long off_bytes = (unsigned long)(count + 1) * off_size;
    if ((unsigned long)(end - p) < off_bytes) return -1;
    long base = p + (long)off_bytes;
    unsigned long first = cff_offset(d + p, off_size);
    unsigned long last = cff_offset(d + p + (long)count * off_size, off_size);
    if (first < 1 || last < first) return -1;
    if ((unsigned long)(end - base) < last - 1) return -1;
    *pos = base + (long)(last - 1);
    return 0;
}

static int validate_cff_table(const u8 *d, unsigned off, unsigned len,
                              char *why, size_t cap) {
    if (len < 4) {
        snprintf(why, cap, "the CFF table is only %u bytes", len);
        return -1;
    }
    unsigned hdr = d[off + 2];
    if (hdr < 4 || hdr > len) {
        snprintf(why, cap, "the CFF header size %u does not fit the %u-byte table", hdr, len);
        return -1;
    }
    long start = (long)off + (long)hdr;
    long end = (long)off + (long)len;
    long pos = start;
    /* name INDEX, top DICT INDEX, string INDEX, global subr INDEX */
    for (int i = 0; i < 4; i++) {
        if (cff_index_ok(d, (long)off, end, &pos) != 0) {
            snprintf(why, cap, "CFF index %d does not fit the %u-byte table", i, len);
            return -1;
        }
    }
    return 0;
}

/* Returns the offset of the face inside the file, or -1 with `why` filled in.
 * This is the only gate between a file on disk and stb_truetype. */
static int validate_sfnt(const u8 *d, long len, int *out_off, char *why, size_t cap) {
    if (len < 12) {
        snprintf(why, cap, "only %ld bytes; too short for an sfnt header", len);
        return -1;
    }
    u32 magic = be32(d);
    long off = 0;
    if (magic == 0x74746366u) {                  /* 'ttcf' collection */
        if (len < 16) {
            snprintf(why, cap, "truncated TrueType collection header");
            return -1;
        }
        u32 n = be32(d + 8);
        if (n == 0 || n > 64) {
            snprintf(why, cap, "collection declares %u faces", (unsigned)n);
            return -1;
        }
        if ((long)16 + (long)n * 4 > len) {
            snprintf(why, cap, "collection face table is truncated");
            return -1;
        }
        off = (long)be32(d + 12);
        if (off + 12 > len) {
            snprintf(why, cap, "collection face starts past the end of the file");
            return -1;
        }
    }
    u32 tag = be32(d + off);
    if (tag != 0x00010000u && tag != 0x74727565u /* 'true' */ &&
        tag != 0x4F54544Fu /* 'OTTO' */) {
        snprintf(why, cap, "not a TrueType/OpenType font (sfnt magic 0x%08X)", (unsigned)tag);
        return -1;
    }
    unsigned nt = be16(d + off + 4);
    if (nt == 0) {
        snprintf(why, cap, "the sfnt declares no tables");
        return -1;
    }
    if (nt > 512) {
        snprintf(why, cap, "the sfnt declares %u tables", nt);
        return -1;
    }
    long dir_end = off + 12 + (long)nt * 16;
    if (dir_end > len) {
        snprintf(why, cap, "the %u-entry table directory needs %ld bytes; file has %ld", nt, dir_end, len);
        return -1;
    }

    Tab tab[T_COUNT];
    memset(tab, 0, sizeof(tab));
    for (unsigned i = 0; i < nt; i++) {
        const u8 *rec = d + off + 12 + (long)i * 16;
        char ts[5];
        tag_str(rec, ts);
        unsigned toff = be32(rec + 8);
        unsigned tlen = be32(rec + 12);
        if ((u64)toff + (u64)tlen > (u64)len) {
            snprintf(why, cap, "table '%s' (%u bytes at %u) runs past the %ld-byte file",
                     ts, tlen, toff, len);
            return -1;
        }
        for (int k = 0; k < T_COUNT; k++)
            if (memcmp(rec, TABLE_TAGS[k], 4) == 0) {
                tab[k].present = 1;
                tab[k].off = toff;
                tab[k].len = tlen;
            }
    }

    if (!tab[T_CMAP].present || !tab[T_HEAD].present || !tab[T_HHEA].present ||
        !tab[T_HMTX].present || !tab[T_MAXP].present) {
        snprintf(why, cap, "a required table (cmap/head/hhea/hmtx/maxp) is missing");
        return -1;
    }
    if (tab[T_HEAD].len < 54) {
        snprintf(why, cap, "the head table is %u bytes (needs 54)", tab[T_HEAD].len);
        return -1;
    }
    if (tab[T_HHEA].len < 36) {
        snprintf(why, cap, "the hhea table is %u bytes (needs 36)", tab[T_HHEA].len);
        return -1;
    }
    if (tab[T_MAXP].len < 6) {
        snprintf(why, cap, "the maxp table is %u bytes (needs 6)", tab[T_MAXP].len);
        return -1;
    }
    unsigned glyphs = be16(d + tab[T_MAXP].off + 4);
    if (glyphs == 0) {
        snprintf(why, cap, "the font declares no glyphs");
        return -1;
    }
    unsigned hmetrics = be16(d + tab[T_HHEA].off + 34);
    if (hmetrics == 0 || hmetrics > glyphs) {
        snprintf(why, cap, "hhea declares %u long metrics for %u glyphs", hmetrics, glyphs);
        return -1;
    }
    unsigned long hmtx_need = 4ul * hmetrics + 2ul * (glyphs - hmetrics);
    if (tab[T_HMTX].len < hmtx_need) {
        snprintf(why, cap, "the hmtx table is %u bytes; %u glyphs need at least %lu",
                 tab[T_HMTX].len, glyphs, hmtx_need);
        return -1;
    }
    if (tab[T_NAME].present &&
        validate_name_table(d, tab[T_NAME].off, tab[T_NAME].len, why, cap) != 0)
        return -1;
    if (validate_cmap_table(d, tab[T_CMAP].off, tab[T_CMAP].len, why, cap) != 0)
        return -1;

    if (tag == 0x4F54544Fu) {
        if (!tab[T_CFF].present) {
            snprintf(why, cap, "an OTTO font without a CFF table");
            return -1;
        }
        if (validate_cff_table(d, tab[T_CFF].off, tab[T_CFF].len, why, cap) != 0)
            return -1;
    } else {
        if (!tab[T_LOCA].present || !tab[T_GLYF].present) {
            snprintf(why, cap, "a TrueType font without loca/glyf");
            return -1;
        }
        unsigned loc_format = be16(d + tab[T_HEAD].off + 50);
        if (loc_format > 1) {
            snprintf(why, cap, "head declares indexToLocFormat %u", loc_format);
            return -1;
        }
        unsigned long loca_need = (unsigned long)(glyphs + 1) * (loc_format ? 4u : 2u);
        if (tab[T_LOCA].len < loca_need) {
            snprintf(why, cap, "the loca table is %u bytes; %u glyphs need %lu",
                     tab[T_LOCA].len, glyphs, loca_need);
            return -1;
        }
        for (unsigned g = 0; g < glyphs; g++) {
            unsigned a = loc_format ? be32(d + tab[T_LOCA].off + (long)g * 4)
                                    : (unsigned)be16(d + tab[T_LOCA].off + (long)g * 2) * 2u;
            unsigned b = loc_format ? be32(d + tab[T_LOCA].off + (long)(g + 1) * 4)
                                    : (unsigned)be16(d + tab[T_LOCA].off + (long)(g + 1) * 2) * 2u;
            if (a > b || b > tab[T_GLYF].len) {
                snprintf(why, cap, "glyph %u outline (%u..%u) is outside the %u-byte glyf table",
                         g, a, b, tab[T_GLYF].len);
                return -1;
            }
            if (a != b && validate_glyph_outline(d, (long)tab[T_GLYF].off, a, b, g, why, cap) != 0)
                return -1;
        }
    }
    *out_off = (int)off;
    return 0;
}

/* ------------------------------------------------------------
 *  Reading one font file
 * ------------------------------------------------------------ */
static int face_name(const stbtt_fontinfo *info, int name_id, char *out, size_t cap) {
    static const struct { int platform, encoding, language; } prefs[] = {
        { 3, 1, 0x409 }, { 3, 10, 0x409 }, { 0, 3, 0 }, { 0, 4, 0 }, { 1, 0, 0 }
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(prefs) / sizeof(prefs[0]); i++) {
        int n = 0;
        const unsigned char *raw = (const unsigned char *)stbtt_GetFontNameString(
            info, &n, prefs[i].platform, prefs[i].encoding, prefs[i].language, name_id);
        if (!raw || n <= 0) continue;
        size_t used = 0;
        if (prefs[i].platform == 1) {
            for (int j = 0; j < n && used + 2 < cap; j++) {
                unsigned c = raw[j];
                if (c >= 0x20 && c != 0x7F) out[used++] = (char)c;   /* MacRoman ASCII range */
            }
            out[used] = 0;
        } else {
            if (n & 1) n--;       /* UTF-16BE needs whole code units */
            for (int j = 0; j + 1 < n; j += 2) {
                unsigned u = ((unsigned)raw[j] << 8) | raw[j + 1];
                if (u >= 0xD800 && u <= 0xDBFF && j + 3 < n) {
                    unsigned lo = ((unsigned)raw[j + 2] << 8) | raw[j + 3];
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        j += 2;
                    }
                }
                if (u < 0x20 || u == 0x7F) continue;
                if (!utf8_put(out, cap, &used, u)) break;
            }
            out[used] = 0;
        }
        char *t = trim_ws(out);
        if (t != out) memmove(out, t, strlen(t) + 1);
        if (out[0]) return 0;
    }
    return -1;
}

static long table_offset(const Face *f, const char *tag) {
    const u8 *d = f->data;
    u32 base = (u32)f->info.fontstart;
    if ((long)base + 12 > f->len) return -1;
    unsigned n = be16(d + base + 4);
    for (unsigned i = 0; i < n; i++) {
        long rec = (long)base + 12 + (long)i * 16;
        if (rec + 16 > f->len) return -1;
        if (memcmp(d + rec, tag, 4) == 0) {
            u32 off = be32(d + rec + 8);
            u32 len = be32(d + rec + 12);
            if ((long)off + (long)len > f->len) return -1;
            return (long)off;
        }
    }
    return -1;
}

static int weight_from_style_name(const char *s) {
    char low[64];
    size_t n = strlen(s);
    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    low[n] = 0;
    if (strstr(low, "thin")) return 100;
    if (strstr(low, "extralight") || strstr(low, "ultralight")) return 200;
    if (strstr(low, "semibold") || strstr(low, "demibold")) return 600;
    if (strstr(low, "extrabold") || strstr(low, "ultrabold")) return 800;
    if (strstr(low, "light")) return 300;
    if (strstr(low, "medium")) return 500;
    if (strstr(low, "black") || strstr(low, "heavy")) return 900;
    if (strstr(low, "bold")) return 700;
    return 0;
}

static void style_from_tables(const Face *f, int *weight, int *italic) {
    *weight = 0;
    *italic = 0;
    long os2 = table_offset(f, "OS/2");
    if (os2 >= 0 && os2 + 6 <= f->len) {
        unsigned w = be16(f->data + os2 + 4);
        if (w >= 1 && w <= 1000) *weight = (int)w;
        if (os2 + 64 <= f->len) {
            unsigned sel = be16(f->data + os2 + 62);
            if (sel & 0x0001u) *italic = 1;
        }
    }
    long head = table_offset(f, "head");
    if (head >= 0 && head + 46 <= f->len) {
        unsigned style = be16(f->data + head + 44);
        if (style & 0x0002u) *italic = 1;
    }
}

/* A monospaced face is one whose 'i' and 'W' advance by the same amount. That
 * is a property of the real hmtx metrics, not of the file name. post's
 * isFixedPitch is the fallback for fonts that lack those two glyphs. */
static int face_is_mono(const Face *f) {
    static const int probe[] = { 'i', 'W', '0', 'm' };
    int advance = 0, compared = 0, same = 1;
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        int gi = stbtt_FindGlyphIndex(&f->info, probe[i]);
        if (gi <= 0) { same = 0; break; }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        if (!compared) { advance = adv; compared = 1; }
        else if (adv != advance) { same = 0; break; }
    }
    if (compared && same && advance > 0) return 1;
    long post = table_offset(f, "post");
    if (post >= 0 && post + 16 <= f->len) {
        if (be32(f->data + post + 12) != 0) return 1;
    }
    return 0;
}

static void family_from_filename(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", base_name(path));
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* A candidate font file: validate, parse, and append to faces[]. Returns 0 on
 * success, -1 when skipped (with the path and the reason already logged). */
static int face_add(const char *path) {
    if (face_count >= MAX_FACES) {
        fprintf(stderr, "font: skip %s: face table is full (%d)\n", path, MAX_FACES);
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "font: skip %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "font: skip %s: cannot seek: %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    long len = ftell(fp);
    if (len <= 0) {
        fprintf(stderr, "font: skip %s: %s\n", path, len == 0 ? "empty file" : strerror(errno));
        fclose(fp);
        return -1;
    }
    if (len > FONT_FILE_MAX) {
        fprintf(stderr, "font: skip %s: %ld bytes is larger than the %ld-byte limit\n",
                path, len, FONT_FILE_MAX);
        fclose(fp);
        return -1;
    }
    rewind(fp);
    u8 *data = malloc((size_t)len);
    if (!data) {
        fprintf(stderr, "font: skip %s: out of memory for %ld bytes\n", path, len);
        fclose(fp);
        return -1;
    }
    if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "font: skip %s: could not read %ld bytes\n", path, len);
        free(data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char why[256];
    int offset = 0;
    if (validate_sfnt(data, len, &offset, why, sizeof(why)) != 0) {
        fprintf(stderr, "font: skip %s: %s\n", path, why);
        free(data);
        return -1;
    }

    stbtt_fontinfo info;
    memset(&info, 0, sizeof(info));
    if (!stbtt_InitFont(&info, data, offset)) {
        fprintf(stderr, "font: skip %s: stb_truetype rejected the font tables\n", path);
        free(data);
        return -1;
    }
    if (info.numGlyphs <= 0) {
        fprintf(stderr, "font: skip %s: the font contains no glyphs\n", path);
        free(data);
        return -1;
    }
    float scale = stbtt_ScaleForPixelHeight(&info, 16.0f);
    if (!(scale > 0.0f) || !isfinite(scale)) {
        fprintf(stderr, "font: skip %s: the font has an unusable unitsPerEm\n", path);
        free(data);
        return -1;
    }

    Face *f = &faces[face_count];
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->len = len;
    f->info = info;
    snprintf(f->path, sizeof(f->path), "%s", path);
    if (face_name(&info, 1, f->family, sizeof(f->family)) != 0 || !f->family[0])
        family_from_filename(path, f->family, sizeof(f->family));

    int weight = 0, italic = 0;
    style_from_tables(f, &weight, &italic);
    if (weight == 0) {
        char style[64] = { 0 };
        if (face_name(&info, 2, style, sizeof(style)) == 0)
            weight = weight_from_style_name(style);
        if (weight == 0) weight = 400;
    }
    f->weight = weight;
    f->italic = italic;
    f->mono = face_is_mono(f);

    /* /root/fonts is scanned first, so its copy of a family wins over the
     * system one. Keeping both would only make the role pick ambiguous. */
    for (int i = 0; i < face_count; i++) {
        if (ci_equal(faces[i].family, f->family) &&
            faces[i].weight == f->weight && faces[i].italic == f->italic) {
            fprintf(stderr, "font: shadowed %s (same family/weight as %s)\n",
                    path, faces[i].path);
            free(data);
            memset(f, 0, sizeof(*f));
            return -1;
        }
    }
    face_count++;
    return 0;
}

/* ------------------------------------------------------------
 *  Scanning directories
 * ------------------------------------------------------------ */
static int name_cmp(const void *a, const void *b) {
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    while (*sa && *sb) {
        int ca = (unsigned char)*sa, cb = (unsigned char)*sb;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        sa++; sb++;
    }
    return (unsigned char)*sa - (unsigned char)*sb;
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        /* No user font directory is the normal case, not an error. */
        if (errno != ENOENT)
            fprintf(stderr, "font: cannot scan %s: %s\n", dir, strerror(errno));
        return;
    }
    static char names[MAX_DIR_FILES][128];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!has_font_ext(de->d_name)) continue;
        if (n >= MAX_DIR_FILES) {
            fprintf(stderr, "font: %s has more than %d font files; ignoring the rest\n",
                    dir, MAX_DIR_FILES);
            break;
        }
        if (strlen(de->d_name) >= sizeof(names[0])) {
            fprintf(stderr, "font: skip %s/%s: file name is too long\n", dir, de->d_name);
            continue;
        }
        snprintf(names[n], sizeof(names[n]), "%s", de->d_name);
        n++;
    }
    closedir(d);

    /* readdir order is filesystem-dependent; sorting makes the face list and
     * therefore every fallback choice repeatable. */
    qsort(names, (size_t)n, sizeof(names[0]), name_cmp);
    for (int i = 0; i < n; i++) {
        char path[FONT_PATH_LEN];
        int wrote = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (wrote < 0 || wrote >= (int)sizeof(path)) {
            fprintf(stderr, "font: skip %s/%s: path is too long\n", dir, names[i]);
            continue;
        }
        face_add(path);
    }
}

/* ------------------------------------------------------------
 *  /etc/mystic/fonts.conf
 *
 *      # one directive per line
 *      ui   = Lato
 *      mono = JetBrains Mono
 *
 *  The value is a family name from the font's name table, or a
 *  file name; both are matched case-insensitively. Missing file,
 *  unknown roles and unknown families are reported and ignored.
 * ------------------------------------------------------------ */
static void load_conf(void) {
    conf_ui[0] = 0;
    conf_mono[0] = 0;
    FILE *f = fopen(FONT_CONF, "r");
    if (!f) return;                       /* optional: defaults still work */
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = trim_ws(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "font: %s:%d: expected 'role = family'\n", FONT_CONF, lineno);
            continue;
        }
        *eq = 0;
        char *key = trim_ws(s);
        char *val = trim_ws(eq + 1);
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') ||
                          (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val[vlen - 1] = 0;
            val++;
        }
        val = trim_ws(val);
        if (!*val) {
            fprintf(stderr, "font: %s:%d: %s has an empty family name\n", FONT_CONF, lineno, key);
            continue;
        }
        if (ci_equal(key, "ui")) snprintf(conf_ui, sizeof(conf_ui), "%s", val);
        else if (ci_equal(key, "mono")) snprintf(conf_mono, sizeof(conf_mono), "%s", val);
        else fprintf(stderr, "font: %s:%d: unknown role \"%s\" (use ui or mono)\n",
                     FONT_CONF, lineno, key);
    }
    fclose(f);
}

/* ------------------------------------------------------------
 *  Mapping the four roles onto the installed faces
 * ------------------------------------------------------------ */
static int face_by_basename(const char *name) {
    for (int i = 0; i < face_count; i++)
        if (ci_equal(base_name(faces[i].path), name)) return i;
    return -1;
}

static int weight_distance(int a, int b) {
    int d = a - b;
    return d < 0 ? -d : d;
}

/* Score a face against a user-supplied family spec. A family-name match beats
 * a file-name match beats a file-stem match; within a match the weight nearest
 * to the wanted one wins. Italic faces lose every tie: this build never
 * synthesises an italic, so it draws the upright face instead (docs/FONTS.md). */
static int spec_score(int i, const char *spec, int want_weight) {
    int score;
    if (ci_equal(faces[i].family, spec)) {
        score = 0;
    } else if (ci_equal(base_name(faces[i].path), spec)) {
        score = 20;
    } else {
        char stem[FONT_NAME_LEN];
        family_from_filename(faces[i].path, stem, sizeof(stem));
        if (!ci_equal(stem, spec)) return -1;
        score = 10;
    }
    if (faces[i].italic) score += 1000;
    score += weight_distance(faces[i].weight, want_weight) * 2;
    return score;
}

static int resolve_spec(const char *spec, int want_weight, int mono_only) {
    int best = -1, best_score = INT_MAX;
    for (int i = 0; i < face_count; i++) {
        if (mono_only && !faces[i].mono) continue;
        int s = spec_score(i, spec, want_weight);
        if (s >= 0 && s < best_score) {
            best_score = s;
            best = i;
        }
    }
    return best;
}

static int pick_in_family(const char *family, int want_weight, int mono_only) {
    int best = -1, best_score = INT_MAX;
    for (int i = 0; i < face_count; i++) {
        if (!ci_equal(faces[i].family, family)) continue;
        if (mono_only && !faces[i].mono) continue;
        int score = weight_distance(faces[i].weight, want_weight) * 2;
        if (faces[i].italic) score += 1000;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int default_ui_face(void) {
    for (const char **p = DEFAULT_UI_FILES; *p; p++) {
        int i = face_by_basename(*p);
        if (i >= 0) return i;
    }
    for (int i = 0; i < face_count; i++) if (!faces[i].mono && !faces[i].italic) return i;
    for (int i = 0; i < face_count; i++) if (!faces[i].italic) return i;
    return face_count ? 0 : -1;
}

static int default_mono_face(void) {
    for (const char **p = DEFAULT_MONO_FILES; *p; p++) {
        int i = face_by_basename(*p);
        if (i >= 0) return i;
    }
    for (int i = 0; i < face_count; i++) if (faces[i].mono && !faces[i].italic) return i;
    for (int i = 0; i < face_count; i++) if (faces[i].mono) return i;
    return -1;
}

static void set_role_family(int role, const Face *f) {
    if (role < 0 || role >= FONT_COUNT) return;
    snprintf(role_family[role], sizeof(role_family[role]), "%s", f->family);
}

static void build_roles(void) {
    for (int i = 0; i < FONT_COUNT; i++) {
        role_face[i] = -1;
        role_family[i][0] = 0;
    }
    if (face_count == 0) {
        fprintf(stderr, "font: no usable font faces found in %s or %s\n",
                USER_FONT_DIR, SYSTEM_FONT_DIR);
        return;
    }

    int ui = -1;
    if (conf_ui[0]) {
        ui = resolve_spec(conf_ui, 400, 0);
        if (ui < 0)
            fprintf(stderr, "font: %s: no installed face matches ui family \"%s\"; using the default\n",
                    FONT_CONF, conf_ui);
    }
    if (ui < 0) ui = default_ui_face();
    if (ui >= 0) {
        int regular = pick_in_family(faces[ui].family, 400, 0);
        int medium = pick_in_family(faces[ui].family, 500, 0);
        int semibold = pick_in_family(faces[ui].family, 600, 0);
        if (regular < 0) regular = ui;
        if (medium < 0) medium = regular;
        if (semibold < 0) semibold = regular;
        role_face[FONT_REGULAR] = regular;
        role_face[FONT_MEDIUM] = medium;
        role_face[FONT_SEMIBOLD] = semibold;
        set_role_family(FONT_REGULAR, &faces[regular]);
        set_role_family(FONT_MEDIUM, &faces[medium]);
        set_role_family(FONT_SEMIBOLD, &faces[semibold]);
    }

    int mono = -1;
    if (conf_mono[0]) {
        mono = resolve_spec(conf_mono, 400, 0);
        if (mono < 0) {
            fprintf(stderr, "font: %s: no installed face matches mono family \"%s\"; using the default\n",
                    FONT_CONF, conf_mono);
        } else if (!faces[mono].mono) {
            fprintf(stderr, "font: %s: mono family \"%s\" is not monospaced; the terminal will use it anyway\n",
                    FONT_CONF, faces[mono].family);
        }
    }
    if (mono < 0) mono = default_mono_face();
    if (mono < 0) {
        if (role_face[FONT_REGULAR] >= 0) {
            role_face[FONT_MONO] = role_face[FONT_REGULAR];
            set_role_family(FONT_MONO, &faces[role_face[FONT_REGULAR]]);
            fprintf(stderr, "font: no monospaced face installed; the terminal falls back to %s\n",
                    role_family[FONT_MONO]);
        }
    } else {
        int picked = pick_in_family(faces[mono].family, 400, faces[mono].mono ? 1 : 0);
        role_face[FONT_MONO] = picked >= 0 ? picked : mono;
        set_role_family(FONT_MONO, &faces[role_face[FONT_MONO]]);
    }

    fprintf(stderr, "font: %d face%s: ui=%s mono=%s\n", face_count,
            face_count == 1 ? "" : "s",
            role_family[FONT_REGULAR][0] ? role_family[FONT_REGULAR] : "(none)",
            role_family[FONT_MONO][0] ? role_family[FONT_MONO] : "(none)");
}

/* ------------------------------------------------------------
 *  Lifecycle
 * ------------------------------------------------------------ */
static void free_fonts(void) {
    Font *f = fonts;
    while (f) {
        Font *next = f->next;
        for (int i = 0; i < MAX_GLYPHS; i++) free(f->cache[i].bitmap);
        free(f);
        f = next;
    }
    fonts = NULL;
}

static void free_faces(void) {
    for (int i = 0; i < face_count; i++) {
        free(faces[i].data);
        faces[i].data = NULL;
        faces[i].len = 0;
    }
    face_count = 0;
}

int font_init(const char *dir) {
    font_ready = 0;
    free_fonts();
    free_faces();

    char keep[FONT_PATH_LEN];
    if (dir && *dir) snprintf(keep, sizeof(keep), "%s", dir);
    else if (font_dir[0]) snprintf(keep, sizeof(keep), "%s", font_dir);
    else snprintf(keep, sizeof(keep), "%s", SYSTEM_FONT_DIR);
    snprintf(font_dir, sizeof(font_dir), "%s", keep);

    load_conf();

    /* user fonts first: a family present in both places resolves to the
     * owner's copy, and the shadowed system file is logged. */
    scan_dir(USER_FONT_DIR);
    if (!ci_equal(font_dir, USER_FONT_DIR) && !ci_equal(font_dir, SYSTEM_FONT_DIR))
        scan_dir(font_dir);
    scan_dir(SYSTEM_FONT_DIR);

    build_roles();
    font_ready = 1;
    return face_count > 0 ? 0 : -1;
}

int font_reload(void) {
    /* Rescans the directories and re-reads fonts.conf. Existing Font objects
     * are released: call it when no Font* is in use (a settings click), not
     * from inside a draw. */
    return font_init(font_dir);
}

void font_shutdown(void) {
    font_ready = 0;
    free_fonts();
    free_faces();
    for (int i = 0; i < FONT_COUNT; i++) {
        role_face[i] = -1;
        role_family[i][0] = 0;
    }
    conf_ui[0] = conf_mono[0] = 0;
    font_dir[0] = 0;
}

/* ------------------------------------------------------------
 *  Face lookup for a role + size
 * ------------------------------------------------------------ */
static void font_rebind(Font *f, int source) {
    for (int i = 0; i < MAX_GLYPHS; i++) {
        free(f->cache[i].bitmap);
        f->cache[i].bitmap = NULL;
        f->cache[i].cp = 0;
        f->cache[i].w = f->cache[i].h = 0;
        f->cache[i].xoff = f->cache[i].yoff = 0;
        f->cache[i].advance = 0;
        f->cache[i].used = 0;
        f->cache[i].src = -1;
        f->cache[i].last_use = 0;
    }
    f->use_clock = 0;
    f->source = source;
    f->data = faces[source].data;
    f->info = faces[source].info;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, f->size);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &lg);
    f->ascent = a * f->scale;
    f->descent = d * f->scale;
}

Font *font_get(int face, float size) {
    if (!font_ready || face < 0 || face >= FONT_COUNT) return NULL;

    /* one place applies the global text scale: every caller asks for a
     * nominal size and gets the scaled one back, so no layout can forget. */
    size *= ui_scale_get();
    size = (float)((int)(size * 2.0f + 0.5f)) / 2.0f;
    if (size < 5.0f) size = 5.0f;
    if (size > 96.0f) size = 96.0f;

    int source = role_face[face];
    if (source < 0 || source >= face_count) return NULL;

    for (Font *f = fonts; f; f = f->next) {
        if (f->face != face || fabsf(f->size - size) >= 0.01f) continue;
        /* A settings screen can remap the role at runtime. Re-point the
         * existing (face,size) entry instead of freeing it: a caller still
         * holding the pointer sees a valid font, with the new face. */
        if (f->source != source) font_rebind(f, source);
        return f;
    }

    Font *f = calloc(1, sizeof(Font));
    if (!f) {
        fprintf(stderr, "font: out of memory for a %.1fpx %s face\n", (double)size,
                face == FONT_MONO ? "mono" : "ui");
        return NULL;
    }
    f->face = face;
    f->size = size;
    font_rebind(f, source);
    f->next = fonts;
    fonts = f;
    return f;
}

/* One monotonic counter per face, bumped on every cache access. It only ever
 * orders glyphs against each other, so on the 4-billionth access - where an
 * unsigned would wrap and the ordering would collapse - halving every stored
 * stamp keeps the relative order and buys another 4 billion. */
static unsigned next_use(Font *f) {
    if (f->use_clock >= UINT_MAX - 1) {
        for (int i = 0; i < MAX_GLYPHS; i++) f->cache[i].last_use >>= 1;
        f->use_clock = UINT_MAX / 2;
    }
    return ++f->use_clock;
}

/* ------------------------------------------------------------
 *  Which installed face can draw this codepoint?
 *
 *  The old lookup asked the role's face only: stbtt_FindGlyphIndex()
 *  returning 0 meant the index of glyph 0, .notdef, which is the box
 *  the user saw where a Cyrillic or Japanese letter should be - even
 *  with a face installed that had the letter.  So a miss now walks the
 *  whole face list (the same list the roles were chosen from) and the
 *  first face with real ink in the glyph wins; a face that maps the
 *  codepoint to an empty outline (a colour-emoji layer, a space) is
 *  kept only as a last resort, because an empty bitmap looks exactly
 *  like a missing glyph.
 *
 *  The answer is recorded in the glyph cache entry, including "nowhere"
 *  (-1), so the walk happens once per codepoint per size, not once per
 *  paint.  The face list is sorted by file name at scan time, so the
 *  choice is the same on every run.
 * ------------------------------------------------------------ */
static int face_has_cp(int fi, int cp) {
    if (fi < 0 || fi >= face_count) return 0;
    return stbtt_FindGlyphIndex(&faces[fi].info, cp) != 0;
}

/* Does this face put ink on paper for cp at this pixel size? A codepoint can
 * map to a glyph whose outline is empty (U+00A0, U+200B, a COLR emoji base),
 * and choosing that face would draw nothing at all. */
static int face_has_ink(int fi, int cp, float size) {
    if (fi < 0 || fi >= face_count) return 0;
    int gi = stbtt_FindGlyphIndex(&faces[fi].info, cp);
    if (gi == 0) return 0;
    float scale = stbtt_ScaleForPixelHeight(&faces[fi].info, size);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&faces[fi].info, gi, scale, scale, &x0, &y0, &x1, &y1);
    return (x1 > x0 && y1 > y0);
}

static int face_pick(Font *f, int cp) {
    if (face_has_cp(f->source, cp)) return f->source;
    int empty = -1;
    for (int i = 0; i < face_count; i++) {
        if (i == f->source || !face_has_cp(i, cp)) continue;
        if (face_has_ink(i, cp, f->size)) return i;
        if (empty < 0) empty = i;
    }
    return empty;              /* -1: no installed face has this codepoint */
}

static void glyph_rasterise(Font *f, Glyph *g, int cp, int src) {
    const stbtt_fontinfo *info = &f->info;
    float scale = f->scale;
    if (src >= 0 && src != f->source && src < face_count) {
        /* A fallback face is rasterised at the SAME pixel height as the role's
         * face (that is what keeps the text on one baseline), which is why the
         * scale is derived from this font's size and not from the face. */
        info = &faces[src].info;
        scale = stbtt_ScaleForPixelHeight(info, f->size);
    }
    int glyph_index = stbtt_FindGlyphIndex(info, cp);

    int advance_width, left_bearing;
    stbtt_GetGlyphHMetrics(info, glyph_index, &advance_width, &left_bearing);
    g->advance = advance_width * scale;

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, glyph_index, scale, scale, &x0, &y0, &x1, &y1);
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    g->cp = cp;
    g->used = 1;

    if (g->w <= 0 || g->h <= 0) { g->bitmap = NULL; return; }
    if (g->w > 512 || g->h > 512) { g->w = g->h = 0; return; }
    g->bitmap = malloc((size_t)g->w * g->h);
    if (!g->bitmap) { g->w = g->h = 0; return; }
    stbtt_MakeGlyphBitmap(info, g->bitmap, g->w, g->h, g->w, scale, scale, glyph_index);
}

static Glyph *glyph_get(Font *f, int cp) {
    const unsigned start = (unsigned)cp % MAX_GLYPHS;
    Glyph *empty = NULL;

    /* The table is open-addressed, but eviction can free a slot in the middle
     * of another codepoint's probe run, so a lookup must never stop at the
     * first empty slot: it walks the table and compares codepoints, which is
     * always correct wherever the entry ended up. A hit stops at the match,
     * so the common path stays short; only a miss pays for the full walk. */
    for (unsigned i = 0; i < MAX_GLYPHS; i++) {
        Glyph *g = &f->cache[(start + i) % MAX_GLYPHS];
        if (!g->used) {
            if (!empty) empty = g;
            continue;
        }
        if (g->cp == cp) { g->last_use = next_use(f); return g; }
    }

    Glyph *slot = empty;
    if (!slot) {
        /* Full: evict the least recently used glyph and hand its slot to the
         * new one. Without this the 2048 entries were permanent, so every
         * codepoint first seen after the table filled stayed invisible. */
        slot = &f->cache[0];
        for (int i = 1; i < MAX_GLYPHS; i++)
            if (f->cache[i].last_use < slot->last_use) slot = &f->cache[i];
        free(slot->bitmap);      /* the evicted bitmap must not leak */
        slot->bitmap = NULL;
        slot->used = 0;
    }

    int src = face_pick(f, cp);
    glyph_rasterise(f, slot, cp, src);   /* src < 0 rasterises the primary .notdef */
    slot->src = src;
    slot->last_use = next_use(f);
    return slot;
}

/* Every walk of a string below uses font_utf8_next(), which rejects a broken
 * sequence instead of inventing a codepoint out of the bytes after a name that
 * was cut mid-character. */

float font_height(Font *f) { return f ? f->size * 1.4f : 0.0f; }
float font_ascent(Font *f) { return f ? f->ascent : 0.0f; }

/* Walk one codepoint of a NUL-terminated string. A broken byte is consumed as
 * one byte and returned as U+FFFD, so a consumer that draws it shows the same
 * replacement character the terminal uses for a broken sequence. */
/* Decode one codepoint and advance the cursor.
 *
 * This used to call strlen(s) for EVERY codepoint - on the remaining buffer -
 * which made every string walk quadratic: measuring a 1 MiB text file cost
 * ~5.5e11 byte reads and opening a 4 MiB file in Viewer froze for minutes.
 * The remaining length is tracked by the walkers instead (they know where the
 * NUL is), and this takes an explicit bound so no scan happens per glyph. */
static int next_cp_n(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *s = *pp;
    if (end && s >= end) { *pp = (const unsigned char *)""; return 0; }
    int avail = end ? (int)(end - s) : 4;
    int step = 1;
    int cp = font_utf8_next((const char *)s, avail, &step);
    if (step < 1) step = 1;
    *pp += step;
    return cp < 0 ? 0xFFFD : cp;
}

float font_width(Font *f, const char *text) {
    if (!f || !text) return 0;
    float w = 0;
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + strlen(text);   /* one scan, not one per glyph */
    while (p < end && *p) {
        int cp = next_cp_n(&p, end);
        Glyph *g = glyph_get(f, cp);
        if (g) w += g->advance;
    }
    return w;
}

float font_draw(Surface *s, Font *f, float x, float y, const char *text, u32 color) {
    if (!f || !text) return x;
    float pen = x;
    float base = y + f->ascent;
    float cr = (float)GET_R(color), cg = (float)GET_G(color), cb = (float)GET_B(color);
    float ca = GET_A(color) / 255.0f;

    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + strlen(text);   /* one scan, not one per glyph */
    while (p < end && *p) {
        int cp = next_cp_n(&p, end);
        Glyph *g = glyph_get(f, cp);
        if (!g) continue;

        if (g->bitmap) {
            int gx = (int)(pen + g->xoff);
            int gy = (int)(base + g->yoff);
            for (int yy = 0; yy < g->h; yy++) {
                int dy = gy + yy;
                if ((unsigned)dy >= (unsigned)s->h) continue;
                u32 *row = s->px + (size_t)dy * s->stride;
                const u8 *src = g->bitmap + (size_t)yy * g->w;
                for (int xx = 0; xx < g->w; xx++) {
                    int dx = gx + xx;
                    if ((unsigned)dx >= (unsigned)s->w) continue;
                    /* Honour the surface clip. Glyphs wrote straight into the
                     * framebuffer and ignored it, so any text an app drew
                     * outside its own rect - a label wider than the window, a
                     * heading in a narrow pane - appeared over the wallpaper or
                     * the app beside it. Every other primitive respected the
                     * clip; this is the one that leaked. */
                    if (s->cx1 > s->cx0 &&
                        (dx < s->cx0 || dx >= s->cx1 || dy < s->cy0 || dy >= s->cy1))
                        continue;
                    float cov = (src[xx] / 255.0f) * ca;
                    if (cov <= 0.004f) continue;
                    u32 d = row[dx];
                    float inv = 1.0f - cov;
                    row[dx] = 0xFF000000u
                            | ((u32)(cr * cov + GET_R(d) * inv) << 16)
                            | ((u32)(cg * cov + GET_G(d) * inv) << 8)
                            |  (u32)(cb * cov + GET_B(d) * inv);
                }
            }
        }
        pen += g->advance;
    }
    return pen;
}

/* Is this codepoint drawn at all? C0/C1 controls, the zero-width formatting
 * characters and the space separators never put ink on a surface, so counting
 * them as "glyphs the faces are missing" would make every text file with a
 * newline look broken. A combining mark IS drawn (it sits on the cell before
 * it), so a missing accent is reported - it really is missing. */
static int cp_is_drawn(int cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;   /* controls      */
    if (cp == 0x20 || cp == 0xA0 || cp == 0x1680) return 0; /* spaces        */
    if (cp >= 0x2000 && cp <= 0x200A) return 0;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F) return 0;
    if (cp == 0x3000) return 0;                             /* ideographic space */
    if (cp >= 0x200B && cp <= 0x200F) return 0;             /* ZWSP..RLM     */
    if (cp >= 0x202A && cp <= 0x202E) return 0;             /* bidi controls */
    if (cp >= 0x2060 && cp <= 0x2064) return 0;
    if (cp >= 0x2066 && cp <= 0x206F) return 0;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return 0;             /* var selectors */
    if (cp == 0xFEFF) return 0;                             /* BOM/ZWNBSP    */
    return 1;
}

/* ------------------------------------------------------------
 *  Coverage: how much of a string the installed faces can draw
 *
 *  Counted from the same face list and the same stbtt_FindGlyphIndex()
 *  answer glyph_get() uses, so the number a caller prints is the number
 *  of codepoints that really came from another face - not an estimate
 *  from the string's length. "missing" counts the codepoints no installed
 *  face has at all: those are the ones that end up as .notdef boxes.
 *  Controls and space separators are skipped: they are never glyphs.
 * ------------------------------------------------------------ */
void font_coverage(Font *f, const char *text, int *fallback, int *missing) {
    if (fallback) *fallback = 0;
    if (missing) *missing = 0;
    if (!f || !text) return;
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + strlen(text);
    while (p < end && *p) {
        int cp = next_cp_n(&p, end);
        if (!cp_is_drawn(cp)) continue;
        if (face_has_cp(f->source, cp)) continue;
        int found = 0;
        for (int i = 0; i < face_count; i++)
            if (i != f->source && face_has_cp(i, cp)) { found = 1; break; }
        if (found) { if (fallback) (*fallback)++; }
        else       { if (missing)  (*missing)++; }
    }
}

/* ------------------------------------------------------------
 *  Runtime family switching - the settings app's interface
 *
 *  These are not in mystic.h (that header belongs to the rest
 *  of the desktop), so a caller declares them itself; the exact
 *  declarations are in docs/FONTS.md. All of them are safe to
 *  call after font_init(); none of them needs a restart.
 * ------------------------------------------------------------ */
int font_family_count(void) {
    int n = 0;
    for (int i = 0; i < face_count; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (ci_equal(faces[j].family, faces[i].family)) { dup = 1; break; }
        if (!dup) n++;
    }
    return n;
}

const char *font_family_name(int index) {
    if (index < 0) return NULL;
    int n = 0;
    for (int i = 0; i < face_count; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (ci_equal(faces[j].family, faces[i].family)) { dup = 1; break; }
        if (dup) continue;
        if (n++ == index) return faces[i].family;
    }
    return NULL;
}

int font_face_count(void) {
    return face_count;
}

int font_role_family(int role, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (role < 0 || role >= FONT_COUNT || !role_family[role][0]) return -1;
    snprintf(out, cap, "%s", role_family[role]);
    return 0;
}

int font_set_ui_family(const char *family) {
    if (!font_ready || !family || !*family) return -1;
    int idx = resolve_spec(family, 400, 0);
    if (idx < 0) {
        fprintf(stderr, "font: no installed face matches \"%s\"; keeping %s\n",
                family, role_family[FONT_REGULAR][0] ? role_family[FONT_REGULAR] : "the default");
        return -1;
    }
    snprintf(conf_ui, sizeof(conf_ui), "%s", faces[idx].family);
    build_roles();
    return 0;
}

int font_set_mono_family(const char *family) {
    if (!font_ready || !family || !*family) return -1;
    int idx = resolve_spec(family, 400, 0);
    if (idx < 0) {
        fprintf(stderr, "font: no installed face matches \"%s\"; keeping %s\n",
                family, role_family[FONT_MONO][0] ? role_family[FONT_MONO] : "the default");
        return -1;
    }
    if (!faces[idx].mono)
        fprintf(stderr, "font: \"%s\" is not monospaced; the terminal will use it anyway\n",
                faces[idx].family);
    snprintf(conf_mono, sizeof(conf_mono), "%s", faces[idx].family);
    build_roles();
    return 0;
}

int font_set_role_family(int role, const char *family) {
    if (role == FONT_MONO) return font_set_mono_family(family);
    if (role == FONT_REGULAR || role == FONT_MEDIUM || role == FONT_SEMIBOLD)
        return font_set_ui_family(family);
    return -1;
}

/* Write the current mapping back as a fonts.conf the loader can read. */
int font_save_config(const char *path) {
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "font: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "# Mystic OS fonts - see docs/FONTS.md\n");
    fprintf(f, "# role = family (a family name from the font, or its file name)\n");
    if (role_family[FONT_REGULAR][0]) fprintf(f, "ui   = %s\n", role_family[FONT_REGULAR]);
    if (role_family[FONT_MONO][0]) fprintf(f, "mono = %s\n", role_family[FONT_MONO]);
    if (fclose(f) != 0) {
        fprintf(stderr, "font: could not finish writing %s\n", path);
        return -1;
    }
    return 0;
}
