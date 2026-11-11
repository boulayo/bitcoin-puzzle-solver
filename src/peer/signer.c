/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* signer.c - the pzsign process (see signer.h). */
#include "signer.h"
#include "sys.h"
#include <pzcore/seal.h>
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

bool signer_primes(const char *path, const char *file)
{
    const char *const argv[] = {path, "primes", file, NULL};
    int st;
    const sys_proc p = sys_spawn(argv, NULL, NULL, NULL);
    return p && sys_proc_ended(p, true, &st) && st == 0;
}

static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = D[p[i] >> 4], out[2 * i + 1] = D[p[i] & 15];
    out[2 * n] = 0;
}

char *signer_job(int i, const uint8_t eid[32], const uint8_t share[32], const uint8_t pub[2][33], const uint8_t key[33], const char *primes,
                 const uint8_t (*digests)[32], int ndigests)
{
    for (const char *c = primes; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) return NULL;   /* a path that would need escaping: not one of ours */
    const size_t cap = 512 + strlen(primes) + (size_t)ndigests * 70;
    char *j = malloc(cap), e[65], sh[65], p0[67], p1[67], k[67], d[65];
    if (!j) return NULL;
    hex(eid, 32, e), hex(share, 32, sh), hex(pub[0], 33, p0), hex(pub[1], 33, p1), hex(key, 33, k);
    size_t o = (size_t)snprintf(j, cap, "{\"op\": \"sign\", \"i\": %d, \"eid\": \"%s\", \"share\": \"%s\", \"pub\": [\"%s\", \"%s\"], \"key\": \"%s\", "
                                         "\"primes\": \"%s\", \"digests\": [", i, e, sh, p0, p1, k, primes);
    for (int n = 0; n < ndigests; n++) {
        hex(digests[n], 32, d);
        o += (size_t)snprintf(j + o, cap - o, "%s\"%s\"", n ? ", " : "", d);
    }
    snprintf(j + o, cap - o, "]}");
    pz_secure_zero(sh, sizeof sh);
    return j;
}
