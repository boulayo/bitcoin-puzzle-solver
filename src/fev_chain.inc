/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fev_chain.inc - the parts of the lane field arithmetic common to fev.h and fev52.h, written against their
 * fev_set_lane / fev_mul / fev_sqr. Included at the end of either header. */

static inline PZ_VABI void fev_broadcast(fev *r, const fe *x)
{
    for (int lane = 0; lane < FEV_LANES; lane++) fev_set_lane(r, lane, x);
}

/* r = a^(2^n) */
static inline PZ_VABI void fev_sqr_n(fev *r, const fev *a, int n)
{
    fev_sqr(r, a);
    for (int i = 1; i < n; i++) fev_sqr(r, r);
}

/* r = a^-1 in every lane (a != 0 mod p): the libsecp256k1 addition chain, 255 squarings and 15 products. */
static inline PZ_VABI void fev_inv(fev *r, const fev *a)
{
    fev x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    fev_sqr(&x2, a);            fev_mul(&x2, &x2, a);
    fev_sqr(&x3, &x2);          fev_mul(&x3, &x3, a);
    fev_sqr_n(&x6, &x3, 3);     fev_mul(&x6, &x6, &x3);
    fev_sqr_n(&x9, &x6, 3);     fev_mul(&x9, &x9, &x3);
    fev_sqr_n(&x11, &x9, 2);    fev_mul(&x11, &x11, &x2);
    fev_sqr_n(&x22, &x11, 11);  fev_mul(&x22, &x22, &x11);
    fev_sqr_n(&x44, &x22, 22);  fev_mul(&x44, &x44, &x22);
    fev_sqr_n(&x88, &x44, 44);  fev_mul(&x88, &x88, &x44);
    fev_sqr_n(&x176, &x88, 88); fev_mul(&x176, &x176, &x88);
    fev_sqr_n(&x220, &x176, 44); fev_mul(&x220, &x220, &x44);
    fev_sqr_n(&x223, &x220, 3); fev_mul(&x223, &x223, &x3);
    fev_sqr_n(&t, &x223, 23);   fev_mul(&t, &t, &x22);
    fev_sqr_n(&t, &t, 5);       fev_mul(&t, &t, a);
    fev_sqr_n(&t, &t, 3);       fev_mul(&t, &t, &x2);
    fev_sqr_n(&t, &t, 2);       fev_mul(r, &t, a);
}
