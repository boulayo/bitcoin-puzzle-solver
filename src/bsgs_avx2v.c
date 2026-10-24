/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_avx2v.c - BSGS windows with lane-parallel arithmetic on AVX2 (see Makefile for the flags). */
#include "bsgs_priv.h"
#include "cpu_engine.h"
#define ISA avx2v
#define VBYTES 32
#include "bsgs_vec_impl.inc"

const bs_vengine BSV_AVX2V = {"AVX2", NL, cpu_has_avx2, CAT(bsv_prepare_, ISA), 2 * (BS_HALF + 1) * sizeof(fev), CAT(bsv_groups_, ISA)};
