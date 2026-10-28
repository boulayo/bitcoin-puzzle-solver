/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* testutil.h - the few helpers shared by the unit tests of pzcore (nothing but libc). */
#ifndef PZCORE_TESTUTIL_H
#define PZCORE_TESTUTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TU_UNUSED __attribute__((unused))
static int g_fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fails++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want)) != 0) { printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); g_fails++; } } while (0)
#define TEST_DONE(name) do { printf(g_fails ? name ": %d FAILED\n" : name ": OK\n", g_fails); return g_fails != 0; } while (0)

/* xorshift64*: deterministic, good enough for test data */
static uint64_t tu_state = 0x9E3779B97F4A7C15ULL;
static TU_UNUSED uint64_t tu_rnd(void)
{
    tu_state ^= tu_state >> 12;
    tu_state ^= tu_state << 25;
    tu_state ^= tu_state >> 27;
    return tu_state * 0x2545F4914F6CDD1DULL;
}
static TU_UNUSED void tu_fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(tu_rnd() >> 32); }

/* An on/off variable of the environment, as pzsolve reads them: on with any value except "" and "0". */
static TU_UNUSED int tu_env_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static TU_UNUSED void tu_hex(const uint8_t *p, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * n] = 0;
}

/* hex text to bytes; returns the number of bytes, or -1 */
static TU_UNUSED int tu_unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

#endif
