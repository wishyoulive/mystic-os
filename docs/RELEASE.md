# Releasing Mystic OS v1

This is the release guide for **Mystic OS v1**: the zip users download, what is
inside it, how it is produced, how to run it, and how to prove that it booted.
The executable half of this document is `tools/release.sh`; the copy the user
reads is the `README.txt` generated into the bundle.

Everything below describes the tree as it exists on this machine. Paths are
relative to the repository root (`C:\Users\saket\dev\mystic` here).

---

## 1. What a release is

The shipping unit is one file:

```
build/release/mystic-os-v1.zip
```

It is **not** an ISO, and it is not produced by the `zip` command. This machine
has no `zip` binary, so `tools/release.sh` writes the archive with Python's
standard library:

```
python -m zipfile -c mystic-os-v1.zip vmlinuz-virt initramfs.cpio.gz run.sh README.txt
```

The user needs nothing but QEMU and a shell (Git Bash on Windows). They extract
the zip and run `run.sh`; no build tool, compiler or network download is needed
to boot it.

The two payloads come from the normal image build:

| Input | Produced by | What it is |
|---|---|---|
| `build/cache/vmlinuz-virt` | `tools/fetch-kernel.sh` | Alpine's `linux-virt` kernel, 6.18.63 in this build (`tools/fetch-kernel.sh:15`), unmodified |
| `build/initramfs.cpio.gz` | `tools/mkimage.py` | gzip'd cpio containing the Alpine base rootfs with `stage/` laid over it: our `/init`, `mystic-desktop`, `pkg`, the app bundles, kernel modules, fonts and themes |

`tools/release.sh` **never downloads and never compiles**. If `src/` or
`stage/` changed since the last image build, rebuild first (`./image.sh`); a
release script that rebuilds silently can ship a half-built image, so this one
refuses to guess.

---

## 2. What is in the zip

All four entries are at the top level of the archive (no enclosing folder):

| Entry | Mode | Purpose |
|---|---|---|
| `vmlinuz-virt` | `0644` | the kernel, booted by `-kernel` |
| `initramfs.cpio.gz` | `0644` | our userspace, loaded by `-initrd` |
| `run.sh` | `0755` | finds QEMU, creates the 2 GB disk on first run, boots the two files above |
| `README.txt` | `0644` | what works, what does not, controls, and how to check the boot |

In the build this document was written against, the payloads are
12,657,664 bytes (kernel) and 12,809,272 bytes (initramfs), and the finished
zip is about 24 MB. `release.sh` measures both payloads and writes their
sizes, rounded to one decimal MB, into the generated `README.txt` on every run;
the exact byte counts are printed on the terminal and the zip's own byte count
is printed at the end. The rounded README values cannot go stale because they
are generated.

`build/release/stage/` keeps the same four uncompressed files next to the zip,
so a release can be inspected without extracting it.

---

## 3. Building the release (maintainer)

```bash
# 1. Build the image that should ship. This compiles the desktop, packs
#    stage/ over the Alpine base, and boots it. Use `probe` for a headless
#    build+check that powers off instead of opening a window.
./image.sh
# or: ./image.sh probe

# 2. Package the existing image.
./tools/release.sh
```

On success the last lines are the final size and hash:

```
release: wrote /.../build/release/mystic-os-v1.zip
         25105270 bytes  (23.9 MB)
         sha256 1d59ff7108f953a861f7067e18c2082d353dd1ca2096bc8630de232e90ccdc0d
```

That is the dry run recorded in section 8. The byte count and hash change
slightly from run to run because the archive stores staging timestamps and the
README records the build time; the script prints the real values for the file
it just wrote (and a hash copied from here will not match a later run).

The script also lists the archive contents and verifies it before printing
those numbers.

### What `release.sh` checks before it writes anything

1. Both inputs exist, are regular files, and are not empty.
2. The kernel has the x86 boot header magic `HdrS` at byte 0x202 — a
   non-empty text file with the right name is rejected, not shipped.
3. The initramfs starts with the gzip magic `1f 8b`.
4. A Python interpreter with the `zipfile` module is available.
5. The staged copies are the same size as the inputs (catches a full disk).
6. The generated run.sh is executable.
7. After zipping: an explicit `ZipFile.testzip()` call returns no bad member,
   the archive contains exactly the four expected names, and `run.sh` is
   executable inside the archive. (`python -m zipfile -t` alone is not enough:
   its exit status does not fail on a CRC-only mismatch.)

Failure messages say which input is wrong and what to do. Nothing is written
when an input check fails; the temporary zip is removed on any error.

### Why the archive modes are rewritten

Windows has no POSIX mode bits: `chmod +x` on Git Bash is not visible to a
native Windows Python, so `python -m zipfile -c` stores `0666` for every entry.
Extracting that on Linux or a Mac would give a `run.sh` that cannot be
executed. After creating the zip, `release.sh` rewrites the central directory
with `create_system = 3` (Unix) and explicit modes: `0644` for the three data
files, `0755` for `run.sh`. This is the one post-processing step, and the
archive itself is still created by `python -m zipfile -c` as required by the
no-`zip` rule.

### Testing with a fake build tree

`MYSTIC_BUILD` moves the whole `build/` root (inputs and output) without
changing any file in the repository:

```bash
rm -rf /tmp/fake-build
mkdir -p /tmp/fake-build/cache
cp build/cache/vmlinuz-virt /tmp/fake-build/cache/
cp build/initramfs.cpio.gz /tmp/fake-build/
MYSTIC_BUILD=/tmp/fake-build ./tools/release.sh
# -> /tmp/fake-build/release/mystic-os-v1.zip
```

This is how the release mechanics were verified without writing a release zip
into the working tree. With missing or malformed files in the same tree the
script exits 1 and writes nothing.

---

## 4. Running the bundle (user)

Extract the **whole** zip into an empty directory. The entries are at the root
of the archive, so extracting it into a shared folder scatters four files.

```bash
# Linux / macOS / Git Bash on Windows
mkdir mystic-os-v1
cd mystic-os-v1
python -m zipfile -e ../mystic-os-v1.zip .   # or: unzip ../mystic-os-v1.zip
./run.sh
```

On Windows, Explorer's "Extract All" works too; run `run.sh` from Git Bash
afterwards, not by double-clicking it. `run.sh` needs `bash`.

QEMU must be installed. `run.sh` looks for `qemu-system-x86_64` on `PATH`, then
the standard Windows (`C:\Program Files\qemu`) and macOS locations. Any other
install can be used with:

```bash
QEMU=/path/to/qemu-system-x86_64 ./run.sh
```

### First run

`run.sh` creates `mystic-disk.img` the first time: a blank 2 GB raw disk made
with `dd`, then checks that the file really is 2,147,483,648 bytes (and
refuses to continue if not). The OS formats it ext4 on first boot
(`stage/init:82-103`), mounts it on `/root`, and keeps installed apps and user
files there. Later runs reuse the file. Delete `mystic-disk.img` to return to a
factory-fresh system; that also deletes everything under `/root` and every
installed package.

The VM is given 1 GB of RAM, a 1280x800-capable `-vga std` display, user-mode
networking and the virtio disk. The exact QEMU invocation is:

| Argument | Why |
|---|---|
| `-kernel vmlinuz-virt -initrd initramfs.cpio.gz` | boot our payloads directly, no bootloader |
| `-m 1024` | 1 GB of RAM |
| `-vga std` | bochs/simpledrm framebuffer; the desktop draws into `/dev/fb0` |
| `-drive file=mystic-disk.img,format=raw,if=virtio` | the 2 GB disk, created on first run |
| `-netdev user,id=n0 -device virtio-net-pci,netdev=n0` | user-mode network (guest gets 10.0.2.15); needed by `pkg` and the browser |
| `-accel whpx -cpu qemu64` | Windows Hypervisor Platform plus the explicit CPU model WHPX needs here (`tools/smoke.sh:139`); both only when `qemu -accel help` lists `whpx` |
| `-no-reboot` | a crash stops QEMU instead of rebooting forever |
| `-append "console=ttyS0 rdinit=/init loglevel=6"` (headless) | serial console, start our `/init` as PID 1, and keep kernel notices such as the version banner on the log |
| `-append "console=ttyS0 rdinit=/init loglevel=3 quiet"` (window) | the same boot with a quiet kernel log; this is why window mode has no verbose serial output |
| `-display none -monitor none -serial stdio` | headless mode only: serial output on the terminal, monitor off so stdio belongs to the serial port |

WHPX makes the VM fast on Windows. If the host QEMU does not have it (Linux,
macOS, or an old build), `run.sh` prints a note and boots without `-accel whpx`
and without `-cpu qemu64`: same OS, slower emulation.

### Controls

| Key / action | Effect |
|---|---|
| `Super` or the eye in the top bar | open/close the launcher |
| `F1` … `F5` | terminal, files, settings, monitor, notes (`src/main.c:1526`) |
| `Ctrl+1` … `Ctrl+6` | switch theme (`src/main.c:1429`); the themes are `stage/etc/mystic/themes/` |
| `Ctrl+Alt+L` | lock the screen; the v1 password is `mystic` (`stage/etc/mystic/passwd`) |
| `Ctrl+Alt+G` | QEMU's shortcut: give the mouse back to the host |
| close the window, or `Ctrl+C` in headless mode | stop QEMU |

Anything typed goes to the focused window. The calculator, terminal and
browser all take keyboard input from the same evdev path.

---

## 5. Verifying that it booted

There are two levels: the serial log (scriptable) and the window (visual).

### Serial log — the strict check

Run the headless mode and keep the output:

```bash
cd mystic-os-v1
./run.sh headless | tee serial.log
```

Wait a few seconds, look for the desktop line, then press `Ctrl+C`. If GNU
`timeout` is available, this is non-interactive:

```bash
timeout 60 ./run.sh headless > serial.log 2>&1 || true
```

A healthy boot contains these lines, in this order:

```
Linux version 6.18.63-0-virt ...
net: eth0 up, 10.0.2.15/24
disk: mounted /dev/vda (ext4) on /root
starting mystic-desktop
display: 1280x800 32bpp stride=1280
ui: scale 1.12
input: keyboard /dev/input/event0 (AT Translated Set 2 keyboard)
input: pointer  /dev/input/event1 (ImExPS/2 Generic Explorer Mouse)
wms: listening on /run/mystic/wm.sock
wms: N installed app bundle(s)
```

The kernel version, input device names, UI scale and app-bundle count depend
on the machine and the build. `run.sh headless` passes `loglevel=6` so the
kernel banner (a notice-level printk that `loglevel=3` would filter out)
reaches the log; window mode runs quietly and is not used for this check.
These are the six lines the boot check asserts:

| Line | Emitted by | Proves |
|---|---|---|
| `Linux version 6.18...` | the kernel | the kernel was loaded and decompressed |
| `net: eth0 up, 10.0.2.15/24` | `stage/init:54` | virtio-net is up and DHCP leased from QEMU's user network |
| `disk: mounted /dev/vda (ext4) on /root` | `stage/init:85` | virtio-blk works and the data disk was mounted |
| `starting mystic-desktop` | `stage/init:197` | our PID 1 reached `exec /usr/bin/mystic-desktop` |
| `display: 1280x800 32bpp stride=1280` | `src/fb.c:49` | the framebuffer mapped and is the expected shape |
| `wms: listening on /run/mystic/wm.sock` | `src/wmsrv.c:114` | the window server started and apps can connect |

On the **first** boot the disk line is instead:

```
disk: formatted ext4 and mounted on /root
```

because the blank 2 GB image has no filesystem yet (`stage/init:101`). The
second boot says `mounted`. Both mean the disk path worked. If the line says
`disk: could not mount` or `disk: no /dev/vda`, the VM is not getting the
virtio disk — check that `mystic-disk.img` exists and that the `-drive` line
was not edited.

The scriptable check asserts all six. The disk test accepts the first-boot
wording `formatted ext4 and mounted` and the later-boot wording `mounted`:

```bash
grep -q  'Linux version 6.18' serial.log &&
grep -q  'net: eth0 up, 10.0.2.15/24' serial.log &&
grep -Eq 'disk: (mounted /dev/vda \(ext4\) on /root|formatted ext4 and mounted on /root)' serial.log &&
grep -q  'starting mystic-desktop' serial.log &&
grep -q  'display: 1280x800 32bpp' serial.log &&
grep -q  'wms: listening on /run/mystic/wm.sock' serial.log &&
echo 'BOOT OK'
```

`wms: listening...` is the last line of the six: after it the window server is
accepting connections, the desktop scans the app bundles and enters its event
loop, so no further "ready" message is printed. The following
`wms: N installed app bundle(s)` line is a useful extra check that
`/usr/share/mystic/apps` was found; `N` depends on the image.

### Window — the visual check

Run `./run.sh` (no argument). Within a few seconds the QEMU window shows the
desktop: wallpaper, top bar with the clock, the eye on the left. Press `Super`
and the launcher appears; `F1` opens the terminal. That is the same state the
serial lines describe.

Note that serial output is only visible in headless mode. In window mode
it goes to QEMU's default console device; the framebuffer window is the
desktop.

### If it does not boot

| Symptom | Cause / fix |
|---|---|
| `error: qemu-system-x86_64 not found` | install QEMU or set `QEMU=`; the window will not start without it |
| `note: WHPX is not available` | normal on Linux/macOS; TCG is slower but boots |
| `Could not open 'mystic-disk.img': Permission denied` | another QEMU still has the file open; stop it or delete the file |
| `error: missing or empty vmlinuz-virt` | only part of the zip was extracted; extract all four entries |
| no `starting mystic-desktop` in serial.log | `/init` did not run; check that `-initrd` points at the extracted `initramfs.cpio.gz` |
| `disk: /dev/vda did not mount, creating ext4 filesystem` on a disk you care about | **known risk**: `stage/init` formats whenever the mount fails, not only on a blank disk (comment at `stage/init:88-89`). Keep backups of `mystic-disk.img`; do not treat it as the only copy |

---

## 6. What the release claims

The generated `README.txt` and this section say the same thing. Everything
listed here is visible in the source tree:

**Works**

- **Our own userspace as PID 1** on Linux 6.18: `stage/init`,
  `stage/usr/bin/mystic-desktop`, `stage/usr/bin/pkg`. The desktop is not a
  session inside another distro; `stage/` is laid over a minimal Alpine base
  for kernel modules and busybox only.
- **Our compositor and window manager** (`src/main.c`, `src/gfx.c`,
  `src/font.c`): six themes (`stage/etc/mystic/themes/`), four wallpapers
  (`src/wallpaper.c:36`), dock, launcher, lock screen, 1280x800 framebuffer.
  No X11, no Wayland, no GPU driver: pixels go to `/dev/fb0` (`src/fb.c`).
- **Our terminal** (`src/term.c`, a real pty and VT100 subset) and the
  in-desktop apps: files, settings, monitor, notes, calculator, Apps store.
- **Our HTTP client and HTML renderer** (`src/client/apps/web.c`): its own
  DNS/TCP/HTTP code and HTML-to-text layout. Plain HTTP only; the code
  rejects `https://` with an explicit message (`src/client/apps/web.c:472`).
- **Sandboxed external apps** (`src/wmsrv.c`): each app is a separate process,
  uid 1000, in private mount/IPC/UTS/network namespaces inside a chroot
  (`src/wmsrv.c:406`, `:482`, `:485`). Inherited descriptors are closed. An app
  whose manifest says `network = host` keeps the real network namespace; the
  browser does, the games do not.
- **The package store**: `stage/usr/bin/pkg` plus the in-desktop Store. Each
  download is checked against the index's SHA-256 before it is unpacked
  (`stage/usr/bin/pkg:308`), and app bundles install at runtime onto the data
  disk. The repository is a GitHub release; the default base URL is in
  `stage/etc/mystic/repo.conf`.
- **A persistent virtio disk**: ext4 on `/dev/vda`, mounted on `/root`, with
  `/root` and installed apps surviving a reboot.

**Does not work yet — honestly**

- **No disk encryption.** `stage/init` mounts `/dev/vda` with `mount -t ext4`
  directly (`stage/init:84`); there is no LUKS or `cryptsetup` in `stage/`.
  Anyone holding the zip or `mystic-disk.img` can read its contents. Do not put
  secrets on it.
- **No Wine.** There is no Wine binary, module or source in the tree (a search
  for `wine` under `src/` and `stage/` finds nothing). Windows programs do not
  run. Apps are Linux ELF binaries built static against musl.
- **No DRM video, and no video at all.** There is no media player, no codec
  stack and no content-protection module (Widevine or otherwise). The kernel's
  Direct Rendering Manager (`drm.ko` and friends) is present, but those are
  display drivers for the framebuffer, not video decoding or DRM playback.
  `src/fb.c` and `src/gfx.c` are how pixels are produced; the browser renders
  text, never video or images (its own header, `src/client/apps/web.c:28`,
  says "no CSS and no images").
- **The browser is plain HTTP only**: no HTTPS, no JavaScript, no CSS, no
  images, no video. It reflows HTML to text and numbers links.
- **Packages are signed, and the key ships with the image.** The repository
  index is Ed25519-signed and checked against `/etc/mystic/repo.pub` before a
  single row is read; every tarball is then checked against the sha256 in that
  signed index and refused on a mismatch. This stops a hostile network or
  mirror, but not an attacker who can rewrite the boot image and replace the
  key with it - there is no secure-boot chain. See `docs/SIGNING.md`.
- **It is a QEMU bundle, not an ISO.** There is no bootloader image, no
  partitioning step and no bare-metal installer. `run.sh` boots the kernel
  directly with `-kernel`/`-initrd`.
- **The desktop is CPU-drawn.** There is no GPU acceleration; heavy animation
  and full-screen scrolling cost CPU on purpose, in exchange for running on a
  framebuffer that any old PC or QEMU provides.

---

## 7. Rebuilding, versioning, publishing

### The release must match the image

`tools/release.sh` packages whatever is in `build/initramfs.cpio.gz` and
`build/cache/vmlinuz-virt`. After changing any file under `src/` or `stage/`,
run `./image.sh` again before `./tools/release.sh`. The release script
deliberately does not call the compiler: a mismatch between a new binary and
an old image is exactly the kind of release bug this script must not hide.
It does print a warning (not a failure) when files under `src/` or `stage/`
are newer than the initramfs, so a stale image is a conscious choice.

### Version

The string `v1` appears in three places:

1. `NAME="mystic-os-v1.zip"` in `tools/release.sh`,
2. the `MYSTIC OS v1` heading and `v1` references in the generated
   `README.txt` heredoc in `tools/release.sh`,
3. this document's title and bundle names.

There is no version variable to update beyond that; a v2 release should change
those together and keep the same four-file layout.

### Publishing checklist

1. `./image.sh` (or `./image.sh probe`) — build the final image.
2. `./tools/release.sh` — package it; capture the final size and SHA-256 it
   prints.
3. Optional extra CRC check:
   `python -c 'import zipfile,sys; b=zipfile.ZipFile(sys.argv[1]).testzip(); print("OK" if b is None else "BAD: "+b); sys.exit(1 if b else 0)' build/release/mystic-os-v1.zip`
   — the script already runs this same `testzip()` and fails on the first bad
   member.
4. Upload the zip as a release asset. Publish the SHA-256 next to it so users
   can check what they downloaded; the script prints it for this purpose.
5. Keep `build/release/stage/` or the zip itself, not just the loose payloads;
   the run.sh and README inside the zip are generated and are not in `stage/`.

The zip is a build artifact and is not part of the source tree; do not commit
it. The two inputs and the archive live under `build/`, which is generated.

---

## 8. How this release process was verified

On 2026-11-17, on this machine, without booting QEMU:

- `bash -n tools/release.sh` — clean, no syntax errors.
- A full run against `/tmp/relbuild` (copies of the real
  `build/cache/vmlinuz-virt` and `build/initramfs.cpio.gz`) produced
  `/tmp/relbuild/release/mystic-os-v1.zip`: 25,105,270 bytes (23.9 MB; earlier
  runs differed by a few bytes because timestamps change), four entries, `run.sh`
  mode `0755`, kernel and initramfs modes `0644`.
- Extracted payload SHA-256 values matched the inputs byte for byte. An
  explicit `ZipFile.testzip()` check returned no bad member. Corrupting a
  member in a synthetic zip made `testzip()` return its name (while
  `python -m zipfile -t` still exited 0), and corrupting a data byte in a copy
  of the generated zip made it return `error: ... invalid bit length repeat`.
  Injecting that corruption into a copy of the build made `release.sh` exit 1
  with `archive failed its CRC/decompression test: ...` and write no archive.
- The generated `stage/run.sh` was run against a fake `QEMU=` recorder:
  with `MYSTIC_DISK_MB=8` (the default is 2048) it created the disk only on
  the first run, passed `-m 1024`, `-netdev user,id=n0`, `-device
  virtio-net-pci,netdev=n0`, the virtio `-drive`, and `-accel whpx -cpu
  qemu64` when `-accel help` listed WHPX (and omitted both cleanly when it did
  not). Headless mode added `-display none -monitor none -serial stdio` and
  `loglevel=6`; window mode added `loglevel=3 quiet` and no `-display`.
  A missing `vmlinuz-virt` was refused. A run with no `MYSTIC_DISK_MB`
  override created exactly 2,147,483,648 bytes (2 GiB) on the first run and
  kept the file on the second.
- Refusal cases — absent build tree, zero-byte inputs, a non-bzImage kernel,
  a non-gzip initramfs — all exited 1 and wrote no archive.
- The six-line boot check was run against `build/serial.log` (captured at
  `loglevel=6`): it passed. A copy of that log with the `Linux version` line
  removed failed the first grep, so the check is not vacuous.
- **Not verified here:** an actual boot of this exact archive. QEMU was not run
  for this change. The expected boot lines above are the strings the code
  emits (`stage/init`, `src/fb.c`, `src/wmsrv.c`), and the boot check is a
  documented user step rather than something `release.sh` performs. Run
  `./run.sh headless` and the grep test from section 5 to verify a real boot.

## 9. Related files

| File | Role |
|---|---|
| `tools/release.sh` | builds the zip, refuses bad inputs, prints size and SHA-256 |
| `image.sh` | builds the desktop, packs `stage/` and boots the image |
| `tools/build-desktop.sh` | compiles `src/` into `stage/usr/bin/mystic-desktop` |
| `tools/mkimage.py` | packs `stage/` over the base rootfs into `initramfs.cpio.gz` |
| `tools/fetch-kernel.sh` | downloads the pinned Alpine kernel and its modules |
| `tools/check-size.sh` | enforces the 40 MB shipping budget on kernel + initramfs |
| `stage/init` | PID 1: modules, network, disk, then `exec mystic-desktop` |
| `stage/usr/bin/pkg` | package manager (runtime app installs) |
| `stage/etc/mystic/repo.conf` | package repository base URL |
