/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_avx512i2.c - the AVX-512 IFMA kangaroo round with the second half of the step done for two vectors at a time
 * (KV_PAIR in kang_vec_impl.inc). Same arithmetic and same results as kang_avx512i.c; the program times both and keeps the faster. */
#include "kangaroo_priv.h"
#define ISA avx512i2
#define PZ_VBYTES 64
#define PZ_FEV_IFMA 1
#define KV_PAIR 1
#include "kang_vec_impl.inc"
