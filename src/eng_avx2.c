/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_avx2.c - CPU linear-search engine, avx2 variant. Built with ISA-specific flags (see Makefile). */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA avx2
#define VBYTES 32
#include "engine_impl.inc"

static bool supported(void) { return cpu_has_avx2(); }
const cpu_engine ENG_AVX2 = {"avx2", LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
