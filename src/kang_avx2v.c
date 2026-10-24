/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_avx2v.c - kangaroo round with lane-parallel arithmetic on AVX2 (see Makefile for the flags). */
#include "kangaroo_priv.h"
#define ISA avx2v
#define PZ_VBYTES 32
#include "kang_vec_impl.inc"
