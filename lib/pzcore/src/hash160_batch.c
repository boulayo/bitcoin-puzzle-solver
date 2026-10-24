/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash160_batch.c - run-time choice of the lane kernel and the conversion between byte strings and lanes. */
#include <pzcore/hash160_batch.h>
#include <pzcore/cpu.h>
#include <pzcore/hash.h>
#include <stdatomic.h>
#include "h160_impl.h"

typedef struct {
    const char *name;
    unsigned lanes;
    pz_h160_kernel_fn kernel;      /* NULL: the scalar code */
    bool (*available)(void);
} impl_t;

static bool always(void) { return true; }
/* best first */
static const impl_t IMPLS[] = {
#ifdef PZ_HAVE_X86_SIMD
    {"avx512", 16, pz_h160_kernel_avx512, pz_cpu_has_avx512},
    {"avx2", 8, pz_h160_kernel_avx2, pz_cpu_has_avx2},
#endif
#ifdef PZ_HAVE_ARM_SHA2
    {"neon2", 8, pz_h160_kernel_neon2, pz_cpu_has_sha2},
    {"neon", 4, pz_h160_kernel_neon, pz_cpu_has_sha2},
#endif
    {"portable", 4, pz_h160_kernel_portable, always},
    {"scalar", 1, NULL, always},
};
#define NIMPL (sizeof IMPLS / sizeof IMPLS[0])

static _Atomic(const impl_t *) g_impl;      /* NULL until first use */

static const impl_t *best(void)
{
    for (size_t i = 0; i < NIMPL; i++)
        if (IMPLS[i].available()) return &IMPLS[i];
    return &IMPLS[NIMPL - 1];
}

static const impl_t *current(void)
{
    const impl_t *im = atomic_load_explicit(&g_impl, memory_order_acquire);
    if (!im) {
        im = best();          /* the same answer in every thread, so a race only repeats the work */
        atomic_store_explicit(&g_impl, im, memory_order_release);
    }
    return im;
}

const char *pz_hash160_batch_impl(void) { return current()->name; }
unsigned pz_hash160_batch_lanes(void) { return current()->lanes; }
pz_hash160_lanes_fn pz_hash160_batch_kernel(void) { return current()->kernel; }

size_t pz_hash160_batch_list(const char **names, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < NIMPL; i++) {
        if (!IMPLS[i].available()) continue;
        if (names && n < cap) names[n] = IMPLS[i].name;
        n++;
    }
    return n;
}

bool pz_hash160_batch_select(const char *name)
{
    if (!name || !strcmp(name, "auto")) {
        atomic_store_explicit(&g_impl, best(), memory_order_release);
        return true;
    }
    for (size_t i = 0; i < NIMPL; i++) {
        if (strcmp(IMPLS[i].name, name) != 0 || !IMPLS[i].available()) continue;
        atomic_store_explicit(&g_impl, &IMPLS[i], memory_order_release);
        return true;
    }
    return false;
}

#define MAXLANES 16

void pz_hash160_x_batch(size_t n, const uint8_t *xs, const uint8_t *parity, uint8_t *out)
{
    const impl_t *im = current();
    if (!im->kernel) {
        for (size_t i = 0; i < n; i++) pz_hash160_pub33(parity[i], xs + 32 * i, out + 20 * i);
        return;
    }
    const size_t L = im->lanes;
    uint32_t X[8 * MAXLANES] PZ_ALIGN(64), pfx[MAXLANES] PZ_ALIGN(64), res[5 * MAXLANES] PZ_ALIGN(64);
    for (size_t i = 0; i < n; i += L) {
        const size_t cnt = n - i < L ? n - i : L;
        for (size_t l = 0; l < L; l++) {
            const size_t k = i + (l < cnt ? l : cnt - 1);       /* the unused lanes of the last step repeat its last key */
            const uint8_t *x = xs + 32 * k;
            for (size_t w = 0; w < 8; w++)
                X[w * L + l] = (uint32_t)x[4 * w] << 24 | (uint32_t)x[4 * w + 1] << 16 | (uint32_t)x[4 * w + 2] << 8 | x[4 * w + 3];
            pfx[l] = 2u + (parity[k] & 1u);
        }
        im->kernel(X, pfx, res);
        for (size_t l = 0; l < cnt; l++) {
            uint8_t *o = out + 20 * (i + l);
            for (size_t w = 0; w < 5; w++) {
                const uint32_t v = res[w * L + l];
                o[4 * w] = (uint8_t)v; o[4 * w + 1] = (uint8_t)(v >> 8); o[4 * w + 2] = (uint8_t)(v >> 16); o[4 * w + 3] = (uint8_t)(v >> 24);
            }
        }
    }
}
