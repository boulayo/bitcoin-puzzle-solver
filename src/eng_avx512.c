/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_avx512.c - CPU linear-search engine, avx512 variant. Built with ISA-specific flags (see Makefile). */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA avx512
#define PZ_VBYTES 64
#include "engine_impl.inc"

static bool supported(void) { return cpu_has_avx512(); }
const cpu_engine ENG_AVX512 = {"avx512", PZ_LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
