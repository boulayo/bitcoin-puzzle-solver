/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* permute.c - the keyed permutation: visit the numbers 0..m-1 in an order that depends on a seed, and go back from a position to the
 * number. usage: permute [m] [seed-byte]   (default m = 20, seed-byte = 1) */
#include <pzcore/pzcore.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const unsigned long m = argc > 1 ? strtoul(argv[1], NULL, 10) : 20;
    uint8_t seed[32];
    memset(seed, argc > 2 ? atoi(argv[2]) : 1, sizeof seed);
    if (m < 1 || m > 1000000) { fprintf(stderr, "m must be between 1 and 1000000\n"); return 2; }

    pz_u256 domain = pz_u256_from_u64(m);
    pz_perm p;
    if (!pz_perm_init(&p, seed, &domain)) { fprintf(stderr, "domain not supported\n"); return 2; }

    unsigned char *seen = calloc(m, 1);
    for (unsigned long i = 0; i < m; i++) {
        pz_u256 x = pz_u256_from_u64(i), y, z;
        if (!pz_perm_apply(&p, &x, &y) || !pz_perm_invert(&p, &y, &z) || z.w[0] != i || y.w[0] >= m || seen[y.w[0]]) {
            fprintf(stderr, "not a permutation at %lu\n", i);
            return 1;
        }
        seen[y.w[0]] = 1;
        if (i < 30) printf("%s%llu", i ? " " : "visit order: ", (unsigned long long)y.w[0]);
    }
    printf("%s\nall %lu numbers visited exactly once\n", m > 30 ? " ..." : "", m);
    free(seen);

    /* a domain of 2^100 numbers needs no table: any position can be asked directly */
    pz_u256 big = pz_u256_pow2(100), pos = pz_u256_pow2(70), at;
    pz_perm_init(&p, seed, &big);
    pz_perm_apply(&p, &pos, &at);
    char a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF];
    printf("in 2^100 numbers, position 0x%s holds 0x%s\n", pz_u256_to_hex(&pos, a), pz_u256_to_hex(&at, b));
    return 0;
}
