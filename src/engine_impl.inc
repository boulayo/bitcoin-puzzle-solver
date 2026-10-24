/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* engine_impl.inc - CPU linear-search engine, instantiated once per ISA.
 * The includer defines ISA (token) and PZ_VBYTES before including.
 *
 * Algorithm (per group of 2*HALF consecutive keys centered on C = c*G):
 *   - one batched modular inversion (Montgomery trick) of the HALF+1
 *     denominators  x(i*G) - x(C)  gives the slopes for  C + i*G  and
 *     C - i*G  at the same time, plus the next group center  C + 2*HALF*G;
 *   - every candidate's compressed public key is hashed PZ_LANES at a time
 *     with the vectorized SHA-256 + RIPEMD-160 pipeline;
 *   - only candidates whose first digest word equals the target's are
 *     verified in full with the scalar reference hash. */

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define PZ_HASH_LANES_FN CAT(hash_lanes_, ISA)
#include <pzcore/impl/hash_lanes.inc>

#define HALF CPU_HALF
#define GRP CPU_GRP

typedef struct {
    u32 X[8][PZ_LANES] PZ_ALIGN(64);
    u32 pfx[PZ_LANES] PZ_ALIGN(64);
    u32 out[5][PZ_LANES] PZ_ALIGN(64);
    u64 koff[PZ_LANES];       /* key offset from the claim start */
    int n;
    const scan_job *job;
    pz_u256 base;             /* first key of the claim */
} ctx_t;

static void ctx_verify(ctx_t *c, int lane)
{
    u8 pub[33], h[20];
    pub[0] = (u8)c->pfx[lane];
    for (int i = 0; i < 8; i++) {
        u32 w = c->X[i][lane];
        pub[1 + 4 * i] = (u8)(w >> 24); pub[2 + 4 * i] = (u8)(w >> 16);
        pub[3 + 4 * i] = (u8)(w >> 8);  pub[4 + 4 * i] = (u8)w;
    }
    pz_hash160(pub, 33, h);
    if (memcmp(h, c->job->target_h160, 20) != 0) return;
    pz_u256 key;
    pz_u256 d = pz_u256_from_u64(c->koff[lane]);
    pz_u256_add(&key, &c->base, &d);
    if (c->job->on_hit) c->job->on_hit(c->job->user, &key);
}

static inline void ctx_flush(ctx_t *c)
{
#ifdef PZ_PROF_NOHASH   /* profiling aid: measures the elliptic-curve half alone */
    c->n = 0;
    return;
#endif
    PZ_HASH_W0_FN(&c->X[0][0], c->pfx, &c->out[0][0]);
    const u32 t0 = c->job->target_w[0];
    for (int l = 0; l < c->n; l++)
        if (UNLIKELY(c->out[0][l] == t0)) ctx_verify(c, l);
    c->n = 0;
}

static inline void ctx_add(ctx_t *c, const pz_fe *x, const pz_fe *y, u64 koff)
{
    pz_fe xn = *x, yn = *y;
    pz_fe_normalize(&xn);
    pz_fe_normalize(&yn);
    const int l = c->n;
    c->X[0][l] = (u32)(xn.n[3] >> 32); c->X[1][l] = (u32)xn.n[3];
    c->X[2][l] = (u32)(xn.n[2] >> 32); c->X[3][l] = (u32)xn.n[2];
    c->X[4][l] = (u32)(xn.n[1] >> 32); c->X[5][l] = (u32)xn.n[1];
    c->X[6][l] = (u32)(xn.n[0] >> 32); c->X[7][l] = (u32)xn.n[0];
    c->pfx[l] = 2u + (u32)(yn.n[0] & 1);
    c->koff[l] = koff;
    if (++c->n == PZ_LANES) ctx_flush(c);
}

/* Processes one group. On success *C advances by GRP*G. Returns false if a
 * denominator was zero (only possible for pathological tiny keys). */
static inline bool group_step(ctx_t *c, u64 cbase, pz_ge *C, pz_fe *dx, pz_fe *tmp)
{
    const pz_fe xc = C->x, yc = C->y;
    for (int i = 0; i < HALF; i++) pz_fe_sub(&dx[i], &cpu_gtab[i].x, &xc);
    pz_fe_sub(&dx[HALF], &cpu_gstep.x, &xc);
    if (UNLIKELY(!pz_fe_inv_batch(dx, tmp, HALF + 1))) return false;

    ctx_add(c, &xc, &yc, cbase);
    for (int i = 1; i <= HALF; i++) {
        const pz_ge *S = &cpu_gtab[i - 1];
        const pz_fe *inv = &dx[i - 1];
        pz_fe lam, t, x3, y3;
        if (i < HALF) {
            pz_fe_sub(&t, &S->y, &yc);
            pz_fe_mul(&lam, &t, inv);
            pz_fe_sqr(&x3, &lam);
            pz_fe_sub(&x3, &x3, &xc);
            pz_fe_sub(&x3, &x3, &S->x);
            pz_fe_sub(&t, &xc, &x3);
            pz_fe_mul(&y3, &lam, &t);
            pz_fe_sub(&y3, &y3, &yc);
            ctx_add(c, &x3, &y3, cbase + (u64)i);
        }
        /* C - i*G : slope is -(S.y + yc)/(S.x - xc) */
        pz_fe_add(&t, &S->y, &yc);
        pz_fe_mul(&lam, &t, inv);
        pz_fe_sqr(&x3, &lam);
        pz_fe_sub(&x3, &x3, &xc);
        pz_fe_sub(&x3, &x3, &S->x);
        pz_fe_sub(&t, &x3, &xc);
        pz_fe_mul(&y3, &lam, &t);
        pz_fe_sub(&y3, &y3, &yc);
        ctx_add(c, &x3, &y3, cbase - (u64)i);
    }
    /* next center */
    pz_fe lam, t, nx, ny;
    pz_fe_sub(&t, &cpu_gstep.y, &yc);
    pz_fe_mul(&lam, &t, &dx[HALF]);
    pz_fe_sqr(&nx, &lam);
    pz_fe_sub(&nx, &nx, &xc);
    pz_fe_sub(&nx, &nx, &cpu_gstep.x);
    pz_fe_sub(&t, &xc, &nx);
    pz_fe_mul(&ny, &lam, &t);
    pz_fe_sub(&ny, &ny, &yc);
    pz_fe_normalize(&nx);
    pz_fe_normalize(&ny);
    C->x = nx;
    C->y = ny;
    return true;
}

/* Exact but slow processing of one group (scalar multiplication per key). Returns false when the next centre is the point
 * at infinity (its key is 0 mod n): the caller then keeps taking this path for the next group. */
static bool group_slow(ctx_t *c, u64 cbase, const pz_u256 *center, pz_ge *Cnext)
{
    for (int i = -HALF; i < HALF; i++) {
        pz_u256 k = *center, d = pz_u256_from_u64((u64)(i < 0 ? -i : i));
        if (i < 0) pz_u256_sub(&k, &k, &d); else pz_u256_add(&k, &k, &d);
        pz_ge p;
        if (pz_ec_mul_g(&p, &k)) ctx_add(c, &p.x, &p.y, cbase + (u64)(int64_t)i);
    }
    pz_u256 nk = *center;
    pz_u256_add_u64(&nk, &nk, GRP);
    return pz_ec_mul_g(Cnext, &nk);
}

static u64 CAT(eng_scan_, ISA)(const scan_job *job, const pz_u256 *key_start, u64 nkeys)
{
    cpu_tables_init();
    ctx_t *c = pz_xaligned(64, sizeof *c);
    pz_fe *dx = pz_xaligned(64, sizeof(pz_fe) * (HALF + 1));
    pz_fe *tmp = pz_xaligned(64, sizeof(pz_fe) * (HALF + 1));
    c->job = job;
    c->base = *key_start;
    pz_u256 center = *key_start;
    pz_u256_add_u64(&center, &center, HALF);
    pz_ge C;
    bool cvalid = pz_ec_mul_g(&C, &center);      /* false: the centre is the point at infinity (key 0 mod n) */
    const u64 ngroups = (nkeys + GRP - 1) / GRP;
    u64 g = 0, unreported = 0;
    for (; g < ngroups; g++) {
        if ((g & 15) == 0) {
            if (job->progress && unreported) { atomic_fetch_add_explicit(job->progress, unreported, memory_order_relaxed); unreported = 0; }
            if (job->stop && atomic_load_explicit(job->stop, memory_order_relaxed)) break;
        }
        const u64 cbase = g * GRP + HALF;
        if (UNLIKELY(!cvalid || !group_step(c, cbase, &C, dx, tmp))) cvalid = group_slow(c, cbase, &center, &C);
        pz_u256_add_u64(&center, &center, GRP);
        unreported += GRP;
    }
    if (c->n) ctx_flush(c);
    if (job->progress && unreported) atomic_fetch_add_explicit(job->progress, unreported, memory_order_relaxed);
    pz_afree(c); pz_afree(dx); pz_afree(tmp);
    u64 covered = g * GRP;
    return covered > nkeys ? nkeys : covered;
}

static void CAT(eng_hash_, ISA)(const u32 *X, const u32 *pfx, u32 *out) { PZ_HASH_LANES_FN(X, pfx, out); }
