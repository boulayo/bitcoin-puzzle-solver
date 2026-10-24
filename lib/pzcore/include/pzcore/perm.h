/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* perm.h - a keyed pseudo-random permutation of [0, M): a bijection of the integers below M onto themselves that depends only on a
 * 256-bit seed and on M, can be evaluated in either direction in expected constant time per call (no table, no walk over the domain).
 *
 * It is a format-preserving Feistel network: the numbers of 2^bits >= M are split in two halves and mixed by 8 rounds whose round
 * function is SHA-256 of (a fixed tag, the seed, the width, the round number, the half); the numbers that land at M or above are sent
 * through the network again ("cycle walking"), which keeps it a bijection of [0, M). The result for a given (seed, M, x) is fixed: it is
 * the same on every architecture, byte order and compiler and in every version of the library (the tests pin 380 values produced by
 * pzsolve 1.8.1), so an order that was stored or relied upon stays valid. (pzsolve uses it to visit the blocks of a search range in an
 * order that depends on the campaign seed.)
 *
 * Cost: one pass of the network is 8 SHA-256 evaluations of two blocks each (a few microseconds). A pass sends a number outside the
 * domain with probability below 1/2, and a number outside is passed again, so a call takes fewer than 2 passes on average and more
 * than k passes with probability below 2^-k. M may be anything from 1 to 2^192. This is not a cipher for secrets: 8 Feistel rounds with an
 * unkeyed hash mix the numbers for ordering work, they do not encrypt. A pz_perm is not modified after pz_perm_init, so any number of threads
 * may share one. */
#ifndef PZCORE_PERM_H
#define PZCORE_PERM_H

#include <pzcore/u256.h>

PZ_BEGIN_DECLS

#define PZ_PERM_ROUNDS 8
/* the largest M the permutation takes is 2^PZ_PERM_MAX_BITS */
#define PZ_PERM_MAX_BITS 192

/* treat as opaque */
typedef struct {
    uint8_t seed[32];
    pz_u256 m;            /* domain size (>= 1) */
    unsigned bits;        /* bit length of m-1 */
    unsigned wl, wr;      /* initial left/right widths */
    bool swap1;           /* used when bits == 1 */
} pz_perm;

/* Prepares the permutation of [0, m) for the given seed. Returns false, and leaves *p unspecified, if m is 0 or above 2^192. */
bool pz_perm_init(pz_perm *p, const uint8_t seed[32], const pz_u256 *m);
/* y = the image of x, and back: x = the number whose image is y. Both return false, and leave *y untouched, if the argument is not below
 * m (the walk of a number outside the domain would not end). y may alias x. */
bool pz_perm_apply(const pz_perm *p, const pz_u256 *x, pz_u256 *y);
bool pz_perm_invert(const pz_perm *p, const pz_u256 *y, pz_u256 *x);

PZ_END_DECLS

#endif
