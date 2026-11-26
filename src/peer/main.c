/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* main.c - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md). */
#include "bt.h"
#include "btc.h"
#include "chain_spv.h"
#include "dbg.h"
#include "sys.h"
#include "dht.h"
#include "dpfeed.h"
#include "home.h"
#include "netfile.h"
#include "sweep.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <pzcore/seed.h>
#include <signal.h>
#include <ctype.h>
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

/* the usage, on stdout when it was asked for (status 0), else on stderr (2) */
static int usage_to(FILE *f)
{
    fprintf(f, "usage: pzpeer run [--puzzle N | --network FILE] [--payout ADDRESS] [options] [-- PZSOLVE OPTIONS]\n"
                    "         the home: pzsolve on the network, its points published, the records of others kept, and for every\n"
                    "         coincidence with another home the prize paid: 45%% to each payout address, 10%% to the author of pzpeer.\n"
                    "         The network of every puzzle with a public key is in the program; without either option, that of the\n"
                    "         first one still open. A puzzle without a public key (--puzzle 71) has a linear search instead: the\n"
                    "         home scans blocks nobody claimed and publishes its claims; a key found stays with it, nothing is\n"
                    "         paid. A network file is for tests and puzzles of one's own\n"
                    "       pzpeer node [--puzzle N | --network FILE] [options]\n"
                    "         a node without pzsolve: it keeps the records of others, alerts their owners and relays sessions; its\n"
                    "         identity is DIR/node.seed, made at the first start\n"
                    "       pzpeer sweep --to ADDRESS [options]  the transaction that moves the coins of a found key, signed and NOT sent,\n"
                    "                                            to give to a mining pool in private (the key on the standard input);\n"
                    "                                            with --private it goes to MARA's Slipstream (--pool URL: another pool)\n"
                    "       pzpeer netcheck NETWORK [--chain]    the values and the fingerprint of a network (a file, or a puzzle number);\n"
                    "                                            --chain: its coins as a home without a node sees them, from the Electrum\n"
                    "                                            servers of the program, each one proven (the headers kept in " SWEEP_DIR ")\n"
                    "       pzpeer dpcheck NETWORK DP_BIN        recompute every point of a campaign's dp.bin and check its tag\n"
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
                    "  --store-gb N         the records kept for others, in GB of disk (default 5; their index takes 30 to 60 %% of it in RAM)\n"
                    "options of run:\n"
                    "  --payout ADDRESS          where the share of this home goes (any address: 1..., 3..., bc1q..., bc1p...); kept in the\n"
                    "                            folder, and asked on the terminal at the first start when it is not given\n"
                    "  --accept-terms            accept the terms of the payout without the question of the first start (for a service)\n"
                    "  --rpc H:P:USER:PASSWORD   a bitcoind of one's own (or, better on a shared machine, --rpc H:P --rpc-cookie FILE, the\n"
                    "                            .cookie of its data directory)\n"
                    "  --electrum HOST:PORT      without --rpc the chain comes from Electrum servers (plain TCP): those of the network file,\n"
                    "                            these, or a built-in list on the main chain. None is trusted: every answer is proven\n"
                    "  --pzsolve PATH, --pzsign PATH   the two programs (default: beside pzpeer, or in PATH)\n"
                    "  --id-bits N, --exit-when-done, --solo-wait SECONDS   for tests\n");
    return f == stdout ? 0 : 2;
}

static int usage(void) { return usage_to(stderr); }

/* a wrong command line: what is wrong, in one line, status 2 */
static int bad(const char *cmd, const char *fmt, const char *arg)
{
    fprintf(stderr, "pzpeer %s: ", cmd);
    fprintf(stderr, fmt, arg);
    fprintf(stderr, " (see 'pzpeer --help')\n");
    return 2;
}

/* a puzzle number (the network written in the program), or 0 for the path of a network file */
static int puzzle_of(const char *s)
{
    int n = 0;
    for (; *s >= '0' && *s <= '9' && n <= 1000; s++) n = n * 10 + (*s - '0');
    return *s || n < 1 || n > 1000 ? 0 : n;
}

/* The network of a file, or of a puzzle when path is NULL, for the checks of records, or of claims (bn) on a linear one, which has no
 * records: its nodes take none (dht.c, answer) */
static bool load_net(const secp256k1_context *ctx, const char *path, int puzzle, netfile *nf, record_net *net, block_net *bn)
{
    char err[300];
    uint8_t base[32];
    if (!netfile_get(path, puzzle, nf, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return false;
    }
    if (nf->linear) {
        memset(net, 0, sizeof *net), memcpy(net->fp, nf->fp, 32), net->dpbits = RECORD_DP_MAX;
        netfile_blocks(nf, bn);
        return true;
    }
    pz_u256_to_be(&nf->start, base);
    if (!record_net_init(ctx, net, nf->fp, nf->dpbits, nf->pubkey33, base)) {
        fprintf(stderr, "the start of the range is the key of the puzzle (network %s)\n", nf->net);
        return false;
    }
    return true;
}

static int cmd_dpcheck(const secp256k1_context *ctx, const char *netpath, const char *dppath)
{
    netfile nf;
    record_net net;
    block_net bn;
    dpfeed df;
    if (!load_net(ctx, puzzle_of(netpath) ? NULL : netpath, puzzle_of(netpath), &nf, &net, &bn)) return 1;
    if (nf.linear) return fprintf(stderr, "network %s: a linear search has no dp.bin\n", nf.net), 1;
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

/* --chain: the coins of the address of a puzzle as a home without a node sees them, from the Electrum servers of the program, each one
 * proven: the named coins one by one, then the scan of the address. The block headers are kept in SWEEP_DIR, as pzpeer sweep keeps them
 * (the first time some 640 000: minutes). The check of a machine that will run without a node of its own. */
static int netcheck_chain(const netfile *nf)
{
    const char *list[CHAIN_SPV_SERVERS];
    uint8_t cp[32], h160[20], spk[BTC_SPK_MAX], txid[32];
    uint64_t cph, tip;
    char why[300], line[NETFILE_LINE_MAX], hex[65];
    payout_coin coins[64];
    chain_spv spv;
    if (strcmp(nf->chain, "main")) return fprintf(stderr, "--chain: for a network of the main chain\n"), 1;
    if (nf->linear && btc_address_script(BTC_MAIN, nf->address, spk) != BTC_P2PKH_LEN) return fprintf(stderr, "--chain: not a P2PKH address\n"), 1;
    if (!nf->linear) pz_hash160(nf->pubkey33, 33, h160), btc_script_p2pkh(h160, spk);
    const int n = chain_spv_defaults(list, &cph, cp);
    if (!home_own_dir(SWEEP_DIR) || !chain_spv_init(&spv, BTC_MAIN, cph, cp, list, n, SWEEP_DIR "/headers.bin")) return 1;
    spv.max_tip_age = 6 * 3600, spv.whole = 1;                  /* every coin listed must be proven */
    if (!spv_tip(&spv.headers, &tip))
        printf("the block headers since block %llu are fetched from the servers into " SWEEP_DIR "/headers.bin (the first time: minutes)\n",
               (unsigned long long)cph);
    fflush(stdout);
    int ok = chain_spv_ready(&spv, why, sizeof why) == 1;
    printf("chain: %s\n", why);
    const session_chain ch = chain_spv_iface(&spv);
    for (int i = 0; ok && netfile_opt(nf, "prize", i, line, sizeof line); i++) {
        unsigned vout;
        if (sscanf(line, "%64[0-9a-fA-F]:%u", hex, &vout) != 2 || !btc_hash_from_hex(hex, txid)) continue;
        const int k = ch.confirmations(ch.ctx, txid, vout);
        if (k > 0) printf("prize %s: unspent, %d confirmations, proven\n", line, k);
        else printf("prize %s: %s\n", line, k == 0 ? "unspent, in no block proven yet" : k == -1 ? "spent, proven" : "no server gives it");
        ok = k != -2;
    }
    const int m = ok ? ch.coins(ch.ctx, spk, coins, (int)(sizeof coins / sizeof *coins)) : -1;
    uint64_t total = 0;
    for (int i = 0; i < m; i++) total += coins[i].value;
    if (ok && m >= 0) printf("the scan of the address: %d coins, %llu satoshis, each proven in a block\n", m, (unsigned long long)total);
    else if (ok) printf("the scan of the address: %s\n", spv.unproven[0] ? spv.unproven : "no server answered");
    chain_spv_free(&spv);
    return ok && m >= 0 ? 0 : 1;
}

static int cmd_netcheck(const char *path, bool chain)
{
    netfile nf;
    char err[300];
    if (!netfile_get(puzzle_of(path) ? NULL : path, puzzle_of(path), &nf, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    char a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF];
    if (nf.linear)
        printf("network %s: puzzle %d, a linear search of address %s, range %s:%s, blocks of 2^%u keys, chain %s\nfingerprint ", nf.net, nf.puzzle,
               nf.address, pz_u256_to_hex(&nf.start, a), pz_u256_to_hex(&nf.end, b), nf.blockbits, nf.chain);
    else
        printf("network %s: puzzle %d, range %s:%s, schema %s, dpbits %u, chain %s\nfingerprint ", nf.net, nf.puzzle, pz_u256_to_hex(&nf.start, a),
               pz_u256_to_hex(&nf.end, b), nf.schema, nf.dpbits, nf.chain);
    for (int i = 0; i < 32; i++) printf("%02x", nf.fp[i]);
    printf("\n");
    char line[NETFILE_LINE_MAX];
    for (int i = 0; !chain && netfile_opt(&nf, "prize", i, line, sizeof line); i++) printf("prize %s\n", line);   /* txid:vout:satoshis */
    return chain ? netcheck_chain(&nf) : 0;
}

/* the time at the start of a line, as the lines of a home have it */
static const char *stamp(char out[16])
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, 16, "[%02d:%02d:%02d]", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return out;
}

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void print_alert(void *user, const record *mine, const record *theirs, bool merge)
{
    char x[65], o[65], ts[16];
    pz_hex_encode(mine->x, 32, x);
    pz_hex_encode(theirs->owner, 32, o);
    printf("%s alert (%s): x %s, the other owner %s\n", stamp(ts), merge ? "merge" : "useful", x, o);
    fflush(stdout);
}

/* The folder of a home or a node, before anything is written in it: for this user alone (a home keeps there its seed, the distances of
 * every point and the output of pzsolve, which shows a key it finds), and for this process alone (two pzpeer on one folder would write
 * over each other's records, journals and sessions) */
/* A node for manual tests of the network: no points of its own (no dp.bin, no sessions), so it is a custodian and a mailbox only. */
static int cmd_node(const secp256k1_context *ctx, int argc, char **argv)
{
    const char *network = NULL, *dir = NULL, *bind = NULL, *boot[16 + 32];
    int puzzle = 0;
    char entries[16][NETFILE_LINE_MAX], dirname[48], seedpath[1100], err[300];
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
        if (!strcmp(a, "--help")) return usage_to(stdout);
        if (!v) return bad("node", strncmp(a, "--", 2) ? "unexpected argument '%s'" : "unrecognized option '%s', or it needs a value", a);
        if (!strcmp(a, "--network")) network = v;
        else if (!strcmp(a, "--puzzle") && !(puzzle = puzzle_of(v))) return bad("node", "--puzzle: invalid value '%s' (expected a puzzle number)", v);
        else if (!strcmp(a, "--puzzle")) {}
        else if (!strcmp(a, "--dir")) dir = v;
        else if (!strcmp(a, "--bind")) bind = v;
        else if (!strcmp(a, "--peer") && npeer < 32) peers[npeer++] = v;
        else if (!strcmp(a, "--bt-router") && nrouters < 8) routers[nrouters++] = v;
        else if (!strcmp(a, "--debug")) debug = v;
        else if (!strcmp(a, "--listen")) {
            port = strtol(v, &end, 10);
            if (*end || port < 0 || port > 65535) return bad("node", "--listen: invalid value '%s' (expected a port from 0 to 65535)", v);
        } else if (!strcmp(a, "--store-gb")) {
            store_gb = strtol(v, &end, 10);
            if (*end || store_gb < 1 || store_gb > 1000000) return bad("node", "--store-gb: invalid value '%s' (expected 1 to 1000000)", v);
        } else return bad("node", "unrecognized option '%s'", a);
        i++;
    }
    if (debug && !dbg_open(debug)) return fprintf(stderr, "--debug: cannot write %s\n", debug), 1;
    netfile nf;
    record_net net;
    block_net bn;
    uint8_t seed[32];
    home_identity id;
    if (!load_net(ctx, network, puzzle, &nf, &net, &bn)) return 1;
    if (!dir) snprintf(dirname, sizeof dirname, "pznode-%.31s", nf.net), dir = dirname;
    if (!home_own_dir(dir)) return 1;
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
    while (nb < 16 && netfile_opt(&nf, "entry", nb, entries[nb], sizeof entries[nb])) boot[nb] = entries[nb], nb++;   /* then --peer */
    for (int i = 0; i < npeer; i++) boot[nb++] = peers[i];
    dht_config cfg = {.ctx = ctx, .net = &net, .self = &id, .id_bits = NODE_ID_ZERO_BITS, .listen_host = bind, .listen_port = (int)port,
                      .dir = dir, .bootstrap = boot, .nbootstrap = nb, .on_alert = print_alert, .verbose = verbose, .portmap = portmap,
                      .allow_private = allow_private || strcmp(nf.chain, "main") != 0,
                      .store_max = (uint64_t)store_gb * 1000000000ull / STORE_ENTRY, .blocks = nf.linear ? &bn : NULL};
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
    pz_hex_encode(id.xonly, 32, idhex);
    char ts[16], how[40] = "not listening";
    if (port) snprintf(how, sizeof how, "listening on port %u", dht_port(d));
    if (n >= 0) printf("%s node %s, %s, %d contact%s\n", stamp(ts), idhex, how, n, n == 1 ? "" : "s");   /* else dht_start said why */
    fflush(stdout);
    for (int tick = 1; n >= 0 && !interrupted; tick++) {
        sys_sleep_ms(200);
        if (tick % 300 == 0) {                                 /* every minute */
            const int c = dht_count(d);
            printf("%s %d contact%s, %llu %s held\n", stamp(ts), c, c == 1 ? "" : "s",
                   (unsigned long long)(nf.linear ? dht_claims_held(d) : dht_local_count(d)), nf.linear ? "claims" : "records");
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
    const char *peers[32], *servers[8], *routers[8], *debug = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--")) {                                 /* the rest goes to pzsolve hunt, but not what pzpeer gives it itself */
            static const char *const OWN[] = {"--puzzle", "--range", "--address", "--pubkey", "--campaign", "--dir", "--seed", "--mode", "--step",
                                              "--part", "--dp-bits", "--jump-seed", "--start-key", "--ui"};   /* --part: half a block would be claimed */
            for (int j = i + 1; j < argc; j++) {
                const char *o = argv[j];
                const size_t l = strcspn(o, "=");                 /* pzsolve takes a long option by any unique start of its name */
                bool own = false;
                if (o[0] == '-' && o[1] != '-')                    /* short ones, also grouped (-qk) or with their value (-k02aa) */
                    for (const char *c = o + 1; *c && isalpha((unsigned char)*c) && !own; c++) own = strchr("prakcdsm", *c) != NULL;
                for (size_t k = 0; !own && o[0] == '-' && o[1] == '-' && l > 2 && k < sizeof OWN / sizeof *OWN; k++) own = !strncmp(o, OWN[k], l);
                if (own) return bad("run", "%s after --: pzpeer gives pzsolve the range, the part, the key, the walk and the folder itself", o);
            }
            cfg.pzsolve_args = (const char *const *)argv + i + 1, cfg.nargs = argc - i - 1;
            break;
        }
        if (!strcmp(a, "--verbose")) { cfg.verbose = true; continue; }
        if (!strcmp(a, "--exit-when-done")) { cfg.exit_when_done = true; continue; }
        if (!strcmp(a, "--allow-private")) { cfg.allow_private = true; continue; }
        if (!strcmp(a, "--no-bt")) { cfg.no_bt = true; continue; }
        if (!strcmp(a, "--no-portmap")) { cfg.portmap = false; continue; }
        if (!strcmp(a, "--accept-terms")) { cfg.accept_terms = true; continue; }
        if (!strcmp(a, "--help")) return usage_to(stdout);
        if (!v) return bad("run", strncmp(a, "--", 2) ? "unexpected argument '%s'" : "unrecognized option '%s', or it needs a value", a);
        if (!strcmp(a, "--network")) cfg.network = v;
        else if (!strcmp(a, "--puzzle") && !(cfg.puzzle = puzzle_of(v))) return bad("run", "--puzzle: invalid value '%s' (expected a puzzle number)", v);
        else if (!strcmp(a, "--puzzle")) {}
        else if (!strcmp(a, "--dir")) cfg.dir = v;
        else if (!strcmp(a, "--payout")) cfg.payout = v;
        else if (!strcmp(a, "--rpc")) {                        /* kept here, and blanked where the list of processes shows it */
            static char rpc[512];
            snprintf(rpc, sizeof rpc, "%s", v), cfg.rpc = rpc;
            memset(argv[i + 1], 'x', strlen(v));
        }
        else if (!strcmp(a, "--rpc-cookie")) cfg.rpc_cookie = v;
        else if (!strcmp(a, "--bind")) cfg.listen_host = v;
        else if (!strcmp(a, "--pzsolve")) cfg.pzsolve = v;
        else if (!strcmp(a, "--pzsign")) cfg.pzsign = v;
        else if (!strcmp(a, "--peer") && cfg.npeers < 32) peers[cfg.npeers++] = v;
        else if (!strcmp(a, "--electrum") && cfg.nelectrum < 8) servers[cfg.nelectrum++] = v;
        else if (!strcmp(a, "--bt-router") && cfg.nbt_routers < 8) routers[cfg.nbt_routers++] = v;
        else if (!strcmp(a, "--debug")) debug = v;
        else if (!strcmp(a, "--listen") || !strcmp(a, "--id-bits") || !strcmp(a, "--store-gb") || !strcmp(a, "--solo-wait")) {
            char *end;
            const long n = strtol(v, &end, 10);
            if (*end || n < 0 || n > (!strcmp(a, "--listen") ? 65535 : !strcmp(a, "--id-bits") ? 32 : 1000000))
                return fprintf(stderr, "pzpeer run: %s: invalid value '%s' (expected %s)\n", a, v,
                               !strcmp(a, "--listen") ? "a port from 0 to 65535" : !strcmp(a, "--id-bits") ? "0 to 32" : "1 to 1000000"), 2;
            if (!strcmp(a, "--listen")) cfg.listen_port = (int)n;
            else if (!strcmp(a, "--id-bits")) cfg.id_bits = (unsigned)n;
            else if (!strcmp(a, "--solo-wait")) cfg.solo_wait = (unsigned)n;
            else cfg.store_gb = (unsigned)n;
        } else return bad("run", "unrecognized option '%s'", a);
        i++;
    }
    cfg.peers = peers, cfg.electrum = servers, cfg.bt_routers = routers;
    if (debug && !dbg_open(debug)) return fprintf(stderr, "--debug: cannot write %s\n", debug), 1;
    netfile nf;
    char err[300], dirname[48];
    if (!netfile_get(cfg.network, cfg.puzzle, &nf, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!cfg.dir) snprintf(dirname, sizeof dirname, "pznet-%.31s", nf.net), cfg.dir = dirname;
    return home_own_dir(cfg.dir) ? home_run(&cfg) : 1;
}

int main(int argc, char **argv)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);                                   /* a terminal or a pipe that goes away is no reason to die, leaving pzsolve */
#endif
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))) return usage_to(stdout);
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        printf("pzpeer %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    (void)pz_seed_harden();                                     /* the home seed, the pieces, the keys of the tiny coins: no core file */
    sys_more_files();
    const secp256k1_context *ctx = keys_ctx();
    const int rc = argc == 4 && !strcmp(argv[1], "dpcheck")  ? cmd_dpcheck(ctx, argv[2], argv[3])
                 : (argc == 3 || (argc == 4 && !strcmp(argv[3], "--chain"))) && !strcmp(argv[1], "netcheck") ? cmd_netcheck(argv[2], argc == 4)
                 : argc >= 2 && !strcmp(argv[1], "node")     ? cmd_node(ctx, argc, argv)
                 : argc >= 2 && !strcmp(argv[1], "run")      ? cmd_run(argc, argv)
                 : argc >= 2 && !strcmp(argv[1], "sweep")    ? sweep_main(argc, argv)
                                                             : usage();
    return rc;
}
