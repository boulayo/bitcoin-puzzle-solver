/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kangaroo.h - interval discrete logarithm with known public key. */
#ifndef PZ_KANGAROO_H
#define PZ_KANGAROO_H
#include "run.h"
#include <math.h>
#define KANG_C_MEAN 1.06         /* measured mean cost of the kangaroo, in units of sqrt(W) (W = 2^32..2^44) */
/* The mean cost of the kangaroo on an interval of W keys, in steps: KANG_C_MEAN sqrt(W). */
static inline long double kang_mean_cost(const pz_u256 *W) { return KANG_C_MEAN * sqrtl(pz_u256_to_ld(W)); }
#define KANG_MAX_W_LOG2 188      /* the largest interval, 2^188 keys: the exponents are signed 192-bit numbers (+-2^191), the wild walkers on 3Q start
                                  * as far as 3W below zero and the collision of two of them, 6x = d2 - d1, needs 6W < 2^191 */
/* NULL if the kangaroo can work on an interval of W keys, else the reason it cannot (too small, or beyond KANG_MAX_W_LOG2). */
const char *kang_range_error(const pz_u256 *W);
int kangaroo_selftest(void);
/* Every CPU variant of the kangaroo step against the portable one, bit for bit (prints one line each). */
int kang_round_selftest(void);
/* Name of the CPU kangaroo step in use (chosen by measurement on first use). */
const char *kang_round_name(void);
/* The instruction set of the CPU step variants (fixed for a machine, unlike the measured choice): for keys of saved measurements. */
const char *kang_round_isa(void);
int kangaroo_stats(int argc, char **argv);
int kang_log_file_selftest(void);    /* loading dp.bin: a torn last record is cut off and nothing else, a read error is an error */
int kang_log_fault_selftest(void);   /* the DP log when the disk refuses writes: no overflow, nothing lost, torn records repaired */
int kang_merge_selftest(void);   /* kang_merge_file: collision across two logs, refusal of foreign logs */
/* Builds a synthetic kangaroo state on an interval of 2^wbits keys (for GPU self-tests). Free with kang_free_test_state. The key is at
 * the given offset from the start of the interval (the first form: 123456789). */
struct kang_state *kang_make_test_state(unsigned wbits, int nthreads);
struct kang_state *kang_make_test_state_at(unsigned wbits, int nthreads, const pz_u256 *offset);
struct kang_state *kang_make_test_state_dp(unsigned wbits, int nthreads, const pz_u256 *offset, unsigned dp_bits);   /* with --dp-bits (0: automatic) */
void kang_free_test_state(struct kang_state *ks);
/* Runs the CPU kangaroo on [lo, lo + 2^wbits) until `key` is found; returns the steps used or -1. Test overrides: R jumps
 * instead of 256 (R > 0, a power of two), dp_bits (dp > 0) and no restart of walkers without distinguished points. */
typedef struct { unsigned R, dp; bool no_watchdog; } kang_try_opts;
double kang_try(const pz_u256 *lo, unsigned wbits, const pz_u256 *key, int nthreads, double timeout, u64 seedn, const kang_try_opts *o);
/* Measures the CPU walk rate (steps/s, all cores) over `secs` seconds; prints when `print` is set. */
double kangaroo_bench(double secs, bool print);
/* 'merge': adds the distinguished points of other computers' logs (or campaign folders) to the campaign of `r`, which must be
 * set up like for a hunt (cfg, paths, lock taken). Returns 1 when a file could not be used, else 0; r->found tells whether the key was found. */
int kangaroo_merge(run_ctx *r, const char *const *files, int nfiles);
/* 'bench --together': the CPU kangaroo threads and the GPUs running together as in a campaign (puzzle 140 shape), over several
 * CPU thread counts (or only `threads`, if > 0) and with each slower GPU switched off in turn; prints the rates and the
 * verdict in *choice (may be NULL); `gpu_filter` as --gpu (NULL: the default GPUs). Returns 0, or 1 when there is no usable GPU. */
struct tg_choice;
int kangaroo_bench_together(int threads, const char *gpu_filter, struct tg_choice *choice);
/* Steps per second of every CPU step variant on 1 thread, one thread per physical core and all threads, and of the
 * mixed SMT layout (prints). */
void kang_bench_cpu(const sysinfo_t *s, double secs);

#endif
