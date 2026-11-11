/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* electrum.c - the client of the Electrum protocol (see electrum.h). Every wait has the deadline of the call (sys.h). */
#include "dbg.h"
#include "electrum.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>

/* the next line into *line (a pointer into the buffer, NUL-terminated, valid until the next call) */
static bool read_line(electrum *e, int64_t deadline, char **line, size_t *len)
{
    static const size_t CHUNK = 65536;
    size_t scanned = 0;
    for (;;) {
        char *nl = e->len > scanned ? memchr(e->buf + scanned, '\n', e->len - scanned) : NULL;
        if (nl) {
            *nl = 0;
            *line = e->buf, *len = (size_t)(nl - e->buf);
            return true;
        }
        scanned = e->len;
        if (e->len >= ELECTRUM_LINE_MAX) return false;         /* a line with no end: not an Electrum server, or a hostile one */
        if (e->cap - e->len < CHUNK) {
            char *p = realloc(e->buf, e->cap + CHUNK);
            if (!p) return false;
            e->buf = p, e->cap += CHUNK;
        }
        const long r = sys_recv(e->fd, e->buf + e->len, e->cap - e->len, deadline);
        if (r <= 0) return false;                              /* closed, an error, or too late */
        e->len += (size_t)r;
    }
}

/* drops the line read_line gave */
static void drop_line(electrum *e, size_t len)
{
    memmove(e->buf, e->buf + len + 1, e->len - len - 1);
    e->len -= len + 1;
}

void electrum_close(electrum *e)
{
    sys_close(e->fd);
    free(e->buf);
    *e = (electrum){.fd = -1};
}

bool electrum_call(electrum *e, const char *method, json_w *params, json_doc *doc, const json_val **result)
{
    *doc = (json_doc){0};
    *result = NULL;
    const int64_t deadline = sys_now_ms() + e->timeout_ms;
    const char *pt = "[]";
    size_t pl = 2, bl = 0;
    bool ok = e->fd >= 0 && (!params || (pt = jw_done(params, &pl)) != NULL);
    const uint64_t id = ++e->id;
    json_w w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "jsonrpc"), jw_str(&w, "2.0");
    jw_key(&w, "id"), jw_u64(&w, id);
    jw_key(&w, "method"), jw_str(&w, method);
    jw_key(&w, "params");
    if (ok) jw_raw(&w, pt, pl);
    else jw_null(&w);
    jw_end(&w);
    const char *body = jw_done(&w, &bl);
    ok = ok && body && sys_send_all(e->fd, body, bl, deadline) && sys_send_all(e->fd, "\n", 1, deadline);
    jw_free(&w);
    if (params) jw_free(params);
    bool answered = false;
    while (ok) {                                               /* the answer to this call: notifications and stale answers are skipped */
        char *line;
        size_t len;
        uint64_t got;
        if (!(ok = read_line(e, deadline, &line, &len))) break;
        const bool parsed = json_parse(doc, line, len);
        drop_line(e, len);
        if (!parsed) {
            ok = false;
            break;
        }
        if (json_u64(json_get(doc->v, "id"), &got) && got == id) {
            const json_val *err = json_get(doc->v, "error");
            *result = json_get(doc->v, "result");
            ok = *result && (!err || err->type == JSON_NULL);
            answered = true;
            break;
        }
        json_free(doc);
    }
    if (!ok) {
        json_free(doc);
        *result = NULL;
    }
    dbg("electrum fd %d %s: %s", e->fd, method, ok ? "ok" : answered ? "error" : "no answer");
    if (!answered) electrum_close(e);                          /* nothing more can be asked on a connection that lost an answer */
    return ok;
}

bool electrum_open(electrum *e, const char *hostport, int timeout_ms)
{
    char host[256];
    *e = (electrum){.fd = -1, .timeout_ms = timeout_ms};
    const char *colon = strrchr(hostport, ':');
    if (!colon || !colon[1] || colon == hostport) return false;
    size_t hl = (size_t)(colon - hostport);
    const char *h = hostport;
    if (h[0] == '[' && hl >= 2 && h[hl - 1] == ']') h++, hl -= 2;   /* [v6]:port */
    if (hl >= sizeof host) return false;
    memcpy(host, h, hl), host[hl] = 0;
    e->fd = sys_connect_host(host, colon + 1, sys_now_ms() + timeout_ms);
    dbg("electrum %s: %s (fd %d)", hostport, e->fd < 0 ? "no connection" : "connected", e->fd);
    if (e->fd < 0) return false;
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w), jw_str(&w, "pzpeer"), jw_str(&w, "1.4"), jw_end(&w);
    if (!electrum_call(e, "server.version", &w, &d, &res)) {
        electrum_close(e);
        return false;
    }
    json_free(&d);
    return true;
}
