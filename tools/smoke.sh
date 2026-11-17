#!/usr/bin/env bash
# ============================================================
#  tools/smoke.sh - does the OS actually run?
#
#  Not a unit test: this boots the real image and checks the
#  real machine. Every check is something that failed at least
#  once during development, so the list is earned, not invented.
#
#    ./tools/smoke.sh
# ============================================================
set -u
cd "$(dirname "$0")/.."

QEMU="${QEMU:-/c/Program Files/qemu/qemu-system-x86_64.exe}"
[ -x "$QEMU" ] || { echo "FAIL: qemu not found"; exit 1; }
[ -f build/initramfs.cpio.gz ] || { echo "FAIL: no image; run ./image.sh first"; exit 1; }

BOOT=( -kernel build/cache/vmlinuz-virt -initrd build/initramfs.cpio.gz
       -m 256 -vga std -no-reboot -display none -serial stdio )
"$QEMU" -accel help 2>/dev/null | grep -qi whpx && BOOT+=( -accel whpx )

pass=0; fail=0
check() {                       # check <name> <condition-result>
    if [ "$2" = "1" ]; then printf '  ok    %s\n' "$1"; pass=$((pass+1));
    else printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); fi
}

echo "== booting (probe mode) =="
out=$(timeout 120 "$QEMU" "${BOOT[@]}" \
        -append "console=ttyS0 rdinit=/init mystic.probe=1 loglevel=0" 2>&1 | tr -d '\r')
rc=$?

echo "$out" | grep -q -- "--- mystic probe ---" && check "kernel reached userspace" 1 || check "kernel reached userspace" 0
echo "$out" | grep -q "kernel   : 6\."            && check "kernel version reported" 1 || check "kernel version reported" 0
echo "$out" | grep -q "card0: yes"                && check "DRM card0 present" 1 || check "DRM card0 present" 0
echo "$out" | grep -q "fb0  : yes"                && check "framebuffer present" 1 || check "framebuffer present" 0
echo "$out" | grep -q "event0"                    && check "input event device" 1 || check "input event device" 0
echo "$out" | grep -qi "segfault\|panic\|not syncing" && check "no kernel/userspace crash" 0 || check "no kernel/userspace crash" 1

echo
echo "== booting (desktop mode) =="
dout=$(timeout 60 "$QEMU" "${BOOT[@]}" \
        -append "console=ttyS0 rdinit=/init loglevel=0" 2>&1 | tr -d '\r' &
       sleep 20; pkill -f qemu-system-x86_64 2>/dev/null; wait 2>/dev/null)
echo "$dout" | grep -q "display: .*x.*32bpp"    && check "desktop opened the display" 1 || check "desktop opened the display" 0
echo "$dout" | grep -q "input: keyboard"        && check "desktop bound a keyboard" 1 || check "desktop bound a keyboard" 0
echo "$dout" | grep -qi "segfault"              && check "desktop did not crash" 0 || check "desktop did not crash" 1

echo
echo "== compile checks =="
ZIG=$(command -v zig || ls -d /c/Users/saket/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-x86_64-windows-*/zig.exe 2>/dev/null | head -1)
if [ -n "$ZIG" ]; then
    for f in src/*.c src/apps/*.c; do
        if "$ZIG" cc -target x86_64-linux-musl -Os -static -Wall -Wextra -Wno-unused-parameter \
             -c "$f" -Isrc -o /tmp/smoke.o 2>/tmp/smoke.err; then
            check "compiles clean: $f" 1
        else
            check "compiles clean: $f" 0
            head -3 /tmp/smoke.err | sed 's/^/        /'
        fi
    done
else
    echo "  skip  compile checks (zig not found)"
fi

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ] || exit 1
