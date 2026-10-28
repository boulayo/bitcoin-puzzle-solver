/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* platform.c - see platform.h. POSIX implementation first (Linux, macOS), Windows (MinGW-w64) second. */
#include "platform.h"
#include "envopts.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#ifndef _WIN32
/* ===================================================================================================== POSIX */
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
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
extern char **environ;
#endif

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
int pz_getpid(void) { return (int)getpid(); }

void pz_hostname(char *buf, size_t n)
{
    if (n == 0) return;
    if (gethostname(buf, n - 1) != 0) buf[0] = 0;
    buf[n - 1] = 0;
}

void pz_os_description(char *buf, size_t n)
{
    char name[128] = "";
#ifdef __APPLE__
    char ver[64] = "";
    size_t pz_vl = sizeof ver;
    if (sysctlbyname("kern.osproductversion", ver, &pz_vl, NULL, 0) == 0 && ver[0]) snprintf(name, sizeof name, "macOS %s", ver);
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

int pz_thread_prio_level(void)
{
    errno = 0;
    return getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
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
#else
/* macOS and the BSDs have no per-thread nice value (setpriority acts on the whole process): the boost is not available. */
bool pz_thread_boost(bool on) { (void)on; return false; }
int pz_thread_prio_level(void) { return 0; }
bool pz_boost_possible(void) { return false; }
#endif

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

void pz_console_init(void) {}
bool pz_console_is_own(void) { return false; }

bool pz_stderr_is_terminal(void)
{
    return isatty(2) && !g_env.term_dumb;   /* a dumb terminal cannot move the cursor */
}

int pz_terminal_cols(void)
{
    struct winsize ws;
    return ioctl(2, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 0;
}

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
    signal(SIGPIPE, SIG_IGN);
}

void pz_interrupt_done(void) {}

void *pz_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

void *pz_lib_open(const char *const *names)
{
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    return h;
}

#else
/* =================================================================================================== Windows */
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/locking.h>

/* CRT descriptors on top of Win32 handles. _sopen_s with _SH_DENYNO lets other processes read files we keep open
 * (the lock file, the DP log), as on POSIX. */
static int open_flags(const char *path, int flags)
{
    int fd = -1;
    if (_sopen_s(&fd, path, flags | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) return -1;
    return fd;
}

int pz_open_read(const char *path) { return open_flags(path, _O_RDONLY); }
int pz_open_rw(const char *path, bool create) { return open_flags(path, _O_RDWR | (create ? _O_CREAT : 0)); }
int pz_open_trunc(const char *path, int mode) { (void)mode; return open_flags(path, _O_WRONLY | _O_CREAT | _O_TRUNC); }   /* no POSIX modes: the folder decides who can read */

int pz_create_temp(char *tmpl)
{
    size_t l = strlen(tmpl);
    if (l < 6 || strcmp(tmpl + l - 6, "XXXXXX") != 0) { errno = EINVAL; return -1; }
    for (int attempt = 0; attempt < 100; attempt++) {
        u8 r[6];
        if (!pz_os_random(r, sizeof r)) return -1;
        static const char al[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; i < 6; i++) tmpl[l - 6 + i] = al[r[i] % 36];
        int fd = open_flags(tmpl, _O_RDWR | _O_CREAT | _O_EXCL);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}

static HANDLE fd_handle(int fd) { return (HANDLE)_get_osfhandle(fd); }

static void set_errno_from_win(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: errno = EACCES; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: errno = ENOMEM; break;
    default: errno = EIO;
    }
}

i64 pz_pread(int fd, void *buf, size_t n, u64 off)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (n > 0x40000000) n = 0x40000000;   /* a single ReadFile takes a DWORD count */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        set_errno_from_win(e);
        return -1;
    }
    return (i64)got;
}

i64 pz_pwrite(int fd, const void *buf, size_t n, u64 off)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (n > 0x40000000) n = 0x40000000;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)n, &put, &ov)) { set_errno_from_win(GetLastError()); return -1; }
    return (i64)put;
}

i64 pz_fd_size(int fd)
{
    LARGE_INTEGER sz;
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz)) return -1;
    return (i64)sz.QuadPart;
}

int pz_fd_sync(int fd)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE || !FlushFileBuffers(h)) { errno = EIO; return -1; }
    return 0;
}

int pz_fd_truncate(int fd, u64 size)
{
    HANDLE h = fd_handle(fd);
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    if (h == INVALID_HANDLE_VALUE || !SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof info)) { errno = EIO; return -1; }
    return 0;
}

/* MinGW's truncate() takes a 32-bit length (only ftruncate is renamed by _FILE_OFFSET_BITS): a file of 2 GiB or more would not be cut,
 * or cut to its size modulo 4 GiB. */
int pz_truncate_path(const char *path, u64 size)
{
    const int fd = pz_open_rw(path, false);
    if (fd < 0) return -1;
    const int rc = pz_fd_truncate(fd, size);
    const int e = errno;
    close(fd);
    errno = e;
    return rc;
}

void pz_fd_drop_cache(int fd, u64 off, u64 len) { (void)fd; (void)off; (void)len; }

int pz_rename_replace_ex(const char *from, const char *to, bool durable)
{
    /* MoveFileEx replaces atomically on NTFS; a reader (`list`, an antivirus scan) holding `to` open for a moment
     * makes it fail with a sharing error, so retry for up to ~2 s before giving up */
    for (int attempt = 0; attempt < 80; attempt++) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0))) return 0;
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) { set_errno_from_win(e); return -1; }
        Sleep(25);
    }
    errno = EACCES;
    return -1;
}

int pz_rename_replace(const char *from, const char *to) { return pz_rename_replace_ex(from, to, true); }

int pz_mkdir1(const char *path)
{
    if (CreateDirectoryA(path, NULL)) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS && pz_is_dir(path)) return 0;
    if (e == ERROR_ALREADY_EXISTS) { errno = ENOTDIR; return -1; }
    set_errno_from_win(e);
    return -1;
}

bool pz_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool pz_make_temp_dir(char *out, size_t n, const char *prefix)
{
    char base[MAX_PATH + 1];
    DWORD l = GetTempPathA(sizeof base, base);
    if (l == 0 || l > MAX_PATH) return false;
    for (int attempt = 0; attempt < 100; attempt++) {
        u8 r[4];
        if (!pz_os_random(r, sizeof r)) return false;
        if ((size_t)snprintf(out, n, "%s%s%02x%02x%02x%02x", base, prefix, r[0], r[1], r[2], r[3]) >= n) return false;
        if (CreateDirectoryA(out, NULL)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

int pz_lock_try(int fd, bool exclusive)
{
    /* Windows byte-range locks are mandatory: lock a byte far beyond the end of the file, so that the PID written at
     * its start stays readable by the process that is refused the lock */
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.OffsetHigh = 0x40000000;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (LockFileEx(h, flags, 0, 1, 0, &ov)) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) return 1;
    set_errno_from_win(e);
    return -1;
}

/* Large pages need the "Lock pages in memory" right (SeLockMemoryPrivilege), which an administrator grants with
 * secpol.msc; without it ordinary 4 KiB pages are used. */
static bool enable_lock_memory_privilege(void)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

void *pz_map_zeroed(size_t bytes, bool *huge)
{
    if (huge) *huge = false;
    void *m = NULL;
    SIZE_T lp = GetLargePageMinimum();
    if (lp && bytes >= ((size_t)1 << 30) && !g_env.no_large_pages && enable_lock_memory_privilege()) {
        size_t rounded = (bytes + lp - 1) / lp * lp;
        m = VirtualAlloc(NULL, rounded, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);   /* zeroed, locked in RAM */
        if (m && huge) *huge = true;
    }
    if (!m) m = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   /* demand-zero pages */
    return m;
}

void pz_unmap(void *p, size_t bytes)
{
    (void)bytes;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

void *pz_aligned_raw(size_t align, size_t n) { return _aligned_malloc(n, align); }
void pz_afree(void *p) { _aligned_free(p); }

double pz_now(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = (u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;   /* 100 ns since 1601-01-01 */
    return (double)(t - 116444736000000000ULL) * 1e-7;
}

double pz_mono(void)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

void pz_sleep(double s)
{
    if (!(s > 0)) return;
    double ms = s * 1000.0;
    Sleep(ms >= 4294967294.0 ? 4294967294u : (DWORD)(ms + 0.5));
}

void pz_localtime(time_t t, struct tm *out)
{
    if (localtime_s(out, &t) != 0) memset(out, 0, sizeof *out);
}

int pz_getpid(void) { return (int)GetCurrentProcessId(); }

void pz_hostname(char *buf, size_t n)
{
    DWORD l = (DWORD)n;
    if (n == 0) return;
    if (!GetComputerNameA(buf, &l)) buf[0] = 0;
    buf[n - 1] = 0;
}

void pz_os_description(char *buf, size_t n)
{
    typedef LONG (WINAPI *rtl_get_version)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    rtl_get_version f = nt ? (rtl_get_version)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;   /* GetVersionEx lies without a manifest */
    if (f && f(&v) == 0)
        snprintf(buf, n, "Windows %s%lu.%lu build %lu", v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "11 / " : "", (unsigned long)v.dwMajorVersion,
                 (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
    else snprintf(buf, n, "Windows (version not detected)");
}

bool pz_disk_space(const char *path, u64 *free_bytes, u64 *total_bytes)
{
    ULARGE_INTEGER fb, tb, tf;
    if (!GetDiskFreeSpaceExA(path, &fb, &tb, &tf)) return false;
    *free_bytes = (u64)fb.QuadPart;
    *total_bytes = (u64)tb.QuadPart;
    return true;
}

void pz_power_scheme(char *buf, size_t n)
{
    if (!n) return;
    buf[0] = 0;
    FILE *f = _popen("powercfg /getactivescheme 2>nul", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '(');                       /* "Power Scheme GUID: ...  (Balanced)" */
        char *q = p ? strrchr(p, ')') : NULL;
        if (p && q && q > p + 1) { *q = 0; pz_strlcpy(buf, p + 1, n); }
    }
    _pclose(f);
}

bool pz_prevent_sleep(bool on)
{
    return SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_SYSTEM_REQUIRED : 0)) != 0;
}

bool pz_thread_boost(bool on)
{
    return SetThreadPriority(GetCurrentThread(), on ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL) != 0;
}

bool pz_boost_possible(void) { return true; }

int pz_thread_prio_level(void) { return GetThreadPriority(GetCurrentThread()); }

bool pz_os_random(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static bool g_vt_ok;

void pz_console_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        g_vt_ok = SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

bool pz_stderr_is_terminal(void)
{
    /* only a real console whose escape-sequence processing could be enabled (Windows 10 1607+ / Windows Terminal);
     * pipes, files and the NUL device are not terminals */
    DWORD mode;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return g_vt_ok && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

int pz_terminal_cols(void)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &ci)) return 0;
    return ci.srWindow.Right - ci.srWindow.Left + 1;
}

bool pz_console_is_own(void)
{
    DWORD pids[2];
    return GetConsoleProcessList(pids, 2) == 1;
}

static void (*g_interrupt_fn)(void);
static HANDLE g_done_event;

static BOOL WINAPI console_handler(DWORD type)
{
    if (g_interrupt_fn) g_interrupt_fn();
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) return TRUE;
    /* window closed, logoff or shutdown: the process is terminated as soon as this returns, so give the main thread
     * time to save the state (Windows allows about 5 seconds) */
    if (g_done_event) WaitForSingleObject(g_done_event, 4500);
    return TRUE;
}

void pz_on_interrupt(void (*fn)(void))
{
    g_interrupt_fn = fn;
    if (!g_done_event) g_done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(console_handler, TRUE);
}

void pz_interrupt_done(void) { if (g_done_event) SetEvent(g_done_event); }

void *pz_lib_open(const char *const *names)
{
    HMODULE h = NULL;
    for (int i = 0; names[i] && !h; i++) h = LoadLibraryA(names[i]);
    return (void *)h;
}

void *pz_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }

#endif

/* =================================================================================================== common */

int pz_remove_tree(const char *path)
{
    if (!pz_is_dir(path)) return remove(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t l = strlen(path) + strlen(e->d_name) + 2;
        char *p = pz_xmalloc(l);
        snprintf(p, l, "%s/%s", path, e->d_name);
        if (pz_remove_tree(p) != 0) rc = -1;
        free(p);
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}
