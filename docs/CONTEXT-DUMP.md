# MYSTIC OS — CONTEXT DUMP / HANDOFF

**Last updated after: app sandbox made real, our own browser written, package manager + Store install path, six themes.**
If you only read three sections, read **1**, **6**, **9**.

---

## 1. The project in one page

| | |
|---|---|
| Path | `C:\Users\saket\dev\mystic` (deliberately **not** in OneDrive) |
| Repo | https://github.com/wishyoulive/mystic-os — **public** |
| Release | `v1` — https://github.com/wishyoulive/mystic-os/releases/tag/v1 (24 MB: kernel + initramfs + run.sh + README) |
| Ship state | **~27,150 lines of our own code**, 34 commits, image = 13 MB kernel + 12.5 MB initramfs |
| What it is | Linux kernel (Alpine virt) + **our entire userspace**: compositor, WM, taskbar, launcher, 6 themes, text stack, terminal, 7 in-desktop apps, an external-app protocol with 8 bundles, own browser, package manager, lock screen, **app sandbox** |

**Owner's standing rules**
1. **No placeholders, stubs, TODO bodies, fake data, or padding.**
2. Nothing is "done" until built, booted and seen.
3. No duplicated blocks.
4. **Nothing >40 MB ships in the image** — it goes in the package repo and is installed at runtime. Enforced by `tools/check-size.sh`.
5. RSI swarms with 2 critic rounds; I verify everything myself; max 15 agents.
6. Keep working continuously.

---

## 2. Build and run

```bash
cd C:\Users\saket\dev\mystic
./image.sh                 # build + boot in a QEMU window
./image.sh probe           # headless facts, then power off
./image.sh shot 12         # headless, screendump -> build/shot.png
MYSTIC_RAM=1024 MYSTIC_DISK_MB=2048 ./image.sh   # browser budget (delete build/disk.img to resize)
./tools/build-desktop.sh   # desktop only
./tools/mkapp.sh <id>      # package one external app
./tools/mkimage.py         # pack rootfs
./tools/smoke.sh           # 33 checks
./tools/check-size.sh      # the 40 MB rule
./tools/serve-repo.sh      # package repo on :8000 (guest fetches 10.0.2.2:8000)
python tools/fetch-browser.py --list        # chromium closure (374 pkgs / 682 MB)
```

Toolchain: zig 0.17.1 (`%LOCALAPPDATA%\Microsoft\WinGet\Packages\zig.zig_*\...\zig.exe`),
nasm (`C:\Users\saket\dev\tools\nasm-3.02\nasm.exe`), QEMU (`C:\Program Files\qemu\`).
No WSL, no MSYS2, **no `zip`** (use PowerShell `Compress-Archive`).

---

## 3. Architecture

```
firmware -> Linux 6.18.63 -> initramfs in RAM -> /init (PID 1)
   mounts proc/sys/dev/devpts/dev/shm
   depmod + modprobe (virtio_blk, virtio_net, af_packet, drm/bochs, evdev, ext4, vfat)
   setup_net   (udhcpc -> 10.0.2.15, /etc/udhcpc.script)
   setup_disk  (/dev/vda ext4, formatted on first boot, mounted /root)
   restore_settings (/root/mystic-* -> /etc/mystic/)
   unbind fbcon
   exec /usr/bin/mystic-desktop
```

**Desktop** (`src/main.c`) draws everything on `/dev/fb0` with its own CPU renderer; owns
composition, WM, taskbar (magnification), launcher (eye/Super), lock screen, theme/wallpaper/
text-scale polling, client compositing, input routing, child reaping, and the two
request-file handlers (`install.request`, `repo.refresh`).

**File map** (line counts)

| File | Lines | What |
|---|---|---|
| `src/main.c` | ~1750 | desktop, WM, taskbar, launcher, lock, IPC compositing, pkg requests |
| `src/gfx.c` | ~460 | SDF shapes, 3-pass blur, glows; static scratch buffers |
| `src/font.c` | ~300 | stb_truetype, glyph cache **with eviction**, ui_scale in `font_get` |
| `src/term.c` | ~760 | pty, VT100 subset, UTF-8, cursor clamps, scrollback, `term_resize(_px)` |
| `src/input.c` | ~220 | evdev, key queue (newest-wins), autorepeat, double-click |
| `src/ui.c` | 40 | text scale from `/etc/mystic/ui.conf` |
| `src/wallpaper.c` | ~450 | Dusk / Mesh / Waves(anim) / Orbits(anim) |
| `src/sha256.c` | ~200 | SHA-256, 20k-round salted password, constant-time compare |
| `src/ipc.h` | 90 | the display protocol |
| `src/wmsrv.c` | ~470 | server: accept, shm, **app sandbox**, fork/exec, line protocol |
| `src/client/*` | ~250 | SDK |
| `src/apps/*.c` | 7 | terminal, files, settings, monitor, notes, store, calc |
| `src/client/apps/*.c` | 9 | clock, files, paint, life, mandel, sysmon, g2048, **web**, (+host harnesses) |
| `stage/init` | ~210 | PID 1 |
| `stage/usr/bin/pkg` | ~150 | package manager |
| `stage/usr/bin/linuxapp` | ~60 | glibc root chroot runner |
| `tools/*` | 11 | build, pack, fetch kernel/browser, smoke, size, repo server |

---

## 4. Verified working

- Boot **0.9 s**, **~70–92 MB** of 216 MB.
- Terminal (monospace, 600-line scrollback, aligned at 1.6×).
- Files: toolbar (Back/Up/Home/Refresh), breadcrumbs, **9 drawn icons**, sort, status bar with real sizes and free space.
- Settings: live **text size** (0.8–1.6×) and **wallpaper**, persisted to `/root`, restored at boot.
- Taskbar only when windows exist; magnification; minimise/restore; double-click maximise; snapping; Alt+Tab.
- Lock screen (Ctrl+Alt+L, password `mystic`) — **not re-verified since the key-queue refactor**.
- 8 external bundles as separate processes; Paint strokes, Mandelbrot, Life, 2048 all render.
- **Networking works**: DHCP lease `10.0.2.15`, DNS, TCP, `example.com` fetched (577 bytes) from the shell.
- **App sandbox works and confines** (see §6).
- Package manager + desktop install path; repo with 9 packages (574 MB incl. browser).

---

## 5. Browser: our own, and the pivot for "real sites"

**Ours (`src/client/apps/web.c`, 87 KB, compiles clean, runs):** own HTTP client (getaddrinfo,
port 80, Content-Length + chunked, one redirect), own HTML→text renderer (structure, h1–h6,
lists, numbered links, entities, metric wrapping), URL bar with an honest **"http only"** badge,
history, scrolling, footer with bytes/status/ms.
**Unproven:** the fetch itself — Enter did not start a load in the tested build. **That is bug #1.**

**Real engines** — full research in `docs/BROWSER_PLAN.md`; the obstacle is **budget, not the
display protocol**:

| Option | Union closure | Verdict |
|---|---|---|
| Chromium headless + CDP pipe | 682 MB | all sites, JPEG frames, 8–12 days |
| **Chromium inside Xvfb, `XShmGetImage`, control via CDP** | 807 MB (+7.6 MB of X) | **recommended pivot** — raw pixels, and `apt` comes with the rootfs |
| NetSurf on libnsfb | fits 256 MB | no JS, no video |
| WebKitGTK + own Broadway server | 569 MB | every GTK app, 12–18 days |

No option plays **DRM** video (no Widevine in Alpine v3.24).

Engine already on disk: `build/browser-root/` (1.5 GB, 202 MB `chromium-headless-shell`),
`build/repo/browser.tar.gz` (574 MB). Ubuntu base rootfs fetched:
`build/cache/ubuntu-base.tar.gz` (29 MB, has glibc loader + `apt`).

---

## 6. IN-FLIGHT / JUST-LANDED

### 6.1 App sandbox — WORKS, confined (verified)
`src/wmsrv.c`: `sandbox_child()` unshares NS/NET/IPC/UTS, makes the mount tree private, builds
`/run/mystic/jail/<id>` with bind mounts of **only** the app binary, the WM socket, `/dev/null`,
`/usr/share/fonts` (ro), `/dev/urandom` (ro), `/dev/shm`, `/proc`, then `chroot` +
`setuid(1000)` + `execl("/bin/app")`.

**Proof it is real**: 2048 launches inside the jail, draws through the compositor, and reports
`/dev/urandom: No such file or directory` — i.e. it can only see what was bound.

**Security hunter's CRITICALs, all fixed:**
- **C3** child closes every fd ≥ 3 — it was inheriting a root-opened `/dev/fb0` (read/write the
  entire screen, forge a lock screen) and the evdev keyboard (**the unlock PIN in cleartext**,
  because evdev gives each descriptor its own copy of events). This survived a *perfect* jail.
- **C1** the jail root was never created (`mkdir` doesn't make parents) → chroot ENOENT → ran as root.
- **C2/H3** `unshare`/`setuid` failures now `_exit` instead of falling through.
- **C4** `exec=` must be a bare filename and the unconditional host fallback is gone
  (`exec=../../../../bin/pkg` was a root-exec primitive).
- **H4** WM socket is 0666 so the sandboxed uid can connect.

**Still open from that hunt (H1, H2, H5):** one uid for all apps and `/dev/shm` bound rw → an app
can read/forge/`ftruncate` another app's buffer and `SIGBUS` the desktop; `/dev/shm` and the jail's
`/tmp` are unbounded (a "sandboxed" app can OOM the machine); `/proc` is the host's (needs
`CLONE_NEWPID`).

### 6.2 Store install UI — in flight
Desktop services `/run/mystic/install.request` → `pkg install <name>` (name whitelisted
`[a-z0-9-]{1,32}`) and `/run/mystic/repo.refresh` → `wget` the index to `/run/mystic/repo.index`.
An RSI worker is adding the Store's **Available** section (read index, mark installed, `r`
refreshes, Enter writes the install request). Check `progress` for it.

### 6.3 Unresolved worker outcomes (judgement calls already made)
- `research-browser` — UNRESOLVED on **bookkeeping only** (patch count 45 vs 46, w3m closure
  1.5 vs 12 MB, three anchors labelled with the wrong commit, one false CANNOT, Broadway days
  inconsistent). **Keep the file; conclusions unaffected.**
- `files-rich` / `files-icons` — both landed; the icons revision is a **superset** (nine marks:
  gear for `.conf`, palette disc for `.theme`). Verified visually.
- `arch` (docs/ARCHITECTURE.md) — UNRESOLVED with four stale claims (Theme fields listed that
  `wallpaper.c` never reads; an `image.sh` defect that is now fixed; `--quick` description;
  `icon_for_id` completeness). Low risk, still wrong.

---

## 7. Open items, in the order I would take them

1. **Browser fetch**: Enter in the URL bar does not start a load. Networking is proven from the
   shell, so this is a keybinding/state bug in `web.c`.
2. **Sandbox H1/H2/H5**: per-app uid, per-app private `/dev/shm` (bind a per-app tmpfs), `size=`
   on `/dev/shm`, `CLONE_NEWPID` + fresh procfs.
3. **Encryption** (owner's explicit ask; there is none): kernel has `DM_CRYPT=m`/`BLK_DEV_DM=m`.
   Realistic: ship `cryptsetup` as an installable package + an encrypted `/root` created at first
   boot, or a minimal dm-crypt setup tool.
4. **Package index signing** (Ed25519, ~300 lines, no deps) — today the index and the hashes come
   from the same unauthenticated HTTP source.
5. **Setup screen / user profile** (owner asked): first-boot app for theme, text size, wallpaper,
   password; profile + password change in Settings (`mystic-passwd`).
6. **Lock screen re-verify** (Ctrl+Alt+L) and add a visible Lock action; the handler exists twice
   in `main.c` (`input.key == 38` ~:1203 and `k == 38` ~:1281) — the first is likely dead.
7. **Damage rectangles** in the protocol (`FRAME x y w h`) — the hunter measured up to ~117 ms per
   full recompose with ten windows, which is the stutter the owner feels.
8. `docs/ARCHITECTURE.md` bookkeeping (§6.3).

---

## 8. Gotchas that have each cost hours

1. **The tool/shell mangles backslashes in heredocs.** Any `python - <<'PY'` replacement containing
   `\\n` or `\"` writes a *real* newline into C source. **Use the `edit` tool for anything with a
   backslash.** This has broken `main.c`, `wmsrv.c`, `notes.c`, `image.sh` at least six times.
2. **Patch scripts that abort before writing.** If edit 5 of 6 throws, *nothing* is saved even
   though edits 1–4 printed "ok". Write after each edit, or use the `edit` tool (atomic).
3. **Windows cannot create Linux symlinks** — `mkimage.py` writes cpio directly; the browser fetcher
   materialises symlinks as copies.
4. **`mkimage.py`'s executable rule**: `init`, `bin/`, `sbin/`, `usr/bin/`, `usr/sbin/`, paths ending
   `/bin` (app payloads), `*.script`. Missing it causes silent `ENOENT` on exec and DHCP failure.
5. **CRLF kills shell scripts** (`#!/bin/sh\r`). `.gitattributes`; `sed -i 's/\r$//'`.
6. **A running QEMU locks `build/initramfs.cpio.gz`** → builds fail "Invalid argument".
7. **Hunters must not boot QEMU** (they collide on the image and on `pkill -f qemu`).
8. **Windows Defender flags agent-built harness `.exe`s** in /tmp — delete them, they are not the repo.
9. **Never dispatch a worker on a file you are editing** (`main.c` collisions have cost two rewrites).
10. **WHPX**: without `-accel whpx`, boot is 8 s instead of 0.9 s.
11. **`getaddrinfo ENOTFOUND cheapseek.kdns.fr`** — the model provider drops out mid-run. Work
    written before the error is usually fine; always re-check the file rather than re-running.

---

## 9. Where the 500k goal stands

**~27,150 genuine lines.** Breakdown: desktop 5,238 · in-desktop apps 5,466 · SDK + external apps
5,773 · headers 407 · tools+init 6,724 · docs 3,549. Plus 5,321 lines of third-party
`stb_truetype.h` (not counted as ours).

At the measured rate (~2,800 verified lines per 30-minute swarm with a critic on every file),
500k is ~85 more swarm cycles. The owner forbids padding, so the honest progress metric is
capability; the line count is a by-product.
