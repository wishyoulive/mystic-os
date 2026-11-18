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
 *                steps (13), and every byte is counted: the ring can
 *                never exceed HIST_STATES * CANVAS_BYTES. If malloc
 *                fails, history is disabled and the footer says so
 *                instead of the app dying.
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
 *  The window is one column: a header (the tool, the brush stepper), a
 *  strip of drawn icons (the eight tools, then undo, redo, save, open,
 *  clear), the sheet, one row of inks under it, and one footer line (the
 *  picture folder, the undo depth, the brush). The mixer is a card beside
 *  the sheet when the window is wide enough and a popover on it when it is
 *  not. There are no hint lines: what the program has to say about the
 *  last action appears as a toast and leaves on its own.
 *
 *  Nothing is invented: the only numbers on screen are the mixer channels
 *  you set, the brush size, the undo depth, and the pixel count a fill
 *  really changed.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#ifndef PAINT_HOST_TEST
#  include <poll.h>              /* the event loop waits here; host tests do not */
#endif
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
static const u32 C_HOVER  = RGB(0x22, 0x2A, 0x38);
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

/* ------------------------------------------------------------ tools */

enum { T_PENCIL, T_LINE, T_RECT, T_RECTF, T_ELLIPSE, T_ELLIPSEF, T_FILL, T_PICK, T_N };
static const char *TOOL_NAME[T_N] = {
    "Pencil", "Line", "Rectangle", "Rect filled",
    "Ellipse", "Ellipse filled", "Fill", "Eyedropper"
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
        int general = type[0] >= 'A' && type[0] <= 'Z';
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
        } else if (general) {
            snprintf(err, esz, "general chunk %.4s is not supported", (const char *)type);
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
 *
 *  One spacing scale, one type scale, one rhythm - the same block the
 *  desktop's other apps use, so Paint does not look improvised.
 *
 *  SPACING.  One unit of 8 px scaled by ui_px(); every padding and every
 *            gap below is SP(n), and there is no other kind of space.
 *  TYPE.     Exactly three sizes and the same three faces as the rest of
 *            the desktop: semibold 15 is the title, regular 11 is body
 *            text, medium 9 is small print. No other font_get() call.
 *  HEADER.   One line: the active tool in the title face on the left, a
 *            brush stepper on the right.
 *  STRIP.    One row of drawn icons - the eight tools, then undo, redo,
 *            save, open, clear. It wraps to two rows rather than shrink
 *            the icons below what the text scale can carry.
 *  CANVAS.   All the room that is left, at 1:1 when it fits and scaled to
 *            fit (never cropped) when the window is smaller.
 *  PALETTE.  One row: the current colour, then the twelve inks.
 *  MIXER.    A right-hand panel when the window is wide enough to hold it
 *            beside the canvas, otherwise a popover over the canvas.
 *  FOOTER.   One line: the picture folder, the undo depth, the brush size.
 *  TEXT.     Every string is measured; anything wider than its box is cut
 *            with an ellipsis. text_draw() is the only way this file draws
 *            text, so a label can never run into its neighbour.
 * ============================================================ */

#define UI_UNIT 8
static int SP(int n) { return ui_px(n * UI_UNIT); }

typedef struct { int x, y, w, h; } Rect;

/* The five non-tool buttons, in the order they appear after the tools. */
enum { A_UNDO, A_REDO, A_SAVE, A_OPEN, A_CLEAR, A_N };

/* Icon kinds. The eight tools keep their T_* value; the actions follow. */
enum { IC_UNDO = T_N, IC_REDO, IC_SAVE, IC_OPEN, IC_TRASH, IC_N };

/* What is under the pointer: one code for hit-testing, hover and press. */
enum {
    H_NONE = 0,
    H_TOOL = 10,
    H_ACT  = 20,
    H_SW   = 40,
    H_CHIP = 60,
    H_SL   = 70,
    H_BM   = 80,
    H_BP   = 81,
    H_YES  = 90,
    H_NO   = 91,
    H_POP  = 95
};

typedef struct {
    int vw, vh, pad;

    /* header: tool name left, brush stepper right */
    int header_y, header_h;
    int title_x, title_w;
    int brush_label_x, brush_label_w, show_brush_label;
    int brush_num_x, brush_num_w;
    Rect brush_minus, brush_plus;

    /* the tool strip */
    int strip_y, strip_h, strip_rows, btn, divider_x;
    Rect tool[T_N];
    Rect act[A_N];

    /* the sheet */
    Rect canvas;

    /* the palette row: the current colour, then the twelve inks */
    int palette_y, palette_h, sw;
    Rect chip;
    Rect swatch[PAL_N];

    /* the mixer, wherever it lives: panel or popover */
    int mix_x, mix_w, mix_h, mix_hex_y;
    int mix_lab_y[3];
    Rect mix_bar;
    Rect sl[3];
    int panel_on;
    Rect panel;
    int pop_on;
    Rect pop;

    /* the clear confirmation */
    Rect dlg, dlg_yes, dlg_no;
    int dlg_title_y, dlg_body_y, dlg_btn_y, dlg_btn_h;

    /* the footer */
    int footer_y, footer_h, footer_left_w;
    int footer_right_x, footer_undo_w, footer_brush_x;
} Layout;

typedef struct {
    Font *title, *body, *small;
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
    int hot;         /* H_* under the pointer, for the hover tint */
    int mixer;       /* the narrow-window mixer popover is open */
} PaintState;

static Rect rect_make(int x, int y, int w, int h) { Rect r = { x, y, w, h }; return r; }

static int rect_hit(const Rect *r, int x, int y) {
    return r->w > 0 && r->h > 0 &&
           x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static inline int ui_min(int a, int b) { return a < b ? a : b; }
static inline int ui_max(int a, int b) { return a > b ? a : b; }
static inline int ui_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static int ui_ceil(float v) {
    int i = (int)v;
    return v > (float)i ? i + 1 : i;
}

/* The ink of one line is font_height()/1.4 tall (src/font.c); centring
 * that box, not the line box with its leading, is what puts caps and
 * descenders equally inside a band. */
static int ui_top(Font *f, int band) {
    int ink;
    if (!f || band <= 0) return 0;
    ink = (int)(font_height(f) / 1.4f + 0.5f);
    if (ink >= band) return 0;
    return (band - ink) / 2;
}

/* ------------------------------------------------------------
 *  Text: measured, truncated with an ellipsis, never overflowing
 * ------------------------------------------------------------ */

/* Copy src into dst and make it fit boxw. Whole UTF-8 characters are
 * dropped from the end until the string plus an ellipsis fits; if not even
 * the ellipsis fits the box, the result is empty rather than a smear.
 * Returns 1 when the string was shortened. */
static int text_fit(char *dst, size_t cap, Font *f, const char *src, int boxw) {
    static const char ELL[] = "\xE2\x80\xA6";
    size_t n;
    float ew;

    if (!dst || cap == 0) return 0;
    if (!f || !src) { dst[0] = 0; return 0; }
    if (src != dst) {
        n = 0;
        while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
        dst[n] = 0;
    } else {
        n = strlen(dst);
    }
    if (boxw < 1) { dst[0] = 0; return 1; }
    if (font_width(f, dst) <= (float)boxw) return 0;

    ew = font_width(f, ELL);
    while (n > 0) {
        n--;
        while (n > 0 && ((u8)dst[n] & 0xC0) == 0x80) n--;
        dst[n] = 0;
        if (font_width(f, dst) + ew <= (float)boxw) break;
    }
    if (n == 0) {
        if (ew <= (float)boxw && cap >= 4) memcpy(dst, ELL, 4);
        else dst[0] = 0;
        return 1;
    }
    if (n + 4 > cap) { dst[0] = 0; return 1; }
    memcpy(dst + n, ELL, 4);
    return 1;
}

/* Host proof counters: how many strings were drawn, how many still did
 * not fit after truncation (which must stay zero). */
static int g_text_drawn, g_text_over;

static void text_draw(Surface *s, Font *f, int x, int y, int boxw, int align,
                      const char *t, u32 c) {
    char buf[256];
    float w, dx;

    if (!s || !f || !t || !t[0] || boxw <= 0) return;
    text_fit(buf, sizeof(buf), f, t, boxw);
    if (!buf[0]) return;
    w = font_width(f, buf);
    g_text_drawn++;
    if (w > (float)boxw + 0.5f) {
        g_text_over++;
#ifdef PAINT_HOST_TEST
        printf("TEXT OVERFLOW: box %d px, string %.1f px: \"%s\"\n",
               boxw, (double)w, buf);
#endif
    }
    dx = (float)x;
    if (align == 1) dx = (float)(x + boxw) - w;
    else if (align == 2) dx = (float)x + ((float)boxw - w) * 0.5f;
    font_draw(s, f, dx, (float)y, buf, c);
}

/* ------------------------------------------------------------
 *  Icons - drawn, not typed, on a 16x16 unit grid
 * ------------------------------------------------------------ */

static void ic_line(Surface *s, float ox, float oy, float k,
                    float x0, float y0, float x1, float y1, float wm, u32 c) {
    float w = wm * k;
    if (w < 1.0f) w = 1.0f;
    gfx_line(s, ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, w, c);
}

static void ic_poly(Surface *s, float ox, float oy, float k,
                    const float *pts, int n, u32 c) {
    float p[128];
    if (n < 3 || n > 64) return;
    for (int i = 0; i < n * 2; i += 2) {
        p[i] = ox + pts[i] * k;
        p[i + 1] = oy + pts[i + 1] * k;
    }
    gfx_polygon(s, p, n, c);
}

static void ic_oval(Surface *s, float ox, float oy, float k, float cx, float cy,
                    float rx, float ry, u32 c, int fill, float wm) {
    enum { N = 36 };
    float pts[N * 2];
    for (int i = 0; i < N; i++) {
        float a = (float)i * 6.2831853f / (float)N;
        pts[i * 2] = cx + cosf(a) * rx;
        pts[i * 2 + 1] = cy + sinf(a) * ry;
    }
    if (fill) {
        float p[N * 2];
        for (int i = 0; i < N * 2; i += 2) {
            p[i] = ox + pts[i] * k;
            p[i + 1] = oy + pts[i + 1] * k;
        }
        gfx_polygon(s, p, N, c);
        return;
    }
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        ic_line(s, ox, oy, k, pts[i * 2], pts[i * 2 + 1],
                pts[j * 2], pts[j * 2 + 1], wm, c);
    }
}

/* A clockwise arc in icon units; angles in radians, 0 = right. */
static void ic_arc(Surface *s, float ox, float oy, float k, float cx, float cy,
                   float rx, float ry, float a0, float a1, float wm, u32 c) {
    enum { N = 22 };
    float px = cx + cosf(a0) * rx, py = cy - sinf(a0) * ry;
    for (int i = 1; i <= N; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)N;
        float nx = cx + cosf(a) * rx, ny = cy - sinf(a) * ry;
        ic_line(s, ox, oy, k, px, py, nx, ny, wm, c);
        px = nx;
        py = ny;
    }
}

static void icon_draw(Surface *s, int kind, const Rect *r, u32 c) {
    float k = (float)r->w / 16.0f;
    float ox = (float)r->x, oy = (float)r->y;
    float w1 = 1.35f, w2 = 2.5f;

    if (r->w <= 0 || r->h <= 0) return;
    if (r->w < 12) { w1 = 1.7f; w2 = 2.9f; }   /* keep hairlines visible */

    switch (kind) {
    case T_PENCIL:
        ic_line(s, ox, oy, k, 3.2f, 12.8f, 10.8f, 5.2f, w2, c);
        ic_line(s, ox, oy, k, 10.2f, 4.6f, 11.8f, 3.0f, w2 * 1.15f, c);
        {
            float t[6] = { 1.7f, 14.3f, 4.1f, 13.5f, 2.6f, 12.0f };
            ic_poly(s, ox, oy, k, t, 3, c);
        }
        break;
    case T_LINE:
        ic_line(s, ox, oy, k, 3.0f, 13.0f, 13.0f, 3.0f, w1, c);
        gfx_rrect_aa(s, ox + 2.0f * k, oy + 12.0f * k, 2.0f * k, 2.0f * k, 0, c, 0, 0);
        gfx_rrect_aa(s, ox + 12.0f * k, oy + 2.0f * k, 2.0f * k, 2.0f * k, 0, c, 0, 0);
        break;
    case T_RECT:
        gfx_rrect_aa(s, ox + 2.2f * k, oy + 3.4f * k, 11.6f * k, 9.2f * k, 0,
                     0, c, w1 * k);
        break;
    case T_RECTF:
        gfx_rrect_aa(s, ox + 2.2f * k, oy + 3.4f * k, 11.6f * k, 9.2f * k, 0,
                     c, 0, 0);
        break;
    case T_ELLIPSE:
        ic_oval(s, ox, oy, k, 8.0f, 8.0f, 5.9f, 4.4f, c, 0, w1);
        break;
    case T_ELLIPSEF:
        ic_oval(s, ox, oy, k, 8.0f, 8.0f, 5.9f, 4.4f, c, 1, 0);
        break;
    case T_FILL:
        /* the bucket */
        ic_line(s, ox, oy, k, 4.4f, 6.0f, 10.8f, 6.0f, w1, c);
        ic_line(s, ox, oy, k, 4.4f, 6.0f, 5.7f, 12.4f, w1, c);
        ic_line(s, ox, oy, k, 10.8f, 6.0f, 9.5f, 12.4f, w1, c);
        ic_line(s, ox, oy, k, 5.7f, 12.4f, 9.5f, 12.4f, w1, c);
        ic_line(s, ox, oy, k, 5.2f, 6.0f, 6.4f, 3.2f, w1, c);
        ic_line(s, ox, oy, k, 6.4f, 3.2f, 9.4f, 3.2f, w1, c);
        ic_line(s, ox, oy, k, 9.4f, 3.2f, 10.2f, 6.0f, w1, c);
        /* and the drop it pours */
        ic_oval(s, ox, oy, k, 12.6f, 11.2f, 1.6f, 2.1f, c, 1, 0);
        break;
    case T_PICK:
        ic_line(s, ox, oy, k, 11.4f, 4.6f, 6.0f, 10.0f, w2, c);
        ic_line(s, ox, oy, k, 6.0f, 10.0f, 3.4f, 12.6f, 1.0f, c);
        ic_oval(s, ox, oy, k, 12.2f, 3.8f, 2.1f, 2.1f, c, 1, 0);
        break;
    case IC_UNDO:
        ic_arc(s, ox, oy, k, 8.6f, 10.6f, 4.0f, 4.2f, 0.0f, 3.14159f, w1, c);
        {
            float t[6] = { 4.0f, 10.6f, 7.2f, 8.2f, 7.0f, 12.9f };
            ic_poly(s, ox, oy, k, t, 3, c);
        }
        break;
    case IC_REDO:
        ic_arc(s, ox, oy, k, 7.4f, 10.6f, 4.0f, 4.2f, 3.14159f, 0.0f, w1, c);
        {
            float t[6] = { 12.0f, 10.6f, 8.8f, 8.2f, 9.0f, 12.9f };
            ic_poly(s, ox, oy, k, t, 3, c);
        }
        break;
    case IC_SAVE:
        gfx_rrect_aa(s, ox + 2.6f * k, oy + 2.4f * k, 10.8f * k, 11.2f * k,
                     1.0f * k, 0, c, w1 * k);
        gfx_rrect_aa(s, ox + 5.6f * k, oy + 2.4f * k, 4.8f * k, 3.2f * k, 0, c, 0, 0);
        gfx_rrect_aa(s, ox + 5.2f * k, oy + 9.2f * k, 5.6f * k, 4.4f * k, 0, c, 0, 0);
        break;
    case IC_OPEN: {
        float t[16] = {
            2.4f, 12.6f, 2.4f, 4.6f, 6.4f, 4.6f, 7.6f, 3.2f,
            11.6f, 3.2f, 12.8f, 4.8f, 13.6f, 4.8f, 13.6f, 12.6f
        };
        ic_poly(s, ox, oy, k, t, 8, c);
        break;
    }
    case IC_TRASH:
        ic_line(s, ox, oy, k, 3.4f, 4.6f, 12.6f, 4.6f, w1 * 1.3f, c);
        ic_line(s, ox, oy, k, 6.4f, 4.6f, 6.4f, 2.8f, w1, c);
        ic_line(s, ox, oy, k, 6.4f, 2.8f, 9.6f, 2.8f, w1, c);
        ic_line(s, ox, oy, k, 9.6f, 2.8f, 9.6f, 4.6f, w1, c);
        ic_line(s, ox, oy, k, 4.8f, 4.6f, 5.7f, 13.4f, w1, c);
        ic_line(s, ox, oy, k, 11.2f, 4.6f, 10.3f, 13.4f, w1, c);
        ic_line(s, ox, oy, k, 5.7f, 13.4f, 10.3f, 13.4f, w1, c);
        ic_line(s, ox, oy, k, 7.3f, 6.8f, 7.5f, 11.6f, 1.0f, c);
        ic_line(s, ox, oy, k, 8.7f, 6.8f, 8.5f, 11.6f, 1.0f, c);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------
 *  The one button: an icon chip. Hover and selection are the same
 *  rectangle with a different fill, so nothing moves.
 * ------------------------------------------------------------ */

static void strip_button(Surface *s, const PaintState *st, const Rect *r, int icon,
                         int active, int code, int enabled) {
    int hot = st->hot == code;
    int isz;
    Rect ib;
    u32 fill, bord, ink;

    if (r->w <= 0 || r->h <= 0) return;
    fill = active ? color_alpha(C_ACCENT, 0.16f) : hot ? C_HOVER : 0;
    bord = active ? color_alpha(C_ACCENT, 0.85f) : 0;
    if (fill || bord)
        gfx_rrect_aa(s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                     (float)ui_px(4), fill, bord, 1.0f);

    isz = r->w * 11 / 20;
    if (isz > r->h - 2) isz = r->h - 2;
    if (isz < 4) return;
    ib = rect_make(r->x + (r->w - isz) / 2, r->y + (r->h - isz) / 2, isz, isz);
    ink = !enabled ? color_alpha(C_DIM, 0.35f)
        : active ? C_ACCENT
        : hot ? C_TEXT
        : color_alpha(C_TEXT, 0.72f);
    icon_draw(s, icon, &ib, ink);
}

/* The brush stepper: two small squares and the value between them. */
static void stepper_draw(Surface *s, const Rect *r, int plus, int hot, int enabled) {
    float cx, cy, arm;
    u32 ink = enabled ? (hot ? C_TEXT : C_DIM) : color_alpha(C_DIM, 0.35f);

    if (r->w <= 0 || r->h <= 0) return;
    gfx_rrect_aa(s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                 (float)ui_px(4), hot ? C_HOVER : C_PANEL,
                 enabled ? C_LINE : color_alpha(C_LINE, 0.5f), 1.0f);
    cx = (float)r->x + (float)r->w * 0.5f;
    cy = (float)r->y + (float)r->h * 0.5f;
    arm = (float)r->w * 0.22f;
    if (arm < 2.0f) arm = 2.0f;
    gfx_line(s, cx - arm, cy, cx + arm, cy, 1.4f, ink);
    if (plus) gfx_line(s, cx, cy - arm, cx, cy + arm, 1.4f, ink);
}

static void slider_draw(Surface *s, const Rect *r, int v, u32 col, int hot) {
    float kx;
    int kh, fillw;

    if (r->w <= 0 || r->h <= 0) return;
    kh = r->h + 2 * ui_px(2);
    gfx_rrect_aa(s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                 (float)r->h * 0.5f, color_alpha(C_TEXT, 0.09f), 0, 0);
    fillw = (r->w * v) / 255;
    if (fillw > 1)
        gfx_rrect_aa(s, (float)r->x, (float)r->y, (float)fillw, (float)r->h,
                     (float)r->h * 0.5f, color_alpha(col, 0.55f), 0, 0);
    kx = (float)r->x + (float)(r->w - 1) * (float)v / 255.0f;
    gfx_rrect_aa(s, kx - (float)kh * 0.5f, (float)r->y - ui_px(2),
                 (float)kh, (float)kh, (float)kh * 0.5f, C_TEXT,
                 hot ? C_ACCENT : C_LINE, 1.0f);
}

/* ------------------------------------------------------------
 *  Layout - the single source of every number in this window
 * ------------------------------------------------------------ */

/* Place the mixer's rows inside (x,y,w). with_bar draws the current
 * colour as a wide chip at the top of the panel; the popover has the
 * palette row's chip right underneath it instead. */
static void mixer_build(Layout *L, const Fonts *F, int x, int y, int w, int with_bar) {
    int sh = ui_ceil(font_height(F->small));
    int gap = SP(1);
    int sl_h = ui_max(ui_px(4), gap);
    int yy = y;

    L->mix_x = x;
    L->mix_w = w;
    if (with_bar) {
        L->mix_bar = rect_make(x, y, w, SP(4));
        yy += SP(4) + gap;
    } else {
        L->mix_bar = rect_make(0, 0, 0, 0);
    }
    L->mix_hex_y = yy;
    yy += sh + gap;
    for (int i = 0; i < 3; i++) {
        L->mix_lab_y[i] = yy;
        L->sl[i] = rect_make(x, yy + sh, w, sl_h);
        yy += sh + sl_h + SP(2);
    }
    L->mix_h = yy - SP(2) - y;
}

static void layout_build(int vw, int vh, const Fonts *F, const PaintState *st, Layout *L) {
    int th, sh, pad, gap, over, availw, btn, need, cells, sw, area_top, area_bot;
    int area_x, area_w, area_h, rows_h, row_adv, sl_h, mix_need, panel_w;
    int cvw, cvh, block_w, block_h, block_x, canvas_y, row_x, row_w, panel_on;
    int dw, dh, bx, step, num_w, label_w, title_right;

    memset(L, 0, sizeof(*L));
    L->vw = vw > 0 ? vw : 1;
    L->vh = vh > 0 ? vh : 1;
    L->pad = SP(1);
    pad = L->pad;
    gap = SP(1);
    over = SP(2);

    th = ui_ceil(font_height(F->title));
    sh = ui_ceil(font_height(F->small));

    /* ---- header ---- */
    L->header_y = pad;
    L->header_h = th;

    step = ui_max(SP(2), sh + SP(1));
    num_w = ui_max(SP(2), (int)font_width(F->body, "00") + gap);
    label_w = (int)font_width(F->small, "Brush") + gap;
    bx = L->vw - pad;
    L->brush_plus = rect_make(bx - step, L->header_y + (th - step) / 2, step, step);
    bx = L->brush_plus.x - gap;
    L->brush_num_x = bx - num_w;
    L->brush_num_w = num_w;
    bx = L->brush_num_x;
    L->brush_minus = rect_make(bx - step, L->header_y + (th - step) / 2, step, step);
    bx = L->brush_minus.x - gap;
    L->show_brush_label = 1;
    L->brush_label_x = bx - label_w;
    L->brush_label_w = label_w;
    title_right = L->brush_label_x;
    if (title_right - pad < SP(9)) {
        L->show_brush_label = 0;
        L->brush_label_w = 0;
        L->brush_label_x = L->brush_minus.x;
        title_right = L->brush_minus.x;
    }
    L->title_x = pad;
    L->title_w = ui_max(1, title_right - gap - pad);

    /* ---- the tool strip; it wraps instead of shrinking past legibility ---- */
    availw = L->vw - 2 * pad;
    btn = (int)(font_height(F->small) * 2.0f + 0.5f);
    if (btn < SP(2)) btn = SP(2);
    if (btn > SP(4)) btn = SP(4);
    need = (T_N + A_N) * btn + (T_N + A_N - 1) * gap + 2 * over - gap;
    L->strip_rows = (need <= availw) ? 1 : 2;
    if (L->strip_rows == 2) {
        int fit_t = (availw - (T_N - 1) * gap) / T_N;
        int fit_a = (availw - (A_N - 1) * gap) / A_N;
        int fit = ui_min(fit_t, fit_a);
        if (btn > fit) btn = fit;
        if (btn < 12) btn = 12;
    }
    L->btn = btn;
    L->strip_y = L->header_y + L->header_h + pad;
    L->strip_h = L->strip_rows * btn + (L->strip_rows - 1) * gap;
    L->divider_x = -1;
    if (L->strip_rows == 1) {
        int x = pad;
        for (int i = 0; i < T_N; i++) {
            L->tool[i] = rect_make(x, L->strip_y, btn, btn);
            x += btn + gap;
        }
        L->divider_x = x - gap + over;      /* the middle of the wide gap */
        x += 2 * over - gap;
        for (int i = 0; i < A_N; i++) {
            L->act[i] = rect_make(x, L->strip_y, btn, btn);
            x += btn + gap;
        }
    } else {
        for (int i = 0; i < T_N; i++)
            L->tool[i] = rect_make(pad + i * (btn + gap), L->strip_y, btn, btn);
        for (int i = 0; i < A_N; i++)
            L->act[i] = rect_make(pad + i * (btn + gap), L->strip_y + btn + gap, btn, btn);
    }

    /* ---- the footer ---- */
    L->footer_h = sh;
    L->footer_y = L->vh - pad - sh;
    if (L->footer_y < 0) L->footer_y = 0;
    L->footer_undo_w = ui_ceil(font_width(F->small, "undo 00/00"));
    L->footer_brush_x = L->vw - pad - ui_ceil(font_width(F->small, "brush 00"));
    L->footer_right_x = L->footer_brush_x - SP(2) - L->footer_undo_w;
    L->footer_left_w = L->footer_right_x - SP(2) - pad;
    if (L->footer_left_w < SP(4)) L->footer_left_w = SP(4);

    /* ---- the palette, the canvas and the mixer ----
     * They are one block: the sheet, the row of inks under it, and the
     * mixer beside it when the window can hold all three. The block is
     * centred in the area between the strip and the footer, so a large
     * window shows a centred artboard rather than a sheet pinned to the
     * top with its palette stranded at the bottom. */
    area_top = L->strip_y + L->strip_h + pad;
    area_bot = L->footer_y - pad;
    if (area_bot <= area_top) area_bot = area_top + 1;
    area_x = pad;
    area_w = availw;
    area_h = area_bot - area_top;

    sl_h = ui_max(ui_px(4), gap);
    row_adv = sh + sl_h + SP(2);
    rows_h = 3 * row_adv - SP(2);
    mix_need = SP(4) + gap + sh + gap + rows_h;   /* bar, hex, three rows */
    panel_w = ui_max(SP(18), (int)font_width(F->small, "#FFFFFF") + 2 * gap);

    cells = PAL_N + 1;
    sw = (area_w - (cells - 1) * gap) / cells;
    if (sw > SP(3)) sw = SP(3);
    if (sw < 3) sw = 3;
    L->palette_h = sw;

    panel_on = (area_w - panel_w - 2 * gap >= SP(30) && area_h - gap - sw >= SP(20));
    for (;;) {
        cvw = area_w - (panel_on ? panel_w + 2 * gap : 0);
        cvh = area_h - gap - L->palette_h;
        if (cvw < 1) cvw = 1;
        if (cvh < 1) cvh = 1;
        dw = cvw;
        dh = (int)((long)dw * CANVAS_H / CANVAS_W);
        if (dw > CANVAS_W) { dw = CANVAS_W; dh = CANVAS_H; }
        if (dh > cvh) { dh = cvh; dw = (int)((long)dh * CANVAS_W / CANVAS_H); }
        if (dw > cvw) { dw = cvw; dh = (int)((long)dw * CANVAS_H / CANVAS_W); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (!panel_on || dh >= 2 * pad + mix_need) break;
        panel_on = 0;                    /* the mixer's rows do not fit */
    }
    L->panel_on = panel_on;

    block_w = dw + (panel_on ? 2 * gap + panel_w : 0);
    block_h = dh + gap + L->palette_h;
    if (block_w > area_w) block_w = area_w;
    block_x = area_x + (area_w - block_w) / 2;
    canvas_y = area_top + (area_h - block_h) / 2;
    if (canvas_y < area_top) canvas_y = area_top;
    L->canvas = rect_make(block_x, canvas_y, dw, dh);

    /* the row of inks sits under the sheet: on the sheet's width when the
     * mixer is beside it, on the window's width when it is not */
    L->palette_y = canvas_y + dh + gap;
    row_x = panel_on ? L->canvas.x : area_x;
    row_w = panel_on ? L->canvas.w : area_w;
    sw = (row_w - (cells - 1) * gap) / cells;
    if (sw > SP(3)) sw = SP(3);
    if (sw < 3) sw = 3;
    L->sw = sw;
    L->palette_h = sw;
    L->chip = rect_make(row_x, L->palette_y, sw, sw);
    for (int i = 0; i < PAL_N; i++)
        L->swatch[i] = rect_make(row_x + (i + 1) * (sw + gap), L->palette_y, sw, sw);

    if (panel_on) {
        int ph = ui_min(dh, 2 * pad + mix_need);   /* the card's own height */
        L->panel = rect_make(L->canvas.x + dw + 2 * gap, canvas_y, panel_w, ph);
        mixer_build(L, F, L->panel.x + pad, L->panel.y + pad,
                    L->panel.w - 2 * pad, 1);
    } else {
        int pw = ui_min(SP(28), L->vw - 2 * pad);
        int ph = 2 * pad + sh + gap + rows_h;
        int px, py;
        if (pw < SP(10)) pw = SP(10);
        if (ph > L->vh - 2 * pad) ph = L->vh - 2 * pad;
        if (ph < 1) ph = 1;
        px = ui_clamp(L->chip.x, pad, ui_max(pad, L->vw - pad - pw));
        py = L->palette_y - gap - ph;
        if (py < L->header_y + L->header_h + gap)
            py = ui_max(pad, (L->vh - ph) / 2);
        L->pop = rect_make(px, py, pw, ph);
        L->pop_on = (st && st->mixer) ? 1 : 0;
        if (L->pop_on)
            mixer_build(L, F, L->pop.x + pad, L->pop.y + pad,
                        L->pop.w - 2 * pad, 0);
    }

    /* ---- the clear confirmation ---- */
    {
        int m = SP(1), b_h = ui_max(SP(3), sh + SP(1));
        int dw2 = ui_min(SP(42), L->vw - 2 * pad);
        int dh2 = m + th + m + sh + SP(2) + b_h + m;
        int bw;
        if (dw2 < 100) dw2 = ui_min(100, L->vw);
        if (dh2 > L->vh) dh2 = L->vh;
        L->dlg = rect_make((L->vw - dw2) / 2, (L->vh - dh2) / 2, dw2, dh2);
        L->dlg_title_y = L->dlg.y + m;
        L->dlg_body_y = L->dlg_title_y + th + m;
        L->dlg_btn_h = b_h;
        L->dlg_btn_y = L->dlg.y + dh2 - m - b_h;
        if (L->dlg_btn_y < L->dlg_body_y + sh) L->dlg_btn_y = L->dlg_body_y + sh;
        bw = ui_min(SP(14), (dw2 - 3 * m) / 2);
        if (bw < 1) bw = 1;
        L->dlg_no = rect_make(L->dlg.x + m, L->dlg_btn_y, bw, b_h);
        L->dlg_yes = rect_make(L->dlg.x + dw2 - m - bw, L->dlg_btn_y, bw, b_h);
    }
}

/* ------------------------------------------------------------
 *  The canvas, scaled to fit when the window is too small
 * ------------------------------------------------------------ */

static u32 stage[CANVAS_PX];

/* Scaled to fit: an area average, weighted by how much of each source pixel
 * really falls inside the destination pixel.
 *
 * This used to round the box's edges down to whole source pixels and average
 * those with equal weight. At the fit the app actually opens with (a 400x320
 * sheet shown at 363x291) every tenth destination row or column then averaged
 * two source pixels 50/50 when their true shares were 91/9, and a one-pixel
 * stroke landed in a single destination column at full strength or half
 * strength depending on where it fell - so a stroke pulsed darker as it
 * moved. Measured against a true area average: 185/255 worst error, 4.4% of
 * the sheet more than 8 levels off, and a 1px line's ink swinging 116..232.
 * The weights below make every destination pixel the exact average of the
 * part of the sheet it covers, so an edge moves by fractions of a level
 * instead of jumping. Still one write per destination pixel.
 *
 * The span bounds are walked with an add and a carry instead of a division
 * per pixel, and the weights of one destination pixel always sum to exactly
 * CANVAS_W*CANVAS_H, so the three colour divisions are by a constant. */
static void blit_scaled(Surface *s, const Rect *d, const u32 *src) {
    int xstep = CANVAS_W / d->w, xrest = CANVAS_W % d->w;
    int ystep = CANVAS_H / d->h, yrest = CANVAS_H % d->h;
    long ylo = 0, yhi = CANVAS_H;              /* in 1/d->h source units */
    int sy0 = 0, sy1 = (CANVAS_H - 1) / d->h;
    int yrem0 = 0, yrem1 = (CANVAS_H - 1) % d->h;

    for (int y = 0; y < d->h; y++) {
        int dy = d->y + y;
        if ((unsigned)dy < (unsigned)s->h) {
            long xlo = 0, xhi = CANVAS_W;      /* in 1/d->w source units */
            int sx0 = 0, sx1 = (CANVAS_W - 1) / d->w;
            int xrem0 = 0, xrem1 = (CANVAS_W - 1) % d->w;
            for (int x = 0; x < d->w; x++) {
                int dx = d->x + x;
                if ((unsigned)dx < (unsigned)s->w) {
                    if (sy0 == sy1 && sx0 == sx1) {     /* one source pixel */
                        s->px[(size_t)dy * s->stride + dx] =
                            src[(size_t)sy0 * CANVAS_W + sx0];
                    } else {
                        unsigned long r = 0, g = 0, b = 0;
                        for (int sy = sy0; sy <= sy1; sy++) {
                            long a = (long)sy * d->h, e = a + d->h;
                            long wa = a > ylo ? a : ylo;
                            long wb = e < yhi ? e : yhi;
                            unsigned long wy = (unsigned long)(wb - wa);
                            if (!wy) continue;
                            for (int sx = sx0; sx <= sx1; sx++) {
                                long a2 = (long)sx * d->w, e2 = a2 + d->w;
                                long w2a = a2 > xlo ? a2 : xlo;
                                long w2b = e2 < xhi ? e2 : xhi;
                                unsigned long wx = (unsigned long)(w2b - w2a);
                                u32 c;
                                if (!wx) continue;
                                c = src[(size_t)sy * CANVAS_W + sx];
                                r += wx * wy * GET_R(c);
                                g += wx * wy * GET_G(c);
                                b += wx * wy * GET_B(c);
                            }
                        }
                        surf_set(s, dx, dy,
                                 RGB((int)(r / (CANVAS_W * (unsigned long)CANVAS_H)),
                                     (int)(g / (CANVAS_W * (unsigned long)CANVAS_H)),
                                     (int)(b / (CANVAS_W * (unsigned long)CANVAS_H))));
                    }
                }
                xlo = xhi;                               /* next destination x */
                xhi += CANVAS_W;
                sx0 += xstep;
                xrem0 += xrest;
                if (xrem0 >= d->w) { xrem0 -= d->w; sx0++; }
                sx1 += xstep;
                xrem1 += xrest;
                if (xrem1 >= d->w) { xrem1 -= d->w; sx1++; }
            }
        }
        ylo = yhi;                                       /* next destination y */
        yhi += CANVAS_H;
        sy0 += ystep;
        yrem0 += yrest;
        if (yrem0 >= d->h) { yrem0 -= d->h; sy0++; }
        sy1 += ystep;
        yrem1 += yrest;
        if (yrem1 >= d->h) { yrem1 -= d->h; sy1++; }
    }
}

static void blit_canvas(Surface *s, const Rect *d, const u32 *src) {
    if (d->w <= 0 || d->h <= 0) return;
    if (d->w == CANVAS_W && d->h == CANVAS_H) {
        for (int y = 0; y < CANVAS_H; y++) {
            int dy = d->y + y;
            if ((unsigned)dy >= (unsigned)s->h) continue;
            for (int x = 0; x < CANVAS_W; x++) {
                int dx = d->x + x;
                if ((unsigned)dx >= (unsigned)s->w) continue;
                s->px[(size_t)dy * s->stride + dx] = src[(size_t)y * CANVAS_W + x];
            }
        }
        return;
    }
    blit_scaled(s, d, src);
}

/* Window point -> canvas pixel, the exact inverse of blit_canvas's box. */
static void canvas_map(const Layout *L, int x, int y, int *cx, int *cy) {
    int w = L->canvas.w > 0 ? L->canvas.w : 1;
    int h = L->canvas.h > 0 ? L->canvas.h : 1;
    *cx = (int)((long)(x - L->canvas.x) * CANVAS_W / w);
    *cy = (int)((long)(y - L->canvas.y) * CANVAS_H / h);
}

/* ------------------------------------------------------------
 *  The frame the server gets
 * ------------------------------------------------------------
 *
 *  The server hands an app one shared pixel buffer and reads it whenever it
 *  composites. It composites on our FRAME and, while the pointer is moving,
 *  on every mouse move as well (src/main.c marks the desktop dirty for each
 *  one), and nothing on either side locks the buffer while that happens.
 *
 *  Drawing the window straight into that buffer therefore shows the
 *  compositor a half-finished frame: at 640x520 a frame is 6.7-9.0 ms of
 *  scattered writes (measured on a host build of this file: sheet blit
 *  4.4-5.6 ms, 1px sheet border 1.2-1.9 ms, the rest ~0.8 ms) while the
 *  compositor's read of it, src/main.c's row loop, is 0.53-0.69 ms for the
 *  same 1.33 MB - so the top of the sheet was the stroke you were dragging
 *  and the bottom was the one from the frame before: the flicker the user
 *  kept seeing. No amount of care inside the rasterisers can fix that; the
 *  raster is already exact (two renders of one state are byte-identical).
 *
 *  So the window is drawn into `frame` - memory nothing else reads - and
 *  only the pixels that actually changed are copied across, in one burst,
 *  after the frame is complete. The compositor can then only ever catch a
 *  seam inside a span that is being copied, never the whole window, and
 *  the burst is a flat memcpy rather than milliseconds of drawing.
 *
 *  The same diff answers the other half of the complaint: a frame that
 *  changed nothing - the pointer crossing the sheet with no button down, a
 *  keypress that moved no pixel, the same state drawn again - copies zero
 *  bytes and sends no FRAME at all. It used to rewrite 1.33 MB of shared
 *  memory and make the desktop recompose itself (blur, shadows and all)
 *  for a picture identical to the one already on screen. */
#define FRAME_W WIN_W
#define FRAME_H WIN_H
static u32 frame[FRAME_W * FRAME_H];

/* Copy the changed part of frame[] (stride == w) into the client's shared
 * surface and return how many pixels were copied; 0 means the server
 * already has this exact frame. One contiguous span per changed row, top
 * down, so a seam can only ever fall inside the span being written.
 *
 * The comparison is a plain word loop rather than memcmp: it finds the
 * first changed column in the same pass, so a row that did not change costs
 * one read of each buffer and nothing else, and a row that did needs no
 * second scan to find where to start. */
static long frame_present(MClient *c, int w, int h) {
    long copied = 0;

    if (!c || !c->px || w <= 0 || h <= 0) return 0;
    if (w > c->maxw) w = c->maxw;
    if (h > c->maxh) h = c->maxh;
    for (int y = 0; y < h; y++) {
        const u32 *src = frame + (size_t)y * (size_t)w;
        u32 *dst = c->px + (size_t)y * (size_t)c->stride;
        int x0 = 0, x1;
        while (x0 < w && src[x0] == dst[x0]) x0++;
        if (x0 == w) continue;                   /* the row is the same */
        for (x1 = w - 1; x1 > x0 && src[x1] == dst[x1]; x1--) { }
        memcpy(dst + x0, src + x0, (size_t)(x1 + 1 - x0) * sizeof(u32));
        copied += x1 + 1 - x0;
    }
    return copied;
}

/* ------------------------------------------------------------
 *  Draw
 * ------------------------------------------------------------ */

static void mixer_draw(Surface *s, const Layout *L, const Fonts *F,
                       const PaintState *st) {
    static const char *NM[3] = { "R", "G", "B" };
    static const u32 CC[3] = {
        RGB(0xE0, 0x6C, 0x5C), RGB(0x7B, 0xD8, 0x8F), RGB(0x7A, 0xA2, 0xF7)
    };
    int v[3] = { st->r, st->g, st->b };
    char hex[16];

    snprintf(hex, sizeof(hex), "#%02X%02X%02X", st->r, st->g, st->b);
    text_draw(s, F->small, L->mix_x, L->mix_hex_y, L->mix_w, 0, hex, C_DIM);
    for (int i = 0; i < 3; i++) {
        char val[8];
        snprintf(val, sizeof(val), "%d", v[i]);
        text_draw(s, F->small, L->mix_x, L->mix_lab_y[i], L->mix_w, 0, NM[i], CC[i]);
        text_draw(s, F->small, L->mix_x, L->mix_lab_y[i], L->mix_w, 1, val, C_TEXT);
        slider_draw(s, &L->sl[i], v[i], CC[i], st->slider == i || st->hot == H_SL + i);
    }
}

/* Draw the whole window into `dst`. What `dst` points at is the caller's
 * business: the shipped app points it at frame[] and presents the diff,
 * the host tests point it straight at a buffer of their own. */
static void draw_ui(Surface *dst, const Layout *L, const Fonts *F,
                    const PaintState *st, const char *msg, int msg_warn) {
    int vw = L->vw, vh = L->vh;
    Surface s = *dst;
    u32 cur = RGB(st->r, st->g, st->b);

    if (vw <= 0 || vh <= 0) return;
    if (!s.px) return;
    s.w = vw;
    s.h = vh;
    s.cx0 = s.cy0 = s.cx1 = s.cy1 = 0;
    surf_fill(&s, 0, 0, vw, vh, C_BG);

    /* ---- the sheet, with the shape being dragged on top of it ---- */
    if (L->canvas.w > 0 && L->canvas.h > 0) {
        const u32 *src = canvas;
        if (st->drag) {
            Ink k;
            int x0 = st->sx, y0 = st->sy, x1 = st->px, y1 = st->py;
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
            memcpy(stage, canvas, CANVAS_BYTES);
            ink_init(&k, stage, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                     cur, st->brush);
            shape_draw(&k, st->tool, x0, y0, x1, y1);
            src = stage;
        }
        blit_canvas(&s, &L->canvas, src);
        gfx_rrect_aa(&s, (float)L->canvas.x - 1.0f, (float)L->canvas.y - 1.0f,
                     (float)L->canvas.w + 2.0f, (float)L->canvas.h + 2.0f,
                     2.0f, 0, C_LINE, 1.0f);
    }

    /* ---- the tool strip ---- */
    for (int i = 0; i < T_N; i++)
        strip_button(&s, st, &L->tool[i], i, st->tool == i, H_TOOL + i, 1);
    for (int i = 0; i < A_N; i++) {
        int en = 1;
        if (i == A_UNDO) en = hist_at > 0;
        else if (i == A_REDO) en = hist_at + 1 < hist_n;
        strip_button(&s, st, &L->act[i], IC_UNDO + i, 0, H_ACT + i, en);
    }
    if (L->strip_rows == 1 && L->divider_x > 0)
        surf_fill(&s, L->divider_x, L->strip_y + L->btn / 3, 1,
                  ui_max(1, L->btn / 3), C_LINE);

    /* ---- the palette row: current colour, then the twelve inks ---- */
    gfx_rrect_aa(&s, (float)L->chip.x, (float)L->chip.y,
                 (float)L->chip.w, (float)L->chip.h, 3.0f, cur,
                 L->pop_on ? C_ACCENT : C_TEXT, 1.2f);
    for (int i = 0; i < PAL_N; i++) {
        const Rect *r = &L->swatch[i];
        int chosen = st->r == (int)GET_R(PALETTE[i]) &&
                     st->g == (int)GET_G(PALETTE[i]) &&
                     st->b == (int)GET_B(PALETTE[i]);
        int hot = st->hot == H_SW + i;
        u32 bord = chosen ? C_TEXT : hot ? C_DIM : 0;
        gfx_rrect_aa(&s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                     2.0f, PALETTE[i], bord, bord ? 1.2f : 0.0f);
    }

    /* ---- the mixer: a panel beside the canvas, or a popover on it ---- */
    if (L->panel_on) {
        gfx_rrect_aa(&s, (float)L->panel.x, (float)L->panel.y,
                     (float)L->panel.w, (float)L->panel.h, 6.0f,
                     C_PANEL, C_LINE, 1.0f);
        gfx_rrect_aa(&s, (float)L->mix_bar.x, (float)L->mix_bar.y,
                     (float)L->mix_bar.w, (float)L->mix_bar.h, 4.0f, cur, 0, 0);
        mixer_draw(&s, L, F, st);
    } else if (L->pop_on) {
        gfx_rrect_aa(&s, (float)L->pop.x, (float)L->pop.y,
                     (float)L->pop.w, (float)L->pop.h, 8.0f,
                     C_PANEL, C_ACCENT, 1.0f);
        mixer_draw(&s, L, F, st);
    }

    /* ---- the header ---- */
    {
        int ty = L->header_y + ui_top(F->title, L->header_h);
        char num[8];
        text_draw(&s, F->title, L->title_x, ty, L->title_w, 0,
                  TOOL_NAME[st->tool], C_TEXT);
        if (L->show_brush_label) {
            int ly = L->header_y + ui_top(F->small, L->header_h);
            text_draw(&s, F->small, L->brush_label_x, ly, L->brush_label_w, 1,
                      "Brush", C_DIM);
        }
        snprintf(num, sizeof(num), "%d", st->brush);
        text_draw(&s, F->body, L->brush_num_x,
                  L->header_y + ui_top(F->body, L->header_h),
                  L->brush_num_w, 1, num, C_TEXT);
        stepper_draw(&s, &L->brush_minus, 0, st->hot == H_BM, st->brush > 1);
        stepper_draw(&s, &L->brush_plus, 1, st->hot == H_BP, st->brush < MAX_BRUSH);
    }

    /* ---- the footer: the folder, the undo depth, the brush ---- */
    {
        char why[120];
        const char *dir = paint_dir(why, sizeof(why));
        char und[24], br[16];
        int fy = L->footer_y + ui_top(F->small, L->footer_h);

        text_draw(&s, F->small, L->pad, fy, L->footer_left_w, 0,
                  dir ? dir : "no writable picture folder", dir ? C_DIM : C_WARN);
        if (!hist_ok) snprintf(und, sizeof(und), "undo --");
        else snprintf(und, sizeof(und), "undo %d/%d", hist_at, HIST_STATES - 1);
        text_draw(&s, F->small, L->footer_right_x, fy, L->footer_undo_w, 1,
                  und, hist_oom ? C_WARN : C_DIM);
        snprintf(br, sizeof(br), "brush %d", st->brush);
        text_draw(&s, F->small, L->footer_brush_x, fy,
                  (L->vw - L->pad) - L->footer_brush_x, 1, br, C_DIM);
    }

    /* ---- what just happened, as a toast over the sheet ---- */
    if (msg && msg[0] && !L->pop_on &&
        L->canvas.h >= 5 * ui_ceil(font_height(F->small))) {
        char t[224];
        int inner = ui_min(vw - 2 * L->pad - 2 * SP(2),
                           ui_ceil(font_width(F->small, msg)));
        text_fit(t, sizeof(t), F->small, msg, inner);
        if (t[0]) {
            int w = ui_ceil(font_width(F->small, t)) + 2 * SP(2);
            int h = ui_ceil(font_height(F->small)) + 2 * SP(1);
            int tx, ty;
            if (w > vw - 2 * L->pad) w = vw - 2 * L->pad;
            tx = L->canvas.x + (L->canvas.w - w) / 2;
            tx = ui_clamp(tx, L->pad, ui_max(L->pad, vw - L->pad - w));
            ty = L->canvas.y + L->canvas.h - h - SP(1);
            if (ty < L->canvas.y) ty = L->canvas.y + SP(1);
            gfx_rrect_aa(&s, (float)tx, (float)ty, (float)w, (float)h,
                         (float)h * 0.5f, C_PANEL,
                         msg_warn ? C_WARN : C_ACCENT, 1.0f);
            text_draw(&s, F->small, tx + SP(2), ty + SP(1), w - 2 * SP(2), 0, t,
                      msg_warn ? C_WARN : C_TEXT);
        }
    }

    /* ---- the clear confirmation ---- */
    if (st->dialog) {
        surf_blend(&s, 0, 0, vw, vh, color_alpha(RGB(0, 0, 0), 0.55f));
        gfx_rrect_aa(&s, (float)L->dlg.x, (float)L->dlg.y,
                     (float)L->dlg.w, (float)L->dlg.h, 8.0f,
                     C_PANEL, C_ACCENT, 1.0f);
        text_draw(&s, F->title, L->dlg.x + SP(1), L->dlg_title_y,
                  L->dlg.w - 2 * SP(1), 0, "Clear the canvas?", C_TEXT);
        text_draw(&s, F->small, L->dlg.x + SP(1), L->dlg_body_y,
                  L->dlg.w - 2 * SP(1), 0,
                  "Every pixel goes; undo (u) brings the last state back.", C_DIM);
        {
            const Rect *r = &L->dlg_no;
            u32 fill = st->hot == H_NO ? C_HOVER : 0;
            gfx_rrect_aa(&s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                         4.0f, fill, C_LINE, 1.0f);
            text_draw(&s, F->body, r->x, r->y + ui_top(F->body, r->h),
                      r->w, 2, "Cancel", C_TEXT);
        }
        {
            const Rect *r = &L->dlg_yes;
            gfx_rrect_aa(&s, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
                         4.0f, color_alpha(C_ACCENT, 0.24f),
                         color_alpha(C_ACCENT, 0.85f), 1.0f);
            text_draw(&s, F->body, r->x, r->y + ui_top(F->body, r->h),
                      r->w, 2, "Clear", C_TEXT);
        }
    }
}

/* One frame, end to end: draw it off to the side, hand the server only the
 * pixels that changed, and tell it once - or not at all when the picture is
 * the one it already has. Returns 1 when the server was told to look. */
static int paint_commit(MClient *c, int vw, int vh, const Layout *L,
                        const Fonts *F, const PaintState *st,
                        const char *msg, int msg_warn) {
    Surface s;
    long copied;

    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    if (vw > FRAME_W) vw = FRAME_W;      /* the window is capped at open time */
    if (vh > FRAME_H) vh = FRAME_H;
    s.px = frame;
    s.w = vw;
    s.h = vh;
    s.stride = vw;                       /* rows packed: the diff walks them */
    s.cx0 = s.cy0 = s.cx1 = s.cy1 = 0;
    draw_ui(&s, L, F, st, msg, msg_warn);
    copied = frame_present(c, vw, vh);
    if (copied <= 0) return 0;
    mc_present(c);
    return 1;
}

/* ============================================================
 *  Input
 * ============================================================ */

static int hit_code(const Layout *L, const PaintState *st, int x, int y) {
    if (st->dialog) {
        if (rect_hit(&L->dlg_yes, x, y)) return H_YES;
        if (rect_hit(&L->dlg_no, x, y)) return H_NO;
        return H_NONE;
    }
    for (int i = 0; i < 3; i++) {
        const Rect *r = &L->sl[i];
        if (r->w > 0 &&
            x >= r->x - SP(1) && x < r->x + r->w + SP(1) &&
            y >= r->y - SP(1) && y < r->y + r->h + SP(1))
            return H_SL + i;
    }
    if (L->pop_on && rect_hit(&L->pop, x, y)) return H_POP;
    if (rect_hit(&L->brush_minus, x, y)) return H_BM;
    if (rect_hit(&L->brush_plus, x, y)) return H_BP;
    if (rect_hit(&L->chip, x, y)) return H_CHIP;
    for (int i = 0; i < T_N; i++)
        if (rect_hit(&L->tool[i], x, y)) return H_TOOL + i;
    for (int i = 0; i < A_N; i++)
        if (rect_hit(&L->act[i], x, y)) return H_ACT + i;
    for (int i = 0; i < PAL_N; i++)
        if (rect_hit(&L->swatch[i], x, y)) return H_SW + i;
    return H_NONE;
}

static void slider_set(PaintState *st, const Rect *r, int x, int i) {
    int v = r->w > 1 ? (x - r->x) * 255 / (r->w - 1) : 0;
    v = ui_clamp(v, 0, 255);
    if (i == 0) st->r = v;
    else if (i == 1) st->g = v;
    else st->b = v;
}

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
        } else if (c == 'm' || c == 'M') {
            if (!L->panel_on) { st->mixer = !st->mixer; *dirty = 1; }
        } else if (c == 'k' || c == 'K') {
            if (st->cursor_in &&
                (unsigned)st->px < (unsigned)CANVAS_W &&
                (unsigned)st->py < (unsigned)CANVAS_H) {
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

/* Apply a press: the stepper, the strip, the palette, the mixer, or the
 * sheet. The hit test is the same one the hover tint uses. */
static void press_at(PaintState *st, const Layout *L, int x, int y,
                     char *msg, size_t n, int *dirty) {
    int h = hit_code(L, st, x, y);

    if (st->dialog) {
        if (h == H_YES) do_clear(L, st, msg, n, dirty);
        else if (h == H_NO) { st->dialog = 0; *dirty = 1; }
        return;
    }
    if (h >= H_SL && h < H_SL + 3) {
        st->slider = h - H_SL;
        slider_set(st, &L->sl[st->slider], x, st->slider);
        *dirty = 1;
        return;
    }
    if (h == H_POP) return;                       /* the popover's own body */
    if (h >= H_TOOL && h < H_TOOL + T_N) {
        set_tool(st, h - H_TOOL);
        st->mixer = 0;
        *dirty = 1;
        return;
    }
    if (h >= H_ACT && h < H_ACT + A_N) {
        switch (h - H_ACT) {
        case A_UNDO:
            if (hist_undo()) snprintf(msg, n, "undo");
            else snprintf(msg, n, "nothing to undo");
            break;
        case A_REDO:
            if (hist_redo()) snprintf(msg, n, "redo");
            else snprintf(msg, n, "nothing to redo");
            break;
        case A_SAVE: act_save(msg, n); break;
        case A_OPEN: act_open(msg, n); break;
        case A_CLEAR: st->dialog = 1; break;
        default: break;
        }
        *dirty = 1;
        return;
    }
    if (h >= H_SW && h < H_SW + PAL_N) {
        int i = h - H_SW;
        st->r = GET_R(PALETTE[i]);
        st->g = GET_G(PALETTE[i]);
        st->b = GET_B(PALETTE[i]);
        *dirty = 1;
        return;
    }
    if (h == H_CHIP) {
        if (!L->panel_on) st->mixer = !st->mixer;
        *dirty = 1;
        return;
    }
    if (h == H_BM) {
        if (st->brush > 1) st->brush--;
        *dirty = 1;
        return;
    }
    if (h == H_BP) {
        if (st->brush < MAX_BRUSH) st->brush++;
        *dirty = 1;
        return;
    }
    if (h == H_NONE && st->mixer) {
        st->mixer = 0;                            /* a click outside closes it */
        *dirty = 1;
    }
    if (!rect_hit(&L->canvas, x, y)) return;

    canvas_map(L, x, y, &st->px, &st->py);
    if (st->px < 0) st->px = 0;
    if (st->py < 0) st->py = 0;
    if (st->px > CANVAS_W - 1) st->px = CANVAS_W - 1;
    if (st->py > CANVAS_H - 1) st->py = CANVAS_H - 1;
    st->cursor_in = 1;

    switch (st->tool) {
    case T_PENCIL: {
        Ink k;
        ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
                 RGB(st->r, st->g, st->b), st->brush);
        ink_stamp(&k, st->px, st->py);
        st->lx = st->px;
        st->ly = st->py;
        st->down = 1;
        st->stroke = k.hits > 0;
        if (k.hits) *dirty = 1;
        break;
    }
    case T_FILL: {
        char err[96];
        long got = flood_fill(st->px, st->py, RGB(st->r, st->g, st->b), err, sizeof(err));
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
        u32 v = canvas[(size_t)st->py * CANVAS_W + (size_t)st->px];
        st->r = GET_R(v);
        st->g = GET_G(v);
        st->b = GET_B(v);
        snprintf(msg, n, "picked #%02X%02X%02X", st->r, st->g, st->b);
        *dirty = 1;
        break;
    }
    default:
        st->sx = st->px;
        st->sy = st->py;
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
    if (st->slider >= 0) {
        /* the slider's ring changes when the drag stops */
        st->slider = -1;
        *dirty = 1;
    }
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
        if (k.hits && hist_commit() != 0)
            snprintf(msg, n, "drawn; history is now unavailable");
        /* the preview is gone from the frame whether or not it committed
         * anything, so the frame is new even when k.hits is 0 */
        *dirty = 1;
        return;
    }
    if (st->down) {
        st->down = 0;
        if (!st->stroke) return;                 /* a click that changed nothing */
        st->stroke = 0;
        if (hist_commit() != 0) snprintf(msg, n, "stroke done; history is now unavailable");
        *dirty = 1;
    }
}

/* ============================================================
 *  One frame, one event
 * ============================================================
 *
 *  These two are the app's whole loop body, as functions rather than as
 *  inline code in main(), so the host harness can drive the real thing and
 *  count what actually reaches the server. main() below is then only the
 *  wait/read/quit plumbing.
 */

/* Monotonic milliseconds: how long a message stays on screen. */
static u64 now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000u + (u64)(ts.tv_nsec / 1000000);
}

typedef struct {
    MClient   *c;
    Fonts      F;
    PaintState st;
    Layout     L;
    char       msg[200];
    char       shown[200];
    u64        msg_until;
    int        vw, vh;              /* the window we draw, capped at open */
    int        cap_w, cap_h;
    int        dirty;               /* the state changed: draw before reading */
    int        quit;
    int        was_down;
    long       frames;              /* frames handed to the server */
} PaintApp;

static void paint_app_init(PaintApp *A, MClient *c) {
    memset(A, 0, sizeof(*A));
    A->c = c;
    A->vw = A->cap_w = c->w > 0 ? c->w : 1;
    A->vh = A->cap_h = c->h > 0 ? c->h : 1;
    A->st.tool = T_PENCIL;
    A->st.brush = 3;
    A->st.r = GET_R(PALETTE[0]);
    A->st.g = GET_G(PALETTE[0]);
    A->st.b = GET_B(PALETTE[0]);
    A->st.slider = -1;
    A->st.hot = H_NONE;
    A->dirty = 1;
}

/* Layout, message expiry, and - only when something changed - one frame. */
static void paint_frame(PaintApp *A) {
    layout_build(A->vw, A->vh, &A->F, &A->st, &A->L);

    /* A message appears when it changes and leaves on its own; it is not
     * part of the layout, so it can never push a row around. */
    if (strcmp(A->msg, A->shown) != 0) {
        snprintf(A->shown, sizeof(A->shown), "%s", A->msg);
        A->msg_until = now_ms() + 3200u;
        A->dirty = 1;
    }
    if (A->msg[0] && now_ms() > A->msg_until) {
        A->msg[0] = 0;
        A->shown[0] = 0;
        A->msg_until = 0;
        A->dirty = 1;
    }
    if (!A->dirty) return;
    /* Draw off to the side and hand over only what changed: the frame is
     * the one the server can see only once it is finished, and an unchanged
     * frame (a hover, a key that moved nothing) is not presented at all. */
    if (paint_commit(A->c, A->vw, A->vh, &A->L, &A->F, &A->st,
                     A->msg, g_msg_bad))
        A->frames++;
    A->dirty = 0;
}

static void paint_event(PaintApp *A, const MEvent *ev) {
    PaintState *st = &A->st;
    const Layout *L = &A->L;         /* the layout this event lands in */
    char *msg = A->msg;
    size_t n = sizeof(A->msg);
    int *dirty = &A->dirty;

    switch (ev->type) {
    case MC_CLOSE:
        A->quit = 1;
        break;

    case MC_RESIZE:
        A->vw = ev->w < A->cap_w ? ev->w : A->cap_w;
        A->vh = ev->h < A->cap_h ? ev->h : A->cap_h;
        if (A->vw < 1) A->vw = 1;
        if (A->vh < 1) A->vh = 1;
        *dirty = 1;
        break;

    case MC_KEY: {
        int was_shift = st->shift;
        st->shift = ev->mods & 1;
        /* shift changes the shape a drag is constrained to, so the preview
         * has to be redrawn even if the pointer has not moved */
        if (st->drag && st->shift != was_shift) *dirty = 1;
        if (ev->press) {
            if (ev->key == 1) {                        /* KEY_ESC */
                if (st->dialog || st->drag || st->down || st->mixer) {
                    st->dialog = 0;
                    st->drag = 0;
                    st->down = 0;
                    st->mixer = 0;
                    snprintf(msg, n, "cancelled");
                    *dirty = 1;
                }
            } else if (ev->key == 28) {                /* KEY_ENTER */
                if (st->dialog) do_clear(L, st, msg, n, dirty);
            }
        }
        break;
    }

    case MC_TEXT:
        handle_text(st, ev->text, msg, n, L, dirty, &A->quit);
        break;

    case MC_MOUSE: {
        int x = ev->x, y = ev->y;
        int down = ev->buttons & 1;
        int hot = hit_code(L, st, x, y);
        int cx, cy;
        if (hot != st->hot) { st->hot = hot; *dirty = 1; }
        st->cursor_in = rect_hit(&L->canvas, x, y) && !L->pop_on;
        if (st->cursor_in) canvas_map(L, x, y, &st->px, &st->py);

        if (down && !A->was_down) {
            press_at(st, L, x, y, msg, n, dirty);
        } else if (down && A->was_down) {
            if (st->slider >= 0) {
                slider_set(st, &L->sl[st->slider], x, st->slider);
                *dirty = 1;
            } else if (st->drag) {
                canvas_map(L, x, y, &st->px, &st->py);
                *dirty = 1;
            } else if (st->down) {
                canvas_map(L, x, y, &cx, &cy);
                drag_at(st, cx, cy, msg, n, dirty);
            }
        } else if (!down && A->was_down) {
            release_at(st, msg, n, dirty);
        }
        A->was_down = down;
        /* No unconditional redraw here: a bare hover changes no pixel (the
         * diff in paint_commit proves it - the frame is byte-identical), so
         * it must not cost a frame. Everything that does change the picture
         * - the hover code above, the drag, a stroke, the slider, a press or
         * a release - sets *dirty itself. */
        break;
    }

    case MC_WHEEL:
        if (st->brush + ev->wheel >= 1 && st->brush + ev->wheel <= MAX_BRUSH) {
            st->brush += ev->wheel;
            *dirty = 1;
        }
        break;

    default:
        break;
    }
}

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
    F.title = font_get(FONT_SEMIBOLD, 15);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_MEDIUM, 9);
    if (!F.title || !F.body || !F.small) {
        fprintf(stderr, "paint: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    stamps_build();
    canvas_reset();
    hist_init();

    PaintApp A;
    paint_app_init(&A, &c);
    A.F = F;

    for (;;) {
        paint_frame(&A);

        int got = 0;
        MEvent ev;
        while (mc_poll(&c, &ev)) {
            got = 1;
            paint_event(&A, &ev);
            if (A.quit) {
                hist_done();
                mc_close(&c);
                return 0;
            }
        }

        /* Nothing to do: sleep until the server has something for us
         * rather than for a flat 20 ms. The pointer reports at about
         * 100 Hz, and a frame here is 6.7 ms, so a blind 20 ms nap landed in
         * the gap after almost every frame and the drag preview then moved
         * two or three pointer samples at a time - the jump the user saw.
         * poll() on the socket wakes on the first byte instead. */
        if (!got && !A.dirty) {
            struct pollfd p;
            p.fd = c.fd;
            p.events = POLLIN;
            p.revents = 0;
            while (poll(&p, 1, 20) < 0 && errno == EINTR) { }
        }
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

/* ------------------------------------------------------------
 *  A canvas with real art on it, drawn by the shipped rasterisers, so
 *  the render is a picture of the program rather than an empty sheet.
 * ------------------------------------------------------------ */

static char g_fill_msg[64];

static void demo_art(void) {
    Ink k;
    char err[64];
    long got;

    stamps_build();
    canvas_reset();
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
             RGB(0xF5, 0xC5, 0x42), 6);
    ink_ellipse(&k, 42, 46, 206, 196, 0);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
             RGB(0x4F, 0xD1, 0xC5), 3);
    ink_line(&k, 214, 56, 356, 246);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
             RGB(0xF7, 0x8F, 0xB3), 9);
    ink_rect(&k, 236, 206, 362, 292, 1);
    ink_init(&k, canvas, CANVAS_W, 0, 0, 0, 0, CANVAS_W, CANVAS_H,
             RGB(0x7A, 0xA2, 0xF7), 2);
    ink_rect(&k, 58, 238, 184, 292, 0);
    /* a real flood fill, and the real number of pixels it changed */
    got = flood_fill(120, 120, RGB(0x1E, 0x3A, 0x3E), err, sizeof(err));
    if (got > 0) snprintf(g_fill_msg, sizeof(g_fill_msg), "filled %ld px", got);
    else snprintf(g_fill_msg, sizeof(g_fill_msg), "fill: %s", err);
}

/* ------------------------------------------------------------
 *  The layout proof: every box inside the window, the bands in order,
 *  and every drawn string measured against its box.
 * ------------------------------------------------------------ */

static int g_audit_fail;

static void audit(int cond, const char *what) {
    printf("    %-56s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) g_audit_fail++;
}

static void layout_audit(const Layout *L) {
    int pad = L->pad;
    long aspect;

    printf("    layout %dx%d: header %d..%d  strip %d..%d (%d row%s)  "
           "canvas %d,%d %dx%d  palette %d  footer %d..%d%s\n",
           L->vw, L->vh, L->header_y, L->header_y + L->header_h,
           L->strip_y, L->strip_y + L->strip_h, L->strip_rows,
           L->strip_rows == 1 ? "" : "s",
           L->canvas.x, L->canvas.y, L->canvas.w, L->canvas.h,
           L->palette_y, L->footer_y, L->footer_y + L->footer_h,
           L->panel_on ? "  panel" : "  no panel");
    audit(L->header_y >= 0 && L->header_y + L->header_h <= L->vh,
          "header inside the window");
    audit(L->strip_y >= L->header_y + L->header_h, "strip below the header");
    audit(L->strip_y + L->strip_h <= L->canvas.y, "canvas below the strip");
    audit(L->canvas.x >= 0 && L->canvas.x + L->canvas.w <= L->vw,
          "canvas inside the window (x)");
    audit(L->canvas.y >= 0 && L->canvas.y + L->canvas.h <= L->vh,
          "canvas inside the window (y)");
    audit(L->canvas.y + L->canvas.h <= L->palette_y, "palette below the canvas");
    audit(L->palette_y + L->palette_h <= L->footer_y, "footer below the palette");
    audit(L->footer_y + L->footer_h <= L->vh, "footer inside the window");
    audit(L->title_x + L->title_w < L->brush_minus.x, "title left of the stepper");
    audit(L->brush_plus.x + L->brush_plus.w <= L->vw - pad + 1,
          "brush stepper inside the window");
    audit(L->chip.x + L->chip.w <= L->swatch[0].x, "current colour left of the swatches");
    audit(L->swatch[PAL_N - 1].x + L->swatch[PAL_N - 1].w <= L->vw - pad + 1,
          "palette row inside the window");
    audit(L->footer_right_x > L->pad && L->footer_brush_x > L->footer_right_x,
          "footer columns do not cross");
    aspect = (long)L->canvas.w * CANVAS_H - (long)L->canvas.h * CANVAS_W;
    audit(aspect <= CANVAS_W && aspect >= -CANVAS_W, "canvas keeps its aspect");
    if (L->panel_on) {
        audit(L->panel.x >= L->canvas.x + L->canvas.w, "mixer panel right of the canvas");
        audit(L->panel.x + L->panel.w <= L->vw - pad, "mixer panel inside the window");
        audit(L->panel.y + L->panel.h <= L->palette_y, "mixer panel above the palette");
        audit(L->sl[2].y + L->sl[2].h <= L->panel.y + L->panel.h,
              "mixer rows inside the panel");
        audit(!L->pop_on, "the mixer is shown once (panel, not popover)");
    }
    if (L->pop_on) {
        audit(L->pop.x >= 0 && L->pop.x + L->pop.w <= L->vw, "popover inside the window (x)");
        audit(L->pop.y >= 0 && L->pop.y + L->pop.h <= L->vh, "popover inside the window (y)");
        audit(L->pop.w > 0 && L->pop.h > 0, "popover has room");
        audit(L->sl[0].w > 0, "popover sliders are laid out");
    }
    if (L->dlg.w > 0) {
        audit(L->dlg.x >= 0 && L->dlg.x + L->dlg.w <= L->vw, "dialog inside the window (x)");
        audit(L->dlg_yes.x > L->dlg_no.x + L->dlg_no.w, "dialog buttons do not overlap");
        audit(L->dlg_btn_y + L->dlg_btn_h <= L->dlg.y + L->dlg.h,
              "dialog buttons inside the dialog");
    }
}

static int render_scene(float scale, int vw, int vh, int scene,
                        const char *png, const char *bmp) {
    u32 *px;
    Layout L;
    PaintState st;
    Fonts F;
    const char *msg = "";
    char err[160];

    g_audit_fail = 0;
    g_text_drawn = 0;
    g_text_over = 0;

    /* scale is the value Settings shows (1.0 = Normal). The window server
     * exports the absolute multiplier - scale * UI_BASE, src/ui.c - and the
     * client hands that to ui_scale_set_effective(), so a proof render has
     * to do the same or it would test a scale nobody can select. */
    ui_scale_set_effective(1.4f * scale);
    if (font_init("assets/fonts") != 0) { printf("fonts: font_init failed\n"); return 2; }
    F.title = font_get(FONT_SEMIBOLD, 15);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_MEDIUM, 9);
    if (!F.title || !F.body || !F.small) { printf("fonts: missing faces\n"); return 2; }

    px = calloc((size_t)vw * (size_t)vh, sizeof(u32));
    if (!px) return 2;

    /* The render must not touch the filesystem, but the footer must show
     * the folder this build actually saves to - so the probe is marked as
     * done with the configured path instead of calling mkdir(2). */
    g_dir_tried = 1;
    snprintf(g_dir, sizeof(g_dir), "%s", PIC_DIR);

    demo_art();
    memset(&st, 0, sizeof(st));
    st.tool = T_ELLIPSE;
    st.brush = 6;
    st.r = 0xF5; st.g = 0xC5; st.b = 0x42;
    st.slider = -1;
    st.hot = H_TOOL + T_FILL;
    if (scene == 0) {
        st.drag = 1;
        st.sx = 40; st.sy = 60;
        st.px = 330; st.py = 250;
        st.cursor_in = 1;
        msg = "dragging an ellipse - release to commit";
    } else if (scene == 1) {
        st.dialog = 1;
        st.tool = T_RECTF;
        st.r = 0x7B; st.g = 0xD8; st.b = 0x8F;
        st.hot = H_YES;
    } else {
        st.mixer = 1;
        msg = g_fill_msg;
    }

    layout_build(vw, vh, &F, &st, &L);
    printf("  %dx%d at scale %.2f (title %.0fx%.0f, small %.0fx%.0f)%s\n",
           vw, vh, (double)scale,
           (double)font_height(F.title), (double)font_width(F.title, "M"),
           (double)font_height(F.small), (double)font_width(F.small, "M"),
           L.panel_on ? ", mixer panel" : (L.pop_on ? ", mixer popover" : ""));
    layout_audit(&L);

    {
        Surface dst;
        dst.px = px;
        dst.w = vw;
        dst.h = vh;
        dst.stride = vw;
        dst.cx0 = dst.cy0 = dst.cx1 = dst.cy1 = 0;
        draw_ui(&dst, &L, &F, &st, msg, 0);
    }
    audit(g_text_over == 0, "every drawn string fits its box");

    if (png) {
        remove(png);
        if (png_write_file(png, px, vw, vh, err, sizeof(err)) <= 0) {
            printf("    png %s: %s\n", png, err);
            g_audit_fail++;
        } else {
            printf("    -> %s (%d strings drawn, %d over)\n", png, g_text_drawn, g_text_over);
        }
    }
    if (bmp) {
        render_bmp(bmp, px, vw, vh);
        printf("    -> %s\n", bmp);
    }
    free(px);
    return g_audit_fail;
}

/* Render one size and scale and write a real PNG of the whole window. */
int paint_render_at(float scale, int vw, int vh, const char *png, int scene);

int paint_render_at(float scale, int vw, int vh, const char *png, int scene) {
    return render_scene(scale, vw, vh, scene, png, NULL);
}

/* The older harness entry point: the app's own window size, BMP output. */
int paint_render(float scale, const char *drag_out, const char *dialog_out);

int paint_render(float scale, const char *drag_out, const char *dialog_out) {
    /* This entry point predates the Settings-relative proof: it takes the
     * absolute multiplier the window server exports, where 1.4 is Normal. */
    float rel = scale / 1.4f;
    int rc = 0;
    printf("render at absolute %.2f (Settings %.2f)\n", (double)scale, (double)rel);
    rc |= render_scene(rel, WIN_W, WIN_H, 0, NULL, drag_out);
    rc |= render_scene(rel, WIN_W, WIN_H, 1, NULL, dialog_out);
    return rc ? 1 : 0;
}

/* ------------------------------------------------------------
 *  The input proof: drive the shipped press_at() with the shipped
 *  layout at both ends of the size range, and check that every
 *  capability the old window had still has a control that works.
 * ------------------------------------------------------------ */

static int ui_fails;

static void uichk(int cond, const char *what) {
    printf("    %-56s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ui_fails++;
}

int paint_ui_test(void);

int paint_ui_test(void) {
    Layout L;
    PaintState st;
    Fonts F;
    char msg[200];
    int dirty, cx, cy;

    ui_fails = 0;
    ui_scale_set_effective(1.4f);              /* Settings 1.0 = Normal */
    if (font_init("assets/fonts") != 0) { printf("fonts: font_init failed\n"); return 2; }
    F.title = font_get(FONT_SEMIBOLD, 15);
    F.body  = font_get(FONT_REGULAR, 11);
    F.small = font_get(FONT_MEDIUM, 9);
    if (!F.title || !F.body || !F.small) { printf("fonts: missing faces\n"); return 2; }

    g_dir_tried = 1;
    snprintf(g_dir, sizeof(g_dir), "%s", PIC_DIR);
    stamps_build();
    canvas_reset();
    hist_init();
    msg[0] = 0;

    memset(&st, 0, sizeof(st));
    st.tool = T_PENCIL;
    st.brush = 3;
    st.slider = -1;
    st.r = GET_R(PALETTE[0]);
    st.g = GET_G(PALETTE[0]);
    st.b = GET_B(PALETTE[0]);

    /* ---- the narrow window: two-row strip, popover mixer ---- */
    layout_build(380, 280, &F, &st, &L);
    uichk(!L.panel_on, "380x280: the mixer is a popover, not a panel");
    uichk(L.strip_rows == 2, "380x280: the strip wrapped instead of shrinking");

    for (int i = 0; i < T_N; i++) {
        Rect r = L.tool[i];
        dirty = 0;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        if (st.tool != i || !dirty) uichk(0, TOOL_NAME[i]);
    }
    uichk(1, "every tool button selects its tool");

    {
        Rect r = L.swatch[4];
        dirty = 0;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.r == (int)GET_R(PALETTE[4]) && st.g == (int)GET_G(PALETTE[4]) &&
              st.b == (int)GET_B(PALETTE[4]), "a palette swatch sets the ink");
    }
    {
        Rect r = L.brush_plus;
        dirty = 0;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.brush == 4, "the brush stepper's plus grows the brush");
        r = L.brush_minus;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.brush == 3, "the brush stepper's minus shrinks it");
    }

    /* ---- a stroke on the canvas goes through the fit scale ---- */
    st.tool = T_PENCIL;
    dirty = 0;
    canvas_map(&L, L.canvas.x + L.canvas.w / 2, L.canvas.y + L.canvas.h / 2, &cx, &cy);
    press_at(&st, &L, L.canvas.x + L.canvas.w / 2, L.canvas.y + L.canvas.h / 2,
             msg, sizeof(msg), &dirty);
    release_at(&st, msg, sizeof(msg), &dirty);
    uichk(abs(cx - CANVAS_W / 2) <= CANVAS_W / (L.canvas.w > 0 ? L.canvas.w : 1) + 1 &&
          abs(cy - CANVAS_H / 2) <= CANVAS_H / (L.canvas.h > 0 ? L.canvas.h : 1) + 1,
          "the pointer maps onto the canvas through the fit scale");
    uichk(canvas[(size_t)cy * CANVAS_W + cx] == RGB(st.r, st.g, st.b),
          "the pencil wrote the pixel under the pointer");
    uichk(hist_at == 1, "the stroke committed one undo state");

    /* ---- the eyedropper really reads the pixel under the pointer ---- */
    {
        u32 pv = canvas[(size_t)cy * CANVAS_W + cx];
        st.r = 1; st.g = 2; st.b = 3;
        st.tool = T_PICK;
        dirty = 0;
        press_at(&st, &L, L.canvas.x + L.canvas.w / 2, L.canvas.y + L.canvas.h / 2,
                 msg, sizeof(msg), &dirty);
        uichk(RGB(st.r, st.g, st.b) == pv && dirty,
              "the eyedropper reads the pixel under the pointer");
        st.r = GET_R(pv); st.g = GET_G(pv); st.b = GET_B(pv);
    }

    /* ---- the fill changes a real number of pixels and says how many ---- */
    {
        long was = count_colour(RGB(st.r, st.g, st.b));
        st.tool = T_FILL;
        dirty = 0;
        press_at(&st, &L, L.canvas.x + 2, L.canvas.y + 2, msg, sizeof(msg), &dirty);
        uichk(count_colour(RGB(st.r, st.g, st.b)) > was &&
              strstr(msg, "filled ") != NULL,
              "the fill reports the pixels it changed");
    }
    /* ---- undo and redo ---- */
    {
        Rect r = L.act[A_UNDO];
        int before = hist_at;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(hist_at == before - 1, "the undo button undoes");
        r = L.act[A_REDO];
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(hist_at == before, "the redo button redoes");
    }

    /* ---- clear asks first, and the clear is undoable ---- */
    {
        Rect r = L.act[A_CLEAR];
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.dialog == 1, "the clear button asks first");
        r = L.dlg_yes;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.dialog == 0 && canvas[0] == CANVAS_BG && canvas[CANVAS_PX - 1] == CANVAS_BG,
              "confirming clear empties the sheet");
        uichk(hist_undo() == 1 && canvas[(size_t)cy * CANVAS_W + cx] == RGB(st.r, st.g, st.b),
              "the clear is undoable");
    }

    /* ---- save and open are wired to the two file buttons ---- */
    {
        Rect r = L.act[A_SAVE];
        dirty = 0;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(strncmp(msg, "saved ", 6) == 0, "the save button writes a PNG");
        r = L.act[A_OPEN];
        dirty = 0;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(strncmp(msg, "opened ", 7) == 0, "the open button reads the newest PNG");
    }

    /* ---- the popover mixer in the narrow window ---- */
    {
        Rect r = L.chip;
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.mixer == 1, "the colour chip opens the mixer popover");
        layout_build(380, 280, &F, &st, &L);
        uichk(L.pop_on && L.sl[0].w > 0, "the popover has its three sliders");
        r = L.sl[0];
        press_at(&st, &L, r.x + r.w / 2, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.r >= 126 && st.r <= 129, "dragging the R slider sets the red channel");
        press_at(&st, &L, L.vw - 1, L.vh - 1, msg, sizeof(msg), &dirty);
        uichk(st.mixer == 0, "a click outside closes the popover");
    }

    /* ---- the wide window: the mixer is a panel ---- */
    layout_build(900, 600, &F, &st, &L);
    uichk(L.panel_on, "900x600: the mixer is a panel beside the sheet");
    if (L.panel_on) {
        Rect r = L.sl[2];
        press_at(&st, &L, r.x + r.w / 4, r.y + r.h / 2, msg, sizeof(msg), &dirty);
        uichk(st.b >= 62 && st.b <= 65, "the panel's B slider sets the blue channel");
    }

    /* ---- the truncation itself: an ellipsis, inside the box ---- */
    {
        char b[80];
        int box = (int)font_width(F.small, "undo");
        int cut = text_fit(b, sizeof(b), F.small,
                           "a label far too long for this column", box);
        uichk(cut && strstr(b, "\xE2\x80\xA6") != NULL &&
              font_width(F.small, b) <= (float)box,
              "text_fit cuts with an ellipsis that fits the box");
    }

    /* ---- hover is the same hit test ---- */
    {
        Rect r = L.tool[T_FILL];
        uichk(hit_code(&L, &st, r.x + r.w / 2, r.y + r.h / 2) == H_TOOL + T_FILL,
              "the hover test finds the same button");
    }

    printf("  input: %s (%d failures)\n", ui_fails ? "FAIL" : "PASS", ui_fails);
    return ui_fails;
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
