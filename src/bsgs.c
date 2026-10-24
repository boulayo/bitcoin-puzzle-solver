/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs.c - baby-step giant-step with a symmetric RAM table. See bsgs.h. */
#include "bsgs_priv.h"
#include "cpu_engine.h"
#include "hash.h"
#include "kangaroo.h"
#include "platform.h"
#include "outfmt.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

/* ------------------------------------------------------------------ sizing */

u64 bsgs_slots_for(u64 T)
{
    u64 n = (u64)ceill((long double)T / (long double)BS_LOAD_FACTOR) + 64;
    return (n + 7) & ~(u64)7;
}

u64 bsgs_bytes_for(u64 T) { return bsgs_slots_for(T) * 8; }

u64 bsgs_max_T_for_bytes(u64 bytes)
{
    u64 slots = bytes / 8;
    if (slots <= 72) return 0;
    long double t = (long double)(slots - 72) * (long double)BS_LOAD_FACTOR;
    if (t >= (long double)BS_MAX_T) return BS_MAX_T;
    return (u64)t;
}

u64 bsgs_optimal_T(const u256 *W)
{
    long double w = u256_to_ld(W);
    long double t = ceill(sqrtl(w) / 2.0L);
    if (t < 1) t = 1;
    if (t > (long double)BS_MAX_T) return 0;
    return (u64)t;
}

long double bsgs_mean_cost(const u256 *W, u64 T)
{
    long double w = u256_to_ld(W);
    return (long double)T + w / (4.0L * (long double)T);
}

u64 bsgs_choose_T(const u256 *W, u64 budget_bytes, bool *viable)
{
    u64 topt = bsgs_optimal_T(W), tmax = bsgs_max_T_for_bytes(budget_bytes);
    u64 T = topt ? (topt < tmax ? topt : tmax) : tmax;
    bool ok = false;
    if (T >= 1) ok = bsgs_mean_cost(W, T) <= KANG_C_MEAN * sqrtl(u256_to_ld(W));
    if (viable) *viable = ok;
    return T;
}

/* ------------------------------------------------------------- parameters */

static bool u256_to_u128_checked(const u256 *a, u128 *out)
{
    if (a->w[2] || a->w[3]) return false;
    *out = (u128)a->w[1] << 64 | a->w[0];
    return true;
}

bool bsgs_params_init(bsgs_params *p, const u256 *lo, const u256 *hi, const ge *pub, u64 T, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    if (T < 1 || T > BS_MAX_T) { snprintf(err, errlen, "invalid number of baby steps (%llu)", (unsigned long long)T); return false; }
    if (u256_cmp(lo, hi) > 0 || u256_is_zero(lo)) { snprintf(err, errlen, "invalid range"); return false; }
    p->lo = *lo;
    p->hi = *hi;
    u256 one = u256_from_u64(1), W;
    u256_sub(&W, hi, lo);
    u256_add(&W, &W, &one);
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
    u256 half = u256_from_u128((w128 - 1) >> 1);
    u256_add(&p->c, lo, &half);
    p->P = *pub;
    ec_init();
    ge cG;
    if (!ec_mul_g(&cG, &p->c)) { snprintf(err, errlen, "invalid range centre"); return false; }
    p->Pp_inf = !ge_sub(&p->Pp, pub, &cG);
    return true;
}

/* d = key - c as a signed 128-bit value (the key is within H of c, so it fits). */
static __int128 key_minus_c(const bsgs_params *p, const u256 *key)
{
    u256 d;
    if (u256_cmp(key, &p->c) >= 0) {
        u256_sub(&d, key, &p->c);
        return (__int128)((u128)d.w[1] << 64 | d.w[0]);
    }
    u256_sub(&d, &p->c, key);
    return -(__int128)((u128)d.w[1] << 64 | d.w[0]);
}

u64 bsgs_giant_index(const bsgs_params *p, const u256 *key)
{
    if (u256_cmp(key, &p->lo) < 0 || u256_cmp(key, &p->hi) > 0) return 0;
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
    out->range_start = u256_from_u64(1);
    out->range_end = u256_from_u64(p->Ng);
    u256 rsz = u256_from_u64(p->Ng);
    unsigned cb = cfg_default_chunk_bits(&rsz);
    u256 chunk = u256_pow2(cb);
    if (u256_cmp(&chunk, &rsz) > 0) {
        unsigned b = u256_bits(&rsz) - 1;
        cb = b < 1 ? 1 : b;
    }
    out->chunk_bits = cb;
}

/* ------------------------------------------------------------ giant points */

/* q = Pp - a*S*G.  Returns false when q is the point at infinity (the key is then c + a*S exactly). */
static bool giant_point(const bsgs_params *p, int64_t a, ge *q)
{
    __int128 m = (__int128)a * (__int128)p->S;
    if (m == 0) { *q = p->Pp; return true; }
    u128 mag = (u128)(m < 0 ? -m : m);
    u256 k = u256_from_u128(mag);
    ge mG;
    if (!ec_mul_g(&mG, &k)) return false;
    return m > 0 ? ge_sub(q, &p->Pp, &mG) : ge_add(q, &p->Pp, &mG);
}

/* c + a*S + sgn*b  (mod n) */
static u256 candidate_key(const bsgs_params *p, int64_t a, u64 b, int sgn)
{
    __int128 m = (__int128)a * (__int128)p->S + (sgn < 0 ? -(__int128)b : (__int128)b);
    u128 mag = (u128)(m < 0 ? -m : m);
    u256 mg = u256_from_u128(mag), r;
    if (m >= 0) u256_addmod_n(&r, &p->c, &mg);
    else u256_submod_n(&r, &p->c, &mg);
    return r;
}

static bool verify_key(const bsgs_params *p, const u256 *key)
{
    ge q;
    if (u256_is_zero(key) || !ec_mul_g(&q, key)) return false;
    return ge_equal(&q, &p->P);
}

/* ------------------------------------------------------------------ steps */

static void steps_make(bs_steps *st, const ge *U, u64 kmag, bool negated)
{
    gej *pts = pz_xmalloc(sizeof(gej) * (BS_HALF + 1));
    ge *aff = pz_xmalloc(sizeof(ge) * (BS_HALF + 1));
    gej cur;
    gej_set_ge(&cur, U);
    pts[0] = cur;
    for (int i = 1; i < BS_HALF; i++) {
        gej_add_ge(&cur, &cur, U);
        pts[i] = cur;
    }
    gej_double(&cur, &pts[BS_HALF - 1]);
    pts[BS_HALF] = cur;
    gej_to_ge_batch(aff, pts, BS_HALF + 1);
    memcpy(st->tab, aff, sizeof(ge) * BS_HALF);
    st->step = aff[BS_HALF];
    /* independent check of the last table entry and of the step with scalar multiplications */
    ge chk, chk2;
    u256 k1 = u256_from_u128((u128)kmag * BS_HALF), k2 = u256_from_u128((u128)kmag * BS_GRP);
    if (!ec_mul_g(&chk, &k1) || !ec_mul_g(&chk2, &k2)) pz_die("BSGS: invalid step table");
    if (negated) { ge_neg(&chk, &chk); ge_neg(&chk2, &chk2); }
    if (!ge_equal(&chk, &st->tab[BS_HALF - 1]) || !ge_equal(&chk2, &st->step)) pz_die("BSGS: step table check failed");
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
    steps_make(st, &EC_G, 1, false);
    void *vt = ve->prepare(st);
    const int nl = ve->lanes;
    ge *C = pz_xcalloc((size_t)nl, sizeof(ge));
    u64 *vlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *vhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    u64 *slo = pz_xaligned(64, sizeof(u64) * BS_GRP), *shi = pz_xaligned(64, sizeof(u64) * BS_GRP);
    fe *dx = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1));
    void *scr = pz_xaligned(64, ve->scratch);
    bool ok = vt != NULL;
    const u64 k0 = 7777;
    for (int l = 0; l < nl && ok; l++) ok = ec_mul_g(&C[l], &(u256){{k0 + BS_HALF + (u64)l * BS_GRP, 0, 0, 0}});
    for (int call = 0; call < 2 && ok; call++) {
        ok = ve->groups(vt, C, vlo, vhi, scr);
        for (int l = 0; l < nl && ok; l++) {
            ge S;
            ok = ec_mul_g(&S, &(u256){{k0 + BS_HALF + (u64)(call * nl + l) * BS_GRP, 0, 0, 0}}) && bs_group_base(st, &S, slo, shi, dx, tmp);
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
    const char *off = getenv("PZ_BSGS_SCALAR");                             /* diagnostics: scalar windows only */
    if (off && off[0] == '1') return;
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
    ec_init();
    j->tab = t;
    j->p = p;
    ge SG;
    if (!ec_mul_g(&SG, &(u256){{p->S, 0, 0, 0}})) pz_die("BSGS: invalid S*G");
    ge U;
    ge_neg(&U, &SG);
    steps_make(&j->giant, &U, p->S, true);
}

/* --------------------------------------------------------------- the table */

bool bsgs_table_alloc(bsgs_table *t, u64 T, char *err, size_t errlen)
{
    memset(t, 0, sizeof *t);
    if (T < 1 || T > BS_MAX_T) { snprintf(err, errlen, "invalid table size"); return false; }
    t->T = T;
    t->nslots = bsgs_slots_for(T);
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
    uintptr_t a = ((uintptr_t)m + align - 1) & ~(uintptr_t)(align - 1);
    t->slots = (u64 *)a;
    t->thp = huge;
    return true;
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
    const bs_vengine *ve = pick_vengine();
    void *vt = ve ? ve->prepare(a->st) : NULL;
    const int nl = vt ? ve->lanes : 1;
    fe *dx = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1));
    u64 *xlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *xhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    void *scr = vt ? pz_xaligned(64, ve->scratch) : NULL;
    ge *Cv = pz_xcalloc((size_t)nl, sizeof(ge));
    for (;;) {
        u64 blk = atomic_fetch_add(a->next_block, 1);
        if (blk >= a->nblocks) break;
        const u64 b0 = 1 + blk * a->blk;
        const u64 bend = b0 + a->blk < t->T + 1 ? b0 + a->blk : t->T + 1;   /* exclusive */
        u64 first = b0;
        if (vt) {
            /* nl windows at once while they fit in the block (baby steps are small multiples of G: no zero denominator) */
            for (int l = 0; l < nl; l++)
                if (!ec_mul_g(&Cv[l], &(u256){{b0 + BS_HALF + (u64)l * BS_GRP, 0, 0, 0}})) pz_die("BSGS: invalid centre");
            for (; first + (u64)BS_GRP * nl <= bend; first += (u64)BS_GRP * nl) {
                if (atomic_load_explicit(a->stop, memory_order_relaxed)) { a->stopped = true; goto done; }
                if (!ve->groups(vt, Cv, xlo, xhi, scr)) pz_die("BSGS: zero denominator while building the table");
                const int tot = BS_GRP * nl;
                for (int k = 0; k < tot; k++) {
                    if (k + 16 < tot) __builtin_prefetch(&t->slots[bs_slot_index(t->nslots, xlo[k + 16])], 1, 1);
                    if (bs_insert(t->slots, t->nslots, xlo[k], xhi[k], first + (u64)(k % nl) * BS_GRP + (u64)(k / nl))) a->inserted++;
                }
                if (a->units) atomic_fetch_add_explicit(a->units, (u64)tot, memory_order_relaxed);
            }
        }
        if (first >= bend) continue;
        ge C;
        if (!ec_mul_g(&C, &(u256){{first + BS_HALF, 0, 0, 0}})) pz_die("BSGS: invalid centre");
        for (; first < bend; first += BS_GRP) {
            if (atomic_load_explicit(a->stop, memory_order_relaxed)) { a->stopped = true; goto done; }
            if (!a->gf(a->st, &C, xlo, xhi, dx, tmp)) pz_die("BSGS: zero denominator while building the table");
            u64 cnt = bend - first < BS_GRP ? bend - first : BS_GRP;
            for (u64 i = 0; i < cnt; i++) {
                if (i + 16 < cnt) __builtin_prefetch(&t->slots[bs_slot_index(t->nslots, xlo[i + 16])], 1, 1);
                if (bs_insert(t->slots, t->nslots, xlo[i], xhi[i], first + i)) a->inserted++;
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
    ec_init();
    if (nthreads < 1) nthreads = 1;
    bs_steps *st = pz_xmalloc(sizeof *st);
    steps_make(st, &EC_G, 1, false);
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

typedef struct { const u64 *s; u64 n, cnt; } count_arg;
static void *count_thread(void *p)
{
    count_arg *a = p;
    u64 c = 0;
    for (u64 i = 0; i < a->n; i++) c += a->s[i] != 0;
    a->cnt = c;
    return NULL;
}

u64 bsgs_table_count(const bsgs_table *t, int nthreads)
{
    if (nthreads < 1) nthreads = 1;
    count_arg *a = pz_xcalloc((size_t)nthreads, sizeof *a);
    pthread_t *th = pz_xcalloc((size_t)nthreads, sizeof *th);
    u64 per = (t->nslots + (u64)nthreads - 1) / (u64)nthreads;
    for (int i = 0; i < nthreads; i++) {
        u64 from = (u64)i * per, to = from + per < t->nslots ? from + per : t->nslots;
        a[i] = (count_arg){t->slots + (from < to ? from : t->nslots), from < to ? to - from : 0, 0};
        if (pthread_create(&th[i], NULL, count_thread, &a[i]) != 0) pz_die("pthread_create failed");
    }
    u64 total = 0;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); total += a[i].cnt; }
    free(a); free(th);
    return total;
}

bool bsgs_table_contains(const bsgs_table *t, u64 xlo, u64 xhi, u64 *b_out)
{
    const u64 tag = xhi & (((u64)1 << BS_TAG_BITS) - 1);
    u64 i = bs_slot_index(t->nslots, xlo);
    for (;;) {
        u64 s = t->slots[i];
        if (s == 0) return false;
        if ((s >> BS_VAL_BITS) == tag) { if (b_out) *b_out = s & BS_VAL_MASK; return true; }
        if (++i == t->nslots) i = 0;
    }
}

/* true when the table holds exactly the entry for (x, b) */
static bool table_has_entry(const bsgs_table *t, u64 xlo, u64 xhi, u64 b)
{
    const u64 e = bs_entry(xhi, b);
    u64 i = bs_slot_index(t->nslots, xlo);
    for (;;) {
        u64 s = t->slots[i];
        if (s == 0) return false;
        if (s == e) return true;
        if (++i == t->nslots) i = 0;
    }
}

/* -------------------------------------------------------------- persistence */

#define BS_FILE_MAGIC "PZBSGS01"
#define BS_HDR_BYTES 4096

typedef struct {
    char magic[8];
    u64 endian, version, T, nslots, tag_bits, val_bits, count, body_sum;
    u8 gx[32], tx[32];
    u8 pad[BS_HDR_BYTES - 8 - 8 * 8 - 64 - 32];
    u8 sha[32];
} bs_header;

_Static_assert(sizeof(bs_header) == BS_HDR_BYTES, "header layout");

static u64 mix_slot(u64 v, u64 i)
{
    u64 x = (v + i * 0x9E3779B97F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL;
    x = (x << 31 | x >> 33) * 0x165667B19E3779F9ULL;
    return x ^ (x >> 29);
}

static void header_fill(bs_header *h, u64 T, u64 nslots, u64 body_sum)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, BS_FILE_MAGIC, 8);
    h->endian = 0x0102030405060708ULL;
    h->version = 1;
    h->T = T;
    h->nslots = nslots;
    h->tag_bits = BS_TAG_BITS;
    h->val_bits = BS_VAL_BITS;
    h->count = T;
    h->body_sum = body_sum;
    fe_to_be(h->gx, &EC_G.x);
    ge tg;
    if (!ec_mul_g(&tg, &(u256){{T, 0, 0, 0}})) pz_die("BSGS: invalid T*G");
    fe_to_be(h->tx, &tg.x);
    sha256((const u8 *)h, offsetof(bs_header, sha), h->sha);
}

static bool header_check(const bs_header *h, char *err, size_t errlen)
{
    u8 dg[32];
    if (memcmp(h->magic, BS_FILE_MAGIC, 8) != 0) { snprintf(err, errlen, "not a pzsolve BSGS table"); return false; }
    sha256((const u8 *)h, offsetof(bs_header, sha), dg);
    if (memcmp(dg, h->sha, 32) != 0) { snprintf(err, errlen, "table header corrupted"); return false; }
    if (h->endian != 0x0102030405060708ULL || h->version != 1 || h->tag_bits != BS_TAG_BITS || h->val_bits != BS_VAL_BITS) {
        snprintf(err, errlen, "unsupported table format (version %llu)", (unsigned long long)h->version);
        return false;
    }
    if (h->T < 1 || h->T > BS_MAX_T || h->nslots != bsgs_slots_for(h->T) || h->count != h->T) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    ec_init();
    u8 gx[32], tx[32];
    ge tg;
    fe_to_be(gx, &EC_G.x);
    if (!ec_mul_g(&tg, &(u256){{h->T, 0, 0, 0}})) { snprintf(err, errlen, "inconsistent table parameters"); return false; }
    fe_to_be(tx, &tg.x);
    if (memcmp(gx, h->gx, 32) != 0 || memcmp(tx, h->tx, 32) != 0) { snprintf(err, errlen, "the table does not match the secp256k1 curve"); return false; }
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
    header_fill(&h, t->T, t->nslots, sum);
    ok = full_pwrite(fd, &h, sizeof h, 0) && pz_fd_sync(fd) == 0;   /* the header goes last: a partial file never validates */
    if (close(fd) != 0) ok = false;
    if (!ok || pz_rename_replace(tmp, path) != 0) { snprintf(err, errlen, "saving %s failed: %s", path, strerror(errno)); unlink(tmp); return false; }
    return true;
}

bool bsgs_table_peek(const char *path, u64 *T, char *err, size_t errlen)
{
    int fd = pz_open_read(path);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return false; }
    bs_header h;
    bool ok = full_pread(fd, &h, sizeof h, 0);
    close(fd);
    if (!ok) { snprintf(err, errlen, "%s is too short", path); return false; }
    if (!header_check(&h, err, errlen)) return false;
    if (T) *T = h.T;
    return true;
}

bool bsgs_table_load(bsgs_table *t, const char *path, int nthreads, char *err, size_t errlen)
{
    int fd = pz_open_read(path);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return false; }
    bs_header h;
    if (!full_pread(fd, &h, sizeof h, 0)) { snprintf(err, errlen, "%s is too short", path); close(fd); return false; }
    if (!header_check(&h, err, errlen)) { close(fd); return false; }
    i64 want = (i64)BS_HDR_BYTES + (i64)h.nslots * 8;
    i64 have = pz_fd_size(fd);
    if (have != want) {
        snprintf(err, errlen, "%s has size %lld instead of %lld (truncated file?)", path, (long long)have, (long long)want);
        close(fd);
        return false;
    }
    if (!bsgs_table_alloc(t, h.T, err, errlen)) { close(fd); return false; }
    u64 sum = 0;
    bool ok = io_all(fd, t->slots, t->nslots, nthreads, false, &sum);
    close(fd);
    if (!ok) { snprintf(err, errlen, "reading %s failed", path); bsgs_table_free(t); return false; }
    if (sum != h.body_sum) { snprintf(err, errlen, "the contents of %s are corrupted (different checksum)", path); bsgs_table_free(t); return false; }
    if (bsgs_table_count(t, nthreads) != t->T) { snprintf(err, errlen, "wrong number of entries in %s", path); bsgs_table_free(t); return false; }
    /* spot check against freshly computed points */
    u64 rs = 0x9E3779B97F4A7C15ULL ^ h.T;
    for (int i = 0; i < 256; i++) {
        rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
        u64 b = 1 + (i == 0 ? 0 : i == 1 ? t->T - 1 : rs % t->T);
        ge q;
        if (!ec_mul_g(&q, &(u256){{b, 0, 0, 0}})) continue;
        fe x = q.x;
        fe_normalize(&x);
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
    u256 k = candidate_key(p, a, b, +1);
    if (verify_key(p, &k)) { c->found(c->user, &k); return; }
    if (b) {
        k = candidate_key(p, a, b, -1);
        if (verify_key(p, &k)) { c->found(c->user, &k); return; }
    }
    if (c->falsepos) atomic_fetch_add_explicit(c->falsepos, 1, memory_order_relaxed);
}

static inline void probe(scan_ctx *c, u64 xlo, u64 xhi, int64_t a)
{
    const bsgs_table *t = c->j->tab;
    const u64 tag = xhi & (((u64)1 << BS_TAG_BITS) - 1);
    u64 i = bs_slot_index(t->nslots, xlo);
    for (;;) {
        u64 s = t->slots[i];
        if (s == 0) return;
        if ((s >> BS_VAL_BITS) == tag) try_candidate(c, a, s & BS_VAL_MASK);
        if (++i == t->nslots) i = 0;
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
        ge q;
        if (!giant_point(p, a, &q)) { try_candidate(c, a, 0); continue; }   /* b = 0: Q is the point at infinity */
        fe x = q.x;
        fe_normalize(&x);
        probe(c, x.n[0], x.n[1], a);
    }
}

u64 bsgs_scan(const bsgs_job *j, int64_t a0, u64 n, bsgs_found_fn found, void *user, _Atomic u64 *progress, _Atomic u64 *falsepos, _Atomic int *stop)
{
    const bsgs_params *p = j->p;
    const bsgs_table *t = j->tab;
    if (p->Pp_inf) pz_die("bsgs_scan: the key is the centre of the range (to be handled first)");
    bs_group_fn gf = pick_group();
    const bs_vengine *ve = pick_vengine();
    void *vt = ve ? ve->prepare(&j->giant) : NULL;
    const int nl = vt ? ve->lanes : 1;
    fe *dx = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(fe) * (BS_HALF + 1));
    u64 *xlo = pz_xaligned(64, sizeof(u64) * BS_GRP * nl), *xhi = pz_xaligned(64, sizeof(u64) * BS_GRP * nl);
    void *scr = vt ? pz_xaligned(64, ve->scratch) : NULL;
    ge *Cv = pz_xcalloc((size_t)nl, sizeof(ge));
    bool vvalid = false;
    scan_ctx ctx = {j, found, user, falsepos};
    const u64 ngroups = (n + BS_GRP - 1) / BS_GRP;
    u64 g = 0;
    ge C;
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
                const int tot = BS_GRP * nl;
                for (int k = 0; k < tot; k++) {
                    if (k + 16 < tot) __builtin_prefetch(&t->slots[bs_slot_index(t->nslots, xlo[k + 16])], 0, 1);
                    probe(&ctx, xlo[k], xhi[k], a_first + (int64_t)(k % nl) * BS_GRP + k / nl);
                }
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
            for (int i = 0; i < BS_GRP; i++) {
                if (i + 16 < BS_GRP) __builtin_prefetch(&t->slots[bs_slot_index(t->nslots, xlo[i + 16])], 0, 1);
                probe(&ctx, xlo[i], xhi[i], a_first + i);
            }
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
    u256 *key;
    _Atomic int *hit;
    _Atomic u64 *scanned;
} solve_arg;

static void solve_found(void *user, const u256 *key)
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

bool bsgs_solve(const u256 *lo, const u256 *hi, const ge *pub, u64 T, int nthreads, u256 *key, u64 *giants)
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
    u256 found = u256_zero();
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
