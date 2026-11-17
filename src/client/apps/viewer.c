/* ============================================================
 *  Mystic OS - apps (external) / viewer
 *
 *  A document and image viewer that runs as its own process.
 *  It is started with a path:   viewer /root/Pictures/wall.ppm
 *
 *  What is real here:
 *
 *    PNG      a decoder written in this file. It verifies the
 *             signature and every chunk CRC, inflates the zlib
 *             stream with its own deflate implementation (stored,
 *             fixed Huffman and dynamic Huffman), undoes all five
 *             row filters, and converts every legal bit depth
 *             (1/2/4/8/16) for grey, RGB, palette, grey+alpha and
 *             RGBA - including Adam7 interlacing and tRNS.
 *    BMP      24- and 32-bit uncompressed BI_RGB bitmaps, bottom-up
 *             and top-down, with the 4-byte row padding.
 *    PNM      P1/P4 bitmap, P2/P5 greyscale and P3/P6 pixmap, in
 *             both ASCII and binary forms, maxval up to 65535.
 *    text     the whole file is read (up to 4 MiB), indexed by
 *             line, tabs expanded to 8 columns, drawn with the mono
 *             font with line numbers and real vertical + horizontal
 *             scrolling.
 *    others   never faked. A video, an audio file, a PDF or an
 *             archive shows its real stat() facts, the type that
 *             src/filetypes.c detected from its bytes, and one
 *             honest line saying what would be needed.
 *
 *  Every number on screen comes from stat(2), the file's own
 *  header, or the decoded pixels. The pixel readout under the
 *  pointer is the actual 32-bit pixel from the decoder.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifndef VIEWER_HOST_TEST
#  include <unistd.h>
#endif

/* The type registry belongs to src/filetypes.c (another worker owns it).
 * Include it when it exists; the local detector below is only the fallback
 * for a tree where it has not landed yet. viewer_registry_type() is the
 * only place that knows the registry's shape. */
#if defined(__has_include)
#  if __has_include("../../filetypes.h")
#    include "../../filetypes.h"
#    define VIEWER_HAVE_FILETYPES 1
#  endif
#endif

/* ------------------------------------------------------------ sizes */

#define WIN_W 880
#define WIN_H 620

#define MAX_TEXT_BYTES   (4u * 1024u * 1024u)     /* the cap the footer reports */
#define MAX_IMAGE_FILE   (96u * 1024u * 1024u)    /* refuse to malloc a bigger file */
#define MAX_IMAGE_PIXELS 16000000u                /* 16 Mpx, about 64 MiB of RGBA */
#define MAX_IMAGE_SIDE   20000
#define MAX_RAW_BYTES    (160u * 1024u * 1024u)   /* decompressed PNG bytes */

/* ------------------------------------------------------------ colours */

static const u32 C_BG      = RGB(0x10, 0x15, 0x1D);
static const u32 C_BG2     = RGB(0x16, 0x1D, 0x28);
static const u32 C_PANEL   = RGB(0x1A, 0x21, 0x2D);
static const u32 C_LINE    = RGB(0x2A, 0x34, 0x45);
static const u32 C_TEXT    = RGB(0xE8, 0xEC, 0xF6);
static const u32 C_DIM     = RGB(0x8A, 0x93, 0xA6);
static const u32 C_ACCENT  = RGB(0x4F, 0xD1, 0xC5);
static const u32 C_WARN    = RGB(0xE0, 0x6C, 0x5C);
static const u32 C_CHK1    = RGB(0x0C, 0x10, 0x17);
static const u32 C_CHK2    = RGB(0x14, 0x1A, 0x23);
static const u32 C_GUTTER  = RGB(0x0D, 0x12, 0x19);

/* ------------------------------------------------------------ categories
 * One local vocabulary, so the app does not depend on the registry's enum
 * names. viewer_registry_type() maps whatever it provides onto these. */
enum {
    VCAT_UNKNOWN, VCAT_TEXT, VCAT_IMAGE, VCAT_AUDIO,
    VCAT_VIDEO, VCAT_PDF, VCAT_DOCUMENT, VCAT_ARCHIVE, VCAT_BINARY
};

static const char *cat_name(int c) {
    switch (c) {
    case VCAT_TEXT:    return "text";
    case VCAT_IMAGE:   return "image";
    case VCAT_AUDIO:   return "audio";
    case VCAT_VIDEO:   return "video";
    case VCAT_PDF:     return "pdf";
    case VCAT_DOCUMENT:return "document";
    case VCAT_ARCHIVE: return "archive";
    case VCAT_BINARY:  return "binary data";
    default:           return "unknown";
    }
}

/* What one honest sentence would be needed for a category this build does
 * not decode. The caller prints this verbatim; it never claims to work. */
static const char *cat_needs(int c) {
    switch (c) {
    case VCAT_AUDIO:
        return "needs an audio decoder and an output device; not implemented in this build";
    case VCAT_VIDEO:
        return "needs a video decoder and a frame clock; not implemented in this build";
    case VCAT_PDF:
        return "needs a PDF parser and rasteriser; not implemented in this build";
    case VCAT_DOCUMENT:
        return "needs a document renderer for this format; not implemented in this build";
    case VCAT_ARCHIVE:
        return "needs the matching archive reader; not implemented in this build";
    case VCAT_IMAGE:
        return "this image format has no decoder here (PNG, BMP and PPM/PGM are decoded)";
    default:
        return "no decoder for these bytes; only the file's own facts are shown";
    }
}

/* ------------------------------------------------------------ document */

typedef struct {
    int  w, h;
    u32 *px;           /* RGBA, 0xAARRGGBB, w*h */
    int  opaque;       /* no pixel has alpha < 255 */
    char fmt[96];      /* what the file's header actually said */
} VImage;

typedef enum { DOC_NONE, DOC_IMAGE, DOC_TEXT, DOC_OTHER, DOC_ERROR } DocKind;

typedef struct {
    char       path[512];
    char       name[256];
    struct stat st;
    int        have_st;
    DocKind    kind;

    char       error[256];         /* DOC_ERROR: the real reason */
    char       other_note[256];    /* DOC_OTHER: one honest line */

    VImage     img;

    u8        *text;
    size_t     text_len;
    int        truncated;
    u32       *line_off;           /* byte offset of each line in text[] */
    int        nlines;
    int        max_cols;           /* widest line with tabs expanded */

    int        first_line;         /* text scroll position, in lines */
    int        col_off;            /* text scroll position, in columns */

    float      zoom;               /* image zoom; 0 = fit the window */
    float      pan_x, pan_y;       /* image pan, in window pixels */

    int        mx, my;             /* pointer, for the pixel readout */
    int        dragging;
    int        last_mx, last_my;

    int        cat;
    char       type_name[96];
    char       type_note[160];
    int        type_known;
} Viewer;

static void viewer_free(Viewer *v) {
    free(v->img.px);
    free(v->text);
    free(v->line_off);
    v->img.px = NULL;
    v->text = NULL;
    v->line_off = NULL;
}

/* ------------------------------------------------------------ numbers */

static int vmax_i(int a, int b) { return a > b ? a : b; }
static int vmin_i(int a, int b) { return a < b ? a : b; }

static int iround(float x) { return (int)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

static u32 rd_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u16 rd_le16(const u8 *p) { return (u16)(((unsigned)p[1] << 8) | p[0]); }
static u32 rd_le32(const u8 *p) {
    return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
}

/* Human size from a real byte count. The exact number of bytes is printed
 * too, in the facts panel. */
static void fmt_size(char *out, size_t n, unsigned long long bytes) {
    if (bytes < 1024ull)
        snprintf(out, n, "%llu B", bytes);
    else if (bytes < 1024ull * 1024ull)
        snprintf(out, n, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull)
        snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, n, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

static u32 pix_rgba(u8 r, u8 g, u8 b, u8 a) {
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* ------------------------------------------------------------ file io */

static int read_head(const char *path, u8 *out, size_t want, size_t *got,
                     char *err, size_t esz) {
    *got = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, esz, "%s", strerror(errno));
        return -1;
    }
    size_t n = fread(out, 1, want, f);
    if (ferror(f)) {
        snprintf(err, esz, "%s", strerror(errno));
        fclose(f);
        return -1;
    }
    fclose(f);
    *got = n;
    return 0;
}

/* Read at most cap bytes. One byte past the cap is requested so a file that
 * is larger than stat() said (or larger than the cap) is detected honestly. */
static int read_file_cap(const char *path, size_t cap, u8 **out, size_t *outlen,
                         int *truncated, char *err, size_t esz) {
    *out = NULL;
    *outlen = 0;
    if (truncated) *truncated = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, esz, "%s", strerror(errno));
        return -1;
    }
    u8 *buf = malloc(cap + 1u);
    if (!buf) {
        snprintf(err, esz, "out of memory for %lu bytes", (unsigned long)(cap + 1u));
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, cap + 1u, f);
    if (ferror(f)) {
        snprintf(err, esz, "%s", strerror(errno));
        free(buf);
        fclose(f);
        return -1;
    }
    if (n > cap) {
        n = cap;
        if (truncated) *truncated = 1;
    }
    fclose(f);
    buf[n] = 0;
    *out = buf;
    *outlen = n;
    return 0;
}

/* ------------------------------------------------------------ checksums */

static u32 crc_tab[256];
static int crc_ready;

static void crc_build(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[n] = c;
    }
    crc_ready = 1;
}

static u32 crc32_of(const u8 *p, size_t n) {
    if (!crc_ready) crc_build();
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u32 adler32_of(const u8 *p, size_t n) {
    u32 a = 1, b = 0;
    size_t i = 0;
    while (i < n) {
        size_t chunk = n - i;
        if (chunk > 5552u) chunk = 5552u;
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
 *  deflate / zlib
 *
 *  A complete inflater: stored blocks, fixed Huffman and dynamic
 *  Huffman, with the canonical-code tables built exactly as RFC
 *  1951 describes. It writes into an output buffer whose exact
 *  size the PNG header already predicted, and it refuses to write
 *  past that size, so a corrupt file cannot run the viewer out of
 *  memory.
 * ============================================================ */

typedef struct {
    const u8 *in;
    size_t len, pos;
    u32 bitbuf;
    int bitcnt;
    u8 *out;
    size_t cap, n;
    int err;
    const char *why;
} VZ;

typedef struct {
    short count[16];
    unsigned short sym[288];
} VHuf;

static void vz_fail(VZ *z, const char *why) {
    if (!z->err) { z->err = 1; z->why = why; }
}

static int vz_bits(VZ *z, int n) {
    if (n <= 0) return 0;
    while (z->bitcnt < n) {
        if (z->pos >= z->len) { vz_fail(z, "deflate stream ended in the middle of a code"); return 0; }
        z->bitbuf |= (u32)z->in[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & (((u32)1 << n) - 1u));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

static void vz_put(VZ *z, u8 b) {
    if (z->n >= z->cap) { vz_fail(z, "pixel stream is larger than the image header says"); return; }
    z->out[z->n++] = b;
}

/* Returns 1 complete code, 0 valid but incomplete (all-zero distance tables
 * are legal), -1 over-subscribed. */
static int vh_make(VHuf *h, const u8 *lens, int n) {
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

static int vh_dec(VZ *z, const VHuf *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = vz_bits(z, 1);
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
    vz_fail(z, "invalid Huffman code");
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

static VHuf vz_fixed_lit, vz_fixed_dist;
static int vz_fixed_ready;

static void vz_fixed_build(void) {
    u8 l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    vh_make(&vz_fixed_lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    vh_make(&vz_fixed_dist, l, 30);
    vz_fixed_ready = 1;
}

static int vz_inflate(VZ *z) {
    int final = 0;
    while (!final && !z->err) {
        final = vz_bits(z, 1);
        int type = vz_bits(z, 2);
        if (z->err) break;

        if (type == 0) {                          /* stored */
            z->bitbuf = 0;
            z->bitcnt = 0;
            if (z->pos + 4 > z->len) { vz_fail(z, "stored block header is truncated"); break; }
            unsigned len = (unsigned)z->in[z->pos] | ((unsigned)z->in[z->pos + 1] << 8);
            unsigned nlen = (unsigned)z->in[z->pos + 2] | ((unsigned)z->in[z->pos + 3] << 8);
            z->pos += 4;
            if ((len ^ 0xFFFFu) != nlen) { vz_fail(z, "stored block length does not match its complement"); break; }
            if (z->pos + len > z->len) { vz_fail(z, "stored block runs past the stream"); break; }
            for (unsigned i = 0; i < len; i++) vz_put(z, z->in[z->pos + i]);
            z->pos += len;
            continue;
        }

        VHuf *lit = NULL, *dist = NULL;
        if (type == 1) {
            if (!vz_fixed_ready) vz_fixed_build();
            lit = &vz_fixed_lit;
            dist = &vz_fixed_dist;
        } else if (type == 2) {
            int hlit = vz_bits(z, 5) + 257;
            int hdist = vz_bits(z, 5) + 1;
            int hclen = vz_bits(z, 4) + 4;
            if (z->err) break;
            if (hlit > 286 || hdist > 30) { vz_fail(z, "dynamic Huffman table is too large"); break; }

            static const u8 ord[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            u8 cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) cl[ord[i]] = (u8)vz_bits(z, 3);
            if (z->err) break;
            VHuf ch;
            if (vh_make(&ch, cl, 19) < 0) { vz_fail(z, "code-length table is over-subscribed"); break; }

            u8 lens[320];
            memset(lens, 0, sizeof(lens));
            int total = hlit + hdist;
            int n = 0;
            while (n < total) {
                int sym = vh_dec(z, &ch);
                if (z->err || sym < 0) { vz_fail(z, "bad code-length symbol"); break; }
                if (sym < 16) {
                    lens[n++] = (u8)sym;
                } else if (sym == 16) {
                    if (n == 0) { vz_fail(z, "repeat code with no previous length"); break; }
                    int rep = 3 + vz_bits(z, 2);
                    u8 prev = lens[n - 1];
                    while (rep-- > 0) {
                        if (n >= total) { vz_fail(z, "repeat runs past the table"); break; }
                        lens[n++] = prev;
                    }
                } else if (sym == 17) {
                    int rep = 3 + vz_bits(z, 3);
                    while (rep-- > 0) {
                        if (n >= total) { vz_fail(z, "zero repeat runs past the table"); break; }
                        lens[n++] = 0;
                    }
                } else {
                    int rep = 11 + vz_bits(z, 7);
                    while (rep-- > 0) {
                        if (n >= total) { vz_fail(z, "long zero repeat runs past the table"); break; }
                        lens[n++] = 0;
                    }
                }
                if (z->err) break;
            }
            if (z->err) break;
            static VHuf dl, dd;
            if (vh_make(&dl, lens, hlit) < 0) { vz_fail(z, "literal table is over-subscribed"); break; }
            if (vh_make(&dd, lens + hlit, hdist) < 0) { vz_fail(z, "distance table is over-subscribed"); break; }
            lit = &dl;
            dist = &dd;
        } else {
            vz_fail(z, "reserved deflate block type 3");
            break;
        }

        for (;;) {
            int sym = vh_dec(z, lit);
            if (z->err || sym < 0) break;
            if (sym < 256) {
                vz_put(z, (u8)sym);
                continue;
            }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) { vz_fail(z, "invalid length symbol"); break; }
            int len = LBASE[sym] + vz_bits(z, LEXT[sym]);
            int dsym = vh_dec(z, dist);
            if (z->err || dsym < 0) { vz_fail(z, "bad distance code"); break; }
            if (dsym >= 30) { vz_fail(z, "invalid distance symbol"); break; }
            size_t d = (size_t)DBASE[dsym] + (size_t)vz_bits(z, DEXT[dsym]);
            if (z->err) break;
            if (d > z->n) { vz_fail(z, "distance points before the start of the output"); break; }
            for (int i = 0; i < len; i++)
                vz_put(z, z->out[z->n - d]);
            if (z->err) break;
        }
    }
    return z->err ? -1 : 0;
}

/* ============================================================
 *  PNG
 * ============================================================ */

static const u8 PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undo the five PNG row filters in place. Every row is filter-byte first.
 * bpp is the filter's bytes-per-pixel, i.e. ceil(bits-per-pixel / 8). */
static int unfilter_rows(u8 *buf, int rows, size_t rowbytes, int bpp) {
    const size_t rs = rowbytes + 1u;
    for (int y = 0; y < rows; y++) {
        u8 ft = buf[(size_t)y * rs];
        if (ft > 4) return y;                  /* 0..4 are the only PNG filters */
        u8 *row = buf + (size_t)y * rs + 1u;
        const u8 *prev = y > 0 ? buf + (size_t)(y - 1) * rs + 1u : NULL;
        for (size_t x = 0; x < rowbytes; x++) {
            int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
            switch (ft) {
            case 0: break;
            case 1: row[x] = (u8)(row[x] + a); break;
            case 2: row[x] = (u8)(row[x] + b); break;
            case 3: row[x] = (u8)(row[x] + ((a + b) >> 1)); break;
            case 4: row[x] = (u8)(row[x] + paeth(a, b, c)); break;
            default: break;
            }
        }
    }
    return -1;
}

static unsigned sample_at(const u8 *row, int bitpos, int depth) {
    if (depth == 8) return row[bitpos >> 3];
    if (depth == 16) {
        int i = bitpos >> 3;
        return ((unsigned)row[i] << 8) | (unsigned)row[i + 1];
    }
    int i = bitpos >> 3;
    int shift = 8 - depth - (bitpos & 7);
    return ((unsigned)row[i] >> shift) & ((1u << depth) - 1u);
}

static u8 scale_sample(unsigned v, int depth) {
    if (depth == 16) return (u8)((v * 255u + 32767u) / 65535u);
    unsigned mx = (1u << depth) - 1u;
    if (mx == 0) return 0;
    return (u8)((v * 255u + mx / 2u) / mx);
}

static int png_channels(int ctype) {
    switch (ctype) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

typedef struct { int x0, y0, dx, dy; } PngPass;

static const PngPass PNG_PASS[7] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 }
};

static size_t png_pass_bytes(int w, int h, int bitspp, int interlace) {
    if (!interlace) {
        size_t rb = ((size_t)w * (size_t)bitspp + 7u) / 8u;
        return (rb + 1u) * (size_t)h;
    }
    size_t total = 0;
    for (int p = 0; p < 7; p++) {
        int x0 = PNG_PASS[p].x0, y0 = PNG_PASS[p].y0;
        int dx = PNG_PASS[p].dx, dy = PNG_PASS[p].dy;
        if (w <= x0 || h <= y0) continue;
        int pw = (w - x0 + dx - 1) / dx;
        int ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = ((size_t)pw * (size_t)bitspp + 7u) / 8u;
        total += (rb + 1u) * (size_t)ph;
    }
    return total;
}

/* Convert one decoded pass into the destination RGBA image. */
static void png_convert_pass(const u8 *buf, int pw, int ph, int depth, int ctype,
                             int dw, int x0, int dxp, int y0, int dyp,
                             const u8 *pal, int pal_n, const u8 *trns, int trns_len,
                             u32 *out, int *has_alpha) {
    int ch = png_channels(ctype);
    int bitspp = ch * depth;
    size_t rowbytes = ((size_t)pw * (size_t)bitspp + 7u) / 8u;
    for (int y = 0; y < ph; y++) {
        const u8 *row = buf + (size_t)y * (rowbytes + 1u) + 1u;
        int gy = y0 + y * dyp;
        for (int x = 0; x < pw; x++) {
            unsigned s[4] = { 0, 0, 0, 0 };
            int base = x * bitspp;
            for (int c = 0; c < ch; c++)
                s[c] = sample_at(row, base + c * depth, depth);

            u32 c = 0;
            if (ctype == 0) {                       /* greyscale */
                u8 g = scale_sample(s[0], depth);
                u8 a = 255;
                if (trns_len >= 2) {
                    unsigned key = ((unsigned)trns[0] << 8) | trns[1];
                    if (s[0] == key) a = 0;
                }
                c = pix_rgba(g, g, g, a);
            } else if (ctype == 2) {                /* RGB */
                u8 r = scale_sample(s[0], depth);
                u8 g = scale_sample(s[1], depth);
                u8 b = scale_sample(s[2], depth);
                u8 a = 255;
                if (trns_len >= 6) {
                    unsigned kr = ((unsigned)trns[0] << 8) | trns[1];
                    unsigned kg = ((unsigned)trns[2] << 8) | trns[3];
                    unsigned kb = ((unsigned)trns[4] << 8) | trns[5];
                    if (s[0] == kr && s[1] == kg && s[2] == kb) a = 0;
                }
                c = pix_rgba(r, g, b, a);
            } else if (ctype == 3) {                /* palette */
                unsigned idx = s[0];
                if (pal_n > 0 && idx < (unsigned)pal_n) {
                    u8 a = 255;
                    if (trns && idx < (unsigned)trns_len) a = trns[idx];
                    c = pix_rgba(pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], a);
                } else {
                    c = pix_rgba(255, 0, 255, 255);  /* visible "no palette" */
                }
            } else if (ctype == 4) {                /* grey + alpha */
                u8 g = scale_sample(s[0], depth);
                u8 a = scale_sample(s[1], depth);
                c = pix_rgba(g, g, g, a);
            } else {                                /* RGBA */
                c = pix_rgba(scale_sample(s[0], depth), scale_sample(s[1], depth),
                             scale_sample(s[2], depth), scale_sample(s[3], depth));
            }
            if (GET_A(c) != 255) *has_alpha = 1;
            out[(size_t)gy * (size_t)dw + (size_t)(x0 + x * dxp)] = c;
        }
    }
}

static int png_decode(const u8 *d, size_t n, VImage *im, char *err, size_t esz) {
    memset(im, 0, sizeof(*im));
    if (n < 8 || memcmp(d, PNG_SIG, 8) != 0) {
        snprintf(err, esz, "not a PNG (bad signature)");
        return -1;
    }

    int W = 0, H = 0, depth = 0, ctype = 0, interlace = 0;
    int saw_ihdr = 0, saw_iend = 0, saw_plte = 0;
    u8 pal[256 * 3];
    int pal_n = 0;
    u8 trns[256];
    int trns_len = 0;
    memset(pal, 0, sizeof(pal));
    memset(trns, 0, sizeof(trns));

    u8 *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    while (pos + 8 <= n) {
        u32 clen = rd_be32(d + pos);
        const u8 *type = d + pos + 4;
        if (clen > 0x7FFFFFFFu || (size_t)clen > n - pos - 8) {
            snprintf(err, esz, "chunk at byte %lu has a length past the end of the file",
                     (unsigned long)pos);
            free(idat);
            return -1;
        }
        const u8 *data = d + pos + 8;
        if (pos + 12u + (size_t)clen > n) {
            snprintf(err, esz, "chunk at byte %lu is cut off", (unsigned long)pos);
            free(idat);
            return -1;
        }
        u32 want_crc = rd_be32(data + clen);
        u32 got_crc = crc32_of(type, (size_t)clen + 4u);
        if (want_crc != got_crc) {
            snprintf(err, esz, "%.4s chunk CRC mismatch (stored %08X, computed %08X)",
                     (const char *)type, want_crc, got_crc);
            free(idat);
            return -1;
        }

        int critical = !(type[0] & 0x20);
        if (!memcmp(type, "IHDR", 4)) {
            if (saw_ihdr) { snprintf(err, esz, "more than one IHDR"); free(idat); return -1; }
            if (clen != 13) { snprintf(err, esz, "IHDR is %u bytes, not 13", (unsigned)clen); free(idat); return -1; }
            W = (int)rd_be32(data);
            H = (int)rd_be32(data + 4);
            depth = data[8];
            ctype = data[9];
            if (data[10] != 0) { snprintf(err, esz, "PNG compression method %u is not deflate", data[10]); free(idat); return -1; }
            if (data[11] != 0) { snprintf(err, esz, "PNG filter method %u is not 0", data[11]); free(idat); return -1; }
            interlace = data[12];
            if (interlace > 1) { snprintf(err, esz, "PNG interlace method %d is not defined", interlace); free(idat); return -1; }
            int ch = png_channels(ctype);
            if (ch == 0) { snprintf(err, esz, "PNG colour type %d is not defined", ctype); free(idat); return -1; }
            int okdepth = 0;
            if (ctype == 0) okdepth = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16);
            else if (ctype == 2 || ctype == 4 || ctype == 6) okdepth = (depth == 8 || depth == 16);
            else if (ctype == 3) okdepth = (depth == 1 || depth == 2 || depth == 4 || depth == 8);
            if (!okdepth) { snprintf(err, esz, "bit depth %d is not legal for colour type %d", depth, ctype); free(idat); return -1; }
            if (W <= 0 || H <= 0 || W > MAX_IMAGE_SIDE || H > MAX_IMAGE_SIDE) {
                snprintf(err, esz, "PNG says %dx%d, outside 1..%d", W, H, MAX_IMAGE_SIDE);
                free(idat);
                return -1;
            }
            if ((unsigned long long)W * (unsigned long long)H > MAX_IMAGE_PIXELS) {
                snprintf(err, esz, "%dx%d is %llu pixels; this viewer caps at %u",
                         W, H, (unsigned long long)W * (unsigned long long)H, MAX_IMAGE_PIXELS);
                free(idat);
                return -1;
            }
            saw_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (!saw_ihdr) { snprintf(err, esz, "PLTE before IHDR"); free(idat); return -1; }
            if (clen == 0 || clen % 3u != 0 || clen > 256u * 3u) {
                snprintf(err, esz, "PLTE length %u is not 3..768 and a multiple of 3", (unsigned)clen);
                free(idat);
                return -1;
            }
            pal_n = (int)(clen / 3u);
            memcpy(pal, data, clen);
            saw_plte = 1;
        } else if (!memcmp(type, "tRNS", 4)) {
            if (!saw_ihdr) { snprintf(err, esz, "tRNS before IHDR"); free(idat); return -1; }
            if (clen > sizeof(trns)) {
                snprintf(err, esz, "tRNS is %u bytes, too long", (unsigned)clen);
                free(idat);
                return -1;
            }
            memcpy(trns, data, clen);
            trns_len = (int)clen;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!saw_ihdr) { snprintf(err, esz, "IDAT before IHDR"); free(idat); return -1; }
            if (ctype == 3 && !saw_plte) { snprintf(err, esz, "palette image has no PLTE before IDAT"); free(idat); return -1; }
            if (idat_len + clen + 1u > idat_cap) {
                size_t cap = idat_cap ? idat_cap : 8192u;
                while (cap < idat_len + clen + 1u) cap *= 2u;
                u8 *np = realloc(idat, cap);
                if (!np) { snprintf(err, esz, "out of memory for the IDAT stream"); free(idat); return -1; }
                idat = np;
                idat_cap = cap;
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            saw_iend = 1;
            pos += 12u + (size_t)clen;
            break;
        } else if (critical) {
            snprintf(err, esz, "unknown critical chunk %.4s", (const char *)type);
            free(idat);
            return -1;
        }
        pos += 12u + (size_t)clen;
    }

    if (!saw_ihdr) { snprintf(err, esz, "no IHDR chunk"); free(idat); return -1; }
    if (!saw_iend) { snprintf(err, esz, "no IEND chunk (file is truncated)"); free(idat); return -1; }
    if (ctype == 3 && pal_n == 0) { snprintf(err, esz, "palette image has no PLTE chunk"); free(idat); return -1; }
    if (idat_len < 6) { snprintf(err, esz, "no pixel data (IDAT is %lu bytes)", (unsigned long)idat_len); free(idat); return -1; }

    /* zlib wrapper */
    u8 cmf = idat[0], flg = idat[1];
    if ((cmf & 0x0Fu) != 8) {
        snprintf(err, esz, "zlib compression method %u is not deflate", cmf & 0x0Fu);
        free(idat);
        return -1;
    }
    if ((cmf >> 4) > 7u) { snprintf(err, esz, "zlib window size code %u is invalid", cmf >> 4); free(idat); return -1; }
    if ((((unsigned)cmf << 8) | flg) % 31u != 0) {
        snprintf(err, esz, "zlib header check failed (0x%02X%02X)", cmf, flg);
        free(idat);
        return -1;
    }
    if (flg & 0x20u) { snprintf(err, esz, "zlib preset dictionary is not supported"); free(idat); return -1; }

    int ch = png_channels(ctype);
    int bitspp = ch * depth;
    size_t rawlen = png_pass_bytes(W, H, bitspp, interlace);
    if (rawlen == 0 || rawlen > MAX_RAW_BYTES) {
        snprintf(err, esz, "decompressed size %lu is outside the %u-byte limit",
                 (unsigned long)rawlen, (unsigned)MAX_RAW_BYTES);
        free(idat);
        return -1;
    }
    u8 *raw = malloc(rawlen);
    if (!raw) {
        snprintf(err, esz, "out of memory for %lu decompressed bytes", (unsigned long)rawlen);
        free(idat);
        return -1;
    }

    VZ z;
    memset(&z, 0, sizeof(z));
    z.in = idat + 2;
    z.len = idat_len - 6;                     /* the 2 header bytes and 4 adler bytes */
    z.out = raw;
    z.cap = rawlen;
    if (vz_inflate(&z) != 0) {
        snprintf(err, esz, "%s", z.why ? z.why : "invalid deflate stream");
        free(raw);
        free(idat);
        return -1;
    }
    if (z.n != rawlen) {
        snprintf(err, esz, "pixel stream is %lu bytes, the header needs %lu",
                 (unsigned long)z.n, (unsigned long)rawlen);
        free(raw);
        free(idat);
        return -1;
    }
    u32 want_adler = rd_be32(idat + idat_len - 4);
    u32 got_adler = adler32_of(raw, rawlen);
    if (want_adler != got_adler) {
        snprintf(err, esz, "adler32 mismatch (stored %08X, computed %08X)", want_adler, got_adler);
        free(raw);
        free(idat);
        return -1;
    }
    free(idat);

    u32 *pix = malloc((size_t)W * (size_t)H * sizeof(u32));
    if (!pix) {
        snprintf(err, esz, "out of memory for %dx%d pixels", W, H);
        free(raw);
        return -1;
    }

    int has_alpha = 0;
    int filt_bpp = (bitspp + 7) / 8;
    if (filt_bpp < 1) filt_bpp = 1;

    if (!interlace) {
        size_t rowbytes = ((size_t)W * (size_t)bitspp + 7u) / 8u;
        int bad = unfilter_rows(raw, H, rowbytes, filt_bpp);
        if (bad >= 0) {
            snprintf(err, esz, "row %d uses filter %u; PNG allows 0..4",
                     bad, raw[(size_t)bad * (rowbytes + 1u)]);
            free(raw);
            free(pix);
            return -1;
        }
        png_convert_pass(raw, W, H, depth, ctype, W, 0, 1, 0, 1,
                         pal, pal_n, trns, trns_len, pix, &has_alpha);
    } else {
        size_t off = 0;
        for (int p = 0; p < 7; p++) {
            int x0 = PNG_PASS[p].x0, y0 = PNG_PASS[p].y0;
            int dx = PNG_PASS[p].dx, dy = PNG_PASS[p].dy;
            if (W <= x0 || H <= y0) continue;
            int pw = (W - x0 + dx - 1) / dx;
            int ph = (H - y0 + dy - 1) / dy;
            if (pw <= 0 || ph <= 0) continue;
            size_t rowbytes = ((size_t)pw * (size_t)bitspp + 7u) / 8u;
            int bad = unfilter_rows(raw + off, ph, rowbytes, filt_bpp);
            if (bad >= 0) {
                snprintf(err, esz, "pass %d row %d uses filter %u; PNG allows 0..4",
                         p + 1, bad, raw[off + (size_t)bad * (rowbytes + 1u)]);
                free(raw);
                free(pix);
                return -1;
            }
            png_convert_pass(raw + off, pw, ph, depth, ctype, W, x0, dx, y0, dy,
                             pal, pal_n, trns, trns_len, pix, &has_alpha);
            off += (rowbytes + 1u) * (size_t)ph;
        }
    }
    free(raw);

    im->w = W;
    im->h = H;
    im->px = pix;
    im->opaque = !has_alpha;

    const char *cn = ctype == 0 ? "greyscale" :
                     ctype == 2 ? "RGB" :
                     ctype == 3 ? "palette" :
                     ctype == 4 ? "grey+alpha" : "RGBA";
    snprintf(im->fmt, sizeof(im->fmt), "PNG %d-bit %s%s",
             depth, cn, interlace ? ", Adam7 interlaced" : ", non-interlaced");
    return 0;
}

/* ============================================================
 *  BMP - 24- and 32-bit uncompressed BI_RGB
 * ============================================================ */

static int bmp_decode(const u8 *d, size_t n, VImage *im, char *err, size_t esz) {
    memset(im, 0, sizeof(*im));
    if (n < 26 || d[0] != 'B' || d[1] != 'M') {
        snprintf(err, esz, "not a BMP (no BM signature)");
        return -1;
    }
    u32 off = rd_le32(d + 10);
    u32 dib = rd_le32(d + 14);
    int W, H, topdown = 0, bpp;
    u32 compression = 0;

    if (dib == 12) {                                  /* BITMAPCOREHEADER */
        if (n < 26) { snprintf(err, esz, "core header is cut off"); return -1; }
        W = (int)rd_le16(d + 18);
        H = (int)rd_le16(d + 20);
        bpp = (int)rd_le16(d + 24);
        if (rd_le16(d + 22) != 1) { snprintf(err, esz, "BMP planes is not 1"); return -1; }
    } else if (dib >= 40) {                           /* INFOHEADER or later */
        if (n < 54) { snprintf(err, esz, "info header is cut off"); return -1; }
        W = (int)rd_le32(d + 18);
        H = (int)rd_le32(d + 22);
        if (rd_le16(d + 26) != 1) { snprintf(err, esz, "BMP planes is not 1"); return -1; }
        bpp = (int)rd_le16(d + 28);
        compression = rd_le32(d + 30);
        if (H < 0) { topdown = 1; H = -H; }
    } else {
        snprintf(err, esz, "BMP header size %u is not a format this decoder knows", dib);
        return -1;
    }

    if (compression != 0) {
        snprintf(err, esz, "BMP compression %u is not BI_RGB (uncompressed)", compression);
        return -1;
    }
    if (bpp != 24 && bpp != 32) {
        snprintf(err, esz, "BMP is %d-bit; this decoder reads 24- and 32-bit BI_RGB", bpp);
        return -1;
    }
    if (W <= 0 || H <= 0) { snprintf(err, esz, "BMP says %dx%d", W, H); return -1; }
    if (W > MAX_IMAGE_SIDE || H > MAX_IMAGE_SIDE ||
        (unsigned long long)W * (unsigned long long)H > MAX_IMAGE_PIXELS) {
        snprintf(err, esz, "BMP %dx%d is larger than this viewer's pixel cap", W, H);
        return -1;
    }

    size_t rowbytes = (((size_t)W * (size_t)bpp + 31u) / 32u) * 4u;
    if (off > n || rowbytes > n || (size_t)H > (n - off) / rowbytes) {
        snprintf(err, esz, "BMP pixel data is cut off (%lu bytes of rows needed, %lu present)",
                 (unsigned long)(rowbytes * (size_t)H), (unsigned long)(n - (off <= n ? off : n)));
        return -1;
    }

    u32 *pix = malloc((size_t)W * (size_t)H * sizeof(u32));
    if (!pix) { snprintf(err, esz, "out of memory for %dx%d pixels", W, H); return -1; }

    for (int y = 0; y < H; y++) {
        int sy = topdown ? y : H - 1 - y;
        const u8 *src = d + off + (size_t)sy * rowbytes;
        u32 *dst = pix + (size_t)y * (size_t)W;
        for (int x = 0; x < W; x++) {
            const u8 *p = src + (size_t)x * (size_t)(bpp / 8);
            /* BI_RGB 32-bit stores B,G,R and one unused byte, so alpha is
             * opaque by definition - not the fourth byte. */
            dst[x] = pix_rgba(p[2], p[1], p[0], 255);
        }
    }

    im->w = W;
    im->h = H;
    im->px = pix;
    im->opaque = 1;
    snprintf(im->fmt, sizeof(im->fmt), "BMP %d-bit BI_RGB, %s",
             bpp, topdown ? "top-down" : "bottom-up");
    return 0;
}

/* ============================================================
 *  PNM - PBM / PGM / PPM, ASCII and binary
 * ============================================================ */

typedef struct {
    const u8 *p;
    size_t n, i;
} PnmCur;

static int pnm_skip(PnmCur *c) {
    for (;;) {
        while (c->i < c->n && (c->p[c->i] == ' ' || c->p[c->i] == '\t' ||
                               c->p[c->i] == '\r' || c->p[c->i] == '\n'))
            c->i++;
        if (c->i < c->n && c->p[c->i] == '#') {
            while (c->i < c->n && c->p[c->i] != '\n') c->i++;
            continue;
        }
        break;
    }
    return 0;
}

static int pnm_int(PnmCur *c, long *out) {
    pnm_skip(c);
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

/* One whitespace byte separates the header from the binary raster. Tolerate
 * CRLF because a file written on Windows has two. */
static int pnm_binary_start(PnmCur *c) {
    if (c->i >= c->n) return -1;
    if (c->p[c->i] == '\r' && c->i + 1 < c->n && c->p[c->i + 1] == '\n') { c->i += 2; return 0; }
    if (c->p[c->i] == ' ' || c->p[c->i] == '\t' || c->p[c->i] == '\r' || c->p[c->i] == '\n') {
        c->i++;
        return 0;
    }
    return -1;
}

static int pnm_decode(const u8 *d, size_t n, VImage *im, char *err, size_t esz) {
    memset(im, 0, sizeof(*im));
    if (n < 2 || d[0] != 'P' || d[1] < '1' || d[1] > '6') {
        snprintf(err, esz, "not a PPM/PGM/PBM file");
        return -1;
    }
    int kind = d[1] - '0';
    PnmCur c;
    c.p = d;
    c.n = n;
    c.i = 2;

    long W = 0, H = 0, maxv = 1;
    if (pnm_int(&c, &W) != 0 || pnm_int(&c, &H) != 0) {
        snprintf(err, esz, "PNM header: width or height is missing");
        return -1;
    }
    if (kind == 2 || kind == 3 || kind == 5 || kind == 6) {
        if (pnm_int(&c, &maxv) != 0) {
            snprintf(err, esz, "PNM header: maxval is missing");
            return -1;
        }
        if (maxv < 1 || maxv > 65535) {
            snprintf(err, esz, "PNM maxval %ld is outside 1..65535", maxv);
            return -1;
        }
    }
    if (W <= 0 || H <= 0 || W > MAX_IMAGE_SIDE || H > MAX_IMAGE_SIDE ||
        (unsigned long long)W * (unsigned long long)H > MAX_IMAGE_PIXELS) {
        snprintf(err, esz, "PNM says %ldx%ld, outside this viewer's limits", W, H);
        return -1;
    }
    unsigned long long npix = (unsigned long long)W * (unsigned long long)H;
    u32 *pix = malloc((size_t)npix * sizeof(u32));
    if (!pix) {
        snprintf(err, esz, "out of memory for %ldx%ld pixels", W, H);
        return -1;
    }

    int iscolour = (kind == 3 || kind == 6);
    int isbitmap = (kind == 1 || kind == 4);
    int binary = (kind >= 4);
    int samp = binary ? (maxv < 256 ? 1 : 2) : 0;

    if (binary) {
        if (pnm_binary_start(&c) != 0) {
            snprintf(err, esz, "PNM header is not followed by a whitespace byte");
            free(pix);
            return -1;
        }
        size_t need;
        if (isbitmap) need = (((size_t)W + 7u) / 8u) * (size_t)H;
        else need = (size_t)W * (size_t)H * (size_t)(iscolour ? 3 : 1) * (size_t)samp;
        if (need > n - c.i) {
            snprintf(err, esz, "PNM raster is cut off (%lu bytes needed, %lu present)",
                     (unsigned long)need, (unsigned long)(n - c.i));
            free(pix);
            return -1;
        }
    }

    for (long y = 0; y < H; y++) {
        u32 *row = pix + (size_t)y * (size_t)W;
        for (long x = 0; x < W; x++) {
            unsigned r = 0, g = 0, b = 0;
            if (isbitmap) {
                unsigned bit;
                if (binary) {
                    size_t bitpos = (size_t)y * (((size_t)W + 7u) / 8u) * 8u + (size_t)x;
                    bit = (d[c.i + (bitpos >> 3)] >> (7 - (bitpos & 7))) & 1u;
                } else {
                    long v;
                    if (pnm_int(&c, &v) != 0) {
                        snprintf(err, esz, "PBM raster ended at row %ld, column %ld", y + 1, x + 1);
                        free(pix);
                        return -1;
                    }
                    bit = (unsigned)(v != 0);
                }
                unsigned char g8 = bit ? 0 : 255;   /* PBM: 1 is black */
                r = g = b = g8;
            } else if (binary) {
                const u8 *p = d + c.i;
                if (iscolour) {
                    for (int ch = 0; ch < 3; ch++) {
                        unsigned v = samp == 1 ? p[ch] : ((unsigned)p[ch * 2] << 8) | p[ch * 2 + 1];
                        unsigned v8 = (unsigned)((v * 255u + (unsigned)maxv / 2u) / (unsigned)maxv);
                        if (maxv == 1) v8 = v ? 255u : 0u;
                        if (ch == 0) r = v8; else if (ch == 1) g = v8; else b = v8;
                    }
                    c.i += (size_t)3 * (size_t)samp;
                } else {
                    unsigned v = samp == 1 ? p[0] : ((unsigned)p[0] << 8) | p[1];
                    unsigned v8 = (unsigned)((v * 255u + (unsigned)maxv / 2u) / (unsigned)maxv);
                    if (maxv == 1) v8 = v ? 255u : 0u;
                    r = g = b = v8;
                    c.i += (size_t)samp;
                }
            } else {
                long v[3] = { 0, 0, 0 };
                int ns = iscolour ? 3 : 1;
                for (int ch = 0; ch < ns; ch++) {
                    if (pnm_int(&c, &v[ch]) != 0) {
                        snprintf(err, esz, "PNM raster ended at row %ld, column %ld", y + 1, x + 1);
                        free(pix);
                        return -1;
                    }
                    if (v[ch] < 0 || v[ch] > maxv) {
                        snprintf(err, esz, "PNM sample %ld is outside 0..%ld", v[ch], maxv);
                        free(pix);
                        return -1;
                    }
                }
                if (iscolour) {
                    r = (unsigned)((v[0] * 255L + maxv / 2) / maxv);
                    g = (unsigned)((v[1] * 255L + maxv / 2) / maxv);
                    b = (unsigned)((v[2] * 255L + maxv / 2) / maxv);
                } else {
                    r = g = b = (unsigned)((v[0] * 255L + maxv / 2) / maxv);
                }
            }
            row[x] = pix_rgba((u8)r, (u8)g, (u8)b, 255);
        }
    }

    im->w = (int)W;
    im->h = (int)H;
    im->px = pix;
    im->opaque = 1;
    snprintf(im->fmt, sizeof(im->fmt), "%s P%d, maxval %ld, %s",
             isbitmap ? "PBM" : iscolour ? "PPM" : "PGM",
             kind, isbitmap ? 1L : maxv, binary ? "binary" : "ASCII");
    return 0;
}

/* ============================================================
 *  text
 * ============================================================ */

/* Display width of one line: tabs advance to the next multiple of 8,
 * CR is ignored, other control bytes take one cell, and UTF-8 continuation
 * bytes take none (the lead byte already counted the glyph). */
static int line_cols(const u8 *s, size_t len) {
    int col = 0;
    for (size_t i = 0; i < len; i++) {
        u8 c = s[i];
        if (c == '\t') col += 8 - (col % 8);
        else if (c == '\r') continue;
        else if ((c & 0xC0u) == 0x80u) continue;
        else col++;
    }
    return col;
}

/* Build the part of a line that starts at column col_off into out[].
 * Tabs are expanded, control bytes become spaces, UTF-8 sequences are kept
 * whole (a glyph that begins before col_off is skipped entirely so the
 * output is never a broken encoding). Returns the bytes written. */
static size_t build_display(const u8 *s, size_t len, int col_off, int maxcols,
                            char *out, size_t outsz) {
    size_t o = 0;
    int col = 0;
    size_t i = 0;
    while (i < len && col - col_off < maxcols) {
        u8 c = s[i];
        if (c == '\r') { i++; continue; }
        if (c == '\t') {
            int target = col + (8 - (col % 8));
            while (col < target) {
                if (col >= col_off && o + 1u < outsz) out[o++] = ' ';
                col++;
                if (col - col_off >= maxcols) break;
            }
            i++;
            continue;
        }
        if (c < 0x20) {
            if (col >= col_off && o + 1u < outsz) out[o++] = ' ';
            col++;
            i++;
            continue;
        }
        if (c < 0x80) {
            if (col >= col_off && o + 1u < outsz) out[o++] = (char)c;
            col++;
            i++;
            continue;
        }
        /* UTF-8: keep the whole sequence together. */
        size_t need = 1;
        if ((c & 0xE0u) == 0xC0u) need = 2;
        else if ((c & 0xF0u) == 0xE0u) need = 3;
        else if ((c & 0xF8u) == 0xF0u) need = 4;
        if (i + need > len) need = 1;
        else if (need > 1) {
            for (size_t k = 1; k < need; k++)
                if ((s[i + k] & 0xC0u) != 0x80u) { need = 1; break; }
        }
        if (need == 1) {
            if (col >= col_off && o + 1u < outsz) out[o++] = '?';
            col++;
            i++;
        } else {
            if (col >= col_off && o + need < outsz) {
                memcpy(out + o, s + i, need);
                o += need;
            }
            col++;
            i += need;
        }
    }
    out[o] = 0;
    return o;
}

static int load_text_file(Viewer *v, char *err, size_t esz) {
    u8 *buf = NULL;
    size_t len = 0;
    int trunc = 0;
    if (read_file_cap(v->path, MAX_TEXT_BYTES, &buf, &len, &trunc, err, esz) != 0)
        return -1;
    if (v->have_st && v->st.st_size > (off_t)MAX_TEXT_BYTES)
        trunc = 1;
    if (len == 0) {
        free(buf);
        snprintf(err, esz, "the file is empty (0 bytes)");
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == 0) {
            free(buf);
            snprintf(err, esz, "the file contains NUL bytes, so it is not text");
            return -1;
        }
    }

    int nl = 1;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n') nl++;

    u32 *offs = malloc(((size_t)nl + 1u) * sizeof(u32));
    if (!offs) {
        free(buf);
        snprintf(err, esz, "out of memory for %d line offsets", nl);
        return -1;
    }
    offs[0] = 0;
    int line = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n') offs[++line] = (u32)(i + 1u);
    offs[nl] = (u32)len;

    int max_cols = 0;
    for (int l = 0; l < nl; l++) {
        size_t start = offs[l];
        size_t end = offs[l + 1];
        if (end > start && buf[end - 1] == '\n') end--;
        int c = line_cols(buf + start, end - start);
        if (c > max_cols) max_cols = c;
    }

    v->text = buf;
    v->text_len = len;
    v->truncated = trunc;
    v->line_off = offs;
    v->nlines = nl;
    v->max_cols = max_cols;
    return 0;
}

/* ============================================================
 *  what kind of file is this?
 *
 *  The registry in src/filetypes.c is the authority when it is
 *  present. This local detector is the fallback: it looks at the
 *  bytes first and the name second, exactly like the registry.
 * ============================================================ */

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int ends_with_ci(const char *s, const char *tail) {
    size_t ls = strlen(s), lt = strlen(tail);
    if (lt > ls) return 0;
    return ci_equal(s + ls - lt, tail);
}

static int looks_text(const u8 *h, size_t n) {
    if (n == 0) return 0;
    size_t lim = n < 4096 ? n : 4096;
    size_t good = 0;
    for (size_t i = 0; i < lim; i++) {
        u8 c = h[i];
        if (c == 0) return 0;
        if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127) || c >= 128) good++;
    }
    return good * 100u >= lim * 95u;
}

typedef struct {
    const char *name;
    const char *ext[4];
} TextExt;

static const TextExt TEXT_EXT[] = {
    { "text",   { ".txt", ".text", ".log", ".asc" } },
    { "Markdown", { ".md", ".markdown", NULL, NULL } },
    { "C source", { ".c", ".h", NULL, NULL } },
    { "source", { ".cc", ".cpp", ".hpp", ".cxx" } },
    { "shell script", { ".sh", ".bash", ".zsh", NULL } },
    { "config", { ".conf", ".cfg", ".ini", ".toml" } },
    { "JSON",   { ".json", NULL, NULL, NULL } },
    { "XML",    { ".xml", ".svg", ".html", ".htm" } },
    { "Python", { ".py", NULL, NULL, NULL } },
    { "Makefile", { "Makefile", "makefile", NULL, NULL } },
    { NULL, { NULL, NULL, NULL, NULL } }
};

static void local_detect(Viewer *v, const u8 *h, size_t n) {
    v->cat = VCAT_UNKNOWN;
    v->type_known = 0;
    snprintf(v->type_name, sizeof(v->type_name), "unrecognised bytes");
    v->type_note[0] = 0;

    if (n >= 8 && !memcmp(h, PNG_SIG, 8)) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "PNG image");
    } else if (n >= 2 && h[0] == 'B' && h[1] == 'M') {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "BMP image");
    } else if (n >= 2 && h[0] == 'P' && h[1] >= '1' && h[1] <= '6') {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "PNM image (P%c)", h[1]);
    } else if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "JPEG image");
    } else if (n >= 6 && (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6))) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "GIF image");
    } else if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4)) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "WebP image");
    } else if (n >= 4 && (h[0] == 'I' && h[1] == 'I' && h[2] == 42 && h[3] == 0)) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "TIFF image (little-endian)");
    } else if (n >= 4 && (h[0] == 'M' && h[1] == 'M' && h[2] == 0 && h[3] == 42)) {
        v->cat = VCAT_IMAGE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "TIFF image (big-endian)");
    } else if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "AVI ", 4)) {
        v->cat = VCAT_VIDEO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "AVI video");
    } else if (n >= 4 && h[0] == 0x1A && h[1] == 0x45 && h[2] == 0xDF && h[3] == 0xA3) {
        v->cat = VCAT_VIDEO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "Matroska/WebM video");
    } else if (n >= 8 && !memcmp(h + 4, "ftyp", 4)) {
        v->cat = VCAT_VIDEO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "ISO base media (MP4/HEIF)");
    } else if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        v->cat = VCAT_AUDIO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "WAV audio");
    } else if (n >= 4 && !memcmp(h, "OggS", 4)) {
        v->cat = VCAT_AUDIO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "Ogg media");
    } else if (n >= 4 && !memcmp(h, "fLaC", 4)) {
        v->cat = VCAT_AUDIO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "FLAC audio");
    } else if (n >= 3 && !memcmp(h, "ID3", 3)) {
        v->cat = VCAT_AUDIO; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "MP3 audio (ID3 tag)");
    } else if (n >= 5 && !memcmp(h, "%PDF", 5)) {
        v->cat = VCAT_PDF; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "PDF document");
    } else if (n >= 4 && h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5) && (h[3] == 4 || h[3] == 6)) {
        v->cat = VCAT_ARCHIVE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "ZIP archive");
    } else if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B) {
        v->cat = VCAT_ARCHIVE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "gzip archive");
    } else if (n >= 3 && !memcmp(h, "BZh", 3)) {
        v->cat = VCAT_ARCHIVE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "bzip2 archive");
    } else if (n >= 6 && !memcmp(h, "\xFD\x37\x7A\x58\x5A\x00", 6)) {
        v->cat = VCAT_ARCHIVE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "xz archive");
    } else if (n >= 6 && !memcmp(h, "7z\xBC\xAF\x27\x1C", 6)) {
        v->cat = VCAT_ARCHIVE; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "7-Zip archive");
    } else if (n >= 4 && !memcmp(h, "\x7F" "ELF", 4)) {
        v->cat = VCAT_BINARY; v->type_known = 1;
        snprintf(v->type_name, sizeof(v->type_name), "ELF executable");
    }

    if (v->cat == VCAT_UNKNOWN) {
        for (int i = 0; TEXT_EXT[i].name; i++) {
            for (int k = 0; k < 4 && TEXT_EXT[i].ext[k]; k++) {
                if (ends_with_ci(v->path, TEXT_EXT[i].ext[k])) {
                    v->cat = VCAT_TEXT; v->type_known = 1;
                    snprintf(v->type_name, sizeof(v->type_name), "%s", TEXT_EXT[i].name);
                    return;
                }
            }
        }
        if (looks_text(h, n)) {
            v->cat = VCAT_TEXT; v->type_known = 1;
            snprintf(v->type_name, sizeof(v->type_name), "text (no NUL bytes, printable)");
        } else if (n > 0) {
            v->cat = VCAT_BINARY; v->type_known = 1;
            snprintf(v->type_name, sizeof(v->type_name), "unrecognised binary data");
        }
    }
}

#ifdef VIEWER_HAVE_FILETYPES
/* The registry answers from the name (that is its brief), so the viewer
 * still sniffs the bytes itself before decoding. The symbol is weak so the
 * app also links on a tree whose link line has not picked up
 * src/filetypes.c yet: the registry is preferred when it is present, and
 * the local detector is the honest fallback when it is not. */
extern const FileType *filetype_for(const char *name) __attribute__((weak));

static int viewer_registry_type(Viewer *v) {
    if (!filetype_for) return -1;             /* src/filetypes.c not linked */
    const FileType *t = filetype_for(v->path);
    if (!t) return -1;
    FileKind k = filetype_kind(t);
    const char *label = filetype_label(t);

    if (k == FT_KIND_UNKNOWN) return -1;      /* let the bytes answer */

    v->type_known = 1;
    snprintf(v->type_name, sizeof(v->type_name), "%s", label);
    switch (k) {
    case FT_KIND_TEXT:
    case FT_KIND_CODE:
        v->cat = VCAT_TEXT;
        break;
    case FT_KIND_IMAGE:
        v->cat = VCAT_IMAGE;
        break;
    case FT_KIND_AUDIO:
        v->cat = VCAT_AUDIO;
        break;
    case FT_KIND_VIDEO:
        v->cat = VCAT_VIDEO;
        break;
    case FT_KIND_DOCUMENT:
        v->cat = (strstr(label, "PDF") != NULL) ? VCAT_PDF : VCAT_DOCUMENT;
        break;
    case FT_KIND_ARCHIVE:
        v->cat = VCAT_ARCHIVE;
        break;
    case FT_KIND_DATA:
    case FT_KIND_EXECUTABLE:
        v->cat = VCAT_BINARY;
        break;
    default:
        v->cat = VCAT_UNKNOWN;
        break;
    }
    return 0;
}
#endif

static void viewer_detect_type(Viewer *v, const u8 *h, size_t n) {
#ifdef VIEWER_HAVE_FILETYPES
    if (viewer_registry_type(v) == 0) return;
#endif
    local_detect(v, h, n);
}

/* ============================================================
 *  loading
 * ============================================================ */

static void set_basename(Viewer *v) {
    const char *p = v->path;
    const char *slash = strrchr(p, '/');
    if (slash && slash[1]) p = slash + 1;
    snprintf(v->name, sizeof(v->name), "%s", p);
    if (!v->name[0]) snprintf(v->name, sizeof(v->name), "(unnamed)");
}

static int load_image_file(Viewer *v, int (*dec)(const u8 *, size_t, VImage *, char *, size_t),
                           const char *label, char *err, size_t esz) {
    if (v->have_st && (unsigned long long)v->st.st_size > MAX_IMAGE_FILE) {
        snprintf(err, esz, "%s is %llu bytes, above this viewer's %u-byte decode cap",
                 label, (unsigned long long)v->st.st_size, (unsigned)MAX_IMAGE_FILE);
        return -1;
    }
    u8 *buf = NULL;
    size_t len = 0;
    int trunc = 0;
    if (read_file_cap(v->path, MAX_IMAGE_FILE, &buf, &len, &trunc, err, esz) != 0)
        return -1;
    if (trunc) {
        free(buf);
        snprintf(err, esz, "%s is larger than the %u-byte cap", label, (unsigned)MAX_IMAGE_FILE);
        return -1;
    }
    int r = dec(buf, len, &v->img, err, esz);
    free(buf);
    return r;
}

static void load_document(Viewer *v, const char *path) {
    memset(v, 0, sizeof(*v));
    v->cat = VCAT_UNKNOWN;
    v->zoom = 0.0f;
    v->kind = DOC_NONE;

    if (!path || !*path) {
        v->kind = DOC_ERROR;
        snprintf(v->error, sizeof(v->error),
                 "no file name was given. Start it as: viewer <path>");
        snprintf(v->name, sizeof(v->name), "(no file)");
        snprintf(v->type_name, sizeof(v->type_name), "nothing to open");
        return;
    }
    snprintf(v->path, sizeof(v->path), "%s", path);
    set_basename(v);

    if (stat(v->path, &v->st) != 0) {
        v->kind = DOC_ERROR;
        snprintf(v->error, sizeof(v->error), "%s", strerror(errno));
        snprintf(v->type_name, sizeof(v->type_name), "unknown");
        return;
    }
    v->have_st = 1;

    if (S_ISDIR(v->st.st_mode)) {
        v->kind = DOC_OTHER;
        v->cat = VCAT_UNKNOWN;
        snprintf(v->type_name, sizeof(v->type_name), "directory");
        snprintf(v->other_note, sizeof(v->other_note),
                 "a directory has no bytes to display; open a file inside it");
        return;
    }
    if (!S_ISREG(v->st.st_mode)) {
        v->kind = DOC_OTHER;
        v->cat = VCAT_UNKNOWN;
        snprintf(v->type_name, sizeof(v->type_name), "special file");
        snprintf(v->other_note, sizeof(v->other_note),
                 "not a regular file; only its facts are shown (nothing is opened)");
        return;
    }

    u8 head[4096];
    size_t got = 0;
    char herr[128];
    if (read_head(v->path, head, sizeof(head), &got, herr, sizeof(herr)) != 0) {
        v->kind = DOC_ERROR;
        snprintf(v->error, sizeof(v->error), "%s", herr);
        snprintf(v->type_name, sizeof(v->type_name), "unknown");
        return;
    }
    viewer_detect_type(v, head, got);

    if (v->st.st_size == 0) {
        v->kind = DOC_ERROR;
        snprintf(v->error, sizeof(v->error), "the file is empty (0 bytes)");
        return;
    }

    char err[256] = { 0 };
    if (got >= 8 && !memcmp(head, PNG_SIG, 8)) {
        v->kind = DOC_IMAGE;
        if (load_image_file(v, png_decode, "the PNG", err, sizeof(err)) != 0) {
            v->kind = DOC_ERROR;
            snprintf(v->error, sizeof(v->error), "PNG: %s", err);
        }
        return;
    }
    if (got >= 2 && head[0] == 'B' && head[1] == 'M') {
        v->kind = DOC_IMAGE;
        if (load_image_file(v, bmp_decode, "the BMP", err, sizeof(err)) != 0) {
            v->kind = DOC_ERROR;
            snprintf(v->error, sizeof(v->error), "BMP: %s", err);
        }
        return;
    }
    if (got >= 2 && head[0] == 'P' && head[1] >= '1' && head[1] <= '6') {
        v->kind = DOC_IMAGE;
        if (load_image_file(v, pnm_decode, "the PNM", err, sizeof(err)) != 0) {
            v->kind = DOC_ERROR;
            snprintf(v->error, sizeof(v->error), "PNM: %s", err);
        }
        return;
    }

    if (v->cat == VCAT_TEXT || (v->cat == VCAT_UNKNOWN && looks_text(head, got))) {
        v->kind = DOC_TEXT;
        if (load_text_file(v, err, sizeof(err)) != 0) {
            v->kind = DOC_ERROR;
            snprintf(v->error, sizeof(v->error), "%s", err);
        }
        return;
    }

    v->kind = DOC_OTHER;
    if (!v->other_note[0])
        snprintf(v->other_note, sizeof(v->other_note), "%s", cat_needs(v->cat));
}

/* ============================================================
 *  scaling the decoded image into a pixel buffer
 *
 *  Shared by the window and by the host harness, so the picture
 *  the harness writes is produced by the same code the app uses.
 *  Below 100% it averages the source rectangle behind every
 *  destination pixel (a box filter, with alpha weighting); at or
 *  above 100% it takes the nearest source pixel, which keeps the
 *  pixel readout honest when zoomed in.
 * ============================================================ */

static u32 blend_over(u32 src, u32 dst) {
    unsigned a = GET_A(src);
    if (a == 255) return src | 0xFF000000u;
    if (a == 0) return dst | 0xFF000000u;
    unsigned ia = 255u - a;
    unsigned r = (GET_R(src) * a + GET_R(dst) * ia + 127u) / 255u;
    unsigned g = (GET_G(src) * a + GET_G(dst) * ia + 127u) / 255u;
    unsigned b = (GET_B(src) * a + GET_B(dst) * ia + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void image_draw(const VImage *im, u32 *dst, int dw, int dh, int stride,
                       float zoom, float pan_x, float pan_y, int checker) {
    if (checker) {
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                dst[(size_t)y * (size_t)stride + (size_t)x] =
                    (((x >> 3) + (y >> 3)) & 1) ? C_CHK2 : C_CHK1;
    }
    if (!im || !im->px || im->w <= 0 || im->h <= 0 || zoom <= 0.0f) return;

    int sw = iround((float)im->w * zoom);
    int sh = iround((float)im->h * zoom);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    int ox = (dw - sw) / 2 + iround(pan_x);
    int oy = (dh - sh) / 2 + iround(pan_y);

    int x0 = vmax_i(0, ox), x1 = vmin_i(dw, ox + sw);
    int y0 = vmax_i(0, oy), y1 = vmin_i(dh, oy + sh);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; y++) {
        int iy = y - oy;
        int sy0 = 0, sy1 = im->h;
        if (zoom < 1.0f) {
            sy0 = (int)floorf((float)iy / zoom);
            sy1 = (int)ceilf((float)(iy + 1) / zoom);
            if (sy0 < 0) sy0 = 0;
            if (sy1 > im->h) sy1 = im->h;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > im->h) sy1 = im->h;
        }
        u32 *drow = dst + (size_t)y * (size_t)stride;
        for (int x = x0; x < x1; x++) {
            int ix = x - ox;
            u32 c;
            if (zoom < 1.0f) {
                int sx0 = (int)floorf((float)ix / zoom);
                int sx1 = (int)ceilf((float)(ix + 1) / zoom);
                if (sx0 < 0) sx0 = 0;
                if (sx1 > im->w) sx1 = im->w;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > im->w) sx1 = im->w;
                if (im->opaque) {
                    unsigned long long r = 0, g = 0, b = 0, n = 0;
                    for (int sy = sy0; sy < sy1; sy++) {
                        const u32 *srow = im->px + (size_t)sy * (size_t)im->w;
                        for (int sx = sx0; sx < sx1; sx++) {
                            u32 p = srow[sx];
                            r += GET_R(p); g += GET_G(p); b += GET_B(p);
                            n++;
                        }
                    }
                    if (n == 0) n = 1;
                    c = pix_rgba((u8)(r / n), (u8)(g / n), (u8)(b / n), 255);
                } else {
                    unsigned long long r = 0, g = 0, b = 0, a = 0, n = 0;
                    for (int sy = sy0; sy < sy1; sy++) {
                        const u32 *srow = im->px + (size_t)sy * (size_t)im->w;
                        for (int sx = sx0; sx < sx1; sx++) {
                            u32 p = srow[sx];
                            unsigned pa = GET_A(p);
                            r += (unsigned long long)GET_R(p) * pa;
                            g += (unsigned long long)GET_G(p) * pa;
                            b += (unsigned long long)GET_B(p) * pa;
                            a += pa;
                            n++;
                        }
                    }
                    if (n == 0) n = 1;
                    if (a == 0) c = 0;
                    else c = pix_rgba((u8)(r / a), (u8)(g / a), (u8)(b / a), (u8)(a / n));
                }
            } else {
                int sx = (int)(((float)ix + 0.5f) / zoom);
                int sy = (int)(((float)iy + 0.5f) / zoom);
                if (sx < 0) sx = 0;
                if (sx >= im->w) sx = im->w - 1;
                if (sy < 0) sy = 0;
                if (sy >= im->h) sy = im->h - 1;
                c = im->px[(size_t)sy * (size_t)im->w + (size_t)sx];
            }
            u32 under = drow[x];
            drow[x] = blend_over(c, under);
        }
    }
}

static float image_effective_zoom(const Viewer *v, int cw, int ch) {
    if (v->img.w <= 0 || v->img.h <= 0) return 1.0f;
    if (v->zoom > 0.0f) return v->zoom;
    float f = (float)cw / (float)v->img.w;
    float g = (float)ch / (float)v->img.h;
    if (g < f) f = g;
    if (f > 32.0f) f = 32.0f;     /* a 1x1 icon should still be visible, not 800x */
    if (f < 0.01f) f = 0.01f;
    return f;
}

#if !defined(VIEWER_HOST_TEST)

/* ============================================================
 *  the window
 * ============================================================ */

#define KEY_ESC      1
#define KEY_0       11
#define KEY_MINUS   12
#define KEY_EQUAL   13
#define KEY_Q       16
#define KEY_F       33
#define KEY_HOME   102
#define KEY_UP     103
#define KEY_PAGEUP 104
#define KEY_LEFT   105
#define KEY_RIGHT  106
#define KEY_END    107
#define KEY_DOWN   108
#define KEY_PAGEDOWN 109

typedef struct {
    Font *title, *ui, *small, *mono;
    int   uh, mh, mw;      /* text line heights and mono cell width */
    int   pad, hh, fh;     /* body padding, header height, footer height */
} Chrome;

typedef struct { int x, y, w, h; } VRect;

static int rect_hit(VRect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void chrome_init(Chrome *C) {
    memset(C, 0, sizeof(*C));
    C->title = font_get(FONT_SEMIBOLD, 12);
    C->ui    = font_get(FONT_REGULAR, 10);
    C->small = font_get(FONT_REGULAR, 8);
    C->mono  = font_get(FONT_MONO, 10);
    C->uh = C->ui ? (int)(font_height(C->ui) + 3.0f) : 15;
    C->mh = C->mono ? (int)(font_height(C->mono) + 2.0f) : 15;
    C->mw = C->mono ? (int)(font_width(C->mono, "M") + 0.5f) : 7;
    if (C->uh < 11) C->uh = 11;
    if (C->mh < 11) C->mh = 11;
    if (C->mw < 4) C->mw = 4;
    C->pad = ui_px(8);
    if (C->pad < 4) C->pad = 4;
    C->hh = 2 * C->uh + C->pad + 4;
    C->fh = C->uh + C->pad;
}

typedef struct { VRect header, body, footer; } Layout;

static void layout_make(Layout *L, int vw, int vh, const Chrome *C) {
    L->header.x = 0; L->header.y = 0; L->header.w = vw; L->header.h = C->hh;
    L->footer.x = 0; L->footer.y = vh - C->fh; L->footer.w = vw; L->footer.h = C->fh;
    int by = C->hh + 2;
    int bh = vh - C->hh - C->fh - 4;
    if (bh < 1) bh = 1;
    L->body.x = C->pad;
    L->body.y = by;
    L->body.w = vw - 2 * C->pad;
    L->body.h = bh;
    if (L->body.w < 1) L->body.w = 1;
}

static void elide_head(char *out, size_t osz, Font *f, const char *text, int maxw) {
    out[0] = 0;
    if (!f || !text || maxw <= 0) return;
    if (font_width(f, text) <= (float)maxw) {
        snprintf(out, osz, "%s", text);
        return;
    }
    size_t len = strlen(text);
    char tmp[1200];
    while (len > 0) {
        len--;
        if (len + 4u >= sizeof(tmp)) continue;
        memcpy(tmp, text, len);
        memcpy(tmp + len, "...", 4);
        if (font_width(f, tmp) <= (float)maxw) {
            snprintf(out, osz, "%s", tmp);
            return;
        }
    }
    snprintf(out, osz, "...");
}

static void elide_tail(char *out, size_t osz, Font *f, const char *text, int maxw) {
    out[0] = 0;
    if (!f || !text || maxw <= 0) return;
    size_t len = strlen(text);
    for (size_t s = 0; s < len; s++) {
        if (font_width(f, text + s) <= (float)maxw) {
            snprintf(out, osz, "%s", text + s);
            return;
        }
    }
    snprintf(out, osz, "%s", len ? text + len - 1 : "");
}

static void text_metrics(const Viewer *v, const Chrome *C, VRect body,
                         int *rows, int *cols, int *gutter) {
    int digits = 1;
    int n = v->nlines > 0 ? v->nlines : 1;
    while (n >= 10) { digits++; n /= 10; }
    int g = digits * C->mw + 2 * C->pad;
    if (g > body.w / 2) g = body.w / 2;
    if (g < C->mw + 2) g = C->mw + 2;
    int avail = body.w - g - 8;
    int c = avail / C->mw;
    if (c < 1) c = 1;
    int r = (body.h - 8) / C->mh;
    if (r < 1) r = 1;
    *rows = r;
    *cols = c;
    *gutter = g;
}

static void text_clamp(Viewer *v, const Chrome *C, VRect body) {
    int rows, cols, gutter;
    text_metrics(v, C, body, &rows, &cols, &gutter);
    int maxfirst = vmax_i(0, v->nlines - rows);
    if (v->first_line > maxfirst) v->first_line = maxfirst;
    if (v->first_line < 0) v->first_line = 0;
    int maxcol = vmax_i(0, v->max_cols - cols);
    if (v->col_off > maxcol) v->col_off = maxcol;
    if (v->col_off < 0) v->col_off = 0;
}

static void image_clamp_pan(Viewer *v, float z, int cw, int ch) {
    if (v->img.w <= 0 || v->img.h <= 0) return;
    int sw = iround((float)v->img.w * z);
    int sh = iround((float)v->img.h * z);
    float mx = sw > cw ? (float)(sw - cw) * 0.5f : 0.0f;
    float my = sh > ch ? (float)(sh - ch) * 0.5f : 0.0f;
    if (v->pan_x > mx) v->pan_x = mx;
    if (v->pan_x < -mx) v->pan_x = -mx;
    if (v->pan_y > my) v->pan_y = my;
    if (v->pan_y < -my) v->pan_y = -my;
}

static void fmt_mode(char *out, size_t n, unsigned mode) {
    char p[11];
    p[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' :
           S_ISBLK(mode) ? 'b' : S_ISFIFO(mode) ? 'p' :
#ifdef S_ISSOCK
           S_ISSOCK(mode) ? 's' :
#endif
           '-';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        p[i + 1] = (mode & (1u << (8 - i))) ? rwx[i] : '-';
    p[10] = 0;
    snprintf(out, n, "%s %04o", p, mode & 07777u);
}

static void fmt_time(char *out, size_t n, time_t t) {
    struct tm tmv;
#if defined(_WIN32)
    int ok = localtime_s(&tmv, &t) == 0;
#else
    int ok = localtime_r(&t, &tmv) != NULL;
#endif
    if (ok)
        strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
    else
        snprintf(out, n, "%lld", (long long)t);
}

static int draw_wrapped(Surface *s, Font *f, int x, int y, int maxw, int lh,
                        const char *text, u32 col) {
    if (!f || !text || maxw <= 0) return y;
    char line[256];
    size_t used = 0;
    line[0] = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        size_t wl = (size_t)(p - w);
        if (wl == 0) break;
        char word[160];
        if (wl >= sizeof(word)) wl = sizeof(word) - 1;
        memcpy(word, w, wl);
        word[wl] = 0;
        char trial[420];
        if (used) snprintf(trial, sizeof(trial), "%s %s", line, word);
        else snprintf(trial, sizeof(trial), "%s", word);
        if (font_width(f, trial) > (float)maxw && used) {
            char shown[420];
            elide_head(shown, sizeof(shown), f, line, maxw);
            font_draw(s, f, (float)x, (float)y, shown, col);
            y += lh;
            snprintf(line, sizeof(line), "%s", word);
            used = strlen(line);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
            used = strlen(line);
        }
    }
    if (used) {
        char shown[420];
        elide_head(shown, sizeof(shown), f, line, maxw);
        font_draw(s, f, (float)x, (float)y, shown, col);
        y += lh;
    }
    return y;
}

static void draw_header(Surface *s, Viewer *v, const Chrome *C, int vw) {
    surf_fill(s, 0, 0, vw, C->hh, C_PANEL);
    surf_fill(s, 0, C->hh - 1, vw, 1, C_LINE);

    char right1[200] = "", right2[220] = "";
    if (v->kind == DOC_IMAGE) {
        snprintf(right1, sizeof(right1), "%s", v->img.fmt);
        char sz[48];
        fmt_size(sz, sizeof(sz), (unsigned long long)v->st.st_size);
        snprintf(right2, sizeof(right2), "%dx%d  %s", v->img.w, v->img.h, sz);
    } else if (v->kind == DOC_TEXT) {
        snprintf(right1, sizeof(right1), "%s", v->type_name);
        char sz[48];
        fmt_size(sz, sizeof(sz), (unsigned long long)v->st.st_size);
        snprintf(right2, sizeof(right2), "%d lines  %s", v->nlines, sz);
    } else {
        snprintf(right1, sizeof(right1), "%s", v->type_name);
        if (v->have_st) {
            char sz[48];
            fmt_size(sz, sizeof(sz), (unsigned long long)v->st.st_size);
            char mode[24];
            fmt_mode(mode, sizeof(mode), (unsigned)v->st.st_mode);
            snprintf(right2, sizeof(right2), "%s  %s", sz, mode);
        }
    }

    char r1[200], r2[220];
    elide_head(r1, sizeof(r1), C->small, right1, vw / 2);
    elide_head(r2, sizeof(r2), C->small, right2, vw / 2);
    snprintf(right1, sizeof(right1), "%s", r1);
    snprintf(right2, sizeof(right2), "%s", r2);

    int y1 = C->pad / 2;
    int y2 = y1 + C->uh + 2;

    int rw1 = C->small ? (int)font_width(C->small, right1) : 0;
    int rw2 = C->small ? (int)font_width(C->small, right2) : 0;
    int rw = vmax_i(rw1, rw2);
    int maxw = vw - 2 * C->pad - rw - 10;
    if (maxw < 20) maxw = 20;

    char name[320];
    elide_head(name, sizeof(name), C->title, v->name, maxw);
    if (C->title) font_draw(s, C->title, (float)C->pad, (float)y1, name, C_TEXT);
    if (C->small && right1[0])
        font_draw(s, C->small, (float)(vw - C->pad - rw1), (float)(y1 + 2), right1, C_ACCENT);

    char path[640];
    elide_tail(path, sizeof(path), C->small, v->path, maxw);
    if (C->small) font_draw(s, C->small, (float)C->pad, (float)y2, path, C_DIM);
    if (C->small && right2[0])
        font_draw(s, C->small, (float)(vw - C->pad - rw2), (float)(y2 + 2), right2, C_DIM);
}

static void draw_footer(Surface *s, Viewer *v, const Chrome *C, const Layout *L, int vw, int vh) {
    surf_fill(s, 0, L->footer.y, vw, L->footer.h, C_PANEL);
    surf_fill(s, 0, L->footer.y, vw, 1, C_LINE);
    int y = L->footer.y + (L->footer.h - C->uh) / 2;

    char hint[256];
    char right[200] = "";
    if (v->kind == DOC_IMAGE) {
        float z = image_effective_zoom(v, L->body.w, L->body.h);
        snprintf(hint, sizeof(hint),
                 "wheel: zoom     drag: pan     +/-: zoom     0 or F: fit     Q: quit     "
                 "showing %.1f%%", (double)(z * 100.0f));
        if (rect_hit(L->body, v->mx, v->my)) {
            int sw = iround((float)v->img.w * z);
            int sh = iround((float)v->img.h * z);
            int ox = L->body.x + (L->body.w - sw) / 2 + iround(v->pan_x);
            int oy = L->body.y + (L->body.h - sh) / 2 + iround(v->pan_y);
            int ix = (int)((float)(v->mx - ox) / z);
            int iy = (int)((float)(v->my - oy) / z);
            if (ix >= 0 && ix < v->img.w && iy >= 0 && iy < v->img.h) {
                u32 p = v->img.px[(size_t)iy * (size_t)v->img.w + (size_t)ix];
                snprintf(right, sizeof(right), "x %d  y %d  #%02X%02X%02X  a%d",
                         ix, iy, GET_R(p), GET_G(p), GET_B(p), GET_A(p));
            }
        }
    } else if (v->kind == DOC_TEXT) {
        if (v->truncated) {
            char tot[48], cap[48];
            fmt_size(tot, sizeof(tot), (unsigned long long)v->st.st_size);
            fmt_size(cap, sizeof(cap), (unsigned long long)MAX_TEXT_BYTES);
            snprintf(hint, sizeof(hint),
                     "SHOWING FIRST %s OF %s (truncated)     arrows scroll     PgUp/PgDn     Home/End     Q: quit",
                     cap, tot);
        } else {
            snprintf(hint, sizeof(hint),
                     "up/down: line     left/right: column (shift=8)     PgUp/PgDn     Home/End     Q: quit");
        }
        snprintf(right, sizeof(right), "line %d/%d  col %d  tabs=8",
                 v->first_line + 1, v->nlines, v->col_off);
    } else {
        snprintf(hint, sizeof(hint), "Q: quit");
        snprintf(right, sizeof(right), "read-only");
    }

    int rw = C->small ? (int)font_width(C->small, right) : 0;
    int maxw = vw - 2 * C->pad - rw - 10;
    if (maxw < 20) maxw = 20;
    char left[320];
    elide_head(left, sizeof(left), C->small, hint, maxw);
    if (C->small) font_draw(s, C->small, (float)C->pad, (float)y, left,
                            v->truncated && v->kind == DOC_TEXT ? C_WARN : C_DIM);
    if (C->small && right[0])
        font_draw(s, C->small, (float)(vw - C->pad - rw), (float)y, right, C_DIM);
}

static void draw_text_view(Surface *s, Viewer *v, const Chrome *C, VRect body) {
    surf_fill(s, body.x, body.y, body.w, body.h, C_BG2);
    int rows, cols, gutter;
    text_metrics(v, C, body, &rows, &cols, &gutter);
    text_clamp(v, C, body);

    int first = v->first_line;
    int coloff = v->col_off;
    int hsb = 6;

    surf_fill(s, body.x, body.y, gutter, body.h - hsb, C_GUTTER);
    surf_fill(s, body.x + gutter - 1, body.y, 1, body.h - hsb, C_LINE);

    char *disp = malloc((size_t)cols * 4u + 8u);
    if (!disp) return;

    for (int i = 0; i < rows; i++) {
        int ln = first + i;
        if (ln >= v->nlines) break;
        int y = body.y + i * C->mh;
        char num[32];
        snprintf(num, sizeof(num), "%d", ln + 1);
        if (C->mono)
            font_draw(s, C->mono,
                      (float)(body.x + gutter - C->pad) - font_width(C->mono, num),
                      (float)y, num, C_DIM);

        size_t start = v->line_off[ln];
        size_t end = v->line_off[ln + 1];
        if (end > start && v->text[end - 1] == '\n') end--;
        build_display(v->text + start, end - start, coloff, cols, disp,
                      (size_t)cols * 4u + 8u);
        Surface ts = *s;
        ts.px = s->px + (size_t)y * (size_t)s->stride + (size_t)(body.x + gutter);
        ts.w = body.w - gutter - 8;
        if (ts.w < 1) ts.w = 1;
        ts.h = C->mh;
        surf_unclip(&ts);
        if (C->mono) font_draw(&ts, C->mono, 2.0f, 0.0f, disp, C_TEXT);
    }
    free(disp);

    /* vertical scrollbar: the thumb is the visible fraction of the file */
    int track_x = body.x + body.w - 5;
    int track_y = body.y + 2;
    int track_h = body.h - hsb - 4;
    if (track_h > 4) {
        surf_fill(s, track_x, track_y, 4, track_h, C_GUTTER);
        if (v->nlines > rows && rows > 0) {
            int thumb_h = track_h * rows / v->nlines;
            if (thumb_h < 16) thumb_h = 16;
            if (thumb_h > track_h) thumb_h = track_h;
            int maxt = v->nlines - rows;
            int ty = track_y + (maxt > 0 ? (track_h - thumb_h) * first / maxt : 0);
            surf_fill(s, track_x, ty, 4, thumb_h, C_LINE);
        }
    }

    /* horizontal scrollbar */
    int htrack_x = body.x + gutter;
    int htrack_w = body.w - gutter - 8;
    if (htrack_w > 4) {
        surf_fill(s, htrack_x, body.y + body.h - 4, htrack_w, 4, C_GUTTER);
        if (v->max_cols > cols && cols > 0) {
            int thumb_w = htrack_w * cols / v->max_cols;
            if (thumb_w < 16) thumb_w = 16;
            if (thumb_w > htrack_w) thumb_w = htrack_w;
            int maxt = v->max_cols - cols;
            int tx = htrack_x + (maxt > 0 ? (htrack_w - thumb_w) * coloff / maxt : 0);
            surf_fill(s, tx, body.y + body.h - 4, thumb_w, 4, C_LINE);
        }
    }
}

static void draw_image_view(Surface *s, Viewer *v, const Chrome *C, VRect body) {
    (void)C;
    Surface sub = *s;
    sub.px = s->px + (size_t)body.y * (size_t)s->stride + (size_t)body.x;
    sub.w = body.w;
    sub.h = body.h;
    surf_unclip(&sub);
    float z = image_effective_zoom(v, body.w, body.h);
    image_clamp_pan(v, z, body.w, body.h);
    image_draw(&v->img, sub.px, sub.w, sub.h, sub.stride, z,
               v->pan_x, v->pan_y, 1);
    surf_fill(s, body.x, body.y, body.w, 1, C_LINE);
    surf_fill(s, body.x, body.y + body.h - 1, body.w, 1, C_LINE);
    surf_fill(s, body.x, body.y, 1, body.h, C_LINE);
    surf_fill(s, body.x + body.w - 1, body.y, 1, body.h, C_LINE);
}

static void fact_line(Surface *s, const Chrome *C, int x, int y, int labelw,
                      int maxw, const char *label, const char *value) {
    if (C->mono) font_draw(s, C->mono, (float)x, (float)y, label, C_DIM);
    char buf[600];
    elide_head(buf, sizeof(buf), C->mono, value, maxw);
    if (C->mono) font_draw(s, C->mono, (float)(x + labelw), (float)y, buf, C_TEXT);
}

static void draw_facts(Surface *s, Viewer *v, const Chrome *C, VRect body) {
    surf_fill(s, body.x, body.y, body.w, body.h, C_BG2);
    int x = body.x + C->pad;
    int y = body.y + C->pad;
    int maxw = body.w - 2 * C->pad;
    int labelw = 10 * C->mw;

    if (v->kind == DOC_ERROR) {
        if (C->ui) font_draw(s, C->ui, (float)x, (float)y, "Cannot open this file", C_WARN);
        y += C->uh + 2;
        y = draw_wrapped(s, C->mono, x, y, maxw, C->mh, v->error, C_TEXT);
        y += C->pad;
    } else {
        if (C->ui) font_draw(s, C->ui, (float)x, (float)y, v->type_name, C_TEXT);
        y += C->uh + 2;
        if (v->other_note[0])
            y = draw_wrapped(s, C->mono, x, y, maxw, C->mh, v->other_note, C_ACCENT);
        y += C->pad;
        if (C->small) font_draw(s, C->small, (float)x, (float)y,
                                "the file's own facts", C_DIM);
        y += C->small ? (int)font_height(C->small) + 4 : C->mh;
    }

    char val[600], tmp[96];
    fact_line(s, C, x, y, labelw, maxw - labelw, "path", v->path); y += C->mh;
    fact_line(s, C, x, y, labelw, maxw - labelw, "type", v->type_name); y += C->mh;
    fact_line(s, C, x, y, labelw, maxw - labelw, "category", cat_name(v->cat)); y += C->mh;

    if (v->have_st) {
        fmt_size(tmp, sizeof(tmp), (unsigned long long)v->st.st_size);
        snprintf(val, sizeof(val), "%s (%lld bytes)", tmp, (long long)v->st.st_size);
        fact_line(s, C, x, y, labelw, maxw - labelw, "size", val); y += C->mh;

        fmt_mode(tmp, sizeof(tmp), (unsigned)v->st.st_mode);
        fact_line(s, C, x, y, labelw, maxw - labelw, "mode", tmp); y += C->mh;

        fmt_time(tmp, sizeof(tmp), v->st.st_mtime);
        fact_line(s, C, x, y, labelw, maxw - labelw, "mtime", tmp); y += C->mh;

        snprintf(val, sizeof(val), "%llu", (unsigned long long)v->st.st_ino);
        fact_line(s, C, x, y, labelw, maxw - labelw, "inode", val); y += C->mh;

        snprintf(val, sizeof(val), "%llu  uid %u  gid %u",
                 (unsigned long long)v->st.st_nlink,
                 (unsigned)v->st.st_uid, (unsigned)v->st.st_gid);
        fact_line(s, C, x, y, labelw, maxw - labelw, "links", val); y += C->mh;

    }
}

static void draw_all(MClient *c, Viewer *v, Chrome *C, int vw, int vh) {
    Surface s;
    s.px = c->px;
    s.w = vw;
    s.h = vh;
    s.stride = c->stride;
    surf_unclip(&s);

    gfx_gradient(&s, 0, 0, vw, vh, C_BG, C_BG2);

    Layout L;
    layout_make(&L, vw, vh, C);

    if (v->kind == DOC_IMAGE) draw_image_view(&s, v, C, L.body);
    else if (v->kind == DOC_TEXT) draw_text_view(&s, v, C, L.body);
    else draw_facts(&s, v, C, L.body);

    draw_header(&s, v, C, vw);
    draw_footer(&s, v, C, &L, vw, vh);
}

static void zoom_around(Viewer *v, const Chrome *C, VRect body,
                        int mx, int my, float factor) {
    float z = image_effective_zoom(v, body.w, body.h);
    float nz = z * factor;
    if (nz < 0.05f) nz = 0.05f;
    if (nz > 32.0f) nz = 32.0f;
    if (nz == z) return;

    int sw = iround((float)v->img.w * z);
    int sh = iround((float)v->img.h * z);
    int ox = body.x + (body.w - sw) / 2 + iround(v->pan_x);
    int oy = body.y + (body.h - sh) / 2 + iround(v->pan_y);
    float ix = (float)(mx - ox) / z;
    float iy = (float)(my - oy) / z;

    int nsw = iround((float)v->img.w * nz);
    int nsh = iround((float)v->img.h * nz);
    float nox = (float)mx - ix * nz;
    float noy = (float)my - iy * nz;
    v->zoom = nz;
    v->pan_x = nox - (float)body.x - (float)(body.w - nsw) * 0.5f;
    v->pan_y = noy - (float)body.y - (float)(body.h - nsh) * 0.5f;
    image_clamp_pan(v, nz, body.w, body.h);
}

static void handle_key(Viewer *v, const Chrome *C, int vw, int vh,
                       int key, int mods, int *quit) {
    Layout L;
    layout_make(&L, vw, vh, C);

    if (key == KEY_Q || key == KEY_ESC) { *quit = 1; return; }

    if (v->kind == DOC_IMAGE) {
        int step = (mods & 1) ? 80 : 24;
        switch (key) {
        case KEY_LEFT:  v->pan_x += (float)step; break;
        case KEY_RIGHT: v->pan_x -= (float)step; break;
        case KEY_UP:    v->pan_y += (float)step; break;
        case KEY_DOWN:  v->pan_y -= (float)step; break;
        case KEY_EQUAL:
            zoom_around(v, C, L.body, L.body.x + L.body.w / 2,
                        L.body.y + L.body.h / 2, 1.25f);
            break;
        case KEY_MINUS:
            zoom_around(v, C, L.body, L.body.x + L.body.w / 2,
                        L.body.y + L.body.h / 2, 0.8f);
            break;
        case KEY_0:
        case KEY_F:
            v->zoom = 0.0f;
            v->pan_x = 0.0f;
            v->pan_y = 0.0f;
            break;
        default: break;
        }
        float z = image_effective_zoom(v, L.body.w, L.body.h);
        image_clamp_pan(v, z, L.body.w, L.body.h);
        return;
    }

    if (v->kind != DOC_TEXT) return;

    int rows, cols, gutter;
    text_metrics(v, C, L.body, &rows, &cols, &gutter);
    int page = rows > 1 ? rows - 1 : 1;
    int step = (mods & 1) ? 8 : 1;
    switch (key) {
    case KEY_UP:       v->first_line -= 1; break;
    case KEY_DOWN:     v->first_line += 1; break;
    case KEY_PAGEUP:   v->first_line -= page; break;
    case KEY_PAGEDOWN: v->first_line += page; break;
    case KEY_HOME:     v->first_line = 0; break;
    case KEY_END:      v->first_line = vmax_i(0, v->nlines - rows); break;
    case KEY_LEFT:     v->col_off -= step; break;
    case KEY_RIGHT:    v->col_off += step; break;
    default: break;
    }
    text_clamp(v, C, L.body);
}

static void handle_wheel(Viewer *v, const Chrome *C, int vw, int vh, int notches) {
    Layout L;
    layout_make(&L, vw, vh, C);
    if (notches == 0) return;

    if (v->kind == DOC_IMAGE) {
        float factor = 1.0f;
        int n = notches;
        if (n > 12) n = 12;
        if (n < -12) n = -12;
        while (n > 0) { factor *= 1.25f; n--; }
        while (n < 0) { factor *= 0.8f; n++; }
        int mx = rect_hit(L.body, v->mx, v->my) ? v->mx : L.body.x + L.body.w / 2;
        int my = rect_hit(L.body, v->mx, v->my) ? v->my : L.body.y + L.body.h / 2;
        zoom_around(v, C, L.body, mx, my, factor);
        return;
    }

    if (v->kind == DOC_TEXT) {
        v->first_line -= notches * 3;
        text_clamp(v, C, L.body);
    }
}

#ifndef VIEWER_LAYOUT_TEST

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : NULL;

    MClient c;
    if (mc_open(&c, "viewer", "Viewer", WIN_W, WIN_H) < 0) {
        fprintf(stderr, "viewer: cannot connect to the window server (%s)\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    /* font_init() returns 0 on success and -1 on failure. */
    if (font_init("/usr/share/fonts") != 0) {
        fprintf(stderr, "viewer: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    Chrome C;
    chrome_init(&C);
    if (!C.mono || !C.ui || !C.title) {
        fprintf(stderr, "viewer: the fonts loaded but none of the four faces did\n");
        mc_close(&c);
        return 1;
    }

    Viewer v;
    load_document(&v, path);

    int capw = c.maxw > 0 ? c.maxw : c.w;
    int caph = c.maxh > 0 ? c.maxh : c.h;
    int vw = c.w, vh = c.h;
    int quit = 0;
    int dirty = 1;

    while (!quit) {
        if (dirty) {
            draw_all(&c, &v, &C, vw, vh);
            mc_present(&c);
            dirty = 0;
        }

        MEvent ev;
        while (!quit && mc_poll(&c, &ev)) {
            switch (ev.type) {
            case MC_CLOSE:
                quit = 1;
                break;
            case MC_RESIZE:
                vw = ev.w < 1 ? 1 : (ev.w > capw ? capw : ev.w);
                vh = ev.h < 1 ? 1 : (ev.h > caph ? caph : ev.h);
                dirty = 1;
                break;
            case MC_KEY:
                if (ev.press) {
                    handle_key(&v, &C, vw, vh, ev.key, ev.mods, &quit);
                    dirty = 1;
                }
                break;
            case MC_MOUSE:
                v.mx = ev.x;
                v.my = ev.y;
                if (v.kind == DOC_IMAGE) {
                    Layout L;
                    layout_make(&L, vw, vh, &C);
                    if ((ev.buttons & 1) && rect_hit(L.body, ev.x, ev.y)) {
                        if (!v.dragging) {
                            v.dragging = 1;
                            v.last_mx = ev.x;
                            v.last_my = ev.y;
                        } else {
                            v.pan_x += (float)(ev.x - v.last_mx);
                            v.pan_y += (float)(ev.y - v.last_my);
                            v.last_mx = ev.x;
                            v.last_my = ev.y;
                            float z = image_effective_zoom(&v, L.body.w, L.body.h);
                            image_clamp_pan(&v, z, L.body.w, L.body.h);
                        }
                    } else {
                        v.dragging = 0;
                    }
                }
                dirty = 1;
                break;
            case MC_WHEEL:
                handle_wheel(&v, &C, vw, vh, ev.wheel);
                dirty = 1;
                break;
            default:
                break;
            }
        }
        usleep(30 * 1000);
    }

    viewer_free(&v);
    mc_close(&c);
    return 0;
}

#endif /* !VIEWER_LAYOUT_TEST */
#endif /* !VIEWER_HOST_TEST */

/* ============================================================
 *  host harness
 *
 *  Built with -DVIEWER_HOST_TEST. It compiles out the window and
 *  the fonts and leaves the real decoders, the real text loader
 *  and the real scaling code. It writes the scaled render as a
 *  PNG (stored-block deflate, the same simple writer paint.c
 *  uses) so the result can be opened and looked at.
 * ============================================================ */
#if defined(VIEWER_HOST_TEST) || defined(VIEWER_LAYOUT_TEST)

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static int host_chunk(FILE *f, const char *type, const u8 *data, size_t len) {
    u8 head[8];
    put_be32(head, (u32)len);
    memcpy(head + 4, type, 4);
    if (fwrite(head, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    u8 *tmp = malloc(len + 4u);
    if (!tmp) return -1;
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    u32 c = crc32_of(tmp, len + 4u);   /* the chunk CRC covers type + data */
    free(tmp);
    u8 tail[4];
    put_be32(tail, c);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

static int host_write_png(const char *path, const u32 *px, int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    size_t stride = (size_t)w * 4u;
    size_t rawlen = (stride + 1u) * (size_t)h;
    u8 *raw = malloc(rawlen);
    if (!raw) return -1;
    u8 *q = raw;
    for (int y = 0; y < h; y++) {
        *q++ = 0;
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * (size_t)w + (size_t)x];
            *q++ = GET_R(c);
            *q++ = GET_G(c);
            *q++ = GET_B(c);
            *q++ = 255;
        }
    }
    size_t nblocks = (rawlen + 65534u) / 65535u;
    size_t zcap = rawlen + nblocks * 5u + 8u;
    u8 *z = malloc(zcap);
    if (!z) { free(raw); return -1; }
    size_t zl = 0;
    z[zl++] = 0x78;
    z[zl++] = 0x01;
    size_t off = 0;
    while (off < rawlen) {
        size_t n = rawlen - off;
        if (n > 65535u) n = 65535u;
        int last = (off + n == rawlen);
        z[zl++] = (u8)(last ? 1 : 0);
        z[zl++] = (u8)(n & 0xFFu);
        z[zl++] = (u8)((n >> 8) & 0xFFu);
        z[zl++] = (u8)(~n & 0xFFu);
        z[zl++] = (u8)((~n >> 8) & 0xFFu);
        memcpy(z + zl, raw + off, n);
        zl += n;
        off += n;
    }
    put_be32(z + zl, adler32_of(raw, rawlen));
    zl += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int ok = fwrite(sig, 1, 8, f) == 8;
    if (ok) {
        u8 ihdr[13];
        put_be32(ihdr, (u32)w);
        put_be32(ihdr + 4, (u32)h);
        ihdr[8] = 8;
        ihdr[9] = 6;
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        ok = host_chunk(f, "IHDR", ihdr, sizeof(ihdr)) == 0;
    }
    if (ok) ok = host_chunk(f, "IDAT", z, zl) == 0;
    if (ok) ok = host_chunk(f, "IEND", NULL, 0) == 0;
    if (ok) ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    free(z);
    return ok ? 0 : -1;
}

#endif /* VIEWER_HOST_TEST || VIEWER_LAYOUT_TEST */

#ifdef VIEWER_HOST_TEST

static void host_render(const Viewer *v, const char *out) {
    const int W = 480, H = 320;
    u32 *canvas = malloc((size_t)W * (size_t)H * sizeof(u32));
    if (!canvas) return;
    /* For the render proof, fit both ways (up and down) rather than the
     * app's "never upscale a small image" initial view. */
    float zx = (float)W / (float)v->img.w;
    float zy = (float)H / (float)v->img.h;
    float z = zx < zy ? zx : zy;
    image_draw(&v->img, canvas, W, H, W, z, 0.0f, 0.0f, 1);
    int rc = host_write_png(out, canvas, W, H);
    printf("   render: %s (%dx%d at %.3fx) %s\n", out, W, H, (double)z,
           rc == 0 ? "written" : "FAILED");
    free(canvas);
}

static void host_report(const char *path) {
    Viewer v;
    load_document(&v, path);
    printf("\n== %s\n", path);
    if (v.have_st)
        printf("   stat: %lld bytes, mode %04o\n",
               (long long)v.st.st_size, (unsigned)(v.st.st_mode & 07777u));
    printf("   detected: %s (%s)\n", v.type_name, cat_name(v.cat));

    if (v.kind == DOC_IMAGE) {
        printf("   decoded: %s, %dx%d, opaque=%d\n", v.img.fmt,
               v.img.w, v.img.h, v.img.opaque);
        int pts[3][2] = {
            { 0, 0 },
            { v.img.w / 2, v.img.h / 2 },
            { v.img.w - 1, v.img.h - 1 }
        };
        for (int i = 0; i < 3; i++) {
            u32 p = v.img.px[(size_t)pts[i][1] * (size_t)v.img.w + (size_t)pts[i][0]];
            printf("   pixel(%d,%d) = #%02X%02X%02X%02X  rgba(%d,%d,%d,%d)\n",
                   pts[i][0], pts[i][1],
                   GET_R(p), GET_G(p), GET_B(p), GET_A(p),
                   GET_R(p), GET_G(p), GET_B(p), GET_A(p));
        }
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char stem[256];
        snprintf(stem, sizeof(stem), "%s", base);
        char *dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = 0;
        char out[512];
        snprintf(out, sizeof(out), ".work/viewer/render-%s.png", stem);
        host_render(&v, out);
    } else if (v.kind == DOC_TEXT) {
        printf("   text: %d lines, widest %d columns, first %lu of %lld bytes%s\n",
               v.nlines, v.max_cols, (unsigned long)v.text_len,
               (long long)v.st.st_size, v.truncated ? " (TRUNCATED)" : "");
        int show = v.nlines < 3 ? v.nlines : 3;
        for (int i = 0; i < show; i++) {
            size_t a = v.line_off[i];
            size_t b = v.line_off[i + 1];
            if (b > a && v.text[b - 1] == '\n') b--;
            printf("   line %d: %.*s\n", i + 1, (int)(b - a), (const char *)(v.text + a));
        }
    } else if (v.kind == DOC_ERROR) {
        printf("   ERROR: %s\n", v.error);
    } else {
        printf("   other: %s\n", v.other_note);
    }
    viewer_free(&v);
}

int main(int argc, char **argv) {
    printf("viewer decoder host harness\n");
    printf("same png_decode / bmp_decode / pnm_decode / load_text_file / image_draw\n"
           "that src/client/apps/viewer.c compiles into the app\n");
    if (argc < 2) {
        printf("usage: viewer_host <file> [file...]\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        host_report(argv[i]);
    return 0;
}

#endif /* VIEWER_HOST_TEST */
