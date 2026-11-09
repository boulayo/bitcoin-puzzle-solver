/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_bt.c - the rendezvous in the table of BitTorrent, against a table of 40 nodes that run here (UDP on the loopback): each knows the
 * others, answers get_peers with the 8 closest to the key, a token and the peers it holds, and takes announce_peer with the right token.
 * Two clients find each other through it; then with nodes that lie close to the key (random bytes, answers under another number, nodes
 * that do not exist, peers without end), where the clients still meet through the honest nodes that answer; then with no table at all;
 * and the reader of bencoding on damaged input. */
#include "peertest.h"
#include "bt.h"
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

#define NODES 40
enum { HONEST, GARBAGE, WRONG_TID, GHOSTS, FLOOD };

typedef struct {
    int fd, kind;
    uint16_t port;                 /* network order */
    uint8_t id[20];
    uint8_t peers[16][6];
    int npeers;
    pthread_t th;
} fake;

static fake F[NODES];
static uint8_t KEY[20];
static atomic_bool g_stop;

static bool closer(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 20; i++)
        if ((a[i] ^ KEY[i]) != (b[i] ^ KEY[i])) return (a[i] ^ KEY[i]) < (b[i] ^ KEY[i]);
    return false;
}

static void token_for(const fake *f, const struct sockaddr_in *from, uint8_t t[4])
{
    memcpy(t, &from->sin_addr, 4);
    t[0] ^= f->id[0], t[1] ^= f->id[1], t[2] ^= (uint8_t)from->sin_port, t[3] ^= (uint8_t)(from->sin_port >> 8);
}

static void *fake_main(void *arg)
{
    fake *f = arg;
    while (!atomic_load(&g_stop)) {
        struct pollfd p = {.fd = f->fd, .events = POLLIN};
        uint8_t m[1024], out[4096], tok[4];
        const uint8_t *a, *v, *s, *tid, *q;
        size_t an, vn, sn, tn, qn, l = 0;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        if (poll(&p, 1, 100) <= 0) continue;
        const ssize_t n = recvfrom(f->fd, m, sizeof m, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0 || !bt_dict_get(m, (size_t)n, "a", &a, &an) || !bt_dict_get(m, (size_t)n, "t", &v, &vn) || !bt_string(v, vn, &tid, &tn) ||
            tn != 2 || !bt_dict_get(m, (size_t)n, "q", &v, &vn) || !bt_string(v, vn, &q, &qn))
            continue;
        token_for(f, &from, tok);
        if (f->kind == GARBAGE) {
            for (size_t i = 0; i < 200; i++) out[i] = (uint8_t)rand();
            (void)sendto(f->fd, out, 200, 0, (struct sockaddr *)&from, fl);
            continue;
        }
        l += (size_t)sprintf((char *)out + l, "d1:rd2:id20:");
        memcpy(out + l, f->id, 20), l += 20;
        if (qn == 13 && !memcmp(q, "announce_peer", 13)) {
            const uint8_t *e;
            if (bt_dict_get(a, an, "token", &v, &vn) && bt_string(v, vn, &s, &sn) && sn == 4 && !memcmp(s, tok, 4) &&
                bt_dict_get(a, an, "port", &v, &vn) && v[0] == 'i' && (e = memchr(v, 'e', vn)) != NULL && f->npeers < 16) {
                const unsigned port = (unsigned)atoi((const char *)v + 1);
                memcpy(f->peers[f->npeers], &from.sin_addr, 4);
                f->peers[f->npeers][4] = (uint8_t)(port >> 8), f->peers[f->npeers][5] = (uint8_t)port;
                f->npeers++;
            } else continue;                                   /* a wrong token: no answer */
        } else {
            int best[8], nb = 0;
            for (int i = 0; i < NODES; i++) {
                int at = 7;
                if (nb < 8) at = nb++;
                else if (!closer(F[i].id, F[best[7]].id)) continue;
                best[at] = i;
                for (; at > 0 && closer(F[best[at]].id, F[best[at - 1]].id); at--) {
                    const int t = best[at];
                    best[at] = best[at - 1], best[at - 1] = t;
                }
            }
            l += (size_t)sprintf((char *)out + l, "5:nodes%d:", (f->kind == GHOSTS ? 60 : 8) * 26);
            for (int i = 0; i < 8 && f->kind != GHOSTS; i++) {
                memcpy(out + l, F[best[i]].id, 20), l += 20;
                memcpy(out + l, "\x7f\0\0\1", 4), l += 4;
                memcpy(out + l, &F[best[i]].port, 2), l += 2;
            }
            for (int i = 0; i < 60 && f->kind == GHOSTS; i++) {  /* nodes very close to the key, at addresses where nobody answers */
                memcpy(out + l, KEY, 20), out[l + 19] ^= (uint8_t)(i + 1), l += 20;
                memcpy(out + l, "\x7f\0\0\1\0", 5), out[l + 5] = (uint8_t)(i + 1), l += 6;
            }
            l += (size_t)sprintf((char *)out + l, "5:token4:");
            memcpy(out + l, tok, 4), l += 4;
            const int np = f->kind == FLOOD ? 300 : f->npeers;
            if (np) {
                l += (size_t)sprintf((char *)out + l, "6:valuesl");
                for (int i = 0; i < np; i++) {
                    l += (size_t)sprintf((char *)out + l, "6:");
                    if (f->kind == FLOOD) out[l] = 10, out[l + 1] = (uint8_t)(i >> 8), out[l + 2] = (uint8_t)i, out[l + 3] = 1, out[l + 4] = 9, out[l + 5] = 9;
                    else memcpy(out + l, f->peers[i], 6);
                    l += 6;
                }
                out[l++] = 'e';
            }
        }
        l += (size_t)sprintf((char *)out + l, "e1:t2:");
        out[l++] = tid[0], out[l++] = (uint8_t)(tid[1] ^ (f->kind == WRONG_TID));
        l += (size_t)sprintf((char *)out + l, "1:y1:re");
        (void)sendto(f->fd, out, l, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

static void table_start(void)
{
    atomic_store(&g_stop, false);
    for (int i = 0; i < NODES; i++) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t al = sizeof a;
        F[i] = (fake){.fd = socket(AF_INET, SOCK_DGRAM, 0)};
        if (F[i].fd < 0 || bind(F[i].fd, (struct sockaddr *)&a, sizeof a) || getsockname(F[i].fd, (struct sockaddr *)&a, &al)) exit(1);
        F[i].port = a.sin_port;
        for (int j = 0; j < 20; j++) F[i].id[j] = (uint8_t)rand();
    }
}

static void table_run(void)
{
    for (int i = 0; i < NODES; i++) pthread_create(&F[i].th, NULL, fake_main, &F[i]);
}

static void table_stop(void)
{
    atomic_store(&g_stop, true);
    for (int i = 0; i < NODES; i++) pthread_join(F[i].th, NULL), close(F[i].fd);
}

static bool has(const net_addr *p, int n, uint16_t port)
{
    for (int i = 0; i < n; i++)
        if (p[i].port == port && p[i].ip[12] == 127 && p[i].ip[15] == 1) return true;
    return false;
}

/* min_took: the nodes that must take the announce (the honest ones among the closest that answer) */
static void test_rendezvous(int liars, int min_took)
{
    char router[32], router2[32];
    const char *routers[2] = {router, router2};
    net_addr peers[BT_PEERS_MAX];
    int took = 0;
    table_start();
    /* the liars are the nodes closest to the key but one in two, and never the routers */
    for (int k = 0; k < liars; k++) F[2 + k].kind = 1 + k % 4;
    for (int k = 0; k < liars; k++)
        if (F[2 + k].kind != GHOSTS) memcpy(F[2 + k].id, KEY, 19);
    table_run();
    snprintf(router, sizeof router, "127.0.0.1:%u", ntohs(F[0].port));
    snprintf(router2, sizeof router2, "127.0.0.1:%u", ntohs(F[1].port));
    bt_config a = {.port = 1111, .routers = routers, .nrouters = 2, .budget_ms = 6000, .wave_ms = 300}, b = a;
    memcpy(a.infohash, KEY, 20), memcpy(b.infohash, KEY, 20);
    b.port = 2222;
    CHECK(bt_rendezvous(&a, peers, BT_PEERS_MAX, &took) >= 0 && !has(peers, BT_PEERS_MAX, 1111));   /* nobody there yet */
    CHECK(took >= min_took);
    int n = bt_rendezvous(&b, peers, BT_PEERS_MAX, &took);
    CHECK(n >= 1 && n <= BT_PEERS_MAX && has(peers, n, 1111) && took >= min_took);
    n = bt_rendezvous(&a, peers, BT_PEERS_MAX, NULL);
    CHECK(n >= 2 && has(peers, n, 2222));
    b.port = 0;                                                /* a node that does not listen looks, and announces nothing */
    n = bt_rendezvous(&b, peers, 1, &took);
    CHECK(n == 1 && took == 0);
    table_stop();
    int stored = 0;
    for (int i = 0; i < NODES; i++) stored += F[i].npeers;
    CHECK(stored >= 2 * min_took && stored <= 3 * 8);
}

static void test_alone(void)
{
    const char *routers[] = {"127.0.0.1:9", "no.such.host.invalid:6881", "127.0.0.1"};
    net_addr peers[4];
    bt_config c = {.port = 1, .routers = routers, .nrouters = 3, .budget_ms = 1500, .wave_ms = 300};
    CHECK(bt_rendezvous(&c, peers, 4, NULL) == -1);
    c.nrouters = 0;
    CHECK(bt_rendezvous(&c, peers, 4, NULL) == -1);
}

static void test_bencode(void)
{
    const char *good = "d1:rd2:id2:ab5:nodes0:6:valuesl1:x2:yzee1:t2:aa1:y1:re";
    const uint8_t *g = (const uint8_t *)good, *v, *s;
    size_t vn, sn;
    const size_t gl = strlen(good);
    CHECK(bt_dict_get(g, gl, "t", &v, &vn) && bt_string(v, vn, &s, &sn) && sn == 2 && !memcmp(s, "aa", 2));
    CHECK(bt_dict_get(g, gl, "r", &v, &vn) && bt_dict_get(v, vn, "id", &v, &vn) && bt_string(v, vn, &s, &sn) && sn == 2);
    CHECK(!bt_dict_get(g, gl, "q", &v, &vn) && !bt_dict_get(g, gl - 1, "t", &v, &vn) && !bt_string(g, gl, &s, &sn));
    CHECK(!bt_dict_get((const uint8_t *)"d3:abce", 7, "abc", &v, &vn));                     /* a key without a value */
    CHECK(!bt_dict_get((const uint8_t *)"d999999999:ae", 13, "a", &v, &vn));                /* a length past the end */
    CHECK(!bt_dict_get((const uint8_t *)"dllllllllllleeeeeeeeeee1:ae", 27, "a", &v, &vn));  /* too deep, and not a key */
    for (int round = 0; round < 200000; round++) {             /* damaged copies: read within their bounds, whatever they say */
        uint8_t m[64];
        const size_t l = 1 + (size_t)rand() % gl;
        memcpy(m, good, l);
        for (int k = rand() % 4; k > 0; k--) m[(size_t)rand() % l] = (uint8_t)rand();
        uint8_t *exact = malloc(l);                            /* its own block: a read past the end is seen by the sanitizer */
        memcpy(exact, m, l);
        if (bt_dict_get(exact, l, "r", &v, &vn)) {
            CHECK(v >= exact && v + vn <= exact + l);
            if (bt_dict_get(v, vn, "values", &v, &vn)) CHECK(v + vn <= exact + l);
        }
        if (bt_dict_get(exact, l, "t", &v, &vn) && bt_string(v, vn, &s, &sn)) CHECK(s >= exact && s + sn <= exact + l);
        free(exact);
    }
}

int main(void)
{
    srand(7);
    for (int j = 0; j < 20; j++) KEY[j] = (uint8_t)rand();
    test_bencode();
    test_rendezvous(0, 8);
    test_rendezvous(6, 4);     /* six liars among the eight closest to the key */
    test_rendezvous(12, 2);    /* every node close to the key lies: the two routers are the only honest ones a lookup meets */
    test_alone();
    TEST_DONE("t_bt");
}
