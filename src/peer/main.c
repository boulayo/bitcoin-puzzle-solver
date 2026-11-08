/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* main.c - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md). */
#include "dpfeed.h"
#include "../netfile.h"             /* the reader of the network file is the one of pzsolve */
#include <stdio.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: pzpeer version\n"
                    "       pzpeer dpcheck NETWORK_FILE DP_BIN   recompute every point of a campaign's dp.bin and check its tag\n");
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

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "version")) {
        printf("pzpeer %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    const int rc = argc == 4 && !strcmp(argv[1], "dpcheck") ? cmd_dpcheck(ctx, argv[2], argv[3]) : usage();
    secp256k1_context_destroy(ctx);
    return rc;
}
