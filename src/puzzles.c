/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* puzzles.c - lookup and run-time verification of the built-in puzzle table. */
#include "puzzles.h"
#include "ec.h"
#include "hash.h"

const puzzle_info *puzzle_find(int n)
{
    if (n < 1 || n > PZ_NUM_PUZZLES) return NULL;
    const puzzle_info *p = &PZ_PUZZLES[n - 1];
    return p->n == n ? p : NULL;
}

const char *puzzle_retired_address(int n)
{
    if (n < PZ_FIRST_RETIRED || n >= PZ_FIRST_RETIRED + PZ_NUM_RETIRED) return NULL;
    return PZ_RETIRED[n - PZ_FIRST_RETIRED];
}

int puzzles_verify(char *err, size_t errlen)
{
    pz_ec_init();
#define FAIL(n, ...) do { snprintf(err, errlen, __VA_ARGS__); return (n); } while (0)
    for (int i = 0; i < PZ_NUM_PUZZLES; i++) {
        const puzzle_info *p = &PZ_PUZZLES[i];
        if (p->n != i + 1) FAIL(i + 1, "puzzle %d: number out of place in the table", i + 1);
        u8 h[20];
        if (!pz_address_to_hash160(p->address, h)) FAIL(p->n, "puzzle %d: invalid address %s (Base58Check)", p->n, p->address);
        u8 pub[33], hp[20];
        bool have_pub = p->pubkey[0] != 0;
        if (have_pub) {
            if (pz_hex_decode(p->pubkey, pub, 33) != 33 || strlen(p->pubkey) != 66) FAIL(p->n, "puzzle %d: wrong public key format", p->n);
            pz_ge q;
            if (!pz_ge_from_pubkey(&q, pub, 33)) FAIL(p->n, "puzzle %d: the public key is not a point on the curve", p->n);
            pz_hash160(pub, 33, hp);
            if (memcmp(hp, h, 20) != 0) FAIL(p->n, "puzzle %d: the public key does not match the address", p->n);
        }
        if (p->privkey[0]) {
            pz_u256 k, lo = pz_u256_pow2((unsigned)p->n - 1), hi = pz_u256_pow2((unsigned)p->n);
            if (!pz_u256_from_hex(&k, p->privkey)) FAIL(p->n, "puzzle %d: unreadable private key", p->n);
            if (pz_u256_cmp(&k, &lo) < 0 || pz_u256_cmp(&k, &hi) >= 0) FAIL(p->n, "puzzle %d: the private key is outside the puzzle range", p->n);
            pz_ge q;
            u8 pk[33];
            if (!pz_ec_mul_g(&q, &k)) FAIL(p->n, "puzzle %d: the private key is zero", p->n);
            pz_ge_to_pub33(pk, &q);
            pz_hash160(pk, 33, hp);
            if (memcmp(hp, h, 20) != 0) FAIL(p->n, "puzzle %d: the private key does not produce the address", p->n);
            if (have_pub && memcmp(pk, pub, 33) != 0) FAIL(p->n, "puzzle %d: the public key is not k*G", p->n);
        }
    }
    for (int i = 0; i < PZ_NUM_RETIRED; i++) {
        u8 h[20];
        if (!pz_address_to_hash160(PZ_RETIRED[i], h)) FAIL(PZ_FIRST_RETIRED + i, "puzzle %d: invalid address %s (Base58Check)", PZ_FIRST_RETIRED + i, PZ_RETIRED[i]);
    }
#undef FAIL
    return 0;
}
