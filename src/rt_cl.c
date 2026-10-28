/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* rt_cl.c - OpenCL implementation of the GPU runtime interface (kernels compiled from the embedded sources). */
#include "rt_priv.h"
#include "cl_api.h"

extern const unsigned char pz_common_cl[], pz_linear_cl[], pz_kang_cl[], pz_test_cl[];
extern const unsigned int pz_common_cl_len, pz_linear_cl_len, pz_kang_cl_len, pz_test_cl_len;

#define CL_DEVICE_PCI_BUS_ID_NV 0x4008
#define CL_DEVICE_PCI_SLOT_ID_NV 0x4009
#ifndef CL_DEVICE_GLOBAL_MEM_CACHE_SIZE
#define CL_DEVICE_GLOBAL_MEM_CACHE_SIZE 0x101E
#endif
#ifndef CL_DEVICE_LOCAL_MEM_SIZE
#define CL_DEVICE_LOCAL_MEM_SIZE 0x1023
#endif
#ifndef CL_DEVICE_HOST_UNIFIED_MEMORY
#define CL_DEVICE_HOST_UNIFIED_MEMORY 0x1035
#endif

static bool clok(cl_int rc, const char *what)
{
    if (rc == CL_SUCCESS) return true;
    rt_log(PZ_LOG_ERR, "OpenCL: %s failed (code %d)", what, rc);
    return false;
}

int rtcl_enumerate(rt_dev *out, int max, char *note, size_t nl)
{
    const cl_api *cl = cl_load();
    if (!cl) {
        snprintf(note, nl, "OpenCL runtime not found");
        return 0;
    }
    cl_platform_id plats[16];
    cl_uint np = 0;
    if (cl->GetPlatformIDs(16, plats, &np) != CL_SUCCESS || np == 0) {
        snprintf(note, nl, "no OpenCL platform");
        return 0;
    }
    int n = 0;
    for (cl_uint p = 0; p < np && p < 16; p++) {
        cl_device_id devs[32];
        cl_uint nd = 0;
        if (cl->GetDeviceIDs(plats[p], CL_DEVICE_TYPE_ALL, 32, devs, &nd) != CL_SUCCESS) continue;
        char pname[64] = "";
        cl->GetPlatformInfo(plats[p], CL_PLATFORM_NAME, sizeof pname, pname, NULL);
        for (cl_uint d = 0; d < nd && d < 32 && n < max; d++) {
            rt_dev *g = &out[n];
            memset(g, 0, sizeof *g);
            g->api = RT_OPENCL;
            g->cl_dev = devs[d];
            g->cu_ordinal = -1;
            pz_strlcpy(g->plat_name, pname, sizeof g->plat_name);
            cl_uint cu = 0, mhz = 0;
            cl_ulong gmem = 0;
            cl_device_type type = 0;
            cl->GetDeviceInfo(devs[d], CL_DEVICE_NAME, sizeof g->name, g->name, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_VENDOR, sizeof g->vendor, g->vendor, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_VERSION, sizeof g->version, g->version, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof cu, &cu, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof mhz, &mhz, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof gmem, &gmem, NULL);
            cl->GetDeviceInfo(devs[d], CL_DEVICE_TYPE, sizeof type, &type, NULL);
            cl_uint uni = 0;
            if (cl->GetDeviceInfo(devs[d], CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof uni, &uni, NULL) == CL_SUCCESS) g->unified = uni != 0;
            cl_ulong l2 = 0, lm = 0;
            if (cl->GetDeviceInfo(devs[d], CL_DEVICE_GLOBAL_MEM_CACHE_SIZE, sizeof l2, &l2, NULL) == CL_SUCCESS) g->l2_bytes = l2;
            if (cl->GetDeviceInfo(devs[d], CL_DEVICE_LOCAL_MEM_SIZE, sizeof lm, &lm, NULL) == CL_SUCCESS) g->smem_bytes = lm;
            g->cu = cu;
            g->mhz = mhz;
            g->gmem = gmem;
            g->is_gpu = (type & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR)) != 0;
            cl_uint bus = 0, slot = 0;
            g->pci_bus = g->pci_dev = -1;
            if (strstr(g->vendor, "NVIDIA") && cl->GetDeviceInfo(devs[d], CL_DEVICE_PCI_BUS_ID_NV, sizeof bus, &bus, NULL) == CL_SUCCESS &&
                cl->GetDeviceInfo(devs[d], CL_DEVICE_PCI_SLOT_ID_NV, sizeof slot, &slot, NULL) == CL_SUCCESS) {
                g->pci_bus = (int)bus;
                g->pci_dev = (int)(slot >> 3);   /* slot id = device << 3 | function */
            }
            n++;
        }
    }
    if (!n) snprintf(note, nl, "no OpenCL device");
    return n;
}

bool rtcl_open(rt_ctx *c)
{
    const cl_api *cl = cl_load();
    if (!cl) return false;
    cl_int rc;
    cl_device_id dev = c->dev.cl_dev;
    cl_context ctx = cl->CreateContext(NULL, 1, &dev, NULL, NULL, &rc);
    if (!ctx || rc != CL_SUCCESS) { rt_log(PZ_LOG_ERR, "OpenCL: could not create a context on %s (%d)", c->dev.name, rc); return false; }
    cl_command_queue q = cl->CreateCommandQueue(ctx, dev, 0, &rc);
    if (!q || rc != CL_SUCCESS) { rt_log(PZ_LOG_ERR, "OpenCL: could not create the command queue (%d)", rc); cl->ReleaseContext(ctx); return false; }
    c->a = ctx;
    c->b = q;
    return true;
}

void rtcl_close(rt_ctx *c)
{
    const cl_api *cl = cl_load();
    if (c->b) cl->ReleaseCommandQueue((cl_command_queue)c->b);
    if (c->a) cl->ReleaseContext((cl_context)c->a);
}

bool rtcl_build(rt_mod *m, rt_prog_id prog, int param)
{
    const cl_api *cl = cl_load();
    const char *src[2] = {(const char *)pz_common_cl, NULL};
    size_t len[2] = {pz_common_cl_len, 0};
    char opts[96];
    switch (prog) {
    case RT_PROG_LINEAR: src[1] = (const char *)pz_linear_cl; len[1] = pz_linear_cl_len; snprintf(opts, sizeof opts, "-cl-std=CL1.2 -DHALF=%d", param & 0xFFFF); break;
    case RT_PROG_KANG: src[1] = (const char *)pz_kang_cl; len[1] = pz_kang_cl_len; snprintf(opts, sizeof opts, "-cl-std=CL1.2 -DNK=%d%s", param & 0xFFFF, (param >> 16) == 1 ? " -DMC=2" : (param >> 16) == 2 ? " -DMC=4" : ""); break;
    default: src[1] = (const char *)pz_test_cl; len[1] = pz_test_cl_len; snprintf(opts, sizeof opts, "-cl-std=CL1.2"); break;
    }
    cl_int rc;
    cl_context ctx = (cl_context)m->c->a;
    cl_device_id dev = m->c->dev.cl_dev;
    cl_program p = cl->CreateProgramWithSource(ctx, 2, src, len, &rc);
    if (!p || rc != CL_SUCCESS) { rt_log(PZ_LOG_ERR, "OpenCL: program creation failed (%d)", rc); return false; }
    rc = cl->BuildProgram(p, 1, &dev, opts, NULL, NULL);
    if (rc != CL_SUCCESS) {
        size_t sz = 0;
        cl->GetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &sz);
        char *log = pz_xcalloc(sz + 1, 1);
        cl->GetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, sz, log, NULL);
        rt_log(PZ_LOG_ERR, "OpenCL: kernel build failed on %s (%d):\n%s", m->c->dev.name, rc, log);
        free(log);
        cl->ReleaseProgram(p);
        return false;
    }
    m->h = p;
    return true;
}

void rtcl_mod_free(rt_mod *m) { if (m->h) cl_load()->ReleaseProgram((cl_program)m->h); }

bool rtcl_kernel(rt_kern *k, const char *name)
{
    cl_int rc;
    cl_kernel h = cl_load()->CreateKernel((cl_program)k->m->h, name, &rc);
    if (!h || rc != CL_SUCCESS) { rt_log(PZ_LOG_ERR, "OpenCL: kernel %s not found (%d)", name, rc); return false; }
    k->h = h;
    return true;
}

void rtcl_kern_free(rt_kern *k) { if (k->h) cl_load()->ReleaseKernel((cl_kernel)k->h); }

bool rtcl_buf_new(rt_buf *b, const void *init)
{
    cl_int rc;
    cl_mem_flags fl = CL_MEM_READ_WRITE | (init ? CL_MEM_COPY_HOST_PTR : 0);
    cl_mem m = cl_load()->CreateBuffer((cl_context)b->c->a, fl, b->bytes, (void *)init, &rc);
    if (!m || rc != CL_SUCCESS) { rt_log(PZ_LOG_ERR, "OpenCL: allocation of a %zu-byte buffer failed (%d)", b->bytes, rc); return false; }
    b->h = m;
    return true;
}

void rtcl_buf_free(rt_buf *b) { if (b->h) cl_load()->ReleaseMemObject((cl_mem)b->h); }

bool rtcl_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n)
{
    return clok(cl_load()->EnqueueWriteBuffer((cl_command_queue)c->b, (cl_mem)b->h, CL_TRUE, off, n, src, 0, NULL, NULL), "buffer write");
}

bool rtcl_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n)
{
    return clok(cl_load()->EnqueueReadBuffer((cl_command_queue)c->b, (cl_mem)b->h, CL_TRUE, off, n, dst, 0, NULL, NULL), "buffer read");
}

bool rtcl_run(rt_ctx *c, rt_kern *k, size_t n)
{
    const cl_api *cl = cl_load();
    cl_kernel h = (cl_kernel)k->h;
    for (int i = 0; i < k->nargs; i++) {
        cl_int rc;
        if (k->is_buf[i]) { cl_mem m = (cl_mem)k->buf[i]->h; rc = cl->SetKernelArg(h, (cl_uint)i, sizeof m, &m); }
        else rc = cl->SetKernelArg(h, (cl_uint)i, sizeof(uint32_t), &k->u32[i]);
        if (!clok(rc, "setting kernel arguments")) return false;
    }
    uint32_t nn = (uint32_t)n;
    if (!clok(cl->SetKernelArg(h, (cl_uint)k->nargs, sizeof nn, &nn), "setting kernel arguments")) return false;
    size_t g = n;
    return clok(cl->EnqueueNDRangeKernel((cl_command_queue)c->b, h, 1, NULL, &g, NULL, 0, NULL, NULL), "kernel execution") &&
           clok(cl->Finish((cl_command_queue)c->b), "clFinish");
}
