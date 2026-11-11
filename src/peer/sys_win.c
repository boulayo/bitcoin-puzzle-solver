/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sys_win.c - sys.h on Windows. A socket and the end of a pipe are kernel handles, whose values fit an int; a file is a descriptor of
 * the C runtime, opened so that other programs can read it meanwhile (pzpeer reads the dp.bin that pzsolve writes). */
#include "sys.h"
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <share.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- time and randomness ---- */

int64_t sys_now_ms(void) { return (int64_t)GetTickCount64(); }
void sys_sleep_ms(int ms) { Sleep(ms > 0 ? (DWORD)ms : 0); }
bool sys_random(void *out, size_t n) { return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0; }

/* ---- sockets ---- */

/* Winsock is started before main, so that no caller has to think of it */
static void __attribute__((constructor)) net_init(void)
{
    WSADATA w;
    (void)WSAStartup(MAKEWORD(2, 2), &w);
}

static int again(void)
{
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR ? SYS_AGAIN : -1;
}

static int clamp(size_t n) { return n > INT_MAX ? INT_MAX : (int)n; }

/* non-blocking, not inherited; a TCP socket without Nagle */
static int prepared(SOCKET s)
{
    u_long on = 1;
    if (s == INVALID_SOCKET) return -1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) return closesocket(s), -1;
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    sys_setopt((int)s, IPPROTO_TCP, TCP_NODELAY, 1);           /* refused by a UDP socket: no matter */
    return (int)s;
}

int sys_socket(int family, int type) { return prepared(socket(family, type, 0)); }

int sys_accept(int lfd, struct sockaddr *from, socklen_t *len)
{
    const SOCKET s = accept(lfd, from, len);
    return s == INVALID_SOCKET ? (WSAGetLastError() == WSAECONNRESET ? SYS_AGAIN : again()) : prepared(s);
}

void sys_setopt(int fd, int level, int name, int value) { (void)setsockopt(fd, level, name, (const char *)&value, sizeof value); }

void sys_close(int fd)
{
    if (fd >= 0) closesocket(fd);
}

int sys_poll(struct pollfd *p, int n, int ms) { return WSAPoll(p, (ULONG)n, ms); }

/* select and not WSAPoll for the wait of a connect: before Windows 10 2004 WSAPoll does not report a connection that was refused */
int sys_connect(const struct sockaddr *to, socklen_t len, int64_t deadline)
{
    const int fd = sys_socket(to->sa_family, SOCK_STREAM);
    if (fd < 0) return -1;
    if (connect(fd, to, len) == 0) return fd;
    int why = WSAGetLastError(), wl = sizeof why;
    if (why == WSAEWOULDBLOCK) {
        const int64_t left = deadline - sys_now_ms();
        const struct timeval tv = {(long)(left / 1000), (long)(left % 1000) * 1000};
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET((SOCKET)fd, &w);
        FD_SET((SOCKET)fd, &e);
        const int r = left > 0 ? select(0, NULL, &w, &e, &tv) : 0;
        why = r == 0 ? WSAETIMEDOUT : WSAGetLastError();
        if (r > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&why, &wl) == 0 && why == 0) return fd;
    }
    closesocket(fd);
    WSASetLastError(why);
    return -1;
}

long sys_recv_now(int fd, void *p, size_t cap)
{
    const int r = recv(fd, p, clamp(cap), 0);
    return r < 0 ? again() : r;
}

long sys_send_now(int fd, const void *p, size_t n)
{
    const int r = send(fd, p, clamp(n), 0);
    return r < 0 ? again() : r;
}

long sys_recvfrom(int fd, void *p, size_t cap, struct sockaddr_in *from)
{
    int fl = sizeof *from;
    const int r = recvfrom(fd, p, clamp(cap), 0, (struct sockaddr *)from, &fl);
    return r < 0 ? again() : fl == sizeof *from ? r : -1;
}

long sys_sendto(int fd, const void *p, size_t n, const struct sockaddr_in *to)
{
    const int r = sendto(fd, p, clamp(n), 0, (const struct sockaddr *)to, sizeof *to);
    return r < 0 ? again() : r;
}

const char *sys_net_why(void)
{
    static _Thread_local char why[160];
    const int e = WSAGetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)e, 0, why, sizeof why, NULL))
        snprintf(why, sizeof why, "error %d", e);
    why[strcspn(why, "\r\n")] = 0;
    return why;
}

/* ---- child processes ---- */

/* argv as one command line, quoted as the C runtime of the child will read it back */
static bool command_line(const char *const argv[], char *out, size_t cap)
{
    size_t o = 0;
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        const bool quote = !*a || strpbrk(a, " \t\"");
        if (o + 2 * strlen(a) + 4 > cap) return false;
        if (i) out[o++] = ' ';
        if (quote) out[o++] = '"';
        for (size_t k = 0; a[k]; k++) {
            size_t slashes = 0;
            while (a[k] == '\\') slashes++, k++;
            /* backslashes are themselves, except before a quote (or the closing one), where each is written twice */
            const bool before_quote = a[k] == '"' || (!a[k] && quote);
            for (size_t j = 0; j < (before_quote ? 2 * slashes : slashes); j++) out[o++] = '\\';
            if (!a[k]) break;
            if (a[k] == '"') out[o++] = '\\';
            out[o++] = !i && a[k] == '/' ? '\\' : a[k];        /* the program: its path as Windows writes it */
        }
        if (quote) out[o++] = '"';
    }
    out[o] = 0;
    return true;
}

static int pipe_fd(HANDLE h) { return (int)(intptr_t)h; }
static HANDLE pipe_handle(int fd) { return (HANDLE)(intptr_t)fd; }

sys_proc sys_spawn(const char *const argv[], const char *log, int *to, int *from)
{
    char cmd[8192];
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    HANDLE made[4] = {NULL, NULL, NULL, NULL}, std[3], list[3], me = GetCurrentProcess();
    HANDLE in_w = NULL, out_r = NULL;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOEXA si = {.StartupInfo = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES}};
    SIZE_T size = 0;
    int nmade = 0, nlist = 0;
    bool ok = command_line(argv, cmd, sizeof cmd);
    /* the three standard handles of the child: the pipes, the log, or copies of those of pzpeer that the child can inherit */
    for (int i = 0; ok && i < 3; i++) {
        std[i] = NULL;
        if (to && i == 0) ok = CreatePipe(&std[0], &in_w, &inherit, 0) && SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        else if (to && i == 1) ok = CreatePipe(&out_r, &std[1], &inherit, 0) && SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        else if (log && i == 0) std[0] = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
        else if (log && i == 1)
            std[1] = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else if (log) {
            std[2] = std[1];
            continue;
        } else {
            const HANDLE own = GetStdHandle(i == 0 ? STD_INPUT_HANDLE : i == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
            if (own && own != INVALID_HANDLE_VALUE && !DuplicateHandle(me, own, me, &std[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) std[i] = NULL;
        }
        if (std[i] == INVALID_HANDLE_VALUE) std[i] = NULL, ok = false;
        if (std[i]) made[nmade++] = list[nlist++] = std[i];
    }
    si.StartupInfo.hStdInput = std[0], si.StartupInfo.hStdOutput = std[1], si.StartupInfo.hStdError = std[2];
    /* only those handles reach the child, whatever else pzpeer has open: the sockets, dp.bin, the pipes of the other sessions */
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    si.lpAttributeList = ok ? malloc(size) : NULL;
    ok = si.lpAttributeList && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
    if (ok) {
        ok = (!nlist || UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, (SIZE_T)nlist * sizeof *list, NULL, NULL)) &&
             CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    free(si.lpAttributeList);
    for (int i = 0; i < nmade; i++) CloseHandle(made[i]);
    if (!ok) {
        if (in_w) CloseHandle(in_w);
        if (out_r) CloseHandle(out_r);
        return 0;
    }
    CloseHandle(pi.hThread);
    if (to) *to = pipe_fd(in_w), *from = pipe_fd(out_r);
    return (sys_proc)pi.hProcess;
}

int sys_proc_ended(sys_proc p, bool block, int *status)
{
    DWORD code = 0;
    if (WaitForSingleObject((HANDLE)p, block ? INFINITE : 0) != WAIT_OBJECT_0) return 0;
    *status = GetExitCodeProcess((HANDLE)p, &code) && code < 256 ? (int)code : -1;
    CloseHandle((HANDLE)p);
    return 1;
}

void sys_proc_kill(sys_proc p) { TerminateProcess((HANDLE)p, (UINT)-1); }   /* a status no program ends with by itself */

/* a Ctrl-Break to its process group, which is the child alone (pzsolve takes it as a Ctrl-C); a pzpeer that has no console cannot
 * send one, and ends the child */
void sys_proc_interrupt(sys_proc p)
{
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetProcessId((HANDLE)p))) sys_proc_kill(p);
}

bool sys_pipe_write(int fd, const void *p, size_t n)
{
    while (n) {
        DWORD w = 0;
        if (!WriteFile(pipe_handle(fd), p, (DWORD)clamp(n), &w, NULL) || !w) return false;
        p = (const char *)p + w, n -= w;
    }
    return true;
}

/* a pipe has no wait with a time limit: it is looked at every few milliseconds until bytes are there */
bool sys_pipe_read(int fd, void *p, size_t n, int timeout_ms)
{
    while (n) {
        DWORD there = 0, got = 0;
        for (const int64_t until = sys_now_ms() + timeout_ms; !there; Sleep(2))
            if (!PeekNamedPipe(pipe_handle(fd), NULL, 0, NULL, &there, NULL) || (!there && sys_now_ms() >= until)) return false;
        if (!ReadFile(pipe_handle(fd), p, there < n ? there : (DWORD)clamp(n), &got, NULL) || !got) return false;
        p = (char *)p + got, n -= got;
    }
    return true;
}

void sys_pipe_close(int fd)
{
    if (fd >= 0) CloseHandle(pipe_handle(fd));
}

/* ---- files ---- */

static bool is_dir(const char *path)
{
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool sys_mkdir(const char *path) { return CreateDirectoryA(path, NULL) || is_dir(path); }
bool sys_private(const char *path) { return is_dir(path); }

static int open_flags(const char *path, int flags)
{
    int fd = -1;
    return _sopen_s(&fd, path, flags | _O_BINARY | _O_NOINHERIT, _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 ? fd : -1;
}

int sys_open_secret(const char *path)
{
    const int fd = open_flags(path, _O_RDONLY);
    return fd >= 0 ? fd : errno == ENOENT ? -1 : -2;
}

int sys_create_secret(const char *path) { return open_flags(path, _O_WRONLY | _O_CREAT | _O_EXCL); }
int sys_open_rw(const char *path) { return open_flags(path, _O_RDWR | _O_CREAT); }
FILE *sys_fopen(const char *path, const char *mode) { return fopen(path, mode); }   /* sys_spawn gives a child nothing but its own handles */

long sys_pread(int fd, void *p, size_t n, uint64_t off)
{
    OVERLAPPED at = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32)};
    DWORD got = 0;
    return ReadFile((HANDLE)_get_osfhandle(fd), p, (DWORD)clamp(n), &got, &at) || GetLastError() == ERROR_HANDLE_EOF ? (long)got : -1;
}

long sys_pwrite(int fd, const void *p, size_t n, uint64_t off)
{
    OVERLAPPED at = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32)};
    DWORD put = 0;
    return WriteFile((HANDLE)_get_osfhandle(fd), p, (DWORD)clamp(n), &put, &at) ? (long)put : -1;
}

int64_t sys_size(int fd) { return _filelengthi64(fd); }
bool sys_truncate(int fd, uint64_t size) { return _chsize_s(fd, (__int64)size) == 0; }
bool sys_sync(int fd) { return _commit(fd) == 0; }
bool sys_replace(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0; }

void sys_remove_prefixed(const char *dir, const char *prefix)
{
    char p[1400];
    WIN32_FIND_DATAA f;
    snprintf(p, sizeof p, "%s/%s*", dir, prefix);
    const HANDLE h = FindFirstFileA(p, &f);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &f)) {
        snprintf(p, sizeof p, "%s/%s", dir, f.cFileName);
        DeleteFileA(p);
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
}

/* ---- the stop ---- */

static void (*g_on_stop)(int);

/* Ctrl-C and Ctrl-Break ask once; when the console closes or the user logs off the system ends the process as soon as this returns, so
 * it does not return: the main thread stops what it runs and ends the process itself, within the seconds the system grants. */
static BOOL WINAPI on_console(DWORD type)
{
    g_on_stop(0);
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) Sleep(INFINITE);
    return TRUE;
}

void sys_on_stop(void (*fn)(int))
{
    g_on_stop = fn;
    SetConsoleCtrlHandler(on_console, TRUE);
}
