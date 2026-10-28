/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kangaroo_priv.h - internals of the symmetric Gaudry-Schost kangaroo shared
 * by the CPU workers, the GPU workers and the self-test.
 *
 * Algorithm (see docs/en/ALGORITHMS.md, sections 4.1, 8.8 and 8.10):
 *   Exponents are relative to base = start: the unknown x = key - base lies in [0, W). Walkers move on equivalence classes
 *   {P, -P} (canonical representative = even y). Every walker is E = c*x + u with c = 0 (tame), 1 (wild on Q) or 3 (wild on 3Q),
 *   its start u in a fixed residue class mod 12 and every jump a multiple of 12, so the residue of a walker never changes. The
 *   species (c, residue, zone, share: kang_species) are chosen so that the pairs that can meet are the ones whose equation
 *   (c1 - c2) x = u2 - u1 has a whole solution for many x. Every walker carries its exact exponent d and a sign sigma, so a
 *   collision of E1 = s1 c1 x + d1 and E2 = s2 c2 x + d2 (s = sign of the walker) gives (s1 c1 - s2 c2) x = d2 - d1: ANY two
 *   distinct-kind distinguished points on the same class reveal x.
 *
 *   Every start zone is split into bands with their own shares, and the mean jump is W^(3/4)/16: without drift a walker stays
 *   within about sqrt(n) jumps of its start, and with long walks short jumps leave too few pairs of walks that can cross
 *   (puzzle 135 on one machine: 1.35 instead of 1.02). */
#ifndef PZ_KANGAROO_PRIV_H
#define PZ_KANGAROO_PRIV_H

#include "common.h"
#include "kangaroo.h"
#include <pzcore/ec.h>

#define KANG_R 256               /* jump table size */
#define KANG_NESC 16             /* escape jumps, after the R regular ones: used only to leave a fruitless cycle */
#define KANG_CK_PERIOD 64        /* steps between the cycle checkpoints (detects cycles up to ~31 steps) */
#define KANG_NOTAG 0xFFFFFFFFu   /* "no tag yet" */
#define KANG_JUMP_DIV 16.0L      /* mean jump W^(3/4) / KANG_JUMP_DIV */
#define KANG_KSHIFT 3            /* bits of the kind code at the bottom of tagk */
#define KANG_K_THREAD_MAX 4096   /* kangaroos per CPU thread: one field inversion per round of the herd (measured optimum) */

/* signed 192-bit integer (two's complement) */
typedef struct { u64 w[3]; } i192;

static inline i192 i192_zero(void) { i192 r = {{0, 0, 0}}; return r; }
static inline void i192_add_u128(i192 *a, u128 v)
{
    u128 t = (u128)a->w[0] + (u64)v;
    a->w[0] = (u64)t;
    t = (t >> 64) + a->w[1] + (u64)(v >> 64);
    a->w[1] = (u64)t;
    a->w[2] += (u64)(t >> 64);
}
static inline void i192_neg(i192 *a)
{
    u128 t = (u128)(~a->w[0]) + 1;
    a->w[0] = (u64)t;
    t = (t >> 64) + (u128)(~a->w[1]);
    a->w[1] = (u64)t;
    a->w[2] = ~a->w[2] + (u64)(t >> 64);
}
static inline bool i192_is_neg(const i192 *a) { return a->w[2] >> 63; }
static inline void i192_add(i192 *r, const i192 *a, const i192 *b)
{
    u128 t = (u128)a->w[0] + b->w[0];
    r->w[0] = (u64)t;
    t = (t >> 64) + a->w[1] + b->w[1];
    r->w[1] = (u64)t;
    r->w[2] = a->w[2] + b->w[2] + (u64)(t >> 64);
}
static inline void i192_sub(i192 *r, const i192 *a, const i192 *b)
{
    i192 nb = *b;
    i192_neg(&nb);
    i192_add(r, a, &nb);
}
/* arithmetic shift right by one */
static inline void i192_sar1(i192 *a)
{
    a->w[0] = a->w[0] >> 1 | a->w[1] << 63;
    a->w[1] = a->w[1] >> 1 | a->w[2] << 63;
    a->w[2] = (u64)((int64_t)a->w[2] >> 1);
}
static inline i192 i192_from_signed(bool neg, const pz_u256 *mag)
{
    i192 r = {{mag->w[0], mag->w[1], mag->w[2]}};
    if (neg) i192_neg(&r);
    return r;
}

/* distinguished-point record: 32 bytes. tagk = (x.n[2] & ~7) | kind. The steps write the codes tame / wild with its sign (0 to 2);
 * kang_dp_recode turns them into the codes of the species (0 to 4). */
typedef struct { u64 tagk; i192 d; } dp_rec;
#define KIND_TAME 0
#define KIND_WILD_P 1            /* wild, sigma = +1 */
#define KIND_WILD_M 2            /* wild, sigma = -1 */
#define KIND_WILD3_P 3           /* wild on 3Q, sigma = +1 */
#define KIND_WILD3_M 4           /* wild on 3Q, sigma = -1 */
#define KANG_MOD 12              /* jumps are multiples of it, every walker keeps its start residue */

/* A species of walkers: E = coef*x + u, u = res (mod KANG_MOD) uniform in [lo, hi] thousandths of W,
 * share w millionths of the herd (each zone is split into bands with their own shares). */
typedef struct { u8 coef, res; int16_t lo, hi; uint32_t w; } kang_species;

typedef struct {
    unsigned R;
    pz_fe jx[KANG_R + KANG_NESC], jy[KANG_R + KANG_NESC];
    u128 jv[KANG_R + KANG_NESC];
    u64 dp_mask;                 /* applied to x.n[1] */
    unsigned dp_bits;
    u64 cap_since;               /* watchdog: steps without a DP before a restart (about 24 * 2^dp_bits: 64 bits); only
                                  * tested at the checkpoint steps */
} kang_params_t;

typedef struct kang_state {
    run_ctx *r;
    pz_u256 W;                      /* interval width */
    long double c_mean;          /* expected cost in units of sqrt(W) */
    char jump_id[9];             /* first 4 bytes of the SHA-256 of the jump table, in hex: equal on all computers of a campaign */
    pz_u256 base;                   /* key = base + x */
    pz_ge Qp;                       /* pubkey - base*G */
    pz_ge Q3;                       /* 3*Qp */
    long double sqrtW;
    kang_params_t P;
    int K_thread;                /* kangaroos per CPU thread */
    int K_total;
    /* DP table */
    dp_rec *tab;
    u64 cap, mask;
    u64 count;
    u64 thin_mask;               /* only DPs whose (tag>>KANG_KSHIFT)&thin_mask == 0 are stored once the table is full */
    unsigned thin_level;
    pthread_mutex_t mu;
    /* append-only log for resume */
    FILE *log;
    dp_rec *logbuf;
    size_t nlog, caplog;
    u64 log_recs;                /* whole records known to be in the file (loaded + flushed) */
    bool log_broken;             /* the last write failed: the file is cut back to whole records and reopened before the next one */
    bool log_warned;             /* the full-disk warning was given (until a write succeeds again) */
    char log_path[620];
    /* statistics */
    _Atomic u64 dups, restarts, rejected, dropped;
    u64 loaded;
    u64 epoch;
    long double expected_steps;
    pthread_t *threads;
    int nthreads;
} kang_state;

/* per-worker (CPU or host-side GPU) kangaroo herd */
typedef struct kang_worker {
    int K;
    pz_fe *x, *y;
    i192 *d;
    u8 *kind;                    /* 0 tame, 1 wild (wild on 3Q: 3; the steps only test kind != 0) */
    u8 *spec;                    /* species of each walker (index in the table of kangaroo.c) */
    int8_t *sigma;
    u32 *prev, *prev2;           /* tags of the points one and two steps back */
    u64 *lastdp;                 /* value of `round` at the last distinguished point (or restart) of each walker */
    u32 *ck, *mn;                /* fruitless cycles: tag at the last checkpoint, least tag since */
    u8 *esc;                     /* back on the checkpoint: leaving the cycle from its point of least tag */
    u64 round;                   /* steps taken by every walker of the herd (the step number of the next round) */
    pz_fe *dx, *tmp;
    u32 *jidx;
    dp_rec *dpbuf;
    int *dpidx;
    int ndp, dpcap;
    int *restart;                /* indices needing a new random start */
    int nrestart;
    const kang_params_t *P;
    u64 rng[4];
    /* lane-parallel rounds (kang_vec_impl.inc): the coordinates in vector form, allocated by the first such round */
    void *vx, *vy, *vdx, *vtmp;
    void *vaux;                  /* distances, tags, counters and signs in vector form */
    u64 *vjt;                    /* jump points as limbs, then the jump distances */
    u8 *vstale;                  /* x[i] / y[i] written by scalar code: reload the lane */
    void (*vsync)(struct kang_worker *);   /* copies the vector coordinates back to x / y */
} kang_worker;

/* Fruitless cycles. The walk on classes {P, -P} falls into short cycles that never reach a distinguished point:
 * 2-cycles (about 1 step in 2R) are avoided by the prev2 rule, but 4-, 6-, ... cycles remain, about one every 4 R^2
 * steps (2.6e5 with R = 256): with dp_bits around 45 every walker would be trapped within minutes. At every checkpoint
 * step (step number a multiple of KANG_CK_PERIOD, the same for all walkers) each walker takes the tag of its point as
 * its checkpoint and keeps the least tag seen since; coming back to the checkpoint means a cycle, and the walker leaves
 * it from the point of least tag with an escape jump (a table of its own, so that the walk cannot fall back into the
 * same cycle through the regular jumps). The exit point depends only on the cycle, not on where or when a walker
 * entered it: walkers that met keep walking together, which is what makes their collision visible at the next
 * distinguished point.
 *
 * A tag is the low 32 bits of x.n[2] (a false match only starts a harmless extra detection, one in 2^32 per
 * comparison). Jump index of a walker at a point with low word x0 and tag t (pure function of the state). */
static inline u32 kang_tag(u64 n2) { return (u32)n2; }
static inline u32 kang_jump_index(u32 R, u64 x0, u32 t, u32 prev2, u32 ck, u32 mn, u8 esc)
{
    if ((esc || t == ck) && t == mn) return R + (u32)((x0 >> 16) & (KANG_NESC - 1));
    u32 j = (u32)x0 & (R - 1);
    if (t == prev2) j = (j + 1) & (R - 1);   /* deterministic 2-cycle escape */
    return j;
}

/* State update of the cycle detection with the tag t of the point the walker has just left; bnd: this is a
 * checkpoint step. */
static inline void kang_cycle_update(bool bnd, u32 t, u32 *ck, u32 *mn, u8 *esc)
{
    if (*esc || t == *ck) {
        if (t == *mn) { *esc = 0; *ck = *mn = KANG_NOTAG; }          /* leaves now */
        else if (*esc && bnd) { *esc = 0; *ck = *mn = t; }          /* not found within a period: start over */
        else *esc = 1;
    } else if (bnd) {
        *ck = *mn = t;
    } else if (t < *mn) {
        *mn = t;
    }
}

/* Room for one more distinguished point in w->dpbuf (it grows when a caller accumulates several rounds, as the GPU
 * self-test does; a worker that empties it every round never exceeds K). */
static inline void kang_dp_reserve(kang_worker *w)
{
    if (w->ndp < w->dpcap) return;
    w->dpcap *= 2;
    w->dpbuf = pz_xrealloc(w->dpbuf, sizeof(dp_rec) * (size_t)w->dpcap);
    w->dpidx = pz_xrealloc(w->dpidx, sizeof(int) * (size_t)w->dpcap);
}

/* The kind code of a distinguished point of walker i, written by a step (tame / wild with its sign), becomes the code of its
 * species (three bits). */
static inline void kang_dp_recode(const kang_state *ks, const kang_worker *w, dp_rec *r, int i)
{
    unsigned k = (unsigned)(r->tagk & 3);
    if (k && w->kind[i] == 3) k += 2;
    r->tagk = (r->tagk & ~7ULL) | k;
}

/* Pseudo-random generator (xoshiro256**) seeded from the campaign seed. */
void krng_seed(u64 s[4], const u8 seed[32], const char *tag, u64 a, u64 b);
u64 krng_next(u64 s[4]);
pz_u256 krng_below(u64 s[4], const pz_u256 *bound);   /* uniform in [0, bound) */

kang_state *kang_create(run_ctx *r);
int kang_configure(kang_state *ks, int k_gpu_total);
void kang_destroy(kang_state *ks);
int kang_load_log(kang_state *ks);
int kang_flush_log(kang_state *ks);
typedef struct { u64 read, added, dups, skipped; bool solved; } kang_merge_stats;
int kang_merge_file(kang_state *ks, const char *path, kang_merge_stats *st);

/* The file of distinguished points (kang_log.c) and the table it feeds (kangaroo.c). Magic of the logs of this campaign: logs of earlier
 * layouts of the walkers ("PZDP1" to "PZDP3", written by versions before 1.9.1) have other kind codes and another base, their points
 * cannot be mixed with these. */
#define KANG_LOG_MAGIC "PZDP4\n"
void kang_log_fp(const kang_state *ks, u8 fp[32]);                                       /* fingerprint of the campaign in a log header */
FILE *kang_log_create(const kang_state *ks, const char *path, const u8 fp[32]);
void kang_log_push(kang_state *ks, const dp_rec *rec);                                   /* mu held */
long long kang_log_read(kang_state *ks, FILE *f);                                        /* the records of an open log into the table: their number, -1 on a read error */
int kang_table_insert(kang_state *ks, const dp_rec *rec, bool from_log);                 /* mu held: 1 inserted, 0 duplicate, 2 solved, -1 skipped */
int kang_kind_coef(unsigned k);                                                          /* coefficient of x in the exponent of a point of kind k */

kang_worker *kang_worker_new(kang_state *ks, int K, u64 stream);
void kang_worker_free(kang_worker *w);
void kang_restart_one(kang_state *ks, kang_worker *w, int i);
void kang_restart_rng(kang_state *ks, kang_worker *w, int i, u64 rng[4]);
void kang_init_herd_parallel(kang_state *ks, kang_worker *w, int nthreads, u64 stream);
void kang_init_herd(kang_state *ks, kang_worker *w);

/* Inserts DPs into the shared table. dup_out[k] is set for records that
 * duplicate an entry of the same kind (the walker should be restarted).
 * Returns true when the key has been found. */
bool kang_insert_dps(kang_state *ks, const dp_rec *recs, int n, bool *dup_out);

/* ISA variants of one batch round over the herd. */
void kang_round_base(kang_worker *w);
void kang_round_x86(kang_worker *w);
void kang_round_avx2v(kang_worker *w);
void kang_round_avx512v(kang_worker *w);
void kang_round_avx512i(kang_worker *w);
void kang_round_avx512i2(kang_worker *w);     /* the same, two vectors at a time in the second half of the step (see kv_bwd_step) */
typedef void (*kang_round_fn)(kang_worker *);
/* The step variants this build has, the portable one first (kangaroo.c); `supported` tells whether this CPU can run one. */
typedef struct { const char *id, *name; bool (*supported)(void); kang_round_fn fn; } kang_round_variant;
const kang_round_variant *kang_round_variants(int *n);
/* The fastest round on this CPU among those that pass kang_round_check (measured once per process). */
kang_round_fn kang_pick_round(void);
const char *kang_round_name(void);
/* Makes w->x / w->y current (after lane-parallel rounds). */
void kang_worker_sync(kang_worker *w);
/* Runs `fn` and the portable scalar round side by side on identical herds (distinguished points, restarts, a kangaroo
 * forced onto its jump point) and compares everything; false at the first difference. */
bool kang_round_check(kang_round_fn fn, int K, int rounds);

/* Runs the CPU herd until stop. */
void kang_cpu_worker_main(kang_state *ks, int widx, int slot, u64 stream);

#endif

