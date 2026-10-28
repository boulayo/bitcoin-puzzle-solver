/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_kang.h - the numbers that the kangaroo kernel (pz_kang.cl) and the host code must agree on, in one place. Only preprocessor lines, so that
 * the same text is read by the C compiler of the program (kangaroo_priv.h, gpu_priv.h, gpu.c), by the OpenCL driver and NVRTC (the file is
 * put in front of pz_kang.cl: rt_cl.c, tools/gen_ptx.py) and by the simulated CUDA driver (tests/mock_cuda/kern.c). */
#ifndef PZ_KANG_H
#define PZ_KANG_H

#define KANG_NESC 16             /* escape jumps, after the R regular ones: used only to leave a fruitless cycle */
#define KANG_CK_PERIOD 64        /* steps between the cycle checkpoints (detects cycles up to ~31 steps) */
#define KANG_NOTAG 0xFFFFFFFFu   /* "no tag yet" */
#define KANG_STATE_WORDS 32      /* 32-bit words of the state of a kangaroo on the device */
#define KANG_JUMP_WORDS 20       /* 32-bit words of an entry of the jump table: x[8] y[8] v[4] */

#endif
