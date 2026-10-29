/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_common.cl - secp256k1 field/curve arithmetic and hash160 for OpenCL.
 * Field elements are 8 x 32-bit limbs, little-endian; values are lazy (< 2^256)
 * exactly like the host code in fe.h. */

#ifndef PZ_CUDA
#define PZ_CONST_TABLE __constant     /* file-scope constant tables (CUDA: __constant__, see pz_cuda.h) */
#define PZ_NOINLINE inline            /* CUDA: a real call, so the big routines are compiled once (much smaller PTX) */
#define PZ_CTZ(x) (31 - (int)clz((x) & (0u - (x))))   /* trailing zeros of a non-zero x (OpenCL C 1.2 has clz only) */
typedef long s64;
#endif
/* Every kernel takes the number of work-items as its last argument: CUDA launches whole blocks, so the threads past
 * the end must return at once (with OpenCL the global size is exact and the test never fires). */
#define PZ_GUARD(gid) if ((gid) >= pz_n) return

typedef uint u32;
typedef ulong u64;
typedef struct { uint v[8]; } fe;
typedef struct { fe x, y; } ge;

PZ_CONST_TABLE uint SHA_K[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
PZ_CONST_TABLE uint SHA_H0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
PZ_CONST_TABLE uchar RMD_RL[80] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8, 3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12, 1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2, 4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};
PZ_CONST_TABLE uchar RMD_RR[80] = {5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12, 6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2, 15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13, 8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14, 12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};
PZ_CONST_TABLE uchar RMD_SL[80] = {11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8, 7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12, 11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5, 11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12, 9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};
PZ_CONST_TABLE uchar RMD_SR[80] = {8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6, 9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11, 9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5, 15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8, 8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};
PZ_CONST_TABLE uint RMD_KL[5] = {0x00000000u, 0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xA953FD4Eu};
PZ_CONST_TABLE uint RMD_KR[5] = {0x50A28BE6u, 0x5C4DD124u, 0x6D703EF3u, 0x7A6D76E9u, 0x00000000u};

/* ---------------- field ---------------- */

inline void fe_set_u32(fe *r, uint a) { r->v[0] = a; for (int i = 1; i < 8; i++) r->v[i] = 0; }

#ifndef PZ_FAST_FE
/* ---- OpenCL: the same carry chains with clang's add-with-carry builtins on AMD GPUs, where they become v_add_co_ci_u32 chains (an
 * addition takes 15 instructions instead of 69 on gfx1030), and on pocl, which tests that path; elsewhere a C stand-in for them
 * (other compilers may not have the builtins, or not lower them well). ---- */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#if (defined(__AMDGCN__) || defined(POCL_DEVICE_ADDRESS_BITS)) && __has_builtin(__builtin_addc) && __has_builtin(__builtin_subc) \
    && !defined(PZ_TEST_NO_BUILTINS)
#define PZ_ADDC(a, b, ci, co) __builtin_addc((uint)(a), (uint)(b), (ci), (co))
#define PZ_SUBC(a, b, ci, co) __builtin_subc((uint)(a), (uint)(b), (ci), (co))
#define PZ_CARRY 1                    /* the products on carry chains too (below) */
#else
inline uint pz_addc(uint a, uint b, uint ci, uint *co) { const ulong s = (ulong)a + b + ci; *co = (uint)(s >> 32); return (uint)s; }
inline uint pz_subc(uint a, uint b, uint ci, uint *co) { const ulong s = (ulong)a - b - ci; *co = (uint)(s >> 63); return (uint)s; }
#define PZ_ADDC(a, b, ci, co) pz_addc((a), (b), (ci), (co))
#define PZ_SUBC(a, b, ci, co) pz_subc((a), (b), (ci), (co))
#ifdef PZ_TEST_CARRY
#define PZ_CARRY 1                    /* tests/t_gpufe: the products of the builtin path, here with the C helpers */
#endif
#endif

/* r = a + b, folding 2^256 = 2^32 + 977 (mod p); a second wrap leaves less than 2^32 + 977, so its fold stops at limb 2 */
inline void fe_add(fe *r, const fe *a, const fe *b)
{
    uint o[8], c = 0, w;
#pragma unroll
    for (int i = 0; i < 8; i++) o[i] = PZ_ADDC(a->v[i], b->v[i], c, &c);
    w = c;
    o[0] = PZ_ADDC(o[0], w * 977u, 0u, &c);
    o[1] = PZ_ADDC(o[1], w, c, &c);
#pragma unroll
    for (int i = 2; i < 8; i++) o[i] = PZ_ADDC(o[i], 0u, c, &c);
    w = c;
    o[0] = PZ_ADDC(o[0], w * 977u, 0u, &c);
    o[1] = PZ_ADDC(o[1], w, c, &c);
    o[2] += c;
#pragma unroll
    for (int i = 0; i < 8; i++) r->v[i] = o[i];
}

/* r = a - b: a borrow means a - b + 2^256, too large by 2^32 + 977; if subtracting that borrows again, once more (it stops at limb 2) */
inline void fe_sub(fe *r, const fe *a, const fe *b)
{
    uint o[8], c = 0, w;
#pragma unroll
    for (int i = 0; i < 8; i++) o[i] = PZ_SUBC(a->v[i], b->v[i], c, &c);
    w = c;
    o[0] = PZ_SUBC(o[0], w * 977u, 0u, &c);
    o[1] = PZ_SUBC(o[1], w, c, &c);
#pragma unroll
    for (int i = 2; i < 8; i++) o[i] = PZ_SUBC(o[i], 0u, c, &c);
    w = c;
    o[0] = PZ_SUBC(o[0], w * 977u, 0u, &c);
    o[1] = PZ_SUBC(o[1], w, c, &c);
    o[2] -= c;
#pragma unroll
    for (int i = 0; i < 8; i++) r->v[i] = o[i];
}

/* r = a - b - c: both borrow chains, then one correction by the number of borrows (0 to 2), as in the CUDA version */
inline void fe_sub2(fe *r, const fe *a, const fe *b, const fe *c)
{
    uint o[8], k = 0, k2 = 0, w;
#pragma unroll
    for (int i = 0; i < 8; i++) o[i] = PZ_SUBC(a->v[i], b->v[i], k, &k);
#pragma unroll
    for (int i = 0; i < 8; i++) o[i] = PZ_SUBC(o[i], c->v[i], k2, &k2);
    w = k + k2;
    o[0] = PZ_SUBC(o[0], w * 977u, 0u, &k);
    o[1] = PZ_SUBC(o[1], w, k, &k);
#pragma unroll
    for (int i = 2; i < 8; i++) o[i] = PZ_SUBC(o[i], 0u, k, &k);
    w = k;
    o[0] = PZ_SUBC(o[0], w * 977u, 0u, &k);
    o[1] = PZ_SUBC(o[1], w, k, &k);
    o[2] -= k;
#pragma unroll
    for (int i = 0; i < 8; i++) r->v[i] = o[i];
}

#else
/* ---- CUDA: addition, subtraction and (below) the products and their reduction on PTX carry chains (PZ_FAST_FE,
 * pz_cuda.h): on sm_86 add+sub take 88 instead of 176 instructions. Same results (congruent, < 2^256); the start-up
 * self-test compares them with the CPU bit for bit. ---- */


inline void fe_add(fe *r, const fe *a, const fe *b)
{
    uint r0, r1, r2, r3, r4, r5, r6, r7, c;
    ADD_CC(r0, a->v[0], b->v[0]); ADDC_CC(r1, a->v[1], b->v[1]); ADDC_CC(r2, a->v[2], b->v[2]); ADDC_CC(r3, a->v[3], b->v[3]);
    ADDC_CC(r4, a->v[4], b->v[4]); ADDC_CC(r5, a->v[5], b->v[5]); ADDC_CC(r6, a->v[6], b->v[6]); ADDC_CC(r7, a->v[7], b->v[7]);
    ADDC(c, 0u, 0u);
    /* 2^256 = 2^32 + 977 (mod p) */
    ADD_CC(r0, r0, c * 977u); ADDC_CC(r1, r1, c); ADDC_CC(r2, r2, 0u); ADDC_CC(r3, r3, 0u);
    ADDC_CC(r4, r4, 0u); ADDC_CC(r5, r5, 0u); ADDC_CC(r6, r6, 0u); ADDC_CC(r7, r7, 0u);
    ADDC(c, 0u, 0u);
    /* a second wrap leaves r < 2^32 + 977: the fold stops at limb 2 */
    ADD_CC(r0, r0, c * 977u); ADDC_CC(r1, r1, c); ADDC(r2, r2, 0u);
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3; r->v[4] = r4; r->v[5] = r5; r->v[6] = r6; r->v[7] = r7;
}

inline void fe_sub(fe *r, const fe *a, const fe *b)
{
    uint r0, r1, r2, r3, r4, r5, r6, r7, w;
    SUB_CC(r0, a->v[0], b->v[0]); SUBC_CC(r1, a->v[1], b->v[1]); SUBC_CC(r2, a->v[2], b->v[2]); SUBC_CC(r3, a->v[3], b->v[3]);
    SUBC_CC(r4, a->v[4], b->v[4]); SUBC_CC(r5, a->v[5], b->v[5]); SUBC_CC(r6, a->v[6], b->v[6]); SUBC_CC(r7, a->v[7], b->v[7]);
    SUBC(w, 0u, 0u);
    w &= 1u;   /* borrow: the result is a - b + 2^256, i.e. too large by 2^32 + 977 */
    SUB_CC(r0, r0, w * 977u); SUBC_CC(r1, r1, w); SUBC_CC(r2, r2, 0u); SUBC_CC(r3, r3, 0u);
    SUBC_CC(r4, r4, 0u); SUBC_CC(r5, r5, 0u); SUBC_CC(r6, r6, 0u); SUBC_CC(r7, r7, 0u);
    SUBC(w, 0u, 0u);
    w &= 1u;   /* borrowed again: the value is now >= 2^256 - 2^32 - 977, subtract once more (stops at limb 2) */
    SUB_CC(r0, r0, w * 977u); SUBC_CC(r1, r1, w); SUBC(r2, r2, 0u);
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3; r->v[4] = r4; r->v[5] = r5; r->v[6] = r6; r->v[7] = r7;
}

/* r = a - b - c: both borrow chains first, then one correction by the number of borrows w (0 to 2), w (2^32 + 977); if that borrows
 * again the value is at least 2^256 - 2^33 - 1954 and one more 2^32 + 977 is subtracted (it stops at limb 2). 36 instructions
 * instead of the 50 of two fe_sub. */
inline void fe_sub2(fe *r, const fe *a, const fe *b, const fe *c)
{
    uint r0, r1, r2, r3, r4, r5, r6, r7, w, w2;
    SUB_CC(r0, a->v[0], b->v[0]); SUBC_CC(r1, a->v[1], b->v[1]); SUBC_CC(r2, a->v[2], b->v[2]); SUBC_CC(r3, a->v[3], b->v[3]);
    SUBC_CC(r4, a->v[4], b->v[4]); SUBC_CC(r5, a->v[5], b->v[5]); SUBC_CC(r6, a->v[6], b->v[6]); SUBC_CC(r7, a->v[7], b->v[7]);
    SUBC(w, 0u, 0u);
    SUB_CC(r0, r0, c->v[0]); SUBC_CC(r1, r1, c->v[1]); SUBC_CC(r2, r2, c->v[2]); SUBC_CC(r3, r3, c->v[3]);
    SUBC_CC(r4, r4, c->v[4]); SUBC_CC(r5, r5, c->v[5]); SUBC_CC(r6, r6, c->v[6]); SUBC_CC(r7, r7, c->v[7]);
    SUBC(w2, 0u, 0u);
    w = (w & 1u) + (w2 & 1u);
    SUB_CC(r0, r0, w * 977u); SUBC_CC(r1, r1, w); SUBC_CC(r2, r2, 0u); SUBC_CC(r3, r3, 0u);
    SUBC_CC(r4, r4, 0u); SUBC_CC(r5, r5, 0u); SUBC_CC(r6, r6, 0u); SUBC_CC(r7, r7, 0u);
    SUBC(w, 0u, 0u);
    w &= 1u;
    SUB_CC(r0, r0, w * 977u); SUBC_CC(r1, r1, w); SUBC(r2, r2, 0u);
    r->v[0] = r0; r->v[1] = r1; r->v[2] = r2; r->v[3] = r3; r->v[4] = r4; r->v[5] = r5; r->v[6] = r6; r->v[7] = r7;
}

#endif

#ifdef PZ_FAST_FE
/* ---- CUDA: the products on mad.wide chains. ptxas turns a mad.lo.cc and a madc.hi.cc of the same two limbs, whose addends are
 * the low and high halves of a 64-bit register pair, into one IMAD.WIDE.U32 (.X when it takes the carry): one instruction per
 * limb product, against about three for the product written in C (an IMAD.WIDE.U32 plus the 64-bit additions of the column and
 * a move that zeroes the high half of the addend). For the pairs to stay aligned the products go to two accumulators: E gets
 * the ones whose position (i + j for a[j] * b[i]) is even, O the odd ones, and E + O is formed once at the end. On sm_86
 * (ptxas 12.0, the routine alone) a multiplication goes from 235 to 121 instructions and a square from 160 to 81. ---- */

/* acc[i + j] += a[j] * bi for j = j0, j0 + 2, .. 7, as one carry chain over the pairs (i + j, i + j + 1). The positions of acc
 * up to *top hold values, the ones above are zero; the carry out of the last pair goes to the next position. All the
 * arguments but the limbs are constants once the loops are unrolled, so the tests below cost nothing. */
inline void fe_mad_chain(uint *acc, int *top, const uint *a, const uint bi, const int i, const int j0)
{
    const int tp = *top;
    int k = i + j0;
#pragma unroll
    for (int j = j0; j < 8; j += 2) {
        k = i + j;
        const uint lo = k <= tp ? acc[k] : 0u, hi = k + 1 <= tp ? acc[k + 1] : 0u;
        if (j == j0) MADLO_CC(acc[k], a[j], bi, lo);
        else MADLOC_CC(acc[k], a[j], bi, lo);
        if (j + 2 < 8 || k + 1 <= tp) MADHIC_CC(acc[k + 1], a[j], bi, hi);
        else MADHIC(acc[k + 1], a[j], bi, hi);   /* the last pair was zero or half zero: no carry out */
    }
    if (k + 1 <= tp) { ADDC(acc[k + 2], 0u, 0u); *top = k + 2; }
    else *top = k + 1;
}

/* t = E + O, with E[0] at position 0 and O starting at position 1 */
inline void fe_eo_sum(uint t[16], const uint *E, const int te, const uint *O, const int to)
{
    t[0] = E[0];
    ADD_CC(t[1], E[1], O[1]);
#pragma unroll
    for (int k = 2; k < 15; k++) ADDC_CC(t[k], k <= te ? E[k] : 0u, k <= to ? O[k] : 0u);
    ADDC(t[15], 15 <= te ? E[15] : 0u, 15 <= to ? O[15] : 0u);
}

/* r = t mod p for a 512-bit t = L + H 2^256 (16 limbs), result < 2^256: u = L + H (2^32 + 977) in two chains of mad.wide, the
 * limbs of H at even positions times 977 with L, those at odd positions times 977 with H 2^32 (whose limbs, two by two, are
 * the addend pairs), then u + the top limbs of u (2^32 + 977). */
inline void fe_reduce512(fe *r, const uint t[16])
{
    uint u0, u1, u2, u3, u4, u5, u6, u7, u8, u9, z1, z2, z3, z4, z5, z6, z7, z8, z9;
    MADLO_CC(u0, t[8], 977u, t[0]); MADHIC_CC(u1, t[8], 977u, t[1]); MADLOC_CC(u2, t[10], 977u, t[2]); MADHIC_CC(u3, t[10], 977u, t[3]);
    MADLOC_CC(u4, t[12], 977u, t[4]); MADHIC_CC(u5, t[12], 977u, t[5]); MADLOC_CC(u6, t[14], 977u, t[6]); MADHIC_CC(u7, t[14], 977u, t[7]);
    ADDC(u8, 0u, 0u);
    MADLO_CC(z1, t[9], 977u, t[8]); MADHIC_CC(z2, t[9], 977u, t[9]); MADLOC_CC(z3, t[11], 977u, t[10]); MADHIC_CC(z4, t[11], 977u, t[11]);
    MADLOC_CC(z5, t[13], 977u, t[12]); MADHIC_CC(z6, t[13], 977u, t[13]); MADLOC_CC(z7, t[15], 977u, t[14]); MADHIC_CC(z8, t[15], 977u, t[15]);
    ADDC(z9, 0u, 0u);
    /* u = 10 limbs: u8 + u9 2^32 < 2^32 + 977 */
    ADD_CC(u1, u1, z1); ADDC_CC(u2, u2, z2); ADDC_CC(u3, u3, z3); ADDC_CC(u4, u4, z4); ADDC_CC(u5, u5, z5); ADDC_CC(u6, u6, z6);
    ADDC_CC(u7, u7, z7); ADDC_CC(u8, u8, z8); ADDC(u9, z9, 0u);
    /* fold top = u8 + u9*2^32 (< 2^33): add top*977 + top*2^32 (< 2^66) to the low 256 bits */
    const ulong top = (ulong)u8 | ((ulong)u9 << 32);
    const ulong v = top * 977UL;
    const ulong mid = (v >> 32) + (top & 0xFFFFFFFFUL);
    const uint w0 = (uint)v, w1 = (uint)mid, w2 = (uint)((top >> 32) + (mid >> 32));
    uint c;
    ADD_CC(u0, u0, w0); ADDC_CC(u1, u1, w1); ADDC_CC(u2, u2, w2); ADDC_CC(u3, u3, 0u);
    ADDC_CC(u4, u4, 0u); ADDC_CC(u5, u5, 0u); ADDC_CC(u6, u6, 0u); ADDC_CC(u7, u7, 0u);
    ADDC(c, 0u, 0u);
    /* a wrap leaves a value < 2^66: the last fold stops at limb 2 */
    ADD_CC(u0, u0, c * 977u); ADDC_CC(u1, u1, c); ADDC(u2, u2, 0u);
    r->v[0] = u0; r->v[1] = u1; r->v[2] = u2; r->v[3] = u3; r->v[4] = u4; r->v[5] = u5; r->v[6] = u6; r->v[7] = u7;
}

inline void fe_mul(fe *r, const fe *a, const fe *b)
{
    uint E[18], O[18], t[16];
    int te = -1, to = -1;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        /* a[j] * b[i] is at position i + j: the even j go to E when i is even, to O when it is odd */
        fe_mad_chain((i & 1) ? O : E, (i & 1) ? &to : &te, a->v, b->v[i], i, 0);
        fe_mad_chain((i & 1) ? E : O, (i & 1) ? &te : &to, a->v, b->v[i], i, 1);
    }
    fe_eo_sum(t, E, te, O, to);
    fe_reduce512(r, t);
}

/* dedicated square: the 28 products a_i*a_j (i < j) once, doubled, plus the 8 squares a_i^2 on one chain (36 products instead of 64) */
inline void fe_sqr(fe *r, const fe *a)
{
    uint E[18], O[18], s[16], t[16];
    int te = -1, to = -1;
#pragma unroll
    for (int i = 0; i < 7; i++) {
        fe_mad_chain(O, &to, a->v, a->v[i], i, i + 1);       /* j = i + 1, i + 3, ..: odd positions */
        if (i < 6) fe_mad_chain(E, &te, a->v, a->v[i], i, i + 2);   /* j = i + 2, i + 4, ..: even positions from 2 */
    }
    /* s = E + O: position 1 is O alone, E starts at position 2; the sum is < 2^511 */
    s[0] = 0;
    s[1] = O[1];
    ADD_CC(s[2], E[2], O[2]);
#pragma unroll
    for (int k = 3; k < 15; k++) ADDC_CC(s[k], k <= te ? E[k] : 0u, k <= to ? O[k] : 0u);
    ADDC(s[15], 15 <= te ? E[15] : 0u, 15 <= to ? O[15] : 0u);
#pragma unroll
    for (int k = 15; k > 1; k--) t[k] = (s[k] << 1) | (s[k - 1] >> 31);
    t[1] = s[1] << 1;
    t[0] = 0;
    MADLO_CC(t[0], a->v[0], a->v[0], t[0]); MADHIC_CC(t[1], a->v[0], a->v[0], t[1]);
#pragma unroll
    for (int i = 1; i < 7; i++) { MADLOC_CC(t[2 * i], a->v[i], a->v[i], t[2 * i]); MADHIC_CC(t[2 * i + 1], a->v[i], a->v[i], t[2 * i + 1]); }
    MADLOC_CC(t[14], a->v[7], a->v[7], t[14]); MADHIC(t[15], a->v[7], a->v[7], t[15]);
    fe_reduce512(r, t);
}
#elif defined(PZ_CARRY)
/* r = t mod p for a 512-bit t = L + H 2^256 (16 limbs), result < 2^256: u = L + 977 H (one 64-bit product per limb, the halves
 * added on one carry chain), u + H 2^32, then u + the top limbs of u (2^32 + 977) */
inline void fe_reduce512(fe *r, const uint t[16])
{
    uint u[10], c = 0, w;
    ulong q[8];
#pragma unroll
    for (int i = 0; i < 8; i++) q[i] = (ulong)t[8 + i] * 977u + t[i];
    u[0] = (uint)q[0];
#pragma unroll
    for (int i = 1; i < 8; i++) u[i] = PZ_ADDC((uint)q[i], (uint)(q[i - 1] >> 32), c, &c);
    u[8] = (uint)(q[7] >> 32) + c;
    c = 0;
#pragma unroll
    for (int i = 1; i < 9; i++) u[i] = PZ_ADDC(u[i], t[7 + i], c, &c);
    u[9] = c;
    /* fold top = u8 + u9*2^32 (< 2^33): add top*977 + top*2^32 (< 2^66) to the low 256 bits; a wrap leaves less than 2^66 */
    const ulong top = (ulong)u[8] | ((ulong)u[9] << 32);
    const ulong v = top * 977UL;
    const ulong mid = (v >> 32) + (top & 0xFFFFFFFFUL);
    u[0] = PZ_ADDC(u[0], (uint)v, 0u, &c);
    u[1] = PZ_ADDC(u[1], (uint)mid, c, &c);
    u[2] = PZ_ADDC(u[2], (uint)((top >> 32) + (mid >> 32)), c, &c);
#pragma unroll
    for (int i = 3; i < 8; i++) u[i] = PZ_ADDC(u[i], 0u, c, &c);
    w = c;
    u[0] = PZ_ADDC(u[0], w * 977u, 0u, &c);
    u[1] = PZ_ADDC(u[1], w, c, &c);
    u[2] += c;
#pragma unroll
    for (int i = 0; i < 8; i++) r->v[i] = u[i];
}

/* the 8x8 product by rows: the 64-bit products of a row, each with the limb of the rows before as addend, then their halves on one
 * carry chain */
inline void fe_mul(fe *r, const fe *a, const fe *b)
{
    uint t[16];
#pragma unroll
    for (int i = 0; i < 16; i++) t[i] = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        ulong q[8];
        uint c = 0;
#pragma unroll
        for (int j = 0; j < 8; j++) q[j] = (ulong)a->v[j] * b->v[i] + t[i + j];
        t[i] = (uint)q[0];
#pragma unroll
        for (int j = 1; j < 8; j++) t[i + j] = PZ_ADDC((uint)q[j], (uint)(q[j - 1] >> 32), c, &c);
        t[i + 8] = (uint)(q[7] >> 32) + c;
    }
    fe_reduce512(r, t);
}

/* dedicated square: the 28 products a_i*a_j (i < j) once, doubled, plus the 8 squares (36 products instead of 64) */
inline void fe_sqr(fe *r, const fe *a)
{
    uint t[16], c;
#pragma unroll
    for (int i = 0; i < 16; i++) t[i] = 0;
#pragma unroll
    for (int i = 0; i < 7; i++) {
        ulong q[8];
        c = 0;
#pragma unroll
        for (int j = i + 1; j < 8; j++) q[j] = (ulong)a->v[j] * a->v[i] + t[i + j];
        t[2 * i + 1] = (uint)q[i + 1];
#pragma unroll
        for (int j = i + 2; j < 8; j++) t[i + j] = PZ_ADDC((uint)q[j], (uint)(q[j - 1] >> 32), c, &c);
        t[i + 8] = (uint)(q[7] >> 32) + c;
    }
    /* double (the off-diagonal sum is < 2^511), then add the squares at positions 2i */
    t[15] = t[14] >> 31;
#pragma unroll
    for (int k = 14; k > 0; k--) t[k] = (t[k] << 1) | (t[k - 1] >> 31);
    t[0] <<= 1;
    c = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        const ulong q = (ulong)a->v[i] * a->v[i];
        t[2 * i] = PZ_ADDC(t[2 * i], (uint)q, c, &c);
        t[2 * i + 1] = PZ_ADDC(t[2 * i + 1], (uint)(q >> 32), c, &c);
    }
    fe_reduce512(r, t);
}
#else
/* without add-with-carry builtins: 64-bit C, which such compilers turn into better code than the carry chains above */
/* r += c * (2^32 + 977), keeping r < 2^256 */
inline void fe_fold(fe *r, uint c)
{
    ulong acc = (ulong)r->v[0] + (ulong)c * 0x3D1u;
    r->v[0] = (uint)acc;
    ulong carry = acc >> 32;
    acc = (ulong)r->v[1] + (ulong)c + carry;
    r->v[1] = (uint)acc;
    carry = acc >> 32;
#pragma unroll
    for (int i = 2; i < 8; i++) { acc = (ulong)r->v[i] + carry; r->v[i] = (uint)acc; carry = acc >> 32; }
    if (carry) {
        acc = (ulong)r->v[0] + 0x3D1u;
        r->v[0] = (uint)acc;
        carry = acc >> 32;
        acc = (ulong)r->v[1] + 1u + carry;
        r->v[1] = (uint)acc;
        carry = acc >> 32;
#pragma unroll
        for (int i = 2; i < 8; i++) { acc = (ulong)r->v[i] + carry; r->v[i] = (uint)acc; carry = acc >> 32; }
    }
}

/* r = t mod p for a 512-bit t (16 limbs), result < 2^256 */
inline void fe_reduce512(fe *r, const uint t[16])
{
    /* low + high*(2^32 + 977) */
    fe o;
    ulong carry = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        ulong acc = (ulong)t[8 + i] * 977u + (ulong)t[i] + carry + (i > 0 ? (ulong)t[8 + i - 1] : 0UL);
        o.v[i] = (uint)acc;
        carry = acc >> 32;
    }
    ulong top = carry + (ulong)t[15];     /* < 2^33 */
    ulong add0 = top * 977u;              /* limbs 0..1 */
    ulong acc = (ulong)o.v[0] + (add0 & 0xffffffffUL);
    o.v[0] = (uint)acc;
    carry = acc >> 32;
    acc = (ulong)o.v[1] + (add0 >> 32) + (top & 0xffffffffUL) + carry;
    o.v[1] = (uint)acc;
    carry = acc >> 32;
    acc = (ulong)o.v[2] + (top >> 32) + carry;
    o.v[2] = (uint)acc;
    carry = acc >> 32;
#pragma unroll
    for (int i = 3; i < 8; i++) { acc = (ulong)o.v[i] + carry; o.v[i] = (uint)acc; carry = acc >> 32; }
    if (carry) fe_fold(&o, 1u);
    *r = o;
}

inline void fe_mul(fe *r, const fe *a, const fe *b)
{
    uint t[16];
#pragma unroll
    for (int i = 0; i < 16; i++) t[i] = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        ulong carry = 0;
        ulong ai = a->v[i];
#pragma unroll
        for (int j = 0; j < 8; j++) {
            ulong p = ai * (ulong)b->v[j] + (ulong)t[i + j] + carry;
            t[i + j] = (uint)p;
            carry = p >> 32;
        }
        t[i + 8] = (uint)carry;
    }
    fe_reduce512(r, t);
}

/* dedicated square: the 28 products a_i*a_j (i < j) once, doubled, plus the 8 squares (36 products instead of 64) */
inline void fe_sqr(fe *r, const fe *a)
{
    uint t[16];
#pragma unroll
    for (int i = 0; i < 16; i++) t[i] = 0;
#pragma unroll
    for (int i = 0; i < 7; i++) {
        ulong carry = 0;
        ulong ai = a->v[i];
#pragma unroll
        for (int j = i + 1; j < 8; j++) {
            ulong p = ai * (ulong)a->v[j] + (ulong)t[i + j] + carry;
            t[i + j] = (uint)p;
            carry = p >> 32;
        }
        t[i + 8] = (uint)carry;
    }
    /* double (the off-diagonal sum is < 2^511) */
    t[15] = t[14] >> 31;
#pragma unroll
    for (int k = 14; k > 0; k--) t[k] = (t[k] << 1) | (t[k - 1] >> 31);
    t[0] <<= 1;
    ulong carry = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) {
        ulong p = (ulong)a->v[i] * (ulong)a->v[i];
        ulong s = (ulong)t[2 * i] + (p & 0xffffffffUL) + carry;
        t[2 * i] = (uint)s;
        s = (ulong)t[2 * i + 1] + (p >> 32) + (s >> 32);
        t[2 * i + 1] = (uint)s;
        carry = s >> 32;
    }
    fe_reduce512(r, t);
}

#endif

inline void fe_sqr_n(fe *r, const fe *a, int n)
{
    fe t = *a;
    for (int i = 0; i < n; i++) fe_sqr(&t, &t);
    *r = t;
}

/* canonical reduction */
#ifdef PZ_FAST_FE
/* v >= p exactly when v + 2^32 + 977 carries out of 2^256, and then the wrapped sum is v - p: one carry chain and eight
 * selects (17 instructions; the same in 64-bit C compiles to about 60) */
inline void fe_normalize(fe *r)
{
    uint t0, t1, t2, t3, t4, t5, t6, t7, c;
    ADD_CC(t0, r->v[0], 0x3D1u); ADDC_CC(t1, r->v[1], 1u); ADDC_CC(t2, r->v[2], 0u); ADDC_CC(t3, r->v[3], 0u);
    ADDC_CC(t4, r->v[4], 0u); ADDC_CC(t5, r->v[5], 0u); ADDC_CC(t6, r->v[6], 0u); ADDC_CC(t7, r->v[7], 0u);
    ADDC(c, 0u, 0u);
    r->v[0] = c ? t0 : r->v[0]; r->v[1] = c ? t1 : r->v[1]; r->v[2] = c ? t2 : r->v[2]; r->v[3] = c ? t3 : r->v[3];
    r->v[4] = c ? t4 : r->v[4]; r->v[5] = c ? t5 : r->v[5]; r->v[6] = c ? t6 : r->v[6]; r->v[7] = c ? t7 : r->v[7];
}
#else
inline void fe_normalize(fe *r)
{
    uint t[8], c = 0;
    t[0] = PZ_ADDC(r->v[0], 0x3D1u, 0u, &c);
    t[1] = PZ_ADDC(r->v[1], 1u, c, &c);
#pragma unroll
    for (int i = 2; i < 8; i++) t[i] = PZ_ADDC(r->v[i], 0u, c, &c);
#pragma unroll
    for (int i = 0; i < 8; i++) r->v[i] = c ? t[i] : r->v[i];
}
#endif

/* r = c ? p - r : r for a normalized, non-zero r (a branch-free select: with c random per lane the warp would run the
 * subtraction anyway) */
#ifdef PZ_FAST_FE
/* p - r = ~r - (2^32 + 976) for 0 < r < p: with the mask m (all ones when negating) the value is r ^ m, minus m & (2^32 + 976),
 * in place on one borrow chain */
inline void fe_cneg_norm(fe *r, uint c)
{
    const uint m = 0u - c;
    uint x0 = r->v[0] ^ m, x1 = r->v[1] ^ m, x2 = r->v[2] ^ m, x3 = r->v[3] ^ m, x4 = r->v[4] ^ m, x5 = r->v[5] ^ m, x6 = r->v[6] ^ m, x7 = r->v[7] ^ m;
    SUB_CC(x0, x0, m & 0x3D0u); SUBC_CC(x1, x1, m & 1u); SUBC_CC(x2, x2, 0u); SUBC_CC(x3, x3, 0u);
    SUBC_CC(x4, x4, 0u); SUBC_CC(x5, x5, 0u); SUBC_CC(x6, x6, 0u); SUBC(x7, x7, 0u);
    r->v[0] = x0; r->v[1] = x1; r->v[2] = x2; r->v[3] = x3; r->v[4] = x4; r->v[5] = x5; r->v[6] = x6; r->v[7] = x7;
}
#else
inline void fe_cneg_norm(fe *r, uint c)
{
    const uint m = 0u - c;
    uint k = 0;
    r->v[0] = PZ_SUBC(r->v[0] ^ m, m & 0x3D0u, 0u, &k);
    r->v[1] = PZ_SUBC(r->v[1] ^ m, m & 1u, k, &k);
#pragma unroll
    for (int i = 2; i < 8; i++) r->v[i] = PZ_SUBC(r->v[i] ^ m, 0u, k, &k);
}
#endif

inline bool fe_is_zero(const fe *a)   /* a normalized */
{
    uint o = 0;
#pragma unroll
    for (int i = 0; i < 8; i++) o |= a->v[i];
    return o == 0;
}

/* a^(p-2): 255 squarings and 15 products (~64000 instructions). Kept as the reference for fe_inv. */
PZ_NOINLINE void fe_inv_pow(fe *r, const fe *a)
{
    fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    fe_sqr(&x2, a);             fe_mul(&x2, &x2, a);
    fe_sqr(&x3, &x2);           fe_mul(&x3, &x3, a);
    fe_sqr_n(&x6, &x3, 3);      fe_mul(&x6, &x6, &x3);
    fe_sqr_n(&x9, &x6, 3);      fe_mul(&x9, &x9, &x3);
    fe_sqr_n(&x11, &x9, 2);     fe_mul(&x11, &x11, &x2);
    fe_sqr_n(&x22, &x11, 11);   fe_mul(&x22, &x22, &x11);
    fe_sqr_n(&x44, &x22, 22);   fe_mul(&x44, &x44, &x22);
    fe_sqr_n(&x88, &x44, 44);   fe_mul(&x88, &x88, &x44);
    fe_sqr_n(&x176, &x88, 88);  fe_mul(&x176, &x176, &x88);
    fe_sqr_n(&x220, &x176, 44); fe_mul(&x220, &x220, &x44);
    fe_sqr_n(&x223, &x220, 3);  fe_mul(&x223, &x223, &x3);
    fe_sqr_n(&t, &x223, 23);    fe_mul(&t, &t, &x22);
    fe_sqr_n(&t, &t, 5);        fe_mul(&t, &t, a);
    fe_sqr_n(&t, &t, 3);        fe_mul(&t, &t, &x2);
    fe_sqr_n(&t, &t, 2);        fe_mul(r, &t, a);
}

/* ---- inversion by divsteps: Bernstein-Yang "safegcd" (https://gcd.cr.yp.to), 30 steps per round, variable time ----
 *
 * (delta, f, g) -> (1 - delta, g, (g - f)/2) if delta > 0 and g odd; (1 + delta, f, (g + f)/2) if g odd; (1 + delta, f,
 * g/2) otherwise, started with delta = 1, f = p (odd), g = x. 30 steps depend only on the low 30 bits of f and g and give
 * a matrix t = (u v; q r) with (f', g') = t (f, g) / 2^30, |u| + |v|, |q| + |r| <= 2^30. The coefficients d, e with
 * f = d x and g = e x (mod p) follow the same matrix, divided by 2^30 modulo p (a multiple of p is added to make the
 * division exact). After ~18 rounds g = 0 and f = +-1, so x^-1 = +-d. Numbers are 9 limbs of 30 bits, signed on top.
 * Same method as secp256k1's modinv32_var. On sm_86 a warp executes about 14600 instructions for an inversion (19 rounds for the
 * slowest of its 32 inputs: 760 each, 340 of them for the two matrix products), against 22000 with the divsteps one at a time
 * (the steps of a round one at a time) and 64000 for the power a^(p-2). */
#define SG_M30 0x3FFFFFFF
typedef struct { int v[9]; } sg9;
typedef struct { int u, v, q, r; } sg_t;
/* p in limbs of 30 bits, and p^-1 mod 2^30 */
PZ_CONST_TABLE int SG_P[9] = {1073740847, 1073741819, 1073741823, 1073741823, 1073741823, 1073741823, 1073741823, 1073741823, 65535};
#define SG_PINV 769313487u

/* 30 divsteps in variable time, as libsecp256k1's modinv32_var does them (eta = -delta): a run of zero bits of g is done in one go
 * (g /= 2^zeros and the row of f doubled as many times), and when f and g swap roles the bottom bits of g are cancelled by adding
 * w f, which counts as that many divsteps on an odd g; w is taken mod 8 at most, where f^-1 = f, so no table is needed. The matrix
 * is that of the 30 divsteps one by one. With random inputs a batch takes 9.8 turns of the loop on average and 12.6 for the slowest
 * of 32 work-items (a warp waits for its slowest), against 30 for the steps one at a time; each turn costs about as much as one
 * divstep did. */
inline int sg_divsteps(int eta, uint f, uint g, sg_t *t)
{
    uint u = 1, v = 0, q = 0, r = 1;
    int i = 30;
    for (;;) {
        const int zeros = PZ_CTZ(g | (0xFFFFFFFFu << i));   /* the bit at i stops the count after the steps that are left */
        g >>= zeros;
        u <<= zeros;
        v <<= zeros;
        eta -= zeros;
        i -= zeros;
        if (i == 0) break;
        if (eta < 0) {                                     /* (f, g) -> (g, -f) */
            uint x;
            eta = -eta;
            x = f; f = g; g = 0u - x;
            x = u; u = q; q = 0u - x;
            x = v; v = r; r = 0u - x;
        }
        /* cancel the bottom min(eta + 1, i, 3) bits of g: no more than i steps are left, and after eta + 1 the swap is due */
        const int limit = eta + 1 > i ? i : eta + 1;
        const uint m = (0xFFFFFFFFu >> (32 - limit)) & 7u;
        const uint w = (g * (0u - f)) & m;
        g += f * w;
        q += u * w;
        r += v * w;
    }
    t->u = (int)u; t->v = (int)v; t->q = (int)q; t->r = (int)r;
    return eta;
}

inline void sg_update_fg(sg9 *f, sg9 *g, const sg_t *t)
{
    const int u = t->u, v = t->v, q = t->q, r = t->r;
    s64 cf = (s64)u * f->v[0] + (s64)v * g->v[0];
    s64 cg = (s64)q * f->v[0] + (s64)r * g->v[0];
    cf >>= 30;                                       /* the low 30 bits are zero */
    cg >>= 30;
#pragma unroll
    for (int i = 1; i < 9; i++) {
        cf += (s64)u * f->v[i] + (s64)v * g->v[i];
        cg += (s64)q * f->v[i] + (s64)r * g->v[i];
        f->v[i - 1] = (int)cf & SG_M30; cf >>= 30;
        g->v[i - 1] = (int)cg & SG_M30; cg >>= 30;
    }
    f->v[8] = (int)cf;
    g->v[8] = (int)cg;
}

inline void sg_update_de(sg9 *d, sg9 *e, const sg_t *t)
{
    const int u = t->u, v = t->v, q = t->q, r = t->r;
    const int sd = d->v[8] >> 31, se = e->v[8] >> 31;     /* negative numbers get p added first, to stay in (-2p, p) */
    int md = (u & sd) + (v & se);
    int me = (q & sd) + (r & se);
    s64 cd = (s64)u * d->v[0] + (s64)v * e->v[0];
    s64 ce = (s64)q * d->v[0] + (s64)r * e->v[0];
    md -= (int)((SG_PINV * (uint)cd + (uint)md) & SG_M30);   /* t (d, e) + p (md, me) is a multiple of 2^30 */
    me -= (int)((SG_PINV * (uint)ce + (uint)me) & SG_M30);
    cd += (s64)SG_P[0] * md;
    ce += (s64)SG_P[0] * me;
    cd >>= 30;
    ce >>= 30;
#pragma unroll
    for (int i = 1; i < 9; i++) {
        cd += (s64)u * d->v[i] + (s64)v * e->v[i] + (s64)SG_P[i] * md;
        ce += (s64)q * d->v[i] + (s64)r * e->v[i] + (s64)SG_P[i] * me;
        d->v[i - 1] = (int)cd & SG_M30; cd >>= 30;
        e->v[i - 1] = (int)ce & SG_M30; ce >>= 30;
    }
    d->v[8] = (int)cd;
    e->v[8] = (int)ce;
}

inline void sg_carry(sg9 *r)
{
#pragma unroll
    for (int i = 0; i < 8; i++) { r->v[i + 1] += r->v[i] >> 30; r->v[i] &= SG_M30; }
}

PZ_NOINLINE void fe_inv(fe *r, const fe *a)
{
    fe n = *a;
    fe_normalize(&n);
    sg9 f, g, d, e;
    for (int i = 0; i < 9; i++) { f.v[i] = SG_P[i]; d.v[i] = 0; e.v[i] = 0; }
    e.v[0] = 1;
    /* x in limbs of 30 bits */
#pragma unroll
    for (int i = 0; i < 9; i++) {
        const int bit = 30 * i, w = bit >> 5, sh = bit & 31;
        const ulong t = (ulong)n.v[w] | (w + 1 < 8 ? (ulong)n.v[w + 1 < 8 ? w + 1 : 7] << 32 : 0UL);
        g.v[i] = (int)((t >> sh) & SG_M30);
    }
    int eta = -1;
    for (int it = 0; it < 25; it++) {
        sg_t t;
        eta = sg_divsteps(eta, (uint)f.v[0], (uint)g.v[0], &t);
        sg_update_de(&d, &e, &t);
        sg_update_fg(&f, &g, &t);
        int z = 0;
#pragma unroll
        for (int i = 0; i < 9; i++) z |= g.v[i];
        if (z == 0) break;
    }
    /* f = +-1 (or p, when x = 0, and then d = 0): x^-1 = f d, with d in (-2p, p) brought into [0, p) */
    sg_carry(&d);
    for (int rep = 0; rep < 2; rep++) {
        const int m = d.v[8] >> 31;
#pragma unroll
        for (int i = 0; i < 9; i++) d.v[i] += SG_P[i] & m;
        sg_carry(&d);
    }
    if (f.v[8] < 0) {
#pragma unroll
        for (int i = 0; i < 9; i++) d.v[i] = SG_P[i] - d.v[i];
        sg_carry(&d);
    }
    ulong buf = 0;
    int bits = 0, wi = 0;
#pragma unroll
    for (int i = 0; i < 9; i++) {
        buf |= (ulong)(uint)d.v[i] << bits;
        bits += 30;
        if (bits >= 32) { r->v[wi < 7 ? wi : 7] = (uint)buf; wi++; buf >>= 32; bits -= 32; }
    }
}

/* ---------------- loads from memory spaces ---------------- */

inline void fe_load_g(fe *r, __global const uint *p) { for (int i = 0; i < 8; i++) r->v[i] = p[i]; }
inline void fe_load_c(fe *r, __constant const uint *p) { for (int i = 0; i < 8; i++) r->v[i] = p[i]; }
/* the same from a 16-byte aligned address, as two 16-byte loads: a quarter of the load instructions, and where the work-items of a warp
 * read different entries (the jump table of the kangaroo) a quarter of the cache lines served */
inline void fe_load_g16(fe *r, __global const uint *p)
{
    const uint4 a = *(__global const uint4 *)p, b = *(__global const uint4 *)(p + 4);
    r->v[0] = a.x; r->v[1] = a.y; r->v[2] = a.z; r->v[3] = a.w; r->v[4] = b.x; r->v[5] = b.y; r->v[6] = b.z; r->v[7] = b.w;
}
inline void fe_load_c16(fe *r, __constant const uint *p)
{
    const uint4 a = *(__constant const uint4 *)p, b = *(__constant const uint4 *)(p + 4);
    r->v[0] = a.x; r->v[1] = a.y; r->v[2] = a.z; r->v[3] = a.w; r->v[4] = b.x; r->v[5] = b.y; r->v[6] = b.z; r->v[7] = b.w;
}
inline void fe_store_g(__global uint *p, const fe *r) { for (int i = 0; i < 8; i++) p[i] = r->v[i]; }

/* ---------------- Jacobian points (for start-point computation) ---------------- */

typedef struct { fe x, y, z; int inf; } gej;

inline void gej_dbl(gej *r, const gej *a)
{
    if (a->inf) { *r = *a; return; }
    fe A, B, C, D, E, F, t, nx, ny, nz;
    fe_sqr(&A, &a->x);
    fe_sqr(&B, &a->y);
    fe_sqr(&C, &B);
    fe_add(&t, &a->x, &B);
    fe_sqr(&D, &t);
    fe_sub(&D, &D, &A);
    fe_sub(&D, &D, &C);
    fe_add(&D, &D, &D);
    fe_add(&E, &A, &A);
    fe_add(&E, &E, &A);
    fe_sqr(&F, &E);
    fe_mul(&nz, &a->y, &a->z);
    fe_add(&nz, &nz, &nz);
    fe_add(&t, &D, &D);
    fe_sub(&nx, &F, &t);
    fe_sub(&t, &D, &nx);
    fe_mul(&ny, &E, &t);
    fe_add(&C, &C, &C);
    fe_add(&C, &C, &C);
    fe_add(&C, &C, &C);
    fe_sub(&ny, &ny, &C);
    r->x = nx; r->y = ny; r->z = nz; r->inf = 0;
}

/* a + b, b affine; handles a == b (doubling) and a == -b */
inline void gej_add_ge(gej *r, const gej *a, const ge *b)
{
    if (a->inf) { r->x = b->x; r->y = b->y; fe_set_u32(&r->z, 1); r->inf = 0; return; }
    fe z1z1, u2, s2, h, hh, i, j, rr, v, t, nx, ny, nz;
    fe_sqr(&z1z1, &a->z);
    fe_mul(&u2, &b->x, &z1z1);
    fe_mul(&s2, &b->y, &a->z);
    fe_mul(&s2, &s2, &z1z1);
    fe_sub(&h, &u2, &a->x);
    fe_sub(&rr, &s2, &a->y);
    fe hn = h, rn = rr;
    fe_normalize(&hn);
    if (fe_is_zero(&hn)) {
        fe_normalize(&rn);
        if (fe_is_zero(&rn)) { gej_dbl(r, a); return; }
        r->inf = 1;
        return;
    }
    fe_sqr(&hh, &h);
    fe_add(&i, &hh, &hh);
    fe_add(&i, &i, &i);
    fe_mul(&j, &h, &i);
    fe_add(&rr, &rr, &rr);
    fe_mul(&v, &a->x, &i);
    fe_sqr(&nx, &rr);
    fe_sub(&nx, &nx, &j);
    fe_sub(&nx, &nx, &v);
    fe_sub(&nx, &nx, &v);
    fe_sub(&t, &v, &nx);
    fe_mul(&ny, &rr, &t);
    fe_mul(&t, &a->y, &j);
    fe_add(&t, &t, &t);
    fe_sub(&ny, &ny, &t);
    fe_add(&t, &a->z, &h);
    fe_sqr(&nz, &t);
    fe_sub(&nz, &nz, &z1z1);
    fe_sub(&nz, &nz, &hh);
    r->x = nx; r->y = ny; r->z = nz; r->inf = 0;
}

/* r = a + t*D where D is affine and t < 2^32; result affine. Returns 0 if infinity. */
PZ_NOINLINE int ge_add_mul_small(ge *r, const ge *a, const ge *D, uint t)
{
    gej acc;
    acc.inf = 1;
    fe_set_u32(&acc.x, 0); fe_set_u32(&acc.y, 0); fe_set_u32(&acc.z, 0);
    for (int bit = 31; bit >= 0; bit--) {
        gej_dbl(&acc, &acc);
        if ((t >> bit) & 1u) gej_add_ge(&acc, &acc, D);
    }
    gej_add_ge(&acc, &acc, a);
    if (acc.inf) return 0;
    fe zi, zi2, zi3;
    fe_inv(&zi, &acc.z);
    fe_sqr(&zi2, &zi);
    fe_mul(&zi3, &zi2, &zi);
    fe_mul(&r->x, &acc.x, &zi2);
    fe_mul(&r->y, &acc.y, &zi3);
    fe_normalize(&r->x);
    fe_normalize(&r->y);
    return 1;
}

/* ---------------- hash160 of a compressed key ---------------- */

#define ROTR(x, n) rotate((uint)(x), (uint)(32 - (n)))
#define ROTL(x, n) rotate((uint)(x), (uint)(n))

/* digest words (big-endian SHA-256 state) of the 33-byte message pfx||x ; xw = big-endian words of x */
inline void sha256_33(uint pfx, const uint xw[8], uint out[8])
{
    uint w[16];
    w[0] = (pfx << 24) | (xw[0] >> 8);
#pragma unroll
    for (int i = 1; i < 8; i++) w[i] = (xw[i - 1] << 24) | (xw[i] >> 8);
    w[8] = (xw[7] << 24) | 0x00800000u;
#pragma unroll
    for (int i = 9; i < 15; i++) w[i] = 0;
    w[15] = 264;
    uint a = SHA_H0[0], b = SHA_H0[1], c = SHA_H0[2], d = SHA_H0[3], e = SHA_H0[4], f = SHA_H0[5], g = SHA_H0[6], h = SHA_H0[7];
#pragma unroll
    for (int i = 0; i < 64; i++) {
        uint wi;
        if (i < 16) wi = w[i];
        else {
            uint w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            uint s0 = ROTR(w15, 7) ^ ROTR(w15, 18) ^ (w15 >> 3);
            uint s1 = ROTR(w2, 17) ^ ROTR(w2, 19) ^ (w2 >> 10);
            wi = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
            w[i & 15] = wi;
        }
        uint S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint ch = (e & f) ^ (~e & g);
        uint t1 = h + S1 + ch + SHA_K[i] + wi;
        uint S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint mj = (a & b) ^ (a & c) ^ (b & c);
        uint t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    out[0] = a + SHA_H0[0]; out[1] = b + SHA_H0[1]; out[2] = c + SHA_H0[2]; out[3] = d + SHA_H0[3];
    out[4] = e + SHA_H0[4]; out[5] = f + SHA_H0[5]; out[6] = g + SHA_H0[6]; out[7] = h + SHA_H0[7];
}

inline uint rmd_f(int rd, uint x, uint y, uint z)
{
    switch (rd) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

inline uint bswap32(uint x) { return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24); }

/* RIPEMD-160 of the 32-byte SHA-256 digest; out = five little-endian words */
inline void ripemd160_32(const uint dig[8], uint out[5])
{
    uint m[16];
#pragma unroll
    for (int i = 0; i < 8; i++) m[i] = bswap32(dig[i]);
    m[8] = 0x80;
#pragma unroll
    for (int i = 9; i < 14; i++) m[i] = 0;
    m[14] = 256;
    m[15] = 0;
    uint a = 0x67452301u, b = 0xEFCDAB89u, c = 0x98BADCFEu, d = 0x10325476u, e = 0xC3D2E1F0u;
    uint a2 = a, b2 = b, c2 = c, d2 = d, e2 = e;
#pragma unroll
    for (int j = 0; j < 80; j++) {
        const int rd = j / 16;
        uint t = ROTL(a + rmd_f(rd, b, c, d) + m[RMD_RL[j]] + RMD_KL[rd], RMD_SL[j]) + e;
        a = e; e = d; d = ROTL(c, 10); c = b; b = t;
        t = ROTL(a2 + rmd_f(4 - rd, b2, c2, d2) + m[RMD_RR[j]] + RMD_KR[rd], RMD_SR[j]) + e2;
        a2 = e2; e2 = d2; d2 = ROTL(c2, 10); c2 = b2; b2 = t;
    }
    out[0] = 0xEFCDAB89u + c + d2;
    out[1] = 0x98BADCFEu + d + e2;
    out[2] = 0x10325476u + e + a2;
    out[3] = 0xC3D2E1F0u + a + b2;
    out[4] = 0x67452301u + b + c2;
}

/* Word 2 of the hash160, the filter of the linear search (the few keys that pass are hashed again in full): h3 + e + a2 is
 * rol(t76, 10) + rol(t'75, 10), where t_j (t'_j) is the word made by step j of the left (right) line, so it does not depend on
 * the last three steps of the left RIPEMD-160 line nor on the last four of the right one, which the compiler drops. */
PZ_NOINLINE uint hash160_w2(const fe *x, uint parity)
{
    uint xw[8], dig[8], h[5];
#pragma unroll
    for (int i = 0; i < 8; i++) xw[i] = x->v[7 - i];
    sha256_33(2u + parity, xw, dig);
    ripemd160_32(dig, h);
    return h[2];
}

/* hash160 words of the compressed key with x (normalized fe) and y parity */
PZ_NOINLINE void hash160_words(const fe *x, uint parity, uint out[5])
{
    uint xw[8], dig[8];
#pragma unroll
    for (int i = 0; i < 8; i++) xw[i] = x->v[7 - i];
    sha256_33(2u + parity, xw, dig);
    ripemd160_32(dig, out);
}
