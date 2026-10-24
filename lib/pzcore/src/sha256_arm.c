/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* sha256_arm.c - SHA-256 of whole blocks with the ARMv8 SHA-256 instructions (built with -march=armv8-a+crypto; used by hash.c only
 * when the CPU reports them). */
#include "h160_impl.h"

#ifdef PZ_HAVE_ARM_SHA2
#include <arm_neon.h>
#include <pzcore/impl/hash_tables.h>

void pz_sha256_blocks_arm(uint32_t state[8], const uint8_t *data, size_t nblocks)
{
    uint32x4_t s0 = vld1q_u32(state), s1 = vld1q_u32(state + 4);
    while (nblocks--) {
        uint32x4_t m[4];
        for (int i = 0; i < 4; i++) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
        const uint32x4_t save0 = s0, save1 = s1;
#pragma GCC unroll 16
        for (int i = 0; i < 16; i++) {
            uint32x4_t w = m[i & 3];
            if (i >= 4) {
                w = vsha256su1q_u32(vsha256su0q_u32(w, m[(i + 1) & 3]), m[(i + 2) & 3], m[(i + 3) & 3]);
                m[i & 3] = w;
            }
            const uint32x4_t wk = vaddq_u32(w, vld1q_u32(pz_sha256_k + 4 * i));
            const uint32x4_t prev = s0;
            s0 = vsha256hq_u32(s0, s1, wk);
            s1 = vsha256h2q_u32(s1, prev, wk);
        }
        s0 = vaddq_u32(s0, save0);
        s1 = vaddq_u32(s1, save1);
        data += 64;
    }
    vst1q_u32(state, s0);
    vst1q_u32(state + 4, s1);
}
#endif
