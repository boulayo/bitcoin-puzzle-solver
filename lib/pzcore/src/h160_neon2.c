/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* h160_neon2.c - lane kernel of the batch hash160 for aarch64 with the ARMv8 SHA-256 instructions (built with -march=armv8-a+crypto). */
#include "h160_impl.h"

#ifdef PZ_HAVE_ARM_SHA2
#define PZ_VBYTES 32
#define PZ_HASH_LANES_FN h160_lanes
#define PZ_HASH_EXTERNAL "pzcore/impl/hash_lanes_neon.inc"
#include <pzcore/impl/hash_lanes.inc>

void pz_h160_kernel_neon2(const uint32_t *X, const uint32_t *pfx, uint32_t *out) { h160_lanes(X, pfx, out); }
#endif
