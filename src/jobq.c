/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* jobq.c - interval-based work scheduler. See jobq.h. */
#include "jobq.h"
#include <pzcore/hash.h>
#include "platform.h"
#include <errno.h>

#define V_CAP (((u128)1) << 126)

static pz_u256 u256_from_u128v(u128 v) { return pz_u256_from_u128(v); }

static char *u128_to_hex(u128 v, char *buf)
{
    if (v >> 64 == 0) snprintf(buf, 40, "%llx", (unsigned long long)v);
    else snprintf(buf, 40, "%llx%016llx", (unsigned long long)(v >> 64), (unsigned long long)v);
    return buf;
}

static bool u128_from_hex(const char *s, u128 *out)
{
    u128 v = 0;
    size_t n = strlen(s);
    if (!n || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        int c = s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = v << 4 | (u128)d;
    }
    *out = v;
    return true;
}

/* Blocks of the whole search order (M = ceil(R / 2^b)) and of the part in cfg->part. */
static bool part_blocks(const campaign_cfg *cfg, part_spec *ps, pz_u256 *m_all, pz_u256 *own, char *err, size_t errn)
{
    if (!part_parse(cfg->part, ps, err, errn)) return false;
    pz_u256 cs = pz_u256_pow2(cfg->chunk_bits), one = pz_u256_from_u64(1), m, rs = cfg_range_size(cfg);
    pz_u256_add(&m, &rs, &cs);
    pz_u256_sub(&m, &m, &one);
    m = pz_u256_shr(&m, cfg->chunk_bits);    /* M = ceil(R / 2^b) */
    pz_u256 mm1;
    pz_u256_sub(&mm1, &m, &one);
    if (pz_u256_bits(&mm1) > 192) {                  /* the permutation of the visiting order works on at most 192 bits */
        snprintf(err, errn, "the range is too large for the visiting order: with blocks of 2^%u keys you need %u position bits, the maximum is 192%s",
                 cfg->chunk_bits, pz_u256_bits(&mm1), cfg->chunk_bits < 40 ? " (--chunk-bits 40 helps up to puzzle 232; beyond that use -r with a sub-range)" : " (use -r with a sub-range)");
        return false;
    }
    *m_all = m;
    *own = m;
    if (!ps->total) return true;
    u32 rem;
    const pz_u256 q = pz_u256_divmod_u32(&m, ps->total, &rem);       /* share r holds q blocks, plus one when r < rem */
    pz_u256 o = pz_u256_mul_u32(&q, ps->count);
    for (unsigned i = 0; i < ps->count; i++)
        if (ps->res[i] < rem) pz_u256_add_u64(&o, &o, 1);
    if (pz_u256_is_zero(&o)) {
        char ms[80];
        snprintf(err, errn, "with blocks of 2^%u keys the range has only %s blocks: too few for %u shares (use a smaller --chunk-bits or fewer shares)",
                 cfg->chunk_bits, pz_u256_to_dec(&m, ms), ps->total);
        return false;
    }
    *own = o;
    return true;
}

bool sched_check_part(const campaign_cfg *cfg, char *err, size_t errn)
{
    part_spec ps;
    pz_u256 m, own;
    if (cfg->chunk_bits < 1 || cfg->chunk_bits > 40) { snprintf(err, errn, "chunk_bits must be between 1 and 40"); return false; }
    return part_blocks(cfg, &ps, &m, &own, err, errn);
}

sched_t *sched_new(const campaign_cfg *cfg)
{
    sched_t *s = pz_xcalloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->range_start = cfg->range_start;
    s->range_end = cfg->range_end;
    s->cbits = cfg->chunk_bits;
    seed_to_id(cfg->seed, s->seed_id);
    if (s->cbits < 1 || s->cbits > 40) pz_die("chunk_bits must be between 1 and 40 (got %u)", s->cbits);
    s->range_size = cfg_range_size(cfg);
    pz_u256 m, own;
    char perr[300];
    if (!part_blocks(cfg, &s->part, &m, &own, perr, sizeof perr)) pz_die("%s", perr);
    part_format(&s->part, s->part_text, sizeof s->part_text);
    s->blocks_all = m;
    s->blocks_part = own;
    if (pz_u256_is_zero(&m)) pz_die("perm_init: empty domain");
    if (!pz_perm_init(&s->perm, cfg->seed, &m)) {
        pz_u256 mm1;
        const pz_u256 one1 = pz_u256_from_u64(1);
        pz_u256_sub(&mm1, &m, &one1);
        pz_die("perm_init: domain too large (%u bits)", pz_u256_bits(&mm1));
    }
    s->part_keys = s->range_size;
    if (s->part.total) {
        /* the keys of this part: its blocks are all full except the block that holds the end of the range, when that one is short */
        const pz_u256 full = pz_u256_pow2(s->cbits), one = pz_u256_from_u64(1);
        pz_u256 last = m, first_of_last;
        pz_u256_sub(&last, &last, &one);                              /* id of the last block of the range */
        first_of_last = pz_u256_shl(&last, s->cbits);
        pz_u256 len;
        pz_u256_sub(&len, &s->range_size, &first_of_last);
        s->part_keys = pz_u256_shl(&own, s->cbits);
        if (pz_u256_cmp(&len, &full) < 0) {
            const pz_u256 pos = sched_perm_invert(&s->perm, &last);         /* where the order visits it */
            u32 r;
            (void)pz_u256_divmod_u32(&pos, s->part.total, &r);
            for (unsigned i = 0; i < s->part.count; i++) {
                if (s->part.res[i] != r) continue;
                pz_u256 deficit;
                pz_u256_sub(&deficit, &full, &len);
                pz_u256_sub(&s->part_keys, &s->part_keys, &deficit);
                break;
            }
        }
    }
    pz_u256 vt = pz_u256_shl(&own, s->cbits);
    const bool ovf = pz_u256_bits(&own) + s->cbits > 126;                  /* the virtual space is capped at 2^126 */
    s->v_limit = ovf ? V_CAP : (u128)vt.w[1] << 64 | vt.w[0];
    s->v_capped = ovf;
    s->capdone = 16;
    s->done = pz_xmalloc(s->capdone * sizeof *s->done);
    s->capinf = 16;
    s->inflight = pz_xmalloc(s->capinf * sizeof *s->inflight);
    return s;
}

void sched_free(sched_t *s)
{
    if (!s) return;
    free(s->done);
    free(s->inflight);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* Inserts [a,b) into the done set, merging neighbours. mu held. */
static void add_done(sched_t *s, u128 a, u128 b)
{
    if (a >= b) return;
    size_t i = 0;
    while (i < s->ndone && s->done[i].b < a) i++;
    size_t j = i;
    while (j < s->ndone && s->done[j].a <= b) {
        if (s->done[j].a < a) a = s->done[j].a;
        if (s->done[j].b > b) b = s->done[j].b;
        j++;
    }
    size_t removed = j - i;
    if (removed == 0) {
        if (s->ndone == s->capdone) {
            s->capdone *= 2;
            s->done = pz_xrealloc(s->done, s->capdone * sizeof *s->done);
        }
        memmove(&s->done[i + 1], &s->done[i], (s->ndone - i) * sizeof *s->done);
        s->ndone++;
    } else if (removed > 1) {
        memmove(&s->done[i + 1], &s->done[j], (s->ndone - j) * sizeof *s->done);
        s->ndone -= removed - 1;
    }
    s->done[i].a = a;
    s->done[i].b = b;
}

static u128 first_gap(const sched_t *s)
{
    if (s->ndone && s->done[0].a == 0) return s->done[0].b;
    return 0;
}

/* Position in the order of the whole range of the seq-th block of this computer's part. */
static pz_u256 global_seq(const sched_t *s, u128 seq)
{
    if (!s->part.total) return u256_from_u128v(seq);
    const u32 m = s->part.count;
    const pz_u256 base = u256_from_u128v(seq / m);
    pz_u256 k = pz_u256_mul_u32(&base, s->part.total);
    pz_u256_add_u64(&k, &k, s->part.res[(unsigned)(seq % m)]);
    return k;
}

bool sched_part_position(const sched_t *s, const pz_u256 *gseq, pz_u256 *local)
{
    if (!s->part.total) { *local = *gseq; return true; }
    u32 r;
    const pz_u256 q = pz_u256_divmod_u32(gseq, s->part.total, &r);
    for (unsigned i = 0; i < s->part.count; i++) {
        if (s->part.res[i] != r) continue;
        pz_u256 t = pz_u256_mul_u32(&q, s->part.count);
        pz_u256_add_u64(&t, &t, i);
        *local = t;
        return true;
    }
    return false;
}

static void load_chunk(sched_t *s, u128 seq)
{
    if (s->cur_valid && s->cur_seq == seq) return;
    pz_u256 sq = global_seq(s, seq);
    pz_u256 c = sched_perm_apply(&s->perm, &sq);
    pz_u256 off = pz_u256_shl(&c, s->cbits);
    pz_u256_add(&s->cur_start, &s->range_start, &off);
    pz_u256 rem;
    pz_u256_sub(&rem, &s->range_end, &s->cur_start);
    pz_u256 one = pz_u256_from_u64(1);
    pz_u256_add(&rem, &rem, &one);
    u64 full = 1ULL << s->cbits;
    if (rem.w[1] || rem.w[2] || rem.w[3] || rem.w[0] > full) s->cur_len = full;
    else s->cur_len = rem.w[0];
    s->cur_seq = seq;
    s->cur_valid = true;
}

claim_status sched_claim(sched_t *s, u64 want, claim_t *out)
{
    pthread_mutex_lock(&s->mu);
    if (want < 1024) want = 1024;
    want &= ~(u64)1023;
    for (int guard = 0; guard < 1000000; guard++) {
        if (s->next >= s->v_limit) {
            if (s->ninf) { pthread_mutex_unlock(&s->mu); return CLAIM_WAIT; }
            u128 g = first_gap(s);
            if (g >= s->v_limit) { pthread_mutex_unlock(&s->mu); return CLAIM_DONE; }
            s->next = g;
            continue;
        }
        /* skip an already-done interval */
        bool skipped = false;
        for (size_t i = 0; i < s->ndone; i++) {
            if (s->done[i].a <= s->next && s->next < s->done[i].b) { s->next = s->done[i].b; skipped = true; break; }
            if (s->done[i].a > s->next) break;
        }
        if (skipped) continue;
        u128 seq = s->next >> s->cbits;
        u128 chunk_base = seq << s->cbits;
        u64 off = (u64)(s->next - chunk_base);
        load_chunk(s, seq);
        if (off >= s->cur_len) {   /* filler beyond a short last chunk */
            add_done(s, s->next, chunk_base + ((u128)1 << s->cbits));
            s->next = chunk_base + ((u128)1 << s->cbits);
            continue;
        }
        u128 end = s->next + want;
        u128 cend = chunk_base + s->cur_len;
        if (end > cend) end = cend;
        for (size_t i = 0; i < s->ndone; i++)
            if (s->done[i].a > s->next) { if (s->done[i].a < end) end = s->done[i].a; break; }
        out->v_start = s->next;
        out->nkeys = (u64)(end - s->next);
        pz_u256 o = pz_u256_from_u64(off);
        pz_u256_add(&out->key_start, &s->cur_start, &o);
        out->id = ++s->next_id;
        if (s->ninf == s->capinf) {
            s->capinf *= 2;
            s->inflight = pz_xrealloc(s->inflight, s->capinf * sizeof *s->inflight);
        }
        s->inflight[s->ninf].id = out->id;
        s->inflight[s->ninf].a = s->next;
        s->inflight[s->ninf].b = end;
        s->ninf++;
        s->next = end;
        if (end == cend && s->cur_len < ((u64)1 << s->cbits)) {
            u128 next_chunk = chunk_base + ((u128)1 << s->cbits);
            add_done(s, end, next_chunk);
            s->next = next_chunk;
        }
        pthread_mutex_unlock(&s->mu);
        return CLAIM_OK;
    }
    pthread_mutex_unlock(&s->mu);
    return CLAIM_DONE;
}

void sched_complete(sched_t *s, const claim_t *c, u64 covered)
{
    pthread_mutex_lock(&s->mu);
    for (size_t i = 0; i < s->ninf; i++) {
        if (s->inflight[i].id == c->id) {
            s->inflight[i] = s->inflight[--s->ninf];
            break;
        }
    }
    if (covered > c->nkeys) covered = c->nkeys;
    if (covered) {
        add_done(s, c->v_start, c->v_start + covered);
        s->keys_done += covered;
    }
    pthread_mutex_unlock(&s->mu);
}

u128 sched_keys_done(sched_t *s)
{
    pthread_mutex_lock(&s->mu);
    u128 k = s->keys_done;
    pthread_mutex_unlock(&s->mu);
    return k;
}

size_t sched_open_intervals(sched_t *s)
{
    pthread_mutex_lock(&s->mu);
    size_t n = s->ndone;
    pthread_mutex_unlock(&s->mu);
    return n;
}

double sched_fraction(sched_t *s)
{
    long double kd = (long double)sched_keys_done(s);
    long double r = pz_u256_to_ld(&s->part_keys);
    double f = (double)(kd / r);
    return f > 1.0 ? 1.0 : f;
}

bool sched_finished(sched_t *s)
{
    pthread_mutex_lock(&s->mu);
    bool f = !s->v_capped && s->ndone == 1 && s->done[0].a == 0 && s->done[0].b >= s->v_limit;
    pthread_mutex_unlock(&s->mu);
    return f;
}

bool sched_locate(sched_t *s, const pz_u256 *key, pz_u256 *seq, u64 *offset)
{
    if (pz_u256_cmp(key, &s->range_start) < 0 || pz_u256_cmp(key, &s->range_end) > 0) return false;
    pz_u256 d;
    pz_u256_sub(&d, key, &s->range_start);
    pz_u256 c = pz_u256_shr(&d, s->cbits);
    pz_u256 mask = pz_u256_pow2(s->cbits), one = pz_u256_from_u64(1);
    pz_u256_sub(&mask, &mask, &one);
    *offset = d.w[0] & mask.w[0];
    *seq = sched_perm_invert(&s->perm, &c);
    return true;
}

/* true when `path` exists and carries a valid checksum */
static bool state_file_valid(const char *path)
{
    char *text = pz_read_file(path, NULL);
    if (!text) return false;
    char *crc = strstr(text, "crc=");
    bool ok = false;
    if (crc) {
        u8 dg[32];
        char hx[17];
        pz_sha256((u8 *)text, (size_t)(crc - text), dg);
        pz_hex_encode(dg, 8, hx);
        ok = strncmp(crc + 4, hx, 16) == 0;
    }
    free(text);
    return ok;
}

int sched_save(sched_t *s, const char *path)
{
    pthread_mutex_lock(&s->mu);
    size_t cap = 1024 + strlen(s->part_text) + s->ndone * 80;
    char *buf = pz_xmalloc(cap + 64);
    char h1[48], h2[48], pl[PART_TEXT + 8] = "";
    if (s->part.total) snprintf(pl, sizeof pl, "part=%s\n", s->part_text);        /* absent without a split: the format of earlier versions */
    int n = snprintf(buf, cap, "PZSCHED 1\ncbits=%u\n%sseed_id=%s\nv_limit=%s\nkeys_done=%s\nndone=%zu\n", s->cbits, pl, s->seed_id,
                     u128_to_hex(s->v_limit, h1), u128_to_hex(s->keys_done, h2), s->ndone);
    for (size_t i = 0; i < s->ndone; i++) {
        n += snprintf(buf + n, cap - (size_t)n, "iv %s %s\n", u128_to_hex(s->done[i].a, h1), u128_to_hex(s->done[i].b, h2));
    }
    pthread_mutex_unlock(&s->mu);
    u8 dg[32];
    char hx[17];
    pz_sha256((u8 *)buf, (size_t)n, dg);
    pz_hex_encode(dg, 8, hx);
    n += snprintf(buf + n, 64, "crc=%s\n", hx);
    /* keep one backup of the previous checkpoint, but never rotate a damaged file over a good backup */
    char bak[1100];
    snprintf(bak, sizeof bak, "%s.bak", path);
    if (state_file_valid(path)) pz_rename_replace(path, bak);
    int rc = pz_write_file_atomic(path, buf, (size_t)n, 0644);
    free(buf);
    return rc;
}

/* copies the value of the line "key=value" into out */
static bool state_field(const char *text, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t len = strcspn(p + kl + 1, "\n");
            if (len == 0 || len >= n) return false;
            memcpy(out, p + kl + 1, len);
            out[len] = 0;
            return true;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

int sched_load(sched_t *s, const char *path, char *err, size_t errlen)
{
    size_t len;
    char *text = pz_read_file(path, &len);
    if (!text) {
        if (errno == ENOENT) return 1;
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    char *crc = strstr(text, "crc=");
    if (!crc) { snprintf(err, errlen, "checkpoint without checksum"); free(text); return -1; }
    u8 dg[32];
    char hx[17];
    pz_sha256((u8 *)text, (size_t)(crc - text), dg);
    pz_hex_encode(dg, 8, hx);
    if (strncmp(crc + 4, hx, 16)) { snprintf(err, errlen, "invalid checkpoint checksum (corrupt file?)"); free(text); return -1; }
    *crc = 0;
    unsigned cb = 0;
    char pz_vl[64] = "", kd[64] = "", nds[32] = "";
    size_t nd = 0;
    if (strncmp(text, "PZSCHED 1\n", 10) != 0 || !state_field(text, "cbits", nds, sizeof nds)) {
        snprintf(err, errlen, "unrecognized checkpoint format");
        free(text);
        return -1;
    }
    {
        char pt[PART_TEXT] = "";
        (void)state_field(text, "part", pt, sizeof pt);              /* absent: no split */
        if (strcmp(pt, s->part_text) != 0) {
            snprintf(err, errlen, "the checkpoint belongs to a different split between computers (--part %s, the campaign uses %s): use another campaign name with -c",
                     pt[0] ? pt : "none", s->part_text[0] ? s->part_text : "none");
            free(text);
            return -2;
        }
    }
    {
        char sid[40] = "";
        /* absent in checkpoints written before the seed was recorded (up to 1.9.1): those are accepted as they were */
        if (state_field(text, "seed_id", sid, sizeof sid) && strcmp(sid, s->seed_id) != 0) {
            snprintf(err, errlen, "the checkpoint was written by a campaign with another seed (seed id %.16s, this campaign has %s): its positions point at other blocks",
                     sid, s->seed_id);
            free(text);
            return -2;
        }
    }
    cb = (unsigned)pz_strtou64(nds);
    if (!state_field(text, "v_limit", pz_vl, sizeof pz_vl) || !state_field(text, "keys_done", kd, sizeof kd) || !state_field(text, "ndone", nds, sizeof nds)) {
        snprintf(err, errlen, "unrecognized checkpoint format");
        free(text);
        return -1;
    }
    nd = (size_t)pz_strtou64(nds);
    u128 vlim, kdone;
    if (!u128_from_hex(pz_vl, &vlim) || !u128_from_hex(kd, &kdone) || cb != s->cbits || vlim != s->v_limit) {
        snprintf(err, errlen, "the checkpoint does not match the campaign (different chunk_bits or size)");
        free(text);
        return -2;
    }
    s->ndone = 0;
    const char *p = strstr(text, "\niv ");
    for (size_t i = 0; i < nd; i++) {
        char a[64] = "", b[64] = "";
        u128 va = 0, vb = 0;
        if (!p) { snprintf(err, errlen, "checkpoint interval %zu is missing", i); free(text); return -1; }
        p += 4;   /* skip "\niv " */
        size_t la = strcspn(p, " \n"), lb;
        if (la == 0 || la >= sizeof a || p[la] != ' ') { snprintf(err, errlen, "checkpoint interval %zu is invalid", i); free(text); return -1; }
        memcpy(a, p, la);
        p += la + 1;
        lb = strcspn(p, " \n");
        if (lb == 0 || lb >= sizeof b) { snprintf(err, errlen, "checkpoint interval %zu is invalid", i); free(text); return -1; }
        memcpy(b, p, lb);
        p += lb;
        if (!u128_from_hex(a, &va) || !u128_from_hex(b, &vb) || va >= vb || vb > s->v_limit) {
            snprintf(err, errlen, "checkpoint interval %zu is invalid", i);
            free(text);
            return -1;
        }
        add_done(s, va, vb);
        p = strstr(p, "\niv ");
    }
    s->keys_done = kdone;
    s->next = first_gap(s);
    free(text);
    return 0;
}
