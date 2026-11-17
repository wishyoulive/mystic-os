/* ============================================================
 *  Mystic OS - clipboard.c
 *
 *  The clipboard service. Two halves in one file:
 *
 *    - the library the desktop and its apps link against: the
 *      clip_*() functions, which speak the daemon protocol and
 *      fall back to the store file when clipd is not running;
 *    - clipd itself, selected with -DCLIPD_MAIN, a small serial
 *      server on /run/mystic/clipboard.sock.
 *
 *  One writer at a time is the whole concurrency story, and it is
 *  enforced twice: clipd holds an exclusive flock on
 *  /root/clipboard/clipd.lock for its whole life, and a library
 *  that cannot reach the socket takes the same lock before it
 *  reads-modifies-writes the store. A process can therefore never
 *  interleave a write with the daemon. Readers need no lock: the
 *  store is only ever replaced by rename(), so a read sees a
 *  complete old or complete new file.
 *
 *  POSIX is the shipping target. The small _WIN32 branches in the
 *  transport and file helpers exist so this same file can be built
 *  as a host harness (zig cc without -target) and exercised on the
 *  development machine - the clipboard logic, the wire protocol and
 *  the store are the same code either way.
 * ============================================================ */
#include "clipboard.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <afunix.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <direct.h>
#  include <io.h>
#  include <process.h>
#  include <windows.h>

typedef SOCKET clip_sock_t;
typedef int    clip_socklen_t;
#  define CLIP_BAD_SOCKET      INVALID_SOCKET
#  define clip_close_socket    closesocket
#  define clip_close_file      _close
#  define clip_unlink          _unlink
#  define clip_getpid          _getpid
#  define clip_fileno          _fileno
#  define CLIP_MKDIR(p, m)     _mkdir(p)
#  define clip_rename_replace  clip_win_rename
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <sys/un.h>
#  include <unistd.h>

typedef int clip_sock_t;
typedef socklen_t clip_socklen_t;
#  define CLIP_BAD_SOCKET      (-1)
#  define clip_close_socket    close
#  define clip_close_file      close
#  define clip_unlink          unlink
#  define clip_getpid          getpid
#  define clip_fileno          fileno
#  define CLIP_MKDIR(p, m)     mkdir((p), (m))
#  define clip_rename_replace  rename
#endif

#ifndef O_CLOEXEC
#  define O_CLOEXEC 0
#endif

/* Internal only: "the daemon was not there, try the file". It is never
 * returned from a public function. */
#define CLIP_NODAEMON (-100)

/* Largest file the store reader will look at. A healthy store cannot
 * exceed the payload budget plus the fixed record headers. */
#define CLIP_STORE_MAX \
    (CLIP_MAX_PAYLOAD + (size_t)CLIP_MAX_ENTRIES * 96u + 4096u)

static int g_clip_error = CLIP_OK;

/* ------------------------------------------------------------
 *  Small utilities
 * ------------------------------------------------------------ */

static int clip_set_error(int rc) {
    if (rc == CLIP_NODAEMON) rc = CLIP_ERR_BUSY;
    g_clip_error = rc;
    return rc;
}

static void clip_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
#endif
}

static char *clip_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int clip_ieq(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int clip_parse_u64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10u) return -1;
        v = v * 10u + d;
    }
    *out = v;
    return 0;
}

static int clip_parse_i64(const char *s, int64_t *out) {
    int neg = 0;
    if (!s || !*s) return -1;
    if (*s == '-') { neg = 1; s++; }
    uint64_t v;
    if (clip_parse_u64(s, &v) != 0) return -1;
    if (v > (uint64_t)INT64_MAX) return -1;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 0;
}

/* Split a mutable line at spaces/tabs. Returns NULL at the end. */
static char *clip_next_tok(char **sp) {
    char *s = *sp;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *sp = s; return NULL; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = 0; s++; }
    *sp = s;
    return start;
}

static int clip_mkdir_one(const char *path) {
    if (CLIP_MKDIR(path, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* mkdir -p with a fixed buffer. The paths are compile-time constants or
 * come from the test harness, so 1024 bytes is plenty; a longer one is
 * rejected rather than truncated. */
static int clip_mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);

    char *start = tmp + 1;
#ifdef _WIN32
    if (len >= 3 && tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\'))
        start = tmp + 3;
#endif
    for (char *p = start; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (clip_mkdir_one(tmp) != 0) return -1;
        *p = '/';
    }
    return clip_mkdir_one(tmp);
}

static char *clip_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return clip_strdup(".");
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
#ifdef _WIN32
    if (n == 2 && path[1] == ':') n = 3;   /* "C:" -> "C:/" */
#endif
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = 0;
    return out;
}

/* The PNG path entry stores a real path. Read the signature and IHDR so
 * the width/height shown to the user are the file's own numbers. This is
 * not a decoder and does not pretend to be one. */
static int clip_png_measure(const char *path, uint32_t *w, uint32_t *h) {
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    unsigned char hdr[24];
    if (!path || !*path) return CLIP_ERR_INVAL;

    FILE *f = fopen(path, "rb");
    if (!f) return CLIP_ERR_IO;
    size_t n = fread(hdr, 1, sizeof hdr, f);
    int read_err = ferror(f);
    fclose(f);
    if (n != sizeof hdr) return read_err ? CLIP_ERR_IO : CLIP_ERR_INVAL;
    if (memcmp(hdr, sig, 8) != 0) return CLIP_ERR_INVAL;

    /* 4-byte big-endian chunk length, then the type "IHDR", then the
     * two 32-bit big-endian dimensions. The first chunk of a PNG must be
     * IHDR and it must be 13 bytes of data. */
    uint32_t clen = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                    ((uint32_t)hdr[10] << 8) | (uint32_t)hdr[11];
    if (clen != 13 || memcmp(hdr + 12, "IHDR", 4) != 0) return CLIP_ERR_INVAL;
    uint32_t pw = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) |
                  ((uint32_t)hdr[18] << 8) | (uint32_t)hdr[19];
    uint32_t ph = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                  ((uint32_t)hdr[22] << 8) | (uint32_t)hdr[23];
    if (pw == 0 || ph == 0) return CLIP_ERR_INVAL;
    *w = pw;
    *h = ph;
    return CLIP_OK;
}

/* ------------------------------------------------------------
 *  Entry
 * ------------------------------------------------------------ */

typedef struct {
    ClipType type;
    uint64_t id;
    uint64_t stamp;
    int      pinned;

    char    *text;                    /* text                         */
    uint32_t text_len;

    char    *files;                   /* NUL-separated paths back to back */
    uint32_t files_len;               /* bytes in `files`             */
    uint32_t nfiles;

    int      image_kind;
    uint32_t width, height;
    uint32_t nbytes;
    uint8_t *rgba;
    char     png_path[CLIP_MAX_PATH];
} Entry;

static void entry_free(Entry *e) {
    if (!e) return;
    free(e->text);
    free(e->files);
    free(e->rgba);
    free(e);
}

static size_t entry_bytes(const Entry *e) {
    switch (e->type) {
        case CLIP_TYPE_TEXT:  return e->text_len;
        case CLIP_TYPE_FILES: return e->files_len;
        case CLIP_TYPE_IMAGE: return e->image_kind == CLIP_IMAGE_RGBA
                                     ? e->nbytes : strlen(e->png_path);
        default:              return 0;
    }
}

static uint32_t entry_width(const Entry *e) {
    return e->type == CLIP_TYPE_IMAGE ? e->width : 0;
}
static uint32_t entry_height(const Entry *e) {
    return e->type == CLIP_TYPE_IMAGE ? e->height : 0;
}

/* Payload bytes as they go on the wire and into the store. */
static const unsigned char *entry_payload(const Entry *e, size_t *len) {
    switch (e->type) {
        case CLIP_TYPE_TEXT:
            *len = e->text_len;
            return (const unsigned char *)e->text;
        case CLIP_TYPE_FILES:
            *len = e->files_len;
            return (const unsigned char *)e->files;
        case CLIP_TYPE_IMAGE:
            if (e->image_kind == CLIP_IMAGE_RGBA) {
                *len = e->nbytes;
                return e->rgba;
            }
            *len = strlen(e->png_path);
            return (const unsigned char *)e->png_path;
        default:
            *len = 0;
            return NULL;
    }
}

static int entry_same(const Entry *a, const Entry *b) {
    if (a->type != b->type) return 0;
    switch (a->type) {
        case CLIP_TYPE_TEXT:
            return a->text_len == b->text_len &&
                   memcmp(a->text, b->text, a->text_len) == 0;
        case CLIP_TYPE_FILES:
            return a->nfiles == b->nfiles &&
                   a->files_len == b->files_len &&
                   memcmp(a->files, b->files, a->files_len) == 0;
        case CLIP_TYPE_IMAGE:
            if (a->image_kind != b->image_kind) return 0;
            if (a->image_kind == CLIP_IMAGE_PNG)
                return strcmp(a->png_path, b->png_path) == 0;
            return a->width == b->width && a->height == b->height &&
                   a->nbytes == b->nbytes &&
                   memcmp(a->rgba, b->rgba, a->nbytes) == 0;
        default:
            return 0;
    }
}

/* A file list is a count and a blob of NUL-terminated paths. This is the
 * one place that decides whether such a blob is well formed. */
static int files_blob_ok(const char *blob, uint32_t len, uint32_t count) {
    if (count == 0 || count > CLIP_MAX_FILES) return 0;
    if (len == 0 || len > (uint32_t)CLIP_MAX_FILES * CLIP_MAX_PATH) return 0;
    const char *p = blob;
    uint32_t left = len;
    for (uint32_t i = 0; i < count; i++) {
        const char *nul = memchr(p, 0, left);
        if (!nul) return 0;
        size_t plen = (size_t)(nul - p);
        if (plen == 0 || plen >= CLIP_MAX_PATH) return 0;
        p += plen + 1;
        left -= (uint32_t)(plen + 1);
    }
    return left == 0;
}

static Entry *entry_new(ClipType type) {
    Entry *e = calloc(1, sizeof *e);
    if (e) e->type = type;
    return e;
}

static Entry *entry_make_text(const char *text, uint32_t len) {
    if (!text || len == 0 || len > CLIP_MAX_TEXT) return NULL;
    if (memchr(text, 0, len) != NULL) return NULL;
    Entry *e = entry_new(CLIP_TYPE_TEXT);
    if (!e) return NULL;
    e->text = malloc((size_t)len + 1);
    if (!e->text) { free(e); return NULL; }
    memcpy(e->text, text, len);
    e->text[len] = 0;
    e->text_len = len;
    return e;
}

static Entry *entry_make_files(const char *blob, uint32_t len, uint32_t count) {
    if (!blob || !files_blob_ok(blob, len, count)) return NULL;
    Entry *e = entry_new(CLIP_TYPE_FILES);
    if (!e) return NULL;
    e->files = malloc(len);
    if (!e->files) { free(e); return NULL; }
    memcpy(e->files, blob, len);
    e->files_len = len;
    e->nfiles = count;
    return e;
}

static Entry *entry_make_rgba(const void *px, uint32_t w, uint32_t h) {
    if (!px || w == 0 || h == 0) return NULL;
    uint64_t pixels = (uint64_t)w * h;
    if (pixels > CLIP_MAX_IMAGE_PIXELS) return NULL;
    uint64_t bytes = pixels * 4u;
    if (bytes > CLIP_MAX_IMAGE_BYTES) return NULL;
    Entry *e = entry_new(CLIP_TYPE_IMAGE);
    if (!e) return NULL;
    e->image_kind = CLIP_IMAGE_RGBA;
    e->width = w;
    e->height = h;
    e->nbytes = (uint32_t)bytes;
    e->rgba = malloc(e->nbytes);
    if (!e->rgba) { free(e); return NULL; }
    memcpy(e->rgba, px, e->nbytes);
    return e;
}

static Entry *entry_make_png(const char *path, uint32_t w, uint32_t h) {
    if (!path) return NULL;
    size_t n = strlen(path);
    if (n == 0 || n >= CLIP_MAX_PATH || w == 0 || h == 0) return NULL;
    Entry *e = entry_new(CLIP_TYPE_IMAGE);
    if (!e) return NULL;
    e->image_kind = CLIP_IMAGE_PNG;
    e->width = w;
    e->height = h;
    memcpy(e->png_path, path, n + 1);
    return e;
}

/* Fill a caller-owned public entry. Returns 0, or -1 if an allocation
 * failed (the caller then runs clip_entry_free, which is safe on a
 * partially filled structure). */
static int entry_to_public(const Entry *src, ClipEntry *dst) {
    memset(dst, 0, sizeof *dst);
    dst->type   = src->type;
    dst->id     = src->id;
    dst->stamp  = src->stamp;
    dst->pinned = src->pinned;

    switch (src->type) {
        case CLIP_TYPE_TEXT:
            dst->text = malloc((size_t)src->text_len + 1);
            if (!dst->text) return -1;
            memcpy(dst->text, src->text, src->text_len);
            dst->text[src->text_len] = 0;
            dst->text_len = src->text_len;
            return 0;

        case CLIP_TYPE_FILES: {
            char (*arr)[CLIP_MAX_PATH] =
                calloc(src->nfiles ? src->nfiles : 1, CLIP_MAX_PATH);
            if (!arr) return -1;
            const char *p = src->files;
            uint32_t left = src->files_len;
            for (uint32_t i = 0; i < src->nfiles; i++) {
                const char *nul = memchr(p, 0, left);
                size_t plen = (size_t)(nul - p);
                memcpy(arr[i], p, plen);
                arr[i][plen] = 0;
                p += plen + 1;
                left -= (uint32_t)(plen + 1);
            }
            dst->files = arr;
            dst->nfiles = src->nfiles;
            return 0;
        }

        case CLIP_TYPE_IMAGE:
            dst->image.kind = src->image_kind;
            dst->image.width = src->width;
            dst->image.height = src->height;
            if (src->image_kind == CLIP_IMAGE_RGBA) {
                dst->image.rgba = malloc(src->nbytes ? src->nbytes : 1);
                if (!dst->image.rgba) return -1;
                memcpy(dst->image.rgba, src->rgba, src->nbytes);
                dst->image.nbytes = src->nbytes;
            } else {
                memcpy(dst->image.png_path, src->png_path,
                       strlen(src->png_path) + 1);
            }
            return 0;

        default:
            return -1;
    }
}

void clip_entry_free(ClipEntry *entry) {
    if (!entry) return;
    free(entry->text);
    free(entry->files);
    free(entry->image.rgba);
    free(entry);
}

void clip_history_free(ClipEntry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].text);
        free(entries[i].files);
        free(entries[i].image.rgba);
    }
    free(entries);
}

/* ------------------------------------------------------------
 *  History
 * ------------------------------------------------------------ */

typedef struct {
    Entry   *v[CLIP_MAX_ENTRIES];   /* newest first, no gaps */
    int      n;
    uint64_t next_id;
} Hist;

static void hist_init(Hist *h) {
    memset(h, 0, sizeof *h);
    h->next_id = 1;
}

static void hist_free(Hist *h) {
    for (int i = 0; i < h->n; i++) entry_free(h->v[i]);
    h->n = 0;
}

static size_t hist_bytes(const Hist *h) {
    size_t total = 0;
    for (int i = 0; i < h->n; i++) total += entry_bytes(h->v[i]);
    return total;
}

static void hist_remove(Hist *h, int idx) {
    entry_free(h->v[idx]);
    for (int i = idx; i + 1 < h->n; i++) h->v[i] = h->v[i + 1];
    h->n--;
}

static int hist_oldest_unpinned(const Hist *h) {
    for (int i = h->n - 1; i >= 0; i--)
        if (!h->v[i]->pinned) return i;
    return -1;
}

/* Takes ownership of `e` in every case: it is stored on success and
 * freed on failure. */
static int hist_insert(Hist *h, Entry *e) {
    int pinned = e->pinned;

    /* De-duplication: an identical entry is removed first and its pin
     * state is carried over to the new copy at the front. */
    for (int i = 0; i < h->n; i++) {
        if (entry_same(h->v[i], e)) {
            pinned = h->v[i]->pinned;
            hist_remove(h, i);
            break;
        }
    }
    e->pinned = pinned;

    if (h->n >= CLIP_MAX_ENTRIES) {
        int victim = hist_oldest_unpinned(h);
        if (victim < 0) { entry_free(e); return CLIP_ERR_FULL; }
        hist_remove(h, victim);
    }
    while (hist_bytes(h) + entry_bytes(e) > CLIP_MAX_PAYLOAD) {
        int victim = hist_oldest_unpinned(h);
        if (victim < 0) { entry_free(e); return CLIP_ERR_FULL; }
        hist_remove(h, victim);
    }

    e->id = h->next_id++;
    e->stamp = (uint64_t)time(NULL);
    memmove(&h->v[1], &h->v[0], (size_t)h->n * sizeof h->v[0]);
    h->v[0] = e;
    h->n++;
    return CLIP_OK;
}

static Entry *hist_find_newest(const Hist *h, ClipType type) {
    for (int i = 0; i < h->n; i++)
        if (type == CLIP_TYPE_NONE || h->v[i]->type == type) return h->v[i];
    return NULL;
}

static int hist_pin_toggle(Hist *h, int idx) {
    if (idx < 0 || idx >= h->n) return CLIP_ERR_RANGE;
    h->v[idx]->pinned = !h->v[idx]->pinned;
    return h->v[idx]->pinned;
}

static int hist_clear_all(Hist *h) {
    int removed = h->n;
    hist_free(h);
    return removed;
}

/* ------------------------------------------------------------
 *  Store file
 * ------------------------------------------------------------ */

static int clip_write_all(FILE *f, const void *p, size_t n) {
    if (n == 0) return 0;
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int clip_write_u32(FILE *f, uint32_t v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 0);
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    return clip_write_all(f, b, 4);
}

static int clip_write_u64(FILE *f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    return clip_write_all(f, b, 8);
}

static uint32_t clip_get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t clip_get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static FILE *clip_open_new_file(const char *path) {
#ifdef _WIN32
    return fopen(path, "wb");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
#endif
}

static int clip_fsync_fd(int fd) {
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

#ifdef _WIN32
static int clip_win_rename(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#endif

static void clip_fsync_dir(const char *path) {
#ifdef _WIN32
    (void)path;
#else
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
#endif
}

/* Whole file into memory. Returns 0 on success, 1 if the file does not
 * exist, or a negative CLIP_ERR_*. */
static int clip_read_whole(const char *path, unsigned char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 1 : CLIP_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return CLIP_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return CLIP_ERR_IO; }
    if ((unsigned long)sz > (unsigned long)CLIP_STORE_MAX) {
        fclose(f);
        return CLIP_ERR_PROTO;
    }
    rewind(f);
    size_t len = (size_t)sz;
    unsigned char *buf = malloc(len ? len : 1);
    if (!buf) { fclose(f); return CLIP_ERR_IO; }
    if (len && fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return CLIP_ERR_IO;
    }
    if (ferror(f)) { free(buf); fclose(f); return CLIP_ERR_IO; }
    fclose(f);
    *out = buf;
    *out_len = len;
    return 0;
}

static Entry *entry_from_record(uint32_t type, uint32_t flags,
                                uint64_t id, uint64_t stamp,
                                uint32_t w, uint32_t h, uint32_t count,
                                const unsigned char *payload, uint32_t len) {
    if (flags & ~1u) return NULL;
    if (w > 0xFFFFu || h > 0xFFFFu) return NULL;
    if (count > CLIP_MAX_FILES) return NULL;

    Entry *e = NULL;
    switch (type) {
        case 1: {
            if (w != 0 || h != 0 || count != 0) return NULL;
            if (len == 0 || len > CLIP_MAX_TEXT) return NULL;
            if (memchr(payload, 0, len) != NULL) return NULL;
            e = entry_new(CLIP_TYPE_TEXT);
            if (!e) return NULL;
            e->text = malloc((size_t)len + 1);
            if (!e->text) { free(e); return NULL; }
            memcpy(e->text, payload, len);
            e->text[len] = 0;
            e->text_len = len;
            break;
        }
        case 2: {
            if (w != 0 || h != 0) return NULL;
            if (!files_blob_ok((const char *)payload, len, count)) return NULL;
            e = entry_new(CLIP_TYPE_FILES);
            if (!e) return NULL;
            e->files = malloc(len);
            if (!e->files) { free(e); return NULL; }
            memcpy(e->files, payload, len);
            e->files_len = len;
            e->nfiles = count;
            break;
        }
        case 3: {
            if (count != 0 || w == 0 || h == 0) return NULL;
            uint64_t pixels = (uint64_t)w * h;
            if (pixels > CLIP_MAX_IMAGE_PIXELS) return NULL;
            uint64_t bytes = pixels * 4u;
            if (bytes > CLIP_MAX_IMAGE_BYTES || bytes != len) return NULL;
            e = entry_new(CLIP_TYPE_IMAGE);
            if (!e) return NULL;
            e->image_kind = CLIP_IMAGE_RGBA;
            e->width = w;
            e->height = h;
            e->nbytes = len;
            e->rgba = malloc(len);
            if (!e->rgba) { free(e); return NULL; }
            memcpy(e->rgba, payload, len);
            break;
        }
        case 4: {
            if (count != 0 || w == 0 || h == 0) return NULL;
            if (len == 0 || len >= CLIP_MAX_PATH) return NULL;
            if (memchr(payload, 0, len) != NULL) return NULL;
            e = entry_new(CLIP_TYPE_IMAGE);
            if (!e) return NULL;
            e->image_kind = CLIP_IMAGE_PNG;
            e->width = w;
            e->height = h;
            memcpy(e->png_path, payload, len);
            e->png_path[len] = 0;
            break;
        }
        default:
            return NULL;
    }
    e->id = id;
    e->stamp = stamp;
    e->pinned = (flags & 1u) ? 1 : 0;
    return e;
}

/* Load the store into a fresh Hist. Returns CLIP_OK (missing file is an
 * empty history), or a negative error after freeing anything it built. */
static int store_load(const char *path, Hist *h) {
    unsigned char *buf = NULL;
    size_t len = 0;
    int rc = clip_read_whole(path, &buf, &len);
    if (rc == 1) return CLIP_OK;                 /* no file yet */
    if (rc < 0) return rc;

    if (len < 24 || memcmp(buf, "MYSTCLIP", 8) != 0) goto corrupt;
    uint32_t version = clip_get_u32(buf + 8);
    uint32_t count = clip_get_u32(buf + 12);
    uint64_t next_id = clip_get_u64(buf + 16);
    if (version != 1 || count > CLIP_MAX_ENTRIES || next_id == 0) goto corrupt;

    size_t pos = 24;
    uint64_t prev_id = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (len - pos < 40) goto corrupt;
        uint32_t type = clip_get_u32(buf + pos + 0);
        uint32_t flags = clip_get_u32(buf + pos + 4);
        uint64_t id = clip_get_u64(buf + pos + 8);
        uint64_t stamp = clip_get_u64(buf + pos + 16);
        uint32_t w = clip_get_u32(buf + pos + 24);
        uint32_t hs = clip_get_u32(buf + pos + 28);
        uint32_t cnt = clip_get_u32(buf + pos + 32);
        uint32_t plen = clip_get_u32(buf + pos + 36);
        if (plen > len - pos - 40) goto corrupt;
        if (id >= prev_id) goto corrupt;          /* newest first, strict */
        prev_id = id;

        Entry *e = entry_from_record(type, flags, id, stamp, w, hs, cnt,
                                     buf + pos + 40, plen);
        if (!e) goto corrupt;
        for (int k = 0; k < h->n; k++) {
            if (entry_same(h->v[k], e)) { entry_free(e); goto corrupt; }
        }
        h->v[h->n++] = e;
        pos += 40 + plen;
    }
    if (pos != len) goto corrupt;

    uint64_t max_id = h->n ? h->v[0]->id : 0;
    if (next_id <= max_id) next_id = max_id + 1;
    h->next_id = next_id;
    free(buf);
    return CLIP_OK;

corrupt:
    free(buf);
    hist_free(h);
    return CLIP_ERR_PROTO;
}

static int store_save(const char *path, const Hist *h) {
    char tmp[sizeof(CLIP_STORE_FILE) + 64];
    char *dir = clip_dirname(path);
    if (!dir) return -1;
    if (clip_mkdir_p(dir) != 0) { free(dir); return -1; }
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)clip_getpid());

    FILE *f = clip_open_new_file(tmp);
    if (!f) { free(dir); return -1; }

    int ok = 1;
    if (clip_write_all(f, "MYSTCLIP", 8) != 0) ok = 0;
    if (ok && clip_write_u32(f, 1) != 0) ok = 0;
    if (ok && clip_write_u32(f, (uint32_t)h->n) != 0) ok = 0;
    if (ok && clip_write_u64(f, h->next_id) != 0) ok = 0;

    for (int i = 0; ok && i < h->n; i++) {
        const Entry *e = h->v[i];
        uint32_t type = 0;
        switch (e->type) {
            case CLIP_TYPE_TEXT:  type = 1; break;
            case CLIP_TYPE_FILES: type = 2; break;
            case CLIP_TYPE_IMAGE: type = e->image_kind == CLIP_IMAGE_RGBA ? 3 : 4; break;
            default: ok = 0; continue;
        }
        size_t plen = 0;
        const unsigned char *payload = entry_payload(e, &plen);
        if (plen > 0xFFFFFFFFu) { ok = 0; break; }
        if (clip_write_u32(f, type) != 0) ok = 0;
        if (ok && clip_write_u32(f, e->pinned ? 1u : 0u) != 0) ok = 0;
        if (ok && clip_write_u64(f, e->id) != 0) ok = 0;
        if (ok && clip_write_u64(f, e->stamp) != 0) ok = 0;
        if (ok && clip_write_u32(f, entry_width(e)) != 0) ok = 0;
        if (ok && clip_write_u32(f, entry_height(e)) != 0) ok = 0;
        if (ok && clip_write_u32(f, e->type == CLIP_TYPE_FILES ? e->nfiles : 0) != 0) ok = 0;
        if (ok && clip_write_u32(f, (uint32_t)plen) != 0) ok = 0;
        if (ok && clip_write_all(f, payload, plen) != 0) ok = 0;
    }

    if (ok && fflush(f) != 0) ok = 0;
    if (ok && clip_fsync_fd(clip_fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        clip_unlink(tmp);
        free(dir);
        return -1;
    }
    if (clip_rename_replace(tmp, path) != 0) {
        clip_unlink(tmp);
        free(dir);
        return -1;
    }
    clip_fsync_dir(dir);
    free(dir);
    return 0;
}

/* ------------------------------------------------------------
 *  Locking
 * ------------------------------------------------------------ */

static int clip_open_lock_file(void) {
#ifdef _WIN32
    return _open(CLIP_LOCK_FILE, _O_CREAT | _O_RDWR | _O_BINARY,
                 _S_IREAD | _S_IWRITE);
#else
    return open(CLIP_LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
#endif
}

static int clip_try_lock(int fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(&ov, 0, sizeof ov);
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0, 1, 0, &ov) ? 0 : -1;
#else
    int r;
    do { r = flock(fd, LOCK_EX | LOCK_NB); } while (r != 0 && errno == EINTR);
    return r == 0 ? 0 : -1;
#endif
}

static void clip_unlock(int fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    if (h == INVALID_HANDLE_VALUE) return;
    memset(&ov, 0, sizeof ov);
    UnlockFileEx(h, 0, 1, 0, &ov);
#else
    flock(fd, LOCK_UN);
#endif
}

/* A library write when clipd is not running: wait up to one second for
 * the lock, because the daemon may be starting or stopping. */
static int clip_fallback_lock(int *out_fd) {
    for (int i = 0; i < 100; i++) {
        int fd = clip_open_lock_file();
        if (fd < 0) return CLIP_ERR_IO;
        if (clip_try_lock(fd) == 0) { *out_fd = fd; return CLIP_OK; }
        clip_close_file(fd);
        clip_sleep_ms(10);
    }
    return CLIP_ERR_BUSY;
}

/* clipd's own lock: one instance only, but wait a moment for a fallback
 * writer that is finishing. */
static int clip_daemon_lock(int *out_fd) {
    int fd = clip_open_lock_file();
    if (fd < 0) return CLIP_ERR_IO;
    for (int i = 0; i < 100; i++) {
        if (clip_try_lock(fd) == 0) { *out_fd = fd; return CLIP_OK; }
        clip_sleep_ms(20);
    }
    clip_close_file(fd);
    return CLIP_ERR_BUSY;
}

/* ------------------------------------------------------------
 *  Sockets
 * ------------------------------------------------------------ */

static int clip_net_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
        done = 1;
    }
#endif
    return 0;
}

static void clip_sock_timeout(clip_sock_t fd, int seconds) {
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000u;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static int clip_send_all(clip_sock_t fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        size_t chunk = n > 0x40000000u ? 0x40000000u : n;
        long w = (long)send(fd, (const char *)p, (int)chunk, 0);
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int clip_recv_all(clip_sock_t fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n > 0) {
        size_t chunk = n > 0x40000000u ? 0x40000000u : n;
        long r = (long)recv(fd, (char *)p, (int)chunk, 0);
        if (r <= 0) return -1;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

/* Read one '\n'-terminated line, strip CRs, NUL-terminate. Returns the
 * length or -1 on EOF/error/overlong line. */
static int clip_recv_line(clip_sock_t fd, char *out, size_t cap) {
    size_t i = 0;
    for (;;) {
        char c;
        long r = (long)recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (i + 1 >= cap) return -1;
        out[i++] = c;
    }
    out[i] = 0;
    return (int)i;
}

static clip_sock_t clip_daemon_connect(void) {
    if (clip_net_init() != 0) return CLIP_BAD_SOCKET;
    clip_sock_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == CLIP_BAD_SOCKET) return CLIP_BAD_SOCKET;

    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(CLIP_SOCKET_PATH);
    if (n == 0 || n >= sizeof a.sun_path) {
        clip_close_socket(fd);
        return CLIP_BAD_SOCKET;
    }
    memcpy(a.sun_path, CLIP_SOCKET_PATH, n + 1);
    if (connect(fd, (struct sockaddr *)&a, (clip_socklen_t)sizeof a) != 0) {
        clip_close_socket(fd);
        return CLIP_BAD_SOCKET;
    }
    clip_sock_timeout(fd, 5);
    return fd;
}

/* ------------------------------------------------------------
 *  Client: protocol replies
 * ------------------------------------------------------------ */

static int clip_rpc_error(const char *line) {
    if (strncmp(line, "ERR ", 4) == 0) {
        char *sp = (char *)line + 4;
        char *tok = clip_next_tok(&sp);
        int64_t code;
        if (clip_parse_i64(tok, &code) == 0 && code < 0 && code > -1000)
            return (int)code;
    }
    return CLIP_ERR_PROTO;
}

static int clip_tok_u64(char **sp, uint64_t *v) {
    char *t = clip_next_tok(sp);
    return t ? clip_parse_u64(t, v) : -1;
}

static int clip_tok_type(char **sp, ClipType *t) {
    char *s = clip_next_tok(sp);
    if (!s) return -1;
    if (clip_ieq(s, "ANY"))   { *t = CLIP_TYPE_NONE;  return 0; }
    if (clip_ieq(s, "TEXT"))  { *t = CLIP_TYPE_TEXT;  return 0; }
    if (clip_ieq(s, "FILES")) { *t = CLIP_TYPE_FILES; return 0; }
    if (clip_ieq(s, "IMAGE")) { *t = CLIP_TYPE_IMAGE; return 0; }
    return -1;
}

static const char *clip_type_name(ClipType t) {
    switch (t) {
        case CLIP_TYPE_TEXT:  return "TEXT";
        case CLIP_TYPE_FILES: return "FILES";
        case CLIP_TYPE_IMAGE: return "IMAGE";
        default:              return "ANY";
    }
}

/* Read `len` payload bytes plus the one framing newline. */
static int clip_read_framed(clip_sock_t fd, size_t len, unsigned char **out) {
    unsigned char *buf = malloc(len + 1);
    if (!buf) return CLIP_ERR_IO;
    if (clip_recv_all(fd, buf, len) != 0) { free(buf); return CLIP_ERR_PROTO; }
    char end;
    if (clip_recv_all(fd, &end, 1) != 0 || end != '\n') {
        free(buf);
        return CLIP_ERR_PROTO;
    }
    buf[len] = 0;   /* one spare byte; text uses it, others ignore it */
    *out = buf;
    return CLIP_OK;
}

/* Shared parser for GET and a full HIST record.
 *   OK GET <TYPE> <kind> <id> <pin> <stamp> <w> <h> <count> <len>
 * The caller has already consumed the first token ("OK" or "ENTRY") and
 * passes `sp` pointing just after the type token. */
static int clip_parse_entry(clip_sock_t fd, ClipType type, char **sp, ClipEntry *e) {
    uint64_t kind64, id, pin, stamp, w, h, count, len;
    if (clip_tok_u64(sp, &kind64) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &id) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &pin) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &stamp) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &w) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &h) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &count) != 0) return CLIP_ERR_PROTO;
    if (clip_tok_u64(sp, &len) != 0) return CLIP_ERR_PROTO;
    if (id == 0 || pin > 1 || kind64 > CLIP_IMAGE_PNG || w > 0xFFFFu || h > 0xFFFFu)
        return CLIP_ERR_PROTO;

    memset(e, 0, sizeof *e);
    e->type = type;
    e->id = id;
    e->stamp = stamp;
    e->pinned = (int)pin;

    switch (type) {
        case CLIP_TYPE_TEXT: {
            if (kind64 != 0 || w != 0 || h != 0 || count != 0) return CLIP_ERR_PROTO;
            if (len == 0 || len > CLIP_MAX_TEXT) return CLIP_ERR_PROTO;
            unsigned char *buf = NULL;
            int rc = clip_read_framed(fd, (size_t)len, &buf);
            if (rc != CLIP_OK) return rc;
            if (memchr(buf, 0, (size_t)len) != NULL) { free(buf); return CLIP_ERR_PROTO; }
            e->text = (char *)buf;   /* clip_read_framed left room for the NUL */
            e->text_len = (uint32_t)len;
            return CLIP_OK;
        }
        case CLIP_TYPE_FILES: {
            if (kind64 != 0 || w != 0 || h != 0) return CLIP_ERR_PROTO;
            if (count == 0 || count > CLIP_MAX_FILES) return CLIP_ERR_PROTO;
            if (len == 0 || len > (uint64_t)CLIP_MAX_FILES * CLIP_MAX_PATH)
                return CLIP_ERR_PROTO;
            unsigned char *buf = NULL;
            int rc = clip_read_framed(fd, (size_t)len, &buf);
            if (rc != CLIP_OK) return rc;
            if (!files_blob_ok((const char *)buf, (uint32_t)len, (uint32_t)count)) {
                free(buf);
                return CLIP_ERR_PROTO;
            }
            char (*arr)[CLIP_MAX_PATH] = calloc((size_t)count, CLIP_MAX_PATH);
            if (!arr) { free(buf); return CLIP_ERR_IO; }
            const char *p = (const char *)buf;
            uint32_t left = (uint32_t)len;
            for (uint32_t i = 0; i < (uint32_t)count; i++) {
                const char *nul = memchr(p, 0, left);
                size_t plen = (size_t)(nul - p);
                memcpy(arr[i], p, plen);
                arr[i][plen] = 0;
                p += plen + 1;
                left -= (uint32_t)(plen + 1);
            }
            free(buf);
            e->files = arr;
            e->nfiles = (uint32_t)count;
            return CLIP_OK;
        }
        case CLIP_TYPE_IMAGE: {
            if (kind64 == 0 || count != 0 || w == 0 || h == 0)
                return CLIP_ERR_PROTO;
            if (kind64 == CLIP_IMAGE_RGBA) {
                uint64_t bytes = w * h * 4u;
                if (bytes > CLIP_MAX_IMAGE_BYTES || bytes != len)
                    return CLIP_ERR_PROTO;
                unsigned char *buf = NULL;
                int rc = clip_read_framed(fd, (size_t)len, &buf);
                if (rc != CLIP_OK) return rc;
                e->image.kind = CLIP_IMAGE_RGBA;
                e->image.width = (uint32_t)w;
                e->image.height = (uint32_t)h;
                e->image.nbytes = (uint32_t)len;
                e->image.rgba = buf;
                return CLIP_OK;
            }
            if (len == 0 || len >= CLIP_MAX_PATH) return CLIP_ERR_PROTO;
            unsigned char *buf = NULL;
            int rc = clip_read_framed(fd, (size_t)len, &buf);
            if (rc != CLIP_OK) return rc;
            if (memchr(buf, 0, (size_t)len) != NULL) { free(buf); return CLIP_ERR_PROTO; }
            e->image.kind = CLIP_IMAGE_PNG;
            e->image.width = (uint32_t)w;
            e->image.height = (uint32_t)h;
            memcpy(e->image.png_path, buf, (size_t)len);
            e->image.png_path[len] = 0;
            free(buf);
            return CLIP_OK;
        }
        default:
            return CLIP_ERR_PROTO;
    }
}

/* Ask the daemon for the newest entry of a type (ANY = any type). */

static int clip_rpc_get(ClipType want, ClipEntry **out) {
    *out = NULL;
    clip_sock_t fd = clip_daemon_connect();
    if (fd == CLIP_BAD_SOCKET) return CLIP_NODAEMON;

    char req[64];
    snprintf(req, sizeof req, "GET %s\n", clip_type_name(want));
    int rc = CLIP_ERR_IO;
    char resp[512];
    if (clip_send_all(fd, req, strlen(req)) == 0 &&
        clip_recv_line(fd, resp, sizeof resp) >= 0) {
        if (strncmp(resp, "OK GET ", 7) != 0) {
            rc = clip_rpc_error(resp);
        } else {
            char *sp = resp + 7;
            ClipType type;
            if (clip_tok_type(&sp, &type) == 0 &&
                (want == CLIP_TYPE_NONE || type == want)) {
                ClipEntry *e = calloc(1, sizeof *e);
                if (!e) {
                    rc = CLIP_ERR_IO;
                } else {
                    rc = clip_parse_entry(fd, type, &sp, e);
                    if (rc == CLIP_OK) {
                        *out = e;
                    } else {
                        clip_entry_free(e);
                    }
                }
            } else {
                rc = CLIP_ERR_PROTO;
            }
        }
    }
    clip_close_socket(fd);
    return rc;
}

static int clip_rpc_set_line(const char *line, const void *payload,
                             size_t plen, uint64_t *out_id) {
    /* A SET is idempotent by content: the daemon de-duplicates, so if the
     * reply is lost we may repeat it without creating a second entry. A
     * lost reply is exactly what a timeout looks like, and the entry is
     * already on disk at that point. Retry once. */
    for (int attempt = 0; attempt < 2; attempt++) {
        clip_sock_t fd = clip_daemon_connect();
        if (fd == CLIP_BAD_SOCKET) return CLIP_NODAEMON;

        int rc = CLIP_ERR_IO;
        char resp[512];
        if (clip_send_all(fd, line, strlen(line)) == 0 &&
            (plen == 0 || clip_send_all(fd, payload, plen) == 0) &&
            clip_recv_line(fd, resp, sizeof resp) >= 0) {
            if (strncmp(resp, "OK SET ", 7) == 0) {
                char *sp = resp + 7;
                uint64_t id;
                rc = clip_parse_u64(clip_next_tok(&sp), &id) == 0 && id != 0
                         ? CLIP_OK : CLIP_ERR_PROTO;
                if (rc == CLIP_OK && out_id) *out_id = id;
            } else {
                rc = clip_rpc_error(resp);
            }
        }
        clip_close_socket(fd);
        if (rc != CLIP_ERR_IO) return rc;
    }
    return CLIP_ERR_IO;
}

static int clip_rpc_history(int want, ClipEntry **out, int *count) {
    *out = NULL;
    *count = 0;
    clip_sock_t fd = clip_daemon_connect();
    if (fd == CLIP_BAD_SOCKET) return CLIP_NODAEMON;

    char req[64];
    snprintf(req, sizeof req, "HIST %d full\n", want);
    int rc = CLIP_ERR_IO;
    char resp[512];
    if (clip_send_all(fd, req, strlen(req)) == 0 &&
        clip_recv_line(fd, resp, sizeof resp) >= 0) {
        if (strncmp(resp, "OK HIST ", 8) != 0) {
            rc = clip_rpc_error(resp);
        } else {
            uint64_t n = 0;
            char *sp = resp + 8;
            if (clip_tok_u64(&sp, &n) != 0 || n > CLIP_MAX_ENTRIES) {
                rc = CLIP_ERR_PROTO;
            } else {
                ClipEntry *arr = n ? calloc((size_t)n, sizeof *arr) : NULL;
                if (n && !arr) {
                    rc = CLIP_ERR_IO;
                } else {
                    rc = CLIP_OK;
                    for (uint64_t i = 0; i < n; i++) {
                        char line[512];
                        if (clip_recv_line(fd, line, sizeof line) < 0) {
                            rc = CLIP_ERR_PROTO;
                            break;
                        }
                        char *lp = line;
                        char *tag = clip_next_tok(&lp);
                        uint64_t idx = 0;
                        ClipType type;
                        if (!tag || strcmp(tag, "ENTRY") != 0 ||
                            clip_tok_u64(&lp, &idx) != 0 ||
                            idx != i ||
                            clip_tok_type(&lp, &type) != 0) {
                            rc = CLIP_ERR_PROTO;
                            break;
                        }
                        rc = clip_parse_entry(fd, type, &lp, &arr[i]);
                        if (rc != CLIP_OK) break;
                    }
                    if (rc == CLIP_OK) {
                        *out = arr;
                        *count = (int)n;
                    } else {
                        clip_history_free(arr, (int)n);
                    }
                }
            }
        }
    }
    clip_close_socket(fd);
    return rc;
}

static int clip_rpc_pin(int index, int *state) {
    clip_sock_t fd = clip_daemon_connect();
    if (fd == CLIP_BAD_SOCKET) return CLIP_NODAEMON;

    char req[64];
    snprintf(req, sizeof req, "PIN %d\n", index);
    int rc = CLIP_ERR_IO;
    char resp[512];
    if (clip_send_all(fd, req, strlen(req)) == 0 &&
        clip_recv_line(fd, resp, sizeof resp) >= 0) {
        if (strncmp(resp, "OK PIN ", 7) == 0) {
            char *sp = resp + 7;
            uint64_t idx, s;
            if (clip_tok_u64(&sp, &idx) == 0 && clip_tok_u64(&sp, &s) == 0 &&
                idx == (uint64_t)index && s <= 1) {
                if (state) *state = (int)s;
                rc = CLIP_OK;
            } else {
                rc = CLIP_ERR_PROTO;
            }
        } else {
            rc = clip_rpc_error(resp);
        }
    }
    clip_close_socket(fd);
    return rc;
}

static int clip_rpc_clear(int *removed) {
    clip_sock_t fd = clip_daemon_connect();
    if (fd == CLIP_BAD_SOCKET) return CLIP_NODAEMON;

    int rc = CLIP_ERR_IO;
    char resp[512];
    if (clip_send_all(fd, "CLEAR\n", 6) == 0 &&
        clip_recv_line(fd, resp, sizeof resp) >= 0) {
        if (strncmp(resp, "OK CLEAR ", 9) == 0) {
            char *sp = resp + 9;
            uint64_t n;
            if (clip_tok_u64(&sp, &n) == 0 && n <= 0x7FFFFFFFu) {
                if (removed) *removed = (int)n;
                rc = CLIP_OK;
            } else {
                rc = CLIP_ERR_PROTO;
            }
        } else {
            rc = clip_rpc_error(resp);
        }
    }
    clip_close_socket(fd);
    return rc;
}

static int clip_daemon_ping(int *entries) {
    clip_sock_t fd = clip_daemon_connect();
    if (fd == CLIP_BAD_SOCKET) return 0;
    int alive = 0;
    if (clip_send_all(fd, "PING\n", 5) == 0) {
        char resp[256];
        if (clip_recv_line(fd, resp, sizeof resp) >= 0 &&
            strncmp(resp, "OK PONG", 7) == 0) {
            alive = 1;
            if (entries) {
                char *sp = resp + 7;
                uint64_t n;
                if (clip_tok_u64(&sp, &n) == 0 && n <= 0x7FFFFFFFu)
                    *entries = (int)n;
            }
        }
    }
    clip_close_socket(fd);
    return alive;
}

/* ------------------------------------------------------------
 *  Client: file-backed behaviour
 * ------------------------------------------------------------ */

typedef struct {
    int  lock_fd;
    Hist h;
} Fallback;

static int fb_begin(Fallback *fb) {
    memset(fb, 0, sizeof *fb);
    fb->lock_fd = -1;
    hist_init(&fb->h);

    if (clip_mkdir_p(CLIP_STORE_DIR) != 0) return CLIP_ERR_IO;
    int rc = clip_fallback_lock(&fb->lock_fd);
    if (rc != CLIP_OK) return rc;

    /* The daemon may have come up between the failed connect and the
     * lock acquisition. If it has the store, let it be the writer. */
    if (clip_daemon_ping(NULL)) {
        clip_unlock(fb->lock_fd);
        clip_close_file(fb->lock_fd);
        fb->lock_fd = -1;
        return CLIP_NODAEMON;
    }

    rc = store_load(CLIP_STORE_FILE, &fb->h);
    if (rc == CLIP_ERR_PROTO) {
        /* Match clipd: keep the damaged file for inspection, start empty. */
        char bad[sizeof(CLIP_STORE_FILE) + 64];
        snprintf(bad, sizeof bad, "%s.corrupt-%lu", CLIP_STORE_FILE,
                 (unsigned long)time(NULL));
        clip_rename_replace(CLIP_STORE_FILE, bad);
        hist_init(&fb->h);
        rc = CLIP_OK;
    }
    if (rc != CLIP_OK) {
        clip_unlock(fb->lock_fd);
        clip_close_file(fb->lock_fd);
        fb->lock_fd = -1;
        hist_free(&fb->h);
        return rc;
    }
    return CLIP_OK;
}

static void fb_end(Fallback *fb) {
    hist_free(&fb->h);
    if (fb->lock_fd >= 0) {
        clip_unlock(fb->lock_fd);
        clip_close_file(fb->lock_fd);
        fb->lock_fd = -1;
    }
}

static int fb_store(Fallback *fb) {
    return store_save(CLIP_STORE_FILE, &fb->h) == 0 ? CLIP_OK : CLIP_ERR_IO;
}

static int fb_set_text(const char *text, uint32_t len) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    Entry *e = entry_make_text(text, len);
    if (!e) rc = CLIP_ERR_IO;
    else {
        rc = hist_insert(&fb.h, e);
        if (rc == CLIP_OK) rc = fb_store(&fb);
    }
    fb_end(&fb);
    return rc;
}

static int fb_set_files(const char *blob, uint32_t len, uint32_t count) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    Entry *e = entry_make_files(blob, len, count);
    if (!e) rc = CLIP_ERR_INVAL;
    else {
        rc = hist_insert(&fb.h, e);
        if (rc == CLIP_OK) rc = fb_store(&fb);
    }
    fb_end(&fb);
    return rc;
}

static int fb_set_image(const unsigned char *rgba, uint32_t w, uint32_t h) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    Entry *e = entry_make_rgba(rgba, w, h);
    if (!e) rc = CLIP_ERR_TOOBIG;
    else {
        rc = hist_insert(&fb.h, e);
        if (rc == CLIP_OK) rc = fb_store(&fb);
    }
    fb_end(&fb);
    return rc;
}

static int fb_set_png(const char *path, uint32_t w, uint32_t h) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    Entry *e = entry_make_png(path, w, h);
    if (!e) rc = CLIP_ERR_INVAL;
    else {
        rc = hist_insert(&fb.h, e);
        if (rc == CLIP_OK) rc = fb_store(&fb);
    }
    fb_end(&fb);
    return rc;
}

static int fb_pin(int index, int *state) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    int s = hist_pin_toggle(&fb.h, index);
    if (s < 0) rc = s;
    else {
        rc = fb_store(&fb);
        if (rc == CLIP_OK && state) *state = s;
    }
    fb_end(&fb);
    return rc;
}

static int fb_clear(int *removed) {
    Fallback fb;
    int rc = fb_begin(&fb);
    if (rc != CLIP_OK) return rc;

    int n = hist_clear_all(&fb.h);
    rc = fb_store(&fb);
    if (rc == CLIP_OK && removed) *removed = n;
    fb_end(&fb);
    return rc;
}

/* ------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------ */

int clip_set_text(const char *text) {
    if (!text) return clip_set_error(CLIP_ERR_INVAL);
    size_t len = strlen(text);
    if (len == 0) return clip_set_error(CLIP_ERR_INVAL);
    if (len > CLIP_MAX_TEXT) return clip_set_error(CLIP_ERR_TOOBIG);

    char line[64];
    snprintf(line, sizeof line, "SET TEXT %zu\n", len);
    int rc = clip_rpc_set_line(line, text, len, NULL);
    if (rc == CLIP_NODAEMON) {
        rc = fb_set_text(text, (uint32_t)len);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_set_line(line, text, len, NULL);
    }
    return clip_set_error(rc);
}

int clip_set_filelist(const char *const *paths, int count) {
    if (!paths || count <= 0) return clip_set_error(CLIP_ERR_INVAL);
    if (count > CLIP_MAX_FILES) return clip_set_error(CLIP_ERR_TOOBIG);

    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (!paths[i]) return clip_set_error(CLIP_ERR_INVAL);
        size_t n = strlen(paths[i]);
        if (n == 0) return clip_set_error(CLIP_ERR_INVAL);
        if (n >= CLIP_MAX_PATH) return clip_set_error(CLIP_ERR_TOOBIG);
        total += n + 1;
    }
    if (total > (size_t)CLIP_MAX_FILES * CLIP_MAX_PATH)
        return clip_set_error(CLIP_ERR_TOOBIG);

    char *blob = malloc(total);
    if (!blob) return clip_set_error(CLIP_ERR_IO);
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(paths[i]) + 1;
        memcpy(blob + pos, paths[i], n);
        pos += n;
    }
    if (!files_blob_ok(blob, (uint32_t)total, (uint32_t)count)) {
        free(blob);
        return clip_set_error(CLIP_ERR_INVAL);
    }

    char line[64];
    snprintf(line, sizeof line, "SET FILES %d %zu\n", count, total);
    int rc = clip_rpc_set_line(line, blob, total, NULL);
    if (rc == CLIP_NODAEMON) {
        rc = fb_set_files(blob, (uint32_t)total, (uint32_t)count);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_set_line(line, blob, total, NULL);
    }
    free(blob);
    return clip_set_error(rc);
}

int clip_set_image(const void *rgba, int width, int height, int stride) {
    if (!rgba || width <= 0 || height <= 0)
        return clip_set_error(CLIP_ERR_INVAL);
    uint64_t pixels = (uint64_t)(uint32_t)width * (uint32_t)height;
    if (pixels > CLIP_MAX_IMAGE_PIXELS)
        return clip_set_error(CLIP_ERR_TOOBIG);
    uint64_t bytes = pixels * 4u;
    if (bytes > CLIP_MAX_IMAGE_BYTES)
        return clip_set_error(CLIP_ERR_TOOBIG);
    if (stride == 0) stride = width * 4;
    if (stride < width * 4)
        return clip_set_error(CLIP_ERR_INVAL);

    const unsigned char *packed = rgba;
    unsigned char *copy = NULL;
    if (stride != width * 4) {
        copy = malloc((size_t)bytes);
        if (!copy) return clip_set_error(CLIP_ERR_IO);
        for (int y = 0; y < height; y++)
            memcpy(copy + (size_t)y * (size_t)width * 4u,
                   (const unsigned char *)rgba + (size_t)y * (size_t)stride,
                   (size_t)width * 4u);
        packed = copy;
    }

    char line[96];
    snprintf(line, sizeof line, "SET IMAGE RGBA %d %d %zu\n",
             width, height, (size_t)bytes);
    int rc = clip_rpc_set_line(line, packed, (size_t)bytes, NULL);
    if (rc == CLIP_NODAEMON) {
        rc = fb_set_image(packed, (uint32_t)width, (uint32_t)height);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_set_line(line, packed, (size_t)bytes, NULL);
    }
    free(copy);
    return clip_set_error(rc);
}

int clip_set_image_png(const char *png_path) {
    if (!png_path) return clip_set_error(CLIP_ERR_INVAL);
    size_t len = strlen(png_path);
    if (len == 0) return clip_set_error(CLIP_ERR_INVAL);
    if (len >= CLIP_MAX_PATH) return clip_set_error(CLIP_ERR_TOOBIG);

    uint32_t w = 0, h = 0;
    int rc = clip_png_measure(png_path, &w, &h);
    if (rc != CLIP_OK) return clip_set_error(rc);

    char line[64];
    snprintf(line, sizeof line, "SET IMAGE PNG %zu\n", len);
    rc = clip_rpc_set_line(line, png_path, len, NULL);
    if (rc == CLIP_NODAEMON) {
        rc = fb_set_png(png_path, w, h);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_set_line(line, png_path, len, NULL);
    }
    return clip_set_error(rc);
}

ClipEntry *clip_get_current(ClipType type) {
    if (type < CLIP_TYPE_NONE || type > CLIP_TYPE_IMAGE) {
        g_clip_error = CLIP_ERR_INVAL;
        return NULL;
    }
    ClipEntry *e = NULL;
    int rc = clip_rpc_get(type, &e);
    if (rc == CLIP_NODAEMON) {
        Hist h;
        hist_init(&h);
        rc = store_load(CLIP_STORE_FILE, &h);
        if (rc == CLIP_OK) {
            Entry *src = hist_find_newest(&h, type);
            if (!src) {
                rc = CLIP_ERR_EMPTY;
            } else {
                e = calloc(1, sizeof *e);
                if (!e) rc = CLIP_ERR_IO;
                else if (entry_to_public(src, e) != 0) {
                    clip_entry_free(e);
                    e = NULL;
                    rc = CLIP_ERR_IO;
                }
            }
        }
        hist_free(&h);
    }
    g_clip_error = rc;
    return e;
}

int clip_history(ClipEntry **out, int want) {
    if (!out) return clip_set_error(CLIP_ERR_INVAL);
    *out = NULL;
    if (want <= 0 || want > CLIP_MAX_ENTRIES) want = CLIP_MAX_ENTRIES;

    int got = 0;
    int rc = clip_rpc_history(want, out, &got);
    if (rc == CLIP_NODAEMON) {
        Hist h;
        hist_init(&h);
        rc = store_load(CLIP_STORE_FILE, &h);
        if (rc == CLIP_OK) {
            int n = h.n < want ? h.n : want;
            if (n > 0) {
                ClipEntry *arr = calloc((size_t)n, sizeof *arr);
                if (!arr) {
                    rc = CLIP_ERR_IO;
                } else {
                    for (int i = 0; i < n; i++) {
                        if (entry_to_public(h.v[i], &arr[i]) != 0) {
                            clip_history_free(arr, n);
                            arr = NULL;
                            rc = CLIP_ERR_IO;
                            break;
                        }
                    }
                    if (rc == CLIP_OK) {
                        *out = arr;
                        got = n;
                    }
                }
            }
        }
        hist_free(&h);
    }
    if (rc != CLIP_OK) return clip_set_error(rc);
    g_clip_error = CLIP_OK;
    return got;
}

int clip_pin(int index) {
    int state = 0;
    int rc = clip_rpc_pin(index, &state);
    if (rc == CLIP_NODAEMON) {
        rc = fb_pin(index, &state);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_pin(index, &state);
    }
    if (rc != CLIP_OK) return clip_set_error(rc);
    g_clip_error = CLIP_OK;
    return state;
}

int clip_clear(void) {
    int removed = 0;
    int rc = clip_rpc_clear(&removed);
    if (rc == CLIP_NODAEMON) {
        rc = fb_clear(&removed);
        if (rc == CLIP_NODAEMON || rc == CLIP_ERR_BUSY)
            rc = clip_rpc_clear(&removed);
    }
    if (rc != CLIP_OK) return clip_set_error(rc);
    g_clip_error = CLIP_OK;
    return removed;
}

int clip_running(void) {
    int alive = clip_daemon_ping(NULL);
    g_clip_error = CLIP_OK;
    return alive;
}

const char *clip_store_path(void) {
    return CLIP_STORE_FILE;
}

const char *clip_strerror(int rc) {
    switch (rc) {
        case CLIP_OK:         return "ok";
        case CLIP_ERR_INVAL:  return "invalid clipboard argument";
        case CLIP_ERR_TOOBIG: return "clipboard entry is too large";
        case CLIP_ERR_FULL:   return "clipboard history is full of pinned entries";
        case CLIP_ERR_EMPTY:  return "clipboard has no entry of that type";
        case CLIP_ERR_IO:     return "clipboard I/O error";
        case CLIP_ERR_BUSY:   return "clipboard is busy";
        case CLIP_ERR_PROTO:  return "clipboard protocol or store format error";
        case CLIP_ERR_RANGE:  return "clipboard history index out of range";
        default:              return "unknown clipboard error";
    }
}

int clip_last_error(void) {
    return g_clip_error;
}

/* ------------------------------------------------------------
 *  Daemon
 * ------------------------------------------------------------ */

static Hist g_hist;
static volatile sig_atomic_t g_clipd_stop = 0;

static void clipd_stop_signal(int sig) {
    (void)sig;
    g_clipd_stop = 1;
}

static int clip_reply(clip_sock_t fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    return clip_send_all(fd, buf, (size_t)n);
}

static int clip_send_error(clip_sock_t fd, int code, const char *word) {
    return clip_reply(fd, "ERR %d %s\n", code, word ? word : "error");
}

/* The daemon's view of a mutation: insert, persist, reply. The entry is
 * consumed by hist_insert() either way. */
static int daemon_apply(clip_sock_t c, Entry *e) {
    int rc = hist_insert(&g_hist, e);
    if (rc != CLIP_OK) {
        clip_send_error(c, rc, rc == CLIP_ERR_FULL ? "full" : "invalid");
        return -1;
    }
    if (store_save(CLIP_STORE_FILE, &g_hist) != 0) {
        fprintf(stderr, "clipd: cannot write %s: %s\n",
                CLIP_STORE_FILE, strerror(errno));
        Hist tmp;
        hist_init(&tmp);
        int lr = store_load(CLIP_STORE_FILE, &tmp);
        hist_free(&g_hist);
        if (lr == CLIP_OK) g_hist = tmp;
        else hist_init(&g_hist);
        clip_send_error(c, CLIP_ERR_IO, "store");
        return -1;
    }
    clip_reply(c, "OK SET %" PRIu64 "\n", g_hist.v[0]->id);
    return 0;
}

static int daemon_read_payload(clip_sock_t c, size_t len, unsigned char **out) {
    unsigned char *buf = malloc(len ? len : 1);
    if (!buf) return CLIP_ERR_IO;
    if (clip_recv_all(c, buf, len) != 0) { free(buf); return CLIP_ERR_IO; }
    *out = buf;
    return CLIP_OK;
}

static int daemon_set(clip_sock_t c, char *sp) {
    char *kind = clip_next_tok(&sp);
    if (!kind) { clip_send_error(c, CLIP_ERR_INVAL, "set"); return -1; }

    if (strcmp(kind, "TEXT") == 0) {
        uint64_t len = 0;
        if (clip_tok_u64(&sp, &len) != 0 || len == 0 || len > CLIP_MAX_TEXT) {
            clip_send_error(c, CLIP_ERR_TOOBIG, "text");
            return -1;
        }
        unsigned char *buf = NULL;
        int rc = daemon_read_payload(c, (size_t)len, &buf);
        if (rc != CLIP_OK) { clip_send_error(c, rc, "read"); return -1; }
        Entry *e = entry_make_text((const char *)buf, (uint32_t)len);
        free(buf);
        if (!e) { clip_send_error(c, CLIP_ERR_INVAL, "text"); return -1; }
        return daemon_apply(c, e);
    }

    if (strcmp(kind, "FILES") == 0) {
        uint64_t count = 0, len = 0;
        if (clip_tok_u64(&sp, &count) != 0 || clip_tok_u64(&sp, &len) != 0 ||
            count == 0 || count > CLIP_MAX_FILES ||
            len == 0 || len > (uint64_t)CLIP_MAX_FILES * CLIP_MAX_PATH) {
            clip_send_error(c, CLIP_ERR_INVAL, "files");
            return -1;
        }
        unsigned char *buf = NULL;
        int rc = daemon_read_payload(c, (size_t)len, &buf);
        if (rc != CLIP_OK) { clip_send_error(c, rc, "read"); return -1; }
        Entry *e = entry_make_files((const char *)buf, (uint32_t)len,
                                    (uint32_t)count);
        free(buf);
        if (!e) { clip_send_error(c, CLIP_ERR_INVAL, "files"); return -1; }
        return daemon_apply(c, e);
    }

    if (strcmp(kind, "IMAGE") == 0) {
        char *mode = clip_next_tok(&sp);
        if (!mode) { clip_send_error(c, CLIP_ERR_INVAL, "image"); return -1; }

        if (clip_ieq(mode, "RGBA")) {
            uint64_t w = 0, h = 0, len = 0;
            if (clip_tok_u64(&sp, &w) != 0 || clip_tok_u64(&sp, &h) != 0 ||
                clip_tok_u64(&sp, &len) != 0 || w == 0 || h == 0 ||
                w > 0xFFFFu || h > 0xFFFFu) {
                clip_send_error(c, CLIP_ERR_INVAL, "rgba");
                return -1;
            }
            uint64_t pixels = w * h;
            uint64_t bytes = pixels * 4u;
            if (pixels > CLIP_MAX_IMAGE_PIXELS || bytes > CLIP_MAX_IMAGE_BYTES ||
                bytes != len) {
                clip_send_error(c, CLIP_ERR_TOOBIG, "rgba");
                return -1;
            }
            unsigned char *buf = NULL;
            int rc = daemon_read_payload(c, (size_t)len, &buf);
            if (rc != CLIP_OK) { clip_send_error(c, rc, "read"); return -1; }
            Entry *e = entry_make_rgba(buf, (uint32_t)w, (uint32_t)h);
            free(buf);
            if (!e) { clip_send_error(c, CLIP_ERR_TOOBIG, "rgba"); return -1; }
            return daemon_apply(c, e);
        }

        if (clip_ieq(mode, "PNG")) {
            uint64_t len = 0;
            if (clip_tok_u64(&sp, &len) != 0 || len == 0 || len >= CLIP_MAX_PATH) {
                clip_send_error(c, CLIP_ERR_INVAL, "png");
                return -1;
            }
            unsigned char *buf = NULL;
            int rc = daemon_read_payload(c, (size_t)len, &buf);
            if (rc != CLIP_OK) { clip_send_error(c, rc, "read"); return -1; }
            char path[CLIP_MAX_PATH];
            memcpy(path, buf, (size_t)len);
            path[len] = 0;
            free(buf);
            if (memchr(path, 0, (size_t)len) != NULL) {
                clip_send_error(c, CLIP_ERR_INVAL, "png");
                return -1;
            }
            uint32_t w = 0, h = 0;
            rc = clip_png_measure(path, &w, &h);
            if (rc != CLIP_OK) { clip_send_error(c, rc, "png"); return -1; }
            Entry *e = entry_make_png(path, w, h);
            if (!e) { clip_send_error(c, CLIP_ERR_INVAL, "png"); return -1; }
            return daemon_apply(c, e);
        }

        clip_send_error(c, CLIP_ERR_INVAL, "image-kind");
        return -1;
    }

    clip_send_error(c, CLIP_ERR_INVAL, "set-kind");
    return -1;
}

static int daemon_get(clip_sock_t c, char *sp) {
    char *ts = clip_next_tok(&sp);
    ClipType want;
    if (!ts) { clip_send_error(c, CLIP_ERR_INVAL, "get"); return -1; }
    if (clip_ieq(ts, "ANY"))        want = CLIP_TYPE_NONE;
    else if (clip_ieq(ts, "TEXT"))  want = CLIP_TYPE_TEXT;
    else if (clip_ieq(ts, "FILES")) want = CLIP_TYPE_FILES;
    else if (clip_ieq(ts, "IMAGE")) want = CLIP_TYPE_IMAGE;
    else { clip_send_error(c, CLIP_ERR_INVAL, "type"); return -1; }

    Entry *e = hist_find_newest(&g_hist, want);
    if (!e) { clip_send_error(c, CLIP_ERR_EMPTY, "empty"); return -1; }

    size_t plen = 0;
    const unsigned char *payload = entry_payload(e, &plen);
    int kind = e->type == CLIP_TYPE_IMAGE ? e->image_kind : 0;
    if (clip_reply(c,
            "OK GET %s %d %" PRIu64 " %d %" PRIu64 " %u %u %u %zu\n",
            clip_type_name(e->type), kind, e->id, e->pinned, e->stamp,
            entry_width(e), entry_height(e),
            e->type == CLIP_TYPE_FILES ? e->nfiles : 0, plen) != 0)
        return -1;
    if (clip_send_all(c, payload, plen) != 0) return -1;
    if (clip_send_all(c, "\n", 1) != 0) return -1;
    return 0;
}

static size_t clip_utf8_cut(const char *s, size_t n) {
    if (n == 0) return 0;
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return 0;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if (i - 1 + need <= n) return n;
    return i - 1;
}

static char clip_printable(char c) {
    unsigned char u = (unsigned char)c;
    return (u < 0x20 || u == 0x7F) ? ' ' : c;
}

static uint32_t clip_preview(const Entry *e, char *out, size_t cap) {
    if (cap == 0) return 0;
    size_t max = cap - 1;

    if (e->type == CLIP_TYPE_TEXT) {
        size_t n = e->text_len;
        if (n > max) n = clip_utf8_cut(e->text, max);
        for (size_t i = 0; i < n; i++) out[i] = clip_printable(e->text[i]);
        out[n] = 0;
        return (uint32_t)n;
    }

    if (e->type == CLIP_TYPE_FILES) {
        size_t pos = 0;
        const char *p = e->files;
        uint32_t left = e->files_len;
        for (uint32_t f = 0; f < e->nfiles && pos < max; f++) {
            size_t flen = strlen(p);
            if (f && pos + 2 < max) { out[pos++] = ','; out[pos++] = ' '; }
            for (size_t i = 0; i < flen && pos < max; i++)
                out[pos++] = clip_printable(p[i]);
            p += flen + 1;
            left -= (uint32_t)(flen + 1);
        }
        (void)left;
        out[pos] = 0;
        return (uint32_t)pos;
    }

    char tmp[CLIP_MAX_PATH + 64];
    int n;
    if (e->image_kind == CLIP_IMAGE_RGBA)
        n = snprintf(tmp, sizeof tmp, "rgba %ux%u, %u bytes",
                     e->width, e->height, e->nbytes);
    else
        n = snprintf(tmp, sizeof tmp, "png %ux%u %s",
                     e->width, e->height, e->png_path);
    if (n < 0) { out[0] = 0; return 0; }
    size_t len = (size_t)n;
    if (len >= sizeof tmp) len = sizeof tmp - 1;
    if (len > max) len = clip_utf8_cut(tmp, max);
    for (size_t i = 0; i < len; i++) out[i] = clip_printable(tmp[i]);
    out[len] = 0;
    return (uint32_t)len;
}

static int daemon_hist(clip_sock_t c, char *sp) {
    uint64_t want = 0;
    if (clip_tok_u64(&sp, &want) != 0 || want > CLIP_MAX_ENTRIES) {
        clip_send_error(c, CLIP_ERR_INVAL, "hist");
        return -1;
    }
    int full = 0;
    char *mode = clip_next_tok(&sp);
    if (mode) {
        if (clip_ieq(mode, "FULL")) full = 1;
        else if (!clip_ieq(mode, "PREVIEW")) {
            clip_send_error(c, CLIP_ERR_INVAL, "hist-mode");
            return -1;
        }
    }
    int n = g_hist.n < (int)want ? g_hist.n : (int)want;
    if (clip_reply(c, "OK HIST %d\n", n) != 0) return -1;

    for (int i = 0; i < n; i++) {
        Entry *e = g_hist.v[i];
        int kind = e->type == CLIP_TYPE_IMAGE ? e->image_kind : 0;
        size_t plen = 0;
        const unsigned char *payload = entry_payload(e, &plen);
        if (!full) {
            char pv[CLIP_MAX_PREVIEW + 1];
            uint32_t pn = clip_preview(e, pv, sizeof pv);
            if (clip_reply(c, "ENTRY %d %s %d %" PRIu64 " %d %" PRIu64 " %u\n",
                           i, clip_type_name(e->type), kind, e->id,
                           e->pinned, e->stamp, pn) != 0)
                return -1;
            if (clip_send_all(c, pv, pn) != 0) return -1;
            if (clip_send_all(c, "\n", 1) != 0) return -1;
        } else {
            if (clip_reply(c,
                    "ENTRY %d %s %d %" PRIu64 " %d %" PRIu64 " %u %u %u %zu\n",
                    i, clip_type_name(e->type), kind, e->id, e->pinned,
                    e->stamp, entry_width(e), entry_height(e),
                    e->type == CLIP_TYPE_FILES ? e->nfiles : 0, plen) != 0)
                return -1;
            if (clip_send_all(c, payload, plen) != 0) return -1;
            if (clip_send_all(c, "\n", 1) != 0) return -1;
        }
    }
    return 0;
}

static int daemon_pin(clip_sock_t c, char *sp) {
    uint64_t idx = 0;
    if (clip_tok_u64(&sp, &idx) != 0 || idx > 0x7FFFFFFFu) {
        clip_send_error(c, CLIP_ERR_RANGE, "index");
        return -1;
    }
    int state = hist_pin_toggle(&g_hist, (int)idx);
    if (state < 0) {
        clip_send_error(c, state, "index");
        return -1;
    }
    if (store_save(CLIP_STORE_FILE, &g_hist) != 0) {
        /* Put the flag back so memory matches the file that is still there. */
        g_hist.v[idx]->pinned = !g_hist.v[idx]->pinned;
        clip_send_error(c, CLIP_ERR_IO, "store");
        return -1;
    }
    clip_reply(c, "OK PIN %" PRIu64 " %d\n", idx, state);
    return 0;
}

static int daemon_clear(clip_sock_t c) {
    int removed = hist_clear_all(&g_hist);
    if (store_save(CLIP_STORE_FILE, &g_hist) != 0) {
        Hist tmp;
        hist_init(&tmp);
        int lr = store_load(CLIP_STORE_FILE, &tmp);
        hist_free(&g_hist);
        if (lr == CLIP_OK) g_hist = tmp;
        else hist_init(&g_hist);
        clip_send_error(c, CLIP_ERR_IO, "store");
        return -1;
    }
    clip_reply(c, "OK CLEAR %d\n", removed);
    return 0;
}

static int daemon_client_ok(const char *name, const char *pid) {
    size_t n = strlen(name);
    if (n == 0 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) return 0;
    }
    uint64_t v;
    return clip_parse_u64(pid, &v) == 0;
}

/* Returns 0 to keep the connection, -1 to close it. */
static int daemon_command(clip_sock_t c, char *line) {
    char *sp = line;
    char *verb = clip_next_tok(&sp);
    if (!verb) return 0;

    if (strcmp(verb, "QUIT") == 0) {
        clip_send_all(c, "BYE\n", 4);
        return -1;
    }
    if (strcmp(verb, "PING") == 0) {
        clip_reply(c, "OK PONG %d\n", g_hist.n);
        return 0;
    }
    if (strcmp(verb, "CLIENT") == 0) {
        char *name = clip_next_tok(&sp);
        char *pid = clip_next_tok(&sp);
        if (!name || !pid || !daemon_client_ok(name, pid)) {
            clip_send_error(c, CLIP_ERR_INVAL, "client");
            return -1;
        }
        uint64_t pidv;
        clip_parse_u64(pid, &pidv);
        clip_reply(c, "OK CLIPD %d %" PRIu64 " %d\n",
                   CLIP_PROTOCOL, pidv, g_hist.n);
        return 0;
    }
    if (strcmp(verb, "SET") == 0)    return daemon_set(c, sp);
    if (strcmp(verb, "GET") == 0)    return daemon_get(c, sp);
    if (strcmp(verb, "HIST") == 0)   return daemon_hist(c, sp);
    if (strcmp(verb, "PIN") == 0)    return daemon_pin(c, sp);
    if (strcmp(verb, "CLEAR") == 0)  return daemon_clear(c);

    clip_send_error(c, CLIP_ERR_PROTO, "command");
    return -1;
}

static void daemon_handle(clip_sock_t c) {
    for (;;) {
        char line[1024];
        if (clip_recv_line(c, line, sizeof line) < 0) return;
        if (daemon_command(c, line) != 0) return;
    }
}

int clipd_main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("usage: clipd\n"
                   "  Serves the Mystic clipboard on %s.\n"
                   "  History: %s (at most %d entries).\n",
                   CLIP_SOCKET_PATH, CLIP_STORE_FILE, CLIP_MAX_ENTRIES);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("clipd protocol %d\n", CLIP_PROTOCOL);
            return 0;
        }
        fprintf(stderr, "clipd: unknown argument '%s' (try --help)\n", argv[1]);
        return 2;
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    umask(0077);
#endif
    if (clip_net_init() != 0) {
        fprintf(stderr, "clipd: cannot initialise sockets\n");
        return 1;
    }
    if (clip_mkdir_p(CLIP_STORE_DIR) != 0) {
        fprintf(stderr, "clipd: cannot create %s: %s\n",
                CLIP_STORE_DIR, strerror(errno));
        return 1;
    }

    int lock_fd = -1;
    int rc = clip_daemon_lock(&lock_fd);
    if (rc != CLIP_OK) {
        if (rc == CLIP_ERR_BUSY)
            fprintf(stderr, "clipd: another instance owns %s\n", CLIP_LOCK_FILE);
        else
            fprintf(stderr, "clipd: cannot lock %s: %s\n",
                    CLIP_LOCK_FILE, strerror(errno));
        return rc == CLIP_ERR_BUSY ? 0 : 1;
    }

    hist_init(&g_hist);
    rc = store_load(CLIP_STORE_FILE, &g_hist);
    if (rc == CLIP_ERR_PROTO) {
        char bad[sizeof(CLIP_STORE_FILE) + 64];
        snprintf(bad, sizeof bad, "%s.corrupt-%lu", CLIP_STORE_FILE,
                 (unsigned long)time(NULL));
        if (clip_rename_replace(CLIP_STORE_FILE, bad) == 0)
            fprintf(stderr, "clipd: %s was corrupt; moved to %s\n",
                    CLIP_STORE_FILE, bad);
        else
            fprintf(stderr, "clipd: %s was corrupt and could not be moved\n",
                    CLIP_STORE_FILE);
        hist_init(&g_hist);
    } else if (rc != CLIP_OK) {
        fprintf(stderr, "clipd: cannot read %s: %s\n",
                CLIP_STORE_FILE, strerror(errno));
        clip_unlock(lock_fd);
        clip_close_file(lock_fd);
        return 1;
    }

    char *sock_dir = clip_dirname(CLIP_SOCKET_PATH);
    if (!sock_dir || clip_mkdir_p(sock_dir) != 0) {
        fprintf(stderr, "clipd: cannot create socket directory for %s\n",
                CLIP_SOCKET_PATH);
        free(sock_dir);
        hist_free(&g_hist);
        clip_unlock(lock_fd);
        clip_close_file(lock_fd);
        return 1;
    }
    free(sock_dir);

    /* The lock proves no other clipd is alive, so a socket file left by
     * a crash is stale and safe to remove. */
    clip_unlink(CLIP_SOCKET_PATH);

    clip_sock_t listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == CLIP_BAD_SOCKET) {
        fprintf(stderr, "clipd: socket: %s\n", strerror(errno));
        hist_free(&g_hist);
        clip_unlock(lock_fd);
        clip_close_file(lock_fd);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(CLIP_SOCKET_PATH);
    if (path_len == 0 || path_len >= sizeof addr.sun_path) {
        fprintf(stderr, "clipd: socket path is too long: %s\n", CLIP_SOCKET_PATH);
        clip_close_socket(listen_fd);
        hist_free(&g_hist);
        clip_unlock(lock_fd);
        clip_close_file(lock_fd);
        return 1;
    }
    memcpy(addr.sun_path, CLIP_SOCKET_PATH, path_len + 1);

    if (bind(listen_fd, (struct sockaddr *)&addr,
             (clip_socklen_t)sizeof addr) != 0 ||
        listen(listen_fd, 64) != 0) {
        fprintf(stderr, "clipd: cannot listen on %s: %s\n",
                CLIP_SOCKET_PATH, strerror(errno));
        clip_close_socket(listen_fd);
        hist_free(&g_hist);
        clip_unlock(lock_fd);
        clip_close_file(lock_fd);
        return 1;
    }
#ifndef _WIN32
    chmod(CLIP_SOCKET_PATH, 0600);
#endif

    signal(SIGTERM, clipd_stop_signal);
    signal(SIGINT, clipd_stop_signal);
    fprintf(stderr, "clipd: pid %ld, %d entr%s, %s\n",
            (long)clip_getpid(), g_hist.n, g_hist.n == 1 ? "y" : "ies",
            CLIP_SOCKET_PATH);

    while (!g_clipd_stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(listen_fd, &rf);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int r = select((int)listen_fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        if (r == 0) continue;
        clip_sock_t c = accept(listen_fd, NULL, NULL);
        if (c == CLIP_BAD_SOCKET) continue;
        clip_sock_timeout(c, 5);
        daemon_handle(c);
        clip_close_socket(c);
    }

    clip_close_socket(listen_fd);
    clip_unlink(CLIP_SOCKET_PATH);
    hist_free(&g_hist);
    clip_unlock(lock_fd);
    clip_close_file(lock_fd);
    return 0;
}

#ifdef CLIPD_MAIN
int main(int argc, char **argv) {
    return clipd_main(argc, argv);
}
#endif
