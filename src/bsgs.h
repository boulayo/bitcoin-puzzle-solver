/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs.h - baby-step giant-step with a symmetric RAM table, for ranges whose public key is known.
 *
 * Unknown key k in [lo, hi], W = hi - lo + 1, target P = k*G.  Let c = lo + (W-1)/2, d = k - c, |d| <= H = ceil(W/2).
 *   Baby steps:  the x-coordinates of b*G for b = 1..T go into a hash table (b and -b share x, so one entry covers both).
 *   Giant steps: S = 2T+1;  for a = -A..A  look up the x-coordinate of Q_a = (P - c*G) - a*S*G.  A hit gives b with
 *                Q_a = +-b*G, hence k = c + a*S +- b.
 * Every one of the (2A+1)*S >= 2H+1 values of d is covered exactly once, so the search is exhaustive and deterministic:
 * with the giants visited in a random order the expected cost is T + W/(4T) group operations, minimal (1.0*sqrt(W))
 * for T = sqrt(W)/2, against 1.06*sqrt(W) (KANG_C_MEAN) for the kangaroo, at the price of 8/0.65 ~ 12 bytes of RAM per baby step.
 * The table does not depend on the target nor on the range: it can be saved to disk and reused. */
#ifndef PZ_BSGS_H
#define PZ_BSGS_H

#include "campaign.h"
#include <stdatomic.h>

#define BS_HALF 512                 /* points on each side of a group centre (one batch inversion per group) */
#define BS_GRP (2 * BS_HALF)
#define BS_MAX_T (((u64)1 << 40) - 1)   /* the baby index is stored in 40 bits */
#define BS_LOAD_FACTOR 0.65

typedef struct {
    u64 *slots;          /* (tag:24 | b:40); 0 = empty */
    u64 nslots;
    u64 T;               /* baby steps stored: b = 1..T */
    void *map;           /* mmap'd region (slots lives inside it) */
    size_t map_bytes;
    bool thp;            /* transparent huge pages accepted by the kernel */
} bsgs_table;

typedef struct {
    pz_u256 lo, hi, W;      /* key range and its size */
    pz_u256 c;              /* centre key, lo + (W-1)/2 */
    u64 T, S;            /* baby steps, giant stride S = 2T+1 */
    u64 A, Ng;           /* giants a in [-A, A], Ng = 2A+1 */
    pz_ge P;                /* target public key */
    pz_ge Pp;               /* P - c*G, valid unless Pp_inf */
    bool Pp_inf;         /* P == c*G: the key is c */
} bsgs_params;

/* ---- sizing ---- */
u64 bsgs_slots_for(u64 T);
u64 bsgs_bytes_for(u64 T);
u64 bsgs_max_T_for_bytes(u64 bytes);
/* ceil(sqrt(W)/2), the table size minimising the mean cost; 0 when it exceeds BS_MAX_T. */
u64 bsgs_optimal_T(const pz_u256 *W);
/* Mean number of group operations (builds + giants) for table size T on a range of W keys. */
long double bsgs_mean_cost(const pz_u256 *W, u64 T);
/* Table size to use given a memory budget; *viable tells whether it beats the kangaroo (KANG_C_MEAN*sqrt(W), 1.06). */
u64 bsgs_choose_T(const pz_u256 *W, u64 budget_bytes, bool *viable);

/* ---- parameters of a search ---- */
bool bsgs_params_init(bsgs_params *p, const pz_u256 *lo, const pz_u256 *hi, const pz_ge *pub, u64 T, char *err, size_t errlen);
/* Giant index (1-based, as used by the scheduler) in which `key` is found, 0 if outside the range. */
u64 bsgs_giant_index(const bsgs_params *p, const pz_u256 *key);
/* Scheduler configuration over the giant indices [1, Ng]. */
void bsgs_sched_cfg(const bsgs_params *p, const campaign_cfg *cfg, campaign_cfg *out);

/* ---- the baby-step table ---- */
bool bsgs_table_alloc(bsgs_table *t, u64 T, char *err, size_t errlen);
void bsgs_table_free(bsgs_table *t);
/* Builds the whole table with `nthreads` threads. units[i] counts the babies inserted by thread i. Returns false if stopped. */
bool bsgs_table_build(bsgs_table *t, int nthreads, _Atomic u64 **units, _Atomic int *stop);
/* Number of non-empty slots (parallel). */
u64 bsgs_table_count(const bsgs_table *t, int nthreads);
/* Looks the baby index up: true when the table has an entry for the x-coordinate (lo, hi are x.n[0], x.n[1], normalized). */
bool bsgs_table_contains(const bsgs_table *t, u64 xlo, u64 xhi, u64 *b_out);
/* Persistence. peek() reads only the header. */
bool bsgs_table_save(const bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen);
bool bsgs_table_peek(const char *path, u64 *T, char *err, size_t errlen);
bool bsgs_table_load(bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen);

/* ---- giant steps ---- */
typedef struct { pz_ge tab[BS_HALF]; pz_ge step; } bs_steps;     /* tab[i] = (i+1)*U, step = BS_GRP*U */
typedef struct {
    const bsgs_table *tab;
    const bsgs_params *p;
    bs_steps giant;                                         /* U = -S*G */
} bsgs_job;

void bsgs_job_init(bsgs_job *j, const bsgs_table *t, const bsgs_params *p);
/* A candidate key whose verification succeeded. */
typedef void (*bsgs_found_fn)(void *user, const pz_u256 *key);
/* Scans the giants with indices a0 .. a0+n-1 (a0 may be negative); whole groups are processed, so up to BS_GRP-1 giants
 * beyond the end are covered too.  Returns the number of the n giants fully covered.  `probes` counts table lookups,
 * `falsepos` the tag matches that did not verify. */
u64 bsgs_scan(const bsgs_job *j, int64_t a0, u64 n, bsgs_found_fn found, void *user, _Atomic u64 *progress, _Atomic u64 *falsepos,
              _Atomic int *stop);

/* Diagnostic: giant windows that needed the exact slow path (the key sits exactly on a giant step). */
extern _Atomic u64 bsgs_slow_windows;

/* Convenience for tests and the self-test: exhaustive search of [lo, hi] for the key behind `pub`, `nthreads` threads.
 * Returns true and the key when found; *giants receives the giants scanned. */
/* Name of the window routine in use ("scalar" or the lane-parallel variant) and the check of every variant against
 * the scalar one (prints one line each, returns the failures). */
const char *bsgs_engine_name(void);
int bsgs_vengine_selftest(void);
bool bsgs_solve(const pz_u256 *lo, const pz_u256 *hi, const pz_ge *pub, u64 T, int nthreads, pz_u256 *key, u64 *giants);

#endif
