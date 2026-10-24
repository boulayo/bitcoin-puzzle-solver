/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_perm.c - the keyed permutation: bijection, inverse, edge cases of the domain, refusal of bad arguments, and the fixed outputs that
 * every version of the library must reproduce (a stored order stays valid only if they never change). */
#include <pzcore/perm.h>
#include "perm_pins.h"
#include "testutil.h"

static void seed_of(uint8_t sd[32], int which)
{
    for (int i = 0; i < 32; i++) {
        switch (which) {
        case 0: sd[i] = (uint8_t)(i * 7 + 3); break;
        case 1: sd[i] = 0; break;
        case 2: sd[i] = 0xff; break;
        default: sd[i] = (uint8_t)(i * 131 + 17); break;
        }
    }
}

/* brute force over a small domain: a bijection onto [0, m), invert undoes apply, few fixed points */
static void test_domain(const uint8_t *seed, uint64_t m)
{
    pz_perm p;
    pz_u256 M = pz_u256_from_u64(m);
    CHECK(pz_perm_init(&p, seed, &M));
    uint8_t *seen = calloc(m, 1);
    uint64_t fixed = 0;
    for (uint64_t i = 0; i < m; i++) {
        pz_u256 x = pz_u256_from_u64(i), y, z;
        CHECK(pz_perm_apply(&p, &x, &y));
        CHECK(y.w[1] == 0 && y.w[0] < m);
        if (y.w[0] >= m) continue;
        CHECK(!seen[y.w[0]]);
        seen[y.w[0]] = 1;
        CHECK(pz_perm_invert(&p, &y, &z) && pz_u256_cmp(&z, &x) == 0);
        if (y.w[0] == i) fixed++;
    }
    for (uint64_t i = 0; i < m; i++) CHECK(seen[i]);
    if (m >= 1000) CHECK(fixed < m / 50 + 5);
    free(seen);
}

static void test_pins(void)
{
    for (size_t i = 0; i < sizeof PERM_PINS / sizeof *PERM_PINS; i++) {
        const struct perm_pin *pin = &PERM_PINS[i];
        uint8_t seed[32];
        pz_u256 m, x, y, got;
        seed_of(seed, pin->seed);
        CHECK(pz_u256_from_hex(&m, pin->m) && pz_u256_from_hex(&x, pin->x) && pz_u256_from_hex(&y, pin->y));
        pz_perm p;
        CHECK(pz_perm_init(&p, seed, &m));
        CHECK(pz_perm_apply(&p, &x, &got) && pz_u256_cmp(&got, &y) == 0);
        CHECK(pz_perm_invert(&p, &y, &got) && pz_u256_cmp(&got, &x) == 0);
    }
}

int main(void)
{
    uint8_t seed[32];
    seed_of(seed, 0);
    const uint64_t sizes[] = {1, 2, 3, 4, 5, 7, 8, 16, 100, 1000, 1024, 4096, 5000, 65536, 100003};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) test_domain(seed, sizes[i]);
    seed[0] ^= 1;
    test_domain(seed, 10000);
    seed_of(seed, 0);

    /* big domains of 2^bits: round trip and range */
    for (unsigned bits = 20; bits <= 190; bits += 13) {
        pz_perm p;
        pz_u256 M = pz_u256_pow2(bits);
        CHECK(pz_perm_init(&p, seed, &M));
        for (int t = 0; t < 200; t++) {
            pz_u256 x, y, z;
            for (int j = 0; j < 4; j++) x.w[j] = tu_rnd();
            pz_u256 mask = pz_u256_pow2(bits), one = pz_u256_from_u64(1);
            pz_u256_sub(&mask, &mask, &one);
            for (int j = 0; j < 4; j++) x.w[j] &= mask.w[j];
            CHECK(pz_perm_apply(&p, &x, &y) && pz_perm_invert(&p, &y, &z));
            CHECK(pz_u256_cmp(&y, &M) < 0);
            CHECK(pz_u256_cmp(&z, &x) == 0);
        }
    }
    /* big domain that is not a power of two (cycle walking) */
    {
        pz_perm p;
        pz_u256 M = pz_u256_pow2(100);
        pz_u256_add_u64(&M, &M, 12345);
        CHECK(pz_perm_init(&p, seed, &M));
        for (int t = 0; t < 200; t++) {
            pz_u256 x = pz_u256_from_u128(((pz_u128)tu_rnd() << 64 | tu_rnd()) >> 29), y, z;
            CHECK(pz_perm_apply(&p, &x, &y) && pz_perm_invert(&p, &y, &z));
            CHECK(pz_u256_cmp(&y, &M) < 0);
            CHECK(pz_u256_cmp(&z, &x) == 0);
        }
    }
    /* different seeds give different orders */
    {
        pz_perm a, b;
        pz_u256 M = pz_u256_pow2(60);
        uint8_t s2[32];
        memcpy(s2, seed, 32);
        s2[31] ^= 0x80;
        CHECK(pz_perm_init(&a, seed, &M) && pz_perm_init(&b, s2, &M));
        int same = 0;
        for (int i = 0; i < 1000; i++) {
            pz_u256 x = pz_u256_from_u64((uint64_t)i), ya, yb;
            CHECK(pz_perm_apply(&a, &x, &ya) && pz_perm_apply(&b, &x, &yb));
            if (!pz_u256_cmp(&ya, &yb)) same++;
        }
        CHECK(same == 0);
    }
    /* limits of the domain, and refusal of arguments outside it */
    {
        pz_perm p;
        pz_u256 zero = pz_u256_zero(), M = pz_u256_pow2(192), y, keep;
        CHECK(!pz_perm_init(&p, seed, &zero));                       /* empty domain */
        CHECK(pz_perm_init(&p, seed, &M));                           /* 2^192 is the largest */
        pz_u256_add_u64(&M, &M, 1);
        CHECK(!pz_perm_init(&p, seed, &M));                          /* 2^192 + 1 is not */
        pz_u256 big = pz_u256_pow2(255);
        CHECK(!pz_perm_init(&p, seed, &big));

        pz_u256 m = pz_u256_from_u64(1000), x = pz_u256_from_u64(1000);
        CHECK(pz_perm_init(&p, seed, &m));
        y = keep = pz_u256_from_u64(77);
        CHECK(!pz_perm_apply(&p, &x, &y) && pz_u256_cmp(&y, &keep) == 0);      /* x == m: refused, output untouched */
        CHECK(!pz_perm_invert(&p, &x, &y) && pz_u256_cmp(&y, &keep) == 0);
        x = pz_u256_pow2(200);
        CHECK(!pz_perm_apply(&p, &x, &y) && !pz_perm_invert(&p, &x, &y));
        x = pz_u256_from_u64(999);
        CHECK(pz_perm_apply(&p, &x, &y));                           /* m - 1 is fine */
        /* the output may be the input */
        pz_u256 v = pz_u256_from_u64(123), want;
        CHECK(pz_perm_apply(&p, &v, &want));
        CHECK(pz_perm_apply(&p, &v, &v) && pz_u256_cmp(&v, &want) == 0);
        CHECK(pz_perm_invert(&p, &v, &v) && v.w[0] == 123);
    }
    /* the two-element domain uses the first 28 bytes of the seed only (part of the definition) */
    {
        pz_perm a, b;
        pz_u256 two = pz_u256_from_u64(2), zero = pz_u256_zero(), ya, yb;
        uint8_t s2[32];
        for (int k = 0; k < 8; k++) {
            seed_of(s2, 0);
            s2[0] = (uint8_t)k;
            uint8_t s3[32];
            memcpy(s3, s2, 32);
            s3[28] ^= 0x55; s3[31] ^= 0xff;
            CHECK(pz_perm_init(&a, s2, &two) && pz_perm_init(&b, s3, &two));
            CHECK(pz_perm_apply(&a, &zero, &ya) && pz_perm_apply(&b, &zero, &yb) && pz_u256_cmp(&ya, &yb) == 0);
        }
    }
    test_pins();
    TEST_DONE("t_perm");
}
