/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* envopts.h - the environment variables the program reads, taken once at start-up into a plain structure. No other file calls getenv:
 * the code reads g_env. The variables and their meaning are in docs/en/USAGE.md (section 7.10); where each one is used is listed in
 * docs/en/ARCHITECTURE.md. */
#ifndef PZ_ENVOPTS_H
#define PZ_ENVOPTS_H

#include "common.h"

enum { PZ_ENV_PATH = 1024 };                          /* size of the fields that hold a folder name (a longer value is ignored) */
enum { PZ_ENV_API_AUTO = 0, PZ_ENV_API_CUDA, PZ_ENV_API_OPENCL };
enum { PZ_ENV_TUNE_DEFAULT = 0, PZ_ENV_TUNE_OFF, PZ_ENV_TUNE_ALL };

/* A variable that is not set, or is set to the empty string, leaves its field at the value of a program started with a clean environment
 * (below, "unset"). The flags follow the two-state rule: on with any value except "" and "0". A value that cannot be used is reported
 * once, as a warning "NAME=TEXT ignored (...)", and the field keeps its unset value. */
struct pz_env {
    /* GPU runtimes and kernels */
    int gpu_api;                    /* PZ_GPU_API=cuda|opencl|auto: PZ_ENV_API_*, auto when unset */
    bool no_cuda;                   /* PZ_NO_CUDA: do not load the CUDA driver */
    bool no_opencl;                 /* PZ_NO_OPENCL: do not load the OpenCL library */
    bool opencl_all;                /* PZ_OPENCL_ALL: use the OpenCL CPU devices and the second view of a device too */
    int gpu_half;                   /* PZ_GPU_HALF=N: window of the linear kernel (the values a device accepts are checked per device), 0 unset */
    int gpu_nk;                     /* PZ_GPU_NK=N[b|c][l]: walkers per work-item of the kangaroo kernel, 0 unset */
    int gpu_nk_cfg;                 /*   the suffix b (1) or c (2): products at every other (fourth) walker, 0 without */
    bool gpu_nk_l2;                 /*   the suffix l: herd sized to the L2 cache */
    char gpu_nk_text[16];           /*   the text as given, for the messages */
    int gpu_tune;                   /* PZ_GPU_TUNE=0|1: PZ_ENV_TUNE_OFF, PZ_ENV_TUNE_ALL (also the devices that are not GPUs), else PZ_ENV_TUNE_DEFAULT */
    /* the rest of the program */
    bool no_profile;                /* PZ_NO_PROFILE: same as --no-tune */
    bool no_large_pages;            /* PZ_NO_LARGE_PAGES: Windows, no large pages for the tables */
    double bench_scale;             /* PZ_BENCH_SCALE=X (0.01 to 100): factor of the durations of the measurements, 1 unset */
    /* places and terminal (the standard variables) */
    char xdg_cache_home[PZ_ENV_PATH];   /* XDG_CACHE_HOME */
    char home[PZ_ENV_PATH];             /* HOME */
    char local_app_data[PZ_ENV_PATH];   /* LOCALAPPDATA */
    char tmpdir[PZ_ENV_PATH];           /* TMPDIR */
    bool term_dumb;                 /* TERM=dumb */
    bool no_color;                  /* NO_COLOR set to anything but "" */
    /* tests and diagnostics */
    bool kang_verbose;              /* PZ_KANG_VERBOSE: the speed of every kangaroo step */
    bool kang_mix;                  /* PZ_KANG_MIX: scalar step on the second thread of a core */
    char kang_round[16];            /* PZ_KANG_ROUND=id: imposes the CPU kangaroo step (the check against the steps of this CPU is made where it is used) */
    bool bsgs_scalar;               /* PZ_BSGS_SCALAR: BSGS with the scalar windows only */
    bool assume_ifma;               /* PZ_ASSUME_IFMA: IFMA is taken as present when AVX-512 is (tests on hosts whose CPUID hides it) */
    bool expect_shani;              /* PZ_EXPECT_SHANI: the tests fail if the shani paths cannot run */
    int test_kthread;               /* PZ_TEST_KTHREAD=N (16 to 2^20): walkers per CPU thread, 0 unset */
    int test_jump;                  /* PZ_TEST_JUMP=N (1 to 2^31-1): mean jump in units of sqrt(W), 0 unset */
    long long test_dp_cap;          /* PZ_TEST_DP_CAP=N: entries of the table of distinguished points to start with, -1 unset */
};

/* The one instance. It holds the values of a clean environment until pz_env_init is called (the programs call it first thing in main), and
 * is read-only afterwards, except for the tests, which set the fields they need directly and put them back. */
extern struct pz_env g_env;

/* Resets g_env and reads the environment. Calling it again reads the environment again. */
void pz_env_init(void);

/* The value of PZ_GPU_NK, "128", "128b" (products at every other walker), "128c" (every fourth), with a final "l" for the herd sized to the
 * L2 cache. False if the text is anything else. */
bool pz_env_parse_nk(const char *text, int *nk, int *cfg, bool *l2);

#endif
