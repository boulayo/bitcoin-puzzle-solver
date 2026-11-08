/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seed.h - a fresh secret seed of 256 bits from several independent entropy sources: the home seed of the shared network
 * (docs/en/NETWORK.md 4.1) and the seeds of pzsolve's campaigns.
 *
 * The sources, in the order they are read, and the bits each one is credited when it passes its health tests:
 *   cpu-rng  RDSEED (else RDRAND) on x86-64, RNDRRS (else RNDR) on arm64: 128 bits (RNDR 64). Mixed in, but not counted by the rule
 *            below: nobody can check it, and the operating system often seeds its own generator from it.
 *   tpm      TPM2_GetRandom, 8 x 32 bytes, through /dev/tpmrm0 or /dev/tpm0 (Linux) or TBS (Windows): 128 bits. No TPM, or no
 *            permission to use it, means the source is skipped, not an error.
 *   caller   the source the caller passes in pz_seed_opts (for example GPU timing jitter): the bits it claims, at most 256.
 *   jitter   CPU timing jitter: rounds of 4096 timer deltas, each around a fixed walk through 1 MiB of memory. 0.5 bit per sample
 *            that is not stuck, at most 256, and the live estimate of the round must reach 1 bit per sample.
 *   os       getrandom (Linux), getentropy (macOS, BSD), BCryptGenRandom (Windows): 1024 bytes in two calls, 256 bits. Read last,
 *            so that its output spends the least time in memory.
 * The system context (process ids, clocks, host name, boot id, OS and CPU) is public and credited 0 bits: it only makes the salt.
 *
 * Rules:
 *   - the OS source is mandatory: if it fails there is no seed (PZ_SEED_E_OS);
 *   - tpm + caller + jitter must be credited at least 128 bits. Short of that, the jitter source collects further rounds, for at most
 *     max_ms in all; still short, there is no seed (PZ_SEED_E_SHORT) unless PZ_SEED_ALLOW_DEGRADED is set;
 *   - every source reads a fixed amount and tests its own raw output (the SP 800-90B repetition count and adaptive proportion tests,
 *     and the known failures of each kind of source). One that fails is excluded and wiped; the others are not affected.
 *
 * Mixing (HKDF-SHA256, RFC 5869):
 *   salt = SHA-256("pzseed1/salt" || context fields, each as tag (1) || length (1) || bytes)
 *   IKM  = 11 || "pzseed1/ikm" || 5 || for each source in the order above:
 *            index (1) || used (1) || length (1) || name || length (1) || method || bits (4) || length (8) || raw output
 *          (integers big-endian; a source that is not used has used = 0, bits = 0 and no output)
 *   seed = HKDF-Expand(HKDF-Extract(salt, IKM), "pzseed1/seed", 32)
 * Every field carries its length, so two different lists of records never give the same IKM and no source can make its bytes pass
 * for another's. One honest source with enough entropy makes the seed unpredictable; a dishonest one that sees all the others can
 * only grind, about 2^b tries to fix b bits of the seed.
 *
 * Memory: the raw output of every source is kept in one region locked in RAM (mlock, VirtualLock), left out of core dumps and seen
 * as zeros by a forked child (Linux), and wiped before it is unmapped. The report holds counts and reasons, never data.
 * Unlike the rest of pzcore this module talks to the operating system: it maps pages, reads the TPM device and the clock, and on
 * Windows needs bcrypt (tbs.dll is loaded at run time). Thread-safe; one call takes about 15 ms. */
#ifndef PZCORE_SEED_H
#define PZCORE_SEED_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

#define PZ_SEED_LEN       32
#define PZ_SEED_EXTRA_MAX 65536            /* bytes the caller's source may give */
#define PZ_SEED_MAX_MS    3000             /* default bound of the jitter collection when the 128 bits are not reached */

/* The caller's source: writes exactly len bytes (pz_seed_opts.extra_len) of its raw output to buf, which is in the locked region,
 * and returns true when its own health tests passed; false, with a short reason in why (never data), excludes it. */
typedef bool (*pz_seed_source_fn)(void *arg, uint8_t *buf, size_t len, char *why, size_t whylen);

enum {
    PZ_SEED_ALLOW_DEGRADED = 1,            /* make the seed even with fewer than 128 counted bits (the OS alone, as pzsolve's campaign
                                              seeds always had); the report says degraded */
    PZ_SEED_HARDEN = 2                     /* harden the whole process first: no core files and, on Linux, not dumpable, so that other
                                              processes of the same user cannot attach to it or read its memory */
};

typedef struct {
    pz_seed_source_fn extra;               /* NULL: no caller's source */
    void *extra_arg;
    const char *extra_name;                /* its name in the report and in the mix, at most 15 characters ("caller" when NULL) */
    size_t extra_len;                      /* bytes it gives, 1..PZ_SEED_EXTRA_MAX */
    unsigned extra_bits;                   /* bits it claims for them; at most 256, and at most 8 per byte, are credited */
    unsigned flags;                        /* PZ_SEED_ALLOW_DEGRADED, PZ_SEED_HARDEN */
    unsigned max_ms;                       /* 0: PZ_SEED_MAX_MS */
} pz_seed_opts;

typedef enum {
    PZ_SEED_OK = 0,
    PZ_SEED_E_OS,                          /* the operating system's generator failed */
    PZ_SEED_E_SHORT,                       /* tpm + caller + jitter gave fewer than 128 bits, even after the longer collection */
    PZ_SEED_E_MEM,                         /* no memory could be mapped */
    PZ_SEED_E_ARG                          /* an option out of range */
} pz_seed_status;

enum { PZ_SEED_SRC_CPU, PZ_SEED_SRC_TPM, PZ_SEED_SRC_CALLER, PZ_SEED_SRC_JITTER, PZ_SEED_SRC_OS, PZ_SEED_NSRC };

typedef enum { PZ_SEED_ABSENT = 0, PZ_SEED_USED, PZ_SEED_EXCLUDED } pz_seed_state;

typedef struct {
    char name[16];                         /* cpu-rng, tpm, the caller's name, jitter, os */
    char method[32];                       /* RDSEED, /dev/tpmrm0, getrandom, the timer and the walk, ... */
    pz_seed_state state;
    unsigned bits;                         /* credited; 0 unless used */
    char reason[160];                      /* why it is absent or excluded, or which tests it passed */
    double ms;
} pz_seed_source;

typedef struct {
    pz_seed_source src[PZ_SEED_NSRC];      /* indexed by PZ_SEED_SRC_* */
    unsigned counted;                      /* the bits of tpm + caller + jitter */
    unsigned jitter_rounds;                /* rounds of 4096 samples collected */
    double jitter_h;                       /* live min-entropy estimate of the last round, bits per sample */
    bool degraded;                         /* made with fewer than 128 counted bits (PZ_SEED_ALLOW_DEGRADED) */
    bool locked;                           /* the region of the raw outputs was locked in RAM */
    bool hardened;                         /* PZ_SEED_HARDEN took effect */
    char context[320];                     /* the public context that made the salt */
    char error[300];                       /* what went wrong when the status is not PZ_SEED_OK */
    double ms;
} pz_seed_report;

/* Writes a fresh seed and returns PZ_SEED_OK; any other status leaves seed all zero and says why in rep->error. opts may be NULL
 * (no caller's source, default rules); rep may be NULL. */
pz_seed_status pz_seed_generate(uint8_t seed[PZ_SEED_LEN], const pz_seed_opts *opts, pz_seed_report *rep);

PZ_END_DECLS

#endif
