/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* engine_vec_impl.inc - CPU linear-search engine with lane-parallel elliptic-curve arithmetic (pz_fev.h), instantiated once
 * per vector width. The includer defines ISA (token) and PZ_VBYTES (32 or 64) and VHALF (points on each side of a centre).
 *
 * N = PZ_VBYTES/8 independent windows are processed at once, one per vector lane. Round g covers N consecutive windows of
 * GRP = 2*VHALF keys starting at key_start + g*N*GRP; lane l takes window g*N + l, so after any number of rounds
 * the keys covered form a prefix of the claim (the scheduler relies on that).
 *
 * Per round and lane: one Montgomery batch inversion of the VHALF+1 denominators x(i*G) - x(C), then the points
 * C +- i*G, whose compressed public keys are hashed 2N at a time with the vectorized SHA-256 + RIPEMD-160 pipeline.
 *
 * The field arithmetic is pz_fev.h (9 limbs of 29 bits, vpmuludq) or, when the includer defines PZ_FEV_IFMA, fev52.h (5 limbs
 * of 52 bits, AVX-512 IFMA); this file only relies on their common interface. */

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define PZ_HASH_LANES_FN CAT(hash_lanes_, ISA)
#include <pzcore/impl/hash_lanes.inc>
/* With 32 vector registers the search runs faster with the products, vctx_emit and vround inlined (measured: 2.7 % on avx512v,
 * 0 to 2 % on avx512i); with the 16 of AVX2 the spills make it slower (-1 to -2.4 %). */
#if PZ_VBYTES == 64
#define PZ_FEV_INLINE __attribute__((always_inline))
#define VINLINE __attribute__((always_inline))
#define VFLUSH __attribute__((noinline))    /* the hash of the batches stays out of the loop of the step: called once per 8 iterations */
#else
#define VINLINE
#define VFLUSH
#endif
#ifdef PZ_FEV_IFMA
#include <pzcore/fev52.h>
#else
#include <pzcore/fev.h>
#endif
#include <pthread.h>

#define NL PZ_FEV_LANES            /* elliptic-curve lanes */
#define GRPV (2 * VHALF)        /* keys per window */
#if PZ_LANES != 2 * NL
#error "the hash width must be twice the number of curve lanes"
#endif

/* The candidates are hashed VBATCHES batches of PZ_LANES at a time: the curve arithmetic of several rounds of the loop in vround
 * then runs back to back, and out-of-order execution overlaps the dependency chains of consecutive points instead of stopping
 * at every call of the hash. */
#ifndef VBATCHES
#define VBATCHES 8
#endif

typedef struct {
    u32 X[8][PZ_LANES] PZ_ALIGN(64);
    u32 pfx[PZ_LANES] PZ_ALIGN(64);
    u64 koff[PZ_LANES] PZ_ALIGN(64);
} vbatch_t;

typedef struct {
    vbatch_t b[VBATCHES];
    u32 outw[PZ_LANES] PZ_ALIGN(64);      /* the filter word of the digests */
    int n;                    /* candidates in b[], filled in order */
    const scan_job *job;
    pz_u256 base;             /* first key of the claim */
} vctx_t;

static PZ_VABI void vctx_verify(vctx_t *c, const vbatch_t *b, int lane)
{
    u8 pub[33], h[20];
    pub[0] = (u8)b->pfx[lane];
    for (int i = 0; i < 8; i++) {
        u32 w = b->X[i][lane];
        pub[1 + 4 * i] = (u8)(w >> 24); pub[2 + 4 * i] = (u8)(w >> 16);
        pub[3 + 4 * i] = (u8)(w >> 8);  pub[4 + 4 * i] = (u8)w;
    }
    pz_hash160(pub, 33, h);
    if (memcmp(h, c->job->target_h160, 20) != 0) return;
    pz_u256 key;
    pz_u256 d = pz_u256_from_u64(b->koff[lane]);
    pz_u256_add(&key, &c->base, &d);
    if (c->job->on_hit) c->job->on_hit(c->job->user, &key);
}

static VFLUSH PZ_VABI void vctx_flush(vctx_t *c)
{
#ifdef PZ_PROF_NOHASH
    c->n = 0;
    return;
#endif
    const u32 t0 = c->job->target_w[PZ_HASH_FILTER_WORD];
    const pz_vu t = pz_v_set(t0);
    for (int k = 0; k * PZ_LANES < c->n; k++) {
        vbatch_t *b = &c->b[k];
        PZ_HASH_FILTER_FN(&b->X[0][0], b->pfx, c->outw);
        /* one vector comparison for the batch; lanes past n hold older keys, which the loop skips */
        const pz_vu w = pz_v_load(c->outw);
#if PZ_VBYTES == 64
        const bool any = _mm512_cmpeq_epi32_mask((__m512i)w, (__m512i)t) != 0;
#else
        const bool any = _mm256_movemask_epi8(_mm256_cmpeq_epi32((__m256i)w, (__m256i)t)) != 0;
#endif
        if (UNLIKELY(any))
            for (int l = 0; l < PZ_LANES && k * PZ_LANES + l < c->n; l++)
                if (c->outw[l] == t0) vctx_verify(c, b, l);
    }
    c->n = 0;
}

/* one candidate from the scalar fallback path */
static PZ_VABI void vctx_add_scalar(vctx_t *c, const pz_fe *x, const pz_fe *y, u64 koff)
{
    pz_fe xn = *x, yn = *y;
    pz_fe_normalize(&xn);
    pz_fe_normalize(&yn);
    vbatch_t *b = &c->b[c->n / PZ_LANES];
    const int l = c->n % PZ_LANES;
    b->X[0][l] = (u32)(xn.n[3] >> 32); b->X[1][l] = (u32)xn.n[3];
    b->X[2][l] = (u32)(xn.n[2] >> 32); b->X[3][l] = (u32)xn.n[2];
    b->X[4][l] = (u32)(xn.n[1] >> 32); b->X[5][l] = (u32)xn.n[1];
    b->X[6][l] = (u32)(xn.n[0] >> 32); b->X[7][l] = (u32)xn.n[0];
    b->pfx[l] = 2u + (u32)(yn.n[0] & 1);
    b->koff[l] = koff;
    if (++c->n == VBATCHES * PZ_LANES) vctx_flush(c);
}

typedef u32 vw __attribute__((vector_size(NL * 4)));

static inline PZ_VABI bool vl_any(pz_vl v)
{
#if PZ_VBYTES == 64
    return _mm512_test_epi64_mask((__m512i)v, (__m512i)v) != 0;
#else
    return !_mm256_testz_si256((__m256i)v, (__m256i)v);
#endif
}

/* Canonical x and the parity of the canonical y, then NL candidates appended to the hash batch. The fast forms cover all but
 * about 2^-24 of the lanes (2^-48 with IFMA); for those the full normalization runs on the whole vector. */
static inline VINLINE PZ_VABI void vctx_emit(vctx_t *c, pz_fev *x, const pz_fev *y, pz_vl koff)
{
    pz_vl unsure = pz_fev_normalize_fast(x);
    pz_vl par = pz_fev_parity_fast(y, &unsure);
    if (UNLIKELY(vl_any(unsure))) {
        pz_fev yn = *y;
        pz_fev_normalize(x);
        pz_fev_normalize(&yn);
        par = yn.l[0] & pz_vl_set(1);
    }
    vbatch_t *b = &c->b[c->n / PZ_LANES];
    const int n = c->n % PZ_LANES;
#pragma GCC unroll 8
    for (int j = 0; j < 8; j++) {            /* word j counted from the least significant end: bits [32j, 32j+32) */
        const int bit = 32 * j, a = bit / PZ_FEV_BITS, sh = bit % PZ_FEV_BITS;
        pz_vl w = x->l[a] >> sh;
#pragma GCC unroll 2
        for (int k = 1; k <= 2; k++)          /* the following limbs that still overlap the word */
            if (k * PZ_FEV_BITS - sh < 32 && a + k < PZ_FEV_NLIMB) w |= x->l[a + k] << (k * PZ_FEV_BITS - sh);
        w &= pz_vl_set(0xFFFFFFFFULL);
        vw w32 = __builtin_convertvector(w, vw);
        memcpy(&b->X[7 - j][n], &w32, sizeof w32);
    }
    vw p32 = __builtin_convertvector(par + pz_vl_set(2), vw);
    memcpy(&b->pfx[n], &p32, sizeof p32);
    memcpy(&b->koff[n], &koff, sizeof koff);
    c->n += NL;
    if (c->n == VBATCHES * PZ_LANES) vctx_flush(c);
}

/* the same operations with one operand a constant given as limbs, broadcast to every lane */
static inline PZ_VABI void fev_bcast(pz_fev *r, const u64 *a)
{
#pragma GCC unroll 9
    for (int i = 0; i < PZ_FEV_NLIMB; i++) r->l[i] = pz_vl_set(a[i]);
}
static inline PZ_VABI void fev_bc_sub(pz_fev *r, const u64 *a, const pz_fev *b) { pz_fev t; fev_bcast(&t, a); pz_fev_sub(r, &t, b); }
/* a - b for a difference that only meets a tight factor in a product, or goes to the hash: with 29-bit limbs without the carry
 * pass (pz_fev_sub_lazy of two tight elements, a valid factor next to a tight one); IFMA reads only 52 bits of each limb and needs
 * the carries. */
static inline PZ_VABI void fev_sub_nc(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#ifdef PZ_FEV_IFMA
    pz_fev_sub(r, a, b);
#else
    pz_fev_sub_lazy(r, a, b);
#endif
}
static inline PZ_VABI void fev_bc_sub_nc(pz_fev *r, const u64 *a, const pz_fev *b) { pz_fev t; fev_bcast(&t, a); fev_sub_nc(r, &t, b); }
static inline PZ_VABI void fev_bc_add(pz_fev *r, const pz_fev *a, const u64 *b) { pz_fev t; fev_bcast(&t, b); pz_fev_add(r, a, &t); }
static inline PZ_VABI void fev_sub2_bc(pz_fev *r, const pz_fev *a, const pz_fev *b, const u64 *c) { pz_fev t; fev_bcast(&t, c); pz_fev_sub2(r, a, b, &t); }

/* x3 = l^2 - xc - Sx and y3 = l*t - yc of the step. With IFMA the subtrahends go into the products as the constants K2 - xc - Sx
 * and K1 - yc (pz_fev_sqr_add, pz_fev_mul_add): the term is added to the low columns before the reduction, whose carry pass
 * makes the result tight, and the separate subtraction with its own carry pass is saved, a stage of the chain of each point
 * (measured: the carry passes between the products were a third of the time of the curve part). K2 - xc and K1 - yc are formed
 * once per round (vcons_set). With 29-bit limbs the lazy result of a product cannot be subtracted from without a carry pass, so
 * the plain forms stay and the "constants" are xc and yc themselves (the code of those engines is as before). Inlined like the
 * products on AVX-512: thin wrappers, and a pointer to the constants of the round then never waits in a spill slot. */
#ifdef PZ_FEV_IFMA
static inline PZ_VABI void vcons_set(pz_fev *cx, pz_fev *ny, const pz_fev *xc, const pz_fev *yc)
{
#pragma GCC unroll 5
    for (int i = 0; i < PZ_FEV_NLIMB; i++) {
        cx->l[i] = pz_vl_set(pz_fev_k2[i]) - xc->l[i];
        ny->l[i] = pz_vl_set(pz_fev_k1[i]) - yc->l[i];
    }
}
static inline VINLINE PZ_VABI void fev_sqr_sub2_bc(pz_fev *r, const pz_fev *a, const pz_fev *cx, const u64 *Sx)
{
    pz_fev t;
#pragma GCC unroll 5
    for (int i = 0; i < PZ_FEV_NLIMB; i++) t.l[i] = cx->l[i] - pz_vl_set(Sx[i]);
    pz_fev_sqr_add(r, a, &t);
}
static inline VINLINE PZ_VABI void fev_mul_sub(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *ny) { pz_fev_mul_add(r, a, b, ny); }
#else
static inline VINLINE PZ_VABI void fev_sqr_sub2_bc(pz_fev *r, const pz_fev *a, const pz_fev *xc, const u64 *Sx) { pz_fev_sqr(r, a); fev_sub2_bc(r, r, xc, Sx); }
static inline VINLINE PZ_VABI void fev_mul_sub(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *yc) { pz_fev_mul(r, a, b); fev_sub_nc(r, r, yc); }
#endif

static inline PZ_VABI bool vec_any_zero(const pz_fev *a)
{
    pz_vl z = a->l[0];
    for (int i = 1; i < PZ_FEV_NLIMB; i++) z |= a->l[i];
    for (int l = 0; l < NL; l++)
        if (z[l] == 0) return true;
    return false;
}

/* Montgomery batch inversion of a[0..n) (each lane independently, n >= 4); false if some lane has a zero element. a[0] and
 * a[1] are tight, the others may be fev_sub_nc results: each is only ever multiplied by a tight value.
 * With IFMA a product is latency-bound in a chain (about 85 cycles against 60 when independent), so there the forward pass
 * runs as two interleaved chains, over the even and over the odd indices; the 29-bit layouts are throughput-bound and measured
 * no faster that way. */
#ifdef PZ_FEV_IFMA
#define VCHAINS 2
#else
#define VCHAINS 1
#endif
static inline PZ_VABI bool vinv_batch(pz_fev *a, pz_fev *tmp, int n)
{
    for (int i = 0; i < VCHAINS; i++) tmp[i] = a[i];
    for (int i = VCHAINS; i < n; i++) pz_fev_mul(&tmp[i], &tmp[i - VCHAINS], &a[i]);
    pz_fev all, inv, ic[VCHAINS];
#if VCHAINS == 2
    const int le = (n - 1) & ~1, lo = (n - 2) | 1;           /* last even and last odd index */
    pz_fev_mul(&all, &tmp[le], &tmp[lo]);
#else
    all = tmp[n - 1];
#endif
    pz_fev chk = all;
    pz_fev_normalize(&chk);
    if (UNLIKELY(vec_any_zero(&chk))) return false;
    pz_fev_inv(&inv, &all);
#if VCHAINS == 2
    pz_fev_mul(&ic[0], &inv, &tmp[lo]);         /* inverse of the even product */
    pz_fev_mul(&ic[1], &inv, &tmp[le]);         /* inverse of the odd product */
#else
    ic[0] = inv;
#endif
    for (int i = n - 1; i >= VCHAINS; i--) {
        pz_fev *v = &ic[i % VCHAINS], t;
        pz_fev_mul(&t, v, &tmp[i - VCHAINS]);
        pz_fev_mul(v, v, &a[i]);
        a[i] = t;
    }
    for (int i = 0; i < VCHAINS; i++) a[i] = ic[i];
    return true;
}

typedef struct {
    u64 sx[PZ_FEV_NLIMB], sy[PZ_FEV_NLIMB];      /* the point N*GRP*G (centre advance per round), as limbs */
} vstep_t;

static void to_limbs(u64 out[PZ_FEV_NLIMB], const pz_fe *x)
{
    pz_fev t;
    pz_fev_set_lane(&t, 0, x);
    for (int i = 0; i < PZ_FEV_NLIMB; i++) out[i] = t.l[i][0];
}

/* i*G for i = 1..VHALF as limbs of this field layout, built once */
static u64 vtab_x[VHALF][PZ_FEV_NLIMB], vtab_y[VHALF][PZ_FEV_NLIMB];
static pthread_once_t vtab_once = PTHREAD_ONCE_INIT;
static void vtab_build(void)
{
    pz_ge *g = pz_xmalloc(sizeof(pz_ge) * VHALF);
    cpu_gmultiples(g, VHALF);
    for (int i = 0; i < VHALF; i++) {
        to_limbs(vtab_x[i], &g[i].x);
        to_limbs(vtab_y[i], &g[i].y);
    }
    free(g);
}

/* One round: the NL windows whose centres are in (xc, yc). Advances the centres by N*GRP*G. false = zero denominator. */
static inline VINLINE PZ_VABI bool vround(vctx_t *c, pz_fev *xc, pz_fev *yc, const vstep_t *st, u64 wbase, pz_fev *dx, pz_fev *tmp)
{
    const pz_vl kb = pz_vl_set(wbase);
    pz_vl lane_off = pz_vl_set(0);
    for (int l = 0; l < NL; l++) lane_off[l] = (u64)l * GRPV;
    const pz_vl kw = kb + lane_off;                 /* window start offsets of the lanes */

    /* The differences below that only ever meet a tight factor in a product, or go to the hash, skip the carry pass
     * (fev_sub_nc); the denominators too, except the first two, which start the chains of products of vinv_batch. */
    fev_bc_sub(&dx[0], vtab_x[0], xc);
    fev_bc_sub(&dx[1], vtab_x[1], xc);
#pragma GCC unroll 1
    for (int i = 2; i < VHALF; i++) fev_bc_sub_nc(&dx[i], vtab_x[i], xc);
    fev_bc_sub_nc(&dx[VHALF], st->sx, xc);
    if (UNLIKELY(!vinv_batch(dx, tmp, VHALF + 1))) return false;
#ifdef PZ_FEV_IFMA
    pz_fev cxv, nyv;               /* K2 - xc and K1 - yc, the terms the products of the step add */
    vcons_set(&cxv, &nyv, xc, yc);
    const pz_fev *cx = &cxv, *ny = &nyv;
#else
    const pz_fev *cx = xc, *ny = yc;
#endif

    {   /* the centre itself */
        pz_fev x = *xc, y = *yc;
        vctx_emit(c, &x, &y, kw + pz_vl_set(VHALF));
    }
    for (int i = 1; i < VHALF; i++) {
        const u64 *Sx = vtab_x[i - 1], *Sy = vtab_y[i - 1];
        const pz_fev *inv = &dx[i - 1];
        /* C + i*G and C - i*G (slope -(S.y + yc)/(S.x - xc): only its square matters for x) are computed stage by stage,
         * side by side, so that the carry chains of one overlap the products of the other */
        pz_fev tp, tm, lp, lm, xp, xm, yp, ym;
        fev_bc_sub_nc(&tp, Sy, yc);
        fev_bc_add(&tm, yc, Sy);
        pz_fev_mul(&lp, &tp, inv);
        pz_fev_mul(&lm, &tm, inv);
        fev_sqr_sub2_bc(&xp, &lp, cx, Sx);
        fev_sqr_sub2_bc(&xm, &lm, cx, Sx);
        fev_sub_nc(&tp, xc, &xp);
        fev_sub_nc(&tm, &xm, xc);
        fev_mul_sub(&yp, &lp, &tp, ny);
        fev_mul_sub(&ym, &lm, &tm, ny);
        vctx_emit(c, &xp, &yp, kw + pz_vl_set(VHALF + i));
        vctx_emit(c, &xm, &ym, kw + pz_vl_set(VHALF - i));
    }
    {   /* i = VHALF: only C - VHALF*G is needed */
        const u64 *Sx = vtab_x[VHALF - 1], *Sy = vtab_y[VHALF - 1];
        pz_fev lam, t, x3, y3;
        fev_bc_add(&t, yc, Sy);
        pz_fev_mul(&lam, &t, &dx[VHALF - 1]);
        fev_sqr_sub2_bc(&x3, &lam, cx, Sx);
        fev_sub_nc(&t, &x3, xc);
        fev_mul_sub(&y3, &lam, &t, ny);
        vctx_emit(c, &x3, &y3, kw);
    }
    /* next centres: C + (N*GRP)*G; the new yc must be tight, as the subtractions of the next round take it */
    pz_fev lam, t, nx, nyc;
    fev_bc_sub_nc(&t, st->sy, yc);
    pz_fev_mul(&lam, &t, &dx[VHALF]);
    fev_sqr_sub2_bc(&nx, &lam, cx, st->sx);
    fev_sub_nc(&t, xc, &nx);
#ifdef PZ_FEV_IFMA
    fev_mul_sub(&nyc, &lam, &t, ny);
#else
    pz_fev_mul(&nyc, &lam, &t);
    pz_fev_sub(&nyc, &nyc, yc);
#endif
    *xc = nx;
    *yc = nyc;
    return true;
}

/* Exact but slow: the NL windows of a round, one scalar multiplication per key (a key that is 0 mod n has no point and is
 * skipped). The batch is flushed at the end so that the vector path always starts on a multiple of NL entries. */
static PZ_VABI void vround_slow(vctx_t *c, const pz_u256 *start, u64 wbase)
{
    for (int l = 0; l < NL; l++) {
        u64 w0 = wbase + (u64)l * GRPV;
        for (int p = 0; p < GRPV; p++) {
            pz_u256 k = *start;
            pz_u256_add_u64(&k, &k, w0 + (u64)p);
            pz_ge pt;
            if (pz_ec_mul_g(&pt, &k)) vctx_add_scalar(c, &pt.x, &pt.y, w0 + (u64)p);
        }
    }
    if (c->n) vctx_flush(c);
}

/* Loads the window centres of the round starting at wbase; false when one is the point at infinity (key 0 mod n). */
static PZ_VABI bool load_centres(pz_fev *xc, pz_fev *yc, const pz_u256 *start, u64 wbase)
{
    for (int l = 0; l < NL; l++) {
        pz_u256 k = *start;
        pz_u256_add_u64(&k, &k, wbase + (u64)l * GRPV + VHALF);
        pz_ge pt;
        if (!pz_ec_mul_g(&pt, &k)) return false;
        pz_fev_set_lane(xc, l, &pt.x);
        pz_fev_set_lane(yc, l, &pt.y);
    }
    return true;
}

static u64 CAT(engv_scan_, ISA)(const scan_job *job, const pz_u256 *key_start, u64 nkeys)
{
    cpu_tables_init();
    pthread_once(&vtab_once, vtab_build);
    vctx_t *c = pz_xaligned(64, sizeof *c);
    pz_fev *dx = pz_xaligned(64, sizeof(pz_fev) * (VHALF + 1));
    pz_fev *tmp = pz_xaligned(64, sizeof(pz_fev) * (VHALF + 1));
    c->job = job;
    c->base = *key_start;
    vstep_t st;
    {
        pz_ge sp;
        if (!pz_ec_mul_g(&sp, &(pz_u256){{(u64)NL * GRPV, 0, 0, 0}})) pz_die("scan: invalid step");
        to_limbs(st.sx, &sp.x);
        to_limbs(st.sy, &sp.y);
    }
    const u64 per_round = (u64)NL * GRPV;
    const u64 rounds = (nkeys + per_round - 1) / per_round;
    pz_fev xc, yc;
    bool cvalid = false;            /* xc / yc hold the centres of round g */
    u64 g = 0, unreported = 0;
    for (; g < rounds; g++) {
        if ((g & 7) == 0) {
            if (job->progress && unreported) { atomic_fetch_add_explicit(job->progress, unreported, memory_order_relaxed); unreported = 0; }
            if (job->stop && atomic_load_explicit(job->stop, memory_order_relaxed)) break;
        }
        const u64 wbase = g * per_round;
        if (UNLIKELY(!cvalid)) cvalid = load_centres(&xc, &yc, key_start, wbase);
        if (UNLIKELY(!cvalid || !vround(c, &xc, &yc, &st, wbase, dx, tmp))) {
            vround_slow(c, key_start, wbase);
            cvalid = false;         /* the next round starts from freshly computed centres */
        }
        unreported += per_round;
    }
    if (c->n) vctx_flush(c);
    if (job->progress && unreported) atomic_fetch_add_explicit(job->progress, unreported, memory_order_relaxed);
    pz_afree(c); pz_afree(dx); pz_afree(tmp);
    u64 covered = g * per_round;
    return covered > nkeys ? nkeys : covered;
}

static void CAT(engv_hash_, ISA)(const u32 *X, const u32 *pfx, u32 *out) { PZ_HASH_LANES_FN(X, pfx, out); }
