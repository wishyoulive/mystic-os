/* ============================================================
 *  Mystic OS - apps (external) / paint
 *
 *  A paint program that runs as its own process: it connects to the
 *  window server, is handed a shared pixel buffer, and keeps a canvas
 *  of its own between frames.
 *
 *  What is real here, and where it comes from:
 *
 *    tools       pencil, line, rectangle, filled rectangle, ellipse,
 *                filled ellipse, flood fill, eyedropper. Every shape is
 *                rasterised twice - once into the shared buffer while
 *                you drag (the live preview) and once into the canvas
 *                when you let go - by the same function, so what you
 *                see is what is committed.
 *    fill        a span (scanline) seed fill with an explicit stack,
 *                not recursion. Four-connected, exact colour match, and
 *                it reports how many pixels it changed.
 *    undo/redo   a bounded ring of whole canvas bitmaps kept in malloc'd
 *                memory. HIST_STATES snapshots => HIST_STATES-1 undo
 *                steps (13), and every byte is counted and shown on
 *                screen: the ring can never exceed HIST_STATES *
 *                CANVAS_BYTES. If malloc fails, history is disabled and
 *                the app says so instead of dying.
 *    save        a real PNG in /root/Pictures (or /tmp/Pictures when the
 *                jail has hidden /root - the place actually used is
 *                printed). The writer emits a valid zlib stream made of
 *                *stored* deflate blocks: legal, simple and exact, no
 *                compression library needed. IHDR/IDAT/IEND CRCs are
 *                computed over the real bytes.
 *    open        a real decoder: it verifies the signature and every
 *                chunk CRC, inflates zlib (stored *and* fixed *and*
 *                dynamic Huffman), unfilters all five PNG row filters,
 *                and converts 8-bit grey / RGB / palette / grey+alpha /
 *                RGBA. Anything it cannot decode is refused with the
 *                reason, never drawn as garbage.
 *    colours     a twelve-ink palette plus a 24-bit RGB mixer with three
 *                draggable sliders; the hex code is shown.
 *    brush       width 1..16, a disc stamp, shared by every shape.
 *    clear       asks first, and the clear is undoable.
 *
 *  Nothing is invented: the only numbers on screen are the pen position,
 *  the mixer channels you set, the brush size, the undo bytes, and the
 *  size of files that stat() reported.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ sizes */

#define WIN_W 640
#define WIN_H 520

#define CANVAS_W 400
#define CANVAS_H 320
#define CANVAS_PX  (CANVAS_W * CANVAS_H)
#define CANVAS_BYTES ((size_t)CANVAS_PX * 4u)

#define HIST_STATES 14          /* 13 undo steps + the live state */
#define MAX_BRUSH   16

#define CX0 100                 /* canvas origin inside the window */
#define CY0 8
#define RAIL_X 8
#define RAIL_W 84
#define PANEL_X 508
#define PANEL_W 124

/* The OS build writes to the desktop's Pictures folder. The host test
 * overrides these on the compiler command line so it can exercise the
 * same save/open code without touching the real /root. */
#ifndef PIC_DIR
#  define PIC_DIR "/root/Pictures"
#endif
#ifndef PIC_DIR_FALLBACK
#  define PIC_DIR_FALLBACK "/tmp/Pictures"
#endif

/* ------------------------------------------------------------ colours */

static const u32 C_BG     = RGB(0x12, 0x16, 0x1F);
static const u32 C_PANEL  = RGB(0x1A, 0x21, 0x2D);
static const u32 C_LINE   = RGB(0x2A, 0x34, 0x45);
static const u32 C_TEXT   = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM    = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_WARN   = RGB(0xE0, 0x6C, 0x5C);
static const u32 CANVAS_BG = RGB(0x0E, 0x14, 0x1C);

/* The twelve inks. Light on purpose: the sheet is dark. */
#define PAL_N 12
static const u32 PALETTE[PAL_N] = {
    RGB(0xE8, 0xEC, 0xF6),   /* bone    */
    RGB(0xFF, 0x6B, 0x5B),   /* rust    */
    RGB(0xF5, 0xC5, 0x42),   /* amber   */
    RGB(0x7B, 0xD8, 0x8F),   /* green   */
    RGB(0x4F, 0xD1, 0xC5),   /* teal    */
    RGB(0x6B, 0xC5, 0xE8),   /* sky     */
    RGB(0x7A, 0xA2, 0xF7),   /* blue    */
    RGB(0xB9, 0x8B, 0xF7),   /* violet  */
    RGB(0xF7, 0x8F, 0xB3),   /* pink    */
    RGB(0xFF, 0xA2, 0x4F),   /* orange  */
    RGB(0x94, 0xA3, 0xB8),   /* slate   */
    RGB(0x2B, 0x33, 0x42),   /* graphite*/
};
static const char *PAL_NAME[PAL_N] = {
    "bone", "rust", "amber", "green", "teal", "sky",
    "blue", "violet", "pink", "orange", "slate", "graphite"
};

/* ------------------------------------------------------------ tools */

enum { T_PENCIL, T_LINE, T_RECT, T_RECTF, T_ELLIPSE, T_ELLIPSEF, T_FILL, T_PICK, T_N };
static const char *TOOL_NAME[T_N] = {
    "Pencil", "Line", "Rect", "Rect F", "Ellipse", "Ellip F", "Fill", "Pick"
};

/* ============================================================
 *  PNG - a writer that emits stored deflate blocks and a reader
 *  that really inflates (stored, fixed and dynamic Huffman).
 * ============================================================ */

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 crc_tab[256];
static int  crc_ready;

static void crc_build(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[n] = c;
    }
    crc_ready = 1;
}

/* running CRC: start with 0xFFFFFFFF, finish by XOR 0xFFFFFFFF */
static u32 crc32_upd(u32 c, const u8 *p, size_t n) {
    if (!crc_ready) crc_build();
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

static u32 adler32_of(const u8 *p, size_t n) {
    u32 a = 1, b = 0;
    size_t i = 0;
    while (i < n) {
        size_t chunk = n - i;
        if (chunk > 5552) chunk = 5552;
        for (size_t k = 0; k < chunk; k++) {
            a += p[i + k];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        i += chunk;
    }
    return (b << 16) | a;
}

/* ---- inflate ---------------------------------------------------- */

typedef struct {
    const u8 *in;
    size_t len, pos;
    u32 bitbuf;
    int  bitcnt;
    u8  *out;
    size_t cap, n;
    int  err;
} ZIn;

typedef struct {
    short count[16];
    unsigned short sym[288];
} Huf;

static int zbits(ZIn *z, int n) {
    if (n <= 0) return 0;
    while (z->bitcnt < n) {
        if (z->pos >= z->len) { z->err = 1; return 0; }
        z->bitbuf |= (u32)z->in[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & (((u32)1 << n) - 1u));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

static void zput(ZIn *z, u8 b) {
    if (z->n >= z->cap) { z->err = 1; return; }
    z->out[z->n++] = b;
}

/* canonical Huffman table from a list of code lengths.
 * Returns 1 for a complete code, 0 for a valid but incomplete one
 * (all-zero distance tables are legal), -1 if oversubscribed. */
static int huf_make(Huf *h, const u8 *lens, int n) {
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        h->count[lens[i]]++;
    }
    if (h->count[0] == n) return 0;             /* no symbols at all */
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;                /* over-subscribed */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (unsigned short)i;
    return left == 0 ? 1 : 0;
}

static int huf_dec(ZIn *z, const Huf *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = zbits(z, 1);
        if (z->err) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first)
            return (int)h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->err = 1;
    return -1;
}

static const unsigned short LBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char LEXT[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short DBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const unsigned char DEXT[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static Huf fixed_lit, fixed_dist;
static int fixed_ready;

static void fixed_build(void) {
    u8 l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    huf_make(&fixed_lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    huf_make(&fixed_dist, l, 30);
    fixed_ready = 1;
}

/* Decode one raw deflate stream. Returns 0 on success. The output is
 * exactly as long as the stream said; the caller checks that length. */
static int inflate_raw(ZIn *z) {
    int final = 0;
    while (!final && !z->err) {
        final = zbits(z, 1);
        int type = zbits(z, 2);
        if (z->err) break;

        if (type == 0) {                        /* stored */
            z->bitbuf = 0;
            z->bitcnt = 0;
            if (z->pos + 4 > z->len) { z->err = 1; break; }
            unsigned len = (unsigned)z->in[z->pos] | ((unsigned)z->in[z->pos + 1] << 8);
            unsigned nlen = (unsigned)z->in[z->pos + 2] | ((unsigned)z->in[z->pos + 3] << 8);
            z->pos += 4;
            if ((len ^ 0xFFFFu) != nlen) { z->err = 1; break; }
            if (z->pos + len > z->len) { z->err = 1; break; }
            for (unsigned i = 0; i < len; i++) zput(z, z->in[z->pos + i]);
            z->pos += len;
            continue;
        }

        Huf *lit = NULL, *dist = NULL;
        if (type == 1) {
            if (!fixed_ready) fixed_build();
            lit = &fixed_lit;
            dist = &fixed_dist;
        } else if (type == 2) {
            int hlit = zbits(z, 5) + 257;
            int hdist = zbits(z, 5) + 1;
            int hclen = zbits(z, 4) + 4;
            if (z->err) break;
            if (hlit > 286 || hdist > 30) { z->err = 1; break; }

            static const u8 ord[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            u8 cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) cl[ord[i]] = (u8)zbits(z, 3);
            if (z->err) break;
            Huf ch;
            if (huf_make(&ch, cl, 19) < 0) { z->err = 1; break; }

            u8 lens[320];
            memset(lens, 0, sizeof(lens));
            int total = hlit + hdist;
            int n = 0;
            while (n < total) {
                int sym = huf_dec(z, &ch);
                if (z->err || sym < 0) { z->err = 1; break; }
                if (sym < 16) {
                    lens[n++] = (u8)sym;
                } else if (sym == 16) {
                    if (n == 0) { z->err = 1; break; }
                    int rep = 3 + zbits(z, 2);
                    u8 prev = lens[n - 1];
                    while (rep-- > 0) {
                        if (n >= total) { z->err = 1; break; }
                        lens[n++] = prev;
                    }
                } else if (sym == 17) {
                    int rep = 3 + zbits(z, 3);
                    while (rep-- > 0) {
                        if (n >= total) { z->err = 1; break; }
                        lens[n++] = 0;
                    }
                } else {
                    int rep = 11 + zbits(z, 7);
                    while (rep-- > 0) {
                        if (n >= total) { z->err = 1; break; }
                        lens[n++] = 0;
                    }
                }
                if (z->err) break;
            }
            if (z->err) break;
            static Huf dl, dd;
            if (huf_make(&dl, lens, hlit) < 0) { z->err = 1; break; }
            if (huf_make(&dd, lens + hlit, hdist) < 0) { z->err = 1; break; }
            lit = &dl;
            dist = &dd;
        } else {
            z->err = 1;
            break;
        }

        for (;;) {
            int sym = huf_dec(z, lit);
            if (z->err || sym < 0) break;
            if (sym < 256) {
                zput(z, (u8)sym);
                continue;
            }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) { z->err = 1; break; }
            int len = LBASE[sym] + zbits(z, LEXT[sym]);
            int dsym = huf_dec(z, dist);
            if (z->err || dsym < 0) { z->err = 1; break; }
            if (dsym >= 30) { z->err = 1; break; }
            size_t d = (size_t)DBASE[dsym] + (size_t)zbits(z, DEXT[dsym]);
            if (z->err) break;
            if (d > z->n) { z->err = 1; break; }
            for (int i = 0; i < len; i++)
                zput(z, z->out[z->n - d]);      /* overlapping copy is legal */
            if (z->err) break;
        }
    }
    return z->err ? -1 : 0;
}

/* ---- chunk writer ------------------------------------------------ */

static int wr_chunk(FILE *f, const char *type, const u8 *data, size_t len) {
    u8 head[8];
    put_be32(head, (u32)len);
    memcpy(head + 4, type, 4);
    if (fwrite(head, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    u32 c = crc32_upd(0xFFFFFFFFu, (const u8 *)type, 4);
    c = crc32_upd(c, data, len) ^ 0xFFFFFFFFu;
    u8 tail[4];
    put_be32(tail, c);
    if (fwrite(tail, 1, 4, f) != 4) return -1;
    return 0;
}

/* Write the canvas as an 8-bit RGBA PNG. Rows use filter 0 and the
 * zlib stream is a sequence of stored deflate blocks - both are fully
 * legal and let this stay dependency-free and exact.
 * Returns the number of bytes in the finished file, or -1. */
static long png_write_file(const char *path, const u32 *px, int w, int h,
                           char *err, size_t esz) {
    if (w <= 0 || h <= 0) { snprintf(err, esz, "empty image"); return -1; }

    size_t stride = (size_t)w * 4u;
    size_t rawlen = (stride + 1u) * (size_t)h;
    u8 *raw = malloc(rawlen);
    if (!raw) { snprintf(err, esz, "out of memory for %lu filtered bytes", (unsigned long)rawlen); return -1; }

    u8 *q = raw;
    for (int y = 0; y < h; y++) {
        *q++ = 0;                                /* filter: none */
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * (size_t)w + (size_t)x];
            *q++ = GET_R(c);
            *q++ = GET_G(c);
            *q++ = GET_B(c);
            *q++ = (u8)(GET_A(c) ? GET_A(c) : 255);
        }
    }

    size_t nblocks = (rawlen + 65534u) / 65535u;
    size_t zcap = rawlen + nblocks * 5u + 16u;
    u8 *z = malloc(zcap);
    if (!z) { free(raw); snprintf(err, esz, "out of memory for the zlib stream"); return -1; }

    size_t zl = 0;
    z[zl++] = 0x78;                              /* CM=8, CINFO=7 */
    z[zl++] = 0x01;                              /* FCHECK: 0x7801 % 31 == 0 */
    size_t off = 0;
    while (off < rawlen) {
        size_t n = rawlen - off;
        if (n > 65535u) n = 65535u;
        int last = (off + n == rawlen);
        z[zl++] = (u8)(last ? 1 : 0);            /* BFINAL, BTYPE=00 (stored) */
        z[zl++] = (u8)(n & 0xFFu);
        z[zl++] = (u8)((n >> 8) & 0xFFu);
        z[zl++] = (u8)(~n & 0xFFu);
        z[zl++] = (u8)((~n >> 8) & 0xFFu);
        memcpy(z + zl, raw + off, n);
        zl += n;
        off += n;
    }
    u32 ad = adler32_of(raw, rawlen);
    put_be32(z + zl, ad);
    zl += 4;

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, esz, "%s: %s", path, strerror(errno));
        free(raw); free(z);
        return -1;
    }

    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int ok = 1;
    if (fwrite(sig, 1, 8, f) != 8) ok = 0;

    if (ok) {
        u8 ihdr[13];
        put_be32(ihdr, (u32)w);
        put_be32(ihdr + 4, (u32)h);
        ihdr[8] = 8;         /* bit depth */
        ihdr[9] = 6;         /* colour type: truecolour with alpha */
        ihdr[10] = 0;        /* compression: deflate */
        ihdr[11] = 0;        /* filter method 0 */
        ihdr[12] = 0;        /* no interlace */
        if (wr_chunk(f, "IHDR", ihdr, sizeof(ihdr)) < 0) ok = 0;
    }
    if (ok && wr_chunk(f, "IDAT", z, zl) < 0) ok = 0;
    if (ok && wr_chunk(f, "IEND", NULL, 0) < 0) ok = 0;
    if (ok && fflush(f) != 0) ok = 0;
    long wrote = ok ? (long)ftell(f) : -1;
    if (fclose(f) != 0) ok = 0;
    free(raw);
    free(z);

    if (!ok) {
        snprintf(err, esz, "%s: write failed: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, esz, "%s: rename failed: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return wrote;
}

/* ---- PNG reader -------------------------------------------------- */

typedef struct {
    u8 *p;
    size_t len, cap;
} Bytes;

static int bytes_add(Bytes *b, const u8 *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 8192;
        while (cap < b->len + n) cap *= 2;
        u8 *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 0;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undo the per-row filters in place. bpp is bytes per pixel. */
static int unfilter(u8 *raw, int w, int h, int bpp, char *err, size_t esz) {
    size_t stride = (size_t)w * (size_t)bpp;
    size_t rs = stride + 1u;
    for (int y = 0; y < h; y++) {
        u8 ft = raw[(size_t)y * rs];
        u8 *row = raw + (size_t)y * rs + 1u;
        u8 *prev = y > 0 ? raw + (size_t)(y - 1) * rs + 1u : NULL;
        switch (ft) {
        case 0:
            break;
        case 1:
            for (size_t x = (size_t)bpp; x < stride; x++)
                row[x] = (u8)(row[x] + row[x - (size_t)bpp]);
            break;
        case 2:
            if (prev)
                for (size_t x = 0; x < stride; x++)
                    row[x] = (u8)(row[x] + prev[x]);
            break;
        case 3:
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0;
                int b = prev ? prev[x] : 0;
                row[x] = (u8)(row[x] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
                row[x] = (u8)(row[x] + paeth(a, b, c));
            }
            break;
        default:
            snprintf(err, esz, "row %d uses unknown filter %u", y, (unsigned)ft);
            return -1;
        }
    }
    return 0;
}

static u32 over(u32 bg, int r, int g, int b, int a) {
    if (a >= 255) return RGB(r, g, b);
    if (a <= 0) return bg;
    int ia = 255 - a;
    int rr = (r * a + GET_R(bg) * ia + 127) / 255;
    int gg = (g * a + GET_G(bg) * ia + 127) / 255;
    int bb = (b * a + GET_B(bg) * ia + 127) / 255;
    return RGB(rr, gg, bb);
}

/* Decode a PNG file into dst (dw x dh), centred and scaled down if the
 * source is larger than the canvas. sw/sh report the source size. */
static int png_read_file(const char *path, u32 *dst, int dw, int dh,
                         int *sw, int *sh, char *err, size_t esz) {
    *sw = *sh = 0;

    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, esz, "%s: %s", path, strerror(errno)); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { snprintf(err, esz, "%s: seek failed", path); fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64L * 1024L * 1024L) {
        snprintf(err, esz, "%s: file size %ld is not a picture I will read", path, sz);
        fclose(f);
        return -1;
    }
    rewind(f);
    u8 *buf = malloc((size_t)sz);
    if (!buf) { snprintf(err, esz, "out of memory reading %ld bytes", sz); fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, esz, "%s: short read", path);
        free(buf); fclose(f);
        return -1;
    }
    fclose(f);

    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if ((size_t)sz < 8 || memcmp(buf, sig, 8) != 0) {
        snprintf(err, esz, "%s: not a PNG (bad signature)", path);
        free(buf);
        return -1;
    }

    int W = 0, H = 0, depth = 0, colour = 0, channels = 0;
    u8 pal[256 * 3];
    int pal_n = 0;
    int have_ihdr = 0, have_iend = 0;
    Bytes idat = { NULL, 0, 0 };
    int rc = -1;

    size_t p = 8;
    while (p + 12 <= (size_t)sz) {
        u32 clen = be32(buf + p);
        if (clen > 0x7FFFFFFFu || p + 12u + clen > (size_t)sz) {
            snprintf(err, esz, "%s: truncated chunk at byte %lu", path, (unsigned long)p);
            goto done;
        }
        const u8 *type = buf + p + 4;
        const u8 *data = buf + p + 8;
        u32 want = be32(buf + p + 8 + clen);
        u32 got = crc32_upd(0xFFFFFFFFu, type, 4u + clen) ^ 0xFFFFFFFFu;
        if (want != got) {
            snprintf(err, esz, "%s: CRC mismatch in the %.4s chunk", path, (const char *)type);
            goto done;
        }
        int critical = type[0] >= 'A' && type[0] <= 'Z';
        if (!memcmp(type, "IHDR", 4)) {
            if (clen != 13) { snprintf(err, esz, "IHDR is %u bytes, not 13", clen); goto done; }
            W = (int)be32(data);
            H = (int)be32(data + 4);
            depth = data[8];
            colour = data[9];
            if (data[10] != 0) { snprintf(err, esz, "compression method %u is not deflate", data[10]); goto done; }
            if (data[11] != 0) { snprintf(err, esz, "filter method %u is unknown", data[11]); goto done; }
            if (data[12] != 0) { snprintf(err, esz, "this is an interlaced PNG; I only read non-interlaced ones"); goto done; }
            if (W <= 0 || H <= 0 || W > 20000 || H > 20000) {
                snprintf(err, esz, "PNG says %dx%d, which is out of range", W, H);
                goto done;
            }
            if (depth != 8) {
                snprintf(err, esz, "bit depth %d is not 8; I read 8-bit PNGs only", depth);
                goto done;
            }
            switch (colour) {
            case 0: channels = 1; break;
            case 2: channels = 3; break;
            case 3: channels = 1; break;
            case 4: channels = 2; break;
            case 6: channels = 4; break;
            default:
                snprintf(err, esz, "colour type %d is not one I know", colour);
                goto done;
            }
            have_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (clen % 3u || clen > sizeof(pal)) {
                snprintf(err, esz, "PLTE is %u bytes", clen);
                goto done;
            }
            memcpy(pal, data, clen);
            pal_n = (int)(clen / 3u);
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!have_ihdr) { snprintf(err, esz, "IDAT before IHDR"); goto done; }
            if (bytes_add(&idat, data, clen) < 0) { snprintf(err, esz, "out of memory collecting IDAT"); goto done; }
        } else if (!memcmp(type, "IEND", 4)) {
            have_iend = 1;
        } else if (critical) {
            snprintf(err, esz, "critical chunk %.4s is not supported", (const char *)type);
            goto done;
        }
        p += 12u + clen;
    }

    if (!have_ihdr) { snprintf(err, esz, "%s: no IHDR", path); goto done; }
    if (!have_iend) { snprintf(err, esz, "%s: no IEND (file truncated?)", path); goto done; }
    if (!idat.len) { snprintf(err, esz, "%s: no IDAT", path); goto done; }
    if (colour == 3 && pal_n == 0) { snprintf(err, esz, "%s: palette image with no PLTE", path); goto done; }
    if ((size_t)W * (size_t)H > 16000000u) { snprintf(err, esz, "%s: %dx%d is too many pixels", path, W, H); goto done; }

    /* zlib header */
    if (idat.len < 6 || idat.p[0] != 0x78) {
        snprintf(err, esz, "%s: not a zlib stream (CMF 0x%02x)", path, idat.p[0]);
        goto done;
    }
    if ((((unsigned)idat.p[0] << 8) | idat.p[1]) % 31u != 0) {
        snprintf(err, esz, "%s: bad zlib header check", path);
        goto done;
    }
    if (idat.p[1] & 0x20u) { snprintf(err, esz, "%s: preset dictionary is not supported", path); goto done; }

    size_t stride = (size_t)W * (size_t)channels;
    size_t rawlen = (stride + 1u) * (size_t)H;
    u8 *raw = malloc(rawlen);
    if (!raw) { snprintf(err, esz, "out of memory for %lu decoded bytes", (unsigned long)rawlen); goto done; }

    ZIn z;
    memset(&z, 0, sizeof(z));
    z.in = idat.p + 2;
    z.len = idat.len - 2;
    z.out = raw;
    z.cap = rawlen;
    if (inflate_raw(&z) != 0) {
        snprintf(err, esz, "%s: the compressed stream is damaged (byte %lu of %lu)",
                 path, (unsigned long)z.pos, (unsigned long)z.len);
        free(raw);
        goto done;
    }
    if (z.n != rawlen) {
        snprintf(err, esz, "%s: stream expands to %lu bytes, expected %lu",
                 path, (unsigned long)z.n, (unsigned long)rawlen);
        free(raw);
        goto done;
    }
    /* the 4 byte adler32 that closes the zlib stream */
    {
        size_t at = z.pos;
        if (at + 4 <= z.len) {
            u32 want = be32(z.in + at);
            u32 got = adler32_of(raw, rawlen);
            if (want != got) {
                snprintf(err, esz, "%s: adler32 mismatch in the pixel stream", path);
                free(raw);
                goto done;
            }
        }
    }
    if (unfilter(raw, W, H, channels, err, esz) != 0) {
        free(raw);
        goto done;
    }

    /* convert to ARGB over the canvas background, then place it */
    u32 *src = malloc((size_t)W * (size_t)H * sizeof(u32));
    if (!src) {
        snprintf(err, esz, "out of memory for %dx%d pixels", W, H);
        free(raw);
        goto done;
    }
    for (int y = 0; y < H; y++) {
        const u8 *row = raw + (size_t)y * (stride + 1u) + 1u;
        for (int x = 0; x < W; x++) {
            u32 c = 0;
            const u8 *q2 = row + (size_t)x * (size_t)channels;
            if (colour == 0) {
                c = over(CANVAS_BG, q2[0], q2[0], q2[0], 255);
            } else if (colour == 2) {
                c = over(CANVAS_BG, q2[0], q2[1], q2[2], 255);
            } else if (colour == 3) {
                int idx = q2[0];
                if (idx >= pal_n) idx = 0;
                c = over(CANVAS_BG, pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], 255);
            } else if (colour == 4) {
                c = over(CANVAS_BG, q2[0], q2[0], q2[0], q2[1]);
            } else {
                c = over(CANVAS_BG, q2[0], q2[1], q2[2], q2[3]);
            }
            src[(size_t)y * (size_t)W + (size_t)x] = c;
        }
    }
    free(raw);

    /* fit: never enlarge, keep the aspect ratio, centre on the canvas */
    double sc = 1.0;
    double sx = (double)dw / (double)W;
    double sy = (double)dh / (double)H;
    if (sx < sc) sc = sx;
    if (sy < sc) sc = sy;
    int tw = (int)((double)W * sc + 0.5);
    int th = (int)((double)H * sc + 0.5);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw > dw) tw = dw;
    if (th > dh) th = dh;
    int ox = (dw - tw) / 2;
    int oy = (dh - th) / 2;

    for (int i = 0; i < dw * dh; i++) dst[i] = CANVAS_BG;
    for (int y = 0; y < th; y++) {
        int sy2 = (int)(((double)y + 0.5) / sc);
        if (sy2 < 0) sy2 = 0;
        if (sy2 >= H) sy2 = H - 1;
        for (int x = 0; x < tw; x++) {
            int sx2 = (int)(((double)x + 0.5) / sc);
            if (sx2 < 0) sx2 = 0;
            if (sx2 >= W) sx2 = W - 1;
            dst[(size_t)(oy + y) * (size_t)dw + (size_t)(ox + x)] =
                src[(size_t)sy2 * (size_t)W + (size_t)sx2];
        }
    }
    free(src);
    *sw = W;
    *sh = H;
    rc = 0;

done:
    free(idat.p);
    free(buf);
    return rc;
}

/* ============================================================
 *  Canvas, brush and history
 * ============================================================ */

static u32 canvas[CANVAS_PX];

/* brush stamps: the offsets of a disc of diameter s, built once */
typedef struct { signed char dx, dy; } Off;
static Off stamp_off[MAX_BRUSH + 1][256];
static int stamp_n[MAX_BRUSH + 1];

static void stamps_build(void) {
    for (int s = 1; s <= MAX_BRUSH; s++) {
        int n = 0;
        if (s <= 2) {
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++) {
                    if (n >= 256) break;
                    stamp_off[s][n].dx = (signed char)x;
                    stamp_off[s][n].dy = (signed char)y;
                    n++;
                }
        } else {
            int s2 = s * s;
            for (int dy = -s / 2; dy <= s / 2; dy++)
                for (int dx = -s / 2; dx <= s / 2; dx++) {
                    if (4 * (dx * dx + dy * dy) > s2 + s) continue;
                    if (n >= 256) break;
                    stamp_off[s][n].dx = (signed char)dx;
                    stamp_off[s][n].dy = (signed char)dy;
                    n++;
                }
        }
        stamp_n[s] = n;
    }
}

/* A target for the rasterisers: either the committed canvas or the
 * shared pixel buffer during a drag preview. Clipping is in target
 * coordinates so a preview can never leave the canvas rectangle. */
typedef struct {
    u32 *px;
    int  stride;
    int  ox, oy;                 /* canvas coord -> target coord */
    int  cx0, cy0, cx1, cy1;     /* clip, [cx0,cx1) x [cy0,cy1) */
    u32  col;
    int  brush;
    long hits;                   /* pixels actually changed */
} Ink;

static void ink_init(Ink *k, u32 *px, int stride, int ox, int oy,
                     int x0, int y0, int x1, int y1, u32 col, int brush) {
    k->px = px;
    k->stride = stride;
    k->ox = ox;
    k->oy = oy;
    k->cx0 = x0;
    k->cy0 = y0;
    k->cx1 = x1;
    k->cy1 = y1;
    k->col = col;
    k->brush = brush < 1 ? 1 : (brush > MAX_BRUSH ? MAX_BRUSH : brush);
    k->hits = 0;
}

static void ink_set(Ink *k, int X, int Y) {
    if (X < k->cx0 || Y < k->cy0 || X >= k->cx1 || Y >= k->cy1) return;
    u32 *p = &k->px[(size_t)Y * (size_t)k->stride + (size_t)X];
    if (*p == k->col) return;
    *p = k->col;
    k->hits++;
}

static void ink_stamp(Ink *k, int x, int y) {
    int X = x + k->ox, Y = y + k->oy;
    int b = k->brush;
    for (int i = 0; i < stamp_n[b]; i++)
        ink_set(k, X + stamp_off[b][i].dx, Y + stamp_off[b][i].dy);
}

/* one horizontal run of a filled shape - the brush width does not apply
 * to a fill, only to an outline */
static void ink_span(Ink *k, int xa, int xb, int y) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    int Y = y + k->oy;
    if (Y < k->cy0 || Y >= k->cy1) return;
    int A = xa + k->ox, B = xb + k->ox;
    if (A < k->cx0) A = k->cx0;
    if (B >= k->cx1) B = k->cx1 - 1;
    if (A > B) return;
    u32 *p = &k->px[(size_t)Y * (size_t)k->stride + (size_t)A];
    for (int X = A; X <= B; X++) {
        if (*p != k->col) { *p = k->col; k->hits++; }
        p++;
    }
}

static void ink_line(Ink *k, int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;        /* negative */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ink_stamp(k, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void ink_rect(Ink *k, int x0, int y0, int x1, int y1, int filled) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (filled) {
        for (int y = y0; y <= y1; y++) ink_span(k, x0, x1, y);
        return;
    }
    ink_line(k, x0, y0, x1, y0);
    ink_line(k, x0, y1, x1, y1);
    ink_line(k, x0, y0, x0, y1);
    ink_line(k, x1, y0, x1, y1);
}

/* An ellipse inscribed in the drag rectangle. The outline is sampled
 * along both axes, so it stays closed whatever the aspect ratio; the
 * filled form asks the ellipse equation for each row's half width. */
static void ink_ellipse(Ink *k, int x0, int y0, int x1, int y1, int filled) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    double cx = ((double)x0 + (double)x1) * 0.5;
    double cy = ((double)y0 + (double)y1) * 0.5;
    double a = ((double)x1 - (double)x0) * 0.5;
    double b = ((double)y1 - (double)y0) * 0.5;
    if (a < 0.5 || b < 0.5) {
        k->brush = 1;
        ink_line(k, x0, y0, x1, y1);
        return;
    }
    if (filled) {
        for (int y = y0; y <= y1; y++) {
            double t = ((double)y - cy) / b;
            if (t < -1.0 || t > 1.0) continue;
            double w = a * sqrt(1.0 - t * t);
            int xa = (int)floor(cx - w + 0.5);
            int xb = (int)floor(cx + w + 0.5);
            ink_span(k, xa, xb, y);
        }
        return;
    }
    for (int x = x0; x <= x1; x++) {
        double t = ((double)x - cx) / a;
        if (t < -1.0 || t > 1.0) continue;
        double h = b * sqrt(1.0 - t * t);
        ink_stamp(k, x, (int)floor(cy - h + 0.5));
        ink_stamp(k, x, (int)floor(cy + h + 0.5));
    }
    for (int y = y0; y <= y1; y++) {
        double t = ((double)y - cy) / b;
        if (t < -1.0 || t > 1.0) continue;
        double w = a * sqrt(1.0 - t * t);
        ink_stamp(k, (int)floor(cx - w + 0.5), y);
        ink_stamp(k, (int)floor(cx + w + 0.5), y);
    }
}

static void shape_draw(Ink *k, int tool, int x0, int y0, int x1, int y1) {
    switch (tool) {
    case T_LINE:     ink_line(k, x0, y0, x1, y1); break;
    case T_RECT:     ink_rect(k, x0, y0, x1, y1, 0); break;
    case T_RECTF:    ink_rect(k, x0, y0, x1, y1, 1); break;
    case T_ELLIPSE:  ink_ellipse(k, x0, y0, x1, y1, 0); break;
    case T_ELLIPSEF: ink_ellipse(k, x0, y0, x1, y1, 1); break;
    default: break;
    }
}

/* ---- flood fill -------------------------------------------------
 * Span fill: find the horizontal run through the seed, paint it, then
 * push one seed per contiguous run on the rows above and below. The
 * stack is malloc'd and grown as needed - no recursion, no fixed depth
 * limit small enough to overflow on a striped picture. */
#define FILL_STACK_MAX 262144

static long flood_fill(int sx, int sy, u32 col, char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    if ((unsigned)sx >= (unsigned)CANVAS_W || (unsigned)sy >= (unsigned)CANVAS_H) return 0;
    u32 target = canvas[(size_t)sy * CANVAS_W + (size_t)sx];
    if (target == col) return 0;

    size_t cap = 1024;
    int *st = malloc(cap * 2u * sizeof(int));
    if (!st) { snprintf(err, esz, "fill: out of memory for the span stack"); return 0; }
    size_t n = 0;
    st[n++] = sx;
    st[n++] = sy;
    long changed = 0;

    while (n > 0) {
        int y = st[--n];
        int x = st[--n];
        if ((unsigned)x >= (unsigned)CANVAS_W || (unsigned)y >= (unsigned)CANVAS_H) continue;
        u32 *row = &canvas[(size_t)y * CANVAS_W];
        if (row[x] != target) continue;

        int xl = x;
        while (xl > 0 && row[xl - 1] == target) xl--;
        int xr = x;
        while (xr < CANVAS_W - 1 && row[xr + 1] == target) xr++;
        for (int i = xl; i <= xr; i++) row[i] = col;
        changed += (long)(xr - xl + 1);

        for (int ny = y - 1; ny <= y + 1; ny += 2) {
            if ((unsigned)ny >= (unsigned)CANVAS_H) continue;
            u32 *nrow = &canvas[(size_t)ny * CANVAS_W];
            int i = xl;
            while (i <= xr) {
                while (i <= xr && nrow[i] != target) i++;
                if (i > xr) break;
                if (n + 2 > cap * 2u) {
                    if (cap >= FILL_STACK_MAX) {
                        snprintf(err, esz, "fill stopped: more than %d spans", FILL_STACK_MAX);
                        free(st);
                        return changed;
                    }
                    cap *= 2;
                    int *np = realloc(st, cap * 2u * sizeof(int));
                    if (!np) {
                        snprintf(err, esz, "fill: out of memory growing the span stack");
                        free(st);
                        return changed;
                    }
                    st = np;
                }
                st[n++] = i;
                st[n++] = ny;
                while (i <= xr && nrow[i] == target) i++;
            }
        }
    }
    free(st);
    return changed;
}

/* ---- history ---------------------------------------------------- */

static u32 *hist[HIST_STATES];
static int  hist_n;              /* how many states are stored */
static int  hist_at;             /* index of the state the canvas shows */
static size_t hist_bytes;
static int  hist_ok = 1;
static int  hist_oom;

static void hist_free_at(int i) {
    if (!hist[i]) return;
    free(hist[i]);
    hist[i] = NULL;
    hist_bytes -= CANVAS_BYTES;
}

static int hist_init(void) {
    for (int i = 0; i < HIST_STATES; i++) hist[i] = NULL;
    hist_n = hist_at = 0;
    hist_bytes = 0;
    hist_ok = 1;
    hist_oom = 0;
    u32 *s = malloc(CANVAS_BYTES);
    if (!s) { hist_ok = 0; hist_oom = 1; return -1; }
    memcpy(s, canvas, CANVAS_BYTES);
    hist[0] = s;
    hist_n = 1;
    hist_at = 0;
    hist_bytes = CANVAS_BYTES;
    return 0;
}

/* Record the canvas as a new state, dropping any redo tail. When the
 * ring is full the oldest state is freed, so memory is bounded at
 * HIST_STATES * CANVAS_BYTES for the whole life of the process. */
static int hist_commit(void) {
    if (!hist_ok) return -1;
    for (int i = hist_at + 1; i < hist_n; i++) hist_free_at(i);
    hist_n = hist_at + 1;
    if (hist_n >= HIST_STATES) {
        hist_free_at(0);
        for (int i = 1; i < HIST_STATES; i++) hist[i - 1] = hist[i];
        hist[HIST_STATES - 1] = NULL;
        hist_n--;
        hist_at--;
    }
    if (!hist[hist_n]) {
        u32 *s = malloc(CANVAS_BYTES);
        if (!s) { hist_ok = 0; hist_oom = 1; return -1; }
        hist[hist_n] = s;
        hist_bytes += CANVAS_BYTES;
    }
    memcpy(hist[hist_n], canvas, CANVAS_BYTES);
    hist_at = hist_n;
    hist_n++;
    return 0;
}

static int hist_undo(void) {
    if (!hist_ok || hist_at <= 0) return 0;
    hist_at--;
    memcpy(canvas, hist[hist_at], CANVAS_BYTES);
    return 1;
}

static int hist_redo(void) {
    if (!hist_ok || hist_at + 1 >= hist_n) return 0;
    hist_at++;
    memcpy(canvas, hist[hist_at], CANVAS_BYTES);
    return 1;
}

static void hist_done(void) {
    for (int i = 0; i < HIST_STATES; i++) hist_free_at(i);
}

/* ---- canvas ----------------------------------------------------- */

static void canvas_reset(void) {
    for (size_t i = 0; i < (size_t)CANVAS_PX; i++) canvas[i] = CANVAS_BG;
}

/* ============================================================
 *  Files
 * ============================================================ */

static char g_dir[64];
static int  g_dir_tried;
static int  g_msg_bad;          /* the last message was a failure */

/* mkdir(2) takes a mode on Linux; the Windows host harness needs _mkdir.
 * This branch only exists under PAINT_HOST_TEST, so the shipped object
 * still calls the real mkdir(path, 0755). */
#ifdef PAINT_HOST_TEST
#  ifdef _WIN32
#    include <direct.h>
#    define paint_mkdir(p) _mkdir(p)
#  else
#    define paint_mkdir(p) mkdir((p), 0755)
#  endif
#else
#  define paint_mkdir(p) mkdir((p), 0755)
#endif

/* The desktop's /root/Pictures, or the jail-visible /tmp/Pictures when
 * the sandbox has hidden /root. Whatever was actually used is printed,
 * so the answer to "where did my picture go" is on screen. */
static const char *paint_dir(char *why, size_t whyn) {
    if (why && whyn) why[0] = 0;
    if (g_dir[0]) return g_dir;
    if (g_dir_tried) return NULL;
    g_dir_tried = 1;
    const char *cand[2] = { PIC_DIR, PIC_DIR_FALLBACK };
    for (int i = 0; i < 2; i++) {
        if (paint_mkdir(cand[i]) != 0 && errno != EEXIST) {
            if (i == 0 && why && whyn)
                snprintf(why, whyn, "%s: %s", cand[i], strerror(errno));
            continue;
        }
        if (access(cand[i], W_OK) == 0) {
            snprintf(g_dir, sizeof(g_dir), "%s", cand[i]);
            return g_dir;
        }
        if (i == 0 && why && whyn)
            snprintf(why, whyn, "%s: not writable (%s)", cand[i], strerror(errno));
    }
    return NULL;
}

static int ends_png(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *e = name + n - 4;
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') &&
           (e[2] == 'n' || e[2] == 'N') && (e[3] == 'g' || e[3] == 'G');
}

static void act_save(char *msg, size_t n) {
    g_msg_bad = 1;
    char why[120];
    const char *dir = paint_dir(why, sizeof(why));
    if (!dir) {
        snprintf(msg, n, "save: no writable picture folder (%s)", why[0] ? why : "none");
        return;
    }
    char path[256];
    path[0] = 0;
    for (int i = 1; i <= 9999; i++) {
        struct stat st;
        snprintf(path, sizeof(path), "%s/paint-%04d.png", dir, i);
        if (stat(path, &st) != 0) break;
        path[0] = 0;
    }
    if (!path[0]) { snprintf(msg, n, "save: paint-0001..9999 all exist"); return; }

    char err[160];
    long bytes = png_write_file(path, canvas, CANVAS_W, CANVAS_H, err, sizeof(err));
    if (bytes < 0) { snprintf(msg, n, "save failed: %s", err); return; }
    g_msg_bad = 0;
    if (!strcmp(dir, PIC_DIR))
        snprintf(msg, n, "saved %s  (%ld bytes)", path, bytes);
    else
        snprintf(msg, n, "saved %s  (%ld bytes)  [/root hidden: %s]", path, bytes, why);
}

/* Open the newest .png in the picture folder. Newest by st_mtime, which
 * is the order the user thinks in after saving a few files. */
static void act_open(char *msg, size_t n) {
    g_msg_bad = 1;
    const char *dirs[2] = { PIC_DIR, PIC_DIR_FALLBACK };
    char best[256];
    best[0] = 0;
    time_t best_t = 0;
    int seen = 0;

    for (int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != NULL) {
            if (!ends_png(e->d_name)) continue;
            seen++;
            char p[256];
            snprintf(p, sizeof(p), "%s/%s", dirs[d], e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (!best[0] || st.st_mtime > best_t ||
                (st.st_mtime == best_t && strcmp(p, best) > 0)) {
                best_t = st.st_mtime;
                snprintf(best, sizeof(best), "%s", p);
            }
        }
        closedir(dp);
    }
    if (!best[0]) {
        snprintf(msg, n, "open: no .png in %s (%d files seen)", PIC_DIR, seen);
        return;
    }
    int sw = 0, sh = 0;
    char err[160];
    if (png_read_file(best, canvas, CANVAS_W, CANVAS_H, &sw, &sh, err, sizeof(err)) != 0) {
        snprintf(msg, n, "open failed: %s", err);
        return;
    }
    g_msg_bad = 0;
    int had_undo = hist_commit() == 0;
    snprintf(msg, n, "opened %s  %dx%d -> canvas %dx%d%s", best, sw, sh,
             CANVAS_W, CANVAS_H, had_undo ? "" : "  (no undo memory)");
}

/* ============================================================
 *  Layout and UI
 * ============================================================ */

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    int  vw, vh;
    Rect canvas;
    Rect tool[T_N];
    Rect brush_minus, brush_plus;
    Rect undo_btn, redo_btn;
    Rect swatch[PAL_N];
    Rect sl[3];
    Rect dlg_yes, dlg_no, dlg;
    int  y_colour, y_preview, y_hex, y_mixer, y_drag, y_name;
    int  y_info[6];
    int  line_h;
} Layout;

typedef struct {
    Font *body, *small, *mono, *big;
} Fonts;

typedef struct {
    int tool, brush;
    int r, g, b;
    int shift;
    int down;        /* left button held while painting the canvas */
    int drag;        /* a shape is being dragged */
    int sx, sy;      /* where the drag started, canvas coords */
    int px, py;      /* where the pointer is, canvas coords */
    int lx, ly;      /* last pencil position */
    int slider;      /* 0/1/2 while dragging a mixer slider, else -1 */
    int dialog;      /* 1 while the clear confirmation is up */
    int cursor_in;   /* pointer is over the canvas */
    int stroke;      /* this pencil stroke changed at least one pixel */
} PaintState;

static Rect rect_make(int x, int y, int w, int h) { Rect r = { x, y, w, h }; return r; }

static int rect_hit(const Rect *r, int x, int y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* Every position below comes from the real font metrics, so a larger text
 * scale moves the rows instead of letting them overlap. */
static void layout_build(int vw, int vh, const Fonts *F, Layout *L) {
    memset(L, 0, sizeof(*L));
    L->vw = vw;
    L->vh = vh;

    int sh = (int)font_height(F->small);
    int mh = (int)font_height(F->mono);
    int gh = (int)font_height(F->big);
    if (sh < 1) sh = 1;
    if (mh < 1) mh = 1;
    if (gh < 1) gh = 1;
    L->line_h = sh;

    L->canvas = rect_make(CX0, CY0, CANVAS_W, CANVAS_H);
    if (L->canvas.x + L->canvas.w > vw) L->canvas.w = vw - L->canvas.x;
    if (L->canvas.y + L->canvas.h > vh) L->canvas.h = vh - L->canvas.y;
    if (L->canvas.w < 0) L->canvas.w = 0;
    if (L->canvas.h < 0) L->canvas.h = 0;

    /* left rail: eight tools, then brush width and history */
    int th = sh + 6;
    if (th < 22) th = 22;
    int ty = 8;
    for (int i = 0; i < T_N; i++) {
        L->tool[i] = rect_make(RAIL_X, ty, RAIL_W, th);
        ty += th + 4;
    }
    ty += 6;
    L->brush_minus = rect_make(RAIL_X, ty + sh + 2, 40, th - 4);
    L->brush_plus  = rect_make(RAIL_X + 44, ty + sh + 2, 40, th - 4);
    ty += sh + 2 + (th - 4) + 8;
    L->undo_btn = rect_make(RAIL_X, ty, 40, th - 4);
    L->redo_btn = rect_make(RAIL_X + 44, ty, 40, th - 4);

    /* right panel: colour preview, hex code, palette, mixer */
    int y = 8;
    L->y_colour = y;
    y += sh + 2;
    L->y_preview = y;
    y += 32;
    L->y_hex = y;
    y += mh + 4;
    for (int i = 0; i < PAL_N; i++) {
        int c = i % 4, r = i / 4;
        L->swatch[i] = rect_make(PANEL_X + c * 32, y + r * 26, 28, 22);
    }
    y += 3 * 26 - 2;
    L->y_mixer = y + 2;
    y += sh + 10;
    for (int i = 0; i < 3; i++) {
        L->sl[i] = rect_make(PANEL_X + 16, y + sh - 2, 78, 10);
        y += sh + 12;
    }
    L->y_drag = y + 2;
    L->y_name = y + sh + 2;

    /* the six information lines, anchored to the bottom of the window */
    int dy = sh + 3;
    int start = CY0 + CANVAS_H + 16;
    if (start + 6 * dy > vh - 4) start = vh - 4 - 6 * dy;
    if (start < CY0 + CANVAS_H + 4) start = CY0 + CANVAS_H + 4;
    for (int i = 0; i < 6; i++) L->y_info[i] = start + i * dy;

    /* the clear confirmation */
    int dw = 340;
    if (dw > vw - 20) dw = vw - 20;
    if (dw < 120) dw = 120;
    int dh = 14 + gh + 6 + sh + 14 + 26 + 14;
    L->dlg = rect_make((vw - dw) / 2, (vh - dh) / 2, dw, dh);
    L->dlg_yes = rect_make(L->dlg.x + 16, L->dlg.y + dh - 40, 140, 26);
    L->dlg_no  = rect_make(L->dlg.x + dw - 156, L->dlg.y + dh - 40, 140, 26);
}

/* Copy t into buf, cutting whole UTF-8 characters until it fits maxw. */
static void fit_text(char *buf, size_t bsz, Font *f, const char *t, int maxw) {
    if (buf != t) snprintf(buf, bsz, "%s", t);   /* callers pass buf as t too */
    size_t n = strlen(buf);
    while (n > 1 && font_width(f, buf) > (float)maxw) {
        buf[--n] = 0;
        while (n > 1 && ((u8)buf[n] & 0xC0) == 0x80) buf[--n] = 0;
    }
}

static void draw_button(Surface *s, Font *f, Rect r, const char *label,
                        int active, int enabled) {
    u32 fill = active ? color_alpha(C_ACCENT, 0.20f) : C_PANEL;
    u32 border = active ? C_ACCENT : C_LINE;
    gfx_rrect_aa(s, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 4.0f,
                 fill, border, 1.0f);
    u32 tc = enabled ? (active ? C_TEXT : C_DIM) : color_alpha(C_DIM, 0.45f);
    char buf[64];
    fit_text(buf, sizeof(buf), f, label, r.w - 6);
    float tw = font_width(f, buf);
    float ty = (float)r.y + ((float)r.h - font_height(f)) * 0.5f;
    if (ty < (float)r.y + 1.0f) ty = (float)r.y + 1.0f;
    font_draw(s, f, (float)r.x + ((float)r.w - tw) * 0.5f, ty, buf, tc);
}

static void draw_slider(Surface *s, Rect r, int v, u32 channel_col, int active) {
    gfx_rrect_aa(s, (float)r.x, (float)r.y, (float)r.w, (float)r.h,
                 (float)r.h * 0.5f, color_alpha(C_TEXT, 0.10f), 0, 0);
    float knob = (float)r.x + ((float)(r.w - 1) * (float)v) / 255.0f;
    float kw = (float)r.h + 4.0f;
    gfx_rrect_aa(s, knob - kw * 0.5f, (float)r.y - 2.0f, kw, (float)r.h + 4.0f,
                 kw * 0.5f, channel_col, active ? C_TEXT : C_LINE, active ? 1.5f : 1.0f);
}

static void draw_ui(MClient *c, const Layout *L, const Fonts *F,
                    const PaintState *st, const char *msg, int msg_warn) {
    int vw = L->vw, vh = L->vh;
    if (vw <= 0 || vh <= 0) return;
    Surface s = { .px = c->px, .w = vw, .h = vh, .stride = c->stride };

    surf_fill(&s, 0, 0, vw, vh, C_BG);

    /* ---- canvas ---- */
    if (L->canvas.w > 0 && L->canvas.h > 0) {
        gfx_rrect_aa(&s, (float)L->canvas.x - 2, (float)L->canvas.y - 2,
                     (float)L->canvas.w + 4, (float)L->canvas.h + 4, 3.0f,
                     0, C_LINE, 2.0f);
        Surface cv = { .px = canvas, .w = CANVAS_W, .h = CANVAS_H, .stride = CANVAS_W };
        surf_push(&s, L->canvas.x, L->canvas.y, &cv, 0, 0,
                  L->canvas.w, L->canvas.h);
    }

    /* ---- live preview of the shape being dragged ---- */
    if (st->drag && L->canvas.w > 0) {
        Ink k;
        int x1 = st->px, y1 = st->py;
        int x0 = st->sx, y0 = st->sy;
        if (st->shift) {
            int dx = x1 - x0, dy = y1 - y0;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            if (st->tool == T_LINE) {
                if (adx > 2 * ady) y1 = y0;
                else if (ady > 2 * adx) x1 = x0;
                else {
                    int m = adx > ady ? adx : ady;
                    x1 = x0 + (dx < 0 ? -m : m);
                    y1 = y0 + (dy < 0 ? -m : m);
                }
            } else {
                int m = adx > ady ? adx : ady;
                x1 = x0 + (dx < 0 ? -m : m);
                y1 = y0 + (dy < 0 ? -m : m);
            }
        }
        ink_init(&k, s.px, s.stride, L->canvas.x, L->canvas.y,
                 L->canvas.x, L->canvas.y,
                 L->canvas.x + L->canvas.w, L->canvas.y + L->canvas.h,
                 RGB(st->r, st->g, st->b), st->brush);
        shape_draw(&k, st->tool, x0, y0, x1, y1);
    }

    /* ---- tool rail ---- */
    for (int i = 0; i < T_N; i++) {
        char lab[32];
        snprintf(lab, sizeof(lab), "%d %s", i + 1, TOOL_NAME[i]);
        draw_button(&s, F->small, L->tool[i], lab, st->tool == i, 1);
    }
    {
        char b[16];
        int by = L->brush_minus.y - L->line_h - 2;
        snprintf(b, sizeof(b), "BRUSH %d", st->brush);
        font_draw(&s, F->small, (float)RAIL_X, (float)by, b, color_alpha(C_TEXT, 0.5f));
        draw_button(&s, F->body, L->brush_minus, "-", 0, st->brush > 1);
        draw_button(&s, F->body, L->brush_plus, "+", 0, st->brush < MAX_BRUSH);
        draw_button(&s, F->small, L->undo_btn, "undo", 0, hist_at > 0);
        draw_button(&s, F->small, L->redo_btn, "redo", 0, hist_at + 1 < hist_n);
    }

    /* ---- colour panel ---- */
    font_draw(&s, F->small, (float)PANEL_X, (float)L->y_colour, "COLOUR",
              color_alpha(C_TEXT, 0.5f));
    gfx_rrect_aa(&s, (float)PANEL_X, (float)L->y_preview, (float)PANEL_W, 30.0f,
                 5.0f, RGB(st->r, st->g, st->b), C_LINE, 1.0f);
    {
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X", st->r, st->g, st->b);
        font_draw(&s, F->mono, (float)PANEL_X, (float)L->y_hex, hex, C_TEXT);
    }
    for (int i = 0; i < PAL_N; i++) {
        int chosen = st->r == (int)GET_R(PALETTE[i]) &&
                     st->g == (int)GET_G(PALETTE[i]) &&
                     st->b == (int)GET_B(PALETTE[i]);
        if (chosen)
            gfx_rrect_aa(&s, (float)L->swatch[i].x - 2, (float)L->swatch[i].y - 2,
                         (float)L->swatch[i].w + 4, (float)L->swatch[i].h + 4, 6.0f,
                         color_mix(PALETTE[i], C_TEXT, 0.55f), 0, 0);
        gfx_rrect_aa(&s, (float)L->swatch[i].x, (float)L->swatch[i].y,
                     (float)L->swatch[i].w, (float)L->swatch[i].h, 4.0f,
                     PALETTE[i], 0, 0);
    }
    font_draw(&s, F->small, (float)PANEL_X, (float)L->y_mixer, "MIXER  24-BIT",
              color_alpha(C_TEXT, 0.5f));
    {
        static const char *nm[3] = { "R", "G", "B" };
        int v[3] = { st->r, st->g, st->b };
        u32 cc[3] = { RGB(0xE0, 0x6C, 0x5C), RGB(0x7B, 0xD8, 0x8F), RGB(0x7A, 0xA2, 0xF7) };
        for (int i = 0; i < 3; i++) {
            float y = (float)L->sl[i].y - L->line_h + 2.0f;
            font_draw(&s, F->small, (float)PANEL_X, y, nm[i], cc[i]);
            draw_slider(&s, L->sl[i], v[i], cc[i], st->slider == i);
            char num[8];
            snprintf(num, sizeof(num), "%d", v[i]);
            font_draw(&s, F->mono, (float)(PANEL_X + PANEL_W) - font_width(F->mono, num),
                      y, num, C_TEXT);
        }
    }
    {
        /* at the largest text scale the panel is taller than the canvas; do
         * not let its last rows run into the information block */
        int room = L->y_info[0] - 8;
        char nm2[32];
        snprintf(nm2, sizeof(nm2), "%s", PAL_NAME[0]);
        for (int i = 0; i < PAL_N; i++)
            if (st->r == (int)GET_R(PALETTE[i]) && st->g == (int)GET_G(PALETTE[i]) &&
                st->b == (int)GET_B(PALETTE[i]))
                snprintf(nm2, sizeof(nm2), "%s", PAL_NAME[i]);
        if (L->y_drag + L->line_h <= room)
            font_draw(&s, F->small, (float)PANEL_X, (float)L->y_drag, "drag R G B",
                      color_alpha(C_TEXT, 0.4f));
        if (L->y_name + L->line_h <= room)
            font_draw(&s, F->small, (float)PANEL_X, (float)L->y_name, nm2, C_DIM);
    }

    /* ---- information lines ---- */
    surf_blend(&s, RAIL_X, L->y_info[0] - 8, vw - 2 * RAIL_X, 1,
               color_alpha(C_TEXT, 0.10f));
    {
        char line[220];
        char why[120];
        const char *dir = paint_dir(why, sizeof(why));
        snprintf(line, sizeof(line), "FILE  %s", dir ? dir : "(no writable folder)");
        fit_text(line, sizeof(line), F->small, line, vw - 2 * RAIL_X - 170);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[0], line, C_DIM);
    }
    {
        char right[64], buf[64];
        snprintf(right, sizeof(right), "%s   brush %d", TOOL_NAME[st->tool], st->brush);
        fit_text(buf, sizeof(buf), F->small, right, 160);
        float rw = font_width(F->small, buf);
        font_draw(&s, F->small, (float)(vw - RAIL_X) - rw, (float)L->y_info[0], buf,
                  C_ACCENT);
    }
    {
        char line[200];
        unsigned long used_t = (unsigned long)(hist_bytes * 10u / (1024u * 1024u));
        unsigned long cap_t  = (unsigned long)((size_t)HIST_STATES * CANVAS_BYTES * 10u /
                                               (1024u * 1024u));
        if (!hist_ok)
            snprintf(line, sizeof(line), "UNDO  unavailable: out of memory for a snapshot");
        else
            snprintf(line, sizeof(line),
                     "UNDO  %d of %d steps   history %lu.%lu MB of %lu.%lu MB   redo %d",
                     hist_at, HIST_STATES - 1, used_t / 10ul, used_t % 10ul,
                     cap_t / 10ul, cap_t % 10ul, hist_n - 1 - hist_at);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[1], line,
                  hist_oom ? C_WARN : C_DIM);
    }
    {
        char line[200];
        snprintf(line, sizeof(line),
                 "1-8 tool   [ ] brush   u undo   y redo   s save   o open   c clear   q quit");
        fit_text(line, sizeof(line), F->small, line, vw - 2 * RAIL_X);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[2], line,
                  color_alpha(C_TEXT, 0.55f));
        snprintf(line, sizeof(line),
                 "drag to draw   shift = 45 deg lines and square/circle boxes   k picks a colour");
        fit_text(line, sizeof(line), F->small, line, vw - 2 * RAIL_X);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[3], line,
                  color_alpha(C_TEXT, 0.40f));
    }
    if (msg && msg[0]) {
        char line[220];
        fit_text(line, sizeof(line), F->small, msg, vw - 2 * RAIL_X - 130);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[4], line,
                  msg_warn ? C_WARN : C_ACCENT);
    }
    {
        char pos[48];
        if (st->cursor_in)
            snprintf(pos, sizeof(pos), "pen %d,%d", st->px, st->py);
        else
            snprintf(pos, sizeof(pos), "pen -, -");
        float pw = font_width(F->mono, pos);
        font_draw(&s, F->mono, (float)(vw - RAIL_X) - pw, (float)L->y_info[4], pos,
                  color_alpha(C_TEXT, 0.45f));
    }
    {
        char line[200];
        snprintf(line, sizeof(line),
                 "saves a real PNG (zlib stored blocks); opens 8-bit grey/RGB/palette/RGBA PNGs");
        fit_text(line, sizeof(line), F->small, line, vw - 2 * RAIL_X);
        font_draw(&s, F->small, (float)RAIL_X, (float)L->y_info[5], line,
                  color_alpha(C_TEXT, 0.30f));
    }

    /* ---- the clear confirmation ---- */
    if (st->dialog) {
        surf_blend(&s, 0, 0, vw, vh, color_alpha(RGB(0, 0, 0), 0.55f));
        gfx_rrect_aa(&s, (float)L->dlg.x, (float)L->dlg.y,
                     (float)L->dlg.w, (float)L->dlg.h, 8.0f,
                     C_PANEL, C_ACCENT, 1.5f);
        font_draw(&s, F->big, (float)L->dlg.x + 16.0f, (float)L->dlg.y + 14.0f,
                  "Clear the canvas?", C_TEXT);
        {
            char line[200];
            snprintf(line, sizeof(line), "Undo (u) brings the last state back.");
            fit_text(line, sizeof(line), F->small, line, L->dlg.w - 32);
            font_draw(&s, F->small, (float)L->dlg.x + 16.0f,
                      (float)L->dlg.y + 14.0f + font_height(F->big) + 6.0f,
                      line, C_DIM);
        }
        draw_button(&s, F->body, L->dlg_yes, "Clear", 1, 1);
        draw_button(&s, F->body, L->dlg_no, "Cancel", 0, 1);
    }
}

/* ============================================================
 *  Input
 * ============================================================ */

static void set_tool(PaintState *st, int t) {
    if (t >= 0 && t < T_N) st->tool = t;
}

static void do_clear(const Layout *L, PaintState *st, char *msg, size_t n, int *dirty) {
    (void)L;
    canvas_reset();
    if (hist_commit() != 0)
        snprintf(msg, n, "cleared; history is now unavailable");
    else
        snprintf(msg, n, "canvas cleared - u brings it back");
    st->dialog = 0;
    *dirty = 1;
}

static void handle_text(PaintState *st, const char *t, char *msg, size_t n,
                        const Layout *L, int *dirty, int *quit) {
    for (const char *p = t; *p && !*quit; p++) {
        char c = *p;
        if (st->dialog) {
            if (c == 'y' || c == 'Y') do_clear(L, st, msg, n, dirty);
            else if (c == 'n' || c == 'N' || c == 27) { st->dialog = 0; *dirty = 1; }
            continue;
        }
        if (c >= '1' && c <= '8') { set_tool(st, c - '1'); *dirty = 1; }
        else if (c == '[' || c == '{') {
            if (st->brush > 1) { st->brush--; *dirty = 1; }
        } else if (c == ']' || c == '}') {
            if (st->brush < MAX_BRUSH) { st->brush++; *dirty = 1; }
        } else if (c == 'u' || c == 'U') {
            if (hist_undo()) { snprintf(msg, n, "undo"); *dirty = 1; }
            else snprintf(msg, n, "nothing to undo");
            *dirty = 1;
        } else if (c == 'y' || c == 'Y') {
            if (hist_redo()) { snprintf(msg, n, "redo"); *dirty = 1; }
            else snprintf(msg, n, "nothing to redo");
            *dirty = 1;
        } else if (c == 's' || c == 'S') {
            act_save(msg, n);
            *dirty = 1;
        } else if (c == 'o' || c == 'O') {
            act_open(msg, n);
            *dirty = 1;
        } else if (c == 'c' || c == 'C') {
            st->dialog = 1;
            *dirty = 1;
        } else if (c == 'q' || c == 'Q') {
            *quit = 1;
        } else if (c == 'k' || c == 'K') {
            if (st->cursor_in &&
                (unsigned)st->px < (unsigned)CANVAS_W && (unsigned)st->py < (unsigned)CANVAS_H) {
                u32 v = canvas[(size_t)st->py * CANVAS_W + (size_t)st->px];
                st->r = GET_R(v);
                st->g = GET_G(v);
                st->b = GET_B(v);
                snprintf(msg, n, "picked #%02X%02X%02X", st->r, st->g, st->b);
                *dirty = 1;
            }
        }
    }
}

/* Apply a press: buttons, swatches, sliders, the canvas. */
static void press_at(PaintState *st, const Layout *L, int x, int y,
                     char *msg, size_t n, int *dirty) {
    if (st->dialog) {
        if (rect_hit(&L->dlg_yes, x, y)) do_clear(L, st, msg, n, dirty);
        else if (rect_hit(&L->dlg_no, x, y)) { st->dialog = 0; *dirty = 1; }
        return;
    }

    /* mixer sliders first: their knob sticks out of the track */
    for (int i = 0; i < 3; i++) {
        Rect r = L->sl[i];
        if (x >= r.x - 4 && x < r.x + r.w + 4 && y >= r.y - 8 && y < r.y + r.h + 8) {
            int v = (x - r.x) * 255 / (r.w - 1);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            if (i == 0) st->r = v;
            else if (i == 1) st->g = v;
            else st->b = v;
            st->slider = i;
            *dirty = 1;
            return;
        }
    }
    for (int i = 0; i < PAL_N; i++) {
        if (rect_hit(&L->swatch[i], x, y)) {
            st->r = GET_R(PALETTE[i]);
            st->g = GET_G(PALETTE[i]);
            st->b = GET_B(PALETTE[i]);
            *dirty = 1;
            return;
        }
    }
    for (int i = 0; i < T_N; i++) {
        if (rect_hit(&L->tool[i], x, y)) { set_tool(st, i); *dirty = 1; return; }
    }
    if (rect_hit(&L->brush_minus, x, y)) {
        if (st->brush > 1) st->brush--;
        *dirty = 1;
        return;
    }
    if (rect_hit(&L->brush_plus, x, y)) {
        if (st->brush < MAX_BRUSH) st->brush++;
        *dirty = 1;
        return;
    }
    if (rect_hit(&L->undo_btn, x, y)) {
        if (hist_undo()) { snprintf(msg, n, "undo"); *dirty = 1; }
        else snprintf(msg, n, "nothing to undo");
        *dirty = 1;
        return;
    }
    if (rect_hit(&L->redo_btn, x, y)) {
        if (hist_redo()) { snprintf(msg, n, "redo"); *dirty = 1; }
        else snprintf(msg, n, "nothing to redo");
        *dirty = 1;
        return;
    }
    if (!rect_hit(&L->canvas, x, y)) return;

    int cx = x - L->canvas.x, cy = y - L->canvas.y;
    if ((unsigned)cx >= (unsigned)CANVAS_W || (unsigned)cy >= (unsigned)CANVAS_H) return;
    st->px = cx;
    st->py = cy;
    st->cursor_in = 1;

    switch (st->tool) {
    case T_PENCIL: {
        Ink k;
        ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                 RGB(st->r, st->g, st->b), st->brush);
        ink_stamp(&k, cx, cy);
        st->lx = cx;
        st->ly = cy;
        st->down = 1;
        st->stroke = k.hits > 0;
        if (k.hits) *dirty = 1;
        break;
    }
    case T_FILL: {
        char err[96];
        long got = flood_fill(cx, cy, RGB(st->r, st->g, st->b), err, sizeof(err));
        g_msg_bad = err[0] != 0;
        if (got > 0 && hist_commit() != 0) err[0] = 0;
        if (err[0])
            snprintf(msg, n, "%s%s", err, got > 0 ? " (undo restores the part filled)" : "");
        else if (got == 0)
            snprintf(msg, n, "fill: that area is already this colour");
        else
            snprintf(msg, n, "filled %ld px%s", got, hist_ok ? "" : "; no undo memory");
        *dirty = 1;
        break;
    }
    case T_PICK: {
        u32 v = canvas[(size_t)cy * CANVAS_W + (size_t)cx];
        st->r = GET_R(v);
        st->g = GET_G(v);
        st->b = GET_B(v);
        snprintf(msg, n, "picked #%02X%02X%02X", st->r, st->g, st->b);
        *dirty = 1;
        break;
    }
    default:
        st->sx = cx;
        st->sy = cy;
        st->px = cx;
        st->py = cy;
        st->drag = 1;
        *dirty = 1;
        break;
    }
}

static void drag_at(PaintState *st, int x, int y, char *msg, size_t n, int *dirty) {
    (void)msg;
    (void)n;
    if (st->drag) {
        st->px = x;
        st->py = y;
        *dirty = 1;
        return;
    }
    if (st->down) {
        int cx = x, cy = y;
        if (cx < -64) cx = -64;
        if (cy < -64) cy = -64;
        if (cx > CANVAS_W + 64) cx = CANVAS_W + 64;
        if (cy > CANVAS_H + 64) cy = CANVAS_H + 64;
        Ink k;
        ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                 RGB(st->r, st->g, st->b), st->brush);
        ink_line(&k, st->lx, st->ly, cx, cy);
        st->lx = cx;
        st->ly = cy;
        st->px = cx;
        st->py = cy;
        if (k.hits) { st->stroke = 1; *dirty = 1; }
    }
}

static void release_at(PaintState *st, char *msg, size_t n, int *dirty) {
    if (st->drag) {
        int x1 = st->px, y1 = st->py;
        int x0 = st->sx, y0 = st->sy;
        if (st->shift) {
            int dx = x1 - x0, dy = y1 - y0;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            if (st->tool == T_LINE) {
                if (adx > 2 * ady) y1 = y0;
                else if (ady > 2 * adx) x1 = x0;
                else {
                    int m = adx > ady ? adx : ady;
                    x1 = x0 + (dx < 0 ? -m : m);
                    y1 = y0 + (dy < 0 ? -m : m);
                }
            } else {
                int m = adx > ady ? adx : ady;
                x1 = x0 + (dx < 0 ? -m : m);
                y1 = y0 + (dy < 0 ? -m : m);
            }
        }
        Ink k;
        ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                 RGB(st->r, st->g, st->b), st->brush);
        shape_draw(&k, st->tool, x0, y0, x1, y1);
        st->drag = 0;
        st->px = x1;
        st->py = y1;
        if (k.hits) {
            if (hist_commit() != 0)
                snprintf(msg, n, "drawn; history is now unavailable");
            *dirty = 1;
        }
        return;
    }
    if (st->down) {
        st->down = 0;
        if (!st->stroke) return;                 /* a click that changed nothing */
        st->stroke = 0;
        if (hist_commit() != 0) snprintf(msg, n, "stroke done; history is now unavailable");
        *dirty = 1;
    }
    st->slider = -1;
}

/* ============================================================
 *  main
 * ============================================================ */

#ifndef PAINT_HOST_TEST

int main(void) {
    MClient c;
    if (mc_open(&c, "paint", "Paint", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "paint: cannot reach the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    font_init("/usr/share/fonts");
    Fonts F;
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_REGULAR, 10);
    F.mono  = font_get(FONT_MONO, 10);
    F.big   = font_get(FONT_SEMIBOLD, 14);
    if (!F.body || !F.small || !F.mono || !F.big) {
        fprintf(stderr, "paint: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    stamps_build();
    canvas_reset();
    hist_init();

    int cap_w = c.w, cap_h = c.h;
    int vw = c.w, vh = c.h;

    PaintState st;
    memset(&st, 0, sizeof(st));
    st.tool = T_PENCIL;
    st.brush = 3;
    st.r = GET_R(PALETTE[0]);
    st.g = GET_G(PALETTE[0]);
    st.b = GET_B(PALETTE[0]);
    st.slider = -1;

    char msg[200];
    snprintf(msg, sizeof(msg), "ready - %d undo steps of %lu KB each",
             HIST_STATES - 1,
             (unsigned long)(CANVAS_BYTES / 1024u));
    Layout L;
    int dirty = 1;
    int was_down = 0;

    for (;;) {
        layout_build(vw, vh, &F, &L);

        if (dirty) {
            draw_ui(&c, &L, &F, &st, msg, g_msg_bad);
            mc_present(&c);
            dirty = 0;
        }

        int got = 0;
        MEvent ev;
        while (mc_poll(&c, &ev)) {
            got = 1;
            int quit = 0;

            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;

            case MC_RESIZE:
                vw = ev.w < cap_w ? ev.w : cap_w;
                vh = ev.h < cap_h ? ev.h : cap_h;
                if (vw < 1) vw = 1;
                if (vh < 1) vh = 1;
                dirty = 1;
                break;

            case MC_KEY:
                if (ev.press) {
                    st.shift = ev.mods & 1;
                    if (ev.key == 1) {                        /* KEY_ESC */
                        if (st.dialog || st.drag) {
                            st.dialog = 0;
                            st.drag = 0;
                            st.down = 0;
                            snprintf(msg, sizeof(msg), "cancelled");
                            dirty = 1;
                        }
                    } else if (ev.key == 28) {                /* KEY_ENTER */
                        if (st.dialog) do_clear(&L, &st, msg, sizeof(msg), &dirty);
                    }
                } else {
                    st.shift = ev.mods & 1;
                }
                break;

            case MC_TEXT:
                handle_text(&st, ev.text, msg, sizeof(msg), &L, &dirty, &quit);
                break;

            case MC_MOUSE: {
                int x = ev.x, y = ev.y;
                int down = ev.buttons & 1;
                st.cursor_in = rect_hit(&L.canvas, x, y);
                if (st.cursor_in) {
                    st.px = x - L.canvas.x;
                    st.py = y - L.canvas.y;
                }

                if (down && !was_down) {
                    press_at(&st, &L, x, y, msg, sizeof(msg), &dirty);
                } else if (down && was_down) {
                    if (st.slider >= 0) {
                        Rect r = L.sl[st.slider];
                        int v = (x - r.x) * 255 / (r.w - 1);
                        if (v < 0) v = 0;
                        if (v > 255) v = 255;
                        if (st.slider == 0) st.r = v;
                        else if (st.slider == 1) st.g = v;
                        else st.b = v;
                        dirty = 1;
                    } else if (st.drag) {
                        st.px = x - L.canvas.x;
                        st.py = y - L.canvas.y;
                        dirty = 1;
                    } else if (st.down) {
                        drag_at(&st, x - L.canvas.x, y - L.canvas.y, msg, sizeof(msg), &dirty);
                    }
                } else if (!down && was_down) {
                    release_at(&st, msg, sizeof(msg), &dirty);
                }
                was_down = down;
                dirty = 1;
                break;
            }

            case MC_WHEEL:
                if (st.brush + ev.wheel >= 1 && st.brush + ev.wheel <= MAX_BRUSH) {
                    st.brush += ev.wheel;
                    dirty = 1;
                }
                break;

            default:
                break;
            }

            if (quit) {
                hist_done();
                mc_close(&c);
                return 0;
            }
        }

        if (!got && !dirty) usleep(20 * 1000);
    }
}

#else /* PAINT_HOST_TEST */

/* ============================================================
 *  Host-only self test (never compiled into the OS app).
 *  It drives the real rasterisers, the real history and the real
 *  PNG writer/reader, then checks pixels.
 * ============================================================ */

static u64 canvas_sum(void) {
    u64 s = 0;
    for (int i = 0; i < CANVAS_PX; i++) s += canvas[i] * 2654435761u;
    return s;
}

static long count_colour(u32 c) {
    long n = 0;
    for (int i = 0; i < CANVAS_PX; i++) if (canvas[i] == c) n++;
    return n;
}

static int fails;

static void chk(int cond, const char *what) {
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

int paint_selftest(const char *path);

int paint_selftest(const char *path) {
    stamps_build();
    canvas_reset();
    if (hist_init() != 0) { printf("hist_init failed\n"); return 2; }

    printf("canvas %dx%d = %lu bytes; history %d states = %lu bytes\n",
           CANVAS_W, CANVAS_H, (unsigned long)CANVAS_BYTES,
           HIST_STATES, (unsigned long)(HIST_STATES * CANVAS_BYTES));

    /* ---- a line with the pencil brush ---- */
    u32 red = RGB(0xE8, 0x3A, 0x3A);
    Ink k;
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, red, 1);
    ink_line(&k, 10, 10, 90, 50);
    chk(k.hits > 0, "line: changed pixels");
    chk(canvas[(size_t)10 * CANVAS_W + 10] == red, "line: start pixel painted");
    chk(canvas[(size_t)30 * CANVAS_W + 50] == red, "line: x=50 is on the raster");
    chk(canvas[(size_t)30 * CANVAS_W + 80] != red, "line: off-line pixel untouched");
    hist_commit();

    /* ---- rectangle outline ---- */
    u32 blue = RGB(0x4F, 0x7A, 0xE8);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, blue, 1);
    ink_rect(&k, 100, 20, 180, 80, 0);
    chk(canvas[(size_t)20 * CANVAS_W + 100] == blue, "rect: top-left corner");
    chk(canvas[(size_t)80 * CANVAS_W + 180] == blue, "rect: bottom-right corner");
    chk(canvas[(size_t)50 * CANVAS_W + 140] != blue, "rect: interior empty");
    hist_commit();

    /* ---- filled ellipse ---- */
    u32 green = RGB(0x3F, 0xB0, 0x5A);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, green, 1);
    ink_ellipse(&k, 220, 20, 300, 90, 1);
    long green_n = count_colour(green);
    chk(green_n > 1000, "ellipse fill: a real area was painted");
    chk(canvas[(size_t)55 * CANVAS_W + 260] == green, "ellipse fill: centre painted");
    chk(canvas[(size_t)21 * CANVAS_W + 221] != green, "ellipse fill: bbox corner outside");
    hist_commit();
    u64 before_fill = canvas_sum();

    /* ---- scanline flood fill inside the ellipse ---- */
    u32 amber = RGB(0xF5, 0xC5, 0x42);
    char err[128];
    long got = flood_fill(260, 55, amber, err, sizeof(err));
    chk(got == green_n, "flood fill: changed exactly the ellipse pixels");
    chk(count_colour(amber) == green_n, "flood fill: amber count matches");
    chk(canvas[(size_t)21 * CANVAS_W + 221] != amber, "flood fill: did not leak outside");
    long spill = 0;
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < CANVAS_W; x++) if (canvas[(size_t)y * CANVAS_W + x] == amber) spill++;
    chk(spill == 0, "flood fill: nothing above the ellipse");
    hist_commit();
    u64 after_fill = canvas_sum();

    /* ---- undo / redo ---- */
    chk(hist_undo() == 1, "undo: one step available");
    chk(canvas_sum() == before_fill, "undo: canvas is byte-for-byte the old state");
    chk(hist_redo() == 1, "redo: one step available");
    chk(canvas_sum() == after_fill, "redo: canvas is byte-for-byte the new state");

    /* ---- history is bounded ---- */
    int avail = hist_at;
    int steps = 0;
    while (hist_undo()) steps++;
    printf("undo steps available after 4 commits: %d\n", avail);
    chk(steps == avail, "undo: walked every stored step back to the first state");
    chk(hist_n <= HIST_STATES, "history: never more than HIST_STATES bitmaps");
    chk(hist_bytes <= (size_t)HIST_STATES * CANVAS_BYTES, "history: byte budget honoured");
    printf("history bytes in use: %lu of %lu\n",
           (unsigned long)hist_bytes, (unsigned long)(HIST_STATES * CANVAS_BYTES));
    while (hist_redo()) { }
    for (int i = 0; i < 24; i++) {
        Ink ke;
        ink_init(&ke, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                 RGB((i * 37) & 0xFF, (i * 73) & 0xFF, (i * 109) & 0xFF), 1);
        ink_rect(&ke, 5 + i, 5 + i, 40 + i, 40 + i, 0);
        hist_commit();
    }
    printf("after 24 more commits: %d states, %lu bytes\n",
           hist_n, (unsigned long)hist_bytes);
    chk(hist_n == HIST_STATES, "history: ring filled to its cap");
    chk(hist_bytes == (size_t)HIST_STATES * CANVAS_BYTES, "history: bytes accounted exactly");
    printf("undo steps at the cap: %d (need at least 10)\n", hist_at);
    chk(hist_at >= 10, "history: at least 10 undo steps");

    /* ---- a few more strokes so the PNG has real structure ---- */
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, RGB(0xE8, 0xEC, 0xF6), 5);
    ink_line(&k, 20, 200, 380, 300);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, RGB(0xB9, 0x8B, 0xF7), 2);
    ink_ellipse(&k, 30, 120, 200, 260, 0);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H, RGB(0xF7, 0x8F, 0xB3), 9);
    ink_rect(&k, 300, 150, 390, 240, 1);
    hist_commit();

    /* ---- PNG write ---- */
    remove(path);   /* the host harness rewrites its own proof file */
    long bytes = png_write_file(path, canvas, CANVAS_W, CANVAS_H, err, sizeof(err));
    printf("png_write_file: %ld bytes (%s)\n", bytes, bytes > 0 ? "written" : err);
    chk(bytes > 0, "png: file written");

    FILE *f = fopen(path, "rb");
    u8 head[33];
    size_t hn = f ? fread(head, 1, sizeof(head), f) : 0;
    if (f) fclose(f);
    chk(hn == sizeof(head), "png: header readable");
    printf("header (%u bytes):", (unsigned)hn);
    for (size_t i = 0; i < hn; i++) printf(" %02X", head[i]);
    printf("\n");
    chk(head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G',
        "png: signature");
    chk(be32(head + 8) == 13 && !memcmp(head + 12, "IHDR", 4), "png: IHDR first");
    chk(be32(head + 16) == CANVAS_W && be32(head + 20) == CANVAS_H, "png: IHDR size");
    chk(head[24] == 8 && head[25] == 6, "png: 8-bit RGBA");
    printf("IHDR: %ux%u depth %u colour %u compression %u filter %u interlace %u\n",
           be32(head + 16), be32(head + 20), head[24], head[25], head[26], head[27], head[28]);

    /* ---- PNG read back, through the real decoder ---- */
    u32 *round = malloc(CANVAS_BYTES);
    if (!round) { printf("out of memory\n"); return 2; }
    int sw = 0, sh = 0;
    int rc = png_read_file(path, round, CANVAS_W, CANVAS_H, &sw, &sh, err, sizeof(err));
    printf("png_read_file: rc=%d source %dx%d (%s)\n", rc, sw, sh, rc ? err : "ok");
    chk(rc == 0 && sw == CANVAS_W && sh == CANVAS_H, "png: read back");
    long diff = 0;
    int first = -1;
    for (int i = 0; i < CANVAS_PX; i++) {
        if (round[i] != canvas[i]) {
            if (first < 0) first = i;
            diff++;
        }
    }
    printf("round trip: %ld differing pixels of %d\n", diff, CANVAS_PX);
    if (first >= 0)
        printf("first difference at %d,%d: wrote %08lX read %08lX\n",
               first % CANVAS_W, first / CANVAS_W,
               (unsigned long)canvas[first], (unsigned long)round[first]);
    chk(diff == 0, "png: every pixel survived the round trip");

    /* ---- a corrupt file must be refused, not drawn ---- */
    {
        FILE *g = fopen(path, "r+b");
        if (g) {
            fseek(g, 40, SEEK_SET);
            int c = fgetc(g);
            fseek(g, 40, SEEK_SET);
            fputc(c ^ 0x5A, g);
            fclose(g);
        }
            int rc2 = png_read_file(path, round, CANVAS_W, CANVAS_H, &sw, &sh, err, sizeof(err));
        printf("tampered byte: rc=%d reason=%s\n", rc2, err);
        chk(rc2 != 0, "png: a corrupt chunk is refused with a reason");
        /* put it back so the proof file stays valid */
        if (bytes > 0) {
            FILE *h = fopen(path, "r+b");
            if (h) {
                fseek(h, 40, SEEK_SET);
                int c = fgetc(h);
                fseek(h, 40, SEEK_SET);
                fputc(c ^ 0x5A, h);
                fclose(h);
            }
        }
    }
    free(round);

    /* ---- the app's own save/open path, if the harness moved it ---- */
    if (strcmp(PIC_DIR, "/root/Pictures") != 0) {
        char m[220];
        u64 before = canvas_sum();
        act_save(m, sizeof(m));
        printf("act_save: %s\n", m);
        chk(strstr(m, "saved ") != NULL && strstr(m, ".png") != NULL,
            "app save: wrote paint-NNNN.png");
        {
            char *at = strstr(m, "(");
            long sz = at ? strtol(at + 1, NULL, 10) : -1;
            printf("act_save: reported size %ld bytes\n", sz);
            chk(sz > 1000, "app save: the file on disk is the whole PNG");
        }
        canvas_reset();
        chk(canvas_sum() != before, "app open: canvas was cleared first");
        act_open(m, sizeof(m));
        printf("act_open: %s\n", m);
        chk(strstr(m, "opened ") != NULL, "app open: newest PNG loaded");
        chk(canvas_sum() == before, "app open: every pixel came back");
    } else {
        printf("app save/open path skipped: PIC_DIR is the real /root\n");
    }

    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

/* Render the whole window (real fonts, real drawing code) to a BMP so the
 * layout can be looked at. Host-only. */
static void render_bmp(const char *path, const u32 *px, int w, int h) {
    int row = (w * 3 + 3) & ~3;
    size_t size = 54u + (size_t)row * (size_t)h;
    u8 *b = calloc(1, size);
    if (!b) return;
    b[0] = 'B'; b[1] = 'M';
    b[2] = (u8)size; b[3] = (u8)(size >> 8); b[4] = (u8)(size >> 16); b[5] = (u8)(size >> 24);
    b[10] = 54;
    b[14] = 40;
    b[18] = (u8)w; b[19] = (u8)(w >> 8); b[20] = (u8)(w >> 16); b[21] = (u8)(w >> 24);
    b[22] = (u8)h; b[23] = (u8)(h >> 8); b[24] = (u8)(h >> 16); b[25] = (u8)(h >> 24);
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < h; y++) {
        u8 *dst = b + 54u + (size_t)(h - 1 - y) * (size_t)row;
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * (size_t)w + (size_t)x];
            dst[x * 3 + 0] = GET_B(c);
            dst[x * 3 + 1] = GET_G(c);
            dst[x * 3 + 2] = GET_R(c);
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(b, 1, size, f);
        fclose(f);
    }
    free(b);
}

int paint_render(float scale, const char *drag_out, const char *dialog_out);

int paint_render(float scale, const char *drag_out, const char *dialog_out) {
    ui_scale_set_effective(scale);
    if (font_init("assets/fonts") != 0) { printf("fonts: font_init failed\n"); return 2; }
    Fonts F;
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_REGULAR, 10);
    F.mono  = font_get(FONT_MONO, 10);
    F.big   = font_get(FONT_SEMIBOLD, 14);
    if (!F.body || !F.small || !F.mono || !F.big) { printf("fonts: missing faces\n"); return 2; }
    printf("render scale %.2f: body %.2f high, small %.2f, mono %.2f wide for '#'\n",
           (double)scale, (double)font_height(F.body), (double)font_height(F.small),
           (double)font_width(F.mono, "#"));

    int vw = WIN_W, vh = WIN_H;
    u32 *px = calloc((size_t)vw * (size_t)vh, sizeof(u32));
    if (!px) return 2;
    MClient c;
    memset(&c, 0, sizeof(c));
    c.px = px;
    c.w = vw;
    c.h = vh;
    c.stride = vw;

    /* the render must not touch /root; pretend the probe already failed */
    g_dir_tried = 1;
    g_dir[0] = 0;

    Layout L;
    layout_build(vw, vh, &F, &L);

    PaintState st;
    memset(&st, 0, sizeof(st));
    st.tool = T_ELLIPSE;
    st.brush = 6;
    st.r = 0xF5; st.g = 0xC5; st.b = 0x42;
    st.drag = 1;
    st.sx = 40; st.sy = 60;
    st.px = 330; st.py = 250;
    st.cursor_in = 1;
    st.slider = -1;
    draw_ui(&c, &L, &F, &st, "dragging an ellipse - release to commit", 0);
    render_bmp(drag_out, px, vw, vh);
    printf("render          %s (shape drag in progress)\n", drag_out);

    st.drag = 0;
    st.dialog = 1;
    st.tool = T_RECTF;
    st.r = 0x7B; st.g = 0xD8; st.b = 0x8F;
    draw_ui(&c, &L, &F, &st, "saved /root/Pictures/paint-0001.png (512423 bytes)", 0);
    render_bmp(dialog_out, px, vw, vh);
    printf("render          %s (clear confirmation)\n", dialog_out);

    free(px);
    return 0;
}

/* Decode two PNGs and compare them pixel for pixel. Used by the harness
 * to feed the reader streams that zlib produced with fixed Huffman,
 * dynamic Huffman and every row filter - i.e. files this writer would
 * never make itself. */
int paint_decode_compare(const char *ref, const char *variant);

int paint_decode_compare(const char *ref, const char *variant) {
    u32 *a = malloc(CANVAS_BYTES);
    u32 *b = malloc(CANVAS_BYTES);
    if (!a || !b) { printf("out of memory\n"); free(a); free(b); return 2; }
    char err[160];
    int aw = 0, ah = 0, bw = 0, bh = 0;
    int ra = png_read_file(ref, a, CANVAS_W, CANVAS_H, &aw, &ah, err, sizeof(err));
    if (ra != 0) { printf("reference %s: %s\n", ref, err); free(a); free(b); return 2; }
    int rb = png_read_file(variant, b, CANVAS_W, CANVAS_H, &bw, &bh, err, sizeof(err));
    if (rb != 0) { printf("%s: REFUSED: %s\n", variant, err); free(a); free(b); return 1; }
    long diff = 0;
    for (int i = 0; i < CANVAS_PX; i++) if (a[i] != b[i]) diff++;
    printf("%-34s %dx%d  %ld differing pixels of %d  %s\n",
           variant, bw, bh, diff, CANVAS_PX, diff == 0 ? "ok" : "FAIL");
    free(a);
    free(b);
    return diff == 0 ? 0 : 1;
}

#endif /* PAINT_HOST_TEST */
