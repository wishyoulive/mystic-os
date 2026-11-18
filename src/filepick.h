/* ============================================================
 *  Mystic OS - filepick.h
 *  ONE file picker for the whole desktop: the open/save dialog the
 *  desktop draws, the request protocol any app uses to ask for it,
 *  and the dialog's state machine.
 *
 *  WHY IT IS A DESKTOP SERVICE
 *  A sandboxed app cannot fork, cannot see the user's files and must
 *  not paint outside its own window, so it can no more show a file
 *  dialog than it can launch a program. The dialog therefore belongs
 *  to the desktop (src/main.c), which is the one process that already
 *  owns the screen, the keyboard and the real filesystem. Apps ask for
 *  it and read the answer; they never draw it.
 *
 *  THE PROTOCOL - two small files, both written atomically
 *  (temp file + rename, so a reader never sees half a line):
 *
 *      request                              result
 *      -------------------------------      ---------------------------
 *      token=Ab12Xy                         token=Ab12Xy
 *      mode=open                            path=/root/Pictures/a.png
 *      filter=png,jpg                    or
 *      start=/root/Pictures                 token=Ab12Xy
 *      name=untitled.png                    cancel
 *
 *  token   1..16 characters from [A-Za-z0-9]. The desktop copies it
 *          into the result and nothing else identifies the exchange,
 *          so a result left over from a previous dialog can never be
 *          collected by a new one: filepick_result_take() compares the
 *          token it was given with the one in the file and returns
 *          "nothing" on any difference (not even a prefix matches).
 *  mode    "open" (default) or "save".
 *  filter  "*" for every file, or a comma list of extensions -
 *          "png,jpg" is the wallpaper chooser. Directories are always
 *          shown; the filter is applied to files only, by the real
 *          name of each entry, case-insensitively.
 *  start   the directory to show first. "" or a missing path falls
 *          back to the nearest ancestor that can be listed, and the
 *          footer keeps the real reason ("/x/y: No such file or
 *          directory"). In save mode a start that names a FILE opens
 *          its directory and suggests its name.
 *  name    save mode only: the file name the field starts with. Only
 *          the last path component is used; it is a name, not a path.
 *
 *  WHERE THE FILES LIVE
 *  A process outside a jail uses
 *      /run/mystic/filepick.request  ->  /run/mystic/filepick.result
 *  A sandboxed app (wmsrv set MYSTIC_BUNDLE and chrooted it) cannot
 *  write /run/mystic at all - the jail's /run is root-owned - so it
 *  uses the one directory its jail does give it, /tmp:
 *      /tmp/filepick.request         ->  /tmp/filepick.result
 *  The desktop reads that at
 *      /run/mystic/jail/<app id>/tmp/filepick.request
 *  (the same file, through the jail directory the desktop created),
 *  answers in the same place, and the app collects it with the same
 *  filepick_result_take(). One protocol, one implementation; only the
 *  directory differs, and filepick_paths() picks it from the
 *  environment the sandbox already exports.
 *
 *  WHAT CONSUMES A CHOSEN PATH
 *  The desktop returns the path and does nothing else with it - it
 *  never opens, writes or copies the file on the requester's behalf.
 *  The app that asked consumes it:
 *      Settings   the Wallpaper page's "Choose image" row: the chosen
 *                 file goes through wallpaper_set_image() (decode or
 *                 refuse with the real reason) and then into
 *                 /etc/mystic/wallpaper as "image:<path>".
 *      Files      Ctrl+O / Ctrl+S: reveals the chosen file in its
 *                 directory and names it in the status line.
 *      A sandboxed app gets the host path; whether it can READ that
 *      path is the sandbox's decision (the document it was launched
 *      with is the only file bound into its jail), so a browser's
 *      "save as" still has to hand the path back to the desktop to
 *      write. The picker is the chooser, not the writer.
 *
 *  THE STATE MACHINE
 *  Everything below is the dialog's behaviour - scan, filter, sort,
 *  navigate, filter by typing, the save field, the one overwrite
 *  question, cancel - with no fonts, no pixels and no libc state.
 *  src/main.c owns one FilePick and draws it; the host harness in
 *  .work/filepick/ compiles this header and drives the very same
 *  functions with synthetic keys on a scratch tree, which is why the
 *  behaviour below cannot drift away from the dialog on screen.
 *
 *  Every name and size comes from opendir(3) + stat(2); a directory
 *  that cannot be read keeps its path, shows errno's own sentence and
 *  has an empty list, so Backspace can leave it. Nothing is invented:
 *  no placeholder rows, no guessed sizes, no "0 items" for a failure.
 * ============================================================ */
#ifndef MYSTIC_FILEPICK_H
#define MYSTIC_FILEPICK_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ limits */
#define FILEPICK_TOKEN_MAX     16
#define FILEPICK_PATH_MAX     512
#define FILEPICK_NAME_MAX     128
#define FILEPICK_FILTER_MAX    96
#define FILEPICK_NOTICE_MAX   256
#define FILEPICK_MAX_ENTRIES  400
/* Rows the dialog shows when nothing has told the state machine the
 * real height yet. The desktop replaces it every draw from the font
 * metrics; the harness sets it explicitly. */
#define FILEPICK_DEFAULT_ROWS   8

/* ------------------------------------------------------------ files */
#ifndef FILEPICK_REQUEST
#define FILEPICK_REQUEST "/run/mystic/filepick.request"
#endif
#ifndef FILEPICK_RESULT
#define FILEPICK_RESULT  "/run/mystic/filepick.result"
#endif
/* The two paths a requester uses. Outside a jail: the desktop's own
 * /run/mystic files. Inside one (MYSTIC_BUNDLE is set by wmsrv after
 * the chroot): the jail's own /tmp, which is the only writable place
 * a sandboxed app has. */
static inline void filepick_paths(char *req, size_t reqcap,
                                  char *res, size_t rescap) {
    const char *bundle = getenv("MYSTIC_BUNDLE");
    if (bundle && bundle[0]) {
        snprintf(req, reqcap, "%s", "/tmp/filepick.request");
        snprintf(res, rescap, "%s", "/tmp/filepick.result");
    } else {
        snprintf(req, reqcap, "%s", FILEPICK_REQUEST);
        snprintf(res, rescap, "%s", FILEPICK_RESULT);
    }
}

/* ------------------------------------------------------------ modes */
enum { FILEPICK_OPEN = 0, FILEPICK_SAVE = 1 };

/* What the dialog's input handler answers. FILEPICK_NONE means "still
 * open, nothing decided"; the other two end the dialog and are what the
 * desktop turns into the result file. */
typedef enum {
    FILEPICK_NONE   = 0,
    FILEPICK_CHOSEN = 1,
    FILEPICK_CANCEL = 2
} FpEvent;

/* ------------------------------------------------------------ state */
typedef struct {
    char          name[FILEPICK_NAME_MAX];  /* without a trailing slash */
    long long     size;                     /* st_size, real bytes     */
    unsigned char is_dir;
} FilePickEntry;

typedef struct {
    int  active;
    int  mode;                                  /* FILEPICK_OPEN/SAVE  */
    char token[FILEPICK_TOKEN_MAX + 1];
    char filter[FILEPICK_FILTER_MAX];           /* "*" or "png,jpg"    */
    char dir[FILEPICK_PATH_MAX];                /* always absolute     */
    char name[FILEPICK_NAME_MAX];               /* save-mode file name */
    int  name_len;

    FilePickEntry ent[FILEPICK_MAX_ENTRIES];    /* everything stat'd    */
    int  n;
    int  vis[FILEPICK_MAX_ENTRIES];             /* what the filter lets through */
    int  nvis;
    int  sel;                                   /* index into vis[]    */
    int  scroll;                                /* first visible row   */
    int  rows;                                  /* rows the caller can show */
    int  truncated;                             /* > MAX_ENTRIES existed */

    int  err;                                   /* errno from opendir, 0 = none */
    char notice[FILEPICK_NOTICE_MAX];           /* the real reason, or a prompt */

    int  typed_on;                              /* a filter query is up */
    char typed[FILEPICK_FILTER_MAX];
    int  typed_len;

    int  name_focus;                            /* save: the field owns typing */
    int  overwrite;                             /* the one save question is up */
    char overwrite_name[FILEPICK_NAME_MAX];
} FilePick;

/* ------------------------------------------------------------ helpers */
static inline int fp_lc(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static inline int fp_isalnum(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static inline int fp_eq_ci_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (fp_lc((unsigned char)a[i]) != fp_lc((unsigned char)b[i])) return 0;
    return 1;
}

/* Case-insensitive ordering, then a byte comparison so two names that
 * differ only in case keep a fixed order (qsort needs a total order). */
static inline int fp_cmp_ci(const char *a, const char *b) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (*pa && *pb) {
        int ca = fp_lc(*pa), cb = fp_lc(*pb);
        if (ca != cb) return ca - cb;
        pa++; pb++;
    }
    if (*pa) return 1;
    if (*pb) return -1;
    return strcmp(a, b);
}

/* Case-insensitive substring search; the empty needle matches anything. */
static inline int fp_has_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               fp_lc((unsigned char)p[i]) == fp_lc((unsigned char)needle[i]))
            i++;
        if (i == n) return 1;
    }
    return 0;
}

/* "png, jpg" / ".png,.jpg" / "PNG" -> "png,jpg"; "*" stays "*"; nothing
 * usable becomes "*" (an empty filter must not hide every file). */
static inline void fp_filter_normalise(const char *in, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (!in) { snprintf(out, cap, "%s", "*"); return; }
    while (*in == ' ' || *in == '\t') in++;
    if (*in == '*') { snprintf(out, cap, "%s", "*"); return; }

    size_t o = 0;
    for (; *in ; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '.' || c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (o > 0 && out[o - 1] != ',' && o + 1 < cap) out[o++] = ',';
            continue;
        }
        if (!fp_isalnum(c)) continue;              /* no globs, no slashes */
        if (o + 1 >= cap) break;
        out[o++] = (char)fp_lc(c);
    }
    while (o > 0 && out[o - 1] == ',') o--;
    out[o] = 0;
    if (!o) snprintf(out, cap, "%s", "*");
}

/* 1 when `name` passes the filter. Directories are handled by the caller
 * (they are always shown). A file with no extension never matches a real
 * filter - that is what the filter means, not a guess about its type. */
static inline int fp_ext_match(const char *name, const char *filter) {
    if (!filter || !filter[0] || !strcmp(filter, "*")) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    const char *ext = dot + 1;
    const char *p = filter;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (strlen(ext) == n && fp_eq_ci_n(ext, p, n)) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static inline void fp_join(const char *dir, const char *name, char *out, size_t cap) {
    if (!dir || !dir[0]) dir = "/";
    if (!strcmp(dir, "/")) snprintf(out, cap, "/%s", name);
    else                   snprintf(out, cap, "%s/%s", dir, name);
}

/* Resolve the . and .. components of a path by walking it, so a request
 * cannot climb above / and every later join is a plain concatenation.
 * An absolute start stays absolute; a relative one stays relative (the
 * desktop's cwd is /, so there it means the same thing, and a host test
 * can point the state machine at a scratch tree). "" becomes "/". */
static inline void fp_clean_path(const char *in, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (!in || !in[0]) { snprintf(out, cap, "%s", "/"); return; }

    int abs = (in[0] == '/');
    size_t o = 0;
    if (abs) out[o++] = '/';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 0) continue;
        if (n == 1 && start[0] == '.') continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            /* pop one component, never past the root (or the start) */
            if (o > (abs ? 1u : 0u)) {
                if (o > 0 && out[o - 1] == '/') o--;
                while (o > (abs ? 1u : 0u) && out[o - 1] != '/') o--;
            }
            out[o] = 0;
            continue;
        }
        if (o > (abs ? 1u : 0u) && out[o - 1] != '/') {
            if (o + 1 >= cap) break;
            out[o++] = '/';
        }
        if (o + n >= cap) break;                   /* refuse to truncate a name */
        memcpy(out + o, start, n);
        o += n;
        out[o] = 0;
    }
    if (o == 0) snprintf(out, cap, "%s", abs ? "/" : ".");
}

/* ------------------------------------------------------------ listing */
static inline int fp_ent_cmp(const void *pa, const void *pb) {
    const FilePickEntry *a = (const FilePickEntry *)pa;
    const FilePickEntry *b = (const FilePickEntry *)pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;   /* directories first */
    return fp_cmp_ci(a->name, b->name);
}

static inline void fp_clamp_scroll(FilePick *fp) {
    int rows = fp->rows > 0 ? fp->rows : 1;
    if (fp->nvis <= 0) { fp->sel = 0; fp->scroll = 0; return; }
    if (fp->sel < 0) fp->sel = 0;
    if (fp->sel >= fp->nvis) fp->sel = fp->nvis - 1;
    if (fp->sel < fp->scroll) fp->scroll = fp->sel;
    if (fp->sel >= fp->scroll + rows) fp->scroll = fp->sel - rows + 1;
    int maxs = fp->nvis - rows;
    if (maxs < 0) maxs = 0;
    if (fp->scroll > maxs) fp->scroll = maxs;
    if (fp->scroll < 0) fp->scroll = 0;
}

/* Rebuild vis[] from the extension filter and the typed query, keeping the
 * selected entry selected when it survives the new filter. */
static inline void fp_refilter(FilePick *fp) {
    char keep[FILEPICK_NAME_MAX];
    keep[0] = 0;
    if (fp->sel >= 0 && fp->sel < fp->nvis && fp->nvis > 0)
        snprintf(keep, sizeof(keep), "%s", fp->ent[fp->vis[fp->sel]].name);

    fp->nvis = 0;
    for (int i = 0; i < fp->n; i++) {
        const FilePickEntry *e = &fp->ent[i];
        if (!e->is_dir && !fp_ext_match(e->name, fp->filter)) continue;
        if (fp->typed_len > 0 && !fp_has_ci(e->name, fp->typed)) continue;
        fp->vis[fp->nvis++] = i;
    }
    fp->sel = 0;
    if (keep[0]) {
        for (int i = 0; i < fp->nvis; i++) {
            if (!strcmp(fp->ent[fp->vis[i]].name, keep)) { fp->sel = i; break; }
        }
    }
    fp_clamp_scroll(fp);
}

/* opendir(dir) + stat() for every entry. A failed opendir keeps the path
 * (so Backspace can climb out of it) and puts strerror(errno) in notice. */
static inline void fp_scan(FilePick *fp) {
    fp->n = 0;
    fp->nvis = 0;
    fp->truncated = 0;
    fp->err = 0;
    fp->notice[0] = 0;

    DIR *d = opendir(fp->dir);
    if (!d) {
        fp->err = errno ? errno : EIO;
        snprintf(fp->notice, sizeof(fp->notice), "%s: %s",
                 fp->dir, strerror(fp->err));
        fp->sel = 0;
        fp->scroll = 0;
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t nl = strlen(de->d_name);
        if (nl == 0 || nl >= FILEPICK_NAME_MAX) continue;
        if (fp->n >= FILEPICK_MAX_ENTRIES) { fp->truncated = 1; break; }

        char full[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 2];
        int w = snprintf(full, sizeof(full), "%s%s%s", fp->dir,
                         strcmp(fp->dir, "/") ? "/" : "", de->d_name);
        if (w < 0 || (size_t)w >= sizeof(full)) continue;  /* never a cut path */

        struct stat st;
        if (stat(full, &st) != 0) continue;   /* vanished between readdir and stat */

        FilePickEntry *e = &fp->ent[fp->n];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->is_dir = (unsigned char)(S_ISDIR(st.st_mode) ? 1 : 0);
        e->size = (long long)st.st_size;
        fp->n++;
    }
    closedir(d);

    qsort(fp->ent, (size_t)fp->n, sizeof(fp->ent[0]), fp_ent_cmp);
    fp->sel = 0;
    fp->scroll = 0;
    fp_refilter(fp);
}

/* Start a dialog from a request (or, in the harness, directly). */
static inline void filepick_begin(FilePick *fp, int mode, const char *token,
                                  const char *filter, const char *start,
                                  const char *name) {
    memset(fp, 0, sizeof(*fp));
    fp->active = 1;
    fp->mode = (mode == FILEPICK_SAVE) ? FILEPICK_SAVE : FILEPICK_OPEN;
    fp->rows = FILEPICK_DEFAULT_ROWS;
    snprintf(fp->token, sizeof(fp->token), "%s", token ? token : "");
    fp_filter_normalise(filter, fp->filter, sizeof(fp->filter));

    /* The suggested name is a name: keep its last component only. */
    if (name && name[0]) {
        const char *slash = strrchr(name, '/');
        const char *base = slash ? slash + 1 : name;
        size_t n = strlen(base);
        if (n >= sizeof(fp->name)) n = sizeof(fp->name) - 1;
        memcpy(fp->name, base, n);
        fp->name[n] = 0;
        fp->name_len = (int)n;
    }

    char want[FILEPICK_PATH_MAX];
    fp_clean_path(start, want, sizeof(want));

    struct stat st;
    if (fp->mode == FILEPICK_SAVE && want[0] && stat(want, &st) == 0 &&
        !S_ISDIR(st.st_mode)) {
        /* A save request may name the file it is replacing. Open its
         * directory and put that name in the field. The base name is
         * copied before the path is cut at its last slash. */
        char dir[FILEPICK_PATH_MAX];
        char base[FILEPICK_NAME_MAX];
        base[0] = 0;
        snprintf(dir, sizeof(dir), "%s", want);
        char *slash = strrchr(dir, '/');
        if (slash) {
            snprintf(base, sizeof(base), "%s", slash + 1);
            if (slash == dir) dir[1] = 0;
            else *slash = 0;
        }
        if (!fp->name_len && base[0]) {
            snprintf(fp->name, sizeof(fp->name), "%s", base);
            fp->name_len = (int)strlen(fp->name);
        }
        snprintf(want, sizeof(want), "%s", dir[0] ? dir : "/");
    }

    snprintf(fp->dir, sizeof(fp->dir), "%s", want[0] ? want : "/");
    fp_scan(fp);

    /* A start that cannot be listed must not be a dead end: climb to the
     * nearest ancestor that can, and keep the original reason on screen. */
    if (fp->err) {
        char why[FILEPICK_NOTICE_MAX];
        snprintf(why, sizeof(why), "%s", fp->notice);
        int guard = 0;
        while (fp->err && strcmp(fp->dir, "/") && guard++ < FILEPICK_PATH_MAX) {
            char up[FILEPICK_PATH_MAX];
            snprintf(up, sizeof(up), "%s", fp->dir);
            char *slash = strrchr(up, '/');
            if (!slash) break;
            if (slash == up) up[1] = 0;
            else *slash = 0;
            snprintf(fp->dir, sizeof(fp->dir), "%s", up);
            fp_scan(fp);
        }
        if (!fp->err && why[0])
            snprintf(fp->notice, sizeof(fp->notice), "%s", why);
    }
}

/* ------------------------------------------------------------ movement */
static inline void fp_move(FilePick *fp, int delta) {
    if (fp->nvis <= 0) return;
    fp->sel += delta;
    fp_clamp_scroll(fp);
}

static inline void fp_home(FilePick *fp, int end) {
    if (fp->nvis <= 0) return;
    fp->sel = end ? fp->nvis - 1 : 0;
    fp_clamp_scroll(fp);
}

/* Backspace: the parent directory. "" when already at the root. */
static inline int fp_back(FilePick *fp) {
    if (!strcmp(fp->dir, "/")) return 0;
    char up[FILEPICK_PATH_MAX];
    snprintf(up, sizeof(up), "%s", fp->dir);
    char *slash = strrchr(up, '/');
    if (!slash) return 0;
    if (slash == up) up[1] = 0;
    else *slash = 0;
    snprintf(fp->dir, sizeof(fp->dir), "%s", up);
    fp->typed[0] = 0;
    fp->typed_len = 0;
    fp->typed_on = 0;
    fp->overwrite = 0;
    fp_scan(fp);
    return 1;
}

static inline void fp_descend(FilePick *fp, const char *name) {
    char path[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 2];
    fp_join(fp->dir, name, path, sizeof(path));
    snprintf(fp->dir, sizeof(fp->dir), "%s", path);
    fp->typed[0] = 0;
    fp->typed_len = 0;
    fp->typed_on = 0;
    fp->overwrite = 0;
    fp_scan(fp);
}

/* A breadcrumb click: jump to an ancestor/self by absolute path. */
static inline void fp_goto(FilePick *fp, const char *path) {
    fp_clean_path(path, fp->dir, sizeof(fp->dir));
    fp->typed[0] = 0;
    fp->typed_len = 0;
    fp->typed_on = 0;
    fp->overwrite = 0;
    fp_scan(fp);
}

/* ------------------------------------------------------------ choosing */
/* The absolute path the dialog is currently pointing at:
 *   save mode - dir/name (the field),
 *   open mode - dir/selected entry,
 * and "" when there is nothing to point at. */
static inline const char *filepick_target(FilePick *fp, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!fp || !out || cap == 0) return out;
    if (fp->mode == FILEPICK_SAVE) {
        const char *nm = fp->overwrite ? fp->overwrite_name : fp->name;
        if (!nm[0]) return out;
        fp_join(fp->dir, nm, out, cap);
        return out;
    }
    if (fp->sel < 0 || fp->sel >= fp->nvis) return out;
    fp_join(fp->dir, fp->ent[fp->vis[fp->sel]].name, out, cap);
    return out;
}

/* Enter. In open mode a directory descends and a file is the answer; in
 * save mode Enter saves the field into the current directory, asking once
 * if a file is already there (a second Enter on the same name confirms). */
static inline FpEvent filepick_enter(FilePick *fp) {
    if (!fp || !fp->active) return FILEPICK_NONE;

    if (fp->mode == FILEPICK_SAVE) {
        if (fp->name_len <= 0) {
            snprintf(fp->notice, sizeof(fp->notice), "%s",
                     "type a file name");
            return FILEPICK_NONE;
        }
        char path[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 2];
        fp_join(fp->dir, fp->name, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(fp->notice, sizeof(fp->notice),
                     "%s is a directory - type a file name", fp->name);
            return FILEPICK_NONE;
        }
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (!fp->overwrite || strcmp(fp->overwrite_name, fp->name) != 0) {
                fp->overwrite = 1;
                snprintf(fp->overwrite_name, sizeof(fp->overwrite_name), "%s", fp->name);
                snprintf(fp->notice, sizeof(fp->notice),
                         "%s already exists - Enter replaces it, Esc keeps it",
                         fp->name);
                return FILEPICK_NONE;
            }
        }
        fp->overwrite = 0;
        fp->notice[0] = 0;
        return FILEPICK_CHOSEN;
    }

    if (fp->nvis <= 0) return FILEPICK_NONE;
    const FilePickEntry *e = &fp->ent[fp->vis[fp->sel]];
    if (e->is_dir) {
        fp_descend(fp, e->name);
        return FILEPICK_NONE;
    }
    return FILEPICK_CHOSEN;
}

/* Right arrow / l / a second click on the selected row: open it. */
static inline FpEvent filepick_open_selected(FilePick *fp) {
    if (!fp || !fp->active || fp->nvis <= 0) return FILEPICK_NONE;
    const FilePickEntry *e = &fp->ent[fp->vis[fp->sel]];
    if (e->is_dir) {
        fp_descend(fp, e->name);
        return FILEPICK_NONE;
    }
    if (fp->mode == FILEPICK_SAVE) {
        /* clicking a file in a save dialog means "save over this name" */
        snprintf(fp->name, sizeof(fp->name), "%s", e->name);
        fp->name_len = (int)strlen(fp->name);
        fp->overwrite = 0;
        return FILEPICK_NONE;
    }
    return FILEPICK_CHOSEN;
}

/* ------------------------------------------------------------ input */
/* `bytes` is the same byte stream an app receives from the desktop:
 * printable text, '\n' for Enter, '\b'/0x7F for Backspace, '\t' for Tab
 * and the usual ESC [ ... sequences for the arrows, Home/End and
 * PageUp/PageDown. Returns the event that ends the dialog, if any. */
static inline FpEvent filepick_key(FilePick *fp, const char *bytes, int n) {
    if (!fp || !fp->active || !bytes || n <= 0) return FILEPICK_NONE;

    for (int i = 0; i < n; ) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B && i + 1 < n && bytes[i + 1] == '[') {
            int j = i + 2, arg = 0, have = 0;
            while (j < n && bytes[j] >= '0' && bytes[j] <= '9') {
                arg = arg * 10 + (bytes[j] - '0');
                have = 1;
                j++;
            }
            if (j >= n) break;                     /* split sequence: dropped */
            char fin = bytes[j];
            i = j + 1;
            if (fin == 'A')      fp_move(fp, -1);
            else if (fin == 'B') fp_move(fp, +1);
            else if (fin == 'C') { FpEvent ev = filepick_open_selected(fp); if (ev) return ev; }
            else if (fin == 'D') {
                if (fp->name_focus) fp->name_focus = 0;
                else fp_back(fp);
            } else if (fin == 'H') fp_home(fp, 0);
            else if (fin == 'F') fp_home(fp, 1);
            else if (fin == '~' && have) {
                int page = fp->rows > 1 ? fp->rows - 1 : 1;
                if (arg == 5) fp_move(fp, -page);
                else if (arg == 6) fp_move(fp, +page);
                else if (arg == 1 || arg == 7) fp_home(fp, 0);
                else if (arg == 4 || arg == 8) fp_home(fp, 1);
            }
            continue;
        }

        if (c == 0x1B) {
            /* Esc dismisses the overwrite question first, then the dialog:
             * a destructive question must not be the thing a single Escape
             * is taken to mean. */
            if (fp->overwrite) {
                fp->overwrite = 0;
                fp->notice[0] = 0;
                i++;
                continue;
            }
            return FILEPICK_CANCEL;
        }

        i++;
        if (c == '\n' || c == '\r') {
            FpEvent ev = filepick_enter(fp);
            if (ev != FILEPICK_NONE) return ev;
            continue;
        }
        if (c == '\t') {
            if (fp->mode == FILEPICK_SAVE) fp->name_focus = !fp->name_focus;
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (fp->mode == FILEPICK_SAVE && fp->name_focus) {
                if (fp->name_len > 0) {
                    fp->name[--fp->name_len] = 0;
                    fp->overwrite = 0;
                }
                continue;
            }
            /* A typed filter is edited first; an empty one climbs. That
             * order is what makes both "Backspace goes up" and "typing
             * filters" true with one key. */
            if (fp->typed_len > 0) {
                fp->typed[--fp->typed_len] = 0;
                if (fp->typed_len == 0) fp->typed_on = 0;
                fp_refilter(fp);
                continue;
            }
            fp_back(fp);
            continue;
        }
        if (c < 0x20) continue;

        if (fp->mode == FILEPICK_SAVE && fp->name_focus) {
            if (c == '/') continue;                /* a name, not a path */
            if (fp->name_len < (int)sizeof(fp->name) - 1) {
                fp->name[fp->name_len++] = (char)c;
                fp->name[fp->name_len] = 0;
                fp->overwrite = 0;
            }
            continue;
        }
        if (fp->typed_len < (int)sizeof(fp->typed) - 1) {
            fp->typed[fp->typed_len++] = (char)c;
            fp->typed[fp->typed_len] = 0;
            fp->typed_on = 1;
            fp_refilter(fp);
        }
    }
    return FILEPICK_NONE;
}

/* Wheel: move the view by whole rows and pull the selection along, so the
 * highlighted row is never scrolled away. */
static inline void filepick_wheel(FilePick *fp, int notches) {
    if (!fp || !fp->active || notches == 0 || fp->nvis <= 0) return;
    int rows = fp->rows > 0 ? fp->rows : 1;
    int maxs = fp->nvis - rows;
    if (maxs < 0) maxs = 0;
    fp->scroll -= notches * 3;
    if (fp->scroll < 0) fp->scroll = 0;
    if (fp->scroll > maxs) fp->scroll = maxs;
    if (fp->sel < fp->scroll) fp->sel = fp->scroll;
    if (fp->sel >= fp->scroll + rows) fp->sel = fp->scroll + rows - 1;
}

/* ------------------------------------------------------------ request files */
static inline int fp_token_ok(const char *t) {
    if (!t || !t[0]) return 0;
    size_t n = strlen(t);
    if (n > FILEPICK_TOKEN_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!fp_isalnum((unsigned char)t[i])) return 0;
    return 1;
}

/* Read one "key=value" line. Returns 1 when the key matched. */
static inline int fp_line_kv(const char *line, const char *key,
                             char *out, size_t cap) {
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != '=') return 0;
    const char *v = line + kl + 1;
    size_t n = strlen(v);
    while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' ||
                     v[n - 1] == ' '  || v[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
    return 1;
}

/* A token for a new request: [A-Za-z0-9]{1,16}, unique within this
 * process. The pid keeps two processes apart, the counter keeps two
 * requests from one process apart, and the clock keeps two runs apart. */
static inline void fp_make_token(char *out, size_t cap) {
    static unsigned counter;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) ts.tv_sec = ts.tv_nsec = 0;
    counter++;
    snprintf(out, cap, "f%05x%03x%05x",
             (unsigned)(getpid() & 0xFFFFF),
             (unsigned)(counter & 0xFFF),
             (unsigned)(((unsigned long long)ts.tv_nsec / 1000ull) & 0xFFFFF));
}

/* Write a small file atomically: <path>.tmp then rename over <path>.
 * Returns 0 on success, -1 with errno kept and a sentence in err. */
static inline int fp_write_atomic(const char *path, const char *data, size_t len,
                                  char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    char tmp[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 8];
    int w = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (w < 0 || (size_t)w >= sizeof(tmp)) {
        if (err && esz) snprintf(err, esz, "path is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err && esz) snprintf(err, esz, "%s: %s", tmp, strerror(errno));
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t wr = write(fd, data + off, len - off);
        if (wr < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            unlink(tmp);
            if (err && esz) snprintf(err, esz, "%s: %s", tmp, strerror(e));
            errno = e;
            return -1;
        }
        off += (size_t)wr;
    }
    if (close(fd) != 0) {
        int e = errno;
        unlink(tmp);
        if (err && esz) snprintf(err, esz, "%s: %s", tmp, strerror(e));
        errno = e;
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
#ifdef _WIN32
        /* The host harness runs on Windows, where rename() refuses to
         * replace an existing file. The target (musl) replaces it
         * atomically, so this branch never runs there. */
        if (remove(path) == 0 && rename(tmp, path) == 0) return 0;
        e = errno;
#endif
        unlink(tmp);
        if (err && esz) snprintf(err, esz, "%s: %s", path, strerror(e));
        errno = e;
        return -1;
    }
    return 0;
}

/* Ask the desktop for the dialog. Returns 0 when the request file is on
 * disk with `token` in it, -1 with the real reason otherwise. */
static inline int filepick_request(const char *reqpath, int mode,
                                   const char *filter, const char *start,
                                   const char *name, char *token, size_t tcap,
                                   char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    if (!token || tcap < 2) {
        if (err && esz) snprintf(err, esz, "%s", "no room for a token");
        return -1;
    }
    char tok[FILEPICK_TOKEN_MAX + 1];
    fp_make_token(tok, sizeof(tok));
    snprintf(token, tcap, "%s", tok);

    char body[FILEPICK_PATH_MAX + FILEPICK_FILTER_MAX + FILEPICK_NAME_MAX + 96];
    int n = snprintf(body, sizeof(body),
                     "token=%s\nmode=%s\nfilter=%s\nstart=%s\nname=%s\n",
                     tok, mode == FILEPICK_SAVE ? "save" : "open",
                     filter && filter[0] ? filter : "*",
                     start ? start : "/", name ? name : "");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        if (err && esz) snprintf(err, esz, "%s", "the request does not fit");
        return -1;
    }
    return fp_write_atomic(reqpath, body, (size_t)n, err, esz);
}

/* Read a request. Returns 0 when it is complete and its token is legal,
 * -1 otherwise with the reason in err (a caller removes the file either
 * way: a request nobody can answer must not be re-read every frame). */
static inline int filepick_read_request(const char *reqpath, FilePick *fp,
                                        char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    FILE *f = fopen(reqpath, "r");
    if (!f) {
        if (err && esz) snprintf(err, esz, "%s: %s", reqpath, strerror(errno));
        return -1;
    }
    char line[FILEPICK_PATH_MAX + FILEPICK_FILTER_MAX + 64];
    char token[FILEPICK_TOKEN_MAX + 8] = "";
    char mode[16] = "open", filter[FILEPICK_FILTER_MAX] = "*";
    char start[FILEPICK_PATH_MAX] = "/", name[FILEPICK_NAME_MAX] = "";
    while (fgets(line, (int)sizeof(line), f)) {
        if (fp_line_kv(line, "token", token, sizeof(token))) continue;
        if (fp_line_kv(line, "mode", mode, sizeof(mode))) continue;
        if (fp_line_kv(line, "filter", filter, sizeof(filter))) continue;
        if (fp_line_kv(line, "start", start, sizeof(start))) continue;
        (void)fp_line_kv(line, "name", name, sizeof(name));
    }
    fclose(f);

    if (!fp_token_ok(token)) {
        if (err && esz) snprintf(err, esz, "%s", "bad token in the request");
        return -1;
    }
    int m = strcmp(mode, "save") == 0 ? FILEPICK_SAVE : FILEPICK_OPEN;
    filepick_begin(fp, m, token, filter, start, name);
    return 0;
}

/* Write the answer. `ev` is FILEPICK_CHOSEN (with path) or CANCEL. */
static inline int filepick_write_result(const char *respath, const char *token,
                                        FpEvent ev, const char *path,
                                        char *err, size_t esz) {
    if (err && esz) err[0] = 0;
    char body[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 64];
    int n;
    if (ev == FILEPICK_CHOSEN && path && path[0])
        n = snprintf(body, sizeof(body), "token=%s\npath=%s\n", token, path);
    else
        n = snprintf(body, sizeof(body), "token=%s\ncancel\n", token);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        if (err && esz) snprintf(err, esz, "%s", "the result does not fit");
        return -1;
    }
    return fp_write_atomic(respath, body, (size_t)n, err, esz);
}

/* Collect an answer. Returns FILEPICK_CHOSEN with `path` filled,
 * FILEPICK_CANCEL, or FILEPICK_NONE while there is nothing for THIS
 * token. A result written for another token is left untouched and is
 * never returned: that is the stale-dialog rule. */
static inline FpEvent filepick_result_take(const char *respath, const char *token,
                                           char *path, size_t cap) {
    if (path && cap) path[0] = 0;
    FILE *f = fopen(respath, "r");
    if (!f) return FILEPICK_NONE;

    char line[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 64];
    char got[FILEPICK_TOKEN_MAX + 8] = "";
    char answer[FILEPICK_PATH_MAX + FILEPICK_NAME_MAX + 8] = "";
    int cancelled = 0;
    while (fgets(line, (int)sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (fp_line_kv(line, "token", got, sizeof(got))) continue;
        if (!strcmp(line, "cancel")) { cancelled = 1; continue; }
        (void)fp_line_kv(line, "path", answer, sizeof(answer));
    }
    fclose(f);

    if (!token || !got[0] || strcmp(got, token) != 0)
        return FILEPICK_NONE;                     /* somebody else's answer */
    if (cancelled) return FILEPICK_CANCEL;
    if (answer[0]) {
        if (path && cap) snprintf(path, cap, "%s", answer);
        return FILEPICK_CHOSEN;
    }
    return FILEPICK_CANCEL;                       /* a malformed answer is not a choice */
}

#endif /* MYSTIC_FILEPICK_H */
