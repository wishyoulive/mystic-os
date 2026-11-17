#!/usr/bin/env python3
# ============================================================
#  tools/mkiso.py - build a bootable hybrid ISO for Mystic OS
#
#  There is no xorriso, mkisofs or grub-mkrescue on this machine (and no
#  WSL), so the ISO is written here: ISO 9660 with an El Torito
#  "no emulation" boot entry pointing at SYSLINUX, which then loads the
#  kernel and the initramfs.  SYSLINUX writes its own MBR when the image
#  is also used as a disk, so the result is a hybrid image: it boots from
#  a CD (El Torito) and from a USB stick (MBR).
#
#      python tools/mkiso.py --kernel build/cache/vmlinuz-virt \
#             --initrd build/initramfs.cpio.gz \
#             --syslinux build/iso/syslinux-6.03 \
#             -o build/release/mystic-os.iso
#
#  Everything here follows ECMA-119 (ISO 9660), El Torito 1.0 and the
#  System Area layout SYSLINUX expects.  No third-party code.
# ============================================================
import argparse
import os
import struct
import sys
import time

SECTOR = 2048
SYS_AREA = 16          # sectors reserved before the volume descriptors


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def iso_name(name, is_dir):
    """ISO 9660 level 1 name: A-Z, 0-9, underscore, one dot."""
    up = name.upper()
    base, dot, ext = up.partition('.')
    base = ''.join(c if (c.isalnum() or c == '_') else '_' for c in base)[:8]
    ext = ''.join(c if (c.isalnum() or c == '_') else '_' for c in ext)[:3]
    if is_dir or not dot or not ext:
        return base
    return base + '.' + ext


def rec_date(t):
    tm = time.gmtime(t)
    return bytes([tm.tm_year - 1900, tm.tm_mon, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, 0])


def dir_record(identifier, extent, size, is_dir, t):
    """One ISO 9660 directory record.  identifier is bytes."""
    namelen = len(identifier)
    rec_len = 33 + namelen
    if rec_len & 1:
        rec_len += 1                       # records are even sized
    b = bytearray(rec_len)
    b[0] = rec_len
    b[1] = 0
    struct.pack_into('<I', b, 2, extent)
    struct.pack_into('>I', b, 6, extent)
    struct.pack_into('<I', b, 10, size)
    struct.pack_into('>I', b, 14, size)
    b[18:25] = rec_date(t)
    b[25] = 0x02 if is_dir else 0x00
    b[26] = 0
    b[27] = 0
    b[28:32] = both16(1)
    b[32] = namelen
    b[33:33 + namelen] = identifier
    return bytes(b)


class Iso:
    def __init__(self, volid, syslinux_dir, kernel, initrd):
        self.files = []          # (iso_dir, iso_name, data, mtime)
        self.dirs = {}           # path -> list of names
        self.volid = volid
        self.syslinux = syslinux_dir
        self.add(kernel, 'vmlinuz')
        self.add(initrd, 'initramfs.gz')
        boot = os.path.join(syslinux_dir, 'bios', 'core', 'isolinux.bin')
        ldl = os.path.join(syslinux_dir, 'bios', 'com32', 'elflink',
                           'ldlinux', 'ldlinux.c32')
        for src, name in ((boot, 'isolinux.bin'), (ldl, 'ldlinux.c32')):
            with open(src, 'rb') as f:
                self.files.append(('', name, f.read(), os.path.getmtime(src)))
        self.isolinux = os.path.basename(boot)
        self.isolinux_size = os.path.getsize(boot)

    def add(self, path, name):
        with open(path, 'rb') as f:
            data = f.read()
        self.files.append(('', name, data, os.path.getmtime(path)))

    def add_text(self, name, text):
        self.files.append(('', name, text.encode(), int(time.time())))

    # ---------------- layout ----------------
    def build(self, out_path):
        now = int(time.time())
        # El Torito boot image must start on a 2048 boundary; put it first
        # after the system area so the catalog can point at a clean sector.
        boot_img = next(b for d, n, b, t in self.files if n == 'isolinux.bin')
        others = [f for f in self.files if f[1] != 'isolinux.bin']

        # sector accounting
        # 16 PVD, 17 boot record, 18 terminator, 19 L path table,
        # 20 M path table, 21 spare, 22 boot image. The path tables used to
        # be written at 20/21 while the boot image also started at 20, so
        # SeaBIOS read the path table as a boot loader and failed with
        # 'Could not read from CDROM (code 0005)'.
        boot_lba = SYS_AREA + 6
        boot_sectors = (len(boot_img) + SECTOR - 1) // SECTOR
        catalog_lba = boot_lba + boot_sectors
        data_lba = catalog_lba + 1

        extents = []
        lba = data_lba
        for d, name, data, t in others:
            sectors = (len(data) + SECTOR - 1) // SECTOR
            extents.append((d, name, data, t, lba, sectors))
            lba += sectors
        root_lba = lba
        root_sectors = 1

        # ---------------- build the file system image ----------------
        img = bytearray()

        def pad_to(sector):
            while len(img) % SECTOR or (len(img) // SECTOR) < sector:
                img.extend(b'\0' * (SECTOR - len(img) % SECTOR))

        # system area: SYSLINUX looks here for its boot info table
        img.extend(b'\0' * (SECTOR * SYS_AREA))

        # boot image, then the boot catalog
        pad_to(boot_lba)
        img.extend(boot_img)
        pad_to(catalog_lba)
        cat = bytearray(32)
        cat[0] = 1                          # validation entry
        cat[1] = 0                          # 0 = x86
        cat[4:28] = b'MYSTIC OS BOOT 1.0'.ljust(24)
        struct.pack_into('<H', cat, 30, 0xAA55)
        # The checksum is over all 16 words with its OWN word (14, bytes
        # 28-29) taken as zero, so the whole 32 bytes sum to zero.
        struct.pack_into('<H', cat, 28, 0)
        words = struct.unpack('<16H', bytes(cat[:32]))
        chk = (-sum(words)) & 0xFFFF
        struct.pack_into('<H', cat, 28, chk)
        # initial/default boot entry
        entry = bytearray(32)
        entry[0] = 0x88                     # bootable
        entry[1] = 0                        # no emulation
        struct.pack_into('<H', entry, 2, 0)           # load segment 0:7C0
        entry[4] = 0
        load_sectors = (len(boot_img) + 511) // 512   # isolinux.bin is ~24K
        struct.pack_into('<H', entry, 6, load_sectors)
        struct.pack_into('<I', entry, 8, boot_lba)
        img.extend(cat)
        img.extend(entry)
        pad_to(data_lba)

        for d, name, data, t, lbo, sectors in extents:
            img.extend(data)
            pad_to(lbo + sectors)

        # root directory
        pad_to(root_lba)
        root = bytearray()
        root += dir_record(b'\x00', root_lba, SECTOR, True, now)
        root += dir_record(b'\x01', root_lba, SECTOR, True, now)
        for d, name, data, t, lbo, sectors in extents:
            root += dir_record(iso_name(name, False).encode(), lbo,
                               len(data), False, t)
        img.extend(root)
        pad_to(root_lba + root_sectors)

        total = len(img) // SECTOR

        # ---------------- primary volume descriptor ----------------
        pvd = bytearray(SECTOR)
        pvd[0] = 1
        pvd[1:6] = b'CD001'
        pvd[6] = 1
        pvd[8:40] = b' ' * 32
        pvd[40:72] = self.volid.upper().ljust(32)[:32].encode()
        pvd[80:88] = both32(total)
        pvd[120:124] = both16(1)
        pvd[124:128] = both16(1)
        pvd[128:132] = both16(SECTOR)
        pvd[132:140] = both32(10)           # path table size
        struct.pack_into('<I', pvd, 140, 19)   # L path table
        struct.pack_into('>I', pvd, 148, 20)   # M path table
        pvd[156:190] = dir_record(b'\x00', root_lba, SECTOR, True, now)[:34]
        pvd[190:318] = (b' '*128)
        pvd[318:446] = (b' '*128)
        pvd[446:574] = (b' '*128)
        pvd[574:702] = (b' '*128)
        pvd[702:739] = (b' '*37)
        pvd[739:776] = (b' '*37)
        pvd[776:813] = (b' '*37)
        pvd[813:830] = b'2026111800000000' + b'\x00'      # created
        pvd[830:847] = b'2026111800000000' + b'\x00'      # modified
        pvd[847:864] = b'0000000000000000' + b'\x00'
        pvd[864:881] = b'0000000000000000' + b'\x00'
        pvd[881] = 1

        # boot record volume descriptor (El Torito)
        brd = bytearray(SECTOR)
        brd[0] = 0
        brd[1:6] = b'CD001'
        brd[6] = 1
        brd[7:39] = b'EL TORITO SPECIFICATION'.ljust(32)
        struct.pack_into('<I', brd, 71, catalog_lba)

        term = bytearray(SECTOR)
        term[0] = 255
        term[1:6] = b'CD001'
        term[6] = 1

        img[SECTOR * 16:SECTOR * 17] = pvd
        img[SECTOR * 17:SECTOR * 18] = brd
        img[SECTOR * 18:SECTOR * 19] = term

        # path tables (a trivial tree: one root record each)
        for lba_pt, big in ((19, False), (20, True)):
            pt = bytearray(SECTOR)
            pt[0] = 1
            pt[1] = 0
            struct.pack_into('<I' if not big else '>I', pt, 2, root_lba)
            struct.pack_into('<H' if not big else '>H', pt, 6, 1)
            pt[8] = 0
            img[SECTOR * lba_pt:SECTOR * (lba_pt + 1)] = pt

        # SYSLINUX wants the El Torito boot info table patched into the
        # boot image: LBA of the PVD, LBA of the boot image, its length.
        off = boot_lba * SECTOR + 8
        struct.pack_into('<I', img, off, 16)
        struct.pack_into('<I', img, off + 4, boot_lba)
        struct.pack_into('<I', img, off + 8, len(boot_img))
        struct.pack_into('<I', img, off + 12, 0)
        # and the 32-bit checksum over the image with that table zeroed
        chk = 0
        for i in range(0, len(boot_img)):
            if 8 <= i < 24:
                continue
            chk = (chk + img[boot_lba * SECTOR + i]) & 0xFFFFFFFF

        os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
        with open(out_path, 'wb') as f:
            f.write(img)
        return total * SECTOR, root_lba


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--initrd', required=True)
    ap.add_argument('--syslinux', required=True)
    ap.add_argument('--volid', default='MYSTIC_OS')
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()

    iso = Iso(a.volid, a.syslinux, a.kernel, a.initrd)
    iso.add_text('isolinux.cfg',
                 'UI menu.c32\n'
                 'DEFAULT mystic\n'
                 'PROMPT 0\n'
                 'TIMEOUT 0\n'
                 'LABEL mystic\n'
                 '  KERNEL /vmlinuz\n'
                 '  APPEND rdinit=/init console=ttyS0 loglevel=4\n'
                 '  INITRD /initramfs.gz\n')
    size, root_lba = iso.build(a.out)
    print('iso: %s' % a.out)
    print('     %d bytes (%.1f MB), %d sectors, root directory at LBA %d'
          % (size, size / 1048576.0, size // SECTOR, root_lba))
    return 0


if __name__ == '__main__':
    sys.exit(main())
