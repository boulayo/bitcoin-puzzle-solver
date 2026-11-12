/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* signer.c - the pzsign process (see signer.h). */
#include "signer.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>

bool signer_start(signer *s, const char *path)
{
    const char *const argv[] = {path, "session", NULL};
    s->to = s->from = -1;
    s->proc = sys_spawn(argv, NULL, &s->to, &s->from);
    return s->proc != 0;
}

bool signer_send(signer *s, uint8_t type, const uint8_t *payload, size_t len)
{
    if (len + 1 > SIGNER_FRAME_MAX) return false;
    const uint32_t l = (uint32_t)len + 1;
    const uint8_t head[5] = {(uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l, type};
    return sys_pipe_write(s->to, head, 5) && sys_pipe_write(s->to, payload, len);
}

void signer_end_input(signer *s)
{
    sys_pipe_close(s->to);
    s->to = -1;
}

bool signer_recv(signer *s, uint8_t *type, uint8_t **payload, size_t *len, int timeout_ms)
{
    uint8_t head[4];
    *payload = NULL;
    if (!sys_pipe_read(s->from, head, 4, timeout_ms)) return false;
    const uint32_t l = (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
    if (l == 0 || l > SIGNER_FRAME_MAX) return false;
    uint8_t *buf = malloc(l);
    if (!buf || !sys_pipe_read(s->from, buf, l, timeout_ms)) {
        free(buf);
        return false;
    }
    *type = buf[0];
    memmove(buf, buf + 1, l - 1);
    buf[l - 1] = 0;
    *payload = buf;
    *len = l - 1;
    return true;
}

void signer_kill(signer *s)
{
    if (s->proc) sys_proc_kill(s->proc);                       /* not waited for yet: the handle is still this process */
}

void signer_stop(signer *s)
{
    int st;
    sys_pipe_close(s->to), sys_pipe_close(s->from);
    if (s->proc && !sys_proc_ended(s->proc, false, &st)) sys_proc_kill(s->proc), (void)sys_proc_ended(s->proc, true, &st);   /* still running: nothing more to say */
    s->proc = 0, s->to = s->from = -1;
}

size_t signer_job(uint8_t out[SIGNER_JOB_MAX], int i, int first, const uint8_t eid[32], const uint8_t share[32], const uint8_t pub[2][33],
                  const uint8_t key[33], const uint8_t (*digests)[32], int ndigests)
{
    uint8_t *p = out;
    *p++ = 2, *p++ = (uint8_t)i, *p++ = (uint8_t)first;         /* 2: the version of the job and of the envelopes of pzsign */
    memcpy(p, eid, 32), p += 32;
    memcpy(p, share, 32), p += 32;
    memcpy(p, pub[0], 33), p += 33;
    memcpy(p, pub[1], 33), p += 33;
    memcpy(p, key, 33), p += 33;
    memcpy(p, digests, 32 * (size_t)ndigests), p += 32 * (size_t)ndigests;
    return (size_t)(p - out);
}
