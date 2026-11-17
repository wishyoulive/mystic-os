/* ============================================================
 *  Mystic OS - pdf.c
 *  A real, small PDF reader: enough of the format to open a normal
 *  document and put its pages on a Surface, with no fakery.
 *
 *  What is actually implemented
 *  ---------------------------
 *  File structure
 *    - %PDF header and version
 *    - classic xref TABLES, including /Prev chains
 *    - xref STREAMS (/Type /XRef) with /W, /Index, /Prev and the PNG
 *      predictors 10..15 that xref streams normally use
 *    - hybrid files' /XRefStm
 *    - compressed object streams (/Type /ObjStm): every object in one
 *      is resolved on demand, so a PDF written entirely with object
 *      streams opens
 *    - if startxref/xref is broken, the file is rescanned for
 *      "N G obj" headers and the trailer is recovered; the document
 *      records that this happened instead of pretending otherwise
 *    - trailer /Root -> /Pages -> /Kids page tree, with /MediaBox,
 *      /CropBox, /Rotate and /Resources inherited down the tree
 *
 *  Filters
 *    - FlateDecode, with our own inflate (stored, fixed and dynamic
 *      Huffman blocks, zlib header and adler32 checked)
 *    - ASCIIHexDecode, ASCII85Decode, RunLengthDecode
 *    - PNG predictors for Flate (12 is what xref streams use)
 *    - any other filter is refused by name with the stream's byte
 *      offset, so a page that cannot be decoded says exactly why
 *
 *  Content
 *    - text: Tj TJ ' " with the current font and size, Td TD Tm T*
 *      and the Tc Tw Tz TL Ts parameters
 *    - colour: rg RG g G k K, plus cs/sc/scn for DeviceGray/RGB/CMYK
 *    - paths: re m l c v y h, f F f* S s B B* b b* n and w
 *    - state: q Q cm, and rectangular clip paths (W n on a rectangle)
 *    - XObjects: Form XObjects are interpreted with their /Matrix and
 *      /Resources; Image XObjects are counted and reported as skipped
 *    - inline images are stepped over intact (counted, never drawn)
 *
 *  Fonts
 *    - the standard 14 names are mapped onto the fonts the OS ships:
 *      Helvetica/Arial/Times -> Lato, Courier -> JetBrains Mono, and
 *      bold weights pick the bold face.  Advances are measured from
 *      the very TTF that will be drawn, so glyphs and pen positions
 *      agree (see pdf_font_width).
 *    - embedded TrueType/Type1 fonts: the /Widths array is parsed and
 *      used for advances; the embedded program itself is NOT parsed
 *      (the OS already has good TTFs), and the document counts it in
 *      PdfDocStats.embedded_fonts, which the viewer reports.
 *    - /Encoding: WinAnsi by default, /StandardEncoding and
 *      /MacRomanEncoding by name, /Differences with glyph names.
 *      Type0/CID fonts are not interpreted: they are counted and the
 *      viewer says so.
 *
 *  What is deliberately not implemented, and never faked
 *  ------------------------------------------------------
 *    - encryption (the file is refused with /Encrypt and its offset)
 *    - images, JPEG/JPX, shading and transparency groups
 *    - Type0/CID fonts, embedded font programs
 *    - rotated text: glyphs are placed at the transformed origin but
 *      drawn upright; the page-level /Rotate is applied correctly.
 *    - non-rectangular clipping and line dashes
 *    These are reported through PdfDocStats / PdfRenderInfo rather
 *    than drawn as if they had worked.
 *
 *  This file is also embedded directly by src/client/apps/pdfview.c
 *  (define PDF_API static first) so the app bundle stays two files.
 * ============================================================ */
#include "pdf.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PDF_API
#define PDF_API
#endif

/* Hard ceilings. They are limits, not guesses: every one of them has a
 * comment saying what happens when it is reached. */
#define PDF_MAX_OBJECTS   (1 << 20)   /* /Size larger than this is refused */
#define PDF_MAX_DEPTH     48          /* object / page-tree recursion */
#define PDF_MAX_PAGES     4096        /* extra pages are counted, not shown */
#define PDF_MAX_STREAM    (64u << 20) /* one decoded stream */
#define PDF_MAX_CONTENT   (32u << 20) /* one page's concatenated content */
#define PDF_MAX_XREF_SEC  64          /* /Prev chain length */
#define PDF_MAX_PATH      8192        /* points in one path */
#define PDF_MAX_OPERANDS  64          /* operands on the content stack */
#define PDF_MAX_FORM_DEPTH 8          /* nested Form XObjects */
#define PDF_MAX_FONTS     256         /* distinct fonts in one page */

/* ------------------------------------------------------------
 *  Arena: bumps out of 64 KB blocks, freed all at once when the
 *  document closes. PDF objects are small and never resized, which
 *  makes an arena both the smallest and the fastest way to own them.
 * ------------------------------------------------------------ */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used, cap;
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    int oom;
} Arena;

static void ar_init(Arena *a) { a->head = NULL; a->oom = 0; }

static void *ar_alloc(Arena *a, size_t n) {
    n = (n + 7u) & ~(size_t)7u;
    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = 64u * 1024u;
        if (n > cap) cap = n;
        b = calloc(1, sizeof(*b) + cap);
        if (!b) { a->oom = 1; return NULL; }
        b->cap = cap;
        b->next = a->head;
        a->head = b;
    }
    void *p = (u8 *)b + sizeof(*b) + b->used;
    b->used += n;
    return p;
}

static char *ar_strndup(Arena *a, const char *s, size_t n) {
    char *p = ar_alloc(a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *ar_strdup(Arena *a, const char *s) {
    return ar_strndup(a, s, s ? strlen(s) : 0);
}

static void ar_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
}

/* ------------------------------------------------------------
 *  Growable byte buffer. Used for decompressed data and for the
 *  page text extractor.
 * ------------------------------------------------------------ */
typedef struct {
    u8    *p;
    size_t len, cap;
    int    oom;
} Buf;

static void buf_init(Buf *b) { b->p = NULL; b->len = b->cap = 0; b->oom = 0; }

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + extra) {
        if (nc > (1u << 30)) { b->oom = 1; return 0; }
        nc *= 2;
    }
    u8 *np = realloc(b->p, nc);
    if (!np) { b->oom = 1; return 0; }
    b->p = np;
    b->cap = nc;
    return 1;
}

static int buf_put(Buf *b, const void *p, size_t n) {
    if (!n) return 1;
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 1;
}

static int buf_putc(Buf *b, int c) {
    u8 v = (u8)c;
    return buf_put(b, &v, 1);
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ------------------------------------------------------------
 *  inflate (RFC 1951) with the zlib wrapper (RFC 1950).
 *
 *  Written here rather than linked in: the OS image has no zlib and
 *  pulling one in for a reader that needs exactly one algorithm
 *  would be the tail wagging the dog. It handles stored, fixed and
 *  dynamic blocks; distances that reach before the start of the
 *  output, invalid codes and truncated input all fail cleanly.
 * ------------------------------------------------------------ */
typedef struct {
    const u8 *src;
    size_t    len, pos;
    u32       bitbuf;
    int       bitcnt;
    Buf      *out;
    int       fail;
} Inf;

static void inf_put(Inf *z, u8 v) {
    if (!buf_putc(z->out, v)) z->fail = 1;
}

static int inf_bits(Inf *z, int n) {
    while (z->bitcnt < n) {
        if (z->pos >= z->len) { z->fail = 1; return 0; }
        z->bitbuf |= (u32)z->src[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & ((1u << n) - 1u));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

typedef struct {
    short count[16];
    short symbol[288];
} Huff;

/* Canonical Huffman codes, built exactly as RFC 1951 section 3.2.2
 * describes them: count per length, then symbols in order. */
static int huff_build(Huff *h, const u8 *lens, int n) {
    int i, left;
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;              /* no codes at all: legal */
    left = 1;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;                 /* over-subscribed */
    }
    short offs[16];
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (short)i;
    return 0;
}

static int huff_decode(Inf *z, const Huff *h) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        code |= inf_bits(z, 1);
        if (z->fail) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->fail = 1;
    return -1;
}

static const int LBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int LEXT[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const int DBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const int DEXT[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};

static int inf_codes(Inf *z, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(z, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            inf_put(z, (u8)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) { z->fail = 1; return -1; }
            int len = LBASE[sym] + inf_bits(z, LEXT[sym]);
            int ds = huff_decode(z, dist);
            if (ds < 0 || ds >= 30) { z->fail = 1; return -1; }
            size_t back = (size_t)DBASE[ds] + (size_t)inf_bits(z, DEXT[ds]);
            if (back > z->out->len) { z->fail = 1; return -1; }
            while (len--) inf_put(z, z->out->p[z->out->len - back]);
        }
        if (z->fail) return -1;
    }
}

static int inf_stored(Inf *z) {
    z->bitbuf = 0;
    z->bitcnt = 0;
    if (z->pos + 4 > z->len) { z->fail = 1; return -1; }
    unsigned len = z->src[z->pos] | ((unsigned)z->src[z->pos + 1] << 8);
    unsigned nlen = z->src[z->pos + 2] | ((unsigned)z->src[z->pos + 3] << 8);
    z->pos += 4;
    if ((len ^ 0xFFFFu) != nlen) { z->fail = 1; return -1; }
    if (z->pos + len > z->len) { z->fail = 1; return -1; }
    if (!buf_put(z->out, z->src + z->pos, len)) { z->fail = 1; return -1; }
    z->pos += len;
    return 0;
}

static int inf_fixed(Inf *z) {
    u8 lens[288];
    int i;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    Huff lit;
    if (huff_build(&lit, lens, 288) < 0) { z->fail = 1; return -1; }
    u8 dl[30];
    for (i = 0; i < 30; i++) dl[i] = 5;
    Huff dist;
    if (huff_build(&dist, dl, 30) < 0) { z->fail = 1; return -1; }
    return inf_codes(z, &lit, &dist);
}

static const int CLORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inf_dynamic(Inf *z) {
    int hlit = inf_bits(z, 5) + 257;
    int hdist = inf_bits(z, 5) + 1;
    int hclen = inf_bits(z, 4) + 4;
    if (z->fail || hlit > 286 || hdist > 30) { z->fail = 1; return -1; }

    u8 cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) cl[CLORDER[i]] = (u8)inf_bits(z, 3);
    if (z->fail) return -1;
    Huff clh;
    if (huff_build(&clh, cl, 19) < 0) { z->fail = 1; return -1; }

    u8 lens[288 + 30];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(z, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[n++] = (u8)sym;
        } else if (sym == 16) {
            if (n == 0) { z->fail = 1; return -1; }
            int rep = 3 + inf_bits(z, 2);
            u8 prev = lens[n - 1];
            while (rep-- && n < hlit + hdist) lens[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + inf_bits(z, 3);
            while (rep-- && n < hlit + hdist) lens[n++] = 0;
        } else {
            int rep = 11 + inf_bits(z, 7);
            while (rep-- && n < hlit + hdist) lens[n++] = 0;
        }
        if (z->fail) return -1;
    }

    Huff lit, dist;
    if (huff_build(&lit, lens, hlit) < 0) { z->fail = 1; return -1; }
    if (huff_build(&dist, lens + hlit, hdist) < 0) { z->fail = 1; return -1; }
    return inf_codes(z, &lit, &dist);
}

/* Raw deflate. Returns 0 on success and leaves the bytes in out. */
static int inflate_raw(const u8 *src, size_t len, Buf *out) {
    Inf z;
    memset(&z, 0, sizeof(z));
    z.src = src;
    z.len = len;
    z.out = out;

    int last;
    do {
        if (z.pos >= z.len) return -1;
        last = inf_bits(&z, 1);
        int type = inf_bits(&z, 2);
        if (z.fail) return -1;
        int r;
        if (type == 0)      r = inf_stored(&z);
        else if (type == 1) r = inf_fixed(&z);
        else if (type == 2) r = inf_dynamic(&z);
        else return -1;
        if (r < 0 || z.fail) return -1;
    } while (!last);
    return 0;
}

static u32 adler32(const u8 *p, size_t n) {
    u32 a = 1, b = 0;
    while (n) {
        size_t chunk = n < 5552 ? n : 5552;
        for (size_t i = 0; i < chunk; i++) {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

/* FlateDecode payload -> malloc'd bytes. Honours a zlib wrapper when
 * one is present, and (for files written by tools that emit bare
 * deflate) retries without it. `warn` gets an adler32 note, never a
 * silent failure: the bytes are still returned because they decoded. */
static u8 *pdf_inflate(const u8 *src, size_t len, size_t *outlen,
                       char *warn, size_t warnsz, size_t cap) {
    *outlen = 0;
    if (warn && warnsz) warn[0] = 0;
    if (!src || !len) return NULL;

    int zlib = 0;
    if (len >= 2 && (src[0] & 0x0F) == 8 && (((unsigned)src[0] << 8) + src[1]) % 31 == 0) {
        if (src[1] & 0x20) return NULL;          /* preset dictionary: unsupported */
        zlib = 1;
    }

    Buf out;
    buf_init(&out);
    if (inflate_raw(src + (zlib ? 2 : 0), len - (zlib ? 2u : 0u), &out) < 0 || out.oom) {
        /* A bare stream is common enough in the wild; try it before giving up. */
        if (zlib) {
            buf_free(&out);
            buf_init(&out);
            if (inflate_raw(src, len, &out) == 0 && !out.oom) zlib = 0;
            else { buf_free(&out); return NULL; }
        } else {
            buf_free(&out);
            return NULL;
        }
    }
    if (out.len > cap) { buf_free(&out); return NULL; }

    if (zlib && out.len >= 4) {
        const u8 *tail = src + len - 4;
        u32 want = ((u32)tail[0] << 24) | ((u32)tail[1] << 16) |
                   ((u32)tail[2] << 8) | tail[3];
        if (adler32(out.p, out.len) != want) {
            if (warn && warnsz)
                snprintf(warn, warnsz, "adler32 mismatch (kept %lu decoded bytes)",
                         (unsigned long)out.len);
        }
    }

    *outlen = out.len;
    return out.p;                                 /* caller frees */
}

/* ------------------------------------------------------------
 *  The other filters PDF files actually use for text/xref data.
 * ------------------------------------------------------------ */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* <...> ; whitespace ignored; a final odd digit is padded with 0. */
static u8 *ascii_hex_decode(const u8 *src, size_t len, size_t *outlen) {
    Buf b;
    buf_init(&b);
    int hi = -1;
    for (size_t i = 0; i < len; i++) {
        int c = src[i];
        if (c == '>') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0) continue;
        int v = hexval(c);
        if (v < 0) { buf_free(&b); return NULL; }
        if (hi < 0) hi = v;
        else { buf_putc(&b, (hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) buf_putc(&b, hi << 4);
    if (b.oom) { buf_free(&b); return NULL; }
    *outlen = b.len;
    return b.p;
}

/* ASCII85: 'z' is four zero bytes, '~>' ends the stream, the final
 * group of 2..4 characters is padded with 'u' and truncated. */
static u8 *ascii85_decode(const u8 *src, size_t len, size_t *outlen) {
    Buf b;
    buf_init(&b);
    unsigned long acc = 0;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        int c = src[i];
        if (c == '~') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0) continue;
        if (c == 'z' && n == 0) {
            for (int k = 0; k < 4; k++) buf_putc(&b, 0);
            continue;
        }
        if (c < '!' || c > 'u') { buf_free(&b); return NULL; }
        acc = acc * 85 + (unsigned)(c - '!');
        if (++n == 5) {
            for (int k = 3; k >= 0; k--) buf_putc(&b, (int)((acc >> (8 * k)) & 0xFF));
            acc = 0;
            n = 0;
        }
    }
    if (n == 1) { buf_free(&b); return NULL; }
    if (n > 1) {
        int pad = 5 - n;
        for (int k = 0; k < pad; k++) acc = acc * 85 + 84;
        for (int k = 3; k >= 5 - n; k--) buf_putc(&b, (int)((acc >> (8 * k)) & 0xFF));
    }
    if (b.oom) { buf_free(&b); return NULL; }
    *outlen = b.len;
    return b.p;
}

/* RunLengthDecode: 0..127 literal run of n+1, 129..255 repeat, 128 EOD. */
static u8 *runlength_decode(const u8 *src, size_t len, size_t *outlen) {
    Buf b;
    buf_init(&b);
    size_t i = 0;
    while (i < len) {
        int n = src[i++];
        if (n == 128) break;
        if (n < 128) {
            size_t cnt = (size_t)n + 1;
            if (i + cnt > len) { buf_free(&b); return NULL; }
            buf_put(&b, src + i, cnt);
            i += cnt;
        } else {
            if (i >= len) { buf_free(&b); return NULL; }
            u8 v = src[i++];
            size_t cnt = (size_t)257 - (size_t)n;
            for (size_t k = 0; k < cnt; k++) buf_putc(&b, v);
        }
    }
    if (b.oom) { buf_free(&b); return NULL; }
    *outlen = b.len;
    return b.p;
}

/* PNG / TIFF predictors. PDF's predictors are defined on rows of
 * `columns * colors * bpc / 8` bytes. Predictors 10..15 are the PNG
 * ones; each row normally starts with a filter-type byte, but some
 * writers omit it for predictor 10, so both layouts are accepted. */
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int unpredict(Buf *b, int predictor, int columns, int colors, int bpc) {
    if (predictor <= 1) return 1;
    if (columns <= 0 || colors <= 0 || bpc <= 0) return 0;
    if (bpc != 8 && bpc != 16) return 0;
    size_t bpp = (size_t)colors * (size_t)bpc / 8u;   /* bytes per pixel */
    size_t rowlen = (size_t)columns * bpp;
    if (bpp == 0 || rowlen == 0 || rowlen > (1u << 20)) return 0;
    size_t total = b->len;

    if (predictor == 2) {                       /* TIFF, 8-bit components only */
        if (bpc != 8) return 0;
        for (size_t row = 0; row + rowlen <= total; row += rowlen)
            for (size_t i = (size_t)colors; i < rowlen; i++)
                b->p[row + i] = (u8)(b->p[row + i] + b->p[row + i - (size_t)colors]);
        return 1;
    }
    if (predictor < 10 || predictor > 15) return 0;

    size_t rows, stride;
    int has_filter_byte;
    if (total % (rowlen + 1) == 0) {
        rows = total / (rowlen + 1);
        stride = rowlen + 1;
        has_filter_byte = 1;
    } else if (total % rowlen == 0) {
        rows = total / rowlen;
        stride = rowlen;
        has_filter_byte = 0;
    } else {
        return 0;
    }
    if (rows == 0) return 1;

    u8 *out = malloc(total);
    u8 *prev = calloc(1, rowlen);
    if (!out || !prev) { free(out); free(prev); return 0; }

    for (size_t r = 0; r < rows; r++) {
        const u8 *src = b->p + r * stride;
        int ft = has_filter_byte ? src[0] : (predictor - 10);
        if (has_filter_byte) src++;
        u8 *dst = out + r * rowlen;
        for (size_t i = 0; i < rowlen; i++) {
            int x = src[i];
            int a = i >= bpp ? dst[i - bpp] : 0;
            int up = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            switch (ft) {
                case 0: dst[i] = (u8)x; break;
                case 1: dst[i] = (u8)(x + a); break;
                case 2: dst[i] = (u8)(x + up); break;
                case 3: dst[i] = (u8)(x + ((a + up) >> 1)); break;
                case 4: dst[i] = (u8)(x + paeth(a, up, c)); break;
                default: free(out); free(prev); return 0;
            }
        }
        memcpy(prev, dst, rowlen);
    }
    free(b->p);
    b->p = out;
    b->len = rows * rowlen;
    b->cap = total;
    free(prev);
    return 1;
}
/* ------------------------------------------------------------
 *  Objects
 *
 *  A PDF value is one of: null, bool, integer, real, name, string,
 *  reference, array, dictionary, stream.  Everything is allocated
 *  from the document arena, so whole-file teardown is one free().
 * ------------------------------------------------------------ */
typedef enum {
    PV_NULL = 0, PV_BOOL, PV_INT, PV_REAL, PV_NAME, PV_STR,
    PV_REF, PV_ARRAY, PV_DICT, PV_STREAM
} PdfType;

typedef struct PdfVal PdfVal;
typedef struct PdfDictEntry PdfDictEntry;

struct PdfDictEntry {
    char   *key;
    PdfVal *val;
};

struct PdfVal {
    u8 type;
    union {
        int    b;
        long   i;
        double r;
        struct { char *s; } name;
        struct { u8 *s; int len; } str;
        struct { int num, gen; } ref;
        struct { PdfVal **items; int count; } arr;
        struct { PdfDictEntry *e; int count; } dict;
        struct {
            PdfDictEntry *e;
            int    count;
            u8    *raw;        /* bytes as they sit in the file (or ObjStm) */
            size_t rawlen;
            long   raw_off;    /* file offset of the first raw byte */
            u8    *data;       /* decoded bytes, NULL until asked for */
            size_t datalen;
            int    decoded;    /* stream_data() has run */
            int    objstm_counted;
            char   why[160];   /* why decoding failed, exactly */
        } stream;
    } u;
};

typedef struct {
    int  type;   /* 0 free, 1 file offset, 2 object stream */
    long a;      /* offset, or object stream number */
    int  b;      /* generation, or index inside the object stream */
    int  present;
} XEntry;

typedef struct {
    int     present;
    PdfVal *val;
} ObjSlot;

/* Font dictionaries are parsed again for every render, but the document
 * counters must count each one once. The dict pointer is stable for the
 * life of the document, so it is the key. */
typedef struct {
    PdfVal *dict;
    int     bits;
} FontNote;

typedef struct {
    int     num;             /* page object number, 0 for a direct dict */
    PdfVal *dict;
    PdfVal *resources;       /* inherited, may be NULL */
    double  box[4];          /* CropBox if present, else MediaBox */
    int     rotate;
} PdfPage;

struct PdfDoc {
    char    path[512];
    u8     *buf;
    size_t  len;
    Arena   arena;
    Arena   scratch;         /* reset after every render */
    int     vmaj, vmin;

    XEntry *xref;
    int     xref_cap;
    int     xref_size;
    ObjSlot *objs;
    int     obj_count;

    PdfVal *trailer;
    PdfPage *pages;
    int     npages, pages_cap, pages_truncated;
    int     load_state;

    int     reconstructed;
    int     n_xref_streams, n_xref_tables;
    int     n_objstms;
    int     encrypted;
    int     st_embedded_fonts;   /* font dicts carrying a FontFile* */
    int     st_type0_fonts;

    FontNote font_notes[512];
    int      n_font_notes;

    char    title[256];
    char    err[256];
    long    err_off;
};

/* Record one statistic for a font dictionary, once. */
static int font_note(PdfDoc *d, PdfVal *dict, int bit) {
    for (int i = 0; i < d->n_font_notes; i++) {
        if (d->font_notes[i].dict == dict) {
            int fresh = !(d->font_notes[i].bits & bit);
            d->font_notes[i].bits |= bit;
            return fresh;
        }
    }
    if (d->n_font_notes < (int)(sizeof(d->font_notes) / sizeof(d->font_notes[0]))) {
        d->font_notes[d->n_font_notes].dict = dict;
        d->font_notes[d->n_font_notes].bits = bit;
        d->n_font_notes++;
    }
    return 1;
}

/* ------------------------------------------------------------ */

static void pdf_set_err(PdfDoc *d, long off, const char *fmt, ...) {
    if (d->err[0]) return;                        /* keep the first, real cause */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof(d->err), fmt, ap);
    va_end(ap);
    d->err_off = off;
}

static int pdf_is_ws(int c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
static int pdf_is_delim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}
static int pdf_is_regular(int c) {
    return !pdf_is_ws(c) && !pdf_is_delim(c);
}

/* Whitespace and comments. Comments run to the end of the line. */
static void skip_ws(const u8 *b, size_t len, size_t *pos) {
    while (*pos < len) {
        int c = b[*pos];
        if (pdf_is_ws(c)) { (*pos)++; continue; }
        if (c == '%') {
            while (*pos < len && b[*pos] != '\n' && b[*pos] != '\r') (*pos)++;
            continue;
        }
        break;
    }
}

static int at_word(const u8 *b, size_t len, size_t pos, const char *w) {
    size_t n = strlen(w);
    if (pos + n > len) return 0;
    if (memcmp(b + pos, w, n) != 0) return 0;
    if (pos + n < len && pdf_is_regular(b[pos + n])) return 0;
    return 1;
}

/* A long with a conservative ceiling: object numbers and offsets in a
 * PDF are far below 2^62, and refusing bigger ones keeps the arithmetic
 * in parse_int_at from wrapping. */
#define LONG_MAX_HINT 2147483647L

static int parse_int_at(const u8 *b, size_t len, size_t *pos, long *out) {
    skip_ws(b, len, pos);
    size_t p = *pos;
    int neg = 0;
    if (p < len && (b[p] == '+' || b[p] == '-')) { neg = b[p] == '-'; p++; }
    size_t start = p;
    long v = 0;
    while (p < len && b[p] >= '0' && b[p] <= '9') {
        if (v > (LONG_MAX_HINT - 9) / 10) return 0;
        v = v * 10 + (b[p] - '0');
        p++;
    }
    if (p == start) return 0;
    *out = neg ? -v : v;
    *pos = p;
    return 1;
}

static int parse_number_at(const u8 *b, size_t len, size_t *pos, double *out, int *is_int) {
    skip_ws(b, len, pos);
    size_t p = *pos;
    int neg = 0;
    if (p < len && (b[p] == '+' || b[p] == '-')) { neg = b[p] == '-'; p++; }
    size_t digits = 0;
    double v = 0.0;
    int intish = 1;
    while (p < len && b[p] >= '0' && b[p] <= '9') {
        v = v * 10.0 + (b[p] - '0');
        p++;
        digits++;
    }
    if (p < len && b[p] == '.') {
        intish = 0;
        p++;
        double scale = 0.1;
        while (p < len && b[p] >= '0' && b[p] <= '9') {
            v += (b[p] - '0') * scale;
            scale *= 0.1;
            p++;
            digits++;
        }
    }
    if (!digits) return 0;
    *out = neg ? -v : v;
    *is_int = intish;
    *pos = p;
    return 1;
}

static PdfVal *val_new(PdfDoc *d, Arena *ar, PdfType t) {
    PdfVal *v = ar_alloc(ar, sizeof(*v));
    if (!v) { pdf_set_err(d, 0, "out of memory for a PDF object"); return NULL; }
    memset(v, 0, sizeof(*v));
    v->type = (u8)t;
    return v;
}

/* /Name with #XX escapes. */
static char *parse_name_raw(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos) {
    if (*pos >= len || b[*pos] != '/') return NULL;
    (*pos)++;
    Buf tmp;
    buf_init(&tmp);
    while (*pos < len && pdf_is_regular(b[*pos])) {
        int c = b[*pos];
        if (c == '#' && *pos + 2 < len) {
            int hi = hexval(b[*pos + 1]), lo = hexval(b[*pos + 2]);
            if (hi >= 0 && lo >= 0) {
                buf_putc(&tmp, (hi << 4) | lo);
                *pos += 3;
                continue;
            }
        }
        buf_putc(&tmp, c);
        (*pos)++;
    }
    char *s = ar_strndup(ar, tmp.p ? (char *)tmp.p : "", tmp.len);
    buf_free(&tmp);
    if (!s) pdf_set_err(d, (long)*pos, "out of memory for a name");
    return s;
}

/* Literal string: escapes, octal, nested parentheses, line continuation. */
static u8 *parse_string_raw(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int *outlen) {
    if (*pos >= len || b[*pos] != '(') return NULL;
    (*pos)++;
    Buf tmp;
    buf_init(&tmp);
    int depth = 1;
    while (*pos < len && depth > 0) {
        int c = b[*pos];
        if (c == '\\') {
            (*pos)++;
            if (*pos >= len) break;
            int e = b[*pos];
            switch (e) {
                case 'n': buf_putc(&tmp, '\n'); (*pos)++; break;
                case 'r': buf_putc(&tmp, '\r'); (*pos)++; break;
                case 't': buf_putc(&tmp, '\t'); (*pos)++; break;
                case 'b': buf_putc(&tmp, '\b'); (*pos)++; break;
                case 'f': buf_putc(&tmp, '\f'); (*pos)++; break;
                case '(': case ')': case '\\': buf_putc(&tmp, e); (*pos)++; break;
                case '\r': (*pos)++; if (*pos < len && b[*pos] == '\n') (*pos)++; break;
                case '\n': (*pos)++; break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = 0, n = 0;
                        while (n < 3 && *pos < len && b[*pos] >= '0' && b[*pos] <= '7') {
                            v = v * 8 + (b[*pos] - '0');
                            (*pos)++;
                            n++;
                        }
                        buf_putc(&tmp, v & 0xFF);
                    } else {
                        buf_putc(&tmp, e);
                        (*pos)++;
                    }
                    break;
            }
            continue;
        }
        if (c == '(') { depth++; buf_putc(&tmp, c); (*pos)++; continue; }
        if (c == ')') {
            depth--;
            (*pos)++;
            if (depth == 0) break;
            buf_putc(&tmp, c);
            continue;
        }
        buf_putc(&tmp, c);
        (*pos)++;
    }
    if (depth != 0) { buf_free(&tmp); return NULL; }
    u8 *s = (u8 *)ar_alloc(ar, tmp.len + 1);
    if (!s) { buf_free(&tmp); pdf_set_err(d, (long)*pos, "out of memory for a string"); return NULL; }
    if (tmp.len) memcpy(s, tmp.p, tmp.len);
    s[tmp.len] = 0;
    *outlen = (int)tmp.len;
    buf_free(&tmp);
    return s;
}

static u8 *parse_hexstring_raw(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int *outlen) {
    if (*pos >= len || b[*pos] != '<' || (*pos + 1 < len && b[*pos + 1] == '<')) return NULL;
    (*pos)++;
    Buf tmp;
    buf_init(&tmp);
    int hi = -1;
    while (*pos < len && b[*pos] != '>') {
        int c = b[*pos];
        int v = hexval(c);
        if (v >= 0) {
            if (hi < 0) hi = v;
            else { buf_putc(&tmp, (hi << 4) | v); hi = -1; }
        }
        (*pos)++;
    }
    if (*pos >= len) { buf_free(&tmp); return NULL; }
    (*pos)++;
    if (hi >= 0) buf_putc(&tmp, hi << 4);
    u8 *s = (u8 *)ar_alloc(ar, tmp.len + 1);
    if (!s) { buf_free(&tmp); pdf_set_err(d, (long)*pos, "out of memory for a string"); return NULL; }
    if (tmp.len) memcpy(s, tmp.p, tmp.len);
    s[tmp.len] = 0;
    *outlen = (int)tmp.len;
    buf_free(&tmp);
    return s;
}

static PdfVal *parse_value(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int depth);

/* "12 0 R" is an indirect reference, and it can appear wherever a value
 * can: as a dictionary value, an array element, or a whole object. The
 * two integers are followed by the R keyword, which is only a keyword in
 * this position, so the lookahead is bounded and safe. */
static PdfVal *maybe_ref(PdfDoc *d, Arena *ar, const u8 *b, size_t len,
                         size_t *pos, PdfVal *v) {
    if (!v || v->type != PV_INT) return v;
    size_t save = *pos;
    size_t p = *pos;
    skip_ws(b, len, &p);
    long gen = 0;
    if (!parse_int_at(b, len, &p, &gen)) { *pos = save; return v; }
    skip_ws(b, len, &p);
    if (!at_word(b, len, p, "R")) { *pos = save; return v; }
    *pos = p + 1;
    PdfVal *ref = val_new(d, ar, PV_REF);
    if (!ref) return NULL;
    ref->u.ref.num = (int)v->u.i;
    ref->u.ref.gen = (int)gen;
    return ref;
}

/* `[ ... ]` of arbitrary values, with references folded by maybe_ref. */
static PdfVal *parse_array(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int depth) {
    (*pos)++;                                     /* '[' */
    PdfVal **items = NULL;
    int n = 0, cap = 0;
    PdfVal *arr = NULL;

    for (;;) {
        skip_ws(b, len, pos);
        if (*pos >= len) { pdf_set_err(d, (long)*pos, "array is not terminated"); goto fail; }
        if (b[*pos] == ']') { (*pos)++; break; }

        PdfVal *v = parse_value(d, ar, b, len, pos, depth + 1);
        if (!v) goto fail;
        v = maybe_ref(d, ar, b, len, pos, v);
        if (!v) goto fail;

        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            PdfVal **ni = realloc(items, (size_t)nc * sizeof(*ni));
            if (!ni) { pdf_set_err(d, (long)*pos, "out of memory for an array"); goto fail; }
            items = ni;
            cap = nc;
        }
        items[n++] = v;
    }

    arr = val_new(d, ar, PV_ARRAY);
    if (!arr) goto fail;
    arr->u.arr.count = n;
    arr->u.arr.items = ar_alloc(ar, (size_t)(n ? n : 1) * sizeof(PdfVal *));
    if (!arr->u.arr.items) { pdf_set_err(d, (long)*pos, "out of memory for an array"); goto fail; }
    for (int i = 0; i < n; i++) arr->u.arr.items[i] = items[i];
    free(items);
    return arr;

fail:
    free(items);
    return NULL;
}

static PdfVal *parse_dict(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int depth) {
    (*pos) += 2;                                  /* '<<' */
    PdfDictEntry *e = NULL;
    int n = 0, cap = 0;
    PdfVal *dict = NULL;

    for (;;) {
        skip_ws(b, len, pos);
        if (*pos + 1 < len && b[*pos] == '>' && b[*pos + 1] == '>') { (*pos) += 2; break; }
        if (*pos >= len) { pdf_set_err(d, (long)*pos, "dictionary is not terminated"); goto fail; }
        if (b[*pos] != '/') { pdf_set_err(d, (long)*pos, "dictionary key is not a name"); goto fail; }

        char *key = parse_name_raw(d, ar, b, len, pos);
        if (!key) goto fail;
        PdfVal *v = parse_value(d, ar, b, len, pos, depth + 1);
        if (!v) goto fail;
        v = maybe_ref(d, ar, b, len, pos, v);
        if (!v) goto fail;

        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            PdfDictEntry *ne = realloc(e, (size_t)nc * sizeof(*ne));
            if (!ne) { pdf_set_err(d, (long)*pos, "out of memory for a dictionary"); goto fail; }
            e = ne;
            cap = nc;
        }
        e[n].key = key;
        e[n].val = v;
        n++;
    }

    dict = val_new(d, ar, PV_DICT);
    if (!dict) goto fail;
    dict->u.dict.count = n;
    dict->u.dict.e = ar_alloc(ar, (size_t)(n ? n : 1) * sizeof(PdfDictEntry));
    if (!dict->u.dict.e) { pdf_set_err(d, (long)*pos, "out of memory for a dictionary"); goto fail; }
    for (int i = 0; i < n; i++) dict->u.dict.e[i] = e[i];
    free(e);
    return dict;

fail:
    free(e);
    return NULL;
}

static PdfVal *parse_value(PdfDoc *d, Arena *ar, const u8 *b, size_t len, size_t *pos, int depth) {
    if (depth > PDF_MAX_DEPTH) { pdf_set_err(d, (long)*pos, "objects nest deeper than %d", PDF_MAX_DEPTH); return NULL; }
    skip_ws(b, len, pos);
    if (*pos >= len) { pdf_set_err(d, (long)*pos, "unexpected end of data"); return NULL; }

    int c = b[*pos];
    if (c == '[') return parse_array(d, ar, b, len, pos, depth);
    if (c == '<' && *pos + 1 < len && b[*pos + 1] == '<') return parse_dict(d, ar, b, len, pos, depth);
    if (c == '<') {
        int n = 0;
        u8 *s = parse_hexstring_raw(d, ar, b, len, pos, &n);
        if (!s) { pdf_set_err(d, (long)*pos, "bad hex string"); return NULL; }
        PdfVal *v = val_new(d, ar, PV_STR);
        if (!v) return NULL;
        v->u.str.s = s;
        v->u.str.len = n;
        return v;
    }
    if (c == '(') {
        int n = 0;
        u8 *s = parse_string_raw(d, ar, b, len, pos, &n);
        if (!s) { pdf_set_err(d, (long)*pos, "unterminated string"); return NULL; }
        PdfVal *v = val_new(d, ar, PV_STR);
        if (!v) return NULL;
        v->u.str.s = s;
        v->u.str.len = n;
        return v;
    }
    if (c == '/') {
        char *s = parse_name_raw(d, ar, b, len, pos);
        if (!s) return NULL;
        PdfVal *v = val_new(d, ar, PV_NAME);
        if (!v) return NULL;
        v->u.name.s = s;
        return v;
    }
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        double num = 0.0;
        int is_int = 0;
        if (!parse_number_at(b, len, pos, &num, &is_int)) {
            pdf_set_err(d, (long)*pos, "bad number");
            return NULL;
        }
        PdfVal *v = val_new(d, ar, is_int ? PV_INT : PV_REAL);
        if (!v) return NULL;
        if (is_int) v->u.i = (long)num;
        else v->u.r = num;
        return v;
    }
    if (at_word(b, len, *pos, "true"))  { *pos += 4; PdfVal *v = val_new(d, ar, PV_BOOL); if (v) v->u.b = 1; return v; }
    if (at_word(b, len, *pos, "false")) { *pos += 5; PdfVal *v = val_new(d, ar, PV_BOOL); if (v) v->u.b = 0; return v; }
    if (at_word(b, len, *pos, "null"))  { *pos += 4; return val_new(d, ar, PV_NULL); }

    pdf_set_err(d, (long)*pos, "unexpected token '%c' (0x%02X)", (c >= 32 && c < 127) ? c : '?', (unsigned)c);
    return NULL;
}

/* ------------------------------------------------------------
 *  Dictionaries
 * ------------------------------------------------------------ */
/* Streams carry their dictionary in the same shape, so every lookup goes
 * through here rather than assuming PV_DICT. */
static PdfVal *dict_get(const PdfVal *dict, const char *key) {
    if (!dict) return NULL;
    const PdfDictEntry *e;
    int n;
    if (dict->type == PV_DICT) { e = dict->u.dict.e; n = dict->u.dict.count; }
    else if (dict->type == PV_STREAM) { e = dict->u.stream.e; n = dict->u.stream.count; }
    else return NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].key, key)) return e[i].val;
    return NULL;
}

static int name_is(const PdfVal *v, const char *s) {
    return v && v->type == PV_NAME && !strcmp(v->u.name.s, s);
}

static int val_int(const PdfVal *v, long *out) {
    if (!v) return 0;
    if (v->type == PV_INT) { *out = v->u.i; return 1; }
    if (v->type == PV_REAL) { *out = (long)v->u.r; return 1; }
    return 0;
}

static int val_num(const PdfVal *v, double *out) {
    if (!v) return 0;
    if (v->type == PV_INT) { *out = (double)v->u.i; return 1; }
    if (v->type == PV_REAL) { *out = v->u.r; return 1; }
    return 0;
}

/* ------------------------------------------------------------
 *  Cross-reference entries and object cache
 * ------------------------------------------------------------ */
static int xref_set(PdfDoc *d, int num, int type, long a, int b, int replace) {
    if (num < 0 || num >= PDF_MAX_OBJECTS) return 0;
    if (num >= d->xref_cap) {
        int nc = d->xref_cap ? d->xref_cap : 64;
        while (nc <= num) nc *= 2;
        XEntry *ne = realloc(d->xref, (size_t)nc * sizeof(*ne));
        if (!ne) return 0;
        memset(ne + d->xref_cap, 0, (size_t)(nc - d->xref_cap) * sizeof(*ne));
        d->xref = ne;
        d->xref_cap = nc;
    }
    if (d->xref[num].present && !replace) return 1;   /* a newer section won */
    d->xref[num].present = 1;
    d->xref[num].type = type;
    d->xref[num].a = a;
    d->xref[num].b = b;
    if (num + 1 > d->xref_size) d->xref_size = num + 1;
    return 1;
}

static PdfVal *obj_get(PdfDoc *d, int num, int depth);

static PdfVal *obj_resolve(PdfDoc *d, PdfVal *v, int depth) {
    int guard = 0;
    while (v && v->type == PV_REF) {
        if (depth > PDF_MAX_DEPTH || ++guard > 16) return NULL;
        v = obj_get(d, v->u.ref.num, depth + 1);
    }
    return v;
}

/* Parse one "N G obj ... endobj" at a file offset. */
static PdfVal *parse_indirect(PdfDoc *d, long off, int *out_num, int *out_gen) {
    if (off < 0 || (size_t)off >= d->len) {
        pdf_set_err(d, off, "object offset %ld is past the end of the %lu-byte file",
                    off, (unsigned long)d->len);
        return NULL;
    }
    size_t pos = (size_t)off;
    long num = 0, gen = 0;
    if (!parse_int_at(d->buf, d->len, &pos, &num) ||
        !parse_int_at(d->buf, d->len, &pos, &gen)) {
        pdf_set_err(d, off, "no object header at offset %ld", off);
        return NULL;
    }
    skip_ws(d->buf, d->len, &pos);
    if (!at_word(d->buf, d->len, pos, "obj")) {
        pdf_set_err(d, (long)pos, "expected 'obj' at offset %ld", (long)pos);
        return NULL;
    }
    pos += 3;
    if (out_num) *out_num = (int)num;
    if (out_gen) *out_gen = (int)gen;

    PdfVal *v = parse_value(d, &d->arena, d->buf, d->len, &pos, 0);
    if (!v) return NULL;

    size_t after = pos;
    skip_ws(d->buf, d->len, &after);
    if (v->type == PV_DICT && at_word(d->buf, d->len, after, "stream")) {
        size_t p = after + 6;
        if (p < d->len && d->buf[p] == '\r') p++;
        if (p < d->len && d->buf[p] == '\n') p++;
        else if (p >= d->len || d->buf[p] != '\n') {
            pdf_set_err(d, (long)p, "stream keyword at %ld is not followed by a line break", (long)after);
            return NULL;
        }

        PdfVal *st = val_new(d, &d->arena, PV_STREAM);
        if (!st) return NULL;
        st->u.stream.e = v->u.dict.e;
        st->u.stream.count = v->u.dict.count;
        st->u.stream.raw = d->buf + p;
        st->u.stream.raw_off = (long)p;

        /* /Length may be indirect; resolve without recursing into streams. */
        long l = -1;
        PdfVal *lv = dict_get(v, "Length");
        if (lv && lv->type == PV_REF) lv = obj_get(d, lv->u.ref.num, 4);
        if (!val_int(lv, &l)) l = -1;

        size_t enddata;
        if (l >= 0 && p + (size_t)l <= d->len) {
            enddata = p + (size_t)l;
        } else {
            /* Broken /Length: find the endstream keyword. */
            size_t q = p;
            while (q + 9 <= d->len && !(d->buf[q] == 'e' && at_word(d->buf, d->len, q, "endstream"))) q++;
            if (q + 9 > d->len) {
                pdf_set_err(d, (long)p, "stream at %ld has no endstream", (long)p);
                return NULL;
            }
            enddata = q;
            while (enddata > p && (d->buf[enddata - 1] == '\n' || d->buf[enddata - 1] == '\r'))
                enddata--;
        }
        st->u.stream.rawlen = enddata - p;

        size_t q = enddata;
        skip_ws(d->buf, d->len, &q);
        if (!at_word(d->buf, d->len, q, "endstream")) {
            pdf_set_err(d, (long)enddata, "stream at %ld is not closed by endstream", (long)p);
            return NULL;
        }
        return st;
    }
    return v;
}

/* ------------------------------------------------------------
 *  Stream decoding
 * ------------------------------------------------------------ */
static PdfVal *dict_get_resolved(PdfDoc *d, const PdfVal *dict, const char *key) {
    return obj_resolve(d, dict_get(dict, key), 0);
}

/* Apply /Filter and /DecodeParms to one stream's raw bytes. */
static u8 *decode_stream(PdfDoc *d, PdfVal *st, size_t *outlen) {
    u8 *cur = malloc(st->u.stream.rawlen ? st->u.stream.rawlen : 1);
    if (!cur) { snprintf(st->u.stream.why, sizeof(st->u.stream.why), "out of memory"); return NULL; }
    memcpy(cur, st->u.stream.raw, st->u.stream.rawlen);
    size_t curlen = st->u.stream.rawlen;
    int cur_is_malloc = 1;

    PdfVal *filter = dict_get(st, "Filter");
    if (filter && filter->type == PV_REF) filter = obj_resolve(d, filter, 0);
    PdfVal *parms = dict_get(st, "DecodeParms");
    if (parms && parms->type == PV_REF) parms = obj_resolve(d, parms, 0);
    if (!parms) parms = dict_get(st, "DP");
    if (parms && parms->type == PV_REF) parms = obj_resolve(d, parms, 0);

    int nf = 0;
    if (filter) {
        if (filter->type == PV_NAME) nf = 1;
        else if (filter->type == PV_ARRAY) nf = filter->u.arr.count;
        else {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why), "/Filter is neither a name nor an array");
            free(cur);
            return NULL;
        }
    }

    for (int i = 0; i < nf; i++) {
        PdfVal *f = (filter->type == PV_NAME) ? filter : filter->u.arr.items[i];
        if (f && f->type == PV_REF) f = obj_resolve(d, f, 0);
        if (!f || f->type != PV_NAME) {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why), "filter %d is not a name", i);
            free(cur);
            return NULL;
        }
        PdfVal *parm = NULL;
        if (parms) {
            if (parms->type == PV_DICT) parm = parms;
            else if (parms->type == PV_ARRAY && i < parms->u.arr.count)
                parm = obj_resolve(d, parms->u.arr.items[i], 0);
        }
        if (parm && parm->type == PV_REF) parm = obj_resolve(d, parm, 0);

        const char *name = f->u.name.s;
        u8 *next = NULL;
        size_t nextlen = 0;

        if (!strcmp(name, "FlateDecode") || !strcmp(name, "Fl")) {
            long pred = 1, cols = 1, colors = 1, bpc = 8;
            if (parm && parm->type == PV_DICT) {
                val_int(dict_get_resolved(d, parm, "Predictor"), &pred);
                val_int(dict_get_resolved(d, parm, "Columns"), &cols);
                val_int(dict_get_resolved(d, parm, "Colors"), &colors);
                val_int(dict_get_resolved(d, parm, "BitsPerComponent"), &bpc);
            }
            char w[96];
            next = pdf_inflate(cur, curlen, &nextlen, w, sizeof(w), PDF_MAX_STREAM);
            if (!next) {
                snprintf(st->u.stream.why, sizeof(st->u.stream.why),
                         "FlateDecode at byte %ld: corrupt deflate data", st->u.stream.raw_off);
                free(cur);
                return NULL;
            }
            Buf b;
            b.p = next;
            b.len = nextlen;
            b.cap = nextlen;
            b.oom = 0;
            if (!unpredict(&b, (int)pred, (int)cols, (int)colors, (int)bpc)) {
                snprintf(st->u.stream.why, sizeof(st->u.stream.why),
                         "FlateDecode predictor %ld is not supported (columns %ld colors %ld bpc %ld)",
                         pred, cols, colors, bpc);
                free(b.p);
                free(cur);
                return NULL;
            }
            next = b.p;
            nextlen = b.len;
        } else if (!strcmp(name, "ASCIIHexDecode") || !strcmp(name, "AHx")) {
            next = ascii_hex_decode(cur, curlen, &nextlen);
        } else if (!strcmp(name, "ASCII85Decode") || !strcmp(name, "A85")) {
            next = ascii85_decode(cur, curlen, &nextlen);
        } else if (!strcmp(name, "RunLengthDecode") || !strcmp(name, "RL")) {
            next = runlength_decode(cur, curlen, &nextlen);
        } else if (!strcmp(name, "Crypt")) {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why), "/Crypt filter: encryption is not supported");
            free(cur);
            return NULL;
        } else if (!strcmp(name, "DCTDecode") || !strcmp(name, "JPXDecode") ||
                   !strcmp(name, "CCITTFaxDecode") || !strcmp(name, "JBIG2Decode")) {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why),
                     "image filter /%s is not decoded (images are not drawn)", name);
            free(cur);
            return NULL;
        } else {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why), "unsupported filter /%s", name);
            free(cur);
            return NULL;
        }

        if (!next) {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why),
                     "/%s at byte %ld: data is malformed", name, st->u.stream.raw_off);
            free(cur);
            return NULL;
        }
        if (cur_is_malloc) free(cur);
        cur = next;
        curlen = nextlen;
        cur_is_malloc = 1;

        if (curlen > PDF_MAX_STREAM) {
            snprintf(st->u.stream.why, sizeof(st->u.stream.why),
                     "/%s decoded to %lu bytes, above the %lu-byte limit",
                     name, (unsigned long)curlen, (unsigned long)PDF_MAX_STREAM);
            free(cur);
            return NULL;
        }
    }

    /* Copy into the arena so the cache lives exactly as long as the document. */
    u8 *data = ar_alloc(&d->arena, curlen ? curlen : 1);
    if (!data) {
        snprintf(st->u.stream.why, sizeof(st->u.stream.why), "out of memory for a decoded stream");
        free(cur);
        return NULL;
    }
    if (curlen) memcpy(data, cur, curlen);
    free(cur);
    *outlen = curlen;
    return data;
}

static int stream_data(PdfDoc *d, PdfVal *st, const u8 **out, size_t *outlen) {
    if (!st || st->type != PV_STREAM) { *out = NULL; *outlen = 0; return 0; }
    if (st->u.stream.decoded) {
        *out = st->u.stream.data;
        *outlen = st->u.stream.datalen;
        return st->u.stream.data != NULL;
    }
    st->u.stream.decoded = 1;
    size_t n = 0;
    u8 *p = decode_stream(d, st, &n);
    st->u.stream.data = p;
    st->u.stream.datalen = n;
    *out = p;
    *outlen = n;
    return p != NULL;
}

/* ------------------------------------------------------------
 *  Object resolution, including /ObjStm
 * ------------------------------------------------------------ */
#define OBJSTREAM_MAX 4096          /* objects in one /ObjStm */

static PdfVal *obj_from_objstm(PdfDoc *d, int stmnum, int want, int depth) {
    PdfVal *st = obj_get(d, stmnum, depth + 1);
    if (!st || st->type != PV_STREAM) {
        pdf_set_err(d, 0, "object %d is inside object stream %d, which is not a stream", want, stmnum);
        return NULL;
    }
    const u8 *data = NULL;
    size_t len = 0;
    if (!stream_data(d, st, &data, &len)) {
        pdf_set_err(d, st->u.stream.raw_off, "object stream %d: %s", stmnum, st->u.stream.why);
        return NULL;
    }
    if (!st->u.stream.objstm_counted) {
        st->u.stream.objstm_counted = 1;
        d->n_objstms++;
    }
    long n = 0, first = 0;
    if (!val_int(dict_get(st, "N"), &n) || !val_int(dict_get(st, "First"), &first) ||
        n < 0 || first < 0) {
        pdf_set_err(d, st->u.stream.raw_off, "object stream %d has no valid /N or /First", stmnum);
        return NULL;
    }
    if (n > OBJSTREAM_MAX) {
        pdf_set_err(d, st->u.stream.raw_off,
                    "object stream %d claims %ld objects (limit %d)", stmnum, n, OBJSTREAM_MAX);
        return NULL;
    }

    size_t pos = 0;
    long num = 0, off = 0;
    long found_off = -1, next_off = (long)len;
    for (long i = 0; i < n; i++) {
        if (!parse_int_at(data, len, &pos, &num) || !parse_int_at(data, len, &pos, &off)) {
            pdf_set_err(d, st->u.stream.raw_off, "object stream %d: header ends after %ld pairs", stmnum, i);
            return NULL;
        }
        if (num == want) {
            found_off = off;
            /* The next pair's offset bounds this object; if this is the last
             * pair, the end of the stream does. */
            if (i + 1 < n) {
                long tmp;
                size_t save = pos;
                if (parse_int_at(data, len, &pos, &tmp) && parse_int_at(data, len, &pos, &tmp))
                    next_off = tmp;
                else
                    next_off = (long)(len - (size_t)first);
                pos = save;
            } else {
                next_off = (long)(len - (size_t)first);
            }
            break;
        }
    }
    if (found_off < 0) {
        pdf_set_err(d, st->u.stream.raw_off, "object %d is not in object stream %d", want, stmnum);
        return NULL;
    }
    size_t p = (size_t)first + (size_t)found_off;
    size_t end = (size_t)first + (size_t)next_off;
    if (p >= len || end > len || end <= p) {
        pdf_set_err(d, st->u.stream.raw_off,
                    "object %d in stream %d points outside it (offset %ld)", want, stmnum, found_off);
        return NULL;
    }
    return parse_value(d, &d->arena, data, end, &p, depth + 1);
}

static PdfVal *obj_get(PdfDoc *d, int num, int depth) {
    if (num <= 0 || num >= d->obj_count) return NULL;
    ObjSlot *s = &d->objs[num];
    if (s->present) return s->val;
    s->present = 1;                     /* break cycles: a self-reference is null */
    s->val = NULL;

    if (num >= d->xref_cap || !d->xref[num].present) return NULL;
    XEntry *e = &d->xref[num];

    if (e->type == 1) {
        int got = 0;
        PdfVal *v = parse_indirect(d, e->a, &got, NULL);
        if (!v) return NULL;
        if (got != num) {
            pdf_set_err(d, e->a, "xref says object %d is at offset %ld, but %d is there",
                        num, e->a, got);
            return NULL;
        }
        s->val = v;
        return v;
    }
    if (e->type == 2) {
        PdfVal *v = obj_from_objstm(d, (int)e->a, num, depth);
        s->val = v;
        return v;
    }
    return NULL;                        /* free entry: the null object */
}

/* ------------------------------------------------------------
 *  Cross-reference: tables, streams, /Prev, hybrid sections
 * ------------------------------------------------------------ */
static long parse_uint_field(const u8 *p, int w) {
    unsigned long v = 0;
    for (int i = 0; i < w; i++) v = (v << 8) | p[i];
    return (long)v;
}

static int parse_xref_stream(PdfDoc *d, long off, int follow_prev, int depth);

static int parse_xref_section_chain(PdfDoc *d, long off, int depth);

/* Classic table at `off`; returns 0 on failure. */
static int parse_xref_table(PdfDoc *d, long off, int depth) {
    if (depth > PDF_MAX_XREF_SEC) {
        pdf_set_err(d, off, "xref /Prev chain is longer than %d sections", PDF_MAX_XREF_SEC);
        return 0;
    }
    if (off < 0 || (size_t)off >= d->len) {
        pdf_set_err(d, off, "xref offset %ld is past the end of the file", off);
        return 0;
    }
    size_t pos = (size_t)off;
    skip_ws(d->buf, d->len, &pos);
    if (!at_word(d->buf, d->len, pos, "xref")) {
        pdf_set_err(d, off, "no 'xref' keyword at offset %ld", off);
        return 0;
    }
    pos += 4;
    d->n_xref_tables++;

    for (;;) {
        skip_ws(d->buf, d->len, &pos);
        if (pos >= d->len) { pdf_set_err(d, (long)pos, "xref table ends without a trailer"); return 0; }
        if (at_word(d->buf, d->len, pos, "trailer")) break;

        long start = 0, count = 0;
        size_t save = pos;
        if (!parse_int_at(d->buf, d->len, &pos, &start) ||
            !parse_int_at(d->buf, d->len, &pos, &count) || count < 0 || count > PDF_MAX_OBJECTS) {
            pos = save;
            pdf_set_err(d, (long)pos, "malformed xref subsection header");
            return 0;
        }
        for (long i = 0; i < count; i++) {
            long objoff = 0, gen = 0;
            if (!parse_int_at(d->buf, d->len, &pos, &objoff) ||
                !parse_int_at(d->buf, d->len, &pos, &gen)) {
                pdf_set_err(d, (long)pos, "malformed xref entry for object %ld", start + i);
                return 0;
            }
            skip_ws(d->buf, d->len, &pos);
            int type = -1;
            if (pos < d->len) {
                if (d->buf[pos] == 'n') type = 1;
                else if (d->buf[pos] == 'f') type = 0;
            }
            if (type < 0) {
                pdf_set_err(d, (long)pos, "xref entry for object %ld has no n/f flag", start + i);
                return 0;
            }
            pos++;
            if (start + i > 0) {
                if (!xref_set(d, (int)(start + i), type, objoff, (int)gen, 0)) {
                    pdf_set_err(d, (long)pos, "cannot allocate the xref table");
                    return 0;
                }
            }
        }
    }

    pos += 7;                                     /* 'trailer' */
    PdfVal *tr = parse_value(d, &d->arena, d->buf, d->len, &pos, 0);
    if (!tr || tr->type != PV_DICT) {
        pdf_set_err(d, (long)pos, "the xref trailer is not a dictionary");
        return 0;
    }
    /* Merge: the newest section wins, older sections fill the gaps. */
    if (!d->trailer) {
        d->trailer = tr;
    } else {
        for (int i = 0; i < tr->u.dict.count; i++) {
            if (!dict_get(d->trailer, tr->u.dict.e[i].key)) {
                PdfDictEntry *ne = ar_alloc(&d->arena,
                                            (size_t)(d->trailer->u.dict.count + 1) * sizeof(*ne));
                if (!ne) { pdf_set_err(d, (long)pos, "out of memory merging trailers"); return 0; }
                if (d->trailer->u.dict.count)
                    memcpy(ne, d->trailer->u.dict.e,
                           (size_t)d->trailer->u.dict.count * sizeof(*ne));
                ne[d->trailer->u.dict.count] = tr->u.dict.e[i];
                d->trailer->u.dict.e = ne;
                d->trailer->u.dict.count++;
            }
        }
    }

    /* hybrid files: /XRefStm points at an xref stream that supplements this
     * table; it must be read before /Prev so its entries are newer than the
     * previous section's. */
    long xstm = 0;
    if (val_int(dict_get(tr, "XRefStm"), &xstm) && xstm > 0) {
        if (!parse_xref_stream(d, xstm, 0, depth + 1))
            return 0;
    }

    long prev = 0;
    if (val_int(dict_get(tr, "Prev"), &prev) && prev > 0) {
        if (!parse_xref_section_chain(d, prev, depth + 1))
            return 0;
    }
    return 1;
}

/* An xref STREAM: /Type /XRef, /W [w1 w2 w3], entries packed with
 * the PNG Up predictor. This is what PDF 1.5+ writers emit. */
static int parse_xref_stream(PdfDoc *d, long off, int follow_prev, int depth) {
    if (depth > PDF_MAX_XREF_SEC) {
        pdf_set_err(d, off, "xref /Prev chain is longer than %d sections", PDF_MAX_XREF_SEC);
        return 0;
    }
    int num = 0;
    PdfVal *st = parse_indirect(d, off, &num, NULL);
    if (!st || st->type != PV_STREAM) {
        pdf_set_err(d, off, "xref stream offset %ld does not hold a stream", off);
        return 0;
    }
    if (!name_is(dict_get(st, "Type"), "XRef")) {
        pdf_set_err(d, off, "object at %ld is not /Type /XRef", off);
        return 0;
    }
    d->n_xref_streams++;

    long w[3] = { 0, 0, 0 };
    PdfVal *wv = dict_get_resolved(d, st, "W");
    if (wv && wv->type == PV_ARRAY && wv->u.arr.count >= 3) {
        for (int i = 0; i < 3; i++) val_int(wv->u.arr.items[i], &w[i]);
    }
    if (w[0] < 0 || w[1] < 0 || w[2] < 0 || w[0] > 4 || w[1] > 4 || w[2] > 4 ||
        w[0] + w[1] + w[2] == 0) {
        pdf_set_err(d, off, "xref stream /W is invalid");
        return 0;
    }

    long size = 0, prev = 0;
    val_int(dict_get_resolved(d, st, "Size"), &size);
    if (size <= 0 || size > PDF_MAX_OBJECTS) {
        pdf_set_err(d, off, "xref stream /Size %ld is outside 1..%d", size, PDF_MAX_OBJECTS);
        return 0;
    }

    const u8 *data = NULL;
    size_t dlen = 0;
    if (!stream_data(d, st, &data, &dlen)) {
        pdf_set_err(d, st->u.stream.raw_off, "xref stream at %ld: %s", off, st->u.stream.why);
        return 0;
    }

    long index[512];
    int nindex = 0;
    PdfVal *iv = dict_get_resolved(d, st, "Index");
    if (iv && iv->type == PV_ARRAY) {
        for (int i = 0; i + 1 < iv->u.arr.count && nindex < 510; i += 2) {
            long a = 0, b = 0;
            val_int(iv->u.arr.items[i], &a);
            val_int(iv->u.arr.items[i + 1], &b);
            index[nindex++] = a;
            index[nindex++] = b;
        }
    }
    if (!nindex) { index[0] = 0; index[1] = size; nindex = 2; }

    size_t rec = (size_t)w[0] + (size_t)w[1] + (size_t)w[2];
    size_t p = 0;
    for (int s = 0; s < nindex; s += 2) {
        long first = index[s], count = index[s + 1];
        for (long i = 0; i < count; i++) {
            if (p + rec > dlen) {
                pdf_set_err(d, st->u.stream.raw_off,
                            "xref stream ends after %lu of %ld+%ld entries",
                            (unsigned long)(p / rec), first, count);
                return 0;
            }
            const u8 *e = data + p;
            long type = w[0] ? parse_uint_field(e, (int)w[0]) : 1;
            long f2 = w[1] ? parse_uint_field(e + w[0], (int)w[1]) : 0;
            long f3 = w[2] ? parse_uint_field(e + w[0] + w[1], (int)w[2]) : 0;
            long objnum = first + i;
            if (objnum > 0 && objnum < PDF_MAX_OBJECTS) {
                if (type == 0 || type == 1 || type == 2) {
                    if (!xref_set(d, (int)objnum, (int)type, f2, (int)f3, 0)) {
                        pdf_set_err(d, (long)p, "cannot allocate the xref table");
                        return 0;
                    }
                }
            }
            p += rec;
        }
    }

    if (!d->trailer) {
        PdfVal *copy = val_new(d, &d->arena, PV_DICT);
        if (!copy) return 0;
        copy->u.dict.count = st->u.stream.count;
        copy->u.dict.e = st->u.stream.e;
        d->trailer = copy;
    }

    if (follow_prev && val_int(dict_get_resolved(d, st, "Prev"), &prev) && prev > 0) {
        if (!parse_xref_section_chain(d, prev, depth + 1))
            return 0;
    }
    return 1;
}

static int parse_xref_section_chain(PdfDoc *d, long off, int depth) {
    if (off <= 0 || (size_t)off >= d->len) {
        pdf_set_err(d, off, "xref section offset %ld is outside the file", off);
        return 0;
    }
    size_t pos = (size_t)off;
    skip_ws(d->buf, d->len, &pos);
    if (at_word(d->buf, d->len, pos, "xref"))
        return parse_xref_table(d, off, depth);
    return parse_xref_stream(d, off, 1, depth) ? 1 : 0;
}

/* ------------------------------------------------------------
 *  Recovery: find every "N G obj" in the file. Used when startxref
 *  is missing or wrong, which is common in files that were edited
 *  in a text editor or truncated and re-terminated. The reader says
 *  so (PdfDocStats.reconstructed) rather than hiding it.
 * ------------------------------------------------------------ */
static int xref_reconstruct(PdfDoc *d) {
    d->reconstructed = 1;
    d->n_xref_tables = 0;
    d->n_xref_streams = 0;

    for (size_t i = 0; i + 3 <= d->len; i++) {
        if (!(d->buf[i] == 'o' && d->buf[i + 1] == 'b' && d->buf[i + 2] == 'j')) continue;
        if (i + 3 < d->len && pdf_is_regular(d->buf[i + 3])) continue;

        /* Walk back over "N G " immediately before the keyword. */
        size_t p = i;
        while (p > 0 && pdf_is_ws(d->buf[p - 1])) p--;
        size_t gend = p;
        while (p > 0 && d->buf[p - 1] >= '0' && d->buf[p - 1] <= '9') p--;
        if (p == gend) continue;
        long gen = 0;
        size_t gp = p;
        if (!parse_int_at(d->buf, d->len, &gp, &gen)) continue;
        while (p > 0 && pdf_is_ws(d->buf[p - 1])) p--;
        size_t nend = p;
        while (p > 0 && d->buf[p - 1] >= '0' && d->buf[p - 1] <= '9') p--;
        if (p == nend) continue;
        long num = 0;
        size_t np = p;
        if (!parse_int_at(d->buf, d->len, &np, &num)) continue;
        if (num <= 0 || num >= PDF_MAX_OBJECTS) continue;

        xref_set(d, (int)num, 1, (long)p, (int)gen, 1);   /* last one wins */
    }
    if (d->xref_size == 0) {
        pdf_set_err(d, 0, "no objects found: the file does not contain a PDF object");
        return 0;
    }

    /* Find a trailer: the last "trailer" dictionary, or an /Type /XRef
     * stream, or the catalog itself. */
    for (size_t i = d->len; i > 6; i--) {
        if (memcmp(d->buf + i - 7, "trailer", 7) == 0) {
            size_t pos = i;
            PdfVal *tr = parse_value(d, &d->arena, d->buf, d->len, &pos, 0);
            if (tr && tr->type == PV_DICT) { d->trailer = tr; break; }
        }
    }

    for (int n = 1; n < d->xref_size && !dict_get(d->trailer, "Root"); n++) {
        if (!d->xref[n].present || d->xref[n].type != 1) continue;
        int got = 0;
        PdfVal *v = parse_indirect(d, d->xref[n].a, &got, NULL);
        if (!v) {
            d->err[0] = 0;            /* a damaged object must not abort recovery */
            d->err_off = 0;
            continue;
        }
        if (dict_get(v, "Root")) { d->trailer = v; break; }
        if (name_is(dict_get(v, "Type"), "Catalog")) {
            PdfVal *tr = val_new(d, &d->arena, PV_DICT);
            if (!tr) return 0;
            tr->u.dict.count = 1;
            tr->u.dict.e = ar_alloc(&d->arena, sizeof(PdfDictEntry));
            if (!tr->u.dict.e) return 0;
            tr->u.dict.e[0].key = ar_strdup(&d->arena, "Root");
            tr->u.dict.e[0].val = v;
            d->trailer = tr;
            break;
        }
    }
    d->err[0] = 0;
    d->err_off = 0;
    if (!d->trailer || !dict_get(d->trailer, "Root")) {
        pdf_set_err(d, 0, "damaged file: no trailer and no /Type /Catalog object");
        return 0;
    }
    return 1;
}
/* ------------------------------------------------------------
 *  Page tree
 * ------------------------------------------------------------ */
#define PAGE_DEFAULT_W 612.0
#define PAGE_DEFAULT_H 792.0

static int read_box(PdfDoc *d, PdfVal *node, const char *key, double out[4]) {
    PdfVal *v = obj_resolve(d, dict_get(node, key), 0);
    if (!v || v->type != PV_ARRAY || v->u.arr.count < 4) return 0;
    double n[4];
    for (int i = 0; i < 4; i++)
        if (!val_num(v->u.arr.items[i], &n[i])) return 0;
    double x0 = n[0] < n[2] ? n[0] : n[2];
    double x1 = n[0] < n[2] ? n[2] : n[0];
    double y0 = n[1] < n[3] ? n[1] : n[3];
    double y1 = n[1] < n[3] ? n[3] : n[1];
    if (x1 - x0 <= 0.01 || y1 - y0 <= 0.01) return 0;
    out[0] = x0; out[1] = y0; out[2] = x1; out[3] = y1;
    return 1;
}

static int page_push(PdfDoc *d, PdfVal *dict, int num, PdfVal *resources,
                     const double box[4], int rotate) {
    if (d->npages >= PDF_MAX_PAGES) { d->pages_truncated = 1; return 1; }
    if (d->npages == d->pages_cap) {
        int nc = d->pages_cap ? d->pages_cap * 2 : 16;
        if (nc > PDF_MAX_PAGES) nc = PDF_MAX_PAGES;
        PdfPage *np = realloc(d->pages, (size_t)nc * sizeof(*np));
        if (!np) { pdf_set_err(d, 0, "out of memory for the page list"); return 0; }
        d->pages = np;
        d->pages_cap = nc;
    }
    PdfPage *p = &d->pages[d->npages++];
    memset(p, 0, sizeof(*p));
    p->num = num;
    p->dict = dict;
    p->resources = resources;
    p->box[0] = box[0];
    p->box[1] = box[1];
    p->box[2] = box[2];
    p->box[3] = box[3];
    p->rotate = rotate;
    return 1;
}

static int pages_walk(PdfDoc *d, PdfVal *node, int num, PdfVal *resources,
                      const double *box, const double *crop, int rotate,
                      int depth, int *path, int pathlen) {
    if (!node || node->type != PV_DICT) return 1;
    if (depth > PDF_MAX_DEPTH) {
        pdf_set_err(d, 0, "page tree is deeper than %d levels", PDF_MAX_DEPTH);
        return 0;
    }

    /* A cycle would otherwise repeat a subtree until the depth limit; the
     * object numbers on the current path are enough to name it exactly. */
    for (int i = 0; i < pathlen; i++) {
        if (path[i] == num) {
            pdf_set_err(d, 0, "page tree loops back to object %d", num);
            return 0;
        }
    }

    PdfVal *res = obj_resolve(d, dict_get(node, "Resources"), 0);
    if (!res) res = resources;

    double mb[4], cb[4];
    const double *mbp = box, *cbp = crop;
    if (read_box(d, node, "MediaBox", mb)) mbp = mb;
    if (read_box(d, node, "CropBox", cb)) cbp = cb;

    long rot = rotate;
    long r = 0;
    if (val_int(obj_resolve(d, dict_get(node, "Rotate"), 0), &r)) rot = r;
    rot = ((rot % 360) + 360) % 360;
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) rot = 0;

    PdfVal *kids = obj_resolve(d, dict_get(node, "Kids"), 0);
    int is_pages = name_is(dict_get(node, "Type"), "Pages") || (kids && kids->type == PV_ARRAY);

    if (is_pages) {
        if (!kids || kids->type != PV_ARRAY) {
            pdf_set_err(d, 0, "page tree node has no /Kids array");
            return 0;
        }
        for (int i = 0; i < kids->u.arr.count; i++) {
            PdfVal *kv = kids->u.arr.items[i];
            int knum = kv && kv->type == PV_REF ? kv->u.ref.num : 0;
            PdfVal *kid = obj_resolve(d, kv, depth);
            if (!kid || kid->type != PV_DICT) {
                pdf_set_err(d, 0, "page tree /Kids[%d] is not a dictionary", i);
                return 0;
            }
            int np[PDF_MAX_DEPTH + 1];
            for (int k = 0; k < pathlen && k <= PDF_MAX_DEPTH; k++) np[k] = path[k];
            if (pathlen <= PDF_MAX_DEPTH) np[pathlen] = num;
            if (!pages_walk(d, kid, knum, res, mbp, cbp, (int)rot,
                            depth + 1, np, pathlen < PDF_MAX_DEPTH ? pathlen + 1 : pathlen))
                return 0;
            if (d->pages_truncated) return 1;
        }
        return 1;
    }

    double eff[4];
    if (cbp) memcpy(eff, cbp, sizeof(eff));
    else if (mbp) memcpy(eff, mbp, sizeof(eff));
    else {
        /* The PDF default page size when /MediaBox is absent is US Letter. */
        eff[0] = 0; eff[1] = 0; eff[2] = PAGE_DEFAULT_W; eff[3] = PAGE_DEFAULT_H;
    }
    if (eff[2] - eff[0] <= 0.01 || eff[3] - eff[1] <= 0.01) {
        pdf_set_err(d, 0, "page %d has a degenerate /MediaBox", d->npages + 1);
        return 0;
    }
    return page_push(d, node, num, res, eff, (int)rot);
}

/* ------------------------------------------------------------
 *  Fonts
 * ------------------------------------------------------------ */

/* Adobe glyph-name -> Unicode for the names a /Differences array is
 * likely to contain. Anything else is looked up as uniXXXX/uXXXX, and
 * failing that is left as U+FFFD rather than guessed. */
typedef struct { const char *name; u32 cp; } GlyphName;

static const GlyphName GLYPHS[] = {
    {"space", 0x20}, {"exclam", 0x21}, {"quotedbl", 0x22}, {"numbersign", 0x23},
    {"dollar", 0x24}, {"percent", 0x25}, {"ampersand", 0x26}, {"quotesingle", 0x27},
    {"parenleft", 0x28}, {"parenright", 0x29}, {"asterisk", 0x2A}, {"plus", 0x2B},
    {"comma", 0x2C}, {"hyphen", 0x2D}, {"period", 0x2E}, {"slash", 0x2F},
    {"zero", 0x30}, {"one", 0x31}, {"two", 0x32}, {"three", 0x33}, {"four", 0x34},
    {"five", 0x35}, {"six", 0x36}, {"seven", 0x37}, {"eight", 0x38}, {"nine", 0x39},
    {"colon", 0x3A}, {"semicolon", 0x3B}, {"less", 0x3C}, {"equal", 0x3D},
    {"greater", 0x3E}, {"question", 0x3F}, {"at", 0x40},
    {"bracketleft", 0x5B}, {"backslash", 0x5C}, {"bracketright", 0x5D},
    {"asciicircum", 0x5E}, {"underscore", 0x5F}, {"grave", 0x60},
    {"braceleft", 0x7B}, {"bar", 0x7C}, {"braceright", 0x7D}, {"asciitilde", 0x7E},
    {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3}, {"fraction", 0xA4},
    {"yen", 0xA5}, {"florin", 0xA6}, {"section", 0xA7}, {"currency", 0xA8},
    {"quotedblleft", 0x201C}, {"guillemotleft", 0xAB}, {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203A}, {"fi", 0xFB01}, {"fl", 0xFB02},
    {"endash", 0x2013}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"periodcentered", 0xB7}, {"paragraph", 0xB6}, {"bullet", 0x2022},
    {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E}, {"quotedblright", 0x201D},
    {"guillemotright", 0xBB}, {"ellipsis", 0x2026}, {"perthousand", 0x2030},
    {"questiondown", 0xBF}, {"acute", 0xB4}, {"circumflex", 0x2C6}, {"tilde", 0x2DC},
    {"macron", 0xAF}, {"breve", 0x2D8}, {"dotaccent", 0x2D9}, {"dieresis", 0xA8},
    {"ring", 0x2DA}, {"cedilla", 0xB8}, {"hungarumlaut", 0x2DD}, {"ogonek", 0x2DB},
    {"caron", 0x2C7}, {"emdash", 0x2014}, {"AE", 0xC6}, {"ordfeminine", 0xAA},
    {"Lslash", 0x141}, {"Oslash", 0xD8}, {"OE", 0x152}, {"ordmasculine", 0xBA},
    {"ae", 0xE6}, {"dotlessi", 0x131}, {"lslash", 0x142}, {"oslash", 0xF8},
    {"oe", 0x153}, {"germandbls", 0xDF}, {"nbspace", 0xA0}, {"minus", 0x2212},
    {"multiply", 0xD7}, {"divide", 0xF7}, {"plusminus", 0xB1}, {"degree", 0xB0},
    {"copyright", 0xA9}, {"registered", 0xAE}, {"trademark", 0x2122},
    {"Euro", 0x20AC}, {"euro", 0x20AC}, {"mu", 0xB5}, {"onehalf", 0xBD},
    {"onequarter", 0xBC}, {"threequarters", 0xBE}, {"logicalnot", 0xAC},
    {"brokenbar", 0xA6}, {"Eth", 0xD0}, {"eth", 0xF0}, {"Thorn", 0xDE},
    {"thorn", 0xFE}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D},
    {"zcaron", 0x17E}, {"Ydieresis", 0x178}, {"IJ", 0x132}, {"ij", 0x133},
    {"lozenge", 0x25CA}, {"infinity", 0x221E}, {"notequal", 0x2260},
    {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"summation", 0x2211},
    {"product", 0x220F}, {"radical", 0x221A}, {"integral", 0x222B},
    {"partialdiff", 0x2202}, {"approxequal", 0x2248}, {"Delta", 0x2206},
    {"Omega", 0x2126}, {"pi", 0x3C0}, {"arrowleft", 0x2190}, {"arrowright", 0x2192},
    {"arrowup", 0x2191}, {"arrowdown", 0x2193}, {"heart", 0x2665},
    {NULL, 0}
};

static u32 glyph_name_to_unicode(const char *name) {
    if (!name || !name[0]) return 0;
    if (name[1] == 0 && name[0] > 32 && name[0] < 127) return (u8)name[0];
    for (int i = 0; GLYPHS[i].name; i++)
        if (!strcmp(GLYPHS[i].name, name)) return GLYPHS[i].cp;
    if (!strncmp(name, "uni", 3) && strlen(name) >= 7) {
        u32 v = 0;
        for (int i = 3; i < 7; i++) {
            int h = hexval((unsigned char)name[i]);
            if (h < 0) return 0;
            v = (v << 4) | (u32)h;
        }
        return v;
    }
    if (name[0] == 'u' && strlen(name) >= 5) {
        u32 v = 0;
        for (int i = 1; i < 5; i++) {
            int h = hexval((unsigned char)name[i]);
            if (h < 0) return 0;
            v = (v << 4) | (u32)h;
        }
        return v;
    }
    return 0xFFFD;                                /* named, but not in our table */
}

/* WinAnsiEncoding 0x80..0x9F. 0xA0..0xFF are Latin-1 by definition. */
static const u32 WINANSI_HIGH[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

/* StandardEncoding 0xA0..0xFF (Adobe's built-in Type1 encoding). */
static const u32 STANDARD_HIGH[96] = {
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x2044, 0x00A5, 0x0192, 0x00A7,
    0x00A4, 0x0027, 0x201C, 0x00AB, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x0000, 0x2013, 0x2020, 0x2021, 0x00B7, 0x0000, 0x00B6, 0x2022,
    0x201A, 0x201E, 0x201D, 0x00BB, 0x2026, 0x2030, 0x0000, 0x00BF,
    0x0000, 0x0060, 0x00B4, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9,
    0x00A8, 0x0000, 0x02DA, 0x00B8, 0x0000, 0x02DD, 0x02DB, 0x02C7,
    0x2014, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x00C6, 0x0000, 0x00AA, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0141, 0x00D8, 0x0152, 0x00BA, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x00E6, 0x0000, 0x0000, 0x0000, 0x0131, 0x0000, 0x0000,
    0x0142, 0x00F8, 0x0153, 0x00DF, 0x0000, 0x0000, 0x0000, 0x0000
};

static void encoding_default(u32 uni[256], const char *which) {
    memset(uni, 0, 256 * sizeof(u32));
    for (int c = 32; c < 127; c++) uni[c] = (u32)c;
    if (!which || !strcmp(which, "WinAnsiEncoding")) {
        for (int c = 0x80; c <= 0x9F; c++) uni[c] = WINANSI_HIGH[c - 0x80];
        for (int c = 0xA0; c <= 0xFF; c++) uni[c] = (u32)c;
    } else if (!strcmp(which, "StandardEncoding")) {
        for (int c = 0xA0; c <= 0xFF; c++) uni[c] = STANDARD_HIGH[c - 0xA0];
        uni[0x27] = 0x2019;                       /* Standard quotesingle */
        uni[0x60] = 0x2018;
    } else {
        /* MacRomanEncoding and anything else: ASCII is right, the high half
         * is left at 0 and the renderer reports the substitution rather
         * than printing wrong accented letters. */
    }
}

static void encoding_differences(PdfDoc *d, u32 uni[256], PdfVal *enc) {
    PdfVal *diff = obj_resolve(d, dict_get(enc, "Differences"), 0);
    if (!diff || diff->type != PV_ARRAY) return;
    long code = 0;
    for (int i = 0; i < diff->u.arr.count; i++) {
        PdfVal *e = obj_resolve(d, diff->u.arr.items[i], 0);
        if (!e) continue;
        if (e->type == PV_INT) { code = e->u.i; continue; }
        if (e->type == PV_NAME) {
            if (code >= 0 && code < 256) uni[code] = glyph_name_to_unicode(e->u.name.s);
            code++;
        }
    }
}

typedef struct {
    int    parsed;
    int    valid;
    char   base[96];
    int    face;              /* FONT_REGULAR / FONT_SEMIBOLD / FONT_MONO */
    int    embedded;          /* a /FontFile* program is present */
    int    type0;             /* CID font: text is not interpreted */
    int    serif_substituted; /* Times asked for; Lato drawn */
    int    italic_dropped;
    int    first, last;
    int    has_widths;
    float  w[256];
    u8     w_measured[256];
    int    has_missing;
    float  missing;
    u32    uni[256];
} RFont;

static int font_base_name(PdfDoc *d, PdfVal *fd, char *out, size_t outsz) {
    PdfVal *bn = obj_resolve(d, dict_get(fd, "BaseFont"), 0);
    if (!bn) {
        PdfVal *desc = obj_resolve(d, dict_get(fd, "FontDescriptor"), 0);
        bn = obj_resolve(d, dict_get(desc, "FontName"), 0);
    }
    if (!bn || bn->type != PV_NAME) return 0;
    const char *s = bn->u.name.s;
    if (strlen(s) > 7 && s[6] == '+') s += 7;     /* ABCDEF+Subset */
    snprintf(out, outsz, "%s", s);
    return 1;
}

static int ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Map a PDF font name onto the faces the OS actually ships. There is no
 * serif or italic face in the image, so Times becomes Lato and oblique is
 * dropped; the caller records that so the viewer can say it. */
static void pick_face(RFont *f, const char *base, long flags, int have_flags) {
    int bold = ci_contains(base, "bold") || ci_contains(base, "black") ||
               ci_contains(base, "heavy") || ci_contains(base, "semibold");
    int mono = ci_contains(base, "courier") || ci_contains(base, "mono");
    if (!mono && have_flags && (flags & 1)) mono = 1;          /* FixedPitch */
    if (!bold && have_flags && (flags & (1 << 18))) bold = 1;  /* ForceBold */

    f->face = mono ? FONT_MONO : (bold ? FONT_SEMIBOLD : FONT_REGULAR);
    if (!mono && (ci_contains(base, "times") || ci_contains(base, "serif") ||
                  ci_contains(base, "georgia") || ci_contains(base, "garamond")))
        f->serif_substituted = 1;
    if (ci_contains(base, "italic") || ci_contains(base, "oblique"))
        f->italic_dropped = 1;
}

static void font_parse(PdfDoc *d, RFont *f, PdfVal *fd) {
    memset(f, 0, sizeof(*f));
    f->parsed = 1;
    if (!fd || fd->type != PV_DICT) return;

    PdfVal *sub = obj_resolve(d, dict_get(fd, "Subtype"), 0);
    const char *subtype = sub && sub->type == PV_NAME ? sub->u.name.s : "";
    int is_type0 = !strcmp(subtype, "Type0");
    if (is_type0) {
        f->type0 = 1;
        if (font_note(d, fd, 2)) d->st_type0_fonts++;
    }

    char base[96] = "";
    font_base_name(d, fd, base, sizeof(base));
    snprintf(f->base, sizeof(f->base), "%s", base);

    if (!strcmp(base, "Symbol") || !strcmp(base, "ZapfDingbats")) {
        f->valid = 0;                            /* no symbol face installed */
        return;
    }

    PdfVal *desc = obj_resolve(d, dict_get(fd, "FontDescriptor"), 0);
    long flags = 0;
    int have_flags = 0;
    if (desc && desc->type == PV_DICT) {
        flags = 0;
        have_flags = val_int(obj_resolve(d, dict_get(desc, "Flags"), 0), &flags);
        if (obj_resolve(d, dict_get(desc, "FontFile"), 0) ||
            obj_resolve(d, dict_get(desc, "FontFile2"), 0) ||
            obj_resolve(d, dict_get(desc, "FontFile3"), 0)) {
            f->embedded = 1;
            if (font_note(d, fd, 1)) d->st_embedded_fonts++;
        }
        long mw = 0;
        if (val_int(obj_resolve(d, dict_get(desc, "MissingWidth"), 0), &mw) && mw > 0) {
            f->has_missing = 1;
            f->missing = (float)mw;
        }
    }

    pick_face(f, base, flags, have_flags);

    /* Encoding */
    encoding_default(f->uni, NULL);
    PdfVal *enc = obj_resolve(d, dict_get(fd, "Encoding"), 0);
    if (enc && enc->type == PV_NAME) {
        if (!strcmp(enc->u.name.s, "StandardEncoding"))
            encoding_default(f->uni, "StandardEncoding");
        else if (!strcmp(enc->u.name.s, "MacRomanEncoding"))
            ;                                    /* ASCII kept; reported */
    } else if (enc && enc->type == PV_DICT) {
        PdfVal *be = obj_resolve(d, dict_get(enc, "BaseEncoding"), 0);
        if (be && be->type == PV_NAME)
            encoding_default(f->uni, be->u.name.s);
        encoding_differences(d, f->uni, enc);
    }

    /* Widths (in 1/1000 text space units). */
    long first = 0;
    int got_first = val_int(obj_resolve(d, dict_get(fd, "FirstChar"), 0), &first);
    PdfVal *wa = obj_resolve(d, dict_get(fd, "Widths"), 0);
    if (got_first && wa && wa->type == PV_ARRAY) {
        f->first = (int)first;
        f->last = (int)(first + wa->u.arr.count - 1);
        for (int i = 0; i < wa->u.arr.count; i++) {
            int code = (int)first + i;
            if (code < 0 || code > 255) continue;
            double w = 0;
            if (val_num(obj_resolve(d, wa->u.arr.items[i], 0), &w)) {
                f->w[code] = (float)w;
                f->has_widths = 1;
            }
        }
    }
    f->valid = 1;
}

/* ------------------------------------------------------------
 *  Title and open
 * ------------------------------------------------------------ */
static int pdf_utf8_put(char *out, size_t cap, size_t *used, u32 cp) {
    if (cp < 0x80) {
        if (*used + 1 >= cap) return 0;
        out[(*used)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*used + 2 >= cap) return 0;
        out[(*used)++] = (char)(0xC0 | (cp >> 6));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*used + 3 >= cap) return 0;
        out[(*used)++] = (char)(0xE0 | (cp >> 12));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*used + 4 >= cap) return 0;
        out[(*used)++] = (char)(0xF0 | (cp >> 18));
        out[(*used)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    }
    out[*used] = 0;
    return 1;
}

/* /Title is a PDF string: UTF-16BE with a BOM, or PDFDocEncoding, which
 * agrees with Latin-1 for every printable character we can draw. */
static void decode_text_string(const u8 *s, int n, char *out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    if (!s || n <= 0) return;
    if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
        for (int i = 2; i + 1 < n; i += 2) {
            u32 cp = ((u32)s[i] << 8) | s[i + 1];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
                u32 lo = ((u32)s[i + 2] << 8) | s[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (!pdf_utf8_put(out, cap, &used, cp)) break;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        u32 cp = s[i];
        if (cp < 0x20 && cp != '\t') cp = ' ';
        if (!pdf_utf8_put(out, cap, &used, cp)) break;
    }
}

static u8 *read_whole_file(const char *path, size_t *len, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "%s: cannot seek: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        snprintf(err, errsz, "%s: cannot measure: %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    if (n > (long)(256u << 20)) {
        snprintf(err, errsz, "%s: %ld bytes is larger than the 256 MB limit", path, n);
        fclose(f);
        return NULL;
    }
    rewind(f);
    u8 *p = malloc((size_t)n ? (size_t)n : 1);
    if (!p) {
        snprintf(err, errsz, "%s: out of memory for %ld bytes", path, n);
        fclose(f);
        return NULL;
    }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        snprintf(err, errsz, "%s: read %lu of %ld bytes", path, (unsigned long)got, n);
        free(p);
        return NULL;
    }
    *len = (size_t)n;
    return p;
}

static int find_startxref(PdfDoc *d, long *out) {
    size_t window = d->len < 4096 ? d->len : 4096;
    size_t start = d->len - window;
    for (size_t i = d->len; i > start + 9; i--) {
        if (memcmp(d->buf + i - 9, "startxref", 9) == 0) {
            size_t pos = i;
            long off = 0;
            if (parse_int_at(d->buf, d->len, &pos, &off)) { *out = off; return 1; }
            return 0;
        }
    }
    return 0;
}

static void doc_free(PdfDoc *d) {
    if (!d) return;
    ar_free(&d->arena);
    ar_free(&d->scratch);
    free(d->xref);
    free(d->pages);
    free(d->objs);
    free(d->buf);
    free(d);
}

static PdfDoc *doc_open_bytes(u8 *buf, size_t len, const char *path,
                              char *err, size_t errsz) {
    PdfDoc *d = calloc(1, sizeof(*d));
    if (!d) { snprintf(err, errsz, "out of memory for the document"); free(buf); return NULL; }
    ar_init(&d->arena);
    ar_init(&d->scratch);
    d->buf = buf;
    d->len = len;
    snprintf(d->path, sizeof(d->path), "%s", path ? path : "(memory)");

    /* %PDF- may be preceded by up to 1024 bytes of junk, which the spec
     * allows and mail gateways produce. */
    size_t hdr = (size_t)-1;
    size_t lim = len < 1024 ? len : 1024;
    for (size_t i = 0; i + 5 <= lim; i++)
        if (memcmp(buf + i, "%PDF-", 5) == 0) { hdr = i; break; }
    if (hdr == (size_t)-1) {
        snprintf(err, errsz, "%s: no %%PDF- header in the first 1024 bytes", d->path);
        doc_free(d);
        return NULL;
    }
    d->vmaj = (hdr + 5 < len && buf[hdr + 5] >= '0' && buf[hdr + 5] <= '9') ? buf[hdr + 5] - '0' : 1;
    d->vmin = (hdr + 7 < len && buf[hdr + 6] == '.' && buf[hdr + 7] >= '0' && buf[hdr + 7] <= '9')
              ? buf[hdr + 7] - '0' : 0;

    long sx = 0;
    int have_start = find_startxref(d, &sx);
    int ok = 0;
    if (have_start) {
        d->err[0] = 0;
        ok = parse_xref_section_chain(d, sx, 0);
        if (!ok) {
            /* Keep the reason for the fallback out of the way, but remember
             * that it happened: recovery changes what the offsets mean. */
            d->err[0] = 0;
            d->err_off = 0;
        }
    }
    if (!ok) {
        free(d->xref);
        d->xref = NULL;
        d->xref_cap = 0;
        d->xref_size = 0;
        d->trailer = NULL;
        if (!xref_reconstruct(d)) {
            snprintf(err, errsz, "%s: %s (at byte %ld)", d->path, d->err, d->err_off);
            doc_free(d);
            return NULL;
        }
    }

    if (dict_get(d->trailer, "Encrypt")) {
        d->encrypted = 1;
        snprintf(err, errsz, "%s: the file is encrypted (/Encrypt); the standard "
                 "security handler is not implemented", d->path);
        doc_free(d);
        return NULL;
    }

    if (d->xref_size <= 0 || d->xref_size > PDF_MAX_OBJECTS) {
        snprintf(err, errsz, "%s: xref /Size %d is outside 1..%d", d->path,
                 d->xref_size, PDF_MAX_OBJECTS);
        doc_free(d);
        return NULL;
    }
    d->obj_count = d->xref_size;
    d->objs = calloc((size_t)d->obj_count, sizeof(*d->objs));
    if (!d->objs) {
        snprintf(err, errsz, "%s: out of memory for %d objects", d->path, d->obj_count);
        doc_free(d);
        return NULL;
    }

    PdfVal *root = obj_resolve(d, dict_get(d->trailer, "Root"), 0);
    if (!root || root->type != PV_DICT) {
        snprintf(err, errsz, "%s: trailer /Root does not point at a catalog (%s at byte %ld)",
                 d->path, d->err[0] ? d->err : "missing", d->err_off);
        doc_free(d);
        return NULL;
    }
    PdfVal *pages = obj_resolve(d, dict_get(root, "Pages"), 0);
    if (!pages || pages->type != PV_DICT) {
        snprintf(err, errsz, "%s: catalog has no usable /Pages tree", d->path);
        doc_free(d);
        return NULL;
    }
    if (!pages_walk(d, pages, 0, NULL, NULL, NULL, 0, 0, NULL, 0)) {
        snprintf(err, errsz, "%s: %s (at byte %ld)", d->path, d->err, d->err_off);
        doc_free(d);
        return NULL;
    }
    if (d->npages == 0) {
        snprintf(err, errsz, "%s: the page tree is empty", d->path);
        doc_free(d);
        return NULL;
    }

    /* Walk each page's /Font dictionary once so PdfDocStats describes the
     * file as it is, not only the pages somebody has rendered. */
    for (int i = 0; i < d->npages; i++) {
        PdfVal *fonts = obj_resolve(d, dict_get(d->pages[i].resources, "Font"), 0);
        if (!fonts || fonts->type != PV_DICT) continue;
        for (int k = 0; k < fonts->u.dict.count; k++) {
            PdfVal *fd = obj_resolve(d, fonts->u.dict.e[k].val, 0);
            if (!fd || fd->type != PV_DICT) continue;
            RFont tmp;
            font_parse(d, &tmp, fd);
        }
    }

    PdfVal *info = obj_resolve(d, dict_get(d->trailer, "Info"), 0);
    PdfVal *title = obj_resolve(d, dict_get(info, "Title"), 0);
    if (title && title->type == PV_STR)
        decode_text_string(title->u.str.s, title->u.str.len, d->title, sizeof(d->title));
    if (!d->title[0]) {
        const char *base = strrchr(d->path, '/');
        if (!base) base = strrchr(d->path, '\\');
        snprintf(d->title, sizeof(d->title), "%s", base ? base + 1 : d->path);
    }

    d->load_state = 1;
    return d;
}

PDF_API PdfDoc *pdf_open(const char *path, char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!path || !path[0]) {
        if (err) snprintf(err, errsz, "no path given");
        return NULL;
    }
    size_t len = 0;
    char why[256] = "";
    u8 *buf = read_whole_file(path, &len, why, sizeof(why));
    if (!buf) {
        if (err) snprintf(err, errsz, "%s", why);
        return NULL;
    }
    return doc_open_bytes(buf, len, path, err, errsz);
}

PDF_API PdfDoc *pdf_open_memory(const void *bytes, size_t len, char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!bytes || !len) {
        if (err) snprintf(err, errsz, "no data");
        return NULL;
    }
    u8 *copy = malloc(len);
    if (!copy) {
        if (err) snprintf(err, errsz, "out of memory");
        return NULL;
    }
    memcpy(copy, bytes, len);
    return doc_open_bytes(copy, len, "(memory)", err, errsz);
}

PDF_API void pdf_close(PdfDoc *d) { doc_free(d); }

PDF_API int pdf_page_count(const PdfDoc *d) { return d ? d->npages : 0; }

PDF_API int pdf_page_info(const PdfDoc *d, int page, PdfPageInfo *out) {
    if (!d || !out || page < 0 || page >= d->npages) return -1;
    const PdfPage *p = &d->pages[page];
    double w = p->box[2] - p->box[0];
    double h = p->box[3] - p->box[1];
    if (p->rotate == 90 || p->rotate == 270) {
        out->width = h;
        out->height = w;
    } else {
        out->width = w;
        out->height = h;
    }
    out->media_w = w;
    out->media_h = h;
    out->rotate = p->rotate;
    return 0;
}

PDF_API const char *pdf_title(const PdfDoc *d) { return d ? d->title : ""; }
PDF_API const char *pdf_error(const PdfDoc *d) { return d ? d->err : ""; }
PDF_API long pdf_error_offset(const PdfDoc *d) { return d ? d->err_off : 0; }
PDF_API int pdf_version_major(const PdfDoc *d) { return d ? d->vmaj : 0; }
PDF_API int pdf_version_minor(const PdfDoc *d) { return d ? d->vmin : 0; }
PDF_API int pdf_encrypted(const PdfDoc *d) { return d ? d->encrypted : 0; }

PDF_API void pdf_doc_stats(const PdfDoc *d, PdfDocStats *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    if (!d) return;
    st->pages = d->npages;
    st->xref_tables = d->n_xref_tables;
    st->xref_streams = d->n_xref_streams;
    st->object_streams = d->n_objstms;
    st->reconstructed = d->reconstructed;
    st->embedded_fonts = d->st_embedded_fonts;
    st->type0_fonts = d->st_type0_fonts;
    st->encrypted = d->encrypted;
    st->pages_truncated = d->pages_truncated;
}
/* ------------------------------------------------------------
 *  Geometry
 *
 *  A PDF matrix [a b c d e f] maps (x,y) to
 *      x' = a*x + c*y + e
 *      y' = b*x + d*y + f
 *  exactly as section 8.3.3 of the specification writes it.
 * ------------------------------------------------------------ */
typedef struct { double a, b, c, d, e, f; } Mat;

static Mat mat_identity(void) {
    Mat m = { 1, 0, 0, 1, 0, 0 };
    return m;
}
static Mat mat_translate(double tx, double ty) {
    Mat m = { 1, 0, 0, 1, tx, ty };
    return m;
}
/* apply A, then B */
static Mat mat_mul(Mat A, Mat B) {
    Mat m;
    m.a = A.a * B.a + A.b * B.c;
    m.b = A.a * B.b + A.b * B.d;
    m.c = A.c * B.a + A.d * B.c;
    m.d = A.c * B.b + A.d * B.d;
    m.e = A.e * B.a + A.f * B.c + B.e;
    m.f = A.e * B.b + A.f * B.d + B.f;
    return m;
}
static void mat_apply(Mat m, double x, double y, double *ox, double *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}
static double mat_scale_of(Mat m) {
    double det = m.a * m.d - m.b * m.c;
    if (det < 0) det = -det;
    return sqrt(det);
}

static int utf8_encode(u32 cp, char out[5]) {
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
        return 3;
    }
    out[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = 0;
    return 4;
}

/* ------------------------------------------------------------
 *  Font widths
 *
 *  For a standard-14 font with no /Widths, the advance has to come
 *  from the face we are going to draw. font_get() applies the desktop
 *  text scale, so the size actually used is recovered from
 *  font_height()/1.4 (font_height is size*1.4 by construction) and the
 *  advance is divided by it. What comes out is a width in 1/1000 of the
 *  em as stb_truetype scales it - the same scale used for the glyphs,
 *  which is what keeps text from overlapping itself.
 * ------------------------------------------------------------ */
static float ttf_advance_units(int face, u32 cp) {
    if (!cp) return 0.0f;
    Font *f = font_get(face, 40.0f);
    if (!f) return 500.0f;
    float px = font_height(f) / 1.4f;
    if (px < 0.5f) return 500.0f;
    char u[5];
    utf8_encode(cp, u);
    return font_width(f, u) / px * 1000.0f;
}

static float font_width_units(RFont *f, int code) {
    if (f->has_widths && code >= f->first && code <= f->last && f->w[code] > 0.0f)
        return f->w[code];
    if (f->w_measured[code]) return f->w[code];
    u32 cp = f->uni[code];
    float w;
    if (cp) w = ttf_advance_units(f->face, cp);
    else if (f->has_missing) w = f->missing;
    else w = 0.0f;
    f->w[code] = w;
    f->w_measured[code] = 1;
    return w;
}

/* ------------------------------------------------------------
 *  A render context: graphics state, the path being built, the
 *  resources of the page (or Form XObject), and the two sinks
 *  (pixels and extracted text).
 * ------------------------------------------------------------ */
typedef struct {
    Mat    ctm;
    double fill[4];
    int    fill_n;
    double stroke[4];
    int    stroke_n;
    double line_width;
    double char_space, word_space, hscale, leading, rise;
    int    text_mode;
    double fill_alpha, stroke_alpha;
    int    clip_x0, clip_y0, clip_x1, clip_y1;   /* device rect, current clip */
} GState;

typedef struct {
    double *x, *y;
    u8     *mv;          /* point starts a new subpath */
    int     n;
    double  cx, cy;      /* current point */
    double  sx, sy;      /* current subpath start */
    int     pending_clip;
    int     pending_evenodd;
} Path;

typedef struct {
    PdfVal *dict;
    RFont  *f;
} FontCache;

typedef struct Ctx {
    PdfDoc  *d;
    Surface *s;              /* NULL when extracting text */
    Mat      base;           /* user space -> view pixels */
    double   zoom;
    GState   gs;
    GState   stack[32];
    int      sp;
    Path     path;
    PdfVal  *resources;
    FontCache fonts[PDF_MAX_FONTS];
    int      nfonts;
    RFont   *font;
    double   font_size;
    Mat      tm, tlm;
    int      form_depth;

    /* text extraction */
    char    *text;
    size_t   textlen, textcap;
    int      text_truncated;
    int      text_pending_nl;
    int      have_tm;
    double   last_tm_e, last_tm_f;

    /* per-render scratch */
    double  *cross;          /* crossings for the scanline filler */
    int     *cross_dir;
    double  *xs;             /* sorted crossings for one sub-scanline */
    int     *xdir;
    float   *cov;            /* coverage row */
    int      cov_w;
    int      clip_w, clip_h;
    long     base_off;       /* file offset of the content being run */

    PdfRenderInfo *info;
    int      draw;
} Ctx;

static void ctx_clip_reset(Ctx *c) {
    c->gs.clip_x0 = 0;
    c->gs.clip_y0 = 0;
    c->gs.clip_x1 = c->clip_w;
    c->gs.clip_y1 = c->clip_h;
}

static void info_problem(Ctx *c, const char *what, long off) {
    if (!c->info) return;
    c->info->unsupported_ops++;
    if (!c->info->first_problem[0]) {
        snprintf(c->info->first_problem, sizeof(c->info->first_problem), "%s", what);
        c->info->first_problem_offset = off;
    }
}

/* ------------------------------------------------------------
 *  Colour
 * ------------------------------------------------------------ */
static u32 colour_from(const double *v, int n, double alpha) {
    double r = 0, g = 0, b = 0;
    if (n == 1) {
        r = g = b = v[0];
    } else if (n == 3) {
        r = v[0]; g = v[1]; b = v[2];
    } else if (n == 4) {
        double k = v[3];
        r = (1.0 - v[0]) * (1.0 - k);
        g = (1.0 - v[1]) * (1.0 - k);
        b = (1.0 - v[2]) * (1.0 - k);
    }
    int ir = (int)(r * 255.0 + 0.5), ig = (int)(g * 255.0 + 0.5), ib = (int)(b * 255.0 + 0.5);
    if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    int a = (int)(alpha * 255.0 + 0.5);
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((u32)a << 24) | ((u32)ir << 16) | ((u32)ig << 8) | (u32)ib;
}

static u32 fill_colour(const Ctx *c) { return colour_from(c->gs.fill, c->gs.fill_n, c->gs.fill_alpha); }

static void blend_pixel(Surface *s, int x, int y, u32 col, float cov, int cx0, int cy0, int cx1, int cy1) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1) return;
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    float a = GET_A(col) / 255.0f * cov;
    if (a <= 0.002f) return;
    if (a > 1.0f) a = 1.0f;
    u32 *px = &s->px[(size_t)y * s->stride + x];
    u32 dst = *px;
    int inv = (int)((1.0f - a) * 255.0f);
    int sa = (int)(a * 255.0f);
    int r = (GET_R(col) * sa + GET_R(dst) * inv) / 255;
    int g = (GET_G(col) * sa + GET_G(dst) * inv) / 255;
    int b = (GET_B(col) * sa + GET_B(dst) * inv) / 255;
    *px = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* ------------------------------------------------------------
 *  Path construction (already transformed to device space)
 * ------------------------------------------------------------ */
static void path_reset(Ctx *c) {
    c->path.n = 0;
    c->path.pending_clip = 0;
    c->path.pending_evenodd = 0;
    c->path.cx = c->path.cy = 0;
    c->path.sx = c->path.sy = 0;
}

static int path_add(Ctx *c, double x, double y, int move) {
    Path *p = &c->path;
    if (p->n >= PDF_MAX_PATH) {
        info_problem(c, "path with more than 8192 points", c->base_off);
        return 0;
    }
    p->x[p->n] = x;
    p->y[p->n] = y;
    p->mv[p->n] = (u8)move;
    p->n++;
    return 1;
}

static void path_move(Ctx *c, double x, double y) {
    double dx, dy;
    mat_apply(c->gs.ctm, x, y, &dx, &dy);
    c->path.cx = c->path.sx = dx;
    c->path.cy = c->path.sy = dy;
    path_add(c, dx, dy, 1);
}

static void path_line(Ctx *c, double x, double y) {
    double dx, dy;
    mat_apply(c->gs.ctm, x, y, &dx, &dy);
    path_add(c, dx, dy, 0);
    c->path.cx = dx;
    c->path.cy = dy;
}

/* Flatten a cubic Bezier in device space. The step count follows the
 * control polygon's length, so a small glyph-sized curve gets few points
 * and a page-wide arc gets enough to look round. */
static void path_curve(Ctx *c, double x1, double y1, double x2, double y2,
                       double x3, double y3) {
    double d1x, d1y, d2x, d2y, d3x, d3y;
    mat_apply(c->gs.ctm, x1, y1, &d1x, &d1y);
    mat_apply(c->gs.ctm, x2, y2, &d2x, &d2y);
    mat_apply(c->gs.ctm, x3, y3, &d3x, &d3y);
    double x0 = c->path.cx, y0 = c->path.cy;
    double len = hypot(d1x - x0, d1y - y0) + hypot(d2x - d1x, d2y - d1y) +
                 hypot(d3x - d2x, d3y - d2y);
    int steps = (int)(len / 3.0);
    if (steps < 4) steps = 4;
    if (steps > 64) steps = 64;
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps, mt = 1.0 - t;
        double px = mt * mt * mt * x0 + 3 * mt * mt * t * d1x +
                    3 * mt * t * t * d2x + t * t * t * d3x;
        double py = mt * mt * mt * y0 + 3 * mt * mt * t * d1y +
                    3 * mt * t * t * d2y + t * t * t * d3y;
        path_add(c, px, py, 0);
    }
    c->path.cx = d3x;
    c->path.cy = d3y;
}

static void path_close(Ctx *c) {
    if (c->path.n == 0) return;
    if (c->path.cx != c->path.sx || c->path.cy != c->path.sy)
        path_add(c, c->path.sx, c->path.sy, 0);
    c->path.cx = c->path.sx;
    c->path.cy = c->path.sy;
}

/* ------------------------------------------------------------
 *  Scanline fill with non-zero or even-odd winding.
 *
 *  Four sub-scanlines per pixel row give vertical antialiasing, and a
 *  span's partial end pixels are covered fractionally, so a 0.5px rule
 *  is grey rather than invisible.  Subpaths are closed implicitly,
 *  which is what every fill and stroke operator does to a path in PDF.
 * ------------------------------------------------------------ */
static void span_add(Ctx *c, double xa, double xb, float weight) {
    if (xb <= xa) return;
    int x0 = c->gs.clip_x0, x1 = c->gs.clip_x1;
    if (xa < x0) xa = x0;
    if (xb > x1) xb = x1;
    if (xb <= xa) return;
    int ia = (int)floor(xa), ib = (int)ceil(xb);
    if (ia < x0) ia = x0;
    if (ib > x1) ib = x1;
    for (int x = ia; x < ib; x++) {
        double l = xa > x ? xa : x;
        double r = xb < x + 1 ? xb : x + 1;
        if (r > l) c->cov[x - x0] += (float)((r - l) * weight);
    }
}

/* Fill the current path. `x`/`y` point at device-space vertices. */
static void fill_scan(Ctx *c, int even_odd) {
    Path *p = &c->path;
    if (p->n < 2 || !c->s) return;
    int cx0 = c->gs.clip_x0, cx1 = c->gs.clip_x1;
    int cy0 = c->gs.clip_y0, cy1 = c->gs.clip_y1;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    double miny = p->y[0], maxy = p->y[0];
    for (int i = 1; i < p->n; i++) {
        if (p->y[i] < miny) miny = p->y[i];
        if (p->y[i] > maxy) maxy = p->y[i];
    }
    int y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y1 <= y0) return;

    /* Edges: consecutive points of the same subpath, plus the implicit
     * closing edge for every subpath. */
    int nseg = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->mv[i]) {
            int j = i + 1;
            while (j < p->n && !p->mv[j]) j++;
            nseg += (j - i + 1);                   /* open edges + one closing edge */
        }
    }
    if (nseg <= 0) return;
    double *seg = c->cross;                        /* 4 doubles per segment */
    int *dir = c->cross_dir;
    if (nseg > PDF_MAX_PATH + 1) nseg = PDF_MAX_PATH + 1;

    int ns = 0;
    for (int i = 0; i < p->n && ns < nseg; i++) {
        if (!p->mv[i]) continue;
        int j = i;
        while (j + 1 < p->n && !p->mv[j + 1]) j++;
        int first = i, last = j;
        for (int k = first; k <= last && ns < nseg; k++) {
            int k2 = (k == last) ? first : k + 1;
            if (p->y[k] == p->y[k2]) continue;
            seg[ns * 4 + 0] = p->x[k];
            seg[ns * 4 + 1] = p->y[k];
            seg[ns * 4 + 2] = p->x[k2];
            seg[ns * 4 + 3] = p->y[k2];
            dir[ns] = p->y[k2] > p->y[k] ? 1 : -1;
            ns++;
        }
        i = j;
    }

    const int SS = 4;
    u32 col = fill_colour(c);
    for (int y = y0; y < y1; y++) {
        for (int k = 0; k < cx1 - cx0; k++) c->cov[k] = 0.0f;

        for (int ss = 0; ss < SS; ss++) {
            double sy = (double)y + (ss + 0.5) / SS;
            double *xs = c->xs;
            int   *ds = c->xdir;
            int   n = 0;
            for (int k = 0; k < ns; k++) {
                double ay = seg[k * 4 + 1], by = seg[k * 4 + 3];
                if ((sy >= ay && sy < by) || (sy >= by && sy < ay)) {
                    double t = (sy - ay) / (by - ay);
                    xs[n] = seg[k * 4 + 0] + t * (seg[k * 4 + 2] - seg[k * 4 + 0]);
                    ds[n] = dir[k];
                    n++;
                }
            }
            if (n < 2) continue;
            /* insertion sort by x, carrying the direction */
            for (int k = 1; k < n; k++) {
                double vx = xs[k];
                int vd = ds[k];
                int m = k - 1;
                while (m >= 0 && xs[m] > vx) { xs[m + 1] = xs[m]; ds[m + 1] = ds[m]; m--; }
                xs[m + 1] = vx;
                ds[m + 1] = vd;
            }
            float weight = 1.0f / SS;
            if (even_odd) {
                for (int k = 0; k + 1 < n; k += 2)
                    span_add(c, xs[k], xs[k + 1], weight);
            } else {
                int wind = 0;
                double start = 0;
                for (int k = 0; k < n; k++) {
                    int before = wind;
                    wind += ds[k];
                    if (before == 0 && wind != 0) start = xs[k];
                    else if (before != 0 && wind == 0) span_add(c, start, xs[k], weight);
                }
            }
        }

        for (int k = 0; k < cx1 - cx0; k++) {
            if (c->cov[k] > 0.002f) {
                blend_pixel(c->s, cx0 + k, y, col,
                            c->cov[k] > 1.0f ? 1.0f : c->cov[k],
                            cx0, cy0, cx1, cy1);
            }
        }
    }
}

/* A stroke segment as a filled quad. Joins become tiny gaps at very wide
 * line widths, which is honest for the widths documents actually use. */
static void stroke_segment(Ctx *c, double x0, double y0, double x1, double y1, double w) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = hypot(dx, dy);
    if (len < 1e-9 || w <= 0) return;
    double nx = -dy / len * w * 0.5, ny = dx / len * w * 0.5;
    Path *p = &c->path;
    int save = p->n;
    p->x[p->n] = x0 + nx; p->y[p->n] = y0 + ny; p->mv[p->n] = 1; p->n++;
    p->x[p->n] = x1 + nx; p->y[p->n] = y1 + ny; p->mv[p->n] = 0; p->n++;
    p->x[p->n] = x1 - nx; p->y[p->n] = y1 - ny; p->mv[p->n] = 0; p->n++;
    p->x[p->n] = x0 - nx; p->y[p->n] = y0 - ny; p->mv[p->n] = 0; p->n++;
    p->x[p->n] = x0 + nx; p->y[p->n] = y0 + ny; p->mv[p->n] = 0; p->n++;
    fill_scan(c, 0);
    p->n = save;
}

static void stroke_path(Ctx *c) {
    Path *p = &c->path;
    if (p->n < 2) return;
    double w = c->gs.line_width * mat_scale_of(c->gs.ctm);
    if (w < 0.35) w = 0.35;                        /* hairline stays visible */
    /* fill_scan() takes its colour from gs.fill; stroke colour is separate. */
    double sf[4];
    int sfn = c->gs.fill_n;
    memcpy(sf, c->gs.fill, sizeof(sf));
    double sa = c->gs.fill_alpha;
    memcpy(c->gs.fill, c->gs.stroke, sizeof(c->gs.fill));
    c->gs.fill_n = c->gs.stroke_n;
    c->gs.fill_alpha = c->gs.stroke_alpha;

    for (int i = 0; i < p->n; i++) {
        if (p->mv[i]) continue;
        stroke_segment(c, p->x[i - 1], p->y[i - 1], p->x[i], p->y[i], w);
    }
    /* re-close subpaths that were explicitly closed (h / re): the closing
     * point is present as a normal point whose neighbour is the start. */
    memcpy(c->gs.fill, sf, sizeof(sf));
    c->gs.fill_n = sfn;
    c->gs.fill_alpha = sa;
}

/* Rectangular clip: W n. Only an axis-aligned rectangle is honoured; any
 * other clip path is reported instead of being approximated by its box. */
static void apply_pending_clip(Ctx *c) {
    Path *p = &c->path;
    if (!p->pending_clip) return;
    p->pending_clip = 0;
    int n = p->n;
    int distinct = (n == 5 && p->x[4] == p->x[0] && p->y[4] == p->y[0]) ? 4 : n;
    if (distinct != 4) { info_problem(c, "non-rectangular clip path ignored", c->base_off); return; }
    double x0 = p->x[0], x1 = p->x[0], y0 = p->y[0], y1 = p->y[0];
    for (int i = 1; i < 4; i++) {
        if (p->x[i] < x0) x0 = p->x[i];
        if (p->x[i] > x1) x1 = p->x[i];
        if (p->y[i] < y0) y0 = p->y[i];
        if (p->y[i] > y1) y1 = p->y[i];
    }
    for (int i = 0; i < 4; i++) {
        int onx = (p->x[i] == x0 || p->x[i] == x1);
        int ony = (p->y[i] == y0 || p->y[i] == y1);
        if (!onx || !ony) { info_problem(c, "non-rectangular clip path ignored", c->base_off); return; }
    }
    int cx0 = (int)ceil(x0), cy0 = (int)ceil(y0);
    int cx1 = (int)floor(x1), cy1 = (int)floor(y1);
    if (cx0 > c->gs.clip_x0) c->gs.clip_x0 = cx0;
    if (cy0 > c->gs.clip_y0) c->gs.clip_y0 = cy0;
    if (cx1 < c->gs.clip_x1) c->gs.clip_x1 = cx1;
    if (cy1 < c->gs.clip_y1) c->gs.clip_y1 = cy1;
    if (c->gs.clip_x1 < c->gs.clip_x0) c->gs.clip_x1 = c->gs.clip_x0;
    if (c->gs.clip_y1 < c->gs.clip_y0) c->gs.clip_y1 = c->gs.clip_y0;
}

static void paint_path(Ctx *c, int fill, int stroke, int even_odd) {
#ifdef PDF_TRACE_PAINT
    fprintf(stderr, "paint fill=%d stroke=%d fill_n=%d (%.2f %.2f %.2f) stroke_n=%d
",
            fill, stroke, c->gs.fill_n, c->gs.fill[0], c->gs.fill[1], c->gs.fill[2],
            c->gs.stroke_n);
#endif
    apply_pending_clip(c);
    if (!c->s) { path_reset(c); return; }
    if (fill) fill_scan(c, even_odd);
    if (stroke) stroke_path(c);
    path_reset(c);
}

/* ------------------------------------------------------------
 *  Text
 * ------------------------------------------------------------ */
static void text_append_cp(Ctx *c, u32 cp) {
    if (!c->text || !cp) return;
    if (c->text_pending_nl) {
        if (c->textlen && c->text[c->textlen - 1] != '\n') {
            if (c->textlen + 1 >= c->textcap) c->text_truncated = 1;
            else c->text[c->textlen++] = '\n';
        }
        c->text_pending_nl = 0;
    }
    char u[5];
    int n = utf8_encode(cp, u);
    for (int i = 0; i < n; i++) {
        if (c->textlen + 1 >= c->textcap) { c->text_truncated = 1; return; }
        c->text[c->textlen++] = u[i];
    }
    c->text[c->textlen] = 0;
}

static RFont *font_for(Ctx *c, const char *name) {
    /* cached by dictionary identity; a font dict object is parsed once per
     * document, so the pointer is stable while the doc lives. */
    PdfVal *fonts = obj_resolve(c->d, dict_get(c->resources, "Font"), 0);
    PdfVal *fd = obj_resolve(c->d, dict_get(fonts, name), 0);
    if (!fd || fd->type != PV_DICT) return NULL;
    for (int i = 0; i < c->nfonts; i++)
        if (c->fonts[i].dict == fd) return c->fonts[i].f;
    if (c->nfonts >= PDF_MAX_FONTS) {
        info_problem(c, "more than 256 fonts on a page", c->base_off);
        return NULL;
    }
    RFont *f = ar_alloc(&c->d->scratch, sizeof(*f));
    if (!f) return NULL;
    font_parse(c->d, f, fd);
    c->fonts[c->nfonts].dict = fd;
    c->fonts[c->nfonts].f = f;
    c->nfonts++;
    return f;
}

static void text_show(Ctx *c, const u8 *bytes, int len) {
    RFont *f = c->font;
    if (!f) return;
    if (!f->valid || f->type0) {
        if (c->info) c->info->unmapped_text_runs++;
        return;                                    /* counted, never faked */
    }
    if (fabs(c->tm.b) > 1e-9 || fabs(c->tm.c) > 1e-9) {
        if (c->info) c->info->rotated_text_runs++;
    }
    for (int i = 0; i < len; i++) {
        int code = bytes[i];
        u32 uni = f->uni[code];
        float wu = font_width_units(f, code);
        double adv = ((double)wu / 1000.0 * c->font_size + c->gs.char_space +
                      (uni == 0x20 ? c->gs.word_space : 0.0)) * c->gs.hscale;

        if (c->draw && c->s && uni) {
            Mat tsz = { c->font_size * c->gs.hscale, 0, 0, c->font_size, 0, c->gs.rise };
            Mat trm = mat_mul(mat_mul(tsz, c->tm), c->gs.ctm);
            double ox = trm.e, oy = trm.f;
            double size_px = hypot(trm.b, trm.d);
            int mode = c->gs.text_mode;
            if (size_px >= 0.5 && (mode == 0 || mode == 2 || mode == 4 || mode == 6)) {
                float scale = ui_scale_get();
                Font *ff = font_get(f->face, (float)(size_px / scale));
                if (ff) {
                    char u[5];
                    utf8_encode(uni, u);
                    u32 col = fill_colour(c);
                    /* font_draw()'s y is the top of the ascent box. */
                    double top = oy - font_ascent(ff);
                    /* Glyphs are clipped as whole glyphs: the page view clips
                     * the surface, and this rejects the ones outside the
                     * current clip rectangle. */
                    int visible = (ox + size_px > c->gs.clip_x0 && ox < c->gs.clip_x1 &&
                                   oy + size_px * 0.3 > c->gs.clip_y0 && top < c->gs.clip_y1);
                    if (visible) {
                        font_draw(c->s, ff, (float)ox, (float)top, u, col);
                        if (c->info) {
                            c->info->glyphs++;
                            c->info->glyphs_visible++;
                        }
                    }
                }
            }
        }
        if (!c->draw) text_append_cp(c, uni);

        c->tm.e += adv * c->tm.a;
        c->tm.f += adv * c->tm.b;
    }
}

/* ------------------------------------------------------------
 *  Content lexer
 * ------------------------------------------------------------ */
typedef struct {
    Ctx        *c;
    const u8   *b;
    size_t      len, pos;
    size_t      op_pos;
} CLex;

/* 0 = end, 1 = operand in *val, 2 = operator in op, -1 = error */
static int cl_next(CLex *L, PdfVal **val, char *op, size_t opsz) {
    skip_ws(L->b, L->len, &L->pos);
    if (L->pos >= L->len) return 0;
    int ch = L->b[L->pos];

    int is_value = (ch == '[' || ch == '/' || ch == '(' || ch == '<' ||
                    ch == '+' || ch == '-' || ch == '.' || (ch >= '0' && ch <= '9') ||
                    at_word(L->b, L->len, L->pos, "true") ||
                    at_word(L->b, L->len, L->pos, "false") ||
                    at_word(L->b, L->len, L->pos, "null"));
    if (is_value) {
        *val = parse_value(L->c->d, &L->c->d->scratch, L->b, L->len, &L->pos, 0);
        if (!*val) return -1;
        op[0] = 0;
        return 1;
    }
    size_t start = L->pos;
    L->op_pos = start;
    while (L->pos < L->len && pdf_is_regular(L->b[L->pos])) L->pos++;
    size_t n = L->pos - start;
    if (n == 0 || n >= opsz) {
        pdf_set_err(L->c->d, L->c->base_off + (long)L->pos,
                    "unexpected byte 0x%02X in a content stream", (unsigned)L->b[L->pos]);
        return -1;
    }
    memcpy(op, L->b + start, n);
    op[n] = 0;
    *val = NULL;
    return 2;
}

/* Exactly `n` numbers at the top of the operand stack. */
static int take(PdfVal **st, int sp, int n, double *out) {
    if (n < 0 || sp < n) return 0;
    for (int i = 0; i < n; i++)
        if (!val_num(st[sp - n + i], &out[i])) return 0;
    return 1;
}

static const char *take_name(PdfVal **st, int sp) {
    if (sp < 1) return NULL;
    PdfVal *v = st[sp - 1];
    return (v && v->type == PV_NAME) ? v->u.name.s : NULL;
}

/* ------------------------------------------------------------
 *  XObjects
 * ------------------------------------------------------------ */
static int run_content(Ctx *c, const u8 *data, size_t len, int form_depth);

static void draw_xobject(Ctx *c, const char *name) {
    PdfVal *xobjs = obj_resolve(c->d, dict_get(c->resources, "XObject"), 0);
    PdfVal *xo = obj_resolve(c->d, dict_get(xobjs, name), 0);
    if (!xo || xo->type != PV_STREAM) {
        info_problem(c, "Do with an unknown XObject", c->base_off);
        return;
    }
    PdfVal *sub = obj_resolve(c->d, dict_get(xo, "Subtype"), 0);
    if (name_is(sub, "Image")) {
        if (c->info) c->info->images_skipped++;
        return;
    }
    if (!name_is(sub, "Form")) {
        info_problem(c, "XObject with an unsupported subtype", c->base_off);
        return;
    }
    if (c->form_depth >= PDF_MAX_FORM_DEPTH) {
        info_problem(c, "Form XObjects nested too deeply", c->base_off);
        return;
    }

    const u8 *data = NULL;
    size_t len = 0;
    if (!stream_data(c->d, xo, &data, &len)) {
        info_problem(c, xo->u.stream.why, xo->u.stream.raw_off);
        return;
    }

    GState saved = c->gs;
    PdfVal *m = obj_resolve(c->d, dict_get(xo, "Matrix"), 0);
    if (m && m->type == PV_ARRAY && m->u.arr.count >= 6) {
        double v[6];
        int ok = 1;
        for (int i = 0; i < 6; i++) if (!val_num(m->u.arr.items[i], &v[i])) ok = 0;
        if (ok) {
            Mat mm = { v[0], v[1], v[2], v[3], v[4], v[5] };
            c->gs.ctm = mat_mul(mm, c->gs.ctm);
        }
    }
    PdfVal *res = obj_resolve(c->d, dict_get(xo, "Resources"), 0);
    PdfVal *saved_res = c->resources;
    if (res && res->type == PV_DICT) c->resources = res;
    c->form_depth++;
    if (c->info) c->info->forms_drawn++;
    if (!run_content(c, data, len, c->form_depth))
        info_problem(c, "Form XObject content failed", xo->u.stream.raw_off);
    c->form_depth--;
    c->resources = saved_res;
    c->gs = saved;
}

/* Inline images: BI <dict> ID <binary> EI. The binary is stepped over by
 * scanning for a whitespace-delimited EI, which is what viewers do; the
 * pixels are not decoded and are counted as skipped. */
static int skip_inline_image(CLex *L, Ctx *c) {
    PdfVal *v = NULL;
    char op[32];
    /* parameters up to ID */
    for (;;) {
        int r = cl_next(L, &v, op, sizeof(op));
        if (r == 0) return 0;
        if (r < 0) return -1;
        if (r == 2 && !strcmp(op, "ID")) break;
    }
    if (L->pos < L->len && pdf_is_ws(L->b[L->pos])) L->pos++;
    size_t p = L->pos;
    int found = 0;
    while (p + 1 < L->len) {
        if (L->b[p] == 'E' && L->b[p + 1] == 'I' &&
            (p + 2 >= L->len || pdf_is_ws(L->b[p + 2])) &&
            (p == 0 || pdf_is_ws(L->b[p - 1]))) {
            found = 1;
            break;
        }
        p++;
    }
    if (!found) {
        pdf_set_err(c->d, c->base_off + (long)L->pos, "inline image has no EI terminator");
        return -1;
    }
    L->pos = p + 2;
    if (c->info) c->info->inline_images_skipped++;
    return 1;
}

/* ------------------------------------------------------------
 *  The operator dispatcher
 * ------------------------------------------------------------ */
static int exec_op(Ctx *c, const char *op, PdfVal **st, int sp, long off) {
    double v[6];
    GState *g = &c->gs;
    if (c->info) c->info->ops++;

    if (!strcmp(op, "q")) {
        if (c->sp >= (int)(sizeof(c->stack) / sizeof(c->stack[0]))) {
            info_problem(c, "q deeper than 32 levels", off);
            return 0;
        }
        c->stack[c->sp++] = c->gs;
        return 1;
    }
    if (!strcmp(op, "Q")) {
        if (c->sp <= 0) return 0;                  /* stray Q: ignore */
        c->gs = c->stack[--c->sp];
        return 1;
    }
    if (!strcmp(op, "cm")) {
        if (!take(st, sp, 6, v)) { info_problem(c, "cm with bad operands", off); return 0; }
        Mat m = { v[0], v[1], v[2], v[3], v[4], v[5] };
        g->ctm = mat_mul(m, g->ctm);
        return 1;
    }
    if (!strcmp(op, "w")) {
        if (!take(st, sp, 1, v)) return 0;
        g->line_width = v[0];
        return 1;
    }
    if (!strcmp(op, "J") || !strcmp(op, "j") || !strcmp(op, "M")) return 0;
    if (!strcmp(op, "d")) { if (c->info) c->info->dashes_ignored++; return 0; }
    if (!strcmp(op, "ri") || !strcmp(op, "i")) return 0;

    if (!strcmp(op, "gs")) {
        const char *name = take_name(st, sp);
        PdfVal *e = obj_resolve(c->d, dict_get(obj_resolve(c->d, dict_get(c->resources, "ExtGState"), 0), name), 0);
        if (e) {
            double a;
            if (val_num(obj_resolve(c->d, dict_get(e, "ca"), 0), &a)) g->fill_alpha = a;
            if (val_num(obj_resolve(c->d, dict_get(e, "CA"), 0), &a)) g->stroke_alpha = a;
        }
        return 0;
    }

    if (!strcmp(op, "rg") || !strcmp(op, "RG")) {
        if (!take(st, sp, 3, v)) { info_problem(c, "rg with bad operands", off); return 0; }
        if (op[1] == 'g') { for (int i = 0; i < 3; i++) g->fill[i] = v[i]; g->fill_n = 3; }
        else { for (int i = 0; i < 3; i++) g->stroke[i] = v[i]; g->stroke_n = 3; }
        return 1;
    }
    if (!strcmp(op, "g") || !strcmp(op, "G")) {
        if (!take(st, sp, 1, v)) return 0;
        if (op[0] == 'g') { g->fill[0] = v[0]; g->fill_n = 1; }
        else { g->stroke[0] = v[0]; g->stroke_n = 1; }
        return 1;
    }
    if (!strcmp(op, "k") || !strcmp(op, "K")) {
        if (!take(st, sp, 4, v)) { info_problem(c, "k with bad operands", off); return 0; }
        if (op[0] == 'k') { for (int i = 0; i < 4; i++) g->fill[i] = v[i]; g->fill_n = 4; }
        else { for (int i = 0; i < 4; i++) g->stroke[i] = v[i]; g->stroke_n = 4; }
        return 1;
    }
    if (!strcmp(op, "cs") || !strcmp(op, "CS")) {
        const char *n = take_name(st, sp);
        int comps = -1;
        if (n) {
            if (!strcmp(n, "DeviceGray") || !strcmp(n, "CalGray") || !strcmp(n, "G")) comps = 1;
            else if (!strcmp(n, "DeviceRGB") || !strcmp(n, "CalRGB") || !strcmp(n, "RGB")) comps = 3;
            else if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK")) comps = 4;
        }
        if (comps < 0) {
            if (c->info) c->info->colour_spaces_ignored++;
            return 0;
        }
        if (op[0] == 'c') g->fill_n = comps;
        else g->stroke_n = comps;
        return 1;
    }
    if (!strcmp(op, "sc") || !strcmp(op, "scn") || !strcmp(op, "SC") || !strcmp(op, "SCN")) {
        int want = (op[0] == 's') ? g->fill_n : g->stroke_n;
        if (want <= 0) return 0;
        if (sp < want || !take(st, sp, want, v)) return 0;
        if (op[0] == 's') { for (int i = 0; i < want; i++) g->fill[i] = v[i]; }
        else { for (int i = 0; i < want; i++) g->stroke[i] = v[i]; }
        return 1;
    }

    if (!strcmp(op, "BT")) {
        c->tm = mat_identity();
        c->tlm = mat_identity();
        c->have_tm = 0;
        return 1;
    }
    if (!strcmp(op, "ET")) {
        c->text_pending_nl = 1;
        return 1;
    }
    if (!strcmp(op, "Tf")) {
        double size = 0;
        const char *name = (sp >= 2 && st[sp - 2] && st[sp - 2]->type == PV_NAME)
                           ? st[sp - 2]->u.name.s : NULL;
        if (!name || !val_num(st[sp - 1], &size)) {
            info_problem(c, "Tf with bad operands", off);
            return 0;
        }
        RFont *f = font_for(c, name);
        if (!f) {
            info_problem(c, "text uses a font that is not in /Resources", off);
            c->font = NULL;
            return 0;
        }
        c->font = f;
        c->font_size = size;
        return 1;
    }
    if (!strcmp(op, "Tc")) { if (!take(st, sp, 1, v)) return 0; g->char_space = v[0]; return 1; }
    if (!strcmp(op, "Tw")) { if (!take(st, sp, 1, v)) return 0; g->word_space = v[0]; return 1; }
    if (!strcmp(op, "Tz")) { if (!take(st, sp, 1, v)) return 0; g->hscale = v[0] / 100.0; return 1; }
    if (!strcmp(op, "TL")) { if (!take(st, sp, 1, v)) return 0; g->leading = v[0]; return 1; }
    if (!strcmp(op, "Ts")) { if (!take(st, sp, 1, v)) return 0; g->rise = v[0]; return 1; }
    if (!strcmp(op, "Tr")) {
        if (!take(st, sp, 1, v)) return 0;
        g->text_mode = (int)v[0];
        return 1;
    }
    if (!strcmp(op, "Td") || !strcmp(op, "TD")) {
        if (!take(st, sp, 2, v)) { info_problem(c, "Td with bad operands", off); return 0; }
        c->tlm = mat_mul(mat_translate(v[0], v[1]), c->tlm);
        c->tm = c->tlm;
        if (op[1] == 'D') g->leading = -v[1];
        if (c->text) {
            if (v[1] != 0) c->text_pending_nl = 1;
            else if (v[0] > 0 && c->textlen && c->text[c->textlen - 1] != '\n' &&
                     c->text[c->textlen - 1] != ' ')
                text_append_cp(c, ' ');
        }
        c->have_tm = 0;
        return 1;
    }
    if (!strcmp(op, "Tm")) {
        if (!take(st, sp, 6, v)) { info_problem(c, "Tm with bad operands", off); return 0; }
        Mat m = { v[0], v[1], v[2], v[3], v[4], v[5] };
        c->tm = c->tlm = m;
        if (c->text && c->textlen) {
            if (c->have_tm && fabs(m.f - c->last_tm_f) > 0.5) c->text_pending_nl = 1;
            else if (c->have_tm && m.e > c->last_tm_e + 1 &&
                     c->text[c->textlen - 1] != ' ' && c->text[c->textlen - 1] != '\n')
                text_append_cp(c, ' ');
        }
        c->last_tm_e = m.e;
        c->last_tm_f = m.f;
        c->have_tm = 1;
        return 1;
    }
    if (!strcmp(op, "T*")) {
        c->tlm = mat_mul(mat_translate(0, -g->leading), c->tlm);
        c->tm = c->tlm;
        c->text_pending_nl = 1;
        c->have_tm = 0;
        return 1;
    }
    if (!strcmp(op, "Tj")) {
        if (sp < 1 || !st[sp - 1] || st[sp - 1]->type != PV_STR) {
            info_problem(c, "Tj without a string", off);
            return 0;
        }
        text_show(c, st[sp - 1]->u.str.s, st[sp - 1]->u.str.len);
        return 1;
    }
    if (!strcmp(op, "'")) {
        c->tlm = mat_mul(mat_translate(0, -g->leading), c->tlm);
        c->tm = c->tlm;
        if (sp < 1 || !st[sp - 1] || st[sp - 1]->type != PV_STR) return 0;
        c->text_pending_nl = 1;
        text_show(c, st[sp - 1]->u.str.s, st[sp - 1]->u.str.len);
        return 1;
    }
    if (!strcmp(op, "\"")) {
        if (sp < 3 || !st[sp - 3] || !st[sp - 2] ||
            !val_num(st[sp - 3], &v[0]) || !val_num(st[sp - 2], &v[1]) ||
            !st[sp - 1] || st[sp - 1]->type != PV_STR) {
            info_problem(c, "\" with bad operands", off);
            return 0;
        }
        g->word_space = v[0];
        g->char_space = v[1];
        c->tlm = mat_mul(mat_translate(0, -g->leading), c->tlm);
        c->tm = c->tlm;
        c->text_pending_nl = 1;
        text_show(c, st[sp - 1]->u.str.s, st[sp - 1]->u.str.len);
        return 1;
    }
    if (!strcmp(op, "TJ")) {
        PdfVal *a = sp >= 1 ? st[sp - 1] : NULL;
        if (!a || a->type != PV_ARRAY) { info_problem(c, "TJ without an array", off); return 0; }
        for (int i = 0; i < a->u.arr.count; i++) {
            PdfVal *e = a->u.arr.items[i];
            if (e->type == PV_STR) {
                text_show(c, e->u.str.s, e->u.str.len);
            } else if (e->type == PV_INT || e->type == PV_REAL) {
                double num = e->type == PV_INT ? (double)e->u.i : e->u.r;
                double tx = -(num / 1000.0) * c->font_size * c->gs.hscale;
                c->tm.e += tx * c->tm.a;
                c->tm.f += tx * c->tm.b;
            }
        }
        return 1;
    }

    if (!strcmp(op, "m")) {
        if (!take(st, sp, 2, v)) return 0;
        path_move(c, v[0], v[1]);
        return 1;
    }
    if (!strcmp(op, "l")) {
        if (!take(st, sp, 2, v)) return 0;
        path_line(c, v[0], v[1]);
        return 1;
    }
    if (!strcmp(op, "c")) {
        if (!take(st, sp, 6, v)) return 0;
        path_curve(c, v[0], v[1], v[2], v[3], v[4], v[5]);
        return 1;
    }
    if (!strcmp(op, "v") || !strcmp(op, "y")) {
        if (!take(st, sp, 4, v)) return 0;
        if (op[0] == 'v') path_curve(c, c->path.cx, c->path.cy, v[0], v[1], v[2], v[3]);
        else              path_curve(c, v[0], v[1], v[2], v[3], v[2], v[3]);
        return 1;
    }
    if (!strcmp(op, "h")) { path_close(c); return 1; }
    if (!strcmp(op, "re")) {
        if (!take(st, sp, 4, v)) return 0;
        path_move(c, v[0], v[1]);
        path_line(c, v[0] + v[2], v[1]);
        path_line(c, v[0] + v[2], v[1] + v[3]);
        path_line(c, v[0], v[1] + v[3]);
        path_close(c);
        return 1;
    }
    if (!strcmp(op, "f") || !strcmp(op, "F")) { paint_path(c, 1, 0, 0); return 1; }
    if (!strcmp(op, "f*")) { paint_path(c, 1, 0, 1); return 1; }
    if (!strcmp(op, "S")) { paint_path(c, 0, 1, 0); return 1; }
    if (!strcmp(op, "s")) { path_close(c); paint_path(c, 0, 1, 0); return 1; }
    if (!strcmp(op, "B")) { paint_path(c, 1, 1, 0); return 1; }
    if (!strcmp(op, "B*")) { paint_path(c, 1, 1, 1); return 1; }
    if (!strcmp(op, "b")) { path_close(c); paint_path(c, 1, 1, 0); return 1; }
    if (!strcmp(op, "b*")) { path_close(c); paint_path(c, 1, 1, 1); return 1; }
    if (!strcmp(op, "n")) { apply_pending_clip(c); path_reset(c); return 1; }
    if (!strcmp(op, "W")) { c->path.pending_clip = 1; c->path.pending_evenodd = 0; return 1; }
    if (!strcmp(op, "W*")) { c->path.pending_clip = 1; c->path.pending_evenodd = 1; return 1; }

    if (!strcmp(op, "Do")) {
        const char *name = take_name(st, sp);
        if (!name) { info_problem(c, "Do without a name", off); return 0; }
        draw_xobject(c, name);
        return 1;
    }
    if (!strcmp(op, "sh")) {
        info_problem(c, "shading (/sh) is not implemented", off);
        return 0;
    }

    /* Structural operators: marked content and compatibility sections.
     * These carry no pixels and are correctly ignored. */
    if (!strcmp(op, "BMC") || !strcmp(op, "BDC") || !strcmp(op, "EMC") ||
        !strcmp(op, "MP") || !strcmp(op, "DP") || !strcmp(op, "BX") ||
        !strcmp(op, "EX")) return 0;

    char what[80];
    snprintf(what, sizeof(what), "unsupported operator '%s'", op);
    info_problem(c, what, off);
    return 0;
}

static int run_content(Ctx *c, const u8 *data, size_t len, int form_depth) {
    (void)form_depth;
    CLex L;
    memset(&L, 0, sizeof(L));
    L.c = c;
    L.b = data;
    L.len = len;

    PdfVal *stack[PDF_MAX_OPERANDS];
    int sp = 0;
    char op[32];
    PdfVal *v = NULL;

    for (;;) {
        int r = cl_next(&L, &v, op, sizeof(op));
        long off = c->base_off + (long)L.op_pos;
        if (r == 0) break;
        if (r < 0) return 0;
        if (r == 1) {
            if (sp >= PDF_MAX_OPERANDS) {
                pdf_set_err(c->d, off, "more than %d operands before an operator", PDF_MAX_OPERANDS);
                return 0;
            }
            stack[sp++] = v;
            continue;
        }
        if (!strcmp(op, "BI")) {
            int ir = skip_inline_image(&L, c);
            if (ir < 0) return 0;
            if (ir == 0) break;
            sp = 0;
            continue;
        }
        exec_op(c, op, stack, sp, off);
        sp = 0;
    }
    return 1;
}
/* ------------------------------------------------------------
 *  One page's content stream: /Contents may be a single stream or an
 *  array of them, and they are concatenated with a newline between,
 *  which is what the specification says to do.
 * ------------------------------------------------------------ */
static u8 *page_content(PdfDoc *d, PdfVal *contents, size_t *outlen,
                        long *first_off, char *why, size_t whysz) {
    *outlen = 0;
    *first_off = 0;
    Buf out;
    buf_init(&out);

    contents = obj_resolve(d, contents, 0);
    if (!contents) {
        snprintf(why, whysz, "page has no /Contents");
        return NULL;
    }

    int n = (contents->type == PV_ARRAY) ? contents->u.arr.count :
            (contents->type == PV_STREAM) ? 1 : 0;
    if (n == 0) {
        snprintf(why, whysz, "page /Contents is neither a stream nor an array");
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        PdfVal *st = (contents->type == PV_ARRAY)
                     ? obj_resolve(d, contents->u.arr.items[i], 0) : contents;
        if (!st || st->type != PV_STREAM) {
            snprintf(why, whysz, "content stream %d is not a stream", i);
            buf_free(&out);
            return NULL;
        }
        if (i == 0) *first_off = st->u.stream.raw_off;
        const u8 *data = NULL;
        size_t len = 0;
        if (!stream_data(d, st, &data, &len)) {
            snprintf(why, whysz, "content stream at byte %ld: %s",
                     st->u.stream.raw_off, st->u.stream.why);
            buf_free(&out);
            return NULL;
        }
        if (out.len + len > PDF_MAX_CONTENT) {
            snprintf(why, whysz, "page content is larger than %lu bytes",
                     (unsigned long)PDF_MAX_CONTENT);
            buf_free(&out);
            return NULL;
        }
        buf_put(&out, data, len);
        buf_putc(&out, '\n');
    }
    if (out.oom) {
        snprintf(why, whysz, "out of memory concatenating the page content");
        buf_free(&out);
        return NULL;
    }
    *outlen = out.len;
    return out.p;
}

/* ------------------------------------------------------------
 *  Context setup / teardown
 * ------------------------------------------------------------ */
static int ctx_alloc(Ctx *c) {
    c->path.x = calloc(PDF_MAX_PATH, sizeof(double));
    c->path.y = calloc(PDF_MAX_PATH, sizeof(double));
    c->path.mv = calloc(PDF_MAX_PATH, sizeof(u8));
    c->cross = calloc(((size_t)PDF_MAX_PATH + 1) * 4, sizeof(double));
    c->cross_dir = calloc((size_t)PDF_MAX_PATH + 1, sizeof(int));
    c->xs = calloc((size_t)PDF_MAX_PATH + 1, sizeof(double));
    c->xdir = calloc((size_t)PDF_MAX_PATH + 1, sizeof(int));
    if (!c->path.x || !c->path.y || !c->path.mv || !c->cross ||
        !c->cross_dir || !c->xs || !c->xdir) return 0;
    return 1;
}

static void ctx_free(Ctx *c) {
    free(c->path.x);
    free(c->path.y);
    free(c->path.mv);
    free(c->cross);
    free(c->cross_dir);
    free(c->xs);
    free(c->xdir);
    free(c->cov);
}

static void ctx_defaults(Ctx *c, PdfDoc *d, PdfVal *resources, PdfRenderInfo *info,
                         int draw) {
    memset(c, 0, sizeof(*c));
    c->d = d;
    c->resources = resources;
    c->info = info;
    c->draw = draw;
    c->gs.ctm = mat_identity();
    c->gs.fill[0] = 0; c->gs.fill_n = 1;          /* black */
    c->gs.stroke[0] = 0; c->gs.stroke_n = 1;
    c->gs.line_width = 1.0;
    c->gs.hscale = 1.0;
    c->gs.fill_alpha = 1.0;
    c->gs.stroke_alpha = 1.0;
    c->tm = c->tlm = mat_identity();
    c->clip_w = 1;
    c->clip_h = 1;
    c->gs.clip_x1 = 1;
    c->gs.clip_y1 = 1;
}

/* ------------------------------------------------------------
 *  pdf_render_page
 * ------------------------------------------------------------ */
PDF_API int pdf_render_page(PdfDoc *d, int page, Surface *s, int dx, int dy,
                            double zoom, PdfRenderInfo *info) {
    if (info) memset(info, 0, sizeof(*info));
    if (!d) return -1;
    d->err[0] = 0;
    d->err_off = 0;

    if (!s || !s->px) {
        pdf_set_err(d, 0, "render: no surface");
        return -1;
    }
    if (page < 0 || page >= d->npages) {
        pdf_set_err(d, 0, "render: page %d does not exist (the document has %d)",
                    page + 1, d->npages);
        return -1;
    }
    if (!(zoom > 0.01) || zoom > 64.0) {
        pdf_set_err(d, 0, "render: zoom %.3f is outside 0.01..64", zoom);
        return -1;
    }

    PdfPageInfo pi;
    if (pdf_page_info(d, page, &pi) < 0) return -1;
    const PdfPage *pg = &d->pages[page];
    double pw = pi.width * zoom;
    double ph = pi.height * zoom;
    if (info) { info->page_w = pw; info->page_h = ph; }

    /* The page rectangle, clipped to the surface and to its own clip box. */
    int px0 = dx, py0 = dy;
    int px1 = dx + (int)ceil(pw), py1 = dy + (int)ceil(ph);
    int cx0 = px0 < 0 ? 0 : px0;
    int cy0 = py0 < 0 ? 0 : py0;
    int cx1 = px1 > s->w ? s->w : px1;
    int cy1 = py1 > s->h ? s->h : py1;
    if (s->cx1 > s->cx0) {
        if (cx0 < s->cx0) cx0 = s->cx0;
        if (cy0 < s->cy0) cy0 = s->cy0;
        if (cx1 > s->cx1) cx1 = s->cx1;
        if (cy1 > s->cy1) cy1 = s->cy1;
    }
    if (cx1 <= cx0 || cy1 <= cy0) return 0;       /* scrolled off: nothing to do */

    /* A view whose bounds ARE the clip, so every path and glyph is clipped
     * by the rasteriser itself and cannot touch a neighbouring window. */
    Surface view;
    view.px = s->px + (size_t)cy0 * (size_t)s->stride + (size_t)cx0;
    view.w = cx1 - cx0;
    view.h = cy1 - cy0;
    view.stride = s->stride;
    view.cx0 = view.cy0 = view.cx1 = view.cy1 = 0;
    surf_fill(&view, 0, 0, view.w, view.h, 0xFFFFFFFFu);   /* paper */

    double ox = (double)(dx - cx0);
    double oy = (double)(dy - cy0);
    double bx0 = pg->box[0], by0 = pg->box[1], bx1 = pg->box[2], by1 = pg->box[3];
    Mat base;
    switch (pg->rotate) {
        case 90:
            base.a = 0;  base.b = zoom; base.c = zoom;  base.d = 0;
            base.e = ox - by0 * zoom;  base.f = oy - bx0 * zoom;
            break;
        case 180:
            base.a = -zoom; base.b = 0; base.c = 0; base.d = zoom;
            base.e = ox + bx1 * zoom;  base.f = oy - by0 * zoom;
            break;
        case 270:
            base.a = 0;  base.b = -zoom; base.c = -zoom; base.d = 0;
            base.e = ox + by1 * zoom;  base.f = oy + bx1 * zoom;
            break;
        default:
            base.a = zoom; base.b = 0; base.c = 0; base.d = -zoom;
            base.e = ox - bx0 * zoom;  base.f = oy + by1 * zoom;
            break;
    }

    size_t clen = 0;
    long coff = 0;
    char why[220] = "";
    u8 *content = page_content(d, dict_get(pg->dict, "Contents"), &clen, &coff,
                               why, sizeof(why));
    if (!content) {
        pdf_set_err(d, coff, "%s", why);
        return -1;
    }

    ar_free(&d->scratch);
    ar_init(&d->scratch);

    Ctx c;
    ctx_defaults(&c, d, pg->resources, info, 1);
    c.s = &view;
    c.base = base;
    c.zoom = zoom;
    c.clip_w = view.w;
    c.clip_h = view.h;
    c.base_off = coff;
    c.gs.ctm = base;
    ctx_clip_reset(&c);

    int ok = 0;
    if (ctx_alloc(&c)) {
        c.cov = calloc((size_t)(view.w > 0 ? view.w : 1), sizeof(float));
        c.cov_w = view.w;
        if (c.cov) ok = run_content(&c, content, clen, 0);
    }
    if (!ok && !d->err[0])
        pdf_set_err(d, coff, "page %d could not be rendered", page + 1);
    ctx_free(&c);
    free(content);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------
 *  pdf_page_text: the same interpreter, with the pixels switched off.
 *  The only differences are the text sink and the newline rules: a
 *  vertical Td/TD/Tm, T*, ' or " starts a new line, and horizontal
 *  moves inside a line become a space.
 * ------------------------------------------------------------ */
PDF_API int pdf_page_text(PdfDoc *d, int page, char *out, size_t outsz, int *truncated) {
    if (truncated) *truncated = 0;
    if (!d || !out || outsz == 0) return -1;
    out[0] = 0;
    if (page < 0 || page >= d->npages) {
        pdf_set_err(d, 0, "text: page %d does not exist", page + 1);
        return -1;
    }
    const PdfPage *pg = &d->pages[page];

    size_t clen = 0;
    long coff = 0;
    char why[220] = "";
    u8 *content = page_content(d, dict_get(pg->dict, "Contents"), &clen, &coff,
                               why, sizeof(why));
    if (!content) {
        pdf_set_err(d, coff, "%s", why);
        return -1;
    }

    ar_free(&d->scratch);
    ar_init(&d->scratch);

    Ctx c;
    ctx_defaults(&c, d, pg->resources, NULL, 0);
    c.s = NULL;
    c.base_off = coff;
    c.clip_w = 1 << 20;                            /* no clipping in text mode */
    c.clip_h = 1 << 20;
    c.gs.clip_x1 = c.clip_w;
    c.gs.clip_y1 = c.clip_h;
    c.text = out;
    c.textcap = outsz;
    c.textlen = 0;
    out[0] = 0;

    int ok = ctx_alloc(&c) ? run_content(&c, content, clen, 0) : 0;
    int n = (int)c.textlen;
    if (truncated) *truncated = c.text_truncated;
    if (!ok && !d->err[0])
        pdf_set_err(d, coff, "page %d text could not be read", page + 1);
    ctx_free(&c);
    free(content);
    return ok ? n : -1;
}
