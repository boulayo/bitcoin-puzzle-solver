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
        const struct perm_pin *pin = &PERM_PINS[i];
        u8 seed[32];
        u256 m, x, y, got;
        seed_of(seed, pin->seed);
        CHECK(u256_from_hex(&m, pin->m) && u256_from_hex(&x, pin->x) && u256_from_hex(&y, pin->y));
        perm_t p;
        perm_init(&p, seed, &m);
        got = perm_apply(&p, &x);
        CHECK(u256_cmp(&got, &y) == 0);
        got = perm_invert(&p, &y);
        CHECK(u256_cmp(&got, &x) == 0);
    }
    printf(fails ? "t_perm_pin: %d FAILED\n" : "t_perm_pin: OK\n", fails);
    return fails != 0;
}
