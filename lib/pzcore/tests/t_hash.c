/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_hash.c - SHA-256, double SHA-256, RIPEMD-160 and HASH160: the published test vectors, the messages of every length from 0 to 520
 * bytes against Python's hashlib (as one digest of all the digests), unaligned data, the compression functions on their own. */
#include <pzcore/hash.h>
#include "testutil.h"
#include "vectors.h"

static void check_sha(const char *msg, size_t len, const char *want)
{
    uint8_t d[32];
    char hx[65];
    pz_sha256((const uint8_t *)msg, len, d);
    tu_hex(d, 32, hx);
    CHECK_STR(hx, want);
}

static void check_rmd(const char *msg, size_t len, const char *want)
{
    uint8_t d[20];
    char hx[41];
    pz_ripemd160((const uint8_t *)msg, len, d);
    tu_hex(d, 20, hx);
    CHECK_STR(hx, want);
}

/* the message of length n used by the digest-of-digests checks (gen_vectors.py) */
static void pattern(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 167 + n * 13 + 5) & 0xff); }

static void test_known(void)
{
    check_sha("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_sha("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    check_rmd("", 0, "9c1185a5c5e9fc54612808977ee8f548b2258d31");
    check_rmd("a", 1, "0bdc9d2d256b3ee9daae347be6f4dc835a467ffe");
    check_rmd("abc", 3, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
    check_rmd("message digest", 14, "5d0689ef49d2fae572b881b123a85ffa21595f36");
    check_rmd("abcdefghijklmnopqrstuvwxyz", 26, "f71c27109c692c1b56bbdceb5b9d2865b3708dbc");
    check_rmd("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, "12a053384a9c0c88e405a06c27dcf49ada62eb2b");
    check_rmd("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62, "b0e20b6e3116640286ed3a87a5713079b21f5189");
    check_rmd("12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80, "9b752e45573d4b39f4dbd3323cab82bf63326bfb");
    /* a million 'a' */
    char *mil = malloc(1000000);
    memset(mil, 'a', 1000000);
    check_sha(mil, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check_rmd(mil, 1000000, "52783243c1697bdbe16d37f97f68f08325dc1528");
    free(mil);
    /* double SHA-256 and HASH160 */
    uint8_t d[32], r[20];
    char hx[65];
    pz_sha256d(NULL, 0, d);
    tu_hex(d, 32, hx);
    CHECK_STR(hx, "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
    pz_hash160((const uint8_t *)"abc", 3, r);
    tu_hex(r, 20, hx);
    CHECK_STR(hx, "bb1be98c142444d7a56aa3981c3942a978e4dc33");
}

static void test_lengths(void)
{
    /* every length from 0 to 520 (all the padding cases, up to eight blocks) against hashlib, as the hash of all the hashes */
    uint8_t msg[521], d32[32], d20[20];
    uint8_t *all32 = malloc(521 * 32), *all20 = malloc(521 * 20), *allsd = malloc(521 * 32), *allh = malloc(521 * 20);
    for (size_t n = 0; n <= 520; n++) {
        pattern(msg, n);
        pz_sha256(msg, n, all32 + 32 * n);
        pz_sha256d(msg, n, allsd + 32 * n);
        pz_ripemd160(msg, n, all20 + 20 * n);
        pz_hash160(msg, n, allh + 20 * n);
    }
    char hx[65];
    pz_sha256(all32, 521 * 32, d32); tu_hex(d32, 32, hx); CHECK_STR(hx, DIGEST_OF_DIGESTS_SHA256);
    pz_sha256(allsd, 521 * 32, d32); tu_hex(d32, 32, hx); CHECK_STR(hx, DIGEST_OF_DIGESTS_SHA256D);
    pz_sha256(all20, 521 * 20, d32); tu_hex(d32, 32, hx); CHECK_STR(hx, DIGEST_OF_DIGESTS_RIPEMD160);
    pz_sha256(allh, 521 * 20, d32); tu_hex(d32, 32, hx); CHECK_STR(hx, DIGEST_OF_DIGESTS_HASH160);
    (void)d20;
    free(all32); free(all20); free(allsd); free(allh);
}

static void test_alignment(void)
{
    /* the result does not depend on where the data sits in memory */
    uint8_t buf[300 + 16];
    uint8_t want[32], got[32], want20[20], got20[20];
    for (size_t n = 0; n <= 200; n += 7) {
        pattern(buf, n);
        pz_sha256(buf, n, want);
        pz_ripemd160(buf, n, want20);
        for (size_t off = 1; off < 8; off++) {
            memmove(buf + off, buf, n);
            pz_sha256(buf + off, n, got);
            pz_ripemd160(buf + off, n, got20);
            CHECK(!memcmp(want, got, 32) && !memcmp(want20, got20, 20));
            memmove(buf, buf + off, n);
        }
    }
}

static void test_pub33(void)
{
    for (int it = 0; it < 200; it++) {
        uint8_t x[32], pub[33], a[20], b[20];
        tu_fill(x, 32);
        const uint8_t parity = (uint8_t)tu_rnd();
        pub[0] = (uint8_t)(2 + (parity & 1));
        memcpy(pub + 1, x, 32);
        pz_hash160(pub, 33, a);
        pz_hash160_pub33(parity, x, b);
        CHECK(!memcmp(a, b, 20));
    }
    /* parity bytes 0..255 */
    uint8_t x[32] = {0}, h0[20], h1[20], h[20];
    pz_hash160_pub33(0, x, h0);
    pz_hash160_pub33(1, x, h1);
    CHECK(memcmp(h0, h1, 20) != 0);
    for (int p = 0; p < 256; p++) {
        pz_hash160_pub33((uint8_t)p, x, h);
        CHECK(!memcmp(h, (p & 1) ? h1 : h0, 20));
    }
}

static void test_compress(void)
{
    /* SHA-256 of "abc" as one block */
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32_t w[16] = {0x61626380, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24};
    pz_sha256_compress(st, w);
    CHECK(st[0] == 0xba7816bf && st[1] == 0x8f01cfea && st[2] == 0x414140de && st[3] == 0x5dae2223 && st[4] == 0xb00361a3 &&
          st[5] == 0x96177a9c && st[6] == 0xb410ff61 && st[7] == 0xf20015ad);
    /* RIPEMD-160 of "abc" as one block (little-endian words) */
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint32_t x[16] = {0x80636261, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0};
    pz_ripemd160_compress(h, x);
    uint8_t out[20];
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (8 * j));
    char hx[41];
    tu_hex(out, 20, hx);
    CHECK_STR(hx, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
}

int main(void)
{
    test_known();
    test_lengths();
    test_alignment();
    test_pub33();
    test_compress();
    TEST_DONE("t_hash");
}
