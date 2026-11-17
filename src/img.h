/* ============================================================
 *  Mystic OS - img.h
 *
 *  One image decoder for the whole system.
 *
 *  Three apps had three answers to "what is a picture": Files
 *  listed extensions it did not really know, the viewer carried a
 *  PNG/BMP/PNM decoder, and the browser had nothing at all. This
 *  header is the single answer. It decodes into one pixel format,
 *  0xAARRGGBB - the same format the framebuffer and the window
 *  server already use - so no caller has to convert anything.
 *
 *      Img im;
 *      char err[160];
 *      if (img_decode(bytes, len, &im, err, sizeof err) != 0)
 *          show(err);                     // a specific reason
 *      else {
 *          draw(im.px, im.w, im.h, im.stride);
 *          img_free(&im);
 *      }
 *
 *  WHAT IS DECODED, AND HOW COMPLETELY
 *
 *    PNG     all five colour types (0 grey, 2 RGB, 3 palette, 4
 *            grey+alpha, 6 RGBA), every bit depth the format allows
 *            for each (1/2/4/8/16), all five row filters, Adam7
 *            interlacing, PLTE and tRNS, and multiple IDAT chunks.
 *            The zlib stream is inflated by img.c itself: stored,
 *            fixed-Huffman and dynamic-Huffman deflate blocks, with
 *            the CRC of every chunk it reads and the adler32 of the
 *            pixel stream verified. Nothing is vendored.
 *    JPEG    baseline and progressive Huffman (SOF0, SOF1, SOF2),
 *            8-bit samples, 1 and 3 components, chroma sampling
 *            4:4:4, 4:2:2, 4:2:0 and the other legal ratios, restart
 *            intervals (DRI/RSTn), all four progressive scan kinds
 *            (DC first/refine, AC first/refine), and YCbCr -> RGB
 *            (or RGB when the file says so via APP14 or component
 *            ids). Arithmetic-coded, lossless, hierarchical and
 *            4-component (CMYK/YCCK) JPEGs are refused by name
 *            rather than decoded wrongly - see the exact strings in
 *            img.c.
 *    BMP     24- and 32-bit uncompressed BI_RGB, bottom-up and
 *            top-down, 4-byte row padding. 8-bit palette BMPs too.
 *    PNM/PBM P1..P6, ASCII and binary, maxval 1..65535.
 *
 *  WHAT IT IS NOT
 *
 *    No GIF, no WebP, no animated anything, no colour management,
 *    no EXIF orientation (the pixels are returned exactly as the
 *    file stores them), no streaming: img_decode() wants the whole
 *    file in memory and owns nothing of it afterwards.
 *
 *  ERROR SEMANTICS
 *
 *    img_decode() returns 0 and fills *out, or returns -1, leaves
 *    *out zeroed (so img_free() is always safe), and writes a
 *    sentence into err naming the format, the chunk or scan, and
 *    the byte offset where that makes sense. err is NUL-terminated
 *    whenever esz > 0. Truncated input at any byte offset yields
 *    -1, never a read past the buffer: every read is bounds-checked
 *    against len, and every dimension is checked against the caps
 *    below before any allocation.
 *
 *  SCALING
 *
 *    img_scale_fit() shrinks with a box filter (every destination
 *    pixel is the exact average of the source rectangle it covers),
 *    keeps the aspect ratio, never enlarges, and returns a copy when
 *    the source already fits. Alpha is averaged as a plain channel,
 *    so a mostly-transparent source keeps the colour of its opaque
 *    pixels rather than fading to black.
 *
 *  LIMITS
 *
 *    A decoder is a denial-of-service surface: a 100 KB PNG can
 *    claim 4 gigapixels. So width and height are capped at
 *    IMG_MAX_DIM and the pixel count at IMG_MAX_PIXELS, both checked
 *    before a single byte is allocated, and the error says which
 *    limit was hit and what the file claimed. The 8.4-megapixel cap
 *    (a 4K frame fits) keeps the worst case near 110 MB: a 16-bit
 *    RGBA PNG needs 8 bytes of samples plus 4 bytes of output per
 *    pixel, and the heaviest JPEG is three 4:4:4 components, i.e.
 *    6 bytes of coefficients plus 1 byte of plane per component,
 *    then 4 bytes of output.
 *
 *  THREADS AND LINKING
 *
 *    There is no global state: the CRC table, the deflate tables and
 *    the IDCT cosine table are built on the stack per call, so two
 *    threads may decode two images at once. No libm, no libz, no
 *    libjpeg; only the C standard library's malloc/free/memcpy and
 *    snprintf. Every internal symbol is static and starts with
 *    "ic_" so that this file can also be #included into a single
 *    translation unit (the browser does that) without colliding
 *    with the host's own helpers.
 * ============================================================ */
#ifndef MYSTIC_IMG_H
#define MYSTIC_IMG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Limits (checked before allocation)
 * ------------------------------------------------------------ */
#define IMG_MAX_DIM      20000
#define IMG_MAX_PIXELS   (8u * 1024u * 1024u)

/* ------------------------------------------------------------
 *  An image in memory.
 *
 *  px is w*h pixels, row major, no padding beyond stride. stride is
 *  in pixels and is w for every decoder here; it exists so a caller
 *  that copies the image to a wider canvas can record what it did.
 *  A zeroed Img ({0}) is a valid empty image and img_free() accepts
 *  it.
 * ------------------------------------------------------------ */
typedef struct {
    int       w, h;
    int       stride;              /* pixels per row; == w today */
    uint32_t *px;                  /* 0xAARRGGBB, row major        */
} Img;

/* ------------------------------------------------------------
 *  Format sniffing, for callers that have a name or a MIME type but
 *  would rather trust the bytes (a .png that is really a JPEG).
 * ------------------------------------------------------------ */
enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_PNG,
    IMG_FMT_JPEG,
    IMG_FMT_BMP,
    IMG_FMT_PNM
};

/* The format the first bytes belong to, or IMG_FMT_UNKNOWN. */
int img_sniff(const unsigned char *buf, size_t len);

/* "PNG", "JPEG", "BMP", "PNM" or "unknown". Never NULL. */
const char *img_format_name(int fmt);

/* ------------------------------------------------------------
 *  img_decode - the whole API a caller usually needs.
 *
 *  buf/len   the complete file
 *  out       filled on success; zeroed on failure
 *  err/esz   receives a specific reason on failure (may be NULL/0)
 *
 *  Returns 0 on success, -1 on any malformed, unsupported or
 *  out-of-memory input.
 * ------------------------------------------------------------ */
int img_decode(const unsigned char *buf, size_t len, Img *out,
               char *err, size_t esz);

/* ------------------------------------------------------------
 *  img_scale_fit - shrink to fit a box, box filter, aspect kept.
 *
 *  If src->w <= max_w and src->h <= max_h the result is an exact
 *  copy. Otherwise the result is the largest size with the source's
 *  aspect ratio that fits inside max_w x max_h, and each output
 *  pixel is the average of the source pixels it covers. out must
 *  not alias src.
 *
 *  Returns 0 on success, -1 (out zeroed) on bad arguments or OOM.
 * ------------------------------------------------------------ */
int img_scale_fit(const Img *src, int max_w, int max_h, Img *out);

/* Free an image and zero it. Safe on NULL and on an already-freed
 * image. */
void img_free(Img *im);

#ifdef __cplusplus
}
#endif

#endif /* MYSTIC_IMG_H */
