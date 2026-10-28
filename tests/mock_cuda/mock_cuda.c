/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* mock_cuda.c - a fake CUDA driver (libcuda.so.1 / nvcuda.dll) for testing the CUDA code path without a GPU.
 *
 * It implements the driver-API entry points pzsolve uses. Modules are recognised by comparing the PTX image with
 * the embedded copies of src/kernels/ptx/*.ptx; launches run the very same kernel sources compiled as C (kern.c), one
 * work-item after the other. Every call is checked: unknown pointers, copies outside an allocation, launch shapes the
 * kernels were not compiled for, wrong argument lists - any misuse by the host code fails the call and is printed.
 * What it cannot check is the PTX itself (that is the NVIDIA driver's JIT) - the GPU self-test covers it at start-up. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef unsigned int uint;
typedef unsigned long long CUdeviceptr;
typedef int CUresult;

unsigned pz_mock_gid;

extern const unsigned char mock_ptx_linear128[], mock_ptx_linear256[], mock_ptx_linear512[], mock_ptx_linear128m4[], mock_ptx_linear256m4[],
    mock_ptx_linear512m4[], mock_ptx_kang64[], mock_ptx_kang128[], mock_ptx_kang256[], mock_ptx_kang64b[], mock_ptx_kang128b[],
    mock_ptx_kang256b[], mock_ptx_kang16[], mock_ptx_kang32[], mock_ptx_kang16b[], mock_ptx_kang32b[], mock_ptx_kang64c[], mock_ptx_kang128c[], mock_ptx_test[];

typedef void (*search_fn)(const uint *prm, const uint *gtab, uint *hits, const uint max_hits, const uint pz_n);
typedef void (*kang_fn)(uint *state, const uint *jump, uint *dps, uint *dpcount, uint *rlist, uint *rcount, unsigned short *hist, const uint rounds,
                        const uint dp_mask_lo, const uint dp_mask_hi, const uint cap_lo, const uint cap_hi, const uint dps_cap, const uint rlist_cap,
                        const uint R, const uint step_lo, const uint step_hi, const uint pz_n);
#define DECL_SEARCH(h) void mock_search##h(const uint *prm, const uint *gtab, uint *hits, const uint max_hits, const uint pz_n);
#define DECL_KANG(n) void mock_kang##n(uint *state, const uint *jump, uint *dps, uint *dpcount, uint *rlist, uint *rcount, unsigned short *hist, \
                                       const uint rounds, const uint dp_mask_lo, const uint dp_mask_hi, const uint cap_lo, const uint cap_hi, \
                                       const uint dps_cap, const uint rlist_cap, const uint R, const uint step_lo, const uint step_hi, const uint pz_n);
DECL_SEARCH(128) DECL_SEARCH(256) DECL_SEARCH(512)
DECL_KANG(16) DECL_KANG(32) DECL_KANG(64) DECL_KANG(128) DECL_KANG(256) DECL_KANG(16b) DECL_KANG(32b) DECL_KANG(64c) DECL_KANG(128c) DECL_KANG(64b) DECL_KANG(128b) DECL_KANG(256b)
void pz_test_fe(const uint *in, uint *out, const uint pz_n);
void pz_test_hash(const uint *in, uint *out, const uint pz_n);
void pz_test_ec(const uint *in, uint *out, const uint pz_n);
void pz_diag_bw(const uint *src, uint *dst, const uint wpi, const uint pz_n);
void pz_diag_alu(uint *out, const uint mode, const uint iters, const uint pz_n);

enum { E_OK = 0, E_INVALID_VALUE = 1, E_OUT_OF_MEMORY = 2, E_INVALID_IMAGE = 200, E_INVALID_CONTEXT = 201, E_NOT_FOUND = 500, E_LAUNCH = 701 };

static int g_init;
static int g_ctx_token = 0x5a5a;
static void *g_current;

static int fail(int code, const char *what)
{
    fprintf(stderr, "[mock CUDA] misuse: %s\n", what);
    return code;
}

/* ---- allocations (checked) ---- */
typedef struct { char *p; size_t n; } alloc_t;
static alloc_t g_allocs[4096];

static alloc_t *find_alloc(CUdeviceptr d, size_t n)
{
    char *p = (char *)(uintptr_t)d;
    for (int i = 0; i < 4096; i++)
        if (g_allocs[i].p && p >= g_allocs[i].p && p + n <= g_allocs[i].p + g_allocs[i].n) return &g_allocs[i];
    return NULL;
}

/* ---- modules and functions ---- */
/* the modules the driver knows (by their exact PTX text) and the C build of each kernel */
enum { M_LINEAR, M_KANG, M_TEST };
static const struct { const unsigned char *ptx; int kind; search_fn search; kang_fn kang; } MODS[] = {
    {mock_ptx_linear128, M_LINEAR, mock_search128, NULL}, {mock_ptx_linear256, M_LINEAR, mock_search256, NULL},
    {mock_ptx_linear512, M_LINEAR, mock_search512, NULL}, {mock_ptx_linear128m4, M_LINEAR, mock_search128, NULL},
    {mock_ptx_linear256m4, M_LINEAR, mock_search256, NULL}, {mock_ptx_linear512m4, M_LINEAR, mock_search512, NULL},
    {mock_ptx_kang64, M_KANG, NULL, mock_kang64}, {mock_ptx_kang128, M_KANG, NULL, mock_kang128}, {mock_ptx_kang256, M_KANG, NULL, mock_kang256},
    {mock_ptx_kang64b, M_KANG, NULL, mock_kang64b}, {mock_ptx_kang128b, M_KANG, NULL, mock_kang128b}, {mock_ptx_kang256b, M_KANG, NULL, mock_kang256b},
    {mock_ptx_kang16, M_KANG, NULL, mock_kang16}, {mock_ptx_kang32, M_KANG, NULL, mock_kang32}, {mock_ptx_kang16b, M_KANG, NULL, mock_kang16b},
    {mock_ptx_kang32b, M_KANG, NULL, mock_kang32b}, {mock_ptx_kang64c, M_KANG, NULL, mock_kang64c}, {mock_ptx_kang128c, M_KANG, NULL, mock_kang128c},
    {mock_ptx_test, M_TEST, NULL, NULL},
};
#define M_COUNT ((int)(sizeof MODS / sizeof *MODS))
typedef struct { int kind; } module_t;          /* kind = index in MODS + 1 (0 = free slot) */
typedef struct { int kind; int which; } func_t;   /* kind = index in MODS */
static module_t g_mods[64];
static func_t g_funcs[256];
static int g_nfuncs;

/* PZ_MOCK_COUNT=1: at exit, how many modules were loaded (each one is a JIT compilation on a real driver) and kernels launched. */
static unsigned long g_loads, g_unloads, g_launches;
static void report_counts(void)
{
    fprintf(stderr, "[mock CUDA] cuModuleLoadDataEx: %lu, cuModuleUnload: %lu, cuLaunchKernel: %lu\n", g_loads, g_unloads, g_launches);
}

EXPORT CUresult cuInit(unsigned flags)
{
    if (flags) return fail(E_INVALID_VALUE, "cuInit flags");
    if (!g_init && getenv("PZ_MOCK_COUNT")) atexit(report_counts);
    g_init = 1;
    return E_OK;
}
EXPORT CUresult cuDriverGetVersion(int *v) { *v = 12080; return E_OK; }
EXPORT CUresult cuDeviceGetCount(int *n) { if (!g_init) return 3; *n = 1; return E_OK; }
EXPORT CUresult cuDeviceGet(int *d, int ord) { if (ord != 0) return fail(E_INVALID_VALUE, "device ordinal"); *d = 0; return E_OK; }
EXPORT CUresult cuDeviceGetName(char *name, int len, int d) { (void)d; snprintf(name, (size_t)len, "Mock CUDA GPU (CPU)"); return E_OK; }
EXPORT CUresult cuDeviceTotalMem_v2(size_t *b, int d) { (void)d; *b = (size_t)1 << 30; return E_OK; }

EXPORT CUresult cuDeviceGetAttribute(int *v, int attr, int d)
{
    (void)d;
    switch (attr) {
    case 13: *v = 1500000; return E_OK;   /* clock, kHz */
    case 16: *v = 2; return E_OK;         /* SMs (small: the mock runs on the CPU) */
    case 18: *v = 0; return E_OK;         /* integrated */
    case 36: *v = 6001000; return E_OK;   /* memory clock, kHz */
    case 38: *v = 10 << 20; return E_OK;  /* L2 cache (large enough to exercise the cache-sized herd variants) */
    case 81: *v = 102400; return E_OK;    /* shared memory per SM */
    case 37: *v = 128; return E_OK;       /* memory bus width, bits */
    case 33: *v = 99; return E_OK;        /* PCI bus */
    case 34: *v = 0; return E_OK;
    case 75: *v = 8; return E_OK;         /* compute capability 8.6 */
    case 76: *v = 6; return E_OK;
    default: return fail(E_INVALID_VALUE, "unknown device attribute");
    }
}

EXPORT CUresult cuCtxCreate_v2(void **ctx, unsigned flags, int d)
{
    (void)d;
    if (flags & ~7u) return fail(E_INVALID_VALUE, "context flags");
    *ctx = &g_ctx_token;
    g_current = *ctx;
    return E_OK;
}
EXPORT CUresult cuCtxDestroy_v2(void *ctx) { if (ctx != &g_ctx_token) return fail(E_INVALID_CONTEXT, "cuCtxDestroy"); return E_OK; }
EXPORT CUresult cuCtxSetCurrent(void *ctx) { if (ctx != &g_ctx_token) return fail(E_INVALID_CONTEXT, "cuCtxSetCurrent"); g_current = ctx; return E_OK; }
EXPORT CUresult cuCtxSynchronize(void) { return g_current ? E_OK : fail(E_INVALID_CONTEXT, "no current context"); }

EXPORT CUresult cuModuleLoadDataEx(void **mod, const void *image, unsigned nopt, int *opts, void **vals)
{
    (void)opts; (void)vals;
    g_loads++;
    if (!g_current) return fail(E_INVALID_CONTEXT, "module load without a context");
    if (nopt > 8) return fail(E_INVALID_VALUE, "opzioni JIT");
    for (int k = 0; k < M_COUNT; k++) {
        if (strcmp((const char *)image, (const char *)MODS[k].ptx) != 0) continue;
        for (int i = 0; i < 64; i++)
            if (!g_mods[i].kind) { g_mods[i].kind = k + 1; *mod = &g_mods[i]; return E_OK; }
        return fail(E_OUT_OF_MEMORY, "too many modules");
    }
    return fail(E_INVALID_IMAGE, "unknown PTX image");
}

EXPORT CUresult cuModuleUnload(void *mod)
{
    module_t *m = mod;
    if (!m || m < g_mods || m >= g_mods + 64 || !m->kind) return fail(E_INVALID_VALUE, "cuModuleUnload");
    g_unloads++;
    m->kind = 0;
    return E_OK;
}

EXPORT CUresult cuModuleGetFunction(void **f, void *mod, const char *name)
{
    module_t *m = mod;
    if (!m || !m->kind) return fail(E_INVALID_VALUE, "invalid module");
    int kind = m->kind - 1, which = -1;
    if (MODS[kind].kind == M_LINEAR && !strcmp(name, "pz_search")) which = 0;
    if (MODS[kind].kind == M_KANG && !strcmp(name, "pz_kang_step")) which = 0;
    if (MODS[kind].kind == M_TEST)
        which = !strcmp(name, "pz_test_fe") ? 0 : !strcmp(name, "pz_test_hash") ? 1 : !strcmp(name, "pz_test_ec") ? 2
              : !strcmp(name, "pz_diag_bw") ? 3 : !strcmp(name, "pz_diag_alu") ? 4 : -1;
    if (which < 0) return E_NOT_FOUND;
    if (g_nfuncs >= 256) return fail(E_OUT_OF_MEMORY, "too many functions");
    g_funcs[g_nfuncs] = (func_t){kind, which};
    *f = &g_funcs[g_nfuncs++];
    return E_OK;
}

EXPORT CUresult cuFuncGetAttribute(int *v, int attr, void *f)
{
    (void)f;
    if (attr != 0) return fail(E_INVALID_VALUE, "function attribute");
    *v = 128;   /* __launch_bounds__(128) */
    return E_OK;
}

EXPORT CUresult cuMemAlloc_v2(CUdeviceptr *d, size_t n)
{
    if (!g_current) return fail(E_INVALID_CONTEXT, "allocation without a context");
    if (n == 0) return fail(E_INVALID_VALUE, "allocation of 0 bytes");
    for (int i = 0; i < 4096; i++) {
        if (g_allocs[i].p) continue;
        char *p = malloc(n);
        if (!p) return E_OUT_OF_MEMORY;
        memset(p, 0xA5, n);   /* like real device memory: garbage until written */
        g_allocs[i] = (alloc_t){p, n};
        *d = (CUdeviceptr)(uintptr_t)p;
        return E_OK;
    }
    return E_OUT_OF_MEMORY;
}

EXPORT CUresult cuMemFree_v2(CUdeviceptr d)
{
    for (int i = 0; i < 4096; i++)
        if (g_allocs[i].p == (char *)(uintptr_t)d) { free(g_allocs[i].p); g_allocs[i].p = NULL; return E_OK; }
    return fail(E_INVALID_VALUE, "cuMemFree of an unknown pointer");
}

EXPORT CUresult cuMemcpyHtoD_v2(CUdeviceptr d, const void *src, size_t n)
{
    if (!find_alloc(d, n)) return fail(E_INVALID_VALUE, "copy to the GPU outside an allocation");
    memcpy((void *)(uintptr_t)d, src, n);
    return E_OK;
}

EXPORT CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr d, size_t n)
{
    if (!find_alloc(d, n)) return fail(E_INVALID_VALUE, "copy from the GPU outside an allocation");
    memcpy(dst, (const void *)(uintptr_t)d, n);
    return E_OK;
}

EXPORT CUresult cuMemsetD8_v2(CUdeviceptr d, unsigned char v, size_t n)
{
    if (!find_alloc(d, n)) return fail(E_INVALID_VALUE, "memset outside an allocation");
    memset((void *)(uintptr_t)d, v, n);
    return E_OK;
}

static void *ptr_arg(void **params, int i, int *bad)
{
    CUdeviceptr d = *(CUdeviceptr *)params[i];
    if (!find_alloc(d, 1)) { *bad = 1; return NULL; }
    return (void *)(uintptr_t)d;
}

EXPORT CUresult cuLaunchKernel(void *fv, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz, unsigned shmem, void *stream,
                               void **params, void **extra)
{
    func_t *f = fv;
    g_launches++;
    /* PZ_MOCK_FAIL_FILE=path: while that file exists, the next launch fails once and removes it, like a device reset after
     * the machine slept (the test creates the file while a campaign runs) */
    static const char *fault_file;
    static int fault_init;
    if (!fault_init) { fault_init = 1; fault_file = getenv("PZ_MOCK_FAIL_FILE"); }
    if (fault_file) {
        FILE *fp = fopen(fault_file, "r");
        if (fp) {
            fclose(fp);
            remove(fault_file);
            return fail(E_LAUNCH, "injected failure (PZ_MOCK_FAIL_FILE)");
        }
    }
    if (!g_current) return fail(E_INVALID_CONTEXT, "launch without a context");
    if (!f || f < g_funcs || f >= g_funcs + g_nfuncs) return fail(E_INVALID_VALUE, "unknown function");
    if (gy != 1 || gz != 1 || by != 1 || bz != 1 || shmem || stream || extra || !params) return fail(E_INVALID_VALUE, "launch shape");
    if (bx == 0 || bx > 128 || gx == 0) return fail(E_INVALID_VALUE, "block above __launch_bounds__(128) or empty grid");
    const unsigned long long total = (unsigned long long)gx * bx;
    int bad = 0;
    if (MODS[f->kind].kind == M_LINEAR) {
        const uint *prm = ptr_arg(params, 0, &bad), *gtab = ptr_arg(params, 1, &bad);
        uint *hits = ptr_arg(params, 2, &bad);
        uint maxh = *(uint *)params[3], n = *(uint *)params[4];
        if (bad) return fail(E_INVALID_VALUE, "invalid pointer argument (pz_search)");
        if (n > total || total - n >= bx) return fail(E_INVALID_VALUE, "grid inconsistent with the number of work-items");
        search_fn k = MODS[f->kind].search;
        for (unsigned long long g = 0; g < total; g++) { pz_mock_gid = (unsigned)g; k(prm, gtab, hits, maxh, n); }
        return E_OK;
    }
    if (MODS[f->kind].kind == M_KANG) {
        uint *a[7];
        for (int i = 0; i < 7; i++) a[i] = ptr_arg(params, i, &bad);
        uint u[11];
        for (int i = 0; i < 11; i++) u[i] = *(uint *)params[7 + i];
        if (bad) return fail(E_INVALID_VALUE, "invalid pointer argument (pz_kang_step)");
        if (u[10] > total || total - u[10] >= bx) return fail(E_INVALID_VALUE, "grid inconsistent with the number of work-items");
        /* the history must hold rounds * NK * work-items entries of 16 bits: a launch with more rounds would write past it */
        for (unsigned long long g = 0; g < total; g++) {
            pz_mock_gid = (unsigned)g;
            MODS[f->kind].kang(a[0], a[1], a[2], a[3], a[4], a[5], (unsigned short *)a[6], u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10]);
        }
        return E_OK;
    }
    if (f->which == 3) {   /* pz_diag_bw(src, dst, wpi, n): every access must stay inside its allocation */
        const uint *src = ptr_arg(params, 0, &bad);
        uint *dst = ptr_arg(params, 1, &bad);
        uint wpi = *(uint *)params[2], n = *(uint *)params[3];
        if (bad) return fail(E_INVALID_VALUE, "invalid pointer argument (pz_diag_bw)");
        if (n > total || total - n >= bx) return fail(E_INVALID_VALUE, "grid inconsistent with the number of work-items");
        const size_t bytes = (size_t)wpi * n * 4;
        if (!find_alloc((CUdeviceptr)(uintptr_t)src, bytes) || !find_alloc((CUdeviceptr)(uintptr_t)dst, bytes)) return fail(E_INVALID_VALUE, "pz_diag_bw: access beyond the allocation");
        for (unsigned long long g = 0; g < total; g++) { pz_mock_gid = (unsigned)g; pz_diag_bw(src, dst, wpi, n); }
        return E_OK;
    }
    if (f->which == 4) {   /* pz_diag_alu(out, mode, iters, n) */
        uint *o = ptr_arg(params, 0, &bad);
        uint mode = *(uint *)params[1], iters = *(uint *)params[2], n = *(uint *)params[3];
        if (bad) return fail(E_INVALID_VALUE, "invalid pointer argument (pz_diag_alu)");
        if (n > total || total - n >= bx) return fail(E_INVALID_VALUE, "grid inconsistent with the number of work-items");
        if (!find_alloc((CUdeviceptr)(uintptr_t)o, (size_t)n * 4)) return fail(E_INVALID_VALUE, "pz_diag_alu: access beyond the allocation");
        for (unsigned long long g = 0; g < total; g++) { pz_mock_gid = (unsigned)g; pz_diag_alu(o, mode, iters, n); }
        return E_OK;
    }
    const uint *in = ptr_arg(params, 0, &bad);
    uint *out = ptr_arg(params, 1, &bad);
    uint n = *(uint *)params[2];
    if (bad) return fail(E_INVALID_VALUE, "invalid pointer argument (test)");
    if (n > total || total - n >= bx) return fail(E_INVALID_VALUE, "grid inconsistent with the number of work-items");
    void (*k)(const uint *, uint *, const uint) = f->which == 0 ? pz_test_fe : f->which == 1 ? pz_test_hash : pz_test_ec;
    for (unsigned long long g = 0; g < total; g++) { pz_mock_gid = (unsigned)g; k(in, out, n); }
    return E_OK;
}

EXPORT CUresult cuGetErrorString(CUresult rc, const char **s)
{
    *s = rc == 0 ? "no error" : "mock CUDA error";
    return E_OK;
}
