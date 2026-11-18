# A real browser engine on Mystic OS

The owner asked for full HTML/CSS/JS. `src/client/apps/web.c` is our own
renderer: it parses HTML, applies a CSS subset and draws text, and it will
never run a framework. This document is the honest answer to "put a real
engine in it":

* which engine, chosen from measurements of the Alpine index rather than
  belief;
* what it costs in download, disk and RAM;
* how its frames get into a Mystic window and how our input gets out;
* what genuinely works, and what cannot (DRM);
* the exact install command, including the two machine settings the default
  image does not have.

Every number below is from a command in this repository, quoted with the
command that produced it. The tag says where it came from:

| Tag | Meaning |
|---|---|
| **[idx]** | computed from the Alpine v3.24 `APKINDEX` (x86_64) by `python tools/fetch-engine.py --candidates` |
| **[apk]** | measured on the downloaded `.apk` / unpacked ELF in `build/engine-root` |
| **[pkg]** | from the package this run built: `build/repo/engine.tar.gz` |
| **[ours]** | read out of this repository, by symbol or line |
| **[run]** | output of a command that was actually executed, quoted in §11 |
| **[not measured]** | says so, and what would settle it |

Nothing here was booted. No QEMU was run, no page was rendered, no frame was
produced. §11 lists exactly what *was* verified and how, because the
difference between "compiled and checked" and "seen working" is the whole
point of this document.

---

## 0. Verdict

**Chromium headless (`chromium-headless-shell`) is the only candidate that is
genuinely headless and can hand us frames we can blit.** It is also large:
**182 packages, 258.30 MB download, 681.98 MB installed** [idx], packed here
as **259.65 MB** [pkg]. That is twenty times the whole 13.42 MB image, so it is
a package on the data disk, never part of the initramfs.

It needs no X server, no Wayland, no GPU and no compositor: it speaks the
DevTools protocol over a pair of pipes, `Page.startScreencast` delivers JPEG
frames, and `src/img.c` already decodes JPEG into `0xAARRGGBB`, which is the
window format. Input goes the other way through `Input.dispatchMouseEvent` and
`Input.dispatchKeyEvent`.

The default guest cannot hold it: `image.sh` boots with `MYSTIC_RAM=256` and a
64 MB disk [ours]. The install command in §8 raises both, and moves pkg's
download cache off the RAM filesystem.

If 259 MB of download is unacceptable, the honest small answer is **not** a
browser engine: it is NetSurf's framebuffer build at **23 packages, 9.46 MB
download, 22.73 MB installed** [idx], which parses HTML and a CSS 2.1 subset
and runs **no JavaScript at all**. §9 has both that and the "libdom + our
renderer" option, which Alpine does not package.

---

## 1. The candidates, measured

`python tools/fetch-engine.py --candidates` [run] resolves each candidate's
real dependency closure from the index — plain names, `so:`/`cmd:`/`pc:`
provides, every transitive dependency — and prints:

| candidate | packages | download | installed | JS | display |
|---|---|---|---|---|---|
| **chromium-headless-shell** | **182** | **258.3 MB** | **682.0 MB** | V8 | none |
| chromium (full browser) | 181 | 318.5 MB | 799.5 MB | V8 | none needed |
| webkit2gtk-4.1 | 200 | 210.6 MB | 569.1 MB | JavaScriptCore | X or Wayland |
| webkit2gtk-4.1 + xvfb | 212 | 215.9 MB | 576.1 MB | JavaScriptCore | Xvfb (+12 pkgs, +5.3 MB download, +7.0 MB installed) |
| webkit2gtk-6.0 | 206 | 216.3 MB | 585.9 MB | JavaScriptCore | Wayland/GTK4 |
| **wpewebkit** | — | — | — | — | **not in the index** |
| netsurf-framebuffer | 23 | 9.5 MB | 22.7 MB | **none** | fbcon/SDL |
| netsurf (GTK) | 87 | 28.6 MB | 74.2 MB | **none** | X |

Read the index fields carefully, because this repository has been bitten
before: `S:` is the `.apk` **download** size and `I:` is the **installed** size.
`tools/fetch-browser.py` prints `I:` and calls it "compressed"; the numbers in
this table are labelled, and §11 shows the two agreeing with the bytes on disk.

**WPE WebKit does not exist in Alpine v3.24.** The index has `libwpe 1.16.3`
and `libwpebackend-fdo 1.16.1` — the embedding *helpers* — and no WebKit
package that uses them. `apk add wpewebkit` fails, so there is nothing to cost.
The candidate is listed with dashes rather than a number because a number
would be invented.

**WebKitGTK is smaller but cannot be driven from outside its own process.** It
renders into a GTK widget inside the process that owns it. Getting pixels out
means either writing a GTK host program and linking `webkit2gtk-4.1` (a
build-time `-dev` dependency we do not have in the image) and calling
`webkit_web_view_get_snapshot()`, or starting Xvfb and screenshotting a
display. Neither is "the engine hands us a frame we asked for", and both add a
display server to a 256 MB machine. Its download is 47.7 MB smaller; its
*footprint that can actually produce a frame* is not.

**Chromium headless is bigger but honest about the interface.** One process
tree, one pipe, `Page.startScreencast`, `Input.*`. No X, no GPU, no
compositor, and the protocol names are verifiably present in the exact binary
we ship (§11, `[apk]`).

`chromium` (the full browser) is 60 MB more download and 117 MB more installed
for a UI we would never show. The headless shell is the same Blink and V8
without the window chrome.

---

## 2. What is in the package

`python tools/fetch-engine.py` [run] resolved, downloaded, unpacked, checked
and packed it. The tree, largest members first:

| package | installed | apk |
|---|---|---|
| chromium-headless-shell | 208.17 MB | 89.97 MB |
| llvm22-libs | 190.97 MB | 67.29 MB |
| mesa | 44.18 MB | 13.77 MB |
| icu-data-full | 33.11 MB | 12.73 MB |
| x265-libs | 21.34 MB | 4.04 MB |
| ffmpeg-libavcodec | 20.36 MB | 9.33 MB |
| libmagic | 10.79 MB | 0.53 MB |
| gtk+3.0 | 9.93 MB | 3.99 MB |
| aom-libs | 8.38 MB | 3.21 MB |
| libSvtAv1Enc | 7.22 MB | 3.01 MB |

Those are the index's own numbers for the closure it computed; the other 172
packages account for the rest of 682 MB. Two of them deserve a sentence:

* **llvm22-libs (191 MB)** is there because `mesa` needs LLVM for its software
  rasteriser. The engine links `libgbm.so.1` [apk, DT_NEEDED], so `mesa` must be
  installed; LLVM is a *package* dependency of mesa, not a link-time need of
  the engine. `--exclude llvm22-libs` drops 191 MB and the closure check still
  passes (nothing we ship lists `libLLVM.so` in DT_NEEDED), but whether
  Chromium ever `dlopen()`s a DRI driver is exactly the kind of thing that is
  only settled by a boot, so the default keeps it. The tool prints the saving
  before doing anything and checks the result afterwards either way.
* **icu-data-full (33 MB)** is the ICU data the engine reads. Chromium looks
  for `icudtl.dat` beside its executable first, so the packer puts a relative
  symlink there pointing at `usr/share/icu/<ver>/icudt<ver>l.dat` [ours,
  `fetch-engine.py` build_stage step 4]. One copy, two paths.

The package itself [pkg]:

```
build/repo/engine.tar.gz      259,646,299 bytes (259.65 MB)
  members                     1,646 (187 dirs, 1,143 files, 317 symlinks)
  member bytes                682.87 MB
  sha256                      6daccfb99c0a9d918b5fcfa3752b3226c46f0e8aeef8bea9fced59e4ed314bc1
build/repo/index.tsv row      engine  156.0.8078.79  259646299  <sha>
                              engine.tar.gz  ...  682866442   (col 7 = unpacked)
build/repo/index.tsv.sig      Ed25519 over the exact index bytes, verifies
```

The archive is deterministic: mtimes 0, uid/gid 0, sorted members, `gzip -6`
with `mtime=0`. Two runs of the packer produced the same bytes when the stage
was identical.

**Symlinks matter on this package.** 317 of the members are symlinks
(`libgbm.so.1 -> libgbm.so.1.5.0` and friends). Inside the guest that is free
on ext4, which is what `/root` is (`stage/init`: `mkfs.ext4`, `mount -t ext4`)
[ours]. On a filesystem that cannot store links, `pkg` copies each link from
its target and says so before installing — and for this package that costs
**+904.94 MB** (measured: 315 targets, 1,587.03 MB total). That is why §7 says
the data disk must be ext4 and big.

---

## 3. How a frame gets in, and how input gets out

### 3.1 The pipe

`--remote-debugging-pipe` gives the browser two file descriptors: it reads
commands on **fd 3** and writes replies on **fd 4**, each message a JSON object
terminated by a NUL byte. No HTTP, no WebSocket handshake (which would need
SHA-1, a nonce and client-side frame masking), no listening socket inside the
jail.

`engine_spawn()` in `src/client/apps/engineview.c` does one `pipe()`/`fork()`/
`execv()` and hands the child those two numbers. The `!= 3` / `!= 4` guards
before `close()` are deliberate: `pipe()` can return 3, and closing it after
`dup2` would close the descriptor the engine is about to read.

### 3.2 The commands, in order

```json
{"id":1,"method":"Target.createTarget",
 "params":{"url":"about:blank","width":1100,"height":668}}
{"id":2,"method":"Target.attachToTarget",
 "params":{"targetId":"<id>","flatten":true}}
{"id":3,"sessionId":"<s>","method":"Page.enable"}
{"id":4,"sessionId":"<s>","method":"Emulation.setDeviceMetricsOverride",
 "params":{"width":1100,"height":668,"deviceScaleFactor":1,"mobile":false}}
{"id":5,"sessionId":"<s>","method":"Page.startScreencast",
 "params":{"format":"jpeg","quality":70,"maxWidth":1100,"maxHeight":668,
           "everyNthFrame":1}}
```

Then the engine pushes frames:

```json
{"method":"Page.screencastFrame","sessionId":"<s>",
 "params":{"data":"<base64 JPEG>","sessionId":7,"metadata":{...}}}
```

and we must acknowledge every one, or the engine stops sending:

```json
{"id":6,"sessionId":"<s>","method":"Page.screencastFrameAck",
 "params":{"sessionId":7}}
```

The two `sessionId`s are different things and the client keeps them apart: the
top-level one is the flat target session, `params.sessionId` is the screencast
session the ack must name [ours, `handle_message()`].

### 3.3 The pixels

`b64_decode()` turns the payload into a JPEG; `img_decode()` from `src/img.c`
turns that into `Img { w, h, px }` with `0xAARRGGBB` pixels — "the same format
the framebuffer and the window server already use" (`src/img.h`). The viewport
is set to exactly the page rectangle, so the normal case is one `memcpy` per
row into the client shm buffer. During a resize, a frame that still has the old
size is stretched with nearest-neighbour for one tick rather than leaving the
window blank.

No colour conversion, no alpha blend: the compositor ignores the top byte and
`img.c` writes `0xFF` there [ours].

### 3.4 Input

| Event | CDP call | Notes |
|---|---|---|
| pointer move | `Input.dispatchMouseEvent` `mouseMoved` | coordinates translated from the window into the page rect |
| press / release | `mousePressed` / `mouseReleased`, `button:"left"`, `clickCount:1` | |
| wheel | `mouseWheel`, `deltaY = -notches * 120` | one evdev notch is 120 device units |
| printable text | `Input.dispatchKeyEvent` with `text` + `keyUp` | the text comes from the window server's `TEXT` event, so shift is already applied |
| a paste-like burst | `Input.insertText` | more than one character at once |
| Enter, Tab, Backspace, arrows, F-keys, Ctrl+letter | `dispatchKeyEvent` with `windowsVirtualKeyCode` | table `ev_keys[]` |

Limits, stated in the window too: **US layout only** (evdev codes, no IME), one
tab, and mouse modifier keys are not forwarded because the window server's
`MOUSE` message carries position and buttons only (`src/wmsrv.c`,
`wms_notify_mouse`). `WHEEL` carries no position at all, so the app remembers
where the pointer last was — which is what that function's comment says a
client is expected to do.

---

## 4. Why it starts inside the jail

`src/wmsrv.c` chroots each app into `/run/mystic/jail/<id>` and binds only
`/bin/app`, the bundle read-only at `/app` (`MYSTIC_BUNDLE`), fonts, `/dev`
bits, `/proc`, `/dev/shm`, a private `/root` and a writable `/tmp`. There is
**no `/lib`**. The engine's `PT_INTERP` is the literal string
`/lib/ld-musl-x86_64.so.1` [apk], so the kernel cannot start it as shipped.

The fix is a packaging one, done and verified by `tools/fetch-engine.py`:

```
PT_INTERP rewritten: /lib/ld-musl-x86_64.so.1 -> /app/ld-musl-x86_64.so.1
                     (25 bytes at file offset 736)
```

The string fits because the original segment is 24 bytes + NUL and the ELF
leaves 28 bytes before the next non-zero byte (measured: `PT_INTERP offset 736,
filesz 25, next non-zero byte at +28`); the new string is 25 bytes + NUL. The
tool refuses rather than guesses if the room is not there, and reads the value
back after writing.

Patching is required instead of exec'ing the loader by hand (which is how the
`vlc` bundle starts its player) because **Chromium re-execs `/proc/self/exe`**
for its zygote and renderer children. With a loader as the process image that
re-exec would run the loader with no program to load. With `PT_INTERP` patched,
`/proc/self/exe` is the engine binary and the children inherit
`LD_LIBRARY_PATH=/app/usr/lib:/app/lib`, which `src/wmsrv.c` already exports.

**No `wmsrv.c` change is needed for this package.** It is worth saying because
the previous libVLC work needed a one-line environment addition; this one lands
inside what the jail already provides:

* `LD_LIBRARY_PATH` → shared objects in `usr/lib` [ours, wmsrv.c `setenv`];
* `/dev/shm` → Chromium's shared memory segment;
* `/proc` → its process enumeration;
* `/tmp` → `--user-data-dir=/tmp/engine-profile`;
* `/usr/share/fonts` → text.

Two paths are **not** bound and are the most likely reason a first boot would
fail, so they are stated here rather than discovered later:

1. **`/etc/fonts`** is not in the jail. fontconfig then falls back to its
   built-in defaults, which do include `/usr/share/fonts`, but the system
   `fonts.conf` and `conf.d/` are absent. If text renders in the wrong face or
   not at all, this is why.
2. **`/usr/share/icu`** is not in the jail either. The packer mitigates it by
   putting `icudtl.dat` beside the executable, which is the first place
   Chromium looks, but the absolute path its build may also use does not exist
   inside the chroot.

If a boot shows either failure, the fix is six lines in `src/wmsrv.c`, next to
the existing font bind (`sandbox_child`, the `/usr/share/fonts` block):

```c
snprintf(path, sizeof(path), "%s/etc/fonts", jail);
mkdirp_for(jail, "/etc/fonts");
mount("/etc/fonts", path, NULL, MS_BIND | MS_RDONLY, NULL);
snprintf(path, sizeof(path), "%s/usr/share/icu", jail);
mkdirp_for(jail, "/usr/share/icu");
mount("/usr/share/icu", path, NULL, MS_BIND | MS_RDONLY, NULL);
```

That change is **not** in this run's diff (`src/wmsrv.c` belongs to whoever
owns the window server) and it is only needed if the packaged symlink turns
out not to be enough. Reporting it is the honest alternative to pretending the
question is settled.

The app itself checks the two facts it depends on before forking, and shows
which one is missing instead of a dead rectangle:

* `usr/lib/chromium/chromium-headless-shell` not executable → "the engine
  payload is not in this bundle: <path>: <errno>";
* `ld-musl-x86_64.so.1` missing → "the bundle has no <path>, and the jail has
  no /lib: the engine's PT_INTERP is /app/ld-musl-x86_64.so.1".

---

## 5. What works, what does not

| Capability | Status |
|---|---|
| HTML5 parsing | Blink. The binary contains `HTMLDocument`, `CSSStyleSheet` and `blink` strings [apk, byte search]. |
| CSS | Blink: full CSS, not our subset. |
| JavaScript | V8. The binary contains `v8_context_snapshot` [apk]. WebAssembly is V8's. |
| Canvas / WebGL | Canvas yes (software). WebGL is refused: the app passes `--disable-gpu` because there is no GPU; Chromium would otherwise try DRI drivers that need `llvm22-libs` and a DRM node. |
| H.264 / VP9 / AV1 video without DRM | The engine links `libavcodec.so.62`, `libopenh264.so.8`, `libdav1d.so.7`, and the closure carries `x265-libs`, `aom-libs`, `libSvtAv1Enc` [apk, DT_NEEDED + idx]. Decoding is therefore present. **Not measured**: no video was played. |
| Audio from a page | The app passes `--mute-audio` when `/dev/snd/pcmC0D0p` does not exist and says so once on stderr. With a PCM node it does not pass the flag and the page's audio goes to the engine's own output. Whether that output exists in the guest is the audio worker's question, not this document's. |
| **DRM (Netflix, Spotify, Prime)** | **No.** The engine knows the API (`requestMediaKeySystemAccess`, `widevine` strings are present) but the CDM library `libwidevinecdm.so` is not in the binary's directory and there is **no widevine package in the whole Alpine v3.24 index** [idx]. Without a CDM, EME playback fails by design. |
| PDF viewer | Chromium's PDF viewer needs its plugin resources; the headless shell package does not carry them. Use `docview`/`pdf.c`. |
| Downloads | Not wired: `Page.setDownloadBehavior` is not called and there is no file picker in the jail. |
| Tabs | One. `Target.createTarget` is called once. |
| History back/forward | Not wired (Ctrl+R reloads). The protocol has `Page.getNavigationHistory`; the UI does not. |
| IME / non-US keyboards | No. evdev codes + US table. |
| Extensions, sync, accounts | Not present (`--disable-extensions --disable-sync`). |

The engine runs with `--no-sandbox`. That is not an oversight: Chromium's own
sandbox needs unprivileged user namespaces and a filesystem layout the jail
does not have. The OS jail — a chroot, its own mount namespace, uid 1000, no
access to `/root`, the framebuffer, the keyboard device or other clients —
is the sandbox. It is weaker than Chromium's: a compromised renderer has the
engine's whole tree and can reach the network directly (`network = host`).

---

## 6. Files

| Path | What it is |
|---|---|
| `tools/fetch-engine.py` | resolves the closure, downloads with resume, unpacks, patches `PT_INTERP`, compiles the app, checks every `DT_NEEDED`, packs, updates and signs the index, prints the real sizes |
| `src/client/apps/engineview.c` | the window app: CDP over the pipe, JPEG→`0xAARRGGBB` frames, input forwarding, address bar, honest failure cards. `EVP_SELFTEST` builds the parser tests |
| `tools/serve-repo.sh` | keeps the `engine` row when it rebuilds the index (it used to publish a `browser` package from a tree with no manifest — that row is gone) |
| `docs/ENGINE.md` | this file |
| `build/engine-root/` | the unpacked closure (681.98 MB), cache-stamped |
| `build/engine-pkg/` | the package stage (682.87 MB) |
| `build/repo/engine.tar.gz` | the installable package (259.65 MB) |
| `build/engine-apks/` | the `.apk` cache (258.30 MB); `--force` does not delete it |
| `build/engine-build.log` | the compiler's exact words for the app |

The bundle layout, as installed by `pkg` under `/root/apps/engine`:

```
manifest                       name/desc/exec=engine/version/network/bundle/installed
engine                         the static app (212,496 bytes)
ld-musl-x86_64.so.1            musl's loader, at the literal PT_INTERP path
usr/lib/…                      the shared-object closure
usr/lib/chromium/chromium-headless-shell      202.43 MB, PT_INTERP patched
usr/lib/chromium/icudtl.dat -> ../../share/icu/…/icudt…l.dat
usr/share/icu/…                ICU data
bin/ etc/ lib/                 the rest of the closure (busybox, fontconfig, …)
```

`exec = engine`, not `bin`, because Alpine's tree has a top-level `bin/`
directory and a file and a directory cannot share a name. `src/wmsrv.c` only
requires the exec to be a bare file name.

---

## 7. Install it

### 7.1 On the host

```bash
python tools/fetch-engine.py --candidates   # the table in §1, no download
python tools/fetch-engine.py --dry-run      # the closure and the cost, no download
python tools/fetch-engine.py                # 258 MB down, 683 MB unpacked, pack
python tools/serve-repo.sh                  # serve build/repo on :8000
```

The cap is real: a closure over `--max-download-mb` (default **400**) stops
before fetching a byte and prints the numbers. This closure is 258.30 MB, so it
proceeds. `--force` is for the cap and for ignoring the tree cache, not for
skipping checks: the closure check and the archive verification always run.

What it prints when it finishes [run]:

```
==> closure check: 414 ELF files, 728 loadable names (317 from shipped links),
    0 unresolved by name
==> stage: 1142 files, 682.87 MB (317 links, 1 dropped links)
    dropped link usr/lib/vdpau/libvdpau_trace.so -> libvdpau_trace.so.1
    (target not in the tree)
==> packed engine.tar.gz: 259.65 MB from 682.87 MB of members (1646 entries)
==> index row: engine  156.0.8078.79  259646299  6daccfb9…  engine.tar.gz  …
```

The one dropped link is Alpine's own: `libvdpau_trace.so` points at
`libvdpau_trace.so.1`, which the `libvdpau` package does not ship. Shipping a
link to nothing would make `pkg` "repair" it to nothing; dropping it and saying
so is the honest option. No `DT_NEEDED` names it.

### 7.2 The guest's machine settings

The default image boots with 256 MB of RAM and a 64 MB disk [ours,
`image.sh`], and pkg's download cache is `/tmp/pkg` — a **tmpfs**, i.e. RAM.
All three have to change for a 259 MB package:

```bash
# host, once: a bigger disk. image.sh only creates build/disk.img when it is
# missing, so the old 64 MB file has to go first.
rm -f build/disk.img
MYSTIC_RAM=1024 MYSTIC_DISK_MB=2048 ./image.sh
```

`MYSTIC_RAM=1024` is the smallest guest this document is willing to
recommend: the engine binary alone maps 202 MB and the tree is 683 MB on disk.
Whether 1 GB is *enough* for a heavy page is **not measured** — nothing was
booted. If a page dies, the engine's last stderr line appears in the window's
status bar and the answer will be in it.

### 7.3 Install in the guest

```sh
# the download is 259 MB: keep it off /tmp (a RAM filesystem)
MYSTIC_CACHE=/root/pkgcache pkg update
MYSTIC_CACHE=/root/pkgcache pkg install engine
pkg installed            # engine 156.0.8078.79
```

`pkg` checks the signature over the index before reading a row, checks the
sha256 of the tarball before unpacking, checks the exact unpacked size from the
archive's own member list before writing, and reports the symlink situation
before it starts:

```
  space ok: … needs 682.87 MB, 1.99 GB free on /root
  /root stores symlinks: the 317 link member(s) need no extra space
```

On a filesystem without links the same line reads
`will be copied from their targets, +904.94 MB` and the space check uses the
larger number.

Then the Store shows a **Browser Engine** tile, or from a shell:

```sh
/root/apps/engine/engine https://example.com
```

The engine itself is at `/root/apps/engine/usr/lib/chromium/chromium-headless-shell`
if you want to run it with your own flags; it needs
`LD_LIBRARY_PATH=/root/apps/engine/usr/lib` and its `PT_INTERP` is
`/app/ld-musl-x86_64.so.1`, which only resolves **inside** the jail. Outside
it, run it through the loader:

```sh
/root/apps/engine/ld-musl-x86_64.so.1 \
  --library-path /root/apps/engine/usr/lib \
  /root/apps/engine/usr/lib/chromium/chromium-headless-shell \
  --headless --no-sandbox --disable-gpu --dump-dom https://example.com
```

(That is how you would get a first honest signal out of the engine without the
window: `--dump-dom` prints the DOM to stdout. It was not run in this session;
there was no guest.)

---

## 8. Removing it

```sh
pkg remove engine
```

682.87 MB comes back, and the Store tile goes with it.

---

## 9. The smaller alternative, with numbers

If 259 MB of download is the wrong trade, there are two smaller answers and
neither is a modern browser.

### 9.1 NetSurf's framebuffer build — 9.46 MB download, 22.73 MB installed

**23 packages, 9.46 MB download, 22.73 MB installed** [idx], and it renders
straight into a framebuffer or an SDL surface, so no X server and no CDP.

What it is not: **NetSurf 3.11 has no JavaScript engine.** Its CSS support is a
2.1 subset with some CSS3. A page that is a React app, or that needs
`fetch()`, shows nothing but its `<noscript>` or an empty body. For reading
documentation, Wikipedia's early-2000s surface and plain HTML sites it is
genuinely good; for "full HTML/CSS/JS" it is not the thing the owner asked for.

It also has no compositor integration: it wants the framebuffer device, which
the jail does not bind, so it would need the same kind of packaging work this
document describes plus a different present path.

### 9.2 libdom + hubbub + our renderer — **not packaged**

The idea is to take NetSurf's HTML5 parser (`hubbub`), DOM (`libdom`) and CSS
engine (`libcss`) and feed our own renderer. It would give correct HTML5
parsing for a few hundred kilobytes. **Alpine v3.24 does not package any of
them**: a search of the index for `libdom`, `hubbub`, `libcss` returns zero
packages [idx]. They are built *into* the `netsurf` binary, not shipped as
libraries. So this option means vendoring and building NetSurf's libraries
ourselves (they need `nsgenbind`, a code generator, and `libwapcaplet`), which
is a week of build work, not an `apk add`.

It also does not solve JavaScript. A JS engine would be a second dependency:
`quickjs` (0.95 MB download, 1.9 MB installed) or `duktape` (0.15 MB / 0.3 MB)
are in the index [idx], but neither is a browser DOM/Web API implementation, so
a page calling `document.querySelector` still fails.

**Recommendation:** if the owner wants a real browser, install `engine`
(§7). If the owner wants a small, honest HTML reader, NetSurf is the option and
its cost is 23 packages. There is no 10 MB full browser engine, in this index
or anywhere else.

---

## 10. What this document is not

* It is not a benchmark. No page was rendered, so there is no frame rate, no
  load time, no memory figure for Chromium on this OS.
* It is not a promise that the engine starts on the first boot. §4 names the
  two jail paths that are most likely to need one more bind.
* It is not a claim that 1 GB of RAM is enough for heavy pages. It is the
  smallest guest this document will recommend, and §7.3 says where the answer
  will appear if it is not.

---

## 11. Verification log

Everything in this section was executed in this working tree. Commands are
reproducible; output is quoted, not paraphrased.

### 11.1 The compiler

```
$ zig cc -target x86_64-linux-musl -Os -static -Isrc -Wall -Wextra \
        -Wno-unused-parameter -c src/client/apps/engineview.c -o .work/engine/x.o
(no output, exit 0)

$ zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client -Wall -Wextra \
        -Wno-unused-parameter -o .work/engine/app-engineview \
        src/client/apps/engineview.c src/client/mystic_client.c src/gfx.c \
        src/font.c src/ui.c src/filetypes.c src/img.c -lm
$ file .work/engine/app-engineview
ELF 64-bit LSB executable, x86-64, statically linked, stripped      (212,496 bytes)
```

That link line is `tools/mkapp.sh`'s plus `src/img.c` (the JPEG decoder).

### 11.2 The app's own parsers, on the host

`EVP_SELFTEST` compiles the pure functions and their tests with no window
server. Built for the host and **run**:

```
$ zig cc -target x86_64-windows-gnu -Os -DEVP_SELFTEST -Wno-unused-function \
        -Isrc -Wall -Wextra -Wno-unused-parameter \
        -o .work/engine/selftest.exe src/client/apps/engineview.c
$ .work/engine/selftest.exe
ok   id is read
ok   nested result.sessionId
ok   method is read
ok   params slice
ok   params.data slice
ok   base64 AAECAwQ= -> 00 01 02 03 04
ok   the screencast session id is 7 (ack value)
ok   error object is found
ok   the engine's error text is read
ok   string escapes
ok   result.result.value is reachable
ok   the title JSON-decodes (with its \n)
ok   set_err truncates instead of overflowing
ok   evdev letters/digits for Ctrl shortcuts
ok   modifier mask is CDP's
ok   Enter/ArrowLeft/F6 table rows
ok   truncated JSON is refused, not walked past
ok   absent key is absent
all passed
```

**This test found a real bug.** The first version of `js_get()` started its key
scan at the opening quote, so `klen` underflowed and *no key ever matched* —
twelve of the eighteen checks failed. The frame path would have shown "no
frame" forever with no error. Fixed at the source (§ `js_get`), and the
regression is the first twelve lines above.

### 11.3 The engine binary, as downloaded

```
$ python - <<'PY'   # ELF parse of .work/engine/chs.elf
PT_INTERP: /lib/ld-musl-x86_64.so.1
DT_RUNPATH: []
DT_NEEDED (59): libglib-2.0.so.0 … libgbm.so.1 libdrm.so.2 libavcodec.so.62
                libopenh264.so.8 libdav1d.so.7 libX11.so.6 libasound.so.2
                libpulse.so.0 libc.musl-x86_64.so.1
```

Byte search of the same file for the protocol surface the app uses:

```
remote-debugging-pipe  present      startscreencast        present
remote-debugging-port  present      screencastframe        present
user-data-dir          present      dispatchmouseevent     present
no-sandbox             present      dispatchkeyevent       present
disable-gpu            present      attachtotarget         present
disable-dev-shm-usage  present      createtarget           present
devtoolsactiveport     present      navigate               present
v8_context_snapshot    present      HTMLDocument           present
blink                  present      CSSStyleSheet          present
libwidevinecdm         ABSENT
```

The `.apk` was 89,969,624 bytes — the index's `S:` exactly — and its members
sum to 208,174,865 bytes against an index `I:` of 208,171,836; the difference is
exactly `.PKGINFO` + `.SIGN` (2,517 + 512), which `I:` excludes. That is the
same trap `tools/fetch-browser.py` fell into, now measured.

### 11.4 The build

```
$ python tools/fetch-engine.py
==> main:       5961 packages
==> community: 22700 packages
==> index: 28661 packages, 41853 provides
==> engine closure: 182 packages, 258.30 MB download, 681.98 MB installed
==> downloading 182 packages into build\engine-apks
==> 182 apks on disk, 258.30 MB
==> unpacked 1139 files, 681.98 MB, 317 links (recorded, not materialised on
    this host), 0 special files skipped
    index predicted 681.98 MB installed for this closure
==> engine: usr/lib/chromium/chromium-headless-shell
      202.43 MB, PT_INTERP /lib/ld-musl-x86_64.so.1, 59 DT_NEEDED
      PT_INTERP rewritten: /lib/ld-musl-x86_64.so.1 -> /app/ld-musl-x86_64.so.1
                           (25 bytes at file offset 736)
==> window app: build/app-engineview, 212 KB static
==> closure check: 414 ELF files, 728 loadable names (317 from shipped links),
                   0 unresolved by name
==> stage: 1142 files, 682.87 MB (317 links, 1 dropped links)
==> packed engine.tar.gz: 259.65 MB from 682.87 MB of members (1646 entries)
==> index.tsv signed with stage\etc\mystic\repo.pub
```

Three numbers agree that have no reason to: the index's installed size
(681.98 MB), the bytes actually written by the unpacker (681.98 MB), and the
sum of every member in the finished archive (682.87 MB — the difference is the
app 212 KB + the loader 670 KB, which are the two files the packer adds).

### 11.5 The package, read back

```
$ python - <<'PY'   # tarfile over build/repo/engine.tar.gz
members: 1647
kinds: {'dir': 187, 'file': 1143, 'sym': 317}
root-level entries: ['./bin', './engine', './etc', './ld-musl-x86_64.so.1',
                     './lib', './manifest', './usr']
backslash/absolute symlinks: 0
member bytes: 682.87 MB
shipped engine PT_INTERP: /app/ld-musl-x86_64.so.1
bundle has that file: True
manifest: exec = engine / network = host / installed = 682866442
exec file in archive: True
```

and the index row and its signature:

```
$ python tools/sign-index.py verify --in build/repo/index.tsv \
      --sig build/repo/index.tsv.sig --pub build/repo/index.pub
ok: signature verifies over 1718 bytes of build/repo/index.tsv
key fingerprint: sha256 ce177b2cc56f3cfd4c3458a7e7201eece66b45b01a1a888b9a6f1e4cbcf90096
```

### 11.6 The closure check, and what it means

`check_closure()` resolves every `DT_NEEDED` of every shipped ELF **by file
name**, which is what musl's loader does (it opens the name and then rejects a
library whose `DT_SONAME` disagrees). A SONAME-only check — the glibc rule —
would pass packages that cannot start here. Shipped symlinks count as names,
because that is what makes `libgbm.so.1` openable when the real file is
`libgbm.so.1.5.0`.

`414 ELF files, 728 loadable names (317 from shipped links), 0 unresolved`
means the engine's 59 `DT_NEEDED` strings and those of every other ELF in the
tree all have a file or link with that exact name. It does **not** mean the
engine runs; it means nothing is missing at link time.

### 11.7 What was not run

* No boot, no QEMU, no Chromium process, no page, no frame.
* The full install path through `pkg` was not executed (it needs the guest):
  the checks `pkg` performs were read in `stage/usr/bin/pkg` and the package
  was verified against the same rules by hand (`manifest` present, `exec`
  exists and is 0755, no absolute links, index signature, sha256, exact
  unpacked size).
* `--dump-dom` against a real URL was not run; §7.3 gives the command for
  whoever boots it first.
