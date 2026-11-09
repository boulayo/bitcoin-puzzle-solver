/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* spv.h - the light verification of the chain (docs/en/NETWORK.md 8.2): a chain of block headers from a checkpoint, each one checked (the
 * hash of the one before, its proof of work, its target, its time), and the Merkle proof that a transaction is in a block of that chain.
 * Nothing a server says is believed: a header costs its proof of work, a transaction its proof. No I/O but spv_load and spv_save. */
#ifndef PZPEER_SPV_H
#define PZPEER_SPV_H

#include "btc.h"

#define SPV_HEADER 80
#define SPV_PERIOD 2016            /* blocks between two changes of the target */
#define SPV_BRANCH_MAX 32          /* hashes of a Merkle branch */

typedef struct {
    btc_chain chain;
    uint64_t base;                 /* the height of the first header kept: the checkpoint */
    uint8_t cp[32];                /* its hash */
    uint8_t *hdr;                  /* n headers of SPV_HEADER bytes */
    size_t n, cap;
} spv_chain;

/* The hash of the genesis block of a chain, in internal order: the checkpoint of height 0. */
void spv_genesis(btc_chain chain, uint8_t hash[32]);
/* An empty chain that starts at the header of height `base` with the hash `hash` (internal order). On the main chain base is a multiple of
 * SPV_PERIOD, so that every later target can be checked. */
bool spv_init(spv_chain *c, btc_chain chain, uint64_t base, const uint8_t hash[32]);
void spv_free(spv_chain *c);
/* count headers that start at height `at` (the first is the checkpoint itself on an empty chain; else at is at most the tip + 1). Each is
 * checked against the ones before it. They extend the chain, or, when they leave it below its tip, replace its end if they hold more work.
 * False, and nothing changed, when one is invalid or does not connect. now: the time of this computer, for headers from the future. */
bool spv_add(spv_chain *c, uint64_t at, const uint8_t *hdrs, size_t count, uint64_t now);
/* The height of the tip; false while the chain is empty. */
bool spv_tip(const spv_chain *c, uint64_t *height);
/* The header of a height, NULL when it is not kept. */
const uint8_t *spv_header(const spv_chain *c, uint64_t height);
/* Whether the transaction txid (internal order) is at position pos of the block at `height`: the branch (n hashes, internal order, from
 * the leaf up) must lead to the Merkle root of that header. */
bool spv_proof(const spv_chain *c, uint64_t height, const uint8_t txid[32], const uint8_t (*branch)[32], size_t n, uint32_t pos);
/* The chain in a file, so that a restart does not fetch it again. A file that does not fit the checkpoint is ignored. */
bool spv_save(const spv_chain *c, const char *path);
bool spv_load(spv_chain *c, const char *path, uint64_t now);

/* For the tests: the target of compact bits (false when negative or overflowing), and the bits after a period that took `timespan` seconds. */
bool spv_target(uint32_t bits, uint8_t target[32]);
uint32_t spv_retarget(uint32_t bits, int64_t timespan);

#endif
