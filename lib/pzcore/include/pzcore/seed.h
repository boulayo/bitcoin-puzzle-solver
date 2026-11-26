/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seed.h - a fresh secret seed of 256 bits: the home seed of the shared network (docs/en/NETWORK.md 4.1) and the seeds of pzsolve's
 * campaigns.
 *
 * Three inputs, in the order they are read, hashed together:
 *   cpu      the CPU's generator, where there is one: 4 words of RDSEED (else RDRAND) on x86-64, of RNDRRS (else RNDR) on arm64.
 *   timer    256 readings of the CPU's counter, a hash apart: their differences carry the timing noise of the machine.
 *   os       the operating system's generator: getrandom (Linux), getentropy (macOS, BSD), BCryptGenRandom (Windows). 64 bytes in
 *            two calls, which must differ.
 * The OS generator is mandatory: if it fails there is no seed. The other two are mixed in as they come: nothing tests them, nothing is
 * counted for them, and a machine without them gets its seed all the same.
 *
 * The guarantee: the seed is as good as the operating system's generator, which every other secret of the programs comes from too. If
 * that generator is weak, the seed is no worse than it, and as good as the better of the other two. Nothing here measures entropy,
 * so nothing more is promised: a machine whose three inputs are all weak (a virtual machine cloned after its start, say) gets a weak seed.
 *
 * Mixing (HKDF-SHA256, RFC 5869):
 *   IKM  = for each input in the order above: index (1) || length (4, big-endian) || bytes (a CPU without a generator: length 0)
 *   seed = HKDF(salt = "pzseed2", IKM, info = "seed", 32 bytes)
 * Every input carries its index and its length, so the bytes of one never pass for another's. An input that is not honest and sees
 * the others can only grind, about 2^b tries to fix b bits of the seed; the CPU's generator is read first, while nothing else is there
 * to see.
 *
 * The inputs are wiped before the function returns. Unlike the rest of pzcore this module talks to the operating system, and on
 * Windows it needs bcrypt. Thread-safe; a call takes well under a millisecond. */
#ifndef PZCORE_SEED_H
#define PZCORE_SEED_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

#define PZ_SEED_LEN 32

/* Writes a fresh seed and returns true. False when the operating system's generator fails: seed is all zero and err (errlen bytes;
 * may be NULL with errlen 0) says why. */
bool pz_seed_generate(uint8_t seed[PZ_SEED_LEN], char *err, size_t errlen);

/* For a process that holds secrets: no core files and, on Linux, not dumpable, so that other processes of the same user cannot attach
 * to it or read its memory. False when it did not take effect (always on Windows, which has no such call). */
bool pz_seed_harden(void);

PZ_END_DECLS

#endif
