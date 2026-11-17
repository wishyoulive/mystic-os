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
#                                 the static and compile checks
#    ./tools/smoke.sh --quick     static + compile checks only, no QEMU
#
#  The static checks need no QEMU. They parse the tree that actually
#  ships (src/, stage/, assets/) and assert the things that have gone
#  silently wrong in this project before: a text scale that font_get()
#  never applied, a terminal drawn in a proportional face, a taskbar
#  that drew itself with nothing open, a stale binary sitting inside
#  a freshly packed image, a CRLF in /init, and an app bundle linked
#  without the objects it calls into.
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

# ------------------------------------------------------------ static checks
# No QEMU here: these read the tree that gets packed. They run under
# --quick too, because each one is a regression this project shipped.
echo
echo "== static checks =="

mtime() {                        # mtime <file> -> readable timestamp
    stat -c '%y' "$1" 2>/dev/null || date -r "$1" '+%Y-%m-%d %H:%M:%S'
}

# ---- text scale: one number in /etc/mystic/ui.conf must reach font_get()
UI_CONF=stage/etc/mystic/ui.conf
UI_C=src/ui.c
if [ ! -f "$UI_CONF" ]; then
    check "text scale: $UI_CONF present" 0
    note "ui_scale_load() would fall back to 1.0 - the shipped scale is never exercised"
else
    check "text scale: $UI_CONF present" 1
    # Same reading the C does: first line whose first five characters are
    # "scale" (strncmp in ui_scale_load), then atof() on what follows '='.
    # atof() takes a leading number and stops at junk, so "1.2x" is 1.2 to
    # the C and has to be 1.2 here too - a stricter rule would fail a config
    # that actually works.
    ui_val=$(awk '
        /^scale/ {
            i = index($0, "=")
            if (i > 0) {
                v = substr($0, i + 1)
                gsub(/^[ \t]+|[ \t\r]+$/, "", v)
                print v
                exit
            }
        }' "$UI_CONF")
    # The number atof() would read: optional sign, digits, optional fraction,
    # optional exponent. Anything atof() cannot start a number with is 0.
    ui_num=$(printf '%s\n' "$ui_val" | awk '
        { if (match($0, /^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?/))
              print substr($0, RSTART, RLENGTH) }')
    if [ -z "$ui_val" ]; then
        check "text scale: $UI_CONF has a 'scale = <number>' line" 0
        note "no line starts with 'scale' and holds '=' - ui_scale_load() keeps 1.0"
    elif [ -z "$ui_num" ]; then
        check "text scale: $UI_CONF has a 'scale = <number>' line" 0
        note "atof('$ui_val') is 0, which ui_scale_load() clamps to 0.8 - the file is ignored"
    else
        # The three outcomes the C has: accepted, clamped to the bounds, or
        # (an overflowing literal) rejected by isfinite() and left at 1.0.
        ui_verdict=$(awk -v v="$ui_num" 'BEGIN {
            x = v + 0
            if (x > 1e308) print "inf"
            else if (x < 0.8 || x > 1.6) print "clamp"
            else print "ok"
        }')
        case "$ui_verdict" in
            ok)    check "text scale: scale = $ui_num (inside 0.8..1.6)" 1
                   [ "$ui_num" != "$ui_val" ] && note "atof() reads '$ui_num' out of '$ui_val' - the rest of the line is ignored" ;;
            inf)   check "text scale: scale = $ui_num (inside 0.8..1.6)" 0
                   note "atof('$ui_val') overflows to infinity; isfinite() rejects the line, so the scale stays 1.0" ;;
            *)     check "text scale: scale = $ui_num (inside 0.8..1.6)" 0
                   note "ui_scale_load() clamps to 0.8..1.6 - '$ui_num' in the file is not the scale that ships" ;;
        esac
    fi
fi

# Each function the rest of the desktop calls. A rename here compiles into
# a link error only when someone remembers to rebuild.
ui_fn() {                        # ui_fn <name> <grep -E pattern>
    if grep -qE -- "$2" "$UI_C" 2>/dev/null; then
        check "text scale: src/ui.c defines $1" 1
    else
        check "text scale: src/ui.c defines $1" 0
        note "no /$2/ in $UI_C"
    fi
}
ui_fn 'ui_scale_get(void)'     'float[[:space:]]+ui_scale_get\(void\)'
ui_fn 'ui_scale_load(const char *)' 'void[[:space:]]+ui_scale_load\(const char'
ui_fn 'ui_px(int)'             'int[[:space:]]+ui_px\(int'

if grep -qF 'ui_scale_load("/etc/mystic/ui.conf")' src/main.c 2>/dev/null; then
    check "text scale: main.c loads /etc/mystic/ui.conf" 1
else
    check "text scale: main.c loads /etc/mystic/ui.conf" 0
    note "the file is packed at $UI_CONF but nothing reads it"
fi

# The scale has to be applied inside font_get(), not at a call site: that
# is what makes it impossible for a layout to forget it.
font_get_body=$(awk '/^Font \*font_get\(/ { inside = 1 } inside { print } inside && /^}/ { exit }' src/font.c)
if [ -z "$font_get_body" ]; then
    check "text scale: font_get() applies ui_scale_get()" 0
    note "font_get() not found in src/font.c"
elif printf '%s\n' "$font_get_body" | grep -q 'ui_scale_get('; then
    check "text scale: font_get() applies ui_scale_get()" 1
else
    check "text scale: font_get() applies ui_scale_get()" 0
    note "font sizes would ignore the scale - every scaled metric drifts from the text"
fi

# ---- monospace terminal: a proportional face makes columns drift
MONO_TTF=assets/fonts/JetBrainsMono-Regular.ttf
STAGED_MONO=stage/usr/share/fonts/JetBrainsMono-Regular.ttf
if [ -s "$MONO_TTF" ]; then
    check "monospace: $MONO_TTF present" 1
    # A 404 page saved with a .ttf name has the same byte count as nothing
    # useful; the sfnt magic is what stb_truetype actually needs.
    mono_magic=$(od -An -tx1 -N4 "$MONO_TTF" 2>/dev/null | tr -d ' \n')
    case "$mono_magic" in
        00010000|74727565|4f54544f|74746366)
            check "monospace: $MONO_TTF is a real sfnt (magic $mono_magic)" 1 ;;
        *)
            check "monospace: $MONO_TTF is a real sfnt (magic $mono_magic)" 0
            note "stb_truetype cannot parse this - the terminal loses its face" ;;
    esac
else
    check "monospace: $MONO_TTF present" 0
    note "font_get(FONT_MONO, ...) would return NULL and the terminal cannot draw"
    skipped "monospace: sfnt magic (no file)"
fi
if [ -s "$STAGED_MONO" ]; then
    check "monospace: font installed at $STAGED_MONO" 1
else
    check "monospace: font installed at $STAGED_MONO" 0
    note "tools/build-desktop.sh copies assets/fonts/*.ttf there - run it"
fi
if grep -q 'JetBrainsMono-Regular.ttf' src/font.c 2>/dev/null; then
    check "monospace: font.c names the JetBrains Mono file" 1
else
    check "monospace: font.c names the JetBrains Mono file" 0
    note "FONT_FILES[FONT_MONO] must match the file that ships"
fi
# FONT_MONO must be the face handed to the terminal's draw call, not just
# present somewhere in the file: loading it into a variable nobody passes
# on draws the terminal in whatever font happens to be in scope.
mono_get=""
term_call=$(grep -n 'term_draw(' src/main.c 2>/dev/null | head -1 | cut -d: -f1)
# The load that matters is the last one before the draw call: main.c also
# loads FONT_MONO to size a terminal window, which proves nothing about
# what term_draw() itself is handed.
[ -n "$term_call" ] && mono_get=$(grep -n 'font_get(FONT_MONO' src/main.c 2>/dev/null \
    | awk -F: -v t="$term_call" '$1 + 0 < t + 0 { l = $1 } END { print l }')
mono_var=""
[ -n "$mono_get" ] && mono_var=$(sed -n "${mono_get}p" src/main.c \
    | sed -n 's/^.*[*][[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*=[[:space:]]*font_get(FONT_MONO.*/\1/p')
if [ -z "$mono_get" ] || [ -z "$term_call" ]; then
    check "monospace: term_draw() uses FONT_MONO" 0
    note "src/main.c has no font_get(FONT_MONO ...) before a term_draw( call"
elif [ "$term_call" -le "$mono_get" ] || [ $((term_call - mono_get)) -gt 20 ]; then
    check "monospace: term_draw() uses FONT_MONO" 0
    note "the mono face is loaded at src/main.c:$mono_get but term_draw( is at :$term_call"
elif [ -n "$mono_var" ] \
     && sed -n "${term_call},$((term_call + 5))p" src/main.c | grep -qw -- "$mono_var"; then
    check "monospace: term_draw() is passed the FONT_MONO face '$mono_var' (src/main.c:$term_call)" 1
elif sed -n "${term_call}p" src/main.c | grep -q 'font_get(FONT_MONO'; then
    check "monospace: term_draw() is passed the FONT_MONO face (src/main.c:$term_call)" 1
else
    check "monospace: term_draw() uses FONT_MONO" 0
    note "src/main.c:$mono_get loads FONT_MONO into '${mono_var:-?}' but the term_draw( call at :$term_call does not receive it"
fi

# ---- taskbar: the bar is a list of open windows, and nothing else
tb_count=$(grep -cE 'static int taskbar_count\(void\)' src/main.c 2>/dev/null)
tb_at=$(grep -cE 'static Window \*taskbar_at\(int' src/main.c 2>/dev/null)
[ "${tb_count:-0}" -gt 0 ] && check "taskbar: taskbar_count() defined in src/main.c" 1 \
                          || check "taskbar: taskbar_count() defined in src/main.c" 0
[ "${tb_at:-0}" -gt 0 ] && check "taskbar: taskbar_at() defined in src/main.c" 1 \
                       || check "taskbar: taskbar_at() defined in src/main.c" 0

dock_body=$(awk '/^static int dock_rect\(/ { inside = 1 } inside { print } inside && /^}/ { exit }' src/main.c)
if [ -z "$dock_body" ]; then
    check "taskbar: dock_rect() returns 0 when nothing is open ('n <= 0' guard)" 0
    note "dock_rect() not found in src/main.c"
elif printf '%s\n' "$dock_body" | grep -q 'n <= 0' \
     && printf '%s\n' "$dock_body" | grep -q 'return 0'; then
    check "taskbar: dock_rect() returns 0 when nothing is open ('n <= 0' guard)" 1
else
    check "taskbar: dock_rect() returns 0 when nothing is open ('n <= 0' guard)" 0
    note "with zero windows the bar would draw an empty pill and swallow clicks"
fi

# A bar that is never asked for its geometry is dead code; the draw loop and
# the click handler must both consult it (one definition, two call sites).
dock_uses=$(grep -c 'dock_rect(' src/main.c 2>/dev/null)
if [ "${dock_uses:-0}" -ge 3 ]; then
    check "taskbar: dock_rect() used by draw and hit-test ($dock_uses references)" 1
else
    check "taskbar: dock_rect() used by draw and hit-test ($dock_uses references)" 0
    note "expected the definition plus a call in the draw path and in the click path"
fi

# ---- stale build guard: a fresh image around an old binary has bitten us.
# The check is against the sources mystic-desktop is actually built from:
# the .c files build-desktop.sh links, plus the headers those reach through
# #include. A file in src/ that none of them touches cannot make the binary
# stale, so it is listed as ignored instead of failing the run.
BIN=stage/usr/bin/mystic-desktop
DESKTOP_SH=tools/build-desktop.sh
if [ ! -f "$BIN" ] || [ ! -f "$DESKTOP_SH" ]; then
    if [ -f "$BIN" ]; then
        check "build: $BIN present" 1
    else
        check "build: $BIN present" 0
        note "the image would boot without a desktop - run ./tools/build-desktop.sh"
    fi
    if [ -f "$DESKTOP_SH" ]; then
        check "build: $DESKTOP_SH present" 1
    else
        check "build: $DESKTOP_SH present" 0
        note "without it there is no way to know what the binary was built from"
    fi
    skipped "build: stale check (needs both files)"
else
    check "build: $BIN present" 1
    check "build: $DESKTOP_SH present" 1
    desktop_apps=$(sed -n 's/^for app in \(.*\); do[[:space:]]*$/\1/p' "$DESKTOP_SH" 2>/dev/null | head -1)

    # Which .c files does build-desktop.sh actually link? It names src/*.c
    # literally and builds src/apps/<app>.c from a list, so both forms are
    # read back out of the script rather than duplicated here.
    built_c=""
    for f in src/*.c src/apps/*.c; do
        [ -f "$f" ] || continue
        in_build=0
        grep -qF -- "$f" "$DESKTOP_SH" 2>/dev/null && in_build=1
        case "$f" in
            src/apps/*.c)
                app=${f#src/apps/}; app=${app%.c}
                if [ -z "$desktop_apps" ]; then
                    in_build=1            # unreadable list: be conservative
                else
                    case " $desktop_apps " in
                        *" $app "*) in_build=1 ;;
                    esac
                fi ;;
        esac
        [ "$in_build" -eq 1 ] && built_c="$built_c $f"
    done

    if [ -z "$built_c" ]; then
        # With no source list every comparison would be vacuous and pass,
        # which is the silent-failure shape this project keeps hitting.
        check "build: $BIN newer than every source it is built from" 0
        note "no source is named in $DESKTOP_SH - the stale guard cannot work"
    else
        # A header only matters if something the desktop compiles #includes
        # it, so walk the includes from the real sources. This is what keeps
        # a module that has landed in src/ but is not wired into the desktop
        # yet from being reported as a stale build.
        reach=""
        frontier="$built_c"
        while [ -n "$frontier" ]; do
            nxt=""
            for f in $frontier; do
                for inc in $(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$f"); do
                    case " $reach " in *" $inc "*) continue ;; esac
                    reach="$reach $inc"
                    for p in "src/$inc" "src/apps/$inc"; do
                        [ -f "$p" ] && nxt="$nxt $p"
                    done
                done
            done
            frontier="$nxt"
        done

        # main.c is the one file a desktop cannot be built without. If the
        # source list were emptied or rewritten past this script's reader,
        # every comparison below would silently be against the wrong set.
        case " $built_c " in
            *" src/main.c "*) check "build: $DESKTOP_SH compiles src/main.c" 1 ;;
            *) check "build: $DESKTOP_SH compiles src/main.c" 0
               note "the source list was not read back from $DESKTOP_SH: '$built_c'" ;;
        esac

        # A header a compiled unit #includes must have its module linked too,
        # or the feature is declared and never built (a module that lands in
        # src/ and is used by main.c without being added to the SRC list is
        # exactly how the desktop grew a shadow copy of itself before).
        missing_c=""
        for inc in $reach; do
            base=${inc%.h}
            for sib in "src/$base.c" "src/apps/$base.c"; do
                [ -f "$sib" ] || continue
                case " $built_c " in *" $sib "*) continue ;; esac
                case " $missing_c " in *" $sib "*) continue ;; esac
                missing_c="$missing_c $sib"
            done
        done
        if [ -z "$missing_c" ]; then
            check "build: every included header's module is linked" 1
        else
            check "build: every included header's module is linked" 0
            note "a compiled unit includes the header, but $DESKTOP_SH never links:$missing_c"
        fi

        newest_src=""
        newer_count=0
        ignored_src=""
        unlinked_c=""

        # Sources the desktop does not link cannot make its binary stale: an
        # app built by tools/mkapp.sh, or a module not wired in yet. Say so
        # out loud rather than deciding it silently.
        for f in src/*.c src/apps/*.c; do
            [ -f "$f" ] || continue
            case " $built_c " in *" $f "*) continue ;; esac
            unlinked_c="$unlinked_c $f"
            [ "$f" -nt "$BIN" ] && ignored_src="$ignored_src $f"
        done

        for f in $built_c src/*.h src/apps/*.h; do
            [ -f "$f" ] || continue
            case "$f" in
                *.h)
                    base=${f##*/}
                    case " $reach " in
                        *" $base "*) ;;              # reaches a compiled unit
                        *)
                            [ "$f" -nt "$BIN" ] && ignored_src="$ignored_src $f"
                            continue ;;
                    esac ;;
            esac
            [ "$f" -nt "$BIN" ] || continue
            newer_count=$((newer_count + 1))
            if [ -z "$newest_src" ] || [ "$f" -nt "$newest_src" ]; then newest_src=$f; fi
        done
        if [ -z "$newest_src" ]; then
            check "build: $BIN newer than every source it is built from" 1
        else
            check "build: $BIN newer than every source it is built from" 0
            note "binary: $(mtime "$BIN")   $BIN"
            note "newest: $(mtime "$newest_src")   $newest_src  ($newer_count source file(s) newer)"
            note "the packed image would run the old desktop - run ./tools/build-desktop.sh"
        fi
        [ -n "$ignored_src" ] && note "not part of the desktop build, ignored:$ignored_src"
        [ -n "$unlinked_c" ] && note "not linked into the desktop at all (cannot ship until $DESKTOP_SH names it):$unlinked_c"
    fi

    # The app bundles are packed exactly as they sit on disk: image.sh
    # rebuilds the desktop but never re-runs tools/mkapp.sh. A bundle older
    # than the SDK sources it links ships stale code. Reported, not failed:
    # rebuilding is a separate step and this script does not touch the tree.
    stale_apps=""
    newest_app_src=""
    for bundle in stage/usr/share/mystic/apps/*/bin; do
        [ -f "$bundle" ] || continue
        id=${bundle%/bin}; id=${id##*/}
        [ -f "src/client/apps/$id.c" ] || continue
        newest="src/client/apps/$id.c"
        for f in src/client/mystic_client.c src/gfx.c src/font.c src/ui.c \
                 src/mystic.h src/ipc.h src/client/mystic_client.h; do
            [ -f "$f" ] && [ "$f" -nt "$newest" ] && newest=$f
        done
        if [ "$newest" -nt "$bundle" ]; then
            stale_apps="$stale_apps $id"
            if [ -z "$newest_app_src" ] || [ "$newest" -nt "$newest_app_src" ]; then
                newest_app_src=$newest
            fi
        fi
    done
    if [ -n "$stale_apps" ]; then
        note "stale app bundles (newer source than binary):$stale_apps"
        note "rebuild with ./tools/mkapp.sh <id> - newest source is $(mtime "$newest_app_src") $newest_app_src"
    fi
fi

# ---- regressions this project has actually shipped and had to hunt down
# 1) A CRLF in stage/init: the kernel execs /init and the shebang is
#    "#!/bin/sh\r", so PID 1 dies with ENOENT and the box panics with no
#    useful message. Zero information for the person looking at the screen.
INIT=stage/init
# grep cannot answer this: the git-bash build of grep strips CR from its
# input, so a CRLF file matches /\r/ zero times. Count the bytes instead.
crlf_count() { tr -cd '\r' < "$1" 2>/dev/null | wc -c | tr -d ' '; }
if [ ! -f "$INIT" ]; then
    check "history: $INIT present" 0
    note "the kernel has nothing to exec as PID 1"
elif [ "$(crlf_count "$INIT")" = "0" ]; then
    check "history: $INIT has no CRLF line endings" 1
else
    check "history: $INIT has no CRLF line endings" 0
    note "$(crlf_count "$INIT") CR byte(s): exec sees '#!/bin/sh\\r' and fails"
fi

# 2) An external app linked without the SDK objects produced a bundle that
#    built "successfully" and died at load with undefined symbols, and the
#    error had scrolled past because only the tail of the log was shown.
#    Proof: linking clock.c without src/ui.c gives
#      ld.lld: error: undefined symbol: ui_scale_get
#    because font.c calls it. These four files are not optional.
MKAPP=tools/mkapp.sh
if [ -f "$MKAPP" ]; then
    mk_missing=""
    for need in 'src/client/mystic_client.c' 'src/gfx.c' 'src/font.c' 'src/ui.c'; do
        grep -qF -- "$need" "$MKAPP" || mk_missing="$mk_missing $need"
    done
    if [ -z "$mk_missing" ]; then
        check "history: $MKAPP links the SDK objects" 1
    else
        check "history: $MKAPP links the SDK objects" 0
        note "missing from $MKAPP:$mk_missing - the bundle builds and dies at load with undefined symbols"
    fi
else
    check "history: $MKAPP links the SDK objects" 0
    note "no way to build an external app"
fi

# 3) -static is what lets the desktop run with nothing else in the initramfs.
#    Without it the binary wants ld-musl-x86_64.so.1, which the image does
#    not carry, and PID 1's exec fails with no output.
if [ -f "$DESKTOP_SH" ]; then
    if grep -qF -- '-static' "$DESKTOP_SH"; then
        check "history: $DESKTOP_SH passes -static" 1
    else
        check "history: $DESKTOP_SH passes -static" 0
        note "the desktop would need a dynamic loader the initramfs does not have"
    fi
else
    skipped "history: $DESKTOP_SH -static (script missing)"
fi

# ------------------------------------------------------------ compile checks
echo
echo "== compile checks =="
ZIG=$(command -v zig 2>/dev/null)
[ -n "$ZIG" ] || ZIG=$(ls -d /c/Users/saket/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-x86_64-windows-*/zig.exe 2>/dev/null | head -1)
if [ -n "$ZIG" ]; then
    for f in src/*.c src/apps/*.c; do
        [ -f "$f" ] || continue
        if "$ZIG" cc -target x86_64-linux-musl -Os -static -Wall -Wextra -Wno-unused-parameter \
             -c "$f" -Isrc -o /tmp/smoke.o 2>/tmp/smoke.err; then
            check "compiles clean: $f" 1
        else
            check "compiles clean: $f" 0
            head -3 /tmp/smoke.err | sed 's/^/        /'
        fi
    done
    # The external apps are separate processes built by tools/mkapp.sh, which
    # image.sh never calls: nothing else in the project compiles them, so a
    # broken client app ships unnoticed unless this loop covers it.
    for f in src/client/*.c src/client/apps/*.c; do
        [ -f "$f" ] || continue
        if "$ZIG" cc -target x86_64-linux-musl -Os -static -Wall -Wextra -Wno-unused-parameter \
             -c "$f" -Isrc -Isrc/client -o /tmp/smoke.o 2>/tmp/smoke.err; then
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
