/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fe.c - the field operations that are not inline: the square root and the inversion. */
#include <pzcore/fe.h>

/* a^((p+1)/4), p = 2^256 - 2^32 - 977: (p+1)/4 is 223 ones, a zero, 22 ones, four zeros, two ones and two zeros in binary, which
 * an addition chain reaches with a few more steps (253 squarings and 13 multiplications instead of 256 and about 128 for a
 * bit-by-bit square-and-multiply). */
bool pz_fe_sqrt(pz_fe *r, const pz_fe *a)
{
    pz_fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    pz_fe_sqr(&x2, a);            pz_fe_mul(&x2, &x2, a);
    pz_fe_sqr(&x3, &x2);          pz_fe_mul(&x3, &x3, a);
    pz_fe_sqr_n(&x6, &x3, 3);     pz_fe_mul(&x6, &x6, &x3);
    pz_fe_sqr_n(&x9, &x6, 3);     pz_fe_mul(&x9, &x9, &x3);
    pz_fe_sqr_n(&x11, &x9, 2);    pz_fe_mul(&x11, &x11, &x2);
    pz_fe_sqr_n(&x22, &x11, 11);  pz_fe_mul(&x22, &x22, &x11);
    pz_fe_sqr_n(&x44, &x22, 22);  pz_fe_mul(&x44, &x44, &x22);
    pz_fe_sqr_n(&x88, &x44, 44);  pz_fe_mul(&x88, &x88, &x44);
    pz_fe_sqr_n(&x176, &x88, 88); pz_fe_mul(&x176, &x176, &x88);
    pz_fe_sqr_n(&x220, &x176, 44); pz_fe_mul(&x220, &x220, &x44);
    pz_fe_sqr_n(&x223, &x220, 3); pz_fe_mul(&x223, &x223, &x3);
    pz_fe_sqr_n(&t, &x223, 23);   pz_fe_mul(&t, &t, &x22);
    pz_fe_sqr_n(&t, &t, 6);       pz_fe_mul(&t, &t, &x2);
    pz_fe_sqr(&t, &t);
    pz_fe_sqr(&t, &t);
    pz_fe chk;
    pz_fe_sqr(&chk, &t);
    *r = t;
    pz_fe_normalize(r);
    return pz_fe_equiv(&chk, a);
}

/* ---- inversion by divsteps: Bernstein and Yang, "Fast constant-time gcd computation and modular inversion" (https://gcd.cr.yp.to),
 * in the variable-time form of libsecp256k1's modinv64 ----
 *
 * With f = p, g = x and eta = -1, a divstep is: if eta < 0 and g is odd, (eta, f, g) -> (-eta - 1, g, (g - f) / 2); else if g is odd,
 * (eta - 1, f, (g + f) / 2); else (eta - 1, f, g / 2). 62 of them depend only on the low 62 bits of f and g and amount to a matrix
 * t = (u v; q r) with (f', g') = t (f, g) / 2^62 exactly and |u|, |v|, |q|, |r| <= 2^62. The coefficients d, e with f = d x and
 * g = e x (mod p) follow the same matrix, divided by 2^62 modulo p (a multiple of p is added to make the division exact), and stay
 * in (-2p, p). After at most 12 matrices g = 0 and f = +-1 (or f = p when x = 0, and then d = 0), so x^-1 = +-d. Runs of zeros in g
 * are taken at once, and an odd g has up to 6 low bits cancelled by one multiple of f: the matrix of 62 steps costs a few dozen
 * operations instead of 62 rounds of one step. Numbers are 5 limbs of 62 bits, the top one signed, the others in [0, 2^62). */
__extension__ typedef __int128 pz_i128_;
typedef struct { int64_t v[5]; } fe_s62;
typedef struct { int64_t u, v, q, r; } fe_t2x2;
#define FE_M62 (UINT64_MAX >> 2)
static const fe_s62 FE_P62 = {{-(int64_t)0x1000003D1, 0, 0, 0, 256}};   /* p = 2^256 - (2^32 + 977), written with a negative limb */
#define FE_PINV62 0x27C7F6E22DDACACFULL                                    /* p^-1 mod 2^62 */

static int64_t fe_divsteps_62(int64_t eta, uint64_t f, uint64_t g, fe_t2x2 *t)
{
    uint64_t u = 1, v = 0, q = 0, r = 1;   /* u f0 + v g0 = f 2^(62 - i), q f0 + r g0 = g 2^(62 - i) */
    int i = 62;
    for (;;) {
        const int zeros = __builtin_ctzll(g | (UINT64_MAX << i));    /* a bit at i stops the count there */
        g >>= zeros;
        u <<= zeros;
        v <<= zeros;
        eta -= zeros;
        i -= zeros;
        if (i == 0) break;
        /* f and g are odd: replace (f, g) by (g, -f) when eta < 0, then add to g the multiple w of f that clears its low bits: at most 6
         * of them after a swap (w = -g / f mod 64, with f^-1 = f (2 - f^2) mod 64), 4 otherwise, and never more than i or eta + 1 */
        const bool swap = eta < 0;
        if (swap) {
            uint64_t tmp;
            eta = -eta;
            tmp = f; f = g; g = 0 - tmp;
            tmp = u; u = q; q = 0 - tmp;
            tmp = v; v = r; r = 0 - tmp;
        }
        const int limit = (int)eta + 1 > i ? i : (int)eta + 1;
        const uint64_t m = UINT64_MAX >> (64 - limit);
        uint64_t w;
        if (swap) {
            w = (f * g * (f * f - 2)) & m & 63;
        } else {
            w = f + (((f + 1) & 4) << 1);                                /* f^-1 mod 16 */
            w = (0 - w * g) & m & 15;
        }
        g += f * w;
        q += u * w;
        r += v * w;
    }
    t->u = (int64_t)u;
    t->v = (int64_t)v;
    t->q = (int64_t)q;
    t->r = (int64_t)r;
    return eta;
}

/* (d, e) = (t (d, e) + p (md, me)) / 2^62, md and me chosen to make the division exact and to keep both in (-2p, p) */
static void fe_update_de_62(fe_s62 *d, fe_s62 *e, const fe_t2x2 *t)
{
    const int64_t u = t->u, v = t->v, q = t->q, r = t->r;
    const int64_t sd = d->v[4] >> 63, se = e->v[4] >> 63;                /* a negative d or e gets p added */
    int64_t md = (u & sd) + (v & se), me = (q & sd) + (r & se);
    pz_i128_ cd = (pz_i128_)u * d->v[0] + (pz_i128_)v * e->v[0];
    pz_i128_ ce = (pz_i128_)q * d->v[0] + (pz_i128_)r * e->v[0];
    md -= (int64_t)((FE_PINV62 * (uint64_t)cd + (uint64_t)md) & FE_M62);
    me -= (int64_t)((FE_PINV62 * (uint64_t)ce + (uint64_t)me) & FE_M62);
    cd += (pz_i128_)FE_P62.v[0] * md;
    ce += (pz_i128_)FE_P62.v[0] * me;
    cd >>= 62;                                                            /* the low 62 bits are zero */
    ce >>= 62;
    for (int i = 1; i < 5; i++) {                                         /* limbs 1 to 3 of p are zero */
        cd += (pz_i128_)u * d->v[i] + (pz_i128_)v * e->v[i];
        ce += (pz_i128_)q * d->v[i] + (pz_i128_)r * e->v[i];
        if (i == 4) {
            cd += (pz_i128_)FE_P62.v[4] * md;
            ce += (pz_i128_)FE_P62.v[4] * me;
        }
        d->v[i - 1] = (int64_t)((uint64_t)cd & FE_M62);
        e->v[i - 1] = (int64_t)((uint64_t)ce & FE_M62);
        cd >>= 62;
        ce >>= 62;
    }
    d->v[4] = (int64_t)cd;
    e->v[4] = (int64_t)ce;
}

/* (f, g) = t (f, g) / 2^62 on the low len limbs (the others are known to be 0 or -1, folded into limb len - 1) */
static void fe_update_fg_62(int len, fe_s62 *f, fe_s62 *g, const fe_t2x2 *t)
{
    const int64_t u = t->u, v = t->v, q = t->q, r = t->r;
    pz_i128_ cf = (pz_i128_)u * f->v[0] + (pz_i128_)v * g->v[0];
    pz_i128_ cg = (pz_i128_)q * f->v[0] + (pz_i128_)r * g->v[0];
    cf >>= 62;
    cg >>= 62;
    for (int i = 1; i < len; i++) {
        cf += (pz_i128_)u * f->v[i] + (pz_i128_)v * g->v[i];
        cg += (pz_i128_)q * f->v[i] + (pz_i128_)r * g->v[i];
        f->v[i - 1] = (int64_t)((uint64_t)cf & FE_M62);
        g->v[i - 1] = (int64_t)((uint64_t)cg & FE_M62);
        cf >>= 62;
        cg >>= 62;
    }
    f->v[len - 1] = (int64_t)cf;
    g->v[len - 1] = (int64_t)cg;
}

void pz_fe_inv(pz_fe *r, const pz_fe *a)
{
    pz_fe x = *a;
    pz_fe_normalize(&x);
    fe_s62 d = {{0, 0, 0, 0, 0}}, e = {{1, 0, 0, 0, 0}}, f = FE_P62, g;
    g.v[0] = (int64_t)(x.n[0] & FE_M62);
    g.v[1] = (int64_t)((x.n[0] >> 62 | x.n[1] << 2) & FE_M62);
    g.v[2] = (int64_t)((x.n[1] >> 60 | x.n[2] << 4) & FE_M62);
    g.v[3] = (int64_t)((x.n[2] >> 58 | x.n[3] << 6) & FE_M62);
    g.v[4] = (int64_t)(x.n[3] >> 56);
    int64_t eta = -1;
    int len = 5;
    for (;;) {
        fe_t2x2 t;
        eta = fe_divsteps_62(eta, (uint64_t)f.v[0], (uint64_t)g.v[0], &t);
        fe_update_de_62(&d, &e, &t);
        fe_update_fg_62(len, &f, &g, &t);
        if (g.v[0] == 0) {
            int64_t z = 0;
            for (int j = 1; j < len; j++) z |= g.v[j];
            if (z == 0) break;
        }
        /* when the top limbs of f and g are both 0 or -1, fold them into the limb below */
        const int64_t fn = f.v[len - 1], gn = g.v[len - 1];
        if (len > 1 && (fn ^ (fn >> 63)) == 0 && (gn ^ (gn >> 63)) == 0) {
            f.v[len - 2] = (int64_t)((uint64_t)f.v[len - 2] | (uint64_t)fn << 62);
            g.v[len - 2] = (int64_t)((uint64_t)g.v[len - 2] | (uint64_t)gn << 62);
            len--;
        }
    }
    /* d in (-2p, p): carry it, add 2p (limbs of p in [0, 2^62)) and carry again, so that 0 < d < 3p < 2^258 with every limb in
     * [0, 2^62); then the 256-bit field element, its top bits folded back, and the sign of f */
    static const int64_t p2[5] = {2 * 0x3FFFFFFEFFFFFC2FLL, 2 * (int64_t)FE_M62, 2 * (int64_t)FE_M62, 2 * (int64_t)FE_M62, 2 * 0xFF};
    pz_i128_ c = 0;
    for (int i = 0; i < 5; i++) {
        c += (pz_i128_)d.v[i] + p2[i];
        d.v[i] = i < 4 ? (int64_t)((uint64_t)c & FE_M62) : (int64_t)c;
        c >>= 62;
    }
    const uint64_t v0 = (uint64_t)d.v[0], v1 = (uint64_t)d.v[1], v2 = (uint64_t)d.v[2], v3 = (uint64_t)d.v[3], v4 = (uint64_t)d.v[4];
    uint64_t w[4] = {v0 | v1 << 62, v1 >> 2 | v2 << 60, v2 >> 4 | v3 << 58, v3 >> 6 | v4 << 56};
    pz_fe_fold_(w, v4 >> 8);
    pz_fe res = {{w[0], w[1], w[2], w[3]}};
    pz_fe_normalize(&res);
    if (f.v[len - 1] < 0) {
        const pz_fe zero = {{0, 0, 0, 0}};
        pz_fe_sub(&res, &zero, &res);
        pz_fe_normalize(&res);
    }
    *r = res;
}
