/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* claims.h - what a custodian keeps of the claims of the blocks of a shared linear search (docs/en/NETWORK.md 11): the claims of the blocks
 * whose key it is a custodian of, on disk (claims.bin), a few owners a block, checked by the node before they come here. */
#ifndef PZPEER_CLAIMS_H
#define PZPEER_CLAIMS_H

#include "block.h"

#define CLAIMS_PER_BLOCK 4         /* owners of one block kept: more is a flood, not a search */
#define CLAIMS_MAX (1u << 21)      /* claims kept in all (about 1 GB of disk) */

enum { CLAIM_STORED, CLAIM_DUP, CLAIM_FULL, CLAIM_FAILED };

typedef struct claims claims;

/* Opened or made at path, for the network fp; NULL with the reason. */
claims *claims_open(const char *path, const uint8_t fp[32], char *err, size_t errlen);
void claims_close(claims *c);
/* A checked claim: CLAIM_STORED, CLAIM_DUP (the same owner had claimed the block), CLAIM_FULL or CLAIM_FAILED (the disk). */
int claims_put(claims *c, const block_claim *cl);
/* The claims of block index, at most max. */
int claims_get(claims *c, uint64_t index, block_claim *out, int max);
uint64_t claims_count(claims *c);

#endif
