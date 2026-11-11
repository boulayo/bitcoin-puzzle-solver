/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_spv.h - the chain of a session (session_chain) without a node of one's own (docs/en/NETWORK.md 8.2): several Electrum servers, none
 * of them trusted. The headers they give are kept only with their proof of work (spv.h); a coin counts only with the transaction that
 * made it and its Merkle proof; a coin is spent only when a server shows the transaction that spends it, confirmed with its proof or, while
 * it waits in the mempool, valid in every signature. What a lying server can still do is hide things: a delay, never a wrong payment. */
#ifndef PZPEER_CHAIN_SPV_H
#define PZPEER_CHAIN_SPV_H

#include "electrum.h"
#include "session.h"
#include "spv.h"
#include <pthread.h>

#define CHAIN_SPV_SERVERS 8

typedef struct {
    btc_chain chain;
    const char *servers[CHAIN_SPV_SERVERS];   /* "host:port" */
    int nservers;
    electrum conn[CHAIN_SPV_SERVERS];         /* the connections, kept from one operation to the next */
    int64_t retry[CHAIN_SPV_SERVERS];         /* a server that did not answer is left alone until this time */
    pthread_mutex_t io;                       /* one operation at a time on them */
    spv_chain headers;
    pthread_mutex_t mu;                       /* the headers: sessions share them */
    const char *headers_path;                 /* where they are kept between runs (NULL: nowhere) */
    uint64_t fallback_feerate;                /* when no server gives an estimate: satoshis per 1000 virtual bytes */
    int timeout_ms;                           /* of one call to a server; 0: 10000 */
    unsigned max_tip_age;                     /* a chain whose last block is older than this many seconds is not ready (0: any) */
} chain_spv;

/* cp_hash NULL: the genesis block of the chain (cp_height 0). False for a checkpoint of the main chain that is not on a period boundary. */
bool chain_spv_init(chain_spv *c, btc_chain chain, uint64_t cp_height, const uint8_t *cp_hash, const char *const *servers, int nservers,
                    const char *headers_path);
/* What a network of the main chain uses when nothing else is given: public Electrum servers that take plain TCP, of different operators
 * (from the list the Electrum wallet ships; none is trusted), into servers (CHAIN_SPV_SERVERS entries; NULL: not wanted), and the block the
 * headers start from, the first of a difficulty period. The number of servers. */
int chain_spv_defaults(const char **servers, uint64_t *cp_height, uint8_t cp_hash[32]);
void chain_spv_free(chain_spv *c);
session_chain chain_spv_iface(chain_spv *c);
/* Brings the headers up to what the servers have. 1: ready; 0: not now (no server answers, or the chain they give ends too long ago),
 * with the reason in why. */
int chain_spv_ready(chain_spv *c, char *why, size_t cap);

#endif
