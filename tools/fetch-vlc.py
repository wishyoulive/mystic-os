#!/usr/bin/env python3
"""
fetch-vlc.py - build the libVLC video player package for Mystic OS.

One command builds every artifact the player needs and prints the real
numbers it found:

    python tools/fetch-vlc.py                 # resolve, download, unpack, build, pack
    python tools/fetch-vlc.py --dry-run       # resolve, print the plugin audit, pack nothing
    python tools/fetch-vlc.py --audit-only    # which plugins vlc.apk has vs this profile
    python tools/fetch-vlc.py --verify-only   # re-prove the packed tarball loads
    python tools/fetch-vlc.py --profile minimal
    python tools/fetch-vlc.py --profile subs  # playback + FreeType renderer + libass
    python tools/fetch-vlc.py --force         # ignore the caches

What it produces:

    stage/usr/share/mystic/apps/vlc/manifest  the ONE manifest (id 'vlc')
    stage/usr/share/mystic/apps/vlc/bin       the image placeholder: this
                                              player compiled statically. It
                                              cannot dlopen (see below), so
                                              its window says exactly that
                                              and that `pkg install vlc`
                                              fixes it. Never a fake frame.
    build/vlc-root/            the vmem closure (libs + the plugin subset)
    build/vlc-pkg/             the package stage
    build/repo/vlc.tar.gz      the installable package, plus an index row
    build/repo/index.tsv       updated in place and re-signed if a key exists

The package layout - one id everywhere (the directory, the manifest's
'bundle =' line, the Store row and the window's app id):

    manifest                   name/desc/exec/version/network/audio/bundle,
                               and 'placeholder = no' (the image's copy of
                               the same file says 'yes')
    bin                        static launcher (-DVP_BOOT=1): no PT_INTERP,
                               so it runs in a jail that has no /lib
    libexec/vlcplay            the player: DYNAMIC musl, PT_INTERP
                               /lib/ld-musl-x86_64.so.1, RUNPATH $ORIGIN/../lib
    lib/ld-musl-x86_64.so.1    the musl loader from Alpine's musl apk (0755)
    lib/libvlc.so.5 ...        libVLC and its real closure
    lib/vlc/plugins/...        only the plugin files a profile needs

The manifest's 'exec' must be a BARE FILE NAME. src/wmsrv.c (wms_launch_arg)
rejects any exec containing '/' or '..' - 'unsafe exec' - so 'exec =
libexec/vlcplay' is not a layout this OS can launch, and nothing here may
claim otherwise. 'exec = bin' names the static launcher at the bundle root;
libexec/ is an internal path the launcher execs through the bundled loader.

Why a launcher and not just a dynamic bin
-----------------------------------------
The image ships no dynamic loader (stage/lib has no ld-musl), and the
kernel refuses to exec a dynamic ELF whose PT_INTERP does not exist. So the
package's entry point is the static launcher; it replaces itself with the
bundled loader:

    lib/ld-musl-x86_64.so.1 --library-path <bundle>/lib         <bundle>/libexec/vlcplay <args...>

musl's loader supports being run as a program and parses --library-path
(ldso/dynlink.c, __dls3), and it resolves a DT_NEEDED of libc.so or
libc.musl-* to itself (load_library's reserved-name check), so the bundle
needs no separate libc.so.

Why the player is not static, and why -lvlc never appears on its link line
-------------------------------------------------------------------------
Alpine ships libVLC as shared objects only: there is no libvlc.a, and
libVLC's own module loader uses dlopen for plugins. A static musl program
cannot dlopen either - musl's static dlopen is the stub whose whole body is
__dl_seterr("Dynamic loading not supported") (src/ldso/dlopen.c) - so a
static player could never call libVLC. This script builds both ways and
reads the result instead of trusting this paragraph: the static+libvlc link
is attempted and its exact failure printed; the dynamic build is checked for
PT_INTERP, DT_NEEDED and RUNPATH before anything is packed.

Index fields: `S:` is the .apk download size, `I:` the installed size. Both
are printed, labelled. Caches: build/vlc-index/, build/vlc-apks/, and a
signature stamp in build/vlc-root. Everything is written under build/
except the two files in the image stage, which are the bundle the image
ships - and those are only rewritten when their bytes would change.

Honesty notes:
  * The download cap is enforced before anything big is fetched. A profile
    over --max-download-mb is refused unless --force is given.
  * `--profile playback` (the default) ships libavcodec plus VLC's own
    dav1d (AV1), vpx (VP8/VP9), theora, opus, vorbis and mpg123 decoders.
    "libavcodec handles it" is not proof, so `--audit` lists what is really
    in vlc.apk, what this profile takes, and every plugin's DT_NEEDED; the
    package is then rejected unless every DT_NEEDED has an entry with exactly
    that name under lib/ (a SONAME match is not enough) and every undefined
    symbol resolves inside the packed tarball (verify_tarball below).
    VLC 3.0.23 has no libav1_plugin.so and no libmpeg2_plugin.so: AV1 is
    dav1d, MPEG-2 is libavcodec only. The printed numbers are the real ones.
  * No sound driver is staged in the image and QEMU is started without a
    sound device, so this package contains only the dummy audio output.
    libVLC still decodes audio; nothing can play it until a PCM device
    exists. See docs/VLC.md.
"""

import argparse
import glob
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request

ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.24"
REPOS = ["main", "community"]
ARCH = "x86_64"

INDEX_CACHE = os.path.join("build", "vlc-index")
APK_CACHE = os.path.join("build", "vlc-apks")
DEST = os.path.join("build", "vlc-root")
PKG_STAGE = os.path.join("build", "vlc-pkg")
REPO = os.path.join("build", "repo")
PKG_NAME = "vlc"
PKG_VERSION = "3.0.23"

# The bundle id is PKG_NAME, the image stage directory, the manifest's
# 'bundle =' line and the window's app id. There is no second spelling.
IMAGE_BUNDLE = os.path.join("stage", "usr", "share", "mystic", "apps", PKG_NAME)
MANIFEST = os.path.join(IMAGE_BUNDLE, "manifest")
OLD_BUNDLE = os.path.join("stage", "usr", "share", "mystic", "apps", "vlcplayer")

# The dynamic loader the DYNAMIC player needs, taken out of Alpine's musl
# package. It is the only file in the bundle that must be executable.
LOADER_PKG = "musl"
LOADER_IN_APK = "lib/ld-musl-x86_64.so.1"
LOADER_IN_PKG = "lib/ld-musl-x86_64.so.1"

APP_SRC = [
    "src/client/apps/vlcplay.c",
    "src/client/mystic_client.c",
    "src/gfx.c",
    "src/font.c",
    "src/ui.c",
]
BOOT_SRC = ["src/client/apps/vlcplay.c"]

# libVLC is not a package name in Alpine: vlc-libs provides both shared
# objects, so the roots are written the way the index writes them.
BASE_ROOTS = ["so:libvlc.so.5", "so:libvlccore.so.9"]

# A package that only exists to satisfy /bin/sh or the C library must not be
# copied into the tree: the Alpine base image already has both, and a second
# musl in an overlay is how you get a loader that cannot find itself.
BASE_PACKAGES = {
    "musl", "busybox", "busybox-binsh", "alpine-baselayout",
    "alpine-baselayout-data", "apk-tools", "scanelf", "libc-utils",
}

# ---------------------------------------------------------------------------
# Plugin sets. Paths are relative to /usr/lib/vlc/plugins inside vlc.apk.
# These are the files vmem playback actually loads; a missing one is an
# error below, not a silent omission.
# ---------------------------------------------------------------------------
PLUGINS = {
    # vmem hands us each decoded picture. rv32 converts RV24 -> RV32; the
    # i420_rgb converters do the common YUV -> RGB step. No swscale: this
    # player scales into the window itself.
    "vout": [
        "video_output/libvmem_plugin.so",
        "video_chroma/librv32_plugin.so",
        "video_chroma/libchain_plugin.so",
        "video_chroma/libi420_rgb_plugin.so",
        "video_chroma/libi420_rgb_sse2_plugin.so",
        "video_chroma/libi420_rgb_mmx_plugin.so",
    ],
    # Still images and raw video: the cheapest thing that proves the path.
    "image": [
        "demux/libimage_plugin.so",
        "demux/librawvid_plugin.so",
        "codec/librawvideo_plugin.so",
        "codec/libpng_plugin.so",
        "codec/libjpeg_plugin.so",
    ],
    "file": ["access/libfilesystem_plugin.so"],
    # Containers. Each one is VLC's own demuxer, not libavformat.
    "demux": [
        "demux/libmp4_plugin.so",
        "demux/libmkv_plugin.so",
        "demux/libavi_plugin.so",
        "demux/libts_plugin.so",
        "demux/libps_plugin.so",
        "demux/libwav_plugin.so",
        "demux/libogg_plugin.so",
        # .wmv/.wma (WMV3 + WMA decode through libavcodec) and .flac. Both
        # link only libvlccore; 77.7 KB + 29.7 KB.
        "demux/libasf_plugin.so",
        "demux/libflacsys_plugin.so",
    ],
    # What libavcodec itself provides: H.264, HEVC, MPEG-1/2/4, VC-1,
    # VP8/VP9/AV1, AAC/MP3/AC-3/DTS/FLAC/... through one module. There is no
    # separate decoder plugin for H.264, HEVC, MPEG-2 or VC-1 in VLC 3.0.23:
    # libmpeg2_plugin.so and libav1_plugin.so do not exist in the package.
    "ffmpeg": ["codec/libavcodec_plugin.so"],
    # All 14 packetizers. They frame elementary streams inside ts/ogg/ps:
    # without packetizer_a52 an AC-3 track in a .ts is not reassembled, etc.
    # Every one of them links nothing but libvlccore; 571.5 KB in total, of
    # which this profile used to take 333 KB.
    "packetizer": [
        "packetizer/libpacketizer_a52_plugin.so",
        "packetizer/libpacketizer_av1_plugin.so",
        "packetizer/libpacketizer_copy_plugin.so",
        "packetizer/libpacketizer_dirac_plugin.so",
        "packetizer/libpacketizer_dts_plugin.so",
        "packetizer/libpacketizer_flac_plugin.so",
        "packetizer/libpacketizer_h264_plugin.so",
        "packetizer/libpacketizer_hevc_plugin.so",
        "packetizer/libpacketizer_mlp_plugin.so",
        "packetizer/libpacketizer_mpeg4audio_plugin.so",
        "packetizer/libpacketizer_mpeg4video_plugin.so",
        "packetizer/libpacketizer_mpegaudio_plugin.so",
        "packetizer/libpacketizer_mpegvideo_plugin.so",
        "packetizer/libpacketizer_vc1_plugin.so",
    ],
    # Dedicated decoders. Every library they name is ALREADY in libavcodec's
    # closure on this Alpine release (libdav1d 1.5.3, libvpx 1.15.2,
    # libtheora 1.2.0, opus, libvorbis, mpg123-libs), so each costs only the
    # plugin file itself - 14 KB to 38 KB, no new package, no new download.
    # Sizes and DT_NEEDED below are read from vlc.apk by --audit, not guessed:
    #   libdav1d_plugin.so        14,016 B   libdav1d.so.7
    #   libvpx_plugin.so          18,112 B   libvpx.so.11
    #   libtheora_plugin.so       22,360 B   libtheoradec.so.2, libtheoraenc.so.2
    #   libopus_plugin.so         26,304 B   libopus.so.0
    #   libvorbis_plugin.so       30,400 B   libvorbis.so.0, libvorbisenc.so.2
    #   libmpg123_plugin.so       13,936 B   libmpg123.so.0
    # VLC 3.0.23 has NO libav1_plugin.so and NO libmpeg2_plugin.so; AV1 is
    # dav1d, MPEG-2 is libavcodec. --audit prints the absence rather than
    # letting the reader assume otherwise.
    "codecs": [
        "codec/libdav1d_plugin.so",
        "codec/libvpx_plugin.so",
        "codec/libtheora_plugin.so",
        "codec/libopus_plugin.so",
        "codec/libvorbis_plugin.so",
        "codec/libmpg123_plugin.so",
    ],
    # Raw elementary streams: .h264/.hevc/.mpg files that have no container.
    # All three link nothing but libvlccore (42,688 + 18,112 + 14,016 B).
    "raw": [
        "demux/libes_plugin.so",
        "demux/libh26x_plugin.so",
        "demux/libmpgv_plugin.so",
    ],
    # Subtitles, half one: detect a sidecar file and decode its text to SPU
    # subpictures. These three link only libvlccore. Drawing them needs a
    # text renderer, which is the other half below - without it a subtitle
    # track is recognised and decoded but nothing is painted.
    "subs_text": [
        "demux/libsubtitle_plugin.so",
        "codec/libsubsdec_plugin.so",
        "codec/libwebvtt_plugin.so",
    ],
    # Subtitles, half two: FreeType + fontconfig + fribidi + harfbuzz, which
    # is +4.14 MB download / +10.20 MB installed over the default profile
    # (measured from the index, see --audit). Not in the default: the image
    # has TTFs in /usr/share/fonts but no /etc/fonts/fonts.conf, so whether
    # fontconfig can initialise in the guest is a boot-time fact, not a
    # static one. Kept in --profile subs rather than paid for blindly.
    # libass draws ASS/SSA itself (styled positions, fonts, effects) and needs
    # libass.so.9 -> libass, which pulls the same freetype/fontconfig stack the
    # text renderer already needs. Marginal cost over that stack: +0.12 MB
    # download / +0.33 MB installed, so `subs` ships both halves.
    "subs_render": [
        "text_renderer/libfreetype_plugin.so",
        "codec/liblibass_plugin.so",
    ],
    # Audio outputs. alsa is the one that can open the emulated HDA PCM the
    # image now carries; adummy stays as the honest fallback for an image
    # without a sound device (it accepts a volume and discards samples, which
    # is why "vol 100" alone must not be read as "a PCM opened").
    "aout": [
        "audio_output/libalsa_plugin.so",
        "audio_output/libadummy_plugin.so",
    ],
    "http": ["access/libhttp_plugin.so"],
    "subs": ["text_renderer/libfreetype_plugin.so"],
}

PROFILES = {
    # images + raw frames, no external codec library.
    "minimal": PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"],
    # the default: local containers, libavcodec, the dedicated decoders for
    # the formats libavcodec has to reach a separate library for, raw
    # elementary streams, and subtitle decoding (not drawing).
    "playback": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
                 + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["packetizer"]
                 + PLUGINS["codecs"]
                 + PLUGINS["raw"] + PLUGINS["subs_text"] + PLUGINS["aout"]),
    # playback over HTTP as well (adds TLS, more libraries).
    "network": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
                + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["packetizer"]
                + PLUGINS["codecs"]
                + PLUGINS["raw"] + PLUGINS["subs_text"] + PLUGINS["aout"]
                + PLUGINS["http"]),
    # the default plus the FreeType text renderer, so subtitles are painted.
    "subs": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
             + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["packetizer"]
             + PLUGINS["codecs"]
             + PLUGINS["raw"] + PLUGINS["subs_text"] + PLUGINS["aout"]
             + PLUGINS["subs_render"]),
    # what `apk add vlc` would do.
    "full": None,
}

# Files taken out of vlc-libs / vlc. The plugin set is added to this.
LIBVLC_FILES = [
    "usr/lib/libvlc.so.5.6.1",
    "usr/lib/libvlccore.so.9.0.1",
    "usr/lib/vlc/vlc-cache-gen",
]

STAMP_VERSION = "mystic-vlc-root-v2"


# ---------------------------------------------------------------------------
# network + index
# ---------------------------------------------------------------------------
def fetch(url, path=None, timeout=120):
    req = urllib.request.Request(url, headers={"User-Agent": "mystic-os-fetch"})
    data = urllib.request.urlopen(req, timeout=timeout).read()
    if path:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    return data


def load_index(repo, refresh=False):
    """{package: {version,file,download,installed,depends,provides,repo}}."""
    path = os.path.join(INDEX_CACHE, "%s-%s.idx.tar.gz" % (repo, ARCH))
    if refresh and os.path.exists(path):
        os.remove(path)
    if os.path.exists(path):
        with open(path, "rb") as f:
            blob = f.read()
    else:
        blob = fetch("%s/%s/%s/APKINDEX.tar.gz" % (ALPINE, repo, ARCH), path)
    tf = tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz")
    txt = tf.extractfile("APKINDEX").read().decode("utf-8", "replace")

    pkgs = {}
    for block in txt.split("\n\n"):
        if not block.strip():
            continue
        d = {}
        for line in block.strip().split("\n"):
            if len(line) > 1 and line[1] == ":":
                d.setdefault(line[0], []).append(line[2:])
        name = (d.get("P") or [None])[0]
        if not name:
            continue
        ver = (d.get("V") or [""])[0]
        pkgs[name] = {
            "version": ver,
            # APKINDEX v3 has no filename field: the file is <name>-<version>.apk
            "file": (d.get("F") or ["%s-%s.apk" % (name, ver)])[0],
            "download": int((d.get("S") or ["0"])[0] or 0),    # .apk size
            "installed": int((d.get("I") or ["0"])[0] or 0),   # unpacked size
            "depends": " ".join(d.get("D") or []).split(),
            "provides": " ".join(d.get("p") or []).split(),
            "repo": repo,
        }
    return pkgs


def build_world(refresh=False):
    pkgs, provides = {}, {}
    for repo in REPOS:
        idx = load_index(repo, refresh)
        print("==> %-10s %5d packages" % (repo + ":", len(idx)))
        for name, info in idx.items():
            pkgs.setdefault(name, info)
    for name, info in pkgs.items():
        for prov in info["provides"]:
            provides.setdefault(prov.split("=")[0], name)
    return pkgs, provides


def depname(dep):
    """`so:libz.so.1=1.3` -> `so:libz.so.1`; `foo>=1` -> `foo`."""
    return re.split(r"[=<>~]", dep)[0]


def resolve(pkgs, provides, roots):
    """Breadth-first closure over plain names and so:/cmd:/pc: provides."""
    seen, missing = set(), set()
    want = list(roots)
    while want:
        name = want.pop(0)
        node = name if name in pkgs else provides.get(name)
        if node is None:
            if not name.startswith(("so:", "cmd:", "pc:")) and name not in pkgs:
                missing.add(name)
            continue
        if node in seen:
            continue
        seen.add(node)
        for dep in pkgs[node]["depends"]:
            if dep.startswith("!"):            # conflict, not a dependency
                continue
            d = depname(dep)
            if d and d not in seen:
                want.append(d)
    return seen, missing


def check_roots(pkgs, provides, roots):
    """Explicit roots the index cannot resolve - a typo should not be silent."""
    return [r for r in roots if r not in pkgs and r not in provides]


def totals(pkgs, closure):
    real = [n for n in closure if n in pkgs]
    return (sum(pkgs[n]["download"] for n in real),
            sum(pkgs[n]["installed"] for n in real), real)


def report(pkgs, closure, missing, label, top=10):
    dl, ins, real = totals(pkgs, closure)
    print("==> %s: %d packages, %.2f MB download, %.2f MB installed"
          % (label, len(real), dl / 1e6, ins / 1e6))
    if missing:
        print("    not in the index (would have to be added by hand): %s"
              % ", ".join(sorted(missing)))
    print("    heaviest by installed size:")
    for n in sorted(real, key=lambda x: -pkgs[x]["installed"])[:top]:
        p = pkgs[n]
        print("      %-26s %-12s %7.2f MB installed %6.2f MB apk  (%s)"
              % (n, p["version"], p["installed"] / 1e6, p["download"] / 1e6, p["repo"]))
    return dl, ins


# ---------------------------------------------------------------------------
# ELF DT_NEEDED / PT_INTERP - the closure comes from the binaries themselves
# ---------------------------------------------------------------------------
def elf_info(path):
    """(interp or '', [DT_NEEDED]) for an ELF file; ('', []) if unreadable."""
    try:
        with open(path, "rb") as f:
            b = f.read()
        if b[:4] != b"\x7fELF" or len(b) < 64:
            return "", []
        is64 = b[4] == 2
        le = "<" if b[5] == 1 else ">"
        if is64:
            phoff, = struct.unpack_from(le + "Q", b, 0x20)
            shoff, = struct.unpack_from(le + "Q", b, 0x28)
            phentsize, phnum = struct.unpack_from(le + "HH", b, 0x36)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x3a)
        else:
            phoff, = struct.unpack_from(le + "I", b, 0x1c)
            shoff, = struct.unpack_from(le + "I", b, 0x20)
            phentsize, phnum = struct.unpack_from(le + "HH", b, 0x2a)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x2e)

        # PT_INTERP (type 3): the dynamic loader the kernel must find
        interp = ""
        for i in range(phnum):
            off = phoff + i * phentsize
            if is64:
                p_type, = struct.unpack_from(le + "I", b, off)
                p_offset, = struct.unpack_from(le + "Q", b, off + 8)
                p_filesz, = struct.unpack_from(le + "Q", b, off + 32)
            else:
                p_type, = struct.unpack_from(le + "I", b, off)
                p_offset, = struct.unpack_from(le + "I", b, off + 4)
                p_filesz, = struct.unpack_from(le + "I", b, off + 16)
            if p_type == 3:
                interp = b[p_offset:p_offset + p_filesz].split(b"\0")[0].decode(
                    "utf-8", "replace")
                break

        def sh(i):
            off = shoff + i * shentsize
            if is64:
                return struct.unpack_from(le + "IIQQQQIIQQ", b, off)
            return struct.unpack_from(le + "IIIIIIIIII", b, off)

        base = sh(shstrndx)[4]

        def name_at(off):
            end = b.index(b"\0", off)
            return b[off:end].decode("utf-8", "replace")

        dyn = dstr = None
        for i in range(shnum):
            nm = sh(i)[0]
            s = name_at(base + nm)
            if s == ".dynamic":
                dyn = sh(i)
            elif s == ".dynstr":
                dstr = sh(i)[4]
        needed = []
        if dyn is not None and dstr is not None:
            off, end = dyn[4], dyn[4] + dyn[5]
            while off + (16 if is64 else 8) <= end:
                if is64:
                    tag, val = struct.unpack_from(le + "qQ", b, off)
                    off += 16
                else:
                    tag, val = struct.unpack_from(le + "iI", b, off)
                    off += 8
                if tag == 0:
                    break
                if tag == 1:                    # DT_NEEDED
                    needed.append(name_at(dstr + val))
        return interp, needed
    except (ValueError, IndexError, struct.error, OSError):
        return "", []


def elf_needed(path):
    return elf_info(path)[1]


def elf_full(path):
    """Everything the packaging decisions depend on, read from the bytes.

    size, e_type (2 = executable, 3 = shared object), PT_INTERP, DT_NEEDED,
    DT_RPATH/DT_RUNPATH, DT_SONAME and the entry point. A file that is not
    an ELF yields the defaults, so the caller can still report its size.
    """
    try:
        with open(path, "rb") as f:
            b = f.read()
    except OSError:
        return {"size": 0, "e_type": 0, "interp": "", "needed": [],
                "rpath": [], "soname": "", "entry": 0}
    return elf_meta_bytes(b)


def elf_meta_bytes(b):
    """elf_full() over an in-memory image: a tarball member needs no file."""
    out = {"size": len(b), "e_type": 0, "interp": "", "needed": [],
           "rpath": [], "soname": "", "entry": 0}
    if b[:4] != b"\x7fELF" or len(b) < 64:
        return out
    is64 = b[4] == 2
    le = "<" if b[5] == 1 else ">"
    try:
        if is64:
            e_type, = struct.unpack_from(le + "H", b, 0x10)
            e_entry, = struct.unpack_from(le + "Q", b, 0x18)
            phoff, = struct.unpack_from(le + "Q", b, 0x20)
            shoff, = struct.unpack_from(le + "Q", b, 0x28)
            phentsize, phnum = struct.unpack_from(le + "HH", b, 0x36)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x3a)
        else:
            e_type, = struct.unpack_from(le + "H", b, 0x10)
            e_entry, = struct.unpack_from(le + "I", b, 0x18)
            phoff, = struct.unpack_from(le + "I", b, 0x1c)
            shoff, = struct.unpack_from(le + "I", b, 0x20)
            phentsize, phnum = struct.unpack_from(le + "HH", b, 0x2a)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x2e)
        out["e_type"] = e_type
        out["entry"] = e_entry
        for i in range(phnum):
            off = phoff + i * phentsize
            p_type, = struct.unpack_from(le + "I", b, off)
            if p_type != 3:
                continue
            if is64:
                p_offset, = struct.unpack_from(le + "Q", b, off + 8)
                p_filesz, = struct.unpack_from(le + "Q", b, off + 32)
            else:
                p_offset, = struct.unpack_from(le + "I", b, off + 4)
                p_filesz, = struct.unpack_from(le + "I", b, off + 16)
            out["interp"] = b[p_offset:p_offset + p_filesz].split(b"\0")[0].decode(
                "utf-8", "replace")
            break

        def sh(i):
            off = shoff + i * shentsize
            if is64:
                return struct.unpack_from(le + "IIQQQQIIQQ", b, off)
            return struct.unpack_from(le + "IIIIIIIIII", b, off)

        base = sh(shstrndx)[4]

        def name_at(off):
            return b[off:b.index(b"\0", off)].decode("utf-8", "replace")

        dyn = dstr = None
        for i in range(shnum):
            nm = name_at(base + sh(i)[0])
            if nm == ".dynamic":
                dyn = sh(i)
            elif nm == ".dynstr":
                dstr = sh(i)[4]
        if dyn is None or dstr is None:
            return out
        off, end = dyn[4], dyn[4] + dyn[5]
        esz = 16 if is64 else 8
        while off + esz <= end:
            if is64:
                tag, val = struct.unpack_from(le + "qQ", b, off)
            else:
                tag, val = struct.unpack_from(le + "iI", b, off)
            off += esz
            if tag == 0:
                break
            if tag not in (1, 14, 15, 29):
                continue
            v = name_at(dstr + val)
            if tag == 1:
                out["needed"].append(v)
            elif tag == 14:
                out["soname"] = v
            else:
                out["rpath"].append(v)
    except (ValueError, IndexError, struct.error):
        pass
    return out


def check_elf(path, kind, what):
    """Assert the file we just built is the kind of ELF we are going to ship.

    kind "static"  : e_type 2, no PT_INTERP, no DT_NEEDED
    kind "dynamic" : e_type 2, PT_INTERP, no DT_NEEDED on libVLC
    kind "loader"  : e_type 3, SONAME libc.musl-x86_64.so.1, entry point set
    """
    got = elf_full(path)
    if kind in ("static", "dynamic") and got["e_type"] != 2:
        raise RuntimeError("%s is not an executable ELF (e_type=%d)"
                           % (what, got["e_type"]))
    if kind == "static":
        if got["interp"]:
            raise RuntimeError("%s has PT_INTERP %s; a static launcher must not "
                               "depend on a loader the image does not ship"
                               % (what, got["interp"]))
        if got["needed"]:
            raise RuntimeError("%s has DT_NEEDED %s; a static launcher must not"
                               % (what, ", ".join(got["needed"])))
    elif kind == "dynamic":
        if not got["interp"]:
            raise RuntimeError("%s has no PT_INTERP: a static musl binary cannot "
                               "dlopen libVLC (its dlopen is a stub that says "
                               "'Dynamic loading not supported')" % what)
        if got["interp"] != "/lib/ld-musl-x86_64.so.1":
            raise RuntimeError("%s has PT_INTERP %r, expected "
                               "/lib/ld-musl-x86_64.so.1 (the loader the bundle "
                               "ships and the launcher execs)"
                               % (what, got["interp"]))
        if any("vlc" in n for n in got["needed"]):
            raise RuntimeError("%s is linked against libVLC (%s); it must load it "
                               "with dlopen so the package stays relocatable"
                               % (what, ", ".join(got["needed"])))
    elif kind == "loader":
        if got["e_type"] != 3:
            raise RuntimeError("%s is not a shared object (e_type=%d)"
                               % (what, got["e_type"]))
        if got["soname"] != "libc.musl-x86_64.so.1":
            raise RuntimeError("%s has SONAME %r, expected libc.musl-x86_64.so.1"
                               % (what, got["soname"]))
        if got["entry"] == 0:
            raise RuntimeError("%s has no entry point, so it cannot be exec'd"
                               % what)
    return got


def elf_dynsym_bytes(b):
    """(defined, undefined) symbol names from .dynsym, or (set(), set()).

    This is the second half of "the plugin loads": resolving DT_NEEDED says
    the loader can find libdav1d.so.7, but the plugin also imports ~40
    symbols from it (dav1d_open, dav1d_send_data, ...). If one of those is
    missing the loader fails at dlopen with "undefined symbol" and the file
    is as dead as if it were absent. So we read the real dynamic symbol
    table of every shipped object and require each strong undefined symbol to
    be defined by some library we ship (the loader itself counts - musl
    keeps libc's symbols in ld-musl-x86_64.so.1).

    STB_WEAK undefined symbols are skipped: they are allowed to be absent.
    """
    defined, undefined = set(), set()
    if b[:4] != b"\x7fELF" or len(b) < 64:
        return defined, undefined
    is64 = b[4] == 2
    le = "<" if b[5] == 1 else ">"
    try:
        if is64:
            shoff, = struct.unpack_from(le + "Q", b, 0x28)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x3a)
        else:
            shoff, = struct.unpack_from(le + "I", b, 0x20)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", b, 0x2e)

        def sh(i):
            off = shoff + i * shentsize
            if is64:
                return struct.unpack_from(le + "IIQQQQIIQQ", b, off)
            return struct.unpack_from(le + "IIIIIIIIII", b, off)

        def name_at(strtab, off):
            end = b.find(b"\0", strtab + off)
            return b[strtab + off:end].decode("utf-8", "replace") if end >= 0 else ""

        names = []
        for i in range(shnum):
            s = sh(i)
            names.append(name_at(sh(shstrndx)[4], s[0]))
        dynsym = dynstr = None
        for i, nm in enumerate(names):
            if nm == ".dynsym":
                dynsym = sh(i)
            elif nm == ".dynstr":
                dynstr = sh(i)
        if dynsym is None or dynstr is None:
            return defined, undefined
        strtab = dynstr[4]
        esz = 24 if is64 else 16
        count = dynsym[5] // esz
        for i in range(count):
            off = dynsym[4] + i * esz
            if is64:
                st_name, st_info, _other, st_shndx = struct.unpack_from(le + "IBBH", b, off)
            else:
                st_name, = struct.unpack_from(le + "I", b, off)
                st_info = b[off + 12]
                st_shndx, = struct.unpack_from(le + "H", b, off + 14)
            if st_name == 0:
                continue
            bind = st_info >> 4
            if bind not in (1, 2):            # STB_GLOBAL, STB_WEAK only
                continue
            nm = name_at(strtab, st_name)
            if not nm:
                continue
            if st_shndx == 0:                 # SHN_UNDEF
                if bind != 2:                 # a weak import may stay absent
                    undefined.add(nm)
            else:
                defined.add(nm)
    except (ValueError, IndexError, struct.error):
        pass
    return defined, undefined


def elf_dynsym(path):
    try:
        with open(path, "rb") as f:
            return elf_dynsym_bytes(f.read())
    except OSError:
        return set(), set()


def build_player(stage, zig):
    """Compile the DYNAMIC player, the one that talks to libVLC.

    -dynamic is not optional: zig's musl target defaults to a static link,
    and a static musl program's dlopen is a stub. The interpreter is named
    explicitly: the bundle ships it at lib/ld-musl-x86_64.so.1 and the jail
    has no /lib, so the path the kernel records has to be the one the loader
    is installed at, not whatever a host sysroot would suggest. RUNPATH
    $ORIGIN/../lib is the third way to find the closure (the launcher passes
    --library-path, wmsrv passes LD_LIBRARY_PATH), so a direct exec works.
    """
    out = os.path.join(stage, "libexec", "vlcplay")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-O2", "-dynamic",
           "-Wl,-rpath,$ORIGIN/../lib",
           "-Wl,-dynamic-linker=/lib/ld-musl-x86_64.so.1",
           "-Isrc", "-Isrc/client", "-o", out] + APP_SRC + ["-lm", "-pthread", "-ldl"]
    print("==> compiling the DYNAMIC player: %s" % out)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("zig cc failed with status %d" % r.returncode)
    info = check_elf(out, "dynamic", "the player")
    if not any("$ORIGIN" in rp for rp in info["rpath"]):
        raise RuntimeError("the player has no $ORIGIN RUNPATH (got %r); a direct "
                           "exec could not find <bundle>/lib" % (info["rpath"],))
    return out, info


def build_boot(stage, zig):
    """Compile the static launcher that becomes the package's exec = bin.

    -DVLCPLAY_BOOT selects section 0 of src/client/apps/vlcplay.c: a static
    program that execs <bundle>/lib/ld-musl-x86_64.so.1 with --library-path
    and <bundle>/libexec/vlcplay. It pulls in no SDK objects and no libVLC,
    which is why the source list below is one file.
    """
    out = os.path.join(stage, "bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os", "-static", "-s",
           "-DVLCPLAY_BOOT", "-Isrc", "-Isrc/client", "-o", out] + BOOT_SRC
    print("==> compiling the static launcher: %s" % out)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("zig cc failed with status %d" % r.returncode)
    info = check_elf(out, "static", "the launcher")
    return out, info


def build_image_stub(zig):
    """Compile the image placeholder into the stage bundle (id 'vlc').

    Same source, static, and honest when it runs: it cannot dlopen, so its
    window says libVLC is not installed and that `pkg install vlc` brings
    the dynamic player and the closure. The image must not ship the 100+ MB
    closure; it must ship the truth.
    """
    out = os.path.join(IMAGE_BUNDLE, "bin")
    os.makedirs(IMAGE_BUNDLE, exist_ok=True)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os", "-static", "-s",
           "-Isrc", "-Isrc/client", "-o", out] + APP_SRC + ["-lm", "-pthread"]
    print("==> compiling the image placeholder: %s" % out)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("zig cc failed with status %d" % r.returncode)
    info = check_elf(out, "static", "the image placeholder")
    return out, info


def loader_from_apk(pkgs, stage):
    """Copy the musl dynamic loader out of Alpine's musl package.

    The player is dynamic, so something must be its interpreter. The image
    ships none and the jail has no /lib, so the bundle carries its own,
    executable, and the launcher execs that path.
    """
    if LOADER_PKG not in pkgs:
        raise RuntimeError("the index has no %r package; cannot ship a loader"
                           % LOADER_PKG)
    path = get_apk(pkgs, LOADER_PKG, True)
    if not path:
        raise RuntimeError("could not fetch %s" % pkgs[LOADER_PKG]["file"])
    probe = os.path.join(APK_CACHE, "_loader")
    if os.path.isdir(probe):
        shutil.rmtree(probe)
    unpack_apk(path, probe, members={LOADER_IN_APK})
    src = os.path.join(probe, LOADER_IN_APK)
    if not os.path.isfile(src):
        raise RuntimeError("%s does not contain %s" % (path, LOADER_IN_APK))
    info = check_elf(src, "loader", LOADER_IN_APK)
    dst = os.path.join(stage, LOADER_IN_PKG)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    os.chmod(dst, 0o755)
    return dst, info, sha256_file(src), pkgs[LOADER_PKG]["version"]


# ---------------------------------------------------------------------------
# apk fetch / unpack
# ---------------------------------------------------------------------------
def apk_path(pkgs, name):
    return os.path.join(APK_CACHE, pkgs[name]["file"])


def get_apk(pkgs, name, download):
    """Path to a cached .apk, downloading it first if allowed. None on failure."""
    info = pkgs[name]
    path = apk_path(pkgs, name)
    if os.path.exists(path) and os.path.getsize(path) == info["download"]:
        return path
    if not download:
        return None
    url = "%s/%s/%s/%s" % (ALPINE, info["repo"], ARCH, info["file"])
    try:
        fetch(url, path)
    except Exception as e:                                   # noqa: BLE001
        print("    FAILED %s: %s" % (info["file"], e))
        return None
    return path


def unpack_apk(path, dest, members=None):
    """Extract an .apk to dest. Apks are tar.gz plus a signature.

    Windows cannot create the symlinks an apk contains, so links are recorded
    and materialised as copies afterwards - same approach as fetch-browser.py.
    Returns (files_written, links_fixed).
    """
    links, written = [], 0
    try:
        tf = tarfile.open(path, "r:gz")
    except Exception:                                        # noqa: BLE001
        return 0, 0
    for m in tf:
        if m.name.startswith(".SIGN") or m.name.startswith(".") and "/" not in m.name:
            continue
        if members is not None and m.name not in members:
            continue
        target = os.path.join(dest, m.name)
        if m.isdir():
            os.makedirs(target, exist_ok=True)
        elif m.issym() or m.islnk():
            links.append((target, m.linkname))
        elif m.isfile():
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(tf.extractfile(m).read())
            if m.mode & 0o111:
                try:
                    os.chmod(target, 0o755)
                except OSError:
                    pass
            written += 1
    tf.close()

    fixed = 0
    for target, linkname in links:
        src = linkname if os.path.isabs(linkname) else \
            os.path.join(os.path.dirname(target), linkname)
        for cand in (os.path.normpath(src), os.path.join(dest, linkname.lstrip("/"))):
            if os.path.isfile(cand):
                os.makedirs(os.path.dirname(target), exist_ok=True)
                try:
                    with open(cand, "rb") as f:
                        data = f.read()
                    with open(target, "wb") as f:
                        f.write(data)
                    fixed += 1
                except OSError:
                    pass
                break
    return written, fixed


def apk_stats(path, prefix=None):
    """(file count, total file bytes) inside an .apk, optionally under prefix."""
    try:
        tf = tarfile.open(path, "r:gz")
    except Exception:                                        # noqa: BLE001
        return 0, 0
    n = total = 0
    for m in tf:
        if not m.isfile():
            continue
        if prefix is not None and not m.name.startswith(prefix):
            continue
        n += 1
        total += m.size
    tf.close()
    return n, total


def tree_bytes(dest):
    total, files = 0, 0
    for root, _dirs, names in os.walk(dest):
        for n in names:
            try:
                total += os.path.getsize(os.path.join(root, n))
                files += 1
            except OSError:
                pass
    return total, files


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stamp_path_for(repo):
    """The package stamp that describes a given --repo.

    The default repo owns build/vlc-pkg.stamp; any other repo gets its own, so
    a second artifact (e.g. --profile subs into build/repo-subs) can be
    provenance-checked too instead of silently skipping the gate.
    """
    same = (os.path.normcase(os.path.realpath(repo))
            == os.path.normcase(os.path.realpath(REPO)))
    if same:
        return os.path.join("build", "vlc-pkg.stamp")
    return os.path.join("build", "vlc-pkg-%s.stamp"
                        % os.path.basename(os.path.normpath(repo)))


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json_atomic(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# the closure tree
# ---------------------------------------------------------------------------
def root_signature(profile, closure, pkgs, plugins):
    return {
        "stamp": STAMP_VERSION,
        "profile": profile,
        "packages": [[n, pkgs[n]["version"]] for n in sorted(closure) if n in pkgs],
        "plugins": sorted(plugins) if plugins else ["*"],
    }


def tree_is_under_build(path):
    """Refuse to rmtree a directory that is not build/'s child."""
    root = os.path.realpath("build")
    real = os.path.realpath(path)
    return real == root or real.startswith(root + os.sep)


def build_root(pkgs, closure, dest, profile, plugins, stamp, force=False):
    """Download (if needed) and unpack the closure into dest.

    Idempotent: if dest carries the same signature stamp the whole step is a
    cache hit and no network call is made. The tree is built beside dest and
    renamed, so a crash cannot leave a half-populated root.
    """
    if not tree_is_under_build(dest):
        print("error: --dest must be inside build/ (got %s)" % dest, file=sys.stderr)
        return False
    sig = hashlib.sha256(json.dumps(stamp, sort_keys=True).encode()).hexdigest()
    old = read_json(os.path.join(dest, ".vlc-stamp"))
    if (not force and old and old.get("sig") == sig
            and os.path.isfile(os.path.join(dest, "usr", "lib", "libvlc.so.5"))):
        print("==> root cache hit: %s is up to date for profile %r"
              % (dest, profile))
        b, n = tree_bytes(dest)
        print("    %d files, %.2f MB on disk" % (n, b / 1e6))
        return True

    # everything except the base image; vlc/vlc-libs are taken selectively
    # for a trimmed profile and whole for the full one.
    wanted = set(LIBVLC_FILES + ["usr/lib/vlc/plugins/" + p for p in plugins]) \
        if plugins else None
    if plugins is None:
        todo = [n for n in sorted(closure)
                if n in pkgs and n not in BASE_PACKAGES and n != "vlc-libs"]
        need = ["vlc-libs"] if "vlc-libs" in pkgs else []
    else:
        todo = [n for n in sorted(closure)
                if n in pkgs and n not in BASE_PACKAGES
                and n not in ("vlc", "vlc-libs")]
        need = [n for n in ("vlc-libs", "vlc") if n in pkgs]

    got, size, files = 0, 0, []
    for name in todo + need:
        path = get_apk(pkgs, name, True)
        if not path:
            print("error: could not fetch %s" % name, file=sys.stderr)
            return False
        got += 1
        size += os.path.getsize(path)
        files.append((name, path))
        if got % 25 == 0:
            print("    %d/%d packages, %.1f MB" % (got, len(todo) + len(need), size / 1e6))
    print("==> downloaded %d packages, %.1f MB" % (got, size / 1e6))

    tmp = dest + ".new"
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp, exist_ok=True)

    written = fixed = 0
    if wanted is not None:
        # only the plugin files and libVLC itself out of the two vlc packages
        for need_name in need:
            w, f = unpack_apk(apk_path(pkgs, need_name), tmp, members=wanted)
            written += w
            fixed += f
    for name, path in files:
        if name in need:
            continue
        if name == "vlc" and wanted is None:
            # full profile: take the whole package, plugins and all
            pass
        w, f = unpack_apk(path, tmp)
        written += w
        fixed += f

    b, n = tree_bytes(tmp)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.replace(tmp, dest)
    out = dict(stamp)
    out["sig"] = sig
    out["files"] = n
    out["bytes"] = b
    write_json_atomic(os.path.join(dest, ".vlc-stamp"), out)
    print("==> extracted to %s: %d files, %.2f MB on disk, %d symlinks copied"
          % (dest, n, b / 1e6, fixed))
    return True


# ---------------------------------------------------------------------------
def find_zig():
    z = os.environ.get("ZIG")
    if z:
        return z
    z = shutil.which("zig") or shutil.which("zig.exe")
    if z:
        return z
    for cand in glob.glob(os.path.expanduser(
            "~/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/"
            "zig-x86_64-windows-*/zig.exe")):
        if os.path.isfile(cand):
            return cand
    return None


def app_inputs_digest(zig):
    h = hashlib.sha256()
    for p in APP_SRC:
        with open(p, "rb") as f:
            h.update(p.encode() + b"\0" + f.read() + b"\0")
    if zig:
        try:
            h.update(subprocess.check_output([zig, "version"]).strip())
        except (OSError, subprocess.CalledProcessError):
            pass
    h.update(open(MANIFEST, "rb").read())
    # The packaging logic itself: a cached tarball must not survive a change
    # to what build_stage puts in it (the soname aliases were one such fix).
    with open(os.path.abspath(__file__), "rb") as f:
        h.update(b"tool\0" + f.read() + b"\0")
    return h.hexdigest()


def collect_libs(root):
    """Every shared object under usr/lib, except the plugin subtree.

    Returns {basename: absolute path}. A duplicate basename with different
    bytes is a hard error: flattening two libraries into one name would
    silently give one of them the other's DT_NEEDED.
    """
    libdir = os.path.join(root, "usr", "lib")
    out = {}
    for dirpath, _dirs, names in os.walk(libdir):
        rel = os.path.relpath(dirpath, libdir)
        if rel.split(os.sep)[0] == "vlc":
            continue
        for n in names:
            if ".so" not in n:
                continue
            src = os.path.join(dirpath, n)
            if not os.path.isfile(src):
                continue
            if n in out:
                if sha256_file(src) != sha256_file(out[n]):
                    raise RuntimeError("two libraries named %s with different "
                                       "contents: %s and %s" % (n, out[n], src))
                continue
            out[n] = src
    return out


def soname_aliases(libs):
    """alias -> real basename for every DT_SONAME that is not a file yet.

    Alpine's packages carry libvlc.so.5 -> libvlc.so.5.6.1. A trimmed profile
    copies only the versioned file out of the apk, so without this the bundle
    would contain libvlc.so.5.6.1 and no libvlc.so.5 at all: the loader would
    start the player, dlopen("libvlc.so.5") would fail, and the player would
    print "cannot load libvlc.so.5" - the exact defect this task exists to
    remove, one layer deeper. The alias is what the dynamic loader looks for,
    so it is not optional.
    """
    out = {}
    for name, path in libs.items():
        soname = elf_full(path)["soname"]
        if soname and soname != name and soname not in libs:
            out[soname] = name
    return out


def check_closure(libs, aliases, extra):
    """Every DT_NEEDED that is not musl's own libc must resolve inside lib/.

    This is the check that catches "the closure is present but the loader
    cannot find a name". extra is a list of (label, path) for plugins and the
    player, so their NEEDED entries are covered too.
    """
    have = set(libs) | set(aliases)
    missing = {}
    for name, path in list(libs.items()) + list(extra):
        for need in elf_full(path)["needed"]:
            if need == "libc.so" or need.startswith("libc."):
                continue           # musl's loader resolves these to itself
            if need not in have:
                missing.setdefault(need, []).append(name)
    if missing:
        raise RuntimeError(
            "the bundle would be missing DT_NEEDED libraries: %s"
            % ", ".join("%s (needed by %s)" % (k, ", ".join(sorted(v)[:3]))
                        for k, v in sorted(missing.items())))
    return len(have)


def plan_symlinks(libs):
    """name -> target for identical copies that differ only by version suffix.

    Our unpacker materialises Alpine symlinks as copies (Windows cannot make
    them), so libvlc.so.5 and libvlc.so.5.6.1 both exist with the same bytes.
    The package stores the longest name once and links the others to it, or
    the install would be twice the size it needs to be.
    """
    links = {}
    by_size = {}
    for name, path in libs.items():
        by_size.setdefault(os.path.getsize(path), []).append(name)
    for size, names in by_size.items():
        if len(names) < 2 or size == 0:
            continue
        groups = {}
        for n in names:
            groups.setdefault(sha256_file(libs[n]), []).append(n)
        for _sha, group in groups.items():
            if len(group) < 2:
                continue
            group.sort(key=lambda x: (-len(x), x))
            target = group[0]
            for n in group[1:]:
                # only a real soname/version pair, not two unrelated libs
                if target.startswith(n + "."):
                    links[n] = target
    return links


def plugin_list(root, profile, plugins):
    if plugins is not None:
        return sorted(plugins)
    base = os.path.join(root, "usr", "lib", "vlc", "plugins")
    out = []
    for dirpath, _dirs, names in os.walk(base):
        for n in names:
            if n.endswith(".so"):
                out.append(os.path.relpath(os.path.join(dirpath, n), base))
    return sorted(out)


def apk_plugin_index(pkgs):
    """{plugin rel path: size} for every plugin file inside vlc.apk.

    Read from the cached .apk; the caller has already fetched it. This is the
    only honest source for "which codec plugins does Alpine's VLC have":
    APKINDEX has no file list, and docs that list plugins from memory have
    been wrong before (there is no libav1_plugin.so in VLC 3.0.23).
    """
    path = apk_path(pkgs, "vlc")
    out = {}
    try:
        tf = tarfile.open(path, "r:gz")
    except (OSError, tarfile.TarError):
        return out
    prefix = "usr/lib/vlc/plugins/"
    for m in tf:
        if m.isfile() and m.name.startswith(prefix):
            out[m.name[len(prefix):]] = m.size
    tf.close()
    return out


# The plugins this assignment is about, in the words it asked for. `why` is
# printed next to the verdict so the answer cannot drift from the check.
CODEC_WATCHLIST = [
    ("codec/libdav1d_plugin.so", "AV1 (the only AV1 decoder VLC 3.0 has)"),
    ("codec/libav1_plugin.so", "AV1 via a plugin by that name"),
    ("codec/libvpx_plugin.so", "VP8 + VP9"),
    ("codec/libtheora_plugin.so", "Theora"),
    ("codec/libmpeg2_plugin.so", "MPEG-1/2 video"),
    ("codec/libavcodec_plugin.so", "everything else (H.264/HEVC/MPEG-2/VC-1)"),
    ("packetizer/libpacketizer_av1_plugin.so", "AV1 OBU packetizer"),
    ("demux/libsubtitle_plugin.so", "sidecar .srt/.ass/.vtt"),
    ("codec/libsubsdec_plugin.so", "SubRip/SSA -> SPU"),
    ("codec/libwebvtt_plugin.so", "WebVTT -> SPU"),
    ("codec/liblibass_plugin.so", "ASS/SSA rendering via libass"),
    ("text_renderer/libfreetype_plugin.so", "draws text subpictures"),
]


def apk_plugin_facts(pkgs, wanted):
    """{rel: (size, [DT_NEEDED])} read from vlc.apk for the wanted files.

    Read from the package, not from the probe tree: --audit has to be able to
    describe a plugin the profile does not take (liblibass, libfreetype), and
    a file that is not extracted has no DT_NEEDED to read otherwise.
    """
    out = {}
    want = set(wanted)
    try:
        tf = tarfile.open(apk_path(pkgs, "vlc"), "r:gz")
    except (OSError, tarfile.TarError):
        return out
    prefix = "usr/lib/vlc/plugins/"
    for m in tf:
        if not m.isfile() or not m.name.startswith(prefix):
            continue
        rel = m.name[len(prefix):]
        if rel not in want:
            continue
        meta = elf_meta_bytes(tf.extractfile(m).read())
        out[rel] = (m.size, meta["needed"])
    tf.close()
    return out


def audit_plugins(pkgs, provides, closure, selected, profile):
    """Print what vlc.apk has, what this profile takes, and what is absent.

    Two independent columns, deliberately kept apart:
      [apk]  the plugin file exists in vlc.apk, its size, and its DT_NEEDED
             read from the file itself;
      [pkg]  each needed soname resolves to a package that is in this
             profile's closure.
    A plugin that exists but needs a library nobody ships is not coverage,
    so the verdict is SHIPPED only when both columns agree. Resolution is
    judged against the closure this run would actually install, never
    against build/vlc-root, which may be left over from another profile.
    The stricter proof (undefined symbols, packed tarball) is
    verify_tarball().
    """
    files = apk_plugin_index(pkgs)
    if not files:
        print("==> plugin audit: vlc.apk is not readable, skipping")
        return None
    facts = apk_plugin_facts(pkgs, [rel for rel, _why in CODEC_WATCHLIST])
    have_so = set()
    for so, provider in provides.items():
        if provider in closure:
            have_so.add(so.split("=")[0])
    # --profile full ships the whole package, so every file is selected.
    sel = set(files) if selected is None else set(selected)

    print("==> plugin audit: vlc-%s.apk carries %d plugin files; profile %r takes %d"
          % (pkgs["vlc"]["version"], len(files), profile, len(sel)))
    print("    codec watchlist (shipped = the file is taken AND its libraries are):")
    print("    %-44s %-9s %-11s %s" % ("plugin", "in apk", "in profile", "libraries"))
    counts = {"shipped": 0, "absent": 0, "unresolved": 0, "not_selected": 0}
    for rel, why in CODEC_WATCHLIST:
        size = files.get(rel)
        if size is None:
            print("    %-44s %-9s %-11s %s" % (rel, "ABSENT", "-", why))
            counts["absent"] += 1
            continue
        _size, needed = facts.get(rel, (size, []))
        needs = [n for n in needed
                 if not n.startswith("libc.") and n != "libc.so"]
        bad = [n for n in needs
               if n != "libvlccore.so.9" and ("so:" + n) not in have_so]
        chosen = rel in sel
        if bad and chosen:
            verdict = "MISSING"
            counts["unresolved"] += 1
        elif bad:
            verdict = "not selected"
            counts["not_selected"] += 1
        elif chosen:
            verdict = "SHIPPED"
            counts["shipped"] += 1
        else:
            verdict = "not selected"
            counts["not_selected"] += 1
        libs = ", ".join(needs) or "(libvlccore only)"
        if bad:
            libs += "  (not shipped: %s)" % ", ".join(bad)
        print("    %-44s %6.1f KB %-11s %s" % (rel, size / 1024.0, verdict, libs))

    # Completeness, measured rather than asserted: every codec/ and
    # packetizer/ plugin whose DT_NEEDED already resolves inside this closure
    # but which the profile does not take. Anything in this list is a file we
    # could ship for free; the doc says why each one is not worth it.
    scope = [rel for rel in files if rel.startswith(("codec/", "packetizer/"))]
    all_facts = apk_plugin_facts(pkgs, scope)
    free_not_taken = []
    for rel, (_size, needed) in sorted(all_facts.items()):
        if rel in sel:
            continue
        bad = [n for n in needed
               if n != "libvlccore.so.9" and not n.startswith("libc.")
               and ("so:" + n) not in have_so]
        if not bad:
            free_not_taken.append(rel)
    counts["free_not_taken"] = len(free_not_taken)

    by_dir = {}
    for rel in files:
        top = rel.split("/")[0]
        by_dir[top] = by_dir.get(top, 0) + 1
    print("    by category in vlc.apk: %s"
          % ", ".join("%s %d" % (k, v) for k, v in sorted(by_dir.items())))
    print("    verdict: %d shipped, %d present but not in this profile, %d absent from "
          "VLC 3.0.23, %d selected but unresolvable"
          % (counts["shipped"], counts["not_selected"], counts["absent"],
             counts["unresolved"]))
    print("    codec/packetizer plugins that would load against this closure but "
          "the profile does not take: %d (%s)"
          % (len(free_not_taken),
             ", ".join(r.split("/", 1)[1].replace("_plugin.so", "")
                       for r in free_not_taken)))
    return {"apk_plugins": len(files), "selected": len(sel),
            "by_category": by_dir, "verdict": counts,
            "free_not_taken": free_not_taken}


def package_manifest(text):
    """The manifest pkg installs: the image file with 'placeholder = no'.

    The image bundle is a placeholder - only bin, no libVLC - and says so in
    its own manifest. The package carries the real closure, so the one line
    that describes the payload is the one line that differs. pkg copies the
    manifest it unpacks, so /root/apps/vlc/manifest ends up saying 'no' and
    src/apps/store.c can tell the placeholder from the installed package by
    what is on the disk, not by a guess about the directory name.
    """
    out = []
    seen = 0
    for line in text.splitlines():
        key = line.split("=", 1)[0].strip() if "=" in line else ""
        if key == "placeholder":
            out.append("placeholder = no")
            seen += 1
        else:
            out.append(line)
    if seen != 1:
        raise RuntimeError("the bundle manifest must have exactly one "
                           "'placeholder =' line (the image copy is yes); "
                           "found %d" % seen)
    return "\n".join(out) + "\n"


def check_manifest(path):
    """The bundle manifest is the one place the id lives; check it here.

    Returns the file's text so the package ships what the image has (plus
    the one payload line package_manifest() rewrites). A typo in any of
    these lines is a tile that cannot start, so it is a hard error, not a
    warning. 'version' must equal PKG_VERSION: the index row built below is
    what src/apps/store.c compares against, and two numbers that disagree
    would make an installed package look permanently outdated.
    """
    if not os.path.isfile(path):
        raise RuntimeError("%s is missing: the bundle manifest is the source of "
                           "truth for the id, the exec and the jail flags" % path)
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    fields = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        fields[k.strip()] = v.strip()
    need = {"name": None, "desc": None, "exec": "bin",
            "version": PKG_VERSION, "network": "host", "audio": "yes",
            "bundle": PKG_NAME, "placeholder": "yes"}
    for k, want in need.items():
        if k not in fields:
            raise RuntimeError("%s has no %r line" % (path, k))
        if want is not None and fields[k] != want:
            raise RuntimeError("%s has %s = %r, expected %r"
                               % (path, k, fields[k], want))
    return text


def build_stage(root, stage, profile, plugins, app_digest, zig, pkgs):
    """Create build/vlc-pkg and the tar entry list."""
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "libexec"), exist_ok=True)
    os.makedirs(os.path.join(stage, "lib"), exist_ok=True)

    manifest = package_manifest(check_manifest(MANIFEST))
    with open(os.path.join(stage, "manifest"), "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest)

    boot, boot_info = build_boot(stage, zig)

    # The launcher execs <bundle>/<exec>, and src/wmsrv.c refuses any exec
    # that is not a bare file name. Both halves are checked here so a broken
    # manifest can never reach a tarball: a '/' means the desktop will not
    # launch it, a missing file means the kernel cannot.
    exec_name = ""
    for line in manifest.splitlines():
        key, _, val = line.partition("=")
        if key.strip() == "exec":
            exec_name = val.strip()
    if not exec_name or "/" in exec_name or ".." in exec_name:
        raise RuntimeError("'exec = %s' is not a bare file name; src/wmsrv.c\n"
                           "refuses any exec containing '/' or '..'" % exec_name)
    if not os.path.isfile(os.path.join(stage, exec_name)):
        raise RuntimeError("the manifest's exec = %s is not in the bundle "
                           "stage; the tile would be dead" % exec_name)
    player, player_info = build_player(stage, zig)
    loader, loader_info, loader_sha, loader_ver = loader_from_apk(pkgs, stage)

    libs = collect_libs(root)
    links = plan_symlinks(libs)
    aliases = soname_aliases(libs)
    entries = []
    entries.append(("manifest", "file", os.path.join(stage, "manifest"), 0o644, ""))
    entries.append(("bin", "file", boot, 0o755, ""))
    entries.append(("libexec/vlcplay", "file", player, 0o755, ""))
    # The loader is the only library that must be executable. Its mode in
    # the tar is what busybox tar restores as root, so 0755 has to be here.
    entries.append((LOADER_IN_PKG, "file", loader, 0o755, ""))
    for name in sorted(libs):
        dst = os.path.join(stage, "lib", name)
        if name in links:
            entries.append(("lib/" + name, "symlink", None, 0o777, links[name]))
        else:
            shutil.copyfile(libs[name], dst)
            os.chmod(dst, 0o644)
            entries.append(("lib/" + name, "file", dst, 0o644, ""))

    # The soname aliases (libvlc.so.5, libvlccore.so.9, ...) are the names
    # dlopen() and DT_NEEDED use; the versioned files alone are not enough.
    for name in sorted(aliases):
        entries.append(("lib/" + name, "symlink", None, 0o777, aliases[name]))

    plist = plugin_list(root, profile, plugins)
    base = os.path.join(root, "usr", "lib", "vlc", "plugins")
    for rel in plist:
        src = os.path.join(base, rel)
        if not os.path.isfile(src):
            raise RuntimeError("missing selected plugin: %s" % rel)
        dst = os.path.join(stage, "lib", "vlc", "plugins", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o644)
        entries.append(("lib/vlc/plugins/" + rel, "file", dst, 0o644, ""))

    # Fail before packing if a name the loader will ask for is not in lib/.
    closure_names = check_closure(
        libs, aliases,
        [("libexec/vlcplay", player)] +
        [("plugin:" + rel, os.path.join(base, rel)) for rel in plist])

    # directories, so an empty lib/vlc tree still exists after unpack
    dirs = set()
    for arc, _kind, _src, _mode, _ln in entries:
        parts = arc.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))
    for d in sorted(dirs):
        entries.append((d, "dir", None, 0o755, ""))
    entries.sort(key=lambda e: (e[0], e[1]))

    libvlc_path = libs.get("libvlc.so.5") or libs.get("libvlc.so.5.6.1")
    libvlc = elf_full(libvlc_path) if libvlc_path else {}
    info = {
        "exec": exec_name,
        "version": PKG_VERSION,
        "profile": profile,
        "app_digest": app_digest,
        "plugins": plist,
        "player_size": player_info["size"],
        "player_interp": player_info["interp"],
        "player_needed": player_info["needed"],
        "player_rpath": player_info["rpath"],
        "boot_size": boot_info["size"],
        "loader_size": loader_info["size"],
        "loader_sha256": loader_sha,
        "loader_version": loader_ver,
        "libvlc_soname": libvlc.get("soname", ""),
        "libs": len(libs),
        "soname_aliases": len(aliases),
        "closure_names": closure_names,
    }
    write_json_atomic(os.path.join(stage, ".vlc-pkg.json"), info)
    return entries, info


def pack_tarball(entries, stage, out_path):
    """Deterministic gzip tar: sorted entries, mtime 0, uid/gid 0, no names."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        root = tarfile.TarInfo("./")
        root.type = tarfile.DIRTYPE
        root.mode = 0o755
        root.mtime = 0
        tf.addfile(root)
        for arc, kind, src, mode, link in entries:
            ti = tarfile.TarInfo("./" + arc)
            ti.mtime = 0
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            ti.mode = mode
            if kind == "dir":
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            elif kind == "symlink":
                ti.type = tarfile.SYMTYPE
                ti.linkname = link
                tf.addfile(ti)
            else:
                ti.type = tarfile.REGTYPE
                ti.size = os.path.getsize(src)
                with open(src, "rb") as f:
                    tf.addfile(ti, f)
    data = gzip.compress(buf.getvalue(), compresslevel=6, mtime=0)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    tmp = out_path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, out_path)
    return len(data)


# What the packed player dlopens by name (src/client/apps/vlcplay.c:
# dlopen("libvlc.so.5", RTLD_NOW)). A DT_NEEDED scan cannot see this, and a
# bundle that ships only libvlc.so.5.6.1 with the DT_SONAME libvlc.so.5 is a
# bundle that fails at runtime - the loader opens files by name, it does not
# search a directory for a SONAME. Keep this list in step with that call.
DLOPEN_LIBS = ["libvlc.so.5"]


def verify_bundle(files, aliases, label, verbose=True, dlopen=None):
    """Prove the packed bytes load, by name and by symbol.

    files   {arcname: bytes} for every regular file under lib/ plus the
            player (libexec/vlcplay);
    aliases {alias: target} for the soname symlinks the tar carries.

    The rules, each of which has a failing example behind it:
      1. every DT_NEEDED of every shipped object (libraries, loader, player,
         plugins) must have an *entry with exactly that name* under lib/ -
         a regular file or a symlink. Matching a DT_SONAME somewhere is not
         enough: musl's loader opens libvpx.so.11, it does not scan for a
         file whose SONAME is libvpx.so.11 (so libvpx.so.11.0.1 alone is a
         dead bundle, and this check must reject it);
      2. every alias target must exist, and alias chains must not loop;
      3. every name in DLOPEN_LIBS must have an entry (the player dlopens
         libvlc.so.5, which is not in anyone's DT_NEEDED);
      4. the player's PT_INTERP basename must be shipped at lib/;
      5. every strong undefined symbol of each plugin and of the player must
         be defined by some shipped library or by the bundled musl loader
         (which is the C library here). Weak imports may stay absent.

    Returns the summary dict; raises RuntimeError listing every failure, so a
    build that would ship a dead plugin cannot finish successfully.
    """
    dlopen = DLOPEN_LIBS if dlopen is None else dlopen
    # Libraries and the loader - not the plugins: a plugin that exports a name
    # must not be able to satisfy another plugin's import, and nothing loads a
    # plugin by DT_NEEDED name.
    lib_files = {a: d for a, d in files.items()
                 if a.startswith("lib/") and "/vlc/plugins/" not in a}
    entries = {os.path.basename(a) for a in lib_files} | set(aliases)

    problems = []
    # (2) every symlink must land on a real entry, without a cycle.
    for a, t in sorted(aliases.items()):
        cur, seen = t, {a}
        while cur in aliases:
            if cur in seen:
                problems.append("lib/%s: soname symlink loop (%s)"
                                % (a, " -> ".join(sorted(seen | {cur}))))
                cur = None
                break
            seen.add(cur)
            cur = aliases[cur]
        if cur is not None and cur not in entries:
            problems.append("lib/%s -> %s (target is not in the bundle)"
                            % (a, t))

    # (3) dlopen targets, and (4) the interpreter.
    for name in dlopen:
        if name not in entries:
            problems.append("the player dlopens %s but lib/%s is not in the "
                            "bundle (only a file with that DT_SONAME would "
                            "not help: the loader opens by name)" % (name, name))
    player = files.get("libexec/vlcplay")
    if player is not None:
        interp = elf_meta_bytes(player)["interp"]
        if interp:
            base = os.path.basename(interp)
            if base not in entries:
                problems.append("the player's PT_INTERP is %s but lib/%s is not "
                                "in the bundle" % (interp, base))

    # (1) DT_NEEDED of every object, by exact name.
    needed_missing = {}
    for arc in sorted(files):
        for n in elf_meta_bytes(files[arc])["needed"]:
            if n == "libc.so" or n.startswith("libc."):
                continue        # musl resolves these to the loader itself
            if n not in entries:
                needed_missing.setdefault(arc, []).append(n)

    # (5) symbols: plugins and the player import into the shipped libraries.
    defined = set()
    for arc, data in lib_files.items():
        d, _u = elf_dynsym_bytes(data)
        defined |= d

    sym_missing = {}
    rows = []
    for arc in sorted(files):
        if not ("vlc/plugins/" in arc or arc.endswith("libexec/vlcplay")):
            continue
        _d, undef = elf_dynsym_bytes(files[arc])
        miss = sorted(undef - defined)
        if miss:
            sym_missing[arc] = miss
        rows.append((arc.replace("lib/vlc/plugins/", ""),
                     len(elf_meta_bytes(files[arc])["needed"]), len(undef),
                     bool(needed_missing.get(arc)), bool(miss)))

    if verbose:
        print("==> verifying %s: %d plugin/player objects + %d lib files, "
              "%d soname aliases, %d dlopen target(s)"
              % (label, len(rows), len(lib_files), len(aliases), len(dlopen)))
        for arc, nneed, nundef, prev_bad, syms_bad in rows:
            print("    %-46s NEEDED %2d  imports %3d  %s"
                  % (arc, nneed, nundef,
                     "FAIL" if (prev_bad or syms_bad) else "resolve"))

    for arc, miss in needed_missing.items():
        problems.append("%s needs %s (no entry with that name under lib/)"
                        % (arc, ", ".join(miss)))
    for arc, miss in sym_missing.items():
        problems.append("%s imports %d symbol(s) no shipped library defines: %s"
                        % (arc, len(miss), ", ".join(miss[:8])))
    summary = {"objects": len(rows), "libraries": len(lib_files),
               "aliases": len(aliases), "dlopen": list(dlopen),
               "needed_objects": len(files),
               "needed_failures": len(needed_missing),
               "symbol_failures": len(sym_missing),
               "undefined_symbols": sum(r[2] for r in rows)}
    if problems:
        sys.stdout.flush()          # keep the failure next to its table
        msg = "%s would not load:" % label
        for p in problems:
            msg += os.linesep + "  " + p
        raise RuntimeError(msg)
    if verbose:
        print("    all %d objects have an entry for every DT_NEEDED and all %d "
              "plugin/player imports resolve" % (len(files), len(rows)))
    return summary


def read_packed_bundle(path):
    """{arcname: bytes} + {alias: target} from the tarball itself.

    Symlinks are not extracted (Windows cannot create them); their targets are
    recorded and the soname map is built from the real files' DT_SONAME, so an
    alias whose target is missing still fails the check.
    """
    files, aliases = {}, {}
    tf = tarfile.open(path, "r:gz")
    for m in tf:
        arc = m.name[2:] if m.name.startswith("./") else m.name
        if m.issym():
            if arc.startswith("lib/") and \
                    ("vlc/plugins/" not in arc):
                aliases[os.path.basename(arc)] = os.path.basename(m.linkname)
            continue
        if not m.isfile():
            continue
        keep = (arc.startswith("lib/") or arc == "libexec/vlcplay")
        if not keep or (".so" not in arc and arc != "libexec/vlcplay"):
            continue
        files[arc] = tf.extractfile(m).read()
    tf.close()
    return files, aliases


def packed_player_sha(path):
    """sha256 of libexec/vlcplay inside a package, or None."""
    try:
        tf = tarfile.open(path, "r:gz")
    except (OSError, tarfile.TarError):
        return None
    try:
        for m in tf:
            if m.name == "./libexec/vlcplay" and m.isfile():
                return hashlib.sha256(tf.extractfile(m).read()).hexdigest()
    finally:
        tf.close()
    return None


def verify_tarball(path, verbose=True):
    """verify_bundle() over the packed artifact, not the loose tree."""
    files, aliases = read_packed_bundle(path)
    if not files:
        raise RuntimeError("%s contains no libraries: nothing to verify" % path)
    return verify_bundle(files, aliases, os.path.basename(path), verbose)


def upsert_index(tarball, repo):
    """Put the vlc row into <repo>/index.tsv and re-sign it if we can.

    `repo` is the --repo that was built, not the module default: building
    --profile subs --repo build/repo-subs must not rewrite build/repo's index
    (it did once, and the scratch repo was left pointing at an old tarball's
    size and hash). Rows whose tarball is not in that repo are dropped, so a
    scratch repo gets an index that describes only what it actually contains
    instead of dangling entries.

    tools/serve-repo.sh rebuilds index.tsv from stage/usr/share/mystic/apps,
    so a later run of that script will drop this row; the tarball stays. The
    final report says so instead of implying the repository is permanent.
    """
    NL = chr(10)
    TAB = chr(9)
    os.makedirs(repo, exist_ok=True)
    idx = os.path.join(repo, "index.tsv")
    rows = []
    if os.path.isfile(idx):
        with open(idx, "r", encoding="utf-8", newline="") as f:
            for line in f.read().splitlines():
                if not line.strip():
                    continue
                fields = line.split(TAB)
                if fields[0] == PKG_NAME:
                    continue
                fname = fields[4] if len(fields) > 4 else ""
                if fname and not os.path.isfile(os.path.join(repo, fname)):
                    continue          # a row with no tarball in this repo
                rows.append(line)
    size = os.path.getsize(tarball)
    sha = sha256_file(tarball)
    # The tile text is a capability claim the user reads before installing.
    # H.264 has been watched; the rest is shipped and statically proven to
    # load, not measured, so it must not read as "real ... decode" for all.
    desc = ("libVLC video player: vmem I420 output; H.264 measured, "
            "HEVC/VP9/AV1 bundled-unverified")
    rows.append(TAB.join([PKG_NAME, PKG_VERSION, str(size), sha,
                          PKG_NAME + ".tar.gz", desc]))
    tmp = idx + ".part"
    with open(tmp, "w", encoding="utf-8", newline=NL) as f:
        f.write(NL.join(rows) + NL)
    os.replace(tmp, idx)
    with open(os.path.join(repo, "vlc.row"), "w", encoding="utf-8",
              newline=NL) as f:
        f.write(rows[-1] + NL)

    key = os.path.join("build", "sign", "repo.key")
    pub = os.path.join("stage", "etc", "mystic", "repo.pub")
    if os.path.isfile(key):
        # flush ours first: the signer is a child process and inherits the
        # same stdout, so buffered lines would otherwise interleave with it.
        sys.stdout.flush()
        cmd = [sys.executable, os.path.join("tools", "sign-index.py"), "sign",
               "--seed", key, "--in", idx]
        if os.path.isfile(pub):
            cmd += ["--pub", pub]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("warning: could not sign %s" % idx, file=sys.stderr)
        elif os.path.isfile(pub):
            shutil.copyfile(pub, os.path.join(repo, "index.pub"))
    else:
        print("    no build/sign/repo.key: index.tsv is UNSIGNED and pkg will "
              "refuse it", file=sys.stderr)
    return size, sha


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="build the libVLC vmem player package for Mystic OS")
    ap.add_argument("--profile", default="playback", choices=sorted(PROFILES),
                    help="playback (default) = containers + libavcodec + the "
                         "dedicated dav1d/vpx/theora/opus/vorbis/mpg123 decoders "
                         "+ subtitle decoding; minimal = images/raw only; "
                         "subs = playback + the FreeType text renderer + "
                         "libass (ASS/SSA); full = apk add vlc")
    ap.add_argument("--root", action="append", default=None,
                    help="extra package or so:name to add to the closure")
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and print; download nothing beyond the "
                         "two vlc packages needed to read DT_NEEDED")
    ap.add_argument("--audit-only", action="store_true",
                    help="print the plugin audit and stop (needs the cached "
                         "vlc.apk; --dry-run also prints it)")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-verify the already packed tarball and stop: every "
                         "DT_NEEDED soname and every imported symbol")
    ap.add_argument("--download", action="store_true",
                    help="accepted for compatibility; the default builds")
    ap.add_argument("--no-pack", action="store_true",
                    help="build the root but do not create the package")
    ap.add_argument("--max-download-mb", type=float, default=300.0,
                    help="refuse a plan bigger than this (default 300)")
    ap.add_argument("--force", action="store_true",
                    help="ignore the root/package caches and the download cap")
    ap.add_argument("--refresh-index", action="store_true",
                    help="re-download the Alpine APKINDEX files")
    ap.add_argument("--list", action="store_true", help="print the whole closure")
    args = ap.parse_args()

    if not os.path.isdir("src"):
        print("error: run this from the repository root", file=sys.stderr)
        return 2
    os.makedirs(INDEX_CACHE, exist_ok=True)

    tarball_path = os.path.join(args.repo, PKG_NAME + ".tar.gz")
    tarball_path_default = os.path.join(REPO, PKG_NAME + ".tar.gz")
    if args.verify_only:
        if not os.path.isfile(tarball_path):
            print("error: %s does not exist; build it first" % tarball_path,
                  file=sys.stderr)
            return 1
        try:
            summary = verify_tarball(tarball_path)
        except (RuntimeError, OSError, tarfile.TarError) as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
        print("==> %s verified: %d objects checked for DT_NEEDED by exact name, "
              "%d plugin/player symbol sets, %d aliases, %d dlopen target(s) (%s), "
              "%d imports, %d NEEDED failures, %d symbol failures"
              % (tarball_path, summary["needed_objects"], summary["objects"],
                 summary["aliases"], len(summary["dlopen"]),
                 ", ".join(summary["dlopen"]), summary["undefined_symbols"],
                 summary["needed_failures"], summary["symbol_failures"]))
        # Provenance: is this tarball the one the current sources would build?
        # The stamp records the player source's own hash, so a stale artifact
        # is detectable without rebuilding or trusting a timestamp.
        # Provenance is a gate, not a note: a tarball that verifies but was not
        # built from these sources is not the artifact this tree publishes.
        # The gate is app_digest_src (APP_SRC + this tool + the manifest) plus
        # vlcplay_sha256. The compiler version is part of the cache key, not
        # of the publish gate: a host without zig can still verify, it just
        # cannot compare the toolchain component.
        stamp_path = stamp_path_for(args.repo)
        stamp = read_json(stamp_path)
        if not stamp:
            # No stamp for this repo (e.g. a hand-made repo): fall back to the
            # only cross-artifact fact available - does the packed player match
            # the published package's?
            mine = packed_player_sha(tarball_path)
            theirs = packed_player_sha(tarball_path_default)
            if mine and theirs and mine == theirs:
                print("    (no %s; provenance: player bytes match the default "
                      "package, sha256 %s...)" % (stamp_path, mine[:16]))
            elif mine and theirs:
                print("    (no %s; provenance: player DIFFERS from the default "
                      "package: %s... vs %s...)"
                      % (stamp_path, mine[:16], theirs[:16]))
            else:
                print("    (no %s; provenance cannot be checked)" % stamp_path)
            return 0
        built_src = stamp.get("app_digest_src")
        built_player = stamp.get("vlcplay_sha256")
        now_src = app_inputs_digest(None)
        now_player = sha256_file("src/client/apps/vlcplay.c")
        stale, notes = [], []
        if built_src and built_src != now_src:
            stale.append("sources/tooling built %s..., now %s..."
                         % (built_src[:16], now_src[:16]))
        if built_player and built_player != now_player:
            stale.append("vlcplay.c built %s..., now %s..."
                         % (built_player[:16], now_player[:16]))
        if not built_src or not built_player:
            notes.append("stamp has no source/player digests (old build); "
                         "rebuild to record them")
        zig = find_zig()
        if zig and stamp.get("app_digest"):
            if app_inputs_digest(zig) != stamp["app_digest"]:
                notes.append("zig version differs from the build (toolchain "
                             "component of the cache key, not of the gate)")
        elif not zig:
            notes.append("zig not found: the toolchain component is not "
                         "checked; the gate is source digest + player hash")
        for n in notes:
            print("    note: %s" % n)
        if stale:
            print("    STALE package: it verifies internally but was not built "
                  "from these sources (%s)" % "; ".join(stale))
            print("    the publish gate is app_digest_src + vlcplay_sha256; "
                  "rebuild with: python tools/fetch-vlc.py")
            return 1
        if built_src and built_player:
            print("    package is current (%s): sources %s..., vlcplay.c %s..."
                  % (stamp_path, built_src[:16], built_player[:16]))
        return 0

    pkgs, provides = build_world(args.refresh_index)
    print("==> provides: %d" % len(provides))
    plugins = PROFILES[args.profile]
    extra = args.root or []
    wanted = None          # files to take out of the vlc package, if trimmed
    for bad in check_roots(pkgs, provides, extra):
        print("==> warning: --root %r is neither a package nor a provide in the index"
              % bad)

    if plugins is None:
        closure, missing = resolve(pkgs, provides, ["vlc"] + BASE_ROOTS + extra)
        label = "apk add vlc (whole package + whole D: line)"
        plugin_files = None
    else:
        # The closure has to be derived from the plugin binaries, so the two
        # small vlc packages are always needed - 6 MB, cached after the first
        # run. Say so out loud rather than hiding a download in a "dry" run.
        print("==> probing plugin DT_NEEDED: vlc + vlc-libs (%.2f MB of .apk, cached)"
              % ((pkgs["vlc"]["download"] + pkgs["vlc-libs"]["download"]) / 1e6))
        for need in ("vlc", "vlc-libs"):
            if not get_apk(pkgs, need, True):
                print("error: could not fetch %s" % need, file=sys.stderr)
                return 1
        wanted = set(LIBVLC_FILES + ["usr/lib/vlc/plugins/" + p for p in plugins])
        # probe into the cache, not into --dest: a dry run must not populate the
        # tree it says it would build.
        probe = os.path.join(APK_CACHE, "_probe")
        if os.path.isdir(probe):
            shutil.rmtree(probe)
        for need in ("vlc", "vlc-libs"):
            unpack_apk(apk_path(pkgs, need), probe, members=wanted)

        needed = set()
        missing_plugins = []
        for p in plugins:
            f = os.path.join(probe, "usr/lib/vlc/plugins", p)
            if not os.path.exists(f):
                missing_plugins.append(p)
                continue
            for so in elf_needed(f):
                if so != "libc.musl-x86_64.so.1":     # the base image has musl
                    needed.add("so:" + so)
        for p in LIBVLC_FILES:
            f = os.path.join(probe, p)
            if os.path.exists(f):
                for so in elf_needed(f):
                    if so != "libc.musl-x86_64.so.1":
                        needed.add("so:" + so)
        if missing_plugins:
            print("error: plugin files named by the profile are not in vlc.apk: %s"
                  % ", ".join(missing_plugins), file=sys.stderr)
            return 1
        print("==> DT_NEEDED of those files: %s" % ", ".join(sorted(needed)))
        closure, missing = resolve(pkgs, provides, sorted(needed) + extra)
        label = "profile %r (libvlc + %d plugins)" % (args.profile, len(plugins))
        plugin_files = plugins

    dl, _ins = report(pkgs, closure, missing, label)
    if args.list:
        for n in sorted(n for n in closure if n in pkgs):
            p = pkgs[n]
            print("    %-28s %-14s %8.2f MB installed" % (n, p["version"],
                                                          p["installed"] / 1e6))

    # What Alpine's VLC really carries vs what this profile takes. Printed on
    # every run, including --dry-run: it is the evidence the docs quote.
    audit = audit_plugins(pkgs, provides, closure, plugin_files, args.profile)
    if args.audit_only:
        return 0
    if audit and audit["verdict"]["unresolved"]:
        print("error: a plugin in this profile needs a library the bundle does "
              "not ship", file=sys.stderr)
        return 1

    if plugin_files:
        base = os.path.join(APK_CACHE, "_probe", "usr/lib/vlc/plugins")
        payload = sum(os.path.getsize(os.path.join(base, p)) for p in plugin_files
                      if os.path.exists(os.path.join(base, p)))
        nplug, bplug = apk_stats(apk_path(pkgs, "vlc"), "usr/lib/vlc/plugins/")
        nall, ball = apk_stats(apk_path(pkgs, "vlc"))
        print("==> payload: %d of %d plugin files, %.2f MB of the %.2f MB of plugins "
              "in the vlc package" % (len(plugin_files), nplug, payload / 1e6, bplug / 1e6))
        print("    the rest of that package (%.2f MB: /usr/bin, /usr/share/vlc, %d files) "
              "is not kept" % ((ball - bplug) / 1e6, nall - nplug))

    if dl / 1e6 > args.max_download_mb and not args.force:
        print("==> %.2f MB download exceeds --max-download-mb %.0f: refusing."
              % (dl / 1e6, args.max_download_mb))
        print("    Nothing else was downloaded. Re-run with --force to override.")
        return 0

    if args.dry_run:
        print("==> dry run: nothing further downloaded. Without --dry-run this "
              "builds %s and %s/%s.tar.gz" % (args.dest, args.repo, PKG_NAME))
        return 0

    stamp = root_signature(args.profile, closure, pkgs, plugins)
    if not build_root(pkgs, closure, args.dest, args.profile, plugins, stamp,
                      force=args.force):
        return 1

    if args.no_pack:
        print("==> --no-pack: %s left as the loose tree" % args.dest)
        return 0

    zig = find_zig()
    if not zig:
        print("error: zig not found (set ZIG or put zig on PATH); the player "
              "cannot be compiled into the package", file=sys.stderr)
        return 1
    digest = app_inputs_digest(zig)
    # Two digests: the cache key includes the compiler version; the publish
    # gate is source-only, so `--verify-only` works on a host without zig.
    src_digest = app_inputs_digest(None)
    # Provenance: which source the packed player came from. The package cache
    # is keyed on this digest, but printing the player's own hash makes the
    # question "which revision is this tarball?" answerable from the artifact
    # instead of from a file's mtime.
    player_src_sha = sha256_file("src/client/apps/vlcplay.c")

    # The image bundle first: it is the small, honest half of this tool.
    try:
        stub, stub_info = build_image_stub(zig)
    except (RuntimeError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("==> image bundle: %s/{manifest,bin}" % IMAGE_BUNDLE)
    print("    bin: %d bytes, static musl, no PT_INTERP, no DT_NEEDED"
          % stub_info["size"])
    if os.path.isdir(OLD_BUNDLE):
        print("    WARNING: the old bundle %s still exists; two ids would give two\n"
              "    tiles. Remove it - the id is %r everywhere now."
              % (OLD_BUNDLE, PKG_NAME), file=sys.stderr)

    stage = PKG_STAGE
    pkg_stamp_path = stamp_path_for(args.repo)
    tarball = tarball_path
    prev = read_json(pkg_stamp_path)
    cache_hit = (not args.force and prev and prev.get("app_digest") == digest
                 and prev.get("profile") == args.profile
                 and os.path.isfile(tarball)
                 and os.path.getsize(tarball) == prev.get("size")
                 and sha256_file(tarball) == prev.get("sha256"))
    if cache_hit:
        print("==> package cache hit: %s is up to date" % tarball)
        info = prev
        size = prev["size"]
        sha = prev["sha256"]
        usize, ufiles = prev.get("installed", 0), prev.get("files", 0)
        # A cache hit is still an artifact we are about to publish: prove it
        # again. Two seconds of ELF reading beats trusting a stamp.
        try:
            info["verify"] = verify_tarball(tarball)
        except (RuntimeError, OSError, tarfile.TarError) as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
    else:
        try:
            entries, info = build_stage(args.dest, stage, args.profile, plugins,
                                        digest, zig, pkgs)
        except (RuntimeError, OSError) as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
        size = pack_tarball(entries, stage, tarball)
        print("==> packed %s: %.2f MB, %d entries, %d libraries, %d plugins"
              % (tarball, size / 1e6, len(entries) + 1, info["libs"],
                 len(info["plugins"])))
        try:
            info["verify"] = verify_tarball(tarball)
        except (RuntimeError, OSError, tarfile.TarError) as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
        info["vlcplay_sha256"] = player_src_sha
        info["app_digest_src"] = src_digest
        sha = sha256_file(tarball)
        # Payload files and installed bytes are the tarball's own file list,
        # not the staging tree (which also holds .vlc-pkg.json and would
        # overcount by one).
        payload_files = sum(1 for e in entries if e[1] == "file")
        payload_bytes = sum(os.path.getsize(e[2]) for e in entries
                            if e[1] == "file")
        usize, ufiles = payload_bytes, payload_files
        info["size"] = size
        info["sha256"] = sha
        info["installed"] = usize
        info["files"] = ufiles
        write_json_atomic(pkg_stamp_path, info)

    print("==> package: %s  %.2f MB compressed, %.2f MB installed, %d files"
          % (tarball, size / 1e6, usize / 1e6, ufiles))
    print("    bin (launcher): %d bytes, static musl, no PT_INTERP"
          % info.get("boot_size", 0))
    print("    libexec/vlcplay: %d bytes, DYNAMIC musl, PT_INTERP %s"
          % (info.get("player_size", 0), info.get("player_interp", "?")))
    print("    player DT_NEEDED: %s   RUNPATH: %s"
          % (", ".join(info.get("player_needed", [])) or "(none)",
             ", ".join(info.get("player_rpath", [])) or "(none)"))
    print("    lib/%s: %d bytes from musl %s, SONAME libc.musl-x86_64.so.1"
          % (os.path.basename(LOADER_IN_PKG), info.get("loader_size", 0),
             info.get("loader_version", "?")))
    print("    libvlc.so.5 SONAME: %s (dlopen()ed from lib/, never -lvlc)"
          % (info.get("libvlc_soname") or "?"))
    print("    manifest: exec = %s (bare name; version = %s = the index row)"
          % (info.get("exec", "bin"), info.get("version", PKG_VERSION)))
    print("    id everywhere: package %r = bundle %r = manifest 'bundle = %s'"
          % (PKG_NAME, PKG_NAME, PKG_NAME))
    v = info.get("verify") or {}
    if v:
        print("    static proof: %d objects x exact DT_NEEDED, %d lib files, "
              "%d aliases, %d dlopen targets, %d imports, %d unresolved NEEDED, "
              "%d unresolved symbols"
              % (v["needed_objects"], v["libraries"], v["aliases"],
                 len(v.get("dlopen", [])), v["undefined_symbols"],
                 v["needed_failures"], v["symbol_failures"]))
    isz, isha = upsert_index(tarball_path, args.repo)
    print("    built from: src/client/apps/vlcplay.c sha256 %s..., "
          "gate digest %s... (cache key %s...)"
          % ((info.get("vlcplay_sha256") or "not recorded")[:16],
             (info.get("app_digest_src") or "?")[:16],
             (info.get("app_digest") or "?")[:16]))
    print("    index row: %s %s %d %s..." % (PKG_NAME, PKG_VERSION, isz, isha[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
