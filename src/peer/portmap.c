/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* portmap.c - see portmap.h.
 * PCP and NAT-PMP are one datagram to port 5351 of the router of the default route and its answer, which counts only from there; a router
 * that only speaks NAT-PMP answers PCP with version 0 (RFC 6887 9), and is then asked in its way.
 * UPnP is three steps: a search on the LAN (SSDP, one UDP packet) that the router answers with the address of its description; the
 * description (XML over HTTP), where the control address of its WAN connection service is; the request AddPortMapping (SOAP over HTTP).
 * There the router is whoever answers on the LAN, so nothing it says is followed out of the LAN: its description and its control address
 * must be at the very address the answer came from, which must not be a public one. */
#include "portmap.h"
#include "net.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PCP_PORT 5351
#define TIMEOUT_S 3
#define BODY_MAX 262144            /* the longest description read */

static bool fail(char *why, size_t cap, const char *msg)
{
    snprintf(why, cap, "%s", msg);
    return false;
}

/* One HTTP exchange with the router: the request goes, the answer is read until the router closes or cap - 1 bytes came. The local address
 * of the connection into local (the address the router sees this machine at). */
static bool http(const struct sockaddr_in *to, const char *req, size_t rl, char *ans, size_t cap, struct in_addr *local)
{
    const int64_t deadline = sys_now_ms() + TIMEOUT_S * 1000;
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    size_t n = 0;
    long r = 1;
    const int fd = sys_connect((const struct sockaddr *)to, sizeof *to, deadline);
    bool ok = fd >= 0 && getsockname(fd, (struct sockaddr *)&me, &ml) == 0 && sys_send_all(fd, req, rl, deadline);
    while (ok && r > 0 && n < cap - 1)
        if ((r = sys_recv(fd, ans + n, cap - 1 - n, deadline)) > 0) n += (size_t)r;
    sys_close(fd);
    ans[n] = 0;
    if (ok) *local = me.sin_addr;
    return ok && r == 0 && n > 12;                             /* to the end, within the time and the buffer */
}

/* whether the n bytes at p hold word */
static bool holds(const char *p, size_t n, const char *word)
{
    const size_t wl = strlen(word);
    for (size_t i = 0; i + wl <= n; i++)
        if (!memcmp(p + i, word, wl)) return true;
    return false;
}

/* "http://a.b.c.d[:port]/path" at the address of `at` (and its port into it): the path, or NULL */
static const char *url_path(const char *url, size_t len, struct sockaddr_in *at)
{
    char host[64];
    unsigned port = 80;
    if (len < 8 || strncasecmp(url, "http://", 7)) return NULL;
    const char *h = url + 7, *end = url + len, *slash = memchr(h, '/', (size_t)(end - h));
    if (!slash || (size_t)(slash - h) >= sizeof host) return NULL;
    memcpy(host, h, (size_t)(slash - h)), host[slash - h] = 0;
    char *colon = strchr(host, ':');
    struct in_addr a;
    if (colon) {
        *colon = 0;
        if (sscanf(colon + 1, "%u", &port) != 1 || !port || port > 65535) return NULL;
    }
    if (inet_pton(AF_INET, host, &a) != 1 || a.s_addr != at->sin_addr.s_addr) return NULL;
    at->sin_port = htons((uint16_t)port);
    return slash;
}

/* the text between <tag> and </tag> from p on, before limit (NULL: the end) */
static const char *element(const char *p, const char *limit, const char *tag, size_t *len)
{
    char open[40], shut[40];
    snprintf(open, sizeof open, "<%s>", tag), snprintf(shut, sizeof shut, "</%s>", tag);
    const char *a = strstr(p, open);
    if (!a || (limit && a >= limit)) return NULL;
    a += strlen(open);
    const char *b = strstr(a, shut);
    if (!b || (limit && b > limit)) return NULL;
    while (a < b && (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\n' || b[-1] == '\r' || b[-1] == '\t')) b--;
    *len = (size_t)(b - a);
    return a;
}

static bool upnp(char *body, uint16_t port, unsigned lease_s, const char *ssdp, char *why, size_t cap)
{
    static const char SEARCH[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                 "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    char pkt[2048], req[2048], soap[1024], desc[512], type[128], ctl[256];
    net_addr to, seen = {.ip = {[10] = 0xff, [11] = 0xff}};
    struct sockaddr_in dst = {.sin_family = AF_INET}, from, router;
    struct in_addr local;
    const char *loc = NULL, *path = NULL;
    size_t ll = 0;

    /* 1. the search */
    if (!net_parse_addr(ssdp ? ssdp : "239.255.255.250:1900", &to) || to.ip[10] != 0xff) return fail(why, cap, "no IPv4 address to search at");
    memcpy(&dst.sin_addr, to.ip + 12, 4), dst.sin_port = htons(to.port);
    const int fd = sys_socket(AF_INET, SOCK_DGRAM);
    if (fd < 0) return fail(why, cap, "no socket");
    (void)sys_sendto(fd, SEARCH, sizeof SEARCH - 1, &dst);
    const int64_t deadline = sys_now_ms() + TIMEOUT_S * 1000;
    for (int tries = 0; !path && tries < 8 && sys_wait(fd, false, deadline) > 0; tries++) {
        const long n = sys_recvfrom(fd, pkt, sizeof pkt - 1, &from);
        if (n <= 0) continue;
        pkt[n] = 0;
        memcpy(seen.ip + 12, &from.sin_addr, 4), seen.port = 1;
        if (net_addr_public(&seen)) continue;                  /* a router is on the LAN */
        for (char *line = pkt; line && !loc; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
            if (!strncasecmp(line, "location:", 9)) {
                loc = line + 9;
                while (*loc == ' ') loc++;
                ll = strcspn(loc, "\r\n");
            }
        router = from;
        const char *pp = loc ? url_path(loc, ll, &router) : NULL;   /* elsewhere than where the answer came from: not followed */
        const size_t pl = pp ? ll - (size_t)(pp - loc) : 0;
        if (pp && pl < sizeof desc && !memchr(pp, ' ', pl)) memcpy(desc, pp, pl), desc[pl] = 0, path = desc;
        loc = NULL;
    }
    sys_close(fd);
    if (!path) return fail(why, cap, "no router answered the search (UPnP off, or none on this network)");

    /* 2. the description: the control address of the WAN connection service */
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, inet_ntoa(router.sin_addr),
                      ntohs(router.sin_port));
    if (rl <= 0 || (size_t)rl >= sizeof req || !http(&router, req, (size_t)rl, body, BODY_MAX, &local))
        return fail(why, cap, "the router did not give its description");
    type[0] = ctl[0] = 0;
    for (const char *s = body; (s = strstr(s, "<service>")) != NULL; s++) {
        const char *end = strstr(s, "</service>"), *t, *c;
        size_t tl, cl;
        if (!end) break;
        if (!(t = element(s, end, "serviceType", &tl)) || !(c = element(s, end, "controlURL", &cl)) || tl >= sizeof type || cl >= sizeof ctl) continue;
        if (!holds(t, tl, ":WANIPConnection:") && !holds(t, tl, ":WANPPPConnection:")) continue;
        memcpy(type, t, tl), type[tl] = 0;
        struct sockaddr_in at = router;
        const char *cp = cl > 7 && !strncasecmp(c, "http://", 7) ? url_path(c, cl, &at) : c;
        if (!cp) continue;                                     /* a control address on another machine */
        snprintf(ctl, sizeof ctl, "%s%.*s", *cp == '/' ? "" : "/", (int)(cl - (size_t)(cp - c)), cp);
        router = at;
        break;
    }
    if (!ctl[0] || strpbrk(ctl, " \r\n") || strpbrk(type, "\"\r\n")) return fail(why, cap, "the router has no service that opens ports");

    /* 3. the mapping; a router that only takes mappings without end (error 725) is asked for one */
    for (int attempt = 0; attempt < 2; attempt++) {
        const int sl = snprintf(soap, sizeof soap,
            "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:AddPortMapping xmlns:u=\"%s\">"
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>TCP</NewProtocol>"
            "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
            "<NewPortMappingDescription>pzpeer</NewPortMappingDescription><NewLeaseDuration>%u</NewLeaseDuration>"
            "</u:AddPortMapping></s:Body></s:Envelope>\r\n", type, (unsigned)port, (unsigned)port, inet_ntoa(local), attempt ? 0 : lease_s);
        rl = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                      "SOAPAction: \"%s#AddPortMapping\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                      ctl, inet_ntoa(router.sin_addr), ntohs(router.sin_port), type, sl, soap);
        if (sl <= 0 || (size_t)sl >= sizeof soap || rl <= 0 || (size_t)rl >= sizeof req || !http(&router, req, (size_t)rl, body, BODY_MAX, &local))
            return fail(why, cap, "the router did not answer the request");
        if (!strncmp(body, "HTTP/1.", 7) && !strncmp(body + 8, " 200", 4)) {
            snprintf(why, cap, "the router at %s sends port %u to this machine%s", inet_ntoa(router.sin_addr), (unsigned)port, attempt ? " without end" : "");
            return true;
        }
        if (!strstr(body, "<errorCode>725</errorCode>")) break;
    }
    return fail(why, cap, "the router refused to open the port");
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t *p, unsigned v) { put16(p, v >> 16), put16(p + 2, v & 0xffff); }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

/* The datagram to the router, sent again after 250, 500 and 1000 ms without an answer (RFC 6887 8.1.1): the length of the answer of the
 * router to its opcode, or 0. */
static long ask(int fd, const struct sockaddr_in *to, const uint8_t *req, size_t rl, uint8_t *ans, size_t cap)
{
    struct sockaddr_in from;
    for (int wait = 250; wait <= 1000; wait *= 2) {
        if (sys_sendto(fd, req, rl, to) != (long)rl) return 0;
        for (const int64_t deadline = sys_now_ms() + wait; sys_wait(fd, false, deadline) > 0;) {
            const long n = sys_recvfrom(fd, ans, cap, &from);
            if (n == -1) return 0;                             /* nothing listens there (as Windows tells) */
            if (n >= 4 && from.sin_addr.s_addr == to->sin_addr.s_addr && from.sin_port == to->sin_port && ans[1] == (req[1] | 0x80)) return n;
        }
    }
    return 0;
}

/* PCP, or NAT-PMP: the seconds of the mapping, or 0 */
static unsigned pcp(const struct sockaddr_in *router, uint16_t port, unsigned lease_s, const uint8_t nonce[12], char *why, size_t cap)
{
    uint8_t req[60] = {2, 1}, ans[1100];                      /* version 2, MAP */
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    unsigned result = 0, inner = 0, outer = 0, life = 0;
    const char *proto = "PCP";
    /* PCP wants the address the router sees this machine at: that of a socket connected to the router */
    int fd = sys_socket(AF_INET, SOCK_DGRAM);
    const bool seen = fd >= 0 && !connect(fd, (const struct sockaddr *)router, sizeof *router) && !getsockname(fd, (struct sockaddr *)&me, &ml);
    sys_close(fd);
    if (!seen || (fd = sys_socket(AF_INET, SOCK_DGRAM)) < 0) return snprintf(why, cap, "PCP: no way to the router"), 0;
    put32(req + 4, lease_s);
    req[18] = req[19] = 0xff, memcpy(req + 20, &me.sin_addr, 4);                /* this machine, IPv4 in IPv6 */
    memcpy(req + 24, nonce, 12), req[36] = 6;                                   /* TCP */
    put16(req + 40, port), put16(req + 42, port), req[54] = req[55] = 0xff;     /* outside: the same port, at an IPv4 address */
    long n = ask(fd, router, req, sizeof req, ans, sizeof ans);
    if (n >= 60 && ans[0] == 2 && !memcmp(ans + 24, nonce, 12) && ans[36] == 6)               /* the answer to this mapping */
        result = ans[3], inner = get16(ans + 40), outer = get16(ans + 42), life = get32(ans + 4);
    else if (n >= 4 && !ans[0]) {                              /* NAT-PMP: PCP is a version it does not know */
        uint8_t q[12] = {0, 2};                                /* version 0, map TCP */
        put16(q + 4, port), put16(q + 6, port), put32(q + 8, lease_s);
        proto = "NAT-PMP";
        if ((n = ask(fd, router, q, sizeof q, ans, sizeof ans)) >= 16)
            result = ans[0] | get16(ans + 2), inner = get16(ans + 8), outer = get16(ans + 10), life = get32(ans + 12);
        else n = 0;
    } else n = 0;
    sys_close(fd);
    if (!n) snprintf(why, cap, "%s: no answer from %s", proto, inet_ntoa(router->sin_addr));
    else if (result) snprintf(why, cap, "%s: the router refused (result %u)", proto, result);
    else if (inner != port || outer != port || !life) snprintf(why, cap, "%s: the router offers port %u instead", proto, outer);
    else return snprintf(why, cap, "the router at %s sends port %u to this machine for %u s (%s)", inet_ntoa(router->sin_addr),
                         (unsigned)port, life, proto), life < lease_s ? life : lease_s;     /* never asked again later than meant */
    return 0;
}

unsigned portmap(uint16_t port, unsigned lease_s, const uint8_t nonce[12], const char *router, char *why, size_t cap)
{
    struct sockaddr_in at = {.sin_family = AF_INET, .sin_port = htons(PCP_PORT)};
    char first[160], second[160];
    net_addr a;
    unsigned got = 0;
    if (router ? !net_parse_addr(router, &a) || a.ip[10] != 0xff : !sys_gateway(&at.sin_addr))
        snprintf(first, sizeof first, "PCP: no router (no default route)");
    else {
        if (router) memcpy(&at.sin_addr, a.ip + 12, 4), at.sin_port = htons(a.port);
        if ((got = pcp(&at, port, lease_s, nonce, first, sizeof first))) return snprintf(why, cap, "%s", first), got;
    }
    char *body = malloc(BODY_MAX);
    if (body && upnp(body, port, lease_s, router, second, sizeof second)) got = lease_s;
    else if (!body) snprintf(second, sizeof second, "out of memory");
    free(body);
    if (got) snprintf(why, cap, "%s (UPnP)", second);
    else snprintf(why, cap, "%s; UPnP: %s", first, second);
    return got;
}
