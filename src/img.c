/* ============================================================
 *  Mystic OS - img.c
 *
 *  The image decoder behind the API in img.h. One file, no
 *  dependencies beyond the C standard library, and no invented
 *  pixels: every sample comes from a Huffman code, a palette entry
 *  or a literal byte that the file actually stored.
 *
 *  Layout of this file
 *
 *    1. little helpers (big/little endian reads, errors, clamping)
 *    2. checksums (CRC-32 for PNG chunks, adler32 for zlib)
 *    3. DEFLATE (stored, fixed Huffman, dynamic Huffman)
 *    4. PNG (chunks, filters, bit depths, Adam7, tRNS)
 *    5. JPEG (parser, Huffman, baseline + progressive scans, IDCT,
 *             chroma upsampling, YCbCr -> RGB)
 *    6. BMP
 *    7. PNM / PBM
 *    8. img_decode / img_sniff / img_scale_fit / img_free
 *
 *  Two things are worth saying up front because they are the two
 *  mistakes this file is written not to make.
 *
 *  BOUNDS. The input is a byte buffer of known length and nothing
 *  else is promised. Every read is either checked against len (the
 *  parsers) or against a destination buffer whose size was derived
 *  from a dimension that was itself checked (the pixel loops). The
 *  deflate decoder refuses to write past the output size the PNG
 *  header predicted; the unfilter cannot read before the row or
 *  above the first row; the JPEG entropy reader stops at the end of
 *  the buffer and reports it instead of reading on.
 *
 *  HONESTY. A file that uses arithmetic coding, is lossless or is
 *  CMYK is not decoded approximately - it is refused with a message
 *  that names the coding. A palette index past the end of PLTE is an
 *  error, not magenta pixels. A JPEG that ends in the middle of an
 *  MCU is an error, not a half-filled picture. Callers can show the
 *  reason; they never show garbage that looks decoded.
 *
 *  All internal names begin with ic_ (types) or ic_ (functions) and
 *  are static, so this file can be compiled alone or #included into
 *  the browser's single translation unit without colliding.
 * ============================================================ */
#include "img.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------
 *  Limits
 *
 *  IMG_MAX_PIXELS keeps the output buffer at 64 MB. The compressed
 *  stream that expands into the PNG sample bytes is capped a little
 *  above the worst legal case (16-bit RGBA at the pixel cap is
 *  128 MB of samples plus one filter byte per row).
 * ------------------------------------------------------------ */
#define IC_MAX_RAW   ((uint64_t)256 * 1024 * 1024)
#define IC_MAX_PIXELS ((uint64_t)IMG_MAX_PIXELS)

/* ------------------------------------------------------------
 *  1. small helpers
 * ------------------------------------------------------------ */
static uint32_t ic_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t ic_be16(const uint8_t *p) {
    return (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static uint32_t ic_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t ic_le16(const uint8_t *p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static int ic_clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t ic_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return ((uint32_t)(a & 255u) << 24) | ((uint32_t)(r & 255u) << 16) |
           ((uint32_t)(g & 255u) << 8) | (uint32_t)(b & 255u);
}

/* Write a specific reason and return -1, so callers can
 *     return ic_err(err, esz, "...");
 * in one line. */
static int ic_err(char *err, size_t esz, const char *fmt, ...) {
    if (err && esz) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, esz, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ------------------------------------------------------------
 *  2. checksums
 *
 *  CRC-32 (reflected, polynomial 0xEDB88320) over chunk type +
 *  data, as PNG requires, and adler32 over the inflated bytes as
 *  zlib requires. The CRC table is built once per PNG decode, not
 *  kept in a global, so the library is reentrant.
 * ------------------------------------------------------------ */
static void ic_crc_build(uint32_t *tab) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        tab[n] = c;
    }
}

static uint32_t ic_crc32(const uint32_t *tab, const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t ic_adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i = 0;
    while (i < n) {
        size_t chunk = n - i;
        if (chunk > 5552u) chunk = 5552u;      /* the largest n for 32-bit math */
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

/* ============================================================
 *  3. DEFLATE (RFC 1951)
 *
 *  A complete inflater: stored blocks, fixed Huffman blocks and
 *  dynamic Huffman blocks. The Huffman decoder follows RFC 1951's
 *  canonical construction exactly: counts per code length, symbols
 *  sorted by (length, symbol), then walk the code one bit at a time
 *  and stop at the first length where the accumulated code lands in
 *  that length's interval. Because the code is rebuilt from the
 *  lengths every block, an over-subscribed table is detected rather
 *  than producing bogus symbols, and an invalid code is an error
 *  rather than an out-of-bounds read.
 *
 *  The output buffer is exactly the size the PNG IHDR predicted. A
 *  stream that tries to write one byte more is rejected, so a
 *  "compression bomb" cannot make this allocate more than the image
 *  header already justified.
 * ============================================================ */
typedef struct {
    const uint8_t *in;
    size_t len, pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t cap, n;
    int err;
    const char *why;
} IcInf;

typedef struct {
    short count[16];
    unsigned short sym[288];
} IcHuff;

static void ic_inf_fail(IcInf *z, const char *why) {
    if (!z->err) {
        z->err = 1;
        z->why = why;
    }
}

static int ic_inf_bits(IcInf *z, int n) {
    if (n <= 0) return 0;
    while (z->bitcnt < n) {
        if (z->pos >= z->len) {
            ic_inf_fail(z, "deflate stream ended in the middle of a code");
            return 0;
        }
        z->bitbuf |= (uint32_t)z->in[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & (((uint32_t)1 << n) - 1u));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

static void ic_inf_put(IcInf *z, uint8_t b) {
    if (z->n >= z->cap) {
        ic_inf_fail(z, "deflate stream expands to more bytes than the image header allows");
        return;
    }
    z->out[z->n++] = b;
}

/* 1 = complete code, 0 = legal but incomplete, -1 = over-subscribed. */
static int ic_huff_make(IcHuff *h, const uint8_t *lens, int n) {
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        h->count[lens[i]]++;
    }
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (unsigned short)i;
    return left == 0 ? 1 : 0;
}

static int ic_huff_dec(IcInf *z, const IcHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = ic_inf_bits(z, 1);
        if (z->err) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) {
            int at = index + (code - first);
            /* The canonical construction guarantees this is in range;
             * the check makes that a proof rather than an assumption. */
            if (at < 0 || at >= (int)(sizeof(h->sym) / sizeof(h->sym[0]))) {
                ic_inf_fail(z, "Huffman table index out of range");
                return -1;
            }
            return (int)h->sym[at];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    ic_inf_fail(z, "invalid Huffman code");
    return -1;
}

/* RFC 1951 3.2.5: length and distance codes. */
static const unsigned short IC_LBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t IC_LEXT[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short IC_DBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t IC_DEXT[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Build the two fixed tables of RFC 1951 3.2.6 into caller storage,
 * so there is no static "already built" flag to race on. */
static void ic_inf_fixed(IcHuff *lit, IcHuff *dist) {
    uint8_t l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    ic_huff_make(lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    ic_huff_make(dist, l, 30);
}

static int ic_inflate(IcInf *z) {
    int final = 0;
    IcHuff fixed_lit, fixed_dist;
    int fixed_ready = 0;

    while (!final && !z->err) {
        final = ic_inf_bits(z, 1);
        int type = ic_inf_bits(z, 2);
        if (z->err) break;

        if (type == 0) {                              /* stored block */
            z->bitbuf = 0;
            z->bitcnt = 0;
            if (z->pos + 4 > z->len) {
                ic_inf_fail(z, "stored block header is truncated");
                break;
            }
            unsigned len = (unsigned)z->in[z->pos] | ((unsigned)z->in[z->pos + 1] << 8);
            unsigned nlen = (unsigned)z->in[z->pos + 2] | ((unsigned)z->in[z->pos + 3] << 8);
            z->pos += 4;
            if ((len ^ 0xFFFFu) != nlen) {
                ic_inf_fail(z, "stored block length does not match its ones-complement");
                break;
            }
            if (len > z->len - z->pos) {
                ic_inf_fail(z, "stored block runs past the end of the stream");
                break;
            }
            for (unsigned i = 0; i < len; i++) ic_inf_put(z, z->in[z->pos + i]);
            z->pos += len;
            continue;
        }

        IcHuff *lit = NULL, *dist = NULL;

        if (type == 1) {                              /* fixed Huffman */
            if (!fixed_ready) {
                ic_inf_fixed(&fixed_lit, &fixed_dist);
                fixed_ready = 1;
            }
            lit = &fixed_lit;
            dist = &fixed_dist;
        } else if (type == 2) {                        /* dynamic Huffman */
            int hlit = ic_inf_bits(z, 5) + 257;
            int hdist = ic_inf_bits(z, 5) + 1;
            int hclen = ic_inf_bits(z, 4) + 4;
            if (z->err) break;
            if (hlit > 286 || hdist > 30) {
                ic_inf_fail(z, "dynamic Huffman table is larger than RFC 1951 allows");
                break;
            }

            static const uint8_t order[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            uint8_t cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)ic_inf_bits(z, 3);
            if (z->err) break;

            IcHuff ch;
            if (ic_huff_make(&ch, cl, 19) < 0) {
                ic_inf_fail(z, "code-length Huffman table is over-subscribed");
                break;
            }

            uint8_t lens[320];
            memset(lens, 0, sizeof(lens));
            int total = hlit + hdist;
            int n = 0;
            while (n < total) {
                int sym = ic_huff_dec(z, &ch);
                if (z->err || sym < 0) {
                    ic_inf_fail(z, "bad symbol in the code-length table");
                    break;
                }
                if (sym < 16) {
                    lens[n++] = (uint8_t)sym;
                } else {
                    int rep, val;
                    if (sym == 16) {
                        if (n == 0) {
                            ic_inf_fail(z, "repeat length with no previous length");
                            break;
                        }
                        rep = 3 + ic_inf_bits(z, 2);
                        val = lens[n - 1];
                    } else if (sym == 17) {
                        rep = 3 + ic_inf_bits(z, 3);
                        val = 0;
                    } else {
                        rep = 11 + ic_inf_bits(z, 7);
                        val = 0;
                    }
                    while (rep-- > 0) {
                        if (n >= total) {
                            ic_inf_fail(z, "code-length repeat runs past the table");
                            break;
                        }
                        lens[n++] = (uint8_t)val;
                    }
                }
                if (z->err) break;
            }
            if (z->err) break;

            IcHuff dl, dd;
            if (ic_huff_make(&dl, lens, hlit) < 0) {
                ic_inf_fail(z, "literal/length Huffman table is over-subscribed");
                break;
            }
            if (ic_huff_make(&dd, lens + hlit, hdist) < 0) {
                ic_inf_fail(z, "distance Huffman table is over-subscribed");
                break;
            }
            lit = &dl;
            dist = &dd;
        } else {
            ic_inf_fail(z, "reserved deflate block type 3");
            break;
        }

        for (;;) {
            int sym = ic_huff_dec(z, lit);
            if (z->err || sym < 0) break;
            if (sym < 256) {
                ic_inf_put(z, (uint8_t)sym);
                continue;
            }
            if (sym == 256) break;                     /* end of block */
            sym -= 257;
            if (sym >= 29) {
                ic_inf_fail(z, "invalid length symbol");
                break;
            }
            int len = IC_LBASE[sym] + ic_inf_bits(z, IC_LEXT[sym]);
            int dsym = ic_huff_dec(z, dist);
            if (z->err || dsym < 0) {
                ic_inf_fail(z, "bad distance code");
                break;
            }
            if (dsym >= 30) {
                ic_inf_fail(z, "invalid distance symbol");
                break;
            }
            size_t d = (size_t)IC_DBASE[dsym] + (size_t)ic_inf_bits(z, IC_DEXT[dsym]);
            if (z->err) break;
            if (d > z->n) {
                ic_inf_fail(z, "back-reference points before the start of the output");
                break;
            }
            for (int i = 0; i < len; i++)
                ic_inf_put(z, z->out[z->n - d]);
            if (z->err) break;
        }
    }
    return z->err ? -1 : 0;
}

/* ============================================================
 *  4. PNG
 *
 *  Chunk structure, CRC verification, zlib wrapper, the five row
 *  filters, every legal bit depth and colour type, PLTE/tRNS, and
 *  Adam7's seven passes.
 *
 *  Sample scaling is exact, not a shift: a bit depth d has range
 *  0..2^d-1 and is mapped to 0..255 with rounding, and 16-bit
 *  samples are mapped by round(v/257) because 65535 = 255*257.
 *  That is the mapping the PNG specification's "sample depth
 *  scaling" describes, and it means a 16-bit white (65535) is 255
 *  and an 8-bit white is 255 without either being special-cased.
 * ============================================================ */
static const uint8_t IC_PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static int ic_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undo one pass's row filters in place. Returns -1 and a specific
 * message on an unknown filter byte; bpp is the filter's
 * bytes-per-pixel, ceil(bits-per-pixel / 8), minimum 1. */
static int ic_unfilter(uint8_t *buf, int rows, size_t rowbytes, int bpp,
                       int pass, char *err, size_t esz) {
    const size_t rs = rowbytes + 1u;
    for (int y = 0; y < rows; y++) {
        uint8_t ft = buf[(size_t)y * rs];
        uint8_t *row = buf + (size_t)y * rs + 1u;
        const uint8_t *prev = y > 0 ? buf + (size_t)(y - 1) * rs + 1u : NULL;
        if (ft > 4)
            return ic_err(err, esz, "PNG row %d of pass %d uses filter type %u (only 0..4 exist)",
                          y, pass, (unsigned)ft);
        if (ft == 0) continue;
        for (size_t x = 0; x < rowbytes; x++) {
            int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
            switch (ft) {
            case 1: row[x] = (uint8_t)(row[x] + a); break;
            case 2: row[x] = (uint8_t)(row[x] + b); break;
            case 3: row[x] = (uint8_t)(row[x] + ((a + b) >> 1)); break;
            case 4: row[x] = (uint8_t)(row[x] + ic_paeth(a, b, c)); break;
            default: break;                            /* rejected above */
            }
        }
    }
    return 0;
}

/* Read the sample that starts at bit position bitpos. All callers
 * are inside a row whose byte length accounts for the last sample,
 * so the 16-bit read touches bytes that exist. */
static unsigned ic_sample(const uint8_t *row, size_t bitpos, int depth) {
    size_t i = bitpos >> 3;
    if (depth == 8) return row[i];
    if (depth == 16) return ((unsigned)row[i] << 8) | (unsigned)row[i + 1];
    int shift = 8 - depth - (int)(bitpos & 7u);
    return ((unsigned)row[i] >> shift) & ((1u << depth) - 1u);
}

static uint8_t ic_scale_sample(unsigned v, int depth) {
    if (depth == 16) return (uint8_t)((v + 128u) / 257u);   /* 65535 = 255*257 */
    unsigned mx = (1u << depth) - 1u;
    return (uint8_t)((v * 255u + mx / 2u) / mx);
}

static int ic_png_channels(int ctype) {
    switch (ctype) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

typedef struct { int x0, y0, dx, dy; } IcPass;

static const IcPass IC_ADAM7[7] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 }
};

/* Bytes of filtered scanlines the IDAT stream must expand to. */
static size_t ic_png_pass_bytes(int w, int h, int bitspp, int interlace) {
    size_t total = 0;
    if (!interlace) {
        size_t rb = ((size_t)w * (size_t)bitspp + 7u) / 8u;
        return (rb + 1u) * (size_t)h;
    }
    for (int p = 0; p < 7; p++) {
        int x0 = IC_ADAM7[p].x0, y0 = IC_ADAM7[p].y0;
        int dx = IC_ADAM7[p].dx, dy = IC_ADAM7[p].dy;
        if (w <= x0 || h <= y0) continue;
        int pw = (w - x0 + dx - 1) / dx;
        int ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = ((size_t)pw * (size_t)bitspp + 7u) / 8u;
        total += (rb + 1u) * (size_t)ph;
    }
    return total;
}

/* Convert one unfiltered pass into the destination image. Every
 * destination coordinate is computed from the pass geometry, so the
 * write is always inside dw x dh. */
static int ic_png_emit(const uint8_t *buf, int pw, int ph, int depth, int ctype,
                       int dw, int x0, int dxp, int y0, int dyp,
                       const uint8_t *pal, int pal_n,
                       const uint8_t *trns, int trns_len,
                       uint32_t *out, int *has_alpha,
                       char *err, size_t esz) {
    int ch = ic_png_channels(ctype);
    int bitspp = ch * depth;
    size_t rowbytes = ((size_t)pw * (size_t)bitspp + 7u) / 8u;

    for (int y = 0; y < ph; y++) {
        const uint8_t *row = buf + (size_t)y * (rowbytes + 1u) + 1u;
        int gy = y0 + y * dyp;
        for (int x = 0; x < pw; x++) {
            unsigned s[4] = { 0, 0, 0, 0 };
            size_t base = (size_t)x * (size_t)bitspp;
            for (int c = 0; c < ch; c++)
                s[c] = ic_sample(row, base + (size_t)c * (size_t)depth, depth);

            uint32_t c = 0;
            if (ctype == 0) {                          /* greyscale */
                uint8_t g = ic_scale_sample(s[0], depth);
                unsigned a = 255;
                if (trns_len >= 2) {
                    unsigned key = ic_be16(trns);
                    if (s[0] == key) a = 0;
                }
                c = ic_rgba(g, g, g, a);
            } else if (ctype == 2) {                   /* RGB */
                uint8_t r = ic_scale_sample(s[0], depth);
                uint8_t g = ic_scale_sample(s[1], depth);
                uint8_t b = ic_scale_sample(s[2], depth);
                unsigned a = 255;
                if (trns_len >= 6) {
                    if (s[0] == ic_be16(trns) && s[1] == ic_be16(trns + 2) &&
                        s[2] == ic_be16(trns + 4))
                        a = 0;
                }
                c = ic_rgba(r, g, b, a);
            } else if (ctype == 3) {                   /* palette */
                unsigned idx = s[0];
                if (idx >= (unsigned)pal_n)
                    return ic_err(err, esz,
                                  "PNG pixel (%d,%d) uses palette index %u but PLTE has %d entries",
                                  x0 + x * dxp, gy, idx, pal_n);
                unsigned a = 255;
                if (trns && idx < (unsigned)trns_len) a = trns[idx];
                c = ic_rgba(pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], a);
            } else if (ctype == 4) {                   /* grey + alpha */
                uint8_t g = ic_scale_sample(s[0], depth);
                c = ic_rgba(g, g, g, ic_scale_sample(s[1], depth));
            } else {                                   /* RGBA */
                c = ic_rgba(ic_scale_sample(s[0], depth), ic_scale_sample(s[1], depth),
                            ic_scale_sample(s[2], depth), ic_scale_sample(s[3], depth));
            }
            if ((c >> 24) != 255u) *has_alpha = 1;
            out[(size_t)gy * (size_t)dw + (size_t)(x0 + x * dxp)] = c;
        }
    }
    return 0;
}

static int ic_png_decode(const uint8_t *d, size_t n, Img *im,
                         char *err, size_t esz) {
    uint32_t crctab[256];
    ic_crc_build(crctab);

    if (n < 8 || memcmp(d, IC_PNG_SIG, 8) != 0)
        return ic_err(err, esz, "not a PNG (the 8-byte signature is missing)");

    int W = 0, H = 0, depth = 0, ctype = 0, interlace = 0;
    int saw_ihdr = 0, saw_iend = 0, saw_plte = 0;
    uint8_t pal[256 * 3];
    int pal_n = 0;
    uint8_t trns[256];
    int trns_len = 0;
    memset(pal, 0, sizeof(pal));
    memset(trns, 0, sizeof(trns));

    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    while (pos + 8 <= n) {
        uint32_t clen = ic_be32(d + pos);
        const uint8_t *type = d + pos + 4;
        if (clen > 0x7FFFFFFFu)
            return ic_err(err, esz, "PNG chunk at byte %lu claims %lu bytes, which is not a length",
                          (unsigned long)pos, (unsigned long)clen);
        if ((uint64_t)clen + 12u > (uint64_t)(n - pos)) {
            free(idat);
            return ic_err(err, esz, "PNG chunk at byte %lu is cut off (%lu of %lu bytes present)",
                          (unsigned long)pos, (unsigned long)(n - pos),
                          (unsigned long)((uint64_t)clen + 12u));
        }
        const uint8_t *data = d + pos + 8;
        uint32_t want_crc = ic_be32(data + clen);
        uint32_t got_crc = ic_crc32(crctab, type, (size_t)clen + 4u);
        if (want_crc != got_crc) {
            free(idat);
            return ic_err(err, esz, "PNG %.4s chunk CRC is %08lX, the bytes give %08lX",
                          (const char *)type, (unsigned long)want_crc,
                          (unsigned long)got_crc);
        }

        int critical = !(type[0] & 0x20);
        if (!memcmp(type, "IHDR", 4)) {
            if (saw_ihdr) { free(idat); return ic_err(err, esz, "PNG has more than one IHDR"); }
            if (clen != 13) { free(idat); return ic_err(err, esz, "PNG IHDR is %lu bytes, not 13", (unsigned long)clen); }
            W = (int)ic_be32(data);
            H = (int)ic_be32(data + 4);
            depth = data[8];
            ctype = data[9];
            if (data[10] != 0) { free(idat); return ic_err(err, esz, "PNG compression method %u is not 0 (deflate)", data[10]); }
            if (data[11] != 0) { free(idat); return ic_err(err, esz, "PNG filter method %u is not 0", data[11]); }
            interlace = data[12];
            if (interlace > 1) { free(idat); return ic_err(err, esz, "PNG interlace method %d is not defined (0 or 1)", interlace); }
            const char *cn = ctype == 0 ? "greyscale" : ctype == 2 ? "RGB" :
                             ctype == 3 ? "palette" : ctype == 4 ? "grey+alpha" :
                             ctype == 6 ? "RGBA" : NULL;
            if (!cn) { free(idat); return ic_err(err, esz, "PNG colour type %d is not defined", ctype); }
            int ok = 0;
            if (ctype == 0) ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16);
            else if (ctype == 3) ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8);
            else ok = (depth == 8 || depth == 16);
            if (!ok) {
                free(idat);
                return ic_err(err, esz, "PNG bit depth %d is not legal for %s (colour type %d)",
                              depth, cn, ctype);
            }
            if (W <= 0 || H <= 0 || W > IMG_MAX_DIM || H > IMG_MAX_DIM) {
                free(idat);
                return ic_err(err, esz, "PNG says %dx%d, outside the 1..%d limit", W, H, IMG_MAX_DIM);
            }
            if ((uint64_t)W * (uint64_t)H > IC_MAX_PIXELS) {
                free(idat);
                return ic_err(err, esz, "PNG says %dx%d = %llu pixels, above the %u-pixel limit",
                              W, H, (unsigned long long)W * (unsigned long long)H,
                              (unsigned)IMG_MAX_PIXELS);
            }
            saw_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (!saw_ihdr) { free(idat); return ic_err(err, esz, "PNG has PLTE before IHDR"); }
            if (clen == 0 || clen % 3u != 0 || clen > 256u * 3u) {
                free(idat);
                return ic_err(err, esz, "PNG PLTE is %lu bytes; it must be 3..768 and a multiple of 3",
                              (unsigned long)clen);
            }
            pal_n = (int)(clen / 3u);
            memcpy(pal, data, clen);
            saw_plte = 1;
        } else if (!memcmp(type, "tRNS", 4)) {
            if (!saw_ihdr) { free(idat); return ic_err(err, esz, "PNG has tRNS before IHDR"); }
            if (clen > sizeof(trns)) {
                free(idat);
                return ic_err(err, esz, "PNG tRNS is %lu bytes, more than the 256-entry palette",
                              (unsigned long)clen);
            }
            memcpy(trns, data, clen);
            trns_len = (int)clen;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!saw_ihdr) { free(idat); return ic_err(err, esz, "PNG has IDAT before IHDR"); }
            if (ctype == 3 && !saw_plte) { free(idat); return ic_err(err, esz, "PNG palette image has no PLTE before IDAT"); }
            if (idat_len + clen + 1u > idat_cap) {
                size_t cap = idat_cap ? idat_cap : 8192u;
                while (cap < idat_len + (size_t)clen + 1u) cap *= 2u;
                uint8_t *np = realloc(idat, cap);
                if (!np) { free(idat); return ic_err(err, esz, "out of memory collecting %lu IDAT bytes", (unsigned long)cap); }
                idat = np;
                idat_cap = cap;
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            saw_iend = 1;
        } else if (critical) {
            free(idat);
            return ic_err(err, esz, "PNG has unknown critical chunk %.4s", (const char *)type);
        }
        pos += 12u + (size_t)clen;
    }

    if (!saw_ihdr) { free(idat); return ic_err(err, esz, "PNG has no IHDR chunk"); }
    if (!saw_iend) { free(idat); return ic_err(err, esz, "PNG has no IEND chunk (the file is truncated)"); }
    if (ctype == 3 && pal_n == 0) { free(idat); return ic_err(err, esz, "PNG palette image has no PLTE chunk"); }
    if (idat_len < 6) { free(idat); return ic_err(err, esz, "PNG has no pixel data (IDAT is %lu bytes)", (unsigned long)idat_len); }

    /* zlib wrapper (RFC 1950) */
    uint8_t cmf = idat[0], flg = idat[1];
    if ((cmf & 0x0Fu) != 8) {
        free(idat);
        return ic_err(err, esz, "PNG zlib compression method %u is not 8 (deflate)", cmf & 0x0Fu);
    }
    if ((cmf >> 4) > 7u) { free(idat); return ic_err(err, esz, "PNG zlib window size code %u is invalid", cmf >> 4); }
    if ((((unsigned)cmf << 8) | flg) % 31u != 0) {
        free(idat);
        return ic_err(err, esz, "PNG zlib header check failed (CMF %02X, FLG %02X)", cmf, flg);
    }
    if (flg & 0x20u) { free(idat); return ic_err(err, esz, "PNG zlib stream needs a preset dictionary, which PNG forbids"); }

    int ch = ic_png_channels(ctype);
    int bitspp = ch * depth;
    size_t rawlen = ic_png_pass_bytes(W, H, bitspp, interlace);
    if (rawlen == 0 || (uint64_t)rawlen > IC_MAX_RAW) {
        free(idat);
        return ic_err(err, esz, "PNG needs %lu decompressed bytes, outside the %lu-byte cap",
                      (unsigned long)rawlen, (unsigned long)IC_MAX_RAW);
    }
    uint8_t *raw = malloc(rawlen);
    if (!raw) {
        free(idat);
        return ic_err(err, esz, "out of memory for %lu decompressed PNG bytes", (unsigned long)rawlen);
    }

    IcInf z;
    memset(&z, 0, sizeof(z));
    z.in = idat + 2;
    z.len = idat_len - 6;
    z.out = raw;
    z.cap = rawlen;
    if (ic_inflate(&z) != 0) {
        free(raw);
        free(idat);
        return ic_err(err, esz, "PNG pixel stream is corrupt: %s (at compressed byte %lu)",
                      z.why ? z.why : "invalid deflate data", (unsigned long)z.pos);
    }
    if (z.n != rawlen) {
        free(raw);
        free(idat);
        return ic_err(err, esz, "PNG pixel stream expands to %lu bytes, the header requires %lu",
                      (unsigned long)z.n, (unsigned long)rawlen);
    }
    uint32_t want_adler = ic_be32(idat + idat_len - 4);
    uint32_t got_adler = ic_adler32(raw, rawlen);
    if (want_adler != got_adler) {
        free(raw);
        free(idat);
        return ic_err(err, esz, "PNG adler32 of the pixel stream is %08lX, the data gives %08lX",
                      (unsigned long)want_adler, (unsigned long)got_adler);
    }
    free(idat);
    idat = NULL;

    uint32_t *pix = malloc((size_t)W * (size_t)H * sizeof(uint32_t));
    if (!pix) {
        free(raw);
        return ic_err(err, esz, "out of memory for %dx%d pixels (%lu bytes)",
                      W, H, (unsigned long)((size_t)W * (size_t)H * 4u));
    }

    int has_alpha = 0;
    int fbpp = (bitspp + 7) / 8;
    if (fbpp < 1) fbpp = 1;

    if (!interlace) {
        size_t rowbytes = ((size_t)W * (size_t)bitspp + 7u) / 8u;
        if (ic_unfilter(raw, H, rowbytes, fbpp, 0, err, esz) != 0) {
            free(raw);
            free(pix);
            return -1;
        }
        if (ic_png_emit(raw, W, H, depth, ctype, W, 0, 1, 0, 1,
                        pal, pal_n, trns, trns_len, pix, &has_alpha, err, esz) != 0) {
            free(raw);
            free(pix);
            return -1;
        }
    } else {
        size_t off = 0;
        for (int p = 0; p < 7; p++) {
            int x0 = IC_ADAM7[p].x0, y0 = IC_ADAM7[p].y0;
            int dx = IC_ADAM7[p].dx, dy = IC_ADAM7[p].dy;
            if (W <= x0 || H <= y0) continue;
            int pw = (W - x0 + dx - 1) / dx;
            int ph = (H - y0 + dy - 1) / dy;
            if (pw <= 0 || ph <= 0) continue;
            size_t rowbytes = ((size_t)pw * (size_t)bitspp + 7u) / 8u;
            if (ic_unfilter(raw + off, ph, rowbytes, fbpp, p + 1, err, esz) != 0) {
                free(raw);
                free(pix);
                return -1;
            }
            if (ic_png_emit(raw + off, pw, ph, depth, ctype, W, x0, dx, y0, dy,
                            pal, pal_n, trns, trns_len, pix, &has_alpha,
                            err, esz) != 0) {
                free(raw);
                free(pix);
                return -1;
            }
            off += (rowbytes + 1u) * (size_t)ph;
        }
    }
    free(raw);

    im->w = W;
    im->h = H;
    im->stride = W;
    im->px = pix;
    (void)has_alpha;   /* alpha is already in the pixels; nothing to report */
    return 0;
}

/* ============================================================
 *  5. JPEG
 *
 *  Baseline (SOF0), extended sequential (SOF1) and progressive
 *  (SOF2), Huffman only, 8-bit samples. The decoder is deliberately
 *  split the way the format is:
 *
 *    parser      walks markers, keeps up to four quantisation and
 *                four Huffman tables per class (DC/AC), records
 *                sampling factors and the Adobe/JFIF hints that say
 *                whether three components are RGB or YCbCr.
 *    entropy     one bit reader that also understands 0xFF00 as a
 *                data byte and FF D0..D7 as a restart marker. A
 *                marker in the middle of a block is an error, not a
 *                silent zero bit.
 *    scans       every block is decoded into that component's
 *                coefficient plane in zigzag order, so baseline and
 *                progressive share the reconstruction. Progressive
 *                needs the whole plane anyway: its refinement scans
 *                touch coefficients that earlier scans wrote.
 *    IDCT        a separable float inverse DCT built from the eight
 *                cos(k*pi/16) constants, with a DC-only shortcut for
 *                the common flat block and a clamp to 0..255.
 *    colour      YCbCr -> RGB in 16.16 fixed point (the same
 *                coefficients libjpeg uses), or RGB when the file
 *                says so through APP14 or component ids.
 *
 *  What is refused, by name: arithmetic coding (SOF9/10 and the DAC
 *  segment), lossless (SOF3), hierarchical/differential (SOF5..7,
 *  DHP/EXP), 12-bit samples, and 4-component CMYK/YCCK. Those are
 *  formats, not corruptions: decoding them with the wrong algorithm
 *  would produce a picture that looks plausible and is wrong, which
 *  is worse than saying so.
 * ============================================================ */

#define IC_JMAX_COMPS 4
#define IC_JMAX_TABLES 4

typedef struct {
    int id;
    int h, v;                 /* sampling factors                       */
    int tq;                   /* quantisation table                     */
    int16_t *coef;            /* bw*bh*64 coefficients, zigzag order    */
    int bw, bh;               /* coefficient blocks                     */
    int pw, ph;               /* plane size in samples (bw*8 x bh*8)    */
    uint8_t *plane;           /* reconstructed samples, 0..255          */
    int dc_pred;              /* predictor for the current scan         */
} IcJComp;

typedef struct {
    uint8_t bits[17];         /* number of codes of each length 1..16   */
    uint8_t vals[256];
    int mincode[17], maxcode[17], valptr[17];
    int defined;
} IcJHuff;

typedef struct {
    const uint8_t *d;
    size_t len, pos;

    /* entropy cursor, used while a scan is being decoded */
    const uint8_t *ep, *eend;
    uint32_t bitbuf;
    int bitcnt;
    int pending;              /* marker code found in the entropy stream */
    int eof;

    int width, height;
    int ncomp, hmax, vmax, prog;
    IcJComp comp[IC_JMAX_COMPS];

    uint16_t qt[IC_JMAX_TABLES][64];   /* zigzag order, as stored        */
    int qt_defined[IC_JMAX_TABLES];
    IcJHuff huff[2][IC_JMAX_TABLES];   /* [0] DC, [1] AC                 */

    int adobe, adobe_transform, jfif;
    int ri;                            /* restart interval in MCUs       */

    /* current scan */
    int ss, se, ah, al;
    int eob_run;
    int cur_td[IC_JMAX_COMPS], cur_ta[IC_JMAX_COMPS];

    int err_set;
    char *err;
    size_t esz;
} IcJpeg;

/* The zigzag sequence: index in the entropy stream -> natural
 * position (row*8 + column). */
static const uint8_t IC_ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Set the error message once; later, less specific messages must not
 * overwrite the first real reason. */
static int ic_jerr(IcJpeg *j, const char *fmt, ...) {
    if (!j->err_set) {
        j->err_set = 1;
        if (j->err && j->esz) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(j->err, j->esz, fmt, ap);
            va_end(ap);
        }
    }
    return -1;
}

/* The message for "the entropy stream stopped early", naming the
 * marker when that is what stopped it. */
static int ic_jentropy_err(IcJpeg *j, const char *where) {
    if (j->pending)
        return ic_jerr(j, "JPEG marker FF%02X appeared in the middle of %s", j->pending, where);
    return ic_jerr(j, "JPEG entropy data ended in the middle of %s", where);
}

/* ------------------------------------------------------------
 *  entropy reader
 *
 *  0xFF00 is the data byte 0xFF. Any other 0xFF xx is a marker: the
 *  reader consumes it, records it in j->pending and stops supplying
 *  bits. The scan loop checks pending at MCU boundaries (where a
 *  restart marker belongs) and the main loop consumes it as the
 *  marker that ended the scan.
 * ------------------------------------------------------------ */
static int ic_jbyte(IcJpeg *j) {
    if (j->pending || j->eof) return -1;
    if (j->ep >= j->eend) { j->eof = 1; return -1; }
    int c = *j->ep++;
    if (c != 0xFF) return c;
    while (j->ep < j->eend && *j->ep == 0xFF) j->ep++;
    if (j->ep >= j->eend) { j->eof = 1; return -1; }
    int m = *j->ep++;
    if (m == 0) return 0xFF;
    j->pending = m;
    return -1;
}

static int ic_jbits(IcJpeg *j, int n) {
    if (n <= 0) return 0;
    while (j->bitcnt < n) {
        int c = ic_jbyte(j);
        if (c < 0) { j->eof = 1; return 0; }
        j->bitbuf = (j->bitbuf << 8) | (uint32_t)c;
        j->bitcnt += 8;
    }
    j->bitcnt -= n;
    return (int)((j->bitbuf >> j->bitcnt) & (((uint32_t)1 << n) - 1u));
}

static int ic_jbit(IcJpeg *j) {
    return ic_jbits(j, 1);
}

static int ic_jextend(int v, int n) {
    if (n == 0) return 0;
    if (v < (1 << (n - 1))) v -= (1 << n) - 1;
    return v;
}

/* Build the canonical code tables from the 16 counts. Returns -1
 * when the counts describe an over-subscribed code, 0 when legal. */
static int ic_jhuff_build(IcJHuff *h, const uint8_t *counts) {
    int code = 0, k = 0, left = 1;
    memset(h->mincode, 0, sizeof(h->mincode));
    memset(h->maxcode, 0, sizeof(h->maxcode));
    memset(h->valptr, 0, sizeof(h->valptr));
    for (int l = 1; l <= 16; l++) {
        int c = counts[l - 1];
        h->bits[l] = (uint8_t)c;
        if (c) {
            h->valptr[l] = k;
            h->mincode[l] = code;
            code += c;
            k += c;
            h->maxcode[l] = code - 1;
        } else {
            h->maxcode[l] = -1;
        }
        code <<= 1;
        left <<= 1;
        left -= c;
        if (left < 0) return -1;
    }
    return 0;
}

static int ic_jhuff_dec(IcJpeg *j, const IcJHuff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | ic_jbits(j, 1);
        if (j->eof) return -1;
        if (h->maxcode[l] >= 0 && code >= h->mincode[l] && code <= h->maxcode[l]) {
            int at = h->valptr[l] + code - h->mincode[l];
            if (at < 0 || at > 255) {
                ic_jerr(j, "JPEG Huffman table index is out of range");
                return -1;
            }
            return h->vals[at];
        }
    }
    ic_jerr(j, "invalid Huffman code in the JPEG entropy data");
    return -1;
}

/* ------------------------------------------------------------
 *  block decoding
 *
 *  blk is in zigzag order. For baseline every coefficient is
 *  written; for progressive a scan writes only its own band, which
 *  is why the planes start zeroed.
 * ------------------------------------------------------------ */
static int ic_jblock_baseline(IcJpeg *j, IcJComp *c, int16_t *blk, int td, int ta) {
    const IcJHuff *dc = &j->huff[0][td];
    const IcJHuff *ac = &j->huff[1][ta];
    if (!dc->defined) return ic_jerr(j, "JPEG scan uses DC table %d, which the file never defined", td);
    if (!ac->defined) return ic_jerr(j, "JPEG scan uses AC table %d, which the file never defined", ta);

    int t = ic_jhuff_dec(j, dc);
    if (t < 0) {
        if (j->eof || j->pending) return ic_jentropy_err(j, "a block");
        return -1;
    }
    if (t > 11) return ic_jerr(j, "JPEG DC magnitude category %d is not legal for 8-bit samples", t);
    int diff = t ? ic_jextend(ic_jbits(j, t), t) : 0;
    if (j->eof) return ic_jentropy_err(j, "a DC coefficient");
    c->dc_pred += diff;
    if (c->dc_pred < -32768 || c->dc_pred > 32767)
        return ic_jerr(j, "JPEG DC predictor left the 16-bit range (corrupt stream)");
    blk[0] = (int16_t)c->dc_pred;

    int k = 1;
    while (k < 64) {
        int rs = ic_jhuff_dec(j, ac);
        if (rs < 0) {
            if (j->eof || j->pending) return ic_jentropy_err(j, "an AC run");
            return -1;
        }
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;                       /* end of block */
            k += 16;
            if (k > 64) return ic_jerr(j, "JPEG zero run passes the end of the block");
            continue;
        }
        if (s > 10) return ic_jerr(j, "JPEG AC magnitude category %d is not legal for 8-bit samples", s);
        k += r;
        if (k > 63) return ic_jerr(j, "JPEG AC run passes coefficient 63");
        int v = ic_jextend(ic_jbits(j, s), s);
        if (j->eof) return ic_jentropy_err(j, "an AC coefficient");
        blk[k] = (int16_t)v;
        k++;
    }
    return 0;
}

static int ic_jblock_dc_first(IcJpeg *j, IcJComp *c, int16_t *blk, int td) {
    const IcJHuff *dc = &j->huff[0][td];
    if (!dc->defined) return ic_jerr(j, "JPEG scan uses DC table %d, which the file never defined", td);
    int t = ic_jhuff_dec(j, dc);
    if (t < 0) {
        if (j->eof || j->pending) return ic_jentropy_err(j, "a block");
        return -1;
    }
    if (t > 11) return ic_jerr(j, "JPEG DC magnitude category %d is not legal for 8-bit samples", t);
    int diff = t ? ic_jextend(ic_jbits(j, t), t) : 0;
    if (j->eof) return ic_jentropy_err(j, "a DC coefficient");
    c->dc_pred += diff;
    if (c->dc_pred < -32768 || c->dc_pred > 32767)
        return ic_jerr(j, "JPEG DC predictor left the 16-bit range (corrupt stream)");
    blk[0] = (int16_t)(c->dc_pred * (1 << j->al));
    return 0;
}

static int ic_jblock_dc_refine(IcJpeg *j, int16_t *blk) {
    if (ic_jbit(j)) {
        if (j->eof) return ic_jentropy_err(j, "a DC refinement bit");
        blk[0] |= (int16_t)(1 << j->al);
    }
    return 0;
}

static int ic_jblock_ac_first(IcJpeg *j, int16_t *blk, int ta) {
    const IcJHuff *ac = &j->huff[1][ta];
    if (!ac->defined) return ic_jerr(j, "JPEG scan uses AC table %d, which the file never defined", ta);

    if (j->eob_run > 0) { j->eob_run--; return 0; }

    for (int k = j->ss; k <= j->se; k++) {
        int rs = ic_jhuff_dec(j, ac);
        if (rs < 0) {
            if (j->eof || j->pending) return ic_jentropy_err(j, "an AC run");
            return -1;
        }
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r < 15) {
                j->eob_run = (1 << r) - 1;
                if (r) j->eob_run += ic_jbits(j, r);
                if (j->eof) return ic_jentropy_err(j, "an end-of-band run");
                break;
            }
            k += 15;                                   /* 16 zeroes */
            continue;
        }
        if (s > 10) return ic_jerr(j, "JPEG AC magnitude category %d is not legal for 8-bit samples", s);
        k += r;
        if (k > j->se) return ic_jerr(j, "JPEG AC run passes the end of the spectral band");
        int v = ic_jextend(ic_jbits(j, s), s);
        if (j->eof) return ic_jentropy_err(j, "an AC coefficient");
        blk[k] = (int16_t)(v * (1 << j->al));
    }
    return 0;
}

static int ic_jblock_ac_refine(IcJpeg *j, int16_t *blk, int ta) {
    const IcJHuff *ac = &j->huff[1][ta];
    if (!ac->defined) return ic_jerr(j, "JPEG scan uses AC table %d, which the file never defined", ta);

    int p1 = 1 << j->al;
    int m1 = -p1;
    int k = j->ss;

    if (j->eob_run == 0) {
        for (; k <= j->se; k++) {
            int rs = ic_jhuff_dec(j, ac);
            if (rs < 0) {
                if (j->eof || j->pending) return ic_jentropy_err(j, "an AC refinement run");
                return -1;
            }
            int r = rs >> 4, s = rs & 15;
            if (s) {
                if (s != 1)
                    return ic_jerr(j, "JPEG AC refinement gives magnitude category %d; only 1 is legal", s);
                s = ic_jbit(j) ? p1 : m1;
                if (j->eof) return ic_jentropy_err(j, "an AC refinement sign");
            } else if (r != 15) {
                j->eob_run = 1 << r;
                if (r) j->eob_run += ic_jbits(j, r);
                if (j->eof) return ic_jentropy_err(j, "an end-of-band run");
                break;
            }
            /* Walk r still-zero coefficients, appending one correction
             * bit to every already-nonzero one on the way. */
            do {
                if (blk[k] != 0) {
                    int b = ic_jbit(j);
                    if (j->eof) return ic_jentropy_err(j, "a refinement bit");
                    if (b && (blk[k] & p1) == 0)
                        blk[k] = (int16_t)(blk[k] + (blk[k] >= 0 ? p1 : m1));
                } else if (--r < 0) {
                    break;
                }
                k++;
            } while (k <= j->se);
            if (k > j->se)
                return ic_jerr(j, "JPEG AC refinement run passes the end of the spectral band");
            if (s) blk[k] = (int16_t)s;
        }
    }

    if (j->eob_run > 0) {
        for (; k <= j->se; k++) {
            if (blk[k] != 0) {
                int b = ic_jbit(j);
                if (j->eof) return ic_jentropy_err(j, "a refinement bit");
                if (b && (blk[k] & p1) == 0)
                    blk[k] = (int16_t)(blk[k] + (blk[k] >= 0 ? p1 : m1));
            }
        }
        j->eob_run--;
    }
    return 0;
}

/* ------------------------------------------------------------
 *  scan decoding
 *
 *  Interleaved scans walk MCUs (h*v blocks per component);
 *  non-interleaved scans walk the named component's own block grid.
 *  Blocks that fall outside a component's grid exist only as padding
 *  in an interleaved scan; they are decoded into a scratch block so
 *  the entropy stream stays in step and then thrown away.
 * ------------------------------------------------------------ */
static int ic_jrestart(IcJpeg *j) {
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->eob_run = 0;
    if (j->pending >= 0xD0 && j->pending <= 0xD7) { j->pending = 0; return 0; }
    if (j->pending) return ic_jerr(j, "JPEG restart interval expected RSTn but found marker FF%02X", j->pending);
    while (j->ep < j->eend) {
        if (*j->ep++ != 0xFF) continue;
        while (j->ep < j->eend && *j->ep == 0xFF) j->ep++;
        if (j->ep >= j->eend) break;
        int m = *j->ep++;
        if (m >= 0xD0 && m <= 0xD7) return 0;
        if (m != 0) { j->pending = m; break; }
    }
    return ic_jerr(j, "JPEG restart interval is missing its RST marker");
}

static int ic_jdecode_scan(IcJpeg *j, int ns, int single_ci) {
    int64_t total;
    int mcux = 0, mcuy = 0;

    if (ns == 1) {
        IcJComp *c = &j->comp[single_ci];
        total = (int64_t)c->bw * (int64_t)c->bh;
    } else {
        mcux = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
        mcuy = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
        total = (int64_t)mcux * (int64_t)mcuy;
    }
    if (total <= 0) return ic_jerr(j, "JPEG scan has no blocks to decode");

    int16_t dummy[64];
    memset(dummy, 0, sizeof(dummy));

    for (int64_t m = 0; m < total; m++) {
        if (j->ri && m > 0 && (m % j->ri) == 0) {
            if (ic_jrestart(j) != 0) return -1;
            for (int ci = 0; ci < j->ncomp; ci++) j->comp[ci].dc_pred = 0;
        }
        if (j->err_set) return -1;

        if (ns == 1) {
            IcJComp *c = &j->comp[single_ci];
            int bx = (int)(m % c->bw);
            int by = (int)(m / c->bw);
            int16_t *blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64u;
            int rc;
            if (!j->prog) rc = ic_jblock_baseline(j, c, blk, j->cur_td[0], j->cur_ta[0]);
            else if (j->ss == 0 && j->ah == 0) rc = ic_jblock_dc_first(j, c, blk, j->cur_td[0]);
            else if (j->ss == 0) rc = ic_jblock_dc_refine(j, blk);
            else if (j->ah == 0) rc = ic_jblock_ac_first(j, blk, j->cur_ta[0]);
            else rc = ic_jblock_ac_refine(j, blk, j->cur_ta[0]);
            if (rc != 0) return rc;
        } else {
            int mx = (int)(m % mcux);
            int my = (int)(m / mcux);
            for (int s = 0; s < ns; s++) {
                IcJComp *c = &j->comp[s];
                for (int vy = 0; vy < c->v; vy++) {
                    for (int hx = 0; hx < c->h; hx++) {
                        int bx = mx * c->h + hx;
                        int by = my * c->v + vy;
                        int inside = bx < c->bw && by < c->bh;
                        int16_t *blk;
                        if (inside) {
                            blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64u;
                        } else {
                            memset(dummy, 0, sizeof(dummy));
                            blk = dummy;
                        }
                        int rc;
                        if (!j->prog) rc = ic_jblock_baseline(j, c, blk, j->cur_td[s], j->cur_ta[s]);
                        else if (j->ss == 0 && j->ah == 0) rc = ic_jblock_dc_first(j, c, blk, j->cur_td[s]);
                        else if (j->ss == 0) rc = ic_jblock_dc_refine(j, blk);
                        else if (j->ah == 0) rc = ic_jblock_ac_first(j, blk, j->cur_ta[s]);
                        else rc = ic_jblock_ac_refine(j, blk, j->cur_ta[s]);
                        if (rc != 0) return rc;
                    }
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------
 *  inverse DCT
 *
 *  f(x,y) = sum_u sum_v A(u,x) A(v,y) F(u,v) with
 *  A(0,x) = 1/(2*sqrt2) and A(u,x) = cos((2x+1)u*pi/16)/2. The eight
 *  cosines below are the exact mathematical constants; nothing here
 *  is measured or guessed.
 * ------------------------------------------------------------ */
static void ic_jidct_setup(float a[8][8]) {
    static const double base[9] = {
        1.0,
        0.9807852804032304, 0.9238795325112867, 0.8314696123025452,
        0.7071067811865476, 0.5555702330196022, 0.3826834323650898,
        0.1950903220161283, 0.0
    };
    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            int k = ((2 * x + 1) * u) & 31;
            int neg = 0;
            double v;
            if (k >= 16) { k -= 16; neg = 1; }
            if (k == 0) v = 1.0;
            else if (k <= 8) v = base[k];
            else v = -base[16 - k];
            if (neg) v = -v;
            a[u][x] = (float)(u == 0 ? v * 0.3535533905932738 : v * 0.5);
        }
    }
}

static void ic_jidct_block(const int16_t *coef, const uint16_t *qt,
                           const float a[8][8], uint8_t *dst, size_t stride) {
    int dc_only = 1;
    for (int k = 1; k < 64; k++)
        if (coef[k] != 0) { dc_only = 0; break; }

    if (dc_only) {
        int v = ic_clamp255((int)((float)coef[0] * (float)qt[0] / 8.0f + 128.5f));
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                dst[(size_t)y * stride + (size_t)x] = (uint8_t)v;
        return;
    }

    float blk[64];
    for (int k = 0; k < 64; k++)
        blk[IC_ZIGZAG[k]] = (float)coef[k] * (float)qt[k];

    float tmp[64];                                /* tmp[v][x] */
    for (int v = 0; v < 8; v++) {
        for (int x = 0; x < 8; x++) {
            float s = 0.0f;
            for (int u = 0; u < 8; u++)
                s += a[u][x] * blk[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float s = 0.0f;
            for (int v = 0; v < 8; v++)
                s += a[v][y] * tmp[v * 8 + x];
            int iv = (int)(s + (s >= 0.0f ? 128.5f : 127.5f));
            dst[(size_t)y * stride + (size_t)x] = (uint8_t)ic_clamp255(iv);
        }
    }
}

/* ------------------------------------------------------------
 *  marker segment parsers
 * ------------------------------------------------------------ */
static int ic_jparse_dqt(IcJpeg *j, const uint8_t *p, int l) {
    int at = 0;
    while (at < l) {
        int pq = p[at] >> 4, tq = p[at] & 15;
        at++;
        if (pq > 1) return ic_jerr(j, "JPEG quantisation table %d has precision %d (0 or 1)", tq, pq);
        if (tq >= IC_JMAX_TABLES) return ic_jerr(j, "JPEG quantisation table id %d is above 3", tq);
        int need = pq ? 128 : 64;
        if (l - at < need) return ic_jerr(j, "JPEG DQT segment is cut off in table %d", tq);
        for (int k = 0; k < 64; k++)
            j->qt[tq][k] = pq ? ic_be16(p + at + k * 2) : (uint16_t)p[at + k];
        at += need;
        j->qt_defined[tq] = 1;
    }
    return 0;
}

static int ic_jparse_dht(IcJpeg *j, const uint8_t *p, int l) {
    int at = 0;
    while (at < l) {
        if (l - at < 17) return ic_jerr(j, "JPEG DHT segment is cut off before its 16 counts");
        int tc = p[at] >> 4, th = p[at] & 15;
        at++;
        if (tc > 1) return ic_jerr(j, "JPEG Huffman table class %d is not 0 (DC) or 1 (AC)", tc);
        if (th >= IC_JMAX_TABLES) return ic_jerr(j, "JPEG Huffman table id %d is above 3", th);
        const uint8_t *counts = p + at;
        at += 16;
        int nvals = 0;
        for (int i = 0; i < 16; i++) nvals += counts[i];
        if (nvals > 256) return ic_jerr(j, "JPEG Huffman table %d has %d symbols (at most 256)", th, nvals);
        if (l - at < nvals) return ic_jerr(j, "JPEG DHT segment is cut off inside table %d", th);
        IcJHuff *h = &j->huff[tc][th];
        memset(h, 0, sizeof(*h));
        if (ic_jhuff_build(h, counts) != 0)
            return ic_jerr(j, "JPEG Huffman table %s%d is over-subscribed", tc ? "AC" : "DC", th);
        memcpy(h->vals, p + at, (size_t)nvals);
        h->defined = 1;
        at += nvals;
    }
    return 0;
}

static int ic_jfree_coefs(IcJpeg *j) {
    for (int i = 0; i < IC_JMAX_COMPS; i++) {
        free(j->comp[i].coef);
        j->comp[i].coef = NULL;
    }
    return 0;
}

static int ic_jparse_sof(IcJpeg *j, const uint8_t *p, int l, int marker) {
    if (j->ncomp) return ic_jerr(j, "JPEG has a second frame header (FF%02X)", marker);
    if (l < 6) return ic_jerr(j, "JPEG frame header is cut off");
    int prec = p[0];
    int h = ic_be16(p + 1);
    int w = ic_be16(p + 3);
    int nc = p[5];
    if (prec != 8) return ic_jerr(j, "JPEG has %d-bit samples; only 8-bit is supported", prec);
    if (nc == 4)
        return ic_jerr(j, "JPEG has 4 components (CMYK/YCCK); this decoder does not claim that colour transform");
    if (nc == 2)
        return ic_jerr(j, "JPEG has 2 components; only greyscale and YCbCr/RGB are supported");
    if (nc != 1 && nc != 3) return ic_jerr(j, "JPEG has %d components (1 or 3 expected)", nc);
    if (l < 6 + 3 * nc) return ic_jerr(j, "JPEG frame header is cut off in its component list");
    if (w <= 0 || h <= 0) return ic_jerr(j, "JPEG says %dx%d", w, h);
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return ic_jerr(j, "JPEG says %dx%d, outside the 1..%d limit", w, h, IMG_MAX_DIM);
    if ((uint64_t)w * (uint64_t)h > IC_MAX_PIXELS)
        return ic_jerr(j, "JPEG says %dx%d = %llu pixels, above the %u-pixel limit",
                       w, h, (unsigned long long)w * (unsigned long long)h,
                       (unsigned)IMG_MAX_PIXELS);

    j->width = w;
    j->height = h;
    j->ncomp = nc;
    j->hmax = 1;
    j->vmax = 1;
    for (int i = 0; i < nc; i++) {
        IcJComp *c = &j->comp[i];
        c->id = p[6 + 3 * i];
        int hv = p[7 + 3 * i];
        c->h = hv >> 4;
        c->v = hv & 15;
        c->tq = p[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return ic_jerr(j, "JPEG component %d has sampling factors %dx%d (1..4 each)", c->id, c->h, c->v);
        if (c->tq >= IC_JMAX_TABLES)
            return ic_jerr(j, "JPEG component %d uses quantisation table %d (above 3)", c->id, c->tq);
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
        for (int k = 0; k < i; k++)
            if (j->comp[k].id == c->id)
                return ic_jerr(j, "JPEG has two components with id %d", c->id);
    }

    /* Geometry, and the coefficient planes that scans will fill. */
    uint64_t blocks = 0;
    for (int i = 0; i < nc; i++) {
        IcJComp *c = &j->comp[i];
        int cw = (w * c->h + j->hmax - 1) / j->hmax;
        int chh = (h * c->v + j->vmax - 1) / j->vmax;
        c->bw = (cw + 7) / 8;
        c->bh = (chh + 7) / 8;
        c->pw = c->bw * 8;
        c->ph = c->bh * 8;
        if (c->bw <= 0 || c->bh <= 0) return ic_jerr(j, "JPEG component %d has no blocks", c->id);
        blocks += (uint64_t)c->bw * (uint64_t)c->bh;
    }
    if (blocks == 0 || blocks > (uint64_t)IC_MAX_PIXELS)
        return ic_jerr(j, "JPEG block count %llu is out of range", (unsigned long long)blocks);
    if (blocks * 64u * sizeof(int16_t) > (uint64_t)128 * 1024 * 1024)
        return ic_jerr(j, "JPEG needs %llu bytes of coefficients, above the 128 MB cap",
                       (unsigned long long)(blocks * 64u * sizeof(int16_t)));

    for (int i = 0; i < nc; i++) {
        IcJComp *c = &j->comp[i];
        c->coef = calloc((size_t)c->bw * (size_t)c->bh * 64u, sizeof(int16_t));
        if (!c->coef) {
            ic_jfree_coefs(j);
            return ic_jerr(j, "out of memory for %dx%d JPEG coefficients", c->bw * 8, c->bh * 8);
        }
    }
    return 0;
}

/* ------------------------------------------------------------
 *  the marker loop
 * ------------------------------------------------------------ */
static int ic_jmarker(IcJpeg *j) {
    if (j->pending) {
        int m = j->pending;
        j->pending = 0;
        return m;
    }
    for (;;) {
        if (j->pos >= j->len) return -1;
        if (j->d[j->pos] != 0xFF) { j->pos++; continue; }
        while (j->pos < j->len && j->d[j->pos] == 0xFF) j->pos++;
        if (j->pos >= j->len) return -1;
        int m = j->d[j->pos++];
        if (m == 0) continue;
        return m;
    }
}

/* ------------------------------------------------------------
 *  reconstruction and colour
 * ------------------------------------------------------------ */
static int ic_jsample(const IcJComp *c, int hmax, int vmax, int x, int y) {
    int sx = (int)(((int64_t)x * (int64_t)c->h) / (int64_t)hmax);
    int sy = (int)(((int64_t)y * (int64_t)c->v) / (int64_t)vmax);
    if (sx >= c->pw) sx = c->pw - 1;
    if (sy >= c->ph) sy = c->ph - 1;
    return c->plane[(size_t)sy * (size_t)c->pw + (size_t)sx];
}

static int ic_jpeg_reconstruct(IcJpeg *j, Img *im, char *err, size_t esz) {
    float a[8][8];
    ic_jidct_setup(a);

    for (int ci = 0; ci < j->ncomp; ci++) {
        IcJComp *c = &j->comp[ci];
        if (!j->qt_defined[c->tq])
            return ic_err(err, esz, "JPEG component %d uses quantisation table %d, which the file never defined",
                          c->id, c->tq);
        if (!c->coef)
            return ic_err(err, esz, "JPEG component %d has no coefficient plane", c->id);
        c->plane = malloc((size_t)c->pw * (size_t)c->ph);
        if (!c->plane)
            return ic_err(err, esz, "out of memory for %dx%d JPEG component samples", c->pw, c->ph);
        for (int by = 0; by < c->bh; by++) {
            for (int bx = 0; bx < c->bw; bx++) {
                const int16_t *blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64u;
                ic_jidct_block(blk, j->qt[c->tq], a,
                               c->plane + (size_t)by * 8u * (size_t)c->pw + (size_t)bx * 8u,
                               (size_t)c->pw);
            }
        }
        free(c->coef);
        c->coef = NULL;
    }

    size_t npix = (size_t)j->width * (size_t)j->height;
    uint32_t *pix = malloc(npix * sizeof(uint32_t));
    if (!pix) return ic_err(err, esz, "out of memory for %dx%d pixels", j->width, j->height);

    int rgb_mode = 0;
    if (j->ncomp == 3) {
        if (j->adobe) {
            rgb_mode = (j->adobe_transform == 0);
        } else if (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B') {
            rgb_mode = 1;
        }
    }

    for (int y = 0; y < j->height; y++) {
        uint32_t *row = pix + (size_t)y * (size_t)j->width;
        for (int x = 0; x < j->width; x++) {
            int r, g, b;
            int yv = ic_jsample(&j->comp[0], j->hmax, j->vmax, x, y);
            if (j->ncomp == 1) {
                r = g = b = yv;
            } else if (rgb_mode) {
                r = yv;
                g = ic_jsample(&j->comp[1], j->hmax, j->vmax, x, y);
                b = ic_jsample(&j->comp[2], j->hmax, j->vmax, x, y);
            } else {
                int cb = ic_jsample(&j->comp[1], j->hmax, j->vmax, x, y) - 128;
                int cr = ic_jsample(&j->comp[2], j->hmax, j->vmax, x, y) - 128;
                r = ic_clamp255(yv + ((91881 * cr + 32768) >> 16));
                g = ic_clamp255(yv + ((-22554 * cb - 46802 * cr + 32768) >> 16));
                b = ic_clamp255(yv + ((116130 * cb + 32768) >> 16));
            }
            row[x] = ic_rgba(r, g, b, 255);
        }
    }

    im->w = j->width;
    im->h = j->height;
    im->stride = j->width;
    im->px = pix;
    return 0;
}

static int ic_jpeg_run(IcJpeg *j, Img *im, char *err, size_t esz) {
    int scans = 0;

    while (1) {
        int m = ic_jmarker(j);
        if (m < 0) {
            if (j->err_set) return -1;
            return ic_err(err, esz, "JPEG ends without an EOI marker (the file is truncated)");
        }
        if (m == 0xD9) break;                          /* EOI */
        if (m == 0xD8) continue;                       /* stray SOI */
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;

        if (m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF)
            return ic_err(err, esz, "JPEG marker FF%02X is lossless coding, which this decoder does not support", m);
        if (m == 0xC5 || m == 0xC6 || m == 0xCD || m == 0xCE)
            return ic_err(err, esz, "JPEG marker FF%02X is hierarchical/differential coding, which this decoder does not support", m);
        if (m == 0xC9 || m == 0xCA || m == 0xCC)
            return ic_err(err, esz, "JPEG uses arithmetic coding (marker FF%02X); only Huffman JPEGs are supported", m);
        if (m == 0xDE || m == 0xDF)
            return ic_err(err, esz, "JPEG marker FF%02X belongs to hierarchical coding, which this decoder does not support", m);
        if (m == 0xC8)
            return ic_err(err, esz, "JPEG marker FFC8 is reserved, not a frame header");

        if (j->pos + 2 > j->len)
            return ic_err(err, esz, "JPEG marker FF%02X is cut off before its length", m);
        int seglen = ic_be16(j->d + j->pos);
        if (seglen < 2)
            return ic_err(err, esz, "JPEG marker FF%02X has segment length %d", m, seglen);
        if ((uint64_t)j->pos + (uint64_t)seglen > (uint64_t)j->len)
            return ic_err(err, esz, "JPEG marker FF%02X is cut off (%d bytes claimed, %lu present)",
                          m, seglen, (unsigned long)(j->len - j->pos));
        const uint8_t *p = j->d + j->pos + 2;
        int l = seglen - 2;

        switch (m) {
        case 0xC0:                                     /* baseline             */
        case 0xC1:                                     /* extended sequential  */
        case 0xC2:                                     /* progressive          */
            if (ic_jparse_sof(j, p, l, m) != 0) return -1;
            j->prog = (m == 0xC2);
            break;

        case 0xC4:
            if (!j->ncomp) return ic_err(err, esz, "JPEG has a Huffman table before its frame header");
            if (ic_jparse_dht(j, p, l) != 0) return -1;
            break;

        case 0xDB:
            /* DQT before SOF0 is the NORMAL order in a JFIF file: the tables
             * describe how to dequantise, and they are written before the
             * frame header that references them. Rejecting that ordering made
             * every ordinary JPEG fail to open. Tables are only consulted
             * after the scan starts, so there is nothing to check here. */
            if (ic_jparse_dqt(j, p, l) != 0) return -1;
            break;

        case 0xDD:
            if (l < 2) return ic_err(err, esz, "JPEG DRI segment is too short");
            j->ri = ic_be16(p);
            break;

        case 0xDC:
            return ic_err(err, esz, "JPEG defines its height with a DNL marker, which this decoder does not support");

        case 0xDA: {                                   /* start of scan */
            if (!j->ncomp) return ic_err(err, esz, "JPEG has a scan before its frame header");
            if (l < 1) return ic_err(err, esz, "JPEG SOS segment is empty");
            int ns = p[0];
            if (ns < 1 || ns > j->ncomp)
                return ic_err(err, esz, "JPEG scan names %d components but the frame has %d", ns, j->ncomp);
            if (l < 1 + 2 * ns + 3)
                return ic_err(err, esz, "JPEG SOS segment is cut off in its component list");
            int td[IC_JMAX_COMPS], ta[IC_JMAX_COMPS];
            int order[IC_JMAX_COMPS];
            int count = 0;
            for (int s = 0; s < ns; s++) {
                int id = p[1 + 2 * s];
                int t = p[2 + 2 * s];
                int found = -1;
                for (int ci = 0; ci < j->ncomp; ci++)
                    if (j->comp[ci].id == id) found = ci;
                if (found < 0)
                    return ic_err(err, esz, "JPEG scan names component id %d, which the frame does not have", id);
                for (int q = 0; q < count; q++)
                    if (order[q] == found)
                        return ic_err(err, esz, "JPEG scan names component id %d twice", id);
                order[count] = found;
                td[count] = t >> 4;
                ta[count] = t & 15;
                count++;
            }
            j->ss = p[1 + 2 * ns];
            j->se = p[2 + 2 * ns];
            int aa = p[3 + 2 * ns];
            j->ah = aa >> 4;
            j->al = aa & 15;

            if (!j->prog) {
                if (j->ss != 0 || j->se != 63 || j->ah != 0 || j->al != 0)
                    return ic_err(err, esz,
                                  "JPEG baseline scan has Ss=%d Se=%d Ah=%d Al=%d (must be 0/63/0/0)",
                                  j->ss, j->se, j->ah, j->al);
            } else {
                if (j->ah > 13 || j->al > 13)
                    return ic_err(err, esz, "JPEG progressive scan has Ah=%d Al=%d (at most 13)", j->ah, j->al);
                if (j->ah != 0 && j->ah != j->al + 1)
                    return ic_err(err, esz, "JPEG progressive scan has Ah=%d Al=%d (Ah must be 0 or Al+1)", j->ah, j->al);
                if (j->ss == 0) {
                    if (j->se != 0)
                        return ic_err(err, esz, "JPEG progressive DC scan has Se=%d (must be 0)", j->se);
                } else {
                    if (j->ss > j->se || j->se > 63)
                        return ic_err(err, esz, "JPEG progressive AC scan has Ss=%d Se=%d", j->ss, j->se);
                    if (ns != 1)
                        return ic_err(err, esz, "JPEG progressive AC scan covers %d components (must be 1)", ns);
                }
            }
            if (j->se >= 64)
                return ic_err(err, esz, "JPEG scan has Se=%d (at most 63)", j->se);

            for (int s = 0; s < ns; s++) {
                j->cur_td[s] = td[s];
                j->cur_ta[s] = ta[s];
            }
            j->eob_run = 0;
            for (int ci = 0; ci < j->ncomp; ci++) j->comp[ci].dc_pred = 0;
            j->bitbuf = 0;
            j->bitcnt = 0;
            j->pending = 0;
            j->eof = 0;
            j->ep = p + l;                             /* entropy data starts after the segment */
            j->eend = j->d + j->len;

            if (ic_jdecode_scan(j, ns, order[0]) != 0) {
                if (j->err_set) return -1;
                return ic_err(err, esz, "JPEG scan could not be decoded");
            }
            if (j->err_set) return -1;
            j->pos = (size_t)(j->ep - j->d);
            scans++;
            break;
        }

        default:
            if (m >= 0xE0 && m <= 0xEF) {
                if (l >= 5 && !memcmp(p, "JFIF\0", 5)) j->jfif = 1;
                if (l >= 12 && !memcmp(p, "Adobe", 5)) {
                    j->adobe = 1;
                    j->adobe_transform = p[11];
                }
            }
            break;
        }

        if (m != 0xDA) j->pos += (size_t)seglen;
    }

    if (!j->ncomp) return ic_err(err, esz, "JPEG has no frame header (SOF)");
    if (!scans) return ic_err(err, esz, "JPEG has no scans (SOS)");
    if (j->err_set) return -1;

    return ic_jpeg_reconstruct(j, im, err, esz);
}

static void ic_jpeg_free(IcJpeg *j) {
    for (int i = 0; i < IC_JMAX_COMPS; i++) {
        free(j->comp[i].coef);
        j->comp[i].coef = NULL;
        free(j->comp[i].plane);
        j->comp[i].plane = NULL;
    }
}

static int ic_jpeg_decode(const uint8_t *d, size_t n, Img *im,
                          char *err, size_t esz) {
    IcJpeg j;
    memset(&j, 0, sizeof(j));
    j.d = d;
    j.len = n;
    j.pos = 2;
    j.err = err;
    j.esz = esz;

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return ic_err(err, esz, "not a JPEG (no SOI marker)");

    int rc = ic_jpeg_run(&j, im, err, esz);
    ic_jpeg_free(&j);
    if (rc != 0) {
        memset(im, 0, sizeof(*im));
        return -1;
    }
    return 0;
}

/* ============================================================
 *  6. BMP
 *
 *  24- and 32-bit uncompressed BI_RGB, bottom-up and top-down, plus
 *  8-bit palette images because they are the one indexed format BMP
 *  files still arrive in. RLE, BITFIELDS and OS/2 2.x bitmaps are
 *  refused with the compression value that was found.
 *
 *  A 32-bit BI_RGB bitmap's fourth byte is "unused" by the format,
 *  not alpha; treating it as alpha is how half the 32-bit BMPs on
 *  the web become invisible. This decoder reports those pixels as
 *  opaque, which is what the format says they are.
 * ============================================================ */
static int ic_bmp_decode(const uint8_t *d, size_t n, Img *im,
                         char *err, size_t esz) {
    if (n < 26 || d[0] != 'B' || d[1] != 'M')
        return ic_err(err, esz, "not a BMP (no BM signature)");

    uint32_t off = ic_le32(d + 10);
    uint32_t dib = ic_le32(d + 14);
    int W = 0, H = 0, bpp = 0, topdown = 0;
    uint32_t compression = 0;
    uint32_t palette_at = 0, palette_n = 0, palette_entry = 4;

    if (dib == 12) {                                  /* BITMAPCOREHEADER */
        if (n < 26) return ic_err(err, esz, "BMP core header is cut off");
        W = (int)ic_le16(d + 18);
        H = (int)ic_le16(d + 20);
        bpp = (int)ic_le16(d + 24);
        if (ic_le16(d + 22) != 1) return ic_err(err, esz, "BMP planes is %u, not 1", ic_le16(d + 22));
        palette_entry = 3;
    } else if (dib >= 40) {                           /* INFOHEADER or later */
        if (n < 54) return ic_err(err, esz, "BMP info header is cut off");
        W = (int)ic_le32(d + 18);
        int32_t hs = (int32_t)ic_le32(d + 22);
        if (hs < 0) { topdown = 1; hs = -hs; }
        H = hs;
        if (ic_le16(d + 26) != 1) return ic_err(err, esz, "BMP planes is %u, not 1", ic_le16(d + 26));
        bpp = (int)ic_le16(d + 28);
        compression = ic_le32(d + 30);
        uint32_t used = ic_le32(d + 46);
        palette_at = 14u + dib;
        palette_n = used ? used : 256u;
    } else {
        return ic_err(err, esz, "BMP header size %lu is not one this decoder knows",
                      (unsigned long)dib);
    }

    if (compression == 1 || compression == 2)
        return ic_err(err, esz, "BMP uses RLE compression (%lu), which this decoder does not implement",
                      (unsigned long)compression);
    if (compression == 3)
        return ic_err(err, esz, "BMP uses BI_BITFIELDS masks, which this decoder does not implement");
    if (compression != 0)
        return ic_err(err, esz, "BMP compression %lu is not BI_RGB (0)", (unsigned long)compression);
    if (bpp != 8 && bpp != 24 && bpp != 32)
        return ic_err(err, esz, "BMP is %d-bit; this decoder reads 8, 24 and 32-bit BI_RGB", bpp);
    if (dib == 12 && bpp == 8)
        return ic_err(err, esz, "BMP is an OS/2 8-bit palette bitmap, which this decoder does not support");
    if (W <= 0 || H <= 0) return ic_err(err, esz, "BMP says %dx%d", W, H);
    if (W > IMG_MAX_DIM || H > IMG_MAX_DIM)
        return ic_err(err, esz, "BMP says %dx%d, outside the 1..%d limit", W, H, IMG_MAX_DIM);
    if ((uint64_t)W * (uint64_t)H > IC_MAX_PIXELS)
        return ic_err(err, esz, "BMP says %dx%d = %llu pixels, above the %u-pixel limit",
                      W, H, (unsigned long long)W * (unsigned long long)H,
                      (unsigned)IMG_MAX_PIXELS);

    const uint8_t *pal = NULL;
    if (bpp == 8) {
        if (compression != 0 || dib < 40)
            return ic_err(err, esz, "BMP palette handling needs an INFOHEADER and BI_RGB");
        if (palette_n == 0 || palette_n > 256)
            return ic_err(err, esz, "BMP palette claims %lu entries (at most 256)", (unsigned long)palette_n);
        if ((uint64_t)palette_at + (uint64_t)palette_n * palette_entry > (uint64_t)n)
            return ic_err(err, esz, "BMP palette is cut off (%lu entries at byte %lu of %lu)",
                          (unsigned long)palette_n, (unsigned long)palette_at, (unsigned long)n);
        pal = d + palette_at;
    }

    size_t rowbytes = (((size_t)W * (size_t)bpp + 31u) / 32u) * 4u;
    if (off > n)
        return ic_err(err, esz, "BMP pixel data starts at byte %lu, past the %lu-byte file",
                      (unsigned long)off, (unsigned long)n);
    if (rowbytes > n - off || (size_t)H > (n - off) / rowbytes) {
        uint64_t need = (uint64_t)rowbytes * (uint64_t)H;
        return ic_err(err, esz, "BMP pixel data is cut off (%llu bytes of rows needed, %lu present)",
                      (unsigned long long)need, (unsigned long)(n - off));
    }

    uint32_t *pix = malloc((size_t)W * (size_t)H * sizeof(uint32_t));
    if (!pix)
        return ic_err(err, esz, "out of memory for %dx%d pixels", W, H);

    for (int y = 0; y < H; y++) {
        int sy = topdown ? y : H - 1 - y;
        const uint8_t *src = d + off + (size_t)sy * rowbytes;
        uint32_t *dst = pix + (size_t)y * (size_t)W;
        if (bpp == 8) {
            for (int x = 0; x < W; x++) {
                unsigned idx = src[x];
                if (idx >= palette_n) {
                    free(pix);
                    return ic_err(err, esz, "BMP pixel (%d,%d) uses palette index %u but the palette has %lu entries",
                                  x, y, idx, (unsigned long)palette_n);
                }
                const uint8_t *e = pal + (size_t)idx * palette_entry;
                dst[x] = ic_rgba(e[2], e[1], e[0], 255);
            }
        } else {
            int step = bpp / 8;
            for (int x = 0; x < W; x++) {
                const uint8_t *p = src + (size_t)x * (size_t)step;
                dst[x] = ic_rgba(p[2], p[1], p[0], 255);
            }
        }
    }

    im->w = W;
    im->h = H;
    im->stride = W;
    im->px = pix;
    return 0;
}

/* ============================================================
 *  7. PNM / PBM (P1..P6)
 *
 *  ASCII and binary, greyscale and RGB, maxval 1..65535. PBM's
 *  1 = black is the one place in this file where a sample value is
 *  inverted, and it is inverted on purpose.
 * ============================================================ */
typedef struct {
    const uint8_t *p;
    size_t n, i;
} IcPnm;

static void ic_pnm_skip(IcPnm *c) {
    for (;;) {
        while (c->i < c->n) {
            uint8_t ch = c->p[c->i];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f') {
                c->i++;
            } else {
                break;
            }
        }
        if (c->i < c->n && c->p[c->i] == '#') {
            while (c->i < c->n && c->p[c->i] != '\n') c->i++;
            continue;
        }
        break;
    }
}

static int ic_pnm_int(IcPnm *c, long *out) {
    ic_pnm_skip(c);
    if (c->i >= c->n || c->p[c->i] < '0' || c->p[c->i] > '9') return -1;
    long v = 0;
    while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9') {
        v = v * 10 + (c->p[c->i] - '0');
        if (v > 100000000L) return -1;
        c->i++;
    }
    *out = v;
    return 0;
}

/* Exactly one whitespace byte separates the header from the raster.
 * CRLF counts as one separator because a file written on Windows has
 * two bytes where the format says one. */
static int ic_pnm_raster(IcPnm *c) {
    if (c->i >= c->n) return -1;
    if (c->p[c->i] == '\r' && c->i + 1 < c->n && c->p[c->i + 1] == '\n') { c->i += 2; return 0; }
    uint8_t ch = c->p[c->i];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f') {
        c->i++;
        return 0;
    }
    return -1;
}

static int ic_pnm_decode(const uint8_t *d, size_t n, Img *im,
                         char *err, size_t esz) {
    if (n < 2 || d[0] != 'P' || d[1] < '1' || d[1] > '6')
        return ic_err(err, esz, "not a PNM (no P1..P6 magic)");
    int kind = d[1] - '0';
    IcPnm c;
    c.p = d;
    c.n = n;
    c.i = 2;

    long W = 0, H = 0, maxv = 1;
    if (ic_pnm_int(&c, &W) != 0 || ic_pnm_int(&c, &H) != 0)
        return ic_err(err, esz, "PNM header: width or height is missing");
    if (kind == 2 || kind == 3 || kind == 5 || kind == 6) {
        if (ic_pnm_int(&c, &maxv) != 0) return ic_err(err, esz, "PNM header: maxval is missing");
        if (maxv < 1 || maxv > 65535)
            return ic_err(err, esz, "PNM maxval %ld is outside 1..65535", maxv);
    }
    if (W <= 0 || H <= 0)
        return ic_err(err, esz, "PNM says %ldx%ld", W, H);
    if (W > IMG_MAX_DIM || H > IMG_MAX_DIM)
        return ic_err(err, esz, "PNM says %ldx%ld, outside the 1..%d limit", W, H, IMG_MAX_DIM);
    if ((uint64_t)W * (uint64_t)H > IC_MAX_PIXELS)
        return ic_err(err, esz, "PNM says %ldx%ld = %llu pixels, above the %u-pixel limit",
                      W, H, (unsigned long long)W * (unsigned long long)H,
                      (unsigned)IMG_MAX_PIXELS);

    uint32_t *pix = malloc((size_t)W * (size_t)H * sizeof(uint32_t));
    if (!pix) return ic_err(err, esz, "out of memory for %ldx%ld pixels", W, H);

    int iscolour = (kind == 3 || kind == 6);
    int isbitmap = (kind == 1 || kind == 4);
    int binary = (kind >= 4);
    int samp = binary ? (maxv < 256 ? 1 : 2) : 0;

    if (binary) {
        if (ic_pnm_raster(&c) != 0) {
            free(pix);
            return ic_err(err, esz, "PNM header is not followed by a whitespace byte");
        }
        uint64_t need;
        if (isbitmap) need = (((uint64_t)W + 7u) / 8u) * (uint64_t)H;
        else need = (uint64_t)W * (uint64_t)H * (uint64_t)(iscolour ? 3 : 1) * (uint64_t)samp;
        if (need > (uint64_t)(n - c.i)) {
            free(pix);
            return ic_err(err, esz, "PNM raster is cut off (%llu bytes needed, %lu present)",
                          (unsigned long long)need, (unsigned long)(n - c.i));
        }
    }

    for (long y = 0; y < H; y++) {
        uint32_t *row = pix + (size_t)y * (size_t)W;
        for (long x = 0; x < W; x++) {
            unsigned r = 0, g = 0, b = 0;
            if (isbitmap) {
                unsigned bit = 0;
                if (binary) {
                    size_t bitpos = (size_t)y * (((size_t)W + 7u) / 8u) * 8u + (size_t)x;
                    bit = (unsigned)((d[c.i + (bitpos >> 3)] >> (7 - (bitpos & 7))) & 1u);
                } else {
                    long v = 0;
                    if (ic_pnm_int(&c, &v) != 0) {
                        free(pix);
                        return ic_err(err, esz, "PBM raster ended at row %ld, column %ld", y + 1, x + 1);
                    }
                    bit = (unsigned)(v != 0);
                }
                unsigned g8 = bit ? 0u : 255u;            /* PBM: 1 is black */
                r = g = b = g8;
            } else if (binary) {
                const uint8_t *p = d + c.i;
                if (iscolour) {
                    for (int ch = 0; ch < 3; ch++) {
                        unsigned v = samp == 1 ? p[ch]
                                               : ((unsigned)p[ch * 2] << 8) | p[ch * 2 + 1];
                        unsigned v8 = maxv == 1 ? (v ? 255u : 0u)
                                                : (unsigned)((v * 255u + (unsigned)maxv / 2u) / (unsigned)maxv);
                        if (ch == 0) r = v8; else if (ch == 1) g = v8; else b = v8;
                    }
                    c.i += (size_t)3 * (size_t)samp;
                } else {
                    unsigned v = samp == 1 ? p[0] : ((unsigned)p[0] << 8) | p[1];
                    unsigned v8 = maxv == 1 ? (v ? 255u : 0u)
                                            : (unsigned)((v * 255u + (unsigned)maxv / 2u) / (unsigned)maxv);
                    r = g = b = v8;
                    c.i += (size_t)samp;
                }
            } else {
                long v[3] = { 0, 0, 0 };
                int ns = iscolour ? 3 : 1;
                for (int ch = 0; ch < ns; ch++) {
                    if (ic_pnm_int(&c, &v[ch]) != 0) {
                        free(pix);
                        return ic_err(err, esz, "PNM raster ended at row %ld, column %ld", y + 1, x + 1);
                    }
                    if (v[ch] < 0 || v[ch] > maxv) {
                        free(pix);
                        return ic_err(err, esz, "PNM sample %ld is outside 0..%ld", v[ch], maxv);
                    }
                }
                if (iscolour) {
                    r = (unsigned)((v[0] * 255L + maxv / 2) / maxv);
                    g = (unsigned)((v[1] * 255L + maxv / 2) / maxv);
                    b = (unsigned)((v[2] * 255L + maxv / 2) / maxv);
                } else {
                    unsigned g8 = (unsigned)((v[0] * 255L + maxv / 2) / maxv);
                    r = g = b = g8;
                }
            }
            row[x] = ic_rgba(r, g, b, 255);
        }
    }

    im->w = (int)W;
    im->h = (int)H;
    im->stride = (int)W;
    im->px = pix;
    return 0;
}

/* ============================================================
 *  8. the public API
 * ============================================================ */
int img_sniff(const unsigned char *buf, size_t len) {
    if (!buf) return IMG_FMT_UNKNOWN;
    if (len >= 8 && !memcmp(buf, IC_PNG_SIG, 8)) return IMG_FMT_PNG;
    if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) return IMG_FMT_JPEG;
    if (len >= 2 && buf[0] == 'B' && buf[1] == 'M') return IMG_FMT_BMP;
    if (len >= 2 && buf[0] == 'P' && buf[1] >= '1' && buf[1] <= '6') return IMG_FMT_PNM;
    return IMG_FMT_UNKNOWN;
}

const char *img_format_name(int fmt) {
    switch (fmt) {
    case IMG_FMT_PNG: return "PNG";
    case IMG_FMT_JPEG: return "JPEG";
    case IMG_FMT_BMP: return "BMP";
    case IMG_FMT_PNM: return "PNM";
    default: return "unknown";
    }
}

int img_decode(const unsigned char *buf, size_t len, Img *out,
               char *err, size_t esz) {
    if (!out) return ic_err(err, esz, "img_decode: no output image was passed");
    memset(out, 0, sizeof(*out));
    if (err && esz) err[0] = '\0';
    if (!buf) return ic_err(err, esz, "img_decode: no input buffer");
    if (len < 2) return ic_err(err, esz, "not an image: the file is %lu byte%s",
                               (unsigned long)len, len == 1 ? "" : "s");

    int fmt = img_sniff(buf, len);
    switch (fmt) {
    case IMG_FMT_PNG: return ic_png_decode(buf, len, out, err, esz);
    case IMG_FMT_JPEG: return ic_jpeg_decode(buf, len, out, err, esz);
    case IMG_FMT_BMP: return ic_bmp_decode(buf, len, out, err, esz);
    case IMG_FMT_PNM: return ic_pnm_decode(buf, len, out, err, esz);
    default: break;
    }

    char head[3 * 8 + 1];
    size_t hn = len < 8 ? len : 8;
    char *q = head;
    for (size_t i = 0; i < hn; i++) {
        static const char hex[] = "0123456789ABCDEF";
        *q++ = hex[(buf[i] >> 4) & 15];
        *q++ = hex[buf[i] & 15];
        if (i + 1 < hn) *q++ = ' ';
    }
    *q = '\0';
    return ic_err(err, esz, "unrecognised image: the first %lu bytes are %s", (unsigned long)hn, head);
}

int img_scale_fit(const Img *src, int max_w, int max_h, Img *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!src || !src->px) return -1;
    if (src->w <= 0 || src->h <= 0) return -1;
    if (max_w < 1 || max_h < 1) return -1;
    int sw = src->w, sh = src->h;
    int sstride = src->stride > 0 ? src->stride : sw;
    if (sstride < sw) return -1;

    int dw = sw, dh = sh;
    if (sw > max_w || sh > max_h) {
        /* Choose the limiting dimension, then round the other one, so
         * the result keeps the aspect ratio and never exceeds the box. */
        if ((int64_t)sw * max_h <= (int64_t)sh * max_w) {
            dw = max_w;
            dh = (int)(((int64_t)sh * max_w + sw / 2) / sw);
        } else {
            dh = max_h;
            dw = (int)(((int64_t)sw * max_h + sh / 2) / sh);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (dw > max_w) dw = max_w;
        if (dh > max_h) dh = max_h;
    }

    uint32_t *pix = malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (!pix) return -1;

    if (dw == sw && dh == sh) {
        for (int y = 0; y < sh; y++)
            memcpy(pix + (size_t)y * (size_t)dw,
                   src->px + (size_t)y * (size_t)sstride,
                   (size_t)sw * sizeof(uint32_t));
    } else {
        for (int y = 0; y < dh; y++) {
            int y0 = (int)(((int64_t)y * sh) / dh);
            int y1 = (int)(((int64_t)(y + 1) * sh) / dh);
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > sh) y1 = sh;
            for (int x = 0; x < dw; x++) {
                int x0 = (int)(((int64_t)x * sw) / dw);
                int x1 = (int)(((int64_t)(x + 1) * sw) / dw);
                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > sw) x1 = sw;
                uint64_t sa = 0, sr = 0, sg = 0, sb = 0;
                uint64_t cnt = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const uint32_t *row = src->px + (size_t)sy * (size_t)sstride;
                    for (int sx = x0; sx < x1; sx++) {
                        uint32_t c = row[sx];
                        sa += (c >> 24) & 255u;
                        sr += (c >> 16) & 255u;
                        sg += (c >> 8) & 255u;
                        sb += c & 255u;
                        cnt++;
                    }
                }
                if (cnt == 0) cnt = 1;               /* cannot happen; keeps the divide safe */
                uint32_t n2 = (uint32_t)(cnt / 2u);
                pix[(size_t)y * (size_t)dw + (size_t)x] =
                    ic_rgba((unsigned)((sr + n2) / cnt), (unsigned)((sg + n2) / cnt),
                            (unsigned)((sb + n2) / cnt), (unsigned)((sa + n2) / cnt));
            }
        }
    }

    out->w = dw;
    out->h = dh;
    out->stride = dw;
    out->px = pix;
    return 0;
}

void img_free(Img *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = 0;
    im->h = 0;
    im->stride = 0;
}
