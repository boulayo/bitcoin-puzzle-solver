/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* engine_common.inc - what the two templates of the linear-search engines (engine_impl.inc, engine_vec_impl.inc) share: the hash of their
 * instruction set, and the keys of a batch: lane l of X[8][stride] (the words of x, big-endian) and of pfx[stride] (the prefix byte). */

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define PZ_HASH_LANES_FN CAT(hash_lanes_, ISA)
#include <pzcore/impl/hash_lanes.inc>
_Static_assert(PZ_HASH_FILTER_WORD == SCAN_FILTER_WORD, "the word of a near miss");

/* the compressed public key of the point (x, y) into a lane of a batch: X its first word (word i at X[i * stride]), pfx its prefix */
static inline void lane_set(u32 *X, u32 *pfx, int stride, const pz_fe *x, const pz_fe *y)
{
    pz_fe xn = *x, yn = *y;
    pz_fe_normalize(&xn);
    pz_fe_normalize(&yn);
    for (int i = 0; i < 4; i++) {
        X[2 * i * stride] = (u32)(xn.n[3 - i] >> 32);
        X[(2 * i + 1) * stride] = (u32)xn.n[3 - i];
    }
    *pfx = 2u + (u32)(yn.n[0] & 1);
}

/* A candidate of the filter, the key at offset koff of the claim of `job` that starts at base, in a lane (X, stride and pfx as above): its
 * hash160 in full against the target, then the near miss and the hit as scan_job says. */
static void lane_verify(const scan_job *job, const pz_u256 *base, const u32 *X, int stride, u32 pfx, u64 koff)
{
    u8 pub[33], h[20];
    pub[0] = (u8)pfx;
    for (int i = 0; i < 8; i++) {
        const u32 w = X[i * stride];
        pub[1 + 4 * i] = (u8)(w >> 24); pub[2 + 4 * i] = (u8)(w >> 16);
        pub[3 + 4 * i] = (u8)(w >> 8);  pub[4 + 4 * i] = (u8)w;
    }
    pz_hash160(pub, 33, h);
    const bool hit = memcmp(h, job->target_h160, 20) == 0;
    if (!hit && !job->on_near) return;
    pz_u256 key, d = pz_u256_from_u64(koff);
    pz_u256_add(&key, base, &d);
    if (job->on_near) job->on_near(job->user, &key);
    if (hit && job->on_hit) job->on_hit(job->user, &key);
}
