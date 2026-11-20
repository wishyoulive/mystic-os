#!/usr/bin/env python3
"""Assemble the "engine" package: the CDP viewer plus the Chromium closure.

The viewer (src/client/apps/engine.c) expects the engine inside its own bundle,
at <bundle>/usr/lib/chromium/chromium-headless-shell, because a sandboxed app
cannot see the rest of the filesystem. This copies the closure produced by
tools/fetch-chromium.py into a bundle layout and adds the viewer as the payload.

Usage:
  python tools/pack-engine.py --chromium build/repo/chromium.tar.gz \
                              --bundle stage/usr/share/mystic/apps/engine \
                              --out build/repo/engine.tar.gz
"""

import argparse
import hashlib
import os
import sys
import tarfile
import time

# The bundle keeps its own payload at the root as a *file* named bin, and the
# Chromium closure carries bin/sh (Alpine's yash). tar cannot create a directory
# where a file of that name exists, so the whole closure bin/ tree is dropped:
# nothing in it is needed to run the engine, which lives under usr/lib.
SKIP_AT_ROOT = {"chromium", "manifest", ".pkg-name", ".pkg-version", "bin"}


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--chromium", default=os.path.join(here, "build", "repo", "chromium.tar.gz"))
    ap.add_argument("--bundle", default=os.path.join(here, "stage", "usr", "share",
                                                    "mystic", "apps", "engine"))
    ap.add_argument("--out", default=os.path.join(here, "build", "repo", "engine.tar.gz"))
    ap.add_argument("--version", default="156.0.8078.79-r0")
    args = ap.parse_args()

    if not os.path.exists(args.chromium):
        sys.exit("missing %s (build it with tools/fetch-chromium.py)" % args.chromium)
    payload_bin = os.path.join(args.bundle, "bin")
    if not os.path.exists(payload_bin):
        sys.exit("missing %s (build it with tools/mkapp.sh engine)" % payload_bin)

    t0 = time.time()
    n_files = n_links = 0
    skipped = 0
    with tarfile.open(args.chromium, "r:gz") as src, \
            tarfile.open(args.out, "w:gz") as dst:

        def add_path(name, path):
            ti = tarfile.TarInfo(name)
            st = os.stat(path)
            ti.size = st.st_size
            with open(path, "rb") as f:
                head = f.read(4)
                f.seek(0)
                mode = 0o755 if (head == b"\x7fELF" or head[:2] == b"#!") else 0o644
                ti.mode = mode
                ti.mtime = int(st.st_mtime)
                dst.addfile(ti, f)

        # the viewer is the payload the launcher runs
        add_path("bin", payload_bin)
        add_path("manifest", os.path.join(args.bundle, "manifest"))
        n_files += 2

        for ti in src:
            name = ti.name.lstrip("./")
            if not name or name.split("/")[0] in SKIP_AT_ROOT:
                skipped += 1
                continue
            dst.addfile(ti, src.extractfile(ti) if ti.isreg() else None)
            if ti.issym() or ti.islnk():
                n_links += 1
            else:
                n_files += 1

        for extra, text in ((".pkg-name", "engine\n"),
                            (".pkg-version", args.version + "\n")):
            data = text.encode()
            ti = tarfile.TarInfo(extra)
            ti.size = len(data)
            ti.mode = 0o644
            dst.addfile(ti, __import__("io").BytesIO(data))

    h = hashlib.sha256()
    with open(args.out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    size = os.path.getsize(args.out)
    print("wrote %s" % args.out)
    print("  %d files, %d links, %d chromium-root entries skipped"
          % (n_files, n_links, skipped))
    print("  %.1f MB, sha256 %s" % (size / 1048576, h.hexdigest()))
    print("  packed in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
