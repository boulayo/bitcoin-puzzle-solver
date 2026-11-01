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

#ifdef PZ_INTERLEAVE
/* The interleaved form of the loop over the pairs (CUDA only) hashes the two x of a pair one turn of the loop after the field work that
 * made them, with the filter inlined, so that the two hashes and the field work of the next pair are one basic block for ptxas: the hash
 * is bound by the ALU pipe and leaves the FMA pipe idle, the field work the other way round. No module is built from it: ptxas keeps the
 * two phases apart, and the issue model gives 0 to 3 % (RESEARCH.md section 5, after 1.19.2); it is kept for the model
 * (tools/research/sass_sched.py -DPZ_INTERLEAVE=1), and a module built with -DPZ_INTERLEAVE=1 runs it on the simulated driver too
 * (tools/ptx_table.awk). The arithmetic and the keys are those of check_key; a parity instead of y. */
inline uint hash160_w2_inline(const fe *x, uint parity)
{
    uint xw[8], dig[8], h[5];
#pragma unroll
    for (int i = 0; i < 8; i++) xw[i] = x->v[7 - i];
    sha256_33(2u + parity, xw, dig);
    ripemd160_32(dig, h);
    return h[2];
}
/* the key at offset ko whose word 2 passed the filter: hashed in full and compared with the whole target (the copy of x is made here, in
 * the rare block, so that the pending x stay in registers on the common path: the full hash is a call and takes its argument in memory) */
inline void check_full(const fe *xn, uint par, ulong ko, const uint tw[5], __global uint *hits, const uint max_hits)
{
    const fe xv = *xn;
    uint h[5];
    hash160_words(&xv, par, h);
    if (h[0] == tw[0] && h[1] == tw[1] && h[3] == tw[3] && h[4] == tw[4]) {
        uint k = atomic_inc(&hits[0]);
        if (k < max_hits) { hits[2 + 2 * k] = (uint)ko; hits[3 + 2 * k] = (uint)(ko >> 32); }
    }
}
#endif

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
#ifndef PZ_INTERLEAVE
        /* i = m + 1 from HALF down to 1: C + i*G (i < HALF) and C - i*G, then i = 0: the centre itself */
        for (int i = HALF; i >= 0; i--) {
            fe sx, sy, lam, t, x3, y3, im;
            ulong ko;
            if (i == 0) {
                x3 = xc; y3 = yc; ko = koff;
                goto check;
            }
            fe_load_c16(&sx, gtab + (i - 1) * 16);
            fe_load_c16(&sy, gtab + (i - 1) * 16 + 8);
            if (i > 1) {
                fe d;
                fe_mul(&im, &inv, &tmp[i - 2]);
                fe_sub_norm(&d, &sx, &xc);
                fe_mul(&inv, &inv, &d);
            } else {
                im = inv;
            }
            fe xs;                                       /* x3 = lam^2 - (xc + sx) for both points */
            fe_add_norm(&xs, &xc, &sx);
            if (i < HALF) {
                fe_sub_norm(&t, &sy, &yc);
                fe_mul(&lam, &t, &im);
                fe_sqr(&x3, &lam);
                fe_sub(&x3, &x3, &xs);
                fe_normalize_fma(&x3);
                fe_sub_norm(&t, &xc, &x3);
                fe_mul(&y3, &lam, &t);
                fe_sub_norm(&y3, &y3, &yc);
                check_key(&x3, &y3, koff + (ulong)i, tw, hits, max_hits);
            }
            /* C - i*G */
            fe_add_norm(&t, &sy, &yc);
            fe_mul(&lam, &t, &im);
            fe_sqr(&x3, &lam);
            fe_sub(&x3, &x3, &xs);
            fe_normalize_fma(&x3);
            fe_sub_norm(&t, &x3, &xc);
            fe_mul(&y3, &lam, &t);
            fe_sub_norm(&y3, &y3, &yc);
            ko = koff - (ulong)i;
        check:;
            check_key(&x3, &y3, ko, tw, hits, max_hits);
        }
#else
        /* The interleaved form (PZ_INTERLEAVE, see check_full above): the loop is pipelined by one pair. A turn computes the two points of
         * pair i (the inverse, two lambdas, two squarings, two products) and hashes the two x of pair i + 1, kept from the turn before with
         * the parity of their y. The first pending pair is the centre itself (hashed first here, last in the plain form) and C - HALF*G,
         * the point of i = HALF, which has no partner above the window; the epilogue hashes pair 1. Same keys, same arithmetic. */
        fe xa, xb;                                       /* the pending pair: x of C + ia*G and of C - ib*G, and the parity of their y */
        uint pa, pb, ia, ib;
        xa = xc; pa = fe_parity(&yc); ia = 0;
        {
            fe sx, sy, lam, t, x3, y3, im, d, xs;
            fe_load_c16(&sx, gtab + (HALF - 1) * 16);
            fe_load_c16(&sy, gtab + (HALF - 1) * 16 + 8);
            fe_mul(&im, &inv, &tmp[HALF - 2]);
            fe_sub_norm(&d, &sx, &xc);
            fe_mul(&inv, &inv, &d);
            fe_add_norm(&xs, &xc, &sx);
            fe_add_norm(&t, &sy, &yc);
            fe_mul(&lam, &t, &im);
            fe_sqr(&x3, &lam);
            fe_sub(&x3, &x3, &xs);
            fe_normalize_fma(&x3);
            fe_sub_norm(&t, &x3, &xc);
            fe_mul(&y3, &lam, &t);
            fe_sub_norm(&y3, &y3, &yc);
            xb = x3; pb = fe_parity(&y3); ib = HALF;
        }
        for (int i = HALF - 1; i >= 1; i--) {
            fe sx, sy, lam, t, xp, yp, xm, ym, im, xs;
            fe_load_c16(&sx, gtab + (i - 1) * 16);
            fe_load_c16(&sy, gtab + (i - 1) * 16 + 8);
            if (i > 1) {
                fe d;
                fe_mul(&im, &inv, &tmp[i - 2]);
                fe_sub_norm(&d, &sx, &xc);
                fe_mul(&inv, &inv, &d);
            } else {
                im = inv;
            }
            fe_add_norm(&xs, &xc, &sx);                  /* x3 = lam^2 - (xc + sx) for both points */
            fe_sub_norm(&t, &sy, &yc);                   /* C + i*G */
            fe_mul(&lam, &t, &im);
            fe_sqr(&xp, &lam);
            fe_sub(&xp, &xp, &xs);
            fe_normalize_fma(&xp);
            fe_sub_norm(&t, &xc, &xp);
            fe_mul(&yp, &lam, &t);
            fe_sub_norm(&yp, &yp, &yc);
            fe_add_norm(&t, &sy, &yc);                   /* C - i*G */
            fe_mul(&lam, &t, &im);
            fe_sqr(&xm, &lam);
            fe_sub(&xm, &xm, &xs);
            fe_normalize_fma(&xm);
            fe_sub_norm(&t, &xm, &xc);
            fe_mul(&ym, &lam, &t);
            fe_sub_norm(&ym, &ym, &yc);
            /* the two hashes of the pending pair, both before the one branch to the rare full check */
            const uint wa = hash160_w2_inline(&xa, pa), wb = hash160_w2_inline(&xb, pb);
            if ((wa == tw[2]) | (wb == tw[2])) {
                if (wa == tw[2]) check_full(&xa, pa, koff + (ulong)ia, tw, hits, max_hits);
                if (wb == tw[2]) check_full(&xb, pb, koff - (ulong)ib, tw, hits, max_hits);
            }
            xa = xp; pa = fe_parity(&yp); ia = (uint)i;
            xb = xm; pb = fe_parity(&ym); ib = (uint)i;
        }
        {
            const uint wa = hash160_w2_inline(&xa, pa), wb = hash160_w2_inline(&xb, pb);
            if (wa == tw[2]) check_full(&xa, pa, koff + (ulong)ia, tw, hits, max_hits);
            if (wb == tw[2]) check_full(&xb, pb, koff - (ulong)ib, tw, hits, max_hits);
        }
#endif
        C.x = ncx; C.y = ncy;
        koff += GRP;
    }
}
