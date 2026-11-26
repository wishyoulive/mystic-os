/* ============================================================
 *  Mystic OS - filetypes.h
 *
 *  What a file is, answered once for the whole system.
 *
 *  Before this, every part of the desktop guessed. The Files app
 *  carried its own EXT_IMAGE / EXT_ARCHIVE / EXT_TEXT lists and a
 *  kind_of() that knew about nine marks; nothing else knew what a
 *  .webm was, and the idea of "which app opens this?" did not exist
 *  at all. Adding an extension meant editing an app, and two apps
 *  could disagree about the same name. So the answer lives in one
 *  const table instead, with one row per extension:
 *
 *      { "png", "PNG image", FT_KIND_IMAGE, FT_ICON_IMAGE, FT_APP_VIEWER },
 *
 *  and four questions you can ask about any name:
 *
 *      filetype_for("holiday.PNG")   the row above (or the unknown row)
 *      filetype_kind(t)              FT_KIND_IMAGE
 *      filetype_label(t)             "PNG image"
 *      filetype_icon(t)              FT_ICON_IMAGE   (a mark to draw)
 *      filetype_app(t)               "viewer"        (who opens it)
 *
 *  MATCHING (filetype_for)
 *
 *    - Only the last path component matters: "/root/a/b.PNG" is the
 *      PNG row. Callers may pass a bare name or a whole path.
 *    - The winner is the LONGEST table suffix that starts just after
 *      a dot, so "x.tar.gz" is the "tar.gz" row and not the "gz" row,
 *      and "x.tsv" is TSV rather than "sv" or "ts".
 *    - A name whose only dot is its first character has no extension:
 *      ".bashrc" is a dotfile, not a "bashrc" file, and ".gz" is not
 *      a gzip archive. A later dot IS a real separator, so
 *      ".config.json" is JSON and ".tar.gz" is a hidden gzip. This is
 *      the rule Python's os.path.splitext(), Rust's Path::extension()
 *      and git all use, so the answer here does not surprise a caller
 *      that has met extensions before. The compound row needs a real
 *      stem: "backup.tar.gz" is "tar.gz", while a name that starts
 *      with the dot has no stem before "tar" and falls to "gz".
 *    - Comparison is ASCII case-insensitive. The C locale is never
 *      consulted, so "PNG", "png" and "PnG" cannot disagree on a
 *      Turkish locale or anywhere else.
 *    - No name, an empty name, or a name with no usable dot returns
 *      the unknown row. The unknown row's label is literally
 *      "unknown": the registry says so instead of guessing a kind,
 *      an icon or an app.
 *
 *  WHAT THIS MODULE IS NOT
 *
 *  It reads no file contents - a PNG renamed to .txt is the TXT row,
 *  and that is deliberate: the name is what the desktop has before it
 *  opens anything, and it is what a file manager can answer instantly
 *  for three hundred entries without touching the disk.
 *
 *  NO INIT, NO MEMORY
 *
 *  The table is `static const`, built by the compiler. There is no
 *  init function, no allocation, no global that is written after
 *  startup: every call is a scan of rodata and returns a pointer into
 *  it. That is why this can be called from the compositor, an app or
 *  a host test with nothing to set up and nothing to tear down.
 *
 *  ICON IDS
 *
 *  FileIcon values are stable integers. The desktop maps an id to one
 *  of its drawn marks (the Files app already has image, archive, text,
 *  gear, theme and executable marks). Append new ids at the end and
 *  never renumber: a client app compiled against an older header must
 *  still get the mark it was built to expect, and an id it does not
 *  know is a generic page rather than a wrong picture.
 *
 *  APP IDS
 *
 *  An app id is the bundle directory name under /usr/share/mystic/apps
 *  (or /root/apps for an installed one), which is also the id the
 *  desktop opens. The id is the app that SHOULD open the file: the
 *  desktop still has to check that the bundle exists, because a bundle
 *  can be uninstalled. When it is missing, the honest fallback is the
 *  Files app, which can show the entry and its real size.
 * ============================================================ */
#ifndef MYSTIC_FILETYPES_H
#define MYSTIC_FILETYPES_H

/* ------------------------------------------------------------
 *  The coarse family, as asked for by the desktop: a text file and
 *  a log are the same kind even though their labels differ.
 *
 *  FT_KIND_COUNT is not a kind; it is how the table-owning .c knows
 *  how many names it must provide.
 * ------------------------------------------------------------ */
typedef enum {
    FT_KIND_UNKNOWN = 0,
    FT_KIND_IMAGE,
    FT_KIND_VIDEO,
    FT_KIND_AUDIO,
    FT_KIND_TEXT,
    FT_KIND_DOCUMENT,
    FT_KIND_ARCHIVE,
    FT_KIND_CODE,
    FT_KIND_DATA,
    FT_KIND_EXECUTABLE,
    FT_KIND_COUNT
} FileKind;

/* ------------------------------------------------------------
 *  A mark the desktop can draw. Distinct from the kind on purpose:
 *  .conf/.cfg/.ini are text files that want the gear, and .json is a
 *  text file that wants the data mark, so the picture can be more
 *  precise than the family without inventing a new family.
 *
 *  Append only. The numeric value of an existing id is an ABI.
 * ------------------------------------------------------------ */
typedef enum {
    FT_ICON_UNKNOWN = 0,   /* a page with a question, or a plain page */
    FT_ICON_IMAGE,         /* picture frame                          */
    FT_ICON_VIDEO,         /* film frame with a play triangle        */
    FT_ICON_AUDIO,         /* musical note                           */
    FT_ICON_TEXT,          /* page with lines of text                */
    FT_ICON_DOCUMENT,      /* page with a folded corner              */
    FT_ICON_ARCHIVE,       /* boxed bundle / zip                     */
    FT_ICON_CODE,          /* angle brackets < >                     */
    FT_ICON_DATA,          /* stacked discs / database               */
    FT_ICON_EXECUTABLE,    /* gear or chevron in a box               */
    FT_ICON_CONFIG,        /* gear (what Files already draws)        */
    FT_ICON_THEME,         /* a palette disc                         */
    FT_ICON_COUNT
} FileIcon;

/* ------------------------------------------------------------
 *  App ids, spelled once so no call site can typo them.
 *
 *  FT_APP_NONE is the empty string, not NULL: it can be handed to
 *  snprintf/strcmp without a null check, and it means "no app claims
 *  this", which is true of the unknown row.
 * ------------------------------------------------------------ */
#define FT_APP_VIEWER   "viewer"     /* the file viewer (external app) */
#define FT_APP_DOCVIEW  "docview"    /* Documents: reads Office, edits text */
#define FT_APP_FILES    "files"      /* the file manager               */
#define FT_APP_WEB      "web"        /* the browser                    */
#define FT_APP_TERMINAL "terminal"   /* the shell window               */
#define FT_APP_SETTINGS "settings"   /* the Settings app               */
#define FT_APP_VLC      "vlc"        /* the video player (VLC bundle)  */
#define FT_APP_NONE     ""

/* One row. `ext` is lowercase and carries no dot; a compound
 * extension is written with its dot, "tar.gz", which is what makes
 * longest-match meaningful. Every string is a string literal in
 * rodata, so the pointers are valid for the life of the process. */
typedef struct {
    const char *ext;      /* "png", "tar.gz"; "" on the unknown row   */
    const char *label;    /* "PNG image"; "unknown" on the unknown row */
    FileKind    kind;     /* coarse family                             */
    FileIcon    icon;     /* which mark to draw                        */
    const char *app;      /* FT_APP_*; "" when nothing claims it       */
} FileType;

/* ------------------------------------------------------------
 *  Lookup
 * ------------------------------------------------------------ */

/* The row for `name` (a basename or any path), or the unknown row.
 * Never NULL, so callers can dereference without a null check. */
const FileType *filetype_for(const char *name);

/* The row for an extension typed on its own - "png" or ".png",
 * "tar.gz" or ".tar.gz" - for a "New file" dialog. Unlike
 * filetype_for() this is not looking at a filename, so the dotfile
 * rule cannot apply and a leading dot is simply skipped. Exact match,
 * case-insensitive; the unknown row when it is not in the table. */
const FileType *filetype_by_ext(const char *ext);

/* The row returned for everything unrecognised: ext "", label
 * "unknown", kind FT_KIND_UNKNOWN, icon FT_ICON_UNKNOWN, app "". */
const FileType *filetype_unknown(void);

/* ------------------------------------------------------------
 *  The four questions. All of them accept NULL and answer for the
 *  unknown row, so an app that has a FileType * it is unsure about
 *  still draws something honest.
 * ------------------------------------------------------------ */
FileKind    filetype_kind(const FileType *t);
const char *filetype_label(const FileType *t);
FileIcon    filetype_icon(const FileType *t);
const char *filetype_app(const FileType *t);

/* "image" .. "executable", "unknown" for anything out of range. */
const char *filetype_kind_name(FileKind kind);

/* "image" .. "theme", "unknown" for anything out of range. Used by
 * the host test and by anything that has to put a mark's name in a
 * tooltip or a log line. */
const char *filetype_icon_name(FileIcon icon);

/* ------------------------------------------------------------
 *  Iteration, for the Files app's "New file" menu.
 *
 *  Every row is a real extension, in table order (images, video,
 *  audio, text, documents, archives, code, data, executable, then
 *  Mystic's own rows). filetype_at() returns NULL outside
 *  [0, filetype_count()), so a menu loop has one clear stop.
 *
 *  Compound rows ("tar.gz") are included: they are real types, and a
 *  caller that does not want two menu entries for the same thing can
 *  skip any row whose ext contains a dot.
 * ------------------------------------------------------------ */
int filetype_count(void);
const FileType *filetype_at(int index);

#endif /* MYSTIC_FILETYPES_H */
