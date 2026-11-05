/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* platform_win.c - the Windows implementation of platform.h (MinGW-w64), in the order of the header. The POSIX one is platform_posix.c;
 * what the two share is in platform.c. */
#include "platform.h"
#include "envopts.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/locking.h>

/* ---- files ---- */

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

/* ---- memory ---- */

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

/* ---- time ---- */

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

/* ---- process / system ---- */

int pz_getpid(void) { return (int)GetCurrentProcessId(); }

void pz_hostname(char *buf, size_t n)
{
    DWORD l = (DWORD)n;
    if (n == 0) return;
    if (!GetComputerNameA(buf, &l)) buf[0] = 0;
    buf[n - 1] = 0;
}

bool pz_os_random(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
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

/* ---- console ---- */

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

int pz_fd_terminal_cols(int fd)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    HANDLE h = fd == 1 ? GetStdHandle(STD_OUTPUT_HANDLE) : fd == 2 ? GetStdHandle(STD_ERROR_HANDLE) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &ci)) return 0;   /* a pipe or a file: no console */
    return ci.srWindow.Right - ci.srWindow.Left + 1;
}

bool pz_console_is_own(void)
{
    DWORD pids[2];
    return GetConsoleProcessList(pids, 2) == 1;
}

/* ---- secrets and the hardening of the process ---- */

bool pz_stdin_is_terminal(void)
{
    DWORD m;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return h != INVALID_HANDLE_VALUE && h != NULL && GetConsoleMode(h, &m);
}

static HANDLE volatile g_con_in;
static volatile DWORD g_con_mode;

static BOOL WINAPI secret_ctrl(DWORD type)
{
    (void)type;
    if (g_con_in) SetConsoleMode(g_con_in, g_con_mode);      /* then the next handler: the process ends as it would have */
    return FALSE;
}

int pz_read_secret(const char *prompt, char *buf, size_t cap, bool allow_tty)
{
    if (cap < 2) return -1;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool con = h != INVALID_HANDLE_VALUE && h != NULL && GetConsoleMode(h, &mode), own = false;
    if (!con && allow_tty) {
        HANDLE t = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (t != INVALID_HANDLE_VALUE && GetConsoleMode(t, &mode)) { h = t; con = true; own = true; }
        else if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    }
    bool echo_off = false;
    if (con) {
        g_con_mode = mode;
        g_con_in = h;
        SetConsoleCtrlHandler(secret_ctrl, TRUE);
        echo_off = SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT) != 0;
        if (!echo_off) {                                      /* a console whose echo cannot be turned off: no secret is read from it */
            SetConsoleCtrlHandler(secret_ctrl, FALSE);
            g_con_in = NULL;
            if (own) CloseHandle(h);
            buf[0] = 0;
            return -1;
        }
        if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    }
    size_t n = 0;
    int rc = 0;
    bool over = false, eol = false;
    while (!eol) {
        if (con) {
            WCHAR w[256];
            DWORD got = 0;
            if (!ReadConsoleW(h, w, 256, &got, NULL) || got == 0) { if (n == 0 && !over) rc = -1; break; }
            for (DWORD i = 0; i < got; i++) {
                if (w[i] == L'\n') { eol = true; break; }
                if (w[i] == L'\r') continue;
                char u[8];
                const int ul = WideCharToMultiByte(CP_UTF8, 0, &w[i], 1, u, sizeof u, NULL, NULL);
                if (ul > 0 && n + (size_t)ul < cap) { memcpy(buf + n, u, (size_t)ul); n += (size_t)ul; }
                else over = true;
            }
            memset(w, 0, sizeof w);
        } else {
            char c;
            DWORD got = 0;
            if (!ReadFile(h, &c, 1, &got, NULL) || got == 0) { if (n == 0 && !over) rc = -1; break; }
            if (c == '\n') break;
            if (c == '\r') continue;
            if (n + 1 < cap) buf[n++] = c;
            else over = true;
        }
    }
    if (rc == 0) buf[n] = 0;
    if (con) {
        if (echo_off) SetConsoleMode(h, mode);
        SetConsoleCtrlHandler(secret_ctrl, FALSE);
        g_con_in = NULL;
        fputs("\n", stderr);
        if (own) CloseHandle(h);
    }
    if (over && rc == 0) rc = -2;
    if (rc != 0) { memset(buf, 0, cap); __asm__ __volatile__("" : : "r"(buf) : "memory"); return rc; }
    return (int)n;
}

bool pz_mem_lock(void *p, size_t n) { return !p || !n || VirtualLock(p, n) != 0; }
void pz_mem_unlock(void *p, size_t n) { if (p && n) VirtualUnlock(p, n); }

bool pz_harden_process(char *why, size_t n)
{
    if (n) snprintf(why, n, "Windows has no way to forbid a dump of the process from here");
    return false;
}

int pz_run_shell(const char *cmd, const char *const *env, double timeout, int *code, char *why, size_t whyn)
{
    size_t na = 0, total = 2;
    while (env && env[na]) { total += strlen(env[na]) + 1; na++; }
    char *base = GetEnvironmentStringsA();
    if (!base) { if (whyn) snprintf(why, whyn, "cannot read the environment"); return -1; }
    size_t bl = 0;
    while (base[bl]) bl += strlen(base + bl) + 1;
    char *blk = pz_xmalloc(total + bl), *o = blk;
    for (const char *e = base; *e; e += strlen(e) + 1) {
        bool over = false;
        for (size_t i = 0; i < na && !over; i++) {
            const size_t l = strcspn(env[i], "=");
            over = !_strnicmp(e, env[i], l) && e[l] == '=';
        }
        if (!over) { const size_t l = strlen(e) + 1; memcpy(o, e, l); o += l; }
    }
    FreeEnvironmentStringsA(base);
    for (size_t i = 0; i < na; i++) { const size_t l = strlen(env[i]) + 1; memcpy(o, env[i], l); o += l; }
    *o = 0;
    char comspec[MAX_PATH];
    if (!GetEnvironmentVariableA("COMSPEC", comspec, sizeof comspec)) pz_strlcpy(comspec, "cmd.exe", sizeof comspec);
    const size_t cl = strlen(comspec) + strlen(cmd) + 32;
    char *line = pz_xmalloc(cl);
    snprintf(line, cl, "\"%s\" /d /s /c \"%s\"", comspec, cmd);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    const BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, blk, NULL, &si, &pi);
    const DWORD err = GetLastError();
    free(line);
    free(blk);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (whyn) snprintf(why, whyn, "cannot start the command interpreter (error %lu)", (unsigned long)err);
        return -1;
    }
    int rc = 0;
    const DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)(timeout * 1000.0));
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        rc = 1;
    } else {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        *code = (int)ec;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return rc;
}

/* ---- interruption ---- */

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

/* ---- dynamic libraries ---- */

void *pz_lib_open(const char *const *names)
{
    HMODULE h = NULL;
    for (int i = 0; names[i] && !h; i++) h = LoadLibraryA(names[i]);
    return (void *)h;
}

void *pz_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }
