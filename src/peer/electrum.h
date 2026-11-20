/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* electrum.h - a client of the Electrum protocol (JSON-RPC 2.0, one message a line, over plain TCP), which the public servers of the light
 * wallets speak. The connection is not encrypted and the server is not trusted: every answer is used only with its proof (spv.h). */
#ifndef PZPEER_ELECTRUM_H
#define PZPEER_ELECTRUM_H

#include "json.h"

#define ELECTRUM_LINE_MAX (8u << 20)   /* the longest answer taken: 2016 headers are 0.3 MB, a large transaction a few MB */

typedef struct {
    int fd;
    int timeout_ms;                    /* of a whole call */
    int64_t until;                     /* the end of the operation the calls are part of (sys_now_ms; 0: none): a slow server answers
                                        * each call in time and could still make a scan of a hundred calls last an hour */
    uint64_t id;
    char *buf;                         /* what was read and not used yet */
    size_t len, cap;
} electrum;

/* Connects to "host:port" ("[v6]:port" for an IPv6 address) and says hello (server.version). False when the server does not answer in time. */
bool electrum_open(electrum *e, const char *hostport, int timeout_ms);
void electrum_close(electrum *e);
/* One call: params is the JSON array of the parameters (a writer after jw_end; NULL for none; freed here). True with the result in *result
 * (a node of doc, which the caller frees with json_free); false, with doc empty, when the server sent an error, something that is not an
 * answer, or nothing in time; in the last two cases the connection is closed (fd < 0), and every later call on it is false. After until
 * nothing is sent, and the call is false. */
bool electrum_call(electrum *e, const char *method, json_w *params, json_doc *doc, const json_val **result);

#endif
