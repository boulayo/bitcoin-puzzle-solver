/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* selftest.h - built-in correctness checks and benchmarks. */
#ifndef PZ_SELFTEST_H
#define PZ_SELFTEST_H
int selftest_run(int argc, char **argv);
int bench_run(int argc, char **argv);
/* Linear-search rate of this machine (keys/s, all CPU cores, best engine), measured for `secs` seconds. */
double bench_cpu_rate(double secs);
#endif
