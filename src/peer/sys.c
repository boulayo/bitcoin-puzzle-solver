/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sys.c - the part of sys.h that is the same on every system: the waits built on the calls that do not wait, the files written whole,
 * the end of a child. */
#include "sys.h"
#include <string.h>
#include <unistd.h>

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

int sys_proc_stop(sys_proc p, int *in, int grace_ms)
{
    int st = -1;
    if (*in >= 0) sys_pipe_close(*in), *in = -1;
    for (int step = 0; step < 2; step++) {
        for (const int64_t until = sys_now_ms() + (step ? 10000 : grace_ms); sys_now_ms() < until; sys_sleep_ms(100))
            if (sys_proc_ended(p, false, &st)) return st;
        if (step) sys_proc_kill(p);
        else sys_proc_interrupt(p);
    }
    (void)sys_proc_ended(p, true, &st);
    return st;
}

static bool tmp_of(const char *path, char *tmp, size_t cap) { return (size_t)snprintf(tmp, cap, "%s.tmp", path) < cap; }

FILE *sys_new_file(const char *path, bool secret)
{
    char tmp[4200];
    if (!tmp_of(path, tmp, sizeof tmp)) return NULL;
    (void)remove(tmp);
    if (!secret) return sys_fopen(tmp, "wb");
    const int fd = sys_create_secret(tmp);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (f) setvbuf(f, NULL, _IONBF, 0);
    else if (fd >= 0) close(fd);
    return f;
}

bool sys_new_file_done(FILE *f, const char *path, bool ok)
{
    char tmp[4200];
    if (!f) return false;
    ok = ok && fflush(f) == 0 && sys_sync(fileno(f));
    ok = fclose(f) == 0 && ok;
    if (!tmp_of(path, tmp, sizeof tmp)) return false;
    if (!(ok = ok && sys_replace(tmp, path))) (void)remove(tmp);
    return ok;
}
