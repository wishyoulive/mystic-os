#!/usr/bin/env bash
# ============================================================
#  MYSTIC OS - image builder and runner
#
#  ./image.sh            boot the desktop in a window
#  ./image.sh probe      print machine facts, then power off
#  ./image.sh shot 10    boot headless, screenshot after 10s
#  ./image.sh themes     screenshot all three themes in one boot
# ============================================================
set -e
cd "$(dirname "$0")"

BUILD=build
CACHE=$BUILD/cache
KERNEL=$CACHE/vmlinuz-virt
ROOTFS=$CACHE/minirootfs.tar.gz
CPIO=$BUILD/initramfs.cpio.gz

ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then QEMU=qemu-system-x86_64
    elif [ -x "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
    else echo "error: qemu not found" >&2; exit 1; fi
fi

# hardware acceleration if the host offers it, otherwise plain emulation
ACCEL=""
if "$QEMU" -accel help 2>/dev/null | grep -qi whpx; then ACCEL="-accel whpx"; fi

mkdir -p "$CACHE"

if [ ! -f "$KERNEL" ] || [ ! -d "$CACHE"/linux-virt-* ]; then
    ./tools/fetch-kernel.sh
fi
if [ ! -f "$ROOTFS" ]; then
    echo "==> downloading base rootfs"
    curl -sL -o "$ROOTFS.part" "$ALPINE/alpine-minirootfs-3.24.4-x86_64.tar.gz"
    mv "$ROOTFS.part" "$ROOTFS"
fi

echo "==> compiling desktop"
./tools/build-desktop.sh >/dev/null
echo "==> packing rootfs"
PYTHONIOENCODING=utf-8 python tools/mkimage.py --base "$ROOTFS" --stage stage -o "$CPIO" | tail -1

DISK=$BUILD/disk.img
if [ ! -f "$DISK" ]; then
    echo "==> creating ${MYSTIC_DISK_MB:-64} MB disk image (the OS formats it on first boot)"
    dd if=/dev/zero of="$DISK" bs=1048576 count="${MYSTIC_DISK_MB:-64}" 2>/dev/null
fi

BOOT=( -kernel "$KERNEL" -initrd "$CPIO" -m "${MYSTIC_RAM:-256}" -vga std -no-reboot
       -drive file="$DISK",format=raw,if=virtio \n       -netdev user,id=n0 -device virtio-net-pci,netdev=n0 )
[ -n "$ACCEL" ] && BOOT+=( $ACCEL )

case "${1:-run}" in
    run)
        echo "==> booting the desktop (close the window to stop)"
        exec "$QEMU" "${BOOT[@]}" -append "console=ttyS0 rdinit=/init loglevel=3 quiet"
        ;;
    probe)
        timeout 90 "$QEMU" "${BOOT[@]}" -append "console=ttyS0 rdinit=/init mystic.probe=1 loglevel=0" \
            -display none -serial stdio 2>&1 | tr -d '\r' | sed -n '/--- mystic probe ---/,/--- end probe ---/p'
        ;;
    shot)
        WAIT="${2:-10}"
        rm -f "$BUILD/shot.ppm"
        { sleep "$WAIT"; echo "screendump $(pwd -W 2>/dev/null || pwd)/$BUILD/shot.ppm"; sleep 2; echo quit; } \
          | timeout 60 "$QEMU" "${BOOT[@]}" -append "console=ttyS0 rdinit=/init loglevel=3 quiet" \
                -display none -monitor stdio -serial null >/dev/null 2>&1 || true
        PYTHONIOENCODING=utf-8 python tools/ppm2png.py "$BUILD/shot.ppm" "$BUILD/shot.png"
        ;;
    themes)
        rm -f "$BUILD"/theme*.ppm
        { sleep 11
          echo "screendump $(pwd -W 2>/dev/null || pwd)/$BUILD/theme1.ppm"; sleep 2
          echo "sendkey 2"; sleep 3
          echo "screendump $(pwd -W 2>/dev/null || pwd)/$BUILD/theme2.ppm"; sleep 2
          echo "sendkey 3"; sleep 3
          echo "screendump $(pwd -W 2>/dev/null || pwd)/$BUILD/theme3.ppm"; sleep 1
          echo quit
        } | timeout 90 "$QEMU" "${BOOT[@]}" -append "console=ttyS0 rdinit=/init loglevel=3 quiet" \
                -display none -monitor stdio -serial null >/dev/null 2>&1 || true
        for n in 1 2 3; do
            [ -f "$BUILD/theme$n.ppm" ] && PYTHONIOENCODING=utf-8 python tools/ppm2png.py "$BUILD/theme$n.ppm" "$BUILD/theme$n.png"
        done
        ;;
    *)
        echo "usage: ./image.sh [run|probe|shot N|themes]" >&2
        exit 1
        ;;
esac
