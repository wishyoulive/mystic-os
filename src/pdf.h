/* ============================================================
 *  Mystic OS - pdf.h
 *  The PDF reader behind the PDF viewer app.
 *
 *  Everything here is implemented in pdf.c; see the long comment at
 *  the top of that file for exactly which parts of PDF 1.7 are
 *  handled and which are refused (with a reason) rather than faked.
 * ============================================================ */
#ifndef MYSTIC_PDF_H
#define MYSTIC_PDF_H

#include <stddef.h>

#include "mystic.h"

/* pdf.c is also included directly by the viewer app (so the bundle is a
 * single translation unit). In that build the app defines PDF_API as
 * "static __attribute__((unused))" before including pdf.c, and these
 * declarations follow it. */
#ifndef PDF_API
#define PDF_API
#endif

/* ------------------------------------------------------------
 *  A document. Opaque; pdf.c owns everything inside it.
 * ------------------------------------------------------------ */
typedef struct PdfDoc PdfDoc;

/* One page, in points (1/72 inch), with /Rotate already applied to
 * width/height - the same numbers a viewer should lay out. */
typedef struct {
    double width, height;
    double media_w, media_h;   /* the unrotated page box */
    int    rotate;             /* 0, 90, 180 or 270 */
} PdfPageInfo;

/* What the file contained. Honest counters, not decoration: the viewer
 * shows these so a page that used an object stream, a rebuilt xref or an
 * embedded font program is visibly accounted for. */
typedef struct {
    int pages;
    int xref_tables;
    int xref_streams;
    int object_streams;
    int reconstructed;      /* xref was rebuilt by scanning for "N 0 obj" */
    int embedded_fonts;     /* font programs present; /Widths used, program skipped */
    int type0_fonts;        /* CID fonts present; their text is not interpreted */
    int encrypted;
    int pages_truncated;
} PdfDocStats;

/* What one render did. Filled by pdf_render_page. */
typedef struct {
    int    glyphs;            /* glyphs handed to the rasteriser */
    int    glyphs_visible;    /* ... whose origin fell inside the page view */
    long   ops;               /* content operators executed */
    int    images_skipped;    /* Image XObjects not drawn */
    int    inline_images_skipped;
    int    forms_drawn;       /* Form XObjects interpreted */
    int    unsupported_ops;   /* operators we do not implement */
    int    unmapped_text_runs;/* text in a font we cannot map (not drawn) */
    int    rotated_text_runs; /* text matrices with rotation (placed, not turned) */
    int    dashes_ignored;
    int    text_clip_ignored;
    int    colour_spaces_ignored;
    char   first_problem[96]; /* the first unsupported operator, by name */
    long   first_problem_offset;
    double page_w, page_h;    /* the page rectangle in device pixels */
} PdfRenderInfo;

/* ---- opening -------------------------------------------------- */
/* Return NULL and fill err with the exact reason (and pdf_error_offset()
 * on the returned NULL is not available, so `err` carries the byte). */
PDF_API PdfDoc *pdf_open(const char *path, char *err, size_t errsz);
PDF_API PdfDoc *pdf_open_memory(const void *bytes, size_t len, char *err, size_t errsz);
PDF_API void    pdf_close(PdfDoc *d);

/* ---- the document --------------------------------------------- */
PDF_API int pdf_page_count(const PdfDoc *d);
PDF_API int pdf_page_info(const PdfDoc *d, int page, PdfPageInfo *out);
PDF_API const char *pdf_title(const PdfDoc *d);
PDF_API int pdf_version_major(const PdfDoc *d);
PDF_API int pdf_version_minor(const PdfDoc *d);
PDF_API int pdf_encrypted(const PdfDoc *d);
PDF_API void pdf_doc_stats(const PdfDoc *d, PdfDocStats *st);

/* ---- rendering ------------------------------------------------ */
/* Draw `page` into `s` with its top-left corner at (dx,dy), at `zoom`
 * device pixels per point. The page is clipped to the Surface's own
 * bounds AND to its cx0..cx1 clip rectangle, so a viewer can scroll a
 * page larger than the window by passing negative/positive offsets.
 * Returns 0 on success; on failure returns -1 and pdf_error() /
 * pdf_error_offset() say what failed and where in the file. */
PDF_API int pdf_render_page(PdfDoc *d, int page, Surface *s, int dx, int dy,
                    double zoom, PdfRenderInfo *info);

/* The text of a page, in reading order as the content stream shows it:
 * newlines are inserted at T*, ', " and at vertical Td/TD/Tm moves.
 * Bytes are UTF-8. Returns the number of bytes stored (NUL terminated);
 * truncated is set when the buffer was too small. */
PDF_API int pdf_page_text(PdfDoc *d, int page, char *out, size_t outsz, int *truncated);

/* ---- the last failure ----------------------------------------- */
PDF_API const char *pdf_error(const PdfDoc *d);        /* "" when there is none */
PDF_API long pdf_error_offset(const PdfDoc *d); /* file byte offset, -1 if unknown */

#endif /* MYSTIC_PDF_H */
