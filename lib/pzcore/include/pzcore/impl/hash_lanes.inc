/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash_lanes.inc - multi-lane hash160 of compressed public keys with GCC vector extensions: a template, not a header.
 *
 * Included once per translation unit and instruction-set variant, so that the hash is compiled for the instruction set of the
 * includer and, when the includer is a search loop, inlined into it. Before including, define
 *   PZ_VBYTES         the vector width in bytes (16, 32 or 64): the number of lanes is PZ_VBYTES / 4
 *   PZ_HASH_LANES_FN  the name of the function to define (the template also defines PZ_HASH_LANES_FN_w0, see PZ_HASH_W0_FN)
 *   PZ_HASH_EXTERNAL  (optional) a file that implements both functions instead: hash_lanes_neon.inc on aarch64
 *   PZ_HASH_SHA256_EXTERNAL  (optional) a file that replaces only the SHA-256 stage: hash_lanes_shani.inc on x86-64 (PZ_VBYTES 32). It defines
 *                     pz_sha256_lanes_33 with the SHA-256 instructions of the CPU and returns the digest words already byte-swapped,
 *                     as RIPEMD-160 reads them; the rest of this template is used as it is
 * and compile with the matching -m flags (-mavx2, -mavx512f ...; nothing for the 16-byte baseline).
 *
 * The functions it defines take LANES keys at a time:
 *   void PZ_HASH_LANES_FN(const uint32_t *X, const uint32_t *pfx, uint32_t *out)
 *     X[8][PZ_LANES]    the x coordinates as big-endian words, word i of lane l at X[i * PZ_LANES + l]
 *     pfx[PZ_LANES]     the prefix byte of each key: 2 or 3
 *     out[5][PZ_LANES]  the five words of each HASH160 (RIPEMD-160 of SHA-256 of the 33-byte key), little-endian words
 *   void PZ_HASH_W0_FN(const uint32_t *X, const uint32_t *pfx, uint32_t *out0)
 *     the same, but only out[0][..]: the compiler drops the part of RIPEMD-160 that does not reach the first word. A search that
 *     filters on the first word and checks the rest of the few hits with the scalar code does this. */

#include <pzcore/defs.h>
#include <pzcore/impl/hash_tables.h>

#ifndef PZ_CAT
#define PZ_CAT_(a, b) a##b
#define PZ_CAT(a, b) PZ_CAT_(a, b)
#endif

#define PZ_LANES (PZ_VBYTES / 4)
typedef uint32_t pz_vu __attribute__((vector_size(PZ_VBYTES)));

#define PZ_V_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define PZ_V_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static inline pz_vu pz_v_set(uint32_t x)
{
    pz_vu r;
    for (int i = 0; i < PZ_LANES; i++) r[i] = x;
    return r;
}

static inline pz_vu pz_v_bswap(pz_vu x)
{
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

static inline pz_vu pz_v_load(const uint32_t *p)
{
    pz_vu r;
    memcpy(&r, p, sizeof r);
    return r;
}

static inline void pz_v_store(uint32_t *p, pz_vu v) { memcpy(p, &v, sizeof v); }

#ifdef PZ_HASH_EXTERNAL
#include PZ_HASH_EXTERNAL     /* a complete implementation of PZ_HASH_LANES_FN and PZ_HASH_W0_FN for this lane count (aarch64: hash_lanes_neon.inc) */
#else
#ifdef PZ_HASH_SHA256_EXTERNAL
#include PZ_HASH_SHA256_EXTERNAL     /* pz_sha256_lanes_33 for the instruction set of the includer (x86-64: hash_lanes_shani.inc) */
#else
static inline void pz_sha256_lanes_33(const uint32_t *X, const uint32_t *pfx, pz_vu dig[8])
{
    pz_vu w[16];
    pz_vu p = pz_v_load(pfx);
    pz_vu x[8];
    for (int i = 0; i < 8; i++) x[i] = pz_v_load(X + i * PZ_LANES);
    w[0] = (p << 24) | (x[0] >> 8);
    for (int i = 1; i < 8; i++) w[i] = (x[i - 1] << 24) | (x[i] >> 8);
    w[8] = (x[7] << 24) | 0x00800000u;
    for (int i = 9; i < 15; i++) w[i] = pz_v_set(0);
    w[15] = pz_v_set(264);

    /* With AVX2 and AVX-512 the round constants, and the initial state added at the end, are read through pointers the compiler
     * cannot see through: each is then one broadcast from memory (on AVX-512 an operand of the addition) instead of an immediate
     * moved to a general register and broadcast from there. The initial state of the rounds stays visible, as the first rounds
     * fold on it. Not with 16-byte vectors, where the constants are full vectors in memory already, nor for RIPEMD-160, whose
     * constants fold with the padding words and its two zero constants vanish: both counted more instructions. */
    const uint32_t *kt = pz_sha256_k, *hv = pz_sha256_h0;
#if PZ_VBYTES > 16
    __asm__("" : "+r"(kt), "+r"(hv));
#endif
    pz_vu a = pz_v_set(pz_sha256_h0[0]), b = pz_v_set(pz_sha256_h0[1]), c = pz_v_set(pz_sha256_h0[2]), d = pz_v_set(pz_sha256_h0[3]);
    pz_vu e = pz_v_set(pz_sha256_h0[4]), f = pz_v_set(pz_sha256_h0[5]), g = pz_v_set(pz_sha256_h0[6]), h = pz_v_set(pz_sha256_h0[7]);
#pragma GCC unroll 64
    for (int i = 0; i < 64; i++) {
        pz_vu wi;
        if (i < 16) {
            wi = w[i];
        } else {
            pz_vu w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            pz_vu s0 = PZ_V_ROTR(w15, 7) ^ PZ_V_ROTR(w15, 18) ^ (w15 >> 3);
            pz_vu s1 = PZ_V_ROTR(w2, 17) ^ PZ_V_ROTR(w2, 19) ^ (w2 >> 10);
            wi = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
            w[i & 15] = wi;
        }
        pz_vu S1 = PZ_V_ROTR(e, 6) ^ PZ_V_ROTR(e, 11) ^ PZ_V_ROTR(e, 25);
        pz_vu ch = (e & f) ^ (~e & g);
        pz_vu t1 = h + S1 + ch + wi + kt[i];
        pz_vu S0 = PZ_V_ROTR(a, 2) ^ PZ_V_ROTR(a, 13) ^ PZ_V_ROTR(a, 22);
        pz_vu mj = (a & b) ^ (a & c) ^ (b & c);
        pz_vu t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    dig[0] = a + hv[0]; dig[1] = b + hv[1]; dig[2] = c + hv[2]; dig[3] = d + hv[3];
    dig[4] = e + hv[4]; dig[5] = f + hv[5]; dig[6] = g + hv[6]; dig[7] = h + hv[7];
}
#endif

static inline pz_vu pz_rmd_fv(int rd, pz_vu x, pz_vu y, pz_vu z)
{
    switch (rd) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

/* Always inlined: both functions below use it, and an out-of-line copy would compute and store all five words for the first-word
 * filter too. */
static inline __attribute__((always_inline)) void pz_ripemd160_lanes_32(const pz_vu dig[8], pz_vu out[5])
{
    pz_vu m[16];
#ifdef PZ_HASH_SHA256_EXTERNAL
    for (int i = 0; i < 8; i++) m[i] = dig[i];                /* already byte-swapped by the SHA-256 stage */
#else
    for (int i = 0; i < 8; i++) m[i] = pz_v_bswap(dig[i]);
#endif
    m[8] = pz_v_set(0x80);
    for (int i = 9; i < 14; i++) m[i] = pz_v_set(0);
    m[14] = pz_v_set(256);
    m[15] = pz_v_set(0);
    const uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    pz_vu a = pz_v_set(h0), b = pz_v_set(h1), c = pz_v_set(h2), d = pz_v_set(h3), e = pz_v_set(h4);
    pz_vu a2 = a, b2 = b, c2 = c, d2 = d, e2 = e;
#pragma GCC unroll 80
    for (int j = 0; j < 80; j++) {
        const int rd = j / 16;
        pz_vu t = PZ_V_ROTL(a + pz_rmd_fv(rd, b, c, d) + m[pz_rmd_rl[j]] + pz_rmd_kl[rd], pz_rmd_sl[j]) + e;
        a = e; e = d; d = PZ_V_ROTL(c, 10); c = b; b = t;
        t = PZ_V_ROTL(a2 + pz_rmd_fv(4 - rd, b2, c2, d2) + m[pz_rmd_rr[j]] + pz_rmd_kr[rd], pz_rmd_sr[j]) + e2;
        a2 = e2; e2 = d2; d2 = PZ_V_ROTL(c2, 10); c2 = b2; b2 = t;
    }
    out[0] = pz_v_set(h1) + c + d2;
    out[1] = pz_v_set(h2) + d + e2;
    out[2] = pz_v_set(h3) + e + a2;
    out[3] = pz_v_set(h4) + a + b2;
    out[4] = pz_v_set(h0) + b + c2;
}

/* Hashes PZ_LANES compressed public keys. out is [5][PZ_LANES]. */
static PZ_UNUSED void PZ_HASH_LANES_FN(const uint32_t *X, const uint32_t *pfx, uint32_t *out)
{
    pz_vu dig[8], r[5];
    pz_sha256_lanes_33(X, pfx, dig);
    pz_ripemd160_lanes_32(dig, r);
    for (int i = 0; i < 5; i++) pz_v_store(out + i * PZ_LANES, r[i]);
}

/* Only the first word of each pz_hash160 (the search filter; matches are re-checked with the scalar hash). It does not
 * depend on the last step of the left RIPEMD-160 line nor on the last two of the right one, which the compiler drops once
 * pz_ripemd160_lanes_32 is inlined here. */
#define PZ_HASH_W0_FN PZ_CAT(PZ_HASH_LANES_FN, _w0)
static PZ_UNUSED void PZ_HASH_W0_FN(const uint32_t *X, const uint32_t *pfx, uint32_t *out0)
{
    pz_vu dig[8], r[5];
    pz_sha256_lanes_33(X, pfx, dig);
    pz_ripemd160_lanes_32(dig, r);
    pz_v_store(out0, r[0]);
}
#endif /* PZ_HASH_EXTERNAL */
