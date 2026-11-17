/* ============================================================
 *  Mystic OS - sha256.c
 *  SHA-256, FIPS 180-4. Written out rather than linked, so the OS
 *  has no dependency for the one thing that must never be wrong.
 *
 *  Used for the lock screen password file:
 *      /etc/mystic/passwd = "<salt-hex>:<iterations>:<hash-hex>"
 *  The stored value is SHA-256 iterated N times over salt||password,
 *  which is slow enough to make guessing tedious but small enough to
 *  read and check by hand.
 * ============================================================ */
#include "mystic.h"

#include <stdio.h>
#include <string.h>

static const u32 K[64] = {
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
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(u32 h[8], const u8 block[64]) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; i++) {
        u32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K[i] + w[i];
        u32 S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_raw(const u8 *data, size_t len, u8 out[32]) {
    u32 h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) compress(h, data + i * 64);

    u8 tail[128];
    size_t rem = len % 64;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    u64 bits = (u64)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (u8)(bits >> (8 * i));
    compress(h, tail);
    if (tail_len == 128) compress(h, tail + 64);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (u8)(h[i] >> 24);
        out[i * 4 + 1] = (u8)(h[i] >> 16);
        out[i * 4 + 2] = (u8)(h[i] >> 8);
        out[i * 4 + 3] = (u8)(h[i]);
    }
}

void sha256(const void *data, size_t len, u8 out[32]) {
    sha256_raw((const u8 *)data, len, out);
}

static void hex_encode(const u8 *in, int n, char *out) {
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *in, u8 *out, int max) {
    int n = 0;
    while (in[0] && in[1] && n < max) {
        int hi = hex_val(in[0]), lo = hex_val(in[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (u8)((hi << 4) | lo);
        in += 2;
    }
    return n;
}

/* salt||password, hashed `rounds` times over the previous digest */
void mystic_hash_password(const u8 *salt, int salt_len, const char *password,
                          int rounds, u8 out[32]) {
    u8 buf[64 + 32];
    size_t plen = strlen(password);
    if (plen > 64) plen = 64;
    memcpy(buf, salt, (size_t)salt_len);
    memcpy(buf + salt_len, password, plen);
    sha256_raw(buf, (size_t)salt_len + plen, out);
    for (int i = 1; i < rounds; i++) {
        memcpy(buf, out, 32);
        memcpy(buf + 32, salt, (size_t)salt_len);
        sha256_raw(buf, 32 + (size_t)salt_len, out);
    }
}

/* Verify a password against a line of "/etc/mystic/passwd".
 * Returns 1 on success, 0 on mismatch or unreadable file. */
int mystic_check_password(const char *line, const char *password) {
    if (!line || !password) return 0;
    char salt_hex[80] = {0};
    const char *p = line;
    int i = 0;
    while (*p && *p != ':' && i < (int)sizeof(salt_hex) - 1) salt_hex[i++] = *p++;
    if (*p != ':') return 0;
    p++;
    int rounds = 0;
    while (*p >= '0' && *p <= '9') { rounds = rounds * 10 + (*p - '0'); p++; }
    if (*p != ':') return 0;
    p++;
    if (rounds <= 0) rounds = 1;

    u8 salt[32];
    int salt_len = hex_decode(salt_hex, salt, 32);
    if (salt_len <= 0) return 0;

    u8 want[32];
    if (hex_decode(p, want, 32) != 32) return 0;

    u8 got[32];
    mystic_hash_password(salt, salt_len, password, rounds, got);

    /* constant time compare: the loop always runs the full length */
    unsigned diff = 0;
    for (int k = 0; k < 32; k++) diff |= (unsigned)(got[k] ^ want[k]);
    return diff == 0;
}

void mystic_make_password_line(const u8 *salt, int salt_len, const char *password,
                               int rounds, char *out, size_t outsz) {
    u8 digest[32];
    mystic_hash_password(salt, salt_len, password, rounds, digest);
    char salt_hex[80];
    char hash_hex[80];
    hex_encode(salt, salt_len, salt_hex);
    hex_encode(digest, 32, hash_hex);
    snprintf(out, outsz, "%s:%d:%s", salt_hex, rounds, hash_hex);
}
