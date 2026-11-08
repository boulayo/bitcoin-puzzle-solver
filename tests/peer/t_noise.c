/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_noise.c - the handshake XX and the frames of noise.c against vectors made with an independent implementation: the noiseprotocol
 * package of Python with a secp256k1 DH plugged in (SHA256 of the compressed shared point), the frames and the key rotation of BOLT 8
 * written over the cryptography package. Then the refusals: a changed byte anywhere, a message out of turn, another prologue. */
#include "peertest.h"
#include "noise.h"
#include <pzcore/hash.h>
#include <stdio.h>
#include <string.h>


static const char M0[] =
    "0322bd171dff80542457ce8670ecd4fff0126bfed832a378343f66cc3276740825707a6e6574312068656c6c6f";
static const char M1[] =
    "02d7fd84d99f68f768247cb9fb59bc309c81d3e130fde7ee027e329bff39b64528c16f4214035250e04b316e6f01fcd0fa6d3baa480df39f55ce8c65"
    "aae612c5a2d086129f7aeb9b96e7755900bb94cc082ed58e2b7849a35189cbb35106120ed18f";
static const char M2[] =
    "7a8adc932862aca478845651b5f081c27d59b5d1136c22ff7554a316e9b53b5b78cdeb634241edde191842e1922b5e54e23ef23173f952d6707ad301"
    "b81070c3f75289";
static const char HASH[] =
    "549ce94e532bc056bda91c15b4b175cc582e36ea6fbaf3d2f264782dbda2f01b";
static const char F1[] =
    "b30df5e104da2cb484db6ced0449f2c07e274b90ab7adfd8e0a56cdea52e3fd17825c8bdd0b451fb1ef5425889";
static const char F2[] =
    "539101357a849288cb7f09b91d2552d8879861fa7a0e2dbb7ebc953a1ef128d03dfa";
static const char B1[] =
    "5f7170819f29998c50f5582089f4ec745e9786ac55617438dc3ab1a1619539e6e50109c2eda79a1aa13e69b25f5c3f74ec42ab6b3a6e1dc87e0533fb"
    "04d859dc20b654c8d2bc72604df95bfb4418213972fd3cf9822f03832a864f88e6012001059ca68163755eede975384414e4f90878eda813df8ba482"
    "8d2c1b8500b69614aeb86db01574861c5fe2fb9a9d794c5c09ddbd2aeb93f522fd3089497065170444dea5340ab54114c277ccd07a48b4e43a247ecf"
    "6eb6746690f5ba8ad626bfac227affa8db38f3b100271fc137fb716ab84cca3e9fe8f4ed7df262c71d53c86db0eee28692403a06b182";
static const char F3[] =
    "16290810844dab9725b7072c671de12d83fa3084538b3e4a6b7bf41a28af15632878f433dad40a8dd56e2903dc21be4862eeb09e4b";

static size_t unhex(const char *h, uint8_t *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned b;
        sscanf(h, "%2x", &b);
        out[n++] = (uint8_t)b;
    }
    return n;
}

static bool same(const uint8_t *got, size_t n, const char *want)
{
    uint8_t w[1024];
    return unhex(want, w) == n && memcmp(got, w, n) == 0;
}

static void key(const char *label, uint8_t out[32]) { pz_sha256((const uint8_t *)label, strlen(label), out); }

static uint8_t prologue[38];

static void start(secp256k1_context *ctx, noise_hs *i, noise_hs *r)
{
    uint8_t si[32], ei[32], sr[32], er[32];
    key("s_i", si), key("e_i", ei), key("s_r", sr), key("e_r", er);
    CHECK(noise_hs_init(i, ctx, true, prologue, sizeof prologue, si, ei));
    CHECK(noise_hs_init(r, ctx, false, prologue, sizeof prologue, sr, er));
}

/* the three messages, checked against the vectors; the channel of each side */
static void handshake(secp256k1_context *ctx, noise_cs *itx, noise_cs *irx, noise_cs *rtx, noise_cs *rrx)
{
    noise_hs i, r;
    uint8_t m[NOISE_HS_MAX], p[NOISE_PAYLOAD_MAX], ri[33], rr[33], hi[32], hr[32];
    size_t n, pl;
    start(ctx, &i, &r);
    CHECK(noise_hs_write(&r, NULL, 0, m) == 0);                          /* not the responder's turn */
    n = noise_hs_write(&i, (const uint8_t *)"pznet1 hello", 12, m);
    CHECK(same(m, n, M0));
    CHECK(noise_hs_read(&r, m, n, p, &pl) && pl == 12 && !memcmp(p, "pznet1 hello", 12));
    n = noise_hs_write(&r, NULL, 0, m);
    CHECK(same(m, n, M1));
    CHECK(noise_hs_read(&i, m, n, p, &pl) && pl == 0);
    n = noise_hs_write(&i, (const uint8_t *)"pz", 2, m);
    CHECK(same(m, n, M2));
    CHECK(noise_hs_read(&r, m, n, p, &pl) && pl == 2 && !memcmp(p, "pz", 2));
    CHECK(noise_hs_split(&i, itx, irx, ri, hi) && noise_hs_split(&r, rtx, rrx, rr, hr));
    CHECK(same(hi, 32, HASH) && same(hr, 32, HASH));
    uint8_t si[32], sr[32], pi[33], pr[33];
    key("s_i", si), key("s_r", sr);
    secp256k1_pubkey pk;
    size_t l = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &pk, si) && secp256k1_ec_pubkey_serialize(ctx, pi, &l, &pk, SECP256K1_EC_COMPRESSED));
    CHECK(secp256k1_ec_pubkey_create(ctx, &pk, sr) && secp256k1_ec_pubkey_serialize(ctx, pr, &l, &pk, SECP256K1_EC_COMPRESSED));
    CHECK(!memcmp(ri, pr, 33) && !memcmp(rr, pi, 33));                    /* each side learnt the other's static key */
}

static bool recv_frame(noise_cs *rx, const uint8_t *f, size_t n, uint8_t *body, size_t *len)
{
    return n >= NOISE_HEAD + NOISE_TAG && noise_frame_head(rx, f, len) && n == NOISE_HEAD + *len + NOISE_TAG &&
           noise_frame_body(rx, f + NOISE_HEAD, *len, body);
}

static void test_frames(secp256k1_context *ctx)
{
    noise_cs itx, irx, rtx, rrx;
    handshake(ctx, &itx, &irx, &rtx, &rrx);
    uint8_t f[NOISE_HEAD + 300], b[300], big[200];
    size_t n, l;
    n = noise_frame_seal(&itx, (const uint8_t *)"first frame", 11, f);
    CHECK(same(f, n, F1));
    CHECK(recv_frame(&rrx, f, n, b, &l) && l == 11 && !memcmp(b, "first frame", 11));
    n = noise_frame_seal(&itx, NULL, 0, f);
    CHECK(same(f, n, F2));
    CHECK(recv_frame(&rrx, f, n, b, &l) && l == 0);
    for (int k = 0; k < 200; k++) big[k] = (uint8_t)k;
    n = noise_frame_seal(&rtx, big, 200, f);                              /* the other direction */
    CHECK(same(f, n, B1));
    CHECK(recv_frame(&irx, f, n, b, &l) && l == 200 && !memcmp(b, big, 200));
    for (int k = 0; k < 1200; k++) {                                      /* 2400 encryptions: the keys rotate twice */
        char t[16];
        const int tl = snprintf(t, sizeof t, "%d", k);
        n = noise_frame_seal(&itx, (const uint8_t *)t, (size_t)tl, f);
        CHECK(recv_frame(&rrx, f, n, b, &l) && l == (size_t)tl && !memcmp(b, t, l));
    }
    n = noise_frame_seal(&itx, (const uint8_t *)"after two rotations", 19, f);
    CHECK(same(f, n, F3));
    /* a changed bit of the head or of the body is refused */
    uint8_t g[sizeof f];
    noise_cs save = rrx;
    memcpy(g, f, n), g[3] ^= 1;
    CHECK(!noise_frame_head(&rrx, g, &l));
    rrx = save;
    memcpy(g, f, n), g[NOISE_HEAD + 2] ^= 1;
    CHECK(noise_frame_head(&rrx, g, &l) && !noise_frame_body(&rrx, g + NOISE_HEAD, l, b));
    rrx = save;
    CHECK(recv_frame(&rrx, f, n, b, &l) && l == 19);
    CHECK(!recv_frame(&rrx, f, n, b, &l));                                /* the same frame again: the nonce has moved on */
    CHECK(noise_frame_seal(&itx, big, NOISE_FRAME_MAX + 1, f) == 0);
}

static void test_refusals(secp256k1_context *ctx)
{
    uint8_t m[NOISE_HS_MAX], p[NOISE_PAYLOAD_MAX], m1[NOISE_HS_MAX];
    size_t n, pl, n1;
    /* every byte of the second and of the third message is covered */
    for (int which = 1; which <= 2; which++) {
        noise_hs i0, r0;
        start(ctx, &i0, &r0);
        n = noise_hs_write(&i0, NULL, 0, m);
        CHECK(noise_hs_read(&r0, m, n, p, &pl));
        n1 = noise_hs_write(&r0, NULL, 0, m1);
        size_t len = n1;
        if (which == 2) {
            CHECK(noise_hs_read(&i0, m1, n1, p, &pl));
            len = noise_hs_write(&i0, NULL, 0, m1);
        }
        int refused = 0, tried = 0;
        for (size_t b = 0; b < len; b++) {
            noise_hs i = i0, r = r0;
            uint8_t c[NOISE_HS_MAX];
            memcpy(c, m1, len);
            c[b] ^= 0x40;
            tried++;
            refused += which == 1 ? !noise_hs_read(&i, c, len, p, &pl) : !noise_hs_read(&r, c, len, p, &pl);
        }
        CHECK(refused == tried);
        noise_hs i = i0, r = r0;                                          /* a short message */
        CHECK(which == 1 ? !noise_hs_read(&i, m1, len - 1, p, &pl) : !noise_hs_read(&r, m1, len - 1, p, &pl));
    }
    /* another prologue (another network): the second message does not authenticate */
    noise_hs i, r;
    start(ctx, &i, &r);
    uint8_t other[38];
    memcpy(other, prologue, sizeof other), other[37] ^= 1;
    uint8_t sr[32], er[32];
    key("s_r", sr), key("e_r", er);
    CHECK(noise_hs_init(&r, ctx, false, other, sizeof other, sr, er));
    n = noise_hs_write(&i, NULL, 0, m);
    CHECK(noise_hs_read(&r, m, n, p, &pl));
    n = noise_hs_write(&r, NULL, 0, m);
    CHECK(!noise_hs_read(&i, m, n, p, &pl));
    CHECK(noise_hs_write(&i, NULL, 0, m) == 0);                           /* broken for good */
    /* a payload too long, an invalid secret */
    start(ctx, &i, &r);
    CHECK(noise_hs_write(&i, m, NOISE_PAYLOAD_MAX + 1, m1) == 0);
    uint8_t zero[32] = {0};
    CHECK(!noise_hs_init(&i, ctx, true, prologue, sizeof prologue, zero, zero));
}

int main(void)
{
    memcpy(prologue, "pznet1", 6);
    memset(prologue + 6, 0x11, 32);
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    test_frames(ctx);
    test_refusals(ctx);
    secp256k1_context_destroy(ctx);
    printf(g_fails ? "t_noise: %d FAILED\n" : "t_noise: OK\n", g_fails);
    return g_fails != 0;
}
