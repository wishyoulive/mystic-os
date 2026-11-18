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
    ap.add_argument('--modules-kver', default=None,
                    help='pack only lib/modules/<kver>; the other kernel variant in stage/ is skipped')
    ap.add_argument('--gzip-level', type=int, default=9,
                    help='cpio.gz compression level (9 = smallest, 1 = fastest)')
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
    # stage/lib/modules holds one directory per kernel variant (6.18.63-0-virt
    # from linux-virt, 6.18.63-0-lts from linux-lts). Packing both would add
    # ~30 MB to an image that can only ever use one of them, so --modules-kver
    # keeps the one the boot kernel will ask for and reports the rest.
    def kver_of(rel):
        parts = rel.split('/')
        if len(parts) >= 3 and parts[0] == 'lib' and parts[1] == 'modules':
            return parts[2]
        return None

    mod_files = mod_bytes = 0
    skipped_files = skipped_bytes = 0
    skipped_kvers = set()

    if args.stage:
        for root, dirs, files in os.walk(args.stage):
            for d in list(dirs):
                full = os.path.join(root, d)
                rel = os.path.relpath(full, args.stage).replace(os.sep, '/')
                k = kver_of(rel)
                if args.modules_kver and k and k != args.modules_kver:
                    dirs.remove(d)          # do not descend into the other kernel
                    skipped_kvers.add(k)
                    # count what was left out, so the report says how much
                    for r2, _d2, f2 in os.walk(full):
                        for f2n in f2:
                            skipped_files += 1
                            skipped_bytes += os.path.getsize(os.path.join(r2, f2n))
                    continue
                add(rel, S_IFDIR | 0o755, int(os.path.getmtime(full)))
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, args.stage).replace(os.sep, '/')
                k = kver_of(rel)
                sz = os.path.getsize(full)
                if args.modules_kver and k and k != args.modules_kver:
                    skipped_files += 1
                    skipped_bytes += sz
                    skipped_kvers.add(k)
                    continue
                if k:
                    mod_files += 1
                    mod_bytes += sz
                st = os.stat(full)
                # Windows has no executable bit, so decide by path:
                # anything that lives where a program lives is 0755.
                exec_dirs = ('bin/', 'sbin/', 'usr/bin/', 'usr/sbin/', 'usr/local/bin/')
                # an app bundle's payload is called 'bin' and lives under
                # usr/share/mystic/apps/<id>/ - it must be executable too
                is_prog = (rel == 'init' or rel.startswith(exec_dirs)
                           or rel.endswith('/bin')
                           # udhcpc execs its script directly, so it needs +x
                           or rel.endswith('.script')
                           # our own shell tools in usr/bin need +x too
                           or rel.startswith('usr/bin/'))
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
    with gzip.open(args.output, 'wb', compresslevel=args.gzip_level) as f:
        f.write(payload)

    n_files = sum(1 for m in entries.values() if (m[0] & 0o170000) == S_IFREG)
    n_links = sum(1 for m in entries.values() if (m[0] & 0o170000) == S_IFLNK)
    print('entries: %d files, %d symlinks, %d dirs' % (n_files, n_links, len(entries) - n_files - n_links))
    if mod_files:
        print('modules: %d files, %.1f MB for %s' % (mod_files, mod_bytes / 1e6, args.modules_kver))
    if skipped_files or skipped_kvers:
        print('modules: skipped %d files, %.1f MB from %s (not this kernel)'
              % (skipped_files, skipped_bytes / 1e6, ', '.join(sorted(skipped_kvers))))
    print('cpio:    %.1f MB raw' % (len(payload) / 1e6))
    print('output:  %s  %.1f MB (gzip -%d)' % (args.output, os.path.getsize(args.output) / 1e6, args.gzip_level))


if __name__ == '__main__':
    main()
