/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_vec_impl.inc - NL consecutive BSGS windows at once with lane-parallel arithmetic (pz_fev.h or, with PZ_FEV_IFMA,
 * fev52.h), instantiated per ISA. The includer defines ISA (token) and PZ_VBYTES.
 *
 * Lane l takes the window centred at C[l]; the result is what bs_group (bsgs_isa.inc) gives for each window: the
 * normalized x of C + off*U for off = -BS_HALF .. BS_HALF-1, words 0 and 1, interleaved as xlo[(off + BS_HALF)*NL + l],
 * and the centres move on by NL*BS_GRP*U (the caller passes NL consecutive windows, so they become the next NL). Only
 * x is computed for the window points. Checked against the scalar routine before use (bsgs.c). */

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#ifdef PZ_FEV_IFMA
#include <pzcore/fev52.h>
#else
#include <pzcore/fev.h>
#endif

#define NL PZ_FEV_LANES

typedef struct {
    u64 tx[BS_HALF][PZ_FEV_NLIMB], ty[BS_HALF][PZ_FEV_NLIMB];   /* (i+1)*U */
    u64 sx[PZ_FEV_NLIMB], sy[PZ_FEV_NLIMB];                     /* NL*BS_GRP*U */
} CAT(bsv_tab_, ISA);

static void CAT(bsv_limbs_, ISA)(u64 out[PZ_FEV_NLIMB], const pz_fe *x)
{
    pz_fev t;
    pz_fev_set_lane(&t, 0, x);
    for (int i = 0; i < PZ_FEV_NLIMB; i++) out[i] = t.l[i][0];
}

static inline PZ_VABI void bsv_bcast(pz_fev *r, const u64 *a)
{
#pragma GCC unroll 9
    for (int i = 0; i < PZ_FEV_NLIMB; i++) r->l[i] = pz_vl_set(a[i]);
}

/* bits [64k, 64k + 64) of a canonical element */
static inline PZ_VABI pz_vl bsv_word64(const pz_fev *x, int k)
{
    const int bit = 64 * k, a = bit / PZ_FEV_BITS, sh = bit % PZ_FEV_BITS;
    pz_vl w = x->l[a] >> sh;
#pragma GCC unroll 3
    for (int m = 1; m <= 3; m++)
        if (m * PZ_FEV_BITS - sh < 64 && a + m < PZ_FEV_NLIMB) w |= x->l[a + m] << (m * PZ_FEV_BITS - sh);
    return w;
}

/* Canonical form of a tight element. With IFMA a tight element has exact limbs, and the fast reduction of the header only checks the top
 * limb (the full one runs for a vector with a lane at or above 2^256 - 2^208: about 2^-48 per lane); with 29-bit limbs the fast form
 * needs a carry ripple too and measured no cheaper than the full one. */
static inline PZ_VABI void bsv_canon(pz_fev *x)
{
#ifdef PZ_FEV_IFMA
    const pz_vl u = pz_fev_normalize_fast(x);
    u64 any = 0;
    for (int l = 0; l < NL; l++) any |= u[l];
    if (UNLIKELY(any)) pz_fev_normalize(x);
#else
    pz_fev_normalize(x);
#endif
}

static void *CAT(bsv_prepare_, ISA)(const bs_steps *st)
{
    CAT(bsv_tab_, ISA) *t = pz_xaligned(64, sizeof *t);
    for (int i = 0; i < BS_HALF; i++) {
        CAT(bsv_limbs_, ISA)(t->tx[i], &st->tab[i].x);
        CAT(bsv_limbs_, ISA)(t->ty[i], &st->tab[i].y);
    }
    pz_gej acc;
    pz_gej_set_ge(&acc, &st->step);
    for (int k = 1; k < NL; k++) pz_gej_add_ge(&acc, &acc, &st->step);
    pz_ge s;
    if (!pz_gej_to_ge(&s, &acc)) { pz_afree(t); return NULL; }
    CAT(bsv_limbs_, ISA)(t->sx, &s.x);
    CAT(bsv_limbs_, ISA)(t->sy, &s.y);
    return t;
}

static bool CAT(bsv_groups_, ISA)(const void *vt, pz_ge *C, u64 *xlo, u64 *xhi, void *scratch)
{
    const CAT(bsv_tab_, ISA) *T = vt;
    pz_fev *dx = scratch, *tmp = dx + BS_HALF + 1;
    pz_fev xc, yc, b;
    for (int l = 0; l < NL; l++) {
        pz_fev_set_lane(&xc, l, &C[l].x);
        pz_fev_set_lane(&yc, l, &C[l].y);
    }
    for (int i = 0; i < BS_HALF; i++) {
        bsv_bcast(&b, T->tx[i]);
        pz_fev_sub(&dx[i], &b, &xc);
    }
    bsv_bcast(&b, T->sx);
    pz_fev_sub(&dx[BS_HALF], &b, &xc);
    /* batched inversion, every lane independently */
    tmp[0] = dx[0];
    for (int i = 1; i <= BS_HALF; i++) pz_fev_mul(&tmp[i], &tmp[i - 1], &dx[i]);
    {
        pz_fev chk = tmp[BS_HALF];
        pz_fev_normalize(&chk);
        pz_vl z = chk.l[0];
        for (int i = 1; i < PZ_FEV_NLIMB; i++) z |= chk.l[i];
        for (int l = 0; l < NL; l++)
            if (z[l] == 0) return false;
    }
    pz_fev inv;
    pz_fev_inv(&inv, &tmp[BS_HALF]);
    for (int i = BS_HALF; i > 0; i--) {
        pz_fev t;
        pz_fev_mul(&t, &inv, &tmp[i - 1]);
        pz_fev_mul(&inv, &inv, &dx[i]);
        dx[i] = t;
    }
    dx[0] = inv;

    {   /* the centres */
        pz_fev x0 = xc;
        pz_fev_normalize(&x0);
        const pz_vl w0 = bsv_word64(&x0, 0), w1 = bsv_word64(&x0, 1);
        memcpy(xlo + (size_t)BS_HALF * NL, &w0, sizeof w0);
        memcpy(xhi + (size_t)BS_HALF * NL, &w1, sizeof w1);
    }
    for (int i = 1; i <= BS_HALF; i++) {
        pz_fev sx, sy, t, lam, x3;
        bsv_bcast(&sx, T->tx[i - 1]);
        bsv_bcast(&sy, T->ty[i - 1]);
        if (i < BS_HALF) {                       /* C + i*U */
            pz_fev_sub(&t, &sy, &yc);
            pz_fev_mul(&lam, &t, &dx[i - 1]);
            pz_fev_sqr(&x3, &lam);
            pz_fev_sub2(&x3, &x3, &xc, &sx);
            bsv_canon(&x3);
            const pz_vl w0 = bsv_word64(&x3, 0), w1 = bsv_word64(&x3, 1);
            memcpy(xlo + (size_t)(BS_HALF + i) * NL, &w0, sizeof w0);
            memcpy(xhi + (size_t)(BS_HALF + i) * NL, &w1, sizeof w1);
        }
        pz_fev_add(&t, &sy, &yc);                   /* C - i*U: only the square of the slope matters */
        pz_fev_mul(&lam, &t, &dx[i - 1]);
        pz_fev_sqr(&x3, &lam);
        pz_fev_sub2(&x3, &x3, &xc, &sx);
        bsv_canon(&x3);
        const pz_vl w0 = bsv_word64(&x3, 0), w1 = bsv_word64(&x3, 1);
        memcpy(xlo + (size_t)(BS_HALF - i) * NL, &w0, sizeof w0);
        memcpy(xhi + (size_t)(BS_HALF - i) * NL, &w1, sizeof w1);
    }
    /* next centres C + NL*BS_GRP*U */
    pz_fev sx, sy, t, lam, nx, ny;
    bsv_bcast(&sx, T->sx);
    bsv_bcast(&sy, T->sy);
    pz_fev_sub(&t, &sy, &yc);
    pz_fev_mul(&lam, &t, &dx[BS_HALF]);
    pz_fev_sqr(&nx, &lam);
    pz_fev_sub2(&nx, &nx, &xc, &sx);
    pz_fev_sub(&t, &xc, &nx);
    pz_fev_mul(&ny, &lam, &t);
    pz_fev_sub(&ny, &ny, &yc);
    pz_fev_normalize(&nx);
    pz_fev_normalize(&ny);
    for (int l = 0; l < NL; l++) {
        pz_fev_get_lane(&C[l].x, &nx, l);
        pz_fev_get_lane(&C[l].y, &ny, l);
    }
    return true;
}
