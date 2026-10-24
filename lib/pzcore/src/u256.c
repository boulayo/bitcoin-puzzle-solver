/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* u256.c - parsing and printing of 256-bit integers, and the group-order arithmetic. */
#include <pzcore/u256.h>

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
    pz_u256 t = *a;
    char tmp[100];
    int n = 0;
    if (pz_u256_is_zero(&t)) tmp[n++] = '0';
    while (!pz_u256_is_zero(&t)) {
        pz_u128 rem = 0;
        for (int i = 3; i >= 0; i--) {
            const pz_u128 cur = rem << 64 | t.w[i];
            t.w[i] = (uint64_t)(cur / 10);
            rem = cur % 10;
        }
        tmp[n++] = (char)('0' + (int)rem);
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
