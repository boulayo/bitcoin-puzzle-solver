/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* selftest.h - built-in correctness checks and benchmarks. */
#ifndef PZ_SELFTEST_H
#define PZ_SELFTEST_H
#include <stdbool.h>
int selftest_run(int argc, char **argv);
/* The checks of selftest_run; `banner`: the title and the closing summary ('doctor --full' prints its own). Nonzero when one failed. */
int selftest_checks(bool banner);
int bench_run(int argc, char **argv);
/* Linear-search rate of this machine (keys/s, all CPU cores, best engine), measured for `secs` seconds. */
double bench_cpu_rate(double secs);
#endif
