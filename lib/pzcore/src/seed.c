/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seed.c - the seed generator (see seed.h): the CPU's generator, the timer, the operating system's generator and their mixing.
 * The test build (lib/pzcore/tests/t_seed.c includes this file with PZ_SEED_TEST defined) can make the OS generator fail; the library
 * cannot. */
#include <pzcore/seed.h>
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include "bytes.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/resource.h>
#include <sys/stat.h>
#endif
#if defined(__linux__)
#include <poll.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/sysmacros.h>
#endif
#if defined(__APPLE__)
#include <sys/random.h>
#include <sys/sysctl.h>
#endif
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define CPU_WORDS   4              /* 256 bits of the CPU's generator */
#define TIMER_WORDS 256
#define OS_BYTES    64             /* two calls of 32 */
#define IKM_MAX     (3 * 5 + 8 * CPU_WORDS + 8 * TIMER_WORDS + OS_BYTES)

static bool why_set(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && n) vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return false;
}

/* ---- cpu: RDSEED / RDRAND, RNDRRS / RNDR ----
 * Kind 0 is the conditioned output of the entropy source (RDSEED, RNDRRS), kind 1 the DRBG (RDRAND, RNDR). Only the 64-bit forms are
 * used (the 16- and 32-bit RDSEED of some Zen 5 parts return 0 with the carry set, AMD-SB-7055). */
static bool cpu_has(int k)
{
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (k == 0) return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b >> 18 & 1);
    return __get_cpuid(1, &a, &b, &c, &d) && (c >> 30 & 1);
#elif defined(__aarch64__) && defined(__linux__)
    (void)k;
    return (getauxval(AT_HWCAP2) & (1UL << 16)) != 0;    /* HWCAP2_RNG */
#elif defined(__aarch64__) && defined(__APPLE__)
    int v = 0;
    size_t n = sizeof v;
    (void)k;
    return sysctlbyname("hw.optional.arm.FEAT_RNG", &v, &n, NULL, 0) == 0 && v;
#else
    (void)k;
    return false;                                      /* Windows on arm64 has no documented query */
#endif
}

/* one attempt at one word; the success flag (the carry on x86-64, NZCV on arm64) */
static bool cpu_try(int k, uint64_t *v)
{
#if defined(__x86_64__)
    unsigned char ok;
    if (k == 0) __asm__ __volatile__("rdseed %0\n\tsetc %1" : "=r"(*v), "=qm"(ok) : : "cc");
    else __asm__ __volatile__("rdrand %0\n\tsetc %1" : "=r"(*v), "=qm"(ok) : : "cc");
    return ok;
#elif defined(__aarch64__)
    uint32_t ok;     /* by their system-register encodings, which assemblers without +rng accept; Z is set on failure */
    if (k == 0) __asm__ __volatile__("mrs %0, s3_3_c2_c4_1\n\tcset %w1, ne" : "=r"(*v), "=r"(ok) : : "cc");
    else __asm__ __volatile__("mrs %0, s3_3_c2_c4_0\n\tcset %w1, ne" : "=r"(*v), "=r"(ok) : : "cc");
    return ok;
#else
    (void)k;
    *v = 0;
    return false;
#endif
}

/* The words the CPU gives, at most CPU_WORDS: of kind 0, and of kind 1 for those that kind 0 did not give (RDSEED runs dry under
 * load). Ten attempts a word is Intel's advice for RDRAND. */
static size_t cpu_fill(uint64_t w[CPU_WORDS])
{
    size_t n = 0;
    for (int k = 0; k < 2; k++) {
        if (!cpu_has(k)) continue;
        for (int t = 0; t < 10 * CPU_WORDS && n < CPU_WORDS; t++) n += cpu_try(k, &w[n]);
    }
    return n;
}

/* ---- timer ---- */

static inline uint64_t timer_now(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#elif defined(_WIN32)
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

/* The readings, a hash apart: what differs from one run to the next is how long the hashes took. */
static void timer_fill(uint64_t t[TIMER_WORDS])
{
    uint8_t h[32] = {0};
    for (int i = 0; i < TIMER_WORDS; i++) {
        pz_sha256(h, sizeof h, h);
        t[i] = timer_now();
    }
}

/* ---- os: the operating system's generator ---- */

#if defined(_WIN32)
#define OS_NAME "BCryptGenRandom"
#elif defined(__linux__)
#define OS_NAME "getrandom"
#else
#define OS_NAME "getentropy"
#endif

/* one request: the bytes it gave (at most n), or -1 with errno */
static long os_sys(uint8_t *p, size_t n)
{
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return (long)n;
    errno = EIO;
    return -1;
#elif defined(__linux__)
    return (long)getrandom(p, n, 0);           /* blocks until the kernel's pool is initialised */
#else
    return getentropy(p, n) == 0 ? (long)n : -1;
#endif
}

#ifdef PZ_SEED_TEST
enum { OSF_FAIL = 1, OSF_SHORT, OSF_REPEAT, OSF_ENOSYS };
static int g_os_fault;
static long os_request(uint8_t *p, size_t n)
{
    switch (g_os_fault) {
    case OSF_FAIL: errno = EIO; return -1;
    case OSF_SHORT: return 0;                                                     /* success, and nothing given */
    case OSF_REPEAT: for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 167 + 13); return (long)n;   /* the same block every time */
    case OSF_ENOSYS: errno = ENOSYS; return -1;                                   /* a kernel older than 3.17 */
    default: return os_sys(p, n);
    }
}
#else
#define os_request os_sys
#endif

#if defined(__linux__)
/* the old kernels' device, only if it is the real one (1:9), and only once /dev/random is readable: the pool is initialised */
static bool urandom_fill(uint8_t *p, size_t n, char *err, size_t errlen)
{
    int fd = open("/dev/random", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct pollfd pf = {fd, POLLIN, 0};
        while (poll(&pf, 1, -1) < 0 && errno == EINTR) {}
        close(fd);
    }
    struct stat st;
    if ((fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOCTTY)) < 0) return why_set(err, errlen, "no getrandom and no /dev/urandom");
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 1 || minor(st.st_rdev) != 9) {
        close(fd);
        return why_set(err, errlen, "/dev/urandom is not the kernel's device");
    }
    size_t got = 0;
    while (got < n) {
        const long k = read(fd, p + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    return got == n || why_set(err, errlen, "short read from /dev/urandom: %zu of %zu bytes", got, n);
}
#endif

static bool os_fill(uint8_t *p, size_t n, char *err, size_t errlen)
{
    for (size_t got = 0; got < n;) {
        const long r = os_request(p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
#if defined(__linux__)
        if (r < 0 && errno == ENOSYS && got == 0) return urandom_fill(p, n, err, errlen);
#endif
        if (r < 0) return why_set(err, errlen, OS_NAME " failed (errno %d)", errno);
        if (r == 0) return why_set(err, errlen, OS_NAME " gave no bytes");
        got += (size_t)r;
    }
    return true;
}

/* ---- the mixing ---- */

typedef struct { const void *bytes; size_t len; } seed_input;

/* HKDF-SHA256 over the inputs, each behind its index and its length (seed.h); together they hold at most IKM_MAX bytes */
static void mix(uint8_t seed[PZ_SEED_LEN], const seed_input *in, int n)
{
    static const uint8_t salt[] = "pzseed2", info[] = "seed";
    uint8_t ikm[IKM_MAX];
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        ikm[at] = (uint8_t)i;
        store_be32(ikm + at + 1, (uint32_t)in[i].len);
        memcpy(ikm + at + 5, in[i].bytes, in[i].len);
        at += 5 + in[i].len;
    }
    (void)pz_hkdf_sha256(salt, sizeof salt - 1, ikm, at, info, sizeof info - 1, seed, PZ_SEED_LEN);
    pz_secure_zero(ikm, sizeof ikm);
}

bool pz_seed_generate(uint8_t seed[PZ_SEED_LEN], char *err, size_t errlen)
{
    uint64_t cpu[CPU_WORDS], timer[TIMER_WORDS];
    uint8_t os[OS_BYTES];
    const size_t h = OS_BYTES / 2;
    memset(seed, 0, PZ_SEED_LEN);
    /* the CPU's generator first, while no other secret is in the process: a malicious instruction has nothing to adapt to; the OS last,
     * so that its output spends the least time in memory */
    const size_t ncpu = cpu_fill(cpu);
    timer_fill(timer);
    bool ok = os_fill(os, h, err, errlen) && os_fill(os + h, h, err, errlen);        /* two calls: a generator stuck on one block is caught */
    if (ok && !memcmp(os, os + h, h)) ok = why_set(err, errlen, OS_NAME " gave the same %zu bytes twice", h);
    if (ok) mix(seed, (const seed_input[]){{cpu, 8 * ncpu}, {timer, sizeof timer}, {os, sizeof os}}, 3);
    pz_secure_zero(cpu, sizeof cpu);
    pz_secure_zero(timer, sizeof timer);
    pz_secure_zero(os, sizeof os);
    return ok;
}

bool pz_seed_harden(void)
{
#if defined(_WIN32)
    return false;
#else
    const struct rlimit none = {0, 0};
    bool ok = setrlimit(RLIMIT_CORE, &none) == 0;
#if defined(__linux__)
    ok = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0 && ok;    /* also stops ptrace and /proc/PID/mem from processes of the same user */
#endif
    return ok;
#endif
}
