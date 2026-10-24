/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <pzcore/perm.h>
#include <assert.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_domain(const u8 *seed, u64 m)
{
    pz_perm_t p; pz_u256 M = pz_u256_from_u64(m);
    pz_perm_init(&p, seed, &M);
    u8 *seen = calloc(m, 1);
    u64 fixed = 0, adjacent = 0;
    for (u64 i = 0; i < m; i++) {
        pz_u256 x = pz_u256_from_u64(i), y = pz_perm_apply(&p, &x);
        CHECK(y.w[1] == 0 && y.w[0] < m);
        if (y.w[0] >= m) continue;
        CHECK(!seen[y.w[0]]);
        seen[y.w[0]] = 1;
        pz_u256 z = pz_perm_invert(&p, &y);
        CHECK(pz_u256_cmp(&z, &x) == 0);
        if (y.w[0] == i) fixed++;
        if (i && y.w[0] + 1 == pz_u256_from_u64(0).w[0]) adjacent++;
    }
    for (u64 i = 0; i < m; i++) CHECK(seen[i]);
    if (m >= 1000) CHECK(fixed < m / 50 + 5);
    free(seen);
    (void)adjacent;
}

int main(void)
{
    u8 seed[32]; for (int i = 0; i < 32; i++) seed[i] = (u8)(i * 7 + 3);
    u64 sizes[] = {1, 2, 3, 4, 5, 7, 8, 16, 100, 1000, 1024, 4096, 5000, 65536, 100003};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) test_domain(seed, sizes[i]);
    seed[0] ^= 1;
    test_domain(seed, 10000);
    /* big domains: round trip + range */
    for (unsigned bits = 20; bits <= 150; bits += 13) {
        pz_perm_t p; pz_u256 M = pz_u256_pow2(bits);
        pz_perm_init(&p, seed, &M);
        for (int t = 0; t < 200; t++) {
            pz_u256 x = pz_u256_zero();
            for (int j = 0; j < 4; j++) x.w[j] = (u64)rand() * 6364136223846793005ULL + (u64)t * 0x9e3779b97f4a7c15ULL + (u64)j;
            pz_u256 mask = pz_u256_shl(&(pz_u256){{1,0,0,0}}, bits); pz_u256_sub(&mask, &mask, &(pz_u256){{1,0,0,0}});
            for (int j = 0; j < 4; j++) x.w[j] &= mask.w[j];
            pz_u256 y = pz_perm_apply(&p, &x), z = pz_perm_invert(&p, &y);
            CHECK(pz_u256_cmp(&y, &M) < 0);
            CHECK(pz_u256_cmp(&z, &x) == 0);
        }
    }
    /* non power-of-two big domain (cycle walking) */
    {
        pz_perm_t p; pz_u256 M = pz_u256_pow2(100); pz_u256_add_u64(&M, &M, 12345);
        pz_perm_init(&p, seed, &M);
        for (int t = 0; t < 200; t++) {
            pz_u256 x = pz_u256_from_u128(((u128)rand() << 64 | (u64)rand() * 0x9e3779b97f4a7c15ULL) >> 29);
            pz_u256 y = pz_perm_apply(&p, &x), z = pz_perm_invert(&p, &y);
            CHECK(pz_u256_cmp(&y, &M) < 0);
            CHECK(pz_u256_cmp(&z, &x) == 0);
        }
    }
    /* different seeds give different orders */
    {
        pz_perm_t a, b; pz_u256 M = pz_u256_pow2(60); u8 s2[32]; memcpy(s2, seed, 32); s2[31] ^= 0x80;
        pz_perm_init(&a, seed, &M); pz_perm_init(&b, s2, &M);
        int same = 0;
        for (int i = 0; i < 1000; i++) { pz_u256 x = pz_u256_from_u64((u64)i); pz_u256 ya = pz_perm_apply(&a, &x), yb = pz_perm_apply(&b, &x); if (!pz_u256_cmp(&ya, &yb)) same++; }
        CHECK(same == 0);
    }
    printf(fails ? "t_perm: %d FAILED\n" : "t_perm: OK\n", fails);
    return fails != 0;
}
