/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* peertest.h - the few helpers shared by the unit tests of pzpeer (the same checks as the tests of pzcore). */
#ifndef PZPEER_PEERTEST_H
#define PZPEER_PEERTEST_H

#include "sys.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_UNUSED __attribute__((unused))
static int g_fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fails++; } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); \
        if (!g_ || strcmp(g_, w_) != 0) { printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); g_fails++; } } while (0)
#define TEST_DONE(name) do { printf(g_fails ? name ": %d FAILED\n" : name ": OK\n", g_fails); return g_fails != 0; } while (0)

/* hex text to bytes; returns the number of bytes, or -1 */
static PT_UNUSED int pt_unhex(const char *s, uint8_t *out, size_t max)
{
    const size_t n = strlen(s);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

/* hex text to a malloc'd buffer (the caller frees it), its length in *n; NULL on bad hex */
static PT_UNUSED uint8_t *pt_unhex_alloc(const char *s, size_t *n)
{
    const size_t len = strlen(s) / 2;
    uint8_t *b = malloc(len ? len : 1);
    if (!b || pt_unhex(s, b, len) != (int)len) {
        free(b);
        return NULL;
    }
    *n = len;
    return b;
}

static PT_UNUSED const char *pt_hex(const uint8_t *p, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * n] = 0;
    return out;
}

/* a new folder for the files of a test, where the system keeps temporary files: its path into out */
static PT_UNUSED bool pt_tmpdir(const char *name, char *out, size_t cap)
{
    uint8_t r[4];
    const char *base = getenv("TMPDIR");
    if (!base) base = getenv("TEMP");                          /* Windows */
    if (!sys_random(r, sizeof r)) return false;
    snprintf(out, cap, "%s/%s.%02x%02x%02x%02x", base ? base : "/tmp", name, r[0], r[1], r[2], r[3]);
    return sys_mkdir(out);
}

#endif
