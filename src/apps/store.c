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
 *  description.  Typing filters live; Enter keeps the filter and leaves
 *  the box; Ctrl+U clears it; Backspace edits it.  Escape clears the
 *  filter and leaves the box - the desktop treats Esc as a cancel key
 *  and never closes a window with it (src/main.c).
 *
 *  UNINSTALL - 'x' on an Installed row asks first; only 'y' removes.
 *  It deletes /root/apps/<id> and NOTHING else: the path is built from
 *  the validated id, the walk uses lstat() and never follows a symlink,
 *  and a bundle that lives only in /usr/share/mystic/apps (the image)
 *  has nothing in /root to remove. The confirmation names the id, so a
 *  selection that moves underneath the prompt cannot change what is
 *  removed. After a successful removal the window writes
 *  /run/mystic/rescan.request: src/wmsrv.c takes that file and rescans
 *  both roots, so the removed tile disappears from the launcher instead
 *  of staying listed and dead until the next boot.
 *
 *  VEreviewONS - the installed version is the manifest's "version =" line.
 *  A bundle pkg installed under a different id than the package records
 *  the package in <bundle>/.pkg-name; that is how a package whose
 *  directory name differs from its index name is matched to its row. The
 *  ACTIVE copy is the one src/wmsrv.c would launch: a runnable /root/apps
 *  copy wins over the image's, so its version and size are the ones shown.
 *  When a manifest has no version line the index's version is shown with a
 *  '~' in front, so the reader can tell whose number it is. "1.0>1.1"
 *  means the installed manifest and the index disagree. 'U' compares every
 *  installed bundle against the index and reports real counts in the
 *  footer (available updates, image-only bundles that differ - pkg cannot
 *  write the image - and manifests with no version line); it also asks the
 *  desktop for a fresh index, because comparing against a stale cache
 *  would be a guess.
 *
 *  UPDATE INSTALL - Enter/i on an Available row asks the desktop to run
 *  `pkg install <name>` unless a copy pkg really installed under
 *  /root/apps is already at the index version. install_action() is the one
 *  predicate both keys use, so Enter and 'i' can never disagree again.
 *  It answers "install" for: no bundle; a manifest with no version line;
 *  a bundle that lives only in the read-only image root (pkg never writes
 *  there); a manifest that says 'placeholder = yes' (the image's vlc
 *  bundle: the player compiled statically, so it cannot dlopen and link
 *  libVLC at all); and an older /root version.
 *  Only a versioned, non-placeholder /root copy at the index version
 *  answers "already at version N in /root/apps" and writes nothing.
 *  When the only copy is the image's, the install is offered and pkg
 *  writes /root/apps/<bundle>; src/wmsrv.c prefers a runnable installed
 *  copy, so the new payload is what the launcher starts. The row's mark
 *  follows the same rule: a parcel means Enter installs, a tick means pkg
 *  installed it under /root/apps.
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
 *  The TEXT never says 100% before the bundle exists: a finished download
 *  is not an installed package.  The phases are all decided by stat():
 *  .part present -> downloading; .part gone but <file> present -> the
 *  tarball is cached (or finished), which proves the download is complete,
 *  so pkg is verifying/unpacking and the strip is full while the label
 *  says exactly that; neither -> pkg has not started (it may still be
 *  fetching the index).  No phase is fabricated: if the file column or the
 *  size column is missing, the window says "waiting" or shows bytes
 *  without a percentage instead of a made-up bar.
 *
 *  Three properties of the shipped pkg this window depends on: it resumes a
 *  .part with wget -c (so a percentage that starts at 15% and climbs is a
 *  resumed transfer, not a lie), it verifies the final size and sha256 while
 *  the bytes are still named .part (nothing is promoted before it matches the
 *  signed index), and it deletes the .part on a mismatch - which is why the
 *  strip returns to "waiting" instead of sitting at 99% forever after a bad
 *  download.
 *
 *  A TRANSFER THAT IS STILL GROWING IS NOT STUCK. The deadline is idleness on
 *  that .part (STORE_INSTALL_STALL_SEC), with STORE_INSTALL_HARD_SEC as the
 *  only wall-clock backstop, because a 573 MB package over a slow link can
 *  take an hour and used to be declared dead mid-download at a fixed 900 s.
 *
 *  When is it INSTALLED? Not when a manifest merely exists: an update
 *  starts with the old manifest already there. The request remembers the
 *  index version and the old manifest's mtime; the poll succeeds only
 *  when the manifest pkg writes in /root/apps carries the requested
 *  version, or its mtime differs from the baseline. The image root is
 *  not evidence (pkg never writes there). Until then the window keeps
 *  saying "installing", and at the timeout it names the fact it saw
 *  ("still says version 1.0, not 1.1", "the download stopped at ...",
 *  "no download appeared in /tmp/pkg") rather than inventing a result.
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
 *  MANIFEST VEreviewON LINE - this file only READS "version =" and
 *  "placeholder ="; it writes neither. stage/usr/share/mystic/apps/vlc/
 *  manifest carries 'version = 3.0.23' (equal to the index row, kept in
 *  step by tools/fetch-vlc.py, which refuses to pack if they disagree)
 *  and 'placeholder = yes', because the image's copy is the static
 *  launcher without libVLC. The manifest the package installs is the same
 *  file with 'placeholder = no'. wmsrv.c ignores both keys; pkg copies
 *  the manifest it unpacks. A manifest with no version line is not an
 *  error here - the window says so ('~' in the version cell) and treats
 *  the bundle as unproven, so Enter installs instead of claiming it is
 *  current.
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
#include <fcntl.h>
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

/* How long an install may run without ANY byte arriving before the window
 * stops claiming it is running. The deadline is not a stopwatch started when
 * the request was written: a 573 MB package (web) or a 38.8 MB one (vlc) over a
 * slow link is healthy while /tmp/pkg/<file>.part is still growing, and
 * killing the row at a fixed 900 s offered it again mid-transfer. So while the
 * download file is growing the test is idleness (STALL), and HARD is only the
 * backstop for a transfer that trickles forever. Both numbers are wall clock
 * seconds, measured with CLOCK_MONOTONIC. */
#ifndef STORE_INSTALL_TIMEOUT_SEC
#define STORE_INSTALL_TIMEOUT_SEC 900.0
#endif
#ifndef STORE_INSTALL_STALL_SEC
#define STORE_INSTALL_STALL_SEC 300.0
#endif
#ifndef STORE_INSTALL_HARD_SEC
#define STORE_INSTALL_HARD_SEC 7200.0
#endif

#define DESC_LINE 256
#define PKG_MAX 64              /* repo rows we hold; the index is small */
#define INST_MAX (2 * WM_MAX_APPS)   /* both bundle roots together */
#define DISP_MAX (INST_MAX + PKG_MAX + 6)

/* A directory entry's name we are willing to build a path from. The
 * uninstall walk refuses anything with a slash or a dot-dot, so the only
 * path it can ever remove is STORE_ROOT_APPS/<one safe segment>. */
#define SEG_MAX 32
#define VEreviewON_MAX 24

/* Rows the wheel moves per notch. A count, not a pixel size: it moves
 * whole entries, so it follows the text scale for free. */
#define WHEEL_ROWS 3

typedef struct {
    char id[SEG_MAX];
    char pkg[SEG_MAX + 1];       /* package that installed it (.pkg-name), "" for image */
    char name[64];
    char desc[DESC_LINE];
    char exe[128];
    char version[VEreviewON_MAX];   /* manifest "version =", "" when absent */
    long long total;             /* bytes of the ACTIVE bundle tree */
    long long root_total;        /* bytes under /root/apps/<id>, 0 if none */
    int  nfiles;                 /* regular files in the active tree */
    int  have_size;              /* the exec was stat()able: the row can launch */
    int  in_image;               /* a copy in STORE_APP_DIR */
    int  in_root;                /* a copy in STORE_ROOT_APPS (pkg's root) */
    int  placeholder;            /* manifest 'placeholder = yes': not the payload */
} StoreApp;

typedef struct {
    char name[SEG_MAX + 1];      /* pkg names may be 32 chars + NUL */
    char version[VEreviewON_MAX];
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
    char     pending[SEG_MAX + 1];/* package asked for, until its bundle proves it */
    char     pending_version[VEreviewON_MAX]; /* index version at request time */
    char     pending_bundle[SEG_MAX];      /* bundle found when the request was written */
    char     pending_file[64];    /* tarball name from the index row */
    long long pending_total;      /* index bytes, -1 when the row has none */
    long long pending_part;       /* last .part size seen, -1 when absent */
    int       pending_phase;      /* 0 waiting, 1 downloading, 2 verifying */
    int       pending_had;        /* a manifest for this package existed at start */
    long long pending_mtime;      /* ...and its mtime then (seconds since epoch) */
    double    pending_since;      /* CLOCK_MONOTONIC when the request was written,
                                     0 when nothing is in flight */
    long long pending_grow_size;  /* .part size when progress was last seen */
    double    pending_grow_at;    /* CLOCK_MONOTONIC of that moment */

    time_t   index_check_at; /* last stat() of the repo index (1 s gate) */
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

/* One line of a small file, newline stripped. 1 = a non-empty line. */
static int first_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)n, f)) { fclose(f); return 0; }
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    nl = strchr(out, '\r');
    if (nl) *nl = 0;
    fclose(f);
    return out[0] != 0;
}

/* The "version =" line of any manifest, parsed the same way read_manifest()
 * parses it. 1 = the key was there (out then holds its value, possibly ""). */
static int manifest_version(const char *path, char *out, size_t outsz) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    int got = 0;
    while (!got && fgets(line, sizeof(line), f)) {
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
        if (strcmp(k, "version")) continue;
        snprintf(out, outsz, "%s", v);
        got = 1;
    }
    fclose(f);
    return got;
}

/* Whole seconds: the target's struct stat has st_mtim, the host harness'
 * does not, and pkg replaces a manifest that was written long before the
 * request (or writes a new one), so a change of mtime is still visible at
 * this resolution. Version equality is the primary rule anyway. */
static long long stat_mtime(const struct stat *st) {
    return (long long)st->st_mtime;
}

/* Where pkg put the bundle for package `pkg`. Usually <root>/<pkg>; when the
 * package's manifest names another bundle id, stage/usr/bin/pkg records the
 * package in <bundle>/.pkg-name, and that is what makes the package visible
 * under a directory that is not its own name. For vlc the two are the same
 * word, so the directory rule alone finds it. Fills the directory name, the
 * manifest path and its mtime; 0 when no bundle for this package is on disk
 * yet. */
static int pkg_manifest(const char *pkg, char *bundle, size_t bn,
                        char *man, size_t mn, long long *mtime) {
    if (!pkg || !pkg[0]) return 0;

    struct stat st;
    char path[512];

    /* The .pkg-name record is authoritative: an older pkg may have left a
     * directory under the package's own name, and the bundle actually
     * installed by the current one is the one whose .pkg-name says so. */
    DIR *d = opendir(STORE_ROOT_APPS);
    if (d) {
        int found = 0;
        struct dirent *de;
        while (!found && (de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!safe_seg(de->d_name)) continue;

            char owner[SEG_MAX + 1];
            snprintf(path, sizeof(path), "%s/%s/.pkg-name",
                     STORE_ROOT_APPS, de->d_name);
            if (!first_line(path, owner, sizeof(owner))) continue;
            if (strcmp(owner, pkg)) continue;

            snprintf(path, sizeof(path), "%s/%s/manifest",
                     STORE_ROOT_APPS, de->d_name);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (bundle) snprintf(bundle, bn, "%s", de->d_name);
            if (man)    snprintf(man, mn, "%s", path);
            if (mtime)  *mtime = stat_mtime(&st);
            found = 1;
        }
        closedir(d);
        if (found) return 1;
    }

    /* Fallback for a bundle installed by a pkg that did not write
     * .pkg-name (or an image-era hand install): the directory named after
     * the package. */
    if (safe_seg(pkg)) {
        snprintf(path, sizeof(path), "%s/%s/manifest", STORE_ROOT_APPS, pkg);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (bundle) snprintf(bundle, bn, "%s", pkg);
            if (man)    snprintf(man, mn, "%s", path);
            if (mtime)  *mtime = stat_mtime(&st);
            return 1;
        }
    }
    return 0;
}

/* Nothing in flight: every field poll_install() reads goes back to empty. */
static void pending_clear(Store *s) {
    s->pending[0] = 0;
    s->pending_version[0] = 0;
    s->pending_bundle[0] = 0;
    s->pending_file[0] = 0;
    s->pending_total = -1;
    s->pending_part = -1;
    s->pending_phase = 0;
    s->pending_had = 0;
    s->pending_mtime = 0;
    s->pending_since = 0.0;
    s->pending_grow_size = -1;
    s->pending_grow_at = 0.0;
}

/* ------------------------------------------------------------
 * Manifest parsing - identical rules to wms_scan_apps() in
 * src/wmsrv.c: strip the newline, split on the first '=', trim
 * spaces around the key, take the value verbatim. "version" and
 * "placeholder" are the two keys wmsrv.c ignores and this window needs;
 * a missing line is "" and 0.
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
        else if (!strcmp(k, "placeholder"))
            a->placeholder = (!strcmp(v, "yes") || !strcmp(v, "1") ||
                              !strcmp(v, "true"));
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

/* The installed bundle that came from index package `name`. The directory
 * name is usually the package name, but pkg may have been told a different
 * bundle id (the manifest's 'bundle =' line) and records the user in
 * .pkg-name. The exact directory still wins when it exists. */
static int find_inst_pkg(const Store *s, const char *name) {
    /* .pkg-name is the authoritative owner, so a stale directory that
     * happens to carry the package's own name cannot shadow the bundle pkg
     * really installed for it (pkg_manifest() makes the same choice). */
    for (int i = 0; i < s->ninst; i++)
        if (s->inst[i].pkg[0] && !strcmp(s->inst[i].pkg, name)) return i;
    return find_inst(s, name);
}

/* The index row for an installed bundle: by the package recorded in
 * .pkg-name, else by its own directory name. */
static int find_pkg_for_app(const Store *s, const StoreApp *a) {
    int p = a->pkg[0] ? find_pkg(s, a->pkg) : -1;
    return p >= 0 ? p : find_pkg(s, a->id);
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

/* pkg records the package that created a bundle in <bundle>/.pkg-name, so a
 * package whose index name differs from its bundle directory (a manifest
 * 'bundle =' line) can still be found by name. */
static void read_pkg_name(const char *dir, const char *id, char *out, size_t n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/.pkg-name", dir, id);
    if (!first_line(path, out, n)) out[0] = 0;
}

static void add_bundle(Store *s, const char *dir, const char *id, int from_root) {
    if (!id[0] || strlen(id) >= sizeof(s->inst[0].id)) return;
    if (strchr(id, '/')) return;

    StoreApp tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.id, sizeof(tmp.id), "%s", id);
    /* a directory in the shared image root without a manifest is not an
     * app; /root/apps is pkg's own root, so list whatever it put there */
    int has_manifest = read_manifest(dir, &tmp);
    if (!has_manifest && !from_root) return;
    if (from_root) {
        tmp.in_root = 1;
        read_pkg_name(dir, id, tmp.pkg, sizeof(tmp.pkg));
        if (!tmp.desc[0])
            snprintf(tmp.desc, sizeof(tmp.desc), "installed by pkg");
    } else {
        tmp.in_image = 1;
    }
    probe_bundle(dir, &tmp);
    if (from_root) tmp.root_total = tmp.total;

    for (int i = 0; i < s->ninst; i++) {
        if (strcmp(s->inst[i].id, id)) continue;
        StoreApp *cur = &s->inst[i];
        cur->in_root |= tmp.in_root;
        cur->in_image |= tmp.in_image;

        if (tmp.in_root) {
            cur->root_total = tmp.root_total;
            if (tmp.pkg[0]) snprintf(cur->pkg, sizeof(cur->pkg), "%s", tmp.pkg);
        }

        /* src/wmsrv.c launches the copy that can actually run, and when both
         * can run the INSTALLED one wins. Show the facts of that copy, so the
         * version this window compares with the index is the version the
         * launcher would start. */
        int take = 0;
        if (tmp.in_root) {
            if (tmp.have_size) take = 1;
            else if (!cur->in_image) take = 1;   /* root-only bundle */
        } else if (!cur->have_size) {
            take = 1;
        }
        if (take) {
            int in_image = cur->in_image, in_root = cur->in_root;
            long long root_total = cur->root_total;
            char pkg[SEG_MAX + 1];
            snprintf(pkg, sizeof(pkg), "%s", cur->pkg);
            *cur = tmp;
            cur->in_image = in_image;
            cur->in_root = in_root;
            cur->root_total = root_total;
            snprintf(cur->pkg, sizeof(cur->pkg), "%s", pkg);
            if (tmp.in_root && tmp.pkg[0])
                snprintf(cur->pkg, sizeof(cur->pkg), "%s", tmp.pkg);
        }
        return;
    }
    if (s->ninst >= INST_MAX) return;
    s->inst[s->ninst++] = tmp;
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

/* A package counts as present for the ROW MARK when pkg installed it under
 * STORE_ROOT_APPS and the copy is not a placeholder. An image-only copy is
 * on the disk, but pkg cannot update it and Enter offers the install (see
 * install_action), so the parcel - "you can install this" - is the mark
 * that matches what the row's Enter key really does. */
static int pkg_in_root(const Store *s, const char *name) {
    int ii = find_inst_pkg(s, name);
    return ii >= 0 && s->inst[ii].in_root && !s->inst[ii].placeholder;
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
        int ii = find_inst_pkg(s, p->name);
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
    int p = find_pkg_for_app(s, a);
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

/* ------------------------------------------------------------
 * Install polling. Every number in this function comes from stat() on
 * the files pkg itself writes (stage/usr/bin/pkg):
 *
 *     /tmp/pkg/<file>.part  while wget is transferring
 *     /tmp/pkg/<file>       after the rename; sha256 then tar unpack
 *     /root/apps/<bundle>/manifest   only when the bundle is complete
 *
 * The percentage is downloaded/total where total is the index's size
 * column for that exact row; it is capped at 99 until the bundle is
 * really installed, because a downloaded tarball is not an installed
 * package.
 *
 * SUCCESS is not "a manifest exists": an UPDATE starts with the old
 * manifest already there, so that test reported "installed" one frame
 * after the request while pkg had not run and the old version was still
 * on disk. A package counts as installed only when the bundle pkg writes
 * in STORE_ROOT_APPS has
 *
 *   - the version the index row requested (pending_version, copied when
 *     the request was written), or
 *   - a manifest whose mtime differs from the one found when the request
 *     was written (pending_had/pending_mtime) - the proof for a package
 *     whose index row carries no usable version.
 *
 * The image root is deliberately NOT consulted: pkg never writes there,
 * and an image bundle with the same name would make a request look
 * finished without a single byte being installed. Anything that fails
 * both tests stays in flight until the timeout, which then says which
 * fact it actually observed.
 * ------------------------------------------------------------ */
static void poll_install(Store *s) {
    if (!s->pending[0]) return;

    char man[512] = "";
    char bundle[SEG_MAX] = "";
    long long man_mtime = 0;
    int found = pkg_manifest(s->pending, bundle, sizeof(bundle),
                             man, sizeof(man), &man_mtime);

    char seen[VEreviewON_MAX] = "";
    if (found)
        manifest_version(man, seen, sizeof(seen));

    int ver_ok = found && s->pending_version[0] && seen[0] &&
                 !strcmp(seen, s->pending_version);
    int mtime_changed = found && (!s->pending_had || man_mtime != s->pending_mtime);

    if (ver_ok || mtime_changed) {
        char done[SEG_MAX + 1];
        snprintf(done, sizeof(done), "%s", s->pending);
        char got[SEG_MAX];
        snprintf(got, sizeof(got), "%s", bundle[0] ? bundle : done);
        char gotver[VEreviewON_MAX];
        snprintf(gotver, sizeof(gotver), "%s", seen);

        pending_clear(s);
        scan_installed(s);

        long long bytes = 0;
        int files = 0;
        int ii = find_inst(s, got);
        if (ii < 0) ii = find_inst_pkg(s, done);
        if (ii >= 0) { bytes = s->inst[ii].total; files = s->inst[ii].nfiles; }

        char hb[24];
        human_size(bytes, hb, sizeof(hb));
        if (strcmp(got, done))
            snprintf(s->status, sizeof(s->status),
                     "installed %s as bundle %s%s%s - %d file%s, %s on disk",
                     done, got, gotver[0] ? " " : "", gotver,
                     files, files == 1 ? "" : "s", hb);
        else
            snprintf(s->status, sizeof(s->status),
                     "installed %s%s%s - %d file%s, %s on disk",
                     done, gotver[0] ? " " : "", gotver,
                     files, files == 1 ? "" : "s", hb);
        return;
    }

    /* still running: look at pkg's own cache */
    long long part = -1, tarball = -1;
    char path[512];
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

            /* Progress is the last moment the download file grew. A transfer
             * that is still adding bytes is not stuck, whatever the clock
             * says: only idleness (STALL) - or the HARD backstop - ends it. */
            if (part != s->pending_grow_size) {
                s->pending_grow_size = part;
                if (part >= 0) s->pending_grow_at = now;
            }
            double idle = now - s->pending_grow_at;
            if (idle < 0.0) idle = 0.0;
            int downloading = (s->pending_phase == 1);
            int expired = 0;
            if (waited >= STORE_INSTALL_HARD_SEC) expired = 1;
            else if (downloading) {
                if (idle >= STORE_INSTALL_STALL_SEC) expired = 1;
            } else if (waited >= STORE_INSTALL_TIMEOUT_SEC) {
                expired = 1;
            }

            if (expired) {
                /* The desktop forked pkg and the bundle never became the
                 * requested one: say which fact proves it, stop claiming
                 * progress, and leave the row installable again. */
                char why[128];
                if (downloading && idle >= STORE_INSTALL_STALL_SEC) {
                    char hp[24], ht[24];
                    human_size(part, hp, sizeof(hp));
                    if (s->pending_total > 0) {
                        human_size(s->pending_total, ht, sizeof(ht));
                        snprintf(why, sizeof(why),
                                 "no new bytes for %lds at %s of %s",
                                 (long)idle, hp, ht);
                    } else {
                        snprintf(why, sizeof(why),
                                 "no new bytes for %lds at %s", (long)idle, hp);
                    }
                } else if (found) {
                    if (seen[0] && s->pending_version[0])
                        snprintf(why, sizeof(why),
                                 "the installed manifest still says version %s, not %s",
                                 seen, s->pending_version);
                    else if (seen[0])
                        snprintf(why, sizeof(why),
                                 "the installed manifest still says version %s", seen);
                    else
                        snprintf(why, sizeof(why),
                                 "the installed manifest has no version = line");
                } else if (part >= 0) {
                    char hp[24], ht[24];
                    human_size(part, hp, sizeof(hp));
                    if (s->pending_total > 0) {
                        human_size(s->pending_total, ht, sizeof(ht));
                        snprintf(why, sizeof(why),
                                 "the download stopped at %s of %s", hp, ht);
                    } else {
                        snprintf(why, sizeof(why),
                                 "the download stopped at %s", hp);
                    }
                } else if (tarball >= 0) {
                    snprintf(why, sizeof(why),
                             "the tarball was downloaded but pkg did not unpack it");
                } else {
                    snprintf(why, sizeof(why),
                             "no download appeared in %s", STORE_PKG_CACHE);
                }
                snprintf(s->status, sizeof(s->status),
                         "install of %s did not finish in %lds - %s; Enter retries",
                         s->pending, secs, why);
                pending_clear(s);
                return;
            }

            char hpart[24], htotal[24];
            if (s->pending_phase == 1) {
                human_size(part, hpart, sizeof(hpart));
                if (s->pending_total > 0) {
                    human_size(s->pending_total, htotal, sizeof(htotal));
                    int pct = (int)((double)part * 100.0 / (double)s->pending_total);
                    if (pct < 0) pct = 0;
                    if (pct > 99) pct = 99;   /* 100% only when it is installed */
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
 *  settings.c: the design calls for one desktop, not three
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

/* ONE ELLIPSIS. Every string in this window is measured against the box it
 * has to live in; when it does not fit, the tail becomes a single U+2026 and
 * nothing is painted outside its column (or its window). Returned in a
 * caller buffer, so the painter and the probe can see the same string. */
#define ELLIPSIS "\xE2\x80\xA6"

static void fit_ellipsis(Font *f, const char *src, char *out, size_t cap,
                         float maxw) {
    char tmp[512];
    if (!cap) return;
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    out[0] = 0;
    if (!f || maxw <= 0.0f) return;
    if (font_width(f, tmp) <= maxw) {
        snprintf(out, cap, "%s", tmp);
        return;
    }
    /* drop whole UTF-8 characters, never half of one */
    size_t n = strlen(tmp);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) n--;
        tmp[n] = 0;
        char trybuf[600];
        snprintf(trybuf, sizeof(trybuf), "%s" ELLIPSIS, tmp);
        if (font_width(f, trybuf) <= maxw) {
            snprintf(out, cap, "%s", trybuf);
            return;
        }
    }
    /* even the ellipsis alone is wider than the box: draw nothing rather
     * than overflow it */
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
    int bar_w, thumb_min;      /* the list's scroll map, in the right margin */
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
    L->bar_w = ui_px(3);
    L->thumb_min = SP(2);
    L->row_xpad = L->m;

    L->row_h = ui_row_h(L->f.body);
    L->icon_sz = SP(2);
    if (L->icon_sz > L->row_h - SP(1)) L->icon_sz = L->row_h - SP(1);
    if (L->icon_sz < SP(1)) L->icon_sz = SP(1);

    L->header_h = ui_ceilf(font_height(L->f.title)) + 2 * SP(1);
    L->search_h = ui_ceilf(font_height(L->f.body)) + 2 * SP(1);
    L->footer_h = ui_ceilf(font_height(L->f.small)) + 2 * SP(1);

    /* A short window tightens before it gives anything up: the search
     * loses its extra vertical padding, then the footer its padding; only
     * then (never at a sane size) do those bands disappear, footer first.
     * The search band sits on one gap, not two, so count one. */
    if (L->header_h + L->search_h + L->gap + L->footer_h + L->row_h > h)
        L->search_h = L->row_h;
    if (L->header_h + L->search_h + L->gap + L->footer_h + L->row_h > h)
        L->footer_h = ui_ceilf(font_height(L->f.small));
    if (L->header_h + L->search_h + L->gap + L->footer_h + L->row_h > h)
        L->footer_h = 0;
    if (L->header_h + L->search_h + L->gap + L->row_h > h)
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
    L->list_y = L->header_h + (L->search_h ? L->search_h + L->gap : 0);
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
    /* The size column is as wide as the widest size this list will really
     * print, not as wide as a stand-in string: the number comes from the
     * remote index (Pkg.size) and can be any byte count. It is clamped and
     * then fitted below, so a hostile or wrong row can never paint over the
     * version cell. */
    {
        int smax = ui_ceilf(font_width(L->f.small, "999.9 MB"));
        for (int i = 0; i < st->ndisp; i++) {
            const Disp *d = &st->disp[i];
            char sz[48];
            long long bytes = -1;
            if (d->kind == D_APP) {
                bytes = st->inst[d->idx].total;
            } else if (d->kind == D_PKG) {
                const Pkg *k = &st->avail[d->idx];
                bytes = k->have_size ? k->size : -1;
            } else {
                continue;
            }
            if (bytes >= 0) human_size(bytes, sz, sizeof(sz));
            else snprintf(sz, sizeof(sz), "-");
            int sw = ui_ceilf(font_width(L->f.small, sz));
            if (sw > smax) smax = sw;
        }
        if (smax > SP(12)) smax = SP(12);
        L->size_col = smax;
    }
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
    char buf[512];
    fit_ellipsis(f, text, buf, sizeof(buf), maxw);
    if (buf[0]) font_draw(s, f, x, y, buf, color);
}

/* The end of a string - used by the search field, where the caret is at
 * the right and the interesting part of a long filter is its tail. */
static void draw_tail(Surface *s, Font *f, float x, float y, float maxw,
                      const char *text, u32 color) {
    if (!text || !text[0] || maxw < 4.0f) return;
    float full = font_width(f, text);
    if (full <= maxw) { font_draw(s, f, x, y, text, color); return; }
    float budget = maxw - font_width(f, ELLIPSIS);
    if (budget < 0.0f) return;
    const char *p = text;
    while (*p && font_width(f, p) > budget) p++;
    char buf[320];
    snprintf(buf, sizeof(buf), ELLIPSIS "%s", p);
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

/* Ask the desktop's window server to re-read both bundle roots. It polls
 * WM_RESCAN_REQ, takes the file before scanning, and redraws (src/wmsrv.c:
 * wms_apps_poll_rescan). pkg asks for this itself after install/remove; this
 * window has to ask after its own uninstall, or the launcher keeps a listed,
 * clickable tile for a directory that is gone until the next boot. Reports
 * the kernel's own reason when the request cannot be written. */
static int request_rescan(char *err, size_t errsz) {
    err[0] = 0;
    int fd = open(WM_RESCAN_REQ, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "%s: %s", WM_RESCAN_REQ, strerror(errno));
        return 0;
    }
    if (close(fd) != 0) {
        snprintf(err, errsz, "%s: %s", WM_RESCAN_REQ, strerror(errno));
        return 0;
    }
    return 1;
}

/* What Enter/i on a package row must do, decided in ONE place.
 *
 * "Already installed" is only true when a copy pkg wrote under
 * STORE_ROOT_APPS carries the index row's version. Everything else asks
 * pkg, because every other answer was a lie once:
 *
 *   - no bundle at all: install.
 *   - a manifest with no 'version =' line: the old test compared two empty
 *     strings as equal and answered "already installed" without writing a
 *     request. It cannot prove anything, so the request is written.
 *   - a bundle that exists only in the read-only image root: pkg never
 *     writes there, so no installed package exists to be current, and
 *     src/wmsrv.c prefers a runnable /root/apps copy over the image's.
 *     (The image's vlc bundle is exactly this case.)
 *   - 'placeholder = yes' in the manifest: the image ships that bundle
 *     with the statically linked player and no libVLC (musl's static
 *     dlopen is a stub), so even a matching version
 *     string describes a bundle that cannot play. Installing the package
 *     is what replaces it.
 *   - a /root copy whose version differs: an update.
 *
 * Only a versioned, pkg-installed, non-placeholder copy at the index
 * version answers ACT_CURRENT; that is the one answer that skips the
 * request. */
enum { ACT_INSTALL = 0, ACT_UPDATE, ACT_CURRENT };

static int install_action(const Store *s, const Pkg *k, int *ii_out) {
    int ii = find_inst_pkg(s, k->name);
    if (ii_out) *ii_out = ii;
    if (ii < 0) return ACT_INSTALL;

    const StoreApp *a = &s->inst[ii];
    if (!a->version[0] || !k->version[0]) return ACT_INSTALL;
    if (a->placeholder) return ACT_INSTALL;
    if (a->in_image && !a->in_root) return ACT_INSTALL;
    return !strcmp(a->version, k->version) ? ACT_CURRENT : ACT_UPDATE;
}

/* The status line when nothing needs installing: name the version and the
 * root it was read from, so "current" is a fact and not a reassurance. */
static void say_current(Store *s, const char *name, const StoreApp *a) {
    snprintf(s->status, sizeof(s->status),
             "%s is already at version %s in %s - nothing to install",
             name, a->version, STORE_ROOT_APPS);
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

    int ii = -1;
    if (install_action(s, k, &ii) == ACT_CURRENT) {
        say_current(s, k->name, &s->inst[ii]);
        return;
    }

    char err[128];

    /* The baseline for the "did the manifest change?" test, read BEFORE the
     * request is written: after that pkg may already be replacing it. If it
     * is absent, any manifest that appears later is new. */
    pending_clear(s);
    {
        char bundle[SEG_MAX], man[512];
        long long mt = 0;
        if (pkg_manifest(k->name, bundle, sizeof(bundle), man, sizeof(man), &mt)) {
            s->pending_had = 1;
            s->pending_mtime = mt;
            snprintf(s->pending_bundle, sizeof(s->pending_bundle), "%s", bundle);
        }
    }

    if (!write_line(STORE_INSTALL_REQUEST, k->name, err, sizeof(err))) {
        pending_clear(s);
        snprintf(s->status, sizeof(s->status), "%s", err);
        return;
    }

    snprintf(s->pending, sizeof(s->pending), "%s", k->name);
    snprintf(s->pending_version, sizeof(s->pending_version), "%s", k->version);
    snprintf(s->pending_file, sizeof(s->pending_file), "%s", k->file);
    s->pending_total = k->have_size ? k->size : -1;

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        s->pending_since = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        s->pending_grow_at = s->pending_since;
    }

    if (ii >= 0 && s->inst[ii].in_image &&
        (!s->inst[ii].in_root || s->inst[ii].placeholder))
        snprintf(s->status, sizeof(s->status),
                 "installing %s to %s - it will replace the image's copy in the launcher...",
                 k->name, STORE_ROOT_APPS);
    else if (ii >= 0) {
        if (s->pending_bundle[0] && strcmp(s->pending_bundle, k->name))
            snprintf(s->status, sizeof(s->status),
                     "updating %s (bundle %s) to %s...", k->name,
                     s->pending_bundle,
                     k->version[0] ? k->version : "the index version");
        else
            snprintf(s->status, sizeof(s->status), "updating %s to %s...",
                     k->name, k->version[0] ? k->version : "the index version");
    } else
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
        int ii = -1;
        if (install_action(s, k, &ii) == ACT_CURRENT) {
            say_current(s, k->name, &s->inst[ii]);
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
        int p = find_pkg_for_app(s, a);
        if (p < 0 || !s->avail[p].version[0]) continue;
        if (!strcmp(a->version, s->avail[p].version)) continue;
        if (a->in_root) {
            upd++;
            if (!first[0])
                snprintf(first, sizeof(first), ": %s %s>%s", a->id,
                         a->version, s->avail[p].version);
        } else if (a->in_image) {
            img++;
        }
    }

    int fresh = request_refresh(s);     /* this rescans the disk too */

    char msg[192];
    snprintf(msg, sizeof(msg),
             "checked %d installed against %d in the index: %d update%s%s%s%s",
             s->ninst, s->navail, upd, upd == 1 ? "" : "s", first,
             img ? " - image copies differ; installing adds a /root copy that wins" : "",
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

    char rerr[128];
    int rescanned = request_rescan(rerr, sizeof(rerr));
    if (rescanned)
        snprintf(s->status, sizeof(s->status), "removed %s - %d file%s, %s freed%s",
                 id, files, files == 1 ? "" : "s", hb,
                 keep_image ? " (the image copy stays)" : "");
    else
        snprintf(s->status, sizeof(s->status),
                 "removed %s - %d file%s, %s freed%s - rescan request failed: %s",
                 id, files, files == 1 ? "" : "s", hb,
                 keep_image ? " (the image copy stays)" : "", rerr);
}

/* ------------------------------------------------------------
 * Hints and the version cell's strings.
 * ------------------------------------------------------------ */
static void confirm_prompt(const Store *s, char *out, size_t outsz) {
    snprintf(out, outsz, "Remove %s?  y / n", s->confirm_id);
}

/* The selected row's own description, from its manifest or the index - a
 * fact, not an instruction. Nothing is invented when the field is empty;
 * the footer is simply blank. */
static void selected_fact(const Store *s, char *out, size_t outsz) {
    out[0] = 0;
    if (s->sel < 0 || s->sel >= s->ndisp) return;
    const Disp *d = &s->disp[s->sel];
    if (d->kind == D_APP)
        snprintf(out, outsz, "%s", s->inst[d->idx].desc);
    else if (d->kind == D_PKG)
        snprintf(out, outsz, "%s", s->avail[d->idx].desc);
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

/* a circular arrow for the one header action when its label does not fit */
static void draw_refresh_icon(Surface *s, float x, float y, float sz, u32 c) {
    float r = sz * 0.34f;
    float cx = x + sz * 0.50f, cy = y + sz * 0.50f;
    gfx_rrect_aa(s, cx - r, cy - r, r * 2.0f, r * 2.0f, r, 0, c, sz * 0.10f);
    float pts[6] = { cx + r * 0.30f, cy - r * 0.96f,
                     cx + r * 0.96f, cy - r * 0.34f,
                     cx + r * 0.18f, cy - r * 0.20f };
    gfx_polygon(s, pts, 3, c);
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
                      const Store *st, int x, int w, int ry, int idx,
                      int hot) {
    const Disp *d = &st->disp[idx];
    int selected = (idx == st->sel) && disp_selectable(d);
    int iy = ry + (L->row_h - L->icon_sz) / 2;

    /* hover and selection are the same rectangle, different colour */
    if (selected)
        ui_pill(s, x + L->row_xpad, ry, w - 2 * L->row_xpad, L->row_h,
                (float)L->radius, color_alpha(th->accent, 0.18f));
    else if (hot && disp_selectable(d))
        ui_pill(s, x + L->row_xpad, ry, w - 2 * L->row_xpad, L->row_h,
                (float)L->radius, color_alpha(th->text, 0.05f));

    if (d->kind == D_SECT) {
        /* a section is a word, not a row: no icon, no count (the footer has
         * the counts), on the same left inset as the rows' names */
        draw_clipped(s, L->f.small, (float)(x + L->m),
                     (float)(ry + L->small_dy),
                     (float)(w - 2 * L->m), d->msg,
                     color_alpha(th->accent, 0.72f));
        return;
    }

    if (d->kind == D_MSG) {
        draw_clipped(s, L->f.small, (float)(x + L->m),
                     (float)(ry + L->small_dy),
                     (float)(w - 2 * L->m), d->msg,
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
        int ins = pkg_in_root(st, k->name);
        if (ins)
            draw_check_icon(s, (float)(x + L->m), (float)iy,
                            (float)L->icon_sz, th->accent, 0.60f);
        else
            draw_pkg_icon(s, (float)(x + L->m), (float)iy,
                          (float)L->icon_sz, th->accent,
                          (selected || hot) ? 1.0f : 0.85f);
        size = k->have_size ? k->size : -1;
        have_size = k->have_size;
        dim_name = ins;
    }

    if (have_size) human_size(size, sz, sizeof(sz));
    else           snprintf(sz, sizeof(sz), "-");

    if (L->show_ver && ver[0]) {
        char vf[64];
        fit_ellipsis(L->f.small, ver, vf, sizeof(vf), (float)L->ver_col);
        if (vf[0])
            ui_text_right(s, L->f.small, x + L->ver_right, small_y, vf, vcol);
    }
    if (L->show_size) {
        char sf[48];
        fit_ellipsis(L->f.small, sz, sf, sizeof(sf), (float)L->size_col);
        if (sf[0])
            ui_text_right(s, L->f.small, x + L->size_right, small_y, sf,
                          color_alpha(th->text, have_size ? 0.50f : 0.30f));
    }

    const char *nm = (d->kind == D_APP) ? st->inst[d->idx].name
                                       : st->avail[d->idx].name;
    draw_clipped(s, L->f.body, (float)name_x, (float)name_y, (float)name_w, nm,
                 dim_name ? color_alpha(th->text, 0.55f)
                          : (selected ? th->text
                                      : color_alpha(th->text, hot ? 0.94f : 0.86f)));
}

static void store_draw(App *a, Surface *s, int x, int y, int w, int h, const Theme *th) {
    Store *st = a->state;
    if (!st || !th) return;
    st->w = w;
    st->h = h;
    if (w < 1 || h < 1) return;

    /* Bring the two live sources up to date before laying them out.
     * poll_install() only touches the disk while an install is in flight.
     * The index file is stat()ed at most once a second: its mtime+size gate
     * already stops a re-read, and this stops the stat() itself from running
     * on every composed frame. A finished download appears within a second. */
    poll_install(st);
    {
        time_t nowt = time(NULL);
        if (st->index_check_at != nowt) {
            st->index_check_at = nowt;
            sync_index(st);
        }
    }

    Layout L;
    if (!layout_store(&L, st, w, h)) return;

    /* ---- header: the app's name, the Refresh action ---- */
    surf_blend(s, x, y, w, L.header_h, th->panel);
    font_draw(s, L.f.title, (float)(x + L.title_x), (float)(y + L.title_dy),
              "Apps", th->text);

    if (L.btn_w > 0) {
        int hot = st->px >= L.btn_x && st->px < L.btn_x + L.btn_w &&
                  st->py >= L.btn_y && st->py < L.btn_y + L.btn_h;
        if (L.btn_label) {
            ui_button(s, x + L.btn_x, y + L.btn_y, L.btn_w, L.btn_h,
                      (float)L.radius, "Refresh", L.f.small, hot, th);
        } else {
            u32 fill = hot ? color_alpha(th->accent, 0.28f)
                           : color_alpha(th->text, 0.07f);
            u32 bord = hot ? color_alpha(th->accent, 0.85f)
                           : color_alpha(th->panel_border, 0.85f);
            gfx_rrect_aa(s, (float)(x + L.btn_x), (float)(y + L.btn_y),
                         (float)L.btn_w, (float)L.btn_h, (float)L.radius,
                         fill, bord, 1.0f);
            float isz = (float)SP(2);
            draw_refresh_icon(s, (float)(x + L.btn_x) +
                                 ((float)L.btn_w - isz) * 0.5f,
                              (float)(y + L.btn_y) +
                                 ((float)L.btn_h - isz) * 0.5f,
                              isz, color_alpha(th->text, hot ? 0.95f : 0.86f));
        }
    }
    surf_blend(s, x, y + L.header_h - L.rule, w, L.rule,
               color_alpha(th->panel_border, 0.80f));

    /* ---- the search field ---- */
    if (L.search_h > 0) {
        int by = y + L.se_y;
        draw_search_icon(s, (float)(x + L.se_x),
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
            const char *ph = "Search";
            draw_clipped(s, L.f.body, tx, ty, avail, ph,
                         color_alpha(th->text, st->search_focus ? 0.50f : 0.32f));
            if (st->search_focus)
                gfx_line(s, tx, (float)by + (float)SP(1),
                         tx, (float)(by + L.se_h - SP(1)), 1.4f, th->accent);
        }

        if (st->filter[0])
            draw_clear_icon(s,
                            (float)(x + L.se_clear_x + L.se_clear_w / 2),
                            (float)by + (float)L.se_h * 0.5f,
                            (float)L.se_icon, color_alpha(th->text, 0.55f));

        /* one hairline closes the band, inset like the path rule in Files;
         * focus changes its colour, never the geometry */
        surf_blend(s, x + L.se_x, by + L.se_h - L.rule, L.se_w, L.rule,
                   st->search_focus ? color_alpha(th->accent, 0.80f)
                                    : color_alpha(th->panel_border, 0.55f));
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
            int ry = y + L.list_y + i * L.row_h;
            int hot = st->px >= x + L.row_xpad && st->px < x + w - L.row_xpad &&
                      st->py >= ry && st->py < ry + L.row_h;
            paint_row(s, &L, th, st, x, w, ry, idx, hot);
        }

        /* a map of the list, in the right margin (same shape as Files) */
        if (st->ndisp > L.rows) {
            int band = L.list_bot - L.list_y;
            int bx = x + w - L.bar_w - SP(1);
            if (bx > x && band > 0) {
                int thumb = band * L.rows / st->ndisp;
                if (thumb < L.thumb_min) thumb = L.thumb_min;
                if (thumb > band) thumb = band;
                int span = band - thumb;
                int off = max_scroll > 0 ? span * st->scroll / max_scroll : 0;
                ui_pill(s, bx, y + L.list_y, L.bar_w, band,
                        (float)L.bar_w * 0.5f, color_alpha(th->text, 0.06f));
                ui_pill(s, bx, y + L.list_y + off, L.bar_w, thumb,
                        (float)L.bar_w * 0.5f, color_alpha(th->accent, 0.55f));
            }
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
        if (st->filter[0])
            snprintf(right, sizeof(right), "%d shown", st->nshown);
        else
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
        int fact_line = 0;
        if (!left) {
            selected_fact(st, prompt, sizeof(prompt));
            left = prompt;
            col = color_alpha(th->text, 0.44f);
            fact_line = 1;
        }
        int maxw = w - 2 * L.m;
        if (fit) maxw -= (int)rw + L.gap;
        /* a fact cut to a stub is clutter: if the counts have taken the
         * room, let them stand alone rather than print "installed b..." */
        if (!fact_line || maxw >= SP(8))
            draw_clipped(s, L.f.small, (float)(x + L.m), (float)ty,
                         (float)maxw, left, col);
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

        /* 2. escape runs: the same navigation in both modes; a bare Esc
         * cancels the search, the only modal thing this window has */
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
            } else if (s->search_focus) {
                filter_clear(s);
                s->search_focus = 0;
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

/* ------------------------------------------------------------
 * Host harness window (-DSTORE_HARNESS only)
 *
 * .work/<id>/store_action.c drives the SAME AppVT the desktop drives -
 * init, then key bytes - and these two readers print what the window
 * would have drawn. They exist so the proof is about this file's real
 * behaviour: which row Enter acts on, what request it writes, and the
 * sentence left in the status line. The image is never built with
 * -DSTORE_HARNESS, so neither function is in a shipping binary.
 * ------------------------------------------------------------ */
#ifdef STORE_HARNESS
const char *store_harness_status(const App *a) {
    const Store *s = a->state;
    return s ? s->status : "";
}

/* One line per painted row: "<index>\t<kind>\t<label>\tmark=<n>".
 * Returns the number of rows there were, or 0 with no state. */
int store_harness_rows(const App *a, char *out, size_t cap) {
    const Store *s = a->state;
    if (!s || !out || cap == 0) return 0;
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < s->ndisp; i++) {
        const Disp *d = &s->disp[i];
        const char *kind = d->kind == D_APP  ? "APP"  :
                           d->kind == D_PKG  ? "PKG"  :
                           d->kind == D_SECT ? "SECT" : "MSG";
        const char *label = d->msg ? d->msg : "";
        if (d->kind == D_APP) label = s->inst[d->idx].id;
        else if (d->kind == D_PKG) label = s->avail[d->idx].name;
        int w = snprintf(out + n, cap - n, "%d\t%s\t%s\tmark=%d%s\n", i, kind,
                         label, (int)d->mark, i == s->sel ? "\tSELECTED" : "");
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
    return s->ndisp;
}
#endif

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
