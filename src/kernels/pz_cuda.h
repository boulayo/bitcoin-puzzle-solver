/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_cuda.h - lets the OpenCL C kernels (pz_*.cl) compile as CUDA C++ with NVRTC.
 * Prepended by tools/gen_ptx.py; the PTX it produces is embedded in the program and JIT-compiled by the NVIDIA
 * driver for the GPU at hand, so neither the CUDA toolkit nor NVRTC is needed at run time. */
#define PZ_CUDA 1

typedef unsigned int uint;
typedef unsigned long long ulong;
typedef unsigned char uchar;
typedef long long s64;                  /* signed 64-bit (OpenCL: long) */
typedef unsigned short ushort;

#ifndef PZ_MINB
#define PZ_MINB 1
#endif
#define __kernel extern "C" __global__ __launch_bounds__(PZ_BLOCK, PZ_MINB)
#define __global
#define __constant                          /* kernel pointer arguments: ordinary (read-only) global memory */
#define PZ_CONST_TABLE __constant__ const   /* file-scope tables: the constant cache */
#define inline __device__ __forceinline__
#define PZ_NOINLINE __device__ __noinline__

#define get_global_id(d) (blockIdx.x * blockDim.x + threadIdx.x)
#define rotate(x, n) __funnelshift_l((x), (x), (n))
#define mul_hi(a, b) __umulhi((a), (b))
#define atomic_inc(p) atomicAdd((p), 1u)
#define atomic_or(p, v) atomicOr((p), (v))

/* PTX carry-chain instructions used by the PZ_FAST_FE field arithmetic (one instruction per statement; the carry flag
 * travels between consecutive statements). tests/mock_cuda/pz_host.h defines the same macros in C. */
#define ADD_CC(d, a, b)       asm volatile("add.cc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define ADDC_CC(d, a, b)      asm volatile("addc.cc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define ADDC(d, a, b)         asm volatile("addc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define SUB_CC(d, a, b)       asm volatile("sub.cc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define SUBC_CC(d, a, b)      asm volatile("subc.cc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define SUBC(d, a, b)         asm volatile("subc.u32 %0, %1, %2;" : "=r"(d) : "r"(a), "r"(b))
#define MADLO_CC(d, a, b, c)  asm volatile("mad.lo.cc.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(b), "r"(c))
#define MADLOC_CC(d, a, b, c) asm volatile("madc.lo.cc.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(b), "r"(c))
#define MADHI_CC(d, a, b, c)  asm volatile("mad.hi.cc.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(b), "r"(c))
#define MADHIC_CC(d, a, b, c) asm volatile("madc.hi.cc.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(b), "r"(c))
#define MADHIC(d, a, b, c)    asm volatile("madc.hi.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(b), "r"(c))
