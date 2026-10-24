/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash.h - SHA-256, RIPEMD-160, hash160, Base58Check. Scalar reference
 * implementations; the SIMD batch versions live in hashv*.c. */
#ifndef PZ_HASH_H
#define PZ_HASH_H

#include "common.h"

void sha256(const u8 *data, size_t len, u8 out[32]);
void sha256d(const u8 *data, size_t len, u8 out[32]);
void ripemd160(const u8 *data, size_t len, u8 out[20]);
void hash160(const u8 *data, size_t len, u8 out[20]);

/* Single-block compression helpers used by the scalar hot paths. */
void sha256_compress(u32 state[8], const u32 w_in[16]);   /* w_in big-endian words already converted */
void ripemd160_compress(u32 state[5], const u32 x[16]);

/* hash160 of a compressed public key given as parity + 32-byte big-endian x. */
void hash160_pub33(u8 parity, const u8 x_be[32], u8 out[20]);

/* Base58Check. */
int base58check_decode(const char *s, u8 *out, size_t maxlen);   /* returns payload length (checksum verified) or -1 */
void base58check_encode(const u8 *payload, size_t len, char *out, size_t outlen);
/* Decodes a mainnet address into its hash160: P2PKH ("1...", Base58Check) or native SegWit P2WPKH ("bc1q...", bech32). */
bool address_to_hash160(const char *addr, u8 h160[20]);
void hash160_to_address(const u8 h160[20], char *out, size_t outlen);
void privkey_to_wif(const u8 key_be[32], bool compressed, char *out, size_t outlen);

#endif
