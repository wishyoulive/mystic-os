#!/usr/bin/env bash
# ============================================================
#  tools/fetch-kernel.sh
#  Get a kernel + exactly the modules Mystic OS needs, and put
#  them where the image builder will find them.
#
#  Why: Alpine ships drivers as loadable modules, and Alpines's
#  default "virt" kernel has no built-in graphics. We take the kernel
#  and its modules from the same package so their versions always match
#  (a mismatch is the classic "Invalid module format" failure).
# ============================================================
set -e
cd "$(dirname "$0")/.."

VERSION=6.18.63
PKG=linux-virt-${VERSION}-r0
BASE=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64
CACHE=build/cache
STAGE=stage
KDIR=$CACHE/$PKG

# Whole subtrees, not a hand-picked list. Cherry-picking individual .ko files
# means chasing "unknown symbol" errors one build at a time; the entire GPU,
# framebuffer and i2c trees are ~2 MB compressed, so we take them all.
# (A later stage can compute the minimal closure - that is a job for a script,
#  not for a human guessing.)
MODULE_TREES="
kernel/drivers/gpu
kernel/drivers/video
kernel/drivers/i2c
kernel/drivers/input
kernel/drivers/block
kernel/drivers/net
kernel/net/core
kernel/net/packet
kernel/fs/fat
kernel/lib
kernel/fs/ext4
kernel/fs/jbd2
kernel/fs/nls
"

mkdir -p "$CACHE"

# ---- 1. the package (43 MB, cached) ----------------------------------
if [ ! -d "$KDIR" ]; then
    echo "==> downloading $PKG"
    curl -sL -o "$CACHE/$PKG.apk" "$BASE/$PKG.apk"
    mkdir -p "$KDIR"
    tar xzf "$CACHE/$PKG.apk" -C "$KDIR" 2>/dev/null || true
    rm -f "$CACHE/$PKG.apk"
fi

# ---- 2. kernel -------------------------------------------------------
KV=$(ls "$KDIR"/lib/modules)
cp "$KDIR/boot/vmlinuz-virt" "$CACHE/vmlinuz-virt"
echo "==> kernel: $(du -h "$CACHE/vmlinuz-virt" | cut -f1)  (modules for $KV)"

# ---- 3. modules, decompressed, into the staging rootfs ---------------
DEST=$STAGE/lib/modules/$KV
rm -rf "$DEST"
for tree in $MODULE_TREES; do
    src="$KDIR/lib/modules/$KV/$tree"
    [ -d "$src" ] || continue
    ( cd "$KDIR/lib/modules/$KV" && find "$tree" -name '*.ko.gz' ) | while read -r rel; do
        out="$DEST/${rel%.gz}"
        mkdir -p "$(dirname "$out")"
        gzip -dc "$KDIR/lib/modules/$KV/$rel" > "$out"
    done
done

# a couple of modules live directly under kernel/fs/ rather than in a subdir
for rel in kernel/fs/mbcache.ko; do
    src="$KDIR/lib/modules/$KV/$rel.gz"
    [ -f "$src" ] || continue
    out="$DEST/$rel"
    mkdir -p "$(dirname "$out")"
    gzip -dc "$src" > "$out"
done

count=$(find "$DEST" -name '*.ko' | wc -l)
size=$(du -sm "$DEST" | cut -f1)
echo "==> modules: $count files, ${size} MB -> $DEST"
