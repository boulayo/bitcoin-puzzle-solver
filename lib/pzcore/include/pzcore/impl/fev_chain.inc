/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* fev_chain.inc - the parts of the lane field arithmetic common to fev.h and fev52.h, written against their
 * pz_fev_set_lane / pz_fev_mul / pz_fev_sqr. Included at the end of either header. */

static inline PZ_VABI void pz_fev_broadcast(pz_fev *r, const pz_fe *x)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) pz_fev_set_lane(r, lane, x);
}

/* The limbs of x in this layout, kept as a constant of a loop, and the element with those limbs in every lane (no conversion per use) */
static inline void pz_fev_limbs(uint64_t out[PZ_FEV_NLIMB], const pz_fe *x)
{
    pz_fev t;
    pz_fev_set_lane(&t, 0, x);
    for (int i = 0; i < PZ_FEV_NLIMB; i++) out[i] = t.l[i][0];
}
static inline PZ_VABI void pz_fev_from_limbs(pz_fev *r, const uint64_t *a)
{
#pragma GCC unroll 9
    for (int i = 0; i < PZ_FEV_NLIMB; i++) r->l[i] = pz_vl_set(a[i]);
}

/* bits [64k, 64k + 64) of a canonical element, in every lane */
static inline PZ_VABI pz_vl pz_fev_word64(const pz_fev *x, int k)
{
    const int bit = 64 * k, a = bit / PZ_FEV_BITS, sh = bit % PZ_FEV_BITS;
    pz_vl w = x->l[a] >> sh;
#pragma GCC unroll 3
    for (int m = 1; m <= 3; m++)
        if (m * PZ_FEV_BITS - sh < 64 && a + m < PZ_FEV_NLIMB) w |= x->l[a + m] << (m * PZ_FEV_BITS - sh);
    return w;
}

/* r = a^-1 in every lane (0 in a lane that is 0 mod p). The lanes are inverted together by Montgomery's trick: one scalar inversion
 * (pz_fe_inv, divsteps) and 3 (PZ_FEV_LANES - 1) scalar products, a fraction of the time of an addition chain of 255 vector squarings,
 * whose products wait for each other. Out of line: called once per batch. */
static PZ_UNUSED __attribute__((noinline)) PZ_VABI void pz_fev_inv(pz_fev *r, const pz_fev *a)
{
    pz_fev c = *a;
    pz_fe e[PZ_FEV_LANES], t[PZ_FEV_LANES];
    pz_fev_normalize(&c);                                    /* pz_fev_get_lane wants canonical lanes */
    for (int l = 0; l < PZ_FEV_LANES; l++) pz_fev_get_lane(&e[l], &c, l);
    if (PZ_UNLIKELY(!pz_fe_inv_batch(e, t, PZ_FEV_LANES)))
        for (int l = 0; l < PZ_FEV_LANES; l++) pz_fe_inv(&e[l], &e[l]);
    for (int l = 0; l < PZ_FEV_LANES; l++) pz_fev_set_lane(r, l, &e[l]);
}

/* true if some lane of a canonical element is zero */
static inline PZ_VABI bool pz_fev_any_zero(const pz_fev *a)
{
    pz_vl z = a->l[0];
    for (int i = 1; i < PZ_FEV_NLIMB; i++) z |= a->l[i];
    for (int l = 0; l < PZ_FEV_LANES; l++)
        if (z[l] == 0) return true;
    return false;
}

/* Montgomery batch inversion of a[0..n) in every lane, each lane on its own (n >= 4; tmp holds n elements): a[i] becomes its inverse.
 * False, with a[] untouched, if some lane has a zero element. a[0] and a[1] are tight, the others may be lazy differences
 * (pz_fev_sub_lazy): each is only ever multiplied by a tight value. With 52-bit limbs a product is latency-bound in a chain (about 85
 * cycles against 60 when independent), so there the forward pass runs as two interleaved chains, over the even and over the odd indices;
 * the 29-bit layouts are throughput-bound and measured no faster that way. */
#if PZ_FEV_BITS == 52
#define PZ_FEV_CHAINS 2
#else
#define PZ_FEV_CHAINS 1
#endif
static inline PZ_VABI bool pz_fev_inv_batch(pz_fev *a, pz_fev *tmp, int n)
{
    for (int i = 0; i < PZ_FEV_CHAINS; i++) tmp[i] = a[i];
    for (int i = PZ_FEV_CHAINS; i < n; i++) pz_fev_mul(&tmp[i], &tmp[i - PZ_FEV_CHAINS], &a[i]);
    pz_fev all, inv, ic[PZ_FEV_CHAINS];
#if PZ_FEV_CHAINS == 2
    const int le = (n - 1) & ~1, lo = (n - 2) | 1;           /* last even and last odd index */
    pz_fev_mul(&all, &tmp[le], &tmp[lo]);
#else
    all = tmp[n - 1];
#endif
    pz_fev chk = all;
    pz_fev_normalize(&chk);
    if (PZ_UNLIKELY(pz_fev_any_zero(&chk))) return false;
    pz_fev_inv(&inv, &all);
#if PZ_FEV_CHAINS == 2
    pz_fev_mul(&ic[0], &inv, &tmp[lo]);         /* inverse of the even product */
    pz_fev_mul(&ic[1], &inv, &tmp[le]);         /* inverse of the odd product */
#else
    ic[0] = inv;
#endif
    for (int i = n - 1; i >= PZ_FEV_CHAINS; i--) {
        pz_fev *v = &ic[i % PZ_FEV_CHAINS], t;
        pz_fev_mul(&t, v, &tmp[i - PZ_FEV_CHAINS]);
        pz_fev_mul(v, v, &a[i]);
        a[i] = t;
    }
    for (int i = 0; i < PZ_FEV_CHAINS; i++) a[i] = ic[i];
    return true;
}
