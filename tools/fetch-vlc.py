#!/usr/bin/env python3
"""
fetch-vlc.py - build the libVLC video player package for Mystic OS.

One command builds every artifact the player needs and prints the real
numbers it found:

    python tools/fetch-vlc.py                 # resolve, download, unpack, build, pack
    python tools/fetch-vlc.py --dry-run       # resolve and print only
    python tools/fetch-vlc.py --profile minimal
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
    libexec/vlcplayer          the player: DYNAMIC musl, PT_INTERP
                               /lib/ld-musl-x86_64.so.1, RUNPATH $ORIGIN/../lib
    lib/ld-musl-x86_64.so.1    the musl loader from Alpine's musl apk (0755)
    lib/libvlc.so.5 ...        libVLC and its real closure
    lib/vlc/plugins/...        only the plugin files a profile needs

The manifest's 'exec' must be a BARE FILE NAME. src/wmsrv.c (wms_launch_arg)
rejects any exec containing '/' or '..' - 'unsafe exec' - so 'exec =
libexec/vlcplayer' is not a layout this OS can launch, and nothing here may
claim otherwise. 'exec = bin' names the static launcher at the bundle root;
libexec/ is an internal path the launcher execs through the bundled loader.

Why a launcher and not just a dynamic bin
-----------------------------------------
The image ships no dynamic loader (stage/lib has no ld-musl), and the
kernel refuses to exec a dynamic ELF whose PT_INTERP does not exist. So the
package's entry point is the static launcher; it replaces itself with the
bundled loader:

    lib/ld-musl-x86_64.so.1 --library-path <bundle>/lib         <bundle>/libexec/vlcplayer <args...>

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
  * `--profile playback` (the default) decodes H.264/HEVC/MPEG-2/VP8/VP9/
    AV1/AAC/MP3/AC-3 through Alpine's libavcodec. That is a large closure;
    the printed numbers are the real ones.
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
    "src/client/apps/vlcplayer.c",
    "src/client/mystic_client.c",
    "src/gfx.c",
    "src/font.c",
    "src/ui.c",
]
BOOT_SRC = ["src/client/apps/vlcplayer.c"]

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
    ],
    # H.264 / HEVC / MPEG-2 / VC-1 / VP8 / VP9 / AV1 / AAC / MP3 / AC-3:
    # everything that needs libavcodec. The packetizers split elementary
    # streams inside mp4/mkv/ts.
    "ffmpeg": [
        "codec/libavcodec_plugin.so",
        "packetizer/libpacketizer_h264_plugin.so",
        "packetizer/libpacketizer_hevc_plugin.so",
        "packetizer/libpacketizer_mpeg4video_plugin.so",
        "packetizer/libpacketizer_mpeg4audio_plugin.so",
        "packetizer/libpacketizer_mpegvideo_plugin.so",
        "packetizer/libpacketizer_vc1_plugin.so",
    ],
    # The image has no sound device; this is what --aout=dummy resolves to.
    # It adds 14 KB and no library, so "no sound" is a clean state instead
    # of a failing ALSA probe per media.
    "aout": ["audio_output/libadummy_plugin.so"],
    "http": ["access/libhttp_plugin.so"],
    "subs": ["text_renderer/libfreetype_plugin.so"],
}

PROFILES = {
    # images + raw frames, no external codec library.
    "minimal": PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"],
    # the same, plus local containers and libavcodec. This is the default.
    "playback": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
                 + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["aout"]),
    # playback over HTTP as well (adds TLS, more libraries).
    "network": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
                + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["aout"]
                + PLUGINS["http"]),
    # plus subtitle rendering.
    "subs": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
             + PLUGINS["demux"] + PLUGINS["ffmpeg"] + PLUGINS["aout"]
             + PLUGINS["subs"]),
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
    out = {"size": 0, "e_type": 0, "interp": "", "needed": [], "rpath": [],
           "soname": "", "entry": 0}
    try:
        with open(path, "rb") as f:
            b = f.read()
    except OSError:
        return out
    out["size"] = len(b)
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


def build_player(stage, zig):
    """Compile the DYNAMIC player, the one that talks to libVLC.

    -dynamic is not optional: zig's musl target defaults to a static link,
    and a static musl program's dlopen is a stub. RUNPATH $ORIGIN/../lib is
    the third way to find the closure (the launcher passes --library-path,
    wmsrv passes LD_LIBRARY_PATH), so a direct exec still works.
    """
    out = os.path.join(stage, "libexec", "vlcplayer")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-O2", "-dynamic",
           "-Wl,-rpath,$ORIGIN/../lib",
           "-Isrc", "-Isrc/client", "-o", out] + APP_SRC + ["-lm", "-pthread"]
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
    """Compile the static launcher that becomes the package's exec = bin."""
    out = os.path.join(stage, "bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os", "-static", "-s",
           "-DVP_BOOT=1", "-Isrc", "-Isrc/client", "-o", out] + BOOT_SRC
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
    entries.append(("libexec/vlcplayer", "file", player, 0o755, ""))
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
        [("libexec/vlcplayer", player)] +
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


def upsert_index(tarball):
    """Put the vlc row into build/repo/index.tsv and re-sign it if we can.

    tools/serve-repo.sh rebuilds index.tsv from stage/usr/share/mystic/apps,
    so a later run of that script will drop this row; the tarball stays. The
    final report says so instead of implying the repository is permanent.
    """
    os.makedirs(REPO, exist_ok=True)
    idx = os.path.join(REPO, "index.tsv")
    rows = []
    if os.path.isfile(idx):
        with open(idx, "r", encoding="utf-8", newline="") as f:
            for line in f.read().splitlines():
                if not line.strip():
                    continue
                if line.split("\t", 1)[0] == PKG_NAME:
                    continue
                rows.append(line)
    size = os.path.getsize(tarball)
    sha = sha256_file(tarball)
    desc = "libVLC video player: vmem output, real H.264/HEVC/VP9 decode"
    rows.append("%s\t%s\t%d\t%s\t%s.tar.gz\t%s"
                % (PKG_NAME, PKG_VERSION, size, sha, PKG_NAME, desc))
    tmp = idx + ".part"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    os.replace(tmp, idx)
    with open(os.path.join(REPO, "vlc.row"), "w", encoding="utf-8", newline="\n") as f:
        f.write(rows[-1] + "\n")

    key = os.path.join("build", "sign", "repo.key")
    pub = os.path.join("stage", "etc", "mystic", "repo.pub")
    if os.path.isfile(key):
        cmd = [sys.executable, os.path.join("tools", "sign-index.py"), "sign",
               "--seed", key, "--in", idx]
        if os.path.isfile(pub):
            cmd += ["--pub", pub]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("warning: could not sign index.tsv", file=sys.stderr)
        elif os.path.isfile(pub):
            shutil.copyfile(pub, os.path.join(REPO, "index.pub"))
    else:
        print("    no build/sign/repo.key: index.tsv is UNSIGNED and pkg will "
              "refuse it", file=sys.stderr)
    return size, sha


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="build the libVLC vmem player package for Mystic OS")
    ap.add_argument("--profile", default="playback", choices=sorted(PROFILES),
                    help="playback (default) = local files + libavcodec; "
                         "minimal = images/raw only; full = apk add vlc")
    ap.add_argument("--root", action="append", default=None,
                    help="extra package or so:name to add to the closure")
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and print; download nothing beyond the "
                         "two vlc packages needed to read DT_NEEDED")
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
    pkg_stamp_path = os.path.join("build", "vlc-pkg.stamp")
    tarball = os.path.join(args.repo, PKG_NAME + ".tar.gz")
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
        sha = sha256_file(tarball)
        usize, ufiles = tree_bytes(stage)
        info["size"] = size
        info["sha256"] = sha
        info["installed"] = usize
        info["files"] = ufiles
        write_json_atomic(pkg_stamp_path, info)

    print("==> package: %s  %.2f MB compressed, %.2f MB installed, %d files"
          % (tarball, size / 1e6, usize / 1e6, ufiles + 1))
    print("    bin (launcher): %d bytes, static musl, no PT_INTERP"
          % info.get("boot_size", 0))
    print("    libexec/vlcplayer: %d bytes, DYNAMIC musl, PT_INTERP %s"
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
    isz, isha = upsert_index(tarball)
    print("    index row: %s %s %d %s..." % (PKG_NAME, PKG_VERSION, isz, isha[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
