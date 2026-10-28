/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash_lanes_shani.inc - SHA-256 stage of the hash160 of 8 compressed public keys on x86-64 with the SHA extensions, included from
 * hash_lanes.inc (PZ_HASH_SHA256_EXTERNAL) with PZ_VBYTES 32 in a translation unit built with -msha -mavx2 -mssse3 -msse4.1.
 * The 33-byte key is one SHA-256 block with fixed padding. Its words come from the lane layout of the template (word i of all keys in one
 * vector) by 4x4 transposes; the compression runs once per key on sha256rnds2, sha256msg1 and sha256msg2, with PZ_SHANI_IL independent
 * blocks advanced in lock step so that their latencies overlap; the digests are transposed back into the word-per-lane layout, and the
 * byte swap that RIPEMD-160 needs is the shuffle that ends that transpose. RIPEMD-160 itself is the 8-lane code of the template.
 * The instructions keep the state as two registers, s0 = (F, E, B, A) and s1 = (H, G, D, C) from element 0 up, which is used as it is. */

#include <immintrin.h>

#if !defined(__SHA__) || !defined(__AVX2__) || !defined(__SSSE3__) || !defined(__SSE4_1__)
#error "hash_lanes_shani.inc needs -msha -mavx2 -mssse3 -msse4.1"
#endif
#if PZ_VBYTES != 32
#error "hash_lanes_shani.inc is written for 8 lanes (PZ_VBYTES 32)"
#endif

#ifndef PZ_SHANI_IL
#define PZ_SHANI_IL 2      /* blocks per pass: 2 x (2 state + 4 message) registers and the temporaries fill the 16 xmm registers */
#endif
_Static_assert(PZ_SHANI_IL >= 1 && 8 % PZ_SHANI_IL == 0, "PZ_SHANI_IL must divide 8");

/* 4x4 transpose of 32-bit words inside each 128-bit half: r[j] = (a[j], b[j], c[j], d[j]) in both halves */
static inline void pz_shani_transpose4(__m256i a, __m256i b, __m256i c, __m256i d, __m256i r[4])
{
    const __m256i t0 = _mm256_unpacklo_epi32(a, b), t1 = _mm256_unpacklo_epi32(c, d);
    const __m256i t2 = _mm256_unpackhi_epi32(a, b), t3 = _mm256_unpackhi_epi32(c, d);
    r[0] = _mm256_unpacklo_epi64(t0, t1);
    r[1] = _mm256_unpackhi_epi64(t0, t1);
    r[2] = _mm256_unpacklo_epi64(t2, t3);
    r[3] = _mm256_unpackhi_epi64(t2, t3);
}

/* row[k] = (a[k], b[k], c[k], d[k]) for the eight lanes k: four words per key from four lane vectors */
static inline void pz_shani_rows(__m256i a, __m256i b, __m256i c, __m256i d, __m128i row[8])
{
    __m256i r[4];
    pz_shani_transpose4(a, b, c, d, r);
    for (int k = 0; k < 4; k++) {
        row[k] = _mm256_castsi256_si128(r[k]);
        row[k + 4] = _mm256_extracti128_si256(r[k], 1);
    }
}

/* SHA-256 compression, from the initial state, of PZ_SHANI_IL blocks in lock step. Block b has the message words 0..3 in m0[b], 4..7
 * in m1[b] and 8..11 in m2[b]; words 12..15 are the padding of a 33-byte message (zeros, then the length 264). The digests go to
 * d0[b] = (F, E, B, A) and d1[b] = (H, G, D, C), the register layout of the instructions. */
static inline void pz_shani_blocks(const __m128i *m0, const __m128i *m1, const __m128i *m2, __m128i *d0, __m128i *d1)
{
    const __m128i init0 = _mm_set_epi32((int)pz_sha256_h0[0], (int)pz_sha256_h0[1], (int)pz_sha256_h0[4], (int)pz_sha256_h0[5]);
    const __m128i init1 = _mm_set_epi32((int)pz_sha256_h0[2], (int)pz_sha256_h0[3], (int)pz_sha256_h0[6], (int)pz_sha256_h0[7]);
    __m128i m[PZ_SHANI_IL][4], s0[PZ_SHANI_IL], s1[PZ_SHANI_IL];
    for (int b = 0; b < PZ_SHANI_IL; b++) {
        m[b][0] = m0[b]; m[b][1] = m1[b]; m[b][2] = m2[b];
        m[b][3] = _mm_set_epi32(264, 0, 0, 0);
        s0[b] = init0; s1[b] = init1;
    }
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++) {                       /* four rounds per step */
        const __m128i k = _mm_loadu_si128((const __m128i *)(pz_sha256_k + 4 * i));
#pragma GCC unroll 8
        for (int b = 0; b < PZ_SHANI_IL; b++) {
            __m128i w = m[b][i & 3];                     /* words 4i..4i+3; for i >= 4 from the four steps before */
            if (i >= 4) {
                const __m128i w1 = m[b][(i + 1) & 3], w2 = m[b][(i + 2) & 3], w3 = m[b][(i + 3) & 3];
                w = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w, w1), _mm_alignr_epi8(w3, w2, 4)), w3);
                m[b][i & 3] = w;
            }
            const __m128i wk = _mm_add_epi32(w, k);
            s1[b] = _mm_sha256rnds2_epu32(s1[b], s0[b], wk);                          /* rounds 4i, 4i+1: the new (A,B,E,F) */
            s0[b] = _mm_sha256rnds2_epu32(s0[b], s1[b], _mm_shuffle_epi32(wk, 0x0E)); /* rounds 4i+2, 4i+3 */
        }
    }
    for (int b = 0; b < PZ_SHANI_IL; b++) { d0[b] = _mm_add_epi32(s0[b], init0); d1[b] = _mm_add_epi32(s1[b], init1); }
}

/* SHA-256 of the 33-byte keys. Unlike the template's function, the digest words dig[0..7] (A..H) come out byte-swapped. */
static inline void pz_sha256_lanes_33(const uint32_t *X, const uint32_t *pfx, pz_vu dig[8])
{
    /* message words 0..8 of the eight keys, one vector per word, as in the template */
    pz_vu x[8], w[9];
    const pz_vu p = pz_v_load(pfx);
    for (int i = 0; i < 8; i++) x[i] = pz_v_load(X + i * PZ_LANES);
    w[0] = (p << 24) | (x[0] >> 8);
    for (int i = 1; i < 8; i++) w[i] = (x[i - 1] << 24) | (x[i] >> 8);
    w[8] = (x[7] << 24) | 0x00800000u;

    /* one block per key; word 8 is followed by zeros */
    __m128i mw[3][8], d0[8], d1[8];
    const __m256i zero = _mm256_setzero_si256();
    pz_shani_rows((__m256i)w[0], (__m256i)w[1], (__m256i)w[2], (__m256i)w[3], mw[0]);
    pz_shani_rows((__m256i)w[4], (__m256i)w[5], (__m256i)w[6], (__m256i)w[7], mw[1]);
    pz_shani_rows((__m256i)w[8], zero, zero, zero, mw[2]);
    _mm256_zeroupper();             /* the SHA instructions have no VEX form: with dirty upper halves Intel cores add a merge to each of them */
    for (int g = 0; g < 8; g += PZ_SHANI_IL) pz_shani_blocks(mw[0] + g, mw[1] + g, mw[2] + g, d0 + g, d1 + g);

    /* back to one vector per digest word: keys k and k+4 side by side in the two halves of a register, so that the same transpose
     * yields the words of all eight keys; the byte swap is the last shuffle */
    __m256i y[4], z[4], f[4], h[4];
    for (int k = 0; k < 4; k++) {
        y[k] = _mm256_inserti128_si256(_mm256_castsi128_si256(d0[k]), d0[k + 4], 1);
        z[k] = _mm256_inserti128_si256(_mm256_castsi128_si256(d1[k]), d1[k + 4], 1);
    }
    pz_shani_transpose4(y[0], y[1], y[2], y[3], f);      /* F, E, B, A */
    pz_shani_transpose4(z[0], z[1], z[2], z[3], h);      /* H, G, D, C */
    const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    dig[0] = (pz_vu)_mm256_shuffle_epi8(f[3], swap);     /* A */
    dig[1] = (pz_vu)_mm256_shuffle_epi8(f[2], swap);     /* B */
    dig[2] = (pz_vu)_mm256_shuffle_epi8(h[3], swap);     /* C */
    dig[3] = (pz_vu)_mm256_shuffle_epi8(h[2], swap);     /* D */
    dig[4] = (pz_vu)_mm256_shuffle_epi8(f[1], swap);     /* E */
    dig[5] = (pz_vu)_mm256_shuffle_epi8(f[0], swap);     /* F */
    dig[6] = (pz_vu)_mm256_shuffle_epi8(h[1], swap);     /* G */
    dig[7] = (pz_vu)_mm256_shuffle_epi8(h[0], swap);     /* H */
}
