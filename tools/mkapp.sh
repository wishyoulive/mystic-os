#!/usr/bin/env bash
# ============================================================
#  tools/mkapp.sh - build and package an external Mystic OS app
#
#    ./tools/mkapp.sh clock
#
#  An app bundle is a directory:
#      /usr/share/mystic/apps/<id>/manifest
#      /usr/share/mystic/apps/<id>/bin
#
#  The app is a normal static binary that links the client SDK and
#  the drawing library. It runs as its own process.
# ============================================================
set -e
cd "$(dirname "$0")/.."

APP="${1:-}"
[ -n "$APP" ] || { echo "usage: ./tools/mkapp.sh <app-id>" >&2; exit 1; }

SRC="src/client/apps/$APP.c"
[ -f "$SRC" ] || { echo "error: no $SRC" >&2; exit 1; }

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

OUT="build/app-$APP"
echo "==> compiling $SRC"
# gfx.c and font.c are generic: the app draws its own pixels with them
"$ZIG" cc -target x86_64-linux-musl -Os -static -Isrc -Isrc/client \
    -o "$OUT" "$SRC" src/client/mystic_client.c src/gfx.c src/font.c src/ui.c -lm

DEST="stage/usr/share/mystic/apps/$APP"
mkdir -p "$DEST"
cp "$OUT" "$DEST/bin"
chmod +x "$DEST/bin"

# the manifest is what the launcher reads
TITLE=$(printf '%s' "$APP" | sed 's/^./\U&/')
DESC="${2:-Mystic OS application}"
[ -f "$DEST/manifest" ] || cat > "$DEST/manifest" <<EOF
name = $TITLE
desc = $DESC
exec = bin
EOF

echo "    $(du -h "$DEST/bin" | cut -f1)  -> $DEST"
