/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_fev_chain.inc - the parts of the lane field arithmetic common to pz_fev.h and fev52.h, written against their
 * pz_fev_set_lane / pz_fev_mul / pz_fev_sqr. Included at the end of either header. */

static inline PZ_VABI void pz_fev_broadcast(pz_fev *r, const pz_fe *x)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) pz_fev_set_lane(r, lane, x);
}

/* r = a^(2^n) */
static inline PZ_VABI void pz_fev_sqr_n(pz_fev *r, const pz_fev *a, int n)
{
    pz_fev_sqr(r, a);
    for (int i = 1; i < n; i++) pz_fev_sqr(r, r);
}

/* r = a^-1 in every lane (a != 0 mod p): the libsecp256k1 addition chain, 255 squarings and 15 products. */
static inline PZ_VABI void pz_fev_inv(pz_fev *r, const pz_fev *a)
{
    pz_fev x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    pz_fev_sqr(&x2, a);            pz_fev_mul(&x2, &x2, a);
    pz_fev_sqr(&x3, &x2);          pz_fev_mul(&x3, &x3, a);
    pz_fev_sqr_n(&x6, &x3, 3);     pz_fev_mul(&x6, &x6, &x3);
    pz_fev_sqr_n(&x9, &x6, 3);     pz_fev_mul(&x9, &x9, &x3);
    pz_fev_sqr_n(&x11, &x9, 2);    pz_fev_mul(&x11, &x11, &x2);
    pz_fev_sqr_n(&x22, &x11, 11);  pz_fev_mul(&x22, &x22, &x11);
    pz_fev_sqr_n(&x44, &x22, 22);  pz_fev_mul(&x44, &x44, &x22);
    pz_fev_sqr_n(&x88, &x44, 44);  pz_fev_mul(&x88, &x88, &x44);
    pz_fev_sqr_n(&x176, &x88, 88); pz_fev_mul(&x176, &x176, &x88);
    pz_fev_sqr_n(&x220, &x176, 44); pz_fev_mul(&x220, &x220, &x44);
    pz_fev_sqr_n(&x223, &x220, 3); pz_fev_mul(&x223, &x223, &x3);
    pz_fev_sqr_n(&t, &x223, 23);   pz_fev_mul(&t, &t, &x22);
    pz_fev_sqr_n(&t, &t, 5);       pz_fev_mul(&t, &t, a);
    pz_fev_sqr_n(&t, &t, 3);       pz_fev_mul(&t, &t, &x2);
    pz_fev_sqr_n(&t, &t, 2);       pz_fev_mul(r, &t, a);
}
