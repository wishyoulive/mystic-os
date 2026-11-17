# Running a real browser engine on Mystic OS

There is no X11 and no Wayland here. The compositor owns `/dev/fb0`, draws with
its own CPU rasteriser (`src/gfx.c`, `src/fb.c`) and reads `evdev`. Writing a
layout engine is out of the question, so the only question this document
answers is:

> Which existing engine can we run, as a separate process, without writing an
> X server or a Wayland server — and what does each one actually give us?

Everything below is checked against something outside this repository: the
Alpine v3.24 package index (the exact branch `tools/fetch-kernel.sh` and
`image.sh` use), upstream source files, and upstream documentation. Where a
number is arithmetic on those facts it says so. Where it is a guess that has not
been measured on our machine, it says *estimate, unmeasured* and says what would
settle it. Every one of those is marked; everything else is either quoted from
upstream or arithmetic you can repeat from the numbers given.

Nothing in this document has been booted. No browser has been run in Mystic.
That is the point of a plan.

---

## 0. How to read the evidence tags

| Tag | Meaning |
|---|---|
| **[pkg]** | measured from the Alpine v3.24 `APKINDEX` for x86_64 (instance of 2026-11-06), i.e. what `apk add` would actually pull |
| **[src]** | quoted from upstream source at the commit/version named |
| **[doc]** | quoted from upstream documentation |
| **[ours]** | read out of this repository, with `file:line` |
| **[calc]** | arithmetic on the above, shown so it can be re-checked |
| **[est]** | estimate. Not measured. Each one says what would settle it |

Two different sizes appear below and they are never mixed:

- **package size** — the `.apk` download and the `I:` installed size of *that
  package alone*.
- **closure size** — the union of a package and everything it transitively needs,
  resolved through `so:`/`cmd:`/versioned provides in the index.

Wherever a cost is being argued, it is a **closure**, because a closure is what
lands on the disk. That distinction is not pedantry: it is the difference
between "Xvfb costs 268 MB" (wrong) and "Xvfb costs 7.6 MB on top of Chromium"
(right), and the two answers point at different shortlists. Union closures in
§5.5 and §7.1 were computed by resolving the *set* of roots together, so shared
dependencies are counted once.

Closures come from resolving `D:` fields in the index, not from installing; a
real `apk add` may pull a few extra `cmd:` or virtual packages.

Everything in this repository was re-read against the working tree at commit
`efd54b2`. Files that other work is actively editing (`src/main.c`, `image.sh`,
`stage/init`) are cited by symbol with the line number at that commit, because
their line numbers have already moved twice during this document's own writing —
the `print` in §1 is exactly that mistake, caught in review.

---

## 1. The machine we are actually targeting

| Fact | Value | Source |
|---|---|---|
| Guest RAM | **256 MB** | `image.sh:54` → `-m 256` |
| Disk | **64 MB**, FAT, formatted on first boot | `image.sh:51` → `dd ... count=64`, `stage/init` |
| Rootfs today | 29 MB staged, 6.5 MB compressed | `du -sh stage`, `README.md` |
| Machine size is a **knob**, not a constant | `MYSTIC_RAM` (default 256) and `MYSTIC_DISK_MB` (default 64) | `image.sh:50-54` |
| Screen | 1280×800, 32 bpp, stride from `FBIOGET_FSCREENINFO` | `src/fb.c:29-49`, `README.md` |
| Pixel format | `0xAARRGGBB` in a `u32`; on little-endian that is byte order **B,G,R,A** | `src/mystic.h:17-21`, `docs/IPC.md` |
| Compositor buffers | two full-screen `calloc` surfaces (~4.1 MB each at 1280×800) | `src/main.c` `main()`, `screen.px`/`scene.px` (1172/1174 at `efd54b2`) |
| Window send path | text protocol on `/run/mystic/wm.sock` + shm in `/dev/shm` (tmpfs) | `src/ipc.h`, `src/wmsrv.c`, `stage/init` |
| Max window | 80..1600 × 60..1200, clamped | `src/wmsrv.c:114-117` |
| Stride sent to apps | `w` exactly | `src/wmsrv.c:133` |
| Frame semantics | `FRAME` = whole window, **no damage rectangles** | `src/ipc.h:17`, `docs/IPC.md` |
| Users | everything runs as root; `stage/init` `exec`s the desktop directly, no login, no `su` | `stage/init`, `exec /usr/bin/mystic-desktop` (173-184 at `efd54b2`) |
| App binaries | static musl, cross-compiled by zig (`-target x86_64-linux-musl -static`) | `tools/build-desktop.sh:42`, `tools/mkapp.sh:37` |
| Fonts | three TTFs in `/usr/share/fonts`, no fontconfig, no font packages | `stage/usr/share/fonts`, `src/main.c` `FONT_DIR` (30 at `efd54b2`) |
| Network | DHCP via `udhcpc` when `eth0` exists | `stage/init` |

Three consequences that shape every option below:

**(a) Chrome-sized things do not fit. Not yet.** 64 MB of disk and 256 MB of
RAM is not enough for any Chromium-class engine, at any compression. That size
is a knob rather than a constant (`MYSTIC_RAM`, `MYSTIC_DISK_MB`, `image.sh:50-54`),
but the defaults are what the OS ships with, so §6 says exactly how far they have
to move and which *non-obvious* prerequisites are missing on top.

**(b) Our build produces static binaries.** `tools/mkapp.sh:37` and
`tools/build-desktop.sh:42` both say `-target x86_64-linux-musl -Os -static`.
Musl being the libc is not the obstacle — a *dynamically* linked musl binary
could link Alpine's `libwebkit2gtk` in principle, and Alpine does build WebKit
against musl. The obstacles are concrete: (i) the desktop itself is one static
binary, so no engine can be compiled *into* it; (ii) there are no `-dev`
packages and no compiler in the image; (iii) the closure is 569-800 MB. So the
architecture is forced, for practical reasons rather than a law of nature: the
engine runs as its own dynamically linked process from `apk`, and our app is a
thin static client that talks to it. The protocol already supports that — an
app is a process with a socket and a buffer — but it decides the shape of every
candidate below.

**(c) The pixel format is the friendliest possible accident.** Our shm rows are
BGRA bytes. Chromium's CEF off-screen buffer is BGRA
(`cef_render_handler.h`:131-147, **[src]**). libjpeg-turbo decodes straight to
`TJPF_BGRX`/`TJPF_BGRA` (`turbojpeg.h`:366-417, **[src]**). An RFB server
pixel-format of 32 bpp with `redShift=16, greenShift=8, blueShift=0`
(`libnsfb/src/surface/vnc.c`:442-445, **[src]**) is the same bytes. X11 32-bit
`ZPixmap` on little-endian is the same bytes in the common case. **In every
viable design the final blit into the window is a `memcpy` per row, no
per-pixel work.** That is the one thing this OS got right in advance.

---

## 2. The five shapes a solution can take

Before the candidates, the taxonomy, because it makes the ranking obvious:

| Shape | Meaning | Who is in it |
|---|---|---|
| **A. Engine paints into our memory** | engine calls back with a BGRA buffer we own | CEF off-screen, (NetSurf RAM surface with a patch) |
| **B. Engine paints somewhere, we pull pixels** | we read a framebuffer/stream out of another process | Chromium CDP screencast, NetSurf over RFB, Xvfb + any X browser |
| **C. Engine takes the display** | engine becomes DRM master / owns fb0 | QtWebEngine eglfs, cog/KMS |
| **D. We implement the engine's display server** | we speak a small documented protocol the toolkit already has | GTK3/4 Broadway |
| **E. No engine** | text or ancient HTML only | links, w3m, lynx, elinks, dillo |

Shape **C** is not a browser on Mystic OS; it is a different operating system
with a browser in it. Shape **E** does not render real sites. That leaves A, B
and D, which is exactly what the shortlist in section 7 contains.

---

## 3. Candidate 1 — Chromium headless over the DevTools protocol

### 3.1 What exists

| Item | Fact |
|---|---|
| Package | `chromium` 156.0.8078.79-r0, community **[pkg]** |
| Binary | `/usr/lib/chromium/chromium`, wrapper `/usr/bin/chromium-browser` |
| Package size (`.apk` / installed) | 152.09 MB / **330.78 MB** **[pkg]** |
| Full runtime closure | **181 packages, 799.5 MB installed, 318.5 MB download** **[pkg]** |
| Smaller sibling | `chromium-headless-shell` 156.0.8078.79-r0: package 89.97 MB apk / 208.17 MB installed; closure **682.0 MB** **[pkg]** |
| What dominates the closure | `llvm22-libs` 190.97 MB + `mesa` 44.18 MB, pulled by the hard dependency `mesa-dri-gallium`; also `icu-data-full` 33.11 MB, `ffmpeg-libavcodec` 20.36 MB, `x265-libs` 21.34 MB **[pkg]** |
| musl | yes — Alpine builds it, with `musl-*` patches in the APKBUILD **[src]**, and `musl>=1.2.3` is an explicit dependency **[pkg]** |
| glibc | no |
| dbus | `so:libdbus-1.so.3` is a hard link dependency **[pkg]**. Whether headless *needs* a session bus is **unverified** — we did not test it, and it is a two-minute experiment once the image has the package |
| fontconfig | yes, `so:libfontconfig.so.1` is a hard dependency **[pkg]**; `font-opensans` is pulled in too |
| Sandbox | APKBUILD installs `chrome_sandbox` setuid 4755 **[src]**; running as root still needs `--no-sandbox` (Qt documents the equivalent `QTWEBENGINE_DISABLE_SANDBOX` / `--no-sandbox` for the same Chromium core) **[doc]** |

Alpine's build enables proprietary codecs: the APKBUILD sets
`proprietary_codecs=true` and `ffmpeg_branding="Chrome"` **[src]**, and the
package links `so:libavcodec.so.62` (Alpine's own ffmpeg, whose own dependencies
carry x264/x265/aom/vpx/dav1d decoders), plus `so:libopenh264.so.8` and
`so:libdav1d.so.7` directly **[pkg]**. So H.264 and AV1 decode in software on
this build, with no GPU.

There is a comment in Alpine's APKBUILD that matters: `# explicit depends for
--headless` immediately above the dependency list **[src]**. Headless is a
supported configuration of this package, not an afterthought.

One thing it does **not** ship is a CDM. There is no `widevine` package anywhere
in the v3.24 index **[pkg]**, and the APKBUILD mentions neither Widevine nor
`enable_widevine` (it sets only `proprietary_codecs=true` and
`ffmpeg_branding="Chrome"`) **[src]**. Encrypted Media Extensions therefore fail:
Netflix, Spotify, HBO, Disney+ and any DRM video will refuse to play with
`NotSupportedError`, however well H.264 decodes. This is not a detail — it is
most of what people mean by "multimedia on the web" in 2026.

### 3.2 Two ways to talk to it

**(i) TCP + WebSocket** — `--headless --remote-debugging-port=9222`
(`headless/README.md` **[src]**). We would need an HTTP client, a WebSocket
handshake, and masked frame encoding. ~400 lines of C we do not want to write.

**(ii) A pipe** — `--remote-debugging-pipe`. Chromium's own comment:

> `// Enables remote debug over stdio pipes [in=3, out=4] ...`
> `// Optionally, specifies the format for the protocol messages, can be either`
> `// "JSON" (the default) or "CBOR".`
> — `content/public/common/content_switches.cc`:580-586 **[src]**

and the framing, from `content/browser/devtools/devtools_pipe_handler.cc`
`:335-346` **[src]**:

```
// Go over the last read chunk, look for null byte, extract messages.
...
if (*it == 0u) { HandleMessage(...); next_message_start = it + 1; }
```

So: **JSON objects, NUL-terminated, on fd 3 and fd 4.** No HTTP, no WebSocket,
no base64 handshake. This is the single most important practical finding in
this document — a CDP client becomes a `poll()` loop plus a JSON parser, and
the engine never needs a listening socket.

### 3.3 Frames: what `startScreencast` really gives you

Not raw pixels. From the protocol definition (`Page.startScreencast` **[src]**):

```
format:     "jpeg" | "png"
quality:    0..100
maxWidth / maxHeight: integer
everyNthFrame: integer
maxFramesInFlight: "Maximum number of frames sent until screencastFrameAck is
                    required. Defaults to 3."
sendLastFrame: "stores the last produced frame in memory, which is immediately
                sent upon screencastFrameAck. This way, overall performance is
                traded for a better latency."
```

and the event (`Page.screencastFrame` **[src]**):

```
data      : "Base64-encoded compressed image."
metadata  : ScreencastFrameMetadata { offsetTop, pageScaleFactor, deviceWidth,
            deviceHeight, scrollOffsetX, scrollOffsetY, timestamp, monotonicTimestamp }
sessionId : "Frame number."
```

So the pipeline is: **compositor surface → PNG/JPEG encode in the browser →
base64 (+33 %) → NUL-terminated JSON over a pipe → base64 decode → JPEG decode →
BGRA → memcpy into our shm.** The ack (`Page.screencastFrameAck`) is
backpressure, not vsync; with the default 3 frames in flight the browser simply
stops producing.

Cost arithmetic at our window size 1280×680 **[calc]**:

- raw surface: 1280 × 680 × 4 = 3.48 MB per frame
- a typical page as JPEG q70: *estimate, unmeasured*, 80–200 KB → 107–267 KB
  once base64'd. A photo-heavy page is at the top of that.
- JPEG decode of one megapixel with libjpeg-turbo: *estimate, unmeasured*, on
  the order of 5–15 ms on a 2 GHz core. `libjpeg-turbo` is 0.66 MB installed
  **[pkg]** and decodes directly to `TJPF_BGRX`, so there is no colour
  conversion step.
- The whole-window `FRAME` copy the WM then does: 3.48 MB memcpy, per frame,
  because there are no damage rectangles yet (`docs/IPC.md`).

At 30 fps that is 104 MB/s of memcpy plus 30 JPEG decodes per second. On real
hardware that is fine. Under QEMU without KVM it is not, which is why
`image.sh:29-30` prefers `-accel whpx` when the host offers it.

`Page.captureScreenshot` exists as the still-frame path (`format`, `quality`,
`clip`, `fromSurface`, `captureBeyondViewport`, `optimizeForSpeed`) **[src]** —
useful for a "save this page as an image" feature and for tests, not for
animation.

### 3.4 Input

`Input.dispatchMouseEvent` takes exactly what we have:
`type, x, y, modifiers, timestamp, button, buttons, clickCount, force,
tangentialPressure, tiltX, tiltY, twist, deltaX, deltaY, pointerType` **[src]**.

The `buttons` bitmask is **already identical** to ours — the protocol says
"A number indicating which buttons are pressed ... Left=1, Right=2, Middle=4,
Back=8, Forward=16, None=0" **[src]**, and `src/input.c:177-179` sets
`BTN_LEFT → 1`, `BTN_RIGHT → 2`, `BTN_MIDDLE → 4` **[ours]**. So the whole
translation is: forward `input.buttons` unchanged, and on the press/release edge
map the *bit* to the `button` **string** enum (`"none" | "left" | "middle" |
"right" | "back" | "forward"`) **[src]**. No table, no remapping of bits.

`Input.dispatchKeyEvent` takes
`type, modifiers, timestamp, text, unmodifiedText, keyIdentifier, code, key,
windowsVirtualKeyCode, nativeVirtualKeyCode, autoRepeat, isKeypad, isSystemKey,
location, commands` **[src]**. This is the friction point: Linux evdev gives us
a keycode, and Chromium wants a `key`+`code`+VK triple. Our `KEY <keycode>
<mods> <press>` (`src/ipc.h`) carries no keysym at all. We would need an
evdev-keycode → (code, key, VK) table — about 120 lines of static table, plus
`input_poll`'s existing shift/alt/ctrl bits (`src/mystic.h:94-96`) folded into
Chromium's modifier mask. `Input.insertText` covers plain typing in one call and
is the escape hatch when the table is wrong.

`Input.synthesizeScrollGesture` (`x, y, xDistance, yDistance, ...`) gives real
smooth scrolling for free, which is worth having because a wheel event alone
does not produce kinetic scrolling in Chrome.

### 3.5 Does multimedia work?

**Picture: yes. Sound: no, not without work.** The evidence:

- New headless *is* the Chrome binary: "Running Chrome with `--headless` or
  `--headless=new` runs the new Headless mode ... which continues to be provided
  by the Chrome binary" **[doc]**, and the headless README describes it as
  "using all the modern web platform features provided by Chromium and Blink"
  **[src]**. Old headless was a separate embedder and stopped existing in M132.
- The media stack is the normal one: `so:libavcodec.so.62` (Alpine's ffmpeg),
  `so:libdav1d.so.7` and `so:libopenh264.so.8` are all direct dependencies
  **[pkg]**; VP8/VP9 arrive through that ffmpeg, which depends on `libvpx`
  **[pkg]**. Software decode needs no GPU.
- Autoplay is blocked by default in a browser with no user gesture; the switch
  is `--autoplay-policy` (`media/base/media_switches.cc`:61 **[src]**), value
  `no-user-gesture-required`.
- Audio *output* needs a device. In headless there is none; `--mute-audio`
  exists and is documented as "Mutes audio sent to the audio device so it is not
  audible during automated testing" (`media_switches.cc`:181-183 **[src]**) —
  which implies the mixer runs and the device sink is a no-op. So video plays at
  the right speed with silent audio. Real sound means writing an audio sink —
  our own ALSA device on the guest, or a pipe we drain — and that is a separate
  project. (`--use-fake-device-for-media-stream` is *not* that: it is documented
  as "Use fake device for Media Stream to replace actual camera and
  microphone" (`media_switches.cc`:230-233 **[src]**) — it fakes **capture** and
  gives you no output path at all.)

**Verify on target** with: a page containing an inline `<video autoplay muted>`,
`--autoplay-policy=no-user-gesture-required`, screencast on, and check that
successive `data` payloads differ. That is the experiment; it has not been run.

### 3.6 Verdict

**The only option in this document that renders every real site *and* plays
video, and the cheapest to integrate** (no protocol invention, no display server,
a documented stable API, NUL-delimited JSON on two file descriptors). The cost
is brutal and entirely in resources: 682 MB of packages for the headless shell,
~1 GB of RAM to be comfortable, `--no-sandbox` because we are root, a JPEG
decode per frame, and no audio.

**Cannot:** fit in 256 MB / 64 MB; **play DRM video (no Widevine/CDM exists for
Alpine's build, so Netflix/Spotify/HBO-class sites fail even though the codecs
are present)**; provide audio without a sink; give us sub-frame latency; run
without `--no-sandbox` as root; avoid pulling LLVM (via `mesa-dri-gallium`) even
with `--disable-gpu`.

---

## 4. Candidate 2 — NetSurf's framebuffer frontend

This one needs a correction up front, because the premise in the brief is no
longer true.

### 4.1 The fbdev surface was removed in 2018

`netsurf-fb` renders through **libnsfb**, NetSurf's own framebuffer
abstraction. `libnsfb/include/libnsfb.h`:36-45 still lists
`NSFB_SURFACE_LINUX`, and NetSurf's about page still says "A dumb framebuffer
port also exists" **[doc]**. But the surface handlers present in
`libnsfb` master are only:

```
src/surface/Makefile:
SURFACE_HANDLER_yes := surface.c ram.c
SURFACE_HANDLER_$(NSFB_SDL_AVAILABLE) += sdl.c
SURFACE_HANDLER_$(NSFB_XCB_AVAILABLE) += x.c
SURFACE_HANDLER_$(NSFB_VNC_AVAILABLE) += vnc.c
SURFACE_HANDLER_$(NSFB_WLD_AVAILABLE) += wld.c
```

and the reason `linux.c` is missing is a commit:

> `b2bb5654  2018-07-29  remove broken surface handlers for ABLE and linux
> framebuffer interface`

**[src]** — the file is gone from master (checked against the current tree, and
the repository mirror was last pushed 2026-11-15).

So **netsurf-fb cannot write to `/dev/fb0` today.** The "framebuffer" in its
name means "libnsfb", and libnsfb's available surfaces are: RAM, SDL 1.2, XCB,
Wayland, VNC. Three of those need a display server we do not have. That leaves
two that are interesting, and one of them is very interesting.

Also note what the *Alpine* `netsurf` package is: `usr/bin/netsurf` and
`usr/bin/netsurf-gtk3` — the **GTK** frontend, whose closure is 87 packages /
74.2 MB because it needs GTK3 and `libX11` **[pkg]**. There is no
`netsurf-fb` package in Alpine v3.24 (`netsurf-fb`: not in main or community
**[pkg]**). So on Alpine as shipped, NetSurf needs X11 — the one thing we do
not have.

### 4.2 The two surfaces that could work

Note the upstream default first: `frontends/framebuffer/Makefile.defaults`
sets `NETSURF_FB_FRONTEND := sdl` with "Valid values are: x, sdl, linux, vnc,
able" **[src]** — a source build defaults to needing SDL, and the list still
names `linux`. Both are worth knowing before starting.

**RAM surface (`-f ram`).** `libnsfb/src/surface/ram.c` allocates its own
buffer (`realloc`, format defaults to `NSFB_FMT_ABGR8888`) and
`nsfb_get_buffer(nsfb, &ptr, &linelen)` hands out the pointer (`libnsfb.h`:163
**[src]**). NetSurf's frontend already supports picking a surface by name:
`nsfb -f <frontend> -w <w> -h <h>` (`frontends/framebuffer/gui.c`:507-545
**[src]**) and it even exposes `framebuffer_set_surface(nsfb_t *)` for swapping
one in (`framebuffer.c`:648 **[src]**).

The catch is input, and it is a precise catch. NetSurf's toolkit loop calls
`fbtk_event(fbtk, &event, timeout)` → `nsfb_event(root->u.root.fb, event,
timeout)` (`frontends/framebuffer/fbtk/event.c`:188-198 **[src]**), i.e. **events
come from the surface**. And the RAM surface's input handler is a stub:

```c
static bool ram_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    UNUSED(nsfb); UNUSED(event); UNUSED(timeout);
    return false;
}
```

**[src]**. `nsfb_surface_get_rtns()` even *requires* a non-NULL input handler
to register a surface (`src/surface/surface.c`:94-101 **[src]**), so the RAM
surface exists but is deaf and dumb. Feeding it our `MOUSE`/`KEY`/`TEXT`
messages is a patch to `ram_input` (or a new `mystic.c` surface that registers
itself via `_nsfb_register_surface`), roughly 80–150 lines, in *libnsfb*, not in
NetSurf. That is the whole integration.

**VNC surface (`-f vnc`).** This is the elegant one. `src/surface/vnc.c` builds
an RFB server with libvncserver, points `nsfb->ptr` at libvncserver's
framebuffer (`vnc.c`:419-458 **[src]**) and wires input back:

```c
vncscreen->ptrAddEvent = vnc_doptr;
vncscreen->kbdAddEvent = vnc_dokey;
```

**[src]** — so pointer and keyboard return to NetSurf as `nsfb_event_t`s, with
**zero patching**. Our side is a small RFB client (`RFC 6143`): handshake,
`SetPixelFormat` to 32 bpp `redShift=16/green=8/blue=0`, incremental
`FramebufferUpdateRequest`, and `Raw` (plus optionally `Hextile`) decoding.
`libvncserver` is 0.40 MB installed as a package, and **10.0 MB as a closure**
(10 packages: `libgcrypt`, `liblzo2`, `libpng`, `libjpeg`, `openssl`) **[pkg]** —
which is still nothing next to any engine, so the choice survives the corrected
number.

In other words: NetSurf can be a browser *server* on localhost, and our static
app is its display. 600–900 lines of RFB client. No X, no Wayland, no GL, no
patch to NetSurf at all.

### 4.3 What it actually renders

| Feature | Reality | Source |
|---|---|---|
| HTML/CSS | its own engine (Hubbub + LibCSS). It has real flexbox layout (`content/handlers/html/layout_flex.c`), tables (`table.c`), no CSS Grid (no grid file anywhere in `content/handlers/html/`) | **[src]** |
| JavaScript | Duktape binding, `NETSURF_USE_DUKTAPE := YES` by default | **[src]** |
| How complete is that JS | `docs/UnimplementedJavascript.md` is a generated list of **1539 unimplemented binding methods/getters/setters**, including every `URL` accessor and `URLSearchParams` | **[src]** |
| Video | `NETSURF_USE_VIDEO := NO` by default ("Enable NetSurf's use of gstreamer for displaying videos") | **[src]** |
| Fonts | Freetype via `FTC_*` with compiled-in font paths; **no fontconfig** (`font_freetype.c` uses no `Fc*` call) | **[src]** |
| Dependencies | no glib, no dbus, no GTK for the fb frontend; needs libcurl, libpng, and the ~12 NetSurf project libraries (libparserutils, libwapcaplet, hubbub, libcss, libdom, libnsbmp, libnsgif, libnsfb, libsvgtiny, ...) | **[src]** `docs/building-Framebuffer.md` |

Size: no packaged fb build exists to measure. For scale, the Alpine **GTK**
build is 6.00 MB installed **[pkg]**; a full engine is the same engine with a
bigger frontend bolted on, and the frontend is the part that includes GTK and
X11. A source build of the fb frontend plus its libraries should land in single
digit MB — *estimate, unmeasured*; the way to settle it is to build it.

Time to build from source: the NetSurf project's own build order is documented
(`docs/building-Framebuffer.md` **[src]**) and it is a dozen small C libraries,
each with `make install`. Not hard, not quick.

### 4.4 Verdict

**The only candidate that plausibly fits in the machine we have.** It is also
the only one where the pixels arrive as a memory pointer we already own, with no
codec in the path.

**Cannot:** run modern JS applications (1539 unimplemented bindings is the
upstream's own count, and it is a floor, not a ceiling); play video at all by
default; render CSS Grid; use a GPU. It will render documentation, blogs, wikis,
government forms, and it will fall over on anything that expects a 2020s SPA.

---

## 5. The rest, honestly

### 5.1 WebKitGTK + GTK's Broadway backend (shape D)

GTK3 still ships Broadway, and Alpine proves it: `gtk+3.0` 3.24.52-r0 provides
`cmd:broadwayd` **[pkg]** (`gtk4.0` 4.22.4-r0 does too, so the backend survived
into GTK4 — my prior belief that it had been dropped was wrong).

The protocol is small, binary, and fully specified in one header,
`gdk/broadway/broadway-protocol.h` **[src]**:

```
BROADWAY_OP_*   : grab/ungrab pointer, new/show/hide/raise/lower/destroy
                  surface, move_resize, set_transient_for, put_rgb,
                  put_buffer, request_auth, auth_ok, disconnected,
                  set_show_keyboard          (16 opcodes)
BROADWAY_EVENT_*: enter, leave, pointer_move, button_press/release, touch,
                  scroll, key_press/release, grab/ungrab notify,
                  configure/delete notify, screen_size_changed, focus
                  (15 event types)
```

Pixels: `broadway_output_put_buffer()` sends id + width + height + a
zlib-deflated stream in broadway's own 4-byte-symbol encoding, which is
specified in a comment in `gdk/broadway/broadway-buffer.c`:145-170 **[src]** —
literal pixel (`0xaa rr gg bb`), delta-0 run, block reference, solid colour run,
delta run. A decoder is ~150 lines.

Input: it returns. The app receives input because the server sends
`BROADWAY_REPLY_EVENT` replies, and `gdkbroadway-server.c`:320-321 **[src]** turns
each one into `_gdk_broadway_events_got_input (&reply->event.msg)` — a function
defined in `gdk/broadway/gdkeventsource.c`:111 **[src]**, which builds real
`GdkEvent`s out of the `BroadwayInput*Msg` structs in the header above. That is
the whole input path, and it is why implementing the *server* side of this
protocol is enough to host a GTK app.

Crucially, `broadwayd` is only a relay. The app connects to it over a Unix
socket and the browser connects over WebSocket. **We could skip `broadwayd`
entirely and be the server on that Unix socket** — then any GTK3 application in
the image becomes a window in our compositor, and we have not written one line
of X11 or Wayland. That is a much bigger prize than "a browser": it is *every
GTK app*.

The browser part: `webkit2gtk-4.1` 2.48.7-r0, 47.56 MB apk, **125.69 MB
installed**, closure **200 packages / 569.1 MB** **[pkg]**, requiring
`bubblewrap`, `xdg-dbus-proxy`, a D-Bus session (`dbus:org.freedesktop.Secrets`
**[pkg]**) and, for media, GStreamer (`so:libgst*` **[pkg]**), which on Alpine
means the gst plugin packages too (so the 569 MB is optimistic for video).

Broadway provides no GL context, so WebKit would have to run unaccelerated.
That is a supported path, not a hack: `WEBKIT_DISABLE_COMPOSITING_MODE` is read
in `Source/WebKit/UIProcess/gtk/HardwareAccelerationManager.cpp`:45 **[src]**,
and `WEBKIT_DISABLE_DMABUF_RENDERER` in `AcceleratedBackingStore.cpp`:87
**[src]**. With compositing off, WebKit draws with cairo — which is exactly what
a Broadway surface accepts.

**Verdict: technically the best fit for "real sites + video + a real toolkit",
and the most engineering.** Ten to fifteen days: the Broadway server (~1200
lines), keysyms, and then fighting 569 MB of dependencies, D-Bus and a
bubblewrap sandbox that wants user namespaces.

**Cannot:** be small; avoid D-Bus; provide GL; avoid GStreamer for video.

### 5.2 QtWebEngine with eglfs (shape C) — reject

The Qt documentation is unambiguous **[doc]**:

> "EGLFS supports a single fullscreen GL window ... Opening additional OpenGL
> windows or mixing such windows with `QWidget`-based content isn't supported;
> **Qt terminates the application with an error message.**"

and `QT_QPA_EGLFS_FB` "Overrides the framebuffer device. The default is
`/dev/fb0`" **[doc]**. eglfs takes DRM master. It cannot be a window in our
compositor, it cannot share the scanout, and it would blank the desktop
underneath it. Also: `qt6-qtwebengine` 6.11.1-r3 is 126.01 MB apk / **287.72 MB
installed**, closure **796.1 MB** **[pkg]**; upstream's build requirements list
"Glibc library at least 2.16" for Linux **[doc]** (Alpine patches that, but it
tells you what the code assumes).

Qt's *own* docs also document the sandbox requirement — `seccomp-bpf`, and
unprivileged user namespaces or a setuid sandbox — plus the memory warning:
"BUS errors (SIGBUS) might be reported ... caused by Docker running a container
with a too small memory space (such as 64MB)" **[doc]**. That is the same
failure our 256 MB guest would hit.

**Verdict: not a browser on Mystic, a kiosk instead of Mystic.** Only worth
revisiting if the goal changes to "one fullscreen browser appliance".

### 5.3 Framebuffer-native browsers

| Thing | Reality |
|---|---|
| `links` 2.30-r1 (Alpine main) | 1.47 MB installed, closure 9.8 MB **[pkg]**. Its binary's help text still lists graphics drivers ("Graphics driver to use. Drivers are: x, svgalib, fb, direc...", found in the packaged binary), but Alpine links **no** libpng, libjpeg, libX11, DirectFB or svgalib into it **[pkg]** — so `-g` has no decoder and no driver it can use here. Text mode only, plus its own `fb` driver would want `/dev/fb0` and would fight the compositor. |
| `dillo` 3.3.0-r2 | 0.86 MB installed but closure **25.9 MB**, because it is FLTK and `libX11` **[pkg]**. Needs X. Its own engine, no JS. |
| `hv3` | Tk. Not packaged in v3.24 at all **[pkg]**. |
| Ladybird | not packaged in v3.24 **[pkg]**; upstream's `UI/` directory on master contains only `Qt` and `Android` **[src]** — no framebuffer frontend to use. |
| `w3m`, `lynx`, `elinks` | 1.5 / 1.5 / 1.1 MB installed **[pkg]**. Text. They render *text* of real sites, correctly, and belong in the Terminal app, not in a plan for a graphical browser. |

**Verdict: none of these is "renders real sites + multimedia".** They are
fallbacks, and the honest framing is that `w3m` inside the existing Terminal is
a 20-minute win that should not be confused with a browser project.

### 5.4 Engines that paint into a caller-provided buffer (shape A)

**CEF off-screen rendering.** The pixel contract is exactly ours:

> `OnPaint(... const void* buffer, int width, int height)` ... "`buffer` will be
> `width*height*4` bytes in size and represents a **BGRA image with an
> upper-left origin**"
> — `cef_render_handler.h`:131-147 **[src]**

plus `dirtyRects`, so we could fix the WM's missing damage rectangles at the
source. Sound is a callback too: `CefAudioHandler::OnAudioStreamPacket` hands us
decoded PCM plus `GetAudioParameters` (`cef_audio_handler.h`:51-91 **[src]**) —
the only candidate in this document with a documented audio *output* path. This is the *architecturally*
correct answer: the engine pushes frames, no encoding, no capture.

But CEF is **not packaged in Alpine v3.24** **[pkg]**, official builds are
glibc-based, so we would have to build CEF against musl: that is a Chromium
build. Alpine's APKBUILD is the measure of the job — `grep -c '\.patch'` on it
counts 67 references to 46 distinct patch files, `musl-*` among them, plus an
LLVM/clang toolchain and a Rust bootstrap **[src]**. Weeks, not days, and it
would still be 200 MB+.

**WPE WebKit + Cog.** Cog has a headless platform
(`docs/platform-headless.md` **[src]**) — but its requirement is
"**WPEBackend-fdo**", i.e. EGL, and its only parameter is a refresh rate. Frames
arrive as EGL images/DMA-BUFs, not as a plain memory buffer, so getting them
into our shm means `glReadPixels` — which means mesa, which means the 191 MB of
LLVM. And `wpewebkit`/`wpebackend-fdo`/`cog` are not in Alpine v3.24 **[pkg]**,
so all of it is a source build.

**Qt offscreen:** we found no upstream statement that Qt WebEngine runs on the
`offscreen` QPA plugin — Qt's platform notes say nothing about it, and
WebEngine's compositor is GL-based. That is an absence of evidence, not evidence
of impossibility, but it is not worth weeks to disprove when §7.2 #1 exists.

### 5.5 The option that keeps being the answer: Xvfb (shape B, X11 in a box)

`xvfb` 21.1.26-r0 is 1.45 MB as a package **[pkg]**. It is a real X server that
draws into memory and needs no display. Then *any* X11 browser works — Chromium,
Firefox, Dillo, links -g. Get pixels with `XShmGetImage` on the root window
(32-bit `ZPixmap`, which on little-endian is BGRA bytes, i.e. **another
memcpy**), send input with XTEST (`xdotool` is 0.12 MB **[pkg]**).

**The cost, corrected.** Quoting `xvfb`'s own closure (54 packages, 268.5 MB) as
the price of this option double-counts by about 260 MB, and an earlier draft of
this document did exactly that. 190.97 MB of that closure is `llvm22-libs` and
44 MB is `mesa`, and Chromium **already** hard-depends on `mesa-dri-gallium`
**[pkg]**. Resolving the roots together rather than separately:

| Roots | Packages | Closure |
|---|---|---|
| `chromium` | 181 | 799.5 MB |
| `xvfb` alone | 54 | 268.5 MB |
| **`chromium` + `xvfb`** | **193** | **807.1 MB** |
| `chromium-headless-shell` | 182 | 682.0 MB |
| **`chromium-headless-shell` + `xvfb`** | **194** | **689.6 MB** |
| `firefox` | 128 | 687.8 MB |
| **`firefox` + `xvfb`** | **140** | **694.8 MB** |

**[pkg]**, union closures, shared dependencies counted once. So Xvfb costs
**7.6 MB** on top of Chromium and **7.0 MB** on top of Firefox — not 268 MB.
The X server itself is nearly free; what is expensive is the browser.

That changes what this option *is*. It is not "250 MB of extra cost to buy
smooth video". It is "for 7.6 MB of extra packages, get raw pixels at page rate
instead of a decoded JPEG per frame". The arguments against it are therefore
**not** cost:

1. It puts X11 in an OS whose pitch is that it has no X11. That is a positioning
   decision for the owner, not a technical one, and it should be made as such.
2. It gives us pixels and synthetic input (XTEST) and nothing else. Every browser
   *function* — URL bar, history, find-in-page, zoom, print, tabs — has to be
   driven by faking keystrokes into a picture. The CDP path in §3 gives all of
   that as an API. This is the real reason it does not rank first, and it is a
   reason about *controllability*, not megabytes.
3. Firefox does not speak CDP, so the "control API + raw pixels" combination in
   §7.2 #2 is Chromium-only.

Its real advantage stands: **no codec in the frame path.** XShmGetImage gives raw
pixels at whatever rate the page produces them, so video at 30 fps costs 3.5 MB
of memcpy per frame instead of 30 JPEG decodes. Note also that because we capture
the *root* window, popups, `<select>` dropdowns and DevTools all land in the same
image — "window chasing" is not the problem it would be with per-window capture;
the real cost is that everything the browser puts up appears inside one Mystic
window.

### 5.6 Same idea, Wayland flavour

`weston --backend=headless` (closure 271.4 MB, same LLVM problem **[pkg]**) plus
Chromium with `--ozone-platform=wayland`, then read pixels from Weston's
headless output. This is "implement Wayland by proxy" and buys nothing over
Xvfb except a newer protocol. Not recommended.

---

## 6. What has to change on the OS side first

Irrespective of which engine wins, the machine as configured cannot host one.
These are the knobs, with the arithmetic.

| Knob | Now | Needed for Chromium-class | How |
|---|---|---|---|
| Guest RAM | `MYSTIC_RAM`, default 256 | 1024–2048 | `MYSTIC_RAM=1024 ./image.sh` — already supported, no edit |
| Disk | `MYSTIC_DISK_MB`, default 64 | 2048 (682 MB packages + profile + swap) | `MYSTIC_DISK_MB=2048 ./image.sh` — **but see the trap below** |
| `/dev/shm` | tmpfs, default 50 % of RAM | same or bigger; Chromium hammers it | `stage/init:23` (`mount -t tmpfs none /dev/shm`) |
| Root/sandbox | runs as root | `--no-sandbox`, or a non-root `browser` user | `stage/init`, desktop exec block |
| Fonts | 3 TTFs, no fontconfig | several font families + a `fonts.conf` | `stage/usr/share/fonts` |
| Timezone/locale | none | `/etc/localtime` or `TZ`, else TLS and dates misbehave | new files |
| CA bundle | none in the image | `/etc/ssl/certs/ca-certificates.crt` (**~200 KB**) or every HTTPS site fails | new |
| DNS | works: the DHCP script writes `/etc/resolv.conf` | nothing — listed only so nobody goes looking | `stage/etc/udhcpc.script:13-14` |

Two of those rows used to be presented here as source edits. They are not: the
sizes are environment knobs (`image.sh:50-54`), so raising them is a command
line, not a patch. **The trap:** the disk image is only created when it does not
exist —

```sh
DISK=$BUILD/disk.img
if [ ! -f "$DISK" ]; then
    dd if=/dev/zero of="$DISK" bs=1048576 count="${MYSTIC_DISK_MB:-64}"
fi
```

— so `MYSTIC_DISK_MB=2048` silently does nothing once `build/disk.img` exists.
Delete it (or `rm -rf build/`) or the browser will be installed onto a 64 MB
filesystem and the first big download will fail with `ENOSPC` in a way that
looks like a browser bug.

Two of those are silent-failure traps worth naming: **no CA bundle** means
`ERR_CERT_AUTHORITY_INVALID` on every page and looks like a browser bug, and **no
locale** makes `Intl` misbehave in ways that look like site bugs. Both are
one-line fixes that will otherwise eat a day.

The `FRAME`-is-whole-window limitation (`docs/IPC.md`) becomes a real cost at
browser frame rates: 3.5 MB per frame at 30 fps is 104 MB/s through the socket
path and the compositor. Adding a damage rectangle to the protocol would be
worth more here than anywhere else in the system.

---

## 7. Ranked shortlist

### 7.1 The comparison the checklist asks for

One row per candidate, one column per question that was asked. **All sizes are
union closures** resolved together, so nothing is counted twice; "all sites"
means a real engine, not a subset.

| Candidate | Binary | Closure | libc | dbus | fontconfig | All sites | Video | DRM video | Sound |
|---|---|---|---|---|---|---|---|---|---|
| Chromium headless + CDP pipe | `chromium-headless-shell` | **682.0 MB** **[pkg]** | musl (Alpine) | linked, need unverified | yes | **yes** | **yes**, software | **no** (no CDM) | no sink |
| Chromium in Xvfb, CDP for control | `chromium` + `xvfb` | **807.1 MB** **[pkg]** | musl | linked | yes | **yes** | **yes** | **no** (no CDM) | no sink |
| Firefox in Xvfb | `firefox` + `xvfb` | **694.8 MB** **[pkg]** | musl | linked | yes | **yes** | **yes** | no CDM in index | `libasound` linked, no sink |
| WebKitGTK over Broadway | `MiniBrowser` or own GTK app | **569.1 MB** + GStreamer **[pkg]** | musl | **yes** (session + `xdg-dbus-proxy`) | yes | yes, minus Chrome-only | via GStreamer | **no** | with a GStreamer sink |
| QtWebEngine + eglfs | any Qt app | **796.1 MB** **[pkg]** | musl (upstream wants glibc) | linked | yes | yes | yes | no | yes — but it owns the display |
| NetSurf, RAM or VNC surface | `nsfb` | single-digit MB *[est]* | musl | **no** | **no** | **no** | **no** | no | no |
| CEF off-screen | our host + `libcef` | 200 MB+ *[est]* | would need a build | linked | yes | yes | yes | no | **yes** (`CefAudioHandler`) |
| links / dillo / w3m | as packaged | 9.8 / 25.9 / 1.5 MB **[pkg]** | musl | no | dillo only (via FLTK) | no | no | no | no |

Four things fall out of that table, and they are the whole decision:

1. **Anything that renders every site costs ≥ 569 MB.**
2. **The one candidate that fits the current machine is the one that cannot
   render modern sites.**
3. **Nothing on this list plays DRM video**, because no Widevine CDM exists for
   musl/Alpine at all. "Multimedia" here means open video (H.264/VP9/AV1 in
   `<video>`, no EME), and the plan should say so to the owner before the work
   starts rather than after.
4. **Only two designs put pixels in our memory without a codec or a protocol
   translation in between:** CEF off-screen (no musl build exists) and NetSurf's
   RAM surface (no input, needs a patch). Everything else is a decode or a
   protocol — and for Chromium-in-Xvfb, the decode disappears entirely.

### 7.2 The ranking

The ordering has to be argued, not asserted, because the corrected Xvfb number
removes the cost argument that used to separate #1 from #2. What actually
separates them:

| | #1 headless + CDP | #2 Chromium in Xvfb + CDP |
|---|---|---|
| Closure | 682.0 MB | 807.1 MB (**+125.1 MB**, of which 7.6 MB is X) |
| Frame path | browser encodes JPEG → base64 → pipe → we decode | X server writes BGRA → we `memcpy` |
| Cost per 1280×680 frame | 3.48 MB memcpy **+ a JPEG decode** | 3.48 MB memcpy |
| Frames in flight | 3, ack-gated (`maxFramesInFlight`) | as many as we ask for |
| Control API | **yes** — navigate, history, find, zoom, print, cookies, JS eval | none; faking Ctrl+L into a picture |
| X11 in the image | no | yes |
| Days to first working window | 8–12 | 6–9 |

So: **#1 if the browser is to be part of the OS** (no X11, and a real API to
build chrome on); **#2 if frames-per-second is the binding constraint** and X11
in the image is acceptable. That is an editorial decision about what Mystic OS
is, not a technical one, and it belongs to the owner. What is *not* a matter of
taste is that #2 is far cheaper than this document previously claimed: 7.6 MB of
X, not 268 MB.

They also compose, and that is the useful engineering fact: Chromium under Xvfb
*still has* `--remote-debugging-pipe`. You can take pixels from `XShmGetImage`
and control from CDP in the same app, getting raw frames **and** a URL bar that
is not a keystroke puppet. That is exactly #2 as listed, and it is why #2's day
estimate is lower than #1's: the pixel path is dumber, and CDP supplies the
missing half.

#### 1. Chromium headless over `--remote-debugging-pipe` — **8–12 days**

The recommended first attempt if the OS keeps its "no X11" identity. A
NUL-delimited JSON loop, `Page.startScreencast` for frames, `Input.dispatch*` for
input, libjpeg-turbo straight into `TJPF_BGRX`, `memcpy` into the shm.

- Needs: 682.0 MB closure, ~1 GB RAM, 2 GB disk, `--no-sandbox`, a CA bundle,
  `--autoplay-policy`, and the `button`-string mapping of §3.4.
- Work: CDP client ~700 lines; window chrome in our own style; ~8–12 days
  including an honest latency measurement.
- **Cannot:** fit in 256 MB / 64 MB; **play DRM video (no CDM for Alpine, so
  Netflix/Spotify/HBO fail)**; provide audio without a sink; give sub-frame
  latency; run without `--no-sandbox` as root; avoid LLVM via `mesa-dri-gallium`
  even with `--disable-gpu`; beat a raw-pixel capture on frames-per-second.

#### 2. Chromium inside Xvfb — pixels by `XShmGetImage`, control by CDP — **6–9 days**

Best frame path available on this OS, and only 7.6 MB of it is X. The X root
window is the browser's screen; we capture it as BGRA and `memcpy` it into the
window's shm; CDP does navigation, history, zoom, find and print.

- Needs: 807.1 MB closure (`chromium` + `xvfb`), ~1 GB RAM, 2 GB disk,
  `--no-sandbox`, XTEST or CDP for input, `xdotool` 0.12 MB if we want XTEST.
- Work: spawn Xvfb on a private display, `XShmGetImage` into our surface, map our
  window's input to CDP events, and the same CDP chrome as #1 — which is why it
  is *faster* to a usable browser, not slower.
- **Cannot:** keep X11 out of the image; play DRM video; give us per-Mystic-window
  browser dialogs (everything appears inside the one window); run two browsers at
  different sizes without two X servers.

#### 3. NetSurf on libnsfb, VNC or RAM surface — **8–12 days**

The only option that fits the machine *as it is configured today* (256 MB /
64 MB), and the only one that needs no patch to the browser at all if we take the
VNC surface.

- Needs: build NetSurf + ~12 libraries for musl; `libvncserver` closure 10.0 MB;
  fonts. No glib, no dbus, no fontconfig, no GL.
- Work: RFB client ~700 lines (handshake, `SetPixelFormat`, incremental updates,
  Raw + Hextile), or ~150 lines in libnsfb for the RAM surface plus event
  injection; keysyms; NetSurf's own fbtk UI is the chrome, which will not look
  like Mystic.
- **Cannot:** run modern JS (1539 unimplemented bindings, upstream's own count);
  play video; render CSS Grid; do DRM; use a GPU. It is a document browser, and
  the moment the machine is raised to 1 GB its one advantage evaporates.

#### 4. WebKitGTK + our own Broadway server — **12–18 days**

The biggest prize per line of protocol: implement Broadway once and *every GTK
application* becomes a Mystic window, a browser included. It is also the
cheapest "real engine" closure at 569.1 MB.

- Needs: `webkit2gtk-4.1` + GStreamer plugins, a D-Bus session,
  `bubblewrap`/user namespaces (or `WEBKIT_DISABLE_COMPOSITING_MODE=1` and no
  sandbox), a keymap.
- Work: Broadway server ~1200 lines (16 opcodes, 15 event types, the 4-byte
  buffer encoding, zlib), auth, surface lifetime, then the browser chrome.
- **Cannot:** be small; avoid D-Bus; do GL; avoid GStreamer; do DRM.

#### 5. CEF off-screen rendering — **20+ days**

Architecturally the best (BGRA into our pointer, dirty rectangles, and the only
documented audio *output* callback), and the worst to start: there is no musl
CEF, so the first weeks are a Chromium-class build.

- **Cannot:** be attempted before someone has built Chromium for musl once.

#### Rejected

- **QtWebEngine + eglfs** — takes the display; Qt's own docs say it terminates
  when asked to share.
- **WPE/Cog headless** — EGL frames, no plain buffer, nothing packaged, LLVM.
- **weston headless + Wayland client** — the Xvfb idea with a newer protocol and
  no working browser ecosystem to inherit.
- **links -g / dillo / hv3 / Ladybird** — no engine worth the name, or not
  packaged, or needs X.
- **w3m/lynx/elinks** — do this *anyway*, in the Terminal, this week; it is 20
  minutes of work and it makes the OS useful on the day the engine is still not
  there.

## 8. What I would do next, in order

1. **Measure before building.** `rm -f build/disk.img`, then
   `MYSTIC_RAM=2048 MYSTIC_DISK_MB=4096 ./image.sh`, and run *both* frame paths
   against the same page with a 40-line driver:
   `chromium-headless-shell --headless --remote-debugging-pipe --no-sandbox`
   timing `screencastFrame` → ack, then Chromium inside `Xvfb` with
   `XShmGetImage` on the root. That one comparison answers the only question
   upstream source cannot settle for us — *is JPEG-per-frame fast enough on this
   CPU?* — and it is what decides §7.2 #1 vs #2.
2. **Fix the invisible prerequisites** (CA bundle, `/etc/localtime`, fonts,
   `/dev/shm` size, a `browser` user). Half of them will be needed by whichever
   engine wins, and each one otherwise masquerades as an engine bug.
3. **Build the pixel path once, properly:** a `surf_import_bgra(Surface *dst,
   const u8 *src, int src_stride)` helper plus the `HELLO`/shm handshake, shared
   by every candidate. All five viable options end in the same `memcpy`.
4. **Then** pick #1 or #2 on that measurement, not on taste, and keep the
   NetSurf/VNC sketch in a drawer: it is the answer if the owner ever says "it
   has to run on the 256 MB machine".

---

## 9. Evidence index

Package facts: Alpine v3.24 `APKINDEX` for `main` and `community`, x86_64,
downloaded from `https://dl-cdn.alpinelinux.org/alpine/v3.24/{main,community}/x86_64/APKINDEX.tar.gz`,
dependency closures resolved through `so:`/`cmd:` provides. Every multi-package
figure in §5.5 and §7.1 is a **union** closure (roots resolved together, shared
dependencies counted once), which is why `chromium`+`xvfb` is 807.1 MB and not
1068 MB. Package versions
quoted: `chromium 156.0.8078.79-r0`, `chromium-headless-shell 156.0.8078.79-r0`,
`chromium-swiftshader 156.0.8078.79-r0`, `chromium-angle 156.0.8078.79-r0`,
`netsurf 3.11-r1`, `dillo 3.3.0-r2`, `webkit2gtk-4.1 2.48.7-r0`,
`qt6-qtwebengine 6.11.1-r3`, `firefox 160.0-r0`, `links 2.30-r1`,
`xvfb 21.1.26-r0`, `weston-backend-headless 14.0.2-r5`, `gtk+3.0 3.24.52-r0`,
`gtk4.0 4.22.4-r0`, `libjpeg-turbo 3.1.3-r0`, `libvncserver 0.9.15-r0`,
`mesa 26.1.6-r0`, `llvm22-libs 22.1.3-r1`, `ffmpeg-libavcodec 8.1.3-r0`.

Upstream source, Chromium (`chromium.googlesource.com/chromium/src`, `lkgr`):
`headless/README.md`; `content/public/common/content_switches.cc` (lines 580-586);
`content/browser/devtools/devtools_pipe_handler.cc` (335-346, 374-387);
`media/base/media_switches.cc` (61, 181-183).

DevTools protocol: `github.com/ChromeDevTools/devtools-protocol`,
`json/browser_protocol.json` — `Page.startScreencast`, `Page.screencastFrame`,
`Page.screencastFrameAck`, `Page.captureScreenshot`, `ScreencastFrameMetadata`,
`Input.dispatchMouseEvent`, `Input.dispatchKeyEvent`, `Input.synthesizeScrollGesture`.

Alpine packaging: `gitlab.alpinelinux.org/alpine/aports/-/raw/master/community/chromium/{APKBUILD,chromium-launcher.sh}`
(`proprietary_codecs=true`, `ffmpeg_branding="Chrome"`, `chrome_sandbox` 4755,
the `# explicit depends for --headless` comment), and the packaged
`netsurf-3.11-r1.apk` file list (`usr/bin/netsurf`, `usr/bin/netsurf-gtk3`).

NetSurf: `github.com/netsurf-browser/netsurf` — `docs/building-Framebuffer.md`,
`docs/using-framebuffer.md`, `docs/UnimplementedJavascript.md`,
`docs/netsurf-fb.1`, `Makefile.defaults` (`NETSURF_USE_VIDEO := NO`,
`NETSURF_USE_DUKTAPE := YES`), `frontends/framebuffer/{Makefile.defaults,
framebuffer.c,gui.c,font_freetype.c,fbtk/event.c}`,
`content/handlers/html/{layout.c,layout_flex.c,table.c}`.

libnsfb: `github.com/netsurf-browser/libnsfb` — `include/libnsfb.h`,
`include/libnsfb_event.h`, `src/surface/{Makefile,surface.c,ram.c,vnc.c}`;
commit `b2bb5654` (2018-07-29) removed the linux framebuffer and ABLE surfaces.

GTK Broadway: `gitlab.gnome.org/GNOME/gtk`, branch `gtk-3-24` —
`gdk/broadway/broadway-protocol.h` (16 opcodes, 15 event types, the input message
structs), `broadway-output.c` (PUT_BUFFER framing), `broadway-buffer.c` (the
4-byte-symbol encoder, described in the comment at 145-170), `broadway-server.c`
(WebSocket, `/broadway.js`, `/socket`), `gdkbroadway-server.c:320-321` (a
`BROADWAY_REPLY_EVENT` becomes `_gdk_broadway_events_got_input`),
`gdkeventsource.c:111` (that function, which builds the `GdkEvent`s — this is the
evidence that input returns).

WebKit: `github.com/WebKit/WebKit` —
`Source/WebKit/UIProcess/gtk/HardwareAccelerationManager.cpp`,
`Source/WebKit/UIProcess/gtk/AcceleratedBackingStore.cpp`.

Qt: `doc.qt.io/qt-6/embedded-linux.html`,
`doc.qt.io/qt-6/qtwebengine-platform-notes.html`.

CEF: `github.com/chromiumembedded/cef/blob/master/include/cef_render_handler.h`.

libjpeg-turbo: `github.com/libjpeg-turbo/libjpeg-turbo/blob/main/src/turbojpeg.h`.

Chrome headless policy: `developer.chrome.com/blog/removing-headless-old-from-chrome`.
