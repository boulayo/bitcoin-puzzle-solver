/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sys_posix.c - sys.h on Linux and macOS. */
#include "sys.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <net/route.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0             /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

extern char **environ;

/* ---- time and randomness ---- */

int64_t sys_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sys_sleep_ms(int ms)
{
    const struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

bool sys_random(void *out, size_t n)
{
    size_t got = 0;
#if defined(__linux__)
    while (got < n) {
        const ssize_t r = getrandom((char *)out + got, n - got, 0);
        if (r > 0) got += (size_t)r;
        else if (r < 0 && errno != EINTR) break;               /* ENOSYS on an old kernel: the device below */
    }
    if (got == n) return true;
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
}

/* ---- sockets ---- */

static int again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? SYS_AGAIN : -1; }

/* non-blocking, closed on exec, no SIGPIPE; a TCP socket without Nagle (a request is one small write and its answer one small read) */
static int prepared(int fd)
{
    const int fl = fd < 0 ? -1 : fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return sys_close(fd), -1;
    sys_setopt(fd, IPPROTO_TCP, TCP_NODELAY, 1);               /* refused by a UDP socket: no matter */
#ifdef SO_NOSIGPIPE
    sys_setopt(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
    return fd;
}

int sys_socket(int family, int type) { return prepared(socket(family, type, 0)); }

int sys_accept(int lfd, struct sockaddr *from, socklen_t *len)
{
    const int fd = accept(lfd, from, len);
    return fd < 0 ? (errno == ECONNABORTED ? SYS_AGAIN : again()) : prepared(fd);
}

void sys_setopt(int fd, int level, int name, int value) { (void)setsockopt(fd, level, name, &value, sizeof value); }

void sys_close(int fd)
{
    if (fd >= 0) close(fd);
}

int sys_poll(struct pollfd *p, int n, int ms)
{
    int r;
    while ((r = poll(p, (nfds_t)n, ms)) < 0 && errno == EINTR) {}
    return r;
}

int sys_connect(const struct sockaddr *to, socklen_t len, int64_t deadline)
{
    const int fd = sys_socket(to->sa_family, SOCK_STREAM);
    if (fd < 0) return -1;
    if (connect(fd, to, len) == 0) return fd;
    int why = errno;
    socklen_t wl = sizeof why;
    if (why == EINPROGRESS || why == EINTR) {
        const int w = sys_wait(fd, true, deadline);
        if (w > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &why, &wl) == 0 && why == 0) return fd;
        if (w == 0) why = ETIMEDOUT;
        else if (w < 0) why = errno;
    }
    close(fd);
    errno = why;
    return -1;
}

long sys_recv_now(int fd, void *p, size_t cap)
{
    const ssize_t r = recv(fd, p, cap, 0);
    return r < 0 ? again() : (long)r;
}

long sys_send_now(int fd, const void *p, size_t n)
{
    const ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
    return r < 0 ? again() : (long)r;
}

long sys_recvfrom(int fd, void *p, size_t cap, struct sockaddr_in *from)
{
    socklen_t fl = sizeof *from;
    const ssize_t r = recvfrom(fd, p, cap, 0, (struct sockaddr *)from, &fl);
    return r < 0 ? again() : fl == sizeof *from ? (long)r : -1;
}

long sys_sendto(int fd, const void *p, size_t n, const struct sockaddr_in *to)
{
    const ssize_t r = sendto(fd, p, n, MSG_NOSIGNAL, (const struct sockaddr *)to, sizeof *to);
    return r < 0 ? again() : (long)r;
}

const char *sys_net_why(void) { return strerror(errno); }

#if defined(__linux__)
bool sys_gateway(struct in_addr *gw)        /* the line of /proc/net/route toward 0.0.0.0 through a router (RTF_GATEWAY) */
{
    FILE *f = fopen("/proc/net/route", "re");
    char line[256];
    unsigned dst, via, flags;
    bool ok = false;
    while (f && !ok && fgets(line, sizeof line, f))
        ok = sscanf(line, "%*s %x %x %x", &dst, &via, &flags) == 3 && !dst && (flags & 2) && via;
    if (f) fclose(f);
    if (ok) gw->s_addr = via;                  /* written in the order of the network already */
    return ok;
}
#elif defined(__APPLE__)
bool sys_gateway(struct in_addr *gw)        /* the routes through a router, from the kernel; the one toward 0.0.0.0 */
{
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    size_t n = 0;
    char *buf = NULL;
    bool ok = false;
    if (sysctl(mib, 6, NULL, &n, NULL, 0) || !n || !(buf = malloc(n)) || sysctl(mib, 6, buf, &n, NULL, 0)) n = 0;
    for (size_t at = 0; !ok && at + sizeof(struct rt_msghdr) <= n;) {
        const struct rt_msghdr *m = (const struct rt_msghdr *)(buf + at);
        const char *end = buf + at + m->rtm_msglen;
        const struct sockaddr_in *dst = (const struct sockaddr_in *)(m + 1), *via = NULL;   /* the destination first, then the router */
        if (m->rtm_msglen < sizeof *m || end > buf + n) break;
        if ((const char *)(dst + 1) <= end) via = (const struct sockaddr_in *)((const char *)dst + ((dst->sin_len + 3u) & ~3u));
        if (via && (const char *)(via + 1) <= end && (m->rtm_addrs & (RTA_DST | RTA_GATEWAY)) == (RTA_DST | RTA_GATEWAY) &&
            dst->sin_family == AF_INET && !dst->sin_addr.s_addr && via->sin_family == AF_INET && via->sin_addr.s_addr)
            *gw = via->sin_addr, ok = true;
        at += m->rtm_msglen;
    }
    free(buf);
    return ok;
}
#else
bool sys_gateway(struct in_addr *gw) { (void)gw; return false; }
#endif

/* ---- child processes ---- */

/* a pipe whose ends no other child inherits: a pzsign that held a copy of the input of another session's would keep that one from seeing
 * the end of its input */
static bool pipe_cloexec(int p[2])
{
    if (pipe(p) != 0) return false;
    if (fcntl(p[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(p[1], F_SETFD, FD_CLOEXEC) == 0) return true;
    close(p[0]), close(p[1]);
    p[0] = p[1] = -1;                                           /* the caller closes what is left: not these again (the numbers may be another thread's by now) */
    return false;
}

sys_proc sys_spawn(const char *const argv[], const char *log, int *to, int *from)
{
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    pid_t pid = 0;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (to && (!pipe_cloexec(in) || !pipe_cloexec(out))) return sys_close(in[0]), sys_close(in[1]), 0;
    if (to) signal(SIGPIPE, SIG_IGN);                          /* a child that died is an error of the write, not the end of pzpeer */
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    if (to) {                                                  /* the copies on 0 and 1 stay open across exec, the originals do not */
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    } else if (log) {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    const int rc = posix_spawnp(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    sys_close(in[0]), sys_close(out[1]);
    if (rc != 0) return sys_close(in[1]), sys_close(out[0]), errno = rc, 0;
    if (to) *to = in[1], *from = out[0];
    return pid;
}

int sys_proc_ended(sys_proc p, bool block, int *status)
{
    int st;
    pid_t r;
    while ((r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG)) < 0 && errno == EINTR) {}
    if (r != (pid_t)p) return 0;
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return 1;
}

void sys_proc_interrupt(sys_proc p) { kill((pid_t)p, SIGINT); }
void sys_proc_kill(sys_proc p) { kill((pid_t)p, SIGKILL); }

bool sys_pipe_write(int fd, const void *p, size_t n)
{
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p = (const char *)p + w, n -= (size_t)w;
    }
    return true;
}

bool sys_pipe_read(int fd, void *p, size_t n, int timeout_ms)
{
    while (n) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (sys_poll(&pf, 1, timeout_ms) <= 0) return false;
        const ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p = (char *)p + got, n -= (size_t)got;
    }
    return true;
}

void sys_pipe_close(int fd) { sys_close(fd); }

/* ---- files ---- */

bool sys_mkdir(const char *path)
{
    struct stat st;
    return mkdir(path, 0700) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool sys_private(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077);
}

int sys_open_secret(const char *path)
{
    struct stat st;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? -1 : -2;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077)) return fd;
    close(fd);
    return -2;
}

int sys_create_secret(const char *path) { return open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600); }
int sys_open_rw(const char *path) { return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600); }

FILE *sys_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f) (void)fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
    return f;
}

long sys_pread(int fd, void *p, size_t n, uint64_t off) { return (long)pread(fd, p, n, (off_t)off); }
long sys_pwrite(int fd, const void *p, size_t n, uint64_t off) { return (long)pwrite(fd, p, n, (off_t)off); }
int64_t sys_size(int fd) { return (int64_t)lseek(fd, 0, SEEK_END); }
bool sys_truncate(int fd, uint64_t size) { return ftruncate(fd, (off_t)size) == 0; }
bool sys_sync(int fd) { return fsync(fd) == 0; }

bool sys_replace(const char *from, const char *to)
{
    if (rename(from, to) != 0) return false;
    const char *slash = strrchr(to, '/');                      /* the rename itself survives a crash once the folder is synced */
    char dir[1100];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - to) + (slash == to) : 1, slash ? to : ".");
    const int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) (void)fsync(dfd), close(dfd);
    return true;
}

/* ---- the stop ---- */

void sys_on_stop(void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigaction(SIGINT, &sa, NULL), sigaction(SIGTERM, &sa, NULL), sigaction(SIGHUP, &sa, NULL);
}
