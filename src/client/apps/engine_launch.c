/* ============================================================
 *  Mystic OS - apps/external / engine_launch
 *
 *  The package's `bin`: a STATIC program whose whole job is to start the
 *  DYNAMIC engine viewer through the musl loader the bundle carries.
 *
 *      Engine [url]        (after: pkg install engine)
 *
 *  WHY A LAUNCHER EXISTS AT ALL
 *  ----------------------------
 *  src/wmsrv.c chroots each app into a directory assembled from binds: the
 *  payload at /bin/app, the bundle read-only at /app, the socket, fonts, a
 *  few /dev nodes, and the app's own /tmp and /root. There is no /lib in that
 *  jail, and a dynamic ELF's PT_INTERP is an absolute path the kernel resolves
 *  before main() runs - so a dynamic payload cannot start there. The same is
 *  true of the viewer: it must be dynamic because only a dynamic musl program
 *  can load libjpeg.so.8 (a static musl program has no dynamic loader at all),
 *  and it must be started by something.
 *
 *  This file is that something, and it is the same pattern the VLC package
 *  already uses (src/client/apps/vlcplay.c, section 0):
 *
 *      <root>/ld-musl-x86_64.so.1
 *          --library-path <root>/usr/lib:<root>/lib
 *          <root>/libexec/engine <args...>
 *
 *  musl's loader is itself an executable, so execv() on it with the program
 *  as an argument is a supported entry point - no /lib, no PT_INTERP, no
 *  dynamic dependency in this binary beyond libc (it is linked -static, so
 *  even that is inside the file).
 *
 *  WHERE THE BUNDLE IS
 *  -------------------
 *   1. $MYSTIC_BUNDLE   src/wmsrv.c exports /app inside the jail. A value
 *                       that is not absolute is refused by name: a relative
 *                       path cannot be a mount point, and silently guessing
 *                       would start the wrong program.
 *   2. /app             the default, which is where the sandbox binds the
 *                       bundle when the server exports nothing.
 *  Trailing slashes are stripped, so MYSTIC_BUNDLE=/app/ and /app are the
 *  same root.
 *
 *  WHAT IT SAYS WHEN IT CANNOT START
 *  ---------------------------------
 *  Every failure names the exact path and errno and exits 127 - the shell's
 *  "command not found" status, and the one src/wmsrv.c reports as a failed
 *  spawn. A tile that dies silently is the failure this program exists to
 *  prevent, so nothing here exits 0 or disappears without a sentence:
 *
 *      engine: /app/ld-musl-x86_64.so.1: No such file or directory - the
 *      bundle must carry the musl loader at its root
 *
 *  BUILD
 *  -----
 *      zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
 *             -o bin src/client/apps/engine_launch.c
 *
 *  It links no SDK object and no libjpeg: the proof is the ELF itself, which
 *  must have e_type 2, no PT_INTERP and no DT_NEEDED (tools/pack-engine.py
 *  checks all three before packing).
 *
 *  The pure path logic below is also compiled for the HOST and run, because a
 *  launcher's failure mode is a string, not a syscall:
 *
 *      zig cc -target x86_64-windows -DENGINE_LAUNCH_TEST \
 *             -o launcher-test.exe src/client/apps/engine_launch.c
 *      ./launcher-test.exe        # 33 checks, prints the exact argv layout
 *
 *  That is how the root selection, the trailing-slash rule, the refusal of a
 *  relative MYSTIC_BUNDLE, the buffer arithmetic and the argv passthrough were
 *  verified in the run that added this file. What no host run can show is the
 *  exec itself inside the wmsrv jail; that needs a boot.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ENGINE_LAUNCH_TEST)
#include <errno.h>
#include <unistd.h>
#endif

/* Relative paths inside the bundle. The loader sits at the bundle ROOT, not
 * under lib/: it is the same file tools/pack-engine.py patches Chromium's
 * PT_INTERP to (/app/ld-musl-x86_64.so.1), and one copy is enough. */
#define ENG_LOADER_REL  "ld-musl-x86_64.so.1"
#define ENG_ENGINE_REL  "libexec/engine"
#define ENG_DEFAULT_ROOT "/app"

/* Sizes are worst cases, not round numbers:
 *   root    511 + NUL
 *   loader  511 + 1 + 20 ("ld-musl-x86_64.so.1") + NUL       = 533
 *   engine  511 + 1 + 14 ("libexec/engine")      + NUL       = 526
 *   libpath 511 + 1+7 + 1 + 511 + 1+3            + NUL       = 1036
 * so nothing that fits in root[] can truncate a path built from it. */
typedef struct {
    char root[512];
    char loader[640];
    char engine[640];
    char libpath[1152];     /* <root>/usr/lib:<root>/lib */
} EngPlan;

/* Why a root was refused, so the message can be true. A too-long or relative
 * MYSTIC_BUNDLE is NOT "unset": saying so would be a lie. */
enum {
    ENG_ROOT_TOO_LONG = -2,
    ENG_ROOT_BAD_ENV  = -1,
    ENG_ROOT_OK       =  1
};

/* "<root><sep><rel>", where sep disappears when root already ends in '/'.
 * Returns 1 when the result fit, 0 when it did not. */
static int eng_join(char *out, size_t n, const char *root, const char *rel) {
    const char *sep = "/";
    size_t rl = strlen(root);
    if (rl && root[rl - 1] == '/') sep = "";
    return (size_t)snprintf(out, n, "%s%s%s", root, sep, rel) < n;
}

/* Pick the bundle root. Pure string work - this is what the host test drives.
 *
 *   env NULL or ""            -> ENG_DEFAULT_ROOT
 *   env not starting with '/' -> ENG_ROOT_BAD_ENV
 *   env longer than out[]     -> ENG_ROOT_TOO_LONG
 *   otherwise                 -> env with trailing '/' removed
 */
static int eng_root(const char *env, char *out, size_t n, int *why) {
    size_t len;

    if (!out || n < 2) {
        if (why) *why = ENG_ROOT_TOO_LONG;
        return 0;
    }
    out[0] = 0;
    if (!env || !*env) {
        if (ENG_DEFAULT_ROOT[0] && strlen(ENG_DEFAULT_ROOT) >= n) {
            if (why) *why = ENG_ROOT_TOO_LONG;
            return 0;
        }
        snprintf(out, n, "%s", ENG_DEFAULT_ROOT);
        return ENG_ROOT_OK;
    }
    if (env[0] != '/') {
        if (why) *why = ENG_ROOT_BAD_ENV;
        return 0;
    }
    len = strlen(env);
    while (len > 1 && env[len - 1] == '/') len--;
    if (len >= n) {
        if (why) *why = ENG_ROOT_TOO_LONG;
        return 0;
    }
    memcpy(out, env, len);
    out[len] = 0;
    return ENG_ROOT_OK;
}

/* root -> the absolute paths and the library search path. 0 when any of them
 * would be truncated (which the buffer arithmetic above rules out for roots
 * that fit in root[]; it is still checked rather than assumed). */
static int eng_plan(const char *root, EngPlan *p) {
    char usrlib[640], lib[640];

    if (!root || !*root || !p) return 0;
    if (strlen(root) >= sizeof(p->root)) return 0;
    snprintf(p->root, sizeof(p->root), "%s", root);
    if (!eng_join(p->loader, sizeof(p->loader), p->root, ENG_LOADER_REL)) return 0;
    if (!eng_join(p->engine, sizeof(p->engine), p->root, ENG_ENGINE_REL)) return 0;
    if (!eng_join(usrlib, sizeof(usrlib), p->root, "usr/lib")) return 0;
    if (!eng_join(lib, sizeof(lib), p->root, "lib")) return 0;
    return (size_t)snprintf(p->libpath, sizeof(p->libpath), "%s:%s", usrlib, lib)
           < sizeof(p->libpath);
}

/* The loader command line: 4 fixed words, the caller's argv[1..] verbatim,
 * then NULL. argv[0] is the loader, which is what the loader expects. */
static int eng_args(EngPlan *p, int argc, char **argv, char ***out) {
    int i, extra = argc > 1 ? argc - 1 : 0;
    char **a = (char **)calloc((size_t)extra + 5u, sizeof *a);

    if (!a) return 0;
    a[0] = p->loader;
    a[1] = (char *)"--library-path";
    a[2] = (char *)p->libpath;
    a[3] = (char *)p->engine;
    for (i = 0; i < extra; i++) a[4 + i] = argv[1 + i];
    a[4 + extra] = NULL;
    *out = a;
    return 1;
}

#if defined(ENGINE_LAUNCH_TEST)

/* Host harness: the string logic above, run for real. Not part of the package
 * (the shipped binary is built without this define). */
static int t_fail;

static void t_eq(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %-28s got \"%s\" want \"%s\"\n", what, got, want);
        t_fail++;
    } else {
        printf("ok   %-28s \"%s\"\n", what, got);
    }
}

static void t_int(const char *what, int got, int want) {
    if (got != want) {
        printf("FAIL %-28s got %d want %d\n", what, got, want);
        t_fail++;
    } else {
        printf("ok   %-28s %d\n", what, got);
    }
}

int main(void) {
    EngPlan p;
    char root[512], **args;
    int why = 0;
    char longenv[600];
    char *av[4];

    printf("== engine_launch: bundle root and loader command ==\n");

    t_int("env=/app", eng_root("/app", root, sizeof root, &why), ENG_ROOT_OK);
    t_eq("  root", root, "/app");
    t_int("  plan", eng_plan(root, &p), 1);
    t_eq("  loader", p.loader, "/app/ld-musl-x86_64.so.1");
    t_eq("  engine", p.engine, "/app/libexec/engine");
    t_eq("  libpath", p.libpath, "/app/usr/lib:/app/lib");

    t_int("env=/app/ (trailing slash)", eng_root("/app/", root, sizeof root, &why), ENG_ROOT_OK);
    t_eq("  root", root, "/app");

    t_int("env=NULL (default)", eng_root(NULL, root, sizeof root, &why), ENG_ROOT_OK);
    t_eq("  root", root, "/app");
    t_int("env=\"\" (default)", eng_root("", root, sizeof root, &why), ENG_ROOT_OK);
    t_eq("  root", root, "/app");

    t_int("env=app (relative)", eng_root("app", root, sizeof root, &why), 0);
    t_int("  why", why, ENG_ROOT_BAD_ENV);

    memset(longenv, 'a', sizeof longenv - 1);
    longenv[0] = '/';
    longenv[sizeof longenv - 1] = 0;
    t_int("env 599 chars", eng_root(longenv, root, sizeof root, &why), 0);
    t_int("  why", why, ENG_ROOT_TOO_LONG);

    /* "/" is a legal root: the joins must not produce "//x" */
    t_int("env=/ (root is /)", eng_root("/", root, sizeof root, &why), ENG_ROOT_OK);
    t_eq("  root", root, "/");
    t_int("  plan", eng_plan(root, &p), 1);
    t_eq("  loader", p.loader, "/ld-musl-x86_64.so.1");
    t_eq("  engine", p.engine, "/libexec/engine");
    t_eq("  libpath", p.libpath, "/usr/lib:/lib");

    /* argv passthrough, exactly as main() would do it */
    av[0] = (char *)"bin";
    av[1] = (char *)"/root/start.html";
    av[2] = (char *)"http://example.org/";
    av[3] = NULL;
    t_int("plan for argv test", eng_plan("/app", &p), 1);
    t_int("argv no args", eng_args(&p, 1, av, &args), 1);
    t_eq("  args[0]", args[0], "/app/ld-musl-x86_64.so.1");
    t_eq("  args[1]", args[1], "--library-path");
    t_eq("  args[2]", args[2], "/app/usr/lib:/app/lib");
    t_eq("  args[3]", args[3], "/app/libexec/engine");
    t_int("  args[4] is NULL", args[4] == NULL, 1);
    free(args);

    t_int("argv with 2 args", eng_args(&p, 3, av, &args), 1);
    t_eq("  args[4]", args[4], "/root/start.html");
    t_eq("  args[5]", args[5], "http://example.org/");
    t_int("  args[6] is NULL", args[6] == NULL, 1);
    printf("  the command line: %s %s %s %s %s %s\n", args[0], args[1], args[2],
           args[3], args[4], args[5]);
    free(args);

    printf("\n%s (%d failures)\n", t_fail ? "FAILED" : "PASSED", t_fail);
    return t_fail ? 1 : 0;
}

#else  /* the shipped program */

/* When nothing names the mount, the launcher is sitting in the bundle root,
 * so /proc/self/exe is the bundle: /root/apps/engine/bin -> /root/apps/engine.
 * This is what makes the payload runnable outside the jail too (a maintainer
 * poking at it from a shell), and it is why the absolute /app fallback is only
 * the last resort. */
static int root_from_exe(char *out, size_t cap) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    size_t len;
    if (n <= 0) return 0;
    exe[n] = 0;
    slash = strrchr(exe, '/');
    if (!slash || slash == exe) return 0;
    *slash = 0;
    len = strlen(exe);
    if (len + 1 > cap) return 0;
    memcpy(out, exe, len + 1);
    return 1;
}

int main(int argc, char **argv) {
    const char *env = getenv("MYSTIC_BUNDLE");
    char derived[sizeof(((EngPlan *)0)->root)];
    char root[sizeof(((EngPlan *)0)->root)];
    if (!env || !*env)
        if (root_from_exe(derived, sizeof derived)) env = derived;
    EngPlan plan;
    char **args;
    int why = 0;

    if (eng_root(env, root, sizeof root, &why) != ENG_ROOT_OK) {
        if (why == ENG_ROOT_BAD_ENV) {
            fprintf(stderr,
                    "engine: MYSTIC_BUNDLE='%s' is not an absolute path, so it "
                    "cannot name the bundle mount\n", env);
        } else {
            fprintf(stderr,
                    "engine: %s is too long for the %u-byte bundle-root "
                    "buffer\n",
                    (env && *env) ? "MYSTIC_BUNDLE" : "the default root",
                    (unsigned)sizeof root);
        }
        return 127;
    }
    fprintf(stderr, "engine: launcher root=%s\n", root);
    if (!eng_plan(root, &plan)) {
        fprintf(stderr, "engine: the bundle paths under %s do not fit the "
                        "launcher's buffers\n", root);
        return 127;
    }

    /* Say which path, and why, before spending an exec on it. */
    if (access(plan.loader, X_OK) != 0) {
        fprintf(stderr, "engine: %s: %s - the bundle must carry the musl "
                        "loader at its root (%s)\n",
                plan.loader, strerror(errno), ENG_LOADER_REL);
        return 127;
    }
    if (access(plan.engine, X_OK) != 0) {
        fprintf(stderr, "engine: %s: %s - the dynamic viewer must be installed "
                        "at %s\n", plan.engine, strerror(errno), ENG_ENGINE_REL);
        return 127;
    }

    /* The loader gets the same list with --library-path, so this is for the
     * programs the viewer forks later (Chromium re-execs /proc/self/exe, and
     * its children need the closure too). Keep a value the server exported:
     * src/wmsrv.c may know directories this launcher does not. */
    setenv("LD_LIBRARY_PATH", plan.libpath, 0);

    if (!eng_args(&plan, argc, argv, &args)) {
        fprintf(stderr, "engine: out of memory building the loader command "
                        "line\n");
        return 127;
    }
    execv(plan.loader, args);

    fprintf(stderr, "engine: cannot exec %s: %s\n", plan.loader,
            strerror(errno));
    free(args);
    return 127;
}

#endif /* ENGINE_LAUNCH_TEST */
