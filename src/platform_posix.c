/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* platform_posix.c - the POSIX implementation of platform.h (Linux, macOS; a few #ifdef __APPLE__ shims), in the order of the header.
 * The Windows one is platform_win.c; what the two share is in platform.c. */
#include "platform.h"
#include "envopts.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <spawn.h>
#include <termios.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/prctl.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

extern char **environ;   /* unistd.h declares it only with _GNU_SOURCE; macOS does not declare it at all */

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#ifdef __APPLE__
#undef pthread_create          /* platform.h redirects it here; this is the one real call */
int pz_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    if (attr) return pthread_create(th, attr, fn, arg);
    pthread_attr_t own;
    if (pthread_attr_init(&own) != 0) return pthread_create(th, NULL, fn, arg);
    pthread_attr_setstacksize(&own, (size_t)8 << 20);
    const int rc = pthread_create(th, &own, fn, arg);
    pthread_attr_destroy(&own);
    return rc;
}
#endif

/* ---- files ---- */

int pz_open_read(const char *path) { return open(path, O_RDONLY | O_CLOEXEC); }
int pz_open_rw(const char *path, bool create) { return open(path, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0644); }
int pz_open_trunc(const char *path, int mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)mode); }

int pz_create_temp(char *tmpl)
{
    int fd = mkstemp(tmpl);
    if (fd >= 0) fchmod(fd, 0644);   /* mkstemp creates 0600; these are ordinary data files */
    return fd;
}

i64 pz_pread(int fd, void *buf, size_t n, u64 off)
{
    ssize_t r;
    do r = pread(fd, buf, n, (off_t)off); while (r < 0 && errno == EINTR);
    return r;
}

i64 pz_pwrite(int fd, const void *buf, size_t n, u64 off)
{
    ssize_t r;
    do r = pwrite(fd, buf, n, (off_t)off); while (r < 0 && errno == EINTR);
    return r;
}

i64 pz_fd_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (i64)st.st_size : -1;
}

int pz_fd_sync(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;   /* macOS: fsync() only reaches the drive's cache */
#endif
    return fsync(fd);
}

int pz_fd_truncate(int fd, u64 size) { return ftruncate(fd, (off_t)size); }
int pz_truncate_path(const char *path, u64 size) { return truncate(path, (off_t)size); }

void pz_fd_drop_cache(int fd, u64 off, u64 len)
{
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED);
#else
    (void)fd; (void)off; (void)len;               /* macOS has no equivalent */
#endif
}
int pz_rename_replace_ex(const char *from, const char *to, bool durable) { (void)durable; return rename(from, to); }
int pz_rename_replace(const char *from, const char *to) { return pz_rename_replace_ex(from, to, true); }

int pz_mkdir1(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST && pz_is_dir(path)) return 0;
    if (errno == EEXIST) errno = ENOTDIR;
    return -1;
}

bool pz_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool pz_make_temp_dir(char *out, size_t n, const char *prefix)
{
    const char *t = g_env.tmpdir[0] ? g_env.tmpdir : "/tmp";
    if ((size_t)snprintf(out, n, "%s/%sXXXXXX", t, prefix) >= n) return false;
    return mkdtemp(out) != NULL;
}

int pz_lock_try(int fd, bool exclusive)
{
    for (;;) {
        if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) return 0;
        if (errno == EINTR) continue;
        return errno == EWOULDBLOCK ? 1 : -1;
    }
}

/* ---- memory ---- */

void *pz_map_zeroed(size_t bytes, bool *huge)
{
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return NULL;
    if (huge) *huge = false;
#ifdef MADV_HUGEPAGE
    if (huge) *huge = madvise(m, bytes, MADV_HUGEPAGE) == 0;   /* transparent huge pages, where the kernel allows them */
#endif
    return m;
}

void pz_unmap(void *p, size_t bytes) { if (p) munmap(p, bytes); }

void *pz_aligned_raw(size_t align, size_t n)
{
    void *p = NULL;
    return posix_memalign(&p, align, n) == 0 ? p : NULL;
}

void pz_afree(void *p) { free(p); }

/* ---- time ---- */

double pz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double pz_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void pz_sleep(double s)
{
    if (!(s > 0)) return;
    struct timespec ts = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void pz_localtime(time_t t, struct tm *out) { localtime_r(&t, out); }

/* ---- process / system ---- */

int pz_getpid(void) { return (int)getpid(); }

void pz_hostname(char *buf, size_t n)
{
    if (n == 0) return;
    if (gethostname(buf, n - 1) != 0) buf[0] = 0;
    buf[n - 1] = 0;
}

bool pz_os_random(void *buf, size_t n)
{
    u8 *p = buf;
#ifdef __linux__
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        p += r;
        n -= (size_t)r;
    }
#else
    while (n) {                                          /* macOS and the BSDs: getentropy, at most 256 bytes per call */
        const size_t k = n < 256 ? n : 256;
        if (getentropy(p, k) != 0) break;
        p += k;
        n -= k;
    }
#endif
    if (n == 0) return true;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);   /* very old kernels */
    if (fd < 0) return false;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        p += r;
        n -= (size_t)r;
    }
    close(fd);
    return n == 0;
}

void pz_os_description(char *buf, size_t n)
{
    char name[128] = "";
#ifdef __APPLE__
    char ver[64] = "";
    size_t vlen = sizeof ver;
    if (sysctlbyname("kern.osproductversion", ver, &vlen, NULL, 0) == 0 && ver[0]) snprintf(name, sizeof name, "macOS %s", ver);
    else snprintf(name, sizeof name, "macOS");
    char *t = NULL;
#else
    char *t = pz_read_file("/etc/os-release", NULL);
#endif
    if (t) {
        char *p = strstr(t, "PRETTY_NAME=");
        if (p) {
            p += 12;
            if (*p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && *p != '\n' && i + 1 < sizeof name) name[i++] = *p++;
            name[i] = 0;
        }
        free(t);
    }
    struct utsname u;
    if (uname(&u) != 0) { u.sysname[0] = 0; u.release[0] = 0; }
    snprintf(buf, n, "%s%s%s %s", name, name[0] ? " (" : "", u.sysname, u.release);
    if (name[0]) { size_t l = strlen(buf); if (l + 2 < n) { buf[l] = ')'; buf[l + 1] = 0; } }
}

bool pz_disk_space(const char *path, u64 *free_bytes, u64 *total_bytes)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return false;
    *free_bytes = (u64)v.f_bavail * v.f_frsize;
    *total_bytes = (u64)v.f_blocks * v.f_frsize;
    return true;
}

void pz_power_scheme(char *buf, size_t n) { if (n) buf[0] = 0; }

#ifdef __APPLE__
/* macOS: `caffeinate -i -w <our pid>` keeps the system from idle sleep for as long as this process lives (the helper exits with it). */
static pid_t g_caffeinate;

bool pz_prevent_sleep(bool on)
{
    if (!on) {
        if (g_caffeinate > 0) { kill(g_caffeinate, SIGTERM); waitpid(g_caffeinate, NULL, 0); }
        g_caffeinate = 0;
        return true;
    }
    if (g_caffeinate > 0) return true;
    char pid[24];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *const argv[] = {"caffeinate", "-i", "-w", pid, NULL};
    pid_t child = 0;
    if (posix_spawn(&child, "/usr/bin/caffeinate", NULL, NULL, argv, environ) != 0) return false;
    g_caffeinate = child;
    return true;
}
#else
bool pz_prevent_sleep(bool on) { (void)on; return false; }
#endif

#ifdef __linux__
/* The nice value is per thread on Linux: setpriority(PRIO_PROCESS, tid) leaves the other threads of the process alone. The boost is
 * relative to the value the thread had when it was first boosted (a process started with `nice` keeps its relation to the others). */
bool pz_thread_boost(bool on)
{
    static __thread int base;
    static __thread bool have;
    const id_t tid = (id_t)syscall(SYS_gettid);
    if (!have) {
        errno = 0;
        const int v = getpriority(PRIO_PROCESS, tid);
        if (v == -1 && errno != 0) return false;
        base = v;
        have = true;
    }
    int want = on ? base - 5 : base;
    if (want < -20) want = -20;
    return setpriority(PRIO_PROCESS, tid, want) == 0;
}

/* Tried once, on a thread of its own (the nice value is per thread, so the caller is not affected). */
static void *boost_probe(void *p)
{
    bool ok = pz_thread_boost(true);
    if (ok && !pz_thread_boost(false)) ok = false;
    *(bool *)p = ok;
    return NULL;
}

bool pz_boost_possible(void)
{
    static _Atomic int known;                  /* 0 not tried, 1 yes, 2 no */
    const int k = atomic_load(&known);
    if (k) return k == 1;
    bool ok = false;
    pthread_t th;
    if (pthread_create(&th, NULL, boost_probe, &ok) == 0) pthread_join(th, NULL);
    atomic_store(&known, ok ? 1 : 2);
    return ok;
}

int pz_thread_prio_level(void)
{
    errno = 0;
    return getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
}
#else
/* macOS and the BSDs have no per-thread nice value (setpriority acts on the whole process): the boost is not available. */
bool pz_thread_boost(bool on) { (void)on; return false; }
int pz_thread_prio_level(void) { return 0; }
bool pz_boost_possible(void) { return false; }
#endif

/* ---- console ---- */

void pz_console_init(void)
{
    /* every command that writes a file handles EPIPE and EFBIG like a full disk: neither signal may kill the program */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGXFSZ, SIG_IGN);
}

bool pz_stderr_is_terminal(void)
{
    return isatty(2) && !g_env.term_dumb;   /* a dumb terminal cannot move the cursor */
}

int pz_fd_terminal_cols(int fd)
{
    struct winsize ws;
    return isatty(fd) && ioctl(fd, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 0;
}
bool pz_console_is_own(void) { return false; }

/* ---- secrets and the hardening of the process ---- */

bool pz_stdin_is_terminal(void) { return isatty(0); }

/* The terminal that pz_read_secret has put in no-echo mode, for the handler of the stop signals: tcsetattr is among the functions a signal handler may call. */
static struct termios g_tio_saved;
static volatile sig_atomic_t g_tio_fd = -1;

static void secret_signal(int sig)
{
    if (g_tio_fd >= 0) tcsetattr(g_tio_fd, TCSANOW, &g_tio_saved);
    const char nl = '\n';
    const ssize_t w = write(2, &nl, 1);
    (void)w;
    raise(sig);                    /* SA_RESETHAND: the signal is pending and takes its default action when this handler returns */
}

int pz_read_secret(const char *prompt, char *buf, size_t cap, bool allow_tty)
{
    if (cap < 2) return -1;
    int fd = 0;
    bool own = false, tty = isatty(0);
    if (!tty && allow_tty) {
        const int t = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (t >= 0) { fd = t; own = true; tty = true; }
    }
    static const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction old[4], oldtstp, sa;
    bool inst[4] = {false, false, false, false}, ign_tstp = false, echo_off = false;
    if (tty) {
        struct termios nt;
        if (tcgetattr(fd, &g_tio_saved) != 0) {              /* a terminal whose echo cannot be turned off: no secret is read from it */
            if (own) close(fd);
            buf[0] = 0;
            return -1;
        }
        nt = g_tio_saved;
        nt.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = secret_signal;
        sa.sa_flags = SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        g_tio_fd = fd;
        for (int i = 0; i < 4; i++) {
            struct sigaction cur;
            if (sigaction(sigs[i], NULL, &cur) == 0 && cur.sa_handler == SIG_IGN && sigs[i] == SIGHUP) continue;   /* nohup */
            inst[i] = sigaction(sigs[i], &sa, &old[i]) == 0;
        }
        struct sigaction ig;
        memset(&ig, 0, sizeof ig);
        ig.sa_handler = SIG_IGN;
        sigemptyset(&ig.sa_mask);
        ign_tstp = sigaction(SIGTSTP, &ig, &oldtstp) == 0;   /* a stop with the echo off would leave the shell without echo */
        echo_off = tcsetattr(fd, TCSANOW, &nt) == 0;
        if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    }
    size_t n = 0;
    int rc = 0;
    bool over = false;
    for (;;) {
        char c;
        const ssize_t k = read(fd, &c, 1);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) { rc = -1; break; }
        if (k == 0) { if (n == 0 && !over) rc = -1; break; }
        if (c == '\n') break;
        if (n + 1 < cap) buf[n++] = c;
        else over = true;
    }
    if (n && buf[n - 1] == '\r' && rc == 0) n--;
    if (rc == 0) buf[n] = 0;
    if (tty) {
        if (echo_off) tcsetattr(fd, TCSANOW, &g_tio_saved);
        g_tio_fd = -1;
        const ssize_t w = write(2, "\n", 1);
        (void)w;
        for (int i = 0; i < 4; i++) if (inst[i]) sigaction(sigs[i], &old[i], NULL);
        if (ign_tstp) sigaction(SIGTSTP, &oldtstp, NULL);
        if (own) close(fd);
    }
    if (over && rc == 0) rc = -2;
    if (rc != 0) { memset(buf, 0, cap); __asm__ __volatile__("" : : "r"(buf) : "memory"); return rc; }
    return (int)n;
}

bool pz_mem_lock(void *p, size_t n)
{
    if (!p || !n) return true;
    const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE), a = (uintptr_t)p & ~(pg - 1), e = ((uintptr_t)p + n + pg - 1) & ~(pg - 1);
#if defined(__linux__) && defined(MADV_DONTDUMP)
    madvise((void *)a, e - a, MADV_DONTDUMP);
#endif
    return mlock((void *)a, e - a) == 0;
}

void pz_mem_unlock(void *p, size_t n)
{
    if (!p || !n) return;
    const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE), a = (uintptr_t)p & ~(pg - 1), e = ((uintptr_t)p + n + pg - 1) & ~(pg - 1);
    munlock((void *)a, e - a);
}

bool pz_harden_process(char *why, size_t n)
{
    bool ok = true;
    if (n) why[0] = 0;
    struct rlimit rl = {0, 0};
    if (setrlimit(RLIMIT_CORE, &rl) != 0) {
        if (n) snprintf(why, n, "core dumps could not be disabled (%s)", strerror(errno));
        ok = false;
    }
#ifdef __linux__
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        if (n && ok) snprintf(why, n, "the process could not be made non-dumpable (%s)", strerror(errno));
        ok = false;
    }
#endif
    return ok;
}

int pz_run_shell(const char *cmd, const char *const *env, double timeout, int *code, char *why, size_t whyn)
{
    size_t ne = 0, na = 0, k = 0;
    for (char **e = environ; e && *e; e++) ne++;
    while (env && env[na]) na++;
    char **envp = pz_xmalloc((ne + na + 1) * sizeof *envp);
    for (char **e = environ; e && *e; e++) {
        bool over = false;                                  /* a variable of the same name is replaced by the added one */
        for (size_t i = 0; i < na && !over; i++) {
            const size_t l = strcspn(env[i], "=");
            over = !strncmp(*e, env[i], l) && (*e)[l] == '=';
        }
        if (!over) envp[k++] = *e;
    }
    for (size_t i = 0; i < na; i++) envp[k++] = (char *)env[i];
    envp[k] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_init(&at);
    sigset_t none, dfl;
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);                               /* this program ignores SIGPIPE; the command should not inherit that */
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &dfl);
    posix_spawnattr_setpgroup(&at, 0);                      /* a group of its own: the whole of it can be stopped after the time */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(envp);
    if (rc != 0) {
        if (whyn) snprintf(why, whyn, "cannot start /bin/sh: %s", strerror(rc));
        return -1;
    }
    const double t0 = pz_mono();
    for (;;) {
        int st;
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            return 0;
        }
        if (w < 0 && errno != EINTR) {
            if (whyn) snprintf(why, whyn, "waitpid: %s", strerror(errno));
            return -1;
        }
        if (pz_mono() - t0 > timeout) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return 1;
        }
        pz_sleep(0.05);
    }
}

/* ---- interruption ---- */

static void (*g_interrupt_fn)(void);
static void on_posix_signal(int sig) { (void)sig; if (g_interrupt_fn) g_interrupt_fn(); }

void pz_on_interrupt(void (*fn)(void))
{
    g_interrupt_fn = fn;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_posix_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* closing the terminal sends SIGHUP: stop gracefully and save, unless the process was started under nohup (ignored) */
    struct sigaction old;
    if (sigaction(SIGHUP, NULL, &old) == 0 && old.sa_handler != SIG_IGN) sigaction(SIGHUP, &sa, NULL);
}

void pz_interrupt_done(void) {}

/* ---- dynamic libraries ---- */

void *pz_lib_open(const char *const *names)
{
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    return h;
}

void *pz_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }
