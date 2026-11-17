#!/usr/bin/env bash
# ============================================================
#  tools/check-size.sh - enforce the shipping rule
#
#  THE RULE: anything that needs more than 40 MB of storage is
#  NOT shipped in the boot image. It goes in the package
#  repository (tools/serve-repo.sh) and is installed at runtime
#  with `pkg install` onto the data disk.
#
#  The image is kernel + initramfs. This script measures both,
#  lists every staged file over 1 MB, and fails if the total is
#  over budget so a big feature cannot sneak in unnoticed.
#
#      ./tools/check-size.sh            check the current build
#      LIMIT_MB=48 ./tools/check-size.sh
# ============================================================
set -u
cd "$(dirname "$0")/.."

LIMIT_MB=${LIMIT_MB:-40}
KERNEL=build/cache/vmlinuz-virt
CPIO=build/initramfs.cpio.gz

mb() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }
bytes() { wc -c < "$1" 2>/dev/null || echo 0; }

kb=$(bytes "$KERNEL"); cb=$(bytes "$CPIO")
total=$((kb + cb))

echo "image contents"
printf '  %-28s %6.1f MB\n' "kernel" "$(mb "$kb")"
printf '  %-28s %6.1f MB\n' "initramfs (userspace)" "$(mb "$cb")"
printf '  %-28s %6.1f MB   limit %s MB\n' "TOTAL" "$(mb "$total")" "$LIMIT_MB"

echo
echo "staged files over 1 MB"
big=0
while IFS= read -r line; do
    size=${line%% *}
    path=${line#* }
    [ -n "$path" ] || continue
    printf '  %6.1f MB  %s\n' "$(mb "$size")" "$path"
    big=$((big + 1))
done <<EOF
$(find stage -type f -size +1024k -printf '%s %p\n' 2>/dev/null | sort -rn | head -20)
EOF
[ "$big" -eq 0 ] && echo "  (none)"

forbidden=0
echo
echo "rule check: anything >40 MB must be installable, not shipped"
for p in build/browser-root build/cache/ubuntu-base.tar.gz; do
    [ -e "$p" ] || continue
    s=$(du -sm "$p" 2>/dev/null | cut -f1)
    if grep -q "$(basename "$p")" <(find stage -mindepth 1 2>/dev/null); then
        echo "  VIOLATION: $p is staged inside the image"
        forbidden=1
    else
        echo "  ok: $p (${s} MB) stays outside the image"
    fi
done

status=0
if [ "$total" -gt $((LIMIT_MB * 1048576)) ]; then
    echo
    echo "FAIL: image is $(mb "$total") MB, over the ${LIMIT_MB} MB budget."
    echo "      Move the feature into the package repository instead."
    status=1
fi
[ "$forbidden" -ne 0 ] && status=1
[ "$status" -eq 0 ] && echo && echo "PASS: image is within budget and nothing large is baked in."
exit "$status"
