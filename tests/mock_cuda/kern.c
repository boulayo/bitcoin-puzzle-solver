/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kern.c - one kernel module of the mock CUDA driver: the OpenCL C sources compiled as C.
 * Built several times with -DMOCK_LINEAR/-DMOCK_KANG/-DMOCK_TEST, -DHALF=... and renaming macros (see Makefile). */
#include "pz_host.h"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../src/kernels/pz_common.cl"
#if defined(MOCK_LINEAR)
#include "../../src/kernels/pz_linear.cl"
#elif defined(MOCK_KANG)
#include "../../src/kernels/pz_kang.cl"
#else
#include "../../src/kernels/pz_test.cl"
#endif
