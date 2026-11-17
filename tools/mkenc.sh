#!/usr/bin/env bash
# ============================================================
#  tools/mkenc.sh - the encryption half of Mystic OS
#
#  What this does, and why it is three commands instead of
#  "cryptsetup luksFormat":
#
#   1. stages the two kernel modules dm-crypt needs. They are
#      built as modules (CONFIG_BLK_DEV_DM=m, CONFIG_DM_CRYPT=m
#      in build/cache/linux-virt-*/boot/config-*) but
#      tools/fetch-kernel.sh does not copy kernel/drivers/md,
#      so without this step modprobe dm_crypt fails at boot.
#      aes and xts are built into the kernel: no crypto module
#      is required for aes-xts-plain64.
#
#   2. creates the boot half of the disk key:
#      stage/etc/mystic/luks.key, 32 bytes from /dev/urandom.
#      It is packed into the initramfs, NOT written to the disk.
#      The disk gets its own 32 random bytes on first boot, in
#      the MYSTICC1 header, so neither half alone decrypts
#      anything. docs/ENCRYPTION.md is explicit about what that
#      does and does not protect.
#
#   3. compiles stage/sbin/mystic-dmcrypt, a static musl binary
#      that speaks the dm-ioctl ABI itself. dmsetup would need
#      libdevmapper, libuuid and libblkid; cryptsetup would add
#      libcryptsetup, libjson-c and argon2. Three ioctls and
#      64 random bytes need none of that. The program is written
#      here rather than in a second file because this is the tool
#      that builds it - what you read is what runs.
#
#  Usage:
#    ./tools/mkenc.sh              stage modules + key + helper, then verify
#    ./tools/mkenc.sh status       what the next boot will do with build/disk.img
#    ./tools/mkenc.sh test         boot-path simulation (fake /dev, real init)
#    ./tools/mkenc.sh key          print the current key fingerprint
#    ./tools/mkenc.sh new-key      replace the boot half (old disks stop opening)
#    ./tools/mkenc.sh reset-disk   move build/disk.img aside so the next boot
#                                  creates an encrypted one
#
#  Run it AFTER tools/fetch-kernel.sh: that script wipes
#  stage/lib/modules/<version> and would delete the modules again.
# ============================================================
set -eu
cd "$(dirname "$0")/.."

CACHE=${MYSTIC_CACHE:-build/cache}
STAGE=stage
WORK=build/mkenc
ZIG=${ZIG:-zig}
MODULES="
kernel/drivers/md/dm-mod.ko
kernel/security/keys/encrypted-keys/encrypted-keys.ko
kernel/drivers/md/dm-crypt.ko
"

say()  { printf '==> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---- 0. which kernel are we staging for? -----------------------------
KVER=$(ls "$STAGE"/lib/modules 2>/dev/null | head -1 || true)
PKGDIR=""
for d in "$CACHE"/linux-virt-*; do
    [ -d "$d/lib/modules" ] && PKGDIR=$d && break
done
if [ -z "$KVER" ]; then
    [ -n "$PKGDIR" ] || die "no kernel package in $CACHE - run ./tools/fetch-kernel.sh first"
    KVER=$(ls "$PKGDIR"/lib/modules | head -1)
fi
KO_SRC=${PKGDIR:+$PKGDIR/lib/modules/$KVER}
KO_DST=$STAGE/lib/modules/$KVER

# ====================================================================
#  1. kernel modules
# ====================================================================
stage_modules() {
    [ -n "$KO_SRC" ] || die "no unpacked kernel package in $CACHE - run ./tools/fetch-kernel.sh first"
    local n=0 rel src out
    for rel in $MODULES; do
        src=$KO_SRC/$rel.gz
        out=$KO_DST/$rel
        if [ ! -f "$out" ]; then
            [ -f "$src" ] || die "missing $src (kernel package is incomplete)"
            mkdir -p "$(dirname "$out")"
            gzip -dc "$src" > "$out" || die "cannot decompress $src"
        fi
        n=$((n + 1))
    done
    say "modules: $n staged in $KO_DST/kernel/{drivers/md,security/keys}"
}

# ====================================================================
#  2. the helper, compiled from the source below
# ====================================================================
emit_source() {
    mkdir -p "$WORK"
    cat > "$WORK/mystic-dmcrypt.c" <<'MYSTIC_DMCRYPT_C_EOF'
/* ============================================================
 * mystic-dmcrypt - the smallest thing that can open a dm-crypt
 * device, with no libdevmapper, no libcryptsetup and no libuuid.
 *
 * cryptsetup pulls in libcryptsetup, libdevmapper, libuuid, libblkid,
 * libjson-c and argon2; dmsetup alone still needs libdevmapper. This
 * program talks to /dev/mapper/control directly: three ioctls
 * (DM_DEV_CREATE, DM_TABLE_LOAD, DM_DEV_SUSPEND) are enough, because
 * a dm-crypt target with a plain 64-byte key needs no metadata
 * parsing at all. The on-disk metadata is our own 4 KiB header
 * (magic MYSTICC1) described in docs/ENCRYPTION.md.
 *
 *   keygen <file>                  32 bytes from /dev/urandom, 0600
 *   fingerprint <file>             first 16 hex of SHA-256(file)
 *   probe <dev>                    exit 0 "crypt <ver> <fp>", 10 "blank",
 *                                  11 "foreign", 1 error
 *   init <dev> <boot-key>          write a fresh header (blank device only)
 *   attach <name> <dev> <boot-key> create + load + resume the mapping
 *   detach <name>                  remove the mapping
 *   status <name>                  exit 0 if the mapping exists
 *   selftest                       hash / table / header checks
 *
 * Build (target, static musl):
 *   zig cc -target x86_64-linux-musl -Os -static -Wall -Wextra \
 *          mystic-dmcrypt.c -o mystic-dmcrypt
 * Build (host test, no kernel headers, no ioctls):
 *   zig cc -DMYSTIC_HOSTTEST=1 mystic-dmcrypt.c -o mystic-dmcrypt.exe
 * ============================================================ */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(MYSTIC_HOSTTEST) && defined(_WIN32)
#define _CRT_RAND_S          /* rand_s(): Windows has no /dev/urandom */
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(MYSTIC_HOSTTEST) && defined(_WIN32)
/* the host-test build exists to run the hash, the table string and the header
 * codec on a workstation; it needs no ioctls, only the same file primitives */
#include <io.h>
#define fsync _commit
static ssize_t htest_pread(int fd, void *buf, size_t n, long long off)
{
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)n);
}
#define pread htest_pread
#else
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/dm-ioctl.h>
/* BLKGETSIZE64 is _IOR(0x12,114,size_t); defined here so this file needs
 * no <linux/fs.h> (which collides with <sys/mount.h> in some libcs). */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#endif

/* ------------------------------------------------------------
 * on-disk header.  Little-endian byte offsets, not a struct:
 * a struct would make the format depend on the compiler's
 * padding rules, and this header is the one thing that must
 * never change shape.
 *
 *   0x00  8   "MYSTICC1"
 *   0x08  4   version                    (1)
 *   0x0c  4   data offset, 512B sectors  (2048 = 1 MiB)
 *   0x10  4   cipher id                  (1 = aes-xts-plain64)
 *   0x14  4   key bytes                  (64)
 *   0x18 32   disk half of the key material
 *   0x38 32   SHA-256(boot half || disk half)
 *   0x58 ...  zero
 * ------------------------------------------------------------ */
#define HDR_MAGIC "MYSTICC1"
#define HDR_VERSION 1u
#define HDR_SECTOR 512u
#define HDR_OFFSET_SECTORS 2048u
#define HDR_CIPHER_XTS 1u
#define HDR_BYTES 4096u
#define O_MAGIC 0x00
#define O_VERSION 0x08
#define O_OFFSET 0x0c
#define O_CIPHER 0x10
#define O_KEYBYTES 0x14
#define O_DISKHALF 0x18
#define O_FINGERPRINT 0x38
#define HALF 32
#define KEY_MATERIAL (HALF * 2)
#define FPRINT 32

#define EXIT_NOHDR 10       /* probe: nothing of ours here, the device is blank */
#define EXIT_FOREIGN 11     /* probe: nothing of ours here, but something is */
#define DM_MAPPER_DIR "/dev/mapper"
#define DM_CONTROL DM_MAPPER_DIR "/control"

/* In the host-test build the kernel ABI is replaced by the same layout so the
 * header codec, the table string and the key handling can be executed on a
 * machine with no device mapper. The numbers are the frozen DM v4 ABI. */
#ifdef MYSTIC_HOSTTEST
struct dm_ioctl {
    uint32_t version[3];
    uint32_t data_size;
    uint32_t data_start;
    uint32_t target_count;
    int32_t open_count;
    uint32_t flags;
    uint32_t event_nr;
    uint32_t padding;
    uint64_t dev;
    char name[128];
    char uuid[129];
    char data[7];
};
struct dm_target_spec {
    uint64_t sector_start;
    uint64_t length;
    int32_t status;
    uint32_t next;
    char target_type[16];
};
#define DM_IOCTL 0xfd
#define DM_DEV_CREATE 0xc138fd00UL
#define DM_DEV_REMOVE 0xc138fd01UL
#define DM_DEV_SUSPEND 0xc138fd03UL
#define DM_DEV_STATUS 0xc138fd04UL
#define DM_TABLE_LOAD 0xc138fd09UL
#endif

_Static_assert(sizeof(struct dm_target_spec) == 40, "dm_target_spec is 40 bytes in DM v4");
_Static_assert(sizeof(struct dm_ioctl) == 312, "dm_ioctl is 312 bytes in DM v4");

static const char *prog = "mystic-dmcrypt";

static int die(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "%s: ", prog);
    fprintf(stderr, fmt, a ? a : "", b ? b : "");
    fputc('\n', stderr);
    return 1;
}

static int die_errno(const char *what, const char *arg)
{
    int e = errno;
    fprintf(stderr, "%s: %s%s%s: %s\n", prog, what, arg ? " " : "", arg ? arg : "",
            strerror(e));
    return 1;
}

/* ---------------- SHA-256 (FIPS 180-4) ---------------- */

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t fill;
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(struct sha256 *s, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(struct sha256 *s, const void *data, size_t n)
{
    const unsigned char *p = data;
    s->len += n;
    if (s->fill) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
    while (n >= 64) {
        sha256_block(s, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(s->buf, p, n);
        s->fill = n;
    }
}

static void sha256_final(struct sha256 *s, unsigned char out[FPRINT])
{
    uint64_t bits = s->len * 8u;
    unsigned char pad[72];
    size_t padlen = (s->fill < 56) ? 56 - s->fill : 120 - s->fill;
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(s, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

static void sha256_buf(const void *data, size_t n, unsigned char out[FPRINT])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

/* ---------------- little helpers ---------------- */

static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = 0;
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static int read_exact(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

/* 32 bytes of kernel randomness. On the target that is /dev/urandom; the
 * host-test build only needs unpredictable bytes for its own temporary files. */
static int urandom_read(unsigned char *buf, size_t n)
{
#if defined(MYSTIC_HOSTTEST) && defined(_WIN32)
    size_t i;
    for (i = 0; i < n; i += 4) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) { errno = EIO; return -1; }
        memcpy(buf + i, &v, n - i < 4 ? n - i : 4);
    }
    wipe(buf, 0);
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    int r;
    if (fd < 0) return -1;
    r = read_exact(fd, buf, n);
    close(fd);
    return r;
#endif
}

/* 32 bytes, exactly. A longer file is refused rather than truncated: a
 * passphrase typed into a file must not silently become 32 bytes of it. */
static int key_read(const char *path, unsigned char half[HALF])
{
    int fd = open(path, O_RDONLY);
    unsigned char extra;
    ssize_t n;
    if (fd < 0) return die_errno("cannot open key", path);
    if (read_exact(fd, half, HALF) < 0) {
        close(fd);
        return die_errno("cannot read 32 bytes from key", path);
    }
    n = read(fd, &extra, 1);
    close(fd);
    if (n != 0) {
        wipe(half, HALF);
        return die("%s is not exactly 32 bytes (it must be the raw key half, "
                   "not a passphrase)", path, NULL);
    }
    return 0;
}

static int keygen(const char *path)
{
    unsigned char half[HALF];
    int fd;
    struct stat st;
    if (stat(path, &st) == 0)
        return die("%s already exists; refusing to overwrite a key that a disk "
                   "may be encrypted with", path, NULL);
    if (urandom_read(half, HALF) < 0) return die_errno("cannot read", "/dev/urandom");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        wipe(half, HALF);
        return die_errno("cannot create key", path);
    }
    if (write_all(fd, half, HALF) < 0 || fsync(fd) < 0) {
        close(fd);
        wipe(half, HALF);
        return die_errno("cannot write key", path);
    }
    close(fd);
    wipe(half, HALF);
    return 0;
}

/* ---------------- header codec ---------------- */

enum dev_state { DEV_ERROR = -1, DEV_BLANK = 1, DEV_FOREIGN = 2, DEV_CRYPT = 0 };

struct disk_info {
    enum dev_state state;
    uint32_t version;
    uint32_t offset_sectors;
    uint32_t key_bytes;
    unsigned char disk_half[HALF];
    unsigned char fingerprint[FPRINT];
};

static void header_build(unsigned char hdr[HDR_BYTES], const unsigned char material[KEY_MATERIAL])
{
    memset(hdr, 0, HDR_BYTES);
    memcpy(hdr + O_MAGIC, HDR_MAGIC, 8);
    put_le32(hdr + O_VERSION, HDR_VERSION);
    put_le32(hdr + O_OFFSET, HDR_OFFSET_SECTORS);
    put_le32(hdr + O_CIPHER, HDR_CIPHER_XTS);
    put_le32(hdr + O_KEYBYTES, KEY_MATERIAL);
    memcpy(hdr + O_DISKHALF, material + HALF, HALF);
    sha256_buf(material, KEY_MATERIAL, hdr + O_FINGERPRINT);
}

/* returns DEV_CRYPT and fills di, or DEV_BLANK / DEV_FOREIGN / DEV_ERROR */
static int probe_dev(const char *dev, struct disk_info *di)
{
    unsigned char buf[HDR_BYTES];
    ssize_t n;
    size_t i;
    int zero = 1;
    int fd = open(dev, O_RDONLY);
    memset(di, 0, sizeof *di);
    if (fd < 0) return die_errno("cannot open device", dev);
    n = pread(fd, buf, sizeof buf, 0);
    if (n < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return die_errno("cannot read the first 4 KiB of", dev);
    }
    close(fd);
    if (n >= 8 && memcmp(buf, HDR_MAGIC, 8) == 0) {
        if (n < (ssize_t)(O_FINGERPRINT + FPRINT)) {
            fprintf(stderr, "%s: %s says MYSTICC1 but is too short to hold the header\n",
                    prog, dev);
            return DEV_ERROR;
        }
        di->version = get_le32(buf + O_VERSION);
        di->offset_sectors = get_le32(buf + O_OFFSET);
        di->key_bytes = get_le32(buf + O_KEYBYTES);
        memcpy(di->disk_half, buf + O_DISKHALF, HALF);
        memcpy(di->fingerprint, buf + O_FINGERPRINT, FPRINT);
        if (di->version != HDR_VERSION || di->key_bytes != KEY_MATERIAL ||
            di->offset_sectors < 8 || (di->offset_sectors % 8) != 0) {
            fprintf(stderr, "%s: %s header is not version %u / 64-byte key / aligned offset\n",
                    prog, dev, HDR_VERSION);
            return DEV_ERROR;
        }
        di->state = DEV_CRYPT;
        return DEV_CRYPT;
    }
    /* ext2/3/4 superblock magic, at byte 0x438 of the device. Checked before
     * the all-zero test so a real filesystem is never mistaken for blank. */
    if (n >= 0x43a && buf[0x438] == 0x53 && buf[0x439] == 0xef) {
        di->state = DEV_FOREIGN;
        return DEV_FOREIGN;
    }
    for (i = 0; i < (size_t)n; i++)
        if (buf[i]) { zero = 0; break; }
    if (n == 0 || (zero && (size_t)n == HDR_BYTES)) {
        di->state = DEV_BLANK;
        return DEV_BLANK;
    }
    di->state = DEV_FOREIGN;
    return DEV_FOREIGN;
}

/* ---------------- dm-ioctl ---------------- */

#ifndef MYSTIC_HOSTTEST
struct dm_buf {
    struct dm_ioctl dmi;
    unsigned char pad[4096 - sizeof(struct dm_ioctl)];
};

static void dm_prepare(struct dm_buf *b, const char *name)
{
    memset(b, 0, sizeof *b);
    b->dmi.version[0] = 4;      /* DM_VERSION_MAJOR - the only field the kernel */
    b->dmi.version[1] = 0;      /* checks. A lower minor is always accepted, so  */
    b->dmi.version[2] = 0;      /* this stays compatible with every DM v4 kernel. */
    b->dmi.data_size = sizeof *b;
    b->dmi.data_start = (uint32_t)sizeof(struct dm_ioctl);
    snprintf(b->dmi.name, sizeof b->dmi.name, "%s", name);
}

static int dm_simple(int fd, unsigned long cmd, struct dm_ioctl *dmi, const char *what)
{
    if (ioctl(fd, cmd, dmi) < 0) {
        int e = errno;
        fprintf(stderr, "%s: dm %s failed: %s\n", prog, what, strerror(e));
        errno = e;
        return -1;
    }
    return 0;
}
#endif

/* "0 <sectors> crypt aes-xts-plain64 <key-hex> 0 <dev> <offset>" */
static int table_build(char *out, size_t cap, unsigned long long sectors, const char *keyhex,
                       const char *dev, uint32_t offset)
{
    int n = snprintf(out, cap, "0 %llu crypt aes-xts-plain64 %s 0 %s %u", sectors, keyhex, dev,
                     (unsigned)offset);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return n;
}

static int dev_size_bytes(const char *dev, unsigned long long *bytes)
{
#ifdef MYSTIC_HOSTTEST
    struct stat st;
    if (stat(dev, &st) < 0) return die_errno("cannot stat device", dev);
    if (!S_ISREG(st.st_mode))
        return die("%s is not a regular file in the host-test build", dev, NULL);
    *bytes = (unsigned long long)st.st_size;
    return 0;
#else
    size_t sz = 0;
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return die_errno("cannot open device", dev);
    if (ioctl(fd, BLKGETSIZE64, &sz) == 0) {
        close(fd);
        *bytes = sz;
        return 0;
    }
    /* fall back to sysfs before giving up: /sys/class/block/vda/size is in
     * 512-byte sectors and exists even when the ioctl is unavailable. */
    {
        char path[256], buf[64];
        const char *base = strrchr(dev, '/');
        ssize_t n;
        int n2;
        close(fd);
        base = base ? base + 1 : dev;
        n2 = snprintf(path, sizeof path, "/sys/class/block/%s/size", base);
        if (n2 < 0 || (size_t)n2 >= sizeof path) return die("device path is too long", NULL, NULL);
        fd = open(path, O_RDONLY);
        if (fd < 0) return die_errno("cannot size device", dev);
        n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) return die_errno("cannot read", path);
        buf[n] = 0;
        *bytes = strtoull(buf, NULL, 10) * 512ULL;
        return 0;
    }
#endif
}

static int cmd_attach(const char *name, const char *dev, const char *keypath, int print_table)
{
    struct disk_info di;
    unsigned char boot[HALF];
    unsigned char material[KEY_MATERIAL];
    unsigned char want[FPRINT];
    char keyhex[KEY_MATERIAL * 2 + 1];
    char table[512];
    unsigned long long bytes = 0, sectors;
    int rc = 1;

    if (strchr(name, '/') || !*name || strlen(name) >= 64)
        return die("%s is not a usable mapping name", name, NULL);
    if (probe_dev(dev, &di) != DEV_CRYPT)
        return die("%s has no MYSTICC1 header (run 'init' on a blank disk first)", dev, NULL);
    if (key_read(keypath, boot) != 0) return 1;
    memcpy(material, boot, HALF);
    memcpy(material + HALF, di.disk_half, HALF);
    sha256_buf(material, KEY_MATERIAL, want);
    if (memcmp(want, di.fingerprint, FPRINT) != 0) {
        char a[17], b[17];
        to_hex(want, 8, a);
        to_hex(di.fingerprint, 8, b);
        wipe(material, sizeof material);
        wipe(boot, sizeof boot);
        return die("key does not match this disk (header %s, key %s) - the disk is "
                   "left untouched", b, a);
    }
    if (dev_size_bytes(dev, &bytes) < 0) goto out;
    if (bytes / HDR_SECTOR <= di.offset_sectors) {
        die("%s is too small for the header area", dev, NULL);
        goto out;
    }
    sectors = bytes / HDR_SECTOR - di.offset_sectors;
    to_hex(material, KEY_MATERIAL, keyhex);
    if (table_build(table, sizeof table, sectors, keyhex, dev, di.offset_sectors) < 0) {
        die("cannot build the dm table (device path too long?)", NULL, NULL);
        goto out;
    }
    wipe(keyhex, sizeof keyhex);
    wipe(material, sizeof material);
    wipe(boot, sizeof boot);

    if (print_table) {
        /* tools/mkenc.sh uses this to show the shape of the table without the
         * key material. */
        char shape[320];
        int n = snprintf(shape, sizeof shape,
                         "0 %llu crypt aes-xts-plain64 <64-byte key> 0 %s %u", sectors, dev,
                         (unsigned)di.offset_sectors);
        if (n > 0 && (size_t)n < sizeof shape) printf("%s\n", shape);
        return 0;
    }

#ifdef MYSTIC_HOSTTEST
    /* the shape, not the key: this line ends up in a boot log */
    printf("host-test: mapping %s from 0 %llu crypt aes-xts-plain64 "
           "<64-byte key> 0 %s %u\n",
           name, sectors, dev, (unsigned)di.offset_sectors);
    return 0;
#else
    {
        struct dm_buf b;
        struct dm_target_spec *spec;
        char *params;
        char node[128];
        size_t plen;
        int cfd;
        unsigned maj, min;

        cfd = open(DM_CONTROL, O_RDWR);
        if (cfd < 0) {
            die_errno("cannot open " DM_CONTROL " (is dm_mod loaded?)", NULL);
            return 1;
        }
        dm_prepare(&b, name);
        if (ioctl(cfd, DM_DEV_STATUS, &b.dmi) == 0) {
            printf("already active\n");
            close(cfd);
            return 0;
        }
        dm_prepare(&b, name);
        if (dm_simple(cfd, DM_DEV_CREATE, &b.dmi, "DM_DEV_CREATE") < 0) {
            close(cfd);
            return 1;
        }
        dm_prepare(&b, name);
        b.dmi.target_count = 1;
        spec = (struct dm_target_spec *)((unsigned char *)&b + b.dmi.data_start);
        spec->sector_start = 0;
        spec->length = sectors;
        spec->status = 0;
        snprintf(spec->target_type, sizeof spec->target_type, "crypt");
        params = (char *)(spec + 1);
        plen = strlen(table) + 1;
        if (plen > sizeof b.pad - sizeof *spec) {
            die("dm table is too long for the ioctl buffer", NULL, NULL);
            dm_prepare(&b, name);
            dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE (cleanup)");
            close(cfd);
            return 1;
        }
        memcpy(params, table, plen);
        spec->next = (uint32_t)((sizeof *spec + plen + 7) & ~(size_t)7);
        b.dmi.data_size = b.dmi.data_start + spec->next;
        if (dm_simple(cfd, DM_TABLE_LOAD, &b.dmi, "DM_TABLE_LOAD") < 0) {
            dm_prepare(&b, name);
            dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE (cleanup)");
            close(cfd);
            return 1;
        }
        dm_prepare(&b, name);
        if (dm_simple(cfd, DM_DEV_SUSPEND, &b.dmi, "DM_DEV_SUSPEND (resume)") < 0) {
            dm_prepare(&b, name);
            dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE (cleanup)");
            close(cfd);
            return 1;
        }
        /* the kernel wrote the real major:minor into dmi.dev on the way out */
        maj = (unsigned)((b.dmi.dev >> 8) & 0xfffu);
        min = (unsigned)(b.dmi.dev & 0xffu) | (unsigned)((b.dmi.dev >> 12) & 0xfff00u);
        close(cfd);
        snprintf(node, sizeof node, "%s/%s", DM_MAPPER_DIR, name);
        if (access(node, F_OK) != 0) {
            /* devtmpfs normally creates this node; if it did not, make it. */
            if (mknod(node, S_IFBLK | 0600, makedev(maj, min)) < 0) {
                die_errno("mapping is live but its device node could not be created", node);
                return 1;
            }
        }
        printf("active %s %u:%u\n", node, maj, min);
    }
    return 0;
#endif
out:
    wipe(material, sizeof material);
    wipe(boot, sizeof boot);
    wipe(keyhex, sizeof keyhex);
    return rc;
}

/* ---------------- selftest ---------------- */

static int cmd_selftest(void)
{
    static const struct {
        const char *msg;
        const char *want;
    } vec[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    int fails = 0, i;
    char hex[FPRINT * 2 + 1];
    unsigned char d[FPRINT];
    const char *tmpdir = (access("/tmp", W_OK) == 0) ? "/tmp" : ".";

    for (i = 0; i < 3; i++) {
        sha256_buf(vec[i].msg, strlen(vec[i].msg), d);
        to_hex(d, FPRINT, hex);
        printf("%s sha256(\"%s\")\n", strcmp(hex, vec[i].want) ? "FAIL" : "ok  ",
               *vec[i].msg ? vec[i].msg : "");
        if (strcmp(hex, vec[i].want)) fails++;
    }
    {   /* 1,000,000 x 'a' - the classic long-message vector */
        struct sha256 s;
        unsigned char chunk[1000];
        memset(chunk, 'a', sizeof chunk);
        sha256_init(&s);
        for (i = 0; i < 1000; i++) sha256_update(&s, chunk, sizeof chunk);
        sha256_final(&s, d);
        to_hex(d, FPRINT, hex);
        printf("%s sha256(1,000,000 x 'a')\n",
               strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
                   ? "FAIL" : "ok  ");
        if (strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"))
            fails++;
    }
    {   /* hex + table string for a known 64-byte material on a 2 MiB device */
        unsigned char m[KEY_MATERIAL];
        char keyhex[KEY_MATERIAL * 2 + 1];
        char table[512];
        const char *wanthex =
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
        const char *wanttable =
            "0 2048 crypt aes-xts-plain64 "
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
            " 0 /dev/fake0 2048";
        for (i = 0; i < KEY_MATERIAL; i++) m[i] = (unsigned char)i;
        to_hex(m, KEY_MATERIAL, keyhex);
        printf("%s hex(64 bytes)\n", strcmp(keyhex, wanthex) ? "FAIL" : "ok  ");
        if (strcmp(keyhex, wanthex)) fails++;
        table_build(table, sizeof table, 2048, keyhex, "/dev/fake0", 2048);
        printf("%s dm table, 2 MiB device -> 2048 data sectors at offset 2048\n",
               strcmp(table, wanttable) ? "FAIL" : "ok  ");
        if (strcmp(table, wanttable)) fails++;
        /* 64 MiB, the size image.sh creates by default */
        table_build(table, sizeof table, 131072 - 2048, keyhex, "/dev/vda", 2048);
        printf("%s dm table, 64 MiB device -> 129024 data sectors\n",
               strstr(table, "0 129024 crypt aes-xts-plain64 ") == table ? "ok  " : "FAIL");
        if (strstr(table, "0 129024 crypt aes-xts-plain64 ") != table) fails++;
    }
    {   /* header round-trip through a real file, then the blank check */
        char disk[512];
        char keyf[512];
        unsigned char m[KEY_MATERIAL], hdr[HDR_BYTES];
        struct disk_info di;
        int fd;
        int ok = 1;
        snprintf(disk, sizeof disk, "%s/mystic-selftest-disk-XXXXXX", tmpdir);
        snprintf(keyf, sizeof keyf, "%s/mystic-selftest-key-XXXXXX", tmpdir);
        fd = mkstemp(disk);
        if (fd < 0) {
            printf("FAIL  cannot create %s: %s\n", disk, strerror(errno));
            fails++;
        } else {
            for (i = 0; i < KEY_MATERIAL; i++) m[i] = (unsigned char)(i * 7);
            header_build(hdr, m);
            if (write_all(fd, hdr, sizeof hdr) < 0 || ftruncate(fd, 1024 * 1024) < 0 ||
                fsync(fd) < 0)
                ok = 0;
            close(fd);
            if (ok && probe_dev(disk, &di) == DEV_CRYPT &&
                memcmp(di.disk_half, m + HALF, HALF) == 0 &&
                memcmp(di.fingerprint, hdr + O_FINGERPRINT, FPRINT) == 0)
                printf("ok   probe() finds the header and reads the disk half back\n");
            else {
                printf("FAIL  probe() did not read the header back\n");
                fails++;
            }
            if (ok && probe_dev(disk, &di) == DEV_CRYPT && di.offset_sectors == HDR_OFFSET_SECTORS &&
                di.key_bytes == KEY_MATERIAL && di.version == HDR_VERSION)
                printf("ok   header fields: version %u, offset %u sectors, key %u bytes\n",
                       (unsigned)di.version, (unsigned)di.offset_sectors,
                       (unsigned)di.key_bytes);
            else {
                printf("FAIL  header fields are wrong\n");
                fails++;
            }
            unlink(disk);
        }
        fd = mkstemp(disk);
        if (fd >= 0) {
            /* an all-zero first 4 KiB is "blank" - that is the first-boot case */
            if (ftruncate(fd, 4096) == 0) {
                close(fd);
                if (probe_dev(disk, &di) == DEV_BLANK)
                    printf("ok   an all-zero 4 KiB device is blank\n");
                else {
                    printf("FAIL  an all-zero device was not reported blank\n");
                    fails++;
                }
            } else close(fd);
            unlink(disk);
        }
        fd = mkstemp(disk);
        if (fd >= 0) {
            /* an ext4 superblock magic at 0x438 must NEVER read as blank */
            unsigned char sb[4096];
            memset(sb, 0, sizeof sb);
            sb[0x438] = 0x53;
            sb[0x439] = 0xef;
            if (write_all(fd, sb, sizeof sb) == 0) {
                close(fd);
                if (probe_dev(disk, &di) == DEV_FOREIGN)
                    printf("ok   an ext4 superblock magic reads as foreign, not blank\n");
                else {
                    printf("FAIL  an ext4 magic was not reported foreign\n");
                    fails++;
                }
            } else close(fd);
            unlink(disk);
        }
        fd = mkstemp(keyf);
        if (fd >= 0) {
            static const unsigned char k32[HALF] = {
                0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
            unsigned char got[HALF];
            if (write_all(fd, k32, HALF) == 0) {
                close(fd);
                if (key_read(keyf, got) == 0 && memcmp(got, k32, HALF) == 0)
                    printf("ok   key_read() accepts exactly 32 bytes\n");
                else {
                    printf("FAIL  key_read() rejected a 32-byte key\n");
                    fails++;
                }
                /* 33 bytes must be refused: a passphrase is not a key half */
                fd = open(keyf, O_WRONLY | O_APPEND);
                if (fd >= 0) {
                    unsigned char one = 'x';
                    (void)write_all(fd, &one, 1);
                    close(fd);
                }
                if (key_read(keyf, got) != 0)
                    printf("ok   key_read() refuses a 33-byte file\n");
                else {
                    printf("FAIL  key_read() accepted a 33-byte file\n");
                    fails++;
                }
            } else close(fd);
            unlink(keyf);
        }
    }
    printf("%s %d check(s) failed\n", fails ? "FAIL" : "ok  ", fails);
    return fails ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: %s <command>\n"
            "  keygen <file>                     32 random bytes, mode 0600\n"
            "  fingerprint <file>                first 16 hex of SHA-256(file)\n"
            "  probe <dev>                       crypt | blank | foreign\n"
            "  init <dev> <key-file>             write the MYSTICC1 header (blank disk only)\n"
            "  attach <name> <dev> <key-file>    create + load + resume the mapping\n"
            "  detach <name>                     remove the mapping\n"
            "  status <name>                     exit 0 if the mapping exists\n"
            "  selftest                          hash / table / header checks\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *cmd;
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep selftest lines in order */
#if defined(MYSTIC_HOSTTEST) && defined(_WIN32)
    _set_fmode(_O_BINARY);   /* Windows text mode turns 0x1a into end-of-file */
#endif
    if (argc < 2) {
        usage();
        return 2;
    }
    cmd = argv[1];
    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "keygen") && argc == 3) return keygen(argv[2]);
    if (!strcmp(cmd, "fingerprint") && argc == 3) {
        unsigned char half[HALF], d[FPRINT];
        char hex[17];
        if (key_read(argv[2], half) != 0) return 1;
        sha256_buf(half, HALF, d);
        wipe(half, sizeof half);
        to_hex(d, 8, hex);
        printf("%s\n", hex);
        return 0;
    }
    if (!strcmp(cmd, "probe") && argc == 3) {
        struct disk_info di;
        int st = probe_dev(argv[2], &di);
        char hex[FPRINT * 2 + 1];
        if (st == DEV_ERROR) return 1;
        if (st == DEV_BLANK) {
            printf("blank\n");
            return EXIT_NOHDR;
        }
        if (st == DEV_FOREIGN) {
            printf("foreign\n");
            return EXIT_FOREIGN;
        }
        to_hex(di.fingerprint, 8, hex);
        printf("crypt %u %s\n", (unsigned)di.version, hex);
        return 0;
    }
    if (!strcmp(cmd, "init") && argc == 4) {
        struct disk_info di;
        unsigned char boot[HALF], material[KEY_MATERIAL], hdr[HDR_BYTES];
        char hex[17];
        int st = probe_dev(argv[2], &di);
        int fd;
        if (st == DEV_ERROR) return 1;
        if (st != DEV_BLANK)
            return die("%s already holds %s; init only writes to a blank device", argv[2],
                       st == DEV_CRYPT ? "an encrypted volume" : "a filesystem");
        if (key_read(argv[3], boot) != 0) return 1;
        memcpy(material, boot, HALF);
        if (urandom_read(material + HALF, HALF) < 0) {
            wipe(boot, sizeof boot);
            return die_errno("cannot read", "/dev/urandom");
        }
        header_build(hdr, material);
        fd = open(argv[2], O_WRONLY);
        if (fd < 0) {
            wipe(material, sizeof material);
            wipe(boot, sizeof boot);
            return die_errno("cannot open device for writing", argv[2]);
        }
        if (write_all(fd, hdr, sizeof hdr) < 0 || fsync(fd) < 0) {
            int e = errno;
            close(fd);
            wipe(material, sizeof material);
            wipe(boot, sizeof boot);
            errno = e;
            return die_errno("cannot write the header to", argv[2]);
        }
        close(fd);
        {
            unsigned char d[FPRINT];
            sha256_buf(material, KEY_MATERIAL, d);
            to_hex(d, 8, hex);
        }
        wipe(material, sizeof material);
        wipe(boot, sizeof boot);
        printf("initialised %u-byte header, data at sector %u, fingerprint %s\n",
               HDR_BYTES, HDR_OFFSET_SECTORS, hex);
        return 0;
    }
    if (!strcmp(cmd, "attach") && (argc == 5 || argc == 6)) {
        return cmd_attach(argv[2], argv[3], argv[4], argc == 6 && !strcmp(argv[5], "--table"));
    }
    if (!strcmp(cmd, "detach") && argc == 3) {
#ifdef MYSTIC_HOSTTEST
        (void)cmd;
        printf("host-test: detach needs a running device mapper\n");
        return 0;
#else
        struct dm_buf b;
        int cfd = open(DM_CONTROL, O_RDWR);
        if (cfd < 0) return die_errno("cannot open " DM_CONTROL " (is dm_mod loaded?)", NULL);
        dm_prepare(&b, argv[2]);
        if (dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE") < 0) {
            close(cfd);
            return 1;
        }
        close(cfd);
        printf("removed %s/%s\n", DM_MAPPER_DIR, argv[2]);
        return 0;
#endif
    }
    if (!strcmp(cmd, "status") && argc == 3) {
#ifdef MYSTIC_HOSTTEST
        printf("host-test: status needs a running device mapper\n");
        return 0;
#else
        struct dm_buf b;
        int cfd = open(DM_CONTROL, O_RDWR);
        if (cfd < 0) return die_errno("cannot open " DM_CONTROL " (is dm_mod loaded?)", NULL);
        dm_prepare(&b, argv[2]);
        if (ioctl(cfd, DM_DEV_STATUS, &b.dmi) < 0) {
            close(cfd);
            return 1;
        }
        close(cfd);
        printf("active %s/%s\n", DM_MAPPER_DIR, argv[2]);
        return 0;
#endif
    }
    usage();
    return 2;
}
MYSTIC_DMCRYPT_C_EOF
}

build_helper() {
    emit_source
    mkdir -p "$STAGE/sbin"
    say "helper: compiling stage/sbin/mystic-dmcrypt (static musl)"
    # The exact flags the rest of the project builds with. -static is the
    # point: the initramfs has one libc and no shared library search path
    # worth debugging, and this binary runs before anything else.
    $ZIG cc -target x86_64-linux-musl -Os -static -Wall -Wextra \
        -Wno-unused-parameter "$WORK/mystic-dmcrypt.c" \
        -o "$STAGE/sbin/mystic-dmcrypt" || die "compiling the helper failed"
    chmod +x "$STAGE/sbin/mystic-dmcrypt" 2>/dev/null || true
}

# The host build exists only to run the code on this machine: the hash
# vectors, the header codec, the exact dm table string and - for the
# simulation - the real probe/init/attach logic against regular files.
build_host_helper() {
    emit_source
    exe=$WORK/mystic-dmcrypt-host.exe
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) ;;
        *) exe=$WORK/mystic-dmcrypt-host ;;
    esac
    say "helper: compiling the host build ($(basename "$exe"))" >&2
    $ZIG cc -DMYSTIC_HOSTTEST=1 -Wall -Wextra -Wno-unused-parameter \
        "$WORK/mystic-dmcrypt.c" -o "$exe" || die "compiling the host helper failed"
    printf '%s\n' "$exe"
}

# ====================================================================
#  3. the boot half of the key
# ====================================================================
# The fingerprint shown is SHA-256(boot half)[0..7], printed by the helper
# itself so there is exactly one implementation of it.
key_fingerprint() {
    "$HOST" fingerprint "$STAGE/etc/mystic/luks.key"
}

stage_key() {
    mkdir -p "$STAGE/etc/mystic"
    if [ -f "$STAGE/etc/mystic/luks.key" ]; then
        size=$(wc -c < "$STAGE/etc/mystic/luks.key" | tr -d ' ')
        [ "$size" = 32 ] || die "$STAGE/etc/mystic/luks.key is $size bytes, not 32 - \
move it away and run './tools/mkenc.sh new-key'"
        say "key: reusing $STAGE/etc/mystic/luks.key (fp $(key_fingerprint))"
    else
        "$HOST" keygen "$STAGE/etc/mystic/luks.key" || die "cannot create the key"
        say "key: created $STAGE/etc/mystic/luks.key (fp $(key_fingerprint))"
        warn "this file is the boot half of every disk key. Do not commit it and"
        warn "do not publish an initramfs built without it. See docs/ENCRYPTION.md."
    fi
}

# ====================================================================
#  4. verification - every line is a fact about a file on disk
# ====================================================================
elf_report() {   # elf_report <path> -> "ET_EXEC, 3 program headers, no PT_INTERP"
    python - "$1" <<'PYEOF'
import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read()
if d[:4] != b'\x7fELF':
    print('not-elf'); sys.exit(1)
if d[4] != 2 or d[5] != 1:
    print('not-elf64-le'); sys.exit(1)
e_type, = struct.unpack_from('<H', d, 16)
e_phoff, = struct.unpack_from('<Q', d, 32)
e_phentsize, e_phnum = struct.unpack_from('<HH', d, 54)
interp = False
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, = struct.unpack_from('<I', d, off)
    if p_type == 3:
        interp = True
names = {1: 'PT_LOAD', 2: 'PT_DYNAMIC', 3: 'PT_INTERP', 4: 'PT_NOTE', 6: 'PT_PHDR',
         7: 'PT_TLS', 0x6474e550: 'PT_GNU_EH_FRAME', 0x6474e551: 'PT_GNU_STACK',
         0x6474e552: 'PT_GNU_RELRO', 0x6474e553: 'PT_GNU_PROPERTY'}
kinds = []
if not interp:
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        t, = struct.unpack_from('<I', d, off)
        n = names.get(t)
        if n and n not in kinds:
            kinds.append(n)
print('%s, %d program header(s), %s' % (
    'ET_EXEC' if e_type == 2 else 'e_type=%d' % e_type,
    e_phnum,
    'PT_INTERP present (DYNAMIC)' if interp else 'no PT_INTERP (static)'))
sys.exit(1 if interp else 0)
PYEOF
}

verify() {
    local bad=0 f
    say "verify: helper"
    if [ -f "$STAGE/sbin/mystic-dmcrypt" ]; then
        printf '    size      : %s bytes\n' "$(wc -c < "$STAGE/sbin/mystic-dmcrypt" | tr -d ' ')"
        printf '    mode here : %s (tools/mkimage.py forces 0755 for sbin/* in the image)\n' \
            "$(ls -l "$STAGE/sbin/mystic-dmcrypt" | cut -c1-10)"
        printf '    elf       : %s\n' "$(elf_report "$STAGE/sbin/mystic-dmcrypt")" || bad=1
    else
        warn "stage/sbin/mystic-dmcrypt is missing"; bad=1
    fi

    say "verify: modules"
    for f in $MODULES; do
        if [ ! -f "$KO_DST/$f" ]; then
            warn "missing $KO_DST/$f"; bad=1; continue
        fi
        if [ "$(head -c 4 "$KO_DST/$f" | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]; then
            warn "$KO_DST/$f is not an ELF object"; bad=1; continue
        fi
        # vermagic is what the kernel checks. A module built for another
        # kernel version is the classic "Invalid module format" boot failure,
        # so the string is checked here rather than discovered at boot.
        if grep -aq -- "$KVER" "$KO_DST/$f"; then
            printf '    %-58s %7s bytes, vermagic %s\n' "${f#kernel/}" \
                "$(wc -c < "$KO_DST/$f" | tr -d ' ')" "$KVER"
        else
            warn "$KO_DST/$f does not mention $KVER (wrong kernel package?)"; bad=1
        fi
    done
    # dm-crypt resolves symbols from both of these; a partial stage is a
    # module that will not load, which is worse than no encryption at all.
    for dep in kernel/drivers/md/dm-mod.ko \
               kernel/security/keys/encrypted-keys/encrypted-keys.ko; do
        [ -f "$KO_DST/$dep" ] || { warn "dm-crypt needs $dep"; bad=1; }
    done

    say "verify: key"
    if [ -f "$STAGE/etc/mystic/luks.key" ]; then
        printf '    file      : %s\n' "$STAGE/etc/mystic/luks.key"
        printf '    length    : %s bytes\n' "$(wc -c < "$STAGE/etc/mystic/luks.key" | tr -d ' ')"
        printf '    fingerprint: %s (SHA-256 of the 32-byte boot half, first 8 bytes)\n' \
            "$(key_fingerprint)"
        printf '    packed as : /etc/mystic/luks.key, mode 0644 (tools/mkimage.py clears\n'
        printf '                the mode of non-executable files; the boot half is not\n'
        printf '                secret from root, only from a copy of the disk image)\n'
    else
        warn "no $STAGE/etc/mystic/luks.key"; bad=1
    fi
    return $bad
}

# ====================================================================
#  5. what the next boot will do
# ====================================================================
cmd_status() {
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST"' EXIT
    printf 'kernel modules : %s\n' "$KO_DST"
    for f in $MODULES; do
        [ -f "$KO_DST/$f" ] && printf '  present      : %s\n' "${f#kernel/}" \
                            || printf '  MISSING      : %s\n' "${f#kernel/}"
    done
    if [ -f "$STAGE/sbin/mystic-dmcrypt" ]; then
        printf 'helper         : stage/sbin/mystic-dmcrypt (%s bytes)\n' \
            "$(wc -c < "$STAGE/sbin/mystic-dmcrypt" | tr -d ' ')"
    else
        printf 'helper         : MISSING - run ./tools/mkenc.sh\n'
    fi
    if [ -f "$STAGE/etc/mystic/luks.key" ]; then
        printf 'boot key half  : stage/etc/mystic/luks.key (fp %s)\n' "$(key_fingerprint)"
    else
        printf 'boot key half  : MISSING\n'
    fi
    if [ ! -f build/disk.img ]; then
        printf 'disk           : build/disk.img does not exist yet\n'
        printf 'next boot      : creates it, writes the MYSTICC1 header, mkfs.ext4\n'
        printf '                 inside dm-crypt, mounts it on /root\n'
        return 0
    fi
    printf 'disk           : build/disk.img (%s bytes)\n' \
        "$(wc -c < build/disk.img | tr -d ' ')"
    if "$HOST" probe build/disk.img > "$WORK/status.probe" 2>&1; then
        printf 'next boot      : unlocks it with the fingerprint above (%s)\n' \
            "$(cat "$WORK/status.probe")"
    else
        case "$(cat "$WORK/status.probe")" in
            blank)
                printf 'next boot      : blank disk -> writes the header and formats it\n' ;;
            foreign)
                printf 'next boot      : NO encryption header, and not blank:\n'
                printf '                 a plain ext4 disk boots unencrypted (same messages\n'
                printf '                 as before). To encrypt, run ./tools/mkenc.sh reset-disk\n'
                printf '                 and let the next boot create a fresh volume.\n' ;;
            *)
                printf 'next boot      : cannot read the disk: %s\n' "$(cat "$WORK/status.probe")" ;;
        esac
    fi
}

# ====================================================================
#  6. key and disk lifecycle
# ====================================================================
cmd_key() {
    [ -f "$STAGE/etc/mystic/luks.key" ] || die "no key yet - run ./tools/mkenc.sh"
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST"' EXIT
    printf 'file        : %s\n' "$STAGE/etc/mystic/luks.key"
    printf 'length      : %s bytes\n' "$(wc -c < "$STAGE/etc/mystic/luks.key" | tr -d ' ')"
    printf 'fingerprint : %s\n' "$(key_fingerprint)"
    printf 'a disk only opens with this half AND the half in its own MYSTICC1 header.\n'
}

cmd_new_key() {
    [ -f "$STAGE/etc/mystic/luks.key" ] || die "no key yet - run ./tools/mkenc.sh"
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST"' EXIT
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    mkdir -p "$WORK"
    mv "$STAGE/etc/mystic/luks.key" "$WORK/luks.key.$stamp"
    "$HOST" keygen "$STAGE/etc/mystic/luks.key" || die "cannot create the new key"
    printf 'old half saved : %s\n' "$WORK/luks.key.$stamp"
    printf 'new fingerprint: %s\n' "$(key_fingerprint)"
    warn "every existing encrypted disk now needs the saved file:"
    warn "  mystic-dmcrypt attach mystic-root /dev/vda $WORK/luks.key.$stamp"
    warn "Put it back as stage/etc/mystic/luks.key if you want the next image to open them."
}

cmd_reset_disk() {
    if [ ! -f build/disk.img ]; then
        printf 'build/disk.img does not exist - the next boot will create an encrypted one.\n'
        return 0
    fi
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    mv build/disk.img "build/disk.img.plain-$stamp"
    printf 'moved build/disk.img -> build/disk.img.plain-%s\n' "$stamp"
    printf 'The old image is untouched and still mounts as a plain ext4 disk if you\n'
    printf 'rename it back. image.sh will create a fresh one on the next run.\n'
}

# ====================================================================
#  7. the boot-path simulation
# ====================================================================
# No kernel, no QEMU, no device mapper: stage/init's own functions run
# against a regular file standing in for /dev/vda, a fake mount/mkfs,
# and the host build of the helper. What is NOT covered by this: the
# three dm ioctls, because they need a real kernel. Everything the
# script decides before those ioctls is covered.
cmd_test() {
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST"' EXIT
    say "selftest of the host build (hash vectors, table string, header codec)"
    "$HOST" selftest || die "host selftest failed"
    SIM=$WORK/sim
    rm -rf "$SIM"
    mkdir -p "$SIM"
    cat > "$SIM/run.sh" <<'MYSTIC_SIM_EOF'
#!/bin/sh
# generated by: ./tools/mkenc.sh test
#
# Boots stage/init's disk logic with no kernel, no QEMU and no device
# mapper. What is real here: stage/init itself (sourced, not copied), the
# host build of stage/sbin/mystic-dmcrypt (so the MYSTICC1 header, the
# key check and the SHA-256 fingerprint are the shipping code), and the
# four states a disk can be in. What is fake: the block device is a
# regular file, mount/mkfs/modprobe are shell scripts, and the three dm
# ioctls never run (they need a kernel). `disk_present` is replaced for
# every scenario except the first, because a Windows build host has no
# block devices at all; scenario 1 keeps the real [ -b ] check.
set -u

ROOT=${ROOT:?}
SIM=${SIM:?}
DMCRYPT=${SIM_DMCRYPT:?}
INIT=$ROOT/stage/init
BIN=$SIM/bin
DEV=$SIM/dev
ST=$SIM/state
FS=$SIM/fs

pass=0
fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass + 1)); }
no()  { printf '  FAIL  %s\n' "$1"; fail=$((fail + 1)); }
check()      { if printf '%s\n' "$out" | grep -qF -- "$1"; then ok "$1"; else no "$1: expected '$1'"; fi; }
check_not()  { if printf '%s\n' "$out" | grep -qF -- "$1"; then no "unexpected '$1'"; else ok "no '$1'"; fi; }

rm -rf "$BIN" "$DEV" "$FS" "$ST"
mkdir -p "$BIN" "$DEV" "$FS" "$ST"

# ---- fake tools -----------------------------------------------------
cat > "$BIN/mount" <<'FMT_EOF'
#!/bin/sh
# fake mount: succeeds only if this device already holds a filesystem
i=0; n=$#; dev=""; tgt=""
while [ "$i" -lt "$n" ]; do
    a=$1; shift; i=$((i + 1))
    case "$a" in
        -t|-o) if [ "$i" -lt "$n" ]; then shift; i=$((i + 1)); fi ;;
        -*) ;;
        *) if [ -z "$dev" ]; then dev=$a; else tgt=$a; fi ;;
    esac
done
[ -n "$dev" ] || { echo "fake mount: no device" >&2; exit 2; }
m=$FAKE_FS/fs$(printf '%s' "$dev" | tr '/' '_')
[ -f "$m" ] || { echo "mount: $dev: wrong fs type, bad option, bad superblock" >&2; exit 32; }
printf '%s' "$dev" > "$FAKE_STATE/mounted"
echo "fake-mount: $dev on ${tgt:-?}"
exit 0
FMT_EOF

cat > "$BIN/mkfs.ext4" <<'FMT_EOF'
#!/bin/sh
# fake mkfs.ext4 -q -F <dev>: records that the device now holds a filesystem
dev=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) [ -z "$dev" ] && dev=$a ;;
    esac
done
[ -n "$dev" ] || { echo "fake mkfs.ext4: no device" >&2; exit 2; }
printf 'ext4' > "$FAKE_FS/fs$(printf '%s' "$dev" | tr '/' '_')"
echo "fake-mkfs.ext4: ext4 on $dev"
exit 0
FMT_EOF

cat > "$BIN/modprobe" <<'FMT_EOF'
#!/bin/sh
# fake modprobe: "loading" dm_mod creates the control node; nothing loads
# at all when modules.ok is absent (the missing-module scenario)
[ -f "$FAKE_STATE/modules.ok" ] || { echo "modprobe: FATAL: Module $1 not found." >&2; exit 1; }
: > "$FAKE_STATE/control"
exit 0
FMT_EOF
chmod +x "$BIN"/*

reset_boot() {   # one boot: fresh machine state, same disks
    rm -rf "$ST"
    mkdir -p "$ST"
    : > "$ST/modules.ok"
}
no_modules() {   # a boot where dm_mod/dm_crypt cannot be loaded
    rm -f "$ST/modules.ok" "$ST/control"
}
new_disk() {     # new_disk <path> <KiB>  - a blank disk, no filesystem
    python -c "import sys; open(sys.argv[1], 'wb').truncate(int(sys.argv[2]) * 1024)" "$1" "$2"
    rm -f "$FS/fs$(printf '%s' "$1" | tr '/' '_')"
}
plain_disk() {   # a disk an older build formatted: ext4 magic, no header
    python - "$1" <<'PYEOF'
import sys
d = bytearray(64 * 1024)
d[0x438] = 0x53          # ext2/3/4 superblock magic 0x53EF at 0x438
d[0x439] = 0xef
open(sys.argv[1], 'wb').write(d)
PYEOF
    printf 'ext4' > "$FS/fs$(printf '%s' "$1" | tr '/' '_')"
}

out=""
run_setup() {    # run_setup [real-block-check 0|1] [helper-path]
    real=${1:-0}
    helper=${2:-$DMCRYPT}
    out=$(
        export PATH="$BIN:$PATH"
        export MYSTIC_INIT_LIB=1 MYSTIC_DISK="$DISK" MYSTIC_KEY="$KEY" \
               MYSTIC_DMCRYPT="$helper" MYSTIC_DM_CONTROL="$ST/control" \
               MYSTIC_MKFS="$BIN/mkfs.ext4" MYSTIC_CRYPT_LOG="$ST/crypt.log" \
               MYSTIC_MAP=mystic-root FAKE_STATE="$ST" FAKE_FS="$FS"
        . "$INIT"
        if [ "$real" != 1 ]; then
            disk_present() { [ -f "$MYSTIC_DISK" ]; }
        fi
        setup_disk
        echo "status: $CRYPT_STATUS"
        [ -f "$ST/mounted" ] && echo "mounted: $(cat "$ST/mounted")"
        exit 0
    ) 2>&1
}
show() { printf '%s\n' "$out" | sed 's/^/        | /'; }

KEY=$SIM/luks.key
rm -f "$KEY"
"$DMCRYPT" keygen "$KEY" >/dev/null || { echo "cannot create the test key" >&2; exit 1; }

# ---------------------------------------------------------------- 1
printf '\n== 1. no disk at all (real [ -b ] check) ==\n'
DISK=/dev/vda
reset_boot
run_setup 1
show
check "disk: no /dev/vda"
check_not "fake-mount"

# ---------------------------------------------------------------- 2
printf '\n== 2. first boot on a blank disk ==\n'
DISK=$DEV/vda
new_disk "$DISK" 8192
reset_boot
run_setup
show
check "disk: created dm-crypt volume on $DISK (aes-xts-plain64)"
check "disk: dm-crypt unlocked, no ext4 inside - creating one"
check "disk: formatted ext4 inside dm-crypt and mounted on /root"
check "mounted: /dev/mapper/mystic-root"
check "status: aes-xts-plain64, key "
hdr=$("$DMCRYPT" probe "$DISK" 2>&1)
show_hdr=$hdr
printf '        | header now: %s\n' "$show_hdr"
case "$hdr" in
    "crypt 1 "*) ok "the disk file now starts with a MYSTICC1 header" ;;
    *)           no "the disk file has no MYSTICC1 header ($hdr)" ;;
esac

# ---------------------------------------------------------------- 3
printf '\n== 3. second boot on the same disk ==\n'
reset_boot
run_setup
show
check "disk: dm-crypt unlocked, formatted ext4 mounted on /root"
check "mounted: /dev/mapper/mystic-root"
check_not "created dm-crypt volume"
check_not "no ext4 inside"

# ---------------------------------------------------------------- 4
printf '\n== 4. a plain ext4 disk from an older build ==\n'
DISK=$DEV/plain
plain_disk "$DISK"
reset_boot
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK has no encryption header and is not blank"
check "disk: mounted $DISK (ext4) on /root"
check_not "created dm-crypt volume"
check_not "creating ext4 filesystem"
[ "$(cksum "$DISK")" = "$before" ] && ok "the plain disk was not written to" \
                                 || no "the plain disk was modified"

# ---------------------------------------------------------------- 5
printf '\n== 5. encrypted disk, dm modules missing ==\n'
DISK=$DEV/vda
reset_boot
no_modules
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK is encrypted but cannot be opened - dm_mod/dm_crypt did not load"
check "disk: refusing to format it, /root stays on the RAM filesystem"
check_not "creating ext4 filesystem"
check_not "fake-mount"
[ "$(cksum "$DISK")" = "$before" ] && ok "the encrypted disk was not written to" \
                                  || no "the encrypted disk was modified"

# ---------------------------------------------------------------- 6
printf '\n== 6. encrypted disk, wrong key ==\n'
DISK=$DEV/vda
rm -f "$SIM/wrong.key"
"$DMCRYPT" keygen "$SIM/wrong.key" >/dev/null
KEY=$SIM/wrong.key
reset_boot
before=$(cksum "$DISK")
run_setup
show
check "disk: encrypted, but the key did not unlock it - $DISK is left untouched"
check "key does not match this disk"
check "status: locked (key does not match)"
check_not "creating ext4 filesystem"
[ "$(cksum "$DISK")" = "$before" ] && ok "the encrypted disk was not written to" \
                                  || no "the encrypted disk was modified"
KEY=$SIM/luks.key

# ---------------------------------------------------------------- 7
printf '\n== 7. blank disk, no key in the image ==\n'
DISK=$DEV/fresh
new_disk "$DISK" 8192
KEY=$SIM/absent.key
rm -f "$KEY"
reset_boot
run_setup
show
check "disk: no encryption key at $KEY - using plain ext4"
check "disk: formatted ext4 and mounted on /root"
check_not "created dm-crypt volume"
KEY=$SIM/luks.key

# ---------------------------------------------------------------- 8
printf '\n== 8. no helper in the image ==\n'
DISK=$DEV/fresh
new_disk "$DISK" 8192
reset_boot
run_setup 0 "$SIM/no-such-helper"
show
check "disk: dm-crypt unavailable - no $SIM/no-such-helper in the image"
check "disk: formatted ext4 and mounted on /root"
check_not "created dm-crypt volume"

# ---------------------------------------------------------------- 9
printf '\n== 9. an encrypted disk whose ext4 was never created ==\n'
DISK=$DEV/emptyvol
new_disk "$DISK" 8192
reset_boot
"$DMCRYPT" init "$DISK" "$KEY" >/dev/null      # a header...
# ...and no filesystem inside it: the fake filesystem marker belongs to the
# mapping (/dev/mapper/mystic-root), which every scenario shares, so it has to
# be cleared explicitly to model mkfs never having run.
rm -f "$FS/fs_dev_mapper_mystic-root"
run_setup
show
check "disk: dm-crypt unlocked, no ext4 inside - creating one"
check "disk: formatted ext4 inside dm-crypt and mounted on /root"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ] || exit 1
exit 0
MYSTIC_SIM_EOF
    say "boot-path simulation: stage/init + host helper + fake /dev"
    ROOT=$(pwd) SIM="$SIM" SIM_DMCRYPT="$HOST" sh "$SIM/run.sh" \
        || die "the boot-path simulation failed"
}

# ====================================================================
#  8. main
# ====================================================================
cmd_stage() {
    say "kernel: $KVER"
    stage_modules
    build_helper
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST"' EXIT
    say "selftest of the same code on this machine"
    "$HOST" selftest || die "selftest failed"
    stage_key
    verify || die "verification failed"
    printf '\nNext: ./tools/mkenc.sh status   (what the next boot will do)\n'
    printf '      ./tools/mkenc.sh test     (boot-path simulation, no QEMU)\n'
}

case "${1:-stage}" in
    stage)      cmd_stage ;;
    status)     cmd_status ;;
    test)       cmd_test ;;
    key)        cmd_key ;;
    new-key)    cmd_new_key ;;
    reset-disk) cmd_reset_disk ;;
    -h|--help|help)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        ;;
    *) die "unknown command '$1' (try --help)" ;;
esac
