/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_kdf.c - HMAC-SHA256 (RFC 4231), HKDF-SHA256 (RFC 5869), PBKDF2-HMAC-SHA256 (RFC 7914 and the usual vectors), scrypt (RFC 7914),
 * the comparison in constant time and the wipe. The vectors are the published ones; gen_seal_vectors.py checks the PBKDF2 and scrypt ones
 * against hashlib. */
#include <pzcore/seal.h>
#include "testutil.h"

static void hexof(const uint8_t *p, size_t n, char *out) { tu_hex(p, n, out); }

static void check_hmac(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, const char *want, size_t cmp_chars)
{
    uint8_t d[32];
    char hx[65];
    pz_hmac_sha256_oneshot(key, kl, msg, ml, d);
    hexof(d, 32, hx);
    CHECK(strncmp(hx, want, cmp_chars) == 0);
    /* the same in pieces of 1 to 70 bytes */
    for (size_t step = 1; step < 71; step += 13) {
        pz_hmac_sha256 c;
        pz_hmac_sha256_init(&c, key, kl);
        for (size_t o = 0; o < ml; o += step) pz_hmac_sha256_update(&c, msg + o, ml - o < step ? ml - o : step);
        pz_hmac_sha256_final(&c, d);
        hexof(d, 32, hx);
        CHECK(strncmp(hx, want, cmp_chars) == 0);
    }
}

static void test_hmac(void)
{
    uint8_t key[131], data[160];
    memset(key, 0x0b, 20);
    check_hmac(key, 20, (const uint8_t *)"Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 64);
    check_hmac((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28,
               "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 64);
    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    check_hmac(key, 20, data, 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 64);
    for (int i = 0; i < 25; i++) key[i] = (uint8_t)(i + 1);
    memset(data, 0xcd, 50);
    check_hmac(key, 25, data, 50, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 64);
    memset(key, 0x0c, 20);
    check_hmac(key, 20, (const uint8_t *)"Test With Truncation", 20, "a3b6167473100ee06e0c796c2955552b", 32);
    memset(key, 0xaa, 131);                              /* keys longer than the block are hashed first */
    check_hmac(key, 131, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54,
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 64);
    const char *m7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    check_hmac(key, 131, (const uint8_t *)m7, strlen(m7), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", 64);
    /* an empty key and an empty message are valid */
    uint8_t d[32];
    pz_hmac_sha256_oneshot(NULL, 0, NULL, 0, d);
    char hx[65];
    hexof(d, 32, hx);
    CHECK_STR(hx, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

static void check_hkdf(const uint8_t *salt, size_t sl, const uint8_t *ikm, size_t il, const uint8_t *info, size_t nl, size_t len, const char *want)
{
    uint8_t okm[128];
    char hx[300];
    CHECK(pz_hkdf_sha256(salt, sl, ikm, il, info, nl, okm, len));
    hexof(okm, len, hx);
    CHECK_STR(hx, want);
}

static void test_hkdf(void)
{
    uint8_t ikm[80], salt[80], info[80];
    memset(ikm, 0x0b, 22);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    check_hkdf(salt, 13, ikm, 22, info, 10, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    for (int i = 0; i < 80; i++) { ikm[i] = (uint8_t)i; salt[i] = (uint8_t)(0x60 + i); info[i] = (uint8_t)(0xb0 + i); }
    check_hkdf(salt, 80, ikm, 80, info, 80, 82,
               "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87");
    memset(ikm, 0x0b, 22);
    check_hkdf(NULL, 0, ikm, 22, NULL, 0, 42, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    uint8_t big[PZ_HKDF_SHA256_MAX_OUT + 1];
    CHECK(pz_hkdf_sha256(NULL, 0, ikm, 22, NULL, 0, big, PZ_HKDF_SHA256_MAX_OUT));
    CHECK(!pz_hkdf_sha256(NULL, 0, ikm, 22, NULL, 0, big, PZ_HKDF_SHA256_MAX_OUT + 1));
}

static void check_pbkdf2(const char *pw, const char *salt, uint32_t c, size_t len, const char *want)
{
    uint8_t out[64];
    char hx[129];
    CHECK(pz_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), (const uint8_t *)salt, strlen(salt), c, out, len));
    hexof(out, len, hx);
    CHECK_STR(hx, want);
}

static void test_pbkdf2(void)
{
    check_pbkdf2("password", "salt", 1, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    check_pbkdf2("password", "salt", 2, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    check_pbkdf2("password", "salt", 4096, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    check_pbkdf2("passwd", "salt", 1, 64,
                 "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    check_pbkdf2("Password", "NaCl", 80000, 64,
                 "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
    uint8_t o[4];
    CHECK(!pz_pbkdf2_sha256((const uint8_t *)"p", 1, (const uint8_t *)"s", 1, 0, o, 4));       /* zero iterations */
}

static void check_scrypt(const char *pw, const char *salt, uint64_t N, uint32_t r, uint32_t p, const char *want)
{
    uint8_t out[64];
    char hx[129];
    const size_t ws = pz_scrypt_work_size(N, r, p);
    CHECK(ws == (size_t)128 * r * (N + p + 2));
    void *work = malloc(ws);
    CHECK(work && pz_scrypt((const uint8_t *)pw, strlen(pw), (const uint8_t *)salt, strlen(salt), N, r, p, out, 64, work, ws));
    hexof(out, 64, hx);
    CHECK_STR(hx, want);
    if (work) {
        /* a buffer that is one byte short is refused */
        uint8_t keep[64];
        memcpy(keep, out, 64);
        memset(out, 0xee, 64);
        CHECK(!pz_scrypt((const uint8_t *)pw, strlen(pw), (const uint8_t *)salt, strlen(salt), N, r, p, out, 64, work, ws - 1));
        CHECK(out[0] == 0xee && out[63] == 0xee);
    }
    free(work);
}

static void test_scrypt(void)
{
    check_scrypt("", "", 16, 1, 1,
                 "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
    check_scrypt("password", "NaCl", 1024, 8, 16,
                 "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
    check_scrypt("pleaseletmein", "SodiumChloride", 16384, 8, 1,
                 "7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887");
    /* parameters that are not valid */
    CHECK(pz_scrypt_work_size(0, 8, 1) == 0 && pz_scrypt_work_size(1, 8, 1) == 0 && pz_scrypt_work_size(24, 8, 1) == 0);
    CHECK(pz_scrypt_work_size(16, 0, 1) == 0 && pz_scrypt_work_size(16, 1, 0) == 0 && pz_scrypt_work_size(16, 1u << 20, 1u << 10) == 0);
    CHECK(pz_scrypt_work_size(1ULL << 62, 8, 1) == 0);
    CHECK(pz_scrypt_work_size(1ULL << 18, 8, 1) == (size_t)128 * 8 * ((1u << 18) + 3));
}

static void test_misc(void)
{
    uint8_t a[40], b[40];
    for (int i = 0; i < 40; i++) a[i] = b[i] = (uint8_t)(i * 7);
    CHECK(pz_ct_equal(a, b, 40) && pz_ct_equal(a, b, 0));
    for (int i = 0; i < 40; i++) {
        b[i] ^= 0x80;
        CHECK(!pz_ct_equal(a, b, 40));
        CHECK(i == 39 || pz_ct_equal(a, b, (size_t)i));          /* only the first i bytes: equal */
        b[i] ^= 0x80;
    }
    /* the wipe: through a pointer the compiler cannot see through, the bytes are zero afterwards */
    uint8_t *vol = malloc(64);
    memset(vol, 0xa5, 64);
    pz_secure_zero(vol, 64);
    int all0 = 1;
    for (int i = 0; i < 64; i++) all0 &= vol[i] == 0;
    CHECK(all0);
    free(vol);
    pz_secure_zero(NULL, 0);
    pz_secure_zero(NULL, 5);
}

int main(void)
{
    test_hmac();
    test_hkdf();
    test_pbkdf2();
    test_scrypt();
    test_misc();
    TEST_DONE("t_kdf");
}
