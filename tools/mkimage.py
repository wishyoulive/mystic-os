#!/usr/bin/env python3
"""
mkimage.py - build a bootable Linux initramfs from distro tarballs.

Why this exists: Windows cannot create the hundreds of symlinks a Linux
rootfs is built from (no admin, no symlink privilege). So we never extract
to disk. We read the tarballs, and emit a cpio "newc" archive - a format
that stores symlinks as data, not as filesystem objects - then gzip it.

  python mkimage.py --base alpine-minirootfs.tar.gz --stage stage/ -o build/initramfs.cpio.gz

Anything in `stage/` is laid on top of the base rootfs (later files win).
"""

import argparse
import gzip
import io
import os
import sys
import tarfile

S_IFREG = 0o100000
S_IFDIR = 0o040000
S_IFLNK = 0o120000


class CpioWriter:
    """cpio newc writer. Written by hand because `cpio` isn't on Windows."""

    def __init__(self, out):
        self.out = out
        self.ino = 1

    def _hdr(self, name, mode, size, nlink, mtime, devmajor=0, devminor=0, rdevmajor=0, rdevminor=0):
        self.out.write(b'070701')
        for v in (self.ino, mode, 0, 0, nlink, mtime, size,
                  devmajor, devminor, rdevmajor, rdevminor, len(name) + 1, 0):
            self.out.write(b'%08X' % (v & 0xFFFFFFFF))
        self.ino += 1
        self.out.write(name.encode() + b'\0')
        pad = (-(110 + len(name) + 1)) % 4
        self.out.write(b'\0' * pad)

    def _data(self, data):
        self.out.write(data)
        self.out.write(b'\0' * ((-len(data)) % 4))

    def entry(self, name, mode, mtime, data=b'', nlink=1):
        self._hdr(name, mode, len(data), nlink, mtime)
        if data:
            self._data(data)

    def trailer(self):
        self._hdr('TRAILER!!!', 0, 0, 1, 0)
        self.out.write(b'\0' * 512)


def normalize(name):
    name = name.lstrip('./')
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', action='append', default=[], help='base rootfs tarball(s)')
    ap.add_argument('--stage', default=None, help='directory overlaid on top')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    entries = {}   # path -> (mode, mtime, data_or_None, symlink_target_or_None)
    order = []

    def add(path, mode, mtime, data=None, target=None):
        if path not in entries:
            order.append(path)
        entries[path] = (mode, mtime, data, target)

    # ---- base tarballs -------------------------------------------------
    for tb in args.base:
        with tarfile.open(tb, 'r:*') as tf:
            for m in tf:
                name = normalize(m.name).rstrip('/')
                if not name:
                    continue
                if m.isdir():
                    add(name, S_IFDIR | 0o755, int(m.mtime), target=None)
                elif m.issym():
                    add(name, S_IFLNK | 0o777, int(m.mtime), target=m.linkname)
                elif m.islnk():
                    add(name, S_IFLNK | 0o777, int(m.mtime), target=normalize(m.linkname))
                elif m.isfile():
                    add(name, S_IFREG | (m.mode & 0o7777), int(m.mtime), data=tf.extractfile(m).read())

    # ---- stage overlay -------------------------------------------------
    if args.stage:
        for root, dirs, files in os.walk(args.stage):
            for d in dirs:
                full = os.path.join(root, d)
                rel = os.path.relpath(full, args.stage).replace(os.sep, '/')
                add(rel, S_IFDIR | 0o755, int(os.path.getmtime(full)))
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, args.stage).replace(os.sep, '/')
                st = os.stat(full)
                # Windows has no executable bit, so decide by path:
                # anything that lives where a program lives is 0755.
                exec_dirs = ('bin/', 'sbin/', 'usr/bin/', 'usr/sbin/', 'usr/local/bin/')
                is_prog = rel == 'init' or rel.startswith(exec_dirs)
                mode = 0o755 if is_prog else 0o644
                add(rel, S_IFREG | mode, int(st.st_mtime), data=open(full, 'rb').read())

    # ---- parents must exist --------------------------------------------
    for path in list(order):
        parts = path.split('/')
        for i in range(1, len(parts)):
            parent = '/'.join(parts[:i])
            if parent and parent not in entries:
                add(parent, S_IFDIR | 0o755, 0, target=None)

    # ---- emit ----------------------------------------------------------
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    raw = io.BytesIO()
    w = CpioWriter(raw)
    # directories first, so every parent exists before its children
    for path in sorted(order, key=lambda p: (p.count('/'), p)):
        mode, mtime, data, target = entries[path]
        if (mode & 0o170000) == S_IFDIR:
            nlink = 2 + sum(1 for o in entries if o.startswith(path + '/') and o.count('/') == path.count('/') + 1)
            w.entry(path, mode, mtime, b'', nlink=nlink)
        elif (mode & 0o170000) == S_IFLNK:
            if target is None:
                raise SystemExit('symlink with no target: %r mode=%o' % (path, mode))
            w.entry(path, mode, mtime, target.encode(), nlink=1)
        else:
            w.entry(path, mode, mtime, data or b'')
    w.trailer()

    payload = raw.getvalue()
    with gzip.open(args.output, 'wb', compresslevel=9) as f:
        f.write(payload)

    n_files = sum(1 for m in entries.values() if (m[0] & 0o170000) == S_IFREG)
    n_links = sum(1 for m in entries.values() if (m[0] & 0o170000) == S_IFLNK)
    print('entries: %d files, %d symlinks, %d dirs' % (n_files, n_links, len(entries) - n_files - n_links))
    print('cpio:    %.1f MB raw' % (len(payload) / 1e6))
    print('output:  %s  %.1f MB' % (args.output, os.path.getsize(args.output) / 1e6))


if __name__ == '__main__':
    main()
