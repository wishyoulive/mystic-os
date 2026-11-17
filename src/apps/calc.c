/* ============================================================
 *  Mystic OS - apps/calc.c
 *  "Calculator": an expression evaluator, not a keypad.
 *
 *  What you type is a string. Enter parses it with a recursive
 *  descent parser (real precedence, real associativity) and shows
 *  either the number or a message that names what is wrong - which
 *  parenthesis, which character, which operator, and whether the
 *  divisor was zero.
 *
 *  Grammar, lowest precedence first:
 *
 *      expr    := term   (('+' | '-') term)*
 *      term    := unary  (('*' | '/' | '%') unary)*
 *      unary   := ('+' | '-') unary | primary
 *      primary := number | '(' expr ')' | 'ans'
 *
 *  So '*' '/' '%' bind tighter than '+' '-' (2+3*4 is 14), and the
 *  loops make each level left associative (8-3-2 is 3, not 7;
 *  8/4/2 is 1). '%' is the remainder, the same function C's fmod
 *  computes. 'ans' is the previous result - the parser is handed
 *  it, so it is a real value and not a substitution in the text.
 *  Nesting is capped at MAX_NEST levels and the cap is checked
 *  before recursing, so no input can grow the stack.
 *
 *  Keys:  digits . + - * / % ( )   append to the expression
 *         Enter or '='             evaluate
 *         Backspace                delete the last character
 *         c                        clear the expression
 *         'ans'                    the value of the last result
 *
 *  Buttons: C, (, ), /, = . '(' ')' and '/' append their token;
 *  C clears the entry and '=' evaluates it, because appending a
 *  literal 'C' or '=' would only produce a garbage-character error
 *  and those are the two meanings a calculator must have.
 *
 *  Every band in this window is measured from font_height() of the font
 *  it actually draws with, because font_get() applies the global text
 *  scale (src/font.c): at scale 1.6 a 10-point label is 22 px tall, so a
 *  fixed 14 px row stride would print one history row on top of the next.
 *  Only paddings and gaps stay nominal, and those go through ui_px().
 *
 *  The expression buffer is a fixed 64 bytes and nothing is allocated per
 *  frame; the single allocation is the state struct, made once in
 *  calc_init() and released in calc_free().
 * ============================================================ */
#include "mystic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---8<--- parser: everything between the markers is a pure function
 * of its arguments and is exercised on the host by a harness that
 * extracts exactly this text (see the build note in the report).
 * ---8<--- */
#define EXPR_MAX   64      /* 63 characters + the terminating NUL */
#define RESULT_MAX 28
#define MSG_MAX    96
#define MAX_NEST   16      /* recursion guard for '(' groups and unary runs */

enum {
    E_OK = 0,          /* no error */
    E_CHAR,            /* a character that has no meaning here */
    E_NAME,            /* an identifier that is not 'ans' */
    E_NOANS,           /* 'ans' used before any result exists */
    E_NUMBER,          /* a malformed number */
    E_TRAIL_OP,        /* the expression stops on an operator */
    E_OPERAND,         /* an operator where a value belongs */
    E_PAREN_OPEN,      /* a '(' with no ')' */
    E_PAREN_CLOSE,     /* a ')' with no '(' */
    E_DIV0,
    E_MOD0,
    E_NEST,
    E_RANGE,
    E_EXPECT           /* a value belongs here, input ended */
};

typedef struct {
    const char *src;
    int   pos;
    int   err;                  /* E_*; the first error wins */
    int   err_pos;
    char  err_ch;               /* the offending character, 0 if none */
    char  err_name[24];         /* the offending identifier for E_NAME */
    double ans;
    int   have_ans;
} Parser;

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_op(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%';
}

/* The last non-space character before `pos`, or 0 at the start. Used to
 * explain *why* a value is missing: after a '+' the input ended on an
 * operator, after a '(' it never began a value at all. */
static char prev_nonspace(const char *s, int pos) {
    for (int i = pos - 1; i >= 0; i--)
        if (s[i] != ' ') return s[i];
    return 0;
}

static void perr(Parser *p, int code, int pos, char ch) {
    if (p->err) return;             /* keep the first, most specific error */
    p->err = code;
    p->err_pos = pos;
    p->err_ch = ch;
}

/* Every message names the problem, not just "syntax error". */
static void err_text(const Parser *p, char *msg, size_t n) {
    switch (p->err) {
        case E_CHAR:
            if (p->err_ch > 32 && p->err_ch < 127)
                snprintf(msg, n, "garbage character '%c' at %d", p->err_ch,
                         p->err_pos + 1);
            else
                snprintf(msg, n, "garbage byte 0x%02X at %d",
                         (unsigned char)p->err_ch, p->err_pos + 1);
            break;
        case E_NAME:
            snprintf(msg, n, "unknown name '%s' - only 'ans' is known",
                     p->err_name);
            break;
        case E_NOANS:
            snprintf(msg, n, "'ans' has no value yet - evaluate something");
            break;
        case E_NUMBER:
            if (p->err_ch == 0)
                snprintf(msg, n, "number too long at %d", p->err_pos + 1);
            else
                snprintf(msg, n, "malformed number at %d", p->err_pos + 1);
            break;
        case E_TRAIL_OP:
            if (p->err_ch)
                snprintf(msg, n, "trailing operator '%c' - nothing after it",
                         p->err_ch);
            else
                snprintf(msg, n, "trailing operator - the expression ends on one");
            break;
        case E_OPERAND:
            snprintf(msg, n, "operator '%c' where a number is expected",
                     p->err_ch);
            break;
        case E_PAREN_OPEN:
            snprintf(msg, n, "unmatched parenthesis: the '(' at %d has no ')'",
                     p->err_pos + 1);
            break;
        case E_PAREN_CLOSE:
            snprintf(msg, n, "unmatched parenthesis: the ')' at %d has no '('",
                     p->err_pos + 1);
            break;
        case E_DIV0:
            snprintf(msg, n, "division by zero (the '/' at %d)", p->err_pos + 1);
            break;
        case E_MOD0:
            snprintf(msg, n, "modulo by zero (the '%%' at %d)", p->err_pos + 1);
            break;
        case E_NEST:
            snprintf(msg, n, "nested too deeply - the limit is %d levels",
                     MAX_NEST);
            break;
        case E_RANGE:
            snprintf(msg, n, "the result is out of range");
            break;
        default:
            snprintf(msg, n, "a value belongs at %d, the expression ends there",
                     p->err_pos + 1);
            break;
    }
}

static double parse_expr(Parser *p, int depth);

/* primary := number | '(' expr ')' | 'ans'
 *
 * `depth` is the parenthesis depth; parse_expr() rejects anything above
 * MAX_NEST before it recurses, so this never runs away. */
static double parse_primary(Parser *p, int depth) {
    if (p->err) return 0.0;

    while (p->src[p->pos] == ' ') p->pos++;
    char c = p->src[p->pos];
    int at = p->pos;

    if (c == '(') {
        p->pos++;
        double v = parse_expr(p, depth + 1);
        if (p->err) return 0.0;
        while (p->src[p->pos] == ' ') p->pos++;
        if (p->src[p->pos] == ')') { p->pos++; return v; }
        /* "1+(2*3" - the group was opened and never closed */
        perr(p, E_PAREN_OPEN, at, '(');
        return 0.0;
    }

    if (c == ')') {
        char prev = prev_nonspace(p->src, at);
        /* "(1+)" is really an operator with nothing after it */
        if (is_op(prev)) perr(p, E_TRAIL_OP, at, prev);
        else             perr(p, E_PAREN_CLOSE, at, ')');
        return 0.0;
    }

    if (is_digit(c) || c == '.') {
        int start = p->pos;
        int digits = 0, dots = 0;
        for (;;) {
            char d = p->src[p->pos];
            if (is_digit(d)) { p->pos++; digits++; }
            else if (d == '.') {
                if (dots) { perr(p, E_NUMBER, p->pos, '.'); return 0.0; }
                dots = 1;
                p->pos++;
            } else break;
        }
        if (!digits) { perr(p, E_NUMBER, start, c); return 0.0; }
        int len = p->pos - start;
        if (len >= 24) { perr(p, E_NUMBER, start, 0); return 0.0; }

        char buf[24];
        memcpy(buf, p->src + start, (size_t)len);
        buf[len] = 0;
        return strtod(buf, NULL);
    }

    if (is_alpha(c)) {
        int start = p->pos;
        while (is_alpha(p->src[p->pos])) p->pos++;
        int len = p->pos - start;
        if (len > (int)sizeof(p->err_name) - 1) len = (int)sizeof(p->err_name) - 1;
        memcpy(p->err_name, p->src + start, (size_t)len);
        p->err_name[len] = 0;

        if (p->pos - start == 3 &&
            (!strcmp(p->err_name, "ans") || !strcmp(p->err_name, "ANS"))) {
            if (!p->have_ans) { perr(p, E_NOANS, at, 0); return 0.0; }
            return p->ans;
        }
        perr(p, E_NAME, at, 0);
        return 0.0;
    }

    if (c == 0) {
        char prev = prev_nonspace(p->src, at);
        if (is_op(prev))      perr(p, E_TRAIL_OP, at, prev);
        else if (prev == '(') perr(p, E_EXPECT, at, '(');
        else                  perr(p, E_EXPECT, at, 0);
        return 0.0;
    }

    /* A '*' '/' or '%' here is an operator missing its left operand;
     * anything else is a character this syntax does not use. */
    if (c == '*' || c == '/' || c == '%') perr(p, E_OPERAND, at, c);
    else                                  perr(p, E_CHAR, at, c);
    return 0.0;
}

/* unary := ('+' | '-') unary | primary - and the sign run is counted
 * against the same depth budget as parentheses. */
static double parse_unary(Parser *p, int depth) {
    if (p->err) return 0.0;

    while (p->src[p->pos] == ' ') p->pos++;
    char c = p->src[p->pos];

    if (c == '+' || c == '-') {
        int at = p->pos;
        p->pos++;
        if (depth + 1 >= MAX_NEST) { perr(p, E_NEST, at, c); return 0.0; }
        double v = parse_unary(p, depth + 1);
        if (p->err) return 0.0;
        return c == '-' ? -v : v;
    }
    return parse_primary(p, depth);
}

/* term := unary (('*' | '/' | '%') unary)* - tighter than + and -, and
 * left associative because the accumulator is updated in the loop. */
static double parse_term(Parser *p, int depth) {
    double v = parse_unary(p, depth);
    if (p->err) return 0.0;

    for (;;) {
        while (p->src[p->pos] == ' ') p->pos++;
        char c = p->src[p->pos];
        if (!(c == '*' || c == '/' || c == '%')) return v;

        int at = p->pos;
        p->pos++;
        double rhs = parse_unary(p, depth);
        if (p->err) return 0.0;

        if (c == '*') {
            v *= rhs;
        } else if (c == '/') {
            if (rhs == 0.0) { perr(p, E_DIV0, at, '/'); return 0.0; }
            v /= rhs;
        } else {
            if (rhs == 0.0) { perr(p, E_MOD0, at, '%'); return 0.0; }
            v = fmod(v, rhs);
        }
    }
}

/* expr := term (('+' | '-') term)* */
static double parse_expr(Parser *p, int depth) {
    if (depth >= MAX_NEST) { perr(p, E_NEST, p->pos, 0); return 0.0; }

    double v = parse_term(p, depth);
    if (p->err) return 0.0;

    for (;;) {
        while (p->src[p->pos] == ' ') p->pos++;
        char c = p->src[p->pos];
        if (c != '+' && c != '-') return v;

        p->pos++;
        double rhs = parse_term(p, depth);
        if (p->err) return 0.0;
        v = (c == '+') ? v + rhs : v - rhs;
    }
}

/* Format a result for the display: integers without a decimal point,
 * ordinary magnitudes with up to 10 significant digits, and only very
 * small or very large values in exponent form. */
static void fmt_number(double v, char *out, size_t n) {
    double ip = 0.0;
    if (v == 0.0) { snprintf(out, n, "0"); return; }   /* also -0 */

    double a = v < 0.0 ? -v : v;
    if (a < 1e15 && modf(v, &ip) == 0.0) snprintf(out, n, "%.0f", v);
    else if (a >= 1e15 || a < 1e-9)      snprintf(out, n, "%.6g", v);
    else                                 snprintf(out, n, "%.10g", v);
}

/* Parse one whole expression. Returns 1 and *out on success, or 0 with
 * msg holding the reason. `ans` is the previous result, if there is one. */
static int parser_eval(const char *src, double ans, int have_ans,
                       double *out, char *msg, size_t msgsz) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.ans = ans;
    p.have_ans = have_ans;

    int i = 0;
    while (src[i] == ' ') i++;
    if (src[i] == 0) {
        snprintf(msg, msgsz, "empty expression - type a number first");
        return 0;
    }

    double v = parse_expr(&p, 0);

    /* Anything left over after a complete expression is an error: an
     * extra ')' or a stray character. */
    if (!p.err) {
        while (src[p.pos] == ' ') p.pos++;
        if (src[p.pos] == ')')    perr(&p, E_PAREN_CLOSE, p.pos, ')');
        else if (src[p.pos] != 0) perr(&p, E_CHAR, p.pos, src[p.pos]);
    }
    if (!p.err && !isfinite(v)) perr(&p, E_RANGE, 0, 0);

    if (p.err) { err_text(&p, msg, msgsz); return 0; }

    *out = v;
    return 1;
}
/* ---8<--- end parser ---8<--- */

/* ------------------------------------------------------------
 *  The window
 * ------------------------------------------------------------ */
#define HIST_MAX  5        /* last five evaluated expressions */
#define PAD_N     10       /* nominal paddings: ui_px() applies the scale */
#define BTN_GAP_N  6
#define BTN_PAD_N  5       /* extra height inside a button */
#define HINT_TEXT "Enter evaluates  -  c clears  -  'ans' is the last result"

typedef struct {
    char expr[EXPR_MAX];     /* the fixed, bounded input line */
    int  len;
    char result[RESULT_MAX]; /* formatted result of the last evaluation */
    int  have_value;
    char err[MSG_MAX];       /* error of the last evaluation, "" if none */
    double last;             /* the value 'ans' stands for */
    int  have_ans;
    int  notice;             /* a key was dropped because expr[] was full */

    struct {
        char expr[EXPR_MAX];
        char value[RESULT_MAX];
    } hist[HIST_MAX];        /* [0] is the most recent */
    int nhist;

    int w, h;
} Calc;

/* Real pixel measurements, all derived from the fonts that are actually
 * drawn. layout_calc() is the only place that knows them, so the painter
 * and the hit test can never disagree. */
typedef struct {
    float pad;
    float expr_y, expr_size;      /* top of the display, its nominal size */
    float res_y;                  /* result / error line                 */
    float rule_y;                 /* hairline over the history           */
    float hist_label_y;           /* the word "history"                  */
    float hist_top, hist_h;       /* first row, and one row stride        */
    float btn_y, btn_h, btn_w, btn_gap;
    float hint_y;
    int   btn_n, hist_lines;
    int   show_expr, show_res, show_rule, show_hist, show_hint;
} Layout;

/* One geometry function for drawing *and* hit testing, so a click always
 * lands on the button that was painted. Returns 0 only when the fonts are
 * not loaded, in which case nothing can be measured or drawn. */
static int layout_calc(int w, int h, Layout *L) {
    memset(L, 0, sizeof *L);

    Font *fres = font_get(FONT_MEDIUM, 14);
    Font *frow = font_get(FONT_REGULAR, 11);
    Font *fsm  = font_get(FONT_REGULAR, 10);
    Font *fbtn = font_get(FONT_MEDIUM, 11);
    if (!fres || !frow || !fsm || !fbtn) return 0;

    float frh   = font_height(fres);   /* one result line  */
    float frowh = font_height(frow);   /* one history row  */
    float fsmh  = font_height(fsm);    /* label and hint   */
    float fbth  = font_height(fbtn);   /* button label     */

    float pad = (float)ui_px(PAD_N);
    float g2  = (float)ui_px(2);
    float g4  = (float)ui_px(4);
    float top = (float)ui_px(6);
    L->pad = pad;

    /* The display is the one line that can shrink and still say
     * everything, so its budget is what is left after the result, one
     * button row and the gaps; clamped to a readable 11..20 points. */
    float scale = ui_scale_get();
    if (scale < 0.1f) scale = 1.0f;
    L->btn_h = fbth + (float)ui_px(BTN_PAD_N);
    float budget = (float)h - top - frh - L->btn_h - 2.0f * g4 - g2;
    int nominal = (int)(budget / (1.4f * scale));
    if (nominal < 11) nominal = 11;
    if (nominal > 20) nominal = 20;
    L->expr_size = (float)nominal;

    Font *fbig = font_get(FONT_SEMIBOLD, L->expr_size);
    if (!fbig) return 0;
    float fe = font_height(fbig);

    L->expr_y = top;
    L->res_y  = top + fe + g2;
    float after_res = L->res_y + frh;

    L->show_expr = (w >= ui_px(40) && (float)h >= top + fe);
    L->show_res  = ((float)h >= after_res);

    /* The five buttons are the only mouse control the app has, so they are
     * reserved before the history. First try hint + buttons, then buttons
     * alone; the history only gets what is still free, and the buttons
     * themselves go only when even the smallest display plus the result
     * line cannot share the height with them. */
    L->btn_gap = (float)ui_px(BTN_GAP_N);
    L->btn_w   = ((float)w - 2.0f * pad - 4.0f * L->btn_gap) / 5.0f;

    float label_min = font_width(fbtn, "(") + (float)ui_px(8);
    if (label_min < (float)ui_px(24)) label_min = (float)ui_px(24);

    for (int pass = 0; pass < 2 && L->btn_n == 0; pass++) {
        int want_hint = (pass == 0);
        float bottom = (float)h - g2;

        if (want_hint) {
            float hy = bottom - fsmh;
            if (font_width(fsm, HINT_TEXT) > (float)w - 2.0f * pad) continue;
            if (hy < after_res + g4) continue;
            bottom = hy - g2;
        }
        float by = bottom - L->btn_h;
        if (L->btn_w >= label_min && by >= after_res + g4) {
            L->btn_n = 5;
            L->btn_y = by;
            if (want_hint) {
                L->show_hint = 1;
                L->hint_y    = (float)h - g2 - fsmh;
            }
        }
    }

    /* History gets whatever is left between the result and the buttons. */
    L->rule_y = after_res + g4;
    float rows_top = L->rule_y + g2 + fsmh + g2;
    float rows_bottom = L->btn_n ? L->btn_y - g2
                      : (L->show_hint ? L->hint_y - g2 : (float)h - g2);

    int n = 0;
    if (rows_bottom > rows_top)
        n = (int)((rows_bottom - rows_top) / (frowh + 1.0f));
    if (n > HIST_MAX) n = HIST_MAX;

    if (n > 0) {
        L->show_hist    = 1;
        L->show_rule    = 1;
        L->hist_label_y = L->rule_y + g2;
        L->hist_top     = rows_top;
        L->hist_h       = frowh + 1.0f;
        L->hist_lines   = n;
    }
    return 1;
}

/* Text inside maxw pixels, cut with ".." when it has to be. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!text || !text[0] || maxw < 6.0f) return;

    char buf[MSG_MAX + 8];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 4) len = sizeof(buf) - 4;
    memcpy(buf, text, len);
    buf[len] = 0;

    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }

    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0) {
        buf[cut] = 0;
        if (font_width(f, buf) <= budget) break;
        cut--;
    }
    if (cut == 0) return;
    buf[cut] = '.';
    buf[cut + 1] = '.';
    buf[cut + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* Right aligned text that keeps its *end* visible when it is too wide -
 * the character just typed must never scroll out of sight. Returns the x
 * where the drawn text ends. */
static float draw_tail(Surface *s, Font *f, float right, float y, float maxw,
                       const char *text, u32 color) {
    float tw = font_width(f, text);
    const char *start = text;

    if (tw > maxw) {
        /* Drop characters from the front until the rest fits: the longest
         * suffix, so the character just typed stays on screen. Starting the
         * search at the end would accept the empty string immediately and
         * draw nothing at all. */
        size_t len = strlen(text);
        size_t cut = 0;
        while (cut < len && font_width(f, text + cut) > maxw) cut++;
        start = text + cut;
        tw = font_width(f, start);
    }

    float sx = right - tw;
    if (sx < right - maxw) sx = right - maxw;
    font_draw(s, f, sx, y, start, color);
    return sx + tw;
}

static void hist_add(Calc *c, const char *expr, const char *value) {
    int keep = c->nhist < HIST_MAX ? c->nhist : HIST_MAX - 1;
    for (int i = keep; i > 0; i--) c->hist[i] = c->hist[i - 1];
    snprintf(c->hist[0].expr, sizeof c->hist[0].expr, "%s", expr);
    snprintf(c->hist[0].value, sizeof c->hist[0].value, "%s", value);
    if (c->nhist < HIST_MAX) c->nhist++;
}

static int expr_append(Calc *c, char ch) {
    if (c->len >= EXPR_MAX - 1) return 0;
    c->expr[c->len++] = ch;
    c->expr[c->len] = 0;
    return 1;
}

/* Editing the text makes the previous complaint stale, so it goes. */
static void edit_changed(Calc *c) {
    c->notice = 0;
    c->err[0] = 0;
    c->have_value = 0;
    c->result[0] = 0;
}

static void calc_enter(Calc *c) {
    double v = 0.0;
    char msg[MSG_MAX];

    c->notice = 0;
    if (!parser_eval(c->expr, c->last, c->have_ans, &v, msg, sizeof msg)) {
        snprintf(c->err, sizeof c->err, "%s", msg);
        c->result[0] = 0;
        c->have_value = 0;
        return;
    }

    c->err[0] = 0;
    c->have_ans = 1;
    c->last = v;
    fmt_number(v, c->result, sizeof c->result);
    c->have_value = 1;

    /* The pair goes into the history before the line is cleared, so the
     * expression you just evaluated is the first row under the input. */
    hist_add(c, c->expr, c->result);
    c->len = 0;
    c->expr[0] = 0;
}

/* 'c' clears the entry. The history and 'ans' are kept: they are the
 * calculator's memory, not part of the line being typed. */
static void calc_clear(Calc *c) {
    c->len = 0;
    c->expr[0] = 0;
    c->notice = 0;
    c->err[0] = 0;
    c->result[0] = 0;
    c->have_value = 0;
}

static void calc_init(App *a, int w, int h) {
    Calc *c = calloc(1, sizeof(Calc));
    if (!c) return;              /* the draw/key/click handlers test state */
    c->w = w;
    c->h = h;
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

static void calc_key(App *a, const char *bytes, int n) {
    Calc *c = a->state;
    if (!c || !bytes) return;

    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)bytes[i];

        if (ch == 0x1b) {                 /* arrow / Home / End report runs */
            int skip = 1;
            if (i + 1 < n && bytes[i + 1] == '[') {
                skip = 3;
                if (i + 3 < n && bytes[i + 3] == '~') skip = 4;
            }
            i += skip - 1;
            continue;
        }

        if (ch == '\n' || ch == '\r' || ch == '=') { calc_enter(c); continue; }

        if (ch == '\b' || ch == 0x7f) {
            if (c->len > 0) {
                c->len--;
                c->expr[c->len] = 0;
            }
            edit_changed(c);
            continue;
        }

        if (ch == 'c' || ch == 'C') { calc_clear(c); continue; }

        if (ch == ' ') continue;          /* spacing is not part of the line */

        if (is_digit((char)ch) || ch == '.' || is_op((char)ch) ||
            ch == '(' || ch == ')' || is_alpha((char)ch)) {
            if (!expr_append(c, (char)ch)) {
                c->notice = 1;            /* the line is full; say so */
                continue;
            }
            edit_changed(c);
        }
        /* any other byte is ignored: the display is a calculator line,
         * not a text field, so there is nothing to complain about */
    }
}

static void calc_click(App *a, int x, int y, int buttons) {
    Calc *c = a->state;
    if (!c) return;
    if (buttons && !(buttons & 1)) return;      /* left button only */
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;

    Layout L;
    if (!layout_calc(c->w, c->h, &L)) return;
    if (L.btn_n == 0) return;
    if ((float)y < L.btn_y || (float)y >= L.btn_y + L.btn_h) return;

    for (int i = 0; i < L.btn_n; i++) {
        float bx = L.pad + (float)i * (L.btn_w + L.btn_gap);
        if ((float)x < bx || (float)x >= bx + L.btn_w) continue;

        switch (i) {
            case 0: calc_clear(c); break;                    /* C  */
            case 1:                                          /* (  */
            case 2:                                          /* )  */
            case 3:                                          /* /  */
                if (expr_append(c, "()/"[i - 1])) edit_changed(c);
                else c->notice = 1;
                break;
            default: calc_enter(c); break;                   /* =  */
        }
        return;
    }
}

static void calc_draw(App *a, Surface *s, int x, int y, int w, int h,
                      const Theme *th) {
    Calc *c = a->state;
    if (!c || !s || !th) return;
    c->w = w;
    c->h = h;

    Layout L;
    if (!layout_calc(w, h, &L) || !L.show_expr) return;

    Font *fbig = font_get(FONT_SEMIBOLD, L.expr_size);
    Font *fres = font_get(FONT_MEDIUM, 14);
    Font *frow = font_get(FONT_REGULAR, 11);
    Font *fsm  = font_get(FONT_REGULAR, 10);
    Font *fbtn = font_get(FONT_MEDIUM, 11);
    if (!fbig || !fres || !frow || !fsm || !fbtn) return;

    float left  = (float)x + L.pad;
    float right = (float)(x + w) - L.pad;
    float maxw  = right - left;
    float fe    = font_height(fbig);
    float fres_h = font_height(fres);
    float fsm_h  = font_height(fsm);

    /* ---- the display: right aligned, caret at the right edge ---- */
    float caret_w = (float)ui_px(2);
    float text_right = right - caret_w - (float)ui_px(3);
    if (text_right < left + 1.0f) text_right = right;

    if (c->len == 0) {
        draw_tail(s, fbig, text_right, (float)y + L.expr_y, text_right - left, "0",
                  color_alpha(th->text, 0.28f));
    } else {
        draw_tail(s, fbig, text_right, (float)y + L.expr_y, text_right - left,
                  c->expr, th->text);
    }
    /* The caret has its own column at the right edge: the text is aligned
     * just left of it, so it is always painted where typing continues. */
    if (right - caret_w > left) {
        float cy  = (float)y + L.expr_y + fe * 0.14f;
        float chh = fe * 0.72f;
        if (chh < 3.0f) chh = 3.0f;
        surf_blend(s, (int)(right - caret_w), (int)cy, (int)caret_w, (int)chh,
                   color_alpha(th->accent, 0.75f));
    }

    /* ---- result, or the reason there is none ---- */
    if (L.show_res) {
        float ry = (float)y + L.res_y;

        /* The ans badge is an extra. It never takes width from an error
         * message - the named error is required output - and it is only
         * drawn when a useful amount of room is left for the message. */
        const char *badge = NULL;
        char ansbuf[RESULT_MAX + 8], val[RESULT_MAX];
        float bw = 0.0f;
        if (c->have_ans) {
            fmt_number(c->last, val, sizeof val);
            snprintf(ansbuf, sizeof ansbuf, "ans %s", val);
            bw = font_width(fsm, ansbuf);
            /* Only the room that is actually left decides: at 350 px the
             * badge fits at every scale, and the gate is the message width,
             * not a second fixed window width. */
            if (!c->err[0] &&
                bw + (float)ui_px(90) <= maxw)
                badge = ansbuf;
        }
        float msgw = badge ? maxw - bw - (float)ui_px(8) : maxw;

        if (c->err[0]) {
            draw_clipped(s, fres, left, ry, msgw, c->err, th->accent);
        } else if (c->have_value) {
            char line[RESULT_MAX + 4];
            snprintf(line, sizeof line, "= %s", c->result);
            draw_clipped(s, fres, left, ry, msgw, line, th->accent);
        } else if (c->notice) {
            draw_clipped(s, fsm, left, ry + (fres_h - fsm_h) * 0.5f, msgw,
                         "the line is full - 63 characters", th->text_dim);
        }

        if (badge)
            font_draw(s, fsm, right - bw, ry + (fres_h - fsm_h) * 0.5f, badge,
                      color_alpha(th->text, 0.40f));
    }

    if (L.show_rule)
        surf_blend(s, (int)left, (int)((float)y + L.rule_y), (int)maxw, 1,
                   th->panel_border);

    /* ---- history: last five, most recent first ---- */
    if (L.show_hist) {
        font_draw(s, fsm, left, (float)y + L.hist_label_y, "history",
                  color_alpha(th->text, 0.40f));

        int lines = c->nhist < L.hist_lines ? c->nhist : L.hist_lines;
        if (lines == 0)
            draw_clipped(s, frow, left, (float)y + L.hist_top, maxw,
                         "no results yet", color_alpha(th->text, 0.30f));
        for (int i = 0; i < lines; i++) {
            float hy = (float)y + L.hist_top + (float)i * L.hist_h;
            char vb[RESULT_MAX + 4];
            snprintf(vb, sizeof vb, "= %s", c->hist[i].value);
            float vw = font_width(frow, vb);
            float room = maxw - vw - (float)ui_px(10);
            if (room < (float)ui_px(40)) {
                draw_clipped(s, frow, left, hy, maxw, c->hist[i].expr,
                             color_alpha(th->text, 0.55f));
                continue;
            }
            font_draw(s, frow, right - vw, hy, vb,
                      color_alpha(th->accent, 0.80f));
            draw_clipped(s, frow, left, hy, room, c->hist[i].expr,
                         color_alpha(th->text, 0.55f));
        }
    }

    /* ---- the five buttons ---- */
    if (L.btn_n > 0) {
        static const char *LABEL[5] = { "C", "(", ")", "/", "=" };
        float fbth = font_height(fbtn);
        for (int i = 0; i < L.btn_n; i++) {
            float bx = (float)x + L.pad + (float)i * (L.btn_w + L.btn_gap);
            float by = (float)y + L.btn_y;
            int accent = (i == 4);

            gfx_rrect_aa(s, bx, by, L.btn_w, L.btn_h, (float)ui_px(6),
                         accent ? color_alpha(th->accent, 0.22f)
                                : color_alpha(th->text, 0.07f),
                         accent ? th->accent : th->panel_border,
                         accent ? 1.4f : 1.0f);

            float lw = font_width(fbtn, LABEL[i]);
            font_draw(s, fbtn, bx + (L.btn_w - lw) * 0.5f,
                      by + (L.btn_h - fbth) * 0.5f, LABEL[i],
                      accent ? th->accent : th->text);
        }
    }

    if (L.show_hint) {
        float hw = font_width(fsm, HINT_TEXT);
        if (hw <= (float)w - 2.0f * L.pad)
            font_draw(s, fsm, (float)x + ((float)w - hw) * 0.5f,
                      (float)y + L.hint_y, HINT_TEXT,
                      color_alpha(th->text, 0.34f));
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
