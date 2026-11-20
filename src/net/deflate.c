/* ============================================================
 *  Mystic OS - src/net/deflate.c
 *  DEFLATE (RFC 1951) decompressor with the zlib (RFC 1950) and
 *  gzip (RFC 1952) wrappers.  Written from the RFCs; nothing here
 *  is linked from zlib, miniz or any other compress library, and
 *  the checksums are computed by this file.
 *
 *  Design notes
 *  ------------
 *  - The decoder is a resumable state machine.  It never consumes a
 *    symbol it cannot finish: before decoding anything that may need
 *    more bits than are buffered it takes a mark (bit buffer, bit
 *    count, input position) and restores it if the input runs dry.
 *    That is what makes one-byte input fragmentation work without a
 *    second copy of the decoder.
 *  - It never writes output it cannot deliver.  A literal is decoded
 *    only when the caller's output buffer has room; a match that does
 *    not fit is suspended in DF_ST_MATCH and resumed on the next call.
 *    So a one-byte output buffer is legal too.
 *  - The 32 KiB window is written for every output byte, which makes
 *    overlapping matches (distance < length) trivially correct: the
 *    RFC's copy is byte by byte, and we do exactly that.
 *  - Input pushed by the caller is appended to an internal buffer and
 *    compacted after each df_feed(), so the buffer only ever holds
 *    the bytes the decoder could not use yet (a few dozen at most,
 *    plus whatever arrived in this call).
 *
 *  Every constant below carries the RFC section it comes from.
 * ============================================================ */

#include "net/deflate.h"

#include <stdlib.h>
#include <string.h>

/* ---- sizes and limits, all from RFC 1951 ---------------------------- */

/* section 3.2.5: "the distances are 1..32768" and the window is 32K */
#define DF_WIN      32768u
#define DF_WIN_MASK 32767u
/* section 3.2.7: code lengths are 1..15 bits */
#define DF_MAXBITS  15
/* section 3.2.7: HLIT+257 is 257..286, HDIST+1 is 1..32 */
#define DF_MAXLIT   286
#define DF_MAXDIST  32
/* section 3.2.7: HCLEN+4 code lengths, in this order (the permutation) */
#define DF_CLENS    19
/* the literal/length alphabet of the fixed tree has 288 entries
 * (section 3.2.6); 286 and 287 are never valid symbols but they do
 * have codes, so the table needs room for them. */
#define DF_FIXED_SYMS 288

static const uint8_t DF_CLEN_ORDER[DF_CLENS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* section 3.2.5, the length codes 257..285: base length and extra bits.
 * The last entry (285) is the 258-byte code with no extra bits. */
static const uint16_t DF_LBASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t DF_LEXT[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
/* section 3.2.5, the distance codes 0..29: base distance and extra
 * bits.  Codes 30 and 31 exist in the fixed tree but are reserved. */
static const uint16_t DF_DBASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t DF_DEXT[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};

/* ---- states -------------------------------------------------------- */
enum {
    DF_ST_HEADER = 0,
    DF_ST_BLOCK,
    DF_ST_STORED,
    DF_ST_CODES,
    DF_ST_MATCH,
    DF_ST_TRAILER,
    DF_ST_DONE,
    DF_ST_ERROR
};

/* gzip header sub-states (RFC 1952 section 2.3) */
enum {
    GZ_FIXED = 0,     /* ID1 ID2 CM FLG MTIME(4) XFL OS  = 10 bytes */
    GZ_EXTRA_LEN,     /* FEXTRA: XLEN, little endian */
    GZ_EXTRA,         /* FEXTRA: XLEN bytes, skipped */
    GZ_NAME,          /* FNAME: zero terminated */
    GZ_COMMENT,       /* FCOMMENT: zero terminated */
    GZ_HCRC,          /* FHCRC: low 16 bits of the header's crc32 */
    GZ_END            /* header done: deflate data follows */
};

/* ---- Huffman canonical code --------------------------------------- */
/* Built the way RFC 1951 section 3.2.2 describes: count the codes of
 * each length, then assign symbols in order of length and symbol. */
typedef struct {
    uint16_t count[DF_MAXBITS + 1];
    uint16_t symbol[DF_FIXED_SYMS];
    int      empty;               /* no codes at all */
} DfHuff;

/* what the tree is for; the incomplete-tree rule depends on it */
enum { DF_KIND_CLEN = 0, DF_KIND_LENS = 1, DF_KIND_DIST = 2 };

/* ---- the stream ---------------------------------------------------- */
struct DfStream {
    /* configuration */
    int      wrap;            /* DF_WRAP_*, may be resolved from AUTO */
    int      ended;           /* df_end_input() was called */
    size_t   limit;           /* 0 = no output bound */
    uint32_t dist_limit;      /* largest legal distance, RFC 1950 CINFO */

    /* input */
    uint8_t *ibuf;
    size_t   ilen, ipos, icap;   /* IPOS is the next unread byte */

    /* bit reader; LSB-first, exactly as RFC 1951 section 3.1.1 packs it */
    uint64_t bitbuf;
    int      bitcnt;

    /* output history and literals */
    uint8_t  win[DF_WIN];
    size_t   wpos;            /* next write index in the ring */
    size_t   whave;           /* valid bytes in the ring (<= DF_WIN) */

    /* state machine */
    int      state;
    int      last_block;      /* BFINAL of the block being decoded */
    int      err;             /* sticky error */
    int      fixed_ready;

    /* pending output: a stored block, a suspended match, or one literal
     * that was decoded when the caller's buffer was full */
    uint32_t stored_left;
    uint32_t match_left;
    uint32_t match_dist;
    int      have_pend;
    uint8_t  pend;

    /* wrapper decoding */
    int      gz_sub;
    uint32_t gz_flg;
    uint32_t gz_left;         /* FEXTRA bytes still to skip */
    size_t   gz_scan;         /* FNAME/FCOMMENT bytes already checked */
    uint32_t hdr_crc;         /* crc32 of the gzip header so far */
    uint32_t tr_crc;          /* gzip trailer crc, kept across a resume */
    int      tr_have;         /* tr_crc holds the crc of this member */

    /* running checksums over the output */
    uint32_t crc;
    uint32_t adler_s1, adler_s2;
    uint32_t isize;

    DfHuff   lit, dist;
    DfStats  st;
};

/* defined with the bit reader below; needed by df_decode */
static int df_want_bits(DfStream *s, int n);

/* Which optional gzip field comes next, in FLG order (RFC 1952 2.3). */
#define GZ_NEXT_FIELD(s) \
    (((s)->gz_flg & 0x08u) ? GZ_NAME : \
     ((s)->gz_flg & 0x10u) ? GZ_COMMENT : \
     ((s)->gz_flg & 0x02u) ? GZ_HCRC : GZ_END)

/* ============================================================
 *  Checksums
 * ============================================================ */

/* CRC-32, the reflected polynomial 0xEDB88320 that RFC 1952 section 8
 * names (crc32 as used by gzip and PNG).  The table is built once, on
 * first use; every racer writes identical values. */
static uint32_t df_crc_table[256];
static int      df_crc_ready;

static void df_crc_build(void) {
    uint32_t n, c;
    int k;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        df_crc_table[n] = c;
    }
    df_crc_ready = 1;
}

static uint32_t df_crc_byte(uint32_t c, uint8_t b) {
    if (!df_crc_ready) df_crc_build();
    return df_crc_table[(c ^ b) & 0xffu] ^ (c >> 8);
}

uint32_t df_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    size_t i;
    if (!df_crc_ready) df_crc_build();
    for (i = 0; i < len; i++) c = df_crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return ~c;
}

/* adler32, RFC 1950 section 9: two sums modulo 65521, s1 starting at 1.
 * The conditional subtract keeps both sums below 65521 after every
 * byte, so neither can ever overflow a uint32. */
uint32_t df_adler32(uint32_t adler, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t s1 = adler & 0xffffu, s2 = (adler >> 16) & 0xffffu;
    size_t i;
    if (s1 == 0 && s2 == 0) s1 = 1;          /* the RFC's initial value */
    for (i = 0; i < len; i++) {
        s1 += p[i];
        if (s1 >= 65521u) s1 -= 65521u;
        s2 += s1;
        if (s2 >= 65521u) s2 -= 65521u;
    }
    return (s2 << 16) | s1;
}

/* one output byte into the running checksum of the current member */
static void df_check_byte(DfStream *s, uint8_t b) {
    if (s->wrap == DF_WRAP_GZIP) {
        s->crc = df_crc_byte(s->crc, b);
        s->isize++;
    } else {
        s->adler_s1 += b;
        if (s->adler_s1 >= 65521u) s->adler_s1 -= 65521u;
        s->adler_s2 += s->adler_s1;
        if (s->adler_s2 >= 65521u) s->adler_s2 -= 65521u;
    }
}

/* ============================================================
 *  Huffman trees
 * ============================================================ */

/* Build a canonical code from code lengths.  Returns DF_OK, or
 * DF_ERR_HUFF_OVER for an over-subscribed set (more codes than the
 * bit space allows) or DF_ERR_HUFF_INCOMPLETE for an incomplete set
 * (some bit patterns are not a code) when that is not allowed.
 *
 * RFC 1951 section 3.2.7 legalises exactly one incomplete case: a
 * single code of length one ("If only one distance code is used, it
 * is encoded using one bit").  The code-length alphabet itself must
 * always be complete, and an alphabet with no codes at all is legal
 * (a block with no matches never decodes a distance) - any attempt to
 * use it then fails as DF_ERR_BAD_CODE. */
static int df_huff_build(DfHuff *h, const uint8_t *lens, int n, int kind) {
    int i, len, left, max = 0;
    uint16_t offs[DF_MAXBITS + 1];

    memset(h, 0, sizeof *h);
    for (i = 0; i < n; i++) {
        if (lens[i] > DF_MAXBITS) return DF_ERR_BAD_CLEN;
        h->count[lens[i]]++;
    }
    if (h->count[0] == n) { h->empty = 1; return DF_OK; }

    left = 1;
    for (len = 1; len <= DF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return DF_ERR_HUFF_OVER;
        if (h->count[len]) max = len;
    }
    if (left > 0 && !(kind != DF_KIND_CLEN && max == 1))
        return DF_ERR_HUFF_INCOMPLETE;

    offs[1] = 0;
    for (len = 1; len < DF_MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    return DF_OK;
}

/* Decode one symbol.  The caller owns the mark / rollback: on
 * DF_NEED_INPUT or DF_ERR_TRUNCATED some bits have been consumed, so
 * the mark taken before the call must be restored.  Codes are packed
 * most-significant-bit first (RFC 1951 section 3.1.1), while the bit
 * stream itself is least-significant-bit first, hence the shifting. */
static int df_decode(DfStream *s, const DfHuff *h, int *out) {
    int len, code = 0, first = 0, index = 0;
    if (h->empty) return DF_ERR_BAD_CODE;
    for (len = 1; len <= DF_MAXBITS; len++) {
        int r = df_want_bits(s, 1);
        int count;
        if (r != DF_OK) return r;
        code |= (int)(s->bitbuf & 1u);
        s->bitbuf >>= 1;
        s->bitcnt--;
        count = h->count[len];
        if (code - first < count) {
            *out = h->symbol[index + (code - first)];
            return DF_OK;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return DF_ERR_BAD_CODE;      /* no code of 15 bits or fewer matches */
}

/* The fixed tree of RFC 1951 section 3.2.6, as code lengths. */
static void df_build_fixed(DfStream *s) {
    uint8_t lit[DF_FIXED_SYMS], dst[DF_MAXDIST];
    int i;
    for (i = 0; i < 144; i++) lit[i] = 8;
    for (; i < 256; i++) lit[i] = 9;
    for (; i < 280; i++) lit[i] = 7;
    for (; i < DF_FIXED_SYMS; i++) lit[i] = 8;
    for (i = 0; i < DF_MAXDIST; i++) dst[i] = 5;
    (void)df_huff_build(&s->lit, lit, DF_FIXED_SYMS, DF_KIND_LENS);
    (void)df_huff_build(&s->dist, dst, DF_MAXDIST, DF_KIND_DIST);
    s->fixed_ready = 1;
}

/* ============================================================
 *  Input and bit reader
 * ============================================================ */

typedef struct {
    uint64_t bitbuf;
    int      bitcnt;
    size_t   ipos;
} DfMark;

static DfMark df_mark(const DfStream *s) {
    DfMark m;
    m.bitbuf = s->bitbuf;
    m.bitcnt = s->bitcnt;
    m.ipos = s->ipos;
    return m;
}

static void df_unmark(DfStream *s, DfMark m) {
    s->bitbuf = m.bitbuf;
    s->bitcnt = m.bitcnt;
    s->ipos = m.ipos;
}

/* Make at least n bits available.  All-or-nothing in the sense that a
 * failure only ever *adds* buffered bits; the caller restores its mark
 * when it does not get DF_OK, so the net effect is no change. */
static int df_want_bits(DfStream *s, int n) {
    while (s->bitcnt < n) {
        if (s->ipos >= s->ilen) return s->ended ? DF_ERR_TRUNCATED : DF_NEED_INPUT;
        s->bitbuf |= (uint64_t)s->ibuf[s->ipos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    return DF_OK;
}

/* Consume n bits (n <= 32; df_want_bits must have succeeded). */
static uint32_t df_take(DfStream *s, int n) {
    uint32_t v;
    if (n <= 0) return 0;
    v = (uint32_t)(s->bitbuf & ((1ULL << n) - 1ULL));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* Drop bits up to the next byte boundary (RFC 1951 section 3.2.3 for
 * stored blocks and the end of the last block before a wrapper's
 * trailer, which RFC 1950/1952 store byte-aligned). */
static void df_align(DfStream *s) {
    int drop = s->bitcnt & 7;
    if (drop) {
        s->bitbuf >>= drop;
        s->bitcnt -= drop;
    }
}

/* Give whole buffered bytes back to the input, so a byte-aligned
 * reader (gzip's next member magic, trailing-garbage detection) sees
 * them.  Only legal when bitcnt is a multiple of 8. */
static void df_pushback(DfStream *s) {
    while (s->bitcnt >= 8 && s->ipos > 0) {
        s->ipos--;
        s->ibuf[s->ipos] = (uint8_t)(s->bitbuf & 0xffu);
        s->bitbuf >>= 8;
        s->bitcnt -= 8;
    }
    s->bitbuf = 0;
    s->bitcnt = 0;
}

/* Need n whole bytes at IPOS (no bits buffered). */
static int df_want(DfStream *s, size_t n) {
    if (s->ilen - s->ipos >= n) return DF_OK;
    return s->ended ? DF_ERR_TRUNCATED : DF_NEED_INPUT;
}

static int df_append(DfStream *s, const uint8_t *in, size_t n) {
    if (n > (size_t)-1 - s->ilen) return DF_ERR_NOMEM;
    if (s->ilen + n > s->icap) {
        size_t cap = s->icap ? s->icap : 512;
        uint8_t *p;
        while (cap < s->ilen + n) {
            if (cap > ((size_t)-1) / 2) { cap = s->ilen + n; break; }
            cap *= 2;
        }
        p = (uint8_t *)realloc(s->ibuf, cap);
        if (!p) return DF_ERR_NOMEM;
        s->ibuf = p;
        s->icap = cap;
    }
    memcpy(s->ibuf + s->ilen, in, n);
    s->ilen += n;
    return DF_OK;
}

/* ============================================================
 *  Output
 * ============================================================ */

/* Emit one byte: into the history window always, into the caller's
 * buffer always (df_codes / df_stored / df_match only call this when
 * there is room), and into the running checksum. */
static void df_put(DfStream *s, uint8_t *out, size_t *op, uint8_t b) {
    s->win[s->wpos] = b;
    s->wpos = (s->wpos + 1) & DF_WIN_MASK;
    if (s->whave < DF_WIN) s->whave++;
    out[(*op)++] = b;
    df_check_byte(s, b);
    s->st.total_out++;
}

/* ============================================================
 *  Wrapper headers and trailers
 * ============================================================ */

/* A new deflate stream begins: reset the history and the checksums.
 * Each gzip member is an independent deflate stream, so a distance
 * may never reach into the previous member. */
static void df_begin_stream(DfStream *s) {
    s->wpos = 0;
    s->whave = 0;
    s->last_block = 0;
    s->tr_have = 0;
    s->crc = 0xffffffffu;
    s->adler_s1 = 1;
    s->adler_s2 = 0;
    s->isize = 0;
    s->st.members++;
    s->state = DF_ST_BLOCK;
}

static int df_zlib_header(DfStream *s) {
    unsigned cmf, flg;
    int r = df_want(s, 2);
    if (r != DF_OK) return r;
    cmf = s->ibuf[s->ipos];
    flg = s->ibuf[s->ipos + 1];
    /* RFC 1950 section 2.2 */
    if ((cmf & 0x0fu) != 8) return DF_ERR_BAD_HEADER;         /* CM = deflate */
    if ((cmf >> 4) > 7) return DF_ERR_BAD_HEADER;             /* CINFO <= 7 */
    if (((cmf << 8) | flg) % 31u != 0) return DF_ERR_BAD_HEADER; /* FCHECK */
    if (flg & 0x20u) return DF_ERR_BAD_HEADER;                /* FDICT */
    s->ipos += 2;
    {
        uint32_t w = 1u << ((cmf >> 4) + 8);                  /* 256 .. 32768 */
        if (w < s->dist_limit) s->dist_limit = w;
    }
    df_begin_stream(s);
    return DF_OK;
}

/* gzip header, RFC 1952 section 2.3.  Parsed in sub-states so a
 * fragmented header works exactly like fragmented deflate data.  The
 * FHCRC is the low 16 bits of the crc32 of every header byte up to
 * but not including the CRC16 itself, so the crc is folded in as the
 * bytes are consumed. */
static int df_gzip_header(DfStream *s) {
    for (;;) {
        switch (s->gz_sub) {
        case GZ_FIXED: {
            const uint8_t *p;
            int i, r = df_want(s, 10);
            if (r != DF_OK) return r;
            p = s->ibuf + s->ipos;
            if (p[0] != 0x1f || p[1] != 0x8b) return DF_ERR_BAD_HEADER; /* ID1 ID2 */
            if (p[2] != 8) return DF_ERR_BAD_HEADER;                    /* CM */
            if (p[3] & 0xe0u) return DF_ERR_BAD_HEADER;                 /* reserved */
            s->gz_flg = p[3];
            for (i = 0; i < 10; i++) s->hdr_crc = df_crc_byte(s->hdr_crc, p[i]);
            s->ipos += 10;
            s->gz_scan = 0;
            /* fields appear in this order when their FLG bit is set */
            if (s->gz_flg & 0x04u)      s->gz_sub = GZ_EXTRA_LEN;   /* FEXTRA */
            else if (s->gz_flg & 0x08u) s->gz_sub = GZ_NAME;        /* FNAME */
            else if (s->gz_flg & 0x10u) s->gz_sub = GZ_COMMENT;     /* FCOMMENT */
            else if (s->gz_flg & 0x02u) s->gz_sub = GZ_HCRC;        /* FHCRC */
            else                        s->gz_sub = GZ_END;
            break;
        }
        case GZ_EXTRA_LEN: {
            const uint8_t *p;
            int r = df_want(s, 2);
            if (r != DF_OK) return r;
            p = s->ibuf + s->ipos;
            s->gz_left = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            s->hdr_crc = df_crc_byte(s->hdr_crc, p[0]);
            s->hdr_crc = df_crc_byte(s->hdr_crc, p[1]);
            s->ipos += 2;
            s->gz_sub = s->gz_left ? GZ_EXTRA : GZ_NEXT_FIELD(s);
            break;
        }
        case GZ_EXTRA: {
            size_t avail = s->ilen - s->ipos;
            size_t n = s->gz_left;
            size_t i;
            if (n > avail) n = avail;
            for (i = 0; i < n; i++) s->hdr_crc = df_crc_byte(s->hdr_crc, s->ibuf[s->ipos + i]);
            s->ipos += n;
            s->gz_left -= (uint32_t)n;
            if (s->gz_left) return s->ended ? DF_ERR_TRUNCATED : DF_NEED_INPUT;
            s->gz_sub = GZ_NEXT_FIELD(s);
            break;
        }
        case GZ_NAME:
        case GZ_COMMENT: {
            size_t i = s->gz_scan;
            while (s->ipos + i < s->ilen) {
                uint8_t b = s->ibuf[s->ipos + i];
                s->hdr_crc = df_crc_byte(s->hdr_crc, b);
                i++;
                if (b == 0) {
                    s->ipos += i;
                    s->gz_scan = 0;
                    /* The field is finished, so clear its FLG bit before
                     * asking which field comes next.  GZ_NEXT_FIELD()
                     * reads the FLG bits, so without this a FNAME (or
                     * FCOMMENT) header would ask for another name for
                     * ever and eat the deflate data looking for a zero. */
                    if (s->gz_sub == GZ_NAME) s->gz_flg &= ~0x08u;
                    else                       s->gz_flg &= ~0x10u;
                    s->gz_sub = GZ_NEXT_FIELD(s);
                    goto advanced;
                }
            }
            s->gz_scan = i;    /* remember the bytes already folded in */
            return s->ended ? DF_ERR_TRUNCATED : DF_NEED_INPUT;
        }
        case GZ_HCRC: {
            const uint8_t *p;
            uint32_t want;
            int r = df_want(s, 2);
            if (r != DF_OK) return r;
            p = s->ibuf + s->ipos;
            want = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            /* hdr_crc is the un-inverted LFSR state (the trailer path
             * inverts it the same way); the header's CRC16 is the low
             * 16 bits of the finished CRC32, so invert before the low
             * half is taken. */
            if (want != ((~s->hdr_crc) & 0xffffu)) return DF_ERR_BAD_HEADER;
            s->ipos += 2;
            s->gz_sub = GZ_END;
            break;
        }
        case GZ_END:
            df_begin_stream(s);
            return DF_OK;
        default:
            return DF_ERR_STATE;
        }
advanced:;
    }
}

static int df_header(DfStream *s) {
    int r;
    if (s->wrap == DF_WRAP_AUTO) {
        unsigned b0, b1;
        r = df_want(s, 2);
        if (r != DF_OK) return r;
        b0 = s->ibuf[s->ipos];
        b1 = s->ibuf[s->ipos + 1];
        if (b0 == 0x1f && b1 == 0x8b)
            s->wrap = DF_WRAP_GZIP;
        else if ((b0 & 0x0fu) == 8 && (b0 >> 4) <= 7 && (((b0 << 8) | b1) % 31u) == 0)
            s->wrap = DF_WRAP_ZLIB;
        else
            s->wrap = DF_WRAP_RAW;
        s->st.wrap = s->wrap;
    }
    switch (s->wrap) {
    case DF_WRAP_RAW:
        df_begin_stream(s);
        return DF_OK;
    case DF_WRAP_ZLIB:
        return df_zlib_header(s);
    case DF_WRAP_GZIP:
        return df_gzip_header(s);
    default:
        return DF_ERR_BAD_HEADER;
    }
}

/* zlib's adler32 (RFC 1950 section 2.2, big-endian) and gzip's
 * crc32 + ISIZE (RFC 1952 section 2.3, little-endian). */
static int df_trailer(DfStream *s) {
    int r;
    df_align(s);                       /* padding of the last block */
    if (s->wrap == DF_WRAP_GZIP) {
        uint32_t got, isize;
        /* Two 32-bit fields, and the second may land in a later df_feed
         * call.  Keep the first in the stream: taking it and then
         * returning DF_NEED_INPUT used to lose it, so the resume read
         * ISIZE as the CRC and every 1-byte-fragmented gzip trailer
         * failed. */
        if (!s->tr_have) {
            r = df_want_bits(s, 32);
            if (r != DF_OK) return r;
            s->tr_crc = df_take(s, 32);    /* stored little-endian */
            s->tr_have = 1;
        }
        r = df_want_bits(s, 32);
        if (r != DF_OK) return r;
        got = s->tr_crc;
        isize = df_take(s, 32);
        s->tr_have = 0;
        s->st.crc = ~s->crc;
        s->st.isize = s->isize;
        if (got != s->st.crc) return DF_ERR_BAD_CRC;
        if (isize != s->isize) return DF_ERR_BAD_LENGTH;
    } else {
        uint32_t got, v;
        r = df_want_bits(s, 32);
        if (r != DF_OK) return r;
        v = df_take(s, 32);            /* four bytes, first one first */
        got = ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
              ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
        s->st.adler = (s->adler_s2 << 16) | s->adler_s1;
        if (got != s->st.adler) return DF_ERR_BAD_ADLER;
    }
    df_pushback(s);                    /* bytes beyond the trailer belong to the caller */
    s->state = DF_ST_DONE;
    return DF_OK;
}

/* ============================================================
 *  Block decoding
 * ============================================================ */

static int df_block_done(DfStream *s) {
    if (!s->last_block) { s->state = DF_ST_BLOCK; return DF_OK; }
    s->state = (s->wrap == DF_WRAP_RAW) ? DF_ST_DONE : DF_ST_TRAILER;
    return DF_OK;
}

/* Dynamic Huffman block header, RFC 1951 section 3.2.7.  Everything is
 * parsed into locals and committed only at the end, so a need-input in
 * the middle of the header simply restarts it on the next call. */
static int df_dyn_header(DfStream *s, int bfinal) {
    DfMark mk = df_mark(s);
    uint8_t lens[DF_MAXLIT + DF_MAXDIST];
    uint8_t cl[DF_CLENS];
    DfHuff clh, lh, dh;
    int hlit, hdist, hclen, i, n, r;

    r = df_want_bits(s, 14);           /* HLIT(5) HDIST(5) HCLEN(4) */
    if (r != DF_OK) goto need;
    hlit = (int)df_take(s, 5) + 257;
    hdist = (int)df_take(s, 5) + 1;
    hclen = (int)df_take(s, 4) + 4;
    if (hlit > DF_MAXLIT || hdist > DF_MAXDIST) { r = DF_ERR_BAD_HEADER; goto bad; }

    memset(cl, 0, sizeof cl);
    for (i = 0; i < hclen; i++) {
        r = df_want_bits(s, 3);
        if (r != DF_OK) goto need;
        cl[DF_CLEN_ORDER[i]] = (uint8_t)df_take(s, 3);
    }
    r = df_huff_build(&clh, cl, DF_CLENS, DF_KIND_CLEN);
    if (r != DF_OK) goto bad;

    /* the code lengths of the literal/length and distance alphabets are
     * themselves Huffman coded, with 16/17/18 as repeat codes */
    n = hlit + hdist;
    i = 0;
    while (i < n) {
        int sym;
        r = df_decode(s, &clh, &sym);
        if (r != DF_OK) goto need;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            int rep;
            if (i == 0) { r = DF_ERR_BAD_CLEN; goto bad; }
            r = df_want_bits(s, 2);
            if (r != DF_OK) goto need;
            rep = 3 + (int)df_take(s, 2);
            if (i + rep > n) { r = DF_ERR_BAD_CLEN; goto bad; }
            while (rep-- > 0) {
                lens[i] = lens[i - 1];
                i++;
            }
        } else {
            int rep;
            if (sym == 17) {
                r = df_want_bits(s, 3);
                if (r != DF_OK) goto need;
                rep = 3 + (int)df_take(s, 3);
            } else {
                r = df_want_bits(s, 7);
                if (r != DF_OK) goto need;
                rep = 11 + (int)df_take(s, 7);
            }
            if (i + rep > n) { r = DF_ERR_BAD_CLEN; goto bad; }
            while (rep-- > 0) lens[i++] = 0;
        }
    }

    r = df_huff_build(&lh, lens, hlit, DF_KIND_LENS);
    if (r != DF_OK) goto bad;
    if (lens[256] == 0) { r = DF_ERR_HUFF_INCOMPLETE; goto bad; } /* missing EOB */
    r = df_huff_build(&dh, lens + hlit, hdist, DF_KIND_DIST);
    if (r != DF_OK) goto bad;

    /* commit */
    s->lit = lh;
    s->dist = dh;
    s->last_block = bfinal;
    s->st.blocks++;
    s->st.dynamic_blocks++;
    s->state = DF_ST_CODES;
    return DF_OK;

need:
    df_unmark(s, mk);
    return r;
bad:
    return r;
}

static int df_block(DfStream *s) {
    DfMark mk;
    int r, bfinal, btype;

    mk = df_mark(s);
    r = df_want_bits(s, 3);
    if (r != DF_OK) return r;
    bfinal = (int)(s->bitbuf & 1u);
    btype = (int)((s->bitbuf >> 1) & 3u);
    df_take(s, 3);

    switch (btype) {
    case 0: {                                  /* stored, section 3.2.4 */
        uint32_t len, nlen;
        df_align(s);
        r = df_want_bits(s, 32);
        if (r != DF_OK) { df_unmark(s, mk); return r; }
        len = df_take(s, 16);
        nlen = df_take(s, 16);
        if ((len ^ 0xffffu) != nlen) return DF_ERR_BAD_STORED;
        s->last_block = bfinal;
        s->st.blocks++;
        s->st.stored_blocks++;
        s->stored_left = len;
        s->state = len ? DF_ST_STORED : DF_ST_BLOCK;
        if (!len) return df_block_done(s);
        return DF_OK;
    }
    case 1:                                    /* fixed, section 3.2.6 */
        if (!s->fixed_ready) df_build_fixed(s);
        s->last_block = bfinal;
        s->st.blocks++;
        s->st.fixed_blocks++;
        s->state = DF_ST_CODES;
        return DF_OK;
    case 2:                                    /* dynamic, section 3.2.7 */
        r = df_dyn_header(s, bfinal);
        /* df_dyn_header restores to the start of *its* part, but this
         * function already took the 3-bit block header.  If the dynamic
         * header needs more input the block header must be re-read too,
         * or the next call would decode the header's HLIT/HDIST bits as
         * a new BTYPE (the 1-byte-feed failures: bogus bad-header and
         * incomplete-tree errors on streams that decode fine in one
         * pass).  Restore the whole block, not half of it. */
        if (r == DF_NEED_INPUT) df_unmark(s, mk);
        return r;
    default:                                   /* BTYPE=11 reserved */
        return DF_ERR_BAD_HEADER;
    }
}

/* Stored block body: copy LEN bytes.  Byte aligned by then, but the
 * bit buffer may still hold whole bytes, so drain those first. */
static int df_stored(DfStream *s, uint8_t *out, size_t outcap, size_t *op) {
    while (s->stored_left) {
        size_t room = outcap - *op;
        if (s->limit && s->st.total_out >= s->limit) return DF_ERR_LIMIT;
        if (!room) return DF_NEED_OUTPUT;
        if (s->bitcnt >= 8) {
            df_put(s, out, op, (uint8_t)df_take(s, 8));
            s->stored_left--;
            continue;
        }
        if (s->ipos >= s->ilen) return s->ended ? DF_ERR_TRUNCATED : DF_NEED_INPUT;
        {
            size_t n = s->stored_left;
            size_t i;
            if (n > s->ilen - s->ipos) n = s->ilen - s->ipos;
            if (n > room) n = room;
            if (s->limit && s->limit - (size_t)s->st.total_out < n)
                n = s->limit - (size_t)s->st.total_out;
            for (i = 0; i < n; i++) df_put(s, out, op, s->ibuf[s->ipos++]);
            s->stored_left -= (uint32_t)n;
        }
    }
    return df_block_done(s);
}

/* Huffman-coded block body.  One symbol per iteration; the mark is
 * restored if the symbol cannot be finished, and nothing is written
 * before every bit of the symbol (including the distance) is known. */
static int df_codes(DfStream *s, uint8_t *out, size_t outcap, size_t *op) {
    for (;;) {
        DfMark mk;
        int sym, r, len, dist, ds, extra;

        /* a literal that did not fit in the caller's buffer last time */
        if (s->have_pend) {
            if (s->limit && s->st.total_out >= s->limit) return DF_ERR_LIMIT;
            if (*op >= outcap) return DF_NEED_OUTPUT;
            df_put(s, out, op, s->pend);
            s->have_pend = 0;
            continue;
        }

        mk = df_mark(s);
        r = df_decode(s, &s->lit, &sym);
        if (r != DF_OK) { df_unmark(s, mk); return r; }

        if (sym < 256) {                        /* literal byte */
            if (s->limit && s->st.total_out >= s->limit) return DF_ERR_LIMIT;
            s->st.literals++;
            if (*op >= outcap) {                /* keep it, never drop it */
                s->pend = (uint8_t)sym;
                s->have_pend = 1;
                return DF_NEED_OUTPUT;
            }
            df_put(s, out, op, (uint8_t)sym);
            continue;
        }
        if (sym == 256) {                       /* end of block */
            return df_block_done(s);
        }
        if (sym > 285) { return DF_ERR_BAD_CODE; } /* 286, 287: reserved */

        len = DF_LBASE[sym - 257];
        extra = DF_LEXT[sym - 257];
        if (extra) {
            r = df_want_bits(s, extra);
            if (r != DF_OK) { df_unmark(s, mk); return r; }
            len += (int)df_take(s, extra);
        }
        r = df_decode(s, &s->dist, &ds);
        if (r != DF_OK) { df_unmark(s, mk); return r; }
        if (ds > 29) { return DF_ERR_BAD_CODE; }  /* 30, 31: reserved */
        dist = DF_DBASE[ds];
        extra = DF_DEXT[ds];
        if (extra) {
            r = df_want_bits(s, extra);
            if (r != DF_OK) { df_unmark(s, mk); return r; }
            dist += (int)df_take(s, extra);
        }
        /* RFC 1951 section 3.2.3: a distance may not reach before the
         * start of the output, and RFC 1950 section 2.2: it may not
         * exceed the window the zlib header declared. */
        if ((uint32_t)dist > s->whave) return DF_ERR_BAD_DISTANCE;
        if ((uint32_t)dist > s->dist_limit) return DF_ERR_BAD_DISTANCE;

        s->match_left = (uint32_t)len;
        s->match_dist = (uint32_t)dist;
        s->st.matches++;
        s->st.match_bytes += (uint64_t)len;
        if ((uint32_t)len > s->st.max_match) s->st.max_match = (uint32_t)len;
        if ((uint32_t)dist > s->st.max_distance) s->st.max_distance = (uint32_t)dist;
        s->state = DF_ST_MATCH;
        return DF_OK;
    }
}

/* Copy a suspended match, byte by byte so that overlapping matches
 * (distance < length) come out right. */
static int df_match(DfStream *s, uint8_t *out, size_t outcap, size_t *op) {
    while (s->match_left) {
        uint8_t b;
        if (s->limit && s->st.total_out >= s->limit) return DF_ERR_LIMIT;
        if (*op >= outcap) return DF_NEED_OUTPUT;
        b = s->win[(s->wpos + DF_WIN - s->match_dist) & DF_WIN_MASK];
        df_put(s, out, op, b);
        s->match_left--;
    }
    s->state = DF_ST_CODES;
    return DF_OK;
}

/* ============================================================
 *  The state machine
 * ============================================================ */

static int df_step(DfStream *s, uint8_t *out, size_t outcap, size_t *op) {
    switch (s->state) {
    case DF_ST_HEADER:  return df_header(s);
    case DF_ST_BLOCK:   return df_block(s);
    case DF_ST_STORED:  return df_stored(s, out, outcap, op);
    case DF_ST_CODES:   return df_codes(s, out, outcap, op);
    case DF_ST_MATCH:   return df_match(s, out, outcap, op);
    case DF_ST_TRAILER: return df_trailer(s);
    case DF_ST_DONE: {
        /* A complete stream.  Nothing pending -> done.  gzip may have
         * another member right behind this one; anything else is
         * trailing garbage and is named as such. */
        size_t avail = s->ilen - s->ipos;
        if (avail == 0) return DF_DONE;
        if (s->wrap != DF_WRAP_GZIP) return DF_ERR_TRAILING;
        if (avail < 2) return s->ended ? DF_ERR_TRAILING : DF_NEED_INPUT;
        if (s->ibuf[s->ipos] != 0x1f || s->ibuf[s->ipos + 1] != 0x8b)
            return DF_ERR_TRAILING;
        s->gz_sub = GZ_FIXED;
        s->hdr_crc = 0xffffffffu;
        s->gz_scan = 0;
        s->state = DF_ST_HEADER;
        return DF_OK;
    }
    default:
        return DF_ERR_STATE;
    }
}

int df_feed(DfStream *s,
            const uint8_t *in, size_t inlen, size_t *in_used,
            uint8_t *out, size_t outcap, size_t *out_used) {
    size_t ip0, op = 0;
    int rc = DF_OK;

    if (!s) return DF_ERR_ARG;
    if (inlen && !in) return DF_ERR_ARG;
    if (outcap && !out) return DF_ERR_ARG;

    if (in_used) *in_used = 0;
    if (out_used) *out_used = 0;
    if (s->state == DF_ST_ERROR) return s->err;

    if (inlen) {
        rc = df_append(s, in, inlen);
        if (rc != DF_OK) return rc;
    }
    ip0 = s->ipos;
    /* Every byte handed over was copied into ibuf, so all of them are
     * taken.  Report inlen, not the parse position: a rollback (see the
     * mark/restore in df_codes) leaves IPOS where it was, and reporting
     * that as "unused" made a caller that re-feeded the tail duplicate
     * bytes already buffered - every fragmented stream decoded wrong.
     * The caller may therefore drop the whole buffer. */
    if (in_used) *in_used = inlen;

    for (;;) {
        rc = df_step(s, out, outcap, &op);
        if (rc != DF_OK) break;
    }

    s->st.total_in += (uint64_t)(s->ipos - ip0);
    if (out_used) *out_used = op;
    if (rc < 0) {
        if (s->err == DF_OK) s->err = rc;   /* first error wins */
        s->state = DF_ST_ERROR;
        rc = s->err;
    }

    /* keep only the bytes the decoder has not used yet */
    if (s->ipos) {
        size_t left = s->ilen - s->ipos;
        if (left) memmove(s->ibuf, s->ibuf + s->ipos, left);
        s->ilen = left;
        s->ipos = 0;
    }
    return rc;
}

/* ============================================================
 *  Lifecycle, limit, one-shot helper
 * ============================================================ */

void df_reset(DfStream *s, DfWrap wrap, size_t window_bits) {
    size_t bits = window_bits ? window_bits : 15;
    if (bits > 15) bits = 15;
    s->wrap = (int)wrap;
    s->ended = 0;
    /* s->limit is the caller's bound and survives a reset on purpose */
    s->dist_limit = (uint32_t)1u << bits;
    s->ilen = s->ipos = 0;
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->wpos = s->whave = 0;
    s->state = DF_ST_HEADER;
    s->last_block = 0;
    s->err = DF_OK;
    s->fixed_ready = 0;
    s->stored_left = s->match_left = s->match_dist = 0;
    s->have_pend = 0;
    s->pend = 0;
    s->gz_sub = GZ_FIXED;
    s->gz_flg = s->gz_left = 0;
    s->gz_scan = 0;
    s->hdr_crc = 0xffffffffu;
    s->tr_crc = 0;
    s->tr_have = 0;
    s->crc = 0xffffffffu;
    s->adler_s1 = 1;
    s->adler_s2 = 0;
    s->isize = 0;
    memset(&s->lit, 0, sizeof s->lit);
    memset(&s->dist, 0, sizeof s->dist);
    memset(&s->st, 0, sizeof s->st);
    s->st.wrap = s->wrap;
}

DfStream *df_new(DfWrap wrap, size_t window_bits) {
    DfStream *s = (DfStream *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->limit = 0;
    df_reset(s, wrap, window_bits);
    return s;
}

void df_free(DfStream *s) {
    if (!s) return;
    free(s->ibuf);
    free(s);
}

void df_set_limit(DfStream *s, size_t max_output) {
    if (s) s->limit = max_output;
}

void df_end_input(DfStream *s) {
    if (s) s->ended = 1;
}

const DfStats *df_stats(const DfStream *s) {
    return s ? &s->st : NULL;
}

int df_inflate(DfWrap wrap,
               const uint8_t *in, size_t inlen,
               uint8_t *out, size_t outcap, size_t *out_len) {
    DfStream *s;
    size_t ip = 0, op = 0;
    int rc;

    if (out_len) *out_len = 0;
    if ((inlen && !in) || (outcap && !out)) return DF_ERR_ARG;
    s = df_new(wrap, 15);
    if (!s) return DF_ERR_NOMEM;

    for (;;) {
        size_t used = 0, made = 0;
        const uint8_t *cur_in = in ? in + ip : NULL;
        uint8_t *cur_out = out ? out + op : NULL;
        rc = df_feed(s, cur_in, inlen - ip, &used, cur_out, outcap - op, &made);
        ip += used;
        op += made;
        if (rc == DF_DONE) {
            if (ip == inlen) { rc = DF_OK; break; }
            /* gzip may carry several members back to back */
            if (s->wrap == DF_WRAP_GZIP && inlen - ip >= 2 &&
                in[ip] == 0x1f && in[ip + 1] == 0x8b)
                continue;
            rc = DF_ERR_TRAILING;
            break;
        }
        if (rc == DF_NEED_INPUT) {
            if (s->ended) { rc = DF_ERR_TRUNCATED; break; }
            df_end_input(s);
            continue;
        }
        if (rc == DF_NEED_OUTPUT) { rc = DF_ERR_LIMIT; break; }
        if (rc == DF_OK) continue;    /* df_feed does not return it; be safe */
        break;                        /* an error */
    }
    df_free(s);
    if (out_len) *out_len = op;
    return rc;
}

const char *df_strerror(int code) {
    switch (code) {
    case DF_OK:                 return "ok";
    case DF_NEED_INPUT:         return "need input";
    case DF_NEED_OUTPUT:        return "need output";
    case DF_DONE:               return "end of stream";
    case DF_ERR_ARG:            return "invalid argument";
    case DF_ERR_NOMEM:          return "out of memory";
    case DF_ERR_TRUNCATED:      return "truncated stream";
    case DF_ERR_HUFF_OVER:      return "over-subscribed Huffman tree";
    case DF_ERR_HUFF_INCOMPLETE:return "incomplete Huffman tree";
    case DF_ERR_BAD_CODE:       return "invalid symbol";
    case DF_ERR_BAD_DISTANCE:   return "distance reaches before output start";
    case DF_ERR_BAD_HEADER:     return "bad header";
    case DF_ERR_BAD_STORED:     return "stored block LEN/NLEN mismatch";
    case DF_ERR_BAD_ADLER:      return "adler32 mismatch";
    case DF_ERR_BAD_CRC:        return "crc32 mismatch";
    case DF_ERR_BAD_LENGTH:     return "gzip ISIZE mismatch";
    case DF_ERR_LIMIT:          return "output limit reached";
    case DF_ERR_TRAILING:       return "trailing data after the stream";
    case DF_ERR_BAD_CLEN:       return "bad code-length repeat";
    case DF_ERR_STATE:          return "internal state fault";
    default:                    return "unknown deflate status";
    }
}
