/* ============================================================
 *  Mystic OS - apps/calc.c
 *  "Calculator": a complete desk calculator with a real button grid.
 *
 *  WHAT IT IS
 *  ----------
 *  An immediate-execution calculator, the way a desk calculator works:
 *  one accumulator, one pending binary operator, and a display that shows
 *  either the number being typed or a result. Every button on the screen
 *  has a keyboard key and both call the same calc_press(), so the mouse
 *  and the keyboard cannot drift apart:
 *
 *      MC  MR  M+  M-  C          k   m   a   d   c
 *      %   CE  <-  1/x x^2        %   e   Bk  r   s
 *      7   8   9   /   sqrt       7   8   9   /   q
 *      4   5   6   *   +/-        4   5   6   *   n
 *      1   2   3   -   =          1   2   3   -   = or Enter
 *      0 (double wide)  .  +      0   .   +
 *
 *  Bk is Backspace. 'x'/'X' is also multiply, 'k' MC, 'm' MR, 'a' or
 *  Shift+M M+, 'd' M-, 's' x^2, 'q' sqrt, 'r' 1/x, 'n' +/-. Escape is
 *  CE when the byte reaches the app; main.c currently closes the focused
 *  window on Esc before any app is offered it, so 'e' is the reliable CE.
 *
 *  NUMBERS AND ERRORS
 *  ------------------
 *  All arithmetic is double. The entry is a bounded text buffer (23
 *  characters) so the digits actually typed, including a trailing '.',
 *  are what the value line shows; strtod() produces the double for each
 *  operation. Division by zero, 1/0, the square root of a negative value
 *  and any non-finite result set the error state: the value line says
 *  "Error", the line above names the cause, and any key clears it. A
 *  digit starts a new number afterwards, any other key resets to 0.
 *
 *  '%' with a pending operator is percent of the accumulator: 200 + 10 %
 *  puts 20 in the entry, so = gives 220. With no operator pending it is
 *  a plain value/100.
 *
 *  LAYOUT
 *  ------
 *  No pixel constant decides the geometry. The display band and the 5x6
 *  grid are shares of the content rectangle, a cell is what is left over,
 *  and the three font sizes (status, value, button) are the largest
 *  nominal point sizes whose real font_height() fits those bands. That is
 *  what keeps the app correct under the global text scale: at 1.6 the same
 *  arithmetic asks for smaller nominal sizes and nothing overflows.
 *  layout_calc() is the only place that knows a rectangle; the painter
 *  draws the array it recorded and the click handler hit-tests that same
 *  array, so a button cannot be painted anywhere its click does not land.
 *  When the window is too short for both lines of the display and readable
 *  buttons, the status line is dropped first, then the labels stop being
 *  drawn rather than being printed over each other - the grid survives
 *  because it is the whole interface.
 * ============================================================ */
#include "mystic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------
 *  State
 * ------------------------------------------------------------ */
#define ENTRY_MAX  24    /* sign + digits + '.' + NUL; 23 visible chars */
#define WHY_MAX    64
#define TAPE_MAX   96
#define TEXT_MAX   128

/* Button ids. The ten digits are contiguous and in order, which is what
 * lets the key handler map '7' with B_0 + ('7' - '0'). */
enum {
    B_0 = 0, B_1, B_2, B_3, B_4, B_5, B_6, B_7, B_8, B_9,
    B_DOT, B_ADD, B_SUB, B_MUL, B_DIV, B_EQ,
    B_C, B_CE, B_BACK, B_NEG, B_PCT, B_RECIP, B_SQ, B_SQRT,
    B_MC, B_MR, B_MPLUS, B_MMINUS,
    B_COUNT
};

enum { OP_NONE = 0, OP_ADD, OP_SUB, OP_MUL, OP_DIV };

typedef struct {
    double value;          /* the number the value line stands for       */
    double acc;            /* left operand of the pending operation      */
    double memory;         /* MC/MR/M+/M- storage                       */
    int    mem_set;        /* memory holds a value (draws the M badge)   */
    int    pending;        /* OP_* - the operation waiting for '='       */
    int    fresh;          /* the next digit starts a new entry          */
    int    entered;        /* a number was typed since the pending op    */
    int    error;          /* value line shows "Error" until a key       */
    char   err[WHY_MAX];   /* what went wrong, shown on the status line  */
    char   entry[ENTRY_MAX]; /* the digits as typed / the last result    */
    char   tape[TAPE_MAX]; /* "a op b = r" of the last evaluation        */
    int    have_tape;
    int    w, h;           /* the content rectangle, for hit testing     */
} Calc;

/* The label is UTF-8; the faces Mystic ships (Lato, JetBrains Mono) have
 * the multiplication, division, minus, arrow and superscript glyphs. */
enum { S_DIGIT = 0, S_FN, S_MEM, S_OP, S_EQ };

typedef struct {
    int         id;
    int         row, col;    /* top-left cell of the button, 0-based */
    int         rows, cols;  /* how many cells it spans              */
    int         style;       /* S_* - which theme colours it uses    */
    const char *label;
} BtnDef;

/* 5 columns x 6 rows = 30 cells, filled exactly by 28 buttons.
 * 0 spans two columns; '=' spans the last two rows. This is the classic
 * desk layout: memory across the top, digits 3x3 in the middle, a column
 * of operators on the right, and a tall '=' where the thumb expects it. */
static const BtnDef BTNS[] = {
    { B_MC,     0, 0, 1, 1, S_MEM,   "MC"          },
    { B_MR,     0, 1, 1, 1, S_MEM,   "MR"          },
    { B_MPLUS,  0, 2, 1, 1, S_MEM,   "M+"          },
    { B_MMINUS, 0, 3, 1, 1, S_MEM,   "M-"          },
    { B_C,      0, 4, 1, 1, S_FN,    "C"           },
    { B_PCT,    1, 0, 1, 1, S_FN,    "%"           },
    { B_CE,     1, 1, 1, 1, S_FN,    "CE"          },
    { B_BACK,   1, 2, 1, 1, S_FN,    "\xE2\x86\x90" },   /* U+2190 */
    { B_RECIP,  1, 3, 1, 1, S_FN,    "1/x"         },
    { B_SQ,     1, 4, 1, 1, S_FN,    "x\xC2\xB2"   },   /* x U+00B2 */
    { B_7,      2, 0, 1, 1, S_DIGIT, "7"           },
    { B_8,      2, 1, 1, 1, S_DIGIT, "8"           },
    { B_9,      2, 2, 1, 1, S_DIGIT, "9"           },
    { B_DIV,    2, 3, 1, 1, S_OP,    "\xC3\xB7"    },   /* U+00F7 */
    { B_SQRT,   2, 4, 1, 1, S_FN,    "\xE2\x88\x9A" },  /* U+221A */
    { B_4,      3, 0, 1, 1, S_DIGIT, "4"           },
    { B_5,      3, 1, 1, 1, S_DIGIT, "5"           },
    { B_6,      3, 2, 1, 1, S_DIGIT, "6"           },
    { B_MUL,    3, 3, 1, 1, S_OP,    "\xC3\x97"    },   /* U+00D7 */
    { B_NEG,    3, 4, 1, 1, S_FN,    "\xC2\xB1"    },   /* U+00B1 */
    { B_1,      4, 0, 1, 1, S_DIGIT, "1"           },
    { B_2,      4, 1, 1, 1, S_DIGIT, "2"           },
    { B_3,      4, 2, 1, 1, S_DIGIT, "3"           },
    { B_SUB,    4, 3, 1, 1, S_OP,    "\xE2\x88\x92" },  /* U+2212 */
    { B_EQ,     4, 4, 2, 1, S_EQ,    "="           },
    { B_0,      5, 0, 1, 2, S_DIGIT, "0"           },
    { B_DOT,    5, 2, 1, 1, S_DIGIT, "."           },
    { B_ADD,    5, 3, 1, 1, S_OP,    "+"           },
};
#define NBTN ((int)(sizeof BTNS / sizeof BTNS[0]))
#define CALC_COLS 5
#define CALC_ROWS 6

/* One painted button, in coordinates local to the app rectangle. Built by
 * layout_calc(), drawn by calc_draw() and hit-tested by calc_click(). */
typedef struct {
    float x, y, w, h;
    int   id, style;
    const char *label;
} BtnRect;

typedef struct {
    float pad, gap;
    int   show_status;

    float disp_x, disp_y, disp_w, disp_h, disp_radius;

    int   status_size;
    float status_x, status_y, status_h;

    int   value_size;
    float value_x, value_y, value_w, value_h;

    float grid_x, grid_y, cell_w, cell_h, btn_radius;
    int   btn_size;

    BtnRect btn[NBTN];
    int     nbtn;
} Layout;

/* ------------------------------------------------------------
 *  Formatting
 * ------------------------------------------------------------ */

/* A double as the display wants it: whole numbers without a decimal
 * point, ordinary magnitudes with up to 12 significant digits, and only
 * genuinely huge or tiny values in exponent form. Entries above 1e15 can
 * lose their low digits to the double - that is real double precision,
 * not a display limit. */
static void fmt_number(double v, char *out, size_t n) {
    if (!out || !n) return;

    if (v == 0.0) {                 /* also -0.0 */
        snprintf(out, n, "0");
        return;
    }

    double ip = 0.0;
    double a = fabs(v);
    if (a < 1e15 && modf(v, &ip) == 0.0) snprintf(out, n, "%.0f", v);
    else if (a >= 1e15 || a < 1e-9)     snprintf(out, n, "%.8g", v);
    else                                snprintf(out, n, "%.12g", v);
}

/* Same glyphs as the buttons, so the status line reads like the key you
 * would have pressed. */
static const char *op_symbol(int op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "\xE2\x88\x92";   /* U+2212 */
        case OP_MUL: return "\xC3\x97";       /* U+00D7 */
        case OP_DIV: return "\xC3\xB7";       /* U+00F7 */
        default:     return "";
    }
}

/* Store a computed value: the double and the text on the value line stay
 * in step because this is the only place a result is written. */
static void entry_store(Calc *c, double v) {
    c->value = v;
    fmt_number(v, c->entry, sizeof c->entry);
}

/* Back to a blank calculation. Memory and the last tape are deliberately
 * separate: C clears the calculation (MC is the memory's own clear) and
 * the tape is erased only when the error state is cleared or C is hit. */
static void reset_calc(Calc *c) {
    c->value   = 0.0;
    c->acc     = 0.0;
    c->pending = OP_NONE;
    c->fresh   = 0;
    c->entered = 0;
    c->error   = 0;
    c->err[0]  = 0;
    c->tape[0] = 0;
    c->have_tape = 0;
    snprintf(c->entry, sizeof c->entry, "0");
}

static void set_error(Calc *c, const char *why) {
    c->error = 1;
    snprintf(c->err, sizeof c->err, "%s", why ? why : "error");
}

/* The entry is only ever plain decimal text: no exponent, at most one
 * dot, an optional leading sign. Backspace on a formatted result can
 * leave a shorter number but never a malformed one. */
static int entry_plain(const char *s) {
    if (!s || !s[0]) return 0;
    for (const char *p = s; *p; p++)
        if (*p == 'e' || *p == 'E') return 0;
    return 1;
}

/* ------------------------------------------------------------
 *  The operations - every one is real double arithmetic
 * ------------------------------------------------------------ */

static void press_digit(Calc *c, char d) {
    if (c->fresh || !entry_plain(c->entry)) {
        snprintf(c->entry, sizeof c->entry, "0");
        c->fresh = 0;
    }

    size_t len = strlen(c->entry);
    if (!strcmp(c->entry, "0")) {
        c->entry[0] = d;
        c->entry[1] = 0;
    } else if (!strcmp(c->entry, "-0")) {
        c->entry[1] = d;
        c->entry[2] = 0;
    } else if (len + 1 < sizeof c->entry) {
        c->entry[len]     = d;
        c->entry[len + 1] = 0;
    }
    /* else the entry is full: the extra digit is dropped, the number that
     * is on screen stays the number the calculator will use */

    c->value   = strtod(c->entry, NULL);
    c->entered = 1;
}

static void press_dot(Calc *c) {
    if (c->fresh || !entry_plain(c->entry)) {
        snprintf(c->entry, sizeof c->entry, "0");
        c->fresh = 0;
    }
    size_t len = strlen(c->entry);
    if (!strchr(c->entry, '.') && len + 1 < sizeof c->entry) {
        c->entry[len]     = '.';
        c->entry[len + 1] = 0;
    }
    c->value   = strtod(c->entry, NULL);
    c->entered = 1;
}

/* Delete one byte of the number being shown. On a result this edits the
 * displayed digits, which is what people expect; an exponent-form result
 * (1.5e+20) cannot be edited digit by digit, so it clears instead of
 * turning into something strtod would read differently. */
static void press_back(Calc *c) {
    if (!entry_plain(c->entry)) {
        snprintf(c->entry, sizeof c->entry, "0");
        c->value   = 0.0;
        c->fresh   = 0;
        c->entered = 1;
        return;
    }

    size_t len = strlen(c->entry);
    if (len > 1) {
        len--;
        if (len == 1 && c->entry[0] == '-') {
            snprintf(c->entry, sizeof c->entry, "0");
        } else {
            c->entry[len] = 0;
        }
    } else {
        snprintf(c->entry, sizeof c->entry, "0");
    }

    c->value   = strtod(c->entry, NULL);
    c->fresh   = 0;
    c->entered = 1;
}

/* +/- flips the sign of the entry without going through fmt_number, so
 * "12." stays "12." and "-0" is representable while typing. */
static void press_neg(Calc *c) {
    size_t len = strlen(c->entry);
    if (c->entry[0] == '-') {
        memmove(c->entry, c->entry + 1, len);          /* includes the NUL */
        c->value = -c->value;
    } else if (len + 1 < sizeof c->entry) {
        memmove(c->entry + 1, c->entry, len + 1);
        c->entry[0] = '-';
        c->value    = -c->value;
    } else {
        return;                                        /* no room for a sign */
    }
    c->entered = 1;
}

/* CE: clear the entry, keep the accumulator and the pending operator, so
 * "5 + 3 CE 2 =" is 5 + 2. */
static void press_ce(Calc *c) {
    c->value   = 0.0;
    c->fresh   = 0;
    c->entered = 0;
    snprintf(c->entry, sizeof c->entry, "0");
}

/* % of the accumulator: with an operation pending the entry becomes
 * acc * entry / 100 (200 + 10 % -> 20, then = -> 220); with nothing
 * pending it is entry / 100. */
static void press_pct(Calc *c) {
    double v = c->value;
    if (c->pending != OP_NONE) v = c->acc * v / 100.0;
    else                       v = v / 100.0;

    if (!isfinite(v)) { set_error(c, "the percentage is out of range"); return; }
    entry_store(c, v);
    c->fresh   = 1;
    c->entered = 1;
}

static void press_recip(Calc *c) {
    if (c->value == 0.0) { set_error(c, "division by zero (1/x)"); return; }
    double v = 1.0 / c->value;
    if (!isfinite(v)) { set_error(c, "1/x is out of range"); return; }
    entry_store(c, v);
    c->fresh   = 1;
    c->entered = 1;
}

static void press_sq(Calc *c) {
    double v = c->value * c->value;
    if (!isfinite(v)) { set_error(c, "the square is out of range"); return; }
    entry_store(c, v);
    c->fresh   = 1;
    c->entered = 1;
}

static void press_sqrt(Calc *c) {
    if (c->value < 0.0) { set_error(c, "square root of a negative number"); return; }
    entry_store(c, sqrt(c->value));
    c->fresh   = 1;
    c->entered = 1;
}

static void press_mem(Calc *c, int id) {
    switch (id) {
        case B_MC:
            c->memory  = 0.0;
            c->mem_set = 0;
            break;
        case B_MR:
            entry_store(c, c->memory);
            c->fresh   = 1;
            c->entered = 1;
            break;
        case B_MPLUS:
        case B_MMINUS: {
            double m = (id == B_MPLUS) ? c->memory + c->value
                                       : c->memory - c->value;
            if (!isfinite(m)) { set_error(c, "memory is out of range"); return; }
            c->memory  = m;
            c->mem_set = 1;
            break;
        }
        default:
            break;
    }
}

/* One place applies a pending binary operation, so '=' and a new
 * operator cannot behave differently. Returns 0 when it set the error. */
static int apply_op(Calc *c, double a, double b, int op, double *out) {
    double r = 0.0;

    switch (op) {
        case OP_ADD: r = a + b; break;
        case OP_SUB: r = a - b; break;
        case OP_MUL: r = a * b; break;
        case OP_DIV:
            if (b == 0.0) { set_error(c, "division by zero"); return 0; }
            r = a / b;
            break;
        default:
            return 0;
    }

    if (!isfinite(r)) { set_error(c, "the result is out of range"); return 0; }
    *out = r;
    return 1;
}

static void press_op(Calc *c, int op) {
    if (c->pending != OP_NONE && c->entered) {
        double r = 0.0;
        if (!apply_op(c, c->acc, c->value, c->pending, &r)) return;
        c->acc = r;
        entry_store(c, r);
    } else if (c->pending == OP_NONE) {
        c->acc = c->value;
    }
    /* else an operator was pressed twice in a row: the new one replaces
     * the old one and the accumulator is unchanged */

    c->pending = op;
    c->fresh   = 1;
    c->entered = 0;
}

static void press_eq(Calc *c) {
    if (c->pending == OP_NONE) { c->fresh = 1; return; }

    int    op = c->pending;
    double a  = c->acc;
    double b  = c->value;
    double r  = 0.0;

    if (!apply_op(c, a, b, op, &r)) return;

    char sa[ENTRY_MAX], sb[ENTRY_MAX], sr[ENTRY_MAX];
    fmt_number(a, sa, sizeof sa);
    fmt_number(b, sb, sizeof sb);
    fmt_number(r, sr, sizeof sr);
    snprintf(c->tape, sizeof c->tape, "%s %s %s = %s",
             sa, op_symbol(op), sb, sr);
    c->have_tape = 1;

    entry_store(c, r);
    c->acc     = r;
    c->pending = OP_NONE;
    c->fresh   = 1;
    c->entered = 0;
}

/* Every button and every key ends up here. Nothing else calls the press_*
 * helpers, which is what keeps the two input paths honest mirrors. */
static void calc_press(Calc *c, int id) {
    if (!c) return;

    if (c->error) {
        reset_calc(c);
        if (id >= B_0 && id <= B_9)      press_digit(c, (char)('0' + (id - B_0)));
        else if (id == B_DOT)            press_dot(c);
        return;                          /* the error was the whole meaning */
    }

    switch (id) {
        case B_MC: case B_MR: case B_MPLUS: case B_MMINUS:
            press_mem(c, id); break;
        case B_C:    reset_calc(c); break;   /* memory survives C */
        case B_CE:   press_ce(c); break;
        case B_BACK: press_back(c); break;
        case B_NEG:  press_neg(c); break;
        case B_PCT:  press_pct(c); break;
        case B_RECIP:press_recip(c); break;
        case B_SQ:   press_sq(c); break;
        case B_SQRT: press_sqrt(c); break;
        case B_DOT:  press_dot(c); break;
        case B_ADD:  press_op(c, OP_ADD); break;
        case B_SUB:  press_op(c, OP_SUB); break;
        case B_MUL:  press_op(c, OP_MUL); break;
        case B_DIV:  press_op(c, OP_DIV); break;
        case B_EQ:   press_eq(c); break;
        default:
            if (id >= B_0 && id <= B_9)
                press_digit(c, (char)('0' + (id - B_0)));
            break;
    }
}

/* ------------------------------------------------------------
 *  Layout: the one place that turns a rectangle into geometry
 * ------------------------------------------------------------ */

static float widest_single_label(Font *f) {
    float widest = 0.0f;
    if (!f) return 0.0f;
    for (int i = 0; i < NBTN; i++) {
        if (BTNS[i].cols != 1) continue;   /* only one-cell labels are tight */
        float w = font_width(f, BTNS[i].label);
        if (w > widest) widest = w;
    }
    return widest;
}

static float cell_height_for(float inner_h, float disp_h, float gap) {
    float grid_h = inner_h - disp_h - gap;
    return (grid_h - (float)(CALC_ROWS - 1) * gap) / (float)CALC_ROWS;
}

static int layout_calc(int w, int h, Layout *L) {
    memset(L, 0, sizeof *L);

    float scale = ui_scale_get();
    if (!(scale > 0.1f)) scale = 1.0f;

    Font *fs  = font_get(FONT_MEDIUM,   10);
    Font *fvm = font_get(FONT_SEMIBOLD, 12);   /* the smallest value line */
    Font *fsm = font_get(FONT_MEDIUM,    8);   /* the smallest button     */
    if (!fs || !fvm || !fsm) return 0;

    /* Padding shrinks with the window as well as with the text scale, so
     * a small window spends its pixels on buttons and not on margins. */
    float pad = (float)ui_px(10);
    float lim_w = (float)w * 0.045f;
    float lim_h = (float)h * 0.035f;
    if (pad > lim_w) pad = lim_w;
    if (pad > lim_h) pad = lim_h;
    if (pad < 2.0f)  pad = 2.0f;

    float gap = (float)ui_px(4);
    if (gap > pad * 0.55f) gap = pad * 0.55f;
    if (gap < 1.0f) gap = 1.0f;

    float g2 = (float)ui_px(2);
    if (g2 > gap) g2 = gap;
    if (g2 < 1.0f) g2 = 1.0f;

    float inner_w = (float)w - 2.0f * pad;
    float inner_h = (float)h - 2.0f * pad;
    if (inner_w <= 0.0f || inner_h <= 0.0f) return 0;

    L->pad = pad;
    L->gap = gap;

    /* ---- vertical split: display band over the button grid ---- */
    float status_h = font_height(fs);
    float min_disp = 2.0f * g2 + status_h + g2 + font_height(fvm);
    float disp_h   = inner_h * 0.26f;
    float max_disp = inner_h * 0.44f;
    if (disp_h < min_disp) disp_h = min_disp;
    if (disp_h > max_disp) disp_h = max_disp;

    int   show_status = 1;
    float cell_h = cell_height_for(inner_h, disp_h, gap);
    Font *fmin   = font_get(FONT_MEDIUM, 8);
    float min_cell = fmin ? font_height(fmin) + 1.0f : 8.0f;

    if (cell_h < min_cell) {
        /* Not enough height for both display lines and printable rows:
         * the status line is the least important thing in the window. */
        float disp2 = font_height(fvm) + 2.0f * g2;
        float cell2 = cell_height_for(inner_h, disp2, gap);
        if (cell2 > cell_h) {
            show_status = 0;
            disp_h      = disp2;
            cell_h      = cell2;
        }
    }
    if (cell_h < 4.0f) return 0;

    float cell_w = (inner_w - (float)(CALC_COLS - 1) * gap) / (float)CALC_COLS;
    if (cell_w < 8.0f) return 0;

    /* ---- the fonts are fitted to the bands they have to live in ---- */
    int status_size = 10;
    int value_size  = (int)((disp_h - 2.0f * g2 - (float)ui_px(2) -
                             (show_status ? status_h + g2 : 0.0f)) / (1.4f * scale));
    if (value_size > 72) value_size = 72;
    if (value_size < 12) value_size = 12;

    int btn_size = (int)((cell_h - (float)ui_px(6)) / (1.4f * scale));
    if (btn_size > 34) btn_size = 34;
    if (btn_size < 7)  btn_size = 7;

    float label_room = cell_w - (float)ui_px(2);
    for (int tries = 0; tries < 18 && btn_size > 7; tries++) {
        Font *fb = font_get(FONT_MEDIUM, (float)btn_size);
        if (!fb) break;
        if (font_height(fb) <= cell_h - 0.5f &&
            widest_single_label(fb) <= label_room)
            break;
        btn_size--;
    }

    /* ---- the rectangles, recorded once for the painter and the hit test ---- */
    L->show_status  = show_status;
    L->disp_x       = pad;
    L->disp_y       = pad;
    L->disp_w       = inner_w;
    L->disp_h       = disp_h;
    L->disp_radius  = inner_w * 0.08f;
    if (L->disp_radius > disp_h * 0.40f) L->disp_radius = disp_h * 0.40f;
    if (L->disp_radius < 2.0f) L->disp_radius = 2.0f;

    L->status_size = status_size;
    L->status_x    = pad + g2;
    L->status_y    = pad + g2;
    L->status_h    = status_h;

    L->value_size = value_size;
    L->value_x    = pad + g2;
    L->value_w    = inner_w - 2.0f * g2;
    L->value_y    = pad + 2.0f * g2 + (show_status ? status_h + g2 : 0.0f);
    L->value_h    = disp_h - 2.0f * g2 - (show_status ? status_h + g2 : 0.0f);

    L->grid_x = pad;
    L->grid_y = pad + disp_h + gap;
    L->cell_w = cell_w;
    L->cell_h = cell_h;

    float radius = (cell_w < cell_h ? cell_w : cell_h) * 0.20f;
    if (radius > cell_h * 0.45f) radius = cell_h * 0.45f;
    if (radius < 1.0f) radius = 1.0f;
    L->btn_radius = radius;
    L->btn_size   = btn_size;

    for (int i = 0; i < NBTN; i++) {
        const BtnDef *d = &BTNS[i];
        BtnRect *b = &L->btn[L->nbtn++];
        b->x = L->grid_x + (float)d->col * (cell_w + gap);
        b->y = L->grid_y + (float)d->row * (cell_h + gap);
        b->w = (float)d->cols * cell_w + (float)(d->cols - 1) * gap;
        b->h = (float)d->rows * cell_h + (float)(d->rows - 1) * gap;
        b->id    = d->id;
        b->style = d->style;
        b->label = d->label;
    }
    return 1;
}

/* ------------------------------------------------------------
 *  Drawing
 * ------------------------------------------------------------ */

/* The longest prefix of `text` that fits in maxw, cut on a UTF-8 boundary
 * and marked with "..". Used for the status line and for a value that is
 * still too wide at the smallest value font. */
static void fit_text(Font *f, const char *text, float maxw,
                     char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = 0;
    if (!f || !text || !text[0] || maxw <= 0.0f) return;

    size_t n = strlen(text);
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, text, n);
    out[n] = 0;
    if (font_width(f, out) <= maxw) return;

    float dots = font_width(f, "..");
    size_t cut = n;
    while (cut > 0) {
        cut--;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0u) == 0x80u) cut--;
        out[cut] = 0;
        if (font_width(f, out) + dots <= maxw) break;
    }
    if (cut == 0) { out[0] = 0; return; }
    if (cut + 3 <= outsz) {
        out[cut]     = '.';
        out[cut + 1] = '.';
        out[cut + 2] = 0;
    }
}

/* What the status line says: the cause of an error, the expression being
 * built, or the last completed calculation. All three are the
 * calculator's real state, never decoration. */
static void build_status(const Calc *c, char *buf, size_t n) {
    if (!buf || !n) return;
    buf[0] = 0;

    if (c->error) { snprintf(buf, n, "%s", c->err); return; }

    if (c->pending != OP_NONE) {
        char a[ENTRY_MAX];
        fmt_number(c->acc, a, sizeof a);
        if (c->entered)
            snprintf(buf, n, "%s %s %s", a, op_symbol(c->pending), c->entry);
        else
            snprintf(buf, n, "%s %s", a, op_symbol(c->pending));
        return;
    }

    if (c->have_tape) snprintf(buf, n, "%s", c->tape);
}

static void btn_colors(const Theme *th, int style,
                       u32 *fill, u32 *border, u32 *label) {
    switch (style) {
        case S_MEM:
            *fill   = color_alpha(th->accent, 0.13f);
            *border = color_alpha(th->accent, 0.34f);
            *label  = th->accent;
            break;
        case S_FN:
            *fill   = color_alpha(th->text, 0.11f);
            *border = th->panel_border;
            *label  = th->text;
            break;
        case S_OP:
            *fill   = color_alpha(th->accent, 0.20f);
            *border = color_alpha(th->accent, 0.48f);
            *label  = th->accent;
            break;
        case S_EQ:
            *fill   = th->accent;
            *border = th->accent;
            *label  = th->bg_top;      /* opaque theme colour, contrasts in both */
            break;
        default:                       /* S_DIGIT */
            *fill   = color_alpha(th->text, 0.055f);
            *border = th->panel_border;
            *label  = th->text;
            break;
    }
}

static void calc_draw(App *a, Surface *s, int x, int y, int w, int h,
                      const Theme *th) {
    Calc *c = a->state;
    if (!c || !s || !th) return;
    c->w = w;
    c->h = h;

    Layout L;
    if (!layout_calc(w, h, &L)) return;

    Font *fst  = L.show_status ? font_get(FONT_MEDIUM, (float)L.status_size) : NULL;
    Font *fbtn = font_get(FONT_MEDIUM, (float)L.btn_size);
    if (!fbtn) return;
    if (L.show_status && !fst) return;

    float ox = (float)x;
    float oy = (float)y;

    /* ---- the display recess ----
     * A 5% text-colour wash reads as a darker well in a light theme and a
     * lighter one in a dark theme, so one expression serves both. */
    gfx_rrect_aa(s, ox + L.disp_x, oy + L.disp_y, L.disp_w, L.disp_h,
                 L.disp_radius, color_alpha(th->text, 0.05f),
                 th->panel_border, 1.0f);

    /* ---- status line ---- */
    char mem[ENTRY_MAX + 8];
    mem[0] = 0;
    if (c->mem_set) {
        char m[ENTRY_MAX];
        fmt_number(c->memory, m, sizeof m);
        snprintf(mem, sizeof mem, "M %s", m);
    }

    if (L.show_status && fst) {
        float memw = mem[0] ? font_width(fst, mem) + (float)ui_px(8) : 0.0f;
        float room = L.value_w - memw;

        char status[TEXT_MAX];
        build_status(c, status, sizeof status);
        if (status[0] && room > 2.0f) {
            char shown[TEXT_MAX];
            fit_text(fst, status, room, shown, sizeof shown);
            font_draw(s, fst, ox + L.status_x, oy + L.status_y, shown,
                      c->error ? th->accent : color_alpha(th->text, 0.55f));
        }
        if (mem[0] && font_width(fst, mem) <= L.value_w) {
            float mw = font_width(fst, mem);
            font_draw(s, fst, ox + L.value_x + L.value_w - mw,
                      oy + L.status_y, mem, color_alpha(th->accent, 0.90f));
        }
    }

    /* ---- the value, right aligned ----
     * The value font is the one layout picked for the band; if the number
     * is still wider than the window it is reduced in steps (real font
     * metrics each time) until it fits or reaches the readable floor. */
    const char *shown = c->error ? "Error" : c->entry;
    int vs = L.value_size;
    Font *fv = font_get(FONT_SEMIBOLD, (float)vs);
    if (!fv) return;
    float tw = font_width(fv, shown);
    for (int tries = 0; tries < 6 && tw > L.value_w && vs > 11; tries++) {
        int next = (int)((float)vs * L.value_w / tw);
        if (next >= vs) next = vs - 1;
        if (next < 11) next = 11;
        vs = next;
        fv = font_get(FONT_SEMIBOLD, (float)vs);
        if (!fv) return;
        tw = font_width(fv, shown);
    }
    char vtext[TEXT_MAX];
    fit_text(fv, shown, L.value_w, vtext, sizeof vtext);
    tw = font_width(fv, vtext);

    float vy = oy + L.value_y + (L.value_h - font_height(fv)) * 0.5f;
    if (vy < oy + L.value_y) vy = oy + L.value_y;
    font_draw(s, fv, ox + L.value_x + L.value_w - tw, vy, vtext,
              c->error ? th->accent : th->text);

    /* ---- the buttons ---- */
    float fbth = font_height(fbtn);
    for (int i = 0; i < L.nbtn; i++) {
        const BtnRect *b = &L.btn[i];
        u32 fill, border, label;
        btn_colors(th, b->style, &fill, &border, &label);

        float bx = ox + b->x;
        float by = oy + b->y;
        gfx_rrect_aa(s, bx, by, b->w, b->h, L.btn_radius, fill, border, 1.0f);

        float lw = font_width(fbtn, b->label);
        if (fbth <= b->h - 1.0f && lw <= b->w - 2.0f) {
            font_draw(s, fbtn, bx + (b->w - lw) * 0.5f,
                      by + (b->h - fbth) * 0.5f, b->label, label);
        }
    }
}

/* ------------------------------------------------------------
 *  Events
 * ------------------------------------------------------------ */

static void calc_init(App *a, int w, int h) {
    Calc *c = calloc(1, sizeof(Calc));
    if (!c) return;                 /* draw/key/click all test the state */
    c->w = w;
    c->h = h;
    reset_calc(c);
    a->state = c;
}

static void calc_free(App *a) {
    free(a->state);
    a->state = NULL;
}

static void calc_resize(App *a, int w, int h) {
    Calc *c = a->state;
    if (!c) return;
    c->w = w;
    c->h = h;
}

/* The bytes the desktop delivers: printable text from input.text, escape
 * runs for the navigation keys. Arrow/Home/End/PageUp report runs are not
 * used by this app, but they must be consumed whole so their final 'A' or
 * '~' is never mistaken for a key of its own. A bare ESC is CE - main.c
 * intercepts Esc today, so this is the path for a desktop that forwards
 * it later. */
static void calc_key(App *a, const char *bytes, int n) {
    Calc *c = a->state;
    if (!c || !bytes) return;

    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)bytes[i];

        if (ch == 0x1b) {
            if (i + 1 < n && (bytes[i + 1] == '[' || bytes[i + 1] == 'O')) {
                i += 2;
                while (i < n) {
                    unsigned char t = (unsigned char)bytes[i];
                    if ((t >= 'A' && t <= 'Z') || t == '~') break;
                    i++;
                }
                /* a navigation key is still a key: it clears Error */
                if (c->error) reset_calc(c);
            } else {
                calc_press(c, B_CE);
            }
            continue;
        }

        switch (ch) {
            case '\n': case '\r': case '=': calc_press(c, B_EQ);   break;
            case '\b': case 0x7f:           calc_press(c, B_BACK); break;
            case '\t': case ' ':
                /* no meaning here, but every key still clears Error */
                if (c->error) reset_calc(c);
                break;
            case '+':                       calc_press(c, B_ADD);  break;
            case '-':                       calc_press(c, B_SUB);  break;
            case '*': case 'x': case 'X':   calc_press(c, B_MUL);  break;
            case '/':                       calc_press(c, B_DIV);  break;
            case '.':                       calc_press(c, B_DOT);  break;
            case '%':                       calc_press(c, B_PCT);  break;
            case 'c': case 'C':             calc_press(c, B_C);    break;
            case 'e': case 'E':             calc_press(c, B_CE);   break;
            case 'r': case 'R':             calc_press(c, B_RECIP);break;
            case 'q': case 'Q':             calc_press(c, B_SQRT); break;
            case 's': case 'S':             calc_press(c, B_SQ);   break;
            case 'n': case 'N': case '~':   calc_press(c, B_NEG);  break;
            case 'm':                       calc_press(c, B_MR);   break;
            case 'M': case 'a': case 'A':   calc_press(c, B_MPLUS);break;
            case 'd': case 'D':             calc_press(c, B_MMINUS);break;
            case 'k': case 'K':             calc_press(c, B_MC);   break;
            default:
                if (ch >= '0' && ch <= '9') {
                    /* calc_press' error branch starts the new number with
                     * this digit, so 9/0=7 shows 7, not 0 */
                    calc_press(c, B_0 + (int)(ch - '0'));
                } else if (c->error) {
                    /* an unmapped key clears Error rather than leaving
                     * the calculator stuck */
                    reset_calc(c);
                }
                break;
        }
    }
}

static void calc_click(App *a, int x, int y, int buttons) {
    Calc *c = a->state;
    if (!c) return;
    if (buttons && !(buttons & 1)) return;      /* left button only */
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;

    Layout L;
    if (!layout_calc(c->w, c->h, &L)) return;

    for (int i = 0; i < L.nbtn; i++) {
        const BtnRect *b = &L.btn[i];
        if ((float)x < b->x || (float)x >= b->x + b->w) continue;
        if ((float)y < b->y || (float)y >= b->y + b->h) continue;
        calc_press(c, b->id);
        return;
    }
}

const AppVT app_vt_calc = {
    .id = "calc",
    .title = "Calculator",
    .init = calc_init,
    .draw = calc_draw,
    .key = calc_key,
    .click = calc_click,
    .resize = calc_resize,
    .free = calc_free,
};
