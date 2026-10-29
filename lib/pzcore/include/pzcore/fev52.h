/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fev52.h - lane-parallel arithmetic in GF(p), p = 2^256 - 2^32 - 977 (secp256k1), with AVX-512 IFMA.
 *
 * Same interface as pz_fev.h (the engine in engine_vec_impl.inc is written against either), for 8 lanes of 64 bits.
 * One lane holds one field element in 5 limbs of 52 bits: vpmadd52luq / vpmadd52huq add the low / high 52 bits of a
 * 52x52-bit product to a 64-bit accumulator, so a full product takes 25 + 25 multiply-adds (81 multiplications plus
 * the additions with the 29-bit layout of pz_fev.h).
 *
 * The multipliers read only the low 52 bits of their operands, so every value that may reach a product must have
 * exact limbs. Every operation here therefore returns a "tight" element:
 *   limbs 0..3 < 2^52, limb 4 <= PZ_FEV_TOP_MAX = 2^49    (value < 2^257)
 * and every operation accepts tight operands (there is no loose form: pz_fev_add carries too).
 *
 * Constants: 2^256 = c (mod p) with c = 2^32 + 977, and the product columns of weight 2^260 = 16 c = PZ_FEV_R. */
#ifndef PZ_FEV_H
#define PZ_FEV_H

#include <pzcore/fe.h>
#include <immintrin.h>

#if !defined(PZ_VBYTES) || PZ_VBYTES != 64
#error "fev52.h needs PZ_VBYTES == 64 (AVX-512 IFMA)"
#endif

#define PZ_FEV_LANES 8
/* The includer may define PZ_FEV_INLINE (as __attribute__((always_inline))) to have the products inlined at every call; by
 * default the compiler decides, and in a large loop it keeps them out of line. */
#ifndef PZ_FEV_INLINE
#define PZ_FEV_INLINE
#endif
#define PZ_FEV_NLIMB 5
#define PZ_FEV_BITS 52
#define PZ_FEV_MASK (((uint64_t)1 << PZ_FEV_BITS) - 1)
#define PZ_FEV_TOPBITS 48                  /* bits of limb 4 in a canonical element (4 * 52 + 48 = 256) */
#define PZ_FEV_TOP_MAX ((uint64_t)1 << 49)
#define PZ_FEV_C 0x1000003D1ULL            /* 2^256 mod p */
#define PZ_FEV_R 0x1000003D10ULL           /* 2^260 mod p */
typedef uint64_t pz_vl __attribute__((vector_size(64)));
typedef struct { pz_vl l[5]; } pz_fev;

/* Multiples of p (4p and 8p) written with limbs that dominate a tight (K1) or the sum of two tight (K2) subtrahends;
 * checked by t_fev52. */
static const uint64_t pz_fev_k1[5] PZ_UNUSED = {0x1ffffbfffff0bcULL, 0x1ffffffffffffeULL, 0x1ffffffffffffeULL, 0x1ffffffffffffeULL, 0x3fffffffffffeULL};
static const uint64_t pz_fev_k2[5] PZ_UNUSED = {0x2ffff7ffffe178ULL, 0x2ffffffffffffdULL, 0x2ffffffffffffdULL, 0x2ffffffffffffdULL, 0x7fffffffffffdULL};

static inline PZ_VABI pz_vl pz_vl_set(uint64_t x) { return (pz_vl)_mm512_set1_epi64((long long)x); }
/* acc + low / high 52 bits of the 104-bit product of the low 52 bits of a and b */
static inline PZ_VABI pz_vl pz_vl_mlo(pz_vl acc, pz_vl a, pz_vl b) { return (pz_vl)_mm512_madd52lo_epu64((__m512i)acc, (__m512i)a, (__m512i)b); }
static inline PZ_VABI pz_vl pz_vl_mhi(pz_vl acc, pz_vl a, pz_vl b) { return (pz_vl)_mm512_madd52hi_epu64((__m512i)acc, (__m512i)a, (__m512i)b); }

/* ---- conversion from / to the scalar representation ---- */

static inline PZ_VABI void pz_fev_set_lane(pz_fev *r, int lane, const pz_fe *x)
{
    pz_fe t = *x;
    pz_fe_normalize(&t);
    r->l[0][lane] = t.n[0] & PZ_FEV_MASK;
    r->l[1][lane] = (t.n[0] >> 52 | t.n[1] << 12) & PZ_FEV_MASK;
    r->l[2][lane] = (t.n[1] >> 40 | t.n[2] << 24) & PZ_FEV_MASK;
    r->l[3][lane] = (t.n[2] >> 28 | t.n[3] << 36) & PZ_FEV_MASK;
    r->l[4][lane] = t.n[3] >> 16;
}

/* canonical lane value back to the scalar form */
static inline PZ_VABI void pz_fev_get_lane(pz_fe *x, const pz_fev *v, int lane)
{
    const uint64_t a = v->l[0][lane], b = v->l[1][lane], c = v->l[2][lane], d = v->l[3][lane], e = v->l[4][lane];
    x->n[0] = a | b << 52;
    x->n[1] = b >> 12 | c << 40;
    x->n[2] = c >> 24 | d << 28;
    x->n[3] = d >> 36 | e << 16;
}

/* ---- carry handling ---- */

/* x[0..4] with limbs below 2^63 and limb 4 below 2^60: folds the bits of limb 4 at and above 2^48 (weight 2^256 = c)
 * into limb 0, then one carry ripple. Result tight (limb 4 < 2^48 + 2^12). */
static inline PZ_VABI void pz_fev_settle(pz_vl *x)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK);
    const pz_vl t = x[4] >> PZ_FEV_TOPBITS;            /* < 2^12, so t * c < 2^45 fits the low half of the product */
    x[4] &= pz_vl_set(((uint64_t)1 << PZ_FEV_TOPBITS) - 1);
    x[0] = pz_vl_mlo(x[0], t, pz_vl_set(PZ_FEV_C));
#pragma GCC unroll 4
    for (int i = 0; i < 4; i++) {
        x[i + 1] += x[i] >> PZ_FEV_BITS;
        x[i] &= M;
    }
}

/* ---- arithmetic ---- */

static inline PZ_VABI void pz_fev_add(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = a->l[i] + b->l[i];
    pz_fev_settle(r->l);
}

/* r = a - b */
static inline PZ_VABI void pz_fev_sub(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k1[i]) - b->l[i];
    pz_fev_settle(r->l);
}

/* r = a - b - c; a may also be three times a canonical element (limbs below 2^54) */
static inline PZ_VABI void pz_fev_sub2(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *c)
{
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k2[i]) - b->l[i] - c->l[i];
    pz_fev_settle(r->l);
}

/* pz_fev_sub and pz_fev_sub2 without the final carries, for a result that only goes to pz_fev_normalize (which settles first); a may
 * also be the sum of two tight elements, limb by limb. */
static inline PZ_VABI void pz_fev_sub_lazy(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k1[i]) - b->l[i];
}
static inline PZ_VABI void pz_fev_sub2_lazy(pz_fev *r, const pz_fev *a, const pz_fev *b, const pz_fev *c)
{
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = a->l[i] + pz_vl_set(pz_fev_k2[i]) - b->l[i] - c->l[i];
}

/* Reduces the product columns t[0..9] (t[k] < 2^57, value = a*b with a, b < 2^257) to a tight element. */
static inline PZ_VABI void pz_fev_reduce_cols(pz_fev *r, pz_vl *t)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK), R = pz_vl_set(PZ_FEV_R);
    /* the high half (weight 2^260) as exact 52-bit limbs: its value is at most a*b / 2^260 < 2^254, so t[9] ends < 2^46 */
#pragma GCC unroll 4
    for (int k = 5; k < 9; k++) {
        t[k + 1] += t[k] >> PZ_FEV_BITS;
        t[k] &= M;
    }
    /* h_k * 2^(52 k) * 2^260 = h_k * R * 2^(52 k); h_4 * R < 2^83, so the last high half (weight 2^260) is < 2^31 */
    pz_vl top = pz_vl_set(0);
#pragma GCC unroll 4
    for (int k = 0; k < 4; k++) {
        t[k] = pz_vl_mlo(t[k], t[k + 5], R);
        t[k + 1] = pz_vl_mhi(t[k + 1], t[k + 5], R);
    }
    t[4] = pz_vl_mlo(t[4], t[9], R);
    top = pz_vl_mhi(top, t[9], R);
    /* bits at and above 2^256: limb 4 above bit 48 plus top * 16; their product with c can exceed 52 bits */
    const pz_vl hi = (t[4] >> PZ_FEV_TOPBITS) + (top << 4), C = pz_vl_set(PZ_FEV_C);
    t[4] &= pz_vl_set(((uint64_t)1 << PZ_FEV_TOPBITS) - 1);
    t[0] = pz_vl_mlo(t[0], hi, C);
    t[1] = pz_vl_mhi(t[1], hi, C);
#pragma GCC unroll 4
    for (int i = 0; i < 4; i++) {
        t[i + 1] += t[i] >> PZ_FEV_BITS;
        r->l[i] = t[i] & M;
    }
    r->l[4] = t[4];
}

static inline PZ_FEV_INLINE PZ_VABI void pz_fev_mul(pz_fev *r, const pz_fev *a, const pz_fev *b)
{
    pz_vl t[10];
#pragma GCC unroll 10
    for (int k = 0; k < 10; k++) t[k] = pz_vl_set(0);
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++)
#pragma GCC unroll 5
        for (int j = 0; j < 5; j++) {
            t[i + j] = pz_vl_mlo(t[i + j], a->l[i], b->l[j]);
            t[i + j + 1] = pz_vl_mhi(t[i + j + 1], a->l[i], b->l[j]);
        }
    pz_fev_reduce_cols(r, t);
}

static inline PZ_FEV_INLINE PZ_VABI void pz_fev_sqr(pz_fev *r, const pz_fev *a)
{
    pz_vl t[10];
#pragma GCC unroll 10
    for (int k = 0; k < 10; k++) t[k] = pz_vl_set(0);
    /* the cross products once, doubled afterwards (doubling an operand would break the 52-bit input limit) */
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++)
#pragma GCC unroll 5
        for (int j = i + 1; j < 5; j++) {
            t[i + j] = pz_vl_mlo(t[i + j], a->l[i], a->l[j]);
            t[i + j + 1] = pz_vl_mhi(t[i + j + 1], a->l[i], a->l[j]);
        }
#pragma GCC unroll 10
    for (int k = 1; k < 10; k++) t[k] += t[k];
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) {
        t[2 * i] = pz_vl_mlo(t[2 * i], a->l[i], a->l[i]);
        t[2 * i + 1] = pz_vl_mhi(t[2 * i + 1], a->l[i], a->l[i]);
    }
    pz_fev_reduce_cols(r, t);
}

/* Canonical reduction: every lane in [0, p), limbs 0..3 < 2^52 and limb 4 < 2^48. */
static inline PZ_VABI void pz_fev_normalize(pz_fev *r)
{
    const pz_vl M = pz_vl_set(PZ_FEV_MASK), M48 = pz_vl_set(((uint64_t)1 << PZ_FEV_TOPBITS) - 1);
    /* one fold leaves limb 4 below 2^48 + 2^11 (limbs below 2^63; at most 2^48 for tight ones), so r < 2^256 + 2^219; then r >= p  <=>
     * r + c reaches 2^256 (and r - p < p) */
    pz_fev_settle(r->l);
    pz_vl t[5];
    t[0] = r->l[0] + pz_vl_set(PZ_FEV_C);
#pragma GCC unroll 4
    for (int i = 1; i < 5; i++) t[i] = r->l[i];
#pragma GCC unroll 4
    for (int i = 0; i < 4; i++) {
        t[i + 1] += t[i] >> PZ_FEV_BITS;
        t[i] &= M;
    }
    const pz_vl sel = pz_vl_set(0) - (t[4] >> PZ_FEV_TOPBITS);     /* all ones where r >= p */
    t[4] &= M48;
#pragma GCC unroll 5
    for (int i = 0; i < 5; i++) r->l[i] = (t[i] & sel) | (r->l[i] & ~sel);
}

/* Canonical reduction in the common case, with the interface of pz_fev.h. A tight element has exact limbs 0..3, so it is
 * canonical already when limb 4 is below 2^48 - 1 (the value is then below 2^256 - 2^208 < p): nothing to do but check. The
 * returned vector is non-zero in the other lanes (about 2^-48 per lane, every value >= p among them), which keep their value
 * and need pz_fev_normalize. */
static inline PZ_VABI pz_vl pz_fev_normalize_fast(pz_fev *r) { return (r->l[4] + pz_vl_set(1)) >> PZ_FEV_TOPBITS; }

/* Bit 0 of the canonical value of each lane of a tight element: that of limb 0, except in the lanes set in *unsure as by
 * pz_fev_normalize_fast. */
static inline PZ_VABI pz_vl pz_fev_parity_fast(const pz_fev *a, pz_vl *unsure)
{
    *unsure |= (a->l[4] + pz_vl_set(1)) >> PZ_FEV_TOPBITS;
    return a->l[0] & pz_vl_set(1);
}

#include <pzcore/impl/fev_chain.inc>

#endif
