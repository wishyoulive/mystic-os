#!/usr/bin/env python3
"""Build the bootable ISO from the CURRENT kernel and initramfs.

    python tools/mkiso.py

Why this file exists twice over: the owner spent an evening testing a
ten-hour-old OS because launch.bat boots an ISO and nothing rebuilt the ISO
after image.sh changed the initramfs. So this reads whatever
build/initramfs.cpio.gz holds, right now.

The format: ISO 9660 with an El Torito no-emulation entry pointing at
SYSLINUX, which loads /vmlinuz and /initramfs.gz. My first version wrote the
ISO by hand and SeaBIOS rejected it with "Could not read from CDROM (code
0005)"; pycdlib produces the same structure correctly, so it is used here.
"""
import os
import sys

import pycdlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

SYSL = 'build/iso/syslinux-6.03'
OUT = 'build/release/mystic-os.iso'
ISOLINUX = SYSL + '/bios/core/isolinux.bin'
LDLINUX = SYSL + '/bios/com32/elflink/ldlinux/ldlinux.c32'

SYSL_URL = ('https://mirrors.edge.kernel.org/pub/linux/utils/boot/syslinux/'
            'syslinux-6.03.tar.xz')
SYSL_CACHE = 'build/cache/syslinux-6.03.tar.xz'


def need_syslinux():
    """Fetch the two boot files the ISO needs, once.

    These used to be unpacked by hand into build/iso, which meant the ISO step
    broke whenever the build directory was cleaned. They are small, they come
    from the upstream release, and only two members are needed.
    """
    if os.path.exists(ISOLINUX) and os.path.exists(LDLINUX):
        return
    import tarfile, urllib.request
    if not os.path.exists(SYSL_CACHE):
        os.makedirs(os.path.dirname(SYSL_CACHE), exist_ok=True)
        print('iso: fetching syslinux 6.03 (%s)' % SYSL_URL)
        urllib.request.urlretrieve(SYSL_URL, SYSL_CACHE)
    want = {
        'syslinux-6.03/bios/core/isolinux.bin': ISOLINUX,
        'syslinux-6.03/bios/com32/elflink/ldlinux/ldlinux.c32': LDLINUX,
    }
    with tarfile.open(SYSL_CACHE, 'r:xz') as t:
        for member, dest in want.items():
            src = t.extractfile(member)
            if src is None:
                sys.exit('iso: %s not in %s' % (member, SYSL_CACHE))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as f:
                f.write(src.read())
            print('iso: %s (%d bytes)' % (dest, os.path.getsize(dest)))


need_syslinux()
REQUIRED = ['build/cache/vmlinuz-virt', 'build/initramfs.cpio.gz', ISOLINUX, LDLINUX]
missing = [f for f in REQUIRED if not os.path.exists(f)]
if missing:
    sys.exit('missing: %s (run ./image.sh and tools/fetch-kernel.sh first)'
             % ', '.join(missing))

CONFIG = '\n'.join([
    'DEFAULT mystic',
    'PROMPT 0',
    'TIMEOUT 0',
    'LABEL mystic',
    '  KERNEL /vmlinuz',
    '  APPEND rdinit=/init console=ttyS0 loglevel=4',
    '  INITRD /initramfs.gz',
    '',
])
with open('build/iso/isolinux.cfg', 'w', newline='\n') as f:
    f.write(CONFIG)

iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, vol_ident='MYSTIC_OS', joliet=3)
for src, iso_path, joliet_path in [
        ('build/cache/vmlinuz-virt', '/VMLINUZ.;1', '/vmlinuz'),
        ('build/initramfs.cpio.gz', '/INITRAMFS.GZ;1', '/initramfs.gz'),
        (LDLINUX, '/LDLINUX.C32;1', '/ldlinux.c32'),
        (ISOLINUX, '/ISOLINUX.BIN;1', '/isolinux.bin'),
        ('build/iso/isolinux.cfg', '/ISOLINUX.CFG;1', '/isolinux.cfg')]:
    iso.add_file(src, iso_path=iso_path, joliet_path=joliet_path)

iso.add_eltorito('/ISOLINUX.BIN;1', boot_load_size=4, media_name='noemul',
                 boot_info_table=True)

os.makedirs('build/release', exist_ok=True)
if os.path.exists(OUT):
    os.remove(OUT)
iso.write(OUT)
iso.close()

size = os.path.getsize(OUT)
print('iso: %s  %d bytes (%.1f MB)' % (OUT, size, size / 1048576.0))
