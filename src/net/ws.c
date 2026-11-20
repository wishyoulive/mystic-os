/* ============================================================
 *  Mystic OS - net/ws.c
 *  An RFC 6455 WebSocket client.  See net/ws.h for the contract.
 *
 *  The three things a WebSocket client can get wrong, and how this
 *  file avoids them:
 *
 *   1. The accept value.  §4.1 hashes the Sec-WebSocket-Key *header
 *      field value* -- the base64 text -- concatenated with the GUID,
 *      not the 16 raw bytes those characters decode to.  Not
 *      base64(SHA-1(GUID || text)) either, and not SHA-1 of the decoded
 *      nonce.  The RFC's own example is in the harness: the 16 bytes
 *      that base64 encode to "dGhlIHNhbXBsZSBub25jZQ==" (the string
 *      "the sample nonce") must produce "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
 *      (§1.3).
 *
 *   2. Framing.  A frame is not a message.  The opcode, FIN, the three
 *      length forms and the mask bit are parsed independently, and a
 *      message is assembled across frames before it is delivered, so a
 *      caller never sees half a text message (§5.4).
 *
 *   3. The masking key.  A client MUST mask every frame with a fresh
 *      key, and §5.3 points at RFC 4086 for "fresh".  There is no
 *      deterministic fallback here: if no key can be had, the send
 *      fails rather than putting a predictable key on the wire.
 *
 *  Nothing in this file opens a socket, reads a clock, or calls a
 *  function outside the C library.  Every byte moves through the two
 *  callbacks in WsTransport, which is what lets the harness drive the
 *  whole protocol from memory buffers.
 * ============================================================ */
#include "ws.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A distinguished positive return from frame_finish: "the frame was a
 * fragment, keep reading".  Public functions only ever return WS_OK or
 * a negative code, so this cannot escape. */
#define WS_RECV_MORE 1

/* ============================================================
 *  SHA-1, FIPS 180-4 §6.1.  RFC 6455 §4.1 asks for SHA-1 by name for
 *  the accept value, and the algorithm is frozen, so the 20-byte digest
 *  here is the one every server computes.  It is not used for
 *  signatures or passwords; ws.c uses it for nothing else.
 * ============================================================ */
typedef struct {
    uint32_t h[5];
    uint64_t bits;          /* total bytes fed, in bits */
    uint8_t  buf[64];
    size_t   have;
} Sha1;

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(Sha1 *s, const uint8_t p[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->bits = 0; s->have = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bits += (uint64_t)n * 8u;
    while (n) {
        size_t k = 64 - s->have;
        if (k > n) k = n;
        memcpy(s->buf + s->have, p, k);
        s->have += k; p += k; n -= k;
        if (s->have == 64) { sha1_block(s, s->buf); s->have = 0; }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->bits;             /* before the padding is fed */
    uint8_t one = 0x80, zero = 0x00, len[8];

    sha1_update(s, &one, 1);
    while (s->have != 56) sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

/* ============================================================
 *  base64, RFC 4648 §4 with padding.  Only encoding is needed: the
 *  client sends a key and a server sends back an accept, and both are
 *  encoded.
 * ============================================================ */
/* 64 symbols plus the NUL that a string literal always carries.  The
 * table is indexed 0..63, so the NUL is never read; declaring the array
 * as a literal without a size says so and keeps -Wunterminated-string-
 * initialization quiet. */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t need = ((n + 2) / 3) * 4;
    if (cap < need + 1) return WS_ERR_NOSPACE;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = rem > 1 ? B64[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

/* ============================================================
 *  UTF-8, RFC 3629 §4 (which is what RFC 6455 §5.6 means by "UTF-8").
 *  Rejects overlong forms, surrogates, and anything above U+10FFFF:
 *  each of those is a way for a text frame to carry bytes that are
 *  not text, which is exactly what §8.1 forbids.
 * ============================================================ */
static int utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1Fu; need = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; need = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; need = 3; }
        else return 0;
        if (c >= 0xF5) return 0;                 /* > U+10FFFF by construction */
        if (n - i - 1 < need) return 0;          /* truncated sequence         */
        for (size_t k = 1; k <= need; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (need == 1 && cp < 0x80)    return 0; /* overlong */
        if (need == 2 && cp < 0x800)   return 0;
        if (need == 3 && cp < 0x10000) return 0;
        if (cp > 0x10FFFF)             return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  /* surrogate */
        i += need + 1;
    }
    return 1;
}

/* Incremental RFC 3629 validator for a text message split across
 * frames.  §5.6 allows a codepoint to straddle a fragment boundary, so
 * an individual fragment need not be valid UTF-8 on its own and a
 * whole-buffer check cannot be used.  `need` counts the continuation
 * bytes still expected (0 at a codepoint boundary), `seq` is the total
 * length of the sequence in progress, and `cp` accumulates it.
 * Returns 1 while the bytes seen so far are a valid prefix. */
static int utf8_feed(int *need, uint32_t *cp, int *seq,
                     const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (*need == 0) {
            if (c < 0x80) continue;
            if (c >= 0xF5) return 0;             /* > U+10FFFF by construction */
            if (c == 0xC0 || c == 0xC1) return 0; /* overlong 2-byte lead */
            if ((c & 0xE0) == 0xC0)      { *need = 1; *seq = 2; *cp = c & 0x1Fu; }
            else if ((c & 0xF0) == 0xE0) { *need = 2; *seq = 3; *cp = c & 0x0Fu; }
            else if ((c & 0xF8) == 0xF0) { *need = 3; *seq = 4; *cp = c & 0x07u; }
            else return 0;
        } else {
            if ((c & 0xC0) != 0x80) return 0;
            *cp = (*cp << 6) | (c & 0x3Fu);
            if (--*need == 0) {
                if (*seq == 2 && *cp < 0x80)    return 0;  /* overlong */
                if (*seq == 3 && *cp < 0x800)   return 0;
                if (*seq == 4 && *cp < 0x10000) return 0;
                if (*cp > 0x10FFFF)             return 0;
                if (*cp >= 0xD800 && *cp <= 0xDFFF) return 0;  /* surrogate */
            }
        }
    }
    return 1;
}

/* ============================================================
 *  Small string helpers.  All of them work on NUL-terminated strings
 *  that came out of the response buffer, which parse_response() made
 *  sure contain no control bytes.
 * ============================================================ */
static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

static int token_is(const char *s, size_t n, const char *tok) {
    size_t tl = strlen(tok);
    if (n != tl) return 0;
    for (size_t i = 0; i < n; i++)
        if (ascii_lower((unsigned char)s[i]) != ascii_lower((unsigned char)tok[i]))
            return 0;
    return 1;
}

/* RFC 7230 §7: a comma-separated list of tokens, with optional
 * whitespace around each.  "Upgrade: websocket" and
 * "Connection: keep-alive, Upgrade" both answer yes for their token. */
static int list_has_token(const char *list, const char *tok) {
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > s && token_is(s, (size_t)(e - s), tok)) return 1;
    }
    return 0;
}

/* Subprotocol names are case-sensitive (§4.1), unlike header tokens. */
static int protocol_offered(const char *list, const char *want) {
    size_t wl = strlen(want);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - s) == wl && memcmp(s, want, wl) == 0) return 1;
    }
    return 0;
}

/* tchar, RFC 7230 §3.2.6. */
static int is_tchar(int c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z')) return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
        return 1;
    default:
        return 0;
    }
}

/* A request part may not carry a control byte: CR or LF would end the
 * request line or a header early and let a URL inject its own header. */
static int part_unsafe(const char *s, int forbid_space) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) return 1;
        if (forbid_space && c == ' ') return 1;
    }
    return 0;
}

static int protocols_ok(const char *s) {
    if (!s) return 1;
    size_t n = strlen(s);
    if (n == 0 || n > WS_MAX_PROTOCOLS) return 0;
    int any = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') {
            if (!is_tchar((unsigned char)*p)) return 0;
            p++;
        }
        if (p == start) return 0;    /* RFC 7230: no empty list element */
        any = 1;
        if (*p == ',') {
            p++;
            if (!*p) return 0;       /* nor a trailing comma          */
        }
    }
    return any;
}

static void set_err(char *err, size_t esz, const char *fmt, ...) {
    if (!err || esz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, esz, fmt, ap);
    va_end(ap);
}

/* ============================================================
 *  Connection state
 * ============================================================ */
typedef struct {
    int       active;        /* a header has been parsed, payload pending  */
    uint8_t   hdr[14];       /* 2 fixed + 8 length + 4 mask (server: none) */
    size_t    hdr_have;
    size_t    hdr_need;      /* 0 = start a new frame                      */
    int       fin;
    int       opcode;
    uint8_t  *dst;           /* where this frame's payload goes            */
    size_t    len;           /* payload length of this frame               */
    size_t    got;
} WsFrame;

struct WsConn {
    WsTransport tr;
    size_t      max_message;
    WsRandFn    rnd;
    void       *rnd_ctx;
    FILE       *urandom;

    /* handshake */
    int      hs_sent;
    int      hs_done;
    size_t   hs_scan;
    uint8_t  hs_key[WS_KEY_LEN];
    char     hs_protocols[WS_MAX_PROTOCOLS + 1];

    /* Bytes that arrived with the response but belong to the frame
     * stream, plus the response itself while it is being read. */
    uint8_t *ibuf;
    size_t   icap, ilen, ipos;

    WsFrame  fr;
    uint8_t  ctl[125];       /* the payload of the control frame in hand   */

    /* A message being reassembled across frames. */
    uint8_t *msg;
    size_t   msgcap, msglen;
    int      assembling;
    int      msg_opcode;

    int      close_sent;
    int      recv_closed;
    int      failed;
    int      send_frag;      /* 0, or the opcode of the open outgoing message */
    /* Incremental UTF-8 state for an outgoing fragmented text message. */
    int      send_cp_need;
    uint32_t send_cp;
    int      send_cp_seq;
    uint16_t close_code_sent;
};

/* ============================================================
 *  Transport
 * ============================================================ */
static int read_exact(WsConn *c, uint8_t *dst, size_t need, size_t *have) {
    while (*have < need) {
        if (c->ipos < c->ilen) {
            size_t k = c->ilen - c->ipos;
            if (k > need - *have) k = need - *have;
            memcpy(dst + *have, c->ibuf + c->ipos, k);
            c->ipos += k;
            *have += k;
            continue;
        }
        long r = c->tr.read(c->tr.ctx, dst + *have, need - *have);
        if (r > 0) {
            if ((size_t)r > need - *have) return WS_ERR_IO;
            *have += (size_t)r;
        } else if (r == 0) {
            return WS_ERR_EOF;
        } else if (r == WS_IO_AGAIN) {
            return WS_ERR_AGAIN;
        } else {
            return WS_ERR_IO;
        }
    }
    return WS_OK;
}

static int write_all(WsConn *c, const uint8_t *p, size_t n) {
    while (n) {
        long r = c->tr.write(c->tr.ctx, p, n);
        if (r <= 0 || (size_t)r > n) return WS_ERR_IO;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return WS_OK;
}

static int random_bytes(WsConn *c, void *buf, size_t n) {
    if (c->rnd)
        return c->rnd(c->rnd_ctx, buf, n) == 0 ? WS_OK : WS_ERR_RANDOM;
    if (!c->urandom) {
        c->urandom = fopen("/dev/urandom", "rb");
        if (!c->urandom) return WS_ERR_RANDOM;
    }
    return fread(buf, 1, n, c->urandom) == n ? WS_OK : WS_ERR_RANDOM;
}

/* ============================================================
 *  The accept value: base64(SHA-1(base64(key) || GUID)), where
 *  base64(key) is the Sec-WebSocket-Key header value, RFC 6455 §4.1.
 * ============================================================ */
int ws_accept_key(const uint8_t key[WS_KEY_LEN], char out[WS_ACCEPT_BUF]) {
    if (!key || !out) return WS_ERR_INVAL;
    /* §4.1 step 5: the server concatenates the received header field
     * value (the base64 text) with the GUID and hashes that string.  The
     * 16 bytes are what the client puts *into* the header, so the header
     * value has to be rebuilt here before hashing.  Hashing the raw
     * bytes instead yields a value no conforming server will send. */
    char key_b64[WS_KEY_B64_LEN + 1];
    int kn = b64_encode(key, WS_KEY_LEN, key_b64, sizeof key_b64);
    if (kn < 0) return WS_ERR_NOSPACE;
    Sha1 s;
    uint8_t digest[20];
    sha1_init(&s);
    sha1_update(&s, key_b64, (size_t)kn);
    sha1_update(&s, WS_GUID, strlen(WS_GUID));
    sha1_final(&s, digest);
    if (b64_encode(digest, sizeof digest, out, WS_ACCEPT_BUF) < 0)
        return WS_ERR_NOSPACE;
    /* Zero the digest: it is not secret, but leaving hash state on the
     * stack after the function returns is a habit with no upside. */
    memset(digest, 0, sizeof digest);
    memset(&s, 0, sizeof s);
    memset(key_b64, 0, sizeof key_b64);
    return WS_OK;
}

/* ============================================================
 *  The upgrade request, RFC 6455 §4.1.
 *
 *      GET /chat HTTP/1.1
 *      Host: server.example.com
 *      Upgrade: websocket
 *      Connection: Upgrade
 *      Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
 *      [Origin: http://example.com]
 *      [Sec-WebSocket-Protocol: chat, superchat]
 *      Sec-WebSocket-Version: 13
 *
 *  No Sec-WebSocket-Extensions: this client implements none, and
 *  offering one it cannot speak would be a lie the first server frame
 *  would expose.
 * ============================================================ */
void ws_mask(void *data, size_t len, uint32_t mask_key, size_t offset) {
    uint8_t *p = data;
    /* §5.3: octet i of the payload is XORed with octet i mod 4 of the
     * masking key, and the key travels in network byte order. */
    const uint8_t k[4] = {
        (uint8_t)(mask_key >> 24), (uint8_t)(mask_key >> 16),
        (uint8_t)(mask_key >> 8),  (uint8_t)mask_key
    };
    for (size_t i = 0; i < len; i++) p[i] ^= k[(offset + i) & 3u];
}

int ws_build_request(const WsRequest *r, char *buf, size_t cap) {
    if (!r || !buf || !r->host || !r->path || !r->key) return WS_ERR_INVAL;
    if (part_unsafe(r->host, 1) || part_unsafe(r->path, 1)) return WS_ERR_INVAL;
    if (r->origin && part_unsafe(r->origin, 1)) return WS_ERR_INVAL;
    size_t hl = strlen(r->host), pl = strlen(r->path);
    if (hl == 0 || hl > WS_MAX_HOST) return WS_ERR_INVAL;
    if (pl == 0 || pl > WS_MAX_PATH || r->path[0] != '/') return WS_ERR_INVAL;
    if (r->origin && strlen(r->origin) > WS_MAX_ORIGIN) return WS_ERR_INVAL;
    if (!protocols_ok(r->protocols)) return WS_ERR_INVAL;

    char key[WS_KEY_B64_LEN + 1];
    if (b64_encode(r->key, WS_KEY_LEN, key, sizeof key) < 0) return WS_ERR_INVAL;

    char tmp[WS_MAX_REQUEST];
    int n = snprintf(tmp, sizeof tmp,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "%s%s%s"
                     "%s%s%s"
                     "Sec-WebSocket-Version: " WS_VERSION "\r\n"
                     "\r\n",
                     r->path, r->host, key,
                     r->origin ? "Origin: " : "",
                     r->origin ? r->origin : "",
                     r->origin ? "\r\n" : "",
                     r->protocols ? "Sec-WebSocket-Protocol: " : "",
                     r->protocols ? r->protocols : "",
                     r->protocols ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof tmp) return WS_ERR_NOSPACE;
    if ((size_t)n >= cap) return WS_ERR_NOSPACE;
    memcpy(buf, tmp, (size_t)n + 1);
    return n;
}

/* ============================================================
 *  Response parsing and validation, RFC 6455 §4.1.
 *
 *  The response is read into ibuf and parsed in place: every header
 *  name and value is NUL terminated inside the block that is about to
 *  be consumed, and whatever followed the final CRLFCRLF is moved to
 *  the front of ibuf for the frame reader.  A server is allowed to
 *  send its first frame in the same packet as its 101, and this is
 *  where those bytes survive.
 * ============================================================ */
static size_t find_hdr_end(const uint8_t *p, size_t from, size_t n) {
    for (size_t i = from; i + 3 < n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' &&
            p[i + 2] == '\r' && p[i + 3] == '\n')
            return i + 4;
    return 0;
}

static int parse_response(WsConn *c, size_t hdrlen, WsResponse *out,
                          char *err, size_t esz) {
    uint8_t *b = c->ibuf;
    memset(out, 0, sizeof *out);

    /* ---- status line: HTTP/1.1 SP 101 [SP reason] CRLF ---- */
    size_t e = 0;
    while (e + 1 < hdrlen && !(b[e] == '\r' && b[e + 1] == '\n')) e++;
    if (e + 1 >= hdrlen) {
        set_err(err, esz, "the status line is not terminated by CRLF");
        return WS_ERR_HANDSHAKE;
    }
    if (e < 12 || memcmp(b, "HTTP/1.1 ", 9) != 0 ||
        b[9] < '0' || b[9] > '9' || b[10] < '0' || b[10] > '9' ||
        b[11] < '0' || b[11] > '9' || (e > 12 && b[12] != ' ')) {
        char line[80];
        size_t k = e < sizeof line - 1 ? e : sizeof line - 1;
        memcpy(line, b, k);
        line[k] = '\0';
        set_err(err, esz, "not an HTTP/1.1 status line: \"%s\"", line);
        return WS_ERR_HANDSHAKE;
    }
    out->status = (b[9] - '0') * 100 + (b[10] - '0') * 10 + (b[11] - '0');
    if (out->status != 101) {
        set_err(err, esz, "server answered %d, not 101 Switching Protocols",
                out->status);
        return WS_ERR_STATUS;
    }

    /* ---- header fields ---- */
    int have_upgrade = 0, have_conn = 0, have_accept = 0, have_proto = 0;
    size_t s = e + 2;
    while (s < hdrlen) {
        size_t le = s;
        while (le + 1 < hdrlen && !(b[le] == '\r' && b[le + 1] == '\n')) le++;
        if (le + 1 >= hdrlen) {
            set_err(err, esz, "a response header line is not terminated");
            return WS_ERR_HANDSHAKE;
        }
        if (le == s) break;                        /* empty line: done      */
        if (b[s] == ' ' || b[s] == '\t') {
            /* RFC 7230 §3.2.4: a server MUST NOT generate obs-fold. */
            set_err(err, esz, "the response uses obsolete header line folding");
            return WS_ERR_HANDSHAKE;
        }
        size_t colon = s;
        while (colon < le && b[colon] != ':') colon++;
        if (colon == le) {
            set_err(err, esz, "a response header line has no ':'");
            return WS_ERR_HANDSHAKE;
        }
        size_t ne = colon;
        while (ne > s && (b[ne - 1] == ' ' || b[ne - 1] == '\t')) ne--;
        if (ne != colon) {
            set_err(err, esz, "whitespace before ':' in a response header");
            return WS_ERR_HANDSHAKE;
        }
        if (ne == s) {
            set_err(err, esz, "a response header has an empty name");
            return WS_ERR_HANDSHAKE;
        }
        for (size_t i = s; i < ne; i++) {
            if (!is_tchar(b[i])) {
                set_err(err, esz, "a response header name has a bad character");
                return WS_ERR_HANDSHAKE;
            }
        }
        size_t vs = colon + 1;
        while (vs < le && (b[vs] == ' ' || b[vs] == '\t')) vs++;
        size_t ve = le;
        while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
        for (size_t i = vs; i < ve; i++) {
            if ((b[i] < 0x20 && b[i] != '\t') || b[i] == 0x7F) {
                set_err(err, esz,
                        "a response header value contains a control character");
                return WS_ERR_HANDSHAKE;
            }
        }
        b[colon] = '\0';
        b[ve] = '\0';
        const char *name = (const char *)(b + s);
        const char *val = (const char *)(b + vs);

        if (ci_equal(name, "upgrade")) {
            if (!out->upgrade[0] &&
                strlen(val) < sizeof out->upgrade) {
                memcpy(out->upgrade, val, strlen(val) + 1);
            }
            if (list_has_token(val, "websocket")) have_upgrade = 1;
        } else if (ci_equal(name, "connection")) {
            if (!out->connection[0] &&
                strlen(val) < sizeof out->connection) {
                memcpy(out->connection, val, strlen(val) + 1);
            }
            if (list_has_token(val, "upgrade")) have_conn = 1;
        } else if (ci_equal(name, "sec-websocket-accept")) {
            if (have_accept) {
                set_err(err, esz,
                        "the response has more than one Sec-WebSocket-Accept");
                return WS_ERR_HANDSHAKE;
            }
            if (strlen(val) >= sizeof out->accept) {
                set_err(err, esz, "Sec-WebSocket-Accept is too long to be one");
                return WS_ERR_HANDSHAKE;
            }
            memcpy(out->accept, val, strlen(val) + 1);
            have_accept = 1;
        } else if (ci_equal(name, "sec-websocket-protocol")) {
            if (have_proto) {
                set_err(err, esz,
                        "the response has more than one Sec-WebSocket-Protocol");
                return WS_ERR_HANDSHAKE;
            }
            if (strlen(val) >= sizeof out->protocol) {
                set_err(err, esz, "Sec-WebSocket-Protocol is too long");
                return WS_ERR_HANDSHAKE;
            }
            memcpy(out->protocol, val, strlen(val) + 1);
            have_proto = 1;
        } else if (ci_equal(name, "sec-websocket-extensions")) {
            out->extensions++;
        }
        s = le + 2;
    }

    /* ---- the four checks of §4.1, in the order a failure should be
     *      reported: was this even the right protocol, then the
     *      nonce that proves the server read the key ---- */
    if (!have_upgrade) {
        set_err(err, esz,
                "the response has no \"Upgrade: websocket\" header (§4.1)");
        return WS_ERR_HANDSHAKE;
    }
    if (!have_conn) {
        set_err(err, esz,
                "the response has no \"Connection: Upgrade\" token (§4.1)");
        return WS_ERR_HANDSHAKE;
    }
    if (!have_accept) {
        set_err(err, esz,
                "the response has no Sec-WebSocket-Accept header (§4.1)");
        return WS_ERR_HANDSHAKE;
    }
    char expect[WS_ACCEPT_BUF];
    if (ws_accept_key(c->hs_key, expect) != WS_OK) {
        set_err(err, esz, "could not compute the expected accept value");
        return WS_ERR_HANDSHAKE;
    }
    if (strcmp(expect, out->accept) != 0) {
        set_err(err, esz,
                "Sec-WebSocket-Accept is \"%s\" but SHA-1 of the "
                "Sec-WebSocket-Key header value gives \"%s\"",
                out->accept, expect);
        return WS_ERR_HANDSHAKE;
    }
    if (out->extensions) {
        set_err(err, esz,
                "the server selected an extension (Sec-WebSocket-Extensions) "
                "the client did not offer (§4.1)");
        return WS_ERR_HANDSHAKE;
    }
    if (have_proto) {
        if (!c->hs_protocols[0] || strchr(out->protocol, ',') ||
            !protocol_offered(c->hs_protocols, out->protocol)) {
            set_err(err, esz,
                    "the server chose subprotocol \"%s\", which the client "
                    "did not offer (§4.1)", out->protocol);
            return WS_ERR_HANDSHAKE;
        }
    }
    return WS_OK;
}

static int handshake_fill(WsConn *c) {
    long r = c->tr.read(c->tr.ctx, c->ibuf + c->ilen, c->icap - c->ilen);
    if (r > 0) {
        if ((size_t)r > c->icap - c->ilen) return WS_ERR_IO;
        c->ilen += (size_t)r;
        return WS_OK;
    }
    if (r == 0) return WS_ERR_EOF;
    if (r == WS_IO_AGAIN) return WS_ERR_AGAIN;
    return WS_ERR_IO;
}

int ws_handshake(WsConn *c, const WsRequest *req, WsResponse *resp,
                 char *err, size_t esz) {
    if (!c || !req || !resp) return WS_ERR_INVAL;
    if (err && esz) err[0] = '\0';
    if (c->hs_done) return WS_ERR_STATE;
    if (c->failed) return WS_ERR_CLOSED;

    if (!c->hs_sent) {
        char reqbuf[WS_MAX_REQUEST];
        int n = ws_build_request(req, reqbuf, sizeof reqbuf);
        if (n < 0) {
            set_err(err, esz, "the upgrade request is unusable: %s",
                    ws_strerror(n));
            return n;
        }
        /* Keep a copy of what the response must agree with, so a retry
         * after WS_ERR_AGAIN does not depend on the caller's buffers. */
        memcpy(c->hs_key, req->key, WS_KEY_LEN);
        c->hs_protocols[0] = '\0';
        if (req->protocols) {
            size_t pn = strlen(req->protocols);
            if (pn > WS_MAX_PROTOCOLS) {
                set_err(err, esz, "the subprotocol list is too long");
                return WS_ERR_INVAL;
            }
            memcpy(c->hs_protocols, req->protocols, pn + 1);
        }
        int rc = write_all(c, (const uint8_t *)reqbuf, (size_t)n);
        if (rc != WS_OK) {
            set_err(err, esz, "the upgrade request could not be sent");
            c->failed = 1;
            return rc;
        }
        c->hs_sent = 1;
    }

    for (;;) {
        size_t from = c->hs_scan > 3 ? c->hs_scan - 3 : 0;
        size_t end = find_hdr_end(c->ibuf, from, c->ilen);
        if (end) {
            size_t left = c->ilen - end;
            int rc = parse_response(c, end, resp, err, esz);
            if (rc != WS_OK) {
                c->failed = 1;
                c->recv_closed = 1;
                return rc;
            }
            if (left) memmove(c->ibuf, c->ibuf + end, left);
            c->ilen = left;
            c->ipos = 0;
            c->hs_done = 1;
            return WS_OK;
        }
        if (c->ilen >= c->icap) {
            set_err(err, esz, "the response headers exceed %d bytes",
                    (int)WS_MAX_HANDSHAKE);
            c->failed = 1;
            return WS_ERR_HANDSHAKE;
        }
        c->hs_scan = c->ilen;
        int rc = handshake_fill(c);
        if (rc == WS_ERR_AGAIN) return WS_ERR_AGAIN;
        if (rc != WS_OK) {
            set_err(err, esz, rc == WS_ERR_EOF
                    ? "the server closed the connection before answering"
                    : "reading the upgrade response failed");
            c->failed = 1;
            return rc;
        }
    }
}

/* ============================================================
 *  Writing frames
 * ============================================================ */
static int frame_write(WsConn *c, int opcode, int fin, const void *data,
                       size_t len) {
    uint8_t mask[4];
    int rc = random_bytes(c, mask, sizeof mask);
    if (rc != WS_OK) return rc;

    uint8_t hdr[14];
    size_t hl;
    hdr[0] = (uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    if (len <= 125) {
        hdr[1] = (uint8_t)(0x80 | len);          /* MASK bit always set   */
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    memcpy(hdr + hl, mask, 4);
    hl += 4;

    rc = write_all(c, hdr, hl);
    if (rc != WS_OK) return rc;

    uint32_t key = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) |
                   ((uint32_t)mask[2] << 8) | (uint32_t)mask[3];
    uint8_t chunk[512];
    const uint8_t *p = data;
    for (size_t off = 0; off < len; ) {
        size_t k = len - off;
        if (k > sizeof chunk) k = sizeof chunk;
        memcpy(chunk, p + off, k);
        ws_mask(chunk, k, key, off);
        rc = write_all(c, chunk, k);
        if (rc != WS_OK) return rc;
        off += k;
    }
    return WS_OK;
}

static int sendable(WsConn *c) {
    if (!c) return WS_ERR_INVAL;
    if (!c->hs_done) return WS_ERR_STATE;
    if (c->close_sent || c->failed) return WS_ERR_CLOSED;
    return WS_OK;
}

int ws_send_frame(WsConn *c, int opcode, int fin, const void *data, size_t len) {
    int rc = sendable(c);
    if (rc != WS_OK) return rc;
    if (len && !data) return WS_ERR_INVAL;
    switch (opcode) {
    case WS_OP_TEXT:
    case WS_OP_BINARY:
        if (c->send_frag) return WS_ERR_STATE;
        if (opcode == WS_OP_TEXT) {
            if (fin) {
                if (!utf8_valid(data, len)) return WS_ERR_UTF8;
            } else {
                /* A fragmented text message is checked as it is fed: a
                 * fragment may end in the middle of a codepoint. */
                c->send_cp_need = 0;
                c->send_cp = 0;
                c->send_cp_seq = 0;
                if (!utf8_feed(&c->send_cp_need, &c->send_cp,
                               &c->send_cp_seq, data, len))
                    return WS_ERR_UTF8;
            }
        }
        break;
    case WS_OP_CONT:
        if (!c->send_frag) return WS_ERR_STATE;
        if (c->send_frag == WS_OP_TEXT) {
            if (!utf8_feed(&c->send_cp_need, &c->send_cp,
                           &c->send_cp_seq, data, len))
                return WS_ERR_UTF8;
            if (fin && c->send_cp_need != 0) return WS_ERR_UTF8;
        }
        break;
    case WS_OP_PING:
    case WS_OP_PONG:
        if (!fin || len > 125) return WS_ERR_INVAL;
        break;
    case WS_OP_CLOSE:
        return WS_ERR_INVAL;      /* ws_send_close owns the bookkeeping */
    default:
        return WS_ERR_INVAL;
    }
    rc = frame_write(c, opcode, fin, data, len);
    if (rc == WS_ERR_IO) c->failed = 1;
    if (rc != WS_OK) return rc;
    if (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY)
        c->send_frag = fin ? 0 : opcode;
    else if (opcode == WS_OP_CONT)
        c->send_frag = fin ? 0 : c->send_frag;
    /* A control frame between fragments must not disturb the text
     * validator, so reset it only when a data message ends. */
    if (fin && (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY ||
                opcode == WS_OP_CONT)) {
        c->send_cp_need = 0;
        c->send_cp = 0;
        c->send_cp_seq = 0;
    }
    return WS_OK;
}

int ws_send(WsConn *c, int opcode, const void *data, size_t len) {
    int rc = sendable(c);
    if (rc != WS_OK) return rc;
    if (opcode != WS_OP_TEXT && opcode != WS_OP_BINARY) return WS_ERR_INVAL;
    if (c->send_frag) return WS_ERR_STATE;
    if (len && !data) return WS_ERR_INVAL;
    if (opcode == WS_OP_TEXT && !utf8_valid(data, len)) return WS_ERR_UTF8;
    rc = frame_write(c, opcode, 1, data, len);
    if (rc == WS_ERR_IO) c->failed = 1;
    return rc;
}

int ws_send_text(WsConn *c, const char *text) {
    if (!text) return WS_ERR_INVAL;
    return ws_send(c, WS_OP_TEXT, text, strlen(text));
}

static int control_send(WsConn *c, int opcode, const void *data, size_t len) {
    int rc = sendable(c);
    if (rc != WS_OK) return rc;
    if (len > 125) return WS_ERR_INVAL;   /* §5.5 control frame limit */
    if (len && !data) return WS_ERR_INVAL;
    rc = frame_write(c, opcode, 1, data, len);
    if (rc == WS_ERR_IO) c->failed = 1;
    return rc;
}

int ws_ping(WsConn *c, const void *data, size_t len) {
    return control_send(c, WS_OP_PING, data, len);
}

int ws_pong(WsConn *c, const void *data, size_t len) {
    return control_send(c, WS_OP_PONG, data, len);
}

/* §7.4: the codes an endpoint may put in a Close frame.  1004 is
 * reserved, 1005/1006/1015 are "MUST NOT be set as a status code in a
 * Close control frame", 1012-2999 are unassigned, and 3000-4999 are
 * the registered/private ranges.  A wire code of 0 is not a code at
 * all: that would be an empty payload, which ws_send_close sends when
 * asked for code 0. */
static int close_code_ok(uint16_t code) {
    if (code >= 1000 && code <= 1003) return 1;
    if (code >= 1007 && code <= 1011) return 1;
    if (code >= 3000 && code <= 4999) return 1;
    return 0;
}

int ws_send_close(WsConn *c, uint16_t code, const char *reason) {
    if (!c) return WS_ERR_INVAL;
    if (!c->hs_done || c->close_sent || c->failed) return WS_ERR_CLOSED;
    size_t rl = reason ? strlen(reason) : 0;
    if (code == 0) {
        if (rl) return WS_ERR_INVAL;      /* a reason needs a status code */
    } else if (!close_code_ok(code)) {
        return WS_ERR_INVAL;
    }
    if (rl > 123) return WS_ERR_INVAL;    /* 2 + 123 = the 125-byte limit */
    uint8_t p[125];
    size_t n = 0;
    if (code) {
        p[0] = (uint8_t)(code >> 8);
        p[1] = (uint8_t)code;
        n = 2;
    }
    if (rl) {
        if (!utf8_valid((const uint8_t *)reason, rl)) return WS_ERR_UTF8;
        memcpy(p + n, reason, rl);
        n += rl;
    }
    int rc = frame_write(c, WS_OP_CLOSE, 1, p, n);
    if (rc == WS_ERR_IO) { c->failed = 1; return rc; }
    if (rc != WS_OK) return rc;
    c->close_sent = 1;
    c->close_code_sent = code;
    return WS_OK;
}

/* ============================================================
 *  Reading frames
 * ============================================================ */
static int msg_reserve(WsConn *c, size_t need) {
    if (need <= c->msgcap) return WS_OK;
    size_t cap = c->msgcap ? c->msgcap : 1024;
    while (cap < need) {
        if (cap > c->max_message / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap > c->max_message) cap = c->max_message;
    if (cap < need) return WS_ERR_TOOBIG;
    uint8_t *p = realloc(c->msg, cap);
    if (!p) return WS_ERR_NOMEM;
    c->msg = p;
    c->msgcap = cap;
    return WS_OK;
}

/* Parse and validate the header just read, and decide where its payload
 * goes.  Everything that makes a frame illegal for a client to receive
 * is refused here, before a byte of payload is buffered: RSV bits, an
 * unknown opcode, a masked server frame (§5.1), a fragmented or
 * oversized control frame (§5.5), a 64-bit length with the top bit set
 * (§5.2), a continuation with nothing to continue (§5.4), a new message
 * while one is half assembled, and a message that would exceed the
 * caller's limit. */
static int frame_begin(WsConn *c) {
    WsFrame *f = &c->fr;
    uint8_t b0 = f->hdr[0], b1 = f->hdr[1];
    int fin = (b0 & 0x80) != 0;
    int op = b0 & 0x0F;
    int len7 = b1 & 0x7F;
    int masked = (b1 & 0x80) != 0;

    f->hdr_need = 0;
    f->hdr_have = 0;
    f->fin = fin;
    f->opcode = op;
    f->got = 0;

    if (b0 & 0x70) return WS_ERR_PROTO;        /* §5.2: no extension in use */
    if (op != WS_OP_CONT && op != WS_OP_TEXT && op != WS_OP_BINARY &&
        op != WS_OP_CLOSE && op != WS_OP_PING && op != WS_OP_PONG)
        return WS_ERR_PROTO;
    if (masked) return WS_ERR_PROTO;           /* §5.1: server MUST NOT mask */

    uint64_t len;
    if (len7 == 126) {
        len = ((uint64_t)f->hdr[2] << 8) | f->hdr[3];
        /* §5.2: "the minimal number of bytes MUST be used to encode the
         * length".  A 125-byte payload in the 16-bit form is a protocol
         * error even though it decodes to the right number. */
        if (len < 126) return WS_ERR_PROTO;
    } else if (len7 == 127) {
        if (f->hdr[2] & 0x80) return WS_ERR_PROTO;   /* §5.2: top bit is 0 */
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | f->hdr[2 + i];
        if (len <= 65535) return WS_ERR_PROTO;       /* §5.2: minimal form   */
    } else {
        len = (uint64_t)len7;
    }

    if (op & 0x08) {                           /* control frame */
        /* §5.5: control frames MUST NOT be fragmented and are limited to
         * 125 bytes of payload (so they always fit in one unfragmented
         * frame even when a data message is in pieces around them). */
        if (!fin || len > 125) return WS_ERR_PROTO;
        f->dst = c->ctl;
        f->len = (size_t)len;
        f->active = 1;
        return WS_OK;
    }

    if (op == WS_OP_CONT) {
        if (!c->assembling) return WS_ERR_PROTO;    /* §5.4 */
    } else {
        if (c->assembling) return WS_ERR_PROTO;
        if (!fin) {
            c->assembling = 1;
            c->msg_opcode = op;
        }
    }
    if (len > (uint64_t)c->max_message ||
        len > (uint64_t)(c->max_message - c->msglen))
        return WS_ERR_TOOBIG;
    size_t need = c->msglen + (size_t)len;
    int rc = msg_reserve(c, need);
    if (rc != WS_OK) return rc;
    f->dst = len ? c->msg + c->msglen : NULL;
    f->len = (size_t)len;
    f->active = 1;
    return WS_OK;
}

static int close_received(WsConn *c, const WsFrame *f, WsMessage *m) {
    /* §5.5.1: the payload is empty, or a 2-byte code plus a UTF-8 reason.
     * A 1-byte payload is neither. */
    if (f->len == 1) return WS_ERR_PROTO;
    uint16_t code = 0;
    if (f->len >= 2) {
        code = (uint16_t)(((uint16_t)c->ctl[0] << 8) | c->ctl[1]);
        if (!close_code_ok(code)) return WS_ERR_PROTO;          /* §7.4   */
        if (!utf8_valid(c->ctl + 2, f->len - 2)) return WS_ERR_UTF8;
    }
    if (!c->close_sent) {
        /* §5.5.1: answer a Close that arrived first, echoing the code.
         * If this cannot be sent the connection is over anyway. */
        (void)ws_send_close(c, code, NULL);
    }
    c->recv_closed = 1;
    m->opcode = WS_OP_CLOSE;
    m->data = c->ctl;
    m->len = f->len;
    m->close_code = code;
    return WS_OK;
}

static int frame_finish(WsConn *c, WsMessage *m) {
    WsFrame *f = &c->fr;
    f->active = 0;
    int op = f->opcode;

    if (op == WS_OP_PING) {
        /* §5.5.3: the pong must carry the ping's application data. */
        if (!c->close_sent) {
            int rc = ws_pong(c, c->ctl, f->len);
            if (rc != WS_OK) return rc;
        }
        m->opcode = WS_OP_PING;
        m->data = c->ctl;
        m->len = f->len;
        return WS_OK;
    }
    if (op == WS_OP_PONG) {
        m->opcode = WS_OP_PONG;
        m->data = c->ctl;
        m->len = f->len;
        return WS_OK;
    }
    if (op == WS_OP_CLOSE) return close_received(c, f, m);

    c->msglen += f->len;
    if (!f->fin) return WS_RECV_MORE;
    int mop = (op == WS_OP_CONT) ? c->msg_opcode : op;
    if (mop == WS_OP_TEXT && !utf8_valid(c->msg, c->msglen))
        return WS_ERR_UTF8;
    m->opcode = mop;
    m->data = c->msg;
    m->len = c->msglen;
    c->assembling = 0;
    c->msglen = 0;
    c->msg_opcode = 0;
    return WS_OK;
}

static int recv_transport_fail(WsConn *c, int rc) {
    if (rc == WS_ERR_AGAIN) return rc;         /* state kept, caller retries */
    c->recv_closed = 1;
    if (rc == WS_ERR_EOF) return WS_ERR_EOF;
    c->failed = 1;
    return WS_ERR_IO;
}

/* A protocol error means §7.1.7: fail the connection with a Close that
 * names the reason.  Best effort, because if the transport is the
 * problem there is nowhere to send it. */
static int proto_fail(WsConn *c, int rc) {
    if (rc == WS_ERR_NOMEM) {
        c->failed = 1;
        c->recv_closed = 1;
        return rc;
    }
    uint16_t code = WS_CLOSE_PROTOCOL_ERROR;
    if (rc == WS_ERR_UTF8)  code = WS_CLOSE_BAD_PAYLOAD;
    if (rc == WS_ERR_TOOBIG) code = WS_CLOSE_TOO_BIG;
    if (!c->close_sent) (void)ws_send_close(c, code, NULL);
    c->failed = 1;
    c->recv_closed = 1;
    return rc;
}

int ws_recv(WsConn *c, WsMessage *m) {
    if (!c || !m) return WS_ERR_INVAL;
    memset(m, 0, sizeof *m);
    if (!c->hs_done) return WS_ERR_STATE;
    if (c->recv_closed || c->failed) return WS_ERR_CLOSED;

    for (;;) {
        WsFrame *f = &c->fr;
        if (!f->active) {
            if (f->hdr_need == 0) f->hdr_need = 2;
            int rc = read_exact(c, f->hdr, f->hdr_need, &f->hdr_have);
            if (rc != WS_OK) return recv_transport_fail(c, rc);
            if (f->hdr_need == 2) {
                int len7 = f->hdr[1] & 0x7F;
                /* Two violations are already visible in the two fixed
                 * header bytes, so fail now instead of waiting for
                 * extended length or mask bytes that cannot make the
                 * frame legal: §5.1 forbids a masked server frame, and
                 * §5.5 caps a control frame at 125 bytes, which the
                 * 7-bit form always encodes (126/127 mean it is over). */
                if ((f->hdr[1] & 0x80) ||
                    ((f->hdr[0] & 0x08) && len7 > 125))
                    return proto_fail(c, WS_ERR_PROTO);
                size_t extra = len7 == 126 ? 2 : len7 == 127 ? 8 : 0;
                if (extra) {
                    f->hdr_need = 2 + extra;
                    continue;
                }
            }
            rc = frame_begin(c);
            if (rc != WS_OK) return proto_fail(c, rc);
            continue;
        }
        if (f->got < f->len) {
            int rc = read_exact(c, f->dst, f->len, &f->got);
            if (rc != WS_OK) return recv_transport_fail(c, rc);
        }
        int rc = frame_finish(c, m);
        if (rc == WS_RECV_MORE) continue;
        if (rc != WS_OK) return proto_fail(c, rc);
        return WS_OK;
    }
}

/* ============================================================
 *  Lifecycle and reporting
 * ============================================================ */
WsConn *ws_conn_new(const WsConfig *cfg) {
    if (!cfg || !cfg->tr.read || !cfg->tr.write) return NULL;
    WsConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->tr = cfg->tr;
    c->max_message = cfg->max_message ? cfg->max_message
                                      : (size_t)WS_DEFAULT_MAX_MESSAGE;
    c->rnd = cfg->rand;
    c->rnd_ctx = cfg->rand_ctx;
    c->ibuf = malloc(WS_MAX_HANDSHAKE);
    if (!c->ibuf) {
        free(c);
        return NULL;
    }
    c->icap = WS_MAX_HANDSHAKE;
    return c;
}

void ws_conn_free(WsConn *c) {
    if (!c) return;
    if (c->urandom) fclose(c->urandom);
    free(c->msg);
    free(c->ibuf);
    free(c);
}

int ws_is_closed(const WsConn *c) {
    if (!c) return 1;
    return c->close_sent || c->recv_closed || c->failed;
}

int ws_close_sent(const WsConn *c) {
    return c ? c->close_sent : 0;
}

const char *ws_strerror(int rc) {
    switch (rc) {
    case WS_OK:            return "ok";
    case WS_ERR_INVAL:     return "invalid argument";
    case WS_ERR_NOMEM:     return "out of memory";
    case WS_ERR_AGAIN:     return "no bytes available yet";
    case WS_ERR_EOF:       return "the transport ended without a Close frame";
    case WS_ERR_IO:        return "transport read or write failed";
    case WS_ERR_HANDSHAKE: return "the upgrade response did not satisfy RFC 6455 §4.1";
    case WS_ERR_STATUS:    return "the server answered with a status other than 101";
    case WS_ERR_PROTO:     return "the peer violated RFC 6455";
    case WS_ERR_TOOBIG:    return "the message is larger than max_message";
    case WS_ERR_UTF8:      return "a text payload is not valid UTF-8";
    case WS_ERR_CLOSED:    return "the connection is closed";
    case WS_ERR_STATE:     return "the call is out of order for this connection";
    case WS_ERR_RANDOM:    return "no masking key could be produced";
    case WS_ERR_NOSPACE:   return "the caller's buffer is too small";
    default:               return "unknown WebSocket error";
    }
}

const char *ws_opcode_name(int opcode) {
    switch (opcode) {
    case WS_OP_CONT:   return "continuation";
    case WS_OP_TEXT:   return "text";
    case WS_OP_BINARY: return "binary";
    case WS_OP_CLOSE:  return "close";
    case WS_OP_PING:   return "ping";
    case WS_OP_PONG:   return "pong";
    default:           return "unknown";
    }
}
