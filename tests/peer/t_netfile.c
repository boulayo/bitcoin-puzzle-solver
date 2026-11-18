/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_netfile.c - the network file: its values, a fingerprint that does not change with the layout of the file nor with its optional lines
 * and changes with every defining line, and the files that are refused. */
#include "peertest.h"
#include "netfile.h"
#include <pzcore/hash.h>

static const char BODY[] =
    "# a test network\n"
    "net=t1\npuzzle=0\n"
    "pubkey=0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\n"
    "range=1000:ffff\njumpseed=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n"
    "schema=net-06\ndpbits=8\nchain=regtest\n";

/* BODY with `from` replaced by `to`, and `more` after it */
static bool variant(const char *from, const char *to, const char *more, netfile *nf)
{
    char text[2048], err[200];
    const char *at = from ? strstr(BODY, from) : NULL;
    if (at) snprintf(text, sizeof text, "%.*s%s%s%s", (int)(at - BODY), BODY, to, at + strlen(from), more);
    else snprintf(text, sizeof text, "%s%s", BODY, more);
    return netfile_parse(text, nf, err, sizeof err);
}

int main(void)
{
    netfile a, b;
    CHECK(variant(NULL, NULL, "", &a) && !strcmp(a.net, "t1") && a.dpbits == 8 && !strcmp(a.chain, "regtest") && a.opts_len == 0);
    /* another layout and other optional lines: the same network */
    CHECK(variant("net=t1\npuzzle=0\n", "  puzzle = 0   # zero\r\n\n net=t1\n", "entry=127.0.0.1:9735\nelectrum=h:1\nentry=b:2\n", &b));
    CHECK(!memcmp(a.fp, b.fp, 32) && !strcmp(b.opts, "electrum=h:1\nentry=127.0.0.1:9735\nentry=b:2"));
    /* every defining line counts */
    static const char *const CHANGES[][2] = {{"net=t1", "net=t2"}, {"puzzle=0", "puzzle=1"}, {"range=1000", "range=1001"}, {"dpbits=8", "dpbits=9"},
                                             {"jumpseed=00", "jumpseed=01"}, {"chain=regtest", "chain=signet"}, {"pubkey=0279", "pubkey=0379"}};
    for (size_t i = 0; i < sizeof CHANGES / sizeof *CHANGES; i++) CHECK(variant(CHANGES[i][0], CHANGES[i][1], "", &b) && memcmp(a.fp, b.fp, 32));
    /* refused */
    static const char *const BAD[][2] = {{"net=t1\n", ""}, {"net=t1", "net=T1"}, {"dpbits=8", "dpbits=7"}, {"dpbits=8", "dpbits=61"},
                                         {"schema=net-06", "schema=net-05"}, {"chain=regtest", "chain=moon"}, {"range=1000:ffff", "range=ffff:1000"},
                                         {"range=1000", "range=0"}, {"pubkey=02", "pubkey=05"}, {"puzzle=0", "puzzle=257"}, {"jumpseed=00", "jumpseed=0"}};
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) CHECK(!variant(BAD[i][0], BAD[i][1], "", &b));
    CHECK(!variant(NULL, NULL, "net=t1\n", &b) && !variant(NULL, NULL, "color=red\n", &b) && !variant(NULL, NULL, "no equal sign\n", &b));
    CHECK(!variant(NULL, NULL, "sig=00:00\n", &b));            /* files are not signed */
    /* the networks written in the program: puzzle 140 (its public key, its range, the jump seed from the key, 25 zero bits, the main
     * chain), the first open puzzle by default, and none for a puzzle without a public key */
    char err[200];
    uint8_t m[12 + 33] = "pznet1/jumps", seed[32];
    static const char P140[] = "031f6a332d3c5c4f2de2378c012f429cd109ba07d69690c6c701b6bb87860d6640";
    for (int i = 0; i < 33; i++) sscanf(P140 + 2 * i, "%2hhx", &m[12 + i]);
    pz_sha256(m, sizeof m, seed);
    CHECK(netfile_puzzle(140, &a, err, sizeof err) && !strcmp(a.net, "p140") && a.puzzle == 140 && a.dpbits == 25 && !strcmp(a.chain, "main") &&
          !memcmp(a.pubkey33, m + 12, 33) && !memcmp(a.jumpseed, seed, 32) && a.start.w[2] == 1ull << 11 && a.end.w[2] == (1ull << 12) - 1);
    CHECK(netfile_puzzle(0, &b, err, sizeof err) && b.puzzle == 140 && !memcmp(a.fp, b.fp, 32));
    CHECK(netfile_puzzle(145, &b, err, sizeof err) && b.dpbits == 28 && memcmp(a.fp, b.fp, 32));
    CHECK(!netfile_puzzle(71, &b, err, sizeof err) && !netfile_puzzle(161, &b, err, sizeof err));
    TEST_DONE("t_netfile");
}
