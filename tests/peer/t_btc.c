/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_btc.c - Bitcoin transactions: scripts of addresses, serialization, txid, the amount check, the legacy signature hash, DER,
 * signing and verification. The expected values come from transactions of the main chain and from gen_btc_vectors.py, an independent
 * implementation in Python (btc_vectors.h). */
#include "btc.h"
#include "peertest.h"
#include "btc_vectors.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <secp256k1.h>

#define NV(a) (sizeof (a) / sizeof (a)[0])

static const uint8_t ORDER[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                                  0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};

/* the compressed point G: the public key of the secret key 1, the key of puzzle 1 */
static const char G33[] = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

static void test_hex(void)
{
    uint8_t b[40], h[32];
    char s[81];
    CHECK(btc_unhex("00ff7fA0bC", 10, b) && !memcmp(b, "\x00\xff\x7f\xa0\xbc", 5));
    btc_hex(b, 5, s);
    CHECK_STR(s, "00ff7fa0bc");
    CHECK(!btc_unhex("abc", 3, b) && !btc_unhex("0g", 2, b) && !btc_unhex("g0", 2, b) && !btc_unhex(" 0", 2, b) && btc_unhex("", 0, b));
    CHECK(btc_hash_from_hex("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16", h));
    CHECK(h[0] == 0x16 && h[31] == 0xf4);   /* internal order: the reverse */
    btc_hash_to_hex(h, s);
    CHECK_STR(s, "f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16");
    CHECK(!btc_hash_from_hex("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e1", h));
    CHECK(!btc_hash_from_hex("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e166", h));
    CHECK(!btc_hash_from_hex("x4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16", h) && !btc_hash_from_hex(NULL, h));
}

static void test_address(void)
{
    btc_chain c;
    CHECK(btc_chain_parse("main", &c) && c == BTC_MAIN && btc_chain_parse("test", &c) && c == BTC_TEST);
    CHECK(btc_chain_parse("signet", &c) && c == BTC_SIGNET && btc_chain_parse("regtest", &c) && c == BTC_REGTEST);
    CHECK(!btc_chain_parse("mainnet", &c) && !btc_chain_parse("", &c) && !btc_chain_parse(NULL, &c));

    /* Addresses whose scripts are known: 1A1z... is the address of the key of the genesis block (hash160 62e907...), 1BgG...,
     * mrCD... and bc1qw508... are those of the key 1 (hash160 751e76..., checked below), 3JvL... is P2SH of its P2WPKH script,
     * bc1qrp33.../tb1qrp33... are the P2WSH examples of BIP173 (the P2PK script of the key 1), bc1p0xlx... is the P2TR address of
     * BIP350 with the x of G, tb1pqqqq... is in BIP350's list. */
    static const struct { btc_chain chain; const char *addr, *spk; } V[] = {
        {BTC_MAIN, "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa", "76a91462e907b15cbf27d5425399ebf6f0fb50ebb88f1888ac"},
        {BTC_MAIN, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"},
        {BTC_MAIN, "3JvL6Ymt8MVWiCNHC7oWU6nLeHNJKLZGLN", "a914bcfeb728b584253d5f3f70bcb780e9ef218a68f487"},
        {BTC_MAIN, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", "0014751e76e8199196d454941c45d1b3a323f1433bd6"},
        {BTC_MAIN, "BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", "0014751e76e8199196d454941c45d1b3a323f1433bd6"},
        {BTC_MAIN, "bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3",
         "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262"},
        {BTC_MAIN, "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
         "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"},
        {BTC_TEST, "mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r", "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"},
        {BTC_TEST, "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", "0014751e76e8199196d454941c45d1b3a323f1433bd6"},
        {BTC_TEST, "tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7",
         "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262"},
        {BTC_TEST, "tb1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesf3hn0c",
         "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433"},
        {BTC_SIGNET, "mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r", "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"},
        {BTC_SIGNET, "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", "0014751e76e8199196d454941c45d1b3a323f1433bd6"},
        {BTC_REGTEST, "mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r", "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"},
        /* refused: another chain's, a bad checksum, mixed case, the old bech32 checksum on a taproot address */
        {BTC_MAIN, "mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r", ""},
        {BTC_MAIN, "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", ""},
        {BTC_MAIN, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMJ", ""},
        {BTC_MAIN, "bc1qW508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", ""},
        {BTC_MAIN, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5", ""},
        {BTC_MAIN, "", ""},
        {BTC_MAIN, "bc1", ""},
        {BTC_TEST, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", ""},
        {BTC_TEST, "3JvL6Ymt8MVWiCNHC7oWU6nLeHNJKLZGLN", ""},
        {BTC_TEST, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", ""},
        {BTC_SIGNET, "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0", ""},
        {BTC_REGTEST, "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", ""},
        {BTC_REGTEST, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", ""},
    };
    for (size_t k = 0; k < NV(V); k++) {
        uint8_t spk[BTC_SPK_MAX];
        char hex[2 * BTC_SPK_MAX + 1];
        const size_t n = btc_address_script(V[k].chain, V[k].addr, spk);
        CHECK_STR(n ? pt_hex(spk, n, hex) : "", V[k].spk);
    }
    uint8_t g[33], h[20], spk[BTC_SPK_MAX];
    pt_unhex(G33, g, sizeof g);
    pz_hash160(g, 33, h);
    CHECK(btc_address_script(BTC_MAIN, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", spk) == 25 && !memcmp(spk + 3, h, 20));
    uint8_t w[22] = {0, 20};
    memcpy(w + 2, h, 20);
    pz_hash160(w, 22, h);
    CHECK(btc_address_script(BTC_MAIN, "3JvL6Ymt8MVWiCNHC7oWU6nLeHNJKLZGLN", spk) == 23 && !memcmp(spk + 2, h, 20));
    CHECK(btc_address_script((btc_chain)4, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", spk) == 0);
    CHECK(btc_address_script(BTC_MAIN, NULL, spk) == 0);

    /* Each form on each chain, the addresses made here with the encoders of pzcore: accepted by its own chain only (test and signet
     * share all their forms, regtest shares the Base58 ones and has its own bech32 prefix). */
    static const struct { const char *hrp; uint8_t v_pkh, v_sh; } F[] = {{"bc", 0x00, 0x05}, {"tb", 0x6f, 0xc4}, {"tb", 0x6f, 0xc4}, {"bcrt", 0x6f, 0xc4}};
    uint8_t prog[40];
    for (int i = 0; i < 40; i++) prog[i] = (uint8_t)(i * 37 + 11);
    for (int ch = 0; ch < 4; ch++) {
        char a[6][100];
        uint8_t pl[21];
        pl[0] = F[ch].v_pkh;
        memcpy(pl + 1, prog, 20);
        pz_base58check_encode(pl, 21, a[0], sizeof a[0]);
        pl[0] = F[ch].v_sh;
        pz_base58check_encode(pl, 21, a[1], sizeof a[1]);
        pz_segwit_address_encode(a[2], sizeof a[2], F[ch].hrp, 0, prog, 20);
        pz_segwit_address_encode(a[3], sizeof a[3], F[ch].hrp, 0, prog, 32);
        pz_segwit_address_encode(a[4], sizeof a[4], F[ch].hrp, 1, prog, 32);
        static const size_t len[5] = {25, 23, 22, 34, 34};
        static const uint8_t lead[5][2] = {{0x76, 0xa9}, {0xa9, 20}, {0x00, 20}, {0x00, 32}, {0x51, 32}};
        for (int f = 0; f < 5; f++)
            for (int on = 0; on < 4; on++) {
                const bool same = on == ch || (ch == 1 && on == 2) || (ch == 2 && on == 1) || (f < 2 && ch && on);
                const size_t n = btc_address_script((btc_chain)on, a[f], spk);
                CHECK(n == (same ? len[f] : 0));
                if (n) CHECK(spk[0] == lead[f][0] && spk[1] == lead[f][1] && !memcmp(spk + (f ? 2 : 3), prog, len[f] - (f == 0 ? 5 : f == 1 ? 3 : 2)));
            }
        /* witness programs that are valid addresses but no standard output we pay: version 1 of 20 or 40 bytes, versions 2 to 16 */
        pz_segwit_address_encode(a[5], sizeof a[5], F[ch].hrp, 1, prog, 40);
        CHECK(a[5][0] && btc_address_script((btc_chain)ch, a[5], spk) == 0);
        pz_segwit_address_encode(a[5], sizeof a[5], F[ch].hrp, 1, prog, 20);
        CHECK(a[5][0] && btc_address_script((btc_chain)ch, a[5], spk) == 0);
        for (unsigned v = 2; v <= 16; v++) {
            pz_segwit_address_encode(a[5], sizeof a[5], F[ch].hrp, v, prog, 32);
            CHECK(a[5][0] && btc_address_script((btc_chain)ch, a[5], spk) == 0);
        }
    }
    uint8_t h1[20];
    CHECK(btc_script_is_p2pkh(spk, 0, NULL) == false);
    btc_script_p2pkh(prog, spk);
    CHECK(btc_script_is_p2pkh(spk, 25, h1) && !memcmp(h1, prog, 20) && !btc_script_is_p2pkh(spk, 24, h1));
    spk[24] = 0xad;
    CHECK(!btc_script_is_p2pkh(spk, 25, h1));
}

/* the raw bytes of a vector */
static uint8_t *raw_of(const char *hex, size_t *n)
{
    uint8_t *b = pt_unhex_alloc(hex, n);
    CHECK(b != NULL);
    return b;
}

static bool same_hash(const uint8_t h[32], const char *display)
{
    char s[65];
    btc_hash_to_hex(h, s);
    return !strcmp(s, display);
}

static bool round_trip(const uint8_t *raw, size_t n, const char *txid_display)
{
    btc_tx tx;
    uint8_t out[BTC_TX_MAX_SIZE], h1[32], h2[32];
    return btc_tx_parse(&tx, raw, n) && btc_tx_size(&tx) == n && btc_tx_serialize(&tx, out, sizeof out) == n && !memcmp(out, raw, n) &&
           btc_tx_txid(&tx, h1) && same_hash(h1, txid_display) && btc_txid(raw, n, h2) && !memcmp(h1, h2, 32);
}

static void test_main_chain(void)
{
    for (size_t k = 0; k < NV(MAIN_TX); k++) {
        size_t n;
        uint8_t *raw = raw_of(MAIN_TX[k].raw, &n);
        if (!raw) continue;
        if (!round_trip(raw, n, MAIN_TX[k].txid)) {
            printf("FAIL main-chain transaction: %s\n", MAIN_TX[k].what);
            g_fails++;
        }
        free(raw);
    }
    /* The amount check, and the first payment verified with the output it spends, as pzpeer does with any input: the script of the
     * output (a P2PK one here) is the scriptCode of the signature hash. */
    size_t n9, n170;
    uint8_t *b9 = raw_of(MAIN_TX[1].raw, &n9), *b170 = raw_of(MAIN_TX[2].raw, &n170);
    uint8_t id9[32], id170[32], digest[32];
    btc_hash_from_hex(MAIN_TX[1].txid, id9);
    btc_hash_from_hex(MAIN_TX[2].txid, id170);
    uint64_t v = 0;
    const uint8_t *spk = NULL;
    size_t sl = 0;
    CHECK(btc_prevout(b9, n9, id9, 0, &v, &spk, &sl) && v == 5000000000ull && sl == 67 && spk[0] == 65 && spk[1] == 4 && spk[66] == 0xac);
    btc_tx t170;
    CHECK(btc_tx_parse(&t170, b170, n170) && t170.n_in == 1 && !memcmp(t170.in[0].prev_txid, id9, 32) && t170.in[0].prev_vout == 0);
    CHECK(btc_sighash_legacy(&t170, 0, spk, sl, digest));
    char hex[65];
    CHECK_STR(pt_hex(digest, 32, hex), B170_SIGHASH);
    const uint8_t *ss = t170.in[0].script;   /* a push of the signature with its hashtype: nothing else in a P2PK spend */
    CHECK(t170.in[0].script_len == 1 + ss[0] && ss[ss[0]] == 1);
    CHECK(btc_ecdsa_verify(digest, ss + 1, ss[0] - 1u, spk + 1, 65));
    digest[31] ^= 1;
    CHECK(!btc_ecdsa_verify(digest, ss + 1, ss[0] - 1u, spk + 1, 65));
    digest[31] ^= 1;
    uint8_t ub[65];   /* the key in the hybrid form (06/07) is refused */
    memcpy(ub, spk + 1, 65);
    ub[0] = 6 + (ub[64] & 1);
    CHECK(!btc_ecdsa_verify(digest, ss + 1, ss[0] - 1u, ub, 65));
    CHECK(btc_prevout(b170, n170, id170, 0, &v, &spk, &sl) && v == 1000000000ull && sl == 67);
    CHECK(btc_prevout(b170, n170, id170, 1, &v, &spk, &sl) && v == 4000000000ull && sl == 67);
    v = 7;
    CHECK(!btc_prevout(b170, n170, id170, 2, &v, &spk, &sl) && v == 7);    /* no such output */
    CHECK(!btc_prevout(b170, n170, id9, 0, &v, &spk, &sl));                /* not the transaction asked for */
    CHECK(!btc_prevout(b170, n170 - 1, id170, 0, &v, &spk, &sl));          /* cut */
    CHECK(!btc_prevout(NULL, 0, id170, 0, &v, &spk, &sl));
    for (size_t at = 0; at < n170; at += 7) {   /* any changed byte: another hash, or no transaction at all */
        b170[at] ^= 0x20;
        CHECK(!btc_prevout(b170, n170, id170, 0, &v, &spk, &sl) && !btc_prevout(b170, n170, id170, 1, &v, &spk, &sl));
        b170[at] ^= 0x20;
    }
    uint8_t *longer = malloc(n170 + 1);
    memcpy(longer, b170, n170);
    longer[n170] = 0;
    CHECK(!btc_prevout(longer, n170 + 1, id170, 0, &v, &spk, &sl) && !btc_txid(longer, n170 + 1, digest));
    free(longer);
    free(b9);
    free(b170);
}

static void test_segwit(void)
{
    size_t n, ns;
    uint8_t *raw = raw_of(BIP143_TX, &n), *stripped = raw_of(BIP143_STRIPPED, &ns);
    uint8_t txid[32], out[BTC_TX_MAX_SIZE], digest[32], code[64];
    btc_tx tx;
    CHECK(btc_tx_parse(&tx, raw, n) && tx.n_in == 2 && tx.n_out == 2 && tx.locktime == 0x11 && tx.in[0].sequence == 0xffffffee);
    CHECK(btc_tx_serialize(&tx, out, sizeof out) == ns && !memcmp(out, stripped, ns));   /* the witness is left out */
    CHECK(round_trip(stripped, ns, BIP143_TXID));
    CHECK(btc_txid(raw, n, txid) && same_hash(txid, BIP143_TXID));
    /* the legacy signature of its input 0 */
    const int cl = pt_unhex(BIP143_IN0_SPK, code, sizeof code);
    const uint8_t *ss = tx.in[0].script;
    CHECK(btc_sighash_legacy(&tx, 0, code, (size_t)cl, digest) && btc_ecdsa_verify(digest, ss + 1, ss[0] - 1u, code + 1, 33));
    /* the amount check reads a segwit transaction too */
    uint64_t v;
    const uint8_t *spk;
    size_t sl;
    CHECK(btc_prevout(raw, n, txid, 0, &v, &spk, &sl) && v == 112340000 && sl == 25 && btc_script_is_p2pkh(spk, sl, NULL));
    CHECK(btc_prevout(raw, n, txid, 1, &v, &spk, &sl) && v == 223450000 && sl == 25 && spk == raw + n - 4 - 108 - 25);
    /* malformed: another flag, the flag with an empty witness, a cut witness, a byte too many */
    raw[5] = 2;
    CHECK(!btc_txid(raw, n, txid) && !btc_tx_parse(&tx, raw, n));
    raw[5] = 1;
    uint8_t *empty = malloc(ns + 4);   /* marker, flag, the legacy body, two empty witnesses, lock time */
    memcpy(empty, stripped, 4);
    empty[4] = 0;
    empty[5] = 1;
    memcpy(empty + 6, stripped + 4, ns - 8);
    empty[ns - 2] = 0;
    empty[ns - 1] = 0;
    memcpy(empty + ns, stripped + ns - 4, 4);
    CHECK(!btc_txid(empty, ns + 4, txid));
    empty[ns - 1] = 1;   /* one item of length 0... which is not there: the lock time is read as its length */
    CHECK(!btc_txid(empty, ns + 4, txid));
    free(empty);
    CHECK(!btc_txid(raw, n - 5, txid) && !btc_txid(raw, 6, txid) && !btc_txid(raw, 5, txid) && !btc_txid(raw, 0, txid));
    free(raw);
    free(stripped);
}

/* a transaction written by hand: version 1, n_in inputs with scriptSigs of sig_len bytes, one output of value with a script of
 * spk_len bytes, lock time 0 */
static size_t make_raw(uint8_t *b, unsigned n_in, unsigned sig_len, uint64_t value, unsigned spk_len)
{
    size_t o = 0;
    b[o++] = 1; b[o++] = 0; b[o++] = 0; b[o++] = 0;
    b[o++] = (uint8_t)n_in;
    for (unsigned k = 0; k < n_in; k++) {
        memset(b + o, (int)k + 1, 36);
        o += 36;
        b[o++] = (uint8_t)sig_len;
        memset(b + o, 0x51, sig_len);
        o += sig_len;
        memset(b + o, 0xff, 4);
        o += 4;
    }
    b[o++] = 1;
    for (int i = 0; i < 8; i++) b[o++] = (uint8_t)(value >> 8 * i);
    b[o++] = (uint8_t)spk_len;
    memset(b + o, 0x6a, spk_len);
    o += spk_len;
    memset(b + o, 0, 4);
    return o + 4;
}

static void test_serialization(void)
{
    for (size_t k = 0; k < NV(TX_VEC); k++) {
        size_t n;
        uint8_t *raw = raw_of(TX_VEC[k].tx, &n);
        if (!raw) continue;
        if (!round_trip(raw, n, TX_VEC[k].txid)) {
            printf("FAIL random transaction %zu\n", k);
            g_fails++;
        }
        if (k == 0) CHECK(n == BTC_TX_MAX_SIZE);   /* the first one fills a btc_tx */
        free(raw);
    }
    /* beyond the capacities of a btc_tx: not parsed, but a txid all the same */
    uint8_t b[8192], h[32];
    btc_tx tx;
    size_t n = make_raw(b, 16, BTC_SCRIPTSIG_MAX, 1000, BTC_SPK_MAX);
    CHECK(btc_tx_parse(&tx, b, n) && btc_txid(b, n, h));
    n = make_raw(b, 17, 0, 1000, 25);
    CHECK(!btc_tx_parse(&tx, b, n) && btc_txid(b, n, h) && tx.n_in == 0);
    n = make_raw(b, 1, BTC_SCRIPTSIG_MAX + 1, 1000, 25);
    CHECK(!btc_tx_parse(&tx, b, n) && btc_txid(b, n, h));
    n = make_raw(b, 1, 0, 1000, BTC_SPK_MAX + 1);
    CHECK(!btc_tx_parse(&tx, b, n) && btc_txid(b, n, h));
    n = make_raw(b, 1, 0, BTC_MAX_MONEY, 25);
    CHECK(btc_tx_parse(&tx, b, n) && tx.out[0].value == BTC_MAX_MONEY);
    n = make_raw(b, 1, 0, BTC_MAX_MONEY + 1, 25);   /* not a valid transaction at all */
    CHECK(!btc_tx_parse(&tx, b, n) && !btc_txid(b, n, h));
    n = make_raw(b, 1, 0, UINT64_MAX, 25);
    CHECK(!btc_txid(b, n, h));
    /* no outputs; no inputs (read as a segwit marker with flag 1 and then garbage) */
    make_raw(b, 1, 0, 0, 0);
    b[4 + 1 + 41] = 0;   /* the output count; the zero value that followed reads as the lock time */
    CHECK(!btc_txid(b, 4 + 1 + 41 + 1 + 4, h));
    b[4 + 1 + 41] = 1;
    CHECK(btc_txid(b, 4 + 1 + 41 + 1 + 9 + 4, h));   /* the same bytes with the output: a transaction */
    n = make_raw(b, 0, 0, 1000, 25);
    CHECK(!btc_txid(b, n, h) && !btc_tx_parse(&tx, b, n));
    /* a length in a longer form than it needs: refused, as the node does */
    n = make_raw(b, 1, 0x20, 1000, 25);
    memmove(b + 4 + 36 + 3, b + 4 + 36 + 1, n - (4 + 36 + 1));
    b[4 + 36 + 1] = 0xfd;   /* 0x20 as fd 20 00 */
    b[4 + 36 + 2] = 0x20;
    b[4 + 36 + 3] = 0x00;
    CHECK(!btc_txid(b, n + 2, h));
    /* a script length beyond the bytes there are, with a huge count */
    n = make_raw(b, 1, 0, 1000, 25);
    b[4] = 0xfe; b[5] = 0xff; b[6] = 0xff; b[7] = 0xff; b[8] = 0x7f;   /* 0x7fffffff inputs: above MAX_SIZE */
    CHECK(!btc_txid(b, n, h));
    b[4] = 0xfd; b[5] = 0xff; b[6] = 0xff;                            /* 65535 inputs and not the bytes for them */
    CHECK(!btc_txid(b, n, h));

    /* the builder */
    btc_tx_init(&tx);
    uint8_t id[32] = {1}, spk[BTC_SPK_MAX + 1] = {0x76};
    CHECK(tx.version == 2 && tx.locktime == 0 && btc_tx_size(&tx) == 0 && btc_tx_serialize(&tx, b, sizeof b) == 0);
    for (int k = 0; k < BTC_MAX_IN; k++) CHECK(btc_tx_add_in(&tx, id, (uint32_t)k, BTC_SEQUENCE_FINAL));
    CHECK(!btc_tx_add_in(&tx, id, 99, 0));
    CHECK(btc_tx_size(&tx) == 0);   /* no output yet */
    CHECK(!btc_tx_add_out(&tx, 1, spk, BTC_SPK_MAX + 1) && !btc_tx_add_out(&tx, BTC_MAX_MONEY + 1, spk, 25) && !btc_tx_add_out(&tx, 1, NULL, 3));
    for (int k = 0; k < BTC_MAX_OUT; k++) CHECK(btc_tx_add_out(&tx, (uint64_t)k, spk, (size_t)k));
    CHECK(!btc_tx_add_out(&tx, 0, spk, 0));
    n = btc_tx_size(&tx);
    CHECK(n == 4 + 1 + 16 * 41 + 1 + 16 * 9 + 120 + 4 && btc_tx_serialize(&tx, b, n - 1) == 0 && btc_tx_serialize(&tx, b, n) == n);
    btc_tx back;
    CHECK(btc_tx_parse(&back, b, n) && back.n_in == 16 && back.n_out == 16 && back.in[15].prev_vout == 15 && back.out[15].script_len == 15);
    tx.in[3].script_len = BTC_SCRIPTSIG_MAX + 1;   /* a btc_tx filled by hand beyond its capacities */
    CHECK(btc_tx_size(&tx) == 0 && !btc_tx_txid(&tx, h));
}

static void test_sighash(void)
{
    btc_tx tx[NV(TX_VEC)];
    for (size_t k = 0; k < NV(TX_VEC); k++) {
        size_t n;
        uint8_t *raw = raw_of(TX_VEC[k].tx, &n);
        CHECK(raw && btc_tx_parse(&tx[k], raw, n));
        free(raw);
    }
    for (size_t k = 0; k < NV(SIGHASH_VEC); k++) {
        uint8_t code[BTC_SCRIPTCODE_MAX], digest[32];
        char hex[65];
        const int cl = pt_unhex(SIGHASH_VEC[k].code, code, sizeof code);
        CHECK(cl >= 0 && btc_sighash_legacy(&tx[SIGHASH_VEC[k].tx], SIGHASH_VEC[k].in, code, (size_t)cl, digest));
        if (strcmp(pt_hex(digest, 32, hex), SIGHASH_VEC[k].digest)) {
            printf("FAIL sighash vector %zu: %s\n", k, hex);
            g_fails++;
        }
    }
    uint8_t code[BTC_SCRIPTCODE_MAX + 1] = {0}, digest[32];
    CHECK(btc_sighash_legacy(&tx[0], 15, code, BTC_SCRIPTCODE_MAX, digest));
    CHECK(!btc_sighash_legacy(&tx[0], 16, code, 25, digest));                     /* no such input (the old "1" bug is not copied) */
    CHECK(!btc_sighash_legacy(&tx[0], 0, code, BTC_SCRIPTCODE_MAX + 1, digest));
    CHECK(!btc_sighash_legacy(&tx[0], 0, NULL, 1, digest) && btc_sighash_legacy(&tx[0], 0, NULL, 0, digest));
}

static void test_der(void)
{
    for (size_t k = 0; k < NV(DER_VEC); k++) {
        uint8_t r[32], s[32], der[BTC_DER_MAX];
        char hex[2 * BTC_DER_MAX + 1];
        pt_unhex(DER_VEC[k].r, r, 32);
        pt_unhex(DER_VEC[k].s, s, 32);
        const size_t n = btc_der_encode(r, s, der);
        CHECK_STR(n ? pt_hex(der, n, hex) : "", DER_VEC[k].der);
        CHECK(n <= 71);
    }
    /* s = n - 1 is high: it becomes 1 */
    uint8_t r[32] = {0}, s[32], der[BTC_DER_MAX];
    r[31] = 5;
    memcpy(s, ORDER, 32);
    s[31] -= 1;
    CHECK(btc_der_encode(r, s, der) == 8 && !memcmp(der, "\x30\x06\x02\x01\x05\x02\x01\x01", 8));
}

/* r and s of the DER signature at der, the lengths already known to be right */
static void der_rs(const uint8_t *der, uint8_t r[32], uint8_t s[32])
{
    const size_t rl = der[3], sl = der[5 + rl];
    memset(r, 0, 32);
    memset(s, 0, 32);
    memcpy(r + 32 - (rl > 32 ? 32 : rl), der + 4 + (rl > 32), rl > 32 ? 32 : rl);
    memcpy(s + 32 - (sl > 32 ? 32 : sl), der + 6 + rl + (sl > 32), sl > 32 ? 32 : sl);
}

/* a 32-byte big-endian integer as a DER INTEGER: its minimal bytes, with a zero before a top bit that is set */
static size_t der_int(uint8_t *o, const uint8_t v[32])
{
    size_t i = 0;
    while (i < 31 && !v[i]) i++;
    const size_t pad = v[i] >> 7, n = 32 - i;
    o[0] = 2;
    o[1] = (uint8_t)(n + pad);
    o[2] = 0;
    memcpy(o + 2 + pad, v + i, n);
    return 2 + pad + n;
}

/* n - s, big-endian */
static void negate(uint8_t s[32])
{
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        const int d = ORDER[i] - s[i] - borrow;
        s[i] = (uint8_t)d;
        borrow = d < 0;
    }
}

static void test_sign(void)
{
    for (size_t k = 0; k < NV(SIGN_VEC); k++) {
        size_t nu, ns;
        uint8_t *uraw = raw_of(SIGN_VEC[k].unsigned_tx, &nu), *sraw = raw_of(SIGN_VEC[k].signed_tx, &ns);
        btc_tx tx, put, want;
        uint8_t out[BTC_TX_MAX_SIZE], key[32], spk[4][BTC_P2PKH_LEN];
        CHECK(btc_tx_parse(&tx, uraw, nu) && btc_tx_parse(&want, sraw, ns) && tx.n_in == SIGN_VEC[k].n);
        put = tx;
        for (size_t i = 0; i < tx.n_in; i++) {
            pt_unhex(SIGN_VEC[k].key[i], key, 32);
            pt_unhex(SIGN_VEC[k].spk[i], spk[i], BTC_P2PKH_LEN);
            CHECK(!btc_verify_p2pkh(&tx, i, spk[i], BTC_P2PKH_LEN));   /* unsigned */
            CHECK(btc_sign_p2pkh(&tx, i, key));
            /* the same signature through the path of the two-party signer, given with a high s: it comes out low */
            const uint8_t *ss = want.in[i].script;
            uint8_t r[32], s[32];
            der_rs(ss + 1, r, s);
            negate(s);
            CHECK(btc_put_sig_p2pkh(&put, i, r, s, ss + 2 + ss[0]));
        }
        CHECK(btc_tx_serialize(&tx, out, sizeof out) == ns && !memcmp(out, sraw, ns));
        CHECK(btc_tx_serialize(&put, out, sizeof out) == ns && !memcmp(out, sraw, ns));
        for (size_t i = 0; i < tx.n_in; i++) {
            CHECK(btc_verify_p2pkh(&tx, i, spk[i], BTC_P2PKH_LEN));
            CHECK(!btc_verify_p2pkh(&tx, i, spk[(i + 1) % tx.n_in], BTC_P2PKH_LEN) || tx.n_in == 1 ||
                  !memcmp(spk[i], spk[(i + 1) % tx.n_in], BTC_P2PKH_LEN));
        }
        /* SIGHASH_ALL covers the outputs, the other inputs and the lock time */
        btc_tx bad = tx;
        bad.out[0].value++;
        for (size_t i = 0; i < tx.n_in; i++) CHECK(!btc_verify_p2pkh(&bad, i, spk[i], BTC_P2PKH_LEN));
        bad = tx;
        bad.locktime ^= 1;
        CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        if (tx.n_in > 1) {
            bad = tx;
            bad.in[1].sequence ^= 1;
            CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
            bad = tx;
            bad.in[1].script_len = 0;   /* the scriptSigs of the others are not covered */
            CHECK(btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        }
        /* the form of the scriptSig: another hashtype, a byte more or less, a high s */
        bad = tx;
        btc_in *in = &bad.in[0];
        in->script[in->script[0]] = 0x81;
        CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        bad = tx;
        in->script[in->script_len++] = 0;
        CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        bad = tx;
        in->script_len--;
        CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        bad = tx;
        in->script_len = 0;
        CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
        {
            /* the same r and s with s high, in DER by hand */
            const uint8_t *ss = tx.in[0].script;
            uint8_t r[32], s[32], der[80];
            der_rs(ss + 1, r, s);
            negate(s);
            size_t o = 2;
            o += der_int(der + o, r);
            o += der_int(der + o, s);
            der[0] = 0x30;
            der[1] = (uint8_t)(o - 2);
            der[o++] = 1;   /* the hashtype */
            in->script[0] = (uint8_t)o;
            memcpy(in->script + 1, der, o);
            in->script[1 + o] = 33;
            memcpy(in->script + 2 + o, ss + 2 + ss[0], 33);
            in->script_len = (uint8_t)(1 + o + 1 + 33);
            CHECK(!btc_verify_p2pkh(&bad, 0, spk[0], BTC_P2PKH_LEN));
            negate(s);   /* and with the low s, the same bytes are the signature btc_sign_p2pkh made */
            o = 2;
            o += der_int(der + o, r);
            o += der_int(der + o, s);
            der[1] = (uint8_t)(o - 2);
            CHECK(o + 1 == ss[0] && !memcmp(der, ss + 1, o));
        }
        /* the estimate is an upper bound, at most a few bytes above */
        const size_t est = btc_vsize_p2pkh(tx.n_in, tx.out, tx.n_out);
        CHECK(est >= ns && est - ns <= 2 * tx.n_in);
        free(uraw);
        free(sraw);
    }

    /* an uncompressed key, signed by libsecp256k1 directly: verify takes the 65-byte form too */
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t key[32] = {0}, pub[65], h[20], spk[BTC_P2PKH_LEN], digest[32], der[BTC_DER_MAX];
    key[31] = 3;
    secp256k1_pubkey pk;
    secp256k1_ecdsa_signature sig;
    size_t pl = 65, dl = sizeof der;
    CHECK(secp256k1_ec_pubkey_create(ctx, &pk, key) && secp256k1_ec_pubkey_serialize(ctx, pub, &pl, &pk, SECP256K1_EC_UNCOMPRESSED));
    pz_hash160(pub, 65, h);
    btc_script_p2pkh(h, spk);
    btc_tx tx;
    btc_tx_init(&tx);
    uint8_t id[32];
    memset(id, 0xaa, 32);
    btc_tx_add_in(&tx, id, 1, BTC_SEQUENCE_FINAL);
    btc_tx_add_out(&tx, 5000, spk, sizeof spk);
    CHECK(btc_sighash_legacy(&tx, 0, spk, sizeof spk, digest) && secp256k1_ecdsa_sign(ctx, &sig, digest, key, NULL, NULL));
    CHECK(secp256k1_ecdsa_signature_serialize_der(ctx, der, &dl, &sig));
    btc_in *in = &tx.in[0];
    in->script[0] = (uint8_t)(dl + 1);
    memcpy(in->script + 1, der, dl);
    in->script[1 + dl] = 1;
    in->script[2 + dl] = 65;
    memcpy(in->script + 3 + dl, pub, 65);
    in->script_len = (uint8_t)(3 + dl + 65);
    CHECK(btc_verify_p2pkh(&tx, 0, spk, sizeof spk));
    /* signed with btc_sign_p2pkh instead, it is the compressed key's: not the owner of this script */
    CHECK(btc_sign_p2pkh(&tx, 0, key) && !btc_verify_p2pkh(&tx, 0, spk, sizeof spk));
    secp256k1_context_destroy(ctx);

    /* refusals */
    uint8_t zero[32] = {0}, rs[32] = {1}, g[33];
    pt_unhex(G33, g, 33);
    CHECK(!btc_sign_p2pkh(&tx, 0, zero) && !btc_sign_p2pkh(&tx, 0, ORDER) && !btc_sign_p2pkh(&tx, 1, key));
    CHECK(btc_put_sig_p2pkh(&tx, 0, rs, rs, g) && !btc_put_sig_p2pkh(&tx, 1, rs, rs, g));
    g[0] = 4;
    CHECK(!btc_put_sig_p2pkh(&tx, 0, rs, rs, g));
    g[0] = 2;
    CHECK(!btc_put_sig_p2pkh(&tx, 0, zero, rs, g) && !btc_put_sig_p2pkh(&tx, 0, rs, ORDER, g));
    CHECK(!btc_verify_p2pkh(&tx, 0, spk, sizeof spk - 1) && !btc_verify_p2pkh(&tx, 1, spk, sizeof spk));
    CHECK(!btc_ecdsa_verify(digest, der, 7, pub, 65) && !btc_ecdsa_verify(digest, der, dl, pub, 64) && !btc_ecdsa_verify(digest, der, dl, pub, 33));
}

static void test_size(void)
{
    btc_out out[3] = {{.script_len = 25}, {.script_len = 22}, {.script_len = 34}};
    CHECK(btc_vsize_p2pkh(1, out, 1) == 4 + 1 + 148 + 1 + 34 + 4);
    CHECK(btc_vsize_p2pkh(2, out + 1, 2) == 4 + 1 + 296 + 1 + 31 + 43 + 4);
    CHECK(btc_fee(192, 1000) == 192 && btc_fee(192, 1001) == 193 && btc_fee(0, 5000) == 0 && btc_fee(1, 1) == 1 && btc_fee(250, 0) == 0);
    CHECK(btc_fee(100000, 100000000) == 10000000000ull);
}

/* Mutations of real transactions: no read or write outside the buffers whatever they are, and the functions agree with each other. */
static void test_mutations(void)
{
    const char *seeds[] = {MAIN_TX[2].raw, BIP143_TX, TX_VEC[1].tx, SIGN_VEC[1].signed_tx};
    uint64_t x = 0x243f6a8885a308d3ull;
    for (size_t k = 0; k < NV(seeds); k++) {
        size_t n;
        uint8_t *orig = raw_of(seeds[k], &n);
        for (int m = 0; m < 4000; m++) {
            uint8_t *b = malloc(n);
            memcpy(b, orig, n);
            size_t len = n;
            for (int c = 0; c < 1 + m % 3; c++) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const size_t at = x % len;
                if ((x >> 40) % 4) b[at] = (uint8_t)(x >> 48);
                else len = at ? at : 1;
            }
            btc_tx tx;
            uint8_t h1[32], h2[32];
            const bool parsed = btc_tx_parse(&tx, b, len), hashed = btc_txid(b, len, h1);
            CHECK(!parsed || (hashed && btc_tx_txid(&tx, h2) && !memcmp(h1, h2, 32)));
            if (hashed) {
                uint64_t v;
                const uint8_t *spk;
                size_t sl;
                CHECK(btc_prevout(b, len, h1, 0, &v, &spk, &sl) && spk >= b && spk + sl <= b + len && v <= BTC_MAX_MONEY);
            }
            free(b);
        }
        free(orig);
    }
}

int main(void)
{
    test_hex();
    test_address();
    test_main_chain();
    test_segwit();
    test_serialization();
    test_sighash();
    test_der();
    test_sign();
    test_size();
    test_mutations();
    TEST_DONE("t_btc");
}
