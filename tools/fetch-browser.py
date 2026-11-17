#!/usr/bin/env python3
"""
fetch-browser.py - download the browser engine closure for Mystic OS.

The OS stays small; the engine is a separate tree that the VM mounts read-only
over 9p. This script resolves the real dependency closure from the Alpine
package indexes and extracts it to build/browser-root/.

Why a resolver: chromium-headless-shell pulls in ~600 packages (X libraries it
does not use, ICU data, fonts, libLLVM...). Guessing the list is how you get a
browser that starts and then dies on a missing .so, so the closure is computed
from the indexes' own D: fields, including the `so:libfoo.so.1` provides form.

    python tools/fetch-browser.py [--root chromium-headless-shell] [--dest DIR]

Cached in build/browser-apks/, so a second run is instant.
"""

import argparse
import io
import os
import re
import sys
import tarfile
import urllib.request

ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.24"
REPOS = ["main", "community"]
ARCH = "x86_64"
CACHE = os.path.join("build", "browser-apks")
DEST = os.path.join("build", "browser-root")


def fetch(url, path=None):
    req = urllib.request.Request(url, headers={"User-Agent": "mystic-os-fetch"})
    data = urllib.request.urlopen(req, timeout=60).read()
    if path:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    return data


def load_index(repo):
    """Return {package: {version, file, size, depends[]}} for one repo."""
    blob = fetch("%s/%s/%s/APKINDEX.tar.gz" % (ALPINE, repo, ARCH))
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
            "size": int((d.get("I") or ["0"])[0] or 0),
            "depends": ((d.get("D") or [""])[0] or "").split(),
            "repo": repo,
        }
    return pkgs


def resolve(pkgs, provides, roots):
    """Depth-first closure. Handles plain names and `so:`/`cmd:` provides."""
    want, seen, missing = list(roots), set(), set()
    while want:
        name = want.pop()
        if name in seen:
            continue
        seen.add(name)
        pkg = pkgs.get(name)
        if pkg is None:
            # a provide (so:libcrypto.so.3) is satisfied by some package
            owner = provides.get(name)
            if owner and owner not in seen:
                want.append(owner)
            elif not name.startswith(("so:", "cmd:", "pc:")):
                missing.add(name)
            continue
        for dep in pkg["depends"]:
            if dep.startswith(("!",)):        # conflict, not a dependency
                continue
            dep = re.split(r"[=<>~]", dep)[0]
            if dep and dep not in seen:
                want.append(dep)
    return seen, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", action="append", default=None)
    ap.add_argument("--dest", default=DEST)
    ap.add_argument("--list", action="store_true", help="only print the closure")
    args = ap.parse_args()
    roots = args.root or ["chromium-headless-shell"]

    pkgs, provides = {}, {}
    for repo in REPOS:
        idx = load_index(repo)
        print("==> %s: %d packages" % (repo, len(idx)))
        for name, info in idx.items():
            pkgs.setdefault(name, info)
        # provides index: every package that declares `so:foo` or `cmd:foo`
        for name, info in idx.items():
            for dep in info["depends"]:
                pass
        # Alpine lists provides in `p:` (lower case) - parse it separately
    # second pass: provides come from the `p:` field, which load_index skipped
    for repo in REPOS:
        blob = fetch("%s/%s/%s/APKINDEX.tar.gz" % (ALPINE, repo, ARCH))
        txt = tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz"
                           ).extractfile("APKINDEX").read().decode("utf-8", "replace")
        cur = None
        for line in txt.split("\n"):
            if line.startswith("P:"):
                cur = line[2:]
            elif line.startswith("p:") and cur:
                for prov in line[2:].split():
                    provides.setdefault(prov.split("=")[0], cur)

    print("==> provides: %d" % len(provides))
    closure, missing = resolve(pkgs, provides, roots)
    total = sum(pkgs[n]["size"] for n in closure if n in pkgs)
    print("==> closure: %d packages, %.1f MB compressed" % (len(closure), total / 1e6))
    if missing:
        print("    not in the index (skipped): %s" % ", ".join(sorted(missing)[:8]))
    if args.list:
        for n in sorted(closure):
            if n in pkgs:
                print("  %-34s %s" % (n, pkgs[n]["version"]))
        return 0

    os.makedirs(CACHE, exist_ok=True)
    os.makedirs(args.dest, exist_ok=True)

    files, done, size = [], 0, 0
    for name in sorted(closure):
        info = pkgs.get(name)
        if not info or not info["file"]:
            continue
        path = os.path.join(CACHE, info["file"])
        if not os.path.exists(path):
            url = "%s/%s/%s/%s" % (ALPINE, info["repo"], ARCH, info["file"])
            try:
                fetch(url, path)
            except Exception as e:
                print("    FAILED %s: %s" % (info["file"], e))
                continue
        size += os.path.getsize(path)
        files.append(path)
        done += 1
        if done % 25 == 0:
            print("    %d/%d packages, %.0f MB" % (done, len(closure), size / 1e6))
    print("==> downloaded %d packages, %.1f MB" % (done, size / 1e6))

    # extract; apks are tar.gz plus a signature, and Windows cannot make
    # symlinks, so links become copies in a second pass
    links = []
    for path in files:
        try:
            tf = tarfile.open(path, "r:gz")
        except Exception:
            continue
        for m in tf:
            if m.name.startswith(".SIGN") or m.name in (".PKGINFO",):
                continue
            target = os.path.join(args.dest, m.name)
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
        tf.close()

    fixed = 0
    for target, linkname in links:
        src = linkname if os.path.isabs(linkname) else os.path.join(os.path.dirname(target), linkname)
        src = os.path.normpath(src)
        for cand in (src, os.path.join(args.dest, linkname.lstrip("/"))):
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
    print("==> extracted to %s (%d symlinks materialised as copies)" % (args.dest, fixed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
