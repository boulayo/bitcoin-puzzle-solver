/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* upnp.c - see upnp.h. Three steps: a search on the LAN (SSDP, one UDP packet) that the router answers with the address of its description;
 * the description (XML over HTTP), where the control address of its WAN connection service is; the request AddPortMapping (SOAP over HTTP).
 * The router is whoever answers on the LAN, so nothing it says is followed out of the LAN: its description and its control address must be
 * at the very address the answer came from, which must not be a public one. */
#include "upnp.h"
#include "net.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
    const struct timeval tv = {.tv_sec = TIMEOUT_S};
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    size_t n = 0;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    bool ok = connect(fd, (const struct sockaddr *)to, sizeof *to) == 0 && getsockname(fd, (struct sockaddr *)&me, &ml) == 0;
#ifdef MSG_NOSIGNAL
    ok = ok && send(fd, req, rl, MSG_NOSIGNAL) == (ssize_t)rl;
#else
    ok = ok && send(fd, req, rl, 0) == (ssize_t)rl;
#endif
    while (ok && n < cap - 1) {
        const ssize_t r = recv(fd, ans + n, cap - 1 - n, 0);
        if (r < 0) ok = false;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    ans[n] = 0;
    if (ok) *local = me.sin_addr;
    return ok && n > 12 && n < cap - 1;                        /* a full buffer: longer than anything a router says */
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

static bool map_with(char *body, uint16_t port, unsigned lease_s, const char *ssdp, char *why, size_t cap)
{
    static const char SEARCH[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                 "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    char pkt[2048], req[2048], soap[1024], desc[512], type[128], ctl[256];
    net_addr to, seen = {.ip = {[10] = 0xff, [11] = 0xff}};
    struct sockaddr_in dst = {.sin_family = AF_INET}, from, router;
    socklen_t fl = sizeof from;
    struct in_addr local;
    const char *loc = NULL, *path = NULL;
    size_t ll = 0;

    /* 1. the search */
    if (!net_parse_addr(ssdp ? ssdp : "239.255.255.250:1900", &to) || to.ip[10] != 0xff) return fail(why, cap, "no IPv4 address to search at");
    memcpy(&dst.sin_addr, to.ip + 12, 4), dst.sin_port = htons(to.port);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return fail(why, cap, "no socket");
    (void)sendto(fd, SEARCH, sizeof SEARCH - 1, 0, (const struct sockaddr *)&dst, sizeof dst);
    struct pollfd p = {.fd = fd, .events = POLLIN};
    for (int tries = 0; !path && tries < 8 && poll(&p, 1, TIMEOUT_S * 1000) > 0; tries++) {
        const ssize_t n = recvfrom(fd, pkt, sizeof pkt - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0 || fl != sizeof from) continue;
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
    close(fd);
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
        if (!memmem(t, tl, ":WANIPConnection:", 17) && !memmem(t, tl, ":WANPPPConnection:", 18)) continue;
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
            snprintf(why, cap, "the router at %s sends port %u to %s", inet_ntoa(router.sin_addr), (unsigned)port, attempt ? "this machine, without end" : "this machine");
            return true;
        }
        if (!strstr(body, "<errorCode>725</errorCode>")) break;
    }
    return fail(why, cap, "the router refused to open the port");
}

bool upnp_map(uint16_t port, unsigned lease_s, const char *ssdp, char *why, size_t cap)
{
    char *body = malloc(BODY_MAX);
    const bool ok = body ? map_with(body, port, lease_s, ssdp, why, cap) : fail(why, cap, "out of memory");
    free(body);
    return ok;
}
