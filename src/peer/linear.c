/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* linear.c - see linear.h. The folder of the home: home.seed (its identity), block (the block under way: a restart resumes it), claims.own
 * (its claims, for the republish), campaigns/b<block> (pzsolve on that block), node/ (the table and the claims it keeps for others). */
#include "linear.h"
#include "block.h"
#include "bt.h"
#include "dbg.h"
#include "dht.h"
#include "keys.h"
#include "sys.h"
#include <pzcore/address.h>
#include <pzcore/seal.h>
#include <pzcore/u256.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PICK_TRIES 64              /* blocks looked at for one nobody claimed; then a claimed one is scanned again */
#define STATUS_S 600
#define REPUBLISH_S 86400

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig, g_stop = 1; }

static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    dbg("home %s", line);
    printf("[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fflush(stdout);
}

typedef struct {
    const home_cfg *cfg;
    const netfile *nf;
    block_net bn;
    const char *dir;
    const home_identity *id;
    dht *d;
    uint64_t nblocks;
    char addr[64];                 /* the target as pzsolve takes it: the mainnet form of its hash160 */
    uint64_t *own;                 /* the blocks this home claimed (claims.own) */
    size_t nown;
} lin;

static int64_t mono_s(void) { return sys_now_ms() / 1000; }

/* the first and the last key of block i */
static void block_range(const lin *l, uint64_t i, pz_u256 *first, pz_u256 *last)
{
    pz_u256 off = pz_u256_from_u64(i), one = pz_u256_from_u64(1), size = pz_u256_pow2(l->bn.bits);
    off = pz_u256_shl(&off, l->bn.bits);
    pz_u256_add(first, &l->nf->start, &off);
    pz_u256_add(last, first, &size);
    pz_u256_sub(last, last, &one);
    if (pz_u256_cmp(last, &l->nf->end) > 0) *last = l->nf->end;
}

static bool owned(const lin *l, uint64_t i)
{
    for (size_t k = 0; k < l->nown; k++)
        if (l->own[k] == i) return true;
    return false;
}

/* claims.own: each claim after its length (2 bytes). With republish, every one goes to its custodians again; else they are only counted */
static void own_claims(lin *l, bool republish)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/claims.own", l->dir);
    FILE *f = sys_fopen(path, "rb");
    uint8_t b[2 + BLOCK_CLAIM_MAX];
    size_t sent = 0;
    l->nown = 0;
    while (f && fread(b, 1, 2, f) == 2) {
        const size_t len = (size_t)b[0] << 8 | b[1];
        block_claim c;
        if (len > BLOCK_CLAIM_MAX || fread(b + 2, 1, len, f) != len || !block_claim_decode(b + 2, len, &c)) break;
        uint64_t *g = realloc(l->own, (l->nown + 1) * sizeof *g);
        if (!g) break;
        l->own = g, l->own[l->nown++] = c.index;
        if (republish && !g_stop) sent += dht_claim_publish(l->d, &c) > 0;
    }
    if (f) fclose(f);
    if (republish && l->nown) say("the claims of this home sent again: %zu of %zu held by the network", sent, l->nown);
}

static bool own_add(lin *l, const block_claim *c)
{
    char path[1200];
    uint8_t b[2 + BLOCK_CLAIM_MAX];
    const size_t len = block_claim_encode(c, b + 2);
    b[0] = (uint8_t)(len >> 8), b[1] = (uint8_t)len;
    snprintf(path, sizeof path, "%s/claims.own", l->dir);
    FILE *f = sys_fopen(path, "ab");
    const bool ok = f && fwrite(b, 1, 2 + len, f) == 2 + len && fflush(f) == 0 && sys_sync(fileno(f));
    if (f) fclose(f);
    uint64_t *g = ok ? realloc(l->own, (l->nown + 1) * sizeof *g) : NULL;
    if (g) l->own = g, l->own[l->nown++] = c->index;
    return ok;
}

/* The next block: one nobody claimed, picked at random (with many homes two seldom pick the same); after PICK_TRIES claimed ones, a
 * claimed one all the same: a second pass over the claimed blocks, so that a false claim delays the key and does not hide it. */
static uint64_t pick(lin *l, bool *again)
{
    uint64_t i = 0;
    *again = false;
    for (int t = 0; t < PICK_TRIES && !g_stop; t++) {
        uint8_t r[8];
        uint64_t v = 0;
        if (!sys_random(r, sizeof r)) break;
        for (int k = 0; k < 8; k++) v = v << 8 | r[k];
        i = v % l->nblocks;
        if (owned(l, i) && l->nown < l->nblocks) continue;
        block_claim c;
        if (dht_claims_of(l->d, i, &c, 1) <= 0) return i;      /* none, or no custodian to ask (alone): scanned here */
    }
    *again = true;
    return i;
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
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/block", l->dir);
    if (!i) {
        remove(path);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = sys_fopen(tmp, "w");
    const bool ok = f && fprintf(f, "%llu\n", (unsigned long long)*i) > 0 && fflush(f) == 0 && sys_sync(fileno(f));
    if (f) fclose(f);
    if (!ok || !sys_replace(tmp, path)) say("cannot write %s: a restart picks another block", path);
}

/* pzsolve on block i: the near misses go to near.txt of its campaign, the end of its input stops it */
static sys_proc start(const lin *l, uint64_t i, int *in)
{
    char range[2 * PZ_U256_HEX_BUF + 2], a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF], name[32], camp[1100], log[1100];
    pz_u256 first, last;
    block_range(l, i, &first, &last);
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

/* The end of a block pzsolve scanned whole: its claim from the near misses, to the network and to claims.own. */
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
    if (!block_claim_make(keys_ctx(), &l->bn, &l->id->kp, i, (const uint8_t(*)[32])keys, n, &c))
        say("block %llu scanned, but with fewer proof keys than a claim needs (it happens once in a thousand blocks): not claimed",
            (unsigned long long)i);
    else if (!own_add(l, &c)) say("cannot write %s/claims.own: the claim of block %llu is not kept", l->dir, (unsigned long long)i);
    else say("block %llu scanned and claimed with %d proof keys: %d custodians hold the claim", (unsigned long long)i, c.n, dht_claim_publish(l->d, &c));
    free(keys);
}

static void status(const lin *l, bool have, uint64_t i, int64_t since)
{
    const double nodes = dht_nodes(l->d), held = (double)dht_claims_held(l->d);
    char net[200] = "";
    if (held > 0 && nodes > 0)                                 /* a claim goes to DHT_KSTORE custodians */
        snprintf(net, sizeof net, "; the network holds about %.0f claimed blocks of %llu (%.4f%%)", held * nodes / DHT_KSTORE,
                 (unsigned long long)l->nblocks, 100 * held * nodes / DHT_KSTORE / (double)l->nblocks);
    say("linear search: %s, %zu blocks claimed by this home, %d contacts%s", have ? "a block under way" : "no block under way", l->nown,
        dht_count(l->d), net);
    if (have) say("block %llu for %lld minutes", (unsigned long long)i, (long long)((mono_s() - since) / 60));
}

int linear_run(const home_cfg *cfg, const netfile *nf, const char *dir)
{
    char err[400], path[1200], nodedir[1100];
    uint8_t seed[32];
    home_identity id;
    lin l = {.cfg = cfg, .nf = nf, .dir = dir, .id = &id};
    netfile_blocks(nf, &l.bn);
    pz_u256 span;                                              /* the blocks: the range in pieces of 2^bits keys, the last one maybe short */
    pz_u256_sub(&span, &nf->end, &nf->start);
    span = pz_u256_shr(&span, nf->blockbits);
    l.nblocks = span.w[1] | span.w[2] | span.w[3] || span.w[0] == UINT64_MAX ? UINT64_MAX : span.w[0] + 1;
    if (!pz_hash160_to_address(nf->h160, l.addr, sizeof l.addr)) return fprintf(stderr, "invalid address\n"), 1;
    snprintf(path, sizeof path, "%s/home.seed", dir);
    const int got = keys_seed_file(path, seed, err, sizeof err);
    if (got <= 0) return fprintf(stderr, "%s\n", err), 1;
    if (got == 2) say("a new home seed in %s: keep a copy of it, it is the identity of this home and of its claims", path);
    if (!keys_identity(keys_ctx(), seed, nf->fp, cfg->id_bits, &id)) return fprintf(stderr, "no identity\n"), 1;
    record_net rn = {.dpbits = RECORD_DP_MAX};                 /* no records on a linear network: no point has all the zero bits */
    memcpy(rn.fp, nf->fp, 32);
    const char *boot[64];
    char entries[16][256];
    int nb = 0;
    for (const char *p = nf->opts; *p && nb < 16;) {           /* the entry lines of the file and the peers given */
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 6 && len - 6 < sizeof entries[0] && !strncmp(p, "entry=", 6)) memcpy(entries[nb], p + 6, len - 6), entries[nb][len - 6] = 0, boot[nb] = entries[nb], nb++;
        p += len + (e != NULL);
    }
    for (int i = 0; i < cfg->npeers && nb < 64; i++) boot[nb++] = cfg->peers[i];
    snprintf(nodedir, sizeof nodedir, "%s/node", dir);
    const bool main_chain = !strcmp(nf->chain, "main");
    dht_config dc = {.ctx = keys_ctx(), .net = &rn, .self = &id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                     .listen_port = cfg->listen_port, .dir = nodedir, .bootstrap = boot, .nbootstrap = nb, .verbose = cfg->verbose,
                     .portmap = cfg->portmap, .allow_private = !main_chain || cfg->allow_private, .blocks = &l.bn};
    dc.bt_routers = bt_routers(main_chain, cfg->no_bt, cfg->bt_routers, cfg->nbt_routers, &dc.nbt_routers);
    if (!(l.d = dht_create(&dc, err, sizeof err))) return fprintf(stderr, "%s\n", err), 1;
    sys_on_stop(on_signal);
    const int nc = dht_start(l.d);
    if (nc < 0) return fprintf(stderr, "cannot listen\n"), dht_stop(l.d), 1;
    own_claims(&l, false);
    say("home on the linear network %s (%s, blocks of 2^%u keys, %llu of them): %d contact%s, %zu blocks claimed by this home before", nf->net,
        nf->address, nf->blockbits, (unsigned long long)l.nblocks, nc, nc == 1 ? "" : "s", l.nown);
    uint64_t i = 0;
    bool have = state_read(&l, &i), again = false, found = false;
    sys_proc ps = 0;
    int in = -1, rc = 0;
    int64_t since = mono_s(), next_status = mono_s() + STATUS_S, next_repub = mono_s() + REPUBLISH_S;
    while (!g_stop && !found && !rc) {
        if (!ps) {
            if (!have) i = pick(&l, &again), have = !g_stop, state_write(&l, &i);
            if (g_stop) break;
            if (again) say("every block looked at is claimed: block %llu is scanned again", (unsigned long long)i);
            if (!(ps = start(&l, i, &in))) {
                say("cannot start %s", cfg->pzsolve);
                rc = 1;
                break;
            }
            say("block %llu: pzsolve scans it, its output in %s/pzsolve.out", (unsigned long long)i, dir);
            since = mono_s();
        }
        sys_sleep_ms(500);
        int st;
        if (sys_proc_ended(ps, false, &st)) {
            char camp[1300], fpath[1400];
            snprintf(camp, sizeof camp, "%s/campaigns/b%llu", dir, (unsigned long long)i);
            snprintf(fpath, sizeof fpath, "%s/FOUND.txt", camp);
            sys_pipe_close(in), in = -1, ps = 0;
            if (st == 0 && access(fpath, F_OK) == 0) {
                say("THE KEY OF THE PUZZLE IS IN %s. Do not spend it with a wallet: its transaction shows the public key, and with it anyone"
                    " finds the key at once and sends the coins elsewhere first. pzpeer sweep --to ADDRESS --private, with the key on its input,"
                    " makes the transaction and gives it to a mining pool only", fpath);
                found = true;
            } else if (st == 1) {
                claim(&l, i, camp);
                (void)sys_remove_dir(camp);
                state_write(&l, NULL), have = false;
                if (cfg->exit_when_done) break;
            } else {
                say("pzsolve ended with status %d (all of it in %s/pzsolve.out): this home goes on as a custodian, without a search", st, dir);
                while (!g_stop) sys_sleep_ms(500);
            }
        }
        if (mono_s() >= next_status) status(&l, have, i, since), next_status = mono_s() + STATUS_S;
        if (mono_s() >= next_repub) own_claims(&l, true), next_repub = mono_s() + REPUBLISH_S;
    }
    if (ps) {                                                  /* it saves where it is in the block, and ends */
        int st;
        sys_pipe_close(in);
        (void)sys_proc_ended(ps, true, &st);
    }
    say("home stopped: %zu blocks claimed by this home%s", l.nown, found ? ", and the key found" : "");
    dht_stop(l.d);
    free(l.own);
    pz_secure_zero(seed, sizeof seed);
    return rc;
}
