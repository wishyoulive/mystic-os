#!/usr/bin/env bash
# ============================================================
#  tools/fetch-kernel.sh
#  Get a kernel + exactly the modules Mystic OS needs, and put
#  them where the image builder will find them.
#
#  usage:  tools/fetch-kernel.sh [virt|lts]
#          MYSTIC_KERNEL=lts tools/fetch-kernel.sh
#          tools/fetch-kernel.sh --hw        (same as lts)
#
#  TWO KERNELS, because they are not the same hardware story:
#
#    virt  linux-virt-<ver>  (44,585,823 byte .apk)
#          virtio disk/net, PS/2 input, bochs-drm framebuffer, HDA
#          playback.  THIS KERNEL SHIPS NO V4L2, NO uvcvideo AND NO
#          snd-usb-audio AT ALL - they are not in the package, so no
#          webcam or USB microphone can work on the default image.
#
#    lts   linux-lts-<ver>   (146,407,326 byte .apk)
#          everything virt has, plus drivers/media (uvcvideo + the
#          videobuf2 the V4L2 core needs), sound/usb (snd-usb-audio),
#          USB host + HID and the Wi-Fi stacks.  About 3x the package
#          and 5.3x the initramfs (84.5 MB vs 15.9 MB); ./image.sh --hw
#          says so loudly.
#
#  Why the kernel and its modules come from the same package: a module
#  built for a different kernel revision fails with "Invalid module
#  format", so the two can never be fetched separately.
# ============================================================
set -e
cd "$(dirname "$0")/.."

VERSION=6.18.63
BASE=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64
CACHE=build/cache
STAGE=stage

usage() {
    cat <<'USAGE'
usage: tools/fetch-kernel.sh [virt|lts]
       MYSTIC_KERNEL=lts tools/fetch-kernel.sh
       tools/fetch-kernel.sh --hw          (same as lts)

  virt  linux-virt (44,585,823 byte package): virtio, PS/2, bochs-drm,
        HDA playback. No V4L2, no uvcvideo, no snd-usb-audio at all.
  lts   linux-lts (146,407,326 byte package): adds drivers/media,
        sound/usb, USB host + HID and the Wi-Fi stacks.
USAGE
}

case "${1:-${MYSTIC_KERNEL:-virt}}" in
    --hw|lts|LTS|hw)        VARIANT=lts ;;
    --virt|virt|VIRT|"")    VARIANT=virt ;;
    -h|--help|help)         usage; exit 0 ;;
    *) echo "error: unknown kernel variant '${1}' (want virt or lts)" >&2; exit 2 ;;
esac

if [ "$VARIANT" = lts ]; then
    PKG=linux-lts-${VERSION}-r0
    KIMAGE=vmlinuz-lts
    # Whole subtrees, not a hand-picked list: cherry-picking individual .ko
    # files means chasing "unknown symbol" errors one build at a time. The
    # virt trees are here unchanged so the lts image boots exactly like the
    # default one, and the hardware trees are added on top:
    #   drivers/media  uvcvideo + videodev + media/mc + videobuf2
    #   sound/usb      snd-usb-audio   (covered by kernel/sound below)
    #   drivers/usb    usbcore, xhci/ehci/ohci/uhci, usbhid
    #   drivers/hid    hid + hid-generic and the device quirks
    #   net/wireless + net/mac80211 + drivers/net/wireless  Wi-Fi
    # Every path is checked against the extracted package; anything that
    # is not really there is skipped and reported, never invented.
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
kernel/sound
kernel/fs/nls
kernel/drivers/media
kernel/drivers/usb
kernel/drivers/hid
kernel/drivers/leds
kernel/net/wireless
kernel/net/mac80211
kernel/drivers/net/wireless
"
else
    PKG=linux-virt-${VERSION}-r0
    KIMAGE=vmlinuz-virt
    # Whole subtrees, not a hand-picked list. Cherry-picking individual .ko
    # files means chasing "unknown symbol" errors one build at a time; the
    # entire GPU, framebuffer and i2c trees are ~2 MB compressed, so we take
    # them all. kernel/drivers/usb + hid are here because virt DOES ship
    # usbcore/xhci/ehci/usbhid (a USB keyboard works); what it does not ship,
    # and cannot, is kernel/drivers/media (no uvcvideo) and kernel/sound/usb
    # (no snd-usb-audio) - so no webcam and no USB microphone on this kernel.
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
kernel/sound
kernel/fs/nls
kernel/drivers/usb
kernel/drivers/hid
"
fi
KDIR=$CACHE/$PKG

# The modules a webcam and a USB microphone actually need. These are the
# roots of the dependency closure that is resolved below from the kernel's
# own modules.dep - so the list of files that end up in the image is
# computed from the package, not from memory.
HW_ROOTS="
kernel/drivers/media/usb/uvc/uvcvideo
kernel/sound/usb/snd-usb-audio
kernel/drivers/hid/usbhid/usbhid
kernel/drivers/hid/hid-generic
kernel/net/wireless/cfg80211
kernel/net/mac80211/mac80211
"

mkdir -p "$CACHE"

# ---- 1. the package (44 MB virt / 146 MB lts, cached) ----------------
if [ ! -d "$KDIR" ]; then
    echo "==> downloading $PKG"
    curl -sL -o "$CACHE/$PKG.apk.part" "$BASE/$PKG.apk"
    # A half download must not leave a cache directory behind: the next run
    # would trust it and fail later with a confusing "no kernel" error.
    if ! tar tzf "$CACHE/$PKG.apk.part" >/dev/null 2>&1; then
        echo "error: $BASE/$PKG.apk is not a readable package (download failed?)" >&2
        rm -f "$CACHE/$PKG.apk.part"
        exit 1
    fi
    bytes=$(wc -c < "$CACHE/$PKG.apk.part")
    echo "==> $PKG: $bytes bytes"
    rm -rf "$KDIR"
    mkdir -p "$KDIR"
    tar xzf "$CACHE/$PKG.apk.part" -C "$KDIR"
    rm -f "$CACHE/$PKG.apk.part"
fi

[ -f "$KDIR/boot/$KIMAGE" ] || { echo "error: $PKG has no boot/$KIMAGE" >&2; exit 1; }

# ---- 2. kernel -------------------------------------------------------
KV=$(ls "$KDIR"/lib/modules)
cp "$KDIR/boot/$KIMAGE" "$CACHE/$KIMAGE"
echo "==> variant: $VARIANT   kernel: $KIMAGE $(du -h "$CACHE/$KIMAGE" | cut -f1)  (modules for $KV)"

# ---- 3. modules, decompressed, into the staging rootfs ---------------
# Each variant stages into its own /lib/modules/<kver>, so the two images
# can live side by side in stage/ and the running kernel picks its own by
# uname -r. tools/mkimage.py is told which one to pack.
DEST=$STAGE/lib/modules/$KV
rm -rf "$DEST"
MODSRC=$KDIR/lib/modules/$KV
for tree in $MODULE_TREES; do
    if [ ! -d "$MODSRC/$tree" ]; then
        echo "    $tree: not in $PKG (skipped)"
        continue
    fi
    n=0
    while read -r rel; do
        out="$DEST/${rel%.gz}"
        mkdir -p "$(dirname "$out")"
        gzip -dc "$MODSRC/$rel" > "$out"
        n=$((n + 1))
    done <<EOF
$( cd "$MODSRC" && find "$tree" -name '*.ko.gz' )
EOF
    bytes=$( cd "$DEST" && find "$tree" -name '*.ko' -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}' )
    printf '    %-28s %4s modules  %9s bytes\n' "$tree" "$n" "$bytes"
done

# a couple of modules live directly under kernel/fs/ rather than in a subdir
for rel in kernel/fs/mbcache.ko; do
    src="$MODSRC/$rel.gz"
    [ -f "$src" ] || continue
    out="$DEST/$rel"
    mkdir -p "$(dirname "$out")"
    gzip -dc "$src" > "$out"
done

# ---- 4. dependency closure of the webcam / microphone path -----------
# modules.dep ships inside the package, so this is the kernel's own answer
# to "what does uvcvideo need", not a guess. Every module the closure names
# is copied if a tree above did not already cover it; a name that does not
# exist in the package is reported as missing and never substituted.
closure_report() {
    dep=$MODSRC/modules.dep
    [ -f "$dep" ] || { echo "==> closure: no modules.dep in the package"; return 0; }
    roots=""
    for r in $HW_ROOTS; do
        if [ -f "$MODSRC/$r.ko.gz" ]; then
            roots="$roots $r"
        else
            echo "    root not in this kernel: $r.ko"
        fi
    done
    [ -n "$roots" ] || { echo "==> closure: none of the webcam/microphone modules exist in $PKG"; return 0; }

    # BFS over modules.dep: path (no .ko.gz) -> space separated deps
    awk -v roots="$roots" '
        {
            line = $0
            ci = index(line, ": ")
            if (ci > 0) { mod = substr(line, 1, ci - 1); deps = substr(line, ci + 2) }
            else        { mod = line; deps = "" }
            gsub(/^[ \t]+|[ \t]+$/, "", mod)
            sub(/\.ko\.gz$/, "", mod)
            if (!(mod in seen)) { seen[mod] = 1 }
            nd = split(deps, arr, " ")
            for (i = 1; i <= nd; i++) {
                d = arr[i]
                gsub(/^[ \t]+|[ \t]+$/, "", d)
                sub(/\.ko\.gz$/, "", d)
                if (d != "") adj[mod] = adj[mod] " " d
            }
        }
        END {
            n = split(roots, R, " ")
            for (i = 1; i <= n; i++) {
                if (R[i] == "") continue
                if (!(R[i] in seen)) continue        # root really absent
                if (R[i] in vis) continue
                vis[R[i]] = 1; q[++qt] = R[i]
            }
            while (qh < qt) {
                m = q[++qh]
                print m
                nd = split(adj[m], arr, " ")
                for (i = 1; i <= nd; i++) {
                    d = arr[i]
                    if (d == "" || (d in vis)) continue
                    vis[d] = 1; q[++qt] = d
                }
            }
        }
    ' "$dep" > "$CACHE/closure-$VARIANT.txt"

    total=0
    pulled=0
    missing=0
    while read -r m; do
        [ -n "$m" ] || continue
        out="$DEST/$m.ko"
        if [ ! -f "$out" ]; then
            if [ -f "$MODSRC/$m.ko.gz" ]; then
                mkdir -p "$(dirname "$out")"
                gzip -dc "$MODSRC/$m.ko.gz" > "$out"
                pulled=$((pulled + 1))
                how="pulled"
            else
                echo "    MISSING in $PKG: $m.ko"
                missing=$((missing + 1))
                continue
            fi
        else
            how="in tree"
        fi
        b=$(wc -c < "$out")
        total=$((total + b))
        printf '    %-52s %9s  %s\n' "$m.ko" "$b" "$how"
    done < "$CACHE/closure-$VARIANT.txt"
    nm=$(wc -l < "$CACHE/closure-$VARIANT.txt")
    echo "==> closure: $nm modules, $total bytes ($pulled pulled from outside the trees, $missing missing)"
}

echo "==> webcam + microphone closure (from $PKG/lib/modules/$KV/modules.dep)"
closure_report

count=$(find "$DEST" -name '*.ko' | wc -l)
bytes=$(find "$DEST" -name '*.ko' -printf '%s\n' | awk '{s+=$1} END{print s+0}')
mb=$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1e6}')
echo "==> modules: $count files, $mb MB unpacked ($bytes bytes) -> $DEST"

# ---- 5. say what this kernel can and cannot do, in one honest line ---
if [ -d "$DEST/kernel/drivers/media" ] && [ -f "$DEST/kernel/sound/usb/snd-usb-audio.ko" ]; then
    echo "==> hardware: uvcvideo + snd-usb-audio present -> webcam and USB microphone can work"
else
    echo "==> hardware: NO drivers/media and NO sound/usb in this kernel ->"
    echo "              a webcam and a USB microphone cannot work on this image;"
    echo "              use ./image.sh --hw (linux-lts) for that hardware"
fi
