/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_avx512v.c - CPU linear-search engine with lane-parallel curve arithmetic, avx512v variant. Built with ISA-specific flags. */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA avx512v
#define PZ_VBYTES 64
#define VHALF 256
#include "engine_vec_impl.inc"

static bool supported(void) { return cpu_has_avx512(); }
const cpu_engine ENG_AVX512V = {"avx512v", PZ_LANES, (u64)NL * GRPV, supported, CAT(engv_scan_, ISA), CAT(engv_hash_, ISA)};
