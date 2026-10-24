/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_isa.inc - the group routine of the baby/giant steps (instantiated per ISA).
 * The includer defines BS_GROUP (function name). It mirrors the linear-search engine: one batch inversion serves
 * the +i and -i points of a window, but here only x is needed, so the y-coordinates of the new points are never computed. */

bool BS_GROUP(const bs_steps *st, ge *C, u64 *xlo, u64 *xhi, fe *dx, fe *tmp)
{
    const fe xc = C->x, yc = C->y;
    for (int i = 0; i < BS_HALF; i++) fe_sub(&dx[i], &st->tab[i].x, &xc);
    fe_sub(&dx[BS_HALF], &st->step.x, &xc);
    if (UNLIKELY(!fe_inv_batch(dx, tmp, BS_HALF + 1))) return false;

    fe x0 = xc;
    fe_normalize(&x0);
    xlo[BS_HALF] = x0.n[0];
    xhi[BS_HALF] = x0.n[1];
    for (int i = 1; i <= BS_HALF; i++) {
        const ge *S = &st->tab[i - 1];
        const fe *inv = &dx[i - 1];
        fe lam, t, x3;
        if (i < BS_HALF) {                       /* C + i*U */
            fe_sub(&t, &S->y, &yc);
            fe_mul(&lam, &t, inv);
            fe_sqr(&x3, &lam);
            fe_sub(&x3, &x3, &xc);
            fe_sub(&x3, &x3, &S->x);
            fe_normalize(&x3);
            xlo[BS_HALF + i] = x3.n[0];
            xhi[BS_HALF + i] = x3.n[1];
        }
        fe_add(&t, &S->y, &yc);                  /* C - i*U: the slope is -(S.y + yc)/(S.x - xc), only its square matters */
        fe_mul(&lam, &t, inv);
        fe_sqr(&x3, &lam);
        fe_sub(&x3, &x3, &xc);
        fe_sub(&x3, &x3, &S->x);
        fe_normalize(&x3);
        xlo[BS_HALF - i] = x3.n[0];
        xhi[BS_HALF - i] = x3.n[1];
    }
    /* next centre C + BS_GRP*U (full point: y is needed) */
    fe lam, t, nx, ny;
    fe_sub(&t, &st->step.y, &yc);
    fe_mul(&lam, &t, &dx[BS_HALF]);
    fe_sqr(&nx, &lam);
    fe_sub(&nx, &nx, &xc);
    fe_sub(&nx, &nx, &st->step.x);
    fe_sub(&t, &xc, &nx);
    fe_mul(&ny, &lam, &t);
    fe_sub(&ny, &ny, &yc);
    fe_normalize(&nx);
    fe_normalize(&ny);
    C->x = nx;
    C->y = ny;
    return true;
}
