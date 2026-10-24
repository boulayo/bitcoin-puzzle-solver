/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash_neon.inc - hash160 of LANES compressed public keys on aarch64, included from hash_simd.inc (PZ_HASH_EXTERNAL).
 * SHA-256 uses the ARMv8 SHA2 instructions (sha256h, sha256h2, sha256su0, sha256su1), four keys at a time in lock step. RIPEMD-160
 * runs on NEON vectors of four lanes; NEON_NV such vectors (LANES = 4 * NEON_NV) are advanced together inside every step, which gives
 * the out-of-order core NEON_NV * 2 independent dependency chains (two RIPEMD lines per vector). Rotations are a shift left plus a
 * shift-right-insert. Layout as in hash_simd.inc: X[8][LANES] and pfx[LANES] in, out[5][LANES] (or out0[LANES]) out. */

#include <arm_neon.h>

#ifndef NEON_NV
#define NEON_NV (LANES / 4)
#endif
#define NV NEON_NV

/* 4x4 transpose of 32-bit words: r[i][j] = a[j][i] */
static inline void neon_transpose4(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t d, uint32x4_t r[4])
{
    const uint32x4x2_t t01 = vtrnq_u32(a, b), t23 = vtrnq_u32(c, d);
    r[0] = vcombine_u32(vget_low_u32(t01.val[0]), vget_low_u32(t23.val[0]));
    r[1] = vcombine_u32(vget_low_u32(t01.val[1]), vget_low_u32(t23.val[1]));
    r[2] = vcombine_u32(vget_high_u32(t01.val[0]), vget_high_u32(t23.val[0]));
    r[3] = vcombine_u32(vget_high_u32(t01.val[1]), vget_high_u32(t23.val[1]));
}

/* SHA-256 of the 33-byte public keys of one group of four lanes (lanes 4g..4g+3): dig[w] = digest word w of the four keys. */
static inline void neon_sha256_group(const u32 *X, const u32 *pfx, int g, uint32x4_t dig[8])
{
    /* message words 0..8 of the four keys, one vector per word */
    uint32x4_t x[8], wv[9];
    const uint32x4_t p = vld1q_u32(pfx + 4 * g);
    for (int i = 0; i < 8; i++) x[i] = vld1q_u32(X + i * LANES + 4 * g);
    wv[0] = vorrq_u32(vshlq_n_u32(p, 24), vshrq_n_u32(x[0], 8));
    for (int i = 1; i < 8; i++) wv[i] = vorrq_u32(vshlq_n_u32(x[i - 1], 24), vshrq_n_u32(x[i], 8));
    wv[8] = vorrq_u32(vshlq_n_u32(x[7], 24), vdupq_n_u32(0x00800000u));

    /* per key: m[k][0..3] = the sixteen words of its block (words 9..14 are zero, word 15 is the length 264 bits) */
    uint32x4_t m[4][4], t[4];
    const uint32x4_t zero = vdupq_n_u32(0);
    neon_transpose4(wv[0], wv[1], wv[2], wv[3], t);
    for (int k = 0; k < 4; k++) m[k][0] = t[k];
    neon_transpose4(wv[4], wv[5], wv[6], wv[7], t);
    for (int k = 0; k < 4; k++) m[k][1] = t[k];
    neon_transpose4(wv[8], zero, zero, zero, t);
    for (int k = 0; k < 4; k++) m[k][2] = t[k];
    const uint32x4_t last = vsetq_lane_u32(264, zero, 3);
    for (int k = 0; k < 4; k++) m[k][3] = last;

    uint32x4_t s0[4], s1[4];
    for (int k = 0; k < 4; k++) { s0[k] = vld1q_u32(SHA256_H0); s1[k] = vld1q_u32(SHA256_H0 + 4); }
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++) {
#pragma GCC unroll 4
        for (int k = 0; k < 4; k++) {
            uint32x4_t w = m[k][i & 3];
            if (i >= 4) {
                w = vsha256su1q_u32(vsha256su0q_u32(w, m[k][(i + 1) & 3]), m[k][(i + 2) & 3], m[k][(i + 3) & 3]);
                m[k][i & 3] = w;
            }
            const uint32x4_t wk = vaddq_u32(w, vld1q_u32(SHA256_K + 4 * i));
            const uint32x4_t prev = s0[k];
            s0[k] = vsha256hq_u32(s0[k], s1[k], wk);
            s1[k] = vsha256h2q_u32(s1[k], prev, wk);
        }
    }
    const uint32x4_t h0 = vld1q_u32(SHA256_H0), h1 = vld1q_u32(SHA256_H0 + 4);
    for (int k = 0; k < 4; k++) { s0[k] = vaddq_u32(s0[k], h0); s1[k] = vaddq_u32(s1[k], h1); }
    neon_transpose4(s0[0], s0[1], s0[2], s0[3], dig);
    neon_transpose4(s1[0], s1[1], s1[2], s1[3], dig + 4);
}

/* Rotation left by a literal count: the counts are literals in each case so that the intrinsics get their immediates, and after the
 * unrolling of the 80 steps the switch folds. */
#define NEON_ROTL_CASE(n) case n: return vsriq_n_u32(vshlq_n_u32(x, n), x, 32 - (n))
static inline uint32x4_t neon_rotl(uint32x4_t x, int n)
{
    switch (n) {
    NEON_ROTL_CASE(5);  NEON_ROTL_CASE(6);  NEON_ROTL_CASE(7);  NEON_ROTL_CASE(8);  NEON_ROTL_CASE(9);  NEON_ROTL_CASE(10);
    NEON_ROTL_CASE(11); NEON_ROTL_CASE(12); NEON_ROTL_CASE(13); NEON_ROTL_CASE(14); NEON_ROTL_CASE(15);
    default: return vorrq_u32(vshlq_u32(x, vdupq_n_s32(n)), vshlq_u32(x, vdupq_n_s32(n - 32)));
    }
}

static inline uint32x4_t neon_rmd_f(int rd, uint32x4_t x, uint32x4_t y, uint32x4_t z)
{
    switch (rd) {
    case 0: return veorq_u32(veorq_u32(x, y), z);
    case 1: return vbslq_u32(x, y, z);                       /* (x & y) | (~x & z) */
    case 2: return veorq_u32(vornq_u32(x, y), z);            /* (x | ~y) ^ z */
    case 3: return vbslq_u32(z, x, y);                       /* (x & z) | (y & ~z) */
    default: return veorq_u32(x, vornq_u32(y, z));           /* x ^ (y | ~z) */
    }
}

/* RIPEMD-160 of the 32-byte digests dig[w][v] (word w of the keys of vector v): out[i][v] = little-endian result word i. */
static inline void neon_ripemd160(const uint32x4_t dig[8][NV], uint32x4_t out[5][NV])
{
    uint32x4_t m[16][NV];
    for (int v = 0; v < NV; v++) {
        for (int i = 0; i < 8; i++) m[i][v] = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(dig[i][v])));
        m[8][v] = vdupq_n_u32(0x80);
        for (int i = 9; i < 14; i++) m[i][v] = vdupq_n_u32(0);
        m[14][v] = vdupq_n_u32(256);
        m[15][v] = vdupq_n_u32(0);
    }
    const u32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint32x4_t a[NV], b[NV], c[NV], d[NV], e[NV], a2[NV], b2[NV], c2[NV], d2[NV], e2[NV];
    for (int v = 0; v < NV; v++) {
        a[v] = a2[v] = vdupq_n_u32(h0); b[v] = b2[v] = vdupq_n_u32(h1); c[v] = c2[v] = vdupq_n_u32(h2);
        d[v] = d2[v] = vdupq_n_u32(h3); e[v] = e2[v] = vdupq_n_u32(h4);
    }
#pragma GCC unroll 80
    for (int j = 0; j < 80; j++) {
        const int rd = j / 16;
#pragma GCC unroll 4
        for (int v = 0; v < NV; v++) {
            uint32x4_t t = vaddq_u32(vaddq_u32(a[v], neon_rmd_f(rd, b[v], c[v], d[v])), vaddq_u32(m[RMD_RL[j]][v], vdupq_n_u32(RMD_KL[rd])));
            t = vaddq_u32(neon_rotl(t, RMD_SL[j]), e[v]);
            a[v] = e[v]; e[v] = d[v]; d[v] = neon_rotl(c[v], 10); c[v] = b[v]; b[v] = t;
            t = vaddq_u32(vaddq_u32(a2[v], neon_rmd_f(4 - rd, b2[v], c2[v], d2[v])), vaddq_u32(m[RMD_RR[j]][v], vdupq_n_u32(RMD_KR[rd])));
            t = vaddq_u32(neon_rotl(t, RMD_SR[j]), e2[v]);
            a2[v] = e2[v]; e2[v] = d2[v]; d2[v] = neon_rotl(c2[v], 10); c2[v] = b2[v]; b2[v] = t;
        }
    }
    for (int v = 0; v < NV; v++) {
        out[0][v] = vaddq_u32(vaddq_u32(vdupq_n_u32(h1), c[v]), d2[v]);
        out[1][v] = vaddq_u32(vaddq_u32(vdupq_n_u32(h2), d[v]), e2[v]);
        out[2][v] = vaddq_u32(vaddq_u32(vdupq_n_u32(h3), e[v]), a2[v]);
        out[3][v] = vaddq_u32(vaddq_u32(vdupq_n_u32(h4), a[v]), b2[v]);
        out[4][v] = vaddq_u32(vaddq_u32(vdupq_n_u32(h0), b[v]), c2[v]);
    }
}

static inline void neon_hash160(const u32 *X, const u32 *pfx, uint32x4_t out[5][NV])
{
    uint32x4_t dig[8][NV], g[8];
    for (int v = 0; v < NV; v++) {
        neon_sha256_group(X, pfx, v, g);
        for (int i = 0; i < 8; i++) dig[i][v] = g[i];
    }
    neon_ripemd160(dig, out);
}

/* Hashes LANES compressed public keys. out is [5][LANES]. */
static void HASH_LANES_FN(const u32 *X, const u32 *pfx, u32 *out)
{
    uint32x4_t r[5][NV];
    neon_hash160(X, pfx, r);
    for (int i = 0; i < 5; i++)
        for (int v = 0; v < NV; v++) vst1q_u32(out + i * LANES + 4 * v, r[i][v]);
}

/* Only the first word of each hash160 (the search filter; matches are re-checked with the scalar hash). The words that do not
 * reach out[0] are dead code after the inlining and the unrolling. */
#define HASH_W0_FN CAT(HASH_LANES_FN, _w0)
static void HASH_W0_FN(const u32 *X, const u32 *pfx, u32 *out0)
{
    uint32x4_t r[5][NV];
    neon_hash160(X, pfx, r);
    for (int v = 0; v < NV; v++) vst1q_u32(out0 + 4 * v, r[0][v]);
}
