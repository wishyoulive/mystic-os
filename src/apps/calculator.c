/* ============================================================
 *  Mystic OS - apps/calculator.c
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
 *  The expression buffer is a fixed 64 bytes. Nothing here allocates.
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
    int   nest;                 /* current recursion depth */
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
#define PAD       10
#define BTN_H     22
#define BTN_GAP    6
#define HIST_H    14

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

typedef struct {
    int expr_y, res_y, rule_y, hist_label_y, hist_top;
    int hist_lines;
    int btn_y, btn_w, btn_n;
    int hint_y;
    int show_expr, show_res, show_rule, show_hist, show_hint;
} Layout;

/* One geometry function for drawing *and* hit testing, so a click always
 * lands on the button that was painted. */
static void layout_calc(int w, int h, Layout *L) {
    memset(L, 0, sizeof *L);

    L->expr_y       = 6;
    L->res_y        = 40;
    L->rule_y       = 62;
    L->hist_label_y = 68;
    L->hist_top     = 84;
    L->hint_y       = h - 15;

    L->show_expr = (w >= 40 && h >= L->expr_y + 30);
    L->show_res  = (h >= L->res_y + 20);
    L->show_rule = (h >= L->rule_y + 6);
    L->show_hist = (h >= L->hist_top + HIST_H);

    /* five buttons above the hint line, never on top of the history */
    int by = h - 15 - 6 - BTN_H;
    int bw = (w - 2 * PAD - 4 * BTN_GAP) / 5;
    if (bw >= 24 && by >= L->hist_top && w >= 5 * 24 + 4 * BTN_GAP + 2 * PAD) {
        L->btn_y = by;
        L->btn_w = bw;
        L->btn_n = 5;
    }

    int bottom = L->btn_n ? L->btn_y - 8 : (h - 15 - 4);
    int n = (bottom - L->hist_top) / HIST_H;
    if (n < 0) n = 0;
    if (n > HIST_MAX) n = HIST_MAX;
    if (!L->show_hist) n = 0;
    L->hist_lines = n;

    L->show_hint = (L->hint_y > L->rule_y + 8 && w >= 200);
}

/* Text inside maxw pixels, cut with ".." when it has to be. */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!text || !text[0] || maxw < 6.0f) return;

    char buf[EXPR_MAX + 8];
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
        size_t cut = strlen(text);
        while (cut > 0 && font_width(f, text + cut) > maxw) cut--;
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
    layout_calc(c->w, c->h, &L);
    if (L.btn_n == 0) return;
    if (y < L.btn_y || y >= L.btn_y + BTN_H) return;

    for (int i = 0; i < L.btn_n; i++) {
        int bx = PAD + i * (L.btn_w + BTN_GAP);
        if (x < bx || x >= bx + L.btn_w) continue;

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
    layout_calc(w, h, &L);
    if (!L.show_expr) return;

    Font *fbig = font_get(FONT_SEMIBOLD, 20);
    Font *fres = font_get(FONT_MEDIUM, 14);
    Font *frow = font_get(FONT_REGULAR, 11);
    Font *fsm  = font_get(FONT_REGULAR, 10);
    Font *fbtn = font_get(FONT_MEDIUM, 11);
    if (!fbig || !fres || !frow || !fsm || !fbtn) return;

    float left  = (float)x + (float)PAD;
    float right = (float)(x + w) - (float)PAD;
    float maxw  = right - left;

    /* ---- the expression, big, right aligned ---- */
    if (c->len == 0) {
        font_draw(s, fbig, left, (float)(y + L.expr_y), "0",
                  color_alpha(th->text, 0.28f));
    } else {
        float endx = draw_tail(s, fbig, right, (float)(y + L.expr_y), maxw,
                               c->expr, th->text);
        /* a caret, so the line visibly ends where typing continues */
        if (endx + 5.0f < right)
            surf_blend(s, (int)(endx + 2.0f), y + L.expr_y + 3, 2, 20,
                       color_alpha(th->accent, 0.75f));
    }

    /* ---- result, or the reason there is none ---- */
    if (L.show_res) {
        float ry = (float)(y + L.res_y);
        if (c->err[0]) {
            draw_clipped(s, fres, left, ry, maxw, c->err, th->accent);
        } else if (c->have_value) {
            char line[RESULT_MAX + 4];
            snprintf(line, sizeof line, "= %s", c->result);
            draw_clipped(s, fres, left, ry, maxw, line, th->accent);
        } else if (c->notice) {
            draw_clipped(s, fsm, left, ry + 3.0f, maxw,
                         "the line is full - 63 characters", th->text_dim);
        }

        /* what 'ans' currently stands for */
        if (c->have_ans && w >= 240) {
            char ansbuf[RESULT_MAX + 8], val[RESULT_MAX];
            fmt_number(c->last, val, sizeof val);
            snprintf(ansbuf, sizeof ansbuf, "ans %s", val);
            float aw = font_width(fsm, ansbuf);
            if (aw + 80.0f < maxw)
                font_draw(s, fsm, right - aw, ry + 3.0f, ansbuf,
                          color_alpha(th->text, 0.40f));
        }
    }

    if (L.show_rule)
        surf_blend(s, x + PAD, y + L.rule_y, w - 2 * PAD, 1, th->panel_border);

    /* ---- history: last five, most recent first ---- */
    if (L.hist_lines > 0) {
        font_draw(s, fsm, left, (float)(y + L.hist_label_y), "history",
                  color_alpha(th->text, 0.40f));

        int lines = c->nhist < L.hist_lines ? c->nhist : L.hist_lines;
        for (int i = 0; i < lines; i++) {
            float hy = (float)(y + L.hist_top) + (float)i * (float)HIST_H;
            float vw = font_width(frow, c->hist[i].value);
            if (vw + 40.0f > maxw) {
                draw_clipped(s, frow, left, hy, maxw, c->hist[i].expr,
                             color_alpha(th->text, 0.55f));
                continue;
            }
            font_draw(s, frow, right - vw, hy, c->hist[i].value,
                      color_alpha(th->accent, 0.80f));
            draw_clipped(s, frow, left, hy, (right - vw - 10.0f) - left,
                         c->hist[i].expr, color_alpha(th->text, 0.55f));
        }
    }

    /* ---- the five buttons ---- */
    if (L.btn_n > 0) {
        static const char *LABEL[5] = { "C", "(", ")", "/", "=" };
        for (int i = 0; i < L.btn_n; i++) {
            float bx = (float)(x + PAD + i * (L.btn_w + BTN_GAP));
            float by = (float)(y + L.btn_y);
            int accent = (i == 4);

            gfx_rrect_aa(s, bx, by, (float)L.btn_w, (float)BTN_H, 6.0f,
                         accent ? color_alpha(th->accent, 0.22f)
                                : color_alpha(th->text, 0.07f),
                         accent ? th->accent : th->panel_border,
                         accent ? 1.4f : 1.0f);

            float lw = font_width(fbtn, LABEL[i]);
            font_draw(s, fbtn, bx + ((float)L.btn_w - lw) * 0.5f, by + 5.0f,
                      LABEL[i], accent ? th->accent : th->text);
        }
    }

    if (L.show_hint) {
        const char *hint = "Enter evaluates  -  c clears  -  'ans' is the last result";
        float hw = font_width(fsm, hint);
        if (hw < (float)w - 8.0f)
            font_draw(s, fsm, (float)x + ((float)w - hw) * 0.5f,
                      (float)(y + L.hint_y), hint, color_alpha(th->text, 0.34f));
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
