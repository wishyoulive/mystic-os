/* ============================================================
 *  Mystic OS - css.h
 *  The slice of CSS a text browser needs, and nothing else.
 *
 *  A stylesheet arrives as bytes: from a <style> element, or from a
 *  .css file the browser fetched. css_parse() turns those bytes into
 *  a fixed array of rules. Nothing here allocates, nothing here reads
 *  a clock, a file or /proc: every byte that decides a style comes
 *  from the text the caller passes in.
 *
 *      CssSheet sheet;
 *      css_sheet_init(&sheet);
 *      css_parse(style_block, style_len, &sheet);   // <style> #1
 *      css_parse(more_css,    more_len,    &sheet); // <style> #2, appended
 *      css_parse(fetched,     fetched_len, &sheet); // <link rel=stylesheet>
 *
 *  css_for() then answers the only question layout asks, "what does
 *  this element look like?", resolving in this order:
 *
 *      1. inherited text properties from the enclosing block
 *      2. the built-in HTML sheet + tag defaults (css_defaults())
 *      3. presentational hints (align=, nowrap, <font> etc.)
 *      4. the author's rules: id > class > tag, later wins on a tie
 *
 *  The element's ancestors are supplied as a stack of CssNode, which
 *  is what makes descendant selectors ("div.note p") and inheritance
 *  work. css_node_init() fills one node and resolves its style:
 *
 *      CssNode html, body, div, p;
 *      css_node_init(&html, "html", NULL,    NULL, 0, NULL,  &sheet);
 *      css_node_init(&body, "body", NULL,    NULL, 0, &html, &sheet);
 *      css_node_init(&div,  "div",  "note",  NULL, 0, &body, &sheet);
 *      css_node_init(&p,    "p",    NULL,    "lead", 0, &div, &sheet);
 *      // p.style is the result of `div.note p`, with body's text
 *      // properties inherited through div and p.
 *
 *  WHAT IS IN THE SUBSET
 *
 *  Selectors:  tag, .class, #id, universal *, any of them on one
 *              compound, and descendant combinations of up to
 *              CSS_MAX_SEL_PARTS compounds. A comma-separated group
 *              is split into one rule per selector. Pseudo-classes,
 *              attribute selectors and the child/sibling combinators
 *              are outside the subset: that selector is dropped (and
 *              counted), the rest of the rule and the sheet survive.
 *
 *  Properties: color, background-color  (#rgb #rgba #rrggbb #rrggbbaa,
 *              rgb(), rgba(), the 148 CSS names, transparent)
 *              font-weight, font-style, text-decoration, text-align,
 *              margin-top, margin-bottom, padding-top, padding-bottom,
 *              border-top, border-bottom (and the -width/-style/-color
 *              longhands), width, max-width, line-height,
 *              list-style-type, white-space.
 *
 *              width/max-width take px or %; line-height takes px, a
 *              unitless multiplier or a percentage. The renderer treats
 *              every CSS px as a design pixel and scales it with the
 *              desktop's text scale, the same way it treats margins.
 *
 *  At-rules:   @media, @import, @charset, @font-face, @supports and any
 *              other @... are skipped as a unit, including a balanced
 *              { ... } block and strings inside it. Nothing in them is
 *              applied, and none of them is an error.
 *
 *  WHAT IS DELIBERATELY NOT A CSS PROPERTY HERE
 *
 *  The text browser also needs three presentation facts that this
 *  property set has no syntax for. They are part of the built-in
 *  document defaults, in a tag table beside the default sheet:
 *
 *      font_size   h1..h6 (24, 20, 17, 15, 13, 12 px, the same sizes
 *                  web.c uses) and the 12 px base; inherited
 *      indent      ul, ol, menu, dir, blockquote, dd: +24 px each
 *                  nesting level; inherited and cumulative
 *      monospace   pre, code, kbd, samp, tt
 *
 *  An author sheet cannot change those, because they are outside the
 *  property set; that is stated here rather than hidden. Everything
 *  an author *can* write is handled by the same parser that reads the
 *  built-in sheet, so the defaults are real CSS, not a second code path.
 *
 *  MEMORY
 *
 *  A CssSheet is CSS_MAX_RULES rules, fixed size; sizeof(CssSheet) is
 *  around 95 KB. Put it in the browser's document state (static or
 *  heap), not in a small thread stack. css_for() itself allocates
 *  nothing and has no static mutable state except the one-time parse
 *  of the built-in sheet, which is safe because Mystic has no threads.
 * ============================================================ */
#ifndef MYSTIC_CSS_H
#define MYSTIC_CSS_H

#include <stddef.h>

#include "mystic.h"

#ifndef CSS_API
#define CSS_API
#endif

/* ------------------------------------------------------------
 *  Limits. All compile-time, all visible, all enforced.
 * ------------------------------------------------------------ */
#define CSS_MAX_RULES       256   /* one rule per selector of a group */
#define CSS_MAX_SEL_PARTS   4     /* "div.note p a" has three         */
#define CSS_TAG_MAX         16
#define CSS_CLASS_MAX       24
#define CSS_ID_MAX          24
#define CSS_WARN_MAX        96    /* one line of stats.first_problem  */

/* ------------------------------------------------------------
 *  The properties a rule can set. CssStyle.set is a mask of these,
 *  so a caller can tell "this element is 0 px from a rule" apart
 *  from "this element is 0 px because nothing said otherwise".
 *
 *  CSS_SET_FONT_SIZE, CSS_SET_MONO and CSS_SET_INDENT are only ever
 *  set by the built-in tag table (see the top of this file); the
 *  parser has no syntax that produces them.
 * ------------------------------------------------------------ */
enum {
    CSS_SET_COLOR           = 1u << 0,
    CSS_SET_BACKGROUND      = 1u << 1,
    CSS_SET_FONT_SIZE       = 1u << 2,
    CSS_SET_WEIGHT          = 1u << 3,
    CSS_SET_ITALIC          = 1u << 4,
    CSS_SET_DECORATION      = 1u << 5,
    CSS_SET_ALIGN           = 1u << 6,
    CSS_SET_MARGIN_TOP      = 1u << 7,
    CSS_SET_MARGIN_BOTTOM   = 1u << 8,
    CSS_SET_PAD_TOP         = 1u << 9,
    CSS_SET_PAD_BOTTOM      = 1u << 10,
    CSS_SET_BORDER_TOP_W    = 1u << 11,
    CSS_SET_BORDER_TOP_S    = 1u << 12,
    CSS_SET_BORDER_TOP_C    = 1u << 13,
    CSS_SET_BORDER_BOTTOM_W = 1u << 14,
    CSS_SET_BORDER_BOTTOM_S = 1u << 15,
    CSS_SET_BORDER_BOTTOM_C = 1u << 16,
    CSS_SET_LIST_STYLE      = 1u << 17,
    CSS_SET_WHITE_SPACE     = 1u << 18,
    CSS_SET_MONO            = 1u << 19,
    CSS_SET_INDENT          = 1u << 20,
    CSS_SET_WIDTH           = 1u << 21,
    CSS_SET_MAX_WIDTH       = 1u << 22,
    CSS_SET_LINE_HEIGHT     = 1u << 23
};

/* text-align */
enum {
    CSS_ALIGN_LEFT = 0,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_RIGHT,
    CSS_ALIGN_JUSTIFY
};

/* text-decoration is a bit set: `underline line-through` is legal CSS. */
enum {
    CSS_TD_NONE         = 0,
    CSS_TD_UNDERLINE    = 1 << 0,
    CSS_TD_LINE_THROUGH = 1 << 1,
    CSS_TD_OVERLINE     = 1 << 2
};

/* border style. CSS's groove/ridge/inset/outset are drawn as solid. */
enum {
    CSS_BORDER_NONE = 0,
    CSS_BORDER_SOLID,
    CSS_BORDER_DASHED,
    CSS_BORDER_DOTTED,
    CSS_BORDER_DOUBLE
};

/* list-style-type */
enum {
    CSS_LIST_NONE = 0,
    CSS_LIST_DISC,
    CSS_LIST_CIRCLE,
    CSS_LIST_SQUARE,
    CSS_LIST_DECIMAL,
    CSS_LIST_LOWER_ALPHA,
    CSS_LIST_UPPER_ALPHA,
    CSS_LIST_LOWER_ROMAN,
    CSS_LIST_UPPER_ROMAN
};

/* white-space */
enum {
    CSS_WS_NORMAL = 0,
    CSS_WS_PRE,
    CSS_WS_NOWRAP,
    CSS_WS_PRE_WRAP,
    CSS_WS_PRE_LINE
};

#define CSS_WEIGHT_NORMAL  400
#define CSS_WEIGHT_BOLD    700
/* Internal sentinels for font-weight: bolder/lighter, which depend on
 * the inherited weight and are therefore resolved in the cascade. */
#define CSS_WEIGHT_BOLDER  (-1)
#define CSS_WEIGHT_LIGHTER (-2)

/* The colour a page has when nothing says otherwise: CSS's initial
 * `color`, opaque black. A browser that wants the desktop theme to
 * show through can pass a synthetic parent CssNode with its own
 * CssStyle (see css_for) instead of starting from this. */
#define CSS_INITIAL_COLOR  0xFF000000u
#define CSS_BASE_FONT_SIZE 12   /* web.c's body size, a real number here */

/* ------------------------------------------------------------
 *  A resolved style.
 *
 *  Colours are 0xAARRGGBB like the rest of Mystic. CSS writes alpha
 *  LAST (#rrggbbaa); the parser converts on the way in, so no caller
 *  has to think about the byte order twice.
 *
 *  background is 0x00000000 ("no background") unless a rule set one;
 *  a 00 alpha from `#rrggbb00` or `transparent` means the same thing.
 *  There is no has_background flag, because "alpha is zero" is the
 *  same answer and cannot disagree with it.
 *
 *  margin/padding/border widths are whole px. Margins may be negative;
 *  padding is clamped at 0. Borders are visible when style != NONE and
 *  width > 0. A border whose colour was never written takes the text
 *  colour (CSS currentColor), filled in by css_for().
 *
 *  width and max_width are px; 0 means auto / no limit. A percentage
 *  is kept in width_pct / max_width_pct instead, because it can only be
 *  resolved against the column the renderer finally has. A width or a
 *  max-width that applies is a constrained column and the renderer
 *  centres it in the available width, which is what makes a page with
 *  `max-width: 960px` read like a page and not like a stretched line.
 *
 *  line_height is the resolved line box in px; 0 means "the face's own
 *  height". line_height_pct is the parsed multiplier in percent (150
 *  for 1.5) and is resolved against font_size by css_for(). line-height
 *  inherits, width and max-width do not.
 *
 *  font_size is px. indentation accumulates through nesting.
 * ------------------------------------------------------------ */
typedef struct {
    unsigned set;             /* CSS_SET_* actually applied to this element */

    u32 color;
    u32 background;
    u32 border_top_color;
    u32 border_bottom_color;

    int font_size;
    int weight;               /* 100..900; >= 600 is the semibold face */
    int italic;
    int decoration;           /* CSS_TD_* bits */
    int align;                /* CSS_ALIGN_* */
    int list_style;           /* CSS_LIST_* */
    int white_space;          /* CSS_WS_* */

    int margin_top, margin_bottom;
    int pad_top, pad_bottom;

    int border_top_w, border_top_style;
    int border_bottom_w, border_bottom_style;

    int width, width_pct;             /* px; 0 = auto; pct 0..1000 */
    int max_width, max_width_pct;     /* px; 0 = none; pct 0..1000 */
    int line_height;                  /* resolved px; 0 = face height */
    int line_height_pct;              /* 0 = unset; 150 = 1.5 */

    int monospace;            /* pre/code/kbd/samp/tt or CSS_ATTR_MONO */
    int indent;               /* px of horizontal indent at this block  */
} CssStyle;

/* ------------------------------------------------------------
 *  Selectors and rules.
 *
 *  One compound is one element: tag, one class, one id, any subset.
 *  An empty compound (the universal selector `*`) matches any element.
 *  part[nparts-1] is the element itself; earlier parts are ancestors,
 *  nearest first.
 * ------------------------------------------------------------ */
typedef struct {
    char tag[CSS_TAG_MAX];
    char cls[CSS_CLASS_MAX];
    char id[CSS_ID_MAX];
} CssCompound;

typedef struct {
    CssCompound part[CSS_MAX_SEL_PARTS];
    int nparts;
    int specificity;          /* 10000*ids + 100*classes + 1*tags */
} CssSelector;

typedef struct {
    CssSelector sel;
    CssStyle    props;        /* only the .set bits are meaningful */
} CssRule;

typedef struct {
    int at_rules;         /* @media/@import/... skipped as a unit */
    int bad_rules;        /* no block, or no selector in the group */
    int bad_selectors;    /* one selector outside the subset        */
    int bad_decls;        /* no colon, or an empty value            */
    int unknown_props;    /* property outside the subset            */
    int bad_values;       /* supported property, unusable value     */
    int truncated;        /* CSS_MAX_RULES reached, rest dropped    */
    int ignored;          /* sum of the six counters above          */
    char first_problem[CSS_WARN_MAX];   /* "line 12: ...", or ""     */
} CssStats;

typedef struct {
    CssRule rule[CSS_MAX_RULES];
    int     nrules;
    CssStats stats;
} CssSheet;

/* ------------------------------------------------------------
 *  Element state for css_for().
 *
 *  `tag`, `classes`, `id` describe this element and are the same
 *  strings the caller passes to css_for(); css_node_init() keeps them
 *  in sync. `up` is the enclosing element, whose `style` must already
 *  be resolved. `attrs` are presentational hints from the HTML
 *  attributes themselves (CSS_ATTR_*), which rank below every author
 *  rule but above the built-in sheet, exactly as HTML's presentational
 *  hints do.
 *
 *  Note that attr_state also carries inheritance: css_for() starts
 *  from up->style, not from the initial values, for every inherited
 *  property. NULL means "this is the root of the document".
 * ------------------------------------------------------------ */
enum {
    CSS_ATTR_BOLD      = 1u << 0,  /* <b>/<strong> seen as a hint      */
    CSS_ATTR_ITALIC    = 1u << 1,  /* <i>/<em>                         */
    CSS_ATTR_UNDERLINE = 1u << 2,  /* <u>/<ins>                        */
    CSS_ATTR_STRIKE    = 1u << 3,  /* <s>/<strike>/<del>               */
    CSS_ATTR_CENTER    = 1u << 4,  /* <center>, align="center"         */
    CSS_ATTR_RIGHT     = 1u << 5,  /* align="right"                    */
    CSS_ATTR_JUSTIFY   = 1u << 6,  /* align="justify"                  */
    CSS_ATTR_NOWRAP    = 1u << 7,  /* nowrap                           */
    CSS_ATTR_MONO      = 1u << 8,  /* <tt>/<code> seen as a hint       */
    CSS_ATTR_PRE       = 1u << 9   /* <pre> seen as a hint             */
};

typedef struct CssNode {
    const char *tag;          /* lower case, as stored in a selector   */
    const char *classes;      /* the raw class attribute, or NULL      */
    const char *id;           /* the raw id attribute, or NULL         */
    unsigned    attrs;         /* CSS_ATTR_* for this element           */
    const struct CssNode *up;  /* enclosing element, or NULL            */
    CssStyle    style;         /* filled by css_node_init               */
} CssNode;

/* ------------------------------------------------------------
 *  The API.
 * ------------------------------------------------------------ */

/* Zero a sheet (or reset it) before the first css_parse(). */
CSS_API void css_sheet_init(CssSheet *sheet);

/* Parse CSS text and APPEND its rules to `sheet`, in document order.
 * A page with three <style> blocks and a fetched sheet calls this four
 * times and gets one cascade. Returns the number of rules added, or
 * -1 if text is NULL or sheet is NULL. Malformed input never makes it
 * fail: the bad rule is counted in sheet->stats and parsing continues
 * with the next one. */
CSS_API int css_parse(const char *text, size_t len, CssSheet *sheet);

/* The built-in HTML sheet: h1..h6, p, ul/ol, li, blockquote, pre, a,
 * b/strong, i/em, u, s/del, hr, th, caption, center, nobr. Parsed from
 * a real CSS string by css_parse() the first time it is needed, and
 * applied by css_for() at UA priority (below any author rule). The
 * returned pointer is read-only and lives for the process. */
CSS_API const CssSheet *css_defaults(void);

/* Resolve one element. `sheet` may be NULL (built-in defaults only).
 * `tag` may be NULL for an unknown element, which is treated as an
 * unstyled inline box (it still inherits). `class_attr` is the whole
 * class attribute ("note wide"); `id_attr` is the id. `attr_state` is
 * this element's CssNode (attributes + ancestors + parent style) and
 * may be NULL at the root. */
CSS_API CssStyle css_for(const CssSheet *sheet, const char *tag,
                         const char *class_attr, const char *id_attr,
                         const CssNode *attr_state);

/* Fill `n` from the arguments and set n->style = css_for(...). The
 * usual way to build the ancestor stack; see the example above. */
CSS_API void css_node_init(CssNode *n, const char *tag, const char *class_attr,
                           const char *id_attr, unsigned attrs,
                           const CssNode *parent, const CssSheet *sheet);

/* Fill a style with the initial values (black text, no background,
 * 12 px, weight 400). css_for() starts from this plus inheritance. */
CSS_API void css_style_init(CssStyle *style);

/* Parse one colour token that must be the WHOLE string: #rgb, #rgba,
 * #rrggbb, #rrggbbaa, rgb()/rgba(), a CSS colour name, or transparent.
 * Returns 1 and stores 0xAARRGGBB, or 0. Useful for <font color>. */
CSS_API int css_color(const char *text, size_t len, u32 *out);

/* Apply declarations from an inline style="..." attribute to an
 * already-resolved style: last declaration wins, no selector, no
 * inheritance. Returns how many declarations took effect, or -1 on a
 * NULL style. Unknown properties are ignored and counted nowhere (an
 * inline attribute has no sheet to record them in). */
CSS_API int css_inline(CssStyle *style, const char *decls, size_t len);

#endif /* MYSTIC_CSS_H */
