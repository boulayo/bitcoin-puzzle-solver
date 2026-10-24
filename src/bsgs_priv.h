/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_priv.h - internals shared by bsgs.c and the per-ISA group routine. */
#ifndef PZ_BSGS_PRIV_H
#define PZ_BSGS_PRIV_H

#include "bsgs.h"

/* Computes the x-coordinates of C + off*U for off = -BS_HALF .. BS_HALF-1 (U is the step the tables were built for),
 * stores x.n[0] / x.n[1] (normalized) at position off + BS_HALF, and advances C by BS_GRP*U.  dx and tmp hold
 * BS_HALF + 1 elements.  Returns false (C untouched) when a denominator is zero: some point of the window is
 * the point at infinity or collides with the centre, which the caller handles exactly. */
typedef bool (*bs_group_fn)(const bs_steps *st, ge *C, u64 *xlo, u64 *xhi, fe *dx, fe *tmp);
bool bs_group_base(const bs_steps *st, ge *C, u64 *xlo, u64 *xhi, fe *dx, fe *tmp);
bool bs_group_x86(const bs_steps *st, ge *C, u64 *xlo, u64 *xhi, fe *dx, fe *tmp);

/* Lane-parallel variant: `lanes` consecutive windows per call (centres C[0..lanes), advanced by lanes*BS_GRP*U), x
 * words interleaved as xlo[(off + BS_HALF)*lanes + l]. prepare() builds its tables for a bs_steps (NULL on failure,
 * free with pz_afree); scratch is the size of the work area groups() needs; false = a zero denominator (C untouched). */
typedef struct {
    const char *name;
    int lanes;
    bool (*supported)(void);
    void *(*prepare)(const bs_steps *st);
    size_t scratch;
    bool (*groups)(const void *vt, ge *C, u64 *xlo, u64 *xhi, void *scratch);
} bs_vengine;
extern const bs_vengine BSV_AVX2V, BSV_AVX512V, BSV_AVX512I;

#define BS_TAG_BITS 24
#define BS_VAL_BITS 40
#define BS_VAL_MASK (((u64)1 << BS_VAL_BITS) - 1)

static inline u64 bs_slot_index(u64 nslots, u64 xlo) { return (u64)(((u128)xlo * nslots) >> 64); }
static inline u64 bs_entry(u64 xhi, u64 b) { return ((xhi & (((u64)1 << BS_TAG_BITS) - 1)) << BS_VAL_BITS) | b; }

/* Idempotent lock-free insertion (an identical entry already present is not duplicated). Returns true when added. */
static inline bool bs_insert(u64 *slots, u64 nslots, u64 xlo, u64 xhi, u64 b)
{
    const u64 e = bs_entry(xhi, b);
    u64 i = bs_slot_index(nslots, xlo);
    for (;;) {
        u64 s = __atomic_load_n(&slots[i], __ATOMIC_RELAXED);
        if (s == 0) {
            u64 expected = 0;
            if (__atomic_compare_exchange_n(&slots[i], &expected, e, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
            s = expected;
        }
        if (s == e) return false;
        if (++i == nslots) i = 0;
    }
}

#endif
