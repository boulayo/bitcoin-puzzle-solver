/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_perm_pin.c - the permutation of a campaign must never change: a campaign visits its blocks in the order this function
 * defines, so any change of the algorithm would make a resumed campaign skip or repeat blocks. The table holds outputs produced
 * by the implementation of release 1.8.1, for several seeds and domain sizes (power of two or not, one bit to 192 bits). */
#include "../src/perm.h"
#include "perm_pins.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void seed_of(u8 sd[32], int which)
{
    for (int i = 0; i < 32; i++) {
        switch (which) {
        case 0: sd[i] = (u8)(i * 7 + 3); break;
        case 1: sd[i] = 0; break;
        case 2: sd[i] = 0xff; break;
        default: sd[i] = (u8)(i * 131 + 17); break;
        }
    }
}

int main(void)
{
    for (size_t i = 0; i < sizeof PERM_PINS / sizeof *PERM_PINS; i++) {
        const struct pz_perm_pin *pin = &PERM_PINS[i];
        u8 seed[32];
        pz_u256 m, x, y, got;
        seed_of(seed, pin->seed);
        CHECK(pz_u256_from_hex(&m, pin->m) && pz_u256_from_hex(&x, pin->x) && pz_u256_from_hex(&y, pin->y));
        pz_perm_t p;
        pz_perm_init(&p, seed, &m);
        got = pz_perm_apply(&p, &x);
        CHECK(pz_u256_cmp(&got, &y) == 0);
        got = pz_perm_invert(&p, &y);
        CHECK(pz_u256_cmp(&got, &x) == 0);
    }
    printf(fails ? "t_perm_pin: %d FAILED\n" : "t_perm_pin: OK\n", fails);
    return fails != 0;
}
