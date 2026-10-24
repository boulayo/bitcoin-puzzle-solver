/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_avx512i.c - CPU linear-search engine with lane-parallel curve arithmetic on AVX-512 IFMA (fev52.h).
 * Built with ISA-specific flags (see Makefile); selected only on processors that report IFMA. */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA avx512i
#define VBYTES 64
#define VHALF 256
#define FEV_IFMA 1
#include "engine_vec_impl.inc"

static bool supported(void) { return cpu_has_ifma(); }
const cpu_engine ENG_AVX512I = {"avx512i", LANES, (u64)NL * GRPV, supported, CAT(engv_scan_, ISA), CAT(engv_hash_, ISA)};
