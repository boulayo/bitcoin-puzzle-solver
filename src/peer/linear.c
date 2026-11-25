/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* linear.c - see linear.h. The folder of the home: home.seed (its identity), block (the block under way: a restart resumes it),
 * own_claims.bin (the blocks it scanned, with their claims: claims.h), campaigns/b<block> (pzsolve on that block), node/ (the table and
 * the claims it keeps for others). */
#include "linear.h"
#include "block.h"
#include "bt.h"
#include "claims.h"
#include "dbg.h"
#include "dht.h"
#include "keys.h"
#include "sys.h"
#include <pzcore/address.h>
#include <pzcore/seal.h>
#include <pzcore/u256.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PICK_TRIES 64              /* blocks looked at for one nobody claimed; then a claimed one is scanned again */
#define STATUS_S 600
#define REPUBLISH_FIRST_S 120      /* the claims of this home go to their custodians again two minutes after a start, */
#define REPUBLISH_S 86400          /* and then every day: one a turn of the loop */

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig, g_stop = 1; }

typedef struct {
    const home_cfg *cfg;
    const netfile *nf;
    block_net bn;
    const char *dir;
    const home_identity *id;
    dht *d;
    uint64_t nblocks;
    char addr[64];                 /* the target as pzsolve takes it: the mainnet form of its hash160 */
    claims *own;                   /* the blocks this home scanned, with their claims (own_claims.bin) */
    uint64_t repub;                /* the next of them to send again */
    unsigned repub_sent, repub_held;   /* in this round: those sent again, those the network holds */
} lin;

static int64_t mono_s(void) { return sys_now_ms() / 1000; }

static bool scanned(const lin *l, uint64_t i)
{
    block_claim c;
    return claims_get(l->own, i, &c, 1) > 0;
}

/* The next block: one nobody claimed, picked at random (with many homes two seldom pick the same). When PICK_TRIES of them are all
 * claimed, the second pass: of those, the one whose claims show the fewest proof keys, so that a false claim, which shows few, delays
 * the key and does not hide it. Never one this home scanned, unless it scanned every block. */
static uint64_t pick(lin *l, bool *again)
{
    uint64_t best = 0;
    int fewest = BLOCK_PROOFS_MAX * CLAIMS_PER_BLOCK + 1;
    const bool all = claims_count(l->own) >= l->nblocks;
    *again = true;
    for (int t = 0; t < PICK_TRIES && !g_stop; t++) {
        uint8_t r[8];
        uint64_t i = 0;
        block_claim c[CLAIMS_PER_BLOCK];
        if (!sys_random(r, sizeof r)) break;
        for (int k = 0; k < 8; k++) i = i << 8 | r[k];
        i %= l->nblocks;
        if (scanned(l, i) && !all) continue;
        const int n = dht_claims_of(l->d, i, c, CLAIMS_PER_BLOCK);
        if (n <= 0) return *again = false, i;                  /* none, or no custodian to ask (alone): scanned here */
        const int keys = block_claims_keys(c, n);
        if (keys < fewest) best = i, fewest = keys;
    }
    return best;
}

/* The next of this home's claims to its custodians again; at the end of a round, how many the network holds */
static void republish(lin *l, int64_t *next)
{
    block_claim c;
    const uint64_t n = claims_count(l->own);
    for (; l->repub < n && claims_at(l->own, l->repub, &c); l->repub++)
        if (c.n >= block_min_proofs(l->bn.bits)) {            /* a block with too few proof keys is kept, not claimed */
            l->repub_sent++, l->repub_held += dht_claim_publish(l->d, &c) > 0, l->repub++;
            return;
        }
    if (l->repub_sent) home_say("the claims of this home sent again: %u of %u held by the network", l->repub_held, l->repub_sent);
    l->repub = l->repub_sent = l->repub_held = 0, *next = mono_s() + REPUBLISH_S;
}

static bool state_read(const lin *l, uint64_t *i)
{
    char path[1200];
    unsigned long long v;
    snprintf(path, sizeof path, "%s/block", l->dir);
    FILE *f = sys_fopen(path, "r");
    const bool ok = f && fscanf(f, "%llu", &v) == 1 && v < l->nblocks;
    if (f) fclose(f);
    if (ok) *i = v;
    return ok;
}

static void state_write(const lin *l, const uint64_t *i)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/block", l->dir);
    if (!i) {
        remove(path);
        return;
    }
    FILE *f = sys_new_file(path, false);
    if (!sys_new_file_done(f, path, f && fprintf(f, "%llu\n", (unsigned long long)*i) > 0)) home_say("cannot write %s: a restart picks another block", path);
}

/* pzsolve on block i: the near misses go to near.txt of its campaign, the end of its input stops it */
static sys_proc start(const lin *l, uint64_t i, int *in)
{
    char range[2 * PZ_U256_HEX_BUF + 2], a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF], name[32], camp[1100], log[1100];
    pz_u256 first, last;
    if (!block_range(&l->bn, i, &first, &last)) return 0;
    snprintf(range, sizeof range, "%s:%s", pz_u256_to_hex(&first, a), pz_u256_to_hex(&last, b));
    snprintf(name, sizeof name, "b%llu", (unsigned long long)i);
    snprintf(camp, sizeof camp, "%s/campaigns", l->dir);
    snprintf(log, sizeof log, "%s/pzsolve.out", l->dir);
    const char *argv[64] = {l->cfg->pzsolve, "hunt", "-r", range, "-a", l->addr, "--near", "-c", name, "-d", camp, "--ui", "log", "--stop-with-input"};
    int n = 14;
    for (int k = 0; k < l->cfg->nargs && n < 62; k++) argv[n++] = l->cfg->pzsolve_args[k];
    argv[n] = NULL;
    return sys_spawn(argv, log, in, NULL);
}

/* The end of a block pzsolve scanned whole: its claim from the near misses, kept here (this home does not scan the block again) and sent
 * to the network when it has enough proof keys. */
static void claim(lin *l, uint64_t i, const char *campdir)
{
    char path[1400], line[128];
    snprintf(path, sizeof path, "%s/near.txt", campdir);
    FILE *f = sys_fopen(path, "r");
    uint8_t (*keys)[32] = NULL;
    size_t n = 0, cap = 0;
    while (f && fgets(line, sizeof line, f)) {
        pz_u256 k;
        line[strcspn(line, "\r\n")] = 0;
        if (!pz_u256_from_hex(&k, line)) continue;
        if (n == cap) {
            uint8_t (*g)[32] = realloc(keys, (cap = cap ? 2 * cap : 256) * 32);
            if (!g) break;
            keys = g;
        }
        pz_u256_to_be(&k, keys[n++]);
    }
    if (f) fclose(f);
    block_claim c;
    const int min = block_min_proofs(l->bn.bits);
    const bool made = block_claim_make(keys_ctx(), &l->bn, &l->id->kp, i, (const uint8_t(*)[32])keys, n, &c);
    free(keys);
    if (!made) return;                                         /* never: i is a block of the range */
    if (claims_put(l->own, &c) == CLAIM_FAILED) home_say("cannot write %s/own_claims.bin: a restart may scan block %llu again", l->dir, (unsigned long long)i);
    if (c.n < min)
        home_say("block %llu scanned, with %d proof keys of the %d a claim needs (about one block in a hundred has so few): not claimed, and"
            " others may scan it again", (unsigned long long)i, c.n, min);
    else home_say("block %llu scanned and claimed with %d proof keys: %d custodians hold the claim", (unsigned long long)i, c.n, dht_claim_publish(l->d, &c));
}

static void status(const lin *l, bool have, uint64_t i, int64_t since)
{
    const double nodes = dht_nodes(l->d), held = (double)dht_claims_held(l->d);
    char net[200] = "";
    if (held > 0 && nodes > 0)                                 /* a claim goes to DHT_KSTORE custodians; a block has one claim, seldom more */
        snprintf(net, sizeof net, "; the network holds about %.0f claims for %llu blocks (%.4f%%)", held * nodes / DHT_KSTORE,
                 (unsigned long long)l->nblocks, 100 * held * nodes / DHT_KSTORE / (double)l->nblocks);
    home_say("linear search: %s, %llu blocks scanned by this home, %d contacts%s", have ? "a block under way" : "no block under way",
        (unsigned long long)claims_count(l->own), dht_count(l->d), net);
    if (have) home_say("block %llu for %lld minutes", (unsigned long long)i, (long long)((mono_s() - since) / 60));
}

int linear_run(const home_cfg *cfg, const netfile *nf, const char *dir)
{
    char err[400], path[1200], nodedir[1100];
    uint8_t seed[32];
    home_identity id;
    lin l = {.cfg = cfg, .nf = nf, .dir = dir, .id = &id};
    netfile_blocks(nf, &l.bn);
    l.nblocks = block_count(&l.bn);
    if (!pz_hash160_to_address(nf->h160, l.addr, sizeof l.addr)) return fprintf(stderr, "invalid address\n"), 1;
    snprintf(path, sizeof path, "%s/home.seed", dir);
    const int got = keys_seed_file(path, seed, err, sizeof err);
    if (got <= 0) return fprintf(stderr, "%s\n", err), 1;
    if (got == 2) home_say("a new home seed in %s: keep a copy of it, it is the identity of this home and of its claims", path);
    const bool have_id = keys_identity(keys_ctx(), seed, nf->fp, cfg->id_bits, &id);
    pz_secure_zero(seed, sizeof seed);
    if (!have_id) return fprintf(stderr, "no identity\n"), 1;
    snprintf(path, sizeof path, "%s/own_claims.bin", dir);
    if (!(l.own = claims_open(path, nf->fp, err, sizeof err))) return fprintf(stderr, "%s\n", err), 1;
    record_net rn = {.dpbits = RECORD_DP_MAX};                 /* a linear network has no records: its nodes refuse them (dht.c) */
    memcpy(rn.fp, nf->fp, 32);
    const char *boot[64];
    char entries[16][NETFILE_LINE_MAX];
    int nb = 0;
    while (nb < 16 && netfile_opt(nf, "entry", nb, entries[nb], sizeof entries[nb])) boot[nb] = entries[nb], nb++;
    for (int i = 0; i < cfg->npeers && nb < 64; i++) boot[nb++] = cfg->peers[i];
    snprintf(nodedir, sizeof nodedir, "%s/node", dir);
    const bool main_chain = !strcmp(nf->chain, "main");
    dht_config dc = {.ctx = keys_ctx(), .net = &rn, .self = &id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                     .listen_port = cfg->listen_port, .dir = nodedir, .bootstrap = boot, .nbootstrap = nb, .verbose = cfg->verbose,
                     .portmap = cfg->portmap, .allow_private = !main_chain || cfg->allow_private, .blocks = &l.bn};
    dc.bt_routers = bt_routers(main_chain, cfg->no_bt, cfg->bt_routers, cfg->nbt_routers, &dc.nbt_routers);
    if (!(l.d = dht_create(&dc, err, sizeof err))) return fprintf(stderr, "%s\n", err), claims_close(l.own), 1;
    sys_on_stop(on_signal);
    const int nc = dht_start(l.d);
    if (nc < 0) return fprintf(stderr, "cannot listen\n"), dht_stop(l.d), claims_close(l.own), 1;
    home_say("home on the linear network %s (%s, blocks of 2^%u keys, %llu of them): %d contact%s, %llu blocks scanned by this home before",
        nf->net, nf->address, nf->blockbits, (unsigned long long)l.nblocks, nc, nc == 1 ? "" : "s", (unsigned long long)claims_count(l.own));
    uint64_t i = 0;
    bool have = state_read(&l, &i), again = false, found = false;
    sys_proc ps = 0;
    int in = -1, rc = 0;
    bool search = true;                                        /* false once pzsolve failed: a custodian only */
    int64_t since = mono_s(), next_status = mono_s() + STATUS_S, next_repub = mono_s() + REPUBLISH_FIRST_S;
    while (!g_stop && !found && !rc) {
        if (!ps && search) {
            if (!have) {
                i = pick(&l, &again);
                if (g_stop) break;
                have = true, state_write(&l, &i);
                if (again) home_say("every block looked at is claimed: block %llu, whose claims show the fewest proof keys, is scanned again",
                               (unsigned long long)i);
            }
            if (!(ps = start(&l, i, &in))) {
                home_say("cannot start %s", cfg->pzsolve);
                rc = 1;
                break;
            }
            home_say("block %llu: pzsolve scans it, its output in %s/pzsolve.out", (unsigned long long)i, dir);
            since = mono_s();
        }
        sys_sleep_ms(500);
        int st;
        if (ps && sys_proc_ended(ps, false, &st)) {
            char camp[1300], fpath[1400];
            snprintf(camp, sizeof camp, "%s/campaigns/b%llu", dir, (unsigned long long)i);
            snprintf(fpath, sizeof fpath, "%s/FOUND.txt", camp);
            sys_pipe_close(in), in = -1, ps = 0;
            if (st == 0 && access(fpath, F_OK) == 0) {
                home_say("THE KEY OF THE PUZZLE IS IN %s. Do not spend it with a wallet: its transaction shows the public key, and with it anyone"
                    " finds the key at once and sends the coins elsewhere first. pzpeer sweep --to ADDRESS --private, with the key on its input,"
                    " makes the transaction and gives it to a mining pool only", fpath);
                found = true;
            } else if (st == 1) {
                claim(&l, i, camp);
                (void)sys_remove_dir(camp);
                state_write(&l, NULL), have = false;
                if (cfg->exit_when_done) break;
            } else {
                home_say("pzsolve ended with status %d (all of it in %s/pzsolve.out): this home goes on as a custodian, without a search", st, dir);
                search = false;
            }
        }
        if (mono_s() >= next_status) status(&l, have, i, since), next_status = mono_s() + STATUS_S;
        if (mono_s() >= next_repub) republish(&l, &next_repub);
    }
    if (ps) (void)sys_proc_stop(ps, &in, PZSOLVE_STOP_MS);    /* it saves where it is in the block, and ends */
    home_say("home stopped: %llu blocks scanned by this home%s", (unsigned long long)claims_count(l.own), found ? ", and the key found" : "");
    dht_stop(l.d);
    claims_close(l.own);
    return rc;
}
