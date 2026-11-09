/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_relay.c - the relay (docs/en/NETWORK.md 7.7): a node that listens joins the connections of two homes that cannot dial each other;
 * the channel of a session, dialled, the partner's own connection, or through the relay of the slot of time. */
#include "dht_priv.h"
#include <inttypes.h>
#include <pzcore/hash.h>
#include <stdlib.h>

#define RELAY_PER_BLOCK 2          /* relays (waiting or under way) of the homes of one address block, */
#define RELAY_PER_ID 2             /* ... and waits of one identity */


typedef struct {
    dht *d;
    relay_slot *slot;
    net_conn *a, *b;
} relay_job;

static void *relay_main(void *arg)
{
    relay_job *j = arg;
    dht *d = j->d;
    const uint64_t n = net_splice(j->a, j->b, d->cfg.relay_bytes, d->relay_ms, &d->stop);
    dlog(d, "a relay ended: %" PRIu64 " bytes", n);
    pthread_mutex_lock(&d->r_mu);                              /* the last use of d: dht_stop may free it once this is unlocked */
    j->slot->used = false;
    d->nactive--;
    d->relays_done++;
    pthread_cond_broadcast(&d->r_cv);
    pthread_mutex_unlock(&d->r_mu);
    free(j);
    return NULL;
}

/* the relays (waiting or under way) that count against a home at block blk with identity id (r_mu held) */
static void relay_count(dht *d, const uint8_t blk[16], const uint8_t id[32], int *in_block, int *of_id)
{
    *in_block = *of_id = 0;
    for (int i = 0; i < RELAY_WAITING; i++) {
        const relay_slot *w = &d->waiting[i];
        if (!w->used) continue;
        *in_block += w->limited[0] && !memcmp(w->block[0], blk, 16);
        *of_id += !memcmp(w->from, id, 32);
    }
    for (int i = 0; i < d->cfg.relay_max; i++)
        for (int k = 0; k < 2; k++) *in_block += d->active[i].used && d->active[i].limited[k] && !memcmp(d->active[i].block[k], blk, 16);
}

/* the waits that are over, closed (r_mu held) */
void relay_sweep(dht *d)
{
    const int64_t now = now_ms();
    for (int i = 0; i < RELAY_WAITING; i++)
        if (d->waiting[i].used && now >= d->waiting[i].until) {
            net_close(d->waiting[i].c);
            d->waiting[i].used = false;
        }
}

/* RELAY(name, partner) from a home on c: it waits here for the partner's RELAY(name, this home), within the limits; when the partner is
 * waiting, both get RELAYED and a thread of their own copies their bytes. True when this node keeps c (waiting, or relayed). */
bool relay_in(dht *d, net_conn *c, const uint8_t *m, size_t len)
{
    if (len != 65 || !d->port || !memcmp(m + 33, c->remote, 32)) return false;
    const uint8_t *name = m + 1, *to = m + 33;
    uint8_t blk[16];
    net_addr_block(&c->addr, blk);
    const bool lim = limited(d, &c->addr);
    net_conn *other = NULL;
    relay_slot *act = NULL;
    bool kept = false;
    pthread_mutex_lock(&d->r_mu);
    relay_sweep(d);
    for (int i = 0; i < RELAY_WAITING && !other; i++) {
        relay_slot *w = &d->waiting[i];
        if (!w->used || memcmp(w->name, name, 32) || memcmp(w->from, to, 32) || memcmp(w->to, c->remote, 32)) continue;
        w->used = false;
        if (!net_alive(w->c)) {                                /* the partner went away while it waited */
            net_close(w->c);
            continue;
        }
        other = w->c;
        for (int k = 0; k < d->cfg.relay_max && !act; k++)
            if (!d->active[k].used) act = &d->active[k];
        if (act) {
            *act = *w;
            act->used = true;
            act->limited[1] = lim;
            memcpy(act->block[1], blk, 16);
            act->c = NULL;
            d->nactive++;
        }
    }
    if (!other) {
        int in_block, of_id, free_at = -1;
        relay_count(d, blk, c->remote, &in_block, &of_id);
        for (int i = 0; i < RELAY_WAITING; i++) {
            relay_slot *w = &d->waiting[i];
            if (w->used && !memcmp(w->name, name, 32) && !memcmp(w->from, c->remote, 32)) {   /* the same home again: the new join wins */
                in_block -= w->limited[0] && !memcmp(w->block[0], blk, 16);
                of_id--;
                net_close(w->c);
                w->used = false;
            }
            if (!w->used && free_at < 0) free_at = i;
        }
        if (free_at >= 0 && (!lim || in_block < RELAY_PER_BLOCK) && of_id < RELAY_PER_ID) {
            relay_slot *w = &d->waiting[free_at];
            memset(w, 0, sizeof *w);
            w->used = kept = true;
            memcpy(w->name, name, 32), memcpy(w->from, c->remote, 32), memcpy(w->to, to, 32), memcpy(w->block[0], blk, 16);
            w->limited[0] = lim;
            w->c = c;
            w->until = now_ms() + d->slot_ms + d->slot_ms / 4;  /* a slot, and a quarter for clocks a little apart */
        }
    }
    pthread_mutex_unlock(&d->r_mu);
    if (!other) return kept;
    if (!act) {                                                /* full: both go, and look for the relay of the next slot */
        net_close(other);
        return false;
    }
    const uint8_t go = DHT_RELAYED;
    relay_job *j = malloc(sizeof *j);
    pthread_t th;
    if (j) *j = (relay_job){d, act, other, c};
    if (j && net_send(other, &go, 1) && net_send(c, &go, 1) && spawn(&th, relay_main, j, QUERY_STACK, true)) {
        dlog(d, "relaying a pair");
        return true;
    }
    free(j);
    net_close(other);
    net_close(c);
    pthread_mutex_lock(&d->r_mu);
    act->used = false;
    d->nactive--;
    pthread_cond_broadcast(&d->r_cv);
    pthread_mutex_unlock(&d->r_mu);
    return true;
}

void dht_relays(dht *d, int *waiting, int *active, uint64_t *done)
{
    pthread_mutex_lock(&d->r_mu);
    relay_sweep(d);
    int w = 0;
    for (int i = 0; i < RELAY_WAITING; i++) w += d->waiting[i].used;
    *waiting = w, *active = d->nactive, *done = d->relays_done;
    pthread_mutex_unlock(&d->r_mu);
}

/* The relay of a pair in a slot of time: the node closest to SHA256("pznet1/relay" || name || slot) that is neither of the two homes.
 * Both homes find the same one without a word between them; a relay that fails or lies costs one slot, and the next slot has a relay of
 * its own (another key), so no node near one key can stop a pair. */
static bool relay_of(dht *d, const uint8_t name[32], const uint8_t partner[32], uint64_t slot, dht_contact *out)
{
    uint8_t m[12 + 32 + 8] = "pznet1/relay", k[32], pid[32];
    dht_contact cs[DHT_KSTORE];
    memcpy(m + 12, name, 32);
    for (int b = 0; b < 8; b++) m[44 + b] = (uint8_t)(slot >> (56 - 8 * b));
    pz_sha256(m, sizeof m, k);
    keys_node_id(partner, pid);
    const int n = dht_lookup(d, k, cs, DHT_KSTORE);
    for (int i = 0; i < n; i++)
        if (!is_me(d, cs[i].id) && memcmp(cs[i].id, pid, 32)) {
            *out = cs[i];
            return true;
        }
    return false;
}

/* joins the relay r and waits there for the partner until until: the channel, its handshake end to end done (party 0 initiates) */
static net_conn *relay_join(dht *d, const dht_contact *r, const uint8_t partner[32], const uint8_t name[32], int64_t until)
{
    uint8_t req[65], ans[1];
    size_t n;
    net_conn *c = dht_dial(d, r);
    if (!c) {
        rt_failed(d, r->id);
        return NULL;
    }
    req[0] = DHT_RELAY;
    memcpy(req + 1, name, 32), memcpy(req + 33, partner, 32);
    bool ok = net_send(c, req, sizeof req);
    while (ok && !stopping(d) && now_ms() < until && !net_readable(c, until - now_ms() < 200 ? (int)(until - now_ms()) : 200)) {}
    ok = ok && !stopping(d) && net_readable(c, 0) && net_recv(c, ans, sizeof ans, &n) && n == 1 && ans[0] == DHT_RELAYED &&
         net_rehandshake(&d->ncfg, c, memcmp(d->me.xonly, partner, 32) < 0, partner);
    if (!ok) {
        net_close(c);
        return NULL;
    }
    dlog(d, "a channel through a relay");
    return c;
}

static net_conn *await(void *arg, dht_wait_fn wait, int64_t ms, bool *give_up)
{
    if (wait) return wait(arg, (int)ms, give_up);
    nap(ms);
    return NULL;
}

net_conn *dht_channel(dht *d, const uint8_t partner[32], const uint8_t name[32], int wait_ms, dht_wait_fn wait, void *arg)
{
    const int64_t until = now_ms() + wait_ms;
    bool give_up = false;
    dht_contact pc;
    /* direct: party 0 dials the partner it finds, and so does a side that does not listen (nobody can dial it) */
    if ((memcmp(d->me.xonly, partner, 32) < 0 || !d->port) && dht_find(d, partner, &pc)) {
        net_conn *c = dht_dial(d, &pc);
        if (c) return c;
        rt_failed(d, pc.id);
    }
    for (int round = 0; !stopping(d) && now_ms() < until; round++) {
        /* the partner's own connection: a side that listens gives it the first slot, then looks between the tries of the relay */
        net_conn *c = await(arg, wait, round == 0 && d->port ? (d->slot_ms < until - now_ms() ? d->slot_ms : until - now_ms()) : 0, &give_up);
        if (c || give_up) return c;
        const uint64_t wall = wall_us() / 1000, slot = wall / (uint64_t)d->slot_ms;
        const int64_t left = (int64_t)((slot + 1) * (uint64_t)d->slot_ms - wall), end = now_ms() + left < until ? now_ms() + left : until;
        dht_contact r;
        if (relay_of(d, name, partner, slot, &r) && (c = relay_join(d, &r, partner, name, end))) return c;
        while (!stopping(d) && now_ms() < end) {               /* this slot's relay did not serve: the next slot has another */
            if ((c = await(arg, wait, end - now_ms() < 200 ? end - now_ms() : 200, &give_up)) || give_up) return c;
        }
    }
    return NULL;
}

