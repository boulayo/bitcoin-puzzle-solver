/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kara.h - Karatsuba forms of the 8x32 -> 16x32-bit limb product, as measured in docs/en/RESEARCH.md section 8.6. Plain C: used by the
 * kernels of variants.cl (NVRTC dialect, see tools/research/sass_count.py) and by kara_check.c (host check of the arithmetic). */

/* 4x4 -> 8 limbs, schoolbook */
inline void k_mul44(uint t[8], const uint a[4], const uint b[4])
{
#pragma unroll
    for (int i = 0; i < 8; i++) t[i] = 0;
#pragma unroll
    for (int i = 0; i < 4; i++) {
        ulong carry = 0;
        ulong ai = a[i];
#pragma unroll
        for (int j = 0; j < 4; j++) {
            ulong p = ai * (ulong)b[j] + (ulong)t[i + j] + carry;
            t[i + j] = (uint)p;
            carry = p >> 32;
        }
        t[i + 4] = (uint)carry;
    }
}

/* t = z0 + z1 * 2^128 + z2 * 2^256 for z0, z2 of 8 limbs and z1 of 9 */
inline void k_join(uint t[16], const uint z0[8], const uint z1[9], const uint z2[8])
{
#pragma unroll
    for (int i = 0; i < 4; i++) t[i] = z0[i];
    ulong cc = 0;
#pragma unroll
    for (int i = 0; i < 4; i++) { cc += (ulong)z0[4 + i] + z1[i]; t[4 + i] = (uint)cc; cc >>= 32; }
#pragma unroll
    for (int i = 0; i < 4; i++) { cc += (ulong)z2[i] + z1[4 + i]; t[8 + i] = (uint)cc; cc >>= 32; }
    cc += (ulong)z2[4] + z1[8]; t[12] = (uint)cc; cc >>= 32;
#pragma unroll
    for (int i = 5; i < 8; i++) { cc += z2[i]; t[8 + i] = (uint)cc; cc >>= 32; }
}

/* additive form: z1 = (a0 + a1)(b0 + b1) - z0 - z2, the sums having 129 bits (the carries ca, cb are folded in with masks) */
inline void mul512_kara_add(uint t[16], const uint *a, const uint *b)
{
    uint z0[8], z2[8], zm[8], sa[4], sb[4];
    k_mul44(z0, &a[0], &b[0]);
    k_mul44(z2, &a[4], &b[4]);
    ulong c = 0, d = 0;
#pragma unroll
    for (int i = 0; i < 4; i++) {
        c += (ulong)a[i] + a[4 + i]; sa[i] = (uint)c; c >>= 32;
        d += (ulong)b[i] + b[4 + i]; sb[i] = (uint)d; d >>= 32;
    }
    const uint ca = (uint)c, cb = (uint)d;
    k_mul44(zm, sa, sb);
    uint m[9];                                  /* zm + ca*sb*2^128 + cb*sa*2^128 + ca*cb*2^256 */
    ulong cy = 0;
    const uint ma = 0u - ca, mb = 0u - cb;
#pragma unroll
    for (int i = 0; i < 4; i++) m[i] = zm[i];
#pragma unroll
    for (int i = 0; i < 4; i++) {
        cy += (ulong)zm[4 + i] + (sb[i] & ma) + (sa[i] & mb);
        m[4 + i] = (uint)cy; cy >>= 32;
    }
    m[8] = (uint)cy + (ca & cb);
    uint z1[9];
    long bw = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        bw += (long)m[i] - (long)z0[i] - (long)z2[i];
        z1[i] = (uint)bw; bw >>= 32;
    }
    z1[8] = (uint)(m[8] + bw);
    k_join(t, z0, z1, z2);
}

/* |x1 - x0| of two 4-limb halves; returns all ones when x1 < x0 */
inline uint k_absdiff(uint d[4], const uint *x)
{
    long bw = 0;
    uint t4[4];
#pragma unroll
    for (int i = 0; i < 4; i++) { bw += (long)x[4 + i] - (long)x[i]; t4[i] = (uint)bw; bw >>= 32; }
    const uint neg = (uint)bw;
    ulong cy = neg & 1u;
#pragma unroll
    for (int i = 0; i < 4; i++) { cy += (ulong)(t4[i] ^ neg); d[i] = (uint)cy; cy >>= 32; }
    return neg;
}

/* subtractive form: z1 = z0 + z2 - sign * |a1 - a0| * |b1 - b0| (no 129-bit sums) */
inline void mul512_kara_sub(uint t[16], const uint *a, const uint *b)
{
    uint z0[8], z2[8], zm[8], da[4], db[4];
    k_mul44(z0, &a[0], &b[0]);
    k_mul44(z2, &a[4], &b[4]);
    const uint neg = k_absdiff(da, a) ^ k_absdiff(db, b);      /* all ones when (a1 - a0)(b1 - b0) < 0 */
    k_mul44(zm, da, db);
    uint z1[9];
    ulong cy = neg ? 0 : 1;                                     /* z0 + z2 + (neg ? zm : -zm), as two's complement */
#pragma unroll
    for (int i = 0; i < 8; i++) {
        cy += (ulong)z0[i] + z2[i] + (ulong)(zm[i] ^ ~neg);
        z1[i] = (uint)cy; cy >>= 32;
    }
    z1[8] = (uint)cy + ~neg;
    k_join(t, z0, z1, z2);
}
