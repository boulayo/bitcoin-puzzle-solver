/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* home.h - the daemon of a home (pzpeer run, docs/en/NETWORK.md 7): the home seed and the keys, pzsolve on the network with the start key,
 * the points of its dp.bin published, a node of the table that keeps the records of others, and a session for every useful coincidence,
 * paid through a bitcoind of its own or, without one, through Electrum servers whose answers are checked (chain_spv.h). */
#ifndef PZPEER_HOME_H
#define PZPEER_HOME_H

#include <stdbool.h>

typedef struct {
    const char *network;            /* the network file */
    const char *dir;                /* the folder of the home: home.seed, start.key, the campaign, the node's files; NULL: pznet-<net> */
    const char *pzsolve, *pzsign;   /* the programs: a path, or a name looked up in PATH */
    const char *payout;             /* the payout address of this home */
    const char *rpc;                /* bitcoind: host:port:user:password, or host:port with rpc_cookie (btc_rpc_init_arg) */
    const char *rpc_cookie;         /* the cookie file of bitcoind, or NULL */
    const char *const *electrum;    /* without rpc: Electrum servers host:port, besides the electrum lines of the network file (chain_spv.h);
                                     * a network of the main chain with none of either uses a built-in list */
    int nelectrum;
    const char *listen_host;        /* NULL: every address */
    int listen_port;                /* 0: do not listen (dial out only) */
    unsigned store_gb;              /* the records kept for others, in GB of disk; 0: the default of the node (37) */
    const char *const *peers;       /* host:port, besides the entry lines of the network file */
    int npeers;
    const char *const *bt_routers;  /* nodes of the table of BitTorrent to find the others through (bt.h); none given: the well-known ones */
    int nbt_routers;                /*   on a network of the main chain, none on a test network */
    bool no_bt;                     /* do not use that table */
    bool upnp;                      /* ask the router of the LAN to let the listening port in */
    unsigned id_bits;               /* leading zero bits of node IDs (NODE_ID_ZERO_BITS; fewer on a test network) */
    const char *const *pzsolve_args; /* more options for pzsolve hunt (for example -t 2 --no-gpu) */
    int nargs;
    bool allow_private;             /* loopback and private addresses on a main-chain network too (other chains allow them) */
    bool exit_when_done;            /* tests: stop after the first session that ends, or when pzsolve ends */
    bool verbose;
} home_cfg;

/* Runs until SIGINT or SIGTERM (or, with exit_when_done, the end of the first session or of pzsolve). 0 when it ended well. */
int home_run(const home_cfg *cfg);

#endif
