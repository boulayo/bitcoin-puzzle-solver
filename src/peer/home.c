/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* home.c - the daemon of a home (see home.h). One thread feeds the table from dp.bin; the node answers the others; every useful coincidence
 * gets a thread of its own that runs the session. The channel of a pair comes from dht_channel (docs/en/NETWORK.md 7.7): dialled by party 0
 * (the smaller identity) when it finds the partner or by a side that does not listen, the partner's own connection, or a relay.
 *
 * A hello can start a session before this home has the alert of its pair, and anyone can copy the x of a published record into one of
 * its own: such a session costs little until the partner proved its distance (session.c makes the primes of pzsign only after that), at
 * most HELLO_SESSIONS of them run at a time, one per partner, and a slot whose partner proved nothing is given back: they never take the
 * slots of the pairs the table alerted. */
#include "home.h"
#include "chain_rpc.h"
#include "bt.h"
#include "chain_spv.h"
#include "dht.h"
#include "dpfeed.h"
#include "keys.h"
#include "session.h"
#include "netfile.h"
#include <pzcore/seal.h>
#include <pzcore/seed.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_SESSIONS 64
#define HELLO_SESSIONS 16          /* sessions started by a hello alone, without an alert of the table (the others always have room) */
#define FAILS_MAX 10               /* failed signings of a home on a network, with any partner (project 4.6) */
#define FAILS_PAIR_MAX 3           /* and with one pair */
#define RETRY_S 600                /* a kept pair whose session did not pay starts again after this, */
#define RETRIES 144                /* for about a day (the signings that fail are counted, session.h) */

/* the own points: x prefix -> index in dp.bin (the distance is read back from the file when it is needed: 16 bytes a point in memory) */
typedef struct {
    uint64_t *key, *idx;
    size_t cap, n;
} own_index;

typedef struct session_slot {
    bool used, running, have_conn;
    bool alerted;                   /* the table alerted the pair, or its partner proved its distance in a session: the slot is kept */
    bool waiting;                   /* its session waits for the partner to dial */
    int64_t retry_at;               /* when its session starts again (0: never), the seconds of mono_s */
    int tries;
    uint8_t eid[32], partner[32];
    record mine, theirs;
    net_conn *conn;                 /* an incoming channel handed by on_session */
    uint8_t first[SESSION_MSG_MAX];
    size_t first_len;
} session_slot;

typedef struct {
    const home_cfg *cfg;
    secp256k1_context *ctx;
    netfile nf;
    record_net net;
    uint8_t base[32], seed[32], pay_spk[BTC_SPK_MAX];
    size_t pay_len;
    btc_chain chain;
    chain_spv spv;                  /* the chain without a node of one's own (no --rpc); its headers are shared by the sessions */
    char hdrpath[4200];
    home_identity id;
    dht *d;
    char dpbin[1200], logpath[1200];
    FILE *dpr;                      /* dp.bin for the reads back */
    own_index own;
    pid_t pzsolve;
    pthread_mutex_t mu;             /* own, dpr, the sessions */
    pthread_cond_t cv;
    session_slot s[MAX_SESSIONS];
    int ended;                      /* sessions that ended, and how: for exit_when_done */
    bool paid;
    session_limit limit;            /* the failed signings, counted in the home folder */
    payout_coin prizes[SESSION_PRIZES_MAX];   /* the prize= lines of the network file */
    int nprizes;
    char limitpath[1200];
} home;

static int64_t mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec;
}

static volatile sig_atomic_t g_stop;                            /* 1: stop; 2 (a second signal): do not wait for the sessions */
static void on_signal(int sig) { (void)sig, g_stop = g_stop < 2 ? g_stop + 1 : 2; }

static void say(const home *h, const char *fmt, ...)
{
    va_list ap;
    char line[600];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(stdout, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fflush(stdout);
    (void)h;
}

/* ---- files of the home ---- */

/* the home seed: made once, from several sources (lib/pzcore/src/seed.c), and kept */
static bool home_seed(home *h, char *err, size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/home.seed", h->cfg->dir);
    const int got = keys_file_read(path, h->seed, err, errlen);
    if (got) return got > 0;
    pz_seed_report rep;
    if (pz_seed_generate(h->seed, NULL, &rep) != PZ_SEED_OK) return snprintf(err, errlen, "no home seed: %s", rep.error), false;
    if (!keys_file_write(path, h->seed)) return snprintf(err, errlen, "cannot write %.300s: %s", path, strerror(errno)), false;
    say(h, "a new home seed in %s: keep a copy of it, it is the key of every point of this home", path);
    return true;
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

/* ---- pzsolve ---- */

static bool start_pzsolve(home *h, char *err, size_t errlen)
{
    char key[1100], camp[1100];
    snprintf(key, sizeof key, "%s/start.key", h->cfg->dir);
    snprintf(camp, sizeof camp, "%s/campaigns", h->cfg->dir);
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
    snprintf(h->logpath, sizeof h->logpath, "%s/pzsolve.out", h->cfg->dir);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, h->logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    const int rc = posix_spawn(&h->pzsolve, h->cfg->pzsolve, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return snprintf(err, errlen, "cannot start %s: %s", h->cfg->pzsolve, strerror(rc)), false;
    snprintf(h->dpbin, sizeof h->dpbin, "%s/net-%s/dp.bin", camp, h->nf.net);
    say(h, "pzsolve started (pid %d), its output in %s", (int)h->pzsolve, h->logpath);
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

static void hex8(const uint8_t *p, char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
}

typedef struct { home *h; session_slot *s; } session_job, chan_wait;

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
    char tag[17], primes[1200], why[300] = "";
    hex8(s->eid, tag);
    const int me = memcmp(s->mine.owner, s->theirs.owner, 32) < 0 ? 0 : 1;
    snprintf(primes, sizeof primes, "%s/primes-%s.json", cfg->dir, tag);
    say(h, "session %s: a coincidence with %02x%02x..., party %d", tag, s->theirs.owner[0], s->theirs.owner[1], me);
    session_result res = SESSION_FAILED;
    bool proved = false;
    /* the channel (docs/en/NETWORK.md 7.7): the partner's hello if it came first (dialling as well would lose it); else dialled, the
     * partner's own connection (on_session), or through a relay */
    pthread_mutex_lock(&h->mu);
    net_conn *c = s->have_conn ? s->conn : NULL;
    s->waiting = !c;                                            /* a hello may come in only while dht_channel looks */
    pthread_mutex_unlock(&h->mu);
    chan_wait w = {h, s};
    if (!c) c = dht_channel(h->d, s->theirs.owner, s->eid, 600000, incoming, &w);
    pthread_mutex_lock(&h->mu);
    s->waiting = false;
    pthread_mutex_unlock(&h->mu);
    if (!c) {
        snprintf(why, sizeof why, "no channel with the partner");
        goto done;
    }
    {
        pthread_mutex_lock(&h->mu);
        conn_chan cc = {c, c == s->conn ? s->first : NULL, s->first_len, 0};
        pthread_mutex_unlock(&h->mu);
        session_chan chan = {&cc, cc_send, cc_recv};
        btc_rpc rpc = {0};
        chain_rpc crpc = {.rpc = &rpc, .chain = h->chain, .conf_target = 2, .fallback_feerate = 2000};   /* within 2 blocks (project 4.5) */
        if (cfg->rpc && !btc_rpc_init_arg(&rpc, cfg->rpc, cfg->rpc_cookie)) {
            snprintf(why, sizeof why, "--rpc: %s", rpc.err.msg);
            goto done;
        }
        /* a chain that is behind would report coins that are not those of the chain */
        if ((cfg->rpc ? chain_rpc_ready(&crpc, why, sizeof why) : chain_spv_ready(&h->spv, why, sizeof why)) != 1) {
            if (cfg->rpc) btc_rpc_done(&rpc);
            goto done;
        }
        const session_chain chain = cfg->rpc ? chain_rpc_iface(&crpc) : chain_spv_iface(&h->spv);
        session_cfg sc = {.ctx = h->ctx, .net = &h->net, .identity = &h->id.kp, .mine = s->mine, .theirs = s->theirs, .pay_len = h->pay_len,
                          .max_feerate = 200000, .pzsign = cfg->pzsign, .primes = primes, .limit = &h->limit, .proved = &proved,
                          .chan = &chan, .chain = &chain, .timeout_ms = 300000, .poll_ms = 1000, .cancel_blocks = 3};
        memcpy(sc.base, h->base, 32), memcpy(sc.pubkey33, h->nf.pubkey33, 33), memcpy(sc.partner, s->theirs.owner, 32);
        memcpy(sc.pay_spk, h->pay_spk, h->pay_len);
        memcpy(sc.prizes, h->prizes, sizeof sc.prizes), sc.nprizes = h->nprizes;
        const uint32_t n = (uint32_t)s->eid[0] << 24 | (uint32_t)s->eid[1] << 16 | (uint32_t)s->eid[2] << 8 | s->eid[3];
        if (!own_distance(h, s->mine.x, s->mine.t, sc.d) || !keys_coin(h->ctx, h->seed, h->nf.fp, n, sc.coin_sec))
            snprintf(why, sizeof why, "the distance of the own point is not in dp.bin");
        else res = session_run(&sc, why, sizeof why);
        pz_secure_zero(&sc, sizeof sc);
        if (cfg->rpc) btc_rpc_done(&rpc);
    }
done:
    remove(primes);                                             /* the Paillier key of this session: a secret, of no use after it */
    say(h, "session %s: %s (%s)", tag, res == SESSION_PAID ? "PAID" : res == SESSION_CANCELLED ? "cancelled" : "failed", why);
    pthread_mutex_lock(&h->mu);
    if (c) net_close(c);
    if (s->have_conn && s->conn != c) net_close(s->conn);      /* the partner's connection came too, after another one */
    s->running = false, s->have_conn = false, s->conn = NULL;
    s->alerted |= proved;                                       /* an alert that comes after it starts no second session */
    s->used = s->alerted;                                       /* a pair whose partner never proved anything gives its slot back */
    s->retry_at = s->alerted && res != SESSION_PAID && ++s->tries < RETRIES ? mono_s() + RETRY_S : 0;
    h->ended++;
    h->paid |= res == SESSION_PAID;
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

/* the slot of a pair (mu held): found, or (make) made; NULL when there is none, or no room */
static session_slot *slot_of(home *h, const record *mine, const record *theirs, bool make, bool *fresh)
{
    record pair[2];
    uint8_t eid[32];
    *fresh = false;
    if (!pieces_order(mine, theirs, pair)) return NULL;
    pieces_eid(&h->net, pair, eid);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (h->s[i].used && !memcmp(h->s[i].eid, eid, 32)) return &h->s[i];
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
    s->running = true;
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
    if (len < 1 + 2 * RECORD_LEN || msg[0] != S_HELLO || !record_decode(msg + 1, &a) || !record_decode(msg + 1 + RECORD_LEN, &b) ||
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
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (h->s[i].used && !h->s[i].alerted) open++, busy |= !memcmp(h->s[i].partner, a.owner, 32);
        if (!busy && open < HELLO_SESSIONS) s = slot_of(h, &b, &a, true, &fresh);
    }
    /* a session that runs takes the connection only while it waits for one */
    if (!s || s->have_conn || (s->running && !s->waiting) || len > sizeof s->first) {
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
    for (const char *p = h->nf.upd; *p;) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        char hex[65];
        if (l > 9 && l - 9 < sizeof servers[0] && !strncmp(p, "electrum=", 9) && n < CHAIN_SPV_SERVERS)
            memcpy(servers[n], p + 9, l - 9), servers[n][l - 9] = 0, list[n] = servers[n], n++;
        else if (!strncmp(p, "checkpoint=", 11) && sscanf(p, "checkpoint=%llu:%64[0-9a-fA-F]", &cph, hex) == 2 && btc_hash_from_hex(hex, cp))
            has_cp = true;                                      /* of several, the last */
        p += l + (e != NULL);
    }
    if (!n) return snprintf(err, cap, "no chain: give --rpc (a bitcoind of your own) or --electrum HOST:PORT (public servers, checked)"), false;
    if (!has_cp && h->chain == BTC_MAIN) return snprintf(err, cap, "the network file gives no checkpoint for the headers: use --rpc"), false;
    snprintf(h->hdrpath, sizeof h->hdrpath, "%s/headers.bin", cfg->dir);
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
    h->ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    trust_set ts;
    int valid;
    /* the folder holds the seed, the distances of every point (dp.bin of the campaign, made 0644 by pzsolve), the primes of the sessions
     * and the output of pzsolve, which shows a key it finds: nobody else may go in */
    struct stat ds;
    if ((mkdir(cfg->dir, 0700) != 0 && errno != EEXIST) || stat(cfg->dir, &ds) != 0 || !S_ISDIR(ds.st_mode) || ds.st_uid != geteuid() ||
        (ds.st_mode & 077))
        return fprintf(stderr, "%s must be a folder of this user that nobody else can open (chmod 700 it)\n", cfg->dir), 1;
    DIR *dir = opendir(cfg->dir);                               /* the primes of sessions cut by a crash: secrets of no further use */
    for (struct dirent *e; dir && (e = readdir(dir));)
        if (!strncmp(e->d_name, "primes-", 7)) {
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", cfg->dir, e->d_name);
            remove(p);
        }
    if (dir) closedir(dir);
    snprintf(h->limitpath, sizeof h->limitpath, "%s/failed-signings", cfg->dir);
    pthread_mutex_init(&h->limit.mu, NULL);
    pthread_cond_init(&h->limit.cv, NULL);
    h->limit.path = h->limitpath, h->limit.max = FAILS_MAX, h->limit.pair_max = FAILS_PAIR_MAX;
    if (!netfile_load(cfg->network, &h->nf, err, sizeof err)) return fprintf(stderr, "%s (%s)\n", err, cfg->network), 1;
    if (!trust_load(&ts, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!trust_check(h->ctx, &ts, &h->nf, &valid)) return fprintf(stderr, "%s: %d valid signatures of maintainers, %d needed\n", cfg->network, valid, ts.threshold), 1;
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
    /* the table: the entry lines of the file and the peers given */
    const char *boot[64];
    char entries[16][256];
    int nb = 0;
    for (const char *p = h->nf.upd; *p && nb < 16;) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 6 && l - 6 < sizeof entries[0] && !strncmp(p, "entry=", 6)) memcpy(entries[nb], p + 6, l - 6), entries[nb][l - 6] = 0, boot[nb] = entries[nb], nb++;
        p += l + (e != NULL);
    }
    for (int i = 0; i < cfg->npeers && nb < 64; i++) boot[nb++] = cfg->peers[i];
    /* the prizes, prize=txid:vout:sats (the txid as explorers show it; the value is the node's): a session finds them even when the
     * scan of the puzzle address fails (session.c) */
    for (const char *p = h->nf.upd; *p;) {
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
    snprintf(nodedir, sizeof nodedir, "%s/node", cfg->dir);
    mkdir(nodedir, 0700);
    dht_config dc = {.ctx = h->ctx, .net = &h->net, .self = &h->id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                     .listen_port = cfg->listen_port, .dir = nodedir, .bootstrap = boot, .nbootstrap = nb, .on_alert = on_alert,
                     .own_distance = own_distance, .on_session = on_session, .user = h, .verbose = cfg->verbose, .upnp = cfg->upnp,
                     .allow_private = h->chain != BTC_MAIN || cfg->allow_private};
    dc.bt_routers = bt_routers(h->chain == BTC_MAIN, cfg->no_bt, cfg->bt_routers, cfg->nbt_routers, &dc.nbt_routers);
    if (!(h->d = dht_create(&dc, err, sizeof err))) return fprintf(stderr, "%s\n", err), 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL), sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    const int nc = dht_start(h->d);
    char idhex[17];
    hex8(h->id.xonly, idhex);
    say(h, "home %s... on network %s: %d contacts, %s", idhex, h->nf.net, nc, cfg->listen_port ? "listening" : "not listening");
    int rc = nc < 0 || !start_pzsolve(h, err, sizeof err);
    if (rc) fprintf(stderr, "%s\n", nc < 0 ? "cannot listen" : err);
    /* the feed: the new points of dp.bin to the table, every half second */
    dpfeed df = {0};
    uint64_t published = 0;
    bool solved_alone = false;
    while (!rc && !g_stop) {
        const struct timespec ts2 = {0, 500000000};
        nanosleep(&ts2, NULL);
        if (!df.f && dpfeed_open(&df, h->dpbin)) {
            pthread_mutex_lock(&h->mu);
            h->dpr = fopen(h->dpbin, "rb");
            if (h->dpr) (void)fcntl(fileno(h->dpr), F_SETFD, FD_CLOEXEC);   /* the distances of the home: not for pzsign */
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
            published += (uint64_t)dht_publish(h->d, recs, n);
        }
        int st;
        if (h->pzsolve > 0 && waitpid(h->pzsolve, &st, WNOHANG) == h->pzsolve) {
            solved_alone = WIFEXITED(st) && WEXITSTATUS(st) == 0;   /* pzsolve ends with 0 only when it found the key itself */
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
            busy |= s->running;
        }
        pthread_mutex_unlock(&h->mu);
        if (done && !busy) break;
    }
    if (h->pzsolve > 0) kill(h->pzsolve, SIGINT), waitpid(h->pzsolve, NULL, 0);
    if (!g_stop) g_stop = 1;                                    /* a session that waits for its channel gives up now */
    bool busy = true, paid = false;
    for (int wait = 0; busy && wait < 600; wait++) {            /* sessions under way end on their own: a payout is not cut in half */
        pthread_mutex_lock(&h->mu);
        pthread_cond_broadcast(&h->cv);
        busy = false;
        for (int i = 0; i < MAX_SESSIONS; i++) busy |= h->s[i].running;
        paid = h->paid;
        pthread_mutex_unlock(&h->mu);
        if (busy && g_stop < 2) sleep(1);
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
        secp256k1_context_destroy(h->ctx);
    }
    return rc ? rc : (paid || solved_alone || !cfg->exit_when_done) ? 0 : 3;
}
