/* ============================================================
 *  Mystic OS - src/net/hpack.h
 *
 *  HPACK header compression for HTTP/2, written from RFC 7541.
 *
 *  No borrowed tables and no third-party code: the 61 static entries of
 *  Appendix A and all 257 codes of Appendix B live in hpack.c, and the
 *  Huffman code table was extracted mechanically from the RFC's own
 *  "code as bits" and "code as hex" columns (see .work/n6/gen.py) rather
 *  than typed from memory.  A 513-node binary trie is built from those
 *  codes and hpack_huffman_verify() walks every code through it, so the
 *  decoder and the table can be shown to agree.
 *
 *  WHAT IS IMPLEMENTED
 *  -------------------
 *    §2.3.1 + App.A  The 61-entry static table, index 1..61.
 *    §2.3.2 + §2.3.3 Dynamic table, newest entry first, addressable from
 *                    index 62 upward; index 0 in an indexed field is a
 *                    decoding error (§6.1).
 *    §4.1            Entry size = name + value + 32 octets.
 *    §4.2 / §4.3     Dynamic table size changes: the decoder validates a
 *                    size update against the SETTINGS_HEADER_TABLE_SIZE
 *                    it advertised and evicts; the encoder records the
 *                    change and emits the size update at the beginning of
 *                    the next header block, smallest value first when the
 *                    size was lowered and then raised.
 *    §4.4            Eviction from the end of the table; an entry larger
 *                    than the maximum empties the table and is not added.
 *    §5.1            Integer representation, any prefix length 1..8 bits,
 *                    both directions.
 *    §5.2            String literal representation (H bit + length +
 *                    octets), both directions, with Huffman coding.
 *    §6.1            Indexed header field.
 *    §6.2.1          Literal with incremental indexing.
 *    §6.2.2          Literal without indexing.
 *    §6.2.3          Literal never indexed; the flag is carried on the
 *                    decoded field so an intermediary can re-encode it
 *                    with the same representation.
 *    §6.3            Dynamic table size update.
 *
 *  WHAT IS NOT HERE
 *  ----------------
 *    - Header block framing (HEADERS/CONTINUATION frames) and the HPACK
 *      connection state.  This file compresses and decompresses header
 *      blocks only; the HTTP/2 layer above owns frames and SETTINGS.
 *    - A compression strategy beyond the policy described on
 *      hpack_set_indexing(): this is not a "best compression" encoder.
 *    - Any use of the thread's globals.  Two contexts never share state,
 *      so one per direction per connection is all a caller needs; a
 *      single context is not safe against concurrent calls.
 *
 *  MEMORY
 *  ------
 *  A context owns its dynamic entries (malloc'd copies), one scratch
 *  buffer used to hold Huffman-decoded strings during a call, and the
 *  entry array grown by doubling.  Fields handed to the emit callback,
 *  and strings returned by hpack_lookup(), point either into the caller's
 *  block, into the scratch buffer, or into the table: they stay valid
 *  until the callback returns (for decoded fields) or until the table
 *  changes (for lookups).  Nothing points into freed memory afterwards.
 * ============================================================ */
#ifndef MYSTIC_NET_HPACK_H
#define MYSTIC_NET_HPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Constants taken from the RFCs.
 * ------------------------------------------------------------ */
#define HPACK_STATIC_ENTRIES      61u   /* RFC 7541, Appendix A, Table 1   */
#define HPACK_DEFAULT_TABLE_SIZE  4096u /* RFC 7540 §6.5.2 default for
                                         * SETTINGS_HEADER_TABLE_SIZE      */
#define HPACK_ENTRY_OVERHEAD      32u   /* RFC 7541 §4.1                   */
#define HPACK_HUFFMAN_SYMBOLS     257u  /* 0..255 plus EOS, Appendix B     */

/* ------------------------------------------------------------
 *  Status codes.  Zero is success; every failure is negative and
 *  comes with a sentence in the caller's err buffer.
 * ------------------------------------------------------------ */
#define HPACK_OK              0
#define HPACK_ERR_ARG        (-1)   /* NULL/insane argument                 */
#define HPACK_ERR_NOMEM      (-2)   /* malloc/realloc failed                */
#define HPACK_ERR_TRUNCATED  (-3)   /* block ends inside an item            */
#define HPACK_ERR_BAD_INDEX  (-4)   /* index 0, or past the table end       */
#define HPACK_ERR_HUFFMAN    (-5)   /* bad code, EOS in the string, bad pad */
#define HPACK_ERR_NOSPACE    (-6)   /* output buffer too small              */
#define HPACK_ERR_SIZE_UPDATE (-7)  /* size update above the advertised max */
#define HPACK_ERR_SIZE_POS   (-8)   /* size update after a header field     */
#define HPACK_ERR_OVERFLOW   (-9)   /* integer larger than 32 bits          */
#define HPACK_ERR_STATE      (-10)  /* context used in the wrong role       */
#define HPACK_ERR_LIMIT      (-11)  /* string/entry beyond a sanity bound   */

/* ------------------------------------------------------------
 *  One header field.
 *
 *  `sensitive` is the §7.1.3 "never indexed" marker.  When decoding it
 *  is set for a §6.2.3 representation; when encoding it forces that
 *  representation.  The encoder does not set it by itself, because RFC
 *  7541's own Appendix C indexes set-cookie; hpack_sensitive_name()
 *  offers the list §7.1.3 names for callers that want to apply it.
 * ------------------------------------------------------------ */
typedef struct {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
    unsigned    sensitive;      /* 0 or 1 */
} HpackField;

/* Appendix A, Table 1.  hpack_static_table[i] is RFC index i + 1, so
 * hpack_static_table[0] is 1: :authority with an empty value. */
extern const HpackField hpack_static_table[HPACK_STATIC_ENTRIES];

/* ------------------------------------------------------------
 *  Counters, all measured by this code.  A caller can put them on a
 *  diagnostics screen without recomputing anything.
 * ------------------------------------------------------------ */
typedef struct {
    uint64_t blocks;            /* header blocks decoded/encoded          */
    uint64_t fields;            /* header fields in those blocks          */
    uint64_t block_bytes;       /* octets of header block in/out          */
    uint64_t list_size;         /* sum of name+value+32 over all fields   */
    uint64_t indexed;           /* §6.1 representations seen/written      */
    uint64_t literal_indexed;   /* §6.2.1                                 */
    uint64_t literal_noindex;   /* §6.2.2                                 */
    uint64_t literal_never;     /* §6.2.3                                 */
    uint64_t size_updates;      /* §6.3                                   */
    uint64_t huffman_strings;   /* string literals with H = 1             */
    uint64_t plain_strings;     /* string literals with H = 0             */
    uint64_t huffman_in;        /* Huffman-coded octets read/written      */
    uint64_t huffman_out;       /* octets those decoded to / came from    */
    uint64_t inserts;           /* entries added to the dynamic table     */
    uint64_t evictions;         /* entries evicted by §4.4                */
    uint64_t table_clears;      /* table emptied by §4.4 (entry too big)  */
    uint32_t max_index;         /* largest index referenced               */
    uint32_t max_table_size;    /* largest table size after an update     */
} HpackStats;

/* ------------------------------------------------------------
 *  A decoded or encoded header block in numbers.  `list_size` is what
 *  RFC 7540 §6.5.2 counts for SETTINGS_MAX_HEADER_LIST_SIZE: the sum of
 *  every field's name and value plus 32 octets per field.
 * ------------------------------------------------------------ */
typedef struct {
    size_t fields;
    size_t block_bytes;
    size_t list_size;
    size_t table_size;          /* dynamic table size after the block, §4.1 */
    size_t table_count;         /* entries in the dynamic table afterwards  */
    int    size_updates;
} HpackBlock;

/* ------------------------------------------------------------
 *  Role and encoder policy.
 * ------------------------------------------------------------ */
typedef enum {
    HPACK_ROLE_DECODER = 0,     /* decompresses blocks from the peer */
    HPACK_ROLE_ENCODER = 1      /* compresses blocks for the peer    */
} HpackRole;

typedef enum {
    /* Exact match in the static or dynamic table -> indexed field.
     * Otherwise a literal with incremental indexing, except that a
     * field whose `sensitive` is set, or one whose entry would not fit
     * the dynamic table, is sent without touching the table. */
    HPACK_INDEX_AUTO = 0,
    /* Never add to the dynamic table: §6.2.2 literals throughout. */
    HPACK_INDEX_NONE = 1,
    /* Never add and set the never-indexed bit: §6.2.3 throughout. */
    HPACK_INDEX_NEVER = 2
} HpackIndexing;

typedef struct HpackCtx HpackCtx;

/* ------------------------------------------------------------
 *  Lifecycle.
 *
 *  max_table_size is the dynamic table size both ends already agree on,
 *  which for a fresh HTTP/2 connection is HPACK_DEFAULT_TABLE_SIZE (the
 *  SETTINGS_HEADER_TABLE_SIZE default).  Pass the peer's announced value
 *  when one arrived before the first header block; the setting itself is
 *  the signal, so no size update is generated for it.
 *
 *  Create one context per direction: HPACK_ENCODER for blocks you send,
 *  HPACK_DECODER for blocks you receive.  They keep separate tables,
 *  which is what the protocol requires.
 *
 *  hpack_new returns NULL on out-of-memory or an impossible size.
 * ------------------------------------------------------------ */
HpackCtx *hpack_new(HpackRole role, size_t max_table_size);
void      hpack_free(HpackCtx *ctx);

/* Empty the dynamic table, drop pending size updates and reset the
 * counters.  Keeps role, limits, Huffman and indexing policy.  This is
 * for a fresh connection or a fresh header block stream, not for use in
 * the middle of one: dropping a pending size update while the peer still
 * knows the old size would desynchronise the two tables. */
int hpack_reset(HpackCtx *ctx);

/* ------------------------------------------------------------
 *  Decoder.
 * ------------------------------------------------------------ */

/* Tell the decoder the largest table size we are willing to advertise,
 * i.e. the SETTINGS_HEADER_TABLE_SIZE we sent.  A §6.3 update above it
 * is HPACK_ERR_SIZE_UPDATE.  Lowering the limit does not evict by itself:
 * the peer may still reference entries that fit the size it is using
 * until it sends the §6.3 update that acknowledges the change, and that
 * update is what shrinks the table (§4.2, §4.3). */
int hpack_decoder_set_limit(HpackCtx *ctx, size_t limit);

/* Decode one complete header block (the reassembled HEADERS +
 * CONTINUATION payload; HPACK itself has no framing).
 *
 * emit() is called once per field, in the order the fields appear in the
 * block.  Its `field` pointer is valid only for the duration of the call.
 * A non-zero return from emit stops decoding and is returned unchanged
 * by hpack_decode(); that is how a caller enforces its own
 * SETTINGS_MAX_HEADER_LIST_SIZE (accumulate field->name_len +
 * field->value_len + 32) without this file inventing a limit.
 *
 * `out` may be NULL.  `err`, when non-NULL and errsz > 0, receives a
 * sentence naming the rule that failed.  On success the table reflects
 * the block exactly as the encoder's table does.
 *
 * A block that fails partway through leaves the context in an undefined
 * table state: HPACK is not restartable inside a block, so a caller must
 * treat any error as fatal for the connection (RFC 7540 §4.3). */
int hpack_decode(HpackCtx *ctx, const uint8_t *block, size_t len,
                 int (*emit)(void *user, const HpackField *field), void *user,
                 HpackBlock *out, char *err, size_t errsz);

/* ------------------------------------------------------------
 *  Encoder.
 * ------------------------------------------------------------ */

/* Choose a larger table size, or a smaller one.  The change is signalled
 * at the beginning of the next header block (§4.2); lowering the size
 * evicts immediately so the index space matches the decoder's once it
 * applies the update.  Two changes before the next block are collapsed
 * to the minimum first, then the final value, as §4.2 requires.  The
 * value must not exceed what the peer's SETTINGS allows; that ceiling is
 * the caller's to track. */
int hpack_encoder_set_max(HpackCtx *ctx, size_t max_table_size);

/* Huffman-code string literals (default on).  §4.2 of RFC 7540 leaves
 * the choice to the encoder; Appendix C is reproduced with both. */
void hpack_set_huffman(HpackCtx *ctx, int on);
int  hpack_huffman_enabled(const HpackCtx *ctx);

/* Representation policy, see HpackIndexing.  Default HPACK_INDEX_AUTO. */
void hpack_set_indexing(HpackCtx *ctx, HpackIndexing mode);
HpackIndexing hpack_indexing(const HpackCtx *ctx);

/* Encode one header block.
 *
 * `outlen` receives the octets written.  If the buffer is too small the
 * call returns HPACK_ERR_NOSPACE and the context must be discarded with
 * the connection: a half-written block cannot be completed.  Size the
 * buffer with hpack_encode_bound() and it cannot happen.
 *
 * `outinfo` may be NULL. */
int hpack_encode(HpackCtx *ctx, const HpackField *fields, size_t count,
                 uint8_t *out, size_t cap, size_t *outlen,
                 HpackBlock *outinfo, char *err, size_t errsz);

/* A buffer size that always holds the block hpack_encode() would produce
 * for these fields with the same Huffman setting: an upper bound, not an
 * estimate.  Index decisions do not matter because every representation
 * is covered.  SIZE_MAX if the input is too large to describe. */
size_t hpack_encode_bound(const HpackField *fields, size_t count, int huffman);

/* ------------------------------------------------------------
 *  The tables.
 * ------------------------------------------------------------ */

/* RFC index -> field, static 1..61 and dynamic 62..61+count (§2.3.3).
 * Returns HPACK_OK, HPACK_ERR_BAD_INDEX, or HPACK_ERR_ARG.  The returned
 * name/value pointers stay valid until the dynamic table changes; for a
 * static entry they are permanent. */
int hpack_lookup(const HpackCtx *ctx, uint32_t index, HpackField *out);

/* Static entry for RFC index 1..61, or NULL. */
const HpackField *hpack_static_entry(uint32_t index);

/* Dynamic entry, 0 = newest, 1 = next, ...  Returns HPACK_OK or
 * HPACK_ERR_BAD_INDEX. */
int hpack_dynamic_entry(const HpackCtx *ctx, size_t i, HpackField *out);

size_t hpack_table_size(const HpackCtx *ctx);   /* §4.1 octets, measured */
size_t hpack_table_count(const HpackCtx *ctx);
size_t hpack_table_max(const HpackCtx *ctx);

const HpackStats *hpack_stats(const HpackCtx *ctx);
void hpack_stats_reset(HpackCtx *ctx);

/* ------------------------------------------------------------
 *  §5.1 integer representation.
 *
 *  hpack_int_encode writes `value` in `prefix_bits` bits: the low bits of
 *  the first octet, whose high bits are written as zero so the caller can
 *  OR in the representation's pattern afterwards (that is exactly how §6
 *  builds its first octet).  Returns HPACK_OK, HPACK_ERR_ARG for
 *  prefix_bits outside 1..8, or HPACK_ERR_NOSPACE.
 *
 *  hpack_int_decode reads at *pos and advances it.  A value that does not
 *  fit 32 bits is HPACK_ERR_OVERFLOW, a missing continuation octet is
 *  HPACK_ERR_TRUNCATED. */
int hpack_int_encode(uint32_t value, unsigned prefix_bits,
                     uint8_t *out, size_t cap, size_t *outlen);
int hpack_int_decode(const uint8_t *in, size_t len, size_t *pos,
                     unsigned prefix_bits, uint32_t *value);

/* ------------------------------------------------------------
 *  §5.2 string literal representation.
 *
 *  Decoded octets are not NUL-terminated (a header value may contain any
 *  octet).  HPACK_ERR_NOSPACE means the caller's buffer is too small.  On
 *  any error *pos is left where it was, so a caller can report the offset
 *  of the first byte it could not decode.
 * ------------------------------------------------------------ */
int hpack_string_encode(const void *data, size_t len, int huffman,
                        uint8_t *out, size_t cap, size_t *outlen);
int hpack_string_decode(const uint8_t *in, size_t len, size_t *pos,
                        char *out, size_t cap, size_t *outlen);

/* ------------------------------------------------------------
 *  Appendix B Huffman code.
 *
 *  The code is complete and prefix-free; the trie in hpack.c has 513
 *  nodes and 257 leaves.  hpack_huffman_verify() proves that at runtime:
 *  it walks all 257 codes from the root and reports the first symbol
 *  whose path does not end at its own leaf.  Returns HPACK_OK or
 *  HPACK_ERR_STATE with *bad_symbol set (may be NULL).
 * ------------------------------------------------------------ */

/* Exact number of octets the encoded string needs, or SIZE_MAX if the
 * bit count does not fit size_t. */
size_t hpack_huffman_encoded_len(const uint8_t *in, size_t len);

/* Upper bound on the decoded length of `encoded_len` octets.  The
 * shortest Appendix B code is 5 bits, so decoded <= ceil(encoded * 8 / 5);
 * this is the buffer a caller must provide to be sure. */
size_t hpack_huffman_max_decoded(size_t encoded_len);

int hpack_huffman_encode(const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, size_t *outlen);
int hpack_huffman_decode(const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, size_t *outlen);

int hpack_huffman_verify(uint32_t *bad_symbol);

/* Appendix B code for one symbol (0..255, 256 = EOS): the code aligned to
 * the least significant bit and its length in bits, as the RFC's "code as
 * hex" and "len" columns give them.  Returns HPACK_OK or HPACK_ERR_ARG. */
int hpack_huffman_code(uint32_t symbol, uint32_t *code, unsigned *bits);

/* ------------------------------------------------------------
 *  Small helpers.
 * ------------------------------------------------------------ */

/* The names RFC 7541 §7.1.3 names as values worth protecting from
 * compression: Cookie and Authorization.  set-cookie carries the same
 * secret as Cookie and proxy-authorization the same as Authorization, so
 * they are included too.  Case-insensitive; 1 if `name` is one of them. */
int hpack_sensitive_name(const char *name, size_t len);

/* A short, stable description of an HPACK_* status code. */
const char *hpack_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* MYSTIC_NET_HPACK_H */
