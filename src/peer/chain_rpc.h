/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_rpc.h - the chain of a session (session_chain) on the home's own bitcoind, through its JSON-RPC: the coins of an address, the
 * broadcast, the confirmations of an output, the height and the fee rate. The node is trusted (docs/en/NETWORK.md 8): it validated
 * every block, and what it says of its own set of unspent outputs is the chain, values included; a pruned node serves as well as a
 * full one. */
#ifndef PZPEER_CHAIN_RPC_H
#define PZPEER_CHAIN_RPC_H

#include "btc_rpc.h"
#include "session.h"

typedef struct {
    btc_rpc *rpc;
    btc_chain chain;
    unsigned conf_target;          /* blocks for estimatesmartfee (6) */
    uint64_t fallback_feerate;     /* when the node has no estimate (regtest, a node just started): satoshis per 1000 virtual bytes */
    uint64_t tip;                  /* the last height the node gave: the height while it does not answer */
} chain_rpc;

/* The interface on c, which must outlive it. */
session_chain chain_rpc_iface(chain_rpc *c);

/* Whether the node can serve a session: 1 yes; 0 not now (it does not answer, it is still syncing, or its blocks are behind the
 * headers it knows); -1 never (it is on another chain than c->chain). why gets the reason, or where the node is (cap bytes). A
 * session does not start on a node that is behind: the coins it reports may be spent already, or not there yet. */
int chain_rpc_ready(chain_rpc *c, char *why, size_t cap);

#endif
