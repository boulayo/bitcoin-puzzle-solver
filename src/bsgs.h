/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs.h - baby-step giant-step with a symmetric RAM table, for ranges whose public key is known.
 *
 * Unknown key k in [lo, hi], W = hi - lo + 1, target P = k*G.  Let c = lo + (W-1)/2, d = k - c, |d| <= H = ceil(W/2).
 *   Baby steps:  the x-coordinates of b*G for b = 1..T go into a hash table (b and -b share x, so one entry covers both).
 *   Giant steps: S = 2T+1;  for a = -A..A  look up the x-coordinate of Q_a = (P - c*G) - a*S*G.  A hit gives b with
 *                Q_a = +-b*G, hence k = c + a*S +- b.
 * Every one of the (2A+1)*S >= 2H+1 values of d is covered exactly once, so the search is exhaustive and deterministic:
 * with the giants visited in a random order the expected cost is T + W/(4T) group operations, minimal (1.0*sqrt(W))
 * for T = sqrt(W)/2, against 1.06*sqrt(W) (KANG_C_MEAN) for the kangaroo, at the price of 4/0.70 ~ 5.7 bytes of RAM per baby step.
 * The table does not depend on the target nor on the range: it can be saved to disk and reused.
 *
 * Table layout ("PZBSGS03"): 4-byte cells holding a 32-bit tag of the x-coordinate (x.n[1], 0 = empty), BS_BUCKET32 of them per
 * bucket, one 64-byte cache line.  An x-coordinate belongs to the bucket floor(x.n[0]*nbuckets/2^64); a baby goes into the first
 * free cell of its bucket, or, when the bucket is full, of the next ones (wrapping around), so the cells of a bucket are always
 * filled from the first one on.  A lookup reads its bucket (one line) and goes on to the next only when the last cell of the bucket
 * is in use.  The cells do not keep b: when a tag matches, b is found by a small baby-step giant-step over [1, T] with a table of
 * about 2*sqrt(T) entries that keep it (bsgs_recover), which a tag matching by chance (about once in 2^28 giant steps) costs too.
 * Tables saved by the earlier versions are read and used as they are: "PZBSGS02" (8-byte cells: 24-bit tag | 40-bit b, 8 per
 * bucket, load 0.70, 11.4 bytes per baby step) and "PZBSGS01" (the same cells in one array probed linearly, bsgs_table.linear, load
 * 0.65, 12.3 bytes per baby step). */
#ifndef PZ_BSGS_H
#define PZ_BSGS_H

#include "campaign.h"
#include <stdatomic.h>

#define BS_HALF 512                 /* points on each side of a group centre (one batch inversion per group) */
#define BS_GRP (2 * BS_HALF)
#define BS_MAX_T (((u64)1 << 40) - 1)   /* the baby index is at most 40 bits (the cells of PZBSGS01/02 store it in 40 bits) */
#define BS_BUCKET32 16              /* cells of 4 bytes per bucket: 64 bytes, one cache line */
#define BS_LOAD_NUM 7               /* a new table has BS_LOAD_NUM / BS_LOAD_DEN of its cells in use (integers: its size must be */
#define BS_LOAD_DEN 10              /* the same on every platform, the header of a saved table is checked against it) */
#define BS_LOAD_FACTOR ((double)BS_LOAD_NUM / BS_LOAD_DEN)
#define BS_BUCKET 8                 /* PZBSGS02: cells of 8 bytes per bucket (64 bytes), BS_LOAD2_NUM / BS_LOAD2_DEN of them in use */
#define BS_LOAD2_NUM 7
#define BS_LOAD2_DEN 10

typedef struct {
    u64 *slots;          /* the cells, 64-byte aligned: 8 bytes (tag:24 | b:40) in PZBSGS01/02, two of 4 bytes (tag32) per word in PZBSGS03; 0 = empty */
    u64 nslots;          /* number of 8-byte words (a multiple of BS_BUCKET: 64 bytes) */
    u64 T;               /* baby steps stored: b = 1..T */
    void *map;           /* mmap'd region (slots lives inside it) */
    size_t map_bytes;
    bool thp;            /* transparent huge pages accepted by the kernel */
    bool tag32;          /* PZBSGS03: 4-byte cells with a tag only (every table built with bsgs_table_alloc) */
    bool linear;         /* table read from a PZBSGS01 file: one array of cells, linear probing (never true for a table that was built) */
    _Atomic(void *) aux; /* PZBSGS03: the small table that gives b back when a tag matches, built on the first match (bsgs_recover) */
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

/* ---- sizing (for a table that is built, in the bucketed layout) ---- */
u64 bsgs_slots_for(u64 T);                  /* cells */
u64 bsgs_bytes_for(u64 T);                  /* RAM of the table */
u64 bsgs_max_T_for_bytes(u64 bytes);        /* the largest T whose table fits in `bytes` */
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
/* Looks the x-coordinate up (lo, hi are x.n[0], x.n[1], normalized): true when the table has an entry with its tag. *b_out (optional)
 * receives the baby index stored with it, 0 for a PZBSGS03 table, whose cells keep only the tag (bsgs_recover gives it back). */
bool bsgs_table_contains(const bsgs_table *t, u64 xlo, u64 xhi, u64 *b_out);
/* The baby step b in [1, T] with x(b*G) = x(q), 0 when there is none: what a matching tag of a PZBSGS03 table leaves to find. A
 * baby-step giant-step over [1, T] with a table of about 2*sqrt(T) entries that keep b (built on the first call, kept with the
 * table, safe from several threads): about sqrt(T)/4 giant steps for each sign of q. */
u64 bsgs_recover(const bsgs_table *t, const pz_ge *q);
/* Persistence. peek() reads only the header; peek_ram() also gives the RAM the table takes once loaded (more for an old-format file
 * than bsgs_bytes_for(T)). A table read from an old-format file is written back in that format. */
bool bsgs_table_save(const bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen);
bool bsgs_table_peek(const char *path, u64 *T, char *err, size_t errlen);
bool bsgs_table_peek_ram(const char *path, u64 *T, u64 *ram_bytes, char *err, size_t errlen);
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
