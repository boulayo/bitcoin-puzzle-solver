/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_seal.c - k*P for any point, the seal and its opening against the vectors of gen_seal_vectors.py (an independent Python
 * implementation), every byte of the payload damaged in turn, wrong keys, wrong associated data, hostile points, the text form, and the key
 * of a password (the real cost, N = 2^18, once per vector: set PZ_TEST_SEAL_FAST=1 to skip those two, as the slow runs under Valgrind do). */
#include <pzcore/seal.h>
#include <pzcore/address.h>
#include "testutil.h"
#include "seal_vectors.h"

static void u256_of_hex(pz_u256 *r, const char *hex) { CHECK(pz_u256_from_hex(r, hex)); }

static void point_of_hex(pz_ge *p, const char *hex)
{
    uint8_t b[33];
    CHECK(tu_unhex(hex, b, 33) == 33);
    CHECK(pz_ge_from_pubkey(p, b, 33));
}

static void test_mul(void)
{
    for (size_t i = 0; i < sizeof MUL_VEC / sizeof *MUL_VEC; i++) {
        pz_ge p, r;
        pz_u256 k;
        uint8_t b[33];
        char hx[67];
        point_of_hex(&p, MUL_VEC[i].p_hex);
        u256_of_hex(&k, MUL_VEC[i].k_hex);
        const bool ok = pz_ec_mul(&r, &p, &k);
        CHECK(ok == (MUL_VEC[i].r_hex[0] != 0));
        if (ok) {
            pz_ge_to_pub33(b, &r);
            tu_hex(b, 33, hx);
            CHECK_STR(hx, MUL_VEC[i].r_hex);
        }
    }
    /* G itself, a sum, zero, and the order */
    pz_ge a, b, ab, s, t;
    pz_u256 x = {{0x1234567890abcdefULL, 0x0fedcba098765432ULL, 7, 0}}, y = {{99, 0, 0, 5}}, xy;
    CHECK(pz_ec_mul(&a, &pz_ec_g, &x) && pz_ec_mul_g(&t, &x) && pz_ge_equal(&a, &t));
    pz_ec_mul_g(&a, &x);
    pz_ec_mul_g(&b, &y);
    pz_u256_addmod_n(&xy, &x, &y);
    pz_ec_mul_g(&ab, &xy);
    CHECK(pz_ge_add(&s, &a, &b) && pz_ge_equal(&s, &ab));
    CHECK(!pz_ec_mul(&t, &a, &(pz_u256){{0, 0, 0, 0}}));
    CHECK(!pz_ec_mul(&t, &a, &pz_secp256k1_n));
}

static void test_scalar(void)
{
    uint8_t b[32];
    pz_u256 d, v;
    char hx[65];
    memset(b, 0, 32);
    pz_seal_scalar_from_bytes(&d, b);
    CHECK(d.w[0] == 1 && !d.w[1] && !d.w[2] && !d.w[3]);
    pz_u256_sub(&v, &pz_secp256k1_n, &(pz_u256){{2, 0, 0, 0}});                  /* n - 2 -> n - 1 */
    pz_u256_to_be(&v, b);
    pz_seal_scalar_from_bytes(&d, b);
    pz_u256_to_hex(&d, hx);
    CHECK_STR(hx, "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140");
    pz_u256_sub(&v, &pz_secp256k1_n, &(pz_u256){{1, 0, 0, 0}});                  /* n - 1 -> 1 */
    pz_u256_to_be(&v, b);
    pz_seal_scalar_from_bytes(&d, b);
    CHECK(d.w[0] == 1 && !d.w[1] && !d.w[2] && !d.w[3]);
    memset(b, 0xff, 32);                                                           /* 2^256 - 1 - (n - 1) + 1 */
    pz_seal_scalar_from_bytes(&d, b);
    pz_u256_to_hex(&d, hx);
    CHECK_STR(hx, "14551231950b75fc4402da1732fc9bec0");
}

static int hexbytes(const char *hex, uint8_t *out, size_t max) { return hex[0] ? tu_unhex(hex, out, max) : 0; }

static void test_vectors(void)
{
    for (size_t i = 0; i < sizeof SEAL_VEC / sizeof *SEAL_VEC; i++) {
        pz_u256 d, r;
        pz_ge q;
        uint8_t plain[32], aad[PZ_SEAL_AAD_MAX], pay[PZ_SEAL_PAYLOAD_LEN], want[PZ_SEAL_PAYLOAD_LEN], back[32];
        char hx[2 * PZ_SEAL_PAYLOAD_LEN + 1], text[PZ_SEAL_TEXT_BUF];
        u256_of_hex(&d, SEAL_VEC[i].d_hex);
        u256_of_hex(&r, SEAL_VEC[i].r_hex);
        CHECK(pz_ec_mul_g(&q, &d));
        CHECK(tu_unhex(SEAL_VEC[i].plain_hex, plain, 32) == 32);
        const int al = hexbytes(SEAL_VEC[i].aad_hex, aad, sizeof aad);
        CHECK(al >= 0);
        CHECK(pz_seal(pay, &q, plain, aad, (size_t)al, &r) == PZ_SEAL_OK);
        tu_hex(pay, PZ_SEAL_PAYLOAD_LEN, hx);
        CHECK_STR(hx, SEAL_VEC[i].payload_hex);
        CHECK(tu_unhex(SEAL_VEC[i].payload_hex, want, sizeof want) == PZ_SEAL_PAYLOAD_LEN);
        CHECK(pz_seal_encode(pay, text, sizeof text) > 0);
        CHECK_STR(text, SEAL_VEC[i].text);
        uint8_t dec[PZ_SEAL_PAYLOAD_LEN];
        CHECK(pz_seal_decode(dec, SEAL_VEC[i].text) == PZ_SEAL_OK && !memcmp(dec, pay, sizeof dec));
        CHECK(pz_seal_open(back, want, &d, aad, (size_t)al) == PZ_SEAL_OK && !memcmp(back, plain, 32));
    }
}

static void test_roundtrip_and_damage(void)
{
    pz_u256 d, r;
    pz_ge q;
    uint8_t plain[32], back[32], pay[PZ_SEAL_PAYLOAD_LEN], aad[] = "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH\0" "71";
    const size_t al = sizeof aad - 1;
    for (int it = 0; it < 12; it++) {
        uint8_t b[32];
        tu_fill(b, 32);
        pz_seal_scalar_from_bytes(&d, b);
        tu_fill(b, 32);
        pz_seal_scalar_from_bytes(&r, b);
        tu_fill(plain, 32);
        CHECK(pz_ec_mul_g(&q, &d));
        CHECK(pz_seal(pay, &q, plain, aad, al, &r) == PZ_SEAL_OK);
        memset(back, 0, 32);
        CHECK(pz_seal_open(back, pay, &d, aad, al) == PZ_SEAL_OK && !memcmp(back, plain, 32));
        /* a different r gives a different payload that opens to the same secret */
        uint8_t pay2[PZ_SEAL_PAYLOAD_LEN];
        pz_u256 r2;
        pz_u256_addmod_n(&r2, &r, &(pz_u256){{1, 0, 0, 0}});
        CHECK(pz_seal(pay2, &q, plain, aad, al, &r2) == PZ_SEAL_OK && memcmp(pay, pay2, sizeof pay) != 0);
        CHECK(pz_seal_open(back, pay2, &d, aad, al) == PZ_SEAL_OK && !memcmp(back, plain, 32));
    }
    /* one bit of each byte changed in turn */
    uint8_t keep[32];
    for (int i = 0; i < PZ_SEAL_PAYLOAD_LEN; i++) {
        for (int bit = 0; bit < 8; bit += 7) {
            uint8_t bad[PZ_SEAL_PAYLOAD_LEN];
            memcpy(bad, pay, sizeof bad);
            bad[i] ^= (uint8_t)(1 << bit);
            memset(back, 0x5a, 32);
            memcpy(keep, back, 32);
            const pz_seal_status st = pz_seal_open(back, bad, &d, aad, al);
            CHECK(st != PZ_SEAL_OK && !memcmp(back, keep, 32));                 /* the output is not written on failure */
            if (i == 0) CHECK(st == PZ_SEAL_E_VERSION);
            else if (i < 5) CHECK(st == PZ_SEAL_E_KEYID);
            else if (i < 38) CHECK(st == PZ_SEAL_E_POINT || st == PZ_SEAL_E_TAG);
            else CHECK(st == PZ_SEAL_E_TAG);
            if (i == 5 && bit == 0) CHECK(st == PZ_SEAL_E_TAG);                 /* 02 <-> 03: the opposite point is valid, the tag catches it */
            if (i == 5 && bit == 7) CHECK(st == PZ_SEAL_E_POINT);               /* 82 / 83: not a compressed point */
        }
    }
    /* wrong key, wrong associated data, hostile arguments */
    pz_u256 d2;
    pz_u256_addmod_n(&d2, &d, &(pz_u256){{1, 0, 0, 0}});
    CHECK(pz_seal_open(back, pay, &d2, aad, al) == PZ_SEAL_E_KEYID);
    CHECK(pz_seal_open(back, pay, &d, aad, al - 1) == PZ_SEAL_E_TAG);
    uint8_t aad2[sizeof aad];
    memcpy(aad2, aad, sizeof aad);
    aad2[3] ^= 1;
    CHECK(pz_seal_open(back, pay, &d, aad2, al) == PZ_SEAL_E_TAG);
    CHECK(pz_seal_open(back, pay, &d, NULL, 0) == PZ_SEAL_E_TAG);
    CHECK(pz_seal_open(back, pay, &(pz_u256){{0, 0, 0, 0}}, aad, al) == PZ_SEAL_E_ARG);
    CHECK(pz_seal_open(back, pay, &pz_secp256k1_n, aad, al) == PZ_SEAL_E_ARG);
    uint8_t longaad[PZ_SEAL_AAD_MAX + 1];
    memset(longaad, 'a', sizeof longaad);
    CHECK(pz_seal_open(back, pay, &d, longaad, sizeof longaad) == PZ_SEAL_E_ARG);
    CHECK(pz_seal(pay, &q, plain, longaad, sizeof longaad, &r) == PZ_SEAL_E_ARG);
    CHECK(pz_seal(pay, &q, plain, longaad, PZ_SEAL_AAD_MAX, &r) == PZ_SEAL_OK);
    CHECK(pz_seal(pay, &q, plain, aad, al, &(pz_u256){{0, 0, 0, 0}}) == PZ_SEAL_E_ARG);
    CHECK(pz_seal(pay, &q, plain, aad, al, &pz_secp256k1_n) == PZ_SEAL_E_ARG);
    pz_ge off = q;
    pz_fe_set_u64(&off.y, 3);
    CHECK(!pz_ge_on_curve(&off) && pz_seal(pay, &off, plain, aad, al, &r) == PZ_SEAL_E_ARG);
    /* R that is not a point: a small x for which x^3 + 7 has no square root (found by trial), the uncompressed prefix, an x not below p */
    CHECK(pz_seal(pay, &q, plain, aad, al, &r) == PZ_SEAL_OK);
    uint8_t bad[PZ_SEAL_PAYLOAD_LEN];
    int found = 0;
    for (int x = 1; x < 40 && !found; x++) {
        memcpy(bad, pay, sizeof bad);
        memset(bad + 6, 0, 32);
        bad[5 + 32] = (uint8_t)x;
        pz_ge t;
        if (!pz_ge_from_pubkey(&t, bad + 5, 33)) found = 1;
    }
    CHECK(found && pz_seal_open(back, bad, &d, aad, al) == PZ_SEAL_E_POINT && pz_seal_peek(bad, NULL) == PZ_SEAL_E_POINT);
    memcpy(bad, pay, sizeof bad);
    bad[5] = 4;
    CHECK(pz_seal_open(back, bad, &d, aad, al) == PZ_SEAL_E_POINT);
    memset(bad + 6, 0xff, 32);                                                   /* x not below p */
    bad[5] = 2;
    CHECK(pz_seal_open(back, bad, &d, aad, al) == PZ_SEAL_E_POINT);
    uint8_t id[4];
    CHECK(pz_seal_peek(pay, id) == PZ_SEAL_OK && !memcmp(id, pay + 1, 4));
}

static void test_text(void)
{
    uint8_t pay[PZ_SEAL_PAYLOAD_LEN], dec[PZ_SEAL_PAYLOAD_LEN];
    char text[PZ_SEAL_TEXT_BUF];
    tu_fill(pay, sizeof pay);
    pay[0] = PZ_SEAL_VERSION;
    const int n = pz_seal_encode(pay, text, sizeof text);
    CHECK(n > 100 && n < PZ_SEAL_TEXT_BUF && (size_t)n == strlen(text) && !strncmp(text, "PZENC1:", 7));
    CHECK(pz_seal_decode(dec, text) == PZ_SEAL_OK && !memcmp(dec, pay, sizeof dec));
    char t2[PZ_SEAL_TEXT_BUF];
    CHECK(pz_seal_encode(pay, t2, (size_t)n) == -1 && t2[0] == 0);                  /* no room for the NUL */
    CHECK(pz_seal_encode(pay, t2, (size_t)n + 1) == n);
    strcpy(t2, text);
    t2[n - 1] = 0;                                                                   /* cut */
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);
    strcpy(t2, text);
    t2[20] = t2[20] == '2' ? '3' : '2';                                              /* one character changed */
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);
    strcpy(t2, text);
    t2[20] = '0';                                                                    /* not in the alphabet */
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);
    strcpy(t2, text + 1);
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);                              /* no prefix */
    strcpy(t2, "pzenc1:");
    strcat(t2, text + 7);
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);
    CHECK(pz_seal_decode(dec, NULL) == PZ_SEAL_E_FORMAT && pz_seal_decode(dec, "PZENC1:") == PZ_SEAL_E_FORMAT);
    /* a valid Base58Check string of another length */
    uint8_t shortp[40];
    memset(shortp, 3, sizeof shortp);
    strcpy(t2, "PZENC1:");
    CHECK(pz_base58check_encode(shortp, sizeof shortp, t2 + 7, sizeof t2 - 7) > 0);
    CHECK(pz_seal_decode(dec, t2) == PZ_SEAL_E_FORMAT);
}

static void check_pwkey(size_t i, void *work, size_t ws)
{
    pz_u256 d;
    pz_ge q;
    uint8_t b[33];
    char hx[67], id[9];
    CHECK(pz_seal_key_from_password(&d, (const uint8_t *)PWKEY_VEC[i].pw, strlen(PWKEY_VEC[i].pw), PWKEY_VEC[i].log2n,
                                    work, ws));
    pz_u256_to_hex(&d, hx);
    CHECK_STR(hx, PWKEY_VEC[i].d_hex);
    CHECK(pz_ec_mul_g(&q, &d));
    pz_ge_to_pub33(b, &q);
    tu_hex(b, 33, hx);
    CHECK_STR(hx, PWKEY_VEC[i].q_hex);
    uint8_t k4[4];
    pz_seal_key_id(&q, k4);
    tu_hex(k4, 4, id);
    CHECK_STR(id, PWKEY_VEC[i].keyid_hex);
}

static void test_password(void)
{
    const bool fast = tu_env_on("PZ_TEST_SEAL_FAST");
    size_t ws = pz_scrypt_work_size(1ULL << PZ_SEAL_SCRYPT_LOG2N, 8, 1);
    void *work = malloc(ws);
    CHECK(work != NULL);
    for (size_t i = 0; i < sizeof PWKEY_VEC / sizeof *PWKEY_VEC; i++) {
        if (PWKEY_VEC[i].log2n >= 18 && fast) continue;
        if (work) check_pwkey(i, work, ws);
    }
    pz_secure_zero(work, ws);
    free(work);
    pz_u256 d;
    uint8_t buf[8];
    CHECK(!pz_seal_key_from_password(&d, buf, 1, 4, buf, sizeof buf));          /* work buffer too small */
    CHECK(!pz_seal_key_from_password(&d, buf, 1, 0, buf, sizeof buf));
}

int main(void)
{
    test_mul();
    test_scalar();
    test_vectors();
    test_roundtrip_and_damage();
    test_text();
    test_password();
    TEST_DONE("t_seal");
}
