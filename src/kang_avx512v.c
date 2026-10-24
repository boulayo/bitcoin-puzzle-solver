/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_avx512v.c - kangaroo round with lane-parallel arithmetic on AVX-512 (see Makefile for the flags). */
#include "kangaroo_priv.h"
#define ISA avx512v
#define VBYTES 64
#include "kang_vec_impl.inc"
