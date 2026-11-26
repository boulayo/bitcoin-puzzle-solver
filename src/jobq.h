/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* jobq.h - the scheduler of the linear search: the blocks of a range in the order of the seed, claimed in pieces.
 *
 * The range is split into blocks of 2^chunk_bits keys (--chunk-bits). The k-th block visited by the campaign is block perm(k),
 * where perm is the seed-keyed bijection of perm.h. Inside a block the keys are scanned in order, which is what the engines
 * are fastest at.
 *
 * Progress is kept in a "virtual" linear space V = k * 2^chunk_bits + offset as a sorted set of completed intervals. Workers claim
 * contiguous pieces of V; the set holds a hole for each claim in flight and for each claim given back unfinished (a stop, a GPU that
 * failed), which the visit fills again, so it stays tiny and is cheap to checkpoint. */
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
    char seed_id[17];         /* fingerprint of the seed (seed_to_id): the checkpoint records it, so that one of another seed is refused */
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
    /* the block the last claim came from (load_block): its position, first key and length */
    u128 cur_seq; bool cur_valid; pz_u256 cur_start; u64 cur_len;
    u128 keys_done;        /* exact count of keys covered (excluding fillers) */
    pz_u256 range_size;
    vival *start; size_t nstart;   /* the done set as the session found it (sched_load), for sched_revert */
    u128 start_keys;
    u128 given_back;       /* keys of this session that went back to be scanned again (sched_reopen, sched_revert) */
    u128 snap_back;        /* given_back at the last snapshot */
    u128 saved_back;       /* given_back at the last snapshot that reached the disk: while they differ, the disk still has those keys as scanned */
    bool frozen;           /* sched_freeze */
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
/* The keys of a completed claim, its n first ones from virtual position v, to be scanned again (a worker the canary condemned, canary.h):
 * the visit fills the hole like that of a claim given back. */
void sched_reopen(sched_t *s, u128 v, u64 n);
/* Everything completed in this session to be scanned again: the done set as the checkpoint loaded it (nothing for a new campaign). */
void sched_revert(sched_t *s);
/* The keys of this session given back by those two; *unsaved (optional): whether some were since the last state that reached the disk, so
 * that the state on disk still records them as scanned and must be written again at once. */
u128 sched_given_back(sched_t *s, bool *unsaved);
/* From now on no claim that ends is recorded as scanned (a key found that FOUND.txt could not take: its block stays to be scanned again). */
void sched_freeze(sched_t *s);

/* Persistence (text, with checksum). sched_save writes the state as it is now to path (the one before kept as path.bak); sched_snapshot
 * takes that text (*len bytes, to free) for sched_write to write later, when something else must reach the disk first, and sched_saved
 * says when it has; *back (optional): it has keys given back since the last state saved, whose copy on disk (path.bak after one write)
 * still has them as scanned. */
int sched_save(sched_t *s, const char *path);
char *sched_snapshot(sched_t *s, size_t *len, bool *back);
int sched_write(const char *snap, size_t len, const char *path);
void sched_saved(sched_t *s);
/* 0 loaded; 1 the file is missing; -1 unreadable or damaged; -2 intact but it belongs to another campaign, another seed or another share
 * of the order. Checkpoints written before the seed was recorded (no seed_id line) are accepted. */
int sched_load(sched_t *s, const char *path, char *err, size_t errlen);

u128 sched_keys_done(sched_t *s);
/* Number of disjoint completed intervals (diagnostic, thread-safe). */
size_t sched_open_intervals(sched_t *s);
/* Fraction of this computer's part covered, in [0,1] (of the range without a split). */
double sched_fraction(sched_t *s);
bool sched_finished(sched_t *s);
/* Where `key` will be or was scanned: the position of its block in the visiting order of the whole range (*seq) and its offset in the block.
 * False outside the range. */
bool sched_locate(sched_t *s, const pz_u256 *key, pz_u256 *seq, u64 *offset);

#endif
