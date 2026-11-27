#!/usr/bin/env python3
"""Build the VLC package: launcher, player, libVLC closure, plugins, loader.

Two inputs are supported, and both produce the same bundle shape:

  --src <closure.tar.gz>   an Alpine closure laid out as usr/lib, usr/share/vlc,
  --extras <apk ...>       (optionally) alsa-lib's libasound and the audio
                           output plugins, remapped to lib/ and share/vlc/.

  --bundle <vlc.tar.gz>    a bundle that is already in the target shape (lib/,
                           plugins, etc). Everything is copied EXCEPT bin,
                           manifest, libexec/ and the bundled loader, which are
                           rebuilt from --bin, --player and --loader. This is
                           the "repackage with a new player" path; it needs no
                           network and reproduces the proven closure exactly.

The bundle the OS launches must be:

    bin                      STATIC launcher (manifest exec = bin)
    libexec/vlcplay          DYNAMIC player: PT_INTERP /lib/ld-musl-x86_64.so.1,
                             RUNPATH $ORIGIN/../lib
    lib/ld-musl-x86_64.so.1  the loader the launcher execs; musl's loader
                             understands --library-path and provides
                             libc.musl-x86_64.so.1, which libvlc needs
    lib/libvlc.so.5 -> ...   the soname symlinks the loader asks for
    lib/vlc/plugins/...      demuxers, decoders, the vmem output

Why the launcher cannot be the dynamic player: wmsrv chroots the app into a
jail whose binds are the payload at /bin/app, the bundle at /app, the socket,
fonts and /dev nodes - there is no /lib, and PT_INTERP is absolute, so a
dynamic exec target dies before main(). The static launcher execs the bundled
loader with --library-path instead.

The manifest says 'placeholder = no': src/apps/store.c treats 'yes' as "the
image's static placeholder, not the payload" and keeps offering the install.

Usage:
  python tools/pack-vlc.py --bundle build/repo/vlc.tar.gz \
                           --bin build/vlc-pack/bin \
                           --player build/vlc-pack/libexec/vlcplay \
                           --loader stage/lib/ld-musl-x86_64.so.1 \
                           --out build/repo/vlc.tar.gz

The output is written to <out>.part and renamed, so --bundle and --out may be
the same file.
"""

import argparse
import hashlib
import io
import os
import posixpath
import re
import struct
import sys
import tarfile

# Names this script owns; a --bundle input may not supply them.
CONTROLLED = ("bin", "manifest", "lib/ld-musl-x86_64.so.1")
CONTROLLED_PREFIX = ("libexec/",)
# provided by the loader itself, never members
MUSL_NAMES = {"libc.so", "libc.musl-x86_64.so.1", "ld-musl-x86_64.so.1"}


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


def elf_facts_bytes(d):
    """size, PT_INTERP and DT_NEEDED straight from ELF bytes."""
    if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2:
        return None
    phoff = struct.unpack_from("<Q", d, 0x20)[0]
    ents = struct.unpack_from("<H", d, 0x36)[0]
    n = struct.unpack_from("<H", d, 0x38)[0]
    # A truncated .so passes the magic check above and then made the program
    # table walk raise struct.error out of a build (the traceback came from
    # prune_broken_modules, not from anything the caller could act on). Treat
    # a header that points outside the file as "not an ELF we can read".
    if ents < 56 or phoff + n * ents > len(d):
        return None
    interp = None
    loads, dyn = [], None
    for i in range(n):
        off = phoff + i * ents
        t, _fl, o, va, _pa, fs, _ms, _al = struct.unpack_from("<IIQQQQQQ", d, off)
        if t == 1:
            loads.append((va, o, fs))
        elif t == 2:
            dyn = (o, fs)
        elif t == 3:
            interp = d[o:o + fs].rstrip(b"\x00").decode("ascii", "replace")
    needed = []
    if dyn:
        entries = []
        # The dynamic table is sized by DT_* entries that the file may not
        # actually contain; stop at the end of the buffer.
        for j in range(dyn[1] // 16):
            if dyn[0] + (j + 1) * 16 > len(d):
                break
            tag, val = struct.unpack_from("<QQ", d, dyn[0] + j * 16)
            entries.append((tag, val))
            if tag == 0:
                break
        sv = [v for t, v in entries if t == 5]
        if sv:
            so = next((o + (sv[0] - va) for va, o, fs in loads
                       if va <= sv[0] < va + fs), None)
            if so is not None:
                for t, v in entries:
                    if t != 1:
                        continue
                    end = d.find(b"\x00", so + v)
                    if end < 0:          # NEEDED name runs past the buffer
                        continue
                    needed.append(d[so + v:end].decode("ascii", "replace"))
    return {"size": len(d), "interp": interp, "needed": needed}


def elf_facts(path):
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    return elf_facts_bytes(d)


def prune_broken_modules(members, read):
    """Names of lib/vlc modules whose DT_NEEDED cannot be satisfied.

    A module that needs a library the bundle does not carry can only fail to
    load ("libpulse.so.0: No such file or directory"), and VLC logs it on
    every start. The inherited closure carried libpulse_plugin.so,
    libjack_plugin.so and libvlc_pulse.so with neither libpulse nor libjack
    in lib/, so they are dropped here instead of shipped broken. A module
    that needs a dropped module goes too (the loop), and a symlink to a
    dropped member goes with it.

    members maps name -> None for a regular file, or the symlink target for a
    symlink. read(name) returns the bytes of a regular member.
    """
    provided = set(members)
    needs = {}
    for name in members:
        if not name.startswith("lib/vlc/"):
            continue
        facts = elf_facts_bytes(read(name))
        if facts:
            needs[name] = facts["needed"]
    drop = set()
    while True:
        more = set()
        for name, need in needs.items():
            if name in drop:
                continue
            for n in need:
                if n in MUSL_NAMES:
                    continue
                # NEEDED names are sonames; the bundle keeps them under lib/
                path = n if n.startswith("lib/") else "lib/" + n
                if path not in provided or path in drop:
                    more.add(name)
                    break
        more -= drop
        if not more:
            break
        drop |= more
    for name, link in members.items():
        if name in drop or link is None:
            continue
        if link.startswith("/"):
            resolved = link.lstrip("/")
        else:
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), link))
        if resolved in drop:
            drop.add(name)
    return drop


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--src", default=os.path.join(here, "build", "vlc-orig.tar.gz"))
    ap.add_argument("--bundle", default=None,
                    help="a previous vlc.tar.gz in bundle shape; copies its "
                         "closure instead of --src")
    ap.add_argument("--bin", default=os.path.join(here, "build", "vlc-pack", "bin"),
                    help="the STATIC launcher to ship as bin")
    ap.add_argument("--player", default=None,
                    help="the DYNAMIC player to ship as libexec/vlcplay")
    ap.add_argument("--loader", default=None,
                    help="musl's ld-musl-x86_64.so.1 to ship at "
                         "lib/ld-musl-x86_64.so.1")
    ap.add_argument("--out", default=os.path.join(here, "build", "repo", "vlc.tar.gz"))
    ap.add_argument("--version", default="3.0.23")
    ap.add_argument("--extras", nargs="*", default=[],
                    help="packages whose audio plugins and libasound are added: "
                         "the upstream vlc package carries libalsa_plugin.so, and "
                         "alsa-lib carries libasound.so.2")
    args = ap.parse_args()

    if not args.bundle and not os.path.exists(args.src):
        sys.exit("missing %s (or pass --bundle)" % args.src)
    if not os.path.exists(args.bin):
        sys.exit("missing %s (the static launcher)" % args.bin)
    if args.player and not os.path.exists(args.player):
        sys.exit("missing %s (the dynamic player)" % args.player)
    if args.loader and not os.path.exists(args.loader):
        sys.exit("missing %s (the musl loader)" % args.loader)

    for name, path in (("bin", args.bin), ("libexec/vlcplay", args.player)):
        if not path:
            continue
        f = elf_facts(path)
        if f is None:
            sys.exit("%s is not an ELF64 file" % path)
        if name == "bin" and f["interp"] is not None:
            sys.exit("bin must be STATIC (it is the manifest's exec inside a "
                     "jail with no /lib) but %s has PT_INTERP %s"
                     % (path, f["interp"]))
        if name == "libexec/vlcplay" and f["interp"] is None:
            sys.exit("%s is static; the player must be dynamic so dlopen works"
                     % path)

    manifest = (
        "name = VLC Player\n"
        "desc = Video and audio player (libVLC), decoded in the bundle and drawn in its own window\n"
        "exec = bin\n"
        "version = %s\n"
        "network = host\n"
        "audio = yes\n"
        "bundle = vlc\n"
        "placeholder = no\n" % args.version
    )

    want_prefixes = ("usr/lib/", "usr/share/vlc/", "etc/")
    n_files = n_links = n_skipped = n_pruned = 0
    libs = []
    seen = set()
    out_tmp = args.out + ".part"

    with tarfile.open(out_tmp, "w:gz") as dst:
        def add(name, data=None, link=None, mode=0o644, size=0):
            """Add one member unless the name is already taken (dedupe)."""
            nonlocal n_files, n_links, n_skipped
            if name in seen:
                n_skipped += 1
                return False
            seen.add(name)
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            if link is not None:
                ti.type = tarfile.SYMTYPE
                ti.linkname = link
                dst.addfile(ti)
                n_links += 1
            else:
                ti.size = size
                dst.addfile(ti, data)
                n_files += 1
            return True

        def add_file(name, path, mode=0o644):
            st = os.stat(path)
            with open(path, "rb") as f:
                ti = tarfile.TarInfo(name)
                ti.size = st.st_size
                ti.mode = mode
                ti.mtime = int(st.st_mtime)
                if add(name, data=f, mode=mode, size=st.st_size):
                    return True
            return False

        # ---- the pieces this script owns -----------------------------
        add_file("bin", args.bin, 0o755)
        add("manifest", io.BytesIO(manifest.encode()), size=len(manifest))
        if args.player:
            add_file("libexec/vlcplay", args.player, 0o755)
        if args.loader:
            add_file("lib/ld-musl-x86_64.so.1", args.loader, 0o755)

        # ---- the closure ---------------------------------------------
        drop = set()
        if args.bundle:
            # Pre-pass: find lib/vlc modules whose DT_NEEDED the bundle cannot
            # provide, so they are not shipped as modules that can only fail.
            members, blobs = {}, {}
            with tarfile.open(args.bundle, "r:*") as pre:
                for ti in pre:
                    name = ti.name.lstrip("./")
                    if not name or name in CONTROLLED:
                        continue
                    if any(name.startswith(p) for p in CONTROLLED_PREFIX):
                        continue
                    if ti.issym() or ti.islnk():
                        members[name] = ti.linkname.lstrip("./")
                    elif ti.isreg():
                        members[name] = None
                        if name.startswith("lib/vlc/"):
                            blobs[name] = pre.extractfile(ti).read()
            drop = prune_broken_modules(members, lambda n: blobs[n])
            if drop:
                print("pruned %d module(s) whose DT_NEEDED is not in the "
                      "bundle:" % len(drop))
                for n in sorted(drop):
                    print("    " + n)

        if args.bundle:
            with tarfile.open(args.bundle, "r:*") as src:
                for ti in src:
                    name = ti.name.lstrip("./")
                    if not name or name in CONTROLLED:
                        continue
                    if any(name.startswith(p) for p in CONTROLLED_PREFIX):
                        continue
                    if name in drop:
                        n_pruned += 1
                        continue
                    if ti.isdir():
                        continue
                    if ti.issym() or ti.islnk():
                        link = ti.linkname
                        if link.startswith("./"):
                            link = link[2:]
                        add(name, link=link, mode=0o777)
                        base = os.path.basename(name)
                        if name.startswith("lib/") and ".so." in base:
                            libs.append((os.path.dirname(name), base))
                        continue
                    if not ti.isreg():
                        continue
                    blob = src.extractfile(ti).read()
                    add(name, data=io.BytesIO(blob), mode=ti.mode,
                        size=len(blob))
                    base = os.path.basename(name)
                    if name.startswith("lib/") and ".so." in base:
                        libs.append((os.path.dirname(name), base))
        else:
            with tarfile.open(args.src, "r:*") as src:
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
                        base = os.path.basename(out)
                        if out.startswith("lib/") and ".so." in base:
                            libs.append((os.path.dirname(out), base))
                        continue
                    if not ti.isreg():
                        continue
                    blob = src.extractfile(ti).read()
                    # ELF or a script runs; a data file does not
                    mode = 0o755 if (blob[:4] == b"\x7fELF" or blob[:2] == b"#!") else 0o644
                    add(out, data=io.BytesIO(blob), mode=mode, size=len(blob))
                    base = os.path.basename(out)
                    if out.startswith("lib/") and ".so." in base:
                        libs.append((os.path.dirname(out), base))

            # ---- audio: the output plugin and libasound ------------------
            # The closure this package was first built from had only the dummy
            # output, so the player honestly reported "no audio output". These
            # packages are the ones that provide a real one.
            for apk in args.extras:
                if not os.path.exists(apk):
                    sys.exit("missing extra package %s" % apk)
                with tarfile.open(apk, "r:*") as ex:
                    for ti in ex:
                        name = ti.name.lstrip("./")
                        keep = (name.startswith("usr/lib/vlc/plugins/audio_output/")
                                or re.match(r"usr/lib/libasound\.so[.0-9]*$", name) is not None)
                        if not keep or not ti.isreg():
                            continue
                        out = name[len("usr/"):] if name.startswith("usr/") else name
                        blob = ex.extractfile(ti).read()
                        mode = 0o755 if blob[:4] == b"\x7fELF" else 0o644
                        add(out, data=io.BytesIO(blob), mode=mode, size=len(blob))
                        base = os.path.basename(out)
                        if ".so." in base:
                            libs.append((os.path.dirname(out), base))

        # ---- soname links next to each versioned library --------------
        # A --bundle input usually carries them already; add() dedupes, so a
        # library that lost its alias is repaired and an existing one is not
        # emitted twice (the old writer emitted duplicates, 620 members for
        # 274 files).
        for d, base in sorted(set(libs)):
            for alias in sonames(base):
                add(d + "/" + alias, link=base, mode=0o777)

    os.replace(out_tmp, args.out)

    h = hashlib.sha256()
    with open(args.out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)

    print("wrote %s" % args.out)
    print("  %d files, %d links, %d duplicate members skipped, %d broken modules pruned"
          % (n_files, n_links, n_skipped, n_pruned))
    print("  %.1f MB, sha256 %s"
          % (os.path.getsize(args.out) / 1048576, h.hexdigest()))
    b = elf_facts(args.bin)
    print("  bin: %d bytes, PT_INTERP %s, NEEDED %s"
          % (b["size"], b["interp"] or "(none: static)",
             b["needed"] or "(none)"))
    if args.player:
        p = elf_facts(args.player)
        print("  libexec/vlcplay: %d bytes, PT_INTERP %s, NEEDED %s"
              % (p["size"], p["interp"] or "(none)", p["needed"] or "(none)"))
    if args.loader:
        print("  lib/ld-musl-x86_64.so.1: %d bytes" % os.path.getsize(args.loader))


if __name__ == "__main__":
    main()
