# Contributing to Mystic OS

You can write C. You want a window with your pixels in it, on a real screen,
booted from a real kernel. This document is the path from an empty file to that
window, and it is written from the tree as it is — every command and path below
was read out of the files named next to it, and where a number was measured by
booting the image the text says so. Where the code and this text disagree, the
code is right.

Read `README.md` (what the OS is and the rules it keeps), `docs/ARCHITECTURE.md`
(how a boot becomes a desktop) and `docs/IPC.md` (the display protocol) first.
This guide is the practical companion to those three.

---

## 1. What you need installed

| Tool | Why | How the scripts find it |
|---|---|---|
| `qemu-system-x86_64` | the whole OS runs in it — there is no other way to see your app | `$QEMU`, else PATH, else `/c/Program Files/qemu/qemu-system-x86_64.exe`; `image.sh:25` errors out if none |
| `zig` | used as the cross-compiler (`-target x86_64-linux-musl -static`), so you can build Linux binaries from Windows with no Linux toolchain | `$ZIG`, else PATH, else the WinGet `zig.zig_*` package path (`tools/build-desktop.sh:13`, `tools/mkapp.sh:23`) |
| `python` 3 | `tools/mkimage.py` packs the initramfs, `tools/ppm2png.py` converts screenshots | `image.sh` invokes it literally as `python` with `PYTHONIOENCODING=utf-8` |
| `curl` | first run only: the kernel package and the Alpine minirootfs | `tools/fetch-kernel.sh`, `image.sh:39` |

Not needed for `./image.sh`: **nasm**. `nasm` is only used by `./build.sh`, the
stage-1 512-byte boot sector kept as a lesson (`boot/boot.asm`). `image.sh` never
calls `build.sh`. The README mentions nasm because that path is still in the
tree, not because the desktop needs it.

First run needs a network connection (kernel package ~43 MB, rootfs ~3 MB).
After that everything is cached in `build/cache/` and boots are local.

---

## 2. Build and boot with one command

```bash
./image.sh
```

That is `./image.sh run` (the `case` defaults to `run`). It does, in order:

1. `./tools/fetch-kernel.sh` — only if `build/cache/vmlinuz-virt` or the module
   tree is missing. Downloads the Alpine `linux-virt` package, copies
   `vmlinuz-virt`, and stages whole module subtrees into `stage/lib/modules/`.
2. Downloads `build/cache/minirootfs.tar.gz` (Alpine 3.24.4 x86_64) — only if absent.
3. `./tools/build-desktop.sh` — compiles `build/mystic-desktop` with zig and
   copies it, plus the Lato fonts, into `stage/`.
4. `tools/mkimage.py --base … --stage stage -o build/initramfs.cpio.gz` — builds
   the cpio that becomes the root filesystem in RAM.
5. Creates `build/disk.img` (64 MB) if absent. The guest formats it ext4 on
   first boot; `/root` then survives reboots.
6. Boots QEMU: `-kernel … -initrd … -m 256 -vga std -no-reboot
   -drive file=build/disk.img,format=raw,if=virtio`, plus `-accel whpx` when the
   host advertises it.

Close the QEMU window to stop it. Inside the desktop: `1` `2` `3` switch theme,
`F1`…`F5` open Terminal/Files/Settings/Monitor/Notes, Super (or the eye in the
top-left) opens the launcher, `Ctrl+Alt+L` locks the screen.

Other modes, all from `image.sh`:

| Command | What it does | Output |
|---|---|---|
| `./image.sh` / `./image.sh run` | boot the desktop in a window | the window |
| `./image.sh probe` | boot headless, print machine facts, power off | probe block on stdout |
| `./image.sh shot 10` | boot headless, screendump after 10 s | `build/shot.png` |
| `./image.sh themes` | one boot, screenshot each theme via `sendkey` | `build/theme1.png` … `theme3.png` |

There is no incremental path: every `./image.sh` rebuilds the desktop, repacks
the initramfs and reboots. Measured on the machine this guide was written on:
12 s for `probe` (build + pack + boot + poweroff) and 21 s for `shot 8`, of
which 8 s is the wait, not the build. It is also why "it worked on my machine"
and "it booted" are the same sentence in this project.

---

## 3. Two ways to add an app

|  | **External app** (recommended) | **In-desktop app** (AppVT) |
|---|---|---|
| Runs as | its own Linux process, forked by the launcher | code inside `mystic-desktop` |
| New files | one: `src/client/apps/<id>.c` | one: `src/apps/<id>.c`, plus edits to 4 shared files |
| Talks to the desktop via | unix socket + shared memory (`src/ipc.h`) | the `AppVT` callback table in `src/mystic.h` |
| Build | `./tools/mkapp.sh <id>` | `tools/build-desktop.sh` |
| Appears in the launcher | automatically, once a bundle exists | only if you add it to `LAUNCHER[]` |
| If it crashes | one dead window; the desktop keeps running | the whole desktop dies |
| Drawing | you own a `w × h` pixel buffer | you draw into the scene surface, inside your rect |
| Can use | libc and anything you can link statically | libc, `gfx.c`, `font.c`, and the desktop's internals |

**External is easier, and it is the recommended path.** Three reasons, all
mechanical: you add exactly one file and run exactly one packaging command; you
do not touch `src/mystic.h`, `src/apps.c`, `src/main.c` or
`tools/build-desktop.sh`; and an external app that dereferences NULL costs you a
window instead of the session. `src/client/apps/clock.c` is a complete, working
example of the shape.

An in-desktop app is the right choice when the app has to *act on the desktop
itself*: Settings writes `/etc/mystic/current-theme`, which the desktop re-reads
once a second (`src/main.c`), and the store writes `/run/mystic/launch.request`
because only the desktop may fork and exec a bundle (`src/apps/store.c:348`).
Those need the desktop's process, not a socket.

### If you do want an in-desktop app

The registry is opt-in at five places, and a missing one fails silently (the app
compiles but nothing can open it):

1. `src/apps/<id>.c` defines `const AppVT app_vt_<id> = { .id, .title, .init,
   .draw, .key, .click, .resize, .free };` — see `src/apps/sysinfo.c` (the
   reference app) or `src/apps/files.c`.
2. `src/mystic.h` — an `#ifdef WITH_<ID> extern const AppVT app_vt_<id>;`
   block next to the existing ones.
3. `src/apps.c` — add `&app_vt_<id>` to `REGISTRY[]` inside `#ifdef WITH_<ID>`.
4. `tools/build-desktop.sh` — add the id as one more word to the `for app in …`
   loop. That loop is the only thing that decides which `src/apps/*.c` files are
   compiled into the desktop and which `-DWITH_<ID>` macros exist, so an app
   missing from it is silently absent — it will not even fail to link.
5. `src/main.c` — a `LauncherItem` in `LAUNCHER[]` (id, label, icon routine),
   and/or an F-key case in the `switch (k)` that launches F1…F5. Without one of
   those, no code path ever calls `win_open(WIN_APP, "<id>")` and the app is
   unreachable.

Two rules for the drawing callback. You are given the **whole scene surface**
plus a rectangle `(x, y, w, h)`; nothing clips for you — shorten the surface to
the rect as `rect_clip()` in `src/apps/sysinfo.c` does, or every `gfx_*` call
will happily paint outside your window. And state lives in `a->state`; the same
`App` is drawn every frame, so do not re-`readdir` on every paint.

---

## 4. Your first external app, end to end

### 4.1 Write the file

Create `src/client/apps/demo.c`. The includes are relative because the SDK is
two directories up. This listing compiles clean with the packaging command below
(`zig 0.17.1`, `-Wall -Wextra`, zero warnings) and is deliberately the smallest
thing that is still a real app: a window, a live counter, a key handler, a
pointer handler.

```c
/* ============================================================
 *  Mystic OS - apps (external) / demo
 *  The smallest complete external app: a window, real pixels,
 *  a key handler and a mouse handler.
 * ============================================================ */
#include "../mystic_client.h"
#include "../../mystic.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* three colours, cycled with the space bar */
static const u32 PALETTE[3] = {
    0xFF7AA2F7u, 0xFF4FD1C5u, 0xFFE0AF68u
};

static void draw(MClient *c, Font *title, Font *small, int frames, int pick) {
    Surface s = { c->px, c->w, c->h, c->stride };

    gfx_gradient(&s, 0, 0, s.w, s.h, RGB(0x0D, 0x14, 0x22), RGB(0x1B, 0x26, 0x3A));
    gfx_rrect_aa(&s, 16, 40, (float)s.w - 32, 64, 12, PALETTE[pick],
                 RGB(0xE8, 0xEC, 0xF6), 1.0f);

    font_draw(&s, title, 28, 56, "Hello from another process", RGB(0x0D, 0x14, 0x22));

    char buf[64];
    snprintf(buf, sizeof(buf), "pid %d   frames %d   space: colour   q: quit",
             (int)getpid(), frames);
    font_draw(&s, small, 16, (float)s.h - 24, buf, RGB(0x8A, 0x93, 0xA6));
}

int main(void) {
    MClient c;
    if (mc_open(&c, "demo", "Demo", 320, 200) < 0) {
        fprintf(stderr, "demo: no window server on %s\n",
                getenv("MYSTIC_SOCKET") ? getenv("MYSTIC_SOCKET") : "/run/mystic/wm.sock");
        return 1;
    }

    /* the fonts live in the rootfs; without them there is nothing to draw */
    font_init("/usr/share/fonts");
    Font *title = font_get(FONT_SEMIBOLD, 16);
    Font *small = font_get(FONT_REGULAR, 10);
    if (!title || !small) {
        fprintf(stderr, "demo: no fonts in /usr/share/fonts\n");
        mc_close(&c);
        return 1;
    }

    int frames = 0, pick = 0;
    draw(&c, title, small, frames, pick);
    mc_present(&c);

    for (;;) {
        MEvent ev;
        if (!mc_poll(&c, &ev)) {          /* no event: sleep, do not spin */
            usleep(30 * 1000);
            continue;
        }

        if (ev.type == MC_CLOSE) { mc_close(&c); return 0; }

        if (ev.type == MC_KEY && ev.press) {
            if (ev.key == 16) { mc_close(&c); return 0; }        /* q */
            if (ev.key == 57) { pick = (pick + 1) % 3; }         /* space */
            frames++;
            draw(&c, title, small, frames, pick);
            mc_present(&c);
        } else if (ev.type == MC_MOUSE && (ev.buttons & 1)) {
            /* a dot where the pointer is held down, in window coordinates */
            Surface s = { c.px, c.w, c.h, c.stride };
            gfx_rrect_aa(&s, (float)ev.x - 6, (float)ev.y - 6, 12, 12, 6,
                         RGB(0xE8, 0xEC, 0xF6), 0, 0);
            mc_present(&c);
        }
    }
}
```

What each part is doing, in the SDK's own terms (`src/client/mystic_client.h`):

- `mc_open(&c, id, title, w, h)` connects to `$MYSTIC_SOCKET` (the launcher sets
  it to `/run/mystic/wm.sock`), sends `HELLO`, mmaps the shared buffer and fills
  `c.px`, `c.w`, `c.h`, `c.stride`. `id` is a short name, `title` is the window
  title (the manifest's `name` is only the launcher label — they are independent).
  The server clamps size to 80…1600 × 60…1200 (`src/wmsrv.c:114`).
- `Surface s = { c.px, c.w, c.h, c.stride }` — field order is
  `{ px, w, h, stride }` (the `Surface` in `src/mystic.h`). Use `c->stride`,
  never assume it equals the width; the protocol gives it to you in `OK` for
  that reason.
- `mc_present(&c)` writes `FRAME\n`; until you do, the server composites
  nothing. Draw first, then present.
- `mc_poll(&c, &ev)` returns 1 per queued event, 0 when the socket is empty. The
  socket is non-blocking, so the loop must sleep (`usleep`) rather than spin.
- `ev.key` is a raw Linux `input-event-codes.h` value, not Unicode: `16` = Q,
  `57` = space. Printable text arrives separately as `MC_TEXT` in `ev.text`.
- `MC_MOUSE` carries window-relative `x`, `y` and a button mask (1 left, 2 right,
  4 middle). Motion is forwarded while the pointer is inside your content rect,
  even when another window has focus (the pointer-forwarding block in
  `src/main.c`). Keys and text are forwarded only to the focused window, so
  click your window before wondering why typing does nothing.
- `MC_CLOSE` means the user pressed your red dot or the socket closed. Call
  `mc_close(&c)` and return — do not keep drawing into a window that is gone.
- `MC_RESIZE` arrives when the user drags the window's bottom-right grip, but
  **nothing grows your buffer**: the server only sends the message, `mc_poll`
  does not re-map the shared memory, and the compositor copies
  `min(window, your w × h)`. So a window dragged (or double-clicked) bigger than
  your `HELLO` size just has an unpainted area — it does not crash and it does
  not scale. Ignore `MC_RESIZE`, or redraw within your original rectangle; do
  not treat `ev.w`/`ev.h` as the new size of `c.px`.

### 4.2 Build and package

```bash
./tools/mkapp.sh demo
```

That is the whole build. What it does (`tools/mkapp.sh`), so you can debug it
when it says no:

1. Requires `src/client/apps/demo.c`; refuses with `error: no src/client/apps/demo.c`.
2. Finds zig (`$ZIG`, PATH, WinGet path) and runs exactly:

   ```bash
   "$ZIG" cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
       -o build/app-demo src/client/apps/demo.c \
          src/client/mystic_client.c src/gfx.c src/font.c -lm
   ```

   `gfx.c` and `font.c` are compiled **into your binary**; there is no shared
   drawing library in the image. The result is a static ELF of roughly 120 KB —
   your app carries its own copy of the renderer, which is the price of being a
   separate process with no dependencies.
3. Creates `stage/usr/share/mystic/apps/demo/`, copies the binary to `bin`, and
   `chmod +x` it.
4. Writes `manifest` **only if it does not already exist**.

If you want the compile command without the packaging side effect, run that
`zig cc` command by hand; the output path is yours to choose.

### 4.3 The manifest

`stage/usr/share/mystic/apps/demo/manifest`, three `key = value` lines:

```
name = Demo
desc = Mystic OS application
exec = bin
```

Parsed by `wms_scan_apps()` (`src/wmsrv.c:228`): one field per line, split at the
first `=`. The key is trimmed on both sides; the value only loses **leading**
spaces. Trailing spaces are kept, because the only thing removed from the end of
the value is the `\n` itself. Unknown keys are ignored. Buffers are `name[64]`,
`desc[160]`, `exec[128]`, `id[32]` (from the directory name), and lines are read
256 bytes at a time — longer values are silently truncated. A missing `name`
falls back to the directory name; a missing `exec` falls back to `bin`.

That asymmetry has teeth. Write `name = Demo` followed by two spaces and the
tile label ends in two spaces; write `exec = bin` followed by a space and the
launcher looks for a file literally called `bin ` (with the space), which does
not exist, so `execl` fails with ENOENT and you get no window. The same applies
to a `\r` left at the end of every line by a CRLF file: only `\n` is stripped,
so `exec` becomes `bin\r`. `mkapp.sh` writes clean LF lines; a manifest edited
in a Windows editor may not — see §8.1.

`mkapp.sh` derives `name` by capitalising the first character of the id and
writes `desc = Mystic OS application` unless you pass a second argument:

```bash
./tools/mkapp.sh demo "One line about what it does"
```

Because the manifest is never overwritten, changing the text afterwards means
editing `stage/usr/share/mystic/apps/<id>/manifest` by hand.

The one rule that bites: **`exec` must name a file that is packed executable.**
`tools/mkimage.py:110` cannot copy a Windows executable bit, so it decides the
mode from the path — `init`, anything under `bin/`, `sbin/`, `usr/bin/`,
`usr/sbin/`, `usr/local/bin/`, or anything whose path ends in `/bin` is packed
`0755`; everything else is `0644`. `mkapp.sh` names the payload `bin`, so the
normal path is safe; hand-rolled bundles with a payload called `payload` are not
(it packs as `0100644` — verified by packing both variants and reading the cpio
headers back). Section 8.2 is what that looks like at runtime.

### 4.4 Boot it and find it in the launcher

```bash
./image.sh
```

Then Super, or click the eye, to open the launcher. The grid is two lists
concatenated: the built-in apps first (the `LAUNCHER[]` table of `LauncherItem`s
in `src/main.c`), then every installed bundle in id order (`qsort` by id,
`src/wmsrv.c:267`). Your tile is labelled with the manifest `name` and drawn as
the first letter of that name inside a rounded square (the bundle branch of
`draw_launcher()` in `src/main.c`); none of that comes from your code.

Verified by booting this tree and pressing Super: the built-in tiles came first,
then the installed bundles alphabetically by id, each showing the first letter of
its manifest `name`. Because the label is `name` and not the id, two bundles can
render identical tiles — the shipped external Files app is called
`Files (app)` in its manifest for exactly that reason, so its tile cannot be
confused with the built-in Files tile.

Clicking the tile forks and execs `/usr/share/mystic/apps/demo/bin`
(`wms_launch`, `src/wmsrv.c:271`) with two variables set for the child:
`MYSTIC_SOCKET=/run/mystic/wm.sock` and `MYSTIC_APP_ID=demo`. Up to 32 bundles
are listed (`WM_MAX_APPS`), and 8 clients may be connected at once (`WM_MAX_CLIENTS`).
The ninth connection is accepted and closed immediately with no reply
(`src/wmsrv.c:183`), so `mc_open` fails and no window appears.

The bundle list is scanned once, at desktop start (`wms_scan_apps()` in
`main()`). A new bundle needs a reboot to appear — which you get for free,
because adding the bundle means repacking the image anyway.

---

## 5. Testing

Start with the test that answers the real question, then use the machine checks
to prove it to someone who was not watching.

**`./image.sh run`** — boot the desktop in a window, open your app from the
launcher, use it: type, click, drag it by the title bar, close it with the red
dot. Then read the console output of the boot for `wms:` lines and for your own
`fprintf(stderr, …)`. Nothing else substitutes for this; the rest of this
section exists so that "it worked" is checkable when you are not there.

**`./tools/smoke.sh --quick`** — no QEMU. Compiles every `src/*.c` and
`src/apps/*.c` with `-Wall -Wextra -Wno-unused-parameter` and passes a file when
the compiler exits 0. Note precisely what that does and does not mean, because
it is easy to over-read: `zig cc` prints warnings and still exits 0, and smoke
sends the compiler's stderr to `/tmp/smoke.err` — a file it overwrites for every
file and only prints when a compile fails. **Warnings are therefore neither
fatal nor visible** in a passing run, and the loop does not stop at the first
one, it compiles every file regardless. The check is "this file compiles", not
"this file is clean". Read the warnings in the build output yourself, or add
`-Werror` to your own command. Note also what the list does *not* cover:
`src/client/apps/*.c`, so your external app is checked by `mkapp.sh`'s compile
step, not by this.

**`./tools/smoke.sh`** — the full run: two probe boots, one desktop boot, then
the compile checks. Every check prints `PASS` or `FAIL`, and the script exits
non-zero if anything failed. Each check is something that actually broke during
development, so the list is earned rather than invented:

| Check | What it means | Usual cause when it fails |
|---|---|---|
| qemu available / kernel image present / initramfs present | the preflight can even try | `build/cache/` empty → run `./image.sh` once; or `QEMU=` is wrong |
| disk image present | `build/disk.img` exists (created with `dd` if missing) | no `dd`, or a read-only `build/` |
| kernel reached userspace | the probe block `--- mystic probe ---` appeared | kernel panicked, or `rdinit=/init` did not find `/init` |
| kernel version reported | `uname -r` starts with `6.` | the kernel line was lost from the serial log |
| DRM card0 present / framebuffer present | `/dev/dri/card0` and `/dev/fb0` exist | GPU/fb modules missing or mismatched (§8.3) |
| module loaded: drm, bochs, virtio_blk | those names are in `/proc/modules` — proof they are actually loaded, not merely present on disk | the module did not load; check the console for `unknown symbol` |
| input: `/dev/input/event0` node exists | evdev bound a device | `evdev`/`psmouse` not loaded, or QEMU started without the default PS/2 devices |
| boot time ≤ `MAX_BOOT_SECONDS` (5) | the guest's own `/proc/uptime` | software emulation without `-accel`; raise the budget rather than pretending |
| no kernel/userspace crash | no `segfault`, `panic`, `not syncing` | the desktop or an app crashed |
| disk: ext4 mounted on `/dev/vda` | the block device, filesystem and mount all worked | `virtio_blk`/`ext4` modules missing, or no disk attached |
| boot counter increments | `/root/boots.txt` is one higher after the second boot | the disk is not persistent (mounted read-only, formatted every boot) |
| desktop opened the display | `display: WxH 32bpp` on stderr (`src/fb.c:49`) | the desktop never got `/dev/fb0` |
| desktop bound a keyboard | `input: keyboard <name>` on stderr | no event device, or the capability probe classified nothing |
| desktop did not crash | no `segfault` in the desktop boot log | your bug |
| compiles clean: `src/<file>.c` | that file compiles: the compiler exited 0. A warning passes this check, and its text lands in `/tmp/smoke.err` (overwritten per file, printed only on failure) | a hard error — syntax, unknown type, missing include |

Environment knobs, from the header of `tools/smoke.sh`: `MAX_BOOT_SECONDS` (5),
`BOOT_TIMEOUT` (60), `ACCEL_TIMEOUT` (25), `DESKTOP_WAIT` (12), `QEMU`,
`MYSTIC_ACCEL` (`whpx`, `kvm`, `tcg`, `none`). If an advertised accelerator hangs
the guest, `smoke.sh` retries once with plain emulation and says so.

**`./image.sh probe`** — the same probe boot, printed for a human. Use it when
smoke says a module or a device is missing; it prints the kernel, uptime, memory,
the real module list, DRM/fb facts out of `/sys`, the input devices, the disk
state, the boot counter and a permissions test file. A run of this tree while
writing this guide reported `kernel: 6.18.63-0-virt`, `uptime: 0.86s`,
`memory: 216 MB`, `card0: yes`, `fb0 : yes`, a 1280×800 `bochs-drmdrmfb`, two
input devices and `disk: mounted, boot counter now 26` — every one of those
numbers comes from the guest's own `/proc` and `/sys`, which is the point.

**`./image.sh shot 10`** — boot headless and screendump at 10 s into
`build/shot.png`. The captures in `docs/screenshots/` are real captures from the
running OS, and this is the same headless-screendump route; it is the only
objective evidence that your window drew what you think it drew.
`./image.sh themes` does the same three times while pressing `2` and `3`.

---

## 6. Where things live

```
image.sh                        build + boot pipeline; run | probe | shot N | themes
build.sh                        stage-1 512-byte boot sector (nasm) — not used by image.sh
run.sh                          boots the old build/mystic.img — not used by image.sh

tools/
  fetch-kernel.sh               kernel + module subtrees from Alpine linux-virt
  build-desktop.sh              cross-compile the desktop into stage/
  mkapp.sh                      build + package ONE external app into stage/
  mkimage.py                    hand-written cpio newc writer (needs no symlinks)
  ppm2png.py                    QEMU screendump -> PNG
  smoke.sh                      booted + compile verification

stage/                          the only files that are "us" in the rootfs
  init                          PID 1: mounts, modprobe, disk, exec the desktop
  etc/mystic/themes/*.theme     three themes, parsed by src/theme.c
  etc/mystic/passwd             lock-screen hash
  usr/share/mystic/apps/<id>/   installed bundles: manifest + bin
  usr/bin/mystic-desktop        the compiled desktop - a build output, ignored by git
  usr/share/fonts/*.ttf         Lato, copied from assets/
  lib/modules/<ver>/            staged kernel modules

src/
  main.c                        the desktop: layout, dock, launcher, windows, event loop
  wmsrv.c                       the window-server half of the protocol
  ipc.h                         the protocol in one header (WmClient, WmApp, WM_APP_DIR)
  mystic.h                      Surface, colours, gfx_*, font_*, AppVT, App
  gfx.c                         gradients, SDF rounded rects, box blur, shadows, lines
  font.c                        TrueType rasterising via stb_truetype
  fb.c                          /dev/fb0
  input.c                       /dev/input/event* (evdev), keymap, ordered key queue
  theme.c                       .theme parser
  term.c                        forkpty + /bin/sh + a VT100-subset parser
  sha256.c                      password hashing
  apps.c                        the AppVT registry
  apps/*.c                      in-desktop apps, one file per app; the live set is
                                REGISTRY[] in apps.c plus the LAUNCHER[] tiles in main.c
  client/
    mystic_client.h/.c          the external-app SDK: mc_open/present/poll/close
    apps/*.c                    external apps, one file per app; what ships as a bundle
                                is whatever mkapp.sh has put under
                                stage/usr/share/mystic/apps/

docs/                           ARCHITECTURE.md, IPC.md, screenshots/, this file
build/                          all generated: desktop binary, initramfs, disk, screenshots
```

`build/` is gitignored, and so is every build output inside `stage/`: the
`.gitignore` lists `stage/usr/bin/mystic-desktop`,
`stage/usr/share/mystic/apps/*/bin`, `stage/lib/modules/`,
`stage/usr/share/fonts/*.ttf`, `stage/sbin/` and `stage/usr/lib/`. Building and
booting therefore never dirties a tracked file — checked by comparing `git
status` before and after a full `./image.sh probe`: identical. `.gitattributes`
pins `*.sh`, `tools/*.py` and `stage/init` to `text eol=lf` — see §8.1 for why
that line exists.

---

## 7. The invariants this project enforces

These are the rules the tree is held to. Most are enforced by something you can
run; where a rule is a convention rather than a gate, the text says which.

1. **Nothing is "done" until it has been built and booted here.** README rule 1.
   A commit message in this repo names the boot it was seen in ("Verified: …"),
   and `tools/smoke.sh` exists to turn "it boots" into a machine-checkable fact.
   If you cannot boot it, say so instead of implying it works.
2. **No file grows by copy-paste.** README rule 2. Two apps needing the same
   helper means the helper moves into `gfx.c`, `font.c` or `apps.c` — not into
   both files. Duplicated blocks get deleted in review.
3. **Every number comes from a syscall or `/proc`.** Memory from
   `/proc/meminfo`, processes from `/proc/<pid>`, files from `readdir`/`stat`,
   resolution from `FBIOGET_VSCREENINFO`, time from `time()`, load from
   `/proc/loadavg`. No invented RAM totals, no synthetic frame rates, no
   placeholder rows in a list.
4. **No placeholders.** No `TODO` bodies, no stub handlers that draw "coming
   soon", no menu entry that does nothing. A control that exists but is not
   wired is a defect, not a plan.
5. **File modes are decided by path at pack time.** `tools/mkimage.py:110`,
   because the build runs on Windows where there is no executable bit. A program
   must be `init`, live under `bin/`-style directories, or end in `/bin`.
6. **LF line endings in anything the kernel or a shell executes.**
   `.gitattributes`; see §8.1.
7. **Zero warnings, by convention rather than by tooling.**
   `build-desktop.sh` compiles the desktop with `-Wall -Wextra
   -Wno-unused-parameter`; `smoke.sh` compiles every file in `src/` and
   `src/apps/` with the same flags. Neither passes `-Werror`, and `zig cc` exits
   0 on warnings, so a warning prints during a build and the build carries on —
   this tree has built with one (`unused function 'icon_for_window'` in
   `src/main.c`). Keep the output empty anyway: a warning is a defect you have
   not read yet.
8. **Draw inside the rectangle you were given.** The compositor copies exactly
   `w × h` from your buffer, and hands in-desktop apps the whole scene surface.

---

## 8. When it breaks: four failures this project actually had

### 8.1 CRLF in a shell script

**Symptom.** `./image.sh` dies immediately with something like
`./image.sh: line 5: $'\r': command not found`, or the guest's PID 1 does
nothing at all and the kernel reports it cannot execute `/init` — for a file
that is clearly there.

**Cause.** A Windows editor saved the file with CRLF. The first line is then
`#!/bin/sh\r`, so the kernel looks for an interpreter whose name is literally
`/bin/sh\r`, which does not exist. Once the shell is already running, every
line carries a stray `\r`.

**Fix / guard.** `.gitattributes` in this repo pins `*.sh`, `tools/*.py` and
`stage/init` to `eol=lf`, so a fresh clone is correct. If a working copy is
already wrong: `sed -i 's/\r$//' <file>`, and make sure your editor does not
re-add them. Check with `grep -c $'\r' tools/mkapp.sh` (must be 0) or
`file stage/init` (must not say "with CRLF line terminators").

### 8.2 A bundle payload that is not executable

**Symptom.** You click your app in the launcher, nothing happens, no window ever
appears. The desktop's stderr shows
`wms: cannot exec /usr/share/mystic/apps/demo/bin` (`src/wmsrv.c:283`). When
this first bit, the exec failed in a way that read as a silent `ENOENT`
(commit `2a185da`, which added the rule below).

**Cause.** `tools/mkimage.py` marks a file `0755` only when its path is `init`,
starts with `bin/`, `sbin/`, `usr/bin/`, `usr/sbin/`, `usr/local/bin/`, or ends
with `/bin`. A payload with any other name is packed `0644`, and the kernel will
not execute a non-executable file.

**Fix.** Keep the payload called `bin` — `mkapp.sh` does — or rename it so the
path ends in `/bin`. If you hand-write a bundle, verify the packed mode, not the
mode on your Windows disk (which has no such bit): build the cpio and read the
entry back, or boot and `ls -l /usr/share/mystic/apps/<id>/`.

### 8.3 A missing kernel module ("unknown symbol")

**Symptom.** The console shows something like
`modprobe: can't load module drm: unknown symbol in module, or unknown parameter`
(or whichever module it is), and then either `/dev/fb0` never appears — so
`stage/init` never starts the desktop and drops you to a shell with the banner —
or `/dev/input/event0` never appears, so the desktop runs with no keyboard or
mouse.

**Cause.** Alpine's `linux-virt` builds the graphics, framebuffer, input and
filesystem drivers as loadable modules, and `stage/init`'s `load_modules()`
(line 32) modprobes an explicit list by name. `tools/fetch-kernel.sh` stages
whole module subtrees rather than a hand-picked list, for a stated reason: "a
cherry-picked list means chasing `unknown symbol` errors one build at a time"
(`tools/fetch-kernel.sh:23`). A module that is missing, or whose version does
not match the kernel, cannot load — which is also why the kernel and its modules
are taken from the same package (`Invalid module format` is the other half of
this failure).

**Fix.** Add the module name to the `modprobe` list in `stage/init`, and its
subtree to `MODULE_TREES` in `tools/fetch-kernel.sh` if it is not already
staged. Note that `image.sh` only calls `fetch-kernel.sh` when the kernel binary
or the unpacked module package is missing, so after editing `MODULE_TREES` run
`./tools/fetch-kernel.sh` explicitly. Then check `./image.sh probe`: the `modules :` line is
`/proc/modules`, i.e. what actually loaded — more names than the list, because
dependencies travel with their dependants (26 on a probe boot of this tree).
`card0: yes` and `fb0 : yes` are the two facts the desktop needs.

### 8.4 devpts not mounted — forkpty fails

**Symptom.** The desktop starts and everything else works, but the terminal
window is dead: no prompt, no shell, nothing typed ever comes back.

**Cause.** `term.c` opens a pty with `forkpty()`. That needs the `devpts`
filesystem mounted on `/dev/pts`; without it the slave side cannot be opened and
no shell runs. `stage/init:18` mounts it (`mount -t devpts none /dev/pts`) with
the comment "without devpts forkpty() cannot open the slave side"; it was added
in the same commit as the terminal (`b4b8e83`).

**Check.** Open Terminal with `F1` — it is the only app that needs a pty, so it
is the exact test. From a shell, `ls /dev/pts` shows whether the mount is there.

### Debugging your own app, in order

| What you see | What it means |
|---|---|
| no window at all, `demo: no window server` | `mc_open` failed: the socket path is wrong, or the desktop's `wms_start` failed |
| `wms: cannot exec …` | §8.2, or the manifest's `exec` names a file that is not in the bundle |
| window appears, all black | you never called `mc_present`, or you drew before `mc_open` returned |
| window draws once, then freezes | you are blocking in the loop (no `usleep`, or a `read` on a blocking fd) |
| rows skewed or sheared | the `Surface` was built with the wrong stride; use `c->stride`, field order `{ px, w, h, stride }` |
| crash after N frames | you wrote outside `c->w * c->h`; nothing clips your buffer for you |
| keys do nothing | the window is not focused — click it; keys go to the focused window only |
| mouse does nothing | the pointer is outside the content rect, or you ignored `ev.buttons & 1` |
| two identical tiles in the launcher | two bundle directories under `WM_APP_DIR`, or two manifests with the same `name` — tiles are drawn from `name`, not from the id |

Your app inherits the desktop's stderr — the console PID 1 was started on — so
`fprintf(stderr, …)` is the debugger, and printing `getpid()` is a cheap way to
prove the app is a separate process, which is the entire point of the SDK.

---

## 9. Before you say it works

- [ ] `./tools/mkapp.sh <id>` succeeds, and `stage/usr/share/mystic/apps/<id>/`
      contains `manifest` and `bin`.
- [ ] The build printed no warnings. Look at the `./image.sh` output itself:
      `zig` prints them there and `smoke.sh` hides them, so no script will fail
      for you.
- [ ] `./image.sh` boots, the app appears in the launcher (Super), and clicking
      it opens a window that draws.
- [ ] Keys and clicks do what you say they do; closing the window makes the
      process exit — check there is no repeating error on the console afterwards.
- [ ] `./image.sh shot 10` (with your app opened in the boot, if you automate it)
      or a phone photo of the QEMU window proves the pixels.
- [ ] `./tools/smoke.sh` passes, not just `--quick`.
- [ ] No placeholder text, no hardcoded machine fact, no claim about a boot you
      did not do.
- [ ] If you touched the protocol, `docs/IPC.md` is updated; if you changed how
      the desktop works, `docs/ARCHITECTURE.md` is updated. Both are written from
      the code, with real symbol names — keep them that way.
