/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_rpc.h - the chain of a session (session_chain) on a bitcoind, through its JSON-RPC: the coins of an address with their values checked
 * against the transactions that made them, the broadcast, the confirmations of an output, the height and the fee rate. */
#ifndef PZPEER_CHAIN_RPC_H
#define PZPEER_CHAIN_RPC_H

#include "btc_rpc.h"
#include "session.h"

typedef struct {
    btc_rpc *rpc;
    btc_chain chain;
    unsigned conf_target;          /* blocks for estimatesmartfee (6) */
    uint64_t fallback_feerate;     /* when the node has no estimate (regtest, a node just started): satoshis per 1000 virtual bytes */
} chain_rpc;

/* The interface on c, which must outlive it. */
session_chain chain_rpc_iface(chain_rpc *c);

#endif
