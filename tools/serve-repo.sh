#!/usr/bin/env bash
# ============================================================
#  tools/serve-repo.sh - serve a Mystic package repository
#
#  QEMU's user-mode networking gives the guest 10.0.2.15 and makes
#  the host reachable at 10.0.2.2, so `pkg` in the VM fetches from
#  here. Nothing large is baked into the image.
#
#      ./tools/serve-repo.sh            build the repo and serve it
#      ./tools/serve-repo.sh --index    only (re)build the index
# ============================================================
set -e
cd "$(dirname "$0")/.."

REPO=${REPO_DIR:-build/repo}
PORT=${PORT:-8000}
mkdir -p "$REPO"

# --- packages -------------------------------------------------------
# Each app bundle in stage/ becomes a package. Optional extras (the
# browser engine tree) are added if their directories exist.
pack_one() {
    local name="$1" src="$2" desc="$3"
    [ -d "$src" ] || return 0
    local tgz="$REPO/$name.tar.gz"
    if [ ! -f "$tgz" ] || [ -n "$(find "$src" -newer "$tgz" -print -quit 2>/dev/null)" ]; then
        echo "  packing $name"
        tar -czf "$tgz" -C "$src" . 2>/dev/null
    fi
    local size sha
    size=$(wc -c < "$tgz")
    sha=$(sha256sum "$tgz" | cut -d' ' -f1)
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "1.0" "$size" "$sha" "$name.tar.gz" "$desc" >> "$REPO/index.tsv.new"
}

: > "$REPO/index.tsv.new"
for d in stage/usr/share/mystic/apps/*/; do
    [ -d "$d" ] || continue
    name=$(basename "$d")
    desc=$(sed -n 's/^desc *= *//p' "$d/manifest" 2>/dev/null | head -1)
    pack_one "$name" "$d" "${desc:-Mystic application}"
done
# the browser engine, if it has been fetched by tools/fetch-browser.py
pack_one "browser" "build/browser-root" "Chromium headless engine (installs to /root/apps)"

mv "$REPO/index.tsv.new" "$REPO/index.tsv"
echo "repository: $REPO  ($(wc -l < "$REPO/index.tsv") packages, $(du -sh "$REPO" | cut -f1))"

if [ "${1:-}" != "--index" ]; then
    echo "serving on http://0.0.0.0:$PORT  (guest: http://10.0.2.2:$PORT)"
    cd "$REPO" && exec python -m http.server "$PORT"
fi
