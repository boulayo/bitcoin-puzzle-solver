/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* lz.c - decoder of the compressed GPU kernel texts (see lz.h). A small LZ77 format of our own, made for text with long repeats (the PTX
 * modules, which differ from one another in few lines: a module is encoded against another one as a preset dictionary).
 *
 *   stream   = size, check, sequence...
 *   size     = varint: bytes of the unpacked text
 *   check    = 4 bytes, little endian: FNV-1a hash of the unpacked text
 *   sequence = token, [varint], literals, [offset, [varint]]
 *   token    = literal count in the high nibble, match length - 4 in the low one; a nibble of 15 is followed by a varint
 *              (to be added to 15): the first one after the token (literals), the second one after the offset (match)
 *   offset   = varint: how far back the match starts (1 = the previous byte); it may reach into the dictionary
 *   varint   = 7 bits per byte, low group first, bit 7 set when another byte follows (at most 5 bytes, below 2^32)
 * The last sequence has only literals and ends the text; every other one ends with a match. Nothing may follow the last sequence. */
#include "lz.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOB_MAX ((size_t)64 << 20)   /* the biggest text pz_blob_text unpacks (the PTX modules are about 1 MiB) */
#define BLOB_DEPTH 8                  /* the longest chain of bases */

static bool get_varint(const unsigned char *s, size_t n, size_t *i, size_t *v)
{
    size_t r = 0;
    for (unsigned sh = 0; sh <= 28; sh += 7) {
        if (*i >= n) return false;
        unsigned b = s[(*i)++];
        r |= (size_t)(b & 127) << sh;
        if (!(b & 128)) { *v = r; return sh < 28 || b < 16; }   /* the fifth byte stops at 2^32 */
    }
    return false;
}

size_t pz_lz_rawsize(const unsigned char *src, size_t n)
{
    size_t i = 0, raw;
    return get_varint(src, n, &i, &raw) ? raw : PZ_LZ_ERR;
}

size_t pz_lz_unpack(const unsigned char *s, size_t n, unsigned char *dst, size_t dlen, size_t cap)
{
    size_t i = 0, raw, o = dlen, end;
    if (dlen > cap || !get_varint(s, n, &i, &raw) || raw > cap - dlen || n - i < 4) return PZ_LZ_ERR;
    const uint32_t want = (uint32_t)s[i] | (uint32_t)s[i + 1] << 8 | (uint32_t)s[i + 2] << 16 | (uint32_t)s[i + 3] << 24;
    i += 4;
    end = dlen + raw;
    for (;;) {
        size_t lit, mlen, off, x;
        if (i >= n) return PZ_LZ_ERR;
        const unsigned tok = s[i++];
        lit = tok >> 4;
        mlen = tok & 15;
        if (lit == 15) {
            if (!get_varint(s, n, &i, &x) || x > raw) return PZ_LZ_ERR;   /* (a count above the text is wrong anyway; this keeps the sums small) */
            lit += x;
        }
        if (lit > n - i || lit > end - o) return PZ_LZ_ERR;
        memcpy(dst + o, s + i, lit);
        i += lit;
        o += lit;
        if (o == end) {
            if (mlen) return PZ_LZ_ERR;                                   /* the last sequence has no match */
            break;
        }
        if (!get_varint(s, n, &i, &off) || off == 0 || off > o) return PZ_LZ_ERR;
        if (mlen == 15) {
            if (!get_varint(s, n, &i, &x) || x > raw) return PZ_LZ_ERR;
            mlen += x;
        }
        mlen += 4;
        if (mlen > end - o) return PZ_LZ_ERR;
        if (off >= mlen) memcpy(dst + o, dst + o - off, mlen);
        else for (size_t k = 0; k < mlen; k++) dst[o + k] = dst[o + k - off];   /* the match overlaps its own output: byte by byte */
        o += mlen;
    }
    if (i != n) return PZ_LZ_ERR;
    uint32_t h = 2166136261u;                                              /* FNV-1a */
    for (size_t k = dlen; k < end; k++) h = (h ^ dst[k]) * 16777619u;
    return h == want ? raw : PZ_LZ_ERR;
}

static char *blob_text(const pz_blob *b, size_t *len, int depth)
{
    char *dict = NULL, *buf;
    size_t dlen = 0, raw = pz_lz_rawsize(b->z, b->zlen);
    if (raw == PZ_LZ_ERR || raw > BLOB_MAX || depth > BLOB_DEPTH) return NULL;
    if (b->base && !(dict = blob_text(b->base, &dlen, depth + 1))) return NULL;
    buf = malloc(dlen + raw + 1);
    if (buf && dlen) memcpy(buf, dict, dlen);
    free(dict);
    if (!buf) return NULL;
    if (pz_lz_unpack(b->z, b->zlen, (unsigned char *)buf, dlen, dlen + raw) != raw) { free(buf); return NULL; }
    memmove(buf, buf + dlen, raw);
    buf[raw] = 0;
    *len = raw;
    return buf;
}

char *pz_blob_text(const pz_blob *b, size_t *len) { return blob_text(b, len, 0); }
