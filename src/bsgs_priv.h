/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_priv.h - internals shared by bsgs.c, the per-ISA group routine and the tests. */
#ifndef PZ_BSGS_PRIV_H
#define PZ_BSGS_PRIV_H

#include "bsgs.h"

/* Computes the x-coordinates of C + off*U for off = -BS_HALF .. BS_HALF-1 (U is the step the tables were built for),
 * stores x.n[0] / x.n[1] (normalized) at position off + BS_HALF, and advances C by BS_GRP*U.  dx and tmp hold
 * BS_HALF + 1 elements.  Returns false (C untouched) when a denominator is zero: some point of the window is
 * the point at infinity or collides with the centre, which the caller handles exactly. */
typedef bool (*bs_group_fn)(const bs_steps *st, pz_ge *C, u64 *xlo, u64 *xhi, pz_fe *dx, pz_fe *tmp);
bool bs_group_base(const bs_steps *st, pz_ge *C, u64 *xlo, u64 *xhi, pz_fe *dx, pz_fe *tmp);
bool bs_group_x86(const bs_steps *st, pz_ge *C, u64 *xlo, u64 *xhi, pz_fe *dx, pz_fe *tmp);

/* Lane-parallel variant: `lanes` consecutive windows per call (centres C[0..lanes), advanced by lanes*BS_GRP*U), x
 * words interleaved as xlo[(off + BS_HALF)*lanes + l]. prepare() builds its tables for a bs_steps (NULL on failure,
 * free with pz_afree); scratch is the size of the work area groups() needs; false = a zero denominator (C untouched). */
typedef struct {
    const char *name;
    int lanes;
    bool (*supported)(void);
    void *(*prepare)(const bs_steps *st);
    size_t scratch;
    bool (*groups)(const void *vt, pz_ge *C, u64 *xlo, u64 *xhi, void *scratch);
} bs_vengine;
extern const bs_vengine BSV_AVX2V, BSV_AVX512V, BSV_AVX512I;

#define BS_TAG_BITS 24
#define BS_VAL_BITS 40
#define BS_VAL_MASK (((u64)1 << BS_VAL_BITS) - 1)
#define BS_TAG_MASK (((u64)1 << BS_TAG_BITS) - 1)

#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
#include <emmintrin.h>
#define BS_SSE2 1
#endif

static inline u64 bs_slot_index(u64 nslots, u64 xlo) { return (u64)(((u128)xlo * nslots) >> 64); }   /* linear (old) layout: first cell to look at */
static inline u64 bs_bucket_index(u64 nbuckets, u64 xlo) { return (u64)(((u128)xlo * nbuckets) >> 64); }
static inline u64 bs_entry(u64 xhi, u64 b) { return ((xhi & BS_TAG_MASK) << BS_VAL_BITS) | b; }

/* Idempotent lock-free insertion (an identical entry already present is not duplicated). Returns true when added.
 * A baby takes the first free cell of its bucket and, when the bucket is full, of the following ones (wrapping around): the
 * cells of a bucket are therefore taken in order, so "the last cell is free" means "nothing was ever pushed past this bucket",
 * which is where a lookup stops. The probe cannot run forever: a table always has more cells than babies. */
static inline bool bs_insert(u64 *cells, u64 nbuckets, u64 xlo, u64 xhi, u64 b)
{
    const u64 e = bs_entry(xhi, b);
    u64 i = bs_bucket_index(nbuckets, xlo);
    for (;;) {
        u64 *bk = cells + i * BS_BUCKET;
        for (int j = 0; j < BS_BUCKET; j++) {
            u64 s = __atomic_load_n(&bk[j], __ATOMIC_RELAXED);
            if (s == 0) {
                u64 expected = 0;
                if (__atomic_compare_exchange_n(&bk[j], &expected, e, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
                s = expected;
            }
            if (s == e) return false;
        }
        if (++i == nbuckets) i = 0;
    }
}

/* True when a cell of the bucket may carry `tag` (the 24 bits above the baby index). An empty cell has tag 0, so for tag 0 the
 * answer is a "maybe" that the caller settles cell by cell; for any other tag it is exact. The two versions give the same answer. */
static inline bool bs_bucket_maybe_scalar(const u64 *bk, u64 tag)
{
    unsigned m = 0;
    for (int j = 0; j < BS_BUCKET; j++) m |= (unsigned)((bk[j] >> BS_VAL_BITS) == tag);
    return m != 0;
}

static inline bool bs_bucket_maybe(const u64 *bk, u64 tag)
{
#ifdef BS_SSE2
    /* the tag sits in the high 32-bit word of each cell, above 8 bits of the baby index: mask those off and compare 32 bits */
    const __m128i want = _mm_set_epi32((int)(tag << 8), 0, (int)(tag << 8), 0);
    const __m128i keep = _mm_set_epi32((int)0xFFFFFF00u, 0, (int)0xFFFFFF00u, 0);
    __m128i e0 = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128((const __m128i *)bk), keep), want);
    __m128i e1 = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128((const __m128i *)(bk + 2)), keep), want);
    __m128i e2 = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128((const __m128i *)(bk + 4)), keep), want);
    __m128i e3 = _mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128((const __m128i *)(bk + 6)), keep), want);
    /* the low words compare 0 with 0 and always agree: only the bits of the high words (1 and 3) count */
    return (_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_or_si128(e0, e1), _mm_or_si128(e2, e3)))) & 0xA) != 0;
#else
    return bs_bucket_maybe_scalar(bk, tag);
#endif
}

/* ---- PZBSGS03: 4-byte cells holding a tag only ---- */

/* The tag of an x-coordinate: its 32 low bits of x.n[1], with 0 (an empty cell) replaced by 1 */
static inline u32 bs_tag32(u64 xhi) { const u32 t = (u32)xhi; return t ? t : 1; }

/* Lock-free insertion of a tag, in the first free cell of the bucket or of the following ones (wrapping around), as bs_insert. Equal tags
 * are kept (two babies may have them): every call adds a cell. */
static inline void bs_insert32(u32 *cells, u64 nbuckets, u64 xlo, u32 tag)
{
    u64 i = bs_bucket_index(nbuckets, xlo);
    for (;;) {
        u32 *bk = cells + i * BS_BUCKET32;
        for (int j = 0; j < BS_BUCKET32; j++) {
            u32 expected = 0;
            if (__atomic_load_n(&bk[j], __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(&bk[j], &expected, tag, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
        }
        if (++i == nbuckets) i = 0;
    }
}

/* True when a cell of the bucket holds `tag` (never 0, so an empty cell does not match). The two versions give the same answer. */
static inline bool bs_bucket32_has_scalar(const u32 *bk, u32 tag)
{
    unsigned m = 0;
    for (int j = 0; j < BS_BUCKET32; j++) m |= (unsigned)(bk[j] == tag);
    return m != 0;
}

static inline bool bs_bucket32_has(const u32 *bk, u32 tag)
{
#ifdef BS_SSE2
    const __m128i want = _mm_set1_epi32((int)tag);
    __m128i e0 = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)bk), want);
    __m128i e1 = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(bk + 4)), want);
    __m128i e2 = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(bk + 8)), want);
    __m128i e3 = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(bk + 12)), want);
    return _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(e0, e1), _mm_or_si128(e2, e3))) != 0;
#else
    return bs_bucket32_has_scalar(bk, tag);
#endif
}

#endif
