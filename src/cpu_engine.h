/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu_engine.h - CPU linear-search engines (one per instruction-set variant). */
#ifndef PZ_CPU_ENGINE_H
#define PZ_CPU_ENGINE_H

#include "common.h"
#include <pzcore/ec.h>
#include <stdatomic.h>

#define CPU_HALF 256                 /* group = 2*HALF consecutive keys per batch inversion */
#define CPU_GRP (2 * CPU_HALF)

typedef struct scan_job {
    u32 target_w[5];                 /* RIPEMD-160 digest as little-endian words */
    u8 target_h160[20];
    _Atomic int *stop;               /* non-zero asks the engine to return */
    void (*on_hit)(void *user, const pz_u256 *key);   /* called for every full 160-bit match */
    void *user;
    _Atomic u64 *progress;           /* optional: keys scanned, updated in batches */
} scan_job;

typedef struct {
    const char *name;
    int lanes;
    u64 granule;                     /* keys per indivisible step: a scan covers a whole number of them */
    bool (*supported)(void);
    /* Scans [start, start+nkeys) rounded up to whole groups; returns the number
     * of keys of the claim that were fully covered. start must be >= 1. */
    u64 (*scan)(const scan_job *job, const pz_u256 *start, u64 nkeys);
    /* Exposed for tests/benchmarks: hash160 of `lanes` compressed pubkeys. */
    void (*hash160_lanes)(const u32 *X, const u32 *pfx, u32 *out);
} cpu_engine;

void scan_job_init(scan_job *j, const u8 h160[20]);

/* Shared read-only tables (built by cpu_tables_init, which is idempotent). */
void cpu_tables_init(void);
extern pz_ge cpu_gtab[CPU_HALF];        /* cpu_gtab[i] = (i+1)*G */
extern pz_ge cpu_gstep;                 /* CPU_GRP*G */
/* out[i] = (i+1)*G for i < n, any n (the first CPU_HALF from cpu_gtab) */
void cpu_gmultiples(pz_ge *out, int n);

const cpu_engine *const *cpu_engine_all(int *n);
const cpu_engine *cpu_engine_by_name(const char *name);
/* Known-answer check of an engine on one granule (a few milliseconds): false if it misses or misplaces the key. */
bool cpu_engine_quickcheck(const cpu_engine *e);
/* Picks the fastest supported engine that passes the quick check by running a short benchmark. */
const cpu_engine *cpu_engine_autotune(double seconds_per_engine, bool verbose);
/* Plain reference scan (one scalar multiplication per key), for tests. */
u64 cpu_scan_reference(const scan_job *job, const pz_u256 *start, u64 nkeys);

/* x86 feature probes (no-ops elsewhere). */
bool cpu_has_avx2(void);
bool cpu_has_avx512(void);
bool cpu_has_ifma(void);
/* aarch64 probe: the SHA-256 instructions (pzcore/cpu.h). */
bool cpu_has_sha2(void);

#endif
