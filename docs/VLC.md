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
   codec library at all - that is the first milestone and it is realistic. A player that decodes **H.264/HEVC** needs
   libavcodec and adds **67.5 MB gzipped** - 5x the whole image. That one is a
   separate disk image or a 9p mount, not part of the 13 MB initramfs.

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
| Sound | **none**: no `-audiodev`, no sound `-device` in the QEMU line | `image.sh` BOOT array **[ours]** |
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

## 3. The display path: `vmem` + `RV32`

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

The callbacks we implement, with the header's own contract:

| Callback | Signature | What we do |
|---|---|---|
| `lock` | `void *(*)(void *opaque, void **planes)` | `planes[0] = window->px; return window;` |
| `unlock` | `void (*)(void *opaque, void *picture, void *const *planes)` | nothing |
| `display` | `void (*)(void *opaque, void *picture)` | send `FRAME` to the window server |
| `setup` | `unsigned (*)(void **opaque, char *chroma, unsigned *w, unsigned *h, unsigned *pitches, unsigned *lines)` | write `"RV32"`, set `w`=window width, `h`=window height, `pitches[0]=stride*4` from the wm's `OK` line, `lines[0]=h`, return 1 |
| `cleanup` | `void (*)(void *opaque)` | nothing |

Header notes we must respect, quoted **[apk]**:

- "one or three pixel planes ... Those planes must be aligned on 32-bytes
  boundaries" - our shm mapping is page-aligned, so this is free.
- "we recommend that pitches and lines be multiple of 32" - the pitch comes
  from the wm (see 3.4); on the 1280x800 screen the stride is 1280 pixels, so
  `pitch = 5120` bytes, a multiple of 32. If the screen width is ever not a
  multiple of 8, round the buffer's stride up, not the window's width.
- "Rendering video into custom memory buffers is considerably less efficient
  than rendering in a custom window as normal" - true in general, irrelevant
  here because we have no window system to render into.

### 3.2 `RV32` is our pixel format, exactly

`RV32` is `VLC_CODEC_RGB32` (`include/vlc_fourcc.h`: "24 bits RGB padded to
32 bits") **[apk]**. Its bit layout is set twice in upstream, identically:

- `modules/video_output/vmem.c`, `Open()`, for `VLC_CODEC_RGB32`:
  `rmask = 0xff0000, gmask = 0x00ff00, bmask = 0x0000ff` **[src]**
- `src/misc/es_format.c`, `video_format_FixRgb()`, same three masks for
  `VLC_CODEC_RGB32` **[src]**

A pixel is therefore `u32 = 0x00RRGGBB`, and on little-endian the bytes in
memory are `B, G, R, 0x00`. Our window buffer is `0xAARRGGBB` with the alpha
byte ignored by the compositor's `memcpy` **[ours]**. **Same u32, same bytes.**

That is the entire "port": there is no conversion code to write, and the only
thing we must not do is treat the top byte as meaningful.

### 3.3 The copy vmem makes - and how to spend it well

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
    picture_CopyPixels(locked, pic);      /* <-- VLC copies into OUR planes */
    picture_Release(locked);
}
```

Two consequences, both important:

1. **vmem is not zero-copy.** VLC decodes into its own pooled picture and then
   copies it into the planes we handed over. The header warns about exactly
   this ("Memory copying is required between LibVLC reference picture buffers
   and application buffers").
2. **So hand it the window itself.** If `planes[0]` *is* the window's shm
   mapping, that internal `picture_CopyPixels` **is** our present: VLC's copy
   becomes the blit, and the player has no second copy to do. That is the design
   in Appendix B.

The one thing that forces a copy in the other direction is scaling. `setup()`
tells VLC the size to deliver. If we ask for the window size and the video is
1920x1080 in a 1280x720 window, VLC needs a converter that also scales: the
`i420_rgb` converters only convert chroma and copy rows 1:1 (they reject odd
width or height outright in `Activate()`) **[src]**, while
`modules/video_chroma/swscale.c` does scale but costs
`video_chroma/libswscale_plugin.so` (26 KB) + `ffmpeg7-libswscale` (0.63 MB
installed) **[pkg]**. For the first milestone: **ask for the video's own size,
open the window at that size, and crop if it does not fit.** A nearest-neighbour
scaler in the player is ~20 lines and 0 bytes; measuring whether it is needed is
part of milestone 2.

### 3.4 Integration with the window server

The protocol already fits **[ours]**, `src/ipc.h`:

```
app -> wm : HELLO <id> <title...> <w> <h>
wm  -> app: OK <shm-path> <stride>
app -> wm : FRAME
```

- The app `mmap`s the shm file the window server created (`/dev/shm/mystic-*`).
- **The buffer is screen-sized, not window-sized.** `src/wmsrv.c` sets
  `c->maxw = max(w, screen_w)`, `c->stride = c->maxw`, and `OK` reports
  `path stride maxw maxh`. So a 640x360 window still gets a 1280-pixel stride,
  and `pitch = stride * 4 = 5120`.
- Therefore tell VLC the video is **`w` = window width, `h` = window height,
  pitch = stride*4**: each row is written at the start of a stride-wide row, no
  scaling, no repacking. The compositor copies exactly `cl->w` pixels per row
  from `cl->px`, which is the same window, so the extra column space is never
  read. Getting this wrong - telling VLC `pitch = w*4` - makes VLC write rows
  on top of each other, which looks like a diagonal smear.
- `planes[0]` points at that mapping; `display` writes the FRAME line.
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
hand-written list. Four profiles **[pkg]** + **[apk]**:

| Profile | Plugins kept | Closure | Download | Installed | Payload gzipped |
|---|---|---|---|---|---|
| `minimal` | 10 (vmem, rv32, chain, i420_rgb, image/raw demuxers, rawvideo/png/jpeg codecs, filesystem access) | **8 packages** | **1.98 MB** | **4.01 MB** | **1.10 MB** with symlinks (17 files, 2.85 MB raw) / **2.15 MB** as the copies our image actually gets (23 files, 5.37 MB raw) |
| `trim` | 17 (+ mp4/mkv/avi demux, avcodec, h264/hevc packetizers, http access) | **71 packages** | **38.19 MB** | **103.35 MB** | **67.50 MB** (250 files, 173.15 MB raw) |
| `trim+subs` | 18 (+ `text_renderer/libfreetype_plugin.so`) | **83 packages** | **42.34 MB** | **113.55 MB** | not measured (the freetype stack adds 10.2 MB installed) |
| `full` | the whole package, `apk add vlc` | 279 packages | 184.68 MB | 502.88 MB | n/a |

The `minimal` line is the important one: **1.10 MB gzipped** on top of a 13.42 MB
image (2.15 MB in the shape our stage tree can actually hold - see risk 5, the
symlink copies), for a player that can decode PNG, JPEG and raw frames into a
window. Those files are 1.28 MB of `vlc-libs` itself, 0.28 MB of the ten plugins,
and 1.25 MB of `libjpeg-turbo` + `libpng` + `zlib` + `dbus-libs` **[apk]**.

`trim` is where it becomes a different product. **67.5 MB gzipped is 5x the
entire image.** The reason is visible in the download: ffmpeg's `libavcodec`
links ~24 shared libraries, and the ones that hurt are encoders we never call -
`x265-libs` 21.3 MB, `aom-libs` 8.4 MB, `libSvtAv1Enc` 7.2 MB, `rav1e-libs`
2.4 MB, `libjxl` 3.7 MB **[pkg]**. There is one genuine absurdity worth naming:
`libavutil.so.59` has a hard `DT_NEEDED` on **`libX11.so.6`** **[apk]**, so the
X11-free video player ships an X11 library it never calls - 1.16 MB of it.

Measured on disk after unpacking `--profile trim`: 469 files, 178.83 MB
(the index says 103.35 MB installed; the difference is 117 symlink targets
materialised as copies, which is a Windows artefact of the unpacker - inside the
image they are symlinks).

### 3.7 Which codecs work without ffmpeg

Also worth stating precisely, because it decides whether `minimal` is a toy.
Alpine's VLC build has these **decoders** that need no external codec library
(plugin files in `codec/`) **[apk]**: `rawvideo`, `png`, `jpeg`, `theora`,
`vorbis`, `flac`, `opus`, `speex`, `mpg123` (MP3), `a52` (AC-3), `faad` (AAC),
`vpx` (VP8/VP9), `dav1d` (AV1), `subsdec`.

It has **no H.264, HEVC, MPEG-2 or VC-1 decoder at all**: there is no
H.264 decoder plugin, no `libmpeg2_plugin.so`, no `libvc1_plugin.so` in the
package. The `packetizer/libpacketizer_h264_plugin.so` and `..._hevc_plugin.so`
files only *parse* the bitstream, and `codec/libx264_plugin.so` /
`codec/libx265_plugin.so` are **encoders** - neither decodes anything. So:

- **Without libavcodec** you can play: WebM/Matroska with VP8/VP9/AV1, Ogg
  Theora, FLAC/MP3/Opus/Vorbis audio, and still images. That is a real player,
  and it is the `minimal` set plus the `libvpx`/`libdav1d`/`libtheora`/`opus`
  libraries - **7.50 MB installed / 3.08 MB of `.apk`** for exactly
  `libvpx 3.34 + libdav1d 1.75 + libtheora 0.73 + libvorbis 0.80 + opus 0.47
  + libogg 0.04 + mpg123-libs 0.37` **[pkg]** (the plugins themselves are
  14-26 KB each).
- **H.264/HEVC/MPEG-2 require `codec/libavcodec_plugin.so`**, which is the
  38 MB download. There is no cheaper option in Alpine's VLC 3.0.23.

---

## 4. Audio: honest verdict

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

Decoding is not the problem. **There is nothing to write the PCM to.**

| Thing needed | State today | Evidence |
|---|---|---|
| An emulated sound card | **absent**: QEMU is started with `-kernel -initrd -m -vga -drive -netdev -device virtio-net-pci` and no sound device or `-audiodev` | `image.sh` BOOT array **[ours]** |
| A driver | **absent**: `CONFIG_SND=m`, `CONFIG_SND_PCM=m`, `CONFIG_SND_HDA_INTEL=m`, `CONFIG_SND_VIRTIO=m` are all modules, and `tools/fetch-kernel.sh` stages `gpu video i2c input block net fs/* lib` but **not `kernel/sound`** - 188 `.ko` files end up in `stage/`, zero of them sound | `build/cache/linux-virt-6.18.63-r0/boot/config-6.18.63-0-virt`, `find stage/lib/modules -name '*snd*'` **[ours]** |
| A `/dev/snd/*` node, a mixer, a volume UI | **absent**; nothing in `stage/init` creates or uses one | `stage/init` **[ours]** |

So: **sound cannot work today, and it is not a libVLC problem.** What it would
take, with measured costs:

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
host, which is a QEMU-build/host question and not a guest one. Until steps 1 and
2 are done, `libvlc_audio_set_callbacks` would happily deliver PCM to a ring
buffer that nobody drains - so the honest first player should pass `--aout=dummy`
(or simply not set audio callbacks) and show "no sound device" in its status line
rather than pretend.

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
| `trim` extra libraries (avcodec, avformat, avutil, swresample, dav1d, matroska, ebml, libstdc++, libgcc) | 25,737,576 | **[apk]** |
| **`trim` mapped-code floor** | **29.81 MB** | **[calc]** |

The player's own allocation is the frame buffer, and if the frame buffer *is* the
window mapping then it is already counted in the window server's budget. The
desktop uses two full-screen surfaces (~4.1 MB each at 1280x800) **[ours]**.

What is **not** measured here is libVLC's heap: thread stacks, the decoder's
internal buffers, the plugin registry, and for `trim` whatever libavcodec
allocates while decoding (which for H.264 1080p is tens of MB in the general
case). **[est]** With 256 MB of RAM, `minimal` should sit comfortably
(frame buffer + 2.8 MB of code + VLC's heap, call it "tens of MB at most");
`trim` is the one to watch. The way to settle it is one boot and reading
`/proc/<pid>/status` `VmRSS` plus `VmHWM` after playing a file - that is
milestone 1's acceptance test, and it takes ten minutes once the tree exists.

---

## 6. Verdict

**Is a libVLC-based player with `vmem` output realistic for a 13 MB image?**

- **As a player for images and VLC's own codecs (VP8/VP9/AV1/Theora/MP3/FLAC/
  Opus): yes.** 1.98 MB of download, **1.10 MB gzipped** added to the image
  (2.15 MB with the symlink copies our packer produces), no
  X11, no Wayland, no window-system server, no conversion code. The display
  path is the callbacks in section 3.1 and a `FRAME` line.
- **As a player for H.264/HEVC/MP4: not inside the initramfs.** 38.19 MB of
  download and **67.5 MB gzipped**, i.e. five copies of the whole OS, to gain
  one decoder. If the design wants that, it belongs on the data disk or a
  read-only 9p mount the way `docs/BROWSER_PLAN.md` proposes for the browser
  engine - and the 64 MB data disk is too small for it too, so the disk size
  becomes a knob (`MYSTIC_DISK_MB`).
- **Sound: no, not today**, for reasons that have nothing to do with VLC
  (section 4). 732 KB of kernel modules and one QEMU device away, plus a sink.

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
`libvlc.so.5`, `libvlccore.so.9`, the 10 plugins and their libraries, and prints
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

**M2 - a real video, VLC's own codecs.**
Add `libvpx` (VP8/VP9), `libdav1d` (AV1), `libtheora`, `libvorbis`, `libogg`,
`libopus`, `libmpg123` **plugins** (each 14-26 KB) and their libraries:
**7.50 MB installed / 3.08 MB of `.apk`** **[pkg]**. Play a WebM file.
*Acceptance:* 30 s of VP9 in WebM plays at the file's frame rate in a window;
`display` is called ~N times for N frames.

**M3 - decide about H.264.**
Either accept a 67.5 MB tree on the data disk (`--profile trim --download`,
which works today and produces 469 files), or stay with M2. Nothing in the code
changes; only which libraries are on the filesystem.

**M4 - sound, if the design wants it.** Section 4, steps 1-3. Acceptance: a small
test program opens `/dev/snd/pcmC0D0p`, writes a 440 Hz sine, and it is audible
from the host.

---

## 8. Risks, and what this document does not prove

1. **The plugin set is a hypothesis tested only by `DT_NEEDED`.** A plugin whose
   libraries all resolve can still fail in `Activate()` (missing hardware,
   missing sub-module), and VLC then falls back to another module or reports the
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
python tools/fetch-vlc.py --dry-run       # resolve and print; build nothing
python tools/fetch-vlc.py --profile minimal
python tools/fetch-vlc.py --force         # ignore the caches
```

- **It builds by default.** The expensive steps are cached: `build/vlc-index/`
  (APKINDEX), `build/vlc-apks/` (the `.apk` files), `build/vlc-root/` (a
  signature stamp, so a second run makes no network call) and the package
  itself (`build/vlc-pkg.stamp`, keyed on the app sources, the manifest and
  this script). `--dry-run` downloads nothing beyond the two vlc packages it
  must read `DT_NEEDED` from, and says so.
- Output 1, the **image bundle**: `stage/usr/share/mystic/apps/vlc/{manifest,bin}`.
  `bin` there is the player compiled statically (171,152 bytes). It cannot
  dlopen, so its window says libVLC is not installed and that `pkg install vlc`
  brings it: a placeholder with the truth in it, never a fake player.
- Output 2, the **package** `build/repo/vlc.tar.gz` (38.79 MB compressed /
  38,792,152 bytes, 103.39 MB installed / 103,387,892 bytes) plus the `vlc`
  row in `build/repo/index.tsv`:
  `manifest`; `bin` (static launcher, 29,128 bytes, no PT_INTERP);
  `libexec/vlcplayer` (dynamic, 531,520 bytes, PT_INTERP
  `/lib/ld-musl-x86_64.so.1`, RUNPATH `$ORIGIN/../lib`);
  `lib/ld-musl-x86_64.so.1` (from Alpine's musl apk, 670,312 bytes, mode 0755);
  and the closure - 129 regular files under `lib/` (128 shared objects plus
  the loader), 113 soname/version symlinks, 27 plugin modules in 7 plugin
  directories, and the soname aliases
  `libvlc.so.5` and `libvlccore.so.9` that `dlopen()`/`DT_NEEDED` actually use.
- Why three files: the image has `/lib/ld-musl-x86_64.so.1` from the Alpine
  base, but the jail is a chroot that binds only `/bin/app`, `/app` (the
  bundle), the fonts, `/dev` bits and `/proc`. PT_INTERP cannot resolve there,
  so `bin` is static and runs the bundled loader explicitly:
  `lib/ld-musl-x86_64.so.1 --library-path <bundle>/lib <bundle>/libexec/vlcplayer`.
- The manifest's `exec` is a **bare file name**, and it has to be: `wmsrv`
  rejects any `exec` containing `/` or `..` (`wms_launch_arg`, "unsafe exec"),
  so `exec = libexec/vlcplayer` is not a layout Mystic can launch and no
  document should suggest it. `exec = bin` names the static launcher at the
  bundle root; `libexec/vlcplayer` and `lib/` are internal paths the
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
==> profile 'minimal' (libvlc + 10 plugins): 8 packages,
    1.98 MB download, 4.01 MB installed
==> profile 'trim' (libvlc + 17 plugins): 71 packages,
    38.19 MB download, 103.35 MB installed
```

`DT_NEEDED` of the plugin set, read from the unpacked `.so` files:

```
==> DT_NEEDED of those files: so:libavcodec.so.61, so:libavformat.so.61,
    so:libavutil.so.59, so:libebml.so.5, so:libgcc_s.so.1, so:libjpeg.so.8,
    so:libmatroska.so.7, so:libpng16.so.16, so:libstdc++.so.6,
    so:libvlccore.so.9, so:libz.so.1
```

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
trim     250 files    173.15 MB raw     67.50 MB gzipped   (duplicate copies included)
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

### Three things had to be true before a frame appeared

1. **The player must be dynamically linked.** A static musl binary's `dlopen()`
   is a stub: the app reported "Dynamic loading not supported" and showed the
   install command instead of a window. The bundle therefore carries a dynamic
   `bin` with `PT_INTERP /lib/ld-musl-x86_64.so.1`, `src/lib/ld-musl-x86_64.so.1`
   is in the image, and the ELF interpreter string has to be corrected after
   linking: zig prefixes `-dynamic-linker` with its own sysroot, so the built
   binary pointed at `C:/Users/.../lib/ld-musl-x86_64.so.1` until the `PT_INTERP`
   bytes were rewritten.
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
   `libX.so` alias from the versioned names: 337 links.

### Audio

`libalsa_plugin.so` and `libasound.so.2` now ship in the bundle
(`tools/pack-vlc.py --extras build/vlc.apk build/alsa.apk`), and the player's
footer shows `vol 100` where it used to show `no audio output` - the ALSA output
opened the OS's PCM device and libVLC reports a working volume.
`docs/screenshots/video-vlc-audio.png`.

Why it was missing: the first closure was built with a filter that kept 27
plugins, and `audio_output/` contained only `libadummy_plugin.so` - a sink that
discards samples - with no `libasound` dependency at all. Nothing failed loudly:
libVLC simply reported that it had no audio output and played the video.
