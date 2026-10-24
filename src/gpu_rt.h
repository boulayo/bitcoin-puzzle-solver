/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_rt.h - the small GPU runtime interface the GPU code (gpu.c) is written against, with two implementations:
 * OpenCL (rt_cl.c, kernels compiled from source by the driver) and CUDA (rt_cuda.c, driver API + embedded PTX).
 * Both are loaded at run time (dlopen / LoadLibrary): the program starts and works on machines with neither. */
#ifndef PZ_GPU_RT_H
#define PZ_GPU_RT_H

#include "common.h"

typedef enum { RT_CUDA = 0, RT_OPENCL = 1 } rt_api;
typedef enum { RT_PROG_LINEAR, RT_PROG_KANG, RT_PROG_TEST } rt_prog_id;

typedef struct {
    rt_api api;
    int index;                     /* position in the combined list: what --gpu N refers to */
    char name[128], vendor[64], plat_name[64], version[64];
    unsigned cu, mhz;
    u64 gmem;
    bool is_gpu;
    u64 l2_bytes, smem_bytes;      /* last-level cache of the device and fast local memory per SM / compute unit (0 when unknown) */
    double peak_gbs;               /* theoretical memory bandwidth in GB/s when the runtime tells (CUDA), else 0 */
    bool unified;                  /* the device uses the system RAM (integrated GPU): gmem is the share of it the driver may map, not own memory */
    int dup_of;                    /* OpenCL entry of a GPU that CUDA also drives: index of the CUDA entry, else -1 */
    /* backend handles */
    void *cl_dev;                  /* OpenCL */
    int cu_ordinal, cc;            /* CUDA: device ordinal, compute capability (major*10+minor) */
    int pci_bus, pci_dev;          /* -1 when unknown */
} rt_dev;

typedef struct rt_ctx rt_ctx;
typedef struct rt_mod rt_mod;
typedef struct rt_kern rt_kern;
typedef struct rt_buf rt_buf;

#define RT_MAX_DEVS 32
#define RT_MAX_ARGS 24

/* Lists the devices of both runtimes (CUDA first). `note` receives a one-line explanation when a runtime is missing. */
int rt_enumerate(rt_dev *out, int max, char *note, size_t note_len);

rt_ctx *rt_open(const rt_dev *d);
void rt_close(rt_ctx *c);
/* Program `prog`; `param` is HALF for the linear kernel, NK for the kangaroo, 0 for the tests. NULL on failure (logged). */
rt_mod *rt_build(rt_ctx *c, rt_prog_id prog, int param);
void rt_mod_free(rt_mod *m);
/* `nargs` = arguments the caller sets; the work-item count is appended as the last argument by rt_launch. */
rt_kern *rt_kernel(rt_mod *m, const char *name, int nargs);
void rt_kern_free(rt_kern *k);

rt_buf *rt_buf_new(rt_ctx *c, size_t bytes, const void *init);
void rt_buf_free(rt_buf *b);
bool rt_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n);
bool rt_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n);

void rt_arg_buf(rt_kern *k, int i, rt_buf *b);
void rt_arg_u32(rt_kern *k, int i, uint32_t v);
/* Runs `n` work-items and waits for completion. */
bool rt_run(rt_ctx *c, rt_kern *k, size_t n);

const char *rt_api_name(rt_api a);

#endif
