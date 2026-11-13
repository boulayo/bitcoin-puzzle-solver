/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_portmap.c - the request to open a port, against a router that runs here. One that speaks PCP, one that only speaks NAT-PMP, one
 * whose PCP offers another port and that then takes UPnP. Then routers that refuse PCP and speak UPnP: one that answers the search, serves
 * its description and takes AddPortMapping; and those that are not right: one that only takes mappings without end, one that refuses, one
 * whose description is elsewhere, one with no service that opens ports, one that talks without end, and none at all. */
#include "peertest.h"
#include "portmap.h"
#include "sys.h"
#include <pthread.h>
#include <stdatomic.h>

enum { GOOD, PERMANENT_ONLY, REFUSES, ELSEWHERE, NO_SERVICE, ENDLESS, ABSOLUTE_CTL, PCP, NATPMP, PCP_OTHER_PORT };

static int g_udp, g_tcp;
static atomic_int g_kind, g_maps, g_pcp_maps;
static uint8_t g_pcp_req[60];      /* the last PCP request (under g_mu) */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   /* g_last */
static uint16_t g_port;            /* of the HTTP side */
static atomic_bool g_stop;
static char g_last[8192];          /* the last AddPortMapping taken */

static bool last_has(const char *text)
{
    pthread_mutex_lock(&g_mu);
    const bool has = strstr(g_last, text) != NULL;
    pthread_mutex_unlock(&g_mu);
    return has;
}

static void *ssdp_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        char m[1024], out[512];
        struct sockaddr_in from;
        if (sys_wait(g_udp, false, sys_now_ms() + 100) <= 0) continue;
        const long n = sys_recvfrom(g_udp, m, sizeof m - 1, &from);
        if (n <= 0) continue;
        m[n] = 0;
        uint8_t *q = (uint8_t *)m, r[60] = {0};
        if (n == 60 && q[0] == 2 && q[1] == 1) {               /* PCP MAP */
            const int k = atomic_load(&g_kind);
            pthread_mutex_lock(&g_mu);
            memcpy(g_pcp_req, q, 60);
            pthread_mutex_unlock(&g_mu);
            if (k == NATPMP) {                                 /* a version it does not know */
                const uint8_t v[8] = {0, 0x81, 0, 1};
                (void)sys_sendto(g_udp, v, sizeof v, &from);
                continue;
            }
            memcpy(r, q, 60), r[1] = 0x81, r[3] = k == PCP || k == PCP_OTHER_PORT ? 0 : 2;   /* 2: not authorized */
            if (k == PCP_OTHER_PORT) r[43]++;
            if (!r[3]) g_pcp_maps++;
            (void)sys_sendto(g_udp, r, 60, &from);
            continue;
        }
        if (n == 12 && q[0] == 0 && q[1] == 2) {               /* NAT-PMP, map TCP */
            r[1] = 0x82, memcpy(r + 8, q + 4, 8);              /* the ports and the lifetime asked */
            g_pcp_maps++;
            (void)sys_sendto(g_udp, r, 16, &from);
            continue;
        }
        if (!strstr(m, "M-SEARCH") || !strstr(m, "InternetGatewayDevice")) continue;
        const int l = snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                               "Location: http://%s:%u/desc.xml\r\n\r\n", g_kind == ELSEWHERE ? "192.0.2.7" : "127.0.0.1", g_port);
        (void)sys_sendto(g_udp, out, (size_t)l, &from);
    }
    return NULL;
}

static void *http_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        static char req[8192], out[8192];
        if (sys_wait(g_tcp, false, sys_now_ms() + 100) <= 0) continue;
        const int c = sys_accept(g_tcp, NULL, NULL);
        const int64_t until = sys_now_ms() + 5000;
        if (c < 0) continue;
        size_t n = 0;
        for (;;) {                                             /* the headers, then the body they announce */
            const long r = sys_recv(c, req + n, sizeof req - 1 - n, until);
            if (r <= 0) break;
            n += (size_t)r, req[n] = 0;
            const char *end = strstr(req, "\r\n\r\n"), *cl = strstr(req, "Content-Length:");
            if (end && (!cl || n >= (size_t)(end + 4 - req) + (size_t)atoi(cl + 15))) break;
        }
        const char *body = "";
        int status = 200;
        if (!strncmp(req, "GET /desc.xml ", 14)) {
            body = g_kind == NO_SERVICE ? "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
                                          "<controlURL>/l3</controlURL></service></serviceList></device></root>"
                 : "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
                   "<controlURL>/l3</controlURL></service></serviceList><deviceList><device><serviceList><service>\n"
                   "<serviceType> urn:schemas-upnp-org:service:WANIPConnection:1 </serviceType><controlURL>%s</controlURL>"
                   "</service></serviceList></device></deviceList></device></root>";
        } else if (!strncmp(req, "POST /ctl ", 10) && strstr(req, "SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"")) {
            const bool endless = strstr(req, "<NewLeaseDuration>0</NewLeaseDuration>") != NULL;
            if (g_kind == REFUSES) status = 500, body = "<errorCode>718</errorCode>";
            else if (g_kind == PERMANENT_ONLY && !endless) status = 500, body = "<errorCode>725</errorCode>";
            else {
                pthread_mutex_lock(&g_mu);
                snprintf(g_last, sizeof g_last, "%s", req);
                pthread_mutex_unlock(&g_mu);
                g_maps++;
            }
        } else status = 404;
        char ctl[64];
        snprintf(ctl, sizeof ctl, g_kind == ABSOLUTE_CTL ? "http://127.0.0.1:%u/ctl" : "ctl", g_port);
        static char text[2048];
        snprintf(text, sizeof text, body, ctl);
        const int l = snprintf(out, sizeof out, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", status,
                               status == 200 ? "OK" : "Error", strlen(text), text);
        (void)sys_send_all(c, out, (size_t)l, until);
        for (int i = 0; g_kind == ENDLESS && i < 400 && sys_send_all(c, out, sizeof out, until); i++) {}
        sys_close(c);
    }
    return NULL;
}

int main(void)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}, b = a;
    socklen_t al = sizeof a;
    char router[32], why[400];
    const uint8_t nonce[12] = "pzpeer nonce";
    pthread_t t1, t2;
    g_udp = sys_socket(AF_INET, SOCK_DGRAM), g_tcp = sys_socket(AF_INET, SOCK_STREAM);
    if (bind(g_udp, (struct sockaddr *)&a, sizeof a) || getsockname(g_udp, (struct sockaddr *)&a, &al)) return 1;
    if (bind(g_tcp, (struct sockaddr *)&b, sizeof b) || listen(g_tcp, 8) || getsockname(g_tcp, (struct sockaddr *)&b, &al)) return 1;
    g_port = ntohs(b.sin_port);
    snprintf(router, sizeof router, "127.0.0.1:%u", ntohs(a.sin_port));
    pthread_create(&t1, NULL, ssdp_main, NULL), pthread_create(&t2, NULL, http_main, NULL);

    g_kind = PCP;
    CHECK(portmap(4240, 7200, nonce, router, why, sizeof why) == 7200 && strstr(why, "(PCP)") && g_pcp_maps == 1 && g_maps == 0);
    pthread_mutex_lock(&g_mu);
    CHECK(g_pcp_req[0] == 2 && g_pcp_req[7] == (7200 & 0xff) && !memcmp(g_pcp_req + 16, "\0\0\xff\xff\x7f\0\0\1", 8) &&
          !memcmp(g_pcp_req + 24, nonce, 12) && g_pcp_req[36] == 6 && g_pcp_req[41] == (4240 & 0xff) && g_pcp_req[43] == (4240 & 0xff));
    pthread_mutex_unlock(&g_mu);
    g_kind = NATPMP;
    CHECK(portmap(4241, 3600, nonce, router, why, sizeof why) == 3600 && strstr(why, "(NAT-PMP)") && g_pcp_maps == 2 && g_maps == 0);
    g_kind = PCP_OTHER_PORT;                                   /* another port is no use: UPnP is asked */
    CHECK(portmap(4242, 7200, nonce, router, why, sizeof why) == 7200 && strstr(why, "(UPnP)") && g_pcp_maps == 3 && g_maps == 1);
    g_maps = 0;

    g_kind = GOOD;
    CHECK(portmap(4242, 7200, nonce, router, why, sizeof why) && g_maps == 1);
    CHECK(last_has("<NewExternalPort>4242</NewExternalPort>") && last_has("<NewInternalPort>4242</NewInternalPort>") &&
          last_has("<NewInternalClient>127.0.0.1</NewInternalClient>") && last_has("<NewProtocol>TCP</NewProtocol>") &&
          last_has("<NewLeaseDuration>7200</NewLeaseDuration>"));
    g_kind = ABSOLUTE_CTL;
    CHECK(portmap(4243, 7200, nonce, router, why, sizeof why) && g_maps == 2 && last_has("<NewExternalPort>4243<"));
    g_kind = PERMANENT_ONLY;
    CHECK(portmap(4244, 7200, nonce, router, why, sizeof why) && g_maps == 3 && last_has("<NewLeaseDuration>0<") && strstr(why, "without end"));
    g_kind = REFUSES;
    CHECK(!portmap(4245, 7200, nonce, router, why, sizeof why) && strstr(why, "refused to open"));
    g_kind = ELSEWHERE;                                        /* a description on another machine is not fetched */
    CHECK(!portmap(4245, 7200, nonce, router, why, sizeof why) && strstr(why, "no router"));
    g_kind = NO_SERVICE;
    CHECK(!portmap(4245, 7200, nonce, router, why, sizeof why) && strstr(why, "no service"));
    g_kind = ENDLESS;
    CHECK(!portmap(4245, 7200, nonce, router, why, sizeof why));
    CHECK(g_maps == 3);
    atomic_store(&g_stop, true);
    pthread_join(t1, NULL), pthread_join(t2, NULL);
    sys_close(g_udp), sys_close(g_tcp);
    CHECK(!portmap(4245, 7200, nonce, router, why, sizeof why) && strstr(why, "no router"));   /* nobody answers */
    CHECK(!portmap(4245, 7200, nonce, "[::1]:1900", why, sizeof why));
    TEST_DONE("t_portmap");
}
