/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_shani.c - CPU linear-search engine for x86-64 with the SHA extensions: the 8-lane avx2 engine with the SHA-256 stage done by
 * sha256rnds2 and friends (hash_lanes_shani.inc) and RIPEMD-160 on the AVX2 lanes. Built with -msha on top of the flags of eng_avx2.c and
 * used only when the CPU reports the extensions; the autotune picks it only if it measures faster than the others. */
#include "cpu_engine.h"
#include <pzcore/hash.h>
#include <pzcore/impl/hash_tables.h>

#define ISA shani
#define PZ_VBYTES 32
#define PZ_HASH_SHA256_EXTERNAL "pzcore/impl/hash_lanes_shani.inc"
#include "engine_impl.inc"

static bool supported(void) { return cpu_has_shani(); }
const cpu_engine ENG_SHANI = {"shani", PZ_LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
