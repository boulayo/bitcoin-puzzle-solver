/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_avx512i.c - BSGS windows with lane-parallel arithmetic on AVX-512 IFMA (see Makefile for the flags). */
#include "bsgs_priv.h"
#include "cpu_engine.h"
#define ISA avx512i
#define PZ_VBYTES 64
#define PZ_FEV_IFMA 1
#include "bsgs_vec_impl.inc"

const bs_vengine BSV_AVX512I = {"AVX-512 IFMA", NL, cpu_has_ifma, CAT(bsv_prepare_, ISA), 2 * (BS_HALF + 1) * sizeof(pz_fev), CAT(bsv_groups_, ISA)};
