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
 * hits: [0] = number of hits, [1] = error flag, then pairs (koff_lo, koff_hi); a hit matches the whole target hash160
 * (word 2 first, the other words only for the keys that pass it) and is re-verified by the host  */

#ifndef HALF
#define HALF 128
#endif
#define GRP (2 * HALF)

inline void load_ge_c(ge *p, __constant const uint *q)
{
    for (int i = 0; i < 8; i++) { p->x.v[i] = q[i]; p->y.v[i] = q[8 + i]; }
}

/* the key at offset ko with public point (xn, y), xn normalized: compared with the target on word 2 of its hash160, then in full */
inline void check_key(const fe *xn, const fe *y, ulong ko, const uint tw[5], __global uint *hits, const uint max_hits)
{
    const uint par = fe_parity(y);
    const fe xv = *xn;
    if (hash160_w2(&xv, par) != tw[2]) return;
    uint h[5];
    hash160_words(&xv, par, h);
    if (h[0] == tw[0] && h[1] == tw[1] && h[3] == tw[3] && h[4] == tw[4]) {
        uint k = atomic_inc(&hits[0]);
        if (k < max_hits) { hits[2 + 2 * k] = (uint)ko; hits[3 + 2 * k] = (uint)(ko >> 32); }
    }
}

/* the two keys of a pair, C + i*G (when `both`) and C - i*G, from the centre (xc, yc), the table point (sx, sy) and the inverse im of
 * sx - xc; koff is the key offset of the centre */
inline void pair_keys(const fe *xc, const fe *yc, const fe *sx, const fe *sy, const fe *im, const bool both, const ulong koff, const ulong i,
                      const uint tw[5], __global uint *hits, const uint max_hits)
{
    fe lam, t, x3, y3, xs;                              /* x3 = lam^2 - (xc + sx) for both points */
    fe_add_norm(&xs, xc, sx);
    if (both) {
        fe_sub_norm(&t, sy, yc);
        fe_mul(&lam, &t, im);
        fe_sqr(&x3, &lam);
        fe_sub(&x3, &x3, &xs);
        fe_normalize_fma(&x3);
        fe_sub_norm(&t, xc, &x3);
        fe_mul(&y3, &lam, &t);
        fe_sub_norm(&y3, &y3, yc);
        check_key(&x3, &y3, koff + i, tw, hits, max_hits);
    }
    /* C - i*G */
    fe_add_norm(&t, sy, yc);
    fe_mul(&lam, &t, im);
    fe_sqr(&x3, &lam);
    fe_sub(&x3, &x3, &xs);
    fe_normalize_fma(&x3);
    fe_sub_norm(&t, &x3, xc);
    fe_mul(&y3, &lam, &t);
    fe_sub_norm(&y3, &y3, yc);
    check_key(&x3, &y3, koff - i, tw, hits, max_hits);
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
            if (m < HALF) fe_load_c16(&sx, gtab + m * 16);
            else sx = GS.x;
            fe_sub_norm(&d, &sx, &xc);                  /* the centre and the table are normalized */
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
            fe_sub_norm(&d, &GS.x, &xc);
            fe_mul(&inv, &inv, &d);
            fe_sub_norm(&t, &GS.y, &yc);
            fe_mul(&lam, &t, &im);
            fe_sqr(&ncx, &lam);
            fe_sub(&ncx, &ncx, &xc);
            fe_sub(&ncx, &ncx, &GS.x);
            fe_sub(&t, &xc, &ncx);
            fe_mul(&ncy, &lam, &t);
            fe_sub_norm(&ncy, &ncy, &yc);
            fe_normalize(&ncx);
            fe_normalize(&ncy);
        }
        /* i = m + 1 from HALF down to 2: C + i*G (i < HALF) and C - i*G. The first denominator (i = 1, whose inverse is what is left of
         * the product) and the centre (i = 0) come after the loop: inside it the compiler copied inv into im and the centre into x3 and y3
         * ahead of the tests on i, 24 moves per pair of keys on sm_86 */
        for (int i = HALF; i >= 2; i--) {
            fe sx, sy, im, d;
            fe_load_c16(&sx, gtab + (i - 1) * 16);
            fe_load_c16(&sy, gtab + (i - 1) * 16 + 8);
            fe_mul(&im, &inv, &tmp[i - 2]);
            fe_sub_norm(&d, &sx, &xc);
            fe_mul(&inv, &inv, &d);
            pair_keys(&xc, &yc, &sx, &sy, &im, i < HALF, koff, (ulong)i, tw, hits, max_hits);
        }
        {
            fe sx, sy;
            fe_load_c16(&sx, gtab);
            fe_load_c16(&sy, gtab + 8);
            pair_keys(&xc, &yc, &sx, &sy, &inv, true, koff, 1, tw, hits, max_hits);
        }
        check_key(&xc, &yc, koff, tw, hits, max_hits);
        C.x = ncx; C.y = ncy;
        koff += GRP;
    }
}
