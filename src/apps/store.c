/* ============================================================
 *  Mystic OS - apps/store.c
 *  "Apps": one window that really manages packages.
 *
 *  Live tree note: this file is C only and talks to the kernel with
 *  opendir/readdir/lstat/stat/unlink/rmdir/open/read/write; it never
 *  invents a number. Every size, version, percentage and error string
 *  below is read from the disk or from errno at the moment it is shown.
 *
 *  Installed - the bundles that are already on the disk, read with
 *  readdir() from /usr/share/mystic/apps and /root/apps (that is where
 *  `pkg install` unpacks them).  A directory counts as a bundle when it
 *  holds a readable 'manifest'; the manifest is parsed the way
 *  src/wmsrv.c parses it (name/desc/exec/version, one '=' per line).
 *  The size shown is the REAL installed size: every regular file under
 *  <root>/<id>, summed with lstat().  The payload is stat()ed too so a
 *  bundle whose exec is missing is drawn dimmed instead of pretending.
 *
 *  Available - the packages the repository advertises, read from
 *  /run/mystic/repo.index.  The index is the same TSV the installer
 *  caches (stage/usr/bin/pkg):
 *
 *      name<TAB>version<TAB>size<TAB>sha256<TAB>file<TAB>description
 *
 *  The size column is the compressed tarball's size in bytes.  The file
 *  column is the tarball's name, and that is what makes real install
 *  progress possible (see below).
 *
 *  SEARCH - '/' focuses a filter box over every row's name and
 *  description (and therefore also over the derived tag, which is a
 *  word from the description).  Typing filters live; Enter keeps the
 *  filter and leaves the box; Ctrl+U clears it; Backspace edits it.
 *  There is no Escape path because src/main.c closes the window on Esc
 *  before any app sees it.
 *
 *  UNINSTALL - 'x' on an Installed row asks first; only 'y' removes.
 *  It deletes /root/apps/<id> and NOTHING else: the path is built from
 *  the validated id, the walk uses lstat() and never follows a symlink,
 *  and a bundle that lives in /usr/share/mystic/apps (the image) is
 *  refused, because that tree belongs to the OS and not to pkg.
 *  The confirmation names the id, so a selection that moves underneath
 *  the prompt cannot change what is removed.
 *
 *  VERSIONS - the installed version is the manifest's "version =" line.
 *  When a manifest has no such line the index's version is shown with a
 *  '~' in front, so the reader can tell whose number it is.  "1.0>1.1"
 *  means the installed manifest and the index disagree.  'U' compares
 *  every installed bundle against the index and reports real counts in
 *  the footer (available updates, image bundles that differ but cannot
 *  be replaced, manifests with no version line); it also asks the
 *  desktop for a fresh index, because comparing against a stale cache
 *  would be a guess.
 *
 *  UPDATE INSTALL - 'i' (or Enter on an Available row) asks the desktop
 *  to run `pkg install <name>` for a package that is missing or whose
 *  index version differs.  An image bundle is refused: the launcher
 *  gives the image's copy priority (src/wmsrv.c: "the image's copy
 *  wins"), so replacing /root/apps/<id> would change nothing a person
 *  can see.
 *
 *  INSTALL PROGRESS - the Store cannot see pkg's stdout, but it can see
 *  pkg's download file.  stage/usr/bin/pkg caches in /tmp/pkg and writes
 *  <cache>/<file>.part while wget is transferring, so:
 *
 *      downloaded = st_size("/tmp/pkg/<file>.part")
 *      total      = the index row's size column
 *      percent    = 100 * downloaded / total, clamped to 99 while the
 *                   transfer is still running
 *
 *  The TEXT never says 100% before <root>/<name>/manifest appears: a
 *  finished download is not an installed package.  The phases are all
 *  decided by stat(): .part present -> downloading; .part gone but
 *  <file> present -> the tarball is cached (or finished), which proves
 *  the download is complete, so pkg is verifying/unpacking and the strip
 *  is full while the label says exactly that; neither -> pkg has not
 *  started (it may still be fetching the index).  No phase is fabricated:
 *  if the file column or the size column is missing, the window says
 *  "waiting" or shows bytes without a percentage instead of a made-up bar.
 *
 *  This window never forks and never installs by itself.  It asks the
 *  desktop, which owns the privileged work:
 *
 *    Enter on an Installed row -> /run/mystic/launch.request   (one id)
 *    Enter/i on an Available row -> /run/mystic/install.request (one name)
 *    'r' or the Refresh button -> /run/mystic/repo.refresh      ("refresh")
 *
 *  The desktop validates the install name against [a-z0-9-]{1,32}
 *  before running `pkg install <name>`.  This file applies the same
 *  rule twice: a row whose name would fail it is never listed, so the
 *  single line written to install.request can only ever be a name the
 *  desktop accepts.
 *
 *  MANIFEST VERSION LINE - this file only READS "version =".  The files
 *  outside this one that would need it are listed in the final report,
 *  not edited here: every stage/usr/share/mystic/apps/<id>/manifest, and
 *  the minimal manifest that stage/usr/bin/pkg writes when a package
 *  ships without one.
 *
 *  Two coordinate spaces: draw() gets this window's origin on the
 *  compositor surface and paints at origin + local, while click() gets
 *  APP-LOCAL coordinates (src/main.c: app_draw(cur->app, s, cx0, cy0,
 *  ...) vs app_click(hit->app, lx - 1, ly - tb - 1, ...)).  Every
 *  rectangle in here - Refresh, the search box, the row band, each
 *  column edge - is app-local, so the painter adds the origin and the
 *  hit-test never does.
 *
 *  VISUAL LANGUAGE - the ONE VISUAL LANGUAGE block gives the spacing
 *  unit and the only three fonts: semibold 15 title, regular 11 body,
 *  medium 9 small.  Geometry comes from one layout_store() Layout: the
 *  inset, the icon box, one row height for every display row, and every
 *  column edge. draw(), key(), click() and the wheel all read that one
 *  Layout, so they cannot disagree about where a row is, and every line
 *  is centred by ui_top(), never by a guessed baseline.
 *
 *  The window is a header (the app's name and one action, Refresh), one
 *  search field, the list, and one status line.  The list has two
 *  sections whose labels carry the same marks as their rows; a row is
 *  the bundle or package name on the left and its version and real size
 *  right-aligned on one edge.  Columns are dropped when the window is
 *  too narrow for them.  A drawn mark tells the two kinds apart: a
 *  parcel for a package that can be installed, a tick for one already
 *  on the disk. Both are geometry built from the theme's accent - no
 *  images. The selected row's description and the action Enter will
 *  take live on the one status line instead of a separate text band.
 *
 *  Keys:  j/k or Up/Down  move        Enter  launch / install / update
 *         Home/End/g/G    first/last  PgUp/PgDn  by a screen
 *         /               search      Ctrl+U  clear the filter
 *         r               refresh the repo index
 *         x               uninstall   (then y confirms, any other key
 *                                      cancels)
 *         i               install / update the selected package
 *         U               check for updates (index vs installed)
 *  Mouse: click the Refresh button, the search box, its x, or a row;
 *         click the selected row again to activate it. The wheel
 *         scrolls whole rows and keeps the selection in the window.
 * ============================================================ */
#include "mystic.h"
#include "ipc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
/* pkg's cache: stage/usr/bin/pkg uses CACHE="${MYSTIC_CACHE:-/tmp/pkg}".
 * The image never overrides MYSTIC_CACHE, so /tmp/pkg is the real path. */
#ifndef STORE_PKG_CACHE
#define STORE_PKG_CACHE "/tmp/pkg"
#endif

/* how long an install may run before the window stops claiming it is
 * running (pkg's own wget timeout is 300 s per read; the browser package
 * is 573 MB, so a shorter deadline would declare a healthy download dead
 * and offer the row again mid-transfer). */
#ifndef STORE_INSTALL_TIMEOUT_SEC
#define STORE_INSTALL_TIMEOUT_SEC 900.0
#endif

#define DESC_LINE 256
#define PKG_MAX 64              /* repo rows we hold; the index is small */
#define INST_MAX (2 * WM_MAX_APPS)   /* both bundle roots together */
#define DISP_MAX (INST_MAX + PKG_MAX + 6)

/* A directory entry's name we are willing to build a path from. The
 * uninstall walk refuses anything with a slash or a dot-dot, so the only
 * path it can ever remove is STORE_ROOT_APPS/<one safe segment>. */
#define SEG_MAX 32
#define VERSION_MAX 24

/* Rows the wheel moves per notch. A count, not a pixel size: it moves
 * whole entries, so it follows the text scale for free. */
#define WHEEL_ROWS 3

typedef struct {
    char id[SEG_MAX];
    char name[64];
    char desc[DESC_LINE];
    char exe[128];
    char version[VERSION_MAX];   /* manifest "version =", "" when absent */
    long long total;             /* bytes of the ACTIVE bundle tree */
    long long root_total;        /* bytes under /root/apps/<id>, 0 if none */
    int  nfiles;                 /* regular files in the active tree */
    int  have_size;              /* the exec was stat()able: the row can launch */
    int  in_image;               /* a copy in STORE_APP_DIR */
    int  in_root;                /* a copy in STORE_ROOT_APPS (pkg's root) */
} StoreApp;

typedef struct {
    char name[SEG_MAX + 1];      /* pkg names may be 32 chars + NUL */
    char version[VERSION_MAX];
    char desc[DESC_LINE];
    char file[64];               /* index column 5: tarball basename */
    long long size;              /* bytes, the index's size column */
    int  have_size;
} Pkg;

/* one painted line of the window: a section title, a real row, or a note */
enum { D_SECT = 1, D_APP, D_PKG, D_MSG };

/* the mark painted in a row's icon column: a parcel for an installable
 * package, a tick for anything already on the disk */
enum { MARK_NONE = 0, MARK_PKG, MARK_CHECK };

/* what the version column is showing, for its colour */
enum { VC_NONE = 0, VC_PLAIN, VC_INDEX, VC_UPDATE };

typedef struct {
    int         kind;
    int         idx;    /* D_APP: inst[] index, D_PKG: avail[] index,
                           D_SECT: how many rows the section shows */
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

    /* the search box. filter is ASCII only: the keys arrive as bytes and
     * names/descriptions are UTF-8, but a byte-wise case-folded match is
     * still a real match for the ASCII text these files hold. */
    char     filter[64];
    int      filter_len;
    int      search_focus;
    int      nshown;         /* selectable rows the filter left visible */
    int      nsh_inst, nsh_avail;

    /* an uninstall waiting for its yes/no */
    char     confirm_id[SEG_MAX];
    int      confirm_keep_image;

    char     error[128];     /* why the bundle directory could not be read */
    char     status[192];    /* result of the last request, or progress */
    char     pending[SEG_MAX];    /* name asked for, until its manifest appears */
    char     pending_file[64];    /* tarball name from the index row */
    long long pending_total;      /* index bytes, -1 when the row has none */
    long long pending_part;       /* last .part size seen, -1 when absent */
    int       pending_phase;      /* 0 waiting, 1 downloading, 2 verifying */
    double    pending_since;      /* CLOCK_MONOTONIC when the request was written,
                                     0 when nothing is in flight */

    int      px, py;         /* last pointer position reported to us */
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

/* A path segment from readdir that is safe to append to a root: no
 * separator, no traversal, short enough for the id buffers. */
static int safe_seg(const char *n) {
    if (!n || !n[0] || strlen(n) >= SEG_MAX) return 0;
    if (!strcmp(n, ".") || !strcmp(n, "..")) return 0;
    if (strchr(n, '/') || strchr(n, '\\')) return 0;
    return 1;
}

/* ------------------------------------------------------------
 * Manifest parsing - identical rules to wms_scan_apps() in
 * src/wmsrv.c: strip the newline, split on the first '=', trim
 * spaces around the key, take the value verbatim. "version" is the one
 * key wmsrv.c ignores and this window needs; a missing line is "".
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
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;
        char *ke = k + strlen(k);
        while (ke > k && ke[-1] == ' ') *--ke = 0;
        char *ve = v + strlen(v);
        while (ve > v && ve[-1] == ' ') *--ve = 0;
        if (!strcmp(k, "name"))         snprintf(a->name, sizeof(a->name), "%s", v);
        else if (!strcmp(k, "desc"))    snprintf(a->desc, sizeof(a->desc), "%s", v);
        else if (!strcmp(k, "exec"))    snprintf(a->exe, sizeof(a->exe), "%s", v);
        else if (!strcmp(k, "version")) snprintf(a->version, sizeof(a->version), "%s", v);
    }
    fclose(f);

    if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
    if (!a->exe[0])  snprintf(a->exe,  sizeof(a->exe),  "bin");
    return 1;
}

/* ------------------------------------------------------------
 * Real sizes. tree_scan() sums the st_size of every regular file
 * under a bundle with lstat(), so a symlink contributes nothing but
 * the link itself (it is not followed) and a device file contributes
 * nothing either. That is the installed size the disk is using.
 * ------------------------------------------------------------ */
static void tree_scan(const char *path, int depth, long long *bytes, int *files) {
    if (depth > 24) return;                 /* a pathological tree, not a bundle */
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[512];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || n >= (int)sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            tree_scan(child, depth + 1, bytes, files);
        } else if (S_ISREG(st.st_mode)) {
            *bytes += (long long)st.st_size;
            (*files)++;
        }
    }
    closedir(d);
}

/* The payload is a real file on disk; its size is what we show. The
 * bundle total is the tree above, not the payload alone. */
static void probe_bundle(const char *dir, StoreApp *a) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", dir, a->id, a->exe);

    struct stat st;
    a->have_size = (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;

    snprintf(path, sizeof(path), "%s/%s", dir, a->id);
    a->total = 0;
    a->nfiles = 0;
    tree_scan(path, 0, &a->total, &a->nfiles);
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
 * Lookups. Both lists are tiny (<= 64 + 64) and every caller needs the
 * answer for the row it is painting, so a linear scan is the honest and
 * cheapest thing here.
 * ------------------------------------------------------------ */
static int find_pkg(const Store *s, const char *name) {
    for (int i = 0; i < s->navail; i++)
        if (!strcmp(s->avail[i].name, name)) return i;
    return -1;
}

static int find_inst(const Store *s, const char *name) {
    for (int i = 0; i < s->ninst; i++)
        if (!strcmp(s->inst[i].id, name)) return i;
    return -1;
}

/* ------------------------------------------------------------
 * A row matches when the filter is a case-insensitive substring of its
 * name or of its description.
 * ------------------------------------------------------------ */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int row_match(const Store *s, const char *name, const char *desc) {
    if (!s->filter[0]) return 1;
    return contains_ci(name, s->filter) || contains_ci(desc, s->filter);
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

    int hit_inst = 0, hit_avail = 0;
    for (int i = 0; i < s->ninst; i++)
        if (row_match(s, s->inst[i].name, s->inst[i].desc)) hit_inst++;
    for (int i = 0; i < s->navail; i++)
        if (row_match(s, s->avail[i].name, s->avail[i].desc)) hit_avail++;

    s->ndisp = 0;
    s->sel = -1;

    disp_push(s, D_SECT, hit_inst, "Installed");
    if (s->ndisp > 0) s->disp[s->ndisp - 1].mark = MARK_CHECK;
    if (s->ninst == 0)
        disp_push(s, D_MSG, 0, "no app bundles installed");
    else if (hit_inst == 0)
        disp_push(s, D_MSG, 0, "no installed bundle matches");
    else
        for (int i = 0; i < s->ninst; i++)
            if (row_match(s, s->inst[i].name, s->inst[i].desc))
                disp_push(s, D_APP, i, NULL);

    disp_push(s, D_SECT, hit_avail, "Available");
    if (s->ndisp > 0) s->disp[s->ndisp - 1].mark = MARK_PKG;
    if (s->index_state == 0)
        disp_push(s, D_MSG, 0, "no index - press r to fetch it");
    else if (s->index_state < 0)
        disp_push(s, D_MSG, 0, s->index_err);
    else if (s->navail == 0)
        disp_push(s, D_MSG, 0, "the index lists no packages");
    else if (hit_avail == 0)
        disp_push(s, D_MSG, 0, "no index package matches");
    else
        for (int i = 0; i < s->navail; i++)
            if (row_match(s, s->avail[i].name, s->avail[i].desc))
                disp_push(s, D_PKG, i, NULL);

    s->nsh_inst = hit_inst;
    s->nsh_avail = hit_avail;
    s->nshown = hit_inst + hit_avail;

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
}

/* ------------------------------------------------------------
 * Reading the two things this window knows about: the bundles on
 * disk, and the index the desktop downloaded.
 * ------------------------------------------------------------ */
static long long root_total(const char *id) {
    char path[512];
    long long bytes = 0;
    int files = 0;                 /* only the byte total is shown for a shadowed copy */
    snprintf(path, sizeof(path), "%s/%s", STORE_ROOT_APPS, id);
    tree_scan(path, 0, &bytes, &files);
    return bytes;
}

static void add_bundle(Store *s, const char *dir, const char *id, int from_root) {
    if (!id[0] || strlen(id) >= sizeof(s->inst[0].id)) return;
    if (strchr(id, '/')) return;

    for (int i = 0; i < s->ninst; i++) {
        if (strcmp(s->inst[i].id, id)) continue;
        /* The image root is scanned first, so the active record (name,
         * desc, version, sizes) is the image's copy - which is also the
         * copy src/wmsrv.c launches when both roots hold the id. A root
         * copy found later only adds the facts the uninstall prompt
         * needs: that it exists and how much removing it would free. */
        if (from_root) {
            s->inst[i].in_root = 1;
            s->inst[i].root_total = root_total(id);
        } else {
            s->inst[i].in_image = 1;
        }
        return;
    }
    if (s->ninst >= INST_MAX) return;

    StoreApp *a = &s->inst[s->ninst];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    /* a directory in the shared bundle root without a manifest is not an
     * app; /root/apps is pkg's own root, so list whatever it put there */
    if (!read_manifest(dir, a) && !from_root) return;
    if (!a->desc[0] && from_root)
        snprintf(a->desc, sizeof(a->desc), "installed by pkg");
    if (from_root) a->in_root = 1; else a->in_image = 1;
    probe_bundle(dir, a);
    if (from_root) a->root_total = a->total;
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
        if (line[0] == '#') continue;          /* index_ok() allows comments */

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
            long long v = strtoll(field[2], &end, 10);
            if (end && end != field[2] && v > 0) {
                k->size = v;
                k->have_size = 1;
            }
        }
        /* column 5 is the tarball's basename; anything with a separator
         * or a dot-dot is not a name pkg would accept either, so it is
         * kept only for display and never used to build a cache path */
        if (nf > 4 && field[4][0] && !strchr(field[4], '/') &&
            strcmp(field[4], ".") && strcmp(field[4], "..")) {
            snprintf(k->file, sizeof(k->file), "%s", field[4]);
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

/* ------------------------------------------------------------
 * Sizes and versions, printed the same way everywhere.
 * ------------------------------------------------------------ */
static void human_size(long long b, char *out, size_t outsz) {
    if (b < 0)                 snprintf(out, outsz, "?");
    else if (b >= 1024LL * 1024 * 1024)
        snprintf(out, outsz, "%.1f GB", (double)b / 1073741824.0);
    else if (b >= 1024LL * 1024)
        snprintf(out, outsz, "%.1f MB", (double)b / 1048576.0);
    else if (b >= 1024)
        snprintf(out, outsz, "%.1f KB", (double)b / 1024.0);
    else
        snprintf(out, outsz, "%lld B", b);
}

static long long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long long)st.st_size;
}

/* The version cell. VC_UPDATE means the installed manifest and the index
 * disagree; VC_INDEX means there is no "version =" line in the installed
 * manifest and the '~' marks the number as the index's, not the disk's. */
static int version_cell(const Store *s, int kind, int idx, char *out, size_t outsz) {
    out[0] = 0;
    if (kind == D_PKG) {
        const Pkg *p = &s->avail[idx];
        int ii = find_inst(s, p->name);
        if (ii >= 0 && s->inst[ii].version[0] && p->version[0] &&
            strcmp(s->inst[ii].version, p->version)) {
            snprintf(out, outsz, "%s>%s", s->inst[ii].version, p->version);
            return VC_UPDATE;
        }
        if (!p->version[0]) { snprintf(out, outsz, "-"); return VC_NONE; }
        snprintf(out, outsz, "%s", p->version);
        return VC_PLAIN;
    }
    if (kind != D_APP) return VC_NONE;

    const StoreApp *a = &s->inst[idx];
    int p = find_pkg(s, a->id);
    if (a->version[0]) {
        if (p >= 0 && s->avail[p].version[0] &&
            strcmp(a->version, s->avail[p].version)) {
            snprintf(out, outsz, "%s>%s", a->version, s->avail[p].version);
            return VC_UPDATE;
        }
        snprintf(out, outsz, "%s", a->version);
        return VC_PLAIN;
    }
    if (p >= 0 && s->avail[p].version[0]) {
        snprintf(out, outsz, "~%s", s->avail[p].version);
        return VC_INDEX;
    }
    snprintf(out, outsz, "-");
    return VC_NONE;
}

/* A real update needs a pkg-managed bundle: the image's copy is the one
 * the launcher runs, so replacing the /root/apps copy would be invisible. */
static int has_update(const Store *s, const StoreApp *a) {
    if (!a->in_root || a->in_image || !a->version[0]) return 0;
    int p = find_pkg(s, a->id);
    return p >= 0 && s->avail[p].version[0] &&
           strcmp(a->version, s->avail[p].version) != 0;
}

/* ------------------------------------------------------------
 * Install polling. Every number in this function comes from stat() on
 * the files pkg itself writes (stage/usr/bin/pkg):
 *
 *     /tmp/pkg/<file>.part  while wget is transferring
 *     /tmp/pkg/<file>       after the rename; sha256 then tar unpack
 *     <root>/<name>/manifest  only when the bundle is complete
 *
 * The percentage is downloaded/total where total is the index's size
 * column for that exact row; it is capped at 99 until the manifest
 * exists, because a downloaded tarball is not an installed package.
 * ------------------------------------------------------------ */
static void poll_install(Store *s) {
    if (!s->pending[0]) return;

    /* success is a real manifest, in pkg's own root or in the image root */
    const char *roots[2] = { STORE_ROOT_APPS, STORE_APP_DIR };
    char path[512];
    struct stat st;
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s/manifest", roots[i], s->pending);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            char done[SEG_MAX];
            snprintf(done, sizeof(done), "%s", s->pending);
            s->pending[0] = 0;
            s->pending_file[0] = 0;
            s->pending_total = -1;
            s->pending_part = -1;
            s->pending_phase = 0;
            s->pending_since = 0.0;

            scan_installed(s);

            long long bytes = 0;
            int files = 0;
            int ii = find_inst(s, done);
            if (ii >= 0) { bytes = s->inst[ii].total; files = s->inst[ii].nfiles; }
            char hb[24];
            human_size(bytes, hb, sizeof(hb));
            snprintf(s->status, sizeof(s->status), "installed %s - %d file%s, %s on disk",
                     done, files, files == 1 ? "" : "s", hb);
            return;
        }
    }

    /* still running: look at pkg's own cache */
    long long part = -1, tarball = -1;
    if (s->pending_file[0]) {
        snprintf(path, sizeof(path), "%s/%s.part", STORE_PKG_CACHE, s->pending_file);
        part = file_size(path);
        snprintf(path, sizeof(path), "%s/%s", STORE_PKG_CACHE, s->pending_file);
        tarball = file_size(path);
    }
    s->pending_part = part;
    s->pending_phase = part >= 0 ? 1 : (tarball >= 0 ? 2 : 0);

    if (s->pending_since > 0.0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            double waited = now - s->pending_since;
            if (waited < 0.0) waited = 0.0;
            long secs = (long)waited;

            if (waited >= STORE_INSTALL_TIMEOUT_SEC) {
                /* the desktop forked pkg and nothing arrived: say so, stop
                 * claiming progress, and leave the row installable again. */
                snprintf(s->status, sizeof(s->status),
                         "install of %s did not finish - Enter retries; run pkg to see why",
                         s->pending);
                s->pending[0] = 0;
                s->pending_file[0] = 0;
                s->pending_total = -1;
                s->pending_part = -1;
                s->pending_phase = 0;
                s->pending_since = 0.0;
                return;
            }

            char hpart[24], htotal[24];
            if (s->pending_phase == 1) {
                human_size(part, hpart, sizeof(hpart));
                if (s->pending_total > 0) {
                    human_size(s->pending_total, htotal, sizeof(htotal));
                    int pct = 0;
                    if (s->pending_total > 0) {
                        pct = (int)((double)part * 100.0 / (double)s->pending_total);
                        if (pct < 0) pct = 0;
                        if (pct > 99) pct = 99;   /* 100% only when it is installed */
                    }
                    snprintf(s->status, sizeof(s->status),
                             "installing %s - %s / %s (%d%%), %lds",
                             s->pending, hpart, htotal, pct, secs);
                } else {
                    snprintf(s->status, sizeof(s->status),
                             "installing %s - %s downloaded, %lds",
                             s->pending, hpart, secs);
                }
            } else if (s->pending_phase == 2) {
                human_size(tarball, hpart, sizeof(hpart));
                snprintf(s->status, sizeof(s->status),
                         "installing %s - %s downloaded, verifying/unpacking, %lds",
                         s->pending, hpart, secs);
            } else {
                snprintf(s->status, sizeof(s->status),
                         "installing %s - waiting for pkg, %lds", s->pending, secs);
            }
        }
    }
}

/* ============================================================
 *  ONE VISUAL LANGUAGE
 *
 *  This block is identical, byte for byte, in files.c, store.c and
 *  settings.c: the owner asked for one desktop, not three
 *  improvisations. Change it in all three at once.
 *
 *  SPACING. One unit, 8 px, scaled by ui_px() so it follows the global
 *  text scale. Every padding and every gap in this window is SP(n);
 *  there is no other kind of space.
 *
 *  TYPE. Exactly three sizes, and the same three faces in all three
 *  windows: semibold 15 is a title, regular 11 is body text, medium 9
 *  is small print. No other font_get() call is allowed.
 *
 *  ROWS. Every list row is ui_row_h(): the body line box plus one unit
 *  of leading. Hover and selection are the same rectangle with a
 *  different colour, so a row never grows, shrinks or gains a border
 *  when the pointer moves over it.
 * ============================================================ */
#define UI_UNIT 8
static int SP(int n) { return ui_px(n * UI_UNIT); }

typedef struct { Font *title, *body, *small; } UiFont;

static UiFont ui_fonts(void) {
    UiFont f;
    f.title = font_get(FONT_SEMIBOLD, 15);
    f.body  = font_get(FONT_REGULAR,  11);
    f.small = font_get(FONT_MEDIUM,    9);
    return f;
}

/* The ink of one line is font_height()/1.4 tall (src/font.c:119), and
 * centring that box - not the line box, which includes leading - is what
 * puts caps and descenders equally inside a band. No guessed baseline. */
static int ui_top(Font *f, int band) {
    int ink;
    if (!f || band <= 0) return 0;
    ink = (int)(font_height(f) / 1.4f + 0.5f);
    if (ink >= band) return 0;
    return (band - ink) / 2;
}

static int ui_ceilf(float v) {
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* the one row height: body line box + one spacing unit */
static int ui_row_h(Font *body) {
    int h = ui_ceilf(font_height(body)) + SP(1);
    return h < SP(3) ? SP(3) : h;
}

/* A filled rounded rectangle: the only shape hover and selection use. */
static void ui_pill(Surface *s, int x, int y, int w, int h, float r, u32 c) {
    if (w <= 0 || h <= 0) return;
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, r, c, 0, 0);
}

/* A value on the shared right edge. */
static void ui_text_right(Surface *s, Font *f, int right, int y, const char *t, u32 c) {
    if (!f || !t || !t[0]) return;
    font_draw(s, f, (float)right - font_width(f, t), (float)y, t, c);
}

/* One button: the header action and the dialog buttons share it, so a
 * button looks the same wherever it appears. */
static void ui_button(Surface *s, int x, int y, int w, int h, float r,
                      const char *label, Font *f, int primary, const Theme *th) {
    u32 fill = primary ? color_alpha(th->accent, 0.30f)
                       : color_alpha(th->text, 0.07f);
    u32 bord = primary ? color_alpha(th->accent, 0.85f)
                       : color_alpha(th->panel_border, 0.85f);
    if (w <= 0 || h <= 0 || !f) return;
    gfx_rrect_aa(s, (float)x, (float)y, (float)w, (float)h, r, fill, bord, 1.0f);
    float tw = font_width(f, label);
    font_draw(s, f, (float)x + ((float)w - tw) * 0.5f,
              (float)y + (float)ui_top(f, h), label,
              primary ? th->text : color_alpha(th->text, 0.88f));
}


/* ------------------------------------------------------------
 *  Layout - the single source of every number in this window
 *
 *  The header is the app's name and one action (Refresh). Under it sits
 *  one search field. Then the list, one row height for every display
 *  entry - section labels, messages, installed bundles and index
 *  packages - and one status line. Columns are dropped from the right
 *  when the window cannot hold them, so two values can never overlap.
 * ------------------------------------------------------------ */
typedef struct {
    UiFont f;                  /* the only three fonts in this window */

    int m, gap;                /* SP(2) outer inset, SP(1) between things */
    int header_h, search_h, footer_h;
    int row_h, icon_sz, rows;
    int list_y, list_bot;

    /* header: title left, Refresh right */
    int title_x, title_dy;
    int btn_x, btn_y, btn_w, btn_h, btn_label;

    /* search field */
    int se_x, se_y, se_w, se_h, se_icon, se_text_x, se_clear_x, se_clear_w, se_dy;

    /* row grid */
    int name_x, name_w, row_dy, small_dy;
    int ver_right, ver_col, size_right, size_col;
    int show_ver, show_size;
    int row_xpad;

    int radius, rule, prog_h, footer_dy;
} Layout;

static int layout_store(Layout *L, const Store *st, int w, int h) {
    memset(L, 0, sizeof(*L));
    if (w < 1 || h < 1) return 0;

    L->f = ui_fonts();
    if (!L->f.title || !L->f.body || !L->f.small) return 0;

    L->m = SP(2);
    L->gap = SP(1);
    L->rule = ui_px(1);
    L->radius = SP(1);
    L->prog_h = ui_px(2);
    L->row_xpad = L->m;

    L->row_h = ui_row_h(L->f.body);
    L->icon_sz = SP(2);
    if (L->icon_sz > L->row_h - SP(1)) L->icon_sz = L->row_h - SP(1);
    if (L->icon_sz < SP(1)) L->icon_sz = SP(1);

    L->header_h = ui_ceilf(font_height(L->f.title)) + 2 * SP(1);
    L->search_h = ui_ceilf(font_height(L->f.body)) + 2 * SP(1);
    L->footer_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);

    /* a short window gives up the footer first, then the search field */
    if (L->header_h + L->search_h + 2 * L->gap + L->footer_h + L->row_h > h)
        L->footer_h = 0;
    if (L->header_h + L->search_h + 2 * L->gap + L->row_h > h)
        L->search_h = 0;
    if (L->header_h + L->row_h > h) L->header_h = h;

    /* header: title on the left, one action right-aligned on the margin */
    L->title_x = L->m;
    L->title_dy = ui_top(L->f.title, L->header_h);
    L->btn_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);
    if (L->btn_h > L->header_h - SP(1)) L->btn_h = L->header_h - SP(1);
    if (L->btn_h < SP(2)) L->btn_h = SP(2);
    L->btn_y = (L->header_h - L->btn_h) / 2;
    {
        int full = 2 * SP(2) + ui_ceilf(font_width(L->f.small, "Refresh"));
        int room = w - 2 * L->m -
                   ui_ceilf(font_width(L->f.title, "Apps")) - 2 * L->gap;
        L->btn_label = (full <= room);
        L->btn_w = L->btn_label ? full : 2 * SP(2);
        if (L->btn_w > w - 2 * L->m) L->btn_w = w - 2 * L->m;
        L->btn_x = w - L->m - L->btn_w;
        if (L->btn_x < L->m) L->btn_x = L->m;
    }

    /* the search field, on the same margin as everything else */
    L->se_x = L->m;
    L->se_w = w - 2 * L->m;
    L->se_y = L->header_h + SP(1);
    L->se_h = L->search_h;
    L->se_icon = ui_ceilf(font_height(L->f.small));
    L->se_text_x = L->se_x + SP(1) + L->se_icon + SP(1);
    L->se_clear_w = L->se_icon + SP(1);
    L->se_clear_x = L->se_x + L->se_w - SP(1) - L->se_clear_w;
    L->se_dy = ui_top(L->f.body, L->se_h);
    if (L->se_w < 2 * SP(4)) L->se_w = 2 * SP(4);

    /* bands */
    L->list_y = L->header_h + (L->search_h ? L->search_h + 2 * L->gap : 0);
    L->list_bot = h - L->footer_h;
    if (L->list_bot < L->list_y) L->list_bot = L->list_y;
    {
        int avail = L->list_bot - L->list_y;
        L->rows = avail > 0 ? avail / L->row_h : 0;
        if (L->rows > DISP_MAX) L->rows = DISP_MAX;
    }
    L->row_dy = ui_top(L->f.body, L->row_h);
    L->small_dy = ui_top(L->f.small, L->row_h);
    L->footer_dy = L->footer_h > 0 ? ui_top(L->f.small, L->footer_h) : 0;

    /* columns: every value ends on the one right edge */
    L->name_x = L->m + L->icon_sz + L->gap;
    L->size_col = ui_ceilf(font_width(L->f.small, "999.9 MB"));
    L->size_right = w - L->m;

    int vmax = ui_ceilf(font_width(L->f.small, "0.0"));
    for (int i = 0; i < st->ndisp; i++) {
        const Disp *d = &st->disp[i];
        if (d->kind != D_APP && d->kind != D_PKG) continue;
        char v[64];
        version_cell(st, d->kind, d->idx, v, sizeof(v));
        int vw = ui_ceilf(font_width(L->f.small, v));
        if (vw > vmax) vmax = vw;
    }
    if (vmax > SP(12)) vmax = SP(12);
    L->ver_col = vmax;

    L->show_size = 1;
    L->show_ver = 1;
    L->ver_right = L->size_right - L->size_col - L->gap;
    L->name_w = L->ver_right - L->ver_col - L->gap - L->name_x;

    if (L->name_w < SP(10)) {                    /* drop the size column */
        L->show_size = 0;
        L->ver_right = L->size_right;
        L->name_w = L->ver_right - L->ver_col - L->gap - L->name_x;
    }
    if (L->name_w < SP(10)) {                    /* then the version column */
        L->show_ver = 0;
        L->name_w = L->size_right - L->name_x;
    }
    if (L->name_w < SP(6)) L->name_w = SP(6);
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
    if (len >= sizeof(buf) - 3) len = sizeof(buf) - 3;
    memcpy(buf, text, len);
    buf[len] = 0;
    if (font_width(f, buf) <= maxw + 0.5f) { font_draw(s, f, x, y, buf, color); return; }
    float budget = maxw - font_width(f, "..");
    size_t cut = len;
    while (cut > 0 && font_width(f, buf) > budget) buf[--cut] = 0;
    if (cut == 0) return;
    memcpy(buf + cut, "..", 3);
    font_draw(s, f, x, y, buf, color);
}

/* The end of a string - used by the search field, where the caret is at
 * the right and the interesting part of a long filter is its tail. */
static void draw_tail(Surface *s, Font *f, float x, float y, float maxw,
                      const char *text, u32 color) {
    if (!text || !text[0] || maxw < 4.0f) return;
    float full = font_width(f, text);
    if (full <= maxw) { font_draw(s, f, x, y, text, color); return; }
    const char *p = text;
    while (*p && font_width(f, p) > maxw - font_width(f, "..")) p++;
    char buf[320];
    snprintf(buf, sizeof(buf), "..%s", p);
    font_draw(s, f, x, y, buf, color);
}

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

/* Returns 1 when the request was written, so check-for-updates can say
 * whether a fresh index is on its way or the write failed. */
static int request_refresh(Store *s) {
    char err[128];
    int ok = write_line(STORE_REPO_REFRESH, "refresh", err, sizeof(err));

    /* a refresh is also the moment to look at the disk again: pkg writes
     * into a bundle root, so a finished install shows up here too */
    scan_installed(s);

    if (!ok) {
        snprintf(s->status, sizeof(s->status), "%s", err);
        return 0;
    }
    snprintf(s->status, sizeof(s->status),
             "refresh requested - the desktop fetches the index");
    return 1;
}

/* Ask for one package. The name and the tarball's basename are copied
 * from the index row that was just displayed, so the progress poll below
 * looks at exactly the file pkg is about to write. */
static void start_install(Store *s, const Pkg *k) {
    if (s->pending[0]) {
        snprintf(s->status, sizeof(s->status),
                 "already installing %s - wait for it", s->pending);
        return;
    }
    if (!pkg_name_ok(k->name)) {      /* belt and braces: load_index filtered */
        snprintf(s->status, sizeof(s->status), "refusing package name");
        return;
    }

    int ii = find_inst(s, k->name);
    if (ii >= 0 && s->inst[ii].in_image) {
        snprintf(s->status, sizeof(s->status),
                 "the image's copy of %s is what launches; pkg installs to %s, "
                 "which it shadows", k->name, STORE_ROOT_APPS);
        return;
    }
    if (ii >= 0 && s->inst[ii].version[0] && k->version[0] &&
        !strcmp(s->inst[ii].version, k->version)) {
        snprintf(s->status, sizeof(s->status), "%s is already at version %s",
                 k->name, s->inst[ii].version);
        return;
    }

    char err[128];
    if (!write_line(STORE_INSTALL_REQUEST, k->name, err, sizeof(err))) {
        snprintf(s->status, sizeof(s->status), "%s", err);
        return;
    }

    snprintf(s->pending, sizeof(s->pending), "%s", k->name);
    snprintf(s->pending_file, sizeof(s->pending_file), "%s", k->file);
    s->pending_total = k->have_size ? k->size : -1;
    s->pending_part = -1;
    s->pending_phase = 0;
    s->pending_since = 0.0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        s->pending_since = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    if (ii >= 0)
        snprintf(s->status, sizeof(s->status), "updating %s to %s...",
                 k->name, k->version[0] ? k->version : "the index version");
    else
        snprintf(s->status, sizeof(s->status), "installing %s...", k->name);
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
         * AND /root/apps, which is where pkg unpacks. */
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
        int ii = find_inst(s, k->name);
        int differs = ii >= 0 && s->inst[ii].version[0] && k->version[0] &&
                      strcmp(s->inst[ii].version, k->version) != 0;
        if (is_installed(s, k->name) && !differs) {
            snprintf(s->status, sizeof(s->status), "already installed: %s", k->name);
            return;
        }
        start_install(s, k);
        return;
    }

    /* section titles and notes are not rows you can act on */
}

/* ------------------------------------------------------------
 * Check for updates: compare every installed manifest version with the
 * index version for the same id. The index may be old, so this also asks
 * the desktop for a fresh one; the result shown is the comparison that
 * could be made right now, not a promise about the next index.
 * ------------------------------------------------------------ */
static void check_updates(Store *s) {
    int upd = 0, img = 0, nov = 0;
    char first[96];
    first[0] = 0;

    for (int i = 0; i < s->ninst; i++) {
        const StoreApp *a = &s->inst[i];
        if (!a->version[0]) { nov++; continue; }
        int p = find_pkg(s, a->id);
        if (p < 0 || !s->avail[p].version[0]) continue;
        if (!strcmp(a->version, s->avail[p].version)) continue;
        if (a->in_image) {
            img++;
        } else if (a->in_root) {
            upd++;
            if (!first[0])
                snprintf(first, sizeof(first), ": %s %s>%s", a->id,
                         a->version, s->avail[p].version);
        }
    }

    int fresh = request_refresh(s);     /* this rescans the disk too */

    char msg[192];
    snprintf(msg, sizeof(msg),
             "checked %d installed against %d in the index: %d update%s%s%s%s",
             s->ninst, s->navail, upd, upd == 1 ? "" : "s", first,
             img ? " - image copies differ, pkg cannot replace them" : "",
             nov ? " - manifests with no version = are skipped" : "");
    if (fresh && (int)strlen(msg) + 26 < (int)sizeof(msg))
        strncat(msg, " - fetching the index", sizeof(msg) - strlen(msg) - 1);
    else if (!fresh && (int)strlen(msg) + 32 < (int)sizeof(msg))
        strncat(msg, " - refresh request failed (press r)", sizeof(msg) - strlen(msg) - 1);

    snprintf(s->status, sizeof(s->status), "%s", msg);
}

/* ------------------------------------------------------------
 * Uninstall. rm_tree() only ever receives STORE_ROOT_APPS/<id> where id
 * is a validated display-list bundle: no '/', no "..". It removes the
 * directory contents with lstat(), never follows a symlink (a link is
 * unlinked, not descended into), and rmdir()s the directory itself.
 * The image root is never passed to it - that is the OS's tree.
 * ------------------------------------------------------------ */
static int rm_tree(const char *path, int depth, long long *bytes, int *files,
                   char *err, size_t errsz) {
    if (depth > 24) {
        snprintf(err, errsz, "%s: nesting too deep", path);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return 0;
    }

    int ok = 1;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[512];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || n >= (int)sizeof(child)) {
            snprintf(err, errsz, "%s: path too long", path);
            ok = 0;
            break;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            snprintf(err, errsz, "%s: %s", child, strerror(errno));
            ok = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = rm_tree(child, depth + 1, bytes, files, err, errsz);
        } else {
            /* a symlink is unlinked, never followed; only a regular file
             * contributes bytes, and st_size of a link is not disk used */
            if (unlink(child) != 0) {
                snprintf(err, errsz, "%s: %s", child, strerror(errno));
                ok = 0;
                break;
            }
            if (S_ISREG(st.st_mode)) {
                *bytes += (long long)st.st_size;
                (*files)++;
            }
        }
    }
    closedir(d);
    if (!ok) return 0;

    if (rmdir(path) != 0) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return 0;
    }
    return 1;
}

static void do_uninstall(Store *s) {
    char id[SEG_MAX];
    snprintf(id, sizeof(id), "%s", s->confirm_id);
    int keep_image = s->confirm_keep_image;
    s->confirm_id[0] = 0;
    s->confirm_keep_image = 0;

    if (!safe_seg(id)) {
        snprintf(s->status, sizeof(s->status), "refusing an unsafe bundle name");
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORE_ROOT_APPS, id);

    struct stat st;
    if (lstat(path, &st) != 0) {
        snprintf(s->status, sizeof(s->status), "%s: %s", path, strerror(errno));
        scan_installed(s);           /* the row was stale: drop it */
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(s->status, sizeof(s->status), "%s is not a directory - refusing", path);
        scan_installed(s);
        return;
    }

    long long bytes = 0;
    int files = 0;
    char err[192];
    if (!rm_tree(path, 0, &bytes, &files, err, sizeof(err))) {
        snprintf(s->status, sizeof(s->status), "removal stopped: %s", err);
        scan_installed(s);           /* show whatever is left, honestly */
        return;
    }

    scan_installed(s);
    char hb[24];
    human_size(bytes, hb, sizeof(hb));
    snprintf(s->status, sizeof(s->status), "removed %s - %d file%s, %s freed%s",
             id, files, files == 1 ? "" : "s", hb,
             keep_image ? " (the image copy stays)" : "");
}

/* ------------------------------------------------------------
 * Hints and the version cell's strings.
 * ------------------------------------------------------------ */
static void confirm_prompt(const Store *s, char *out, size_t outsz) {
    snprintf(out, outsz, "Remove %s from %s?  y = remove, n = cancel",
             s->confirm_id, STORE_ROOT_APPS);
}

static void hint_for(const Store *s, int sel, char *out, size_t outsz) {
    out[0] = 0;
    if (s->confirm_id[0]) {
        confirm_prompt(s, out, outsz);
        return;
    }
    if (s->search_focus) {
        snprintf(out, outsz,
                 "typing filters the list - Enter keeps it - Ctrl+U clears it");
        return;
    }
    if (sel < 0 || sel >= s->ndisp) {
        snprintf(out, outsz, "r: fetch the index - U: check for updates - /: search");
        return;
    }

    const Disp *d = &s->disp[sel];
    if (d->kind == D_APP) {
        const StoreApp *a = &s->inst[d->idx];
        int p = find_pkg(s, a->id);
        char vnote[80];
        vnote[0] = 0;
        if (!a->version[0] && p >= 0 && s->avail[p].version[0])
            snprintf(vnote, sizeof(vnote),
                     " - manifest has no version = (~%s is the index's)",
                     s->avail[p].version);

        if (a->in_root && a->in_image) {
            char hb[24];
            human_size(a->root_total, hb, sizeof(hb));
            snprintf(out, outsz,
                     "Enter: launch (image copy) - x removes the %s %s copy%s",
                     hb, STORE_ROOT_APPS, vnote);
        } else if (a->in_root) {
            if (has_update(s, a)) {
                char rb[40];
                rb[0] = 0;
                if (s->avail[p].have_size) {
                    char hb[24];
                    human_size(s->avail[p].size, hb, sizeof(hb));
                    snprintf(rb, sizeof(rb), " (%s on the repo)", hb);
                }
                snprintf(out, outsz,
                         "Enter: launch - x: remove - i: update to %s%s%s",
                         s->avail[p].version, rb, vnote);
            } else {
                snprintf(out, outsz, "Enter: launch - x: remove from %s%s",
                         STORE_ROOT_APPS, vnote);
            }
        } else {
            snprintf(out, outsz,
                     "Enter: launch - shipped with the image; x cannot remove it%s",
                     vnote);
        }
        return;
    }

    if (d->kind == D_PKG) {
        const Pkg *k = &s->avail[d->idx];
        int ii = find_inst(s, k->name);
        if (ii < 0) {
            if (k->have_size) {
                char hb[24];
                human_size(k->size, hb, sizeof(hb));
                if (k->version[0])
                    snprintf(out, outsz, "Enter: install %s %s (%s on the repo)",
                             k->name, k->version, hb);
                else
                    snprintf(out, outsz, "Enter: install %s (%s on the repo)",
                             k->name, hb);
            } else {
                snprintf(out, outsz, "Enter: install %s", k->name);
            }
            return;
        }
        const StoreApp *a = &s->inst[ii];
        if (a->in_image) {
            snprintf(out, outsz,
                     "%s is installed from the image - Enter does nothing", k->name);
        } else if (a->version[0] && k->version[0] &&
                   strcmp(a->version, k->version)) {
            snprintf(out, outsz, "Enter or i: update %s %s -> %s",
                     k->name, a->version, k->version);
        } else {
            snprintf(out, outsz, "%s is installed - Enter does nothing", k->name);
        }
        return;
    }

    snprintf(out, outsz, "r: fetch the index - /: search");
}

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

/* a magnifier for the search box: a ring and a handle */
static void draw_search_icon(Surface *s, float x, float y, float sz, u32 c) {
    float r = sz * 0.28f;
    float cx = x + sz * 0.40f, cy = y + sz * 0.40f;
    gfx_rrect_aa(s, cx - r, cy - r, r * 2.0f, r * 2.0f, r, 0, c, sz * 0.10f);
    gfx_line(s, cx + r * 0.70f, cy + r * 0.70f,
                x + sz * 0.92f, y + sz * 0.92f, sz * 0.11f, c);
}

/* the clear mark at the right end of the search box */
static void draw_clear_icon(Surface *s, float cx, float cy, float sz, u32 c) {
    float r = sz * 0.26f;
    gfx_line(s, cx - r, cy - r, cx + r, cy + r, sz * 0.10f, c);
    gfx_line(s, cx + r, cy - r, cx - r, cy + r, sz * 0.10f, c);
}

/* One painted line. `x` is the window's origin (the painter is the only
 * side that adds it); `ry` is the row's top in the compositor's surface.
 * Every number comes from the Layout the hit-test also uses, so the row a
 * click lands on is this row - and each text line is centred in the row
 * with the face's own ascent (line_top()), not with a +1 or +2 fudge. */
/* One painted row. `x` is the window's origin; `ry` is the row's top on
 * the compositor's surface. Every number comes from the Layout the click
 * test also uses, so the row a click lands on is this row - and each text
 * line is centred in the row with its own face (row_dy / small_dy). */
static void paint_row(Surface *s, const Layout *L, const Theme *th,
                      const Store *st, int x, int w, int ry, int idx) {
    const Disp *d = &st->disp[idx];
    int selected = (idx == st->sel) && disp_selectable(d);
    int iy = ry + (L->row_h - L->icon_sz) / 2;

    if (selected)
        ui_pill(s, x + L->row_xpad, ry, w - 2 * L->row_xpad, L->row_h,
                (float)L->radius, color_alpha(th->accent, 0.20f));

    if (d->kind == D_SECT) {
        char label[64];
        snprintf(label, sizeof(label), "%s (%d)", d->msg, d->idx);
        if (d->mark == MARK_CHECK)
            draw_check_icon(s, (float)(x + L->m), (float)iy,
                            (float)L->icon_sz, th->accent, 0.85f);
        else
            draw_pkg_icon(s, (float)(x + L->m), (float)iy,
                          (float)L->icon_sz, th->accent, 0.85f);
        draw_clipped(s, L->f.small, (float)(x + L->name_x),
                     (float)(ry + L->small_dy),
                     (float)(w - L->m - L->name_x), label,
                     color_alpha(th->accent, 0.72f));
        return;
    }

    if (d->kind == D_MSG) {
        draw_clipped(s, L->f.small, (float)(x + L->name_x),
                     (float)(ry + L->small_dy),
                     (float)(w - L->m - L->name_x), d->msg,
                     color_alpha(th->text, 0.42f));
        return;
    }

    int small_y = ry + L->small_dy;
    int name_y = ry + L->row_dy;
    int name_x = x + L->name_x;
    int name_w = L->name_w;

    char ver[64];
    int vkind = version_cell(st, d->kind, d->idx, ver, sizeof(ver));
    u32 vcol = color_alpha(th->text, 0.50f);
    if (vkind == VC_UPDATE) vcol = color_alpha(th->accent, 0.90f);
    else if (vkind == VC_INDEX) vcol = color_alpha(th->text, 0.38f);
    else if (vkind == VC_NONE) vcol = color_alpha(th->text, 0.26f);

    char sz[32];
    long long size = 0;
    int have_size = 0;
    int dim_name = 0;

    if (d->kind == D_APP) {
        const StoreApp *e = &st->inst[d->idx];
        draw_check_icon(s, (float)(x + L->m), (float)iy,
                        (float)L->icon_sz, th->accent,
                        e->have_size ? 0.90f : 0.55f);
        size = e->total;
        have_size = e->nfiles > 0;
        dim_name = !e->have_size;
    } else {
        const Pkg *k = &st->avail[d->idx];
        int ins = is_installed(st, k->name);
        if (ins)
            draw_check_icon(s, (float)(x + L->m), (float)iy,
                            (float)L->icon_sz, th->accent, 0.60f);
        else
            draw_pkg_icon(s, (float)(x + L->m), (float)iy,
                          (float)L->icon_sz, th->accent, selected ? 1.0f : 0.85f);
        size = k->have_size ? k->size : -1;
        have_size = k->have_size;
        dim_name = ins;
    }

    if (have_size) human_size(size, sz, sizeof(sz));
    else           snprintf(sz, sizeof(sz), "-");

    if (L->show_ver && ver[0])
        ui_text_right(s, L->f.small, x + L->ver_right, small_y, ver, vcol);
    if (L->show_size)
        ui_text_right(s, L->f.small, x + L->size_right, small_y, sz,
                      color_alpha(th->text, have_size ? 0.50f : 0.30f));

    const char *nm = (d->kind == D_APP) ? st->inst[d->idx].name
                                       : st->avail[d->idx].name;
    draw_clipped(s, L->f.body, (float)name_x, (float)name_y, (float)name_w, nm,
                 dim_name ? color_alpha(th->text, 0.55f)
                          : (selected ? th->text : color_alpha(th->text, 0.86f)));
}

static void store_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Store *st = a->state;
    if (!st || !th) return;
    st->w = w;
    st->h = h;
    if (w < 1 || h < 1) return;

    /* bring the two live sources up to date before laying them out */
    poll_install(st);
    sync_index(st);

    Layout L;
    if (!layout_store(&L, st, w, h)) return;

    /* ---- header: the app's name, the Refresh action ---- */
    surf_blend(s, x, y, w, L.header_h, th->panel);
    font_draw(s, L.f.title, (float)(x + L.title_x), (float)(y + L.title_dy),
              "Apps", th->text);

    if (L.btn_w > 0) {
        int hot = st->px >= L.btn_x && st->px < L.btn_x + L.btn_w &&
                  st->py >= L.btn_y && st->py < L.btn_y + L.btn_h;
        ui_button(s, x + L.btn_x, y + L.btn_y, L.btn_w, L.btn_h,
                  (float)L.radius, "Refresh", L.f.small, hot, th);
    }
    surf_blend(s, x, y + L.header_h - L.rule, w, L.rule,
               color_alpha(th->panel_border, 0.80f));

    /* ---- the search field ---- */
    if (L.search_h > 0) {
        int by = y + L.se_y;
        gfx_rrect_aa(s, (float)(x + L.se_x), (float)by, (float)L.se_w,
                     (float)L.se_h, (float)L.radius,
                     color_alpha(th->panel_border, 0.28f),
                     st->search_focus ? color_alpha(th->accent, 0.80f)
                                      : color_alpha(th->panel_border, 0.55f),
                     1.0f);
        draw_search_icon(s, (float)(x + L.se_x + SP(1)),
                         (float)by + ((float)L.se_h - (float)L.se_icon) * 0.5f,
                         (float)L.se_icon, color_alpha(th->text, 0.50f));

        float tx = (float)(x + L.se_text_x);
        float ty = (float)by + (float)L.se_dy;
        float avail = (float)(x + L.se_clear_x) - tx - (float)L.gap;
        if (avail < 8.0f) avail = 8.0f;

        if (st->filter[0]) {
            draw_tail(s, L.f.body, tx, ty, avail, st->filter, th->text);
            if (st->search_focus) {
                float tw = font_width(L.f.body, st->filter);
                float cx = tx + (tw <= avail ? tw : avail) + 2.0f;
                if (cx > (float)(x + L.se_clear_x) - 2.0f)
                    cx = (float)(x + L.se_clear_x) - 2.0f;
                gfx_line(s, cx, (float)by + (float)SP(1), cx,
                         (float)(by + L.se_h - SP(1)), 1.4f, th->accent);
            }
        } else {
            const char *ph = st->search_focus ? "type to filter"
                                              : "press / to search";
            draw_clipped(s, L.f.body, tx, ty, avail, ph,
                         color_alpha(th->text, st->search_focus ? 0.50f : 0.32f));
            if (st->search_focus)
                gfx_line(s, tx + font_width(L.f.body, ph) + 2.0f,
                         (float)by + (float)SP(1),
                         tx + font_width(L.f.body, ph) + 2.0f,
                         (float)(by + L.se_h - SP(1)), 1.4f, th->accent);
        }

        if (st->filter[0])
            draw_clear_icon(s,
                            (float)(x + L.se_clear_x + L.se_clear_w / 2),
                            (float)by + (float)L.se_h * 0.5f,
                            (float)L.se_icon, color_alpha(th->text, 0.55f));
    }

    /* ---- rows; the selection is kept inside the scroll window ---- */
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

    /* ---- one status line; while an install runs, its progress strip ---- */
    if (L.footer_h > 0) {
        int fy = y + h - L.footer_h;
        surf_blend(s, x, fy, w, L.footer_h, th->panel);

        if (st->pending[0]) {
            float frac = 0.0f;
            if (st->pending_phase == 2) frac = 1.0f;
            else if (st->pending_phase == 1 && st->pending_total > 0) {
                frac = (float)((double)st->pending_part / (double)st->pending_total);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 0.995f) frac = 0.995f;
            }
            int trackw = w;
            surf_blend(s, x, fy, trackw, L.prog_h,
                       color_alpha(th->accent, 0.14f));
            int fw = (int)((float)trackw * frac);
            if (fw > 0)
                surf_blend(s, x, fy, fw, L.prog_h,
                           color_alpha(th->accent, 0.70f));
        } else {
            surf_blend(s, x, fy, w, L.rule, th->panel_border);
        }

        int ty = fy + L.footer_dy;

        char right[64];
        snprintf(right, sizeof(right), "%d installed - %d available",
                 st->ninst, st->navail);
        float rw = font_width(L.f.small, right);
        int fit = (x + w - L.m) - (x + L.m) > (int)rw + 2 * L.gap;
        if (fit)
            ui_text_right(s, L.f.small, x + w - L.m, ty, right,
                          color_alpha(th->text, 0.40f));

        char prompt[192];
        prompt[0] = 0;
        if (st->confirm_id[0]) confirm_prompt(st, prompt, sizeof(prompt));

        const char *left = prompt[0] ? prompt
                         : st->status[0] ? st->status
                         : (st->error[0] ? st->error : 0);
        u32 col = prompt[0] ? color_alpha(th->accent, 0.95f)
                : st->status[0] ? color_alpha(th->accent, 0.62f)
                : th->accent;
        if (!left) {
            hint_for(st, st->sel, prompt, sizeof(prompt));
            left = prompt;
            col = color_alpha(th->text, 0.44f);
        }
        int maxw = w - 2 * L.m;
        if (fit) maxw -= (int)rw + L.gap;
        draw_clipped(s, L.f.small, (float)(x + L.m), (float)ty, (float)maxw,
                     left, col);
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

static void filter_changed(Store *s) {
    s->filter_len = (int)strlen(s->filter);
    build_disp(s);
}

static void filter_clear(Store *s) {
    s->filter[0] = 0;
    s->filter_len = 0;
    filter_changed(s);
}

/* Arm the uninstall prompt for a bundle id. Only /root/apps is ever
 * removable; the caller has already checked that a root copy exists. */
static void arm_uninstall(Store *s, const StoreApp *a) {
    if (!safe_seg(a->id)) {
        snprintf(s->status, sizeof(s->status), "refusing an unsafe bundle name");
        return;
    }
    snprintf(s->confirm_id, sizeof(s->confirm_id), "%s", a->id);
    s->confirm_keep_image = a->in_image;
    s->search_focus = 0;
    snprintf(s->status, sizeof(s->status),
             "remove %s from %s? y = remove, n = cancel",
             a->id, STORE_ROOT_APPS);
}

static void store_key(App *a, const char *bytes, int n) {
    Store *s = a->state;
    if (!s || !bytes) return;

    Layout L;
    int rows = 1;
    if (layout_store(&L, s, s->w, s->h) && L.rows > 0) rows = L.rows;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        /* 1. an armed uninstall owns the keyboard. Only 'y' removes; any
         * other byte cancels, so holding Enter can never delete anything.
         * The rest of the batch is consumed too: the answer must not turn
         * into a second command. */
        if (s->confirm_id[0]) {
            if (c == 'y' || c == 'Y') {
                do_uninstall(s);
            } else {
                s->confirm_id[0] = 0;
                s->confirm_keep_image = 0;
                snprintf(s->status, sizeof(s->status), "uninstall cancelled");
            }
            return;
        }

        /* 2. escape runs: the same navigation in both modes */
        if (c == 0x1B) {
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

        /* 3. the search box: printable bytes edit the filter, Enter keeps
         * it, Ctrl+U clears it, Backspace deletes one byte. Nothing else
         * reaches the list while the box has focus. */
        if (s->search_focus) {
            if (c == '\n' || c == '\r' || c == '\t') {
                s->search_focus = 0;
                continue;
            }
            if (c == 0x08 || c == 0x7f) {
                if (s->filter_len > 0) {
                    s->filter[--s->filter_len] = 0;
                    filter_changed(s);
                }
                continue;
            }
            if (c == 0x15) {                    /* Ctrl+U */
                filter_clear(s);
                continue;
            }
            if (c >= 0x20 && c < 0x7f) {
                if (s->filter_len < (int)sizeof(s->filter) - 1) {
                    s->filter[s->filter_len++] = (char)c;
                    s->filter[s->filter_len] = 0;
                    filter_changed(s);
                }
                continue;
            }
            continue;
        }

        /* 4. commands */
        switch (c) {
            case 'j': move_sel(s, +1); break;
            case 'k': move_sel(s, -1); break;
            case 'g': sel_first(s); break;
            case 'G': sel_last(s); break;
            case 0x01: sel_first(s); break;                         /* Ctrl+A */
            case 0x05: sel_last(s); break;                          /* Ctrl+E */

            case '/':
                s->search_focus = 1;
                break;

            case 0x15:                                              /* Ctrl+U */
                filter_clear(s);
                break;

            case 0x08: case 0x7f:
                if (s->filter_len > 0) {
                    s->filter[--s->filter_len] = 0;
                    filter_changed(s);
                }
                break;

            case 'r': request_refresh(s); break;

            case 'U':
                check_updates(s);
                break;

            case 'x': case 'X': {
                if (s->sel < 0 || s->sel >= s->ndisp) break;
                Disp *d = &s->disp[s->sel];
                if (d->kind == D_APP) {
                    const StoreApp *e = &s->inst[d->idx];
                    if (!e->in_root)
                        snprintf(s->status, sizeof(s->status),
                                 "%s is in %s - only %s bundles are removable",
                                 e->id, STORE_APP_DIR, STORE_ROOT_APPS);
                    else
                        arm_uninstall(s, e);
                } else if (d->kind == D_PKG) {
                    int ii = find_inst(s, s->avail[d->idx].name);
                    if (ii >= 0 && s->inst[ii].in_root) arm_uninstall(s, &s->inst[ii]);
                    else snprintf(s->status, sizeof(s->status),
                                  "%s has no copy in %s",
                                  s->avail[d->idx].name, STORE_ROOT_APPS);
                }
                break;
            }

            case 'i': case 'I': {
                if (s->sel < 0 || s->sel >= s->ndisp) break;
                Disp *d = &s->disp[s->sel];
                int p = -1;
                if (d->kind == D_PKG) p = d->idx;
                else if (d->kind == D_APP) p = find_pkg(s, s->inst[d->idx].id);
                if (p < 0)
                    snprintf(s->status, sizeof(s->status),
                             "the index has no package for this row");
                else
                    start_install(s, &s->avail[p]);
                break;
            }

            case '\n':
            case '\r': activate(s); break;
            default: break;
        }
    }
}

/* The window's wheel hook (AppVT.wheel). Positive notches scroll up, as
 * the desktop delivers them. The view moves by whole rows, and the
 * selection is pulled to the nearest selectable row when the view passes
 * it, so it can never be scrolled out of sight. */
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

    int lo = s->scroll;
    int hi = s->scroll + L.rows - 1;
    if (hi >= s->ndisp) hi = s->ndisp - 1;
    int any = 0;
    for (int i = lo; i <= hi && !any; i++)
        if (disp_selectable(&s->disp[i])) any = 1;
    if (!any) { s->scroll = old; return; }

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
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px = x;
    s->py = y;

    /* an armed uninstall is dismissed by any click; the click must not
     * also act on a row behind the prompt */
    if (s->confirm_id[0]) {
        s->confirm_id[0] = 0;
        s->confirm_keep_image = 0;
        snprintf(s->status, sizeof(s->status), "uninstall cancelled");
        return;
    }

    Layout L;
    if (!layout_store(&L, s, s->w, s->h)) return;

    /* the Refresh button: its rectangle and the pointer are both app-local */
    if (L.btn_w > 0 && x >= L.btn_x && x < L.btn_x + L.btn_w &&
        y >= L.btn_y && y < L.btn_y + L.btn_h) {
        request_refresh(s);
        return;
    }

    /* the search field owns its whole band; its clear mark is a target */
    if (L.search_h > 0 && y >= L.se_y && y < L.se_y + L.se_h &&
        x >= L.se_x && x < L.se_x + L.se_w) {
        if (s->filter_len > 0 &&
            x >= L.se_clear_x && x < L.se_clear_x + L.se_clear_w) {
            filter_clear(s);
            s->search_focus = 1;
            return;
        }
        s->search_focus = 1;
        return;
    }

    if (L.rows < 1) return;

    /* Only the row band owns rows. */
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
}

static void store_init(App *a, int w, int h) {
    Store *s = calloc(1, sizeof(Store));
    if (!s) return;
    s->w = w;
    s->h = h;
    s->sel = -1;
    s->index_size = -1;
    s->pending_total = -1;
    s->pending_part = -1;
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
