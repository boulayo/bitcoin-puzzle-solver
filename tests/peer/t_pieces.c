/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_pieces.c - the pieces of the key of a coincidence, for the three kinds of pair (tame with wild +, tame with wild -, wild + with wild -),
 * and, when PZSIGN names a pzsign binary, a whole two-party signing driven through signer.c with the pieces of a coincidence, once with
 * each party first, the signatures checked with libsecp256k1. */
#include "peertest.h"
#include "pieces.h"
#include "signer.h"
#include <pzcore/hash.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static secp256k1_context *ctx;
static const uint8_t FP[32] = {0x77};

static void hashed(const char *s, uint8_t out[32]) { pz_sha256((const uint8_t *)s, strlen(s), out); }

typedef struct {
    record_net net;
    uint8_t k[32], base[32], p33[33];
    record pair[2];
    uint8_t d[2][32];                  /* the distances of the pair, in the order of the parties */
} fixture;

/* a coincidence: the puzzle key k = base + x, two homes with types ta, tb and t_a*x + d_a = t_b*x + d_b on the even-y point */
static void make(fixture *f, int ta, int tb, const char *label)
{
    char l[64];
    secp256k1_pubkey p;
    size_t n = 33;
    memset(f->base, 0, 32);
    f->base[23] = 0x80;                                         /* base = 2^63 */
    uint8_t x[32] = {0}, h[32];
    snprintf(l, sizeof l, "%s x", label);
    hashed(l, h);
    memcpy(x + 16, h, 16);                                      /* x < 2^128 */
    memcpy(f->k, x, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, f->k, f->base));
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, f->k) && secp256k1_ec_pubkey_serialize(ctx, f->p33, &n, &p, SECP256K1_EC_COMPRESSED));
    CHECK(record_net_init(ctx, &f->net, FP, 0, f->p33, f->base));
    /* d_b at random, d_a = (t_b - t_a)*x + d_b */
    uint8_t da[32], db[32], tx[32];
    snprintf(l, sizeof l, "%s d", label);
    hashed(l, db);
    db[0] = 0;
    memcpy(da, db, 32);
    for (int m = 0; m < abs(tb - ta); m++) {
        memcpy(tx, x, 32);
        if (tb - ta < 0) CHECK(secp256k1_ec_seckey_negate(ctx, tx));
        CHECK(secp256k1_ec_seckey_tweak_add(ctx, da, tx));
    }
    record a = {.t = ta}, b = {.t = tb};
    secp256k1_pubkey r;
    uint8_t c[33];
    n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &r, da) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &r, SECP256K1_EC_COMPRESSED));
    if (ta) {                                                   /* R = t_a*Q' + d_a*G */
        uint8_t xs[32];
        memcpy(xs, x, 32);
        if (ta < 0) CHECK(secp256k1_ec_seckey_negate(ctx, xs));
        CHECK(secp256k1_ec_seckey_tweak_add(ctx, xs, da));
        CHECK(secp256k1_ec_pubkey_create(ctx, &r, xs) && secp256k1_ec_pubkey_serialize(ctx, c, &(size_t){33}, &r, SECP256K1_EC_COMPRESSED));
    }
    if (c[0] == 3) {                                            /* the even-y point: every sign flips */
        a.t = -a.t, b.t = -b.t;
        CHECK(secp256k1_ec_seckey_negate(ctx, da) && secp256k1_ec_seckey_negate(ctx, db));
    }
    memcpy(a.x, c + 1, 32), memcpy(b.x, c + 1, 32);
    secp256k1_keypair ka, kb;                                   /* the owners: any keys (the zero bits of node IDs do not matter here) */
    secp256k1_xonly_pubkey xa, xb;
    uint8_t s[32];
    snprintf(l, sizeof l, "%s home a", label), hashed(l, s);
    CHECK(secp256k1_keypair_create(ctx, &ka, s) && secp256k1_keypair_xonly_pub(ctx, &xa, NULL, &ka));
    snprintf(l, sizeof l, "%s home b", label), hashed(l, s);
    CHECK(secp256k1_keypair_create(ctx, &kb, s) && secp256k1_keypair_xonly_pub(ctx, &xb, NULL, &kb));
    CHECK(secp256k1_xonly_pubkey_serialize(ctx, a.owner, &xa) && secp256k1_xonly_pubkey_serialize(ctx, b.owner, &xb));
    CHECK(record_sign(ctx, &f->net, &ka, &a) && record_sign(ctx, &f->net, &kb, &b));
    CHECK(record_check(ctx, &f->net, &a) && record_check(ctx, &f->net, &b));
    CHECK(pieces_order(&b, &a, f->pair));
    const bool a_first = !memcmp(f->pair[0].owner, a.owner, 32);
    memcpy(f->d[0], a_first ? da : db, 32);
    memcpy(f->d[1], a_first ? db : da, 32);
}

static void test_pieces(int ta, int tb, const char *label)
{
    fixture f;
    make(&f, ta, tb, label);
    uint8_t x0[32], x1[32], sum[32];
    CHECK(pieces_secret(ctx, &f.net, f.base, f.pair, 0, f.d[0], x0));
    CHECK(pieces_secret(ctx, &f.net, f.base, f.pair, 1, f.d[1], x1));
    memcpy(sum, x0, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, sum, x1) && !memcmp(sum, f.k, 32));   /* x0 + x1 is the key */
    secp256k1_pubkey X0, X1, P, S;
    const secp256k1_pubkey *both[2] = {&X0, &X1};
    CHECK(pieces_public(ctx, &f.net, f.base, f.pair, 0, &X0) && pieces_public(ctx, &f.net, f.base, f.pair, 1, &X1));
    CHECK(secp256k1_ec_pubkey_combine(ctx, &S, both, 2) && secp256k1_ec_pubkey_parse(ctx, &P, f.p33, 33) && !secp256k1_ec_pubkey_cmp(ctx, &S, &P));
    uint8_t wrong[32];
    memcpy(wrong, f.d[1], 32), wrong[31] ^= 1;
    CHECK(!pieces_secret(ctx, &f.net, f.base, f.pair, 1, wrong, x1));           /* a distance that is not the record's */
    CHECK(!pieces_secret(ctx, &f.net, f.base, f.pair, 0, f.d[1], x0));          /* the distance of the other party */
    record same[2];
    CHECK(!pieces_order(&f.pair[0], &f.pair[0], same));
    record other = f.pair[1];
    other.t = f.pair[0].t;
    CHECK(!pieces_order(&f.pair[0], &other, same));                             /* the same type: a merge, not a key */
    other = f.pair[1], other.x[0] ^= 1;
    CHECK(!pieces_order(&f.pair[0], &other, same));
    uint8_t e1[32], e2[32];
    pieces_eid(&f.net, f.pair, e1);
    f.pair[1].seq++;
    pieces_eid(&f.net, f.pair, e2);
    CHECK(memcmp(e1, e2, 32) != 0);
}

/* one direction of the relay: the frames of party `from` go to the other as messages; its final answer is kept, and ends the other's input */
typedef struct { signer *s; int from; uint8_t *answer; size_t len; bool ok; } relay;

static void *relay_main(void *arg)
{
    relay *r = arg;
    for (;;) {
        uint8_t type, *p;
        size_t len;
        if (!signer_recv(&r->s[r->from], &type, &p, &len, 120000)) return NULL;
        if (type == SIGNER_OUT) {
            const bool sent = signer_send(&r->s[1 - r->from], SIGNER_IN, p, len);
            free(p);
            if (!sent) return NULL;
            continue;
        }
        r->ok = type == SIGNER_RESULT;
        if (!r->ok) printf("  party %d: %s\n", r->from, (char *)p);
        r->answer = p, r->len = len;
        signer_end_input(&r->s[1 - r->from]);
        return NULL;
    }
}

/* both parties through signer.c, relayed by two threads (one per direction: a message of 200 KB fills a pipe, and a single relay that
 * writes to one party while the other waits to write would block both) */
static void test_signing(const char *bin, int first)
{
    fixture f;
    make(&f, 0, 1, "signing");
    uint8_t piece[2][32], pub[2][33], eid[32], digests[2][32], job[SIGNER_JOB_MAX];
    for (int i = 0; i < 2; i++) {
        secp256k1_pubkey X;
        size_t n = 33;
        CHECK(pieces_secret(ctx, &f.net, f.base, f.pair, i, f.d[i], piece[i]));
        CHECK(pieces_public(ctx, &f.net, f.base, f.pair, i, &X) && secp256k1_ec_pubkey_serialize(ctx, pub[i], &n, &X, SECP256K1_EC_COMPRESSED));
    }
    pieces_eid(&f.net, f.pair, eid);
    hashed("digest 0", digests[0]);
    memset(digests[1], 0xff, 32);                               /* above the order: signed as the number mod n */
    signer s[2];
    for (int i = 0; i < 2; i++) {
        const size_t n = signer_job(job, i, first, eid, piece[i], (const uint8_t(*)[33])pub, f.p33, (const uint8_t(*)[32])digests, 2);
        CHECK(signer_start(&s[i], bin) && signer_send(&s[i], SIGNER_JOB, job, n));
    }
    relay r[2] = {{s, 0, NULL, 0, false}, {s, 1, NULL, 0, false}};
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, relay_main, &r[i]);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    /* both have the same signatures, r || s of each digest */
    CHECK(r[0].ok && r[1].ok && r[0].len == 128 && r[1].len == 128 && !memcmp(r[0].answer, r[1].answer, 128));
    for (int i = 0; i < 2 && r[0].ok && r[0].len == 128; i++) {
        secp256k1_ecdsa_signature sig;
        secp256k1_pubkey P;
        CHECK(secp256k1_ecdsa_signature_parse_compact(ctx, &sig, r[0].answer + 64 * i) && secp256k1_ec_pubkey_parse(ctx, &P, f.p33, 33));
        CHECK(secp256k1_ecdsa_verify(ctx, &sig, digests[i], &P));   /* low s too: libsecp256k1 refuses a high one */
    }
    for (int i = 0; i < 2; i++) signer_stop(&s[i]), free(r[i].answer);
    printf("  two-party signing through pzsign, party %d first: %s\n", first, g_fails ? "failed" : "two valid signatures");
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    test_pieces(0, 1, "tame and wild +");
    test_pieces(0, -1, "tame and wild -");
    test_pieces(1, -1, "wild + and wild -");
    test_pieces(-1, 0, "wild - and tame");
    const char *bin = getenv("PZSIGN");
    if (bin) test_signing(bin, 0), test_signing(bin, 1);
    else printf("  two-party signing skipped (set PZSIGN)\n");
    secp256k1_context_destroy(ctx);
    printf(g_fails ? "t_pieces: %d FAILED\n" : "t_pieces: OK\n", g_fails);
    return g_fails != 0;
}
