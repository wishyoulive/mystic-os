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
 *  Every rectangle in here - btn_rect(), the row band - is app-local,
 *  so the painter adds the origin and the hit-test never does. That
 *  is the rule src/apps/files.c spells out; mixing the two only looks
 *  right while the window happens to sit at (0,0).
 *
 *  Keys:  j/k or Up/Down  move        Enter  launch / install
 *         Home/End/g/G    first/last  PgUp/PgDn  by a screen
 *         r               refresh the repo index
 *  Mouse: click the Refresh button, click a row to select it, click
 *         the selected row again to activate it.
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
 * seconds, so a minute and a half of silence means it did not happen) */
#ifndef STORE_INSTALL_TIMEOUT_SEC
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

typedef struct {
    int         kind;
    int         idx;    /* D_APP: inst[] index, D_PKG: avail[] index,
                           D_SECT: how many rows the section holds */
    const char *msg;    /* D_SECT label / D_MSG text (static or Store-owned) */
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
    if (s->ninst == 0)
        disp_push(s, D_MSG, 0, "no app bundles installed");
    else
        for (int i = 0; i < s->ninst; i++)
            disp_push(s, D_APP, i, NULL);

    disp_push(s, D_SECT, s->navail, "Available");
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
 * Geometry. draw(), key() and click() must agree on where the rows
 * are, so they all derive the vertical metrics from the fonts and
 * share one layout function.
 *
 * There is no ROW_H/HEAD_H/FOOT_H/LINE_H constant. Every vertical
 * size is the height of the font that draws in it plus a fixed
 * amount of leading, which is what "leading" means: the space
 * between baselines. font_get() is where the global text scale is
 * applied (src/font.c:125), so a scale change moves every one of
 * these numbers together with the glyphs: rows cannot overlap, the
 * bands cannot cut the text, and because click() runs the very same
 * geo_of()/layout_of() pair a click still lands on the row that was
 * painted. Horizontal paddings stay fixed on purpose.
 * ------------------------------------------------------------ */
typedef struct {
    int row_h;      /* one row                (font of the row + 4) */
    int head_h;     /* title band + its rule  (font of the title + 12) */
    int foot_h;     /* status line at the bottom (font of the line + 6) */
    int line_h;     /* one wrapped description line (font + 2) */
} Geo;

static Geo geo_of(Font *fp, Font *fr) {
    Geo g;
    int fph = (int)(font_height(fp) + 0.5f);
    int frh = (int)(font_height(fr) + 0.5f);
    g.row_h  = frh + 4;
    g.head_h = fph + 12;
    g.foot_h = frh + 6;
    g.line_h = frh + 2;
    return g;
}

/* The fonts the two bodies of text in this window use, exactly as
 * store_draw() asks for them, so every caller gets the same metrics. */
static int geo_now(Geo *g) {
    Font *fp = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    if (!fp || !fr) return 0;
    *g = geo_of(fp, fr);
    return 1;
}

/* The nl thresholds only decide how many description lines are shown at
 * all (3 on a tall window, 1 or none on a short one); they are not text
 * positions. The band and the rows above it are computed from the metrics. */
static void layout_of(int h, const Geo *g, int *rows, int *band, int *nlines) {
    int nl = h >= 210 ? DESC_MAXLINES : h >= 172 ? 2 : h >= 134 ? 1 : 0;
    for (;;) {
        int b = nl > 0 ? nl * g->line_h + g->line_h + 8
                       : (h >= 96 ? g->line_h + 8 : 0);
        int r = (h - g->head_h - g->foot_h - b) / g->row_h;
        if (r >= 1 || nl == 0) {
            *rows = r < 0 ? 0 : r;
            *band = b;
            *nlines = nl;
            return;
        }
        nl--;
    }
}

/* When the bundle directory cannot be read, its error line gets the first
 * row slot instead of replacing the list - the Available section stays
 * usable. The painter, the pager and the click hit-test shift by the same
 * derived amount, so a click still lands on the row that was painted. */
static int list_offset(const Store *s, const Geo *g) {
    return s->error[0] ? g->row_h : 0;
}

static int list_rows(const Store *s, int rows) {
    if (s->error[0] && rows > 0) rows--;
    return rows > 0 ? rows : 0;
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
 * column was edited. */
static void refresh_wrap(Store *s, Font *fr, int w) {
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

    if (s->wrap_w == w && !strcmp(s->wrap_id, key)) return;
    snprintf(s->wrap_id, sizeof(s->wrap_id), "%s", key);
    s->wrap_w = w;
    s->dn = 0;
    if (!key[0]) return;
    s->dn = wrap_desc(fr, text, (float)w - 12.0f, s->dlines, DESC_MAXLINES);
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

/* The Refresh button, APP-LOCAL. draw() adds the window origin to paint
 * it, store_click() compares the app-local pointer against it directly -
 * one rectangle, one owner, so the two can never drift apart (the
 * coordinate-space rule src/apps/files.c documents). Returns 0 when the
 * button does not fit: a button that is not painted is not clickable. */
static int btn_rect(const Geo *g, int w, int *bx, int *by, int *bw, int *bh) {
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fd || w < 120) return 0;

    int tw = (int)(font_width(fd, "Refresh") + 0.5f);
    int bwi = tw + 18;
    int bhi = g->head_h - 10;
    if (bhi < 12) bhi = 12;

    int x = w - bwi - 6;
    if (x < 48) return 0;                  /* no room right of the title */
    *bx = x;
    *by = 5;
    *bw = bwi;
    *bh = bhi;
    return 1;
}

static void paint_row(Surface *s, Font *fr, Font *fd, const Theme *th,
                      int x, int ry, int w, int row_h, Store *st, int idx) {
    Disp *d = &st->disp[idx];
    int selected = (idx == st->sel);
    float fx = (float)x;
    float fy = (float)ry;

    if (selected && disp_selectable(d))
        gfx_rrect_aa(s, fx + 3.0f, fy - 1.0f, (float)w - 6.0f, (float)row_h,
                     4.0f, color_alpha(th->accent, 0.20f), 0, 0);

    if (d->kind == D_SECT) {
        char label[64];
        snprintf(label, sizeof(label), "%s (%d)", d->msg, d->idx);
        float lw = font_width(fd, label);
        font_draw(s, fd, fx + 8.0f, fy + 2.0f, label, color_alpha(th->accent, 0.75f));
        float rx = fx + 8.0f + lw + 8.0f;
        float re = fx + (float)w - 8.0f;
        if (re > rx)
            surf_blend(s, (int)rx, ry + row_h / 2, (int)(re - rx), 1, th->panel_border);
        return;
    }

    if (d->kind == D_MSG) {
        draw_clipped(s, fd, fx + 14.0f, fy + 2.0f, (float)w - 22.0f, d->msg,
                     color_alpha(th->text, 0.45f));
        return;
    }

    float right = fx + (float)w - 6.0f;

    if (d->kind == D_APP) {
        StoreApp *e = &st->inst[d->idx];

        char sz[32];
        if (e->have_size)
            snprintf(sz, sizeof(sz), "%.1f KB", (double)e->size / 1024.0);
        else
            snprintf(sz, sizeof(sz), "no payload");
        float sw = font_width(fr, sz);
        if (sw + 90.0f < (float)w) {
            font_draw(s, fr, right - sw, fy, sz,
                      color_alpha(th->text, e->have_size ? 0.55f : 0.35f));
            right -= sw + 10.0f;
        }

        if (w >= 320) {
            float ew = font_width(fd, e->exe);
            if (right - ew > fx + 140.0f) {
                font_draw(s, fd, right - ew, fy + 1.0f, e->exe,
                          color_alpha(th->text, 0.35f));
                right -= ew + 10.0f;
            }
        }

        if (e->from_root) {     /* came from the repo, not the image */
            float mw = font_width(fd, "pkg");
            if (right - mw > fx + 140.0f) {
                font_draw(s, fd, right - mw, fy + 1.0f, "pkg",
                          color_alpha(th->accent, 0.55f));
                right -= mw + 10.0f;
            }
        }

        float name_x = fx + 8.0f;
        draw_clipped(s, fr, name_x, fy, right - name_x, e->name,
                     selected ? th->text : th->text_dim);
        return;
    }

    /* D_PKG: name, the index's size, and its description */
    Pkg *k = &st->avail[d->idx];
    int ins = is_installed(st, k->name);

    char sz[32];
    if (!k->have_size)
        snprintf(sz, sizeof(sz), "?");
    else if (k->size >= 1048576)
        snprintf(sz, sizeof(sz), "%.1f MB", (double)k->size / 1048576.0);
    else
        snprintf(sz, sizeof(sz), "%ld KB", (k->size + 1023) / 1024);
    {
        float sw = font_width(fd, sz);
        if (sw + 70.0f < (float)w) {
            font_draw(s, fd, right - sw, fy + 1.0f, sz,
                      color_alpha(th->text, k->have_size ? 0.55f : 0.35f));
            right -= sw + 10.0f;
        }
    }

    if (ins) {
        float iw = font_width(fd, "installed");
        if (right - iw > fx + 150.0f) {
            font_draw(s, fd, right - iw, fy + 1.0f, "installed",
                      color_alpha(th->accent, 0.70f));
            right -= iw + 10.0f;
        }
    }

    float name_x = fx + 8.0f;
    float name_w = (float)w * 0.34f;
    if (name_w > 170.0f) name_w = 170.0f;
    if (name_w < 64.0f) name_w = 64.0f;

    float desc_x = name_x + name_w + 6.0f;
    float desc_w = right - 8.0f - desc_x;
    if (desc_w > 20.0f)
        draw_clipped(s, fd, desc_x, fy + 1.0f, desc_w, k->desc,
                     color_alpha(th->text, ins ? 0.30f : 0.50f));

    draw_clipped(s, fr, name_x, fy, name_w, k->name,
                 ins ? color_alpha(th->text, 0.55f)
                     : (selected ? th->text : th->text_dim));
}

static void store_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Store *st = a->state;
    if (!st) return;
    st->w = w;
    st->h = h;
    if (w < 40 || h < 40) return;

    Font *fp = font_get(FONT_MEDIUM, 12);
    Font *fr = font_get(FONT_REGULAR, 11);
    Font *fd = font_get(FONT_REGULAR, 10);
    if (!fp || !fr || !fd) return;

    /* bring the two sources up to date before painting them */
    poll_install(st);
    sync_index(st);

    Geo g = geo_of(fp, fr);

    /* header: what this window is, and the Refresh action */
    font_draw(s, fp, (float)x + 6.0f, (float)y + 5.0f, "Apps", th->text);
    {
        int bx, by, bw, bh;
        if (btn_rect(&g, w, &bx, &by, &bw, &bh)) {
            /* the rect is app-local; the painter is the one that adds (x,y) */
            float px = (float)(x + bx);
            float py = (float)(y + by);

            gfx_rrect_aa(s, px, py, (float)bw, (float)bh, 4.0f,
                         color_alpha(th->accent, 0.18f),
                         color_alpha(th->accent, 0.55f), 1.0f);
            float ty = py + ((float)bh - font_height(fd)) * 0.5f;
            if (ty < py) ty = py;
            font_draw(s, fd, px + 9.0f, ty, "Refresh", th->text);
        }
    }
    surf_blend(s, x + 4, y + g.head_h - 6, w - 8, 1, th->panel_border);

    int rows = 0, band = 0, nlines = 0;
    layout_of(h, &g, &rows, &band, &nlines);

    if (st->error[0])
        draw_clipped(s, fr, (float)x + 6.0f, (float)y + (float)g.head_h,
                     (float)w - 12.0f, st->error, th->accent);

    int nrows = list_rows(st, rows);
    int top = y + g.head_h + list_offset(st, &g);

    if (nrows > 0) {
        /* keep the selected row inside the scroll window */
        if (st->sel >= 0) {
            if (st->sel < st->scroll) st->scroll = st->sel;
            if (st->sel >= st->scroll + nrows) st->scroll = st->sel - nrows + 1;
        }
        int max_scroll = st->ndisp - nrows;
        if (max_scroll < 0) max_scroll = 0;
        if (st->scroll > max_scroll) st->scroll = max_scroll;
        if (st->scroll < 0) st->scroll = 0;

        for (int i = 0; i < nrows; i++) {
            int idx = st->scroll + i;
            if (idx >= st->ndisp) break;
            int ry = top + i * g.row_h;
            paint_row(s, fr, fd, th, x, ry, w, g.row_h, st, idx);
        }
    }

    /* description band: the selected row's own text, wrapped */
    if (band > 0) {
        float by = (float)(y + h) - (float)g.foot_h - (float)band;
        surf_blend(s, x + 4, (int)by, w - 8, 1, th->panel_border);
        refresh_wrap(st, fr, w);

        int sel = st->sel;
        if (sel >= 0 && sel < st->ndisp && disp_selectable(&st->disp[sel])) {
            int shown = st->dn;
            if (shown > nlines) shown = nlines;
            if (shown == 0 && nlines > 0)
                draw_clipped(s, fr, (float)x + 6.0f, by + 5.0f, (float)w - 12.0f,
                             st->disp[sel].kind == D_PKG
                                 ? "no description in the index"
                                 : "this manifest has no desc = line",
                             th->text_dim);
            for (int i = 0; i < shown; i++)
                draw_clipped(s, fr, (float)x + 6.0f,
                             by + 5.0f + (float)i * (float)g.line_h,
                             (float)w - 12.0f, st->dlines[i],
                             color_alpha(th->text, 0.85f));

            /* the hint: what Enter will ask for, and nothing else */
            char hint[128];
            float hy = (float)(y + h) - (float)g.foot_h - (float)g.line_h - 3.0f;
            if (st->disp[sel].kind == D_PKG) {
                Pkg *k = &st->avail[st->disp[sel].idx];
                if (is_installed(st, k->name))
                    snprintf(hint, sizeof(hint), "%s is installed - Enter does nothing",
                             k->name);
                else if (k->version[0])
                    snprintf(hint, sizeof(hint), "Enter: pkg install %s (v%s)",
                             k->name, k->version);
                else
                    snprintf(hint, sizeof(hint), "Enter: pkg install %s", k->name);
            } else {
                StoreApp *e = &st->inst[st->disp[sel].idx];
                if (e->from_root)
                    snprintf(hint, sizeof(hint),
                             "%s is in %s - the launcher reads %s",
                             e->id, STORE_ROOT_APPS, STORE_APP_DIR);
                else
                    snprintf(hint, sizeof(hint),
                             "Enter: ask the desktop to launch %s", e->id);
            }
            draw_clipped(s, fd, (float)x + 6.0f, hy, (float)w - 12.0f, hint,
                         color_alpha(th->text, 0.40f));
        } else {
            float hy = (float)(y + h) - (float)g.foot_h - (float)g.line_h - 3.0f;
            draw_clipped(s, fd, (float)x + 6.0f, hy, (float)w - 12.0f,
                         "r: fetch the package index", color_alpha(th->text, 0.40f));
        }
    }

    /* footer: how much is installed and offered, plus the last outcome.
     * A status that does not fit beside the counts gets the whole line - a
     * failure message that is never painted would be worse than a lost
     * count. */
    {
        float fy = (float)(y + h) - font_height(fr) - 3.0f;
        char foot[96];
        snprintf(foot, sizeof(foot), "%d installed - %d available%s",
                 st->ninst, st->navail,
                 st->index_state == 0 ? " - no index" : "");

        float tw = st->status[0] ? font_width(fr, st->status) : 0.0f;
        if (st->status[0] && tw + font_width(fr, foot) + 18.0f < (float)w) {
            font_draw(s, fr, (float)(x + w) - tw - 6.0f, fy, st->status,
                      color_alpha(th->accent, 0.55f));
            draw_clipped(s, fr, (float)x + 6.0f, fy, (float)w - tw - 12.0f, foot,
                         color_alpha(th->text, 0.45f));
        } else if (st->status[0]) {
            draw_clipped(s, fr, (float)x + 6.0f, fy, (float)w - 12.0f, st->status,
                         color_alpha(th->accent, 0.55f));
        } else {
            draw_clipped(s, fr, (float)x + 6.0f, fy, (float)w - 12.0f, foot,
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

    Geo g;
    int rows = 1, band = 0, nlines = 0;
    if (geo_now(&g)) layout_of(s->h, &g, &rows, &band, &nlines);
    rows = list_rows(s, rows);
    if (rows < 1) rows = 1;

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

static void store_click(App *a, int x, int y, int buttons) {
    Store *s = a->state;
    if (!s) return;
    if (buttons && !(buttons & 1)) return;      /* left button only */

    /* The Refresh button. btn_rect() is app-local and so is (x,y): the draw
     * origin must not appear in this comparison. */
    Geo g;
    if (!geo_now(&g)) return;

    int bx = 0, by = 0, bw = 0, bh = 0;
    if (btn_rect(&g, s->w, &bx, &by, &bw, &bh) &&
        x >= bx && x < bx + bw && y >= by && y < by + bh) {
        request_refresh(s);
        return;
    }

    int rows = 0, band = 0, nlines = 0;
    if (!geo_now(&g)) return;
    layout_of(s->h, &g, &rows, &band, &nlines);
    rows = list_rows(s, rows);
    int top = g.head_h + list_offset(s, &g);
    if (rows < 1) return;

    /* Only the row band owns rows. A click above the first row - the
     * "Apps" title, the Refresh button, the rule under them, or the error
     * line when the bundle directory is unreadable - and a click in the
     * description band or the footer belong to no row and must do nothing.
     * Reject the bands first, then divide a value that is known to be >= 0,
     * so truncation and floor agree and no negative or out-of-range index
     * can be produced. */
    if (y < top) return;                        /* header band */
    if (y >= s->h - g.foot_h) return;           /* footer band */

    int row = (y - top) / g.row_h;              /* y >= top: exact */
    if (row < 0 || row >= rows) return;         /* past the list */
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
    .free = store_free,
};
