/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.c - secp256k1 point arithmetic. */
#include "ec.h"
#include <pthread.h>

const ge EC_G = {
    {{0x59F2815B16F81798ULL, 0x029BFCDB2DCE28D9ULL, 0x55A06295CE870B07ULL, 0x79BE667EF9DCBBACULL}},
    {{0x9C47D08FFB10D4B8ULL, 0xFD17B448A6855419ULL, 0x5DA4FBFC0E1108A8ULL, 0x483ADA7726A3C465ULL}}
};

#define COMB_W 64
static ge g_comb[COMB_W][15];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

void gej_set_ge(gej *r, const ge *a)
{
    r->x = a->x;
    r->y = a->y;
    fe_set_u64(&r->z, 1);
    r->inf = false;
}

void gej_double(gej *r, const gej *a)
{
    if (a->inf) { *r = *a; return; }
    fe A, B, C, D, E, F, t;
    fe_sqr(&A, &a->x);
    fe_sqr(&B, &a->y);
    fe_sqr(&C, &B);
    fe_add(&t, &a->x, &B);
    fe_sqr(&D, &t);
    fe_sub(&D, &D, &A);
    fe_sub(&D, &D, &C);
    fe_add(&D, &D, &D);
    fe_add(&E, &A, &A);
    fe_add(&E, &E, &A);
    fe_sqr(&F, &E);
    fe nz;
    fe_mul(&nz, &a->y, &a->z);
    fe_add(&nz, &nz, &nz);
    fe nx, ny;
    fe_add(&t, &D, &D);
    fe_sub(&nx, &F, &t);
    fe_sub(&t, &D, &nx);
    fe_mul(&ny, &E, &t);
    fe_add(&C, &C, &C);
    fe_add(&C, &C, &C);
    fe_add(&C, &C, &C);
    fe_sub(&ny, &ny, &C);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

void gej_add_ge(gej *r, const gej *a, const ge *b)
{
    if (a->inf) { gej_set_ge(r, b); return; }
    fe z1z1, u2, s2, h, hh, i, j, rr, v, t;
    fe_sqr(&z1z1, &a->z);
    fe_mul(&u2, &b->x, &z1z1);
    fe_mul(&s2, &b->y, &a->z);
    fe_mul(&s2, &s2, &z1z1);
    fe_sub(&h, &u2, &a->x);
    fe_sub(&rr, &s2, &a->y);
    fe hn = h, rn = rr;
    fe_normalize(&hn);
    if (UNLIKELY(fe_is_zero(&hn))) {
        fe_normalize(&rn);
        if (fe_is_zero(&rn)) { gej_double(r, a); return; }
        r->inf = true;
        return;
    }
    fe_sqr(&hh, &h);
    fe_add(&i, &hh, &hh);
    fe_add(&i, &i, &i);
    fe_mul(&j, &h, &i);
    fe_add(&rr, &rr, &rr);
    fe_mul(&v, &a->x, &i);
    fe nx, ny, nz;
    fe_sqr(&nx, &rr);
    fe_sub(&nx, &nx, &j);
    fe_sub(&nx, &nx, &v);
    fe_sub(&nx, &nx, &v);
    fe_sub(&t, &v, &nx);
    fe_mul(&ny, &rr, &t);
    fe_mul(&t, &a->y, &j);
    fe_add(&t, &t, &t);
    fe_sub(&ny, &ny, &t);
    fe_add(&t, &a->z, &h);
    fe_sqr(&nz, &t);
    fe_sub(&nz, &nz, &z1z1);
    fe_sub(&nz, &nz, &hh);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

void gej_add(gej *r, const gej *a, const gej *b)
{
    if (a->inf) { *r = *b; return; }
    if (b->inf) { *r = *a; return; }
    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    fe_sqr(&z1z1, &a->z);
    fe_sqr(&z2z2, &b->z);
    fe_mul(&u1, &a->x, &z2z2);
    fe_mul(&u2, &b->x, &z1z1);
    fe_mul(&s1, &a->y, &b->z);
    fe_mul(&s1, &s1, &z2z2);
    fe_mul(&s2, &b->y, &a->z);
    fe_mul(&s2, &s2, &z1z1);
    fe_sub(&h, &u2, &u1);
    fe_sub(&rr, &s2, &s1);
    fe hn = h, rn = rr;
    fe_normalize(&hn);
    if (UNLIKELY(fe_is_zero(&hn))) {
        fe_normalize(&rn);
        if (fe_is_zero(&rn)) { gej_double(r, a); return; }
        r->inf = true;
        return;
    }
    fe_add(&t, &h, &h);
    fe_sqr(&i, &t);
    fe_mul(&j, &h, &i);
    fe_add(&rr, &rr, &rr);
    fe_mul(&v, &u1, &i);
    fe nx, ny, nz;
    fe_sqr(&nx, &rr);
    fe_sub(&nx, &nx, &j);
    fe_sub(&nx, &nx, &v);
    fe_sub(&nx, &nx, &v);
    fe_sub(&t, &v, &nx);
    fe_mul(&ny, &rr, &t);
    fe_mul(&t, &s1, &j);
    fe_add(&t, &t, &t);
    fe_sub(&ny, &ny, &t);
    fe_add(&t, &a->z, &b->z);
    fe_sqr(&nz, &t);
    fe_sub(&nz, &nz, &z1z1);
    fe_sub(&nz, &nz, &z2z2);
    fe_mul(&nz, &nz, &h);
    r->x = nx; r->y = ny; r->z = nz;
    r->inf = false;
}

void gej_to_ge_batch(ge *out, const gej *in, size_t n)
{
    if (!n) return;
    fe *zs = pz_xmalloc(n * sizeof(fe)), *tmp = pz_xmalloc(n * sizeof(fe));
    for (size_t i = 0; i < n; i++) zs[i] = in[i].z;
    if (!fe_inv_batch(zs, tmp, n)) pz_die("gej_to_ge_batch: unexpected point at infinity");
    for (size_t i = 0; i < n; i++) {
        fe zi2, zi3;
        fe_sqr(&zi2, &zs[i]);
        fe_mul(&zi3, &zi2, &zs[i]);
        fe_mul(&out[i].x, &in[i].x, &zi2);
        fe_mul(&out[i].y, &in[i].y, &zi3);
        fe_normalize(&out[i].x);
        fe_normalize(&out[i].y);
    }
    free(zs);
    free(tmp);
}

bool gej_to_ge(ge *r, const gej *a)
{
    if (a->inf) return false;
    fe zi, zi2, zi3;
    fe_inv(&zi, &a->z);
    fe_sqr(&zi2, &zi);
    fe_mul(&zi3, &zi2, &zi);
    fe_mul(&r->x, &a->x, &zi2);
    fe_mul(&r->y, &a->y, &zi3);
    fe_normalize(&r->x);
    fe_normalize(&r->y);
    return true;
}

void ge_neg(ge *r, const ge *a)
{
    r->x = a->x;
    fe_neg(&r->y, &a->y);
    fe_normalize(&r->y);
}

bool ge_equal(const ge *a, const ge *b) { return fe_equiv(&a->x, &b->x) && fe_equiv(&a->y, &b->y); }

bool ge_on_curve(const ge *p)
{
    fe y2, x3, seven;
    fe_sqr(&y2, &p->y);
    fe_sqr(&x3, &p->x);
    fe_mul(&x3, &x3, &p->x);
    fe_set_u64(&seven, 7);
    fe_add(&x3, &x3, &seven);
    return fe_equiv(&y2, &x3);
}

bool fe_sqrt(fe *r, const fe *a)
{
    u256 e;
    for (int i = 0; i < 4; i++) e.w[i] = FE_P.n[i];
    u256_add_u64(&e, &e, 1);
    e = u256_shr(&e, 2);
    fe acc;
    fe_set_u64(&acc, 1);
    for (int i = 255; i >= 0; i--) {
        fe_sqr(&acc, &acc);
        if (u256_bit(&e, (unsigned)i)) fe_mul(&acc, &acc, a);
    }
    fe chk;
    fe_sqr(&chk, &acc);
    *r = acc;
    fe_normalize(r);
    return fe_equiv(&chk, a);
}

static void comb_build(void)
{
    gej *pts = pz_xmalloc(sizeof(gej) * COMB_W * 15);
    gej base;
    gej_set_ge(&base, &EC_G);
    for (int i = 0; i < COMB_W; i++) {
        gej cur = base;
        pts[i * 15] = cur;
        for (int j = 1; j < 15; j++) {
            gej_add(&cur, &cur, &base);
            pts[i * 15 + j] = cur;
        }
        gej_double(&base, &base);
        gej_double(&base, &base);
        gej_double(&base, &base);
        gej_double(&base, &base);
    }
    gej_to_ge_batch(&g_comb[0][0], pts, COMB_W * 15);
    free(pts);
}

void ec_init(void) { pthread_once(&g_once, comb_build); }

void ec_mul_g_j(gej *r, const u256 *k)
{
    ec_init();
    gej acc;
    acc.inf = true;
    fe_zero(&acc.x); fe_zero(&acc.y); fe_zero(&acc.z);
    for (int i = 0; i < COMB_W; i++) {
        unsigned d = (unsigned)(k->w[i >> 4] >> ((i & 15) * 4)) & 15;
        if (d) gej_add_ge(&acc, &acc, &g_comb[i][d - 1]);
    }
    *r = acc;
}

bool ec_mul_g(ge *r, const u256 *k)
{
    gej j;
    ec_mul_g_j(&j, k);
    return gej_to_ge(r, &j);
}

bool ge_add(ge *r, const ge *a, const ge *b)
{
    gej j;
    gej_set_ge(&j, a);
    gej_add_ge(&j, &j, b);
    return gej_to_ge(r, &j);
}

bool ge_sub(ge *r, const ge *a, const ge *b)
{
    ge nb;
    ge_neg(&nb, b);
    return ge_add(r, a, &nb);
}

void ge_to_pub33(u8 out[33], const ge *p)
{
    fe x = p->x, y = p->y;
    fe_normalize(&y);
    out[0] = fe_is_odd(&y) ? 3 : 2;
    fe_to_be(out + 1, &x);
}

bool ge_from_pubkey(ge *r, const u8 *b, size_t len)
{
    if (len == 33 && (b[0] == 2 || b[0] == 3)) {
        u256 xv;
        u256_from_be(&xv, b + 1);
        u256 pv;
        for (int i = 0; i < 4; i++) pv.w[i] = FE_P.n[i];
        if (u256_cmp(&xv, &pv) >= 0) return false;
        fe x, y2, seven, y;
        fe_from_u256(&x, &xv);
        fe_sqr(&y2, &x);
        fe_mul(&y2, &y2, &x);
        fe_set_u64(&seven, 7);
        fe_add(&y2, &y2, &seven);
        if (!fe_sqrt(&y, &y2)) return false;
        if ((fe_is_odd(&y) ? 1 : 0) != (b[0] & 1)) { fe_neg(&y, &y); fe_normalize(&y); }
        r->x = x;
        r->y = y;
        return true;
    }
    if (len == 65 && b[0] == 4) {
        fe_from_be(&r->x, b + 1);
        fe_from_be(&r->y, b + 33);
        return ge_on_curve(r);
    }
    return false;
}
