#!/usr/bin/env bash
# ============================================================
#  tools/release.sh - package the built image into the shippable zip
#
#    ./tools/release.sh
#    MYSTIC_BUILD=/tmp/fake-build ./tools/release.sh      # test tree
#
#  This script builds and downloads NOTHING. It packages what is already
#  on disk, and refuses loudly if a required input is missing, empty, or
#  not the kind of file it claims to be:
#
#      build/cache/vmlinuz-virt   a Linux bzImage ("HdrS" at offset 0x202)
#      build/initramfs.cpio.gz    a gzip stream ("1f 8b")
#
#  Output:
#      build/release/mystic-os-v1.zip
#          vmlinuz-virt  initramfs.cpio.gz  run.sh  README.txt
#      build/release/stage/       the same four files, uncompressed
#
#  There is no `zip` binary on this machine, so the archive is written by
#  Python's standard library: `python -m zipfile -c`. The script also
#  tests the archive, checks that its four entries are at the top level,
#  checks that run.sh is executable inside the archive, and prints the
#  final byte size and SHA-256.
#
#  If src/ or stage/ changed since the last image build, rebuild first
#  (./image.sh). This script packages the existing image on purpose: a
#  release script that silently compiles is a release script that can
#  ship a half-built image.
# ============================================================
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="${MYSTIC_BUILD:-$ROOT/build}"
KERNEL="$BUILD/cache/vmlinuz-virt"
INITRD="$BUILD/initramfs.cpio.gz"
OUT="$BUILD/release"
STAGE="$OUT/stage"
NAME="mystic-os-v1.zip"
ZIP="$OUT/$NAME"
ZIP_TMP="$OUT/$NAME.tmp"

die() { echo "release: $*" >&2; exit 1; }
bytes_of() { wc -c < "$1" | tr -d '[:space:]'; }
mb_of() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }

# ---- 1. inputs: refuse before writing anything -----------------------
missing=0
for f in "$KERNEL" "$INITRD"; do
    if [ ! -e "$f" ]; then
        echo "release: missing input: $f" >&2
        missing=1
    elif [ ! -f "$f" ]; then
        echo "release: input is not a regular file: $f" >&2
        missing=1
    elif [ ! -s "$f" ]; then
        echo "release: input is empty: $f" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo >&2
    echo "Nothing was written. Build the image first:" >&2
    echo "    ./image.sh" >&2
    echo "then run ./tools/release.sh again." >&2
    exit 1
fi

# A non-empty file with the wrong contents is worse than a missing one:
# it would be zipped and shipped. Check the x86 boot header and gzip magic.
kernel_magic=$(od -An -tx1 -j514 -N4 "$KERNEL" 2>/dev/null | tr -d '[:space:]')
[ "$kernel_magic" = "48647253" ] \
    || die "$KERNEL is not a Linux bzImage (no HdrS magic at byte 514)"

initrd_magic=$(od -An -tx1 -N2 "$INITRD" 2>/dev/null | tr -d '[:space:]')
[ "$initrd_magic" = "1f8b" ] \
    || die "$INITRD is not gzip (first bytes: $initrd_magic)"

# ---- 2. python: the zipper, since this machine has no zip -----------
PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
    for cand in python python3 py; do
        if command -v "$cand" >/dev/null 2>&1; then
            PYTHON="$cand"
            break
        fi
    done
fi
[ -n "$PYTHON" ] \
    || die "python not found (the zip is written with 'python -m zipfile' because this machine has no zip binary)"
"$PYTHON" -c 'import zipfile' >/dev/null 2>&1 \
    || die "$PYTHON cannot create zip files (zipfile module missing)"

kernel_bytes=$(bytes_of "$KERNEL")
initrd_bytes=$(bytes_of "$INITRD")

# The inputs are intentionally used as they are. Say so when the tree moved
# on since the image was packed, so a stale image is a loud choice rather
# than an accident. Only meaningful for the real build tree (not for a
# MYSTIC_BUILD test tree, whose sources are elsewhere).
if [ -z "${MYSTIC_BUILD:-}" ]; then
    stale=$(find "$ROOT/src" "$ROOT/stage" -type f -newer "$INITRD" 2>/dev/null | wc -l | tr -d '[:space:]')
    if [ "$stale" -gt 0 ]; then
        echo "release: note: $stale file(s) under src/ or stage/ are newer than" >&2
        echo "         $INITRD" >&2
        echo "         run ./image.sh first if this release should include them." >&2
    fi
fi

# ---- 3. stage the bundle --------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"
rm -f "$ZIP_TMP"
trap 'rm -f "$ZIP_TMP" "$ZIP_TMP.modes"' EXIT

cp "$KERNEL" "$STAGE/vmlinuz-virt"
cp "$INITRD" "$STAGE/initramfs.cpio.gz"
[ "$(bytes_of "$STAGE/vmlinuz-virt")" = "$kernel_bytes" ] \
    || die "copy of the kernel is short - is the disk full?"
[ "$(bytes_of "$STAGE/initramfs.cpio.gz")" = "$initrd_bytes" ] \
    || die "copy of the initramfs is short - is the disk full?"

# ---- 4. the launcher that ships with the image ----------------------
cat > "$STAGE/run.sh" <<'RUN_SH'
#!/usr/bin/env bash
# ============================================================
#  Mystic OS v1 - boot this bundle in QEMU
#
#    ./run.sh              a window
#    ./run.sh headless     serial console only (for verifying a boot)
#
#  Files used from this directory:
#    vmlinuz-virt        the kernel
#    initramfs.cpio.gz   our userspace
#    mystic-disk.img     created blank (2 GB) on the first run; the OS
#                        formats it ext4 and keeps /root there
#
#  QEMU must be on PATH, or set QEMU=/path/to/qemu-system-x86_64
# ============================================================
set -eu
cd "$(dirname "$0")"

KERNEL=vmlinuz-virt
INITRD=initramfs.cpio.gz
DISK=mystic-disk.img
DISK_MB="${MYSTIC_DISK_MB:-2048}"      # 2 GB; override only for testing
MODE="${1:-window}"

for f in "$KERNEL" "$INITRD"; do
    [ -s "$f" ] || {
        echo "error: missing or empty $f - extract the whole zip" >&2
        exit 1
    }
done

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        QEMU=qemu-system-x86_64
    elif [ -x "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then
        QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
    elif [ -x "/Applications/QEMU.app/Contents/MacOS/qemu-system-x86_64" ]; then
        QEMU="/Applications/QEMU.app/Contents/MacOS/qemu-system-x86_64"
    else
        echo "error: qemu-system-x86_64 not found" >&2
        echo "       install QEMU or set QEMU=/path/to/qemu-system-x86_64" >&2
        exit 1
    fi
fi

case "$MODE" in
    window|headless) ;;
    *) echo "usage: ./run.sh [window|headless]" >&2; exit 1 ;;
esac

if [ ! -s "$DISK" ]; then
    echo "==> creating a blank ${DISK_MB} MB disk image: $DISK"
    rm -f "$DISK"
    if ! dd if=/dev/zero of="$DISK" bs=1048576 count="$DISK_MB" 2>/dev/null; then
        rm -f "$DISK"
        echo "error: could not create $DISK (out of disk space?)" >&2
        exit 1
    fi
    want=$((DISK_MB * 1048576))
    got=$(wc -c < "$DISK" | tr -d '[:space:]')
    if [ "$got" != "$want" ]; then
        rm -f "$DISK"
        echo "error: $DISK is $got bytes, expected $want" >&2
        exit 1
    fi
fi

BOOT=( -kernel "$KERNEL" -initrd "$INITRD" -m 1024 -vga std -no-reboot
       -drive "file=$DISK,format=raw,if=virtio"
       -netdev user,id=n0 -device virtio-net-pci,netdev=n0 )

# WHPX is the Windows Hypervisor Platform accelerator. If this QEMU has no
# WHPX (a Linux or macOS host, or an old build) boot without it: slower,
# but the same OS. WHPX needs an explicit CPU model here - the default one
# asks for features the host does not expose and the guest never reaches
# userspace (the same combination tools/smoke.sh uses).
if "$QEMU" -accel help 2>/dev/null | grep -qi whpx; then
    BOOT+=( -accel whpx -cpu qemu64 )
else
    echo "note: WHPX is not available here; booting without hardware acceleration" >&2
fi

if [ "$MODE" = headless ]; then
    exec "$QEMU" "${BOOT[@]}" -display none -monitor none -serial stdio \
         -append "console=ttyS0 rdinit=/init loglevel=6"
fi
exec "$QEMU" "${BOOT[@]}" \
     -append "console=ttyS0 rdinit=/init loglevel=3 quiet"
RUN_SH
chmod +x "$STAGE/run.sh"

# ---- 5. the README that ships with the image ------------------------
cat > "$STAGE/README.txt" <<EOF
MYSTIC OS v1
============

A small desktop operating system for ordinary PCs. Linux is the kernel;
the desktop, the apps, the browser and the package manager are ours.

WHAT IS IN THIS ARCHIVE
  vmlinuz-virt        Linux 6.18, Alpine virt build, unmodified ($(mb_of "$kernel_bytes") MB)
  initramfs.cpio.gz   our userspace: /init, the desktop, its app bundles,
                      kernel modules, fonts and themes ($(mb_of "$initrd_bytes") MB)
  run.sh              boots those two files in QEMU
  README.txt          this file

HOW TO RUN
  Extract the whole zip into a directory, then from that directory:

      ./run.sh              open a window
      ./run.sh headless     serial console only (used to verify a boot)

  On Windows run this from Git Bash, not by double-clicking run.sh.
  QEMU must be installed and on PATH. If it is not:
      QEMU=/path/to/qemu-system-x86_64 ./run.sh

  The VM gets 1 GB of RAM, user-mode networking, and a blank 2 GB virtio
  disk called mystic-disk.img that run.sh creates on the first run. The
  OS formats it ext4 on first boot and keeps /root and installed apps
  there, so they survive a reboot. Delete that file to start over.

  Window mode passes quiet to the kernel; headless uses loglevel=6 so the
  boot lines below reach the serial console. Both start /init as PID 1.

WHAT WORKS
  - Our own userspace as PID 1 on Linux 6.18: /init, the package manager
    (pkg) and the desktop. This is not a remix of another distribution.
  - Our compositor and window manager: six themes, four wallpapers, a
    dock, a launcher, a lock screen and a 1280x800 desktop, all drawn by
    the CPU with no X11, Wayland or GPU driver.
  - Our terminal, file manager, settings, monitor, notes, calculator and
    Apps store, plus the external app bundles.
  - Our HTTP client and HTML-to-text browser: real pages over plain HTTP,
    rendered by our own tokeniser and layout code.
  - Sandboxed apps: every external app is its own process, uid 1000, in
    private mount, IPC, UTS and network namespaces inside a chroot. It
    reaches the window server only through its socket. An app that asks
    for "network = host" in its manifest (the browser) keeps the real
    network namespace; the games do not.
  - The package store: sha256-checked app bundles installed from the
    Mystic repository at runtime, so they do not bloat this image.
  - A virtio disk that persists /root and installed apps.

WHAT DOES NOT WORK YET - HONESTLY
  - No disk encryption. The ext4 data disk and the initramfs are not
    encrypted; anyone holding the image or the disk can read them. There
    is no LUKS in this build.
  - No Wine. Windows programs do not run. There is no compatibility
    layer in the image, only Linux (musl, static) binaries.
  - No DRM video, and no video playback at all. There is no media player,
    no codec stack and no content-protection DRM (Widevine or otherwise).
    The kernel's drm modules are display drivers, not video decoding.
  - The browser is plain HTTP only. No JavaScript, no CSS, no images,
    no HTTPS, no video.
  - Packages are checksum-verified (sha256) but not signed yet.
  - This is a QEMU bundle, not an ISO. There is no bare-metal installer.
  - The desktop is drawn by the CPU. There is no GPU acceleration, so
    animations and full-screen scrolling cost CPU.

CONTROLS
  Super                     open/close the launcher (the eye does too)
  F1..F5                    terminal, files, settings, monitor, notes
  Ctrl+1..6                 switch theme
  Ctrl+Alt+L                lock the screen (v1 password: mystic)
  Ctrl+Alt+G                QEMU: release the mouse grab back to the host

HOW TO CHECK IT BOOTED
  Run ./run.sh headless and read the serial output. A healthy boot
  contains these lines, in this order:

      Linux version 6.18...
      net: eth0 up, 10.0.2.15/24
      disk: mounted /dev/vda (ext4) on /root
      starting mystic-desktop
      display: 1280x800 32bpp stride=1280
      wms: listening on /run/mystic/wm.sock

  The exact kernel version can differ. On the very first boot the disk
  line is:
      disk: formatted ext4 and mounted on /root
  If "starting mystic-desktop" and "display: 1280x800" both appear, our
  desktop started. Stop QEMU with Ctrl+C (headless) or close the window.

      ./run.sh headless | tee serial.log
      grep -q "starting mystic-desktop" serial.log && echo booted

Source: https://github.com/wishyoulive/mystic-os
Built: $(date -u '+%Y-%m-%d %H:%M UTC') from the kernel and initramfs image
in that source tree.
EOF

# ---- 6. zip it, test it, and prove the shape of the archive ---------
echo "==> staging image"
printf '    vmlinuz-virt      %10s bytes  (%s MB)\n' "$kernel_bytes" "$(mb_of "$kernel_bytes")"
printf '    initramfs.cpio.gz %10s bytes  (%s MB)\n' "$initrd_bytes" "$(mb_of "$initrd_bytes")"
echo "==> writing $ZIP with python -m zipfile"

( cd "$STAGE" && "$PYTHON" -m zipfile -c "../$NAME.tmp" \
    vmlinuz-virt initramfs.cpio.gz run.sh README.txt )
# Windows has no POSIX mode bits: python -m zipfile stores 0666 for every
# entry, so run.sh would not be executable after extraction on Linux or a
# Mac. Mark all four entries explicitly (Unix create_system + real modes).
( cd "$STAGE" && "$PYTHON" - "../$NAME.tmp" <<'PY'
import os, sys, zipfile

path = sys.argv[1]
tmp = path + ".modes"
modes = {
    "vmlinuz-virt": 0o100644,
    "initramfs.cpio.gz": 0o100644,
    "run.sh": 0o100755,
    "README.txt": 0o100644,
}
with zipfile.ZipFile(path, "r") as src:
    entries = [(info, src.read(info.filename)) for info in src.infolist()]
with zipfile.ZipFile(tmp, "w") as dst:
    for info, data in entries:
        info.create_system = 3                      # Unix mode bits apply
        info.external_attr = modes[info.filename] << 16
        dst.writestr(info, data)
os.replace(tmp, path)
PY
)

# python -m zipfile -t only reports raised decompression errors; a member
# with a bad CRC can still exit 0. Ask testzip() directly for the first bad
# member name, and treat a decompression exception as a failure too.
bad_member=$( cd "$STAGE" && "$PYTHON" - "../$NAME.tmp" <<'PY'
import sys, zipfile
try:
    bad = zipfile.ZipFile(sys.argv[1]).testzip()
except Exception as exc:
    sys.stdout.buffer.write(("error: %s" % exc).encode() + b"\n")
else:
    sys.stdout.buffer.write((bad or "").encode() + b"\n")
PY
) || die "could not read the archive just written"
[ -z "$bad_member" ] \
    || die "archive failed its CRC/decompression test: $bad_member"

names=$( cd "$STAGE" && "$PYTHON" -c \
    'import sys, zipfile; print("\n".join(sorted(zipfile.ZipFile(sys.argv[1]).namelist())))' \
    "../$NAME.tmp" | tr -d '\r' )
expected=$(printf 'README.txt\ninitramfs.cpio.gz\nrun.sh\nvmlinuz-virt\n')
[ "$names" = "$expected" ] \
    || die "unexpected archive layout: $(printf '%s' "$names" | tr '\n' ' ')"

run_mode=$( cd "$STAGE" && "$PYTHON" -c \
    'import sys, zipfile; print(oct(zipfile.ZipFile(sys.argv[1]).getinfo("run.sh").external_attr >> 16))' \
    "../$NAME.tmp" | tr -d '\r' )
case "$run_mode" in
    *755) ;;
    *) die "run.sh is not executable inside the archive (mode $run_mode)" ;;
esac

mv "$ZIP_TMP" "$ZIP"
trap - EXIT

zip_bytes=$(bytes_of "$ZIP")
sha=$( cd "$OUT" && "$PYTHON" - "$NAME" <<'PY'
import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
print(h.hexdigest())
PY
)
sha=$(printf '%s' "$sha" | tr -d '\r')

echo
echo "archive contents"
( cd "$STAGE" && "$PYTHON" -m zipfile -l "../$NAME" )
echo
echo "release: wrote $ZIP"
printf '         %s bytes  (%s MB)\n' "$zip_bytes" "$(mb_of "$zip_bytes")"
printf '         sha256 %s\n' "$sha"
