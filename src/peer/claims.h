/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* claims.h - a file of claims of the blocks of a shared linear search (docs/en/NETWORK.md 11): what a custodian keeps of the blocks whose
 * key it is a custodian of (claims.bin, checked by the node before they come here), and what a home keeps of its own (own_claims.bin). A
 * few owners a block, and another owner only with a proof key the claims held lack: the proof keys of a block are the same for whoever
 * scans it, so a claim copied under a new identity, which costs no scan, brings none. */
#ifndef PZPEER_CLAIMS_H
#define PZPEER_CLAIMS_H

#include "block.h"

#define CLAIMS_PER_BLOCK 4         /* owners of one block kept: more is a flood, not a search */
#define CLAIMS_MAX (1u << 21)      /* claims kept in all (about 1 GB of disk) */

/* what claims_put did, and the status of the answer to CLAIM: the first three leave the claim held */
enum { CLAIM_STORED, CLAIM_DUP, CLAIM_COPY, CLAIM_FULL, CLAIM_FAILED };

typedef struct claims claims;

/* Opened or made at path, for the network fp; NULL with the reason. A slot that cannot be read ends the file there (a write cut by a
 * crash): the owners send their claims again. */
claims *claims_open(const char *path, const uint8_t fp[32], char *err, size_t errlen);
void claims_close(claims *c);
/* A checked claim: CLAIM_STORED, CLAIM_DUP (the same owner had claimed the block), CLAIM_COPY (its proof keys are all in the claims held of
 * the block), CLAIM_FULL (CLAIMS_PER_BLOCK owners, or CLAIMS_MAX claims) or CLAIM_FAILED (the disk). */
int claims_put(claims *c, const block_claim *cl);
/* The claims of block index, at most max (and CLAIMS_PER_BLOCK). */
int claims_get(claims *c, uint64_t index, block_claim *out, int max);
/* The k-th claim kept, in the order they came; false past the end. */
bool claims_at(claims *c, uint64_t k, block_claim *out);
uint64_t claims_count(claims *c);

#endif
