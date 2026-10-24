/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_fe.c - the (possibly assembly) pz_fe_mul/pz_fe_sqr must agree with the portable C versions and with Python-checked identities. */
#include <pzcore/fe.h>
#include "../src/cpu_engine.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static u64 s = 0x1234ABCD5678EF01ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static void pick(pz_fe *a, int mode)
{
    for (int j = 0; j < 4; j++) a->n[j] = rnd();
    switch (mode % 12) {
    case 0: memset(a, 0, sizeof *a); break;
    case 1: memset(a, 0xff, sizeof *a); break;
    case 2: *a = pz_fe_p; break;
    case 3: a->n[3] = a->n[2] = a->n[1] = ~0ULL; break;
    case 4: a->n[0] = ~0ULL; a->n[1] = a->n[2] = a->n[3] = 0; break;
    case 5: a->n[3] = 1ULL << 63; a->n[0] = a->n[1] = a->n[2] = 0; break;
    case 6: a->n[0] = 1; a->n[1] = a->n[2] = a->n[3] = 0; break;
    case 7: a->n[3] = ~0ULL; a->n[2] = ~0ULL; break;
    case 8: a->n[1] = a->n[3] = 0; break;
    default: break;
    }
}

int main(void)
{
#ifdef PZ_FE_ASM
    if (!cpu_has_avx2()) { printf("t_fe (asm): CPU without BMI2/ADX, skipped\n"); return 0; }
    const char *mode = "asm";
#else
    const char *mode = "C";
#endif
    for (int it = 0; it < 400000; it++) {
        pz_fe a, b, r1, r2, r3;
        pick(&a, it);
        pick(&b, it / 12 + it);
        pz_fe_mul(&r1, &a, &b);
        pz_fe_mul_c(&r2, &a, &b);
        CHECK(pz_fe_equiv(&r1, &r2));
        pz_fe_sqr(&r1, &a);
        pz_fe_sqr_c(&r2, &a);
        CHECK(pz_fe_equiv(&r1, &r2));
        /* aliasing */
        r3 = a;
        pz_fe_mul(&r3, &r3, &b);
        pz_fe_mul_c(&r2, &a, &b);
        CHECK(pz_fe_equiv(&r3, &r2));
        r3 = b;
        pz_fe_mul(&r3, &a, &r3);
        CHECK(pz_fe_equiv(&r3, &r2));
        r3 = a;
        pz_fe_sqr(&r3, &r3);
        pz_fe_sqr_c(&r2, &a);
        CHECK(pz_fe_equiv(&r3, &r2));
        /* algebra: (a*b)/b == a */
        pz_fe bn = b;
        pz_fe_normalize(&bn);
        if (!pz_fe_is_zero(&bn)) {
            pz_fe bi, ab;
            pz_fe_inv(&bi, &b);
            pz_fe_mul(&ab, &a, &b);
            pz_fe_mul(&r1, &ab, &bi);
            CHECK(pz_fe_equiv(&r1, &a));
        }
        /* add / sub / neg against straightforward 256-bit modular arithmetic */
        {
            pz_u256 ua, ub, pp, sum, dif;
            pz_fe_to_u256(&ua, &a);
            pz_fe_to_u256(&ub, &b);
            for (int k = 0; k < 4; k++) pp.w[k] = pz_fe_p.n[k];
            u64 c = pz_u256_add(&sum, &ua, &ub);
            if (c || pz_u256_cmp(&sum, &pp) >= 0) pz_u256_sub(&sum, &sum, &pp);
            if (pz_u256_sub(&dif, &ua, &ub)) pz_u256_add(&dif, &dif, &pp);
            pz_fe fr; pz_u256 got;
            pz_fe_add(&fr, &a, &b); pz_fe_to_u256(&got, &fr); CHECK(!pz_u256_cmp(&got, &sum));
            pz_fe_sub(&fr, &a, &b); pz_fe_to_u256(&got, &fr); CHECK(!pz_u256_cmp(&got, &dif));
            fr = a; pz_fe_add(&fr, &fr, &fr); pz_u256 two; pz_u256_add(&two, &ua, &ua); if (pz_u256_cmp(&two, &ua) < 0 || pz_u256_cmp(&two, &pp) >= 0) pz_u256_sub(&two, &two, &pp);
            pz_fe_to_u256(&got, &fr); CHECK(!pz_u256_cmp(&got, &two));
            pz_fe_neg(&fr, &a); pz_fe_add(&fr, &fr, &a); pz_fe_normalize(&fr); CHECK(pz_fe_is_zero(&fr));
        }
        if (fails > 10) break;
    }
    /* every pair of the special patterns (including the double wrap-around of lazy operands) for add / sub */
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < 12; j++)
            for (int rep = 0; rep < 50; rep++) {
                pz_fe a, b, fr;
                pick(&a, i);
                pick(&b, j);
                pz_u256 ua, ub, pp, sum, dif, got;
                pz_fe_to_u256(&ua, &a);
                pz_fe_to_u256(&ub, &b);
                for (int k = 0; k < 4; k++) pp.w[k] = pz_fe_p.n[k];
                u64 c = pz_u256_add(&sum, &ua, &ub);
                if (c || pz_u256_cmp(&sum, &pp) >= 0) pz_u256_sub(&sum, &sum, &pp);
                if (pz_u256_sub(&dif, &ua, &ub)) pz_u256_add(&dif, &dif, &pp);
                pz_fe_add(&fr, &a, &b); pz_fe_to_u256(&got, &fr); CHECK(!pz_u256_cmp(&got, &sum));
                pz_fe_sub(&fr, &a, &b); pz_fe_to_u256(&got, &fr); CHECK(!pz_u256_cmp(&got, &dif));
                /* chained use of lazily reduced results */
                pz_fe t1, t2;
                pz_fe_add(&t1, &a, &b);
                pz_fe_add(&t2, &t1, &t1);
                pz_fe_sub(&t2, &t2, &a);
                pz_fe_sub(&t2, &t2, &t1);
                pz_fe_to_u256(&got, &t2);
                pz_u256 want;
                pz_fe w = b;
                pz_fe_add_c(&w, &b, &a);   /* reference path: portable versions */
                pz_fe_add_c(&w, &w, &w);
                pz_fe_sub_c(&w, &w, &a);
                pz_fe_sub_c(&w, &w, &t1);
                pz_fe_to_u256(&want, &w);
                CHECK(!pz_u256_cmp(&got, &want));
            }
    printf(fails ? "t_fe (%s): %d FAILED\n" : "t_fe (%s): OK\n", mode, fails);
    return fails != 0;
}
