/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kangaroo.c - symmetric Gaudry-Schost kangaroo: setup, DP table, solving, herds. The CPU workers are in kang_cpu.c, the file of
 * distinguished points in kang_log.c, the self-tests in kang_selftest.c. */
#include "kangaroo_priv.h"
#include <pzcore/hash.h>
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#include <math.h>

/* ---------------- RNG ---------------- */

static inline u64 rotl64(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

u64 krng_next(u64 s[4])
{
    const u64 result = rotl64(s[1] * 5, 7) * 9;
    const u64 t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

void krng_seed(u64 s[4], const u8 seed[32], const char *tag, u64 a, u64 b)
{
    u8 buf[32 + 16 + 16 + 16];
    memcpy(buf, seed, 32);
    memset(buf + 32, 0, 16);
    strncpy((char *)buf + 32, tag, 15);
    memcpy(buf + 48, &a, 8);
    memcpy(buf + 56, &b, 8);
    u8 h[32];
    pz_sha256(buf, 64, h);
    memcpy(s, h, 32);
    if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
    for (int i = 0; i < 8; i++) krng_next(s);
}

pz_u256 krng_below(u64 s[4], const pz_u256 *bound)
{
    unsigned bits = pz_u256_bits(bound);
    if (bits == 0) return pz_u256_zero();
    pz_u256 mask = pz_u256_pow2(bits), one = pz_u256_from_u64(1);
    pz_u256_sub(&mask, &mask, &one);
    for (;;) {
        pz_u256 v;
        for (int i = 0; i < 4; i++) v.w[i] = krng_next(s) & mask.w[i];
        if (pz_u256_cmp(&v, bound) < 0) return v;
    }
}

/* ---------------- DP table ---------------- */

static inline unsigned log2u(u64 v) { return 63 - (unsigned)__builtin_clzll(v); }

static bool dp_empty(const dp_rec *e) { return e->tagk == 0 && !(e->d.w[0] | e->d.w[1] | e->d.w[2]); }
static inline u64 slot_of(const kang_state *ks, u64 tagk) { return ((tagk >> KANG_KSHIFT) * 0x9E3779B97F4A7C15ULL) >> (64 - log2u(ks->cap)); }
static inline u64 kind_of(const kang_state *ks, u64 tagk) { return tagk & (((u64)1 << KANG_KSHIFT) - 1); }

/* coefficient of x in the exponent E = coef*x + d of a point of kind k */
int kang_kind_coef(unsigned k)
{
    static const int c[5] = {0, 1, -1, 3, -3};
    return k < 5 ? c[k] : 0;
}

/* Two points on the same class: c_a x + d_a = c_b x + d_b, so x = (d_b - d_a) / (c_a - c_b), which must be whole. */
static bool solve_pair(kang_state *ks, const dp_rec *a, const dp_rec *b)
{
    int den = kang_kind_coef((unsigned)kind_of(ks, a->tagk)) - kang_kind_coef((unsigned)kind_of(ks, b->tagk));
    if (den == 0) return false;
    i192 x;
    i192_sub(&x, &b->d, &a->d);
    if (den < 0) { den = -den; i192_neg(&x); }
    bool neg = i192_is_neg(&x);
    if (neg) i192_neg(&x);
    pz_u256 mag = {{x.w[0], x.w[1], x.w[2], 0}}, key;
    if (den > 1) {
        u32 rem;
        mag = pz_u256_divmod_u32(&mag, (u32)den, &rem);
        if (rem) return false;
    }
    if (neg) {
        if (pz_u256_cmp(&ks->base, &mag) <= 0) return false;
        pz_u256_sub(&key, &ks->base, &mag);
    } else {
        pz_u256_add(&key, &ks->base, &mag);
    }
    return run_submit_key(ks->r, &key, "kangaroo");
}

/* False (nothing changed) when the memory for the larger table is not there. */
static bool table_grow(kang_state *ks)
{
    u64 ncap = ks->cap * 2;
    dp_rec *nt = calloc(ncap, sizeof(dp_rec));
    if (!nt) return false;
    dp_rec *old = ks->tab;
    u64 ocap = ks->cap;
    ks->tab = nt;
    ks->cap = ncap;
    ks->mask = ncap - 1;
    for (u64 i = 0; i < ocap; i++) {
        if (dp_empty(&old[i])) continue;
        u64 s = slot_of(ks, old[i].tagk);
        while (!dp_empty(&nt[s])) s = (s + 1) & ks->mask;
        nt[s] = old[i];
    }
    free(old);
    atomic_store_explicit(&ks->cap_mirror, ncap, memory_order_relaxed);
    return true;
}

/* Halves the table load when the RAM budget is exhausted: from now on only DPs whose tag has one more zero bit are kept (stored
 * entries that do not satisfy the stricter rule are dropped). It is done in place, so the table never exists twice in memory.
 *
 * Why the entries that stay are still found: a lookup goes from the first probe h of a tag to the first empty slot, so an entry at
 * slot p is found when the slots h .. p-1 are all taken. The sweep starts after an empty slot, which no probe crosses, and visits the
 * slots in order. The entry at slot p is lifted out and, if it stays, put back by the same rule: from h to the first empty slot,
 * with p counting as empty (so at p at the latest). The slots before p hold only entries put by that rule and the entries not visited
 * yet are after p, so the slots h .. p-1 are taken when it is put, and later steps empty only slots after p. */
static void table_thin(kang_state *ks)
{
    ks->thin_mask = ks->thin_mask << 1 | 1;
    ks->thin_level++;
    const dp_rec none = {0, {{0, 0, 0}}};
    u64 start = 0;
    while (!dp_empty(&ks->tab[start])) start++;       /* there is one: the last slot is never filled */
    u64 kept = 0;
    for (u64 i = 1; i <= ks->cap; i++) {
        const u64 p = (start + i) & ks->mask;
        const dp_rec e = ks->tab[p];
        if (dp_empty(&e)) continue;
        if ((e.tagk >> KANG_KSHIFT) & ks->thin_mask) { ks->tab[p] = none; continue; }
        u64 s = slot_of(ks, e.tagk);
        while (s != p && !dp_empty(&ks->tab[s])) s = (s + 1) & ks->mask;
        if (s != p) { ks->tab[s] = e; ks->tab[p] = none; }
        kept++;
    }
    ks->count = kept;
    atomic_store_explicit(&ks->count_mirror, kept, memory_order_relaxed);
    atomic_store_explicit(&ks->thin_mirror, ks->thin_level, memory_order_relaxed);
    pz_log(PZ_LOG_WARN, "distinguished-point table full (RAM limit): halving the density of stored points (level %u). Raise --mem to avoid this", ks->thin_level);
}

/* mu held. Returns 1 inserted, 0 duplicate of same kind, 2 solved, -1 skipped (thinned out) */
int kang_table_insert(kang_state *ks, const dp_rec *rec, bool from_log)
{
    if (ks->thin_mask && ((rec->tagk >> KANG_KSHIFT) & ks->thin_mask)) { atomic_fetch_add(&ks->dropped, 1); return -1; }
    if (ks->count * 10 >= ks->cap * 8) {
        u64 max_entries = ks->r->mem_limit / sizeof(dp_rec);
        /* the old table and the larger one exist together while the entries move: both must fit in the limit */
        const bool grown = ks->cap * 3 <= max_entries && table_grow(ks);
        if (!grown && ks->count * 100 >= ks->cap * 93) {
            table_thin(ks);
            if ((rec->tagk >> KANG_KSHIFT) & ks->thin_mask) { atomic_fetch_add(&ks->dropped, 1); return -1; }
        }
    }
    if (ks->count + 1 >= ks->cap) { atomic_fetch_add(&ks->dropped, 1); return -1; }      /* never fill the last slot: the probe below must end */
    u64 s = slot_of(ks, rec->tagk);
    for (;;) {
        dp_rec *e = &ks->tab[s];
        if (dp_empty(e)) {
            *e = *rec;
            ks->count++;
            atomic_store_explicit(&ks->count_mirror, ks->count, memory_order_relaxed);
            if (!from_log) kang_log_push(ks, rec);
            return 1;
        }
        if ((e->tagk >> KANG_KSHIFT) == (rec->tagk >> KANG_KSHIFT)) {
            if (kind_of(ks, e->tagk) == kind_of(ks, rec->tagk)) return 0;
            if (solve_pair(ks, e, rec)) return 2;
            atomic_fetch_add(&ks->rejected, 1);
            return 0;
        }
        s = (s + 1) & ks->mask;
    }
}

bool kang_insert_dps(kang_state *ks, const dp_rec *recs, int n, bool *dup_out)
{
    bool solved = false;
    pthread_mutex_lock(&ks->mu);
    for (int i = 0; i < n; i++) {
        int rc = kang_table_insert(ks, &recs[i], false);
        if (dup_out) dup_out[i] = (rc == 0);
        if (rc == 2) { solved = true; break; }
        if (ks->log_ready && i + 1 < n) {                /* a full block of the log (a big batch of a GPU): written without the lock, */
            kang_table_unlock(ks);                       /* not by the next full buffer under it */
            pthread_mutex_lock(&ks->mu);
        }
    }
    kang_table_unlock(ks);
    return solved;
}

/* ---------------- setup ---------------- */

static long double u256_ld(const pz_u256 *v) { return pz_u256_to_ld(v); }

/* The species of walkers (docs/en/ALGORITHMS.md, sections 8.8 and 8.10): tame walkers near 0 in the four residues mod 12 that are
 * multiples of 3, wild ones on Q in three residues 4 apart, wild ones on 3Q in one residue, every zone split into bands with their own
 * shares, optimized in the exact coverage model (tools/research/kbands.py), from tools/research/kang_schema3.txt by tools/research/kspec_table.py. */
static const kang_species KSPEC3[] = {
#include "kang_spec3.inc"
};

_Static_assert(sizeof KSPEC3 / sizeof *KSPEC3 <= 255, "the species index of a walker is a byte");

#define NKSPEC3 ((int)(sizeof KSPEC3 / sizeof *KSPEC3))

/* floor(sqrt(n)) with integers only: the jump table must come out the same on every architecture (long double is
 * 80 bits on x86, 64 on Apple arm64 and 128 on Linux arm64, so a table computed with it could differ between computers). */
static pz_u256 u256_isqrt(const pz_u256 *n)
{
    pz_u256 x = *n, res = pz_u256_zero();
    const unsigned b = pz_u256_bits(n);
    pz_u256 bit = pz_u256_pow2(b ? (b - 1) & ~1u : 0);
    while (!pz_u256_is_zero(&bit)) {
        pz_u256 t;
        pz_u256_add(&t, &res, &bit);
        res = pz_u256_shr(&res, 1);
        if (pz_u256_cmp(&x, &t) >= 0) {
            pz_u256_sub(&x, &x, &t);
            pz_u256_add(&res, &res, &bit);
        }
        bit = pz_u256_shr(&bit, 2);
    }
    return res;
}

/* a * q for q < 2^64 */
static pz_u256 u256_mul_u64(const pz_u256 *a, u64 q)
{
    pz_u256 lo = pz_u256_mul_u32(a, (u32)q), hi = pz_u256_mul_u32(a, (u32)(q >> 32));
    hi = pz_u256_shl(&hi, 32);
    pz_u256_add(&lo, &lo, &hi);
    return lo;
}

#define STR_(x) #x
#define STR(x) STR_(x)

const char *kang_range_error(const pz_u256 *W)
{
    if (pz_u256_bits(W) < 17) return "range too small for the kangaroo (at least 2^16 keys needed): use --mode linear";
    const pz_u256 max = pz_u256_pow2(KANG_MAX_W_LOG2);
    if (pz_u256_cmp(W, &max) > 0) return "range too large for the kangaroo (maximum 2^" STR(KANG_MAX_W_LOG2) " keys)";
    return NULL;
}

kang_state *kang_create(run_ctx *r)
{
    pz_ec_init();
    kang_state *ks = pz_xcalloc(1, sizeof *ks);
    ks->r = r;
    pthread_mutex_init(&ks->mu, NULL);
    pthread_mutex_init(&ks->wmu, NULL);
    ks->W = cfg_range_size(&r->cfg);
    const char *rerr = kang_range_error(&ks->W);
    if (rerr) pz_die("%s", rerr);
    ks->sqrtW = sqrtl(u256_ld(&ks->W));
    ks->epoch = (u64)r->sessions | (u64)r->node << 32;   /* node 0 keeps the streams of the single-computer campaigns */

    int nthr = r->use_cpu ? r->nthreads : 0;
    int k_gpu = 0;
    ks->K_thread = 0;
    if (nthr) {
        long double k = ks->sqrtW / (256.0L * nthr);
        ks->K_thread = k < 16 ? 16 : (k > KANG_K_THREAD_MAX ? KANG_K_THREAD_MAX : (int)k);
        ks->K_thread = (ks->K_thread + 15) & ~15;          /* whole vectors for the lane-parallel rounds */
        if (g_env.test_kthread) ks->K_thread = (g_env.test_kthread + 15) & ~15;   /* tests: walkers per CPU thread (the regime of long walks on a small interval) */
    }
    ks->K_total = ks->K_thread * nthr + k_gpu;
    return ks;
}

/* What depends on the interval and on the walkers: base (the walkers are E = c*x + u with x = key - base in [0, W)), Q' = pubkey - base*G and 3Q'. */
static void kang_setup(kang_state *ks)
{
    run_ctx *r = ks->r;
    ks->base = r->cfg.range_start;
    if (pz_u256_is_zero(&ks->base)) {
        ks->Qp = r->cfg.pubkey;
    } else {
        pz_ge bg;
        if (!pz_ec_mul_g(&bg, &ks->base)) pz_die("invalid base");
        if (!pz_ge_sub(&ks->Qp, &r->cfg.pubkey, &bg)) {
            /* the public key is exactly base*G */
            run_submit_key(r, &ks->base, "kangaroo (base)");
            ks->Qp = bg;   /* unused */
        }
    }
    pz_ge q2;
    if (!pz_ge_add(&q2, &ks->Qp, &ks->Qp) || !pz_ge_add(&ks->Q3, &q2, &ks->Qp)) ks->Q3 = ks->Qp;   /* only if Qp is not a point: the key was found above */
    ks->expected_steps = KANG_C_MEAN * ks->sqrtW;
}

/* Called once the total number of walkers (CPU + GPU) is known. */
int kang_configure(kang_state *ks, int k_gpu_total)
{
    run_ctx *r = ks->r;
    int nthr = r->use_cpu ? r->nthreads : 0;
    ks->K_total = ks->K_thread * nthr + k_gpu_total;
    if (ks->K_total <= 0) return -1;
    kang_setup(ks);
    if (r->mem_limit == 0) {
        u64 lim = (u64)((double)r->sys.mem_avail * 0.4);
        if (lim > (64ULL << 30)) lim = 64ULL << 30;
        if (lim < (256ULL << 20)) lim = 256ULL << 20;
        r->mem_limit = lim;
    }
    u64 max_entries = r->mem_limit / sizeof(dp_rec);
    int dp_ov = 0;
    long double ov = 0.01L * ks->sqrtW / ks->K_total;
    if (ov >= 1.0L) dp_ov = (int)floorl(log2l(ov));
    int dp_mem = 0;
    long double need = KANG_DP_MEM_MARGIN * ks->sqrtW / (0.8L * (long double)max_entries);
    if (need > 1.0L) dp_mem = (int)ceill(log2l(need));
    int dp = dp_ov > dp_mem ? dp_ov : dp_mem;
    if (dp < 2) dp = 2;
    if (dp > 60) dp = 60;
    if (r->cfg.dp_bits) {
        /* a walker reaches a distinguished point every 2^dp steps: after the collision every walker still has to get
         * there, so K * 2^dp must stay well below sqrt(W), otherwise the search costs many times its expected effort */
        long double room = ks->sqrtW / ks->K_total;
        int dmax = room >= 4.0L ? (int)floorl(log2l(room)) - 1 : 1;
        if (dmax < dp) dmax = dp;   /* never below what memory requires */
        if ((int)r->cfg.dp_bits > dmax) {
            if (!r->silent_setup)
                pz_log(PZ_LOG_WARN, "--dp-bits %u is too high for this range with %d kangaroos (distinguished points would be too rare): using %d",
                       r->cfg.dp_bits, ks->K_total, dmax);
            dp = dmax;
        } else dp = (int)r->cfg.dp_bits;
    }
    ks->P.dp_bits = (unsigned)dp;
    ks->P.dp_mask = ((u64)1 << dp) - 1;
    /* a walker without a DP for 24 times the mean distance between DPs is in a cycle (false alarm ~e^-24); the counter
     * is 64-bit: with large dp_bits (45 on puzzle 140) a 32-bit cap would restart every walker long before its next DP */
    ks->P.cap_since = dp >= 58 ? ~0ULL : 24ULL * ((u64)1 << dp) + 16;
    ks->P.R = KANG_R;

    /* jump table (the walk on classes is a symmetric random walk: no drift to tune): uniform in [2/3, 4/3] of the mean, rounded up
     * to a multiple of KANG_MOD, in integer arithmetic (identical on every architecture). Without drift a walker stays within about
     * sqrt(n) jumps of its start (n ~ sqrt(W)/K steps each), so two walks can cross only if their starts are that close: the number
     * of such pairs is about (mean / W^(3/4)) * K^1.5, hence the mean of W^(3/4)/16: K^1.5/16 pairs, and each walk covers a
     * fraction 1/(16 sqrt(K)) of W (docs/en/ALGORITHMS.md 8.10) */
    u128 mean3;
    {
        const pz_u256 rw = u256_isqrt(&ks->W), qw = u256_isqrt(&rw);          /* sqrt(W) and W^(1/4), rounded down */
        pz_u256 m;
        if (g_env.test_jump) {                                              /* tests: mean jump in units of sqrt(W) */
            m = pz_u256_mul_u32(&rw, (u32)g_env.test_jump);
        } else {
            m = u256_mul_u64(&rw, qw.w[0]);                                 /* W^(3/4); W^(1/4) < 2^48 */
            m = pz_u256_shr(&m, 4);                                            /* the mean jump is W^(3/4) / 16 */
        }
        if (pz_u256_bits(&m) > 120) m = pz_u256_pow2(120);                        /* jump distances are 128-bit (W above 2^165) */
        mean3 = (u128)m.w[1] << 64 | m.w[0];
        if (mean3 < 2 * KANG_MOD) mean3 = 2 * KANG_MOD;
    }
    const long double mean = (long double)mean3;                               /* only for the log line */
    u64 rg[4];
    krng_seed(rg, r->cfg.seed, "jumps4", 0, 0);
    pz_u256 bound = pz_u256_from_u128(mean3 / 3 * 2 + 1);
    const pz_u256 low = pz_u256_from_u128(mean3 / 3 * 2);
    pz_ge pts[KANG_R + KANG_NESC];
    for (unsigned j = 0; j < KANG_R + KANG_NESC; j++) {   /* the escape jumps after the regular ones, same distribution */
        pz_u256 v = krng_below(rg, &bound);
        u32 rem;
        pz_u256_add(&v, &v, &low);
        v = pz_u256_divmod_u32(&v, KANG_MOD, &rem);
        pz_u256_add_u64(&v, &v, 1);
        v = pz_u256_mul_u32(&v, KANG_MOD);
        ks->P.jv[j] = (u128)v.w[1] << 64 | v.w[0];
        if (!pz_ec_mul_g(&pts[j], &v)) pz_die("invalid jump");
        ks->P.jx[j] = pts[j].x;
        ks->P.jy[j] = pts[j].y;
    }

    {   /* identifies the jump table: computers of one campaign must show the same id */
        u8 jb[(KANG_R + KANG_NESC) * 16], jh[32];
        for (unsigned j = 0; j < KANG_R + KANG_NESC; j++) {
            memcpy(jb + 16 * j, &(u64){(u64)ks->P.jv[j]}, 8);
            memcpy(jb + 16 * j + 8, &(u64){(u64)(ks->P.jv[j] >> 64)}, 8);
        }
        pz_sha256(jb, sizeof jb, jh);
        snprintf(ks->jump_id, sizeof ks->jump_id, "%02x%02x%02x%02x", jh[0], jh[1], jh[2], jh[3]);
        memcpy(ks->jump_tag, jh, sizeof ks->jump_tag);
    }

    /* The table starts with room for the points expected at 60 % load, as far as the memory limit allows. With a small --dp-bits on a large interval
     * that is more than a u64 holds (2^69 points on puzzle 140 with --dp-bits 2), and the conversion of such a number is undefined: the number is
     * cut at 2^62, far above any table that memory can hold, and the limit decides. */
    const long double n_target = KANG_DP_MEM_MARGIN * ks->sqrtW / ldexpl(1.0L, dp);
    const long double want_max = ldexpl(1.0L, 62);
    const long double want_f = n_target / 0.6L;
    const u64 want = (want_f < want_max ? (u64)want_f : (u64)1 << 62) + 4096;
    u64 cap = 4096;
    while (cap < want && (cap << 1) <= max_entries) cap <<= 1;   /* a power of two, never above the memory limit */
    if (g_env.test_dp_cap >= 0) cap = (u64)g_env.test_dp_cap;                    /* tests: force a tiny table to exercise growth */
    if (cap < 64) cap = 64;
    ks->cap = cap;
    ks->mask = cap - 1;
    atomic_store_explicit(&ks->cap_mirror, cap, memory_order_relaxed);
    ks->tab = calloc(cap, sizeof(dp_rec));
    if (!ks->tab) pz_die("not enough memory for the distinguished-point table (%llu entries)", (unsigned long long)cap);
    ks->log_hdr = KANG_LOG_HDR;
    ks->caplog = ks->capw = KANG_LOG_BUF_INIT;
    ks->logbuf = pz_xmalloc(ks->caplog * sizeof(dp_rec));
    ks->wbuf = pz_xmalloc(ks->capw * sizeof(dp_rec));
    snprintf(ks->log_path, sizeof ks->log_path, "%s/dp.bin", r->cfg.dir);

    if (r->silent_setup) return 0;
    char b1[32], b2[32], b3[32];
    pz_log(PZ_LOG_INFO, "kangaroo: W = 2^%.1Lf, %d kangaroos (CPU %d x %d, GPU %d), dp-bits %d, mean jump %.3Le, table %s (max %s)",
           log2l(u256_ld(&ks->W)), ks->K_total, nthr, ks->K_thread, k_gpu_total, dp, mean, pz_fmt_bytes((double)cap * sizeof(dp_rec), b1, sizeof b1),
           pz_fmt_bytes((double)r->mem_limit, b2, sizeof b2));
    pz_log(PZ_LOG_INFO, "kangaroo: jump table id %s (computers working on the same campaign must show the same id)", ks->jump_id);
    pz_log(PZ_LOG_INFO, "kangaroo: expected effort ~%s steps (%.2Lf x sqrt(W)); expected distinguished points ~%s",
           pz_fmt_si((double)ks->expected_steps, b3, sizeof b3), (long double)KANG_C_MEAN, pz_fmt_si((double)(ks->expected_steps / ldexpl(1.0L, dp)), b1, sizeof b1));
    return 0;
}

void kang_destroy(kang_state *ks)
{
    if (!ks) return;
    kang_flush_log(ks);
    if (ks->log) fclose(ks->log);
    free(ks->tab);
    free(ks->logbuf);
    free(ks->wbuf);
    pthread_mutex_destroy(&ks->mu);
    pthread_mutex_destroy(&ks->wmu);
    free(ks);
}

/* ---------------- herds ---------------- */

kang_worker *kang_worker_new(kang_state *ks, int K, u64 stream)
{
    kang_worker *w = pz_xcalloc(1, sizeof *w);
    w->K = K;
    w->P = &ks->P;
    w->x = pz_xaligned(64, sizeof(pz_fe) * (size_t)K);
    w->y = pz_xaligned(64, sizeof(pz_fe) * (size_t)K);
    w->d = pz_xcalloc((size_t)K, sizeof(i192));
    w->kind = pz_xcalloc((size_t)K, 1);
    w->sigma = pz_xcalloc((size_t)K, 1);
    w->prev = pz_xcalloc((size_t)K, sizeof(u32));
    w->prev2 = pz_xcalloc((size_t)K, sizeof(u32));
    w->lastdp = pz_xcalloc((size_t)K, sizeof(u64));
    w->ck = pz_xcalloc((size_t)K, sizeof(u32));
    w->mn = pz_xcalloc((size_t)K, sizeof(u32));
    w->esc = pz_xcalloc((size_t)K, 1);
    /* dx / tmp: allocated by the first scalar round (the GPU mirrors and the lane-parallel rounds normally never need them) */
    w->jidx = pz_xcalloc((size_t)K, 4);
    w->dpcap = K;                /* a round yields at most one distinguished point per kangaroo */
    w->dpbuf = pz_xcalloc((size_t)w->dpcap, sizeof(dp_rec));
    w->dpidx = pz_xcalloc((size_t)w->dpcap, sizeof(int));
    w->restart = pz_xcalloc((size_t)K, sizeof(int));
    w->vstale = pz_xcalloc((size_t)K, 1);
    w->spec = pz_xcalloc((size_t)K, 1);
    krng_seed(w->rng, ks->r->cfg.seed, "herd", ks->epoch, stream);
    /* the shares of the species, as exact as K allows: walker i takes the species at (i + 1/2) / K of the cumulated shares. The
     * species here is the group of bands with the same coefficient and residue (the first band of the group stands for it), and
     * every start draws the band at random by the shares: with a few dozen walkers and 51 bands, a fixed band per walker would
     * leave some residues without walkers, and the keys that only those could reach without a solution */
    const int ns = NKSPEC3;
    const kang_species *sp = KSPEC3;
    u64 gw[256] = {0}, tot = 0;
    int first[256];
    for (int s = 0; s < ns; s++) {
        int g = s;
        for (int t = 0; t < s; t++)
            if (sp[t].coef == sp[s].coef && sp[t].res == sp[s].res) { g = t; break; }
        first[s] = g;
        gw[g] += sp[s].w;
        tot += sp[s].w;
    }
    for (int i = 0; i < K; i++) {
        const u64 pos = ((u64)(2 * i + 1) * tot) / (2 * (u64)K);
        u64 acc = 0;
        int s = 0;
        while (s < ns - 1 && (first[s] != s || pos >= acc + gw[s])) acc += first[s] == s ? gw[s] : 0, s++;
        w->spec[i] = (u8)s;
        w->kind[i] = sp[s].coef;
    }
    return w;
}

void kang_worker_free(kang_worker *w)
{
    if (!w) return;
    pz_afree(w->x); pz_afree(w->y); free(w->d); free(w->kind); free(w->spec); free(w->sigma); free(w->prev); free(w->prev2);
    free(w->lastdp); free(w->ck); free(w->mn); free(w->esc); pz_afree(w->dx); pz_afree(w->tmp); free(w->jidx); free(w->dpbuf); free(w->dpidx); free(w->restart);
    pz_afree(w->vx); pz_afree(w->vy); pz_afree(w->vdx); pz_afree(w->vtmp); pz_afree(w->vjt); pz_afree(w->vaux); free(w->vstale);
    free(w);
}

void kang_worker_sync(kang_worker *w)
{
    if (w->vsync) w->vsync(w);
}

/* Residue of a mod m, in [0, m). */
static u32 i192_mod_u32(const i192 *a, u32 m)
{
    const bool neg = i192_is_neg(a);
    i192 t = *a;
    if (neg) i192_neg(&t);
    pz_u256 mag = {{t.w[0], t.w[1], t.w[2], 0}};
    u32 rem;
    pz_u256_divmod_u32(&mag, m, &rem);
    return neg && rem ? m - rem : rem;
}

/* A start of species sp, E = c*x + u: the point c*Qp + u*G and u. False when it cannot be used (u = 0 on a tame
 * walker, or the point at infinity, which reveals the key). */
static bool start_species(kang_state *ks, const kang_species *sp, u64 rng[4], pz_ge *p, i192 *d)
{
    pz_u256 lo = pz_u256_mul_u32(&ks->W, (u32)(sp->lo < 0 ? -sp->lo : sp->lo)), span = pz_u256_mul_u32(&ks->W, (u32)(sp->hi - sp->lo));
    u32 rem;
    lo = pz_u256_divmod_u32(&lo, 1000, &rem);
    span = pz_u256_divmod_u32(&span, 1000, &rem);
    pz_u256_add_u64(&span, &span, 1);
    const pz_u256 v = krng_below(rng, &span);
    /* u = lo + v, lo negative or not */
    i192 u = i192_from_signed(false, &v), l = i192_from_signed(sp->lo < 0, &lo);
    i192_add(&u, &u, &l);
    const pz_u256 back = pz_u256_from_u64((i192_mod_u32(&u, KANG_MOD) + KANG_MOD - sp->res) % KANG_MOD);
    const i192 b = i192_from_signed(false, &back);
    i192_sub(&u, &u, &b);                                 /* down to the residue of the species */
    const bool neg = i192_is_neg(&u);
    i192 m = u;
    if (neg) i192_neg(&m);
    const pz_u256 mag = {{m.w[0], m.w[1], m.w[2], 0}};
    const pz_ge *cq = sp->coef == 3 ? &ks->Q3 : &ks->Qp;
    if (pz_u256_is_zero(&mag)) {
        if (sp->coef == 0) return false;
        *p = *cq;
    } else {
        pz_ge ug;
        if (!pz_ec_mul_g(&ug, &mag)) return false;
        if (neg) pz_ge_neg(&ug, &ug);
        if (sp->coef == 0) *p = ug;
        else if (!pz_ge_add(p, &ug, cq)) {
            /* c*x + u = 0: x = -u / c */
            pz_u256 x;
            if (!neg) return false;                       /* x would be negative */
            x = sp->coef == 3 ? pz_u256_divmod_u32(&mag, 3, &rem) : mag;
            if (sp->coef == 3 && rem) return false;
            pz_u256 key;
            pz_u256_add(&key, &ks->base, &x);
            run_submit_key(ks->r, &key, "kangaroo (start)");
            return false;
        }
    }
    *d = u;
    return true;
}

void kang_restart_rng(kang_state *ks, kang_worker *w, int i, u64 rng[4])
{
    for (;;) {
        pz_ge p;
        i192 d;
        int8_t sg = 1;
        {
            const int ns = NKSPEC3;
            const kang_species *sp = KSPEC3, *band = &sp[w->spec[i]];
            u64 tot = 0;                                 /* a band of the walker's group, by the shares */
            for (int s = 0; s < ns; s++)
                if (sp[s].coef == band->coef && sp[s].res == band->res) tot += sp[s].w;
            u64 pick = krng_next(rng) % tot;
            for (int s = 0; s < ns; s++)
                if (sp[s].coef == band->coef && sp[s].res == band->res) {
                    if (pick < sp[s].w) { band = &sp[s]; break; }
                    pick -= sp[s].w;
                }
            if (!start_species(ks, band, rng, &p, &d)) continue;
        }
        pz_fe_normalize(&p.y);
        if (pz_fe_is_odd(&p.y)) {   /* canonical representative: even y */
            pz_ge_neg(&p, &p);
            i192_neg(&d);
            sg = (int8_t)-sg;
        }
        w->x[i] = p.x;
        w->y[i] = p.y;
        w->vstale[i] = 1;
        w->d[i] = d;
        w->sigma[i] = w->kind[i] == 0 ? 1 : sg;
        w->prev[i] = w->prev2[i] = KANG_NOTAG;
        w->ck[i] = w->mn[i] = KANG_NOTAG;
        w->esc[i] = 0;
        w->lastdp[i] = w->round;
        return;
    }
}

void kang_restart_one(kang_state *ks, kang_worker *w, int i) { kang_restart_rng(ks, w, i, w->rng); }

void kang_init_herd(kang_state *ks, kang_worker *w)
{
    for (int i = 0; i < w->K; i++) kang_restart_one(ks, w, i);
}

typedef struct { kang_state *ks; kang_worker *w; int from, to; u64 rng[4]; } init_task;

static void *init_thread(void *p)
{
    init_task *t = p;
    for (int i = t->from; i < t->to; i++) kang_restart_rng(t->ks, t->w, i, t->rng);
    return NULL;
}

/* Initializes a (large) herd with several threads, each using its own random stream. */
void kang_init_herd_parallel(kang_state *ks, kang_worker *w, int nthreads, u64 stream)
{
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 32) nthreads = 32;
    pthread_t th[32];
    init_task tk[32];
    int per = (w->K + nthreads - 1) / nthreads;
    for (int t = 0; t < nthreads; t++) {
        tk[t].ks = ks;
        tk[t].w = w;
        tk[t].from = t * per < w->K ? t * per : w->K;
        tk[t].to = (t + 1) * per < w->K ? (t + 1) * per : w->K;
        krng_seed(tk[t].rng, ks->r->cfg.seed, "init", ks->epoch, stream * 64 + (u64)t);
        pthread_create(&th[t], NULL, init_thread, &tk[t]);
    }
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
}
