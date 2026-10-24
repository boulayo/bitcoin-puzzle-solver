/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* h160_impl.h - the lane kernels of the batch hash160, one translation unit per instruction set (private to the library). */
#ifndef PZCORE_H160_IMPL_H
#define PZCORE_H160_IMPL_H

#include <pzcore/defs.h>

/* which kernels exist in this build: the x86-64 ones unless PZ_NO_X86_VARIANTS is defined, the aarch64 ones unless PZ_NO_ARM_SHA2 is */
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
#define PZ_HAVE_X86_SIMD 1
#endif
#if defined(__aarch64__) && !defined(__AARCH64EB__) && !defined(PZ_NO_ARM_SHA2)
#define PZ_HAVE_ARM_SHA2 1
#endif

/* Layout as in impl/hash_lanes.inc: X[8][lanes] big-endian words, pfx[lanes], out[5][lanes] little-endian words. */
typedef void (*pz_h160_kernel_fn)(const uint32_t *X, const uint32_t *pfx, uint32_t *out);

void pz_h160_kernel_portable(const uint32_t *X, const uint32_t *pfx, uint32_t *out);   /* 4 lanes, no instruction-set flags */
#ifdef PZ_HAVE_X86_SIMD
void pz_h160_kernel_avx2(const uint32_t *X, const uint32_t *pfx, uint32_t *out);       /* 8 lanes */
void pz_h160_kernel_avx512(const uint32_t *X, const uint32_t *pfx, uint32_t *out);     /* 16 lanes */
#endif
#ifdef PZ_HAVE_ARM_SHA2
void pz_h160_kernel_neon(const uint32_t *X, const uint32_t *pfx, uint32_t *out);       /* 4 lanes, ARMv8 SHA-256 instructions */
void pz_h160_kernel_neon2(const uint32_t *X, const uint32_t *pfx, uint32_t *out);      /* 8 lanes */
void pz_sha256_blocks_arm(uint32_t state[8], const uint8_t *data, size_t nblocks);     /* scalar SHA-256 with those instructions */
#endif

#endif
