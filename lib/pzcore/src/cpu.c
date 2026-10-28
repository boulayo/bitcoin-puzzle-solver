/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu.c - detection of the instruction-set extensions used by the fast paths. */
#include <pzcore/cpu.h>

#if defined(__x86_64__)
#include <cpuid.h>
/* ADX is CPUID leaf 7, EBX bit 19. Not __builtin_cpu_supports("adx"): clang (Apple clang too) rejects that feature name. */
static bool has_adx(void)
{
    unsigned a, b, c, d;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b >> 19 & 1);
}

bool pz_cpu_has_avx2(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") && has_adx();
}

bool pz_cpu_has_avx512(void)
{
    __builtin_cpu_init();
    return pz_cpu_has_avx2() && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
           __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq");
}

bool pz_cpu_has_ifma(void)
{
    if (!pz_cpu_has_avx512()) return false;
    return __builtin_cpu_supports("avx512ifma");
}

/* The SHA extensions are CPUID leaf 7, EBX bit 29 (no __builtin_cpu_supports name for them in older compilers). The code that uses them is
 * built with AVX2, SSSE3 and SSE4.1 as well, and runs next to the AVX2 lanes. */
bool pz_cpu_has_shani(void)
{
    unsigned a, b, c, d;
    __builtin_cpu_init();
    return pz_cpu_has_avx2() && __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1") &&
           __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b >> 29 & 1);
}
#else
bool pz_cpu_has_avx2(void) { return false; }
bool pz_cpu_has_avx512(void) { return false; }
bool pz_cpu_has_ifma(void) { return false; }
bool pz_cpu_has_shani(void) { return false; }
#endif

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
bool pz_cpu_has_sha2(void) { return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0; }
#elif defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
bool pz_cpu_has_sha2(void)
{
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &v, &n, NULL, 0) == 0 && v;
}
#else
bool pz_cpu_has_sha2(void) { return false; }
#endif
