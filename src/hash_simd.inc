/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash_simd.inc - multi-lane hash160 of compressed public keys using GCC
 * vector extensions. Included once per instruction-set variant; the includer
 * must define VBYTES (16, 32 or 64) before including.
 *
 * Input layout: X[8][LANES]  big-endian 32-bit words of the x coordinate
 *               pfx[LANES]   0x02 / 0x03
 * Output:       out[5][LANES] the five little-endian words of RIPEMD160(SHA256(pubkey)). */

#define LANES (VBYTES / 4)
typedef u32 vu __attribute__((vector_size(VBYTES)));

#define V_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define V_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static inline vu v_set(u32 x)
{
    vu r;
    for (int i = 0; i < LANES; i++) r[i] = x;
    return r;
}

static inline vu v_bswap(vu x)
{
    return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

static inline vu v_load(const u32 *p)
{
    vu r;
    memcpy(&r, p, sizeof r);
    return r;
}

static inline void v_store(u32 *p, vu v) { memcpy(p, &v, sizeof v); }

static inline void sha256_lanes_33(const u32 *X, const u32 *pfx, vu dig[8])
{
    vu w[16];
    vu p = v_load(pfx);
    vu x[8];
    for (int i = 0; i < 8; i++) x[i] = v_load(X + i * LANES);
    w[0] = (p << 24) | (x[0] >> 8);
    for (int i = 1; i < 8; i++) w[i] = (x[i - 1] << 24) | (x[i] >> 8);
    w[8] = (x[7] << 24) | 0x00800000u;
    for (int i = 9; i < 15; i++) w[i] = v_set(0);
    w[15] = v_set(264);

    vu a = v_set(SHA256_H0[0]), b = v_set(SHA256_H0[1]), c = v_set(SHA256_H0[2]), d = v_set(SHA256_H0[3]);
    vu e = v_set(SHA256_H0[4]), f = v_set(SHA256_H0[5]), g = v_set(SHA256_H0[6]), h = v_set(SHA256_H0[7]);
#pragma GCC unroll 64
    for (int i = 0; i < 64; i++) {
        vu wi;
        if (i < 16) {
            wi = w[i];
        } else {
            vu w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            vu s0 = V_ROTR(w15, 7) ^ V_ROTR(w15, 18) ^ (w15 >> 3);
            vu s1 = V_ROTR(w2, 17) ^ V_ROTR(w2, 19) ^ (w2 >> 10);
            wi = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
            w[i & 15] = wi;
        }
        vu S1 = V_ROTR(e, 6) ^ V_ROTR(e, 11) ^ V_ROTR(e, 25);
        vu ch = (e & f) ^ (~e & g);
        vu t1 = h + S1 + ch + wi + SHA256_K[i];
        vu S0 = V_ROTR(a, 2) ^ V_ROTR(a, 13) ^ V_ROTR(a, 22);
        vu mj = (a & b) ^ (a & c) ^ (b & c);
        vu t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    dig[0] = a + SHA256_H0[0]; dig[1] = b + SHA256_H0[1]; dig[2] = c + SHA256_H0[2]; dig[3] = d + SHA256_H0[3];
    dig[4] = e + SHA256_H0[4]; dig[5] = f + SHA256_H0[5]; dig[6] = g + SHA256_H0[6]; dig[7] = h + SHA256_H0[7];
}

static inline vu rmd_fv(int rd, vu x, vu y, vu z)
{
    switch (rd) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

static inline void ripemd160_lanes_32(const vu dig[8], vu out[5])
{
    vu m[16];
    for (int i = 0; i < 8; i++) m[i] = v_bswap(dig[i]);
    m[8] = v_set(0x80);
    for (int i = 9; i < 14; i++) m[i] = v_set(0);
    m[14] = v_set(256);
    m[15] = v_set(0);
    const u32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    vu a = v_set(h0), b = v_set(h1), c = v_set(h2), d = v_set(h3), e = v_set(h4);
    vu a2 = a, b2 = b, c2 = c, d2 = d, e2 = e;
#pragma GCC unroll 80
    for (int j = 0; j < 80; j++) {
        const int rd = j / 16;
        vu t = V_ROTL(a + rmd_fv(rd, b, c, d) + m[RMD_RL[j]] + RMD_KL[rd], RMD_SL[j]) + e;
        a = e; e = d; d = V_ROTL(c, 10); c = b; b = t;
        t = V_ROTL(a2 + rmd_fv(4 - rd, b2, c2, d2) + m[RMD_RR[j]] + RMD_KR[rd], RMD_SR[j]) + e2;
        a2 = e2; e2 = d2; d2 = V_ROTL(c2, 10); c2 = b2; b2 = t;
    }
    out[0] = v_set(h1) + c + d2;
    out[1] = v_set(h2) + d + e2;
    out[2] = v_set(h3) + e + a2;
    out[3] = v_set(h4) + a + b2;
    out[4] = v_set(h0) + b + c2;
}

/* Hashes LANES compressed public keys. out is [5][LANES]. */
static void HASH_LANES_FN(const u32 *X, const u32 *pfx, u32 *out)
{
    vu dig[8], r[5];
    sha256_lanes_33(X, pfx, dig);
    ripemd160_lanes_32(dig, r);
    for (int i = 0; i < 5; i++) v_store(out + i * LANES, r[i]);
}

/* Only the first word of each hash160 (the search filter; matches are re-checked with the scalar hash). It does not
 * depend on the last step of the left RIPEMD-160 line nor on the last two of the right one, which the compiler drops. */
#define HASH_W0_FN CAT(HASH_LANES_FN, _w0)
static void HASH_W0_FN(const u32 *X, const u32 *pfx, u32 *out0)
{
    vu dig[8], r[5];
    sha256_lanes_33(X, pfx, dig);
    ripemd160_lanes_32(dig, r);
    v_store(out0, r[0]);
}
