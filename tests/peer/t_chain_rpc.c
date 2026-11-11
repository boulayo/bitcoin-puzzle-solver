/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_chain_rpc.c - the chain of a session on bitcoind (chain_rpc.c) against a fake node in this file: it listens on a port of 127.0.0.1
 * and answers every call, on as many connections as the client makes, with what the case gives for that method. The real node is in
 * t_session with PZ_REGTEST. */
#include "chain_rpc.h"
#include "peertest.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* the answer to a call: a result, or an error object (JSON texts), as bitcoind gives them to a JSON-RPC 1.0 request */
typedef struct {
    const char *result, *error;
} answer;

/* the case: the answer to the nth call (from 0) of method with params */
typedef answer (*node_fn)(const char *method, const char *params, int nth);

static struct {
    int lfd;
    unsigned port;
    pthread_t th;
    pthread_mutex_t mu;
    bool stop;
    node_fn fn;
    char calls[8192];   /* the methods called, each followed by a space */
} N = {.mu = PTHREAD_MUTEX_INITIALIZER};

static int count_of(const char *method)
{
    char w[80];
    snprintf(w, sizeof w, "%s ", method);
    int n = 0;
    for (const char *p = N.calls; (p = strstr(p, w)); p += strlen(w))
        if (p == N.calls || p[-1] == ' ') n++;
    return n;
}

static void serve_one(int fd)
{
    static char req[1 << 16];
    size_t len = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 5000) <= 0) return;
        const ssize_t k = recv(fd, req + len, sizeof req - 1 - len, 0);
        if (k <= 0) return;
        len += (size_t)k;
        req[len] = 0;
        const char *e = strstr(req, "\r\n\r\n"), *cl = strcasestr(req, "\r\nContent-Length:");
        if (e && cl && len >= (size_t)(e + 4 - req) + strtoul(cl + 17, NULL, 10)) break;
    }
    char method[64] = "", params[4096] = "";
    const char *m = strstr(req, "\"method\":\""), *pa = strstr(req, "\"params\":"), *id = strstr(req, "\"id\":");
    if (m) sscanf(m + 10, "%63[^\"]", method);
    if (pa) {
        snprintf(params, sizeof params, "%s", pa + 9);
        const size_t n = strlen(params);
        if (n && params[n - 1] == '}') params[n - 1] = 0;
    }
    pthread_mutex_lock(&N.mu);
    const int nth = count_of(method);
    if (strlen(N.calls) + strlen(method) + 2 < sizeof N.calls) strcat(strcat(N.calls, method), " ");
    const answer a = N.fn(method, params, nth);
    pthread_mutex_unlock(&N.mu);
    const size_t bl = strlen(a.result ? a.result : "null") + strlen(a.error ? a.error : "null") + 64;
    char *body = malloc(bl), head[128];
    const int n = snprintf(body, bl, "{\"result\":%s,\"error\":%s,\"id\":%llu}", a.result ? a.result : "null", a.error ? a.error : "null",
                           id ? strtoull(id + 5, NULL, 10) : 0);
    const int hl = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
                            a.error ? 500 : 200, n);
    send(fd, head, (size_t)hl, MSG_NOSIGNAL);
    for (int o = 0; o < n;) {
        const ssize_t w = send(fd, body + o, (size_t)(n - o), MSG_NOSIGNAL);
        if (w <= 0) break;
        o += (int)w;
    }
    free(body);
}

static void *node(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&N.mu);
        const bool stop = N.stop;
        pthread_mutex_unlock(&N.mu);
        if (stop) return NULL;
        struct pollfd p = {.fd = N.lfd, .events = POLLIN};
        if (poll(&p, 1, 50) <= 0) continue;
        const int fd = accept(N.lfd, NULL, NULL);
        if (fd < 0) continue;
        serve_one(fd);
        shutdown(fd, SHUT_WR);
        char tmp[64];
        struct pollfd q = {.fd = fd, .events = POLLIN};
        while (poll(&q, 1, 3000) > 0 && recv(fd, tmp, sizeof tmp, 0) > 0) {}
        close(fd);
    }
}

static btc_rpc R;
static chain_rpc CR;
static session_chain CH;

/* the node answers with fn from now on; a fresh client and chain of the regtest */
static void start(node_fn fn, btc_chain chain)
{
    pthread_mutex_lock(&N.mu);
    N.fn = fn;
    N.calls[0] = 0;
    pthread_mutex_unlock(&N.mu);
    CHECK(btc_rpc_init(&R, "127.0.0.1", N.port, "u", "p"));
    CR = (chain_rpc){.rpc = &R, .chain = chain, .fallback_feerate = 2000};
    CH = chain_rpc_iface(&CR);
}

static bool called(const char *calls)
{
    pthread_mutex_lock(&N.mu);
    const bool same = !strcmp(N.calls, calls);
    if (!same) printf("  the calls were: %s\n", N.calls);
    pthread_mutex_unlock(&N.mu);
    return same;
}

static const char *BCI_OK = "{\"chain\":\"regtest\",\"blocks\":870000,\"headers\":870000,\"initialblockdownload\":false,\"pruned\":true}";

/* ---- whether the node can serve a session ---- */

static const char *bci;

static answer ans_bci(const char *method, const char *params, int nth)
{
    (void)params, (void)nth;
    if (!strcmp(method, "getblockchaininfo")) return (answer){bci, NULL};
    return (answer){NULL, "{\"code\":-32601,\"message\":\"Method not found\"}"};
}

static void test_ready(void)
{
    static const struct {
        btc_chain chain;
        const char *info;
        int want;
        const char *why;
    } V[] = {
        {BTC_REGTEST, "{\"chain\":\"regtest\",\"blocks\":200,\"headers\":200,\"initialblockdownload\":false,\"pruned\":false}", 1, "at block 200"},
        {BTC_MAIN, "{\"chain\":\"main\",\"blocks\":870000,\"headers\":870001,\"initialblockdownload\":false,\"pruned\":true}", 1, "870000, pruned"},
        {BTC_TEST, "{\"chain\":\"testnet4\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", 1, "at block 5"},
        {BTC_TEST, "{\"chain\":\"test\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", 1, "at block 5"},
        {BTC_SIGNET, "{\"chain\":\"signet\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", 1, "at block 5"},
        /* behind: still syncing, or blocks behind the headers it knows (back after a few days off) */
        {BTC_MAIN, "{\"chain\":\"main\",\"blocks\":300000,\"headers\":870000,\"initialblockdownload\":true,\"pruned\":false}", 0, "block 300000 of 870000"},
        {BTC_MAIN, "{\"chain\":\"main\",\"blocks\":869000,\"headers\":870000,\"initialblockdownload\":false,\"pruned\":false}", 0, "syncing"},
        {BTC_MAIN, "{\"chain\":\"main\",\"blocks\":870000,\"headers\":870002,\"initialblockdownload\":false,\"pruned\":false}", 0, "syncing"},
        {BTC_REGTEST, "{\"chain\":\"regtest\",\"blocks\":0,\"headers\":0,\"initialblockdownload\":true,\"pruned\":false}", 0, "syncing"},
        /* another chain: never */
        {BTC_MAIN, "{\"chain\":\"test\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", -1, "on the chain test"},
        {BTC_REGTEST, "{\"chain\":\"main\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", -1, "on the chain main"},
        {BTC_SIGNET, "{\"chain\":\"testnet4\",\"blocks\":5,\"headers\":5,\"initialblockdownload\":false,\"pruned\":false}", -1, "testnet4"},
        /* a reply that is not one: as a node that does not answer */
        {BTC_MAIN, "{\"chain\":\"main\",\"blocks\":5}", 0, "does not answer"},
    };
    for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
        char why[300];
        bci = V[k].info;
        start(ans_bci, V[k].chain);
        const int r = chain_rpc_ready(&CR, why, sizeof why);
        if (r != V[k].want || !strstr(why, V[k].why)) {
            printf("FAIL ready %zu: %d (%s)\n", k, r, why);
            g_fails++;
        }
        btc_rpc_done(&R);
    }
    /* nobody listening */
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    bind(s, (struct sockaddr *)&a, sizeof a);
    getsockname(s, (struct sockaddr *)&a, &al);
    close(s);
    char why[300];
    CHECK(btc_rpc_init(&R, "127.0.0.1", ntohs(a.sin_port), "u", "p"));
    CR = (chain_rpc){.rpc = &R, .chain = BTC_MAIN};
    CHECK(chain_rpc_ready(&CR, why, sizeof why) == 0 && strstr(why, "does not answer"));
    btc_rpc_done(&R);
}

/* ---- the coins of an address ---- */

/* the P2PKH script of the hash160 751e76... (the key 1), on regtest mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r */
static const char SPK_HEX[] = "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac";
static uint8_t SPK[BTC_P2PKH_LEN];

static char *unspents;   /* the result of scantxoutset of the case */
static int busy_scans;   /* answered "in progress" first */

/* A pruned node, as the puzzle coins of 2015 are on most nodes that are not archives: the blocks of the coins are gone, and with
 * them the bytes of the transactions that made them. */
static answer ans_pruned(const char *method, const char *params, int nth)
{
    (void)params;
    if (!strcmp(method, "getblockchaininfo")) return (answer){bci, NULL};
    if (!strcmp(method, "scantxoutset")) {
        if (nth < busy_scans) return (answer){NULL, "{\"code\":-8,\"message\":\"Scan already in progress, use action \\\"abort\\\" or \\\"status\\\"\"}"};
        return (answer){unspents, NULL};
    }
    if (!strcmp(method, "getblockhash")) return (answer){"\"0000000000000000000000000000000000000000000000000000000000000011\"", NULL};
    if (!strcmp(method, "getrawtransaction")) return (answer){NULL, "{\"code\":-1,\"message\":\"Block not available (pruned data)\"}"};
    return (answer){NULL, "{\"code\":-32601,\"message\":\"Method not found\"}"};
}

/* the result of scantxoutset with n coins in the order of their txids, as the node lists them: the value of coin k from value(k) */
static char *scan_result(int n, unsigned long long (*value)(int), const char *(*spk)(int), bool (*coinbase)(int), int (*height)(int))
{
    char *r = malloc((size_t)n * 400 + 256);
    size_t o = (size_t)sprintf(r, "{\"success\":true,\"txouts\":100,\"height\":1000,\"bestblock\":\"00\",\"unspents\":[");
    for (int k = 0; k < n; k++) {
        const unsigned long long v = value(k);
        o += (size_t)sprintf(r + o, "%s{\"txid\":\"%064x\",\"vout\":%d,\"scriptPubKey\":\"%s\",\"desc\":\"addr(x)#y\",\"amount\":%llu.%08llu,"
                             "\"coinbase\":%s,\"height\":%d,\"blockhash\":\"00\",\"confirmations\":5}", k ? "," : "", k + 1, k % 2,
                             spk ? spk(k) : SPK_HEX, v / 100000000, v % 100000000, coinbase && coinbase(k) ? "true" : "false",
                             height ? height(k) : 500);
    }
    sprintf(r + o, "],\"total_amount\":0}");
    return r;
}

static unsigned long long dust_then_prize(int k) { return k == 99 ? 100000000ull : 546 + (unsigned)k; }
static unsigned long long three(int k) { return 10000ull * (unsigned)(k + 1); }
static const char *other_spk(int k) { return k == 1 ? "0014751e76e8199196d454941c45d1b3a323f1433bd6" : SPK_HEX; }
static bool coinbase_all(int k) { (void)k; return true; }
static int heights(int k) { return k == 0 ? 902 : k == 1 ? 901 : 0; }   /* the tip is 1000: 902 is not mature, 901 is; 0 cannot be */

static void test_coins(void)
{
    payout_coin pc[64];
    bci = BCI_OK;
    /* 99 tiny coins first, the prize last: on a pruned node, from the values of the scan, the prize among them */
    unspents = scan_result(100, dust_then_prize, NULL, NULL, NULL);
    start(ans_pruned, BTC_REGTEST);
    int n = CH.coins(CH.ctx, SPK, pc, 64);
    CHECK(n == 64);
    int prize = -1;
    for (int k = 0; k < n; k++)
        if (pc[k].value == 100000000) prize = k;
    CHECK(prize >= 0 && pc[prize].txid[0] == 100 && pc[prize].vout == 1);
    for (int k = 0; k < n; k++) CHECK(pc[k].value == 100000000 || pc[k].value >= 546 + 99 - 63);   /* the 63 largest of the others */
    CHECK(called("getblockchaininfo scantxoutset "));   /* nothing else: the node's values are the chain's */
    CHECK(strstr(N.calls, "getrawtransaction") == NULL);
    btc_rpc_done(&R);
    free(unspents);

    /* a scan in progress (the partner on the same node, a wallet): waited for */
    unspents = scan_result(3, three, other_spk, NULL, NULL);
    busy_scans = 2;
    start(ans_pruned, BTC_REGTEST);
    n = CH.coins(CH.ctx, SPK, pc, 64);
    CHECK(n == 2 && pc[0].value + pc[1].value == 40000);   /* the second coin is of another script: not taken */
    CHECK(called("getblockchaininfo scantxoutset scantxoutset scantxoutset "));
    btc_rpc_done(&R);
    /* room for one: the largest */
    busy_scans = 0;
    start(ans_pruned, BTC_REGTEST);
    CHECK(CH.coins(CH.ctx, SPK, pc, 1) == 1 && pc[0].value == 30000);
    btc_rpc_done(&R);
    free(unspents);

    /* coinbase coins: spendable 100 blocks after theirs, the next block being the tip + 1 */
    unspents = scan_result(3, three, NULL, coinbase_all, heights);
    start(ans_pruned, BTC_REGTEST);
    n = CH.coins(CH.ctx, SPK, pc, 64);
    CHECK(n == 1 && pc[0].value == 20000);
    btc_rpc_done(&R);
    free(unspents);

    /* a node that is behind: no scan, no coins */
    bci = "{\"chain\":\"regtest\",\"blocks\":10,\"headers\":900,\"initialblockdownload\":false,\"pruned\":false}";
    unspents = scan_result(3, three, NULL, NULL, NULL);
    start(ans_pruned, BTC_REGTEST);
    CHECK(CH.coins(CH.ctx, SPK, pc, 64) == -1 && called("getblockchaininfo "));
    btc_rpc_done(&R);
    /* the scan fails for another reason */
    bci = BCI_OK;
    busy_scans = 0;
    start(ans_pruned, BTC_MAIN);   /* a main-chain address to scan: the node is on regtest */
    CHECK(CH.coins(CH.ctx, SPK, pc, 64) == -1 && called("getblockchaininfo "));
    btc_rpc_done(&R);
    free(unspents);
}

/* ---- the height, the broadcast, the confirmations, the fee rate ---- */

static answer ans_misc(const char *method, const char *params, int nth)
{
    if (!strcmp(method, "getblockcount")) {   /* 500, then a failure, then 501 */
        if (nth == 1) return (answer){NULL, "{\"code\":-28,\"message\":\"Loading block index...\"}"};
        return (answer){nth ? "501" : "500", NULL};
    }
    if (!strcmp(method, "sendrawtransaction")) {
        static const char *const e[] = {"{\"code\":-27,\"message\":\"Transaction outputs already in utxo set\"}",
                                        "{\"code\":-26,\"message\":\"txn-already-known\"}", "{\"code\":-26,\"message\":\"min relay fee not met\"}",
                                        "{\"code\":-25,\"message\":\"bad-txns-inputs-missingorspent\"}"};
        if (nth == 0) return (answer){"\"f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16\"", NULL};
        return (answer){NULL, e[(nth - 1) % 4]};
    }
    if (!strcmp(method, "gettxout")) {
        if (nth == 0) return (answer){"null", NULL};
        if (nth == 1) return (answer){NULL, "{\"code\":-8,\"message\":\"x\"}"};
        return (answer){"{\"confirmations\":3,\"value\":1.0,\"scriptPubKey\":{\"hex\":\"51\"},\"coinbase\":false}", NULL};
    }
    if (!strcmp(method, "estimatesmartfee")) {
        if (strcmp(params, "[2]")) return (answer){NULL, "{\"code\":-8,\"message\":\"wrong target\"}"};
        if (nth == 0) return (answer){"{\"feerate\":0.00004321,\"blocks\":2}", NULL};
        return (answer){"{\"errors\":[\"Insufficient data or no feerate found\"],\"blocks\":0}", NULL};
    }
    return (answer){NULL, "{\"code\":-32601,\"message\":\"Method not found\"}"};
}

static void test_misc(void)
{
    start(ans_misc, BTC_REGTEST);
    CR.conf_target = 2;
    /* A failed call is not height 0: a session waits a number of blocks from a height, and 0 would end the wait at once. */
    CHECK(CH.height(CH.ctx) == 500);
    CHECK(CH.height(CH.ctx) == 501 && CR.tip == 501);   /* tried again after the failure */
    btc_tx tx;
    uint8_t id[32] = {1}, spk[BTC_P2PKH_LEN];
    pt_unhex(SPK_HEX, spk, sizeof spk);
    btc_tx_init(&tx);
    CHECK(btc_tx_add_in(&tx, id, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&tx, 1000, spk, sizeof spk));
    /* sent; sent already (in a block, in the mempool); refused */
    CHECK(CH.broadcast(CH.ctx, &tx) && CH.broadcast(CH.ctx, &tx) && CH.broadcast(CH.ctx, &tx));
    CHECK(!CH.broadcast(CH.ctx, &tx) && !CH.broadcast(CH.ctx, &tx));
    CHECK(CH.confirmations(CH.ctx, id, 0) == -1 && CH.confirmations(CH.ctx, id, 0) == -2 && CH.confirmations(CH.ctx, id, 0) == 3);
    CHECK(CH.feerate(CH.ctx) == 4321 && CH.feerate(CH.ctx) == 2000);   /* satoshis per 1000 virtual bytes; the fallback */
    btc_rpc_done(&R);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);                                  /* a client that gave up is an error of the send, not the end of the test */
    N.lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    if (N.lfd < 0 || bind(N.lfd, (struct sockaddr *)&a, sizeof a) || listen(N.lfd, 8) || getsockname(N.lfd, (struct sockaddr *)&a, &al)) {
        printf("t_chain_rpc: cannot listen on 127.0.0.1\n");
        return 1;
    }
    N.port = ntohs(a.sin_port);
    pt_unhex(SPK_HEX, SPK, sizeof SPK);
    pthread_create(&N.th, NULL, node, NULL);
    test_ready();
    test_coins();
    test_misc();
    pthread_mutex_lock(&N.mu);
    N.stop = true;
    pthread_mutex_unlock(&N.mu);
    pthread_join(N.th, NULL);
    close(N.lfd);
    TEST_DONE("t_chain_rpc");
}
