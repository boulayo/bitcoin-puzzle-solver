/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_avx512v.c - BSGS windows with lane-parallel arithmetic on AVX-512 (see Makefile for the flags). */
#include "bsgs_priv.h"
#include "cpu_engine.h"
#define ISA avx512v
#define VBYTES 64
#include "bsgs_vec_impl.inc"

const bs_vengine BSV_AVX512V = {"AVX-512", NL, cpu_has_avx512, CAT(bsv_prepare_, ISA), 2 * (BS_HALF + 1) * sizeof(fev), CAT(bsv_groups_, ISA)};
