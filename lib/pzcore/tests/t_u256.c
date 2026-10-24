/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_u256.c - 256-bit integers: text conversion against Python's integers, arithmetic against the compiler's 128-bit integers where the
 * values fit and by identities where they do not, the edges of the shifts, and the refusal of hostile text. */
#include <pzcore/u256.h>
#include "testutil.h"
#include "vectors.h"

/* a random value of at most `bits` bits */
static pz_u256 rnd256(unsigned bits)
{
    pz_u256 x = pz_u256_zero();
    for (int i = 0; i < 4; i++) x.w[i] = tu_rnd();
    if (bits < 256) {
        pz_u256 mask = pz_u256_pow2(bits), one = pz_u256_from_u64(1);
        pz_u256_sub(&mask, &mask, &one);
        for (int i = 0; i < 4; i++) x.w[i] &= mask.w[i];
    }
    return x;
}

static pz_u128 as128(const pz_u256 *a) { return (pz_u128)a->w[0] | (pz_u128)a->w[1] << 64; }
static unsigned bits128(pz_u128 x) { unsigned b = 0; while (b < 128 && (x >> b)) b++; return b; }

static void test_text(void)
{
    char buf[PZ_U256_DEC_BUF + 8], hbuf[PZ_U256_HEX_BUF + 8];
    for (size_t i = 0; i < sizeof U256_VEC / sizeof *U256_VEC; i++) {
        pz_u256 v, w;
        char t[160];
        CHECK(pz_u256_from_hex(&v, U256_VEC[i].hex));
        CHECK_STR(pz_u256_to_dec(&v, buf), U256_VEC[i].dec);
        CHECK_STR(pz_u256_to_hex(&v, hbuf), U256_VEC[i].hex);
        /* the same value with 0x, in upper case, with leading zeros */
        snprintf(t, sizeof t, "0x%s", U256_VEC[i].hex);
        CHECK(pz_u256_from_hex(&w, t) && pz_u256_cmp(&v, &w) == 0);
        snprintf(t, sizeof t, "0X%s", U256_VEC[i].hex);
        for (char *p = t + 2; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
        CHECK(pz_u256_from_hex(&w, t) && pz_u256_cmp(&v, &w) == 0);
        const size_t n = strlen(U256_VEC[i].hex);
        if (n < 64) {
            memset(t, '0', 64 - n);
            strcpy(t + 64 - n, U256_VEC[i].hex);
            CHECK(pz_u256_from_hex(&w, t) && pz_u256_cmp(&v, &w) == 0);
        }
    }
    /* the longest texts */
    {
        pz_u256 all = pz_u256_zero(), one = pz_u256_from_u64(1);
        pz_u256_sub(&all, &all, &one);
        CHECK_STR(pz_u256_to_hex(&all, hbuf), "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        CHECK_STR(pz_u256_to_dec(&all, buf), "115792089237316195423570985008687907853269984665640564039457584007913129639935");
        CHECK(strlen(buf) == 78);
    }
    /* hostile text is refused and the result left alone */
    {
        const char *bad[] = {NULL, "", "0x", "0X", "g", "0xg", " 1", "1 ", "-1", "+1", "0x0x1", "1.5", "1\n", "\xff", "0x ",
                             "10000000000000000000000000000000000000000000000000000000000000000",                       /* 65 digits */
                             "0x10000000000000000000000000000000000000000000000000000000000000000",
                             "00000000000000000000000000000000000000000000000000000000000000000"};                       /* 65 zeros */
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            pz_u256 v = pz_u256_from_u64(0x1234);
            CHECK(!pz_u256_from_hex(&v, bad[i]));
            CHECK(v.w[0] == 0x1234 && !v.w[1] && !v.w[2] && !v.w[3]);
        }
        pz_u256 v;
        CHECK(pz_u256_from_hex(&v, "0000000000000000000000000000000000000000000000000000000000000001") && v.w[0] == 1);   /* 64 digits */
        CHECK(pz_u256_from_hex(&v, "0x0000000000000000000000000000000000000000000000000000000000000001") && v.w[0] == 1);
    }
    /* big-endian bytes */
    {
        pz_u256 v = pz_u256_from_u64(1), w;
        uint8_t be[32];
        pz_u256_to_be(&v, be);
        for (int i = 0; i < 31; i++) CHECK(be[i] == 0);
        CHECK(be[31] == 1);
        for (int it = 0; it < 100; it++) {
            tu_fill(be, 32);
            pz_u256_from_be(&w, be);
            uint8_t back[32];
            pz_u256_to_be(&w, back);
            CHECK(!memcmp(be, back, 32));
        }
    }
}

static void test_arith(void)
{
    /* against unsigned __int128, on values of up to 128 bits */
    for (int it = 0; it < 20000; it++) {
        pz_u256 a = rnd256(1 + (unsigned)(tu_rnd() % 128)), b = rnd256(1 + (unsigned)(tu_rnd() % 128)), r;
        const pz_u128 x = as128(&a), y = as128(&b);
        CHECK(pz_u256_cmp(&a, &b) == (x < y ? -1 : x > y ? 1 : 0));

        CHECK(pz_u256_add(&r, &a, &b) == 0);                                  /* two 128-bit values do not carry out of 256 bits */
        CHECK(as128(&r) == (pz_u128)(x + y) && r.w[2] == (uint64_t)((pz_u128)(x + y) < x) && r.w[3] == 0);
        const uint64_t br = pz_u256_sub(&r, &a, &b);
        if (x >= y) { CHECK(!br && as128(&r) == x - y && !r.w[2] && !r.w[3]); }
        else { CHECK(br == 1 && as128(&r) == (pz_u128)(x - y) && r.w[2] == ~0ULL && r.w[3] == ~0ULL); }

        const unsigned s = (unsigned)(tu_rnd() % 140);
        r = pz_u256_shl(&a, s);
        CHECK(as128(&r) == (s >= 128 ? 0 : (pz_u128)(x << s)));
        if (s > 0 && s < 128) {                                               /* the bits that left the low half are in the high half */
            const pz_u128 hi = x >> (128 - s);
            CHECK(r.w[2] == (uint64_t)hi && r.w[3] == (uint64_t)(hi >> 64));
        }
        r = pz_u256_shr(&a, s);
        CHECK(as128(&r) == (s >= 128 ? 0 : x >> s) && !r.w[2] && !r.w[3]);
        CHECK(pz_u256_bits(&a) == bits128(x));
        for (unsigned i = 0; i < 140; i += 7) CHECK(pz_u256_bit(&a, i) == (i < 128 ? (bool)((x >> i) & 1) : false));

        const unsigned small = 1 + (unsigned)(tu_rnd() % 96);
        pz_u256 sm = rnd256(small);
        const uint32_t m = (uint32_t)tu_rnd();
        r = pz_u256_mul_u32(&sm, m);
        CHECK(as128(&r) == as128(&sm) * m && !r.w[2] && !r.w[3]);

        const uint32_t d = (uint32_t)(tu_rnd() | 1);
        uint32_t rem = 0;
        r = pz_u256_divmod_u32(&a, d, &rem);
        CHECK(as128(&r) == x / d && rem == (uint32_t)(x % d));
        r = pz_u256_divmod_u32(&a, d, NULL);
        CHECK(as128(&r) == x / d);

        CHECK(pz_u256_add_u64(&r, &a, 0xffffffffffffffffULL) == 0 && as128(&r) == x + 0xffffffffffffffffULL);
    }
    /* full width: identities */
    for (int it = 0; it < 5000; it++) {
        pz_u256 a = rnd256(256), b = rnd256(1 + (unsigned)(tu_rnd() % 256)), s, t;
        const uint64_t c = pz_u256_add(&s, &a, &b);
        const uint64_t br = pz_u256_sub(&t, &s, &b);
        CHECK(pz_u256_cmp(&t, &a) == 0 && c == br);                           /* it wraps back with the same carry */
        CHECK(c == (uint64_t)(pz_u256_cmp(&s, &a) < 0));
        pz_u256 u = a;
        pz_u256_add(&u, &u, &b);                                              /* the output may be an input */
        CHECK(pz_u256_cmp(&u, &s) == 0);
        pz_u256_sub(&u, &u, &b);
        CHECK(pz_u256_cmp(&u, &a) == 0);

        const unsigned sh = (unsigned)(tu_rnd() % 256);                       /* shl then shr keeps the low 256 - sh bits */
        t = pz_u256_shl(&a, sh);
        t = pz_u256_shr(&t, sh);
        pz_u256 mask = pz_u256_pow2(256 - sh), one = pz_u256_from_u64(1);
        if (sh == 0) mask = pz_u256_zero();
        pz_u256_sub(&mask, &mask, &one);                                      /* 2^(256-sh) - 1, all ones for sh == 0 */
        for (int i = 0; i < 4; i++) CHECK(t.w[i] == (a.w[i] & mask.w[i]));

        const unsigned bl = pz_u256_bits(&a);
        CHECK(bl == 0 || pz_u256_bit(&a, bl - 1));
        CHECK(!pz_u256_bit(&a, bl));
    }
    /* the edges of the shifts, bit and pow2 */
    {
        pz_u256 one = pz_u256_from_u64(1), r, top = pz_u256_pow2(255);
        const unsigned edge[] = {0, 1, 63, 64, 65, 127, 128, 129, 191, 192, 255, 256, 257, 1000, 0xffffffffu};
        for (size_t i = 0; i < sizeof edge / sizeof *edge; i++) {
            const unsigned s = edge[i];
            r = pz_u256_shl(&one, s);
            if (s < 256) { CHECK(pz_u256_bits(&r) == s + 1 && pz_u256_bit(&r, s)); }
            else CHECK(pz_u256_is_zero(&r));
            pz_u256 p = pz_u256_pow2(s);
            CHECK(pz_u256_cmp(&p, &r) == 0);
            r = pz_u256_shr(&top, s);
            if (s <= 255) { CHECK(pz_u256_bit(&r, 255 - s) && pz_u256_bits(&r) == 256 - s); }
            else CHECK(pz_u256_is_zero(&r));
            CHECK(pz_u256_bit(&top, s) == (s == 255));
        }
        pz_u256 zero = pz_u256_zero();
        CHECK(pz_u256_bits(&zero) == 0 && pz_u256_is_zero(&zero) && !pz_u256_bit(&zero, 0));
        CHECK(!pz_u256_is_zero(&one) && !pz_u256_is_zero(&top));
    }
    CHECK(pz_u256_to_ld(&(pz_u256){{0, 1, 0, 0}}) == 18446744073709551616.0L);
}

static void test_mod_n(void)
{
    const pz_u256 n = pz_secp256k1_n;
    pz_u256 one = pz_u256_from_u64(1), nm1, nm2, r, zero = pz_u256_zero();
    pz_u256_sub(&nm1, &n, &one);
    pz_u256_sub(&nm2, &nm1, &one);
    char hb[PZ_U256_HEX_BUF];
    CHECK_STR(pz_u256_to_hex(&n, hb), "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    pz_u256_addmod_n(&r, &nm1, &one);              CHECK(pz_u256_is_zero(&r));
    pz_u256_addmod_n(&r, &nm1, &nm1);              CHECK(pz_u256_cmp(&r, &nm2) == 0);
    pz_u256_addmod_n(&r, &zero, &zero);            CHECK(pz_u256_is_zero(&r));
    pz_u256_submod_n(&r, &zero, &one);             CHECK(pz_u256_cmp(&r, &nm1) == 0);
    pz_u256_submod_n(&r, &one, &one);              CHECK(pz_u256_is_zero(&r));
    pz_u256_submod_n(&r, &zero, &nm1);             CHECK(pz_u256_cmp(&r, &one) == 0);
    for (int it = 0; it < 5000; it++) {
        pz_u256 a = rnd256(256), b = rnd256(256), s, d;
        if (pz_u256_cmp(&a, &n) >= 0) pz_u256_sub(&a, &a, &n);
        if (pz_u256_cmp(&b, &n) >= 0) pz_u256_sub(&b, &b, &n);
        pz_u256_addmod_n(&s, &a, &b);
        CHECK(pz_u256_cmp(&s, &n) < 0);
        pz_u256_submod_n(&d, &s, &b);
        CHECK(pz_u256_cmp(&d, &a) == 0);
        pz_u256_submod_n(&d, &a, &b);
        CHECK(pz_u256_cmp(&d, &n) < 0);
        pz_u256_addmod_n(&d, &d, &b);                                          /* the output may be an input */
        CHECK(pz_u256_cmp(&d, &a) == 0);
    }
}

int main(void)
{
    test_text();
    test_arith();
    test_mod_n();
    TEST_DONE("t_u256");
}
