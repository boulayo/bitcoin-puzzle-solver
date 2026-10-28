/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* variants.cl - kernels for tools/research/sass_count.py: the bare limb product and the whole field multiplication/square with the
 * production reduction, in the forms of kara.h and chain.h, plus a kernel with the loads and stores alone (k_none) whose
 * instruction count is the overhead to subtract. Compile as: tools/research/sass_count.py ... tools/research/sass/kara.h tools/research/sass/chain.h tools/research/sass/variants.cl */
#define LOAD2(a, b) for (int j = 0; j < 8; j++) { (a).v[j] = in[j * pz_n + i]; (b).v[j] = in[(8 + j) * pz_n + i]; }
#define STORE(r) for (int j = 0; j < 8; j++) out[j * pz_n + i] = (r).v[j]

__kernel void k_none(__global uint *out, __global const uint *in, const uint pz_n)
{
    uint i = (uint)get_global_id(0); PZ_GUARD(i);
    fe a, b, r; LOAD2(a, b);
    for (int j = 0; j < 8; j++) r.v[j] = a.v[j] ^ b.v[j];
    STORE(r);
}
__kernel void k_mul(__global uint *out, __global const uint *in, const uint pz_n)
{
    uint i = (uint)get_global_id(0); PZ_GUARD(i);
    fe a, b, r; LOAD2(a, b);
    fe_mul(&r, &a, &b);
    STORE(r);
}
__kernel void k_sqr(__global uint *out, __global const uint *in, const uint pz_n)
{
    uint i = (uint)get_global_id(0); PZ_GUARD(i);
    fe a, b, r; LOAD2(a, b);
    fe_sqr(&r, &a);
    STORE(r);
}
#define PRODUCT_KERNEL(name, call)                                                       \
__kernel void name(__global uint *out, __global const uint *in, const uint pz_n)         \
{                                                                                         \
    uint i = (uint)get_global_id(0); PZ_GUARD(i);                                         \
    fe a, b; uint t[16]; LOAD2(a, b);                                                     \
    call;                                                                                 \
    for (int j = 0; j < 8; j++) out[j * pz_n + i] = t[j] ^ t[8 + j];                      \
}
PRODUCT_KERNEL(k_prod_kara_add, mul512_kara_add(t, a.v, b.v))
PRODUCT_KERNEL(k_prod_kara_sub, mul512_kara_sub(t, a.v, b.v))
PRODUCT_KERNEL(k_prod_chain, mul512_chain(t, a.v, b.v))
__kernel void k_prod_school(__global uint *out, __global const uint *in, const uint pz_n)   /* the product of fe_mul, without the reduction */
{
    uint i = (uint)get_global_id(0); PZ_GUARD(i);
    fe a, b; uint t[16]; LOAD2(a, b);
    for (int q = 0; q < 16; q++) t[q] = 0;
    for (int ii = 0; ii < 8; ii++) {
        ulong carry = 0; ulong ai = a.v[ii];
        for (int j = 0; j < 8; j++) { ulong p = ai * (ulong)b.v[j] + (ulong)t[ii + j] + carry; t[ii + j] = (uint)p; carry = p >> 32; }
        t[ii + 8] = (uint)carry;
    }
    for (int j = 0; j < 8; j++) out[j * pz_n + i] = t[j] ^ t[8 + j];
}
