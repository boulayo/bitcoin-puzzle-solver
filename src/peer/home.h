/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* home.h - the daemon of a home (pzpeer run, docs/en/NETWORK.md 7): the home seed and the keys, pzsolve on the network with the start key,
 * the points of its dp.bin published, a node of the table that keeps the records of others, and a session for every useful coincidence,
 * paid through bitcoind. */
#ifndef PZPEER_HOME_H
#define PZPEER_HOME_H

#include <stdbool.h>

typedef struct {
    const char *network;            /* the network file */
    const char *dir;                /* the folder of the home: home.seed, start.key, the campaign, the node's files */
    const char *pzsolve, *pzfirma;  /* the programs */
    const char *payout;             /* the payout address of this home */
    const char *rpc;                /* bitcoind: host:port:user:password, or host:port with rpc_cookie (btc_rpc_init_arg) */
    const char *rpc_cookie;         /* the cookie file of bitcoind, or NULL */
    const char *listen_host;        /* NULL: every address */
    int listen_port;                /* 0: do not listen (dial out only) */
    const char *const *peers;       /* host:port, besides the entry lines of the network file */
    int npeers;
    unsigned id_bits;               /* leading zero bits of node IDs (NODE_ID_ZERO_BITS; fewer on a test network) */
    const char *const *pzsolve_args; /* more options for pzsolve hunt (for example -t 2 --no-gpu) */
    int nargs;
    bool exit_when_done;            /* tests: stop after the first session that ends, or when pzsolve ends */
    bool verbose;
} home_cfg;

/* Runs until SIGINT or SIGTERM (or, with exit_when_done, the end of the first session or of pzsolve). 0 when it ended well. */
int home_run(const home_cfg *cfg);

#endif
