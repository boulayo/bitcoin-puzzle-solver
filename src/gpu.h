/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu.h - GPU support (gpu.c, gpu_tune.c, gpu_selftest.c), on CUDA or OpenCL devices through the runtime layer of gpu_rt.h. Both
 * runtimes are loaded at run time: without them the program works on the CPU alone. */
#ifndef PZ_GPU_H
#define PZ_GPU_H

#include "run.h"

typedef struct gpu_session gpu_session;

/* Which runtime drives the GPUs that both can see: auto = CUDA for NVIDIA cards (OpenCL as a fallback), OpenCL for the others. */
typedef enum { GPU_API_AUTO = 0, GPU_API_CUDA, GPU_API_OPENCL } gpu_api_pref;
void gpu_set_api_pref(gpu_api_pref p);
/* Dies with a clear message when an index of a --gpu list does not exist. */
void gpu_check_filter(const char *filter);   /* default: environment variable PZ_GPU_API, else auto */

/* Lists detected devices (CUDA and OpenCL) to stdout. Returns the number found. */
int gpu_list_devices(void);
/* A line naming the GPUs a campaign would use by default (runtime, name, units, driver): part of the key of the saved profile. */
void gpu_signature(char *out, size_t n);
/* Number of GPUs that would be used by default (0 when there is no runtime). Silent. */
int gpu_count_usable(void);
/* Prepares one worker per selected device (self-tested). Returns count, session array in *out. */
int gpu_linear_start(run_ctx *r, gpu_session **out);
void gpu_linear_launch(run_ctx *r, gpu_session *s, int n, _Atomic int *active);
void gpu_linear_join(gpu_session *s, int n);

/* CPU cores to set aside for driving the sessions: one per OpenCL device (drivers may poll while waiting), none for
 * CUDA devices (their context waits without spinning). */
int gpu_cores_needed(const gpu_session *s, int n);

/* Self-test of every usable GPU (0 = ok or no GPU). */
int gpu_selftest(void);
void gpu_bench(void);
/* 'doctor': the GPUs a campaign would use (what they are, whether they compute correctly), and the saved kernel choices. */
void gpu_doctor(bool deep);
void gpu_doctor_cache(void);

/* Kangaroo: start() reports how many walkers the GPUs will run (k_total), launch() receives the kang_state. */
int gpu_kangaroo_start(run_ctx *r, gpu_session **out, int *k_total);
void gpu_kangaroo_launch(run_ctx *r, void *ks, gpu_session *s, int n, _Atomic int *active);
void gpu_kangaroo_join(gpu_session *s, int n);
/* Makes the kangaroo thread of one session idle (or resume): 'bench --together'. Index and name of the device for messages. */
void gpu_pause(gpu_session *s, bool paused);
/* Priority of the threads that serve the GPUs (they launch the kernels and collect the results): with every core busy computing
 * they can wait for their turn, and the GPU idles meanwhile. The change applies at the next launch of every running session. */
void gpu_set_boost(bool on);
bool gpu_boost_get(void);
int gpu_session_index(const gpu_session *s);
gpu_session *gpu_session_at(gpu_session *s, int i);   /* the i-th session of an array returned by the start functions */

/* 'bench --retune': ignore the saved kernel choice of the GPUs, measure again and save the new one. */
void gpu_set_retune(bool on);

#endif
