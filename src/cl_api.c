/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cl_api.c - dlopen-based OpenCL loader (the OpenCL runtime is optional on every platform: without it there is simply no GPU). */
#include "cl_api.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

static cl_api g_api;
static int g_state;   /* 0 = untried, 1 = ok, -1 = failed */

#define LOAD(field, sym) do { *(void **)&g_api.field = pz_lib_sym(g_api.lib, "cl" sym); if (!g_api.field) goto fail; } while (0)

const cl_api *cl_load(void)
{
    if (g_state) return g_state > 0 ? &g_api : NULL;
#if defined(_WIN32)
    const char *names[] = {"OpenCL.dll", NULL};   /* installed by every GPU driver (NVIDIA, AMD, Intel) */
#elif defined(__APPLE__)
    /* The system framework (deprecated by Apple but still shipped; it lives in the dyld shared cache, so dlopen finds it even
     * though the file is not on disk), then an ICD loader installed by the user (Homebrew). No NVIDIA/CUDA on macOS. */
    const char *names[] = {"/System/Library/Frameworks/OpenCL.framework/OpenCL", "libOpenCL.dylib", NULL};
#else
    const char *names[] = {"libOpenCL.so.1", "libOpenCL.so", NULL};
#endif
    g_api.lib = pz_lib_open(names);
    if (!g_api.lib) goto fail;
    LOAD(GetPlatformIDs, "GetPlatformIDs");
    LOAD(GetPlatformInfo, "GetPlatformInfo");
    LOAD(GetDeviceIDs, "GetDeviceIDs");
    LOAD(GetDeviceInfo, "GetDeviceInfo");
    LOAD(CreateContext, "CreateContext");
    LOAD(ReleaseContext, "ReleaseContext");
    LOAD(CreateCommandQueue, "CreateCommandQueue");
    LOAD(ReleaseCommandQueue, "ReleaseCommandQueue");
    LOAD(CreateBuffer, "CreateBuffer");
    LOAD(ReleaseMemObject, "ReleaseMemObject");
    LOAD(CreateProgramWithSource, "CreateProgramWithSource");
    LOAD(BuildProgram, "BuildProgram");
    LOAD(GetProgramBuildInfo, "GetProgramBuildInfo");
    LOAD(ReleaseProgram, "ReleaseProgram");
    LOAD(CreateKernel, "CreateKernel");
    LOAD(ReleaseKernel, "ReleaseKernel");
    LOAD(SetKernelArg, "SetKernelArg");
    LOAD(EnqueueNDRangeKernel, "EnqueueNDRangeKernel");
    LOAD(EnqueueReadBuffer, "EnqueueReadBuffer");
    LOAD(EnqueueWriteBuffer, "EnqueueWriteBuffer");
    LOAD(Finish, "Finish");
    g_state = 1;
    return &g_api;
fail:
    g_state = -1;
    return NULL;
}
