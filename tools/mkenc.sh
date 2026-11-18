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
#    ./tools/mkenc.sh seal [rounds]  seal the boot half with the lock-screen password
#                                  so a boot has to ask for it (see docs/ENCRYPTION.md)
#    ./tools/mkenc.sh unseal       put the plaintext half back (no boot prompt)
#    ./tools/mkenc.sh reset-disk   move build/disk.img aside so the next boot
#                                  creates an encrypted one
#    ./tools/mkenc.sh elf <file>   print an ELF file's program headers and fail
#                                  if it needs a dynamic loader
#
#  Run it AFTER tools/fetch-kernel.sh: that script wipes
#  stage/lib/modules/<version> and would delete the modules again.
# ============================================================
set -eu
cd "$(dirname "$0")/.."

CACHE=${MYSTIC_CACHE:-build/cache}
STAGE=${MYSTIC_STAGE:-stage}   # MYSTIC_STAGE lets a harness seal/unseal a copy of stage/
WORK=${MYSTIC_WORK:-build/mkenc}   # MYSTIC_WORK lets a harness build outside the tree
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
    local n=0 rel src out
    for rel in $MODULES; do
        src=$KO_SRC/$rel.gz
        out=$KO_DST/$rel
        if [ ! -f "$out" ]; then
            [ -n "$KO_SRC" ] || die "no unpacked kernel package in $CACHE - run ./tools/fetch-kernel.sh first"
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
#ifndef _WIN32
#include <termios.h>   /* echo off for the boot passphrase prompt */
#endif

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

/* probe results, as shell exit codes - stage/init branches on these */
#define EXIT_PROBE_BLANK 10     /* first 4 KiB is zero */
#define EXIT_PROBE_FS 11        /* ext2/3/4 superblock magic at 0x438 */
#define EXIT_PROBE_UNKNOWN 12   /* neither: never guess */
/* attach failures, as shell exit codes - the headline must not blame the key
 * for a device-mapper error, and must not blame dm for a wrong key */
#define EXIT_BADARG 1
#define EXIT_KEYFILE 2
#define EXIT_KEYMISMATCH 3
#define EXIT_DM 4
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

static int tmp_open(char *buf, size_t cap, const char *dir, const char *stem)
{
    snprintf(buf, cap, "%s/%s-XXXXXX", dir, stem);
    return mkstemp(buf);
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

/* ============================================================
 *  The sealed boot key
 *
 *  The 32-byte boot half of the disk key normally sits in the
 *  initramfs in the clear. When the owner sets a passphrase it is
 *  stored as a 112-byte blob instead:
 *
 *    0x00  8  "MYSTICKS"
 *    0x08  4  version (1)
 *    0x0c  4  PBKDF2-HMAC-SHA256 rounds
 *    0x10 16  salt
 *    0x20 12  ChaCha20 nonce (RFC 8439, 96-bit)
 *    0x2c  4  flags (0)
 *    0x30 32  ChaCha20 ciphertext of the 32-byte boot half
 *    0x50 32  HMAC-SHA256(mac_key, bytes 0x00..0x4f)
 *
 *  PBKDF2-HMAC-SHA256(passphrase, salt, rounds) produces 64 bytes:
 *  the first 32 encrypt, the last 32 authenticate (encrypt-then-MAC).
 *  All three primitives are from published standards and are checked
 *  against their published test vectors in selftest, so this is
 *  standard cryptography written small, not a new construction. What
 *  it is worth is discussed in docs/ENCRYPTION.md section 6.
 * ============================================================ */
#define SEAL_MAGIC "MYSTICKS"
#define SEAL_VERSION 1u
#define SEAL_BYTES 112u
#define SEAL_ROUNDS 50000u         /* see docs/ENCRYPTION.md 6: the lock-screen
                                    * file (20000 rounds) is the cheaper verifier
                                    * whenever both leaked, so a bigger number here
                                    * would only slow the honest boot */
#define SEAL_MAXROUNDS 20000000u   /* refuse a blob that would hang the boot */
#define O_S_VERSION 0x08
#define O_S_ROUNDS 0x0c
#define O_S_SALT 0x10
#define S_SALT 16
#define O_S_NONCE 0x20
#define S_NONCE 12
#define O_S_FLAGS 0x2c
#define O_S_CIPHER 0x30
#define O_S_TAG 0x50
/* a marker in the header, set only after a filesystem was created inside the
 * mapping. It is what lets init tell "never formatted" (mkfs is safe) from
 * "was formatted once, the mount failed" (never touch it) even when the
 * filesystem inside is not ext4. */
#define O_FSMARK 0x58
#define FSMARK_MAGIC "MYSTICFS"
#define FSMARK_VERSION 1u

/* attach / unseal failures as shell exit codes, distinct so that stage/init
 * can report the real cause instead of blaming the key for a console */
#define EXIT_SEALBAD 5      /* the blob did not authenticate: wrong passphrase or damage */
#define EXIT_SEALFORMAT 6   /* not a sealed key at all */
#define EXIT_NOPASS 7       /* no passphrase could be read */
#define EXIT_NOPASSWD 8     /* passphrase is not the lock-screen password */

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/* RFC 8439 section 2.3: 20 rounds of the ChaCha quarter round on a
 * 16-word state. The constants are the ASCII of "expand 32-byte k". */
static void chacha20_block(const unsigned char key[32], const unsigned char nonce[12],
                           uint32_t counter, unsigned char out[64])
{
    static const char sigma[17] = "expand 32-byte k";
    uint32_t st[16], x[16];
    int i, j;
    for (i = 0; i < 4; i++) st[i] = get_le32((const unsigned char *)sigma + 4 * i);
    for (i = 0; i < 8; i++) st[4 + i] = get_le32(key + 4 * i);
    st[12] = counter;
    for (i = 0; i < 3; i++) st[13 + i] = get_le32(nonce + 4 * i);
    memcpy(x, st, sizeof x);
    for (i = 0; i < 10; i++) {
#define QR(a, b, c, d)                    \
        a += b; d = rotl32(d ^ a, 16);    \
        c += d; b = rotl32(b ^ c, 12);    \
        a += b; d = rotl32(d ^ a, 8);     \
        c += d; b = rotl32(b ^ c, 7)
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
#undef QR
    }
    for (j = 0; j < 16; j++) put_le32(out + 4 * j, x[j] + st[j]);
    wipe(st, sizeof st);
    wipe(x, sizeof x);
}

static void chacha20_xor(const unsigned char key[32], const unsigned char nonce[12],
                         uint32_t counter, unsigned char *buf, size_t len)
{
    unsigned char block[64];
    size_t i = 0;
    while (i < len) {
        size_t n = (len - i) < 64 ? (len - i) : 64, j;
        chacha20_block(key, nonce, counter, block);
        for (j = 0; j < n; j++) buf[i + j] ^= block[j];
        counter++;
        i += n;
    }
    wipe(block, sizeof block);
}

/* HMAC-SHA256 (RFC 2104) over a||b, so PBKDF2 can hash salt||INT(i)
 * without copying either into a temporary buffer. */
static void hmac_sha256_parts(const unsigned char *key, size_t keylen,
                              const unsigned char *a, size_t alen,
                              const unsigned char *b, size_t blen,
                              unsigned char out[FPRINT])
{
    unsigned char k[64], pad[64], inner[FPRINT];
    struct sha256 s;
    size_t i;
    if (keylen > sizeof k) {
        sha256_buf(key, keylen, k);
        memset(k + FPRINT, 0, sizeof k - FPRINT);
    } else {
        memcpy(k, key, keylen);
        memset(k + keylen, 0, sizeof k - keylen);
    }
    for (i = 0; i < sizeof pad; i++) pad[i] = (unsigned char)(k[i] ^ 0x36);
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    if (a && alen) sha256_update(&s, a, alen);
    if (b && blen) sha256_update(&s, b, blen);
    sha256_final(&s, inner);
    for (i = 0; i < sizeof pad; i++) pad[i] = (unsigned char)(k[i] ^ 0x5c);
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    sha256_update(&s, inner, FPRINT);
    sha256_final(&s, out);
    wipe(k, sizeof k);
    wipe(pad, sizeof pad);
    wipe(inner, sizeof inner);
}

/* PBKDF2-HMAC-SHA256 (RFC 8018). Returns -1 for a round count that is zero
 * or absurd: a sealed blob must not be able to hang the boot. */
static int pbkdf2_sha256(const char *pass, size_t passlen,
                         const unsigned char *salt, size_t saltlen,
                         uint32_t rounds, unsigned char *out, size_t outlen)
{
    unsigned char u[FPRINT], v[FPRINT], t[FPRINT], be[4];
    uint32_t i, c;
    size_t have = 0;
    if (rounds == 0 || rounds > SEAL_MAXROUNDS) return -1;
    for (i = 1; have < outlen; i++) {
        size_t take, j;
        be[0] = (unsigned char)(i >> 24);
        be[1] = (unsigned char)(i >> 16);
        be[2] = (unsigned char)(i >> 8);
        be[3] = (unsigned char)i;
        hmac_sha256_parts((const unsigned char *)pass, passlen, salt, saltlen, be, 4, u);
        memcpy(t, u, FPRINT);
        for (c = 1; c < rounds; c++) {
            hmac_sha256_parts((const unsigned char *)pass, passlen, u, FPRINT, NULL, 0, v);
            memcpy(u, v, FPRINT);
            for (j = 0; j < FPRINT; j++) t[j] ^= v[j];
        }
        take = (outlen - have) < FPRINT ? (outlen - have) : FPRINT;
        memcpy(out + have, t, take);
        have += take;
    }
    wipe(u, sizeof u);
    wipe(v, sizeof v);
    wipe(t, sizeof t);
    return 0;
}

/* One line, no echo. A too-long passphrase is refused rather than truncated:
 * silently using 128 of 200 characters would create a key nobody can reopen. */
static int read_line_fd(int fd, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        unsigned char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0 || ch == '\n') break;
        if (ch == '\r') continue;
        if (ch == 0x7f || ch == 0x08) {
            if (n) out[--n] = 0;
            continue;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return (int)n;
}

/* Reads a passphrase from the console when prompt is set, otherwise from
 * stdin (tools/mkenc.sh and the boot simulation drive it that way). The
 * shipping boot path never passes the passphrase through argv or a pipe. */
static int read_passphrase(int prompt, const char *console_path, char *out, size_t cap)
{
    int fd = 0, close_fd = 0, rc;
#ifndef _WIN32
    struct termios old, quiet;
    int tty = 0;
#endif
    if (prompt) {
        const char *p = (console_path && *console_path) ? console_path : "/dev/console";
        fd = open(p, O_RDONLY);
        if (fd < 0) fd = 0;
        else close_fd = 1;
        fprintf(stderr, "passphrase: ");
        fflush(stderr);
    }
#ifndef _WIN32
    if (close_fd && tcgetattr(fd, &old) == 0) {
        quiet = old;
        quiet.c_lflag &= (tcflag_t)~ECHO;
        if (tcsetattr(fd, TCSANOW, &quiet) == 0) tty = 1;
    }
#endif
    rc = read_line_fd(fd, out, cap);
#ifndef _WIN32
    if (tty) {
        tcsetattr(fd, TCSANOW, &old);
        fprintf(stderr, "\n");   /* the Enter was not echoed either */
    }
#endif
    if (close_fd) close(fd);
    if (rc <= 0) {
        wipe(out, cap);
        return -1;
    }
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *in, unsigned char *out, int max)
{
    int n = 0;
    while (in[0] && in[1] && n < max) {
        int hi = hex_nibble(in[0]), lo = hex_nibble(in[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        in += 2;
    }
    return n;
}

/* Byte-for-byte the derivation in src/sha256.c mystic_hash_password(), so the
 * passphrase checked here is the one the desktop lock screen accepts. */
static void hash_password(const unsigned char *salt, size_t salt_len,
                          const char *password, uint32_t rounds, unsigned char out[FPRINT])
{
    unsigned char buf[64 + 32];
    size_t plen = strlen(password);
    uint32_t i;
    if (plen > 64) plen = 64;
    memcpy(buf, salt, salt_len);
    memcpy(buf + salt_len, password, plen);
    sha256_buf(buf, salt_len + plen, out);
    for (i = 1; i < rounds; i++) {
        memcpy(buf, out, FPRINT);
        memcpy(buf + FPRINT, salt, salt_len);
        sha256_buf(buf, FPRINT + salt_len, out);
    }
    wipe(buf, sizeof buf);
}

/* 1 match, 0 mismatch, -1 unreadable/unparseable. Format: salt:rounds:hash */
static int check_password_line(const char *line, const char *password)
{
    char salt_hex[80];
    const char *p = line;
    int i = 0, rounds = 0, salt_len;
    unsigned char salt[32], want[FPRINT], got[FPRINT];
    unsigned diff = 0;
    while (*p && *p != ':' && i < (int)sizeof salt_hex - 1) salt_hex[i++] = *p++;
    salt_hex[i] = 0;
    if (*p != ':') return -1;
    p++;
    while (*p >= '0' && *p <= '9') { rounds = rounds * 10 + (*p - '0'); p++; }
    if (*p != ':') return -1;
    p++;
    if (rounds <= 0) rounds = 1;
    salt_len = hex_decode(salt_hex, salt, 32);
    if (salt_len <= 0) return -1;
    if (hex_decode(p, want, FPRINT) != FPRINT) return -1;
    hash_password(salt, (size_t)salt_len, password, (uint32_t)rounds, got);
    /* constant time: the loop always runs the full length */
    for (i = 0; i < FPRINT; i++) diff |= (unsigned)(got[i] ^ want[i]);
    wipe(got, sizeof got);
    return diff == 0;
}

static int check_password_file(const char *path, const char *password)
{
    char line[256];
    int fd = open(path, O_RDONLY), n;
    if (fd < 0) return -1;
    n = (int)read(fd, line, sizeof line - 1);
    close(fd);
    if (n <= 0) return -1;
    line[n] = 0;
    n = check_password_line(line, password);
    wipe(line, sizeof line);
    return n;
}

/* Reads and validates a sealed-key blob. Returns 0, or -1 with err set. */
static int seal_read(const char *path, unsigned char blob[SEAL_BYTES], uint32_t *rounds,
                     char *err, size_t errcap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0) {
        snprintf(err, errcap, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    n = read(fd, blob, SEAL_BYTES + 1);
    close(fd);
    if (n < 0) {
        snprintf(err, errcap, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    if ((size_t)n != SEAL_BYTES) {
        snprintf(err, errcap, "%s is %ld bytes, not %u - not a sealed key", path, (long)n,
                 (unsigned)SEAL_BYTES);
        return -1;
    }
    if (memcmp(blob, SEAL_MAGIC, 8) != 0) {
        snprintf(err, errcap, "%s does not start with %s", path, SEAL_MAGIC);
        return -1;
    }
    if (get_le32(blob + O_S_VERSION) != SEAL_VERSION) {
        snprintf(err, errcap, "%s is sealed-key version %u, this helper speaks %u", path,
                 (unsigned)get_le32(blob + O_S_VERSION), (unsigned)SEAL_VERSION);
        return -1;
    }
    if (get_le32(blob + O_S_FLAGS) != 0) {
        snprintf(err, errcap, "%s has unknown flags set - refusing to guess", path);
        return -1;
    }
    *rounds = get_le32(blob + O_S_ROUNDS);
    if (*rounds == 0 || *rounds > SEAL_MAXROUNDS) {
        snprintf(err, errcap, "%s asks for %u KDF rounds (limit %u)", path, (unsigned)*rounds,
                 (unsigned)SEAL_MAXROUNDS);
        return -1;
    }
    return 0;
}

/* Derives the 64-byte key pair and fills blob[O_S_CIPHER..] and the tag. */
static int seal_build(unsigned char blob[SEAL_BYTES], const unsigned char boot[HALF],
                      const char *pass, uint32_t rounds)
{
    unsigned char dk[64], tag[FPRINT];
    if (urandom_read(blob + O_S_SALT, S_SALT) < 0 || urandom_read(blob + O_S_NONCE, S_NONCE) < 0)
        return -1;
    if (pbkdf2_sha256(pass, strlen(pass), blob + O_S_SALT, S_SALT, rounds, dk, sizeof dk) < 0)
        return -1;
    memcpy(blob + O_S_CIPHER, boot, HALF);
    chacha20_xor(dk, blob + O_S_NONCE, 1, blob + O_S_CIPHER, HALF);
    hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, tag);
    memcpy(blob + O_S_TAG, tag, FPRINT);
    wipe(dk, sizeof dk);
    wipe(tag, sizeof tag);
    return 0;
}

static int cmd_seal(const char *keypath, const char *sealpath, const char *passwd_file,
                    uint32_t rounds)
{
    unsigned char boot[HALF], blob[SEAL_BYTES];
    char pass[192], hex[17];
    int fd, v;
    if (rounds == 0) rounds = SEAL_ROUNDS;
    if (rounds > SEAL_MAXROUNDS) return die("round count is above the limit", NULL, NULL);
    if (key_read(keypath, boot) != 0) return EXIT_KEYFILE;
    if (read_passphrase(0, NULL, pass, sizeof pass) != 0) {
        wipe(boot, sizeof boot);
        return die("no passphrase on stdin", NULL, NULL);
    }
    if (passwd_file && *passwd_file) {
        v = check_password_file(passwd_file, pass);
        if (v < 0) {
            wipe(boot, sizeof boot);
            wipe(pass, sizeof pass);
            return die("cannot read the lock-screen password file %s", passwd_file, NULL);
        }
        if (v == 0) {
            wipe(boot, sizeof boot);
            wipe(pass, sizeof pass);
            fprintf(stderr, "%s: that passphrase does not match %s - sealing it would "
                            "lock the disk away from the lock screen\n", prog, passwd_file);
            return EXIT_NOPASSWD;
        }
    }
    memset(blob, 0, sizeof blob);
    memcpy(blob, SEAL_MAGIC, 8);
    put_le32(blob + O_S_VERSION, SEAL_VERSION);
    put_le32(blob + O_S_ROUNDS, rounds);
    if (seal_build(blob, boot, pass, rounds) < 0) {
        wipe(boot, sizeof boot);
        wipe(pass, sizeof pass);
        return die_errno("cannot build the sealed key", "");
    }
    fd = open(sealpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        wipe(boot, sizeof boot);
        wipe(pass, sizeof pass);
        return die_errno("cannot create", sealpath);
    }
    if (write_all(fd, blob, sizeof blob) < 0 || fsync(fd) < 0) {
        int e = errno;
        close(fd);
        wipe(boot, sizeof boot);
        wipe(pass, sizeof pass);
        errno = e;
        return die_errno("cannot write", sealpath);
    }
    close(fd);
    {
        unsigned char d[FPRINT];
        sha256_buf(boot, HALF, d);
        to_hex(d, 8, hex);
    }
    printf("sealed %s as %s: %u rounds, key fingerprint %s\n", keypath, sealpath,
           (unsigned)rounds, hex);
    wipe(boot, sizeof boot);
    wipe(pass, sizeof pass);
    wipe(blob, sizeof blob);
    return 0;
}

static int cmd_unseal(const char *sealpath, const char *keypath, int prompt,
                      const char *console_path, const char *passwd_file)
{
    unsigned char blob[SEAL_BYTES], dk[64], want[FPRINT], boot[HALF];
    char pass[192], err[256];
    uint32_t rounds = 0;
    unsigned diff = 0;
    int fd, i, v, pw_ok = -1;   /* -1 no lock-screen file, 0 mismatch, 1 match */
    if (seal_read(sealpath, blob, &rounds, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", prog, err);
        return EXIT_SEALFORMAT;
    }
    if (read_passphrase(prompt, console_path, pass, sizeof pass) != 0) {
        fprintf(stderr, "%s: no passphrase could be read\n", prog);
        return EXIT_NOPASS;
    }
    /* The lock-screen file is only consulted to explain a failure: after the
     * owner changes the lock-screen password, the seal still opens with the
     * passphrase it was made with, and that must not become unreachable data.
     * So a mismatch warns, and only decides the message when the tag fails. */
    if (passwd_file && *passwd_file && access(passwd_file, R_OK) == 0) {
        v = check_password_file(passwd_file, pass);
        if (v >= 0) pw_ok = v;
    }
    if (pbkdf2_sha256(pass, strlen(pass), blob + O_S_SALT, S_SALT, rounds, dk, sizeof dk) < 0) {
        wipe(pass, sizeof pass);
        fprintf(stderr, "%s: %s asks for unusable KDF rounds\n", prog, sealpath);
        return EXIT_SEALFORMAT;
    }
    hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, want);
    for (i = 0; i < (int)FPRINT; i++) diff |= (unsigned)(want[i] ^ blob[O_S_TAG + i]);
    if (diff) {
        wipe(dk, sizeof dk);
        wipe(pass, sizeof pass);
        if (pw_ok == 0) {
            fprintf(stderr, "%s: that is not the lock-screen password\n", prog);
            return EXIT_NOPASSWD;
        }
        fprintf(stderr, "%s: %s did not authenticate - wrong passphrase or damaged seal\n",
                prog, sealpath);
        return EXIT_SEALBAD;
    }
    if (pw_ok == 0)
        fprintf(stderr, "%s: note: the seal opened, but this passphrase no longer matches %s\n",
                prog, passwd_file);
    memcpy(boot, blob + O_S_CIPHER, HALF);
    chacha20_xor(dk, blob + O_S_NONCE, 1, boot, HALF);
    fd = open(keypath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        wipe(dk, sizeof dk);
        wipe(pass, sizeof pass);
        wipe(boot, sizeof boot);
        return die_errno("cannot create the unsealed key", keypath);
    }
    (void)chmod(keypath, 0600);
    if (write_all(fd, boot, HALF) < 0 || fsync(fd) < 0) {
        int e = errno;
        close(fd);
        wipe(dk, sizeof dk);
        wipe(pass, sizeof pass);
        wipe(boot, sizeof boot);
        errno = e;
        return die_errno("cannot write the unsealed key to", keypath);
    }
    close(fd);
    (void)chmod(keypath, 0600);
    printf("unsealed %s -> %s (%u rounds)\n", sealpath, keypath, (unsigned)rounds);
    wipe(dk, sizeof dk);
    wipe(pass, sizeof pass);
    wipe(boot, sizeof boot);
    wipe(blob, sizeof blob);
    return 0;
}

static int cmd_verify(const char *passwd_file, int prompt, const char *console_path)
{
    char pass[192];
    int v;
    if (read_passphrase(prompt, console_path, pass, sizeof pass) != 0) {
        fprintf(stderr, "%s: no passphrase could be read\n", prog);
        return EXIT_NOPASS;
    }
    v = check_password_file(passwd_file, pass);
    wipe(pass, sizeof pass);
    if (v < 0) {
        fprintf(stderr, "%s: cannot read or parse %s\n", prog, passwd_file);
        return EXIT_NOPASS;
    }
    if (!v) {
        fprintf(stderr, "%s: wrong password\n", prog);
        return 1;
    }
    printf("password matches %s\n", passwd_file);
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

enum dev_state {
    DEV_ERROR = -1,
    DEV_CRYPT = 0,
    DEV_BLANK = 1,      /* first 4 KiB is zero: a disk nothing has written to */
    DEV_FS = 2,         /* ext2/3/4 superblock magic at byte 0x438 */
    DEV_UNKNOWN = 3     /* something is there and it is not ours: do not guess */
};

struct disk_info {
    enum dev_state state;
    uint32_t version;
    uint32_t offset_sectors;
    uint32_t key_bytes;
    unsigned char disk_half[HALF];
    unsigned char fingerprint[FPRINT];
    int fs_marked;      /* header marker: a filesystem was created in the mapping */
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

/* fills di and returns DEV_CRYPT, DEV_BLANK, DEV_FS, DEV_UNKNOWN or DEV_ERROR */
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
        di->fs_marked = memcmp(buf + O_FSMARK, FSMARK_MAGIC, 8) == 0 &&
                        get_le32(buf + O_FSMARK + 8) == FSMARK_VERSION;
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
        di->state = DEV_FS;
        return DEV_FS;
    }
    for (i = 0; i < (size_t)n; i++)
        if (buf[i]) { zero = 0; break; }
    if (n == 0 || (zero && (size_t)n == HDR_BYTES)) {
        di->state = DEV_BLANK;
        return DEV_BLANK;
    }
    /* Not blank, no filesystem magic, no header. A damaged header looks
     * exactly like this, so the only safe answer is "I do not know". */
    di->state = DEV_UNKNOWN;
    return DEV_UNKNOWN;
}

/* exit 0 marked, 1 not marked, 2 no readable MYSTICC1 header */
static int cmd_hasfs(const char *dev)
{
    struct disk_info di;
    int st = probe_dev(dev, &di);
    if (st != DEV_CRYPT) return 2;
    return di.fs_marked ? 0 : 1;
}

/* Set the "a filesystem was created inside this mapping" marker. Written
 * after mkfs+mount succeed, so the next boot can never confuse "formatted
 * once, mount failed" with "never formatted". */
static int cmd_markfs(const char *dev)
{
    unsigned char hdr[HDR_BYTES];
    int fd = open(dev, O_RDWR);
    if (fd < 0) return die_errno("cannot open", dev);
    if (read_exact(fd, hdr, sizeof hdr) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return die_errno("cannot read the header of", dev);
    }
    if (memcmp(hdr, HDR_MAGIC, 8) != 0) {
        close(fd);
        return die("%s has no MYSTICC1 header", dev, NULL);
    }
    memcpy(hdr + O_FSMARK, FSMARK_MAGIC, 8);
    put_le32(hdr + O_FSMARK + 8, FSMARK_VERSION);
    if (lseek(fd, 0, SEEK_SET) < 0 || write_all(fd, hdr, sizeof hdr) < 0 || fsync(fd) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return die_errno("cannot write the header of", dev);
    }
    close(fd);
    printf("marked %s: a filesystem was created inside its mapping\n", dev);
    return 0;
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
    if (key_read(keypath, boot) != 0) return EXIT_KEYFILE;
    memcpy(material, boot, HALF);
    memcpy(material + HALF, di.disk_half, HALF);
    sha256_buf(material, KEY_MATERIAL, want);
    if (memcmp(want, di.fingerprint, FPRINT) != 0) {
        char a[17], b[17];
        to_hex(want, 8, a);
        to_hex(di.fingerprint, 8, b);
        wipe(material, sizeof material);
        wipe(boot, sizeof boot);
        die("key does not match this disk (header %s, key %s) - the disk is "
            "left untouched", b, a);
        return EXIT_KEYMISMATCH;
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
            return EXIT_DM;
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
            return EXIT_DM;
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
            return EXIT_DM;
        }
        memcpy(params, table, plen);
        spec->next = (uint32_t)((sizeof *spec + plen + 7) & ~(size_t)7);
        b.dmi.data_size = b.dmi.data_start + spec->next;
        if (dm_simple(cfd, DM_TABLE_LOAD, &b.dmi, "DM_TABLE_LOAD") < 0) {
            dm_prepare(&b, name);
            dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE (cleanup)");
            close(cfd);
            return EXIT_DM;
        }
        dm_prepare(&b, name);
        if (dm_simple(cfd, DM_DEV_SUSPEND, &b.dmi, "DM_DEV_SUSPEND (resume)") < 0) {
            dm_prepare(&b, name);
            dm_simple(cfd, DM_DEV_REMOVE, &b.dmi, "DM_DEV_REMOVE (cleanup)");
            close(cfd);
            return EXIT_DM;
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
                return EXIT_DM;
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
        fd = tmp_open(disk, sizeof disk, tmpdir, "mystic-selftest-disk");
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
        fd = tmp_open(disk, sizeof disk, tmpdir, "mystic-selftest-disk");
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
        fd = tmp_open(disk, sizeof disk, tmpdir, "mystic-selftest-disk");
        if (fd >= 0) {
            /* an ext4 superblock magic at 0x438 must NEVER read as blank */
            unsigned char sb[4096];
            memset(sb, 0, sizeof sb);
            sb[0x438] = 0x53;
            sb[0x439] = 0xef;
            if (write_all(fd, sb, sizeof sb) == 0) {
                close(fd);
                if (probe_dev(disk, &di) == DEV_FS)
                    printf("ok   an ext4 superblock magic reads as fs, not blank\n");
                else {
                    printf("FAIL  an ext4 magic was not reported fs\n");
                    fails++;
                }
            } else close(fd);
            unlink(disk);
        }
        fd = tmp_open(disk, sizeof disk, tmpdir, "mystic-selftest-disk");
        if (fd >= 0) {
            /* neither zero, nor a filesystem, nor ours: "unknown", never blank.
             * This is what a damaged MYSTICC1 magic looks like. */
            unsigned char junk[4096];
            memset(junk, 0xAA, sizeof junk);
            if (write_all(fd, junk, sizeof junk) == 0) {
                close(fd);
                if (probe_dev(disk, &di) == DEV_UNKNOWN)
                    printf("ok   data that is not blank and not ext4 reads as unknown\n");
                else {
                    printf("FAIL  unrecognised data was not reported unknown\n");
                    fails++;
                }
            } else close(fd);
            unlink(disk);
        }
        fd = tmp_open(keyf, sizeof keyf, tmpdir, "mystic-selftest-key");
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
    {   /* HMAC-SHA256, RFC 4231 test case 2 */
        static const char *want = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
        unsigned char d2[FPRINT];
        char h2[FPRINT * 2 + 1];
        hmac_sha256_parts((const unsigned char *)"Jefe", 4,
                          (const unsigned char *)"what do ya want for nothing?", 28,
                          NULL, 0, d2);
        to_hex(d2, FPRINT, h2);
        printf("%s hmac-sha256 (RFC 4231 case 2)\n", strcmp(h2, want) ? "FAIL" : "ok  ");
        if (strcmp(h2, want)) fails++;
    }
    {   /* PBKDF2-HMAC-SHA256, the published vectors for P="password" S="salt" */
        static const struct { unsigned r; const char *want; } v[] = {
            {1u,    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
            {2u,    "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
            {4096u, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
        };
        int k;
        for (k = 0; k < 3; k++) {
            unsigned char dk[32];
            char h2[65];
            if (pbkdf2_sha256("password", 8, (const unsigned char *)"salt", 4, v[k].r, dk, 32) < 0) {
                printf("FAIL  pbkdf2-hmac-sha256 rounds %u refused\n", v[k].r);
                fails++;
                continue;
            }
            to_hex(dk, 32, h2);
            printf("%s pbkdf2-hmac-sha256 rounds %u\n", strcmp(h2, v[k].want) ? "FAIL" : "ok  ",
                   v[k].r);
            if (strcmp(h2, v[k].want)) fails++;
        }
    }
    {   /* ChaCha20, RFC 8439 section 2.4.2: the sunscreen plaintext/ciphertext
         * pair, and the keystream block it starts with, both reproduced by
         * OpenSSL's ChaCha20 for the same key, nonce and counter. */
        static const char *pt =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
            "for the future, sunscreen would be it.";
        static const char *want_ct =
            "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
            "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
            "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937"
            "365af90bbf74a35be6b40b8eedf2785e42874d";
        static const char *want_ks =
            "224f51f3401bd9e12fde276fb8631ded8c131f823d2c06e27e4fcaec9ef3cf78"
            "8a3b0aa372600a92b57974cded2b9334794cba40c63e34cdea212c4cf07d41b7";
        unsigned char key[32], nonce[S_NONCE], buf[512];
        char h2[2 * sizeof buf + 1];
        size_t plen = strlen(pt);
        for (i = 0; i < 32; i++) key[i] = (unsigned char)i;
        memset(nonce, 0, sizeof nonce);
        nonce[7] = 0x4a;
        memset(buf, 0, 64);
        chacha20_xor(key, nonce, 1, buf, 64);
        to_hex(buf, 64, h2);
        printf("%s chacha20 keystream, counter 1\n", strcmp(h2, want_ks) ? "FAIL" : "ok  ");
        if (strcmp(h2, want_ks)) fails++;
        memcpy(buf, pt, plen);
        chacha20_xor(key, nonce, 1, buf, plen);
        to_hex(buf, plen, h2);
        printf("%s chacha20 of the RFC 8439 plaintext\n", strcmp(h2, want_ct) ? "FAIL" : "ok  ");
        if (strcmp(h2, want_ct)) fails++;
    }
    {   /* seal -> open with the right passphrase, and fail with the wrong one */
        unsigned char boot[HALF], blob[SEAL_BYTES], dk[64], tag[FPRINT], got[HALF];
        const char *good = "correct horse battery staple";
        const char *bad = "correct horse battery stapl";
        unsigned diff;
        int k, ok = 1;
        for (k = 0; k < HALF; k++) boot[k] = (unsigned char)(0xA0 + k);
        memset(blob, 0, sizeof blob);
        memcpy(blob, SEAL_MAGIC, 8);
        put_le32(blob + O_S_VERSION, SEAL_VERSION);
        put_le32(blob + O_S_ROUNDS, 1000);   /* fast: the vectors above cover the KDF */
        if (seal_build(blob, boot, good, 1000) < 0) {
            printf("FAIL  seal_build failed\n");
            fails++;
            ok = 0;
        }
        if (ok) {
            pbkdf2_sha256(good, strlen(good), blob + O_S_SALT, S_SALT, 1000, dk, sizeof dk);
            hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, tag);
            diff = 0;
            for (k = 0; k < (int)FPRINT; k++) diff |= (unsigned)(tag[k] ^ blob[O_S_TAG + k]);
            printf("%s the sealed key authenticates with the right passphrase\n",
                   diff ? "FAIL" : "ok  ");
            if (diff) fails++;
            memcpy(got, blob + O_S_CIPHER, HALF);
            chacha20_xor(dk, blob + O_S_NONCE, 1, got, HALF);
            printf("%s it decrypts back to the same 32 bytes\n",
                   memcmp(got, boot, HALF) ? "FAIL" : "ok  ");
            if (memcmp(got, boot, HALF)) fails++;
            pbkdf2_sha256(bad, strlen(bad), blob + O_S_SALT, S_SALT, 1000, dk, sizeof dk);
            hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, tag);
            diff = 0;
            for (k = 0; k < (int)FPRINT; k++) diff |= (unsigned)(tag[k] ^ blob[O_S_TAG + k]);
            printf("%s the wrong passphrase does not authenticate\n", diff ? "ok  " : "FAIL");
            if (!diff) fails++;
            blob[O_S_CIPHER + 3] ^= 0x01;
            pbkdf2_sha256(good, strlen(good), blob + O_S_SALT, S_SALT, 1000, dk, sizeof dk);
            hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, tag);
            diff = 0;
            for (k = 0; k < (int)FPRINT; k++) diff |= (unsigned)(tag[k] ^ blob[O_S_TAG + k]);
            printf("%s one flipped ciphertext byte is detected\n", diff ? "ok  " : "FAIL");
            if (!diff) fails++;
            blob[O_S_CIPHER + 3] ^= 0x01;
            blob[O_S_ROUNDS] ^= 0x01;      /* tamper with the work factor too */
            pbkdf2_sha256(good, strlen(good), blob + O_S_SALT, S_SALT, 1001, dk, sizeof dk);
            hmac_sha256_parts(dk + 32, 32, blob, O_S_TAG, NULL, 0, tag);
            diff = 0;
            for (k = 0; k < (int)FPRINT; k++) diff |= (unsigned)(tag[k] ^ blob[O_S_TAG + k]);
            printf("%s a tampered round count is detected\n", diff ? "ok  " : "FAIL");
            if (!diff) fails++;
        }
        wipe(boot, sizeof boot);
        wipe(blob, sizeof blob);
        wipe(dk, sizeof dk);
    }
    {   /* the lock-screen password check, on the format used by
         * /etc/mystic/passwd: salt:rounds:hash. tools/mkenc.sh test also
         * cross-checks hash_password() against src/sha256.c itself. */
        unsigned char salt[4] = {0xde, 0xad, 0xbe, 0xef};
        unsigned char dg[FPRINT];
        char salthex[9], hashhex[FPRINT * 2 + 1], line[128];
        int yes, no_;
        hash_password(salt, sizeof salt, "mystic", 7, dg);
        to_hex(salt, sizeof salt, salthex);
        to_hex(dg, FPRINT, hashhex);
        snprintf(line, sizeof line, "%s:7:%s", salthex, hashhex);
        yes = check_password_line(line, "mystic");
        no_ = check_password_line(line, "mysti");
        printf("%s check_password_line() accepts the password it was derived from\n",
               yes == 1 ? "ok  " : "FAIL");
        if (yes != 1) fails++;
        printf("%s and rejects a one-character-shorter one\n", no_ == 0 ? "ok  " : "FAIL");
        if (no_ != 0) fails++;
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
            "  probe <dev>                       exit 0 crypt, 10 blank, 11 fs, 12 unknown\n"
            "  init <dev> <key-file>             write the MYSTICC1 header (blank disk only)\n"
            "  attach <name> <dev> <key-file>    create + load + resume the mapping\n"
            "                                    exit 2 key file, 3 wrong key, 4 dm error\n"
            "  detach <name>                     remove the mapping\n"
            "  status <name>                     exit 0 if the mapping exists\n"
            "  hasfs <dev>                       exit 0 mapping was formatted, 1 not, 2 no header\n"
            "  markfs <dev>                      record that a filesystem exists in the mapping\n"
            "  seal <key> <out> [--passwd-file P] [--rounds N]\n"
            "                                    passphrase (stdin) seals the 32-byte key\n"
            "  unseal <seal> <out> [--prompt [--console C]] [--passwd-file P]\n"
            "                                    exit 5 bad seal/passphrase, 6 bad file,\n"
            "                                    7 no passphrase, 8 not the lock-screen one\n"
            "  verify <passwd-file> [--prompt [--console C]]\n"
            "                                    check a passphrase against /etc/mystic/passwd\n"
            "  selftest                          hash / KDF / cipher / table / header checks\n",
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
        if (key_read(argv[2], half) != 0) return EXIT_KEYFILE;
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
        if (st == DEV_ERROR) return EXIT_BADARG;
        if (st == DEV_BLANK) {
            printf("blank\n");
            return EXIT_PROBE_BLANK;
        }
        if (st == DEV_FS) {
            printf("fs\n");
            return EXIT_PROBE_FS;
        }
        if (st == DEV_UNKNOWN) {
            printf("unknown\n");
            return EXIT_PROBE_UNKNOWN;
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
        if (st == DEV_ERROR) return EXIT_BADARG;
        if (st != DEV_BLANK)
            return die("%s already holds %s; init only writes to a blank device", argv[2],
                       st == DEV_CRYPT ? "an encrypted volume"
                                       : st == DEV_FS ? "a filesystem"
                                                      : "data that is not a filesystem");
        if (key_read(argv[3], boot) != 0) return EXIT_KEYFILE;
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
    if (!strcmp(cmd, "hasfs") && argc == 3) return cmd_hasfs(argv[2]);
    if (!strcmp(cmd, "markfs") && argc == 3) return cmd_markfs(argv[2]);
    if (!strcmp(cmd, "seal") && argc >= 4) {
        const char *passwd_file = NULL;
        uint32_t rounds = 0;
        int i;
        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--passwd-file") && i + 1 < argc) passwd_file = argv[++i];
            else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = (uint32_t)strtoul(argv[++i], NULL, 10);
            else return die("unknown seal option %s", argv[i], NULL);
        }
        return cmd_seal(argv[2], argv[3], passwd_file, rounds);
    }
    if (!strcmp(cmd, "unseal") && argc >= 4) {
        const char *passwd_file = NULL, *console_path = NULL;
        int prompt = 0, i;
        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--prompt")) prompt = 1;
            else if (!strcmp(argv[i], "--console") && i + 1 < argc) console_path = argv[++i];
            else if (!strcmp(argv[i], "--passwd-file") && i + 1 < argc) passwd_file = argv[++i];
            else return die("unknown unseal option %s", argv[i], NULL);
        }
        return cmd_unseal(argv[2], argv[3], prompt, console_path, passwd_file);
    }
    if (!strcmp(cmd, "verify") && argc >= 3) {
        int prompt = 0, i;
        const char *console_path = NULL;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--prompt")) prompt = 1;
            else if (!strcmp(argv[i], "--console") && i + 1 < argc) console_path = argv[++i];
            else return die("unknown verify option %s", argv[i], NULL);
        }
        return cmd_verify(argv[2], prompt, console_path);
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

# The passphrase scenarios need a /etc/mystic/passwd line. Making one with the
# shipping implementation (src/sha256.c) rather than a second copy of the
# algorithm is the point: the helper's own check then has to agree with the
# code the desktop lock screen runs, or the test fails.
build_pwline() {
    exe=$WORK/mystic-pwline.exe
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) ;;
        *) exe=$WORK/mystic-pwline ;;
    esac
    cat > "$WORK/mystic-pwline.c" <<'MYSTIC_PWLINE_C_EOF'
#include "mystic.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    unsigned char salt[16];
    char line[256];
    int i;
    if (argc != 3) { fprintf(stderr, "usage: pwline <password> <rounds>\n"); return 2; }
    for (i = 0; i < (int)sizeof salt; i++) salt[i] = (unsigned char)(0x10 + i * 7);
    mystic_make_password_line(salt, (int)sizeof salt, argv[1], atoi(argv[2]), line, sizeof line);
    printf("%s\n", line);
    return 0;
}
MYSTIC_PWLINE_C_EOF
    $ZIG cc -Isrc "$WORK/mystic-pwline.c" src/sha256.c -o "$exe" \
        || die "compiling src/sha256.c for the password cross-check failed"
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
        if [ -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
            warn "$STAGE/etc/mystic/luks.key.sealed is also present; the plaintext"
            warn "file wins at boot, so the seal is not doing anything. Remove the"
            warn "plaintext file (./tools/mkenc.sh seal) or the seal (./tools/mkenc.sh unseal)."
        fi
    elif [ -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
        # Creating a new plaintext key here would quietly defeat the seal and
        # leave a sealed blob that no longer matches the disk.
        say "key: using the sealed key $STAGE/etc/mystic/luks.key.sealed"
        say "     (the plaintext half is intentionally absent; the boot passphrase"
        say "     is the one in $STAGE/etc/mystic/passwd)"
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
        elf_verdict=$(elf_report "$STAGE/sbin/mystic-dmcrypt"); elf_rc=$?
        printf '    elf       : %s\n' "$elf_verdict"
        if [ "$elf_rc" -ne 0 ]; then
            warn "stage/sbin/mystic-dmcrypt is not a static executable (see above)"
            bad=1
        fi
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
    elif [ -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
        seal_bytes=$(wc -c < "$STAGE/etc/mystic/luks.key.sealed" | tr -d ' ')
        printf '    file      : %s\n' "$STAGE/etc/mystic/luks.key.sealed"
        printf '    length    : %s bytes (%s)\n' "$seal_bytes" \
            "$([ "$seal_bytes" = 112 ] && echo 'the only valid size' || echo 'WRONG: must be 112')"
        printf '    packed as : /etc/mystic/luks.key.sealed; the boot sequence asks for\n'
        printf '                the lock-screen passphrase and unseals it into RAM\n'
        head -c 8 "$STAGE/etc/mystic/luks.key.sealed" | grep -q 'MYSTICKS' \
            || { warn "the sealed key does not start with MYSTICKS"; bad=1; }
        [ "$seal_bytes" = 112 ] || bad=1
    else
        warn "no $STAGE/etc/mystic/luks.key and no luks.key.sealed"; bad=1
    fi
    return $bad
}

# ====================================================================
#  5. what the next boot will do
# ====================================================================
cmd_status() {
    DISKIMG=${MYSTIC_DISK_IMG:-build/disk.img}
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
    printf 'kernel modules : %s\n' "$KO_DST"
    for f in $MODULES; do
        [ -f "$KO_DST/$f" ] && printf '  present      : %s\n' "${f#kernel/}"                             || printf '  MISSING      : %s\n' "${f#kernel/}"
    done
    if [ -f "$STAGE/sbin/mystic-dmcrypt" ]; then
        printf 'helper         : stage/sbin/mystic-dmcrypt (%s bytes)\n'             "$(wc -c < "$STAGE/sbin/mystic-dmcrypt" | tr -d ' ')"
    else
        printf 'helper         : MISSING - run ./tools/mkenc.sh\n'
    fi
    if [ -f "$STAGE/etc/mystic/luks.key" ]; then
        printf 'boot key half  : %s (fp %s, plaintext)\n' \
            "$STAGE/etc/mystic/luks.key" "$(key_fingerprint)"
    elif [ -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
        seal_rounds=$(python -c "import sys,struct; d=open(sys.argv[1],'rb').read(); print(struct.unpack('<I', d[12:16])[0] if len(d)==112 else '?')" \
            "$STAGE/etc/mystic/luks.key.sealed" 2>/dev/null || echo '?')
        printf 'boot key half  : %s (sealed, %s PBKDF2-HMAC-SHA256 rounds)\n' \
            "$STAGE/etc/mystic/luks.key.sealed" "$seal_rounds"
        printf '                 the boot asks for the lock-screen passphrase\n'
        printf '                 in probe mode it cannot prompt, so /root stays unmounted\n'
    else
        printf 'boot key half  : MISSING\n'
    fi
    if [ ! -f "$DISKIMG" ]; then
        printf 'disk           : %s does not exist yet\n' "$DISKIMG"
        printf 'next boot      : creates it, writes the MYSTICC1 header, mkfs.ext4\n'
        printf '                 inside dm-crypt, mounts it on /root\n'
        return 0
    fi
    printf 'disk           : %s (%s bytes)\n' "$DISKIMG" "$(wc -c < "$DISKIMG" | tr -d ' ')"
    if "$HOST" probe "$DISKIMG" > "$WORK/status.probe" 2>&1; then
        # A valid header says nothing about the key. attach --table checks the
        # SHA-256 fingerprint in that header against the boot half and stops
        # before any ioctl, so this is the verdict the next boot will get.
        if [ ! -f "$STAGE/etc/mystic/luks.key" ]; then
            printf 'next boot      : will NOT open it - no boot key at %s\n'                 "$STAGE/etc/mystic/luks.key"
        elif "$HOST" attach mystic-root "$DISKIMG" "$STAGE/etc/mystic/luks.key" --table > "$WORK/status.table" 2>&1; then
            printf 'next boot      : unlocks it (%s)\n' "$(cat "$WORK/status.probe")"
            printf 'dm table       : %s\n' "$(cat "$WORK/status.table")"
        else
            printf 'next boot      : will NOT open it - %s\n' "$(cat "$WORK/status.table")"
        fi
    else
        case "$(cat "$WORK/status.probe")" in
            blank)
                printf 'next boot      : blank disk -> writes the header and formats it\n' ;;
            fs)
                printf 'next boot      : NO encryption header, but this is an ext4 disk:\n'
                printf '                 it boots unencrypted with the same messages as before.\n'
                printf '                 To encrypt, run ./tools/mkenc.sh reset-disk and let the\n'
                printf '                 next boot create a fresh volume.\n' ;;
            unknown)
                printf 'next boot      : NO header and no filesystem magic - the boot will\n'
                printf '                 REFUSE to format it. ./tools/mkenc.sh reset-disk to\n'
                printf '                 start over.\n' ;;
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
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
    printf 'file        : %s\n' "$STAGE/etc/mystic/luks.key"
    printf 'length      : %s bytes\n' "$(wc -c < "$STAGE/etc/mystic/luks.key" | tr -d ' ')"
    printf 'fingerprint : %s\n' "$(key_fingerprint)"
    printf 'a disk only opens with this half AND the half in its own MYSTICC1 header.\n'
}

cmd_new_key() {
    [ -f "$STAGE/etc/mystic/luks.key" ] || die "no key yet - run ./tools/mkenc.sh"
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
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

# ====================================================================
#  6b. the passphrase seal on the boot half
# ====================================================================
# Reads the passphrase without echoing it. MYSTIC_SEAL_PASS_FILE exists so
# that a harness or a scripted install can drive this; it is not for people,
# because a passphrase in a file on the build host is not a secret.
read_seal_passphrase() {   # read_seal_passphrase <why>
    pw=""
    pw2=""
    if [ -n "${MYSTIC_SEAL_PASS_FILE:-}" ]; then
        [ -f "$MYSTIC_SEAL_PASS_FILE" ] || die "MYSTIC_SEAL_PASS_FILE=$MYSTIC_SEAL_PASS_FILE does not exist"
        warn "reading the passphrase from $MYSTIC_SEAL_PASS_FILE (automation only)"
        pw=$(cat "$MYSTIC_SEAL_PASS_FILE")
        return 0
    fi
    [ -t 0 ] || die "no terminal to ask for the passphrase - set MYSTIC_SEAL_PASS_FILE for a scripted run"
    printf '%s (the lock-screen password): ' "$1" >&2
    IFS= read -r -s pw
    printf '\n' >&2
    if [ "$1" = "passphrase" ]; then
        printf 'again: ' >&2
        IFS= read -r -s pw2
        printf '\n' >&2
        [ "$pw" = "$pw2" ] || die "the two passphrases differ - nothing was written"
        pw2=""
    fi
    [ -n "$pw" ] || die "an empty passphrase is not a passphrase"
}

cmd_seal() {
    if [ ! -f "$STAGE/etc/mystic/luks.key" ]; then
        if [ -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
            die "already sealed: $STAGE/etc/mystic/luks.key.sealed (run ./tools/mkenc.sh unseal to undo)"
        fi
        die "no key at $STAGE/etc/mystic/luks.key - run ./tools/mkenc.sh first"
    fi
    [ -f "$STAGE/etc/mystic/passwd" ] || die "no $STAGE/etc/mystic/passwd - \
the lock screen has no password, so a sealed key could never be opened. \
Set one in Settings > Users first."
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
    read_seal_passphrase passphrase
    printf '%s\n' "$pw" | "$HOST" verify "$STAGE/etc/mystic/passwd" >/dev/null \
        || die "that is not the lock-screen password - nothing was sealed"
    fp=$(key_fingerprint)
    if [ -n "${1:-}" ]; then
        printf '%s\n' "$pw" | "$HOST" seal "$STAGE/etc/mystic/luks.key" \
            "$STAGE/etc/mystic/luks.key.sealed" --passwd-file "$STAGE/etc/mystic/passwd" \
            --rounds "$1" \
            || die "sealing failed - $STAGE/etc/mystic/luks.key is unchanged"
    else
        printf '%s\n' "$pw" | "$HOST" seal "$STAGE/etc/mystic/luks.key" \
            "$STAGE/etc/mystic/luks.key.sealed" --passwd-file "$STAGE/etc/mystic/passwd" \
            || die "sealing failed - $STAGE/etc/mystic/luks.key is unchanged"
    fi
    pw=""
    rm -f "$STAGE/etc/mystic/luks.key"
    say "sealed: $STAGE/etc/mystic/luks.key.sealed (key fp $fp)"
    say "the plaintext half is gone; the next boot asks for the passphrase before"
    say "mounting /root. Rebuild the image (./image.sh) so the initramfs gets it."
    warn "an encrypted disk keeps opening: the seal holds the same 32 bytes."
    warn "changing the lock-screen password does NOT re-seal: run seal again after it."
}

cmd_unseal() {   # put the plaintext half back and turn the boot prompt off
    [ -f "$STAGE/etc/mystic/luks.key.sealed" ] \
        || die "no $STAGE/etc/mystic/luks.key.sealed - nothing is sealed"
    [ ! -f "$STAGE/etc/mystic/luks.key" ] \
        || die "$STAGE/etc/mystic/luks.key already exists - move it away first"
    HOST=$(build_host_helper)
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
    read_seal_passphrase passphrase
    printf '%s\n' "$pw" | "$HOST" unseal "$STAGE/etc/mystic/luks.key.sealed" \
        "$STAGE/etc/mystic/luks.key" --passwd-file "$STAGE/etc/mystic/passwd" \
        || die "could not unseal - nothing was changed"
    pw=""
    rm -f "$STAGE/etc/mystic/luks.key.sealed"
    say "unsealed: $STAGE/etc/mystic/luks.key (fp $(key_fingerprint))"
    say "the boot no longer asks for a passphrase; the initramfs carries the key"
}

cmd_reset_disk() {
    # Moving a disk away is only safe advice if the next boot can open the new
    # one. After a git clean (stage/sbin/ and stage/lib/modules/ are ignored)
    # it cannot, and a fresh disk would come back unencrypted without a word.
    missing=""
    if [ ! -f "$STAGE/etc/mystic/luks.key" ] && [ ! -f "$STAGE/etc/mystic/luks.key.sealed" ]; then
        missing="$missing key"
    fi
    [ -f "$STAGE/sbin/mystic-dmcrypt" ] || missing="$missing helper"
    for f in $MODULES; do
        [ -f "$KO_DST/$f" ] || missing="$missing $(basename "$f")"
    done
    if [ -n "$missing" ]; then
        die "run ./tools/mkenc.sh first: missing$missing - the new disk would come back unencrypted"
    fi
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
    PWL=$(build_pwline)
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb" "$PWL" "${PWL%.exe}.pdb"' EXIT
    say "selftest of the host build (hash vectors, table string, header codec)"
    "$HOST" selftest || die "host selftest failed"
    SIM=$WORK/sim
    rm -rf "$SIM"
    mkdir -p "$SIM"
    # The passphrase used by the sealed-key scenarios, and the passwd line for
    # it, generated by src/sha256.c (the lock screen's own code).
    SIM_PASS='correct horse battery staple'
    "$PWL" "$SIM_PASS" 20000 > "$WORK/sim.passwd" || die "pwline failed"
    say "passphrase cross-check: $WORK/sim.passwd was written by src/sha256.c"
    cat > "$SIM/run.sh" <<'MYSTIC_SIM_EOF'
#!/bin/sh
# generated by: ./tools/mkenc.sh test
#
# Boots stage/init's disk logic with no kernel, no QEMU and no device
# mapper. What is real here: stage/init itself (sourced, not copied), the
# host build of stage/sbin/mystic-dmcrypt (so the MYSTICC1 header, the key
# check and the SHA-256 fingerprint are the shipping code), and the bytes on
# the fake disks - the fake mkfs writes a real ext4 superblock magic into the
# device and the fake mount decides from that magic, so a format is visible
# to cksum and "there is a filesystem but it will not mount" is a state the
# simulation can actually reach. What is fake: the block device is a regular
# file, mount/mkfs/modprobe are shell scripts, and the three dm ioctls never
# run (they need a kernel). `disk_present` is replaced for every scenario
# except the first, because a Windows build host has no block devices at all;
# scenario 1 keeps the real [ -b ] check.
set -u

ROOT=${ROOT:?}
SIM=${SIM:?}
DMCRYPT=${SIM_DMCRYPT:?}
INIT=$ROOT/stage/init
BIN=$SIM/bin
DEV=$SIM/dev
ST=$SIM/state
MAP=$SIM/mapping          # stands in for /dev/mapper/mystic-root

pass=0
fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass + 1)); }
no()  { printf '  FAIL  %s\n' "$1"; fail=$((fail + 1)); }
check()     { if printf '%s\n' "$out" | grep -qF -- "$1"; then ok "$1"; else no "expected: $1"; fi; }
check_not() { if printf '%s\n' "$out" | grep -qF -- "$1"; then no "unexpected: $1"; else ok "no '$1'"; fi; }
same()      { if [ "$(cksum "$1")" = "$2" ]; then ok "$3"; else no "$3"; fi; }

rm -rf "$BIN" "$DEV" "$ST"
mkdir -p "$BIN" "$DEV" "$ST"

# ---- fake tools -----------------------------------------------------
cat > "$BIN/mount" <<'FMT_EOF'
#!/bin/sh
# fake mount: succeeds only if the device itself carries the ext4 superblock
# magic at byte 1080 - the first thing a real mount checks. FAKE_MOUNT_FAIL
# forces the "filesystem is there but will not mount" case.
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
[ -f "$FAKE_MOUNT_FAIL" ] && { echo "mount: $dev: I/O error" >&2; exit 32; }
[ "$(od -v -An -tx1 -j 1080 -N 2 "$dev" 2>/dev/null | tr -d '[:space:]')" = "53ef" ] || {
    echo "mount: $dev: wrong fs type, bad option, bad superblock" >&2; exit 32; }
printf '%s' "$dev" > "$FAKE_STATE/mounted"
echo "fake-mount: $dev on ${tgt:-?}"
exit 0
FMT_EOF

cat > "$BIN/mkfs.ext4" <<'FMT_EOF'
#!/bin/sh
# fake mkfs.ext4 -q -F <dev>: writes the real ext2/3/4 superblock magic
# (0x53EF at byte 1080) into the device, so the change is on the disk and a
# cksum comparison can see it.
dev=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) [ -z "$dev" ] && dev=$a ;;
    esac
done
[ -n "$dev" ] || { echo "fake mkfs.ext4: no device" >&2; exit 2; }
printf '%s' "$dev" | python -c '
import os, sys
p = sys.stdin.read().strip()
f = open(p, "r+b" if os.path.exists(p) else "wb")
f.seek(1080)
f.write(bytes([0x53, 0xEF]))
f.close()
' || { echo "fake mkfs.ext4: cannot write the superblock" >&2; exit 1; }
echo "fake-mkfs.ext4: ext4 superblock written at byte 1080 of $dev"
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
    : > "$MAP"
}
plain_disk() {   # a disk an older build formatted: ext4 magic, no header
    python - "$1" <<'PYEOF'
import sys
d = bytearray(64 * 1024)
d[0x438] = 0x53          # ext2/3/4 superblock magic 0x53EF at byte 1080
d[0x439] = 0xef
open(sys.argv[1], 'wb').write(d)
PYEOF
}
mount_will_fail() { : > "$ST/mount-fails"; }
ext4_inside() {   # the decrypted view of a volume that already holds ext4
    python - "$MAP" <<'PYEOF'
import sys, os
f = open(sys.argv[1], 'r+b' if os.path.exists(sys.argv[1]) else 'wb')
f.seek(1080)
f.write(bytes([0x53, 0xEF]))
f.close()
PYEOF
}
typed() { printf '%s\n' "$1" > "$ST/console"; }   # stands in for /dev/console

out=""
run_setup() {    # run_setup [real-block-check 0|1] [helper-path]
    real=${1:-0}
    helper=${2:-$DMCRYPT}
    out=$(
        export PATH="$BIN:$PATH"
        export MYSTIC_INIT_LIB=1 MYSTIC_DISK="$DISK" MYSTIC_KEY="$KEY" \
               MYSTIC_DMCRYPT="$helper" MYSTIC_DM_CONTROL="$ST/control" \
               MYSTIC_MKFS="$MKFS_BIN" MYSTIC_CRYPT_LOG="$ST/crypt.log" \
               MYSTIC_MAP=mystic-root MYSTIC_MAP_PATH="$MAP" \
               MYSTIC_SEAL="$SEAL" MYSTIC_RAMKEY="$ST/ram/luks.key" \
               MYSTIC_CONSOLE="$CONSOLE" MYSTIC_PASSWD="$PASSWD" \
               MYSTIC_CMDLINE="$CMDLINE" \
               FAKE_STATE="$ST" FAKE_MOUNT_FAIL="$ST/mount-fails"
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

# the passphrase side: a /etc/mystic/passwd line made by the SHIPPING code
# (src/sha256.c), a sealed copy of the same boot half, and the paths init uses
PASSWD=${SIM_PASSWD:-}
PASSPHRASE=${SIM_PASS:-}
SEAL=$SIM/luks.key.sealed
CONSOLE=$ST/console
CMDLINE=$SIM/cmdline
MKFS_BIN=$BIN/mkfs.ext4
if [ -n "$PASSWD" ] && [ -f "$PASSWD" ]; then
    rm -f "$SEAL" "$SIM/sealkey"
    "$DMCRYPT" keygen "$SIM/sealkey" >/dev/null 2>&1
    printf '%s\n' "$PASSPHRASE" | "$DMCRYPT" seal "$SIM/sealkey" "$SEAL" \
        --passwd-file "$PASSWD" > "$SIM/seal.log" 2>&1 \
        || { echo "cannot seal the test key:" >&2; cat "$SIM/seal.log" >&2; exit 1; }
    echo "seal: $(cat "$SIM/seal.log")"
else
    echo "seal: no passwd file supplied, the sealed-key scenarios are skipped" >&2
fi
: > "$CMDLINE"

# ---------------------------------------------------------------- 1
echo
echo "== 1. no disk at all (real [ -b ] check) =="
DISK=/dev/vda
reset_boot
run_setup 1
show
check "disk: no /dev/vda"
check_not "fake-mount"

# ---------------------------------------------------------------- 2
echo
echo "== 2. first boot on a blank disk =="
DISK=$DEV/vda
new_disk "$DISK" 8192
reset_boot
run_setup
show
check "disk: created dm-crypt volume on $DISK (aes-xts-plain64)"
check "disk: dm-crypt unlocked, no filesystem inside - creating one"
check "disk: formatted ext4 inside dm-crypt and mounted on /root"
check "mounted: $MAP"
check "status: aes-xts-plain64, key "
check "crypt: marked $DISK: a filesystem was created inside its mapping"
hdr=$("$DMCRYPT" probe "$DISK" 2>&1)
printf '        | header now: %s\n' "$hdr"
case "$hdr" in
    "crypt 1 "*) ok "the disk file now starts with a MYSTICC1 header" ;;
    *)           no "the disk file has no MYSTICC1 header ($hdr)" ;;
esac
if [ "$(od -v -An -tx1 -j 1080 -N 2 "$MAP" | tr -d '[:space:]')" = "53ef" ]; then
    ok "the ext4 superblock was written inside the mapping"
else
    no "no ext4 superblock inside the mapping"
fi
if [ "$(od -v -An -tx1 -j 1080 -N 2 "$DISK" | tr -d '[:space:]')" = "0000" ]; then
    ok "the raw disk has no ext4 superblock at byte 1080 (ciphertext starts there)"
else
    no "an ext4 superblock is visible on the raw disk"
fi

# ---------------------------------------------------------------- 3
echo
echo "== 3. second boot on the same disk =="
reset_boot
run_setup
show
check "disk: dm-crypt unlocked, formatted ext4 mounted on /root"
check "mounted: $MAP"
check_not "created dm-crypt volume"
check_not "no filesystem inside"

# ---------------------------------------------------------------- 4
echo
echo "== 4. a plain ext4 disk from an older build =="
DISK=$DEV/plain
plain_disk "$DISK"
reset_boot
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK has no encryption header, so it is not encrypted"
check "disk: mounted $DISK (ext4) on /root"
check_not "created dm-crypt volume"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the plain disk was not written to"

# ---------------------------------------------------------------- 5
echo
echo "== 5. encrypted disk, dm modules missing =="
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
same "$DISK" "$before" "the encrypted disk was not written to"

# ---------------------------------------------------------------- 6
echo
echo "== 6. encrypted disk, wrong key =="
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
same "$DISK" "$before" "the encrypted disk was not written to"
KEY=$SIM/luks.key

# ---------------------------------------------------------------- 7
echo
echo "== 7. blank disk, no key in the image at all =="
DISK=$DEV/fresh
new_disk "$DISK" 8192
KEY=$SIM/absent.key
SEAL=$SIM/no-seal-either
rm -f "$KEY"
reset_boot
run_setup
show
check "disk: this image has no boot key, so $DISK stays UNENCRYPTED"
check "disk: formatted ext4 and mounted on /root"
check_not "created dm-crypt volume"
KEY=$SIM/luks.key
SEAL=$SIM/luks.key.sealed

# ---------------------------------------------------------------- 8
echo
echo "== 8. no helper in the image, blank disk =="
DISK=$DEV/fresh
new_disk "$DISK" 8192
reset_boot
run_setup 0 "$SIM/no-such-helper"
show
check "disk: dm-crypt unavailable - no $SIM/no-such-helper in the image"
check "disk: $DISK is blank, so this creates an UNENCRYPTED /root"
check "disk: formatted ext4 and mounted on /root"
check_not "created dm-crypt volume"

# ---------------------------------------------------------------- 9
echo
echo "== 9. encrypted volume whose ext4 was never created =="
DISK=$DEV/emptyvol
new_disk "$DISK" 8192
reset_boot
"$DMCRYPT" init "$DISK" "$KEY" >/dev/null      # a header...
rm -f "$MAP"                                   # ...and no filesystem inside it
run_setup
show
check "disk: dm-crypt unlocked, no filesystem inside - creating one"
check "disk: formatted ext4 inside dm-crypt and mounted on /root"

# ---------------------------------------------------------------- 10
echo
echo "== 10. encrypted volume with an ext4 that will not mount =="
DISK=$DEV/vda
reset_boot
mount_will_fail
before=$(cksum "$MAP")
run_setup
show
check "disk: dm-crypt unlocked, ext4 is there but did not mount - refusing to format it"
check "disk: nothing was written; the volume is intact"
check_not "no filesystem inside"
check_not "formatted ext4"
same "$MAP" "$before" "the unmountable volume was not reformatted"

# ---------------------------------------------------------------- 11
echo
echo "== 11. header present, helper missing (a git clean removes stage/sbin) =="
DISK=$DEV/vda
reset_boot
before=$(cksum "$DISK")
run_setup 0 "$SIM/no-such-helper"
show
check "disk: $DISK is encrypted but cannot be opened - no $SIM/no-such-helper in the image, and $DISK carries a MYSTICC1 header"
check "disk: refusing to format it, /root stays on the RAM filesystem"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the encrypted disk was not written to"

# ---------------------------------------------------------------- 12
echo
echo "== 12. header with a corrupted version byte =="
DISK=$DEV/badver
new_disk "$DISK" 8192
"$DMCRYPT" init "$DISK" "$KEY" >/dev/null
python -c "
import sys
f = open(sys.argv[1], 'r+b'); f.seek(8); f.write(bytes([0xFF])); f.close()" "$DISK"
reset_boot
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK is encrypted but cannot be opened - the probe failed, and $DISK carries a MYSTICC1 header"
check "disk: refusing to format it, /root stays on the RAM filesystem"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the unreadable volume was not written to"

# ---------------------------------------------------------------- 13
echo
echo "== 13. a disk that is neither blank, nor ext4, nor ours =="
DISK=$DEV/junk
python -c "
import sys
open(sys.argv[1], 'wb').write(bytes([0xAA]) * 8192)" "$DISK"
reset_boot
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK is not blank, not ext4 and carries no MYSTICC1 header"
check "disk: $DISK did not mount and is not blank - refusing to format it"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the unrecognised disk was not written to"

# ---------------------------------------------------------------- 14
echo
echo "== 14. helper present, dm-crypt fails to create the mapping =="
DISK=$DEV/vda
cp "$DMCRYPT" "$SIM/fail-dm.real"
cat > "$SIM/fail-dm" <<'FMT_EOF'
#!/bin/sh
# a helper whose dm ioctl fails, exactly like a missing /dev/mapper/control
case "$1" in
    probe) exec "$0.real" "$@" ;;
esac
echo "mystic-dmcrypt: dm DM_DEV_CREATE failed: Operation not permitted" >&2
exit 4
FMT_EOF
chmod +x "$SIM/fail-dm"
reset_boot
run_setup 0 "$SIM/fail-dm"
show
check "disk: dm-crypt could not create the mapping - $DISK is left untouched"
check "status: locked (dm-crypt error)"
check_not "key did not unlock it"
check_not "key does not match"

# ---------------------------------------------------------------- 15
echo
echo "== 15. encrypted disk whose boot key file is gone =="
DISK=$DEV/vda
reset_boot
KEY=$SIM/gone.key
SEAL=$SIM/no-seal-either
rm -f "$KEY"
before=$(cksum "$DISK")
run_setup
show
check "disk: no key at $KEY and no sealed key at $SEAL"
check "disk: refusing to touch $DISK, /root stays on the RAM filesystem"
check "status: locked (no key)"
check_not "key does not match"
same "$DISK" "$before" "the encrypted disk was not written to"
KEY=$SIM/luks.key
SEAL=$SIM/luks.key.sealed

echo
echo "== 16. an unrecognised disk with no helper in the image =="
DISK=$DEV/junk2
python -c "import sys; open(sys.argv[1], 'wb').write(bytes([0xAA]) * 8192)" "$DISK"
reset_boot
before=$(cksum "$DISK")
run_setup 0 "$SIM/no-such-helper"
show
check "disk: no $SIM/no-such-helper in the image, and $DISK is not blank and carries no MYSTICC1 header"
check "disk: $DISK did not mount and is not blank - refusing to format it"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the unrecognised disk was not written to"

# ---------------------------------------------------------------- 17
# P0-2 on the plain path: the old code formatted whenever the mount failed,
# including a real ext4 that merely would not mount.
echo
echo "== 17. a plain ext4 disk whose mount fails = "
DISK=$DEV/plainsick
plain_disk "$DISK"
reset_boot
mount_will_fail
before=$(cksum "$DISK")
run_setup
show
check "disk: $DISK has no encryption header, so it is not encrypted"
check "disk: $DISK has an ext4 superblock but did not mount - refusing to format it"
check_not "creating ext4 filesystem"
same "$DISK" "$before" "the unmountable plain ext4 was not reformatted"

# ---------------------------------------------------------------- 18
# P0-1's last hole: the helper writes the header, then reports failure.
# The header is on the disk from that moment and mkfs must never follow it.
echo
echo "== 18. the helper writes the header, then reports failure =="
DISK=$DEV/halffail
new_disk "$DISK" 8192
cp "$DMCRYPT" "$SIM/init-then-fail.real"
cat > "$SIM/init-then-fail" <<'FMT_EOF'
#!/bin/sh
# a real 'init' (so a real header lands on the disk), followed by the failure
# that used to send stage/init down the "using plain ext4" path
case "$1" in
    init) "$0.real" "$@"; rc=$?
          [ "$rc" -eq 0 ] && { echo "mystic-dmcrypt: simulated failure after writing the header" >&2; exit 1; }
          exit "$rc" ;;
    *) exec "$0.real" "$@" ;;
esac
FMT_EOF
chmod +x "$SIM/init-then-fail" "$SIM/init-then-fail.real"
reset_boot
run_setup 0 "$SIM/init-then-fail"
show
check "disk: could not create the encrypted volume - $DISK is left untouched"
check "disk: the MYSTICC1 header is on the disk; it will not be formatted"
check "status: not created (header written, mapping failed)"
check_not "creating ext4 filesystem"
case "$("$DMCRYPT" probe "$DISK" 2>&1)" in
    "crypt 1 "*) ok "the header is still on the disk after the failure" ;;
    *)           no "the header was destroyed by a plain mkfs" ;;
esac

# ---------------------------------------------------------------- 19
if [ -f "$SEAL" ]; then
echo
echo "== 19. sealed key, correct passphrase, first boot =="
DISK=$DEV/sealednew
new_disk "$DISK" 8192
KEY=$SIM/no-plaintext.key     # a sealed image has no plaintext half
reset_boot
typed "$PASSPHRASE"
run_setup
show
check "key: unsealed $SEAL into $ST/ram/luks.key (RAM only)"
check "key: $ST/ram/luks.key (unsealed earlier in this boot)"
check "disk: created dm-crypt volume on $DISK (aes-xts-plain64)"
check "disk: formatted ext4 inside dm-crypt and mounted on /root"
check "mounted: $MAP"
if [ -f "$ST/ram/luks.key" ]; then
    no "the unsealed key is still on the ramfs after attach"
else
    ok "the unsealed key was wiped after attach"
fi

# ---------------------------------------------------------------- 20
echo
echo "== 20. sealed key, wrong passphrase =="
DISK=$DEV/sealedwrong
new_disk "$DISK" 8192
KEY=$SIM/no-plaintext.key
reset_boot
typed "this is not the password"
before=$(cksum "$DISK")
run_setup
show
check "disk: that is not the lock-screen password (attempt 1 of 3)"
check "disk: giving up after 3 attempts - the volume stays locked"
check "status: locked (passphrase refused)"
check_not "created dm-crypt volume"
same "$DISK" "$before" "the blank disk was not written to"

# ---------------------------------------------------------------- 21
echo
echo "== 21. sealed key, no console to prompt on =="
DISK=$DEV/vda
reset_boot
CONSOLE=$SIM/no-console
before=$(cksum "$DISK")
run_setup
show
check "disk: $SEAL needs the passphrase, and there is no console at $SIM/no-console"
check "status: locked (no usable key)"
check_not "created dm-crypt volume"
same "$DISK" "$before" "the encrypted disk was not written to"
CONSOLE=$ST/console

# ---------------------------------------------------------------- 22
echo
echo "== 22. sealed key, probe mode (nobody there to type) =="
printf 'mystic.probe=1\n' > "$CMDLINE"
reset_boot
typed "$PASSPHRASE"
before=$(cksum "$DISK")
run_setup
show
check "disk: $SEAL needs the passphrase; probe mode does not prompt"
check "status: locked (no usable key)"
same "$DISK" "$before" "the encrypted disk was not written to"
: > "$CMDLINE"

# ---------------------------------------------------------------- 23
echo
echo "== 23. the header says a filesystem was created, but no ext4 magic =="
# A volume created normally (so the header records the filesystem), then its
# ext4 magic zeroed: this is what a damaged or non-ext4 filesystem inside the
# volume looks like, and it must not be reformatted.
DISK=$DEV/marked
KEY=$SIM/luks.key
new_disk "$DISK" 8192
reset_boot
run_setup >/dev/null 2>&1          # create it, marker included
if "$DMCRYPT" hasfs "$DISK"; then
    ok "a freshly created volume records its filesystem in the header"
else
    no "a freshly created volume does not record its filesystem"
fi
python - "$MAP" <<'PYEOF'
import sys
f = open(sys.argv[1], 'r+b')
f.seek(1080)
f.write(bytes(2))
f.close()
PYEOF
reset_boot
before=$(cksum "$MAP")
run_setup
show
check "disk: dm-crypt unlocked, no ext4 magic, but its header records that a"
check "disk: filesystem was created in it once - refusing to format it"
check_not "no filesystem inside"
same "$MAP" "$before" "the volume was not reformatted"

# ---------------------------------------------------------------- 24
echo
echo "== 24. sealed key, second boot on the volume it created =="
DISK=$DEV/sealednew               # created in 19 with the sealed key
KEY=$SIM/no-plaintext.key
reset_boot
typed "$PASSPHRASE"
ext4_inside                       # the volume still holds its ext4
run_setup
show
check "disk: dm-crypt unlocked, formatted ext4 mounted on /root"
check "mounted: $MAP"
check_not "created dm-crypt volume"
KEY=$SIM/luks.key
fi   # SEAL exists

# ---------------------------------------------------------------- 25
echo
echo "== 25. no mkfs.ext4 in the image, blank disk = "
DISK=$DEV/nomkfs
new_disk "$DISK" 8192
reset_boot
MKFS_BIN=$SIM/no-mkfs
run_setup
show
check "disk: no mkfs.ext4 in the image"
check "disk: mkfs.ext4 failed inside the mapping - nothing is mounted"
MKFS_BIN=$BIN/mkfs.ext4

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ] || exit 1
exit 0
MYSTIC_SIM_EOF
    say "boot-path simulation: stage/init + host helper + fake /dev"
    ROOT=$(pwd) SIM="$SIM" SIM_DMCRYPT="$HOST" SIM_PASSWD="$WORK/sim.passwd" \
        SIM_PASS="$SIM_PASS" sh "$SIM/run.sh" \
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
    trap 'rm -f "$HOST" "${HOST%.exe}.pdb"' EXIT
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
    seal)       shift; cmd_seal "$@" ;;
    unseal)     cmd_unseal ;;
    reset-disk) cmd_reset_disk ;;
    elf)        shift; elf_report "${1:?usage: mkenc.sh elf <file>}" ;;
    -h|--help|help)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        ;;
    *) die "unknown command '$1' (try --help)" ;;
esac
