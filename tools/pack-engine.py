#!/usr/bin/env python3
"""Assemble the "engine" package: the browser viewer plus the Chromium closure.

The viewer (src/client/apps/engine.c) is a DevTools client that must decode a
screencast JPEG per frame with the bundle's libjpeg-turbo instead of the
scalar decoder in src/img.c. That makes it a DYNAMIC musl program (only a
dynamic one can load a .so; a static musl binary has no dynamic loader at
all), and a dynamic program cannot be the manifest's `exec` in the wmsrv jail,
which has no /lib and resolves PT_INTERP before main() runs.

So the package ships the pair the VLC package already uses:

    bin                       STATIC launcher, the manifest's exec
                              (src/client/apps/engine_launch.c)
                              execv's <bundle>/ld-musl-x86_64.so.1
                              --library-path <bundle>/usr/lib:<bundle>/lib
                              <bundle>/libexec/engine <args...>
    libexec/engine            the viewer, DYNAMIC musl, PT_INTERP
                              /app/ld-musl-x86_64.so.1, DT_NEEDED libjpeg.so.8
    ld-musl-x86_64.so.1       the loader at the bundle root
    usr/lib/...               the Chromium closure, libjpeg.so.8.3.2 included
    manifest                  copied from the stage bundle with ONE change:
                              'placeholder = yes' becomes 'placeholder = no'.
                              src/apps/store.c parses that key (:541), refuses
                              the bundle in pkg_in_root() (:890) and returns
                              ACT_INSTALL from install_action() (:1690), so a
                              packaged manifest that still said "yes" would
                              keep offering "install" for a package that is
                              installed - and Enter would fetch 264 MB again.
                              The image's copy keeps "yes": it IS a stand-in.

Version identity is one string, taken from the manifest: the Store compares
manifest 'version' with the index row (src/apps/store.c:539), and nothing in
src/ reads .pkg-version (grep: 0 hits). The packaged .pkg-version and
build/repo/engine.row therefore carry the manifest's version verbatim.

Everything is built here, from source, in one command:

    python tools/pack-engine.py                # build both, then pack
    python tools/pack-engine.py --no-build     # pack what build/engine-bundle has

Before anything is packed, the two ELFs are read back and the assertions the
design depends on are checked (see check_elf): a static launcher with no
PT_INTERP and no DT_NEEDED, a viewer with the right interpreter and libjpeg
in DT_NEEDED. After the tarball is written it is REOPENED and the same checks
run on the members that will actually be installed, so a packaging mistake
cannot pass by inspecting the wrong file.

The libjpeg the viewer links against is extracted from the Chromium tarball
itself (usr/lib/libjpeg.so.8.3.2 -> build/engine-libs/libjpeg.so), which is
exactly the copy the package ships: the link cannot pick up some host library
that would not exist in the jail.

Usage:
  python tools/pack-engine.py --chromium build/repo/chromium.tar.gz \
                              --bundle stage/usr/share/mystic/apps/engine \
                              --out build/repo/engine.tar.gz
"""

import argparse
import glob
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time

# The bundle keeps its payload at the root as a *file* named bin, and the
# Chromium closure carries bin/sh (Alpine's yash). tar cannot create a
# directory where a file of that name exists, so the whole closure bin/ tree
# is dropped: nothing in it is needed to run the engine, which lives under
# usr/lib. The .pkg-* and PKGINFO files at the closure root would give the
# package two identities, so they are dropped too.
SKIP_AT_ROOT = {"chromium", "manifest", ".pkg-name", ".pkg-version", "bin",
                "PKGINFO", "SIGN.RSA.alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub",
                "post-deinstall", "post-install", "pre-deinstall", "trigger"}

# Where the jail mounts the bundle (src/wmsrv.c binds it read-only at /app),
# and therefore what PT_INTERP has to name for a direct exec to work. The
# launcher never relies on this - it execs the loader by path - but Chromium
# re-execs /proc/self/exe for its zygote and renderer children, and those
# children are started by the kernel through PT_INTERP.
APP_ROOT = "/app"
APP_INTERP = APP_ROOT + "/ld-musl-x86_64.so.1"

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

APP_SRC = os.path.join("src", "client", "apps", "engine.c")
LAUNCH_SRC = os.path.join("src", "client", "apps", "engine_launch.c")
APP_DEPS = [os.path.join("src", "client", "mystic_client.c"),
            os.path.join("src", "gfx.c"),
            os.path.join("src", "font.c"),
            os.path.join("src", "ui.c"),
            os.path.join("src", "filetypes.c"),
            os.path.join("src", "img.c"),
            os.path.join("src", "audio.c"),
            os.path.join("src", "client", "cdpws.c")]


# ---------------------------------------------------------------- ELF reader

def elf_full(blob):
    """Everything the packaging decisions depend on, read from the bytes.

    size, e_type (2 = executable, 3 = shared object), PT_INTERP, DT_NEEDED,
    DT_RPATH/DT_RUNPATH, DT_SONAME and the entry point. Shared with the tar
    members by passing their bytes, so the check can run on the file on disk
    AND on what the archive really contains.
    """
    out = {"size": 0, "e_type": 0, "interp": "", "needed": [], "rpath": [],
           "soname": "", "entry": 0}
    b = blob
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


def dyn_symbols(blob):
    """(defined, strong-undefined, weak-undefined) names from .dynsym.

    Weak undefined symbols may legitimately stay unresolved (the loader skips
    them), so they are counted apart from the ones that must be found.
    """
    le = "<"
    defined, undefined, weak = set(), set(), set()
    if blob[:4] != b"\x7fELF" or blob[4] != 2:
        return defined, undefined, weak
    shoff, = struct.unpack_from(le + "Q", blob, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", blob, 0x3a)

    def sh(i):
        return struct.unpack_from(le + "IIQQQQIIQQ", blob, shoff + i * shentsize)

    base = sh(shstrndx)[4]

    def name_at(o):
        return blob[o:blob.index(b"\0", o)].decode("utf-8", "replace")

    dynsym = dstr = None
    for i in range(shnum):
        s = sh(i)
        n = name_at(base + s[0])
        if n == ".dynsym":
            dynsym = s
        elif n == ".dynstr":
            dstr = s[4]
    if dynsym is None or dstr is None:
        return defined, undefined, weak
    off, end = dynsym[4], dynsym[4] + dynsym[5]
    while off + 24 <= end:
        nm, info, _other, shndx, _val, _size = struct.unpack_from(
            le + "IBBHQQ", blob, off)
        off += 24
        if not nm:
            continue
        name = name_at(dstr + nm)
        if shndx:
            defined.add(name)
        elif (info >> 4) == 2:          # STB_WEAK
            weak.add(name)
        else:
            undefined.add(name)
    return defined, undefined, weak


def check_closure(viewer_blob, loader_blob, libjpeg_blob):
    """Every non-jpeg import must be exported by the loader (musl = libc) or
    by the packaged libjpeg.

    The viewer is linked against zig's musl sysroot, the loader comes from the
    image's stage/lib. If those two are different musl versions, a symbol the
    viewer calls may simply not exist in the loader, and the failure is a load
    time 'Error relocating ...: symbol not found' - invisible to every check
    that only looks at DT_NEEDED. This compares the symbol tables instead.
    """
    _, undef, _ = dyn_symbols(viewer_blob)
    have = set()
    for blob in (loader_blob, libjpeg_blob):
        d, _, _ = dyn_symbols(blob)
        have |= d
    missing = sorted(n for n in undef if n not in have)
    if missing:
        raise SystemExit("the viewer imports symbols nothing in the bundle "
                         "defines: %s" % ", ".join(missing[:12]))
    return len(undef)


def check_symbols(viewer_blob, libjpeg_blob):
    """Every jpeg_* the viewer imports must be exported by the packaged .so.

    This is the check a hand-declared ABI header needs: a prototype that
    names a function the library does not have links only because the linker
    is told to allow undefined symbols in a shared object's consumer - the
    failure would appear at load time as 'Error relocating ...: symbol not
    found'. Compare the symbol tables instead of trusting the header.
    """
    _, undef, _ = dyn_symbols(viewer_blob)
    defined, _, _ = dyn_symbols(libjpeg_blob)
    need = sorted(n for n in undef if n.startswith("jpeg_"))
    missing = [n for n in need if n not in defined]
    if missing:
        raise SystemExit("the viewer imports %s but the packaged libjpeg does "
                         "not export it" % ", ".join(missing))
    if not need:
        raise SystemExit("the viewer imports no jpeg_* symbol: it was built "
                         "without HAVE_TURBOJPEG")
    return need


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def check_elf(blob, kind, what):
    """Assert the file is the kind of ELF this package can ship.

    kind "static"  : e_type 2, no PT_INTERP, no DT_NEEDED (the launcher)
    kind "viewer"  : e_type 2, PT_INTERP APP_INTERP, libjpeg.so.8 in NEEDED,
                     a libc in NEEDED, and a $ORIGIN RUNPATH
    kind "loader"  : e_type 3, SONAME libc.musl-x86_64.so.1, entry point set
    """
    got = elf_full(blob)
    if kind in ("static", "viewer") and got["e_type"] != 2:
        raise SystemExit("%s is not an executable ELF (e_type=%d)"
                         % (what, got["e_type"]))
    if kind == "static":
        if got["interp"]:
            raise SystemExit("%s has PT_INTERP %s; a static launcher must not "
                             "depend on a loader" % (what, got["interp"]))
        if got["needed"]:
            raise SystemExit("%s has DT_NEEDED %s; a static launcher must not"
                             % (what, ", ".join(got["needed"])))
    elif kind == "viewer":
        if got["interp"] != APP_INTERP:
            raise SystemExit("%s has PT_INTERP %r, expected %r"
                             % (what, got["interp"], APP_INTERP))
        if "libjpeg.so.8" not in got["needed"]:
            raise SystemExit("%s does not need libjpeg.so.8 (NEEDED %s): the "
                             "viewer would fall back to the scalar decoder"
                             % (what, ", ".join(got["needed"])))
        if not any(n == "libc.so" or n.startswith("libc.")
                   for n in got["needed"]):
            raise SystemExit("%s does not need a libc (%s)"
                             % (what, ", ".join(got["needed"])))
        if not any("$ORIGIN" in rp for rp in got["rpath"]):
            raise SystemExit("%s has no $ORIGIN RUNPATH (got %r)"
                             % (what, got["rpath"]))
    elif kind == "loader":
        if got["e_type"] != 3:
            raise SystemExit("%s is not a shared object (e_type=%d)"
                             % (what, got["e_type"]))
        if got["soname"] != "libc.musl-x86_64.so.1":
            raise SystemExit("%s has SONAME %r, expected libc.musl-x86_64.so.1"
                             % (what, got["soname"]))
        if got["entry"] == 0:
            raise SystemExit("%s has no entry point, so it cannot be exec'd"
                             % what)
    return got


def write_pt_interp(path, want):
    """Make PT_INTERP say `want`, in place, and prove it afterwards.

    The compile command already asks the linker for APP_INTERP. This is here
    because that argument is an absolute POSIX path, and MSYS/git-bash
    rewrites those before zig sees them: vlcplay.c documents the same trap,
    where PT_INTERP came out as C:/Users/.../lib/ld-musl-x86_64.so.1. A
    Python subprocess does not go through that translation, but the packer
    should not depend on which shell started it. The rewrite is legal because
    PT_INTERP's p_filesz is just the string length and the new name is shorter
    than the ones this has to fix.
    """
    b = bytearray(read_file(path))
    if b[:4] != b"\x7fELF" or b[4] != 2:
        raise SystemExit("%s is not a 64-bit ELF" % path)
    phoff, = struct.unpack_from("<Q", b, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", b, 0x36)
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", b, off)[0] != 3:
            continue
        p_offset, = struct.unpack_from("<Q", b, off + 8)
        p_filesz, = struct.unpack_from("<Q", b, off + 32)
        cur = bytes(b[p_offset:p_offset + p_filesz]).split(b"\0")[0].decode(
            "utf-8", "replace")
        if cur == want:
            return cur
        need = len(want.encode()) + 1
        if need > p_filesz:
            raise SystemExit("%s: PT_INTERP %r is %d bytes, too short for %r"
                             % (path, cur, p_filesz, want))
        b[p_offset:p_offset + need - 1] = want.encode()
        b[p_offset + need - 1] = 0
        struct.pack_into("<Q", b, off + 32, need)
        with open(path, "wb") as f:
            f.write(b)
        print("interp: %r -> %r (%s)" % (cur, want, path))
        return cur
    raise SystemExit("%s has no PT_INTERP; it cannot be a dynamic payload"
                     % path)


def retarget_interp(blob, want):
    """Same as write_pt_interp, for a member that only exists in memory."""
    if blob[:4] != b"\x7fELF" or blob[4] != 2:
        return blob, None
    data = bytearray(blob)
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", data, off)[0] != 3:
            continue
        start, = struct.unpack_from("<Q", data, off + 8)
        size, = struct.unpack_from("<Q", data, off + 32)
        cur = bytes(data[start:start + size]).split(b"\0")[0].decode()
        if cur == want:
            return bytes(data), cur
        need = len(want.encode()) + 1
        if need > size:
            raise SystemExit("cannot retarget %r to %r: the slot is %d bytes"
                             % (cur, want, size))
        data[start:start + need - 1] = want.encode()
        data[start + need - 1] = 0
        struct.pack_into("<Q", data, off + 32, need)
        return bytes(data), cur
    return blob, None


# ------------------------------------------------------------------- building

def find_zig():
    z = os.environ.get("ZIG")
    if z:
        return z
    for name in ("zig", "zig.exe"):
        p = shutil.which(name)
        if p:
            return p
    for cand in glob.glob(os.path.expanduser(
            "~/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/"
            "zig-x86_64-windows-*/zig.exe")):
        return cand
    return None


def extract_libjpeg(chromium, libdir):
    """Pull the exact libjpeg the package ships into a linkable directory.

    The closure member is usr/lib/libjpeg.so.8.3.2 (SONAME libjpeg.so.8). The
    linker wants a file named libjpeg.so, so it is written under that name;
    DT_NEEDED still records the SONAME. Returns (path, elf_info, member).
    """
    os.makedirs(libdir, exist_ok=True)
    out = os.path.join(libdir, "libjpeg.so")
    member = "usr/lib/libjpeg.so.8.3.2"
    with tarfile.open(chromium, "r:gz") as t:
        ti = None
        for cand in (member, "./" + member):
            try:
                ti = t.getmember(cand)
                break
            except KeyError:
                pass
        if ti is None:
            raise SystemExit("%s has no %s: the package would carry no JPEG "
                             "decoder" % (chromium, member))
        blob = t.extractfile(ti).read()
    with open(out, "wb") as f:
        f.write(blob)
    info = elf_full(blob)
    if info["soname"] != "libjpeg.so.8":
        raise SystemExit("%s has SONAME %r, expected libjpeg.so.8"
                         % (member, info["soname"]))
    return out, info, len(blob)


def build(zig, launcher_out, viewer_out, libdir, chromium):
    """Compile the static launcher and the dynamic viewer.

    The launcher takes no source but its own file: it links no SDK object and
    no libjpeg, which is what keeps it startable in a jail with no /lib.
    """
    for p in [APP_SRC, LAUNCH_SRC] + APP_DEPS:
        if not os.path.exists(p):
            raise SystemExit("error: missing source %s" % p)

    os.makedirs(os.path.dirname(launcher_out), exist_ok=True)
    os.makedirs(os.path.dirname(viewer_out), exist_ok=True)
    libjpeg, jinfo, jsize = extract_libjpeg(chromium, libdir)
    print("==> libjpeg for the link: %s (%d bytes, SONAME %s)"
          % (libjpeg, jsize, jinfo["soname"]))

    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os", "-static", "-s",
           "-Isrc", "-Isrc/client", "-o", launcher_out, LAUNCH_SRC]
    print("==> compiling the STATIC launcher: %s" % launcher_out)
    if subprocess.run(cmd).returncode != 0:
        raise SystemExit("zig cc failed for the launcher")
    linfo = check_elf(read_file(launcher_out), "static", "the launcher")
    print("    e_type=%d interp=%r needed=%s"
          % (linfo["e_type"], linfo["interp"], linfo["needed"]))

    # -dynamic is not optional: zig's musl target defaults to a static link,
    # and a static musl program has no dynamic loader to map libjpeg with.
    # The interpreter and RUNPATH name the jail's paths, not the host's.
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-O2", "-dynamic",
           "-DHAVE_TURBOJPEG",
           "-Wl,-rpath,$ORIGIN/../usr/lib:$ORIGIN/../lib",
           "-Wl,-dynamic-linker=" + APP_INTERP,
           "-Isrc", "-Isrc/client", "-Isrc/client/apps",
           "-o", viewer_out, APP_SRC] + APP_DEPS + \
          ["-L" + libdir, "-ljpeg", "-lm"]
    print("==> compiling the DYNAMIC viewer: %s" % viewer_out)
    if subprocess.run(cmd).returncode != 0:
        raise SystemExit("zig cc failed for the viewer")
    old = write_pt_interp(viewer_out, APP_INTERP)
    vinfo = check_elf(read_file(viewer_out), "viewer", "the viewer")
    print("    e_type=%d interp=%r needed=%s rpath=%s"
          % (vinfo["e_type"], vinfo["interp"], vinfo["needed"],
             vinfo["rpath"]))
    if old and old != APP_INTERP:
        print("    (the linker's PT_INTERP was %r and was rewritten)" % old)
    return linfo, vinfo


# -------------------------------------------------------------------- packing

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chromium", default=os.path.join(HERE, "build", "repo",
                                                       "chromium.tar.gz"))
    ap.add_argument("--bundle", default=os.path.join(HERE, "stage", "usr",
                                                     "share", "mystic", "apps",
                                                     "engine"))
    ap.add_argument("--out", default=os.path.join(HERE, "build", "repo",
                                                  "engine.tar.gz"))
    ap.add_argument("--build-dir", default=os.path.join(HERE, "build",
                                                        "engine-bundle"))
    ap.add_argument("--libs", default=os.path.join(HERE, "build",
                                                   "engine-libs"))
    ap.add_argument("--version", default="156.0.8078.79",
                    help="fallback identity when the manifest has no "
                         "'version =' line; the manifest is authoritative")
    ap.add_argument("--no-build", action="store_true",
                    help="pack the binaries already in --build-dir")
    ap.add_argument("--no-row", action="store_true",
                    help="do not write build/repo/engine.row")
    args = ap.parse_args()

    if not os.path.exists(args.chromium):
        sys.exit("missing %s (build it with tools/fetch-chromium.py)"
                 % args.chromium)
    manifest = os.path.join(args.bundle, "manifest")
    if not os.path.exists(manifest):
        sys.exit("missing %s (build it with tools/mkapp.sh engine)" % manifest)
    with open(manifest, "r") as f:
        mtext = f.read()

    def manifest_value(key):
        for line in mtext.splitlines():
            k, _, v = line.partition("=")
            if k.strip() == key:
                return v.strip()
        return ""

    if manifest_value("exec") != "bin":
        sys.exit("%s says 'exec = %s'; the package's payload is the static "
                 "launcher at bin" % (manifest, manifest_value("exec")))

    # Version identity: the manifest is the only version src/ reads. The Store
    # parses manifest 'version' (src/apps/store.c:539) and compares it with the
    # index row; nothing in src/ reads .pkg-version (grep: 0 hits). One string
    # for the manifest, .pkg-version and the engine.row, taken from the
    # manifest itself so they cannot drift.
    pkg_version = manifest_value("version") or args.version

    # The image's copy of this bundle is a placeholder and its manifest says
    # so. The INSTALLED package is not: its bin is a working launcher. That
    # word is not decoration - src/apps/store.c parses 'placeholder' (:541),
    # rejects the bundle in pkg_in_root() (:890) and returns ACT_INSTALL from
    # install_action() (:1690) - so a packaged manifest that still said "yes"
    # would make a successful install keep offering "install", and Enter
    # would re-request the whole 264 MB tarball. Rewrite the member, leave the
    # image's copy alone, and assert the archive says no (below).
    lines = []
    seen_placeholder = False
    for line in mtext.splitlines():
        k, _, v = line.partition("=")
        if k.strip() == "placeholder":
            lines.append("placeholder = no")
            seen_placeholder = True
        else:
            lines.append(line)
    if not seen_placeholder:
        lines.append("placeholder = no")
    pkg_manifest = "\n".join(lines).rstrip("\n") + "\n"

    launcher = os.path.join(args.build_dir, "bin")
    viewer = os.path.join(args.build_dir, "libexec", "engine")
    if args.no_build:
        for p in (launcher, viewer):
            if not os.path.exists(p):
                sys.exit("--no-build but %s does not exist" % p)
        check_elf(read_file(launcher), "static", "the launcher")
        vinfo = check_elf(read_file(viewer), "viewer", "the viewer")
    else:
        zig = find_zig()
        if not zig:
            sys.exit("error: zig not found (set ZIG=)")
        _, vinfo = build(zig, launcher, viewer, args.libs, args.chromium)

    loader_src = os.path.join(HERE, "stage", "lib", "ld-musl-x86_64.so.1")
    if not os.path.exists(loader_src):
        sys.exit("missing %s (the image ships it)" % loader_src)
    check_elf(read_file(loader_src), "loader", "the musl loader")

    t0 = time.time()
    n_files = n_links = 0
    skipped = 0
    flat_added = []

    def add_blob(dst, name, blob, mode):
        ti = tarfile.TarInfo(name)
        ti.size = len(blob)
        ti.mode = mode
        ti.mtime = int(time.time())
        dst.addfile(ti, io.BytesIO(blob))

    def add_path(dst, name, path):
        st = os.stat(path)
        with open(path, "rb") as f:
            blob = f.read()
        mode = 0o755 if (blob[:4] == b"\x7fELF" or blob[:2] == b"#!") else 0o644
        ti = tarfile.TarInfo(name)
        ti.size = st.st_size
        ti.mode = mode
        ti.mtime = int(st.st_mtime)
        dst.addfile(ti, io.BytesIO(blob))
        return blob

    with tarfile.open(args.chromium, "r:gz") as src, \
            tarfile.open(args.out, "w:gz") as dst:

        # the launcher is the manifest's exec, the viewer is what it starts
        add_path(dst, "bin", launcher)
        add_path(dst, "libexec/engine", viewer)
        add_blob(dst, "manifest", pkg_manifest.encode(), 0o644)
        n_files += 3
        print("manifest: placeholder %r (image) -> %r (package)"
              % (manifest_value("placeholder"), "no"))

        members = set()
        for m in src.getmembers():
            n = m.name
            while n.startswith("./"):
                n = n[2:]
            members.add(n)
        for ti in src:
            # ".pkg-name" must be skipped as written: lstrip("./") also eats
            # the leading dot, which is why this used to skip nothing.
            rawname = ti.name
            name = rawname
            while name.startswith("./"):
                name = name[2:]
            if not name or name.split("/")[0] in SKIP_AT_ROOT:
                skipped += 1
                continue
            if ti.isreg() and name == "usr/lib/chromium/chromium-headless-shell":
                blob, old = retarget_interp(src.extractfile(ti).read(), APP_INTERP)
                ti.size = len(blob)
                dst.addfile(ti, io.BytesIO(blob))
                print("interp: %r -> %r (chromium-headless-shell)"
                      % (old, APP_INTERP))
                n_files += 1
                continue
            blob = src.extractfile(ti).read() if ti.isreg() else None
            dst.addfile(ti, io.BytesIO(blob) if blob is not None else None)
            if ti.issym() or ti.islnk():
                n_links += 1
            else:
                n_files += 1
            # Libraries in private subdirectories (libpulsecommon under
            # usr/lib/pulseaudio, the dri/dri drivers, gio modules) are found
            # only if the loader is told about that exact directory. A flat
            # copy in usr/lib makes the closure loadable with a single -L,
            # which is what the jail needs: a missing libpulsecommon is
            # reported as 'Error relocating libpulse.so.0: pa_strbuf_puts:
            # symbol not found'.
            parts = name.split("/")
            if (blob is not None and len(parts) >= 4 and parts[0] == "usr"
                    and parts[1] == "lib" and ".so" in parts[-1]):
                flat = "usr/lib/" + parts[-1]
                if flat not in members and flat not in flat_added:
                    flat_added.append(flat)
                    add_blob(dst, flat, blob,
                             0o755 if blob[:4] == b"\x7fELF" else 0o644)
                    n_files += 1

        # The loader lives at the bundle root: that is the path the viewer's
        # and Chromium's PT_INTERP name, and the launcher execs it directly.
        add_path(dst, "ld-musl-x86_64.so.1", loader_src)
        # empty marker: sends the engine child's stderr to the console
        add_blob(dst, "engine-log", b"", 0o644)
        n_files += 2

        for extra, text in ((".pkg-name", "engine\n"),
                            (".pkg-version", pkg_version + "\n")):
            add_blob(dst, extra, text.encode(), 0o644)
            n_files += 1

    # ---- read the finished archive back: this is what will be installed ----
    print("\n==> verifying the members that were actually written")
    with tarfile.open(args.out, "r:gz") as t:
        names = {}
        installed = 0
        for ti in t.getmembers():
            names[ti.name] = ti
            if ti.isreg():
                installed += ti.size
        for need in ("bin", "libexec/engine", "manifest",
                     "ld-musl-x86_64.so.1", "usr/lib/libjpeg.so.8.3.2"):
            if need not in names:
                raise SystemExit("the archive has no %s" % need)
        again_linfo = check_elf(t.extractfile(names["bin"]).read(),
                                "static", "bin in the archive")
        again_vinfo = check_elf(t.extractfile(names["libexec/engine"]).read(),
                               "viewer", "libexec/engine in the archive")
        check_elf(t.extractfile(names["ld-musl-x86_64.so.1"]).read(),
                  "loader", "ld-musl-x86_64.so.1 in the archive")
        # The manifest is what the Store parses after an install: exec must be
        # the launcher and placeholder must be no, or the Store keeps saying
        # "install" for a package that IS installed (src/apps/store.c:541,
        # 890, 1690). Check the bytes in the archive, not the stage file.
        man = t.extractfile(names["manifest"]).read().decode().splitlines()
        mkeys = {}
        for line in man:
            k, _, v = line.partition("=")
            mkeys[k.strip()] = v.strip()
        if mkeys.get("exec") != "bin":
            raise SystemExit("the packaged manifest says exec = %r, not bin"
                             % mkeys.get("exec"))
        if mkeys.get("placeholder") != "no":
            raise SystemExit("the packaged manifest says placeholder = %r: the "
                             "Store would keep offering install for an "
                             "installed package (store.c:1690)"
                             % mkeys.get("placeholder"))
        if mkeys.get("version") != pkg_version:
            raise SystemExit("the packaged manifest says version = %r but the "
                             "package identity is %r"
                             % (mkeys.get("version"), pkg_version))
        if "version" not in mkeys:
            raise SystemExit("the packaged manifest has no version line; the "
                             "Store compares it with the index row")
        print("    manifest         exec = %s, version = %s, placeholder = %s"
              % (mkeys.get("exec"), mkeys.get("version"),
                 mkeys.get("placeholder")))
        print("    .pkg-version     %s (same string; nothing in src/ reads it)"
              % pkg_version)
        jblob = t.extractfile(names["usr/lib/libjpeg.so.8.3.2"]).read()
        jinfo = elf_full(jblob)
        if jinfo["soname"] != "libjpeg.so.8":
            raise SystemExit("the packaged libjpeg has SONAME %r"
                             % jinfo["soname"])
        print("    bin              static, e_type=%d, interp=%r, needed=%s"
              % (again_linfo["e_type"], again_linfo["interp"],
                 again_linfo["needed"]))
        print("    libexec/engine   dynamic, PT_INTERP %r, NEEDED %s"
              % (again_vinfo["interp"],
                 ", ".join(again_vinfo["needed"])))
        print("    loader           present at the bundle root")
        print("    libjpeg.so.8.3.2 SONAME %s, %d bytes"
              % (jinfo["soname"], jinfo["size"]))
        vblob = t.extractfile(names["libexec/engine"]).read()
        imported = check_symbols(vblob, jblob)
        n_res = check_closure(
            vblob, t.extractfile(names["ld-musl-x86_64.so.1"]).read(), jblob)
        print("    the viewer imports %d jpeg_* symbols, all exported by it:"
              % len(imported))
        print("      %s" % " ".join(imported))
        print("    all %d undefined symbols resolve against the loader "
              "(musl libc) + libjpeg" % n_res)

    h = hashlib.sha256()
    with open(args.out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    size = os.path.getsize(args.out)
    print("\nwrote %s" % args.out)
    print("  %d files, %d links, %d chromium-root entries skipped"
          % (n_files, n_links, skipped))
    print("  %.1f MB, sha256 %s" % (size / 1048576, h.hexdigest()))
    print("  %.1f MB installed (%d bytes)" % (installed / 1048576.0, installed))
    print("  packed in %.1f s" % (time.time() - t0))

    # The index row is a build artifact of THIS file: serve-repo.sh rebuilds
    # index.tsv from build/repo/engine.row (it skips stage/.../engine on
    # purpose), and pkg verifies the row's sha256 against the tarball. Leaving
    # a stale row after a repack publishes a package nobody can install:
    # 'checksum mismatch'. Signing stays with the publisher (sign-index.py,
    # --seed build/sign/repo.key), so only the row is written here.
    if not args.no_row:
        row = "\t".join([os.path.basename(args.out).split(".")[0],
                         pkg_version,
                         str(size), h.hexdigest(), os.path.basename(args.out),
                         manifest_value("desc") or "Mystic OS application",
                         str(installed)])
        row_path = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                                "engine.row")
        with open(row_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(row + "\n")
        print("  row: %s" % row_path)
        print("       %s" % row)
        print("  run tools/serve-repo.sh to fold the row into index.tsv and "
              "re-sign it")


if __name__ == "__main__":
    main()
