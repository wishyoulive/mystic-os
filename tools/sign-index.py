#!/usr/bin/env python3
"""
sign-index.py - Ed25519 signing for the Mystic OS package repository.

No dependencies: Ed25519 (RFC 8032) and SHA-512 come from hashlib, the
curve arithmetic is here. The same algorithm is implemented a second,
independent time in C in src/tools/ed25519.c; the two are cross-checked
against each other and against the RFC 8032 test vectors.

The repository is a directory holding:

    index.tsv         name<TAB>version<TAB>size<TAB>sha256<TAB>file<TAB>desc
    index.tsv.sig     64 raw bytes: R || S, little-endian
    index.pub         32 raw bytes: the publisher's public key

pkg verifies index.tsv.sig over the exact bytes of index.tsv before it
reads a single row from it, and refuses the repository otherwise.

Commands
--------
    keygen          make a new keypair; the seed goes to a file, the
                    public key is printed (and can be written raw)
    sign            sign a file, writing <in>.sig (64 raw bytes)
    verify          check a signature (this is the reference checker)
    selftest        run the RFC 8032 section 7.1 vectors

Examples
--------
    # once, on the publisher's machine, kept out of the repository:
    tools/sign-index.py keygen --seed build/sign/repo.key \
                               --pub stage/etc/mystic/repo.pub

    # every time the index changes:
    tools/sign-index.py sign --seed build/sign/repo.key \
                             --in build/repo/index.tsv

    # confirm what pkg will do:
    tools/sign-index.py verify --pub stage/etc/mystic/repo.pub \
                               --in build/repo/index.tsv

Exit status is 0 on success and 1 on a bad signature or a bad key.
"""

import argparse
import hashlib
import os
import sys

# ---------------------------------------------------------------------------
# Ed25519, RFC 8032. The arithmetic is the RFC's reference model: points are
# extended Edwards coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, x*y = T/Z.
# ---------------------------------------------------------------------------

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, P - 2, P)


_D = -121665 * _inv(121666) % P
_I = pow(2, (P - 1) // 4, P)


def _xrecover(y):
    """The x coordinate for a given y on the curve, or a wrong root."""
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = (x * _I) % P
    if x % 2 != 0:
        x = P - x
    return x


_BY = 4 * _inv(5) % P
_BX = _xrecover(_BY)
_B = (_BX % P, _BY % P, 1, (_BX * _BY) % P)


def _edwards(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    p1 = (y1 - x1) * (y2 - x2) % P
    p2 = (y1 + x1) * (y2 + x2) % P
    p3 = t1 * 2 * _D * t2 % P
    p4 = z1 * 2 * z2 % P
    e = p2 - p1
    f = p4 - p3
    g = p4 + p3
    h = p2 + p1
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _scalarmult(point, n):
    result = (0, 1, 1, 0)
    addend = point
    while n > 0:
        if n & 1:
            result = _edwards(result, addend)
        addend = _edwards(addend, addend)
        n >>= 1
    return result


def _encodeint(y):
    return int(y).to_bytes(32, "little")


def _encodepoint(point):
    x, y, z, _t = point
    zi = _inv(z)
    x = x * zi % P
    y = y * zi % P
    return _encodeint(y | ((x & 1) << 255))


def _bit(h, i):
    return (h[i // 8] >> (i % 8)) & 1


def _decodepoint(s):
    """Decode a 32-byte point, or None if it is not a point on the curve."""
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    if y >= P:                       # non-canonical encoding
        return None
    x = _xrecover(y)
    if (x & 1) != _bit(s, 255):
        x = P - x
    point = (x, y, 1, x * y % P)
    # -x^2 + y^2 == 1 + d*x^2*y^2
    xx = x * x % P
    yy = y * y % P
    if (-xx + yy - 1 - _D * xx % P * yy) % P != 0:
        return None
    return point


def _secret_expand(seed):
    """(clamped scalar, nonce prefix) from a 32-byte seed."""
    if len(seed) != 32:
        raise ValueError("an Ed25519 seed is exactly 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = 2 ** 254 + sum(2 ** i * _bit(h, i) for i in range(3, 254))
    return a, h[32:]


def public_key(seed):
    """The 32-byte public key for a 32-byte seed."""
    a, _prefix = _secret_expand(seed)
    return _encodepoint(_scalarmult(_B, a))


def sign(message, seed):
    """The 64-byte signature for message (bytes) under seed (32 bytes)."""
    a, prefix = _secret_expand(seed)
    pub = _encodepoint(_scalarmult(_B, a))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % L
    r_enc = _encodepoint(_scalarmult(_B, r))
    k = int.from_bytes(hashlib.sha512(r_enc + pub + message).digest(), "little") % L
    s = (r + k * a) % L
    return r_enc + _encodeint(s)


def verify(message, signature, pub):
    """True if signature is a valid Ed25519 signature of message under pub."""
    if len(signature) != 64 or len(pub) != 32:
        return False
    r_enc = signature[:32]
    s = int.from_bytes(signature[32:], "little")
    if s >= L:                       # non-canonical S would be malleable
        return False
    a_point = _decodepoint(pub)
    r_point = _decodepoint(r_enc)
    if a_point is None or r_point is None:
        return False
    k = int.from_bytes(hashlib.sha512(r_enc + pub + message).digest(), "little") % L
    left = _scalarmult(_B, s)
    right = _edwards(r_point, _scalarmult(a_point, k))
    return _encodepoint(left) == _encodepoint(right)


def fingerprint(pub):
    """SHA-256 of the raw public key, the string a user can compare."""
    return hashlib.sha256(pub).hexdigest()


# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def read_pubkey(path):
    """32 raw bytes, or 64 hex digits with an optional trailing newline."""
    data = read_bytes(path)
    if len(data) == 32:
        return data
    text = data.decode("ascii", "replace").strip()
    if len(text) == 64:
        try:
            return bytes.fromhex(text)
        except ValueError:
            pass
    raise SystemExit("%s: not a 32-byte or 64-hex-digit public key" % path)


def read_seed(path):
    data = read_bytes(path)
    if len(data) == 32:
        return data
    text = data.decode("ascii", "replace").strip()
    if len(text) == 64:
        try:
            return bytes.fromhex(text)
        except ValueError:
            pass
    raise SystemExit("%s: not a 32-byte or 64-hex-digit Ed25519 seed" % path)


def write_bytes(path, data, mode=None):
    parent = os.path.dirname(os.path.abspath(path))
    if parent and not os.path.isdir(parent):
        os.makedirs(parent, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(path, mode)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_keygen(args):
    if os.path.exists(args.seed) and not args.force:
        raise SystemExit(
            "%s already exists - refusing to overwrite a signing key "
            "(pass --force only if you are certain)" % args.seed)
    seed = os.urandom(32)
    pub = public_key(seed)
    write_bytes(args.seed, seed.hex().encode("ascii") + b"\n", 0o600)
    if args.pub:
        # raw 32 bytes: exactly what /etc/mystic/repo.pub and pkg expect
        write_bytes(args.pub, pub)
    print("seed      : %s (mode 0600, keep it off any shared machine)" % args.seed)
    if args.pub:
        print("public key: %s (32 raw bytes)" % args.pub)
    print("public hex: %s" % pub.hex())
    print("fingerprint: sha256 %s" % fingerprint(pub))
    return 0


def cmd_sign(args):
    seed = read_seed(args.seed)
    pub = public_key(seed)
    if args.pub:
        expected = read_pubkey(args.pub)
        if expected != pub:
            raise SystemExit(
                "%s does not match the seed - refusing to sign with a key "
                "whose public half is not the one being distributed\n"
                "  seed gives: %s\n  %s has:    %s"
                % (args.pub, pub.hex(), args.pub, expected.hex()))
    message = read_bytes(args.input)
    sig = sign(message, seed)
    write_bytes(args.sig, sig)
    print("signed    : %s (%d bytes)" % (args.input, len(message)))
    print("signature : %s (64 raw bytes)" % args.sig)
    print("public hex: %s" % pub.hex())
    print("fingerprint: sha256 %s" % fingerprint(pub))
    return 0


def cmd_verify(args):
    pub = read_pubkey(args.pub)
    message = read_bytes(args.input)
    sig = read_bytes(args.sig)
    if len(sig) != 64:
        print("FAIL: %s is %d bytes, a signature is 64" % (args.sig, len(sig)))
        return 1
    if verify(message, sig, pub):
        print("ok: signature verifies over %d bytes of %s" % (len(message), args.input))
        print("key fingerprint: sha256 %s" % fingerprint(pub))
        return 0
    print("FAIL: signature does not verify over %s with that key" % args.input)
    return 1


def cmd_selftest(_args):
    """RFC 8032 section 7.1, tests 1-3, in both directions."""
    vectors = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
         "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]
    fails = 0
    for n, (sk_hex, pk_hex, msg_hex, sig_hex) in enumerate(vectors, 1):
        seed = bytes.fromhex(sk_hex)
        want_pub = bytes.fromhex(pk_hex)
        msg = bytes.fromhex(msg_hex)
        want_sig = bytes.fromhex(sig_hex)
        got_pub = public_key(seed)
        if got_pub != want_pub:
            print("vector %d: public key mismatch" % n)
            fails += 1
        got_sig = sign(msg, seed)
        if got_sig != want_sig:
            print("vector %d: signature mismatch" % n)
            fails += 1
        if not verify(msg, want_sig, want_pub):
            print("vector %d: valid signature rejected" % n)
            fails += 1
        bad = bytearray(want_sig)
        bad[3] ^= 0x01
        if verify(msg, bytes(bad), want_pub):
            print("vector %d: corrupted signature accepted" % n)
            fails += 1
        if msg and verify(msg[:-1], want_sig, want_pub):
            print("vector %d: shortened message accepted" % n)
            fails += 1
        # S + L is the same point with a non-canonical scalar; must fail.
        s_plus_l = (int.from_bytes(want_sig[32:], "little") + L)
        malleable = want_sig[:32] + s_plus_l.to_bytes(32, "little")
        if verify(msg, malleable, want_pub):
            print("vector %d: non-canonical S accepted" % n)
            fails += 1
    if fails:
        print("selftest: %d failure(s)" % fails)
        return 1
    print("selftest: ok (3 RFC 8032 vectors, sign + verify + 3 negative cases each)")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(
        prog="sign-index.py",
        description="Ed25519 signing for the Mystic OS package repository")
    sub = ap.add_subparsers(dest="cmd")

    k = sub.add_parser("keygen", help="create a keypair (seed written, key printed)")
    k.add_argument("--seed", default="build/sign/repo.key", help="seed file (default %(default)s)")
    k.add_argument("--pub", default=None, help="also write the 32-byte public key here")
    k.add_argument("--force", action="store_true", help="overwrite an existing seed")
    k.set_defaults(func=cmd_keygen)

    s = sub.add_parser("sign", help="sign a file, writing <file>.sig by default")
    s.add_argument("--seed", required=True, help="the publisher's seed file")
    s.add_argument("--in", dest="input", required=True, help="the file to sign")
    s.add_argument("--sig", default=None, help="signature output (default <in>.sig)")
    s.add_argument("--pub", default=None,
                   help="public key to cross-check against the seed (recommended)")
    s.set_defaults(func=cmd_sign)

    v = sub.add_parser("verify", help="verify a signature")
    v.add_argument("--pub", required=True)
    v.add_argument("--in", dest="input", required=True)
    v.add_argument("--sig", default=None, help="default <in>.sig")
    v.set_defaults(func=cmd_verify)

    t = sub.add_parser("selftest", help="run the RFC 8032 vectors")
    t.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    if getattr(args, "sig", "x") is None:
        args.sig = args.input + ".sig"
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
