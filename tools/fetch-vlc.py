#!/usr/bin/env python3
"""
fetch-vlc.py - resolve (and optionally fetch) the libVLC closure for Mystic OS.

Why this exists: `apk add vlc` on Alpine is one package that ships *every*
plugin, so its D: line lists every shared library any plugin links against -
GTK, X11, EGL/mesa, samba, gstreamer. That closure is the first number this
script prints, so nobody has to take it on faith.

The second number is the one we might afford: libVLC itself (vlc-libs:
libvlc.so.5 + libvlccore.so.9) plus only the plugin .so files a "video in a
window" player needs. Their real DT_NEEDED entries are read out of the
downloaded .apk with the ELF reader below, and only those `so:` names are
resolved against the index - the closure comes from the actual binaries, not
from a guessed list.

    python tools/fetch-vlc.py                      # dry run: full vlc closure
    python tools/fetch-vlc.py --profile trim       # dry run: libvlc + vmem+ffmpeg
    python tools/fetch-vlc.py --profile minimal    # dry run: libvlc + vmem, no ffmpeg
    python tools/fetch-vlc.py --profile trim --download

Dry run is the default and nothing is written outside build/.  A plan bigger
than --max-download-mb is refused (pass --force to override).  A plugin profile
does fetch the two small vlc packages (~7 MB) even in a dry run, because the
closure is derived from their binaries; that is the only exception and it is
printed when it happens.

Caches: build/vlc-index/ and build/vlc-apks/.  Destination: build/vlc-root/.
All under build/, which is outside the image - the image is built from stage/.

Field note, because it bit us in tools/fetch-browser.py: APKINDEX `S:` is the
.apk download size and `I:` is the *installed* size.  fetch-browser.py prints
I: as "compressed", which overstates the download.  Both are printed here,
labelled.
"""

import argparse
import io
import os
import re
import struct
import sys
import tarfile
import urllib.request

ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.24"
REPOS = ["main", "community"]
ARCH = "x86_64"

INDEX_CACHE = os.path.join("build", "vlc-index")
APK_CACHE = os.path.join("build", "vlc-apks")
DEST = os.path.join("build", "vlc-root")

# "libvlc" is not a package name in Alpine: vlc-libs provides both shared
# objects, so the roots are written the way the index writes them.
BASE_ROOTS = ["so:libvlc.so.5", "so:libvlccore.so.9"]

# ---------------------------------------------------------------------------
# Which plugin .so files a player needs. Paths are relative to
# /usr/lib/vlc/plugins. Read out of the plugin directory of vlc-3.0.23-r4.apk;
# a runtime VLC may still load extra modules (a text renderer for subtitles,
# a demuxer for another container), which is what a first run in the VM says.
# ---------------------------------------------------------------------------
PLUGINS = {
    # vmem = decode into memory we own; rv32/i420_rgb = convert to RV32, which
    # is byte-identical to a Mystic window; chain = the chroma filter chain.
    "vout": [
        "video_output/libvmem_plugin.so",
        "video_chroma/librv32_plugin.so",
        "video_chroma/libchain_plugin.so",
        "video_chroma/libi420_rgb_sse2_plugin.so",
    ],
    # VLC's own decoders: no external codec library at all.
    "image": [
        "demux/libimage_plugin.so",
        "demux/librawvid_plugin.so",
        "codec/librawvideo_plugin.so",
        "codec/libpng_plugin.so",
        "codec/libjpeg_plugin.so",
    ],
    "file": ["access/libfilesystem_plugin.so"],
    # H.264 / HEVC / VP9 / AAC: everything that needs libavcodec.
    "ffmpeg": [
        "demux/libmp4_plugin.so",
        "demux/libmkv_plugin.so",
        "demux/libavi_plugin.so",
        "codec/libavcodec_plugin.so",
        "packetizer/libpacketizer_h264_plugin.so",
        "packetizer/libpacketizer_hevc_plugin.so",
        "access/libhttp_plugin.so",
    ],
    "subs": ["text_renderer/libfreetype_plugin.so"],
}

PROFILES = {
    # cheapest thing that proves the display path works: a PNG or raw frame
    # decoded by VLC's own decoders and handed to our buffer.
    "minimal": PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"],
    # plus H.264/HEVC/VP9/AAC via libavcodec.
    "trim": PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"] + PLUGINS["ffmpeg"],
    # plus subtitle rendering.
    "trim+subs": (PLUGINS["vout"] + PLUGINS["image"] + PLUGINS["file"]
                  + PLUGINS["ffmpeg"] + PLUGINS["subs"]),
    # what `apk add vlc` would do.
    "full": None,
}

LIBVLC_FILES = ["usr/lib/libvlc.so.5.6.1", "usr/lib/libvlccore.so.9.0.1",
                "usr/lib/vlc/vlc-cache-gen"]


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


def load_index(repo):
    """{package: {version,file,download,installed,depends,provides,repo}}."""
    path = os.path.join(INDEX_CACHE, "%s-%s.idx.tar.gz" % (repo, ARCH))
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
            # APKINDEX v3 has no filename field: it is <name>-<version>.apk
            "file": (d.get("F") or ["%s-%s.apk" % (name, ver)])[0],
            "download": int((d.get("S") or ["0"])[0] or 0),    # .apk size
            "installed": int((d.get("I") or ["0"])[0] or 0),   # unpacked size
            "depends": " ".join(d.get("D") or []).split(),
            "provides": " ".join(d.get("p") or []).split(),
            "repo": repo,
        }
    return pkgs


def build_world():
    pkgs, provides = {}, {}
    for repo in REPOS:
        idx = load_index(repo)
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
# ELF DT_NEEDED - the plugin closure comes from the binaries themselves
# ---------------------------------------------------------------------------
def elf_needed(path):
    """DT_NEEDED sonames of an ELF file; [] if it is not readable as one."""
    try:
        with open(path, "rb") as f:
            b = f.read()
        if b[:4] != b"\x7fELF" or len(b) < 64:
            return []
        is64 = b[4] == 2
        le = "<" if b[5] == 1 else ">"
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
        if dyn is None or dstr is None:
            return []
        out, off, end = [], dyn[4], dyn[4] + dyn[5]
        while off + 16 <= end:
            tag, val = struct.unpack_from(le + "qQ", b, off)
            off += 16
            if tag == 0:
                break
            if tag == 1:                        # DT_NEEDED
                out.append(name_at(dstr + val))
        return out
    except (ValueError, IndexError, struct.error, OSError):
        return []


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


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="resolve the libVLC closure "
                                             "from the Alpine index")
    ap.add_argument("--profile", default="full", choices=sorted(PROFILES),
                    help="full (default) = what apk add vlc pulls; trim = libvlc"
                         "+vmem+ffmpeg; minimal = libvlc+vmem, VLC decoders only")
    ap.add_argument("--root", action="append", default=None,
                    help="extra package or so:name to add to the closure")
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--download", action="store_true",
                    help="actually download and unpack (default: dry run)")
    ap.add_argument("--dry-run", action="store_true",
                    help="the default, stated explicitly")
    ap.add_argument("--max-download-mb", type=float, default=300.0,
                    help="refuse a plan bigger than this (default 300)")
    ap.add_argument("--force", action="store_true", help="download past the cap")
    ap.add_argument("--list", action="store_true", help="print the whole closure")
    args = ap.parse_args()

    download = args.download and not args.dry_run
    if not os.path.isdir("src"):
        print("error: run this from the repository root", file=sys.stderr)
        return 2
    os.makedirs(INDEX_CACHE, exist_ok=True)

    pkgs, provides = build_world()
    print("==> provides: %d" % len(provides))
    plugins = PROFILES[args.profile]
    extra = args.root or []
    wanted = None          # files to take out of the vlc package, if not whole
    for bad in check_roots(pkgs, provides, extra):
        print("==> warning: --root %r is neither a package nor a provide in the index"
              % bad)

    if plugins is None:
        closure, missing = resolve(pkgs, provides, ["vlc"] + BASE_ROOTS + extra)
        label = "apk add vlc (whole package + whole D: line)"
        plugin_files = None
    else:
        # The closure has to be derived from the plugin binaries, so the two
        # small vlc packages are always needed - 7 MB, cached after the first
        # run. Say so out loud rather than hiding a download in a "dry" run.
        print("==> probing plugin DT_NEEDED: vlc + vlc-libs (%.2f MB of .apk, cached)"
              % ((pkgs["vlc"]["download"] + pkgs["vlc-libs"]["download"]) / 1e6))
        for need in ("vlc", "vlc-libs"):
            if not get_apk(pkgs, need, True):
                print("error: could not fetch %s" % need, file=sys.stderr)
                return 1
        wanted = set(["usr/lib/vlc/plugins/" + p for p in plugins] + LIBVLC_FILES)  # noqa: E501
        # probe into the cache, not into --dest: a dry run must not populate the
        # tree it says it would build.
        probe = os.path.join(APK_CACHE, "_probe")
        for need in ("vlc", "vlc-libs"):
            unpack_apk(apk_path(pkgs, need), probe, members=wanted)

        needed = set()
        for p in ["usr/lib/vlc/plugins/" + x for x in plugins] + ["usr/lib/libvlc.so.5.6.1"]:
            f = os.path.join(probe, p)
            if not os.path.exists(f):
                print("    MISSING in the package: %s" % p, file=sys.stderr)
                continue
            for so in elf_needed(f):
                if so != "libc.musl-x86_64.so.1":     # the base image has musl
                    needed.add("so:" + so)
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

    if not download:
        print("==> dry run: nothing further downloaded. --download fetches %.2f MB "
              "into %s" % (dl / 1e6, args.dest))
        return 0

    # ---- download + unpack ----------------------------------------------
    os.makedirs(APK_CACHE, exist_ok=True)
    os.makedirs(args.dest, exist_ok=True)
    got, size, files = 0, 0, []
    for name in sorted(n for n in closure if n in pkgs):
        path = get_apk(pkgs, name, True)
        if not path:
            continue
        got += 1
        size += os.path.getsize(path)
        files.append((name, path))
        if got % 25 == 0:
            print("    %d/%d packages, %.1f MB" % (got, len(closure), size / 1e6))
    print("==> downloaded %d packages, %.1f MB" % (got, size / 1e6))

    written = fixed = 0
    # the plugin files come from the vlc package, which is *not* a dependency of
    # vlc-libs and therefore not in the closure - take just what we asked for.
    if wanted is not None:
        for need in ("vlc", "vlc-libs"):
            w, f = unpack_apk(apk_path(pkgs, need), args.dest, members=wanted)
            written += w
            fixed += f
    for _name, path in files:
        w, f = unpack_apk(path, args.dest)
        written += w
        fixed += f
    total, nfiles = tree_bytes(args.dest)
    print("==> extracted to %s: %d files written, %.2f MB on disk, "
          "%d symlinks copied" % (args.dest, nfiles, total / 1e6, fixed))

    cachegen = os.path.join(args.dest, "usr/lib/vlc/vlc-cache-gen")
    print("==> plugin cache: %s"
          % ("vlc-cache-gen present - run it against usr/lib/vlc/plugins at image build"
             if os.path.exists(cachegen) else
             "vlc-cache-gen absent - libvlccore will scan the plugin directory"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
