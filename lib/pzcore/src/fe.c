/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fe.c - the one field operation that is not inline. */
#include <pzcore/fe.h>

/* a^((p+1)/4) by square-and-multiply over the 254 bits of the exponent */
bool pz_fe_sqrt(pz_fe *r, const pz_fe *a)
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
    pz_fe chk;
    pz_fe_sqr(&chk, &acc);
    *r = acc;
    pz_fe_normalize(r);
    return pz_fe_equiv(&chk, a);
}
