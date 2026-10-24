/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* puzzles.h - built-in database of the 160 puzzles of the 1000 BTC challenge.
 * The table (puzzles_data.c) is generated from data/puzzles.csv and verified at build time
 * of the data (tools/gen_puzzles.py) and at run time ('pzsolve selftest'). */
#ifndef PZ_PUZZLES_H
#define PZ_PUZZLES_H

#include "common.h"

#define PZ_NUM_PUZZLES 160
/* Puzzles 161..256 were outputs of the same 2015 transaction; the creator withdrew their funds in 2017 (block 475240).
 * Only their addresses are kept, so that a search for them (an exercise: they hold nothing) needs no -a. */
#define PZ_FIRST_RETIRED 161
#define PZ_NUM_RETIRED 96

typedef struct {
    int n;                 /* puzzle number: the key lies in [2^(n-1), 2^n - 1] */
    const char *address;   /* P2PKH mainnet address (compressed key) */
    const char *pubkey;    /* compressed public key in hex; "" when never revealed */
    const char *privkey;   /* private key in hex; "" when the puzzle is still unsolved */
} puzzle_info;

extern const puzzle_info PZ_PUZZLES[PZ_NUM_PUZZLES];
extern const char *const PZ_RETIRED[PZ_NUM_RETIRED];

/* NULL when n is outside 1..160. */
const puzzle_info *puzzle_find(int n);
/* Address of a withdrawn puzzle (161..256), NULL otherwise. */
const char *puzzle_retired_address(int n);
/* Verifies the whole table (checksums, ranges, key -> address, pubkey -> address).
 * Returns 0 on success, otherwise the number of the first inconsistent puzzle. Includes the withdrawn ones. */
int puzzles_verify(char *err, size_t errlen);

#endif
