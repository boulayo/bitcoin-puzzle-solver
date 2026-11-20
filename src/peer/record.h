/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* record.h - the record of a distinguished point (docs/en/NETWORK.md 7.2): its type, the zero bits of the walk that found it, the point and
 * its owner; and the proof that the owner knows the distance of the point. The point is R = t*Q' + d*G with Q' = P - base*G, t in {0, +1,
 * -1}, R the representative with even y. A record carries no signature: a custodian takes a record only from its owner, over a channel
 * that authenticated it, and nothing comes of a record without the owner's proof of possession. */
#ifndef PZPEER_RECORD_H
#define PZPEER_RECORD_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stdint.h>

#define RECORD_LEN 66              /* type, zero bits, x, owner: in alerts, mailboxes, sessions and proofs */
#define RECORD_WIRE 34             /* the same without the owner: in a STORE, whose sender is the owner */
#define RECORD_DP_MAX 64           /* the most zero bits a record may claim */
#define RECORD_DP_MORE 8           /* zero bits a home may add to the network's (home.c): a record claims at most these more */

typedef struct {
    int t;                         /* 0 tame, +1 or -1 wild */
    unsigned dp;                   /* the zero bits of the distinguished points of the walk that found it: the network's or more */
    uint8_t x[32];                 /* x of R */
    uint8_t owner[32];             /* x-only identity of the owner */
} record;

/* What a network gives to the checks: its fingerprint, the least zero bits of its distinguished points and Q' (the puzzle key less
 * base*G). */
typedef struct {
    uint8_t fp[32];
    unsigned dpbits;
    secp256k1_pubkey qp;
} record_net;

/* Q' from the public key of the puzzle (33 bytes) and the start of the range (32 bytes, big-endian). False when base*G = P (the key is base). */
bool record_net_init(const secp256k1_context *ctx, record_net *net, const uint8_t fp[32], unsigned dpbits, const uint8_t pubkey33[33],
                     const uint8_t base32[32]);

/* The type, the zero bits, x and the owner; the first RECORD_WIRE bytes are a record in a STORE. */
void record_encode(const record *r, uint8_t out[RECORD_LEN]);
/* The bytes that name a record wherever it counts (the hello, the name of a pair, a proof, the session): its encoding with the zero bits
 * at 0. Two custodians may hold one point of one owner with other zero bits (the first copy each got), and it is the same record. */
void record_name(const record *r, uint8_t out[RECORD_LEN]);
bool record_same(const uint8_t a[RECORD_LEN], const uint8_t b[RECORD_LEN]);   /* two encodings, whatever their zero bits */
/* Format only: the type and the zero bits. */
bool record_decode(const uint8_t in[RECORD_LEN], record *r);
/* Whether x has the zero bits of a distinguished point: bits 64 to 64 + dpbits - 1, as pzsolve tests x.n[1]. */
bool record_dp_ok(const uint8_t x[32], unsigned dpbits);
/* The checks on arrival: x on the curve with the zero bits it claims, at least the network's, and an owner that is a key. */
bool record_check(const secp256k1_context *ctx, const record_net *net, const record *r);

/* d*G = R - t*Q', the point whose scalar the owner knows; false at infinity (d = 0) or for an x off the curve. */
bool record_dpoint(const secp256k1_context *ctx, const record_net *net, const record *r, secp256k1_pubkey *out);

/* The proof of possession (docs/en/NETWORK.md 7.2): a BIP340 signature by the scalar d of R - t*Q', over the record, the identity of the asker
 * and a nonce of the asker. d32 is d mod n, big-endian, with R = t*Q' + d*G for the even-y R of the record. */
bool record_prove(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t d32[32], const uint8_t asker[32],
                  const uint8_t nonce[32], uint8_t sig[64]);
bool record_proof_ok(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t asker[32], const uint8_t nonce[32],
                     const uint8_t sig[64]);

#endif
