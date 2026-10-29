/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* rt.c - dispatch of the GPU runtime interface to OpenCL or CUDA, and the combined device list. */
#include "rt_priv.h"
#include "envopts.h"
#include <stdarg.h>
#include <pthread.h>

const char *rt_api_name(rt_api a) { return a == RT_CUDA ? "CUDA" : "OpenCL"; }

/* ---------------- log lines of devices that start at the same time ---------------- */

typedef struct { pz_loglevel lvl; char *msg; } log_line;
typedef struct { log_line *v; int n, cap; bool done; } log_slot;
struct rt_logseq {
    pthread_mutex_t mu;
    int n, head;                 /* slots; the first one that is not done */
    log_slot *slot;
    rt_log_sink sink;
    void *ctx;
};

static _Thread_local struct { rt_logseq *q; int slot; } t_log;

static void log_emit(rt_logseq *q, pz_loglevel lvl, const char *msg)
{
    if (q->sink) q->sink(q->ctx, lvl, msg);
    else pz_log(lvl, "%s", msg);
}

void rt_log(pz_loglevel lvl, const char *fmt, ...)
{
    char msg[1024];                                         /* what pz_log keeps of a line */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_logseq *q = t_log.q;
    if (!q) { pz_log(lvl, "%s", msg); return; }
    pthread_mutex_lock(&q->mu);
    if (t_log.slot == q->head) {
        log_emit(q, lvl, msg);
    } else {
        log_slot *s = &q->slot[t_log.slot];
        if (s->n == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 16;
            s->v = pz_xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
        }
        s->v[s->n].lvl = lvl;
        s->v[s->n++].msg = pz_xstrdup(msg);
    }
    pthread_mutex_unlock(&q->mu);
}

rt_logseq *rt_logseq_new(int slots, rt_log_sink sink, void *ctx)
{
    rt_logseq *q = pz_xcalloc(1, sizeof *q);
    pthread_mutex_init(&q->mu, NULL);
    q->n = slots;
    q->slot = pz_xcalloc((size_t)slots, sizeof *q->slot);
    q->sink = sink;
    q->ctx = ctx;
    return q;
}

void rt_logseq_enter(rt_logseq *q, int slot)
{
    t_log.q = q;
    t_log.slot = slot;
}

void rt_logseq_done(rt_logseq *q, int slot)
{
    t_log.q = NULL;
    pthread_mutex_lock(&q->mu);
    q->slot[slot].done = true;
    while (q->head < q->n && q->slot[q->head].done) {
        if (++q->head >= q->n) break;
        log_slot *s = &q->slot[q->head];                    /* the next slot is the one that prints at once now: what it kept comes first */
        for (int i = 0; i < s->n; i++) { log_emit(q, s->v[i].lvl, s->v[i].msg); free(s->v[i].msg); }
        s->n = 0;
    }
    pthread_mutex_unlock(&q->mu);
}

void rt_logseq_free(rt_logseq *q)
{
    if (!q) return;
    for (int i = 0; i < q->n; i++) {
        for (int k = 0; k < q->slot[i].n; k++) free(q->slot[i].v[k].msg);
        free(q->slot[i].v);
    }
    pthread_mutex_destroy(&q->mu);
    free(q->slot);
    free(q);
}

int rt_enumerate(rt_dev *out, int max, char *note, size_t nl)
{
    char n1[160] = "", n2[160] = "";
    int nc = 0, no = 0;
    if (!g_env.no_cuda) nc = rtcu_enumerate(out, max, n1, sizeof n1);
    if (!g_env.no_opencl) no = rtcl_enumerate(out + nc, max - nc, n2, sizeof n2);
    int n = nc + no;
    for (int i = 0; i < n; i++) { out[i].index = i; out[i].dup_of = -1; }
    /* the NVIDIA OpenCL driver exposes the same GPUs that CUDA drives: pair them by PCI location, or by name and order
     * when the location is unknown, so a GPU is not used twice */
    bool taken[RT_MAX_DEVS] = {false};
    for (int i = nc; i < n; i++) {
        if (!out[i].is_gpu) continue;
        for (int j = 0; j < nc; j++) {
            if (taken[j]) continue;
            bool same = out[i].pci_bus >= 0 && out[j].pci_bus >= 0 ? out[i].pci_bus == out[j].pci_bus && out[i].pci_dev == out[j].pci_dev
                                                                     : strcmp(out[i].name, out[j].name) == 0;
            if (same) { out[i].dup_of = j; taken[j] = true; break; }
        }
    }
    if (note) {
        note[0] = 0;
        if (n1[0] && n2[0]) snprintf(note, nl, "%s; %s", n1, n2);
        else snprintf(note, nl, "%s", n1[0] ? n1 : n2);
    }
    return n;
}

rt_ctx *rt_open(const rt_dev *d)
{
    rt_ctx *c = pz_xcalloc(1, sizeof *c);
    c->api = d->api;
    c->dev = *d;
    bool ok = c->api == RT_CUDA ? rtcu_open(c) : rtcl_open(c);
    if (!ok) { free(c); return NULL; }
    return c;
}

void rt_close(rt_ctx *c)
{
    if (!c) return;
    if (c->api == RT_CUDA) rtcu_close(c); else rtcl_close(c);
    free(c);
}

rt_mod *rt_build(rt_ctx *c, rt_prog_id prog, int param)
{
    rt_mod *m = pz_xcalloc(1, sizeof *m);
    m->c = c;
    bool ok = c->api == RT_CUDA ? rtcu_build(m, prog, param) : rtcl_build(m, prog, param);
    if (!ok) { free(m); return NULL; }
    return m;
}

void rt_mod_free(rt_mod *m)
{
    if (!m) return;
    if (m->c->api == RT_CUDA) rtcu_mod_free(m); else rtcl_mod_free(m);
    free(m);
}

rt_kern *rt_kernel(rt_mod *m, const char *name, int nargs)
{
    if (!m || nargs < 0 || nargs >= RT_MAX_ARGS) return NULL;
    rt_kern *k = pz_xcalloc(1, sizeof *k);
    k->m = m;
    k->nargs = nargs;
    bool ok = m->c->api == RT_CUDA ? rtcu_kernel(k, name) : rtcl_kernel(k, name);
    if (!ok) { free(k); return NULL; }
    return k;
}

void rt_kern_free(rt_kern *k)
{
    if (!k) return;
    if (k->m->c->api == RT_CUDA) rtcu_kern_free(k); else rtcl_kern_free(k);
    free(k);
}

rt_buf *rt_buf_new(rt_ctx *c, size_t bytes, const void *init)
{
    rt_buf *b = pz_xcalloc(1, sizeof *b);
    b->c = c;
    b->bytes = bytes ? bytes : 4;
    bool ok = c->api == RT_CUDA ? rtcu_buf_new(b, init) : rtcl_buf_new(b, init);
    if (!ok) {                                           /* the device memory may be allocated even when the copy or the clearing failed */
        if (c->api == RT_CUDA) rtcu_buf_free(b); else rtcl_buf_free(b);
        free(b);
        return NULL;
    }
    return b;
}

rt_buf *rt_buf_part(rt_buf *of, size_t off, size_t bytes)
{
    if (!of || !bytes || off + bytes > of->bytes || off % rt_part_align(of->c)) return NULL;
    rt_buf *b = pz_xcalloc(1, sizeof *b);
    b->c = of->c;
    b->bytes = bytes;
    b->part = true;
    if (!(of->c->api == RT_CUDA ? rtcu_buf_part(b, of, off) : rtcl_buf_part(b, of, off))) { free(b); return NULL; }
    return b;
}

size_t rt_part_align(rt_ctx *c) { return c->api == RT_CUDA ? 4 : rtcl_part_align(c); }

void rt_buf_free(rt_buf *b)
{
    if (!b) return;
    if (b->c->api == RT_CUDA) rtcu_buf_free(b); else rtcl_buf_free(b);
    free(b);
}

bool rt_write(rt_ctx *c, rt_buf *b, size_t off, const void *src, size_t n)
{
    if (!b || off + n > b->bytes) return false;
    if (!n) return true;
    return c->api == RT_CUDA ? rtcu_write(c, b, off, src, n) : rtcl_write(c, b, off, src, n);
}

bool rt_read(rt_ctx *c, rt_buf *b, size_t off, void *dst, size_t n)
{
    if (!b || off + n > b->bytes) return false;
    if (!n) return true;
    return c->api == RT_CUDA ? rtcu_read(c, b, off, dst, n) : rtcl_read(c, b, off, dst, n);
}

void rt_arg_buf(rt_kern *k, int i, rt_buf *b)
{
    if (!k || i < 0 || i >= k->nargs) return;
    k->is_buf[i] = true;
    k->buf[i] = b;
}

void rt_arg_u32(rt_kern *k, int i, uint32_t v)
{
    if (!k || i < 0 || i >= k->nargs) return;
    k->is_buf[i] = false;
    k->u32[i] = v;
}

bool rt_run(rt_ctx *c, rt_kern *k, size_t n)
{
    if (!k || n == 0 || n > 0xFFFFFFFFu) return false;
    for (int i = 0; i < k->nargs; i++) if (k->is_buf[i] && !k->buf[i]) return false;
    return c->api == RT_CUDA ? rtcu_run(c, k, n) : rtcl_run(c, k, n);
}
