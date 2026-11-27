/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_devices.c - 'pzsolve devices': the CPU and its engines, the RAM and the GPUs. */
#include "cmd.h"
#include "gpu.h"
#include "outfmt.h"

int cmd_devices(void)
{
    sysinfo_t s;
    sysinfo_detect(&s);
    char ram_total_text[32], ram_avail_text[32];
    pz_section("CPU");
    pz_prop_line("model", "%s", s.model);
    pz_prop_line("threads", "%d usable logical thread%s on %d physical core%s", s.ncpu, PLURAL(s.ncpu), s.ncore, PLURAL(s.ncore));
    pz_prop_line("RAM", "%s total, %s available", pz_fmt_bytes((double)s.mem_total, ram_total_text, sizeof ram_total_text), pz_fmt_bytes((double)s.mem_avail, ram_avail_text, sizeof ram_avail_text));
    pz_section("CPU engines");
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    for (int i = 0; i < n; i++) printf("  %-10s%2d lanes   %s\n", all[i]->name, all[i]->lanes, all[i]->supported() ? "available" : "not supported by this CPU");
    pz_section("GPUs (CUDA and OpenCL)");
    gpu_list_devices();
    return 0;
}
