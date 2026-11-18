/* ============================================================
 *  Mystic OS - apps/calc.c
 *  "Calculator": a portrait desk calculator with a scientific mode.
 *
 *  TWO KEYPADS, ONE WINDOW
 *  -----------------------
 *  The grid is four columns wide and grows downwards, not sideways. The
 *  plain keypad is 4 x 7 cells (memory, clear/edit, digits, operators);
 *  the scientific keypad adds a 4 x 4 block of functions underneath, so
 *  the same window can be a small desk calculator or a full scientific
 *  one. Both keypads are driven by one BtnDef table per block, and
 *  layout_calc() records every rectangle once: the painter draws that
 *  array and calc_click() hit-tests that same array, so a button can
 *  never be painted where its click does not land.
 *
 *  PLAIN MODE
 *  ----------
 *  Immediate execution, exactly as before: one accumulator, one pending
 *  binary operator, and a display that shows either the number being
 *  typed or a result. The plain keypad is four columns wide:
 *
 *      MC   MR   M+   M-
 *      C    CE   <-   1/x
 *      x2   sq   %    +-
 *      7    8    9    div
 *      4    5    6    mul
 *      1    2    3    sub
 *      0    .    =    +
 *
 *  Every button has a keyboard key and both call the same calc_press():
 *  0-9 . + - * or x / % = or Enter, Backspace, r 1/x, q sq, s x2,
 *  n or ~ +/-, k MC, m MR, a M+, d M-, c C, e CE.
 *
 *  '%' with a pending operator is percent of the accumulator: 200 + 10 %
 *  puts 20 in the entry, so = gives 220. With no operator pending it is
 *  a plain value/100.
 *
 *  SCIENTIFIC MODE
 *  ---------------
 *  The "2nd" chip (key 'i') swaps the immediate-execution accumulator for
 *  a real expression line and a recursive-descent evaluator. The line is
 *  edited like text: digits, operators, parentheses and functions append,
 *  Backspace deletes one glyph, Enter/'=' evaluates, CE clears the line,
 *  C clears everything (memory survives, as in plain mode).
 *
 *  Grammar, tightest first:
 *
 *      postfix   number | constant | name(args) | ( expr )   then ! and %
 *      power     postfix  ( '^' unary | 'root' unary )        right assoc
 *      unary     '+' unary | '-' unary | power
 *      term      unary ( ('*'|'/') unary )*
 *      expr      term  ( ('+'|'-') term )*
 *
 *  '^' binds tighter than unary minus, so -2^2 is -4 and 2^-1 is 0.5.
 *  'root' is the infix y-th root: 9 root 2 is 3. Its left operand is any
 *  finished value, so the constants and the postfix keys work too: 5! root
 *  2 is sqrt(120), pi root 2 is sqrt(pi) and 50% root 2 is sqrt(0.5).
 *  Functions may be written with or without parentheses - sin(30) and
 *  sin 30 are the same call - and '=' closes any parentheses the user left
 *  open, because the tape then shows the expression that was evaluated. Every function
 *  is real double arithmetic on libm; a domain error (log of a
 *  non-positive number, even root of a negative number, factorial of a
 *  non-integer, 0 to a negative power, ...) sets Error and the line above
 *  names the cause. Nothing can reach the display as nan or inf: every
 *  result is checked with isfinite().
 *
 *  In scientific mode '%' is the postfix "divide by 100" (2^50% is
 *  2^0.5), because an infix modulo would have no sensible reading next to
 *  '^'. The plain accumulator's percent-of-accumulator rule is unchanged.
 *
 *  A value followed by a digit or a constant is a product, so the '*' is
 *  written into the line where it can be seen: pi then 2 is pi*2,
 *  (1+2) then 3 is (1+2)*3, 5! then 2 is 5!*2. A '.' that is still being
 *  typed is not a finished value, so 2. then 5 is 2.5 - and 2. then pi is
 *  the product 2.*pi, which the parser reads as 2*pi.
 *
 *  Scientific keys (all of them reachable from the keyboard):
 *
 *      i     2nd / scientific toggle        g   DEG <-> RAD
 *      b j t sin cos tan                    B J T   asin acos atan
 *      l o   ln log10                       f       exp (e^x)
 *      ^     x^y                            u       e (2.71828...)
 *      y     y-root (infix "root")          p       pi
 *      !     n!                             ( )     parentheses
 *
 *  A function button applies to what is already on the line: with "30"
 *  showing, sin makes it "sin(30)". Press the function first to build
 *  "sin(" and then type the argument. The '2nd' chip stays lit while the
 *  scientific keypad is open; the DEG/RAD chip shows the angle unit that
 *  the trig keys and the inverse trig keys use.
 *
 *  LAYOUT
 *  ------
 *  No pixel constant decides the geometry. The header band, the display
 *  band and the grid are shares of the content rectangle; a cell is what
 *  is left over; the font sizes are the largest nominal point sizes whose
 *  real font_height() fits those bands. That is what keeps the app correct
 *  under the global text scale: at 1.6 the same arithmetic asks for
 *  smaller nominal sizes and nothing overflows. The grid is centred and a
 *  button is never wider than twice its height, so the portrait shape
 *  survives even if the desktop hands the app a wide rectangle.
 *
 *  Labels are never shrunk below 9 device pixels. When the cells get too
 *  small for that, the keys are still drawn and hit-tested but their text
 *  is dropped, and any single label that does not fit its own key is
 *  dropped too - the calculator never prints text over itself.
 *
 *  VERIFYING
 *  ---------
 *  The plain keypad is the immediate-execution calculator users already
 *  have, so its behaviour is pinned by a matrix at the bottom of this file
 *  (calc_selftest, compiled only under HOST_TEST): backspace, dot, +/-,
 *  1/x, x2, sqrt, repeated operators, the entry ceiling and the
 *  q/s/n/r/k/m/a/d/e keys, every case driven through calc_key() - the same
 *  byte path the desktop uses. The .work host harness includes this file
 *  and calls it, so "exactly as it does now" is a test, not a promise.
 * ============================================================ */
#include "mystic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------
 *  Limits and state
 * ------------------------------------------------------------ */
#define ENTRY_MAX  24    /* sign + digits + '.' + NUL; 23 visible chars */
#define WHY_MAX    128
#define TAPE_MAX   176
#define TEXT_MAX   128
#define EXPR_MAX   120
#define MAX_NEST   16    /* grammar levels of nesting; see the header     */

/* The ten digits are contiguous and in order, which is what lets the key
 * handler map '7' with B_0 + ('7' - '0'). */
enum {
    B_0 = 0, B_1, B_2, B_3, B_4, B_5, B_6, B_7, B_8, B_9,
    B_DOT, B_ADD, B_SUB, B_MUL, B_DIV, B_EQ,
    B_C, B_CE, B_BACK, B_NEG, B_PCT, B_RECIP, B_SQ, B_SQRT,
    B_MC, B_MR, B_MPLUS, B_MMINUS,
    /* scientific keypad and the two mode chips */
    B_SCI, B_DEG,
    B_LP, B_RP, B_POW, B_ROOT, B_FACT,
    B_SIN, B_COS, B_TAN, B_ASIN, B_ACOS, B_ATAN,
    B_LN, B_LOG, B_EXP, B_PI, B_ECONST,
    B_COUNT
};

enum { OP_NONE = 0, OP_ADD, OP_SUB, OP_MUL, OP_DIV };

/* The constants are the mathematical values, written here so the file does
 * not depend on M_PI being visible under the build's feature macros. */
#define CALC_PI 3.14159265358979323846
#define CALC_E  2.71828182845904523536

typedef struct {
    /* ---- plain keypad: immediate execution ---- */
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

    /* ---- scientific mode ---- */
    int    sci;            /* the scientific keypad is open              */
    int    deg;            /* 1 = degrees, 0 = radians (starts in DEG)   */
    char   expr[EXPR_MAX]; /* the expression line being edited           */

    int    w, h;           /* the content rectangle, for hit testing     */
} Calc;

/* The label is UTF-8; the faces Mystic ships (Lato, JetBrains Mono) have
 * the multiplication, division, minus, arrow, root and superscript glyphs. */
enum { S_DIGIT = 0, S_FN, S_MEM, S_OP, S_EQ };

typedef struct {
    int         id;
    int         row, col;    /* top-left cell of the button, 0-based */
    int         rows, cols;  /* how many cells it spans              */
    int         style;       /* S_* - which theme colours it uses    */
    const char *label;
} BtnDef;

/* The plain keypad: 4 columns x 7 rows = 28 cells, filled exactly.
 * The digit block (7-8-9 / 4-5-6 / 1-2-3) is the familiar one and sits in
 * the middle; clear and editing across the top; the operators in the
 * right-hand column. */
static const BtnDef BASE_BTNS[] = {
    { B_MC,     0, 0, 1, 1, S_MEM,   "MC"          },
    { B_MR,     0, 1, 1, 1, S_MEM,   "MR"          },
    { B_MPLUS,  0, 2, 1, 1, S_MEM,   "M+"          },
    { B_MMINUS, 0, 3, 1, 1, S_MEM,   "M-"          },

    { B_C,      1, 0, 1, 1, S_FN,    "C"           },
    { B_CE,     1, 1, 1, 1, S_FN,    "CE"          },
    { B_BACK,   1, 2, 1, 1, S_FN,    "\xE2\x86\x90" },   /* U+2190 */
    { B_RECIP,  1, 3, 1, 1, S_FN,    "1/x"         },

    { B_SQ,     2, 0, 1, 1, S_FN,    "x\xC2\xB2"   },   /* x U+00B2 */
    { B_SQRT,   2, 1, 1, 1, S_FN,    "\xE2\x88\x9A" },  /* U+221A */
    { B_PCT,    2, 2, 1, 1, S_FN,    "%"           },
    { B_NEG,    2, 3, 1, 1, S_FN,    "\xC2\xB1"    },   /* U+00B1 */

    { B_7,      3, 0, 1, 1, S_DIGIT, "7"           },
    { B_8,      3, 1, 1, 1, S_DIGIT, "8"           },
    { B_9,      3, 2, 1, 1, S_DIGIT, "9"           },
    { B_DIV,    3, 3, 1, 1, S_OP,    "\xC3\xB7"    },   /* U+00F7 */

    { B_4,      4, 0, 1, 1, S_DIGIT, "4"           },
    { B_5,      4, 1, 1, 1, S_DIGIT, "5"           },
    { B_6,      4, 2, 1, 1, S_DIGIT, "6"           },
    { B_MUL,    4, 3, 1, 1, S_OP,    "\xC3\x97"    },   /* U+00D7 */

    { B_1,      5, 0, 1, 1, S_DIGIT, "1"           },
    { B_2,      5, 1, 1, 1, S_DIGIT, "2"           },
    { B_3,      5, 2, 1, 1, S_DIGIT, "3"           },
    { B_SUB,    5, 3, 1, 1, S_OP,    "\xE2\x88\x92" },  /* U+2212 */

    { B_0,      6, 0, 1, 1, S_DIGIT, "0"           },
    { B_DOT,    6, 1, 1, 1, S_DIGIT, "."           },
    { B_EQ,     6, 2, 1, 1, S_EQ,    "="           },
    { B_ADD,    6, 3, 1, 1, S_OP,    "+"           },
};

/* The scientific block: 4 x 4 cells appended below the plain keypad.
 * Reading order is the one people expect from a scientific calculator -
 * trig and its inverses, then logarithms, then powers, then constants and
 * parentheses. */
static const BtnDef SCI_BTNS[] = {
    { B_SIN,   0, 0, 1, 1, S_FN, "sin"        },
    { B_COS,   0, 1, 1, 1, S_FN, "cos"        },
    { B_TAN,   0, 2, 1, 1, S_FN, "tan"        },
    { B_ASIN,  0, 3, 1, 1, S_FN, "asin"       },

    { B_ACOS,  1, 0, 1, 1, S_FN, "acos"       },
    { B_ATAN,  1, 1, 1, 1, S_FN, "atan"       },
    { B_LN,    1, 2, 1, 1, S_FN, "ln"         },
    { B_LOG,   1, 3, 1, 1, S_FN, "log"        },

    { B_EXP,   2, 0, 1, 1, S_FN, "exp"        },
    { B_POW,   2, 1, 1, 1, S_OP, "x^y"        },
    { B_ROOT,  2, 2, 1, 1, S_OP, "y\xE2\x88\x9Ax" },  /* y U+221A x */
    { B_FACT,  2, 3, 1, 1, S_FN, "n!"         },

    { B_PI,    3, 0, 1, 1, S_FN, "\xCF\x80"   },      /* U+03C0 */
    { B_ECONST,3, 1, 1, 1, S_FN, "e"          },
    { B_LP,    3, 2, 1, 1, S_FN, "("          },
    { B_RP,    3, 3, 1, 1, S_FN, ")"          },
};

#define NBASE  ((int)(sizeof BASE_BTNS / sizeof BASE_BTNS[0]))
#define NSCI   ((int)(sizeof SCI_BTNS  / sizeof SCI_BTNS[0]))
#define BASE_ROWS 7
#define SCI_ROWS  4
#define GRID_COLS 4
#define NRECT  (NBASE + NSCI + 2)   /* + the 2nd and DEG/RAD chips */

/* One painted button, in coordinates local to the app rectangle. Built by
 * layout_calc(), drawn by calc_draw() and hit-tested by calc_click(). */
enum { K_GRID = 0, K_CHIP };

typedef struct {
    float x, y, w, h;
    int   id, style, kind;
    const char *label;
} BtnRect;

typedef struct {
    float pad, gap, g2;
    int   show_status;

    /* header band: mode chips and the memory badge */
    float head_y, head_h, chip_h, chip_radius;

    /* display recess */
    float disp_x, disp_y, disp_w, disp_h, disp_radius;

    int   status_size;
    float status_x, status_y, status_w, status_h;

    int   value_size;
    float value_x, value_y, value_w, value_h;

    /* button grid */
    float grid_x, grid_y, cell_w, cell_h, btn_radius;
    int   btn_size, label_ok, rows;

    BtnRect btn[NRECT];
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

/* Back to a blank calculation. Memory and the mode switches are
 * deliberately separate: C clears the calculation (MC is the memory's own
 * clear) and the tape is erased with it. */
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
    c->expr[0] = 0;
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
 *  The plain operations - every one is real double arithmetic
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

/* ------------------------------------------------------------
 *  Scientific mode: editing the expression line
 *
 *  The line is plain text and the parser reads it back, so what is on
 *  screen is exactly what is evaluated. Every edit goes through these
 *  helpers, which is what keeps the buffer bounded (EXPR_MAX) and keeps
 *  an operator from being appended twice by accident.
 * ------------------------------------------------------------ */

static int expr_cat(Calc *c, const char *s) {
    if (!s || !s[0]) return 1;
    size_t have = strlen(c->expr);
    size_t add  = strlen(s);
    if (have + add + 1 > sizeof c->expr) {
        set_error(c, "the expression is full");
        return 0;
    }
    memcpy(c->expr + have, s, add + 1);
    return 1;
}

static void expr_wrap(Calc *c, const char *pre, const char *post) {
    char buf[EXPR_MAX];
    int n = snprintf(buf, sizeof buf, "%s%s%s", pre, c->expr, post);
    if (n < 0 || (size_t)n >= sizeof buf) {
        set_error(c, "the expression is full");
        return;
    }
    memcpy(c->expr, buf, (size_t)n + 1);
}

/* Delete one UTF-8 glyph, not one byte: the label glyphs (pi, root) are
 * multi-byte and half a glyph is not text. */
static void expr_back(Calc *c) {
    size_t n = strlen(c->expr);
    if (!n) return;
    n--;
    while (n > 0 && ((unsigned char)c->expr[n] & 0xC0u) == 0x80u) n--;
    c->expr[n] = 0;
}

static int expr_last(const Calc *c) {
    size_t n = strlen(c->expr);
    return n ? (unsigned char)c->expr[n - 1] : 0;
}

/* Where does a value end?
 *
 * One test, shared by the y-root key and by the implicit multiply, so the
 * two can never disagree about what counts as a finished value: a digit,
 * a trailing '.', a ')', a postfix '!' or '%', the pi glyph, or one of the
 * constants spelled as a word ("pi", "e"). A trailing '.' is something a
 * digit must not multiply into (2. then 5 is 2.5, not 2.*5); the constants
 * may follow it (2. then pi is the product 2.*pi), which is why the digit
 * path tests expr_last() as well. */
static int expr_ends_value(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n) return 0;

    unsigned char last = (unsigned char)s[n - 1];
    if ((last >= '0' && last <= '9') || last == '.' || last == ')' ||
        last == '!' || last == '%')
        return 1;

    if (n >= 2 && (unsigned char)s[n - 2] == 0xCF && last == 0x80)
        return 1;                                  /* U+03C0, the pi key */

    /* The parser also accepts the constants as words. A letter is only a
     * value at the end of a name, never inside one. */
    size_t at = n;                                 /* start of the last name */
    while (at > 0) {
        unsigned char ch = (unsigned char)s[at - 1];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_')
            at--;
        else
            break;
    }
    const char *name = s + at;
    if (!strcmp(name, "pi") || !strcmp(name, "e")) return 1;
    return 0;
}

/* After a finished value the next digit starts a new factor, so the '*'
 * the parser needs is written into the line where it can be seen - never
 * implied silently. A digit or a dot continues the number already being
 * typed (2 then 5 is 25, 2. then 5 is 2.5), so those two tails are the
 * only ones that must not be multiplied. */
static void sci_mul_before_atom(Calc *c) {
    int last = expr_last(c);
    if (last == '.' || (last >= '0' && last <= '9')) return;
    if (expr_ends_value(c->expr)) expr_cat(c, "*");
}

/* Constants and memory recall also multiply into a finished value; unlike a
 * digit they may follow the '.' of a half-typed number. */
static void sci_mul_before_value(Calc *c) {
    if (expr_ends_value(c->expr)) expr_cat(c, "*");
}

static void sci_digit(Calc *c, char d) {
    if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
    sci_mul_before_atom(c);

    size_t n = strlen(c->expr);
    /* a lone 0 is replaced, not extended: typing 0 then 5 is 5, and
     * "2+0" then 5 is "2+5" */
    int cur_zero = (n > 0 && c->expr[n - 1] == '0' &&
                    (n == 1 || c->expr[n - 2] != '.') &&
                    (n == 1 || !(c->expr[n - 2] >= '0' && c->expr[n - 2] <= '9')));
    if (cur_zero) { c->expr[n - 1] = d; return; }

    char s[2];
    s[0] = d;
    s[1] = 0;
    expr_cat(c, s);
}

static void sci_dot(Calc *c) {
    if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
    sci_mul_before_atom(c);

    size_t n = strlen(c->expr);
    if (n == 0) { expr_cat(c, "0."); return; }

    int prev = (unsigned char)c->expr[n - 1];
    if ((prev >= '0' && prev <= '9') || prev == '.') {
        /* the current number must not get a second dot */
        for (size_t i = n; i > 0; i--) {
            int ch = (unsigned char)c->expr[i - 1];
            if (ch >= '0' && ch <= '9') continue;
            if (ch == '.') return;
            break;
        }
        expr_cat(c, ".");
        return;
    }
    if (prev == '(' || prev == ',' || prev == '+' || prev == '-' ||
        prev == '*' || prev == '/' || prev == '^')
        expr_cat(c, "0.");
}

static void sci_op(Calc *c, int op) {
    c->fresh = 0;
    size_t n = strlen(c->expr);
    if (n == 0) {
        if (op == '-') expr_cat(c, "-");    /* a leading minus is a number */
        return;
    }

    int last = (unsigned char)c->expr[n - 1];
    if (last == '(' || last == ',') {
        if (op == '-') expr_cat(c, "-");
        return;
    }
    if (last == '+' || last == '-' || last == '*' || last == '/' || last == '^') {
        if (op == '-' && last != '-') { expr_cat(c, "-"); return; }  /* 2*-3 */
        c->expr[n - 1] = (char)op;                                   /* replace */
        return;
    }
    if (last == '.') return;                 /* "2." is not finished yet */

    char s[2];
    s[0] = (char)op;
    s[1] = 0;
    expr_cat(c, s);
}

/* A function applies to the value already on the line. With the line at a
 * point where an operand starts (empty, or after an operator) it opens a
 * call instead, which is how "2 + sin(" is built. */
static void sci_function(Calc *c, const char *name) {
    c->fresh = 0;
    int last = expr_last(c);
    int at_start = (last == 0 || last == '(' || last == ',' || last == '+' ||
                    last == '-' || last == '*' || last == '/' || last == '^');
    char pre[24];
    snprintf(pre, sizeof pre, "%s(", name);
    if (at_start) expr_cat(c, pre);
    else          expr_wrap(c, pre, ")");
}

/* -(...) wraps the whole line so the sign applies to the value, not to
 * the first term; pressing it again unwraps, so +/- is a real toggle. */
static int expr_neg_wrapped(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || s[0] != '-' || s[1] != '(' || s[n - 1] != ')') return 0;
    int depth = 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0 && i != n - 1) return 0;
        }
    }
    return depth == 0;
}

static void sci_neg(Calc *c) {
    c->fresh = 0;
    if (!c->expr[0]) { expr_cat(c, "-"); return; }
    if (expr_neg_wrapped(c->expr)) {
        size_t n = strlen(c->expr);
        memmove(c->expr, c->expr + 2, n - 3);
        c->expr[n - 3] = 0;
        return;
    }
    expr_wrap(c, "-(", ")");
}

/* 1/x, x2 and sqrt apply to what is on the line, so their effect stays
 * visible in the expression instead of replacing it invisibly. */
static void sci_wrap_key(Calc *c, const char *pre, const char *post,
                         const char *when_empty) {
    c->fresh = 0;
    if (!c->expr[0]) {
        if (when_empty) expr_cat(c, when_empty);
        return;
    }
    expr_wrap(c, pre, post);
}

/* ------------------------------------------------------------
 *  Scientific mode: the recursive-descent evaluator
 * ------------------------------------------------------------ */

enum {
    P_OK = 0, P_EMPTY, P_CHAR, P_NUM, P_RPAREN, P_LPAREN, P_TRAILING,
    P_OPERATOR, P_IDENT, P_DOMAIN, P_RANGE, P_DEPTH, P_COMMA, P_EXTRA
};

typedef enum { TK_END, TK_NUM, TK_IDENT, TK_OP, TK_LP, TK_RP, TK_COMMA } TokKind;

typedef struct {
    TokKind kind;
    double  num;
    char    op;
    char    name[12];
} Token;

typedef struct {
    const char *p;          /* cursor into the text being parsed */
    Token       tok;        /* the token currently under the parser */
    int         code;       /* P_* once something has gone wrong */
    char        detail[64]; /* offending text, for the message */
    int         deg;        /* angle unit for the trig functions */
} Parser;

static int is_digit_ch(unsigned char c) { return c >= '0' && c <= '9'; }
static int is_alpha_ch(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void p_fail(Parser *P, int code, const char *detail) {
    if (P->code) return;
    P->code = code;
    snprintf(P->detail, sizeof P->detail, "%s", detail ? detail : "");
}

static void lex_next(Parser *P) {
    const char *s = P->p;
    while (*s == ' ' || *s == '\t') s++;

    P->tok.kind = TK_END;
    P->tok.num  = 0.0;
    P->tok.op   = 0;
    P->tok.name[0] = 0;

    if (!*s) { P->p = s; return; }

    unsigned char c = (unsigned char)s[0];

        /* The pi glyph is the constant; the word "pi" is accepted too. */
    if (c == 0xCF && (unsigned char)s[1] == 0x80) {          /* U+03C0 */
        P->tok.kind = TK_IDENT;
        snprintf(P->tok.name, sizeof P->tok.name, "pi");
        P->p = s + 2;
        return;
    }

    /* The pretty glyphs the buttons use are operators too. */
    if (c == 0xC3 && (unsigned char)s[1] == 0x97) {          /* U+00D7 */
        P->tok.kind = TK_OP; P->tok.op = '*'; P->p = s + 2; return;
    }
    if (c == 0xC3 && (unsigned char)s[1] == 0xB7) {          /* U+00F7 */
        P->tok.kind = TK_OP; P->tok.op = '/'; P->p = s + 2; return;
    }
    if (c == 0xE2 && (unsigned char)s[1] == 0x88 &&
        (unsigned char)s[2] == 0x92) {                       /* U+2212 */
        P->tok.kind = TK_OP; P->tok.op = '-'; P->p = s + 3; return;
    }

    if (c == '(') { P->tok.kind = TK_LP;    P->p = s + 1; return; }
    if (c == ')') { P->tok.kind = TK_RP;    P->p = s + 1; return; }
    if (c == ',') { P->tok.kind = TK_COMMA; P->p = s + 1; return; }

    if (c == '+' || c == '-' || c == '*' || c == '/' ||
        c == '%' || c == '^' || c == '!') {
        P->tok.kind = TK_OP; P->tok.op = (char)c; P->p = s + 1; return;
    }

    if (is_digit_ch(c) || (c == '.' && is_digit_ch((unsigned char)s[1]))) {
        const char *start = s;
        while (is_digit_ch((unsigned char)*s)) s++;
        if (*s == '.') {
            s++;
            while (is_digit_ch((unsigned char)*s)) s++;
        }
        if (*s == 'e' || *s == 'E') {
            const char *q = s + 1;
            if (*q == '+' || *q == '-') q++;
            if (is_digit_ch((unsigned char)*q)) {
                s = q;
                while (is_digit_ch((unsigned char)*s)) s++;
            }
        }
        size_t len = (size_t)(s - start);
        if (len >= 48) { p_fail(P, P_NUM, "number too long"); P->p = s; return; }
        char buf[48];
        memcpy(buf, start, len);
        buf[len] = 0;
        char *end = NULL;
        double v = strtod(buf, &end);
        if (!end || *end || !isfinite(v)) { p_fail(P, P_NUM, buf); P->p = s; return; }
        P->tok.kind = TK_NUM;
        P->tok.num  = v;
        P->p = s;
        return;
    }

    if (is_alpha_ch(c)) {
        size_t n = 0;
        while (is_alpha_ch((unsigned char)*s) || is_digit_ch((unsigned char)*s)) {
            if (n + 1 < sizeof P->tok.name) {
                unsigned char ch = (unsigned char)*s;
                if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
                P->tok.name[n++] = (char)ch;
            }
            s++;
        }
        P->tok.name[n] = 0;
        P->tok.kind = TK_IDENT;
        P->p = s;
        return;
    }

    /* Anything else is a character the calculator does not know. Keep the
     * glyph (up to 4 bytes) so the message can name it. */
    char bad[8];
    size_t n = 1;
    if ((c & 0xE0u) == 0xC0u) n = 2;
    else if ((c & 0xF0u) == 0xE0u) n = 3;
    else if ((c & 0xF8u) == 0xF0u) n = 4;
    if (n > 4) n = 1;
    for (size_t i = 0; i < n && s[i]; i++) bad[i] = s[i];
    bad[n] = 0;
    p_fail(P, P_CHAR, bad);
    P->p = s + n;
}

/* ---- domain-checked arithmetic: nothing inf or nan leaves here ---- */

static double p_domain(Parser *P, const char *why) {
    p_fail(P, P_DOMAIN, why);
    return 0.0;
}

static double p_pow(Parser *P, double base, double e) {
    if (base == 0.0 && e < 0.0)
        return p_domain(P, "0 raised to a negative power is undefined");
    if (base < 0.0 && e != floor(e))
        return p_domain(P, "a negative base needs a whole-number power");
    double r = pow(base, e);
    if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    return r;
}

static double p_root(Parser *P, double x, double n) {
    if (n == 0.0) return p_domain(P, "the zeroth root is undefined");
    if (n == 1.0) return x;
    if (n == 2.0) {
        if (x < 0.0) return p_domain(P, "even root of a negative number");
        return sqrt(x);                  /* exact for perfect squares */
    }
    if (x < 0.0) {
        if (n != floor(n) || fmod(n, 2.0) == 0.0)
            return p_domain(P, "even root of a negative number");
        double r = -pow(-x, 1.0 / n);
        if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
        return r;
    }
    double r = pow(x, 1.0 / n);
    if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    return r;
}

static double p_fact(Parser *P, double x) {
    if (x < 0.0) return p_domain(P, "factorial of a negative number");
    if (x != floor(x)) return p_domain(P, "factorial of a non-integer");
    if (x > 170.0) { p_fail(P, P_RANGE, NULL); return 0.0; }
    double r = 1.0;
    for (int i = 2; i <= (int)x; i++) r *= (double)i;
    if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    return r;
}

static double p_angle_in(Parser *P, double x) {
    return P->deg ? x * (CALC_PI / 180.0) : x;
}

static double p_angle_out(Parser *P, double x) {
    return P->deg ? x * (180.0 / CALC_PI) : x;
}

static double p_tan(Parser *P, double x) {
    if (P->deg) {
        double m = fmod(x, 180.0);
        if (m == 90.0 || m == -90.0)
            return p_domain(P, "tan is undefined at 90 degrees");
    }
    double r = tan(p_angle_in(P, x));
    if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    return r;
}

static double p_log(Parser *P, double x, int base10) {
    if (x <= 0.0)
        return p_domain(P, base10 ? "log10 of a non-positive number"
                                  : "ln of a non-positive number");
    double r = base10 ? log10(x) : log(x);
    if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    return r;
}

/* One place applies a named function, so the parser cannot grow a second
 * slightly different copy of a domain rule. */
static double p_apply(Parser *P, const char *name, double a) {
    if (!strcmp(name, "sin")) {
        double r = sin(p_angle_in(P, a));
        if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
        return r;
    }
    if (!strcmp(name, "cos")) {
        double r = cos(p_angle_in(P, a));
        if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
        return r;
    }
    if (!strcmp(name, "tan"))  return p_tan(P, a);
    if (!strcmp(name, "asin")) {
        if (a < -1.0 || a > 1.0)
            return p_domain(P, "asin needs a value between -1 and 1");
        return p_angle_out(P, asin(a));
    }
    if (!strcmp(name, "acos")) {
        if (a < -1.0 || a > 1.0)
            return p_domain(P, "acos needs a value between -1 and 1");
        return p_angle_out(P, acos(a));
    }
    if (!strcmp(name, "atan")) return p_angle_out(P, atan(a));
    if (!strcmp(name, "ln"))   return p_log(P, a, 0);
    if (!strcmp(name, "log") || !strcmp(name, "log10"))
        return p_log(P, a, 1);
    if (!strcmp(name, "exp")) {
        double r = exp(a);
        if (!isfinite(r)) { p_fail(P, P_RANGE, NULL); return 0.0; }
        return r;
    }
    if (!strcmp(name, "sqrt")) {
        if (a < 0.0) return p_domain(P, "square root of a negative number");
        return sqrt(a);
    }
    p_fail(P, P_IDENT, name);
    return 0.0;
}

/* ---- the grammar ---- */

static double p_expr(Parser *P, int depth);
static double p_power(Parser *P, int depth);

static int p_stop(Parser *P, int depth) {
    if (!P->code && depth > MAX_NEST) p_fail(P, P_DEPTH, NULL);
    return P->code != 0;
}

static double p_primary(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;

    Token t = P->tok;

    if (t.kind == TK_NUM) { lex_next(P); return t.num; }

    if (t.kind == TK_LP) {
        lex_next(P);
        double v = p_expr(P, depth + 1);
        if (P->code) return 0.0;
        if (P->tok.kind != TK_RP) { p_fail(P, P_LPAREN, NULL); return 0.0; }
        lex_next(P);
        return v;
    }

    if (t.kind == TK_IDENT) {
        lex_next(P);
        if (!strcmp(t.name, "pi")) return CALC_PI;
        if (!strcmp(t.name, "e"))  return CALC_E;

        if (!strcmp(t.name, "root")) {
            /* root(x, y) - the function form of the infix y-th root */
            if (P->tok.kind != TK_LP) { p_fail(P, P_LPAREN, NULL); return 0.0; }
            lex_next(P);
            double x = p_expr(P, depth + 1);
            if (P->code) return 0.0;
            if (P->tok.kind != TK_COMMA) { p_fail(P, P_COMMA, NULL); return 0.0; }
            lex_next(P);
            double n = p_expr(P, depth + 1);
            if (P->code) return 0.0;
            if (P->tok.kind != TK_RP) { p_fail(P, P_LPAREN, NULL); return 0.0; }
            lex_next(P);
            return p_root(P, x, n);
        }

        double a;
        if (P->tok.kind == TK_LP) {
            lex_next(P);
            a = p_expr(P, depth + 1);
            if (P->code) return 0.0;
            if (P->tok.kind != TK_RP) { p_fail(P, P_LPAREN, NULL); return 0.0; }
            lex_next(P);
        } else {
            a = p_primary(P, depth + 1);   /* sin 30, the calculator form */
            if (P->code) return 0.0;
        }
        return p_apply(P, t.name, a);
    }

    if (t.kind == TK_END) { p_fail(P, P_TRAILING, NULL); return 0.0; }
    if (t.kind == TK_OP) {
        char op[2];
        op[0] = t.op;
        op[1] = 0;
        p_fail(P, P_OPERATOR, op);
        return 0.0;
    }
    if (t.kind == TK_RP)    { p_fail(P, P_RPAREN, NULL); return 0.0; }
    if (t.kind == TK_COMMA) { p_fail(P, P_COMMA, NULL);  return 0.0; }
    return 0.0;
}

static double p_postfix(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;
    double v = p_primary(P, depth);
    while (!P->code && P->tok.kind == TK_OP) {
        if (P->tok.op == '!') {
            lex_next(P);
            v = p_fact(P, v);
        } else if (P->tok.op == '%') {
            lex_next(P);
            v = v / 100.0;
            if (!isfinite(v)) { p_fail(P, P_RANGE, NULL); return 0.0; }
        } else {
            break;
        }
    }
    return v;
}

static double p_unary(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;
    if (P->tok.kind == TK_OP && (P->tok.op == '+' || P->tok.op == '-')) {
        int neg = (P->tok.op == '-');
        lex_next(P);
        double v = p_unary(P, depth + 1);
        if (P->code) return 0.0;
        return neg ? -v : v;
    }
    return p_power(P, depth);
}

/* postfix ('^' unary)? - the exponent is a unary, so 2^-1 works and the
 * operator is right associative: 2^3^2 is 2^(3^2). The base is a postfix
 * expression, which is what makes -2^2 read as -(2^2). */
static double p_power(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;
    double v = p_postfix(P, depth);
    while (!P->code) {
        if (P->tok.kind == TK_OP && P->tok.op == '^') {
            lex_next(P);
            double e = p_unary(P, depth + 1);
            if (P->code) return 0.0;
            v = p_pow(P, v, e);
        } else if (P->tok.kind == TK_IDENT && !strcmp(P->tok.name, "root")) {
            lex_next(P);
            double n = p_unary(P, depth + 1);
            if (P->code) return 0.0;
            v = p_root(P, v, n);
        } else {
            break;
        }
    }
    return v;
}

static double p_term(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;
    double v = p_unary(P, depth);
    while (!P->code && P->tok.kind == TK_OP &&
           (P->tok.op == '*' || P->tok.op == '/')) {
        int op = P->tok.op;
        lex_next(P);
        double r = p_unary(P, depth);
        if (P->code) return 0.0;
        if (op == '/') {
            if (r == 0.0) return p_domain(P, "division by zero");
            v = v / r;
        } else {
            v = v * r;
        }
        if (!isfinite(v)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    }
    return v;
}

static double p_expr(Parser *P, int depth) {
    if (p_stop(P, depth)) return 0.0;
    double v = p_term(P, depth);
    while (!P->code && P->tok.kind == TK_OP &&
           (P->tok.op == '+' || P->tok.op == '-')) {
        int op = P->tok.op;
        lex_next(P);
        double r = p_term(P, depth);
        if (P->code) return 0.0;
        v = (op == '+') ? v + r : v - r;
        if (!isfinite(v)) { p_fail(P, P_RANGE, NULL); return 0.0; }
    }
    return v;
}

/* Turn a parser code into the sentence the status line shows. Every
 * message names the thing that is wrong, which is the point of having a
 * parser instead of a keypad. */
static void eval_why(int code, const char *detail, char *out, size_t n) {
    switch (code) {
        case P_EMPTY:    snprintf(out, n, "the expression is empty"); break;
        case P_CHAR:     snprintf(out, n, "unexpected character '%s'", detail); break;
        case P_NUM:      snprintf(out, n, "malformed number '%s'", detail); break;
        case P_RPAREN:   snprintf(out, n, "unmatched ')'"); break;
        case P_LPAREN:   snprintf(out, n, "missing a closing ')'"); break;
        case P_TRAILING: snprintf(out, n, "the expression ends with an operator"); break;
        case P_OPERATOR: snprintf(out, n, "unexpected operator '%s'", detail); break;
        case P_IDENT:    snprintf(out, n, "unknown name '%s'", detail); break;
        case P_DOMAIN:   snprintf(out, n, "%s", detail); break;
        case P_RANGE:    snprintf(out, n, "the result is out of range"); break;
        case P_DEPTH:    snprintf(out, n, "the expression nests deeper than %d levels",
                                  MAX_NEST); break;
        case P_COMMA:    snprintf(out, n, "unexpected ','"); break;
        case P_EXTRA:    snprintf(out, n, "unexpected text after the expression"); break;
        default:         snprintf(out, n, "error"); break;
    }
}

/* Evaluate a finished piece of text. Returns 1 and the double on success;
 * on failure returns 0 and writes a sentence naming the fault. */
static int sci_eval_text(const char *text, int deg, double *out,
                         char *why, size_t whyn) {
    Parser P;
    memset(&P, 0, sizeof P);
    P.p   = text ? text : "";
    P.deg = deg;

    lex_next(&P);
    if (P.code) { eval_why(P.code, P.detail, why, whyn); return 0; }
    if (P.tok.kind == TK_END) { eval_why(P_EMPTY, "", why, whyn); return 0; }

    double v = p_expr(&P, 0);
    if (!P.code) {
        if (P.tok.kind == TK_RP)       p_fail(&P, P_RPAREN, NULL);
        else if (P.tok.kind != TK_END) p_fail(&P, P_EXTRA, NULL);
    }
    if (P.code) { eval_why(P.code, P.detail, why, whyn); return 0; }
    if (!isfinite(v)) { eval_why(P_RANGE, "", why, whyn); return 0; }

    *out = v;
    return 1;
}

/* Build the text to evaluate: what is on the line, with the parentheses
 * the user left open closed at the end. The closed text is what the tape
 * records, so it is always clear what ran. */
static int sci_work_text(const Calc *c, char *work, size_t worksz) {
    int n = snprintf(work, worksz, "%s", c->expr);
    if (n < 0 || (size_t)n >= worksz) return 0;
    if (!work[0]) return 1;

    int open = 0;
    for (const char *p = c->expr; *p; p++) {
        if (*p == '(') open++;
        else if (*p == ')') { open--; if (open < 0) return 1; /* let the parser name it */ }
    }
    while (open > 0) {
        size_t len = strlen(work);
        if (len + 2 > worksz) break;
        work[len]     = ')';
        work[len + 1] = 0;
        open--;
    }
    return 1;
}

static int sci_eval_current(const Calc *c, char *work, size_t worksz,
                            double *out, char *why, size_t whyn) {
    if (!sci_work_text(c, work, worksz)) {
        snprintf(why, whyn, "the expression is too long to evaluate");
        return 0;
    }
    if (!work[0]) { snprintf(why, whyn, "the expression is empty"); return 0; }
    return sci_eval_text(work, c->deg, out, why, whyn);
}

/* '=' in scientific mode: evaluate, remember the result on both lines,
 * and leave the result on the expression line so an operator can carry
 * on from it. */
static int sci_equals(Calc *c) {
    if (!c->expr[0]) { c->fresh = 1; return 1; }

    char work[EXPR_MAX + MAX_NEST + 2];
    char why[WHY_MAX];
    double v = 0.0;
    if (!sci_eval_current(c, work, sizeof work, &v, why, sizeof why)) {
        set_error(c, why);
        return 0;
    }

    char sr[ENTRY_MAX];
    fmt_number(v, sr, sizeof sr);
    snprintf(c->tape, sizeof c->tape, "%s = %s", work, sr);
    c->have_tape = 1;

    entry_store(c, v);
    snprintf(c->expr, sizeof c->expr, "%s", sr);
    c->fresh = 1;
    return 1;
}

/* The two mode chips. Switching never throws the number on screen away:
 * entering scientific mode starts the line from the value that plain mode
 * had finished with, and leaving it evaluates the line (so the plain
 * accumulator resumes from the right number). */
static void toggle_sci(Calc *c) {
    if (c->error) reset_calc(c);   /* a mode switch is a key press too */
    if (!c->sci) {
        if (c->pending != OP_NONE) press_eq(c);
        c->sci   = 1;
        c->fresh = 0;
        if (!c->error && entry_plain(c->entry) &&
            (c->value != 0.0 || c->have_tape))
            snprintf(c->expr, sizeof c->expr, "%s", c->entry);
        else
            c->expr[0] = 0;
        return;
    }

    if (!c->error && c->expr[0]) {
        char work[EXPR_MAX + MAX_NEST + 2];
        char why[WHY_MAX];
        double v = 0.0;
        if (sci_eval_current(c, work, sizeof work, &v, why, sizeof why)) {
            c->value = v;
        } else {
            set_error(c, why);
        }
    }
    c->sci     = 0;
    c->expr[0] = 0;
    c->pending = OP_NONE;
    c->entered = 0;
    c->fresh   = 1;
    if (!c->error) entry_store(c, c->value);
}

/* Every scientific key. B_SCI and B_DEG are handled by calc_press before
 * the error branch, so they are not repeated here. */
static void calc_press_sci(Calc *c, int id) {
    switch (id) {
        case B_0: case B_1: case B_2: case B_3: case B_4:
        case B_5: case B_6: case B_7: case B_8: case B_9:
            sci_digit(c, (char)('0' + (id - B_0)));
            break;
        case B_DOT:  sci_dot(c); break;
        case B_ADD:  sci_op(c, '+'); break;
        case B_SUB:  sci_op(c, '-'); break;
        case B_MUL:  sci_op(c, '*'); break;
        case B_DIV:  sci_op(c, '/'); break;
        case B_POW:  sci_op(c, '^'); break;

        case B_LP:
            if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
            expr_cat(c, "(");
            break;
        case B_RP:
            c->fresh = 0;
            expr_cat(c, ")");
            break;
        case B_ROOT:
            /* The y-th root needs a value on its left. 9, 2., 5!, 50%, pi
             * and e are all values, so all of them take " root ". */
            if (expr_ends_value(c->expr)) {
                expr_cat(c, " root ");
                c->fresh = 0;
            }
            break;
        case B_FACT:
            if (c->expr[0]) { expr_cat(c, "!"); c->fresh = 0; }
            break;
        case B_PCT:
            if (c->expr[0]) { expr_cat(c, "%"); c->fresh = 0; }
            break;

        case B_SIN:   sci_function(c, "sin");  break;
        case B_COS:   sci_function(c, "cos");  break;
        case B_TAN:   sci_function(c, "tan");  break;
        case B_ASIN:  sci_function(c, "asin"); break;
        case B_ACOS:  sci_function(c, "acos"); break;
        case B_ATAN:  sci_function(c, "atan"); break;
        case B_LN:    sci_function(c, "ln");   break;
        case B_LOG:   sci_function(c, "log");   break;
        case B_EXP:   sci_function(c, "exp");  break;

        case B_PI:
            if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
            else sci_mul_before_value(c);
            expr_cat(c, "\xCF\x80");          /* the pi glyph, as labelled */
            break;
        case B_ECONST:
            if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
            else sci_mul_before_value(c);
            expr_cat(c, "e");
            break;

        case B_NEG:   sci_neg(c); break;
        case B_RECIP: sci_wrap_key(c, "1/(", ")", "1/"); break;
        case B_SQ:    sci_wrap_key(c, "(", ")^2", NULL); break;
        case B_SQRT:  sci_wrap_key(c, "sqrt(", ")", "sqrt("); break;

        case B_BACK:  expr_back(c); c->fresh = 0; break;
        case B_CE:    c->expr[0] = 0; c->fresh = 0; break;
        case B_C:     reset_calc(c); break;
        case B_EQ:    sci_equals(c); break;

        case B_MC:
            press_mem(c, B_MC);
            break;
        case B_MR: {
            if (c->fresh) { c->expr[0] = 0; c->fresh = 0; }
            else sci_mul_before_value(c);
            char m[ENTRY_MAX];
            fmt_number(c->memory, m, sizeof m);
            expr_cat(c, m);
            break;
        }
        case B_MPLUS:
        case B_MMINUS:
            if (c->expr[0] && !sci_equals(c)) break;
            press_mem(c, id);
            break;

        default:
            break;
    }
}

/* Every button and every key ends up here. Nothing else calls the press_*
 * helpers, which is what keeps the two input paths honest mirrors. */
static void calc_press(Calc *c, int id) {
    if (!c) return;

    /* The two mode switches are never destructive and never hit the error
     * branch: they are how the user moves between the keypads. */
    if (id == B_SCI) { toggle_sci(c); return; }
    if (id == B_DEG) {
        if (c->error) reset_calc(c);
        c->deg = !c->deg;
        return;
    }

    if (c->error) {
        int sci = c->sci;
        reset_calc(c);
        if (id >= B_0 && id <= B_9) {
            char d = (char)('0' + (id - B_0));
            if (sci) sci_digit(c, d);
            else     press_digit(c, d);
        } else if (id == B_DOT) {
            if (sci) sci_dot(c);
            else     press_dot(c);
        } else if (sci && id == B_LP) {
            expr_cat(c, "(");
        }
        return;                          /* the error was the whole meaning */
    }

    if (c->sci) { calc_press_sci(c, id); return; }

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

/* Only the labels that are really on screen are measured: a hidden
 * scientific block must not make the plain keypad's text smaller. */
static float widest_single_label(Font *f, int sci) {
    float widest = 0.0f;
    if (!f) return 0.0f;
    for (int i = 0; i < NBASE; i++) {
        if (BASE_BTNS[i].cols != 1) continue;   /* only one-cell labels are tight */
        float w = font_width(f, BASE_BTNS[i].label);
        if (w > widest) widest = w;
    }
    if (!sci) return widest;
    for (int i = 0; i < NSCI; i++) {
        if (SCI_BTNS[i].cols != 1) continue;
        float w = font_width(f, SCI_BTNS[i].label);
        if (w > widest) widest = w;
    }
    return widest;
}

/* Height of one grid row, given the height the grid has to live in. */
static float cell_height_for(float grid_h, int rows, float gap) {
    return (grid_h - (float)(rows - 1) * gap) / (float)rows;
}

static void layout_add(Layout *L, const BtnDef *d, int row_base) {
    if (L->nbtn >= NRECT) return;
    BtnRect *b = &L->btn[L->nbtn++];
    b->x = L->grid_x + (float)d->col * (L->cell_w + L->gap);
    b->y = L->grid_y + (float)(d->row + row_base) * (L->cell_h + L->gap);
    b->w = (float)d->cols * L->cell_w + (float)(d->cols - 1) * L->gap;
    b->h = (float)d->rows * L->cell_h + (float)(d->rows - 1) * L->gap;
    b->id    = d->id;
    b->style = d->style;
    b->kind  = K_GRID;
    b->label = d->label;
}

static int layout_calc(int w, int h, const Calc *c, Layout *L) {
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
    L->g2  = g2;

    int sci  = c && c->sci;
    int deg  = c ? c->deg : 1;
    int rows = BASE_ROWS + (sci ? SCI_ROWS : 0);

    /* ---- header band: mode chips, height follows the status font ---- */
    float status_h = font_height(fs);
    float chip_h   = status_h + 2.0f * g2;
    if (chip_h > inner_h * 0.22f) chip_h = inner_h * 0.22f;
    if (chip_h < 8.0f) return 0;

    float body_h = inner_h - chip_h - gap;
    if (body_h <= 0.0f) return 0;

    /* ---- vertical split: display band over the button grid ---- */
    float min_disp = 2.0f * g2 + status_h + g2 + font_height(fvm);
    float disp_h   = body_h * 0.26f;
    float max_disp = body_h * 0.44f;
    if (disp_h < min_disp) disp_h = min_disp;
    if (disp_h > max_disp) disp_h = max_disp;

    int   show_status = 1;
    float grid_h = body_h - disp_h - gap;
    float cell_h = cell_height_for(grid_h, rows, gap);
    float min_cell = font_height(fsm) + 1.0f;

    if (cell_h < min_cell) {
        /* Not enough height for both display lines and printable rows:
         * the status line is the least important thing in the window. */
        float disp2 = font_height(fvm) + 2.0f * g2;
        float grid2 = body_h - disp2 - gap;
        float cell2 = cell_height_for(grid2, rows, gap);
        if (cell2 > cell_h) {
            show_status = 0;
            disp_h      = disp2;
            cell_h      = cell2;
        }
    }
    if (cell_h < 4.0f) return 0;

    float cell_w = (inner_w - (float)(GRID_COLS - 1) * gap) / (float)GRID_COLS;
    if (cell_w < 8.0f) return 0;

    /* Portrait discipline: a key is never wider than twice its height, so
     * a wide window does not turn the keypad into a letterbox. The grid is
     * then centred in whatever width it was given. */
    float grid_w = inner_w;
    if (cell_w > cell_h * 2.0f) {
        cell_w = cell_h * 2.0f;
        grid_w = (float)GRID_COLS * cell_w + (float)(GRID_COLS - 1) * gap;
    }
    float grid_x = pad + (inner_w - grid_w) * 0.5f;

    /* ---- the fonts are fitted to the bands they have to live in ---- */
    int status_size = 10;
    int value_size  = (int)((disp_h - 2.0f * g2 - (float)ui_px(2) -
                             (show_status ? status_h + g2 : 0.0f)) / (1.4f * scale));
    if (value_size > 72) value_size = 72;
    if (value_size < 12) value_size = 12;

    int btn_size = (int)((cell_h - (float)ui_px(6)) / (1.4f * scale));
    if (btn_size > 34) btn_size = 34;

    /* A label is never drawn smaller than 9 device pixels. Below that the
     * keys stay but their text is dropped - that is the legibility floor,
     * not a guess at one. */
    int min_legible = (int)(9.0f / scale + 0.999f);
    if (min_legible < 6) min_legible = 6;
    if (btn_size < min_legible) btn_size = min_legible;

    float label_room = cell_w - (float)ui_px(2);
    int label_ok = 0;
    for (int tries = 0; tries < 40 && btn_size >= min_legible; tries++) {
        Font *fb = font_get(FONT_MEDIUM, (float)btn_size);
        if (!fb) break;
        if (font_height(fb) <= cell_h - 0.5f &&
            widest_single_label(fb, sci) <= label_room) {
            label_ok = 1;
            break;
        }
        if (btn_size == min_legible) break;
        btn_size--;
    }

    /* ---- the rectangles, recorded once for the painter and the hit test ---- */
    L->show_status = show_status;
    L->rows        = rows;
    L->head_y      = pad;
    L->head_h      = chip_h;
    L->chip_h      = chip_h;
    L->chip_radius = chip_h * 0.35f;
    if (L->chip_radius < 2.0f) L->chip_radius = 2.0f;

    /* The display sits in the same column as the keypad. When the window
     * is wider than the portrait grid, the whole calculator is one column
     * centred in it, instead of a wide display floating over a narrow pad. */
    L->disp_x      = grid_x;
    L->disp_y      = pad + chip_h + gap;
    L->disp_w      = grid_w;
    L->disp_h      = disp_h;
    L->disp_radius = grid_w * 0.08f;
    if (L->disp_radius > disp_h * 0.40f) L->disp_radius = disp_h * 0.40f;
    if (L->disp_radius < 2.0f) L->disp_radius = 2.0f;

    /* Text on the top row of a rounded recess has to clear the corner
     * curve, otherwise the first glyph is clipped by the border. The inset
     * comes from the radius, not from a guessed number. */
    float corner_inset = 0.0f;
    if (L->disp_radius > g2 + 1.0f) {
        float rr = L->disp_radius;
        float dy = g2 + 0.5f;
        corner_inset = rr - sqrtf(dy * (2.0f * rr - dy));
        if (corner_inset < 0.0f) corner_inset = 0.0f;
    }

    L->status_size = status_size;
    L->status_x    = L->disp_x + g2 + corner_inset;
    L->status_y    = L->disp_y + g2;
    L->status_w    = L->disp_w - 2.0f * (g2 + corner_inset);
    L->status_h    = status_h;

    L->value_size = value_size;
    L->value_x    = L->disp_x + g2;
    L->value_y    = L->status_y + (show_status ? status_h + g2 : 0.0f);
    L->value_w    = L->disp_w - 2.0f * g2;
    L->value_h    = L->disp_y + L->disp_h - g2 - L->value_y;

    L->grid_x = grid_x;
    L->grid_y = L->disp_y + L->disp_h + gap;
    L->cell_w = cell_w;
    L->cell_h = cell_h;

    float radius = (cell_w < cell_h ? cell_w : cell_h) * 0.20f;
    if (radius > cell_h * 0.45f) radius = cell_h * 0.45f;
    if (radius < 1.0f) radius = 1.0f;
    L->btn_radius = radius;
    L->btn_size   = btn_size;
    L->label_ok   = label_ok;

    for (int i = 0; i < NBASE; i++) layout_add(L, &BASE_BTNS[i], 0);
    if (sci)
        for (int i = 0; i < NSCI; i++) layout_add(L, &SCI_BTNS[i], BASE_ROWS);

    /* ---- the chips, left-aligned in the header band ---- */
    {
        float cx = L->disp_x;
        float chip_pad = g2 * 2.0f;

        const char *labels[2];
        int ids[2], styles[2], nch = 0;
        labels[nch] = "2nd";
        ids[nch]    = B_SCI;
        styles[nch] = sci ? S_OP : S_FN;
        nch++;
        if (sci) {
            labels[nch] = deg ? "DEG" : "RAD";
            ids[nch]    = B_DEG;
            styles[nch] = S_MEM;
            nch++;
        }

        for (int i = 0; i < nch; i++) {
            float cw = font_width(fs, labels[i]) + 2.0f * chip_pad;
            float minw = chip_h * 1.4f;
            if (cw < minw) cw = minw;
            if (cw > L->disp_w) cw = L->disp_w;
            if (i > 0 && cx + cw > L->disp_x + L->disp_w) break;   /* no room: skip it */
            if (L->nbtn >= NRECT) break;

            BtnRect *b = &L->btn[L->nbtn++];
            b->x = cx;
            b->y = L->head_y;
            b->w = cw;
            b->h = chip_h;
            b->id    = ids[i];
            b->style = styles[i];
            b->kind  = K_CHIP;
            b->label = labels[i];
            cx += cw + gap;
        }
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

/* The longest suffix that fits, marked with "..". An expression being
 * typed grows at its right end, so when it is wider than the display the
 * end is what must stay visible. */
static void tail_text(Font *f, const char *text, float maxw,
                      char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = 0;
    if (!f || !text || !text[0] || maxw <= 0.0f) return;

    size_t n = strlen(text);
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, text, n);
    out[n] = 0;
    if (font_width(f, out) <= maxw) return;

    char tmp[TEXT_MAX + 8];
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)text[i] & 0xC0u) == 0x80u) continue;
        snprintf(tmp, sizeof tmp, "..%s", text + i);
        if (font_width(f, tmp) <= maxw) {
            snprintf(out, outsz, "%s", tmp);
            return;
        }
    }
    out[0] = 0;
}

/* What the status line says: the cause of an error, the expression being
 * built, the angle unit, or the last completed calculation. All of them
 * are the calculator's real state, never decoration. */
static void build_status(const Calc *c, char *buf, size_t n) {
    if (!buf || !n) return;
    buf[0] = 0;

    if (c->error) { snprintf(buf, n, "%s", c->err); return; }

    if (c->sci) {
        if (c->have_tape) snprintf(buf, n, "%s", c->tape);
        else              snprintf(buf, n, "scientific - %s",
                                   c->deg ? "degrees" : "radians");
        return;
    }

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
    if (!layout_calc(w, h, c, &L)) return;

    /* The status line, the chips and the memory badge share one small face. */
    Font *fsmall = font_get(FONT_MEDIUM, (float)L.status_size);
    Font *fbtn   = L.label_ok ? font_get(FONT_MEDIUM, (float)L.btn_size) : NULL;
    if (!fsmall) return;
    if (L.label_ok && !fbtn) return;

    float ox = (float)x;
    float oy = (float)y;

    /* ---- header: mode chips on the left, the memory badge on the right ---- */
    char mem[ENTRY_MAX + 8];
    mem[0] = 0;
    if (c->mem_set) {
        char m[ENTRY_MAX];
        fmt_number(c->memory, m, sizeof m);
        snprintf(mem, sizeof mem, "M %s", m);
    }

    float chips_right = L.disp_x;
    for (int i = 0; i < L.nbtn; i++) {
        const BtnRect *b = &L.btn[i];
        if (b->kind != K_CHIP) continue;
        u32 fill, border, label;
        btn_colors(th, b->style, &fill, &border, &label);
        gfx_rrect_aa(s, ox + b->x, oy + b->y, b->w, b->h,
                     L.chip_radius, fill, border, 1.0f);

        float lw = font_width(fsmall, b->label);
        float lh = font_height(fsmall);
        if (lw <= b->w - 2.0f && lh <= b->h - 1.0f) {
            font_draw(s, fsmall, ox + b->x + (b->w - lw) * 0.5f,
                      oy + b->y + (b->h - lh) * 0.5f, b->label, label);
        }
        if (b->x + b->w > chips_right) chips_right = b->x + b->w;
    }

    /* The memory badge only goes in if it does not run into the chips. */
    if (mem[0]) {
        float mw = font_width(fsmall, mem);
        float mx = L.disp_x + L.disp_w - mw;
        if (mw <= L.disp_w - L.g2 && mx >= chips_right + L.g2) {
            font_draw(s, fsmall, ox + mx,
                      oy + L.head_y + (L.head_h - font_height(fsmall)) * 0.5f,
                      mem, color_alpha(th->accent, 0.90f));
        }
    }

    /* ---- the display recess ----
     * A 5% text-colour wash reads as a darker well in a light theme and a
     * lighter one in a dark theme, so one expression serves both. */
    gfx_rrect_aa(s, ox + L.disp_x, oy + L.disp_y, L.disp_w, L.disp_h,
                 L.disp_radius, color_alpha(th->text, 0.05f),
                 th->panel_border, 1.0f);

    /* ---- status line ---- */
    if (L.show_status) {
        char status[TEXT_MAX];
        build_status(c, status, sizeof status);
        if (status[0] && L.status_w > 2.0f) {
            char shown[TEXT_MAX];
            fit_text(fsmall, status, L.status_w, shown, sizeof shown);
            font_draw(s, fsmall, ox + L.status_x, oy + L.status_y, shown,
                      c->error ? th->accent : color_alpha(th->text, 0.55f));
        }
    }

    /* ---- the value, right aligned ----
     * The value font is the one layout picked for the band; if the number
     * or the expression is still wider than the window it is reduced in
     * steps (real font metrics each time) until it fits or reaches the
     * readable floor. */
    int   showing_expr = c->sci && !c->error && c->expr[0];
    const char *shown  = c->error ? "Error" : (showing_expr ? c->expr : c->entry);
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
    if (showing_expr) tail_text(fv, shown, L.value_w, vtext, sizeof vtext);
    else              fit_text(fv, shown, L.value_w, vtext, sizeof vtext);
    tw = font_width(fv, vtext);

    float vy = oy + L.value_y + (L.value_h - font_height(fv)) * 0.5f;
    if (vy < oy + L.value_y) vy = oy + L.value_y;
    font_draw(s, fv, ox + L.value_x + L.value_w - tw, vy, vtext,
              c->error ? th->accent : th->text);

    /* ---- the buttons ---- */
    float fbth = fbtn ? font_height(fbtn) : 0.0f;
    for (int i = 0; i < L.nbtn; i++) {
        const BtnRect *b = &L.btn[i];
        if (b->kind != K_GRID) continue;
        u32 fill, border, label;
        btn_colors(th, b->style, &fill, &border, &label);

        float bx = ox + b->x;
        float by = oy + b->y;
        gfx_rrect_aa(s, bx, by, b->w, b->h, L.btn_radius, fill, border, 1.0f);

        if (!L.label_ok || !fbtn) continue;
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
    c->w  = w;
    c->h  = h;
    c->deg = 1;                     /* a desk calculator starts in degrees */
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
            case '(':                       calc_press(c, B_LP);   break;
            case ')':                       calc_press(c, B_RP);   break;
            case '^':                       calc_press(c, B_POW);  break;
            case '!':                       calc_press(c, B_FACT); break;
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
            /* scientific keypad - free letters, documented in the header */
            case 'i': case 'I':             calc_press(c, B_SCI);  break;
            case 'g': case 'G':             calc_press(c, B_DEG);  break;
            case 'b':                       calc_press(c, B_SIN);  break;
            case 'j':                       calc_press(c, B_COS);  break;
            case 't':                       calc_press(c, B_TAN);  break;
            case 'B':                       calc_press(c, B_ASIN); break;
            case 'J':                       calc_press(c, B_ACOS); break;
            case 'T':                       calc_press(c, B_ATAN); break;
            case 'l': case 'L':             calc_press(c, B_LN);   break;
            case 'o': case 'O':             calc_press(c, B_LOG);  break;
            case 'f': case 'F':             calc_press(c, B_EXP);  break;
            case 'p': case 'P':             calc_press(c, B_PI);   break;
            case 'u': case 'U':             calc_press(c, B_ECONST);break;
            case 'y': case 'Y':             calc_press(c, B_ROOT); break;
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
    if (!layout_calc(c->w, c->h, c, &L)) return;

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

/* ============================================================
 *  Host proof harness (not part of the OS build)
 *
 *  The plain keypad is an immediate-execution calculator users already
 *  relies on, so its behaviour is pinned here by a matrix instead of by
 *  prose. Each case starts from a fresh calculator, types real key bytes
 *  through calc_key() - the same entry point the desktop's input layer
 *  uses - and then checks the value line, the Error flag, the sentence
 *  naming the cause, or the memory.
 *
 *  This block is compiled only for the host harness, which includes this
 *  file together with stubs for the font, surface and theme calls:
 *
 *      zig cc -DHOST_TEST -Isrc -I. -o calc_h.exe harness.c -lm
 *
 *  harness.c includes "apps/calc.c" and calls calc_selftest(); there is no
 *  main() here because the harness owns it.
 * ============================================================ */
#ifdef HOST_TEST

/* Key bytes as a printable label: a backspace would otherwise edit the
 * terminal line the results are printed on. */
static void st_label(const char *keys, char *out, size_t outsz) {
    size_t o = 0;
    if (!outsz) return;
    for (; keys && *keys && o + 4 < outsz; keys++) {
        unsigned char ch = (unsigned char)*keys;
        if (ch == 0x08) {
            out[o++] = '\\';
            out[o++] = 'b';
        } else if (ch == 0x7f) {
            out[o++] = 'D';
            out[o++] = 'E';
            out[o++] = 'L';
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}

/* One fresh calculator for one case. */
static Calc *st_fresh(App *a) {
    memset(a, 0, sizeof *a);
    a->vt = &app_vt_calc;
    calc_init(a, 300, 520);
    return a->state;
}

/* A case with a result: the value line must read exactly `want`, and no
 * Error flag may be standing. */
static int st_plain(int *cases, const char *keys, const char *want,
                    const char *note) {
    char label[TEXT_MAX];
    App a;
    Calc *c = st_fresh(&a);
    (*cases)++;
    if (!c) {
        printf("FAIL plain \"%s\" (%s): no state\n", keys, note);
        return 1;
    }
    calc_key(&a, keys, (int)strlen(keys));
    st_label(keys, label, sizeof label);

    int bad = 0;
    if (c->error || strcmp(c->entry, want)) {
        printf("FAIL plain \"%s\" (%s): entry \"%s\"%s, want \"%s\"\n",
               label, note, c->entry, c->error ? " [Error]" : "", want);
        bad = 1;
    } else {
        printf("  ok   %-28s -> \"%s\"  (%s)\n", label, c->entry, note);
    }
    calc_free(&a);
    return bad;
}

/* A case that must fail: Error on the value line and a sentence that names
 * the fault. The double must stay finite, because the display is never
 * allowed to show nan or inf. */
static int st_error(int *cases, const char *keys, const char *why,
                    const char *note) {
    char label[TEXT_MAX];
    App a;
    Calc *c = st_fresh(&a);
    (*cases)++;
    if (!c) {
        printf("FAIL error \"%s\" (%s): no state\n", keys, note);
        return 1;
    }
    calc_key(&a, keys, (int)strlen(keys));
    st_label(keys, label, sizeof label);

    int bad = 0;
    if (!c->error) {
        printf("FAIL error \"%s\" (%s): no Error, entry \"%s\"\n",
               label, note, c->entry);
        bad = 1;
    } else if (!strstr(c->err, why)) {
        printf("FAIL error \"%s\" (%s): \"%s\" does not name \"%s\"\n",
               label, note, c->err, why);
        bad = 1;
    } else if (!isfinite(c->value)) {
        printf("FAIL error \"%s\" (%s): value is not finite\n", label, note);
        bad = 1;
    } else {
        printf("  ok   %-28s -> Error: %s\n", label, c->err);
    }
    calc_free(&a);
    return bad;
}

/* A case that finishes in memory: the recalled value line and the stored
 * double have to agree. */
static int st_memory(int *cases, const char *keys, double want,
                     const char *want_entry, const char *note) {
    char label[TEXT_MAX];
    App a;
    Calc *c = st_fresh(&a);
    (*cases)++;
    if (!c) {
        printf("FAIL memory \"%s\" (%s): no state\n", keys, note);
        return 1;
    }
    calc_key(&a, keys, (int)strlen(keys));
    st_label(keys, label, sizeof label);

    int bad = 0;
    if (!c->mem_set || c->memory != want) {
        printf("FAIL memory \"%s\" (%s): M %s%.17g, want %.17g\n", label, note,
               c->mem_set ? "" : "(unset) ", c->memory, want);
        bad = 1;
    } else if (strcmp(c->entry, want_entry)) {
        printf("FAIL memory \"%s\" (%s): entry \"%s\", want \"%s\"\n",
               label, note, c->entry, want_entry);
        bad = 1;
    } else {
        printf("  ok   %-28s -> M %.17g, entry \"%s\"  (%s)\n",
               label, c->memory, c->entry, note);
    }
    calc_free(&a);
    return bad;
}

int calc_selftest(void) {
    int cases = 0, fails = 0;

    printf("calc.c plain-mode regression matrix (calc_key byte path)\n");

    /* backspace */
    fails += st_plain(&cases, "123\b", "12", "backspace drops one digit");
    fails += st_plain(&cases, "12\b\b\b", "0", "backspace past the last digit");
    fails += st_plain(&cases, "1.5\b", "1.", "backspace inside a decimal");

    /* dot */
    fails += st_plain(&cases, ".5", "0.5", "a leading dot gets its zero");
    fails += st_plain(&cases, "1.2.3", "1.23", "one dot per number");
    fails += st_plain(&cases, "2.", "2.", "a trailing dot stays as typed");

    /* +/- */
    fails += st_plain(&cases, "12n", "-12", "n flips the sign");
    fails += st_plain(&cases, "12nn", "12", "n flips it back");
    fails += st_plain(&cases, "12.5n", "-12.5", "the sign keeps the decimals");

    /* 1/x, x squared, square root */
    fails += st_plain(&cases, "4r", "0.25", "r is 1/x");
    fails += st_plain(&cases, "9s", "81", "s is x squared");
    fails += st_plain(&cases, "9q", "3", "q is the square root");
    fails += st_plain(&cases, "2q", "1.41421356237", "sqrt(2) to 12 digits");

    /* repeated operators, immediate execution */
    fails += st_plain(&cases, "7++3=", "10", "a repeated operator is replaced");
    fails += st_plain(&cases, "7+*3=", "21", "the second operator wins");
    fails += st_plain(&cases, "7+3*4=", "40", "plain mode is immediate: (7+3)*4");
    fails += st_plain(&cases, "200+10%=", "220", "% is of the accumulator");

    /* the entry ceiling: 23 visible characters */
    fails += st_plain(&cases, "123456789012345678901234567890",
                      "12345678901234567890123", "the entry stops at 23 digits");

    /* the documented letter keys */
    fails += st_plain(&cases, "5acm", "5", "a stores, c clears, m recalls");
    fails += st_memory(&cases, "3ac1dcm", 2.0, "2", "d subtracts from memory");
    fails += st_plain(&cases, "5akm", "0", "k clears memory, m recalls 0");
    fails += st_plain(&cases, "5+3e2=", "7", "e is CE, the pending + survives");
    fails += st_plain(&cases, "123e", "0", "CE empties the entry");

    /* the errors the plain keypad owns */
    fails += st_error(&cases, "0r", "division by zero", "1/x of zero");
    fails += st_error(&cases, "5/0=", "division by zero", "divide by zero");
    fails += st_error(&cases, "9nq", "square root of a negative",
                      "sqrt of a negative");

    printf("calc.c matrix: %d cases, %d failures\n", cases, fails);
    return fails;
}

#endif /* HOST_TEST */
