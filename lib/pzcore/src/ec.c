/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.c - secp256k1 point arithmetic. */
#include <pzcore/ec.h>
#include <pthread.h>

const pz_ge pz_ec_g = {
    {{0x59F2815B16F81798ULL, 0x029BFCDB2DCE28D9ULL, 0x55A06295CE870B07ULL, 0x79BE667EF9DCBBACULL}},
    {{0x9C47D08FFB10D4B8ULL, 0xFD17B448A6855419ULL, 0x5DA4FBFC0E1108A8ULL, 0x483ADA7726A3C465ULL}}
};

#define COMB_W 64
static pz_ge g_comb[COMB_W][15];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

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
    if (UNLIKELY(pz_fe_is_zero(&hn))) {
        pz_fe_normalize(&rn);
        if (pz_fe_is_zero(&rn)) { pz_gej_double(r, a); return; }
        r->inf = true;
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
    if (UNLIKELY(pz_fe_is_zero(&hn))) {
        pz_fe_normalize(&rn);
        if (pz_fe_is_zero(&rn)) { pz_gej_double(r, a); return; }
        r->inf = true;
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

void pz_gej_to_ge_batch(pz_ge *out, const pz_gej *in, size_t n)
{
    if (!n) return;
    pz_fe *zs = pz_xmalloc(n * sizeof(pz_fe)), *tmp = pz_xmalloc(n * sizeof(pz_fe));
    for (size_t i = 0; i < n; i++) zs[i] = in[i].z;
    if (!pz_fe_inv_batch(zs, tmp, n)) pz_die("gej_to_ge_batch: unexpected point at infinity");
    for (size_t i = 0; i < n; i++) {
        pz_fe zi2, zi3;
        pz_fe_sqr(&zi2, &zs[i]);
        pz_fe_mul(&zi3, &zi2, &zs[i]);
        pz_fe_mul(&out[i].x, &in[i].x, &zi2);
        pz_fe_mul(&out[i].y, &in[i].y, &zi3);
        pz_fe_normalize(&out[i].x);
        pz_fe_normalize(&out[i].y);
    }
    free(zs);
    free(tmp);
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

bool pz_fe_sqrt(pz_fe *r, const pz_fe *a)
{
    pz_u256 e;
    for (int i = 0; i < 4; i++) e.w[i] = pz_fe_p.n[i];
    pz_u256_add_u64(&e, &e, 1);
    e = pz_u256_shr(&e, 2);
    pz_fe acc;
    pz_fe_set_u64(&acc, 1);
    for (int i = 255; i >= 0; i--) {
        pz_fe_sqr(&acc, &acc);
        if (pz_u256_bit(&e, (unsigned)i)) pz_fe_mul(&acc, &acc, a);
    }
    pz_fe chk;
    pz_fe_sqr(&chk, &acc);
    *r = acc;
    pz_fe_normalize(r);
    return pz_fe_equiv(&chk, a);
}

static void comb_build(void)
{
    pz_gej *pts = pz_xmalloc(sizeof(pz_gej) * COMB_W * 15);
    pz_gej base;
    pz_gej_set_ge(&base, &pz_ec_g);
    for (int i = 0; i < COMB_W; i++) {
        pz_gej cur = base;
        pts[i * 15] = cur;
        for (int j = 1; j < 15; j++) {
            pz_gej_add(&cur, &cur, &base);
            pts[i * 15 + j] = cur;
        }
        pz_gej_double(&base, &base);
        pz_gej_double(&base, &base);
        pz_gej_double(&base, &base);
        pz_gej_double(&base, &base);
    }
    pz_gej_to_ge_batch(&g_comb[0][0], pts, COMB_W * 15);
    free(pts);
}

void pz_ec_init(void) { pthread_once(&g_once, comb_build); }

void pz_ec_mul_g_j(pz_gej *r, const pz_u256 *k)
{
    pz_ec_init();
    pz_gej acc;
    acc.inf = true;
    pz_fe_zero(&acc.x); pz_fe_zero(&acc.y); pz_fe_zero(&acc.z);
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

void pz_ge_to_pub33(u8 out[33], const pz_ge *p)
{
    pz_fe x = p->x, y = p->y;
    pz_fe_normalize(&y);
    out[0] = pz_fe_is_odd(&y) ? 3 : 2;
    pz_fe_to_be(out + 1, &x);
}

bool pz_ge_from_pubkey(pz_ge *r, const u8 *b, size_t len)
{
    if (len == 33 && (b[0] == 2 || b[0] == 3)) {
        pz_u256 xv;
        pz_u256_from_be(&xv, b + 1);
        pz_u256 pv;
        for (int i = 0; i < 4; i++) pv.w[i] = pz_fe_p.n[i];
        if (pz_u256_cmp(&xv, &pv) >= 0) return false;
        pz_fe x, y2, seven, y;
        pz_fe_from_u256(&x, &xv);
        pz_fe_sqr(&y2, &x);
        pz_fe_mul(&y2, &y2, &x);
        pz_fe_set_u64(&seven, 7);
        pz_fe_add(&y2, &y2, &seven);
        if (!pz_fe_sqrt(&y, &y2)) return false;
        if ((pz_fe_is_odd(&y) ? 1 : 0) != (b[0] & 1)) { pz_fe_neg(&y, &y); pz_fe_normalize(&y); }
        r->x = x;
        r->y = y;
        return true;
    }
    if (len == 65 && b[0] == 4) {
        pz_fe_from_be(&r->x, b + 1);
        pz_fe_from_be(&r->y, b + 33);
        return pz_ge_on_curve(r);
    }
    return false;
}
