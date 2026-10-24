/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_avx2v.c - CPU linear-search engine with lane-parallel curve arithmetic, avx2v variant. Built with ISA-specific flags. */
#include "cpu_engine.h"
#include <pzcore/hash.h>
#include <pzcore/impl/hash_tables.h>

#define ISA avx2v
#define PZ_VBYTES 32
#define VHALF 256
#include "engine_vec_impl.inc"

static bool supported(void) { return cpu_has_avx2(); }
const cpu_engine ENG_AVX2V = {"avx2v", PZ_LANES, (u64)NL * GRPV, supported, CAT(engv_scan_, ISA), CAT(engv_hash_, ISA)};
