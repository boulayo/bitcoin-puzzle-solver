/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* lz.h - decoder of the compressed GPU kernel texts (the PTX modules are embedded compressed and unpacked when one is loaded).
 * The format is described in lz.c; the encoder is part of tools/embed.c, which runs while the program is built. */
#ifndef PZ_LZ_H
#define PZ_LZ_H

#include <stddef.h>

#define PZ_LZ_ERR ((size_t)-1)

/* Size of the text a stream unpacks to (read from its header), or PZ_LZ_ERR if the header is not valid. */
size_t pz_lz_rawsize(const unsigned char *src, size_t n);

/* Unpacks the stream src[0..n) into dst[dlen ..], where dst has room for cap bytes in all. dst[0..dlen) holds the dictionary the stream was
 * encoded against (none: dlen = 0); matches may point into it. Returns the size of the unpacked text, or PZ_LZ_ERR if the stream is not a
 * complete, valid stream of that dictionary (truncated, corrupt, too big for cap, wrong checksum); it never reads or writes out of range,
 * whatever src holds, and dst is only written at [dlen, cap). */
size_t pz_lz_unpack(const unsigned char *src, size_t n, unsigned char *dst, size_t dlen, size_t cap);

/* An embedded text: the compressed stream, and the text it was encoded against (NULL if it stands alone). tools/embed.c -z writes these. */
typedef struct pz_blob {
    const unsigned char *z;
    size_t zlen;
    const struct pz_blob *base;
} pz_blob;

/* The text of a blob (its base is unpacked first, and freed again) in a new NUL-terminated buffer; *len is its size without the NUL.
 * Returns NULL if the data is not valid or memory runs out. The caller frees the buffer. */
char *pz_blob_text(const pz_blob *b, size_t *len);

#endif
