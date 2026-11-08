/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* record.h - the record of a distinguished point (docs/en/NETWORK.md 7.2): 138 bytes, signed by its owner, and the proof that the owner knows
 * the distance of the point. The point is R = t*Q' + d*G with Q' = P - base*G, t in {0, +1, -1}, R the representative with even y. */
#ifndef PZPEER_RECORD_H
#define PZPEER_RECORD_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stdint.h>

#define RECORD_LEN 138
#define RECORD_BODY 74             /* the bytes the owner signs: all but the signature */
#define RECORD_VERSION 1

typedef struct {
    int t;                         /* 0 tame, +1 or -1 wild */
    uint8_t x[32];                 /* x of R */
    uint8_t owner[32];             /* x-only identity of the owner */
    uint64_t seq;                  /* strictly increasing for an owner */
    uint8_t sig[64];
} record;

/* What a network gives to the checks: its fingerprint, the bits of its distinguished points and Q' (the puzzle key less base*G). */
typedef struct {
    uint8_t fp[32];
    unsigned dpbits;
    secp256k1_pubkey qp;
} record_net;

/* Q' from the public key of the puzzle (33 bytes) and the start of the range (32 bytes, big-endian). False when base*G = P (the key is base). */
bool record_net_init(const secp256k1_context *ctx, record_net *net, const uint8_t fp[32], unsigned dpbits, const uint8_t pubkey33[33],
                     const uint8_t base32[32]);

void record_encode(const record *r, uint8_t out[RECORD_LEN]);
/* Format only: the version and the type. */
bool record_decode(const uint8_t in[RECORD_LEN], record *r);
/* Whether x has the zero bits of a distinguished point: bits 64 to 64 + dpbits - 1, as pzsolve tests x.n[1]. */
bool record_dp_ok(const uint8_t x[32], unsigned dpbits);
/* Signs r (all its fields but sig) with the identity of the owner, whose x-only key must be r->owner. */
bool record_sign(const secp256k1_context *ctx, const record_net *net, const secp256k1_keypair *id, record *r);
/* The checks on arrival: x on the curve with the zero bits, and the signature of the owner. */
bool record_check(const secp256k1_context *ctx, const record_net *net, const record *r);

/* The proof of possession (docs/en/NETWORK.md 7.2): a BIP340 signature by the scalar d of R - t*Q', over the record, the identity of the asker
 * and a nonce of the asker. d32 is d mod n, big-endian, with R = t*Q' + d*G for the even-y R of the record. */
bool record_prove(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t d32[32], const uint8_t asker[32],
                  const uint8_t nonce[32], uint8_t sig[64]);
bool record_proof_ok(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t asker[32], const uint8_t nonce[32],
                     const uint8_t sig[64]);

#endif
