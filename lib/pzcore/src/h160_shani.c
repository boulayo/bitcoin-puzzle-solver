/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* h160_shani.c - lane kernel of the batch hash160 for x86-64 with the SHA extensions: SHA-256 with sha256rnds2 and friends, RIPEMD-160 on
 * the 8 AVX2 lanes (built with -msha -mavx2 -mssse3 -msse4.1, see the Makefile). */
#include "h160_impl.h"

#ifdef PZ_HAVE_X86_SIMD
#define PZ_VBYTES 32
#define PZ_HASH_LANES_FN h160_lanes
#define PZ_HASH_SHA256_EXTERNAL "pzcore/impl/hash_lanes_shani.inc"
#include <pzcore/impl/hash_lanes.inc>

void pz_h160_kernel_shani(const uint32_t *X, const uint32_t *pfx, uint32_t *out) { h160_lanes(X, pfx, out); }
#endif
