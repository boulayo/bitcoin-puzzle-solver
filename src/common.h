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
#include <pzcore/defs.h>

#ifndef PZ_VERSION
#define PZ_VERSION "0.0.0-dev"
#endif
#ifndef PZ_BUILD_INFO
#define PZ_BUILD_INFO "unknown"
#endif

/* the fixed-width types and the attribute macros (PZ_ALIGN, PZ_UNUSED, PZ_VABI) come from pzcore */
typedef pz_u128 u128;
typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef uint8_t u8;

#define LIKELY(x)   PZ_LIKELY(x)
#define UNLIKELY(x) PZ_UNLIKELY(x)
#ifdef __MINGW32__
#define PZ_PRINTF gnu_printf   /* MinGW with __USE_MINGW_ANSI_STDIO: C99 formats (%zu, %Lf) */
#else
#define PZ_PRINTF printf
#endif

/* Exit status of 'hunt' (see 'pzsolve help hunt'). Every other command uses 0 for success and PZ_EXIT_ERROR for an error. */
enum {
    PZ_EXIT_FOUND = 0,           /* the key was found */
    PZ_EXIT_NOT_FOUND = 1,       /* the search finished (the whole range, or the whole --part) without the key */
    PZ_EXIT_ERROR = 2,           /* an error: invalid options, campaign in use, unreadable files, a state that could not be saved */
    PZ_EXIT_INTERRUPTED = 130    /* stopped by Ctrl-C, SIGTERM or the closing of the terminal: the state is saved */
};

/* Fatal error helper: prints and exits with status PZ_EXIT_ERROR. */
void pz_die(const char *fmt, ...) __attribute__((noreturn, format(PZ_PRINTF, 1, 2)));
bool pz_help(const char *topic, FILE *f);               /* the help of a command, or the general one (NULL); false: no such command */

/* Wall-clock helpers (implemented in platform.c). */
double pz_now(void);                 /* seconds since the epoch */
double pz_mono(void);                /* monotonic seconds */

/* Logging: always goes to the terminal (stderr) and, once a campaign is open,
 * to its log file. */
typedef enum { PZ_LOG_INFO, PZ_LOG_WARN, PZ_LOG_ERR, PZ_LOG_DEBUG } pz_loglevel;
void pz_log(pz_loglevel lvl, const char *fmt, ...) __attribute__((format(PZ_PRINTF, 2, 3)));
/* The same line in the log file only: for what the terminal already shows in another form (the header of the panel). */
void pz_log_file(pz_loglevel lvl, const char *fmt, ...) __attribute__((format(PZ_PRINTF, 2, 3)));
void pz_log_set_file(const char *path);
void pz_log_set_verbose(int v);
int pz_log_verbose(void);       /* -v: also the debug lines and the internal counters of the panel */
void pz_log_set_quiet(int q);   /* 0: everything is printed, 1: warnings and errors only, 2: nothing (the self-tests that provoke errors on purpose) */
int pz_log_quiet(void);
/* In-place dashboard: `text` is nlines lines joined by '\n' (no trailing newline). It replaces the previous block;
 * log messages erase it first and the reporter redraws it below them. */
void pz_ui_block(const char *text, int nlines);
void pz_log_terminal_line_break(void);   /* keeps the block on screen and moves the cursor below it */
/* Prints text with no line longer than `width` columns. A line that is too long is broken at a space (a word longer than the line
 * is left whole) and the continuation lines are indented by `hang` columns; with hang < 0 they are indented to the column where the
 * description of an option or command starts (the text after the first run of two or more spaces), or to the line's own indentation
 * plus two. */
void pz_fputs_wrap(FILE *fp, const char *text, int width, int hang);
#define PZ_LINE_MAX 128   /* longest line written to the terminal */

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
/* A number given on the command line (`name` is the option or the parameter): the whole text must be an integer between lo and hi,
 * otherwise the program ends with "NAME: invalid value 'TEXT' (expected an integer between LO and HI)". */
long long pz_arg_int(const char *name, const char *s, long long lo, long long hi);
/* Whether `s` is a list of non-negative integers in decimal, separated by single commas ("0", "0,2"), as --gpu takes. */
bool pz_index_list_valid(const char *s);
/* A speed in units per second: a number, decimals allowed, with an optional k, M, G or T ("800M", "1.5G"); positive, at most 1e15. */
bool pz_parse_rate(const char *s, double *out);

/* Finds "key<sep>value" in a text and returns a pointer to the first character after the separator (the value runs to the end of the line,
 * or of the text), NULL when there is none. By default the key must stand at the start of a line, and the separator right after it; of
 * several matches the first is taken. Flags:
 *   PZ_KV_ANYWHERE  the key may start anywhere in the text, not only at the start of a line
 *   PZ_KV_LAST      take the last match instead of the first
 *   PZ_KV_PAD       spaces and tabs may stand between the key and the separator ("model name   : value")
 * The text is a C string: an embedded NUL ends it. Nothing is skipped after the separator and nothing is copied: the caller decides how
 * the value ends and what blanks mean. A key that is only the beginning of a longer name does not match ("cbits" in "cbitsx=1"). */
enum { PZ_KV_ANYWHERE = 1, PZ_KV_LAST = 2, PZ_KV_PAD = 4 };
const char *pz_kv_find(const char *text, const char *key, char sep, unsigned flags);

/* Hex helpers. */
int pz_hex_decode(const char *hex, u8 *out, size_t maxlen);   /* returns bytes or -1 */
void pz_hex_encode(const u8 *in, size_t n, char *out);        /* out needs 2n+1 */

/* 128-bit decimal/hex formatting (buf >= 41 bytes). */
char *u128_to_dec(u128 v, char *buf);
/* Human readable quantities: 1.23 G, etc. buf >= 32 bytes */
char *pz_fmt_si(double v, char *buf, size_t n);
char *pz_group_digits(const char *in, char *out, size_t n);   /* "1234567" -> "1,234,567" */
char *pz_fmt_duration(double secs, char *buf, size_t n);
char *pz_fmt_u64(u64 v, char *buf, size_t n);                  /* 1234567 -> "1,234,567" */
char *pz_fmt_percent(double frac, char *buf, size_t n);        /* 0.123456 -> "12.3456%", 1.2e-9 -> "1.200e-07%" */
char *pz_fmt_age(double secs, char *buf, size_t n);            /* "just now", "7s ago", "3m 12s ago", "2h 5m ago" */
char *pz_fmt_bytes(double v, char *buf, size_t n);

/* Atomic write: writes data to a temporary file of its own next to path (so concurrent writers never share one), fsyncs, renames over path. */
int pz_write_file_atomic(const char *path, const void *data, size_t len, int mode);
/* The same; durable false skips the fsync, for files that are rewritten every few seconds and made again at the next start (report.txt,
 * status.json): after a crash the old content, or none, may be seen. */
int pz_write_file_atomic_ex(const char *path, const void *data, size_t len, int mode, bool durable);
/* Sets the line "key=value" in the text file `path` (one such line per key, none of the keys contains '='), keeping the other lines: the cache
 * files shared by every pzsolve of the user. Atomic like pz_write_file_atomic, and serialised between programs by a lock on path.lock when
 * the system gives one. 0 on success. */
int pz_keyed_file_put(const char *path, const char *key, const char *val);
/* The lines of such a file read whole (pz_read_file), one at a time, cut out in place: *pos starts at the text (NULL: none) and moves on; *eq is
 * the '=' that ends the key, NULL on a line without one. NULL at the end of the text. */
char *pz_keyed_line(char **pos, char **eq);
/* Reads a whole file into a malloc'd, NUL-terminated buffer. */
char *pz_read_file(const char *path, size_t *len);
/* Number of places where `needle` (not empty) occurs in the file, overlapping ones included. The file is read in blocks of 64 KiB, so a log of
 * tens of MB costs no memory. -1 when it cannot be opened. */
long long pz_file_count(const char *path, const char *needle);
int pz_mkdir_p(const char *path);
/* Path of a file in the per-user cache directory of the program (created if needed): $XDG_CACHE_HOME/pzsolve, %LOCALAPPDATA%\pzsolve
 * or ~/.cache/pzsolve. False when there is no such directory. */
bool pz_cache_file(const char *name, char *out, size_t n);

#endif
