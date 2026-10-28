/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu.h - run-time detection of the instruction-set extensions that the fast paths of pzcore need. Every function is safe to
 * call from any thread at any time, needs no initialisation and returns false on architectures where the extension does not exist. */
#ifndef PZCORE_CPU_H
#define PZCORE_CPU_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

/* x86-64: AVX2, BMI2 and ADX (the MULX/ADX field multiplication and the 8-lane code). The operating system must also save the
 * AVX registers. */
bool pz_cpu_has_avx2(void);
/* x86-64: the above and AVX-512 F, VL, BW and DQ (the 16-lane code). */
bool pz_cpu_has_avx512(void);
/* x86-64: the above and AVX-512 IFMA (Ice Lake, Zen 4 and later). */
bool pz_cpu_has_ifma(void);
/* x86-64: AVX2 (as above) and the SHA extensions (sha256rnds2, sha256msg1, sha256msg2: AMD Zen 2 and later, Intel Ice Lake and later). */
bool pz_cpu_has_shani(void);
/* aarch64: the ARMv8 SHA-256 instructions. */
bool pz_cpu_has_sha2(void);

PZ_END_DECLS

#endif
