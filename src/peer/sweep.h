/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sweep.h - pzpeer sweep: the transaction that moves the coins of a key found by a linear search to an address of the finder. It is made
 * and signed here and sent nowhere: until it is spent, nobody knows the public key of such an address, and a transaction that waits in
 * the public mempool shows it to everyone. The range of the key is small, so a program finds the private key from the public one in
 * seconds and replaces the transaction with its own. The finder gives the transaction straight to a mining pool that takes private
 * transactions; pzpeer only prints it. */
#ifndef PZPEER_SWEEP_H
#define PZPEER_SWEEP_H

#include "session.h"

typedef struct {
    uint64_t rate;                 /* in: satoshis per 1000 virtual bytes, 0 for twice the estimate of the chain, 50 to 1000 sat/vB; out: used */
    uint64_t total;                /* out: the satoshis of the coins taken */
    int listed;                    /* out: the coins of the address the chain showed */
} sweep_info;

/* The confirmed coins of the key to the script to (session_coins: a scan of its address and the named coins, the prize= of a network
 * file or of the table of the program): the largest, as many as a transaction holds, each worth more than the fee of its own input (the
 * address of a puzzle collects dozens of coins of dust), less a fee that passes at once. With author, the share of the author of what
 * the fee leaves goes to its script (payout_author). Signed and checked: NULL, or why there is no transaction.
 * A transaction that spends a part of the coins shows the public key and leaves the rest to whoever is first, so a chain read from
 * servers (pending is offered) must show them all: it is refused when it did not answer about the address or about a named coin (a
 * server hid it), and when no coin is named, as servers cannot show a coin older than their checkpoint. A node of one's own sees every
 * coin, and its sweep takes what it lists. */
const char *sweep_tx(const session_chain *ch, const uint8_t sec[32], const payout_coin *named, int nnamed, const uint8_t *to, size_t to_len,
                     const payout_terms *author, sweep_info *info, btc_tx *tx);

/* The pool --private gives the transaction to: MARA's Slipstream, free and open to all since August 2026, which mines it without showing it
 * in the public mempool. Its interface is not documented by MARA: this is the one its own page and the programs of others use. */
#define SWEEP_POOL "https://slipstream.mara.com/api/transactions"

/* pzpeer sweep --to ADDRESS [--chain NAME] [--rpc ... | --electrum HOST:PORT ...] [--feerate SAT_PER_VBYTE] [--private | --pool URL]; the
 * key (64 hex digits) is read from the standard input, never from the command line, where the list of processes would show it. Without
 * --private or --pool the transaction is printed and NOT sent; with them it goes to that pool only, never to the public network. On the
 * main chain the named coins are those of the table of the program for the address of the key (an open puzzle). */
int sweep_main(int argc, char **argv);

#endif
