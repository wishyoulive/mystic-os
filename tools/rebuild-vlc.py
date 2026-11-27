#!/usr/bin/env python3
"""Rebuild the VLC player inside an existing package tarball.

The player is two binaries and the difference matters:

  bin              a STATIC launcher (27,648 bytes). The jail has no /lib, so a
                   dynamic binary there cannot start; this one only execs the
                   bundle's own loader on the payload.
  libexec/vlcplay  the real player, dynamic, PT_INTERP rewritten to
                   /lib/ld-musl-x86_64.so.1 and linked against libVLC and its
                   plugins from the bundle's usr/lib.

Editing src/client/apps/vlcplay.c therefore needs BOTH rebuilt, and the tarball
needs repacking without losing the 300-odd soname symlinks that Windows cannot
create. That is what this tool is for: it streams the existing tarball (which
already holds the libraries, plugins and loader), replaces only those two
members, updates the index row and re-signs the index.

    python tools/rebuild-vlc.py                 # rebuild both, repack, sign
    python tools/rebuild-vlc.py --no-sign       # leave the index signature alone
    python tools/rebuild-vlc.py --check-only    # report the two binaries as they
                                                # are in the tarball, change nothing

Why the PT_INTERP rewrite: zig prefixes -dynamic-linker with its own sysroot, so
the field it writes names a host path. The jail only has the bundle's loader at
/lib/ld-musl-x86_64.so.1. The rewrite is done in place on a same-length string
and then read back from the file, because a command line that looks right has
already been wrong once here.
"""

import argparse
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TARBALL = os.path.join(REPO, "build", "repo", "vlc.tar.gz")
INDEX = os.path.join(REPO, "build", "repo", "index.tsv")
SIGN_KEY = os.path.join(REPO, "build", "sign", "repo.key")
PLAYER_SRC = os.path.join(REPO, "src", "client", "apps", "vlcplay.c")

WANT_INTERP = b"/lib/ld-musl-x86_64.so.1"

PLAYER_SOURCES = [
    "src/client/apps/vlcplay.c",
    "src/client/mystic_client.c",
    "src/gfx.c",
    "src/font.c",
    "src/ui.c",
    "src/filetypes.c",
    "src/audio.c",
    "src/img.c",
]

PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3


# --- ELF helpers -----------------------------------------------------------

def elf_interp(data):
    """PT_INTERP contents, or None for a static binary."""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return "<not an ELF64 file>"
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", data, off)[0] == PT_INTERP:
            o = struct.unpack_from("<Q", data, off + 8)[0]
            sz = struct.unpack_from("<Q", data, off + 0x20)[0]
            return data[o:o + sz].split(b"\0")[0].decode() or "<empty>"
    return None


def set_interp(path, want=WANT_INTERP):
    """Rewrite PT_INTERP in place. Returns (before, after) as text."""
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("%s is not an ELF64 file" % path)
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", data, off)[0] != PT_INTERP:
            continue
        o = struct.unpack_from("<Q", data, off + 8)[0]
        sz = struct.unpack_from("<Q", data, off + 0x20)[0]
        before = data[o:o + sz].split(b"\0")[0].decode()
        if sz < len(want) + 1:
            raise SystemExit("PT_INTERP field is %d bytes, the path needs %d"
                             % (sz, len(want) + 1))
        data[o:o + sz] = want + b"\0" * (sz - len(want))
        open(path, "wb").write(bytes(data))
        after = elf_interp(bytes(data))
        if after != want.decode():
            raise SystemExit("rewrite did not take: %r" % (after,))
        return before, after
    raise SystemExit("%s has no PT_INTERP, so it is static and cannot load "
                     "libvlc" % path)


# --- building --------------------------------------------------------------

def find_zig():
    if os.environ.get("ZIG"):
        return os.environ["ZIG"]
    out = subprocess.run(["where", "zig"], capture_output=True, text=True)
    if out.returncode == 0 and out.stdout.strip():
        return out.stdout.splitlines()[0].strip()
    roots = [os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages")]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for d in sorted(os.listdir(root)):
            if not d.startswith("zig.zig_"):
                continue
            for sub in sorted(os.listdir(os.path.join(root, d))):
                p = os.path.join(root, d, sub, "zig.exe")
                if os.path.exists(p):
                    return p
    return None


def build(zig, out, static, extra=()):
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os",
           "-Isrc", "-Isrc/client"]
    if static:
        cmd += ["-static", "-DVLCPLAY_BOOT"]
        cmd += [PLAYER_SOURCES[0]]
    else:
        cmd += ["-dynamic",
                "-Wl,-rpath,$ORIGIN/../lib",
                "-Wl,-dynamic-linker=/lib/ld-musl-x86_64.so.1"]
        cmd += PLAYER_SOURCES
        cmd += ["-lm", "-pthread", "-ldl"]
    cmd += list(extra)
    cmd += ["-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("error: build failed (%d)" % r.returncode)
    for line in (r.stdout + r.stderr).splitlines():
        if line.strip() and "warning:" in line:
            print("  note: %s" % line.strip(), file=sys.stderr)
    return os.path.getsize(out)


# --- tarball ---------------------------------------------------------------

def repack(tarball, members):
    """Replace members (name -> bytes) in place, keeping every symlink entry."""
    src = tarfile.open(tarball, "r:gz")
    fd, tmp = tempfile.mkstemp(suffix=".tar.gz")
    os.close(fd)
    out = tarfile.open(tmp, "w:gz")
    seen = set()
    for ti in src:
        name = ti.name.lstrip("./")
        if name in members:
            data = members[name]
            ti.size = len(data)
            ti.mode = 0o755
            out.addfile(ti, io.BytesIO(data))
            seen.add(name)
            continue
        out.addfile(ti, io.BytesIO(src.extractfile(ti).read())
                    if ti.isreg() else None)
    out.close()
    src.close()
    missing = set(members) - seen
    if missing:
        os.unlink(tmp)
        raise SystemExit("error: the tarball has no member %s"
                         % ", ".join(sorted(missing)))
    os.replace(tmp, tarball)


def index_row(tarball):
    sha = hashlib.sha256(open(tarball, "rb").read()).hexdigest()
    return sha, os.path.getsize(tarball)


def update_index(path, size, sha):
    rows = [l.rstrip("\n") for l in open(path, encoding="utf-8") if l.strip()]
    out, found = [], False
    for r in rows:
        f = r.split("\t")
        if f[0] == "vlc":
            f[2], f[3] = str(size), sha
            r = "\t".join(f)
            found = True
        out.append(r)
    if not found:
        raise SystemExit("error: no vlc row in %s" % path)
    open(path, "w", encoding="utf-8", newline="").write("\n".join(out) + "\n")


def sign(index):
    r = subprocess.run([sys.executable, os.path.join(HERE, "sign-index.py"),
                        "sign", "--seed", SIGN_KEY, "--in", index],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("error: signing failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tarball", default=TARBALL)
    ap.add_argument("--index", default=INDEX)
    ap.add_argument("--no-sign", action="store_true")
    ap.add_argument("--check-only", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.tarball):
        raise SystemExit("error: %s does not exist" % args.tarball)

    if args.check_only:
        t = tarfile.open(args.tarball, "r:gz")
        print("==> %s" % args.tarball)
        print("    %d members" % len(t.getmembers()))
        # A tarball made by `tar -C dir .` stores ./bin; one made by
        # tools/pack-vlc.py stores bin. repack() accepts either (it strips
        # ./, ), so this report must too - it used to print MISSING for both
        # members of a pack-vlc.py tarball and still exit 0.
        names = set(t.getnames())
        missing = []
        for name in ("bin", "libexec/vlcplay"):
            member = "./" + name if "./" + name in names else name
            if member not in names:
                print("    %-18s MISSING" % name)
                missing.append(name)
                continue
            d = t.extractfile(member).read()
            print("    %-18s %8d bytes  interp=%s"
                  % (name, len(d), elf_interp(d)))
        return 1 if missing else 0

    zig = find_zig()
    if not zig:
        raise SystemExit("error: zig not found (set ZIG=)")

    tmpdir = tempfile.mkdtemp(prefix="vlc-rebuild-")
    try:
        player = os.path.join(tmpdir, "vlcplay")
        launcher = os.path.join(tmpdir, "launcher")
        print("==> building the dynamic player")
        psize = build(zig, player, static=False)
        before, after = set_interp(player)
        print("    %d bytes, PT_INTERP %s -> %s" % (psize, before, after))
        print("==> building the static launcher")
        lsize = build(zig, launcher, static=True)
        if elf_interp(open(launcher, "rb").read()) is not None:
            raise SystemExit("error: the launcher came out dynamic; the jail "
                             "has no /lib and it would not start")
        print("    %d bytes, no PT_INTERP" % lsize)

        print("==> repacking %s" % args.tarball)
        # Back up everything this run is about to change. update_index()
        # rewrites index.tsv BEFORE sign() runs, so a signing failure
        # (build/sign/repo.key missing, or a key that does not match the
        # distributed public half) left a new index beside the old signature:
        # pkg verifies the pair before it reads a row, so it then refused
        # EVERY package until someone noticed - and the tarball had already
        # been replaced, so there was nothing to fall back to.
        backup = os.path.join(tmpdir, "restore")
        os.makedirs(backup, exist_ok=True)
        saved = []
        for path in (args.tarball, args.index, args.index + ".sig"):
            if os.path.exists(path):
                shutil.copy2(path, os.path.join(backup, os.path.basename(path)))
                saved.append(path)
        try:
            repack(args.tarball, {"bin": open(launcher, "rb").read(),
                                  "libexec/vlcplay": open(player, "rb").read()})
            sha, size = index_row(args.tarball)
            update_index(args.index, size, sha)
            print("    %d bytes, sha256 %s" % (size, sha))
            if not args.no_sign:
                sign(args.index)
                print("    index re-signed")
            else:
                print("    --no-sign: %s no longer matches its signature; pkg "
                      "will refuse it until it is signed" % args.index,
                      file=sys.stderr)
        except BaseException:
            for path in saved:
                shutil.copy2(os.path.join(backup, os.path.basename(path)), path)
            print("    error: restored the tarball and the index - the mirror is "
                  "unchanged", file=sys.stderr)
            raise
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
