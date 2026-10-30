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
#define PZ_BSWAP(x) __byte_perm((x), 0u, 0x0123u)
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

/* a + b on the FMA pipe: mad.lo with a multiplier of 1 that neither compiler can see (pz_one lives in the constant bank, which the host
 * could overwrite), so ptxas keeps the IMAD instead of folding it back into an IADD3. The linear kernel is bound by the ALU pipe, where
 * IADD3, LOP3 and SHF run, while the FMA pipe, where IMAD runs, is mostly idle: the hash moves part of its additions there (RESEARCH.md
 * section 5). The multiplier is read from c[3][0] as an operand, or held in one register where the addend is an immediate. */
__constant__ uint pz_one = 1;
__constant__ uint pz_zero = 0;
inline uint pz_addf(uint a, uint b)
{
    uint d;
    asm("mad.lo.u32 %0, %1, %2, %3;" : "=r"(d) : "r"(a), "r"(pz_one), "r"(b));
    return d;
}
#define PZ_ADDF(a, b) pz_addf((a), (b))
/* the carry, or the borrow, out of a chain as a 0/1 value: x*0 + 0 + CF on the FMA pipe, where addc.u32 d, 0, 0 is a SEL on the ALU
 * pipe and the borrow of subc.u32 d, 0, 0 needs an AND besides; x is any register already at hand (its value does not matter: the
 * zero comes from the constant bank as an operand, so the instruction needs no other register) */
#define ADDC_CF(d, x)         asm volatile("madc.lo.u32 %0, %1, %2, 0;" : "=r"(d) : "r"(x), "r"(pz_zero))
#define PZ_NORMALIZE_FMA 1    /* fe_normalize_fma: the selects of the normalization as predicated mad.lo (pz_common.cl) */

/* trailing zeros of a non-zero x: bit reversal and bfind.shiftamt (the leading zeros in one instruction, where __clz takes two) */
inline int pz_ctz(uint x)
{
    uint r;
    asm("brev.b32 %0, %1;" : "=r"(r) : "r"(x));
    asm("bfind.shiftamt.u32 %0, %1;" : "=r"(r) : "r"(r));
    return (int)r;
}
#define PZ_CTZ(x) pz_ctz(x)
