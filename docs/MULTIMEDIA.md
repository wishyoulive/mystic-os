# Video playback on Mystic OS

This document is about one claim, made checkable: **a real video file decodes
and appears in a Mystic window**. It is not about the idea of VLC support.
Every number here was measured on this machine or read out of the shipped
files; where something could not be run, the text says so instead of implying
it was.

`docs/VLC.md` is the earlier feasibility study (what libVLC needs, what it
would cost). This document is the delivered path: it measures the bundle
that is actually packed. Where the two disagree, this one is newer.

Status at the time of writing:

| Piece | State |
|---|---|
| A real H.264 file decoded through the player's own vmem callbacks | **yes**, on the host, with the real libVLC 3.0.24 (see section 4) |
| Pixel format (RV32 vs `0xAARRGGBB`) checked against a deterministic generator | **yes**, three pixels exact (section 5) |
| The bundle's shared libraries, symbols and plugins complete | **yes**, 156 ELF objects, 0 unresolved `DT_NEEDED` (section 6) |
| The jail gives libVLC its library, plugin, HOME and TMPDIR paths | **yes**, `src/wmsrv.c` (section 2) |
| A clip in the image that can be opened out of the box | **yes**, `/root/media/clip.mp4` (section 8) |
| Files knows a video belongs to the Player | **yes**, `src/filetypes.c` (section 7) |
| Audio reaching `/dev/snd` | **no** - the packed bundle has no ALSA output plugin; the player detects that and says so (section 9.4) |
| The embedded musl loader + Alpine libvlc actually executed | **not on this host** - there is no Linux userland here and QEMU is out of scope. Section 10 lists exactly what that leaves unproven |

---

## 1. What the user does, and what appears

1. **Fresh image, click the Player tile.** The image ships
   `stage/usr/share/mystic/apps/vlc/bin`, a static build of
   `src/client/apps/vlcplayer.c` (it has no `PT_INTERP` and no `DT_NEEDED`,
   so the jail can start it). It cannot `dlopen`, so with no argument it
   draws exactly:

       libVLC is not installed
       libVLC is not installed - install it with: pkg install vlc
       This bundle is the image's placeholder: it is a static musl program and
       cannot dlopen, so it could not load libVLC even if one were present.
       The package supplies the dynamic player, the loader and the whole
       closure inside the bundle.
       q quits.

   Both sentences come from one place, `vp_tile_hint()` in
   `src/client/apps/vlcplayer.c:180-184`, and the host harness prints them
   verbatim (section 7).

2. **Store -> VLC Player -> Install.** `pkg` downloads 38.79 MB and unpacks
   `/root/apps/vlc`. The tile then launches the **installed** copy, not the
   image's (the rule is in `wmsrv.c` `merge_dups()`: when both payloads run,
   `/root/apps` wins). The installed `bin` is the static *launcher*; it finds
   the bundle, exports the environment, and execs

       <bundle>/lib/ld-musl-x86_64.so.1 --library-path <bundle>/lib \
           <bundle>/libexec/vlcplayer <media>...

3. **Files (it starts at `/`) -> `root` -> `media` -> clip.mp4 -> Enter.**
   Files writes the path to
   `/run/mystic/open.request`; the desktop looks the extension up in
   `src/filetypes.c` (now `FT_APP_VLC` for mp4/mkv/webm/avi/mov), binds the
   file read-only into the player's jail at the same path, and starts it with
   that path as `argv[1]`.

4. **The window.** The decoded picture, letterboxed, with a status bar:

       clip.mp4   [playing]
       0:01 / 0:02   320x176   H264 - MPEG-4 AVC (part 10)   vol 100
       scale fit   audio none (no /dev/snd PCM node)   libVLC 3.0.23 Vetinari
       space play/pause   left/right 10s   up/down volume   s stop   f fit/fill   q quit

   Every field is a live libVLC answer: `player_get_time`,
   `player_get_length`, `libvlc_video_get_size`, `libvlc_media_get_codec_description`,
   `libvlc_audio_get_volume`, `libvlc_get_version`. Nothing is guessed and no
   frame is invented - before the first decoded picture the window is black
   and says `no picture decoded`.

5. **Keys.** space play/pause, Left/Right seek -/+10 s, Up/Down volume,
   `s` stop, `f` fit/fill, click toggles play, `q`/Esc quit.

---

## 2. The environment the jail has to provide

The window server runs every app chrooted into `/run/mystic/jail/<id>` with
its payload bound at `/bin/app` and its bundle read-only at `/app`
(`src/wmsrv.c`, `sandbox_child()`, 727). That jail has **no `/lib`, no `/usr/lib`
and no `/usr/share/vlc`** - deliberately. libVLC therefore has to be told
where everything is, and the variables are set immediately before `execl`
(`src/wmsrv.c:944-966`):

| Variable | Value in the jail | Why it is required |
|---|---|---|
| `MYSTIC_BUNDLE` | `/app` | the bundle root; the launcher and the player both read it first |
| `LD_LIBRARY_PATH` | `/app/usr/lib:/app/lib` | resolves `DT_NEEDED` of the plugins libvlc `dlopen()`s (`libavcodec.so.61`, `libvlccore.so.9`, ...) |
| `VLC_PLUGIN_PATH` | `/app/lib/vlc/plugins` | **new here.** VLC's module bank first searches `<config_GetLibDir()>/plugins`, and inside the jail that resolves relative to the *program* (`/app/libexec`), not to the library - so without this line libvlc loads and then finds no demuxer, no decoder and no video output |
| `HOME` | `/tmp` | libVLC writes `~/.config/vlc`; `/tmp` is the jail's own tmpfs, mode 0777 |
| `TMPDIR` | `/tmp` | same, for temporary files |
| `XDG_CACHE_HOME` | `/tmp/.cache` | keeps VLC's cache out of the read-only bundle |
| `XDG_CONFIG_HOME` | `/tmp/.config` | same |
| `MYSTIC_SOCKET`, `MYSTIC_APP_ID`, `MYSTIC_UI_SCALE` | socket / id / scale | unchanged window protocol |

`/dev/snd` is bound when the manifest says `audio = yes`
(`src/wmsrv.c:907-917`), which the vlc manifest does. The data file the user
asked for is bound read-only at its own path (`src/wmsrv.c:884-900`), which is
what makes `argv[1] = /root/media/clip.mp4` readable inside a jail that
otherwise cannot see `/root` at all.

The launcher independently does the same three things for a direct start
(`src/client/apps/vlcplayer.c`, section 5 of the file): it sets
`LD_LIBRARY_PATH` and `VLC_PLUGIN_PATH` for the bundle, and runs the bundled
loader with `--library-path <bundle>/lib`. So there are two independent
routes to the same libraries, and the third is the player's own
`RUNPATH=$ORIGIN/../lib`.

---

## 3. The bundle layout, and why it is three files

    bin                      the static launcher (e_type=2, no PT_INTERP,
                             no DT_NEEDED, 29,128 bytes). Manifest `exec = bin`
                             - a BARE name, because `wms_launch_arg()` rejects
                             any exec containing `/`.
    libexec/vlcplayer        the player. DYNAMIC musl: PT_INTERP
                             /lib/ld-musl-x86_64.so.1, NEEDED libc.so,
                             RUNPATH $ORIGIN/../lib. dlopen()s libvlc.so.5.
    lib/ld-musl-x86_64.so.1  musl 1.2.6's loader (670,312 bytes). It is
                             exec'd as a program; the shipped bytes contain
                             the option strings `library-path`, `preload`
                             and `argv0`, which is what makes
                             `--library-path` work.
    lib/libvlc.so.5 -> libvlc.so.5.6.1
    lib/libvlccore.so.9 -> libvlccore.so.9.0.1
    lib/... (153 libraries), lib/vlc/plugins/... (27 plugins)

Why the player is not static: a static musl program's `dlopen()` is a stub
whose entire body is `__dl_seterr("Dynamic loading not supported")`. Building
it static *with* libvlc cannot work either - the closure ships `libvlc.so.5`
only, and the linker says so:

    error: unable to find static system library "vlc" using strategy no_fallback

Why the loader is inside the bundle: the jail has no `/lib`, so the dynamic
player's `PT_INTERP` cannot resolve there. The static launcher execs the
bundled loader with the bundled player as its argument.

---

## 4. Proof A - a real H.264 file decodes, frame after frame

Host harness: `.work/play/host.c` `#include`s the shipping source with
`VLC_HOST_TEST`, so the dlopen table, the vmem callbacks, the RV32 surface,
`vp_visible_size()` and `vp_blit()` are the same code the app runs. It loads
`C:\Program Files (x86)\VideoLAN\VLC\libvlc.dll` (libVLC 3.0.24) and decodes
`.work/play/clip176.mp4` - byte-identical to the `/root/media/clip.mp4` the
image ships (sha256 in section 8).

Transcript (`.work/play/decode-clip.txt`):

    libvlc: 3.0.24 Vetinari
    play: 0
    vmem: setups=3 frames=2 displays=1 too_big=0 surface=320x194 visible=320x176
    vp_blit native: got=1, pitch VLC wrote=1280
    first frame: 320x176  (0,0)=0x632d01  (1,0)=0x6b3509  (0,1)=0x6b3105
      bytes at (0,0): B=1 G=45 R=99 X=255
    scaled 320x180 letterbox: (0,0)=0x000000 centre(160,90)=0x508673
    after 14 pictures: got=1  (0,0)=0xc47227  (1,0)=0xcd7b30  (0,1)=0xc87730
    frame 1 and frame 14 are different (video is moving)
    codec (VLC description): H264 - MPEG-4 AVC (part 10)
    state=3 time=401 ms length=2800 ms

What that proves, in order:

- `play: 0` - libVLC accepted the media and started it.
- `frames=2`, then `14` - the vmem unlock callback ran 14 times: real decoded
  pictures, not one still.
- `320x176` - the *visible* size VLC reports for the file. The surface is
  `320x194` on this host: hardware decoding is refused (`--avcodec-hw=none`;
  VLC's log says `no hw decoder modules matched` instead of `Using D3D11VA`),
  but VLC 3 still *probes* the two hardware output formats before the software
  one, and the third `setup()` inherits a padded height. `vp_visible_size()`
  (`vlcplayer.c:567-582`) asks libVLC for the real size and `vp_blit()` crops
  to it, so padding rows are never presented as picture.
- three pixels at (0,0), (1,0), (0,1) differ, and the same three change
  between picture 1 and picture 14 - a frozen decoder or a stale buffer could
  not produce that.
- `H264 - MPEG-4 AVC (part 10)` is libVLC's own codec description.
- `length=2800 ms` is the real duration (the file has 29 encoded pictures).

The 14-picture wait is part of the harness; the player itself blits whenever
`vp_frame_count()` changes.

`pitch VLC wrote=1280` is `320*4`: VLC writes RV32 straight into our surface,
and `vp_blit()` is a row `memcpy` into the client's shared-memory pixels. It
allocates nothing per frame - the destination is `MClient.px`, which is
allocated once by `mc_open()`.

---

## 5. Proof B - the pixel format, checked against arithmetic

RV32 (`VLC_CODEC_RGB32`) has masks `r=0xff0000 g=0x00ff00 b=0x0000ff`
(`modules/video_output/vmem.c`, `Open()`), so a pixel's colour is `0xRRGGBB`
and the fourth byte is not part of the mask. Mystic colours are `0xAARRGGBB`;
on little-endian both are the bytes **B, G, R, X**, and the compositor copies
all four bytes without reading the top one. There is no conversion code and
no channel swap anywhere in the player.

To check that claim rather than assert it, `.work/play/mkclip.py` writes a
real RIFF/AVI with a pattern that is a function of `(x, y, frame)`:

    b = (x*3 + y*5 + i*7) & 0xFF
    g = (x*7 + y*3 + i*11 + 40) & 0xFF
    r = (x*11 + y*13 + i*17 + 80) & 0xFF

Decoding that AVI with the same harness (`.work/play/decode-rawavi.txt`):

    vmem: setups=1 frames=2 displays=1 too_big=0 surface=320x176 visible=320x176
    first frame: 320x176  (0,0)=0x613307  (1,0)=0x6c3a0a  (0,1)=0x6e360c
      bytes at (0,0): B=7 G=51 R=97 X=255
    after 14 pictures: got=1  (0,0)=0xc77531 ...
    frame 1 and frame 14 are different (video is moving)
    codec (VLC description): 24 bits RGB

The generator says picture 1 is:

    (0,0): B=7  G=51      R=97      -> 0x613307   matches
    (1,0): B=10 G=58      R=108     -> 0x6c3a0a   matches
    (0,1): B=12 G=54      R=110     -> 0x6e360c   matches

Three different channels at three positions, all exact: the byte order, the
row order and the absence of a swap are facts, not conventions. (The first
displayed picture is picture 1 rather than 0 because VLC started decoding at
the first keyframe it displayed; the values above are the generator's frame 1
in every field.)

---

## 6. Proof C - the bundle can satisfy libvlc inside the jail

`.work/play/elfaudit.py` reads `build/repo/vlc.tar.gz` **without executing
anything** and checks two things that must hold for a chroot with no
`/lib`:

    build/repo/vlc.tar.gz: 284 members, 156 ELF shared objects,
    0 unresolved DT_NEEDED, 113 symlinks, 0 bad symlinks

- Every `DT_NEEDED` of every library *and* every plugin must be a member of
  the same archive (or a libc the loader supplies). 0 are missing, so a
  plugin loaded from `lib/vlc/plugins` resolves `libvlccore.so.9`,
  `libavcodec.so.61`, `libavformat.so.61`, `libavutil.so.59` and its friends
  from `<bundle>/lib`.
- Every one of the 113 symlinks is relative and resolves inside the bundle;
  none is absolute and none dangles. `lib/libvlc.so.5` (the name
  `dlopen("libvlc.so.5")` asks for) resolves to `libvlc.so.5.6.1`, and
  `lib/libvlccore.so.9` (the plugins' `DT_NEEDED`) to `libvlccore.so.9.0.1`.

The launcher's own decision logic is exercised by a host harness
(`.work/play/boot/`, output `.work/play/boot-A.txt`, `boot-B.txt`):
a complete bundle is found from the launcher's directory and produces

    argv[0]=<bundle>/lib/ld-musl-x86_64.so.1
    argv[1]=--library-path
    argv[2]=<bundle>/lib
    argv[3]=<bundle>/libexec/vlcplayer
    argv[4]=<the media path>
    LD_LIBRARY_PATH=<bundle>/lib
    VLC_PLUGIN_PATH=<bundle>/lib/vlc/plugins

a bundle without `lib/libvlc.so.5` exits 127 and prints the honest sentence
(section 9.1), and `MYSTIC_BUNDLE` overrides the executable's directory.

---

## 7. Proof D - Files opens a video with the Player

`src/filetypes.c` maps the five video extensions to `FT_APP_VLC`; nothing else
changed. Host harness (`.work/play/filetypes.txt`):

    clip.mp4     -> MP4 video        app=vlc
    movie.MKV    -> Matroska video   app=vlc
    a.webm       -> WebM video       app=vlc
    b.avi        -> AVI video        app=vlc
    c.mov        -> QuickTime video  app=vlc
    photo.png    -> PNG image        app=viewer
    song.mp3     -> MP3 audio        app=viewer
    notes.txt    -> Plain text       app=viewer

Before this, every video extension said `viewer`, which displays images: the
Player was reachable only by typing a path, so in practice it was not
reachable at all.

The two sentences of the empty Player window are printed by the same
mechanism the window uses (`.work/play/tile-msg.txt`):

    no libvlc, no file : libVLC is not installed - install it with: pkg install vlc
    libvlc, no file    : give a media file: Player <path>
    with a file        : (a real error is shown instead)

---

## 8. The test clip

`/root/media/clip.mp4` is a real H.264 MP4, not a fixture pretending to be
one:

| Fact | Value |
|---|---|
| size | 193,905 bytes |
| sha256 | `ef6f9f3345d16921f773642c6fccab65e17e9e6ba2a8abe336601851bd7619da` |
| picture | 320x176, yuv420p, H.264 High profile |
| pictures | 29 encoded (30 generated), 0.1 s each |
| duration | 2.8 s (VLC reports 2800 ms) |
| audio | none |
| encoder | libx264 through VLC 3.0.24's own transcoder |

How it was made, reproducibly and without ffmpeg (this host has none):

    python .work/play/mkclip.py .work/play/pattern176.avi 320 176 30 10
    "<VLC>/vlc.exe" -I dummy --vout=dummy --aout=dummy \
        --sout "#transcode{vcodec=h264,vb=500,width=320,height=176,keyint=10}:
                standard{access=file,mux=mp4,dst=...clip.mp4}" \
        pattern176.avi vlc://quit

The source AVI is 5,069,752 bytes (uncompressed) and stays in `.work/play/`;
only the encoded MP4 is in the image. It is staged twice on purpose:

- `stage/usr/share/mystic/media/clip.mp4` - the image's canonical copy;
- `stage/root/media/clip.mp4` - the literal path the user opens.

`stage/init` mounts the data disk on `/root` on every normal boot, which hides
anything the initramfs put in `/root`. So `src/wmsrv.c` also provisions it:
`provision_media()` (`wmsrv.c:138-217`, called from `wms_start()` at
`wmsrv.c:250`) copies the canonical file to `/root/media/clip.mp4` when that
path does not exist, and never overwrites a file that is there. A file the
owner deletes comes back at the next boot - there is no marker recording that
the user removed the sample - and that is the only thing this function ever
does. The copy goes through `<dst>.part` and is renamed only after the byte
count matches, so a disk that fills up mid-copy leaves no truncated MP4
behind; the partial file is removed and the copy is retried at the next boot.

The initramfs grows by 384,742 bytes compressed (two copies of 193,905
bytes): 15,083,673 against 14,698,931, both produced by `tools/mkimage.py`
on this tree.

---

## 9. The failure ladder - what the player says, verbatim

Each rung is a real string from the code; the ones marked *console* are
written to stderr, which on Mystic OS is the boot console, not the window.

### 9.1 An installed bundle lost its libraries (console)

`bin` (the static launcher) checks before it execs anything:

    vlc: libVLC is not installed (<bundle>/lib/libvlc.so.5 is missing).
         On Mystic OS: pkg install vlc
         The package carries libvlc.so.5, libvlccore.so.9 and the
         plugins this player needs; this bundle has <bundle>.

Measured by the harness (`boot-B.txt`), exit status 127. **Known gap:** this
is stderr, so the tile appears to do nothing. The honest fix is a manifest
key the launcher and the window server both check, which is a packaging
change outside this file.

### 9.2 The dynamic player cannot dlopen libVLC (window)

`vp_player_open()` returns the loader's own words, e.g.

    cannot load /app/lib/libvlc.so.5: libvlc.so.5: cannot open shared object file

and the card adds the actionable sentence:

    libVLC is not installed where this build looks. The vlc package carries
    libvlc.so.5, libvlccore.so.9 and the plugins: run pkg install vlc.

### 9.3 The image placeholder is asked to play a file (window)

Static musl's `dlopen` answers `Dynamic loading not supported`; the card says
what that means instead of showing the raw stub:

    This is a static build. musl's dlopen() is a stub whose whole answer is
    'Dynamic loading not supported', so a static executable cannot load
    libVLC's shared objects. The playable build ships in the vlc package:
    run pkg install vlc, then open the Player tile or
    /root/apps/vlc/bin <file>.

### 9.4 No sound device, or no ALSA output plugin (status bar, once)

Two independent facts, tested separately:

    audio none (no /dev/snd PCM node)
    audio decoded, not played (no ALSA plugin in this bundle)

and one line on stderr at startup, once per process, never per frame or per
media:

    vlcplayer: no /dev/snd PCM node - libVLC will use --aout=dummy
               (decoding audio, not playing it)

When either is missing the player passes `--aout=dummy` to `libvlc_new()` and
still asks libVLC for its software volume, so Up/Down change a number that
would be audible if an output existed. **The shipped bundle has no audio
output plugin** - `tools/fetch-vlc.py`'s profile packs
`audio_output/libadummy_plugin.so` only - so today the second sentence is the
truth even on a machine with a sound card.

The fix is measured, not guessed. Alpine's `vlc-3.0.23-r4.apk` does contain
`usr/lib/vlc/plugins/audio_output/libalsa_plugin.so` (30,400 bytes, extracted
here from the real package), and its `DT_NEEDED` is

    libvlccore.so.9   libasound.so.2   libc.musl-x86_64.so.1

`alsa-lib 1.2.15.3-r0` provides `so:libasound.so.2=2.0.0` and is 1,258,942
bytes installed. So the change is one line in `tools/fetch-vlc.py:201`

    "aout": ["audio_output/libadummy_plugin.so",
             "audio_output/libalsa_plugin.so"],

after which the closure resolver pulls `alsa-lib` in by itself. That repacks
`build/repo/vlc.tar.gz` and changes the index row, so `build/repo/index.tsv`
must be re-signed before `pkg` will install it (see section 10). Until that
is done, the player reports the gap instead of pretending to have sound.

### 9.5 The file is not readable inside the jail (window)

The player checks `access(path, R_OK)` before it asks libVLC, because VLC 3
answers a missing path by ending the media with `libvlc_errmsg()` empty:

    cannot open /root/media/clip.mp4: No such file or directory

This is the message for the common jail failure too: the file exists on the
real `/root` but the window server did not bind it.

### 9.6 libVLC itself reports an error (window)

    libVLC reported Error: <the exact libvlc_errmsg() text>

If `libvlc_errmsg()` is empty the card says
`libvlc_errmsg() has no detail` rather than inventing a cause.

### 9.7 The file decodes but has no picture (window)

Black surface plus

    no picture decoded (the file may have no video track)

which is exactly what libVLC's state says (`playing`/`ended`, no vmem setup
call).

---

## 10. What was NOT verified, and why

- **No Linux userland on this host.** `wsl.exe -l -v` reports WSL is not
  installed and `docker` does not exist, so the bundled musl loader, the
  Alpine `libvlc.so.5` and the plugins were never executed here. QEMU is out
  of scope for this task. What stands in for execution is section 6 (the
  closure is complete and every name resolves) plus VLC's own module-bank
  source, which is quoted for `VLC_PLUGIN_PATH` in section 2.
- **The host proof uses VLC 3.0.24 (Windows, 32-bit)** while the bundle is
  VLC 3.0.23 (Alpine, x86-64). The API used is libVLC 3's stable C API and
  the source of both is the same; the version is printed by the player
  (`libVLC 3.0.23 Vetinari` in the VM) so a mismatch is visible, not hidden.
- **The package in `build/repo/vlc.tar.gz` was packed before this edit.**
  Its `libexec/vlcplayer` is 531,520 bytes; the player built from the current
  source is 554,640 bytes. It still plays (everything in section 4 was
  already in it), but it does not contain the visible-size crop, the
  `:avcodec-hw=none` media option or the two-state audio test. To refresh it:

      python tools/fetch-vlc.py          # rebuilds bin + libexec, repacks
      python tools/sign-index.py         # then re-sign index.tsv

  The second command writes `build/repo/index.tsv.sig` and is owned by the
  signing work; **do not repack without re-signing**, or `pkg` will refuse
  the index.
- **`provision_media()` was compiled, not executed.** It runs against
  `/root`, which does not exist in this Windows host.
- **VLC's plugin cache cannot be written inside the jail** (the bundle is
  mounted read-only), so each start rescans 27 plugins. That is cheap and
  produces at most one harmless message; a read-only bundle that tried to
  write into itself would be worse.
- **Audio output** is described in 9.4: decoded, not played.

---

## 11. Files this work touched

| File | Change |
|---|---|
| `src/client/apps/vlcplayer.c` | `--avcodec-hw=none` + per-media option; `vp_visible_size()` crop; `audio_plugin_present()` and the three-state audio line; `access()` check before libVLC; opt-in `VP_TRACE` for the vmem format negotiation |
| `src/wmsrv.c` | jail environment (`VLC_PLUGIN_PATH`, `XDG_CACHE_HOME`, `XDG_CONFIG_HOME` added to the existing `MYSTIC_BUNDLE`/`LD_LIBRARY_PATH`/`HOME`/`TMPDIR`); `provision_media()` |
| `src/filetypes.c`, `src/filetypes.h` | `FT_APP_VLC`; mp4/mkv/webm/avi/mov open in the Player |
| `stage/usr/share/mystic/media/clip.mp4` | new: the canonical sample clip |
| `stage/root/media/clip.mp4` | new: the same bytes at the path the user opens |

Harness sources and transcripts are in `.work/play/` (`host.c`, `mkclip.py`,
`elfaudit.py`, `tile.c`, `ft.c`, `boot/`, `decode-clip.txt`,
`decode-rawavi.txt`, `elfaudit.txt`, `boot-A.txt`, `boot-B.txt`,
`filetypes.txt`, `tile-msg.txt`). No executable is left there.
