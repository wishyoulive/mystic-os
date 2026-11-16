#!/usr/bin/env bash
# ============================================================
#  MYSTIC OS - build script
#  Assembles the boot sector and packs it into a bootable disk image.
#  Read it top to bottom: there are only 5 real steps.
# ============================================================

set -e                      # stop on the first error, don't limp on
cd "$(dirname "$0")"        # always run from the project root

BUILD=build
IMAGE="$BUILD/mystic.img"

# ---- 1. find the assembler ---------------------------------------------
NASM="${NASM:-}"
if [ -z "$NASM" ]; then
    if command -v nasm >/dev/null 2>&1; then
        NASM=nasm
    elif [ -x /c/Users/saket/dev/tools/nasm-3.02/nasm.exe ]; then
        NASM=/c/Users/saket/dev/tools/nasm-3.02/nasm.exe
    else
        echo "error: nasm not found.  install it, or set NASM=/path/to/nasm" >&2
        exit 1
    fi
fi

echo "==> assembler: $NASM"

# ---- 2. assemble the boot sector ---------------------------------------
mkdir -p "$BUILD"
echo "==> assembling boot/boot.asm"
"$NASM" -f bin boot/boot.asm -o "$BUILD/boot.bin"

SIZE=$(wc -c < "$BUILD/boot.bin")
if [ "$SIZE" -ne 512 ]; then
    echo "error: boot sector is $SIZE bytes, the BIOS requires exactly 512" >&2
    exit 1
fi
echo "    ok: 512 bytes"

# ---- 3. the disk signature must be AA55 --------------------------------
SIG=$(od -An -tx1 -j510 -N2 "$BUILD/boot.bin" | tr -d ' \n')
if [ "$SIG" != "55aa" ]; then
    echo "error: missing 0xAA55 boot signature (got $SIG)" >&2
    exit 1
fi
echo "    ok: boot signature 0xAA55"

# ---- 4. build a 1.44 MB blank disk and drop the sector in --------------
echo "==> creating disk image"
dd if=/dev/zero of="$IMAGE" bs=512 count=2880 2>/dev/null
dd if="$BUILD/boot.bin" of="$IMAGE" bs=512 count=1 conv=notrunc 2>/dev/null

# ---- 5. read it back and prove it landed ------------------------------
BACK=$(od -An -tx1 -j510 -N2 "$IMAGE" | tr -d ' \n')
[ "$BACK" = "55aa" ] || { echo "error: image is corrupt" >&2; exit 1; }

echo
echo "built: $IMAGE  ($(wc -c < "$IMAGE") bytes)"
echo "the PC will boot this. next step is a kernel to jump to."
