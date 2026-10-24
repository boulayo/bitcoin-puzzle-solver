/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* common.h - basic types, helpers and logging shared by every module. */
#ifndef PZ_COMMON_H
#define PZ_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PZ_VERSION
#define PZ_VERSION "0.0.0-dev"
#endif
#ifndef PZ_BUILD_INFO
#define PZ_BUILD_INFO "unknown"
#endif

typedef unsigned __int128 u128;
typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef uint8_t u8;

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PZ_ALIGN(n) __attribute__((aligned(n)))
#define PZ_UNUSED   __attribute__((unused))
/* Win64: hot internal routines use the System V calling convention, so a call the compiler did not inline does not have
 * to save and restore xmm6-xmm15 (callee-saved in the Microsoft ABI) - worth ~20% in the SIMD-lane engines. */
#if defined(_WIN32) && defined(__x86_64__)
#define PZ_VABI __attribute__((sysv_abi))
#else
#define PZ_VABI
#endif
#ifdef __MINGW32__
#define PZ_PRINTF gnu_printf   /* MinGW with __USE_MINGW_ANSI_STDIO: C99 formats (%zu, %Lf) */
#else
#define PZ_PRINTF printf
#endif

/* Fatal error helper: prints and exits with status 2. */
void pz_die(const char *fmt, ...) __attribute__((noreturn, format(PZ_PRINTF, 1, 2)));

/* Wall-clock helpers (implemented in platform.c). */
double pz_now(void);                 /* seconds since the epoch */
double pz_mono(void);                /* monotonic seconds */

/* Logging: always goes to the terminal (stderr) and, once a campaign is open,
 * to its log file. */
typedef enum { PZ_LOG_INFO, PZ_LOG_WARN, PZ_LOG_ERR, PZ_LOG_DEBUG } pz_loglevel;
void pz_log(pz_loglevel lvl, const char *fmt, ...) __attribute__((format(PZ_PRINTF, 2, 3)));
void pz_log_set_file(const char *path);
void pz_log_set_verbose(int v);
void pz_log_set_quiet(int q);
int pz_log_quiet(void);
/* An on/off environment variable: set to anything but "" or "0" (PZ_NO_CUDA=0 does not switch CUDA off). */
bool pz_env_on(const char *name);
/* In-place dashboard: `text` is nlines lines joined by '\n' (no trailing newline). It replaces the previous block;
 * log messages erase it first and the reporter redraws it below them. */
void pz_ui_block(const char *text, int nlines);
void pz_log_terminal_line_break(void);   /* keeps the block on screen and moves the cursor below it */

/* Memory helpers that abort on failure. */
void *pz_xmalloc(size_t n);
void *pz_xcalloc(size_t n, size_t sz);
void *pz_xaligned(size_t align, size_t n);   /* zeroed, aligned; release with pz_afree() */
void pz_afree(void *p);
void *pz_xrealloc(void *p, size_t n);
char *pz_xstrdup(const char *s);

/* Bounded string copy (always NUL-terminates, silently truncates). */
void pz_strlcpy(char *dst, const char *src, size_t n);

/* Decimal parsing without strtol/atoi/sscanf: on glibc >= 2.38 those resolve to __isoc23_* symbols and would
 * make the binary refuse to start on older distributions. */
int pz_atoi(const char *s);
u64 pz_strtou64(const char *s);
/* Strict versions for user input: the whole string must be a decimal number (optional sign for i64) without overflow. */
bool pz_parse_i64(const char *s, i64 *out);
bool pz_parse_u64(const char *s, u64 *out);

/* Hex helpers. */
int pz_hex_decode(const char *hex, u8 *out, size_t maxlen);   /* returns bytes or -1 */
void pz_hex_encode(const u8 *in, size_t n, char *out);        /* out needs 2n+1 */

/* 128-bit decimal/hex formatting (buf >= 41 bytes). */
char *u128_to_dec(u128 v, char *buf);
/* Human readable quantities: 1.23 G, etc. buf >= 32 bytes */
char *pz_fmt_si(double v, char *buf, size_t n);
char *pz_fmt_duration(double secs, char *buf, size_t n);
char *pz_fmt_bytes(double v, char *buf, size_t n);

/* Atomic write: writes data to path.tmp, fsyncs, renames over path. */
int pz_write_file_atomic(const char *path, const void *data, size_t len, int mode);
/* Reads a whole file into a malloc'd, NUL-terminated buffer. */
char *pz_read_file(const char *path, size_t *len);
int pz_mkdir_p(const char *path);
/* Path of a file in the per-user cache directory of the program (created if needed): $XDG_CACHE_HOME/pzsolve, %LOCALAPPDATA%\pzsolve
 * or ~/.cache/pzsolve. False when there is no such directory. */
bool pz_cache_file(const char *name, char *out, size_t n);

#endif
