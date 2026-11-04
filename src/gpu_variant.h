/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* gpu_variant.h - the code of a GPU kernel variant: the number that PZ_GPU_HALF and PZ_GPU_NK take and that gpu_tune.txt saves, and everything
 * that is derived from its fields. The layout (the fields, and the macros that build a code from them), the name that the logs and the tables
 * print, the text form that the environment takes and the options of the OpenCL build are all here (gpu_variant.c), so that nothing else needs
 * to know which bit is which. Which variants a device offers is the business of gpu_tune.c. Libc only. */
#ifndef PZ_GPU_VARIANT_H
#define PZ_GPU_VARIANT_H

#include <stdbool.h>
#include <stddef.h>

/* Compile-time variants of the linear kernel: half-window (group = 2*half keys) | minimum resident blocks per SM << 16
 * (CUDA only: 4 caps the kernel at 128 registers, trading a few spills for more resident warps). The autotuner measures
 * them on the device and keeps the fastest. Bits 19 and up are free for a form of the kernel (the hash variants and the
 * interleaved loop of RESEARCH.md section 5, after 1.19.2, used them while a card timed them). */
#define VAR_HALF(v) ((v) & 0xFFFF)
#define VAR_MINB(v) (((v) >> 16) & 7)

/* Kangaroo kernel variants: kangaroos per work-item (NK: one field inversion is shared by NK steps, so larger is cheaper
 * in arithmetic but needs more private memory) | configuration << 16: 0 keeps the running product of the batch inversion
 * for every walker, 1 only for every other one (half the traffic for those products, half a field multiplication more per
 * step), 2 every fourth, 3 the block herd (KV_BLOCK: a few walkers per work-item in registers, one inversion per work-group of
 * KANG_GROUP and round) | metadata << 20: 1 writes the tag two steps back once per launch instead of at every step and reads the
 * flags of a quad once (KANG_META in pz_kang.cl: 7 bytes less per step on the SASS, slower on the RTX A2000 as measured, RESEARCH.md
 * section 5 after 1.19.2; the suffix m of PZ_GPU_NK), 0 the form of 1.19.2. Measured on the device like the linear variants. */
#define KV_NK(v) ((v) & 0xFFFF)
#define KV_CFG(v) (((v) >> 16) & 0xF)
#define KV_META(v) (((v) >> 20) & 1)
#define KV_METAFLAG (1 << 20)
#define KV_L2(v) (((v) >> 24) & 1)            /* the herd is sized to fit the L2 cache (fewer work-items), see kang_l2_threads */
#define KV_KERNEL(v) ((v) & 0x1FFFFF)         /* NK | cfg << 16 | metadata << 20: what the kernel build takes (modules.txt: cfg + 16 metadata) */
#define KV_L2FLAG (1 << 24)
#define KV_BLOCK 3                            /* KV_CFG of the block herd */
#define KV_WIDE(v) (((v) >> 25) & 3)          /* a herd wider than the default one (more work-items), see kang_wide_threads_for */
#define KV_WIDEFLAG(w) ((w) << 25)
enum { KV_WIDE_X4 = 1, KV_WIDE_X8 = 2, KV_WIDE_FULL = 3 };   /* OpenCL: 4 or 8 times the work-items; CUDA: 512 per SM past the cap */

/* Building a code from its fields (constant expressions, so that a table of variants can be written with them): the window and the minimum
 * of resident blocks per SM of a linear kernel; the walkers per work-item, the configuration (KV_CFG), the form of the metadata (0 or 1), the
 * L2 herd (0 or 1) and the width of the herd (KV_WIDE_*, 0 for the default one) of a kangaroo kernel. The fields are read back with the
 * macros above. */
#define VAR_MAKE(half, minb) ((half) | (minb) << 16)
#define KV_MAKE(nk, cfg, meta, l2, wide) ((nk) | (cfg) << 16 | (meta) << 20 | (l2) << 24 | KV_WIDEFLAG(wide))

/* The name of a variant as the logs, the tables of 'bench --variants' and 'doctor' print it ("256, register cap", "NK 128 quads, tags once"),
 * and its text code as PZ_GPU_HALF or PZ_GPU_NK takes it ("262400", "128cm"; "-" for a kangaroo variant that no text can force: the wide herds).
 * Each writes into b (n bytes) and returns it. */
char *gpu_variant_name(int v, char *b, size_t n);
char *gpu_kvariant_name(int v, char *b, size_t n);
char *gpu_variant_code(int v, char *b, size_t n);
char *gpu_kvariant_code(int v, char *b, size_t n);

/* The text of PZ_GPU_NK, "128", "64b", "4g", "128cm", "64l": the walkers per work-item, then the suffix of a configuration (b pairs, c quads,
 * g block herd), then m (the tags written once) and l (the herd sized to the L2 cache). On success the code, without a width (the wide herds
 * have no text); false, leaving *code alone, for anything else. */
bool gpu_kvariant_parse(const char *text, int *code);

/* The option of the OpenCL build for a configuration (KV_CFG) of the kangaroo kernel, with its blank in front (" -DMC=2"), or "" for the
 * default configuration and for one it does not know; and the option for the form of the metadata. */
const char *gpu_kvariant_cl_config(int cfg);
const char *gpu_kvariant_cl_meta(int meta);

#endif
