# Mystic OS — the app sandbox

Every app launched from a launcher tile is a separate process, in its own
namespaces, chrooted into a jail, running as its own unprivileged uid. The
kernel enforces that; this document says exactly what it is, what it costs, and
what it does **not** protect against. It matches `src/wmsrv.c` (the sandbox
plan), `src/ipc.h` (the fields that hold the credential) and `stage/init`
(what a boot has to provide). Line numbers are from the revision this was
written against; the symbol names are what to grep when they move.

The short version:

| an app can… | because |
|---|---|
| read its own bundle (`/app`, read-only) and its own appdata (`/root`) | two bind mounts; the rest of the host is not in the jail |
| read `/etc` read-only, including `/etc/mystic` | one read-only bind in every jail — what that costs is in §2.3 |
| write `/tmp` and `/dev/shm`, 64 MiB each and never less, owned by it | one per-app tmpfs each, `SBX_SHM_FLOOR`, before the fork |
| talk to the window server over the one socket bound into its jail | `MYSTIC_SOCKET`, mode 0666 |
| read fonts, `/dev/null`, `/dev/zero`, `/dev/urandom`, a procfs | bound read-only, or as writable device nodes |
| **not** see `/root`, `/home`, the disk, the framebuffer, the keyboard, other apps' pixels or another app's `/dev/shm` | mount namespace + chroot + DAC + a different uid per bundle |
| **not** signal, ptrace or kill the desktop or another app | private pid namespace (unless `procs = yes`), and a different uid |

---

## 1. Credentials — one uid per app, from a pool

`SBX_UID_BASE` `2000` (`src/wmsrv.c:91`), `SBX_UID_SLOTS` = `WM_MAX_CLIENTS`
(8) → **uids 2000..2015, gids 2000..2015**, `setgroups(0, NULL)` so
supplementary groups are empty. The slot index is the pool index:
`uid = 2000 + slot`, and `src/wmsrv.c:1738` is the drop that makes the gid
equal the uid.

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
/dev/zero            devtmpfs node, bind (anonymous memory is backed by it)
/dev/urandom         bind, read-only
/dev/console         bind, only when the manifest says procs = yes
/dev/snd             bind, only when the manifest says audio = yes
/dev/shm             tmpfs, sbx_tmpfs_size(), mode 0700, owner = the app
/etc                 the host /etc, bind, read-only
/run/mystic/wm.sock  the window socket, bind
/root                the app's own appdata, bind, owned by the app
/tmp                 tmpfs, the same size and owner as /dev/shm
/usr/share/fonts     bind, read-only
/proc                a fresh procfs for the app's own pid namespace,
                     or the host /proc read-only when procs = yes
```

Nothing else exists. There is no path to the real `/root`, to `/home`, to the
disk, to the framebuffer or to any other app's jail. `/etc` is the one system
directory handed over whole; §2.3 says what is in it and what that costs.

### 2.1 The two tmpfs mounts, and the arithmetic

The largest buffer a client can be given is one screen: `HELLO` is clamped to
`screen_w x screen_h` and the buffer is exactly `stride * maxh * 4` bytes. Both
per-app tmpfs mounts are sized by one function, `sbx_tmpfs_size()`, and there
is one constant to reason about, `SBX_SHM_FLOOR` (`src/wmsrv.c:287`):

```
need = 2 * (screen_w * screen_h * 4) + 1 MiB
size = need > SBX_SHM_FLOOR ? need : SBX_SHM_FLOOR
       SBX_SHM_FLOOR = 64 * 1024 * 1024 = 67,108,864 B
```

At the default 1280x800 screen the formula gives
`2 * 4,096,000 + 1,048,576 = 9,240,576 B`, which is *below* the floor, so what
is actually mounted is **64 MiB — 67,108,864 B — for `/dev/shm` and again for
`/tmp`**. The formula takes over only once `need` passes 64 MiB, which needs a
buffer larger than `(64 MiB - 1 MiB) / 2 = 33,030,144 B`, i.e. a screen of more
than 8,257,536 pixels. 2560x1440 (3,686,400 px) is still the floor; 3840x2160
is not, and there the mount is `2 * 33,177,600 + 1 MiB = 67,403,776 B`.

*Why a floor:* two frames plus a megabyte is the minimum a GUI client needs,
but the browser engine maps tens of megabytes of shared memory before it draws
anything and died during startup with the formula alone. 64 MiB is the number
that lets it reach its first frame; it is still a cap, not a free-for-all.

*Why 2x + 1 MiB at all:* one buffer is the one the server maps; the second is
enough room for an app to keep a second full-screen buffer of its own in shared
memory, and the megabyte covers everything else an app puts there (a player's
cache, a lock file, a score). That is the point: an app can fill its own
`/dev/shm` and get `ENOSPC`; it cannot fill the machine, and it cannot touch
another app's buffer because that buffer is in a different tmpfs instance that
its mount namespace never sees.

Both mounts are mode `0700`, owned by the app's uid, `nosuid,nodev`; `/dev/shm`
is additionally `noexec` (nothing there is ever executable) and `/tmp` is not
(a runtime may unpack and exec a helper). tmpfs charges pages when they are
written, not when the mount is made, so the floor is a ceiling and not a
reservation: eight bundles may be allowed 128 MiB each across the two mounts
without the launcher handing out a gigabyte at once. §6.11 is the other side of
that.

The host `/dev/shm` (`stage/init`) is capped at 128 MiB: eight clients x one
1920x1080 buffer (`1920*1080*4 = 8,294,400 B`) = 63.3 MiB worst case. With the
per-app tmpfs in place a jailed client never maps there — its buffer is in its
own jail's tmpfs — so that mount now holds only what a *not jailed* client
maps: one the desktop did not launch, or one launched with `host = yes`
(§2.6).

### 2.2 Why the desktop mounts `/tmp` and `/dev/shm`, not the app

They have to be the *same instances* on both sides of the jail:

* the file picker's bridge is a file: a sandboxed app writes
  `/tmp/filepick.request` and reads `/tmp/filepick.result`, and the desktop
  reads and writes the same files as `<run>/jail/<id>/tmp/...` (`src/main.c`'s
  `filepick` state, protocol in `src/filepick.h`). A tmpfs mounted inside the
  child's mount namespace is invisible to the desktop, and the dialog would
  never appear.
* the pixel buffer is created by the server at `HELLO`, after the app has
  already started. It lands in `<run>/jail/<id>/dev/shm/<leaf>`, which is the
  app's `/dev/shm/<leaf>` only because the tmpfs was mounted before the fork.

Both are unmounted when the bundle's last process is gone
(`sbx_jail_forget`, from the pool sweep), and a leftover from a crash is
replaced on the next launch (`umount2(MNT_DETACH)` first).

### 2.3 `/etc` is bound read-only in every jail, and that has a price

`sbx_plan_init()` adds the host `/etc` as a read-only bind for **every** plan
(`src/wmsrv.c:465`), independent of `network = host`. It is there for the
reason the source gives: a toolkit looks up `/etc/passwd`, the fontconfig files
and `/etc/ssl/certs/ca-bundle.crt` before it can draw or fetch anything, and an
engine that cannot find them fails in a way that prints nothing.

The reason it is *not* a credential leak in the classic sense is the shadow
file: `/etc/shadow` is mode `0640 root:root`, so an app running as uid
2000..2015 can open the directory and cannot read that file. The `chroot = no`
verifier asserts exactly that — `access("/etc/shadow", R_OK)` must fail
(`src/wmsrv.c:1771`) — and the shipped tree has no `/etc/shadow` at all.

What an app can read, in the shipped tree (`stage/etc`): `/etc/mystic/` —
`current-theme`, `ui.conf`, `wallpaper`, `themes/`, `repo.conf` and its public
key `repo.pub`, the lock screen's credential as `passwd`
(`salt:iterations:hash`, read by `src/main.c:3727`), and `luks.key`, the
32-byte plaintext boot half of the disk key (`docs/ENCRYPTION.md` §6.1). Also
`/etc/ssl/certs/ca-bundle.crt`, `/etc/mke2fs.conf`, `/etc/mystic-banner` and
`/etc/udhcpc.script`. There is no `/etc/passwd` in the shipped image, so no
user list is exposed either way.

So the honest statement is: `/etc` is configuration, not secrets *by
convention*, and two files the convention does not cover — the lock hash and
the disk key half — are readable by every app. §6.10 is the consequence.

### 2.4 The jail root is not writable by the app

Every directory in the skeleton is created `root:root 0755`; the only writable
places are the appdata bind (`/root`), the two tmpfs mounts, and the device
nodes an app uid can actually open: `/dev/null` and `/dev/zero` (0666, bound
without `ro`), and the playback PCM nodes under `/dev/snd` when `audio = yes`
(`stage/init` chmods `pcmC*D*p` to 0666). `/dev/urandom` is bound read-only,
and `/dev/console` — bound for a `procs = yes` app — keeps the host node's
`0600 root:root`, so it is not one of them (§3). That is a DAC fact, and it is
**checked at launch** rather than assumed: after the credential drop, the app
itself runs

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

### 2.5 Symlinks are refused as bind targets

`mount(2)` follows a symlink in the target path. The app owns one directory
that the jail mounts into — its appdata, at `/root` — and it can plant symlinks
there. Without a check, the next launch's document bind (`open /root/x` from
the file picker) would create/`mount` over the *symlink's* target as root.
`sbx_walk()` therefore takes the target apart one component at a time below the
jail root: it `lstat()`s each, refuses `S_ISLNK` anywhere (and refuses a
non-directory in the middle of the path), and only then creates what is
missing. `sbx_target_verdict()` is that decision, isolated so the host harness
can prove it (see §7).

### 2.6 `chroot = no` and `host = yes` — parsed, unused, unfinished

Two manifest keys widen the jail. Both are read by `sbx_manifest_line()`
(`src/wmsrv.c:637` and `:638`), and **neither is used by a shipped bundle**: of
the twelve manifests under `stage/usr/share/mystic/apps/`, not one contains
`chroot = no` or `host = yes`. What they would do:

* `chroot = no` — the plan keeps the mount, IPC, UTS, net and pid namespaces,
  the uid drop, the private `/tmp` and `/dev/shm` and the bundle at `/app`, and
  skips the `chroot` step. The app then sees the real `/`, `/lib`, `/usr` and
  `/etc` as its own uid — and the host `/proc` that comes with them, not the
  fresh procfs the plan mounts at a jail root the app never enters. The "root
  is not writable" launch check is replaced by "cannot read `/etc/shadow`"
  (`src/wmsrv.c:1762`).
* `host = yes` — not a plan at all: `wms_launch()` forks, drops to the sandbox
  uid, bind-mounts the bundle at `/app` and execs the payload in the desktop's
  own mount, pid, net and IPC namespaces, with no chroot and no tmpfs of its
  own (`src/wmsrv.c:1954`). Its pixel buffer is created in the host `/dev/shm`,
  because `make_shm()` checks the credential's `host` flag
  (`src/wmsrv.c:996`). Only the uid boundary and the DAC that comes with it
  remain.

Both exist for a third-party runtime that will not start inside a cage holding
only its own bundle, and **both are unfinished**: `host = yes` builds no step
list, so `MYSTIC_SANDBOX_PRINT=1` prints nothing to check it against, and no
test exercises that launch path; the `chroot = no` *plan* is covered by the
harness (§7), the `mount(2)` calls it triggers are not; and no shipped bundle
needs either of them. They are off because nothing sets them, not because the
parser refuses them — there is no allowlist, so a package that ships one of
these lines gets it, and a downloaded bundle's manifest is trusted exactly as
far as `pkg` verifies the archive. Treat the pair as a change to be reviewed,
not a setting to flip.

Nor does the bundle they were invented for need them. The browser engine
(`stage/usr/share/mystic/apps/engine/`) ships `exec = bin`, `network = host`
and `procs = yes` — no `chroot`, no `host` — so it is launched exactly like
Paint: the same chroot, the same two 64 MiB tmpfs, the same uid drop, the same
read-only `/app`. Its package is built for that cage: `tools/fetch-engine.py`
rewrites the engine's `PT_INTERP` to `/app/ld-musl-x86_64.so.1` and puts the
rest of the closure under `/app/usr/lib`, and the client execs it from there
with `LD_LIBRARY_PATH=/app/usr/lib:/app/lib`, rendering a page as a screenshot
(`--screenshot`) when the DevTools channel is not up — one frame per
navigation, arriving over the ordinary window socket. That is why the engine is
not an exception to the model: the chroot, the uid, the tmpfs and the
read-only `/app` are Paint's. Two manifest lines do make it wider than Paint —
`network = host`, which `web` and `vlc` share and which the DevTools transport
needs because the client talks to the browser over loopback TCP, and
`procs = yes`, which `sysmon` shares. §3 covers the second; the first is the
same host-network choice three bundles already make.

Nothing else in this document relies on `chroot = no` or `host = yes`.

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
jail instead (`src/wmsrv.c:497`). The plan also adds a `/dev/console` bind for
these bundles, but that one is **inert**: `sbx_walk()` creates the mount point
`0620` (`src/wmsrv.c:482`), a bind mount keeps the *source* inode's attributes,
and devtmpfs makes `/dev/console` `0600 root:root` (`stage/init` only chmods
the audio and hw nodes), so an app uid cannot open it and nothing in the tree
tries to. It is not a privilege; it is there so a future `procs = yes` app can
be handed a console without changing the sandbox. What the wider view costs,
precisely:

* the app can read every process's name and command line
  (`/proc/<pid>/stat`, `/proc/<pid>/cmdline`, `/proc/<pid>/comm`) and
  system-wide files (`/proc/meminfo`, `/proc/stat`, `/proc/net/dev`);
* it still cannot read another uid's `/proc/<pid>/environ`, `mem`, `fd` or
  `maps` — those are root/owner-only and it is neither;
* it still cannot signal another app (different uid), and it still has its own
  mount namespace, its own tmpfs, and no *host* `/root` — only its own appdata
  bound there. `procs = yes` says nothing about the network: an app that also
  says `network = host` keeps the host netns, and the engine does exactly that.

Two shipped manifests say it today: `sysmon`, and the browser engine's
*placeholder* bundle (`stage/usr/share/mystic/apps/engine/manifest`). The
engine package builder `tools/fetch-engine.py` writes the real bundle's
manifest without a `procs` line, so a fetched engine is an ordinary chrooted
app with its own pid namespace; the shipped placeholder is the one that asks
for the wider view, and the client only reads `/proc/self/exe`, which a private
procfs provides just as well. Dropping that line
from the placeholder manifest would make sysmon the only exception again. Every
other bundle gets the private namespace, and a bundle that asks for
`procs = yes` without needing it is simply giving up a layer it did not have
to.

---

## 4. Pixel buffers

The server creates one file per client and maps it; the app opens the path the
`OK` line gave it. Both must be the same inode, so the path is the app's view:

* jailed client: `<run>/jail/<id>/dev/shm/<leaf>` on the host, `/dev/shm/<leaf>`
  inside the jail — the same file, because the tmpfs is shared;
* a client the desktop did not launch (an app started from a shell, a
  developer's test client): `/dev/shm/<leaf>`, and it is not chrooted.

The file is `0600` and owned by the connection's real uid, inside that uid's
`0700`, 64 MiB tmpfs. Another app cannot open it — it has no path to it and,
even with one, no permission. That closes the exact hole the audit proved:
read, forge and `ftruncate` of *another* app's buffer.

---

## 5. What the desktop owes the sandbox

`stage/init` provides, and `wms_start()` re-checks:

* `/dev/shm` on tmpfs (128 MiB, `1777`, `nosuid,nodev`) — the server's buffers;
* `/run/mystic` and `/run/mystic/jail`, `root:root 0755`;
* devtmpfs, so `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/console` and
  `/dev/snd` exist to be bound;
* procfs on `/proc`, so a `procs = yes` app has the host view to bind.

The uids 2000..2015 need no `/etc/passwd` entry: `setuid(2)` takes a number.
The jail now carries the host `/etc` (read-only), but the shipped `/etc` has no
`passwd` file in it, so the app sees no user list either way.

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
   Two manifests currently ask for it (sysmon and the engine placeholder).
3. **No seccomp filter.** The isolation is namespaces + DAC + chroot. An app can
   use every syscall its uid is permitted, including `clone`, `mount` (fails: no
   `CAP_SYS_ADMIN`), `ptrace` of itself, and — with `network = host` — real
   sockets.
4. **`audio = yes` binds `/dev/snd`**, the whole directory, but DAC decides
   what is reachable: `stage/init` opens only the playback PCM nodes
   (`pcmC*D*p`, 0666) so the Clock can beep; capture nodes, `controlC*`, `seq`
   and `timer` keep devtmpfs's `0600 root:root`, so a sandboxed app can play and
   cannot record. If init ever opens a capture node, this item becomes the
   recording-device warning it used to be. Only bundles whose manifest asks for
   `audio = yes` get the directory at all.
5. **The window socket is mode 0666.** Any local process can connect and open a
   window; it cannot read another app's buffer (0600) and cannot claim another
   app's identity (`SO_PEERCRED`). This is a single-user machine whose local
   processes are all ours.
6. **No user namespace.** Apps run as real host uids, not as uid 0 in a mapped
   namespace. That is deliberate (it keeps `/root` readable by root only), but
   it means a kernel bug in a namespace is a direct escape rather than one that
   has to be chained.
7. **`/dev/shm` inside the jail does not survive the app.** Whatever is written
   there lives in a per-app tmpfs and is gone when the bundle's last process
   exits. Persistent state belongs in `/root`. Nothing shipped depends on the
   other path today: `g2048` tries `/root/g2048.best` first
   (`src/client/apps/g2048.c`, `BEST_CANDIDATES`), `/root` is the appdata bind,
   it is writable by the app, and the launch check refuses to start the app if
   it is not. `g2048.c`'s own header comment still describes the sandbox as it
   was before the per-app tmpfs and the appdata bind — host `/dev/shm`, an
   unwritable `/root` — so the candidate order in the code, not that comment,
   is what runs. Keep the rule for anything new: `/dev/shm` is scratch, `/root`
   is storage.
8. **A desktop that dies without cleaning up leaves that bundle's tmpfs
   mounted** until the bundle is launched again or the machine reboots
   (mounts live in the desktop's namespace). It is bounded — two per bundle,
   64 MiB each at 1280x800, so 128 MiB per bundle — and the next launch
   detaches it.
9. **Two bridges deliberately cross the boundary**: the file picker (files in
   `<jail>/tmp`) and the clipboard (`<jail>/run/mystic/clipboard.sock`, served
   by the desktop). Both are one-way or text-only, and both are the reason the
   app can be useful at all. They are the first place to look if this sandbox
   is ever audited again.
10. **`/etc` is in every jail, and it carries two secrets.** The read-only bind
    in §2.3 hands every app `/etc/mystic/passwd` — the lock screen's
    `salt:20,000:SHA-256(password)` line (`src/sha256.c`), a construction its
    own header calls small enough to read and check by hand — the same design
    choice that makes it cheap to search offline — and `/etc/mystic/luks.key`,
    the 32-byte plaintext boot half of the 64-byte disk key
    (`docs/ENCRYPTION.md`). An app can copy either into `/root/appdata/<id>`,
    where it survives a reboot. That hash protects the same password the boot
    half is sealed under (`docs/ENCRYPTION.md` §6.2), so cracking it once is
    enough for both; and the boot half plus a copy of the disk image is the
    whole key, because the other half sits unencrypted in the disk header. The
    fix is to stop binding the directory and bind the handful of files that are
    needed (`resolv.conf`, `hosts`, the CA bundle, the fontconfig files), or to
    move the key and the lock hash out of `/etc`. Until then, half of the disk
    key is not a secret from a sandboxed app.
11. **The 64 MiB floor is a memory ceiling, not a small number.** Each app may
    keep 64 MiB resident in `/dev/shm` and another 64 MiB in `/tmp`, and tmpfs
    pages are real memory once written. Eight running bundles can therefore
    pin a gigabyte without allocating one byte through the window server;
    before the floor the same eight were bounded near 9 MiB each. That is the
    price of an engine that starts, and it is paid in RAM, not in isolation.
12. **The two escape hatches in §2.6 are honoured, not locked.** `chroot = no`
    and `host = yes` are one manifest line away, and an installed bundle's
    manifest is read exactly like a shipped one. One `pkg install` of a bundle
    whose manifest says `host = yes` is all it takes for that bundle to run in
    the desktop's namespaces; the only thing in front of that is who may
    publish to the package repository.

---

## 7. Proving it

The plan is data, and the data is in the shipped file, so the parts that do not
need a kernel can be tested on the build machine:

```
$ zig cc -target x86_64-linux-musl -Os -static -Isrc \
      -Wall -Wextra -Wno-unused-parameter -c src/wmsrv.c -o .work/sandbox/x.o
        (silent)

$ zig cc -Wall -Wextra -Isrc <scratch>/plan_harness.c -o <scratch>/plan.exe
$ ./<scratch>/plan.exe
        ... 50 checks, 0 failure(s)   (exit 0)
```

The harness `#define MYSTIC_SANDBOX_PLAN` and `#include`s the **real**
`src/wmsrv.c`, so it runs the real `sbx_cred_acquire()`, `sbx_cred_del_pid()`,
`sbx_plan_init()`, `sbx_tmpfs_size()` and `sbx_manifest_line()`. It is scratch,
not shipped: the copy used for the numbers above was written for this revision,
because the one the previous revision of this document pointed at predates the
`no_chroot` parameter and no longer compiles. Rebuild it whenever
`sbx_plan_init()`'s signature changes. It proves:

* two different apps get two different uids (2000, 2001, 2002), a second window
  of the same app keeps its uid, closing one window of a running bundle does
  **not** release it, a bundle with no process left is released, and the ninth
  bundle is refused rather than sharing a uid;
* `sbx_tmpfs_size()` at 1280x800, 1920x1080 and 3840x2160: the first two are
  the floor (67,108,864 B), the third is the formula (67,403,776 B), and
  9,240,576 B — the number this document printed before the floor existed — is
  *not* what is mounted;
* `/dev/shm` and `/tmp` are that size, `mode=0700,uid=<the app>,gid=<the app>`,
  both mounted **before** the fork, `/dev/shm` `noexec` and both `nosuid,nodev`;
* `/dev/zero` and `/etc` (read-only) are in every plan — the `chroot = no`
  one included — and `/dev/console` only in a `procs = yes` plan;
* `CLONE_NEWPID` is in the plan for an ordinary app and absent for
  `procs = yes`, with a fresh procfs in the first case and a read-only host
  `/proc` in the second; `network = host` removes `CLONE_NEWNET`; the engine
  manifest's combination (`procs = yes` + `network = host`) has neither;
* `chroot = no` removes the chroot step while keeping both private tmpfs and
  the read-only `/app` bind;
* `procs`, `chroot` and `host` are three independent switches, and any value
  other than the exact widening one (`yes`, `no`, `yes`) leaves the sandbox
  alone;
* `sbx_target_verdict()` refuses a symlink target — for a file and for a
  directory — accepts a real directory and a real file, creates a missing one,
  and refuses a FIFO and a path it cannot inspect. That is the decision
  `sbx_walk()` branches on in §2.5.

What it does **not** prove, because a Windows build machine has no `mount(2)`:
the syscalls themselves. Those are covered by the compile, by the executor
being a single `switch` over the same step list the printer prints, and by the
launch-time `access()` checks in §2.4, which fail the launch if the result is
not what the plan said.

The other half of the proof is on the machine: every launch prints one line

```
wms: sandbox: uids 2000..2015, one per bundle (gid == uid, no supplementary groups), private /dev/shm and /tmp per app
wms: paint connected 520x360 as uid 2000 (/run/mystic/jail/paint/dev/shm/mystic-312-1)
wms: paint (uid 2000) disconnected; uid released
wms: sandbox: unmounted /run/mystic/jail/paint/dev/shm
```

and `MYSTIC_SANDBOX_PRINT=1` prints the full plan for each app as it starts.
