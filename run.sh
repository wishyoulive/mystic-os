#!/usr/bin/env bash
# Run the disk image in QEMU (a fake, disposable PC in a window).
set -e
cd "$(dirname "$0")"

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        QEMU=qemu-system-x86_64
    elif [ -x "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then
        QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
    else
        echo "error: qemu not found. set QEMU=/path/to/qemu-system-x86_64" >&2
        exit 1
    fi
fi

[ -f build/mystic.img ] || ./build.sh

echo "==> booting build/mystic.img in QEMU  (close the window to stop)"
exec "$QEMU" -drive format=raw,file=build/mystic.img -m 32
