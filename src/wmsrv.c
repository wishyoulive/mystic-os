#define _GNU_SOURCE        /* unshare(), setgroups(), CLONE_* : musl
                            * only exposes them when this precedes
                            * every include, ours included. */
/* ============================================================
 *  Mystic OS - wmsrv.c
 *  The server half of the display protocol: a listening socket,
 *  one shared-memory buffer per app, and line-oriented messages.
 * ============================================================ */
#include "ipc.h"

#ifdef MYSTIC_SANDBOX_PLAN
/* The host harness build: the plan is portable C, so it needs nothing but
 * stdio, strings and the S_IF* bits. No kernel calls, nothing Linux-only. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <grp.h>
#include <linux/magic.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif


/* The plan below is the sandbox, as data: it is built from the bundle, the
 * credential and the screen size, printed by the host harness, and executed
 * step by step. It is included before the Linux-only server body so that a
 * harness can build and print a plan without a kernel (docs/SECURITY.md). */
/* ============================================================
 *  The sandbox plan
 *
 *  What a jailed app gets is DATA before it is syscalls: one SbxPlan is
 *  built from the bundle, the credential and the screen size, printed by
 *  the host harness, and then executed - by sandbox_prepare() in the
 *  desktop's own namespaces before the fork, and by sandbox_child() in the
 *  app's namespaces afterwards. The printer and both executors walk the
 *  same array, so a printed plan cannot describe something different from
 *  what runs.
 *
 *  Why the split:
 *
 *   - the jail skeleton and the two per-app tmpfs mounts happen in the
 *     PARENT, before fork. They have to: the desktop writes the app's file
 *     picker request to <jail>/tmp/filepick.request (main.c) and creates the
 *     app's pixel buffer inside <jail>/dev/shm after HELLO. Both paths only
 *     reach the app if that tmpfs instance is inherited by it instead of
 *     being mounted on the child's side of the fork.
 *
 *   - every read-only bind, the procfs, the chroot, the credential drop and
 *     the post-drop writability check happen in the CHILD, after unshare(),
 *     so nothing the app can influence exists before its namespaces do.
 *
 *  This part of the file compiles on any host (see docs/SECURITY.md,
 *  "proving it"): it is only arrays, snprintf and mode bits, no kernel
 *  calls, so the harness can run it on the build machine and print the
 *  exact plan. The executor below is Linux.
 * ============================================================ */

/* ---------------------------------------------------------- credentials
 * One uid per BUNDLE, from a small indexed pool: uid = 2000 + slot.
 *
 * Why per bundle and not per connection: an app's own /root (its appdata) can
 * only have one owner, and two windows of the same program must not fight over
 * it. Two DIFFERENT apps never share a uid, which is the hole the audit proved:
 * with one SANDBOX_UID for everything, any app could open, forge and ftruncate
 * any other app's pixel buffer and SIGBUS the compositor.
 *
 * The range is capped at the number of windows the server can serve
 * (WM_MAX_CLIENTS), and a uid is only handed to a bundle that has no live
 * process, so a uid is never reused while an app is still running under it.
 */
#define SBX_UID_BASE   2000
#define SBX_UID_SLOTS  WM_MAX_CLIENTS        /* 16 -> uid 2000..2015 */
#define SBX_UID_TOP    (SBX_UID_BASE + SBX_UID_SLOTS)
#define SBX_GID_BASE   SBX_UID_BASE          /* gid == uid; groups stay empty */

typedef struct {
    char id[32];                  /* bundle that owns this uid, "" = slot free */
    int  host;                    /* the app runs in the host namespaces */
    int  nanchor;                 /* launches whose process is still alive */
    int  npid;                    /* connections the kernel attributes here */
    long anchor[SBX_UID_SLOTS];
    long pid[SBX_UID_SLOTS];
} SbxCred;

static SbxCred sbx_creds[SBX_UID_SLOTS];

static int sbx_uid_of_slot(int i) { return SBX_UID_BASE + i; }

static int sbx_uid_valid(int uid) {
    return uid >= SBX_UID_BASE && uid < SBX_UID_TOP;
}

/* The harness has no children to watch, so a recorded process counts as
 * alive: its output then shows the pool doing its job instead of every slot
 * being collected the moment it is handed out. */
#ifdef MYSTIC_SANDBOX_PLAN
static int sbx_pid_alive(long pid) { return pid > 0; }
static void sbx_jail_forget(const char *id) { (void)id; }
#else
static int sbx_pid_alive(long pid) {
    return pid > 0 && kill((pid_t)pid, 0) == 0;
}
static void sbx_jail_forget(const char *id);   /* defined in the executor */
#endif

/* Drop entries whose process is gone. A bundle whose processes have all
 * exited loses its uid (and with it the per-app tmpfs - sbx_jail_forget()
 * unmounts it in the server). */
static void sbx_cred_gc(void) {
    for (int i = 0; i < SBX_UID_SLOTS; i++) {
        SbxCred *c = &sbx_creds[i];
        if (!c->id[0]) continue;
        int k = 0;
        for (int j = 0; j < c->nanchor; j++)
            if (sbx_pid_alive(c->anchor[j])) c->anchor[k++] = c->anchor[j];
        c->nanchor = k;
        k = 0;
        for (int j = 0; j < c->npid; j++)
            if (sbx_pid_alive(c->pid[j])) c->pid[k++] = c->pid[j];
        c->npid = k;
        if (c->nanchor == 0 && c->npid == 0) {
            sbx_jail_forget(c->id);
            c->id[0] = 0;
        }
    }
}

static SbxCred *sbx_cred_by_uid(int uid) {
    if (!sbx_uid_valid(uid)) return NULL;
    SbxCred *c = &sbx_creds[uid - SBX_UID_BASE];
    return c->id[0] ? c : NULL;
}

/* Reserve the uid for a bundle. Returns the uid, or -1 when every slot is
 * held by a live app (which is also when the client table is full).
 *
 * `reuse` comes back 1 when this bundle already holds a uid, i.e. a second
 * window of a program that is already running: the tmpfs the first window
 * mounted is kept instead of being replaced underneath it. */
static int sbx_cred_acquire(const char *id, int *reuse) {
    if (reuse) *reuse = 0;
    if (!id || !id[0]) return -1;
    sbx_cred_gc();
    int slot = -1;
    for (int i = 0; i < SBX_UID_SLOTS; i++) {
        if (sbx_creds[i].id[0] && !strcmp(sbx_creds[i].id, id)) {
            if (reuse) *reuse = 1;
            return sbx_uid_of_slot(i);
        }
        if (!sbx_creds[i].id[0] && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    SbxCred *c = &sbx_creds[slot];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id);
    return sbx_uid_of_slot(slot);
}

static void sbx_cred_add_anchor(int uid, long pid) {
    SbxCred *c = sbx_cred_by_uid(uid);
    if (!c || pid <= 0) return;
    for (int i = 0; i < c->nanchor; i++) if (c->anchor[i] == pid) return;
    if (c->nanchor < SBX_UID_SLOTS) c->anchor[c->nanchor++] = pid;
}

static void sbx_cred_add_pid(int uid, long pid) {
    SbxCred *c = sbx_cred_by_uid(uid);
    if (!c || pid <= 0) return;
    for (int i = 0; i < c->npid; i++) if (c->pid[i] == pid) return;
    if (c->npid < SBX_UID_SLOTS) c->pid[c->npid++] = pid;
}

/* A window closed: forget that connection. The uid goes back to the pool when
 * the last process of that bundle is gone - which may be now, or a moment
 * later when the app finishes exiting (the next gc sees it). */
static void sbx_cred_del_pid(int uid, long pid) {
    SbxCred *c = sbx_cred_by_uid(uid);
    if (c) {
        for (int i = 0; i < c->npid; i++) {
            if (c->pid[i] != pid) continue;
            for (int j = i + 1; j < c->npid; j++) c->pid[j - 1] = c->pid[j];
            c->npid--;
            break;
        }
    }
    sbx_cred_gc();
}

/* ---------------------------------------------------------- the plan */
#define SBX_STEPS 24
#define SBX_PATH  320

/* where a step runs */
enum { SBX_PARENT = 0, SBX_CHILD = 1 };

/* what a step does */
enum {
    SBX_MKDIR = 1,     /* mkdir -p, no symlink in the way */
    SBX_TMPFS,         /* mount -t tmpfs with size= and mode= */
    SBX_CHOWN,         /* give the app its appdata, recursively if needed */
    SBX_BIND,          /* mount --bind, optionally read-only */
    SBX_PROC,          /* mount -t proc proc (the app's own pid namespace) */
    SBX_PROC_RO,       /* mount --bind /proc, read-only (procs = yes) */
    SBX_CHROOT,
    SBX_DROP,          /* setgroups(0), setgid, setuid */
    SBX_VERIFY         /* prove the jail is not writable and /tmp+/root are */
};

/* mount flags, in our own bits so the plan prints on a host without MS_* */
enum { SBX_F_NOSUID = 1u << 0, SBX_F_NODEV = 1u << 1, SBX_F_NOEXEC = 1u << 2 };

typedef struct {
    unsigned char where;             /* SBX_PARENT or SBX_CHILD */
    unsigned char op;
    unsigned char ro;
    unsigned mode;
    unsigned flags;                  /* SBX_F_* */
    int uid;
    unsigned long long size;
    char src[SBX_PATH];
    char dst[SBX_PATH];              /* absolute for the parent, jail-relative
                                      * for the child */
    char opts[112];
    int absolute;                    /* dst is a host path, not jail-relative */
    int dst_is_abs;                  /* same, for tmpfs steps */
} SbxStep;

typedef struct {
    char id[32];
    char jail[SBX_PATH];
    int  uid, gid;
    unsigned ns;                     /* SBX_NS_* */
    int  procs, net, audio;
    int  no_chroot;                  /* namespaces and uid, but no chroot */
    char tmpdir[SBX_PATH];           /* the app's private temp directory */
    char appdata[SBX_PATH];          /* the app's own data directory */
    int  reuse;                      /* a window of this app is already up */
    int  stride, maxh;
    unsigned long long buf_bytes, shm_size, tmp_size;
    int  nstep;
    SbxStep step[SBX_STEPS];
} SbxPlan;

/* The namespace set is described with our own bits and translated to CLONE_*
 * at the one place that calls unshare(), so the plan prints and compares on a
 * host that has no clone(2). */
enum { SBX_NS_MNT = 1u << 0, SBX_NS_IPC = 1u << 1, SBX_NS_UTS = 1u << 2,
       SBX_NS_NET = 1u << 3, SBX_NS_PID = 1u << 4 };

/* Every per-app tmpfs is two buffers plus a megabyte: the buffer is at most
 * stride * maxh * 4 bytes (the server clamps a HELLO to the screen, so that is
 * the whole window), and 2x gives an app room for exactly one more buffer's
 * worth of its own shared memory on top, + 1 MiB for the rest (a player's
 * cache, a lock file, a score). That is the whole point: an app can fill its
 * own tmpfs and get ENOSPC, not fill the machine. */
#define SBX_MIB 1048576ull
static unsigned long long sbx_buf_bytes(int stride, int maxh) {
    if (stride < 1) stride = 1;
    if (maxh < 1) maxh = 1;
    return (unsigned long long)stride * (unsigned long long)maxh * 4ull;
}
/* The per-app tmpfs holds the client's frame buffer and whatever the app maps.
 * Two frames plus a megabyte is the minimum a GUI client needs, but a browser
 * engine maps tens of megabytes of shared memory before it draws anything, and
 * with only the minimum it dies during startup. The floor is stated here so the
 * arithmetic stays in one place; it is still a cap, not a free-for-all. */
#define SBX_SHM_FLOOR (64ull * 1024ull * 1024ull)
static unsigned long long sbx_tmpfs_size(unsigned long long buf) {
    unsigned long long need = 2ull * buf + SBX_MIB;
    return need > SBX_SHM_FLOOR ? need : SBX_SHM_FLOOR;
}

static void sbx_ns_names(unsigned ns, char *buf, size_t cap) {
    static const struct { unsigned bit; const char *name; } tab[] = {
        { SBX_NS_MNT, "NEWNS" },  { SBX_NS_IPC, "NEWIPC" },
        { SBX_NS_UTS, "NEWUTS" }, { SBX_NS_NET, "NEWNET" },
        { SBX_NS_PID, "NEWPID" },
    };
    size_t o = 0;
    buf[0] = 0;
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!(ns & tab[i].bit)) continue;
        int n = snprintf(buf + o, cap - o, "%s%s", o ? "|" : "", tab[i].name);
        if (n < 0) break;
        if ((size_t)n >= cap - o) { buf[cap - 1] = 0; return; }
        o += (size_t)n;
    }
}

static SbxStep *sbx_add(SbxPlan *p, int where, int op,
                        const char *src, const char *dst) {
    static SbxStep overflow;       /* a build bug, never a valid plan */
    if (p->nstep >= SBX_STEPS) {
        fprintf(stderr, "wms: sandbox plan for %s is full (%d steps); "
                        "the extra step is not added\n", p->id, SBX_STEPS);
        memset(&overflow, 0, sizeof(overflow));
        return &overflow;
    }
    SbxStep *s = &p->step[p->nstep++];
    memset(s, 0, sizeof(*s));
    s->where = (unsigned char)where;
    s->op = (unsigned char)op;
    s->uid = p->uid;
    if (src) snprintf(s->src, sizeof(s->src), "%s", src);
    if (dst) snprintf(s->dst, sizeof(s->dst), "%s", dst);
    return s;
}

static void sbx_plan_init(SbxPlan *p, const char *run, const char *id,
                          const char *exe, const char *appdata,
                          int uid, int procs, int net, int audio,
                          int no_chroot, int stride, int maxh, const char *arg) {
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id ? id : "");
    snprintf(p->jail, sizeof(p->jail), "%s/jail/%s", run, p->id);
    p->uid = uid;
    p->gid = uid;
    p->procs = procs ? 1 : 0;
    p->no_chroot = no_chroot ? 1 : 0;
    p->net = net ? 1 : 0;
    p->audio = audio ? 1 : 0;
    p->stride = stride;
    p->maxh = maxh;
    p->buf_bytes = sbx_buf_bytes(stride, maxh);
    p->shm_size = sbx_tmpfs_size(p->buf_bytes);
    p->tmp_size = p->shm_size;

    /* A private pid namespace is the default: the app sees itself and
     * nothing else, and cannot signal any process outside its own
     * namespace - not even the desktop. procs = yes is the single, named
     * exception (sysmon reads every process); it skips CLONE_NEWPID and
     * gets the host /proc read-only instead. That trade is made once, in
     * the manifest, and documented in docs/SECURITY.md. */
    p->ns = SBX_NS_MNT | SBX_NS_IPC | SBX_NS_UTS;
    if (!p->net) p->ns |= SBX_NS_NET;
    if (!p->procs) p->ns |= SBX_NS_PID;

    char dir[SBX_PATH];

    /* ---- parent: skeleton + the two per-app tmpfs mounts */
    sbx_add(p, SBX_PARENT, SBX_MKDIR, NULL, p->jail)->mode = 0755;
    snprintf(dir, sizeof(dir), "%s/tmp", p->jail);
    snprintf(p->tmpdir, sizeof(p->tmpdir), "%s", dir);
    snprintf(p->appdata, sizeof(p->appdata), "%s", appdata);
    sbx_add(p, SBX_PARENT, SBX_MKDIR, NULL, dir)->mode = 0755;
    {
        SbxStep *s = sbx_add(p, SBX_PARENT, SBX_TMPFS, "tmpfs", dir);
        s->mode = 0700; s->size = p->tmp_size;
        s->flags = SBX_F_NOSUID | SBX_F_NODEV;
        snprintf(s->opts, sizeof(s->opts),
                 "size=%llu,mode=0700,uid=%d,gid=%d", p->tmp_size, uid, uid);
    }
    if (p->no_chroot) {
        /* The frame buffer lives in the per-app tmpfs; without a chroot the app
         * sees the host /dev/shm, so that tmpfs is mounted over it. */
        char shmdir[SBX_PATH];
        snprintf(shmdir, sizeof(shmdir), "%s/dev/shm", p->jail);
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, shmdir, "/dev/shm");
        s->mode = 0700;
        s->absolute = 1;
    }
    snprintf(dir, sizeof(dir), "%s/dev/shm", p->jail);
    sbx_add(p, SBX_PARENT, SBX_MKDIR, NULL, dir)->mode = 0755;
    {
        SbxStep *s = sbx_add(p, SBX_PARENT, SBX_TMPFS, "tmpfs", dir);
        s->mode = 0700; s->size = p->shm_size;
        s->flags = SBX_F_NOSUID | SBX_F_NODEV | SBX_F_NOEXEC;
        snprintf(s->opts, sizeof(s->opts),
                 "size=%llu,mode=0700,uid=%d,gid=%d", p->shm_size, uid, uid);
    }

    /* Without a chroot the child's /tmp is the host's, which an app uid cannot
     * write: the private tmpfs is bound over it instead, so an un-chrooted app
     * still gets its own writable /tmp and nothing else. */
    if (p->no_chroot) {
        char tmpdir[SBX_PATH];
        snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", p->jail);
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, tmpdir, "/tmp");
        s->mode = 0700;
    }

    /* ---- parent: the app's own /root, owned by the app */
    sbx_add(p, SBX_PARENT, SBX_CHOWN, appdata, appdata)->mode = 0700;

    {
        /* the bundle directory: a package may ship a runtime beside the
         * executable (libVLC and its plugins), and only that one directory is
         * added - never /root, never the rest of the apps tree */
        char bundle[SBX_PATH];
        snprintf(bundle, sizeof(bundle), "%s", exe);
        char *slash = strrchr(bundle, '/');
        if (slash) *slash = 0;
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, bundle, "/app");
        s->ro = 1; s->mode = 0755;
        s->absolute = p->no_chroot;      /* /app is the interpreter path */
    }

    /* ---- child: the payload and its own bundle, read-only.
     * Without a chroot the bundle still goes to /app, because the interpreter
     * the package writes into PT_INTERP is /app/ld-musl-x86_64.so.1. */
    /* Without a chroot the payload runs from its real path, so it is not
     * mounted anywhere; inside a chroot it has to be at /bin/app. */
    if (!p->no_chroot) {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, exe, "/bin/app");
        s->ro = 1; s->mode = 0755;
    }
    /* ---- child: the window socket */
    {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, WM_SOCKET, "/run/mystic/wm.sock");
        s->mode = 0666;
    }

    /* ---- child: name resolution, only for an app that asked for the netns */
    if (p->net) {
        static const char *const netfiles[] = {
            "/etc/resolv.conf", "/etc/hosts", "/etc/ssl/certs/ca-bundle.crt"
        };
        for (size_t i = 0; i < sizeof(netfiles) / sizeof(netfiles[0]); i++) {
            SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, netfiles[i], netfiles[i]);
            s->ro = 1; s->mode = 0644;
        }
    }

    /* ---- child: its own /root (the appdata directory, bound) */
    {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, appdata, "/root");
        s->mode = 0700;
    }

    /* ---- child: the four devices a GUI app cannot do without */
    {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/dev/null", "/dev/null");
        s->mode = 0666;
    }
    {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/usr/share/fonts", "/usr/share/fonts");
        s->ro = 1; s->mode = 0755;
    }
    {
        /* /etc is read-only configuration, not secrets: /etc/passwd, /etc/hosts
         * and the fontconfig files are what a toolkit looks up before it can
         * draw anything, and an engine without them fails in ways that print
         * nothing. /etc/shadow is mode 0640 root:root, so an app uid cannot
         * read it even though the directory is present. */
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/etc", "/etc");
        s->ro = 1; s->mode = 0755;
    }
    {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/dev/urandom", "/dev/urandom");
        s->ro = 1; s->mode = 0666;
    }
    {
        /* mmap of anonymous memory and libraries is backed by /dev/zero on
         * some paths; without it an engine's allocator fails early. */
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/dev/zero", "/dev/zero");
        s->mode = 0666;
    }
    if (p->procs) {
        /* Apps that ask for process visibility are the ones that also need to
         * report why they failed; the console is how that reaches the serial
         * log. It is tied to procs so it is not handed out to every app. */
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/dev/console", "/dev/console");
        s->mode = 0620;
    }

    /* ---- child: the one document the app asked to open, read-only */
    if (arg && arg[0] == '/' && !strstr(arg, ".."))
        sbx_add(p, SBX_CHILD, SBX_BIND, arg, arg)->ro = 1;

    /* ---- child: audio, only when the manifest says 'audio = yes' */
    if (p->audio) {
        SbxStep *s = sbx_add(p, SBX_CHILD, SBX_BIND, "/dev/snd", "/dev/snd");
        s->mode = 0755;
    }

    /* ---- child: processes. Private procfs, or the host's read-only. */
    if (p->procs) sbx_add(p, SBX_CHILD, SBX_PROC_RO, "/proc", "/proc");
    else          sbx_add(p, SBX_CHILD, SBX_PROC, "proc", "/proc");

    /* ---- child: switch root, drop the credential, prove the result */
    if (!p->no_chroot)
        sbx_add(p, SBX_CHILD, SBX_CHROOT, NULL, NULL);
    sbx_add(p, SBX_CHILD, SBX_DROP, NULL, NULL);
    sbx_add(p, SBX_CHILD, SBX_VERIFY, NULL, NULL);
}

/* Print the plan. Every number below is the number the executor uses: the
 * sizes come from the same struct, and the step list is the list it walks. */
static void sbx_plan_print(const SbxPlan *p, FILE *f) {
    char ns[96];
    sbx_ns_names(p->ns, ns, sizeof(ns));
    fprintf(f, "sandbox plan: %s\n", p->id);
    fprintf(f, "  uid/gid    : %d/%d, groups empty (setgroups(0))\n", p->uid, p->gid);
    fprintf(f, "  procs      : %s\n", p->procs
            ? "yes -> NO pid namespace, host /proc bound read-only"
            : "no  -> its own pid namespace, a fresh procfs");
    fprintf(f, "  unshare    : %s\n", ns);
    fprintf(f, "  buffer     : stride %d * maxh %d * 4 = %llu B\n",
            p->stride, p->maxh, p->buf_bytes);
    fprintf(f, "  /dev/shm   : 2 * %llu + %llu = %llu B (%.2f MiB)  [2x buffer + 1 MiB]\n",
            p->buf_bytes, SBX_MIB, p->shm_size, (double)p->shm_size / (double)SBX_MIB);
    fprintf(f, "  /tmp       : 2 * %llu + %llu = %llu B (%.2f MiB)  [2x buffer + 1 MiB]\n",
            p->buf_bytes, SBX_MIB, p->tmp_size, (double)p->tmp_size / (double)SBX_MIB);
    fprintf(f, "  jail       : %s\n", p->jail);
    if (p->reuse)
        fprintf(f, "  note       : another window of this app is up -> its tmpfs is reused\n");
    for (int i = 0; i < p->nstep; i++) {
        const SbxStep *s = &p->step[i];
        const char *who = s->where == SBX_PARENT ? "parent" : "child ";
        switch (s->op) {
        case SBX_MKDIR:
            fprintf(f, "  %2d %s mkdir -p %s (mode %04o, root:root)\n",
                    i + 1, who, s->dst, s->mode);
            break;
        case SBX_TMPFS: {
            char fl[32] = "";
            if (s->flags & SBX_F_NOSUID) snprintf(fl + strlen(fl), 8, ",nosuid");
            if (s->flags & SBX_F_NODEV)  snprintf(fl + strlen(fl), 7, ",nodev");
            if (s->flags & SBX_F_NOEXEC) snprintf(fl + strlen(fl), 8, ",noexec");
            fprintf(f, "  %2d %s mount -t tmpfs -o %s%s %s %s\n",
                    i + 1, who, s->opts, fl, s->src, s->dst);
            break;
        }
        case SBX_CHOWN:
            fprintf(f, "  %2d %s chown -R %s %d:%d (only if the owner differs), mode %04o\n",
                    i + 1, who, s->dst, s->uid, s->uid, s->mode);
            break;
        case SBX_BIND:
            fprintf(f, "  %2d %s mount --bind%s %s %s%s (mode %04o)\n",
                    i + 1, who, s->ro ? ",ro" : "", s->src, p->jail, s->dst, s->mode);
            break;
        case SBX_PROC:
            fprintf(f, "  %2d %s mount -t proc proc %s/proc (the app's own pid namespace)\n",
                    i + 1, who, p->jail);
            break;
        case SBX_PROC_RO:
            fprintf(f, "  %2d %s mount --bind,ro /proc %s/proc (procs = yes)\n",
                    i + 1, who, p->jail);
            break;
        case SBX_CHROOT:
            fprintf(f, "  %2d %s chroot %s + chdir /\n", i + 1, who, p->jail);
            break;
        case SBX_DROP:
            fprintf(f, "  %2d %s setgroups(0, NULL); setgid(%d); setuid(%d)\n",
                    i + 1, who, p->gid, p->uid);
            break;
        case SBX_VERIFY:
            fprintf(f, "  %2d %s verify uid==%d, / and /bin NOT writable, /tmp and /root writable\n",
                    i + 1, who, p->uid);
            break;
        default:
            fprintf(f, "  %2d %s ?\n", i + 1, who);
            break;
        }
    }
}

/* What to do with the path a mount will land on. The runtime lstat()s, turns
 * the mode into one of the four types below and hands the result here; keeping
 * the decision in one pure function is what makes "a symlink target is
 * refused" provable on a host that has no mount(2). */
enum { SBX_TYPE_DIR = 1, SBX_TYPE_FILE = 2, SBX_TYPE_LINK = 3, SBX_TYPE_OTHER = 4 };
enum { SBX_T_MAKE = 0, SBX_T_DIR = 1, SBX_T_FILE = 2,
       SBX_T_NO_SYMLINK = 3, SBX_T_NO_TYPE = 4 };

static int sbx_target_verdict(int lstat_err, int type, int want_dir) {
    if (lstat_err) return lstat_err == ENOENT ? SBX_T_MAKE : SBX_T_NO_TYPE;
    if (type == SBX_TYPE_LINK) return SBX_T_NO_SYMLINK;
    if (want_dir) return type == SBX_TYPE_DIR ? SBX_T_DIR : SBX_T_NO_TYPE;
    return type == SBX_TYPE_FILE ? SBX_T_FILE : SBX_T_NO_TYPE;
}

/* One "key = value" line of a bundle manifest, pure text -> WmApp, so the host
 * harness can prove what a manifest means without a filesystem.
 *
 * The one line that WEAKENS the sandbox is spelled out here and in
 * docs/SECURITY.md:
 *
 *   default (no line): unshare(CLONE_NEWPID) + a fresh procfs, so the app sees
 *     itself and its own children only, and cannot signal the desktop or any
 *     other app.
 *   procs = yes: no pid namespace; the host /proc is bound read-only instead,
 *     so the app can see every process name and command line on the machine
 *     (it still cannot read another uid's environ/mem/fd, cannot signal another
 *     uid, and still has its own mount namespace, tmpfs and appdata, and no
 *     network).
 *
 * Only an app whose purpose is to read other processes needs it - sysmon today.
 * A value other than "yes" is ignored rather than guessed at.
 */
static void sbx_manifest_line(WmApp *a, const char *line) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    char *cr = strchr(buf, '\r');
    if (cr) *cr = 0;
    char *eq = strchr(buf, '=');
    if (!eq) return;
    *eq = 0;
    char *k = buf, *v = eq + 1;
    while (*k == ' ') k++;
    while (*v == ' ') v++;
    char *ke = k + strlen(k);
    while (ke > k && ke[-1] == ' ') *--ke = 0;
    /* trailing whitespace in the value is a typo, not part of a name: strip it
     * so a manifest saved with CRLF still means what it says */
    char *ve = v + strlen(v);
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;

    if (!strcmp(k, "name")) snprintf(a->name, sizeof(a->name), "%s", v);
    else if (!strcmp(k, "desc")) snprintf(a->desc, sizeof(a->desc), "%s", v);
    else if (!strcmp(k, "exec")) snprintf(a->exe, sizeof(a->exe), "%s", v);
    else if (!strcmp(k, "network") && !strcmp(v, "host")) a->net = 1;
    else if (!strcmp(k, "audio") && !strcmp(v, "yes")) a->audio = 1;
    else if (!strcmp(k, "procs") && !strcmp(v, "yes")) a->procs = 1;
    else if (!strcmp(k, "chroot") && !strcmp(v, "no")) a->no_chroot = 1;
    else if (!strcmp(k, "host") && !strcmp(v, "yes")) a->host_ns = 1;
}


/* Everything below this line is the server: Linux only. */
#ifndef MYSTIC_SANDBOX_PLAN

static int listen_fd = -1;
static int shm_seq = 0;

/* The reason the last launch failed, kept for the launcher to pick up with
 * wms_take_launch_error(). Empty when the last launch worked. */
static char last_launch_error[192];

/* ---------------------------------------------------------- paths
 * The run dir and the two bundle roots are constants in the shipped system.
 * MYSTIC_RUN_DIR and MYSTIC_APP_ROOTS ("<image>:<installed>") let the host
 * harness point the real scanner at a fixture tree - the same test hook pkg has
 * with MYSTIC_APPS. Only the user of this process can set its environment, so
 * neither is a way in; a desktop with them unset behaves exactly as before.
 */
static const char *run_dir(void) {
    const char *e = getenv("MYSTIC_RUN_DIR");
    return (e && e[0]) ? e : WM_RUN_DIR;
}

static void run_file(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", run_dir(), leaf);
}

static void app_roots(char *image, size_t ni, char *installed, size_t nw) {
    snprintf(image, ni, "%s", WM_APP_DIR);
    snprintf(installed, nw, "%s", WM_APP_DIR_RW);
    const char *e = getenv("MYSTIC_APP_ROOTS");
    if (!e || !e[0]) return;
    const char *colon = strchr(e, ':');
    if (!colon) return;
    size_t l = (size_t)(colon - e);
    if (l > 0 && l < ni) { memcpy(image, e, l); image[l] = 0; }
    if (colon[1] && strlen(colon + 1) < nw)
        snprintf(installed, nw, "%s", colon + 1);
}

/* ---------------------------------------------------------- helpers */
static void sendf(WmClient *c, const char *fmt, ...) {
    if (!c || c->fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    /* a short write on a socket is possible; a chatty error is better
     * than a silent one */
    ssize_t w = write(c->fd, buf, (size_t)n);
    (void)w;
}

static int screen_w = 1280, screen_h = 800;

void wms_set_screen(int w, int h) {
    if (w > 0 && h > 0) { screen_w = w; screen_h = h; }
}

/* One buffer per client. The server creates and maps it; the app opens the
 * path the OK message handed it. Both must resolve to the same inode:
 *
 *   jailed : <run>/jail/<id>/dev/shm/<leaf>   ==  /dev/shm/<leaf> in the jail
 *   host   : /dev/shm/<leaf>                  (a client the desktop did not
 *                                              launch, e.g. an app started from
 *                                              a shell; that one is not chrooted)
 *
 * The file is 0600 and owned by the uid the KERNEL says the app runs as, and it
 * lives in that app's own 0700 tmpfs, so no other app can open it - let alone
 * ftruncate it. That was the audit's "one app can SIGBUS the desktop" hole.
 *
 * Why it is still created at HELLO and not before the fork: HELLO carries the
 * size. Why that works at all: the tmpfs is mounted by the server, in the host
 * namespace, so a file created here after the app started is the same file the
 * app sees at /dev/shm/<leaf>.
 */
static int make_shm(WmClient *c) {
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "mystic-%d-%d", (int)getpid(), ++shm_seq);
    snprintf(c->shm_path, sizeof(c->shm_path), "/dev/shm/%s", leaf);
    if (c->jailed)
        snprintf(c->shm_host, sizeof(c->shm_host), "%s/jail/%s/dev/shm/%s",
                 run_dir(), c->jail_id, leaf);
    else
        snprintf(c->shm_host, sizeof(c->shm_host), "/dev/shm/%s", leaf);

    unlink(c->shm_host);
    int fd = open(c->shm_host, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "wms: cannot create %s: %s\n", c->shm_host, strerror(errno));
        c->shm_path[0] = 0;
        return -1;
    }

    size_t bytes = (size_t)c->stride * (size_t)c->maxh * 4;
    /* A buffer lives on a tmpfs that is smaller than the machine (9.2 MiB per
     * app inside the jail, 128 MiB for the host's /dev/shm). Read the free
     * space before touching a page: ftruncate() beyond a tmpfs limit succeeds
     * and the SIGBUS only arrives on the first write - which would be this
     * memset, in the desktop. Fail the client instead. The per-app size is
     * 2 buffers + 1 MiB, so a second window of the same app fits and a third
     * gets one honest line rather than a dead compositor. */
    {
        struct statfs sfs;
        if (statfs(c->shm_host, &sfs) == 0) {
            unsigned long long free_b = (unsigned long long)sfs.f_bsize *
                                        (unsigned long long)sfs.f_bfree;
            if ((unsigned long long)bytes > free_b) {
                fprintf(stderr, "wms: %s: a %zu B buffer does not fit in the "
                                "%llu B left in %s\n",
                        c->id[0] ? c->id : "client", bytes, free_b, c->shm_host);
                close(fd);
                unlink(c->shm_host);
                c->shm_path[0] = 0;
                return -1;
            }
        }
    }
    if (ftruncate(fd, (off_t)bytes) < 0) {
        fprintf(stderr, "wms: cannot size %s to %zu bytes: %s\n",
                c->shm_host, bytes, strerror(errno));
        close(fd);
        unlink(c->shm_host);
        c->shm_path[0] = 0;
        return -1;
    }

    c->px = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c->px == MAP_FAILED) {
        fprintf(stderr, "wms: cannot map %s: %s\n", c->shm_host, strerror(errno));
        close(fd);
        unlink(c->shm_host);
        c->px = NULL;
        c->shm_path[0] = 0;
        return -1;
    }
    c->shm_fd = fd;
    /* start black so an app that has not drawn yet is not garbage */
    memset(c->px, 0, bytes);
    /* the app runs as its own uid and still has to draw */
    if (chown(c->shm_host, (uid_t)c->uid, (gid_t)c->uid) != 0)
        fprintf(stderr, "wms: chown %s to %d failed: %s\n",
                c->shm_host, c->uid, strerror(errno));
    return 0;
}

static void free_shm(WmClient *c) {
    if (c->px && c->px != MAP_FAILED) {
        size_t bytes = (size_t)c->stride * (size_t)c->maxh * 4;
        munmap(c->px, bytes);
    }
    c->px = NULL;
    if (c->shm_fd >= 0) close(c->shm_fd);
    c->shm_fd = -1;
    if (c->shm_host[0]) unlink(c->shm_host);
    c->shm_host[0] = 0;
    c->shm_path[0] = 0;
}

/* ---------------------------------------------------------- start */

/* ------------------------------------------------------------- media sample
 *
 * The image carries one real, short H.264 clip at
 * /usr/share/mystic/media/clip.mp4 (320x176, 2.8 s, encoded with libx264
 * through VLC itself; the generator and the proof are in docs/MULTIMEDIA.md).
 * It exists so the
 * owner can open a video on a machine that has never had one, without
 * downloading anything.
 *
 * It cannot simply be staged at /root/media in the initramfs: on every normal
 * boot stage/init mounts the data disk on /root, which hides whatever the
 * initramfs put there, and a fresh disk is empty. So the window server - the
 * first process that runs as root after the disk is mounted - puts one copy
 * in place. It never overwrites: a file the user already has at that path is
 * theirs. A partial copy is removed, so a full disk at boot retries next time
 * instead of leaving a truncated MP4 that VLC cannot demux.
 */
static void provision_media(void) {
    static const char *const names[] = { "clip.mp4" };
    char src[256], dst[256], tmp[256];
    unsigned char buf[65536];

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        struct stat st;
        long long src_size;
        snprintf(src, sizeof(src), "/usr/share/mystic/media/%s", names[i]);
        snprintf(dst, sizeof(dst), "/root/media/%s", names[i]);
        snprintf(tmp, sizeof(tmp), "%s.part", dst);

        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
            continue;                       /* this image has no sample */
        /* Saved now: the stat() of the destination below may fail, and POSIX
         * leaves the buffer undefined after a failure - reading st_size from
         * it would then compare the copy against garbage. */
        src_size = (long long)st.st_size;
        if (stat(dst, &st) == 0)
            continue;                       /* already provisioned or user's */

        mkdir("/root", 0755);
        if (mkdir("/root/media", 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "wms: media: cannot create /root/media: %s\n",
                    strerror(errno));
            continue;
        }
        remove(tmp);
        int in = open(src, O_RDONLY);
        if (in < 0) {
            fprintf(stderr, "wms: media: cannot read %.120s: %s\n", src, strerror(errno));
            continue;
        }
        int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "wms: media: cannot create %.120s: %s\n", tmp, strerror(errno));
            close(in);
            continue;
        }
        long long copied = 0;
        int failed = 0;
        for (;;) {
            ssize_t r = read(in, buf, sizeof(buf));
            if (r == 0) break;
            if (r < 0) {
                if (errno == EINTR) continue;
                failed = 1;
                fprintf(stderr, "wms: media: read %.120s failed: %s\n", src, strerror(errno));
                break;
            }
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = write(out, buf + off, (size_t)(r - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    failed = 1;
                    fprintf(stderr, "wms: media: write %.120s failed: %s\n",
                            tmp, strerror(errno));
                    break;
                }
                off += w;
            }
            if (failed) break;
            copied += r;
        }
        close(in);
        if (close(out) != 0) failed = 1;
        if (failed || copied != src_size) {
            remove(tmp);                     /* retry on the next boot */
            continue;
        }
        if (rename(tmp, dst) != 0) {
            fprintf(stderr, "wms: media: rename %.120s failed: %s\n", dst, strerror(errno));
            remove(tmp);
            continue;
        }
        fprintf(stderr, "wms: media: %s (%lld bytes) from %s\n",
                dst, copied, src);
    }
}

int wms_start(void) {
    char dir[64];
    snprintf(dir, sizeof(dir), "%s", WM_SOCKET);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    /* The sandbox's own anchor: one directory per bundle, made by
     * sandbox_prepare() below it. Created here so the first launch does not
     * have to invent it. */
    snprintf(dir, sizeof(dir), "%s/jail", run_dir());
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "wms: cannot create %s: %s\n", dir, strerror(errno));
    fprintf(stderr, "wms: sandbox: uids %d..%d, one per bundle (gid == uid, "
                    "no supplementary groups), private /dev/shm and /tmp per app\n",
            SBX_UID_BASE, SBX_UID_TOP - 1);

    unlink(WM_SOCKET);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WM_SOCKET);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    listen(listen_fd, 8);
    /* connect() needs write permission on the socket inode, and apps run as
     * their own sandbox uids (2000.., see the plan above). This is a
     * single-tenant system - every local process is one of our sandboxed apps -
     * so the socket is connectable, while what is behind it (the filesystem,
     * every other app's pixels) is not: see docs/SECURITY.md. */
    chmod(WM_SOCKET, 0666);
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    fprintf(stderr, "wms: listening on %s\n", WM_SOCKET);

    /* the one file the user can open before installing anything */
    provision_media();
    return 0;
}

/* ---------------------------------------------------------- accept */
static WmClient *slot_free(WmClient *cl, int n) {
    for (int i = 0; i < n; i++) if (!cl[i].used) return &cl[i];
    return NULL;
}

static void handle_line(WmClient *c, char *line) {
    if (!strncmp(line, "HELLO ", 6)) {
        char title[80] = "app";
        int w = 400, h = 300;
        /* HELLO <id> <w> <h> <title...> */
        char id[32] = "app";
        if (sscanf(line + 6, "%31s %d %d %79[^\n]", id, &w, &h, title) < 3) {
            sendf(c, "ERR bad hello\n");
            return;
        }
        if (w < 80) w = 80;
        if (h < 60) h = 60;
        /* A window cannot be bigger than the screen, and the buffer is exactly
         * one screen: stride * maxh * 4 is then the largest buffer the plan
         * reserved a tmpfs for. The old 1600x1200 ceiling let a client ask for
         * more than the screen and get a buffer the size of neither. */
        if (w > screen_w) w = screen_w;
        if (h > screen_h) h = screen_h;

        /* Who is this, really? The kernel answers. SO_PEERCRED carries the
         * sender's uid and pid as OUR namespace sees them, and an app cannot
         * forge it. A uid from the sandbox pool means the desktop launched this
         * process into a jail, so its buffer belongs in that jail's tmpfs and
         * the bundle id is the one the launch recorded - not the one the app
         * typed. That keeps a lying HELLO from aiming a buffer at another
         * app's /dev/shm or at another app's file-picker directory. */
        c->uid = 0;
        c->pid = 0;
        c->jailed = 0;
        c->jail_id[0] = 0;
        {
            struct ucred uc;
            socklen_t ul = sizeof(uc);
            if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &uc, &ul) == 0) {
                SbxCred *cred = sbx_cred_by_uid((int)uc.uid);
                c->uid = (int)uc.uid;
                c->pid = (long)uc.pid;
                if (cred) {
                    c->jailed = 1;
                    /* An app launched in the host namespaces maps /dev/shm
                     * itself, so its buffer belongs in the real one: the
                     * credential records that when the uid is reserved. */
                    if (cred->host) c->jailed = 0;
                    snprintf(c->jail_id, sizeof(c->jail_id), "%s", cred->id);
                    sbx_cred_add_pid(c->uid, c->pid);
                    if (strcmp(cred->id, id) != 0)
                        fprintf(stderr, "wms: uid %d calls itself \"%.31s\"; "
                                        "it was launched as \"%.31s\" and stays that\n",
                                c->uid, id, cred->id);
                }
            } else {
                fprintf(stderr, "wms: SO_PEERCRED on a new connection failed: %s\n",
                        strerror(errno));
            }
        }

        /* A second HELLO on the same connection replaces the window. Release
         * the previous buffer first: make_shm overwrites shm_path/shm_fd/px,
         * so without this the old mapping, its fd and its /dev/shm file leak
         * for the lifetime of the server. */
        if (c->shm_fd >= 0 || c->px || c->shm_path[0]) {
            fprintf(stderr, "wms: %s sent HELLO twice, replacing its buffer\n",
                    c->id[0] ? c->id : "client");
            free_shm(c);
        }

        /* The bundle id is authoritative for a jailed client: the launch
         * recorded it, the kernel proved which uid is connecting. For a client
         * the desktop did not launch (no sandbox uid) the id is its own - it is
         * not jailed, so nothing depends on it. */
        if (c->jailed) snprintf(c->id, sizeof(c->id), "%s", c->jail_id);
        else           snprintf(c->id, sizeof(c->id), "%s", id);
        snprintf(c->title, sizeof(c->title), "%s", title);
        c->w = w;
        c->h = h;
        /* The buffer is the whole screen, not the size the app asked to open
         * at: otherwise a client can never grow, and maximising its window just
         * draws the same small picture in a bigger frame. */
        c->maxw = screen_w;
        c->maxh = screen_h;
        c->stride = screen_w;                /* keep it simple and explicit */

        if (make_shm(c) < 0) {
            sendf(c, "ERR no shm\n");
            /* nothing to draw into: drop the connection deliberately rather
             * than leaving a client that can never present a frame */
            fprintf(stderr, "wms: %s could not get a buffer, disconnecting\n", c->id);
            wms_shutdown_client(c);
            return;
        }
        sendf(c, "OK %s %d %d %d\n", c->shm_path, c->stride, c->maxw, c->maxh);
        c->want_frame = 1;
        fprintf(stderr, "wms: %s connected %dx%d as uid %d (%s)\n",
                c->id, c->w, c->h, c->uid, c->jailed ? c->shm_host : "not jailed");
        return;
    }
    if (!strncmp(line, "FRAME", 5)) { c->want_frame = 1; return; }
    if (!strncmp(line, "TITLE ", 6)) {
        snprintf(c->title, sizeof(c->title), "%s", line + 6);
        return;
    }
    if (!strncmp(line, "CLOSE", 5)) { wms_shutdown_client(c); return; }
    if (!strncmp(line, "EVENT ", 6)) {
        /* apps may report their own status; we only log it */
        fprintf(stderr, "wms: %s says %s\n", c->id, line + 6);
        return;
    }
}

static void read_client(WmClient *c) {
    for (;;) {
        size_t space = sizeof(c->rx) - (size_t)c->rxlen - 1;
        if (space == 0) {
            /* The buffer is full and the loop above consumed every complete
             * line in it, so what is left is one line longer than rx[]. Never
             * read 0 bytes here (read()==0 means EOF) and never let rxlen run
             * past the array: reject the line explicitly and drop the client,
             * logging why. */
            fprintf(stderr, "wms: %s sent a line longer than %d bytes, disconnecting\n",
                    c->id[0] ? c->id : "client", (int)sizeof(c->rx) - 1);
            sendf(c, "ERR line too long\n");
            /* Swallow what the app is still sending before closing: closing a
             * socket that still has unread input makes the peer's kernel reset
             * the connection, which would throw away the ERR we just sent. */
            for (int i = 0; i < 16; i++) {
                char sink[256];
                ssize_t d = read(c->fd, sink, sizeof(sink));
                if (d <= 0) break;
            }
            wms_shutdown_client(c);
            return;
        }
        ssize_t n = read(c->fd, c->rx + c->rxlen, space);
        if (n > 0) {
            c->rxlen += (int)n;
            c->rx[c->rxlen] = 0;
            char *start = c->rx;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = 0;
                handle_line(c, start);
                if (!c->used) return;
                start = nl + 1;
            }
            int rest = (int)(c->rx + c->rxlen - start);
            if (rest > 0) memmove(c->rx, start, (size_t)rest);
            c->rxlen = rest;
            c->rx[rest] = 0;          /* the tail is a partial line, keep it */
            continue;
        }
        if (n == 0) { wms_shutdown_client(c); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        wms_shutdown_client(c);
        return;
    }
}

/* Called every frame with the client table. Accepts new connections and
 * reads whatever is pending. Never blocks. */
void wms_poll(WmClient *cl, int n) {
    if (listen_fd < 0) return;

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) break;

        WmClient *c = slot_free(cl, n);
        if (!c) { close(fd); continue; }
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->fd = fd;
        c->shm_fd = -1;
        c->win = -1;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fprintf(stderr, "wms: connection accepted\n");
    }

    for (int i = 0; i < n; i++)
        if (cl[i].used) read_client(&cl[i]);
}

/* ---------------------------------------------------------- notify */
void wms_notify_key(WmClient *c, int key, int mods, int press) {
    sendf(c, "KEY %d %d %d\n", key, mods, press);
}
void wms_notify_text(WmClient *c, const char *utf8) {
    /* The event stream is line oriented, so a newline inside the text would end
     * the message early: Enter reached every sandboxed app as an EMPTY text
     * event and so never did anything. Line breakers travel as 0x01. */
    char buf[512];
    size_t o = 0;
    for (const char *p = utf8 ? utf8 : ""; *p && o + 1 < sizeof(buf); p++)
        buf[o++] = (*p == 10 || *p == 13) ? 1 : *p;
    buf[o] = 0;
    sendf(c, "TEXT %s\n", buf);
}
void wms_notify_wheel(WmClient *c, int notches) {
    /* A wheel has no position of its own: the client already knows where its
     * pointer is from the MOUSE events. One number, positive = up. */
    sendf(c, "WHEEL %d\n", notches);
}
void wms_notify_mouse(WmClient *c, int x, int y, int buttons) {
    sendf(c, "MOUSE %d %d %d\n", x, y, buttons);
}
void wms_notify_resize(WmClient *c, int w, int h) {
    sendf(c, "RESIZE %d %d\n", w, h);
}
void wms_notify_close(WmClient *c) {
    sendf(c, "CLOSE\n");
}

void wms_shutdown_client(WmClient *c) {
    if (!c || !c->used) return;
    free_shm(c);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->used = 0;
    c->win = -1;
    /* The window is gone: forget the connection against the uid that owned it.
     * The uid itself goes back to the pool when that bundle has no process
     * left, and so does its tmpfs (sbx_jail_forget) - an app that keels over
     * without ever connecting is collected by the same sweep on the next
     * launch, so a broken bundle cannot eat the pool. */
    if (c->jailed) {
        sbx_cred_del_pid(c->uid, c->pid);
        SbxCred *cred = sbx_cred_by_uid(c->uid);
        fprintf(stderr, "wms: %s (uid %d) disconnected; %s\n", c->id, c->uid,
                cred ? "its other window is still up" : "uid released");
        c->jailed = 0;
        c->uid = 0;
        c->pid = 0;
        c->jail_id[0] = 0;
    }
}

/* ---------------------------------------------------------- apps */
static int cmp_app(const void *a, const void *b) {
    return strcmp(((const WmApp *)a)->id, ((const WmApp *)b)->id);
}

/* Two roots, on purpose: the image's own bundles, and /root/apps where pkg
 * unpacks what the user installed - /root is the only place that survives a
 * reboot. Scanning just the first meant a package installed from the Store
 * could never be launched; and because the launcher then exec'd everything from
 * WM_APP_DIR, the tiles it did draw for installed bundles were dead. Each
 * WmApp now remembers its root and wms_launch() uses it. */
static int scan_root(WmApp *out, int n, int max, const char *root, int from_image) {
    if (strlen(root) >= sizeof(out[0].root)) {
        fprintf(stderr, "wms: app root %.80s is too long to remember; skipped\n", root);
        return n;
    }
    DIR *d = opendir(root);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        /* the id becomes a path component twice (manifest, payload) */
        if (strchr(e->d_name, '/')) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/manifest", root, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        WmApp *a = &out[n];
        memset(a, 0, sizeof(*a));
        a->from_image = from_image;
        snprintf(a->id, sizeof(a->id), "%s", e->d_name);
        snprintf(a->root, sizeof(a->root), "%s", root);

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            sbx_manifest_line(a, line);
        }
        fclose(f);
        if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", a->id);
        if (!a->exe[0]) snprintf(a->exe, sizeof(a->exe), "bin");
        n++;
    }
    closedir(d);
    return n;
}

int wms_bundle_payload(const WmApp *app, char *out, size_t cap,
                       char *why, size_t whyn) {
    if (why && whyn) why[0] = 0;
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!app) return -1;

    /* exec= and the directory name both come off the disk, so both are
     * untrusted: a bare file name and a bare directory name, or nothing. A
     * '..' or a slash here would point the launcher at a file of the bundle's
     * choosing instead of its own payload. */
    if (!app->id[0] || strchr(app->id, '/') || strstr(app->id, "..")) {
        if (why && whyn) snprintf(why, whyn, "unusable bundle id '%.31s'", app->id);
        return -1;
    }
    if (!app->exe[0] || strchr(app->exe, '/') || strstr(app->exe, "..")) {
        if (why && whyn) snprintf(why, whyn, "unsafe exec '%.31s'", app->exe);
        return -1;
    }

    const char *root = app->root[0] ? app->root : WM_APP_DIR;
    int k = snprintf(out, cap, "%s/%s/%s", root, app->id, app->exe);
    if (k < 0 || (size_t)k >= cap) {
        if (why && whyn) snprintf(why, whyn, "path longer than %d bytes", (int)cap - 1);
        out[0] = 0;
        return -1;
    }

    /* The three ways a bundle is dead: nothing there, not a file, not
     * executable. Each one is reported with the kernel's own words, because
     * "it did not start" is not a diagnosis. */
    struct stat st;
    if (stat(out, &st) != 0) {
        if (why && whyn) snprintf(why, whyn, "%s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        if (why && whyn) snprintf(why, whyn, "not a regular file");
        return -1;
    }
    if (access(out, X_OK) != 0) {
        if (why && whyn) snprintf(why, whyn, "%s", strerror(errno));
        return -1;
    }
    return 0;
}

/* 1 when the payload is there and executable. Used to decide which of two
 * bundles with the same id is worth a tile. */
static int payload_ok(const WmApp *a) {
    char path[512], why[96];
    return wms_bundle_payload(a, path, sizeof(path), why, sizeof(why)) == 0;
}

/* Two roots can hold the same id, and exactly one of them gets the tile. Two
 * rules, both of which shipping needs:
 *
 *   - a bundle whose payload is missing or not executable cannot start, so it
 *     must not shadow one that can. A half-finished install then leaves the
 *     shipped app working instead of turning a good tile into a dead one.
 *
 *   - when both can start, the installed copy wins. /usr/share is the image: it
 *     cannot be upgraded without reflashing, while /root/apps is only there
 *     because the user installed it on purpose. This is what makes the 'vlc'
 *     package replace the shipped vlcplayer stub instead of leaving a second
 *     tile for one program.
 *
 * A tie (neither works) keeps the image's copy, so the error names the bundle
 * that shipped with the OS. The decision is symmetric in the argument order
 * because qsort is not stable: whichever copy comes first, the same one wins. */
static int merge_dups(WmApp *out, int n) {
    int keep = 0;
    for (int i = 0; i < n; i++) {
        int at = -1;
        for (int j = 0; j < keep; j++)
            if (!strcmp(out[j].id, out[i].id)) { at = j; break; }
        if (at < 0) {
            if (keep != i) out[keep] = out[i];
            keep++;
            continue;
        }
        int cand_ok = payload_ok(&out[i]);
        int kept_ok = payload_ok(&out[at]);
        int take;
        const char *why;
        if (cand_ok != kept_ok) {
            /* exactly one can start: it gets the tile, whatever order it came in */
            take = cand_ok;
            why = take ? "the other copy has no runnable payload"
                       : "this copy has no runnable payload";
        } else if (cand_ok) {
            /* both run: the installed copy wins, so a package can replace a
             * stub the image shipped */
            take = !out[i].from_image && out[at].from_image;
            why = "both run, and the installed copy wins";
        } else {
            /* neither runs: keep the shipped bundle, so the error names it */
            take = out[i].from_image && !out[at].from_image;
            why = "neither copy runs; keeping the shipped one";
        }
        if (take) {
            fprintf(stderr, "wms: bundle %s: launching %.120s, not %.120s (%s)\n",
                    out[i].id, out[i].root, out[at].root, why);
            out[at] = out[i];
        } else {
            fprintf(stderr, "wms: bundle %s: launching %.120s, not %.120s (%s)\n",
                    out[at].id, out[at].root, out[i].root, why);
        }
    }
    return keep;
}

int wms_scan_apps(WmApp *out, int max) {
    if (!out || max <= 0) return 0;
    char image[256], installed[256];
    app_roots(image, sizeof(image), installed, sizeof(installed));
    int n = scan_root(out, 0, max, image, 1);
    n = scan_root(out, n, max, installed, 0);
    if (n == max)
        fprintf(stderr, "wms: app list is full (%d); a bundle may be missing\n", max);

    /* readdir order is arbitrary; sort so the launcher is stable. */
    qsort(out, (size_t)n, sizeof(WmApp), cmp_app);
    return merge_dups(out, n);
}

int wms_apps_poll_rescan(WmApp *out, int *count, int max) {
    if (!out || max <= 0) return 0;

    char req[256];
    run_file(req, sizeof(req), WM_RESCAN_LEAF);
    struct stat st;
    if (stat(req, &st) != 0) return 0;

    /* Take the request before scanning: pkg writes it after the bundle is in
     * place, and a request we cannot remove must not turn into a rescan (and a
     * few hundred stat() calls) on every frame. Remember which one we could not
     * delete instead. */
    static long long stubborn_ino = -1, stubborn_mtime = -1;
    if (unlink(req) != 0 && errno != ENOENT) {
        if ((long long)st.st_ino == stubborn_ino && (long long)st.st_mtime == stubborn_mtime)
            return 0;
        stubborn_ino = (long long)st.st_ino;
        stubborn_mtime = (long long)st.st_mtime;
        fprintf(stderr, "wms: cannot remove %s (%s); scanning anyway\n", req, strerror(errno));
    } else {
        stubborn_ino = -1;
        stubborn_mtime = -1;
    }

    int n = wms_scan_apps(out, max);
    if (count) *count = n;
    fprintf(stderr, "wms: rescan: %d bundle(s)\n", n);
    return 1;
}

/* ---------------------------------------------------------- launch errors
 * A tile that does not start used to fail three syscalls deep in a forked
 * child and leave nothing behind but a pid. Every failure now becomes one line
 * that says which bundle, which path and what the kernel said - logged, handed
 * to the launcher through wms_take_launch_error(), and written to
 * WM_LAUNCH_ERR so it survives even a launcher that never asks.
 */
static void launch_failed(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_launch_error, sizeof(last_launch_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "wms: launch failed: %s\n", last_launch_error);

    char path[256];
    run_file(path, sizeof(path), WM_LAUNCH_LEAF);
    mkdir(run_dir(), 0755);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "wms: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", last_launch_error);
    fclose(f);
}

int wms_take_launch_error(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!last_launch_error[0]) { out[0] = 0; return 0; }
    snprintf(out, (size_t)cap, "%s", last_launch_error);
    last_launch_error[0] = 0;
    return 1;
}

/* ---------------------------------------------------------- sandbox
 * Every app runs in its own mount/IPC/UTS/PID namespace, chrooted into a jail
 * that contains its own payload, its own bundle (read-only, at /app), its own
 * size-limited /tmp and /dev/shm, its own appdata as /root, the window socket
 * and nothing else. It runs as its own uid from the pool above, so even if it
 * escaped the chroot it could not read /root (root-owned, mode 0700) or any
 * other app's buffer.
 *
 * The plan built by sbx_plan_init() is what runs: sandbox_prepare() executes
 * the parent steps and sandbox_child() the child steps, in order. Fail closed
 * everywhere - an app that cannot be isolated does not run.
 */

/* The one place the name spaces become kernel bits. */
static int sbx_clone_flags(unsigned ns) {
    int f = 0;
    if (ns & SBX_NS_MNT) f |= CLONE_NEWNS;
    if (ns & SBX_NS_IPC) f |= CLONE_NEWIPC;
    if (ns & SBX_NS_UTS) f |= CLONE_NEWUTS;
    if (ns & SBX_NS_NET) f |= CLONE_NEWNET;
    if (ns & SBX_NS_PID) f |= CLONE_NEWPID;
    return f;
}

/* A bundle whose processes are all gone gives its tmpfs back. Called from
 * sbx_cred_gc() (portable part) through the prototype there. */
static void sbx_jail_forget(const char *id) {
    if (!id || !id[0] || strchr(id, '/') || strstr(id, "..")) return;
    char path[SBX_PATH];
    snprintf(path, sizeof(path), "%s/jail/%s/dev/shm", run_dir(), id);
    if (umount2(path, MNT_DETACH) == 0) fprintf(stderr, "wms: sandbox: unmounted %s\n", path);
    snprintf(path, sizeof(path), "%s/jail/%s/tmp", run_dir(), id);
    if (umount2(path, MNT_DETACH) == 0) fprintf(stderr, "wms: sandbox: unmounted %s\n", path);
}

/* st_mode -> the four types the verdict understands. */
static int sbx_type_of(unsigned mode) {
    if (S_ISLNK(mode)) return SBX_TYPE_LINK;
    if (S_ISDIR(mode)) return SBX_TYPE_DIR;
    if (S_ISREG(mode)) return SBX_TYPE_FILE;
    return SBX_TYPE_OTHER;
}

/* Pull apart <base><sub> one component at a time, creating what is missing and
 * refusing to follow a symlink anywhere on the way.
 *
 * This is the hole the audit called out: mount(2) follows a symlink in the
 * target path, and an app can plant symlinks in the one directory it owns -
 * its appdata, which is bound at /root. Without this check a bundle could put
 * "-> /etc" in its appdata, ask the desktop to open /root/x, and have the next
 * mount land on a host path of its choosing - as root. Nothing is mounted on a
 * path that is, or goes through, a symlink. */
static int sbx_walk(const char *base, const char *sub, int want_dir, unsigned mode,
                    char *out, size_t cap, char *err, size_t errn) {
    int n = snprintf(out, cap, "%s%s", base, sub ? sub : "");
    if (n < 0 || (size_t)n >= cap) {
        snprintf(err, errn, "path longer than %d bytes", (int)cap - 1);
        return -1;
    }
    char *p = out + strlen(base);
    while (*p == '/') p++;
    if (!*p) {
        /* the base itself */
        struct stat st;
        if (lstat(out, &st) == 0) {
            if (S_ISLNK(st.st_mode)) { snprintf(err, errn, "refusing to use %s: it is a symlink", out); return -1; }
            if (!S_ISDIR(st.st_mode)) { snprintf(err, errn, "refusing to use %s: not a directory", out); return -1; }
            return 0;
        }
        if (errno != ENOENT || mkdir(out, mode ? (mode_t)mode : 0755) != 0) {
            snprintf(err, errn, "mkdir %s: %s", out, strerror(errno));
            return -1;
        }
        return 0;
    }
    for (;;) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        int last = slash == NULL;
        struct stat st;
        int r = lstat(out, &st);
        int verdict = sbx_target_verdict(r ? errno : 0,
                                         r ? SBX_TYPE_OTHER : sbx_type_of(st.st_mode),
                                         last ? want_dir : 1);
        if (verdict == SBX_T_MAKE) {
            if (last && !want_dir) {
                int fd = open(out, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW,
                              mode ? (mode_t)mode : 0644);
                if (fd < 0) { snprintf(err, errn, "create %s: %s", out, strerror(errno)); return -1; }
                close(fd);
            } else if (mkdir(out, last ? (mode ? (mode_t)mode : 0755) : 0755) != 0) {
                snprintf(err, errn, "mkdir %s: %s", out, strerror(errno));
                return -1;
            }
        } else if (verdict == SBX_T_NO_SYMLINK) {
            snprintf(err, errn, "refusing to mount on %s: it is a symlink", out);
            return -1;
        } else if (verdict == SBX_T_NO_TYPE) {
            snprintf(err, errn, "refusing to mount on %s: wrong file type%s", out,
                     r ? " (or it cannot be inspected)" : "");
            return -1;
        }
        if (last) break;
        *slash = '/';
        p = slash + 1;
        while (*p == '/') p++;
        if (!*p) break;
    }
    return 0;
}

/* Recursive lchown with FTW_PHYS: never follows a symlink inside the app's own
 * tree, so a link the app planted cannot make root chown a host file. */
static uid_t sbx_owner_uid;
static gid_t sbx_owner_gid;
static int   sbx_owner_err;

static int sbx_chown_cb(const char *path, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    if (flag == FTW_NS || flag == FTW_DNR) {
        if (!sbx_owner_err) sbx_owner_err = errno ? errno : EACCES;
        return 0;
    }
    if (lchown(path, sbx_owner_uid, sbx_owner_gid) != 0 && errno != ENOENT && !sbx_owner_err)
        sbx_owner_err = errno;
    return 0;                     /* keep going; report the first failure */
}

/* The app's own /root. One owner per bundle, so when the uid assigned to a
 * bundle changes (another app took its slot after a reboot) the tree is
 * re-owned once; the fast path is a single stat().
 *
 * A failed re-own is logged and the launch continues: the app starts and says
 * which file it cannot write, which is better than a tile that does nothing. */
static int sbx_adopt_appdata(const char *path, int uid, int gid,
                             char *err, size_t errn) {
    char abs[SBX_PATH * 2];
    if (sbx_walk("", path, 1, 0700, abs, sizeof(abs), err, errn) != 0) return -1;
    struct stat st;
    if (stat(abs, &st) != 0) {
        snprintf(err, errn, "stat %s: %s", abs, strerror(errno));
        return -1;
    }
    if (st.st_uid != (uid_t)uid || st.st_gid != (gid_t)gid) {
        sbx_owner_uid = (uid_t)uid;
        sbx_owner_gid = (gid_t)gid;
        sbx_owner_err = 0;
        if (nftw(abs, sbx_chown_cb, 16, FTW_PHYS) != 0 && !sbx_owner_err) sbx_owner_err = errno;
        if (sbx_owner_err)
            fprintf(stderr, "wms: appdata %s: could not re-own everything (%s); "
                            "the app will report what it cannot write\n",
                    abs, strerror(sbx_owner_err));
        else
            fprintf(stderr, "wms: appdata %s: re-owned %d:%d (was %d:%d)\n", abs, uid, gid,
                    (int)st.st_uid, (int)st.st_gid);
    }
    if ((st.st_mode & 0777) != 0700 && chmod(abs, 0700) != 0)
        fprintf(stderr, "wms: appdata %s: chmod 0700 failed: %s\n", abs, strerror(errno));
    return 0;
}

/* Parent side: the skeleton and the two per-app tmpfs mounts, in the desktop's
 * own namespaces, before the fork. Returns 0, or -1 with a sentence in err. */
static int sandbox_prepare(const SbxPlan *p, char *err, size_t errn) {
    char path[SBX_PATH * 2], anchor[SBX_PATH];
    if (mkdir(run_dir(), 0755) != 0 && errno != EEXIST) {
        snprintf(err, errn, "mkdir %s: %s", run_dir(), strerror(errno));
        return -1;
    }
    snprintf(anchor, sizeof(anchor), "%s/jail", run_dir());
    if (mkdir(anchor, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errn, "mkdir %s: %s", anchor, strerror(errno));
        return -1;
    }
    size_t al = strlen(anchor);
    for (int i = 0; i < p->nstep; i++) {
        const SbxStep *s = &p->step[i];
        if (s->where != SBX_PARENT) continue;
        /* One parent step legitimately points elsewhere: the appdata chown,
         * which targets /root/appdata/<id>. Everything else must be inside
         * the jail anchor. This check refused every launch until it allowed
         * that one case - the plan was right, the guard was too strict. */
        if (s->op == SBX_CHOWN) {
            if (strncmp(s->dst, "/root/appdata/", 14) != 0) {
                snprintf(err, errn, "sandbox step %d: refusing to chown outside appdata: %.80s",
                         i + 1, s->dst);
                return -1;
            }
        } else if (strncmp(s->dst, anchor, al) != 0 ||
                   (s->dst[al] != 0 && s->dst[al] != '/')) {
            snprintf(err, errn, "sandbox step %d points outside %s", i + 1, anchor);
            return -1;
        }
        switch (s->op) {
        case SBX_MKDIR:
            if (sbx_walk(anchor, s->dst + al, 1, s->mode, path, sizeof(path), err, errn) != 0)
                return -1;
            break;
        case SBX_TMPFS: {
            if (s->dst_is_abs) {
                snprintf(path, sizeof(path), "%s", s->dst);
                mkdir(path, 0755);
            } else if (sbx_walk(anchor, s->dst + al, 1, 0755, path, sizeof(path), err, errn) != 0) {
                return -1;
            }
            struct statfs sfs;
            int already = p->reuse && statfs(path, &sfs) == 0 &&
                          (unsigned long)sfs.f_type == (unsigned long)TMPFS_MAGIC;
            if (already) {
                /* a second window of the same app shares the tmpfs the first
                 * one mounted - same uid, same trust domain, and detaching it
                 * would break the first window's /tmp bridge underneath it */
                fprintf(stderr, "wms: sandbox %s: reusing the tmpfs at %s\n", p->id, path);
            } else {
                if (umount2(path, MNT_DETACH) == 0)
                    fprintf(stderr, "wms: sandbox %s: unmounted a stale %s\n", p->id, path);
                unsigned long fl = MS_NOSUID | MS_NODEV;
                if (s->flags & SBX_F_NOEXEC) fl |= MS_NOEXEC;
                if (mount("tmpfs", path, "tmpfs", fl, s->opts) != 0) {
                    snprintf(err, errn, "mount -t tmpfs -o %s %s: %s",
                             s->opts, path, strerror(errno));
                    return -1;
                }
            }
            struct stat st;
            if (stat(path, &st) != 0) {
                snprintf(err, errn, "stat %s: %s", path, strerror(errno));
                return -1;
            }
            if (st.st_uid != (uid_t)p->uid || (st.st_mode & 0777) != 0700) {
                snprintf(err, errn, "%s is owned by %d:%d mode %04o; wanted %d:%d mode 0700",
                         path, (int)st.st_uid, (int)st.st_gid, (unsigned)(st.st_mode & 0777),
                         p->uid, p->uid);
                return -1;
            }
            break;
        }
        case SBX_CHOWN:
            if (sbx_adopt_appdata(s->dst, p->uid, p->gid, err, errn) != 0) return -1;
            break;
        default:
            snprintf(err, errn, "sandbox step %d is not a parent step", i + 1);
            return -1;
        }
    }
    return 0;
}

/* Child side: one step of the plan, inside the app's own namespaces. */
static int sbx_step_child(const SbxPlan *p, const SbxStep *s, char *err, size_t errn) {
    char tgt[SBX_PATH * 2];
    struct stat st;
    switch (s->op) {
    case SBX_BIND: {
        int want_dir = stat(s->src, &st) == 0 && S_ISDIR(st.st_mode);
        if (s->absolute) {
            /* chroot-less mode: the destination is a real path, created here
             * rather than inside the jail tree */
            if (mkdir(s->dst, (mode_t)(s->mode ? s->mode : 0755)) != 0 &&
                errno != EEXIST) {
                snprintf(err, errn, "mkdir %s: %s", s->dst, strerror(errno));
                return -1;
            }
            snprintf(tgt, sizeof(tgt), "%s", s->dst);
        } else if (sbx_walk(p->jail, s->dst, want_dir, s->mode, tgt, sizeof(tgt), err, errn) != 0) {
            return -1;
        }
        if (mount(s->src, tgt, NULL, MS_BIND, NULL) != 0) {
            snprintf(err, errn, "mount --bind %s on %s: %s", s->src, tgt, strerror(errno));
            return -1;
        }
        if (s->ro &&
            mount(NULL, tgt, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) {
            /* second pass: MS_RDONLY on the initial bind is ignored */
            snprintf(err, errn, "remount,ro %s: %s", tgt, strerror(errno));
            return -1;
        }
        return 0;
    }
    case SBX_PROC:
        if (sbx_walk(p->jail, s->dst, 1, 0755, tgt, sizeof(tgt), err, errn) != 0) return -1;
        if (mount("proc", tgt, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
            snprintf(err, errn, "mount -t proc %s: %s", tgt, strerror(errno));
            return -1;
        }
        return 0;
    case SBX_PROC_RO:
        if (sbx_walk(p->jail, s->dst, 1, 0755, tgt, sizeof(tgt), err, errn) != 0) return -1;
        if (mount("/proc", tgt, NULL, MS_BIND, NULL) != 0) {
            snprintf(err, errn, "mount --bind /proc %s: %s", tgt, strerror(errno));
            return -1;
        }
        /* A read-only flag on the initial bind is IGNORED by Linux; read-only
         * requires a second, remount pass. Without this an app with procs=yes
         * that was promised a read-only /proc actually got a writable one. */
        if (mount(NULL, tgt, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) {
            snprintf(err, errn, "remount,ro /proc %s: %s", tgt, strerror(errno));
            return -1;
        }
        return 0;
    case SBX_CHROOT:
        if (chroot(p->jail) != 0) {
            snprintf(err, errn, "chroot %s: %s", p->jail, strerror(errno));
            return -1;
        }
        if (chdir("/") != 0) {
            snprintf(err, errn, "chdir /: %s", strerror(errno));
            return -1;
        }
        return 0;
    case SBX_DROP:
        if (setgroups(0, NULL) != 0) {
            snprintf(err, errn, "setgroups(0): %s", strerror(errno));
            return -1;
        }
        if (setgid((gid_t)p->gid) != 0 || setuid((uid_t)p->uid) != 0) {
            snprintf(err, errn, "setgid/setuid %d:%d: %s", p->uid, p->gid, strerror(errno));
            return -1;
        }
        return 0;
    case SBX_VERIFY:
        /* The checks that would have caught the bugs this file has already
         * had: an app that cannot write its own /root (so it silently saves
         * nothing) and a jail root that the app can write (so the "sandbox"
         * is a directory). Both fail the launch instead of the user. */
        if (getuid() != (uid_t)p->uid || getgid() != (gid_t)p->gid) {
            snprintf(err, errn, "credential is %d:%d, wanted %d:%d",
                     (int)getuid(), (int)getgid(), p->uid, p->gid);
            return -1;
        }
        /* "The root is not writable" is the check that separates a chroot from
         * a directory, and it is the one that must fail closed. With chroot = no
         * the root is the real filesystem on purpose, so the check would refuse
         * every launch; what still holds is that the app runs as its own uid and
         * cannot write the system tree, which is asserted below instead. */
        if (!p->no_chroot) {
            if (access("/", W_OK) == 0) {
                snprintf(err, errn, "the jail root is writable by uid %d", p->uid);
                return -1;
            }
            if (access("/bin", W_OK) == 0) {
                snprintf(err, errn, "/bin is writable by uid %d", p->uid);
                return -1;
            }
        } else if (access("/etc/shadow", R_OK) == 0) {
            snprintf(err, errn, "uid %d can read /etc/shadow", p->uid);
            return -1;
        }
        if (access(p->no_chroot ? p->tmpdir : "/tmp", W_OK) != 0) {
            snprintf(err, errn, "the app cannot write /tmp: %s", strerror(errno));
            return -1;
        }
        /* In a chroot the payload's data directory is /root; without one it is
         * the app's own directory, which HOME points at. Checking the wrong one
         * would pass an app that cannot save anything. */
        if (access(p->no_chroot ? p->appdata : "/root", W_OK) != 0) {
            snprintf(err, errn, "the app cannot write its own data directory: %s",
                     strerror(errno));
            return -1;
        }
        return 0;
    }
    snprintf(err, errn, "unknown sandbox step %d", (int)s->op);
    return -1;
}

/* One line from the child, before it becomes the app: what failed, on what
 * path, and what the kernel said. The write end is close-on-exec, so the parent
 * treats "the pipe closed without a word" as proof that exec worked. */
#define LAUNCH_MSG_MAX 256

static void child_report(int fd, const char *what, const char *path) {
    if (fd < 0) return;
    int err = errno;                  /* before anything else can clobber it */
    char buf[LAUNCH_MSG_MAX];
    int n = snprintf(buf, sizeof(buf), "%s %.160s: %s", what, path ? path : "",
                     strerror(err));
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
}

/* The keeper's job: stay outside the app's pid namespace, hold the pid that
 * wms_launch() returned, forward the four signals a desktop sends, and reap the
 * app. It never touches the app's mounts and never runs app code. */
static volatile pid_t sbx_keeper_app;

static void sbx_keeper_signal(int sig) {
    if (sbx_keeper_app > 0) kill(sbx_keeper_app, sig);
}

static void sbx_keeper_wait(pid_t app, int errfd) {
    close(errfd);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sbx_keeper_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sbx_keeper_app = app;
    int st = 0;
    while (waitpid(app, &st, 0) < 0 && errno == EINTR) { }
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 0);
}

static void sandbox_child(const SbxPlan *plan, const char *exe,
                          const char *arg, int errfd) {
    char err[SBX_PATH * 2];

    /* Nothing above stderr belongs to the app. Without this it inherits our
     * root-opened /dev/fb0 (read AND write the whole screen, forge a lock
     * screen) and the evdev keyboard (the unlock PIN in cleartext), plus the
     * listener and every other client's socket and buffer. Namespaces hide
     * paths; they do not take away file descriptors. errfd is the one
     * exception: it is how this child tells the desktop why it never became an
     * app. */
    for (int fd = 3; fd < 1024; fd++) if (fd != errfd) close(fd);

    /* CLONE_NEWPID is the one flag that cannot be unshared by the process that
     * wants it: unshare() gives the new pid namespace to CHILDREN, so the app
     * is a fork of this one and this one becomes its keeper. Every other
     * namespace is unshared by the app itself, which keeps the keeper in the
     * desktop's own mount and network namespaces. */
    if (plan->ns & SBX_NS_PID) {
        if (unshare(CLONE_NEWPID) != 0) {
            child_report(errfd, "cannot create a pid namespace for", plan->id);
            _exit(125);
        }
        pid_t app = fork();
        if (app < 0) {
            child_report(errfd, "cannot fork the jailed app", plan->id);
            _exit(122);
        }
        if (app > 0) sbx_keeper_wait(app, errfd);   /* never returns */
    }
    unsigned rest = plan->ns & ~(unsigned)SBX_NS_PID;
    if (rest && unshare(sbx_clone_flags(rest)) != 0) {
        child_report(errfd, "cannot unshare namespaces for", plan->id);
        fprintf(stderr, "wms: unshare failed (%s); refusing to run %s\n",
                strerror(errno), plan->id);
        _exit(125);
    }
    /* Changes to the mount tree from here on stay in this namespace. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        fprintf(stderr, "wms: sandbox %s: mount --make-rprivate /: %s\n",
                plan->id, strerror(errno));

    for (int i = 0; i < plan->nstep; i++) {
        const SbxStep *s = &plan->step[i];
        if (s->where != SBX_CHILD) continue;
        if (sbx_step_child(plan, s, err, sizeof(err)) != 0) {
            int n = snprintf(NULL, 0, "%s", err);
            if (errfd >= 0 && n > 0) {
                ssize_t w = write(errfd, err, (size_t)n < LAUNCH_MSG_MAX
                                        ? (size_t)n : LAUNCH_MSG_MAX);
                (void)w;
            }
            fprintf(stderr, "wms: sandbox %s: %s\n", plan->id, err);
            _exit(123);
        }
    }

    setenv("MYSTIC_SOCKET", "/run/mystic/wm.sock", 1);
    setenv("MYSTIC_APP_ID", plan->id, 1);
    {
        /* the text scale is a desktop setting; a client app cannot read
         * /etc/mystic from inside its jail, so it travels in the env */
        char sc[32];
        snprintf(sc, sizeof(sc), "%.3f", (double)ui_scale_get());
        setenv("MYSTIC_UI_SCALE", sc, 1);
    }
    setenv("HOME", plan->no_chroot ? plan->tmpdir : "/tmp", 1);
    setenv("TMPDIR", plan->no_chroot ? plan->tmpdir : "/tmp", 1);
    /* the bundle's own tree, if the app needs more than its payload */
    setenv("MYSTIC_BUNDLE", "/app", 1);
    setenv("LD_LIBRARY_PATH", "/app/usr/lib:/app/lib", 1);
    /* libVLC's module bank searches <dir of the vlc libs>/vlc/plugins (which
     * inside the jail would be /app/libexec/vlc/plugins, because config
     * GetLibDir() follows the program, not the dlopen()ed library) and then
     * every directory in VLC_PLUGIN_PATH. The bundle keeps its plugins in
     * lib/vlc/plugins, so without this line the player would find libvlc and
     * then discover no demuxers, no codecs and no video output at all - a
     * black window with "no suitable interface module". */
    setenv("VLC_PLUGIN_PATH", "/app/lib/vlc/plugins", 1);
    /* VLC and every other library that keeps state writes under these; /tmp
     * is the jail's own tmpfs, owned by this app (mode 0700, size in the plan). */
    setenv("XDG_CACHE_HOME", "/tmp/.cache", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/.config", 1);
    (void)exe;
    (void)arg;
}

static int wms_launch_arg(const WmApp *app, WmClient *cl, int slot, const char *arg) {
    if (!app || slot < 0) return -1;

    /* The payload comes from the root the manifest was found under, never from
     * a hardcoded one: a pkg-installed bundle lives in /root/apps and exec'ing
     * <WM_APP_DIR>/<id> for it hit a path that does not exist, which is why
     * every installed tile was clickable and dead. The path is also proven to
     * be a regular executable before a fork is spent on it, so the failure is
     * one honest line instead of a pid that never opens a window. */
    char exe[512], why[128];
    if (wms_bundle_payload(app, exe, sizeof(exe), why, sizeof(why)) != 0) {
        if (exe[0]) launch_failed("%s: %s (%s)", app->id[0] ? app->id : "?", why, exe);
        else        launch_failed("%s: %s", app->id[0] ? app->id : "?", why);
        return -1;
    }

    /* The credential is reserved before the fork and before any mount, because
     * the tmpfs has to be owned by it and the process has to be running as it
     * by the time it connects. `reuse` says another window of this bundle is
     * already up: then the tmpfs it mounted is kept. */
    int reuse = 0;
    int uid = sbx_cred_acquire(app->id, &reuse);
    if (uid >= 0 && sbx_uid_valid(uid)) sbx_creds[uid - SBX_UID_BASE].host = app->host_ns;
    if (uid < 0) {
        launch_failed("%s: no sandbox uid free (%d apps are already running)",
                      app->id, SBX_UID_SLOTS);
        return -1;
    }

    char appdata[512];
    snprintf(appdata, sizeof(appdata), "/root/appdata/%s", app->id);

    if (app->host_ns) {
        /* Mount the bundle at /app once: the engine's PT_INTERP is
         * /app/ld-musl-x86_64.so.1, which is what makes it runnable outside a
         * chroot without rewriting the package per launch. */
        char bundle[512];
        snprintf(bundle, sizeof(bundle), "%s", exe);
        char *slash = strrchr(bundle, '/');
        if (slash) *slash = 0;
        mkdir("/app", 0755);
        if (mount(bundle, "/app", NULL, MS_BIND, NULL) != 0 && errno != EBUSY)
            fprintf(stderr, "wms: %s: bind %s at /app: %s (continuing)\n",
                    app->id, bundle, strerror(errno));

        pid_t hp = fork();
        if (hp < 0) {
            launch_failed("%s: fork: %s", app->id, strerror(errno));
            return -1;
        }
        if (hp == 0) {
            setgid((gid_t)uid);
            setuid((uid_t)uid);
            setenv("MYSTIC_SOCKET", WM_SOCKET, 1);
            setenv("MYSTIC_APP_ID", app->id, 1);
            setenv("MYSTIC_BUNDLE", "/app", 1);
            setenv("LD_LIBRARY_PATH", "/app/usr/lib:/app/lib", 1);
            setenv("HOME", "/tmp", 1);
            setenv("TMPDIR", "/tmp", 1);
            if (arg && arg[0]) execl(exe, "app", arg, (char *)NULL);
            else               execl(exe, "app", (char *)NULL);
            fprintf(stderr, "wms: %s: exec %s: %s\n", app->id, exe, strerror(errno));
            _exit(127);
        }
        /* Anchor the pid exactly as the jailed path does. Without this the uid is
         * reserved but no process is recorded against it, so the next acquire - or
         * any client disconnect, which runs sbx_cred_gc - frees a uid a live process
         * is still using and hands it to another bundle: the shared-uid hole the
         * pool exists to close. */
        sbx_cred_add_anchor(uid, (long)hp);
        fprintf(stderr, "wms: launched %s pid %d (host namespaces, uid %d)\n",
                app->id, (int)hp, uid);
        return (int)hp;
    }

    SbxPlan plan;
    sbx_plan_init(&plan, run_dir(), app->id, exe, appdata, uid,
                  app->procs, app->net, app->audio, app->no_chroot,
                  screen_w, screen_h, arg);
    plan.reuse = reuse;
    if (getenv("MYSTIC_SANDBOX_PRINT") && getenv("MYSTIC_SANDBOX_PRINT")[0])
        sbx_plan_print(&plan, stderr);

    char err[SBX_PATH * 2];
    if (sandbox_prepare(&plan, err, sizeof(err)) != 0) {
        launch_failed("%s: %s", app->id, err);
        sbx_cred_gc();               /* the reservation has no process yet */
        return -1;
    }

    int ep[2];
    if (pipe(ep) != 0) {
        launch_failed("%s: pipe: %s", app->id, strerror(errno));
        sbx_cred_gc();
        return -1;
    }
    /* The write end dies with the exec, so the parent can tell "running" from
     * "never ran" without polling /proc or waiting for a window. */
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);
    fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        launch_failed("%s: fork: %s", app->id, strerror(errno));
        close(ep[0]);
        close(ep[1]);
        sbx_cred_gc();
        return -1;
    }
    if (pid == 0) {
        close(ep[0]);
        sandbox_child(&plan, exe, arg, ep[1]);
        if (arg && arg[0]) {
            /* an app that was asked to open a document gets the path as
             * argv[1]; the path was validated by the desktop, and the app
             * still cannot see anything outside its own jail */
            execl(app->no_chroot ? exe : "/bin/app", "app", arg, (char *)NULL);
        } else {
            execl(app->no_chroot ? exe : "/bin/app", "app", (char *)NULL);
        }
        /* exec failed: /bin/app is not an executable (a 0-byte placeholder, a
         * library, a wrong architecture). The kernel says which. */
        child_report(ep[1], "cannot exec", exe);
        fprintf(stderr, "wms: cannot exec the jailed %s: %s\n", app->id, strerror(errno));
        _exit(127);
    }

    /* The keeper (or the app itself when it has no pid namespace). Either way
     * this pid lives as long as the app, so the uid cannot be handed to
     * another bundle while it is still in use. */
    sbx_cred_add_anchor(uid, (long)pid);

    close(ep[1]);
    /* Wait for one of two answers: the pipe closes (exec happened, the app is
     * running) or it carries a sentence (it did not). Bounded, because a mount
     * that hangs must not freeze the desktop's frame loop. */
    char msg[LAUNCH_MSG_MAX + 1];
    int got = 0, timed_out = 0;
    for (int i = 0; i < 2000 && got < (int)sizeof(msg) - 1; i++) {
        ssize_t r = read(ep[0], msg + got, sizeof(msg) - 1 - (size_t)got);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;                       /* closed: nothing went wrong */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (i == 1999) { timed_out = 1; break; }
        struct timespec ts = { 0, 1000000 };     /* 1 ms */
        nanosleep(&ts, NULL);
    }
    close(ep[0]);
    msg[got] = 0;

    if (got > 0) {
        for (int i = 0; i < got; i++) if (msg[i] == 10 || msg[i] == 13) msg[i] = ' ';
        launch_failed("%s: %s", app->id, msg);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        sbx_cred_gc();
        return -1;
    }
    if (timed_out)
        fprintf(stderr, "wms: %s has not exec'd after 2 s; leaving it alone\n", app->id);
    (void)cl;
    return (int)pid;
}

/* Two entry points, one implementation: the launcher and the store use the
 * plain one; Files passes a document path so the app can open it. */
int wms_launch(const WmApp *app, WmClient *c, int slot) {
    return wms_launch_arg(app, c, slot, NULL);
}

int wms_launch_path(const WmApp *app, WmClient *c, int slot, const char *path) {
    return wms_launch_arg(app, c, slot, path);
}

#endif /* !MYSTIC_SANDBOX_PLAN */