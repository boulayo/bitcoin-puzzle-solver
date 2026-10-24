/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_avx512i.c - kangaroo round with lane-parallel arithmetic on AVX-512 IFMA (see Makefile for the flags). */
#include "kangaroo_priv.h"
#define ISA avx512i
#define PZ_VBYTES 64
#define PZ_FEV_IFMA 1
#include "kang_vec_impl.inc"
