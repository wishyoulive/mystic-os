#!/usr/bin/env python3
"""check_initramfs.py - refuse an initramfs that carries runtime state.

    python tools/check_initramfs.py build/initramfs.cpio.gz

Why this exists: /run is runtime state. `stage/init` creates /run/mystic on
every boot, and `tools/mkimage.py` packs `stage/` wholesale, so a file left in
`stage/run/` reaches the image. For one path that is not cosmetic:
`src/input.c:448-458` opens /run/mystic/input-script at boot when
MYSTIC_INPUT_TEST is unset and replays it as a scripted input device - the copy
found in this tree typed `rm -rf /root/apps/engine` and `pkg install engine`
into whatever window had focus.

Two artifacts are built from the same initramfs, so both call this:
`tools/release.sh` (the zip) and `tools/mkiso.py` (the bootable ISO). A test
image may carry the hook on purpose (`tools/mkimage.py` warns when it does);
anything you would hand to someone else must not.

Exit status: 0 clean, 1 the hook is present (the member is printed),
2 the archive could not be read.
"""

import gzip
import sys

HOOK = 'run/mystic/input-script'


def scan(path):
    """Names in the archive that are the scripted-input hook (usually 0 or 1)."""
    found = []
    with gzip.open(path, 'rb') as f:
        while True:
            hdr = f.read(110)
            if not hdr:
                raise ValueError('truncated archive: no TRAILER!!! entry')
            if len(hdr) < 110:
                raise ValueError('truncated header (%d bytes)' % len(hdr))
            if hdr[:6] not in (b'070701', b'070702'):
                raise ValueError('not a newc cpio archive (magic %r)' % hdr[:6])
            fld = [int(hdr[6 + i * 8:14 + i * 8], 16) for i in range(13)]
            size, namesize = fld[6], fld[11]
            if namesize < 1 or namesize > 4096:
                raise ValueError('implausible name length %d' % namesize)
            name = f.read(namesize)[:-1].decode('utf-8', 'replace')
            f.read((-(110 + namesize)) % 4)
            if name == 'TRAILER!!!':
                return found
            if name.lstrip('./') == HOOK:
                found.append(name)
            f.read(size)
            f.read((-size) % 4)


def main(argv):
    if len(argv) != 1:
        print('usage: check_initramfs.py <initramfs.cpio.gz>', file=sys.stderr)
        return 2
    path = argv[0]
    try:
        found = scan(path)
    except (OSError, ValueError, EOFError) as e:
        print('%s: cannot read as a gzip cpio archive: %s' % (path, e),
              file=sys.stderr)
        return 2
    if found:
        print(found[0])
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
