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
    # vlc and engine are not plain stage bundles. Their packages carry a
    # runtime closure (libVLC 103 MB, Chromium 683 MB) and are produced by
    # tools/fetch-vlc.py and tools/fetch-engine.py; packing the stage
    # directory here would overwrite the real tarball with an image
    # placeholder and install an app whose library is not there.
    if [ "$name" = vlc ] || [ "$name" = engine ]; then continue; fi
    desc=$(sed -n 's/^desc *= *//p' "$d/manifest" 2>/dev/null | head -1)
    pack_one "$name" "$d" "${desc:-Mystic application}"
done
# keep the vlc row tools/fetch-vlc.py wrote, if the package is there
if [ -f "$REPO/vlc.tar.gz" ] && [ -f "$REPO/vlc.row" ]; then
    cat "$REPO/vlc.row" >> "$REPO/index.tsv.new"
fi
# keep the engine row tools/fetch-engine.py wrote. This replaces the old
# 'pack_one browser build/browser-root' line: that tree has no manifest, so the
# row it published installed a bundle pkg could not read a manifest from, and
# nothing ever ran. The real package is build/repo/engine.tar.gz and its row is
# produced (and signed) by the tool that builds it.
if [ -f "$REPO/engine.tar.gz" ] && [ -f "$REPO/engine.row" ]; then
    cat "$REPO/engine.row" >> "$REPO/index.tsv.new"
fi

mv "$REPO/index.tsv.new" "$REPO/index.tsv"

# --- sign it ---------------------------------------------------------
# pkg refuses an index whose Ed25519 signature does not verify, so an
# unsigned development mirror would be useless. The signing key is the
# publisher's and is deliberately not in the repository: build/ is ignored.
KEY=${REPO_KEY:-build/sign/repo.key}
PUB=${REPO_PUB:-stage/etc/mystic/repo.pub}
if [ -f "$KEY" ]; then
    if [ -f "$PUB" ]; then
        python tools/sign-index.py sign --seed "$KEY" --in "$REPO/index.tsv" --pub "$PUB"
        cp "$PUB" "$REPO/index.pub"
    else
        python tools/sign-index.py sign --seed "$KEY" --in "$REPO/index.tsv"
    fi
else
    echo "WARNING: no signing key at $KEY - this index is UNSIGNED and pkg will refuse it." >&2
    echo "         create one once: python tools/sign-index.py keygen --seed $KEY --pub $PUB" >&2
    echo "         (then rebuild the image so $PUB is the key it trusts)" >&2
fi

echo "repository: $REPO  ($(wc -l < "$REPO/index.tsv") packages, $(du -sh "$REPO" | cut -f1))"

if [ "${1:-}" != "--index" ]; then
    echo "serving on http://0.0.0.0:$PORT  (guest: http://10.0.2.2:$PORT)"
    cd "$REPO" && exec python -m http.server "$PORT"
fi
