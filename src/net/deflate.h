/* ============================================================
 *  Mystic OS - src/net/deflate.h
 *  DEFLATE (RFC 1951) with the zlib (RFC 1950) and gzip (RFC 1952)
 *  wrappers: a streaming decompressor, written here from the RFCs.
 *
 *  No zlib, no miniz, no borrowed tables.  Every constant in
 *  deflate.c is annotated with the section of the RFC it comes
 *  from, and the checksums (adler32, crc32) are computed by this
 *  file too, so a caller can verify a wrapper's trailer without
 *  linking anything.
 *
 *  What is implemented (all of it exercised by .work/n4/)
 *  ------------------------------------------------------
 *    - stored, fixed-Huffman and dynamic-Huffman blocks
 *      (RFC 1951 sections 3.2.3 .. 3.2.7)
 *    - the length/distance tables of RFC 1951 section 3.2.5,
 *      including the 258-byte code 285 and the 32768 distance
 *    - a 32 KiB history window, so distances reaching back into
 *      earlier output are reproduced byte for byte
 *    - streaming input: df_feed() may be handed the stream one
 *      byte at a time, in any fragmentation
 *    - streaming output: it writes at most outcap bytes per call
 *      and resumes a half-copied match on the next call, so a
 *      one-byte output buffer works as well
 *    - zlib header checking: CM, CINFO, FCHECK, FDICT
 *    - gzip header: ID1/ID2, CM, the reserved FLG bits, FEXTRA,
 *      FNAME, FCOMMENT, FHCRC, and the CRC32/ISIZE trailer;
 *      multi-member files are decoded member by member
 *    - adler32 (RFC 1950 section 9) and crc32 (RFC 1952 section 8)
 *      verified against the trailer, never assumed
 *    - honest errors: over-subscribed Huffman tree, incomplete
 *      tree, missing end-of-block code, reserved block type,
 *      reserved distance code, a distance that reaches before the
 *      start of the output, LEN/NLEN mismatch, bad header bytes,
 *      checksum mismatch, truncation, and an optional output limit
 *
 *  What is deliberately not implemented
 *  -----------------------------------
 *    - compression.  This is a decompressor; there is no deflate
 *      encoder here and the header does not pretend to offer one.
 *    - preset dictionaries (RFC 1950 FDICT).  A zlib stream that
 *      asks for one is refused with DF_ERR_BAD_HEADER rather than
 *      silently decoded against the wrong history.
 *    - gzip's optional "extra field" contents are skipped, not
 *      interpreted (the RFC gives them no meaning of their own).
 *
 *  Threading: a DfStream is not thread-safe, but there is no
 *  mutable global state other than the lazily built CRC-32 table,
 *  which is idempotent (every thread that races to build it writes
 *  the same 256 values, then reads them).
 *
 *  Memory: one DfStream holds a 32 KiB window plus whatever
 *  unread input the caller pushed in one call.  Unread input is
 *  compacted after every df_feed(), so a caller that feeds 1 MiB
 *  at a time costs 1 MiB, not the whole stream.
 * ============================================================ */
#ifndef MYSTIC_NET_DEFLATE_H
#define MYSTIC_NET_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------
 *  Status codes.
 *
 *  Positive values are "not finished, call me again"; zero is
 *  success (only df_inflate returns it); negative values are hard
 *  errors and are sticky on the stream.
 * ------------------------------------------------------------ */
#define DF_OK               0   /* df_inflate: the whole stream decoded */
#define DF_NEED_INPUT       1   /* every byte given so far was consumed */
#define DF_NEED_OUTPUT      2   /* the output buffer is full; call again */
#define DF_DONE             3   /* end of stream (gzip: end of a member) */

#define DF_ERR_ARG          (-1)   /* NULL pointer / impossible argument */
#define DF_ERR_NOMEM        (-2)   /* out of memory growing the input buffer */
#define DF_ERR_TRUNCATED    (-3)   /* input ended in the middle of the stream */
#define DF_ERR_HUFF_OVER    (-4)   /* Huffman tree is over-subscribed */
#define DF_ERR_HUFF_INCOMPLETE (-5) /* Huffman tree is incomplete */
#define DF_ERR_BAD_CODE     (-6)   /* reserved/invalid symbol in the stream */
#define DF_ERR_BAD_DISTANCE (-7)   /* distance reaches before output start */
#define DF_ERR_BAD_HEADER   (-8)   /* zlib/gzip header invalid, or BTYPE=3 */
#define DF_ERR_BAD_STORED   (-9)   /* stored block LEN/NLEN mismatch */
#define DF_ERR_BAD_ADLER    (-10)  /* zlib adler32 trailer mismatch */
#define DF_ERR_BAD_CRC      (-11)  /* gzip crc32 trailer mismatch */
#define DF_ERR_BAD_LENGTH   (-12)  /* gzip ISIZE trailer mismatch */
#define DF_ERR_LIMIT        (-13)  /* output bound (buffer or df_set_limit) */
#define DF_ERR_TRAILING     (-14)  /* bytes after a complete stream */
#define DF_ERR_BAD_CLEN     (-15)  /* bad code-length repeat (16/17/18) */
#define DF_ERR_STATE        (-16)  /* internal state machine fault */

/* ------------------------------------------------------------
 *  The wrapper around the deflate data.
 * ------------------------------------------------------------ */
typedef enum {
    DF_WRAP_RAW  = 0,   /* RFC 1951 only: no header, no trailer */
    DF_WRAP_ZLIB = 1,   /* RFC 1950: 2-byte header, adler32 trailer */
    DF_WRAP_GZIP = 2,   /* RFC 1952: header + crc32/ISIZE trailer */
    DF_WRAP_AUTO = 3    /* sniff: gzip magic, else a valid zlib header,
                         * else raw.  Sniffing is a convenience and it is
                         * ambiguous by nature - a raw stream whose first
                         * two bytes happen to form a valid zlib header
                         * (0x78 0x01/0x9c/0xda ...) will be read as zlib.
                         * Pass DF_WRAP_RAW when you know it is raw. */
} DfWrap;

/* ------------------------------------------------------------
 *  Counters, measured by this code, not guessed.  A caller can
 *  print them straight into a diagnostics screen.
 * ------------------------------------------------------------ */
typedef struct {
    int      wrap;          /* wrapper actually used (DF_WRAP_*), after sniffing */
    uint64_t total_in;      /* compressed bytes consumed, headers+trailers included */
    uint64_t total_out;     /* uncompressed bytes produced */
    uint64_t blocks;        /* deflate blocks decoded */
    uint64_t stored_blocks; /* of which BTYPE=00 */
    uint64_t fixed_blocks;  /* of which BTYPE=01 */
    uint64_t dynamic_blocks;/* of which BTYPE=10 */
    uint64_t literals;      /* literal bytes decoded */
    uint64_t matches;       /* LZ77 matches decoded */
    uint64_t match_bytes;   /* bytes produced by those matches */
    uint32_t max_match;     /* longest match seen (bytes) */
    uint32_t max_distance;  /* largest distance used (bytes) */
    uint32_t members;       /* deflate streams (gzip members) started */
    uint32_t adler;         /* adler32 computed over the last zlib stream */
    uint32_t crc;           /* crc32 computed over the last gzip member */
    uint32_t isize;         /* ISIZE computed over the last gzip member */
} DfStats;

/* ------------------------------------------------------------
 *  The stream.  Opaque; deflate.c owns everything inside.
 * ------------------------------------------------------------ */
typedef struct DfStream DfStream;

/* Create a stream.  window_bits is the history window as a power of
 * two: 0 means the protocol maximum, 15 (32768 bytes, RFC 1951
 * section 3.2.5).  Smaller values are honoured as a *limit*: a
 * distance beyond 1<<window_bits is then rejected
 * (DF_ERR_BAD_DISTANCE), which is what a receiver that promised a
 * smaller window must do.  Returns NULL on out-of-memory. */
DfStream *df_new(DfWrap wrap, size_t window_bits);

/* Free the stream and its input buffer. */
void df_free(DfStream *s);

/* Return the stream to its just-created state with (possibly) a new
 * wrapper and window.  Keeps the allocated input buffer. */
void df_reset(DfStream *s, DfWrap wrap, size_t window_bits);

/* Bound the total output.  0 (the default) means "no bound beyond the
 * caller's buffer".  Exceeding it is DF_ERR_LIMIT, a hard error, so a
 * zip bomb costs bounded memory and bounded time. */
void df_set_limit(DfStream *s, size_t max_output);

/* Tell the stream that no more input will ever arrive.  From then on
 * a stream that stops in the middle of a symbol or block reports
 * DF_ERR_TRUNCATED instead of DF_NEED_INPUT. */
void df_end_input(DfStream *s);

/* ------------------------------------------------------------
 *  Streaming decode.
 *
 *  in/inlen      the next bytes of the compressed stream (may be NULL
 *                when inlen is 0).
 *  in_used       out: how many of the caller's bytes the stream took.
 *                Every byte given is copied into the stream (or is part
 *                of the bytes already copied), so this is inlen once the
 *                call returns DF_OK, DF_NEED_INPUT, DF_NEED_OUTPUT or
 *                DF_DONE.  The caller may drop the whole buffer and must
 *                not hand the same bytes again.  On DF_ERR_NOMEM nothing
 *                was taken and it is 0.
 *  out/outcap    where to put uncompressed bytes; at most outcap are
 *                written per call.  NULL/0 is legal and is how a
 *                caller asks "how much output do you have for me?".
 *  out_used      out: how many were written.
 *
 *  Returns DF_DONE when the stream ended (gzip: when a member ended),
 *  DF_NEED_INPUT when everything given was consumed, DF_NEED_OUTPUT
 *  when the output buffer filled up, or a negative error.  Errors are
 *  sticky: once df_feed has returned a negative value every later
 *  call returns the same one.
 *
 *  Calling df_feed after DF_DONE with more input continues a
 *  multi-member gzip file if the next bytes are the gzip magic, and
 *  fails with DF_ERR_TRAILING otherwise.
 * ------------------------------------------------------------ */
int df_feed(DfStream *s,
            const uint8_t *in, size_t inlen, size_t *in_used,
            uint8_t *out, size_t outcap, size_t *out_used);

/* ------------------------------------------------------------
 *  One-shot decode of a whole buffer (the harness and small callers
 *  use this).  Returns DF_OK when the stream decoded completely and
 *  every input byte was used, DF_ERR_LIMIT when out is too small, or
 *  a negative error.  out_len may be NULL.
 * ------------------------------------------------------------ */
int df_inflate(DfWrap wrap,
               const uint8_t *in, size_t inlen,
               uint8_t *out, size_t outcap, size_t *out_len);

/* The counters so far.  Valid until the stream is freed. */
const DfStats *df_stats(const DfStream *s);

/* ------------------------------------------------------------
 *  Checksums, computed in deflate.c (RFC 1950 section 9 and
 *  RFC 1952 section 8).  Both take a running value, and the running
 *  value starts at 0 for both, so
 *      df_crc32(0, buf, len)
 *  is the CRC-32 of buf and
 *      df_adler32(0, buf, len)
 *  is its adler32.
 * ------------------------------------------------------------ */
uint32_t df_crc32(uint32_t crc, const void *data, size_t len);
uint32_t df_adler32(uint32_t adler, const void *data, size_t len);

/* A short, stable description of a DF_* status code. */
const char *df_strerror(int code);

#endif /* MYSTIC_NET_DEFLATE_H */
