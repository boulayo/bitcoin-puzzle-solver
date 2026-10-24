/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_neon2.c - CPU linear-search engine for aarch64: the same as eng_neon.c with 8 lanes, that is two NEON vectors advanced together,
 * which doubles the independent dependency chains of RIPEMD-160 (faster on cores that are limited by latency, slower on others: the
 * autotune decides). */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA neon2
#define VBYTES 32
#define PZ_HASH_EXTERNAL "hash_neon.inc"
#include "engine_impl.inc"

static bool supported(void) { return cpu_has_sha2(); }
const cpu_engine ENG_NEON2 = {"neon2", LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
