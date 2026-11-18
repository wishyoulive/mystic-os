# Mystic OS — the app sandbox

Every app launched from a launcher tile is a separate process, in its own
namespaces, chrooted into a jail, running as its own unprivileged uid. The
kernel enforces that; this document says exactly what it is, what it costs, and
what it does **not** protect against. It matches `src/wmsrv.c` (the sandbox
plan), `src/ipc.h` (the fields that hold the credential) and `stage/init`
(what a boot has to provide).

The short version:

| an app can… | because |
|---|---|
| read its own bundle (`/app`, read-only) and its own appdata (`/root`) | two bind mounts, nothing else from the host is visible |
| write `/tmp` and `/dev/shm`, both size-limited tmpfs owned by it | mounted per app, before the fork |
| talk to the window server over the one socket bound into its jail | `MYSTIC_SOCKET`, mode 0666 |
| read fonts, `/dev/null`, `/dev/urandom`, a procfs | bound read-only (or as the one writable device pair) |
| **not** see `/root`, `/home`, the disk, `/etc`, the framebuffer, the keyboard, other apps' pixels or another app's `/dev/shm` | mount namespace + chroot + DAC + a different uid per app |
| **not** signal, ptrace or kill the desktop or another app | private pid namespace (unless `procs = yes`), and a different uid |

---

## 1. Credentials — one uid per app, from a pool

`SBX_UID_BASE` `2000`, `SBX_UID_SLOTS` = `WM_MAX_CLIENTS` (8) → **uids
2000..2007, gids 2000..2007**, `setgroups(0, NULL)` so supplementary groups are
empty. The slot index is the pool index: `uid = 2000 + slot`.

*Allocated* by `wms_launch_arg()` → `sbx_cred_acquire()`, before the fork and
before any mount: the tmpfs has to be owned by that uid and the process has to
be running as it by the time it connects.

*Bound to the connection* by the kernel, not by the wire. `wmsrv.c` asks
`SO_PEERCRED` on the accepted socket, which returns the sender's uid and pid as
the desktop's pid namespace sees them; an app cannot forge it. That is also how
the buffer is made: `make_shm()` chowns the file to the uid the *kernel*
reports, and a jailed client's buffer is created inside that uid's tmpfs. A
`HELLO` that names another app is logged and ignored — the bundle id used for
paths is the one recorded at launch.

*Released* when the bundle has no process left (checked whenever the pool is
swept: on a launch or a disconnect). Two facts are tracked per
entry: the launch's pid (the keeper below) and the pid of each connected
client. `sbx_cred_gc()` drops the ones the kernel says are gone and frees the
slot — and with it, unmounts the two tmpfs instances. A bundle that dies before
ever connecting is collected by the same sweep on the next launch, so a broken
app cannot eat the pool.

### Why one uid per *bundle* and not per window

The audit asked for one uid per client. One uid per **bundle** is what shipped,
and the reason is the app's own `/root`:

* an appdata directory has exactly one owner, and Paint's pictures, the 2048
  best score and the browser's session have to survive a restart. If two
  windows of one program got two uids, the second window's `chown` would lock
  the first one out of its own files.
* two windows of one program are one trust domain. The boundary that matters —
  and the one the audit proved was missing — is between *different* apps: with
  a single `SANDBOX_UID 1000` for everything, any app could open, forge and
  `ftruncate` any other app's pixel buffer and SIGBUS the compositor. That is
  gone: two different bundles never share a uid.
* a second window of the same bundle keeps the uid **and** the tmpfs the first
  one mounted (`plan.reuse`), so the file-picker bridge in the first window
  keeps working.

When a uid changes (after a reboot slots are handed out in launch order, so
`paint` may be 2002 today and 2000 tomorrow) the appdata tree is re-owned once,
recursively, with `nftw(..., FTW_PHYS)` + `lchown` — symlinks inside the app's
own tree are **not** followed, so a link the app planted can never make root
`chown` a host file. The fast path is a single `stat()`.

---

## 2. The filesystem an app sees

Built by `sandbox_prepare()` (the desktop, before the fork) and
`sandbox_child()` (the app, after `unshare`), from the step list that
`sbx_plan_init()` produces. To see it:

```sh
MYSTIC_SANDBOX_PRINT=1 /usr/bin/mystic-desktop     # prints every plan it builds
```

```
/                    root:root 0755, not writable by the app
/bin/app             the payload, bind, read-only
/app                 its own bundle, bind, read-only (libs, plugins, data)
/dev/null            devtmpfs node, bind
/dev/urandom         bind, read-only
/dev/snd             bind, only when the manifest says audio = yes
/dev/shm             tmpfs, size=2x buffer + 1 MiB, mode 0700, owner = the app
/run/mystic/wm.sock  the window socket, bind
/root                the app's own appdata, bind, owned by the app
/tmp                 tmpfs, size=2x buffer + 1 MiB, mode 0700, owner = the app
/usr/share/fonts     bind, read-only
/proc                a fresh procfs for the app's own pid namespace,
                     or the host /proc read-only when procs = yes
```

Nothing else exists. There is no path to the real `/root`, to `/etc`, to the
framebuffer or to any other app's jail.

### The two tmpfs mounts, and the arithmetic

The largest buffer an app can be given is one screen: `HELLO` is clamped to
`screen_w x screen_h` and the buffer is exactly that, `stride * maxh * 4`
bytes. Both per-app tmpfs mounts are

```
size = 2 * (screen_w * screen_h * 4) + 1 MiB
```

At 1280x800: `2 * 4,096,000 + 1,048,576 = 9,240,576 B (8.81 MiB)`.

*Why 2x + 1 MiB:* one buffer is the one the server maps; the second is enough
room for an app to keep a second full-screen buffer of its own in shared
memory, and the megabyte covers everything else an app puts there (a player's
cache, a lock file, a score). That is the point: an app can fill its own
`/dev/shm` and get `ENOSPC`; it cannot fill the machine, and it cannot touch
another app's buffer because that buffer is in a different tmpfs instance that
its mount namespace never sees.

`/tmp` uses the same formula so there is one number to reason about. It is
`nosuid,nodev` but **not** `noexec` (a runtime may unpack and exec a helper);
`/dev/shm` is `nosuid,nodev,noexec` (nothing there is ever executable).

The host `/dev/shm` (`stage/init`) is capped at 128 MiB: eight clients x one
1920x1080 buffer (`1920*1080*4 = 8,294,400 B`) = 63.3 MiB worst case.

### Why the desktop mounts `/tmp` and `/dev/shm`, not the app

They have to be the *same instances* on both sides of the jail:

* the file picker's bridge is a file: a sandboxed app writes
  `/tmp/filepick.request` and reads `/tmp/filepick.result`, and the desktop
  reads and writes the same files as `<run>/jail/<id>/tmp/...` (`main.c`,
  `filepick_service`). A tmpfs mounted inside the child's mount namespace is
  invisible to the desktop, and the dialog would never appear.
* the pixel buffer is created by the server at `HELLO`, after the app has
  already started. It lands in `<run>/jail/<id>/dev/shm/<leaf>`, which is the
  app's `/dev/shm/<leaf>` only because the tmpfs was mounted before the fork.

Both are unmounted when the bundle's last process is gone
(`sbx_jail_forget`, from the pool sweep), and a leftover from a crash is
replaced on the next launch (`umount2(MNT_DETACH)` first).

### The jail root is not writable by the app

Every directory in the skeleton is created `root:root 0755`; the only writable
places are the appdata bind (`/root`), the two tmpfs mounts, and the device
nodes bound over `/dev` (`/dev/null`, `/dev/urandom`, `/dev/snd`). That is a DAC
fact, and it is **checked at launch** rather than assumed: after the credential
drop, the app itself runs

```
access("/", W_OK) and access("/bin", W_OK)  must fail
access("/tmp", W_OK) and access("/root", W_OK) must succeed
```

and refuses to start if either is wrong. Those four syscalls would have caught
two of the bugs this file has already had: an app that could not write its own
`/root` (so it silently saved nothing) and a jail whose root was writable.

Deliberately **not** done: a read-only remount of `/` in the jail. It would not
add anything the root-owned skeleton and the check above do not already give,
and it would cover `<run>/jail/<id>/run`, where the clipboard bridge's socket
lives — a read-only mount in front of a socket the app must `connect()` to is a
risk with no upside on this kernel. If the skeleton ever gains a world-writable
directory, that decision has to be revisited.

### Symlinks are refused as bind targets

`mount(2)` follows a symlink in the target path. The app owns one directory
that the jail mounts into — its appdata, at `/root` — and it can plant symlinks
there. Without a check, the next launch's document bind (`open /root/x` from
the file picker) would create/`mount` over the *symlink's* target as root.
`sbx_walk()` therefore takes the target apart one component at a time below the
jail root: it `lstat()`s each, refuses `S_ISLNK` anywhere (and refuses a
non-directory in the middle of the path), and only then creates what is
missing. `sbx_target_verdict()` is that decision, isolated so the host harness
can prove it (see §7).

---

## 3. Processes — `CLONE_NEWPID`, and the one exception

By default `unshare()` is called with `CLONE_NEWNS | CLONE_NEWIPC |
CLONE_NEWUTS | (CLONE_NEWNET unless network = host) | CLONE_NEWPID`, and the
jail mounts a **fresh procfs**. The app is pid 1 of its namespace: it sees
itself (and its own children) and nothing else, and a `kill()` from inside can
never reach the desktop or another app, because they are not in its namespace.

`CLONE_NEWPID` is the one flag a process cannot give itself: `unshare()` gives
the new namespace to *children*. So the forked child unshares the pid namespace,
forks once more, and becomes the app's **keeper**: it stays in the desktop's
namespaces (no app code, no app mounts), holds the pid `wms_launch()` returns,
forwards SIGTERM/SIGINT/SIGHUP/SIGQUIT, and reaps the app. That pid is what the
uid pool tracks, so an app's uid cannot be handed to another bundle while it is
still alive. The app unshares every other namespace itself.

### `procs = yes` — the named trade

`sysmon` reads every process on the machine; a private pid namespace would show
it exactly one process, itself. A manifest may therefore say

```
procs = yes
```

which skips `CLONE_NEWPID` and binds the **host `/proc` read-only** into the
jail instead. What that costs, precisely:

* the app can read every process's name and command line
  (`/proc/<pid>/stat`, `/proc/<pid>/cmdline`, `/proc/<pid>/comm`) and
  system-wide files (`/proc/meminfo`, `/proc/stat`, `/proc/net/dev`);
* it still cannot read another uid's `/proc/<pid>/environ`, `mem`, `fd` or
  `maps` — those are root/owner-only and it is neither;
* it still cannot signal another app (different uid), and it still has its own
  mount namespace, its own tmpfs, no `/root`, and no network.

Only sysmon's manifest sets it. Every other bundle gets the private namespace,
and a bundle that asks for `procs = yes` without needing it is simply giving up
a layer it did not have to.

---

## 4. Pixel buffers

The server creates one file per client and maps it; the app opens the path the
`OK` line gave it. Both must be the same inode, so the path is the app's view:

* jailed client: `<run>/jail/<id>/dev/shm/<leaf>` on the host, `/dev/shm/<leaf>`
  inside the jail — the same file, because the tmpfs is shared;
* a client the desktop did not launch (an app started from a shell, a
  developer's test client): `/dev/shm/<leaf>`, and it is not chrooted.

The file is `0600` and owned by the connection's real uid, inside that uid's
`0700` tmpfs. Another app cannot open it — it has no path to it and, even with
one, no permission. That closes the exact hole the audit proved: read, forge
and `ftruncate` of *another* app's buffer.

---

## 5. What the desktop owes the sandbox

`stage/init` provides, and `wms_start()` re-checks:

* `/dev/shm` on tmpfs (128 MiB, `1777`, `nosuid,nodev`) — the server's buffers;
* `/run/mystic` and `/run/mystic/jail`, `root:root 0755`;
* devtmpfs, so `/dev/null`, `/dev/urandom` and `/dev/snd` exist to be bound;
* procfs on `/proc`, so a `procs = yes` app has the host view to bind.

The uids 2000..2007 need no `/etc/passwd` entry: `setuid(2)` takes a number, and
the jail has no `/etc/passwd` to read.

The desktop itself keeps root. The sandbox protects the user's files and the
desktop from an app, not the app from a compromised kernel.

---

## 6. Resident risks (read this before claiming the sandbox is complete)

1. **An app owns the buffer it draws into**, so it can `ftruncate()` its own
   file and the desktop's read of that mapping can `SIGBUS`. Before this work
   any app could do that to *any* app's buffer; now only to its own. The
   complete fix is a sealed memfd (`F_SEAL_SHRINK`) bind-mounted into the jail
   instead of a regular file; it is not in this tree because it could not be
   exercised on the target kernel, and a wrong guess there would leave every
   app with no buffer at all. Until it lands, a hostile app can still take the
   desktop down — with its own data only.
2. **`procs = yes` sees the host process list.** That is what §3 says it buys
   and what sysmon is for. It cannot read another uid's process *contents*.
3. **No seccomp filter.** The isolation is namespaces + DAC + chroot. An app can
   use every syscall its uid is permitted, including `clone`, `mount` (fails:
   no `CAP_SYS_ADMIN`), `ptrace` of itself, and — with `network = host` — real
   sockets.
4. **`audio = yes` binds `/dev/snd`.** That is a recording device as much as a
   playback one. Only bundles whose manifest asks for it get it.
5. **The window socket is mode 0666.** Any local process can connect and open a
   window; it cannot read another app's buffer (0600) and cannot claim another
   app's identity (`SO_PEERCRED`). This is a single-user machine whose local
   processes are all ours.
6. **No user namespace.** Apps run as real host uids, not as uid 0 in a mapped
   namespace. That is deliberate (it keeps `/root` readable by root only), but
   it means a kernel bug in a namespace is a direct escape rather than one that
   has to be chained.
7. **`/dev/shm` inside the jail does not survive the app.** `g2048` kept its
   best score in `/dev/shm/g2048.best`; that file is now in a per-app tmpfs and
   is gone when the app exits. Persistent state belongs in `/root` (the
   appdata bind), and that app should be moved there. The same is true of
   anything else that treated `/dev/shm` as storage.
8. **A desktop that dies without cleaning up leaves that bundle's tmpfs
   mounted** until the bundle is launched again or the machine reboots
   (mounts live in the desktop's namespace). It is bounded - two per bundle,
   at most 9 MiB each at 1280x800 - and the next launch detaches it.
9. **Two bridges deliberately cross the boundary**: the file picker (files in
   `<jail>/tmp`) and the clipboard (`<jail>/run/mystic/clipboard.sock`, served
   by the desktop). Both are one-way or text-only, and both are the reason the
   app can be useful at all. They are the first place to look if this sandbox
   is ever audited again.

---

## 7. Proving it

The plan is data, and the data is in the shipped file, so the parts that do not
need a kernel can be tested on the build machine:

```
$ zig cc -target x86_64-linux-musl -Os -static -Isrc \
      -Wall -Wextra -Wno-unused-parameter -c src/wmsrv.c -o .work/sandbox/x.o
        (silent)

$ zig cc -Wall -Wextra -Isrc .work/sandbox/plan_harness.c -o .work/sandbox/plan.exe
$ ./.work/sandbox/plan.exe
        ... 44 checks, 0 failure(s)   (exit 0)
```

The harness `#define MYSTIC_SANDBOX_PLAN` and `#include`s the **real**
`src/wmsrv.c`, so it runs the real `sbx_cred_acquire()`,
`sbx_cred_del_pid()`, `sbx_plan_init()`, `sbx_tmpfs_size()` and
`sbx_target_verdict()`. It proves:

* two different apps get two different uids (2000, 2001), a second window of
  the same app keeps its uid, a uid with no live process is released, and the
  ninth bundle is refused rather than sharing a uid;
* `/dev/shm` is `2 * (1280*800*4) + 1 MiB = 9,240,576 B`, and `/tmp` the same,
  both `mode=0700,uid=<the app>`, both mounted **before** the fork, `/dev/shm`
  `noexec` and both `nosuid,nodev`;
* `CLONE_NEWPID` is in the plan for an ordinary app and absent for
  `procs = yes`, with a fresh procfs in the first case and a read-only host
  `/proc` in the second; `network = host` removes `CLONE_NEWNET`;
* a symlink bind target is refused, for a file and for a directory, while a
  real directory, a real file and a missing path are handled correctly.

What it does **not** prove, because a Windows build machine has no `mount(2)`:
the syscalls themselves. Those are covered by the compile, by the executor
being a single `switch` over the same step list the printer prints, and by the
launch-time `access()` checks in §2, which fail the launch if the result is not
what the plan said.

The other half of the proof is on the machine: every launch prints one line

```
wms: sandbox: uids 2000..2007, one per bundle (gid == uid, no supplementary groups), private /dev/shm and /tmp per app
wms: paint connected 520x360 as uid 2000 (/run/mystic/jail/paint/dev/shm/mystic-312-1)
wms: paint (uid 2000) disconnected; uid released
wms: sandbox: unmounted /run/mystic/jail/paint/dev/shm
```

and `MYSTIC_SANDBOX_PRINT=1` prints the full plan for each app as it starts.
