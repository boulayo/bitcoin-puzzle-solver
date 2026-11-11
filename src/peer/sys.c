/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sys.c - the part of sys.h that is the same on every system: the waits built on the calls that do not wait. */
#include "sys.h"
#include <string.h>

int sys_wait(int fd, bool to_write, int64_t deadline)
{
    const int64_t left = deadline - sys_now_ms();
    struct pollfd p = {.fd = fd, .events = to_write ? POLLOUT : POLLIN};
    const int r = sys_poll(&p, 1, left <= 0 ? 0 : left > 1 << 30 ? 1 << 30 : (int)left);
    return r > 0 ? 1 : r;
}

long sys_recv(int fd, void *p, size_t cap, int64_t deadline)
{
    for (;;) {
        const long r = sys_recv_now(fd, p, cap);
        if (r != SYS_AGAIN) return r;
        const int w = sys_wait(fd, false, deadline);
        if (w <= 0) return w ? -1 : SYS_AGAIN;
    }
}

bool sys_recv_all(int fd, void *p, size_t n, int64_t deadline)
{
    while (n) {
        const long r = sys_recv(fd, p, n, deadline);
        if (r <= 0) return false;                              /* closed before the end, an error, or too late */
        p = (char *)p + r, n -= (size_t)r;
    }
    return true;
}

bool sys_send_all(int fd, const void *p, size_t n, int64_t deadline)
{
    while (n) {
        const long r = sys_send_now(fd, p, n);
        if (r > 0) p = (const char *)p + r, n -= (size_t)r;
        else if (r != SYS_AGAIN || sys_wait(fd, true, deadline) <= 0) return false;
    }
    return true;
}

int sys_connect_host(const char *host, const char *port, int64_t deadline)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return -2;
    int fd = -1;
    for (const struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) fd = sys_connect(ai->ai_addr, (socklen_t)ai->ai_addrlen, deadline);
    freeaddrinfo(res);
    return fd;
}
