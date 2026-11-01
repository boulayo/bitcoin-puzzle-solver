/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_kang.cl - symmetric kangaroo step kernel (version 2: jump history instead of a distance per step).
 *
 * Mirrors kang_isa.inc exactly (same arithmetic, same tie-breaking, same events), so the host can verify a device herd
 * against the CPU implementation bit for bit.
 *
 * State per kangaroo = 32 uints:
 *   [0..7] x  [8..15] y  [16..21] d (192-bit two's complement, as of the start of the launch)
 *   [22] kind (0 tame / 1 wild)  [23] sigma flag (1 => sigma = -1)
 *   [24] tag one step back  [25] tag two steps back  [26] cycle checkpoint tag  [27] least tag since
 *   [28] leaving a cycle (0/1)  [29,30] step number of the last distinguished point or restart (64-bit)  [31] unused
 * jump table: per jump 20 uints = x[8] y[8] v[4]; R regular jumps, then KANG_NESC escape jumps. In global memory, not __constant:
 * every walker reads its own entry, and the constant cache serializes the reads of a warp at different addresses (NVIDIA)
 * DP record = 10 uints: tagk_lo tagk_hi d[6] index pad
 *
 * Why it is built like this. The private arrays of a work-item (tens of KB) live in device memory, and the kernel is
 * limited by that traffic, not by arithmetic. So:
 *  - The distance d is not carried from step to step. Each step appends one 16-bit entry to a history buffer (jump
 *    index, and whether the point was negated); d is rebuilt by replaying the entries, at the end of the launch for all
 *    kangaroos and, for the rare distinguished points, when they occur (from d at the start of the launch).
 *  - One pass per round instead of two. The pass that moves the kangaroos also computes the denominators of the next
 *    round and their running products, so the walker's x is not read again. The running products alternate between
 *    prefix and suffix products from one round to the next (the pass runs downwards, then upwards, ...): Montgomery's
 *    trick works with either, and each pass reads the products the previous one left behind and overwrites them.
 *  - The state a step needs besides x and y is a few 32-bit words (tags, flags, jump index); the two cycle tags are written only when
 *    they change. The two tags one and two steps back are written at every step (KANG_META 0, the form of 1.19.2). With KANG_META 1 the
 *    tag two steps back, which only the next jump choice uses and the step that computes it already knows, is kept for the host alone: the
 *    last round of a launch stores it (a launch that stops early because a product was zero leaves the value the launch began with, for
 *    walkers the host restarts anyway), and the quads kernel keeps the flags of its block in registers. Counted on the SASS the second form
 *    moves 7 bytes less per step; measured on an RTX A2000 it was slower (RESEARCH.md section 5, after 1.19.2), so it is the variant the
 *    tuner measures (the m suffix of PZ_GPU_NK) and the first is the default.
 * The history of a launch is rounds * NK * work-items 16-bit entries, laid out so that the work-items of a warp write
 * adjacent addresses. Entry: bits 0-8 jump index, bit 9 negated, 0xFFFF no step (a round skipped for a zero product). */

#ifndef NK
#define NK 64
#endif
#ifndef MC
#define MC 1                       /* kangaroos per block of stored batch products (1: every one) */
#endif
#ifndef KANG_META
#define KANG_META 0                /* 1: the tag two steps back written once per launch, the flags of a quad read once (the top of the file) */
#endif
#define KS KANG_STATE_WORDS        /* KANG_* are in pz_kang.h, which the builds put in front of this file (the host reads it too) */
#define JS KANG_JUMP_WORDS
#define H_NOP 0xFFFFu
#define H_FLIP 0x200u
#define H_JMASK 0x1FFu
#define FL_KIND 1u
#define FL_SG 2u
#define FL_ESC 4u
#define FL_JSHIFT 8

/* the distance of jump j: one 16-byte load (an entry is 80 bytes and its distance starts 64 bytes into it, so it is 16-byte aligned) */
inline uint4 jump_dist(__global const uint *jump, const uint j) { return *(__global const uint4 *)(jump + j * JS + 16); }

inline void d_add_jump(uint d[6], const uint4 v)
{
    ulong acc = (ulong)d[0] + v.x;
    d[0] = (uint)acc;
    ulong c = acc >> 32;
    acc = (ulong)d[1] + v.y + c; d[1] = (uint)acc; c = acc >> 32;
    acc = (ulong)d[2] + v.z + c; d[2] = (uint)acc; c = acc >> 32;
    acc = (ulong)d[3] + v.w + c; d[3] = (uint)acc; c = acc >> 32;
    acc = (ulong)d[4] + c; d[4] = (uint)acc; c = acc >> 32;
    d[5] += (uint)c;
}

inline void d_neg(uint d[6])
{
    ulong c = 1;
    for (int i = 0; i < 6; i++) { ulong s = (ulong)(~d[i]) + c; d[i] = (uint)s; c = s >> 32; }
}

/* jump chosen by a kangaroo (kang_jump_index in kangaroo_priv.h): low bits of x, moved to the next jump when the walk
 * would close a 2-cycle, or an escape jump at the point of least tag of a detected cycle. Tag = low 32 bits of x.n[2]. */
inline uint kang_jidx(const fe *x, uint prev2, uint ck, uint mn, uint esc, uint R)
{
    const uint tag = x->v[4];
    if ((esc || tag == ck) && tag == mn) return R + ((x->v[0] >> 16) & (KANG_NESC - 1));
    uint j = x->v[0] & (R - 1u);
    if (tag == prev2) j = (j + 1u) & (R - 1u);
    return j;
}

/* kang_cycle_update: on a checkpoint step (bnd) take the checkpoint, keep the least tag since, leave from it once back on it */
inline void kang_cycle(int bnd, uint t, uint *ck, uint *mn, uint *esc)
{
    if (*esc || t == *ck) {
        if (t == *mn) { *esc = 0; *ck = *mn = KANG_NOTAG; }
        else if (*esc && bnd) { *esc = 0; *ck = *mn = t; }
        else *esc = 1;
    } else if (bnd) {
        *ck = *mn = t;
    } else if (t < *mn) {
        *mn = t;
    }
}

/* one entry of the history: d += the jump it names (whose distance v was loaded before), negated if the point was */
inline void kang_replay_one(uint d[6], const uint h, const uint4 v)
{
    if (h == H_NOP) return;
    d_add_jump(d, v);
    if (h & H_FLIP) d_neg(d);
}
inline uint hist_jump(const uint h) { return h == H_NOP ? 0u : (h & H_JMASK); }   /* a round without a step reads a valid entry */

/* d += the jumps of `nr` rounds of the history of one kangaroo (entries off0, off0 + stride, ...). The history is far larger than the
 * caches: its entries, then their jump distances, are loaded KANG_REPLAY_BATCH at a time before they are used, so that the loads of a
 * batch wait for the memory together instead of one after the other. */
#define KANG_REPLAY_BATCH 8
inline void kang_replay(__global const ushort *hist, const size_t off0, const size_t stride, const uint nr, __global const uint *jump, uint d[6])
{
    uint i = 0;
    for (; i + KANG_REPLAY_BATCH <= nr; i += KANG_REPLAY_BATCH) {
        uint h[KANG_REPLAY_BATCH];
#pragma unroll
        for (int k = 0; k < KANG_REPLAY_BATCH; k++) h[k] = hist[off0 + (size_t)(i + k) * stride];
#pragma unroll
        for (int k = 0; k < KANG_REPLAY_BATCH; k++) kang_replay_one(d, h[k], jump_dist(jump, hist_jump(h[k])));
    }
    for (; i < nr; i++) {
        const uint h = hist[off0 + (size_t)i * stride];
        kang_replay_one(d, h, jump_dist(jump, hist_jump(h)));
    }
}

/* The distance in the state `st` of a walker at the end of a launch: += the jumps of its `nr` rounds. On CUDA a call of its own, like
 * kang_emit_dp: the registers of the batches of the replay then take no part in the allocation of the kernel's main loop. */
PZ_NOINLINE void kang_replay_state(__global uint *st, __global const ushort *hist, const size_t off0, const size_t stride, const uint nr,
                                   __global const uint *jump)
{
    uint d[6];
    for (int i = 0; i < 6; i++) d[i] = st[16 + i];
    kang_replay(hist, off0, stride, nr, jump, d);
    for (int i = 0; i < 6; i++) st[16 + i] = d[i];
}

/* A distinguished point after round `rd` of the launch: d is rebuilt from its value at the start of the launch */
PZ_NOINLINE void kang_emit_dp(__global const uint *st, __global const ushort *hist, const size_t off0, const size_t stride, const uint nr,
                              __global const uint *jump, __global uint *dps, __global uint *dpcount, const uint dps_cap, const uint widx,
                              const uint kindcode, const uint t4, const uint t5)
{
    uint d[6];
    for (int i = 0; i < 6; i++) d[i] = st[16 + i];
    kang_replay(hist, off0, stride, nr, jump, d);
    const uint slot = atomic_inc(dpcount);
    if (slot < dps_cap) {
        __global uint *o = dps + (size_t)slot * 10;
        o[0] = (t4 & ~3u) | kindcode;
        o[1] = t5;
        for (int i = 0; i < 6; i++) o[2 + i] = d[i];
        o[8] = widx;
        o[9] = 0;
    }
}

/* The words of a walker besides x and y, in separate arrays so that the kernels capped at 128 registers read each word where they use it
 * (one word of 16 bytes read at the start of the step kept three more registers live through it and spilled some of them on sm_75): FL[k]
 * the flags (FL_*) and the jump index, and the tags in one of two forms (KANG_META, see the top of the file):
 *   0  PP[k] the tags one and two steps back, written at every step as one 8-byte word; CM[k] the checkpoint tag and the least tag since;
 *   1  PV[k] the tag one step back; P2[k] the tag two steps back, written in the last round only; CM[k] the two cycle tags as one uint2.
 * KANG_TAGS declares the arrays, KANG_TAGS_IN fills them from the state, KANG_TAGS_OUT writes them back and KANG_TAGS_STEP is the part of the
 * step that reads and updates them (needs k, f, tag, bnd and more declared; leaves ck, mn, esc and prev2 declared). */
#if KANG_META
#define KANG_TAGS uint PV[NK], P2[NK]; uint2 CM[NK]
#define KANG_TAGS_IN(k, prev, prev2, ck, mn) do { PV[k] = prev; P2[k] = prev2; CM[k].x = ck; CM[k].y = mn; } while (0)
#define KANG_TAGS_OUT(s, k) do { s[24] = PV[k]; s[25] = P2[k]; s[26] = CM[k].x; s[27] = CM[k].y; } while (0)
#define KANG_TAGS_STEP \
            const uint2 cm = CM[k]; \
            uint ck = cm.x, mn = cm.y, esc = (f >> 2) & 1u; \
            kang_cycle(bnd, tag, &ck, &mn, &esc); \
            if (ck != cm.x || mn != cm.y) { CM[k].x = ck; CM[k].y = mn; } \
            const uint prev2 = PV[k];                      /* the old prev becomes prev2 */ \
            if (!more) P2[k] = prev2;                      /* for the host (see the top of the file) */ \
            PV[k] = tag;
#else
#define KANG_TAGS uint PP[NK][2], CM[NK][2]
#define KANG_TAGS_IN(k, prev, prev2, ck, mn) do { PP[k][0] = prev; PP[k][1] = prev2; CM[k][0] = ck; CM[k][1] = mn; } while (0)
#define KANG_TAGS_OUT(s, k) do { s[24] = PP[k][0]; s[25] = PP[k][1]; s[26] = CM[k][0]; s[27] = CM[k][1]; } while (0)
#define KANG_TAGS_STEP \
            uint ck = CM[k][0], mn = CM[k][1], esc = (f >> 2) & 1u; \
            const uint ck0 = ck, mn0 = mn; \
            kang_cycle(bnd, tag, &ck, &mn, &esc); \
            if (ck != ck0 || mn != mn0) { CM[k][0] = ck; CM[k][1] = mn; } \
            const uint prev2 = PP[k][0];                   /* the old prev becomes prev2 */ \
            PP[k][0] = tag; \
            PP[k][1] = prev2;
#endif

/* the step of one kangaroo once its inverse ik is known; needs k, i, rd, n, bnd, more, f (= FL[k]), J, xk, yk, jx, jy, ik, t, lam, x3, y3, J2
 * declared */
#define KANG_STEP { \
            fe_sub_norm(&t, &yk, &jy);                     /* x, y and the jump points are normalized */ \
            fe_mul(&lam, &t, &ik); \
            fe_sqr(&x3, &lam); \
            fe_sub2(&x3, &x3, &xk, &jx); \
            fe_normalize(&x3); \
            fe_sub_norm(&t, &xk, &x3); \
            fe_mul(&y3, &lam, &t); \
            fe_sub_norm(&y3, &y3, &yk); \
            fe_normalize(&y3); \
            const uint flip = y3.v[0] & 1u; \
            fe_cneg_norm(&y3, flip);                       /* canonical representative: even y */ \
            hist[(size_t)rd * HS + (size_t)k * pz_n + gid] = (ushort)(J | (flip ? H_FLIP : 0u)); \
            /* cycle detection with the tag of the point just left */ \
            const uint tag = xk.v[4]; \
            KANG_TAGS_STEP \
            const uint sg = ((f >> 1) & 1u) ^ flip; \
            J2 = kang_jidx(&x3, prev2, ck, mn, esc, R); \
            FL[k] = (f & FL_KIND) | (sg << 1) | (esc << 2) | (J2 << FL_JSHIFT); \
            X[k] = x3; \
            Y[k] = y3; \
            if ((x3.v[2] & dp_mask_lo) == 0 && (x3.v[3] & dp_mask_hi) == 0) { \
                const uint kindcode = (f & FL_KIND) == 0 ? 0u : (sg == 0 ? 1u : 2u); \
                kang_emit_dp(state + (size_t)(base + k) * KS, hist, (size_t)k * pz_n + gid, HS, rd + 1, jump, dps, dpcount, dps_cap, base + k, kindcode, \
                             x3.v[4], x3.v[5]); \
                LD[k] = n + 1; \
            } else if (bnd && n + 1 - LD[k] > cap) { \
                const uint slot = atomic_inc(rcount); \
                if (slot < rlist_cap) rlist[slot] = base + k; \
                LD[k] = n + 1; \
            } \
}

#ifndef KANG_BLOCK
/* One launch = `rounds` steps of the NK kangaroos of this work-item; the number of rounds is at most the depth of the
 * history buffer. Step number of the first round: step_hi:step_lo (the same for every kangaroo of the herd). */
__kernel void pz_kang_step(__global uint *state, __global const uint *jump, __global uint *dps, __global uint *dpcount,
                           __global uint *rlist, __global uint *rcount, __global ushort *hist, const uint rounds, const uint dp_mask_lo,
                           const uint dp_mask_hi, const uint cap_lo, const uint cap_hi, const uint dps_cap, const uint rlist_cap, const uint R,
                           const uint step_lo, const uint step_hi, const uint pz_n)
{
    const uint gid = (uint)get_global_id(0);
    PZ_GUARD(gid);
    const uint base = gid * NK;
    const size_t HS = (size_t)NK * pz_n;                  /* history stride between rounds */
    fe X[NK], Y[NK];
#if MC > 1
    fe TP[2][NK / MC];                                    /* products at the block boundaries, two sets used alternately */
#else
    fe TP[NK];
#endif
    uint FL[NK];                                          /* flags and jump index */
    KANG_TAGS;                                            /* the tags (KANG_TAGS_*) */
    ulong LD[NK];
    const ulong cap = (ulong)cap_lo | ((ulong)cap_hi << 32);
    const ulong step0 = (ulong)step_lo | ((ulong)step_hi << 32);
    for (int k = 0; k < NK; k++) {
        __global const uint *s = state + (size_t)(base + k) * KS;
        for (int i = 0; i < 8; i++) { X[k].v[i] = s[i]; Y[k].v[i] = s[8 + i]; }
        const uint prev = s[24], prev2 = s[25], ck = s[26], mn = s[27], esc = s[28];
        KANG_TAGS_IN(k, prev, prev2, ck, mn);
        LD[k] = (ulong)s[29] | ((ulong)s[30] << 32);
        FL[k] = (s[22] & 1u) | ((s[23] & 1u) << 1) | ((esc & 1u) << 2) | (kang_jidx(&X[k], prev2, ck, mn, esc, R) << FL_JSHIFT);
    }
    /* denominators of the first round and their prefix products */
    fe acc;
    for (int k = 0; k < NK; k++) {
        fe jx, d;
        fe_load_g16(&jx, jump + (FL[k] >> FL_JSHIFT) * JS);
        fe_sub_norm(&d, &X[k], &jx);
#if MC > 1
        if (k % MC == 0) { if (k == 0) fe_set_u32(&TP[0][0], 1); else TP[0][k / MC] = acc; }
#endif
        if (k == 0) acc = d;
        else fe_mul(&acc, &acc, &d);
#if MC == 1
        TP[k] = acc;
#endif
    }
    for (uint rd = 0; rd < rounds; rd++) {
        const ulong n = step0 + rd;
        const int bnd = (n & (KANG_CK_PERIOD - 1)) == 0;   /* checkpoint step: cycle detection, watchdog */
        {
            fe chk = acc;
            fe_normalize(&chk);
            if (fe_is_zero(&chk)) {
                /* a kangaroo sits on its jump point (practically never): all of them restart, no more steps this launch */
                for (int k = 0; k < NK; k++) {
                    const uint slot = atomic_inc(rcount);
                    if (slot < rlist_cap) rlist[slot] = base + k;
                }
                for (uint r2 = rd; r2 < rounds; r2++)
                    for (int k = 0; k < NK; k++) hist[(size_t)r2 * HS + (size_t)k * pz_n + gid] = H_NOP;
                break;
            }
        }
        fe inv;
        fe_inv(&inv, &acc);
        const int dir = rd & 1;                            /* even rounds: down (prefix products), odd rounds: up (suffix) */
        const int more = rd + 1 < rounds;
        fe accn;
#if MC > 1
        for (int b = 0; b < NK / MC; b++) {
            fe BX[MC], BD[MC], BQ[MC];
#if MC >= 4 && KANG_META
            uint BF[MC];                                  /* the flags of the block, read once (the quads kernel has the registers; at the
                                                           * 128-register cap of the pairs they would spill: there the step reads them again) */
#define FL_OF(c, k) BF[c]
#else
#define FL_OF(c, k) FL[k]
#endif
            /* denominators of this block (kept for the steps, with x) and, from the stored product of all later blocks, the product of
             * the denominators after each walker of the block */
#pragma unroll
            for (int c = 0; c < MC; c++) {
                const int i = b * MC + c, k = dir ? i : NK - 1 - i;
                fe jx0;
                BX[c] = X[k];
#if MC >= 4 && KANG_META
                BF[c] = FL[k];
#endif
                fe_load_g16(&jx0, jump + (FL_OF(c, k) >> FL_JSHIFT) * JS);
                fe_sub_norm(&BD[c], &BX[c], &jx0);
            }
            BQ[MC - 1] = TP[rd & 1][NK / MC - 1 - b];
#pragma unroll
            for (int c = MC - 2; c >= 0; c--) fe_mul(&BQ[c], &BQ[c + 1], &BD[c + 1]);
            if (more) { if (b == 0) fe_set_u32(&TP[(rd + 1) & 1][0], 1); else TP[(rd + 1) & 1][b] = accn; }
#pragma unroll
            for (int c = 0; c < MC; c++) {
                const int i = b * MC + c, k = dir ? i : NK - 1 - i;
                const uint f = FL_OF(c, k);
                const uint J = f >> FL_JSHIFT;
                const fe xk = BX[c], yk = Y[k];
                fe jx, jy, ik, lam, t, x3, y3;
                uint J2;
                fe_load_g16(&jx, jump + J * JS);
                fe_load_g16(&jy, jump + J * JS + 8);
                if (i < NK - 1) {
                    fe_mul(&ik, &inv, &BQ[c]);
                    fe_mul(&inv, &inv, &BD[c]);
                } else {
                    ik = inv;
                }
                KANG_STEP
                if (more) {                                /* denominator of the next round, running product */
                    fe jx2, d2;
                    fe_load_g16(&jx2, jump + J2 * JS);
                    fe_sub_norm(&d2, &x3, &jx2);
                    if (i == 0) accn = d2;
                    else fe_mul(&accn, &accn, &d2);
                }
            }
        }
#else
        for (int i = 0; i < NK; i++) {
            const int k = dir ? i : NK - 1 - i;
            const uint f = FL[k];
            const uint J = f >> FL_JSHIFT;
            const fe xk = X[k], yk = Y[k];
            fe jx, jy, ik, lam, t, x3, y3;
            uint J2;
            fe_load_g16(&jx, jump + J * JS);
            fe_load_g16(&jy, jump + J * JS + 8);
            if (i < NK - 1) {
                const fe pk = TP[dir ? k + 1 : k - 1];
                fe d;
                fe_mul(&ik, &inv, &pk);
                fe_sub_norm(&d, &xk, &jx);
                fe_mul(&inv, &inv, &d);
            } else {
                ik = inv;
            }
            KANG_STEP
            if (more) {                                    /* denominator of the next round, running product */
                fe jx2, d2;
                fe_load_g16(&jx2, jump + J2 * JS);
                fe_sub_norm(&d2, &x3, &jx2);
                if (i == 0) accn = d2;
                else fe_mul(&accn, &accn, &d2);
                TP[k] = accn;
            }
        }
#endif
        if (more) acc = accn;
    }
    /* write the herd back; d advances by the jumps of the whole launch */
    for (int k = 0; k < NK; k++) {
        __global uint *s = state + (size_t)(base + k) * KS;
        kang_replay_state(s, hist, (size_t)k * pz_n + gid, HS, rounds, jump);
        for (int i = 0; i < 8; i++) { s[i] = X[k].v[i]; s[8 + i] = Y[k].v[i]; }
        const uint f = FL[k];
        s[22] = f & 1u;
        s[23] = (f >> 1) & 1u;
        KANG_TAGS_OUT(s, k);
        s[28] = (f >> 2) & 1u;
        s[29] = (uint)LD[k];
        s[30] = (uint)(LD[k] >> 32);
        s[31] = 0;
    }
}
#else
/* The block herd (KANG_GROUP work-items per work-group, NK = a few walkers each): one field inversion (in the 32 lanes of one warp) per
 * work-group and round, instead of one per work-item. Each work-item multiplies the denominators of its walkers (prefix products, as above); the
 * products of the work-group go to local memory, where 32 work-items (one warp on NVIDIA, a different one each round, so that the
 * inversions fall on the four schedulers of an SM in turn) multiply the KANG_GROUP / 32 products of a column, invert the 32 column products
 * together (one fe_inv for the 32 lanes) and work back to the inverse of each product (Montgomery's trick on the column). The walkers stay in
 * the registers of their work-item for the whole launch (the loops over them are unrolled): the device memory sees the history (2 bytes a
 * step), the distinguished points and the state at the start and at the end of a launch. The walk, the history, the state and the events
 * are those of the kernel above with NK walkers per work-item; the host code does not tell them apart. */
#ifndef PZ_BARRIER                         /* tests/mock_cuda/pz_host.h has its own: there the work-items of a block take turns */
#ifdef PZ_CUDA
#if defined(PZ_BLOCK) && PZ_BLOCK != KANG_GROUP
#error "the block herd is built for work-groups of KANG_GROUP work-items: the CUDA block must have the same size"
#endif
#define PZ_LOCAL __shared__
#define PZ_LOCAL_PTR
#define PZ_BARRIER() __syncthreads()
#define PZ_LID() ((uint)threadIdx.x)
#define PZ_GRP() ((uint)blockIdx.x)
#define PZ_KANG_ATTR
#else
#define PZ_LOCAL __local
#define PZ_LOCAL_PTR __local
#define PZ_BARRIER() barrier(CLK_LOCAL_MEM_FENCE)
#define PZ_LID() ((uint)get_local_id(0))
#define PZ_GRP() ((uint)get_group_id(0))
#define PZ_KANG_ATTR __attribute__((reqd_work_group_size(KANG_GROUP, 1, 1)))
#endif
#endif
#define KANG_ROOTS 32                      /* inversions per work-group and round: the lanes of one warp */
#define KANG_COL (KANG_GROUP / KANG_ROOTS) /* products of the work-items under one inversion */

/* *r = a^-1 for every work-item of the work-group (a != 0). Work-item c of the set `set` (the work-items 32 set .. 32 set + 31) multiplies
 * the products of the work-items c, c + 32, c + 64, ..., inverts the result and works back to the inverse of each factor. bt and bq are
 * KANG_GROUP and KANG_GROUP - KANG_ROOTS elements of local memory. Every work-item of the group calls it, in the same round. */
inline void kang_group_inv(fe *r, const fe *a, PZ_LOCAL_PTR fe *bt, PZ_LOCAL_PTR fe *bq, const uint lid, const uint set)
{
    bt[lid] = *a;
    PZ_BARRIER();
    if (lid / KANG_ROOTS == set) {
        const uint c = lid % KANG_ROOTS;
        fe q = bt[c];
#pragma unroll
        for (int j = 1; j < KANG_COL; j++) {
            const fe p = bt[c + j * KANG_ROOTS];
            bq[c + (j - 1) * KANG_ROOTS] = q;
            fe_mul(&q, &q, &p);
        }
        fe inv;
        fe_inv(&inv, &q);
#pragma unroll
        for (int j = KANG_COL - 1; j > 0; j--) {
            const fe p = bt[c + j * KANG_ROOTS], b = bq[c + (j - 1) * KANG_ROOTS];
            fe t;
            fe_mul(&t, &inv, &b);
            bt[c + j * KANG_ROOTS] = t;
            fe_mul(&inv, &inv, &p);
        }
        bt[c] = inv;
    }
    PZ_BARRIER();
    *r = bt[lid];
}

__kernel PZ_KANG_ATTR void pz_kang_step(__global uint *state, __global const uint *jump, __global uint *dps, __global uint *dpcount,
                                        __global uint *rlist, __global uint *rcount, __global ushort *hist, const uint rounds,
                                        const uint dp_mask_lo, const uint dp_mask_hi, const uint cap_lo, const uint cap_hi, const uint dps_cap,
                                        const uint rlist_cap, const uint R, const uint step_lo, const uint step_hi, const uint pz_n)
{
    PZ_LOCAL fe BT[KANG_GROUP];
    PZ_LOCAL fe BQ[KANG_GROUP - KANG_ROOTS];
    const uint gid = (uint)get_global_id(0), lid = PZ_LID(), grp = PZ_GRP();
    const int mine = gid < pz_n;                          /* a work-item past the end of the herd takes part in the inversions only */
    int live = mine;                                      /* 0 once its walkers have been sent to restart */
    const uint base = gid * NK;
    const size_t HS = (size_t)NK * pz_n;                  /* history stride between rounds */
    fe X[NK], Y[NK], TP[NK];
    uint FL[NK];                                          /* flags and jump index */
    KANG_TAGS;                                            /* the tags (KANG_TAGS_*) */
    ulong LD[NK];
    const ulong cap = (ulong)cap_lo | ((ulong)cap_hi << 32);
    const ulong step0 = (ulong)step_lo | ((ulong)step_hi << 32);
    if (mine) {
#pragma unroll
        for (int k = 0; k < NK; k++) {
            __global const uint *s = state + (size_t)(base + k) * KS;
            for (int i = 0; i < 8; i++) { X[k].v[i] = s[i]; Y[k].v[i] = s[8 + i]; }
            const uint prev = s[24], prev2 = s[25], ck = s[26], mn = s[27], esc = s[28];
            KANG_TAGS_IN(k, prev, prev2, ck, mn);
            LD[k] = (ulong)s[29] | ((ulong)s[30] << 32);
            FL[k] = (s[22] & 1u) | ((s[23] & 1u) << 1) | ((esc & 1u) << 2) | (kang_jidx(&X[k], prev2, ck, mn, esc, R) << FL_JSHIFT);
        }
    }
    for (uint rd = 0; rd < rounds; rd++) {
        const ulong n = step0 + rd;
        const int bnd = (n & (KANG_CK_PERIOD - 1)) == 0;   /* checkpoint step: cycle detection, watchdog */
#if KANG_META
        const int more = rd + 1 < rounds;
#endif
        fe acc;
        fe_set_u32(&acc, 1);
        if (live) {
            /* the denominators of this round and their prefix products */
#pragma unroll
            for (int k = 0; k < NK; k++) {
                fe jx, d;
                fe_load_g16(&jx, jump + (FL[k] >> FL_JSHIFT) * JS);
                fe_sub_norm(&d, &X[k], &jx);
                if (k == 0) TP[0] = d;
                else fe_mul(&TP[k], &TP[k - 1], &d);
            }
            fe chk = TP[NK - 1];
            fe_normalize(&chk);
            if (fe_is_zero(&chk)) {
                /* a kangaroo sits on its jump point (practically never): the walkers of the work-item restart, no more steps this launch */
                for (int k = 0; k < NK; k++) {
                    const uint slot = atomic_inc(rcount);
                    if (slot < rlist_cap) rlist[slot] = base + k;
                }
                for (uint r2 = rd; r2 < rounds; r2++)
                    for (int k = 0; k < NK; k++) hist[(size_t)r2 * HS + (size_t)k * pz_n + gid] = H_NOP;
                live = 0;
            } else {
                acc = TP[NK - 1];
            }
        }
        fe inv;
        kang_group_inv(&inv, &acc, BT, BQ, lid, (rd + grp) % KANG_COL);
        if (!live) continue;
#pragma unroll
        for (int k = NK - 1; k >= 0; k--) {
            const uint f = FL[k];
            const uint J = f >> FL_JSHIFT;
            const fe xk = X[k], yk = Y[k];
            fe jx, jy, ik, lam, t, x3, y3;
            uint J2;
            fe_load_g16(&jx, jump + J * JS);
            fe_load_g16(&jy, jump + J * JS + 8);
            if (k > 0) {
                fe d;
                fe_mul(&ik, &inv, &TP[k - 1]);
                fe_sub_norm(&d, &xk, &jx);
                fe_mul(&inv, &inv, &d);
            } else {
                ik = inv;
            }
            KANG_STEP
        }
    }
    if (!mine) return;
    /* write the herd back; d advances by the jumps of the whole launch */
#pragma unroll
    for (int k = 0; k < NK; k++) {
        __global uint *s = state + (size_t)(base + k) * KS;
        kang_replay_state(s, hist, (size_t)k * pz_n + gid, HS, rounds, jump);
        for (int i = 0; i < 8; i++) { s[i] = X[k].v[i]; s[8 + i] = Y[k].v[i]; }
        const uint f = FL[k];
        s[22] = f & 1u;
        s[23] = (f >> 1) & 1u;
        KANG_TAGS_OUT(s, k);
        s[28] = (f >> 2) & 1u;
        s[29] = (uint)LD[k];
        s[30] = (uint)(LD[k] >> 32);
        s[31] = 0;
    }
}
#endif
