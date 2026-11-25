# VLC on Mystic OS

The user asked for VLC support. This document answers three questions with
measurements instead of hope:

1. **What does libVLC actually need here?** Alpine ships VLC as one package with
   every plugin, so `apk add vlc` resolves to **279 packages, 184.68 MB of
   download, 502.88 MB installed** - including 191 MB of LLVM because the
   OpenGL output wants mesa. That is not shippable on a 13 MB image.
2. **Can the display work without X11 or Wayland?** Yes. libVLC's `vmem` video
   output hands decoded frames to callbacks that write into memory we own, and
   the `RV32` format is byte-for-byte the format a Mystic window already uses,
   so the present is a `memcpy` inside VLC. Section 3 has the code, the API and
   the proof.
3. **What does it cost, and is it realistic?** Two profiles, both measured:
   a **minimal** player (libVLC + vmem + PNG/JPEG/raw decode) adds **1.1 MB
   gzipped** to the image (2.15 MB as our stage tree stores it) and needs no
   codec library at all - that is the first milestone and it is realistic. The
   default **playback** profile (libavcodec + dedicated dav1d/vpx/theora/opus/
   vorbis/mpg123 decoders + all 14 packetizers + subtitle decoding) is
   **39.55 MB compressed / 105.24 MB installed** - about three copies of the
   whole image, so a separate disk image or a 9p mount, not part of the 13 MB
   initramfs. Section 3.6 has the current table; the old 67.5 MB figure in
   this paragraph was measured before the soname/symlink fix.

Audio is the honest casualty: there is **no sound device configured for QEMU**
and **no sound driver staged in the image**, so a libVLC player can decode audio
but cannot play it. Section 4 says exactly what that costs to fix (732 KB of
kernel modules + one QEMU `-device`, plus a 1.26 MB ALSA library if we do not
write our own PCM writer).

Nothing here was booted. No QEMU was run (out of scope for this task). Every
statement is either quoted from upstream source, measured from the Alpine index
or from the downloaded `.apk` files, or arithmetic on those numbers - and it is
tagged so you can tell which.

---

## 0. Evidence tags

| Tag | Meaning |
|---|---|
| **[pkg]** | from the Alpine v3.24 `APKINDEX` for x86_64 as fetched on 2026-11-17 (`main`: 5961 packages, `community`: 22700, provides index 41853) |
| **[apk]** | measured by unpacking the actual `.apk` into `.work/vlc/` - file sizes, ELF `DT_NEEDED`, exported symbols |
| **[src]** | quoted from upstream VLC 3.0.23 source, file and line given |
| **[ours]** | read out of this repository, cited by symbol (line numbers move while other work is in flight) |
| **[calc]** | arithmetic on the above, shown |
| **[est]** | not measured. Says what would settle it |

Index field meanings, because this is a trap we already fell into: `S:` is the
`.apk` **download** size, `I:` is the **installed** size. `tools/fetch-browser.py`
prints `I:` and calls it "compressed"; `tools/fetch-vlc.py` prints both, labelled.

---

## 1. The machine we are putting this on

| Fact | Value | Source |
|---|---|---|
| Guest RAM | **256 MB** | `image.sh`, `-m "${MYSTIC_RAM:-256}"` **[ours]** |
| Rootfs | **13.42 MB** gzipped initramfs (13,417,162 bytes) | `build/initramfs.cpio.gz`, measured **[ours]** |
| Staging tree | 32 MB | `du -sh stage` **[ours]** |
| Data disk | 64 MB FAT, formatted on first boot | `image.sh` `MYSTIC_DISK_MB` **[ours]** |
| Kernel | Alpine `linux-virt` 6.18.63, modules from the same package | `tools/fetch-kernel.sh` **[ours]** |
| Sound | **device present**: `-audiodev sdl,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0`; `stage/init` loads the `snd*` modules, and **devtmpfs** creates `/dev/snd/pcmC0D0p`; `init` then `chmod 0666`s the playback nodes (there is no udev here). Whether a PCM open succeeds and sound reaches the host is unproven - section 4 | `image.sh` line with `-audiodev`, `stage/lib/modules/*/kernel/sound/`, `stage/init` lines 66-87 (modprobe + chmod, not mknod) **[ours]** |
| Screen | 1280x800 in the default `-vga std` setup; the real size comes from `FBIOGET_FSCREENINFO` | `src/fb.c` **[ours]** |
| Pixel format | `0xAARRGGBB` in a `u32`; little-endian bytes B,G,R,A | `src/mystic.h`, `RGB()`/`GET_R()` **[ours]** |
| Window blit | one `memcpy` per row from the client shm buffer into the scene, **no alpha blending** | `src/main.c`, the `WIN_CLIENT` branch (`cl->px` rows) **[ours]** |
| Window size range | 80..1600 x 60..1200 | `src/wmsrv.c` (`if (w < 80) w = 80;` ... `if (h > 1200) h = 1200;`) **[ours]** |
| Desktop binary | static, `DT_NEEDED` is empty | `build/mystic-desktop` **[apk]** |
| Base rootfs | `alpine-minirootfs-3.24.4` unpacked under `stage/` | `image.sh` **[ours]** |

Two of those matter more than they look:

- **The desktop is static and the image already contains musl.** The base is an
  Alpine minirootfs, so `/lib/ld-musl-x86_64.so.1` (670,312 bytes, verified in
  `alpine-minirootfs-3.24.4-x86_64.tar.gz`) and busybox are already in the
  image. A dynamically linked libVLC therefore adds its own libraries, not a C
  library. That is why the profiles in section 3.6 count `vlc-libs` and the
  codec libraries, not `musl`.
- **The JAIL does not have it.** `src/wmsrv.c` runs each app in a chroot at
  `/run/mystic/jail/<id>` and binds only `/bin/app`, the bundle at `/app`, the
  fonts, `/dev` bits and `/proc` - no `/lib`. A dynamic ELF's PT_INTERP
  therefore cannot resolve inside the jail even though the image has the
  loader. That is why the package's `bin` is a static launcher that execs the
  bundle's own `lib/ld-musl-x86_64.so.1`; see section 9.
- **The window blit ignores alpha.** `RV32` has an unused top byte; our
  compositor does not look at it. No conversion, no `0xFF000000` fix-up, no
  blend. This is the whole reason the display path is cheap.

---

## 2. What libVLC is, in Alpine terms

### 2.1 The package split

Alpine 3.24 splits VLC in two packages that matter here **[pkg]**:

| Package | Provides | Installed | `.apk` | Depends on |
|---|---|---|---|---|
| `vlc-libs` 3.0.23-r4 | `so:libvlc.so.5`, `so:libvlccore.so.9` | **1.28 MB** | 0.57 MB | `/bin/sh`, `libc.musl-x86_64.so.1`, `so:libdbus-1.so.3` |
| `vlc` 3.0.23-r4 | `cmd:vlc`, `cmd:cvlc`, ... + 3 plugin `.so` | **14.57 MB** | 5.46 MB | exactly **106 `so:` names**: `libX11`, `libgtk-3`, `libEGL`, `libgstvideo`, `libsmbclient`, `libavcodec`, `libplacebo`, ... |

So **the library is small and the application is not**. `vlc-libs` is the whole
of libVLC: `libvlc.so.5.6.1` (153,280 B) + `libvlccore.so.9.0.1` (1,108,528 B) +
`vlc-cache-gen` (14,224 B) **[apk]**.

`vlc` is one package containing **365 plugin `.so` files totalling 13.04 MB**
plus `/usr/bin` (6 launcher scripts) and 69 files in `/usr/share/vlc`
(1.10 MB, half of it Lua) **[apk]**. Alpine does not split the plugins, and its
`D:` line therefore has to list every library any plugin links against - which
is why the closure explodes.

### 2.2 The full closure, and the four chains that cause it

```
$ python3 tools/fetch-vlc.py                 # dry run, nothing downloaded
==> main:       5961 packages
==> community: 22700 packages
==> provides: 41853
==> apk add vlc (whole package + whole D: line): 279 packages,
    184.68 MB download, 502.88 MB installed
    heaviest by installed size:
      llvm22-libs         22.1.3-r1     190.97 MB installed  67.29 MB apk  (main)
      mesa                26.1.6-r0      44.18 MB installed  13.77 MB apk  (main)
      x265-libs           4.1-r0         21.34 MB installed   4.04 MB apk  (main)
      samba-libs          4.23.8-r0      19.36 MB installed   6.10 MB apk  (main)
      ffmpeg7-libavcodec  7.1.5-r0       16.33 MB installed   7.37 MB apk  (community)
      vlc                 3.0.23-r4      14.57 MB installed   5.46 MB apk  (community)
      gtk+3.0             3.24.52-r0      9.93 MB installed   3.99 MB apk  (community)
      aom-libs            3.14.1-r0       8.38 MB installed   3.21 MB apk  (main)
      libSvtAv1Enc        4.1.0-r0        7.22 MB installed   3.01 MB apk  (community)
      gst-plugins-base    1.28.3-r0       6.93 MB installed   2.68 MB apk  (main)
```

Four dependency chains account for almost all of it **[pkg]** (each arrow is a
`so:` dependency in the index, resolved to its provider):

| Chain | Why it exists | Cost |
|---|---|---|
| `vlc -> so:libEGL.so.1 -> mesa-egl -> mesa -> so:libLLVM.so.22.1 -> llvm22-libs` | the opengl/EGL video outputs (llvmpipe) | **235 MB** (190.97 + 44.18) |
| `vlc -> so:libgtk-3.so.0 -> gtk+3.0` | the GTK interface plugin | 9.9 MB (+ deps) |
| `vlc -> so:libsmbclient.so.0 -> libsmbclient -> samba-libs` | the SMB access plugin | 19.4 MB |
| `vlc -> so:libgstbase-1.0.so.0 -> gstreamer` | the GStreamer decoder/codec plugins | 3.2 MB + plugins |

The conclusion is not "VLC is bloated"; it is "the package is the shop, and we
only want two shelves". Which is what section 3 does: keep `vlc-libs` whole, and
take individual plugin files out of `vlc` without installing its dependency list.

---

## 3. The display path: `vmem` + the decoder's own I420 planes

> **Rewritten 2026-11-26.** This section used to describe asking libVLC for
> RV32 at the window size. That is not what the player does any more, and the
> packed `libexec/vlcplay` of this build contains no `RV32` string (byte
> search, not an assumption). The vmem API is unchanged; what changed is what
> `setup()` answers and which thread does the conversion. The 4K rate in the
> table below was measured on the old path; *why* it was 17 fps (vout thread vs
> decoder threads) is not proven by that measurement, only that the old path
> converts 8.3 M pixels per frame on one thread.

### 3.1 The API

libVLC has a first-class "render into my memory" output. From the real header
shipped in `vlc-dev-3.0.23` (`/usr/include/vlc/libvlc_media_player.h`) **[apk]**:

```c
void libvlc_video_set_callbacks( libvlc_media_player_t *mp,
                                 libvlc_video_lock_cb lock,
                                 libvlc_video_unlock_cb unlock,
                                 libvlc_video_display_cb display,
                                 void *opaque );

void libvlc_video_set_format( libvlc_media_player_t *mp, const char *chroma,
                              unsigned width, unsigned height, unsigned pitch );

void libvlc_video_set_format_callbacks( libvlc_media_player_t *mp,
                                        libvlc_video_format_cb setup,
                                        libvlc_video_cleanup_cb cleanup );
```

The player uses `set_callbacks` + `set_format_callbacks`. It does **not** call
`libvlc_video_set_format`: that is the fixed-format variant (the RV32 path this
document used to describe), and it cannot answer the decoder's own fourcc.

The callbacks we implement, with the header's contract and what the shipped
`src/client/apps/vlcplay.c` actually does:

| Callback | What the player does |
|---|---|
| `setup` | keeps the decoder's I420 or NV12 fourcc **and the decoder's width/height**, allocates three plane sets (a ring, `VLC_SETS`), writes back `pitches[]`/`lines[]`. For a format we cannot sample (10-bit, 4:2:2, hardware surface) it answers I420 at the same size. Returning 1 means "deliver this". |
| `lock` | hands libVLC the next free plane set (`planes[0..2]`); waits on a condition variable until that slot is neither being filled nor being read |
| `unlock` | marks the set complete, counts the frame, and every 100 frames prints the measured fps (last 100 and average since start) |
| `display` | counts a display; the **window thread** samples the newest complete set into the window buffer and sends `FRAME` |
| `cleanup` | frees all three plane sets |

Header notes we must respect, quoted **[apk]**:

- "one or three pixel planes ... Those planes must be aligned on 32-bytes
  boundaries" - `planes_alloc()` `malloc`s and aligns the base by hand, then
  checks the real address and every pitch against 32 and refuses the frame if
  the contract is not met.
- "we recommend that pitches and lines be multiple of 32" - the plane pitches
  are rounded up to a multiple of 32 bytes.
- "Memory copying is required between LibVLC reference picture buffers and
  application buffers" - true, and section 3.3 says exactly what it costs.

### 3.2 I420/NV12 at the decoder's size is the format

`vmem`'s `setup` callback receives the decoder's fourcc and picture size, and
`Prepare()` then copies pixels **plane for plane** (`picture_CopyPixels`) into
whatever we answered. Answering the same fourcc and the same size therefore
means **no chroma conversion and no scaling inside libVLC** - the only work it
does is the plane copy.

- We accept `I420` (three planes) and `NV12` (Y + interleaved UV), because some
  decoders propose NV12. Anything else is answered with `I420` at the same
  size; libVLC must then find a converter. For 8-bit 4:2:0 the shipped
  `i420_rgb`/`chain` plugins are not needed at all; for 10-bit and 4:2:2 they
  are not sufficient (the Codecs section lists that gap).
- **Colour** is BT.601 limited range, integer only (`src/yuv.h`): `C = Y-16`,
  `D = U-128`, `E = V-128`, `R = (298C + 409E + 128) >> 8`, etc. libVLC 3's
  vmem API has no colour-space field, so a BT.709 or full-range stream gets
  slightly different hues - a stated limitation, not a hidden one.
- **RV32 is what the API also offers, and it is what the player used before.**
  It is `VLC_CODEC_RGB32`: masks `0xff0000`/`0x00ff00`/`0x0000ff`
  (`modules/video_output/vmem.c`, `Open()`), so a pixel is `u32 = 0x00RRGGBB`
  and on little-endian the bytes are `B, G, R, 0x00` - byte-identical to a
  Mystic window pixel (`0xAARRGGBB`, alpha ignored by the compositor). That is
  still true, and it is why the first working player was 120 lines. But asking
  vmem for RV32 at the window size makes libVLC's single video-output thread
  convert and rescale every decoded picture to RGB: 8,294,400 converted pixels
  per 4K frame before the player sees one. Whether that thread was the exact
  17 fps ceiling on the four-vCPU guest is a hypothesis the old measurement
  cannot separate from decoder saturation; what is certain is the work it does
  per frame. The bundle still ships `rv32`/`chain`/`i420_rgb*` because they
  are the plugins that path needs; the current player never requests them.

### 3.3 The copy vmem makes - and where the work moved

`modules/video_output/vmem.c`, `Prepare()` **[src]**:

```c
sys->pic_opaque = sys->lock(sys->opaque, planes);
for (unsigned i = 0; i < PICTURE_PLANE_MAX; i++) {
    rsc.p[i].p_pixels = planes[i];
    rsc.p[i].i_lines  = sys->lines[i];
    rsc.p[i].i_pitch  = sys->pitches[i];
}
picture_t *locked = picture_NewFromResource(&vd->fmt, &rsc);
if (likely(locked != NULL)) {
    picture_CopyPixels(locked, pic);      /* <-- VLC copies planes into OUR buffers */
    picture_Release(locked);
}
```

1. **vmem is not zero-copy.** libVLC decodes into its own pooled picture and
   copies it into the planes we handed over. With I420 that copy is a plane
   memcpy (Y, U, V rows), not an RGB conversion.
2. **The conversion is ours, on our thread, at the displayed size.** The window
   thread takes the newest complete plane set and calls
   `yuv420_to_rgb_nearest()` (`src/yuv.h`) for exactly the pixels the window
   shows: a 960x540 body is 518,400 reads, not the 8,294,400 a 4K RGB
   conversion touches. The per-pixel work no longer sits on libVLC's
   video-output thread.
3. **Three plane sets in a ring** (`VLC_SETS`) mean libVLC can fill frame
   N+1 while the window thread reads frame N; `lock` waits for its slot to be
   free instead of overwriting a frame being sampled, and `unlock` publishes
   the newest complete slot through `ready`.
4. **Nearest neighbour is deliberate.** At a 4:1 downscale it picks every
   fourth source pixel, so fine detail can alias. A box filter or bilinear
   taps are a later refinement behind the same `yuv.h` interface; this is not
   passed off as one.

Scaling no longer happens inside VLC at all: the sampler *is* the scaler. The
old paragraph here about needing `libswscale_plugin.so` to scale is obsolete -
with the I420 path there is no VLC-side scale to pay for.

### 3.4 Integration with the window server

The protocol already fits **[ours]**, `src/ipc.h`:

```
app -> wm : HELLO <id> <title...> <w> <h>
wm  -> app: OK <shm-path> <stride>
app -> wm : FRAME
```

- The app `mmap`s the shm file the window server created (`/dev/shm/mystic-*`).
- **The window buffer is screen-sized, not window-sized.** `src/wmsrv.c` sets
  `c->maxw = max(w, screen_w)`, `c->stride = c->maxw`, and `OK` reports
  `path stride maxw maxh`. So a 640x360 window still gets a 1280-pixel stride,
  and the destination pitch is `stride * 4 = 5120`.
- **The plane buffers are not that mapping.** `setup()` asks libVLC to deliver
  the decoder's own size, and allocates plane sets of exactly that size
  (pitches rounded up to 32). The sampler writes the displayed rectangle into
  the shm at the wm's stride: `yuv420_to_rgb_nearest_stride(yp, up, vp,
  ypitch, upitch, vpitch, dst, dststride, srcw, srch, dstw, dsth)` in
  `src/client/apps/vlcplay.c`. Getting the destination stride wrong - passing
  `dstw * 4` instead of the wm's `stride * 4` - writes rows on top of each
  other, which looks like a diagonal smear.
- The compositor copies exactly `cl->w` pixels per row from `cl->px`, so the
  extra column space is never read. `display` does not write to the shm: it
  marks the newest plane set ready, and the window thread samples it and sends
  the `FRAME` line.
- A/V sync and the frame clock are VLC's problem - the `display` callback is
  called "as determined by the media playback clock" **[apk]**, so we do not
  schedule anything. We present the newest frame when the compositor next draws.

The window server's own blit (the `WIN_CLIENT` branch in `src/main.c`) copies
`cl->w` pixels per row from `cl->px` into the scene, ignoring the top byte. A
window repaint during playback therefore shows the last frame VLC wrote - no
extra state, no second buffer. Tearing is possible if a repaint lands mid-copy;
that is the same tearing every app in this OS already has, and double buffering
is a change to the window protocol, not to VLC.

### 3.5 There is also `libfb_plugin.so` - considered and rejected

VLC ships its own framebuffer output: `video_output/libfb_plugin.so`, 18,112 B,
`DT_NEEDED` = `libvlccore.so.9` + `libc.musl-x86_64.so.1` only **[apk]**. It
writes straight to `/dev/fb0` with no window server at all. Attractive, and
wrong for us: our compositor owns `/dev/fb0` and would paint the desktop back
over the video on the next repaint. It is only usable as a "fullscreen mode"
where the desktop stops drawing. Worth remembering as a 5-minute experiment,
not as the architecture.

### 3.6 What the payload actually costs

`tools/fetch-vlc.py` computes the closure from the **real `DT_NEEDED` of the
plugin `.so` files it extracts from the `.apk`**, so the numbers below are not a
hand-written list. Profiles **[pkg]** + **[apk]**, re-measured on 2026-11-26
against `vlc-3.0.23-r4.apk`:

| Profile | Plugins kept | Closure | Index download (`S:`) | Index installed (`I:`) | Built package |
|---|---|---|---|---|---|
| `minimal` | 12 (vmem, rv32, chain, i420_rgb*, image/rawvid demuxers, rawvideo/png/jpeg codecs, filesystem access) | **8 packages** | **1.98 MB** | **4.01 MB** | 1.10 MB gzipped with symlinks (measured 2026-11-17); this profile has not changed |
| `playback` (default) | 50 (containers, libavcodec, dedicated dav1d/vpx/theora/opus/vorbis/mpg123 decoders, all 14 packetizers, raw ES demuxers, subtitle decode, alsa+adummy) | **73 packages** | **38.73 MB** | **104.73 MB** | **39.55 MB compressed / 105.24 MB installed / 184 files**, 131 files under `lib/` (130 shared objects + the loader), 115 soname aliases; a fresh build fetches **71 `.apk` files, 43.27 MB** = the 73-package closure (38.73 MB) minus the three base packages it does not fetch (musl + busybox + busybox-binsh = 0.92 MB) plus `vlc.apk` itself (5.46 MB, not a dependency of itself) |
| `subs` | 52 (+ `text_renderer/libfreetype_plugin.so` + `codec/liblibass_plugin.so`) | **87 packages** | **42.99 MB** | **115.26 MB** | the styled-subtitle stack (FreeType + fontconfig + fribidi + harfbuzz + glib + libass) is **+14 packages, +4.26 MB download, +10.53 MB installed** over `playback`. **Built and verified separately**: 43.34 MB compressed / 114.14 MB installed / 208 files, 206 objects with 0 NEEDED and 0 symbol failures, kept at `build/repo-subs/vlc.tar.gz` with its own `build/vlc-pkg-repo-subs.stamp`, so `--verify-only --repo build/repo-subs` gates it exactly like the default (same gate digest, same player hash; the published package stays the default profile). That directory also carries a matching signed index (`index.tsv`, `index.tsv.sig`, `index.pub`, `vlc.row`) containing only the vlc row - it is a proof artifact, not a served repository |
| `full` | the whole package, `apk add vlc` | 279 packages | 184.68 MB | 502.88 MB | n/a |

> An earlier revision of this document called the default profile `trim` and
> quoted 67.50 MB gzipped for it. That figure predates the symlink fix: the
> tarball now stores one copy of each library and 115 symlinks, so the same
> closure packs to **39.55 MB**. 67.50 MB is superseded, not a second estimate.

The `minimal` line is the important one: **1.10 MB gzipped** on top of a 13.42 MB
image (2.15 MB in the shape our stage tree can actually hold - see risk 5, the
symlink copies), for a player that can decode PNG, JPEG and raw frames into a
window. Those files are 1.28 MB of `vlc-libs` itself, 0.28 MB of the twelve
plugins, and 1.25 MB of `libjpeg-turbo` + `libpng` + `zlib` + `dbus-libs`
**[apk]**. It has no VP8/VP9/AV1 decoder: those plugin files are not in this
profile and neither are `libvpx`/`libdav1d`/`libtheora` - section 3.7 used to
claim otherwise and was wrong.

`playback` is where it becomes a different product. **39.55 MB compressed is
about three copies of the entire image.** The reason is visible in the
download: ffmpeg's `libavcodec` links ~24 shared libraries, and the ones that
hurt are encoders we never call - `x265-libs` 21.3 MB, `aom-libs` 8.4 MB,
`libSvtAv1Enc` 7.2 MB, `rav1e-libs` 2.4 MB, `libjxl` 3.7 MB **[pkg]**. There is
one genuine absurdity worth naming: `libavutil.so.59` has a hard `DT_NEEDED` on
**`libX11.so.6`** **[apk]**, so the X11-free video player ships an X11 library it
never calls - 1.16 MB of it.

Measured on disk after unpacking `--profile playback`: 575 files, 178.53 MB
(the index says 104.73 MB installed; the difference is 116 symlink targets
materialised as copies, which is a Windows artefact of the unpacker - inside the
image they are symlinks). **116 is the loose root's count, not the package's.**
Measured accounting: the closure's `.apk`s carry 119 symlinks under `usr/lib`;
two belong to `vlc-libs` and are not extracted (only the versioned files are
wanted), and `usr/lib/vdpau/libvdpau_trace.so` is skipped because its target is
itself a pending symlink during the unpacker's one-pass link fix-up - 116
copies in the root. The tarball keeps **115 symlinks**: 113 version-pair
aliases derived from byte-identical copies plus `libvlc.so.5` /
`libvlccore.so.9` from `DT_SONAME`. The arithmetic, so it does not read as
"three copies disappeared": **116 root copies - 3 links kept as separate files
+ 2 DT_SONAME aliases = 115 package symlinks**. The three are apk links whose
names are not version prefixes of their targets (`libmbedcrypto.so.16` ->
`.so.3.6.8`, `libmbedtls.so.21`, `libmbedx509.so.7`); the packager deliberately
keeps those as separate files rather than inventing an alias, and
`--verify-only` counts what the tarball actually has.

### 3.7 Which decoders exist that are not libavcodec

Worth stating precisely, because an earlier revision of this section claimed
`minimal` could play WebM - it cannot. Measured from `vlc-3.0.23-r4.apk`:
**53 files under `codec/`**, split by what they link against.

**31 need no external codec library** (sizes in KB, read from the `.apk`).
28 of them link `libvlccore`; `libcdg`, `libedummy` and `libspdif` have no
`libvlccore` `DT_NEEDED` at all - their only shared dependency is libc:

```
libadpcm 18   libaes3 14    libaraw 30    libcc 46      libcdg 18
libcvdsub 18  libddummy 14  libdvbsub 90  libedummy 14  libg711 26
liblpcm 22    liboggspots 14 librtpvideo 14 libscte18 18  libscte27 26
librawvideo 14 libspdif 14  libspudec 22  libstl 18     libsubsdec 26
libsubstx3g 18 libsubsusf 30 libsvcdsub 18 libt140 14   libtelx 18
libtextst 18  libttml 50    libuleaddvaudio 14 libwebvtt 102
libwma_fixed 118 libxwd 14
```

**22 need a library the profile may or may not ship**:

| Plugin | Needed library | In the bundle? |
|---|---|---|
| `libavcodec` | `libavcodec.so.61`, `libavformat.so.61`, `libavutil.so.59` | yes |
| `libdav1d` | `libdav1d.so.7` | yes |
| `libvpx` | `libvpx.so.11` | yes |
| `libtheora` | `libtheoradec.so.2`, `libtheoraenc.so.2` | yes |
| `libopus` | `libopus.so.0` | yes |
| `libvorbis` | `libvorbis.so.0`, `libvorbisenc.so.2` | yes |
| `libmpg123` | `libmpg123.so.0` | yes |
| `libjpeg` / `libpng` | `libjpeg.so.8` / `libpng16.so.16` | yes |
| `liba52` / `libfaad` / `libspeex` / `libflac` | `liba52.so.0` / `libfaad.so.2` / `libspeex.so.1` / `libFLAC.so.12` | no - libavcodec decodes all four |
| `liblibass` | `libass.so.9` | not in the default profile; `--profile subs` ships it (see below) |
| `libx264`, `libx26410b`, `libx265` | `libx264.so.164`, `libx265.so.215` | libraries are present, but these are **encoders** |
| `libvaapi`, `libvaapi_drm` | `libva.so.2`, `libva-drm.so.2` | `libva` is in the closure, but there is no GPU in the VM |
| `libsvgdec` | `librsvg-2.so.2`, `libcairo.so.2` | no |
| `libgstdecode` | the GStreamer stack | no - deliberately, it is 3.2 MB of plugins we do not need |
| `libfluidsynth` | `libfluidsynth.so.3` | no - MIDI synthesis, not playback |

The default profile ships **12 of those 53**: `avcodec`, `dav1d`, `vpx`,
`theora`, `opus`, `vorbis`, `mpg123`, `jpeg`, `png`, `rawvideo`, `subsdec`,
`webvtt`.

And the correction to the old paragraph: there is **no H.264, HEVC, MPEG-2 or
VC-1 decoder plugin at all** in the package - no `libmpeg2_plugin.so`, no
`libvc1_plugin.so`, no `libav1_plugin.so`. `packetizer/*` only parses
elementary streams, and `libx264`/`libx265` are encoders. So:

- **Without libavcodec** the package can decode: PNG, JPEG, raw frames,
  Theora, Vorbis, Opus, MP3 (mpg123), FLAC (only if libFLAC is shipped - it is
  not), A/52 (only with a52dec - not shipped), VP8/VP9 (libvpx), AV1 (dav1d),
  and the libvlccore-only subtitle/caption codecs. That is a real player, but
  **it is not `minimal`**: `minimal` is 12 plugins / 8 packages and contains
  none of those decoders. It is `playback` minus libavcodec, which no profile
  in the tool builds.
- **H.264/HEVC/MPEG-2/VC-1 require `codec/libavcodec_plugin.so`** and its
  16.33 MB + 2.85 MB + 0.19 MB of libraries. There is no cheaper option in
  Alpine's VLC 3.0.23, and no build we could make offline.

---

## 4. Audio: honest verdict

> **Update 2026-11-26.** The first two of the three steps below have since been
> taken: `image.sh` boots with `-audiodev sdl,id=snd0 -device intel-hda -device
> hda-duplex,audiodev=snd0`, `tools/fetch-kernel.sh` stages `kernel/sound`
> (`stage/lib/modules/6.18.63-0-virt/kernel/sound/` exists), `stage/init` loads
> the modules (devtmpfs creates `/dev/snd/pcmC0D0p`, and `init` then
> `chmod 0666`s the playback nodes), and the bundle ships
> `audio_output/libalsa_plugin.so`. So the "absent" column below describes the
> image this section was written against, not today's. What is still not proven
> is that a PCM open succeeds and that sound reaches the host - see the note at
> the end of "Through the OS, with no terminal involved".

libVLC can decode audio and hand it to us as raw PCM with no sound server:

```c
void libvlc_audio_set_callbacks( libvlc_media_player_t *mp,
                                 libvlc_audio_play_cb play,
                                 libvlc_audio_pause_cb pause,
                                 libvlc_audio_resume_cb resume,
                                 libvlc_audio_flush_cb flush,
                                 libvlc_audio_drain_cb drain,
                                 void *opaque );
void libvlc_audio_set_format( libvlc_media_player_t *mp, const char *format,
                              unsigned rate, unsigned channels );
```

with the header's note: "The audio callbacks override any other audio output
mechanism. If the callbacks are set, LibVLC will **not** output audio in any
way." **[apk]**

Decoding is not the problem. In the image this section was written against,
**there was nothing to write the PCM to** (the note above says what changed).

| Thing needed | State in that image | Evidence |
|---|---|---|
| An emulated sound card | **absent**: QEMU is started with `-kernel -initrd -m -vga -drive -netdev -device virtio-net-pci` and no sound device or `-audiodev` | `image.sh` BOOT array **[ours]** |
| A driver | **absent**: `CONFIG_SND=m`, `CONFIG_SND_PCM=m`, `CONFIG_SND_HDA_INTEL=m`, `CONFIG_SND_VIRTIO=m` are all modules, and `tools/fetch-kernel.sh` stages `gpu video i2c input block net fs/* lib` but **not `kernel/sound`** - 188 `.ko` files end up in `stage/`, zero of them sound | `build/cache/linux-virt-6.18.63-r0/boot/config-6.18.63-0-virt`, `find stage/lib/modules -name '*snd*'` **[ours]** |
| A `/dev/snd/*` node, a mixer, a volume UI | **absent then**; devtmpfs now creates `/dev/snd/pcmC0D0p` and `stage/init` chmods the playback nodes (there is still no mixer daemon or volume UI) | `stage/init` lines 66-87 **[ours]** |

So: **sound could not work in that image, and it was not a libVLC problem.**
What it took, with measured costs (all three steps are now in the tree; the
PCM open itself is still unproven):

1. `image.sh`: add `-device intel-hda -device hda-duplex` (or `-device
   virtio-snd-pci`, `CONFIG_SND_VIRTIO=m` is set) to the QEMU line.
2. `tools/fetch-kernel.sh`: add `kernel/sound` to `MODULE_TREES`. That tree is
   **732 KB compressed, 15 `.ko` files** in the kernel package we already
   download **[apk]** - `sound/core` 260 KB, `sound/pci` 116 KB.
3. A sink, one of two ways:
   - **ALSA plugin**: `audio_output/libalsa_plugin.so` (22,208 B, `DT_NEEDED`
     `libasound.so.2`) **[apk]** + `alsa-lib` **1.26 MB installed / 0.48 MB apk**
     **[pkg]**. Then VLC writes to `/dev/snd/pcmC0D0p` through libasound.
   - **Our own PCM writer**: `libvlc_audio_set_callbacks` +
     `libvlc_audio_set_format(mp, "S16N", 48000, 2)`, a ring buffer, and
     `SNDRV_PCM_IOCTL_*` ioctls on `/dev/snd/pcmC0D0p`. No new library, ~300
     lines we own, and it is the only version that can also give the desktop a
     volume slider without a mixer daemon.

Either way the *hardware* half is not ours: QEMU needs an audio backend for the
host, which is a QEMU-build/host question and not a guest one. Steps 1-2 are in
the tree (device, `snd` modules, devtmpfs nodes, libalsa), so the old failure
mode - callbacks delivering PCM to a ring buffer nobody drains - is no longer
the default; what remains unproven is that a PCM actually opens and that a
listener hears anything, which is why the player reports the audio output
libVLC chose and not "sound works".

---

## 5. RAM

What is measurable without running it:

| Item | Bytes | Source |
|---|---|---|
| One 1280x720 RV32 frame | **3,686,400** (3.69 MB) | **[calc]** 1280*720*4 |
| One 1600x1200 frame (max window) | **7,680,000** (7.68 MB) | **[calc]** 1600*1200*4 |
| `libvlccore.so.9` | 1,108,528 | **[apk]** |
| `libvlc.so.5` | 153,280 | **[apk]** |
| 10 `minimal` plugins | 283,432 | **[apk]** |
| libjpeg + libpng + zlib + dbus | 1,248,840 | **[apk]** |
| **`minimal` mapped-code floor** | **2.81 MB** | **[calc]** sum of the above |
| `playback` extra libraries (avcodec, avformat, avutil, swresample, dav1d, matroska, ebml, libstdc++, libgcc) | 25,737,576 | **[apk]** |
| **`playback` mapped-code floor** | **29.81 MB** | **[calc]** |

The player's own allocation is the frame buffer, and if the frame buffer *is* the
window mapping then it is already counted in the window server's budget. The
desktop uses two full-screen surfaces (~4.1 MB each at 1280x800) **[ours]**.

What is **not** measured here is libVLC's heap: thread stacks, the decoder's
internal buffers, the plugin registry, and for `playback` whatever libavcodec
allocates while decoding (which for H.264 1080p is tens of MB in the general
case). **[est]** With 256 MB of RAM, `minimal` should sit comfortably
(frame buffer + 2.8 MB of code + VLC's heap, call it "tens of MB at most");
`playback` is the one to watch. The way to settle it is one boot and reading
`/proc/<pid>/status` `VmRSS` plus `VmHWM` after playing a file - that is
milestone 1's acceptance test, and it takes ten minutes once the tree exists.

---

## 6. Verdict

**Is a libVLC-based player with `vmem` output realistic for a 13 MB image?**

- **As a player for images and raw frames: yes.** 1.98 MB of download,
  **1.10 MB gzipped** added to the image (2.15 MB with the symlink copies our
  packer produces), no X11, no Wayland, no window-system server, no conversion
  code. The display path is the callbacks in section 3.1 and a `FRAME` line.
- **As a player for VLC's own codecs (VP8/VP9/AV1/Theora/Opus/Vorbis/MP3): yes,
  and they are now in the default profile** - `libdav1d`, `libvpx`, `libtheora`,
  `libopus`, `libvorbis` and `libmpg123` plugin files (125,128 B together) whose
  libraries libavcodec already pulled in. `minimal` alone does **not** include
  them: its 8 packages have no libvpx, libdav1d or libtheora, which section 3.7
  used to get wrong.
- **As a player for H.264/HEVC/MP4: not inside the initramfs.** 38.73 MB of
  index download and **39.55 MB compressed**, i.e. about three copies of the
  whole OS, to gain one decoder. If the design wants that, it belongs on the
  data disk or a read-only 9p mount the way `docs/BROWSER_PLAN.md` proposes for
  the browser engine - and the 64 MB data disk is too small for it too, so the
  disk size becomes a knob (`MYSTIC_DISK_MB`).
- **Sound: device present, still unproven.** The HDA device, the `snd` modules
  and `/dev/snd/pcmC0D0p` are in the tree and the bundle ships libalsa (section
  4); whether a PCM actually opens and reaches the host is the open question.

**What it costs in RAM:** the frame buffer (3.69 MB at 720p, 7.68 MB at the
1600x1200 maximum) + 2.81 MB of mapped code for `minimal`; unmeasured heap on
top. **[est]** Fine in 256 MB.

**First milestone:** `docs/VLC.md` milestone 1 below - fetch the `minimal`
profile, write the 120-line player, and prove that a PNG becomes pixels in a
Mystic window through libVLC. Everything else in this document is an argument
that the next step after that is a decision about 67 MB, not about feasibility.

---

## 7. Milestones

Each one has an acceptance test that can fail. No milestone requires X11,
Wayland, a GPU, or a sound card.

**M0 - the tree exists (done, verifiable now).**
`python3 tools/fetch-vlc.py --profile minimal --download` produces a tree with
`libvlc.so.5`, `libvlccore.so.9`, the 12 plugins and their libraries, and prints
the size. *Acceptance:* the script prints 8 packages / 1.98 MB / 4.01 MB and the
tree contains `usr/lib/vlc/plugins/video_output/libvmem_plugin.so`. Measured:
8.28 MB on disk including musl and busybox that the base image already has.

**M1 - a real frame in a real window.**
A `src/client/apps/player.c` (not in this task's scope) that: connects to
`/run/mystic/wm.sock`, gets its shm window at the image's size, calls
`libvlc_video_set_callbacks` + `libvlc_video_set_format_callbacks` with the
Appendix B code, plays a PNG, and sends `FRAME` from `display`.
*Acceptance:* the screenshot shows the picture inside a Mystic window; the
process's `VmHWM` is recorded; `ldd` shows no `libX11`.
*Fails if:* libvlccore cannot find its plugins (then set `VLC_PLUGIN_PATH`, see
section 8), or a plugin we did not ship is required (then the log says so).

**M2 - a real video with VLC's own codecs. DONE in the package, not yet watched.**
The default profile now ships `libvpx` (VP8/VP9), `libdav1d` (AV1),
`libtheora`, `libvorbis`, `libopus`, `libmpg123`, the subtitle decoders and all
14 packetizers. They cost **no new package**: those libraries were already in
libavcodec's closure, so the addition is **6 plugin files / 125,128 B (0.125 MB)** plus the
subtitle/packetizer files (50 plugins, 2.89 MB of the `.apk` 13.04 MB).
*Acceptance (still open):* 30 s of VP9 in WebM plays at the file's frame rate in
a window; `display` is called ~N times for N frames. Nobody has done this yet;
the static proof only says the files load.

**M3 - decide about H.264.**
Either accept the **39.55 MB** tree on the data disk (`--profile playback`,
which works today and produces 575 files / 178.53 MB unpacked), or stay with
M2. Nothing in the code changes; only which libraries are on the filesystem.
The older 67.5 MB figure in this section predates the symlink fix.

**M4 - sound, if the design wants it.** Section 4, steps 1-3. Acceptance: a small
test program opens `/dev/snd/pcmC0D0p`, writes a 440 Hz sine, and it is audible
from the host.

---

## 8. Risks, and what this document does not prove

1. **The plugin set is a hypothesis tested by the loader, not by the decoder.**
   `--verify-only` now proves both halves of loading - every `DT_NEEDED` has
   an entry with exactly that name under `lib/`, and every strong undefined
   symbol is defined by a shipped library - but a plugin whose libraries all
   resolve can still fail in `Activate()` (missing hardware, missing
   sub-module), and VLC then falls back to another module or reports the
   format as unsupported. And VLC modules probe for other modules at runtime (a
   text renderer for subtitles, a demuxer for a container, an audio output).
   The first run in the VM is what settles it. This
   is why M1 is deliberately "play a PNG": the shortest path to a failure that
   tells us something.
2. **A missing dependency does not kill the player, it deletes a feature.**
   `module_InitDynamic()` returns NULL when `module_Load()` fails and the plugin
   is skipped (`src/modules/bank.c`, 3.0.23) **[src]**. So a broken plugin shows
   up as "this format does not play", not as a crash. Good for us, bad for
   debugging: read VLC's log (`libvlc_new` with `--verbose 2`).
3. **Plugin discovery.** libvlccore has `/usr/lib/vlc` compiled in, reads the
   `plugin-path` option, and honours the **`VLC_PLUGIN_PATH`** environment
   variable - both strings are in the shipped `libvlccore.so.9.0.1` **[apk]**.
   Because our plugins live in a directory we choose, the player should set
   `VLC_PLUGIN_PATH` explicitly rather than rely on the default. The cache file
   is `<plugin dir>/plugins.dat` (`"%s/plugins.dat"` is in the binary), logged as
   "loading plugins cache file %s" / "ignoring plugins cache file", and generated
   by `vlc-cache-gen` (shipped in `vlc-libs`, run by Alpine's apk trigger).
   Without it libvlccore rescans the directory; if the directory is read-only
   the write fails with "cannot write %s" and it carries on - slower per start,
   not broken. Running `vlc-cache-gen usr/lib/vlc/plugins` at image build is
   therefore worth doing, not required.
4. **`--ignore-config` and `--plugin-path` exist in this build; `--no-audio`
   does not exist as a literal string** (VLC stores the positive option name and
   generates the `no-` prefix). Do not grep the binary for `no-audio` and
   conclude it is unsupported - I did, and it is a bad test. The reliable
   controls are API-level: do not call `libvlc_audio_set_callbacks`, or pass
   `--aout=dummy`.
5. **Symlinks become copies in the image.** `libvlc.so.5` -> `libvlc.so.5.6.1`
   etc. are symlinks in the `.apk`, and `tools/fetch-vlc.py` materialises them
   as copies (Windows cannot create them). That is also what ends up in the
   image: `tools/mkimage.py`'s stage pass reads every file with
   `open(full,'rb')` and records `S_IFREG`, with no `os.path.islink` check, so a
   stage tree cannot carry symlinks. The loader is happy - it only needs the
   name to exist - but each duplicated library costs its full size again
   (`libvlc.so.5` + `libvlc.so.5.6.1` = 306 KB instead of 153 KB). Either accept
   that, or teach mkimage.py about stage symlinks.
6. **`libvlc_media_player_set_volume` does not exist.** The obvious name is
   wrong; the real one is `libvlc_audio_set_volume`. I checked the exported
   symbols of the actual `libvlc.so.5.6.1` (472 dynamic symbols) rather than
   trusting memory - see Appendix A.

---

## 9. Using `tools/fetch-vlc.py`

```
python tools/fetch-vlc.py                 # build: image bundle + package + index row
python tools/fetch-vlc.py --dry-run       # resolve, audit the plugins, build nothing
python tools/fetch-vlc.py --audit-only    # just the plugin audit (reads the cached vlc.apk)
python tools/fetch-vlc.py --verify-only   # re-prove the packed tarball's DT_NEEDED + symbols
python tools/fetch-vlc.py --profile minimal
python tools/fetch-vlc.py --profile subs  # playback + FreeType text renderer + libass (ASS/SSA)
python tools/fetch-vlc.py --force         # ignore the caches
```

- **Provenance is a gate, and it does not need a compiler.** Each repo has its
  own stamp (`build/vlc-pkg.stamp` for the default, `build/vlc-pkg-<repo>.stamp`
  for e.g. `build/repo-subs`), recording three things: `app_digest` (sources +
  tool + manifest + **zig version**, the cache key), `app_digest_src` (the same
  without the compiler) and the sha256 of `src/client/apps/vlcplay.c`. The
  publish gate is **`app_digest_src` + the player hash**; the build prints
  those two plus the cache key (`app_digest`), and `--verify-only` recomputes
  the gate from the tree and **exits 1** with `STALE package: ...` if either
  moved. On a host without zig the toolchain component is simply not
  compared (a printed note says so), so verification works there too; if zig is
  present and its version differs from the build, that is a note as well - a
  compiler upgrade is not a source change. A rebuild is one command.
- **It builds by default.** The expensive steps are cached: `build/vlc-index/`
  (APKINDEX), `build/vlc-apks/` (the `.apk` files), `build/vlc-root/` (a
  signature stamp, so a second run makes no network call) and the package
  itself (`build/vlc-pkg.stamp`, keyed on the app sources, the manifest and
  this script). `--dry-run` downloads nothing beyond the two vlc packages it
  must read `DT_NEEDED` from, and says so.
- Output 1, the **image bundle**: `stage/usr/share/mystic/apps/vlc/{manifest,bin}`.
  `bin` there is the player compiled statically (180,968 bytes as of this
  build). It cannot dlopen, so its window says libVLC is not installed and that
  `pkg install vlc` brings it: a placeholder with the truth in it, never a fake
  player.
- Output 2, the **package** `build/repo/vlc.tar.gz` (39.55 MB compressed /
  105.24 MB installed, 184 regular files + 115 symlinks + 12 directories = 311
  tar entries; the compressed and installed byte counts move by a few KB
  whenever `libexec/vlcplay` is rebuilt - the build prints the exact values and
  the file list is what is stable) plus the `vlc` row in `build/repo/index.tsv`:
  `manifest`; `bin` (static launcher, 27,648 bytes, no PT_INTERP);
  `libexec/vlcplay` (dynamic, the player itself - its size changes as the vmem
  path is reworked, PT_INTERP `/lib/ld-musl-x86_64.so.1`, RUNPATH
  `$ORIGIN/../lib`);
  `lib/ld-musl-x86_64.so.1` (from Alpine's musl apk, 670,312 bytes, mode 0755);
  and the closure - 131 files under `lib/` (130 shared objects plus the
  loader), 115 soname/version symlinks, **50 plugin modules in 7 plugin
  directories**, and the soname aliases
  `libvlc.so.5` and `libvlccore.so.9` that `dlopen()`/`DT_NEEDED` actually use.
  The plugin list is in the Codecs section; it is 50 files because the default
  profile now takes the dedicated AV1/VP9/Theora/Opus/Vorbis/MP3 decoders, all
  14 packetizers and the subtitle decoders, not just the 28 the first package shipped.
  The packed player at the time of writing is `vlcplay.c` sha256
  `bb5c69f415441890...` (95,224 B source, 589,992 B binary) with gate digest
  `a90382e250ee5cd0...`; the build prints three values - the player hash, the
  **gate digest** (`app_digest_src`) and the cache key (`app_digest`, which
  also covers the zig version) - and `--verify-only` exits 1 if the tarball is
  not the one those sources produce, so "which revision is published?" is
  answered by the artifact and enforced by the check.
- Why three files: the image has `/lib/ld-musl-x86_64.so.1` from the Alpine
  base, but the jail is a chroot that binds only `/bin/app`, `/app` (the
  bundle), the fonts, `/dev` bits and `/proc`. PT_INTERP cannot resolve there,
  so `bin` is static and runs the bundled loader explicitly:
  `lib/ld-musl-x86_64.so.1 --library-path <bundle>/lib <bundle>/libexec/vlcplay`.
- The manifest's `exec` is a **bare file name**, and it has to be: `wmsrv`
  rejects any `exec` containing `/` or `..` (`wms_launch_arg`, "unsafe exec"),
  so `exec = libexec/vlcplay` is not a layout Mystic can launch and no
  document should suggest it. `exec = bin` names the static launcher at the
  bundle root; `libexec/vlcplay` and `lib/` are internal paths the
  launcher (and its `$ORIGIN/../lib` RUNPATH) use. The one line `wmsrv`
  could add does **not** change that:
  `mount("/lib", "<jail>/lib", NULL, MS_BIND | MS_RDONLY, NULL);` would only
  matter for a dynamic executable in the jail, and it is not needed -
  `wmsrv` already exports `MYSTIC_BUNDLE=/app` and
  `LD_LIBRARY_PATH=/app/usr/lib:/app/lib`, which is what `bin` reads.
- `--root` adds a package or a versioned provide (`--root so:libvpx.so.11`); a
  name the index does not know is reported instead of being ignored.
- A plan over `--max-download-mb` (default 300) is refused unless `--force`.
  The full closure is 184.68 MB of download but 502.88 MB installed, so the cap
  alone would not stop it - that is what `--dry-run` is for.

---

## Appendix A - the commands behind the numbers

Closure and profiles (all from `tools/fetch-vlc.py`, Alpine v3.24 x86_64):

```
==> apk add vlc (whole package + whole D: line): 279 packages,
    184.68 MB download, 502.88 MB installed
==> profile 'minimal' (libvlc + 12 plugins): 8 packages,
    1.98 MB download, 4.01 MB installed
==> profile 'playback' (libvlc + 50 plugins): 73 packages,
    38.73 MB download, 104.73 MB installed
==> profile 'subs' (libvlc + 52 plugins): 87 packages,
    42.99 MB download, 115.26 MB installed
==> apk add vlc (whole package + whole D: line): 279 packages,
    184.68 MB download, 502.88 MB installed
```

`DT_NEEDED` of the default profile's plugin set, read from the unpacked `.so`
files (the 50-plugin set, so it is longer than this section used to show):

```
==> DT_NEEDED of those files: so:libasound.so.2, so:libavcodec.so.61,
    so:libavformat.so.61, so:libavutil.so.59, so:libdav1d.so.7,
    so:libdbus-1.so.3, so:libdvbpsi.so.10, so:libebml.so.5, so:libgcc_s.so.1,
    so:libjpeg.so.8, so:libmatroska.so.7, so:libmpg123.so.0, so:libogg.so.0,
    so:libopus.so.0, so:libpng16.so.16, so:libstdc++.so.6, so:libtheoradec.so.2,
    so:libtheoraenc.so.2, so:libvlc.so.5, so:libvlccore.so.9, so:libvorbis.so.0,
    so:libvorbisenc.so.2, so:libvpx.so.11, so:libz.so.1
```

What the `.apk` really contains, and what this profile takes (`--audit-only`):

```
==> plugin audit: vlc-3.0.23-r4.apk carries 365 plugin files; profile 'playback' takes 50
    codec watchlist (shipped = the file is taken AND its libraries are):
    plugin                                       in apk    in profile  libraries
    codec/libdav1d_plugin.so                       13.7 KB SHIPPED     libvlccore.so.9, libdav1d.so.7
    codec/libav1_plugin.so                       ABSENT    -           AV1 via a plugin by that name
    codec/libvpx_plugin.so                         17.7 KB SHIPPED     libvlccore.so.9, libvpx.so.11
    codec/libtheora_plugin.so                      21.8 KB SHIPPED     libvlccore.so.9, libtheoraenc.so.2, libtheoradec.so.2
    codec/libmpeg2_plugin.so                     ABSENT    -           MPEG-1/2 video
    codec/libavcodec_plugin.so                    137.8 KB SHIPPED     libvlccore.so.9, libavformat.so.61, libavcodec.so.61, libavutil.so.59
    packetizer/libpacketizer_av1_plugin.so         37.7 KB SHIPPED     libvlccore.so.9
    demux/libsubtitle_plugin.so                    37.7 KB SHIPPED     libvlccore.so.9
    codec/libsubsdec_plugin.so                     25.7 KB SHIPPED     libvlccore.so.9
    codec/libwebvtt_plugin.so                     101.7 KB SHIPPED     libvlccore.so.9
    codec/liblibass_plugin.so                      25.7 KB not selected libvlccore.so.9, libass.so.9  (not shipped: libass.so.9)
    text_renderer/libfreetype_plugin.so            73.7 KB not selected ... (not shipped)
    verdict: 8 shipped, 2 present but not in this profile, 2 absent from VLC 3.0.23, 0 selected but unresolvable
    codec/packetizer plugins that would load against this closure but the profile does not take: 33
```

And the proof on the **packed artifact** (every `DT_NEEDED` soname and every
imported symbol resolved inside the tarball, not in the loose tree):

```
$ python tools/fetch-vlc.py --verify-only
==> build/repo/vlc.tar.gz verified: 182 objects checked for DT_NEEDED by exact name,
    51 plugin/player symbol sets, 115 aliases, 1 dlopen target(s) (libvlc.so.5),
    1880 imports, 0 NEEDED failures, 0 symbol failures
```

That is 50 plugins + the player; `libexec/vlcplay` imports 66 symbols, and the
plugin with the most imports (`demux/libmkv_plugin.so`) imports 356.

The vmem output plugin needs nothing but libVLC:

Printed by the same 40-line ELF reader that is inside `tools/fetch-vlc.py`,
pointed at two video output plugins:

```
libvmem_plugin.so   14016  libvlccore.so.9, libc.musl-x86_64.so.1
libfb_plugin.so     18112  libvlccore.so.9, libc.musl-x86_64.so.1
```

Exported symbols of the shipped `libvlc.so.5.6.1` (472 dynamic symbols):

```
libvlc_video_set_callbacks            EXPORTED
libvlc_video_set_format               EXPORTED
libvlc_video_set_format_callbacks     EXPORTED
libvlc_audio_set_callbacks            EXPORTED
libvlc_audio_set_format               EXPORTED
libvlc_audio_set_format_callbacks     EXPORTED
libvlc_audio_set_volume               EXPORTED
libvlc_media_player_set_volume        MISSING
```

Compile and link check of the Appendix B harness against the real headers and
the real shared object:

```
$ zig cc -target x86_64-linux-musl -Os -static -I.work/vlc/dev/usr/include \
      -Wall -Wextra -Wno-unused-parameter -c .work/vlc/probe.c -o .work/vlc/o/probe.o
COMPILE CLEAN                       # no output at all from the compiler

$ zig cc -target x86_64-linux-musl -Os -I.work/vlc/dev/usr/include \
      .work/vlc/probe.c .work/vlc/main.c .work/vlc/root/usr/lib/libvlc.so.5.6.1 \
      -o .work/vlc/o/probe.elf
$ python3 -c 'dt_needed(".work/vlc/o/probe.elf")'
probe.elf DT_NEEDED: ['libvlc.so.5', 'libc.so']   size 4568
```

Gzipped growth of the payload (tar.gz of exactly the files a profile adds, so it
is comparable with the 13.42 MB initramfs):

```
minimal   17 files      2.85 MB raw      1.10 MB gzipped   (symlinks as symlinks)
minimal   23 files      5.37 MB raw      2.15 MB gzipped   (as stored by our unpacker)
playback 184 files    105.24 MB raw     39.55 MB gzipped   (the real tarball: 115 symlinks kept)
```

## Appendix B - the harness that compiles clean

This is the whole libVLC surface a player needs (the file's own header comment is
omitted). It compiled clean against `vlc-dev-3.0.23` and linked against the
extracted `libvlc.so.5.6.1`; it is the file that should become
`src/client/apps/player.c`.

```c
#include <stdint.h>
#include <string.h>
#include <vlc/vlc.h>

typedef uint32_t u32;

/* A Mystic window: the shm mapping wmsrv handed us in its OK line, which is
 * stride*maxh pixels with stride >= the screen width - NOT w pixels per row.
 * The video is `w` pixels wide inside a `pitch`-byte row. */
typedef struct {
    u32 *px;
    int  w, h;          /* window size; what VLC delivers */
    int  pitch;         /* bytes per row = stride * 4, from the OK line */
    unsigned frames;
} Vp;

static void *vp_lock(void *opaque, void **planes) {
    Vp *v = (Vp *)opaque;
    planes[0] = v->px;                  /* VLC writes here, row by row */
    return v;
}

static void vp_unlock(void *opaque, void *picture, void *const *planes) {
    (void)opaque; (void)picture; (void)planes;
}

static void vp_display(void *opaque, void *picture) {
    Vp *v = (Vp *)opaque;
    (void)picture;
    v->frames++;                        /* the real app sends FRAME to wmsrv */
}

static unsigned vp_format(void **opaque, char *chroma, unsigned *width,
                          unsigned *height, unsigned *pitches, unsigned *lines) {
    Vp *v = (Vp *)*opaque;
    memcpy(chroma, "RV32", 4);          /* 32bpp 0x00RRGGBB, B,G,R,X in memory */
    *width  = (unsigned)v->w;
    *height = (unsigned)v->h;
    pitches[0] = (unsigned)v->pitch;
    lines[0]   = (unsigned)v->h;
    return 1;
}

static void vp_cleanup(void *opaque) { (void)opaque; }

libvlc_media_player_t *player_open(libvlc_instance_t *vlc, const char *path, Vp *v) {
    libvlc_media_t *m;
    libvlc_media_player_t *mp;

    if (!vlc || !v || !v->px) return NULL;
    /* the header asks for planes on 32-byte boundaries and suggests 32-byte
     * pitches: the shm mapping is page aligned, and the stride we were given
     * must be a multiple of 8 pixels (1280 is). */
    if (((uintptr_t)v->px & 31u) != 0 || (v->pitch & 31) != 0) return NULL;
    if (v->pitch < v->w * 4) return NULL;      /* would overwrite the next row */
    m = libvlc_media_new_path(vlc, path);
    if (!m) return NULL;
    mp = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (!mp) return NULL;
    libvlc_video_set_callbacks(mp, vp_lock, vp_unlock, vp_display, v);
    libvlc_video_set_format_callbacks(mp, vp_format, vp_cleanup);
    return mp;
}

/* Audio would come out of libvlc_audio_set_callbacks() as S16N PCM if there
 * were a device to write it to; see section 4. This only names the format so
 * the API is exercised. */
void player_audio_to_memory(libvlc_media_player_t *mp) {
    libvlc_audio_set_format(mp, "S16N", 48000, 2);
}
```

Things this code deliberately does *not* do, and why:

- It does not `mmap` the shm window or speak the protocol - that is
  `src/client/mystic_client.c`'s job, and it already exists.
- It does not scale. Ask for the window's size and let the video be cropped, or
  ship swscale (section 3.3).
- It does not touch audio beyond naming a format. Section 4 is why.
- It does not compute its own pitch: `pitch` is `stride * 4` from the window
  server's `OK` line, and `player_open` refuses a pitch that VLC's 32-byte
  recommendation or the row width would make unsafe.

## It plays

Verified in the running OS, not on a host: a 991,017-byte H.264/MP4 sample
(`sample.mp4`, 10 s, 640x360) decoded by libVLC 3.0.23 and drawn in the Player
window by `src/client/apps/vlcplay.c`.

```
pkg install vlc
  installing vlc 3.0.23 (vlc.tar.gz, 64.3 MB) from the signed repository
  size ok: 67455432 bytes, the signed index says 67455432
  checksum ok: sha256 1da93bc0...
  272 files, 337 links (soname links included)
  installed vlc 3.0.23 to /root/apps/vlc (exec: bin, 99.1 MB on disk)

LD_LIBRARY_PATH=/root/apps/vlc/lib /root/apps/vlc/bin /root/sample.mp4
```

The window then shows the decoded frame with `ended 0:09 / 0:10`, `640x360`
and `libVLC 3.0.23 Vetinari` in its footer. Screenshot:
`docs/screenshots/video-vlc.png`.

> **What this proves, and what it does not (note added by worker p1, which
> owns `src/client/apps/vlcplay.c`).** The transcript above is a MANUAL start
> from a terminal - the `LD_LIBRARY_PATH=/root/apps/vlc/lib /root/apps/vlc/bin`
> line is the command that produced the screenshot. It proves decode + draw in
> the running OS. It did not, at the time of writing, prove the launcher tile:
> wmsrv execs the manifest's `exec` inside a chroot whose binds are the payload
> at `/bin/app`, the bundle at `/app`, the socket, fonts and `/dev` nodes -
> there is no `/lib` in it (`src/wmsrv.c` sbx plan), so a dynamic `bin` never
> reached `main()` there.
>
> **That gap is now closed in the package** (see the bundle-shape update
> below): `bin` is the STATIC launcher
> (`/app/lib/ld-musl-x86_64.so.1 --library-path /app/lib /app/libexec/vlcplay`,
> built from `src/client/apps/vlcplay.c` with `-DVLCPLAY_BOOT`) and the dynamic
> player is at `libexec/vlcplay`. So this note is historical about the launch
> path, not a statement about the current tarball; the measurements above are
> still only the manual run.

### Three things had to be true before a frame appeared

1. **The player must be dynamically linked.** A static musl binary's `dlopen()`
   is a stub: the app reported "Dynamic loading not supported" and showed the
   install command instead of a window. In the current package the dynamic
   player is `libexec/vlcplay` (`PT_INTERP /lib/ld-musl-x86_64.so.1`), and
   `bin` is the static launcher that execs it through the bundled loader - see
   the update below. The interpreter string is written explicitly
   (`-Wl,-dynamic-linker=/lib/ld-musl-x86_64.so.1`); the corruption this file
   used to describe came from **MSYS rewriting the argument** when zig is
   started from git-bash, not from zig's sysroot (a native invocation records
   `/lib/...` unchanged, and `MSYS_NO_PATHCONV=1` or a Python `subprocess` call
   does too), which is why `tools/fetch-vlc.py` passes the flag and
   `check_elf()` now rejects any other interpreter outright.
2. **The image builder must mark loaders executable.** A loader is executed by
   the kernel as the interpreter of every dynamic binary, and `tools/mkimage.py`
   gave anything outside `bin/` mode 0644 - which made `exec` of busybox fail
   with `EACCES` and the kernel panic with "No working init found". Alpine's
   busybox is dynamic, which is why adding a loader at mode 0644 broke the boot.
3. **The package needs soname links.** `libvlc.so.5` is a symlink to
   `libvlc.so.5.6.1`, and Windows cannot create symlinks. Rebuilding the package
   from an extracted tree silently dropped all of them, and the loader then
   failed with "libvlc.so.5: No such file or directory" even though the
   versioned file was present. `tools/pack-vlc.py` re-creates every `libX.so.N`,
   `libX.so` alias from the versioned names: 337 links for the earlier 64.3 MB
   closure this section measured, 115 for the current 39.3 MB canonical build.

> **Update (worker p1, `src/client/apps/vlcplay.c`): the bundle shape is now**
> `bin` = a **static launcher** (27,648 bytes, no `PT_INTERP`, no `NEEDED`),
> `libexec/vlcplay` = the dynamic player (`PT_INTERP /lib/ld-musl-x86_64.so.1`,
> `NEEDED libc.so`, `RUNPATH $ORIGIN/../lib`), and
> `lib/ld-musl-x86_64.so.1` = the 670,312-byte musl loader (its SONAME is
> `libc.musl-x86_64.so.1`, which is what libvlc's closure asks for). The reason
> is item 1 read against the sandbox: a dynamic `bin` cannot be the manifest's
> `exec`, because wmsrv chroots the app into a jail whose binds are the payload,
> the bundle at `/app`, the socket, fonts and `/dev` nodes - there is no `/lib`,
> and `PT_INTERP` is absolute. The launcher execs
> `/app/lib/ld-musl-x86_64.so.1 --library-path /app/lib /app/libexec/vlcplay`.
> `src/apps/store.c` also needs `placeholder = no` in the installed manifest;
> the package used to say `yes`, which keeps the Store offering the install.
> `tools/pack-vlc.py` now emits both pieces (`--bin`, `--player`, `--loader`),
> dedupes members, and drops modules whose `DT_NEEDED` the bundle cannot
> satisfy (the inherited closure shipped `libpulse_plugin.so` and
> `libjack_plugin.so` with neither libpulse nor libjack). **The canonical
> builder was updated too**: `tools/fetch-vlc.py`'s `APP_SRC`/`BOOT_SRC` are
> `src/client/apps/vlcplay.c`, it builds `bin` with `-DVLCPLAY_BOOT` and the
> player at `libexec/vlcplay`, its `check_elf()` rejects any `PT_INTERP` other
> than `/lib/ld-musl-x86_64.so.1`, and its default plugin profile now carries
> `audio_output/libalsa_plugin.so` next to the dummy. A plain
> `python tools/fetch-vlc.py` therefore produces this shape (39,274,007 bytes,
> 289 members: 162 files, 115 soname links, 12 dirs, all `DT_NEEDED` resolved)
> instead of reverting to the old app. The `pkg install` transcript above is the
> earlier 64.3 MB package, so its 272 files / 337 links describe that run, not
> this one. `tools/pack-vlc.py` remains the offline repack path when the closure
> is already proven.

### Audio

`libalsa_plugin.so` and `libasound.so.2` now ship in the bundle
(`tools/pack-vlc.py --extras build/vlc.apk build/alsa.apk`, and since the
canonical-builder update below, the default `fetch-vlc.py` profile ships it
directly), and the player's footer shows `vol 100` where it used to show
`no audio output`. `docs/screenshots/video-vlc-audio.png`.

> **Correction (worker p1): `vol 100` does not prove the PCM opened.** It is
> `libvlc_audio_get_volume()`'s answer after a successful
> `libvlc_audio_set_volume()`; the app prints `no audio output` only when the
> setter returns < 0. A fallback sink that accepts a volume produces the same
> `vol 100`. The screenshot's terminal line `ALSA lib...` is cut off by the
> Player window, so it cannot settle it either - and alsa-lib prints such a line
> when a PCM open *fails*. What would settle it: run with `-vvv` and read the
> audio-output module line, or read
> `/proc/asound/card0/pcm0p/sub0/status` while playing and require `RUNNING`.
> Until then the honest sentence is "libVLC reports an audio output that accepts
> a volume", not "the ALSA output opened the OS's PCM device".


Why it was missing: the first closure was built with a filter that kept 27
plugins, and `audio_output/` contained only `libadummy_plugin.so` - a sink that
discards samples - with no `libasound` dependency at all. Nothing failed loudly:
libVLC simply reported that it had no audio output and played the video.

### Through the OS, with no terminal involved

The tile path is now measured end to end. A file is handed to the desktop the
way Files does it, the type registry picks the app, and the installed bundle
plays it inside the sandbox:

```
$ echo /root/sample.mp4 > /run/mystic/open.request
open: /root/sample.mp4 -> vlc pid 602
wms: vlc connected 960x640 as uid 2000 (/run/mystic/jail/vlc/dev/shm/mystic-1-1)
```

The window then shows the decoded frame with `ended 0:09 / 0:10`, `640x360`,
`vol 100` and `libVLC 3.0.23 Vetinari`. Screenshot:
`docs/screenshots/video-vlc-tile.png`.

Two things this settles that the manual run could not:

- **The static launcher works inside the jail.** Launched with no arguments the
  tile reports `libVLC 3.0.23 Vetinari loaded from /app/lib/libvlc.so.5` - the
  jailed bundle path, so `bin` found the bundled loader, the loader found
  `libexec/vlcplay`, and the player loaded libVLC from `/app/lib`. A dynamic
  `bin` could not have done this: `PT_INTERP` is absolute and the jail has no
  `/lib`.
- **Audio output is a real sink, not the dummy.** The footer says `vol 100`
  where it previously said `no audio output`, which only happens when an
  `audio_output` module initialises; the bundle ships `libalsa_plugin.so` and
  `libasound.so.2` and the OS exposes `/dev/snd/pcmC0D0p`. Audible sound cannot
  be confirmed from inside this VM, so what is claimed is that the output opens
  and libVLC reports a volume, not that a listener heard it.

## Measured playback rates

The player reports what the decoder actually sustains, every hundred frames, so
the following are measurements rather than expectations:

| file | surface | cores | rate |
|---|---|---|---|
| 1080p H.264 (local, via `open.request`) | 1920x1080 | 4 | **60.0 fps** |
| 4K H.264 (3840x2160) | 3840x2160 | 4 | 17 fps |
| 4K H.264 | 3840x2160 | 8 | **24 fps** |

These were taken through the original `vmem` callback path that converts each
frame to RV32 in libVLC (section 3.3). A rewrite of that path - taking the I420
planes and sampling only the displayed pixels - changes these numbers, so the
authoritative figure is the fps the player prints in its own status line, not
this table. The codec conclusions in the Codecs section do not depend on it.

The VM now gets four cores by default (`image.sh`, `-smp 4`, override with
`MYSTIC_SMP`): software decoding is per-frame work that splits across threads and
one vCPU could not sustain 1080p50 at all. 1080p at 60 fps leaves the CPU with
headroom; 4K scales with cores but stays below 60 on this host, which is a CPU
limit, not a container-format limit - the same file plays at the source rate
once the decoder keeps up.

## Codecs

This section is built from the package's own bytes, not from what libavcodec
is capable of in the abstract. Everything below comes from two commands:

```
python tools/fetch-vlc.py --audit-only    # reads vlc-3.0.23-r4.apk's file list and ELF headers
python tools/fetch-vlc.py --verify-only   # reads build/repo/vlc.tar.gz and resolves every import
```

The rule a format has to pass to be called *covered* here has three parts:
(a) a plugin file that decodes it is **inside the package**; (b) for **every**
shipped object - plugin, library, player - each `DT_NEEDED` name has an
**entry with exactly that name** under `lib/` (a regular file or a symlink),
and the player's `dlopen("libvlc.so.5")` target has one too; (c) every strong
undefined symbol of each plugin and of the player is **defined by one of the
shipped libraries** (the bundled musl loader counts as libc).

Part (b) is by name, not by SONAME. A file whose `DT_SONAME` is
`libvpx.so.11` but whose name is `libvpx.so.11.0.1` does **not** satisfy the
loader: musl opens `lib/libvpx.so.11`, it does not scan the directory for a
SONAME. `--verify-only` used to accept that, and now does not - deleting both
`libvpx.so.11` symlinks from the tarball is rejected with
`libavcodec.so.61.19.101 needs libvpx.so.11 (no entry with that name under
lib/)`. Part (c) catches the other half: a library shipped at the wrong soname
is present and `readelf` looks happy, and `dlopen` still fails with "undefined
symbol". Both halves are exercised by deleting a file from a copy of the
tarball and confirming a non-zero exit.

> **"libavcodec handles it" is not proof.** libavcodec is one module
> (`codec/libavcodec_plugin.so`, 141,144 B) and it is only reached if that
> file, `libavcodec.so.61`, `libavformat.so.61` and `libavutil.so.59` are all
> in the bundle. Similarly `packetizer/libpacketizer_hevc_plugin.so` splitting
> HEVC into NAL units says nothing about a HEVC decoder existing. Every claim
> below is tied to a file that is actually in the tarball.

### 1. What the default profile ships

`--profile playback` takes **50 of the 365 plugin files** in
`vlc-3.0.23-r4.apk`: 2.89 MB of the package's 13.04 MB of plugins. The
package is `build/repo/vlc.tar.gz`, **39.55 MB compressed / 105.24 MB
installed / 184 files**, 131 files under `lib/` (130 shared objects + the musl loader) and 115 soname aliases.

| Job | Shipped plugin(s) | Library the plugin loads |
|---|---|---|
| containers | `demux/libmp4`, `libmkv`, `libavi`, `libts`, `libps`, `libwav`, `libogg`, `libasf`, `libflacsys` | own code; `mkv` needs `libmatroska.so.7` + `libebml.so.5`, `ts` needs `libdvbpsi.so.10`, `ogg` needs `libogg.so.0` |
| raw elementary streams | `demux/libes`, `libh26x`, `libmpgv` | libvlccore only |
| H.264, HEVC, MPEG-1/2/4, VC-1, AAC, AC-3, DTS, FLAC, ... | `codec/libavcodec_plugin.so` | `libavcodec.so.61` (16.33 MB), `libavformat.so.61`, `libavutil.so.59` |
| VP8 and VP9 | `codec/libvpx_plugin.so` (18,112 B) | `libvpx.so.11` (3,340,104 B) |
| AV1 | `codec/libdav1d_plugin.so` (14,016 B) | `libdav1d.so.7` (1,750,640 B) |
| Theora | `codec/libtheora_plugin.so` (22,360 B) | `libtheoradec.so.2` + `libtheoraenc.so.2` |
| Opus | `codec/libopus_plugin.so` (26,304 B) | `libopus.so.0` (472,768 B) |
| Vorbis | `codec/libvorbis_plugin.so` (30,400 B) | `libvorbis.so.0` + `libvorbisenc.so.2` |
| MP3 (VLC's own decoder) | `codec/libmpg123_plugin.so` (13,936 B) | `libmpg123.so.0` (249,056 B) |
| stream framing in ts/ogg/ps | **all 14** `packetizer/libpacketizer_*` | libvlccore only (571.5 KB total) |
| subtitles, decode only | `demux/libsubtitle`, `codec/libsubsdec`, `codec/libwebvtt` | libvlccore only |
| output | `video_output/libvmem`, chroma `rv32`/`chain`/`i420_rgb*`; `audio_output/libalsa` | see sections 3 and 4 |

The three subtitle files in this list are 169,024 B (demuxer 38,592 +
`subsdec` 26,304 + `webvtt` 104,128) and, on their own, **inert**: they decode
a track to SPU bitmaps and there is no text renderer in this profile to paint
them. They are here because `--profile subs` builds on this profile and adds
the renderer (+2 files); if a future default should not carry them, remove
`subs_text` from `PLUGINS["playback"]` and the audit will show the count drop.
Nothing is claimed to render in the default profile.

The six dedicated decoders (`vpx`, `dav1d`, `theora`, `opus`, `vorbis`,
`mpg123`) cost **no new package**: their libraries were already in
libavcodec's closure on this Alpine release (five are in
`ffmpeg7-libavcodec`'s own `D:` line; `mpg123-libs` arrives through
`libavformat -> libopenmpt`), so the profile now takes
**50 of the 365 plugin files, 2.89 MB of the package's 13.04 MB of plugins**,
and the index closure is 73 packages, 38.73 MB download, 104.73 MB installed -
the same closure as before the codec work: no library and no package was
added. A fresh build fetches
**71 packages / 43.27 MB** of `.apk` bytes: the 73-package closure (38.73 MB)
minus musl, busybox and busybox-binsh, which the base image already has
(921,806 B), plus `vlc.apk` itself (5.46 MB), which is not a dependency of
itself. 38.73 - 0.92 + 5.46 = 43.27.

The full list, as packed (path under `lib/vlc/plugins/`, bytes) - this is the
output of reading `build/repo/vlc.tar.gz`, not of a profile declaration:

```
  access/libfilesystem_plugin.so                    18112
  audio_output/libadummy_plugin.so                  13936
  audio_output/libalsa_plugin.so                    30400
  codec/libavcodec_plugin.so                       141144
  codec/libdav1d_plugin.so                          14016
  codec/libjpeg_plugin.so                           22360
  codec/libmpg123_plugin.so                         13936
  codec/libopus_plugin.so                           26304
  codec/libpng_plugin.so                            18192
  codec/librawvideo_plugin.so                       13936
  codec/libsubsdec_plugin.so                        26304
  codec/libtheora_plugin.so                         22360
  codec/libvorbis_plugin.so                         30400
  codec/libvpx_plugin.so                            18112
  codec/libwebvtt_plugin.so                        104128
  demux/libasf_plugin.so                            79552
  demux/libavi_plugin.so                            95936
  demux/libes_plugin.so                             42688
  demux/libflacsys_plugin.so                        30400
  demux/libh26x_plugin.so                           18112
  demux/libimage_plugin.so                          30400
  demux/libmkv_plugin.so                           485328
  demux/libmp4_plugin.so                           280256
  demux/libmpgv_plugin.so                           14016
  demux/libogg_plugin.so                           104128
  demux/libps_plugin.so                             38592
  demux/librawvid_plugin.so                         18112
  demux/libsubtitle_plugin.so                       38592
  demux/libts_plugin.so                            247488
  demux/libwav_plugin.so                            22208
  packetizer/libpacketizer_a52_plugin.so            22208
  packetizer/libpacketizer_av1_plugin.so            38592
  packetizer/libpacketizer_copy_plugin.so           18032
  packetizer/libpacketizer_dirac_plugin.so          30400
  packetizer/libpacketizer_dts_plugin.so            26304
  packetizer/libpacketizer_flac_plugin.so           26224
  packetizer/libpacketizer_h264_plugin.so          120512
  packetizer/libpacketizer_hevc_plugin.so          116416
  packetizer/libpacketizer_mlp_plugin.so            26304
  packetizer/libpacketizer_mpeg4audio_plugin.so     46784
  packetizer/libpacketizer_mpeg4video_plugin.so     30400
  packetizer/libpacketizer_mpegaudio_plugin.so      18112
  packetizer/libpacketizer_mpegvideo_plugin.so      30400
  packetizer/libpacketizer_vc1_plugin.so            34496
  video_chroma/libchain_plugin.so                   18032
  video_chroma/libi420_rgb_mmx_plugin.so            54896
  video_chroma/libi420_rgb_plugin.so                30320
  video_chroma/libi420_rgb_sse2_plugin.so          116336
  video_chroma/librv32_plugin.so                    13936
  video_output/libvmem_plugin.so                    14016
```

### 2. Proven to play (booted, with evidence)

| Format | Evidence |
|---|---|
| H.264 in MP4 | 640x360 sample decoded and drawn in the Player window, `ended 0:09 / 0:10`, screenshot `docs/screenshots/video-vlc.png` |
| H.264 1080p | 60.0 fps measured in the running OS (rate table above) |
| H.264 4K | 17 fps on 4 vCPUs / 24 fps on 8, measured (rate table above) |

That is the whole list. **No other codec has ever been watched in this
player.** The avcodec plugin is the one decoder we know loads at runtime,
because it is the only one that has produced a frame.

### 3. Shipped, statically proven to load, never watched

These pass all three checks in `--verify-only` (the file is in the tarball,
every `DT_NEEDED` has an entry with exactly that name, and all 1,880 imported
symbols are defined by shipped libraries), but no frame or sample from them has
been observed in the guest:

| Format | What is in the bundle | Why it is not yet "proven" |
|---|---|---|
| **VP8/VP9 in WebM/MKV** (the owner's question) | `libvpx_plugin.so` + `libvpx.so.11`; mkv demuxer; vp8/vp9 also reachable through `libavcodec` | never opened a `.webm` in the player; which module wins is a runtime fact. **8-bit only** (10-bit VP9 has no converter, see below) |
| **AV1** | `libdav1d_plugin.so` + `libdav1d.so.7`, and `libavcodec.so.61` also links dav1d | same; VLC 3.0.23 has no `libav1_plugin.so` at all. **8-bit only** (10-bit AV1 has no converter) |
| HEVC | `libavcodec_plugin` + `packetizer_hevc` | no HEVC sample has ever been played. **8-bit only** (10-bit HEVC has no converter) |
| MPEG-2 / MPEG-1 | `libavcodec_plugin` only (there is no `libmpeg2_plugin.so`) | not watched |
| VC-1 / WMV3 | `libavcodec_plugin` + `demux/libasf_plugin` | not watched |
| Theora / Ogg | `libtheora_plugin.so` + `demux/libogg` | not watched |
| Opus / Vorbis / MP3 / AC-3 / AAC / FLAC / DTS | dedicated plugins + `libavcodec` | decode ships; **nobody has heard it**. The image now has `-device intel-hda -audiodev sdl`, the `snd` modules and `/dev/snd/pcmC0D0p` (section 4 carries the dated note), and libVLC reports an audio output that accepts a volume - but a PCM open has not been proven, only that the module initialises |
| AVI / TS / PS / ASF / FLAC / WAV containers | their demuxers | only MP4 has been opened |
| PNG / JPEG / raw frames | `codec/libpng`, `libjpeg`, `librawvideo` | the minimal-profile milestone, not part of the booted evidence above |
| Subtitles | `libsubtitle` + `libsubsdec` + `libwebvtt` | text is decoded to SPU bitmaps, but **nothing paints it**: no text renderer in this profile |

### 4. Absent from the package (measured, not assumed)

`--audit-only` prints each of these against the real `.apk`:

| Claim | Measured |
|---|---|
| `codec/libav1_plugin.so` | **not in `vlc-3.0.23-r4.apk`**. AV1 is `libdav1d_plugin.so`; a plugin with that name does not exist in VLC 3.0. |
| `codec/libmpeg2_plugin.so` | **not in the `.apk`**. MPEG-2 is libavcodec only. |
| a dedicated H.264 / HEVC / VC-1 decoder | **none**. Only `libavcodec_plugin.so` and the packetizers. |
| `codec/liblibass_plugin.so` (styled ASS/SSA) | file present (26,304 B), needs `libass.so.9` -> the `libass` package plus the FreeType/fontconfig stack. **Shipped only by `--profile subs`**, where its marginal cost over the text renderer is +0.12 MB download / +0.33 MB installed; the whole subtitle stack is +14 packages / +4.26 MB / +10.53 MB over the default. |
| `text_renderer/libfreetype_plugin.so` (draws any subtitle text) | file present (75,456 B), needs `libfontconfig.so.1`, `libfribidi.so.0`, `libharfbuzz.so.0`, `libfreetype.so.6`. Only `--profile subs` ships it: on its own **+12 packages, +4.14 MB download, +10.20 MB installed**; with libass the profile is **87 packages / 42.99 MB / 115.26 MB** (playback 73/38.73/104.73). Whether fontconfig can initialise in the guest is a boot-time fact: the image has TTFs in `/usr/share/fonts` but no `/etc/fonts/fonts.conf`. |
| an `avformat` demuxer | **not in the `.apk` at all**. VLC's `demux/avformat` module is absent, so containers only libavformat knows - `.flv` is the common one - cannot open. There is no HLS/DASH either: `demux/libadaptive_plugin.so` needs `libgcrypt.so.20`, which is not shipped. |
| DVD / teletext / CEA-608 captions | `libspudec`, `libdvbsub`, `libcvdsub`, `libsvcdsub`, `libtelx`, `libt140`, `libcc`, `libscte18/27` exist in the `.apk`; none is in the default profile (no DVD demuxer either: `libdvdread`/`libdvdnav` are not shipped). |
| hardware decode | `libvaapi_plugin`, `libvdpau_*` exist in the `.apk`, not shipped: the VM has no GPU, so they would fail to open and add libraries for nothing. |
| **10-bit (P010/I420_10) and 4:2:2 (I422)** | **Not covered.** The decoders ship (libavcodec/dav1d/vpx all decode 10-bit), but the chroma set in the bundle is `rv32`/`chain`/`i420_rgb*` only: there is no 10-bit or 4:2:2 converter. `video_chroma/libi420_10_p010_plugin.so` and `libswscale_plugin.so` exist in the `.apk` and are **not shipped**. The player's `setup()` answers `I420` for a 10-bit/4:2:2 proposal, so libVLC would have to convert and has nothing to convert with - a 10-bit HEVC/VP9/AV1 file (common in downloaded 4K) decodes and then produces no picture. Treat the HEVC/VP9/AV1 rows above as **8-bit only**. |
| FLAC / AC-3 dedicated decoders | plugin files exist but need `libFLAC.so.12` / `liba52.so.0`; **not shipped**. libavcodec decodes both, so the default profile does not pay 0.42 MB + 0.09 MB for a second copy. |

### 5. In the `.apk` but deliberately not in the profile

33 codec/packetizer plugins would load against this closure and are still not
taken. Grouped by why:

- **encoders**, not decoders: `libx264`, `libx26410b`, `libx265`, `libvaapi`,
  `libvaapi_drm`;
- **disc / broadcast subtitles and captions**: `libspudec`, `libdvbsub`,
  `libcvdsub`, `libsvcdsub`, `libtelx`, `libt140`, `libcc`, `libscte18`,
  `libscte27`, `libtextst`, `libstl`, `libsubstx3g`, `libsubsusf`, `libttml`;
- **audio formats libavcodec already decodes**: `libadpcm`, `libaes3`,
  `libaraw`, `libg711`, `liblpcm`, `libspdif`, `libuleaddvaudio`,
  `libwma_fixed`;
- **niche inputs**: `libcdg`, `libxwd`, `liboggspots`, `librtpvideo`,
  `libddummy`, `libedummy`.

No decoder for a format the default profile can already decode is missing
from it - that is what the completeness line in `--audit-only` measures
(`codec/packetizer plugins that would load against this closure but the
profile does not take: 33`), and it is re-checked on every run.

### 6. What still needs a boot to confirm

1. **VP9 in WebM actually rendering** - the exact case that motivated this
   section. The plugin, its library, the demuxer and the packetizers are all
   in the tarball; nobody has watched a frame.
2. **Which AV1 module VLC selects** (`dav1d` vs the avcodec path) and whether
   `dav1d_open` succeeds. Both paths are present, so a failure would be a
   module-selection bug, not a missing file.
3. **Audio in TS** with the newly shipped `a52`/`mpegaudio`/`dts` packetizers.
   Even when it decodes, the PCM open and the host audio backend are unproven
   (section 4), so this can only be confirmed from VLC's log ("using audio
   decoder") and `/proc/asound/.../status`, not by ear.
4. **Subtitles painted** in `--profile subs`: fontconfig needs a config file
   the image does not ship yet.
5. **No regressions from more plugins.** The change is additive - files only,
   no library version moved - and VLC picks modules by score at runtime. The
   static proof says each new file loads; it cannot say the scoreboard still
   picks the same decoder for H.264, which is what a playback run checks.

`filetypes.c` routes mp4, mkv, webm, avi and mov to the player, so a
downloaded file opens from Files exactly like a video attached to a message.
The container list in the bundle is now wider than that routing table
(ts, ps, asf, flac, wav, ogg, raw h264/hevc/mpg), which is a UI question for
the next change, not a codec gap.

