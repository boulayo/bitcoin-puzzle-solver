/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* bt.h - finding the first nodes of a network with no server of its own (docs/en/NETWORK.md 7.9): the nodes announce themselves in the
 * distributed table of BitTorrent (Mainline DHT, BEP 5) under a key made from the fingerprint of the network, and look each other up
 * there. Only addresses pass through it, and none is trusted: each is a candidate that the table of pzpeer then dials and checks
 * (Noise, the node ID). UDP over IPv4, as that table is. */
#ifndef PZPEER_BT_H
#define PZPEER_BT_H

#include "net.h"
#include <stdatomic.h>

#define BT_PEERS_MAX 64            /* addresses taken from one round */

typedef struct {
    uint8_t infohash[20];          /* the key of the network in that table */
    uint16_t port;                 /* the port this node listens on, to announce; 0: only look */
    const char *const *routers;    /* "host:port" of nodes of that table to start from */
    int nrouters;
    int budget_ms;                 /* of the whole round; 0: 20000 */
    int wave_ms;                   /* the wait for the answers of one wave of questions; 0: 1500 */
    const atomic_bool *stop;       /* when it turns true the round ends within a moment (NULL: never) */
} bt_config;

/* One round: a lookup of the key, the addresses found under it into out (at most max, BT_PEERS_MAX at most), and the announce of the own
 * port to the nodes closest to the key. The count of addresses, or -1 when no node of that table answered. *announced (when not NULL):
 * how many nodes took the announce. */
int bt_rendezvous(const bt_config *cfg, net_addr *out, int max, int *announced);

/* The routers to start from: those given, or else, on a network of the main chain, the well-known ones of that table; none with off or
 * on a test network with none given. */
const char *const *bt_routers(bool main_chain, bool off, const char *const *given, int ngiven, int *n);

/* Bencoding, for bt.c and the tests: the value of key in the dictionary at d (false when d is not a dictionary or has no such key), and
 * the bytes of a string value. Never read past n, whatever the input. */
bool bt_dict_get(const uint8_t *d, size_t n, const char *key, const uint8_t **v, size_t *vn);
bool bt_string(const uint8_t *v, size_t n, const uint8_t **s, size_t *sn);

#endif
