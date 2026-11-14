/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* u256.c - parsing and printing of 256-bit integers, and the group-order arithmetic. */
#include <pzcore/u256.h>
#include <string.h>

const pz_u256 pz_secp256k1_n = {{0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL}};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pz_u256_from_hex(pz_u256 *r, const char *s)
{
    if (!s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = 0;
    while (s[n] && n <= 64) n++;
    if (n == 0 || n > 64) return false;
    pz_u256 v = pz_u256_zero();
    for (size_t i = 0; i < n; i++) {
        const int d = hexval(s[i]);
        if (d < 0) return false;
        v = pz_u256_shl(&v, 4);
        v.w[0] |= (uint64_t)d;
    }
    *r = v;
    return true;
}

char *pz_u256_to_hex(const pz_u256 *a, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    char full[64];
    for (int i = 0; i < 64; i++) full[63 - i] = digits[(a->w[i >> 4] >> ((i & 15) * 4)) & 15];
    int start = 0;
    while (start < 63 && full[start] == '0') start++;
    memcpy(buf, full + start, (size_t)(64 - start));
    buf[64 - start] = 0;
    return buf;
}

char *pz_u256_to_dec(const pz_u256 *a, char *buf)
{
    /* nineteen digits at a time: 10^19 is the largest power of ten that fits in 64 bits */
    const uint64_t chunk_div = 10000000000000000000ULL;
    pz_u256 t = *a;
    char tmp[80];
    int n = 0;
    if (pz_u256_is_zero(&t)) tmp[n++] = '0';
    while (!pz_u256_is_zero(&t)) {
        pz_u128 rem = 0;
        for (int i = 3; i >= 0; i--) {
            const pz_u128 cur = rem << 64 | t.w[i];
            t.w[i] = (uint64_t)(cur / chunk_div);
            rem = cur % chunk_div;
        }
        uint64_t chunk = (uint64_t)rem;
        for (int j = 0; j < 19; j++) {
            tmp[n++] = (char)('0' + chunk % 10);
            chunk /= 10;
            if (chunk == 0 && pz_u256_is_zero(&t)) break;   /* the last chunk has no leading zeros */
        }
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}

long double pz_u256_to_ld(const pz_u256 *a)
{
    long double r = 0;
    for (int i = 3; i >= 0; i--) r = r * 18446744073709551616.0L + (long double)a->w[i];
    return r;
}

void pz_u256_addmod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    uint64_t c = pz_u256_add(r, a, b);
    if (c || pz_u256_cmp(r, &pz_secp256k1_n) >= 0) pz_u256_sub(r, r, &pz_secp256k1_n);
}

void pz_u256_submod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    if (pz_u256_sub(r, a, b)) pz_u256_add(r, r, &pz_secp256k1_n);
}

/* The product into 8 limbs, then folded with 2^256 = c (mod n), c = 2^256 - n of 129 bits, until it fits in 4 limbs (three folds at
 * most), then n taken off while it is not below n. Not constant-time: for public scalars (the step of a search). */
void pz_u256_mulmod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    static const uint64_t C[3] = {0x402DA1732FC9BEBFULL, 0x4551231950B75FC4ULL, 1};
    uint64_t x[8] = {0};
    for (int i = 0; i < 4; i++) {
        pz_u128 carry = 0;
        for (int j = 0; j < 4; j++) {
            carry += (pz_u128)a->w[i] * b->w[j] + x[i + j];
            x[i + j] = (uint64_t)carry, carry >>= 64;
        }
        x[i + 4] = (uint64_t)carry;
    }
    while (x[4] | x[5] | x[6] | x[7]) {
        uint64_t y[8] = {x[0], x[1], x[2], x[3], 0, 0, 0, 0};
        for (int i = 4; i < 8; i++) {                           /* y += x[i] * c * 2^(64 (i - 4)) */
            pz_u128 carry = 0;
            int k = i - 4;
            for (int j = 0; j < 3; j++, k++) {
                carry += (pz_u128)x[i] * C[j] + y[k];
                y[k] = (uint64_t)carry, carry >>= 64;
            }
            for (; carry && k < 8; k++) carry += y[k], y[k] = (uint64_t)carry, carry >>= 64;
        }
        memcpy(x, y, sizeof x);
    }
    pz_u256 t = {{x[0], x[1], x[2], x[3]}};
    while (pz_u256_cmp(&t, &pz_secp256k1_n) >= 0) pz_u256_sub(&t, &t, &pz_secp256k1_n);
    *r = t;
}

void pz_u256_invmod_n(pz_u256 *r, const pz_u256 *a)
{
    pz_u256 e = pz_secp256k1_n, acc = pz_u256_from_u64(1), base = *a;
    pz_u256_sub(&e, &e, &(pz_u256){{2, 0, 0, 0}});              /* a^(n - 2) = 1 / a: n is prime */
    for (int i = 0; i < 256; i++) {
        if (e.w[i / 64] >> (i % 64) & 1) pz_u256_mulmod_n(&acc, &acc, &base);
        pz_u256_mulmod_n(&base, &base, &base);
    }
    *r = acc;
}
