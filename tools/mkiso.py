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
