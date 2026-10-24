// SPDX-License-Identifier: AGPL-3.0-or-later
// cxx_check.cpp - the public headers compile as C++ and the functions link with C linkage (make lib-cxx).
#include <pzcore/pzcore.h>
#include <cstdio>
#include <cstring>

int main()
{
    pz_u256 k = pz_u256_from_u64(1);
    pz_ge p;
    if (!pz_ec_mul_g(&p, &k)) return 1;
    uint8_t pub[33], h[20];
    pz_ge_to_pub33(pub, &p);
    pz_hash160(pub, sizeof pub, h);
    char addr[PZ_P2PKH_ADDRESS_BUF];
    if (!pz_hash160_to_address(h, addr, sizeof addr) || std::strcmp(addr, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH") != 0) return 2;

    pz_perm perm;
    uint8_t seed[32] = {1};
    pz_u256 m = pz_u256_from_u64(1000), y, z, three = pz_u256_from_u64(3);
    if (!pz_perm_init(&perm, seed, &m) || !pz_perm_apply(&perm, &three, &y) || !pz_perm_invert(&perm, &y, &z) || pz_u256_cmp(&z, &three) != 0) return 4;

    pz_fe a, b;
    pz_fe_set_u64(&a, 7);
    pz_fe_inv(&b, &a);
    pz_fe_mul(&b, &b, &a);
    pz_fe one;
    pz_fe_set_u64(&one, 1);
    if (!pz_fe_equiv(&b, &one)) return 5;
    std::puts("cxx_check: OK");
    return 0;
}
