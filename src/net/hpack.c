/* ============================================================
 *  Mystic OS - src/net/hpack.c
 *
 *  HPACK header compression for HTTP/2 (RFC 7541), from the RFC.
 *
 *  Read hpack.h first: it states what is implemented, what the two roles
 *  mean, and who owns every pointer.  This file is the machinery.
 *
 *  The two tables of the RFC are data, not code.  Both were extracted
 *  from the RFC text by .work/n6/gen.py: Appendix A's 61 rows and
 *  Appendix B's 257 codes, the latter cross-checked between the "code as
 *  bits" and "code as hex" columns and turned into a 513-node trie.  The
 *  trie is checked at runtime by hpack_huffman_verify(), which walks
 *  every one of the 257 codes and demands that it end at its own leaf.
 *
 *  VERIFIED, on the host, against the RFC text itself (not against another
 *  implementation), by .work/n6/harness.c:
 *
 *    - the 61 entries of Appendix A and all 257 codes of Appendix B are
 *      dumped and compared with the RFC's own tables: 61/61 and 257/257;
 *    - all 16 header-block examples of Appendix C.2 .. C.6 - 491 octets,
 *      60 header fields - decode to the RFC's header lists and leave the
 *      RFC's dynamic tables entry for entry, including the five evictions
 *      that C.5 and C.6 produce at SETTINGS_HEADER_TABLE_SIZE = 256;
 *    - the same 16 blocks are re-encoded to the RFC's own octets, byte for
 *      byte, with and without Huffman coding;
 *    - C.1's three integer examples round trip, and the rules of §5.1,
 *      §5.2, §4.2, §4.4, §6.1 and §6.3 are each exercised through their
 *      own error codes;
 *    - 120 randomised blocks (104324 octets, 1297 fields) on one context
 *      pair keep the encoder's and decoder's tables identical;
 *    - a two-way differential test against the independent Python hpack
 *      4.2.0 package (a test oracle in .work/n6/oracle, not part of the
 *      OS) accepts 32 of our blocks and we accept its 32.
 * ============================================================ */
#include "hpack.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A full binary tree with 257 leaves has 2*257 - 1 nodes; the generator
 * builds exactly that many and hpack_huffman_verify() re-counts them. */
#define HUFF_TREE_NODES 513

/* One node of the Appendix B trie.  sym < 0 is an internal node. */
typedef struct {
    int16_t child[2];
    int16_t sym;
} HpackHuffNode;

/* Appendix B: code as hex aligned to the LSB, and length in bits.
 * Index is the symbol; 256 is EOS.  Generated from the RFC text by
 * .work/n6/gen.py and pasted here; the values were re-derived from the
 * 'code as bits' column and checked against 'code as hex'. */
static const uint32_t huff_code[HPACK_HUFFMAN_SYMBOLS] = {
    0x00001ff8, 0x007fffd8, 0x0fffffe2, 0x0fffffe3, 0x0fffffe4, 0x0fffffe5,
    0x0fffffe6, 0x0fffffe7, 0x0fffffe8, 0x00ffffea, 0x3ffffffc, 0x0fffffe9,
    0x0fffffea, 0x3ffffffd, 0x0fffffeb, 0x0fffffec, 0x0fffffed, 0x0fffffee,
    0x0fffffef, 0x0ffffff0, 0x0ffffff1, 0x0ffffff2, 0x3ffffffe, 0x0ffffff3,
    0x0ffffff4, 0x0ffffff5, 0x0ffffff6, 0x0ffffff7, 0x0ffffff8, 0x0ffffff9,
    0x0ffffffa, 0x0ffffffb, 0x00000014, 0x000003f8, 0x000003f9, 0x00000ffa,
    0x00001ff9, 0x00000015, 0x000000f8, 0x000007fa, 0x000003fa, 0x000003fb,
    0x000000f9, 0x000007fb, 0x000000fa, 0x00000016, 0x00000017, 0x00000018,
    0x00000000, 0x00000001, 0x00000002, 0x00000019, 0x0000001a, 0x0000001b,
    0x0000001c, 0x0000001d, 0x0000001e, 0x0000001f, 0x0000005c, 0x000000fb,
    0x00007ffc, 0x00000020, 0x00000ffb, 0x000003fc, 0x00001ffa, 0x00000021,
    0x0000005d, 0x0000005e, 0x0000005f, 0x00000060, 0x00000061, 0x00000062,
    0x00000063, 0x00000064, 0x00000065, 0x00000066, 0x00000067, 0x00000068,
    0x00000069, 0x0000006a, 0x0000006b, 0x0000006c, 0x0000006d, 0x0000006e,
    0x0000006f, 0x00000070, 0x00000071, 0x00000072, 0x000000fc, 0x00000073,
    0x000000fd, 0x00001ffb, 0x0007fff0, 0x00001ffc, 0x00003ffc, 0x00000022,
    0x00007ffd, 0x00000003, 0x00000023, 0x00000004, 0x00000024, 0x00000005,
    0x00000025, 0x00000026, 0x00000027, 0x00000006, 0x00000074, 0x00000075,
    0x00000028, 0x00000029, 0x0000002a, 0x00000007, 0x0000002b, 0x00000076,
    0x0000002c, 0x00000008, 0x00000009, 0x0000002d, 0x00000077, 0x00000078,
    0x00000079, 0x0000007a, 0x0000007b, 0x00007ffe, 0x000007fc, 0x00003ffd,
    0x00001ffd, 0x0ffffffc, 0x000fffe6, 0x003fffd2, 0x000fffe7, 0x000fffe8,
    0x003fffd3, 0x003fffd4, 0x003fffd5, 0x007fffd9, 0x003fffd6, 0x007fffda,
    0x007fffdb, 0x007fffdc, 0x007fffdd, 0x007fffde, 0x00ffffeb, 0x007fffdf,
    0x00ffffec, 0x00ffffed, 0x003fffd7, 0x007fffe0, 0x00ffffee, 0x007fffe1,
    0x007fffe2, 0x007fffe3, 0x007fffe4, 0x001fffdc, 0x003fffd8, 0x007fffe5,
    0x003fffd9, 0x007fffe6, 0x007fffe7, 0x00ffffef, 0x003fffda, 0x001fffdd,
    0x000fffe9, 0x003fffdb, 0x003fffdc, 0x007fffe8, 0x007fffe9, 0x001fffde,
    0x007fffea, 0x003fffdd, 0x003fffde, 0x00fffff0, 0x001fffdf, 0x003fffdf,
    0x007fffeb, 0x007fffec, 0x001fffe0, 0x001fffe1, 0x003fffe0, 0x001fffe2,
    0x007fffed, 0x003fffe1, 0x007fffee, 0x007fffef, 0x000fffea, 0x003fffe2,
    0x003fffe3, 0x003fffe4, 0x007ffff0, 0x003fffe5, 0x003fffe6, 0x007ffff1,
    0x03ffffe0, 0x03ffffe1, 0x000fffeb, 0x0007fff1, 0x003fffe7, 0x007ffff2,
    0x003fffe8, 0x01ffffec, 0x03ffffe2, 0x03ffffe3, 0x03ffffe4, 0x07ffffde,
    0x07ffffdf, 0x03ffffe5, 0x00fffff1, 0x01ffffed, 0x0007fff2, 0x001fffe3,
    0x03ffffe6, 0x07ffffe0, 0x07ffffe1, 0x03ffffe7, 0x07ffffe2, 0x00fffff2,
    0x001fffe4, 0x001fffe5, 0x03ffffe8, 0x03ffffe9, 0x0ffffffd, 0x07ffffe3,
    0x07ffffe4, 0x07ffffe5, 0x000fffec, 0x00fffff3, 0x000fffed, 0x001fffe6,
    0x003fffe9, 0x001fffe7, 0x001fffe8, 0x007ffff3, 0x003fffea, 0x003fffeb,
    0x01ffffee, 0x01ffffef, 0x00fffff4, 0x00fffff5, 0x03ffffea, 0x007ffff4,
    0x03ffffeb, 0x07ffffe6, 0x03ffffec, 0x03ffffed, 0x07ffffe7, 0x07ffffe8,
    0x07ffffe9, 0x07ffffea, 0x07ffffeb, 0x0ffffffe, 0x07ffffec, 0x07ffffed,
    0x07ffffee, 0x07ffffef, 0x07fffff0, 0x03ffffee, 0x3fffffff,
};

static const uint8_t huff_len[HPACK_HUFFMAN_SYMBOLS] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
     6, 10, 10, 12, 13,  6,  8, 11, 10, 10,  8, 11,  8,  6,  6,  6,
     5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  7,  8, 15,  6, 12, 10,
    13,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
     7,  7,  7,  7,  7,  7,  7,  7,  8,  7,  8, 13, 19, 13, 14,  6,
    15,  5,  6,  5,  6,  5,  6,  6,  6,  5,  7,  7,  6,  6,  6,  5,
     6,  7,  6,  5,  5,  6,  7,  7,  7,  7,  7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
    30,
};

/* The same code as a binary trie, built from the table above at
 * generation time (hpack_huffman_verify() re-walks every one of the 257
 * codes through it at runtime).  child[0] is bit 0, child[1] bit 1;
 * sym is the symbol at a leaf, -1 at an internal node.  513 nodes. */
static const HpackHuffNode huff_tree[HUFF_TREE_NODES] = {
    { {   98,    1 },   -1 },
    { {  151,    2 },   -1 },
    { {  173,    3 },   -1 },
    { {  204,    4 },   -1 },
    { {  263,    5 },   -1 },
    { {  113,    6 },   -1 },
    { {  211,    7 },   -1 },
    { {  104,    8 },   -1 },
    { {  116,    9 },   -1 },
    { {  108,   10 },   -1 },
    { {   11,   14 },   -1 },
    { {   12,  166 },   -1 },
    { {   13,  111 },   -1 },
    { {   -1,   -1 },    0 },
    { {  220,   15 },   -1 },
    { {  222,   16 },   -1 },
    { {  158,   17 },   -1 },
    { {  270,   18 },   -1 },
    { {  216,   19 },   -1 },
    { {  279,   20 },   -1 },
    { {   21,   27 },   -1 },
    { {  377,   22 },   -1 },
    { {  414,   23 },   -1 },
    { {   24,  301 },   -1 },
    { {   25,  298 },   -1 },
    { {   26,  295 },   -1 },
    { {   -1,   -1 },    1 },
    { {  314,   28 },   -1 },
    { {   50,   29 },   -1 },
    { {  362,   30 },   -1 },
    { {  403,   31 },   -1 },
    { {  440,   32 },   -1 },
    { {   33,   55 },   -1 },
    { {   34,   46 },   -1 },
    { {   35,   39 },   -1 },
    { {  510,   36 },   -1 },
    { {   37,   38 },   -1 },
    { {   -1,   -1 },    2 },
    { {   -1,   -1 },    3 },
    { {   40,   43 },   -1 },
    { {   41,   42 },   -1 },
    { {   -1,   -1 },    4 },
    { {   -1,   -1 },    5 },
    { {   44,   45 },   -1 },
    { {   -1,   -1 },    6 },
    { {   -1,   -1 },    7 },
    { {   47,   67 },   -1 },
    { {   48,   63 },   -1 },
    { {   49,   62 },   -1 },
    { {   -1,   -1 },    8 },
    { {  396,   51 },   -1 },
    { {   52,  309 },   -1 },
    { {  486,   53 },   -1 },
    { {   54,  307 },   -1 },
    { {   -1,   -1 },    9 },
    { {   74,   56 },   -1 },
    { {   91,   57 },   -1 },
    { {  274,   58 },   -1 },
    { {  502,   59 },   -1 },
    { {   60,   81 },   -1 },
    { {   61,   65 },   -1 },
    { {   -1,   -1 },   10 },
    { {   -1,   -1 },   11 },
    { {   64,   66 },   -1 },
    { {   -1,   -1 },   12 },
    { {   -1,   -1 },   13 },
    { {   -1,   -1 },   14 },
    { {   68,   71 },   -1 },
    { {   69,   70 },   -1 },
    { {   -1,   -1 },   15 },
    { {   -1,   -1 },   16 },
    { {   72,   73 },   -1 },
    { {   -1,   -1 },   17 },
    { {   -1,   -1 },   18 },
    { {   75,   84 },   -1 },
    { {   76,   79 },   -1 },
    { {   77,   78 },   -1 },
    { {   -1,   -1 },   19 },
    { {   -1,   -1 },   20 },
    { {   80,   83 },   -1 },
    { {   -1,   -1 },   21 },
    { {   82,  512 },   -1 },
    { {   -1,   -1 },   22 },
    { {   -1,   -1 },   23 },
    { {   85,   88 },   -1 },
    { {   86,   87 },   -1 },
    { {   -1,   -1 },   24 },
    { {   -1,   -1 },   25 },
    { {   89,   90 },   -1 },
    { {   -1,   -1 },   26 },
    { {   -1,   -1 },   27 },
    { {   92,   95 },   -1 },
    { {   93,   94 },   -1 },
    { {   -1,   -1 },   28 },
    { {   -1,   -1 },   29 },
    { {   96,   97 },   -1 },
    { {   -1,   -1 },   30 },
    { {   -1,   -1 },   31 },
    { {  133,   99 },   -1 },
    { {  100,  129 },   -1 },
    { {  258,  101 },   -1 },
    { {  102,  126 },   -1 },
    { {  103,  112 },   -1 },
    { {   -1,   -1 },   32 },
    { {  105,  119 },   -1 },
    { {  106,  107 },   -1 },
    { {   -1,   -1 },   33 },
    { {   -1,   -1 },   34 },
    { {  271,  109 },   -1 },
    { {  110,  164 },   -1 },
    { {   -1,   -1 },   35 },
    { {   -1,   -1 },   36 },
    { {   -1,   -1 },   37 },
    { {  114,  124 },   -1 },
    { {  115,  122 },   -1 },
    { {   -1,   -1 },   38 },
    { {  165,  117 },   -1 },
    { {  118,  123 },   -1 },
    { {   -1,   -1 },   39 },
    { {  120,  121 },   -1 },
    { {   -1,   -1 },   40 },
    { {   -1,   -1 },   41 },
    { {   -1,   -1 },   42 },
    { {   -1,   -1 },   43 },
    { {  125,  157 },   -1 },
    { {   -1,   -1 },   44 },
    { {  127,  128 },   -1 },
    { {   -1,   -1 },   45 },
    { {   -1,   -1 },   46 },
    { {  130,  144 },   -1 },
    { {  131,  141 },   -1 },
    { {  132,  140 },   -1 },
    { {   -1,   -1 },   47 },
    { {  134,  229 },   -1 },
    { {  135,  138 },   -1 },
    { {  136,  137 },   -1 },
    { {   -1,   -1 },   48 },
    { {   -1,   -1 },   49 },
    { {  139,  227 },   -1 },
    { {   -1,   -1 },   50 },
    { {   -1,   -1 },   51 },
    { {  142,  143 },   -1 },
    { {   -1,   -1 },   52 },
    { {   -1,   -1 },   53 },
    { {  145,  148 },   -1 },
    { {  146,  147 },   -1 },
    { {   -1,   -1 },   54 },
    { {   -1,   -1 },   55 },
    { {  149,  150 },   -1 },
    { {   -1,   -1 },   56 },
    { {   -1,   -1 },   57 },
    { {  160,  152 },   -1 },
    { {  246,  153 },   -1 },
    { {  256,  154 },   -1 },
    { {  155,  170 },   -1 },
    { {  156,  169 },   -1 },
    { {   -1,   -1 },   58 },
    { {   -1,   -1 },   59 },
    { {  159,  226 },   -1 },
    { {   -1,   -1 },   60 },
    { {  161,  232 },   -1 },
    { {  162,  224 },   -1 },
    { {  163,  168 },   -1 },
    { {   -1,   -1 },   61 },
    { {   -1,   -1 },   62 },
    { {   -1,   -1 },   63 },
    { {  167,  215 },   -1 },
    { {   -1,   -1 },   64 },
    { {   -1,   -1 },   65 },
    { {   -1,   -1 },   66 },
    { {  171,  172 },   -1 },
    { {   -1,   -1 },   67 },
    { {   -1,   -1 },   68 },
    { {  174,  189 },   -1 },
    { {  175,  182 },   -1 },
    { {  176,  179 },   -1 },
    { {  177,  178 },   -1 },
    { {   -1,   -1 },   69 },
    { {   -1,   -1 },   70 },
    { {  180,  181 },   -1 },
    { {   -1,   -1 },   71 },
    { {   -1,   -1 },   72 },
    { {  183,  186 },   -1 },
    { {  184,  185 },   -1 },
    { {   -1,   -1 },   73 },
    { {   -1,   -1 },   74 },
    { {  187,  188 },   -1 },
    { {   -1,   -1 },   75 },
    { {   -1,   -1 },   76 },
    { {  190,  197 },   -1 },
    { {  191,  194 },   -1 },
    { {  192,  193 },   -1 },
    { {   -1,   -1 },   77 },
    { {   -1,   -1 },   78 },
    { {  195,  196 },   -1 },
    { {   -1,   -1 },   79 },
    { {   -1,   -1 },   80 },
    { {  198,  201 },   -1 },
    { {  199,  200 },   -1 },
    { {   -1,   -1 },   81 },
    { {   -1,   -1 },   82 },
    { {  202,  203 },   -1 },
    { {   -1,   -1 },   83 },
    { {   -1,   -1 },   84 },
    { {  205,  242 },   -1 },
    { {  206,  209 },   -1 },
    { {  207,  208 },   -1 },
    { {   -1,   -1 },   85 },
    { {   -1,   -1 },   86 },
    { {  210,  213 },   -1 },
    { {   -1,   -1 },   87 },
    { {  212,  214 },   -1 },
    { {   -1,   -1 },   88 },
    { {   -1,   -1 },   89 },
    { {   -1,   -1 },   90 },
    { {   -1,   -1 },   91 },
    { {  217,  286 },   -1 },
    { {  218,  276 },   -1 },
    { {  219,  410 },   -1 },
    { {   -1,   -1 },   92 },
    { {  221,  273 },   -1 },
    { {   -1,   -1 },   93 },
    { {  223,  272 },   -1 },
    { {   -1,   -1 },   94 },
    { {  225,  228 },   -1 },
    { {   -1,   -1 },   95 },
    { {   -1,   -1 },   96 },
    { {   -1,   -1 },   97 },
    { {   -1,   -1 },   98 },
    { {  230,  240 },   -1 },
    { {  231,  235 },   -1 },
    { {   -1,   -1 },   99 },
    { {  233,  237 },   -1 },
    { {  234,  236 },   -1 },
    { {   -1,   -1 },  100 },
    { {   -1,   -1 },  101 },
    { {   -1,   -1 },  102 },
    { {  238,  239 },   -1 },
    { {   -1,   -1 },  103 },
    { {   -1,   -1 },  104 },
    { {  241,  252 },   -1 },
    { {   -1,   -1 },  105 },
    { {  243,  254 },   -1 },
    { {  244,  245 },   -1 },
    { {   -1,   -1 },  106 },
    { {   -1,   -1 },  107 },
    { {  247,  250 },   -1 },
    { {  248,  249 },   -1 },
    { {   -1,   -1 },  108 },
    { {   -1,   -1 },  109 },
    { {  251,  253 },   -1 },
    { {   -1,   -1 },  110 },
    { {   -1,   -1 },  111 },
    { {   -1,   -1 },  112 },
    { {  255,  262 },   -1 },
    { {   -1,   -1 },  113 },
    { {  257,  261 },   -1 },
    { {   -1,   -1 },  114 },
    { {  259,  260 },   -1 },
    { {   -1,   -1 },  115 },
    { {   -1,   -1 },  116 },
    { {   -1,   -1 },  117 },
    { {   -1,   -1 },  118 },
    { {  264,  267 },   -1 },
    { {  265,  266 },   -1 },
    { {   -1,   -1 },  119 },
    { {   -1,   -1 },  120 },
    { {  268,  269 },   -1 },
    { {   -1,   -1 },  121 },
    { {   -1,   -1 },  122 },
    { {   -1,   -1 },  123 },
    { {   -1,   -1 },  124 },
    { {   -1,   -1 },  125 },
    { {   -1,   -1 },  126 },
    { {  275,  459 },   -1 },
    { {   -1,   -1 },  127 },
    { {  436,  277 },   -1 },
    { {  278,  285 },   -1 },
    { {   -1,   -1 },  128 },
    { {  372,  280 },   -1 },
    { {  281,  332 },   -1 },
    { {  282,  291 },   -1 },
    { {  473,  283 },   -1 },
    { {  284,  290 },   -1 },
    { {   -1,   -1 },  129 },
    { {   -1,   -1 },  130 },
    { {  287,  328 },   -1 },
    { {  288,  388 },   -1 },
    { {  289,  345 },   -1 },
    { {   -1,   -1 },  131 },
    { {   -1,   -1 },  132 },
    { {  292,  296 },   -1 },
    { {  293,  294 },   -1 },
    { {   -1,   -1 },  133 },
    { {   -1,   -1 },  134 },
    { {   -1,   -1 },  135 },
    { {  297,  313 },   -1 },
    { {   -1,   -1 },  136 },
    { {  299,  300 },   -1 },
    { {   -1,   -1 },  137 },
    { {   -1,   -1 },  138 },
    { {  302,  305 },   -1 },
    { {  303,  304 },   -1 },
    { {   -1,   -1 },  139 },
    { {   -1,   -1 },  140 },
    { {  306,  308 },   -1 },
    { {   -1,   -1 },  141 },
    { {   -1,   -1 },  142 },
    { {   -1,   -1 },  143 },
    { {  310,  319 },   -1 },
    { {  311,  312 },   -1 },
    { {   -1,   -1 },  144 },
    { {   -1,   -1 },  145 },
    { {   -1,   -1 },  146 },
    { {  315,  350 },   -1 },
    { {  316,  325 },   -1 },
    { {  317,  322 },   -1 },
    { {  318,  321 },   -1 },
    { {   -1,   -1 },  147 },
    { {  320,  341 },   -1 },
    { {   -1,   -1 },  148 },
    { {   -1,   -1 },  149 },
    { {  323,  324 },   -1 },
    { {   -1,   -1 },  150 },
    { {   -1,   -1 },  151 },
    { {  326,  338 },   -1 },
    { {  327,  336 },   -1 },
    { {   -1,   -1 },  152 },
    { {  465,  329 },   -1 },
    { {  330,  355 },   -1 },
    { {  331,  344 },   -1 },
    { {   -1,   -1 },  153 },
    { {  333,  347 },   -1 },
    { {  334,  342 },   -1 },
    { {  335,  337 },   -1 },
    { {   -1,   -1 },  154 },
    { {   -1,   -1 },  155 },
    { {   -1,   -1 },  156 },
    { {  339,  340 },   -1 },
    { {   -1,   -1 },  157 },
    { {   -1,   -1 },  158 },
    { {   -1,   -1 },  159 },
    { {  343,  346 },   -1 },
    { {   -1,   -1 },  160 },
    { {   -1,   -1 },  161 },
    { {   -1,   -1 },  162 },
    { {   -1,   -1 },  163 },
    { {  348,  360 },   -1 },
    { {  349,  359 },   -1 },
    { {   -1,   -1 },  164 },
    { {  351,  369 },   -1 },
    { {  352,  357 },   -1 },
    { {  353,  354 },   -1 },
    { {   -1,   -1 },  165 },
    { {   -1,   -1 },  166 },
    { {  356,  366 },   -1 },
    { {   -1,   -1 },  167 },
    { {  358,  368 },   -1 },
    { {   -1,   -1 },  168 },
    { {   -1,   -1 },  169 },
    { {  361,  367 },   -1 },
    { {   -1,   -1 },  170 },
    { {  363,  417 },   -1 },
    { {  364,  449 },   -1 },
    { {  365,  434 },   -1 },
    { {   -1,   -1 },  171 },
    { {   -1,   -1 },  172 },
    { {   -1,   -1 },  173 },
    { {   -1,   -1 },  174 },
    { {  370,  385 },   -1 },
    { {  371,  383 },   -1 },
    { {   -1,   -1 },  175 },
    { {  373,  451 },   -1 },
    { {  374,  381 },   -1 },
    { {  375,  376 },   -1 },
    { {   -1,   -1 },  176 },
    { {   -1,   -1 },  177 },
    { {  378,  393 },   -1 },
    { {  379,  390 },   -1 },
    { {  380,  384 },   -1 },
    { {   -1,   -1 },  178 },
    { {  382,  437 },   -1 },
    { {   -1,   -1 },  179 },
    { {   -1,   -1 },  180 },
    { {   -1,   -1 },  181 },
    { {  386,  387 },   -1 },
    { {   -1,   -1 },  182 },
    { {   -1,   -1 },  183 },
    { {  389,  409 },   -1 },
    { {   -1,   -1 },  184 },
    { {  391,  392 },   -1 },
    { {   -1,   -1 },  185 },
    { {   -1,   -1 },  186 },
    { {  394,  400 },   -1 },
    { {  395,  399 },   -1 },
    { {   -1,   -1 },  187 },
    { {  397,  412 },   -1 },
    { {  398,  402 },   -1 },
    { {   -1,   -1 },  188 },
    { {   -1,   -1 },  189 },
    { {  401,  411 },   -1 },
    { {   -1,   -1 },  190 },
    { {   -1,   -1 },  191 },
    { {  404,  427 },   -1 },
    { {  405,  424 },   -1 },
    { {  406,  421 },   -1 },
    { {  407,  408 },   -1 },
    { {   -1,   -1 },  192 },
    { {   -1,   -1 },  193 },
    { {   -1,   -1 },  194 },
    { {   -1,   -1 },  195 },
    { {   -1,   -1 },  196 },
    { {  413,  474 },   -1 },
    { {   -1,   -1 },  197 },
    { {  415,  475 },   -1 },
    { {  416,  471 },   -1 },
    { {   -1,   -1 },  198 },
    { {  481,  418 },   -1 },
    { {  419,  478 },   -1 },
    { {  420,  435 },   -1 },
    { {   -1,   -1 },  199 },
    { {  422,  423 },   -1 },
    { {   -1,   -1 },  200 },
    { {   -1,   -1 },  201 },
    { {  425,  438 },   -1 },
    { {  426,  433 },   -1 },
    { {   -1,   -1 },  202 },
    { {  455,  428 },   -1 },
    { {  490,  429 },   -1 },
    { {  511,  430 },   -1 },
    { {  431,  432 },   -1 },
    { {   -1,   -1 },  203 },
    { {   -1,   -1 },  204 },
    { {   -1,   -1 },  205 },
    { {   -1,   -1 },  206 },
    { {   -1,   -1 },  207 },
    { {   -1,   -1 },  208 },
    { {   -1,   -1 },  209 },
    { {  439,  446 },   -1 },
    { {   -1,   -1 },  210 },
    { {  441,  494 },   -1 },
    { {  442,  461 },   -1 },
    { {  443,  447 },   -1 },
    { {  444,  445 },   -1 },
    { {   -1,   -1 },  211 },
    { {   -1,   -1 },  212 },
    { {   -1,   -1 },  213 },
    { {  448,  460 },   -1 },
    { {   -1,   -1 },  214 },
    { {  450,  467 },   -1 },
    { {   -1,   -1 },  215 },
    { {  452,  469 },   -1 },
    { {  453,  454 },   -1 },
    { {   -1,   -1 },  216 },
    { {   -1,   -1 },  217 },
    { {  456,  484 },   -1 },
    { {  457,  458 },   -1 },
    { {   -1,   -1 },  218 },
    { {   -1,   -1 },  219 },
    { {   -1,   -1 },  220 },
    { {   -1,   -1 },  221 },
    { {  462,  488 },   -1 },
    { {  463,  464 },   -1 },
    { {   -1,   -1 },  222 },
    { {   -1,   -1 },  223 },
    { {  466,  468 },   -1 },
    { {   -1,   -1 },  224 },
    { {   -1,   -1 },  225 },
    { {   -1,   -1 },  226 },
    { {  470,  472 },   -1 },
    { {   -1,   -1 },  227 },
    { {   -1,   -1 },  228 },
    { {   -1,   -1 },  229 },
    { {   -1,   -1 },  230 },
    { {   -1,   -1 },  231 },
    { {  476,  477 },   -1 },
    { {   -1,   -1 },  232 },
    { {   -1,   -1 },  233 },
    { {  479,  480 },   -1 },
    { {   -1,   -1 },  234 },
    { {   -1,   -1 },  235 },
    { {  482,  483 },   -1 },
    { {   -1,   -1 },  236 },
    { {   -1,   -1 },  237 },
    { {  485,  487 },   -1 },
    { {   -1,   -1 },  238 },
    { {   -1,   -1 },  239 },
    { {   -1,   -1 },  240 },
    { {  489,  493 },   -1 },
    { {   -1,   -1 },  241 },
    { {  491,  492 },   -1 },
    { {   -1,   -1 },  242 },
    { {   -1,   -1 },  243 },
    { {   -1,   -1 },  244 },
    { {  495,  503 },   -1 },
    { {  496,  499 },   -1 },
    { {  497,  498 },   -1 },
    { {   -1,   -1 },  245 },
    { {   -1,   -1 },  246 },
    { {  500,  501 },   -1 },
    { {   -1,   -1 },  247 },
    { {   -1,   -1 },  248 },
    { {   -1,   -1 },  249 },
    { {  504,  507 },   -1 },
    { {  505,  506 },   -1 },
    { {   -1,   -1 },  250 },
    { {   -1,   -1 },  251 },
    { {  508,  509 },   -1 },
    { {   -1,   -1 },  252 },
    { {   -1,   -1 },  253 },
    { {   -1,   -1 },  254 },
    { {   -1,   -1 },  255 },
    { {   -1,   -1 },  256 },
};

/* Appendix A, Table 1: index 1..61, in RFC order.  Lengths are
 * measured by the generator, not typed. */
const HpackField hpack_static_table[HPACK_STATIC_ENTRIES] = {
    { ":authority",                10 , "",              0  , 0 },
    { ":method",                   7  , "GET",           3  , 0 },
    { ":method",                   7  , "POST",          4  , 0 },
    { ":path",                     5  , "/",             1  , 0 },
    { ":path",                     5  , "/index.html",   11 , 0 },
    { ":scheme",                   7  , "http",          4  , 0 },
    { ":scheme",                   7  , "https",         5  , 0 },
    { ":status",                   7  , "200",           3  , 0 },
    { ":status",                   7  , "204",           3  , 0 },
    { ":status",                   7  , "206",           3  , 0 },
    { ":status",                   7  , "304",           3  , 0 },
    { ":status",                   7  , "400",           3  , 0 },
    { ":status",                   7  , "404",           3  , 0 },
    { ":status",                   7  , "500",           3  , 0 },
    { "accept-charset",            14 , "",              0  , 0 },
    { "accept-encoding",           15 , "gzip, deflate", 13 , 0 },
    { "accept-language",           15 , "",              0  , 0 },
    { "accept-ranges",             13 , "",              0  , 0 },
    { "accept",                    6  , "",              0  , 0 },
    { "access-control-allow-origin", 27 , "",              0  , 0 },
    { "age",                       3  , "",              0  , 0 },
    { "allow",                     5  , "",              0  , 0 },
    { "authorization",             13 , "",              0  , 0 },
    { "cache-control",             13 , "",              0  , 0 },
    { "content-disposition",       19 , "",              0  , 0 },
    { "content-encoding",          16 , "",              0  , 0 },
    { "content-language",          16 , "",              0  , 0 },
    { "content-length",            14 , "",              0  , 0 },
    { "content-location",          16 , "",              0  , 0 },
    { "content-range",             13 , "",              0  , 0 },
    { "content-type",              12 , "",              0  , 0 },
    { "cookie",                    6  , "",              0  , 0 },
    { "date",                      4  , "",              0  , 0 },
    { "etag",                      4  , "",              0  , 0 },
    { "expect",                    6  , "",              0  , 0 },
    { "expires",                   7  , "",              0  , 0 },
    { "from",                      4  , "",              0  , 0 },
    { "host",                      4  , "",              0  , 0 },
    { "if-match",                  8  , "",              0  , 0 },
    { "if-modified-since",         17 , "",              0  , 0 },
    { "if-none-match",             13 , "",              0  , 0 },
    { "if-range",                  8  , "",              0  , 0 },
    { "if-unmodified-since",       19 , "",              0  , 0 },
    { "last-modified",             13 , "",              0  , 0 },
    { "link",                      4  , "",              0  , 0 },
    { "location",                  8  , "",              0  , 0 },
    { "max-forwards",              12 , "",              0  , 0 },
    { "proxy-authenticate",        18 , "",              0  , 0 },
    { "proxy-authorization",       19 , "",              0  , 0 },
    { "range",                     5  , "",              0  , 0 },
    { "referer",                   7  , "",              0  , 0 },
    { "refresh",                   7  , "",              0  , 0 },
    { "retry-after",               11 , "",              0  , 0 },
    { "server",                    6  , "",              0  , 0 },
    { "set-cookie",                10 , "",              0  , 0 },
    { "strict-transport-security", 25 , "",              0  , 0 },
    { "transfer-encoding",         17 , "",              0  , 0 },
    { "user-agent",                10 , "",              0  , 0 },
    { "vary",                      4  , "",              0  , 0 },
    { "via",                       3  , "",              0  , 0 },
    { "www-authenticate",          16 , "",              0  , 0 },
};

/* ------------------------------------------------------------
 *  A dynamic table entry.  The name and value are copies owned by the
 *  context: the block they came from, and the scratch buffer, do not
 *  outlive hpack_decode().
 * ------------------------------------------------------------ */
typedef struct {
    char   *name;
    char   *value;
    size_t  name_len;
    size_t  value_len;
    size_t  size;           /* name_len + value_len + 32, RFC 7541 §4.1 */
} HpackEntry;

struct HpackCtx {
    HpackRole       role;
    size_t          table_max;      /* current maximum, §4.2           */
    size_t          table_limit;    /* decoder: SETTINGS ceiling, §6.3 */
    uint64_t        table_size;     /* sum of entry sizes, §4.1        */
    HpackEntry     *ent;            /* newest first                    */
    size_t          count;
    size_t          cap;
    int             huffman;        /* encoder: Huffman string lits    */
    HpackIndexing   indexing;       /* encoder: representation policy  */
    int             pending;        /* encoder: a §6.3 update is due   */
    int             has_pending_min;
    size_t          pending_min;    /* smallest size since last block  */
    size_t          pending_final;
    uint8_t        *scratch;        /* Huffman-decoded strings         */
    size_t          scratch_cap;
    size_t          fields_in_block;/* decoder: for §4.2 positioning   */
    HpackStats      st;
};

/* ------------------------------------------------------------
 *  Errors carry a sentence as well as a code.
 * ------------------------------------------------------------ */
static int fail(int rc, char *err, size_t errsz, const char *fmt, ...)
{
    if (err != NULL && errsz > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errsz, fmt, ap);
        va_end(ap);
    }
    return rc;
}

const char *hpack_strerror(int rc)
{
    switch (rc) {
    case HPACK_OK:              return "ok";
    case HPACK_ERR_ARG:         return "invalid argument";
    case HPACK_ERR_NOMEM:       return "out of memory";
    case HPACK_ERR_TRUNCATED:   return "truncated header block";
    case HPACK_ERR_BAD_INDEX:   return "index outside the tables";
    case HPACK_ERR_HUFFMAN:     return "invalid Huffman code";
    case HPACK_ERR_NOSPACE:     return "buffer too small";
    case HPACK_ERR_SIZE_UPDATE: return "table size update above the advertised limit";
    case HPACK_ERR_SIZE_POS:    return "table size update after a header field";
    case HPACK_ERR_OVERFLOW:    return "integer does not fit 32 bits";
    case HPACK_ERR_STATE:       return "wrong role or corrupt context";
    case HPACK_ERR_LIMIT:       return "value beyond the implementation limit";
    default:                    return "unknown HPACK status";
    }
}

/* ============================================================
 *  §5.1  Integer representation
 *
 *  The value occupies the low N bits of the first octet when it is
 *  smaller than 2^N - 1.  Otherwise those bits hold 2^N - 1 and the rest
 *  follows in 7-bit groups, least significant group first, each with the
 *  high bit set except the last (Figure 5 of the RFC).
 * ============================================================ */
int hpack_int_encode(uint32_t value, unsigned prefix_bits,
                     uint8_t *out, size_t cap, size_t *outlen)
{
    uint32_t mask;
    size_t n = 0;

    if (out == NULL || outlen == NULL || prefix_bits < 1 || prefix_bits > 8)
        return HPACK_ERR_ARG;
    mask = (prefix_bits == 8) ? 0xFFu : ((1u << prefix_bits) - 1u);

    if (value < mask) {
        if (cap < 1) return HPACK_ERR_NOSPACE;
        out[n++] = (uint8_t)value;
        *outlen = n;
        return HPACK_OK;
    }
    if (cap < 1) return HPACK_ERR_NOSPACE;
    out[n++] = (uint8_t)mask;       /* the prefix's all-ones escape value */
    value -= mask;
    while (value >= 128) {
        if (n >= cap) return HPACK_ERR_NOSPACE;
        out[n++] = (uint8_t)(0x80 | (value & 0x7Fu));
        value >>= 7;
    }
    if (n >= cap) return HPACK_ERR_NOSPACE;
    out[n++] = (uint8_t)value;
    *outlen = n;
    return HPACK_OK;
}

int hpack_int_decode(const uint8_t *in, size_t len, size_t *pos,
                     unsigned prefix_bits, uint32_t *value)
{
    uint32_t mask, v;
    unsigned shift = 0;

    if (in == NULL || pos == NULL || value == NULL ||
        prefix_bits < 1 || prefix_bits > 8)
        return HPACK_ERR_ARG;
    if (*pos >= len) return HPACK_ERR_TRUNCATED;
    mask = (prefix_bits == 8) ? 0xFFu : ((1u << prefix_bits) - 1u);
    v = (uint32_t)(in[(*pos)++] & mask);
    if (v < mask) {
        *value = v;
        return HPACK_OK;
    }
    for (;;) {
        uint8_t b;
        uint64_t add;
        if (*pos >= len) return HPACK_ERR_TRUNCATED;
        b = in[(*pos)++];
        add = (uint64_t)(b & 0x7Fu) << shift;
        if (add > (uint64_t)UINT32_MAX - v) return HPACK_ERR_OVERFLOW;
        v += (uint32_t)add;
        if ((b & 0x80u) == 0) break;
        shift += 7;
        /* More than five continuation octets cannot fit 32 bits, and §5.1
         * requires such an encoding to be a decoding error. */
        if (shift > 35) return HPACK_ERR_OVERFLOW;
    }
    *value = v;
    return HPACK_OK;
}

/* ============================================================
 *  Appendix B Huffman code
 * ============================================================ */
size_t hpack_huffman_encoded_len(const uint8_t *in, size_t len)
{
    uint64_t bits = 0;
    size_t i;

    if (in == NULL && len != 0) return SIZE_MAX;
    for (i = 0; i < len; i++)
        bits += huff_len[in[i]];
    if (bits > (uint64_t)SIZE_MAX - 7)
        return SIZE_MAX;
    return (size_t)((bits + 7) / 8);
}

size_t hpack_huffman_max_decoded(size_t encoded_len)
{
    /* The shortest code is 5 bits (symbols '0', '1', '2', 'a', 'c', 'e',
     * 'i', 'o', 's', 't'), so 8 encoded bits yield at most 8/5 octets. */
    if (encoded_len > (SIZE_MAX - 4) / 8) return SIZE_MAX;
    return (encoded_len * 8 + 4) / 5;
}

int hpack_huffman_encode(const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, size_t *outlen)
{
    uint64_t acc = 0;
    unsigned nbits = 0;
    size_t i, n = 0;

    if ((in == NULL && len != 0) || (out == NULL && cap != 0) || outlen == NULL)
        return HPACK_ERR_ARG;
    for (i = 0; i < len; i++) {
        unsigned l = huff_len[in[i]];
        acc = (acc << l) | huff_code[in[i]];
        nbits += l;
        while (nbits >= 8) {
            nbits -= 8;
            if (n >= cap) return HPACK_ERR_NOSPACE;
            out[n++] = (uint8_t)(acc >> nbits);
        }
        acc &= (nbits == 0) ? 0 : ((1ull << nbits) - 1);
    }
    if (nbits != 0) {
        unsigned pad = 8 - nbits;
        if (n >= cap) return HPACK_ERR_NOSPACE;
        out[n++] = (uint8_t)((acc << pad) | ((1u << pad) - 1u));
    }
    *outlen = n;
    return HPACK_OK;
}

int hpack_huffman_decode(const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, size_t *outlen)
{
    int node = 0;
    size_t i, n = 0;
    unsigned pending = 0;       /* bits consumed since the last symbol */

    if ((in == NULL && len != 0) || (out == NULL && cap != 0) || outlen == NULL)
        return HPACK_ERR_ARG;
    for (i = 0; i < len; i++) {
        int k;
        for (k = 7; k >= 0; k--) {
            int bit = (in[i] >> k) & 1;
            int next = huff_tree[node].child[bit];
            if (next < 0) return HPACK_ERR_HUFFMAN;   /* incomplete tree */
            node = next;
            pending++;
            if (huff_tree[node].sym >= 0) {
                int sym = huff_tree[node].sym;
                if (sym == 256) return HPACK_ERR_HUFFMAN;  /* §5.2: EOS in a literal */
                if (n >= cap) return HPACK_ERR_NOSPACE;
                out[n++] = (uint8_t)sym;
                node = 0;
                pending = 0;
            }
        }
    }
    /* §5.2: what is left must be at most 7 bits and must be the most
     * significant bits of EOS, i.e. ones. */
    if (pending > 7) return HPACK_ERR_HUFFMAN;
    if (pending != 0) {
        unsigned mask = (1u << pending) - 1u;
        if ((in[len - 1] & mask) != mask) return HPACK_ERR_HUFFMAN;
    }
    *outlen = n;
    return HPACK_OK;
}

int hpack_huffman_code(uint32_t symbol, uint32_t *code, unsigned *bits)
{
    if (symbol >= HPACK_HUFFMAN_SYMBOLS || code == NULL || bits == NULL)
        return HPACK_ERR_ARG;
    *code = huff_code[symbol];
    *bits = huff_len[symbol];
    return HPACK_OK;
}

int hpack_huffman_verify(uint32_t *bad_symbol)
{
    size_t leaves = 0, internal = 0, i;
    unsigned sym;

    if (bad_symbol != NULL) *bad_symbol = 0;
    for (i = 0; i < HUFF_TREE_NODES; i++) {
        const HpackHuffNode *n = &huff_tree[i];
        if (n->sym >= 0) {
            leaves++;
            if (n->child[0] >= 0 || n->child[1] >= 0) {
                if (bad_symbol != NULL) *bad_symbol = (uint32_t)n->sym;
                return HPACK_ERR_STATE;     /* a leaf with children */
            }
        } else {
            internal++;
            if (n->child[0] < 0 || n->child[1] < 0) {
                if (bad_symbol != NULL) *bad_symbol = (uint32_t)i;
                return HPACK_ERR_STATE;     /* an incomplete prefix code */
            }
        }
    }
    if (leaves != HPACK_HUFFMAN_SYMBOLS || internal != HPACK_HUFFMAN_SYMBOLS - 1) {
        if (bad_symbol != NULL) *bad_symbol = 0xFFFFFFFFu;
        return HPACK_ERR_STATE;
    }
    for (sym = 0; sym < HPACK_HUFFMAN_SYMBOLS; sym++) {
        unsigned l = huff_len[sym];
        int node = 0;
        unsigned k;
        for (k = 0; k < l; k++) {
            int bit = (int)((huff_code[sym] >> (l - 1 - k)) & 1u);
            int next = huff_tree[node].child[bit];
            if (next < 0) {
                if (bad_symbol != NULL) *bad_symbol = sym;
                return HPACK_ERR_STATE;
            }
            node = next;
        }
        if (huff_tree[node].sym != (int)sym) {
            if (bad_symbol != NULL) *bad_symbol = sym;
            return HPACK_ERR_STATE;
        }
    }
    return HPACK_OK;
}

/* ============================================================
 *  §5.2  String literal representation
 * ============================================================ */
int hpack_string_encode(const void *data, size_t len, int huffman,
                        uint8_t *out, size_t cap, size_t *outlen)
{
    uint8_t hdr[6];
    size_t hn, body, n = 0;
    int rc;

    if ((data == NULL && len != 0) || (out == NULL && cap != 0) || outlen == NULL)
        return HPACK_ERR_ARG;
    if (huffman) {
        body = hpack_huffman_encoded_len((const uint8_t *)data, len);
        if (body == SIZE_MAX) return HPACK_ERR_LIMIT;
    } else {
        body = len;
    }
    if ((uint64_t)body > (uint64_t)UINT32_MAX)
        return HPACK_ERR_LIMIT;             /* the length is a 32-bit integer */
    rc = hpack_int_encode((uint32_t)body, 7, hdr, sizeof hdr, &hn);
    if (rc != HPACK_OK) return rc;
    if (huffman) hdr[0] |= 0x80;
    if (hn > cap - n) return HPACK_ERR_NOSPACE;
    if (body > cap - n - hn) return HPACK_ERR_NOSPACE;
    if (hn != 0) memcpy(out + n, hdr, hn);
    n += hn;
    if (huffman) {
        size_t w = 0;
        if (body != 0) {
            rc = hpack_huffman_encode((const uint8_t *)data, len, out + n, body, &w);
            if (rc != HPACK_OK) return rc;
        }
        n += w;
    } else {
        if (len != 0) memcpy(out + n, data, len);
        n += len;
    }
    *outlen = n;
    return HPACK_OK;
}

int hpack_string_decode(const uint8_t *in, size_t len, size_t *pos,
                        char *out, size_t cap, size_t *outlen)
{
    uint32_t n;
    int huffman;
    int rc;
    size_t start;

    if (in == NULL || pos == NULL || outlen == NULL || (out == NULL && cap != 0))
        return HPACK_ERR_ARG;
    if (*pos >= len) return HPACK_ERR_TRUNCATED;
    start = *pos;
    huffman = (in[*pos] & 0x80u) != 0;
    rc = hpack_int_decode(in, len, pos, 7, &n);
    if (rc != HPACK_OK) {
        *pos = start;
        return rc;
    }
    if ((size_t)n > len - *pos) {
        *pos = start;
        return HPACK_ERR_TRUNCATED;
    }
    if (huffman) {
        rc = hpack_huffman_decode(in + *pos, n, (uint8_t *)out, cap, outlen);
        if (rc != HPACK_OK) {
            *pos = start;
            return rc;
        }
    } else {
        if ((size_t)n > cap) {
            *pos = start;
            return HPACK_ERR_NOSPACE;
        }
        if (n != 0) memcpy(out, in + *pos, n);
        *outlen = n;
    }
    *pos += n;
    return HPACK_OK;
}

/* ============================================================
 *  The dynamic table (§2.3.2, §4.1, §4.3, §4.4)
 * ============================================================ */
static void table_clear(HpackCtx *c)
{
    size_t i;
    for (i = 0; i < c->count; i++) {
        free(c->ent[i].name);
        free(c->ent[i].value);
        c->ent[i].name = NULL;
        c->ent[i].value = NULL;
    }
    c->count = 0;
    c->table_size = 0;
}

static void table_evict_oldest(HpackCtx *c)
{
    HpackEntry *e;

    if (c->count == 0) return;
    e = &c->ent[c->count - 1];
    c->table_size -= e->size;
    free(e->name);
    free(e->value);
    e->name = NULL;
    e->value = NULL;
    c->count--;
    c->st.evictions++;
}

static void table_reduce(HpackCtx *c, size_t max)
{
    while (c->count != 0 && c->table_size > (uint64_t)max)
        table_evict_oldest(c);
}

static int table_reserve(HpackCtx *c)
{
    size_t ncap;
    HpackEntry *e;

    if (c->count < c->cap) return HPACK_OK;
    ncap = (c->cap == 0) ? 8 : c->cap * 2;
    if (ncap < c->cap) return HPACK_ERR_NOMEM;
    e = realloc(c->ent, ncap * sizeof *e);
    if (e == NULL) return HPACK_ERR_NOMEM;
    c->ent = e;
    c->cap = ncap;
    return HPACK_OK;
}

/* Add one entry at the front, evicting from the end as §4.4 requires. */
static int table_insert(HpackCtx *c, const char *name, size_t name_len,
                        const char *value, size_t value_len)
{
    uint64_t size = (uint64_t)name_len + (uint64_t)value_len + HPACK_ENTRY_OVERHEAD;
    char *ncopy = NULL, *vcopy = NULL;
    int rc;

    if (size > (uint64_t)c->table_max) {
        /* §4.4: an entry larger than the maximum empties the table and is
         * not added.  This is not an error. */
        if (c->count != 0) {
            table_clear(c);
            c->st.table_clears++;
        }
        return HPACK_OK;
    }
    /* §4.4 warns that the new entry may reference the name of an entry
     * this insertion is about to evict, so take the copies first.  The
     * same reasoning protects the caller's pointer, which may point into
     * the table. */
    ncopy = malloc(name_len != 0 ? name_len : 1);
    vcopy = malloc(value_len != 0 ? value_len : 1);
    if (ncopy == NULL || vcopy == NULL) {
        free(ncopy);
        free(vcopy);
        return HPACK_ERR_NOMEM;
    }
    if (name_len != 0) memcpy(ncopy, name, name_len);
    if (value_len != 0) memcpy(vcopy, value, value_len);

    while (c->count != 0 && c->table_size + size > (uint64_t)c->table_max)
        table_evict_oldest(c);

    rc = table_reserve(c);
    if (rc != HPACK_OK) {
        free(ncopy);
        free(vcopy);
        return rc;
    }
    if (c->count != 0)
        memmove(&c->ent[1], &c->ent[0], c->count * sizeof c->ent[0]);
    c->ent[0].name = ncopy;
    c->ent[0].value = vcopy;
    c->ent[0].name_len = name_len;
    c->ent[0].value_len = value_len;
    c->ent[0].size = (size_t)size;
    c->count++;
    c->table_size += size;
    c->st.inserts++;
    return HPACK_OK;
}

/* ============================================================
 *  Context lifecycle
 * ============================================================ */
HpackCtx *hpack_new(HpackRole role, size_t max_table_size)
{
    HpackCtx *c;
    uint64_t max = max_table_size;

    if (role != HPACK_ROLE_DECODER && role != HPACK_ROLE_ENCODER)
        return NULL;
    /* §5.1 integers (and therefore table sizes) are at most 32 bits. */
    if (max > (uint64_t)UINT32_MAX)
        return NULL;
    c = calloc(1, sizeof *c);
    if (c == NULL) return NULL;
    c->role = role;
    c->table_max = max_table_size;
    c->table_limit = max_table_size;
    c->huffman = 1;
    c->indexing = HPACK_INDEX_AUTO;
    return c;
}

void hpack_free(HpackCtx *c)
{
    if (c == NULL) return;
    table_clear(c);
    free(c->ent);
    free(c->scratch);
    free(c);
}

int hpack_reset(HpackCtx *c)
{
    if (c == NULL) return HPACK_ERR_ARG;
    table_clear(c);
    c->pending = 0;
    c->has_pending_min = 0;
    c->pending_min = 0;
    c->pending_final = 0;
    c->fields_in_block = 0;
    memset(&c->st, 0, sizeof c->st);
    return HPACK_OK;
}

/* ============================================================
 *  Table lookups (§2.3.3)
 * ============================================================ */
const HpackField *hpack_static_entry(uint32_t index)
{
    if (index < 1 || index > HPACK_STATIC_ENTRIES) return NULL;
    return &hpack_static_table[index - 1];
}

int hpack_lookup(const HpackCtx *c, uint32_t index, HpackField *out)
{
    size_t di;

    if (c == NULL || out == NULL) return HPACK_ERR_ARG;
    if (index == 0) return HPACK_ERR_BAD_INDEX;      /* §6.1: index 0 is an error */
    if (index <= HPACK_STATIC_ENTRIES) {
        *out = hpack_static_table[index - 1];
        return HPACK_OK;
    }
    di = (size_t)(index - HPACK_STATIC_ENTRIES - 1);
    if (di >= c->count) return HPACK_ERR_BAD_INDEX;
    out->name = c->ent[di].name;
    out->name_len = c->ent[di].name_len;
    out->value = c->ent[di].value;
    out->value_len = c->ent[di].value_len;
    out->sensitive = 0;
    return HPACK_OK;
}

int hpack_dynamic_entry(const HpackCtx *c, size_t i, HpackField *out)
{
    if (c == NULL || out == NULL) return HPACK_ERR_ARG;
    if (i >= c->count) return HPACK_ERR_BAD_INDEX;
    out->name = c->ent[i].name;
    out->name_len = c->ent[i].name_len;
    out->value = c->ent[i].value;
    out->value_len = c->ent[i].value_len;
    out->sensitive = 0;
    return HPACK_OK;
}

size_t hpack_table_size(const HpackCtx *c)  { return c ? (size_t)c->table_size : 0; }
size_t hpack_table_count(const HpackCtx *c) { return c ? c->count : 0; }
size_t hpack_table_max(const HpackCtx *c)   { return c ? c->table_max : 0; }

const HpackStats *hpack_stats(const HpackCtx *c)
{
    static const HpackStats zero;
    return c ? &c->st : &zero;
}

void hpack_stats_reset(HpackCtx *c)
{
    if (c != NULL) memset(&c->st, 0, sizeof c->st);
}

void hpack_set_huffman(HpackCtx *c, int on) { if (c != NULL) c->huffman = on != 0; }
int  hpack_huffman_enabled(const HpackCtx *c) { return c ? c->huffman : 0; }
void hpack_set_indexing(HpackCtx *c, HpackIndexing mode)
{
    if (c != NULL) c->indexing = mode;
}
HpackIndexing hpack_indexing(const HpackCtx *c)
{
    return c ? c->indexing : HPACK_INDEX_AUTO;
}

/* ============================================================
 *  Decoding
 * ============================================================ */

/* The H bit and the length of a string literal at *pos.  On return *pos
 * sits on the first payload octet. */
typedef struct {
    const uint8_t *p;       /* payload */
    uint32_t       enc_len; /* octets on the wire */
    size_t         n;       /* decoded length, once materialised */
    int            huffman;
} HpackStr;

static int str_head(const uint8_t *in, size_t len, size_t *pos, HpackStr *s)
{
    int rc;

    if (*pos >= len) return HPACK_ERR_TRUNCATED;
    s->huffman = (in[*pos] & 0x80u) != 0;
    s->n = 0;
    rc = hpack_int_decode(in, len, pos, 7, &s->enc_len);
    if (rc != HPACK_OK) return rc;
    if ((size_t)s->enc_len > len - *pos) return HPACK_ERR_TRUNCATED;
    s->p = in + *pos;
    return HPACK_OK;
}

/* A zero-length Huffman string decodes to nothing, and the scratch buffer
 * may not even be allocated; point the field at a real empty string so a
 * caller can use it without a NULL check. */
static const char hpack_empty[1] = "";

static int scratch_need(HpackCtx *c, size_t need)
{
    uint8_t *p;

    if (need <= c->scratch_cap) return HPACK_OK;
    p = realloc(c->scratch, need);
    if (p == NULL) return HPACK_ERR_NOMEM;
    c->scratch = p;
    c->scratch_cap = need;
    return HPACK_OK;
}

/* §6.2.1 / §6.2.2 / §6.2.3.  `prefix` is 6 for incremental indexing and 4
 * for the two literals; `never` marks the §6.2.3 representation.  Adding
 * the field to the dynamic table is hpack_decode's job, because it has to
 * happen after the field has been handed to the caller. */
static int decode_literal(HpackCtx *c, const uint8_t *blk, size_t len, size_t *pos,
                          unsigned prefix, int never,
                          HpackField *f, char *err, size_t errsz)
{
    HpackStr nm, vl;
    uint32_t name_index = 0;
    int literal_name;
    const uint8_t *name_p, *val_p;
    size_t name_n, val_n, nbound, vbound, need;
    int rc;

    memset(&nm, 0, sizeof nm);
    memset(&vl, 0, sizeof vl);

    rc = hpack_int_decode(blk, len, pos, prefix, &name_index);
    if (rc != HPACK_OK)
        return fail(rc, err, errsz, "literal name index: %s", hpack_strerror(rc));

    literal_name = (name_index == 0);
    if (literal_name) {
        rc = str_head(blk, len, pos, &nm);
        if (rc != HPACK_OK)
            return fail(rc, err, errsz, "literal name: %s", hpack_strerror(rc));
        nm.n = nm.enc_len;
        *pos += nm.enc_len;
    } else {
        HpackField nf;
        rc = hpack_lookup(c, name_index, &nf);
        if (rc != HPACK_OK)
            return fail(rc, err, errsz,
                        "literal name index %lu: the table holds %lu static and %lu dynamic entries",
                        (unsigned long)name_index,
                        (unsigned long)HPACK_STATIC_ENTRIES,
                        (unsigned long)c->count);
        nm.p = (const uint8_t *)nf.name;
        nm.n = nf.name_len;
        if (name_index > c->st.max_index) c->st.max_index = name_index;
    }

    rc = str_head(blk, len, pos, &vl);
    if (rc != HPACK_OK)
        return fail(rc, err, errsz, "literal value: %s", hpack_strerror(rc));

    /* Reserve room for both decoded strings before decoding either, so a
     * reallocation cannot move a name that already points into scratch. */
    nbound = nm.huffman ? hpack_huffman_max_decoded(nm.enc_len) : 0;
    vbound = vl.huffman ? hpack_huffman_max_decoded(vl.enc_len) : 0;
    if (nbound == SIZE_MAX || vbound == SIZE_MAX || nbound > SIZE_MAX - vbound)
        return fail(HPACK_ERR_LIMIT, err, errsz,
                    "string literal at %lu is too long for this implementation",
                    (unsigned long)*pos);
    need = nbound + vbound;
    if (need != 0) {
        rc = scratch_need(c, need);
        if (rc != HPACK_OK) return fail(rc, err, errsz, "scratch: %s", hpack_strerror(rc));
    }

    if (nm.huffman) {
        size_t got = 0;
        rc = hpack_huffman_decode(nm.p, nm.enc_len, c->scratch, nbound, &got);
        if (rc != HPACK_OK)
            return fail(rc, err, errsz, "huffman name: %s", hpack_strerror(rc));
        name_p = got != 0 ? c->scratch : (const uint8_t *)hpack_empty;
        name_n = got;
        c->st.huffman_strings++;
        c->st.huffman_in += nm.enc_len;
        c->st.huffman_out += got;
    } else {
        name_p = nm.p;
        name_n = nm.n;
        if (literal_name) c->st.plain_strings++;
    }

    if (vl.huffman) {
        size_t off = nm.huffman ? name_n : 0;
        size_t got = 0;
        if (off + vbound > need)
            return fail(HPACK_ERR_STATE, err, errsz, "huffman scratch invariant");
        rc = hpack_huffman_decode(vl.p, vl.enc_len, c->scratch + off, vbound, &got);
        if (rc != HPACK_OK)
            return fail(rc, err, errsz, "huffman value: %s", hpack_strerror(rc));
        val_p = got != 0 ? c->scratch + off : (const uint8_t *)hpack_empty;
        val_n = got;
        c->st.huffman_strings++;
        c->st.huffman_in += vl.enc_len;
        c->st.huffman_out += got;
    } else {
        val_p = vl.p;
        val_n = vl.enc_len;
        c->st.plain_strings++;
    }
    *pos += vl.enc_len;

    f->name = (const char *)name_p;
    f->name_len = name_n;
    f->value = (const char *)val_p;
    f->value_len = val_n;
    f->sensitive = never ? 1u : 0u;
    return HPACK_OK;
}

int hpack_decode(HpackCtx *c, const uint8_t *block, size_t len,
                 int (*emit)(void *user, const HpackField *field), void *user,
                 HpackBlock *out, char *err, size_t errsz)
{
    HpackBlock b;
    size_t pos = 0;
    int rc;

    memset(&b, 0, sizeof b);
    if (c == NULL || emit == NULL || (block == NULL && len != 0))
        return fail(HPACK_ERR_ARG, err, errsz, "hpack_decode: null argument");
    if (c->role != HPACK_ROLE_DECODER)
        return fail(HPACK_ERR_STATE, err, errsz,
                    "hpack_decode: this context was created as an encoder");
    c->fields_in_block = 0;

    while (pos < len) {
        uint8_t first = block[pos];
        int is_field = 1;

        if ((first & 0x80u) != 0) {                 /* §6.1 indexed field */
            uint32_t index = 0;
            HpackField f;

            rc = hpack_int_decode(block, len, &pos, 7, &index);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "indexed field: %s", hpack_strerror(rc));
            rc = hpack_lookup(c, index, &f);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz,
                            "indexed field %lu: the table holds %lu static and %lu dynamic entries",
                            (unsigned long)index,
                            (unsigned long)HPACK_STATIC_ENTRIES,
                            (unsigned long)c->count);
            c->st.indexed++;
            b.list_size += f.name_len + f.value_len + HPACK_ENTRY_OVERHEAD;
            rc = emit(user, &f);
            if (rc != 0) return rc;                 /* caller's own limit */
            if (index > c->st.max_index) c->st.max_index = index;

        } else if ((first & 0x40u) != 0) {          /* §6.2.1 */
            HpackField f;

            rc = decode_literal(c, block, len, &pos, 6, 0, &f, err, errsz);
            if (rc != HPACK_OK) return rc;
            b.list_size += f.name_len + f.value_len + HPACK_ENTRY_OVERHEAD;
            c->st.literal_indexed++;
            /* Emit before the insertion: f.name may point into an entry
             * that this insertion evicts (§4.4's warning in reverse). */
            rc = emit(user, &f);
            if (rc != 0) return rc;
            rc = table_insert(c, f.name, f.name_len, f.value, f.value_len);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "dynamic table: %s", hpack_strerror(rc));

        } else if ((first & 0xE0u) == 0x20u) {      /* §6.3 size update */
            uint32_t newsize = 0;

            if (c->fields_in_block != 0)
                return fail(HPACK_ERR_SIZE_POS, err, errsz,
                            "table size update after %lu header field(s); RFC 7541 §4.2 puts it at the start of the block",
                            (unsigned long)c->fields_in_block);
            rc = hpack_int_decode(block, len, &pos, 5, &newsize);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "table size update: %s", hpack_strerror(rc));
            if ((size_t)newsize > c->table_limit)
                return fail(HPACK_ERR_SIZE_UPDATE, err, errsz,
                            "table size update %lu exceeds the advertised limit %lu",
                            (unsigned long)newsize, (unsigned long)c->table_limit);
            c->table_max = newsize;
            table_reduce(c, newsize);
            c->st.size_updates++;
            if (newsize > c->st.max_table_size) c->st.max_table_size = newsize;
            b.size_updates++;
            is_field = 0;

        } else {                                    /* §6.2.2 / §6.2.3 */
            HpackField f;
            int never = (first & 0x10u) != 0;

            rc = decode_literal(c, block, len, &pos, 4, never, &f, err, errsz);
            if (rc != HPACK_OK) return rc;
            b.list_size += f.name_len + f.value_len + HPACK_ENTRY_OVERHEAD;
            if (never) c->st.literal_never++;
            else       c->st.literal_noindex++;
            rc = emit(user, &f);
            if (rc != 0) return rc;
        }
        if (is_field) {
            b.fields++;
            c->fields_in_block++;
        }
    }

    b.block_bytes = len;
    b.table_size = (size_t)c->table_size;
    b.table_count = c->count;
    c->st.blocks++;
    c->st.fields += b.fields;
    c->st.block_bytes += b.block_bytes;
    c->st.list_size += b.list_size;
    if (out != NULL) *out = b;
    return HPACK_OK;
}

/* ============================================================
 *  Encoding
 * ============================================================ */
static int mem_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) return 0;
    if (alen == 0) return 1;
    return memcmp(a, b, alen) == 0;
}

/* Exact name and value, static table first, then the dynamic table from
 * newest to oldest.  Returns 1 and the RFC index, or 0. */
static int find_exact(const HpackCtx *c, const HpackField *f, uint32_t *index)
{
    size_t i;
    for (i = 0; i < HPACK_STATIC_ENTRIES; i++) {
        if (mem_eq(f->name, f->name_len,
                   hpack_static_table[i].name, hpack_static_table[i].name_len) &&
            mem_eq(f->value, f->value_len,
                   hpack_static_table[i].value, hpack_static_table[i].value_len)) {
            *index = (uint32_t)(i + 1);
            return 1;
        }
    }
    for (i = 0; i < c->count; i++) {
        if (mem_eq(f->name, f->name_len, c->ent[i].name, c->ent[i].name_len) &&
            mem_eq(f->value, f->value_len, c->ent[i].value, c->ent[i].value_len)) {
            *index = (uint32_t)(HPACK_STATIC_ENTRIES + 1 + i);
            return 1;
        }
    }
    return 0;
}

/* Name only, for a literal's indexed-name form.  The lowest static index
 * wins because it is the shortest to encode; then the newest dynamic one. */
static int find_name(const HpackCtx *c, const HpackField *f, uint32_t *index)
{
    size_t i;
    for (i = 0; i < HPACK_STATIC_ENTRIES; i++) {
        if (mem_eq(f->name, f->name_len,
                   hpack_static_table[i].name, hpack_static_table[i].name_len)) {
            *index = (uint32_t)(i + 1);
            return 1;
        }
    }
    for (i = 0; i < c->count; i++) {
        if (mem_eq(f->name, f->name_len, c->ent[i].name, c->ent[i].name_len)) {
            *index = (uint32_t)(HPACK_STATIC_ENTRIES + 1 + i);
            return 1;
        }
    }
    return 0;
}

static int put_bytes(uint8_t *out, size_t cap, size_t *n,
                     const void *p, size_t len)
{
    if (len > cap - *n) return HPACK_ERR_NOSPACE;
    if (len != 0) memcpy(out + *n, p, len);
    *n += len;
    return HPACK_OK;
}

/* Write an integer with `prefix` bits and OR `pattern` into the high bits
 * of the first octet, which is how every §6 representation starts. */
static int put_int(uint8_t *out, size_t cap, size_t *n,
                   uint32_t value, unsigned prefix, uint8_t pattern)
{
    uint8_t tmp[6];
    size_t tn = 0;
    int rc = hpack_int_encode(value, prefix, tmp, sizeof tmp, &tn);

    if (rc != HPACK_OK) return rc;
    tmp[0] |= pattern;
    return put_bytes(out, cap, n, tmp, tn);
}

static int put_string(HpackCtx *c, uint8_t *out, size_t cap, size_t *n,
                      const char *s, size_t len)
{
    uint8_t hdr[6];
    size_t hn = 0, body, w = 0;
    int rc;

    if (c->huffman) {
        body = hpack_huffman_encoded_len((const uint8_t *)s, len);
        if (body == SIZE_MAX) return HPACK_ERR_LIMIT;
    } else {
        body = len;
    }
    if ((uint64_t)body > (uint64_t)UINT32_MAX)
        return HPACK_ERR_LIMIT;
    rc = hpack_int_encode((uint32_t)body, 7, hdr, sizeof hdr, &hn);
    if (rc != HPACK_OK) return rc;
    if (c->huffman) hdr[0] |= 0x80;
    rc = put_bytes(out, cap, n, hdr, hn);
    if (rc != HPACK_OK) return rc;
    if (body > cap - *n) return HPACK_ERR_NOSPACE;
    if (c->huffman) {
        if (body != 0) {
            rc = hpack_huffman_encode((const uint8_t *)s, len, out + *n, body, &w);
            if (rc != HPACK_OK) return rc;
        }
        *n += w;
        c->st.huffman_strings++;
        c->st.huffman_in += w;
        c->st.huffman_out += len;
    } else {
        if (len != 0) memcpy(out + *n, s, len);
        *n += len;
        c->st.plain_strings++;
    }
    return HPACK_OK;
}

int hpack_encoder_set_max(HpackCtx *c, size_t max_table_size)
{
    uint64_t max = max_table_size;

    if (c == NULL) return HPACK_ERR_ARG;
    if (c->role != HPACK_ROLE_ENCODER) return HPACK_ERR_STATE;
    if (max > (uint64_t)UINT32_MAX) return HPACK_ERR_ARG;
    if (max_table_size == c->table_max && !c->pending)
        return HPACK_OK;                    /* no change, nothing to signal */

    if (max_table_size < c->table_max) {
        /* §4.2: when the size changes more than once before the next
         * block, the smallest value must be signalled first. */
        if (!c->has_pending_min || max_table_size < c->pending_min) {
            c->pending_min = max_table_size;
            c->has_pending_min = 1;
        }
    }
    c->pending_final = max_table_size;
    c->pending = 1;
    c->table_max = max_table_size;
    table_reduce(c, max_table_size);
    return HPACK_OK;
}

int hpack_decoder_set_limit(HpackCtx *c, size_t limit)
{
    if (c == NULL) return HPACK_ERR_ARG;
    if (c->role != HPACK_ROLE_DECODER) return HPACK_ERR_STATE;
    if ((uint64_t)limit > (uint64_t)UINT32_MAX) return HPACK_ERR_ARG;
    /* Only the ceiling changes here.  The table itself shrinks when the
     * peer signals it with a §6.3 update, because until that update
     * arrives the peer may still reference entries that fit the old,
     * larger size: §4.2 puts the update at the start of the first block
     * after the change, and §4.3 evicts when that new size is applied. */
    c->table_limit = limit;
    return HPACK_OK;
}

size_t hpack_encode_bound(const HpackField *fields, size_t count, int huffman)
{
    size_t total = 12;      /* at most two size updates, 6 octets each */
    size_t i;

    if (fields == NULL && count != 0) return 0;
    for (i = 0; i < count; i++) {
        size_t nl, vl;
        if (huffman) {
            nl = hpack_huffman_encoded_len((const uint8_t *)fields[i].name,
                                           fields[i].name_len);
            vl = hpack_huffman_encoded_len((const uint8_t *)fields[i].value,
                                           fields[i].value_len);
            if (nl == SIZE_MAX || vl == SIZE_MAX) return SIZE_MAX;
        } else {
            nl = fields[i].name_len;
            vl = fields[i].value_len;
        }
        /* Worst case is a literal with a literal name: 6 octets of name
         * index (the pattern octet plus five continuation octets), 6 for
         * the name's length, the name, 6 for the value's length and the
         * value.  An indexed representation is at most 6 octets, so it is
         * covered too.  The 12 at the top covers two size updates, 6 each. */
        if (nl > SIZE_MAX - total - 18 || vl > SIZE_MAX - total - 18 - nl)
            return SIZE_MAX;
        total += 18 + nl + vl;
    }
    return total;
}

int hpack_encode(HpackCtx *c, const HpackField *fields, size_t count,
                 uint8_t *out, size_t cap, size_t *outlen,
                 HpackBlock *outinfo, char *err, size_t errsz)
{
    HpackBlock b;
    size_t n = 0, i;
    int rc;

    memset(&b, 0, sizeof b);
    if (c == NULL || outlen == NULL || (out == NULL && cap != 0) ||
        (fields == NULL && count != 0))
        return fail(HPACK_ERR_ARG, err, errsz, "hpack_encode: null argument");
    if (c->role != HPACK_ROLE_ENCODER)
        return fail(HPACK_ERR_STATE, err, errsz,
                    "hpack_encode: this context was created as a decoder");

    /* §4.2: the pending size change, smallest value first. */
    if (c->pending) {
        if (c->has_pending_min && c->pending_min < c->pending_final) {
            rc = put_int(out, cap, &n, (uint32_t)c->pending_min, 5, 0x20);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "size update needs more than %lu octets",
                            (unsigned long)cap);
            c->st.size_updates++;
            b.size_updates++;
        }
        rc = put_int(out, cap, &n, (uint32_t)c->pending_final, 5, 0x20);
        if (rc != HPACK_OK)
            return fail(rc, err, errsz, "size update needs more than %lu octets",
                        (unsigned long)cap);
        c->st.size_updates++;
        b.size_updates++;
        if (c->pending_final > c->st.max_table_size)
            c->st.max_table_size = (uint32_t)c->pending_final;
        c->pending = 0;
        c->has_pending_min = 0;
    }

    for (i = 0; i < count; i++) {
        const HpackField *f = &fields[i];
        uint32_t exact = 0, name_index = 0;
        int have_exact, have_name, representation, add = 0;

        /* A zero-length name is representable in HPACK (§5.2 allows a
         * zero-length string) and this layer does not police HTTP
         * semantics; the HTTP/2 layer above is where an empty name is
         * refused.  Keeping it encodable means a field that came in can
         * always be sent out again. */

        have_exact = find_exact(c, f, &exact);
        if (c->indexing != HPACK_INDEX_NEVER && !f->sensitive && have_exact) {
            representation = 0;                 /* indexed field */
        } else if (f->sensitive || c->indexing == HPACK_INDEX_NEVER) {
            representation = 3;                 /* never indexed */
        } else if (c->indexing == HPACK_INDEX_NONE) {
            representation = 2;                 /* without indexing */
        } else {
            uint64_t size = (uint64_t)f->name_len + (uint64_t)f->value_len +
                            HPACK_ENTRY_OVERHEAD;
            if (size > (uint64_t)c->table_max) {
                /* Adding it would empty the table (§4.4); send it as a
                 * literal without indexing instead, so both tables stay
                 * as they are. */
                representation = 2;
            } else {
                representation = 1;             /* incremental indexing */
                add = 1;
            }
        }

        if (representation == 0) {
            rc = put_int(out, cap, &n, exact, 7, 0x80);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "indexed field: %s", hpack_strerror(rc));
            c->st.indexed++;
            if (exact > c->st.max_index) c->st.max_index = exact;
        } else {
            unsigned prefix;
            uint8_t pattern;
            if (representation == 1)      { prefix = 6; pattern = 0x40; }
            else if (representation == 2) { prefix = 4; pattern = 0x00; }
            else                          { prefix = 4; pattern = 0x10; }

            have_name = find_name(c, f, &name_index);
            rc = put_int(out, cap, &n, have_name ? name_index : 0, prefix, pattern);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "literal field: %s", hpack_strerror(rc));
            if (!have_name) {
                rc = put_string(c, out, cap, &n, f->name, f->name_len);
                if (rc != HPACK_OK)
                    return fail(rc, err, errsz, "literal name: %s", hpack_strerror(rc));
            }
            rc = put_string(c, out, cap, &n, f->value, f->value_len);
            if (rc != HPACK_OK)
                return fail(rc, err, errsz, "literal value: %s", hpack_strerror(rc));

            if (representation == 1) c->st.literal_indexed++;
            else if (representation == 2) c->st.literal_noindex++;
            else c->st.literal_never++;

            if (add) {
                /* The name index was already written, so evicting the entry
                 * it named is harmless here; table_insert copies first in
                 * any case. */
                rc = table_insert(c, f->name, f->name_len, f->value, f->value_len);
                if (rc != HPACK_OK)
                    return fail(rc, err, errsz, "dynamic table: %s", hpack_strerror(rc));
            }
        }
        b.fields++;
        b.list_size += f->name_len + f->value_len + HPACK_ENTRY_OVERHEAD;
    }

    b.block_bytes = n;
    b.table_size = (size_t)c->table_size;
    b.table_count = c->count;
    c->st.blocks++;
    c->st.fields += b.fields;
    c->st.block_bytes += n;
    c->st.list_size += b.list_size;
    *outlen = n;
    if (outinfo != NULL) *outinfo = b;
    return HPACK_OK;
}

/* ============================================================
 *  §7.1.3: names whose values are worth keeping out of the table
 * ============================================================ */
static int ieq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

int hpack_sensitive_name(const char *name, size_t len)
{
    if (name == NULL) return 0;
    if (len == 6 && ieq(name, "cookie", 6)) return 1;
    if (len == 13 && ieq(name, "authorization", 13)) return 1;
    if (len == 10 && ieq(name, "set-cookie", 10)) return 1;
    if (len == 19 && ieq(name, "proxy-authorization", 19)) return 1;
    return 0;
}
