/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sweep.h - pzpeer sweep: the transaction that moves the coins of a key found by a linear search to an address of the finder. It is made
 * and signed here and sent nowhere: until it is spent, nobody knows the public key of such an address, and a transaction that waits in
 * the public mempool shows it to everyone. The range of the key is small, so a program finds the private key from the public one in
 * seconds and replaces the transaction with its own. The finder gives the transaction straight to a mining pool that takes private
 * transactions; pzpeer only prints it. */
#ifndef PZPEER_SWEEP_H
#define PZPEER_SWEEP_H

/* pzpeer sweep --to ADDRESS [--chain NAME] [--rpc ... | --electrum HOST:PORT ...] [--feerate SAT_PER_VBYTE]; the key (64 hex digits) is
 * read from the standard input, never from the command line, where the list of processes would show it. */
int sweep_main(int argc, char **argv);

#endif
