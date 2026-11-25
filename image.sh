#!/usr/bin/env bash
# ============================================================
#  MYSTIC OS - image builder and runner
#
#  ./image.sh              boot the desktop in a window      (linux-virt)
#  ./image.sh --hw         the same, on the hardware kernel  (linux-lts)
#  ./image.sh probe        print machine facts, then power off
#  ./image.sh shot 10      boot headless, screenshot after 10s
#  ./image.sh themes       screenshot all three themes in one boot
#
#  --hw (or MYSTIC_KERNEL=lts) swaps linux-virt for linux-lts so that
#  drivers/media (uvcvideo), sound/usb (snd-usb-audio), USB and Wi-Fi
#  exist at all. The default image cannot drive a webcam or a USB
#  microphone: those modules are not in linux-virt, full stop.
#  The module payload grows from 31 MB to 290 MB unpacked, so this mode
#  says so loudly and raises the default RAM to 1 GB.
# ============================================================
set -e
cd "$(dirname "$0")"

BUILD=build
CACHE=$BUILD/cache
ROOTFS=$CACHE/minirootfs.tar.gz
CPIO=$BUILD/initramfs.cpio.gz

ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64

# ---- which kernel? -------------------------------------------------
HW=0
ARGS=()
for a in "$@"; do
    case "$a" in
        --hw|--lts|hw) HW=1 ;;
        *) ARGS+=("$a") ;;
    esac
done
if [ "${MYSTIC_KERNEL:-}" = "lts" ] || [ "${MYSTIC_KERNEL:-}" = "--hw" ]; then HW=1; fi
set -- "${ARGS[@]+"${ARGS[@]}"}"

if [ "$HW" = "1" ]; then
    VARIANT=lts
    KERNEL=$CACHE/vmlinuz-lts
else
    VARIANT=virt
    KERNEL=$CACHE/vmlinuz-virt
fi

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

# ---- kernel + modules ----------------------------------------------
HAVE_PKG=0
for d in "$CACHE/linux-$VARIANT-"*; do [ -d "$d" ] && HAVE_PKG=1; done
if [ ! -f "$KERNEL" ] || [ "$HAVE_PKG" = 0 ]; then
    ./tools/fetch-kernel.sh "$VARIANT"
fi

# The staging root keeps one module tree per kernel variant. The boot kernel
# is uname -r, so the image must carry the tree that matches $KERNEL and no
# other; mkimage.py is told which one below.
KVER=$(ls stage/lib/modules 2>/dev/null | grep -- "-$VARIANT\$" | head -1)
if [ -z "$KVER" ]; then
    echo "error: no staged modules for the $VARIANT kernel;" >&2
    echo "       run ./tools/fetch-kernel.sh $VARIANT" >&2
    exit 1
fi

if [ ! -f "$ROOTFS" ]; then
    echo "==> downloading base rootfs"
    curl -sL -o "$ROOTFS.part" "$ALPINE/alpine-minirootfs-3.24.4-x86_64.tar.gz"
    mv "$ROOTFS.part" "$ROOTFS"
fi

echo "==> compiling desktop"
./tools/build-desktop.sh >/dev/null

# gzip -9 over the 320 MB lts payload is slow and buys very little on data
# that is already compressed kernelspace; measured on this tree: virt at -9
# packs 48 MB in 25 s, lts at -6 packs 320 MB in 27 s. The hardware image is
# already 5.3x the default one, so it packs at -6.
GZLEVEL=9
if [ "$VARIANT" = "lts" ]; then GZLEVEL=6; fi

echo "==> packing rootfs ($VARIANT, kernel $KVER)"
PYTHONIOENCODING=utf-8 python tools/mkimage.py --base "$ROOTFS" --stage stage \
    --modules-kver "$KVER" --gzip-level "$GZLEVEL" -o "$CPIO" | tail -4

CPIO_MB=$(awk -v b="$(wc -c < "$CPIO")" 'BEGIN{printf "%.1f", b/1e6}')
MOD_N=$(find "stage/lib/modules/$KVER" -name '*.ko' 2>/dev/null | wc -l)
MOD_MB=$(find "stage/lib/modules/$KVER" -name '*.ko' -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{printf "%.1f", s/1e6}')
KMOD_N=$(find "stage/lib/modules/$KVER/kernel/drivers/media" -name '*.ko' 2>/dev/null | wc -l)

if [ "$VARIANT" = "lts" ]; then
    cat <<WARN

  ================================================================
   HARDWARE KERNEL (linux-lts) - THIS IMAGE IS BIG
     initramfs      : ${CPIO_MB} MB compressed (the default image is ~16 MB)
     kernel modules : ${MOD_N} files, ${MOD_MB} MB unpacked
     V4L2 drivers   : ${KMOD_N} files under kernel/drivers/media
     RAM needed     : at least 1 GB (MYSTIC_RAM), the initramfs is unpacked
     Boot           : slower than the default image, by design.
   What this buys: uvcvideo (webcam), snd-usb-audio (USB microphone),
   USB host + HID, the Wi-Fi stacks. The default linux-virt image has
   NONE of those - see docs/HARDWARE.md.
  ================================================================

WARN
    if [ "${MYSTIC_RAM:-0}" != "0" ] && [ "${MYSTIC_RAM}" -lt 768 ] 2>/dev/null; then
        echo "  warning: MYSTIC_RAM=${MYSTIC_RAM} MB is very likely too small for this initramfs" >&2
    fi
else
    echo "==> initramfs: ${CPIO_MB} MB, ${MOD_N} modules (${MOD_MB} MB) - no webcam, no USB audio on this kernel"
fi

DISK=$BUILD/disk.img
if [ ! -f "$DISK" ]; then
    echo "==> creating ${MYSTIC_DISK_MB:-64} MB disk image (the OS formats it on first boot)"
    dd if=/dev/zero of="$DISK" bs=1048576 count="${MYSTIC_DISK_MB:-64}" 2>/dev/null
fi

RAM=${MYSTIC_RAM:-256}
if [ "$VARIANT" = "lts" ] && [ "${MYSTIC_RAM:-0}" = "0" ]; then RAM=1024; fi

# Four cores, not one: software video decode is per-frame work that splits
# across threads, and a single vCPU cannot sustain 1080p50 with libavcodec, let
# alone 4K. The guest sees nproc=4 and VLC/Chromium size their decoder threads
# from it. Override with MYSTIC_SMP=1 for a single-core run.
SMP=${MYSTIC_SMP:-4}
BOOT=( -kernel "$KERNEL" -initrd "$CPIO" -m "$RAM" -smp "$SMP" -vga std -no-reboot
       -drive file="$DISK",format=raw,if=virtio
       -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -audiodev sdl,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 )

# A USB bus only exists if QEMU is told to make one, and the default image
# never had one. The hardware variant gets an xHCI controller so a uvcvideo
# device can appear at all; attach a real camera with
#   MYSTIC_USB_HOST=1234:5678 ./image.sh --hw
# (vendor:product from lsusb on the host - untested here, there is no
#  webcam on this build machine).
if [ "$VARIANT" = "lts" ]; then
    BOOT+=( -device qemu-xhci,id=xhci )
    if [ -n "${MYSTIC_USB_HOST:-}" ]; then
        vid=${MYSTIC_USB_HOST%:*}
        pid=${MYSTIC_USB_HOST#*:}
        echo "==> usb passthrough: host device $vid:$pid on the xHCI bus"
        BOOT+=( -device usb-host,bus=xhci.0,vendorid=0x$vid,productid=0x$pid )
    fi
fi

[ -n "$ACCEL" ] && BOOT+=( $ACCEL )

case "${1:-run}" in
    run)
        echo "==> booting the desktop (close the window to stop), $RAM MB RAM"
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
        echo "usage: ./image.sh [--hw] [run|probe|shot N|themes]" >&2
        exit 1
        ;;
esac
