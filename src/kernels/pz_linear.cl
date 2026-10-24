/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_linear.cl - sequential key search kernel.
 *
 * Each work-item scans `ngroups` consecutive groups of GRP = 2*HALF keys
 * starting at its own centre point  C = P0 + gid*D  (D = ngroups*GRP*G).
 * Inside a group one batched inversion yields C +- i*G for i = 1..HALF-1,
 * C - HALF*G and the next centre, exactly like the CPU engine (in the opposite
 * order: the inverses come out of the backward pass from the last one).
 *
 * prm layout (uints): P0.x[8] P0.y[8] D.x[8] D.y[8] GS.x[8] GS.y[8] target[5] ngroups
 * hits: [0] = number of hits, [1] = error flag, then pairs (koff_lo, koff_hi); a hit matches the first 96 bits of
 * the target hash160 and is re-verified by the host  */

#ifndef HALF
#define HALF 128
#endif
#define GRP (2 * HALF)

inline void load_ge_c(ge *p, __constant const uint *q)
{
    for (int i = 0; i < 8; i++) { p->x.v[i] = q[i]; p->y.v[i] = q[8 + i]; }
}

__kernel void pz_search(__constant const uint *prm, __constant const uint *gtab, __global uint *hits, const uint max_hits, const uint pz_n)
{
    const uint gid = (uint)get_global_id(0);
    PZ_GUARD(gid);
    ge P0, D, GS;
    load_ge_c(&P0, prm);
    load_ge_c(&D, prm + 16);
    load_ge_c(&GS, prm + 32);
    uint tw[5];
    for (int i = 0; i < 5; i++) tw[i] = prm[48 + i];
    const uint ngroups = prm[53];

    ge C;
    if (gid == 0) C = P0;
    else if (!ge_add_mul_small(&C, &P0, &D, gid)) { atomic_or(&hits[1], 1u); return; }
    ulong koff = (ulong)gid * (ulong)ngroups * GRP + HALF;   /* offset of C from the claim start */

    /* only the prefix products are kept: the backward pass of the batched inversion recomputes each denominator from
     * the table and uses its inverse at once, which divides the private-memory traffic per key by three */
    fe tmp[HALF];
    for (uint g = 0; g < ngroups; g++) {
        const fe xc = C.x, yc = C.y;
        /* denominators d_m = x((m+1)*G) - xc for m < HALF, d_HALF = x(GRP*G) - xc */
        fe acc;
        for (int m = 0; m <= HALF; m++) {
            fe sx, d;
            if (m < HALF) fe_load_c(&sx, gtab + m * 16);
            else sx = GS.x;
            fe_sub(&d, &sx, &xc);
            if (m == 0) acc = d;
            else fe_mul(&acc, &acc, &d);
            if (m < HALF) tmp[m] = acc;
        }
        fe chk = acc;
        fe_normalize(&chk);
        if (fe_is_zero(&chk)) { atomic_or(&hits[1], 1u); return; }
        fe inv;
        fe_inv(&inv, &acc);
        fe ncx, ncy;
        /* m = HALF: the next centre C + GRP*G */
        {
            fe im, d, lam, t;
            fe_mul(&im, &inv, &tmp[HALF - 1]);
            fe_sub(&d, &GS.x, &xc);
            fe_mul(&inv, &inv, &d);
            fe_sub(&t, &GS.y, &yc);
            fe_mul(&lam, &t, &im);
            fe_sqr(&ncx, &lam);
            fe_sub(&ncx, &ncx, &xc);
            fe_sub(&ncx, &ncx, &GS.x);
            fe_sub(&t, &xc, &ncx);
            fe_mul(&ncy, &lam, &t);
            fe_sub(&ncy, &ncy, &yc);
            fe_normalize(&ncx);
            fe_normalize(&ncy);
        }
        /* i = m + 1 from HALF down to 1: C + i*G (i < HALF) and C - i*G, then i = 0: the centre itself */
        for (int i = HALF; i >= 0; i--) {
            fe sx, sy, lam, t, x3, y3, im;
            ulong ko;
            if (i == 0) {
                x3 = xc; y3 = yc; ko = koff;
                goto check;
            }
            fe_load_c(&sx, gtab + (i - 1) * 16);
            fe_load_c(&sy, gtab + (i - 1) * 16 + 8);
            if (i > 1) {
                fe d;
                fe_mul(&im, &inv, &tmp[i - 2]);
                fe_sub(&d, &sx, &xc);
                fe_mul(&inv, &inv, &d);
            } else {
                im = inv;
            }
            if (i < HALF) {
                fe_sub(&t, &sy, &yc);
                fe_mul(&lam, &t, &im);
                fe_sqr(&x3, &lam);
                fe_sub(&x3, &x3, &xc);
                fe_sub(&x3, &x3, &sx);
                fe_sub(&t, &xc, &x3);
                fe_mul(&y3, &lam, &t);
                fe_sub(&y3, &y3, &yc);
                {
                    fe xn = x3, yn = y3;
                    fe_normalize(&xn); fe_normalize(&yn);
                    uint h[3];
                    hash160_w3(&xn, yn.v[0] & 1u, h);
                    if (h[0] == tw[0] && h[1] == tw[1] && h[2] == tw[2]) {
                        uint k = atomic_inc(&hits[0]);
                        if (k < max_hits) { ulong kk = koff + i; hits[2 + 2 * k] = (uint)kk; hits[3 + 2 * k] = (uint)(kk >> 32); }
                    }
                }
            }
            /* C - i*G */
            fe_add(&t, &sy, &yc);
            fe_mul(&lam, &t, &im);
            fe_sqr(&x3, &lam);
            fe_sub(&x3, &x3, &xc);
            fe_sub(&x3, &x3, &sx);
            fe_sub(&t, &x3, &xc);
            fe_mul(&y3, &lam, &t);
            fe_sub(&y3, &y3, &yc);
            ko = koff - (ulong)i;
        check:;
            {
                fe xn = x3, yn = y3;
                fe_normalize(&xn); fe_normalize(&yn);
                uint h[3];
                hash160_w3(&xn, yn.v[0] & 1u, h);
                if (h[0] == tw[0] && h[1] == tw[1] && h[2] == tw[2]) {
                    uint k = atomic_inc(&hits[0]);
                    if (k < max_hits) { hits[2 + 2 * k] = (uint)ko; hits[3 + 2 * k] = (uint)(ko >> 32); }
                }
            }
        }
        C.x = ncx; C.y = ncy;
        koff += GRP;
    }
}
