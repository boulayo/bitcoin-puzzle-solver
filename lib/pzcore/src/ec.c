/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.c - secp256k1 point arithmetic. */
#include <pzcore/ec.h>
#include <sched.h>
#include <stdatomic.h>

const pz_ge pz_ec_g = {
    {{0x59F2815B16F81798ULL, 0x029BFCDB2DCE28D9ULL, 0x55A06295CE870B07ULL, 0x79BE667EF9DCBBACULL}},
    {{0x9C47D08FFB10D4B8ULL, 0xFD17B448A6855419ULL, 0x5DA4FBFC0E1108A8ULL, 0x483ADA7726A3C465ULL}}
};

/* comb table for k*G: g_comb[i][d-1] = d * 16^i * G, for the 64 windows i of four bits and the digits d = 1..15 */
#define COMB_W 64
static pz_ge g_comb[COMB_W][15];
static _Atomic int g_comb_state;      /* 0: not built, 1: a thread is building it, 2: ready */

static void gej_set_inf(pz_gej *r)
{
    pz_fe_zero(&r->x);
    pz_fe_zero(&r->y);
    pz_fe_zero(&r->z);
    r->inf = true;
}

void pz_gej_set_ge(pz_gej *r, const pz_ge *a)
{
    r->x = a->x;
    r->y = a->y;
    pz_fe_set_u64(&r->z, 1);
    r->inf = false;
}

void pz_gej_double(pz_gej *r, const pz_gej *a)
{
    if (a->inf) { *r = *a; return; }
    pz_fe A, B, C, D, E, F, t;
    pz_fe_sqr(&A, &a->x);
    pz_fe_sqr(&B, &a->y);
    pz_fe_sqr(&C, &B);
    pz_fe_add(&t, &a->x, &B);
    pz_fe_sqr(&D, &t);
    pz_fe_sub(&D, &D, &A);
    pz_fe_sub(&D, &D, &C);
    pz_fe_add(&D, &D, &D);
    pz_fe_add(&E, &A, &A);
    pz_fe_add(&E, &E, &A);
    pz_fe_sqr(&F, &E);
    pz_fe nz;
    pz_fe_mul(&nz, &a->y, &a->z);
    pz_fe_add(&nz, &nz, &nz);
    pz_fe nx, ny;
    pz_fe_add(&t, &D, &D);
    pz_fe_sub(&nx, &F, &t);
    pz_fe_sub(&t, &D, &nx);
    pz_fe_mul(&ny, &E, &t);
    pz_fe_add(&C, &C, &C);
    pz_fe_add(&C, &C, &C);
    pz_fe_add(&C, &C, &C);
    pz_fe_sub(&ny, &ny, &C);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

void pz_gej_add_ge(pz_gej *r, const pz_gej *a, const pz_ge *b)
{
    if (a->inf) { pz_gej_set_ge(r, b); return; }
    pz_fe z1z1, u2, s2, h, hh, i, j, rr, v, t;
    pz_fe_sqr(&z1z1, &a->z);
    pz_fe_mul(&u2, &b->x, &z1z1);
    pz_fe_mul(&s2, &b->y, &a->z);
    pz_fe_mul(&s2, &s2, &z1z1);
    pz_fe_sub(&h, &u2, &a->x);
    pz_fe_sub(&rr, &s2, &a->y);
    pz_fe hn = h, rn = rr;
    pz_fe_normalize(&hn);
    if (PZ_UNLIKELY(pz_fe_is_zero(&hn))) {
        pz_fe_normalize(&rn);
        if (pz_fe_is_zero(&rn)) { pz_gej_double(r, a); return; }
        gej_set_inf(r);
        return;
    }
    pz_fe_sqr(&hh, &h);
    pz_fe_add(&i, &hh, &hh);
    pz_fe_add(&i, &i, &i);
    pz_fe_mul(&j, &h, &i);
    pz_fe_add(&rr, &rr, &rr);
    pz_fe_mul(&v, &a->x, &i);
    pz_fe nx, ny, nz;
    pz_fe_sqr(&nx, &rr);
    pz_fe_sub(&nx, &nx, &j);
    pz_fe_sub(&nx, &nx, &v);
    pz_fe_sub(&nx, &nx, &v);
    pz_fe_sub(&t, &v, &nx);
    pz_fe_mul(&ny, &rr, &t);
    pz_fe_mul(&t, &a->y, &j);
    pz_fe_add(&t, &t, &t);
    pz_fe_sub(&ny, &ny, &t);
    pz_fe_add(&t, &a->z, &h);
    pz_fe_sqr(&nz, &t);
    pz_fe_sub(&nz, &nz, &z1z1);
    pz_fe_sub(&nz, &nz, &hh);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

void pz_gej_add(pz_gej *r, const pz_gej *a, const pz_gej *b)
{
    if (a->inf) { *r = *b; return; }
    if (b->inf) { *r = *a; return; }
    pz_fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    pz_fe_sqr(&z1z1, &a->z);
    pz_fe_sqr(&z2z2, &b->z);
    pz_fe_mul(&u1, &a->x, &z2z2);
    pz_fe_mul(&u2, &b->x, &z1z1);
    pz_fe_mul(&s1, &a->y, &b->z);
    pz_fe_mul(&s1, &s1, &z2z2);
    pz_fe_mul(&s2, &b->y, &a->z);
    pz_fe_mul(&s2, &s2, &z1z1);
    pz_fe_sub(&h, &u2, &u1);
    pz_fe_sub(&rr, &s2, &s1);
    pz_fe hn = h, rn = rr;
    pz_fe_normalize(&hn);
    if (PZ_UNLIKELY(pz_fe_is_zero(&hn))) {
        pz_fe_normalize(&rn);
        if (pz_fe_is_zero(&rn)) { pz_gej_double(r, a); return; }
        gej_set_inf(r);
        return;
    }
    pz_fe_add(&t, &h, &h);
    pz_fe_sqr(&i, &t);
    pz_fe_mul(&j, &h, &i);
    pz_fe_add(&rr, &rr, &rr);
    pz_fe_mul(&v, &u1, &i);
    pz_fe nx, ny, nz;
    pz_fe_sqr(&nx, &rr);
    pz_fe_sub(&nx, &nx, &j);
    pz_fe_sub(&nx, &nx, &v);
    pz_fe_sub(&nx, &nx, &v);
    pz_fe_sub(&t, &v, &nx);
    pz_fe_mul(&ny, &rr, &t);
    pz_fe_mul(&t, &s1, &j);
    pz_fe_add(&t, &t, &t);
    pz_fe_sub(&ny, &ny, &t);
    pz_fe_add(&t, &a->z, &b->z);
    pz_fe_sqr(&nz, &t);
    pz_fe_sub(&nz, &nz, &z1z1);
    pz_fe_sub(&nz, &nz, &z2z2);
    pz_fe_mul(&nz, &nz, &h);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

bool pz_gej_to_ge_batch(pz_ge *out, const pz_gej *in, size_t n)
{
    if (!n) return true;
    for (size_t i = 0; i < n; i++)
        if (in[i].inf) return false;
    /* out[i].x holds the product z_0 * ... * z_i until the backward pass has used it; out is not otherwise needed before */
    pz_fe acc = in[0].z;
    out[0].x = acc;
    for (size_t i = 1; i < n; i++) {
        pz_fe_mul(&acc, &acc, &in[i].z);
        out[i].x = acc;
    }
    pz_fe chk = acc;
    pz_fe_normalize(&chk);
    if (PZ_UNLIKELY(pz_fe_is_zero(&chk))) return false;      /* some z is zero: not a point */
    pz_fe inv, zi, zi2, zi3;
    pz_fe_inv(&inv, &acc);
    for (size_t i = n - 1; i > 0; i--) {
        pz_fe_mul(&zi, &inv, &out[i - 1].x);                 /* 1 / z_i */
        pz_fe_mul(&inv, &inv, &in[i].z);                     /* 1 / (z_0 ... z_{i-1}) */
        pz_fe_sqr(&zi2, &zi);
        pz_fe_mul(&zi3, &zi2, &zi);
        pz_fe_mul(&out[i].x, &in[i].x, &zi2);
        pz_fe_mul(&out[i].y, &in[i].y, &zi3);
        pz_fe_normalize(&out[i].x);
        pz_fe_normalize(&out[i].y);
    }
    pz_fe_sqr(&zi2, &inv);
    pz_fe_mul(&zi3, &zi2, &inv);
    pz_fe_mul(&out[0].x, &in[0].x, &zi2);
    pz_fe_mul(&out[0].y, &in[0].y, &zi3);
    pz_fe_normalize(&out[0].x);
    pz_fe_normalize(&out[0].y);
    return true;
}

bool pz_gej_to_ge(pz_ge *r, const pz_gej *a)
{
    if (a->inf) return false;
    pz_fe zi, zi2, zi3;
    pz_fe_inv(&zi, &a->z);
    pz_fe_sqr(&zi2, &zi);
    pz_fe_mul(&zi3, &zi2, &zi);
    pz_fe_mul(&r->x, &a->x, &zi2);
    pz_fe_mul(&r->y, &a->y, &zi3);
    pz_fe_normalize(&r->x);
    pz_fe_normalize(&r->y);
    return true;
}

void pz_ge_neg(pz_ge *r, const pz_ge *a)
{
    r->x = a->x;
    pz_fe_neg(&r->y, &a->y);
    pz_fe_normalize(&r->y);
}

bool pz_ge_equal(const pz_ge *a, const pz_ge *b) { return pz_fe_equiv(&a->x, &b->x) && pz_fe_equiv(&a->y, &b->y); }

bool pz_ge_on_curve(const pz_ge *p)
{
    pz_fe y2, x3, seven;
    pz_fe_sqr(&y2, &p->y);
    pz_fe_sqr(&x3, &p->x);
    pz_fe_mul(&x3, &x3, &p->x);
    pz_fe_set_u64(&seven, 7);
    pz_fe_add(&x3, &x3, &seven);
    return pz_fe_equiv(&y2, &x3);
}

/* Builds the table in groups of eight windows, so that the Jacobian points of a group fit in a small buffer on the stack and are
 * brought to affine with one inversion per group. */
static void comb_build(void)
{
    enum { GROUP = 8 };
    pz_gej pts[GROUP * 15];
    pz_gej base;
    pz_gej_set_ge(&base, &pz_ec_g);
    for (int w0 = 0; w0 < COMB_W; w0 += GROUP) {
        for (int i = 0; i < GROUP; i++) {
            pz_gej cur = base;
            pts[i * 15] = cur;
            for (int j = 1; j < 15; j++) {
                pz_gej_add(&cur, &cur, &base);
                pts[i * 15 + j] = cur;
            }
            for (int k = 0; k < 4; k++) pz_gej_double(&base, &base);
        }
        pz_gej_to_ge_batch(&g_comb[w0][0], pts, GROUP * 15);   /* cannot fail: no multiple of G below 16^64 is the identity */
    }
}

void pz_ec_init(void)
{
    if (atomic_load_explicit(&g_comb_state, memory_order_acquire) == 2) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_comb_state, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        comb_build();
        atomic_store_explicit(&g_comb_state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_comb_state, memory_order_acquire) != 2) sched_yield();
}

void pz_ec_mul_g_j(pz_gej *r, const pz_u256 *k)
{
    pz_ec_init();
    pz_gej acc;
    gej_set_inf(&acc);
    for (int i = 0; i < COMB_W; i++) {
        unsigned d = (unsigned)(k->w[i >> 4] >> ((i & 15) * 4)) & 15;
        if (d) pz_gej_add_ge(&acc, &acc, &g_comb[i][d - 1]);
    }
    *r = acc;
}

bool pz_ec_mul_g(pz_ge *r, const pz_u256 *k)
{
    pz_gej j;
    pz_ec_mul_g_j(&j, k);
    return pz_gej_to_ge(r, &j);
}

bool pz_ge_add(pz_ge *r, const pz_ge *a, const pz_ge *b)
{
    pz_gej j;
    pz_gej_set_ge(&j, a);
    pz_gej_add_ge(&j, &j, b);
    return pz_gej_to_ge(r, &j);
}

bool pz_ge_sub(pz_ge *r, const pz_ge *a, const pz_ge *b)
{
    pz_ge nb;
    pz_ge_neg(&nb, b);
    return pz_ge_add(r, a, &nb);
}

void pz_ge_to_pub33(uint8_t out[33], const pz_ge *p)
{
    pz_fe y = p->y;
    pz_fe_normalize(&y);
    out[0] = pz_fe_is_odd(&y) ? 3 : 2;
    pz_fe_to_be(out + 1, &p->x);
}

void pz_ge_to_pub65(uint8_t out[65], const pz_ge *p)
{
    out[0] = 4;
    pz_fe_to_be(out + 1, &p->x);
    pz_fe_to_be(out + 33, &p->y);
}

bool pz_ge_from_pubkey(pz_ge *r, const uint8_t *b, size_t len)
{
    if (!b) return false;
    pz_u256 xv, yv;
    pz_ge t;
    if (len == 33 && (b[0] == 2 || b[0] == 3)) {
        pz_u256_from_be(&xv, b + 1);
        pz_u256 pv;
        for (int i = 0; i < 4; i++) pv.w[i] = pz_fe_p.n[i];
        if (pz_u256_cmp(&xv, &pv) >= 0) return false;
        pz_fe y2, seven;
        pz_fe_from_u256(&t.x, &xv);
        pz_fe_sqr(&y2, &t.x);
        pz_fe_mul(&y2, &y2, &t.x);
        pz_fe_set_u64(&seven, 7);
        pz_fe_add(&y2, &y2, &seven);
        if (!pz_fe_sqrt(&t.y, &y2)) return false;
        if ((pz_fe_is_odd(&t.y) ? 1 : 0) != (b[0] & 1)) { pz_fe_neg(&t.y, &t.y); pz_fe_normalize(&t.y); }
        *r = t;
        return true;
    }
    if (len == 65 && b[0] == 4) {
        pz_u256 pv;
        for (int i = 0; i < 4; i++) pv.w[i] = pz_fe_p.n[i];
        pz_u256_from_be(&xv, b + 1);
        pz_u256_from_be(&yv, b + 33);
        if (pz_u256_cmp(&xv, &pv) >= 0 || pz_u256_cmp(&yv, &pv) >= 0) return false;   /* not the canonical encoding of a coordinate */
        pz_fe_from_u256(&t.x, &xv);
        pz_fe_from_u256(&t.y, &yv);
        if (!pz_ge_on_curve(&t)) return false;
        *r = t;
        return true;
    }
    return false;
}
