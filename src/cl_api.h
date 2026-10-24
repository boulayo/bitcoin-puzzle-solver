/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cl_api.h - minimal OpenCL 1.2 declarations, loaded at run time with dlopen so
 * that the program builds and runs on machines without any OpenCL SDK. */
#ifndef PZ_CL_API_H
#define PZ_CL_API_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_uint cl_bool;
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint cl_device_info;
typedef cl_uint cl_platform_info;
typedef cl_uint cl_program_build_info;
typedef cl_uint cl_kernel_work_group_info;
typedef intptr_t cl_context_properties;
typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_event *cl_event;

#define CL_SUCCESS 0
#define CL_TRUE 1
#define CL_DEVICE_TYPE_GPU (1u << 2)
#define CL_DEVICE_TYPE_ACCELERATOR (1u << 3)
#define CL_DEVICE_TYPE_ALL 0xFFFFFFFFu
#define CL_DEVICE_TYPE 0x1000
#define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
#define CL_DEVICE_MAX_CLOCK_FREQUENCY 0x100C
#define CL_DEVICE_GLOBAL_MEM_SIZE 0x101F
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DEVICE_VERSION 0x102F
#define CL_PLATFORM_NAME 0x0902
#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_MEM_READ_WRITE (1u << 0)
#define CL_MEM_COPY_HOST_PTR (1u << 5)

typedef struct {
    void *lib;
    cl_int (*GetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
    cl_int (*GetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void *, size_t *);
    cl_int (*GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
    cl_int (*GetDeviceInfo)(cl_device_id, cl_device_info, size_t, void *, size_t *);
    cl_context (*CreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *, void (*)(const char *, const void *, size_t, void *), void *, cl_int *);
    cl_int (*ReleaseContext)(cl_context);
    cl_command_queue (*CreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties, cl_int *);
    cl_int (*ReleaseCommandQueue)(cl_command_queue);
    cl_mem (*CreateBuffer)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
    cl_int (*ReleaseMemObject)(cl_mem);
    cl_program (*CreateProgramWithSource)(cl_context, cl_uint, const char **, const size_t *, cl_int *);
    cl_int (*BuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *, void (*)(cl_program, void *), void *);
    cl_int (*GetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info, size_t, void *, size_t *);
    cl_int (*ReleaseProgram)(cl_program);
    cl_kernel (*CreateKernel)(cl_program, const char *, cl_int *);
    cl_int (*ReleaseKernel)(cl_kernel);
    cl_int (*SetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
    cl_int (*EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *);
    cl_int (*EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *);
    cl_int (*EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *);
    cl_int (*Finish)(cl_command_queue);
} cl_api;

/* Loads the OpenCL ICD loader. Returns NULL if unavailable. */
const cl_api *cl_load(void);

#endif
