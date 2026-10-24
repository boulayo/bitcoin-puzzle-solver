/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fe.c - the one field operation that is not inline. */
#include <pzcore/fe.h>

/* a^((p+1)/4), p = 2^256 - 2^32 - 977: (p+1)/4 is 223 ones, a zero, 22 ones, four zeros, two ones and two zeros in binary, which
 * the chain of pz_fe_inv reaches with a few more steps (253 squarings and 13 multiplications instead of 256 and about 128 for a
 * bit-by-bit square-and-multiply). */
bool pz_fe_sqrt(pz_fe *r, const pz_fe *a)
{
    pz_fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    pz_fe_sqr(&x2, a);            pz_fe_mul(&x2, &x2, a);
    pz_fe_sqr(&x3, &x2);          pz_fe_mul(&x3, &x3, a);
    pz_fe_sqr_n(&x6, &x3, 3);     pz_fe_mul(&x6, &x6, &x3);
    pz_fe_sqr_n(&x9, &x6, 3);     pz_fe_mul(&x9, &x9, &x3);
    pz_fe_sqr_n(&x11, &x9, 2);    pz_fe_mul(&x11, &x11, &x2);
    pz_fe_sqr_n(&x22, &x11, 11);  pz_fe_mul(&x22, &x22, &x11);
    pz_fe_sqr_n(&x44, &x22, 22);  pz_fe_mul(&x44, &x44, &x22);
    pz_fe_sqr_n(&x88, &x44, 44);  pz_fe_mul(&x88, &x88, &x44);
    pz_fe_sqr_n(&x176, &x88, 88); pz_fe_mul(&x176, &x176, &x88);
    pz_fe_sqr_n(&x220, &x176, 44); pz_fe_mul(&x220, &x220, &x44);
    pz_fe_sqr_n(&x223, &x220, 3); pz_fe_mul(&x223, &x223, &x3);
    pz_fe_sqr_n(&t, &x223, 23);   pz_fe_mul(&t, &t, &x22);
    pz_fe_sqr_n(&t, &t, 6);       pz_fe_mul(&t, &t, &x2);
    pz_fe_sqr(&t, &t);
    pz_fe_sqr(&t, &t);
    pz_fe chk;
    pz_fe_sqr(&chk, &t);
    *r = t;
    pz_fe_normalize(r);
    return pz_fe_equiv(&chk, a);
}
