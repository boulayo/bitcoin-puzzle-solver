/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eng_base.c - CPU linear-search engine, base variant. Built with ISA-specific flags (see Makefile). */
#include "cpu_engine.h"
#include "hash.h"
#include "hash_tables.h"

#define ISA base
#define VBYTES 16
#include "engine_impl.inc"

static bool supported(void) { return true; }
const cpu_engine ENG_BASE = {"baseline", LANES, CPU_GRP, supported, CAT(eng_scan_, ISA), CAT(eng_hash_, ISA)};
