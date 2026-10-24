/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* h160_portable.c - lane kernel of the batch hash160 for portable (built with its own instruction-set flags, see the Makefile). */
#include "h160_impl.h"

#define PZ_VBYTES 16
#define PZ_HASH_LANES_FN h160_lanes
#include <pzcore/impl/hash_lanes.inc>

void pz_h160_kernel_portable(const uint32_t *X, const uint32_t *pfx, uint32_t *out) { h160_lanes(X, pfx, out); }
