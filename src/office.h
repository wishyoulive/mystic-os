/* ============================================================
 *  Mystic OS - office.h
 *  Read the text out of an Office document, with its structure.
 *
 *  This is deliberately not a layout engine and does not pretend to
 *  be Word. It opens the container, extracts the characters and the
 *  shape they were written in - headings, paragraphs, bold/italic
 *  runs, table rows and cells - and leaves the typography to the
 *  caller. Nothing is invented: every character returned here was
 *  stored in the file, and every table cell carries the row and
 *  column the file gave it.
 *
 *  Supported containers:
 *    .docx  Word 2007+      word/document.xml
 *    .xlsx  Excel 2007+     xl/sharedStrings.xml + xl/worksheets/sheetN.xml
 *    .pptx  PowerPoint 2007+ ppt/slides/slideN.xml, in numeric order
 *    .odt .ods .odp         content.xml (OpenDocument)
 *  Detected and reported, not parsed:
 *    .doc .xls .ppt         OLE2 binary formats
 *
 *  A ZIP reader (central directory, local headers, stored and raw
 *  deflate entries, ZIP64 sizes, CRC-32 verification) lives here too,
 *  because that is the whole container for every supported format.
 * ============================================================ */
#ifndef MYSTIC_OFFICE_H
#define MYSTIC_OFFICE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OFFICE_FMT_UNKNOWN = 0,
    OFFICE_FMT_DOCX,
    OFFICE_FMT_XLSX,
    OFFICE_FMT_PPTX,
    OFFICE_FMT_ODT,
    OFFICE_FMT_ODS,
    OFFICE_FMT_ODP,
    OFFICE_FMT_DOC,        /* legacy binary, detected only */
    OFFICE_FMT_XLS,
    OFFICE_FMT_PPT,
    OFFICE_FMT_LEGACY,     /* OLE2, but not recognisably Word/Excel/PPT */
    OFFICE_FMT_RTF,
    OFFICE_FMT_HTML
} OfficeFormat;

/* block kinds */
enum { OFFICE_BLOCK_PARA = 1, OFFICE_BLOCK_TABLE = 2 };

/* paragraph styles, in document order of importance */
enum {
    OFFICE_STYLE_BODY = 0,
    OFFICE_STYLE_TITLE,
    OFFICE_STYLE_H1,
    OFFICE_STYLE_H2,
    OFFICE_STYLE_H3,
    OFFICE_STYLE_BULLET,
    OFFICE_STYLE_QUOTE
};

/* A string inside doc->text. Offsets, not pointers, so the arena can
 * realloc without invalidating every block. */
typedef struct {
    uint32_t off;
    uint32_t len;
} OfficeRef;

/* One styled span of a paragraph. `off` is absolute into doc->text, the
 * same arena the paragraph text lives in, so a run can be drawn without
 * copying. Runs are in document order and do not overlap. */
typedef struct {
    uint32_t off, len;
    unsigned char bold, italic;
} OfficeRun;

typedef struct {
    int kind;              /* OFFICE_BLOCK_PARA or OFFICE_BLOCK_TABLE */
    int style;             /* para: OFFICE_STYLE_* */
    int bold, italic;      /* para: the whole paragraph is bold / italic */
    int level;             /* para: list nesting, 0 at the top */
    OfficeRef para;        /* para: text (len may be 0) */
    OfficeRun *runs;       /* para: styled spans, absolute offsets */
    int nruns;

    int rows, cols;        /* table */
    OfficeRef *cell;       /* table: rows*cols refs, row-major */
    unsigned char *cell_bold;
    int truncated;         /* table: rows/cols were clamped to the caps */
} OfficeBlock;

/* A sheet/workbook that the document mentions. xlsx and ods only. */
typedef struct {
    char name[96];
    int  drawn;            /* 1 = this is the sheet in the table block */
} OfficeSheet;

typedef struct {
    OfficeFormat fmt;
    char format_name[64];
    char file[256];        /* the path that was opened */
    char title[192];
    char author[192];
    char message[288];     /* honest note: what was not parsed, or why */

    OfficeSheet *sheets;
    int nsheets;
    int sheets_cap;        /* private growth bookkeeping */

    OfficeBlock *blocks;
    int nblocks;
    int blocks_cap;        /* private growth bookkeeping */

    char  *text;           /* arena holding every string the blocks refer to */
    size_t text_len, text_cap;

    char  *plain;          /* the whole document as plain text, '\n' lines */
    size_t plain_len;

    int legacy;            /* 1 = OLE2 binary detected, nothing extracted */
    int truncated;         /* 1 = a document cap was hit */
} OfficeDoc;

/* ---- the ZIP container ------------------------------------ */

#define OFFICE_ZIP_MAX_ENTRIES 192

typedef struct {
    char     name[160];
    uint32_t crc;
    uint32_t csize, usize;   /* after ZIP64 resolution */
    uint32_t data_off;       /* first compressed byte in the buffer */
    uint16_t method;         /* 0 = stored, 8 = deflate */
    uint8_t  encrypted;
} OfficeZipEnt;

typedef struct {
    const uint8_t *data;
    size_t len;
    OfficeZipEnt ent[OFFICE_ZIP_MAX_ENTRIES];
    int count;               /* entries actually kept (<= the cap) */
    int total;               /* entries the central directory declares */
    int truncated;
} OfficeZip;

/* Parses the central directory. Returns 0 on success. */
int  office_zip_open(OfficeZip *z, const uint8_t *data, size_t len,
                     char *err, size_t errsz);
/* -1 when the entry does not exist. Exact, case-sensitive match. */
int  office_zip_find(const OfficeZip *z, const char *name);
/* Inflates/copies entry i and verifies its CRC. *out is malloc'd; free it. */
int  office_zip_read(const OfficeZip *z, int i, uint8_t **out, size_t *outlen,
                     char *err, size_t errsz);

/* ---- the document ----------------------------------------- */

/* Cheap sniff of the first bytes plus the package contents. */
OfficeFormat office_detect(const uint8_t *data, size_t len);
const char  *office_format_name(OfficeFormat f);

int  office_open(OfficeDoc *doc, const char *path, char *err, size_t errsz);
/* Same, on bytes the caller already has (used by the tests and the shell). */
int  office_parse(OfficeDoc *doc, const char *path,
                  const uint8_t *data, size_t len, char *err, size_t errsz);
void office_free(OfficeDoc *doc);

/* NUL-terminated views into the arena; safe for the life of the doc. */
const char *office_block_text(const OfficeDoc *doc, const OfficeBlock *b);
const char *office_cell_text(const OfficeDoc *doc, const OfficeBlock *b,
                             int row, int col);

#endif /* MYSTIC_OFFICE_H */
