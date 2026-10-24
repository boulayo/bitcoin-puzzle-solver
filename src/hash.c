/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* hash.c - SHA-256, RIPEMD-160, Base58Check (scalar reference code). */
#include "hash.h"
#include "hash_tables.h"
#include "ec.h"


static inline u32 rotr32(u32 x, int n) { return x >> n | x << (32 - n); }
static inline u32 rotl32(u32 x, int n) { return x << n | x >> (32 - n); }

void sha256_compress(u32 st[8], const u32 win[16])
{
    u32 w[64];
    memcpy(w, win, 64);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + SHA256_K[i] + w[i];
        u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void sha256(const u8 *data, size_t len, u8 out[32])
{
    u32 st[8];
    memcpy(st, SHA256_H0, sizeof st);
    u8 blk[128];
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) {
        u32 w[16];
        for (int j = 0; j < 16; j++) {
            const u8 *p = data + i * 64 + j * 4;
            w[j] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        }
        sha256_compress(st, w);
    }
    size_t rem = len - full * 64;
    memset(blk, 0, sizeof blk);
    memcpy(blk, data + full * 64, rem);
    blk[rem] = 0x80;
    size_t nb = rem + 9 > 64 ? 2 : 1;
    u64 bits = (u64)len * 8;
    for (int j = 0; j < 8; j++) blk[nb * 64 - 1 - j] = (u8)(bits >> (8 * j));
    for (size_t i = 0; i < nb; i++) {
        u32 w[16];
        for (int j = 0; j < 16; j++) {
            const u8 *p = blk + i * 64 + j * 4;
            w[j] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        }
        sha256_compress(st, w);
    }
    for (int j = 0; j < 8; j++) {
        out[j * 4] = (u8)(st[j] >> 24); out[j * 4 + 1] = (u8)(st[j] >> 16);
        out[j * 4 + 2] = (u8)(st[j] >> 8); out[j * 4 + 3] = (u8)st[j];
    }
}

void sha256d(const u8 *data, size_t len, u8 out[32])
{
    u8 t[32];
    sha256(data, len, t);
    sha256(t, 32, out);
}

/* ---- RIPEMD-160 ---- */

static inline u32 rmd_f(int round, u32 x, u32 y, u32 z)
{
    switch (round) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

void ripemd160_compress(u32 h[5], const u32 x[16])
{
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    u32 a2 = a, b2 = b, c2 = c, d2 = d, e2 = e;
    for (int j = 0; j < 80; j++) {
        int rd = j / 16;
        u32 t = rotl32(a + rmd_f(rd, b, c, d) + x[RMD_RL[j]] + RMD_KL[rd], (int)RMD_SL[j]) + e;
        a = e; e = d; d = rotl32(c, 10); c = b; b = t;
        t = rotl32(a2 + rmd_f(4 - rd, b2, c2, d2) + x[RMD_RR[j]] + RMD_KR[rd], (int)RMD_SR[j]) + e2;
        a2 = e2; e2 = d2; d2 = rotl32(c2, 10); c2 = b2; b2 = t;
    }
    u32 t = h[1] + c + d2;
    h[1] = h[2] + d + e2;
    h[2] = h[3] + e + a2;
    h[3] = h[4] + a + b2;
    h[4] = h[0] + b + c2;
    h[0] = t;
}

void ripemd160(const u8 *data, size_t len, u8 out[20])
{
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u8 blk[128];
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) {
        u32 x[16];
        for (int j = 0; j < 16; j++) {
            const u8 *p = data + i * 64 + j * 4;
            x[j] = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
        }
        ripemd160_compress(h, x);
    }
    size_t rem = len - full * 64;
    memset(blk, 0, sizeof blk);
    memcpy(blk, data + full * 64, rem);
    blk[rem] = 0x80;
    size_t nb = rem + 9 > 64 ? 2 : 1;
    u64 bits = (u64)len * 8;
    for (int j = 0; j < 8; j++) blk[nb * 64 - 8 + j] = (u8)(bits >> (8 * j));
    for (size_t i = 0; i < nb; i++) {
        u32 x[16];
        for (int j = 0; j < 16; j++) {
            const u8 *p = blk + i * 64 + j * 4;
            x[j] = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
        }
        ripemd160_compress(h, x);
    }
    for (int j = 0; j < 5; j++) {
        out[j * 4] = (u8)h[j]; out[j * 4 + 1] = (u8)(h[j] >> 8);
        out[j * 4 + 2] = (u8)(h[j] >> 16); out[j * 4 + 3] = (u8)(h[j] >> 24);
    }
}

void hash160(const u8 *data, size_t len, u8 out[20])
{
    u8 t[32];
    sha256(data, len, t);
    ripemd160(t, 32, out);
}

void hash160_pub33(u8 parity, const u8 x_be[32], u8 out[20])
{
    u8 pub[33];
    pub[0] = (u8)(2 + (parity & 1));
    memcpy(pub + 1, x_be, 32);
    hash160(pub, 33, out);
}

/* ---- Base58Check ---- */
static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int base58check_decode(const char *s, u8 *out, size_t maxlen)
{
    u8 buf[128];
    size_t n = 0;   /* big-endian base-256 accumulator, most significant first */
    size_t zeros = 0;
    while (s[zeros] == '1') zeros++;
    for (const char *p = s + zeros; *p; p++) {
        const char *q = strchr(B58, *p);
        if (!q || !*p) return -1;
        unsigned carry = (unsigned)(q - B58);
        for (size_t i = 0; i < n; i++) {
            unsigned v = (unsigned)buf[sizeof buf - 1 - i] * 58 + carry;
            buf[sizeof buf - 1 - i] = (u8)v;
            carry = v >> 8;
        }
        while (carry) {
            if (n >= sizeof buf) return -1;
            buf[sizeof buf - 1 - n] = (u8)carry;
            n++;
            carry >>= 8;
        }
    }
    size_t total = zeros + n;
    if (total < 5 || total > maxlen + 4 || total > 100) return -1;
    u8 raw[128];
    memset(raw, 0, zeros);
    memcpy(raw + zeros, buf + sizeof buf - n, n);
    u8 chk[32];
    sha256d(raw, total - 4, chk);
    if (memcmp(chk, raw + total - 4, 4)) return -1;
    memcpy(out, raw, total - 4);
    return (int)(total - 4);
}

void base58check_encode(const u8 *payload, size_t len, char *out, size_t outlen)
{
    u8 raw[128], chk[32];
    memcpy(raw, payload, len);
    sha256d(payload, len, chk);
    memcpy(raw + len, chk, 4);
    size_t total = len + 4;
    size_t zeros = 0;
    while (zeros < total && raw[zeros] == 0) zeros++;
    u8 digits[200];
    size_t nd = 0;
    for (size_t i = zeros; i < total; i++) {
        unsigned carry = raw[i];
        for (size_t j = 0; j < nd; j++) {
            unsigned v = (unsigned)digits[j] * 256 + carry;
            digits[j] = (u8)(v % 58);
            carry = v / 58;
        }
        while (carry) { digits[nd++] = (u8)(carry % 58); carry /= 58; }
    }
    size_t o = 0;
    for (size_t i = 0; i < zeros && o + 1 < outlen; i++) out[o++] = '1';
    for (size_t i = nd; i > 0 && o + 1 < outlen; i--) out[o++] = B58[digits[i - 1]];
    out[o] = 0;
}

bool address_to_hash160(const char *addr, u8 h160[20])
{
    u8 payload[64];
    int n = base58check_decode(addr, payload, sizeof payload);
    if (n != 21 || payload[0] != 0x00) return false;
    memcpy(h160, payload + 1, 20);
    return true;
}

void hash160_to_address(const u8 h160[20], char *out, size_t outlen)
{
    u8 payload[21];
    payload[0] = 0;
    memcpy(payload + 1, h160, 20);
    base58check_encode(payload, 21, out, outlen);
}

void privkey_to_wif(const u8 key_be[32], bool compressed, char *out, size_t outlen)
{
    u8 payload[34];
    payload[0] = 0x80;
    memcpy(payload + 1, key_be, 32);
    size_t n = 33;
    if (compressed) payload[n++] = 1;
    base58check_encode(payload, n, out, outlen);
}
