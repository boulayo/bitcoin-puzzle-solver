/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* main.c - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md). */
#include "dht.h"
#include "dpfeed.h"
#include "home.h"
#include "netfile.h"
#include <pzcore/seal.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int usage(void)
{
    fprintf(stderr, "usage: pzpeer version\n"
                    "       pzpeer dpcheck NETWORK_FILE DP_BIN   recompute every point of a campaign's dp.bin and check its tag\n"
                    "       pzpeer netcheck NETWORK_FILE         the values, the fingerprint and the signatures of a network file\n"
                    "       pzpeer netsign KEY_FILE NETWORK_FILE the signature line of a maintainer (KEY_FILE: 64 hex digits)\n"
                    "       pzpeer run --network FILE --dir DIR --payout ADDRESS --rpc HOST:PORT:USER:PASSWORD [--listen PORT] [--bind ADDR]\n"
                    "                  [--peer HOST:PORT]... [--pzsolve PATH] [--pzfirma PATH] [--id-bits N] [--allow-private] [--exit-when-done]\n"
                    "                  [--verbose]\n"
                    "                  [-- PZSOLVE OPTIONS]\n"
                    "                    the home: pzsolve on the network, its points published, sessions paid through bitcoind\n"
                    "                    (or --rpc HOST:PORT --rpc-cookie FILE: the .cookie of the node's data directory, no password)\n"
                    "       pzpeer node --network FILE --seed-hex HEX --dir DIR [--listen PORT] [--bind ADDR] [--peer HOST:PORT]...\n"
                    "                  [--store-gb N] [--allow-private] [--verbose]\n"
                    "                    a node of the network until interrupted: it keeps the records of others and alerts their owners,\n"
                    "                    and relays the sessions of homes that cannot dial each other (it publishes nothing of its own);\n"
                    "                    without --listen it only dials out\n"
                    "  --allow-private   take loopback and private addresses on a main-chain network (a network of another chain takes them)\n"
                    "  --store-gb N      the records this node keeps for others, in GB of disk (default 37; the index takes about 1/8 of it in RAM)\n");
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

static int cmd_dpcheck(secp256k1_context *ctx, const char *netpath, const char *dppath)
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

static int cmd_netcheck(secp256k1_context *ctx, const char *path)
{
    netfile nf;
    trust_set t;
    char err[300];
    if (!netfile_load(path, &nf, err, sizeof err)) return fprintf(stderr, "%s (%s)\n", err, path), 1;
    char a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF];
    printf("network %s: puzzle %d, range %s:%s, schema %s, dpbits %u, chain %s\nfingerprint ", nf.net, nf.puzzle, pz_u256_to_hex(&nf.start, a),
           pz_u256_to_hex(&nf.end, b), nf.schema, nf.dpbits, nf.chain);
    for (int i = 0; i < 32; i++) printf("%02x", nf.fp[i]);
    printf("\n");
    int valid = 0;
    if (!trust_load(&t, err, sizeof err)) return printf("signatures: %d, not checked: %s\n", nf.nsig, err), 1;
    const bool ok = trust_check(ctx, &t, &nf, &valid);
    printf("signatures: %d valid of trusted keys, %d needed: %s\n", valid, t.threshold, ok ? "accepted" : "NOT accepted");
    return ok ? 0 : 1;
}

static int cmd_netsign(secp256k1_context *ctx, const char *keypath, const char *path)
{
    netfile nf;
    char err[300], line[200], hex[80] = "";
    uint8_t sec[32];
    FILE *f = fopen(keypath, "r");
    const bool read = f && fgets(hex, sizeof hex, f);
    if (f) fclose(f);
    hex[strcspn(hex, " \t\r\n")] = 0;
    bool ok = read && strlen(hex) == 64;
    for (int i = 0; ok && i < 32; i++) {
        unsigned v;
        ok = sscanf(hex + 2 * i, "%2x", &v) == 1, sec[i] = (uint8_t)v;
    }
    pz_secure_zero(hex, sizeof hex);
    if (!ok) return fprintf(stderr, "%s: expected a key of 64 hex digits\n", keypath), 1;
    ok = netfile_load(path, &nf, err, sizeof err) && trust_sign(ctx, sec, &nf, line);
    pz_secure_zero(sec, sizeof sec);
    if (!ok) return fprintf(stderr, "%s (%s)\n", err[0] ? err : "invalid key", path), 1;
    printf("%s\n", line);
    return 0;
}

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void hex_of(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = H[b[i] >> 4], out[2 * i + 1] = H[b[i] & 15];
    out[2 * n] = 0;
}

static bool unhex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < 2 * n; i++) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] | v : v << 4);
    }
    return true;
}

static void print_alert(void *user, const record *mine, const record *theirs, bool merge)
{
    char x[65], o[65];
    hex_of(mine->x, 32, x);
    hex_of(theirs->owner, 32, o);
    printf("alert (%s): x %s, the other owner %s\n", merge ? "merge" : "useful", x, o);
    fflush(stdout);
}

/* A node for manual tests of the network: no points of its own (no dp.bin, no sessions), so it is a custodian and a mailbox only. */
static int cmd_node(secp256k1_context *ctx, int argc, char **argv)
{
    const char *network = NULL, *seedhex = NULL, *dir = NULL, *bind = NULL, *boot[16 + 32];
    char entries[16][256];
    int nb = 0, npeer = 0;
    long port = 0, store_gb = 0;
    bool verbose = false, allow_private = false;
    const char *peers[32];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "--verbose") || !strcmp(a, "--allow-private")) {
            if (!strcmp(a, "--verbose")) verbose = true;
            else allow_private = true;
            continue;
        }
        if (!v) return usage();
        if (!strcmp(a, "--network")) network = v;
        else if (!strcmp(a, "--seed-hex")) seedhex = v;
        else if (!strcmp(a, "--dir")) dir = v;
        else if (!strcmp(a, "--bind")) bind = v;
        else if (!strcmp(a, "--peer") && npeer < 32) peers[npeer++] = v;
        else if (!strcmp(a, "--listen")) {
            port = strtol(v, &end, 10);
            if (*end || port < 0 || port > 65535) return usage();
        } else if (!strcmp(a, "--store-gb")) {
            store_gb = strtol(v, &end, 10);
            if (*end || store_gb < 1 || store_gb > 1000000) return usage();
        } else return usage();
        i++;
    }
    if (!network || !seedhex || !dir) return usage();
    netfile nf;
    record_net net;
    uint8_t seed[32];
    home_identity id;
    if (!load_net(ctx, network, &nf, &net)) return 1;
    if (!unhex(seedhex, seed, 32)) {
        fprintf(stderr, "--seed-hex: the home seed, 64 hex digits\n");
        return 2;
    }
    const bool have_id = keys_identity(ctx, seed, nf.fp, NODE_ID_ZERO_BITS, &id);
    pz_secure_zero(seed, sizeof seed);
    if (!have_id) {
        fprintf(stderr, "no identity from this seed\n");
        return 1;
    }
    for (const char *p = nf.upd; *p && nb < 16;) {             /* the entry= lines of the network file, then --peer */
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
    const dht_config cfg = {.ctx = ctx, .net = &net, .self = &id, .id_bits = NODE_ID_ZERO_BITS, .listen_host = bind, .listen_port = (int)port,
                            .dir = dir, .bootstrap = boot, .nbootstrap = nb, .on_alert = print_alert, .verbose = verbose,
                            .allow_private = allow_private || strcmp(nf.chain, "main") != 0,
                            .store_max = (uint64_t)store_gb * 1000000000ull / RECORD_LEN};
    char err[300], idhex[65];
    dht *d = dht_create(&cfg, err, sizeof err);
    if (!d) {
        fprintf(stderr, "%s\n", err);
        pz_secure_zero(&id, sizeof id);
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    const int n = dht_start(d);
    hex_of(id.xonly, 32, idhex);
    if (n < 0) fprintf(stderr, "cannot listen on port %ld\n", port);
    else if (port) printf("node %s, listening on port %u, %d contacts\n", idhex, dht_port(d), n);
    else printf("node %s, not listening, %d contacts\n", idhex, n);
    fflush(stdout);
    for (int tick = 1; n >= 0 && !interrupted; tick++) {
        const struct timespec ts = {0, 200 * 1000000};
        nanosleep(&ts, NULL);
        if (tick % 300 == 0) {                                 /* every minute */
            dht_contact cs[64];
            printf("%d contacts, %llu records held\n", dht_contacts(d, cs, 64), (unsigned long long)dht_local_count(d));
            fflush(stdout);
        }
    }
    dht_stop(d);
    pz_secure_zero(&id, sizeof id);
    return n < 0;
}

static int cmd_run(int argc, char **argv)
{
    home_cfg cfg = {.pzsolve = "pzsolve", .pzfirma = "pzfirma", .id_bits = NODE_ID_ZERO_BITS};
    const char *peers[32];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--")) {                                 /* the rest goes to pzsolve hunt */
            cfg.pzsolve_args = (const char *const *)argv + i + 1, cfg.nargs = argc - i - 1;
            break;
        }
        if (!strcmp(a, "--verbose")) { cfg.verbose = true; continue; }
        if (!strcmp(a, "--exit-when-done")) { cfg.exit_when_done = true; continue; }
        if (!strcmp(a, "--allow-private")) { cfg.allow_private = true; continue; }
        if (!v) return usage();
        if (!strcmp(a, "--network")) cfg.network = v;
        else if (!strcmp(a, "--dir")) cfg.dir = v;
        else if (!strcmp(a, "--payout")) cfg.payout = v;
        else if (!strcmp(a, "--rpc")) cfg.rpc = v;
        else if (!strcmp(a, "--rpc-cookie")) cfg.rpc_cookie = v;
        else if (!strcmp(a, "--bind")) cfg.listen_host = v;
        else if (!strcmp(a, "--pzsolve")) cfg.pzsolve = v;
        else if (!strcmp(a, "--pzfirma")) cfg.pzfirma = v;
        else if (!strcmp(a, "--peer") && cfg.npeers < 32) peers[cfg.npeers++] = v;
        else if (!strcmp(a, "--listen") || !strcmp(a, "--id-bits")) {
            char *end;
            const long n = strtol(v, &end, 10);
            if (*end || n < 0 || n > (!strcmp(a, "--listen") ? 65535 : 32)) return usage();
            if (!strcmp(a, "--listen")) cfg.listen_port = (int)n;
            else cfg.id_bits = (unsigned)n;
        } else return usage();
        i++;
    }
    cfg.peers = peers;
    if (!cfg.network || !cfg.dir || !cfg.payout || !cfg.rpc) return usage();
    return home_run(&cfg);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        printf("pzpeer %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    const int rc = argc == 4 && !strcmp(argv[1], "dpcheck")  ? cmd_dpcheck(ctx, argv[2], argv[3])
                 : argc == 3 && !strcmp(argv[1], "netcheck") ? cmd_netcheck(ctx, argv[2])
                 : argc == 4 && !strcmp(argv[1], "netsign")  ? cmd_netsign(ctx, argv[2], argv[3])
                 : argc >= 2 && !strcmp(argv[1], "node")     ? cmd_node(ctx, argc, argv)
                 : argc >= 2 && !strcmp(argv[1], "run")      ? cmd_run(argc, argv)
                                                             : usage();
    secp256k1_context_destroy(ctx);
    return rc;
}
