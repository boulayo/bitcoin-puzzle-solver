/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_net.c - the channel of net.c over 127.0.0.1: a handshake and an exchange, then what must be refused (another network, a responder
 * that is not the expected identity, an ID without the zero bits on either side, a malformed or oversized frame, bytes that are not a
 * handshake) and what must not hang (a peer that says nothing, on both sides, or one that drips its handshake). The addresses a public
 * network refuses, and their blocks. A relay: two connections spliced, a handshake end to end over them that checks both identities,
 * megabytes both ways, and the limit of bytes. Identities with few zero bits, so that they are quick. */
#include "net.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BITS 4
#define SHORT_MS 300               /* the timeout of the tests that wait for nothing */

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0x5e, 0x70};
static home_identity ida, idb, idc, nobits;   /* nobits: an identity whose node ID lacks the BITS zero bits */

static int64_t ms(void) { return sys_now_ms(); }

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
    CHECK(sys_random(r1, 32) && sys_random(r2, 32) && memcmp(r1, r2, 32) != 0);
    /* the addresses a public network refuses (dialled from a NODES answer they would reach the user's own network) */
    static const char *const NOT_PUBLIC[] = {"0.1.2.3:1", "10.0.0.1:1", "100.64.0.1:1", "100.127.255.255:1", "127.0.0.1:1", "169.254.1.1:1",
        "172.16.0.1:1", "172.31.9.9:1", "192.168.1.1:1", "192.0.2.5:1", "198.18.0.1:1", "198.51.100.7:1", "203.0.113.9:1", "224.0.0.1:1",
        "255.255.255.255:1", "[::]:1", "[::1]:1", "[fc00::1]:1", "[fd12::1]:1", "[fe80::1]:1", "[ff02::1]:1", "[2001:db8::1]:1",
        "[::1.2.3.4]:1", "[100::1]:1", "[fec0::1]:1"};
    static const char *const PUBLIC[] = {"1.2.3.4:1", "8.8.8.8:53", "100.63.0.1:1", "100.128.0.1:1", "172.32.0.1:1", "192.0.1.1:1",
        "198.20.0.1:1", "223.255.255.1:1", "[2001:4860::8888]:1", "[2a00:1450::1]:1", "[::ffff:9.9.9.9]:1"};
    for (size_t i = 0; i < sizeof NOT_PUBLIC / sizeof *NOT_PUBLIC; i++)
        if (!net_parse_addr(NOT_PUBLIC[i], &a) || net_addr_public(&a)) printf("FAIL %s:%d: %s is public\n", __FILE__, __LINE__, NOT_PUBLIC[i]), fails++;
    for (size_t i = 0; i < sizeof PUBLIC / sizeof *PUBLIC; i++)
        if (!net_parse_addr(PUBLIC[i], &a) || !net_addr_public(&a)) printf("FAIL %s:%d: %s is not public\n", __FILE__, __LINE__, PUBLIC[i]), fails++;
    /* blocks: the /24 of IPv4, the /48 of IPv6, the whole address otherwise; never the port */
    net_addr b;
    uint8_t ka[16], kb[16];
    CHECK(net_parse_addr("1.2.3.4:1", &a) && net_parse_addr("1.2.3.200:9", &b));
    net_addr_block(&a, ka), net_addr_block(&b, kb);
    CHECK(!memcmp(ka, kb, 16));
    CHECK(net_parse_addr("1.2.4.4:1", &b));
    net_addr_block(&b, kb);
    CHECK(memcmp(ka, kb, 16) != 0);
    CHECK(net_parse_addr("[2a00:1450:4001:1::1]:1", &a) && net_parse_addr("[2a00:1450:4001:ffff::2]:2", &b));
    net_addr_block(&a, ka), net_addr_block(&b, kb);
    CHECK(!memcmp(ka, kb, 16));
    CHECK(net_parse_addr("[2a00:1450:4002::1]:1", &b));
    net_addr_block(&b, kb);
    CHECK(memcmp(ka, kb, 16) != 0);
    CHECK(net_parse_addr("127.0.0.1:1", &a) && net_parse_addr("127.0.0.2:1", &b));
    net_addr_block(&a, ka), net_addr_block(&b, kb);
    CHECK(memcmp(ka, kb, 16) != 0);
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
                CHECK(sys_send_all(c->fd, f, fl, ms() + 3000));
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
    return sys_connect((const struct sockaddr *)&s, sizeof s, ms() + 3000);
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
            CHECK(sys_send_all(fd, junk, sizeof junk, ms() + 3000));
        }
        finish(&s, th);
        CHECK(!s.hs_ok && s.took < 3 * SHORT_MS + 1500);
        sys_close(fd);
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

/* An initiator that sends each handshake message just within one timeout of the last: before, each message had a timeout of its own and
 * this held a handler for about three; now the whole handshake has one. */
static void test_drip(void)
{
    server s;
    pthread_t th;
    net_addr addr;
    start(&s, &th, &idb, FP, BITS, &addr);                     /* the server's timeout: 3 * SHORT_MS */
    const int fd = raw_connect(&addr);
    CHECK(fd >= 0);
    noise_hs hs;
    uint8_t e[32], m[NOISE_HS_MAX], pl[NOISE_PAYLOAD_MAX], prologue[38], port[2] = {0, 0};
    size_t n, plen;
    memcpy(prologue, "pznet1", 6), memcpy(prologue + 6, FP, 32);
    memset(e, 7, sizeof e);
    CHECK(noise_hs_init(&hs, ctx, true, prologue, sizeof prologue, ida.sec, e));
    sys_sleep_ms(2 * SHORT_MS);
    n = noise_hs_write(&hs, NULL, 0, m);
    CHECK(fd >= 0 && sys_send_all(fd, m, n, ms() + 3000) && sys_recv_all(fd, m, NET_HS2, ms() + 3000) && noise_hs_read(&hs, m, NET_HS2, pl, &plen));
    sys_sleep_ms(2 * SHORT_MS);                                /* 4 * SHORT_MS since the connection: past the one deadline */
    n = noise_hs_write(&hs, port, 2, m);
    (void)sys_send_all(fd, m, n, ms() + 3000);
    finish(&s, th);
    CHECK(!s.hs_ok && s.took < 4 * SHORT_MS + 500);
    sys_close(fd);
}

/* ---- a relay: R accepts two connections, reads a join from each, says go to both and splices them ---- */

typedef struct {
    net_cfg cfg;
    int lfd;
    uint64_t max, moved;
} relay_srv;

static void *relay_main(void *arg)
{
    relay_srv *r = arg;
    net_conn *c[2] = {NULL, NULL};
    for (int i = 0; i < 2; i++) {
        net_addr from;
        uint8_t m[16];
        size_t n;
        const int fd = net_accept(r->lfd, 3000, &from);
        c[i] = fd >= 0 ? net_handshake(&r->cfg, fd, &from) : NULL;
        if (!c[i] || !net_recv(c[i], m, sizeof m, &n) || n != 4 || memcmp(m, "join", 4)) break;
    }
    if (c[0] && c[1] && net_send(c[0], (const uint8_t *)"go", 2) && net_send(c[1], (const uint8_t *)"go", 2)) {
        r->moved = net_splice(c[0], c[1], r->max, 10000, NULL);
    } else {
        net_close(c[0]);
        net_close(c[1]);
    }
    return NULL;
}

typedef struct {
    const home_identity *id;
    const uint8_t *expect;          /* the partner it must find at the other end */
    bool initiator, hs_ok;
    net_addr r;
    int frames;                     /* frames of 60 000 bytes the initiator sends and the other side sends back */
    int echoed;
} relay_home;

static void *relay_home_main(void *arg)
{
    relay_home *h = arg;
    net_cfg cfg;
    net_cfg_init(&cfg, ctx, h->id, FP, BITS, 0, 2000);
    net_conn *c = net_dial(&cfg, &h->r, idb.xonly);
    uint8_t m[16];
    size_t n;
    if (!c || !net_send(c, (const uint8_t *)"join", 4) || !net_recv(c, m, sizeof m, &n) || n != 2) {
        net_close(c);
        return NULL;
    }
    h->hs_ok = net_rehandshake(&cfg, c, h->initiator, h->expect);
    uint8_t *big = malloc(60000), *got = malloc(60000);
    for (int i = 0; h->hs_ok && big && got && i < h->frames; i++) {
        memset(big, i, 60000);
        size_t l;
        if (h->initiator) {
            if (!net_send(c, big, 60000) || !net_recv(c, got, 60000, &l) || l != 60000 || memcmp(got, big, 60000)) break;
        } else if (!net_recv(c, got, 60000, &l) || l != 60000 || got[0] != (uint8_t)i || !net_send(c, got, l)) {
            break;
        }
        h->echoed++;
    }
    free(big);
    free(got);
    net_close(c);
    return NULL;
}

/* two homes through a relay R (idb): ida initiates, idc answers; expect_a is the identity ida must find at the other end */
static void relay_run(uint64_t max, const uint8_t *expect_a, int frames, relay_home *a, relay_home *c, relay_srv *r)
{
    pthread_t tr, ta, tc;
    uint16_t port;
    memset(r, 0, sizeof *r);
    net_cfg_init(&r->cfg, ctx, &idb, FP, BITS, 0, 2000);
    r->max = max;
    r->lfd = net_listen("127.0.0.1", 0, &port);
    CHECK(r->lfd >= 0);
    *a = (relay_home){.id = &ida, .expect = expect_a, .initiator = true, .frames = frames};
    *c = (relay_home){.id = &idc, .expect = ida.xonly, .initiator = false, .frames = frames};
    CHECK(net_parse_addr("127.0.0.1:1", &a->r));
    a->r.port = port;
    c->r = a->r;
    CHECK(pthread_create(&tr, NULL, relay_main, r) == 0);
    CHECK(pthread_create(&ta, NULL, relay_home_main, a) == 0);
    CHECK(pthread_create(&tc, NULL, relay_home_main, c) == 0);
    pthread_join(ta, NULL);
    pthread_join(tc, NULL);
    pthread_join(tr, NULL);
    net_close_fd(r->lfd);
}

static void test_relay(void)
{
    relay_home a, c;
    relay_srv r;
    /* 50 frames of 60 000 bytes each way, end to end: 6 MB through R, which only saw the ciphertext of a handshake it is not part of */
    relay_run(64u << 20, idc.xonly, 50, &a, &c, &r);
    CHECK(a.hs_ok && c.hs_ok && a.echoed == 50 && c.echoed == 50);
    CHECK(r.moved > 2 * 50 * 60000ull && r.moved < 2 * 50 * 60100ull + 1000);
    /* the relay's limit of bytes: the copy stops, and the homes see the end of the channel */
    relay_run(1u << 20, idc.xonly, 50, &a, &c, &r);
    CHECK(a.hs_ok && c.hs_ok && a.echoed < 10 && r.moved <= 1u << 20);
    /* the initiator expects another identity than the one at the other end (a relay that splices it to someone else): refused before
     * it shows its own key, and the other side sees the end */
    relay_run(64u << 20, idb.xonly, 1, &a, &c, &r);
    CHECK(!a.hs_ok && !c.hs_ok && a.echoed == 0);
    /* the responder checks the initiator too */
    pthread_t tr, ta, tc;
    uint16_t port;
    memset(&r, 0, sizeof r);
    net_cfg_init(&r.cfg, ctx, &idb, FP, BITS, 0, 2000);
    r.max = 1u << 20;
    r.lfd = net_listen("127.0.0.1", 0, &port);
    a = (relay_home){.id = &ida, .expect = idc.xonly, .initiator = true, .frames = 1};
    c = (relay_home){.id = &idc, .expect = idb.xonly, .initiator = false, .frames = 1};
    CHECK(r.lfd >= 0 && net_parse_addr("127.0.0.1:1", &a.r));
    a.r.port = port, c.r = a.r;
    CHECK(pthread_create(&tr, NULL, relay_main, &r) == 0 && pthread_create(&ta, NULL, relay_home_main, &a) == 0 &&
          pthread_create(&tc, NULL, relay_home_main, &c) == 0);
    pthread_join(ta, NULL), pthread_join(tc, NULL), pthread_join(tr, NULL);
    net_close_fd(r.lfd);
    CHECK(!c.hs_ok && a.echoed == 0);
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
    identity("net c", BITS, &idc);
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
    test_drip();
    test_relay();
    test_ipv6();
    secp256k1_context_destroy(ctx);
    printf(fails ? "t_net: %d FAILED\n" : "t_net: OK\n", fails);
    return fails != 0;
}
