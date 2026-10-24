/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kangaroo.h - interval discrete logarithm with known public key. */
#ifndef PZ_KANGAROO_H
#define PZ_KANGAROO_H
#include "run.h"
#define KANG_C_MEAN 1.06         /* measured mean cost of the kangaroo, in units of sqrt(W) (schema 3, new campaigns: W = 2^32..2^44) */
#define KANG_C_MEAN_V2 1.10      /* the same for schema 2 (campaigns started by 1.7.8: W = 2^32..2^44) */
#define KANG_C_MEAN_V1 1.15      /* the same for the layout of campaigns started by 1.6.0-1.7.7 (1.26 before 1.6.0) */
int kangaroo_selftest(void);
/* Every CPU variant of the kangaroo step against the portable one, bit for bit (prints one line each). */
int kang_round_selftest(void);
/* Name of the CPU kangaroo step in use (chosen by measurement on first use). */
const char *kang_round_name(void);
/* The instruction set of the CPU step variants (fixed for a machine, unlike the measured choice): for keys of saved measurements. */
const char *kang_round_isa(void);
int kangaroo_stats(int argc, char **argv);
/* Conversion of a format-1 DP log (prints one line). */
int kang_log_selftest(void);
int kang_log_fault_selftest(void);   /* the DP log when the disk refuses writes: no overflow, nothing lost, torn records repaired */
int kang_merge_selftest(void);   /* kang_merge_file: collision across two logs, refusal of foreign logs */
/* Builds a synthetic kangaroo state on an interval of 2^wbits keys (for GPU self-tests). Free with kang_free_test_state. */
struct kang_state *kang_make_test_state(unsigned wbits, int nthreads);
/* The same with the layout of the walkers chosen: 1 (campaigns of 1.6.0-1.7.7), 2 (new campaigns), 0 as for a new campaign. */
struct kang_state *kang_make_test_state_s(unsigned wbits, int nthreads, int schema);
void kang_free_test_state(struct kang_state *ks);
/* Runs the CPU kangaroo on [lo, lo + 2^wbits) until `key` is found; returns the steps used or -1. Test overrides: R jumps
 * instead of 256 (R > 0, a power of two), dp_bits (dp > 0), no restart of walkers without distinguished points and the
 * layout of the walkers (schema 1 or 2; 0 as for a new campaign). */
typedef struct { unsigned R, dp; bool no_watchdog; int schema; } kang_try_opts;
double kang_try(const u256 *lo, unsigned wbits, const u256 *key, int nthreads, double timeout, u64 seedn, const kang_try_opts *o);
/* Measures the CPU walk rate (steps/s, all cores) over `secs` seconds; prints when `print` is set. */
double kangaroo_bench(double secs, bool print);
/* 'merge': adds the distinguished points of other computers' logs (or campaign folders) to the campaign of `r`, which must be
 * set up like for a hunt (cfg, paths, lock taken). Returns 10 when the key was found, 1 when a file could not be used, else 0. */
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
