# MYSTIC OS

A small, fast desktop operating system for old PCs.
Runs Linux apps natively; Windows through Wine is the next step.

Not a distro remix: the desktop is ours — our compositor, our renderer, our
window manager, our themes, our browser, our package format, our sandbox.
Everything below was built and booted on this machine.

![Obsidian](docs/screenshots/obsidian.png)

## What works today

A real desktop, drawn pixel by pixel by our own compositor on a bare
framebuffer. No X11, no Wayland, no GPU driver.

| | |
|---|---|
| Boot | 0.9 s to the desktop (hardware accelerated in a VM) |
| Disk | virtio disk, ext4, formatted on first boot; `/root` and installed apps persist |
| Memory | ~175 MB used of 1 GB, including kernel + rootfs |
| Image | 12.8 MB rootfs + 13 MB kernel (everything else installs at runtime) |
| Graphics | bochs/simpledrm framebuffer, 1280×800, 32bpp |
| Text | real TrueType rasterising (Lato + JetBrains Mono), antialiased, one global scale |
| Themes | six, switched live; four wallpapers, two animated |
| Input | evdev keyboard + mouse, wheel, drag / resize / snap, Alt+Tab, Super launcher |
| Apps | Terminal (real pty), Files, Settings, Monitor, Notes, Apps store, Calculator |
| App bundles | Clock, Files, Paint, Life, Mandelbrot, Sysmon, 2048, Browser |
| Browser | our own HTTP client and HTML renderer: real pages over plain HTTP |
| Packages | `pkg` + the Apps store: install from a repository at runtime, sha256-checked |

Press `Super` (or Esc) for the launcher. `1`–`6` switch theme.
Ctrl+Alt+L locks.

### The browser

`src/client/apps/web.c` is a browser written here: its own HTTP client
(DNS, TCP, Content-Length and chunked bodies, redirects), its own HTML
tokeniser and text renderer (headings, lists, numbered links, entities,
metric wrapping) and its own scrollback UI. It renders
`http://example.com/` as `HTTP 200 · 577 bytes · 65 ms`.

It is honest about its limits: plain HTTP only, no JavaScript, no CSS, no
images. A real engine is an installable package, not part of the image —
see `docs/BROWSER_PLAN.md`.

### Security model

Apps are separate processes, launched into a jail: `unshare` of mount, IPC,
UTS and network namespaces, a chroot, and `setuid(1000)`. The jail contains
only the app's own binary, the window socket, `/dev/null`, `/dev/urandom`,
the fonts (read-only) and a private `/dev/shm`. All inherited descriptors are
closed, so a jailed app cannot see the framebuffer or the keyboard. An app
that needs the network must say `network = host` in its manifest — the
Browser does, the games do not.

Privileged actions are request files the desktop polls, so a sandboxed app
never needs to fork: `/run/mystic/{launch,install}.request`, `repo.refresh`,
`current-theme`.

## Build and run

```bash
./image.sh            # build and boot the desktop in a window
./image.sh probe      # boot headless, print machine facts, power off
./image.sh shot 10    # boot headless, screenshot after 10 s
./image.sh themes     # boot once, screenshot the themes
./tools/serve-repo.sh # serve the package repository to the VM
```

Nothing needs to be installed except QEMU and zig — the builder fetches the
kernel and the base rootfs itself.

## Layout

```
src/              the desktop: C, no dependencies
  main.c          the desktop itself: layout, dock, windows, event loop
  wmsrv.c         window server + app sandbox
  gfx.c           CPU drawing: gradients, rounded rects, real blur, polygons
  font.c          TrueType rasterising via stb_truetype, LRU glyph cache
  fb.c            /dev/fb0, full-frame and rectangle present
  input.c         /dev/input/event* (evdev)
  term.c          our terminal: pty, grid, scrollback, TIOCSWINSZ
  apps/           in-desktop apps (files, settings, monitor, notes, store, calc)
  client/         the SDK, the protocol, and the out-of-process apps
stage/            our userspace - the only files that are "us"
  init            PID 1
  usr/bin/pkg     the package manager
  etc/mystic/     themes, ui.conf, repo.conf
tools/            fetch-kernel, build-desktop, mkapp, mkimage, serve-repo, release
docs/             CONTEXT-DUMP.md is the state of the world; read that first
```

## Design targets

| | Target | Now |
|---|---|---|
| Idle RAM | under 128 MB | ~62 MB idle, ~175 MB with the desktop and a browser |
| Boot to desktop | under 2 s | 0.9 s in a VM |
| Apps | Linux native, Windows via Wine | Linux native yes, Wine in progress |
| Security | sandbox, signed packages, disk encryption | sandbox yes; signing and encryption next |

## Rules this project keeps

1. Nothing is "done" until it has been built and booted here.
2. No file grows by copy-paste. Duplication gets deleted.
3. Claim nothing that was not measured on this machine.
4. No placeholder assets. The fonts are real fonts, the numbers are real
   numbers read from `/proc`, the screenshots are real screenshots.
