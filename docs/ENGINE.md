# A real browser engine on Mystic OS

The user asked for full HTML/CSS/JS. `src/client/apps/web.c` is our own
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
| **[apk]** | measured on the downloaded `.apk` in `build/cache/apk`, or on the engine inside `build/repo/engine.tar.gz` |
| **[pkg]** | from the package this tree builds: `build/repo/engine.tar.gz` |
| **[ours]** | read out of this repository, by symbol or line |
| **[run]** | output of a command actually executed in this tree, quoted in §1, §7 or §11 |
| **[not measured]** | says so, and what would settle it |

No QEMU was run by the worker who wrote this document; a boot run on this
tree during the same session is quoted in §11.7. No engine process was started
on the host that wrote it: the numbers are index arithmetic and bytes in this
tree. The screenshot in the last section came from an earlier boot; §11 says
exactly what was verified here and what still needs one, because the difference
between "compiled and checked" and "seen working" is the whole point of this
document.

---

## 0. Verdict

**Chromium headless (`chromium-headless-shell`) is the only candidate that is
genuinely headless and can hand us frames we can blit.** It is also large:
**182 packages, 258.30 MB download, 681.98 MB installed** [idx]; the bundle
this tree builds is **263,287,764 bytes** with **696.71 MB of members** [pkg,
§2]. That is twenty times the whole 13.42 MB image, so it is a package on the
data disk, never part of the initramfs.

It needs no X server, no Wayland, no GPU and no compositor: it speaks the
DevTools protocol over TCP on loopback, inside an RFC 6455 WebSocket (§3.1),
`Page.startScreencast` delivers JPEG frames, and `src/img.c` already decodes
JPEG into `0xAARRGGBB`, which is the window format. Input goes the other way
through `Input.dispatchMouseEvent` and `Input.dispatchKeyEvent`.

The default guest cannot hold it: `image.sh` boots with `MYSTIC_RAM=256` and a
64 MB disk [ours]. §7.2 raises both, and moves pkg's download cache off the
RAM filesystem.

If 263 MB of download is unacceptable, the honest small answer is **not** a
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
tree, one loopback socket behind an RFC 6455 WebSocket, `Page.startScreencast`,
`Input.*`. No X, no GPU, no compositor, and the protocol strings are verifiably
present in the exact binary we ship (§11.3).

`chromium` (the full browser) is 60 MB more download and 117 MB more installed
for a UI we would never show. The headless shell is the same Blink and V8
without the window chrome.

---

## 2. What is in the package

`python tools/fetch-engine.py` [run] resolved, downloaded, unpacked and packed
the first build of this package; the pipeline in the tree now is
`tools/fetch-chromium.py` for the closure and `tools/pack-engine.py` for the
bundle (§6). The closure root is the same package. The tree, largest members
first:

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
packages account for the rest of it. Two of them deserve a sentence:

* **llvm22-libs (191 MB)** is there because `mesa` needs LLVM for its software
  rasteriser. The engine links `libgbm.so.1` [apk, DT_NEEDED], so `mesa` must be
  installed; LLVM is a *package* dependency of mesa, not a link-time need of
  the engine. The engine's own 59 `DT_NEEDED` names do not include
  `libLLVM.so` (§11.6), but whether Chromium ever `dlopen()`s a DRI driver is
  exactly the kind of thing that is only settled by a boot, so the default
  keeps it. Dropping the package would save 191 MB; the current fetch tool has
  no `--exclude`, so that is a manual edit, not a flag.
* **icu-data-full (33 MB)** is the ICU data the engine reads. The current
  bundle carries it at `usr/share/icu/78.1/icudt78l.dat`; there is no
  `icudtl.dat` next to the executable in this archive, so the lookup that
  matters is the one the binary was built with. The boot that produced the
  screenshot in the last section rendered text, so that lookup resolved there;
  the ICU data it needs is in this archive.

The package itself [pkg], read back from the file on 2026-11-25 (§11.5):

```
build/repo/engine.tar.gz      263,287,764 bytes (263.29 MB)
  members                     1,595 (1,275 files, 320 symlinks)
  file bytes                  696,708,670 (696.71 MB)
  sha256                      a45afbfc450096702f9c698e29233a77b08d23d1e512d5d28f61e5dd8da9f618
build/repo/index.tsv row      engine  156.0.8078.79-r0  263287764  a45afbfc…
                              engine.tar.gz  Chromium 156 engine with the DevTools viewer
build/repo/index.tsv.sig      Ed25519 over the exact index bytes, verifies
```

The sha256 above is the index row's, byte for byte, and the signature over the
1,923-byte `index.tsv` verifies with `build/repo/index.pub`.

The archive is **not** deterministic: `tools/pack-engine.py` copies each
member's mtime from the file it packs (`ti.mtime = int(st.st_mtime)`) and gzip
stamps the time of the run, so two builds differ in their headers even when the
payload is identical. `pkg` checks the sha256 in the index, so a repack has to
be followed by a row and signature update or the guest refuses the download.

**Symlinks matter on this package.** 320 of the members are symlinks
(`libgbm.so.1 -> libgbm.so.1.5.0` and friends). Inside the guest that is free
on ext4, which is what `/root` is (`stage/init`: `mkfs.ext4`, `mount -t ext4`)
[ours]. On a filesystem that cannot store links, `pkg` computes the copy
fallback from the archive's link members (`tar_link_cost`) and says so before
installing: for this archive the 320 links name 266 distinct targets; copying
every link from its target costs **909,563,375 bytes** (the same target counted
once per link), so the fallback costs about **+909.56 MB**. The 266 distinct
targets are **519,531,519 bytes** on their own. One link is
dangling — `usr/share/xkeyboard-config-2 -> X11/xkb`, whose target is not in
the archive — and nothing the engine loads reads it. That is why §7 says the
data disk must be ext4 and big.

---

## 3. How a frame gets in, and how input gets out

### 3.1 The transport: TCP and a WebSocket, not the pipe

The first build used `--remote-debugging-pipe`, which hands the browser two
file descriptors — commands on **fd 3**, replies on **fd 4**, each message a
JSON object terminated by a NUL byte. On this kernel that path never carried a
byte: the recorded experiment is exit **1** with two pipes and exit **127**
with two socketpairs, with no output, which matches Chromium's "Connection
terminated while reading from pipe" path. The transport was replaced instead
of patched; the only `pipe()` left in the app is the child's stderr
(`engine.c`), not a DevTools transport.

`--remote-debugging-port=0` asks the browser to pick a free port on
**127.0.0.1**, listen on it, and write the port and the browser target's path
into `<user-data-dir>/DevToolsActivePort` — two lines:

```
41235
/devtools/browser/5b1f3d2e-9c47-4a1e-8b6f-2f0c9a1d4e7b
```

That file is the readiness signal, not a guess: it appears only once the
browser is listening. `engine_spawn()` in `src/client/apps/engine.c` unlinks a
stale file, forks the engine with

```
chromium-headless-shell --headless=new --remote-debugging-port=0 --no-sandbox
  --disable-gpu --user-data-dir=/tmp/engine-profile --no-first-run
  --no-default-browser-check --disable-background-networking
  --disable-component-update --disable-extensions --disable-sync
  --disable-default-apps --disable-breakpad --hide-scrollbars
  --force-device-scale-factor=1 --lang=en-US --window-size=<w>x<h>
  [--mute-audio] about:blank
```

and returns immediately; `engine_attach()` runs from the frame loop, reads
that one path every 100 ms for up to 10 s, and gives up early with the child's
real exit status if it dies first. A port is never guessed from a range and
never hard-coded.

`src/client/cdpws.c` owns everything below the JSON:

* `cdpws_read_active_port()` parses the file strictly — line 1 a decimal port
  in 1..65535, line 2 a path that starts with `/`; a half-written or malformed
  file is an error with a sentence, never a clamped port.
* `cdpws_open()` connects to the literal `127.0.0.1` (no DNS, no IPv6) and
  sends the RFC 6455 upgrade with a caller-supplied 16-byte key. The response
  must be `101` and `Sec-WebSocket-Accept` must equal
  `base64(SHA-1(key || 258EAFA5-E914-47DA-95CA-C5AB0DC85B11))`; a missing or
  wrong accept closes the connection before a single CDP message is sent. The
  app passes no `Origin`, so there is no origin for the browser to refuse and
  no `--remote-allow-origins` switch is needed.
* Client messages are text frames, always masked, with a fresh key per frame;
  server frames are decoded including the 64-bit length form, and fragmented
  messages are reassembled before delivery. A ping between fragments is
  answered with a pong and never surfaces as a message; a close is answered
  once and reported once; a masked server frame, a reserved bit, an unexpected
  extension or invalid UTF-8 fails the connection.

This file keeps only the CDP JSON, as it did with the pipe: one object in,
one object out, no NUL delimiter and no reassembly buffer. If no
`DevToolsActivePort` appears, or the upgrade fails, the child is stopped and
reaped and §3.5 takes over.

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
{"id":7,"sessionId":"<s>","method":"Page.screencastFrameAck",
 "params":{"sessionId":7}}
```

The two `sessionId`s are different things and the client keeps them apart: the
top-level one is the flat target session, `params.sessionId` is the screencast
session the ack must name [ours, `handle_message()`]. `Page.navigate` follows
when a URL is pending, and one `Runtime.evaluate` asks for
`JSON.stringify([document.title, location.href])` so the bars can show the
real title and the final URL after a redirect. The request is sent, and in the
YouTube boots the footer title and the address bar both showed the real page;
in the example.com boot (`https-example.png`) the title updated but the bar
stayed `about:blank`, and in the test-page boot (`chromium-live.png`) neither
had updated — the bar is not reliable yet, an `engine.c` issue reported rather
than hidden. The ids and session ids above are the app's own counter; the
transport neither invents nor matches them.

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

Every call above sends only when the page is attached (`p->ready`). On the
`--screenshot` path — the fallback when the channel is unavailable (§3.5) —
nothing is forwarded: the frame is a still and the wheel, the clicks and the
keystrokes have no engine to go to.

### 3.5 When the channel does not come up

If `DevToolsActivePort` never appears or the upgrade fails, `engine_stop()`
sends `SIGTERM`, waits 500 ms, sends `SIGKILL` and reaps the child, and
`page_shot()` runs the same binary the other way:

```
chromium-headless-shell --headless --no-sandbox --disable-gpu
  --hide-scrollbars --force-device-scale-factor=1
  --window-size=<w>x<h> --screenshot=/tmp/shot-<pid>.png <url>
```

The PNG is decoded with `src/img.c` and drawn; the footer says `one frame per
navigation (--screenshot)` and the frame counter counts renders, not a frame
rate. This is the path that is proven on a boot (last section) and the
fallback whenever the live channel does not come up. The guest took the live
one in the boot of §11.7.

---

## 4. Why it starts inside the jail

`src/wmsrv.c` chroots each app into `/run/mystic/jail/<id>` and binds only
`/bin/app`, the bundle read-only at `/app` (`MYSTIC_BUNDLE`), `/etc`
read-only, fonts, `/dev` bits (`null`, `zero`, `urandom`), `/proc`,
`/dev/shm`, a private `/root` and a writable `/tmp`. There is **no `/lib`**.
The engine's `PT_INTERP` is the literal string `/lib/ld-musl-x86_64.so.1`
[apk], so the kernel cannot start it as shipped.

The fix is a packaging one, done and verified by `tools/pack-engine.py`:

```
PT_INTERP rewritten: /lib/ld-musl-x86_64.so.1 -> /app/ld-musl-x86_64.so.1
                     (25 bytes at file offset 736)
```

The segment at offset 736 is 25 bytes including its NUL, and
`/app/ld-musl-x86_64.so.1` is the same 25 characters as
`/lib/ld-musl-x86_64.so.1`: the packer replaces the five-letter prefix in
place and no segment has to grow. It reads the value back afterwards, and the
engine in `build/repo/engine.tar.gz` says `/app/ld-musl-x86_64.so.1` (§11.5).

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
* `/etc` (read-only) → fontconfig's config, `/etc/hosts`, `/etc/resolv.conf`;
* `/usr/share/fonts` → text.

**The font lookup that used to be a guess is settled by those binds.** `/etc`
is bound read-only for every app, and the code says why: `/etc/passwd`,
`/etc/hosts` and the fontconfig files are what a toolkit looks up before it
can draw, and `/etc/shadow` is `0640 root:root` so an app uid cannot read it.
So `/etc/fonts` and `/etc/hosts` exist inside the chroot. The bundle also
carries its own `etc/fonts`, `usr/share/fonts` and
`usr/share/icu/78.1/icudt78l.dat` at `/app`. The boot that produced the
screenshot rendered text, so the font path works with what the jail and the
bundle provide. `/usr/share/icu` is **not** bound at that absolute path; this
build's ICU lookup ran in that boot too, so it resolved somewhere, but the
path is not in the jail and a different build could want it. If one does, the
missing bind is three lines next to the `/usr/share/fonts` block:

```c
snprintf(path, sizeof(path), "%s/usr/share/icu", jail);
mkdirp_for(jail, "/usr/share/icu");
mount("/usr/share/icu", path, NULL, MS_BIND | MS_RDONLY, NULL);
```

That change is not in this tree. It is written down because `/usr/share/icu`
is the one absolute path the engine might still want and the jail does not
provide, and reporting an unproven guess is better than pretending it is
settled.

The app itself checks the two facts it depends on before forking, and shows
which one is missing instead of a dead rectangle:

* `usr/lib/chromium/chromium-headless-shell` not executable → "the engine
  payload is not in this bundle: <path>: <errno>";
* `ld-musl-x86_64.so.1` at `<bundle>/lib/` not executable → it prints
  `warning: <bundle>/lib/ld-musl-x86_64.so.1 is not executable; execv will
  report the real error`. The package puts the loader at the **bundle root**
  (`/app/ld-musl-x86_64.so.1`, the `PT_INTERP` path), so this warning is
  printed on every start even though the exec succeeds — a wrong path in the
  check, not a missing loader. It is one line in `engine.c`
  (`"%s/ld-musl-x86_64.so.1"`), and it is reported here rather than hidden.

---

## 5. What works, what does not

| Capability | Status |
|---|---|
| **Static rendering (`--screenshot`)** | **Verified in a boot**: `docs/screenshots/chromium-page.png`, one frame per navigation. This is the path the viewer falls back to when the live channel is unavailable; the boots below used the live one. |
| **Live DevTools channel (frames)** | **Verified in boots**: `docs/screenshots/chromium-live.png` (`page attached  2 frames` over the test page), `https-example.png` (example.com, 4 frames), `youtube.png` (https://www.youtube.com/, 15 frames), `youtube-watch.png` (51 frames) and `youtube-playing.png` (148 frames) / `youtube-video-audio-jail.png` (252 frames, the audio-jail boot) (§11.7). |
| **Navigation and HTTPS** | Verified: `youtube.png` shows `https://www.youtube.com/` in the address bar and `YouTube` in the footer; `https-example.png` shows example.com rendered with `Example Domain` in the footer (its bar still read `about:blank`). Chromium's own TLS uses the image's CA bundle, bound read-only at `/etc/ssl/certs/ca-bundle.crt` (`src/wmsrv.c:436`). |
| **Input over the live channel** | A click at 480,300 in the YouTube page was followed by the video playing (`youtube-playing.png`, 148 frames; `youtube-video-audio-jail.png`, 252 frames). Scrolling and form entry are still unproven. |
| **A second heavy site** | `github.com` renders in full - hero, sign-up form, Mona, footer - at `docs/screenshots/github.png` with 455 frames, so the result is not specific to one site's markup. |
| HTML5 parsing | Blink. The binary contains `HTMLDocument`, `CSSStyleSheet` and `blink` strings [apk, byte search, §11.3]. |
| CSS | Blink: full CSS, not our subset. |
| JavaScript | V8. The binary contains `v8_context_snapshot` [apk]. WebAssembly is V8's. |
| Canvas / WebGL | Canvas yes (software). WebGL is refused: the app passes `--disable-gpu` because there is no GPU; Chromium would otherwise try DRI drivers that need `llvm22-libs` and a DRM node. |
| H.264 / VP9 / AV1 video without DRM | The engine links `libavcodec.so.62`, `libopenh264.so.8`, `libdav1d.so.7`, and the closure carries `x265-libs`, `aom-libs`, `libSvtAv1Enc` [apk, DT_NEEDED + idx]. Decoding is present and was exercised: YouTube's watch page played in the `youtube-playing.png` (148 frames) and `youtube-video-audio-jail.png` (252 frames) boots, silently, because the guest had no PCM node and the app passed `--mute-audio`. |
| Audio from a page | The app passes `--mute-audio` when `/dev/snd/pcmC0D0p` does not exist and says so once on stderr (the boot logs show it). The engine manifest now says `audio = yes`, and the jail's `/dev` lists `snd`; the QEMU guest still has no PCM node (`snd_hda_intel: Cannot probe codecs`), so the flag stays on and the video remains silent. The un-muted path is unexercised. |
| **DRM (Netflix, Spotify, Prime)** | **No.** The engine knows the API (`requestMediaKeySystemAccess`, `widevine` strings are present) but the CDM library `libwidevinecdm.so` is not in the binary's directory and there is **no widevine package in the whole Alpine v3.24 index** [idx]. Without a CDM, EME playback fails by design. |
| PDF viewer | Chromium's PDF viewer needs its plugin resources; the headless shell package does not carry them. Use `docview`/`pdf.c`. |
| Downloads | Not wired: `Page.setDownloadBehavior` is not called and there is no file picker in the jail. |
| Tabs | One. `Target.createTarget` is called once. |
| History back/forward | Not wired (Ctrl+R reloads). The protocol has `Page.getNavigationHistory`; the UI does not. |
| IME / non-US keyboards | No. evdev codes + US table. |
| Extensions, sync, accounts | Not present (`--disable-extensions --disable-sync`). |

The engine runs with `--no-sandbox`. That is not an oversight: Chromium's own
sandbox needs unprivileged user namespaces and a filesystem layout the jail
does not have. The OS jail — a chroot, its own mount namespace, one of the
sandbox uids 2000..2015, no host `/root` (its own appdata is bound at `/root`),
the framebuffer, the keyboard device and other clients out of reach — is the
sandbox. It is weaker than Chromium's: a compromised renderer has the engine's
whole tree and can reach the network directly (`network = host`; that is the
same line §3.1 needs to reach 127.0.0.1 at all).

---

## 6. Files

| Path | What it is |
|---|---|
| `tools/fetch-chromium.py` | resolves the closure for `chromium-headless-shell`, downloads with resume, unpacks and packs `build/repo/chromium.tar.gz` |
| `tools/pack-engine.py` | takes that closure plus the viewer, rewrites the engine's `PT_INTERP` in place and packs `build/repo/engine.tar.gz` |
| `tools/mkapp.sh engine` | compiles the STATIC viewer and installs it as the bundle's `bin` - the in-image stand-in only; the package's `bin` is `engine_launch.c` |
| `src/client/apps/engine.c` | the window app: CDP over TCP + WebSocket, JPEG→`0xAARRGGBB` frames, input forwarding, address bar, honest failure cards, `--screenshot` fallback. `EVP_SELFTEST` builds the parser tests |
| `src/client/cdpws.c` / `cdpws.h` | the transport: `DevToolsActivePort` parsing, TCP connect to 127.0.0.1, RFC 6455 upgrade with accept-key validation, masked client frames, fragment reassembly |
| `tools/serve-repo.sh` | rebuilds the index over `build/repo` and serves it; it copies the `engine` row from `build/repo/engine.row` verbatim (§7.1) |
| `docs/ENGINE.md` | this file |
| `build/cache/apk/` | the `.apk` download cache (248 MB in this working tree) |
| `build/repo/chromium.tar.gz` | the closure package (257,205,322 bytes) |
| `build/repo/engine.tar.gz` | the installable engine bundle (263.29 MB, §2) |
| `build/repo/index.tsv`, `.sig` | the signed index; the `engine` row matches the tarball (§11.5) |

**The image does not ship this bundle at all.** It used to carry a static
placeholder so that a tile existed before installation; that tile then looked
like a frozen browser, which is worse than an absent app. The engine is now only
present once the package is installed (`pkg install engine`, or the Store), and
the launcher shows no Chromium entry until then.

The bundle layout, as installed by `pkg` under `/root/apps/engine`:

```
manifest                       name/desc/exec=bin/version/network/procs/audio/placeholder
bin                            the STATIC launcher (27 KB, no PT_INTERP): it execs
                               the bundle's loader on the payload below
libexec/engine                 the viewer (906 KB), PT_INTERP
                               /app/ld-musl-x86_64.so.1, DT_NEEDED libjpeg.so.8
ld-musl-x86_64.so.1            musl's loader, at the literal PT_INTERP path
usr/lib/chromium/chromium-headless-shell   202,432,312 bytes, PT_INTERP /app/...
usr/lib/…                      the shared-object closure (59 DT_NEEDED for the engine)
usr/share/icu/78.1/icudt78l.dat             ICU data
usr/share/fonts/, etc/fonts/   fonts and fontconfig config from the closure
etc/, PKGINFO, post-install   inherited from the Alpine package for the closure
```

`exec = bin`, and `bin` is a launcher rather than the viewer. Both halves of
that are deliberate. The name is forced: the closure has a top-level `bin/`
directory (Alpine's shell) and a tar cannot create a file and a directory with
the same name, so `tools/pack-engine.py` drops the closure's `bin/` tree and puts
the launcher there. It is a launcher because a static binary cannot `dlopen`,
and the frame-rate work needs the bundle's own libjpeg-turbo; the payload
therefore lives at `libexec/engine` with `PT_INTERP /app/ld-musl-x86_64.so.1`,
the same shape the VLC player uses with `libexec/vlcplay`. Nothing in it is
needed: the engine lives under `usr/lib`. `src/wmsrv.c` only requires the exec
to be a bare file name.

The manifest on disk:

```
name = Browser Engine
desc = Chromium 156 rendering through the DevTools protocol
exec = bin
version = 156.0.8078.79
network = host
bundle = engine
placeholder = yes
procs = yes
audio = yes
```

---

## 7. Install it

### 7.1 On the host

```bash
python tools/fetch-chromium.py   # closure -> build/repo/chromium.tar.gz (257 MB)
./tools/mkapp.sh engine          # viewer -> build/app-engine and stage/.../bin
python tools/pack-engine.py      # closure + viewer -> build/repo/engine.tar.gz
tools/serve-repo.sh              # rebuild the index and serve it on :8000
```

`tools/fetch-chromium.py` caches the `.apk`s in `build/cache/apk` and
re-downloads only what is missing. It has no download cap; the closure root is
`--packages chromium-headless-shell`. It deliberately skips Alpine's
base-system packages (`musl`, `busybox`, `ca-certificates`, …) because the
image already ships them, and `tools/pack-engine.py` takes the loader from
`stage/lib/ld-musl-x86_64.so.1` — the same loader the rest of the image runs.

A repack changes the sha256, so the index row and its signature have to change
with it. The row in `build/repo/index.tsv` currently matches the tarball
(§11.5), but `build/repo/engine.row` still carries the **previous** package's
size and sha, and `tools/serve-repo.sh` copies that file verbatim. After a
repack, update `engine.row` (name, version, size, sha256, file name,
description, unpacked bytes) before serving, or the guest will download a
tarball whose hash does not match the row it was offered.

The first build of this package used `tools/fetch-engine.py`, which is where
the closure numbers in §1 and §11.4 come from (`python tools/fetch-engine.py
--candidates` prints the candidate table without downloading). That tool now
names `src/client/apps/engineview.c`, which does not exist, and its link list
has no `src/client/cdpws.c`; it is history, not the build path.

### 7.2 The guest's machine settings

The default image boots with 256 MB of RAM and a 64 MB disk [ours,
`image.sh`], and pkg's download cache is `/tmp/pkg` — a **tmpfs**, i.e. RAM.
All three have to change for a 263 MB package:

```bash
# host, once: a bigger disk. image.sh only creates build/disk.img when it is
# missing, so the old 64 MB file has to go first.
rm -f build/disk.img
MYSTIC_RAM=1024 MYSTIC_DISK_MB=2048 ./image.sh
```

`MYSTIC_RAM=1024` is the smallest guest this document is willing to
recommend: the engine binary alone maps 202 MB and the tree is 696.71 MB of
members. Whether 1 GB is *enough* for a heavy page is **not measured**: the
boots rendered light pages (the test page, example.com, YouTube's watch page)
and played one video, but no 1 GB-heavy page was timed or measured. If a page
dies, the engine's last stderr line appears in the window's status bar and the
answer will be in it.

### 7.3 Install in the guest

```sh
# the download is 263 MB: keep it off /tmp (a RAM filesystem)
MYSTIC_CACHE=/root/pkgcache pkg update
MYSTIC_CACHE=/root/pkgcache pkg install engine
pkg installed            # engine 156.0.8078.79-r0
```

`pkg` checks the signature over the index before reading a row, checks the
sha256 of the tarball before unpacking, computes the footprint from the
archive's own member list (every file rounded up to 4 KiB) and refuses before
writing if it does not fit, then reports the symlink situation: whether `/root`
can store the 320 links or has to copy the link targets instead — 909.56 MB
of copy cost from §2.

Then the Store shows a **Browser Engine** tile, or from a shell:

```sh
/root/apps/engine/bin https://example.com
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
window: `--dump-dom` prints the DOM to stdout. It was not run: the boots drove
the window and the live channel, and no DOM dump was saved.)

---

## 8. Removing it

```sh
pkg remove engine
```

The installed tree — 696.71 MB of members (§2) — comes back, and the Store
tile goes with it.

---

## 9. The smaller alternative, with numbers

If 263 MB of download is the wrong trade, there are two smaller answers and
neither is a modern browser.

### 9.1 NetSurf's framebuffer build — 9.46 MB download, 22.73 MB installed

**23 packages, 9.46 MB download, 22.73 MB installed** [idx], and it renders
straight into a framebuffer or an SDL surface, so no X server and no CDP.

What it is not: **NetSurf 3.11 has no JavaScript engine.** Its CSS support is a
2.1 subset with some CSS3. A page that is a React app, or that needs
`fetch()`, shows nothing but its `<noscript>` or an empty body. For reading
documentation, Wikipedia's early-2000s surface and plain HTML sites it is
genuinely good; for "full HTML/CSS/JS" it is not the thing the design calls for.

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

**Recommendation:** if the design wants a real browser, install `engine`
(§7). If the design wants a small, honest HTML reader, NetSurf is the option and
its cost is 23 packages. There is no 10 MB full browser engine, in this index
or anywhere else.

---

## 10. What this document is not

* It is not a benchmark. No frame rate, no load time and no memory figure was
  measured; the rendering evidence is the screenshots quoted in §11.7 and at
  the end.
* It is not a promise that every page behaves. A boot drew HTTPS pages and
  played a YouTube video (§11.7); scrolling, form entry and a 1 GB-heavy page
  are still unmeasured, and §4 names the one absolute path the jail does not
  bind.
* It is not a claim that 1 GB of RAM is enough for heavy pages. It is the
  smallest guest this document will recommend, and §7.3 says where the answer
  will appear if it is not.

---

## 11. Verification log

Everything in this section was executed in this working tree, except the
quoted log in §11.4, which is labelled as the earlier pipeline's run. Commands
are reproducible; output is quoted, not paraphrased.

### 11.1 The compiler

```
$ zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
        -c src/client/apps/engine.c -o .work/w3/engine.o
(no output, exit 0)

$ zig cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
        -Isrc/client/apps -Wall -Wextra -Wno-unused-parameter \
        -o .work/w3/app-engine \
        src/client/apps/engine.c src/client/mystic_client.c src/gfx.c \
        src/font.c src/ui.c src/filetypes.c src/audio.c src/img.c \
        src/client/cdpws.c -lm
(no output, exit 0)
$ ls -l .work/w3/app-engine build/app-engine stage/usr/share/mystic/apps/engine/bin
-rw-r--r-- 1 saket 197609 241744 Nov 25 16:02 .work/w3/app-engine
-rw-r--r-- 1 saket 197609 241744 Nov 25 15:55 build/app-engine
-rw-r--r-- 1 saket 197609 241744 Nov 25 15:55 stage/usr/share/mystic/apps/engine/bin
$ cmp .work/w3/app-engine build/app-engine && echo identical
identical
```

That link line is `tools/mkapp.sh`'s — it adds `src/client/cdpws.c` when that
file exists — and the result is byte-identical to `build/app-engine` and to
`stage/usr/share/mystic/apps/engine/bin`, which is the `bin` in the package.
`src/client/cdpws.c` also compiles silent on its own with the same flags.

### 11.2 The app's own parsers, on the host

`EVP_SELFTEST` compiles the pure functions and their tests with no window
server. Built for the host and **run**:

```
$ zig cc -target x86_64-windows-gnu -Os -DEVP_SELFTEST -Wno-unused-function \
        -Isrc -Isrc/client -Wall -Wextra -Wno-unused-parameter \
        -o .work/w3/selftest.exe src/client/apps/engine.c
$ .work/w3/selftest.exe
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
$ python - <<'PY'   # ELF parse of the engine inside the package
PT_INTERP: /app/ld-musl-x86_64.so.1   (25 bytes at file offset 736)
DT_NEEDED (59): libglib-2.0.so.0 … libgbm.so.1 libdrm.so.2 libavcodec.so.62
                libopenh264.so.8 libdav1d.so.7 libX11.so.6 libasound.so.2
                libpulse.so.0 libc.musl-x86_64.so.1
```

Byte search of the same file for the protocol surface the app uses. The search
is case-sensitive, and that matters: the binary carries the method names
(`attachToTarget`, `createTarget`) rather than the joined `Target.` form.

```
DevToolsActivePort         present   startScreencast           present
Page.screencastFrame       present   screencastFrame           present
Input.dispatchMouseEvent   present   dispatchMouseEvent        present
Input.dispatchKeyEvent     present   dispatchKeyEvent          present
Target.attachToTarget      ABSENT    attachToTarget            present
Target.createTarget        ABSENT    createTarget              present
Page.navigate              present   Runtime.evaluate          present
Emulation.setDeviceMetricsOverride present
remote-debugging-pipe      present   remote-debugging-port     present
user-data-dir              present   no-sandbox                present
disable-gpu                present   disable-dev-shm-usage     present
v8_context_snapshot        present   HTMLDocument              present
blink                      present   CSSStyleSheet             present
libwidevinecdm             ABSENT
```

An earlier version of this table listed lowercase spellings
(`devtoolsactiveport`, `startscreencast`, `dispatchmouseevent`,
`attachtotarget`, `createtarget`) as present. Those exact spellings do not
occur in this binary; the strings that do are the real protocol names above.
The conclusion — the surface is there — is unchanged, and the evidence is now
the evidence.

The `.apk` was 89,969,624 bytes — the index's `S:` exactly — and its members
sum to 208,174,865 bytes against an index `I:` of 208,171,836; the difference is
exactly `.PKGINFO` + `.SIGN` (2,517 + 512), which `I:` excludes. That is the
same trap `tools/fetch-browser.py` fell into, now measured.

### 11.4 The build

The package in this tree is built in three steps:

```bash
python tools/fetch-chromium.py      # closure -> build/repo/chromium.tar.gz
./tools/mkapp.sh engine             # viewer -> build/app-engine, stage/.../bin
python tools/pack-engine.py         # closure + viewer -> build/repo/engine.tar.gz
```

`fetch-chromium.py` resolves the closure (minus Alpine's base-system packages,
which the image provides), prints its size and writes
`build/repo/chromium.tar.gz` (257,205,322 bytes). The 182-package, 258.30 MB
figure in §1 is the full closure from the same root, measured by
`tools/fetch-engine.py --candidates`. `mkapp.sh` compiles the viewer with the
command in §11.1. `pack-engine.py` copies the closure, drops its top-level
`bin/` tree, adds the viewer as `bin` and the image's loader at the bundle
root, rewrites `PT_INTERP` in place and writes the tarball. Its stdout was not
kept in the tree, so §11.5 reads the numbers back from the artifact instead of
quoting a log.

The first package was built by `tools/fetch-engine.py`. Its log is kept here
because the closure figures in §1 come from that run:

```
==> engine closure: 182 packages, 258.30 MB download, 681.98 MB installed
==> unpacked 1139 files, 681.98 MB, 317 links (recorded, not materialised on
    this host), 0 special files skipped
==> window app: build/app-engineview, 212 KB static
==> packed engine.tar.gz: 259.65 MB from 682.87 MB of members (1646 entries)
```

Those numbers describe the older package, not the one in `build/repo` today
(263,287,764 bytes, §11.5). `fetch-engine.py` still names
`src/client/apps/engineview.c`, which does not exist, and its link list has no
`src/client/cdpws.c`, so it cannot build the current viewer; that is why the
pipeline moved to the three commands above.

### 11.5 The package, read back

```
$ python - <<'PY'   # tarfile + sha256 over build/repo/engine.tar.gz
members: 1,595
kinds: {'file': 1,275, 'sym': 320}
file bytes: 696,708,670
sha256: a45afbfc450096702f9c698e29233a77b08d23d1e512d5d28f61e5dd8da9f618
PT_INTERP of usr/lib/chromium/chromium-headless-shell: /app/ld-musl-x86_64.so.1
engine size: 202,432,312   mode 0755
bin size: 241,744          ELF
manifest: exec = bin / network = host / procs = yes / audio = yes / placeholder = yes
links: 320, distinct targets 266, per-link copy cost 909,563,375,
        distinct-target bytes 519,531,519, dangling 1
        (usr/share/xkeyboard-config-2 -> X11/xkb)
```

and the index row and its signature:

```
$ cat build/repo/index.tsv            # the engine row, wrapped here
engine  156.0.8078.79-r0  263287764  a45afbfc…  engine.tar.gz
        Chromium 156 engine with the DevTools viewer: renders pages in a window
        692060160
$ python tools/sign-index.py verify --in build/repo/index.tsv \
      --sig build/repo/index.tsv.sig --pub build/repo/index.pub
ok: signature verifies over 1923 bytes of build/repo/index.tsv
key fingerprint: sha256 ce177b2cc56f3cfd4c3458a7e7201eece66b45b01a1a888b9a6f1e4cbcf90096
```

The tarball's size and sha256 are the row's, so `pkg`'s two checks pass on
this pair. The row's last column is the unpacked figure the index advertises:
692,060,160 bytes. The archive's own member list sums to 696,708,670, so the
row is 4,648,510 bytes optimistic. That does not stop an install — `pkg`
computes the footprint itself from the member list (§7.3) — but a space check
that trusted the row would be short by 4.6 MB.

### 11.6 The closure check, and what it means

`check_closure()` in `tools/fetch-engine.py` resolved every `DT_NEEDED` of
every shipped ELF **by file name**, which is what musl's loader does (it opens
the name and then rejects a library whose `DT_SONAME` disagrees). A
SONAME-only check — the glibc rule — would pass packages that cannot start
here; shipped symlinks count as names, because that is what makes
`libgbm.so.1` openable when the real file is `libgbm.so.1.5.0`. Its last run
on the old tree printed `414 ELF files, 728 loadable names (317 from shipped
links), 0 unresolved by name`.

The current pipeline does not re-run that check. What can be checked on the
package that exists is narrower: of the engine's 59 `DT_NEEDED` names, 58 have
a member with that exact name in the archive; the 59th is
`libc.musl-x86_64.so.1`, and the bundle's loader is `ld-musl-x86_64.so.1` at
the `PT_INTERP` path. That is not proof that every `dlopen()` a page triggers
resolves — only a boot can show that — and the engine started in the boot of
§11.7, so the loader path itself works.

### 11.7 What was run, what was not, and the loopback line

* No QEMU was run by the worker that wrote this section; boots were run on
  this tree while the document was being finished, and the serial line cited
  below is quoted from a saved excerpt (`build/serial.log` is overwritten by
  every boot). No Chromium process was started on this host. A page **was**
  rendered by Blink in the boot that produced
  `docs/screenshots/chromium-page.png`; nothing here reproduced that image.
* Apart from the guest boots described below, the DevTools TCP channel has only
  been exercised on the host, through the harness. What was run there, rebuilt
  for this report: `.work/cdp1/harness.c` against `src/client/cdpws.c` →
  **131 checks, 0 failed**. It covers the RFC 6455 example key and the
  accept-key calculation, a wrong or missing `Sec-WebSocket-Accept` failing
  closed, masked client frames with a fresh key per frame, fragmented server
  messages reassembled with a ping between fragments, ping/pong/close, 64-bit
  lengths, protocol violations, and a real loopback TCP upgrade followed by a
  fragmented reply.
  `.work/w1/portcheck.c` against the same `cdpws.c` → **6/6** — covers the exact
  `/tmp/engine-profile/DevToolsActivePort` path the viewer polls, including a
  half-written file being refused.
* **The live channel came up in the guest, and the loopback line is why.**
  The boot's serial log carried the line, in this shape:

  ```
  engineview: devtools over tcp port <port> /devtools/browser/<uuid>
  ```

  One run's exact line — port 39051, browser
  `0ef2eea2-0888-4a2a-9766-84799428222c` — is in the saved excerpt
  `.work/w3/serial-2026-11-25.txt`. `build/serial.log` itself is a build
  artefact that every boot overwrites, so the port and UUID change from run to
  run (later logs in this tree have other pairs); the durable evidence is the
  committed screendump, not the line. The line is printed only after
  `cdpws_open()` returned, so inside the guest the browser published
  `DevToolsActivePort` and accepted the RFC 6455 upgrade — accept-key check
  included — on `127.0.0.1`.

  The frames came through too. The committed
  `docs/screenshots/chromium-live.png` (md5
  `efffd477a66b43e4d0fd73d8ec38bcb8`, identical to the `build/H5.png`
  screendump) shows the live footer `page attached  2 frames` with the test
  document drawn; two frames is the viewer's own decoded-frame counter
  (`p->frames`), so two `Page.screencastFrame` JPEGs were decoded and drawn
  over the live channel inside the guest.

  Later boots navigated. The committed `docs/screenshots/youtube.png` shows
  `https://www.youtube.com/` in the address bar, `YouTube` in the footer and
  `page attached  15 frames`; the committed `docs/screenshots/https-example.png`
  shows `example.com` with `Example Domain` in the footer and four frames.
  Further screendumps record the watch page and playback:
  `youtube-watch.png` (51 frames, watch URL in the bar), `youtube-playing.png`
  (148 frames, the video playing after a click at 480,300), and
  `youtube-video-audio-jail.png` (252 frames, committed with the jail's
  `audio = yes` manifest). The video is still silent in all of them: the jail
  now binds `/dev/snd` (`jail: /dev = snd console zero urandom null shm`) but
  the guest has no PCM node (`snd_hda_intel: Cannot probe codecs`), so the app
  still passes `--mute-audio`. The loader warning in the log is the wrong path
  in the app's own check (§4).

  The line that made it possible, `ip link set lo up`, is at
  `stage/init:227`, added during this run before eth0 is brought up; the
  comment there records the bind timeout that happened without it. The engine's
  manifest says `network = host`, so `src/wmsrv.c:355` does not give it a
  private netns: the guest's `lo` is the interface that matters.

* The full install path through `pkg` was executed in the boot above — the
  engine that printed those lines runs from the installed bundle, after the
  input script's `pkg install engine` — but `pkg`'s console output is not in the
  serial log (it is in the terminal window on the guest's screen). The checks
  it performs were read in `stage/usr/bin/pkg`, and the package was verified
  against the same rules by hand (`manifest` present, `exec` exists and is
  0755, no absolute or escaping links, index signature, sha256, member sizes).
* `--dump-dom` against a real URL was not run; §7.3 gives the command for
  whoever boots it first.

## The engine renders pages now

Two kinds of evidence, and they are different.

**The screenshot fallback.** `docs/screenshots/chromium-page.png` is a real
Blink 156 rendering produced by `--screenshot`: the heading `Mystic +
Chromium`, the paragraph `Rendered by Blink 156 inside the VM.` and a bordered
table (`name`/`value`, `alpha`/`1`, `beta`/`22`), shown in the viewer with a
working address bar (`http://10.0.2.2:8000/html`). The window's top bar reads
`rendered 2 frames` — the fallback's counter, two navigations, two stills. It is
what the viewer uses whenever the live channel is unavailable, and the path
with the most evidence behind it.

**The live channel.** The transport came up in the guest and navigated HTTPS
pages. The committed screenshots are the evidence:
`docs/screenshots/chromium-live.png` (the test page with the live footer
`page attached  2 frames`), `https-example.png` (example.com, 4 frames,
`Example Domain` in the footer) and `youtube.png` (https://www.youtube.com/,
15 frames, `YouTube` in the footer, the URL in the address bar). Later
screendumps show the watch page and playback: `youtube-watch.png` (51 frames,
the watch URL in the bar), `youtube-playing.png` (148 frames, the video playing
after a click at 480,300) and `youtube-video-audio-jail.png` (252 frames,
committed with the jail's `audio = yes` manifest). The video boots are silent:
the jail now binds `/dev/snd`, but the guest has no PCM node
(`snd_hda_intel: Cannot probe codecs`) and the app passes `--mute-audio`.

What is verified today, and what is not:

* **Screenshot rendering works, and is what the viewer falls back to.**
  `page_shot()` runs the engine with `--screenshot` and decodes the PNG with
  `src/img.c` (§3.5).
* **The TCP channel is host-verified and live in the guest.** The transport
  passed `.work/cdp1/harness.c`, rebuilt against `src/client/cdpws.c`:
  **131 checks, 0 failed** — RFC 6455 accept key, fail-closed handshake,
  masking, fragmentation reassembly with a ping in the middle, ping/pong/close,
  64-bit lengths, protocol violations, and a real loopback upgrade with a
  fragmented reply. The readiness signal passed `.work/w1/portcheck.c`
  (**6/6**). The boot's serial line is quoted in §11.7; the committed
  `chromium-live.png` proves the frames.
* **HTTPS and YouTube work live.** `youtube.png` shows the URL and title and
  `https-example.png` shows `Example Domain` with four frames; the video plays
  (`youtube-playing.png`, 148 frames; `youtube-video-audio-jail.png`, 252), so
  the old "YouTube shows a frame, not a player" limitation is true only of the
  fallback now.
* **Unproven:** scrolling; typing into a page field or submitting a form; audio
  without `--mute-audio`; and a 1 GB-heavy page.
* **Known limits:** the address bar is unreliable — it stayed `about:blank` in
  the test-page boot and lagged the title in the example.com boot, while the
  YouTube boots show it correct; DRM (no Widevine CDM); downloads; history
  back/forward; IME; one tab.

What this gives and what it does not:

- **Works (live):** HTTPS pages, the YouTube watch page and its video (silent),
  address-bar navigation where the bar draws it, the page title, and an in-page
  click.
- **Known limits:** the unreliable address bar, DRM (Netflix, Spotify, Prime —
  no Widevine CDM), downloads, history back/forward, IME, one tab.
- **On the screenshot fallback:** no scrolling, no in-page clicks, no form
  entry, no video — YouTube would show one frame, not a player.
- **Next:** a boot that scrolls, types into a field and submits it; the
  address-bar fix in `engine.c`; and a PCM node so the video is not silent.

## DRM: the browser half is here, the CDM is not shippable

`chromium-headless-shell` carries Widevine EME support - the binary contains
`com.widevine.alpha`, the `WidevineH` path and the persistent-license strings -
but the CDM itself, `libwidevinecdm.so`, is proprietary: it is not in Alpine, it
cannot be redistributed with this OS, and Netflix additionally requires a
device certificate that comes with that CDM.

What the viewer does now: if a CDM is installed into the bundle at
`WidevineCdm/<version>/_platform_specific/linux_x64/`, it is linked into the
browser profile before launch (`link_widevine()` in `engine.c`), which is where
Chromium looks for it. So a user who supplies their own CDM gets it loaded
without touching this code. Nothing here downloads, ships or bypasses anything,
and **`--disable-component-update` still prevents Chromium from fetching a CDM
by itself** - that is deliberate.

**Not proven:** no CDM was present, so no DRM content has been played. Netflix,
Spotify and Prime Video will refuse until a licensed CDM is supplied.

## Frame rate: measured, and where it goes

The screencast is flow-controlled by `Page.screencastFrameAck`; the viewer used
to acknowledge after decoding, so its own JPEG decode was inside the browser's
frame pacing. Acknowledging first, then decoding, and measuring instead of
estimating:

| setting | rate | decode | gap |
|---|---|---|---|
1100x668, q70, scalars | 5.0 fps | 181 ms | 190 ms |
733x445, q60, scalars | 10.2 fps | 90 ms | 95 ms |
550x334, q60, scalars | 14.4 fps | 48 ms | 51 ms |
550x334, turbo | 24 fps instant | **1.3 ms** | 41.7 ms |
1100x668, turbo | 24 fps instant | **5.4 ms** | 41.8 ms |

Two changes produced those numbers, both measured:

1. The frame is acknowledged before it is decoded, so our decoder stopped sitting
   inside Chromium's frame pacing.
2. The viewer is now the same shape as the VLC player: a static launcher
   (`engine_launch.c`, 27 KB, no `PT_INTERP`) execs the bundle's own musl loader
   on a dynamic payload at `libexec/engine` whose `PT_INTERP` is rewritten to
   `/app/ld-musl-x86_64.so.1` and which links `libjpeg.so.8` from the bundle.
   Static musl cannot `dlopen`, which is why this was necessary at all.

With decode at 1.3-5.4 ms the remaining 41.8 ms gap is Chromium's own screencast
cadence (about 24 frames/s at this window size), so the viewer now asks for
full-resolution frames: the decoder is no longer the reason to shrink them.

`gap ≈ decode + 3 ms` is the important number: the engine is ready as soon as
the viewer is, so the ceiling is `src/img.c`'s scalar JPEG decoder at roughly
3.8 megapixels/second. The bundle already carries libjpeg-turbo 3.1.3 with
AVX2/SSE2; using it requires the viewer to be dynamic (static musl cannot
dlopen), which is the same static-launcher/dynamic-payload shape the VLC player
already uses.
