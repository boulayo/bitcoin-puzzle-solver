/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* home.c - the daemon of a home (see home.h). One thread feeds the table from dp.bin; the node answers the others; every useful coincidence
 * gets a thread of its own that runs the session. Who opens the channel of a pair: party 0 (the smaller identity) when it can find the
 * partner in the table, party 1 only when it does not listen itself (it cannot be found then); the other side waits for the connection. */
#include "home.h"
#include "chain_rpc.h"
#include "dht.h"
#include "dpfeed.h"
#include "keys.h"
#include "session.h"
#include "netfile.h"
#include <pzcore/seal.h>
#include <pzcore/seed.h>
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

/* the own points: x prefix -> index in dp.bin (the distance is read back from the file when it is needed: 16 bytes a point in memory) */
typedef struct {
    uint64_t *key, *idx;
    size_t cap, n;
} own_index;

typedef struct session_slot {
    bool used, running, have_conn;
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
} home;

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig, g_stop = 1; }

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

static bool read_hex32(const char *path, uint8_t out[32])
{
    char hex[80] = "";
    FILE *f = fopen(path, "r");
    const bool got = f && fgets(hex, sizeof hex, f);
    if (f) fclose(f);
    hex[strcspn(hex, " \t\r\n")] = 0;
    bool ok = got && strlen(hex) == 64;
    for (int i = 0; ok && i < 32; i++) {
        unsigned v;
        ok = sscanf(hex + 2 * i, "%2x", &v) == 1, out[i] = (uint8_t)v;
    }
    pz_secure_zero(hex, sizeof hex);
    return ok;
}

static bool write_hex32(const char *path, const uint8_t v[32])
{
    char hex[66];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", v[i]);
    hex[64] = '\n', hex[65] = 0;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* a secret: only this user reads it */
    const bool ok = fd >= 0 && write(fd, hex, 65) == 65;
    if (fd >= 0) close(fd);
    pz_secure_zero(hex, sizeof hex);
    return ok;
}

/* the home seed: made once, from several sources (lib/pzcore/src/seed.c), and kept */
static bool home_seed(home *h, char *err, size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/home.seed", h->cfg->dir);
    if (read_hex32(path, h->seed)) return true;
    if (access(path, F_OK) == 0) return snprintf(err, errlen, "%.300s is not a seed of 64 hex digits: it is left as it is", path), false;
    pz_seed_report rep;
    if (pz_seed_generate(h->seed, NULL, &rep) != PZ_SEED_OK) return snprintf(err, errlen, "no home seed: %s", rep.error), false;
    if (!write_hex32(path, h->seed)) return snprintf(err, errlen, "cannot write %.300s: %s", path, strerror(errno)), false;
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
    const bool wrote = write_hex32(key, sk);
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
    const uint8_t *first;           /* replayed by the first recv (a session that was dialled starts with the partner's hello) */
    size_t first_len;
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
    net_set_timeout(cc->c, timeout_ms);
    return net_recv(cc->c, m, cap, len);
}

static bool run_prog(const char *path, const char *a1, const char *a2)
{
    pid_t pid;
    char *argv[] = {(char *)path, (char *)a1, (char *)a2, NULL};
    int st;
    return posix_spawn(&pid, path, NULL, NULL, argv, environ) == 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void hex8(const uint8_t *p, char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
}

typedef struct { home *h; session_slot *s; } session_job;

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
    say(h, "session %s: a coincidence with %02x%02x..., party %d; making the primes of the signer", tag, s->theirs.owner[0], s->theirs.owner[1], me);
    session_result res = SESSION_FAILED;
    net_conn *c = NULL;
    if (!run_prog(cfg->pzfirma, "primes", primes)) {
        snprintf(why, sizeof why, "pzfirma primes failed");
        goto done;
    }
    /* the channel: dialled by party 0 when it finds the partner, by party 1 when it does not listen; else it comes to us */
    dht_contact pc;
    const bool dial = (me == 0 || cfg->listen_port == 0) && dht_find(h->d, s->theirs.owner, &pc);
    if (dial) c = dht_dial(h->d, &pc);
    if (!c) {                                                   /* not ours to dial, or the partner did not answer: it dials us */
        pthread_mutex_lock(&h->mu);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 600;
        while (!s->have_conn && !g_stop && pthread_cond_timedwait(&h->cv, &h->mu, &until) == 0) {}
        c = s->conn;
        pthread_mutex_unlock(&h->mu);
    }
    if (!c) {
        snprintf(why, sizeof why, "no channel with the partner");
        goto done;
    }
    {
        conn_chan cc = {c, c == s->conn ? s->first : NULL, s->first_len};
        session_chan chan = {&cc, cc_send, cc_recv};
        btc_rpc rpc;
        char host[256] = "", user[128] = "", pass[256] = "";
        unsigned port = 0;
        if (sscanf(cfg->rpc, "%255[^:]:%u:%127[^:]:%255s", host, &port, user, pass) != 4 || !btc_rpc_init(&rpc, host, port, user, pass)) {
            snprintf(why, sizeof why, "invalid --rpc");
            goto done;
        }
        chain_rpc crpc = {.rpc = &rpc, .chain = h->chain, .conf_target = 6, .fallback_feerate = 2000};
        const session_chain chain = chain_rpc_iface(&crpc);
        session_cfg sc = {.ctx = h->ctx, .net = &h->net, .identity = &h->id.kp, .mine = s->mine, .theirs = s->theirs, .pay_len = h->pay_len,
                          .max_feerate = 200000, .pzfirma = cfg->pzfirma, .primes = primes, .chan = &chan, .chain = &chain,
                          .timeout_ms = 300000, .poll_ms = 1000, .cancel_blocks = 3};
        memcpy(sc.base, h->base, 32), memcpy(sc.pubkey33, h->nf.pubkey33, 33), memcpy(sc.partner, s->theirs.owner, 32);
        memcpy(sc.pay_spk, h->pay_spk, h->pay_len);
        const uint32_t n = (uint32_t)s->eid[0] << 24 | (uint32_t)s->eid[1] << 16 | (uint32_t)s->eid[2] << 8 | s->eid[3];
        if (!own_distance(h, s->mine.x, s->mine.t, sc.d) || !keys_coin(h->ctx, h->seed, h->nf.fp, n, sc.coin_sec))
            snprintf(why, sizeof why, "the distance of the own point is not in dp.bin");
        else res = session_run(&sc, why, sizeof why);
        pz_secure_zero(&sc, sizeof sc);
        btc_rpc_done(&rpc);
    }
done:
    if (c) net_close(c);
    remove(primes);
    say(h, "session %s: %s (%s)", tag, res == SESSION_PAID ? "PAID" : res == SESSION_CANCELLED ? "cancelled" : "failed", why);
    pthread_mutex_lock(&h->mu);
    s->running = false, s->have_conn = false, s->conn = NULL;
    h->ended++;
    h->paid |= res == SESSION_PAID;
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

/* the slot of a pair (mu held): found, or made; NULL when there is no room */
static session_slot *slot_of(home *h, const record *mine, const record *theirs, bool *fresh)
{
    record pair[2];
    uint8_t eid[32];
    *fresh = false;
    if (!pieces_order(mine, theirs, pair)) return NULL;
    pieces_eid(&h->net, pair, eid);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (h->s[i].used && !memcmp(h->s[i].eid, eid, 32)) return &h->s[i];
    for (int i = 0; i < MAX_SESSIONS; i++)
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
    session_slot *s = slot_of(h, mine, theirs, &fresh);
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
    session_slot *s = slot_of(h, &b, &a, &fresh);              /* mine is b: the partner's hello lists its record first */
    (void)fresh;
    if (!s || s->have_conn || len > sizeof s->first) {
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
    mkdir(cfg->dir, 0700);
    if (!netfile_load(cfg->network, &h->nf, err, sizeof err)) return fprintf(stderr, "%s (%s)\n", err, cfg->network), 1;
    if (!trust_load(&ts, err, sizeof err)) return fprintf(stderr, "%s\n", err), 1;
    if (!trust_check(h->ctx, &ts, &h->nf, &valid)) return fprintf(stderr, "%s: %d valid signatures of maintainers, %d needed\n", cfg->network, valid, ts.threshold), 1;
    pz_u256_to_be(&h->nf.start, h->base);
    if (!record_net_init(h->ctx, &h->net, h->nf.fp, h->nf.dpbits, h->nf.pubkey33, h->base)) return fprintf(stderr, "invalid network\n"), 1;
    if (!btc_chain_parse(h->nf.chain, &h->chain)) return fprintf(stderr, "unknown chain %s\n", h->nf.chain), 1;
    h->pay_len = cfg->payout ? btc_address_script(h->chain, cfg->payout, h->pay_spk) : 0;
    if (!h->pay_len) return fprintf(stderr, "--payout: not an address of the %s chain\n", h->nf.chain), 1;
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
    char nodedir[1100];
    snprintf(nodedir, sizeof nodedir, "%s/node", cfg->dir);
    mkdir(nodedir, 0700);
    const dht_config dc = {.ctx = h->ctx, .net = &h->net, .self = &h->id, .id_bits = cfg->id_bits, .listen_host = cfg->listen_host,
                           .listen_port = cfg->listen_port, .dir = nodedir, .bootstrap = boot, .nbootstrap = nb, .on_alert = on_alert,
                           .own_distance = own_distance, .on_session = on_session, .user = h, .verbose = cfg->verbose};
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
        for (int i = 0; i < MAX_SESSIONS; i++) busy |= h->s[i].running;
        pthread_mutex_unlock(&h->mu);
        if (done && !busy) break;
    }
    if (h->pzsolve > 0) kill(h->pzsolve, SIGINT), waitpid(h->pzsolve, NULL, 0);
    for (int wait = 0; wait < 600; wait++) {                   /* sessions under way end on their own: a payout is not cut in half */
        bool busy = false;
        pthread_mutex_lock(&h->mu);
        for (int i = 0; i < MAX_SESSIONS; i++) busy |= h->s[i].running;
        pthread_mutex_unlock(&h->mu);
        if (!busy || g_stop > 1) break;
        sleep(1);
    }
    dht_stop(h->d);
    dpfeed_close(&df);
    if (h->dpr) fclose(h->dpr);
    say(h, "home stopped: %llu points published, %s", (unsigned long long)published, h->paid ? "a session paid" : solved_alone ? "pzsolve found the key" : "no payout");
    pz_secure_zero(h->seed, sizeof h->seed);
    pz_secure_zero(&h->id, sizeof h->id);
    secp256k1_context_destroy(h->ctx);
    return rc ? rc : (h->paid || solved_alone || !cfg->exit_when_done) ? 0 : 3;
}
