/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* payout.h - the transactions of a pair that holds the key in two pieces (project 2.4, docs/en/NETWORK.md 8). Both homes build them on their
 * own from the same terms, so the same terms give the same bytes:
 *   first:  the coins of the puzzle (at most 8) back to the puzzle address, less the fee and a tiny coin for each party;
 *   second: the prize of the first and the tiny coin of party F, half each to the payout addresses, the tiny coin back to F;
 *   cancel: F alone spends its tiny coin, which makes a second transaction held back by the partner worthless. */
#ifndef PZPEER_PAYOUT_H
#define PZPEER_PAYOUT_H

#include "btc.h"

#define PAYOUT_TINY_MIN 10000      /* satoshis of a tiny coin, at least */
#define PAYOUT_MAX_COINS 8         /* inputs of the puzzle in one transaction: every one is a two-party signing */

typedef struct {
    uint8_t txid[32];              /* internal order */
    uint32_t vout;
    uint64_t value;                /* verified from the transaction that made it (btc_prevout) */
} payout_coin;

typedef struct {
    uint8_t puzzle_spk[BTC_P2PKH_LEN];    /* P2PKH of the compressed key of the puzzle */
    uint8_t coin_spk[2][BTC_P2PKH_LEN];   /* the tiny coins of the parties (P2PKH of their coin keys) */
    uint8_t pay_spk[2][BTC_SPK_MAX];      /* the payout addresses of the parties */
    size_t pay_len[2];
    uint64_t feerate;                     /* satoshis per 1000 virtual bytes */
    int first;                            /* F, the party whose tiny coin goes into the second transaction */
} payout_terms;

/* The first transaction from the coins of the puzzle: the largest first (ties by outpoint), at most PAYOUT_MAX_COINS, without the coins worth
 * less than 3 times the fee of their input. *used: how many were taken. False when what is taken does not pay the fee and the tiny coins. */
bool payout_first(const payout_terms *t, const payout_coin *coins, size_t n, btc_tx *tx, size_t *used);
/* The second transaction, from the first as it was confirmed: a legacy txid covers the signatures, and a third party can still change the
 * encoding of one before a miner takes it, so the second is built only from the first that is in a block. */
bool payout_second(const payout_terms *t, const btc_tx *first, btc_tx *tx);
/* The cancel of party F: its tiny coin of the first transaction to its own payout address, less the fee. */
bool payout_cancel(const payout_terms *t, const btc_tx *first, btc_tx *tx);
/* The satoshis of a tiny coin: twice the fee of F's cancel at the fee rate of the terms and the dust limit, PAYOUT_TINY_MIN at least. F
 * can therefore always cancel, whatever the rate; were its coin worth less than that fee, S could hold the second transaction back for free. */
uint64_t payout_tiny(const payout_terms *t);
/* The digests of the first n inputs of tx, which spend the puzzle address (all the inputs of the first transaction, input 0 of the second):
 * what pzsign signs, in this order. */
bool payout_digests(const btc_tx *tx, const uint8_t puzzle_spk[BTC_P2PKH_LEN], size_t n, uint8_t (*digests)[32]);

#endif
