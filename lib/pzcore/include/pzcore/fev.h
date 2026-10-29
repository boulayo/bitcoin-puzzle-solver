/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_fev.h - lane-parallel arithmetic in GF(p), p = 2^256 - 2^32 - 977 (secp256k1).
 *
 * The includer defines PZ_VBYTES (32 for AVX2, 64 for AVX-512) before including and compiles with the matching -m flags.
 * One vector lane holds one independent field element, spread over 9 limbs of 29 bits ("structure of arrays"):
 * the 32x32->64 multiplier (vpmuludq) then does 4 or 8 products per instruction, and a product column of up to 9
 * terms of at most 1.03 * 2^60 (loose operands) still fits in a 64-bit lane (9 * PZ_FEV_LOOSE^2 < 2^63.3).
 *
 * Limb bounds used throughout:
 *   tight  : limbs <= PZ_FEV_TIGHT = 2^29 + 2^23   (result of pz_fev_mul, pz_fev_sqr, pz_fev_sub, pz_fev_sub2, pz_fev_normalize)
 *   loose  : limbs <= PZ_FEV_LOOSE = 2 * PZ_FEV_TIGHT (sum of two tight values; the largest operand pz_fev_mul / pz_fev_sqr accept:
 *            nine products of two such limbs still fit in 64 bits)
 * pz_fev_sub / pz_fev_sub2 add a multiple of p whose limbs dominate the subtrahend and finish with a carry pass, so
 * their result is tight. pz_fev_add only adds limbs (tight + tight = loose). */
#ifndef PZ_FEV_H
#define PZ_FEV_H

#include <pzcore/fe.h>
#include <immintrin.h>

#ifndef PZ_VBYTES
#error "define PZ_VBYTES before including fev.h"
#endif

#define PZ_FEV_LANES (PZ_VBYTES / 8)
/* The includer may define PZ_FEV_INLINE (as __attribute__((always_inline))) to have the products inlined at every call; by
 * default the compiler decides, and in a large loop it keeps them out of line. */
#ifndef PZ_FEV_INLINE
#define PZ_FEV_INLINE
#endif
#define PZ_FEV_NLIMB 9
typedef uint64_t pz_vl __attribute__((vector_size(PZ_VBYTES)));
typedef struct { pz_vl l[9]; } pz_fev;

#define PZ_FEV_TIGHT (((uint64_t)1 << 29) + ((uint64_t)1 << 23))
#define PZ_FEV_LOOSE (2 * PZ_FEV_TIGHT)
#define PZ_FEV_BITS 29
#define PZ_FEV_TOPBITS 24                  /* bits of limb 8 in a canonical element (8 * 29 + 24 = 256) */
#define PZ_FEV_MASK (((uint64_t)1 << PZ_FEV_BITS) - 1)
/* 2^261 = 32 * (2^32 + 977) mod p = PZ_FEV_F0 + 256 * 2^29 */
#define PZ_FEV_F0 0x7A20ULL

/* Multiples of p written with limbs that dominate a tight (K1) or the sum of two loose (K2) subtrahend; checked by t_fev. */
static const uint64_t pz_fev_k1[9] PZ_UNUSED = {0x5fff07efULL, 0x5ffffdf5ULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x5ffffffdULL, 0x40fffffdULL};
static const uint64_t pz_fev_k2[9] PZ_UNUSED = {0x11ffc2b2fULL, 0x11ffff7efULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x11ffffff7ULL, 0x100fffff7ULL};

static inline PZ_VABI pz_vl pz_vl_set(uint64_t x)
{
    pz_vl r = {0};
    for (int i = 0; i < PZ_FEV_LANES; i++) r[i] = x;
    return r;
}

/* low 32 bits of every lane multiplied pairwise (vpmuludq) */
static inline PZ_VABI pz_vl pz_vl_mul32(pz_vl a, pz_vl b)
{
#if PZ_VBYTES == 64
    return (pz_vl)_mm512_mul_epu32((__m512i)a, (__m512i)b);
#else
    return (pz_vl)_mm256_mul_epu32((__m256i)a, (__m256i)b);
#endif
}

/* c * PZ_FEV_F0 for c < 2^40 */
static inline PZ_VABI pz_vl pz_vl_mulf0(pz_vl c)
{
    const pz_vl f = pz_vl_set(PZ_FEV_F0);
    return pz_vl_mul32(c, f) + (pz_vl_mul32(c >> 32, f) << 32);
}

/* ---- conversion from / to the scalar representation ---- */

static inline PZ_VABI void pz_fev_set_lane(pz_fev *r, int lane, const pz_fe *x)
{
    pz_fe t = *x;
    pz_fe_normalize(&t);
    pz_u128 lo = (pz_u128)t.n[1] << 64 | t.n[0], hi = (pz_u128)t.n[3] << 64 | t.n[2];
    for (int i = 0; i < 9; i++) {
        int bit = 29 * i;
        uint64_t v;
        if (bit + 29 <= 128) v = (uint64_t)(lo >> bit) & PZ_FEV_MASK;
        else if (bit >= 128) v = (uint64_t)(hi >> (bit - 128)) & PZ_FEV_MASK;
        else v = (uint64_t)((lo >> bit) | (hi << (128 - bit))) & PZ_FEV_MASK;
        r->l[i][lane] = v;
    }
}

/* canonical lane value back to the scalar form */
static inline PZ_VABI void pz_fev_get_lane(pz_fe *x, const pz_fev *v, int lane)
{
    pz_u128 acc = 0;
    int bits = 0, w = 0;
    uint64_t out[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 9; i++) {
        acc |= (pz_u128)v->l[i][lane] << bits;
        bits += 29;
        while (bits >= 64) { out[w++] = (uint64_t)acc; acc >>= 64; bits -= 64; }
    }
    out[w] = (uint64_t)acc;
    x->n[0] = out[0]; x->n[1] = out[1]; x->n[2] = out[2]; x->n[3] = out[3];
}

/* ---- carry handling ---- */

/* Brings every limb to <= 2^29 + 2^17. Valid for limbs below 2^63.5; the carry out of limb 8 (weight 2^261) is folded back. */
static inline PZ_VABI void pz_fev_carry(pz_fev *r)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK);
    pz_vl c = pz_vl_set(0);
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
        pz_vl t = r->l[i] + c;
        c = t >> PZ_FEV_BITS;
        r->l[i] = t & M;
    }
    pz_vl t0 = r->l[0] + pz_vl_mulf0(c);
    pz_vl t1 = r->l[1] + (c << 8);
    pz_vl c0 = t0 >> PZ_FEV_BITS;
    r->l[0] = t0 & M;
    t1 += c0;
    pz_vl c1 = t1 >> PZ_FEV_BITS;
    r->l[1] = t1 & M;
    r->l[2] += c1;
}

/* One parallel carry pass for limbs below 2^37 (the results of the additions/subtractions below): every limb keeps its
 * low 29 bits and receives the carry of its lower neighbour, the carry out of limb 8 is folded back. */
static inline PZ_VABI void pz_fev_carry_small(pz_fev *r)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK);
    pz_vl c[9];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) c[i] = r->l[i] >> PZ_FEV_BITS;
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] &= M;
    r->l[0] += pz_vl_mul32(c[8], pz_vl_set(PZ_FEV_F0));
    r->l[1] += c[0] + (c[8] << 8);
#pragma GCC unroll 7
    for (int i = 2; i < 9; i++) r->l[i] += c[i - 1];
}

/* ---- arithmetic ---- */

static inline PZ_VABI void pz_fev_add(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + b->l[i];
}

/* r = a - b; b tight, a tight or loose. Result tight. */
static inline PZ_VABI void pz_fev_sub(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k1[i]) - b->l[i];
    pz_fev_carry_small(r);
}

/* r = a - b - c; b and c tight or loose, a with limbs below 2^31 (tight, loose, or three times a canonical element: the carries
 * of the pass are then at most 12). Result tight. */
static inline PZ_VABI void pz_fev_sub2(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *c)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k2[i]) - b->l[i] - c->l[i];
    pz_fev_carry_small(r);
}

/* pz_fev_sub and pz_fev_sub2 without their carry pass: limbs below 2^33 for the operands those accept, a result that may go to
 * pz_fev_normalize, pz_fev_normalize_fast or pz_fev_parity_fast. pz_fev_sub_lazy of two tight elements has limbs below
 * 2^31 + 2^23 and may also be a factor of pz_fev_mul whose other factor is tight (nine such products fit in 64 bits, as with two
 * loose factors). */
static inline PZ_VABI void pz_fev_sub_lazy(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k1[i]) - b->l[i];
}
static inline PZ_VABI void pz_fev_sub2_lazy(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *c)
{
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k2[i]) - b->l[i] - c->l[i];
}

/* Reduces the 17 product columns c[0..16] (each < 2^64) to a tight element. */
static inline PZ_VABI void pz_fev_reduce_cols(pz_fev *r, const pz_vl *c)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK), F0 = pz_vl_set(PZ_FEV_F0);
    /* propagate the high columns into 29-bit limbs h[0..8] (weights 2^261 ...) */
    pz_vl h[9], cy = pz_vl_set(0);
#pragma GCC unroll 8
    for (int k = 0; k < 8; k++) {
        pz_vl t = c[9 + k] + cy;
        cy = t >> PZ_FEV_BITS;
        h[k] = t & M;
    }
    h[8] = cy;                                    /* < 2^32 */
    pz_vl acc[10];
#pragma GCC unroll 9
    for (int j = 0; j < 9; j++) acc[j] = c[j];
    acc[9] = pz_vl_set(0);
    /* h_m * 2^(29 m) * 2^261 = h_m * (F0 + 256 * 2^29) * 2^(29 m) */
#pragma GCC unroll 9
    for (int m = 0; m < 9; m++) {
        acc[m] += pz_vl_mul32(h[m], F0);
        acc[m + 1] += h[m] << 8;
    }
    /* column 9 (< 2^40): fold once more */
    {
        pz_vl lo = acc[9] & M, hi = acc[9] >> PZ_FEV_BITS;
        acc[0] += pz_vl_mul32(lo, F0);
        acc[1] += (lo << 8) + pz_vl_mul32(hi, F0);
        acc[2] += hi << 8;
    }
#pragma GCC unroll 9
    for (int j = 0; j < 9; j++) r->l[j] = acc[j];
    pz_fev_carry(r);
}

static inline PZ_FEV_INLINE PZ_VABI void pz_fev_mul(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
    pz_vl c[17];
#pragma GCC unroll 17
    for (int k = 0; k < 17; k++) {
        const int i0 = k > 8 ? k - 8 : 0, i1 = k < 8 ? k : 8;
        pz_vl s = pz_vl_mul32(a->l[i0], b->l[k - i0]);
#pragma GCC unroll 9
        for (int i = i0 + 1; i <= i1; i++) s += pz_vl_mul32(a->l[i], b->l[k - i]);
        c[k] = s;
    }
    pz_fev_reduce_cols(r, c);
}

static inline PZ_FEV_INLINE PZ_VABI void pz_fev_sqr(pz_fev *r, const pz_fev *a)
{
    pz_vl a2[9], c[17];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) a2[i] = a->l[i] << 1;
#pragma GCC unroll 17
    for (int k = 0; k < 17; k++) {
        pz_vl s = pz_vl_set(0);
        /* i < j: doubled cross terms; i == j: the square */
#pragma GCC unroll 9
        for (int i = 0; i < 9; i++) {
            const int j = k - i;
            if (j < i || j > 8) continue;
            s += (i == j) ? pz_vl_mul32(a->l[i], a->l[i]) : pz_vl_mul32(a2[i], a->l[j]);
        }
        c[k] = s;
    }
    pz_fev_reduce_cols(r, c);
}

/* Canonical reduction: every lane in [0, p), limbs < 2^29 and limb 8 < 2^24. The limbs must be below 2^34: every tight or loose
 * element, and the sums and differences of this header before their carry pass. The bits of limb 8 at and above 2^24 (weight
 * 2^256 = 2^32 + 977, at most 2^10 of them) are folded first, so a single carry ripple leaves the limbs below 2^29 and limb 8 at most
 * 2^24 + 32: r < 2^256 + 2^237. Then r >= p  <=>  r + (2^32 + 977) reaches 2^256, and r - p < p. */
static inline PZ_VABI void pz_fev_normalize(pz_fev *r)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK), M24 = pz_vl_set(((uint64_t)1 << 24) - 1);
    {
        const pz_vl h = r->l[8] >> 24;
        r->l[8] &= M24;
        r->l[0] += pz_vl_mul32(h, pz_vl_set(977));
        r->l[1] += h << 3;                      /* 2^32 = 8 * 2^29 */
        pz_vl c = pz_vl_set(0);
#pragma GCC unroll 9
        for (int i = 0; i < 9; i++) {
            pz_vl t = r->l[i] + c;
            c = t >> PZ_FEV_BITS;
            r->l[i] = t & M;
        }
    }
    pz_vl t[9], c = pz_vl_set(0);
    t[0] = r->l[0] + pz_vl_set(977);
    t[1] = r->l[1] + pz_vl_set(8);
#pragma GCC unroll 7
    for (int i = 2; i < 9; i++) t[i] = r->l[i];
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
        pz_vl s = t[i] + c;
        c = s >> PZ_FEV_BITS;
        t[i] = s & M;
    }
    const pz_vl sel = pz_vl_set(0) - (t[8] >> 24);      /* all ones where r >= p */
    t[8] &= M24;
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) r->l[i] = (t[i] & sel) | (r->l[i] & ~sel);
}

/* Canonical reduction in the common case, for the limbs pz_fev_normalize accepts (below 2^34): one carry ripple, one fold of the
 * bits at and above 2^256, and two more carry steps. The result is canonical in every lane where the returned vector is zero.
 * The other lanes, where limb 2 overflowed or the top limb ended all ones (about 2^-24 per lane, which includes every value
 * >= p), keep their value and pz_fev_normalize finishes them. */
static inline PZ_VABI pz_vl pz_fev_normalize_fast(pz_fev *r)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK), M24 = pz_vl_set(((uint64_t)1 << 24) - 1);
    pz_vl c = pz_vl_set(0);
#pragma GCC unroll 9
    for (int i = 0; i < 9; i++) {
        pz_vl t = r->l[i] + c;
        c = t >> PZ_FEV_BITS;
        r->l[i] = t & M;
    }
    const pz_vl h = (r->l[8] >> 24) + (c << 5);          /* the value divided by 2^256; h * 2^256 = h * (2^32 + 977) */
    r->l[8] &= M24;
    const pz_vl t0 = r->l[0] + pz_vl_mul32(h, pz_vl_set(977));
    const pz_vl t1 = r->l[1] + (h << 3) + (t0 >> PZ_FEV_BITS);
    r->l[0] = t0 & M;
    r->l[1] = t1 & M;
    r->l[2] += t1 >> PZ_FEV_BITS;
    return (r->l[2] >> PZ_FEV_BITS) | ((r->l[8] + pz_vl_set(1)) >> 24);
}

/* Bit 0 of the canonical value of each lane of a (limbs below 2^34, as for pz_fev_normalize), a unchanged. With
 * h = floor(a / 2^256) the residue is (a mod 2^256) + h * (2^32 + 977), whose bit 0 is that of limb 0 xor h, unless that sum
 * reaches p: only possible when the top limb ends all ones, and those lanes are set in *unsure (as by pz_fev_normalize_fast). */
static inline PZ_VABI pz_vl pz_fev_parity_fast(const pz_fev *a, pz_vl *unsure)
{
    pz_vl t = a->l[0];
#pragma GCC unroll 8
    for (int i = 1; i < 9; i++) t = a->l[i] + (t >> PZ_FEV_BITS);         /* only the carries: t ends as limb 8 with its carry */
    const pz_vl h = t >> 24;
    *unsure |= ((t & pz_vl_set(((uint64_t)1 << 24) - 1)) + pz_vl_set(1)) >> 24;
    return (a->l[0] ^ h) & pz_vl_set(1);
}

#include <pzcore/impl/fev_chain.inc>

#endif
