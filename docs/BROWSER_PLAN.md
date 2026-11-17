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

The package numbers are *installed* sizes resolved transitively by
`so:`-provides, because that is the number that matters for a disk image and a
RAM budget. They were produced by resolving `D:` fields in the index, not by
installing; a real `apk add` may pull a few extra `cmd:` or virtual packages.

---

## 1. The machine we are actually targeting

| Fact | Value | Source |
|---|---|---|
| Guest RAM | **256 MB** | `image.sh:54` → `-m 256` |
| Disk | **64 MB**, FAT, formatted on first boot | `image.sh:51` → `dd ... count=64`, `stage/init` |
| Rootfs today | 29 MB staged, 6.5 MB compressed | `du -sh stage`, `README.md` |
| Screen | 1280×800, 32 bpp, stride from `FBIOGET_FSCREENINFO` | `src/fb.c:29-49`, `README.md` |
| Pixel format | `0xAARRGGBB` in a `u32`; on little-endian that is byte order **B,G,R,A** | `src/mystic.h:17-21`, `docs/IPC.md` |
| Compositor buffers | two full-screen `calloc` surfaces (~4.1 MB each at 1280×800) | `src/main.c:967-971` |
| Window send path | text protocol on `/run/mystic/wm.sock` + shm in `/dev/shm` (tmpfs) | `src/ipc.h`, `src/wmsrv.c`, `stage/init` |
| Max window | 80..1600 × 60..1200, clamped | `src/wmsrv.c:114-117` |
| Stride sent to apps | `w` exactly | `src/wmsrv.c:133` |
| Frame semantics | `FRAME` = whole window, **no damage rectangles** | `src/ipc.h:17`, `docs/IPC.md` |
| Users | everything runs as root; `stage/init` `exec`s the desktop directly, no login, no `su` | `stage/init:140-151` |
| App binaries | static musl, cross-compiled by zig (`-target x86_64-linux-musl -static`) | `tools/build-desktop.sh:42`, `tools/mkapp.sh:37` |
| Fonts | three TTFs in `/usr/share/fonts`, no fontconfig, no font packages | `stage/usr/share/fonts`, `src/main.c:29` |
| Network | DHCP via `udhcpc` when `eth0` exists | `stage/init` |

Three consequences that shape every option below:

**(a) Chrome-sized things do not fit. Not yet.** 64 MB of disk and 256 MB of
RAM is not enough for any Chromium-class engine, at any compression. Section
6 lists exactly which knobs have to move, with numbers.

**(b) Our app binaries are static.** A Mystic app is `zig cc -static`. It
cannot link against Alpine's shared `libwebkit2gtk` or `libQt6WebEngineCore`.
So the browser cannot *be* the app: the app must be a thin static client
talking to a dynamically-linked engine process. That is a constraint the
protocol already supports — an app is just a process with a socket and a
buffer — but it decides the architecture for every candidate.

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
| Download / installed | 152.09 MB / **330.78 MB** **[pkg]** |
| Full runtime closure | **181 packages, 799.5 MB installed, 318.5 MB download** **[pkg]** |
| Smaller sibling | `chromium-headless-shell` 156.0.8078.79-r0: 89.97 MB apk, 208.17 MB installed, closure **682.0 MB** **[pkg]** |
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
Our `MOUSE <x> <y> <buttons>` maps 1:1 (buttons bitmask 1/2/4 is Chromium's
1/4/2 order — a two-line translation table).

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
  the right speed with silent audio. Real sound would mean either our own ALSA
  sink in a helper process or `--use-fake-device-for-media-stream`, and that is
  a separate project.

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

**Cannot:** fit in 256 MB / 64 MB; provide audio without a sink; give us
sub-frame latency; run without `--no-sandbox` as root; avoid pulling LLVM (via
`mesa-dri-gallium`) even with `--disable-gpu`.

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
`libvncserver` is **0.40 MB installed** **[pkg]**.

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
`BROADWAY_REPLY_EVENT` messages which `_gdk_broadway_events_got_input()`
converts into GDK events (`gdkdisplay-broadway.c`:320-321 **[src]**).

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
of impossibility, but it is not worth weeks to disprove when option 1 exists.

### 5.5 The escape hatch nobody likes: Xvfb (shape B, via X11 in a box)

`xvfb` 21.1.26-r0 is 1.45 MB installed **[pkg]**. It is a real X server that
draws into memory and needs no display. Then *any* X11 browser works — Chromium,
Firefox, Dillo, links -g. Get pixels with `XShmGetImage` on the root window
(32-bit `ZPixmap`, which on little-endian is BGRA bytes, i.e. **another
memcpy**), send input with XTEST (`xdotool` is 0.12 MB **[pkg]**).

The bill: `xvfb`'s closure is **54 packages, 268.5 MB**, and 190.97 MB of that
is `llvm22-libs` pulled in by `mesa-egl`/`mesa` **[pkg]**. Add Chromium's
682 MB on top. It also means shipping X11 in an OS whose whole pitch is that it
has no X11 — a positioning problem, not a technical one, and one the owner
should decide rather than the plan.

Its real advantage: **no codec in the frame path.** XShmGetImage gives raw
pixels at whatever rate the page produces them, so video at 30 fps costs 3.5 MB
of memcpy per frame instead of 30 JPEG decodes. If the CDP screencast turns out
to be too slow to watch a video, this is the fix, and it costs 268 MB.

### 5.6 Same idea, Wayland flavour

`weston --backend=headless` (closure 271.4 MB, same LLVM problem **[pkg]**) plus
Chromium with `--ozone-platform=wayland`, then read pixels from Weston's
headless output. This is "implement Wayland by proxy" and buys nothing over
Xvfb except a newer protocol. Not recommended.

---

## 6. What has to change on the OS side first

Irrespective of which engine wins, the machine as configured cannot host one.
These are the knobs, with the arithmetic.

| Knob | Now | Needed for Chromium-class | Where |
|---|---|---|---|
| Guest RAM | 256 MB | 1024–2048 MB | `image.sh:54` |
| Disk | 64 MB | 2048 MB (682 MB packages + browser cache/profile + swap) | `image.sh:51` |
| `/dev/shm` | tmpfs, default 50 % of RAM → 128 MB | same or bigger; Chromium wants it | `stage/init` (`mount -t tmpfs none /dev/shm`) |
| Root/sandbox | runs as root | `--no-sandbox`, or a non-root `browser` user | `stage/init` |
| Fonts | 3 TTFs, no fontconfig | several font families + a `fonts.conf` | `stage/usr/share/fonts` |
| Timezone/locale | none | `/etc/localtime` or `TZ`, else TLS and dates misbehave | new files |
| CA bundle | none in the image | `/etc/ssl/certs/ca-certificates.crt` (**~200 KB**) or every HTTPS site fails | new |
| DNS | works: the DHCP script writes `/etc/resolv.conf` | nothing — listed only so nobody goes looking | `stage/etc/udhcpc.script:13-14` |

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

One row per candidate, one column per question that was asked. Numbers from
§3-5; "renders all sites" means a real engine, not a subset.

| Candidate | Binary to run | Installed (closure) | musl? | glibc? | dbus? | fontconfig? | All sites? | Video? | Sound? |
|---|---|---|---|---|---|---|---|---|---|
| Chromium headless + CDP pipe | `chromium-headless-shell` | 682.0 MB **[pkg]** | yes (Alpine) | no | `dbus-libs` linked; need **unverified** | yes | **yes** | **yes**, software | no sink |
| Chromium under Xvfb | `chromium` + `xvfb` | 799.5 + 268.5 MB **[pkg]** | yes | no | `dbus-libs` linked | yes | **yes** | **yes** | no sink |
| X11 Firefox under Xvfb | `firefox` + `xvfb` | 290.5 + 268.5 MB **[pkg]** | yes | no | `dbus-libs` linked | yes | **yes** | **yes** | `libasound`/PulseAudio linked, no sink |
| WebKitGTK over Broadway | `MiniBrowser` or our own GTK app | 569.1 MB + GStreamer **[pkg]** | yes (Alpine) | no | **yes** (session + `xdg-dbus-proxy`) | yes | yes, minus Chrome-only/DRM | via GStreamer | yes with a GStreamer sink |
| QtWebEngine + eglfs | any Qt app | 796.1 MB **[pkg]** | yes (Alpine) | upstream says required | `dbus-libs` linked | yes | yes | yes | yes — but it owns the display |
| NetSurf, RAM or VNC surface | `nsfb` | single-digit MB *[est]* | yes | no | **no** | **no** | **no** | **no** | no |
| CEF off-screen | our host + `libcef` | 200 MB+ *[est]* | would need a build | official builds are glibc | linked | yes | yes | yes | **yes** (`CefAudioHandler`) |
| links / dillo / w3m | as packaged | 9.8 / 25.9 / 1.5 MB **[pkg]** | yes | no | no | dillo only (via FLTK) | no | no | no |

Three things fall out of that table and they are the whole decision:

1. **Anything that renders every site costs ≥ 570 MB and needs an engine we do
   not have a display path for yet.**
2. **The one candidate that fits the current machine is the one that cannot
   render modern sites.**
3. **Only two designs put pixels in our memory without a codec or a protocol
   translation in between:** CEF off-screen (no musl build exists) and NetSurf's
   RAM surface (no input, needs a patch). Everything else is a decode or a
   protocol.

### 7.2 The ranking

Ranked by "does it meet the requirement", then "how much of it is our code",
then "what it costs".

#### 1. Chromium headless over `--remote-debugging-pipe` — **8–12 days**

The recommended first attempt, because it is the least invention: a NUL-delimited
JSON loop, `Page.startScreencast` for frames, `Input.dispatch*` for input,
libjpeg-turbo straight into `TJPF_BGRX`, `memcpy` into the shm.

- Needs: 682 MB packages (headless shell) or 799 MB (full chromium), ~1 GB RAM,
  2 GB disk, `--no-sandbox`, a CA bundle, `--autoplay-policy`, an evdev→
  (code,key,VK) table.
- Work: CDP client ~700 lines; key table ~120 lines; window chrome (URL bar,
  history, tabs if wanted) in our own style, which we get to draw ourselves;
  ~8–12 days including an honest latency measurement.
- **Cannot:** be small; play audio without a sink; avoid LLVM in the dependency
  closure; touch-type reliably in non-Latin scripts without more work on the
  key table; give <16 ms input-to-photon.

#### 2. NetSurf on libnsfb, VNC surface (`-f vnc`) — **8–12 days**

The only option that fits in the *current* machine, and the only one that needs
no patch to the browser at all.

- Needs: build NetSurf + ~12 libraries for musl (or find an Alpine build path);
  `libvncserver` 0.40 MB; fonts.
- Work: RFB client ~700 lines (handshake, SetPixelFormat, incremental updates,
  Raw + Hextile); keysyms; the browser chrome is NetSurf's own fbtk UI, so we
  get a toolbar for free but it will not look like Mystic.
- **Cannot:** run modern JS (1539 unimplemented bindings, upstream's own count);
  play video; render CSS Grid; use a GPU. It is a document browser.

#### 3. Chromium or Firefox under Xvfb, `XShmGetImage` + XTEST — **5–8 days**

Fastest to something watchable, because raw pixels at full rate beat a JPEG
per frame, and every X11 browser already works.

- Needs: `xvfb` closure **268.5 MB** *plus* the browser; 1.5–2 GB RAM.
- Work: spawn Xvfb, `XOpenDisplay`, `XShmGetImage` into our surface, XTEST for
  input (or `xdotool`), window management stays ours.
- **Cannot:** be honest about "no X11"; avoid 191 MB of LLVM; give per-window
  integration (we composite one X root, so a browser popup is a second X window
  we must chase).

#### 4. WebKitGTK + our own Broadway server — **12–18 days**

The biggest prize: one protocol, and then *every GTK application* is a window in
Mystic — a browser included.

- Needs: `webkit2gtk-4.1` closure **569.1 MB** + GStreamer plugins for video;
  D-Bus session; `bubblewrap`/user namespaces (or
  `WEBKIT_DISABLE_COMPOSITING_MODE=1` and no sandbox); a keymap.
- Work: Broadway server ~1200 lines (16 opcodes, 15 event types, the 4-byte
  buffer encoding, zlib); auth; surface lifetime; then the same browser chrome
  as option 1.
- **Cannot:** be small; avoid D-Bus; do GL; avoid GStreamer.

#### 5. CEF off-screen rendering — **20+ days**

Architecturally the best (BGRA into our pointer, dirty rects, audio handler), and
the worst to start: there is no musl CEF, so the first three weeks are a
Chromium-class build.

- **Cannot:** be attempted before someone has built Chromium for musl once.

#### Rejected

- **QtWebEngine + eglfs** — takes the display; Qt's own docs say it terminates
  when asked to share.
- **WPE/Cog headless** — EGL frames, no plain buffer, nothing packaged, LLVM.
- **weston headless + Wayland client** — Xvfb's cost without Xvfb's library of
  working browsers.
- **links -g / dillo / hv3 / Ladybird-fb** — no engine worth the name, or not
  packaged, or needs X.
- **w3m/lynx/elinks** — do this *anyway*, in the Terminal, this week; it is not
  a browser for the desktop but it is 20 minutes of work and it makes the OS
  useful on the day the engine is still not there.

---

## 8. What I would do next, in order

1. **Measure before building.** Boot with `-m 1024` and run
   `chromium-headless-shell --headless --remote-debugging-pipe --no-sandbox`
   against a local page, with a 30-line C or python driver that turns on the
   screencast and times `screencastFrame` → ack. That single experiment answers
   the only question that decides between options 1 and 3: *is JPEG-per-frame
   fast enough on this CPU?* Everything else in this document is already
   settled by upstream source.
2. **Fix the invisible prerequisites** (CA bundle, `/etc/localtime`, fonts,
   `/dev/shm` size, a `browser` user). Half of them will be needed by whichever
   engine wins, and each one otherwise masquerades as an engine bug.
3. **Build the pixel path once, properly:** a `surf_import_bgra(Surface *dst,
   const u8 *src, int src_stride)` helper plus the `HELLO`/shm handshake, shared
   by every candidate. All five viable options end in the same `memcpy`.
4. **Then** option 1. And keep the NetSurf/VNC sketch in a drawer, because it is
   the answer if the owner ever says "it has to run on the 256 MB machine".

---

## 9. Evidence index

Package facts: Alpine v3.24 `APKINDEX` for `main` and `community`, x86_64,
downloaded from `https://dl-cdn.alpinelinux.org/alpine/v3.24/{main,community}/x86_64/APKINDEX.tar.gz`,
dependency closures resolved through `so:`/`cmd:` provides. Package versions
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
`gdk/broadway/{broadway-protocol.h,broadway-output.c,broadway-buffer.c,broadway-server.c,gdkdisplay-broadway.c}`.

WebKit: `github.com/WebKit/WebKit` —
`Source/WebKit/UIProcess/gtk/HardwareAccelerationManager.cpp`,
`Source/WebKit/UIProcess/gtk/AcceleratedBackingStore.cpp`.

Qt: `doc.qt.io/qt-6/embedded-linux.html`,
`doc.qt.io/qt-6/qtwebengine-platform-notes.html`.

CEF: `github.com/chromiumembedded/cef/blob/master/include/cef_render_handler.h`.

libjpeg-turbo: `github.com/libjpeg-turbo/libjpeg-turbo/blob/main/src/turbojpeg.h`.

Chrome headless policy: `developer.chrome.com/blog/removing-headless-old-from-chrome`.
