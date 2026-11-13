/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* keys.h - the keys of a home, all derived from its 32-byte seed for one network (docs/en/NETWORK.md 4): HKDF-SHA256 with the salt "pznet1"
 * and an info that names the use and binds the fingerprint F of the network file. */
#ifndef PZPEER_KEYS_H
#define PZPEER_KEYS_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_ID_ZERO_BITS 16       /* leading zero bits of a node ID on a real network (docs/en/NETWORK.md 7.1); tests use fewer */

typedef struct {
    uint8_t sec[32];               /* the secret of the identity: BIP340 key and Noise static key */
    uint8_t pub33[33];             /* its public key, compressed (Noise) */
    uint8_t xonly[32];             /* the identity on the network: records, sessions */
    uint8_t id[32];                /* node ID = SHA256("pznet1/id" || xonly) */
    uint32_t j;                    /* the counter that gave an ID with the leading zero bits */
    secp256k1_keypair kp;
} home_identity;

/* The identity of the home: the first j from 0 whose node ID has zero_bits leading zero bits, about 2^zero_bits derivations (65 000, a second
 * or two, for NODE_ID_ZERO_BITS). zero_bits is at most 32. */
/* The context of libsecp256k1 of the process, made at the first call and blinded with random bytes (its defence against side channels
 * on the secret keys it multiplies by). */
const secp256k1_context *keys_ctx(void);
bool keys_identity(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], unsigned zero_bits, home_identity *out);
/* The start key of pzsolve (hunt --start-key). */
void keys_starts(const uint8_t seed[32], const uint8_t fp[32], uint8_t out[32]);
/* The secret of the n-th tiny coin of the home (P2PKH). False only for a derived value that is not a valid key (never in practice). */
bool keys_coin(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], uint32_t n, uint8_t out[32]);
/* The node ID of an x-only identity, and whether it has the leading zero bits: one hash, the check of a peer that claims it. */
void keys_node_id(const uint8_t xonly[32], uint8_t id[32]);
bool keys_id_ok(const uint8_t id[32], unsigned zero_bits);

/* The files of the secrets of a home (home.seed, start.key): 64 hex digits and a newline. keys_file_read takes only a regular file of this
 * user that no one else may read or write (not a link): a secret others could read may be known already, so the file is left for its owner
 * to look at (chmod 600 it, if nobody else has read it). 1 read, 0 no such file, -1 refused or malformed, with the reason in err. */
int keys_file_read(const char *path, uint8_t out[32], char *err, size_t errlen);
/* Writes a secret to a new file of mode 0600 that then takes the place of path, synced with its folder: whatever was at path (a file
 * others could read, a link) is replaced, never kept or followed. */
bool keys_file_write(const char *path, const uint8_t v[32]);
/* The seed kept in path (home.seed, node.seed): read, or made the first time from several sources (pzcore/seed.h) and written. 1 read,
 * 2 made, 0 refused or failed, with the reason in err. */
int keys_seed_file(const char *path, uint8_t out[32], char *err, size_t errlen);

#endif
