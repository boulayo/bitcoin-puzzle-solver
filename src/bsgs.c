/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs.c - baby-step giant-step with a symmetric RAM table. See bsgs.h. */
#include "bsgs_priv.h"
#include "cpu_engine.h"
#include <pzcore/hash.h>
#include "kangaroo.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

/* ------------------------------------------------------------------ sizing */

/* A table that is built: ceil(T / (BS_BUCKET * load)) buckets and 8 more (so that a tiny table still has room). Integer arithmetic. */
static u64 buckets_for(u64 T)
{
    const u64 per = (u64)BS_BUCKET * BS_LOAD_NUM;
    return (T * BS_LOAD_DEN + per - 1) / per + 8;
}

u64 bsgs_slots_for(u64 T) { return buckets_for(T) * BS_BUCKET; }

u64 bsgs_bytes_for(u64 T) { return bsgs_slots_for(T) * 8; }

u64 bsgs_max_T_for_bytes(u64 bytes)
{
    u64 nb = bytes / ((u64)BS_BUCKET * 8);
    if (nb <= 8) return 0;
    nb -= 8;
    if (nb >= BS_MAX_T) return BS_MAX_T;
    u64 T = nb * BS_BUCKET * BS_LOAD_NUM / BS_LOAD_DEN;
    return T > BS_MAX_T ? BS_MAX_T : T;
}

/* Cells of a PZBSGS01 table (the old layout, load 0.65, probed linearly) for T babies: what the header of such a file must say. The
 * quotient is in long double, so a platform whose long double is a double may round differently by one group: see header_check. */
#define BS_LINEAR_LOAD 0.65
static u64 linear_slots_for(u64 T)
{
    u64 n = (u64)ceill((long double)T / (long double)BS_LINEAR_LOAD) + 64;
    return (n + 7) & ~(u64)7;
}

u64 bsgs_optimal_T(const pz_u256 *W)
{
    long double w = pz_u256_to_ld(W);
    long double t = ceill(sqrtl(w) / 2.0L);
    if (t < 1) t = 1;
    if (t > (long double)BS_MAX_T) return 0;
    return (u64)t;
}

long double bsgs_mean_cost(const pz_u256 *W, u64 T)
{
    long double w = pz_u256_to_ld(W);
    return (long double)T + w / (4.0L * (long double)T);
}

u64 bsgs_choose_T(const pz_u256 *W, u64 budget_bytes, bool *viable)
{
    u64 topt = bsgs_optimal_T(W), tmax = bsgs_max_T_for_bytes(budget_bytes);
    u64 T = topt ? (topt < tmax ? topt : tmax) : tmax;
    bool ok = false;
    if (T >= 1) ok = bsgs_mean_cost(W, T) <= kang_mean_cost(W);
    if (viable) *viable = ok;
    return T;
}

/* ------------------------------------------------------------- parameters */

static bool u256_to_u128_checked(const pz_u256 *a, u128 *out)
{
    if (a->w[2] || a->w[3]) return false;
    *out = (u128)a->w[1] << 64 | a->w[0];
    return true;
}

bool bsgs_params_init(bsgs_params *p, const pz_u256 *lo, const pz_u256 *hi, const pz_ge *pub, u64 T, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    if (T < 1 || T > BS_MAX_T) { snprintf(err, errlen, "invalid number of baby steps (%llu)", (unsigned long long)T); return false; }
    if (pz_u256_cmp(lo, hi) > 0 || pz_u256_is_zero(lo)) { snprintf(err, errlen, "invalid range"); return false; }
    p->lo = *lo;
    p->hi = *hi;
    pz_u256 one = pz_u256_from_u64(1), W;
    pz_u256_sub(&W, hi, lo);
    pz_u256_add(&W, &W, &one);
    p->W = W;
    u128 w128;
    if (!u256_to_u128_checked(&W, &w128) || (w128 >> 118)) { snprintf(err, errlen, "range too large for BSGS (more than 2^118 keys)"); return false; }
    p->T = T;
    p->S = 2 * T + 1;
    u128 H = (w128 >> 1) + (w128 & 1);
    u128 A = (H + p->S - 1) / p->S;
    if (A >= ((u128)1 << 62)) { snprintf(err, errlen, "too many giant steps: increase the memory for the table"); return false; }
    p->A = (u64)A;
    p->Ng = 2 * p->A + 1;
    pz_u256 half = pz_u256_from_u128((w128 - 1) >> 1);
    pz_u256_add(&p->c, lo, &half);
    p->P = *pub;
    pz_ec_init();
    pz_ge cG;
    if (!pz_ec_mul_g(&cG, &p->c)) { snprintf(err, errlen, "invalid range centre"); return false; }
    p->Pp_inf = !pz_ge_sub(&p->Pp, pub, &cG);
    return true;
}

/* d = key - c as a signed 128-bit value (the key is within H of c, so it fits). */
static __int128 key_minus_c(const bsgs_params *p, const pz_u256 *key)
{
    pz_u256 d;
    if (pz_u256_cmp(key, &p->c) >= 0) {
        pz_u256_sub(&d, key, &p->c);
        return (__int128)((u128)d.w[1] << 64 | d.w[0]);
    }
    pz_u256_sub(&d, &p->c, key);
    return -(__int128)((u128)d.w[1] << 64 | d.w[0]);
}

u64 bsgs_giant_index(const bsgs_params *p, const pz_u256 *key)
{
    if (pz_u256_cmp(key, &p->lo) < 0 || pz_u256_cmp(key, &p->hi) > 0) return 0;
    __int128 d = key_minus_c(p, key), S = (__int128)p->S, T = (__int128)p->T;
    __int128 num = d + T, a;
    if (num >= 0) a = num / S;
    else a = -((-num + S - 1) / S);
    __int128 idx = a + (__int128)p->A + 1;
    if (idx < 1 || idx > (__int128)p->Ng) return 0;
    return (u64)idx;
}

void bsgs_sched_cfg(const bsgs_params *p, const campaign_cfg *cfg, campaign_cfg *out)
{
    *out = *cfg;
    out->range_start = pz_u256_from_u64(1);
    out->range_end = pz_u256_from_u64(p->Ng);
    pz_u256 rsz = pz_u256_from_u64(p->Ng);
    unsigned cb = cfg_default_chunk_bits(&rsz);
    pz_u256 chunk = pz_u256_pow2(cb);
    if (pz_u256_cmp(&chunk, &rsz) > 0) {
        unsigned b = pz_u256_bits(&rsz) - 1;
        cb = b < 1 ? 1 : b;
    }
    out->chunk_bits = cb;
}

/* ------------------------------------------------------------ giant points */

/* q = Pp - a*S*G.  Returns false when q is the point at infinity (the key is then c + a*S exactly). */
static bool giant_point(const bsgs_params *p, int64_t a, pz_ge *q)
{
    __int128 m = (__int128)a * (__int128)p->S;
    if (m == 0) { *q = p->Pp; return true; }
    u128 mag = (u128)(m < 0 ? -m : m);
    pz_u256 k = pz_u256_from_u128(mag);
    pz_ge mG;
    if (!pz_ec_mul_g(&mG, &k)) return false;
    return m > 0 ? pz_ge_sub(q, &p->Pp, &mG) : pz_ge_add(q, &p->Pp, &mG);
}

/* c + a*S + sgn*b  (mod n) */
static pz_u256 candidate_key(const bsgs_params *p, int64_t a, u64 b, int sgn)
{
    __int128 m = (__int128)a * (__int128)p->S + (sgn < 0 ? -(__int128)b : (__int128)b);
    u128 mag = (u128)(m < 0 ? -m : m);
    pz_u256 mg = pz_u256_from_u128(mag), r;
    if (m >= 0) pz_u256_addmod_n(&r, &p->c, &mg);
    else pz_u256_submod_n(&r, &p->c, &mg);
    return r;
}

static bool verify_key(const bsgs_params *p, const pz_u256 *key)
{
    pz_ge q;
    if (pz_u256_is_zero(key) || !pz_ec_mul_g(&q, key)) return false;
    return pz_ge_equal(&q, &p->P);
}

/* ------------------------------------------------------------------ steps */

static void steps_make(bs_steps *st, const pz_ge *U, u64 kmag, bool negated)
{
    pz_gej *pts = pz_xmalloc(sizeof(pz_gej) * (BS_HALF + 1));
    pz_ge *aff = pz_xmalloc(sizeof(pz_ge) * (BS_HALF + 1));
    pz_gej cur;
    pz_gej_set_ge(&cur, U);
    pts[0] = cur;
    for (int i = 1; i < BS_HALF; i++) {
        pz_gej_add_ge(&cur, &cur, U);
        pts[i] = cur;
    }
    pz_gej_double(&cur, &pts[BS_HALF - 1]);
    pts[BS_HALF] = cur;
    if (!pz_gej_to_ge_batch(aff, pts, BS_HALF + 1)) pz_die("BSGS tables: unexpected point at infinity");
    memcpy(st->tab, aff, sizeof(pz_ge) * BS_HALF);
    st->step = aff[BS_HALF];
    /* independent check of the last table entry and of the step with scalar multiplications */
    pz_ge chk, chk2;
    pz_u256 k1 = pz_u256_from_u128((u128)kmag * BS_HALF), k2 = pz_u256_from_u128((u128)kmag * BS_GRP);
    if (!pz_ec_mul_g(&chk, &k1) || !pz_ec_mul_g(&chk2, &k2)) pz_die("BSGS: invalid step table");
    if (negated) { pz_ge_neg(&chk, &chk); pz_ge_neg(&chk2, &chk2); }
    if (!pz_ge_equal(&chk, &st->tab[BS_HALF - 1]) || !pz_ge_equal(&chk2, &st->step)) pz_die("BSGS: step table check failed");
    free(pts);
    free(aff);
}

static bs_group_fn pick_group(void)
{
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    if (cpu_has_avx2()) return bs_group_x86;
#endif
    return bs_group_base;
}

/* The lane-parallel windows must give exactly the x words of the scalar routine (two calls, so that the advance of the
 * centres is covered too); a variant that does not is never used. */
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
static bool vengine_check(const bs_vengine *ve)
{
    bs_steps *st = pz_xmalloc(sizeof *st);
    steps_make(st, &pz_ec_g, 1, false);
    void *vt = ve->prepare(st);
    const int nl = ve->lanes;
    pz_ge *C = pz_xcalloc((size_t)nl, sizeof(pz_ge));
    u64 *vlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *vhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    u64 *slo = pz_xaligned(64, sizeof(u64) * BS_GRP), *shi = pz_xaligned(64, sizeof(u64) * BS_GRP);
    pz_fe *dx = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1));
    void *scr = pz_xaligned(64, ve->scratch);
    bool ok = vt != NULL;
    const u64 k0 = 7777;
    for (int l = 0; l < nl && ok; l++) ok = pz_ec_mul_g(&C[l], &(pz_u256){{k0 + BS_HALF + (u64)l * BS_GRP, 0, 0, 0}});
    for (int call = 0; call < 2 && ok; call++) {
        ok = ve->groups(vt, C, vlo, vhi, scr);
        for (int l = 0; l < nl && ok; l++) {
            pz_ge S;
            ok = pz_ec_mul_g(&S, &(pz_u256){{k0 + BS_HALF + (u64)(call * nl + l) * BS_GRP, 0, 0, 0}}) && bs_group_base(st, &S, slo, shi, dx, tmp);
            for (int off = 0; off < BS_GRP && ok; off++)
                ok = vlo[(size_t)off * nl + l] == slo[off] && vhi[(size_t)off * nl + l] == shi[off];
        }
    }
    pz_afree(vt); free(C); pz_afree(vlo); pz_afree(vhi); pz_afree(slo); pz_afree(shi); pz_afree(dx); pz_afree(tmp); pz_afree(scr);
    free(st);
    return ok;
}
#endif

static pthread_once_t g_vengine_once = PTHREAD_ONCE_INIT;
static const bs_vengine *g_vengine;

static void vengine_pick(void)
{
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    const bs_vengine *cand[] = {&BSV_AVX512I, &BSV_AVX512V, &BSV_AVX2V};    /* fastest first */
    if (g_env.bsgs_scalar) return;                                          /* diagnostics: scalar windows only */
    for (size_t i = 0; i < sizeof cand / sizeof *cand; i++) {
        if (!cand[i]->supported()) continue;
        if (vengine_check(cand[i])) { g_vengine = cand[i]; return; }
        pz_log(PZ_LOG_WARN, "BSGS: variant %s does not match the scalar one, excluded", cand[i]->name);
    }
#endif
}

/* The lane-parallel windows in use, NULL for the scalar ones. */
static const bs_vengine *pick_vengine(void)
{
    pthread_once(&g_vengine_once, vengine_pick);
    return g_vengine;
}

const char *bsgs_engine_name(void)
{
    const bs_vengine *ve = pick_vengine();
    return ve ? ve->name : "scalar";
}

int bsgs_vengine_selftest(void)
{
    int fails = 0;
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    const bs_vengine *cand[] = {&BSV_AVX2V, &BSV_AVX512V, &BSV_AVX512I};
    for (size_t i = 0; i < sizeof cand / sizeof *cand; i++) {
        char lb[64];
        snprintf(lb, sizeof lb, "BSGS windows %s", cand[i]->name);
        if (!cand[i]->supported()) { pz_result_line(lb, "not supported by this CPU: skipped"); continue; }
        bool ok = vengine_check(cand[i]);
        pz_result_line(lb, "%s", ok ? "identical to the scalar ones" : "DIFFERENT FROM THE SCALAR ONES");
        fails += !ok;
    }
#endif
    return fails;
}

void bsgs_job_init(bsgs_job *j, const bsgs_table *t, const bsgs_params *p)
{
    pz_ec_init();
    j->tab = t;
    j->p = p;
    pz_ge SG;
    if (!pz_ec_mul_g(&SG, &(pz_u256){{p->S, 0, 0, 0}})) pz_die("BSGS: invalid S*G");
    pz_ge U;
    pz_ge_neg(&U, &SG);
    steps_make(&j->giant, &U, p->S, true);
}

/* --------------------------------------------------------------- the table */

/* nslots cells for T babies; `linear` marks the old layout (only a table read from a PZBSGS01 file) */
static bool table_alloc(bsgs_table *t, u64 T, u64 nslots, bool linear, char *err, size_t errlen)
{
    memset(t, 0, sizeof *t);
    if (T < 1 || T > BS_MAX_T) { snprintf(err, errlen, "invalid table size"); return false; }
    t->T = T;
    t->nslots = nslots;
    t->linear = linear;
    const size_t align = (size_t)2 << 20;
    size_t bytes = (size_t)t->nslots * 8;
    t->map_bytes = bytes + align;
    bool huge = false;
    void *m = pz_map_zeroed(t->map_bytes, &huge);
    if (!m) {
        snprintf(err, errlen, "cannot reserve %.2f GiB for the table: %s", (double)bytes / 1073741824.0, strerror(errno));
        memset(t, 0, sizeof *t);
        return false;
    }
    t->map = m;
    uintptr_t a = ((uintptr_t)m + align - 1) & ~(uintptr_t)(align - 1);     /* 2 MiB: every bucket starts on a cache line */
    t->slots = (u64 *)a;
    t->thp = huge;
    return true;
}

bool bsgs_table_alloc(bsgs_table *t, u64 T, char *err, size_t errlen)
{
    return table_alloc(t, T, T >= 1 && T <= BS_MAX_T ? bsgs_slots_for(T) : 0, false, err, errlen);
}

void bsgs_table_free(bsgs_table *t)
{
    pz_unmap(t->map, t->map_bytes);
    memset(t, 0, sizeof *t);
}

typedef struct {
    bsgs_table *t;
    const bs_steps *st;
    bs_group_fn gf;
    _Atomic u64 *next_block;
    u64 nblocks, blk;
    _Atomic u64 *units;
    _Atomic int *stop;
    u64 inserted;
    bool stopped;
} build_arg;

enum { BUILD_BLOCK = 1 << 22 };

static void *build_thread(void *p)
{
    build_arg *a = p;
    bsgs_table *t = a->t;
    const u64 nb = t->nslots / BS_BUCKET;
    const bs_vengine *ve = pick_vengine();
    void *vt = ve ? ve->prepare(a->st) : NULL;
    const int nl = vt ? ve->lanes : 1;
    pz_fe *dx = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1));
    u64 *xlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *xhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    void *scr = vt ? pz_xaligned(64, ve->scratch) : NULL;
    pz_ge *Cv = pz_xcalloc((size_t)nl, sizeof(pz_ge));
    for (;;) {
        u64 blk = atomic_fetch_add(a->next_block, 1);
        if (blk >= a->nblocks) break;
        const u64 b0 = 1 + blk * a->blk;
        const u64 bend = b0 + a->blk < t->T + 1 ? b0 + a->blk : t->T + 1;   /* exclusive */
        u64 first = b0;
        if (vt) {
            /* nl windows at once while they fit in the block (baby steps are small multiples of G: no zero denominator) */
            for (int l = 0; l < nl; l++)
                if (!pz_ec_mul_g(&Cv[l], &(pz_u256){{b0 + BS_HALF + (u64)l * BS_GRP, 0, 0, 0}})) pz_die("BSGS: invalid centre");
            for (; first + (u64)BS_GRP * nl <= bend; first += (u64)BS_GRP * nl) {
                if (atomic_load_explicit(a->stop, memory_order_relaxed)) { a->stopped = true; goto done; }
                if (!ve->groups(vt, Cv, xlo, xhi, scr)) pz_die("BSGS: zero denominator while building the table");
                const int tot = BS_GRP * nl;
                for (int k = 0; k < tot; k++) {
                    if (k + 16 < tot) __builtin_prefetch(&t->slots[bs_bucket_index(nb, xlo[k + 16]) * BS_BUCKET], 1, 1);
                    if (bs_insert(t->slots, nb, xlo[k], xhi[k], first + (u64)(k % nl) * BS_GRP + (u64)(k / nl))) a->inserted++;
                }
                if (a->units) atomic_fetch_add_explicit(a->units, (u64)tot, memory_order_relaxed);
            }
        }
        if (first >= bend) continue;
        pz_ge C;
        if (!pz_ec_mul_g(&C, &(pz_u256){{first + BS_HALF, 0, 0, 0}})) pz_die("BSGS: invalid centre");
        for (; first < bend; first += BS_GRP) {
            if (atomic_load_explicit(a->stop, memory_order_relaxed)) { a->stopped = true; goto done; }
            if (!a->gf(a->st, &C, xlo, xhi, dx, tmp)) pz_die("BSGS: zero denominator while building the table");
            u64 cnt = bend - first < BS_GRP ? bend - first : BS_GRP;
            for (u64 i = 0; i < cnt; i++) {
                if (i + 16 < cnt) __builtin_prefetch(&t->slots[bs_bucket_index(nb, xlo[i + 16]) * BS_BUCKET], 1, 1);
                if (bs_insert(t->slots, nb, xlo[i], xhi[i], first + i)) a->inserted++;
            }
            if (a->units) atomic_fetch_add_explicit(a->units, cnt, memory_order_relaxed);
        }
    }
done:
    pz_afree(dx); pz_afree(tmp); pz_afree(xlo); pz_afree(xhi); pz_afree(scr); pz_afree(vt); free(Cv);
    return NULL;
}

bool bsgs_table_build(bsgs_table *t, int nthreads, _Atomic u64 **units, _Atomic int *stop)
{
    pz_ec_init();
    if (nthreads < 1) nthreads = 1;
    bs_steps *st = pz_xmalloc(sizeof *st);
    steps_make(st, &pz_ec_g, 1, false);
    _Atomic u64 next = 0;
    _Atomic int never = 0;
    build_arg *args = pz_xcalloc((size_t)nthreads, sizeof *args);
    pthread_t *th = pz_xcalloc((size_t)nthreads, sizeof *th);
    u64 nblocks = (t->T + BUILD_BLOCK - 1) / BUILD_BLOCK;
    for (int i = 0; i < nthreads; i++) {
        args[i] = (build_arg){t, st, pick_group(), &next, nblocks, BUILD_BLOCK, units ? units[i] : NULL, stop ? stop : &never, 0, false};
        if (pthread_create(&th[i], NULL, build_thread, &args[i]) != 0) pz_die("pthread_create failed");
    }
    u64 inserted = 0;
    bool stopped = false;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
        inserted += args[i].inserted;
        stopped |= args[i].stopped;
    }
    free(args); free(th); free(st);
    if (stopped) return false;
    if (inserted != t->T) pz_die("BSGS: the table has %llu entries instead of %llu (unexpected collision)", (unsigned long long)inserted, (unsigned long long)t->T);
    return true;
}

/* One pass over the cells: how many are in use, and (bucketed layout) whether every bucket is filled from its first cell on, the
 * property that ends every lookup. A file whose checksum is right can still have been written by something else. */
typedef struct { const u64 *s; u64 n, cnt; bool linear, ordered; } count_arg;
static void *count_thread(void *p)
{
    count_arg *a = p;
    u64 c = 0;
    bool ordered = true;
    if (a->linear) {
        for (u64 i = 0; i < a->n; i++) c += a->s[i] != 0;
    } else {
        for (u64 i = 0; i < a->n; i += BS_BUCKET) {            /* n is a multiple of the bucket: threads get whole buckets */
            const u64 *bk = a->s + i;
            int used = 0;
            for (int j = 0; j < BS_BUCKET; j++) used += bk[j] != 0;
            for (int j = used; j < BS_BUCKET; j++) ordered &= bk[j] == 0;
            c += (u64)used;
        }
    }
    a->cnt = c;
    a->ordered = ordered;
    return NULL;
}

static u64 table_count(const bsgs_table *t, int nthreads, bool *ordered)
{
    if (nthreads < 1) nthreads = 1;
    count_arg *a = pz_xcalloc((size_t)nthreads, sizeof *a);
    pthread_t *th = pz_xcalloc((size_t)nthreads, sizeof *th);
    u64 per = (t->nslots + (u64)nthreads - 1) / (u64)nthreads;
    per = (per + BS_BUCKET - 1) / BS_BUCKET * BS_BUCKET;
    for (int i = 0; i < nthreads; i++) {
        u64 from = (u64)i * per, to = from + per < t->nslots ? from + per : t->nslots;
        a[i] = (count_arg){t->slots + (from < to ? from : t->nslots), from < to ? to - from : 0, 0, t->linear, true};
        if (pthread_create(&th[i], NULL, count_thread, &a[i]) != 0) pz_die("pthread_create failed");
    }
    u64 total = 0;
    bool ok = true;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); total += a[i].cnt; ok &= a[i].ordered; }
    free(a); free(th);
    if (ordered) *ordered = ok;
    return total;
}

u64 bsgs_table_count(const bsgs_table *t, int nthreads) { return table_count(t, nthreads, NULL); }

bool bsgs_table_contains(const bsgs_table *t, u64 xlo, u64 xhi, u64 *b_out)
{
    const u64 tag = xhi & BS_TAG_MASK;
    if (t->linear) {
        u64 i = bs_slot_index(t->nslots, xlo);
        for (;;) {
            u64 s = t->slots[i];
            if (s == 0) return false;
            if ((s >> BS_VAL_BITS) == tag) { if (b_out) *b_out = s & BS_VAL_MASK; return true; }
            if (++i == t->nslots) i = 0;
        }
    }
    const u64 nb = t->nslots / BS_BUCKET;
    u64 i = bs_bucket_index(nb, xlo);
    for (;;) {
        const u64 *bk = t->slots + i * BS_BUCKET;
        for (int j = 0; j < BS_BUCKET; j++) {
            if (bk[j] == 0) return false;                   /* the cells are taken in order: nothing beyond this one */
            if ((bk[j] >> BS_VAL_BITS) == tag) { if (b_out) *b_out = bk[j] & BS_VAL_MASK; return true; }
        }
        if (++i == nb) i = 0;
    }
}

/* true when the table holds exactly the entry for (x, b) */
static bool table_has_entry(const bsgs_table *t, u64 xlo, u64 xhi, u64 b)
{
    const u64 e = bs_entry(xhi, b);
    if (t->linear) {
        u64 i = bs_slot_index(t->nslots, xlo);
        for (;;) {
            u64 s = t->slots[i];
            if (s == 0) return false;
            if (s == e) return true;
            if (++i == t->nslots) i = 0;
        }
    }
    const u64 nb = t->nslots / BS_BUCKET;
    u64 i = bs_bucket_index(nb, xlo);
    for (;;) {
        const u64 *bk = t->slots + i * BS_BUCKET;
        for (int j = 0; j < BS_BUCKET; j++) {
            if (bk[j] == 0) return false;
            if (bk[j] == e) return true;
        }
        if (++i == nb) i = 0;
    }
}

/* -------------------------------------------------------------- persistence */

/* Two formats, the same 4 KiB header and the same body (the cells, little-endian, 8 bytes each, in memory order):
 *   PZBSGS01  one array of cells, linear probing, about 0.65 of the cells in use; read as it is (bsgs_table.linear)
 *   PZBSGS02  buckets of BS_BUCKET cells, BS_LOAD_NUM / BS_LOAD_DEN of the cells in use; what a built table is saved as
 * The old format cannot be turned into the new one without computing the baby steps again: a cell holds the 24 low bits of the
 * second word of x and the index b, but its home position is only known to within the length of the run it sits in. */
#define BS_MAGIC_V1 "PZBSGS01"
#define BS_MAGIC_V2 "PZBSGS02"
#define BS_HDR_BYTES 4096

typedef struct {
    char magic[8];
    u64 endian, version, T, nslots, tag_bits, val_bits, count, body_sum;
    u8 gx[32], tx[32];
    u64 bucket_cells;          /* version 2: cells per bucket; the padding of version 1, which is zero */
    u8 pad[BS_HDR_BYTES - 8 - 8 * 8 - 64 - 8 - 32];
    u8 sha[32];
} bs_header;

_Static_assert(sizeof(bs_header) == BS_HDR_BYTES, "header layout");

static u64 mix_slot(u64 v, u64 i)
{
    u64 x = (v + i * 0x9E3779B97F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL;
    x = (x << 31 | x >> 33) * 0x165667B19E3779F9ULL;
    return x ^ (x >> 29);
}

static void header_fill(bs_header *h, u64 T, u64 nslots, u64 body_sum, bool linear)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, linear ? BS_MAGIC_V1 : BS_MAGIC_V2, 8);
    h->endian = 0x0102030405060708ULL;
    h->version = linear ? 1 : 2;
    h->T = T;
    h->nslots = nslots;
    h->tag_bits = BS_TAG_BITS;
    h->val_bits = BS_VAL_BITS;
    h->count = T;
    h->body_sum = body_sum;
    h->bucket_cells = linear ? 0 : BS_BUCKET;
    pz_fe_to_be(h->gx, &pz_ec_g.x);
    pz_ge tg;
    if (!pz_ec_mul_g(&tg, &(pz_u256){{T, 0, 0, 0}})) pz_die("BSGS: invalid T*G");
    pz_fe_to_be(h->tx, &tg.x);
    pz_sha256((const u8 *)h, offsetof(bs_header, sha), h->sha);
}

static bool header_check(const bs_header *h, bool *linear, char *err, size_t errlen)
{
    u8 dg[32];
    const bool v1 = memcmp(h->magic, BS_MAGIC_V1, 8) == 0, v2 = memcmp(h->magic, BS_MAGIC_V2, 8) == 0;
    if (!v1 && !v2) {
        snprintf(err, errlen, memcmp(h->magic, "PZBSGS", 6) == 0 ? "table format not supported by this version of pzsolve" : "not a pzsolve BSGS table");
        return false;
    }
    pz_sha256((const u8 *)h, offsetof(bs_header, sha), dg);
    if (memcmp(dg, h->sha, 32) != 0) { snprintf(err, errlen, "table header corrupted"); return false; }
    if (h->endian != 0x0102030405060708ULL || h->version != (v1 ? 1 : 2) || h->tag_bits != BS_TAG_BITS || h->val_bits != BS_VAL_BITS || h->bucket_cells != (v1 ? 0 : BS_BUCKET)) {
        snprintf(err, errlen, "unsupported table format (version %llu)", (unsigned long long)h->version);
        return false;
    }
    if (h->T < 1 || h->T > BS_MAX_T || h->count != h->T) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    if (v2) {
        if (h->nslots != bsgs_slots_for(h->T)) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    } else {
        /* the size of the old tables came from a long double quotient: allow the one group of cells by which another platform may differ */
        const u64 want = linear_slots_for(h->T);
        if (h->nslots % 8 != 0 || h->nslots + 8 < want || h->nslots > want + 8 || h->nslots <= h->T) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    }
    pz_ec_init();
    u8 gx[32], tx[32];
    pz_ge tg;
    pz_fe_to_be(gx, &pz_ec_g.x);
    if (!pz_ec_mul_g(&tg, &(pz_u256){{h->T, 0, 0, 0}})) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    pz_fe_to_be(tx, &tg.x);
    if (memcmp(gx, h->gx, 32) != 0 || memcmp(tx, h->tx, 32) != 0) { snprintf(err, errlen, "the table does not match the secp256k1 curve"); return false; }
    *linear = v1;
    return true;
}

static bool full_pwrite(int fd, const void *buf, size_t n, u64 off)
{
    const char *p = buf;
    while (n) {
        i64 w = pz_pwrite(fd, p, n, off);
        if (w <= 0) return false;
        p += w; off += (u64)w; n -= (size_t)w;
    }
    return true;
}

static bool full_pread(int fd, void *buf, size_t n, u64 off)
{
    char *p = buf;
    while (n) {
        i64 r = pz_pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; off += (u64)r; n -= (size_t)r;
    }
    return true;
}

typedef struct {
    int fd;
    u64 *slots;
    u64 from, to;
    bool write, ok;
    u64 sum;
} io_arg;

enum { IO_BLOCK_SLOTS = 1 << 21 };   /* 16 MiB */

static void *io_thread(void *p)
{
    io_arg *a = p;
    a->ok = true;
    a->sum = 0;
    for (u64 i = a->from; i < a->to && a->ok; i += IO_BLOCK_SLOTS) {
        u64 n = a->to - i < IO_BLOCK_SLOTS ? a->to - i : IO_BLOCK_SLOTS;
        u64 off = (u64)BS_HDR_BYTES + i * 8;
        if (a->write) {
            a->ok = full_pwrite(a->fd, a->slots + i, n * 8, off);
        } else {
            a->ok = full_pread(a->fd, a->slots + i, n * 8, off);
            if (a->ok) pz_fd_drop_cache(a->fd, off, n * 8);
        }
        if (a->ok) for (u64 k = 0; k < n; k++) a->sum += mix_slot(a->slots[i + k], i + k);
    }
    return NULL;
}

static bool io_all(int fd, u64 *slots, u64 nslots, int nthreads, bool write, u64 *sum)
{
    if (nthreads < 1) nthreads = 1;
    io_arg *a = pz_xcalloc((size_t)nthreads, sizeof *a);
    pthread_t *th = pz_xcalloc((size_t)nthreads, sizeof *th);
    u64 per = (nslots + (u64)nthreads - 1) / (u64)nthreads;
    per = (per + IO_BLOCK_SLOTS - 1) / IO_BLOCK_SLOTS * IO_BLOCK_SLOTS;
    for (int i = 0; i < nthreads; i++) {
        u64 from = (u64)i * per > nslots ? nslots : (u64)i * per, to = from + per > nslots ? nslots : from + per;
        a[i] = (io_arg){fd, slots, from, to, write, true, 0};
        if (pthread_create(&th[i], NULL, io_thread, &a[i]) != 0) pz_die("pthread_create failed");
    }
    bool ok = true;
    u64 total = 0;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); ok &= a[i].ok; total += a[i].sum; }
    free(a); free(th);
    if (sum) *sum = total;
    return ok;
}

bool bsgs_table_save(const bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen)
{
    /* a private temporary file in the same directory (two campaigns saving to the same name cannot interleave), then rename */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    int fd = pz_create_temp(tmp);
    if (fd < 0) { snprintf(err, errlen, "cannot create a temporary file next to %s: %s", path, strerror(errno)); return false; }
    u64 sum = 0;
    bool ok = io_all(fd, t->slots, t->nslots, nthreads, true, &sum);
    if (!ok) { snprintf(err, errlen, "writing %s failed: %s", tmp, strerror(errno)); close(fd); unlink(tmp); return false; }
    bs_header h;
    header_fill(&h, t->T, t->nslots, sum, t->linear);
    ok = full_pwrite(fd, &h, sizeof h, 0) && pz_fd_sync(fd) == 0;   /* the header goes last: a partial file never validates */
    if (close(fd) != 0) ok = false;
    if (!ok || pz_rename_replace(tmp, path) != 0) { snprintf(err, errlen, "saving %s failed: %s", path, strerror(errno)); unlink(tmp); return false; }
    return true;
}

bool bsgs_table_peek_ram(const char *path, u64 *T, u64 *ram_bytes, char *err, size_t errlen)
{
    int fd = pz_open_read(path);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return false; }
    bs_header h;
    bool ok = full_pread(fd, &h, sizeof h, 0);
    close(fd);
    if (!ok) { snprintf(err, errlen, "%s is too short", path); return false; }
    bool linear;
    if (!header_check(&h, &linear, err, errlen)) return false;
    if (T) *T = h.T;
    if (ram_bytes) *ram_bytes = h.nslots * 8;
    return true;
}

bool bsgs_table_peek(const char *path, u64 *T, char *err, size_t errlen) { return bsgs_table_peek_ram(path, T, NULL, err, errlen); }

bool bsgs_table_load(bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen)
{
    int fd = pz_open_read(path);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return false; }
    bs_header h;
    if (!full_pread(fd, &h, sizeof h, 0)) { snprintf(err, errlen, "%s is too short", path); close(fd); return false; }
    bool linear;
    if (!header_check(&h, &linear, err, errlen)) { close(fd); return false; }
    i64 want = (i64)BS_HDR_BYTES + (i64)h.nslots * 8;
    i64 have = pz_fd_size(fd);
    if (have != want) {
        snprintf(err, errlen, "%s has size %lld instead of %lld (truncated file?)", path, (long long)have, (long long)want);
        close(fd);
        return false;
    }
    if (!table_alloc(t, h.T, h.nslots, linear, err, errlen)) { close(fd); return false; }
    u64 sum = 0;
    bool ok = io_all(fd, t->slots, t->nslots, nthreads, false, &sum);
    close(fd);
    if (!ok) { snprintf(err, errlen, "reading %s failed", path); bsgs_table_free(t); return false; }
    if (sum != h.body_sum) { snprintf(err, errlen, "the contents of %s are corrupted (different checksum)", path); bsgs_table_free(t); return false; }
    bool ordered;
    if (table_count(t, nthreads, &ordered) != t->T) { snprintf(err, errlen, "wrong number of entries in %s", path); bsgs_table_free(t); return false; }
    if (!ordered) { snprintf(err, errlen, "%s is not laid out as a BSGS table is", path); bsgs_table_free(t); return false; }
    /* spot check against freshly computed points */
    u64 rs = 0x9E3779B97F4A7C15ULL ^ h.T;
    for (int i = 0; i < 256; i++) {
        rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
        u64 b = 1 + (i == 0 ? 0 : i == 1 ? t->T - 1 : rs % t->T);
        pz_ge q;
        if (!pz_ec_mul_g(&q, &(pz_u256){{b, 0, 0, 0}})) continue;
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        if (!table_has_entry(t, x.n[0], x.n[1], b)) {
            snprintf(err, errlen, "spot check failed: %s does not contain baby step %llu", path, (unsigned long long)b);
            bsgs_table_free(t);
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- giant scan */

typedef struct {
    const bsgs_job *j;
    bsgs_found_fn found;
    void *user;
    _Atomic u64 *falsepos;
} scan_ctx;

static void try_candidate(scan_ctx *c, int64_t a, u64 b)
{
    const bsgs_params *p = c->j->p;
    pz_u256 k = candidate_key(p, a, b, +1);
    if (verify_key(p, &k)) { c->found(c->user, &k); return; }
    if (b) {
        k = candidate_key(p, a, b, -1);
        if (verify_key(p, &k)) { c->found(c->user, &k); return; }
    }
    if (c->falsepos) atomic_fetch_add_explicit(c->falsepos, 1, memory_order_relaxed);
}

/* The giant that entry k of a batch belongs to: the batch holds nl windows of BS_GRP giants, the x words interleaved by lane
 * (the scalar routine is the case nl = 1). It is only worked out for the rare entries that match. */
static inline int64_t giant_of(int64_t a_first, int nl, int k) { return a_first + (int64_t)(k % nl) * BS_GRP + k / nl; }

/* Old layout: every cell of the run that starts at the home cell and ends at the first free one, whose tag matches, is a candidate */
static inline void probe_linear(scan_ctx *c, u64 xlo, u64 xhi, int64_t a_first, int nl, int k)
{
    const bsgs_table *t = c->j->tab;
    const u64 tag = xhi & BS_TAG_MASK;
    u64 i = bs_slot_index(t->nslots, xlo);
    for (;;) {
        u64 s = t->slots[i];
        if (s == 0) return;
        if ((s >> BS_VAL_BITS) == tag) try_candidate(c, giant_of(a_first, nl, k), s & BS_VAL_MASK);
        if (++i == t->nslots) i = 0;
    }
}

/* Buckets: the home bucket, and the following ones while the last cell of the bucket is in use. One cache line each; the tags of the
 * 8 cells are compared at once, and the cells one by one only when one of them matches (about once in 10^6 lookups). */
static inline void probe_bucket(scan_ctx *c, u64 nb, u64 xlo, u64 xhi, int64_t a_first, int nl, int k)
{
    const u64 *cells = c->j->tab->slots;
    const u64 tag = xhi & BS_TAG_MASK;
    u64 i = bs_bucket_index(nb, xlo);
    for (;;) {
        const u64 *bk = cells + i * BS_BUCKET;
        if (UNLIKELY(bs_bucket_maybe(bk, tag)))
            for (int j = 0; j < BS_BUCKET; j++)
                if (bk[j] != 0 && (bk[j] >> BS_VAL_BITS) == tag) try_candidate(c, giant_of(a_first, nl, k), bk[j] & BS_VAL_MASK);
        if (bk[BS_BUCKET - 1] == 0) return;         /* a free cell: nothing was ever pushed past this bucket */
        if (++i == nb) i = 0;
    }
}

/* One x-coordinate, for the giant a (the exact slow path) */
static void probe(scan_ctx *c, u64 xlo, u64 xhi, int64_t a)
{
    const bsgs_table *t = c->j->tab;
    if (t->linear) probe_linear(c, xlo, xhi, a, 1, 0);
    else probe_bucket(c, t->nslots / BS_BUCKET, xlo, xhi, a, 1, 0);
}

/* The n x-coordinates of a batch, the lookups started 16 entries ahead so that their cache lines are on the way */
static void probe_batch(scan_ctx *c, const u64 *xlo, const u64 *xhi, int n, int nl, int64_t a_first)
{
    const bsgs_table *t = c->j->tab;
    const int ahead = 16;
    if (t->linear) {
        for (int k = 0; k < n; k++) {
            if (k + ahead < n) __builtin_prefetch(&t->slots[bs_slot_index(t->nslots, xlo[k + ahead])], 0, 1);
            probe_linear(c, xlo[k], xhi[k], a_first, nl, k);
        }
        return;
    }
    const u64 nb = t->nslots / BS_BUCKET;
    for (int k = 0; k < ahead && k < n; k++) __builtin_prefetch(&t->slots[bs_bucket_index(nb, xlo[k]) * BS_BUCKET], 0, 1);
    for (int k = 0; k < n; k++) {
        if (k + ahead < n) __builtin_prefetch(&t->slots[bs_bucket_index(nb, xlo[k + ahead]) * BS_BUCKET], 0, 1);
        probe_bucket(c, nb, xlo[k], xhi[k], a_first, nl, k);
    }
}

_Atomic u64 bsgs_slow_windows;

/* Exact, slow processing of the giants a_first .. a_first+count-1 (scalar multiplications). */
static void slow_giants(scan_ctx *c, int64_t a_first, int count)
{
    const bsgs_params *p = c->j->p;
    atomic_fetch_add_explicit(&bsgs_slow_windows, 1, memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        int64_t a = a_first + i;
        pz_ge q;
        if (!giant_point(p, a, &q)) { try_candidate(c, a, 0); continue; }   /* b = 0: Q is the point at infinity */
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        probe(c, x.n[0], x.n[1], a);
    }
}

u64 bsgs_scan(const bsgs_job *j, int64_t a0, u64 n, bsgs_found_fn found, void *user, _Atomic u64 *progress, _Atomic u64 *falsepos, _Atomic int *stop)
{
    const bsgs_params *p = j->p;
    if (p->Pp_inf) pz_die("bsgs_scan: the key is the centre of the range (to be handled first)");
    bs_group_fn gf = pick_group();
    const bs_vengine *ve = pick_vengine();
    void *vt = ve ? ve->prepare(&j->giant) : NULL;
    const int nl = vt ? ve->lanes : 1;
    pz_fe *dx = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1));
    u64 *xlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *xhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    void *scr = vt ? pz_xaligned(64, ve->scratch) : NULL;
    pz_ge *Cv = pz_xcalloc((size_t)nl, sizeof(pz_ge));
    bool vvalid = false;
    scan_ctx ctx = {j, found, user, falsepos};
    const u64 ngroups = (n + BS_GRP - 1) / BS_GRP;
    u64 g = 0;
    pz_ge C;
    bool cvalid = false;
    for (; g < ngroups; g++) {
        if (stop && atomic_load_explicit(stop, memory_order_relaxed)) break;
        const int64_t a_first = a0 + (int64_t)(g * BS_GRP);
        if (vt && g + (u64)nl <= ngroups) {
            /* nl consecutive windows at once; their centres are recomputed after any exceptional window */
            if (!vvalid) {
                vvalid = true;
                for (int l = 0; l < nl && vvalid; l++) vvalid = giant_point(p, a_first + (int64_t)l * BS_GRP + BS_HALF, &Cv[l]);
            }
            if (vvalid && ve->groups(vt, Cv, xlo, xhi, scr)) {
                probe_batch(&ctx, xlo, xhi, BS_GRP * nl, nl, a_first);
            } else {
                for (int l = 0; l < nl; l++) slow_giants(&ctx, a_first + (int64_t)l * BS_GRP, BS_GRP);
                vvalid = false;
            }
            if (progress) atomic_fetch_add_explicit(progress, (u64)BS_GRP * nl, memory_order_relaxed);
            g += (u64)nl - 1;
            cvalid = false;
            continue;
        }
        const int64_t a_c = a_first + BS_HALF;
        if (!cvalid) cvalid = giant_point(p, a_c, &C);
        if (cvalid && gf(&j->giant, &C, xlo, xhi, dx, tmp)) {
            probe_batch(&ctx, xlo, xhi, BS_GRP, 1, a_first);
        } else {
            /* some point of the window is the point at infinity (the key sits exactly on a giant step) or collides with
             * the centre: process the window exactly and restart the incremental chain at the next centre */
            slow_giants(&ctx, a_first, BS_GRP);
            cvalid = false;
        }
        if (progress) atomic_fetch_add_explicit(progress, BS_GRP, memory_order_relaxed);
    }
    pz_afree(dx); pz_afree(tmp); pz_afree(xlo); pz_afree(xhi); pz_afree(scr); pz_afree(vt); free(Cv);
    u64 covered = g * BS_GRP;
    return covered > n ? n : covered;
}

/* ------------------------------------------------------------------- solve */

typedef struct {
    const bsgs_job *j;
    int64_t a0;
    u64 n;
    _Atomic int *stop;
    pz_u256 *key;
    _Atomic int *hit;
    _Atomic u64 *scanned;
} solve_arg;

static void solve_found(void *user, const pz_u256 *key)
{
    solve_arg *a = user;
    if (atomic_exchange(a->hit, 1) == 0) *a->key = *key;
    atomic_store(a->stop, 1);
}

static void *solve_thread(void *p)
{
    solve_arg *a = p;
    bsgs_scan(a->j, a->a0, a->n, solve_found, a, a->scanned, NULL, a->stop);
    return NULL;
}

bool bsgs_solve(const pz_u256 *lo, const pz_u256 *hi, const pz_ge *pub, u64 T, int nthreads, pz_u256 *key, u64 *giants)
{
    char err[200];
    bsgs_params p;
    if (!bsgs_params_init(&p, lo, hi, pub, T, err, sizeof err)) pz_die("bsgs_solve: %s", err);
    if (giants) *giants = 0;
    if (p.Pp_inf) { *key = p.c; return true; }
    bsgs_table tab;
    if (!bsgs_table_alloc(&tab, T, err, sizeof err)) pz_die("bsgs_solve: %s", err);
    bsgs_table_build(&tab, nthreads, NULL, NULL);
    bsgs_job job;
    bsgs_job_init(&job, &tab, &p);
    if (nthreads < 1) nthreads = 1;
    _Atomic int stop = 0, hit = 0;
    _Atomic u64 scanned = 0;
    pz_u256 found = pz_u256_zero();
    solve_arg *args = pz_xcalloc((size_t)nthreads, sizeof *args);
    pthread_t *th = pz_xcalloc((size_t)nthreads, sizeof *th);
    u64 per = (p.Ng + (u64)nthreads - 1) / (u64)nthreads;
    for (int i = 0; i < nthreads; i++) {
        u64 from = (u64)i * per;
        u64 cnt = from >= p.Ng ? 0 : (p.Ng - from < per ? p.Ng - from : per);
        args[i] = (solve_arg){&job, -(int64_t)p.A + (int64_t)from, cnt, &stop, &found, &hit, &scanned};
        if (pthread_create(&th[i], NULL, solve_thread, &args[i]) != 0) pz_die("pthread_create failed");
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    free(args); free(th);
    bsgs_table_free(&tab);
    if (giants) *giants = atomic_load(&scanned);
    if (atomic_load(&hit)) { *key = found; return true; }
    return false;
}
