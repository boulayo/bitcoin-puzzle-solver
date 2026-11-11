/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_sys.c - the layer over the operating system (sys.h), the same checks on Linux, macOS and Windows: the clock and the random source;
 * TCP and UDP on the loopback with their deadlines; child processes, which are this program started again with a word that says what to
 * do (their pipes, their log, their exit status, a stop request, a kill, and that a child does not inherit the pipe of another); the files
 * of secrets, their replacement, and the files read and written at an offset. */
#include "peertest.h"
#include <signal.h>
#include <unistd.h>

static void test_time_random(void)
{
    uint8_t a[32], b[32];
    const int64_t t0 = sys_now_ms();
    sys_sleep_ms(60);
    const int64_t dt = sys_now_ms() - t0;
    CHECK(dt >= 40 && dt < 2000);
    CHECK(sys_random(a, sizeof a) && sys_random(b, sizeof b) && memcmp(a, b, sizeof a) != 0);
}

static void test_sockets(void)
{
    struct sockaddr_in a = {.sin_family = AF_INET}, from;
    socklen_t al = sizeof a;
    char port[8], buf[64];
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int l = sys_socket(AF_INET, SOCK_STREAM);
    CHECK(l >= 0 && bind(l, (struct sockaddr *)&a, sizeof a) == 0 && listen(l, 4) == 0 && getsockname(l, (struct sockaddr *)&a, &al) == 0);
    CHECK(sys_wait(l, false, sys_now_ms() + 50) == 0 && sys_accept(l, NULL, NULL) == SYS_AGAIN);   /* nobody yet */
    const int c = sys_connect((struct sockaddr *)&a, sizeof a, sys_now_ms() + 3000);
    CHECK(c >= 0 && sys_wait(l, false, sys_now_ms() + 3000) == 1);
    const int s = sys_accept(l, NULL, NULL);
    CHECK(s >= 0);
    /* both ways, all the bytes; then nothing: a deadline, not an error; then the end */
    CHECK(sys_send_all(c, "question", 8, sys_now_ms() + 3000) && sys_recv_all(s, buf, 8, sys_now_ms() + 3000) && !memcmp(buf, "question", 8));
    CHECK(sys_send_all(s, "answer", 6, sys_now_ms() + 3000) && sys_recv_all(c, buf, 6, sys_now_ms() + 3000) && !memcmp(buf, "answer", 6));
    int64_t t0 = sys_now_ms();
    CHECK(sys_recv(c, buf, sizeof buf, t0 + 100) == SYS_AGAIN && sys_now_ms() - t0 >= 80 && sys_recv_now(c, buf, sizeof buf) == SYS_AGAIN);
    CHECK(!sys_recv_all(c, buf, 4, sys_now_ms() + 100));
    sys_close(s);
    CHECK(sys_recv(c, buf, sizeof buf, sys_now_ms() + 3000) == 0);
    sys_close(c);
    /* by name; a name that is none; a port where nobody listens */
    snprintf(port, sizeof port, "%u", ntohs(a.sin_port));
    const int h = sys_connect_host("127.0.0.1", port, sys_now_ms() + 3000);
    CHECK(h >= 0 && sys_connect_host("no.such.host.invalid", port, sys_now_ms() + 3000) == -2);
    sys_close(h);
    sys_close(l);
    t0 = sys_now_ms();
    CHECK(sys_connect((struct sockaddr *)&a, sizeof a, t0 + 3000) == -1 && sys_now_ms() - t0 < 2900 && sys_net_why()[0]);
    /* datagrams */
    struct sockaddr_in u = {.sin_family = AF_INET};
    u.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int r = sys_socket(AF_INET, SOCK_DGRAM), w = sys_socket(AF_INET, SOCK_DGRAM);
    al = sizeof u;
    CHECK(r >= 0 && w >= 0 && bind(r, (struct sockaddr *)&u, sizeof u) == 0 && getsockname(r, (struct sockaddr *)&u, &al) == 0);
    CHECK(sys_recvfrom(r, buf, sizeof buf, &from) == SYS_AGAIN && sys_sendto(w, "ping", 4, &u) == 4);
    CHECK(sys_wait(r, false, sys_now_ms() + 3000) == 1 && sys_recvfrom(r, buf, sizeof buf, &from) == 4 && !memcmp(buf, "ping", 4));
    CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    sys_close(r), sys_close(w);
    sys_close(-1);
}

/* ---- the children ---- */

/* Wine does not deliver the Ctrl-Break that sys_proc_interrupt sends on Windows: there the stop request is run and not checked */
static bool under_wine(void)
{
#ifdef _WIN32
    return GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
#else
    return false;
#endif
}

static volatile sig_atomic_t g_asked;
static void asked(int sig) { (void)sig, g_asked = 1; }

/* what this program does when it is started as a child */
static int child(const char *what)
{
    if (!strcmp(what, "echo")) {                               /* its input in capitals, to the end of the input; status 7 */
        for (int ch; (ch = getchar()) != EOF;) putchar(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        return 7;
    }
    if (!strcmp(what, "say")) {
        printf("to the output\n");
        fprintf(stderr, "to the errors\n");
        return 0;
    }
    sys_on_stop(asked);                                        /* until it is asked to stop (status 5), a minute at most */
    if (!strcmp(what, "obey")) printf("ready\n"), fflush(stdout);   /* "wait" says nothing */
    for (int i = 0; i < 600 && !g_asked; i++) sys_sleep_ms(100);
    return g_asked ? 5 : 6;
}

static void test_processes(const char *self, const char *dir)
{
    char log[300], text[200] = "";
    int to = -1, from = -1, to2 = -1, from2 = -1, st = 0;
    const char *const echo[] = {self, "child", "echo", NULL}, *const say[] = {self, "child", "say", NULL},
               *const wait[] = {self, "child", "wait", NULL}, *const obey[] = {self, "child", "obey", NULL}, *const none[] = {"no-such-program-of-t-sys", NULL};
    /* pipes: what goes in comes out in capitals once the input ends; a second child started meanwhile does not hold the first one's
     * pipe open (the first would never see the end of its input) */
    const sys_proc e = sys_spawn(echo, NULL, &to, &from), w = sys_spawn(wait, NULL, &to2, &from2);
    CHECK(e && w && to >= 0 && from >= 0);
    CHECK(sys_pipe_write(to, "a b", 3) && sys_pipe_write(to, "c", 1));
    sys_pipe_close(to);
    CHECK(sys_pipe_read(from, text, 4, 10000) && !memcmp(text, "A BC", 4));
    CHECK(!sys_pipe_read(from, text, 1, 2000));                /* the end of its output */
    CHECK(sys_proc_ended(e, true, &st) == 1 && st == 7);
    sys_pipe_close(from);
    /* the other one says nothing: a read gives up in time; killed, it did not end by itself */
    int64_t t0 = sys_now_ms();
    CHECK(!sys_pipe_read(from2, text, 1, 300) && sys_now_ms() - t0 >= 250 && sys_now_ms() - t0 < 3000);
    CHECK(sys_proc_ended(w, false, &st) == 0);
    sys_proc_kill(w);
    CHECK(sys_proc_ended(w, true, &st) == 1 && st == -1);
    sys_pipe_close(to2), sys_pipe_close(from2);
    /* a log: the output and the errors of two runs, one after the other */
    snprintf(log, sizeof log, "%s/child.log", dir);
    for (int run = 0; run < 2; run++) {
        const sys_proc p = sys_spawn(say, log, NULL, NULL);
        CHECK(p && sys_proc_ended(p, true, &st) == 1 && st == 0);
    }
    FILE *f = fopen(log, "rb");
    const size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    int lines = 0;
    for (const char *p = text; (p = strstr(p, "to the ")) != NULL; p++) lines++;
    CHECK(lines == 4 && strstr(text, "to the output") && strstr(text, "to the errors"));
    /* a stop request: the child sees it and ends by itself */
    const sys_proc q = sys_spawn(obey, log, NULL, NULL);
    CHECK(q != 0);
    for (int i = 0; i < 100 && !strstr(text, "ready"); i++) {  /* until it has its handler */
        sys_sleep_ms(100);
        if ((f = fopen(log, "rb")) != NULL) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    }
    sys_proc_interrupt(q);
    CHECK(sys_proc_ended(q, true, &st) == 1 && (st == 5 || under_wine()));
    if (st != 5) printf("t_sys: the stop request did not reach the child (status %d): Wine does not pass a Ctrl-Break on\n", st);
    CHECK(sys_spawn(none, NULL, NULL, NULL) == 0);
    CHECK(remove(log) == 0);
}

static void test_files(const char *dir)
{
    char path[300], tmp[300], buf[32] = "";
    snprintf(path, sizeof path, "%s/secret", dir);
    snprintf(tmp, sizeof tmp, "%s/secret.tmp", dir);
    CHECK(sys_private(dir) && sys_mkdir(dir) && !sys_private(path));
    CHECK(sys_open_secret(path) == -1);                        /* none yet */
    int fd = sys_create_secret(path);
    CHECK(fd >= 0 && write(fd, "one\n", 4) == 4 && sys_sync(fd) && close(fd) == 0 && sys_create_secret(path) == -1);
    fd = sys_create_secret(tmp);
    CHECK(fd >= 0 && write(fd, "two\n", 4) == 4 && close(fd) == 0 && sys_replace(tmp, path));   /* over the file that is there */
    fd = sys_open_secret(path);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 4 && !memcmp(buf, "two\n", 4) && close(fd) == 0 && sys_open_secret(tmp) == -1);
    CHECK(remove(path) == 0);
    /* at an offset */
    snprintf(path, sizeof path, "%s/records", dir);
    fd = sys_open_rw(path);
    CHECK(fd >= 0 && sys_size(fd) == 0 && sys_pwrite(fd, "0123456789", 10, 0) == 10 && sys_pwrite(fd, "ab", 2, 20) == 2 && sys_size(fd) == 22);
    CHECK(sys_pread(fd, buf, 4, 6) == 4 && !memcmp(buf, "6789", 4) && sys_pread(fd, buf, 4, 20) == 2 && !memcmp(buf, "ab", 2));
    CHECK(sys_pread(fd, buf, 4, 100) == 0 && sys_truncate(fd, 5) && sys_size(fd) == 5 && sys_sync(fd) && close(fd) == 0);
    fd = sys_open_rw(path);                                    /* kept */
    CHECK(fd >= 0 && sys_size(fd) == 5 && close(fd) == 0);
    /* the files of a prefix, and no other */
    static const char *const NAMES[] = {"primes-1.json", "primes-2.json", "other"};
    for (int i = 0; i < 3; i++) {
        snprintf(tmp, sizeof tmp, "%s/%s", dir, NAMES[i]);
        FILE *f = sys_fopen(tmp, "wb");
        CHECK(f != NULL);
        if (f) fclose(f);
    }
    sys_remove_prefixed(dir, "primes-");
    snprintf(tmp, sizeof tmp, "%s/primes-1.json", dir);
    CHECK(sys_fopen(tmp, "rb") == NULL);
    snprintf(tmp, sizeof tmp, "%s/other", dir);
    CHECK(remove(tmp) == 0 && remove(path) == 0);
}

int main(int argc, char **argv)
{
    char dir[200];
    if (argc == 3 && !strcmp(argv[1], "child")) return child(argv[2]);
    test_time_random();
    test_sockets();
    CHECK(pt_tmpdir("t_sys", dir, sizeof dir));
    test_processes(argv[0], dir);
    test_files(dir);
    CHECK(rmdir(dir) == 0);
    TEST_DONE("t_sys");
}
