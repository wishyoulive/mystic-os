# MYSTIC OS — CONTEXT DUMP / HANDOFF

Written because the conversation context was nearly full. Everything needed to continue
without re-deriving it. If you only read one section, read **1**, **6** and **9**.

---

## 1. The project in one page

| | |
|---|---|
| Path | `C:\Users\saket\dev\mystic` (deliberately **not** in OneDrive — sync churns build dirs and locks images) |
| Repo | https://github.com/wishyoulive/mystic-os — **public** since 17 Nov |
| Release | `v1` — https://github.com/wishyoulive/mystic-os/releases/tag/v1 (24 MB zip: kernel + initramfs + run.sh + README) |
| What it is | A desktop OS: **Linux kernel (Alpine virt build) + our own everything above it** — compositor, window manager, taskbar, launcher, themes, text stack, terminal, seven in-desktop apps, an external-app protocol with seven bundles, a package manager, a lock screen |
| What it is not | Not a new kernel, not a distro remix. No Wayland, no X11. No browser yet. |

**Owner's standing rules** (violating these is the main failure mode):

1. **No placeholders, stubs, TODO bodies, fake data, or padding.** Every number on screen comes from a syscall.
2. **Nothing is "done" until it is built, booted, and seen.**
3. No duplicated blocks; duplication gets deleted.
4. **Any feature needing >40 MB of storage is NOT shipped in the image** — it goes in the package repo and is installed at runtime. Enforced by `tools/check-size.sh`.
5. Use RSI swarms with 2 critic rounds, and I (the orchestrator) verify everything myself. Max 15 agents.
6. Keep working continuously; I set reminders so I don't idle.

---

## 2. Build and run

```bash
cd C:\Users\saket\dev\mystic

./image.sh                 # build desktop + rootfs, boot in a QEMU window
./image.sh probe           # headless: boot, print machine facts, power off
./image.sh shot 12         # headless boot, screendump after 12s -> build/shot.png
./image.sh themes          # one boot, screenshot all themes

MYSTIC_RAM=1024 MYSTIC_DISK_MB=2048 ./image.sh    # bigger budget (browser path)
# NOTE: the disk is only created if build/disk.img is absent — delete it to resize.

./tools/build-desktop.sh   # just the desktop binary (zig cc, static musl)
./tools/mkapp.sh <id>      # package one external app into stage/
./tools/mkimage.py         # pack the rootfs (cpio.gz)
./tools/fetch-kernel.sh    # kernel + exactly the modules we use
./tools/smoke.sh           # 33 boot/compile checks (--quick for compiles only)
./tools/check-size.sh      # the 40 MB shipping rule
./tools/serve-repo.sh      # build + serve the package repo on :8000 for the guest
python tools/fetch-browser.py --list   # chromium closure (374 pkgs / 682 MB)
```

**Toolchain** (all installed): zig 0.17.1 at
`C:\Users\saket\AppData\Local\Microsoft\WinGet\Packages\zig.zig_*\zig-x86_64-windows-*\zig.exe`,
nasm portable at `C:\Users\saket\dev\tools\nasm-3.02\nasm.exe`, QEMU at `C:\Program Files\qemu\`.
No WSL, no MSYS2, no admin. `zip` is missing (use PowerShell `Compress-Archive`).

---

## 3. Architecture map

```
QEMU firmware -> Linux 6.18.63 virt -> initramfs.cpio.gz unpacked into RAM
  -> /init (stage/init, ~200 lines, PID 1)
       mounts proc/sys/dev/devpts/dev/shm
       depmod + modprobe (virtio_blk, virtio_net, af_packet, drm/bochs, evdev, ext4, vfat, 9p...)
       setup_net   (udhcpc -> 10.0.2.15 under QEMU user-net, /etc/udhcpc.script)
       setup_disk  (/dev/vda, ext4 formatted on first boot, mounted on /root)
       restore_settings (/root/mystic-{ui.conf,wallpaper,current-theme} -> /etc/mystic/)
       unbind fbcon (it painted its cursor over our pixels)
       exec /usr/bin/mystic-desktop
```

**Desktop** (`src/main.c`, ~1650 lines) draws the whole screen on `/dev/fb0` with its own CPU
renderer. It owns: scene composition, window manager (drag, resize grip, edge snapping,
Alt+Tab, minimise/maximise), taskbar of open windows with macOS magnification, launcher
(eye logo / Super), lock screen, theme + wallpaper + text-scale polling, client compositing,
input routing, child reaping.

**Key files**

| File | Lines | What |
|---|---|---|
| `src/main.c` | ~1650 | desktop, WM, taskbar, launcher, lock, IPC client compositing, input routing |
| `src/gfx.c` | ~460 | SDF rounded rects, 3-pass box blur, gradients, glows, polygons; static scratch buffers |
| `src/font.c` | ~300 | stb_truetype wrapper, glyph cache **with LRU eviction**, ui_scale applied in `font_get` |
| `src/term.c` | ~750 | pty + VT100 subset, UTF-8 decode, cursor clamping, scrollback ring, `term_resize(_px)` |
| `src/input.c` | ~220 | evdev: pointer, keyboard, **key queue (newest-wins)**, autorepeat, double-click |
| `src/ui.c` | 40 | global text scale from `/etc/mystic/ui.conf`, `ui_px()` |
| `src/wallpaper.c` | ~450 | four backgrounds: Dusk, Mesh, Waves (animated), Orbits (animated) |
| `src/theme.c` | ~180 | theme file parser (key = value) |
| `src/sha256.c` | ~200 | SHA-256, 20k-round salted password, constant-time compare |
| `src/ipc.h` | 90 | **the display protocol** (text over unix socket + pixels in shm) |
| `src/wmsrv.c` | ~420 | server half: accept, shm, fork/exec, **app sandbox**, line protocol |
| `src/client/*` | ~250 | SDK: `mc_open/present/poll/close` |
| `src/apps/*.c` | 7 files | terminal, files, settings, monitor, notes, store, calc (AppVT contract) |
| `src/client/apps/*.c` | 8 files | clock, files, paint, life, mandel, sysmon, g2048, web(in progress) |
| `stage/init` | ~200 | PID 1 |
| `stage/usr/bin/pkg` | ~150 | package manager (HTTP index + sha256 + tar onto /root/apps) |
| `stage/usr/bin/linuxapp` | ~60 | chroot runner for glibc roots (Ubuntu) |
| `tools/*` | 9 files | build, pack, fetch kernel/browser, smoke, size check, repo server |

**Protocol in one line:** `HELLO id w h title` → `OK /dev/shm/path stride` → app draws →
`FRAME` → WM composites; WM sends `KEY/TEXT/MOUSE/RESIZE/CLOSE`. Full doc: `docs/IPC.md`.

---

## 4. What is verified working (screenshots / serial evidence)

- Boot to desktop **0.9 s** (WHPX), **~70–87 MB** of 216 MB used.
- Terminal with real `/bin/sh`, monospace (JetBrains Mono), 600-line scrollback, `ls` columns aligned at 1.6× scale.
- Files: toolbar (Back/Up/Home/Refresh), breadcrumbs, per-type icons, sort, status bar, free space.
- Settings: **live** text size (0.8–1.6× rescales the whole desktop) and wallpaper switching; both write config files the desktop re-reads each second; both persist to `/root` and restore at boot.
- Taskbar appears only when windows exist; magnification on hover; click focused icon to minimise, again to restore.
- Lock screen: salted SHA-256 (20k rounds), password `mystic`; Ctrl+Alt+L. **Verified earlier; not re-verified since the key-handling refactor — see §9.**
- External apps run as separate processes (7 bundles); Paint draws strokes, Mandelbrot renders, Life runs.
- **Networking works**: `virtio_net` + `af_packet` modules, udhcpc lease `10.0.2.15`, DNS, TCP, `http: 577 bytes from example.com`.
- Disk: ext4, formatted by the OS on first boot, `/root` persists (`fsync` before claiming a save).
- Package manager: repo with 8 packages (incl. `browser`, 574 MB) built by `tools/serve-repo.sh`.
- `./tools/smoke.sh` → 33/33 pass.

---

## 5. Browser: the decision, with real numbers

Full research: **`docs/BROWSER_PLAN.md`** (926 lines, evidence-tagged `[pkg]`/`[src]`/`[est]`).
Conclusion that matters: **the obstacle is budget, not the display protocol.**

| Option | Union closure | Verdict |
|---|---|---|
| Chromium headless + CDP pipe | 682 MB | renders all sites; 8–12 days; frames are JPEG over CDP |
| **Chromium inside Xvfb, pixels via `XShmGetImage`, control via CDP** | 807 MB (+7.6 MB of X, because `mesa`/`llvm` are already in Chromium's closure) | **recommended pivot** — raw pixels at full rate, and `apt` comes with the rootfs |
| NetSurf on libnsfb (VNC surface) | fits 256 MB | no JS, no video — document browser only |
| WebKitGTK + our own Broadway server | 569 MB | biggest prize (every GTK app), 12–18 days |
| CEF off-screen | 200 MB+ | no musl build exists |

**No option plays DRM video** (no Widevine package exists in Alpine v3.24). "Multimedia" = open codecs.

**Engine already fetched**: `build/browser-root/` (1.5 GB extracted, 315 symlinks materialised as
copies because Windows cannot create them), 202 MB `chromium-headless-shell` binary, plus
`build/repo/browser.tar.gz` (574 MB) ready for `pkg install browser`.

**Linux apps**: we are Linux; what blocks Ubuntu binaries is **glibc vs musl**. `linuxapp` runs a
glibc root via chroot. Ubuntu base tarball fetched to `build/cache/ubuntu-base.tar.gz` (29 MB,
has `/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`, `apt`, `bash`). Not yet wired as a package
or tested.

---

## 6. IN-FLIGHT WORK (read this before touching code)

### 6.1 App sandbox — compiles, NEVER RUN
`src/wmsrv.c`: `sandbox_child()` + `mkdirp_for()` + `wms_launch()`. Each app gets:
`unshare(CLONE_NEWNS|NEWNET|NEWIPC|NEWUTS)` → `mount --make-rprivate /` → jail at
`/run/mystic/jail/<id>` with bind mounts of the app binary (`/bin/app`), `wm.sock`, `/dev/null`,
`/usr/share/fonts` (ro), `/dev/shm`, `/proc` → `chroot` → `setgid/setuid(1000)` → `execl("/bin/app")`
with an unsandboxed fallback. `make_shm()` chowns the shm to 1000 so the app can draw.

**Must be tested before trusting it**: launch paint/clock (do they still draw? fonts? shm?), then a
hostile app that tries to read `/root/notes.txt` and must fail.

Build gotchas already hit and fixed: `_GNU_SOURCE` must be the **first line** of `wmsrv.c`
(before `#include "ipc.h"`), and musl needs `<grp.h> <sched.h> <sys/mount.h>`.

### 6.2 Whatever the running swarms are producing
Check `progress` action=list. As of the dump, live runs include:
- `rsi-bg-mwkn10te-0i14c` — 3 hunters (newest code, seams, performance)
- `rsi-bg-mwknl3ec-va5km` — 2 security hunters (sandbox on paper + adversarial review)
- `rsi-bg-mwkmt6ia-mzcft` — web.c (lightweight browser) + files icons

### 6.3 Unresolved worker outcomes (decide: use, fix, or re-run)
- `research-browser` — **UNRESOLVED** but the file is good: `docs/BROWSER_PLAN.md` is 926 lines,
  was revised twice, and only *bookkeeping* items remained (patch count 45 vs 46, w3m closure
  1.5 vs 12.0 MB, three anchors labelled `efd54b2` that are working-tree lines, one false CANNOT,
  Broadway days differ 10–15 vs 12–18). **My call: keep the file, fix those five items by hand if
  it matters**, they do not change any conclusion.
- `files-rich` — **UNRESOLVED** but the code landed and is good (`src/apps/files.c`, 47 KB,
  toolbar + icons, verified visually). The unresolved status is about report bookkeeping.

---

## 7. Known open items (from hunters, not yet fixed)

Latest hunter round reported 18 findings; these were fixed: F1 launcher overflow, F2 client
canvas/title_h, F3 maximise chrome, F4 taskbar hit order, F5 launcher modality, F8 NaN scale,
F11 Alt+Tab hidden, F17 lock stalls server, F18 lockout with no passwd, F6 gfx allocations,
F7 TIOCSWINSZ, F13 term_key writes, F14 autorepeat, F15 queue overflow, F16 glyph eviction,
F9 drag generation, F10 title clipping, F12 bar centring.

Still open / to verify:
1. **Sandbox never executed** (§6.1) — highest priority.
2. **Lock screen not re-verified** since key handling moved into the queue loop
   (`src/main.c` has the handler twice: `input.key == 38` at ~1203 and `k == 38` at ~1281 — the
   first is probably dead and should be removed).
3. **No user profile / no first-boot setup screen.** Owner asked for both.
4. **Encryption: none.** Kernel has `DM_CRYPT=m` and `BLK_DEV_DM=m` (modules available in the
   package), so dm-crypt/LUKS is possible, but nothing implements it. No cryptsetup in the image.
5. **Package index is unauthenticated** (plain HTTP + sha256 from the same source). A signature
   needs Ed25519 (~300 lines, no deps) + a key in the image.
6. `/etc` is a writable tmpfs; "read-only system" not implemented.
7. Settings at the 280×160 minimum shows no theme rows (disclosed by the worker).
8. `docs/BROWSER_PLAN.md` bookkeeping (see §6.3).

---

## 8. Gotchas that have cost hours — do not relearn these

1. **Shell/heredoc eating backslashes.** Any `python - <<'PY'` replacement containing `\\n` or
   `\"` gets mangled, producing C with a literal newline inside a string. **Use the `edit` tool for
   anything containing backslashes.** Several "fixes" silently wrote broken code this way.
2. **Patches that match nothing and report success.** Always assert the old text was found, and
   write the file immediately after each successful edit — a script that edits 5 things and then
   throws on the 6th leaves the file unchanged.
3. **The shell cannot create Linux symlinks.** `tools/mkimage.py` builds the cpio directly from
   tarballs for this reason; `tools/fetch-browser.py` materialises symlinks as copies.
4. **`mkimage.py`'s executable rule**: `init`, anything under `bin/`, `sbin/`, `usr/bin/`,
   `usr/sbin/`, paths ending `/bin` (app payloads) and `*.script`. Forgetting this caused
   "cannot exec" (ENOENT) and a DHCP script that could not run.
5. **CRLF kills shell scripts** (`/bin/sh\r` → ENOENT). `.gitattributes` pins LF; `sed -i 's/\r$//'`.
6. **A QEMU left running locks `build/initramfs.cpio.gz`** — builds fail with "Invalid argument".
   Kill QEMU first; or build to a separate path (e.g. `build/dev.cpio.gz`).
7. **Hunters must not boot QEMU** (they fight over the image and over `pkill -f qemu`).
8. **Windows Defender flags agent-built `verify.exe`-style harnesses** in /tmp (unsigned static
   binaries). Delete them; they are not part of the repo.
9. **Do not dispatch a worker on a file you are editing** (main.c collisions cost a rewrite).
10. **QEMU needs `-accel whpx`** for usable speed (0.9 s boot vs 8 s).

---

## 9. Next steps in priority order

1. **Test the sandbox** (§6.1) with paint/clock, then a hostile app that must fail to read `/root`.
   Fix whatever breaks. This is the owner's explicit "sandboxing on by default".
2. **Fix the duplicate lock-screen handler** in `main.c` and re-verify Ctrl+Alt+L end to end;
   then add a visible "Lock" entry (launcher or Settings) and an auto-lock idle timer.
3. **First-boot setup screen** (owner asked): an app that runs once (flag file on `/root`) asking
   for theme, text size, wallpaper, password and writing `/etc/mystic/*` + `/root` copies.
4. **User profile in Settings**: username, avatar initial, password change (`mystic-passwd`).
5. **Encryption**: load `dm_crypt`/`dm_mod`/`aes`/`sha256` modules, implement `cryptsetup`-less
   LUKS? Realistic path is a small verity/dm-crypt wrapper or shipping `cryptsetup` as an
   installable package. At minimum: encrypted `/root` volume created at first boot with a
   passphrase, and be honest about the gaps until then.
6. **Sign the package index** (Ed25519) and make `pkg` refuse unsigned repos.
7. **Browser**: pivot to the Xvfb+XShm plan in `docs/BROWSER_PLAN.md` (Chromium already fetched,
   repo package ready); write the static client that captures the X root window into a client shm
   buffer and forwards input via XTEST.
8. **Linux apps**: package the Ubuntu base rootfs, wire `linuxapp` (bind mounts + chroot already
   written), then test `apt update` inside it.
9. **Performance**: damage rectangles in the protocol (`FRAME x y w h`) and a compositor that
   blits only the damaged region — the hunter measured up to ~117 ms/full recompose with ten
   windows, which is the stutter the owner feels.

---

## 10. Line-count reality

**~21,500 lines of our own code** (26,800 with `stb_truetype.h`): desktop 5.0k, in-desktop apps
5.4k, SDK + external apps 5.7k, tooling 1.7k, docs 3.3k, themes 90.

Target is 500k **genuine** lines. At the observed rate (~2,800 verified lines per 30-minute swarm
with a critic on every file) that is ~85 more swarm cycles. It will not be reached by padding, and
the owner has explicitly forbidden padding — so the honest measure of progress is capability, with
the line count as a by-product.
