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
#include "btc.h"
#include "chain_rpc.h"
#include "dbg.h"
#include "bt.h"
#include "chain_spv.h"
#include "dht.h"
#include "dpfeed.h"
#include "keys.h"
#include "session.h"
#include "sys.h"
#include "netfile.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <pzcore/seed.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SESSIONS 64
#define HELLO_SESSIONS 16          /* sessions started by a hello alone, without an alert of the table (the others always have room) */
#define FAILS_MAX 10               /* failed signings of a home on a network, with any partner (project 4.6) */
#define FAILS_PAIR_MAX 3           /* and with one pair */
#define PAYOUTS_MAX 5              /* payouts of one own point that reached the signing of their first transaction and did not pay */
#define RETRY_S 600                /* a kept pair whose session did not pay starts again after this, */
#define RETRIES 144                /* for about a day (the signings that fail are counted, session.h) */
#define STATUS_S 600               /* the status line of the network, every 10 minutes */
#define C_MEAN 1.06                /* the mean cost of the kangaroo in units of sqrt(W): KANG_C_MEAN of pzsolve (src/kangaroo.h) */

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
    session_paid paid;              /* its payout, once its prize moved, and the key once the pieces are exchanged */
    uint8_t eid[32], partner[32];
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
    char dirname[48];               /* its name when the user gave none */
    chain_spv spv;                  /* the chain without a node of one's own (no --rpc); its headers are shared by the sessions */
    char hdrpath[4200];
    home_identity id;
    dht *d;
    char dpbin[1200], logpath[1200];
    FILE *dpr;                      /* dp.bin for the reads back */
    own_index own;
    sys_proc pzsolve;
    pthread_mutex_t mu;             /* own, dpr, the sessions */
    pthread_cond_t cv;
    session_slot s[MAX_SESSIONS];
    int ended;                      /* sessions that ended, and how: for exit_when_done */
    bool paid;
    session_limit limit;            /* the failed signings, counted in the home folder */
    payout_coin prizes[SESSION_PRIZES_MAX];   /* the prize= lines of the network file */
    int nprizes;
    char limitpath[1200], paidpath[1200], foundpath[1200];
    bool prize_paid;                /* a session of this home paid the prize: pzsolve has nothing more to search */
    struct { int64_t t; uint64_t held, own; } smp[7];   /* the status lines of the last hour: when, records held, own points */
    int nsmp;
    double nodes[6];                /* the last estimates of the nodes that listen (dht_nodes) */
    int nnodes;
} home;

static int64_t mono_s(void) { return sys_now_ms() / 1000; }

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

static void say(const home *h, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(false, fmt, ap);
    va_end(ap);
    (void)h;
}

static void say_secret(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(true, fmt, ap);
    va_end(ap);
}

/* ---- files of the home ---- */

/* the home seed: made once, from several sources (lib/pzcore/src/seed.c), and kept */
static bool home_seed(home *h, char *err, size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/home.seed", h->dir);
    const int got = keys_seed_file(path, h->seed, err, errlen);
    if (got == 2) say(h, "a new home seed in %s: keep a copy of it, it is the key of every point of this home", path);
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

/* ---- the payouts and the key ---- */

/* HOME/paid: a line for every pair whose prize this home saw paid, "<eid> <first txid> <height> <record mine> <record theirs>" (hex), so that
 * after a restart the pieces are still exchanged; written through a new file that replaces the old one (mu held) */
static void paid_save(home *h)
{
    char tmp[1300], a[65], b[65], r0[2 * RECORD_LEN + 1], r1[2 * RECORD_LEN + 1];
    snprintf(tmp, sizeof tmp, "%s.tmp", h->paidpath);
    (void)remove(tmp);
    const int fd = sys_create_secret(tmp);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    bool ok = f != NULL;
    for (int i = 0; ok && i < MAX_SESSIONS; i++) {
        const session_slot *s = &h->s[i];
        uint8_t m[RECORD_LEN], t[RECORD_LEN];
        if (!s->used || !s->paid.paid_at) continue;
        record_encode(&s->mine, m), record_encode(&s->theirs, t);
        btc_hex(s->eid, 32, a), btc_hex(s->paid.first, 32, b), btc_hex(m, RECORD_LEN, r0), btc_hex(t, RECORD_LEN, r1);
        ok = fprintf(f, "%s %s %llu %s %s\n", a, b, (unsigned long long)s->paid.paid_at, r0, r1) > 0;
    }
    ok = ok && fflush(f) == 0 && sys_sync(fileno(f));
    if (f) ok = fclose(f) == 0 && ok;
    else if (fd >= 0) close(fd);
    if (!ok || !sys_replace(tmp, h->paidpath)) remove(tmp), say(h, "cannot write %s: after a restart the pieces would not be exchanged", h->paidpath);
}

/* The pairs of HOME/paid back in their slots: they try the exchange of the pieces again soon, unless the key is in FOUND.txt already (then
 * they only answer the partner, which may still lack this home's piece). */
static void paid_load(home *h)
{
    FILE *f = fopen(h->paidpath, "r");
    char a[80], b[80], r0[2 * RECORD_LEN + 8], r1[2 * RECORD_LEN + 8];
    unsigned long long at;
    const bool known = access(h->foundpath, F_OK) == 0;
    int n = 0;
    while (f && n < MAX_SESSIONS && fscanf(f, "%79s %79s %llu %283s %283s", a, b, &at, r0, r1) == 5) {
        uint8_t m[RECORD_LEN], t[RECORD_LEN], eid[32];
        record pair[2];
        session_slot *s = &h->s[n];
        if (strlen(a) != 64 || strlen(b) != 64 || strlen(r0) != 2 * RECORD_LEN || strlen(r1) != 2 * RECORD_LEN || !at ||
            !btc_unhex(a, 64, eid) || !btc_unhex(b, 64, s->paid.first) || !btc_unhex(r0, 2 * RECORD_LEN, m) || !btc_unhex(r1, 2 * RECORD_LEN, t) ||
            !record_decode(m, &s->mine) || !record_decode(t, &s->theirs) || !pieces_order(&s->mine, &s->theirs, pair))
            continue;
        pieces_eid(&h->net, pair, s->eid);
        if (memcmp(s->eid, eid, 32) || memcmp(s->mine.owner, h->id.xonly, 32)) continue;   /* of another network, or another home */
        memcpy(s->partner, s->theirs.owner, 32);
        s->used = s->alerted = s->proved = true;
        s->paid.paid_at = at, s->paid.have_key = known;
        s->retry_at = known ? 0 : mono_s() + 60;               /* once dp.bin is read back: the distance of the own point comes from it */
        n++;
    }
    if (f) fclose(f);
    h->prize_paid = n > 0;
}

/* The key of the puzzle, from the pieces: into HOME/FOUND.txt and on the terminal, encrypted when the campaign of pzsolve encrypts its key
 * (--encrypt-pubkey or --encrypt-password given to pzsolve after --: the public key is in its campaign.conf). False when it could not be
 * encrypted: then it is shown nowhere, and the pieces are exchanged again later. */
static bool found_key(home *h, const uint8_t key[32])
{
    char conf[1300], hex[65], wif[PZ_WIF_BUF] = "", addr[64] = "", pub[67], sealed[PZ_SEAL_TEXT_BUF] = "", id[9] = "";
    char buf[2048], tmp[1300], date[40];
    snprintf(conf, sizeof conf, "%s/campaigns/net-%s/campaign.conf", h->dir, h->nf.net);
    FILE *f = fopen(conf, "r");
    pz_ge q;
    bool enc = false;
    if (f) {
        char line[300];
        uint8_t q33[33];
        while (!enc && fgets(line, sizeof line, f))
            enc = !strncmp(line, "encrypt_pubkey=", 15) && btc_unhex(line + 15, 66, q33) && pz_ge_from_pubkey(&q, q33, 33);
        fclose(f);
    }
    uint8_t h160[20], kid[4];
    pz_hash160(h->nf.pubkey33, 33, h160);
    if (h->chain == BTC_MAIN) pz_hash160_to_address(h160, addr, sizeof addr);
    btc_hex(h->nf.pubkey33, 33, pub);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
    int n;
    if (enc) {
        uint8_t pay[PZ_SEAL_PAYLOAD_LEN], rb[32];
        pz_u256 r;
        bool ok = false;
        while (!ok && sys_random(rb, 32)) {                     /* a scalar in [1, n - 1], drawn again otherwise */
            pz_u256_from_be(&r, rb);
            ok = !pz_u256_is_zero(&r) && pz_u256_cmp(&r, &pz_secp256k1_n) < 0;
        }
        ok = ok && pz_seal(pay, &q, key, &r) == PZ_SEAL_OK && pz_seal_encode(pay, sealed, sizeof sealed) > 0;
        pz_secure_zero(rb, sizeof rb), pz_secure_zero(&r, sizeof r);
        if (!ok) return say(h, "the key of the puzzle is known but could NOT be encrypted (no random numbers): the exchange is tried again"), false;
        pz_seal_key_id(&q, kid), btc_hex(kid, 4, id);
        n = snprintf(buf, sizeof buf, "*** PRIVATE KEY FOUND (ENCRYPTED) ***\n\ndate:            %s\nnetwork:         %s\npuzzle:          %d\n%s%s%s"
                     "version:         %s\nencrypted to:    key id %s\n\nThe private key opens with the secret of that public key alone. At home:\n"
                     "    pzsolve decrypt-key FOUND.txt\n\n%s\n", date, h->nf.net, h->nf.puzzle, addr[0] ? "address:         " : "", addr,
                     addr[0] ? "\n" : "", PZ_VERSION, id, sealed);
    } else {
        btc_hex(key, 32, hex);
        if (h->chain == BTC_MAIN) pz_privkey_to_wif(key, true, wif, sizeof wif);
        n = snprintf(buf, sizeof buf, "*** PRIVATE KEY FOUND ***\n\ndate:            %s\nnetwork:         %s\npuzzle:          %d\n%s%s%s"
                     "private key hex: 0x%s\n%s%s%spublic key:      %s\nversion:         %s\n\n"
                     "The prize was paid half each to the two payout addresses; the partner knows this key too. It also opens the coins of\n"
                     "this address on the chains that split from Bitcoin (Bitcoin Cash and the others), for whoever moves them first.\n",
                     date, h->nf.net, h->nf.puzzle, addr[0] ? "address:         " : "", addr, addr[0] ? "\n" : "", hex,
                     wif[0] ? "WIF compressed:  " : "", wif, wif[0] ? "\n" : "", pub, PZ_VERSION);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", h->foundpath);
    (void)remove(tmp);
    const int fd = sys_create_secret(tmp);
    bool saved = fd >= 0 && write(fd, buf, (size_t)n) == n && sys_sync(fd);
    if (fd >= 0) saved = close(fd) == 0 && saved;
    saved = saved && sys_replace(tmp, h->foundpath);
    if (!saved) remove(tmp);
    pz_secure_zero(buf, sizeof buf);
    if (enc) say(h, "the key of the puzzle, encrypted to key id %s%s%s: %s (pzsolve decrypt-key opens it)", id, saved ? ", saved in " : " (NOT SAVED)",
                 saved ? h->foundpath : "", sealed);
    else {
        say_secret("THE KEY OF THE PUZZLE: 0x%s%s%s", hex, wif[0] ? "  WIF " : "", wif);
        say(h, saved ? "the key is saved in %s; it also opens the coins of the address on Bitcoin Cash and the other forks, the partner's too"
                     : "the key could NOT be written to %s: copy it from this screen", h->foundpath);
    }
    pz_secure_zero(hex, sizeof hex), pz_secure_zero(wif, sizeof wif);
    return true;
}

/* ---- pzsolve ---- */

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
    snprintf(dp, sizeof dp, "%u", h->nf.dpbits);
    snprintf(name, sizeof name, "net-%s", h->nf.net);
    const char *argv[64] = {h->cfg->pzsolve, "hunt", "-r", range, "-k", pub, "--dp-bits", dp, "--jump-seed", js, "--start-key", key,
                            "-c", name, "-d", camp, "--ui", "log"};
    int n = 18;
    for (int i = 0; i < h->cfg->nargs && n < 62; i++) argv[n++] = h->cfg->pzsolve_args[i];
    argv[n] = NULL;
    snprintf(h->logpath, sizeof h->logpath, "%s/pzsolve.out", h->dir);
    if (!(h->pzsolve = sys_spawn(argv, h->logpath, NULL, NULL))) return snprintf(err, errlen, "cannot start %s", h->cfg->pzsolve), false;
    say(h, "pzsolve started, its output in %s", h->logpath);
    return true;
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

/* the notes of a session, with its tag */
typedef struct { const home *h; const char *tag; } sayer;
static void session_say(void *user, const char *line) { const sayer *w = user; say(w->h, "session %s: %s", w->tag, line); }

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
    btc_hex(s->eid, 8, tag);
    const int me = memcmp(s->mine.owner, s->theirs.owner, 32) < 0 ? 0 : 1;
    say(h, "session %s: a coincidence with %02x%02x..., party %d", tag, s->theirs.owner[0], s->theirs.owner[1], me);
    session_result res = SESSION_FAILED;
    bool proved = false;
    sayer sy = {h, tag};
    pthread_mutex_lock(&h->mu);
    session_paid paid = s->paid;                                /* the session writes its copy; the slot gets it back at the end */
    pthread_mutex_unlock(&h->mu);
    const uint64_t paid_before = paid.paid_at;
    const bool had_key = paid.have_key;                         /* a session that gives the partner our piece again shows nothing new */
    /* the chain and the own terms first: once the channel is open the partner's hello comes within seconds (and is waited for no
     * longer), and a chain without a node takes longer than that to answer */
    btc_rpc rpc = {0};
    chain_rpc crpc = {.rpc = &rpc, .chain = h->chain, .conf_target = 2, .fallback_feerate = 2000};
    session_cfg sc = {0};
    net_conn *c = NULL;
    if (cfg->rpc && !btc_rpc_init_arg(&rpc, cfg->rpc, cfg->rpc_cookie)) {
        snprintf(why, sizeof why, "--rpc: %s", rpc.err.msg);
        goto done;
    }
    /* a chain that is behind would report coins that are not those of the chain */
    if ((cfg->rpc ? chain_rpc_ready(&crpc, why, sizeof why) : chain_spv_ready(&h->spv, why, sizeof why)) != 1) goto done;
    const session_chain chain = cfg->rpc ? chain_rpc_iface(&crpc) : chain_spv_iface(&h->spv);
    sc = (session_cfg){.ctx = h->ctx, .net = &h->net, .identity = &h->id.kp, .mine = s->mine, .theirs = s->theirs, .pay_len = h->pay_len,
                       .min_feerate = 20000, .max_feerate = 1000000,   /* 20 to 1000 satoshis a virtual byte */
                       .feerate = chain.feerate(chain.ctx), .pzsign = cfg->pzsign, .limit = &h->limit, .proved = &proved,
                       .chain = &chain, .timeout_ms = 300000, .cancel_blocks = 3, .paid = &paid, .say = session_say, .user = &sy,
                       /* how often the chain is asked: public servers are not to be hammered */
                       .poll_ms = h->chain == BTC_REGTEST ? 1000 : cfg->rpc ? 2000 : 15000};
    memcpy(sc.base, h->base, 32), memcpy(sc.pubkey33, h->nf.pubkey33, 33), memcpy(sc.partner, s->theirs.owner, 32);
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
    if (cfg->rpc) btc_rpc_done(&rpc);
    say(h, "session %s: %s (%s)", tag, res == SESSION_PAID ? "PAID" : res == SESSION_CANCELLED ? "cancelled" : "failed", why);
    if (paid.have_key && !had_key) paid.have_key = found_key(h, paid.key);
    pz_secure_zero(paid.key, sizeof paid.key);
    pthread_mutex_lock(&h->mu);
    s->paid = paid;
    if (paid.paid_at && !paid_before) paid_save(h), h->prize_paid = true;
    if (c) net_close(c);
    if (s->have_conn && s->conn != c) net_close(s->conn);      /* the partner's connection came too, after another one */
    s->running = false, s->waiting = false, s->have_conn = false, s->conn = NULL;
    s->proved |= proved, s->alerted |= proved;                  /* an alert that comes after it starts no second session */
    /* again in 10 minutes if it did not pay, or paid without the pieces */
    s->retry_at = s->alerted && (res != SESSION_PAID || !paid.have_key) && ++s->tries < RETRIES ? mono_s() + RETRY_S : 0;
    /* a pair whose partner never proved anything gives its slot back, and so does one out of tries (an alert that comes again starts it
     * afresh, within the limits of session.h); a paid pair keeps its slot, so that it is not started again */
    s->used = s->alerted && (res == SESSION_PAID || s->retry_at);
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
            memcpy(s->eid, eid, 32), memcpy(s->partner, theirs->owner, 32);
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
    if (merge) return say(h, "a merge with another home on the same type: no key from it");   /* the walkers go on: a restart is to come */
    bool fresh;
    pthread_mutex_lock(&h->mu);
    session_slot *s = slot_of(h, mine, theirs, true, &fresh);
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
    if (!s) {                                                   /* no alert yet: room for it, one per partner */
        int open = 0;
        bool busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            const session_slot *o = &h->s[i];
            if (o->used && !o->alerted) open++, busy |= !memcmp(o->partner, a.owner, 32);
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

static const char *span(double s, char *out, size_t cap)
{
    if (!isfinite(s) || s < 0) snprintf(out, cap, "unknown");
    else if (s < 3600) snprintf(out, cap, "%.0f minutes", s / 60);
    else if (s < 2 * 86400) snprintf(out, cap, "%.1f hours", s / 3600);
    else if (s < 2 * 365.25 * 86400) snprintf(out, cap, "%.0f days", s / 86400);
    else snprintf(out, cap, "%.3g years", s / (365.25 * 86400));
    return out;
}

/* The work of the whole network, from what this home holds as a custodian: a record goes to the DHT_KSTORE nodes closest to its key, so
 * the network holds about held * nodes / DHT_KSTORE of them, each worth 2^dpbits jumps on average; its rate is the growth of held over the
 * last hour. With the kangaroo the key comes after n jumps with P = 1 - exp(-(n / c)^2), c = C_MEAN / 0.8862 * sqrt(W) (docs/en/RESEARCH.md
 * 4.5): given that it has not come after `done`, half the chance is within sqrt(done^2 + c^2 ln 2) - done more jumps, 9 in 10 within
 * sqrt(done^2 + c^2 ln 10) - done. The estimate of the nodes is a fifth off at each lookup: the last six are averaged. */
static void network_status(home *h, int contacts, uint64_t own)
{
    const uint64_t held = dht_local_count(h->d);
    const int64_t now = mono_s();
    if (h->nsmp == 7) memmove(h->smp, h->smp + 1, 6 * sizeof h->smp[0]), h->nsmp = 6;
    h->smp[h->nsmp].t = now, h->smp[h->nsmp].held = held, h->smp[h->nsmp].own = own, h->nsmp++;
    const double nodes_now = h->cfg->listen_port ? dht_nodes(h->d) : 0;
    if (nodes_now > 0) {
        if (h->nnodes == 6) memmove(h->nodes, h->nodes + 1, 5 * sizeof h->nodes[0]), h->nnodes = 5;
        h->nodes[h->nnodes++] = nodes_now;
    }
    double nodes = 0;
    for (int i = 0; i < h->nnodes; i++) nodes += h->nodes[i] / h->nnodes;
    const double dt = (double)(now - h->smp[0].t), per = ldexp(1, (int)h->nf.dpbits);
    const double share = nodes / (nodes < DHT_KSTORE ? (nodes > 1 ? nodes : 1) : DHT_KSTORE);   /* records in the network per record held */
    if (!h->cfg->listen_port || !held || h->nsmp < 2 || dt <= 0 || nodes <= 0)
        return say(h, "network: %d contacts; %llu points of this home, %llu records held here as a custodian%s", contacts, (unsigned long long)own,
                   (unsigned long long)held, h->cfg->listen_port ? "; the time it needs: in 10 minutes" : "");
    const double grew = held > h->smp[0].held ? (double)(held - h->smp[0].held) : 0;   /* a restart of the store can only lower it */
    const double rate = grew / dt * share * per, mine = (double)(own - h->smp[0].own) / dt * per;
    const long double W = pz_u256_to_ld(&h->nf.end) - pz_u256_to_ld(&h->nf.start) + 1, c = C_MEAN / 0.8862269254527580L * sqrtl(W);
    const long double done = (long double)held * share * per;
    char a[40], b[40];
    const double t50 = rate > 0 ? (double)((sqrtl(done * done + c * c * logl(2)) - done) / rate) : INFINITY;
    const double t90 = rate > 0 ? (double)((sqrtl(done * done + c * c * logl(10)) - done) / rate) : INFINITY;
    say(h, "network: about %.0f nodes, %.3g points in all, 2^%.1f jumps a second (this home %.2g%%); half the chance within %s, 9 in 10 within %s",
        nodes, (double)(held * share), rate > 0 ? log2(rate) : 0.0, rate > 0 ? 100 * mine / rate : 0.0, span(t50, a, sizeof a), span(t90, b, sizeof b));
}

/* ---- the run ---- */

/* The chain without a node: the Electrum servers of the network file and of the command line, the headers from the checkpoint of the file
 * (checkpoint=HEIGHT:HASH, the hash as explorers show it; a chain other than the main one may start from its first block). */
static bool light_chain(home *h, const home_cfg *cfg, char *err, size_t cap)
{
    static char servers[CHAIN_SPV_SERVERS][256];
    const char *list[CHAIN_SPV_SERVERS];
    uint8_t cp[32];
    unsigned long long cph = 0;
    bool has_cp = false;
    int n = 0;
    for (int i = 0; i < cfg->nelectrum && n < CHAIN_SPV_SERVERS; i++) list[n++] = cfg->electrum[i];
    for (const char *p = h->nf.opts; *p;) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        char hex[65];
        if (l > 9 && l - 9 < sizeof servers[0] && !strncmp(p, "electrum=", 9) && n < CHAIN_SPV_SERVERS)
            memcpy(servers[n], p + 9, l - 9), servers[n][l - 9] = 0, list[n] = servers[n], n++;
        else if (!strncmp(p, "checkpoint=", 11) && sscanf(p, "checkpoint=%llu:%64[0-9a-fA-F]", &cph, hex) == 2 && btc_hash_from_hex(hex, cp))
            has_cp = true;                                      /* of several, the last */
        p += l + (e != NULL);
    }
    if (h->chain == BTC_MAIN) {                                 /* what the file and the command line leave out */
        uint64_t dh;
        uint8_t dcp[32];
        const int dn = chain_spv_defaults(n ? NULL : list, &dh, dcp);
        if (!n) n = dn;
        if (!has_cp) has_cp = true, cph = dh, memcpy(cp, dcp, 32);
    }
    if (!n) return snprintf(err, cap, "no chain: give --rpc (a bitcoind of your own) or --electrum HOST:PORT (public servers, checked)"), false;
    snprintf(h->hdrpath, sizeof h->hdrpath, "%s/headers.bin", h->dir);
    if (!chain_spv_init(&h->spv, h->chain, has_cp ? cph : 0, has_cp ? cp : NULL, list, n, h->hdrpath))
        return snprintf(err, cap, "the checkpoint of the network file is not a block of the chain to start from"), false;
    h->spv.fallback_feerate = 2000;
    if (h->chain != BTC_REGTEST) h->spv.max_tip_age = 6 * 3600;   /* six hours without a block: the servers are behind, or lie */
    const int ready = chain_spv_ready(&h->spv, err, cap);
    say(h, "chain: %d Electrum server%s, %s%s", n, n == 1 ? "" : "s", err, ready ? "" : " (a session waits for it)");
    return true;
}

int home_run(const home_cfg *cfg)
{
    static home hs;
    home *h = &hs;
    char err[400];
    memset(h, 0, sizeof *h);
    h->cfg = cfg;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->ctx = keys_ctx();
    if (!netfile_load(cfg->network, &h->nf, err, sizeof err)) return fprintf(stderr, "%s (%s)\n", err, cfg->network), 1;
    snprintf(h->dirname, sizeof h->dirname, "pznet-%.31s", h->nf.net);
    h->dir = cfg->dir ? cfg->dir : h->dirname;
    /* the folder holds the seed, the distances of every point (dp.bin of the campaign, made 0600 by pzsolve) and the output of pzsolve,
     * which shows a key it finds: nobody else may go in */
    if (!sys_mkdir(h->dir) || !sys_private(h->dir))
        return fprintf(stderr, "%s must be a folder of this user that nobody else can open (chmod 700 it)\n", h->dir), 1;
    snprintf(h->limitpath, sizeof h->limitpath, "%s/failed-signings", h->dir);
    snprintf(h->paidpath, sizeof h->paidpath, "%s/paid", h->dir);
    snprintf(h->foundpath, sizeof h->foundpath, "%s/FOUND.txt", h->dir);
    snprintf(h->dpbin, sizeof h->dpbin, "%.1100s/campaigns/net-%.31s/dp.bin", h->dir, h->nf.net);
    pthread_mutex_init(&h->limit.mu, NULL);
    pthread_cond_init(&h->limit.cv, NULL);
    h->limit.path = h->limitpath, h->limit.max = FAILS_MAX, h->limit.pair_max = FAILS_PAIR_MAX, h->limit.pay_max = PAYOUTS_MAX;
    pz_u256_to_be(&h->nf.start, h->base);
    if (!record_net_init(h->ctx, &h->net, h->nf.fp, h->nf.dpbits, h->nf.pubkey33, h->base)) return fprintf(stderr, "invalid network\n"), 1;
    if (!btc_chain_parse(h->nf.chain, &h->chain)) return fprintf(stderr, "unknown chain %s\n", h->nf.chain), 1;
    h->pay_len = cfg->payout ? btc_address_script(h->chain, cfg->payout, h->pay_spk) : 0;
    if (!h->pay_len) return fprintf(stderr, "--payout: not an address of the %s chain\n", h->nf.chain), 1;
    if (cfg->rpc) {                                             /* the node: a wrong --rpc or chain now, not at the first session */
        btc_rpc rpc;
        chain_rpc crpc = {.rpc = &rpc, .chain = h->chain};
        int ready = 0;
        if (btc_rpc_init_arg(&rpc, cfg->rpc, cfg->rpc_cookie)) ready = chain_rpc_ready(&crpc, err, sizeof err);
        else if (rpc.err.kind == BTC_RPC_EPROTO) return fprintf(stderr, "--rpc: %s\n", rpc.err.msg), 1;
        else snprintf(err, sizeof err, "%s", rpc.err.msg);      /* no cookie yet: the node is not running */
        btc_rpc_done(&rpc);
        if (ready < 0) return fprintf(stderr, "bitcoind: %s\n", err), 1;
        say(h, "bitcoind: %s%s", err, ready ? "" : " (a session waits for it)");
    } else if (!light_chain(h, cfg, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!home_seed(h, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!keys_identity(h->ctx, h->seed, h->nf.fp, cfg->id_bits, &h->id)) return fprintf(stderr, "no identity\n"), 1;
    paid_load(h);
    /* the table: the entry lines of the file and the peers given */
    const char *boot[64];
    char entries[16][256];
    int nb = 0;
    for (const char *p = h->nf.opts; *p && nb < 16;) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 6 && l - 6 < sizeof entries[0] && !strncmp(p, "entry=", 6)) memcpy(entries[nb], p + 6, l - 6), entries[nb][l - 6] = 0, boot[nb] = entries[nb], nb++;
        p += l + (e != NULL);
    }
    for (int i = 0; i < cfg->npeers && nb < 64; i++) boot[nb++] = cfg->peers[i];
    /* the prizes, prize=txid:vout:sats (the txid as explorers show it; the value is the node's): a session finds them even when the
     * scan of the puzzle address fails (session.c) */
    for (const char *p = h->nf.opts; *p;) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        char tx[65];
        unsigned vout;
        unsigned long long sats;
        if (!strncmp(p, "prize=", 6) && h->nprizes < SESSION_PRIZES_MAX && sscanf(p, "prize=%64[0-9a-fA-F]:%u:%llu", tx, &vout, &sats) == 3 &&
            btc_hash_from_hex(tx, h->prizes[h->nprizes].txid))
            h->prizes[h->nprizes++].vout = vout;
        p += l + (e != NULL);
    }
    char nodedir[1100];
    snprintf(nodedir, sizeof nodedir, "%s/node", h->dir);
    dht_config dc = {.ctx = h->ctx, .net = &h->net, .self = &h->id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                     .listen_port = cfg->listen_port, .dir = nodedir, .bootstrap = boot, .nbootstrap = nb, .on_alert = on_alert,
                     .own_distance = own_distance, .on_session = on_session, .user = h, .verbose = cfg->verbose, .portmap = cfg->portmap,
                     .allow_private = h->chain != BTC_MAIN || cfg->allow_private,
                     .store_max = (uint64_t)cfg->store_gb * 1000000000ull / RECORD_LEN};
    dc.bt_routers = bt_routers(h->chain == BTC_MAIN, cfg->no_bt, cfg->bt_routers, cfg->nbt_routers, &dc.nbt_routers);
    if (!(h->d = dht_create(&dc, err, sizeof err))) return fprintf(stderr, "%s\n", err), 1;
    sys_on_stop(on_signal);
    const int nc = dht_start(h->d);
    char idhex[17];
    btc_hex(h->id.xonly, 8, idhex);
    say(h, "home %s... on network %s: %d contacts, %s", idhex, h->nf.net, nc, cfg->listen_port ? "listening" : "not listening");
    if (h->prize_paid) say(h, "the prize of this network is paid: pzsolve is not started%s", access(h->foundpath, F_OK) ? "; the pieces of the key are still to be exchanged" : ", and the key is in FOUND.txt");
    int rc = nc < 0 || (!h->prize_paid && !start_pzsolve(h, err, sizeof err));
    if (rc) fprintf(stderr, "%s\n", nc < 0 ? "cannot listen" : err);
    /* the feed: the new points of dp.bin to the table, every half second */
    dpfeed df = {0};
    uint64_t published = 0, found = 0;
    bool solved_alone = false, met = nc > 0;
    int64_t next_status = mono_s() + STATUS_S;
    while (!rc && !g_stop) {
        sys_sleep_ms(500);
        const int contacts = dht_count(h->d);                  /* what the network is to this home: at the first contact, and every 10 minutes */
        if ((contacts > 0) != met || mono_s() >= next_status) {
            if (contacts) network_status(h, contacts, found);
            else say(h, "network: no contact, looking for the others; %llu points of this home wait for them", (unsigned long long)found);
            met = contacts > 0, next_status = mono_s() + STATUS_S;
        }
        if (!df.f && dpfeed_open(&df, h->dpbin)) {
            pthread_mutex_lock(&h->mu);
            h->dpr = sys_fopen(h->dpbin, "rb");                 /* the distances of the home: not for pzsign */
            pthread_mutex_unlock(&h->mu);
        }
        own_point p[DHT_STORE_MAX];
        record recs[DHT_STORE_MAX];
        for (int n; df.f && (n = dpfeed_read(h->ctx, &h->net, &df, p, DHT_STORE_MAX)) > 0;) {
            pthread_mutex_lock(&h->mu);
            for (int i = 0; i < n; i++) {
                own_add(&h->own, p[i].x, p[i].index);
                memset(&recs[i], 0, sizeof recs[i]);
                recs[i].t = p[i].t;
                memcpy(recs[i].x, p[i].x, 32);
            }
            pthread_mutex_unlock(&h->mu);
            pz_secure_zero(p, sizeof p);
            found += (uint64_t)n;
            published += (uint64_t)dht_publish(h->d, recs, n);
        }
        int st;
        pthread_mutex_lock(&h->mu);
        const bool paid_now = h->prize_paid;
        pthread_mutex_unlock(&h->mu);
        if (h->pzsolve && paid_now) {                           /* nothing is left to search: the home stays on for the pieces */
            say(h, "the prize is paid: pzsolve stops");
            sys_proc_interrupt(h->pzsolve), (void)sys_proc_ended(h->pzsolve, true, &st);
            h->pzsolve = 0;
        }
        if (h->pzsolve && sys_proc_ended(h->pzsolve, false, &st)) {
            solved_alone = st == 0;                             /* pzsolve ends with 0 only when it found the key itself */
            say(h, "pzsolve ended (%s); %llu points published", solved_alone ? "it found the key on its own" : "status not 0",
                (unsigned long long)published);
            h->pzsolve = 0;
            if (cfg->exit_when_done) break;
        }
        pthread_mutex_lock(&h->mu);
        const bool done = cfg->exit_when_done && h->ended > 0;
        bool busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_slot *s = &h->s[i];
            if (s->used && !s->running && s->retry_at && mono_s() >= s->retry_at) s->retry_at = 0, start_session(h, s);   /* again */
            busy |= s->running || (s->paid.paid_at && !s->paid.have_key && s->retry_at);   /* a pair paid without the pieces tries again */
        }
        pthread_mutex_unlock(&h->mu);
        if (done && !busy) break;
    }
    int st;
    if (h->pzsolve) sys_proc_interrupt(h->pzsolve), (void)sys_proc_ended(h->pzsolve, true, &st);   /* it saves its state and ends */
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
    say(h, "home stopped: %llu points published, %s", (unsigned long long)published, paid ? "a session paid" : solved_alone ? "pzsolve found the key" : "no payout");
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
