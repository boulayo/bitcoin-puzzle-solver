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
    int listed;                    /* out: the coins the chain listed, -1 when it did not answer */
} sweep_info;

/* The confirmed coins of the key to the script to: the largest, as many as a transaction holds, each worth more than the fee of its own
 * input (the address of a puzzle collects dozens of coins of dust), less a fee that passes at once. With author, the share of the author
 * of what the fee leaves goes to its script (payout_author). Signed and checked. */
bool sweep_tx(const session_chain *ch, const uint8_t sec[32], const uint8_t *to, size_t to_len, const payout_terms *author, sweep_info *info,
              btc_tx *tx);

/* pzpeer sweep --to ADDRESS [--chain NAME] [--rpc ... | --electrum HOST:PORT ...] [--feerate SAT_PER_VBYTE]; the key (64 hex digits) is
 * read from the standard input, never from the command line, where the list of processes would show it. */
int sweep_main(int argc, char **argv);

#endif
