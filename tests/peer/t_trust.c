/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_trust.c - the signatures of the maintainers on a network file: thresholds, keys counted once, and that a change of any line (of the
 * immutable part, through F, or of the updatable part) voids every signature. */
#include "peertest.h"
#include "netfile.h"
#include <pzcore/hash.h>
#include <secp256k1_extrakeys.h>

static secp256k1_context *ctx;

static const char BODY[] =
    "# a test network\n"
    "net=t1\npuzzle=0\n"
    "pubkey=0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\n"
    "range=1000:ffff\njumpseed=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n"
    "schema=rete-06\ndpbits=8\nchain=regtest\n"
    "entry=127.0.0.1:9735\nprize=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff:0:100000000\n";

static void key(const char *label, uint8_t sec[32], char xhex[65])
{
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xo;
    uint8_t x[32];
    pz_sha256((const uint8_t *)label, strlen(label), sec);
    CHECK(secp256k1_keypair_create(ctx, &kp, sec) && secp256k1_keypair_xonly_pub(ctx, &xo, NULL, &kp) && secp256k1_xonly_pubkey_serialize(ctx, x, &xo));
    for (int i = 0; i < 32; i++) sprintf(xhex + 2 * i, "%02x", x[i]);
}

/* the file with the given signature lines; valid signatures counted with the trusted keys of PZNET_TRUST */
static int count(const char *text, const char *trust, const char *threshold, bool *ok)
{
    netfile nf;
    trust_set t;
    char err[200];
    int valid = -1;
    setenv("PZNET_TRUST", trust, 1);
    setenv("PZNET_THRESHOLD", threshold, 1);
    CHECK(netfile_parse(text, &nf, err, sizeof err));
    *ok = trust_load(&t, err, sizeof err) && trust_check(ctx, &t, &nf, &valid);
    return valid;
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t s1[32], s2[32], s3[32];
    char x1[65], x2[65], x3[65], l1[200], l2[200], text[4096], trust[300], err[200];
    key("maintainer 1", s1, x1), key("maintainer 2", s2, x2), key("maintainer 3", s3, x3);
    netfile nf;
    CHECK(netfile_parse(BODY, &nf, err, sizeof err));
    CHECK(trust_sign(ctx, s1, &nf, l1) && trust_sign(ctx, s2, &nf, l2));
    snprintf(text, sizeof text, "%s%s\n%s\n%s\n", BODY, l1, l2, l1);   /* the first signature twice */
    snprintf(trust, sizeof trust, "%s,%s,%s", x1, x2, x3);
    bool ok;
    CHECK(count(text, trust, "2", &ok) == 2 && ok);              /* two distinct keys, the repeated one once */
    CHECK(count(text, trust, "3", &ok) == 2 && !ok);
    CHECK(count(text, x3, "1", &ok) == 0 && !ok);                /* signed by keys that are not trusted */
    /* reordering and comments do not matter; a changed line does */
    char moved[4096];
    snprintf(moved, sizeof moved, "%s\n  # signed\n%s\n%s", l2, l1, BODY);
    CHECK(count(moved, trust, "2", &ok) == 2 && ok);
    char *p = strstr(text, "100000000");
    p[0] = '2';                                                  /* the prize amount */
    CHECK(count(text, trust, "1", &ok) == 0 && !ok);
    p[0] = '1';
    p = strstr(text, "dpbits=8");
    p[7] = '9';                                                  /* an immutable line: another F */
    CHECK(count(text, trust, "1", &ok) == 0 && !ok);
    /* malformed variables */
    trust_set t;
    setenv("PZNET_TRUST", "abc", 1);
    CHECK(!trust_load(&t, err, sizeof err));
    setenv("PZNET_TRUST", x1, 1), setenv("PZNET_THRESHOLD", "2", 1);
    CHECK(!trust_load(&t, err, sizeof err));
    unsetenv("PZNET_TRUST");
    CHECK(!trust_load(&t, err, sizeof err));                     /* no maintainers compiled in yet */
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_trust");
}
