/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* rt_priv.h - internals shared by rt.c (dispatch) and the two runtime implementations. */
#ifndef PZ_RT_PRIV_H
#define PZ_RT_PRIV_H

#include "gpu_rt.h"

struct rt_ctx {
    rt_api api;
    rt_dev dev;
    void *a, *b;                   /* OpenCL: context, queue.  CUDA: CUcontext */
};
struct rt_mod {
    rt_ctx *c;
    void *h;                       /* cl_program / CUmodule */
};
struct rt_buf {
    rt_ctx *c;
    size_t bytes;
    void *h;                       /* cl_mem */
    u64 dptr;                      /* CUdeviceptr */
};
struct rt_kern {
    rt_mod *m;
    void *h;                       /* cl_kernel / CUfunction */
    int nargs;                     /* caller arguments; the work-item count goes at index nargs */
    bool is_buf[RT_MAX_ARGS];
    rt_buf *buf[RT_MAX_ARGS];
    uint32_t u32[RT_MAX_ARGS];
    int block;                     /* CUDA threads per block */
};

/* OpenCL */
int rtcl_enumerate(rt_dev *out, int max, char *note, size_t nl);
bool rtcl_open(rt_ctx *c);
void rtcl_close(rt_ctx *c);
bool rtcl_build(rt_mod *m, rt_prog_id prog, int param);
void rtcl_mod_free(rt_mod *m);
bool rtcl_kernel(rt_kern *k, const char *name);
void rtcl_kern_free(rt_kern *k);
bool rtcl_buf_new(rt_buf *b, const void *init);
void rtcl_buf_free(rt_buf *b);
bool rtcl_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n);
bool rtcl_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n);
bool rtcl_run(rt_ctx *c, rt_kern *k, size_t n);

/* CUDA */
int rtcu_enumerate(rt_dev *out, int max, char *note, size_t nl);
bool rtcu_open(rt_ctx *c);
void rtcu_close(rt_ctx *c);
bool rtcu_build(rt_mod *m, rt_prog_id prog, int param);
void rtcu_mod_free(rt_mod *m);
bool rtcu_kernel(rt_kern *k, const char *name);
void rtcu_kern_free(rt_kern *k);
bool rtcu_buf_new(rt_buf *b, const void *init);
void rtcu_buf_free(rt_buf *b);
bool rtcu_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n);
bool rtcu_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n);
bool rtcu_run(rt_ctx *c, rt_kern *k, size_t n);

#endif
