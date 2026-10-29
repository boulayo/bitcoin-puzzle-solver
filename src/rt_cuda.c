/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* rt_cuda.c - CUDA implementation of the GPU runtime interface.
 *
 * Uses the CUDA *driver* API (nvcuda.dll on Windows, libcuda.so.1 on Linux), which every NVIDIA driver installs:
 * no CUDA toolkit is needed. The kernels are the OpenCL C sources compiled to PTX ahead of time by NVRTC
 * (tools/gen_ptx.py) and embedded in the program, compressed (lz.h); the module to load is unpacked just before. The driver
 * JIT-compiles the PTX for the GPU at hand on first use and keeps the result in its cache. */
#include "rt_priv.h"
#include "platform.h"
#include "lz.h"
#include <pthread.h>

#define RT_CUDA_BLOCK 128          /* threads per block: the kernels are compiled with __launch_bounds__(128) */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

enum {
    CU_DEVICE_ATTRIBUTE_CLOCK_RATE = 13,
    CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT = 16,
    CU_DEVICE_ATTRIBUTE_INTEGRATED = 18,
    CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE = 36,
    CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE = 38,
    CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR = 81,
    CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH = 37,
    CU_DEVICE_ATTRIBUTE_PCI_BUS_ID = 33,
    CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID = 34,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76,
    CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK = 0,
    CU_CTX_SCHED_BLOCKING_SYNC = 4,   /* wait for kernels without spinning: the core stays available to the CPU workers */
    CU_JIT_ERROR_LOG_BUFFER = 5,
    CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6,
};

static struct {
    void *lib;
    CUresult (*Init)(unsigned);
    CUresult (*DriverGetVersion)(int *);
    CUresult (*DeviceGetCount)(int *);
    CUresult (*DeviceGet)(CUdevice *, int);
    CUresult (*DeviceGetName)(char *, int, CUdevice);
    CUresult (*DeviceTotalMem)(size_t *, CUdevice);
    CUresult (*DeviceGetAttribute)(int *, int, CUdevice);
    CUresult (*CtxCreate)(CUcontext *, unsigned, CUdevice);
    CUresult (*CtxDestroy)(CUcontext);
    CUresult (*CtxSetCurrent)(CUcontext);
    CUresult (*CtxSynchronize)(void);
    CUresult (*ModuleLoadDataEx)(CUmodule *, const void *, unsigned, int *, void **);
    CUresult (*ModuleUnload)(CUmodule);
    CUresult (*ModuleGetFunction)(CUfunction *, CUmodule, const char *);
    CUresult (*FuncGetAttribute)(int *, int, CUfunction);
    CUresult (*MemAlloc)(CUdeviceptr *, size_t);
    CUresult (*MemFree)(CUdeviceptr);
    CUresult (*MemcpyHtoD)(CUdeviceptr, const void *, size_t);
    CUresult (*MemcpyDtoH)(void *, CUdeviceptr, size_t);
    CUresult (*MemsetD8)(CUdeviceptr, unsigned char, size_t);
    CUresult (*LaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream, void **, void **);
    CUresult (*GetErrorString)(CUresult, const char **);
} cu;

static int g_state;                /* 0 untried, 1 loaded and initialised, -1 unavailable */
static int g_driver_version;
static char g_why[120];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void cu_load(void)
{
#ifdef _WIN32
    const char *names[] = {"nvcuda.dll", NULL};
#else
    const char *names[] = {"libcuda.so.1", "libcuda.so", NULL};
#endif
    cu.lib = pz_lib_open(names);
    if (!cu.lib) { snprintf(g_why, sizeof g_why, "CUDA driver not found"); g_state = -1; return; }
#define SYM(field, name) do { *(void **)&cu.field = pz_lib_sym(cu.lib, name); if (!cu.field) { snprintf(g_why, sizeof g_why, "CUDA driver lacks %s (too old?)", name); g_state = -1; return; } } while (0)
    SYM(Init, "cuInit");
    SYM(DriverGetVersion, "cuDriverGetVersion");
    SYM(DeviceGetCount, "cuDeviceGetCount");
    SYM(DeviceGet, "cuDeviceGet");
    SYM(DeviceGetName, "cuDeviceGetName");
    SYM(DeviceTotalMem, "cuDeviceTotalMem_v2");
    SYM(DeviceGetAttribute, "cuDeviceGetAttribute");
    SYM(CtxCreate, "cuCtxCreate_v2");
    SYM(CtxDestroy, "cuCtxDestroy_v2");
    SYM(CtxSetCurrent, "cuCtxSetCurrent");
    SYM(CtxSynchronize, "cuCtxSynchronize");
    SYM(ModuleLoadDataEx, "cuModuleLoadDataEx");
    SYM(ModuleUnload, "cuModuleUnload");
    SYM(ModuleGetFunction, "cuModuleGetFunction");
    SYM(FuncGetAttribute, "cuFuncGetAttribute");
    SYM(MemAlloc, "cuMemAlloc_v2");
    SYM(MemFree, "cuMemFree_v2");
    SYM(MemcpyHtoD, "cuMemcpyHtoD_v2");
    SYM(MemcpyDtoH, "cuMemcpyDtoH_v2");
    SYM(MemsetD8, "cuMemsetD8_v2");
    SYM(LaunchKernel, "cuLaunchKernel");
    SYM(GetErrorString, "cuGetErrorString");
#undef SYM
    CUresult rc = cu.Init(0);
    if (rc != 0) { snprintf(g_why, sizeof g_why, "cuInit failed (code %d): no usable NVIDIA GPU", rc); g_state = -1; return; }
    cu.DriverGetVersion(&g_driver_version);
    g_state = 1;
}

static bool cu_ready(void)
{
    pthread_once(&g_once, cu_load);
    return g_state > 0;
}

static bool cuok(CUresult rc, const char *what)
{
    if (rc == 0) return true;
    const char *s = NULL;
    if (cu.GetErrorString) cu.GetErrorString(rc, &s);
    rt_log(PZ_LOG_ERR, "CUDA: %s failed (code %d%s%s)", what, rc, s ? ": " : "", s ? s : "");
    return false;
}

int rtcu_enumerate(rt_dev *out, int max, char *note, size_t nl)
{
    if (!cu_ready()) { snprintf(note, nl, "%s", g_why); return 0; }
    int count = 0;
    if (cu.DeviceGetCount(&count) != 0 || count <= 0) { snprintf(note, nl, "no CUDA GPU"); return 0; }
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        CUdevice d;
        if (cu.DeviceGet(&d, i) != 0) continue;
        rt_dev *g = &out[n];
        memset(g, 0, sizeof *g);
        g->api = RT_CUDA;
        g->cu_ordinal = i;
        g->is_gpu = true;
        cu.DeviceGetName(g->name, (int)sizeof g->name, d);
        size_t mem = 0;
        cu.DeviceTotalMem(&mem, d);
        g->gmem = mem;
        int v = 0, major = 0, minor = 0;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, d) == 0) g->cu = (unsigned)v;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_CLOCK_RATE, d) == 0) g->mhz = (unsigned)(v / 1000);
        cu.DeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, d);
        cu.DeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, d);
        g->cc = major * 10 + minor;
        int mclk = 0, bus = 0;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_INTEGRATED, d) == 0) g->unified = v != 0;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE, d) == 0 && v > 0) g->l2_bytes = (u64)v;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, d) == 0 && v > 0) g->smem_bytes = (u64)v;
        if (cu.DeviceGetAttribute(&mclk, CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE, d) == 0 && cu.DeviceGetAttribute(&bus, CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH, d) == 0)
            g->peak_gbs = 2.0 * mclk * 1e3 * (bus / 8.0) / 1e9;   /* double data rate */
        g->pci_bus = g->pci_dev = -1;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, d) == 0) g->pci_bus = v;
        if (cu.DeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, d) == 0) g->pci_dev = v;
        pz_strlcpy(g->vendor, "NVIDIA", sizeof g->vendor);
        snprintf(g->plat_name, sizeof g->plat_name, "CUDA driver %d.%d", g_driver_version / 1000, (g_driver_version % 1000) / 10);
        snprintf(g->version, sizeof g->version, "sm_%d", g->cc);
        n++;
    }
    return n;
}

static bool use_ctx(rt_ctx *c) { return cuok(cu.CtxSetCurrent((CUcontext)c->a), "cuCtxSetCurrent"); }

bool rtcu_open(rt_ctx *c)
{
    if (!cu_ready()) return false;
    if (c->dev.cc && c->dev.cc < 52) {
        rt_log(PZ_LOG_ERR, "CUDA: %s has compute capability %d.%d, 5.2 (Maxwell) or newer is required", c->dev.name, c->dev.cc / 10, c->dev.cc % 10);
        return false;
    }
    CUdevice d;
    CUcontext ctx = NULL;
    if (!cuok(cu.DeviceGet(&d, c->dev.cu_ordinal), "cuDeviceGet") || !cuok(cu.CtxCreate(&ctx, CU_CTX_SCHED_BLOCKING_SYNC, d), "cuCtxCreate")) return false;
    c->a = ctx;
    return true;
}

void rtcu_close(rt_ctx *c)
{
    if (c->a) cu.CtxDestroy((CUcontext)c->a);
}

bool rtcu_build(rt_mod *m, rt_prog_id prog, int param)
{
    /* the parameter is HALF | minimum blocks per SM << 16 for the linear kernel, NK | configuration << 16 for the kangaroo (see gpu_priv.h) */
    const pz_blob *ptx = NULL;
    for (int i = 0; i < rtcu_module_count && !ptx; i++)
        if (rtcu_modules[i].prog == prog && (prog == RT_PROG_TEST || rtcu_modules[i].param == param)) ptx = rtcu_modules[i].blob;
    if (!ptx) { rt_log(PZ_LOG_ERR, "CUDA: no precompiled module for parameter %d", param); return false; }
    if (!use_ctx(m->c)) return false;
    size_t len;
    char *text = pz_blob_text(ptx, &len);      /* a few ms, once per module: the driver keeps its own copy */
    if (!text) { rt_log(PZ_LOG_ERR, "CUDA: cannot unpack the embedded module for parameter %d", param); return false; }
    char log[4096] = "";
    int opts[2] = {CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
    void *vals[2] = {log, (void *)(uintptr_t)sizeof log};
    CUmodule mod = NULL;
    CUresult rc = cu.ModuleLoadDataEx(&mod, text, 2, opts, vals);
    free(text);
    if (rc != 0) {
        cuok(rc, "PTX module load");
        if (log[0]) rt_log(PZ_LOG_ERR, "CUDA: driver JIT compiler:\n%s", log);
        if (rc == 222) rt_log(PZ_LOG_ERR, "CUDA: the driver is too old for this PTX: update the NVIDIA driver (version 525 or later is required)");
        return false;
    }
    m->h = mod;
    return true;
}

void rtcu_mod_free(rt_mod *m)
{
    if (m->h && use_ctx(m->c)) cu.ModuleUnload((CUmodule)m->h);
}

bool rtcu_kernel(rt_kern *k, const char *name)
{
    if (!use_ctx(k->m->c)) return false;
    CUfunction f = NULL;
    if (!cuok(cu.ModuleGetFunction(&f, (CUmodule)k->m->h, name), "kernel lookup")) return false;
    int maxt = RT_CUDA_BLOCK;
    if (cu.FuncGetAttribute(&maxt, CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, f) != 0 || maxt <= 0) maxt = 32;
    k->block = maxt < RT_CUDA_BLOCK ? maxt : RT_CUDA_BLOCK;
    k->h = f;
    return true;
}

void rtcu_kern_free(rt_kern *k) { (void)k; }   /* functions belong to their module */

bool rtcu_buf_new(rt_buf *b, const void *init)
{
    if (!use_ctx(b->c)) return false;
    CUdeviceptr p = 0;
    if (!cuok(cu.MemAlloc(&p, b->bytes), "GPU memory allocation")) return false;
    b->dptr = p;
    if (init) return cuok(cu.MemcpyHtoD(p, init, b->bytes), "copy to the GPU");
    return cuok(cu.MemsetD8(p, 0, b->bytes), "GPU memory clearing");
}

bool rtcu_buf_part(rt_buf *b, rt_buf *of, size_t off)
{
    b->dptr = of->dptr + off;
    return true;
}

void rtcu_buf_free(rt_buf *b)
{
    if (b->dptr && !b->part && use_ctx(b->c)) cu.MemFree((CUdeviceptr)b->dptr);
}

bool rtcu_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n)
{
    return use_ctx(c) && cuok(cu.MemcpyHtoD((CUdeviceptr)(b->dptr + off), src, n), "copy to the GPU");
}

bool rtcu_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n)
{
    return use_ctx(c) && cuok(cu.MemcpyDtoH(dst, (CUdeviceptr)(b->dptr + off), n), "copy from the GPU");
}

bool rtcu_run(rt_ctx *c, rt_kern *k, size_t n)
{
    if (!use_ctx(c)) return false;
    CUdeviceptr ptrs[RT_MAX_ARGS];
    uint32_t vals[RT_MAX_ARGS + 1];
    void *params[RT_MAX_ARGS + 1];
    for (int i = 0; i < k->nargs; i++) {
        if (k->is_buf[i]) { ptrs[i] = (CUdeviceptr)k->buf[i]->dptr; params[i] = &ptrs[i]; }
        else { vals[i] = k->u32[i]; params[i] = &vals[i]; }
    }
    vals[k->nargs] = (uint32_t)n;
    params[k->nargs] = &vals[k->nargs];
    unsigned block = (unsigned)k->block;
    unsigned grid = (unsigned)((n + block - 1) / block);
    return cuok(cu.LaunchKernel((CUfunction)k->h, grid, 1, 1, block, 1, 1, 0, NULL, params, NULL), "kernel launch") &&
           cuok(cu.CtxSynchronize(), "kernel execution");
}
