/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* kdf.c - HMAC-SHA256, HKDF, PBKDF2, scrypt, the wipe and the constant-time comparison (see seal.h). */
#include <pzcore/seal.h>
#include <pzcore/hash.h>
#include <pzcore/impl/hash_tables.h>
#include "bytes.h"

void pz_secure_zero(void *p, size_t n)
{
    if (!p || !n) return;
    memset(p, 0, n);
    /* the compiler is told that the memory is read afterwards, so that the stores above are not dead */
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

bool pz_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = (const volatile uint8_t *)a, *y = (const volatile uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

/* ---- SHA-256 by pieces, on the compression function of hash.h (private: the library's hash API is one-shot) ---- */

typedef struct { uint32_t st[8]; uint8_t buf[64]; uint64_t len; } sha_t;

static void sha_init(sha_t *s)
{
    memcpy(s->st, pz_sha256_h0, sizeof s->st);
    s->len = 0;
}

static void sha_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
    pz_sha256_compress(st, w);
}

static void sha_update(uint32_t st[8], uint8_t buf[64], uint64_t *len, const uint8_t *d, size_t n)
{
    size_t fill = (size_t)(*len & 63);
    *len += n;
    if (fill) {
        const size_t take = 64 - fill < n ? 64 - fill : n;
        memcpy(buf + fill, d, take);
        d += take;
        n -= take;
        if (fill + take < 64) return;
        sha_block(st, buf);
    }
    for (; n >= 64; n -= 64, d += 64) sha_block(st, d);
    if (n) memcpy(buf, d, n);
}

/* the digest of the data hashed so far; total is the length that goes into the padding (the key block of an HMAC included) */
static void sha_final(uint32_t st[8], uint8_t buf[64], uint64_t len, uint64_t total, uint8_t out[32])
{
    size_t fill = (size_t)(len & 63);
    buf[fill++] = 0x80;
    if (fill > 56) {
        memset(buf + fill, 0, 64 - fill);
        sha_block(st, buf);
        fill = 0;
    }
    memset(buf + fill, 0, 56 - fill);
    const uint64_t bits = total * 8;
    store_be32(buf + 56, (uint32_t)(bits >> 32));
    store_be32(buf + 60, (uint32_t)bits);
    sha_block(st, buf);
    for (int i = 0; i < 8; i++) store_be32(out + 4 * i, st[i]);
}

/* ---- HMAC ---- */

void pz_hmac_sha256_init(pz_hmac_sha256 *c, const uint8_t *key, size_t keylen)
{
    uint8_t k[64], blk[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) pz_sha256(key, keylen, k);
    else if (keylen) memcpy(k, key, keylen);
    sha_t s;
    for (int i = 0; i < 64; i++) blk[i] = k[i] ^ 0x36;
    sha_init(&s);
    sha_block(s.st, blk);
    memcpy(c->in, s.st, sizeof c->in);
    for (int i = 0; i < 64; i++) blk[i] = k[i] ^ 0x5c;
    sha_init(&s);
    sha_block(s.st, blk);
    memcpy(c->out, s.st, sizeof c->out);
    c->len = 0;
    pz_secure_zero(k, sizeof k);
    pz_secure_zero(blk, sizeof blk);
    pz_secure_zero(&s, sizeof s);
}

void pz_hmac_sha256_update(pz_hmac_sha256 *c, const uint8_t *data, size_t len)
{
    if (len) sha_update(c->in, c->buf, &c->len, data, len);
}

void pz_hmac_sha256_final(pz_hmac_sha256 *c, uint8_t out[32])
{
    uint8_t inner[32], blk[64];
    sha_final(c->in, c->buf, c->len, c->len + 64, inner);
    uint32_t st[8];
    memcpy(st, c->out, sizeof st);
    sha_update(st, blk, &(uint64_t){0}, inner, 32);       /* the buffer takes the 32 bytes; the padding follows */
    sha_final(st, blk, 32, 96, out);
    pz_secure_zero(inner, sizeof inner);
    pz_secure_zero(blk, sizeof blk);
    pz_secure_zero(st, sizeof st);
    pz_secure_zero(c, sizeof *c);
}

void pz_hmac_sha256_oneshot(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[32])
{
    pz_hmac_sha256 c;
    pz_hmac_sha256_init(&c, key, keylen);
    pz_hmac_sha256_update(&c, data, len);
    pz_hmac_sha256_final(&c, out);
}

/* ---- HKDF ---- */

bool pz_hkdf_sha256(const uint8_t *salt, size_t saltlen, const uint8_t *ikm, size_t ikmlen, const uint8_t *info, size_t infolen,
                    uint8_t *okm, size_t okmlen)
{
    if (okmlen > PZ_HKDF_SHA256_MAX_OUT) return false;
    uint8_t prk[32], t[32];
    pz_hmac_sha256_oneshot(salt, saltlen, ikm, ikmlen, prk);      /* an empty salt is the zero key of the RFC: HMAC pads a short key with zeros */
    pz_hmac_sha256 base;
    pz_hmac_sha256_init(&base, prk, 32);
    size_t done = 0;
    for (uint8_t i = 1; done < okmlen; i++) {
        pz_hmac_sha256 c = base;
        if (i > 1) pz_hmac_sha256_update(&c, t, 32);
        if (infolen) pz_hmac_sha256_update(&c, info, infolen);
        pz_hmac_sha256_update(&c, &i, 1);
        pz_hmac_sha256_final(&c, t);
        const size_t take = okmlen - done < 32 ? okmlen - done : 32;
        memcpy(okm + done, t, take);
        done += take;
    }
    pz_secure_zero(prk, sizeof prk);
    pz_secure_zero(t, sizeof t);
    pz_secure_zero(&base, sizeof base);
    return true;
}

/* ---- PBKDF2 ---- */

bool pz_pbkdf2_sha256(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t saltlen, uint32_t iter, uint8_t *out, size_t outlen)
{
    if (iter == 0 || (uint64_t)outlen > 0xFFFFFFFFULL * 32) return false;
    pz_hmac_sha256 base;
    pz_hmac_sha256_init(&base, pw, pwlen);
    uint8_t u[32], t[32];
    for (uint32_t blk = 1; outlen; blk++) {
        uint8_t ctr[4];
        store_be32(ctr, blk);
        pz_hmac_sha256 c = base;
        if (saltlen) pz_hmac_sha256_update(&c, salt, saltlen);
        pz_hmac_sha256_update(&c, ctr, 4);
        pz_hmac_sha256_final(&c, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iter; i++) {
            c = base;
            pz_hmac_sha256_update(&c, u, 32);
            pz_hmac_sha256_final(&c, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        const size_t take = outlen < 32 ? outlen : 32;
        memcpy(out, t, take);
        out += take;
        outlen -= take;
    }
    pz_secure_zero(u, sizeof u);
    pz_secure_zero(t, sizeof t);
    pz_secure_zero(&base, sizeof base);
    return true;
}

/* ---- scrypt ---- */

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

static void salsa20_8(uint32_t b[16])
{
    uint32_t x[16];
    memcpy(x, b, sizeof x);
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= ROTL(x[0] + x[12], 7);  x[8] ^= ROTL(x[4] + x[0], 9);
        x[12] ^= ROTL(x[8] + x[4], 13); x[0] ^= ROTL(x[12] + x[8], 18);
        x[9] ^= ROTL(x[5] + x[1], 7);   x[13] ^= ROTL(x[9] + x[5], 9);
        x[1] ^= ROTL(x[13] + x[9], 13); x[5] ^= ROTL(x[1] + x[13], 18);
        x[14] ^= ROTL(x[10] + x[6], 7); x[2] ^= ROTL(x[14] + x[10], 9);
        x[6] ^= ROTL(x[2] + x[14], 13); x[10] ^= ROTL(x[6] + x[2], 18);
        x[3] ^= ROTL(x[15] + x[11], 7); x[7] ^= ROTL(x[3] + x[15], 9);
        x[11] ^= ROTL(x[7] + x[3], 13); x[15] ^= ROTL(x[11] + x[7], 18);
        x[1] ^= ROTL(x[0] + x[3], 7);   x[2] ^= ROTL(x[1] + x[0], 9);
        x[3] ^= ROTL(x[2] + x[1], 13);  x[0] ^= ROTL(x[3] + x[2], 18);
        x[6] ^= ROTL(x[5] + x[4], 7);   x[7] ^= ROTL(x[6] + x[5], 9);
        x[4] ^= ROTL(x[7] + x[6], 13);  x[5] ^= ROTL(x[4] + x[7], 18);
        x[11] ^= ROTL(x[10] + x[9], 7); x[8] ^= ROTL(x[11] + x[10], 9);
        x[9] ^= ROTL(x[8] + x[11], 13); x[10] ^= ROTL(x[9] + x[8], 18);
        x[12] ^= ROTL(x[15] + x[14], 7); x[13] ^= ROTL(x[12] + x[15], 9);
        x[14] ^= ROTL(x[13] + x[12], 13); x[15] ^= ROTL(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) b[i] += x[i];
}

/* BlockMix of RFC 7914 on 2r blocks of 16 words: out gets the even-numbered results first, then the odd ones. in and out do not overlap. */
static void blockmix(const uint32_t *in, uint32_t *out, uint32_t r)
{
    uint32_t x[16];
    memcpy(x, in + (size_t)(2 * r - 1) * 16, sizeof x);
    for (uint32_t i = 0; i < 2 * r; i++) {
        for (int j = 0; j < 16; j++) x[j] ^= in[(size_t)i * 16 + (size_t)j];
        salsa20_8(x);
        memcpy(out + (size_t)((i & 1) ? r + i / 2 : i / 2) * 16, x, sizeof x);
    }
    pz_secure_zero(x, sizeof x);
}

size_t pz_scrypt_work_size(uint64_t N, uint32_t r, uint32_t p)
{
    if (N < 2 || (N & (N - 1)) || r == 0 || p == 0 || (uint64_t)r * p >= (1ULL << 30)) return 0;
    size_t s;
    if (__builtin_mul_overflow((size_t)128 * r, (size_t)(N + p + 2), &s) || N > (1ULL << 48)) return 0;
    return s;
}

bool pz_scrypt(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t saltlen, uint64_t N, uint32_t r, uint32_t p,
               uint8_t *out, size_t outlen, void *work, size_t worksize)
{
    const size_t need = pz_scrypt_work_size(N, r, p);
    if (!need || worksize < need || !work || ((uintptr_t)work & 7) || (uint64_t)outlen > 0xFFFFFFFFULL * 32) return false;
    const size_t bw = (size_t)32 * r;                       /* words in a block of 128 * r bytes */
    uint32_t *b = (uint32_t *)work, *v = b + bw * p, *x = v + bw * (size_t)N, *y = x + bw;
    uint8_t *bb = (uint8_t *)work;                          /* the same memory as bytes: the PBKDF2 output, then the words read from it */
    if (!pz_pbkdf2_sha256(pw, pwlen, salt, saltlen, 1, bb, bw * 4 * p)) return false;
    for (size_t i = 0; i < bw * p; i++) b[i] = load_le32(bb + 4 * i);       /* in place: word i is read before byte i * 4 is overwritten by word i */
    for (uint32_t k = 0; k < p; k++) {
        uint32_t *bk = b + bw * k, *cur = x, *nxt = y;
        memcpy(cur, bk, bw * 4);
        for (uint64_t i = 0; i < N; i++) {
            memcpy(v + bw * (size_t)i, cur, bw * 4);
            blockmix(cur, nxt, r);
            uint32_t *t = cur; cur = nxt; nxt = t;
        }
        for (uint64_t i = 0; i < N; i++) {
            const uint64_t j = (((uint64_t)cur[(size_t)(2 * r - 1) * 16 + 1] << 32) | cur[(size_t)(2 * r - 1) * 16]) & (N - 1);
            const uint32_t *vj = v + bw * (size_t)j;
            for (size_t m = 0; m < bw; m++) cur[m] ^= vj[m];
            blockmix(cur, nxt, r);
            uint32_t *t = cur; cur = nxt; nxt = t;
        }
        memcpy(bk, cur, bw * 4);
    }
    for (size_t i = 0; i < bw * p; i++) store_le32(bb + 4 * i, b[i]);
    return pz_pbkdf2_sha256(pw, pwlen, bb, bw * 4 * p, 1, out, outlen);
}
