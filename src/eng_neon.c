/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_neon.c - CPU linear-search engine for aarch64: the 4-lane engine with the SHA-256 stage done by the ARMv8 SHA2 instructions
 * and RIPEMD-160 on NEON vectors (hash_neon.inc). Built with -march=armv8-a+crypto (see the Makefile) and used only when the CPU
 * reports SHA2. */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA neon
#define VBYTES 16
#define PZ_HASH_EXTERNAL "hash_neon.inc"
#include "engine_impl.inc"

static bool supported(void) { return cpu_has_sha2(); }
const cpu_engine ENG_NEON = {"neon", LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
