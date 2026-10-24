/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* defs.h - what every pzcore header needs: fixed-width types, the 128-bit integer type, a few attribute macros and the
 * extern "C" brackets. pzcore is written for GCC and Clang on 64-bit targets (it uses unsigned __int128, GNU vector types
 * and, on x86-64, inline assembly), so it builds with MinGW-w64 but not with MSVC. */
#ifndef PZCORE_DEFS_H
#define PZCORE_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef __SIZEOF_INT128__
#error "pzcore needs a compiler with unsigned __int128 (GCC or Clang on a 64-bit target)"
#endif

#ifdef __cplusplus
#define PZ_BEGIN_DECLS extern "C" {
#define PZ_END_DECLS }
#else
#define PZ_BEGIN_DECLS
#define PZ_END_DECLS
#endif

/* version of the library interface */
#define PZCORE_VERSION_MAJOR 1
#define PZCORE_VERSION_MINOR 0
#define PZCORE_VERSION_PATCH 0

__extension__ typedef unsigned __int128 pz_u128;

#define PZ_LIKELY(x)   __builtin_expect(!!(x), 1)
#define PZ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PZ_ALIGN(n)    __attribute__((aligned(n)))
#define PZ_UNUSED      __attribute__((unused))

/* Win64: the hot lane routines use the System V calling convention, so a call the compiler did not inline does not have to save
 * and restore xmm6-xmm15 (callee-saved in the Microsoft ABI) - worth ~20% in the SIMD-lane engines. Empty everywhere else. */
#if defined(_WIN32) && defined(__x86_64__)
#define PZ_VABI __attribute__((sysv_abi))
#else
#define PZ_VABI
#endif

#endif
