/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* platform.h - the operating-system services the program needs, behind one interface.
 * Two implementations live in platform.c: POSIX (Linux and macOS, with a few #ifdef __APPLE__ shims) and Windows (MinGW-w64).
 * Everything else in the program is plain C11 + pthreads and does not test _WIN32. */
#ifndef PZ_PLATFORM_H
#define PZ_PLATFORM_H

#include "common.h"
#include <fcntl.h>
#include <time.h>

#ifdef __APPLE__
/* macOS gives secondary threads a 512 KiB stack (Linux: 8 MiB, Windows: the 8 MiB the linker reserves): every thread of the
 * program is created through this wrapper, which asks for 8 MiB unless the caller sets a size of its own. */
#include <pthread.h>
int pz_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
#define pthread_create pz_pthread_create
#endif

#ifndef O_BINARY
#define O_BINARY 0          /* Windows: no newline translation (every file we write is binary or LF-only text) */
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* ---- files (descriptors are always opened in binary mode, offsets are 64-bit) ---- */
int pz_open_read(const char *path);
int pz_open_rw(const char *path, bool create);
int pz_open_trunc(const char *path, int mode);            /* write only, created (with `mode` & ~umask; ignored on Windows) or truncated */
int pz_create_temp(char *tmpl);                            /* tmpl ends in "XXXXXX", replaced in place; read/write */
i64 pz_pread(int fd, void *buf, size_t n, u64 off);       /* bytes read, -1 on error (may be short) */
i64 pz_pwrite(int fd, const void *buf, size_t n, u64 off);
i64 pz_fd_size(int fd);                                    /* -1 on error */
int pz_fd_sync(int fd);
int pz_fd_truncate(int fd, u64 size);
int pz_truncate_path(const char *path, u64 size);          /* truncate(), with a 64-bit length on every system */
void pz_fd_drop_cache(int fd, u64 off, u64 len);           /* hint: these bytes will not be read again */
/* Replaces `to` with `from` atomically (POSIX rename semantics, also when `to` exists or is briefly held open by a
 * reader on Windows). 0 on success. */
int pz_rename_replace(const char *from, const char *to);
/* The same; durable false does not wait for the rename to reach the disk (Windows: no MOVEFILE_WRITE_THROUGH), for files that are made
 * again at the next write. */
int pz_rename_replace_ex(const char *from, const char *to, bool durable);
int pz_mkdir1(const char *path);                           /* one level; 0 also if it already exists as a directory */
bool pz_is_dir(const char *path);
int pz_remove_tree(const char *path);                      /* rm -rf, without spawning a shell */
bool pz_make_temp_dir(char *out, size_t n, const char *prefix);   /* in the system temporary directory */

/* Non-blocking advisory lock of a whole lock file: 0 = acquired, 1 = held by another process, -1 = error (errno). */
int pz_lock_try(int fd, bool exclusive);

/* ---- memory ---- */
/* Zero-filled, page-aligned, lazily committed region; *huge tells whether huge/large pages were obtained. */
void *pz_map_zeroed(size_t bytes, bool *huge);
void pz_unmap(void *p, size_t bytes);
/* Aligned heap blocks (pz_xaligned) must be released with pz_afree: on Windows free() cannot take them. */
void *pz_aligned_raw(size_t align, size_t n);
void pz_afree(void *p);

/* ---- time ---- */
double pz_now(void);                                       /* wall clock, seconds since the epoch */
double pz_mono(void);                                      /* monotonic seconds */
void pz_sleep(double seconds);
void pz_localtime(time_t t, struct tm *out);

/* ---- process / system ---- */
int pz_getpid(void);
void pz_hostname(char *buf, size_t n);
bool pz_os_random(void *buf, size_t n);                    /* cryptographic randomness from the OS */
void pz_os_description(char *buf, size_t n);               /* "Windows 10.0 build 22631", "Ubuntu 22.04.4 LTS (Linux 6.8)", "macOS 14.5 (Darwin 23.5.0)" */
bool pz_disk_space(const char *path, u64 *free_bytes, u64 *total_bytes);   /* of the file system holding `path` */
/* Windows: the active power scheme (name), "" elsewhere or when unknown; prevent_sleep(true) stops the system from going to
 * sleep while the caller lives (Windows: SetThreadExecutionState on the calling thread, macOS: a `caffeinate` helper for the process;
 * false if unsupported, e.g. Linux). */
void pz_power_scheme(char *buf, size_t n);
bool pz_prevent_sleep(bool on);
/* Raises (on) or restores the scheduling priority of the CALLING thread above the CPU workers': ABOVE_NORMAL on Windows, nice -5 on
 * Linux (needs root or CAP_SYS_NICE). False when the system refused: the thread keeps the priority it had. Always false on macOS,
 * which has no per-thread nice value. */
bool pz_thread_boost(bool on);
/* Whether pz_thread_boost works here: always on Windows; elsewhere tried once on a thread of its own (needs root or CAP_SYS_NICE). */
bool pz_boost_possible(void);
/* The priority of the calling thread as the system shows it (Linux: nice value, Windows: THREAD_PRIORITY_* level, macOS: 0): for tests. */
int pz_thread_prio_level(void);

/* ---- console ---- */
void pz_console_init(void);                                /* Windows: UTF-8 output and ANSI escape sequences */
bool pz_stderr_is_terminal(void);                          /* an interactive terminal that understands the escapes */
int pz_terminal_cols(void);                                /* 0 when unknown */
int pz_fd_terminal_cols(int fd);                           /* columns of the terminal that fd 1 or 2 writes to; 0 when it is not one */
/* Windows: the console window belongs to this process alone (started with a double click from Explorer), so it will
 * close as soon as the program ends. Always false elsewhere. */
bool pz_console_is_own(void);

/* ---- interruption ---- */
/* fn runs (possibly on another thread, or in a signal handler: it must only touch atomics / call _exit) on Ctrl-C,
 * SIGTERM, SIGHUP (unless ignored, e.g. under nohup) and, on Windows, when the console window is closed or the
 * user logs off. After a console close Windows waits for pz_interrupt_done() (a few seconds at most) so the state
 * can be saved before the process is terminated. */
void pz_on_interrupt(void (*fn)(void));
void pz_interrupt_done(void);

/* ---- dynamic libraries ---- */
void *pz_lib_open(const char *const *names);               /* first name that loads, NULL if none */
void *pz_lib_sym(void *lib, const char *name);

#endif
