/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_ec.c - the curve: k*G against values computed by Python, the group law, the point at infinity, batch conversion, public-key parsing
 * (valid and hostile), the square root, and the first use of the multiplication table by several threads at once. */
#include <pzcore/ec.h>
#include <pzcore/hash.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "testutil.h"
#include "vectors.h"

#define NVEC (sizeof EC_VEC / sizeof *EC_VEC)

static void hex32(const pz_fe *f, char *out)
{
    uint8_t b[32];
    pz_fe_to_be(b, f);
    tu_hex(b, 32, out);
}

/* ---- the first use of the table, from several threads at once (must run before anything else touches it) ---- */
static atomic_int arrived;
static int thread_bad[8];

static void *racer(void *arg)
{
    const int id = (int)(intptr_t)arg;
    atomic_fetch_add(&arrived, 1);                      /* all start together (pthread barriers do not exist everywhere) */
    while (atomic_load(&arrived) < 8) sched_yield();
    for (size_t i = 0; i < NVEC; i++) {
        const size_t v = (i + (size_t)id) % NVEC;
        pz_u256 k;
        pz_ge p;
        char hx[65];
        if (!pz_u256_from_hex(&k, EC_VEC[v].k) || !pz_ec_mul_g(&p, &k)) { thread_bad[id]++; continue; }
        hex32(&p.x, hx);
        if (strcmp(hx, EC_VEC[v].x) != 0) thread_bad[id]++;
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t th[8];
    for (intptr_t i = 0; i < 8; i++) pthread_create(&th[i], NULL, racer, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 8; i++) CHECK(thread_bad[i] == 0);
}

static void test_vectors(void)
{
    for (size_t i = 0; i < NVEC; i++) {
        pz_u256 k;
        pz_ge p, q;
        char hx[65], hy[65], hh[41];
        uint8_t pub[33], pub65[65], h[20];
        CHECK(pz_u256_from_hex(&k, EC_VEC[i].k));
        CHECK(pz_ec_mul_g(&p, &k));
        CHECK(pz_ge_on_curve(&p));
        hex32(&p.x, hx);
        hex32(&p.y, hy);
        CHECK_STR(hx, EC_VEC[i].x);
        CHECK_STR(hy, EC_VEC[i].y);
        pz_ge_to_pub33(pub, &p);
        pz_hash160(pub, 33, h);
        tu_hex(h, 20, hh);
        CHECK_STR(hh, EC_VEC[i].h160);
        CHECK(pz_ge_from_pubkey(&q, pub, 33) && pz_ge_equal(&p, &q));
        pz_ge_to_pub65(pub65, &p);
        CHECK(pub65[0] == 4 && !memcmp(pub65 + 1, pub + 1, 32));
        CHECK(pz_ge_from_pubkey(&q, pub65, 65) && pz_ge_equal(&p, &q));
        /* the other parity is the negated point */
        pub[0] ^= 1;
        pz_ge neg;
        pz_ge_neg(&neg, &p);
        CHECK(pz_ge_from_pubkey(&q, pub, 33) && pz_ge_equal(&neg, &q));
        /* a coordinate written as another representative of the same residue is the same point for the arithmetic */
        pz_ge lazy = p;
        pz_fe_add(&lazy.x, &p.x, &pz_fe_p);
        CHECK(pz_ge_equal(&p, &lazy));
    }
    /* k >= n wraps; multiples of n are the point at infinity */
    {
        pz_ge g, a, b;
        pz_u256 n = pz_secp256k1_n, k, zero = pz_u256_zero();
        CHECK(!pz_ec_mul_g(&a, &zero));
        CHECK(!pz_ec_mul_g(&a, &n));
        pz_u256 np1;
        pz_u256_add_u64(&np1, &n, 1);
        CHECK(pz_ec_mul_g(&a, &np1) && pz_ge_equal(&a, &pz_ec_g));
        CHECK(pz_ec_mul_g(&g, &(pz_u256){{1, 0, 0, 0}}) && pz_ge_equal(&g, &pz_ec_g) && pz_ge_on_curve(&pz_ec_g));
        for (int it = 0; it < 50; it++) {
            for (int j = 0; j < 4; j++) k.w[j] = tu_rnd();
            if (pz_u256_cmp(&k, &n) < 0) k.w[3] |= 0xFFFFFFFF00000000ULL;       /* make it at least n in most cases */
            pz_u256 red;
            if (pz_u256_cmp(&k, &n) >= 0) pz_u256_sub(&red, &k, &n); else red = k;
            if (pz_u256_is_zero(&red)) continue;
            CHECK(pz_ec_mul_g(&a, &k) && pz_ec_mul_g(&b, &red) && pz_ge_equal(&a, &b));
        }
        pz_gej j;
        pz_ec_mul_g_j(&j, &n);
        CHECK(j.inf);
        pz_ec_mul_g_j(&j, &zero);
        CHECK(j.inf);
        CHECK(!pz_gej_to_ge(&a, &j));
    }
}

static void test_group_law(void)
{
    for (int it = 0; it < 300; it++) {
        pz_u256 k = pz_u256_zero(), j = pz_u256_zero(), sum, diff;
        for (int i = 0; i < 4; i++) { k.w[i] = tu_rnd(); j.w[i] = tu_rnd(); }
        k.w[3] >>= (tu_rnd() % 64);
        j.w[3] >>= (tu_rnd() % 64);
        pz_u256 kk = k, jj = j;
        if (pz_u256_cmp(&kk, &pz_secp256k1_n) >= 0) pz_u256_sub(&kk, &kk, &pz_secp256k1_n);
        if (pz_u256_cmp(&jj, &pz_secp256k1_n) >= 0) pz_u256_sub(&jj, &jj, &pz_secp256k1_n);
        pz_ge a, b, c, e;
        CHECK(pz_ec_mul_g(&a, &kk) && pz_ec_mul_g(&b, &jj));
        pz_u256_addmod_n(&sum, &kk, &jj);
        pz_u256_submod_n(&diff, &kk, &jj);
        if (!pz_u256_is_zero(&sum)) { CHECK(pz_ec_mul_g(&c, &sum) && pz_ge_add(&e, &a, &b) && pz_ge_equal(&e, &c)); }
        if (!pz_u256_is_zero(&diff)) { CHECK(pz_ec_mul_g(&c, &diff) && pz_ge_sub(&e, &a, &b) && pz_ge_equal(&e, &c)); }
        pz_u256 k2;
        pz_u256_addmod_n(&k2, &kk, &kk);
        CHECK(pz_ge_add(&e, &a, &a) && pz_ec_mul_g(&c, &k2) && pz_ge_equal(&e, &c));            /* doubling through the addition */
        pz_ge na;
        pz_ge_neg(&na, &a);
        CHECK(!pz_ge_add(&e, &a, &na));                                                          /* P + (-P) is the point at infinity */
        CHECK(!pz_ge_sub(&e, &a, &a));
        e = a;
        CHECK(pz_ge_add(&e, &e, &b) && pz_ge_on_curve(&e));                                      /* the output may be an input */
        /* Jacobian forms */
        pz_gej ja, jb, jc;
        pz_gej_set_ge(&ja, &a);
        pz_gej_set_ge(&jb, &b);
        pz_gej_add(&jc, &ja, &jb);
        CHECK(pz_gej_to_ge(&e, &jc));
        pz_ge ab;
        CHECK(pz_ge_add(&ab, &a, &b) && pz_ge_equal(&e, &ab));
        pz_gej_add_ge(&jc, &ja, &b);
        CHECK(pz_gej_to_ge(&e, &jc) && pz_ge_equal(&e, &ab));
        pz_gej_double(&jc, &ja);
        CHECK(pz_gej_to_ge(&e, &jc));
        CHECK(pz_ge_add(&c, &a, &a) && pz_ge_equal(&e, &c));
        pz_gej_add(&jc, &ja, &ja);                                                               /* equal points: the general addition doubles */
        CHECK(pz_gej_to_ge(&e, &jc) && pz_ge_equal(&e, &c));
        pz_gej_add_ge(&jc, &ja, &a);
        CHECK(pz_gej_to_ge(&e, &jc) && pz_ge_equal(&e, &c));
    }
    /* the point at infinity in Jacobian form */
    {
        pz_ge a, na, e;
        pz_u256 k = pz_u256_from_u64(5);
        CHECK(pz_ec_mul_g(&a, &k));
        pz_ge_neg(&na, &a);
        pz_gej ja, jn, inf, r;
        pz_gej_set_ge(&ja, &a);
        pz_gej_set_ge(&jn, &na);
        pz_gej_add(&inf, &ja, &jn);
        CHECK(inf.inf && pz_fe_is_zero(&inf.z) == true && !pz_gej_to_ge(&e, &inf));
        pz_gej_add_ge(&r, &ja, &na);
        CHECK(r.inf);
        pz_gej_add(&r, &inf, &ja);                                                               /* infinity is the neutral element */
        CHECK(!r.inf && pz_gej_to_ge(&e, &r) && pz_ge_equal(&e, &a));
        pz_gej_add(&r, &ja, &inf);
        CHECK(!r.inf && pz_gej_to_ge(&e, &r) && pz_ge_equal(&e, &a));
        pz_gej_add_ge(&r, &inf, &a);
        CHECK(!r.inf && pz_gej_to_ge(&e, &r) && pz_ge_equal(&e, &a));
        pz_gej_double(&r, &inf);
        CHECK(r.inf);
        pz_gej_add(&r, &inf, &inf);
        CHECK(r.inf);
    }
}

static void test_batch(void)
{
    static pz_gej in[5000];
    static pz_ge out[5000], one_by_one[5000];
    pz_ge a;
    pz_u256 k = pz_u256_from_u64(3);
    CHECK(pz_ec_mul_g(&a, &k));
    pz_gej cur;
    pz_gej_set_ge(&cur, &a);
    for (int i = 0; i < 5000; i++) {
        pz_gej_add_ge(&cur, &cur, &pz_ec_g);
        in[i] = cur;
        CHECK(pz_gej_to_ge(&one_by_one[i], &cur));
    }
    const size_t sizes[] = {0, 1, 2, 3, 7, 64, 257, 5000};
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        memset(out, 0, sizeof out);
        CHECK(pz_gej_to_ge_batch(out, in, sizes[s]));
        for (size_t i = 0; i < sizes[s]; i++) CHECK(pz_ge_equal(&out[i], &one_by_one[i]));
    }
    /* the results are canonical */
    CHECK(pz_gej_to_ge_batch(out, in, 100));
    for (size_t i = 0; i < 100; i++) {
        char hx[65];
        pz_u256 v;
        pz_fe_to_u256(&v, &out[i].x);
        CHECK(!memcmp(&v, &out[i].x, sizeof v));                 /* normalizing again changes nothing */
        hex32(&out[i].x, hx);
    }
    /* a point at infinity, or a z that is zero, makes the whole conversion fail */
    in[17].inf = true;
    CHECK(!pz_gej_to_ge_batch(out, in, 100));
    CHECK(pz_gej_to_ge_batch(out, in, 17));                       /* the points before it are fine */
    in[17].inf = false;
    pz_fe_zero(&in[40].z);
    CHECK(!pz_gej_to_ge_batch(out, in, 100));
}

static void test_pubkeys(void)
{
    for (size_t i = 0; i < sizeof PUBKEY_BAD / sizeof *PUBKEY_BAD; i++) {
        uint8_t b[80];
        const int n = tu_unhex(PUBKEY_BAD[i], b, sizeof b);
        CHECK(n >= 0);
        pz_ge r;
        memset(&r, 0x5a, sizeof r);
        pz_ge keep = r;
        CHECK(!pz_ge_from_pubkey(&r, b, (size_t)n));
        CHECK(!memcmp(&r, &keep, sizeof r));                      /* left alone */
    }
    pz_ge r;
    uint8_t b[65] = {2};
    CHECK(!pz_ge_from_pubkey(&r, NULL, 33));
    CHECK(!pz_ge_from_pubkey(&r, NULL, 0));
    CHECK(!pz_ge_from_pubkey(&r, b, 0));
    for (size_t len = 0; len < 70; len++) {
        if (len == 33 || len == 65) continue;
        CHECK(!pz_ge_from_pubkey(&r, b, len));
    }
    /* every prefix byte but 2, 3 and 4 is refused for the right lengths */
    pz_ge g = pz_ec_g;
    uint8_t good33[33], good65[65];
    pz_ge_to_pub33(good33, &g);
    pz_ge_to_pub65(good65, &g);
    for (int p = 0; p < 256; p++) {
        uint8_t t33[33], t65[65];
        memcpy(t33, good33, 33); memcpy(t65, good65, 65);
        t33[0] = (uint8_t)p; t65[0] = (uint8_t)p;
        CHECK(pz_ge_from_pubkey(&r, t33, 33) == (p == 2 || p == 3));
        CHECK(pz_ge_from_pubkey(&r, t65, 65) == (p == 4));
    }
    /* x = 0 has no point: x^3 + 7 = 7 is not a square modulo p */
    memset(b, 0, sizeof b);
    b[0] = 2;
    CHECK(!pz_ge_from_pubkey(&r, b, 33));
}

/* a^((p+1)/4) by plain square-and-multiply: the definition the addition chain must agree with */
static void sqrt_slow(pz_fe *r, const pz_fe *a)
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
    *r = acc;
    pz_fe_normalize(r);
}

static void test_sqrt(void)
{
    int squares = 0, others = 0;
    for (int it = 0; it < 2000; it++) {
        pz_fe a, r, s, chk;
        for (int j = 0; j < 4; j++) a.n[j] = tu_rnd();
        if (it % 5 == 0) { a.n[3] = ~0ULL; a.n[2] = ~0ULL; }                  /* above p: a lazy representative */
        const bool sq = pz_fe_sqrt(&r, &a);
        sqrt_slow(&s, &a);
        CHECK(pz_fe_equiv(&r, &s));                                              /* the same exponent, the same root */
        pz_fe_sqr(&chk, &r);
        CHECK(sq == pz_fe_equiv(&chk, &a));
        if (sq) squares++; else others++;
        pz_fe x2;
        pz_fe_sqr(&x2, &a);                                                      /* a square always has a root */
        CHECK(pz_fe_sqrt(&r, &x2));
        pz_fe_sqr(&chk, &r);
        CHECK(pz_fe_equiv(&chk, &x2));
        pz_fe na;
        pz_fe_neg(&na, &a);
        CHECK(pz_fe_equiv(&r, &a) || pz_fe_equiv(&r, &na));                    /* the root of a square is plus or minus the number */
    }
    CHECK(squares > 800 && others > 800);                                        /* about half of the residues are squares */
    pz_fe zero, one, r;
    pz_fe_zero(&zero);
    pz_fe_set_u64(&one, 1);
    CHECK(pz_fe_sqrt(&r, &zero) && pz_fe_is_zero(&r));
    CHECK(pz_fe_sqrt(&r, &one) && pz_fe_equiv(&r, &one));
    pz_fe m1;
    pz_fe_neg(&m1, &one);
    CHECK(!pz_fe_sqrt(&r, &m1));                                                  /* p = 3 mod 4: -1 is not a square */
}

int main(void)
{
    test_threads();
    test_vectors();
    test_group_law();
    test_batch();
    test_pubkeys();
    test_sqrt();
    TEST_DONE("t_ec");
}
