/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_test.cl - kernels used by the start-up self-test to compare the device
 * arithmetic with the host implementation. */

/* in: per item a[8] b[8]; out: per item 8 results x 8 limbs */
__kernel void pz_test_fe(__global const uint *in, __global uint *out, const uint pz_n)
{
    uint i = (uint)get_global_id(0);
    PZ_GUARD(i);
    fe a, b, r;
    fe_load_g(&a, in + i * 16);
    fe_load_g(&b, in + i * 16 + 8);
    __global uint *o = out + i * 64;
    fe_mul(&r, &a, &b);  fe_normalize(&r); fe_store_g(o, &r);
    fe_sqr(&r, &a);      fe_normalize(&r); fe_store_g(o + 8, &r);
    fe_add(&r, &a, &b);  fe_normalize(&r); fe_store_g(o + 16, &r);
    fe_sub(&r, &a, &b);  fe_normalize(&r); fe_store_g(o + 24, &r);
    fe_inv(&r, &a);      fe_normalize(&r); fe_store_g(o + 32, &r);
    r = a;               fe_normalize(&r); fe_store_g(o + 40, &r);
    fe_sub(&r, &b, &a);  fe_normalize(&r); fe_store_g(o + 48, &r);
    fe_add(&r, &a, &a);  fe_normalize(&r); fe_store_g(o + 56, &r);
}

/* in: per item x[8] (limbs) + parity ; out: 5 hash words */
__kernel void pz_test_hash(__global const uint *in, __global uint *out, const uint pz_n)
{
    uint i = (uint)get_global_id(0);
    PZ_GUARD(i);
    fe x;
    fe_load_g(&x, in + i * 9);
    uint h[5];
    hash160_words(&x, in[i * 9 + 8] & 1u, h);
    for (int k = 0; k < 5; k++) out[i * 5 + k] = h[k];
}

/* in: per item a.x a.y D.x D.y t ; out: result x y (or zeros if infinity) */
__kernel void pz_test_ec(__global const uint *in, __global uint *out, const uint pz_n)
{
    uint i = (uint)get_global_id(0);
    PZ_GUARD(i);
    ge a, d, r;
    fe_load_g(&a.x, in + i * 33);
    fe_load_g(&a.y, in + i * 33 + 8);
    fe_load_g(&d.x, in + i * 33 + 16);
    fe_load_g(&d.y, in + i * 33 + 24);
    uint t = in[i * 33 + 32];
    int ok = ge_add_mul_small(&r, &a, &d, t);
    if (!ok) { for (int k = 0; k < 16; k++) out[i * 17 + k] = 0; out[i * 17 + 16] = 0; return; }
    fe_store_g(out + i * 17, &r.x);
    fe_store_g(out + i * 17 + 8, &r.y);
    out[i * 17 + 16] = 1;
}

/* ---- throughput probes of 'pzsolve bench' (not used by the search) ---- */

/* Copies `wpi` words per work-item, coalesced (word k of item i is at k*pz_n + i): the traffic pattern of the kangaroo
 * state, 4 bytes read and 4 written per word. */
__kernel void pz_diag_bw(__global const uint *src, __global uint *dst, const uint wpi, const uint pz_n)
{
    uint i = (uint)get_global_id(0);
    PZ_GUARD(i);
    uint k = 0;
    for (; k + 4 <= wpi; k += 4) {
        uint a = src[(k + 0) * pz_n + i], b = src[(k + 1) * pz_n + i], c = src[(k + 2) * pz_n + i], d = src[(k + 3) * pz_n + i];
        dst[(k + 0) * pz_n + i] = a ^ k;
        dst[(k + 1) * pz_n + i] = b ^ k;
        dst[(k + 2) * pz_n + i] = c ^ k;
        dst[(k + 3) * pz_n + i] = d ^ k;
    }
    for (; k < wpi; k++) dst[k * pz_n + i] = src[k * pz_n + i] ^ k;
}

/* Eight independent chains of one 32-bit integer instruction per loop turn: mode 0 a*m+b (mad.lo), 1 mul_hi(a,m)+b (mad.hi),
 * 2 a+b+m (three-input add). The result goes to memory so that nothing is optimised away. */
__kernel void pz_diag_alu(__global uint *out, const uint mode, const uint iters, const uint pz_n)
{
    uint i = (uint)get_global_id(0);
    PZ_GUARD(i);
    uint m = (i * 2654435761u) | 1u;
    uint a0 = i, a1 = i + 1, a2 = i + 2, a3 = i + 3, a4 = i + 4, a5 = i + 5, a6 = i + 6, a7 = i + 7;
    if (mode == 0) {
        for (uint k = 0; k < iters; k++) {
            a0 = a0 * m + a1; a1 = a1 * m + a2; a2 = a2 * m + a3; a3 = a3 * m + a4;
            a4 = a4 * m + a5; a5 = a5 * m + a6; a6 = a6 * m + a7; a7 = a7 * m + a0;
        }
    } else if (mode == 1) {
        for (uint k = 0; k < iters; k++) {
            a0 = mul_hi(a0, m) + a1; a1 = mul_hi(a1, m) + a2; a2 = mul_hi(a2, m) + a3; a3 = mul_hi(a3, m) + a4;
            a4 = mul_hi(a4, m) + a5; a5 = mul_hi(a5, m) + a6; a6 = mul_hi(a6, m) + a7; a7 = mul_hi(a7, m) + a0;
        }
    } else {
        for (uint k = 0; k < iters; k++) {
            a0 = a0 + a1 + m; a1 = a1 + a2 + m; a2 = a2 + a3 + m; a3 = a3 + a4 + m;
            a4 = a4 + a5 + m; a5 = a5 + a6 + m; a6 = a6 + a7 + m; a7 = a7 + a0 + m;
        }
    }
    out[i] = a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}
