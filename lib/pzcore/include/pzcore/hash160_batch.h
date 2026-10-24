/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash160_batch.h - HASH160 (RIPEMD-160 of SHA-256) of many compressed public keys at once, with the SIMD instructions of the CPU.
 *
 * A compressed public key is 33 bytes: 02 or 03 (the parity of y) and the 32-byte x. Hashing one with pz_hash160 costs about 0.7 us;
 * these functions hash 8 or 16 of them in the time the scalar code needs for one or two, which is what a scan over millions of keys
 * (an address search) needs. Both steps are done in 32-bit lanes of a vector register, one key per lane.
 *
 * The implementation is chosen at run time, the first time it is needed: AVX-512 (16 keys per step), AVX2 (8) on x86-64; NEON with the
 * ARMv8 SHA-256 instructions (8 or 4) on aarch64 CPUs that have them; otherwise portable vector code (4) that needs no particular
 * instruction set; and, only if you ask for it, the scalar reference (1). All of them give the same bits. The names are "avx512",
 * "avx2", "neon2", "neon", "portable" and "scalar".
 *
 * Thread-safety: everything may be called from any number of threads; pz_hash160_batch_select may be called at any time, and every
 * call of pz_hash160_x_batch uses one consistent implementation. No allocation, no global state beyond the choice of the implementation. */
#ifndef PZCORE_HASH160_BATCH_H
#define PZCORE_HASH160_BATCH_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

/* Computes out[i] = HASH160(parity-byte || x[i]) for the n keys. xs holds the n x coordinates as 32 big-endian bytes each (n * 32 bytes),
 * parity[i] gives the prefix byte: its lowest bit selects 03 (set) or 02 (clear), so 0/1 and 2/3 both work, and out receives the n
 * hashes of 20 bytes each (n * 20 bytes). Any n is fine (0 included, then no pointer is read), the buffers need no alignment and
 * out must not overlap the inputs. The keys are not checked: x need not be on the curve. */
void pz_hash160_x_batch(size_t n, const uint8_t *xs, const uint8_t *parity, uint8_t *out);

/* The implementation in use, and the number of keys it hashes in one step (a batch of that many keys or a multiple wastes no work). */
const char *pz_hash160_batch_impl(void);
unsigned pz_hash160_batch_lanes(void);

/* Names of the implementations this CPU can run, best first, ending with "portable" and "scalar". Stores up to cap of them in names and
 * returns how many there are. */
size_t pz_hash160_batch_list(const char **names, size_t cap);

/* Chooses the implementation: NULL or "auto" for the best one (the default), "portable" to force the code that needs no instruction set
 * (what to use to compare with, or on a machine where the SIMD code is suspect), "scalar" for the reference, or any other name from the
 * list. Returns false, and changes nothing, if the name is unknown or the CPU cannot run that implementation. */
bool pz_hash160_batch_select(const char *name);

/* Advanced: the lane kernel of the implementation in use, for callers that keep their data in lane order and do not want the
 * conversion of pz_hash160_x_batch. Layout (lanes = pz_hash160_batch_lanes(), uint32_t arrays):
 *   X[8][lanes]    the x coordinates as big-endian words: word i of key l at X[i * lanes + l]
 *   pfx[lanes]     the prefix bytes, 2 or 3
 *   out[5][lanes]  the hashes as five little-endian words: word i of key l at out[i * lanes + l]
 * Returns NULL for the scalar implementation. The kernel is thread-safe. */
typedef void (*pz_hash160_lanes_fn)(const uint32_t *X, const uint32_t *pfx, uint32_t *out);
pz_hash160_lanes_fn pz_hash160_batch_kernel(void);

PZ_END_DECLS

#endif
