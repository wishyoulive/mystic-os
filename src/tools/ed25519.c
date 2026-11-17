/* ============================================================
 *  src/tools/ed25519.c - RFC 8032 Ed25519 signature verification
 *
 *  A standalone verifier with no dependencies. Built static and
 *  installed as /usr/bin/ed25519-verify, where pkg calls it before
 *  it will use a repository index:
 *
 *      ed25519-verify <pubkey> <message> <signature>
 *
 *  exit 0  signature is valid for this key and message
 *  exit 1  signature is not valid
 *  exit 2  bad usage / unreadable file / malformed key or signature
 *
 *  The public key file is either 32 raw bytes or 64 hex digits
 *  (an optional trailing newline is accepted). The signature file
 *  is exactly 64 raw bytes: R || S, little-endian, as published by
 *  tools/sign-index.py.
 *
 *  This file is deliberately self-contained: SHA-512, the field
 *  arithmetic over 2^255-19 and the group law are all here, so
 *  there is no library to trust and nothing to link. The
 *  arithmetic follows the public-domain TweetNaCl representation
 *  (16 limbs of 16 bits in signed 64-bit integers) because it is
 *  small enough to audit and needs no 128-bit integer support.
 *
 *  Correctness is not asserted, it is checked: --selftest runs the
 *  RFC 8032 section 7.1 vectors plus negative cases, and the build
 *  is cross-checked against an independent Python implementation.
 * ============================================================ */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint64_t u64;
typedef int64_t  i64;

#define FOR(i, n) for (i = 0; i < (i64)(n); ++i)

/* ------------------------------------------------------------------
 * SHA-512 (FIPS 180-4)
 * ------------------------------------------------------------------ */

static const u64 K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

typedef struct {
    u64    h[8];
    u64    bytes;          /* total message length, exact below 2^61 bytes */
    u8     buf[128];
    size_t fill;
} Sha512;

static u64 rotr64(u64 x, unsigned n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(Sha512 *c, const u8 *p)
{
    u64 w[80];
    u64 a, b, cc, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u64)p[8 * i + 0] << 56) | ((u64)p[8 * i + 1] << 48) |
               ((u64)p[8 * i + 2] << 40) | ((u64)p[8 * i + 3] << 32) |
               ((u64)p[8 * i + 4] << 24) | ((u64)p[8 * i + 5] << 16) |
               ((u64)p[8 * i + 6] << 8)  | ((u64)p[8 * i + 7]);
    }
    for (i = 16; i < 80; i++) {
        u64 s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6];  h = c->h[7];

    for (i = 0; i < 80; i++) {
        u64 S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        u64 ch = (e & f) ^ ((~e) & g);
        u64 t1 = h + S1 + ch + K512[i] + w[i];
        u64 S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        u64 mj = (a & b) ^ (a & cc) ^ (b & cc);
        u64 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

static void sha512_init(Sha512 *c)
{
    c->h[0] = 0x6a09e667f3bcc908ULL;
    c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL;
    c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL;
    c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL;
    c->h[7] = 0x5be0cd19137e2179ULL;
    c->bytes = 0;
    c->fill = 0;
}

static void sha512_update(Sha512 *c, const void *data, size_t n)
{
    const u8 *p = (const u8 *)data;

    c->bytes += (u64)n;
    if (c->fill) {
        size_t take = 128 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill == 128) {
            sha512_block(c, c->buf);
            c->fill = 0;
        }
    }
    while (n >= 128) {
        sha512_block(c, p);
        p += 128;
        n -= 128;
    }
    if (n) {
        memcpy(c->buf, p, n);
        c->fill = n;
    }
}

static void sha512_final(Sha512 *c, u8 out[64])
{
    u64 bits = c->bytes << 3;
    int i;

    c->buf[c->fill++] = 0x80;
    if (c->fill > 112) {
        memset(c->buf + c->fill, 0, 128 - c->fill);
        sha512_block(c, c->buf);
        c->fill = 0;
    }
    memset(c->buf + c->fill, 0, 112 - c->fill);
    memset(c->buf + 112, 0, 8);              /* high 64 bits of the length */
    for (i = 0; i < 8; i++)
        c->buf[120 + i] = (u8)(bits >> (56 - 8 * i));
    sha512_block(c, c->buf);

    for (i = 0; i < 8; i++) {
        out[8 * i + 0] = (u8)(c->h[i] >> 56);
        out[8 * i + 1] = (u8)(c->h[i] >> 48);
        out[8 * i + 2] = (u8)(c->h[i] >> 40);
        out[8 * i + 3] = (u8)(c->h[i] >> 32);
        out[8 * i + 4] = (u8)(c->h[i] >> 24);
        out[8 * i + 5] = (u8)(c->h[i] >> 16);
        out[8 * i + 6] = (u8)(c->h[i] >> 8);
        out[8 * i + 7] = (u8)(c->h[i]);
    }
}

/* ------------------------------------------------------------------
 * Field arithmetic modulo p = 2^255 - 19.
 * A field element is 16 limbs of 16 bits, little-endian, deliberately
 * carrying a little slack so intermediate products never overflow.
 * ------------------------------------------------------------------ */

typedef i64 gf[16];

static const gf GF0     = {0};
static const gf GF1     = {1};
static const gf D       = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                           0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2      = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                           0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf BASE_X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                           0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf BASE_Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                           0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf SQRT_M1 = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                           0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

/* L = 2^252 + 27742317777372353535851937790883648493, the order of the
 * base point, little-endian. A signature with S >= L is rejected: without
 * that check (S, R) and (S + L, R) would both verify. */
static const u8 ED_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static void gf_copy(gf r, const gf a) { int i; FOR(i, 16) r[i] = a[i]; }

/* Carry every limb back into 16 bits, folding the overflow of limb 15
 * back into limb 0 with the factor 2^256 == 38 (mod p). */
static void gf_carry(gf o)
{
    int i;
    i64 c;
    FOR(i, 16) {
        o[i] += (i64)1 << 16;
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;   /* c * 2^16: no signed left shift */
    }
}

/* Constant-time conditional swap of p and q when b == 1. */
static void gf_cswap(gf p, gf q, int b)
{
    int i;
    i64 c = ~(i64)(b - 1);
    FOR(i, 16) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* Reduce to the canonical representative and serialise. */
static void gf_pack(u8 *o, const gf n)
{
    int i, j, b;
    gf m, t;
    FOR(i, 16) t[i] = n[i];
    gf_carry(t);
    gf_carry(t);
    gf_carry(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_cswap(t, m, 1 - b);
    }
    FOR(i, 16) {
        o[2 * i]     = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static int ct_verify32(const u8 *x, const u8 *y)
{
    unsigned int i, d = 0;
    for (i = 0; i < 32; i++) d |= (unsigned int)(x[i] ^ y[i]);
    return (int)((1 & ((d - 1) >> 8)) - 1);   /* 0 equal, -1 different */
}

static int gf_neq(const gf a, const gf b)
{
    u8 c[32], d[32];
    gf_pack(c, a);
    gf_pack(d, b);
    return ct_verify32(c, d);
}

static u8 gf_parity(const gf a)
{
    u8 d[32];
    gf_pack(d, a);
    return (u8)(d[0] & 1);
}

static void gf_unpack(gf o, const u8 *n)
{
    int i;
    FOR(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void gf_add(gf o, const gf a, const gf b) { int i; FOR(i, 16) o[i] = a[i] + b[i]; }
static void gf_sub(gf o, const gf a, const gf b) { int i; FOR(i, 16) o[i] = a[i] - b[i]; }

static void gf_mul(gf o, const gf a, const gf b)
{
    int i, j;
    i64 t[31];
    FOR(i, 31) t[i] = 0;
    FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
    FOR(i, 15) t[i] += 38 * t[i + 16];
    FOR(i, 16) o[i] = t[i];
    gf_carry(o);
    gf_carry(o);
}

static void gf_sq(gf o, const gf a) { gf_mul(o, a, a); }

static void gf_inv(gf o, const gf i)
{
    gf c;
    int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        gf_sq(c, c);
        if (a != 2 && a != 4) gf_mul(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

/* i^((p-5)/8), used to take the square root during decompression. */
static void gf_pow2523(gf o, const gf i)
{
    gf c;
    int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        gf_sq(c, c);
        if (a != 1) gf_mul(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

/* ------------------------------------------------------------------
 * Edwards curve arithmetic. A point is (X, Y, Z, T) in extended
 * coordinates: x = X/Z, y = Y/Z, x*y = T/Z.
 * ------------------------------------------------------------------ */

static void pt_add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;

    gf_sub(a, p[1], p[0]);
    gf_sub(t, q[1], q[0]);
    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);
    gf_add(t, q[0], q[1]);
    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);
    gf_mul(c, c, D2);
    gf_mul(d, p[2], q[2]);
    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);

    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

static void pt_cswap(gf p[4], gf q[4], u8 b) { int i; FOR(i, 4) gf_cswap(p[i], q[i], b); }

static void pt_pack(u8 *r, gf p[4])
{
    gf tx, ty, zi;
    gf_inv(zi, p[2]);
    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    r[31] ^= (u8)(gf_parity(tx) << 7);
}

static void pt_scalarmult(gf p[4], gf q[4], const u8 *s)
{
    int i;
    gf_copy(p[0], GF0);
    gf_copy(p[1], GF1);
    gf_copy(p[2], GF1);
    gf_copy(p[3], GF0);
    for (i = 255; i >= 0; --i) {
        u8 b = (u8)((s[i / 8] >> (i & 7)) & 1);
        pt_cswap(p, q, b);
        pt_add(q, p);
        pt_add(p, p);
        pt_cswap(p, q, b);
    }
}

static void pt_scalarbase(gf p[4], const u8 *s)
{
    gf q[4];
    gf_copy(q[0], BASE_X);
    gf_copy(q[1], BASE_Y);
    gf_copy(q[2], GF1);
    gf_mul(q[3], BASE_X, BASE_Y);
    pt_scalarmult(p, q, s);
}

/* Decompress a public key into -A. Returns 0 on success, -1 if the
 * encoding is not a point on the curve. */
static int pt_unpack_neg(gf r[4], const u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;

    gf_copy(r[2], GF1);
    gf_unpack(r[1], p);
    gf_sq(num, r[1]);
    gf_mul(den, num, D);
    gf_sub(num, num, r[2]);
    gf_add(den, r[2], den);

    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);

    gf_pow2523(t, t);
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) gf_mul(r[0], r[0], SQRT_M1);

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) return -1;

    if (gf_parity(r[0]) == (p[31] >> 7)) gf_sub(r[0], GF0, r[0]);

    gf_mul(r[3], r[0], r[1]);
    return 0;
}

/* ------------------------------------------------------------------
 * Scalar arithmetic modulo L
 * ------------------------------------------------------------------ */

static void scalar_reduce(u8 *r)
{
    i64 x[64], carry;
    int i, j;

    FOR(i, 64) x[i] = (i64)r[i];
    FOR(i, 64) r[i] = 0;

    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (i64)ED_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    FOR(j, 32) {
        x[j] += carry - (x[31] >> 4) * (i64)ED_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    FOR(j, 32) x[j] -= carry * (i64)ED_L[j];
    FOR(i, 32) {
        x[i + 1] += x[i] >> 8;
        r[i] = (u8)(x[i] & 255);
    }
}

/* S must be strictly below L. */
static int scalar_canonical(const u8 s[32])
{
    int i;
    for (i = 31; i >= 0; --i) {
        if (s[i] < ED_L[i]) return 1;
        if (s[i] > ED_L[i]) return 0;
    }
    return 0;   /* S == L is not canonical */
}

/* ------------------------------------------------------------------
 * Verification
 * ------------------------------------------------------------------ */

/* pk[32], sig = R[32] || S[32], plus a SHA-512 context already fed
 * exactly R || A || M. The context is consumed. 0 = valid, -1 = invalid. */
static int ed25519_hash_verify(const u8 pk[32], const u8 sig[64], Sha512 *ctx)
{
    u8 h[64], rcheck[32];
    gf p[4], q[4];

    if (!scalar_canonical(sig + 32)) return -1;
    if (pt_unpack_neg(q, pk)) return -1;

    sha512_final(ctx, h);
    scalar_reduce(h);

    /* R' = [h](-A) + [S]B, and R' must be exactly the R in the signature. */
    pt_scalarmult(p, q, h);
    pt_scalarbase(q, sig + 32);
    pt_add(p, q);
    pt_pack(rcheck, p);

    return ct_verify32(rcheck, sig) == 0 ? 0 : -1;
}

/* Convenience wrapper for an in-memory message. */
static int ed25519_verify(const u8 pk[32], const u8 sig[64], const u8 *msg, size_t msglen)
{
    Sha512 ctx;

    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    sha512_update(&ctx, msg, msglen);
    return ed25519_hash_verify(pk, sig, &ctx);
}

/* ------------------------------------------------------------------
 * Small helpers for the command line
 * ------------------------------------------------------------------ */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read a public key: 32 raw bytes, or 64 hex digits with an optional
 * trailing newline. Returns 0 on success. */
static int load_pubkey(const char *path, u8 out[32])
{
    u8 buf[80];
    FILE *f = fopen(path, "rb");
    size_t n, i;

    if (!f) return -1;
    n = fread(buf, 1, sizeof(buf), f);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);

    if (n == 32) { memcpy(out, buf, 32); return 0; }
    if (n == 64 || (n == 65 && buf[64] == '\n')) {
        for (i = 0; i < 64; i += 2) {
            int hi = hexval(buf[i]), lo = hexval(buf[i + 1]);
            if (hi < 0 || lo < 0) return -1;
            out[i / 2] = (u8)((hi << 4) | lo);
        }
        return 0;
    }
    return -1;
}

static int load_sig(const char *path, u8 out[64])
{
    u8 buf[65];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) return -1;
    n = fread(buf, 1, sizeof(buf), f);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    if (n != 64) return -1;
    memcpy(out, buf, 64);
    return 0;
}

/* ------------------------------------------------------------------
 * Self test - RFC 8032 section 7.1 vectors and negative cases.
 * ------------------------------------------------------------------ */

struct vector {
    const char *sk;
    const char *pk;
    const char *msg;
    const char *sig;
};

static const struct vector VECTORS[] = {
    {   /* RFC 8032 TEST 1: empty message */
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
    },
    {   /* RFC 8032 TEST 2: one byte */
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
    },
    {   /* RFC 8032 TEST 3: two bytes */
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
    }
};

static int unhex(const char *s, u8 *out, size_t max)
{
    size_t n = strlen(s) / 2, i;
    if (strlen(s) % 2 || n > max) return -1;
    for (i = 0; i < n; i++) {
        int hi = hexval((unsigned char)s[2 * i]);
        int lo = hexval((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (u8)((hi << 4) | lo);
    }
    return (int)n;
}

static int selftest(void)
{
    Sha512 ctx;
    u8 digest[64];
    static const char *abc =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    u8 want[64];
    int fails = 0;
    size_t v;

    /* SHA-512 must at least match its own published vector. */
    sha512_init(&ctx);
    sha512_update(&ctx, "abc", 3);
    sha512_final(&ctx, digest);
    if (unhex(abc, want, 64) != 64 || memcmp(digest, want, 64) != 0) {
        fprintf(stderr, "selftest: SHA-512(abc) mismatch\n");
        fails++;
    }

    for (v = 0; v < sizeof(VECTORS) / sizeof(VECTORS[0]); v++) {
        u8 pk[32], sig[64], msg[8], bad[64];
        int mlen = unhex(VECTORS[v].msg, msg, sizeof(msg));
        if (unhex(VECTORS[v].pk, pk, 32) != 32 || unhex(VECTORS[v].sig, sig, 64) != 64 || mlen < 0) {
            fprintf(stderr, "selftest: vector %u is malformed\n", (unsigned)v);
            fails++;
            continue;
        }
        if (ed25519_verify(pk, sig, msg, (size_t)mlen) != 0) {
            fprintf(stderr, "selftest: RFC 8032 vector %u did not verify\n", (unsigned)v);
            fails++;
        }
        /* one flipped signature byte must fail */
        memcpy(bad, sig, 64);
        bad[7] ^= 0x01;
        if (ed25519_verify(pk, bad, msg, (size_t)mlen) == 0) {
            fprintf(stderr, "selftest: vector %u accepted a corrupted signature\n", (unsigned)v);
            fails++;
        }
        /* one flipped message byte must fail (skip the empty message) */
        if (mlen > 0) {
            msg[0] ^= 0x01;
            if (ed25519_verify(pk, sig, msg, (size_t)mlen) == 0) {
                fprintf(stderr, "selftest: vector %u accepted a corrupted message\n", (unsigned)v);
                fails++;
            }
            msg[0] ^= 0x01;
        }
        /* S + L is the same point but not a canonical scalar: reject */
        memcpy(bad, sig, 64);
        {
            unsigned carry = 0;
            int i;
            for (i = 0; i < 32; i++) {
                unsigned sum = (unsigned)bad[32 + i] + ED_L[i] + carry;
                bad[32 + i] = (u8)sum;
                carry = sum >> 8;
            }
            if (carry) {
                fprintf(stderr, "selftest: vector %u scalar overflow\n", (unsigned)v);
                fails++;
            }
        }
        if (ed25519_verify(pk, bad, msg, (size_t)mlen) == 0) {
            fprintf(stderr, "selftest: vector %u accepted a non-canonical S\n", (unsigned)v);
            fails++;
        }
    }

    if (fails == 0) printf("selftest: ok\n");
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

static int usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <pubkey> <message> <signature>\n"
            "       %s --selftest\n"
            "  pubkey    : 32 raw bytes or 64 hex digits\n"
            "  signature : 64 raw bytes (R || S)\n"
            "exit: 0 valid, 1 invalid, 2 usage or I/O error\n", argv0, argv0);
    return 2;
}

int main(int argc, char **argv)
{
    u8 pk[32], sig[64];
    u8 buf[65536];
    Sha512 ctx;
    FILE *m;
    size_t n;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc != 4) return usage(argv[0]);

    if (load_pubkey(argv[1], pk) != 0) {
        fprintf(stderr, "ed25519-verify: %s: not a 32-byte or 64-hex-digit public key\n", argv[1]);
        return 2;
    }
    if (load_sig(argv[3], sig) != 0) {
        fprintf(stderr, "ed25519-verify: %s: signature must be exactly 64 bytes\n", argv[3]);
        return 2;
    }
    m = fopen(argv[2], "rb");
    if (!m) {
        fprintf(stderr, "ed25519-verify: %s: cannot read message\n", argv[2]);
        return 2;
    }

    /* Stream the message into the same hash the verifier expects:
     * SHA-512(R || A || M). The index can be any size. */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    while ((n = fread(buf, 1, sizeof(buf), m)) > 0) sha512_update(&ctx, buf, n);
    if (ferror(m)) {
        fclose(m);
        fprintf(stderr, "ed25519-verify: %s: read error\n", argv[2]);
        return 2;
    }
    fclose(m);

    if (ed25519_hash_verify(pk, sig, &ctx) != 0) {
        printf("invalid\n");
        return 1;
    }
    return 0;
}
