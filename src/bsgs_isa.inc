/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_isa.inc - the group routine of the baby/giant steps (instantiated per ISA).
 * The includer defines BS_GROUP (function name). It mirrors the linear-search engine: one batch inversion serves
 * the +i and -i points of a window, but here only x is needed, so the y-coordinates of the new points are never computed. */

bool BS_GROUP(const bs_steps *st, pz_ge *C, u64 *xlo, u64 *xhi, pz_fe *dx, pz_fe *tmp)
{
    const pz_fe xc = C->x, yc = C->y;
    for (int i = 0; i < BS_HALF; i++) pz_fe_sub(&dx[i], &st->tab[i].x, &xc);
    pz_fe_sub(&dx[BS_HALF], &st->step.x, &xc);
    if (UNLIKELY(!pz_fe_inv_batch(dx, tmp, BS_HALF + 1))) return false;

    pz_fe x0 = xc;
    pz_fe_normalize(&x0);
    xlo[BS_HALF] = x0.n[0];
    xhi[BS_HALF] = x0.n[1];
    for (int i = 1; i <= BS_HALF; i++) {
        const pz_ge *S = &st->tab[i - 1];
        const pz_fe *inv = &dx[i - 1];
        pz_fe lam, t, x3;
        if (i < BS_HALF) {                       /* C + i*U */
            pz_fe_sub(&t, &S->y, &yc);
            pz_fe_mul(&lam, &t, inv);
            pz_fe_sqr(&x3, &lam);
            pz_fe_sub(&x3, &x3, &xc);
            pz_fe_sub(&x3, &x3, &S->x);
            pz_fe_normalize(&x3);
            xlo[BS_HALF + i] = x3.n[0];
            xhi[BS_HALF + i] = x3.n[1];
        }
        pz_fe_add(&t, &S->y, &yc);                  /* C - i*U: the slope is -(S.y + yc)/(S.x - xc), only its square matters */
        pz_fe_mul(&lam, &t, inv);
        pz_fe_sqr(&x3, &lam);
        pz_fe_sub(&x3, &x3, &xc);
        pz_fe_sub(&x3, &x3, &S->x);
        pz_fe_normalize(&x3);
        xlo[BS_HALF - i] = x3.n[0];
        xhi[BS_HALF - i] = x3.n[1];
    }
    /* next centre C + BS_GRP*U (full point: y is needed) */
    pz_fe lam, t, nx, ny;
    pz_fe_sub(&t, &st->step.y, &yc);
    pz_fe_mul(&lam, &t, &dx[BS_HALF]);
    pz_fe_sqr(&nx, &lam);
    pz_fe_sub(&nx, &nx, &xc);
    pz_fe_sub(&nx, &nx, &st->step.x);
    pz_fe_sub(&t, &xc, &nx);
    pz_fe_mul(&ny, &lam, &t);
    pz_fe_sub(&ny, &ny, &yc);
    pz_fe_normalize(&nx);
    pz_fe_normalize(&ny);
    C->x = nx;
    C->y = ny;
    return true;
}
