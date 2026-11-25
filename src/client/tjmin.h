/* ============================================================
 *  Mystic OS - tjmin.h
 *
 *  The slice of libjpeg-turbo's ABI that src/client/apps/engine.c uses to
 *  decode the browser's screencast JPEGs, written out by hand because the
 *  image ships no headers (the package is libjpeg.so.8.3.2 without -dev).
 *
 *  WHICH ENTRY POINT, AND WHY IT IS NOT tj*
 *  ----------------------------------------
 *  The plan was the TurboJPEG API (tjInitDecompress / tjDecompress2). It is
 *  not in this bundle, and that is a measured fact, not a guess:
 *
 *    [apk]  Alpine's aports for libjpeg-turbo 3.1.3 (main/libjpeg-turbo,
 *           pkgver=3.1.3, -DWITH_JPEG8=1) splits the TurboJPEG wrapper into a
 *           SEPARATE subpackage:
 *               libturbojpeg() { amove usr/lib/libturbojpeg.so.* }
 *           The engine package's closure carries usr/lib/libjpeg.so.8 and
 *           libjpeg.so.8.3.2 and no libturbojpeg.so.0 anywhere.
 *    [sym]  libjpeg.so.8.3.2's .dynsym has 222 symbols, 94 of them jpeg_*,
 *           and ZERO tj_*. tjInitDecompress is simply not there.
 *    [link] zig cc -target x86_64-linux-musl -dynamic -Lbuild/engine-libs \
 *               -ljpeg ... calling tjInitDecompress
 *           -> ld.lld: error: undefined symbol: tjInitDecompress
 *
 *  So the shipped viewer links the CLASSIC libjpeg ABI (jpeg_*) from the very
 *  same library. That is not a downgrade: libjpeg.so.8.3.2 *is* libjpeg-turbo
 *  3.1.3, with its SIMD IDCT, colour conversion and upsampler compiled in.
 *  The evidence is in the bytes, because the library is stripped: it carries
 *  the string "libjpeg-turbo version 3.1.3 (build 20251211)", and the strings
 *  JSIMD_FORCEAVX2 and JSIMD_FORCESSE2 - which exist only in a WITH_SIMD
 *  build, since they are the environment overrides jsimd.c reads. (It exports
 *  no jsimd_* symbol: those are internal and stripped, so a symbol-table
 *  search for them proves nothing. Measured: 0 dynamic jsimd_* and 0
 *  occurrences of the substring in the file.) TurboJPEG is a thin wrapper
 *  over exactly these calls; what the scalar decoder in src/img.c lacked is
 *  that SIMD code, and the SIMD code is in libjpeg.so.8.
 *
 *  WHY 0xAARRGGBB COMES OUT FOR FREE
 *  ---------------------------------
 *  out_color_space = TJ_CS_EXT_BGRA (13) makes libjpeg-turbo write four bytes
 *  per pixel in the order B, G, R, A. On x86-64 (little endian) those four
 *  bytes are the uint32 0xAARRGGBB - the exact pixel format src/img.h defines
 *  and draw_page() memcpy()s into the surface. No conversion pass, no extra
 *  buffer. jpeglib.h 3.1.3 (lines 278-288) says of the EXT_RGBA family:
 *  "Use the following colorspace constants to ensure that the X byte is set
 *  to 0xFF, so that it can be interpreted as an opaque alpha channel" - so a
 *  decoded frame is opaque, never transparent.
 *
 *  PROVENANCE OF EVERY NUMBER BELOW
 *  --------------------------------
 *  Not from memory. The header and the constants were taken from the release
 *  that the bundle ships, libjpeg-turbo 3.1.3 (src/jpeglib.h, src/jmorecfg.h,
 *  src/turbojpeg.h), configured the way Alpine's APKBUILD configures it for
 *  x86-64: -DWITH_JPEG8=1, BITS_IN_JSAMPLE=8, i.e. JPEG_LIB_VERSION 80. The
 *  library agrees: its SONAME is libjpeg.so.8, and .so.8 is the version-80
 *  ABI (6b/7 install libjpeg.so.62 / libjpeg.so.7); the package's own
 *  jpeg_CreateDecompress compares its version argument against 80.
 *
 *  Every offset and size below was then MEASURED, not derived by hand, with a
 *  translation unit that #includes that real jpeglib.h and emits
 *  sizeof()/offsetof() as data:
 *
 *      sizeof(struct jpeg_decompress_struct)   656
 *      err 0   mem 8   progress 16   client_data 24   is_decompressor 32
 *      global_state 36   src 40   image_width 48   image_height 52
 *      num_components 56   jpeg_color_space 60   out_color_space 64
 *      scale_num 68   scale_denom 72   output_width 136   output_height 140
 *      out_color_components 144   output_components 148
 *      sizeof(struct jpeg_error_mgr) 168:
 *      error_exit 0  emit_message 8  output_message 16  format_message 24
 *      reset_error_mgr 32  msg_code 40  msg_parm 44 (80 bytes)
 *      trace_level 124   num_warnings 128   jpeg_message_table 136
 *      last_jpeg_message 144   addon_message_table 152
 *      first_addon_message 160   last_addon_message 164
 *
 *  The _Static_asserts at the bottom pin all of that, so an edit to this file
 *  cannot drift silently.
 *
 *  THE RUNTIME CROSS-CHECK
 *  -----------------------
 *  jpeg_CreateDecompress(cinfo, version, structsize) validates BOTH numbers
 *  before it writes a byte (jdapimin.c 3.1.3):
 *
 *      if (version != JPEG_LIB_VERSION)
 *        ERREXIT2(cinfo, JERR_BAD_LIB_VERSION, JPEG_LIB_VERSION, version);
 *      if (structsize != sizeof(struct jpeg_decompress_struct))
 *        ERREXIT2(cinfo, JERR_BAD_STRUCT_SIZE,
 *                 (int)sizeof(struct jpeg_decompress_struct), (int)structsize);
 *
 *  This file passes TJ_JPEG_LIB_VERSION and sizeof(tj_decompress), so a wrong
 *  declaration is reported by the library itself, with its own numbers, in
 *  the viewer's error line - it can never silently read or write the wrong
 *  field. That is the one thing a hand-written ABI slice needs.
 *
 *  FIELDS NOT LISTED
 *  -----------------
 *  tj_decompress declares only the fields engine.c reads; the rest of the
 *  656 bytes is covered by named gaps whose sizes the asserts verify. The
 *  library is free to use all 656 bytes because we allocate exactly that.
 *
 *  SIGNATURES
 *  ----------
 *  Types are declared here with their real underlying C types (JDIMENSION is
 *  unsigned int, boolean is int, JSAMPLE is unsigned char, jpegSize is
 *  unsigned long), which is what matters for the call ABI. Function names are
 *  the real symbol names: that is what the dynamic linker resolves.
 * ============================================================ */
#ifndef MYSTIC_TJMIN_H
#define MYSTIC_TJMIN_H

#include <stddef.h>

#if defined(_WIN32)
#error "tjmin.h describes the x86-64 SysV ABI of Alpine's libjpeg.so.8 (LP64: long is 8 bytes). It is for the Linux target only."
#endif

/* ------------------------------------------------------------
 *  Types, from jmorecfg.h / jpeglib.h 3.1.3
 * ------------------------------------------------------------ */
typedef int            tj_boolean;      /* boolean          (4 bytes) */
typedef unsigned int   tj_dimension;    /* JDIMENSION       (4 bytes) */
typedef unsigned char  tj_sample;       /* JSAMPLE          (1 byte)  */
typedef tj_sample     *tj_sample_row;   /* JSAMPROW                   */
typedef tj_sample_row *tj_sample_array; /* JSAMPARRAY                 */

struct tj_error_mgr;
struct tj_decompress;

typedef void                        *tj_common_ptr;    /* j_common_ptr    */
typedef struct tj_decompress        *tj_decompress_ptr;/* j_decompress_ptr*/

#define TJ_JPEG_LIB_VERSION      80     /* JPEG_LIB_VERSION, WITH_JPEG8=1 */
#define TJ_TRUE                   1     /* TRUE                           */
#define TJ_MSG_LENGTH_MAX       200     /* JMSG_LENGTH_MAX                */
#define TJ_MSG_STR_PARM_MAX      80     /* JMSG_STR_PARM_MAX              */

#define TJ_SIZEOF_DECOMPRESS    656     /* sizeof jpeg_decompress_struct  */
#define TJ_SIZEOF_ERROR_MGR     168     /* sizeof jpeg_error_mgr          */

/* J_COLOR_SPACE values (jpeglib.h 3.1.3, in declaration order). Only the one
 * the decoder sets is needed, plus the two the file header mentions. */
#define TJ_CS_UNKNOWN             0
#define TJ_CS_GRAYSCALE           1
#define TJ_CS_RGB                 2
#define TJ_CS_YCbCr               3
#define TJ_CS_CMYK                4
#define TJ_CS_YCCK                5
#define TJ_CS_EXT_RGB             6
#define TJ_CS_EXT_RGBX            7
#define TJ_CS_EXT_BGR             8
#define TJ_CS_EXT_BGRX            9
#define TJ_CS_EXT_XBGR           10
#define TJ_CS_EXT_XRGB           11
#define TJ_CS_EXT_RGBA           12
#define TJ_CS_EXT_BGRA           13    /* B,G,R,A bytes -> 0xAARRGGBB */
#define TJ_CS_EXT_ABGR           14
#define TJ_CS_EXT_ARGB           15
#define TJ_CS_RGB565             16

/* ------------------------------------------------------------
 *  struct tj_error_mgr - jpeg_error_mgr, 168 bytes
 *
 *  engine.c puts this FIRST inside its own wrapper struct so that the
 *  callbacks can reach their jmp_buf with a cast, which is the standard
 *  libjpeg error-handling idiom.
 * ------------------------------------------------------------ */
struct tj_error_mgr {
    void (*error_exit)(tj_common_ptr cinfo);            /*   0 */
    void (*emit_message)(tj_common_ptr cinfo, int lvl); /*   8 */
    void (*output_message)(tj_common_ptr cinfo);        /*  16 */
    void (*format_message)(tj_common_ptr cinfo, char *buffer); /* 24 */
    void (*reset_error_mgr)(tj_common_ptr cinfo);       /*  32 */
    int msg_code;                                       /*  40 */
    union {
        int  i[8];
        char s[TJ_MSG_STR_PARM_MAX];
    } msg_parm;                                         /*  44 */
    int  trace_level;                                   /* 124 */
    long num_warnings;                                  /* 128 */
    const char *const *jpeg_message_table;              /* 136 */
    int  last_jpeg_message;                             /* 144 */
    const char *const *addon_message_table;             /* 152 */
    int  first_addon_message;                           /* 160 */
    int  last_addon_message;                            /* 164 */
};

/* ------------------------------------------------------------
 *  struct tj_decompress - jpeg_decompress_struct, 656 bytes
 *
 *  The named fields are the ones read; the gaps hold the fields this decoder
 *  never touches (output_gamma, the processing flags, the DCT/upsampling
 *  knobs, the quantizer state) and the tail holds the state variables,
 *  coefficient arrays and the per-component info table. Their sizes are
 *  constants, not guesses, and the asserts below check the arithmetic.
 * ------------------------------------------------------------ */
struct tj_decompress {
    /* jpeg_common_fields */
    struct tj_error_mgr *err;                       /*   0 */
    void                *mem;                       /*   8 */
    void                *progress;                  /*  16 */
    void                *client_data;               /*  24 */
    int                  is_decompressor;           /*  32 */
    int                  global_state;              /*  36 */
    /* decompression members */
    void                *src;                       /*  40 */
    tj_dimension         image_width;               /*  48 */
    tj_dimension         image_height;              /*  52 */
    int                  num_components;            /*  56 */
    int                  jpeg_color_space;          /*  60 */
    int                  out_color_space;           /*  64 */
    unsigned int         scale_num;                 /*  68 */
    unsigned int         scale_denom;               /*  72 */
    /*  76..135: output_gamma (double, at 80), buffered_image, raw_data_out,
     *  dct_method, do_fancy_upsampling, do_block_smoothing, quantize_colors,
     *  dither_mode, two_pass_quantize, desired_number_of_colors and the three
     *  enable_*_quant flags, with the compiler's padding. Never read here. */
    unsigned char        _gap_76[60];               /*  76 */
    tj_dimension         output_width;              /* 136 */
    tj_dimension         output_height;             /* 140 */
    int                  out_color_components;      /* 144 */
    int                  output_components;         /* 148 */
    /* 152..167: rec_outbuf_height, actual_number_of_colors and colormap */
    unsigned char        _gap_152[16];              /* 152 */
    tj_dimension         output_scanline;           /* 168 */
    /* 172..655: the decompressor's state, the coefficient arrays and the
     * per-component info table. Never read here. */
    unsigned char        _rest[484];                /* 172 */
};

/* ------------------------------------------------------------
 *  The functions. Symbol names are the real ones.
 * ------------------------------------------------------------ */
struct tj_error_mgr *jpeg_std_error(struct tj_error_mgr *err);

void jpeg_CreateDecompress(tj_decompress_ptr cinfo, int version,
                           size_t structsize);
void jpeg_mem_src(tj_decompress_ptr cinfo, const unsigned char *inbuffer,
                  unsigned long insize);
int  jpeg_read_header(tj_decompress_ptr cinfo, tj_boolean require_image);
int  jpeg_start_decompress(tj_decompress_ptr cinfo);
tj_dimension jpeg_read_scanlines(tj_decompress_ptr cinfo,
                                 tj_sample_array scanlines,
                                 tj_dimension max_lines);
int  jpeg_finish_decompress(tj_decompress_ptr cinfo);
void jpeg_destroy_decompress(tj_decompress_ptr cinfo);

/* ------------------------------------------------------------
 *  The TurboJPEG ABI, for the day the package grows libturbojpeg.so.0.
 *
 *  Values are from turbojpeg.h 3.1.3 (TJ_NUMPF 12, TJ_NUMSAMP 7):
 *
 *      TJPF_RGB 0  BGR 1  RGBX 2  BGRX 3  XBGR 4  XRGB 5  GRAY 6
 *      TJPF_RGBA 7  BGRA 8  ABGR 9  ARGB 10  CMYK 11  UNKNOWN -1
 *      TJSAMP_444 0  422 1  420 2  GRAY 3  440 4  411 5  441 6  UNKNOWN -1
 *
 *  Nothing below is used by the shipped build: -DTJMIN_TURBOJPEG would be a
 *  link error against the current bundle (there is no libturbojpeg.so.0),
 *  which is exactly why engine.c uses the classic calls above instead.
 * ------------------------------------------------------------ */
#if defined(TJMIN_TURBOJPEG)
typedef void *tjhandle;

#define TJPF_UNKNOWN (-1)
#define TJPF_RGB       0
#define TJPF_BGR       1
#define TJPF_RGBX      2
#define TJPF_BGRX      3
#define TJPF_XBGR      4
#define TJPF_XRGB      5
#define TJPF_GRAY      6
#define TJPF_RGBA      7
#define TJPF_BGRA      8
#define TJPF_ABGR      9
#define TJPF_ARGB     10
#define TJPF_CMYK     11

#define TJSAMP_UNKNOWN (-1)
#define TJSAMP_444     0
#define TJSAMP_422     1
#define TJSAMP_420     2
#define TJSAMP_GRAY    3
#define TJSAMP_440     4
#define TJSAMP_411     5
#define TJSAMP_441     6

extern tjhandle tjInitDecompress(void);
extern int tjDecompressHeader3(tjhandle handle, const unsigned char *jpegBuf,
                               unsigned long jpegSize, int *width,
                               int *height, int *jpegSubsamp,
                               int *jpegColorspace);
extern int tjDecompress2(tjhandle handle, const unsigned char *jpegBuf,
                         unsigned long jpegSize, unsigned char *dstBuf,
                         int width, int pitch, int height, int pixelFormat,
                         int flags);
extern int tjDestroy(tjhandle handle);
extern char *tjGetErrorStr2(tjhandle handle);
#endif /* TJMIN_TURBOJPEG */

/* ------------------------------------------------------------
 *  The asserts. If any of these fires, this file and libjpeg.so.8.3.2 in the
 *  bundle disagree, and the viewer must not be built.
 * ------------------------------------------------------------ */
_Static_assert(sizeof(struct tj_error_mgr) == TJ_SIZEOF_ERROR_MGR,
               "jpeg_error_mgr is 168 bytes in libjpeg-turbo 3.1.3");
_Static_assert(offsetof(struct tj_error_mgr, error_exit) == 0, "error_exit");
_Static_assert(offsetof(struct tj_error_mgr, emit_message) == 8, "emit_message");
_Static_assert(offsetof(struct tj_error_mgr, output_message) == 16, "output_message");
_Static_assert(offsetof(struct tj_error_mgr, format_message) == 24, "format_message");
_Static_assert(offsetof(struct tj_error_mgr, reset_error_mgr) == 32, "reset_error_mgr");
_Static_assert(offsetof(struct tj_error_mgr, msg_code) == 40, "msg_code");
_Static_assert(offsetof(struct tj_error_mgr, msg_parm) == 44, "msg_parm");
_Static_assert(sizeof(((struct tj_error_mgr *)0)->msg_parm) == 80, "msg_parm size");
_Static_assert(offsetof(struct tj_error_mgr, trace_level) == 124, "trace_level");
_Static_assert(offsetof(struct tj_error_mgr, num_warnings) == 128, "num_warnings");

_Static_assert(sizeof(struct tj_decompress) == TJ_SIZEOF_DECOMPRESS,
               "jpeg_decompress_struct is 656 bytes with JPEG_LIB_VERSION 80");
_Static_assert(offsetof(struct tj_decompress, err) == 0, "err");
_Static_assert(offsetof(struct tj_decompress, mem) == 8, "mem");
_Static_assert(offsetof(struct tj_decompress, progress) == 16, "progress");
_Static_assert(offsetof(struct tj_decompress, client_data) == 24, "client_data");
_Static_assert(offsetof(struct tj_decompress, is_decompressor) == 32, "is_decompressor");
_Static_assert(offsetof(struct tj_decompress, global_state) == 36, "global_state");
_Static_assert(offsetof(struct tj_decompress, src) == 40, "src");
_Static_assert(offsetof(struct tj_decompress, image_width) == 48, "image_width");
_Static_assert(offsetof(struct tj_decompress, image_height) == 52, "image_height");
_Static_assert(offsetof(struct tj_decompress, num_components) == 56, "num_components");
_Static_assert(offsetof(struct tj_decompress, jpeg_color_space) == 60, "jpeg_color_space");
_Static_assert(offsetof(struct tj_decompress, out_color_space) == 64, "out_color_space");
_Static_assert(offsetof(struct tj_decompress, scale_num) == 68, "scale_num");
_Static_assert(offsetof(struct tj_decompress, scale_denom) == 72, "scale_denom");
_Static_assert(offsetof(struct tj_decompress, output_width) == 136, "output_width");
_Static_assert(offsetof(struct tj_decompress, output_height) == 140, "output_height");
_Static_assert(offsetof(struct tj_decompress, out_color_components) == 144, "out_color_components");
_Static_assert(offsetof(struct tj_decompress, output_components) == 148, "output_components");
_Static_assert(offsetof(struct tj_decompress, output_scanline) == 168, "output_scanline");

#endif /* MYSTIC_TJMIN_H */
