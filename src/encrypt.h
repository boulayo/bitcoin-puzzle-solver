/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* encrypt.h - encrypting the key that a campaign finds to a public key, on top of pzcore/seal.h: the public key given by the user or derived from
 * a password that is typed, the randomness of the encryption, the associated data, the line that is written. The commands encrypt-keygen and
 * decrypt-key are in encrypt_cmd.c. The format and the reasons are in docs/en/ALGORITHMS.md; the use, in docs/en/USAGE.md. */
#ifndef PZ_ENCRYPT_H
#define PZ_ENCRYPT_H

#include "common.h"
#include <pzcore/seal.h>

#define ENC_PASSWORD_MAX 1024        /* bytes of a password (a longer one is refused) */
#define ENC_PASSWORD_ADVISED 12      /* a password with fewer characters gets a warning */

/* A public key as the options give it: 66 hexadecimal digits (compressed) or 130 (uncompressed), a point on the curve. On failure err is the reason. */
bool enc_parse_pubkey(const char *hex, pz_ge *q, u8 q33[33], char *err, size_t errn);
/* The key id of a public key as text: 8 hexadecimal digits (the first four bytes of its HASH160). */
void enc_key_id(const pz_ge *q, char id[9]);
/* The scrypt cost of the key from a password: 18 (N = 2^18, 256 MiB, about a second). Only PZ_TEST_SEAL_LOG2N changes it, and then a warning is
 * printed, once, on the error output. */
unsigned enc_log2n(void);
/* The associated data of the encryption: the address, a zero byte and the puzzle number in decimal (0 when there is none). Returns its length,
 * or -1 when it does not fit in cap. */
int enc_aad(u8 *out, size_t cap, const char *address, int puzzle);
/* A random scalar in [1, n - 1] from the operating system. False when the system gives no randomness. */
bool enc_random_scalar(pz_u256 *r);
/* Whether a key can be encrypted to q here: the system's random numbers work and a test encryption is produced and opened again. */
bool enc_check(const pz_ge *q, char *err, size_t errn);
/* Encrypts the 32 bytes of key to q for the address and puzzle; text (cap bytes, PZ_SEAL_TEXT_BUF is enough) gets the "PZENC1:..." line.
 * False when it could not be done (the system gave no randomness): then text is empty. */
bool enc_seal_key(const pz_ge *q, const u8 key[32], const char *address, int puzzle, char *text, size_t cap);

/* Derives the secret scalar of a password: allocates and locks the 256 MiB of scrypt, wipes and frees them. False if the memory cannot be had. */
bool enc_key_from_password(pz_u256 *d, const char *pw, size_t pwlen);
/* Reads a password (twice and compared when standard input is a terminal; one line otherwise), refuses an empty one, warns about a short one, derives
 * the public key q (and q33), wipes the password and the scalar. This is what encrypt-keygen does, and `hunt --encrypt-password`
 * (hunt = true), which adds the notice about the trust that the machine needs. Exits with a message when the password cannot be had. */
void enc_password_to_pubkey(bool hunt, pz_ge *q, u8 q33[33]);
/* The advice on the password, for the commands that make a key. */
void enc_print_password_advice(FILE *f);

#endif
