/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fev.h - lane-parallel arithmetic in GF(p), p = 2^256 - 2^32 - 977 (secp256k1).
 *
 * The includer defines VBYTES (32 for AVX2, 64 for AVX-512) before including and compiles with the matching -m flags.
 * One vector lane holds one independent field element, spread over 9 limbs of 29 bits ("structure of arrays"):
 * the 32x32->64 multiplier (vpmuludq) then does 4 or 8 products per instruction, and a product column of up to 9
 * terms of at most 1.03 * 2^60 (loose operands) still fits in a 64-bit lane (9 * FEV_LOOSE^2 < 2^63.3).
 *
 * Limb bounds used throughout:
 *   tight  : limbs <= FEV_TIGHT = 2^29 + 2^23   (result of fev_mul, fev_sqr, fev_sub, fev_sub2, fev_normalize)
 *   loose  : limbs <= FEV_LOOSE = 2 * FEV_TIGHT (sum of two tight values; the largest operand fev_mul / fev_sqr accept:
 *            nine products of two such limbs still fit in 64 bits)
 * fev_sub / fev_sub2 add a multiple of p whose limbs dominate the subtrahend and finish with a carry pass, so
 * their result is tight. fev_add only adds limbs (tight + tight = loose). */
#ifndef PZ_FEV_H
#define PZ_FEV_H

#include "fe.h"
#include <immintrin.h>

#ifndef VBYTES
#error "define VBYTES before including fev.h"
#endif

#define FEV_LANES (VBYTES / 8)
#define FEV_NLIMB 9
typedef u64 vl __attribute__((vector_size(VBYTES)));
typedef struct { vl l[9]; } fev;

#define FEV_TIGHT (((u64)1 << 29) + ((u64)1 << 23))
#define FEV_LOOSE (2 * FEV_TIGHT)
#define FEV_BITS 29
#define FEV_TOPBITS 24                  /* bits of limb 8 in a canonical element (8 * 29 + 24 = 256) */
#define FEV_MASK (((u64)1 << FEV_BITS) - 1)
/* 2^261 = 32 * (2^32 + 977) mod p = FEV_F0 + 256 * 2^29 */
#define FEV_F0 0x7A20ULL

/* Multiples of p written with limbs that dominate a tight (K1) or the sum of two loose (K2) subtrahend; checked by t_fev. */
static const u64 FEV_K1[9] PZ_UNUSED = {0x5fff07efULL, 0x5ffffdf5ULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x40fffffdULL};
static const u64 FEV_K2[9] PZ_UNUSED = {0x11ffc2b2fULL, 0x11ffff7efULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x100fffff7ULL};

static inline PZ_VABI vl vl_set(u64 x)
{
    vl r = {0};
    for (int i = 0; i < FEV_LANES; i++) r[i] = x;
    return r;
}

/* low 32 bits of every lane multiplied pairwise (vpmuludq) */
static inline PZ_VABI vl vl_mul32(vl a, vl b)
{
#if VBYTES == 64
    return (vl)_mm512_mul_epu32((__m512i)a, (__m512i)b);
#else
    return (vl)_mm256_mul_epu32((__m256i)a, (__m256i)b);
#endif
}

/* c * FEV_F0 for c < 2^40 */
static inline PZ_VABI vl vl_mulf0(vl c)
{
    const vl f = vl_set(FEV_F0);
    return vl_mul32(c, f) + (vl_mul32(c >> 32, f) << 32);
}

/* ---- conversion from / to the scalar representation ---- */

static inline PZ_VABI void fev_set_lane(fev *r, int lane, const fe *x)
{
    fe t = *x;
    fe_normalize(&t);
    u128 lo = (u128)t.n[1] << 64 | t.n[0], hi = (u128)t.n[3] << 64 | t.n[2];
    for (int i = 0; i < 9; i++) {
        int bit = 29 * i;
        u64 v;
        if (bit + 29 <= 128) v = (u64)(lo >> bit) & FEV_MASK;
        else if (bit >= 128) v = (u64)(hi >> (bit - 128)) & FEV_MASK;
        else v = (u64)((lo >> bit) | (hi << (128 - bit))) & FEV_MASK;
        r->l[i][lane] = v;
    }
}

/* canonical lane value back to the scalar form */
static inline PZ_VABI void fev_get_lane(fe *x, const fev *v, int lane)
{
    u128 acc = 0;
    int bits = 0, w = 0;
    u64 out[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 9; i++) {
        acc |= (u128)v->l[i][lane] << bits;
        bits += 29;
        while (bits >= 64) { out[w++] = (u64)acc; acc >>= 64; bits -= 64; }
    }
    out[w] = (u64)acc;
    x->n[0] = out[0]; x->n[1] = out[1]; x->n[2] = out[2]; x->n[3] = out[3];
}

/* ---- carry handling ---- */

/* Brings every limb to <= 2^29 + 2^17. Valid for limbs below 2^63.5; the carry out of limb 8 (weight 2^261) is folded back. */
static inline PZ_VABI void fev_carry(fev *r)
{
    const vl M = vl_set(FEV_MASK);
    vl c = vl_set(0);
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
        vl t = r->l[i] + c;
        c = t >> FEV_BITS;
        r->l[i] = t & M;
    }
    vl t0 = r->l[0] + vl_mulf0(c);
    vl t1 = r->l[1] + (c << 8);
    vl c0 = t0 >> FEV_BITS;
    r->l[0] = t0 & M;
    t1 += c0;
    vl c1 = t1 >> FEV_BITS;
    r->l[1] = t1 & M;
    r->l[2] += c1;
}

/* One parallel carry pass for limbs below 2^37 (the results of the additions/subtractions below): every limb keeps its
 * low 29 bits and receives the carry of its lower neighbour, the carry out of limb 8 is folded back. */
static inline PZ_VABI void fev_carry_small(fev *r)
{
    const vl M = vl_set(FEV_MASK);
    vl c[9];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) c[i] = r->l[i] >> FEV_BITS;
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] &= M;
    r->l[0] += vl_mul32(c[8], vl_set(FEV_F0));
    r->l[1] += c[0] + (c[8] << 8);
#pragma GCC unroll 7
    for (int i = 2; i < 9; i++) r->l[i] += c[i - 1];
}

/* ---- arithmetic ---- */

static inline PZ_VABI void fev_add(fev *r, const fev *a, const fev *b)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + b->l[i];
}

/* r = a - b; b tight, a tight or loose. Result tight. */
static inline PZ_VABI void fev_sub(fev *r, const fev *a, const fev *b)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + vl_set(FEV_K1[i]) - b->l[i];
    fev_carry_small(r);
}

/* r = a - b - c; a tight or loose, b and c tight or loose. Result tight. */
static inline PZ_VABI void fev_sub2(fev *r, const fev *a, const fev *b, const fev *c)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + vl_set(FEV_K2[i]) - b->l[i] - c->l[i];
    fev_carry_small(r);
}

/* Reduces the 17 product columns c[0..16] (each < 2^64) to a tight element. */
static inline PZ_VABI void fev_reduce_cols(fev *r, const vl *c)
{
    const vl M = vl_set(FEV_MASK), F0 = vl_set(FEV_F0);
    /* propagate the high columns into 29-bit limbs h[0..8] (weights 2^261 ...) */
    vl h[9], cy = vl_set(0);
#pragma GCC unroll 8
    for (int k = 0; k < 8; k++) {
        vl t = c[9 + k] + cy;
        cy = t >> FEV_BITS;
        h[k] = t & M;
    }
    h[8] = cy;                                    /* < 2^32 */
    vl acc[10];
#pragma GCC unroll 9
    for (int j = 0; j < 9; j++) acc[j] = c[j];
    acc[9] = vl_set(0);
    /* h_m * 2^(29 m) * 2^261 = h_m * (F0 + 256 * 2^29) * 2^(29 m) */
#pragma GCC unroll 9
    for (int m = 0; m < 9; m++) {
        acc[m] += vl_mul32(h[m], F0);
        acc[m + 1] += h[m] << 8;
    }
    /* column 9 (< 2^40): fold once more */
    {
        vl lo = acc[9] & M, hi = acc[9] >> FEV_BITS;
        acc[0] += vl_mul32(lo, F0);
        acc[1] += (lo << 8) + vl_mul32(hi, F0);
        acc[2] += hi << 8;
    }
#pragma GCC unroll 9
    for (int j = 0; j < 9; j++) r->l[j] = acc[j];
    fev_carry(r);
}

static inline PZ_VABI void fev_mul(fev *r, const fev *a, const fev *b)
{
    vl c[17];
#pragma GCC unroll 17
    for (int k = 0; k < 17; k++) {
        const int i0 = k > 8 ? k - 8 : 0, i1 = k < 8 ? k : 8;
        vl s = vl_mul32(a->l[i0], b->l[k - i0]);
#pragma GCC unroll 9
        for (int i = i0 + 1; i <= i1; i++) s += vl_mul32(a->l[i], b->l[k - i]);
        c[k] = s;
    }
    fev_reduce_cols(r, c);
}

static inline PZ_VABI void fev_sqr(fev *r, const fev *a)
{
    vl a2[9], c[17];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) a2[i] = a->l[i] << 1;
#pragma GCC unroll 17
    for (int k = 0; k < 17; k++) {
        vl s = vl_set(0);
        /* i < j: doubled cross terms; i == j: the square */
#pragma GCC unroll 9
        for (int i = 0; i < 9; i++) {
            const int j = k - i;
            if (j < i || j > 8) continue;
            s += (i == j) ? vl_mul32(a->l[i], a->l[i]) : vl_mul32(a2[i], a->l[j]);
        }
        c[k] = s;
    }
    fev_reduce_cols(r, c);
}

/* Canonical reduction: every lane in [0, p), limbs < 2^29 and limb 8 < 2^24. */
static inline PZ_VABI void fev_normalize(fev *r)
{
    const vl M = vl_set(FEV_MASK), M24 = vl_set(((u64)1 << 24) - 1);
    fev_carry(r);
    /* fold the bits at and above 2^256 (limb 8 >> 24): h * 2^256 = h * (2^32 + 977); twice, the second time only matters
     * when the first carry ripple produced exactly 2^256 */
    for (int pass = 0; pass < 2; pass++) {
        vl h = r->l[8] >> 24;
        r->l[8] &= M24;
        r->l[0] += vl_mul32(h, vl_set(977));
        r->l[1] += h << 3;                      /* 2^32 = 8 * 2^29 */
        vl c = vl_set(0);
#pragma GCC unroll 9
        for (int i = 0; i < 9; i++) {
            vl t = r->l[i] + c;
            c = t >> FEV_BITS;
            r->l[i] = t & M;
        }
    }
    /* r < 2^256; r >= p  <=>  r + (2^32 + 977) overflows bit 256 */
    vl t[9], c = vl_set(0);
    t[0] = r->l[0] + vl_set(977);
    t[1] = r->l[1] + vl_set(8);
#pragma GCC unroll 7
    for (int i = 2; i < 9; i++) t[i] = r->l[i];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
        vl s = t[i] + c;
        c = s >> FEV_BITS;
        t[i] = s & M;
    }
    const vl sel = vl_set(0) - (t[8] >> 24);      /* all ones where r >= p */
    t[8] &= M24;
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = (t[i] & sel) | (r->l[i] & ~sel);
}

#include "fev_chain.inc"

#endif
