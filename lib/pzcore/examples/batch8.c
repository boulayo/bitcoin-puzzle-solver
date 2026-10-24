/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* batch8.c - the HASH160 of the public keys of consecutive private keys, many at once with the SIMD code of the CPU. */
#include <pzcore/pzcore.h>
#include <stdio.h>

#define N 4096

int main(void)
{
    static uint8_t xs[N * 32], parity[N], hashes[N * 20];
    pz_ge p;
    pz_u256 k = pz_u256_from_u64(1000);
    pz_ec_mul_g(&p, &k);                                   /* the point of key 1000 ... */
    for (int i = 0; i < N; i++) {                          /* ... and of the next ones, by adding G each time */
        pz_fe_to_be(xs + 32 * i, &p.x);
        pz_fe y = p.y;
        pz_fe_normalize(&y);
        parity[i] = pz_fe_is_odd(&y) ? 1 : 0;
        pz_ge_add(&p, &p, &pz_ec_g);
    }
    printf("implementation: %s, %u keys per step\n", pz_hash160_batch_impl(), pz_hash160_batch_lanes());
    pz_hash160_x_batch(N, xs, parity, hashes);

    /* the same with the scalar code, one key at a time */
    int bad = 0;
    for (int i = 0; i < N; i++) {
        uint8_t want[20];
        pz_hash160_pub33(parity[i], xs + 32 * i, want);
        if (memcmp(want, hashes + 20 * i, 20)) bad++;
    }
    char addr[PZ_P2PKH_ADDRESS_BUF];
    pz_hash160_to_address(hashes, addr, sizeof addr);
    printf("key 1000 -> %s\n", addr);
    printf("%d of %d hashes differ from the scalar code\n", bad, N);
    return bad ? 1 : 0;
}
