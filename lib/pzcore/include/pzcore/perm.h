/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* perm.h - keyed pseudo-random permutation of [0, M) (format-preserving
 * Feistel network with SHA-256 round functions and cycle walking).
 *
 * This is what makes every campaign visit the search space in its own order:
 * the chunk index sequence 0,1,2,... is mapped through a bijection that only
 * depends on the 256-bit campaign seed. Evaluating it costs a handful of
 * SHA-256 calls, negligible compared with the 2^32 keys inside a chunk. */
#ifndef PZ_PERM_H
#define PZ_PERM_H

#include <pzcore/u256.h>

#define PZ_PERM_ROUNDS 8

typedef struct {
    u8 seed[32];
    pz_u256 m;            /* domain size (>= 1) */
    unsigned bits;     /* bit length of m-1 */
    unsigned wl, wr;   /* initial left/right widths */
    bool swap1;        /* used when bits == 1 */
} pz_perm_t;

void pz_perm_init(pz_perm_t *p, const u8 seed[32], const pz_u256 *m);
pz_u256 pz_perm_apply(const pz_perm_t *p, const pz_u256 *x);    /* x < m  ->  y < m */
pz_u256 pz_perm_invert(const pz_perm_t *p, const pz_u256 *y);

#endif
