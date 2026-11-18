/* ============================================================
 *  Mystic OS - filetypes.c
 *
 *  The registry itself: one const table, and the pure lookups over
 *  it. Read filetypes.h first - it is the contract this file keeps.
 *
 *  HOW A ROW IS CHOSEN
 *
 *      filetype_for("holiday.PNG")   -> png
 *      filetype_for("/root/a.tar.gz")-> tar.gz   (not gz: longest wins)
 *      filetype_for(".bashrc")       -> unknown  (dotfile, no extension)
 *      filetype_for("README")        -> unknown  (no dot at all)
 *
 *  The scan asks every row one question: is the tail of the basename
 *  this extension, with a dot immediately before it? It keeps the
 *  longest tail that answered yes. Nothing is allocated, nothing is
 *  sorted at run time, and the only static objects are the table and
 *  the unknown row, both const - the linker puts them in .rodata, so
 *  "no globals mutated after init" is true by construction, not by
 *  discipline.
 *
 *  WHY LINEAR
 *
 *  Seventy-five rows, long names capped at seven characters, called
 *  once per entry at directory-scan time rather than per pixel. A
 *  binary search would need the table pre-sorted and a separate key
 *  type for bsearch; the scan is both smaller and obviously correct,
 *  and the host harness measures it. If this ever shows up in a
 *  profile, the switch is a second, sorted index - not a rewrite of
 *  the rules.
 *
 *  ADDING A TYPE
 *
 *  One line, in the group it belongs to. Keep ext lowercase and
 *  dotless, keep label under about 22 characters (the Files list is
 *  narrow), pick the kind and the mark, and name the app that really
 *  opens it. The harness checks the invariants: no duplicate
 *  extension, every label and app non-empty, every lookup
 *  case-insensitive, the dotfile and no-dot cases unknown, and the
 *  compound rows beating their tails.
 * ============================================================ */
#include "filetypes.h"

#include <string.h>

/* ------------------------------------------------------------
 *  The table.
 *
 *  Groups follow the brief's order so a reader can see at a glance
 *  that every requested family is present. The three compound rows
 *  sit at the end of the archives: "tar.gz" is what a tarball is
 *  actually called, and without the row a browser would draw it with
 *  the plain gzip mark and offer the wrong label.
 *
 *  The tail of the table is the OS's own knowledge: .theme is a
 *  palette that Settings opens (not a generic config the viewer
 *  should print), .exe is a real file a mounted share can bring that
 *  this OS cannot execute at all - it leaves the honest fallback
 *  (Files) to the desktop - and .run is an installer script the
 *  shell can actually run.
 * ------------------------------------------------------------ */
static const FileType FT_TABLE[] = {
    /* ---- image ------------------------------------------------------- */
    { "png",      "PNG image",          FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "jpg",      "JPEG image",         FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "jpeg",     "JPEG image",         FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "gif",      "GIF image",          FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "bmp",      "BMP image",          FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "webp",     "WebP image",         FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "svg",      "SVG drawing",        FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "ico",      "ICO icon",           FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "tif",      "TIFF image",         FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "tiff",     "TIFF image",         FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },
    { "ppm",      "PPM image",          FT_KIND_IMAGE,      FT_ICON_IMAGE,      FT_APP_VIEWER   },

    /* ---- video ------------------------------------------------------- */
    { "mp4",      "MP4 video",          FT_KIND_VIDEO,      FT_ICON_VIDEO,      FT_APP_VLC      },
    { "mkv",      "Matroska video",     FT_KIND_VIDEO,      FT_ICON_VIDEO,      FT_APP_VLC      },
    { "webm",     "WebM video",         FT_KIND_VIDEO,      FT_ICON_VIDEO,      FT_APP_VLC      },
    { "avi",      "AVI video",          FT_KIND_VIDEO,      FT_ICON_VIDEO,      FT_APP_VLC      },
    { "mov",      "QuickTime video",    FT_KIND_VIDEO,      FT_ICON_VIDEO,      FT_APP_VLC      },

    /* ---- audio ------------------------------------------------------- */
    { "mp3",      "MP3 audio",          FT_KIND_AUDIO,      FT_ICON_AUDIO,      FT_APP_VIEWER   },
    { "wav",      "WAV audio",          FT_KIND_AUDIO,      FT_ICON_AUDIO,      FT_APP_VIEWER   },
    { "ogg",      "Ogg audio",          FT_KIND_AUDIO,      FT_ICON_AUDIO,      FT_APP_VIEWER   },
    { "flac",     "FLAC audio",         FT_KIND_AUDIO,      FT_ICON_AUDIO,      FT_APP_VIEWER   },
    { "m4a",      "M4A audio",          FT_KIND_AUDIO,      FT_ICON_AUDIO,      FT_APP_VIEWER   },

    /* ---- text -------------------------------------------------------- */
    { "txt",      "Plain text",         FT_KIND_TEXT,       FT_ICON_TEXT,       FT_APP_VIEWER   },
    { "log",      "Log file",           FT_KIND_TEXT,       FT_ICON_TEXT,       FT_APP_VIEWER   },
    { "md",       "Markdown",           FT_KIND_TEXT,       FT_ICON_TEXT,       FT_APP_VIEWER   },
    { "rst",      "reStructuredText",   FT_KIND_TEXT,       FT_ICON_TEXT,       FT_APP_VIEWER   },
    { "csv",      "CSV table",          FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "tsv",      "TSV table",          FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "json",     "JSON data",          FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "xml",      "XML data",           FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "yaml",     "YAML data",          FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "yml",      "YAML data",          FT_KIND_TEXT,       FT_ICON_DATA,       FT_APP_VIEWER   },
    { "ini",      "INI settings",       FT_KIND_TEXT,       FT_ICON_CONFIG,     FT_APP_VIEWER   },
    { "conf",     "Config file",        FT_KIND_TEXT,       FT_ICON_CONFIG,     FT_APP_VIEWER   },
    { "cfg",      "Config file",        FT_KIND_TEXT,       FT_ICON_CONFIG,     FT_APP_VIEWER   },

    /* ---- document ---------------------------------------------------- */
    { "pdf",      "PDF document",       FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },
    { "doc",      "Word document",      FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },
    { "docx",     "Word document",      FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },
    { "odt",      "OpenDocument text",  FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },
    { "rtf",      "Rich text",          FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },
    { "epub",     "EPUB book",          FT_KIND_DOCUMENT,   FT_ICON_DOCUMENT,   FT_APP_VIEWER   },

    /* ---- archive ----------------------------------------------------- */
    { "zip",      "ZIP archive",        FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "tar",      "TAR archive",        FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "gz",       "Gzip archive",       FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "tgz",      "Tar gzip archive",   FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "bz2",      "Bzip2 archive",      FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "xz",       "XZ archive",         FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "7z",       "7-Zip archive",      FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "rar",      "RAR archive",        FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    /* compound: these must win over the gz/bz2/xz row above */
    { "tar.gz",   "Tar gzip archive",   FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "tar.bz2",  "Tar bzip2 archive",  FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },
    { "tar.xz",   "Tar xz archive",     FT_KIND_ARCHIVE,    FT_ICON_ARCHIVE,    FT_APP_FILES    },

    /* ---- code -------------------------------------------------------- */
    { "c",        "C source",           FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "h",        "C header",           FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "cpp",      "C++ source",         FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "hpp",      "C++ header",         FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "py",       "Python script",      FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "js",       "JavaScript",         FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "ts",       "TypeScript",         FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "html",     "HTML page",          FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_WEB      },
    { "htm",      "HTML page",          FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_WEB      },
    { "css",      "CSS stylesheet",     FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "sh",       "Shell script",       FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "rb",       "Ruby script",        FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "go",       "Go source",          FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "rs",       "Rust source",        FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "java",     "Java source",        FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },
    { "lua",      "Lua script",         FT_KIND_CODE,       FT_ICON_CODE,       FT_APP_VIEWER   },

    /* ---- data -------------------------------------------------------- */
    { "db",       "Database",           FT_KIND_DATA,       FT_ICON_DATA,       FT_APP_FILES    },
    { "sqlite",   "SQLite database",    FT_KIND_DATA,       FT_ICON_DATA,       FT_APP_FILES    },
    { "bin",      "Binary data",        FT_KIND_DATA,       FT_ICON_DATA,       FT_APP_FILES    },
    { "img",      "Disk image",         FT_KIND_DATA,       FT_ICON_DATA,       FT_APP_FILES    },
    { "iso",      "ISO image",          FT_KIND_DATA,       FT_ICON_DATA,       FT_APP_FILES    },

    /* ---- executable -------------------------------------------------- */
    { "exe",      "Windows program",    FT_KIND_EXECUTABLE, FT_ICON_EXECUTABLE, FT_APP_FILES    },
    { "run",      "Installer script",   FT_KIND_EXECUTABLE, FT_ICON_EXECUTABLE, FT_APP_TERMINAL },

    /* ---- Mystic's own ------------------------------------------------ */
    { "theme",    "Mystic theme",       FT_KIND_TEXT,       FT_ICON_THEME,      FT_APP_SETTINGS }
};

#define FT_TABLE_N ((int)(sizeof(FT_TABLE) / sizeof(FT_TABLE[0])))

/* The one row for everything that is not in the table. Returned by
 * pointer, never by value: callers can hold it, compare against it
 * and ask the four questions of it. Its app is the empty string, so
 * "nothing claims this" survives a strcmp and a snprintf alike. */
static const FileType FT_UNKNOWN = {
    "", "unknown", FT_KIND_UNKNOWN, FT_ICON_UNKNOWN, FT_APP_NONE
};

/* Kind and icon names, indexed by the enum values. Both arrays are
 * const; filetype_kind_name()/filetype_icon_name() never index them
 * out of range. */
static const char *const FT_KIND_NAME[FT_KIND_COUNT] = {
    "unknown", "image", "video", "audio", "text",
    "document", "archive", "code", "data", "executable"
};
static const char *const FT_ICON_NAME[FT_ICON_COUNT] = {
    "unknown", "image", "video", "audio", "text", "document",
    "archive", "code", "data", "executable", "config", "theme"
};

/* ------------------------------------------------------------
 *  ASCII folding. tolower() would drag in the locale, and the
 *  registry must answer the same way in every one of them.
 * ------------------------------------------------------------ */
static unsigned char ft_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Does s[0..n) equal ext[0..n), ignoring ASCII case? Both are already
 * known to be at least n bytes. */
static int ft_suffix_is(const char *s, const char *ext, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (ft_lower((unsigned char)s[i]) != ft_lower((unsigned char)ext[i]))
            return 0;
    return 1;
}

const FileType *filetype_for(const char *name) {
    if (!name) return &FT_UNKNOWN;

    /* Only the last component can carry the extension: a directory
     * called "backup.old" must not type the file inside it. */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t blen = strlen(base);

    /* One character cannot hold a dot and something after it. */
    if (blen < 2) return &FT_UNKNOWN;

    const FileType *best = &FT_UNKNOWN;
    size_t best_len = 0;

    for (int i = 0; i < FT_TABLE_N; i++) {
        const FileType *t = &FT_TABLE[i];
        size_t el = strlen(t->ext);
        /* el + 1 < blen keeps a character for the dot AND one for the
         * stem, so ".gz" and ".c" are dotfiles rather than typed
         * files. el <= best_len means a longer tail already won, which
         * is what makes "tar.gz" beat "gz" regardless of table order. */
        if (el == 0 || el + 1 >= blen || el <= best_len) continue;

        const char *suf = base + (blen - el);
        if (suf[-1] != '.') continue;
        if (!ft_suffix_is(suf, t->ext, el)) continue;

        best = t;
        best_len = el;
    }
    return best;
}

const FileType *filetype_by_ext(const char *ext) {
    if (!ext) return &FT_UNKNOWN;
    if (*ext == '.') ext++;            /* ".png" and "png" are one answer */
    size_t len = strlen(ext);
    if (len == 0) return &FT_UNKNOWN;

    for (int i = 0; i < FT_TABLE_N; i++) {
        const FileType *t = &FT_TABLE[i];
        if (strlen(t->ext) != len) continue;
        if (ft_suffix_is(ext, t->ext, len)) return t;
    }
    return &FT_UNKNOWN;
}

const FileType *filetype_unknown(void) {
    return &FT_UNKNOWN;
}

FileKind filetype_kind(const FileType *t) {
    return t ? t->kind : FT_KIND_UNKNOWN;
}

const char *filetype_label(const FileType *t) {
    return (t && t->label) ? t->label : FT_UNKNOWN.label;
}

FileIcon filetype_icon(const FileType *t) {
    return t ? t->icon : FT_ICON_UNKNOWN;
}

const char *filetype_app(const FileType *t) {
    return (t && t->app) ? t->app : FT_APP_NONE;
}

const char *filetype_kind_name(FileKind kind) {
    unsigned k = (unsigned)kind;       /* the enum is signed on some ABIs */
    if (k >= (unsigned)FT_KIND_COUNT) return FT_KIND_NAME[FT_KIND_UNKNOWN];
    return FT_KIND_NAME[k];
}

const char *filetype_icon_name(FileIcon icon) {
    unsigned i = (unsigned)icon;
    if (i >= (unsigned)FT_ICON_COUNT) return FT_ICON_NAME[FT_ICON_UNKNOWN];
    return FT_ICON_NAME[i];
}

int filetype_count(void) {
    return FT_TABLE_N;
}

const FileType *filetype_at(int index) {
    if (index < 0 || index >= FT_TABLE_N) return NULL;
    return &FT_TABLE[index];
}
