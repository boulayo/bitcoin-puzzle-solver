/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* sysinfo.c - hardware discovery: Linux (/proc, /sys, cgroups), macOS (sysctl, Mach) and Windows (Win32 API). */
#include "sysinfo.h"
#include <pthread.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <cpuid.h>

static void cpu_brand(char *out, size_t n)
{
    unsigned r[12];
    if (__get_cpuid_max(0x80000000, NULL) < 0x80000004) { snprintf(out, n, "unknown CPU"); return; }
    for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &r[4 * i], &r[4 * i + 1], &r[4 * i + 2], &r[4 * i + 3]);
    char b[49];
    memcpy(b, r, 48);
    b[48] = 0;
    const char *p = b;
    while (*p == ' ') p++;
    snprintf(out, n, "%s", *p ? p : "unknown CPU");
}

void sysinfo_detect(sysinfo_t *s)
{
    memset(s, 0, sizeof *s);
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);   /* logical processors (SMT included), all groups */
    if (n < 1) n = 1;
    for (DWORD i = 0; i < n && i < 1024; i++) s->cpu_ids[s->ncpu++] = (int)i;
    cpu_brand(s->model, sizeof s->model);
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        s->mem_total = ms.ullTotalPhys;
        s->mem_avail = ms.ullAvailPhys;   /* free + standby (file cache that can be dropped): like Linux MemAvailable */
    }
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = len ? malloc(len) : NULL;
    if (info && GetLogicalProcessorInformation(info, &len)) {
        for (DWORD i = 0; i < len / sizeof *info; i++) {
            if (info[i].Relationship != RelationCache) continue;
            const CACHE_DESCRIPTOR *c = &info[i].Cache;
            if (c->Type != CacheUnified && c->Type != CacheData) continue;
            if (c->Level == 2 && !s->l2_kib) s->l2_kib = (int)(c->Size / 1024);
            if (c->Level == 3 && !s->l3_kib) s->l3_kib = (int)(c->Size / 1024);
        }
    }
    free(info);
    /* physical cores (processor group 0: masks are 64-bit; further logical CPUs count as cores of their own) */
    for (int i = 0; i < s->ncpu; i++) s->core_of[i] = -1;
    len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    info = len ? malloc(len) : NULL;
    if (info && GetLogicalProcessorInformation(info, &len)) {
        for (DWORD i = 0; i < len / sizeof *info; i++) {
            if (info[i].Relationship != RelationProcessorCore) continue;
            for (int c = 0; c < s->ncpu && c < 64; c++)
                if (info[i].ProcessorMask >> s->cpu_ids[c] & 1) s->core_of[c] = s->ncore;
            s->ncore++;
        }
    }
    free(info);
    for (int i = 0; i < s->ncpu; i++)
        if (s->core_of[i] < 0) s->core_of[i] = s->ncore++;
}

void sysinfo_pin_thread(const sysinfo_t *s, int n)
{
    /* a hint only: Windows keeps moving threads for power management, and hard affinity would fight it */
    if (s->ncpu <= 0) return;
    SetThreadIdealProcessor(GetCurrentThread(), (DWORD)(s->cpu_ids[n % s->ncpu] % 64));
}

#elif defined(__APPLE__)
/* macOS: no /proc and no affinity API. The topology comes from sysctl, the free memory from the Mach VM statistics. */
#include <mach/mach.h>
#include <sys/sysctl.h>

static u64 sysctl_num(const char *name)
{
    u64 v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return 0;
    if (len == sizeof(u32)) { u32 d; memcpy(&d, &v, sizeof d); return d; }   /* some keys are 32-bit */
    return v;
}

static bool sysctl_str(const char *name, char *out, size_t n)
{
    size_t len = n;
    if (n == 0 || sysctlbyname(name, out, &len, NULL, 0) != 0 || len == 0) return false;
    out[n - 1] = 0;
    return out[0] != 0;
}

void sysinfo_detect(sysinfo_t *s)
{
    memset(s, 0, sizeof *s);
    u64 logical = sysctl_num("hw.logicalcpu");
    if (logical == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        logical = n > 0 ? (u64)n : 1;
    }
    if (logical > 1024) logical = 1024;
    for (int i = 0; i < (int)logical; i++) s->cpu_ids[s->ncpu++] = i;
    /* macOS exposes no per-CPU topology. The logical CPUs of an SMT core (Intel) are numbered next to each other; Apple Silicon has
     * no SMT, so every logical CPU is a core. */
    u64 physical = sysctl_num("hw.physicalcpu");
    if (physical < 1 || physical > logical) physical = logical;
    const int smt = (int)((logical + physical - 1) / physical);
    for (int i = 0; i < s->ncpu; i++) {
        int c = i / smt;
        if (c >= (int)physical) c = (int)physical - 1;
        s->core_of[i] = c;
        if (c + 1 > s->ncore) s->ncore = c + 1;
    }
    char brand[128];
    if (!sysctl_str("machdep.cpu.brand_string", brand, sizeof brand) && !sysctl_str("hw.model", brand, sizeof brand))
        snprintf(brand, sizeof brand, "unknown CPU");
    const char *b = brand;
    while (*b == ' ') b++;
    pz_strlcpy(s->model, b, sizeof s->model);
    s->mem_total = sysctl_num("hw.memsize");
    if (s->mem_total == 0) s->mem_total = (u64)sysconf(_SC_PHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE);
    /* available = free + inactive pages, or the share the system itself reports as available (kern.memorystatus_level, in percent),
     * whichever is larger */
    u64 avail = 0;
    vm_size_t page = 0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    const mach_port_t host = mach_host_self();
    if (host_page_size(host, &page) == KERN_SUCCESS && host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &cnt) == KERN_SUCCESS)
        avail = ((u64)vm.free_count + (u64)vm.inactive_count) * (u64)page;
    mach_port_deallocate(mach_task_self(), host);
    const u64 level = sysctl_num("kern.memorystatus_level");
    if (level > 0 && level <= 100 && s->mem_total / 100 * level > avail) avail = s->mem_total / 100 * level;
    if (avail == 0 || avail > s->mem_total) avail = s->mem_total / 2;
    s->mem_avail = avail;
    u64 l2 = sysctl_num("hw.l2cachesize");
    if (l2 == 0) l2 = sysctl_num("hw.perflevel0.l2cachesize");   /* Apple Silicon: performance cores */
    s->l2_kib = (int)(l2 / 1024);
    s->l3_kib = (int)(sysctl_num("hw.l3cachesize") / 1024);       /* absent on Apple Silicon */
}

void sysinfo_pin_thread(const sysinfo_t *s, int n)
{
    /* macOS has no thread affinity (THREAD_AFFINITY_POLICY is only a hint and does nothing on Apple Silicon): the scheduler decides */
    (void)s;
    (void)n;
}

#else

/* The value of the first line of a /proc/cpuinfo-style text that starts with `key` (followed by blanks and ':'), or false. */
static bool cpuinfo_field(const char *text, const char *key, char *out, size_t n)
{
    const char *q = pz_kv_find(text, key, ':', PZ_KV_PAD);
    if (!q) return false;
    while (*q == ' ' || *q == '\t') q++;
    size_t i = 0;
    while (*q && *q != '\n' && i + 1 < n) out[i++] = *q++;
    out[i] = 0;
    return i > 0;
}

/* The number after "MemTotal:" and the like, in kiB (0 when the line is missing). */
static u64 meminfo_kib(const char *text, const char *key)
{
    const char *p = pz_kv_find(text, key, ':', PZ_KV_ANYWHERE);
    return p ? pz_strtou64(p) : 0;
}

static int cache_kib(int cpu, int idx)
{
    char path[160];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu, idx);
    char *t = pz_read_file(path, NULL);
    if (!t) return 0;
    int v = pz_atoi(t);
    if (strchr(t, 'M')) v *= 1024;
    free(t);
    return v;
}

/* Memory still available to this process under a cgroup limit (v2 memory.max / v1 limit_in_bytes), or 0 when unlimited/unknown. */
static u64 cgroup_headroom(void)
{
    static const char *const limits[] = {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"};
    static const char *const usages[] = {"/sys/fs/cgroup/memory.current", "/sys/fs/cgroup/memory/memory.usage_in_bytes"};
    for (int i = 0; i < 2; i++) {
        char *l = pz_read_file(limits[i], NULL);
        if (!l) continue;
        bool unlimited = strncmp(l, "max", 3) == 0;
        u64 lim = unlimited ? 0 : pz_strtou64(l);
        free(l);
        if (unlimited || lim == 0 || lim >= ((u64)1 << 60)) return 0;
        char *u = pz_read_file(usages[i], NULL);
        u64 used = u ? pz_strtou64(u) : 0;
        free(u);
        return lim > used ? lim - used : 0;
    }
    return 0;
}

void sysinfo_detect(sysinfo_t *s)
{
    memset(s, 0, sizeof *s);
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int i = 0; i < 1024 && s->ncpu < 1024; i++)
            if (CPU_ISSET(i, &set)) s->cpu_ids[s->ncpu++] = i;
    }
    if (s->ncpu == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        for (int i = 0; i < n && i < 1024; i++) s->cpu_ids[s->ncpu++] = i;
    }
    snprintf(s->model, sizeof s->model, "unknown CPU");
    char *ci = pz_read_file("/proc/cpuinfo", NULL);
    if (ci) {
        char m[128], part[32];
        if (cpuinfo_field(ci, "model name", m, sizeof m) || cpuinfo_field(ci, "Model", m, sizeof m) ||        /* x86; some ARM boards */
            cpuinfo_field(ci, "Hardware", m, sizeof m) || cpuinfo_field(ci, "cpu model", m, sizeof m))
            pz_strlcpy(s->model, m, sizeof s->model);
        else if (cpuinfo_field(ci, "CPU implementer", m, sizeof m) && cpuinfo_field(ci, "CPU part", part, sizeof part))
            snprintf(s->model, sizeof s->model, "ARM CPU (implementer %.16s, part %.16s)", m, part);   /* most ARM kernels give no name */
        free(ci);
    }
    char *mi = pz_read_file("/proc/meminfo", NULL);
    if (mi) {
        s->mem_total = meminfo_kib(mi, "MemTotal") * 1024;
        s->mem_avail = meminfo_kib(mi, "MemAvailable") * 1024;
        free(mi);
    }
    u64 cg = cgroup_headroom();
    if (cg && cg < s->mem_avail) s->mem_avail = cg;       /* inside a container the cgroup limit is what the OOM killer enforces */
    if (cg && cg < s->mem_total) s->mem_total = cg + 0;
    s->l2_kib = cache_kib(s->cpu_ids[0], 2);
    s->l3_kib = cache_kib(s->cpu_ids[0], 3);
    /* physical cores: (package, core id) pairs; without topology every CPU is a core */
    int key[1024];
    for (int i = 0; i < s->ncpu; i++) {
        char path[160];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", s->cpu_ids[i]);
        char *t = pz_read_file(path, NULL);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", s->cpu_ids[i]);
        char *pk = pz_read_file(path, NULL);
        key[i] = t ? ((pk ? pz_atoi(pk) : 0) & 0x7fff) << 16 | (pz_atoi(t) & 0xffff) : -1 - i;   /* the package id is -1 on some ARM systems */
        free(t);
        free(pk);
        int c = -1;
        for (int j = 0; j < i && c < 0; j++)
            if (key[j] == key[i]) c = s->core_of[j];
        s->core_of[i] = c >= 0 ? c : s->ncore++;
    }
}

void sysinfo_pin_thread(const sysinfo_t *s, int n)
{
    if (s->ncpu <= 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(s->cpu_ids[n % s->ncpu], &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

#endif

void sysinfo_spread_order(sysinfo_t *s)
{
    int ids[1024], cores[1024], n = 0;
    bool used[1024] = {false};
    for (int c = 0; c < s->ncore; c++)
        for (int i = 0; i < s->ncpu; i++)
            if (s->core_of[i] == c) {
                ids[n] = s->cpu_ids[i];
                cores[n++] = c;
                used[i] = true;
                break;
            }
    for (int i = 0; i < s->ncpu; i++)
        if (!used[i]) {
            ids[n] = s->cpu_ids[i];
            cores[n++] = s->core_of[i];
        }
    memcpy(s->cpu_ids, ids, (size_t)n * sizeof *ids);
    memcpy(s->core_of, cores, (size_t)n * sizeof *cores);
}

int sysinfo_core_cpus(const sysinfo_t *s, int c, int *pos, int max)
{
    int n = 0;
    for (int i = 0; i < s->ncpu && n < max; i++)
        if (s->core_of[i] == c) pos[n++] = i;
    return n;
}
