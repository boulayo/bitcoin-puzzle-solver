/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* keys.h - the keys of a home, all derived from its 32-byte seed for one network (docs/en/NETWORK.md 4): HKDF-SHA256 with the salt "pznet1"
 * and an info that names the use and binds the fingerprint F of the network file. */
#ifndef PZPEER_KEYS_H
#define PZPEER_KEYS_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
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
bool keys_identity(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], unsigned zero_bits, home_identity *out);
/* The start key of pzsolve (hunt --start-key). */
void keys_starts(const uint8_t seed[32], const uint8_t fp[32], uint8_t out[32]);
/* The secret of the n-th tiny coin of the home (P2PKH). False only for a derived value that is not a valid key (never in practice). */
bool keys_coin(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], uint32_t n, uint8_t out[32]);
/* The node ID of an x-only identity, and whether it has the leading zero bits: one hash, the check of a peer that claims it. */
void keys_node_id(const uint8_t xonly[32], uint8_t id[32]);
bool keys_id_ok(const uint8_t id[32], unsigned zero_bits);

#endif
