/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* address.h - the text formats of Bitcoin: Base58Check (P2PKH addresses, WIF private keys), bech32 and bech32m (BIP173, BIP350) and
 * the SegWit addresses built on them.
 *
 * Every function that takes text accepts hostile input: a NULL or over-long string, characters outside the alphabet, mixed case, a
 * wrong checksum and so on are reported by the return value and never read or write outside the buffers. Functions that write
 * text take the size of the buffer and return an error instead of truncating. Thread-safe, no allocation. */
#ifndef PZCORE_ADDRESS_H
#define PZCORE_ADDRESS_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

/* ---- Base58Check ---- */

/* the largest payload (without the four checksum bytes) that Base58Check encoding and decoding handle */
#define PZ_BASE58CHECK_MAX_PAYLOAD 96
/* a buffer that holds the text of any Base58Check string of at most that payload, with the terminating NUL */
#define PZ_BASE58CHECK_MAX_TEXT 140

/* Decodes a Base58Check string and verifies its checksum. Writes the payload (without the checksum) to out, which holds outcap
 * bytes, and returns its length (0 to PZ_BASE58CHECK_MAX_PAYLOAD). Returns -1 when the text is not valid: a NULL pointer, a character
 * outside the alphabet (this includes spaces), a bad checksum, a payload longer than PZ_BASE58CHECK_MAX_PAYLOAD or than outcap. */
int pz_base58check_decode(const char *s, uint8_t *out, size_t outcap);
/* Encodes payload (len <= PZ_BASE58CHECK_MAX_PAYLOAD bytes) with its checksum. Writes the NUL-terminated text to out, which holds
 * outcap bytes, and returns its length; returns -1 and writes an empty string if len is too large or the text does not fit. */
int pz_base58check_encode(const uint8_t *payload, size_t len, char *out, size_t outcap);

/* ---- bech32 and bech32m (BIP173, BIP350) ---- */

typedef enum { PZ_BECH32 = 1, PZ_BECH32M = 2 } pz_bech32_variant;

/* the longest bech32 string, and the longest human-readable part, the standards allow */
#define PZ_BECH32_MAX_LEN 90
#define PZ_BECH32_MAX_HRP 83

/* Encodes the 5-bit values data[0..n) behind the lower-case human-readable part hrp (1 to 83 characters of codes 33 to 126, upper-case
 * letters are lowered) with a checksum of the given variant. Writes the NUL-terminated string to out (outcap bytes) and returns
 * its length; returns -1 and writes an empty string if an argument is invalid, a value is above 31 or the result does not fit in
 * PZ_BECH32_MAX_LEN characters or in out. */
int pz_bech32_encode(char *out, size_t outcap, const char *hrp, const uint8_t *data, size_t n, pz_bech32_variant variant);
/* Decodes and verifies a bech32 or bech32m string. The human-readable part, lowered, goes to hrp (hrpcap bytes, with the NUL); the 5-bit
 * values without the checksum go to data, which holds *n values on entry and *n is the number of values on return. The checksum
 * variant that matched is stored in *variant. Returns false for anything the standards reject: a length over 90 or under 8, mixed case,
 * a character outside the alphabet or outside 33..126 in the human-readable part, an empty human-readable part, a missing
 * separator, a bad checksum. When it returns false nothing is stored. */
bool pz_bech32_decode(const char *s, char *hrp, size_t hrpcap, uint8_t *data, size_t *n, pz_bech32_variant *variant);

/* Regroups the bits of in[0..inlen) (frombits wide each, 1 to 8) into values of tobits bits (1 to 8), most significant first. With pad
 * the last value is filled with zero bits; without it, a leftover of more than frombits-1 bits or of non-zero bits is an error. Writes
 * to out, which holds *outlen values on entry, and stores the count in *outlen. Returns false on error, an input value that is too
 * wide, or out too small. */
bool pz_convert_bits(uint8_t *out, size_t *outlen, unsigned tobits, const uint8_t *in, size_t inlen, unsigned frombits, bool pad);

/* ---- SegWit addresses (BIP141, BIP173, BIP350) ---- */

/* Builds the address of a witness program: witness version 0 to 16, program of 2 to 40 bytes (20 or 32 for version 0), with the
 * bech32 checksum for version 0 and bech32m for the others. Writes the NUL-terminated text to out (outcap bytes) and returns its length,
 * or returns -1 and writes an empty string. */
int pz_segwit_address_encode(char *out, size_t outcap, const char *hrp, unsigned witver, const uint8_t *prog, size_t proglen);
/* Parses a SegWit address whose human-readable part is hrp (compared without regard to case, "bc" for the main network). Stores the
 * version in *witver and the program in prog, which holds *proglen bytes on entry (40 always suffice) and *proglen is the program length on
 * return. Rejects what the standards reject: the wrong checksum variant for the version, a program of the wrong size, padding that
 * is not zero, mixed case. Returns false and stores nothing on error. */
bool pz_segwit_address_decode(const char *hrp, const char *addr, unsigned *witver, uint8_t *prog, size_t *proglen);

/* ---- Bitcoin addresses and keys ---- */

/* Decodes a main-network address into the hash160 of its key: a P2PKH address ("1...", Base58Check, version 0) or a native SegWit
 * P2WPKH address ("bc1q...", bech32, witness version 0 with a 20-byte program). Everything else (P2SH, P2WSH, taproot, other networks,
 * bad checksums, mixed case) is rejected. */
bool pz_address_to_hash160(const char *addr, uint8_t h160[20]);
/* The P2PKH address of a hash160; the buffer must hold PZ_P2PKH_ADDRESS_BUF bytes. Returns false only if outcap is smaller. */
#define PZ_P2PKH_ADDRESS_BUF 36
bool pz_hash160_to_address(const uint8_t h160[20], char *out, size_t outcap);
/* The P2WPKH address of a hash160 for the human-readable part hrp ("bc" or "tb"); returns the length of the text or -1. */
int pz_hash160_to_segwit_address(const char *hrp, const uint8_t h160[20], char *out, size_t outcap);
/* Wallet Import Format of a private key: version 0x80, the 32 bytes, and 0x01 when the public key is compressed. The buffer must hold
 * PZ_WIF_BUF bytes. Returns false only if outcap is smaller. */
#define PZ_WIF_BUF 53
bool pz_privkey_to_wif(const uint8_t key_be[32], bool compressed, char *out, size_t outcap);

PZ_END_DECLS

#endif
