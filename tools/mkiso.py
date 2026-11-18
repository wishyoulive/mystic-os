#!/usr/bin/env python3
"""Build the bootable ISO from the CURRENT kernel and initramfs.

    python tools/mkiso.py

The first version of this script wrote ISO 9660 by hand and SeaBIOS rejected
it; pycdlib produces the same structure correctly. What matters as much as
the format is that this is run after every image rebuild - the owner spent an
evening testing a ten-hour-old ISO because nothing did.
"""
import os, sys
import pycdlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SYSL = 'build/iso/syslinux-6.03'
OUT = 'build/release/mystic-os.iso'

need = ['build/cache/vmlinuz-virt', 'build/initramfs.cpio.gz',
        SYSL + '/bios/core/isolinux.bin',
        SYSL + '/bios/com32/elflink/ldlinux/ldlinux.c32']
for f in need:
    if not os.path.exists(f):
        sys.exit('missing %s - run ./image.sh first' % f)

open('build/iso/isolinux.cfg', 'w').write(
    'DEFAULT mystic
PROMPT 0
TIMEOUT 0
LABEL mystic
'
    '  KERNEL /vmlinuz
'
    '  APPEND rdinit=/init console=ttyS0 loglevel=4
'
    '  INITRD /initramfs.gz
')

iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, vol_ident='MYSTIC_OS', joliet=3)
for src, iso_path, joliet in [
        ('build/cache/vmlinuz-virt', '/VMLINUZ.;1', '/vmlinuz'),
        ('build/initramfs.cpio.gz', '/INITRAMFS.GZ;1', '/initramfs.gz'),
        (SYSL + '/bios/com32/elflink/ldlinux/ldlinux.c32', '/LDLINUX.C32;1', '/ldlinux.c32'),
        (SYSL + '/bios/core/isolinux.bin', '/ISOLINUX.BIN;1', '/isolinux.bin'),
        ('build/iso/isolinux.cfg', '/ISOLINUX.CFG;1', '/isolinux.cfg')]:
    iso.add_file(src, iso_path=iso_path, joliet_path=joliet)
iso.add_eltorito('/ISOLINUX.BIN;1', boot_load_size=4, media_name='noemul',
                 boot_info_table=True)
os.makedirs('build/release', exist_ok=True)
if os.path.exists(OUT):
    os.remove(OUT)
iso.write(OUT)
iso.close()
print('iso: %s  %d bytes' % (OUT, os.path.getsize(OUT)))
