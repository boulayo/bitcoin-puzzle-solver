/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_host.h - compiles the OpenCL C kernels as plain C for the mock CUDA driver (tests only).
 * The work-item index comes from a global the mock sets before calling the kernel for each item. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef unsigned int uint;
typedef unsigned long long ulong;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef long long s64;
typedef struct { uint x, y, z, w; } uint4;   /* the vector type the kernels load jump distances as */

/* Several simulated devices run kernels at the same time (one thread each): the work-item index and the carry flag belong to the thread.
 * Where the compiler has no usable thread-local storage in a DLL (Windows), the driver runs one launch at a time instead. */
#ifdef _WIN32
#define PZ_MOCK_TLS
#else
#define PZ_MOCK_TLS _Thread_local
#endif
extern PZ_MOCK_TLS unsigned pz_mock_gid;

#define PZ_CUDA 1                         /* take the CUDA branches of the kernels (same as the PTX build) */
#define __kernel
#define __global
#define __constant
#define PZ_CONST_TABLE static const
#define PZ_NOINLINE static
#define inline static inline
#define get_global_id(d) (pz_mock_gid)
#define rotate(x, n) ((uint)(((uint)(x) << ((n) & 31)) | ((uint)(x) >> ((32 - ((n) & 31)) & 31))))
#define mul_hi(a, b) ((uint)(((ulong)(uint)(a) * (uint)(b)) >> 32))
#define atomic_inc(p) __atomic_fetch_add((p), 1u, __ATOMIC_RELAXED)
#define atomic_or(p, v) __atomic_fetch_or((p), (v), __ATOMIC_RELAXED)

/* C emulation of the PTX carry-chain instructions (semantics of the PTX ISA: CC.CF is the carry out of add/mad and the
 * borrow out of sub; the .cc-less forms read the flag but leave it unchanged). */
static PZ_MOCK_TLS uint pz_cf;
#define ADD_CC(d, a, b)       do { ulong s_ = (ulong)(uint)(a) + (uint)(b); (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define ADDC_CC(d, a, b)      do { ulong s_ = (ulong)(uint)(a) + (uint)(b) + pz_cf; (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define ADDC(d, a, b)         do { (d) = (uint)((ulong)(uint)(a) + (uint)(b) + pz_cf); } while (0)
#define SUB_CC(d, a, b)       do { ulong s_ = (ulong)(uint)(a) - (uint)(b); (d) = (uint)s_; pz_cf = (uint)(s_ >> 63); } while (0)
#define SUBC_CC(d, a, b)      do { ulong s_ = (ulong)(uint)(a) - (uint)(b) - pz_cf; (d) = (uint)s_; pz_cf = (uint)(s_ >> 63); } while (0)
#define SUBC(d, a, b)         do { (d) = (uint)((ulong)(uint)(a) - (uint)(b) - pz_cf); } while (0)
#define MADLO_CC(d, a, b, c)  do { ulong s_ = (ulong)(uint)((uint)(a) * (uint)(b)) + (uint)(c); (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define MADLOC_CC(d, a, b, c) do { ulong s_ = (ulong)(uint)((uint)(a) * (uint)(b)) + (uint)(c) + pz_cf; (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define MADHI_CC(d, a, b, c)  do { ulong s_ = (((ulong)(uint)(a) * (uint)(b)) >> 32) + (uint)(c); (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define MADHIC_CC(d, a, b, c) do { ulong s_ = (((ulong)(uint)(a) * (uint)(b)) >> 32) + (uint)(c) + pz_cf; (d) = (uint)s_; pz_cf = (uint)(s_ >> 32); } while (0)
#define MADHIC(d, a, b, c)    do { (d) = (uint)((((ulong)(uint)(a) * (uint)(b)) >> 32) + (uint)(c) + pz_cf); } while (0)
