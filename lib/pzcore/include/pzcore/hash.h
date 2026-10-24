/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash.h - SHA-256, double SHA-256, RIPEMD-160 and HASH160 (RIPEMD-160 of SHA-256, the hash of a Bitcoin public key) of byte strings.
 *
 * One-shot functions: the whole message is given at once. data may be NULL only when len is 0, and the message may be of any length
 * (the length counter wraps above 2^61 bytes, as the hash definitions do). out never overlaps data. Thread-safe, no allocation.
 * The batch version for many public keys at a time, with SIMD, is in hash160_batch.h. */
#ifndef PZCORE_HASH_H
#define PZCORE_HASH_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

void pz_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void pz_sha256d(const uint8_t *data, size_t len, uint8_t out[32]);       /* SHA-256 of the SHA-256 */
void pz_ripemd160(const uint8_t *data, size_t len, uint8_t out[20]);
void pz_hash160(const uint8_t *data, size_t len, uint8_t out[20]);       /* RIPEMD-160 of the SHA-256 */

/* The compression functions on one 64-byte block, for callers that manage the padding themselves (mid-states, fixed-size
 * messages). SHA-256: w_in holds the sixteen words of the block already converted from big-endian; state is H0..H7 (start from
 * the initial values of FIPS 180-4). RIPEMD-160: x holds the sixteen words of the block already converted from little-endian. */
void pz_sha256_compress(uint32_t state[8], const uint32_t w_in[16]);
void pz_ripemd160_compress(uint32_t state[5], const uint32_t x[16]);

/* HASH160 of the compressed public key made of the parity byte 02 (parity even) or 03 (odd, any non-zero parity & 1) and the
 * 32-byte big-endian x coordinate. */
void pz_hash160_pub33(uint8_t parity, const uint8_t x_be[32], uint8_t out[20]);

PZ_END_DECLS

#endif
