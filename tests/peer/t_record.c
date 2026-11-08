/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_record.c - the keys of a home, the record of a distinguished point, its proof of possession and the points of dp.bin records. The
 * points are built here with libsecp256k1 from known scalars: a puzzle key k, the start of a range, distances d of both signs. */
#include "dpfeed.h"
#include "keys.h"
#include <pzcore/hash.h>
#include <secp256k1_schnorrsig.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0xf0, 0x0d};

static void hashed(const char *s, uint8_t out[32]) { pz_sha256((const uint8_t *)s, strlen(s), out); }

static void pub33(const uint8_t sec[32], uint8_t out[33])
{
    secp256k1_pubkey p;
    size_t n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, sec) && secp256k1_ec_pubkey_serialize(ctx, out, &n, &p, SECP256K1_EC_COMPRESSED));
}

static void test_keys(void)
{
    uint8_t seed[32], fp2[32] = {1}, a[32], b[32];
    hashed("home seed", seed);
    home_identity id, id2;
    CHECK(keys_identity(ctx, seed, FP, NODE_ID_ZERO_BITS, &id));
    CHECK(id.id[0] == 0 && id.id[1] == 0 && keys_id_ok(id.id, NODE_ID_ZERO_BITS));
    uint8_t again[32];
    keys_node_id(id.xonly, again);
    CHECK(!memcmp(again, id.id, 32));
    CHECK(keys_identity(ctx, seed, FP, NODE_ID_ZERO_BITS, &id2) && id2.j == id.j && !memcmp(id2.sec, id.sec, 32));   /* deterministic */
    CHECK(keys_identity(ctx, seed, fp2, NODE_ID_ZERO_BITS, &id2) && memcmp(id2.xonly, id.xonly, 32) != 0);         /* another network */
    CHECK(!memcmp(id.pub33 + 1, id.xonly, 32));                                                 /* the Noise key has the same x */
    keys_starts(seed, FP, a);
    keys_starts(seed, fp2, b);
    CHECK(memcmp(a, b, 32) != 0 && memcmp(a, id.sec, 32) != 0);
    CHECK(keys_coin(ctx, seed, FP, 0, a) && keys_coin(ctx, seed, FP, 1, b) && memcmp(a, b, 32) != 0);
    uint8_t zid[32] = {0, 0, 0xff}, nid[32] = {0, 1};
    CHECK(keys_id_ok(zid, NODE_ID_ZERO_BITS) && !keys_id_ok(nid, NODE_ID_ZERO_BITS));
    CHECK(keys_id_ok(nid, 15) && !keys_id_ok(nid, 16) && keys_id_ok(zid, 16) && !keys_id_ok(zid, 17) && keys_id_ok(nid, 0));
    /* fewer bits (the tests of the network): the first j whose ID has 4 zero bits, about 16 tries, at or before the one with 16 */
    home_identity few;
    CHECK(keys_identity(ctx, seed, FP, 4, &few) && keys_id_ok(few.id, 4) && few.j <= id.j);
    CHECK(few.j == id.j || !keys_id_ok(few.id, NODE_ID_ZERO_BITS));
    CHECK(!keys_identity(ctx, seed, FP, 33, &few));
}

/* a network with a known key: P = k*G, base, dpbits 0 (every point is distinguished) */
static void make_net(record_net *net, unsigned dpbits)
{
    uint8_t k[32], base[32] = {0}, p[33];
    hashed("puzzle key", k);
    base[31] = 0x10, base[20] = 0x01;
    pub33(k, p);
    CHECK(record_net_init(ctx, net, FP, dpbits, p, base));
}

/* R = t*Q' + d*G, its x and whether its y is even */
static bool point(const record_net *net, int t, const uint8_t d[32], uint8_t x[32])
{
    secp256k1_pubkey r, q = net->qp;
    uint8_t c[33];
    size_t n = 33;
    if (!secp256k1_ec_pubkey_create(ctx, &r, d)) {
        CHECK(!"d is a valid scalar");
        return false;
    }
    if (t) {
        const secp256k1_pubkey dg = r;
        const secp256k1_pubkey *both[2] = {&dg, &q};
        if (t < 0) CHECK(secp256k1_ec_pubkey_negate(ctx, &q));
        CHECK(secp256k1_ec_pubkey_combine(ctx, &r, both, 2));
    }
    CHECK(secp256k1_ec_pubkey_serialize(ctx, c, &n, &r, SECP256K1_EC_COMPRESSED));
    memcpy(x, c + 1, 32);
    return c[0] == 2;
}

static void test_record(void)
{
    record_net net;
    make_net(&net, 0);
    uint8_t seed[32];
    hashed("owner", seed);
    home_identity id;
    CHECK(keys_identity(ctx, seed, FP, NODE_ID_ZERO_BITS, &id));
    record r = {.t = -1, .seq = 0x0102030405060708ULL}, back;
    uint8_t d[32], enc[RECORD_LEN];
    hashed("distance", d);
    d[0] = 0;
    if (!point(&net, r.t, d, r.x)) {                           /* the even-y point: -R = (-t)Q' + (-d)G */
        r.t = -r.t;
        CHECK(secp256k1_ec_seckey_negate(ctx, d));
        CHECK(point(&net, r.t, d, r.x));
    }
    memcpy(r.owner, id.xonly, 32);
    CHECK(record_sign(ctx, &net, &id.kp, &r) && record_check(ctx, &net, &r));
    record_encode(&r, enc);
    CHECK(enc[0] == 1 && enc[66] == 1 && enc[73] == 8);
    uint8_t enc2[RECORD_LEN];
    CHECK(record_decode(enc, &back) && record_check(ctx, &net, &back));
    record_encode(&back, enc2);
    CHECK(!memcmp(enc2, enc, RECORD_LEN));                       /* compared by the encoding: the struct has padding */
    for (int i = 1; i < RECORD_LEN; i++) {                     /* any changed byte: refused by the format or by the signature */
        uint8_t e2[RECORD_LEN];
        memcpy(e2, enc, sizeof e2);
        e2[i] ^= 0x01;
        CHECK(!record_decode(e2, &back) || !record_check(ctx, &net, &back));
    }
    enc[0] = 2;
    CHECK(!record_decode(enc, &back));
    record_net other = net;
    other.fp[5] ^= 1;
    CHECK(!record_check(ctx, &other, &r));                      /* the signature binds the network */
    other = net;
    other.dpbits = 30;
    CHECK(!record_check(ctx, &other, &r));                      /* a random x has no 30 zero bits */
    /* the proof of possession */
    uint8_t asker[32], nonce[32], sig[64], wrong[32];
    hashed("asker", asker), hashed("nonce", nonce);
    CHECK(record_prove(ctx, &net, &r, d, asker, nonce, sig) && record_proof_ok(ctx, &net, &r, asker, nonce, sig));
    nonce[0] ^= 1;
    CHECK(!record_proof_ok(ctx, &net, &r, asker, nonce, sig));
    nonce[0] ^= 1;
    memcpy(wrong, d, 32), wrong[31] ^= 1;
    CHECK(!record_prove(ctx, &net, &r, wrong, asker, nonce, sig));   /* without the distance there is no proof */
    record r2 = r;
    r2.t = r.t ? 0 : 1;                                          /* another type on the same x: the distance does not fit */
    CHECK(!record_prove(ctx, &net, &r2, d, asker, nonce, sig));
    CHECK(record_prove(ctx, &net, &r, d, asker, nonce, sig) && !record_proof_ok(ctx, &net, &r2, asker, nonce, sig));
    /* the zero bits */
    uint8_t x[32] = {0};
    x[23] = 0x01;
    CHECK(record_dp_ok(x, 0) && !record_dp_ok(x, 1));
    x[23] = 0, x[22] = 0x80;
    CHECK(record_dp_ok(x, 15) && !record_dp_ok(x, 16));
    x[22] = 0, x[24] = 0xff, x[15] = 0xff;                      /* bits 63 and 128: outside */
    CHECK(record_dp_ok(x, 64));
}

/* a record of dp.bin from a kind and a signed distance d = sign * mag, with the tag of its point; mag is below 2^191 (the top bit of mag24
 * is cleared), as the distances of pzsolve, which are signed 192-bit integers */
static void dp_record(const record_net *net, unsigned kind, int sign, const uint8_t mag24_in[24], uint8_t rec[DPBIN_REC], uint8_t x[32])
{
    uint8_t mag24[24];
    memcpy(mag24, mag24_in, 24);
    mag24[0] &= 0x7f;
    uint64_t w[3] = {0, 0, 0};
    for (int i = 0; i < 24; i++) w[2 - i / 8] = w[2 - i / 8] << 8 | mag24[i];
    uint8_t d[32] = {0};
    memcpy(d + 8, mag24, 24);
    if (sign < 0) {
        CHECK(secp256k1_ec_seckey_negate(ctx, d));
        unsigned carry = 1;
        for (int i = 0; i < 3; i++) {
            w[i] = ~w[i] + carry;
            carry = carry && w[i] == 0;
        }
    }
    static const int coef[3] = {0, 1, -1};
    (void)point(net, coef[kind], d, x);
    uint64_t limb2 = 0;
    for (int i = 8; i < 16; i++) limb2 = limb2 << 8 | x[i];
    const uint64_t tagk = (limb2 & ~7ULL) | kind;
    for (int b = 0; b < 8; b++) rec[b] = (uint8_t)(tagk >> (8 * b));
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 8; b++) rec[8 + 8 * i + b] = (uint8_t)(w[i] >> (8 * b));
}

static void test_dpfeed(void)
{
    record_net net;
    make_net(&net, 0);
    for (unsigned kind = 0; kind < 3; kind++)
        for (int sign = -1; sign <= 1; sign += 2)
            for (int trial = 0; trial < 4; trial++) {
                uint8_t h[32], rec[DPBIN_REC], x[32], x2[32];
                char label[32];
                snprintf(label, sizeof label, "d %u %d %d", kind, sign, trial);
                hashed(label, h);
                dp_record(&net, kind, sign, h, rec, x);
                own_point p;
                CHECK(dpfeed_point(ctx, &net, rec, &p));
                CHECK(!memcmp(p.x, x, 32));
                CHECK(point(&net, p.t, p.d, x2) && !memcmp(x2, x, 32));       /* R = t*Q' + d*G with even y */
                rec[0] ^= 0x08;                                                 /* a tag that is not the point's */
                CHECK(!dpfeed_point(ctx, &net, rec, &p));
                rec[0] ^= 0x08;
                rec[0] = (uint8_t)((rec[0] & ~7) | 3);                          /* wild on 3Q: not in the network */
                CHECK(!dpfeed_point(ctx, &net, rec, &p));
            }
    /* a file: the header, two records, a third cut in half; then the rest of it */
    const char *path = "build/peer/t_record_dp.bin";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    uint8_t hdr[DPBIN_HDR] = "PZDP5\n", rec[3][DPBIN_REC], x[32], h[32];
    fwrite(hdr, 1, sizeof hdr, f);
    for (int i = 0; i < 3; i++) {
        char label[16];
        snprintf(label, sizeof label, "file %d", i);
        hashed(label, h);
        dp_record(&net, (unsigned)i, 1, h, rec[i], x);
    }
    fwrite(rec[0], 1, DPBIN_REC, f);
    fwrite(rec[1], 1, DPBIN_REC, f);
    fwrite(rec[2], 1, 10, f);
    fflush(f);
    dpfeed df;
    own_point p[8];
    CHECK(dpfeed_open(&df, path));
    CHECK(dpfeed_read(ctx, &net, &df, p, 8) == 2 && df.next == 2 && p[1].index == 1);
    CHECK(dpfeed_read(ctx, &net, &df, p, 8) == 0);
    fwrite(rec[2] + 10, 1, DPBIN_REC - 10, f);
    fclose(f);
    CHECK(dpfeed_read(ctx, &net, &df, p, 8) == 1 && p[0].index == 2 && df.bad == 0);
    dpfeed_close(&df);
    remove(path);
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    test_keys();
    test_record();
    test_dpfeed();
    secp256k1_context_destroy(ctx);
    printf(fails ? "t_record: %d FAILED\n" : "t_record: OK\n", fails);
    return fails != 0;
}
