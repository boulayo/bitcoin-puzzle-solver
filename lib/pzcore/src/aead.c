/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* aead.c - ChaCha20-Poly1305 (see aead.h). Poly1305 in five limbs of 26 bits with 64-bit products, the form of poly1305-donna-32: portable,
 * and far faster than the network it serves. */
#include <pzcore/aead.h>
#include <pzcore/seal.h>
#include "bytes.h"

#define ROTL32(v, n) ((v) << (n) | (v) >> (32 - (n)))
#define QR(a, b, c, d)                          \
    a += b, d ^= a, d = ROTL32(d, 16),          \
    c += d, b ^= c, b = ROTL32(b, 12),          \
    a += b, d ^= a, d = ROTL32(d, 8),           \
    c += d, b ^= c, b = ROTL32(b, 7)

static void chacha20_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]), QR(x[1], x[5], x[9], x[13]), QR(x[2], x[6], x[10], x[14]), QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]), QR(x[1], x[6], x[11], x[12]), QR(x[2], x[7], x[8], x[13]), QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) store_le32(out + 4 * i, x[i] + in[i]);
    pz_secure_zero(x, sizeof x);
}

void pz_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in, uint8_t *out, size_t len)
{
    uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};   /* "expand 32-byte k" */
    for (int i = 0; i < 8; i++) st[4 + i] = load_le32(key + 4 * i);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13 + i] = load_le32(nonce + 4 * i);
    uint8_t ks[64];
    for (size_t o = 0; o < len; o += 64) {
        chacha20_block(st, ks);
        st[12]++;
        const size_t n = len - o < 64 ? len - o : 64;
        for (size_t i = 0; i < n; i++) out[o + i] = in[o + i] ^ ks[i];
    }
    pz_secure_zero(st, sizeof st);
    pz_secure_zero(ks, sizeof ks);
}

typedef struct { uint32_t r[5], h[5], pad[4]; } poly1305;

static void poly_init(poly1305 *p, const uint8_t key[32])
{
    p->r[0] = load_le32(key + 0) & 0x3ffffff;                   /* r clamped as the RFC says */
    p->r[1] = (load_le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (load_le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (load_le32(key + 12) >> 8) & 0x00fffff;
    memset(p->h, 0, sizeof p->h);
    for (int i = 0; i < 4; i++) p->pad[i] = load_le32(key + 16 + 4 * i);
}

/* whole 16-byte blocks; hibit is 2^128 in the top limb (1 << 24), 0 for a last short block that was padded with 0x01 */
static void poly_blocks(poly1305 *p, const uint8_t *m, size_t nblocks, uint32_t hibit)
{
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    for (; nblocks; nblocks--, m += 16) {
        h0 += load_le32(m + 0) & 0x3ffffff;
        h1 += (load_le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (load_le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (load_le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (load_le32(m + 12) >> 8) | hibit;
        const uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c, c = (uint32_t)(d1 >> 26), h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c, c = (uint32_t)(d2 >> 26), h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c, c = (uint32_t)(d3 >> 26), h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c, c = (uint32_t)(d4 >> 26), h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
    }
    p->h[0] = h0, p->h[1] = h1, p->h[2] = h2, p->h[3] = h3, p->h[4] = h4;
}

static void poly_finish(poly1305 *p, uint8_t tag[16])
{
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26, h1 &= 0x3ffffff;                              /* full carry */
    h2 += c, c = h2 >> 26, h2 &= 0x3ffffff;
    h3 += c, c = h3 >> 26, h3 &= 0x3ffffff;
    h4 += c, c = h4 >> 26, h4 &= 0x3ffffff;
    h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5, g1, g2, g3, g4;                       /* g = h - (2^130 - 5); h is kept when g is negative */
    c = g0 >> 26, g0 &= 0x3ffffff;
    g1 = h1 + c, c = g1 >> 26, g1 &= 0x3ffffff;
    g2 = h2 + c, c = g2 >> 26, g2 &= 0x3ffffff;
    g3 = h3 + c, c = g3 >> 26, g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);
    const uint32_t mask = (g4 >> 31) - 1;                       /* all ones when g >= 0 */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);
    const uint32_t w0 = h0 | h1 << 26, w1 = h1 >> 6 | h2 << 20, w2 = h2 >> 12 | h3 << 14, w3 = h3 >> 18 | h4 << 8;   /* h mod 2^128 */
    uint64_t f = (uint64_t)w0 + p->pad[0];                      /* + s, mod 2^128 */
    store_le32(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + p->pad[1] + (f >> 32);
    store_le32(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + p->pad[2] + (f >> 32);
    store_le32(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + p->pad[3] + (f >> 32);
    store_le32(tag + 12, (uint32_t)f);
    pz_secure_zero(p, sizeof *p);
}

void pz_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{
    poly1305 p;
    poly_init(&p, key);
    poly_blocks(&p, msg, len / 16, 1u << 24);
    if (len % 16) {
        uint8_t last[16] = {0};
        memcpy(last, msg + len - len % 16, len % 16);
        last[len % 16] = 1;
        poly_blocks(&p, last, 1, 0);
    }
    poly_finish(&p, tag);
}

/* the input of the AEAD tag: ad and ct each padded with zeros to 16 bytes, then their lengths; every block is whole */
static void poly_padded(poly1305 *p, const uint8_t *m, size_t len)
{
    poly_blocks(p, m, len / 16, 1u << 24);
    if (len % 16) {
        uint8_t last[16] = {0};
        memcpy(last, m + len - len % 16, len % 16);
        poly_blocks(p, last, 1, 1u << 24);
    }
}

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len, uint8_t tag[16])
{
    uint8_t otk[32] = {0}, lens[16];
    pz_chacha20_xor(key, nonce, 0, otk, otk, 32);              /* the one-time key: the first half of block 0 */
    poly1305 p;
    poly_init(&p, otk);
    poly_padded(&p, ad, adlen);
    poly_padded(&p, ct, len);
    store_le32(lens + 0, (uint32_t)adlen), store_le32(lens + 4, (uint32_t)((uint64_t)adlen >> 32));
    store_le32(lens + 8, (uint32_t)len), store_le32(lens + 12, (uint32_t)((uint64_t)len >> 32));
    poly_blocks(&p, lens, 1, 1u << 24);
    poly_finish(&p, tag);
    pz_secure_zero(otk, sizeof otk);
}

void pz_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t adlen, const uint8_t *pt, size_t len,
                  uint8_t *ct, uint8_t tag[16])
{
    pz_chacha20_xor(key, nonce, 1, pt, ct, len);
    aead_tag(key, nonce, ad, adlen, ct, len, tag);
}

bool pz_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len,
                  const uint8_t tag[16], uint8_t *pt)
{
    uint8_t t[16];
    aead_tag(key, nonce, ad, adlen, ct, len, t);
    const bool ok = pz_ct_equal(t, tag, 16);
    pz_secure_zero(t, sizeof t);
    if (ok) pz_chacha20_xor(key, nonce, 1, ct, pt, len);
    return ok;
}
