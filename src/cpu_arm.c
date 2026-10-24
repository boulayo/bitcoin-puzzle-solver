/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu_arm.c - detection of the aarch64 instruction-set extensions used by the engines (false on every other architecture). */
#include "cpu_engine.h"

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
bool cpu_has_sha2(void) { return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0; }
#elif defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
bool cpu_has_sha2(void)
{
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &v, &n, NULL, 0) == 0 && v;
}
#else
bool cpu_has_sha2(void) { return false; }
#endif
