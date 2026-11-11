/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* block.h - the claim of a block of a shared linear search (docs/en/NETWORK.md 11): "I tried every key of block N", with its proof of
 * work. The range of the network is cut into blocks of 2^bits keys. Among the keys of a block, about one in 2^40 has a hash160 with
 * bytes 8 to 12 of the target's: pzsolve finds them on its way (hunt --near gives those with bytes 8 to 11, of which one in 256 has byte
 * 12 too) and nobody knows them without trying the keys. A claim lists them and is signed by its owner; whoever reads it hashes those
 * few keys and knows the block was scanned, or mostly scanned: a claim cannot show that no key was skipped, only that most were tried. */
#ifndef PZPEER_BLOCK_H
#define PZPEER_BLOCK_H

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_PROOFS_MAX 64        /* proof keys in a claim: four times what a block of 2^44 keys has on average */
#define BLOCK_BITS_MIN 40          /* a smaller block would have no proof key, most of the time */
#define BLOCK_BITS_MAX 48          /* the offset of a key in its block is 6 bytes */
#define BLOCK_CLAIM_MAX (8 + 32 + 1 + 6 * BLOCK_PROOFS_MAX + 64)

/* What a linear network gives to the checks. */
typedef struct {
    uint8_t fp[32];                /* the fingerprint of the network */
    uint8_t h160[20];              /* the target */
    uint8_t start[32];             /* the first key of block 0, big-endian */
    unsigned bits;                 /* a block is 2^bits keys */
} block_net;

typedef struct {
    uint64_t index;                /* the block */
    uint8_t owner[32];             /* x-only identity of who scanned it */
    int n;                         /* proof keys */
    uint64_t off[BLOCK_PROOFS_MAX]; /* their offsets in the block, increasing */
    uint8_t sig[64];
} block_claim;

/* The fewest proof keys a claim may have: a quarter of the average, 1 at least. An honest scan has fewer once in a thousand blocks, and
 * that block is scanned again by someone else. */
int block_min_proofs(unsigned bits);
/* Whether key (32 bytes, big-endian) is a proof key of the network: bytes 8 to 12 of its hash160 are the target's. */
bool block_proof_key(const secp256k1_context *ctx, const block_net *net, const uint8_t key[32]);
/* The claim of a block from the near misses of its scan (nkeys keys of 32 bytes: the lines of near.txt, in any order, with repeats and
 * keys of other blocks): its proof keys, sorted, signed by id. False with fewer than the minimum, or more than a claim holds. */
bool block_claim_make(const secp256k1_context *ctx, const block_net *net, const secp256k1_keypair *id, uint64_t index,
                      const uint8_t (*keys)[32], size_t nkeys, block_claim *out);
/* The checks on arrival: the signature, the offsets increasing and inside the block, every key a proof key, enough of them. */
bool block_claim_check(const secp256k1_context *ctx, const block_net *net, const block_claim *c);
size_t block_claim_encode(const block_claim *c, uint8_t out[BLOCK_CLAIM_MAX]);
bool block_claim_decode(const uint8_t *in, size_t len, block_claim *c);   /* format only */

#endif
