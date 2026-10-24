/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash.c - SHA-256, RIPEMD-160 and HASH160 (portable scalar code). */
#include <pzcore/hash.h>
#include <pzcore/cpu.h>
#include <pzcore/impl/hash_tables.h>
#include <stdatomic.h>
#include "bytes.h"
#include "h160_impl.h"

static inline uint32_t rotr32(uint32_t x, int n) { return x >> n | x << (32 - n); }
static inline uint32_t rotl32(uint32_t x, int n) { return x << n | x >> (32 - n); }

/* The rounds are unrolled by the compiler (the tables and the round constants then fold into immediates), and the message schedule
 * is computed in a 16-word window as the rounds consume it: this takes a block from 255 ns to 205 ns on the machine the library was
 * tuned on. The result is the same bit for bit (checked against the plain loop on 100000 random blocks, and by the tests). */
void pz_sha256_compress(uint32_t st[8], const uint32_t win[16])
{
    uint32_t w[16];
    memcpy(w, win, 64);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
#pragma GCC unroll 64
    for (int i = 0; i < 64; i++) {
        uint32_t wi;
        if (i < 16) {
            wi = w[i];
        } else {
            const uint32_t w15 = w[(i + 1) & 15], w2 = w[(i + 14) & 15];
            wi = w[i & 15] + (rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3)) + w[(i + 9) & 15] + (rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10));
            w[i & 15] = wi;
        }
        const uint32_t t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + (g ^ (e & (f ^ g))) + pz_sha256_k[i] + wi;
        const uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) | (c & (a | b)));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

#ifdef PZ_HAVE_ARM_SHA2
static _Atomic int g_arm_sha2 = -1;          /* -1 until probed; the probe is cheap and always gives the same answer */
static bool arm_sha2(void)
{
    int v = atomic_load_explicit(&g_arm_sha2, memory_order_relaxed);
    if (v < 0) {
        v = pz_cpu_has_sha2() ? 1 : 0;
        atomic_store_explicit(&g_arm_sha2, v, memory_order_relaxed);
    }
    return v;
}
#endif

/* the compression function over nblocks consecutive 64-byte blocks of data */
static void sha256_blocks(uint32_t st[8], const uint8_t *data, size_t nblocks)
{
#ifdef PZ_HAVE_ARM_SHA2
    if (arm_sha2()) {
        pz_sha256_blocks_arm(st, data, nblocks);
        return;
    }
#endif
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t w[16];
        for (int j = 0; j < 16; j++) w[j] = load_be32(data + i * 64 + j * 4);
        pz_sha256_compress(st, w);
    }
}

void pz_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t st[8];
    memcpy(st, pz_sha256_h0, sizeof st);
    uint8_t blk[128];
    const size_t full = len / 64;
    sha256_blocks(st, data, full);
    const size_t rem = len - full * 64;
    memset(blk, 0, sizeof blk);
    if (rem) memcpy(blk, data + full * 64, rem);
    blk[rem] = 0x80;
    const size_t nb = rem + 9 > 64 ? 2 : 1;
    const uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) blk[nb * 64 - 1 - j] = (uint8_t)(bits >> (8 * j));
    sha256_blocks(st, blk, nb);
    for (int j = 0; j < 8; j++) store_be32(out + j * 4, st[j]);
}

void pz_sha256d(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t t[32];
    pz_sha256(data, len, t);
    pz_sha256(t, 32, out);
}

/* ---- RIPEMD-160 ---- */

static inline uint32_t rmd_f(int round, uint32_t x, uint32_t y, uint32_t z)
{
    switch (round) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

void pz_ripemd160_compress(uint32_t h[5], const uint32_t x[16])
{
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    uint32_t a2 = a, b2 = b, c2 = c, d2 = d, e2 = e;
#pragma GCC unroll 80    /* with the 80 steps unrolled the index tables, the shifts and the choice of the round function are constants: 311 ns to 139 ns */
    for (int j = 0; j < 80; j++) {
        const int rd = j / 16;
        uint32_t t = rotl32(a + rmd_f(rd, b, c, d) + x[pz_rmd_rl[j]] + pz_rmd_kl[rd], (int)pz_rmd_sl[j]) + e;
        a = e; e = d; d = rotl32(c, 10); c = b; b = t;
        t = rotl32(a2 + rmd_f(4 - rd, b2, c2, d2) + x[pz_rmd_rr[j]] + pz_rmd_kr[rd], (int)pz_rmd_sr[j]) + e2;
        a2 = e2; e2 = d2; d2 = rotl32(c2, 10); c2 = b2; b2 = t;
    }
    uint32_t t = h[1] + c + d2;
    h[1] = h[2] + d + e2;
    h[2] = h[3] + e + a2;
    h[3] = h[4] + a + b2;
    h[4] = h[0] + b + c2;
    h[0] = t;
}

void pz_ripemd160(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t blk[128];
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) {
        uint32_t x[16];
        for (int j = 0; j < 16; j++) x[j] = load_le32(data + i * 64 + j * 4);
        pz_ripemd160_compress(h, x);
    }
    size_t rem = len - full * 64;
    memset(blk, 0, sizeof blk);
    if (rem) memcpy(blk, data + full * 64, rem);
    blk[rem] = 0x80;
    size_t nb = rem + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) blk[nb * 64 - 8 + j] = (uint8_t)(bits >> (8 * j));
    for (size_t i = 0; i < nb; i++) {
        uint32_t x[16];
        for (int j = 0; j < 16; j++) x[j] = load_le32(blk + i * 64 + j * 4);
        pz_ripemd160_compress(h, x);
    }
    for (int j = 0; j < 5; j++) store_le32(out + j * 4, h[j]);
}

void pz_hash160(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint8_t t[32];
    pz_sha256(data, len, t);
    pz_ripemd160(t, 32, out);
}

void pz_hash160_pub33(uint8_t parity, const uint8_t x_be[32], uint8_t out[20])
{
    uint8_t pub[33];
    pub[0] = (uint8_t)(2 + (parity & 1));
    memcpy(pub + 1, x_be, 32);
    pz_hash160(pub, 33, out);
}
