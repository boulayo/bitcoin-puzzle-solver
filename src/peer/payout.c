/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* payout.c - the transactions of a pair (see payout.h). */
#include "payout.h"
#include <stdlib.h>
#include <string.h>

static int by_value(const void *a, const void *b)
{
    const payout_coin *x = a, *y = b;
    if (x->value != y->value) return x->value > y->value ? -1 : 1;
    const int c = memcmp(x->txid, y->txid, 32);
    return c ? c : (x->vout > y->vout) - (x->vout < y->vout);
}

size_t payout_largest(payout_coin *coins, size_t n, size_t max, uint64_t least)
{
    size_t k = 0;
    qsort(coins, n, sizeof *coins, by_value);
    while (k < n && k < max && coins[k].value >= least) k++;
    return k;
}

/* the output of F's cancel and its fee */
static uint64_t cancel_out(const payout_terms *t, btc_out *out)
{
    *out = (btc_out){.script_len = (uint8_t)t->pay_len[t->first]};
    memcpy(out->script, t->pay_spk[t->first], t->pay_len[t->first]);
    return btc_fee(btc_vsize_p2pkh(1, out, 1), t->feerate);
}

/* the fee of the second transaction */
static uint64_t second_fee(const payout_terms *t)
{
    btc_out out[2] = {{0}};
    for (int p = 0; p < 2; p++) out[p].script_len = (uint8_t)t->pay_len[p], memcpy(out[p].script, t->pay_spk[p], t->pay_len[p]);
    return btc_fee(btc_vsize_p2pkh(2, out, 2), t->feerate);
}

uint64_t payout_tiny(const payout_terms *t)
{
    btc_out out;
    const uint64_t need = 2 * cancel_out(t, &out) + 546;
    return need > PAYOUT_TINY_MIN ? need : PAYOUT_TINY_MIN;
}

bool payout_first(const payout_terms *t, const payout_coin *coins, size_t n, btc_tx *tx, size_t *used)
{
    payout_coin take[64];
    if (t->first != 0 && t->first != 1) return false;
    const uint64_t tiny = payout_tiny(t);
    if (n > 64) n = 64;
    memcpy(take, coins, n * sizeof *coins);
    const size_t worth = payout_largest(take, n, PAYOUT_MAX_COINS, 3 * btc_fee(BTC_P2PKH_IN_SIZE, t->feerate));
    uint64_t total = 0;
    btc_tx_init(tx);
    for (size_t i = 0; i < worth; i++) {
        if (total + take[i].value > BTC_MAX_MONEY || !btc_tx_add_in(tx, take[i].txid, take[i].vout, BTC_SEQUENCE_FINAL)) return false;
        total += take[i].value;
    }
    *used = tx->n_in;
    btc_out out[2] = {{0}};                                     /* the outputs as they will be, for the size */
    for (int k = 0; k < 2; k++) {
        out[k].script_len = BTC_P2PKH_LEN;
        memcpy(out[k].script, k ? t->coin_spk[t->first] : t->puzzle_spk, BTC_P2PKH_LEN);
    }
    const uint64_t fee = btc_fee(btc_vsize_p2pkh(tx->n_in, out, 2), t->feerate), second = second_fee(t) + 2 * 546;
    if (!tx->n_in || total < fee + tiny + (second > tiny ? second : tiny)) return false;   /* the prize must stay larger than a tiny
                                                                                           * coin, and pay for the second transaction */
    return btc_tx_add_out(tx, total - fee - tiny, t->puzzle_spk, BTC_P2PKH_LEN) &&
           btc_tx_add_out(tx, tiny, t->coin_spk[t->first], BTC_P2PKH_LEN);
}

/* the outputs of the first transaction: the prize at 0, the tiny coin of F at 1 */
static bool first_ok(const payout_terms *t, const btc_tx *first)
{
    return (t->first == 0 || t->first == 1) && first->n_out == 2 && first->out[0].script_len == BTC_P2PKH_LEN &&
           !memcmp(first->out[0].script, t->puzzle_spk, BTC_P2PKH_LEN) && first->out[1].value == payout_tiny(t) &&
           first->out[1].script_len == BTC_P2PKH_LEN && !memcmp(first->out[1].script, t->coin_spk[t->first], BTC_P2PKH_LEN);
}

bool payout_second(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    if (!first_ok(t, first) || !btc_tx_txid(first, txid)) return false;
    btc_tx_init(tx);
    /* the prize and F's tiny coin, which came from the prize too, half each: the odd satoshi to S */
    const uint64_t all = first->out[0].value + first->out[1].value, fee = second_fee(t);
    if (all < fee + 2 * 546) return false;                      /* each half above the dust limit */
    const uint64_t half = (all - fee) / 2, v[2] = {t->first == 0 ? half : all - fee - half, t->first == 1 ? half : all - fee - half};
    return btc_tx_add_in(tx, txid, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_in(tx, txid, 1, BTC_SEQUENCE_FINAL) &&
           btc_tx_add_out(tx, v[0], t->pay_spk[0], t->pay_len[0]) && btc_tx_add_out(tx, v[1], t->pay_spk[1], t->pay_len[1]);
}

bool payout_cancel(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    btc_out out;
    if (!first_ok(t, first) || t->pay_len[t->first] > BTC_SPK_MAX || !btc_tx_txid(first, txid)) return false;
    const uint64_t fee = cancel_out(t, &out);
    btc_tx_init(tx);
    return btc_tx_add_in(tx, txid, 1, BTC_SEQUENCE_FINAL) && btc_tx_add_out(tx, payout_tiny(t) - fee, out.script, out.script_len);
}

bool payout_digests(const btc_tx *tx, const uint8_t puzzle_spk[BTC_P2PKH_LEN], size_t n, uint8_t (*digests)[32])
{
    if (n > tx->n_in) return false;
    for (size_t i = 0; i < n; i++)
        if (!btc_sighash_legacy(tx, i, puzzle_spk, BTC_P2PKH_LEN, digests[i])) return false;
    return true;
}
