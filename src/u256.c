/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* u256.c - 256-bit helpers and group-order arithmetic. */
#include "u256.h"

const u256 SECP_N = {{0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL}};

bool u256_from_hex(u256 *r, const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = strlen(s);
    if (n == 0 || n > 64) return false;
    char pad[65];
    memset(pad, '0', 64 - n);
    memcpy(pad + 64 - n, s, n);
    pad[64] = 0;
    u8 be[32];
    if (pz_hex_decode(pad, be, 32) != 32) return false;
    u256_from_be(r, be);
    return true;
}

void u256_to_be(const u256 *a, u8 out[32])
{
    for (int i = 0; i < 4; i++) {
        u64 v = a->w[3 - i];
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (u8)(v >> (56 - 8 * j));
    }
}

void u256_from_be(u256 *r, const u8 in[32])
{
    for (int i = 0; i < 4; i++) {
        u64 v = 0;
        for (int j = 0; j < 8; j++) v = v << 8 | in[i * 8 + j];
        r->w[3 - i] = v;
    }
}

char *u256_to_hex(const u256 *a, char *buf)
{
    u8 be[32];
    char full[65];
    u256_to_be(a, be);
    pz_hex_encode(be, 32, full);
    const char *p = full;
    while (*p == '0' && p[1]) p++;
    strcpy(buf, p);
    return buf;
}

char *u256_to_dec(const u256 *a, char *buf)
{
    u256 t = *a;
    char tmp[100];
    int n = 0;
    if (u256_is_zero(&t)) tmp[n++] = '0';
    while (!u256_is_zero(&t)) {
        u128 rem = 0;
        for (int i = 3; i >= 0; i--) {
            u128 cur = rem << 64 | t.w[i];
            t.w[i] = (u64)(cur / 10);
            rem = cur % 10;
        }
        tmp[n++] = (char)('0' + (int)rem);
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}

long double u256_to_ld(const u256 *a)
{
    long double r = 0;
    for (int i = 3; i >= 0; i--) r = r * 18446744073709551616.0L + (long double)a->w[i];
    return r;
}

void u256_addmod_n(u256 *r, const u256 *a, const u256 *b)
{
    u64 c = u256_add(r, a, b);
    if (c || u256_cmp(r, &SECP_N) >= 0) u256_sub(r, r, &SECP_N);
}

void u256_submod_n(u256 *r, const u256 *a, const u256 *b)
{
    if (u256_sub(r, a, b)) u256_add(r, r, &SECP_N);
}
