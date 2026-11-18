#!/usr/bin/env python3
"""
fetch-engine.py - build the real-browser-engine package for Mystic OS.

The owner wants full HTML/CSS/JS. src/client/apps/web.c is our own renderer:
it parses HTML, applies a CSS subset and draws text, and it will never run a
framework. This script builds the package that runs somebody else's engine
instead - resolved from the Alpine index, unpacked into a tree, and packed as
the installable package `engine`.

    python tools/fetch-engine.py --candidates   # the four candidates, measured
    python tools/fetch-engine.py --dry-run      # resolve + print, fetch nothing
    python tools/fetch-engine.py                # download, unpack, build, pack
    python tools/fetch-engine.py --exclude llvm22-libs --profile-hint lean

WHAT IT PRODUCES (all under build/, nothing in the image):

    build/engine-root/          the unpacked closure, Alpine layout
    build/engine-pkg/           the package stage (manifest, bin, ld-musl, usr/)
    build/repo/engine.tar.gz    the installable package
    build/repo/engine.row       its index row, and index.tsv is updated+re-signed
    build/engine-build.log      the compiler output for bin (engineview)

THE CHOICE, AND WHY (numbers are printed by --candidates, not hard-coded):

    candidate                 pkgs      download     installed   display server
    chromium-headless-shell    182      258.3 MB      682.0 MB   none
    chromium (full)            181      318.5 MB      799.5 MB   none
    webkit2gtk-4.1             200      210.6 MB      569.1 MB   X or Wayland
    webkit2gtk-4.1 + xvfb      212      215.9 MB      576.1 MB   Xvfb
    wpewebkit                   -            -             -    NOT PACKAGED
    netsurf-framebuffer         23        9.5 MB       22.7 MB   framebuffer/SDL

    wpewebkit is not in Alpine v3.24 at all: the index has libwpe 1.16.3 and
    libwpebackend-fdo, and no WPE WebKit package. That candidate does not
    exist here, so it is not costed.

    webkit2gtk is the smaller *download* but it cannot be driven from outside
    its own process. It renders into a GTK widget; getting pixels out means
    either webkit_web_view_get_snapshot() from a GTK program we would have to
    write and link against webkit2gtk (a build-time -dev dependency), or an
    X server we screenshot. Nothing in that path is a frame we can request and
    blit at will.

    chromium-headless-shell is headless on its own (no X server, no Wayland,
    no compositor), and it exposes the DevTools protocol over a pair of file
    descriptors (--remote-debugging-pipe). Page.startScreencast then delivers
    JPEG frames on demand and Input.dispatchMouseEvent/KeyEvent forwards our
    input. src/img.c already decodes baseline+progressive JPEG into
    0xAARRGGBB, which is exactly the window format, so the frame path is
    decode+scale+blit and nothing else.

    It is big: 682 MB unpacked. That is why this is a package on the data
    disk and never part of the 13 MB initramfs, and why the tool refuses to
    download more than --max-download-mb (400) without --force.

THE TWO THINGS THAT MAKE IT START INSIDE THE JAIL
-------------------------------------------------
src/wmsrv.c chroots an app into /run/mystic/jail/<id> and binds only
/bin/app, the bundle read-only at /app, fonts, /dev bits, /proc and a
private /root. There is no /lib in there, and the engine binary's PT_INTERP
is the literal string /lib/ld-musl-x86_64.so.1. Two facts follow, both
handled here and both printed by --dry-run:

  1. The bundle carries musl's loader at its root and the engine binary's
     PT_INTERP is rewritten to /app/ld-musl-x86_64.so.1. The string fits:
     the original is 24 bytes + NUL and the ELF reserves 28 bytes before the
     next non-zero byte (measured on 156.0.8078.79: PT_INTERP offset 736,
     filesz 25, next non-zero byte at +28). The new string is 25 bytes + NUL.
     Patching is required rather than exec'ing the loader by hand because
     Chromium re-execs /proc/self/exe for its zygote and renderer children:
     with the loader as the process image that re-exec would run the loader
     with no program to load. With PT_INTERP patched, /proc/self/exe is the
     engine binary itself and the children inherit LD_LIBRARY_PATH, which
     src/wmsrv.c already exports as /app/usr/lib:/app/lib.
  2. Library and data placement follows those two variables: shared objects
     in usr/lib (LD_LIBRARY_PATH), ICU data in usr/share/icu plus a symlink
     at usr/lib/chromium/icudtl.dat where Chromium looks first.

WHAT IS NOT CLAIMED
-------------------
Nothing here boots the engine. This script proves the resolver, the download,
the unpack, the ELF closure (every DT_NEEDED of every shipped ELF resolves
inside the tree), the interpreter patch and the archive. The window app's
frame path is verified by compiling it and by the host self-test of its
parsers, not by a rendered page. docs/ENGINE.md says exactly that.
"""

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.24"
REPOS = ["main", "community"]
ARCH = "x86_64"

INDEX_CACHE = os.path.join("build", "engine-index")
APK_CACHE = os.path.join("build", "engine-apks")
DEST = os.path.join("build", "engine-root")
PKG_STAGE = os.path.join("build", "engine-pkg")
REPO = os.path.join("build", "repo")
PKG_NAME = "engine"
PKG_VERSION_DEFAULT = "156.0.8078.79"
BUILD_LOG = os.path.join("build", "engine-build.log")

# The bundle id is PKG_NAME. src/wmsrv.c appends the manifest's exec value to
# the bundle path, so 'exec' must be a bare file name ("bin").
IMAGE_BUNDLE = os.path.join("stage", "usr", "share", "mystic", "apps", PKG_NAME)
MANIFEST = os.path.join(PKG_STAGE, "manifest")

# Where the engine lives inside the bundle, and the loader it asks for.
ENGINE_BIN = "usr/lib/chromium/chromium-headless-shell"
LOADER_TOP = "ld-musl-x86_64.so.1"          # bundle root -> /app/ld-musl-...
# The window app's file name in the bundle. It is NOT 'bin': Alpine's closure
# ships a top-level /bin (busybox and two shells), and a file and a directory
# cannot share a name. src/wmsrv.c only requires the manifest's exec to be a
# bare file name, so 'engine' is the entry point and /bin stays the tree's.
APP_EXEC = "engine"

# Bump when unpack/stage semantics change: it is part of the cache stamp, so an
# old tree is rebuilt instead of silently packed with the old rules.
TREE_VERSION = 2
# The path the kernel will look for. It must be the bundle mounted at /app,
# which is what src/wmsrv.c does for every app; nothing here may claim the
# bundle works from any other place.
INTERP_JAIL = "/app/" + LOADER_TOP

# The candidates. `roots` is what the closure is resolved from; `frames` says
# how pixels get out of it, because a candidate that cannot hand us a frame is
# not a browser for this OS however small it is.
CANDIDATES = {
    "chromium-headless-shell": {
        "roots": ["chromium-headless-shell"],
        "frames": "CDP Page.startScreencast over --remote-debugging-pipe (JPEG)",
        "display": "none (headless; X client libs linked but no X server)",
        "js": "V8", "css": "Blink", "verdict": "chosen",
    },
    "chromium": {
        "roots": ["chromium"],
        "frames": "same CDP, but the full browser UI we never show",
        "display": "none needed",
        "js": "V8", "css": "Blink", "verdict": "bigger, nothing gained",
    },
    "webkit2gtk-4.1": {
        "roots": ["webkit2gtk-4.1"],
        "frames": "none without a GTK host: webkit_web_view_get_snapshot()",
        "display": "X or Wayland required",
        "js": "JavaScriptCore", "css": "WebKit", "verdict": "no out-of-process frames",
    },
    "webkit2gtk-4.1+xvfb": {
        "roots": ["webkit2gtk-4.1", "xvfb"],
        "frames": "X screenshot of an Xvfb display",
        "display": "Xvfb (12 extra packages)",
        "js": "JavaScriptCore", "css": "WebKit", "verdict": "display server + host app",
    },
    "wpewebkit": {
        "roots": ["wpewebkit"],
        "frames": "would be offscreen EGL", "display": "EGL",
        "js": "JavaScriptCore", "css": "WebKit", "verdict": "not packaged in Alpine",
    },
    "netsurf-framebuffer": {
        "roots": ["netsurf-framebuffer"],
        "frames": "renders straight into a framebuffer/SDL surface",
        "display": "fbcon or SDL",
        "js": "none (NetSurf 3.x has no JS engine)", "css": "CSS 2.1 subset",
        "verdict": "the small honest fallback, not an engine",
    },
}

DEFAULT_ROOT = "chromium-headless-shell"


# ---------------------------------------------------------------------------
# network + index
# ---------------------------------------------------------------------------
def fetch(url, path=None, timeout=120, tries=3):
    """Download, resuming a .part, retrying a dropped connection.

    pkg resumes its transfers, so a build tool that restarts a 90 MB .apk from
    zero is worse than the shell script it replaces. Range is only used when
    the server answers 206; a 200 restarts the file.
    """
    last = None
    for attempt in range(tries):
        try:
            if not path:
                return urllib.request.urlopen(
                    urllib.request.Request(url, headers={"User-Agent": "mystic-fetch-engine/1"}),
                    timeout=timeout).read()
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            part = path + ".part"
            have = os.path.getsize(part) if os.path.exists(part) else 0
            hdr = {"User-Agent": "mystic-fetch-engine/1"}
            if have:
                hdr["Range"] = "bytes=%d-" % have
            r = urllib.request.urlopen(urllib.request.Request(url, headers=hdr),
                                      timeout=timeout)
            data = r.read()
            if have and getattr(r, "status", 200) != 206:
                have = 0                      # server ignored Range: restart
                os.remove(part)
            with open(part, "ab" if have else "wb") as f:
                f.write(data)
            os.replace(part, path)
            return data
        except Exception as e:                # noqa: BLE001 - reported, retried
            last = e
            time.sleep(1 + attempt)
    raise last


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
    return dl, ins, real


# ---------------------------------------------------------------------------
# ELF: DT_NEEDED (for the closure check) and PT_INTERP (for the jail)
# ---------------------------------------------------------------------------
def elf_info(path):
    """(interp, [(needed...)], soname) or (None, [], None) for a non-ELF.

    Reads the dynamic table through PT_DYNAMIC and translates DT_STRTAB's
    virtual address through PT_LOAD, so a stripped library with no section
    headers is still read correctly - the same reason the kernel can run it.
    """
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return (None, [], None)
    if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        return (None, [], None)
    try:
        e_phoff = int.from_bytes(d[0x20:0x28], "little")
        e_phentsize = int.from_bytes(d[0x36:0x38], "little")
        e_phnum = int.from_bytes(d[0x38:0x3a], "little")
        interp = None
        loads, dyn = [], None
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            typ = int.from_bytes(d[o:o + 4], "little")
            off = int.from_bytes(d[o + 8:o + 16], "little")
            vaddr = int.from_bytes(d[o + 16:o + 24], "little")
            filesz = int.from_bytes(d[o + 32:o + 40], "little")
            if typ == 3:                                   # PT_INTERP
                interp = d[off:off + filesz].split(b"\0")[0].decode("utf-8", "replace")
            elif typ == 1:                                 # PT_LOAD
                loads.append((vaddr, off, filesz))
            elif typ == 2:                                 # PT_DYNAMIC
                dyn = (off, filesz)
        if not dyn:
            return (interp, [], None)

        def v2o(addr):
            for vaddr, off, filesz in loads:
                if vaddr <= addr < vaddr + filesz:
                    return off + (addr - vaddr)
            return None

        strtab = None
        entries = []
        off, filesz = dyn
        for o in range(off, off + filesz, 16):
            tag = int.from_bytes(d[o:o + 8], "little", signed=True)
            val = int.from_bytes(d[o + 8:o + 16], "little")
            if tag == 0:
                break
            entries.append((tag, val))
            if tag == 5:                                   # DT_STRTAB
                strtab = v2o(val)
        if strtab is None:
            return (interp, [], None)

        def s(idx):
            if strtab + idx >= len(d):
                return ""
            e = d.find(b"\0", strtab + idx)
            return d[strtab + idx:e].decode("utf-8", "replace")

        needed = [s(v) for t, v in entries if t == 1]
        soname = next((s(v) for t, v in entries if t == 14), None)
        return (interp, needed, soname)
    except Exception:                                     # noqa: BLE001
        return (None, [], None)


def interp_room(d, ph_off, ph_filesz):
    """Bytes available for a new PT_INTERP string without moving anything.

    The segment normally ends at the NUL; the slack is the run of zero bytes
    after it that no other structure occupies. Measured, not assumed.
    """
    start = ph_off
    end = ph_off + ph_filesz
    j = end
    while j < len(d) and d[j] == 0:
        j += 1
    return start, end, j - start


def patch_interp(path, want):
    """Rewrite PT_INTERP in place. Returns (old, new, offset, filesz) or None.

    This is what patchelf --set-interpreter does, done by hand so the tool has
    no dependency and can *verify* what it wrote. It refuses rather than
    guesses: if the new string does not fit in the measured slack, nothing is
    changed and the caller sees why.
    """
    with open(path, "rb") as f:
        d = bytearray(f.read())
    if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2:
        return None
    e_phoff = int.from_bytes(d[0x20:0x28], "little")
    e_phentsize = int.from_bytes(d[0x36:0x38], "little")
    e_phnum = int.from_bytes(d[0x38:0x3a], "little")
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        if int.from_bytes(d[o:o + 4], "little") != 3:
            continue
        off = int.from_bytes(d[o + 8:o + 16], "little")
        filesz = int.from_bytes(d[o + 32:o + 40], "little")
        old = bytes(d[off:off + filesz]).split(b"\0")[0].decode()
        start, end, room = interp_room(d, off, filesz)
        new = want.encode() + b"\0"
        if len(new) > room:
            raise SystemExit(
                "error: %s: PT_INTERP is %d bytes and only %d are free; "
                "'%s' is %d bytes. Refusing to corrupt the engine binary."
                % (path, filesz, room, want, len(new)))
        d[off:off + filesz] = b"\0" * filesz
        d[off:off + len(new)] = new
        # the segment grows by the string's growth, nothing else moves
        d[o + 32:o + 40] = (len(new)).to_bytes(8, "little")
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(d)
        os.replace(tmp, path)
        # read it back: the caller must be able to trust this number
        again, _, _ = elf_info(path)
        if again != want:
            raise SystemExit("error: %s: patch did not take (%r)" % (path, again))
        return (old, want, off, len(new))
    return None


# ---------------------------------------------------------------------------
# unpacking: regular files now, links recorded for the archive
# ---------------------------------------------------------------------------
def apk_path(pkgs, name):
    info = pkgs.get(name)
    if not info:
        return None
    return os.path.join(APK_CACHE, info["file"])


def get_apk(pkgs, name, download=True):
    info = pkgs.get(name)
    if not info:
        return None
    path = os.path.join(APK_CACHE, info["file"])
    if os.path.exists(path) and os.path.getsize(path) == info["download"]:
        return path
    if not download:
        return None
    url = "%s/%s/%s/%s" % (ALPINE, info["repo"], ARCH, info["file"])
    fetch(url, path)
    got = os.path.getsize(path)
    if got != info["download"]:
        raise SystemExit("error: %s is %d bytes, the index says %d - refusing "
                         "to unpack a truncated package"
                         % (info["file"], got, info["download"]))
    return path


def unpack_apk(path, dest, owner, owners, links):
    """Extract one .apk. Returns (files, bytes, skipped, special).

    Symlinks and hardlinks cannot be created on this host (Windows), so they
    are recorded in `links` and materialised later as copies for local
    inspection; the *archive* re-creates them as links (see plan_links).
    """
    n = nbytes = skipped = special = 0
    tf = tarfile.open(path, "r:gz")
    for m in tf:
        name = m.name
        if name.startswith("./"):
            name = name[2:]
        # .PKGINFO, .SIGN.*, .post-install, .trigger, ... are apk control
        # files at the archive root, not payload. A dot-file *inside* a
        # directory (/etc/skel/.profile) is payload and is kept.
        if not name or name.startswith(".SIGN") or name == ".PKGINFO":
            continue
        if "/" not in name and name.startswith("."):
            continue
        rel = name.lstrip("/")
        target = os.path.join(dest, rel)
        if m.isdir():
            os.makedirs(target, exist_ok=True)
            continue
        if m.issym() or m.islnk():
            links[rel] = (m.linkname, owner)
            skipped += 1
            continue
        if not m.isfile():
            special += 1
            continue
        os.makedirs(os.path.dirname(target) or dest, exist_ok=True)
        src = tf.extractfile(m)
        with open(target, "wb") as f:
            shutil.copyfileobj(src, f, 1 << 20)
        if m.mode & 0o111:
            try:
                os.chmod(target, 0o755)
            except OSError:
                pass
        owners[rel] = owner
        n += 1
        nbytes += m.size
    tf.close()
    return n, nbytes, skipped, special


def materialise_links(dest, links):
    """Copy link targets so the local tree is usable (the archive keeps links)."""
    made = 0
    for rel, (target, _owner) in sorted(links.items()):
        src = os.path.join(dest, rel)
        if os.path.exists(src):
            continue
        if target.startswith("/"):
            cand = os.path.join(dest, target.lstrip("/"))
        else:
            cand = os.path.normpath(os.path.join(
                os.path.dirname(os.path.join(dest, rel)), target))
        if os.path.isfile(cand):
            os.makedirs(os.path.dirname(src) or dest, exist_ok=True)
            try:
                shutil.copyfile(cand, src)
                made += 1
            except OSError:
                pass
    return made


def plan_links(dest, links, skipped_pkgs):
    """Turn recorded links into archive members with a target that resolves.

    An absolute Alpine link (/usr/lib/libfoo.so.1 -> /usr/lib/libfoo.so.1.2.3)
    is rewritten relative to the member's own directory, because inside the
    jail /usr/lib is not where the bundle lives. A link whose target is not in
    the tree is dropped and counted: pkg would repair it to nothing, and a
    link to a file we did not ship is a lie either way.
    """
    out, kept, dropped = {}, 0, []
    for rel, (target, owner) in links.items():
        if owner in skipped_pkgs:
            continue
        if target.startswith("/"):
            tgt_rel = target.lstrip("/")
            if not os.path.exists(os.path.join(dest, tgt_rel)):
                dropped.append((rel, target))
                continue
            # POSIX separators: this string is a tar symlink target, not a
            # host path. os.path.relpath gives backslashes on Windows, which
            # inside the guest is one literal file name.
            newt = os.path.relpath(os.path.join(dest, tgt_rel),
                                   os.path.dirname(os.path.join(dest, rel))
                                   ).replace(os.sep, "/")
        else:
            base = os.path.dirname(os.path.join(dest, rel))
            if not os.path.exists(os.path.normpath(os.path.join(base, target))):
                dropped.append((rel, target))
                continue
            newt = target
        out[rel] = newt
        kept += 1
    return out, kept, dropped


def tree_bytes(dest):
    total = files = 0
    for root, _dirs, names in os.walk(dest):
        for nm in names:
            p = os.path.join(root, nm)
            try:
                total += os.path.getsize(p)
                files += 1
            except OSError:
                pass
    return total, files


# ---------------------------------------------------------------------------
# the closure check: every DT_NEEDED of every shipped ELF resolves here
# ---------------------------------------------------------------------------
def check_closure(dest, loader_top, interp, skipped_pkgs, owners, links=None,
                  quiet=False):
    """Return (unresolved[(elf, needed)], names).

    This is the check that catches the classic broken package: a library that
    is present in the closure list but whose file the packer did not include,
    or a package an --exclude dropped that something still needs.

    Resolution is by FILE NAME, which is what musl's loader does: it opens the
    string in DT_NEEDED and then refuses a library whose DT_SONAME disagrees.
    glibc searches by SONAME instead, so a SONAME-only check would pass a
    package that cannot start here. Shipped symlinks count as names, because
    they are what makes "libgbm.so.1" openable when only libgbm.so.1.5.0 is a
    real file.
    """
    loadable, elfs = {}, []
    for root, _dirs, files in os.walk(dest):
        for nm in files:
            p = os.path.join(root, nm)
            rel = os.path.relpath(p, dest).replace(os.sep, "/")
            if owners.get(rel) in skipped_pkgs:
                continue
            try:
                with open(p, "rb") as f:
                    if f.read(4) != b"\x7fELF":
                        continue
            except OSError:
                continue
            _i, needed, _soname = elf_info(p)
            elfs.append((rel, needed))
            loadable.setdefault(os.path.basename(rel), rel)
    for rel in (links or {}):
        loadable.setdefault(os.path.basename(rel), rel)

    reserved = {"libc.musl-x86_64.so.1", "libc.so", "ld-musl-x86_64.so.1"}
    unresolved = []
    for rel, needed in elfs:
        for d in needed:
            if d in reserved or d in loadable:
                continue
            unresolved.append((rel, d))
    if not quiet:
        print("==> closure check: %d ELF files, %d loadable names (%d from "
              "shipped links), %d unresolved by name"
              % (len(elfs), len(loadable), len(links or {}), len(unresolved)))
        for rel, d in unresolved[:20]:
            print("      %s needs %s" % (rel, d))
    # The interpreter the kernel will look for must exist in the bundle. At
    # this point the tree is still Alpine's layout (lib/ld-musl-...), and
    # build_stage copies it to the bundle root where PT_INTERP points; both
    # places count, and the caller is told which one was found.
    cands = [os.path.join(dest, loader_top),
             os.path.join(dest, "lib", loader_top)]
    found = next((c for c in cands if os.path.exists(c)), None)
    if found is None:
        unresolved.append((ENGINE_BIN, "PT_INTERP %s (bundle has no %s)"
                           % (interp, loader_top)))
    return unresolved, loadable


# ---------------------------------------------------------------------------
# the window app: compiled here so the package is one command
# ---------------------------------------------------------------------------
def find_zig():
    z = os.environ.get("ZIG")
    if z:
        return z
    for name in ("zig", "zig.exe"):
        p = shutil.which(name)
        if p:
            return p
    import glob
    for cand in glob.glob(os.path.expanduser(
            "~/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-x86_64-windows-*/zig.exe")):
        return cand
    return None


APP_SRC = os.path.join("src", "client", "apps", "engineview.c")
APP_DEPS = [os.path.join("src", "client", "mystic_client.c"),
            os.path.join("src", "gfx.c"), os.path.join("src", "font.c"),
            os.path.join("src", "ui.c"), os.path.join("src", "filetypes.c"),
            os.path.join("src", "img.c")]


def build_app(out_path):
    """Compile engineview statically. Returns the compiler's own words."""
    zig = find_zig()
    if not zig:
        raise SystemExit("error: zig not found (set ZIG=)")
    for p in [APP_SRC] + APP_DEPS:
        if not os.path.exists(p):
            raise SystemExit("error: missing source %s" % p)
    cmd = [zig, "cc", "-target", "x86_64-linux-musl", "-Os", "-static",
           "-Isrc", "-Isrc/client", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-o", out_path, APP_SRC] + APP_DEPS + ["-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = "$ %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr)
    with open(BUILD_LOG, "w", encoding="utf-8", newline="\n") as f:
        f.write(log)
    if r.returncode != 0:
        print(log, file=sys.stderr)
        raise SystemExit("error: the window app did not compile (%d)" % r.returncode)
    if r.stdout.strip() or r.stderr.strip():
        # the project's rule is a silent compile; a warning here would ship
        print("warning: engineview.c compiled with output (see %s)" % BUILD_LOG,
              file=sys.stderr)
    return os.path.getsize(out_path)


# ---------------------------------------------------------------------------
# packaging
# ---------------------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def pack_tarball(entries, out_path):
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


def build_stage(dest, owners, links, skipped_pkgs, app_path):
    """Assemble build/engine-pkg: manifest, bin, loader, usr/ tree.

    Returns (links_for_archive, bytes, files, manifest_text).
    """
    stage = PKG_STAGE
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    # 1. the window app, static: this is the manifest's exec
    shutil.copyfile(app_path, os.path.join(stage, APP_EXEC))
    os.chmod(os.path.join(stage, APP_EXEC), 0o755)

    # 2. the loader at the bundle root, at the literal path PT_INTERP names.
    #    musl's ldso is in the closure's musl package; /lib/ld-musl-x86_64.so.1
    #    is a real file there (libc.musl... is the link to it).
    loader_src = os.path.join(dest, "lib", "ld-musl-x86_64.so.1")
    if not os.path.exists(loader_src):
        raise SystemExit("error: the closure did not bring musl's loader "
                         "(lib/ld-musl-x86_64.so.1); the bundle cannot start")
    shutil.copyfile(loader_src, os.path.join(stage, LOADER_TOP))
    os.chmod(os.path.join(stage, LOADER_TOP), 0o755)

    # 3. the tree, minus excluded packages, minus the two files already placed
    n = 0
    for root, dirs, names in os.walk(dest):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".engine-"))
        for nm in sorted(names):
            src = os.path.join(root, nm)
            rel = os.path.relpath(src, dest).replace(os.sep, "/")
            if owners.get(rel) in skipped_pkgs:
                continue
            if rel in (APP_EXEC, "manifest", LOADER_TOP):
                continue                       # reserved by the bundle
            if rel.startswith(".engine-") or ("/" not in rel and rel.startswith(".")):
                continue                       # bookkeeping / apk control
            if rel in links:
                continue                       # the archive stores the link,
                                               # not a second copy of the .so
            dst = os.path.join(stage, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            try:
                shutil.copyfile(src, dst)
            except OSError as e:
                print("    skip %s: %s" % (rel, e))
                continue
            if os.access(src, os.X_OK):
                os.chmod(dst, 0o755)
            n += 1

    # 4. ICU: Chromium looks for icudtl.dat beside its executable first.
    #    Alpine's package keeps it in usr/share/icu/<ver>/. A relative symlink
    #    is one copy on disk and the path Chromium tries first.
    icu = None
    for root, _dirs, names in os.walk(os.path.join(stage, "usr", "share", "icu")):
        for nm in names:
            if nm.startswith("icudt") and nm.endswith(".dat"):
                icu = os.path.relpath(os.path.join(root, nm),
                                      os.path.join(stage, "usr", "lib", "chromium")
                                      ).replace(os.sep, "/")
                break
        if icu:
            break
    if icu:
        link = os.path.join(stage, "usr", "lib", "chromium", "icudtl.dat")
        if not os.path.exists(link):
            with open(os.path.join(stage, "usr", "lib", "chromium", ".icu-link"), "w") as f:
                f.write(icu + "\n")
            links.setdefault("usr/lib/chromium/icudtl.dat", (icu, "icu"))

    # 5. links that the archive will recreate (relative, inside the bundle)
    lk, kept, dropped = plan_links(stage, links, skipped_pkgs)

    # 6. the manifest. 'exec' is a bare name because src/wmsrv.c rejects a
    #    path with '/' in it, and 'installed' is the real size of the tree so
    #    the package describes itself with a measured number.
    total, files = tree_bytes(stage)
    manifest = (
        "name = Browser Engine\n"
        "desc = Chromium headless: real HTML/CSS/JS, frames over CDP\n"
        "exec = %s\n"
        "version = %s\n"
        "network = host\n"
        "bundle = %s\n"
        "engine = chromium-headless-shell\n"
        "interp = %s\n"
        "installed = %d\n"
        "placeholder = no\n"
    ) % (APP_EXEC, PKG_VERSION_DEFAULT, PKG_NAME, INTERP_JAIL, total)
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest)

    print("==> stage: %d files, %.2f MB (%d links, %d dropped links)"
          % (files, total / 1e6, kept, len(dropped)))
    for rel, tgt in dropped[:5]:
        print("    dropped link %s -> %s (target not in the tree)" % (rel, tgt))
    return lk, total, files, manifest


def stage_entries(stage, links_out, tree=None):
    """Every member of the package: dirs, files, and the planned links.

    Links are archive members even though they are not on disk (Windows cannot
    create them), so the set is the union of the walked files and links_out.
    """
    entries = []
    seen = set()
    roots = [stage] + ([tree] if tree else [])
    for base in roots:
        for root, dirs, names in os.walk(base):
            dirs.sort()
            rel_root = os.path.relpath(root, base).replace(os.sep, "/")
            if rel_root == ".":
                rel_root = ""
            if rel_root and rel_root not in seen:
                entries.append((rel_root, "dir", None, 0o755, None))
                seen.add(rel_root)
            for nm in sorted(names):
                p = os.path.join(root, nm)
                rel = (rel_root + "/" + nm) if rel_root else nm
                if rel in seen:
                    continue
                seen.add(rel)
                if rel in links_out:
                    entries.append((rel, "symlink", None, 0o777, links_out[rel]))
                else:
                    mode = 0o755 if os.access(p, os.X_OK) else 0o644
                    entries.append((rel, "file", p, mode, None))
    for rel, tgt in links_out.items():
        if rel not in seen:
            entries.append((rel, "symlink", None, 0o777, tgt))
    entries.sort(key=lambda e: e[0])
    return entries


def upsert_index(tarball, installed, repo=None):
    """Put the engine row into build/repo/index.tsv and re-sign it.

    tools/serve-repo.sh rebuilds index.tsv from stage/, so it keeps this row
    from build/repo/engine.row the same way it keeps vlc's; without that the
    row survives exactly until the next mirror rebuild. The report says so.
    """
    repo = repo or REPO
    os.makedirs(repo, exist_ok=True)
    idx = os.path.join(repo, "index.tsv")
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
    desc = "Chromium headless engine: real HTML/CSS/JS, CDP frames"
    # column 7 is the unpacked size; pkg uses it for the exact space check
    # before the first byte is requested.
    row = "%s\t%s\t%d\t%s\t%s.tar.gz\t%s\t%d" % (
        PKG_NAME, PKG_VERSION_DEFAULT, size, sha, PKG_NAME, desc, installed)
    rows.append(row)
    tmp = idx + ".part"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    os.replace(tmp, idx)
    with open(os.path.join(REPO, "engine.row"), "w", encoding="utf-8", newline="\n") as f:
        f.write(row + "\n")

    key = os.path.join("build", "sign", "repo.key")
    pub = os.path.join("stage", "etc", "mystic", "repo.pub")
    if os.path.isfile(key):
        cmd = [sys.executable, os.path.join("tools", "sign-index.py"), "sign",
               "--seed", key, "--in", idx]
        if os.path.isfile(pub):
            cmd += ["--pub", pub]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("warning: could not sign index.tsv: %s%s"
                  % (r.stdout, r.stderr), file=sys.stderr)
        elif os.path.isfile(pub):
            shutil.copyfile(pub, os.path.join(repo, "index.pub"))
            print("==> index.tsv signed with %s" % pub)
    else:
        print("    no build/sign/repo.key: index.tsv is UNSIGNED and pkg will "
              "refuse it", file=sys.stderr)
    return size, sha, row


# ---------------------------------------------------------------------------
def choose_engine_version(pkgs, roots):
    for r in roots:
        if r in pkgs:
            return pkgs[r]["version"].split("-r")[0]
    return PKG_VERSION_DEFAULT


def cmd_candidates(pkgs, provides, refresh):
    print("%-24s %5s %11s %11s  %s" % ("candidate", "pkgs", "download", "installed", "verdict"))
    print("-" * 100)
    picked = None
    for name, spec in CANDIDATES.items():
        missing_roots = [r for r in spec["roots"] if r not in pkgs and r not in provides]
        if missing_roots:
            print("%-24s %5s %11s %11s  %s" % (name, "-", "-", "-",
                                             "ABSENT from the index (%s)" % ", ".join(missing_roots)))
            continue
        closure, missing = resolve(pkgs, provides, spec["roots"])
        dl, ins, real = totals(pkgs, closure)
        print("%-24s %5d %8.1f MB %8.1f MB  %s"
              % (name, len(real), dl / 1e6, ins / 1e6, spec["verdict"]))
        if name == DEFAULT_ROOT:
            picked = (name, len(real), dl, ins)
        elif name == "netsurf-framebuffer":
            fb = (len(real), dl, ins)
    print()
    for name, spec in CANDIDATES.items():
        print("%-24s frames: %s" % (name, spec["frames"]))
        print("%-24s display: %s; JS: %s; CSS: %s"
              % ("", spec["display"], spec["js"], spec["css"]))
    print()
    print("wpewebkit is absent because Alpine v3.24 ships libwpe and "
          "libwpebackend-fdo\nand no WPE WebKit package; 'apk add wpewebkit' "
          "would fail, so it is not costed.")
    if picked:
        print("\nchosen: %s - %d packages, %.1f MB download, %.1f MB installed"
              % (picked[0], picked[1], picked[2] / 1e6, picked[3] / 1e6))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="build the real-browser-engine package for Mystic OS")
    ap.add_argument("--root", action="append", default=None,
                    help="package to resolve instead of %s" % DEFAULT_ROOT)
    ap.add_argument("--candidates", action="store_true",
                    help="measure every candidate and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and print the plan; download nothing")
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--max-download-mb", type=float, default=400.0,
                    help="refuse a closure larger than this without --force "
                         "(the task's cap; default 400)")
    ap.add_argument("--force", action="store_true",
                    help="ignore --max-download-mb and the caches")
    ap.add_argument("--refresh-index", action="store_true")
    ap.add_argument("--exclude", action="append", default=[],
                    help="drop a package from the tree (checked afterwards)")
    ap.add_argument("--allow-unresolved", action="store_true",
                    help="pack even if an ELF's DT_NEEDED is not in the tree")
    ap.add_argument("--no-pack", action="store_true",
                    help="build the tree and the app, do not write the tarball")
    ap.add_argument("--list", action="store_true", help="print the whole closure")
    args = ap.parse_args()

    roots = args.root or [DEFAULT_ROOT]
    if args.force:
        for d in (APK_CACHE,):
            pass                          # keep the cache; --force is about caps

    pkgs, provides = build_world(args.refresh_index)
    print("==> index: %d packages, %d provides" % (len(pkgs), len(provides)))

    if args.candidates:
        return cmd_candidates(pkgs, provides, args.refresh_index)

    bad = [r for r in roots if r not in pkgs and r not in provides]
    if bad:
        raise SystemExit("error: not in the index: %s" % ", ".join(bad))

    closure, missing = resolve(pkgs, provides, roots)
    version = choose_engine_version(pkgs, roots)
    dl, ins, real = report(pkgs, closure, missing, "engine closure")
    skipped = set(args.exclude)
    for s in skipped:
        if s not in pkgs:
            raise SystemExit("error: --exclude %s is not a package in the closure" % s)
    skipped_ins = sum(pkgs[s]["installed"] for s in skipped)
    skipped_dl = sum(pkgs[s]["download"] for s in skipped)
    if skipped:
        print("    excluding: %s (-%.1f MB download, -%.1f MB installed)"
              % (", ".join(sorted(skipped)), skipped_dl / 1e6, skipped_ins / 1e6))
    kept_ins = ins - skipped_ins

    if args.list:
        for n in sorted(real):
            print("  %-34s %s" % (n, pkgs[n]["version"]))

    # the machine facts that decide whether this is installable at all
    print("==> what the guest must be to hold it:")
    print("      image.sh defaults: MYSTIC_RAM=256 MB, MYSTIC_DISK_MB=64")
    print("      this package needs %.1f MB installed + %.1f MB in /tmp/pkg "
          "while it unpacks" % (kept_ins / 1e6, dl / 1e6))
    print("      MYSTIC_RAM=1024 MYSTIC_DISK_MB=2048 ./image.sh   (delete "
          "build/disk.img first: image.sh only creates it once)")

    if args.dry_run:
        print("==> dry run: nothing downloaded, nothing written")
        return 0

    if dl > args.max_download_mb * 1e6 and not args.force:
        print("STOP: the closure is %.1f MB of download, over the %.0f MB cap."
              % (dl / 1e6, args.max_download_mb), file=sys.stderr)
        print("      Nothing was downloaded. Re-run with --force to build it, "
              "or --exclude to drop a package.", file=sys.stderr)
        return 3

    # ---- the tree: cached, or downloaded and unpacked ---------------
    # A second run must be instant: this is 258 MB of download and 682 MB of
    # unpacking, and re-doing it to change one line of the app would be rude.
    # The stamp covers the exact package set and version, nothing else.
    key = ("v%d|" % TREE_VERSION) + "|".join(sorted(real)) + "@" + version + "@" + ",".join(sorted(skipped))
    stamp = hashlib.sha256(key.encode()).hexdigest()
    stamp_path = os.path.join(args.dest, ".engine-stamp")
    cached = (not args.force and os.path.exists(stamp_path) and
              os.path.exists(os.path.join(args.dest, ENGINE_BIN)))
    if cached:
        with open(stamp_path, "r", encoding="utf-8") as f:
            cached = f.read().strip() == stamp
    if cached:
        print("==> tree cached at %s (stamp %s); skipping download and unpack"
              % (args.dest, stamp[:12]))
        owners, links = {}, {}
        if os.path.exists(os.path.join(args.dest, ".engine-owners.json")):
            with open(os.path.join(args.dest, ".engine-owners.json")) as f:
                j = json.load(f)
            owners = j.get("owners", {})
            links = {k: tuple(v) for k, v in j.get("links", {}).items()}
        nbytes, _nf = tree_bytes(args.dest)
    else:
        if os.path.isdir(args.dest):
            shutil.rmtree(args.dest)
        os.makedirs(args.dest, exist_ok=True)

        os.makedirs(APK_CACHE, exist_ok=True)
        print("==> downloading %d packages into %s" % (len(real), APK_CACHE))
        paths, done = [], 0
        for name in sorted(real):
            p = get_apk(pkgs, name, download=True)
            if not p:
                print("    FAILED %s" % name)
                continue
            paths.append((name, p))
            done += 1
            if done % 20 == 0:
                print("    %d/%d" % (done, len(real)))
        got_dl = sum(os.path.getsize(p) for _n, p in paths)
        print("==> %d apks on disk, %.2f MB" % (len(paths), got_dl / 1e6))

        owners, links = {}, {}
        files = nbytes = nlinks = nspecial = 0
        for name, p in paths:
            a, b, c, d = unpack_apk(p, args.dest, name, owners, links)
            files += a
            nbytes += b
            nlinks += c
            nspecial += d
        # Links are recorded, not created: Windows cannot make them, and the
        # archive carries the link member so pkg creates it in the guest.
        print("==> unpacked %d files, %.2f MB, %d links (recorded, not "
              "materialised on this host), %d special files skipped"
              % (files, nbytes / 1e6, nlinks, nspecial))
        print("    index predicted %.2f MB installed for this closure"
              % (ins / 1e6))
        with open(os.path.join(args.dest, ".engine-owners.json"), "w") as f:
            json.dump({"owners": owners, "links": links}, f)
        with open(os.path.join(args.dest, ".engine-links.json"), "w") as f:
            json.dump(links, f)
        with open(stamp_path, "w", encoding="utf-8") as f:
            f.write(stamp + chr(10))

    # ---- the engine binary: interpreter patch ------------------------
    engine = os.path.join(args.dest, *ENGINE_BIN.split("/"))
    if not os.path.exists(engine):
        # --root with any other closure is a resolver/packaging test: unpack it,
        # prove every ELF resolves, and stop before the engine-specific steps.
        # (Used to prove this script's mechanics without a 258 MB download.)
        print("==> %s is not in this closure (--root test run); nothing to patch"
              % ENGINE_BIN)
        unresolved, _names = check_closure(args.dest, LOADER_TOP, INTERP_JAIL,
                                          skipped, owners, links)
        if unresolved and not args.allow_unresolved:
            raise SystemExit("error: %d unresolved DT_NEEDED entries"
                             % len(unresolved))
        print("==> resolver/unpack/closure test passed, %.2f MB unpacked"
              % (nbytes / 1e6))
        return 0
    interp, needed, soname = elf_info(engine)
    print("==> engine: %s" % ENGINE_BIN)
    print("      %.2f MB, PT_INTERP %s, %d DT_NEEDED"
          % (os.path.getsize(engine) / 1e6, interp, len(needed)))
    if interp != INTERP_JAIL:
        res = patch_interp(engine, INTERP_JAIL)
        if not res:
            raise SystemExit("error: %s has no PT_INTERP; it is static, which "
                             "this package does not expect" % ENGINE_BIN)
        print("      PT_INTERP rewritten: %s -> %s (%d bytes at file offset %d)"
              % (res[0], res[1], res[3], res[2]))
    else:
        print("      PT_INTERP already %s" % INTERP_JAIL)

    # ---- the window app ---------------------------------------------
    app_out = os.path.join("build", "app-engineview")
    app_size = build_app(app_out)
    print("==> window app: build/app-engineview, %.0f KB static"
          % (app_size / 1e3))

    # ---- closure check before anything is packed --------------------
    unresolved, names = check_closure(args.dest, LOADER_TOP, INTERP_JAIL,
                                     skipped, owners, links)
    if unresolved and not args.allow_unresolved:
        raise SystemExit(
            "error: %d unresolved DT_NEEDED entries. The package would install "
            "and then fail to load. Re-run with --allow-unresolved to pack "
            "anyway, or add the missing package." % len(unresolved))

    # ---- stage + pack ----------------------------------------------
    links_final, total, nfiles, manifest = build_stage(
        args.dest, owners, dict(links), skipped, app_out)
    if args.no_pack:
        print("==> --no-pack: stage at %s, no tarball" % PKG_STAGE)
        return 0
    entries = stage_entries(PKG_STAGE, links_final)
    tarball = os.path.join(args.repo, PKG_NAME + ".tar.gz")
    size = pack_tarball(entries, tarball)
    print("==> packed %s.tar.gz: %.2f MB from %.2f MB of members (%d entries)"
          % (PKG_NAME, size / 1e6, total / 1e6, len(entries)))
    tsize, sha, row = upsert_index(tarball, total, args.repo)
    print("==> index row: %s" % row)
    print("\ninstall it in the guest:")
    print("    pkg update && pkg install %s" % PKG_NAME)
    return 0


if __name__ == "__main__":
    sys.exit(main())
