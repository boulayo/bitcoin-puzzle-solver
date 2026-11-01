/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seal.h - encrypting a 32-byte secret (a private key) to a public key, and the key derivation functions around it.
 *
 * Two layers. The primitives (kdf.c): HMAC-SHA256, HKDF-SHA256, PBKDF2-HMAC-SHA256, scrypt, a comparison whose time does not depend on where
 * the bytes differ, and a wipe that the compiler cannot remove. The construction (seal.c): ECIES on secp256k1 built on them, in the format
 * that pzsolve writes when it finds a key on a computer that is not its owner's (docs/en/ALGORITHMS.md has the construction and the reasons).
 *
 * Nothing here allocates, reads the clock or the system's random numbers, or does I/O: the caller passes the random scalar of the
 * encryption and, for scrypt, the work buffer. Everything is thread-safe. The scalar multiplications are those of ec.h and are not
 * constant-time: opening a seal on a machine where someone measures the time of the operation is not a use this library protects.
 * The functions that handle secrets clear their own temporaries; the buffers the caller passes are the caller's to clear (pz_secure_zero). */
#ifndef PZCORE_SEAL_H
#define PZCORE_SEAL_H

#include <pzcore/ec.h>

PZ_BEGIN_DECLS

/* ---- wiping and comparing ---- */

/* Sets n bytes to zero in a way the compiler cannot drop as a dead store (a memset before free() or before the end of the scope may be removed). */
void pz_secure_zero(void *p, size_t n);
/* Whether a[0..n) and b[0..n) are equal; the time depends on n only, not on the position of the first difference. */
bool pz_ct_equal(const void *a, const void *b, size_t n);

/* ---- HMAC-SHA256 (RFC 2104, RFC 4231) ---- */

/* The state of an HMAC computed piece by piece. in is the inner hash, running; out is the state of the outer hash after its key block. Keys of any
 * length are accepted (a key over 64 bytes is hashed first). Declared here so that it can live on the caller's stack. */
typedef struct {
    uint32_t in[8], out[8];
    uint8_t buf[64];
    uint64_t len;                  /* bytes of the message so far (the key block of the inner hash not counted) */
} pz_hmac_sha256;

void pz_hmac_sha256_init(pz_hmac_sha256 *c, const uint8_t *key, size_t keylen);
void pz_hmac_sha256_update(pz_hmac_sha256 *c, const uint8_t *data, size_t len);
/* Writes the tag and wipes the state. */
void pz_hmac_sha256_final(pz_hmac_sha256 *c, uint8_t out[32]);
/* One call; data may be NULL when len is 0. */
void pz_hmac_sha256_oneshot(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[32]);

/* ---- HKDF-SHA256 (RFC 5869) ---- */

#define PZ_HKDF_SHA256_MAX_OUT (255 * 32)
/* Extract and expand in one call. salt may be empty (then the salt is 32 zero bytes, as in the RFC), info too. Writes okmlen bytes (at most
 * PZ_HKDF_SHA256_MAX_OUT) and returns true; false, with okm untouched, when okmlen is larger. */
bool pz_hkdf_sha256(const uint8_t *salt, size_t saltlen, const uint8_t *ikm, size_t ikmlen, const uint8_t *info, size_t infolen,
                    uint8_t *okm, size_t okmlen);

/* ---- PBKDF2-HMAC-SHA256 (RFC 8018, RFC 7914) ---- */

/* Writes outlen bytes. False, with out untouched, when iter is 0 or outlen exceeds (2^32 - 1) * 32. */
bool pz_pbkdf2_sha256(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t saltlen, uint32_t iter, uint8_t *out, size_t outlen);

/* ---- scrypt (RFC 7914) ---- */

/* The size of the work buffer pz_scrypt needs: 128 * r * (N + p + 2) bytes (256 MiB and 3 KiB for N = 2^18, r = 8, p = 1). 0 when the
 * parameters are not valid (N not a power of two or below 2, r or p zero, r * p at or above 2^30, a size that does not fit). */
size_t pz_scrypt_work_size(uint64_t N, uint32_t r, uint32_t p);
/* Derives outlen bytes (at most 2^32 - 1 times 32). work holds worksize bytes, at least pz_scrypt_work_size and aligned to 8 bytes (malloc
 * and mmap give that); its contents on return are the key material of the computation: the caller wipes it (pz_secure_zero) before
 * it gives it back. False, with out untouched, for invalid parameters or a work buffer that is too small or misaligned. */
bool pz_scrypt(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t saltlen, uint64_t N, uint32_t r, uint32_t p,
               uint8_t *out, size_t outlen, void *work, size_t worksize);

/* ---- the seal ---- */

/* The format. A payload is 86 bytes: version (1), key id (4), R (33, compressed), ciphertext (32), tag (16). Its text form is
 * "PZENC1:" followed by the Base58Check of the payload (the four check bytes catch a mistyped or cut copy before any secret is asked for). */
#define PZ_SEAL_VERSION 1
#define PZ_SEAL_PAYLOAD_LEN 86
#define PZ_SEAL_TEXT_PREFIX "PZENC1:"
#define PZ_SEAL_TEXT_PREFIX_LEN 7
#define PZ_SEAL_TEXT_BUF 160            /* holds the prefix, the Base58Check text of a payload and the NUL */
#define PZ_SEAL_SCRYPT_LOG2N 18         /* the cost of the key from a password: N = 2^18, r = 8, p = 1 (256 MiB, about a second) */
#define PZ_SEAL_SCRYPT_R 8
#define PZ_SEAL_SCRYPT_P 1

typedef enum {
    PZ_SEAL_OK = 0,
    PZ_SEAL_E_ARG,                      /* an argument out of range: scalar 0 or not below n, a point off the curve */
    PZ_SEAL_E_FORMAT,                   /* the text is not a seal: no prefix, a character outside the Base58 alphabet, a bad checksum, a wrong length */
    PZ_SEAL_E_VERSION,                  /* a version byte this library does not know */
    PZ_SEAL_E_POINT,                    /* R is not a valid compressed point */
    PZ_SEAL_E_KEYID,                    /* the key id is not that of d*G: sealed to another key (wrong password) */
    PZ_SEAL_E_TAG                       /* the authentication failed: the payload was changed */
} pz_seal_status;

/* d = (b mod (n - 1)) + 1, with b read as a big-endian integer: a uniform scalar in [1, n - 1] from 32 uniform bytes (no bias worth the name:
 * n - 1 is within 2^-128 of 2^256). */
void pz_seal_scalar_from_bytes(pz_u256 *d, const uint8_t b[32]);
/* The key of a password: d = scalar_from_bytes(scrypt(password, salt = "pzsolve-found-v1", N = 2^log2n, r = 8, p = 1, dkLen = 32)). The salt is the same for
 * everybody, so the security rests on the password alone: it must be strong (the cost only slows the guesses down). log2n is
 * PZ_SEAL_SCRYPT_LOG2N for every real use (a smaller value is for tests). work holds worksize bytes, at least pz_scrypt_work_size(2^log2n, 8, 1);
 * the caller wipes it afterwards. False for log2n outside 1..40 or a work buffer that is too small. */
bool pz_seal_key_from_password(pz_u256 *d, const uint8_t *pw, size_t pwlen, unsigned log2n, void *work, size_t worksize);
/* The four bytes that name a public key in a payload: the first four of the HASH160 of its compressed form. */
void pz_seal_key_id(const pz_ge *q, uint8_t id[4]);

/* Encrypts the 32 bytes of plain to the public key q. r is the scalar of this encryption, in [1, n - 1], from a good random source and used
 * once: the caller draws it. The payload needs nothing but the secret to be opened: there is no associated data. Returns PZ_SEAL_OK, or
 * PZ_SEAL_E_ARG with out untouched. */
pz_seal_status pz_seal(uint8_t out[PZ_SEAL_PAYLOAD_LEN], const pz_ge *q, const uint8_t plain[32], const pz_u256 *r);

/* What can be read of a payload without the secret: its version and key id. PZ_SEAL_OK, PZ_SEAL_E_VERSION or PZ_SEAL_E_POINT (R is checked). */
pz_seal_status pz_seal_peek(const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], uint8_t keyid[4]);
/* Opens a payload with the private scalar d. The order of the checks is the order of the statuses: version, R on the curve, d in range,
 * key id of d*G, tag (compared in constant time). plain is written only on PZ_SEAL_OK. */
pz_seal_status pz_seal_open(uint8_t plain[32], const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], const pz_u256 *d);

/* The text form. pz_seal_encode writes "PZENC1:..." and the NUL into out (outcap bytes, PZ_SEAL_TEXT_BUF is enough) and returns the length, or
 * -1. pz_seal_decode reads one such token (the string must be exactly the token, no blank around it) and gives PZ_SEAL_OK or PZ_SEAL_E_FORMAT;
 * the version is not looked at there (pz_seal_peek). */
int pz_seal_encode(const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], char *out, size_t outcap);
pz_seal_status pz_seal_decode(uint8_t payload[PZ_SEAL_PAYLOAD_LEN], const char *token);

PZ_END_DECLS

#endif
