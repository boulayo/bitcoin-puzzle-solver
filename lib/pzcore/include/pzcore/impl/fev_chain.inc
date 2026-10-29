/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_fev_chain.inc - the parts of the lane field arithmetic common to pz_fev.h and fev52.h, written against their
 * pz_fev_set_lane / pz_fev_mul / pz_fev_sqr. Included at the end of either header. */

static inline PZ_VABI void pz_fev_broadcast(pz_fev *r, const pz_fe *x)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) pz_fev_set_lane(r, lane, x);
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
