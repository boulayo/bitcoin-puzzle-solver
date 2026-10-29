/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* sysinfo.h - hardware discovery (CPU topology, memory). */
#ifndef PZ_SYSINFO_H
#define PZ_SYSINFO_H

#include "common.h"

typedef struct {
    int ncpu;                 /* CPUs this process may run on */
    int cpu_ids[1024];        /* their ids: one CPU of every physical core first, then the SMT siblings (sysinfo_spread_order) */
    int core_of[1024];        /* physical core of each of them (index in 0 .. ncore-1; SMT siblings share it) */
    int ncore;                /* physical cores among them */
    char model[128];
    u64 mem_total, mem_avail; /* bytes */
    int l2_kib, l3_kib;
} sysinfo_t;

/* Fills s; the CPUs come in the order of sysinfo_spread_order. */
void sysinfo_detect(sysinfo_t *s);
/* Pins the calling thread to the n-th allowed CPU (wraps around). A hint on Windows; does nothing on macOS, which has no affinity API. */
void sysinfo_pin_thread(const sysinfo_t *s, int n);
/* Positions (in cpu_ids) of the CPUs of physical core c, at most `max`; returns how many. */
int sysinfo_core_cpus(const sysinfo_t *s, int c, int *pos, int max);
/* Reorders the CPUs so that the first of every physical core comes first, the SMT siblings after: the threads pinned to positions
 * 0, 1, ... (sysinfo_pin_thread) then fill the cores one by one instead of putting two threads on a core while others stay idle. */
void sysinfo_spread_order(sysinfo_t *s);

#endif
