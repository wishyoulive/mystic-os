#!/usr/bin/env bash
# ============================================================
#  tools/build-desktop.sh
#  Compile the desktop for Linux/x86-64 and place it (with its
#  fonts) inside our staging rootfs.
#
#  zig is doing the job of a cross-compiler here: it can emit a
#  static musl binary from Windows without a Linux toolchain.
# ============================================================
set -e
cd "$(dirname "$0")/.."

ZIG="${ZIG:-}"
if [ -z "$ZIG" ]; then
    if command -v zig >/dev/null 2>&1; then ZIG=zig
    else
        for cand in /c/Users/saket/AppData/Local/Microsoft/WinGet/Packages/zig.zig_*/zig-x86_64-windows-*/zig.exe; do
            [ -x "$cand" ] && ZIG="$cand" && break
        done
    fi
fi
[ -n "$ZIG" ] || { echo "error: zig not found" >&2; exit 1; }
echo "==> compiler: $ZIG ($("$ZIG" version))"

SRC="src/main.c src/apps.c src/term.c src/sha256.c src/wmsrv.c src/ui.c src/wallpaper.c src/gfx.c src/font.c src/fb.c src/input.c src/theme.c"
SRC="$SRC src/shortcuts.c"
SRC="$SRC src/apps/sysinfo.c src/notify.c src/clipboard.c"
DEFS=""
for app in files settings monitor notes store calc; do
    if [ -f "src/apps/$app.c" ]; then
        SRC="$SRC src/apps/$app.c"
        DEFS="$DEFS -DWITH_$(echo $app | tr a-z A-Z)"
    fi
done
echo "==> apps: $DEFS"
OUT=build/mystic-desktop

mkdir -p build stage/usr/bin stage/usr/share/fonts

echo "==> compiling $OUT"
# -static so the binary needs nothing but the kernel;
# -Os keeps the image small; no libc beyond musl.
"$ZIG" cc -target x86_64-linux-musl -Os -static -fno-sanitize=all -Isrc \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" $SRC $DEFS -lm -lutil

size=$(du -h "$OUT" | cut -f1)
echo "    $size  $(wc -l < /dev/null 2>/dev/null)$OUT"

# put it where the OS expects it
cp "$OUT" stage/usr/bin/mystic-desktop
chmod +x stage/usr/bin/mystic-desktop
cp assets/fonts/*.ttf assets/fonts/LICENSE.txt stage/usr/share/fonts/ 2>/dev/null || true

# ---- the repository signature verifier ------------------------------
# pkg will not read a package index it cannot authenticate, so this
# binary ships in the image. It is static, has no dependencies, and
# implements Ed25519 verification from src/tools/ed25519.c.
echo "==> compiling ed25519-verify"
"$ZIG" cc -target x86_64-linux-musl -Os -static -Wall -Wextra \
    -Wno-unused-parameter -o build/ed25519-verify src/tools/ed25519.c
cp build/ed25519-verify stage/usr/bin/ed25519-verify
chmod +x stage/usr/bin/ed25519-verify

echo "==> installed into stage/usr/bin + stage/usr/share/fonts"
