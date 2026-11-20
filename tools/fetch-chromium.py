#!/usr/bin/env python3
"""Build a self-contained Chromium package for Mystic OS from Alpine's APK indexes.

Why this exists: a browser engine is too large to build from source inside this
project, and the only honest way to run one is to carry a real, verified Linu
binary closure. This resolves the full dependency set for chromium-headless-shell
(the Blink + V8 + DevTools build, no GTK, no X11) from Alpine's package indexes,
downloads every .apk, and writes a single tarball the package manager can install.

The output is self-describing: .pkg-name and .pkg-version are read back by `pkg`.

Usage:
  python tools/fetch-chromium.py --branch v3.24 --out build/repo/chromium.tar.gz
"""

import argparse
import hashlib
import os
import re
import sys
import tarfile
import time
import urllib.request
import json

MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
ARCH = "x86_64"

# Packages that belong to the base system, not to the browser. They arrive as
# dependencies (chromium depends on musl and on /bin/sh) and unpacking them would
# overwrite the running system: Alpine's busybox would replace Mystic's shell and
# its musl would shadow the one the whole userland is linked against.
SYSTEM_PACKAGES = {
    "alpine-base", "alpine-baselayout", "alpine-baselayout-data", "alpine-conf",
    "alpine-keys", "apk-tools", "busybox", "busybox-binsh", "busybox-extras",
    "busybox-suid", "libc-utils", "musl", "musl-utils", "openrc", "yash",
    "scanelf", "ssl_client", "ca-certificates", "ca-certificates-bundle",
}


def log(msg):
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def fetch(url, dest, want=None):
    """Download url to dest, resuming until the byte count matches want.

    A server that closes a connection early is not an error python reports, so
    the size is checked after every pass and the transfer resumes with a Range
    request until it is complete. This is the difference between a package that
    installs and one that fails a checksum three steps later."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for attempt in range(6):
        have = os.path.getsize(dest) if os.path.exists(dest) else 0
        if want is not None and have == want:
            return have
        if want is not None and have > want:
            os.remove(dest)
            have = 0
        req = urllib.request.Request(url)
        if have:
            req.add_header("Range", "bytes=%d-" % have)
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                mode = "ab" if have and r.status == 206 else "wb"
                if mode == "wb":
                    have = 0
                with open(dest, mode) as f:
                    while True:
                        chunk = r.read(1 << 16)
                        if not chunk:
                            break
                        f.write(chunk)
        except Exception as e:
            if attempt == 5:
                raise
            log("  retry %d for %s: %s" % (attempt + 1, os.path.basename(dest), e))
            time.sleep(2 + attempt * 2)
            continue
        got = os.path.getsize(dest)
        if want is None or got == want:
            return got
        log("  short read %s: %.1f of %.1f MB, resuming"
            % (os.path.basename(dest), got / 1048576, want / 1048576))
        time.sleep(1)
    raise SystemExit("%s: incomplete after retries" % dest)


def load_index(branch, repo, cache_dir):
    """Return the parsed APKINDEX for one repository, cached on disk."""
    path = os.path.join(cache_dir, "%s-%s-%s.INDEX" % (branch, repo, ARCH))
    if not os.path.exists(path):
        url = "%s/%s/%s/%s/APKINDEX.tar.gz" % (MIRROR, branch, repo, ARCH)
        tmp = path + ".tar.gz"
        log("index: %s" % url)
        fetch(url, tmp)
        with tarfile.open(tmp) as t:
            m = t.getmember("APKINDEX")
            data = t.extractfile(m).read()
        with open(path, "wb") as f:
            f.write(data)
        os.remove(tmp)
    return parse_index(path)


def parse_index(path):
    pkgs, cur = [], {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if cur:
                    pkgs.append(cur)
                    cur = {}
            elif len(line) > 1 and line[1] == ":" and line[0].isalpha():
                cur[line[0]] = line[2:]
    if cur:
        pkgs.append(cur)
    return pkgs


def version_key(v):
    nums = [int(x) for x in re.findall(r"\d+", v)[:5]]
    return nums + [0] * (5 - len(nums))


class Universe(object):
    """Name and provided-name lookup over every package in both repositories."""

    def __init__(self, pkgs):
        self.by_name = {}
        self.by_provide = {}
        for p in pkgs:
            name = p.get("P")
            if not name:
                continue
            self.by_name.setdefault(name, []).append(p)
            self.by_provide.setdefault(name, []).append(p)
            for d in (p.get("p") or "").split():
                self.by_provide.setdefault(re.split(r"[=<>~]", d)[0], []).append(p)

    def pick(self, name):
        for table in (self.by_name, self.by_provide):
            cands = [p for p in table.get(name, []) if p.get("A") in (ARCH, "noarch")]
            if cands:
                return max(cands, key=lambda p: version_key(p.get("V", "0")))
        return None

    def closure(self, roots):
        chosen, queue, seen = {}, list(roots), set()
        while queue:
            name = queue.pop(0)
            if name in seen:
                continue
            seen.add(name)
            p = self.pick(name)
            if not p:
                raise SystemExit("cannot resolve dependency: %s" % name)
            chosen[p["P"]] = p
            for d in (p.get("D") or "").split():
                dn = re.split(r"[=<>~]", d)[0]
                if dn and not dn.startswith("!") and dn not in seen:
                    queue.append(dn)
        return chosen


def apk_path(branch, pkg):
    return "%s/%s/%s/%s/%s-%s.apk" % (
        MIRROR, branch, pkg["repo"], ARCH, pkg["P"], pkg["V"])


def extract_apk(apk, root, symlinks):
    """Unpack one .apk into root. Symlinks are recorded, not created: Windows
    cannot make them without elevation, so the tarball writer emits them as tar
    entries instead."""
    with tarfile.open(apk) as t:
        for m in t.getmembers():
            name = m.name.lstrip("./")
            if not name or name.startswith(".SIGN") or name == ".PKGINFO":
                continue
            dest = os.path.join(root, name)
            if m.isdir():
                os.makedirs(dest, exist_ok=True)
            elif m.issym():
                symlinks[name] = m.linkname
            elif m.islnk():
                symlinks[name] = m.linkname
            elif m.isreg():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                src = t.extractfile(m)
                if src is None:
                    continue
                with open(dest, "wb") as f:
                    while True:
                        chunk = src.read(1 << 16)
                        if not chunk:
                            break
                        f.write(chunk)
                os.chmod(dest, m.mode & 0o777)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--branch", default="v3.24")
    ap.add_argument("--root", default=None, help="staging directory (default build/chromium-root)")
    ap.add_argument("--out", default=None, help="tarball path (default build/repo/chromium.tar.gz)")
    ap.add_argument("--cache", default=None, help="apk download cache (default build/cache/apk)")
    ap.add_argument("--packages", default="chromium-headless-shell",
                    help="space separated root packages")
    ap.add_argument("--id", default="chromium",
                    help="bundle id recorded in .pkg-name (must match the index row)")
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = args.root or os.path.join(here, "build", "chromium-root")
    out = args.out or os.path.join(here, "build", "repo", "chromium.tar.gz")
    cache = args.cache or os.path.join(here, "build", "cache", "apk")
    index_cache = os.path.join(here, "build", "cache")

    pkgs = []
    for repo in ("main", "community"):
        pkgs += [{**p, "repo": repo} for p in load_index(args.branch, repo, index_cache)]
    uni = Universe(pkgs)

    roots = args.packages.split()
    chosen = uni.closure(roots)
    dropped = sorted(n for n in chosen if n in SYSTEM_PACKAGES)
    for n in dropped:
        del chosen[n]
    if dropped:
        log("skipped %d base-system package(s): %s" % (len(dropped), " ".join(dropped)))
    total_dl = sum(int(p.get("S") or 0) for p in chosen.values())
    total_in = sum(int(p.get("I") or 0) for p in chosen.values())
    log("closure: %d packages, %.1f MB download, %.1f MB installed"
        % (len(chosen), total_dl / 1048576, total_in / 1048576))

    os.makedirs(cache, exist_ok=True)
    got_bytes = 0
    apks = []
    for i, (name, p) in enumerate(sorted(chosen.items()), 1):
        dest = os.path.join(cache, "%s-%s.apk" % (name, p["V"]))
        want = int(p.get("S") or 0)
        if not os.path.exists(dest) or os.path.getsize(dest) != want:
            fetch(apk_path(args.branch, p), dest, want)
        size = os.path.getsize(dest)
        if size != want:
            raise SystemExit("%s: expected %d bytes, got %d" % (name, want, size))
        got_bytes += size
        apks.append(dest)
        log("  [%3d/%3d] %-28s %7.1f MB" % (i, len(chosen), name, size / 1048576))

    log("downloaded %.1f MB into %s" % (got_bytes / 1048576, cache))

    if os.path.exists(root):
        for dp, dn, fn in os.walk(root, topdown=False):
            for f in fn:
                os.remove(os.path.join(dp, f))
            for d in dn:
                os.rmdir(os.path.join(dp, d))
    os.makedirs(root, exist_ok=True)

    symlinks = {}
    for i, apk in enumerate(apks, 1):
        extract_apk(apk, root, symlinks)
        log("  unpack [%3d/%3d] %s" % (i, len(apks), os.path.basename(apk)))

    # The headless shell loads its resource paks from the directory of the
    # executable, but Alpine installs them under /usr/lib/chromium, so link them
    # next to the binary instead of copying (the paks are tens of megabytes).
    pak_dir = os.path.join(root, "usr", "lib", "chromium")
    if os.path.isdir(pak_dir):
        for name in sorted(os.listdir(pak_dir)):
            if name.endswith(".pak"):
                symlinks["usr/bin/" + name] = "../lib/chromium/" + name

    main_pkg_version = chosen[roots[0]]["V"]

    # The dynamic loader searches the paths it is given, not their subdirectories,
    # and this closure keeps a few libraries in private directories (pulseaudio's
    # helpers, chromium's own). Collect every directory that holds a shared object
    # so the launcher passes one complete LD_LIBRARY_PATH.
    lib_dirs = []
    for dp, dn, fn in os.walk(root):
        if any(".so" in f for f in fn):
            rel_dir = os.path.relpath(dp, root).replace("\\", "/")
            lib_dirs.append(rel_dir)
    lib_dirs.sort(key=lambda d: (d.count("/"), d))
    lib_path = ":".join('$d/' + d for d in lib_dirs if d != ".") or "$d/lib"

    # pkg only accepts a bare exec name in the bundle root, and the dynamic
    # loader needs the bundled libraries, so the payload is this launcher.
    launcher = os.path.join(root, "chromium")
    script = [
        "#!/bin/sh",
        "# Chromium (headless shell) from the signed package repository.",
        "# The engine is a real Blink build; Mystic drives it through the",
        "# DevTools protocol and draws the frames it returns.",
        'd=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)',
        'export LD_LIBRARY_PATH="' + lib_path + '${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
        'export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}"',
        'exec "$d/usr/bin/chromium-headless-shell" --no-sandbox --disable-gpu "$@"',
    ]
    with open(launcher, "w", newline=chr(10)) as f:
        f.write(chr(10).join(script) + chr(10))
    os.chmod(launcher, 0o755)

    manifest = [
        "name = Chromium",
        "desc = Chromium browser engine (Blink and V8), driven over DevTools",
        "exec = chromium",
        "version = " + main_pkg_version,
        "bundle = chromium",
        "network = host",
    ]
    with open(os.path.join(root, "manifest"), "w", newline=chr(10)) as f:
        f.write(chr(10).join(manifest) + chr(10))

    main_pkg = chosen[roots[0]]
    with open(os.path.join(root, ".pkg-name"), "w") as f:
        f.write(args.id + "\n")
    with open(os.path.join(root, ".pkg-version"), "w") as f:
        f.write(main_pkg["V"] + "\n")

    log("symlinks: %d (emitted as tar entries)" % len(symlinks))

    os.makedirs(os.path.dirname(out), exist_ok=True)
    entries = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, root).replace("\\", "/")
            entries.append((rel, full))
    for rel in symlinks:
        entries.append((rel, None))
    entries.sort()

    t0 = time.time()
    with tarfile.open(out, "w:gz") as t:
        for rel, full in entries:
            ti = tarfile.TarInfo(rel)
            if full is None:
                target = symlinks[rel]
                if os.path.isabs(target):
                    # inside the installed tree an absolute target would point at
                    # the jail root; keep it only if it resolves within the package
                    rel_target = os.path.relpath(target.lstrip("/"), os.path.dirname(rel) or ".")
                    target = rel_target
                    if target.startswith("../../../../"):
                        log("  skip symlink outside package: %s -> %s" % (rel, target))
                        continue
                ti.type = tarfile.SYMTYPE
                ti.linkname = target
                ti.mode = 0o777
                t.addfile(ti)
                continue
            st = os.stat(full)
            ti.size = st.st_size
            # Windows cannot represent the execute bit, so the mode is decided
            # from the content instead of from the host filesystem: an ELF image
            # or a script with a shebang is executable, everything else is not.
            mode = 0o644
            try:
                with open(full, "rb") as probe:
                    head = probe.read(4)
                if head == b"ELF" or head[:2] == b"#!":
                    mode = 0o755
            except OSError:
                pass
            ti.mode = mode
            ti.mtime = int(st.st_mtime)
            with open(full, "rb") as f:
                t.addfile(ti, f)

    h = hashlib.sha256()
    with open(out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    size = os.path.getsize(out)
    log("wrote %s" % out)
    log("  %d entries, %.1f MB, sha256 %s" % (len(entries), size / 1048576, h.hexdigest()))
    log("  packed in %.1f s" % (time.time() - t0))

    meta = {
        "package": args.id,
        "version": main_pkg["V"],
        "branch": args.branch,
        "packages": sorted(chosen),
        "download_bytes": got_bytes,
        "installed_bytes": total_in,
        "tarball_bytes": size,
        "tarball_sha256": h.hexdigest(),
    }
    meta_path = os.path.join(os.path.dirname(out), "chromium.meta.json")
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=1)
    log("  metadata %s" % meta_path)


if __name__ == "__main__":
    main()
