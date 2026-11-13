/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* main.c - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md). */
#include "bt.h"
#include "btc.h"
#include "dbg.h"
#include "sys.h"
#include "dht.h"
#include "dpfeed.h"
#include "home.h"
#include "netfile.h"
#include "sweep.h"
#include <pzcore/seal.h>
#include <pzcore/seed.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define DIR_SEP '\\'
#else
#define DIR_SEP '/'
#endif
#define DEFAULT_PORT 28140         /* the TCP port a node listens on when --listen says nothing */

static int usage(void)
{
    fprintf(stderr, "usage: pzpeer run --network FILE --payout ADDRESS [options] [-- PZSOLVE OPTIONS]\n"
                    "         the home: pzsolve on the network, its points published, the records of others kept, and for every\n"
                    "         coincidence with another home the prize paid half each to the two payout addresses\n"
                    "       pzpeer node --network FILE [options]\n"
                    "         a node without pzsolve: it keeps the records of others, alerts their owners and relays sessions; its\n"
                    "         identity is DIR/node.seed, made at the first start\n"
                    "       pzpeer sweep --to ADDRESS [options]  the transaction that moves the coins of a found key, signed and NOT sent,\n"
                    "                                            to give to a mining pool in private (the key on the standard input)\n"
                    "       pzpeer netcheck NETWORK_FILE         the values and the fingerprint of a network file\n"
                    "       pzpeer dpcheck NETWORK_FILE DP_BIN   recompute every point of a campaign's dp.bin and check its tag\n"
                    "       pzpeer version\n"
                    "options of run and node:\n"
                    "  --dir DIR            the folder of the home or node (pznet-<name of the network>, pznode-<name> for node)\n"
                    "  --listen PORT        the TCP port other nodes dial (default 28140; 0: do not listen, only dial out)\n"
                    "  --bind ADDR          listen on this address only\n"
                    "  --no-portmap         do not ask the router of this LAN to let the port in (PCP, NAT-PMP, UPnP)\n"
                    "  --peer HOST:PORT     a node to start from, besides those found on their own\n"
                    "  --bt-router H:P      a node of the table of BitTorrent to find the other nodes through, in place of the well-known\n"
                    "                       ones (a network of the main chain uses that table; a test network only with this option)\n"
                    "  --no-bt              do not announce this node in the table of BitTorrent nor look for the others there\n"
                    "  --allow-private      take loopback and private addresses on a main-chain network (other chains take them)\n"
                    "  --verbose            what the node does, on stderr\n"
                    "  --debug FILE         every message, step and answer of the chain, with its time, into FILE (no secret is written)\n"
                    "  --store-gb N         the records kept for others, in GB of disk (default 37; the index takes about a fifth of it in RAM)\n"
                    "options of run:\n"
                    "  --rpc H:P:USER:PASSWORD   a bitcoind of one's own (or --rpc H:P --rpc-cookie FILE, the .cookie of its data directory)\n"
                    "  --electrum HOST:PORT      without --rpc the chain comes from Electrum servers (plain TCP): those of the network file,\n"
                    "                            these, or a built-in list on the main chain. None is trusted: every answer is proven\n"
                    "  --pzsolve PATH, --pzsign PATH   the two programs (default: beside pzpeer, or in PATH)\n"
                    "  --id-bits N, --exit-when-done   for tests\n");
    return 2;
}

/* The network of a file, for the checks of records. */
static bool load_net(const secp256k1_context *ctx, const char *path, netfile *nf, record_net *net)
{
    char err[300];
    uint8_t base[32];
    if (!netfile_load(path, nf, err, sizeof err)) {
        fprintf(stderr, "%s (%s)\n", err, path);
        return false;
    }
    pz_u256_to_be(&nf->start, base);
    if (!record_net_init(ctx, net, nf->fp, nf->dpbits, nf->pubkey33, base)) {
        fprintf(stderr, "the start of the range is the key of the puzzle (%s)\n", path);
        return false;
    }
    return true;
}

static int cmd_dpcheck(const secp256k1_context *ctx, const char *netpath, const char *dppath)
{
    netfile nf;
    record_net net;
    dpfeed df;
    if (!load_net(ctx, netpath, &nf, &net)) return 1;
    if (!dpfeed_open(&df, dppath)) {
        fprintf(stderr, "%s: not a dp.bin of this version\n", dppath);
        return 1;
    }
    own_point p[256];
    uint64_t by_type[3] = {0, 0, 0};
    for (int n; (n = dpfeed_read(ctx, &net, &df, p, 256)) > 0;)
        for (int i = 0; i < n; i++) by_type[p[i].t + 1]++;
    printf("%llu records: %llu tame, %llu wild +, %llu wild -, %llu wrong\n", (unsigned long long)df.next, (unsigned long long)by_type[1],
           (unsigned long long)by_type[2], (unsigned long long)by_type[0], (unsigned long long)df.bad);
    dpfeed_close(&df);
    return df.bad ? 1 : 0;
}

static int cmd_netcheck(const char *path)
{
    netfile nf;
    char err[300];
    if (!netfile_load(path, &nf, err, sizeof err)) return fprintf(stderr, "%s (%s)\n", err, path), 1;
    char a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF];
    printf("network %s: puzzle %d, range %s:%s, schema %s, dpbits %u, chain %s\nfingerprint ", nf.net, nf.puzzle, pz_u256_to_hex(&nf.start, a),
           pz_u256_to_hex(&nf.end, b), nf.schema, nf.dpbits, nf.chain);
    for (int i = 0; i < 32; i++) printf("%02x", nf.fp[i]);
    printf("\n");
    return 0;
}

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void print_alert(void *user, const record *mine, const record *theirs, bool merge)
{
    char x[65], o[65];
    btc_hex(mine->x, 32, x);
    btc_hex(theirs->owner, 32, o);
    printf("alert (%s): x %s, the other owner %s\n", merge ? "merge" : "useful", x, o);
    fflush(stdout);
}

/* A node for manual tests of the network: no points of its own (no dp.bin, no sessions), so it is a custodian and a mailbox only. */
static int cmd_node(const secp256k1_context *ctx, int argc, char **argv)
{
    const char *network = NULL, *dir = NULL, *bind = NULL, *boot[16 + 32];
    char entries[16][256], dirname[48], seedpath[1100], err[300];
    int nb = 0, npeer = 0;
    long port = DEFAULT_PORT, store_gb = 0;
    bool verbose = false, allow_private = false, no_bt = false, portmap = true;
    const char *peers[32], *routers[8], *debug = NULL;
    int nrouters = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "--verbose")) { verbose = true; continue; }
        if (!strcmp(a, "--allow-private")) { allow_private = true; continue; }
        if (!strcmp(a, "--no-bt")) { no_bt = true; continue; }
        if (!strcmp(a, "--no-portmap")) { portmap = false; continue; }
        if (!v) return usage();
        if (!strcmp(a, "--network")) network = v;
        else if (!strcmp(a, "--dir")) dir = v;
        else if (!strcmp(a, "--bind")) bind = v;
        else if (!strcmp(a, "--peer") && npeer < 32) peers[npeer++] = v;
        else if (!strcmp(a, "--bt-router") && nrouters < 8) routers[nrouters++] = v;
        else if (!strcmp(a, "--debug")) debug = v;
        else if (!strcmp(a, "--listen")) {
            port = strtol(v, &end, 10);
            if (*end || port < 0 || port > 65535) return usage();
        } else if (!strcmp(a, "--store-gb")) {
            store_gb = strtol(v, &end, 10);
            if (*end || store_gb < 1 || store_gb > 1000000) return usage();
        } else return usage();
        i++;
    }
    if (!network) return usage();
    if (debug && !dbg_open(debug)) return fprintf(stderr, "--debug: cannot write %s\n", debug), 1;
    netfile nf;
    record_net net;
    uint8_t seed[32];
    home_identity id;
    if (!load_net(ctx, network, &nf, &net)) return 1;
    if (!dir) snprintf(dirname, sizeof dirname, "pznode-%.31s", nf.net), dir = dirname;
    if (!sys_mkdir(dir) || !sys_private(dir)) return fprintf(stderr, "%s must be a folder of this user that nobody else can open\n", dir), 1;
    snprintf(seedpath, sizeof seedpath, "%s/node.seed", dir);          /* its identity, as the keys of a Bitcoin node in its folder */
    const int got = keys_seed_file(seedpath, seed, err, sizeof err);
    if (!got) return fprintf(stderr, "%s\n", err), 1;
    if (got == 2) printf("a new node seed in %s: the identity of this node (no coin depends on it)\n", seedpath);
    const bool have_id = keys_identity(ctx, seed, nf.fp, NODE_ID_ZERO_BITS, &id);
    pz_secure_zero(seed, sizeof seed);
    if (!have_id) {
        fprintf(stderr, "no identity from this seed\n");
        return 1;
    }
    for (const char *p = nf.opts; *p && nb < 16;) {             /* the entry= lines of the network file, then --peer */
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 6 && l - 6 < sizeof entries[0] && !strncmp(p, "entry=", 6)) {
            memcpy(entries[nb], p + 6, l - 6);
            entries[nb][l - 6] = 0;
            boot[nb] = entries[nb];
            nb++;
        }
        p += l + (e != NULL);
    }
    for (int i = 0; i < npeer; i++) boot[nb++] = peers[i];
    dht_config cfg = {.ctx = ctx, .net = &net, .self = &id, .id_bits = NODE_ID_ZERO_BITS, .listen_host = bind, .listen_port = (int)port,
                      .dir = dir, .bootstrap = boot, .nbootstrap = nb, .on_alert = print_alert, .verbose = verbose, .portmap = portmap,
                      .allow_private = allow_private || strcmp(nf.chain, "main") != 0,
                      .store_max = (uint64_t)store_gb * 1000000000ull / RECORD_LEN};
    cfg.bt_routers = bt_routers(!strcmp(nf.chain, "main"), no_bt, routers, nrouters, &cfg.nbt_routers);
    char idhex[65];
    dht *d = dht_create(&cfg, err, sizeof err);
    if (!d) {
        fprintf(stderr, "%s\n", err);
        pz_secure_zero(&id, sizeof id);
        return 1;
    }
    sys_on_stop(on_signal);
    const int n = dht_start(d);
    btc_hex(id.xonly, 32, idhex);
    if (n < 0) fprintf(stderr, "cannot listen on port %ld\n", port);
    else if (port) printf("node %s, listening on port %u, %d contacts\n", idhex, dht_port(d), n);
    else printf("node %s, not listening, %d contacts\n", idhex, n);
    fflush(stdout);
    for (int tick = 1; n >= 0 && !interrupted; tick++) {
        sys_sleep_ms(200);
        if (tick % 300 == 0) {                                 /* every minute */
            printf("%d contacts, %llu records held\n", dht_count(d), (unsigned long long)dht_local_count(d));
            fflush(stdout);
        }
    }
    dht_stop(d);
    pz_secure_zero(&id, sizeof id);
    return n < 0;
}

/* name beside this program when it was started by a path, else name alone, for PATH */
static const char *beside(const char *self, const char *name, char out[4096])
{
    size_t dir = 0;
    for (size_t i = 0; self[i]; i++)
        if (self[i] == '/' || (DIR_SEP != '/' && self[i] == DIR_SEP)) dir = i + 1;
    snprintf(out, 4096, "%.*s%s", (int)dir, self, name);
    return out;
}

static int cmd_run(int argc, char **argv)
{
    char solve[4096], sign[4096];
    home_cfg cfg = {.pzsolve = beside(argv[0], "pzsolve", solve), .pzsign = beside(argv[0], "pzsign", sign), .id_bits = NODE_ID_ZERO_BITS,
                    .listen_port = DEFAULT_PORT, .portmap = true};
    const char *peers[32], *electrum[8], *routers[8], *debug = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--")) {                                 /* the rest goes to pzsolve hunt */
            cfg.pzsolve_args = (const char *const *)argv + i + 1, cfg.nargs = argc - i - 1;
            break;
        }
        if (!strcmp(a, "--verbose")) { cfg.verbose = true; continue; }
        if (!strcmp(a, "--exit-when-done")) { cfg.exit_when_done = true; continue; }
        if (!strcmp(a, "--allow-private")) { cfg.allow_private = true; continue; }
        if (!strcmp(a, "--no-bt")) { cfg.no_bt = true; continue; }
        if (!strcmp(a, "--no-portmap")) { cfg.portmap = false; continue; }
        if (!v) return usage();
        if (!strcmp(a, "--network")) cfg.network = v;
        else if (!strcmp(a, "--dir")) cfg.dir = v;
        else if (!strcmp(a, "--payout")) cfg.payout = v;
        else if (!strcmp(a, "--rpc")) cfg.rpc = v;
        else if (!strcmp(a, "--rpc-cookie")) cfg.rpc_cookie = v;
        else if (!strcmp(a, "--bind")) cfg.listen_host = v;
        else if (!strcmp(a, "--pzsolve")) cfg.pzsolve = v;
        else if (!strcmp(a, "--pzsign")) cfg.pzsign = v;
        else if (!strcmp(a, "--peer") && cfg.npeers < 32) peers[cfg.npeers++] = v;
        else if (!strcmp(a, "--electrum") && cfg.nelectrum < 8) electrum[cfg.nelectrum++] = v;
        else if (!strcmp(a, "--bt-router") && cfg.nbt_routers < 8) routers[cfg.nbt_routers++] = v;
        else if (!strcmp(a, "--debug")) debug = v;
        else if (!strcmp(a, "--listen") || !strcmp(a, "--id-bits") || !strcmp(a, "--store-gb")) {
            char *end;
            const long n = strtol(v, &end, 10);
            if (*end || n < 0 || n > (!strcmp(a, "--listen") ? 65535 : !strcmp(a, "--id-bits") ? 32 : 1000000)) return usage();
            if (!strcmp(a, "--listen")) cfg.listen_port = (int)n;
            else if (!strcmp(a, "--id-bits")) cfg.id_bits = (unsigned)n;
            else cfg.store_gb = (unsigned)n;
        } else return usage();
        i++;
    }
    cfg.peers = peers, cfg.electrum = electrum, cfg.bt_routers = routers;
    if (!cfg.network || !cfg.payout) return usage();
    if (debug && !dbg_open(debug)) return fprintf(stderr, "--debug: cannot write %s\n", debug), 1;
    return home_run(&cfg);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        printf("pzpeer %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    (void)pz_seed_harden();                                     /* the home seed, the pieces, the keys of the tiny coins: no core file */
    const secp256k1_context *ctx = keys_ctx();
    const int rc = argc == 4 && !strcmp(argv[1], "dpcheck")  ? cmd_dpcheck(ctx, argv[2], argv[3])
                 : argc == 3 && !strcmp(argv[1], "netcheck") ? cmd_netcheck(argv[2])
                 : argc >= 2 && !strcmp(argv[1], "node")     ? cmd_node(ctx, argc, argv)
                 : argc >= 2 && !strcmp(argv[1], "run")      ? cmd_run(argc, argv)
                 : argc >= 2 && !strcmp(argv[1], "sweep")    ? sweep_main(argc, argv)
                                                             : usage();
    return rc;
}
