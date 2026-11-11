/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sys.h - what pzpeer asks of the operating system, in one place: the clock, randomness, sockets, child processes, the files that hold
 * secrets and the stop signals. sys_posix.c is Linux and macOS, sys_win.c is Windows; the rest of pzpeer is the same code on all three.
 * A socket, a pipe and a file are all an int (-1: none), as on POSIX. */
#ifndef PZPEER_SYS_H
#define PZPEER_SYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#define SYS_PRINTF gnu_printf      /* MinGW with __USE_MINGW_ANSI_STDIO: the formats of C99 (%zu) */
#define gmtime_r(t, tm) (gmtime_s(tm, t) ? NULL : (tm))         /* the same calls under the names the C runtime of Windows lacks */
#define localtime_r(t, tm) (localtime_s(tm, t) ? NULL : (tm))
#else
#define SYS_PRINTF printf
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

/* ---- time and randomness ---- */

int64_t sys_now_ms(void);          /* a clock that only goes forward, in milliseconds: every deadline below is a value of it */
void sys_sleep_ms(int ms);
bool sys_random(void *out, size_t n);   /* from the random source of the system; false when it cannot be read */

/* ---- sockets ----
 * Every socket made here is non-blocking and is not inherited by the processes pzpeer starts; writing to one the other side closed is an
 * error of the call, never a signal. */

#define SYS_AGAIN (-2)             /* nothing now: wait (sys_wait) and call again */

int sys_socket(int family, int type);                           /* SOCK_STREAM (without Nagle) or SOCK_DGRAM; -1 on failure */
int sys_accept(int lfd, struct sockaddr *from, socklen_t *len); /* the next connection of a listening socket; -1, or SYS_AGAIN */
/* A TCP socket connected to an address, or to the first address of a host name that takes the connection; -1 at the deadline or on an
 * error, whose reason sys_net_why gives; -2 for a name that does not resolve. */
int sys_connect(const struct sockaddr *to, socklen_t len, int64_t deadline);
int sys_connect_host(const char *host, const char *port, int64_t deadline);
void sys_setopt(int fd, int level, int name, int value);
void sys_close(int fd);            /* a socket; -1 is ignored */
/* 1 when fd can be read (written, with to_write) or is broken, which the next call on it tells; 0 when the deadline came first (a deadline
 * in the past is one look); -1 on an error. */
int sys_wait(int fd, bool to_write, int64_t deadline);
int sys_poll(struct pollfd *p, int n, int ms);   /* poll: an entry with fd -1 is skipped */
/* What can be taken or given at once: the bytes moved (sys_recv_now: 0 when the other side closed), -1 on an error, SYS_AGAIN. The
 * address of a datagram is IPv4. */
long sys_recv_now(int fd, void *p, size_t cap);
long sys_send_now(int fd, const void *p, size_t n);
long sys_recvfrom(int fd, void *p, size_t cap, struct sockaddr_in *from);
long sys_sendto(int fd, const void *p, size_t n, const struct sockaddr_in *to);
/* The same with the wait: sys_recv gives 1 to cap bytes as soon as some come (0: closed, -1: an error, SYS_AGAIN: the deadline); the
 * other two move all n bytes or are false. */
long sys_recv(int fd, void *p, size_t cap, int64_t deadline);
bool sys_recv_all(int fd, void *p, size_t n, int64_t deadline);
bool sys_send_all(int fd, const void *p, size_t n, int64_t deadline);
const char *sys_net_why(void);     /* the reason of the last failure of a socket call of this thread, for a message */

/* ---- child processes ---- */

typedef intptr_t sys_proc;         /* 0: none */

/* Starts argv[0] (a path, or a name looked up in PATH) with the arguments argv[1..], NULL at the end. With to and from it gets a pipe
 * as its input and one as its output, whose other ends come back here and reach no other child; its errors go where those of pzpeer go.
 * With log its input is empty and its output and errors are appended to that file. The child has a process group of its own: a Ctrl-C
 * at the terminal stops pzpeer, which then decides what becomes of its children. 0 on failure. */
sys_proc sys_spawn(const char *const argv[], const char *log, int *to, int *from);
/* Whether it ended: 1 with its exit status (-1 when it did not end by itself), 0 while it runs. With block, waits for the end. */
int sys_proc_ended(sys_proc p, bool block, int *status);
void sys_proc_interrupt(sys_proc p);   /* asks it to stop, as a Ctrl-C would */
void sys_proc_kill(sys_proc p);
/* The ends of the pipes of sys_spawn: all n bytes, or false (sys_pipe_read: also when nothing comes for timeout_ms). */
bool sys_pipe_write(int fd, const void *p, size_t n);
bool sys_pipe_read(int fd, void *p, size_t n, int timeout_ms);
void sys_pipe_close(int fd);

/* ---- files ----
 * On Linux and macOS a secret file belongs to this user and no one else can open it, and the calls below check it; on Windows the folder
 * decides who reads its files, and nothing is checked. */

bool sys_mkdir(const char *path);          /* one level, for this user alone when it is made; true when the folder is there */
bool sys_private(const char *path);        /* whether path is a folder of this user that nobody else can open */
int sys_open_secret(const char *path);     /* to read: -1 with errno ENOENT when there is none, -2 when it is not a secret file (or a link) */
int sys_create_secret(const char *path);   /* to write, new: -1 when it exists already */
int sys_open_rw(const char *path);         /* to read and write, made for this user alone when it is not there */
FILE *sys_fopen(const char *path, const char *mode);   /* fopen; like every file above, not inherited by the children */
long sys_pread(int fd, void *p, size_t n, uint64_t off);
long sys_pwrite(int fd, const void *p, size_t n, uint64_t off);
int64_t sys_size(int fd);
bool sys_truncate(int fd, uint64_t size);
bool sys_sync(int fd);                     /* what was written is on the disk */
bool sys_replace(const char *from, const char *to);   /* from takes the place of to (which may exist), and stays there after a crash */
void sys_remove_prefixed(const char *dir, const char *prefix);   /* removes the files of dir whose names begin with prefix */

/* ---- the stop ---- */

/* fn is called, as a signal handler is, when the user or the system asks the program to stop: Ctrl-C, a termination request, the
 * terminal closing. */
void sys_on_stop(void (*fn)(int));

#endif
