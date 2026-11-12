/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_payout.c - the transactions of a pair: which coins go in, the amounts, and that the signed transactions are valid spends (signed here
 * with the whole key of a test puzzle and the coin keys, as pzsign and each home would sign them). */
#include "peertest.h"
#include "payout.h"
#include <pzcore/hash.h>
#include <secp256k1.h>

static secp256k1_context *ctx;

static void key(const char *label, uint8_t sec[32], uint8_t spk[BTC_P2PKH_LEN])
{
    secp256k1_pubkey p;
    uint8_t pub[33], h[20];
    size_t n = 33;
    pz_sha256((const uint8_t *)label, strlen(label), sec);
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, sec) && secp256k1_ec_pubkey_serialize(ctx, pub, &n, &p, SECP256K1_EC_COMPRESSED));
    pz_hash160(pub, 33, h);
    btc_script_p2pkh(h, spk);
}

static uint64_t sum_out(const btc_tx *tx)
{
    uint64_t s = 0;
    for (size_t i = 0; i < tx->n_out; i++) s += tx->out[i].value;
    return s;
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t puzzle[32], coin[2][32];
    payout_terms t = {.feerate = 2000, .first = 1};             /* 2 sat/vB */
    key("puzzle", puzzle, t.puzzle_spk);
    key("coin 0", coin[0], t.coin_spk[0]);
    key("coin 1", coin[1], t.coin_spk[1]);
    uint8_t dummy[32];
    key("payout 0", dummy, t.pay_spk[0]), t.pay_len[0] = BTC_P2PKH_LEN;
    t.pay_len[1] = btc_address_script(BTC_REGTEST, "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080", t.pay_spk[1]);
    CHECK(t.pay_len[1] == 22);
    /* ten coins: the prize, nine smaller, two of them dust that is not worth its input */
    payout_coin coins[10];
    for (int i = 0; i < 10; i++) {
        pz_sha256((const uint8_t *)&i, sizeof i, coins[i].txid);
        coins[i].vout = (uint32_t)i;
        coins[i].value = i == 3 ? 100000000 : i < 2 ? 500 : 50000 + 1000 * (uint64_t)i;
    }
    btc_tx a, b, c;
    size_t used;
    CHECK(payout_first(&t, coins, 10, &a, &used));
    CHECK(used == PAYOUT_MAX_COINS && a.n_in == 8 && a.n_out == 3);
    CHECK(!memcmp(a.in[0].prev_txid, coins[3].txid, 32));        /* the largest first */
    uint64_t in = 0;
    for (size_t i = 0; i < a.n_in; i++) {
        in += coins[a.in[i].prev_vout].value;
        CHECK(a.in[i].prev_vout >= 2);                           /* no dust */
    }
    const uint64_t fee_a = in - sum_out(&a);
    CHECK(a.out[1].value == PAYOUT_TINY_MIN && a.out[2].value == PAYOUT_TINY_MIN && fee_a >= btc_fee(btc_tx_size(&a) + 8 * 107, 2000));
    CHECK(fee_a < 4000);                                         /* about 1.3 kB at 2 sat/vB */
    uint8_t dg[8][32];
    CHECK(payout_digests(&a, t.puzzle_spk, a.n_in, dg));
    for (size_t i = 0; i < a.n_in; i++) {                        /* signed as pzsign would: the digest by the whole key */
        CHECK(btc_sign_p2pkh(&a, i, puzzle) && btc_verify_p2pkh(&a, i, t.puzzle_spk, BTC_P2PKH_LEN));
        uint8_t again[32];
        CHECK(btc_sighash_legacy(&a, i, t.puzzle_spk, BTC_P2PKH_LEN, again) && !memcmp(again, dg[i], 32));
    }
    btc_tx a2;                                                   /* the same terms and coins in another order: the same transaction */
    payout_coin rev[10];
    for (int i = 0; i < 10; i++) rev[i] = coins[9 - i];
    CHECK(payout_first(&t, rev, 10, &a2, &used) && a2.n_in == 8);
    for (size_t i = 0; i < 8; i++) CHECK(!memcmp(a2.in[i].prev_txid, a.in[i].prev_txid, 32) && a2.in[i].prev_vout == a.in[i].prev_vout);
    /* the second: the prize and the tiny coin of F (party 1), half each, the tiny coin back to F */
    uint8_t aid[32];
    CHECK(btc_tx_txid(&a, aid));
    CHECK(payout_second(&t, &a, &b) && b.n_in == 2 && b.n_out == 2);
    CHECK(!memcmp(b.in[0].prev_txid, aid, 32) && b.in[0].prev_vout == 0 && b.in[1].prev_vout == 2);
    const uint64_t fee_b = a.out[0].value + PAYOUT_TINY_MIN - sum_out(&b);
    CHECK(b.out[1].value - PAYOUT_TINY_MIN - b.out[0].value <= 1 && fee_b < 1000);
    CHECK(btc_sign_p2pkh(&b, 0, puzzle) && btc_sign_p2pkh(&b, 1, coin[1]));
    CHECK(btc_verify_p2pkh(&b, 0, t.puzzle_spk, BTC_P2PKH_LEN) && btc_verify_p2pkh(&b, 1, t.coin_spk[1], BTC_P2PKH_LEN));
    CHECK(!btc_verify_p2pkh(&b, 1, t.coin_spk[0], BTC_P2PKH_LEN));
    /* the cancel spends the same tiny coin: the two cannot both be in the chain */
    CHECK(payout_cancel(&t, &a, &c) && c.n_in == 1 && c.n_out == 1 && c.in[0].prev_vout == b.in[1].prev_vout);
    CHECK(!memcmp(c.out[0].script, t.pay_spk[1], t.pay_len[1]) && c.out[0].value > PAYOUT_TINY_MIN - 1000);
    CHECK(btc_sign_p2pkh(&c, 0, coin[1]) && btc_verify_p2pkh(&c, 0, t.coin_spk[1], BTC_P2PKH_LEN));
    /* refusals: nothing worth spending, a first transaction that is not one */
    CHECK(!payout_first(&t, coins, 2, &a2, &used));
    btc_tx bad = a;
    bad.out[1].value++;
    CHECK(!payout_second(&t, &bad, &b) && !payout_cancel(&t, &bad, &c));
    /* a high fee rate makes the tiny coins larger: F's cancel always leaves at least its own fee and the dust limit */
    payout_terms hot = t;
    hot.feerate = 500000;                                        /* 500 sat/vB */
    btc_tx ha, hc;
    CHECK(payout_tiny(&t) == PAYOUT_TINY_MIN && payout_tiny(&hot) > 150000);
    CHECK(payout_first(&hot, coins, 10, &ha, &used) && ha.out[1].value == payout_tiny(&hot) && ha.out[2].value == payout_tiny(&hot));
    CHECK(payout_cancel(&hot, &ha, &hc) && hc.out[0].value > payout_tiny(&hot) / 2 && !payout_cancel(&hot, &a, &hc));
    hot.first = 2;
    CHECK(!payout_first(&hot, coins, 10, &ha, &used) && !payout_cancel(&hot, &ha, &hc));
    /* a first transaction only when its prize pays for the second: at 25 sat/vB with P2PKH payouts the second needs more than a tiny coin
     * (before, a prize of a few hundred satoshis more than the tiny coin had its first transaction and never a second) */
    payout_terms w = t;
    w.feerate = 25000, w.pay_len[1] = BTC_P2PKH_LEN, memcpy(w.pay_spk[1], w.pay_spk[0], BTC_P2PKH_LEN);
    int built = 0;
    for (uint64_t v = 30000; v < 60000; v += 7) {
        payout_coin one = coins[3];
        btc_tx f, s2;
        one.value = v;
        const bool first = payout_first(&w, &one, 1, &f, &used);
        built += first;
        CHECK(!first || payout_second(&w, &f, &s2));
    }
    CHECK(built > 0);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_payout");
}
