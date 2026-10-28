/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* chain.h - 8x8-limb product by rows with one carry chain per row: a mad.wide per limb product (the limb of the row before as a
 * 64-bit addend) and a single add chain of the low and high halves (PTX add.cc/addc.cc through the ADD_CC macros of pz_cuda.h).
 * Measured in docs/en/RESEARCH.md section 8.6: -4% instructions on the bare product, nothing on the whole multiplication. */
inline void mul512_chain(uint t[16], const uint *a, const uint *b)
{
    {   /* row 0: nothing to add */
        const ulong p0 = (ulong)a[0] * b[0], p1 = (ulong)a[0] * b[1], p2 = (ulong)a[0] * b[2], p3 = (ulong)a[0] * b[3];
        const ulong p4 = (ulong)a[0] * b[4], p5 = (ulong)a[0] * b[5], p6 = (ulong)a[0] * b[6], p7 = (ulong)a[0] * b[7];
        t[0] = (uint)p0;
        ADD_CC(t[1], (uint)p1, (uint)(p0 >> 32)); ADDC_CC(t[2], (uint)p2, (uint)(p1 >> 32)); ADDC_CC(t[3], (uint)p3, (uint)(p2 >> 32));
        ADDC_CC(t[4], (uint)p4, (uint)(p3 >> 32)); ADDC_CC(t[5], (uint)p5, (uint)(p4 >> 32)); ADDC_CC(t[6], (uint)p6, (uint)(p5 >> 32));
        ADDC_CC(t[7], (uint)p7, (uint)(p6 >> 32)); ADDC(t[8], (uint)(p7 >> 32), 0u);
    }
#pragma unroll
    for (int i = 1; i < 8; i++) {
        const ulong ai = a[i];
        const ulong p0 = ai * b[0] + t[i + 0], p1 = ai * b[1] + t[i + 1], p2 = ai * b[2] + t[i + 2], p3 = ai * b[3] + t[i + 3];
        const ulong p4 = ai * b[4] + t[i + 4], p5 = ai * b[5] + t[i + 5], p6 = ai * b[6] + t[i + 6], p7 = ai * b[7] + t[i + 7];
        t[i] = (uint)p0;
        ADD_CC(t[i + 1], (uint)p1, (uint)(p0 >> 32)); ADDC_CC(t[i + 2], (uint)p2, (uint)(p1 >> 32)); ADDC_CC(t[i + 3], (uint)p3, (uint)(p2 >> 32));
        ADDC_CC(t[i + 4], (uint)p4, (uint)(p3 >> 32)); ADDC_CC(t[i + 5], (uint)p5, (uint)(p4 >> 32)); ADDC_CC(t[i + 6], (uint)p6, (uint)(p5 >> 32));
        ADDC_CC(t[i + 7], (uint)p7, (uint)(p6 >> 32)); ADDC(t[i + 8], (uint)(p7 >> 32), 0u);
    }
}
