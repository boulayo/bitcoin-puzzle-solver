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
 * jump table (constant): per jump 20 uints = x[8] y[8] v[4]; R regular jumps, then KANG_NESC escape jumps
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
 *  - The state a step needs besides x and y is a few 32-bit words (tags, flags, jump index).
 * The history of a launch is rounds * NK * work-items 16-bit entries, laid out so that the work-items of a warp write
 * adjacent addresses. Entry: bits 0-8 jump index, bit 9 negated, 0xFFFF no step (a round skipped for a zero product). */

#ifndef NK
#define NK 64
#endif
#ifndef MC
#define MC 1                       /* kangaroos per block of stored batch products (1: every one) */
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

inline void d_add_jump(uint d[6], __constant const uint *v)
{
    ulong acc = (ulong)d[0] + v[0];
    d[0] = (uint)acc;
    ulong c = acc >> 32;
    acc = (ulong)d[1] + v[1] + c; d[1] = (uint)acc; c = acc >> 32;
    acc = (ulong)d[2] + v[2] + c; d[2] = (uint)acc; c = acc >> 32;
    acc = (ulong)d[3] + v[3] + c; d[3] = (uint)acc; c = acc >> 32;
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

/* d += the jumps of `nr` rounds of the history of one kangaroo (entries off0, off0 + stride, ...) */
inline void kang_replay(__global const ushort *hist, const size_t off0, const size_t stride, const uint nr, __constant const uint *jump, uint d[6])
{
    for (uint i = 0; i < nr; i++) {
        const uint h = hist[off0 + (size_t)i * stride];
        if (h == H_NOP) continue;
        d_add_jump(d, jump + (h & H_JMASK) * JS + 16);
        if (h & H_FLIP) d_neg(d);
    }
}

/* A distinguished point after round `rd` of the launch: d is rebuilt from its value at the start of the launch */
PZ_NOINLINE void kang_emit_dp(__global const uint *st, __global const ushort *hist, const size_t off0, const size_t stride, const uint nr,
                              __constant const uint *jump, __global uint *dps, __global uint *dpcount, const uint dps_cap, const uint widx,
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

/* the step of one kangaroo once its inverse ik is known; needs k, i, rd, n, bnd, f, J, xk, yk, jx, jy, ik, t, lam, x3, y3, J2 declared */
#define KANG_STEP { \
            fe_sub(&t, &yk, &jy); \
            fe_mul(&lam, &t, &ik); \
            fe_sqr(&x3, &lam); \
            fe_sub(&x3, &x3, &xk); \
            fe_sub(&x3, &x3, &jx); \
            fe_sub(&t, &xk, &x3); \
            fe_mul(&y3, &lam, &t); \
            fe_sub(&y3, &y3, &yk); \
            fe_normalize(&x3); \
            fe_normalize(&y3); \
            const uint flip = y3.v[0] & 1u; \
            fe_cneg_norm(&y3, flip);                       /* canonical representative: even y */ \
            hist[(size_t)rd * HS + (size_t)k * pz_n + gid] = (ushort)(J | (flip ? H_FLIP : 0u)); \
            /* cycle detection with the tag of the point just left */ \
            const uint tag = xk.v[4]; \
            uint ck = CM[k][0], mn = CM[k][1], esc = (f >> 2) & 1u; \
            const uint ck0 = ck, mn0 = mn; \
            kang_cycle(bnd, tag, &ck, &mn, &esc); \
            if (ck != ck0 || mn != mn0) { CM[k][0] = ck; CM[k][1] = mn; } \
            const uint prev2 = PP[k][0];                   /* the old prev becomes prev2 */ \
            PP[k][0] = tag; \
            PP[k][1] = prev2; \
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

/* One launch = `rounds` steps of the NK kangaroos of this work-item; the number of rounds is at most the depth of the
 * history buffer. Step number of the first round: step_hi:step_lo (the same for every kangaroo of the herd). */
__kernel void pz_kang_step(__global uint *state, __constant const uint *jump, __global uint *dps, __global uint *dpcount,
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
    uint FL[NK], PP[NK][2], CM[NK][2];                    /* flags and jump index; prev, prev2; checkpoint, least tag */
    ulong LD[NK];
    const ulong cap = (ulong)cap_lo | ((ulong)cap_hi << 32);
    const ulong step0 = (ulong)step_lo | ((ulong)step_hi << 32);
    for (int k = 0; k < NK; k++) {
        __global const uint *s = state + (size_t)(base + k) * KS;
        for (int i = 0; i < 8; i++) { X[k].v[i] = s[i]; Y[k].v[i] = s[8 + i]; }
        const uint prev = s[24], prev2 = s[25], ck = s[26], mn = s[27], esc = s[28];
        PP[k][0] = prev; PP[k][1] = prev2;
        CM[k][0] = ck; CM[k][1] = mn;
        LD[k] = (ulong)s[29] | ((ulong)s[30] << 32);
        FL[k] = (s[22] & 1u) | ((s[23] & 1u) << 1) | ((esc & 1u) << 2) | (kang_jidx(&X[k], prev2, ck, mn, esc, R) << FL_JSHIFT);
    }
    /* denominators of the first round and their prefix products */
    fe acc;
    for (int k = 0; k < NK; k++) {
        fe jx, d;
        fe_load_c(&jx, jump + (FL[k] >> FL_JSHIFT) * JS);
        fe_sub(&d, &X[k], &jx);
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
            /* denominators of this block (kept for the steps) and, from the stored product of all later blocks, the
             * product of the denominators after each walker of the block */
#pragma unroll
            for (int c = 0; c < MC; c++) {
                const int i = b * MC + c, k = dir ? i : NK - 1 - i;
                fe jx0;
                BX[c] = X[k];
                fe_load_c(&jx0, jump + (FL[k] >> FL_JSHIFT) * JS);
                fe_sub(&BD[c], &BX[c], &jx0);
            }
            BQ[MC - 1] = TP[rd & 1][NK / MC - 1 - b];
#pragma unroll
            for (int c = MC - 2; c >= 0; c--) fe_mul(&BQ[c], &BQ[c + 1], &BD[c + 1]);
            if (more) { if (b == 0) fe_set_u32(&TP[(rd + 1) & 1][0], 1); else TP[(rd + 1) & 1][b] = accn; }
#pragma unroll
            for (int c = 0; c < MC; c++) {
                const int i = b * MC + c, k = dir ? i : NK - 1 - i;
                const uint f = FL[k];
                const uint J = f >> FL_JSHIFT;
                const fe xk = BX[c], yk = Y[k];
                fe jx, jy, ik, lam, t, x3, y3;
                uint J2;
                fe_load_c(&jx, jump + J * JS);
                fe_load_c(&jy, jump + J * JS + 8);
                if (i < NK - 1) {
                    fe_mul(&ik, &inv, &BQ[c]);
                    fe_mul(&inv, &inv, &BD[c]);
                } else {
                    ik = inv;
                }
                KANG_STEP
                if (more) {                                /* denominator of the next round, running product */
                    fe jx2, d2;
                    fe_load_c(&jx2, jump + J2 * JS);
                    fe_sub(&d2, &x3, &jx2);
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
            fe_load_c(&jx, jump + J * JS);
            fe_load_c(&jy, jump + J * JS + 8);
            if (i < NK - 1) {
                const fe pk = TP[dir ? k + 1 : k - 1];
                fe d;
                fe_mul(&ik, &inv, &pk);
                fe_sub(&d, &xk, &jx);
                fe_mul(&inv, &inv, &d);
            } else {
                ik = inv;
            }
            KANG_STEP
            if (more) {                                    /* denominator of the next round, running product */
                fe jx2, d2;
                fe_load_c(&jx2, jump + J2 * JS);
                fe_sub(&d2, &x3, &jx2);
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
        uint d[6];
        for (int i = 0; i < 6; i++) d[i] = s[16 + i];
        kang_replay(hist, (size_t)k * pz_n + gid, HS, rounds, jump, d);
        for (int i = 0; i < 8; i++) { s[i] = X[k].v[i]; s[8 + i] = Y[k].v[i]; }
        for (int i = 0; i < 6; i++) s[16 + i] = d[i];
        const uint f = FL[k];
        s[22] = f & 1u;
        s[23] = (f >> 1) & 1u;
        s[24] = PP[k][0];
        s[25] = PP[k][1];
        s[26] = CM[k][0];
        s[27] = CM[k][1];
        s[28] = (f >> 2) & 1u;
        s[29] = (uint)LD[k];
        s[30] = (uint)(LD[k] >> 32);
        s[31] = 0;
    }
}
