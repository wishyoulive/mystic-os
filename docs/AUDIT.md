# Audit — 2026-11-26

Eight hunters swept the tree for defects. The rule for the sweep was that a
finding without a reproduction is not a finding, so every entry below has
evidence: a harness, a command, or a source/citation pair. **Nothing has been
fixed on `master`.** The patches the run produced are parked on the branch
`audit-sweep` so they can be reviewed and landed one at a time.

Full reports (gitignored, on this machine): `.work/hunt/{system,tools,browser,docs}.md`
and the running ledger `.work/hunt/LEDGER.md`.

## How much this covers

| Area | Method | Findings |
|---|---|---|
| boot, sandbox, `pkg` | sandbox-plan harness against the real `src/wmsrv.c`; real `pkg` end-to-end with `MYSTIC_*` test hooks; fixture repositories | 8 |
| compositor, window management | source audit + scripted-input reasoning | 3 |
| input | source audit + pure-function harnesses | 2 |
| client apps | ASan/UBSan builds and drives | 4 |
| browser (web.c, HTML/CSS, URL, TLS) | 20 000 generated documents, 80 000 random URLs, RFC 3986 differential, 24 000 structural PNGs, 12 hand-built X.509 chains | 7 |
| format parsers (font, office, img, pdf) | 114 adversarial files + mutation fuzzing under ASan/UBSan | 2 + 114 negative |
| build/packaging tools | deliberately broken inputs in scratch trees, HEAD-vs-current differentials | 3 |
| documentation | claim-by-claim comparison against code and artifacts | 30 |

## Highest severity

**P1 — a font file can kill the process that loads it.** `src/font.c` accepts a
TTF whose composite glyph references itself. `glyph_rasterise → stbtt_MakeGlyphBitmap
→ stbtt_GetGlyphShape` recurses without a depth limit and the process dies with a
stack overflow. `validate_glyph_outline` (`font.c:542-633`) checks byte spans and
caps components at 512 but never checks the component graph is a DAG. Reproduced;
case saved.

**P2 — a 198-byte `.docx` asks for 268 MB.** `office_zip_read` in `src/office.c`
allocates `usize + 1` from the ZIP central directory without comparing it to
`csize`: 2.5 M:1 amplification, and a 204 KB zip-of-zeros freezes `office_parse`
for 6.0 s. Reproduced.

**S1 — the sandbox uid pool can hand a live uid to a second bundle.**
`wms_launch_arg()` reserves a uid for a `host = yes` launch, forks, and returns
(`src/wmsrv.c:1988`) without `sbx_cred_add_anchor()`, which only the jailed path
reaches (`:2046`). The next acquire — or *any* app disconnect, via
`sbx_cred_del_pid → sbx_cred_gc` — frees that live uid. Harness:
`gamma(running)=2002 delta=2002`, `owner now = delta`. This is the shared-uid hole
the pool was built to close.

**S2 — the sandbox step buffer is exactly full at the widest legal manifest.**
`chroot=no + procs=yes + network=host + audio=yes + a document` needs 25 steps;
`SBX_STEPS` is 24 and `sbx_add()` silently drops the overflow, including the
fail-closed verification step. Harness: `nstep=24, drop=1, verify=0`.

**W1/W2/W3 — URL resolution is wrong in three ways**, all reachable from every
`<a>`, `<link>`, `<form action>`, `Location:` and `<img src>`, and sent verbatim
on the wire: a resolved path loses its trailing slash; the base *query* is treated
as path so `href="?page=2"` drops the last segment; and a reference's query is
itself path-cleaned, so `x?u=https://…` becomes `x?u=https:/…` and `x?p=/../y`
changes the request path.

**D1–D3 — the documentation contradicts the shipped image on three things a user
will notice.** Encryption is described as "designed, not in the image" while
`stage/sbin/mystic-dmcrypt` ships executable and `stage/init:640` sets up dm-crypt;
"no frame has been decoded in the VM yet" survives a `docs/VLC.md` with booted
60 fps evidence; and `docs/RELEASE.md` still says plain-HTTP-only with a citation
that points at `CTL_CHECKBOX`.

## Everything else, by area

- **Sandbox:** absolute bind targets bypass the `sbx_walk()` symlink guard (S4);
  `host=yes` keeps gid 0, skips `setgroups(0)`, checks nothing after `setuid` and
  leaks fds (S5); it also binds the bundle at `/app` in the desktop namespace and
  never unmounts, tolerating `EBUSY` (S6). `prepare_cache` follows a symlinked
  `$CACHE` (S7, not reachable today). `MS_PRIVATE` failure only logs (S8).
- **Compositor:** resize-edge placement, focus and z-order after close, and the
  lock screen's input reach are audited with reproductions in `.work/hunt/wm.md`.
- **Input:** the duplicate Ctrl+letter path (key event and `0x13`), stuck
  modifiers after focus change, and wheel sign are in `.work/hunt/input.md`.
- **Apps:** fixed-buffer writes, pixel-loop bounds and div-by-zero sites are
  itemised per app in `.work/hunt/apps.md`.
- **Tools:** `fetch-vlc.py` published the index before signing (a signing failure
  left a new index beside an old signature); `mkiso.py` raised a traceback instead
  of its documented exit-2 on a non-cpio initramfs; a test rewrote a tracked
  `.pyc`. The run also produced `tools/check_initramfs.py`, a shared gate that
  both artifact builders can call.
- **Docs:** 30 contradictions with doc line and code line — counts (six vs twelve
  themes, four vs eight wallpapers, 27 vs 50 plugins, `WM_MAX_CLIENTS` 8 vs 16),
  a shell command in CONTRIBUTING that cannot link, two shortcut chords that do
  nothing, and 17 stale `file:line` citations.

## Verified as *not* defects (so they are not re-hunted)

- `pkg` refuses `../` members, absolute members, escaping or absolute symlinks,
  a wrong size and a wrong sha **before** unpacking (real `pkg`, host harness).
- The uid pool is correct on the normal jailed path.
- 114 adversarial image and PDF cases (dimensions at every cap, bad Huffman
  tables, sampling factor 0, page-tree/xref/xobject cycles, 100 k-point paths,
  1 M-point MediaBox) were refused or handled correctly.
- Plan flags and tmpfs sizes match the code; `stage/init` provides the four things
  the sandbox expects.

## Also open, found from use rather than by a hunter

- Cyrillic and Devanagari still render as boxes in Chromium after Noto Sans and
  Noto Sans Devanagari were added to `stage/usr/share/fonts`: its closure brings
  its own fontconfig cache, so image fonts alone do not reach it.
- `image.sh themes` sends theme keys that have no binding (`Ctrl+Alt+1…9` is the
  real chord), so its screenshots cannot differ.
- `/dev/shm` in `stage/init` is sized for 8 clients while `WM_MAX_CLIENTS` is 16
  (128 MiB cap against a 132.7 MB worst case).

## What the fixes on `audit-sweep` do

`git log master..audit-sweep` is the list. They are small, each with the test that
proves it: the sandbox anchor for `host=yes`, the font composite-cycle check, the
office `usize`/`csize` bound, the three URL-resolution corrections with an RFC 3986
differential, and the three tool fixes. The documentation pass is a separate
review because it touches 30 claims.

## Follow-up: can full Chromium run natively? (measured)

The package question is settled, from the `chromium` APK itself:

| measurement | value |
|---|---|
closure | 235 packages, 358,530,387 B to download, 889,206,588 B installed |
tree | 2,014 files, 687 symlinks |
Ozone/Wayland code | present: `ui/ozone/platform/wayland/host/*` is compiled into `usr/lib/chromium/chromium` |
libwayland | **statically linked into the binary**: 187 of the 304 printable strings from the packaged `libwayland-client.so.0.25.0` are inside it, including its own error messages |
dynamic Wayland imports | **none** — and that is the proof of static linkage, not of absence: `wl_display_connect` has 0 hits because nothing is imported |
X11 libraries | 10 of the 64 `DT_NEEDED` are X11 (`libX11`, `libXi`, `libXext`, `libXcomposite`, `libXdamage`, `libXfixes`, `libXrandr`, `libXrender`, `libxcb`); they must load even for a Wayland run, and the closure carries them |
GPU | not needed: `--disable-gpu` paints with Skia on the CPU into `wl_shm` buffers (`wayland_shm_buffer.cc`, ARGB8888/XRGB8888 present) |

Flags that matter, with evidence rather than folklore:

- `--ozone-platform=wayland` is required. Alpine's own `/etc/chromium/chromium.conf`
  ships `--ozone-platform-hint=auto` and **that switch does not exist in this
  binary** (0 hits).
- `--enable-features=UseOzonePlatform` is retired in 158 (0 hits).
- `chrome-sandbox` is installed 4755 in the APK; inside our jail that is irrelevant
  because the manifest already controls the sandbox, but `--no-sandbox` will be the
  first thing to try if the setuid helper complains.
- **Audio would fail for a manifest reason, not a hardware one:** the jail binds
  `/dev/snd` only when the manifest says `audio = yes`, and the generated bundle's
  manifest does not. The guest does have HDA (`snd-hda-intel`, `image.sh:144`); it
  has no PulseAudio/PipeWire server, so ALSA is the path and libasound is in the
  closure.
- **DRM:** the guest has `/dev/dri/card0` (bochs) but no render node and no
  accelerated driver in this closure, and the jail does not expose `/dev/dri`. The
  dmabuf/GPU path must not be advertised; the shm path is the one that can work.

One defect found in our own tool while measuring: the bundle generated by
`fetch-chromium.py` for the full browser has a launcher that execs
`usr/bin/chromium-headless-shell`, which **is not in the full chromium closure**
(it has `usr/bin/chromium` → `chromium-browser` → `lib/chromium/chromium-launcher.sh`).
Launched from a manifest (`exec = chromium`) it dies with ENOENT. That is a
launcher/manifest change, not a compositor one, and is parked on `audit-sweep`.

The compositor half is still unwritten: the `wayland` hunter died on a provider
error before producing `docs/WAYLAND.md`. What survives is the protocol research in
`.work/full-chromium/REPORT.md` §7 (wire signatures including the `wl_registry.bind`
special case `u name, s interface, u version, n id`; keycodes sent unchanged because
the client adds 8 against an evdev keymap; object ids from 2; advertise
`wl_compositor 4`, `wl_shm 1`, `wl_seat 5`, `wl_output 2`, `xdg_wm_base 1`).

## Status of the run's three workers

| worker | result |
|---|---|
| `pkg` | **approved** after two revisions; the measurements above are its evidence |
| `input` | unresolved: code written (coalesced motion, page focus that survives a click, live repaint, keep-frame + 5 s retry), 20/20 selftest, three silent builds — but its critic died mid-verification, so nothing is claimed. Parked on `audit-sweep`, unverified |
| `wayland` | failed on a provider error before writing the document; the research survives in the pkg report as noted |
