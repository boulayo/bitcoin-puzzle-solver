/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seed.c - the seed generator (see seed.h): the sources, their health tests, the locked memory and the mixing.
 * The test build (lib/pzcore/tests/t_seed.c includes this file with PZ_SEED_TEST defined) adds the fault injection of g_fault; the
 * library has none of it. */
#include <pzcore/seed.h>
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include "bytes.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#endif
#if defined(__linux__)
#include <poll.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/sysmacros.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/random.h>
#include <sys/sysctl.h>
#endif
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define OS_BYTES       1024        /* two calls of 512 */
#define HW_SEED_WORDS  128         /* RDSEED / RNDRRS: conditioned output of the entropy source */
#define HW_DRBG_WORDS  1024        /* RDRAND / RNDR: DRBG output; 1024 words span more than one reseed (Intel: at most 511 outputs of 128 bits) */
#define TPM_CHUNK      32
#define TPM_CALLS      8
#define TPM_RESP       (12 + TPM_CHUNK)
#define JIT_SAMPLES    4096        /* per round */
#define JIT_MEM        (1u << 20)  /* larger than L1: the walk meets the latencies of L2 and L3 */
#define JIT_MAX_WALK   (1u << 14)  /* bounds the time of a sample when the timer is coarse */
#define BITS_NEEDED    128

/* The cutoffs of the health tests, from SP 800-90B 4.4 for the claimed min-entropy H and false-alarm probability alpha (t_seed.c
 * recomputes each one). Output that claims full entropy (os, cpu-rng, tpm), H = 8 per byte, alpha = 2^-40: repetition count
 * 1 + ceil(40 / 8) = 6; adaptive proportion 26 in a window of 1024; no byte value 29 times in 1024 bytes; a monobit |z| at most
 * 7.1436 (z^2 = 51.03). Jitter, H = 0.5 per 8-bit sample, alpha = 2^-30: repetition count 61, adaptive proportion 810 in 1024. */
enum { B_RCT = 6, B_APT = 26, B_FREQ = 29, B_Z2X100 = 5103, J_RCT = 61, J_APT = 810, APT_W = 1024 };

#ifdef PZ_SEED_TEST
enum { OSF_FAIL = 1, OSF_SHORT, OSF_ZERO, OSF_REPEAT, OSF_ENOSYS, OSF_HALFZERO, OSF_ATTACKER };
enum { HWF_ALLONES = 1, HWF_NOCARRY, HWF_REPEAT, HWF_ZERO, HWF_HALFZERO, HWF_SEEDFAIL, HWF_ATTACKER, HWF_ABSENT, HWF_GOOD };
enum { JF_CONST = 1, JF_BACK, JF_STEP, JF_ALTERNATE, JF_COARSE, JF_LOWENT, JF_ATTACKER, JF_FIRSTBAD };
static struct {
    int os, hw, jit;
    const char *tpm_path;          /* a file that stands for the TPM device */
    uint32_t walk;                 /* the jitter walk, without calibration */
    uint64_t key;                  /* the attacker's sources are a public function of it */
    uint64_t n, jn, t, t0;         /* the state of the simulated generators and timer */
    unsigned round;
} g_fault;
static uint64_t fault_rnd(uint64_t i)          /* splitmix64 of key + i */
{
    uint64_t z = g_fault.key + (i + 1) * 0x9e3779b97f4a7c15ULL;
    z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ z >> 27) * 0x94d049bb133111ebULL;
    return z ^ z >> 31;
}
#endif

static bool why_set(char *why, size_t wn, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, wn, fmt, ap);
    va_end(ap);
    return false;
}

static void note(pz_seed_source *s, pz_seed_state st, unsigned bits, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->reason, sizeof s->reason, fmt, ap);
    va_end(ap);
    s->state = st;
    s->bits = st == PZ_SEED_USED ? bits : 0;
}

static double now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
#endif
}

/* ---- memory ---- */

/* Pages from the OS; the secret ones are locked in RAM, left out of core dumps and wiped in a forked child. A lock that fails (a low
 * RLIMIT_MEMLOCK) is reported, not fatal. */
static uint8_t *map_pages(size_t n, bool secret, bool *locked)
{
    *locked = false;
#if defined(_WIN32)
    void *p = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p && secret && !(*locked = VirtualLock(p, n) != 0)) {
        SIZE_T mn, mx;                     /* the default working set is small: grow it and try again */
        const HANDLE h = GetCurrentProcess();
        if (GetProcessWorkingSetSize(h, &mn, &mx) && SetProcessWorkingSetSize(h, mn + n, mx + n)) *locked = VirtualLock(p, n) != 0;
    }
    return p;
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (secret) {
#if defined(MADV_DONTDUMP)
        madvise(p, n, MADV_DONTDUMP);
#elif defined(MADV_NOCORE)
        madvise(p, n, MADV_NOCORE);
#endif
#if defined(MADV_WIPEONFORK)
        madvise(p, n, MADV_WIPEONFORK);
#endif
        *locked = mlock(p, n) == 0;
    }
    return p;
#endif
}

static void unmap_pages(uint8_t *p, size_t n, bool locked)
{
    if (!p) return;
#if defined(_WIN32)
    if (locked) VirtualUnlock(p, n);
    VirtualFree(p, 0, MEM_RELEASE);
#else
    if (locked) munlock(p, n);
    munmap(p, n);
#endif
}

/* One locked region for every raw output of a generation, sized in advance (allocations cannot fail), wiped as a whole at the end. */
typedef struct { uint8_t *base; size_t size, used; bool locked; } arena;

static void *arena_alloc(arena *a, size_t n)
{
    void *p = a->base + a->used;
    a->used += (n + 63) & ~(size_t)63;
    return p;
}

/* the raw output of a source that is used, in the arena */
typedef struct { const uint8_t *data; size_t len; } chunk;

/* ---- health tests of output that claims full entropy (os, cpu-rng, tpm) ----
 * The repetition count test on the bytes; in every window of 1024 bytes the adaptive proportion test and the count of every byte
 * value (the adaptive proportion test only counts the first value of its window, and misses half of each word stuck at zero); the
 * monobit test over all. They catch a stuck or unfilled buffer and broken hardware, never a weak generator with good-looking output. */
static bool bytes_ok(const uint8_t *p, size_t n, char *why, size_t wn)
{
    unsigned run = 0;
    uint64_t ones = 0;
    for (size_t i = 0; i < n; i++) {
        run = i && p[i] == p[i - 1] ? run + 1 : 1;
        if (run >= B_RCT) return why_set(why, wn, "repetition count test (a byte %u times in a row)", run);
        ones += (unsigned)__builtin_popcount(p[i]);
    }
    for (size_t w = 0; w + APT_W <= n; w += APT_W) {
        unsigned cnt[256] = {0}, same = 0, most = 0;
        for (size_t i = w; i < w + APT_W; i++) {
            same += p[i] == p[w];
            if (++cnt[p[i]] > most) most = cnt[p[i]];
        }
        if (same >= B_APT) return why_set(why, wn, "adaptive proportion test (%u of 1024 bytes equal the first, cutoff %d)", same, B_APT);
        if (most >= B_FREQ) return why_set(why, wn, "byte frequency test (a value %u times in 1024 bytes, cutoff %d)", most, B_FREQ);
    }
    const int64_t d = 2 * (int64_t)ones - 8 * (int64_t)n;   /* |z| = |d| / sqrt(8n) */
    if ((uint64_t)(d * d) * 100 > (uint64_t)B_Z2X100 * 8 * n)
        return why_set(why, wn, "monobit test (%.1f%% of the bits are ones)", 100.0 * (double)ones / (8.0 * (double)n));
    return true;
}

/* ---- cpu-rng: RDSEED / RDRAND, RNDRRS / RNDR ----
 * Kind 0 is the conditioned output of the entropy source (RDSEED, RNDRRS), kind 1 the DRBG (RDRAND, RNDR), the fallback when kind 0
 * gives nothing. Only the 64-bit forms are used (the 16- and 32-bit RDSEED of some Zen 5 parts return 0 with the carry set,
 * AMD-SB-7055). */
#if defined(__aarch64__)
static const char *const hw_name[2] = {"RNDRRS", "RNDR"};
static const unsigned hw_bits[2] = {128, 64};     /* RNDR's reseed interval is not specified */
#else
static const char *const hw_name[2] = {"RDSEED", "RDRAND"};
static const unsigned hw_bits[2] = {128, 128};
#endif
static const unsigned hw_retries[2] = {1000, 10};  /* RDSEED runs dry under load; for RDRAND, 10 retries is Intel's advice */

static bool hw_has(int k)
{
#ifdef PZ_SEED_TEST
    if (g_fault.hw) return g_fault.hw != HWF_ABSENT;
#endif
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
static bool hw_try(int k, uint64_t *v)
{
#ifdef PZ_SEED_TEST
    if (g_fault.hw) {
        switch (g_fault.hw) {
        case HWF_ALLONES: *v = ~(uint64_t)0; return true;
        case HWF_NOCARRY: *v = 0; return false;
        case HWF_REPEAT: *v = 0x9e3779b97f4a7c15ULL ^ (g_fault.n++ & 3); return true;
        case HWF_ZERO: *v = 0; return true;
        case HWF_SEEDFAIL: if (k == 0) { *v = 0; return false; } break;
        default: break;
        }
        *v = fault_rnd(g_fault.n++);       /* attacker-known (HWF_ATTACKER), and the working path of the other faults */
        if (g_fault.hw == HWF_HALFZERO) *v &= 0xffffffffULL;
        return true;
    }
#endif
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

static void hw_pause(void)
{
#if defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static void src_cpu(pz_seed_source *s, chunk *c, arena *a)
{
    uint64_t *w = arena_alloc(a, HW_DRBG_WORDS * 8);
    char why[96], tried[96] = "";
    unsigned long retries = 0;
    snprintf(s->method, sizeof s->method, "none");
    for (int k = 0; k < 2; k++) {
        if (!hw_has(k)) continue;
        const size_t n = k ? HW_DRBG_WORDS : HW_SEED_WORDS;
        size_t i = 0;
        for (unsigned t = 0; i < n;) {             /* t: failures in a row on this word */
            if (hw_try(k, &w[i])) i++, t = 0;
            else if (t++ < hw_retries[k]) retries++, hw_pause();
            else break;
        }
        snprintf(s->method, sizeof s->method, "%s", hw_name[k]);
        if (i < n) {
            snprintf(tried + strlen(tried), sizeof tried - strlen(tried), "%s%s gave no word after %u retries", *tried ? "; " : "", hw_name[k], hw_retries[k]);
            continue;
        }
        size_t ones = 0, zeros = 0, dups = 0;
        for (i = 0; i < n; i++) {
            ones += w[i] == ~(uint64_t)0;
            zeros += w[i] == 0;
            for (size_t j = 0; j < i; j++)
                if (w[i] == w[j]) {
                    dups++;
                    break;
                }
        }
        if (ones) note(s, PZ_SEED_EXCLUDED, 0, "%zu of %zu words all ones: the AMD RDRAND erratum, or a stuck generator", ones, n);
        else if (zeros) note(s, PZ_SEED_EXCLUDED, 0, "%zu of %zu words zero: a broken generator", zeros, n);
        else if (dups) note(s, PZ_SEED_EXCLUDED, 0, "%zu repeated words in %zu", dups, n);
        else if (!bytes_ok((const uint8_t *)w, n * 8, why, sizeof why)) note(s, PZ_SEED_EXCLUDED, 0, "%s", why);
        else {
            note(s, PZ_SEED_USED, hw_bits[k], "%zu words, %lu retries%s%s; health tests passed", n, retries, *tried ? "; " : "", tried);
            c->data = (const uint8_t *)w;
            c->len = n * 8;
            return;
        }
        pz_secure_zero(w, HW_DRBG_WORDS * 8);
        return;
    }
    pz_secure_zero(w, HW_DRBG_WORDS * 8);
    if (*tried) note(s, PZ_SEED_EXCLUDED, 0, "%s", tried);
    else note(s, PZ_SEED_ABSENT, 0, "no hardware generator on this CPU, or none this system can detect");
}

/* ---- tpm: TPM2_GetRandom (TPM 2.0 part 3, 16.1) ---- */

/* tag TPM_ST_NO_SESSIONS, size 12, TPM_CC_GetRandom, bytesRequested */
static const uint8_t tpm_cmd[12] = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x7b, 0, TPM_CHUNK};

/* the response: tag, size, response code, then the 2-byte size and the bytes of a TPM2B_DIGEST */
static bool tpm_parse(const uint8_t *r, size_t n, char *why, size_t wn)
{
    if (n < 10 || r[0] != 0x80 || r[1] != 0x01 || load_be32(r + 2) != n) return why_set(why, wn, "malformed response (%zu bytes)", n);
    if (load_be32(r + 6)) return why_set(why, wn, "response code 0x%x", (unsigned)load_be32(r + 6));
    if (n != TPM_RESP || r[10] != 0 || r[11] != TPM_CHUNK) return why_set(why, wn, "%zu bytes where %d were asked", n < 12 ? 0 : n - 12, TPM_CHUNK);
    return true;
}

#if defined(_WIN32)
typedef UINT32(WINAPI *tbs_create_fn)(const void *, void **);
typedef UINT32(WINAPI *tbs_submit_fn)(void *, UINT32, UINT32, const BYTE *, UINT32, BYTE *, UINT32 *);
typedef UINT32(WINAPI *tbs_close_fn)(void *);
#endif
typedef struct {
    int fd;                                /* the Linux device (in the tests, a file that stands for one) */
#if defined(_WIN32)
    HMODULE dll;
    void *ctx;
    tbs_submit_fn submit;
    tbs_close_fn close;
#endif
} tpm_dev;

#if defined(_WIN32)
static bool tbs_open(tpm_dev *t, char *method, size_t mn, char *why, size_t wn)
{
    const struct { UINT32 version, flags; } params = {2, 1u << 2};     /* TBS_CONTEXT_PARAMS2 with includeTpm20 */
    snprintf(method, mn, "TBS");
    if (!(t->dll = LoadLibraryExA("tbs.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))) return why_set(why, wn, "no tbs.dll");
    const tbs_create_fn create = (tbs_create_fn)(void *)GetProcAddress(t->dll, "Tbsi_Context_Create");
    t->submit = (tbs_submit_fn)(void *)GetProcAddress(t->dll, "Tbsip_Submit_Command");
    t->close = (tbs_close_fn)(void *)GetProcAddress(t->dll, "Tbsip_Context_Close");
    const UINT32 rc = create && t->submit && t->close ? create(&params, &t->ctx) : 1;
    if (rc == 0) return true;
    t->ctx = NULL;
    return why_set(why, wn, "no TPM 2.0 (TBS 0x%x)", (unsigned)rc);
}
#endif

static bool tpm_open(tpm_dev *t, char *method, size_t mn, char *why, size_t wn)
{
    const char *path[2] = {"/dev/tpmrm0", "/dev/tpm0"};     /* the kernel's resource manager first; /dev/tpm0 is busy when a daemon holds it */
    memset(t, 0, sizeof *t);
    t->fd = -1;
#ifdef PZ_SEED_TEST
    if (g_fault.tpm_path) path[0] = path[1] = g_fault.tpm_path;
#endif
#if defined(_WIN32)
#ifdef PZ_SEED_TEST
    if (!g_fault.tpm_path)
#endif
        return tbs_open(t, method, mn, why, wn);
#endif
    int err = ENOENT;
    for (int i = 0; i < 2; i++) {
        if ((t->fd = open(path[i], O_RDWR | O_BINARY | O_CLOEXEC)) >= 0) {
            snprintf(method, mn, "%s", path[i]);
            return true;
        }
        if (errno != ENOENT) err = errno;
    }
    if (err == ENOENT) return why_set(why, wn, "no %s or %s", path[0], path[1]);
    if (err == EACCES) return why_set(why, wn, "no permission to %s (the tss group)", path[0]);
    return why_set(why, wn, "cannot open %s (errno %d)", path[0], err);
}

/* one command and its response: the Linux devices take a whole command per write and give the whole response to one read */
static bool tpm_xfer(tpm_dev *t, uint8_t *r, size_t *n, char *why, size_t wn)
{
#if defined(_WIN32)
    if (t->ctx) {
        UINT32 len = (UINT32)*n;
        const UINT32 rc = t->submit(t->ctx, 0, 200, tpm_cmd, sizeof tpm_cmd, r, &len);   /* locality 0, normal priority */
        *n = len;
        return rc == 0 || why_set(why, wn, "TBS error 0x%x", (unsigned)rc);
    }
#endif
    if (write(t->fd, tpm_cmd, sizeof tpm_cmd) != (long)sizeof tpm_cmd) return why_set(why, wn, "write to the TPM failed (errno %d)", errno);
    const long got = read(t->fd, r, (unsigned)*n);
    if (got < 0) return why_set(why, wn, "read from the TPM failed (errno %d)", errno);
    *n = (size_t)got;
    return true;
}

static void tpm_close(tpm_dev *t)
{
    if (t->fd >= 0) close(t->fd);
#if defined(_WIN32)
    if (t->ctx) t->close(t->ctx);
    if (t->dll) FreeLibrary(t->dll);
#endif
}

static void src_tpm(pz_seed_source *s, chunk *c, arena *a)
{
    uint8_t *p = arena_alloc(a, TPM_CALLS * TPM_CHUNK), r[TPM_RESP];
    char why[96];
    tpm_dev t;
    if (!tpm_open(&t, s->method, sizeof s->method, why, sizeof why)) {
        tpm_close(&t);
        note(s, PZ_SEED_ABSENT, 0, "%s", why);
        return;
    }
    bool ok = true;
    for (int i = 0; i < TPM_CALLS && ok; i++) {
        size_t n = sizeof r;
        if ((ok = tpm_xfer(&t, r, &n, why, sizeof why) && tpm_parse(r, n, why, sizeof why))) memcpy(p + i * TPM_CHUNK, r + 12, TPM_CHUNK);
    }
    tpm_close(&t);
    pz_secure_zero(r, sizeof r);
    for (int i = 0; ok && i < TPM_CALLS; i++)
        for (int j = 0; ok && j < i; j++)
            if (!memcmp(p + i * TPM_CHUNK, p + j * TPM_CHUNK, TPM_CHUNK)) ok = why_set(why, sizeof why, "two answers were the same");
    if (ok && !bytes_ok(p, TPM_CALLS * TPM_CHUNK, why, sizeof why)) ok = false;
    if (!ok) {
        pz_secure_zero(p, TPM_CALLS * TPM_CHUNK);
        note(s, PZ_SEED_EXCLUDED, 0, "%s", why);
        return;
    }
    note(s, PZ_SEED_USED, 128, "%d bytes in %d commands, distinct; health tests passed", TPM_CALLS * TPM_CHUNK, TPM_CALLS);
    c->data = p;
    c->len = TPM_CALLS * TPM_CHUNK;
}

/* ---- caller: the source of opts->extra ---- */

static void src_caller(pz_seed_source *s, chunk *c, arena *a, const pz_seed_opts *o)
{
    if (!o->extra) {
        note(s, PZ_SEED_ABSENT, 0, "none given");
        return;
    }
    uint8_t *p = arena_alloc(a, o->extra_len);
    char why[96] = "";
    snprintf(s->method, sizeof s->method, "callback");
    if (!o->extra(o->extra_arg, p, o->extra_len, why, sizeof why)) {
        note(s, PZ_SEED_EXCLUDED, 0, "its own health tests failed: %s", why);
    } else {
        size_t i = 1;
        while (i < o->extra_len && p[i] == p[0]) i++;    /* the one check that needs no knowledge of the format */
        if (i == o->extra_len) note(s, PZ_SEED_EXCLUDED, 0, "constant output");
    }
    if (s->state == PZ_SEED_EXCLUDED) {
        pz_secure_zero(p, o->extra_len);
        return;
    }
    unsigned bits = o->extra_bits < 256 ? o->extra_bits : 256;
    if (bits > 8 * o->extra_len) bits = (unsigned)(8 * o->extra_len);
    note(s, PZ_SEED_USED, bits, "%zu bytes, %u bits claimed", o->extra_len, o->extra_bits);
    c->data = p;
    c->len = o->extra_len;
}

/* ---- jitter: CPU timing jitter ----
 * One raw sample is the timer delta around a fixed workload: a public xorshift walk through JIT_MEM bytes, one cache line per step,
 * with a read-modify-write and two branches on the data at each step. The walk starts from a constant, so no entropy is claimed for
 * anything but the timing; unlike jitterentropy, its length never depends on earlier timer values, which would chain the samples. */
#if defined(__x86_64__)
#define TIMER_NAME "rdtsc"
static inline uint64_t timer_now(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}
#elif defined(__aarch64__)
#define TIMER_NAME "cntvct_el0"
static inline uint64_t timer_now(void)
{
    uint64_t v;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}
#elif defined(_WIN32)
#define TIMER_NAME "QueryPerformanceCounter"
static inline uint64_t timer_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
#elif defined(__APPLE__)
#define TIMER_NAME "mach_absolute_time"
static inline uint64_t timer_now(void) { return mach_absolute_time(); }
#else
#define TIMER_NAME "clock_gettime"
static inline uint64_t timer_now(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
#endif

#ifdef PZ_SEED_TEST
/* simulated timers; which: 0 at the start of a sample, 1 at its end, 2 for the checks of the timer */
static uint64_t fault_timer(int which)
{
    uint64_t *t = &g_fault.t;
    if (!*t) *t = 1u << 20;
    switch (g_fault.jit) {
    case JF_CONST: return 123456789;                              /* does not advance */
    case JF_BACK: return *t -= 5;                                 /* runs backwards */
    case JF_COARSE: *t += which == 2 ? 37 : which == 0 ? 50 : 1000 + (fault_rnd(g_fault.jn++) & 63); return *t & ~(uint64_t)0xfff;
    case JF_FIRSTBAD:                                             /* the first round has a constant delta, the next ones are real */
        if (which == 2 || g_fault.round != 1) return timer_now();
        return which == 0 ? (g_fault.t0 = timer_now()) : g_fault.t0 + 1000;
    default: break;
    }
    if (which == 2) return *t += 1;
    if (which == 0) return *t += 50;
    const uint64_t i = g_fault.jn++;
    switch (g_fault.jit) {
    case JF_STEP: return *t += 1000;                              /* a constant delta */
    case JF_ALTERNATE: return *t += 1000 + (i & 1);               /* two values in turn: passes the RCT and the APT */
    case JF_LOWENT: return *t += 1000 + 3 * (fault_rnd(i) & 1);   /* one random bit per sample */
    default: return *t += 900 + (fault_rnd(i) & 0x3ff);           /* JF_ATTACKER: random-looking and known to the attacker */
    }
}
#define jt_now(which) (g_fault.jit ? fault_timer(which) : timer_now())
#else
#define jt_now(which) timer_now()
#endif

typedef struct { volatile uint8_t *mem; uint32_t walk, x; } jitter;

static uint64_t jit_sample(jitter *j)
{
    volatile uint8_t *m = j->mem;
    uint32_t x = j->x;
    uint64_t acc = 0;
    const uint64_t t0 = jt_now(0);
    for (uint32_t i = 0; i < j->walk; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const size_t idx = ((size_t)x * 64u + (x >> 26)) & (JIT_MEM - 1);
        const uint8_t v = m[idx];
        if (v & 1) {                                   /* the empty asm keeps the branches branches */
            __asm__ __volatile__("");
            acc += v;
        } else {
            acc ^= (uint64_t)v << (i & 31);
        }
        if (x & 0x100) {
            __asm__ __volatile__("");
            acc = acc << 1 | acc >> 63;
        }
        m[idx] = (uint8_t)(v + 1 + (acc & 1));
    }
    const uint64_t t1 = jt_now(1);
    j->x = x;
    m[0] ^= (uint8_t)acc;
    return t1 - t0;
}

static bool jit_init(jitter *j, char *method, size_t mn, char *why, size_t wn)
{
    snprintf(method, mn, "%s", TIMER_NAME);
    uint64_t prev = jt_now(2), gran = UINT64_MAX;
    unsigned back = 0;
    for (int i = 0; i < 1000; i++) {
        const uint64_t t = jt_now(2);
        if (t < prev) back++;
        else if (t > prev && t - prev < gran) gran = t - prev;    /* equal reads of a coarse timer are not a granule */
        prev = t;
    }
    if (back > 3) return why_set(why, wn, "the timer is not monotonic (%u steps back in 1000 reads)", back);
    if (gran == UINT64_MAX) return why_set(why, wn, "the timer does not advance");
    bool locked;
    if (!(j->mem = map_pages(JIT_MEM, false, &locked))) return why_set(why, wn, "no memory for the walk");
    memset((void *)j->mem, 0x5a, JIT_MEM);    /* every page touched now, so that page faults do not distort the calibration */
    j->x = 0x2545f491u;
    /* the walk doubles until the median delta is at least 64 granules of the timer, so that small variations are resolved */
    for (j->walk = 16; j->walk < JIT_MAX_WALK; j->walk *= 2) {
#ifdef PZ_SEED_TEST
        if (g_fault.walk) {
            j->walk = g_fault.walk;
            break;
        }
#endif
        unsigned above = 0;
        for (int i = 0; i < 32; i++) jit_sample(j);
        for (int i = 0; i < 129; i++) above += jit_sample(j) >= 64 * gran;
        if (above >= 65) break;
    }
    snprintf(method, mn, "%s, walk %u", TIMER_NAME, j->walk);
    return true;
}

/* The live min-entropy estimate of the 8-bit samples (the low byte of each delta): the lower of SP 800-90B's most-common-value
 * estimate (6.3.1) and eight times its Markov estimate on their bits (6.3.3), which sees a predictable sequence that MCV misses. */
static double jit_live(const uint32_t *d, size_t n)
{
    unsigned cnt[256] = {0}, most = 0;
    double c[2][2] = {{0, 0}, {0, 0}}, ones = 0, t[2][2];
    int prev = -1;
    for (size_t i = 0; i < n; i++) {
        const unsigned s = d[i] & 255;
        if (++cnt[s] > most) most = cnt[s];
        for (int b = 7; b >= 0; b--) {
            const int bit = s >> b & 1;
            ones += bit;
            if (prev >= 0) c[prev][bit] += 1;
            prev = bit;
        }
    }
    double p = (double)most / (double)n;
    p += 2.576 * sqrt(p * (1 - p) / (double)(n - 1));       /* the upper bound of the 99% interval */
    const double mcv = -log2(p < 1 ? p : 1);
    for (int x = 0; x < 2; x++)
        for (int y = 0; y < 2; y++) t[x][y] = c[x][0] + c[x][1] > 0 ? c[x][y] / (c[x][0] + c[x][1]) : 0;
    const double p1 = ones / (8.0 * (double)n), p0 = 1 - p1;
#define LG(v) ((v) > 0 ? log2(v) : -INFINITY)
    const double path[6] = {           /* the most likely sequences of 128 bits */
        LG(p0) + 127 * LG(t[0][0]),           LG(p0) + 64 * LG(t[0][1]) + 63 * LG(t[1][0]), LG(p0) + LG(t[0][1]) + 126 * LG(t[1][1]),
        LG(p1) + LG(t[1][0]) + 126 * LG(t[0][0]), LG(p1) + 64 * LG(t[1][0]) + 63 * LG(t[0][1]), LG(p1) + 127 * LG(t[1][1])};
#undef LG
    double best = path[0];
    for (int i = 1; i < 6; i++) best = path[i] > best ? path[i] : best;
    const double markov = -best / 128 < 1 ? -best / 128 : 1;
    return mcv < 8 * markov ? mcv : 8 * markov;
}

/* One round of JIT_SAMPLES samples into d, with its health tests: the number of samples that are not stuck, or 0 and the reason.
 * A sample is stuck when its delta, or the second or third difference, is zero (jitterentropy's test). */
static unsigned jit_round(jitter *j, uint32_t *d, double *h, char *why, size_t wn)
{
#ifdef PZ_SEED_TEST
    g_fault.round++;
#endif
    for (int i = 0; i < 32; i++) jit_sample(j);               /* warm-up, not used */
    unsigned good = 0, stuck = 0, stuck_max = 0, run = 0, run_max = 0, same = 0, same_max = 0;
    uint32_t pd = 0, pd2 = 0;
    for (unsigned i = 0; i < JIT_SAMPLES; i++) {
        const uint32_t v = (uint32_t)jit_sample(j), d2 = v - pd, d3 = d2 - pd2;
        const bool st = v == 0 || d2 == 0 || d3 == 0;
        d[i] = v;
        pd = v;
        pd2 = d2;
        stuck = st ? stuck + 1 : 0;
        stuck_max = stuck > stuck_max ? stuck : stuck_max;
        good += !st && i >= 2;
        run = i && (v & 255) == (d[i - 1] & 255) ? run + 1 : 1;
        run_max = run > run_max ? run : run_max;
        same = i % APT_W ? same + ((v & 255) == (d[i - i % APT_W] & 255)) : 1;
        same_max = same > same_max ? same : same_max;
    }
    *h = jit_live(d, JIT_SAMPLES);
    if (stuck_max >= J_RCT || good < JIT_SAMPLES / 10)
        return why_set(why, wn, "too little variation: %u of %u samples not stuck, longest stuck run %u", good, JIT_SAMPLES, stuck_max);
    if (run_max >= J_RCT) return why_set(why, wn, "repetition count test (a value %u times in a row, cutoff %d)", run_max, J_RCT);
    if (same_max >= J_APT) return why_set(why, wn, "adaptive proportion test (%u of 1024, cutoff %d)", same_max, J_APT);
    if (!(*h >= 1.0)) return why_set(why, wn, "too little variation: live estimate %.3f bits per sample, below 1", *h);
    return good;
}

/* Rounds until one passes, while the other counted sources leave need bits to find and max_ms have not passed. A round that passes
 * has at least 410 samples that are not stuck, so 205 bits: one is always enough. */
static void src_jitter(pz_seed_source *s, chunk *c, arena *a, unsigned need, unsigned max_ms, pz_seed_report *r)
{
    const double t0 = now_ms();
    uint32_t *d = arena_alloc(a, JIT_SAMPLES * 4);
    char why[160];
    jitter j;
    if (!jit_init(&j, s->method, sizeof s->method, why, sizeof why)) {
        note(s, PZ_SEED_EXCLUDED, 0, "%s", why);
        return;
    }
    unsigned good;
    do r->jitter_rounds++;
    while (!(good = jit_round(&j, d, &r->jitter_h, why, sizeof why)) && need && now_ms() - t0 < max_ms);
    unmap_pages((uint8_t *)j.mem, JIT_MEM, false);
    if (!good) {
        pz_secure_zero(d, JIT_SAMPLES * 4);
        note(s, PZ_SEED_EXCLUDED, 0, "%s; %u round%s", why, r->jitter_rounds, r->jitter_rounds > 1 ? "s" : "");
        return;
    }
    note(s, PZ_SEED_USED, good / 2 < 256 ? good / 2 : 256, "%u of %d samples not stuck, 0.5 bit each; live estimate %.2f bits per sample; round %u",
         good, JIT_SAMPLES, r->jitter_h, r->jitter_rounds);
    c->data = (const uint8_t *)d;
    c->len = JIT_SAMPLES * 4;
}

/* ---- os: the operating system's generator ---- */

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
    if (n > 256) n = 256;                      /* getentropy gives at most 256 bytes per call */
    return getentropy(p, n) == 0 ? (long)n : -1;
#endif
}

#ifdef PZ_SEED_TEST
static long os_request(uint8_t *p, size_t n)
{
    switch (g_fault.os) {
    case OSF_FAIL: errno = EIO; return -1;
    case OSF_SHORT: if (g_fault.n++) return 0; return os_sys(p, n / 3);           /* part of the request, then nothing */
    case OSF_ZERO: memset(p, 0, n); return (long)n;                               /* success, and nothing written */
    case OSF_REPEAT: for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 167 + 13); return (long)n;
    case OSF_ENOSYS: errno = ENOSYS; return -1;                                   /* a kernel older than 3.17 */
    case OSF_ATTACKER: for (size_t i = 0; i < n; i++) p[i] = (uint8_t)fault_rnd(g_fault.n++); return (long)n;
    case OSF_HALFZERO: {
        const long r = os_sys(p, n);
        for (long i = 0; i < r; i += 2) p[i] = 0;
        return r;
    }
    default: return os_sys(p, n);
    }
}
#else
#define os_request os_sys
#endif

#if defined(__linux__)
/* the old kernels' device, only if it is the real one (1:9), and only once /dev/random is readable: the pool is initialised */
static bool urandom_fill(uint8_t *p, size_t n, char *why, size_t wn)
{
    int fd = open("/dev/random", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct pollfd pf = {fd, POLLIN, 0};
        while (poll(&pf, 1, -1) < 0 && errno == EINTR) {}
        close(fd);
    }
    struct stat st;
    if ((fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOCTTY)) < 0) return why_set(why, wn, "no getrandom and no /dev/urandom");
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 1 || minor(st.st_rdev) != 9) {
        close(fd);
        return why_set(why, wn, "/dev/urandom is not the kernel's device");
    }
    size_t got = 0;
    while (got < n) {
        const long k = read(fd, p + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    return got == n || why_set(why, wn, "short read from /dev/urandom: %zu of %zu bytes", got, n);
}
#endif

static bool os_fill(uint8_t *p, size_t n, char *method, size_t mn, char *why, size_t wn)
{
#if defined(_WIN32)
    snprintf(method, mn, "BCryptGenRandom");
#elif defined(__linux__)
    snprintf(method, mn, "getrandom");
#else
    snprintf(method, mn, "getentropy");
#endif
    for (size_t got = 0; got < n;) {
        const long r = os_request(p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
#if defined(__linux__)
        if (r < 0 && errno == ENOSYS && got == 0) {
            snprintf(method, mn, "/dev/urandom");
            return urandom_fill(p, n, why, wn);
        }
#endif
        if (r < 0) return why_set(why, wn, "%s failed (errno %d) after %zu of %zu bytes", method, errno, got, n);
        if (r == 0) return why_set(why, wn, "short read: %s gave 0 bytes after %zu of %zu", method, got, n);
        got += (size_t)r;
    }
    return true;
}

static void src_os(pz_seed_source *s, chunk *c, arena *a)
{
    uint8_t *p = arena_alloc(a, OS_BYTES);
    char why[128];
    const size_t h = OS_BYTES / 2;
    /* two calls, so that a generator that gives the same block every time is caught */
    if (!os_fill(p, h, s->method, sizeof s->method, why, sizeof why) || !os_fill(p + h, h, s->method, sizeof s->method, why, sizeof why))
        note(s, PZ_SEED_EXCLUDED, 0, "%s", why);
    else if (!memcmp(p, p + h, h)) note(s, PZ_SEED_EXCLUDED, 0, "two calls gave the same %zu bytes", h);
    else if (!bytes_ok(p, OS_BYTES, why, sizeof why)) note(s, PZ_SEED_EXCLUDED, 0, "%s", why);
    else {
        note(s, PZ_SEED_USED, 256, "%d bytes in two calls, distinct; health tests passed", OS_BYTES);
        c->data = p;
        c->len = OS_BYTES;
        return;
    }
    pz_secure_zero(p, OS_BYTES);
}

/* ---- the context and the mixing ---- */

static size_t ctx_put(uint8_t *e, size_t at, uint8_t tag, const char *v)
{
    const size_t n = strlen(v) < 255 ? strlen(v) : 255;
    if (at + 2 + n > 1024) return at;
    e[at] = tag;
    e[at + 1] = (uint8_t)n;
    memcpy(e + at + 2, v, n);
    return at + 2 + n;
}

/* The public context, as the salt and as a line for the report. On a cloned virtual machine all of it can be the same in every
 * clone, boot id included: it is credited nothing. */
static void context_salt(uint8_t salt[32], char *summary, size_t sn)
{
    char f[8][96] = {"", "", "", "", "", "", "", ""};    /* pid, ppid, uid, time, host, boot id, os, cpu */
    snprintf(f[3], sizeof f[3], "%lld, monotonic %.3f s", (long long)time(NULL), now_ms() / 1e3);
#if defined(_WIN32)
    DWORD hl = sizeof f[4];
    snprintf(f[0], sizeof f[0], "%lu", (unsigned long)GetCurrentProcessId());
    if (!GetComputerNameA(f[4], &hl)) f[4][0] = 0;
    snprintf(f[6], sizeof f[6], "Windows");
#else
    struct utsname u;
    snprintf(f[0], sizeof f[0], "%ld", (long)getpid());
    snprintf(f[1], sizeof f[1], "%ld", (long)getppid());
    snprintf(f[2], sizeof f[2], "%ld", (long)getuid());
    if (gethostname(f[4], sizeof f[4] - 1)) f[4][0] = 0;
    if (uname(&u) == 0) snprintf(f[6], sizeof f[6], "%.20s %.40s %.20s", u.sysname, u.release, u.machine);
#endif
#if defined(__linux__)
    const int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const long n = read(fd, f[5], 36);
        f[5][n == 36 ? 36 : 0] = 0;
        close(fd);
    }
#elif defined(__APPLE__)
    size_t bl = sizeof f[5] - 1;
    if (sysctlbyname("kern.bootsessionuuid", f[5], &bl, NULL, 0)) f[5][0] = 0;
#endif
#if defined(__x86_64__)
    unsigned r[4] = {0, 0, 0, 0};
    char vendor[13] = "";
    if (__get_cpuid(0, &r[0], &r[1], &r[2], &r[3])) {
        memcpy(vendor, &r[1], 4);
        memcpy(vendor + 4, &r[3], 4);
        memcpy(vendor + 8, &r[2], 4);
    }
    __get_cpuid(1, &r[0], &r[1], &r[2], &r[3]);
    const unsigned fam = (r[0] >> 8 & 15) + ((r[0] >> 8 & 15) == 15 ? r[0] >> 20 & 255 : 0);
    snprintf(f[7], sizeof f[7], "%s family %u, hypervisor %u", vendor, fam, r[2] >> 31);
#elif defined(__aarch64__)
    snprintf(f[7], sizeof f[7], "arm64, FEAT_RNG %d", hw_has(0));
#endif
    uint8_t e[1024];
    size_t n = 12;
    memcpy(e, "pzseed1/salt", 12);
    for (int i = 0; i < 8; i++) n = ctx_put(e, n, (uint8_t)(i + 1), f[i]);
    pz_sha256(e, n, salt);
    for (int i = 0; i < 8; i++) if (!f[i][0]) snprintf(f[i], sizeof f[i], "-");
    snprintf(summary, sn, "pid %.12s, ppid %.12s, uid %.12s, time %.40s, host %.40s, boot %.36s, %.60s, %.48s", f[0], f[1], f[2], f[3],
             f[4], f[5], f[6], f[7]);
}

/* HKDF-Extract over the records (seed.h), streamed, then HKDF-Expand for one block: T(1) = HMAC(PRK, info || 1). */
static void mix(const uint8_t salt[32], const pz_seed_source *src, const chunk *raw, uint8_t seed[32])
{
    static const char label[] = "pzseed1/ikm", info[] = "pzseed1/seed";
    uint8_t h[64], prk[32], one = 1;
    pz_hmac_sha256 x;
    pz_hmac_sha256_init(&x, salt, 32);
    h[0] = sizeof label - 1;
    memcpy(h + 1, label, sizeof label - 1);
    h[sizeof label] = PZ_SEED_NSRC;
    pz_hmac_sha256_update(&x, h, sizeof label + 1);
    for (int i = 0; i < PZ_SEED_NSRC; i++) {
        const bool used = src[i].state == PZ_SEED_USED;
        const size_t nl = strlen(src[i].name), ml = strlen(src[i].method), len = used ? raw[i].len : 0;
        size_t k = 0;
        h[k++] = (uint8_t)i;
        h[k++] = used;
        h[k++] = (uint8_t)nl;
        memcpy(h + k, src[i].name, nl);
        k += nl;
        h[k++] = (uint8_t)ml;
        memcpy(h + k, src[i].method, ml);
        k += ml;
        store_be32(h + k, used ? src[i].bits : 0);
        store_be32(h + k + 4, (uint32_t)((uint64_t)len >> 32));
        store_be32(h + k + 8, (uint32_t)len);
        pz_hmac_sha256_update(&x, h, k + 12);
        if (len) pz_hmac_sha256_update(&x, raw[i].data, len);
    }
    pz_hmac_sha256_final(&x, prk);
    pz_hmac_sha256_init(&x, prk, 32);
    pz_hmac_sha256_update(&x, (const uint8_t *)info, sizeof info - 1);
    pz_hmac_sha256_update(&x, &one, 1);
    pz_hmac_sha256_final(&x, seed);
    pz_secure_zero(prk, sizeof prk);
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

pz_seed_status pz_seed_generate(uint8_t seed[PZ_SEED_LEN], const pz_seed_opts *opts, pz_seed_report *rep)
{
    static const pz_seed_opts defaults;
    static const char *const names[PZ_SEED_NSRC] = {"cpu-rng", "tpm", "caller", "jitter", "os"};
    const pz_seed_opts *o = opts ? opts : &defaults;
    pz_seed_report local, *r = rep ? rep : &local;
    const double t0 = now_ms();
    memset(r, 0, sizeof *r);
    memset(seed, 0, PZ_SEED_LEN);
    for (int i = 0; i < PZ_SEED_NSRC; i++) snprintf(r->src[i].name, sizeof r->src[i].name, "%s", names[i]);
    if (o->extra_name) snprintf(r->src[PZ_SEED_SRC_CALLER].name, sizeof r->src[0].name, "%s", o->extra_name);
    if (o->extra && (o->extra_len == 0 || o->extra_len > PZ_SEED_EXTRA_MAX)) {
        snprintf(r->error, sizeof r->error, "the caller's source must give 1 to %d bytes", PZ_SEED_EXTRA_MAX);
        return PZ_SEED_E_ARG;
    }
    if (o->flags & PZ_SEED_HARDEN) r->hardened = pz_seed_harden();
    arena a = {NULL, OS_BYTES + HW_DRBG_WORDS * 8 + TPM_CALLS * TPM_CHUNK + JIT_SAMPLES * 4 + (o->extra ? o->extra_len : 0) + 5 * 64, 0, false};
    if (!(a.base = map_pages(a.size, true, &a.locked))) {
        snprintf(r->error, sizeof r->error, "no memory for the secrets");
        return PZ_SEED_E_MEM;
    }
    r->locked = a.locked;
    uint8_t salt[32];
    chunk raw[PZ_SEED_NSRC];
    memset(raw, 0, sizeof raw);
    context_salt(salt, r->context, sizeof r->context);
    pz_seed_source *s = r->src;
    double t = now_ms();
    /* the CPU's generator first, while no other secret is in the process: a malicious instruction has nothing to adapt to */
    src_cpu(&s[PZ_SEED_SRC_CPU], &raw[PZ_SEED_SRC_CPU], &a);
    s[PZ_SEED_SRC_CPU].ms = now_ms() - t, t = now_ms();
    src_tpm(&s[PZ_SEED_SRC_TPM], &raw[PZ_SEED_SRC_TPM], &a);
    s[PZ_SEED_SRC_TPM].ms = now_ms() - t, t = now_ms();
    src_caller(&s[PZ_SEED_SRC_CALLER], &raw[PZ_SEED_SRC_CALLER], &a, o);
    s[PZ_SEED_SRC_CALLER].ms = now_ms() - t, t = now_ms();
    const unsigned before = s[PZ_SEED_SRC_TPM].bits + s[PZ_SEED_SRC_CALLER].bits;
    src_jitter(&s[PZ_SEED_SRC_JITTER], &raw[PZ_SEED_SRC_JITTER], &a, before < BITS_NEEDED ? BITS_NEEDED - before : 0,
               o->max_ms ? o->max_ms : PZ_SEED_MAX_MS, r);
    s[PZ_SEED_SRC_JITTER].ms = now_ms() - t, t = now_ms();
    /* the OS last: its output spends the least time in memory, and it is read after any snapshot of a virtual machine taken during
     * the slower steps (whose kernel then reseeds) */
    src_os(&s[PZ_SEED_SRC_OS], &raw[PZ_SEED_SRC_OS], &a);
    s[PZ_SEED_SRC_OS].ms = now_ms() - t;
    r->counted = before + s[PZ_SEED_SRC_JITTER].bits;
    pz_seed_status st = PZ_SEED_OK;
    if (s[PZ_SEED_SRC_OS].state != PZ_SEED_USED) {
        st = PZ_SEED_E_OS;
        snprintf(r->error, sizeof r->error, "the operating system's random generator failed (%s): no seed", s[PZ_SEED_SRC_OS].reason);
    } else if (r->counted < BITS_NEEDED && !(o->flags & PZ_SEED_ALLOW_DEGRADED)) {
        st = PZ_SEED_E_SHORT;
        snprintf(r->error, sizeof r->error,
                 "the TPM, the caller's source and CPU jitter give %u bits, %d are needed (tpm: %.60s; jitter: %.100s): no seed; "
                 "make it on another computer", r->counted, BITS_NEEDED, s[PZ_SEED_SRC_TPM].reason, s[PZ_SEED_SRC_JITTER].reason);
    } else {
        r->degraded = r->counted < BITS_NEEDED;
        mix(salt, s, raw, seed);
    }
    pz_secure_zero(a.base, a.size);
    unmap_pages(a.base, a.size, a.locked);
    r->ms = now_ms() - t0;
    return st;
}
