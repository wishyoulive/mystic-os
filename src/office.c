/* ============================================================
 *  Mystic OS - office.c
 *
 *  Text extraction from Office containers, with structure.
 *
 *  Everything here is read-only and self-contained: a ZIP reader
 *  with its own raw-deflate decoder, a small streaming XML
 *  tokeniser, and one extractor per family of document. No part
 *  of this file draws or assumes a screen.
 *
 *  Honesty rules this file follows:
 *    - every character returned came from the document;
 *    - a table cell keeps the row/column the file assigned it;
 *    - formats that are not parsed are detected and named, never
 *      decoded into garbage;
 *    - formatting we do not read (page layout, images, formulas)
 *      is listed in doc->message, not silently applied.
 * ============================================================ */
#include "office.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ limits
 * These are real caps, not guesses: when one is hit the document is
 * marked truncated and the viewer says so. They exist so a malformed
 * or hostile file cannot make a 128 MB desktop allocate forever. */
#define OFFICE_MAX_FILE     (48u * 1024u * 1024u)
#define OFFICE_ARENA_CAP    (16u * 1024u * 1024u)
#define OFFICE_MAX_BLOCKS   8192
#define OFFICE_MAX_ROWS     400
#define OFFICE_MAX_COLS     48
#define OFFICE_MAX_SHEETS   32
#define OFFICE_MAX_STYLES   128
#define OFFICE_MAX_SHARED   200000
#define OFFICE_PLAIN_CAP    (24u * 1024u * 1024u)

/* ------------------------------------------------------------ bytes */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ------------------------------------------------------------ CRC-32
 * ZIP stores one per entry; verifying it is how the reader knows the
 * inflate produced the bytes the writer compressed, not something that
 * happened to fit the buffer. */
static uint32_t crc_tab[256];
static int crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_calc(const uint8_t *p, size_t n) {
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ============================================================
 *  Inflate (RFC 1951), the raw deflate stream inside a ZIP entry.
 *  Bit-by-bit Huffman decoding is slower than a table decoder and
 *  much easier to verify: every branch below is from the RFC.
 * ============================================================ */
typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint32_t bits;
    int nbits;
    int err;
} InBit;

static uint32_t in_get(InBit *b, int need) {
    if (need <= 0) return 0;
    while (b->nbits < need) {
        if (b->pos >= b->n) { b->err = 1; return 0; }
        b->bits |= (uint32_t)b->in[b->pos++] << b->nbits;
        b->nbits += 8;
    }
    uint32_t v = b->bits & ((1u << need) - 1u);
    b->bits >>= need;
    b->nbits -= need;
    return v;
}

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} InHuff;

static int in_huff_build(InHuff *h, const uint8_t *lens, int n) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    uint16_t offs[16];
    offs[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

static int in_huff_dec(InBit *b, const InHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)in_get(b, 1);
        int count = h->count[len];
        if (code - first < count) {
            int i = index + (code - first);
            if (i < 0 || i >= 288) { b->err = 1; return -1; }
            return h->symbol[i];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    b->err = 1;
    return -1;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int in_codes(InBit *b, const InHuff *lit, const InHuff *dist,
                    uint8_t *out, size_t cap, size_t *outlen) {
    for (;;) {
        int sym = in_huff_dec(b, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*outlen >= cap) return -1;
            out[(*outlen)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else if (sym <= 285) {
            int idx = sym - 257;
            size_t len = (size_t)len_base[idx] + in_get(b, len_extra[idx]);
            int dsym = in_huff_dec(b, dist);
            if (dsym < 0 || dsym > 29) return -1;
            size_t d = (size_t)dist_base[dsym] + in_get(b, dist_extra[dsym]);
            if (d == 0 || d > *outlen) return -1;
            if (*outlen + len > cap) return -1;
            for (size_t k = 0; k < len; k++, (*outlen)++)
                out[*outlen] = out[*outlen - d];
        } else {
            return -1;
        }
    }
}

/* Returns 0 and the produced length, or -1 on malformed input. */
static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out,
                       size_t cap, size_t *outlen) {
    InBit b;
    memset(&b, 0, sizeof(b));
    b.in = in;
    b.n = inlen;
    size_t used = 0;
    int final = 0;

    while (!final) {
        final = (int)in_get(&b, 1);
        int type = (int)in_get(&b, 2);
        if (b.err) return -1;

        if (type == 0) {
            /* stored: align to a byte, then LEN/NLEN and raw bytes */
            b.bits = 0;
            b.nbits = 0;
            if (b.pos + 4 > b.n) return -1;
            unsigned len = rd16(b.in + b.pos);
            unsigned nlen = rd16(b.in + b.pos + 2);
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (b.pos + len > b.n) return -1;
            if (used + len > cap) return -1;
            memcpy(out + used, b.in + b.pos, len);
            used += len;
            b.pos += len;
        } else if (type == 1 || type == 2) {
            InHuff lit, dist;
            if (type == 1) {
                uint8_t ll[288];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                uint8_t dl[30];
                for (int i = 0; i < 30; i++) dl[i] = 5;
                if (in_huff_build(&lit, ll, 288) || in_huff_build(&dist, dl, 30))
                    return -1;
            } else {
                int hlit = (int)in_get(&b, 5) + 257;
                int hdist = (int)in_get(&b, 5) + 1;
                int hclen = (int)in_get(&b, 4) + 4;
                if (b.err || hlit > 286 || hdist > 30) return -1;

                static const uint8_t order[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
                };
                uint8_t cl[19];
                memset(cl, 0, sizeof(cl));
                for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)in_get(&b, 3);
                if (b.err) return -1;

                InHuff ch;
                if (in_huff_build(&ch, cl, 19)) return -1;

                uint8_t lens[320];
                memset(lens, 0, sizeof(lens));
                int idx = 0, total = hlit + hdist;
                while (idx < total) {
                    int sym = in_huff_dec(&b, &ch);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lens[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (idx == 0) return -1;
                        int rep = (int)in_get(&b, 2) + 3;
                        if (idx + rep > total) return -1;
                        uint8_t v = lens[idx - 1];
                        while (rep--) lens[idx++] = v;
                    } else if (sym == 17) {
                        int rep = (int)in_get(&b, 3) + 3;
                        if (idx + rep > total) return -1;
                        while (rep--) lens[idx++] = 0;
                    } else {
                        int rep = (int)in_get(&b, 7) + 11;
                        if (idx + rep > total) return -1;
                        while (rep--) lens[idx++] = 0;
                    }
                    if (b.err) return -1;
                }
                if (in_huff_build(&lit, lens, hlit)) return -1;
                if (in_huff_build(&dist, lens + hlit, hdist)) return -1;
            }
            if (in_codes(&b, &lit, &dist, out, cap, &used)) return -1;
        } else {
            return -1;
        }
    }
    *outlen = used;
    return 0;
}

/* ============================================================
 *  ZIP central directory
 * ============================================================ */
int office_zip_open(OfficeZip *z, const uint8_t *data, size_t len,
                    char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!z || !data) return -1;
    memset(z, 0, sizeof(*z));
    z->data = data;
    z->len = len;

    if (len < 22) {
        snprintf(err, errsz, "not a ZIP archive: %zu byte(s) is too short", len);
        return -1;
    }

    /* The end-of-central-directory record is at the end, after an optional
     * comment of up to 64 KB. Scan back for its signature. */
    size_t eocd = 0;
    int found = 0;
    size_t lo = len > (22u + 65535u) ? len - 22u - 65535u : 0;
    for (size_t p = len - 22u + 1; p-- > lo;) {
        if (rd32(data + p) == 0x06054b50u) { eocd = p; found = 1; break; }
    }
    if (!found) {
        snprintf(err, errsz, "no ZIP end-of-central-directory record in %zu byte(s)", len);
        return -1;
    }

    uint32_t total = rd16(data + eocd + 10);
    uint32_t cd_size = rd32(data + eocd + 12);
    uint32_t cd_off = rd32(data + eocd + 16);

    /* ZIP64: the 16-bit/32-bit fields are saturated and the real values
     * live in the ZIP64 EOCD record, located by a locator just before the
     * classic EOCD. */
    if (total == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        if (eocd >= 20 && rd32(data + eocd - 20) == 0x07064b50u) {
            uint64_t off = rd64(data + eocd - 20 + 8);
            if (off <= len && len - (size_t)off >= 56 &&
                rd32(data + (size_t)off) == 0x06064b50u) {
                uint64_t t = rd64(data + (size_t)off + 32);
                uint64_t s = rd64(data + (size_t)off + 40);
                uint64_t o = rd64(data + (size_t)off + 48);
                if (t > 0x7FFFFFFFu || s > len || o > len) {
                    snprintf(err, errsz, "ZIP64 directory does not fit in %zu byte(s)", len);
                    return -1;
                }
                total = (uint32_t)t;
                cd_size = (uint32_t)s;
                cd_off = (uint32_t)o;
            }
        }
    }

    if ((uint64_t)cd_off + cd_size > len) {
        snprintf(err, errsz, "ZIP central directory points past the end of the file");
        return -1;
    }

    z->total = (int)(total > 0x7FFFFFFFu ? 0x7FFFFFFF : total);
    if (total > OFFICE_ZIP_MAX_ENTRIES) z->truncated = 1;

    size_t p = cd_off;
    size_t cd_end = (size_t)cd_off + cd_size;
    uint32_t keep = total > OFFICE_ZIP_MAX_ENTRIES ? OFFICE_ZIP_MAX_ENTRIES : total;

    for (uint32_t i = 0; i < total; i++) {
        if (p + 46 > cd_end || rd32(data + p) != 0x02014b50u) {
            snprintf(err, errsz, "ZIP central directory is corrupt at entry %u", i);
            return -1;
        }
        uint16_t flags = rd16(data + p + 8);
        uint16_t method = rd16(data + p + 10);
        uint32_t crc = rd32(data + p + 16);
        uint32_t csize = rd32(data + p + 20);
        uint32_t usize = rd32(data + p + 24);
        uint16_t nlen = rd16(data + p + 28);
        uint16_t elen = rd16(data + p + 30);
        uint16_t clen = rd16(data + p + 32);
        uint32_t lho = rd32(data + p + 42);

        if (p + 46u + nlen + elen + clen > cd_end) {
            snprintf(err, errsz, "ZIP central directory entry %u overruns the directory", i);
            return -1;
        }

        /* ZIP64 extended sizes, in the order the specification defines. */
        if (usize == 0xFFFFFFFFu || csize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) {
            const uint8_t *ex = data + p + 46 + nlen;
            int ep = 0;
            while (ep + 4 <= elen) {
                uint16_t tag = rd16(ex + ep);
                uint16_t sz = rd16(ex + ep + 2);
                if (ep + 4 + sz > elen) break;
                if (tag == 0x0001) {
                    int q = ep + 4;
                    if (usize == 0xFFFFFFFFu && q + 8 <= ep + 4 + sz) {
                        uint64_t v = rd64(ex + q);
                        if (v > 0xFFFFFFFFu) { snprintf(err, errsz, "ZIP64 entry is too large"); return -1; }
                        usize = (uint32_t)v; q += 8;
                    }
                    if (csize == 0xFFFFFFFFu && q + 8 <= ep + 4 + sz) {
                        uint64_t v = rd64(ex + q);
                        if (v > 0xFFFFFFFFu) { snprintf(err, errsz, "ZIP64 entry is too large"); return -1; }
                        csize = (uint32_t)v; q += 8;
                    }
                    if (lho == 0xFFFFFFFFu && q + 8 <= ep + 4 + sz) {
                        uint64_t v = rd64(ex + q);
                        if (v > 0xFFFFFFFFu) { snprintf(err, errsz, "ZIP64 entry offset is too large"); return -1; }
                        lho = (uint32_t)v;
                    }
                    break;
                }
                ep += 4 + sz;
            }
        }

        if (i < keep) {
            OfficeZipEnt *e = &z->ent[z->count];

            if (nlen >= sizeof(e->name)) {
                /* Not one of the names an Office package uses; skip it
                 * rather than truncate a name into a false match. */
                z->truncated = 1;
                p += 46u + nlen + elen + clen;
                continue;
            }
            if ((size_t)lho + 30 > len || rd32(data + lho) != 0x04034b50u) {
                snprintf(err, errsz, "ZIP local header of '%.*s' is corrupt",
                         (int)nlen, (const char *)data + p + 46);
                return -1;
            }
            uint16_t lnlen = rd16(data + lho + 26);
            uint16_t lelen = rd16(data + lho + 28);
            size_t doff = (size_t)lho + 30u + lnlen + lelen;
            if (doff > len || (uint64_t)doff + csize > len) {
                snprintf(err, errsz, "ZIP entry '%.*s' runs past the end of the file",
                         (int)nlen, (const char *)data + p + 46);
                return -1;
            }

            memcpy(e->name, data + p + 46, nlen);
            e->name[nlen] = 0;
            e->crc = crc;
            e->method = method;
            e->csize = csize;
            e->usize = usize;
            e->data_off = (uint32_t)doff;
            e->encrypted = (uint8_t)((flags & 1) ? 1 : 0);
            z->count++;
        }
        p += 46u + nlen + elen + clen;
    }
    return 0;
}

int office_zip_find(const OfficeZip *z, const char *name) {
    if (!z || !name) return -1;
    for (int i = 0; i < z->count; i++)
        if (!strcmp(z->ent[i].name, name)) return i;
    return -1;
}

int office_zip_read(const OfficeZip *z, int i, uint8_t **out, size_t *outlen,
                    char *err, size_t errsz) {
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (!z || i < 0 || i >= z->count) {
        snprintf(err, errsz, "no such ZIP entry");
        return -1;
    }
    const OfficeZipEnt *e = &z->ent[i];
    if (e->encrypted) {
        snprintf(err, errsz, "ZIP entry '%s' is encrypted", e->name);
        return -1;
    }
    if (e->usize > 256u * 1024u * 1024u) {
        snprintf(err, errsz, "ZIP entry '%s' would be %u bytes", e->name, e->usize);
        return -1;
    }

    uint8_t *buf = malloc((size_t)e->usize + 1);
    if (!buf) {
        snprintf(err, errsz, "out of memory for %u bytes of '%s'", e->usize, e->name);
        return -1;
    }
    const uint8_t *src = z->data + e->data_off;
    size_t made = 0;

    if (e->method == 0) {
        if (e->csize != e->usize) {
            snprintf(err, errsz, "'%s' is stored but its sizes disagree", e->name);
            free(buf);
            return -1;
        }
        if (e->csize) memcpy(buf, src, e->csize);
        made = e->csize;
    } else if (e->method == 8) {
        if (inflate_raw(src, e->csize, buf, e->usize, &made) != 0 || made != e->usize) {
            snprintf(err, errsz, "'%s' is not a valid raw deflate stream (%zu of %u bytes)",
                     e->name, made, e->usize);
            free(buf);
            return -1;
        }
    } else {
        snprintf(err, errsz, "'%s' uses compression method %u, only stored (0) and deflate (8) are supported",
                 e->name, e->method);
        free(buf);
        return -1;
    }
    buf[made] = 0;
    uint32_t got_crc = crc_calc(buf, made);
    if (got_crc != e->crc) {
        snprintf(err, errsz, "'%s' fails its CRC-32 check (%08x, expected %08x)",
                 e->name, got_crc, e->crc);
        free(buf);
        return -1;
    }
    *out = buf;
    if (outlen) *outlen = made;
    return 0;
}

/* ============================================================
 *  Tiny XML tokeniser
 *
 *  Office XML is namespace-heavy but the names in the parts we read
 *  are unambiguous by local name (w:p / a:p / text:p all mean a
 *  paragraph; w:t / a:t mean text). So the tokeniser reports the
 *  local name and keeps the prefix as well, which is enough for the
 *  extractors and avoids a namespace resolver.
 * ============================================================ */
enum { XT_NONE = 0, XT_START, XT_END, XT_TEXT };

typedef struct {
    char name[48];
    char val[192];
} XmlAttr;

typedef struct {
    int  kind;
    char name[48];
    char prefix[16];
    char text[4096];
    int  tlen;
    int  more;          /* text continues on the next call */
    int  selfclose;
    int  nattr;
    XmlAttr attr[10];
} XmlTok;

typedef struct {
    const char *s;
    size_t n, i;
    size_t cd_end;      /* inside <![CDATA[..]]>, absolute end offset */
    int failed;
} XmlReader;

static void xml_init(XmlReader *x, const uint8_t *data, size_t len) {
    memset(x, 0, sizeof(*x));
    x->s = (const char *)data;
    x->n = len;
}

static void local_name(const char *p, size_t n, char *out, size_t cap) {
    size_t c = 0;
    while (c < n && p[c] != ':') c++;
    const char *body = (c < n) ? p + c + 1 : p;
    size_t blen = (c < n) ? n - c - 1 : n;
    if (blen >= cap) blen = cap - 1;
    memcpy(out, body, blen);
    out[blen] = 0;
}

static void name_prefix(const char *p, size_t n, char *out, size_t cap) {
    size_t c = 0;
    while (c < n && p[c] != ':') c++;
    if (c >= n || c >= cap) { out[0] = 0; return; }
    memcpy(out, p, c);
    out[c] = 0;
}

static int utf8_put(char *out, int cap, unsigned cp) {
    if (cp < 0x80) {
        if (cap < 1) return 0;
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one entity at src (which points at '&'); advances *used. */
static int ent_decode(const char *src, size_t avail, char *out, int cap, size_t *used) {
    *used = 0;
    if (avail < 3 || src[0] != '&') return 0;
    size_t semi = 1;
    while (semi < avail && semi < 12 && src[semi] != ';') semi++;
    if (semi >= avail || src[semi] != ';') return 0;
    const char *nm = src + 1;
    size_t nl = semi - 1;
    unsigned cp = 0;
    int known = 1;

    if (nl == 3 && !memcmp(nm, "amp", 3)) cp = '&';
    else if (nl == 2 && !memcmp(nm, "lt", 2)) cp = '<';
    else if (nl == 2 && !memcmp(nm, "gt", 2)) cp = '>';
    else if (nl == 4 && !memcmp(nm, "quot", 4)) cp = '"';
    else if (nl == 4 && !memcmp(nm, "apos", 4)) cp = '\'';
    else if (nl == 4 && !memcmp(nm, "nbsp", 4)) cp = 0xA0;
    else if (nl >= 2 && nm[0] == '#') {
        if (nm[1] == 'x' || nm[1] == 'X') {
            char tmp[10];
            size_t k = 2, t = 0;
            while (k < nl && t + 1 < sizeof(tmp)) tmp[t++] = nm[k++];
            tmp[t] = 0;
            char *end = NULL;
            cp = (unsigned)strtoul(tmp, &end, 16);
            if (!end || *end) known = 0;
        } else {
            char tmp[10];
            size_t k = 1, t = 0;
            while (k < nl && t + 1 < sizeof(tmp)) tmp[t++] = nm[k++];
            tmp[t] = 0;
            char *end = NULL;
            cp = (unsigned)strtoul(tmp, &end, 10);
            if (!end || *end) known = 0;
        }
    } else {
        known = 0;
    }

    if (!known) {
        /* Unknown name: keep the entity as it stood. This never invents
         * characters, and one odd entity cannot swallow the rest. */
        if ((int)(semi + 1) > cap) return 0;
        memcpy(out, src, semi + 1);
        *used = semi + 1;
        return (int)(semi + 1);
    }
    int m = utf8_put(out, cap, cp);
    if (!m) return 0;
    *used = semi + 1;
    return m;
}

static int decode_entities(const char *src, size_t n, char *out, size_t cap) {
    size_t o = 0, i = 0;
    while (i < n && o + 8 < cap) {
        if (src[i] == '&') {
            size_t used = 0;
            int m = ent_decode(src + i, n - i, out + o, (int)(cap - o), &used);
            if (m > 0 && used > 0) { o += (size_t)m; i += used; continue; }
        }
        out[o++] = src[i++];
    }
    out[o] = 0;
    return (int)o;
}

static int xml_text_token(XmlReader *x, XmlTok *t) {
    t->kind = XT_TEXT;
    t->tlen = 0;
    t->more = 0;
    while (x->i < x->n) {
        if (x->cd_end > x->i) {
            while (x->i < x->cd_end && t->tlen + 5 < (int)sizeof(t->text))
                t->text[t->tlen++] = x->s[x->i++];
            if (x->i < x->cd_end) { t->more = 1; t->text[t->tlen] = 0; return 1; }
            x->i = x->cd_end + 3;
            x->cd_end = 0;
            continue;
        }
        if (x->s[x->i] == '<') break;
        if (t->tlen + 16 > (int)sizeof(t->text)) { t->more = 1; break; }
        if (x->s[x->i] == '&') {
            size_t used = 0;
            int m = ent_decode(x->s + x->i, x->n - x->i,
                               t->text + t->tlen, (int)sizeof(t->text) - t->tlen, &used);
            if (m > 0 && used > 0) { t->tlen += m; x->i += used; continue; }
        }
        t->text[t->tlen++] = x->s[x->i++];
    }
    t->text[t->tlen] = 0;
    if (x->i < x->n && x->cd_end == 0 && x->s[x->i] != '<') t->more = 1;
    return 1;
}

static int xml_next(XmlReader *x, XmlTok *t) {
    memset(t, 0, sizeof(*t));
    for (;;) {
        if (x->failed || x->i >= x->n) return 0;
        if (x->cd_end > x->i) return xml_text_token(x, t);
        if (x->s[x->i] != '<') return xml_text_token(x, t);

        if (x->n - x->i >= 4 && !memcmp(x->s + x->i, "<!--", 4)) {
            size_t p = x->i + 4;
            while (p + 3 <= x->n && memcmp(x->s + p, "-->", 3)) p++;
            if (p + 3 > x->n) { x->failed = 1; return 0; }
            x->i = p + 3;
            continue;
        }
        if (x->n - x->i >= 9 && !memcmp(x->s + x->i, "<![CDATA[", 9)) {
            size_t p = x->i + 9;
            while (p + 3 <= x->n && memcmp(x->s + p, "]]>", 3)) p++;
            if (p + 3 > x->n) { x->failed = 1; return 0; }
            x->cd_end = p;
            x->i = p;
            continue;
        }
        if (x->n - x->i >= 2 && x->s[x->i + 1] == '?') {
            size_t p = x->i + 2;
            while (p + 2 <= x->n && memcmp(x->s + p, "?>", 2)) p++;
            if (p + 2 > x->n) { x->failed = 1; return 0; }
            x->i = p + 2;
            continue;
        }
        if (x->n - x->i >= 2 && x->s[x->i + 1] == '!') {
            size_t p = x->i + 2;
            while (p < x->n && x->s[p] != '>') p++;
            if (p >= x->n) { x->failed = 1; return 0; }
            x->i = p + 1;
            continue;
        }

        if (x->n - x->i >= 2 && x->s[x->i + 1] == '/') {
            x->i += 2;
            size_t s = x->i;
            while (x->i < x->n && !isspace((unsigned char)x->s[x->i]) && x->s[x->i] != '>')
                x->i++;
            if (x->i >= x->n) { x->failed = 1; return 0; }
            size_t nl = x->i - s;
            local_name(x->s + s, nl, t->name, sizeof(t->name));
            name_prefix(x->s + s, nl, t->prefix, sizeof(t->prefix));
            while (x->i < x->n && x->s[x->i] != '>') x->i++;
            if (x->i >= x->n) { x->failed = 1; return 0; }
            x->i++;
            t->kind = XT_END;
            return 1;
        }

        /* start tag */
        x->i++;
        size_t s = x->i;
        while (x->i < x->n && !isspace((unsigned char)x->s[x->i]) &&
               x->s[x->i] != '/' && x->s[x->i] != '>')
            x->i++;
        if (x->i >= x->n) { x->failed = 1; return 0; }
        size_t nl = x->i - s;
        if (nl == 0) { x->failed = 1; return 0; }
        local_name(x->s + s, nl, t->name, sizeof(t->name));
        name_prefix(x->s + s, nl, t->prefix, sizeof(t->prefix));
        t->kind = XT_START;

        for (;;) {
            while (x->i < x->n && isspace((unsigned char)x->s[x->i])) x->i++;
            if (x->i >= x->n) { x->failed = 1; return 0; }
            if (x->s[x->i] == '>') { x->i++; break; }
            if (x->s[x->i] == '/') {
                if (x->i + 1 < x->n && x->s[x->i + 1] == '>') {
                    x->i += 2;
                    t->selfclose = 1;
                    break;
                }
                x->i++;
                continue;
            }
            size_t as = x->i;
            while (x->i < x->n && !isspace((unsigned char)x->s[x->i]) &&
                   x->s[x->i] != '=' && x->s[x->i] != '>' && x->s[x->i] != '/')
                x->i++;
            size_t anl = x->i - as;
            char aname[48];
            local_name(x->s + as, anl, aname, sizeof(aname));
            while (x->i < x->n && isspace((unsigned char)x->s[x->i])) x->i++;
            char aval[192];
            aval[0] = 0;
            if (x->i < x->n && x->s[x->i] == '=') {
                x->i++;
                while (x->i < x->n && isspace((unsigned char)x->s[x->i])) x->i++;
                if (x->i < x->n && (x->s[x->i] == '"' || x->s[x->i] == '\'')) {
                    char q = x->s[x->i++];
                    size_t vs = x->i;
                    while (x->i < x->n && x->s[x->i] != q) x->i++;
                    if (x->i >= x->n) { x->failed = 1; return 0; }
                    decode_entities(x->s + vs, x->i - vs, aval, sizeof(aval));
                    x->i++;
                } else {
                    size_t vs = x->i;
                    while (x->i < x->n && !isspace((unsigned char)x->s[x->i]) &&
                           x->s[x->i] != '>')
                        x->i++;
                    decode_entities(x->s + vs, x->i - vs, aval, sizeof(aval));
                }
            }
            if (t->nattr < (int)(sizeof(t->attr) / sizeof(t->attr[0]))) {
                snprintf(t->attr[t->nattr].name, sizeof(t->attr[t->nattr].name), "%s", aname);
                snprintf(t->attr[t->nattr].val, sizeof(t->attr[t->nattr].val), "%s", aval);
                t->nattr++;
            }
        }
        return 1;
    }
}

static const char *xattr(const XmlTok *t, const char *name) {
    for (int i = 0; i < t->nattr; i++)
        if (!strcmp(t->attr[i].name, name)) return t->attr[i].val;
    return NULL;
}

/* ============================================================
 *  Growable byte/string buffer
 * ============================================================ */
typedef struct {
    char *p;
    size_t n, cap;
} StrBuf;

static int sb_reserve(StrBuf *b, size_t need) {
    if (need + 1 <= b->cap) return 0;
    if (need + 1 > OFFICE_PLAIN_CAP) return -1;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < need + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int sb_add(StrBuf *b, const char *s, size_t n) {
    if (sb_reserve(b, b->n + n)) return -1;
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}

/* Removes ASCII space/tab/newline from both ends. Returns how many bytes
 * were removed from the front, which is what keeps run offsets honest. */
static size_t sb_trim(StrBuf *b) {
    if (!b->p || b->n == 0) return 0;
    size_t s = 0, e = b->n;
    while (s < e && (b->p[s] == ' ' || b->p[s] == '\t' || b->p[s] == '\r' || b->p[s] == '\n')) s++;
    while (e > s && (b->p[e - 1] == ' ' || b->p[e - 1] == '\t' ||
                     b->p[e - 1] == '\r' || b->p[e - 1] == '\n')) e--;
    if (s) memmove(b->p, b->p + s, e - s);
    b->n = e - s;
    b->p[b->n] = 0;
    return s;
}

static void sb_free(StrBuf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ============================================================
 *  Document arena, blocks, sheets
 * ============================================================ */
static int doc_append(OfficeDoc *doc, const char *s, size_t n, OfficeRef *out) {
    if (!out) return -1;
    out->off = 0;
    out->len = 0;
    if (n == 0) return 0;
    if (n > 0xFFFFFFFFu || doc->text_len + n + 1 > OFFICE_ARENA_CAP) {
        doc->truncated = 1;
        return -1;
    }
    if (doc->text_len + n + 1 > doc->text_cap) {
        size_t cap = doc->text_cap ? doc->text_cap : 4096;
        while (cap < doc->text_len + n + 1) cap *= 2;
        if (cap > OFFICE_ARENA_CAP) cap = OFFICE_ARENA_CAP;
        char *p = realloc(doc->text, cap);
        if (!p) { doc->truncated = 1; return -1; }
        doc->text = p;
        doc->text_cap = cap;
    }
    memcpy(doc->text + doc->text_len, s, n);
    doc->text[doc->text_len + n] = 0;
    out->off = (uint32_t)doc->text_len;
    out->len = (uint32_t)n;
    doc->text_len += n + 1;
    return 0;
}

static OfficeBlock *doc_add_block(OfficeDoc *doc, int kind) {
    if (doc->nblocks >= OFFICE_MAX_BLOCKS) {
        doc->truncated = 1;
        return NULL;
    }
    if (doc->nblocks == doc->blocks_cap) {
        int cap = doc->blocks_cap ? doc->blocks_cap * 2 : 16;
        if (cap > OFFICE_MAX_BLOCKS) cap = OFFICE_MAX_BLOCKS;
        OfficeBlock *p = realloc(doc->blocks, (size_t)cap * sizeof(*p));
        if (!p) { doc->truncated = 1; return NULL; }
        doc->blocks = p;
        doc->blocks_cap = cap;
    }
    OfficeBlock *b = &doc->blocks[doc->nblocks++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    return b;
}

static OfficeBlock *doc_emit_para(OfficeDoc *doc, int style, int bold, int italic,
                                  int level, const char *s, size_t n) {
    if (n == 0) return NULL;
    OfficeBlock *b = doc_add_block(doc, OFFICE_BLOCK_PARA);
    if (!b) return NULL;
    if (doc_append(doc, s, n, &b->para)) {
        /* keep the block, with no text: the table around it is still real */
        b->para.off = 0;
        b->para.len = 0;
    }
    b->style = style;
    b->bold = bold;
    b->italic = italic;
    b->level = level;
    return b;
}

static void doc_add_sheet(OfficeDoc *doc, const char *name, int drawn) {
    if (!name || !name[0]) return;
    if (doc->nsheets >= OFFICE_MAX_SHEETS) { doc->truncated = 1; return; }
    if (doc->nsheets == doc->sheets_cap) {
        int cap = doc->sheets_cap ? doc->sheets_cap * 2 : 4;
        if (cap > OFFICE_MAX_SHEETS) cap = OFFICE_MAX_SHEETS;
        OfficeSheet *p = realloc(doc->sheets, (size_t)cap * sizeof(*p));
        if (!p) { doc->truncated = 1; return; }
        doc->sheets = p;
        doc->sheets_cap = cap;
    }
    OfficeSheet *s = &doc->sheets[doc->nsheets++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->drawn = drawn;
}

const char *office_block_text(const OfficeDoc *doc, const OfficeBlock *b) {
    if (!doc || !b || b->kind != OFFICE_BLOCK_PARA || b->para.len == 0 || !doc->text)
        return "";
    return doc->text + b->para.off;
}

const char *office_cell_text(const OfficeDoc *doc, const OfficeBlock *b,
                             int row, int col) {
    if (!doc || !b || b->kind != OFFICE_BLOCK_TABLE) return "";
    if (row < 0 || row >= b->rows || col < 0 || col >= b->cols || !b->cell)
        return "";
    const OfficeRef *r = &b->cell[(size_t)row * (size_t)b->cols + (size_t)col];
    if (r->len == 0 || !doc->text) return "";
    return doc->text + r->off;
}

/* ============================================================
 *  Flow: paragraphs and tables, shared by every family
 *
 *  The extractors feed starts/ends/text into these calls. A table
 *  cell accumulates the paragraphs inside it and is committed when
 *  the cell closes, so rows and columns are exactly the file's.
 * ============================================================ */
typedef struct {
    OfficeDoc *doc;
    StrBuf cur;
    int para_open, para_depth;
    int style, bold, italic, level;
    OfficeRun *runs;
    int nruns, runs_cap;
    int skip;
    int oom, halt;

    int tbl_depth;
    int in_cell;
    StrBuf cell;
    OfficeRef *row_cells;
    unsigned char *row_bold;
    int row_n, row_cap;
    int row_bold_cap;
    int cell_emph;
    OfficeRef **rows;
    unsigned char **rows_bold;
    int *row_lens;
    int rows_n, rows_cap;
    int max_cols;
    int truncated;
} Flow;

static void flow_init(Flow *f, OfficeDoc *doc) {
    memset(f, 0, sizeof(*f));
    f->doc = doc;
}

static void flow_free(Flow *f) {
    sb_free(&f->cur);
    sb_free(&f->cell);
    free(f->runs);
    free(f->row_cells);
    free(f->row_bold);
    if (f->rows) {
        for (int i = 0; i < f->rows_n; i++) free(f->rows[i]);
        free(f->rows);
    }
    if (f->rows_bold) {
        for (int i = 0; i < f->rows_n; i++) free(f->rows_bold[i]);
        free(f->rows_bold);
    }
    free(f->row_lens);
    memset(f, 0, sizeof(*f));
}

/* Records which bytes of the current paragraph carry which emphasis. */
static void flow_note_run(Flow *f, uint32_t start, uint32_t n) {
    if (n == 0) return;
    unsigned char bold = (unsigned char)(f->bold ? 1 : 0);
    unsigned char ital = (unsigned char)(f->italic ? 1 : 0);

    if (f->nruns > 0) {
        OfficeRun *r = &f->runs[f->nruns - 1];
        if (r->off + r->len == start && r->bold == bold && r->italic == ital) {
            r->len += n;
            return;
        }
    }
    if (f->nruns >= 4096) {
        /* Keep the text; the style of the tail is merged into the last run
         * rather than lost. The cap is reported through doc->truncated. */
        f->runs[f->nruns - 1].len += n;
        f->doc->truncated = 1;
        return;
    }
    if (f->nruns == f->runs_cap) {
        int cap = f->runs_cap ? f->runs_cap * 2 : 16;
        if (cap > 4096) cap = 4096;
        OfficeRun *p = realloc(f->runs, (size_t)cap * sizeof(*p));
        if (!p) { f->oom = 1; f->halt = 1; return; }
        f->runs = p;
        f->runs_cap = cap;
    }
    OfficeRun *r = &f->runs[f->nruns++];
    r->off = start;
    r->len = n;
    r->bold = bold;
    r->italic = ital;
}

static void flow_text(Flow *f, const char *s, int n) {
    if (f->skip || f->halt || !f->para_open || n <= 0) return;
    uint32_t start = (uint32_t)f->cur.n;
    if (sb_add(&f->cur, s, (size_t)n)) { f->oom = 1; f->halt = 1; return; }
    flow_note_run(f, start, (uint32_t)n);
}

static void flow_para_open(Flow *f, int style) {
    if (f->halt) return;
    if (f->para_depth++ == 0) {
        f->para_open = 1;
        f->cur.n = 0;
        if (f->cur.p) f->cur.p[0] = 0;
        f->style = style;
        f->bold = f->italic = 0;
        f->level = 0;
        f->nruns = 0;
    }
}

static void flow_emit_para(Flow *f) {
    size_t lead = sb_trim(&f->cur);
    if (f->cur.n == 0) { f->nruns = 0; return; }

    if (f->in_cell) {
        if (f->bold || f->italic) f->cell_emph = 1;
        if (f->cell.n) {
            if (sb_add(&f->cell, " ", 1)) { f->oom = 1; f->halt = 1; return; }
        }
        if (sb_add(&f->cell, f->cur.p, f->cur.n)) { f->oom = 1; f->halt = 1; }
        f->nruns = 0;
        return;
    }

    int all_bold = f->nruns > 0, all_ital = f->nruns > 0;
    for (int i = 0; i < f->nruns; i++) {
        if (!f->runs[i].bold) all_bold = 0;
        if (!f->runs[i].italic) all_ital = 0;
    }
    OfficeBlock *b = doc_emit_para(f->doc, f->style, all_bold, all_ital, f->level,
                                   f->cur.p, f->cur.n);
    if (!b) { f->halt = 1; f->nruns = 0; return; }

    if (f->nruns > 0) {
        OfficeRun *copy = malloc((size_t)f->nruns * sizeof(*copy));
        if (copy) {
            uint32_t lo = (uint32_t)lead;
            uint32_t hi = lo + (uint32_t)f->cur.n;
            int n = 0;
            for (int i = 0; i < f->nruns; i++) {
                uint32_t rs = f->runs[i].off, re = rs + f->runs[i].len;
                if (re <= lo || rs >= hi) continue;
                if (rs < lo) rs = lo;
                if (re > hi) re = hi;
                copy[n].off = b->para.off + (rs - lo);
                copy[n].len = re - rs;
                copy[n].bold = f->runs[i].bold;
                copy[n].italic = f->runs[i].italic;
                n++;
            }
            b->runs = copy;
            b->nruns = n;
        } else {
            f->doc->truncated = 1;
        }
    }
    f->nruns = 0;
}

static void flow_para_close(Flow *f) {
    if (f->halt) return;
    if (f->para_depth <= 0) {
        f->para_depth = 0;
        return;
    }
    f->para_depth--;
    if (f->para_depth == 0) {
        flow_emit_para(f);
        f->para_open = 0;
    }
}

/* ---- table accumulation ---- */
static int flow_cell_push(Flow *f, OfficeRef ref, int emph) {
    if (f->row_n >= OFFICE_MAX_COLS) { f->truncated = 1; return 0; }
    if (f->row_n == f->row_cap) {
        int cap = f->row_cap ? f->row_cap * 2 : 8;
        if (cap > OFFICE_MAX_COLS) cap = OFFICE_MAX_COLS;
        OfficeRef *p = realloc(f->row_cells, (size_t)cap * sizeof(*p));
        if (!p) { f->oom = 1; f->halt = 1; return -1; }
        f->row_cells = p;
        f->row_cap = cap;
    }
    if (f->row_n == f->row_bold_cap) {
        int cap = f->row_bold_cap ? f->row_bold_cap * 2 : 8;
        if (cap > OFFICE_MAX_COLS) cap = OFFICE_MAX_COLS;
        unsigned char *bp = realloc(f->row_bold, (size_t)cap);
        if (!bp) { f->oom = 1; f->halt = 1; return -1; }
        f->row_bold = bp;
        f->row_bold_cap = cap;
    }
    f->row_cells[f->row_n] = ref;
    f->row_bold[f->row_n] = (unsigned char)(emph ? 1 : 0);
    f->row_n++;
    return 0;
}

static void flow_table_open(Flow *f) {
    if (f->halt) return;
    f->tbl_depth++;
    if (f->tbl_depth == 1) {
        for (int i = 0; i < f->rows_n; i++) {
            free(f->rows[i]);
            free(f->rows_bold[i]);
        }
        f->rows_n = 0;
        f->max_cols = 0;
        f->truncated = 0;
        f->row_n = 0;
        f->in_cell = 0;
    }
}

static void flow_row_move(Flow *f) {
    if (f->rows_n <= 0) return;
    f->rows[f->rows_n - 1] = f->row_cells;
    f->rows_bold[f->rows_n - 1] = f->row_bold;
    f->row_lens[f->rows_n - 1] = f->row_n;
    if (f->row_n > f->max_cols) f->max_cols = f->row_n;
    f->row_cells = NULL;
    f->row_bold = NULL;
    f->row_n = 0;
    f->row_cap = 0;
    f->row_bold_cap = 0;
}

static void flow_row_open(Flow *f) {
    if (f->halt || f->tbl_depth != 1) return;
    /* A row that was never closed still holds its cells. */
    if (f->rows_n > 0 && f->rows[f->rows_n - 1] == NULL) flow_row_move(f);

    if (f->rows_n >= OFFICE_MAX_ROWS) { f->truncated = 1; f->halt = 1; return; }
    if (f->rows_n == f->rows_cap) {
        int cap = f->rows_cap ? f->rows_cap * 2 : 16;
        if (cap > OFFICE_MAX_ROWS) cap = OFFICE_MAX_ROWS;
        OfficeRef **rp = realloc(f->rows, (size_t)cap * sizeof(*rp));
        if (!rp) { f->oom = 1; f->halt = 1; return; }
        f->rows = rp;
        int *lp = realloc(f->row_lens, (size_t)cap * sizeof(*lp));
        if (!lp) { f->oom = 1; f->halt = 1; return; }
        f->row_lens = lp;
        unsigned char **bp = realloc(f->rows_bold, (size_t)cap * sizeof(*bp));
        if (!bp) { f->oom = 1; f->halt = 1; return; }
        f->rows_bold = bp;
        f->rows_cap = cap;
    }
    f->rows[f->rows_n] = NULL;
    f->rows_bold[f->rows_n] = NULL;
    f->row_lens[f->rows_n] = 0;
    f->rows_n++;
    f->row_n = 0;
}

static void flow_row_repeat(Flow *f, int extra) {
    if (f->halt || f->tbl_depth != 1 || f->rows_n <= 0) return;
    while (extra-- > 0) {
        if (f->rows_n >= OFFICE_MAX_ROWS) { f->truncated = 1; return; }
        if (f->rows_n == f->rows_cap) {
            int cap = f->rows_cap ? f->rows_cap * 2 : 16;
            if (cap > OFFICE_MAX_ROWS) cap = OFFICE_MAX_ROWS;
            OfficeRef **rp = realloc(f->rows, (size_t)cap * sizeof(*rp));
            if (!rp) { f->oom = 1; f->halt = 1; return; }
            f->rows = rp;
            int *lp = realloc(f->row_lens, (size_t)cap * sizeof(*lp));
            if (!lp) { f->oom = 1; f->halt = 1; return; }
            f->row_lens = lp;
            unsigned char **bp = realloc(f->rows_bold, (size_t)cap * sizeof(*bp));
            if (!bp) { f->oom = 1; f->halt = 1; return; }
            f->rows_bold = bp;
            f->rows_cap = cap;
        }
        int src = f->rows_n - 1;
        int n = f->row_lens[src];
        OfficeRef *cp = NULL;
        unsigned char *cb = NULL;
        if (n > 0) {
            cp = malloc((size_t)n * sizeof(*cp));
            cb = malloc((size_t)n);
            if (!cp || !cb) { free(cp); free(cb); f->oom = 1; f->halt = 1; return; }
            memcpy(cp, f->rows[src], (size_t)n * sizeof(*cp));
            memcpy(cb, f->rows_bold[src], (size_t)n);
        }
        f->rows[f->rows_n] = cp;
        f->rows_bold[f->rows_n] = cb;
        f->row_lens[f->rows_n] = n;
        f->rows_n++;
        if (n > f->max_cols) f->max_cols = n;
    }
}

static void flow_row_close(Flow *f) {
    if (f->tbl_depth != 1) return;
    if (f->in_cell) {
        /* tolerate a missing </tc> */
        OfficeRef ref = {0, 0};
        sb_trim(&f->cell);
        doc_append(f->doc, f->cell.p ? f->cell.p : "", f->cell.n, &ref);
        flow_cell_push(f, ref, f->cell_emph);
        f->in_cell = 0;
        f->cell.n = 0;
        f->cell_emph = 0;
    }
    if (f->rows_n > 0 && f->rows[f->rows_n - 1] == NULL) flow_row_move(f);
}

static void flow_cell_open(Flow *f) {
    if (f->halt || f->tbl_depth != 1) return;
    if (!f->in_cell) {
        f->in_cell = 1;
        f->cell.n = 0;
        f->cell_emph = 0;
        if (f->cell.p) f->cell.p[0] = 0;
    }
}

static void flow_cell_close(Flow *f) {
    if (f->halt || f->tbl_depth != 1 || !f->in_cell) return;
    OfficeRef ref = {0, 0};
    sb_trim(&f->cell);
    doc_append(f->doc, f->cell.p ? f->cell.p : "", f->cell.n, &ref);
    flow_cell_push(f, ref, f->cell_emph);
    f->in_cell = 0;
    f->cell.n = 0;
    f->cell_emph = 0;
}

static void flow_cell_repeat(Flow *f, int extra) {
    if (f->halt || f->tbl_depth != 1 || f->row_n <= 0) return;
    while (extra-- > 0) {
        if (flow_cell_push(f, f->row_cells[f->row_n - 1],
                           f->row_bold ? f->row_bold[f->row_n - 1] : 0)) return;
    }
}

static void flow_emit_table(Flow *f) {
    if (f->rows_n == 0) return;
    int cols = f->max_cols;
    if (cols > OFFICE_MAX_COLS) { cols = OFFICE_MAX_COLS; f->truncated = 1; }
    if (cols <= 0) {
        for (int i = 0; i < f->rows_n; i++) free(f->rows[i]);
        f->rows_n = 0;
        return;
    }

    OfficeBlock *b = doc_add_block(f->doc, OFFICE_BLOCK_TABLE);
    if (!b) { f->halt = 1; return; }
    b->rows = f->rows_n;
    b->cols = cols;
    b->cell = calloc((size_t)b->rows * (size_t)cols, sizeof(*b->cell));
    b->cell_bold = calloc((size_t)b->rows * (size_t)cols, 1);
    if (!b->cell || !b->cell_bold) {
        free(b->cell);
        free(b->cell_bold);
        b->cell = NULL;
        b->cell_bold = NULL;
        b->rows = b->cols = 0;
        f->oom = 1;
        f->halt = 1;
        return;
    }
    for (int r = 0; r < b->rows; r++) {
        int n = f->row_lens[r];
        if (n > cols) { n = cols; f->truncated = 1; }
        for (int c = 0; c < n; c++) {
            b->cell[(size_t)r * (size_t)cols + (size_t)c] = f->rows[r][c];
            if (f->rows_bold[r])
                b->cell_bold[(size_t)r * (size_t)cols + (size_t)c] = f->rows_bold[r][c];
        }
    }
    b->truncated = f->truncated;

    for (int i = 0; i < f->rows_n; i++) {
        free(f->rows[i]);
        free(f->rows_bold[i]);
    }
    f->rows_n = 0;
    f->max_cols = 0;
    f->truncated = 0;
}

static void flow_table_close(Flow *f) {
    if (f->halt || f->tbl_depth <= 0) return;
    if (f->tbl_depth == 1) {
        flow_row_close(f);
        flow_emit_table(f);
    }
    f->tbl_depth--;
}

/* ============================================================
 *  ODF style map (bold/italic by style name)
 * ============================================================ */
typedef struct {
    char name[64];
    unsigned char bold, italic;
} StyleEnt;

typedef struct {
    StyleEnt e[OFFICE_MAX_STYLES];
    int n;
    int overflow;
} StyleMap;

static void style_map_scan(StyleMap *m, const uint8_t *xml, size_t len) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    char cur[64] = {0};
    int depth = 0, have = 0;

    while (xml_next(&x, &t)) {
        if (t.kind == XT_START) {
            if (!strcmp(t.name, "style")) {
                if (depth == 0) {
                    const char *nm = xattr(&t, "name");
                    const char *fam = xattr(&t, "family");
                    have = 0;
                    if (nm && *nm &&
                        (!fam || !strcmp(fam, "text") || !strcmp(fam, "paragraph"))) {
                        snprintf(cur, sizeof(cur), "%s", nm);
                        have = 1;
                    }
                }
                if (!t.selfclose) depth++;
            } else if (have && !strcmp(t.name, "text-properties")) {
                const char *w = xattr(&t, "font-weight");
                const char *s = xattr(&t, "font-style");
                int bold = 0, ital = 0;
                if (w) {
                    if (!strcmp(w, "bold") || !strcmp(w, "bolder")) bold = 1;
                    else {
                        int v = atoi(w);
                        if (v >= 600) bold = 1;
                    }
                }
                if (s && (!strcmp(s, "italic") || !strcmp(s, "oblique"))) ital = 1;
                if (bold || ital) {
                    int slot = -1;
                    for (int i = 0; i < m->n; i++)
                        if (!strcmp(m->e[i].name, cur)) { slot = i; break; }
                    if (slot < 0) {
                        if (m->n < OFFICE_MAX_STYLES) {
                            slot = m->n++;
                            snprintf(m->e[slot].name, sizeof(m->e[slot].name), "%s", cur);
                        } else {
                            m->overflow = 1;
                        }
                    }
                    if (slot >= 0) {
                        if (bold) m->e[slot].bold = 1;
                        if (ital) m->e[slot].italic = 1;
                    }
                }
            }
        } else if (t.kind == XT_END && !strcmp(t.name, "style")) {
            if (depth > 0) depth--;
            if (depth == 0) { have = 0; cur[0] = 0; }
        }
    }
}

static void style_map_apply(const StyleMap *m, const char *name, int *bold, int *italic) {
    if (!m || !name) return;
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->e[i].name, name)) continue;
        if (m->e[i].bold) *bold = 1;
        if (m->e[i].italic) *italic = 1;
        return;
    }
}

/* ============================================================
 *  Helpers shared by the extractors
 * ============================================================ */
static int attr_is_on(const char *v) {
    if (!v) return 1;                      /* <w:b/> means on */
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "none"))
        return 0;
    return 1;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int ci_has(const char *s, const char *needle) {
    size_t nl = strlen(needle);
    if (!s || !nl) return 0;
    for (size_t i = 0; s[i]; i++) {
        size_t k = 0;
        while (k < nl && s[i + k] &&
               tolower((unsigned char)s[i + k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int style_from_name(const char *v) {
    if (!v || !*v) return OFFICE_STYLE_BODY;
    if (ci_has(v, "subtitle")) return OFFICE_STYLE_H2;
    if (ci_has(v, "heading")) {
        const char *p = strstr(v, "eading");
        while (p && *p && !isdigit((unsigned char)*p)) p++;
        int lv = p ? atoi(p) : 0;
        if (lv >= 1 && lv <= 3) return OFFICE_STYLE_H1 + lv - 1;
        return OFFICE_STYLE_H3;
    }
    if (!strcmp(v, "Title") || !strcmp(v, "title")) return OFFICE_STYLE_TITLE;
    if (ci_has(v, "title")) return OFFICE_STYLE_TITLE;
    return OFFICE_STYLE_BODY;
}

/* Copies the text of the first element with this local name. */
static int xml_simple_text(const uint8_t *xml, size_t len, const char *want,
                           char *out, size_t cap) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    int depth = 0;
    size_t used = 0;
    out[0] = 0;
    while (xml_next(&x, &t)) {
        if (t.kind == XT_START) {
            if (depth == 0 && !strcmp(t.name, want) && !t.selfclose) {
                depth = 1;
                continue;
            }
            if (depth && !t.selfclose) depth++;
        } else if (t.kind == XT_TEXT) {
            if (depth && used + (size_t)t.tlen < cap) {
                memcpy(out + used, t.text, (size_t)t.tlen);
                used += (size_t)t.tlen;
                out[used] = 0;
            }
        } else if (t.kind == XT_END) {
            if (depth == 1 && !strcmp(t.name, want)) return 1;
            if (depth) depth--;
        }
    }
    return out[0] ? 1 : 0;
}

static void doc_meta(OfficeDoc *doc, const OfficeZip *z, const char *core_name) {
    int i = office_zip_find(z, core_name);
    if (i < 0) return;
    uint8_t *xml = NULL;
    size_t len = 0;
    char err[160];
    if (office_zip_read(z, i, &xml, &len, err, sizeof(err)) != 0) return;
    char tmp[192];
    if (xml_simple_text(xml, len, "title", tmp, sizeof(tmp))) {
        snprintf(doc->title, sizeof(doc->title), "%s", tmp);
    }
    if (xml_simple_text(xml, len, "initial-creator", tmp, sizeof(tmp)) ||
        xml_simple_text(xml, len, "creator", tmp, sizeof(tmp))) {
        snprintf(doc->author, sizeof(doc->author), "%s", tmp);
    }
    free(xml);
}

/* ============================================================
 *  .docx - word/document.xml
 * ============================================================ */
static void docx_style(Flow *f, const char *val) {
    if (!f->para_depth || f->skip) return;
    int st = style_from_name(val);
    if (st == OFFICE_STYLE_BODY) return;
    /* A list paragraph with a heading style stays a heading. */
    if (f->style == OFFICE_STYLE_BULLET && st == OFFICE_STYLE_BODY) return;
    f->style = st;
}

static int parse_docx(OfficeDoc *doc, const uint8_t *xml, size_t len,
                      char *err, size_t errsz) {
    Flow f;
    flow_init(&f, doc);
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    int ppr = 0, run = 0, in_t = 0;

    while (xml_next(&x, &t)) {
        if (f.halt) break;

        if (f.skip) {
            if (t.kind == XT_START && !t.selfclose) f.skip++;
            else if (t.kind == XT_END && f.skip > 0) f.skip--;
            if (f.skip > 1000) f.skip = 0;
            continue;
        }
        if (t.kind == XT_TEXT) {
            if (in_t) flow_text(&f, t.text, t.tlen);
            continue;
        }

        const char *n = t.name;
        if (t.kind == XT_START) {
            if (!strcmp(n, "t")) {
                if (!t.selfclose) in_t++;
                continue;
            }
            if (!strcmp(n, "p")) {
                flow_para_open(&f, OFFICE_STYLE_BODY);
                ppr = 0;
            } else if (!strcmp(n, "pPr")) {
                ppr = 1;
            } else if (!strcmp(n, "pStyle")) {
                if (ppr) docx_style(&f, xattr(&t, "val"));
            } else if (!strcmp(n, "outlineLvl")) {
                if (ppr && f.style == OFFICE_STYLE_BODY) {
                    int lv = clamp_int(atoi(xattr(&t, "val") ? xattr(&t, "val") : "9"), 0, 8);
                    if (lv <= 2) f.style = OFFICE_STYLE_H1 + lv;
                }
            } else if (!strcmp(n, "numPr")) {
                if (ppr) f.style = OFFICE_STYLE_BULLET;
            } else if (!strcmp(n, "ilvl")) {
                if (ppr) {
                    const char *v = xattr(&t, "val");
                    f.level = clamp_int(v ? atoi(v) : 0, 0, 4);
                }
            } else if (!strcmp(n, "tbl")) {
                flow_table_open(&f);
            } else if (!strcmp(n, "tr")) {
                flow_row_open(&f);
            } else if (!strcmp(n, "tc")) {
                flow_cell_open(&f);
            } else if (!strcmp(n, "r")) {
                run = 1;
                if (!ppr) { f.bold = 0; f.italic = 0; }
            } else if (!strcmp(n, "b")) {
                if (!ppr && run) f.bold = attr_is_on(xattr(&t, "val"));
            } else if (!strcmp(n, "i")) {
                if (!ppr && run) f.italic = attr_is_on(xattr(&t, "val"));
            } else if (!strcmp(n, "tab")) {
                if (!ppr) flow_text(&f, "\t", 1);
            } else if (!strcmp(n, "br") || !strcmp(n, "cr")) {
                if (!ppr) flow_text(&f, "\n", 1);
            } else if (!strcmp(n, "del") || !strcmp(n, "moveFrom") ||
                       !strcmp(n, "Fallback") || !strcmp(n, "commentReference")) {
                if (!t.selfclose) f.skip = 1;
            }

            if (t.selfclose) {
                if (!strcmp(n, "p")) flow_para_close(&f);
                else if (!strcmp(n, "tc")) flow_cell_close(&f);
                else if (!strcmp(n, "tr")) flow_row_close(&f);
                else if (!strcmp(n, "tbl")) flow_table_close(&f);
                else if (!strcmp(n, "r")) run = 0;
            }
        } else { /* XT_END */
            if (!strcmp(n, "t")) {
                if (in_t > 0) in_t--;
            } else if (!strcmp(n, "p")) {
                flow_para_close(&f);
            } else if (!strcmp(n, "pPr")) {
                ppr = 0;
            } else if (!strcmp(n, "tbl")) {
                flow_table_close(&f);
            } else if (!strcmp(n, "tr")) {
                flow_row_close(&f);
            } else if (!strcmp(n, "tc")) {
                flow_cell_close(&f);
            } else if (!strcmp(n, "r")) {
                run = 0;
            } else if (!strcmp(n, "del") || !strcmp(n, "moveFrom") ||
                       !strcmp(n, "Fallback") || !strcmp(n, "commentReference")) {
                if (f.skip > 0) f.skip--;
            }
        }
    }

    if (x.failed) {
        snprintf(err, errsz, "word/document.xml is not well-formed: unterminated markup at byte %zu",
                 x.i);
        flow_free(&f);
        return -1;
    }
    if (f.oom) {
        snprintf(err, errsz, "out of memory while reading word/document.xml");
        flow_free(&f);
        return -1;
    }
    if (f.truncated) doc->truncated = 1;
    flow_free(&f);
    return 0;
}

/* ============================================================
 *  XLSX
 * ============================================================ */
typedef struct {
    char **s;
    int n, cap;
    size_t bytes;
} StrTable;

static void strtab_free(StrTable *t) {
    for (int i = 0; i < t->n; i++) free(t->s[i]);
    free(t->s);
    memset(t, 0, sizeof(*t));
}

static int strtab_push(StrTable *t, const char *s, size_t n) {
    if (t->n >= OFFICE_MAX_SHARED) return -1;
    if (t->bytes + n > 32u * 1024u * 1024u) return -1;
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        char **p = realloc(t->s, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        t->s = p;
        t->cap = cap;
    }
    char *copy = malloc(n + 1);
    if (!copy) return -1;
    if (n) memcpy(copy, s, n);
    copy[n] = 0;
    t->s[t->n++] = copy;
    t->bytes += n;
    return 0;
}

static int parse_shared_strings(const uint8_t *xml, size_t len, StrTable *st,
                                char *err, size_t errsz) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    StrBuf cur = {0};
    int in_si = 0, in_rph = 0, in_t = 0;

    while (xml_next(&x, &t)) {
        if (t.kind == XT_TEXT) {
            if (in_t && !in_rph) {
                if (sb_add(&cur, t.text, (size_t)t.tlen)) {
                    snprintf(err, errsz, "out of memory in sharedStrings.xml");
                    sb_free(&cur);
                    return -1;
                }
            }
            continue;
        }
        const char *n = t.name;
        if (t.kind == XT_START) {
            if (!strcmp(n, "si") && !t.selfclose) {
                in_si = 1;
                cur.n = 0;
                if (cur.p) cur.p[0] = 0;
            } else if (!strcmp(n, "rPh") && !t.selfclose) {
                in_rph++;
            } else if (!strcmp(n, "t") && in_si) {
                in_t = 1;
            }
            if (t.selfclose && !strcmp(n, "si") && in_si) in_si = 0;
        } else {
            if (!strcmp(n, "t")) in_t = 0;
            else if (!strcmp(n, "rPh")) { if (in_rph) in_rph--; }
            else if (!strcmp(n, "si")) {
                if (in_si) {
                    if (strtab_push(st, cur.p ? cur.p : "", cur.n)) {
                        snprintf(err, errsz, "out of memory for %d shared strings", st->n);
                        sb_free(&cur);
                        return -1;
                    }
                }
                in_si = 0;
            }
        }
    }
    sb_free(&cur);
    if (x.failed) {
        snprintf(err, errsz, "xl/sharedStrings.xml is not well-formed at byte %zu", x.i);
        return -1;
    }
    return 0;
}

typedef struct {
    int col;
    OfficeRef ref;
} XCell;

typedef struct {
    XCell *c;
    int n, cap;
} XCellList;

static void xcell_free(XCellList *l) {
    free(l->c);
    l->c = NULL;
    l->n = l->cap = 0;
}

static int xcell_push(XCellList *l, int col, OfficeRef ref) {
    if (col < 0 || col >= OFFICE_MAX_COLS) return 0;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        if (cap > OFFICE_MAX_COLS) cap = OFFICE_MAX_COLS;
        XCell *p = realloc(l->c, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        l->c = p;
        l->cap = cap;
    }
    l->c[l->n].col = col;
    l->c[l->n].ref = ref;
    l->n++;
    return 0;
}

typedef struct {
    XCellList *rows;
    int n, cap;
} XSheet;

static void xsheet_free(XSheet *s) {
    for (int i = 0; i < s->n; i++) xcell_free(&s->rows[i]);
    free(s->rows);
    memset(s, 0, sizeof(*s));
}

static int xsheet_add_row(XSheet *s) {
    if (s->n >= OFFICE_MAX_ROWS) return -1;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        if (cap > OFFICE_MAX_ROWS) cap = OFFICE_MAX_ROWS;
        XCellList *p = realloc(s->rows, (size_t)cap * sizeof(*p));
        if (!p) return -1;
        s->rows = p;
        s->cap = cap;
    }
    memset(&s->rows[s->n], 0, sizeof(s->rows[s->n]));
    s->n++;
    return 0;
}

static int col_from_ref(const char *ref) {
    if (!ref) return -1;
    int c = 0, any = 0;
    for (const char *p = ref; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        if (ch < 'A' || ch > 'Z') break;
        c = c * 26 + (ch - 'A' + 1);
        any = 1;
    }
    return any ? c - 1 : -1;
}

static void emit_xlsx_sheet(OfficeDoc *doc, const char *name, XSheet *s) {
    /* Trim trailing rows that hold no text at all: spreadsheets routinely
     * carry thousands of formatted-but-empty rows, and drawing them would
     * be page after page of nothing. Real gaps in the middle are kept. */
    int last = s->n - 1;
    while (last >= 0) {
        int has = 0;
        for (int i = 0; i < s->rows[last].n; i++) {
            OfficeRef *r = &s->rows[last].c[i].ref;
            if (r->len && doc->text[r->off]) { has = 1; break; }
        }
        if (has) break;
        last--;
    }
    int rows = last + 1;
    if (rows <= 0) return;

    if (name && name[0]) doc_emit_para(doc, OFFICE_STYLE_H2, 1, 0, 0, name, strlen(name));

    int cols = 0;
    for (int r = 0; r < rows; r++)
        for (int i = 0; i < s->rows[r].n; i++) {
            int c = s->rows[r].c[i].col + 1;
            if (c > cols) cols = c;
        }
    if (cols > OFFICE_MAX_COLS) cols = OFFICE_MAX_COLS;

    OfficeBlock *b = doc_add_block(doc, OFFICE_BLOCK_TABLE);
    if (!b) return;
    b->rows = rows;
    b->cols = cols;
    b->cell = calloc((size_t)rows * (size_t)cols, sizeof(*b->cell));
    b->cell_bold = calloc((size_t)rows * (size_t)cols, 1);
    if (!b->cell || !b->cell_bold) {
        free(b->cell);
        free(b->cell_bold);
        b->cell = NULL;
        b->cell_bold = NULL;
        b->rows = b->cols = 0;
        doc->truncated = 1;
        return;
    }
    for (int r = 0; r < rows; r++)
        for (int i = 0; i < s->rows[r].n; i++)
            b->cell[(size_t)r * (size_t)cols + (size_t)s->rows[r].c[i].col] =
                s->rows[r].c[i].ref;

    for (int i = 0; i < doc->nsheets; i++)
        if (!strcmp(doc->sheets[i].name, name ? name : "")) doc->sheets[i].drawn = 1;
}

static int parse_xlsx_sheet(OfficeDoc *doc, const uint8_t *xml, size_t len,
                            const StrTable *st, XSheet *sheet,
                            char *err, size_t errsz) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    int in_row = 0, in_cell = 0, in_v = 0, in_is = 0, in_t = 0;
    int row_target = 0;          /* 1-based row number from r=, 0 = next */
    int cell_col = -1, cell_seq = 0;
    char cell_type[16] = {0};
    StrBuf val = {0};
    int bad_shared = 0;

    while (xml_next(&x, &t)) {
        if (t.kind == XT_TEXT) {
            if (in_v || in_t) {
                if (sb_add(&val, t.text, (size_t)t.tlen)) {
                    snprintf(err, errsz, "out of memory in a worksheet cell");
                    sb_free(&val);
                    return -1;
                }
            }
            continue;
        }
        const char *n = t.name;
        if (t.kind == XT_START) {
            if (!strcmp(n, "row")) {
                const char *rf = xattr(&t, "r");
                row_target = rf ? atoi(rf) : 0;
                /* Honour row numbers: a gap is a real gap. */
                int target = row_target > 0 ? row_target - 1 : sheet->n;
                if (target < sheet->n) target = sheet->n;
                if (target >= OFFICE_MAX_ROWS) {
                    doc->truncated = 1;
                    break;
                }
                while (sheet->n < target) {
                    if (xsheet_add_row(sheet)) { doc->truncated = 1; break; }
                }
                if (xsheet_add_row(sheet)) { doc->truncated = 1; break; }
                in_row = 1;
                cell_seq = 0;
            } else if (!strcmp(n, "c") && in_row) {
                const char *rf = xattr(&t, "r");
                cell_col = rf ? col_from_ref(rf) : -1;
                if (cell_col < 0) cell_col = cell_seq;
                cell_seq = cell_col + 1;
                const char *ty = xattr(&t, "t");
                snprintf(cell_type, sizeof(cell_type), "%s", ty ? ty : "");
                val.n = 0;
                if (val.p) val.p[0] = 0;
                in_cell = 1;
                in_v = in_is = in_t = 0;
            } else if (!strcmp(n, "v") && in_cell) {
                in_v = 1;
            } else if (!strcmp(n, "is") && in_cell) {
                in_is = 1;
            } else if (!strcmp(n, "t") && in_is) {
                in_t = 1;
            }
        } else {
            if (!strcmp(n, "v")) {
                in_v = 0;
            } else if (!strcmp(n, "t")) {
                in_t = 0;
            } else if (!strcmp(n, "is")) {
                in_is = 0;
            } else if (!strcmp(n, "c") && in_cell) {
                OfficeRef ref = {0, 0};
                sb_trim(&val);
                const char *text = val.p ? val.p : "";
                size_t tl = val.n;

                if (!strcmp(cell_type, "s")) {
                    int idx = (int)strtol(text, NULL, 10);
                    if (idx >= 0 && idx < st->n) {
                        text = st->s[idx];
                        tl = strlen(text);
                    } else {
                        text = "";
                        tl = 0;
                        bad_shared = 1;
                    }
                } else if (!strcmp(cell_type, "b")) {
                    text = (text[0] == '1') ? "TRUE" : "FALSE";
                    tl = strlen(text);
                }
                if (doc_append(doc, text, tl, &ref)) {
                    doc->truncated = 1;
                }
                if (cell_col >= OFFICE_MAX_COLS) doc->truncated = 1;
                else if (sheet->n > 0 && xcell_push(&sheet->rows[sheet->n - 1], cell_col, ref)) {
                    snprintf(err, errsz, "out of memory for sheet cells");
                    sb_free(&val);
                    return -1;
                }
                in_cell = 0;
            } else if (!strcmp(n, "row")) {
                in_row = 0;
            }
        }
    }
    sb_free(&val);
    if (x.failed) {
        snprintf(err, errsz, "a worksheet is not well-formed at byte %zu", x.i);
        return -1;
    }
    if (bad_shared) {
        snprintf(err, errsz, "a cell refers to a shared string that does not exist");
        /* Not fatal: the remaining cells are still real. */
    }
    return 0;
}

/* ---- workbook + rels ---- */
typedef struct {
    char name[96];
    char rid[32];
    char target[192];
    int  drawn;
} WbSheet;

static int wb_add(WbSheet **list, int *n, int *cap) {
    if (*n >= OFFICE_MAX_SHEETS) return -1;
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        if (c > OFFICE_MAX_SHEETS) c = OFFICE_MAX_SHEETS;
        WbSheet *p = realloc(*list, (size_t)c * sizeof(*p));
        if (!p) return -1;
        *list = p;
        *cap = c;
    }
    memset(&(*list)[*n], 0, sizeof((*list)[*n]));
    (*n)++;
    return 0;
}

static void parse_workbook(const uint8_t *xml, size_t len, WbSheet **out,
                           int *nout, int *outcap) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    while (xml_next(&x, &t)) {
        if (t.kind != XT_START || strcmp(t.name, "sheet")) continue;
        const char *nm = xattr(&t, "name");
        const char *rid = xattr(&t, "id");
        if (!nm) continue;
        if (wb_add(out, nout, outcap)) return;
        WbSheet *s = &(*out)[*nout - 1];
        snprintf(s->name, sizeof(s->name), "%s", nm);
        if (rid) snprintf(s->rid, sizeof(s->rid), "%s", rid);
    }
}

static void parse_rels(const uint8_t *xml, size_t len, WbSheet *sheets, int n) {
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    while (xml_next(&x, &t)) {
        if (t.kind != XT_START || strcmp(t.name, "Relationship")) continue;
        const char *id = xattr(&t, "Id");
        const char *target = xattr(&t, "Target");
        const char *mode = xattr(&t, "TargetMode");
        if (!id || !target || (mode && !strcmp(mode, "External"))) continue;
        for (int i = 0; i < n; i++) {
            if (strcmp(sheets[i].rid, id)) continue;
            /* Targets are relative to xl/, and some writers spell the
             * absolute package path. Both are reduced to a ZIP name. */
            if (target[0] == '/') snprintf(sheets[i].target, sizeof(sheets[i].target), "%s", target + 1);
            else if (!strncmp(target, "xl/", 3)) snprintf(sheets[i].target, sizeof(sheets[i].target), "%s", target);
            else snprintf(sheets[i].target, sizeof(sheets[i].target), "xl/%s", target);
        }
    }
}

static int parse_xlsx(OfficeDoc *doc, const OfficeZip *z, char *err, size_t errsz) {
    StrTable st = {0};
    uint8_t *xml = NULL;
    size_t len = 0;

    int si = office_zip_find(z, "xl/sharedStrings.xml");
    if (si >= 0) {
        if (office_zip_read(z, si, &xml, &len, err, errsz) == 0) {
            int rc = parse_shared_strings(xml, len, &st, err, errsz);
            free(xml);
            xml = NULL;
            if (rc != 0) { strtab_free(&st); return -1; }
        } else {
            /* A missing shared string table is fatal for cell values, so
             * say so rather than show blank cells. */
            strtab_free(&st);
            return -1;
        }
    }

    WbSheet *wb = NULL;
    int nwb = 0, capwb = 0;
    int wi = office_zip_find(z, "xl/workbook.xml");
    if (wi >= 0 && office_zip_read(z, wi, &xml, &len, err, errsz) == 0) {
        parse_workbook(xml, len, &wb, &nwb, &capwb);
        free(xml);
        xml = NULL;
        int ri = office_zip_find(z, "xl/_rels/workbook.xml.rels");
        if (ri >= 0 && office_zip_read(z, ri, &xml, &len, err, errsz) == 0) {
            parse_rels(xml, len, wb, nwb);
            free(xml);
            xml = NULL;
        }
    }

    int drew_any = 0;
    for (int i = 0; i < nwb; i++) {
        const char *name = wb[i].target[0] ? wb[i].target : NULL;
        if (!name) continue;
        int ei = office_zip_find(z, name);
        if (ei < 0) continue;
        if (office_zip_read(z, ei, &xml, &len, err, errsz) != 0) continue;
        XSheet sh = {0};
        int rc = parse_xlsx_sheet(doc, xml, len, &st, &sh, err, errsz);
        free(xml);
        xml = NULL;
        if (rc != 0) { xsheet_free(&sh); free(wb); strtab_free(&st); return -1; }
        int has_rows = sh.n > 0;
        doc_add_sheet(doc, wb[i].name, 0);
        emit_xlsx_sheet(doc, wb[i].name, &sh);
        if (has_rows) drew_any = 1;
        xsheet_free(&sh);
    }

    if (!drew_any) {
        /* No workbook part (or no resolvable sheets): fall back to the
         * worksheet parts that are actually present, in numeric order. */
        int idx[OFFICE_MAX_SHEETS];
        int nidx = 0;
        for (int i = 0; i < z->count && nidx < OFFICE_MAX_SHEETS; i++) {
            const char *nm = z->ent[i].name;
            if (strncmp(nm, "xl/worksheets/sheet", 19)) continue;
            const char *tail = nm + 19;
            int num = atoi(tail);
            if (num <= 0) continue;
            int k = 0;
            while (k < (int)strlen(tail) && isdigit((unsigned char)tail[k])) k++;
            if (strcmp(tail + k, ".xml")) continue;
            idx[nidx++] = i;
        }
        for (int a = 1; a < nidx; a++) {
            int v = idx[a], b = a - 1;
            while (b >= 0 && atoi(z->ent[idx[b]].name + 19) > atoi(z->ent[v].name + 19)) {
                idx[b + 1] = idx[b];
                b--;
            }
            idx[b + 1] = v;
        }
        for (int i = 0; i < nidx; i++) {
            const char *nm = z->ent[idx[i]].name;
            const char *base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            char sheet_label[96];
            snprintf(sheet_label, sizeof(sheet_label), "%s", base);
            char *dot = strrchr(sheet_label, '.');
            if (dot) *dot = 0;

            if (office_zip_read(z, idx[i], &xml, &len, err, errsz) != 0) continue;
            XSheet sh = {0};
            int rc = parse_xlsx_sheet(doc, xml, len, &st, &sh, err, errsz);
            free(xml);
            xml = NULL;
            if (rc != 0) { xsheet_free(&sh); free(wb); strtab_free(&st); return -1; }
            doc_add_sheet(doc, sheet_label, 0);
            emit_xlsx_sheet(doc, sheet_label, &sh);
            xsheet_free(&sh);
            drew_any = 1;
        }
    }

    free(wb);
    strtab_free(&st);
    if (!drew_any) {
        snprintf(err, errsz, "the workbook has no readable worksheet");
        return -1;
    }
    return 0;
}

/* ============================================================
 *  PPTX - ppt/slides/slideN.xml
 * ============================================================ */
static int parse_pptx(OfficeDoc *doc, const OfficeZip *z, char *err, size_t errsz) {
    int idx[OFFICE_MAX_SHEETS];
    int nidx = 0;
    for (int i = 0; i < z->count && nidx < OFFICE_MAX_SHEETS; i++) {
        const char *nm = z->ent[i].name;
        if (strncmp(nm, "ppt/slides/slide", 16)) continue;
        const char *tail = nm + 16;
        int num = atoi(tail);
        if (num <= 0) continue;
        int k = 0;
        while (k < (int)strlen(tail) && isdigit((unsigned char)tail[k])) k++;
        if (strcmp(tail + k, ".xml")) continue;
        idx[nidx++] = i;
    }
    if (nidx == 0) {
        snprintf(err, errsz, "the presentation contains no ppt/slides/slideN.xml");
        return -1;
    }
    /* Slide files are numbered; present them in that order, which is what
     * PowerPoint writes. */
    for (int a = 1; a < nidx; a++) {
        int v = idx[a], b = a - 1;
        while (b >= 0 && atoi(z->ent[idx[b]].name + 16) > atoi(z->ent[v].name + 16)) {
            idx[b + 1] = idx[b];
            b--;
        }
        idx[b + 1] = v;
    }

    for (int i = 0; i < nidx; i++) {
        uint8_t *xml = NULL;
        size_t len = 0;
        if (office_zip_read(z, idx[i], &xml, &len, err, errsz) != 0) continue;

        int num = atoi(z->ent[idx[i]].name + 16);
        char label[48];
        snprintf(label, sizeof(label), "Slide %d", num);
        doc_emit_para(doc, OFFICE_STYLE_H2, 1, 0, 0, label, strlen(label));

        Flow f;
        flow_init(&f, doc);
        XmlReader x;
        xml_init(&x, xml, len);
        XmlTok t;
        int in_t = 0;

        while (xml_next(&x, &t)) {
            if (f.halt) break;
            if (t.kind == XT_TEXT) {
                if (in_t) flow_text(&f, t.text, t.tlen);
                continue;
            }
            const char *n = t.name;
            if (t.kind == XT_START) {
                if (!strcmp(n, "t")) { if (!t.selfclose) in_t++; }
                else if (!strcmp(n, "p")) flow_para_open(&f, OFFICE_STYLE_BODY);
                else if (!strcmp(n, "r")) { f.bold = 0; f.italic = 0; }
                else if (!strcmp(n, "rPr")) {
                    const char *b = xattr(&t, "b");
                    const char *it = xattr(&t, "i");
                    if (b) f.bold = attr_is_on(b);
                    if (it) f.italic = attr_is_on(it);
                } else if (!strcmp(n, "tbl")) flow_table_open(&f);
                else if (!strcmp(n, "tr")) flow_row_open(&f);
                else if (!strcmp(n, "tc")) flow_cell_open(&f);
                else if (!strcmp(n, "br")) flow_text(&f, "\n", 1);

                if (t.selfclose) {
                    if (!strcmp(n, "p")) flow_para_close(&f);
                    else if (!strcmp(n, "tc")) flow_cell_close(&f);
                    else if (!strcmp(n, "tr")) flow_row_close(&f);
                    else if (!strcmp(n, "tbl")) flow_table_close(&f);
                }
            } else {
                if (!strcmp(n, "t")) { if (in_t > 0) in_t--; }
                else if (!strcmp(n, "p")) flow_para_close(&f);
                else if (!strcmp(n, "tbl")) flow_table_close(&f);
                else if (!strcmp(n, "tr")) flow_row_close(&f);
                else if (!strcmp(n, "tc")) flow_cell_close(&f);
            }
        }
        if (x.failed) {
            snprintf(err, errsz, "ppt/slides/slide%d.xml is not well-formed at byte %zu",
                     num, x.i);
            flow_free(&f);
            free(xml);
            return -1;
        }
        if (f.oom) {
            snprintf(err, errsz, "out of memory reading slide %d", num);
            flow_free(&f);
            free(xml);
            return -1;
        }
        if (f.truncated) doc->truncated = 1;
        flow_free(&f);
        free(xml);
    }
    return 0;
}

/* ============================================================
 *  OpenDocument (.odt / .ods / .odp) - content.xml
 * ============================================================ */
static int parse_odf(OfficeDoc *doc, const OfficeZip *z, OfficeFormat fmt,
                     char *err, size_t errsz) {
    int ci = office_zip_find(z, "content.xml");
    if (ci < 0) {
        snprintf(err, errsz, "the OpenDocument package has no content.xml");
        return -1;
    }
    uint8_t *xml = NULL;
    size_t len = 0;
    if (office_zip_read(z, ci, &xml, &len, err, errsz) != 0) return -1;

    StyleMap sm;
    memset(&sm, 0, sizeof(sm));
    int sti = office_zip_find(z, "styles.xml");
    if (sti >= 0) {
        uint8_t *sx = NULL;
        size_t sl = 0;
        char e2[160];
        if (office_zip_read(z, sti, &sx, &sl, e2, sizeof(e2)) == 0) {
            style_map_scan(&sm, sx, sl);
            free(sx);
        }
    }
    style_map_scan(&sm, xml, len);
    if (sm.overflow) doc->truncated = 1;

    int spreadsheet = (fmt == OFFICE_FMT_ODS);
    int presentation = (fmt == OFFICE_FMT_ODP);

    Flow f;
    flow_init(&f, doc);
    XmlReader x;
    xml_init(&x, xml, len);
    XmlTok t;
    int list_depth = 0;
    int in_h = 0, in_p = 0;
    int span_sp = 0;
    int cell_repeat = 1, row_repeat = 1;
    struct { int bold, italic; } span_st[32];

    while (xml_next(&x, &t)) {
        if (f.halt) break;

        if (f.skip) {
            if (t.kind == XT_START && !t.selfclose) f.skip++;
            else if (t.kind == XT_END && f.skip > 0) f.skip--;
            if (f.skip > 1000) f.skip = 0;
            continue;
        }
        if (t.kind == XT_TEXT) { flow_text(&f, t.text, t.tlen); continue; }

        const char *n = t.name;
        if (t.kind == XT_START) {
            if (!strcmp(n, "annotation") || !strcmp(n, "note") ||
                !strcmp(n, "header") || !strcmp(n, "footer") ||
                !strcmp(n, "page-header") || !strcmp(n, "page-footer") ||
                !strcmp(n, "tracked-changes") || !strcmp(n, "index-body") ||
                !strcmp(n, "table-of-content") || !strcmp(n, "bibliography")) {
                if (!t.selfclose) f.skip = 1;
                continue;
            }
            if (!strcmp(n, "h")) {
                int lv = 1;
                const char *ol = xattr(&t, "outline-level");
                if (ol) lv = clamp_int(atoi(ol), 1, 9);
                int style = OFFICE_STYLE_H1 + clamp_int(lv - 1, 0, 2);
                flow_para_open(&f, style);
                in_h++;
                f.bold = 1;
                const char *sn = xattr(&t, "style-name");
                if (sn) style_map_apply(&sm, sn, &f.bold, &f.italic);
            } else if (!strcmp(n, "p")) {
                int style = list_depth > 0 ? OFFICE_STYLE_BULLET : OFFICE_STYLE_BODY;
                flow_para_open(&f, style);
                if (list_depth > 0) f.level = clamp_int(list_depth - 1, 0, 4);
                in_p++;
                const char *sn = xattr(&t, "style-name");
                if (sn) style_map_apply(&sm, sn, &f.bold, &f.italic);
            } else if (!strcmp(n, "span")) {
                if (span_sp < (int)(sizeof(span_st) / sizeof(span_st[0]))) {
                    span_st[span_sp].bold = f.bold;
                    span_st[span_sp].italic = f.italic;
                    span_sp++;
                    const char *sn = xattr(&t, "style-name");
                    if (sn) style_map_apply(&sm, sn, &f.bold, &f.italic);
                }
            } else if (!strcmp(n, "s")) {
                const char *cs = xattr(&t, "c");
                int count = cs ? atoi(cs) : 1;
                if (count < 1) count = 1;
                if (count > 64) count = 64;
                for (int i = 0; i < count; i++) flow_text(&f, " ", 1);
            } else if (!strcmp(n, "tab")) {
                flow_text(&f, "\t", 1);
            } else if (!strcmp(n, "line-break")) {
                flow_text(&f, "\n", 1);
            } else if (!strcmp(n, "list")) {
                if (!t.selfclose) list_depth++;
            } else if (!strcmp(n, "table")) {
                if (spreadsheet) {
                    const char *nm = xattr(&t, "name");
                    doc_add_sheet(doc, nm ? nm : "", 0);
                    if (nm && *nm)
                        doc_emit_para(doc, OFFICE_STYLE_H2, 1, 0, 0, nm, strlen(nm));
                }
                flow_table_open(&f);
            } else if (!strcmp(n, "table-row")) {
                row_repeat = 1;
                const char *rp = xattr(&t, "number-rows-repeated");
                if (rp) row_repeat = clamp_int(atoi(rp), 1, 64);
                flow_row_open(&f);
            } else if (!strcmp(n, "table-cell") || !strcmp(n, "covered-table-cell")) {
                cell_repeat = 1;
                const char *rp = xattr(&t, "number-columns-repeated");
                if (rp) cell_repeat = clamp_int(atoi(rp), 1, 64);
                flow_cell_open(&f);
            } else if (!strcmp(n, "draw-page") || !strcmp(n, "page")) {
                if (presentation) {
                    const char *nm = xattr(&t, "name");
                    if (nm && *nm)
                        doc_emit_para(doc, OFFICE_STYLE_H2, 1, 0, 0, nm, strlen(nm));
                }
            }

            if (t.selfclose) {
                if (!strcmp(n, "h")) { flow_para_close(&f); if (in_h) in_h--; }
                else if (!strcmp(n, "p")) { flow_para_close(&f); if (in_p) in_p--; }
                else if (!strcmp(n, "table-cell") || !strcmp(n, "covered-table-cell")) {
                    int was = f.in_cell;
                    flow_cell_close(&f);
                    if (was && cell_repeat > 1) flow_cell_repeat(&f, cell_repeat - 1);
                }
                else if (!strcmp(n, "table-row")) {
                    flow_row_close(&f);
                    if (row_repeat > 1) flow_row_repeat(&f, row_repeat - 1);
                }
                else if (!strcmp(n, "table")) flow_table_close(&f);
            }
        } else { /* XT_END */
            if (!strcmp(n, "h")) { flow_para_close(&f); if (in_h) in_h--; }
            else if (!strcmp(n, "p")) { flow_para_close(&f); if (in_p) in_p--; }
            else if (!strcmp(n, "span")) {
                if (span_sp > 0) {
                    span_sp--;
                    f.bold = span_st[span_sp].bold;
                    f.italic = span_st[span_sp].italic;
                }
            } else if (!strcmp(n, "list")) {
                if (list_depth > 0) list_depth--;
            } else if (!strcmp(n, "table")) {
                flow_table_close(&f);
            } else if (!strcmp(n, "table-row")) {
                flow_row_close(&f);
                if (row_repeat > 1) flow_row_repeat(&f, row_repeat - 1);
            } else if (!strcmp(n, "table-cell") || !strcmp(n, "covered-table-cell")) {
                int was = f.in_cell;
                flow_cell_close(&f);
                if (was && cell_repeat > 1) flow_cell_repeat(&f, cell_repeat - 1);
            }
        }
    }

    if (x.failed) {
        snprintf(err, errsz, "content.xml is not well-formed at byte %zu", x.i);
        flow_free(&f);
        free(xml);
        return -1;
    }
    if (f.oom) {
        snprintf(err, errsz, "out of memory reading content.xml");
        flow_free(&f);
        free(xml);
        return -1;
    }
    if (f.truncated) doc->truncated = 1;
    flow_free(&f);
    free(xml);
    return 0;
}

/* ============================================================
 *  Plain text, detection, entry points
 * ============================================================ */
static void build_plain(OfficeDoc *doc) {
    StrBuf b = {0};
    for (int i = 0; i < doc->nblocks; i++) {
        const OfficeBlock *bl = &doc->blocks[i];
        if (bl->kind == OFFICE_BLOCK_PARA) {
            const char *s = office_block_text(doc, bl);
            if (!s[0]) continue;
            for (int k = 0; k < bl->level && k < 4; k++) {
                if (sb_add(&b, "  ", 2)) goto done;
            }
            if (bl->style == OFFICE_STYLE_BULLET && sb_add(&b, "\xE2\x80\xA2 ", 4)) goto done;
            if (sb_add(&b, s, strlen(s))) goto done;
            if (sb_add(&b, "\n", 1)) goto done;
        } else if (bl->kind == OFFICE_BLOCK_TABLE) {
            for (int r = 0; r < bl->rows; r++) {
                for (int c = 0; c < bl->cols; c++) {
                    const char *s = office_cell_text(doc, bl, r, c);
                    if (c && sb_add(&b, "\t", 1)) goto done;
                    if (sb_add(&b, s, strlen(s))) goto done;
                }
                if (sb_add(&b, "\n", 1)) goto done;
            }
            if (sb_add(&b, "\n", 1)) goto done;
        }
    }
done:
    doc->plain = b.p;
    doc->plain_len = b.n;
    if (!b.p) doc->plain_len = 0;
}

static int mem_has_name(const uint8_t *d, size_t n, const char *name) {
    size_t L = strlen(name);
    if (L == 0 || n < L) return 0;
    for (size_t i = 0; i + L <= n; i++) {
        if (d[i] != (uint8_t)name[0]) continue;
        if (!memcmp(d + i, name, L)) return 1;
        if (i + 2 * L <= n) {
            size_t k = 0;
            while (k < L && d[i + 2 * k] == (uint8_t)name[k] && d[i + 2 * k + 1] == 0) k++;
            if (k == L) return 1;
        }
    }
    return 0;
}

/* The ZIP is the container for every modern format; returning a format
 * from the parts actually present is more reliable than the extension. */
static OfficeFormat detect_zip(const uint8_t *data, size_t len) {
    OfficeZip *z = malloc(sizeof(*z));
    if (!z) return OFFICE_FMT_UNKNOWN;
    char err[160];
    if (office_zip_open(z, data, len, err, sizeof(err)) != 0) {
        free(z);
        return OFFICE_FMT_UNKNOWN;
    }
    OfficeFormat fmt = OFFICE_FMT_UNKNOWN;

    int mi = office_zip_find(z, "mimetype");
    if (mi >= 0) {
        uint8_t *m = NULL;
        size_t ml = 0;
        if (office_zip_read(z, mi, &m, &ml, err, sizeof(err)) == 0) {
            if (strstr((char *)m, "opendocument.text")) fmt = OFFICE_FMT_ODT;
            else if (strstr((char *)m, "opendocument.spreadsheet")) fmt = OFFICE_FMT_ODS;
            else if (strstr((char *)m, "opendocument.presentation")) fmt = OFFICE_FMT_ODP;
            free(m);
        }
    }
    if (fmt == OFFICE_FMT_UNKNOWN) {
        if (office_zip_find(z, "word/document.xml") >= 0) fmt = OFFICE_FMT_DOCX;
        else if (office_zip_find(z, "xl/workbook.xml") >= 0 ||
                 office_zip_find(z, "xl/worksheets/sheet1.xml") >= 0) fmt = OFFICE_FMT_XLSX;
        else if (office_zip_find(z, "ppt/presentation.xml") >= 0 ||
                 office_zip_find(z, "ppt/slides/slide1.xml") >= 0) fmt = OFFICE_FMT_PPTX;
        else if (office_zip_find(z, "content.xml") >= 0) {
            /* An OpenDocument package without a mimetype entry: read the
             * body element to tell text from a spreadsheet or slides. */
            int ci = office_zip_find(z, "content.xml");
            uint8_t *c = NULL;
            size_t cl = 0;
            if (ci >= 0 && office_zip_read(z, ci, &c, &cl, err, sizeof(err)) == 0) {
                if (strstr((char *)c, "office:spreadsheet")) fmt = OFFICE_FMT_ODS;
                else if (strstr((char *)c, "office:presentation")) fmt = OFFICE_FMT_ODP;
                else if (strstr((char *)c, "office:text")) fmt = OFFICE_FMT_ODT;
                free(c);
            }
        }
    }
    free(z);
    return fmt;
}

OfficeFormat office_detect(const uint8_t *data, size_t len) {
    if (!data || len < 4) return OFFICE_FMT_UNKNOWN;

    /* OLE2 compound file: the legacy binary formats. */
    static const uint8_t ole2[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (len >= 8 && !memcmp(data, ole2, 8)) {
        size_t scan = len > (32u * 1024u * 1024u) ? 32u * 1024u * 1024u : len;
        if (mem_has_name(data, scan, "WordDocument")) return OFFICE_FMT_DOC;
        if (mem_has_name(data, scan, "Workbook")) return OFFICE_FMT_XLS;
        if (mem_has_name(data, scan, "Book")) return OFFICE_FMT_XLS;
        if (mem_has_name(data, scan, "PowerPoint Document")) return OFFICE_FMT_PPT;
        return OFFICE_FMT_LEGACY;
    }
    if (data[0] == 'P' && data[1] == 'K' &&
        ((data[2] == 3 && data[3] == 4) || (data[2] == 5 && data[3] == 6) ||
         (data[2] == 7 && data[3] == 8)))
        return detect_zip(data, len);

    if (len >= 5 && !memcmp(data, "{\\rtf", 5)) return OFFICE_FMT_RTF;
    if (len >= 5 && (!memcmp(data, "<html", 5) || !memcmp(data, "<!DOC", 5)))
        return OFFICE_FMT_HTML;
    return OFFICE_FMT_UNKNOWN;
}

const char *office_format_name(OfficeFormat f) {
    switch (f) {
    case OFFICE_FMT_DOCX: return "Word document (.docx)";
    case OFFICE_FMT_XLSX: return "Excel workbook (.xlsx)";
    case OFFICE_FMT_PPTX: return "PowerPoint presentation (.pptx)";
    case OFFICE_FMT_ODT:  return "OpenDocument text (.odt)";
    case OFFICE_FMT_ODS:  return "OpenDocument spreadsheet (.ods)";
    case OFFICE_FMT_ODP:  return "OpenDocument presentation (.odp)";
    case OFFICE_FMT_DOC:  return "Word 97-2003 (.doc, binary)";
    case OFFICE_FMT_XLS:  return "Excel 97-2003 (.xls, binary)";
    case OFFICE_FMT_PPT:  return "PowerPoint 97-2003 (.ppt, binary)";
    case OFFICE_FMT_LEGACY: return "Microsoft Office 97-2003 (OLE2, binary)";
    case OFFICE_FMT_RTF:  return "Rich Text Format (.rtf)";
    case OFFICE_FMT_HTML: return "HTML document";
    default: return "Unknown or unsupported document";
    }
}

static void doc_set_message(OfficeDoc *doc) {
    switch (doc->fmt) {
    case OFFICE_FMT_DOCX:
        snprintf(doc->message, sizeof(doc->message),
                 "Text, headings, bold/italic runs and tables from word/document.xml. "
                 "Images, footnotes, headers/footers and page layout are not read.");
        break;
    case OFFICE_FMT_XLSX: {
        int drawn = 0;
        for (int i = 0; i < doc->nsheets; i++) if (doc->sheets[i].drawn) drawn++;
        snprintf(doc->message, sizeof(doc->message),
                 "Cell values as stored, %d of %d sheet(s) shown. Formulas are not "
                 "evaluated and number formats are not applied.",
                 drawn, doc->nsheets);
        break;
    }
    case OFFICE_FMT_PPTX:
        snprintf(doc->message, sizeof(doc->message),
                 "Slide text in file-number order. Layout, notes, images and "
                 "speaker notes are not read.");
        break;
    case OFFICE_FMT_ODT:
        snprintf(doc->message, sizeof(doc->message),
                 "Headings, paragraphs, lists and tables from content.xml. "
                 "Images and page layout are not read.");
        break;
    case OFFICE_FMT_ODS: {
        int drawn = 0;
        for (int i = 0; i < doc->nsheets; i++) if (doc->sheets[i].drawn) drawn++;
        snprintf(doc->message, sizeof(doc->message),
                 "%d of %d sheet(s) shown as tables. Cell values are as stored; "
                 "formulas and number formats are not applied.", drawn, doc->nsheets);
        break;
    }
    case OFFICE_FMT_ODP:
        snprintf(doc->message, sizeof(doc->message),
                 "Text frames from content.xml, slide by slide. Images, shapes "
                 "and animations are not read.");
        break;
    case OFFICE_FMT_DOC:
    case OFFICE_FMT_XLS:
    case OFFICE_FMT_PPT:
    case OFFICE_FMT_LEGACY:
        snprintf(doc->message, sizeof(doc->message),
                 "This is the old binary Microsoft Office format (OLE2). It is "
                 "detected, not parsed: open it in Word or LibreOffice and save "
                 "it as .docx / .xlsx / .pptx to read it here.");
        break;
    case OFFICE_FMT_RTF:
        snprintf(doc->message, sizeof(doc->message),
                 "Rich Text Format is detected but not parsed. Save it as .docx "
                 "or .odt to read it here.");
        break;
    case OFFICE_FMT_HTML:
        snprintf(doc->message, sizeof(doc->message),
                 "An HTML file. It is not an Office container; the browser app "
                 "is the one that renders HTML.");
        break;
    default:
        snprintf(doc->message, sizeof(doc->message),
                 "Not a document this reader knows: no OOXML or OpenDocument "
                 "parts were found.");
        break;
    }
    if (doc->truncated) {
        size_t used = strlen(doc->message);
        snprintf(doc->message + used, sizeof(doc->message) - used,
                 " The document exceeded a parser limit, so it is truncated.");
    }
}

int office_parse(OfficeDoc *doc, const char *path, const uint8_t *data, size_t len,
                 char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!doc || !data || !path) {
        snprintf(err, errsz, "no document data");
        return -1;
    }
    memset(doc, 0, sizeof(*doc));
    snprintf(doc->file, sizeof(doc->file), "%s", path);

    if (len > OFFICE_MAX_FILE) {
        snprintf(err, errsz, "%s is %zu bytes; the reader caps files at %u",
                 path, len, OFFICE_MAX_FILE);
        return -1;
    }

    doc->fmt = office_detect(data, len);
    snprintf(doc->format_name, sizeof(doc->format_name), "%s",
             office_format_name(doc->fmt));

    if (doc->fmt == OFFICE_FMT_DOC || doc->fmt == OFFICE_FMT_XLS ||
        doc->fmt == OFFICE_FMT_PPT || doc->fmt == OFFICE_FMT_LEGACY ||
        doc->fmt == OFFICE_FMT_RTF || doc->fmt == OFFICE_FMT_HTML) {
        doc->legacy = 1;
        doc_set_message(doc);
        return 0;
    }

    OfficeZip *z = malloc(sizeof(*z));
    if (!z) {
        snprintf(err, errsz, "out of memory for the ZIP directory");
        return -1;
    }
    if (office_zip_open(z, data, len, err, errsz) != 0) {
        free(z);
        doc_set_message(doc);
        return -1;
    }

    int rc = -1;
    if (doc->fmt == OFFICE_FMT_DOCX) {
        int i = office_zip_find(z, "word/document.xml");
        if (i < 0) {
            snprintf(err, errsz, "the .docx package has no word/document.xml%s",
                     z->truncated ? " (the archive has too many entries to index)" : "");
        } else {
            uint8_t *xml = NULL;
            size_t xlen = 0;
            if (office_zip_read(z, i, &xml, &xlen, err, errsz) == 0) {
                rc = parse_docx(doc, xml, xlen, err, errsz);
                free(xml);
            }
        }
        doc_meta(doc, z, "docProps/core.xml");
    } else if (doc->fmt == OFFICE_FMT_XLSX) {
        rc = parse_xlsx(doc, z, err, errsz);
        doc_meta(doc, z, "docProps/core.xml");
    } else if (doc->fmt == OFFICE_FMT_PPTX) {
        rc = parse_pptx(doc, z, err, errsz);
        doc_meta(doc, z, "docProps/core.xml");
    } else if (doc->fmt == OFFICE_FMT_ODT || doc->fmt == OFFICE_FMT_ODS ||
               doc->fmt == OFFICE_FMT_ODP) {
        rc = parse_odf(doc, z, doc->fmt, err, errsz);
        doc_meta(doc, z, "meta.xml");
    } else {
        snprintf(err, errsz, "the file is a ZIP archive but not an Office document this reader knows");
    }
    free(z);

    if (rc != 0) {
        office_free(doc);
        return -1;
    }

    if (doc->title[0] == 0) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(doc->title, sizeof(doc->title), "%s", base);
    }
    doc_set_message(doc);
    build_plain(doc);
    if (!doc->plain && doc->nblocks > 0) {
        /* The plain view is a convenience; the structured one is intact. */
        doc->plain = malloc(1);
        if (doc->plain) doc->plain[0] = 0;
        doc->plain_len = 0;
    }
    return 0;
}

int office_open(OfficeDoc *doc, const char *path, char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "%s: cannot seek: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    long end = ftell(f);
    if (end < 0) {
        snprintf(err, errsz, "%s: cannot size: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if ((uint64_t)end > OFFICE_MAX_FILE) {
        snprintf(err, errsz, "%s is %ld bytes; the reader caps files at %u",
                 path, end, OFFICE_MAX_FILE);
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err, errsz, "%s: cannot rewind: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    size_t len = (size_t)end;
    uint8_t *data = malloc(len ? len : 1);
    if (!data) {
        snprintf(err, errsz, "%s: out of memory for %zu bytes", path, len);
        fclose(f);
        return -1;
    }
    size_t got = len ? fread(data, 1, len, f) : 0;
    int bad = ferror(f);
    fclose(f);
    if (got != len) {
        snprintf(err, errsz, "%s: read %zu of %zu bytes%s", path, got, len,
                 bad ? " (I/O error)" : "");
        free(data);
        return -1;
    }
    int rc = office_parse(doc, path, data, len, err, errsz);
    free(data);
    return rc;
}

void office_free(OfficeDoc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->nblocks; i++) {
        free(doc->blocks[i].cell);
        free(doc->blocks[i].cell_bold);
        free(doc->blocks[i].runs);
    }
    free(doc->blocks);
    free(doc->text);
    free(doc->plain);
    free(doc->sheets);
    memset(doc, 0, sizeof(*doc));
}
