/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* signer.c - the pzsign process (see signer.h). POSIX: posix_spawn and pipes. */
#include "signer.h"
#include <pzcore/seal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* a pipe whose ends no other child inherits: a pzsign that held a copy of the stdin of another session's would keep that one from seeing
 * the end of its input, and pzsign has no business with any other descriptor of pzpeer (dp.bin, sockets) */
static bool pipe_cloexec(int p[2])
{
    if (pipe(p) != 0) return false;
    if (fcntl(p[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(p[1], F_SETFD, FD_CLOEXEC) == 0) return true;
    close(p[0]), close(p[1]);
    return false;
}

/* a process group of its own: a Ctrl-C at the terminal stops pzpeer, which lets the sessions under way end, and must not kill their
 * pzsign in the middle of a signing */
static int spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, char *const argv[])
{
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    const int rc = posix_spawn(pid, path, fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    return rc;
}

static bool reap(pid_t pid, int *st)
{
    pid_t r;
    while ((r = waitpid(pid, st, 0)) < 0 && errno == EINTR) {}
    return r == pid;
}

bool signer_start(signer *s, const char *path)
{
    int in[2], out[2];
    s->pid = -1, s->to = s->from = -1;
    if (!pipe_cloexec(in)) return false;
    if (!pipe_cloexec(out)) {
        close(in[0]), close(in[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);              /* the copies on 0 and 1 stay open across exec, the originals do not */
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    char *argv[] = {(char *)path, "session", NULL};
    const int rc = spawn(&s->pid, path, &fa, argv);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(out[1]);
    if (rc != 0) {
        close(in[1]), close(out[0]);
        s->pid = -1;
        return false;
    }
    s->to = in[1], s->from = out[0];
    signal(SIGPIPE, SIG_IGN);                                  /* a pzsign that died is an error of the write, not of pzpeer */
    return true;
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w, n -= (size_t)w;
    }
    return true;
}

bool signer_send(signer *s, uint8_t type, const uint8_t *payload, size_t len)
{
    if (len + 1 > SIGNER_FRAME_MAX) return false;
    const uint32_t l = (uint32_t)len + 1;
    const uint8_t head[5] = {(uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l, type};
    return write_all(s->to, head, 5) && write_all(s->to, payload, len);
}

/* n bytes within the deadline (poll, a monotonic budget is not needed: each wait is bounded by timeout_ms) */
static bool read_all(int fd, uint8_t *p, size_t n, int timeout_ms)
{
    while (n) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        const int r = poll(&pf, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        const ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got, n -= (size_t)got;
    }
    return true;
}

bool signer_recv(signer *s, uint8_t *type, uint8_t **payload, size_t *len, int timeout_ms)
{
    uint8_t head[4];
    *payload = NULL;
    if (!read_all(s->from, head, 4, timeout_ms)) return false;
    const uint32_t l = (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
    if (l == 0 || l > SIGNER_FRAME_MAX) return false;
    uint8_t *buf = malloc(l);
    if (!buf || !read_all(s->from, buf, l, timeout_ms)) {
        free(buf);
        return false;
    }
    *type = buf[0];
    memmove(buf, buf + 1, l - 1);
    buf[l - 1] = 0;
    *payload = buf;
    *len = l - 1;
    return true;
}

void signer_kill(signer *s)
{
    if (s->pid > 0) kill(s->pid, SIGKILL);                     /* not reaped yet: the pid is still this process (or its zombie) */
}

void signer_stop(signer *s)
{
    if (s->to >= 0) close(s->to);
    if (s->from >= 0) close(s->from);
    if (s->pid > 0) {
        int st;
        if (waitpid(s->pid, &st, WNOHANG) == 0) kill(s->pid, SIGKILL), (void)reap(s->pid, &st);   /* still running: nothing more to say */
    }
    s->pid = -1, s->to = s->from = -1;
}

bool signer_primes(const char *path, const char *file)
{
    pid_t pid;
    char *argv[] = {(char *)path, "primes", (char *)file, NULL};
    int st;
    return spawn(&pid, path, NULL, argv) == 0 && reap(pid, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = D[p[i] >> 4], out[2 * i + 1] = D[p[i] & 15];
    out[2 * n] = 0;
}

char *signer_job(int i, const uint8_t eid[32], const uint8_t share[32], const uint8_t pub[2][33], const uint8_t key[33], const char *primes,
                 const uint8_t (*digests)[32], int ndigests)
{
    for (const char *c = primes; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) return NULL;   /* a path that would need escaping: not one of ours */
    const size_t cap = 512 + strlen(primes) + (size_t)ndigests * 70;
    char *j = malloc(cap), e[65], sh[65], p0[67], p1[67], k[67], d[65];
    if (!j) return NULL;
    hex(eid, 32, e), hex(share, 32, sh), hex(pub[0], 33, p0), hex(pub[1], 33, p1), hex(key, 33, k);
    size_t o = (size_t)snprintf(j, cap, "{\"op\": \"sign\", \"i\": %d, \"eid\": \"%s\", \"share\": \"%s\", \"pub\": [\"%s\", \"%s\"], \"key\": \"%s\", "
                                         "\"primes\": \"%s\", \"digests\": [", i, e, sh, p0, p1, k, primes);
    for (int n = 0; n < ndigests; n++) {
        hex(digests[n], 32, d);
        o += (size_t)snprintf(j + o, cap - o, "%s\"%s\"", n ? ", " : "", d);
    }
    snprintf(j + o, cap - o, "]}");
    pz_secure_zero(sh, sizeof sh);
    return j;
}
