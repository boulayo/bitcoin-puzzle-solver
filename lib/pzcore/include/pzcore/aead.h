/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* aead.h - ChaCha20, Poly1305 and their AEAD construction (RFC 8439), the cipher of the encrypted channels of pzpeer (Noise
 * ChaChaPoly). Plain C with no table and no branch on the key or the data, so its time does not depend on them. No allocation, thread-safe. */
#ifndef PZCORE_AEAD_H
#define PZCORE_AEAD_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

#define PZ_AEAD_KEY 32
#define PZ_AEAD_NONCE 12
#define PZ_AEAD_TAG 16

/* out = in XOR the ChaCha20 key stream from block `counter` on (RFC 8439 2.4). in and out may be the same buffer. */
void pz_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len);
/* The Poly1305 tag of msg with a one-time key (RFC 8439 2.5). */
void pz_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);

/* AEAD_CHACHA20_POLY1305 (RFC 8439 2.8). seal: ct = pt encrypted, and its tag over ad and ct. open: true and pt = ct decrypted when the tag
 * is right; false, with pt untouched, when it is not. pt and ct may be the same buffer; ad may be NULL when adlen is 0. */
void pz_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t adlen, const uint8_t *pt, size_t len,
                  uint8_t *ct, uint8_t tag[16]);
bool pz_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len,
                  const uint8_t tag[16], uint8_t *pt);

PZ_END_DECLS

#endif
