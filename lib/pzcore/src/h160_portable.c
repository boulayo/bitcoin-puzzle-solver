/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* h160_portable.c - lane kernel of the batch hash160 in portable C (four lanes of 32 bits with the GCC vector extensions, no instruction-set flags): the one every CPU runs. */
#include "h160_impl.h"

#define PZ_VBYTES 16
#define PZ_HASH_LANES_FN h160_lanes
#include <pzcore/impl/hash_lanes.inc>

void pz_h160_kernel_portable(const uint32_t *X, const uint32_t *pfx, uint32_t *out) { h160_lanes(X, pfx, out); }
