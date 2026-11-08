/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* main.c - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md). */
#include "dht.h"
#include "dpfeed.h"
#include "../netfile.h"             /* the reader of the network file is the one of pzsolve */
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
                    "       pzpeer node --network FILE --seed-hex HEX --dir DIR [--listen PORT] [--bind ADDR] [--peer HOST:PORT]... [--verbose]\n"
                    "                    a node of the network until interrupted: it keeps the records of others and alerts their owners\n"
                    "                    (it publishes nothing of its own yet); without --listen it only dials out\n");
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
    long port = 0;
    bool verbose = false;
    const char *peers[32];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "--verbose")) {
            verbose = true;
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
                            .dir = dir, .bootstrap = boot, .nbootstrap = nb, .on_alert = print_alert, .verbose = verbose};
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

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        printf("pzpeer %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    const int rc = argc == 4 && !strcmp(argv[1], "dpcheck") ? cmd_dpcheck(ctx, argv[2], argv[3])
                 : argc >= 2 && !strcmp(argv[1], "node")  ? cmd_node(ctx, argc, argv)
                                                          : usage();
    secp256k1_context_destroy(ctx);
    return rc;
}
