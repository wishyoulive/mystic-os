/* ============================================================
 *  Mystic OS - apps/store.c
 *  "Apps": one window, two sections.
 *
 *  Installed - the bundles that are already on the disk, read with
 *  readdir() from /usr/share/mystic/apps and /root/apps (that is
 *  where `pkg install` unpacks them).  A directory counts as a
 *  bundle when it holds a readable 'manifest'; the manifest is
 *  parsed the way src/wmsrv.c parses it (name/desc/exec, one '='
 *  per line) and the size shown is the real st_size of
 *  <dir>/<id>/<exec>.
 *
 *  Available - the packages the repository advertises, read from
 *  /run/mystic/repo.index.  The index is the same TSV the installer
 *  caches (stage/usr/bin/pkg):
 *
 *      name<TAB>version<TAB>size<TAB>sha256<TAB>file<TAB>description
 *
 *  A package whose name is also an installed bundle (a directory in
 *  either bundle root) is painted as 'installed' and Enter will not
 *  offer it again.
 *
 *  This window never forks and never installs by itself.  It asks
 *  the desktop, which owns the privileged work:
 *
 *    Enter on an Installed row -> /run/mystic/launch.request   (one id)
 *    Enter on an Available row -> /run/mystic/install.request   (one name)
 *    'r' or the Refresh button -> /run/mystic/repo.refresh      ("refresh")
 *
 *  The desktop validates the install name against [a-z0-9-]{1,32}
 *  before running `pkg install <name>`.  This file applies the same
 *  rule twice: a row whose name would fail it is never listed, so
 *  the single line written to install.request can only ever be a
 *  name the desktop accepts.
 *
 *  An install is asynchronous: pkg downloads, verifies and unpacks
 *  over there. The window watches the bundle root for the new
 *  directory, counts the seconds while it waits, and after
 *  STORE_INSTALL_TIMEOUT_SEC gives up with a plain "did not finish"
 *  and offers the row again - a request that never completes must
 *  not sit in the status line for ever.
 *
 *  Two coordinate spaces: draw() gets this window's origin on the
 *  compositor surface and paints at origin + local, while click()
 *  gets APP-LOCAL coordinates (src/main.c: app_draw(cur->app, s,
 *  cx0, cy0, ...) vs app_click(hit->app, lx - 1, ly - tb - 1, ...)).
 *  Every rectangle in here - the Refresh button, the row band, each
 *  column edge - is app-local, so the painter adds the origin and the
 *  hit-test never does. That is the rule src/apps/files.c spells out;
 *  mixing the two only looks right while the window sits at (0,0).
 *
 *  Geometry comes from one layout_store() Layout: the gutter, the
 *  icon box, the row height and every column edge are measured from
 *  the real fonts and ui_px(), and the lines are centred with the
 *  face's own ascent (line_top()). draw(), key(), click() and the wheel
 *  all read that one Layout, so they cannot disagree about where a row
 *  is - and nothing is a hand-tuned padding constant.
 *
 *  Rows carry a drawn mark: a parcel for a package that can be
 *  installed, a tick for one already on the disk (sections carry the
 *  same marks so the list explains itself). Both are geometry built
 *  from the theme's accent - no images.
 *
 *  Keys:  j/k or Up/Down  move        Enter  launch / install
 *         Home/End/g/G    first/last  PgUp/PgDn  by a screen
 *         r               refresh the repo index
 *  Mouse: click the Refresh button, click a row to select it, click
 *         the selected row again to activate it. The wheel scrolls the
 *         list by WHEEL_ROWS rows a notch and keeps the selection inside
 *         the window (positive = up).
 * ============================================================ */
#include "mystic.h"
#include "ipc.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Every path this window touches. The -D overrides exist so the host
 * harness can point the app at a scratch tree instead of the real /run. */
#ifndef STORE_APP_DIR
#define STORE_APP_DIR WM_APP_DIR
#endif
#ifndef STORE_ROOT_APPS
#define STORE_ROOT_APPS "/root/apps"
#endif
#ifndef STORE_REPO_INDEX
#define STORE_REPO_INDEX "/run/mystic/repo.index"
#endif
#ifndef STORE_REPO_REFRESH
#define STORE_REPO_REFRESH "/run/mystic/repo.refresh"
#endif
#ifndef STORE_INSTALL_REQUEST
#define STORE_INSTALL_REQUEST "/run/mystic/install.request"
#endif
#ifndef STORE_LAUNCH_REQUEST
#define STORE_LAUNCH_REQUEST "/run/mystic/launch.request"
#endif

/* how long an install may run before the window stops claiming it is
 * running (pkg's own wget timeout is 300 s; a local repo answers in
 * seconds, so a minute and a half of silence means it did not happen) */#ifndef STORE_INSTALL_TIMEOUT_SEC
/* pkg allows 300 s per network read (stage/usr/bin/pkg) and the
 * browser package is 573 MB, so a shorter deadline would declare a
 * healthy download dead and offer the row again mid-transfer. */
#define STORE_INSTALL_TIMEOUT_SEC 900.0
#endif

#define DESC_LINE 256
#define DESC_MAXLINES 3
#define DESC_WORDS 128          /* a 255-char line cannot hold more words */
#define PKG_MAX 64              /* repo rows we hold; the index is small */
#define INST_MAX (2 * WM_MAX_APPS)   /* both bundle roots together */
#define DISP_MAX (INST_MAX + PKG_MAX + 6)

/* Rows the wheel moves per notch. A count, not a pixel size: it moves
 * whole entries, so it follows the text scale for free. */
#define WHEEL_ROWS 3

typedef struct {
    char id[32];
    char name[64];
    char desc[DESC_LINE];
    char exe[128];
    long size;          /* st_size of <dir>/<id>/<exe>, 0 if unreadable */
    int  have_size;
    int  from_root;     /* came from /root/apps, where pkg installs */
} StoreApp;

typedef struct {
    char name[33];   /* pkg names may be 32 chars + NUL */
    char version[24];
    char desc[DESC_LINE];
    long size;          /* bytes, the index's size column */
    int  have_size;
} Pkg;

/* one painted line of the window: a section title, a real row, or a note */
enum { D_SECT = 1, D_APP, D_PKG, D_MSG };

/* the mark painted in a row's icon column: a parcel for an installable
 * package, a tick for anything already on the disk */
enum { MARK_NONE = 0, MARK_PKG, MARK_CHECK };

typedef struct {
    int         kind;
    int         idx;    /* D_APP: inst[] index, D_PKG: avail[] index,
                           D_SECT: how many rows the section holds */
    const char *msg;    /* D_SECT label / D_MSG text (static or Store-owned) */
    unsigned char mark; /* MARK_*: the icon a section title carries */
} Disp;

typedef struct {
    StoreApp inst[INST_MAX];
    int      ninst;

    Pkg      avail[PKG_MAX];
    int      navail;

    /* the index as it was when we last read it; a change on disk makes
     * draw() read it again, which is how a refresh becomes visible */
    int       index_state;   /* 0 = no file, 1 = read, -1 = could not read */
    char      index_err[160];
    long long index_mtime;
    long long index_size;

    Disp     disp[DISP_MAX];
    int      ndisp;
    int      sel;            /* index into disp[], always a selectable row */
    int      scroll;
    int      w, h;

    char     error[128];     /* why the bundle directory could not be read */
    char     status[128];    /* result of the last request we wrote */
    char     pending[32];    /* name asked for, until its directory appears */
    double   pending_since;  /* CLOCK_MONOTONIC when that request was written,
                                0 when nothing is in flight */

    char     dlines[DESC_MAXLINES][DESC_LINE];  /* wrapped description */
    int      dn;
    int      wrap_w;         /* width the wrap was computed for */
    char     wrap_id[64];    /* which row the wrap holds; "" if none */
} Store;

/* ------------------------------------------------------------
 * The desktop's rule, copied so this window can never hand it a
 * name it would refuse: lowercase letters, digits and '-', 1..32.
 * ------------------------------------------------------------ */
static int pkg_name_ok(const char *n) {
    if (!n || !n[0] || strlen(n) > 32) return 0;
    for (const char *c = n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'))
            return 0;
    return 1;
}

/* ------------------------------------------------------------
 * Manifest parsing - identical rules to wms_scan_apps() in
 * src/wmsrv.c: strip the newline, split on the first '=', trim
 * spaces around the key, take the value verbatim.
 * ------------------------------------------------------------ */
static int read_manifest(const char *dir, StoreApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/manifest", dir, a->id);

    /* defaults first, so a bundle with no manifest (a pkg-installed
     * directory) still has a name and a payload to show */
    snprintf(a->name, sizeof(a->name), "%s", a->id);
    snprintf(a->exe, sizeof(a->exe), "bin");

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;
        char *ke = k + strlen(k);
        while (ke > k && ke[-1] == ' ') *--ke = 0;
        if (!strcmp(k, "name"))      snprintf(a->name, sizeof(a->name), "%s", v);
        else if (!strcmp(k, "desc")) snprintf(a->desc, sizeof(a->desc), "%s", v);
        else if (!strcmp(k, "exec")) snprintf(a->exe,  sizeof(a->exe),  "%s", v);
    }
    fclose(f);

    if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
    if (!a->exe[0])  snprintf(a->exe,  sizeof(a->exe),  "bin");
    return 1;
}

/* The payload is a real file on disk; its size is what we show. */
static void probe_payload(const char *dir, StoreApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", dir, a->id, a->exe);

    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        a->size = (long)st.st_size;
        a->have_size = 1;
    } else {
        a->size = 0;
        a->have_size = 0;
    }
}

static int app_before(const StoreApp *a, const StoreApp *b) {
    return strcmp(a->id, b->id) < 0;
}

static void sort_apps(StoreApp *e, int n) {
    for (int i = 1; i < n; i++) {
        StoreApp key = e[i];
        int j = i - 1;
        while (j >= 0 && app_before(&key, &e[j])) { e[j + 1] = e[j]; j--; }
        e[j + 1] = key;
    }
}

static int pkg_before(const Pkg *a, const Pkg *b) {
    return strcmp(a->name, b->name) < 0;
}

static void sort_pkgs(Pkg *e, int n) {
    for (int i = 1; i < n; i++) {
        Pkg key = e[i];
        int j = i - 1;
        while (j >= 0 && pkg_before(&key, &e[j])) { e[j + 1] = e[j]; j--; }
        e[j + 1] = key;
    }
}

/* ------------------------------------------------------------
 * The display list. Section titles and notes take a row slot but
 * are not selectable, so one flat selection can walk from an
 * installed row to an available one with no special cases.
 * ------------------------------------------------------------ */
static int disp_selectable(const Disp *d) {
    return d->kind == D_APP || d->kind == D_PKG;
}

static void disp_push(Store *s, int kind, int idx, const char *msg) {
    if (s->ndisp >= DISP_MAX) return;
    Disp *d = &s->disp[s->ndisp++];
    d->kind = kind;
    d->idx = idx;
    d->msg = msg;
    d->mark = MARK_NONE;
}

static void build_disp(Store *s) {
    int old = s->sel;
    char keep[64];
    keep[0] = 0;

    if (old >= 0 && old < s->ndisp && disp_selectable(&s->disp[old])) {
        Disp *d = &s->disp[old];
        if (d->kind == D_APP)
            snprintf(keep, sizeof(keep), "a:%s", s->inst[d->idx].id);
        else
            snprintf(keep, sizeof(keep), "p:%s", s->avail[d->idx].name);
    }

    s->ndisp = 0;
    s->sel = -1;

    disp_push(s, D_SECT, s->ninst, "Installed");
    if (s->ndisp > 0) s->disp[s->ndisp - 1].mark = MARK_CHECK;
    if (s->ninst == 0)
        disp_push(s, D_MSG, 0, "no app bundles installed");
    else
        for (int i = 0; i < s->ninst; i++)
            disp_push(s, D_APP, i, NULL);

    disp_push(s, D_SECT, s->navail, "Available");
    if (s->ndisp > 0) s->disp[s->ndisp - 1].mark = MARK_PKG;
    if (s->index_state == 0)
        disp_push(s, D_MSG, 0, "no index - press r to fetch it");
    else if (s->index_state < 0)
        disp_push(s, D_MSG, 0, s->index_err);
    else if (s->navail == 0)
        disp_push(s, D_MSG, 0, "the index lists no packages");
    else
        for (int i = 0; i < s->navail; i++)
            disp_push(s, D_PKG, i, NULL);

    /* same row as before, when it still exists */
    if (keep[0]) {
        for (int i = 0; i < s->ndisp && s->sel < 0; i++) {
            Disp *d = &s->disp[i];
            if (d->kind == D_APP && !strncmp(keep, "a:", 2) &&
                !strcmp(keep + 2, s->inst[d->idx].id))
                s->sel = i;
            else if (d->kind == D_PKG && !strncmp(keep, "p:", 2) &&
                     !strcmp(keep + 2, s->avail[d->idx].name))
                s->sel = i;
        }
    }
    if (s->sel < 0) {
        /* nearest selectable at or after where we were, else the last one */
        int start = old < 0 ? 0 : old;
        if (start > s->ndisp) start = s->ndisp;
        for (int i = start; i < s->ndisp && s->sel < 0; i++)
            if (disp_selectable(&s->disp[i])) s->sel = i;
        for (int i = s->ndisp - 1; i >= 0 && s->sel < 0; i--)
            if (disp_selectable(&s->disp[i])) s->sel = i;
    }
    if (s->sel >= 0 && s->scroll > s->sel) s->scroll = s->sel;
    if (s->sel < 0) s->scroll = 0;

    /* the band text follows the list, so its wrap is stale now */
    s->wrap_id[0] = 0;
    s->wrap_w = -1;
    s->dn = 0;
}

/* ------------------------------------------------------------
 * Reading the two things this window knows about: the bundles on
 * disk, and the index the desktop downloaded.
 * ------------------------------------------------------------ */
static void add_bundle(Store *s, const char *dir, const char *id, int from_root) {
    if (s->ninst >= INST_MAX) return;
    if (!id[0] || strlen(id) >= sizeof(s->inst[0].id)) return;
    if (strchr(id, '/')) return;
    for (int i = 0; i < s->ninst; i++)
        if (!strcmp(s->inst[i].id, id)) return;      /* already listed */

    StoreApp *a = &s->inst[s->ninst];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    /* a directory in the shared bundle root without a manifest is not an
     * app; /root/apps is pkg's own root, so list whatever it put there */
    if (!read_manifest(dir, a) && !from_root) return;
    if (!a->desc[0] && from_root)
        snprintf(a->desc, sizeof(a->desc), "installed by pkg");
    a->from_root = from_root;
    probe_payload(dir, a);
    s->ninst++;
}

static void scan_installed(Store *s) {
    s->ninst = 0;
    s->error[0] = 0;

    DIR *d = opendir(STORE_APP_DIR);
    if (!d) {
        snprintf(s->error, sizeof(s->error), "%s: %s", STORE_APP_DIR, strerror(errno));
    } else {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            add_bundle(s, STORE_APP_DIR, de->d_name, 0);
        }
        closedir(d);
    }

    d = opendir(STORE_ROOT_APPS);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            add_bundle(s, STORE_ROOT_APPS, de->d_name, 1);
        }
        closedir(d);
    }

    sort_apps(s->inst, s->ninst);
    build_disp(s);
}

/* An index row that is already on disk. Two answers, in this order:
 * a listed bundle with that id, then - the literal rule - a directory of
 * that name inside either bundle root, even when its manifest is
 * unreadable and it never made it into the Installed list. */
static int is_installed(const Store *s, const char *name) {
    for (int i = 0; i < s->ninst; i++)
        if (!strcmp(s->inst[i].id, name)) return 1;

    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", STORE_APP_DIR, name);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    snprintf(path, sizeof(path), "%s/%s", STORE_ROOT_APPS, name);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    return 0;
}

static void load_index(Store *s) {
    s->navail = 0;
    s->index_err[0] = 0;
    s->index_state = 0;
    s->index_mtime = 0;
    s->index_size = -1;

    struct stat st;
    if (stat(STORE_REPO_INDEX, &st) != 0) {
        if (errno != ENOENT) {
            s->index_state = -1;
            snprintf(s->index_err, sizeof(s->index_err), "%s: %s",
                     STORE_REPO_INDEX, strerror(errno));
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        s->index_state = -1;
        snprintf(s->index_err, sizeof(s->index_err), "%s: not a file", STORE_REPO_INDEX);
        return;
    }

    FILE *f = fopen(STORE_REPO_INDEX, "r");
    if (!f) {
        s->index_state = -1;
        snprintf(s->index_err, sizeof(s->index_err), "%s: %s",
                 STORE_REPO_INDEX, strerror(errno));
        return;
    }

    char line[512];
    while (s->navail < PKG_MAX && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        if (!line[0]) continue;

        char *field[6];
        int nf = 0;
        char *p = line;
        while (nf < 6) {
            field[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }

        /* the desktop would refuse this name, so it is not offered at all */
        if (!pkg_name_ok(field[0])) continue;

        Pkg *k = &s->avail[s->navail];
        memset(k, 0, sizeof(*k));
        snprintf(k->name, sizeof(k->name), "%s", field[0]);
        if (nf > 1) snprintf(k->version, sizeof(k->version), "%s", field[1]);
        if (nf > 2) {
            char *end = NULL;
            long v = strtol(field[2], &end, 10);
            if (end && end != field[2] && v > 0) {
                k->size = v;
                k->have_size = 1;
            }
        }
        if (nf > 5) snprintf(k->desc, sizeof(k->desc), "%s", field[5]);
        s->navail++;
    }

    int ioerr = ferror(f);
    fclose(f);

    if (ioerr) {
        s->navail = 0;
        s->index_state = -1;
        snprintf(s->index_err, sizeof(s->index_err), "%s: %s",
                 STORE_REPO_INDEX, strerror(EIO));
        return;
    }

    sort_pkgs(s->avail, s->navail);
    s->index_state = 1;
    s->index_mtime = (long long)st.st_mtime;
    s->index_size  = (long long)st.st_size;
}

/* The desktop downloads the index with wget -O, so the file can exist and
 * still be half written. Watching (mtime, size) instead of a one-shot flag
 * means the finished file is read on the next draw, and a refresh the user
 * asked for needs no timer: it appears as soon as the file changes. */
static void sync_index(Store *s) {
    struct stat st;
    if (stat(STORE_REPO_INDEX, &st) != 0) {
        if (s->index_state != 0) {          /* it was here and is gone */
            load_index(s);
            build_disp(s);
        }
        return;
    }
    if (s->index_state == 1 &&
        (long long)st.st_mtime == s->index_mtime &&
        (long long)st.st_size  == s->index_size)
        return;

    int had = s->index_state;
    int before = s->navail;
    load_index(s);
    build_disp(s);

    if (s->index_state == 1 && (had != 1 || s->navail != before))
        snprintf(s->status, sizeof(s->status), "index: %d package%s",
                 s->navail, s->navail == 1 ? "" : "s");
}

/* `pkg install` unpacks into /root/apps. Until that directory shows up the
 * row stays available; once it does, the bundle list and the marks follow.
 * A request that never produces a directory is not allowed to report
 * "installing ..." for ever: see STORE_INSTALL_TIMEOUT_SEC. */
static void poll_install(Store *s) {
    if (!s->pending[0]) return;

    /* success is a real directory, in pkg's own root or in the image root
     * (a bundle can also land there, and that is where the launcher looks) */
    const char *roots[2] = { STORE_ROOT_APPS, STORE_APP_DIR };
    char path[512];
    struct stat st;
    for (int i = 0; i < 2; i++) {
        /* pkg creates the directory BEFORE it untars into it, so a bare
         * directory means "download in progress", not "installed". Wait for
         * the manifest, which is what makes the bundle launchable. */
        snprintf(path, sizeof(path), "%s/%s/manifest", roots[i], s->pending);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            char done[sizeof(s->pending)];
            snprintf(done, sizeof(done), "%s", s->pending);
            s->pending[0] = 0;
            s->pending_since = 0.0;
            scan_installed(s);
            snprintf(s->status, sizeof(s->status), "installed %s", done);
            return;
        }
    }

    /* still running. There is no timer to be called back on, so the wait is
     * measured on the monotonic clock here and shown as it grows. */
    if (s->pending_since > 0.0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            double waited = now - s->pending_since;
            if (waited < 0.0) waited = 0.0;

            if (waited >= STORE_INSTALL_TIMEOUT_SEC) {
                /* the desktop forked pkg and nothing arrived: say so, stop
                 * claiming progress, and leave the row installable again.
                 * The deadline needs CLOCK_MONOTONIC - the same clock
                 * src/main.c and src/apps/files.c time themselves with - so
                 * if the kernel cannot tell time nothing here can either. */
                snprintf(s->status, sizeof(s->status),
                         "install of %s did not finish - Enter retries; run pkg to see why",
                         s->pending);
                s->pending[0] = 0;
                s->pending_since = 0.0;
                return;
            }
            if (waited >= 1.0)
                snprintf(s->status, sizeof(s->status),
                         "installing %s... %lds", s->pending, (long)waited);
        }
    }
}

/* ------------------------------------------------------------
 * Geometry: one Layout, one grid, four callers
 *
 * draw(), key(), click() and the wheel all call layout_store(), so the
 * rows they act on are the rows that were painted. There is no ROW_H or
 * PAD constant: the gutter is ui_px(), a row is the row font's
 * font_height() plus leading, the icon is a square inside that row, and
 * every text line is centred in its band from font_ascent() (line_top()),
 * never from a guessed +2. A change of text scale moves all of it at
 * once, because font_get() applies the scale before these numbers are
 * measured.
 *
 * The horizontal grid, left to right:
 *
 *   pad_x | ICON | icon_gap | NAME (name_w, measured from the widest
 *         real name) | gap | DESCRIPTION (flexible) | gap | SIZE
 *         (right-aligned in size_col) | gap | TAG (right-aligned in
 *         tag_col) | pad_x
 *
 * Columns are dropped from the right when the window cannot hold them
 * (tag first, then size, then the description), so the name keeps its
 * measured width and two columns can never overlap.
 * ------------------------------------------------------------ */
typedef struct {
    Font *fp;          /* title + section labels: medium 12 */
    Font *fr;          /* list rows:              regular 11 */
    Font *fd;          /* size/tag/hint:          regular 10 */

    int pad_x, gap, icon_gap, icon_sz, icon_x, radius;
    int row_xpad, row_ypad;

    int row_h, line_h, head_h, foot_h, band_h, band_pad, nlines, rows;
    int hint_h, rule;          /* rule: hairline thickness, ui_px(1) */
    int list_y, list_bot, band_top, err_y;

    /* the column grid */
    int name_x, name_w, desc_x, desc_w;
    int size_right, size_col, tag_right, tag_col;
    int show_size, show_tag, show_desc;

    /* text tops, from the face that draws each line */
    int title_dy, row_dy, small_row_dy, hint_dy, desc_dy, foot_dy, btn_text_dy;

    /* the Refresh button in the header: the icon->label gap and the
     * label's own offset are fields, so the painter never re-derives them */
    int btn_x, btn_y, btn_w, btn_h, btn_icon, btn_pad, btn_gap, btn_label_dx;

    /* this window's own mark, at the head of the title */
    int title_icon, title_x;
} Layout;

static int iround(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }
static int iceil(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* y for font_draw() that centres one line of `f` in a band `band` px
 * tall. The ink of a line is `ascent` above the baseline and `descent`
 * below it; stbtt's ScaleForPixelHeight makes those sum to the nominal
 * size, which is font_height()/1.4 (src/font.c draws in a 1.4*size
 * line box). So centring the ink box is (band - font_height/1.4)/2, and
 * font_draw()'s baseline then lands font_ascent() below that top:
 * caps and descenders sit equally inside the row. The other apps use
 * the same rule, so "centred" means the same thing everywhere. */
static int line_top(Font *f, int band) {
    int ink;
    if (!f || band <= 0) return 0;
    ink = iround(font_height(f) / 1.4f);
    if (ink >= band) return 0;
    return (band - ink) / 2;
}

/* The description band gives up lines before it gives up rows: it shows
 * as many of DESC_MAXLINES as still leave two whole rows (or all it can
 * on a window too short for that). The hint line always has its slot -
 * it is what tells the owner that Enter writes a request. */
static int layout_store(Layout *L, const Store *st, int w, int h) {
    memset(L, 0, sizeof(*L));
    if (w < 1 || h < 1) return 0;

    L->fp = font_get(FONT_MEDIUM, 12);
    L->fr = font_get(FONT_REGULAR, 11);
    L->fd = font_get(FONT_REGULAR, 10);
    if (!L->fp || !L->fr || !L->fd) return 0;

    int fh_p = iround(font_height(L->fp));
    int fh_r = iround(font_height(L->fr));
    int fh_d = iround(font_height(L->fd));

    L->pad_x     = ui_px(10);
    L->gap       = ui_px(8);
    L->icon_gap  = ui_px(6);
    L->radius    = ui_px(5);
    L->row_xpad  = L->pad_x / 2;
    L->row_ypad  = ui_px(1);
    L->rule      = ui_px(1);
    L->band_pad  = ui_px(4);
    L->btn_pad   = ui_px(6);
    L->btn_icon  = ui_px(12);

    L->row_h  = fh_r + 2 * ui_px(4);
    L->line_h = fh_r + ui_px(5);
    L->hint_h = fh_d + 2 * ui_px(3);
    L->head_h = fh_p + 2 * ui_px(6);
    L->foot_h = fh_r + 2 * ui_px(5);

    L->title_dy     = line_top(L->fp, L->head_h);
    L->row_dy       = line_top(L->fr, L->row_h);
    L->small_row_dy = line_top(L->fd, L->row_h);
    L->hint_dy      = line_top(L->fd, L->hint_h);
    L->desc_dy      = line_top(L->fr, L->line_h);
    L->foot_dy      = line_top(L->fr, L->foot_h);

    /* the icon is a square that fits the row with air above and below */
    L->icon_sz = ui_px(16);
    if (L->icon_sz > L->row_h - 2 * ui_px(3)) L->icon_sz = L->row_h - 2 * ui_px(3);
    if (L->icon_sz < ui_px(10)) L->icon_sz = ui_px(10);
    L->icon_x = L->pad_x;
    L->name_x = L->icon_x + L->icon_sz + L->icon_gap;

    /* Refresh: icon + label, both inside a box one small-font line tall
     * plus padding, centred in the header band and right-aligned on the
     * same gutter as everything else. */
    L->btn_gap = ui_px(5);
    L->btn_label_dx = L->btn_pad + L->btn_icon + L->btn_gap;
    L->btn_w = L->btn_label_dx + iceil(font_width(L->fd, "Refresh")) + L->btn_pad;
    L->btn_h = fh_d + 2 * ui_px(4);
    if (L->btn_h > L->head_h - 2 * ui_px(2)) L->btn_h = L->head_h - 2 * ui_px(2);
    if (L->btn_h < ui_px(12)) L->btn_h = ui_px(12);
    L->btn_y = (L->head_h - L->btn_h) / 2;
    L->btn_text_dy = line_top(L->fd, L->btn_h);

    L->title_icon = L->icon_sz;
    L->title_x = L->pad_x + L->title_icon + L->icon_gap;
    L->btn_x = w - L->pad_x - L->btn_w;
    /* the title and the button must not collide; the button gives way */
    if (L->btn_x < L->title_x + iceil(font_width(L->fp, "Apps")) + L->gap)
        L->btn_w = 0;

    /* ---- the column grid ---------------------------------- */
    int content_right = w - L->pad_x;
    int nmax = 0;
    for (int i = 0; i < st->ninst; i++) {
        int nw = iceil(font_width(L->fr, st->inst[i].name));
        if (nw > nmax) nmax = nw;
    }
    for (int i = 0; i < st->navail; i++) {
        int nw = iceil(font_width(L->fr, st->avail[i].name));
        if (nw > nmax) nmax = nw;
    }
    if (nmax < ui_px(44)) nmax = ui_px(44);

    L->tag_col  = iceil(font_width(L->fd, "installed"));
    L->size_col = iceil(font_width(L->fd, "0000.0 MB"));
    int desc_min = ui_px(70);

    L->show_tag = L->show_size = L->show_desc = 1;
    int right = content_right;
    L->tag_right = right;
    right -= L->tag_col + L->gap;
    L->size_right = right;
    right -= L->size_col + L->gap;

    L->desc_x = L->name_x + nmax + L->gap;
    L->desc_w = right - L->desc_x;

    if (L->desc_w < desc_min) {                 /* drop the tag column */
        L->show_tag = 0;
        right = content_right;
        L->size_right = right;
        right -= L->size_col + L->gap;
        L->desc_w = right - L->desc_x;
    }
    if (L->desc_w < desc_min) {                 /* then the size column */
        L->show_size = 0;
        right = content_right;
        L->desc_w = right - L->desc_x;
    }
    if (L->desc_w < desc_min) {                 /* names only */
        L->show_desc = 0;
        L->desc_w = 0;
        int cap = (right > L->name_x) ? right - L->gap - L->name_x : 0;
        if (cap > 0 && nmax > cap) nmax = cap;
    }
    if (nmax < ui_px(24)) nmax = ui_px(24);
    L->name_w = nmax;

    /* ---- vertical bands ----------------------------------- */
    int err_line = st->error[0] ? L->row_h : 0;
    int usable = h - L->head_h - L->foot_h - err_line;
    int hint_slot = L->hint_h;

    L->nlines = 0;
    L->band_h = 0;
    for (int nl = DESC_MAXLINES; nl >= 0; nl--) {
        /* a band with no description is just the hint: it may sit closer
         * to the rule than a band of text does */
        int pad = nl > 0 ? ui_px(4) : ui_px(2);
        int band = nl * L->line_h + hint_slot + 2 * pad + L->rule;
        int rows = (usable - band) / L->row_h;
        if (rows >= 2 || nl == 0) {
            L->nlines = nl;
            L->band_h = band;
            L->band_pad = pad;
            break;
        }
    }
    if (L->band_h > usable - L->row_h) {        /* no room for a band at all */
        L->band_h = 0;
        L->nlines = 0;
    }
    /* A one-row window is worth less than the band: give the band up rather
     * than leave the list showing a single section title. Below two rows
     * there is nothing left to give, so the band comes back. */
    if (L->band_h > 0 && (usable - L->band_h) / L->row_h < 2) {
        L->band_h = 0;
        L->nlines = 0;
    }

    L->err_y    = L->head_h;
    L->list_y   = L->head_h + err_line;
    L->band_top = h - L->foot_h - L->band_h;
    L->list_bot = L->band_top;
    if (L->list_bot < L->list_y) L->list_bot = L->list_y;
    L->rows = (L->list_bot - L->list_y) / L->row_h;
    if (L->rows < 0) L->rows = 0;
    return 1;
}

/* ------------------------------------------------------------
 * Text helpers
 * ------------------------------------------------------------ */
static void draw_clipped(Surface *s, Font *f, float x, float y, float maxw,
                         const char *text, u32 color) {
    if (!text || !text[0] || maxw < 4.0f) return;

    char buf[320];
    size_t len = strlen(text);
    if (len >= sizeof(buf) - 4) len = sizeof(buf) - 4;
    memcpy(buf, text, len);
    buf[len] = 0;

    if (font_width(f, buf) <= maxw) {
        font_draw(s, f, x, y, buf, color);
        return;
    }

    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0) {
        buf[cut] = 0;
        if (font_width(f, buf) <= budget) break;
        cut--;
    }
    if (cut == 0) return;
    buf[cut] = '.';
    buf[cut + 1] = '.';
    buf[cut + 2] = 0;
    font_draw(s, f, x, y, buf, color);
}

/* Greedy word wrap: fills `out` with at most maxlines lines. The final
 * line gets the whole remainder and is clipped at draw time, so long
 * descriptions end in ".." instead of vanishing. */
static int wrap_desc(Font *fr, const char *text, float maxw,
                     char out[][DESC_LINE], int maxlines) {
    const char *wstart[DESC_WORDS];
    int         wlen[DESC_WORDS];
    int         nw = 0;

    const char *p = text;
    while (*p && nw < DESC_WORDS) {
        while (*p == ' ') p++;
        if (!*p) break;
        wstart[nw] = p;
        while (*p && *p != ' ') p++;
        wlen[nw] = (int)(p - wstart[nw]);
        nw++;
    }

    int nlines = 0;
    int i = 0;
    while (i < nw && nlines < maxlines) {
        if (nlines == maxlines - 1) {          /* last line: the remainder */
            int len = 0;
            out[nlines][0] = 0;
            while (i < nw && len + 1 < DESC_LINE) {
                if (len && len + 1 < DESC_LINE) out[nlines][len++] = ' ';
                for (int k = 0; k < wlen[i] && len + 1 < DESC_LINE; k++)
                    out[nlines][len++] = wstart[i][k];
                i++;
            }
            out[nlines][len] = 0;
            nlines++;
            break;
        }

        int len = 0;
        out[nlines][0] = 0;
        while (i < nw) {
            char cand[DESC_LINE];
            int cl = 0;
            if (len) { memcpy(cand, out[nlines], (size_t)len); cl = len; cand[cl++] = ' '; }

            int take = wlen[i];
            if (cl + take >= DESC_LINE) take = (int)sizeof(cand) - cl - 1;
            if (take < 0) take = 0;
            memcpy(cand + cl, wstart[i], (size_t)take);
            cl += take;
            cand[cl] = 0;

            if (font_width(fr, cand) > maxw) {
                if (len > 0) break;            /* word goes to the next line */
                /* A single word wider than the whole line: hard cut it
                 * and keep the rest of that word for the next line. */
                int keep = 0;
                char test[DESC_LINE];
                while (keep < take) {
                    int t2 = keep + 1;
                    memcpy(test, wstart[i], (size_t)t2);
                    test[t2] = 0;
                    if (font_width(fr, test) > maxw) break;
                    keep = t2;
                }
                if (keep == 0) keep = 1;
                memcpy(out[nlines], wstart[i], (size_t)keep);
                out[nlines][keep] = 0;
                wstart[i] += keep;
                wlen[i] -= keep;
                len = keep;
                break;
            }

            memcpy(out[nlines], cand, (size_t)cl + 1);
            len = cl;
            i++;
        }
        nlines++;
    }
    return nlines;
}

/* The cache is keyed on the selected row's name, not on its row number, and
 * every list rebuild clears it - so a rescan can never leave another
 * package's text in the band, whether rows appeared, vanished, or a desc
 * column was edited. `maxw` is the real text width the band offers. */
static void refresh_wrap(Store *s, Font *fr, int maxw) {
    char key[64];
    key[0] = 0;
    const char *text = "";

    if (s->sel >= 0 && s->sel < s->ndisp) {
        Disp *d = &s->disp[s->sel];
        if (d->kind == D_APP) {
            snprintf(key, sizeof(key), "a:%s", s->inst[d->idx].id);
            text = s->inst[d->idx].desc;
        } else if (d->kind == D_PKG) {
            snprintf(key, sizeof(key), "p:%s", s->avail[d->idx].name);
            text = s->avail[d->idx].desc;
        }
    }

    if (s->wrap_w == maxw && !strcmp(s->wrap_id, key)) return;
    snprintf(s->wrap_id, sizeof(s->wrap_id), "%s", key);
    s->wrap_w = maxw;
    s->dn = 0;
    if (!key[0]) return;
    s->dn = wrap_desc(fr, text, (float)maxw, s->dlines, DESC_MAXLINES);
}

/* ------------------------------------------------------------
 * Requests. The desktop owns the fork/exec and the package
 * manager; this window only writes the one-line request file it
 * watches. Nothing else is ever written to those paths.
 * ------------------------------------------------------------ */
static int write_line(const char *path, const char *value, char *err, size_t errsz) {
    /* "wb": the bytes handed to the desktop must be exactly value + '\n',
     * on any host, not whatever the C library's text mode does to them */
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return 0;
    }

    int e = 0;
    if (fprintf(f, "%s\n", value) < 0) e = errno;
    if (fflush(f) != 0 && !e) e = errno;
    if (ferror(f) && !e) e = EIO;
    if (fclose(f) != 0 && !e) e = errno;
    if (e) {
        /* never leave half a request behind for the desktop to read */
        remove(path);
        snprintf(err, errsz, "%s: %s", path, strerror(e));
        return 0;
    }
    return 1;
}

static void request_refresh(Store *s) {
    char err[128];
    int ok = write_line(STORE_REPO_REFRESH, "refresh", err, sizeof(err));

    /* a refresh is also the moment to look at the disk again: pkg writes
     * into a bundle root, so a finished install shows up here too */
    scan_installed(s);

    if (!ok) {
        snprintf(s->status, sizeof(s->status), "%s", err);
        return;
    }
    snprintf(s->status, sizeof(s->status),
             "refresh requested - the desktop fetches the index");
}

static void activate(Store *s) {
    if (s->sel < 0 || s->sel >= s->ndisp) {
        snprintf(s->status, sizeof(s->status), "nothing selected");
        return;
    }
    Disp *d = &s->disp[s->sel];

    if (d->kind == D_APP) {
        StoreApp *a = &s->inst[d->idx];
        /* Both roots are launchable: wms_scan_apps() reads the image root
         * AND /root/apps, which is where pkg unpacks. (It used to read only
         * the first, so a package installed here could not be started.) */
        char err[128];
        if (!write_line(STORE_LAUNCH_REQUEST, a->id, err, sizeof(err))) {
            snprintf(s->status, sizeof(s->status), "%s", err);
            return;
        }
        snprintf(s->status, sizeof(s->status),
                 "asked the desktop to launch %s", a->id);
        return;
    }

    if (d->kind == D_PKG) {
        Pkg *k = &s->avail[d->idx];
        if (is_installed(s, k->name)) {
            snprintf(s->status, sizeof(s->status), "already installed: %s", k->name);
            return;
        }
        /* One install at a time. Enter repeats (input.c pushes autorepeat),
         * so without this, holding the key forks a pkg per repeat and they
         * all write the same download file. */
        if (s->pending[0]) {
            snprintf(s->status, sizeof(s->status),
                     "already installing %s - wait for it", s->pending);
            return;
        }
        if (!pkg_name_ok(k->name)) {      /* belt and braces: load_index filtered */
            snprintf(s->status, sizeof(s->status), "refusing package name");
            return;
        }
        char err[128];
        if (!write_line(STORE_INSTALL_REQUEST, k->name, err, sizeof(err))) {
            snprintf(s->status, sizeof(s->status), "%s", err);
            return;
        }
        snprintf(s->status, sizeof(s->status), "installing %s...", k->name);
        snprintf(s->pending, sizeof(s->pending), "%s", k->name);
        /* start the wait on the monotonic clock; poll_install() drops the
         * claim again if no bundle appears within the timeout */
        struct timespec ts;
        s->pending_since = 0.0;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
            s->pending_since = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        return;
    }

    /* section titles and notes are not rows you can act on */
}

/* ------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 *  The two row marks. Both are geometry - the same rounded-rect and
 *  line primitives the launcher's icons are made of - so they need no
 *  asset and scale with the row. Theme colours only.
 *
 *  parcel: a lid, a body and the tape down the middle. It means "this
 *          is something you can install". `dim` fades the whole mark.
 *  tick:   a disc with a tick. It means "this is already on the disk".
 * ------------------------------------------------------------ */
static void draw_pkg_icon(Surface *s, float x, float y, float sz,
                          u32 accent, float dim) {
    u32 edge = color_alpha(accent, 0.90f * dim);
    u32 body = color_alpha(accent, 0.30f * dim);
    u32 lid  = color_alpha(accent, 0.78f * dim);
    u32 tape = color_alpha(accent, 0.85f * dim);

    float w = sz * 0.92f;
    float bx = x + (sz - w) * 0.5f;
    float top = y + sz * 0.12f;
    float lid_h = sz * 0.20f;
    float body_top = top + lid_h + sz * 0.02f;
    float body_h = sz * 0.60f;

    /* the body, inset from the lid */
    gfx_rrect_aa(s, bx + sz * 0.06f, body_top, w - sz * 0.12f, body_h,
                 sz * 0.07f, body, edge, 1.0f);
    /* the lid, a solid band a little wider than the body */
    gfx_rrect(s, bx, top, w, lid_h, sz * 0.05f, lid);
    /* the tape, wrapped over the lid and down the body */
    gfx_rrect(s, x + sz * 0.44f, top, sz * 0.12f,
              lid_h + sz * 0.02f + body_h, sz * 0.03f, tape);
}

static void draw_check_icon(Surface *s, float x, float y, float sz,
                            u32 accent, float dim) {
    u32 ring = color_alpha(accent, 0.70f * dim);
    u32 fill = color_alpha(accent, 0.14f * dim);
    u32 tick = color_alpha(accent, 0.95f * dim);
    float d = sz * 0.84f;
    float ox = x + (sz - d) * 0.5f, oy = y + (sz - d) * 0.5f;

    gfx_rrect_aa(s, ox, oy, d, d, d * 0.5f, fill, ring, 1.0f);
    gfx_line(s, ox + d * 0.24f, oy + d * 0.52f,
                ox + d * 0.43f, oy + d * 0.71f, sz * 0.11f, tick);
    gfx_line(s, ox + d * 0.43f, oy + d * 0.71f,
                ox + d * 0.78f, oy + d * 0.30f, sz * 0.11f, tick);
}

/* the Refresh mark, the same circular arrow Files' toolbar uses */
static void draw_refresh_icon(Surface *s, float x, float y, float sz, u32 c) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.30f;
    gfx_rrect_aa(s, cx - r, cy - r, r * 2.0f, r * 2.0f, r, 0, c, sz * 0.12f);
    float head[6] = { cx + r * 0.10f, cy - r * 1.35f,
                      cx + r * 1.35f, cy - r * 0.50f,
                      cx + r * 0.10f, cy - r * 0.18f };
    gfx_polygon(s, head, 3, c);
}

/* One painted line. `x` is the window's origin (the painter is the only
 * side that adds it); `ry` is the row's top in the compositor's surface.
 * Every number comes from the Layout the hit-test also uses, so the row a
 * click lands on is this row - and each text line is centred in the row
 * with the face's own ascent (line_top()), not with a +1 or +2 fudge. */
static void paint_row(Surface *s, const Layout *L, const Theme *th,
                      const Store *st, int x, int w, int ry, int idx) {
    const Disp *d = &st->disp[idx];
    int selected = (idx == st->sel);
    float fx = (float)x;
    int iy = ry + (L->row_h - L->icon_sz) / 2;

    if (selected && disp_selectable(d))
        gfx_rrect_aa(s, fx + (float)L->row_xpad, (float)ry + (float)L->row_ypad,
                     (float)(w - 2 * L->row_xpad),
                     (float)(L->row_h - 2 * L->row_ypad), (float)L->radius,
                     color_alpha(th->accent, 0.20f), 0, 0);

    if (d->kind == D_SECT) {
        /* The title sits on the name column and carries the same mark its
         * rows do, so the list explains its own icon vocabulary. */
        if (d->mark == MARK_CHECK)
            draw_check_icon(s, fx + (float)L->icon_x, (float)iy,
                            (float)L->icon_sz, th->accent, 0.85f);
        else
            draw_pkg_icon(s, fx + (float)L->icon_x, (float)iy,
                          (float)L->icon_sz, th->accent, 0.85f);

        char label[64];
        snprintf(label, sizeof(label), "%s (%d)", d->msg, d->idx);
        float lx = fx + (float)L->name_x;
        float lw = font_width(L->fd, label);
        font_draw(s, L->fd, lx, (float)ry + (float)L->small_row_dy, label,
                  color_alpha(th->accent, 0.75f));

        float rx = lx + lw + (float)L->gap;
        float re = fx + (float)w - (float)L->pad_x;
        if (re > rx)
            surf_blend(s, (int)rx, ry + L->row_h / 2, (int)(re - rx), 1,
                       th->panel_border);
        return;
    }

    if (d->kind == D_MSG) {
        draw_clipped(s, L->fd, fx + (float)L->name_x,
                     (float)ry + (float)L->small_row_dy,
                     (float)w - (float)L->pad_x - (float)L->name_x, d->msg,
                     color_alpha(th->text, 0.45f));
        return;
    }

    float name_y  = (float)ry + (float)L->row_dy;
    float small_y = (float)ry + (float)L->small_row_dy;
    float name_x  = fx + (float)L->name_x;
    float name_w  = (float)L->name_w;

    if (d->kind == D_APP) {
        const StoreApp *e = &st->inst[d->idx];

        /* an installed bundle is a tick; a bundle whose payload we cannot
         * stat is dimmed: it is listed, but it is not launchable */
        draw_check_icon(s, fx + (float)L->icon_x, (float)iy,
                        (float)L->icon_sz, th->accent,
                        e->have_size ? 0.90f : 0.55f);

        char sz[32];
        if (e->have_size)
            snprintf(sz, sizeof(sz), "%.1f KB", (double)e->size / 1024.0);
        else
            snprintf(sz, sizeof(sz), "no payload");
        if (L->show_size) {
            float sw = font_width(L->fd, sz);
            font_draw(s, L->fd, fx + (float)L->size_right - sw, small_y, sz,
                      color_alpha(th->text, e->have_size ? 0.55f : 0.35f));
        }

        if (L->show_tag && e->from_root) {         /* installed by pkg */
            const char *tag = "pkg";
            float tw = font_width(L->fd, tag);
            font_draw(s, L->fd, fx + (float)L->tag_right - tw, small_y, tag,
                      color_alpha(th->accent, 0.55f));
        }

        if (!L->show_desc)
            name_w = fx + (float)w - (float)L->pad_x - name_x;
        draw_clipped(s, L->fr, name_x, name_y, name_w, e->name,
                     selected ? th->text : th->text_dim);
        return;
    }

    /* D_PKG: what the index offers */
    const Pkg *k = &st->avail[d->idx];
    int ins = is_installed(st, k->name);

    if (ins)
        draw_check_icon(s, fx + (float)L->icon_x, (float)iy,
                        (float)L->icon_sz, th->accent, 0.60f);
    else
        draw_pkg_icon(s, fx + (float)L->icon_x, (float)iy,
                      (float)L->icon_sz, th->accent, selected ? 1.0f : 0.85f);

    char sz[32];
    if (!k->have_size)
        snprintf(sz, sizeof(sz), "?");
    else if (k->size >= 1048576)
        snprintf(sz, sizeof(sz), "%.1f MB", (double)k->size / 1048576.0);
    else
        snprintf(sz, sizeof(sz), "%ld KB", (k->size + 1023) / 1024);
    if (L->show_size) {
        float sw = font_width(L->fd, sz);
        font_draw(s, L->fd, fx + (float)L->size_right - sw, small_y, sz,
                  color_alpha(th->text, k->have_size ? 0.55f : 0.35f));
    }

    if (L->show_tag && ins) {
        const char *tag = "installed";
        float tw = font_width(L->fd, tag);
        font_draw(s, L->fd, fx + (float)L->tag_right - tw, small_y, tag,
                  color_alpha(th->accent, 0.70f));
    }

    if (L->show_desc)
        draw_clipped(s, L->fd, fx + (float)L->desc_x, small_y,
                     (float)L->desc_w, k->desc,
                     color_alpha(th->text, ins ? 0.30f : 0.50f));
    else
        name_w = fx + (float)w - (float)L->pad_x - name_x;

    draw_clipped(s, L->fr, name_x, name_y, name_w, k->name,
                 ins ? color_alpha(th->text, 0.55f)
                     : (selected ? th->text : th->text_dim));
}

static void store_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Store *st = a->state;
    if (!st || !th) return;
    st->w = w;
    st->h = h;
    if (w < 40 || h < 40) return;

    /* bring the two sources up to date before laying them out */
    poll_install(st);
    sync_index(st);

    Layout L;
    if (!layout_store(&L, st, w, h)) return;

    /* ---- header: this window's own mark, its name, the Refresh action */
    surf_blend(s, x, y, w, L.head_h, th->panel);
    draw_pkg_icon(s, (float)(x + L.icon_x),
                  (float)y + (float)((L.head_h - L.title_icon) / 2),
                  (float)L.title_icon, th->accent, 1.0f);
    font_draw(s, L.fp, (float)(x + L.title_x), (float)y + (float)L.title_dy,
              "Apps", th->text);

    if (L.btn_w > 0) {
        float bx = (float)(x + L.btn_x), by = (float)(y + L.btn_y);
        gfx_rrect_aa(s, bx, by, (float)L.btn_w, (float)L.btn_h, (float)L.radius,
                     color_alpha(th->accent, 0.18f),
                     color_alpha(th->accent, 0.55f), 1.0f);
        draw_refresh_icon(s, bx + (float)L.btn_pad,
                          by + ((float)L.btn_h - (float)L.btn_icon) * 0.5f,
                          (float)L.btn_icon, color_alpha(th->text, 0.90f));
        font_draw(s, L.fd, bx + (float)L.btn_label_dx,
                  by + (float)L.btn_text_dy, "Refresh", th->text);
    }
    surf_blend(s, x + L.pad_x, y + L.head_h - L.rule, w - 2 * L.pad_x,
               L.rule, th->panel_border);

    /* ---- the error line, when the bundle root could not be read */
    if (st->error[0])
        draw_clipped(s, L.fr, (float)(x + L.pad_x),
                     (float)(y + L.err_y + L.row_dy),
                     (float)(w - 2 * L.pad_x), st->error, th->accent);

    /* ---- rows; the selection is kept inside the scroll window */
    if (L.rows > 0) {
        if (st->sel >= 0) {
            if (st->sel < st->scroll) st->scroll = st->sel;
            if (st->sel >= st->scroll + L.rows) st->scroll = st->sel - L.rows + 1;
        }
        int max_scroll = st->ndisp - L.rows;
        if (max_scroll < 0) max_scroll = 0;
        if (st->scroll > max_scroll) st->scroll = max_scroll;
        if (st->scroll < 0) st->scroll = 0;

        for (int i = 0; i < L.rows; i++) {
            int idx = st->scroll + i;
            if (idx >= st->ndisp) break;
            paint_row(s, &L, th, st, x, w, y + L.list_y + i * L.row_h, idx);
        }
    }

    /* ---- description band: the selected row's own text, wrapped */
    if (L.band_h > 0) {
        int by = y + L.band_top;
        int text_w = w - 2 * L.pad_x;
        surf_blend(s, x + L.pad_x, by, text_w, L.rule, th->panel_border);
        refresh_wrap(st, L.fr, text_w);

        int content_y = by + L.rule + L.band_pad;
        int sel = st->sel;

        if (sel >= 0 && sel < st->ndisp && disp_selectable(&st->disp[sel])) {
            int shown = st->dn;
            if (shown > L.nlines) shown = L.nlines;
            /* a message takes the first description line's slot - and only
             * exists when the band has room for a line at all (a hint-only
             * band at the smallest size has not) */
            if (shown == 0 && L.nlines > 0)
                draw_clipped(s, L.fr, (float)(x + L.pad_x),
                             (float)(content_y + L.desc_dy),
                             (float)text_w,
                             st->disp[sel].kind == D_PKG
                                 ? "no description in the index"
                                 : "this manifest has no desc = line",
                             th->text_dim);
            for (int i = 0; i < shown; i++)
                draw_clipped(s, L.fr, (float)(x + L.pad_x),
                             (float)(content_y + i * L.line_h + L.desc_dy),
                             (float)text_w, st->dlines[i],
                             color_alpha(th->text, 0.85f));

            /* the hint: what Enter will ask the desktop to do */
            char hint[128];
            int hy = content_y + L.nlines * L.line_h + L.hint_dy;
            if (st->disp[sel].kind == D_PKG) {
                const Pkg *k = &st->avail[st->disp[sel].idx];
                if (is_installed(st, k->name))
                    snprintf(hint, sizeof(hint), "%s is installed - Enter does nothing",
                             k->name);
                else if (k->version[0])
                    snprintf(hint, sizeof(hint), "Enter: pkg install %s (v%s)",
                             k->name, k->version);
                else
                    snprintf(hint, sizeof(hint), "Enter: pkg install %s", k->name);
            } else {
                const StoreApp *e = &st->inst[st->disp[sel].idx];
                if (e->from_root)
                    snprintf(hint, sizeof(hint),
                             "%s is in %s - the launcher reads %s",
                             e->id, STORE_ROOT_APPS, STORE_APP_DIR);
                else
                    snprintf(hint, sizeof(hint),
                             "Enter: ask the desktop to launch %s", e->id);
            }
            draw_clipped(s, L.fd, (float)(x + L.pad_x), (float)hy,
                         (float)text_w, hint, color_alpha(th->text, 0.40f));
        } else {
            int hy = content_y + L.hint_dy;
            draw_clipped(s, L.fd, (float)(x + L.pad_x), (float)hy,
                         (float)text_w, "r: fetch the package index",
                         color_alpha(th->text, 0.40f));
        }
    }

    /* ---- footer: how much is installed and offered, plus the outcome.
     * A status that does not fit beside the counts gets the whole line - a
     * failure message that is never painted would be worse than a lost
     * count. */
    {
        int fy = y + h - L.foot_h;
        surf_blend(s, x, fy, w, L.foot_h, th->panel);
        surf_blend(s, x + L.pad_x, fy, w - 2 * L.pad_x, L.rule,
                   th->panel_border);

        float ty = (float)fy + (float)L.foot_dy;
        char foot[96];
        snprintf(foot, sizeof(foot), "%d installed - %d available%s",
                 st->ninst, st->navail,
                 st->index_state == 0 ? " - no index" : "");

        float tw = st->status[0] ? font_width(L.fr, st->status) : 0.0f;
        if (st->status[0] &&
            tw + font_width(L.fr, foot) + (float)(2 * L.gap) < (float)w) {
            font_draw(s, L.fr, (float)(x + w - L.pad_x) - tw, ty, st->status,
                      color_alpha(th->accent, 0.55f));
            draw_clipped(s, L.fr, (float)(x + L.pad_x), ty,
                         (float)w - tw - (float)(L.pad_x + L.gap), foot,
                         color_alpha(th->text, 0.45f));
        } else if (st->status[0]) {
            draw_clipped(s, L.fr, (float)(x + L.pad_x), ty,
                         (float)(w - 2 * L.pad_x), st->status,
                         color_alpha(th->accent, 0.55f));
        } else {
            draw_clipped(s, L.fr, (float)(x + L.pad_x), ty,
                         (float)(w - 2 * L.pad_x), foot,
                         color_alpha(th->text, 0.45f));
        }
    }
}

/* ------------------------------------------------------------
 * Input
 * ------------------------------------------------------------ */
static void move_sel(Store *s, int delta) {
    if (s->ndisp <= 0 || delta == 0) return;
    int dir = delta > 0 ? 1 : -1;
    int steps = delta > 0 ? delta : -delta;
    int i = s->sel;
    if (i < 0) i = dir > 0 ? -1 : s->ndisp;

    while (steps > 0) {
        int j = i + dir;
        while (j >= 0 && j < s->ndisp && !disp_selectable(&s->disp[j])) j += dir;
        if (j < 0 || j >= s->ndisp) break;
        i = j;
        steps--;
    }
    if (i >= 0 && i < s->ndisp) s->sel = i;
}

static void sel_first(Store *s) {
    for (int i = 0; i < s->ndisp; i++)
        if (disp_selectable(&s->disp[i])) { s->sel = i; return; }
}

static void sel_last(Store *s) {
    for (int i = s->ndisp - 1; i >= 0; i--)
        if (disp_selectable(&s->disp[i])) { s->sel = i; return; }
}

static void store_key(App *a, const char *bytes, int n) {
    Store *s = a->state;
    if (!s) return;

    Layout L;
    int rows = 1;
    if (layout_store(&L, s, s->w, s->h) && L.rows > 0) rows = L.rows;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == 0x1B) {                       /* arrow / Home / End / Page runs */
            if (i + 2 < n && bytes[i + 1] == '[') {
                switch (bytes[i + 2]) {
                    case 'A': move_sel(s, -1); break;
                    case 'B': move_sel(s, +1); break;
                    case 'H': sel_first(s); break;
                    case 'F': sel_last(s); break;
                    case '5':                            /* \x1b[5~ PageUp */
                        move_sel(s, -rows);
                        if (i + 3 < n) i++;
                        break;
                    case '6':                            /* \x1b[6~ PageDown */
                        move_sel(s, +rows);
                        if (i + 3 < n) i++;
                        break;
                    default: break;
                }
                i += 2;
            }
            continue;
        }

        switch (c) {
            case 'j': move_sel(s, +1); break;
            case 'k': move_sel(s, -1); break;
            case 'g': sel_first(s); break;
            case 'G': sel_last(s); break;
            case 0x01: sel_first(s); break;                         /* Ctrl+A */
            case 0x05: sel_last(s); break;                          /* Ctrl+E */
            case 'r': request_refresh(s); break;
            case '\n':
            case '\r': activate(s); break;
            default: break;
        }
    }
}

/* The window's wheel hook (AppVT.wheel). Positive notches scroll up, as
 * the desktop delivers them. The view moves under the pointer by whole
 * rows, and the selection is pulled to the nearest selectable row when
 * the view passes it, so it can never be scrolled out of sight. */
static void store_wheel(App *a, int notches) {
    Store *s = a->state;
    if (!s || notches == 0) return;

    Layout L;
    if (!layout_store(&L, s, s->w, s->h) || L.rows <= 0) return;

    int maxs = s->ndisp - L.rows;
    if (maxs < 0) maxs = 0;
    int old = s->scroll;
    int want = s->scroll - notches * WHEEL_ROWS;
    if (want < 0) want = 0;
    if (want > maxs) want = maxs;
    if (want == s->scroll) return;

    s->scroll = want;

    /* The view may only come to rest somewhere a row can actually be
     * selected: when the window would hold nothing but a section title (a
     * one-row window at the smallest size), the move is refused instead of
     * parking the selection off-screen. */
    int lo = s->scroll;
    int hi = s->scroll + L.rows - 1;
    if (hi >= s->ndisp) hi = s->ndisp - 1;
    int any = 0;
    for (int i = lo; i <= hi && !any; i++)
        if (disp_selectable(&s->disp[i])) any = 1;
    if (!any) { s->scroll = old; return; }

    /* Keep the selection exactly where it is while it is still in view.
     * When the view has passed it, take the nearest selectable row at the
     * edge the view came towards - the top for a downward scroll, the
     * bottom for an upward one - so one notch never throws the highlight
     * across the whole window. */
    if (s->sel < lo) {
        for (int i = lo; i <= hi; i++)
            if (disp_selectable(&s->disp[i])) { s->sel = i; break; }
    } else if (s->sel > hi) {
        for (int i = hi; i >= lo; i--)
            if (disp_selectable(&s->disp[i])) { s->sel = i; break; }
    }
}

static void store_click(App *a, int x, int y, int buttons) {
    Store *s = a->state;
    if (!s) return;
    if (buttons && !(buttons & 1)) return;      /* left button only */

    Layout L;
    if (!layout_store(&L, s, s->w, s->h)) return;

    /* The Refresh button: its rectangle and the pointer are both app-local,
     * so the draw origin must not appear in this comparison. */
    if (L.btn_w > 0 && x >= L.btn_x && x < L.btn_x + L.btn_w &&
        y >= L.btn_y && y < L.btn_y + L.btn_h) {
        request_refresh(s);
        return;
    }

    if (L.rows < 1) return;

    /* Only the row band owns rows. A click in the header, the error line,
     * the description band or the footer belongs to no row and must do
     * nothing. Reject those bands first, then divide a value known to be
     * >= 0, so truncation and floor agree. */
    if (y < L.list_y || y >= L.list_bot) return;

    int row = (y - L.list_y) / L.row_h;         /* y >= list_y: exact */
    if (row < 0 || row >= L.rows) return;
    int idx = s->scroll + row;
    if (idx < 0 || idx >= s->ndisp) return;
    if (!disp_selectable(&s->disp[idx])) return;  /* titles and notes */

    int again = (idx == s->sel);
    s->sel = idx;
    if (again) activate(s);                     /* click twice to act */
}

static void store_resize(App *a, int w, int h) {
    Store *s = a->state;
    if (!s) return;
    s->w = w;
    s->h = h;
    s->wrap_w = -1;              /* force a re-wrap for the new width */
}

static void store_init(App *a, int w, int h) {
    Store *s = calloc(1, sizeof(Store));
    if (!s) return;
    s->w = w;
    s->h = h;
    s->sel = -1;
    s->wrap_w = -1;
    s->index_size = -1;
    load_index(s);
    scan_installed(s);           /* ends with build_disp() */
    a->state = s;
}

static void store_free(App *a) {
    Store *s = a->state;
    if (!s) return;
    free(s);
    a->state = NULL;
}

const AppVT app_vt_store = {
    .id = "store",
    .title = "Apps",
    .init = store_init,
    .draw = store_draw,
    .key = store_key,
    .click = store_click,
    .resize = store_resize,
    .wheel = store_wheel,
    .free = store_free,
};
