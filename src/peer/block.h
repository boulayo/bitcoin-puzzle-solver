/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* block.h - the claim of a block of a shared linear search (docs/en/NETWORK.md 11): "I tried every key of block N", with its proof of
 * work. The range of the network is cut into blocks of 2^bits keys. The proof keys of a block are those whose hash160 has bytes 8 to 11
 * of the target's (blocks of 34 to 37 bits: one key in 2^32, every near miss of hunt --near) or bytes 8 to 12 (blocks of 42 to 45 bits:
 * one in 2^40, a near miss in 256), so a block has 16 of them on average at 36 and 44 bits (4 at 34, for tests): nobody knows them
 * without trying the keys, and they are the same for whoever scans the block.
 * A claim lists them and is signed by its owner; whoever reads it hashes those few keys and knows the block was scanned, or mostly
 * scanned: a claim cannot show that no key was skipped, only that at least half of the keys were tried, on average. */
#ifndef PZPEER_BLOCK_H
#define PZPEER_BLOCK_H

#include <pzcore/u256.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_PROOFS_MAX 64        /* proof keys in a claim: four times what a block of 2^44 keys has on average */
#define BLOCK_CLAIM_KEY_TAG "pznet1/blockkey"   /* the key of the custodians of block i: SHA256(tag || F || i, 8 bytes big-endian) */
#define BLOCK_CLAIM_MAX (8 + 32 + 1 + 6 * BLOCK_PROOFS_MAX + 64)

/* What a linear network gives to the checks. */
typedef struct {
    uint8_t fp[32];                /* the fingerprint of the network */
    uint8_t h160[20];              /* the target */
    uint8_t start[32];             /* the first key of block 0, big-endian */
    uint8_t end[32];               /* the last key of the range: the last block may be short */
    unsigned bits;                 /* a block is 2^bits keys */
} block_net;

typedef struct {
    uint64_t index;                /* the block */
    uint8_t owner[32];             /* x-only identity of who scanned it */
    int n;                         /* proof keys */
    uint64_t off[BLOCK_PROOFS_MAX]; /* their offsets in the block, increasing */
    uint8_t sig[64];
} block_claim;

/* Whether blocks of 2^bits keys can be claimed: 34 to 37, or 42 to 45 (fewer would often have no proof key, more too many for a claim). */
bool block_bits_ok(unsigned bits);
/* The fewest proof keys a claim may have: half the average, so that a false claim costs half a block. An honest scan has fewer in about
 * one block in a hundred at 36 and 44 bits (one in ten at 34 and 42, one in a thousand at 37 and 45): that block cannot be claimed, and
 * others may scan it again. */
int block_min_proofs(unsigned bits);
/* The blocks of the range (UINT64_MAX when there are more). */
uint64_t block_count(const block_net *net);
/* The first and the last key of block index; false past the end of the range. */
bool block_range(const block_net *net, uint64_t index, pz_u256 *first, pz_u256 *last);
/* The key of the custodians of the claims of block index. */
void block_key(const block_net *net, uint64_t index, uint8_t key[32]);
/* Whether key (32 bytes, big-endian) is a proof key of the network: bytes 8 to 11, or 8 to 12 (above), of its hash160 are the target's. */
bool block_proof_key(const secp256k1_context *ctx, const block_net *net, const uint8_t key[32]);
/* The claim of a block from the near misses of its scan (nkeys keys of 32 bytes: the lines of near.txt, in any order, with repeats and
 * keys of other blocks): its proof keys, sorted (BLOCK_PROOFS_MAX of them at most: as many prove as much), signed by id. It may have
 * fewer than block_min_proofs, and then no custodian takes it. False only for a block past the range. */
bool block_claim_make(const secp256k1_context *ctx, const block_net *net, const secp256k1_keypair *id, uint64_t index,
                      const uint8_t (*keys)[32], size_t nkeys, block_claim *out);
/* The checks on arrival: the signature, the offsets increasing and inside the block, every key a proof key, enough of them. */
bool block_claim_check(const secp256k1_context *ctx, const block_net *net, const block_claim *c);
/* The distinct proof keys that n claims of one block show together. */
int block_claims_keys(const block_claim *c, int n);
size_t block_claim_encode(const block_claim *c, uint8_t out[BLOCK_CLAIM_MAX]);
bool block_claim_decode(const uint8_t *in, size_t len, block_claim *c);   /* format only */

#endif
