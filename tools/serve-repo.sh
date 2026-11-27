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
    # An interrupted earlier run leaves a truncated tgz that `find -newer` will
    # never repack; publishing it produces a row that matches the corrupt bytes
    # (so pkg's size and sha256 checks both pass) and the install only dies
    # later at unpack. 11 lines of gzip is cheaper than that.
    if command -v gzip >/dev/null 2>&1 && ! gzip -t "$tgz" 2>/dev/null; then
        echo "error: $tgz is not a valid gzip stream - refusing to publish it." >&2
        echo "       delete it and run this script again to repack $name." >&2
        exit 1
    fi
    local size sha
    size=$(wc -c < "$tgz")
    sha=$(sha256sum "$tgz" | cut -d' ' -f1)
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "1.0" "$size" "$sha" "$name.tar.gz" "$desc" >> "$REPO/index.tsv.new"
}

# A .row file (written by tools/fetch-vlc.py, tools/fetch-engine.py, and
# tools/rebuild-vlc.py) carries the fields this script cannot know - version,
# file name and description - but NOT trustworthy bytes: its size and sha256
# are a snapshot from whenever that tool last wrote it. Publishing the row
# verbatim is how build/repo/index.tsv came to describe a 39,548,165-byte
# vlc.tar.gz with sha 682114a4 while the 39,211,461-byte file on disk hashed
# d9951520: the next `pkg install vlc` would download the whole package and
# then die on "size mismatch for vlc.tar.gz" (pkg checks size, then sha256,
# before it unpacks anything), every time. Recompute both fields from the
# tarball that is actually being served, keep the others, and say so when they
# disagreed.
row_of_artifact() {
    local row="$1" tgz="$2" size sha row_size row_sha
    [ -f "$tgz" ] && [ -f "$row" ] || return 0
    if command -v gzip >/dev/null 2>&1 && ! gzip -t "$tgz" 2>/dev/null; then
        echo "error: $tgz is not a valid gzip stream - refusing to publish it." >&2
        echo "       it is the artifact $(basename "$row") describes; rebuild it first." >&2
        exit 1
    fi
    size=$(wc -c < "$tgz")
    sha=$(sha256sum "$tgz" | cut -d' ' -f1)
    row_size=$(cut -f3 "$row")
    row_sha=$(cut -f4 "$row")
    if [ "$row_size" != "$size" ] || [ "$row_sha" != "$sha" ]; then
        echo "  correcting $(basename "$row"): it described different bytes" >&2
        echo "    row  : size=$row_size sha=$row_sha" >&2
        echo "    file : size=$size sha=$sha" >&2
    fi
    awk -F'\t' -v OFS='\t' -v s="$size" -v h="$sha" '{$3=s; $4=h; print}' "$row" \
        >> "$REPO/index.tsv.new"
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
row_of_artifact "$REPO/vlc.row" "$REPO/vlc.tar.gz"
# keep the engine row tools/fetch-engine.py wrote. This replaces the old
# 'pack_one browser build/browser-root' line: that tree has no manifest, so the
# row it published installed a bundle pkg could not read a manifest from, and
# nothing ever ran. The real package is build/repo/engine.tar.gz and its row is
# produced (and signed) by the tool that builds it.
if [ -f "$REPO/engine.tar.gz" ] && [ -f "$REPO/engine.row" ]; then
    row_of_artifact "$REPO/engine.row" "$REPO/engine.tar.gz"
fi

# --- sign, then publish -----------------------------------------------
# pkg refuses an index whose Ed25519 signature does not verify, so an
# unsigned development mirror would be useless. The signing key is the
# publisher's and is deliberately not in the repository: build/ is ignored.
#
# Order matters, and it used to be wrong: `mv index.tsv.new index.tsv` ran
# BEFORE signing. A failed sign (no key, unreadable key, or - the documented
# rotation hazard - a seed whose public half is not the one sign-index.py is
# told to publish) therefore left the new index beside the OLD signature, and
# pkg verifies the pair before it reads a single row, so one failed sign took
# the WHOLE mirror down until someone noticed. Now the signature is made over
# index.tsv.new and published before the index: a failure publishes nothing,
# and a reader that sees the new index always has its signature beside it -
# the same order pkg itself uses when it installs an index (stage/usr/bin/pkg:
# 738-746, "publish the signature first").
KEY=${REPO_KEY:-build/sign/repo.key}
PUB=${REPO_PUB:-stage/etc/mystic/repo.pub}
sign_failed() {
    rm -f "$REPO/index.tsv.new" "$REPO/index.tsv.new.sig"
    echo "error: signing failed - nothing was published; $REPO/index.tsv and" >&2
    echo "       its signature are unchanged. Fix $KEY (or $PUB if that is what" >&2
    echo "       sign-index.py complained about), then run this script again." >&2
    exit 1
}
signed=0
if [ -f "$KEY" ]; then
    if [ -f "$PUB" ]; then
        python tools/sign-index.py sign --seed "$KEY" --in "$REPO/index.tsv.new" \
            --pub "$PUB" || sign_failed
        cp "$PUB" "$REPO/index.pub"
    else
        python tools/sign-index.py sign --seed "$KEY" --in "$REPO/index.tsv.new" \
            || sign_failed
    fi
    signed=1
else
    echo "WARNING: no signing key at $KEY - this index is UNSIGNED and pkg will refuse it." >&2
    echo "         create one once: python tools/sign-index.py keygen --seed $KEY --pub $PUB" >&2
    echo "         (then rebuild the image so $PUB is the key it trusts; any" >&2
    echo "         index.tsv.sig already in $REPO no longer matches)" >&2
fi

# publish: the signature first, then the index
if [ "$signed" = 1 ]; then
    mv "$REPO/index.tsv.new.sig" "$REPO/index.tsv.sig"
fi
mv "$REPO/index.tsv.new" "$REPO/index.tsv"

echo "repository: $REPO  ($(wc -l < "$REPO/index.tsv") packages, $(du -sh "$REPO" | cut -f1))"

if [ "${1:-}" != "--index" ]; then
    echo "serving on http://0.0.0.0:$PORT  (guest: http://10.0.2.2:$PORT)"
    cd "$REPO" && exec python -m http.server "$PORT"
fi
