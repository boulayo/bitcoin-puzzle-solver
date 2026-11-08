/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_net.c - the channel of net.c over 127.0.0.1: a handshake and an exchange, then what must be refused (another network, a responder
 * that is not the expected identity, an ID without the zero bits on either side, a malformed or oversized frame, bytes that are not a
 * handshake) and what must not hang (a peer that says nothing, on both sides). Identities with few zero bits, so that they are quick. */
#include "net.h"
#include <pzcore/hash.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BITS 4
#define SHORT_MS 300               /* the timeout of the tests that wait for nothing */

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0x5e, 0x70};
static home_identity ida, idb, nobits;   /* nobits: an identity whose node ID lacks the BITS zero bits */

static int64_t ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void identity(const char *label, unsigned bits, home_identity *out)
{
    uint8_t seed[32];
    pz_sha256((const uint8_t *)label, strlen(label), seed);
    CHECK(keys_identity(ctx, seed, FP, bits, out));
}

/* the server side of one connection: accept, handshake, one frame of at most cap bytes, the answer "ok:" + frame */
typedef struct {
    net_cfg cfg;
    int lfd;
    size_t cap;
    bool hs_ok, got;
    uint8_t remote[32], hash[32];
    uint16_t port;
    size_t len;
    uint8_t body[300];
    int64_t took;
} server;

static void *serve(void *arg)
{
    server *s = arg;
    net_addr from;
    const int64_t t0 = ms();
    const int fd = net_accept(s->lfd, 3000, &from);
    if (fd < 0) return NULL;
    net_conn *c = net_handshake(&s->cfg, fd, &from);
    s->hs_ok = c != NULL;
    if (c) {
        memcpy(s->remote, c->remote, 32);
        memcpy(s->hash, c->hash, 32);
        s->port = c->addr.port;
        s->got = net_recv(c, s->body, s->cap, &s->len);
        if (s->got) {
            uint8_t ans[310] = "ok:";
            memcpy(ans + 3, s->body, s->len);
            (void)net_send(c, ans, 3 + s->len);
        }
    }
    s->took = ms() - t0;
    net_close(c);
    return NULL;
}

static void start(server *s, pthread_t *th, const home_identity *id, const uint8_t fp[32], unsigned bits, net_addr *addr)
{
    uint16_t port = 0;
    memset(s, 0, sizeof *s);
    net_cfg_init(&s->cfg, ctx, id, fp, bits, 0, SHORT_MS * 3);
    s->cap = 100;
    s->lfd = net_listen("127.0.0.1", 0, &port);
    CHECK(s->lfd >= 0 && port != 0);
    CHECK(net_parse_addr("127.0.0.1:1", addr));
    addr->port = port;
    CHECK(pthread_create(th, NULL, serve, s) == 0);
}

static void finish(server *s, pthread_t th)
{
    pthread_join(th, NULL);
    net_close_fd(s->lfd);
}

static void test_addr(void)
{
    net_addr a;
    char str[64];
    CHECK(net_parse_addr("127.0.0.1:8333", &a) && a.port == 8333 && a.ip[10] == 0xff && a.ip[12] == 127 && a.ip[15] == 1);
    net_addr_str(&a, str, sizeof str);
    CHECK(!strcmp(str, "127.0.0.1:8333"));
    CHECK(net_parse_addr("[::1]:65535", &a) && a.port == 65535 && a.ip[15] == 1 && a.ip[10] == 0);
    net_addr_str(&a, str, sizeof str);
    CHECK(!strcmp(str, "[::1]:65535"));
    CHECK(net_parse_addr("[2001:db8::7]:1", &a) && a.ip[0] == 0x20 && a.ip[15] == 7);
    CHECK(net_parse_addr("localhost:9", &a) && a.port == 9);
    CHECK(!net_parse_addr("127.0.0.1", &a) && !net_parse_addr("127.0.0.1:0", &a) && !net_parse_addr("127.0.0.1:65536", &a));
    CHECK(!net_parse_addr("::1:80", &a) && !net_parse_addr("[::1]80", &a) && !net_parse_addr(":80", &a) && !net_parse_addr("1.2.3.4:8x", &a));
    uint8_t r1[32], r2[32];
    CHECK(net_random(r1, 32) && net_random(r2, 32) && memcmp(r1, r2, 32) != 0);
}

static void test_exchange(void)
{
    server s;
    pthread_t th;
    net_addr addr;
    net_cfg cli;
    start(&s, &th, &idb, FP, BITS, &addr);
    net_cfg_init(&cli, ctx, &ida, FP, BITS, 4242, SHORT_MS * 3);
    uint8_t ans[200], remote[32];
    size_t n;
    CHECK(net_exchange(&cli, &addr, idb.xonly, (const uint8_t *)"hello", 5, ans, sizeof ans, &n, remote));
    CHECK(n == 8 && !memcmp(ans, "ok:hello", 8) && !memcmp(remote, idb.xonly, 32));
    finish(&s, th);
    CHECK(s.hs_ok && s.got && !memcmp(s.remote, ida.xonly, 32) && s.port == 4242);   /* the responder learnt who and which port */

    /* the same with an open connection: frames both ways, the same handshake hash, the identity and the ID of the other side */
    start(&s, &th, &idb, FP, BITS, &addr);
    net_conn *c = net_dial(&cli, &addr, NULL);
    uint8_t hash[32] = {0};
    CHECK(c != NULL);
    if (c) {
        uint8_t id[32];
        keys_node_id(idb.xonly, id);
        CHECK(!memcmp(c->remote, idb.xonly, 32) && !memcmp(c->remote_id, id, 32) && c->addr.port == addr.port);
        CHECK(net_send(c, NULL, 0) && net_recv(c, ans, sizeof ans, &n) && n == 3);
        memcpy(hash, c->hash, 32);
        net_close(c);
    }
    finish(&s, th);
    CHECK(s.hs_ok && s.got && s.len == 0 && !memcmp(s.hash, hash, 32));
}

/* an exchange that must fail, refused by the side that checks (the initiator sees the responder's key first: when the responder refuses,
 * the initiator learns it when the connection closes); neither side may wait long */
static void refused(const home_identity *srv_id, const uint8_t *srv_fp, const home_identity *cli_id, const uint8_t *expect, int line)
{
    server s;
    pthread_t th;
    net_addr addr;
    net_cfg cli;
    uint8_t ans[64];
    size_t n;
    start(&s, &th, srv_id, srv_fp, BITS, &addr);
    net_cfg_init(&cli, ctx, cli_id, FP, BITS, 0, SHORT_MS * 3);
    const int64_t t0 = ms();
    const bool ok = net_exchange(&cli, &addr, expect, (const uint8_t *)"x", 1, ans, sizeof ans, &n, NULL);
    const int64_t took = ms() - t0;
    finish(&s, th);
    if (ok || s.hs_ok || took > 2000 || s.took > 2000) printf("FAIL %s:%d (refused, from line %d)\n", __FILE__, __LINE__, line), fails++;
}

static void test_refusals(void)
{
    uint8_t fp2[32];
    memcpy(fp2, FP, 32);
    fp2[31] ^= 1;
    refused(&idb, fp2, &ida, NULL, __LINE__);                 /* another network: the prologue differs */
    refused(&idb, FP, &ida, ida.xonly, __LINE__);             /* the responder is not the identity that was dialled */
    refused(&nobits, FP, &ida, NULL, __LINE__);               /* the responder's ID has not the zero bits */
    refused(&idb, FP, &nobits, NULL, __LINE__);               /* the initiator's ID has not the zero bits */

    /* frames: one longer than the server takes, one changed on the way */
    for (int kind = 0; kind < 2; kind++) {
        server s;
        pthread_t th;
        net_addr addr;
        net_cfg cli;
        start(&s, &th, &idb, FP, BITS, &addr);
        net_cfg_init(&cli, ctx, &ida, FP, BITS, 0, SHORT_MS * 3);
        net_conn *c = net_dial(&cli, &addr, idb.xonly);
        CHECK(c != NULL);
        if (c) {
            uint8_t big[200] = {0}, ans[16];
            size_t n;
            if (kind == 0) {
                CHECK(net_send(c, big, sizeof big));          /* the server takes 100 bytes at most */
            } else {
                uint8_t f[NOISE_HEAD + 10 + NOISE_TAG];
                const size_t fl = noise_frame_seal(&c->tx, big, 10, f);
                f[NOISE_HEAD + 3] ^= 0x20;
                CHECK(write(c->fd, f, fl) == (ssize_t)fl);
            }
            CHECK(!net_recv(c, ans, sizeof ans, &n));         /* no answer: the server closed */
            net_close(c);
        }
        finish(&s, th);
        CHECK(s.hs_ok && !s.got);
    }
    uint8_t buf[NOISE_FRAME_MAX + 1];
    net_conn fake;
    CHECK(!net_send(&fake, buf, sizeof buf));                  /* longer than a frame: refused before anything is written */
}

static int raw_connect(const net_addr *a)
{
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_port = htons(a->port);
    memcpy(&s.sin_addr, a->ip + 12, 4);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (const struct sockaddr *)&s, sizeof s) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_timeouts(void)
{
    /* a client that connects and says nothing, one that sends bytes that are not a handshake: the server gives up within its timeout */
    for (int kind = 0; kind < 2; kind++) {
        server s;
        pthread_t th;
        net_addr addr;
        start(&s, &th, &idb, FP, BITS, &addr);
        const int fd = raw_connect(&addr);
        CHECK(fd >= 0);
        if (kind == 1 && fd >= 0) {
            uint8_t junk[NET_HS1 + NET_HS3];
            memset(junk, 0x41, sizeof junk);
            junk[0] = 2;
            CHECK(write(fd, junk, sizeof junk) == (ssize_t)sizeof junk);
        }
        finish(&s, th);
        CHECK(!s.hs_ok && s.took < 3 * SHORT_MS + 1500);
        if (fd >= 0) close(fd);
    }
    /* a server that listens and never answers (the kernel accepts the connection): the dialler gives up within its timeout */
    uint16_t port;
    const int lfd = net_listen("127.0.0.1", 0, &port);
    net_addr addr;
    net_cfg cli;
    CHECK(lfd >= 0 && net_parse_addr("127.0.0.1:1", &addr));
    addr.port = port;
    net_cfg_init(&cli, ctx, &ida, FP, BITS, 0, SHORT_MS);
    int64_t t0 = ms();
    CHECK(net_dial(&cli, &addr, NULL) == NULL);
    CHECK(ms() - t0 >= SHORT_MS - 50 && ms() - t0 < SHORT_MS + 1000);
    net_close_fd(lfd);
    /* nobody listens: refused at once */
    t0 = ms();
    CHECK(net_dial(&cli, &addr, NULL) == NULL && ms() - t0 < 1000);
    /* no accept within the time: -1 */
    const int l2 = net_listen("127.0.0.1", 0, &port);
    CHECK(l2 >= 0 && net_accept(l2, 50, &addr) == -1);
    net_close_fd(l2);
}

static void test_ipv6(void)
{
    uint16_t port;
    const int lfd = net_listen("::1", 0, &port);
    if (lfd < 0) {
        printf("t_net: no IPv6 on this machine, its test skipped\n");
        return;
    }
    net_close_fd(lfd);
    server s;
    pthread_t th;
    memset(&s, 0, sizeof s);
    net_cfg_init(&s.cfg, ctx, &idb, FP, BITS, 0, SHORT_MS * 3);
    s.cap = 100;
    s.lfd = net_listen("::1", 0, &port);
    net_addr addr;
    CHECK(s.lfd >= 0 && net_parse_addr("[::1]:1", &addr));
    addr.port = port;
    CHECK(pthread_create(&th, NULL, serve, &s) == 0);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &ida, FP, BITS, 7, SHORT_MS * 3);
    uint8_t ans[64];
    size_t n;
    CHECK(net_exchange(&cli, &addr, idb.xonly, (const uint8_t *)"six", 3, ans, sizeof ans, &n, NULL) && n == 6);
    finish(&s, th);
    CHECK(s.hs_ok && s.port == 7);
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    identity("net a", BITS, &ida);
    identity("net b", BITS, &idb);
    for (int i = 0;; i++) {                                    /* the first identity of 0 bits whose ID lacks BITS of them */
        char label[32];
        snprintf(label, sizeof label, "no bits %d", i);
        identity(label, 0, &nobits);
        if (!keys_id_ok(nobits.id, BITS)) break;
    }
    test_addr();
    test_exchange();
    test_refusals();
    test_timeouts();
    test_ipv6();
    secp256k1_context_destroy(ctx);
    printf(fails ? "t_net: %d FAILED\n" : "t_net: OK\n", fails);
    return fails != 0;
}
