/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dbg.h - the debug log of pzpeer (--debug FILE): a line with its time for every message sent and received, every step of a session and
 * every answer of the chain, to read afterwards what happened. Nothing secret is ever passed to it: no seed, key, share, distance or
 * prime. Without --debug it does nothing. */
#ifndef PZPEER_DBG_H
#define PZPEER_DBG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool dbg_open(const char *path);   /* appends to the file; false when it cannot be opened */
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* the first bytes of p as hex text (8 at most), for a line: a static buffer of the calling thread */
const char *dbg_hex(const uint8_t *p, size_t n);

#endif
