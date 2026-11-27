/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_seed.c - the seed generator: known answers of the mixing (computed with Python's hmac), an input never passing for another, the
 * seed changing with every input, seeds that differ, the failures of the operating system's generator, and the process hardening.
 * It includes seed.c with PZ_SEED_TEST, which adds the hook (g_os_fault) that makes the OS generator fail. */
#define PZ_SEED_TEST 1
#include "../src/seed.c"
#include "testutil.h"

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t a = 0;
    for (size_t i = 0; i < n; i++) a |= p[i];
    return a == 0;
}

static void test_mix(void)
{
    uint8_t cpu[32], timer[16], os[OS_BYTES], seed[32], other[32], ikm[200], big[8 * TIMER_WORDS] = {0};
    char hx[65];
    for (int i = 0; i < 32; i++) cpu[i] = (uint8_t)(3 * i);
    for (int i = 0; i < 16; i++) timer[i] = (uint8_t)(7 * i + 1);
    for (int i = 0; i < OS_BYTES; i++) os[i] = (uint8_t)(255 - i);
    const seed_input in[3] = {{cpu, sizeof cpu}, {timer, sizeof timer}, {os, sizeof os}};
    mix(seed, in, 3);
    tu_hex(seed, 32, hx);
    CHECK_STR(hx, "89bae84849052288a9589f70ee840b9065c1f9d9b221459df2d533e36c34ef97");
    /* the same through pz_hkdf_sha256 over the IKM built here: index, length, bytes */
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        ikm[n++] = (uint8_t)i;
        for (int k = 3; k >= 0; k--) ikm[n++] = (uint8_t)(in[i].len >> (8 * k));
        memcpy(ikm + n, in[i].bytes, in[i].len), n += in[i].len;
    }
    CHECK(pz_hkdf_sha256((const uint8_t *)"pzseed2", 7, ikm, n, (const uint8_t *)"seed", 4, other, 32) && !memcmp(seed, other, 32));
    /* a CPU without a generator, and inputs of the largest size */
    mix(seed, (const seed_input[]){{big, 0}, {big, sizeof big}, {big, OS_BYTES}}, 3);
    tu_hex(seed, 32, hx);
    CHECK_STR(hx, "06cb2e489971f9e935cf19cff9088c932148728ded9d8d29852cb28eef3e1f22");
    /* every bit of every input changes the seed */
    mix(seed, in, 3);
    for (int i = 0; i < 3; i++) {
        uint8_t *b = (uint8_t *)in[i].bytes;
        for (size_t k = 0; k < 8 * in[i].len; k++) {
            b[k / 8] ^= (uint8_t)(1 << k % 8);
            mix(other, in, 3);
            CHECK(memcmp(seed, other, 32) != 0);
            b[k / 8] ^= (uint8_t)(1 << k % 8);
        }
    }
    /* no input passes for another: the same bytes cut elsewhere, or given in another order, make another seed */
    uint8_t cat[sizeof cpu + sizeof timer + sizeof os];
    memcpy(cat, cpu, sizeof cpu), memcpy(cat + sizeof cpu, timer, sizeof timer), memcpy(cat + sizeof cpu + sizeof timer, os, sizeof os);
    for (size_t a = 0; a <= sizeof cat; a += 8)
        for (size_t b = a; b <= sizeof cat; b += 8) {
            if (a == sizeof cpu && b == sizeof cpu + sizeof timer) continue;           /* the cut of `in` */
            mix(other, (const seed_input[]){{cat, a}, {cat + a, b - a}, {cat + b, sizeof cat - b}}, 3);
            CHECK(memcmp(seed, other, 32) != 0);
        }
    mix(other, (const seed_input[]){in[1], in[0], in[2]}, 3);
    CHECK(memcmp(seed, other, 32) != 0);
    mix(other, in, 2);                                                               /* an input left out is not an empty one */
    mix(seed, (const seed_input[]){in[0], in[1], {big, 0}}, 3);
    CHECK(memcmp(seed, other, 32) != 0);
}

/* What the machine gives: the timer moves, the CPU gives at most its words, the OS gives what is asked. Nothing more is asked of them. */
static void test_inputs(void)
{
    uint64_t t[TIMER_WORDS], w[CPU_WORDS] = {0};
    uint8_t a[OS_BYTES], b[OS_BYTES], big[1000] = {0};
    char err[160] = "";
    timer_fill(t);
    CHECK(t[TIMER_WORDS - 1] != t[0]);
    const size_t n = cpu_fill(w);
    CHECK(n <= CPU_WORDS && (n == 0 || cpu_has(0) || cpu_has(1)));
    CHECK(os_fill(a, sizeof a, err, sizeof err) && os_fill(b, sizeof b, err, sizeof err) && memcmp(a, b, sizeof a) != 0 && !err[0]);
    CHECK(pz_os_random(big, sizeof big) && !all_zero(big + 744, 256));     /* more than getentropy gives at once */
    printf("  timer: %llu ticks over %d hashes; CPU generator: %zu of %d words\n", (unsigned long long)(t[TIMER_WORDS - 1] - t[0]),
           TIMER_WORDS - 1, n, CPU_WORDS);
}

static int cmp32(const void *a, const void *b) { return memcmp(a, b, 32); }

static void test_seeds(void)
{
    enum { N = 2000 };
    static uint8_t all[N][32];
    for (int i = 0; i < N; i++) CHECK(pz_seed_generate(all[i], NULL, 0) && !all_zero(all[i], 32));
    qsort(all, N, 32, cmp32);
    unsigned dups = 0;
    for (int i = 1; i < N; i++) dups += !memcmp(all[i], all[i - 1], 32);
    CHECK(dups == 0);
}

/* The OS generator is mandatory: when it fails, gives nothing or gives the same block twice there is no seed, and the reason is said. */
static void test_os_failures(void)
{
    static const struct { int fault; const char *why; } c[] = {
        {OSF_FAIL, OS_NAME " failed (errno"}, {OSF_SHORT, OS_NAME " gave no bytes"}, {OSF_REPEAT, OS_NAME " gave the same 32 bytes twice"}};
    uint8_t seed[32];
    char err[160];
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        memset(seed, 0xa5, sizeof seed);
        err[0] = 0;
        g_os_fault = c[i].fault;
        CHECK(!pz_seed_generate(seed, err, sizeof err) && all_zero(seed, 32) && strstr(err, c[i].why) == err);
        CHECK(!pz_seed_generate(seed, NULL, 0) && all_zero(seed, 32));               /* nobody asks why */
        CHECK(pz_os_random(seed, sizeof seed) == (c[i].fault == OSF_REPEAT));         /* the generator alone checks no repeat */
    }
    g_os_fault = OSF_ENOSYS;                                                         /* a kernel without getrandom: its device, on Linux */
#if defined(__linux__)
    CHECK(pz_seed_generate(seed, err, sizeof err) && !all_zero(seed, 32));
#else
    CHECK(!pz_seed_generate(seed, err, sizeof err) && all_zero(seed, 32));
#endif
    g_os_fault = 0;
    CHECK(pz_seed_generate(seed, err, sizeof err) && !all_zero(seed, 32));
}

/* ---- the process hardening: last, because it stays ---- */

static void test_harden(void)
{
#if defined(_WIN32)
    CHECK(!pz_seed_harden());
#else
    CHECK(pz_seed_harden());
#endif
#if defined(__linux__)
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
#endif
}

int main(void)
{
    test_mix();
    test_inputs();
    test_seeds();
    test_os_failures();
    test_harden();
    TEST_DONE("t_seed");
}
