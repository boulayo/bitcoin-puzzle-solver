/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* home.c - the daemon of a home (see home.h). One thread feeds the table from dp.bin; the node answers the others; every useful coincidence
 * gets a thread of its own that runs the session. The channel of a pair comes from dht_channel (docs/en/NETWORK.md 7.7): dialled by party 0
 * (the smaller identity) when it finds the partner or by a side that does not listen, the partner's own connection, or a relay.
 *
 * A hello can start a session before this home has the alert of its pair, and anyone can copy the x of a published record into one of
 * its own: such a session costs little until the partner proved its distance (session.c starts pzsign only after that), at
 * most HELLO_SESSIONS of them run at a time, one per partner, and a slot whose partner proved nothing is given back: they never take the
 * slots of the pairs the table alerted. */
#include "home.h"
#include "linear.h"
#include "btc.h"
#include "chain_rpc.h"
#include "dbg.h"
#include "bt.h"
#include "chain_spv.h"
#include "dht.h"
#include "dpfeed.h"
#include "keys.h"
#include "session.h"
#include "sweep.h"
#include "sys.h"
#include "netfile.h"
#include "json.h"
#include <pzcore/seal.h>
#include <pzcore/seed.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SESSIONS 64
#define HELLO_SESSIONS 16          /* sessions started by a hello alone, without an alert of the table (the others always have room) */
#define FAILS_MAX 10               /* failed signings of a home on a network, with any partner (project 4.6) */
#define FAILS_PAIR_MAX 3           /* on one own point, for ever, */
#define FAILS_EVER_MAX 64          /* and of this home on a network, for ever: then only a new home seed signs again */
#define PAYOUTS_MAX 5              /* payouts of one own point that reached the signing of their first transaction and did not pay, */
#define PAYOUTS_EVER_MAX 10        /* and of this home on a network, for ever: a prize is paid once, and each later point of a walk that met
                                    * another's would give that partner 5 more tries */
#define RETRY_S 600                /* a kept pair whose session did not pay starts again after this, */
#define RETRIES 144                /* for about a day (the signings that fail are counted, session.h) */
#define STATUS_S 600               /* the status line of the network, every 10 minutes */
#define C_MEAN 1.18                /* the mean cost of the walk of net-06 in units of sqrt(W): KANG_C_NET of pzsolve (src/kangaroo.h) */
#define DP_CHANGES 64              /* changes of them a home remembers */

/* the own points: x prefix -> index in dp.bin (the distance is read back from the file when it is needed: 16 bytes a point in memory) */
typedef struct {
    uint64_t *key, *idx;
    size_t cap, n;
} own_index;

typedef struct session_slot {
    bool used, running, have_conn;
    bool alerted;                   /* the table alerted the pair, or its partner proved its distance in a session: the slot is kept */
    bool proved;                    /* its partner proved its distance */
    bool waiting;                   /* its session has no channel yet: a hello of the partner is taken */
    int64_t retry_at;               /* when its session starts again (0: never), the seconds of mono_s */
    int tries;
    session_paid paid;              /* its payout, once its prize moved or its second transaction was signed */
    uint8_t eid[32];
    record mine, theirs;
    net_conn *conn;                 /* an incoming channel handed by on_session */
    uint8_t first[SESSION_HELLO_MAX];  /* the hello that came with it; a longer first message is no hello */
    size_t first_len;
} session_slot;

typedef struct {
    const home_cfg *cfg;
    const secp256k1_context *ctx;
    netfile nf;
    record_net net;
    uint8_t base[32], seed[32], pay_spk[BTC_SPK_MAX];
    size_t pay_len;
    btc_chain chain;
    const char *dir;                /* the folder of the home */
    chain_spv spv;                  /* the chain without a node of one's own (no --rpc); its headers are shared by the sessions */
    char hdrpath[4200];
    home_identity id;
    dht *d;
    char dpbin[1200], logpath[1200];
    FILE *dpr;                      /* dp.bin for the reads back */
    own_index own;
    sys_proc pzsolve;
    int solve_in;                   /* its input, a pipe pzpeer never writes: closed, or with pzpeer gone, pzsolve saves and ends */
    pthread_mutex_t mu;             /* own, dpr, the sessions */
    pthread_cond_t cv;
    session_slot s[MAX_SESSIONS];
    int ended;                      /* sessions that ended, and how: for exit_when_done */
    bool paid;
    session_limit limit;            /* the failed signings, counted in the home folder */
    payout_coin prizes[SESSION_PRIZES_MAX];   /* the prize= lines of the network file */
    int nprizes;
    char limitpath[1200], paidpath[1200], signlog[1200];
    bool prize_paid;                /* a session of this home paid the prize: pzsolve has nothing more to search */
    bool prize_gone;                /* a session found the puzzle address empty: the prize was taken (by others) */
    bool told_late;                 /* said once that coincidences after the prize are not signed */
    bool told_room;                 /* said once that the custodians cannot keep every point */
    struct { int64_t t; uint64_t held; double weight, own; } smp[7];   /* the status lines of the last hour: when, records held, the
                                                                         * jumps they and the own points stand for */
    int nsmp;
    double nodes[6];                /* the last estimates of the nodes that listen (dht_nodes) */
    int nnodes;
    unsigned dp;                    /* the zero bits of the points of pzsolve now: the network's or more (choose_dp) */
    struct { uint64_t from; unsigned dp; } dps[DP_CHANGES];   /* the zero bits of the records of dp.bin from each index on (dpbits.txt) */
    int ndps;
    char dppath[1200];
} home;

static int64_t mono_s(void) { return sys_now_ms() / 1000; }

bool home_own_dir(const char *dir)
{
    if (!sys_mkdir(dir) || !sys_private(dir))
        return fprintf(stderr, "%s must be a folder of this user that nobody else can open (on Linux and macOS: chmod 700 it)\n", dir), false;
    const int got = sys_lock_dir(dir);
    if (got < 0) fprintf(stderr, "%s: cannot make its lock file\n", dir);
    else if (!got) fprintf(stderr, "%s is the folder of another pzpeer that runs now: one pzpeer a folder (--dir gives another)\n", dir);
    return got > 0;
}

dht_config home_node(const home_cfg *cfg, const netfile *nf, const record_net *net, const home_identity *id, const char *dir)
{
    static char entries[16][NETFILE_LINE_MAX];                 /* static, as the list: one node a process, which keeps it while it runs */
    static const char *boot[16 + 32];
    int nb = 0;
    while (nb < 16 && netfile_opt(nf, "entry", nb, entries[nb], sizeof entries[nb])) boot[nb] = entries[nb], nb++;
    for (int i = 0; i < cfg->npeers && nb < 16 + 32; i++) boot[nb++] = cfg->peers[i];
    const bool main_chain = !strcmp(nf->chain, "main");
    dht_config dc = {.ctx = keys_ctx(), .net = net, .self = id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                     .listen_port = cfg->listen_port, .dir = dir, .bootstrap = boot, .nbootstrap = nb, .verbose = cfg->verbose,
                     .portmap = cfg->portmap, .allow_private = !main_chain || cfg->allow_private,
                     .store_max = (uint64_t)cfg->store_gb * 1000000000ull / STORE_ENTRY};
    dc.bt_routers = bt_routers(main_chain, cfg->no_bt, cfg->bt_routers, cfg->nbt_routers, &dc.nbt_routers);
    return dc;
}

static volatile sig_atomic_t g_stop;                            /* 1: stop; 2 (a second signal): do not wait for the sessions */
static void on_signal(int sig) { (void)sig, g_stop = g_stop < 2 ? g_stop + 1 : 2; }

/* a line for the terminal, with the time; secret: not into the debug log */
static void vsay(bool secret, const char *fmt, va_list ap)
{
    char line[600];
    vsnprintf(line, sizeof line, fmt, ap);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (!secret) dbg("home %s", line);
    fprintf(stdout, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fflush(stdout);
    pz_secure_zero(line, sizeof line);
}

void home_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(false, fmt, ap);
    va_end(ap);
}

void home_say_secret(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(true, fmt, ap);
    va_end(ap);
}

/* ---- files of the home ---- */

/* the home seed: made once (lib/pzcore/src/seed.c), and kept */
static bool home_seed(home *h, char *err, size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/home.seed", h->dir);
    const int got = keys_seed_file(path, h->seed, err, errlen);
    if (got == 2) home_say("a new home seed in %s: keep a copy of it, it is the key of every point of this home", path);
    return got > 0;
}

/* ---- the own points ---- */

static uint64_t prefix(const uint8_t x[32])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | x[i];
    return v | 1;                                               /* never 0, the empty slot */
}

static void own_add(own_index *o, const uint8_t x[32], uint64_t idx)
{
    if (2 * (o->n + 1) > o->cap) {                              /* at most half full */
        own_index g = {.cap = o->cap ? 2 * o->cap : 1u << 16};
        g.key = calloc(g.cap, sizeof *g.key), g.idx = malloc(g.cap * sizeof *g.idx);
        if (!g.key || !g.idx) return free(g.key), free(g.idx);
        for (size_t i = 0; i < o->cap; i++)
            if (o->key[i]) {
                size_t j = (size_t)(o->key[i] * 0x9E3779B97F4A7C15ULL) & (g.cap - 1);
                while (g.key[j]) j = (j + 1) & (g.cap - 1);
                g.key[j] = o->key[i], g.idx[j] = o->idx[i];
            }
        g.n = o->n;
        free(o->key), free(o->idx);
        *o = g;
    }
    const uint64_t k = prefix(x);
    size_t j = (size_t)(k * 0x9E3779B97F4A7C15ULL) & (o->cap - 1);
    while (o->key[j]) j = (j + 1) & (o->cap - 1);
    o->key[j] = k, o->idx[j] = idx, o->n++;
}

/* the distance of an own point, read back from dp.bin (mu held) */
static bool own_lookup(home *h, const uint8_t x[32], int t, uint8_t d[32])
{
    if (!h->own.cap || !h->dpr) return false;
    const uint64_t k = prefix(x);
    for (size_t j = (size_t)(k * 0x9E3779B97F4A7C15ULL) & (h->own.cap - 1); h->own.key[j]; j = (j + 1) & (h->own.cap - 1)) {
        uint8_t rec[DPBIN_REC];
        own_point p;
        if (h->own.key[j] != k || fseeko(h->dpr, (off_t)(DPBIN_HDR + h->own.idx[j] * DPBIN_REC), SEEK_SET) != 0 ||
            fread(rec, 1, sizeof rec, h->dpr) != sizeof rec)
            continue;
        const bool ok = dpfeed_point(h->ctx, &h->net, rec, &p) && !memcmp(p.x, x, 32) && p.t == t;
        if (ok) memcpy(d, p.d, 32);
        pz_secure_zero(&p, sizeof p), pz_secure_zero(rec, sizeof rec);
        if (ok) return true;
    }
    return false;
}

static bool own_distance(void *user, const uint8_t x[32], int t, uint8_t d[32])
{
    home *h = user;
    pthread_mutex_lock(&h->mu);
    const bool ok = own_lookup(h, x, t, d);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

/* ---- the payouts ---- */

/* HOME/paid: a line for every pair whose prize this home saw paid, or whose second transaction it signed, "<eid> <first txid> <height, 0
 * before the block> <second transaction, or -> <record mine> <record theirs>" (hex), so that after a restart the payout is settled and
 * what came to the address since is paid out; written whole or not at all (mu held) */
static void paid_save(home *h)
{
    char a[65], b[65], r0[2 * RECORD_LEN + 1], r1[2 * RECORD_LEN + 1], sec[2 * PAID_TX_MAX + 1];
    FILE *f = sys_new_file(h->paidpath, true);
    bool ok = f != NULL;
    for (int i = 0; ok && i < MAX_SESSIONS; i++) {
        const session_slot *s = &h->s[i];
        uint8_t m[RECORD_LEN], t[RECORD_LEN];
        if (!s->used || (!s->paid.paid_at && !s->paid.second_len)) continue;
        record_encode(&s->mine, m), record_encode(&s->theirs, t);
        pz_hex_encode(s->eid, 32, a), pz_hex_encode(s->paid.first, 32, b), pz_hex_encode(m, RECORD_LEN, r0), pz_hex_encode(t, RECORD_LEN, r1);
        if (s->paid.second_len) pz_hex_encode(s->paid.second, s->paid.second_len, sec);
        else snprintf(sec, sizeof sec, "-");
        ok = fprintf(f, "%s %s %llu %s %s %s\n", a, b, (unsigned long long)s->paid.paid_at, sec, r0, r1) > 0;
    }
    if (!sys_new_file_done(f, h->paidpath, ok)) home_say("cannot write %s: after a restart this home would not know its payouts", h->paidpath);
}

/* The pairs of HOME/paid back in their slots: each has a session soon, which settles its payout and pays out what came to the address
 * since. */
static void paid_load(home *h)
{
    FILE *f = sys_fopen(h->paidpath, "r");
    char a[80], b[80], r0[2 * RECORD_LEN + 8], r1[2 * RECORD_LEN + 8], sec[2 * PAID_TX_MAX + 8];
    unsigned long long at;
    int n = 0;
    _Static_assert(2 * PAID_TX_MAX + 8 == 1208 && 2 * RECORD_LEN + 8 == 140, "the widths of fscanf");
    while (f && n < MAX_SESSIONS && fscanf(f, "%79s %79s %llu %1207s %139s %139s", a, b, &at, sec, r0, r1) == 6) {
        const size_t sl = strcmp(sec, "-") ? strlen(sec) : 0;
        uint8_t m[RECORD_LEN], t[RECORD_LEN], eid[32];
        record pair[2];
        session_slot *s = &h->s[n];
        if (strlen(a) != 64 || strlen(b) != 64 || strlen(r0) != 2 * RECORD_LEN || strlen(r1) != 2 * RECORD_LEN || (!at && !sl) || sl % 2 ||
            sl > 2 * PAID_TX_MAX || !pz_unhex(sec, sl, s->paid.second) ||
            !pz_unhex(a, 64, eid) || !pz_unhex(b, 64, s->paid.first) || !pz_unhex(r0, 2 * RECORD_LEN, m) || !pz_unhex(r1, 2 * RECORD_LEN, t) ||
            !record_decode(m, &s->mine) || !record_decode(t, &s->theirs) || !pieces_order(&s->mine, &s->theirs, pair))
            continue;
        pieces_eid(&h->net, pair, s->eid);
        if (memcmp(s->eid, eid, 32) || memcmp(s->mine.owner, h->id.xonly, 32)) continue;   /* of another network, or another home */
        s->used = s->alerted = s->proved = true;
        s->paid.paid_at = at, s->paid.second_len = sl / 2;
        s->retry_at = mono_s() + 60;                            /* once dp.bin is read back: the distance of the own point comes from it */
        h->prize_paid |= at != 0;                               /* a second transaction not in a block yet is settled first */
        n++;
    }
    if (f) fclose(f);
}

/* The chain of this home for one task: its node through a client of its own (rpc, which the caller wipes with btc_rpc_done), or the light
 * chain they all share. False, with the reason in why, when --rpc cannot be used. */
static bool home_chain(home *h, btc_rpc *rpc, chain_rpc *crpc, session_chain *ch, char *why, size_t cap)
{
    *rpc = (btc_rpc){0};
    *crpc = (chain_rpc){.rpc = rpc, .chain = h->chain};
    if (h->cfg->rpc && !btc_rpc_init_arg(rpc, h->cfg->rpc, h->cfg->rpc_cookie)) return snprintf(why, cap, "--rpc: %s", rpc->err.msg), false;
    *ch = h->cfg->rpc ? chain_rpc_iface(crpc) : chain_spv_iface(&h->spv);
    return true;
}

/* The second transactions this home keeps that are in no block yet, sent to the chain again while their pair has no session (the main
 * loop calls it every ten minutes): a session sends them at every block, but needs the partner, which may be held by an earlier one (this
 * home was stopped and started again while the partner waits its blocks). */
static void resend_seconds(home *h)
{
    static uint8_t raw[MAX_SESSIONS][PAID_TX_MAX];             /* the main loop only */
    size_t len[MAX_SESSIONS];
    int n = 0;
    pthread_mutex_lock(&h->mu);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session_slot *s = &h->s[i];
        if (!s->used || s->running || !s->paid.second_len || s->paid.paid_at) continue;
        memcpy(raw[n], s->paid.second, s->paid.second_len), len[n++] = s->paid.second_len;
    }
    pthread_mutex_unlock(&h->mu);
    btc_rpc rpc;
    chain_rpc crpc;
    session_chain ch;
    char why[300];
    if (!n || !home_chain(h, &rpc, &crpc, &ch, why, sizeof why)) return;
    for (int i = 0; i < n; i++) (void)ch.broadcast(ch.ctx, raw[i], len[i]);
    btc_rpc_done(&rpc);
}

/* pzsolve found the key on its own (its FOUND.txt names it, unless the campaign encrypts it): the coins of the puzzle to the payout
 * address, less the share of the author (payout.h). The public key of the puzzle is known, so the transaction shows nothing new: it goes
 * to the public mempool. The key goes on the terminal once the coins are sent, or at the last try. False when they were not sent: the
 * caller tries again. */
static bool solo_payout(home *h, const char *found, bool last)
{
    char line[256], hex[65] = "", why[300] = "the key of the puzzle is not in it in the clear (encrypted?)", va[32], vb[32];
    uint8_t sec[32], k33[33];
    bool have = false, sent = false;
    FILE *f = sys_fopen(found, "rb");
    while (f && !have && fgets(line, sizeof line, f)) {
        const size_t n = strncmp(line, "private key hex: 0x", 19) ? 0 : strspn(line + 19, "0123456789abcdefABCDEF");
        if (n >= 1 && n <= 64) memset(hex, '0', 64 - n), memcpy(hex + 64 - n, line + 19, n), have = pz_unhex(hex, 64, sec);
    }
    if (f) fclose(f);
    pz_secure_zero(line, sizeof line), pz_secure_zero(hex, sizeof hex);
    secp256k1_pubkey P;
    size_t kl = 33;
    if (have && !(secp256k1_ec_pubkey_create(h->ctx, &P, sec) && secp256k1_ec_pubkey_serialize(h->ctx, k33, &kl, &P, SECP256K1_EC_COMPRESSED) &&
                  !memcmp(k33, h->nf.pubkey33, 33)))
        have = false, snprintf(why, sizeof why, "the key in it is not the key of the puzzle");
    btc_rpc rpc = {0};
    chain_rpc crpc;
    session_chain ch;
    if (have && home_chain(h, &rpc, &crpc, &ch, why, sizeof why) && ch.ready(ch.ctx, why, sizeof why) == 1) {
        payout_terms t = {0};
        sweep_info si = {0};
        uint8_t txid[32];
        char id[65];
        const char *no = payout_author(PAYOUT_TERMS, &t) ? sweep_tx(&ch, sec, h->prizes, h->nprizes, h->pay_spk, h->pay_len, &t, &si) : "no terms";
        if (no) snprintf(why, sizeof why, "%s", no);
        else if (!ch.broadcast(ch.ctx, si.raw, si.len))
            snprintf(why, sizeof why, "the chain did not take the transaction");
        else {
            btc_txid(si.raw, si.len, txid), btc_hash_to_hex(txid, id);
            home_say("the coins of the puzzle are sent: %s BTC to the payout address of this home, %s BTC (%u%%) to the author of pzpeer: %s",
                btc_amount(si.total - si.fee - si.cut, va), btc_amount(si.cut, vb), t.author_pct, id);
            if (si.taken == SWEEP_MAX_IN && si.listed > si.taken)
                home_say("the puzzle address has more coins than a standard transaction holds: the smallest are left there");
            else if (si.listed > si.taken)
                home_say("%d coins worth less than their input are left at the puzzle address", si.listed - si.taken);
            sent = true;
        }
        free(si.raw);
    }
    btc_rpc_done(&rpc);
    if (have && (sent || last)) {
        pz_hex_encode(sec, 32, hex);
        const char *digits = hex + strspn(hex, "0");             /* as pzsolve shows it: no leading zeros */
        home_say_secret("THE KEY OF THE PUZZLE: 0x%s (also in %s)", *digits ? digits : "0", found);
        pz_secure_zero(hex, sizeof hex);
    }
    pz_secure_zero(sec, sizeof sec);
    if (!sent) home_say("the coins of the puzzle are not sent%s (%s): %s", last ? ", and no more tries: move them with the key" : "", found, why);
    pthread_mutex_lock(&h->mu);
    h->prize_gone |= sent;                                      /* no pair is signed for it any more */
    pthread_mutex_unlock(&h->mu);
    return sent;
}

/* ---- pzsolve ---- */

/* the last line of a file that is not blank (the reason pzsolve gives when it ends), "" when there is none */
static void last_line(const char *path, char *out, size_t cap)
{
    char line[400];
    FILE *f = sys_fopen(path, "r");
    out[0] = 0;
    while (f && fgets(line, sizeof line, f))
        if (line[strspn(line, " \t\r\n")]) line[strcspn(line, "\r\n")] = 0, snprintf(out, cap, "%s", line);
    if (f) fclose(f);
}

/* pzsolve saves its state and ends (the end of its input is its Ctrl-C), or is interrupted after PZSOLVE_STOP_MS (sys_proc_stop) */
static void stop_pzsolve(home *h)
{
    (void)sys_proc_stop(h->pzsolve, &h->solve_in, PZSOLVE_STOP_MS);
    h->pzsolve = 0;
}

static bool start_pzsolve(home *h, char *err, size_t errlen)
{
    char key[1100], camp[1100];
    snprintf(key, sizeof key, "%s/start.key", h->dir);
    snprintf(camp, sizeof camp, "%s/campaigns", h->dir);
    uint8_t sk[32];
    keys_starts(h->seed, h->nf.fp, sk);
    const bool wrote = keys_file_write(key, sk);
    pz_secure_zero(sk, sizeof sk);
    if (!wrote) return snprintf(err, errlen, "cannot write %.300s", key), false;
    /* pzsolve knows nothing of the network: the walk of the network file as plain options of a shared walk */
    char range[2 * PZ_U256_HEX_BUF + 2], a[PZ_U256_HEX_BUF], b[PZ_U256_HEX_BUF], pub[67], js[65], dp[8], name[40];
    snprintf(range, sizeof range, "%s:%s", pz_u256_to_hex(&h->nf.start, a), pz_u256_to_hex(&h->nf.end, b));
    for (int i = 0; i < 33; i++) snprintf(pub + 2 * i, 3, "%02x", h->nf.pubkey33[i]);
    for (int i = 0; i < 32; i++) snprintf(js + 2 * i, 3, "%02x", h->nf.jumpseed[i]);
    snprintf(dp, sizeof dp, "%u", h->dp);
    snprintf(name, sizeof name, "net-%s", h->nf.net);
    const char *argv[64] = {h->cfg->pzsolve, "hunt", "-r", range, "-k", pub, "--dp-bits", dp, "--jump-seed", js, "--start-key", key,
                            "-c", name, "-d", camp, "--ui", "log", "--stop-with-input"};
    int n = 19;
    for (int i = 0; i < h->cfg->nargs && n < 62; i++) argv[n++] = h->cfg->pzsolve_args[i];
    argv[n] = NULL;
    snprintf(h->logpath, sizeof h->logpath, "%s/pzsolve.out", h->dir);
    if (!(h->pzsolve = sys_spawn(argv, h->logpath, &h->solve_in, NULL))) return snprintf(err, errlen, "cannot start %s", h->cfg->pzsolve), false;
    home_say("pzsolve started, its output in %s", h->logpath);
    return true;
}

/* ---- the zero bits ---- */

/* The zero bits of the points of this home (docs/en/NETWORK.md 6), from N, the nodes of the network, and K, the walkers of this home's
 * pzsolve. The search takes about E = 2 sqrt(W) jumps.
 * - The delay: once two walks meet, the walker that came second must still reach a distinguished point, 2^dp of its own jumps, while the
 *   network's walkers, about N K of them, walk too: the search costs about N K 2^dp jumps more, a thirty-second of E when
 *   2^dp = E / (32 N K).
 * - The room: E / 2^dp records, each kept by DHT_KSTORE nodes; N nodes that keep S records each (the default store) are half full at the
 *   end when 2^dp = 10 E / (N S).
 * The fewer of the two (a full custodian still tells the coincidences of what it is given; a delay is lost work), at least the network's, at most RECORD_DP_MORE
 * more. *room: the zero bits the room alone would want. */
static unsigned choose_dp(const home *h, double nodes, double walkers, unsigned *room)
{
    const long double W = pz_u256_to_ld(&h->nf.end) - pz_u256_to_ld(&h->nf.start) + 1, E = 2 * sqrtl(W), n = nodes > 1 ? nodes : 1;
    const long double r = ceill(log2l(10 * E / (n * DHT_STORE_DEFAULT))), d = floorl(log2l(E / (32 * n * (walkers > 1 ? walkers : 1))));
    const unsigned lo = h->nf.dpbits, hi = lo + RECORD_DP_MORE < 60 ? lo + RECORD_DP_MORE : 60;
    const long double want = r < d ? r : d;
    *room = r <= lo ? lo : r >= 64 ? 64 : (unsigned)r;
    return want <= lo ? lo : want >= hi ? hi : (unsigned)want;
}

/* the walkers of this home's pzsolve, from the status.json of its campaign; 0 when not known yet */
static double walkers_now(const home *h)
{
    char path[1300];
    snprintf(path, sizeof path, "%.1100s/campaigns/net-%.31s/status.json", h->dir, h->nf.net);
    FILE *f = sys_fopen(path, "rb");
    const long size = f && fseek(f, 0, SEEK_END) == 0 ? ftell(f) : 0;   /* one line a worker: a large machine writes a large file */
    char *text = size > 0 && size < (1 << 20) && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t)size) : NULL;
    const size_t n = text ? fread(text, 1, (size_t)size, f) : 0;
    if (f) fclose(f);
    json_doc d;
    uint64_t k = 0;
    if (n && json_parse(&d, text, n)) {
        if (!json_u64(json_get(&d.v[0], "walkers"), &k)) k = 0;
        json_free(&d);
    }
    free(text);
    return (double)k;
}

/* the whole records of dp.bin */
static uint64_t dpbin_records(const home *h)
{
    FILE *f = sys_fopen(h->dpbin, "rb");
    const off_t end = f && fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : 0;
    if (f) fclose(f);
    return end > DPBIN_HDR ? (uint64_t)(end - DPBIN_HDR) / DPBIN_REC : 0;
}

/* the zero bits of the record of dp.bin at index i: those of pzsolve when it wrote it */
static unsigned dp_at(const home *h, uint64_t i)
{
    int j = h->ndps - 1;
    while (j > 0 && h->dps[j].from > i) j--;
    return h->dps[j].dp;
}

/* dpbits.txt: a line "index zero-bits" for each change, rewritten whole at each (a point that has not the zero bits it would claim claims
 * the network's) */
static void dps_load(home *h)
{
    unsigned long long from;
    unsigned dp;
    FILE *f = sys_fopen(h->dppath, "r");
    h->dps[0].from = 0, h->dps[0].dp = h->nf.dpbits, h->ndps = 1;
    while (f && fscanf(f, "%llu %u", &from, &dp) == 2)
        if (dp >= h->nf.dpbits && dp <= h->nf.dpbits + RECORD_DP_MORE && from >= h->dps[h->ndps - 1].from) {
            if (from == h->dps[h->ndps - 1].from) h->ndps--;                   /* the same index: the later line holds */
            if (h->ndps < DP_CHANGES) h->dps[h->ndps].from = from, h->dps[h->ndps++].dp = dp;
        }
    if (f) fclose(f);
    h->dp = h->dps[h->ndps - 1].dp;
}

/* The zero bits for a network of about this many nodes, when they differ from those of pzsolve by 2 or more (a network or a home 4 times
 * bigger or smaller): each change costs pzsolve a restart. Only once the nodes come from three lookups and the walkers are known. */
static unsigned dp_for(home *h, double nodes)
{
    const double walkers = walkers_now(h);
    unsigned room;
    if (h->nnodes < 3 || walkers <= 0 || h->ndps >= DP_CHANGES) return h->dp;
    const unsigned dp = choose_dp(h, nodes, walkers, &room);
    if (room > dp + 1 && !h->told_room)
        h->told_room = true, home_say("the custodians of about %.0f nodes cannot keep every point of this search: it would take %u zero bits, and"
                                    " the walks lose too much past %u (more room on the nodes: --store-gb)", nodes, room, dp);
    return dp >= h->dp + 2 || dp + 2 <= h->dp ? dp : h->dp;
}

/* the zero bits of the next records of dp.bin (pzsolve is not running): those it wrote keep theirs */
static void set_dp(home *h, unsigned dp, double nodes)
{
    FILE *f;
    const uint64_t from = dpbin_records(h);
    while (h->ndps > 1 && h->dps[h->ndps - 1].from >= from) h->ndps--;   /* past the end of dp.bin (made again, or cut) */
    if (h->dps[h->ndps - 1].from == from) h->ndps--;
    h->dps[h->ndps].from = from, h->dps[h->ndps++].dp = dp, h->dp = dp;
    bool ok = (f = sys_new_file(h->dppath, false)) != NULL;
    for (int i = 0; ok && i < h->ndps; i++) ok = fprintf(f, "%llu %u\n", (unsigned long long)h->dps[i].from, h->dps[i].dp) > 0;
    (void)sys_new_file_done(f, h->dppath, ok);
    home_say("the network has about %.0f nodes: the points of pzsolve have %u zero bits", nodes, dp);
}

/* ---- sessions ---- */

typedef struct {
    net_conn *c;
    const uint8_t *first;           /* replayed by the first recv (a session that came to us starts with the partner's hello) */
    size_t first_len;
    int timeout_ms;                 /* the one set on c */
} conn_chan;

static bool cc_send(void *ctx, const uint8_t *m, size_t len) { return net_send(((conn_chan *)ctx)->c, m, len); }

static bool cc_recv(void *ctx, uint8_t *m, size_t cap, size_t *len, int timeout_ms)
{
    conn_chan *cc = ctx;
    if (cc->first) {
        if (cc->first_len > cap) return false;
        memcpy(m, cc->first, cc->first_len), *len = cc->first_len;
        cc->first = NULL;
        return true;
    }
    /* set only when it changes, which is before the relay of a signing: then one thread sends while another receives, and the send
     * reads the timeout of the connection too */
    if (timeout_ms != cc->timeout_ms) net_set_timeout(cc->c, cc->timeout_ms = timeout_ms);
    return net_recv(cc->c, m, cap, len);
}

typedef struct { home *h; session_slot *s; } session_job, chan_wait;

/* the notes of a session, with its tag; and its payout, kept on disk as soon as it is known */
typedef struct { home *h; session_slot *s; const char *tag; } sayer;
static void session_say(void *user, const char *line) { const sayer *w = user; home_say("session %s: %s", w->tag, line); }
static void session_on_paid(void *user, const session_paid *p)
{
    const sayer *w = user;
    pthread_mutex_lock(&w->h->mu);
    w->s->paid.paid_at = p->paid_at;
    memcpy(w->s->paid.first, p->first, 32);
    memcpy(w->s->paid.second, p->second, p->second_len), w->s->paid.second_len = p->second_len;
    paid_save(w->h);
    w->h->prize_paid |= p->paid_at != 0;                        /* a second transaction kept before its block is not a payout yet */
    pthread_mutex_unlock(&w->h->mu);
}

/* another used slot on the same own point whose partner proved its distance (mu held) */
static bool point_taken(const home *h, const session_slot *s)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (&h->s[i] != s && h->s[i].used && h->s[i].proved && !memcmp(h->s[i].mine.x, s->mine.x, 32)) return true;
    return false;
}

/* The partner of a session proved its distance: the pair has the own point, unless a pair proved there first. Whoever knows the other
 * distance can name the point again under as many identities as it likes: only one of them goes on, and the others give their slots
 * back when their sessions end (session_main). */
static bool session_on_proved(void *user)
{
    const sayer *w = user;
    pthread_mutex_lock(&w->h->mu);
    const bool ok = !point_taken(w->h, w->s);
    w->s->proved |= ok, w->s->alerted |= ok;                    /* an alert that comes after it starts no second session */
    pthread_mutex_unlock(&w->h->mu);
    return ok;
}

/* the partner's own connection to this home, which on_session handed to the slot: dht_channel looks for it while it tries the others */
static net_conn *incoming(void *arg, int ms, bool *give_up)
{
    chan_wait *w = arg;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000, until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&w->h->mu);
    while (!w->s->have_conn && !g_stop && pthread_cond_timedwait(&w->h->cv, &w->h->mu, &until) == 0) {}
    net_conn *c = w->s->have_conn ? w->s->conn : NULL;
    pthread_mutex_unlock(&w->h->mu);
    *give_up = g_stop;
    return c;
}

static void *session_main(void *arg)
{
    session_job *job = arg;
    home *h = job->h;
    session_slot *s = job->s;
    free(job);
    const home_cfg *cfg = h->cfg;
    char tag[17], why[300] = "";
    pz_hex_encode(s->eid, 8, tag);
    home_say("session %s: a coincidence with home %02x%02x%02x%02x...: the payout starts", tag, s->theirs.owner[0], s->theirs.owner[1],
        s->theirs.owner[2], s->theirs.owner[3]);
    session_result res = SESSION_FAILED;
    sayer sy = {h, s, tag};
    pthread_mutex_lock(&h->mu);
    session_paid paid = s->paid;                                /* the session writes its copy; the slot gets it back at the end */
    pthread_mutex_unlock(&h->mu);
    /* the own terms first: once the channel is open the partner's hello comes within seconds (and is waited for no longer); the chain
     * is asked once the partner proved its point (session.c) */
    btc_rpc rpc;
    chain_rpc crpc;
    session_chain chain;
    session_cfg sc = {0};
    net_conn *c = NULL;
    if (!home_chain(h, &rpc, &crpc, &chain, why, sizeof why)) goto done;
    sc = (session_cfg){.ctx = h->ctx, .net = &h->net, .identity = &h->id.kp, .mine = s->mine, .theirs = s->theirs, .pay_len = h->pay_len,
                       .min_feerate = 20000, .max_feerate = 1000000,   /* 20 to 1000 satoshis a virtual byte */
                       .pzsign = cfg->pzsign, .pzsign_log = h->signlog, .limit = &h->limit, .on_proved = session_on_proved,
                       .chain = &chain, .timeout_ms = 300000, .cancel_blocks = 3, .paid = &paid, .say = session_say, .on_paid = session_on_paid, .user = &sy,
                       /* how often the chain is asked: public servers are not to be hammered */
                       .poll_ms = h->chain == BTC_REGTEST ? 1000 : cfg->rpc ? 2000 : 15000};
    memcpy(sc.base, h->base, 32), memcpy(sc.pubkey33, h->nf.pubkey33, 33);
    memcpy(sc.pay_spk, h->pay_spk, h->pay_len);
    memcpy(sc.prizes, h->prizes, sizeof sc.prizes), sc.nprizes = h->nprizes;
    const uint32_t n = (uint32_t)s->eid[0] << 24 | (uint32_t)s->eid[1] << 16 | (uint32_t)s->eid[2] << 8 | s->eid[3];
    if (!own_distance(h, s->mine.x, s->mine.t, sc.d) || !keys_coin(h->ctx, h->seed, h->nf.fp, n, sc.coin_sec)) {
        snprintf(why, sizeof why, "the distance of the own point is not in dp.bin");
        goto done;
    }
    /* the channel (docs/en/NETWORK.md 7.7): the partner's hello if it came first (dialling as well would lose it); else dialled, the
     * partner's own connection (on_session), or through a relay */
    pthread_mutex_lock(&h->mu);
    c = s->have_conn ? s->conn : NULL;
    pthread_mutex_unlock(&h->mu);
    chan_wait w = {h, s};
    if (!c) c = dht_channel(h->d, s->theirs.owner, s->eid, 600000, incoming, &w);
    pthread_mutex_lock(&h->mu);
    s->waiting = false;
    conn_chan cc = {c, c && c == s->conn ? s->first : NULL, s->first_len, 0};
    pthread_mutex_unlock(&h->mu);
    session_chan chan = {&cc, cc_send, cc_recv};
    sc.chan = &chan;
    if (!c) snprintf(why, sizeof why, "no channel with the partner");
    else res = session_run(&sc, why, sizeof why);
done:
    pz_secure_zero(&sc, sizeof sc);
    btc_rpc_done(&rpc);
    home_say("session %s: ended: %s%s", tag, res == SESSION_PAID ? "" : res == SESSION_CANCELLED ? "cancelled: " : "failed: ", why);
    pthread_mutex_lock(&h->mu);
    s->paid = paid;                                             /* a payout was kept, and counted, when the session saw it (session_on_paid) */
    h->prize_gone |= paid.empty && !paid.paid_at;
    if (c) net_close(c);
    if (s->have_conn && s->conn != c) net_close(s->conn);      /* the partner's connection came too, after another one */
    s->running = false, s->waiting = false, s->have_conn = false, s->conn = NULL;
    s->retry_at = s->alerted && !paid.empty && res != SESSION_PAID && ++s->tries < RETRIES ? mono_s() + RETRY_S : 0;   /* again in 10 minutes */
    /* a pair whose partner never proved anything gives its slot back, and so does one out of tries (an alert that comes again starts it
     * afresh, within the limits of session.h); a paid pair keeps its slot, so that it is not started again */
    s->used = s->alerted && (res == SESSION_PAID || s->retry_at || paid.paid_at);   /* a paid pair keeps its slot: the partner may come back */
    s->used &= s->proved || !point_taken(h, s);                 /* the own point has its pair: this one, another identity or a copy, goes */
    h->ended++;
    h->paid |= res == SESSION_PAID;
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

/* the slot of a pair (mu held): found, or (make) made; NULL when there is none, or no room. Once a partner proved its distance on an own
 * point, no other pair on that point gets a slot: whoever knows that distance could name the point again under new identities. */
static session_slot *slot_of(home *h, const record *mine, const record *theirs, bool make, bool *fresh)
{
    record pair[2];
    uint8_t eid[32];
    *fresh = false;
    if (!pieces_order(mine, theirs, pair)) return NULL;
    pieces_eid(&h->net, pair, eid);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (h->s[i].used && !memcmp(h->s[i].eid, eid, 32)) return &h->s[i];
        make &= !(h->s[i].used && h->s[i].proved && !memcmp(h->s[i].mine.x, mine->x, 32));
    }
    for (int i = 0; make && i < MAX_SESSIONS; i++)
        if (!h->s[i].used) {
            session_slot *s = &h->s[i];
            memset(s, 0, sizeof *s);
            s->used = *fresh = true;
            memcpy(s->eid, eid, 32);
            s->mine = *mine, s->theirs = *theirs;
            return s;
        }
    return NULL;
}

static void start_session(home *h, session_slot *s)
{
    session_job *job = malloc(sizeof *job);
    pthread_t t;
    if (!job) return;
    job->h = h, job->s = s;
    s->running = true, s->waiting = true;                      /* until it has a channel the partner's hello is taken: it may be ready first */
    if (pthread_create(&t, NULL, session_main, job) != 0) {
        s->running = false;
        free(job);
        return;
    }
    pthread_detach(t);
}

static void on_alert(void *user, const record *mine, const record *theirs, bool merge)
{
    home *h = user;
    if (merge) return home_say("two walkers of the same kind met, this home's and another's: no key comes from it");   /* a restart is to come */
    bool fresh;
    pthread_mutex_lock(&h->mu);
    const bool late = h->prize_paid || h->prize_gone;          /* only the pairs that paid go on, for what comes to the address later */
    session_slot *s = slot_of(h, mine, theirs, !late, &fresh);
    if (late && !s && !h->told_late) home_say("a coincidence after the prize was taken: nothing to sign (and so for the next ones)"), h->told_late = true;
    if (s) s->alerted = true;                                   /* also a slot that a hello made first */
    if (s && fresh) start_session(h, s);                       /* an alert that comes again (a restart, a mailbox) changes nothing */
    pthread_mutex_unlock(&h->mu);
}

/* an incoming session: its first message is the hello of the partner, which names the pair */
static void on_session(void *user, net_conn *c, const uint8_t *msg, size_t len)
{
    home *h = user;
    record a, b;
    if (len < 1 + 2 * RECORD_LEN || len > SESSION_HELLO_MAX || msg[0] != S_HELLO || !record_decode(msg + 1, &a) ||
        !record_decode(msg + 1 + RECORD_LEN, &b) ||
        memcmp(a.owner, c->remote, 32) || memcmp(b.owner, h->id.xonly, 32) || !record_check(h->ctx, &h->net, &a) ||
        !record_check(h->ctx, &h->net, &b)) {
        net_close(c);
        return;
    }
    bool fresh;
    pthread_mutex_lock(&h->mu);
    session_slot *s = slot_of(h, &b, &a, false, &fresh);       /* mine is b: the partner's hello lists its record first */
    if (!s && !h->prize_paid && !h->prize_gone) {               /* no alert yet: room for it, one per partner (none after the prize) */
        int open = 0;
        bool busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            const session_slot *o = &h->s[i];
            if (o->used && !o->alerted) open++, busy |= !memcmp(o->theirs.owner, a.owner, 32);
            busy |= o->used && o->running && !memcmp(o->mine.x, b.x, 32);   /* one session on a point at a time: a partner proving there
                                                                              * cannot open others under new identities meanwhile */
        }
        if (!busy && open < HELLO_SESSIONS) s = slot_of(h, &b, &a, true, &fresh);
    }
    /* a session that runs takes the connection only while it waits for one */
    if (!s || s->have_conn || (s->running && !s->waiting)) {
        pthread_mutex_unlock(&h->mu);
        net_close(c);
        return;
    }
    s->conn = c, s->have_conn = true;
    memcpy(s->first, msg, len), s->first_len = len;
    if (!s->running) start_session(h, s);                      /* no alert here yet, or an earlier try that ended: the hello is enough */
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

/* ---- the time the network needs ---- */

/* a number with the units of pzsolve (k, M, G, ...), "1234" below a thousand */
static const char *si(double v, char *out, size_t cap)
{
    static const char *const U[] = {"", " k", " M", " G", " T", " P", " E", " Z", " Y"};
    int u = 0;
    for (; v >= 999.5 && u < 8; u++) v /= 1000;
    snprintf(out, cap, u ? "%.3g%s" : "%.0f%s", v, U[u]);
    return out;
}

static const char *span(double s, char *out, size_t cap)
{
    if (!isfinite(s) || s < 0) snprintf(out, cap, "unknown");
    else if (s < 3600) snprintf(out, cap, "%.0f minutes", s / 60);
    else if (s < 2 * 86400) snprintf(out, cap, "%.1f hours", s / 3600);
    else if (s < 2 * 365.25 * 86400) snprintf(out, cap, "%.0f days", s / 86400);
    else snprintf(out, cap, "%.3g years", s / (365.25 * 86400));
    return out;
}

/* The nodes of the network that listen: the average of the last six estimates of dht_nodes, each a fifth off. */
static double nodes_now(home *h)
{
    const double n = dht_nodes(h->d);
    if (n > 0) {
        if (h->nnodes == 6) memmove(h->nodes, h->nodes + 1, 5 * sizeof h->nodes[0]), h->nnodes = 5;
        h->nodes[h->nnodes++] = n;
    }
    double nodes = 0;
    for (int i = 0; i < h->nnodes; i++) nodes += h->nodes[i] / h->nnodes;
    return nodes;
}

/* The work of the whole network, from what this home holds as a custodian: a record goes to the DHT_KSTORE nodes closest to its key, so
 * the network holds about held * nodes / DHT_KSTORE of them, each worth 2^(its zero bits) jumps on average; its rate is the growth of
 * their worth over the last hour. With the kangaroo the key comes after n jumps with P = 1 - exp(-(n / c)^2), c = C_MEAN / 0.8862 *
 * sqrt(W) (docs/en/RESEARCH.md 4.5): given that it has not come after `done`, half the chance is within sqrt(done^2 + c^2 ln 2) - done
 * more jumps, 9 in 10 within sqrt(done^2 + c^2 ln 10) - done. own: the points of this home, own_w the jumps they stand for. */
static void network_status(home *h, int contacts, double nodes, uint64_t own, double own_w)
{
    const uint64_t held = dht_local_count(h->d);
    const double weight = dht_local_weight(h->d);
    const int64_t now = mono_s();
    if (h->nsmp == 7) memmove(h->smp, h->smp + 1, 6 * sizeof h->smp[0]), h->nsmp = 6;
    h->smp[h->nsmp].t = now, h->smp[h->nsmp].held = held, h->smp[h->nsmp].weight = weight, h->smp[h->nsmp].own = own_w, h->nsmp++;
    const double dt = (double)(now - h->smp[0].t);
    const double share = nodes / (nodes < DHT_KSTORE ? (nodes > 1 ? nodes : 1) : DHT_KSTORE);   /* records in the network per record held */
    const char *why = !h->cfg->listen_port ? " (this home does not listen: no time estimate)"
                    : h->nsmp < 2 || dt <= 0 || nodes <= 0 ? "; the time estimate comes with the next line, in 10 minutes"
                    : !held ? "; no record held here: if it lasts, other nodes cannot reach this home on its port (open it on the router),"
                              " and there is no time estimate" : NULL;
    if (why)
        return home_say("network: %d contact%s; %llu points of this home, %llu records held here as a custodian%s", contacts, contacts == 1 ? "" : "s",
                   (unsigned long long)own, (unsigned long long)held, why);
    const double grew = weight > h->smp[0].weight ? weight - h->smp[0].weight : 0;   /* a restart of the store can only lower it */
    const double rate = grew / dt * share, mine = (own_w - h->smp[0].own) / dt;
    const long double W = pz_u256_to_ld(&h->nf.end) - pz_u256_to_ld(&h->nf.start) + 1, c = C_MEAN / 0.8862269254527580L * sqrtl(W);
    const long double done = (long double)weight * share;
    char a[40], b[40], pts[24], jr[24], sh[16];
    const double t50 = rate > 0 ? (double)((sqrtl(done * done + c * c * logl(2)) - done) / rate) : INFINITY;
    const double t90 = rate > 0 ? (double)((sqrtl(done * done + c * c * logl(10)) - done) / rate) : INFINITY;
    const double pc = rate > 0 ? 100 * mine / rate : 0;
    snprintf(sh, sizeof sh, pc >= 1 || pc == 0 ? "%.0f%%" : "%.2g%%", pc > 100 ? 100 : pc);   /* the estimate can put this home above it */
    /* what this home gets on average when the network finds the key: its share of the work times what the terms leave to the homes
     * (two homes split a prize, and one alone takes it whole: either way the share of the work, on average) */
    payout_terms t;
    char ev[48] = "";
    if (payout_author(PAYOUT_TERMS, &t) && pc > 0) snprintf(ev, sizeof ev, ": %.2g%% of the prize on average", (pc > 100 ? 100 : pc) * (100 - t.author_pct) / 100);
    home_say("network: about %.0f nodes, %s points in all, %s jumps/s (this home %s%s); half the chance within %s, 9 in 10 within %s", nodes,
        si((double)held * share, pts, sizeof pts), si(rate, jr, sizeof jr), sh, ev, span(t50, a, sizeof a), span(t90, b, sizeof b));
}

/* ---- the run ---- */

/* the first line of a file of the folder, without the end of the line ("" when there is none) */
static void read_line(const char *path, char *out, size_t cap)
{
    FILE *f = sys_fopen(path, "rb");
    if (!f || !fgets(out, (int)cap, f)) out[0] = 0;
    if (f) fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

static bool write_line(const char *path, const char *text)
{
    FILE *f = sys_fopen(path, "w");
    bool ok = f && fprintf(f, "%s\n", text) > 0;
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

/* a word typed on the terminal after the prompt, which goes to stderr, the terminal even when the output goes to a file ("" at the end
 * of the input) */
static void ask(const char *prompt, char *out, size_t cap)
{
    fputs(prompt, stderr);
    char line[256];
    if (!fgets(line, sizeof line, stdin)) line[0] = 0;
    for (int c = 0; !strchr(line, '\n') && c != '\n' && c != EOF;) c = getchar();   /* the rest of a long line */
    const char *w = line + strspn(line, " \t");
    snprintf(out, cap, "%.*s", (int)strcspn(w, " \t\r\n"), w);
}

/* The terms of the payout, the same for every home and set by the program (payout.h): a line at every start, and accepted once, at the
 * first start on the terminal or with --accept-terms (HOME/terms keeps the version accepted; a new version asks again). */
static bool terms(home *h)
{
    payout_terms t;
    char path[1100], line[16];
    if (!payout_author(PAYOUT_TERMS, &t)) return false;
    const unsigned a = t.author_pct, pair = (100 - a) / 2;
    snprintf(path, sizeof path, "%s/terms", h->dir);
    read_line(path, line, sizeof line);
    if (strtoul(line, NULL, 10) < PAYOUT_TERMS && !h->cfg->accept_terms) {
        if (!isatty(0)) return fprintf(stderr, "the terms of the payout are not accepted yet: start pzpeer run once in a terminal, or add --accept-terms\n"), false;
        fprintf(stderr, "The terms of the payout, the same for every home and set by the program:\n"
               "  - a prize found by two homes together: %u%% to the payout address of each, %u%% to the author of pzpeer (%s);\n"
               "  - a key this home finds on its own: %u%% to its payout address, %u%% to the author, sent at once;\n"
               "  - the coins go straight from the puzzle address to these addresses: nobody holds them, the author included.\n",
               pair, a, t.author_address, 100 - a, a);
        ask("Type yes to accept them: ", line, sizeof line);
        if (strcmp(line, "yes")) return fprintf(stderr, "the terms are not accepted: the home does not start\n"), false;
    }
    snprintf(line, sizeof line, "%u", PAYOUT_TERMS);
    if (!write_line(path, line)) fprintf(stderr, "cannot write %s: the terms will be asked again\n", path);
    home_say("terms: %u%% to each home of a pair and %u%% to the author of pzpeer; %u%% to a home that finds the key on its own", pair, a, 100 - a);
    return true;
}

/* The payout address: --payout, else the one kept in HOME/payout, else asked on the terminal; kept there for the next start. */
static bool payout_address(home *h)
{
    char path[1100], addr[128] = "";
    snprintf(path, sizeof path, "%s/payout", h->dir);
    if (h->cfg->payout) snprintf(addr, sizeof addr, "%s", h->cfg->payout);
    else read_line(path, addr, sizeof addr);
    for (int k = 0; !(h->pay_len = addr[0] ? btc_address_script(h->chain, addr, h->pay_spk) : 0); k++) {
        if (addr[0]) fprintf(stderr, "%s: not an address of the %s chain\n", addr, h->nf.chain);
        if (h->cfg->payout || k == 3 || !isatty(0))
            return fprintf(stderr, addr[0] ? "give it with --payout ADDRESS\n" : "--payout ADDRESS is missing (on a terminal it is asked for)\n"), false;
        ask("The payout address of this home, where its share of a prize goes (1..., 3..., bc1q... or bc1p...): ", addr, sizeof addr);
    }
    if (!write_line(path, addr)) fprintf(stderr, "cannot write %s: give the address with --payout next time\n", path);
    home_say("payout address of this home: %s", addr);
    return true;
}

/* The chain without a node: the Electrum servers of the network file and of the command line, the headers from the checkpoint of the file
 * (checkpoint=HEIGHT:HASH, the hash as explorers show it); for what they leave out, those of the program on the main chain, and the first
 * block on another (chain_spv_init). */
static bool light_chain(home *h, const home_cfg *cfg, char *err, size_t cap)
{
    static char servers[CHAIN_SPV_SERVERS][NETFILE_LINE_MAX];
    const char *list[CHAIN_SPV_SERVERS];
    uint8_t cp[32];
    unsigned long long cph = 0;
    bool has_cp = false;
    int n = 0;
    char line[NETFILE_LINE_MAX], hex[65];
    for (int i = 0; i < cfg->nelectrum && n < CHAIN_SPV_SERVERS; i++) list[n++] = cfg->electrum[i];
    for (int i = 0; n < CHAIN_SPV_SERVERS && netfile_opt(&h->nf, "electrum", i, servers[n], sizeof servers[n]); i++) list[n] = servers[n], n++;
    for (int i = 0; netfile_opt(&h->nf, "checkpoint", i, line, sizeof line); i++)   /* of several, the last */
        if (sscanf(line, "%llu:%64[0-9a-fA-F]", &cph, hex) == 2 && btc_hash_from_hex(hex, cp)) has_cp = true;
    if (!n && h->chain != BTC_MAIN)
        return snprintf(err, cap, "no chain: give --rpc (a bitcoind of your own) or --electrum HOST:PORT (public servers, checked)"), false;
    snprintf(h->hdrpath, sizeof h->hdrpath, "%s/headers.bin", h->dir);
    if (!chain_spv_init(&h->spv, h->chain, has_cp ? cph : 0, has_cp ? cp : NULL, list, n, h->hdrpath))
        return snprintf(err, cap, "the checkpoint of the network file is not a block of the chain to start from"), false;
    /* the first start fetches some 640 000 headers, minutes: now, before the network starts, so that no session waits for them */
    uint64_t from = h->spv.headers.base;
    const bool kept = spv_tip(&h->spv.headers, &from);
    if (h->spv.max_tip_age && (uint64_t)time(NULL) > spv_tip_time(&h->spv.headers) + h->spv.max_tip_age)
        home_say("chain: the block headers since block %llu are fetched from the servers%s", (unsigned long long)from,
                 kept ? "" : " (the first time: minutes)");
    const int ready = chain_spv_ready(&h->spv, err, cap);
    home_say("chain: %d Electrum server%s, %s%s", h->spv.nservers, h->spv.nservers == 1 ? "" : "s", err, ready ? "" : " (a session waits for it)");
    return true;
}

int home_run(const home_cfg *cfg)
{
    static home hs;
    home *h = &hs;
    char err[400];
    memset(h, 0, sizeof *h);
    h->cfg = cfg, h->solve_in = -1;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->ctx = keys_ctx();
    if (!netfile_get(cfg->network, cfg->puzzle, &h->nf, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    h->dir = cfg->dir;
    if (h->nf.linear) return linear_run(cfg, &h->nf, h->dir);   /* a puzzle without a public key: blocks, claims, no payout */
    snprintf(h->limitpath, sizeof h->limitpath, "%s/failed-signings", h->dir);
    snprintf(h->paidpath, sizeof h->paidpath, "%s/paid", h->dir);
    snprintf(h->signlog, sizeof h->signlog, "%s/pzsign.out", h->dir);       /* what pzsign says: not for the terminal */
    snprintf(h->dpbin, sizeof h->dpbin, "%.1100s/campaigns/net-%.31s/dp.bin", h->dir, h->nf.net);
    pthread_mutex_init(&h->limit.mu, NULL);
    h->limit.path = h->limitpath, h->limit.max = FAILS_MAX, h->limit.pair_max = FAILS_PAIR_MAX, h->limit.ever_max = FAILS_EVER_MAX;
    h->limit.pay_max = PAYOUTS_MAX, h->limit.pay_ever_max = PAYOUTS_EVER_MAX;
    pz_u256_to_be(&h->nf.start, h->base);
    if (!record_net_init(h->ctx, &h->net, h->nf.fp, h->nf.dpbits, h->nf.pubkey33, h->base)) return fprintf(stderr, "invalid network\n"), 1;
    snprintf(h->dppath, sizeof h->dppath, "%.1100s/dpbits.txt", h->dir);
    dps_load(h);
    if (!btc_chain_parse(h->nf.chain, &h->chain)) return fprintf(stderr, "unknown chain %s\n", h->nf.chain), 1;
    if (!terms(h) || !payout_address(h)) return 2;
    const char *const sv[] = {cfg->pzsign, "version", NULL};    /* the signer too: a wrong --pzsign now, not after a coincidence */
    const sys_proc sp = sys_spawn(sv, h->signlog, NULL, NULL);
    int sst = -1;
    if (!sp || !sys_proc_ended(sp, true, &sst) || sst) return fprintf(stderr, "%s does not run (pzsign version): --pzsign gives the signer\n", cfg->pzsign), 1;
    if (cfg->rpc) {                                             /* the node: a wrong --rpc or chain now, not at the first session */
        btc_rpc rpc;
        chain_rpc crpc = {.rpc = &rpc, .chain = h->chain};
        int ready = 0;
        if (btc_rpc_init_arg(&rpc, cfg->rpc, cfg->rpc_cookie)) ready = chain_rpc_ready(&crpc, err, sizeof err);
        else if (rpc.err.kind == BTC_RPC_EPROTO) return fprintf(stderr, "--rpc: %s\n", rpc.err.msg), 1;
        else snprintf(err, sizeof err, "%s", rpc.err.msg);      /* no cookie yet: the node is not running */
        const bool refused = !ready && !cfg->rpc_cookie && rpc.err.kind == BTC_RPC_EHTTP && rpc.err.code == 401;
        btc_rpc_done(&rpc);
        if (refused) return fprintf(stderr, "--rpc: the node refused the user or the password (HTTP 401)\n"), 2;   /* a cookie may change */
        if (ready < 0) return fprintf(stderr, "bitcoind: %s\n", err), 1;
        home_say("bitcoind: %s%s", err, ready ? "" : " (a session waits for it)");
    } else if (!light_chain(h, cfg, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!home_seed(h, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!keys_identity(h->ctx, h->seed, h->nf.fp, cfg->id_bits, &h->id)) return fprintf(stderr, "no identity\n"), 1;
    paid_load(h);
    char line[NETFILE_LINE_MAX];
    /* the prizes, prize=txid:vout:sats (the txid as explorers show it; the value is the chain's), of the file or of the program for its
     * networks: a session finds them even when no scan of the puzzle address lists them (session_coins) */
    for (int i = 0; h->nprizes < SESSION_PRIZES_MAX && netfile_opt(&h->nf, "prize", i, line, sizeof line); i++) {
        char tx[65];
        unsigned vout;
        unsigned long long sats;
        if (sscanf(line, "%64[0-9a-fA-F]:%u:%llu", tx, &vout, &sats) == 3 && btc_hash_from_hex(tx, h->prizes[h->nprizes].txid))
            h->prizes[h->nprizes++].vout = vout;
        else home_say("the line prize=%s of the network file is not TXID:VOUT:SATOSHIS: it is left out", line);
    }
    char nodedir[1100];
    snprintf(nodedir, sizeof nodedir, "%s/node", h->dir);
    dht_config dc = home_node(cfg, &h->nf, &h->net, &h->id, nodedir);
    dc.on_alert = on_alert, dc.own_distance = own_distance, dc.on_session = on_session, dc.user = h;
    if (!(h->d = dht_create(&dc, err, sizeof err))) return fprintf(stderr, "%s\n", err), 1;
    sys_on_stop(on_signal);
    const int nc = dht_start(h->d);
    char idhex[17];
    pz_hex_encode(h->id.xonly, 8, idhex);
    home_say("home %s... on network %s: %d contact%s, %s", idhex, h->nf.net, nc, nc == 1 ? "" : "s", cfg->listen_port ? "listening" : "not listening");
    if (h->prize_paid) home_say("the prize of this network is paid: pzsolve is not started");
    int rc = nc < 0 || (!h->prize_paid && !start_pzsolve(h, err, sizeof err));
    if (rc && nc >= 0) fprintf(stderr, "%s\n", err);          /* dht_start says itself why it did not start */
    /* the feed: the new points of dp.bin to the table, every half second */
    dpfeed df = {0};
    uint64_t published = 0, found = 0;
    double found_w = 0;                                         /* the jumps the new points of this home stand for */
    const uint64_t first_new = dpbin_records(h);                /* the points of dp.bin before this start were counted before */
    unsigned next_dp = 0;                                       /* other zero bits for pzsolve, from the last status line */
    double dp_nodes = 0;
    bool solved_alone = false, searched = false, met = nc > 0;
    int solo = 0;                                               /* the tries left to send the coins of a key pzsolve found on its own */
    int64_t solo_at = 0;
    char fpath[1300];
    snprintf(fpath, sizeof fpath, "%.1100s/campaigns/net-%.31s/FOUND.txt", h->dir, h->nf.net);
    int64_t next_status = mono_s() + STATUS_S;
    int pause = 500;                                            /* short while dp.bin has more to read */
    while (!rc && !g_stop) {
        sys_sleep_ms(pause);
        const int contacts = dht_count(h->d);                  /* what the network is to this home: at the first contact, and every 10 minutes */
        if ((contacts > 0) != met || mono_s() >= next_status) {
            const double nodes = contacts ? nodes_now(h) : 0;
            if (contacts) network_status(h, contacts, nodes, found, found_w);
            else home_say("network: no contact, looking for the others; %llu points of this home wait for them", (unsigned long long)found);
            if (contacts) next_dp = dp_for(h, nodes), dp_nodes = nodes;
            const uint64_t waiting = h->pzsolve && contacts ? dpfeed_waiting(&df) : 0;
            if (waiting > 1000) home_say("%llu points of this home are not published yet: it finds them faster than it publishes them, and the network sees"
                                       " them late", (unsigned long long)waiting);
            met = contacts > 0, next_status = mono_s() + STATUS_S;
            resend_seconds(h);
        }
        if (!df.f && dpfeed_open(&df, h->dpbin)) {
            pthread_mutex_lock(&h->mu);
            h->dpr = sys_fopen(h->dpbin, "rb");                 /* the distances of the home: not for pzsign, nor in a buffer of stdio */
            if (h->dpr) setvbuf(h->dpr, NULL, _IONBF, 0);
            pthread_mutex_unlock(&h->mu);
        }
        pthread_mutex_lock(&h->mu);
        const bool paid_now = h->prize_paid, gone = h->prize_gone;
        pthread_mutex_unlock(&h->mu);
        /* the new points of dp.bin, a batch at a time (the lookups of a batch run together), for 2 seconds at most a turn: the loop must
         * still see a stop, the end of pzsolve and its status lines. Once the prize is taken the points are only indexed, for the later
         * sessions of the pairs that paid */
        own_point p[DHT_STORE_MAX];
        record recs[DHT_STORE_MAX];
        pause = 500;
        for (const int64_t t0 = sys_now_ms(); df.f && !g_stop && sys_now_ms() - t0 < 2000;) {
            const int n = dpfeed_read(h->ctx, &h->net, &df, p, DHT_STORE_MAX);
            if (n <= 0) break;
            pause = n == DHT_STORE_MAX ? 10 : 500;              /* more to read: the next turn comes at once */
            pthread_mutex_lock(&h->mu);
            for (int i = 0; i < n; i++) {
                own_add(&h->own, p[i].x, p[i].index);
                memset(&recs[i], 0, sizeof recs[i]);
                const unsigned dp = dp_at(h, p[i].index);
                recs[i].t = p[i].t, recs[i].dp = record_dp_ok(p[i].x, dp) ? dp : h->nf.dpbits;
                if (p[i].index >= first_new) found_w += ldexp(1, (int)recs[i].dp);
                memcpy(recs[i].x, p[i].x, 32);
            }
            pthread_mutex_unlock(&h->mu);
            pz_secure_zero(p, sizeof p);
            found += (uint64_t)n;
            if (!paid_now && !gone) published += (uint64_t)dht_publish(h->d, recs, n);
        }
        int st;
        if (h->pzsolve && (paid_now || gone)) {                 /* nothing is left to search: the home stays on, a custodian */
            home_say(paid_now ? "the prize is paid: pzsolve stops" : "the puzzle address is empty, the prize has been taken: pzsolve stops");
            stop_pzsolve(h);
        }
        if (h->pzsolve && sys_proc_ended(h->pzsolve, false, &st)) {
            char why[400];
            solved_alone = st == 0 && access(fpath, F_OK) == 0;  /* pzsolve ends with 0 when it found the key itself */
            last_line(h->logpath, why, sizeof why);
            if (solved_alone) home_say("pzsolve found the key on its own: it is in %s", fpath);
            else home_say("pzsolve ended with status %d: %s (all of it in %s); this home goes on as a custodian, without a search", st,
                     why[0] ? why : "no reason given", h->logpath);
            sys_pipe_close(h->solve_in), h->solve_in = -1, h->pzsolve = 0;   /* ended: its input closed */
            solo = solved_alone, solo_at = mono_s() + cfg->solo_wait, searched = true;
        }
        if (h->pzsolve && next_dp && next_dp != h->dp && !paid_now && !gone) {   /* it saves its walkers, and goes on with the new zero bits */
            stop_pzsolve(h);
            set_dp(h, next_dp, dp_nodes);
            if (!start_pzsolve(h, err, sizeof err)) home_say("%s: this home goes on as a custodian, without a search", err), searched = true;
        }
        next_dp = 0;
        if (solo && mono_s() >= solo_at) {                      /* at once, but after the sessions of this home, which may be paying it */
            pthread_mutex_lock(&h->mu);
            bool paying = false;
            for (int i = 0; i < MAX_SESSIONS; i++)              /* also a second signed with a partner, which may pay it yet */
                paying |= h->s[i].running || (h->s[i].used && h->s[i].paid.second_len && !h->s[i].paid.paid_at);
            pthread_mutex_unlock(&h->mu);
            const bool last = solo >= 60 || cfg->exit_when_done;   /* a chain that does not answer is asked again for an hour */
            if (paid_now) solo = 0;                             /* a pair of this home paid it */
            else if (!paying) solo = solo_payout(h, fpath, last) || last ? 0 : solo + 1;
            solo_at = mono_s() + 60;
        }
        if (searched && !solo && cfg->exit_when_done) break;
        pthread_mutex_lock(&h->mu);
        const bool done = cfg->exit_when_done && h->ended > 0 && !solo;
        bool busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_slot *s = &h->s[i];
            if (s->used && !s->running && s->retry_at && mono_s() >= s->retry_at) s->retry_at = 0, start_session(h, s);   /* again */
            busy |= s->running;
        }
        pthread_mutex_unlock(&h->mu);
        if (done && !busy) break;
    }
    if (h->pzsolve) stop_pzsolve(h);                            /* it saves its state and ends */
    if (!g_stop) g_stop = 1;                                    /* a session that waits for its channel gives up now */
    bool busy = true, paid = false;
    for (int wait = 0; busy && wait < 600; wait++) {            /* sessions under way end on their own: a payout is not cut in half */
        pthread_mutex_lock(&h->mu);
        pthread_cond_broadcast(&h->cv);
        busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) busy |= h->s[i].running;
        paid = h->paid;
        pthread_mutex_unlock(&h->mu);
        if (busy && g_stop < 2) sys_sleep_ms(1000);
        else break;
    }
    if (h->prize_paid && !paid) home_say("home stopped: the prize is paid");
    else home_say("home stopped: %llu points published, %s", (unsigned long long)published, paid ? "a session paid" : solved_alone ? "pzsolve found the key" : "no payout");
    if (!busy) {                                                /* else sessions still use the node, the context and the keys: the exit ends them */
        dht_stop(h->d);
        dpfeed_close(&df);
        if (h->dpr) fclose(h->dpr);
        if (!cfg->rpc) chain_spv_free(&h->spv);
        pz_secure_zero(h->seed, sizeof h->seed);
        pz_secure_zero(&h->id, sizeof h->id);
    }
    return rc ? rc : (paid || solved_alone || !cfg->exit_when_done) ? 0 : 3;
}
