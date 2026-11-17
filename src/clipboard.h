/* ============================================================
 *  Mystic OS - clipboard.h
 *
 *  A real clipboard: typed entries (text, a list of file paths,
 *  and an image), a bounded history that de-duplicates and can
 *  pin entries, and a store on disk so the history survives a
 *  reboot.
 *
 *  There are two ways to use it:
 *
 *    1. Link src/clipboard.c into a program and call the
 *       clip_*() functions below. They talk to the clipboard
 *       daemon (clipd) when it is running and fall back to the
 *       store file when it is not.
 *
 *    2. Speak the line protocol on /run/mystic/clipboard.sock
 *       directly. That is what a sandboxed app should do since
 *       it needs no linking, only the socket. The protocol is
 *       documented at the bottom of this file.
 *
 *  The daemon is src/clipboard.c compiled with -DCLIPD_MAIN.
 *
 *  Limits are deliberately hard: a desktop that lets one app
 *  copy a 200 MB file into the clipboard has a memory problem,
 *  not a clipboard.
 * ============================================================ */
#ifndef MYSTIC_CLIPBOARD_H
#define MYSTIC_CLIPBOARD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Limits and paths
 * ------------------------------------------------------------ */
#define CLIP_MAX_ENTRIES        32              /* history length            */
#define CLIP_MAX_TEXT           (256u * 1024u)  /* bytes of UTF-8 per entry  */
#define CLIP_MAX_FILES          64              /* paths per file-list entry */
#define CLIP_MAX_PATH           512             /* bytes per file path       */
#define CLIP_MAX_IMAGE_PIXELS   (2u * 1024u * 1024u)  /* 1280x1600 or 1920x1080 is under this */
#define CLIP_MAX_IMAGE_BYTES    (8u * 1024u * 1024u)  /* w*h*4, the RGBA size     */
#define CLIP_MAX_PAYLOAD        (16u * 1024u * 1024u) /* all entries together     */
#define CLIP_MAX_PREVIEW        200             /* bytes in a HIST preview   */
#define CLIP_PROTOCOL           1

/* These are compile-time defaults. A test harness may override the two
 * path macros on the compiler command line; clipd and the library must
 * be built with the same values or they will not find each other. */
#ifndef CLIP_SOCKET_PATH
#define CLIP_SOCKET_PATH        "/run/mystic/clipboard.sock"
#endif
#ifndef CLIP_STORE_DIR
#define CLIP_STORE_DIR          "/root/clipboard"
#endif
#define CLIP_STORE_FILE         CLIP_STORE_DIR "/history.bin"
#define CLIP_LOCK_FILE          CLIP_STORE_DIR "/clipd.lock"

/* ------------------------------------------------------------
 *  Types
 * ------------------------------------------------------------ */
typedef enum {
    CLIP_TYPE_NONE  = 0,     /* only used as a query: "any type" */
    CLIP_TYPE_TEXT  = 1,
    CLIP_TYPE_FILES = 2,
    CLIP_TYPE_IMAGE = 3
} ClipType;

/* A stored image is either raw pixels the producer handed us, or a path
 * to a PNG on disk. A PNG entry keeps its path, not its decoded pixels:
 * the clipboard verifies the PNG signature and reads the real width and
 * height from the IHDR chunk, so those numbers are not invented, but it
 * does not decode the image (there is a PNG decoder in the image viewer,
 * not here). */
enum {
    CLIP_IMAGE_NONE = 0,
    CLIP_IMAGE_RGBA = 1,
    CLIP_IMAGE_PNG  = 2
};

typedef struct {
    int      kind;                    /* CLIP_IMAGE_RGBA or CLIP_IMAGE_PNG */
    uint32_t width, height;
    uint32_t nbytes;                  /* RGBA only: == width*height*4      */
    uint8_t *rgba;                    /* RGBA only, malloc'ed, or NULL     */
    char     png_path[CLIP_MAX_PATH]; /* PNG only                          */
} ClipImage;

typedef struct {
    ClipType type;
    uint64_t id;        /* strictly increasing; newer entries have bigger ids */
    uint64_t stamp;     /* unix time when the entry was copied              */
    int      pinned;    /* 1 = protected from eviction                      */

    /* CLIP_TYPE_TEXT */
    char    *text;      /* malloc'ed, NUL-terminated; never NULL for text   */
    uint32_t text_len;

    /* CLIP_TYPE_FILES */
    char   (*files)[CLIP_MAX_PATH];  /* malloc'ed array of nfiles paths     */
    uint32_t nfiles;

    /* CLIP_TYPE_IMAGE */
    ClipImage image;
} ClipEntry;

/* Return codes. Every public function returns one of these, or a
 * positive count where the comment says so. */
enum {
    CLIP_OK          =  0,
    CLIP_ERR_INVAL   = -1,   /* empty/invalid argument                     */
    CLIP_ERR_TOOBIG  = -2,   /* over one of the CLIP_MAX_* limits          */
    CLIP_ERR_FULL    = -3,   /* all 32 entries are pinned                  */
    CLIP_ERR_EMPTY   = -4,   /* no entry of the requested type             */
    CLIP_ERR_IO      = -5,   /* store or socket I/O failed                 */
    CLIP_ERR_BUSY    = -6,   /* daemon owns the store and did not answer   */
    CLIP_ERR_PROTO   = -7,   /* malformed store file or daemon reply       */
    CLIP_ERR_RANGE   = -8    /* history index out of range                 */
};

/* ------------------------------------------------------------
 *  Writing to the clipboard
 *
 *  Each call copies the value into history position 0 (newest). If an
 *  identical entry is already in the history it is removed first, so the
 *  same text copied twice appears once, at the front. A pinned duplicate
 *  stays pinned.
 *
 *  If the history is full, the oldest UNPINNED entry is dropped. Pinned
 *  entries therefore survive; if all 32 entries are pinned the call
 *  fails with CLIP_ERR_FULL rather than silently throwing away something
 *  the user asked to keep.
 *
 *  clip_set_text takes NUL-terminated UTF-8, 1 .. CLIP_MAX_TEXT bytes.
 *
 *  clip_set_filelist takes count paths, 1 .. CLIP_MAX_FILES, each
 *  1 .. CLIP_MAX_PATH-1 bytes. Order is significant: the same paths in a
 *  different order are a different entry.
 *
 *  clip_set_image takes tightly packed or strided RGBA8 pixels; stride is
 *  in bytes and 0 means width*4. width*height must be <= CLIP_MAX_IMAGE_PIXELS.
 *
 *  clip_set_image_png takes a path to a real PNG file. The file is opened
 *  and its signature and IHDR are checked now; the path is what is stored.
 * ------------------------------------------------------------ */
int clip_set_text(const char *text);
int clip_set_filelist(const char *const *paths, int count);
int clip_set_image(const void *rgba, int width, int height, int stride);
int clip_set_image_png(const char *png_path);

/* ------------------------------------------------------------
 *  Reading
 *
 *  clip_get_current returns a newly allocated copy of the newest entry
 *  of the requested type (CLIP_TYPE_NONE means "the newest entry of any
 *  type"), or NULL if there is none. Free it with clip_entry_free().
 *
 *  clip_history fills an allocated array of up to `want` entries, newest
 *  first (index 0 is the front, which is the index clip_pin takes). A
 *  want of 0 or less means CLIP_MAX_ENTRIES. Returns the number of
 *  entries written, or a negative error. Free the array with
 *  clip_history_free().
 * ------------------------------------------------------------ */
ClipEntry *clip_get_current(ClipType type);
int        clip_history(ClipEntry **out, int want);

/* Toggle the pin on history entry `index` (0 = newest). Returns the new
 * state: 1 pinned, 0 unpinned, or a negative CLIP_ERR_*. */
int clip_pin(int index);

/* Remove every entry, pinned ones included. Returns the number removed,
 * or a negative CLIP_ERR_*. */
int clip_clear(void);

void clip_entry_free(ClipEntry *entry);
void clip_history_free(ClipEntry *entries, int count);

/* ------------------------------------------------------------
 *  Housekeeping
 * ------------------------------------------------------------ */

/* 1 if clipd answered on the socket, 0 if it is not running. In the
 * second case the clip_* calls above still work through the store file,
 * without the daemon. */
int clip_running(void);

/* The canonical store path, for a settings screen or a backup button. */
const char *clip_store_path(void);

/* Human-readable text for a CLIP_ERR_* value. */
const char *clip_strerror(int rc);

/* The result of the most recent call in this process, for the functions
 * that return a pointer or a count where NULL/0 also means "nothing".
 * Not thread-safe; the clipboard API is used from the desktop's single
 * thread (or from one app process). */
int clip_last_error(void);

/* Run the daemon in this process. clipd's main() is provided when
 * clipboard.c is compiled with -DCLIPD_MAIN. It serves until SIGTERM or
 * SIGINT and returns 0. */
int clipd_main(int argc, char **argv);

/* ============================================================
 *  STORE FORMAT - /root/clipboard/history.bin
 *
 *  All integers are little-endian. The file is replaced atomically
 *  (write a temp file, fsync, rename), so a reader always sees a
 *  complete snapshot even while clipd is running.
 *
 *  Header, 24 bytes:
 *      off  size  field
 *        0     8  magic "MYSTCLIP"
 *        8     4  u32 format version, currently 1
 *       12     4  u32 entry count, 0 .. CLIP_MAX_ENTRIES
 *       16     8  u64 next id (ids are never reused, even after a clear)
 *
 *  Then `count` records, newest first. Each record is a 40-byte header
 *  followed by `len` payload bytes:
 *      off  size  field
 *        0     4  u32 type: 1 text, 2 file list, 3 RGBA, 4 PNG path
 *        4     4  u32 flags: bit 0 = pinned, all other bits must be 0
 *        8     8  u64 id
 *       16     8  u64 stamp (unix seconds)
 *       24     4  u32 width      (images; 0 for every other type)
 *       28     4  u32 height     (images; 0 for every other type)
 *       32     4  u32 count      (file lists: number of paths; else 0)
 *       36     4  u32 len        (payload bytes)
 *       40   len  payload
 *
 *  Payload by type:
 *      text   len UTF-8 bytes, may contain newlines, no NUL bytes
 *      files  `count` NUL-terminated path strings back to back; `len`
 *             is the total including every NUL, count is <= 64, each
 *             path is 1 .. 511 bytes
 *      rgba   width*height*4 bytes of RGBA8, len == width*height*4
 *      png    `len` bytes of filesystem path, 1 .. 511 bytes, no NUL
 *
 *  A file that does not match this description, contains a duplicate
 *  entry, has ids that are not strictly decreasing, or has trailing
 *  bytes is corrupt: clipd renames it to history.bin.corrupt-<time> and
 *  starts with an empty history instead of guessing.
 * ============================================================ */

/* ============================================================
 *  SOCKET PROTOCOL - /run/mystic/clipboard.sock
 *
 *  A stream of '\n'-terminated request lines. SET requests are followed
 *  by an exact number of payload bytes (no terminator). Responses are a
 *  status line, an optional exact number of payload bytes, and one
 *  trailing '\n' after a payload. Any ERR response is followed by the
 *  server closing the connection.
 *
 *      CLIENT <name> <pid>\n   -> OK CLIPD <protocol> <pid> <entries>\n
 *      PING\n                  -> OK PONG <entries>\n
 *
 *      SET TEXT <len>\n <len bytes>
 *                              -> OK SET <id>\n
 *      SET FILES <count> <len>\n <len bytes: count NUL-terminated paths>
 *                              -> OK SET <id>\n
 *      SET IMAGE RGBA <w> <h> <len>\n <len bytes>
 *                              -> OK SET <id>\n
 *      SET IMAGE PNG <len>\n <len bytes: path>
 *                              -> OK SET <id>\n
 *
 *      GET ANY|TEXT|FILES|IMAGE\n
 *          -> OK GET <TYPE> <imgkind> <id> <pinned> <stamp> <w> <h> <count> <len>\n
 *             <len payload bytes>\n
 *      HIST <n> [preview|full]\n
 *          -> OK HIST <count>\n
 *             count records; preview (the default) is:
 *                 ENTRY <idx> <TYPE> <imgkind> <id> <pin> <stamp> <plen>\n
 *                 <plen preview bytes>\n
 *             and full is:
 *                 ENTRY <idx> <TYPE> <imgkind> <id> <pin> <stamp> <w> <h> <cnt> <len>\n
 *                 <len payload bytes>\n
 *      PIN <index>\n           -> OK PIN <index> <0|1>\n
 *      CLEAR\n                 -> OK CLEAR <removed>\n
 *      QUIT\n                  -> BYE\n
 *
 *  Errors: ERR <code> <word>\n where <code> is a CLIP_ERR_* value.
 *  Preview bytes contain no newline or NUL: control characters are
 *  replaced by spaces so a client can print a record without parsing it.
 * ============================================================ */

#ifdef __cplusplus
}
#endif

#endif /* MYSTIC_CLIPBOARD_H */
