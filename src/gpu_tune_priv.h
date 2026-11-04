/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_tune_priv.h - what gpu_tune.c (the choice of a variant for a device) shares with gpu_bench.c (the benchmark and 'bench --variants')
 * and gpu_tune_selftest.c (the self-test of the choice): the description of the tuning of a kernel, its constants, and the steps of the
 * procedure that the benchmark repeats and the self-test drives. */
#ifndef PZ_GPU_TUNE_PRIV_H
#define PZ_GPU_TUNE_PRIV_H

#include "gpu_priv.h"

/* How the choice of a variant differs between the two kernels. gpu_tune_pick does the rest, which is the same for both. */
typedef struct {
    bool kang;                    /* the kangaroo kernel (else the linear one): the key of the saved choice */
    int nv, vs[32], def;          /* the variants to try, and the default one */
    double (*measure)(gpu_session *s, int variant);     /* sets the variant, measures it: units per second, 0 when it cannot run */
    double warm_s;                                      /* seconds of the default variant before the measurements (0: none) */
    void (*set)(gpu_session *s, int variant);
    bool (*check)(gpu_session *s);                      /* the bit-exact test of the variant that is set */
    void (*finish)(gpu_session *s);                     /* what the check leaves behind to release (may be NULL) */
    int (*code)(const gpu_session *s);                  /* the variant that is set, as it is saved */
    char *(*name)(int variant, char *buf, size_t n);
    char *(*code_text)(int variant, char *buf, size_t n);         /* the code as PZ_GPU_HALF or PZ_GPU_NK takes it ("-": none takes it) */
    /* the length of a launch (tune_at_length) of the variant that is set; NULL: not tuned, GPU_LAUNCH_S */
    double (*at_length)(gpu_session *s, double target, double *got, uint32_t *size);
    const char *size_unit;                              /* what the size of a launch counts: "groups per work-item", "rounds" */
    /* the log lines: GPU[%d] and the name of the variant that was saved; GPU[%d] and the number of variants; the title of the table of rates
     * (and of the line of the choice) and the unit of the rates; the word before each variant in them and after "picking"; what the table says
     * of a variant that cannot run (NULL: its rate, 0 included) */
    const char *cached_fmt, *intro_fmt, *head, *unit, *entry, *no_rate;
    const char *no_data_fmt;      /* when no variant could run: the table and the default; NULL: no line */
    const char *bad_check_fmt;    /* when the choice fails the check: GPU[%d] and its name */
} tune_ops;

/* The choice of a variant. Each one is measured for a second or so, which on a laptop GPU held back by its power limit or its temperature is
 * noisy by 2 to 3 % (the same variant measured twice), and the fastest of eight noisy rates is lucky by about as much: with one measurement
 * each and a margin of 3 % (1.19.0 to 1.19.2) the choice between two variants 2.8 % apart, window 256 and window 512 with the register cap on
 * the RTX A2000 Laptop, depended on the run. So: the device is warmed up first (TUNE_WARM_S of the default variant, not counted: the clocks of
 * a card at rest rise during the first seconds), the variants are measured in TUNE_TURNS turns (all of them in order, again in the reverse
 * order, then in order: a drift of the clock weighs the same on every variant), only those within TUNE_NEAR of the fastest of the first turn
 * (and the default) go through all the turns (a variant 5 % behind does not come back when the noise is 3 %), and each one gets the median of
 * its rates, which one lucky or unlucky turn does not move. The default stays unless another is faster by more than TUNE_MARGIN: the median
 * of three is still off by about 1 %, so a smaller margin would pick by luck, and a larger one misses the 2.8 % above. More turns would narrow
 * the margin by the square root of their number only, for a tuning that already takes about a minute per kernel on a laptop. */
#define TUNE_TURNS 3
#define TUNE_TURNS_MAX 9          /* 'bench --variants --turns N' at most */
#define TUNE_NEAR 0.95
#define TUNE_MARGIN 1.01
#define TUNE_WARM_S 3.0

/* gpu_tune.c: the steps of the procedure (the rules above), the saved choices and the two kernels' descriptions of their tuning */
double gpu_tune_median(const double *s, int n);
int gpu_tune_choose(const tune_ops *op, const double *rate, int nv, int di);
void gpu_tune_turns(gpu_session *s, const tune_ops *op, int turns, bool cut, int di, double smp[][TUNE_TURNS_MAX], int *nsmp);
int gpu_tune_launch_choose(const double *rate, int n);
void gpu_tune_pick(gpu_session *s, int idx, const tune_ops *op);
void gpu_tune_linear_ops(const rt_dev *d, tune_ops *op);
void gpu_tune_kang_ops(const rt_dev *d, tune_ops *op);
bool gpu_retune_requested(void);                        /* 'bench --retune' */

/* the saved choices (gpu_tune.txt): its path, the key of a device, the reading of a value and of a device's line, and the writing of it */
bool gpu_tune_cache_path(char *out, size_t n);
void gpu_tune_key(const rt_dev *d, bool kang, char *out, size_t n);
int gpu_tune_value(const char *e, double *launch_s);
int gpu_tune_cache_get(const rt_dev *d, bool kang, double *launch_s);
void gpu_tune_cache_put(const rt_dev *d, bool kang, int variant, double launch_s);

/* the variant that is set in a session, as the kernel and the file take it, and the measurements of the variant that is set */
bool gpu_tune_linear_probe(gpu_session *s, pz_u256 *start, uint32_t ng, lin_launch *L, double *dt);
double gpu_tune_kang_measure(gpu_session *s);
void gpu_tune_linear_set_variant(gpu_session *s, int v);
int gpu_tune_linear_code(const gpu_session *s);
void gpu_tune_kang_set_variant(gpu_session *s, int v);
int gpu_tune_kang_code(const gpu_session *s);
int gpu_tune_default_kvariant(const rt_dev *d);
size_t gpu_tune_kang_threads_for(const rt_dev *d, int nk, bool l2fit, int wide, bool block);

/* the name of a variant as the logs and tables print it, and its code as PZ_GPU_HALF or PZ_GPU_NK takes it ("-": none takes it) */
char *gpu_variant_name(int v, char *b, size_t n);
char *gpu_kvariant_name(int v, char *b, size_t n);
char *gpu_variant_code(int v, char *b, size_t n);
char *gpu_kvariant_code(int v, char *b, size_t n);

#endif
