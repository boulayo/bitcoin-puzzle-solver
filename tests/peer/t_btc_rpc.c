/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_btc_rpc.c - the JSON-RPC client of bitcoind against a small fake server in this file: it listens on a port of 127.0.0.1, takes
 * one request per case, keeps it for the checks and answers with a canned reply, framed and delivered as the case says (with a
 * length, chunked, up to the close, cut short; at once, a byte at a time, never). */
#include "btc_rpc.h"
#include "peertest.h"
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum { LENGTH, CHUNKED, CLOSE, CUT, CUT_CHUNKED };   /* framing of the body */
enum { ALL, DRIP, SILENT, NOTHING };                 /* delivery */

typedef struct {
    int status;
    const char *body;   /* @ID@ becomes the id of the request */
    int framing, delivery;
    const char *raw;    /* when not NULL, sent as it is instead */
} reply_case;

static struct {
    int lfd;
    unsigned port;
    reply_case rc[2];   /* the reply to the first connection, and to a second one when n == 2 */
    int n;
    char req[1 << 20];  /* the last request */
    size_t req_len;
    char first[4096];   /* the first request, when there were two */
    pthread_t th;
} S;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void send_some(int fd, const char *p, size_t n, bool drip)
{
    while (n) {
        const size_t k = drip ? 1 : n;
        const ssize_t w = send(fd, p, k, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
        if (drip) nanosleep(&(struct timespec){0, 50000}, NULL);   /* so that the client sees the reply in pieces */
    }
}

/* the text of the case with the id of the request in place of @ID@ */
static char *make_body(const char *tmpl)
{
    char id[32] = "0";
    const char *b = strstr(S.req, "\r\n\r\n"), *p = b ? strstr(b, "\"id\":") : NULL;
    if (p) snprintf(id, sizeof id, "%llu", strtoull(p + 5, NULL, 10));
    char *out = malloc(strlen(tmpl) + 64), *o = out;
    for (const char *t = tmpl; *t;) {
        if (!strncmp(t, "@ID@", 4)) { o += sprintf(o, "%s", id); t += 4; }
        else *o++ = *t++;
    }
    *o = 0;
    return out;
}

static void answer(int fd, const reply_case *rc);

static void *server(void *arg)
{
    (void)arg;
    for (int i = 0; i < S.n; i++) {
        struct pollfd pl = {.fd = S.lfd, .events = POLLIN};
        if (poll(&pl, 1, 5000) <= 0) return NULL;
        const int fd = accept(S.lfd, NULL, NULL);
        if (fd < 0) return NULL;
        if (i) {
            const size_t n = S.req_len < sizeof S.first - 1 ? S.req_len : sizeof S.first - 1;
            memcpy(S.first, S.req, n);
            S.first[n] = 0;
        }
        S.req_len = 0;
        S.req[0] = 0;
        answer(fd, &S.rc[i]);
    }
    return NULL;
}

static void answer(int fd, const reply_case *rc)
{
    /* the request: the head, then Content-Length bytes */
    for (;;) {
        const ssize_t k = recv(fd, S.req + S.req_len, sizeof S.req - 1 - S.req_len, 0);
        if (k <= 0) break;
        S.req_len += (size_t)k;
        S.req[S.req_len] = 0;
        const char *e = strstr(S.req, "\r\n\r\n"), *cl = strcasestr(S.req, "\r\nContent-Length:");
        if (e && cl && S.req_len >= (size_t)(e + 4 - S.req) + strtoul(cl + 17, NULL, 10)) break;
    }
    const bool drip = rc->delivery == DRIP;
    if (rc->delivery == SILENT) {
        char tmp[64];
        struct pollfd p = {.fd = fd, .events = POLLIN};
        while (poll(&p, 1, 5000) > 0 && recv(fd, tmp, sizeof tmp, 0) > 0) {}   /* until the client gives up */
    } else if (rc->raw) {
        char *r = make_body(rc->raw);
        send_some(fd, r, strlen(r), drip);
        free(r);
    } else if (rc->delivery != NOTHING) {
        char *body = make_body(rc->body);
        const size_t n = strlen(body);
        char head[512];
        int hl = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nDate: today\r\n", rc->status);
        if (rc->framing == LENGTH) hl += snprintf(head + hl, sizeof head - hl, "Content-Length: %zu\r\n", n);
        if (rc->framing == CUT) hl += snprintf(head + hl, sizeof head - hl, "content-length: %zu\r\n", n + 50);
        if (rc->framing == CHUNKED || rc->framing == CUT_CHUNKED) hl += snprintf(head + hl, sizeof head - hl, "Transfer-Encoding: chunked\r\n");
        hl += snprintf(head + hl, sizeof head - hl, "\r\n");
        send_some(fd, head, (size_t)hl, drip);
        if (rc->framing == CHUNKED || rc->framing == CUT_CHUNKED) {
            for (size_t i = 0, k = 0; i < n; i += 7, k++) {   /* chunks of 7 bytes, one with an extension */
                char sz[32];
                const size_t c = n - i < 7 ? n - i : 7;
                const int sl = snprintf(sz, sizeof sz, k == 1 ? "%zX;ext=1\r\n" : "%zx\r\n", c);
                send_some(fd, sz, (size_t)sl, drip);
                send_some(fd, body + i, c, drip);
                send_some(fd, "\r\n", 2, drip);
            }
            if (rc->framing == CHUNKED) send_some(fd, "0\r\nX-Trailer: 1\r\n\r\n", 20, drip);
        } else {
            send_some(fd, body, n, drip);
        }
        free(body);
    }
    /* close only once the client has gone, so that nothing it still reads is lost */
    shutdown(fd, SHUT_WR);
    char tmp[64];
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (poll(&p, 1, 3000) > 0 && recv(fd, tmp, sizeof tmp, 0) > 0) {}
    close(fd);
}

/* the replies to n connections, one after the other (two: a call the client makes again, or a call that follows) */
static void serve_n(int n, reply_case a, reply_case b)
{
    S.rc[0] = a;
    S.rc[1] = b;
    S.n = n;
    S.req_len = 0;
    S.req[0] = 0;
    S.first[0] = 0;
    pthread_create(&S.th, NULL, server, NULL);
}

static void serve(reply_case rc) { serve_n(1, rc, rc); }

static void served(void) { pthread_join(S.th, NULL); }

static reply_case ok_body(const char *body) { return (reply_case){200, body, LENGTH, ALL, NULL}; }

/* the request the server received: its first line, a header, the body as JSON */
static bool req_line(const char *want) { return !strncmp(S.req, want, strlen(want)) && S.req[strlen(want)] == '\r'; }

static const char *req_header(const char *name)
{
    static char v[1024];
    const char *e = strstr(S.req, "\r\n\r\n");
    for (const char *p = strstr(S.req, "\r\n"); p && p < e; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, strlen(name)) && l[strlen(name)] == ':') {
            const char *s = l + strlen(name) + 1, *t = strstr(s, "\r\n");
            while (*s == ' ') s++;
            snprintf(v, sizeof v, "%.*s", (int)(t - s), s);
            return v;
        }
    }
    return NULL;
}

/* the params of the request as compact JSON text, as the client wrote them */
static const char *req_params(void)
{
    static char v[4096];
    const char *p = strstr(S.req, "\"params\":");
    if (!p) return "";
    snprintf(v, sizeof v, "%s", p + 9);
    const size_t n = strlen(v);
    if (n && v[n - 1] == '}') v[n - 1] = 0;
    return v;
}

static const char *req_method(void)
{
    static char m[64];
    const char *p = strstr(S.req, "\"method\":\"");
    m[0] = 0;
    if (p) sscanf(p + 10, "%63[^\"]", m);
    return m;
}

static btc_rpc C;

static void client(void)
{
    CHECK(btc_rpc_init(&C, "127.0.0.1", S.port, "user", "pa:ss"));
}

static void test_basic(void)
{
    char host[64];
    snprintf(host, sizeof host, "127.0.0.1:%u", S.port);
    client();
    uint64_t h = 0;
    serve(ok_body("{\"result\":812345,\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockcount(&C, &h) && h == 812345);
    served();
    CHECK(req_line("POST / HTTP/1.1"));
    CHECK_STR(req_header("Host"), host);
    CHECK_STR(req_header("Authorization"), "Basic dXNlcjpwYTpzcw==");
    CHECK_STR(req_header("Content-Type"), "application/json");
    CHECK_STR(req_header("Connection"), "close");
    /* the body, whole */
    const char *b = strstr(S.req, "\r\n\r\n") + 4;
    CHECK_STR(b, "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"getblockcount\",\"params\":[]}");
    CHECK(strtoul(req_header("Content-Length"), NULL, 10) == strlen(b));
    CHECK(C.err.kind == BTC_RPC_OK);

    uint8_t hash[32];
    serve(ok_body("{\"result\":\"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f\",\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockhash(&C, 0, hash) && hash[0] == 0x6f && hash[31] == 0x00);
    served();
    CHECK_STR(req_method(), "getblockhash");
    CHECK_STR(req_params(), "[0]");
    CHECK(strstr(S.req, "\"id\":2,") != NULL);   /* a new id per call */

    /* the generic call, with its own params */
    json_w w;
    jw_init(&w);
    jw_arr(&w);
    jw_str(&w, "x\"y");
    jw_end(&w);
    json_doc d;
    const json_val *res;
    serve(ok_body("{\"result\":{\"a\":[1,2]},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_call(&C, false, "echo", &w, &d, &res) && res->type == JSON_OBJECT && json_at(json_get(res, "a"), 1));
    served();
    CHECK_STR(req_params(), "[\"x\\\"y\"]");
    json_free(&d);
    jw_free(&w);
    btc_rpc_done(&C);
}

/* the genesis coinbase, whose txid is 4a5e1e4b...: the reply of getrawtransaction is checked against it with btc_prevout */
static const char GENESIS_CB[] =
    "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4d04ffff001d0104455468652054696d65732030"
    "332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73ffff"
    "ffff0100f2052a01000000434104678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec1"
    "12de5c384df7ba0b8d578a4c702b6bf11d5fac00000000";

static void test_calls(void)
{
    client();
    char body[2048];
    uint8_t txid[32], bh[32], *raw = NULL;
    size_t len = 0;
    btc_hash_from_hex("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b", txid);
    btc_hash_from_hex("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f", bh);

    /* getrawtransaction with the block, then the amount check of what came */
    snprintf(body, sizeof body, "{\"result\":\"%s\",\"error\":null,\"id\":@ID@}", GENESIS_CB);
    serve(ok_body(body));
    CHECK(btc_rpc_getrawtransaction(&C, txid, bh, &raw, &len) && len == strlen(GENESIS_CB) / 2);
    served();
    CHECK_STR(req_params(), "[\"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b\",0,"
                            "\"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f\"]");
    uint64_t v = 0;
    const uint8_t *spk;
    size_t sl;
    CHECK(raw && btc_prevout(raw, len, txid, 0, &v, &spk, &sl) && v == 5000000000ull && sl == 67);
    free(raw);
    raw = NULL;
    /* without the block; a reply that is not hex */
    serve(ok_body("{\"result\":\"0g\",\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && C.err.kind == BTC_RPC_EPROTO && !raw);
    served();
    CHECK_STR(req_params(), "[\"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b\",0]");
    serve(ok_body("{\"result\":\"abc\",\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && C.err.kind == BTC_RPC_EPROTO);
    served();
    /* not found: the error of bitcoind, with HTTP 500 as it sends it to a JSON-RPC 1.0 request */
    reply_case nf = {500, "{\"result\":null,\"error\":{\"code\":-5,\"message\":\"No such mempool or blockchain transaction. Use gettransaction "
                          "for wallet transactions.\"},\"id\":@ID@}", LENGTH, ALL, NULL};
    serve(nf);
    CHECK(!btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && C.err.kind == BTC_RPC_ERPC && C.err.code == -5);
    CHECK(strstr(C.err.msg, "getrawtransaction: No such mempool") == C.err.msg);
    served();

    /* a large reply: 400 kB of hex, more than the first buffer */
    const size_t big = 200000;
    char *bb = malloc(2 * big + 128);
    int o = sprintf(bb, "{\"result\":\"");
    for (size_t i = 0; i < big; i++) o += sprintf(bb + o, "%02x", (unsigned)(i * 7 & 0xff));
    sprintf(bb + o, "\",\"error\":null,\"id\":@ID@}");
    serve(ok_body(bb));
    CHECK(btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && len == big && raw[big - 1] == (uint8_t)((big - 1) * 7));
    served();
    free(raw);
    raw = NULL;
    reply_case bigc = {200, bb, CHUNKED, ALL, NULL};
    serve(bigc);
    CHECK(btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && len == big && raw[12345] == (uint8_t)(12345 * 7));
    served();
    free(raw);
    free(bb);

    /* scantxoutset: the unspent outputs with their heights, amounts exactly */
    const char *addrs[2] = {"1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"};
    const char *scan =
        "{\"result\":{\"success\":true,\"txouts\":185000000,\"height\":870000,\"bestblock\":\"00000000000000000001\","
        "\"unspents\":[{\"txid\":\"f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16\",\"vout\":1,"
        "\"scriptPubKey\":\"76a914751e76e8199196d454941c45d1b3a323f1433bd688ac\",\"desc\":\"addr(1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH)#x\","
        "\"amount\":0.00012345,\"coinbase\":false,\"height\":869000,\"blockhash\":\"0000\"},"
        "{\"txid\":\"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b\",\"vout\":0,"
        "\"scriptPubKey\":\"0014751e76e8199196d454941c45d1b3a323f1433bd6\",\"desc\":\"d\",\"amount\":6.25000000,\"coinbase\":true,"
        "\"height\":100}],\"total_amount\":6.25012345},\"error\":null,\"id\":@ID@}";
    btc_utxo u[3];
    memset(u, 0xee, sizeof u);
    size_t n = 0;
    uint64_t height = 0;
    serve(ok_body(scan));
    CHECK(btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && n == 2 && height == 870000);
    served();
    CHECK_STR(req_method(), "scantxoutset");
    CHECK_STR(req_params(), "[\"start\",[\"addr(1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH)\",\"addr(bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4)\"]]");
    CHECK(u[0].vout == 1 && u[0].value == 12345 && u[0].height == 869000 && !u[0].coinbase && u[0].spk_len == 25 && u[0].txid[0] == 0x16);
    CHECK(u[1].vout == 0 && u[1].value == 625000000 && u[1].height == 100 && u[1].coinbase && u[1].spk_len == 22 && u[1].spk[1] == 20);
    CHECK(u[2].vout == 0xeeeeeeee);   /* untouched */
    /* with room for one, the largest, which comes last */
    memset(u, 0xee, sizeof u);
    serve(ok_body(scan));
    CHECK(btc_rpc_scan(&C, addrs, 2, u, 1, &n, &height) && n == 2 && u[0].value == 625000000 && u[1].vout == 0xeeeeeeee);
    served();
    /* An address with more coins than room: thousands of tiny ones may be sent to a puzzle address, and the node lists them in the
     * order of their txids, so the prize can come after the room is full. The largest are kept; of equal values, the smaller
     * outpoints. Here 59 coins of 546 satoshis, 40 of 2000, and the prize last: the 23 of 546 with the smallest txids make 64. */
    {
        enum { NU = 100, ROOM = 64 };
        char *many = malloc(NU * 300 + 256);
        size_t at = (size_t)sprintf(many, "{\"result\":{\"success\":true,\"height\":870000,\"unspents\":[");
        for (int k = 0; k < NU; k++) {
            const unsigned long long sat = k < 59 ? 546 : k < 99 ? 2000 : 100000000;
            at += (size_t)sprintf(many + at, "%s{\"txid\":\"%064x\",\"vout\":0,\"scriptPubKey\":\"76a914751e76e8199196d454941c45d1b3a323f1433bd688ac\","
                                 "\"amount\":%llu.%08llu,\"height\":%d}", k ? "," : "", k + 1, sat / 100000000, sat % 100000000, 1000 + k);
        }
        sprintf(many + at, "]},\"error\":null,\"id\":@ID@}");
        btc_utxo w[ROOM];
        serve(ok_body(many));
        CHECK(btc_rpc_scan(&C, addrs, 1, w, ROOM, &n, &height) && n == NU);
        served();
        int prize = 0, mid = 0, tiny = 0, last_tiny = 0;
        for (int k = 0; k < ROOM; k++) {
            prize += w[k].value == 100000000;
            mid += w[k].value == 2000;
            if (w[k].value == 546) tiny++, last_tiny = w[k].txid[0] > last_tiny ? w[k].txid[0] : last_tiny;
        }
        CHECK(prize == 1 && mid == 40 && tiny == 23 && last_tiny == 23);
        free(many);
    }
    /* another amount of the same length; a fraction of a satoshi (1234567e-9 BTC); not JSON; a scan that did not finish; a field
     * missing */
    char *bad = malloc(strlen(scan) + 1);
    strcpy(bad, scan);
    memcpy(strstr(bad, "6.25000000"), "0.62500000", 10);
    serve(ok_body(bad));
    CHECK(btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && u[1].value == 62500000);
    served();
    strcpy(bad, scan);
    memcpy(strstr(bad, "6.25000000"), "1234567e-9", 10);
    serve(ok_body(bad));
    CHECK(!btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && C.err.kind == BTC_RPC_EPROTO);
    served();
    strcpy(bad, scan);
    memcpy(strstr(bad, "true,\"txouts\""), "fals", 4);
    serve(ok_body(bad));
    CHECK(!btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && C.err.kind == BTC_RPC_EPROTO);
    served();
    serve(ok_body("{\"result\":{\"success\":false,\"height\":1,\"unspents\":[]},\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && C.err.kind == BTC_RPC_EPROTO);
    served();
    serve(ok_body("{\"result\":{\"success\":true,\"height\":1,\"unspents\":[{\"txid\":\"00\",\"vout\":1}]},\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_scan(&C, addrs, 2, u, 3, &n, &height) && C.err.kind == BTC_RPC_EPROTO);
    served();
    free(bad);

    /* getblockchaininfo: where the node is */
    btc_chaininfo ci;
    serve(ok_body("{\"result\":{\"chain\":\"main\",\"blocks\":869990,\"headers\":870000,\"bestblockhash\":\"00\",\"verificationprogress\":0.99,"
                  "\"initialblockdownload\":false,\"size_on_disk\":1,\"pruned\":true,\"pruneheight\":860000,\"warnings\":[]},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockchaininfo(&C, &ci) && !strcmp(ci.chain, "main") && ci.blocks == 869990 && ci.headers == 870000 && !ci.ibd && ci.pruned);
    served();
    CHECK_STR(req_method(), "getblockchaininfo");
    serve(ok_body("{\"result\":{\"chain\":\"main\",\"blocks\":1,\"headers\":1,\"pruned\":false},\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_getblockchaininfo(&C, &ci) && C.err.kind == BTC_RPC_EPROTO);   /* no initialblockdownload */
    served();

    /* gettxout: spent, unspent */
    bool found = true;
    btc_utxo t;
    serve(ok_body("{\"result\":null,\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_gettxout(&C, txid, 1, true, &found, &t) && !found);
    served();
    CHECK_STR(req_params(), "[\"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b\",1,true]");
    serve(ok_body("{\"result\":{\"bestblock\":\"00\",\"confirmations\":0,\"value\":0.00000546,\"scriptPubKey\":{\"asm\":\"\","
                  "\"desc\":\"\",\"hex\":\"5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433\",\"type\":\"witness_v1_taproot\"},"
                  "\"coinbase\":false},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_gettxout(&C, txid, 3, false, &found, &t) && found && t.value == 546 && t.confirmations == 0 && t.spk_len == 34 &&
          t.vout == 3 && !memcmp(t.txid, txid, 32) && t.spk[0] == 0x51);
    served();
    CHECK_STR(req_params(), "[\"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b\",3,false]");

    /* estimatesmartfee: a rate, or none */
    uint64_t rate = 1;
    serve(ok_body("{\"result\":{\"feerate\":0.00012,\"blocks\":2},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_estimatesmartfee(&C, 6, &rate) && rate == 12000);
    served();
    CHECK_STR(req_params(), "[6]");
    serve(ok_body("{\"result\":{\"errors\":[\"Insufficient data or no feerate found\"],\"blocks\":0},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_estimatesmartfee(&C, 2, &rate) && rate == 0);
    served();

    /* testmempoolaccept and sendrawtransaction */
    const uint8_t tx[3] = {0x01, 0xab, 0xff};
    bool ok = true;
    char why[64];
    serve(ok_body("{\"result\":[{\"txid\":\"aa\",\"wtxid\":\"aa\",\"allowed\":false,\"reject-reason\":\"min relay fee not met\"}],"
                  "\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_testmempoolaccept(&C, tx, 3, &ok, why, sizeof why) && !ok);
    CHECK_STR(why, "min relay fee not met");
    served();
    CHECK_STR(req_params(), "[[\"01abff\"]]");
    serve(ok_body("{\"result\":[{\"txid\":\"aa\",\"wtxid\":\"aa\",\"allowed\":true,\"vsize\":191,\"fees\":{\"base\":0.00001}}],"
                  "\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_testmempoolaccept(&C, tx, 3, &ok, why, sizeof why) && ok);
    CHECK_STR(why, "");
    served();
    reply_case rej = {500, "{\"result\":null,\"error\":{\"code\":-26,\"message\":\"min relay fee not met, 100 < 191\"},\"id\":@ID@}",
                      LENGTH, ALL, NULL};
    serve(rej);
    CHECK(!btc_rpc_sendrawtransaction(&C, tx, 3, txid) && C.err.kind == BTC_RPC_ERPC && C.err.code == -26);
    CHECK_STR(C.err.msg, "sendrawtransaction: min relay fee not met, 100 < 191");
    served();
    uint8_t got[32];
    serve(ok_body("{\"result\":\"f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16\",\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_sendrawtransaction(&C, tx, 3, got) && got[0] == 0x16 && got[31] == 0xf4);
    served();
    CHECK_STR(req_params(), "[\"01abff\"]");
    btc_rpc_done(&C);
}

static void test_wallet(void)
{
    client();
    char addr[100];
    uint8_t txid[32], last[32];
    serve(ok_body("{\"result\":{\"name\":\"w1\",\"warning\":\"\"},\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_createwallet(&C, "w1"));
    served();
    CHECK(req_line("POST / HTTP/1.1"));
    CHECK_STR(req_params(), "[\"w1\"]");
    CHECK(btc_rpc_set_wallet(&C, "w1") && !btc_rpc_set_wallet(&C, "a/b") && !btc_rpc_set_wallet(&C, "a b") && !btc_rpc_set_wallet(&C, "%2e"));
    CHECK(strcmp(C.wallet, "/wallet/w1") == 0);   /* a refused name leaves the wallet as it was */
    serve(ok_body("{\"result\":\"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080\",\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getnewaddress(&C, "bech32", addr, sizeof addr));
    CHECK_STR(addr, "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080");
    served();
    CHECK(req_line("POST /wallet/w1 HTTP/1.1"));
    CHECK_STR(req_params(), "[\"\",\"bech32\"]");
    serve(ok_body("{\"result\":\"mrCDrCybB6J1vRfbwM5hemdJz73FwDBC8r\",\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_getnewaddress(&C, NULL, addr, 10) && C.err.kind == BTC_RPC_EPROTO);   /* does not fit */
    served();
    CHECK_STR(req_params(), "[]");
    serve(ok_body("{\"result\":\"f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16\",\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_sendtoaddress(&C, "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080", 100000, txid) && txid[0] == 0x16);
    served();
    CHECK(req_line("POST /wallet/w1 HTTP/1.1"));
    CHECK_STR(req_params(), "[\"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080\",0.00100000]");
    serve(ok_body("{\"result\":[\"00000000000000000000000000000000000000000000000000000000000000aa\","
                  "\"00000000000000000000000000000000000000000000000000000000000000bb\"],\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_generatetoaddress(&C, 2, "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080", last) && last[0] == 0xbb);
    served();
    CHECK(req_line("POST / HTTP/1.1"));   /* not a wallet call */
    CHECK_STR(req_params(), "[2,\"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080\"]");
    serve(ok_body("{\"result\":[\"00000000000000000000000000000000000000000000000000000000000000aa\"],\"error\":null,\"id\":@ID@}"));
    CHECK(!btc_rpc_generatetoaddress(&C, 2, "x", NULL) && C.err.kind == BTC_RPC_EPROTO);   /* one block for two */
    served();
    CHECK(btc_rpc_set_wallet(&C, NULL) && C.wallet[0] == 0);
    serve(ok_body("{\"result\":\"x\",\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getnewaddress(&C, NULL, addr, sizeof addr));
    served();
    CHECK(req_line("POST / HTTP/1.1"));
    btc_rpc_done(&C);
}

static void test_http(void)
{
    client();
    uint64_t h = 0;
    static const char *const good = "{\"result\":42,\"error\":null,\"id\":@ID@}";
    /* the framings and deliveries a server may use */
    static const int fr[] = {LENGTH, CHUNKED, CLOSE};
    for (int f = 0; f < 3; f++)
        for (int dl = 0; dl < 2; dl++) {
            h = 0;
            serve((reply_case){200, good, fr[f], dl ? DRIP : ALL, NULL});
            if (!btc_rpc_getblockcount(&C, &h) || h != 42) {
                printf("FAIL framing %d delivery %d: %s\n", fr[f], dl, C.err.msg);
                g_fails++;
            }
            served();
        }
    /* HTTP/1.0, a status line without a reason, a body up to the close */
    serve((reply_case){0, NULL, 0, ALL, "HTTP/1.0 200\r\nServer: x\r\n\r\n{\"result\":7,\"error\":null,\"id\":@ID@}"});
    CHECK(btc_rpc_getblockcount(&C, &h) && h == 7);
    served();

    /* cut short: fewer bytes than the length said, no last chunk, nothing at all */
    serve((reply_case){200, good, CUT, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "cut short"));
    served();
    serve((reply_case){200, good, CUT_CHUNKED, DRIP, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "cut short"));
    served();
    serve((reply_case){200, good, LENGTH, NOTHING, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "without a reply"));
    served();
    serve((reply_case){0, NULL, 0, ALL, "HTTP/1.1 200 OK\r\nContent-Le"});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET);
    served();

    /* not HTTP, a bad length, a bad chunk */
    static const char *const notit[] = {
        "HTTX/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 2x0 OK\r\nContent-Length: 2\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Length: 2x\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n{}",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n{}\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}xx0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\n{}\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\nbad trailer\n\r\n",
    };
    for (size_t k = 0; k < sizeof notit / sizeof notit[0]; k++) {
        serve((reply_case){0, NULL, 0, ALL, notit[k]});
        if (btc_rpc_getblockcount(&C, &h) || C.err.kind != BTC_RPC_EPROTO) {
            printf("FAIL accepted reply %zu: %s\n", k, C.err.msg);
            g_fails++;
        }
        served();
    }

    /* larger than a reply may be: refused when the length says so, and when the bytes keep coming */
    serve((reply_case){0, NULL, 0, ALL, "HTTP/1.1 200 OK\r\nContent-Length: 16777217\r\n\r\n{}"});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EPROTO);
    served();
    const size_t hn = (size_t)18 << 20;
    char *huge = malloc(hn + 1);
    memset(huge, ' ', hn);
    huge[hn] = 0;
    serve((reply_case){200, huge, CLOSE, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EPROTO && strstr(C.err.msg, "too large"));
    served();
    free(huge);

    /* statuses and bodies */
    serve((reply_case){401, "", LENGTH, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EHTTP && C.err.code == 401 && strstr(C.err.msg, "password"));
    served();
    serve((reply_case){503, "<html>busy</html>", LENGTH, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EHTTP && C.err.code == 503);
    served();
    serve((reply_case){404, "{\"result\":null,\"error\":{\"code\":-32601,\"message\":\"Method not found\"},\"id\":@ID@}", LENGTH, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ERPC && C.err.code == -32601);
    served();
    serve((reply_case){500, "{\"result\":3,\"error\":null,\"id\":@ID@}", LENGTH, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EHTTP && C.err.code == 500);
    served();
    /* JSON-RPC 2.0 form: no "error" member on success; an error with HTTP 200 */
    serve(ok_body("{\"jsonrpc\":\"2.0\",\"result\":5,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockcount(&C, &h) && h == 5);
    served();
    serve(ok_body("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-28,\"message\":\"Loading block index...\"},\"id\":@ID@}"));
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ERPC && C.err.code == -28);
    served();
    /* replies that are not what was asked */
    static const char *const wrong[] = {
        "not json", "", "[1]", "{\"result\":1,\"error\":null,\"id\":999}", "{\"result\":1,\"error\":null}", "{\"error\":null,\"id\":@ID@}",
        "{\"result\":null,\"error\":{\"message\":\"no code\"},\"id\":@ID@}", "{\"result\":\"12\",\"error\":null,\"id\":@ID@}",
        "{\"result\":-1,\"error\":null,\"id\":@ID@}", "{\"result\":1.5,\"error\":null,\"id\":@ID@}",
        "{\"result\":null,\"error\":{\"code\":99999999999,\"message\":\"x\"},\"id\":@ID@}",
    };
    for (size_t k = 0; k < sizeof wrong / sizeof wrong[0]; k++) {
        serve(ok_body(wrong[k]));
        if (btc_rpc_getblockcount(&C, &h) || C.err.kind != BTC_RPC_EPROTO) {
            printf("FAIL accepted result %zu (%d): %s\n", k, C.err.kind, C.err.msg);
            g_fails++;
        }
        served();
    }
    /* the message of the server cannot reach a terminal with control characters */
    serve(ok_body("{\"result\":null,\"error\":{\"code\":-1,\"message\":\"a\\nb\\u001b[31m \\u009b31m \xc2\x9b" "1m\"},\"id\":@ID@}"));
    CHECK(!btc_rpc_getblockcount(&C, &h));
    CHECK_STR(C.err.msg, "getblockcount: a?b?[31m ??31m ??1m");   /* C0, and C1 in UTF-8 (CSI, U+009B) */
    served();

    /* a body of 60 kB in chunks of one byte, as they come: each chunk is decoded once, when it is whole */
    {
        const size_t hexn = 30000;
        char *body = malloc(2 * hexn + 64), *wire = malloc(6 * (2 * hexn + 64) + 256);
        int bo = sprintf(body, "{\"result\":\"");
        for (size_t i = 0; i < hexn; i++) bo += sprintf(body + bo, "%02x", (unsigned)(i & 0xff));
        bo += sprintf(body + bo, "\",\"error\":null,\"id\":%llu}", (unsigned long long)C.id + 1);   /* @ID@ would be cut in chunks */
        int wo = sprintf(wire, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        for (int i = 0; i < bo; i++) wo += sprintf(wire + wo, "1\r\n%c\r\n", body[i]);
        sprintf(wire + wo, "0\r\n\r\n");
        uint8_t *raw = NULL, txid[32] = {0};
        size_t len = 0;
        serve((reply_case){0, NULL, 0, ALL, wire});
        CHECK(btc_rpc_getrawtransaction(&C, txid, NULL, &raw, &len) && len == hexn && raw[hexn - 1] == (uint8_t)(hexn - 1));
        served();
        free(raw);
        free(body);
        free(wire);
    }

    /* a scan the client gives up on is aborted: the node would go on with it and refuse the next one */
    C.scan_timeout_ms = 300;
    {
        const char *a1[1] = {"1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"};
        btc_utxo u1[1];
        size_t n1;
        uint64_t h1;
        serve_n(2, (reply_case){200, good, LENGTH, SILENT, NULL}, ok_body("{\"result\":true,\"error\":null,\"id\":@ID@}"));
        CHECK(!btc_rpc_scan(&C, a1, 1, u1, 1, &n1, &h1) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "timeout"));
        served();
        CHECK(strstr(S.first, "\"params\":[\"start\",") && !strcmp(req_method(), "scantxoutset") && !strcmp(req_params(), "[\"abort\"]"));
        /* an address that would change the descriptor around it */
        const char *a2[1] = {"1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH),raw(51"};
        CHECK(!btc_rpc_scan(&C, a2, 1, u1, 1, &n1, &h1) && C.err.kind == BTC_RPC_EPROTO);
    }

    /* a server that never answers: the call ends at its time limit */
    C.timeout_ms = 300;
    const int64_t t0 = now_ms();
    serve((reply_case){200, good, LENGTH, SILENT, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "timeout"));
    const int64_t dt = now_ms() - t0;
    CHECK(dt >= 250 && dt < 3000);
    served();
    btc_rpc_done(&C);

    /* nobody listening */
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    bind(s, (struct sockaddr *)&a, sizeof a);
    getsockname(s, (struct sockaddr *)&a, &al);
    close(s);   /* the port is now free and closed */
    CHECK(btc_rpc_init(&C, "127.0.0.1", ntohs(a.sin_port), "u", "p"));
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "cannot connect"));
    btc_rpc_done(&C);

    /* by name: localhost may resolve to ::1 first, where nobody listens, and then to 127.0.0.1 */
    struct addrinfo *ai;
    if (getaddrinfo("localhost", NULL, NULL, &ai) == 0) {
        freeaddrinfo(ai);
        CHECK(btc_rpc_init(&C, "localhost", S.port, "u", "p"));
        serve(ok_body(good));
        CHECK(btc_rpc_getblockcount(&C, &h) && h == 42);
        served();
        char host[64];
        snprintf(host, sizeof host, "localhost:%u", S.port);
        CHECK_STR(req_header("Host"), host);
        btc_rpc_done(&C);
    }
}

static void test_auth(void)
{
    char dir[256], path[300];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/t_btc_rpc.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/.cookie", dir);
    FILE *f = fopen(path, "wb");
    fputs("__cookie__:abcdef0123\n", f);
    fclose(f);
    CHECK(btc_rpc_init_cookie(&C, "127.0.0.1", S.port, path));
    uint64_t h;
    serve(ok_body("{\"result\":1,\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockcount(&C, &h));
    served();
    CHECK_STR(req_header("Authorization"), "Basic X19jb29raWVfXzphYmNkZWYwMTIz");
    btc_rpc_done(&C);
    CHECK(C.auth[0] == 0 && C.host[0] == 0);   /* wiped */
    f = fopen(path, "wb");
    fputs("no colon here", f);
    fclose(f);
    CHECK(!btc_rpc_init_cookie(&C, "127.0.0.1", S.port, path));
    f = fopen(path, "wb");
    fclose(f);
    CHECK(!btc_rpc_init_cookie(&C, "127.0.0.1", S.port, path));
    f = fopen(path, "wb");
    fputs("a:b\nc:d", f);
    fclose(f);
    CHECK(!btc_rpc_init_cookie(&C, "127.0.0.1", S.port, path));
    remove(path);
    CHECK(!btc_rpc_init_cookie(&C, "127.0.0.1", S.port, path));   /* missing */
    /* arguments */
    char longpass[600];
    memset(longpass, 'p', sizeof longpass - 1);
    longpass[sizeof longpass - 1] = 0;
    CHECK(!btc_rpc_init(&C, "", 8332, "u", "p") && !btc_rpc_init(&C, "h", 0, "u", "p") && !btc_rpc_init(&C, "h", 65536, "u", "p"));
    CHECK(!btc_rpc_init(&C, "h", 8332, "a:b", "p") && !btc_rpc_init(&C, "h", 8332, "u", longpass) && !btc_rpc_init(&C, NULL, 1, "u", "p"));
    CHECK(btc_rpc_init(&C, "::1", 18443, "u", "") && C.timeout_ms == 30000);
    btc_rpc_done(&C);
    /* the host goes into the Host header: nothing that could end its line, or that no name has */
    CHECK(!btc_rpc_init(&C, "127.0.0.1\r\nX-Injected: 1", 8332, "u", "p") && !btc_rpc_init(&C, "a b", 8332, "u", "p"));
    CHECK(!btc_rpc_init(&C, "[::1]", 8332, "u", "p") && btc_rpc_init(&C, "fe80::1%eth0", 8332, "u", "p") && btc_rpc_init(&C, "node-1.lan", 1, "u", "p"));
    btc_rpc_done(&C);

    /* the --rpc option: HOST:PORT:USER:PASSWORD, the password the rest of it; HOST:PORT with the cookie; IPv6 in brackets */
    char want[300];
    CHECK(btc_rpc_init_arg(&C, "127.0.0.1:8332:alice:pa:ss word", NULL) && !strcmp(C.host, "127.0.0.1") && !strcmp(C.port, "8332"));
    CHECK_STR(C.auth, "YWxpY2U6cGE6c3Mgd29yZA==");   /* alice:pa:ss word */
    CHECK(btc_rpc_init_arg(&C, "[::1]:18443:u:p", NULL) && !strcmp(C.host, "::1") && !strcmp(C.port, "18443"));
    static const char *const bad_arg[] = {
        "127.0.0.1", "127.0.0.1:", "127.0.0.1:x:u:p", "127.0.0.1:0:u:p", "127.0.0.1:65536:u:p", "127.0.0.1:99999999999999999999:u:p",
        "127.0.0.1:8332:", "127.0.0.1:8332:user", ":8332:u:p", "[::1:8332:u:p", "[]:8332:u:p", "::1:8332:u:p", "127.0.0.1:8332",
        "127.0.0.1:8332x:u:p", "h\r\nX: 1:8332:u:p", "",
    };
    for (size_t k = 0; k < sizeof bad_arg / sizeof bad_arg[0]; k++)
        if (btc_rpc_init_arg(&C, bad_arg[k], NULL) || C.err.kind != BTC_RPC_EPROTO || !strstr(C.err.msg, "expected HOST:PORT")) {
            printf("FAIL --rpc accepted: \"%s\"\n", bad_arg[k]);
            g_fails++;
        }
    CHECK(!btc_rpc_init_arg(&C, NULL, NULL) && C.auth[0] == 0);
    f = fopen(path, "wb");
    fputs("__cookie__:old", f);
    fclose(f);
    CHECK(!btc_rpc_init_arg(&C, "127.0.0.1:8332:u:p", path));   /* the password or the cookie, not both */
    CHECK(btc_rpc_init_arg(&C, "localhost:8332", path) && !strcmp(C.host, "localhost") && !strcmp(C.cookie, path));
    snprintf(want, sizeof want, "%s/missing", dir);
    CHECK(!btc_rpc_init_arg(&C, "localhost:8332", want) && C.err.kind == BTC_RPC_ENET && strstr(C.err.msg, "cookie file") && C.cookie[0] == 0);

    /* The node writes a new cookie each time it starts: a 401 makes the client read it again and call once more, with the new one. */
    snprintf(want, sizeof want, "127.0.0.1:%u", S.port);
    CHECK(btc_rpc_init_arg(&C, want, path));
    f = fopen(path, "wb");
    fputs("__cookie__:new", f);
    fclose(f);
    serve_n(2, (reply_case){401, "", LENGTH, ALL, NULL}, ok_body("{\"result\":9,\"error\":null,\"id\":@ID@}"));
    CHECK(btc_rpc_getblockcount(&C, &h) && h == 9);
    served();
    CHECK(strstr(S.first, "Basic X19jb29raWVfXzpvbGQ=") != NULL);   /* __cookie__:old */
    CHECK_STR(req_header("Authorization"), "Basic X19jb29raWVfXzpuZXc=");
    /* the same cookie: refused again, a 401 for the caller, and no second call */
    serve((reply_case){401, "", LENGTH, ALL, NULL});
    CHECK(!btc_rpc_getblockcount(&C, &h) && C.err.kind == BTC_RPC_EHTTP && C.err.code == 401);
    served();
    btc_rpc_done(&C);
    remove(path);
    rmdir(dir);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);                                  /* a client that gave up is an error of the send, not the end of the test */
    S.lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    if (S.lfd < 0 || bind(S.lfd, (struct sockaddr *)&a, sizeof a) || listen(S.lfd, 4) || getsockname(S.lfd, (struct sockaddr *)&a, &al)) {
        printf("t_btc_rpc: cannot listen on 127.0.0.1\n");
        return 1;
    }
    S.port = ntohs(a.sin_port);
    test_basic();
    test_calls();
    test_wallet();
    test_http();
    test_auth();
    close(S.lfd);
    TEST_DONE("t_btc_rpc");
}
