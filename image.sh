#!/usr/bin/env bash
# ============================================================
#  MYSTIC OS - image builder
#  Downloads a kernel, packs our rootfs, boots it. Three steps.
# ============================================================
set -e
cd "$(dirname "$0")"

BUILD=build
CACHE=$BUILD/cache
KERNEL=$CACHE/vmlinuz-virt
ROOTFS=$CACHE/minirootfs.tar.gz
CPIO=$BUILD/initramfs.cpio.gz
IMAGE=$BUILD/mystic.img

ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64
KERNEL_URL=$ALPINE/netboot/vmlinuz-virt
ROOTFS_URL=$ALPINE/alpine-minirootfs-3.24.4-x86_64.tar.gz

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then QEMU=qemu-system-x86_64
    elif [ -x "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
    else echo "error: qemu not found" >&2; exit 1; fi
fi

mkdir -p "$CACHE"

# 1. kernel (13 MB, cached)
if [ ! -f "$KERNEL" ]; then
    echo "==> downloading kernel"
    curl -sL -o "$KERNEL.part" "$KERNEL_URL"
    mv "$KERNEL.part" "$KERNEL"
fi
echo "==> kernel: $(du -h "$KERNEL" | cut -f1)  $(file -b "$KERNEL" 2>/dev/null | cut -c1-40)"

# 2. base rootfs (3.7 MB, cached)
if [ ! -f "$ROOTFS" ]; then
    echo "==> downloading base rootfs"
    curl -sL -o "$ROOTFS.part" "$ROOTFS_URL"
    mv "$ROOTFS.part" "$ROOTFS"
fi

# 3. our rootfs = base + stage/ overlay -> cpio.gz
echo "==> packing rootfs (base + stage/)"
PYTHONIOENCODING=utf-8 python tools/mkimage.py --base "$ROOTFS" --stage stage -o "$CPIO"

# 4. boot it
case "${1:-run}" in
    run)
        echo "==> booting in QEMU (close the window to stop)"
        exec "$QEMU" -kernel "$KERNEL" -initrd "$CPIO" \
             -append "console=tty0 rdinit=/init loglevel=3 quiet" \
             -m 256 -vga std
        ;;
    shell)
        echo "==> booting with serial console attached (ctrl-a x to quit)"
        exec "$QEMU" -kernel "$KERNEL" -initrd "$CPIO" \
             -append "console=ttyS0 rdinit=/init loglevel=4" \
             -m 256 -display none -serial stdio -no-reboot
        ;;
    shot)
        # boot headless, grab the screen after $2 seconds, quit
        WAIT="${2:-10}"
        rm -f "$BUILD/shot.ppm"
        { sleep "$WAIT"; echo "screendump $(pwd -W 2>/dev/null || pwd)/$BUILD/shot.ppm"; sleep 3; echo quit; } \
          | "$QEMU" -kernel "$KERNEL" -initrd "$CPIO" \
                -append "console=tty0 rdinit=/init loglevel=3 quiet" \
                -m 256 -display none -monitor stdio -serial null -no-reboot >/dev/null 2>&1 || true
        PYTHONIOENCODING=utf-8 python tools/ppm2png.py "$BUILD/shot.ppm" "$BUILD/shot.png"
        ;;
    *)
        echo "usage: ./image.sh [run|shell|shot]" >&2
        exit 1
        ;;
esac
