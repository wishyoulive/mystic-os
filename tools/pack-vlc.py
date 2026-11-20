#!/usr/bin/env python3
"""Build the VLC package: player binary, libVLC closure, plugins.

The upstream closure comes from the Alpine vlc package as unpacked by
tools/fetch-vlc.py into build/vlc-orig.tar.gz, whose layout is usr/lib. A
bundle needs lib/ (that is where the player and the loader look), so paths are
remapped here, and every shared object gets the soname links the dynamic loader
actually asks for:

    libvlc.so.5      -> libvlc.so.5.6.1
    libvlccore.so.9  -> libvlccore.so.9.0.1

Windows extraction cannot create those links, which is why a package rebuilt
from an extracted tree installs and then fails with "libvlc.so.5: No such file
or directory" - the versioned file is there, the name the loader wants is not.

Usage:
  python tools/pack-vlc.py --src build/vlc-orig.tar.gz \
                           --bin build/vlc-pack/bin \
                           --out build/repo/vlc.tar.gz
"""

import argparse
import hashlib
import io
import os
import re
import sys
import tarfile


def sonames(name):
    """The names a loader may ask for, given a versioned library file name."""
    out = []
    m = re.match(r"^(lib.*?\.so)\.(\d+)\.(\d+)\.(\d+)$", name)
    if m:
        out.append(m.group(1) + "." + m.group(2))          # libX.so.5
        out.append(m.group(1))                             # libX.so
    else:
        m = re.match(r"^(lib.*?\.so)\.(\d+)$", name)
        if m:
            out.append(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--src", default=os.path.join(here, "build", "vlc-orig.tar.gz"))
    ap.add_argument("--bin", default=os.path.join(here, "build", "vlc-pack", "bin"))
    ap.add_argument("--out", default=os.path.join(here, "build", "repo", "vlc.tar.gz"))
    ap.add_argument("--version", default="3.0.23")
    args = ap.parse_args()

    for p in (args.src, args.bin):
        if not os.path.exists(p):
            sys.exit("missing %s" % p)

    manifest = (
        "name = VLC Player\n"
        "desc = Video and audio player (libVLC), decoded in the bundle and drawn in its own window\n"
        "exec = bin\n"
        "version = %s\n"
        "network = host\n"
        "audio = yes\n"
        "bundle = vlc\n"
        "placeholder = yes\n" % args.version
    )

    want_prefixes = ("usr/lib/", "usr/share/vlc/", "etc/")
    n_files = n_links = 0
    libs = []

    with tarfile.open(args.src, "r:*") as src, tarfile.open(args.out, "w:gz") as dst:
        def add(name, data=None, link=None, mode=0o644, size=0):
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            if link is not None:
                ti.type = tarfile.SYMTYPE
                ti.linkname = link
                dst.addfile(ti)
            else:
                ti.size = size
                dst.addfile(ti, data)

        def add_file(name, path, mode=0o644):
            st = os.stat(path)
            with open(path, "rb") as f:
                ti = tarfile.TarInfo(name)
                ti.size = st.st_size
                ti.mode = mode
                ti.mtime = int(st.st_mtime)
                dst.addfile(ti, f)

        add_file("bin", args.bin, 0o755)
        add_file("manifest", path=os.path.join(here, ".work", "vlc-manifest"), mode=0o644) \
            if False else add("manifest", io.BytesIO(manifest.encode()), size=len(manifest))

        for ti in src:
            name = ti.name.lstrip("./")
            if not name.startswith(want_prefixes):
                continue
            # usr/lib -> lib: the bundle root the player resolves against
            out = name[len("usr/"):] if name.startswith("usr/") else name
            if ti.isdir():
                continue
            if ti.issym() or ti.islnk():
                link = ti.linkname
                if link.startswith("usr/"):
                    link = link[4:]
                add(out, link=link, mode=0o777)
                n_links += 1
                continue
            if not ti.isreg():
                continue
            data = src.extractfile(ti)
            blob = data.read()
            # ELF or a script runs; a data file does not
            mode = 0o755 if (blob[:4] == b"\x7fELF" or blob[:2] == b"#!") else 0o644
            add(out, data=io.BytesIO(blob), mode=mode, size=len(blob))
            n_files += 1
            base = os.path.basename(out)
            if out.startswith("lib/") and ".so." in base:
                libs.append((os.path.dirname(out), base))

        # soname links next to each versioned library
        for d, base in sorted(libs):
            for alias in sonames(base):
                add(d + "/" + alias, link=base, mode=0o777)
                n_links += 1

    h = hashlib.sha256()
    with open(args.out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    print("wrote %s" % args.out)
    print("  %d files, %d links (soname links included)" % (n_files, n_links))
    print("  %.1f MB, sha256 %s" % (os.path.getsize(args.out) / 1048576, h.hexdigest()))


if __name__ == "__main__":
    main()
