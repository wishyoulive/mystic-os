#!/usr/bin/env bash
# ============================================================
#  tools/smoke.sh - does the OS actually run?
#
#  Not a unit test: this boots the real image and checks the
#  real machine. Every check is something that failed at least
#  once during development, so the list is earned, not invented.
#
#    ./tools/smoke.sh             full run: two probe boots (the
#                                 second proves the ext4 counter
#                                 moves), one desktop boot, then
#                                 the compile checks
#    ./tools/smoke.sh --quick     compile checks only, no QEMU
#
#  Every check prints PASS or FAIL, and the script exits non-zero
#  if any check failed.
#
#  Environment:
#    MAX_BOOT_SECONDS  boot budget, compared with the probe's own
#                      /proc/uptime reading (default 5)
#    BOOT_TIMEOUT      seconds allowed for one probe boot (default 60)
#    ACCEL_TIMEOUT     seconds allowed for the accelerated first attempt (25)
#    DESKTOP_WAIT      seconds the desktop boot is left running (12)
#    QEMU              path to qemu-system-x86_64
#    MYSTIC_ACCEL      force an accelerator: whpx, kvm, tcg, none
# ============================================================
set -u
cd "$(dirname "$0")/.." || exit 1

QUICK=0
for arg in "$@"; do
    case "$arg" in
        -q|--quick) QUICK=1 ;;
        -h|--help)
            awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print; next } exit }' "$0"
            exit 0 ;;
        *)
            echo "usage: $0 [--quick|--help]" >&2
            exit 2 ;;
    esac
done

KERNEL=build/cache/vmlinuz-virt
INITRD=build/initramfs.cpio.gz
DISK=build/disk.img
MAX_BOOT_SECONDS="${MAX_BOOT_SECONDS:-5}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
DESKTOP_WAIT="${DESKTOP_WAIT:-12}"
ACCEL_TIMEOUT="${ACCEL_TIMEOUT:-25}"

pass=0
fail=0
skip=0

# check <name> <1|0>   - one line, one verdict
check() {
    if [ "$2" = "1" ]; then
        printf '  PASS  %s\n' "$1"; pass=$((pass + 1))
    else
        printf '  FAIL  %s\n' "$1"; fail=$((fail + 1))
    fi
}
skipped() {
    printf '  skip  %s\n' "$1"; skip=$((skip + 1))
}
note() { printf '        %s\n' "$1"; }

is_num() {                       # is_num <string> -> 0 if plain number
    case "$1" in
        ''|*[!0-9.]*|*.*.*) return 1 ;;
        *) return 0 ;;
    esac
}

# ------------------------------------------------------------ QEMU checks
qemu_ok=0
image_ok=0

if [ "$QUICK" -eq 0 ]; then
    echo "== preflight =="

    QEMU="${QEMU:-}"
    if [ -z "$QEMU" ]; then
        if command -v qemu-system-x86_64 >/dev/null 2>&1; then
            QEMU=qemu-system-x86_64
        elif [ -x "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then
            QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
        fi
    fi
    if [ -n "$QEMU" ] && [ -x "$QEMU" ]; then
        check "qemu available" 1
        qemu_ok=1
    else
        check "qemu available" 0
        note "set QEMU=/path/to/qemu-system-x86_64"
    fi

    if [ -f "$KERNEL" ]; then
        check "kernel image present" 1
    else
        check "kernel image present" 0
        note "missing $KERNEL - run ./image.sh once to fetch it"
    fi
    if [ -f "$INITRD" ]; then
        check "initramfs present" 1
    else
        check "initramfs present" 0
        note "missing $INITRD - run ./image.sh once to build it"
    fi
    [ -f "$KERNEL" ] && [ -f "$INITRD" ] && image_ok=1

    # The disk is the whole point of check 1: it must exist as a file the
    # guest formats on first boot and then keeps. 64 MB, same as image.sh.
    if [ ! -f "$DISK" ]; then
        note "creating $DISK (64 MB, the guest formats it on first boot)"
        dd if=/dev/zero of="$DISK" bs=1048576 count=64 2>/dev/null
    fi
    [ -f "$DISK" ] && check "disk image present" 1 || check "disk image present" 0

    # Hardware acceleration: without it the guest clock runs slow enough
    # that the boot budget below is not meaningful. WHPX needs an explicit
    # CPU model - the default one asks for features the host does not have
    # and the guest never reaches userspace.
    ACCEL=()
    ACCEL_NAME=none
    if [ "${MYSTIC_ACCEL:-}" = "none" ]; then
        :
    elif [ -n "${MYSTIC_ACCEL:-}" ]; then
        ACCEL=( -accel "$MYSTIC_ACCEL" )
        ACCEL_NAME="$MYSTIC_ACCEL"
        [ "$MYSTIC_ACCEL" = "whpx" ] && ACCEL+=( -cpu qemu64 )
    elif [ "$qemu_ok" -eq 1 ] && "$QEMU" -accel help 2>/dev/null | grep -qi whpx; then
        ACCEL=( -accel whpx -cpu qemu64 )
        ACCEL_NAME=whpx
    elif [ "$qemu_ok" -eq 1 ] && "$QEMU" -accel help 2>/dev/null | grep -qi kvm; then
        ACCEL=( -accel kvm )
        ACCEL_NAME=kvm
    fi
    echo "        acceleration: $ACCEL_NAME"
fi

# probe_boot <logfile> <timeout> [extra qemu args...]
# Boots the probe init, which prints machine facts and powers itself off.
# Prints the serial log (CRs stripped) on stdout.
probe_boot() {
    local log tmo
    log=$1; tmo=$2; shift 2
    timeout "$tmo" "$QEMU" -kernel "$KERNEL" -initrd "$INITRD" \
        -m 256 -vga std -no-reboot -display none -serial stdio \
        -drive file="$DISK",format=raw,if=virtio \
        "$@" \
        -append "console=ttyS0 rdinit=/init mystic.probe=1 loglevel=0" \
        >"$log" 2>&1
    tr -d '\r' <"$log"
}

counter_of() {                   # counter_of <probe output> -> number
    printf '%s\n' "$1" | sed -n 's/.*boot counter now \([0-9][0-9]*\).*/\1/p' | head -1
}

if [ "$QUICK" -eq 1 ]; then
    echo
    echo "== boot checks =="
    skipped "qemu boot checks (--quick)"
elif [ "$qemu_ok" -eq 1 ] && [ "$image_ok" -eq 1 ]; then
    LOG1=$(mktemp 2>/dev/null || echo /tmp/mystic-smoke-1.$$)
    LOG2=$(mktemp 2>/dev/null || echo /tmp/mystic-smoke-2.$$)
    LOGD=$(mktemp 2>/dev/null || echo /tmp/mystic-smoke-d.$$)
    trap 'rm -f "$LOG1" "$LOG2" "$LOGD"' EXIT

    echo
    echo "== boot 1: probe (machine facts) =="
    tmo="$BOOT_TIMEOUT"
    [ "$ACCEL_NAME" != "none" ] && tmo="$ACCEL_TIMEOUT"
    out=$(probe_boot "$LOG1" "$tmo" ${ACCEL[@]+"${ACCEL[@]}"})

    # An accelerator that QEMU advertises but the host cannot provide hangs
    # the guest silently. One software-emulated boot is the fallback.
    if [ "$ACCEL_NAME" != "none" ] && ! printf '%s' "$out" | grep -q -- '--- mystic probe ---'; then
        note "$ACCEL_NAME produced no probe output, retrying without acceleration"
        ACCEL=()
        ACCEL_NAME=none
        out=$(probe_boot "$LOG1" "$BOOT_TIMEOUT")
    fi

    printf '%s' "$out" | grep -q -- '--- mystic probe ---' \
        && check "kernel reached userspace" 1 || check "kernel reached userspace" 0
    printf '%s' "$out" | grep -q '^kernel[[:space:]]*:[[:space:]]*6\.' \
        && check "kernel version reported" 1 || check "kernel version reported" 0
    printf '%s' "$out" | grep -q 'card0: yes' \
        && check "DRM card0 present" 1 || check "DRM card0 present" 0
    printf '%s' "$out" | grep -q 'fb0  : yes' \
        && check "framebuffer present" 1 || check "framebuffer present" 0

    # ---- kernel modules: /proc/modules only lists what is actually loaded
    modules=$(printf '%s\n' "$out" | sed -n 's/^modules[[:space:]]*:[[:space:]]*//p' | head -1)
    if [ -z "$modules" ]; then
        note "probe printed no modules line"
    fi
    for m in drm bochs virtio_blk; do
        case " $modules " in
            *" $m "*) check "module loaded: $m" 1 ;;
            *)        check "module loaded: $m" 0 ;;
        esac
    done

    # ---- input: the probe lists /dev/input, so a bare "event0" line means
    # the evdev node exists (the unindented lines after it are device names)
    input_section=$(printf '%s\n' "$out" | awk '
        /^input[[:space:]]*:/ { inside = 1; next }
        inside && /^[^[:space:]]/ { inside = 0 }
        inside')
    printf '%s\n' "$input_section" | grep -qx '[[:space:]]*event0[[:space:]]*' \
        && check "input: /dev/input/event0 node exists" 1 \
        || check "input: /dev/input/event0 node exists" 0

    # ---- boot time, as the guest itself measured it
    uptime_s=$(printf '%s\n' "$out" | sed -n 's/^uptime[[:space:]]*:[[:space:]]*\([0-9][0-9.]*\)s.*/\1/p' | head -1)
    if ! is_num "$MAX_BOOT_SECONDS"; then
        check "boot time within budget" 0
        note "MAX_BOOT_SECONDS='$MAX_BOOT_SECONDS' is not a number"
    elif ! is_num "$uptime_s"; then
        check "boot time within ${MAX_BOOT_SECONDS}s budget" 0
        note "probe printed no parsable uptime line"
    else
        awk -v u="$uptime_s" -v m="$MAX_BOOT_SECONDS" 'BEGIN { exit !(u <= m) }' \
            && check "boot time ${uptime_s}s <= ${MAX_BOOT_SECONDS}s budget" 1 \
            || check "boot time ${uptime_s}s <= ${MAX_BOOT_SECONDS}s budget" 0
        [ "$ACCEL_NAME" = "none" ] && note "software emulation: the budget assumes -accel (raise MAX_BOOT_SECONDS to accept a slower boot)"
    fi

    printf '%s' "$out" | grep -qi 'segfault\|panic\|not syncing' \
        && check "no kernel/userspace crash" 0 || check "no kernel/userspace crash" 1

    # ---- disk: the ext4 filesystem must be there, and the counter it keeps
    # in /root/boots.txt must be one higher after a second boot.
    printf '%s' "$out" | grep -Eq 'mounted /dev/vda \(ext4\)|formatted ext4' \
        && check "disk: ext4 mounted on /dev/vda" 1 \
        || check "disk: ext4 mounted on /dev/vda" 0
    c1=$(counter_of "$out")

    echo
    echo "== boot 2: probe (does the disk remember?) =="
    out2=$(probe_boot "$LOG2" "$BOOT_TIMEOUT" ${ACCEL[@]+"${ACCEL[@]}"})
    c2=$(counter_of "$out2")

    if [ -z "$c1" ] || [ -z "$c2" ]; then
        check "disk: boot counter increments across reboots" 0
        note "counter not printed (boot 1: '${c1:-none}', boot 2: '${c2:-none}')"
    elif [ "$c2" -eq $((c1 + 1)) ]; then
        check "disk: boot counter increments across reboots ($c1 -> $c2)" 1
    else
        check "disk: boot counter increments across reboots ($c1 -> $c2)" 0
    fi

    # ---- the desktop, not just the init
    echo
    echo "== boot 3: desktop =="
    "$QEMU" -kernel "$KERNEL" -initrd "$INITRD" \
        -m 256 -vga std -no-reboot -display none -serial stdio \
        -drive file="$DISK",format=raw,if=virtio \
        ${ACCEL[@]+"${ACCEL[@]}"} \
        -append "console=ttyS0 rdinit=/init loglevel=0" >"$LOGD" 2>&1 &
    dpid=$!
    sleep "$DESKTOP_WAIT"
    kill "$dpid" 2>/dev/null            # this boot only, not every qemu on the box
    sleep 1
    kill -9 "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    dout=$(tr -d '\r' <"$LOGD")

    printf '%s' "$dout" | grep -q 'display: .*x.*32bpp' \
        && check "desktop opened the display" 1 || check "desktop opened the display" 0
    printf '%s' "$dout" | grep -q 'input: keyboard' \
        && check "desktop bound a keyboard" 1 || check "desktop bound a keyboard" 0
    printf '%s' "$dout" | grep -qi 'segfault' \
        && check "desktop did not crash" 0 || check "desktop did not crash" 1
else
    echo
    echo "== boot checks =="
    skipped "qemu boot checks (qemu or image unavailable)"
fi

# ------------------------------------------------------------ compile checks
echo
echo "== compile checks =="
ZIG=$(command -v zig 2>/dev/null)
[ -n "$ZIG" ] || ZIG=$(ls -d /c/Users/saket/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-x86_64-windows-*/zig.exe 2>/dev/null | head -1)
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
    skipped "compile checks (zig not found)"
fi

# ------------------------------------------------------------ summary
echo
if [ "$fail" -eq 0 ]; then
    printf 'RESULT: PASS - %d checks passed' "$pass"
    [ "$skip" -gt 0 ] && printf ', %d skipped' "$skip"
    printf '\n'
    exit 0
fi
printf 'RESULT: FAIL - %d of %d checks failed' "$fail" $((pass + fail))
[ "$skip" -gt 0 ] && printf ', %d skipped' "$skip"
printf '\n'
exit 1
