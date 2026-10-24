/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* jobq.h - work scheduler for linear campaigns.
 *
 * The search space is split into chunks of 2^chunk_bits keys. The k-th chunk
 * visited by the campaign is chunk perm(k), where perm is the seed-keyed
 * bijection of perm.h. Inside a chunk keys are scanned sequentially, which is
 * what the EC engines are fastest at.
 *
 * Progress is tracked in a "virtual" linear space V = k * 2^chunk_bits + offset
 * as a sorted set of completed intervals. Workers claim contiguous pieces of
 * V; the set only ever holds one hole per claim in flight, so it stays tiny
 * and is cheap to checkpoint. */
#ifndef PZ_JOBQ_H
#define PZ_JOBQ_H

#include "common.h"
#include "campaign.h"
#include <pzcore/perm.h>
#include <pthread.h>

/* The order of the blocks of a campaign. Their arguments are always below the number of blocks, so a refusal of the permutation
 * is an internal error. */
static inline pz_u256 sched_perm_apply(const pz_perm *p, const pz_u256 *x)
{
    pz_u256 y;
    if (!pz_perm_apply(p, x, &y)) pz_die("internal error: a position of the visiting order is beyond the last block");
    return y;
}
static inline pz_u256 sched_perm_invert(const pz_perm *p, const pz_u256 *y)
{
    pz_u256 x;
    if (!pz_perm_invert(p, y, &x)) pz_die("internal error: a block is beyond the last block of the campaign");
    return x;
}

typedef struct {
    pz_u256 key_start;   /* first key of the claim */
    u64 nkeys;        /* keys in the claim */
    u128 v_start;     /* position in the virtual space */
    u64 id;
} claim_t;

typedef enum { CLAIM_OK = 0, CLAIM_WAIT = 1, CLAIM_DONE = 2 } claim_status;

typedef struct { u128 a, b; } vival;   /* [a, b) */

typedef struct sched {
    pthread_mutex_t mu;
    pz_perm perm;
    part_spec part;        /* this computer's shares of the search order (total == 0: all of it, see campaign.h) */
    char part_text[PART_TEXT];
    pz_u256 blocks_all;       /* blocks of the whole search order */
    pz_u256 blocks_part;      /* blocks of this computer's part (all of them without a split) */
    pz_u256 part_keys;        /* keys of this computer's part (the range size without a split) */
    pz_u256 range_start, range_end;
    unsigned cbits;
    u128 v_limit;          /* virtual space size, in this computer's blocks (capped at 2^126) */
    bool v_capped;
    u128 next;             /* next virtual position to hand out */
    vival *done; size_t ndone, capdone;
    struct { u64 id; u128 a, b; } *inflight; size_t ninf, capinf;
    u64 next_id;
    /* cached chunk */
    u128 cur_seq; bool cur_valid; pz_u256 cur_start; u64 cur_len;
    u128 keys_done;        /* exact count of keys covered (excluding fillers) */
    pz_u256 range_size;
} sched_t;

sched_t *sched_new(const campaign_cfg *cfg);
/* Whether cfg->part (see campaign.h) can be dealt out on this range: every share needs at least one block. */
bool sched_check_part(const campaign_cfg *cfg, char *err, size_t errn);
/* Position in this computer's own order of the block at position `gseq` of the whole search order; false when another computer's
 * share holds it. Without a split the positions are the same. */
bool sched_part_position(const sched_t *s, const pz_u256 *gseq, pz_u256 *local);
void sched_free(sched_t *s);
claim_status sched_claim(sched_t *s, u64 want, claim_t *out);
/* covered == nkeys marks the whole claim as done; smaller values mark only
 * the prefix (used when a worker is interrupted). */
void sched_complete(sched_t *s, const claim_t *c, u64 covered);

/* Persistence (text, with checksum). */
int sched_save(sched_t *s, const char *path);
/* 0 loaded; 1 the file is missing; -1 unreadable or damaged; -2 intact but it belongs to another campaign or another share of the order. */
int sched_load(sched_t *s, const char *path, char *err, size_t errlen);

u128 sched_keys_done(sched_t *s);
/* Number of disjoint completed intervals (diagnostic, thread-safe). */
size_t sched_open_intervals(sched_t *s);
/* Fraction of this computer's part covered, in [0,1] (of the range without a split). */
double sched_fraction(sched_t *s);
bool sched_finished(sched_t *s);
/* Sequence number (position in the visiting order of the whole range) at which `key` will be/was
 * scanned, as a fraction of the whole campaign. */
bool sched_locate(sched_t *s, const pz_u256 *key, pz_u256 *seq, u64 *offset);

#endif
