/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kangaroo.c - symmetric Gaudry-Schost kangaroo: setup, DP table, solving, CPU workers. */
#include "kangaroo_priv.h"
#include "hash.h"
#include "cpu_engine.h"
#include "platform.h"
#include <math.h>
#include <float.h>
#include <unistd.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#endif

/* ---------------- RNG ---------------- */

static inline u64 rotl64(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

u64 krng_next(u64 s[4])
{
    const u64 result = rotl64(s[1] * 5, 7) * 9;
    const u64 t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

void krng_seed(u64 s[4], const u8 seed[32], const char *tag, u64 a, u64 b)
{
    u8 buf[32 + 16 + 16 + 16];
    memcpy(buf, seed, 32);
    memset(buf + 32, 0, 16);
    strncpy((char *)buf + 32, tag, 15);
    memcpy(buf + 48, &a, 8);
    memcpy(buf + 56, &b, 8);
    u8 h[32];
    sha256(buf, 64, h);
    memcpy(s, h, 32);
    if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
    for (int i = 0; i < 8; i++) krng_next(s);
}

u256 krng_below(u64 s[4], const u256 *bound)
{
    unsigned bits = u256_bits(bound);
    if (bits == 0) return u256_zero();
    u256 mask = u256_pow2(bits), one = u256_from_u64(1);
    u256_sub(&mask, &mask, &one);
    for (;;) {
        u256 v;
        for (int i = 0; i < 4; i++) v.w[i] = krng_next(s) & mask.w[i];
        if (u256_cmp(&v, bound) < 0) return v;
    }
}

/* ---------------- DP table ---------------- */

static inline unsigned log2u(u64 v) { return 63 - (unsigned)__builtin_clzll(v); }

static bool dp_empty(const dp_rec *e) { return e->tagk == 0 && !(e->d.w[0] | e->d.w[1] | e->d.w[2]); }
static inline u64 slot_of(const kang_state *ks, u64 tagk) { return ((tagk >> ks->kshift) * 0x9E3779B97F4A7C15ULL) >> (64 - log2u(ks->cap)); }
static inline u64 kind_of(const kang_state *ks, u64 tagk) { return tagk & (((u64)1 << ks->kshift) - 1); }

/* coefficient of x in the exponent E = coef*x + d of a point of kind k (both schemas) */
static int kind_coef(unsigned k)
{
    static const int c[5] = {0, 1, -1, 3, -3};
    return k < 5 ? c[k] : 0;
}

/* Two points on the same class: c_a x + d_a = c_b x + d_b, so x = (d_b - d_a) / (c_a - c_b), which must be whole. */
static bool solve_pair(kang_state *ks, const dp_rec *a, const dp_rec *b)
{
    int den = kind_coef((unsigned)kind_of(ks, a->tagk)) - kind_coef((unsigned)kind_of(ks, b->tagk));
    if (den == 0) return false;
    i192 x;
    i192_sub(&x, &b->d, &a->d);
    if (den < 0) { den = -den; i192_neg(&x); }
    bool neg = i192_is_neg(&x);
    if (neg) i192_neg(&x);
    u256 mag = {{x.w[0], x.w[1], x.w[2], 0}}, key;
    if (den > 1) {
        u32 rem;
        mag = u256_divmod_u32(&mag, (u32)den, &rem);
        if (rem) return false;
    }
    if (neg) {
        if (u256_cmp(&ks->base, &mag) <= 0) return false;
        u256_sub(&key, &ks->base, &mag);
    } else {
        u256_add(&key, &ks->base, &mag);
    }
    return run_submit_key(ks->r, &key, "kangaroo");
}

#define KANG_LOG_BUF_MAX ((size_t)1 << 22)      /* records the log buffer may grow to while the disk refuses writes (128 MiB) */
static void log_push(kang_state *ks, const dp_rec *rec);

/* False (nothing changed) when the memory for the larger table is not there. */
static bool table_grow(kang_state *ks)
{
    u64 ncap = ks->cap * 2;
    dp_rec *nt = calloc(ncap, sizeof(dp_rec));
    if (!nt) return false;
    dp_rec *old = ks->tab;
    u64 ocap = ks->cap;
    ks->tab = nt;
    ks->cap = ncap;
    ks->mask = ncap - 1;
    for (u64 i = 0; i < ocap; i++) {
        if (dp_empty(&old[i])) continue;
        u64 s = slot_of(ks, old[i].tagk);
        while (!dp_empty(&nt[s])) s = (s + 1) & ks->mask;
        nt[s] = old[i];
    }
    free(old);
    return true;
}

/* Halves the table load when the RAM budget is exhausted: from now on only DPs whose tag has one more
 * zero bit are kept (stored entries that do not satisfy the stricter rule are dropped). */
static void table_thin(kang_state *ks)
{
    dp_rec *old = ks->tab;
    dp_rec *nt = calloc(ks->cap, sizeof(dp_rec));
    if (!nt) {                                    /* no memory to rebuild the table: keep it as it is (records that do not fit are dropped) */
        static _Atomic int warned;
        if (!atomic_exchange(&warned, 1)) pz_log(PZ_LOG_ERR, "not enough memory to shrink the distinguished-point table: new points that do not fit are discarded");
        return;
    }
    ks->thin_mask = ks->thin_mask << 1 | 1;
    ks->thin_level++;
    u64 kept = 0;
    ks->tab = nt;
    for (u64 i = 0; i < ks->cap; i++) {
        if (dp_empty(&old[i]) || ((old[i].tagk >> ks->kshift) & ks->thin_mask)) continue;
        u64 s = slot_of(ks, old[i].tagk);
        while (!dp_empty(&nt[s])) s = (s + 1) & ks->mask;
        nt[s] = old[i];
        kept++;
    }
    free(old);
    ks->count = kept;
    pz_log(PZ_LOG_WARN, "distinguished-point table full (RAM limit): halving the density of stored points (level %u). Raise --mem to avoid this", ks->thin_level);
}

/* mu held. Returns 1 inserted, 0 duplicate of same kind, 2 solved, -1 skipped (thinned out) */
static int table_insert(kang_state *ks, const dp_rec *rec, bool from_log)
{
    if (ks->thin_mask && ((rec->tagk >> ks->kshift) & ks->thin_mask)) { atomic_fetch_add(&ks->dropped, 1); return -1; }
    if (ks->count * 10 >= ks->cap * 8) {
        u64 max_entries = ks->r->mem_limit / sizeof(dp_rec);
        const bool grown = ks->cap * 2 <= max_entries && table_grow(ks);
        if (!grown && ks->count * 100 >= ks->cap * 93) {
            table_thin(ks);
            if ((rec->tagk >> ks->kshift) & ks->thin_mask) { atomic_fetch_add(&ks->dropped, 1); return -1; }
        }
    }
    if (ks->count + 1 >= ks->cap) { atomic_fetch_add(&ks->dropped, 1); return -1; }      /* never fill the last slot: the probe below must end */
    u64 s = slot_of(ks, rec->tagk);
    for (;;) {
        dp_rec *e = &ks->tab[s];
        if (dp_empty(e)) {
            *e = *rec;
            ks->count++;
            if (!from_log) log_push(ks, rec);
            return 1;
        }
        if ((e->tagk >> ks->kshift) == (rec->tagk >> ks->kshift)) {
            if (kind_of(ks, e->tagk) == kind_of(ks, rec->tagk)) return 0;
            if (solve_pair(ks, e, rec)) return 2;
            atomic_fetch_add(&ks->rejected, 1);
            return 0;
        }
        s = (s + 1) & ks->mask;
    }
}

bool kang_insert_dps(kang_state *ks, const dp_rec *recs, int n, bool *dup_out)
{
    bool solved = false;
    pthread_mutex_lock(&ks->mu);
    for (int i = 0; i < n; i++) {
        int rc = table_insert(ks, &recs[i], false);
        if (dup_out) dup_out[i] = (rc == 0);
        if (rc == 2) { solved = true; break; }
    }
    pthread_mutex_unlock(&ks->mu);
    return solved;
}

/* ---------------- log persistence ---------------- */

/* Fingerprint of the campaign in a DP log: base, W and public key. Format 2 (1.6.0) has base = start - W/32; format 1
 * had the interval centre start + (W - 1)/2 as base. */
static void log_fingerprint(const kang_state *ks, const u256 *base, const char *tag, u8 fp[32])
{
    u8 buf[32 * 3 + 33];
    u256_to_be(base, buf);
    u256_to_be(&ks->W, buf + 32);
    memcpy(buf + 64, tag, 8);
    memcpy(buf + 72, ks->r->cfg.pubkey33, 33);
    sha256(buf, 72 + 33, fp);
}

/* A write failed (disk full, quota, I/O error): what reached the file is unknown, maybe a torn record, maybe whole ones. Reopens the file,
 * cuts it back to whole records and drops from the buffer the records that did get there, so the next write appends exactly the rest. */
static int log_resync(kang_state *ks)
{
    if (ks->log) { fclose(ks->log); ks->log = NULL; }
    FILE *f = fopen(ks->log_path, "rb");
    if (!f) return -1;
    const off_t size = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : -1;
    fclose(f);
    if (size < 38) return -1;
    const u64 whole = (u64)(size - 38) / sizeof(dp_rec);
    if (truncate(ks->log_path, 38 + (off_t)(whole * sizeof(dp_rec))) != 0) return -1;
    const u64 on_file = whole > ks->log_recs ? whole - ks->log_recs : 0;      /* of the buffered records, already written */
    const size_t drop = on_file > ks->nlog ? ks->nlog : (size_t)on_file;
    memmove(ks->logbuf, ks->logbuf + drop, (ks->nlog - drop) * sizeof(dp_rec));
    ks->nlog -= drop;
    ks->log_recs = whole;
    ks->log = fopen(ks->log_path, "ab");
    if (!ks->log) return -1;
    ks->log_broken = false;
    return 0;
}

int kang_flush_log(kang_state *ks)
{
    if (!ks->log && !ks->log_broken) return 0;                       /* no log at all (test states) */
    if (ks->log_broken && log_resync(ks) != 0) return -1;
    if (ks->nlog && (fwrite(ks->logbuf, sizeof(dp_rec), ks->nlog, ks->log) != ks->nlog || fflush(ks->log) != 0)) {
        ks->log_broken = true;
        return -1;
    }
    if (ks->nlog) {
        ks->log_recs += ks->nlog;
        ks->nlog = 0;
        ks->log_warned = false;
        return 0;
    }
    if (fflush(ks->log) != 0) { ks->log_broken = true; return -1; }
    return 0;
}

/* mu held. Appends to the log buffer, writing it out when it is full. When the disk refuses, the buffer grows (up to KANG_LOG_BUF_MAX
 * records) instead of overflowing, and the next checkpoint tries again; beyond that new points stay in the table only. */
static void log_push(kang_state *ks, const dp_rec *rec)
{
    if (!ks->log && !ks->log_broken) return;
    if (ks->nlog == ks->caplog) {
        kang_flush_log(ks);
        if (ks->nlog == ks->caplog) {
            if (ks->caplog >= KANG_LOG_BUF_MAX) {
                atomic_fetch_add(&ks->dropped, 1);
                if (!ks->log_warned) {
                    ks->log_warned = true;
                    pz_log(PZ_LOG_ERR, "writing %s failed (disk full?): new distinguished points stay in RAM only and will be lost if the program stops", ks->log_path);
                }
                return;
            }
            ks->caplog *= 2;
            ks->logbuf = pz_xrealloc(ks->logbuf, ks->caplog * sizeof(dp_rec));
        }
    }
    ks->logbuf[ks->nlog++] = *rec;
}

/* Magic and fingerprint of the logs of this campaign: format 2 for schema 1, format 3 for schema 2 (another base and
 * other kind codes: the points of the two cannot be mixed). */
static const char *log_magic(const kang_state *ks) { return ks->schema >= 3 ? "PZDP4\n" : ks->schema == 2 ? "PZDP3\n" : "PZDP2\n"; }
static void log_fp(const kang_state *ks, u8 fp[32])
{
    log_fingerprint(ks, &ks->base, ks->schema >= 3 ? "PZDPLOG4" : ks->schema == 2 ? "PZDPLOG3" : "PZDPLOG2", fp);
}

static FILE *log_create(const kang_state *ks, const char *path, const u8 fp[32])
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(log_magic(ks), 1, 6, f);
    fwrite(fp, 1, 32, f);
    fflush(f);
    return f;
}

/* A format-1 log of this campaign: its distinguished points stay valid (they are points of the curve with their exact
 * exponents), only the base changed. Wild distances are re-expressed from the old base c to the new one b:
 * sigma*(k - c) + d = sigma*(k - b) + d - sigma*(c - b). Writes them to a new log and keeps the old file as dp.bin.v1. */
static int log_convert_v1(kang_state *ks, FILE *f)
{
    u256 one = u256_from_u64(1), wm1, c, delta;
    u256_sub(&wm1, &ks->W, &one);
    wm1 = u256_shr(&wm1, 1);
    u256_add(&c, &ks->r->cfg.range_start, &wm1);
    u256_sub(&delta, &c, &ks->base);
    const i192 dl = i192_from_signed(false, &delta);
    char tmp[640], old[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", ks->log_path);
    snprintf(old, sizeof old, "%s.v1", ks->log_path);
    u8 fp[32];
    log_fp(ks, fp);
    FILE *o = log_create(ks, tmp, fp);
    if (!o) { fclose(f); return -1; }
    dp_rec rec;
    u64 n = 0;
    while (fread(&rec, sizeof rec, 1, f) == 1) {
        const unsigned kind = rec.tagk & 3;
        if (kind == KIND_WILD_P) i192_sub(&rec.d, &rec.d, &dl);
        else if (kind == KIND_WILD_M) i192_add(&rec.d, &rec.d, &dl);
        if (fwrite(&rec, sizeof rec, 1, o) != 1) { fclose(o); fclose(f); return -1; }
        n++;
    }
    fclose(f);
    if (fclose(o) != 0 || rename(ks->log_path, old) != 0 || rename(tmp, ks->log_path) != 0) return -1;
    pz_log(PZ_LOG_INFO, "distinguished points: converted %llu points of the previous format to the new kangaroo schema (original in %s)",
           (unsigned long long)n, old);
    return 0;
}

int kang_load_log(kang_state *ks)
{
    FILE *f = fopen(ks->log_path, "rb");
    u8 fp[32];
    log_fp(ks, fp);
    if (!f) {
        ks->log = log_create(ks, ks->log_path, fp);
        return ks->log ? 1 : -1;
    }
    u8 hdr[38];
    bool hdr_ok = fread(hdr, 1, 38, f) == 38;
    if (hdr_ok && ks->schema == 1 && memcmp(hdr, "PZDP1\n", 6) == 0) {
        u8 fp1[32];
        u256 one = u256_from_u64(1), wm1, c;
        u256_sub(&wm1, &ks->W, &one);
        wm1 = u256_shr(&wm1, 1);
        u256_add(&c, &ks->r->cfg.range_start, &wm1);
        log_fingerprint(ks, &c, "PZDPLOG1", fp1);
        if (memcmp(hdr + 6, fp1, 32) == 0) {
            if (log_convert_v1(ks, f) != 0) { pz_log(PZ_LOG_ERR, "converting %s failed", ks->log_path); return -1; }
            f = fopen(ks->log_path, "rb");
            if (!f || fread(hdr, 1, 38, f) != 38) { if (f) fclose(f); return -1; }
        }
    }
    if (!hdr_ok || memcmp(hdr, log_magic(ks), 6) != 0 || memcmp(hdr + 6, fp, 32) != 0) {
        fclose(f);
        pz_log(PZ_LOG_ERR, "file %s does not belong to this campaign (different header): ignored", ks->log_path);
        return -1;
    }
    dp_rec rec;
    u64 n = 0;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {
        int rc = table_insert(ks, &rec, true);
        n++;
        if (rc == 2) break;
    }
    pthread_mutex_unlock(&ks->mu);
    fclose(f);
    ks->loaded = n;
    ks->log_recs = n;
    /* truncate any partially written tail record and continue appending */
    off_t good = 38 + (off_t)(n * sizeof(dp_rec));
    if (truncate(ks->log_path, good) != 0) { /* best effort */ }
    ks->log = fopen(ks->log_path, "ab");
    return ks->log ? 0 : -1;
}

/* Adds the distinguished points of another computer's log of the same campaign (same range and public key; the jump table must
 * be the same too, i.e. the same seed, or the walks of the two computers never meet): returns 0, -1 if the file cannot be
 * opened, -2 if it is not a log of this campaign. A collision between a tame and a wild point solves the key. */
int kang_merge_file(kang_state *ks, const char *path, kang_merge_stats *st)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    u8 hdr[38], fp[32];
    log_fp(ks, fp);
    if (fread(hdr, 1, 38, f) != 38 || memcmp(hdr, log_magic(ks), 6) != 0 || memcmp(hdr + 6, fp, 32) != 0) { fclose(f); return -2; }
    dp_rec rec;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {      /* a partly written last record is ignored */
        st->read++;
        const int rc = table_insert(ks, &rec, false);
        if (rc == 1) st->added++;
        else if (rc == 0) st->dups++;
        else if (rc < 0) st->skipped++;
        else { st->solved = true; break; }
    }
    pthread_mutex_unlock(&ks->mu);
    fclose(f);
    return 0;
}

/* x.n[2] of (sign*X + d)*G for the self-test of the log conversion */
static u64 tag_of(const i192 *X, int sign, const i192 *d)
{
    i192 e = *d, t = *X;
    if (sign < 0) i192_neg(&t);
    if (sign) i192_add(&e, &e, &t);
    const bool neg = i192_is_neg(&e);
    if (neg) i192_neg(&e);
    u256 m = {{e.w[0], e.w[1], e.w[2], 0}};
    ge p;
    if (!ec_mul_g(&p, &m)) return 0;
    fe_normalize(&p.x);
    return p.x.n[2];
}

/* A format-1 log (interval-centred base) written by hand, loaded: every converted record must still describe its
 * point with the new base. Returns the number of failures. */
int kang_log_selftest(void)
{
    kang_state *ks = kang_make_test_state_s(40, 1, 1);
    if (!ks) return 1;
    int fails = 0;
    u256 key = u256_pow2(90), one = u256_from_u64(1), wm1, c;
    u256_add_u64(&key, &key, 123456789);          /* the key of kang_make_test_state */
    u256_sub(&wm1, &ks->W, &one);
    wm1 = u256_shr(&wm1, 1);
    u256_add(&c, &ks->r->cfg.range_start, &wm1);
    const bool lowc = u256_cmp(&key, &c) < 0;     /* x relative to the old base, signed */
    u256 m;
    if (lowc) u256_sub(&m, &c, &key); else u256_sub(&m, &key, &c);
    const i192 Xo = i192_from_signed(lowc, &m);
    u256_sub(&m, &key, &ks->base);
    const i192 Xn = i192_from_signed(false, &m);
    u8 fp1[32];
    log_fingerprint(ks, &c, "PZDPLOG1", fp1);
    FILE *f = fopen(ks->log_path, "wb");
    if (!f) { kang_free_test_state(ks); return 1; }
    fwrite("PZDP1\n", 1, 6, f);
    fwrite(fp1, 1, 32, f);
    u64 s = 0x5EED5EEDULL;
    const int N = 30;
    for (int i = 0; i < N; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        const unsigned kind = (unsigned)i % 3;
        u256 dm = u256_from_u64(s & ((1ULL << 41) - 1));
        dp_rec r;
        r.d = i192_from_signed(s >> 63, &dm);
        const int sign = kind == KIND_TAME ? 0 : (kind == KIND_WILD_P ? 1 : -1);
        r.tagk = (tag_of(&Xo, sign, &r.d) & ~3ULL) | kind;
        fwrite(&r, sizeof r, 1, f);
    }
    fclose(f);
    if (kang_load_log(ks) != 0 || ks->loaded != (u64)N) fails++;
    char old[640];
    snprintf(old, sizeof old, "%s.v1", ks->log_path);
    FILE *o = fopen(old, "rb");
    if (!o) fails++; else fclose(o);
    kang_flush_log(ks);
    f = fopen(ks->log_path, "rb");
    u8 hdr[38];
    if (!f || fread(hdr, 1, 38, f) != 38 || memcmp(hdr, log_magic(ks), 6) != 0) fails++;
    for (int i = 0; f && i < N; i++) {
        dp_rec r;
        if (fread(&r, sizeof r, 1, f) != 1) { fails++; break; }
        const unsigned kind = r.tagk & 3;
        const int sign = kind == KIND_TAME ? 0 : (kind == KIND_WILD_P ? 1 : -1);
        if ((tag_of(&Xn, sign, &r.d) & ~3ULL) != (r.tagk & ~3ULL)) fails++;
    }
    if (f) fclose(f);
    kang_free_test_state(ks);
    printf("  conversion of the distinguished-point log from format 1: %s\n", fails ? "FAILED" : "OK");
    return fails;
}

/* ---------------- setup ---------------- */

static long double u256_ld(const u256 *v) { return u256_to_ld(v); }

/* Schema 2: the species of walkers (docs/en/ALGORITHMS.md, section 8.8). Tame walkers near 0 in the four residues that are multiples
 * of 3, wild ones on Q in three residues 4 apart, wild ones on 3Q in one residue; zones a little wider than where the
 * classes fold. Found by enumerating the residue patterns mod 12 in the exact coverage model and measured in the walk
 * model (tools/ksim.c, design file). */
static const kang_species KSPEC2[] = {
    {0, 0, -23, 23, 42000}, {0, 3, -23, 23, 94000}, {0, 6, -23, 23, 42000}, {0, 9, -23, 23, 116000},
    {1, 3, -1005, 5, 117000}, {1, 7, -1005, 5, 116000}, {1, 11, -1005, 5, 116000},
    {3, 9, -3005, 5, 358000},
};

/* Schema 3 (docs/en/ALGORITHMS.md, section 8.10): the same residues, every zone split into bands with their own shares, optimized in
 * the exact coverage model (tools/kbands.py), from tools/kang_schema3.txt by tools/kspec_table.py. */
static const kang_species KSPEC3[] = {
#include "kang_spec3.inc"
};

_Static_assert(sizeof KSPEC3 / sizeof *KSPEC3 <= 255, "the species index of a walker is a byte");

static const kang_species *spec_table(const kang_state *ks, int *n)
{
    if (ks->schema >= 3) { *n = (int)(sizeof KSPEC3 / sizeof *KSPEC3); return KSPEC3; }
    *n = (int)(sizeof KSPEC2 / sizeof *KSPEC2);
    return KSPEC2;
}

/* floor(sqrt(n)) with integers only: the jump table of schema 3 must come out the same on every architecture (long double is
 * 80 bits on x86, 64 on Apple arm64 and 128 on Linux arm64, so a table computed with it could differ between computers). */
static u256 u256_isqrt(const u256 *n)
{
    u256 x = *n, res = u256_zero();
    const unsigned b = u256_bits(n);
    u256 bit = u256_pow2(b ? (b - 1) & ~1u : 0);
    while (!u256_is_zero(&bit)) {
        u256 t;
        u256_add(&t, &res, &bit);
        res = u256_shr(&res, 1);
        if (u256_cmp(&x, &t) >= 0) {
            u256_sub(&x, &x, &t);
            u256_add(&res, &res, &bit);
        }
        bit = u256_shr(&bit, 2);
    }
    return res;
}

/* a * q for q < 2^64 */
static u256 u256_mul_u64(const u256 *a, u64 q)
{
    u256 lo = u256_mul_u32(a, (u32)q), hi = u256_mul_u32(a, (u32)(q >> 32));
    hi = u256_shl(&hi, 32);
    u256_add(&lo, &lo, &hi);
    return lo;
}

/* The layout of a campaign: the one its log was written with; for a new one PZ_KANG_SCHEMA if set, else schema 3 with
 * at least KANG_SCHEMA2_MIN_K walkers (always on the open puzzles). With fewer, the pairs of the right residues are too
 * few walkers each: schema 2 costs more (64 walkers on W = 2^28: 1.31 against 1.22 sqrt(W) on the curve, the same in the model). */
#define KANG_SCHEMA2_MIN_K 256
static int campaign_schema(const run_ctx *r, int k_total)
{
    if (r->kang_schema >= 1 && r->kang_schema <= 3) return r->kang_schema;
    char path[640], hdr[6];
    snprintf(path, sizeof path, "%s/dp.bin", r->cfg.dir);
    FILE *f = fopen(path, "rb");
    const bool got = f && fread(hdr, 1, 6, f) == 6;
    if (f) fclose(f);
    if (got && (!memcmp(hdr, "PZDP1\n", 6) || !memcmp(hdr, "PZDP2\n", 6))) return 1;
    if (got && !memcmp(hdr, "PZDP3\n", 6)) return 2;
    if (got && !memcmp(hdr, "PZDP4\n", 6)) return 3;
    const char *e = getenv("PZ_KANG_SCHEMA");
    if (e && e[0] >= '1' && e[0] <= '3' && !e[1]) return e[0] - '0';
    if (LDBL_MANT_DIG != 64) return 3;                  /* schemas 1 and 2 need the x86 long double (see kang_setup_schema) */
    return k_total >= KANG_SCHEMA2_MIN_K ? 3 : 1;
}

kang_state *kang_create(run_ctx *r)
{
    ec_init();
    kang_state *ks = pz_xcalloc(1, sizeof *ks);
    ks->r = r;
    pthread_mutex_init(&ks->mu, NULL);
    ks->W = cfg_range_size(&r->cfg);
    if (u256_bits(&ks->W) < 17) pz_die("range too small for the kangaroo (at least 2^16 keys needed): use --mode linear");
    if (u256_bits(&ks->W) > 190) pz_die("range too large for the kangaroo (maximum 2^190)");
    ks->tame_w = u256_shr(&ks->W, 5);                     /* W/32 */
    ks->wild_w = u256_shr(&ks->W, 4);                     /* W + W/16 */
    u256_add(&ks->wild_w, &ks->wild_w, &ks->W);
    ks->tame_frac = 1.0 / 3.0;
    ks->sqrtW = sqrtl(u256_ld(&ks->W));
    ks->epoch = (u64)r->sessions | (u64)r->node << 32;   /* node 0 keeps the streams of the single-computer campaigns */

    int nthr = r->use_cpu ? r->nthreads : 0;
    int k_gpu = 0;
    ks->K_thread = 0;
    if (nthr) {
        long double k = ks->sqrtW / (256.0L * nthr);
        ks->K_thread = k < 16 ? 16 : (k > KANG_K_THREAD_MAX ? KANG_K_THREAD_MAX : (int)k);
        ks->K_thread = (ks->K_thread + 15) & ~15;          /* whole vectors for the lane-parallel rounds */
        const char *kt = getenv("PZ_TEST_KTHREAD");        /* tests: walkers per CPU thread (the regime of long walks on a small interval) */
        if (kt && pz_atoi(kt) >= 16) ks->K_thread = (pz_atoi(kt) + 15) & ~15;
    }
    ks->K_total = ks->K_thread * nthr + k_gpu;
    return ks;
}

/* The layout of the walkers, now that their number is known, and what depends on it (base, Q', 3Q'). */
static void kang_setup_schema(kang_state *ks)
{
    run_ctx *r = ks->r;
    ks->schema = campaign_schema(r, ks->K_total);
    if (ks->schema < 3 && LDBL_MANT_DIG != 64 && !r->silent_setup)
        pz_log(PZ_LOG_WARN, "kangaroo: schema %d computes its jump table with long double, which differs between architectures: do not "
               "combine this campaign (--node, merge) with computers of another architecture. Schema 3 has no such limit", ks->schema);
    ks->kshift = ks->schema >= 2 ? 3 : 2;
    ks->c_mean = ks->schema >= 3 ? KANG_C_MEAN : ks->schema == 2 ? KANG_C_MEAN_V2 : KANG_C_MEAN_V1;
    /* schema 1: base = start - S, the interval sits at [S, S + W) above the point where the classes fold (S = W/32 keeps
     * the first keys away from it); schema 2: base = start, the zones of the species are placed around the fold */
    u256 S = ks->schema >= 2 ? u256_zero() : u256_shr(&ks->W, 5);
    if (u256_cmp(&S, &r->cfg.range_start) > 0) S = r->cfg.range_start;
    u256_sub(&ks->base, &r->cfg.range_start, &S);
    if (u256_is_zero(&ks->base)) {
        ks->Qp = r->cfg.pubkey;
    } else {
        ge bg;
        if (!ec_mul_g(&bg, &ks->base)) pz_die("invalid base");
        if (!ge_sub(&ks->Qp, &r->cfg.pubkey, &bg)) {
            /* the public key is exactly base*G */
            run_submit_key(r, &ks->base, "kangaroo (base)");
            ks->Qp = bg;   /* unused */
        }
    }
    if (ks->schema >= 2) {
        ge q2;
        if (!ge_add(&q2, &ks->Qp, &ks->Qp) || !ge_add(&ks->Q3, &q2, &ks->Qp)) ks->Q3 = ks->Qp;   /* only if Qp is not a point: the key was found above */
    }
    ks->expected_steps = ks->c_mean * ks->sqrtW;
}

/* Called once the total number of walkers (CPU + GPU) is known. */
int kang_configure(kang_state *ks, int k_gpu_total)
{
    run_ctx *r = ks->r;
    int nthr = r->use_cpu ? r->nthreads : 0;
    ks->K_total = ks->K_thread * nthr + k_gpu_total;
    if (ks->K_total <= 0) return -1;
    kang_setup_schema(ks);
    if (r->mem_limit == 0) {
        u64 lim = (u64)((double)r->sys.mem_avail * 0.4);
        if (lim > (64ULL << 30)) lim = 64ULL << 30;
        if (lim < (256ULL << 20)) lim = 256ULL << 20;
        r->mem_limit = lim;
    }
    u64 max_entries = r->mem_limit / sizeof(dp_rec);
    int dp_ov = 0;
    long double ov = 0.01L * ks->sqrtW / ks->K_total;
    if (ov >= 1.0L) dp_ov = (int)floorl(log2l(ov));
    int dp_mem = 0;
    long double need = 4.0L * ks->sqrtW / (0.8L * (long double)max_entries);
    if (need > 1.0L) dp_mem = (int)ceill(log2l(need));
    int dp = dp_ov > dp_mem ? dp_ov : dp_mem;
    if (dp < 2) dp = 2;
    if (dp > 60) dp = 60;
    if (r->cfg.dp_bits) {
        /* a walker reaches a distinguished point every 2^dp steps: after the collision every walker still has to get
         * there, so K * 2^dp must stay well below sqrt(W), otherwise the search costs many times its expected effort */
        long double room = ks->sqrtW / ks->K_total;
        int dmax = room >= 4.0L ? (int)floorl(log2l(room)) - 1 : 1;
        if (dmax < dp) dmax = dp;   /* never below what memory requires */
        if ((int)r->cfg.dp_bits > dmax) {
            if (!r->silent_setup)
                pz_log(PZ_LOG_WARN, "--dp-bits %u is too high for this range with %d kangaroos (distinguished points would be too rare): using %d",
                       r->cfg.dp_bits, ks->K_total, dmax);
            dp = dmax;
        } else dp = (int)r->cfg.dp_bits;
    }
    ks->P.dp_bits = (unsigned)dp;
    ks->P.dp_mask = ((u64)1 << dp) - 1;
    /* a walker without a DP for 24 times the mean distance between DPs is in a cycle (false alarm ~e^-24); the counter
     * is 64-bit: with large dp_bits (45 on puzzle 140) a 32-bit cap would restart every walker long before its next DP */
    ks->P.cap_since = dp >= 58 ? ~0ULL : 24ULL * ((u64)1 << dp) + 16;
    ks->P.R = KANG_R;

    /* jump table (the walk on classes is a symmetric random walk: no drift to tune). Schema 1: even, uniform in
     * [2, 2*mean]; schemas 2 and 3: uniform in [2/3, 4/3] of the mean, rounded up to a multiple of KANG_MOD.
     * Mean: 12 sqrt(W) in schemas 1 and 2. Without drift a walker stays within about sqrt(n) jumps of its start (n ~ sqrt(W)/K
     * steps each), so two walks can cross only if their starts are that close: the number of such pairs is about
     * (mean / W^(3/4)) * K^1.5, only 0.6 for puzzle 135 with a jump of 12 sqrt(W). Schema 3 takes W^(3/4)/16: K^1.5/16 pairs,
     * and each walk covers a fraction 1/(16 sqrt(K)) of W (8.10) */
    long double mean = KANG_JUMP_MEAN * ks->sqrtW;      /* schemas 1 and 2 */
    u128 mean3 = 0;                                     /* schema 3: integer arithmetic, identical on every architecture */
    if (ks->schema >= 3) {
        const u256 rw = u256_isqrt(&ks->W), qw = u256_isqrt(&rw);          /* sqrt(W) and W^(1/4), rounded down */
        const char *tj = getenv("PZ_TEST_JUMP");                            /* tests: mean jump in units of sqrt(W) */
        u256 m;
        if (tj && pz_atoi(tj) > 0) {
            m = u256_mul_u32(&rw, (u32)pz_atoi(tj));
        } else {
            m = u256_mul_u64(&rw, qw.w[0]);                                 /* W^(3/4); W^(1/4) < 2^48 */
            m = u256_shr(&m, 4);                                            /* / KANG_JUMP_DIV (16) */
        }
        if (u256_bits(&m) > 120) m = u256_pow2(120);                        /* jump distances are 128-bit (W above 2^165) */
        mean3 = (u128)m.w[1] << 64 | m.w[0];
        if (mean3 < 2 * KANG_MOD) mean3 = 2 * KANG_MOD;
        mean = (long double)mean3;                                          /* only for the log line */
    } else if (mean < 2 * KANG_MOD) mean = 2 * KANG_MOD;
    u64 rg[4];
    krng_seed(rg, r->cfg.seed, ks->schema >= 3 ? "jumps4" : ks->schema == 2 ? "jumps3" : "jumps2", 0, 0);
    u256 bound = u256_from_u128(ks->schema >= 3 ? mean3 / 3 * 2 + 1 : (u128)(ks->schema >= 2 ? mean * 2 / 3 + 1 : mean));
    const u256 low = u256_from_u128(ks->schema >= 3 ? mean3 / 3 * 2 : (u128)(mean * 2 / 3));
    ge pts[KANG_R + KANG_NESC];
    for (unsigned j = 0; j < KANG_R + KANG_NESC; j++) {   /* the escape jumps after the regular ones, same distribution */
        u256 v = krng_below(rg, &bound);
        if (ks->schema >= 2) {
            u32 rem;
            u256_add(&v, &v, &low);
            v = u256_divmod_u32(&v, KANG_MOD, &rem);
            u256_add_u64(&v, &v, 1);
            v = u256_mul_u32(&v, KANG_MOD);
        } else {
            v = u256_shl(&v, 1);
            u256_add_u64(&v, &v, 2);
        }
        ks->P.jv[j] = (u128)v.w[1] << 64 | v.w[0];
        if (!ec_mul_g(&pts[j], &v)) pz_die("invalid jump");
        ks->P.jx[j] = pts[j].x;
        ks->P.jy[j] = pts[j].y;
    }

    {   /* identifies the jump table: computers of one campaign must show the same id */
        u8 jb[(KANG_R + KANG_NESC) * 16], jh[32];
        for (unsigned j = 0; j < KANG_R + KANG_NESC; j++) {
            memcpy(jb + 16 * j, &(u64){(u64)ks->P.jv[j]}, 8);
            memcpy(jb + 16 * j + 8, &(u64){(u64)(ks->P.jv[j] >> 64)}, 8);
        }
        sha256(jb, sizeof jb, jh);
        snprintf(ks->jump_id, sizeof ks->jump_id, "%02x%02x%02x%02x", jh[0], jh[1], jh[2], jh[3]);
    }

    long double n_target = 4.0L * ks->sqrtW / ldexpl(1.0L, dp);
    u64 want = (u64)(n_target / 0.6L) + 4096;
    u64 cap = 4096;
    while (cap < want && cap < max_entries) cap <<= 1;
    if (getenv("PZ_TEST_DP_CAP")) cap = pz_strtou64(getenv("PZ_TEST_DP_CAP"));   /* tests: force a tiny table to exercise growth */
    if (cap < 64) cap = 64;
    ks->cap = cap;
    ks->mask = cap - 1;
    ks->tab = calloc(cap, sizeof(dp_rec));
    if (!ks->tab) pz_die("not enough memory for the distinguished-point table (%llu entries)", (unsigned long long)cap);
    ks->caplog = 32768;
    ks->logbuf = pz_xmalloc(ks->caplog * sizeof(dp_rec));
    snprintf(ks->log_path, sizeof ks->log_path, "%s/dp.bin", r->cfg.dir);

    if (r->silent_setup) return 0;
    char b1[32], b2[32], b3[32];
    pz_log(PZ_LOG_INFO, "kangaroo: schema %d, W=2^%.1Lf, kangaroos %d (CPU %d x %d, GPU %d), dp-bits %d, mean jump %.3Le, table %s (max %s)",
           ks->schema, log2l(u256_ld(&ks->W)), ks->K_total, nthr, ks->K_thread, k_gpu_total, dp, mean, pz_fmt_bytes((double)cap * sizeof(dp_rec), b1, sizeof b1),
           pz_fmt_bytes((double)r->mem_limit, b2, sizeof b2));
    pz_log(PZ_LOG_INFO, "kangaroo: jump table id %s (computers working on the same campaign must show the same id)", ks->jump_id);
    pz_log(PZ_LOG_INFO, "kangaroo: expected effort ~%s steps (%.2Lf x sqrt(W)); expected distinguished points ~%s",
           pz_fmt_si((double)ks->expected_steps, b3, sizeof b3), ks->c_mean, pz_fmt_si((double)(ks->expected_steps / ldexpl(1.0L, dp)), b1, sizeof b1));
    /* pairs of walkers whose walks can cross, about (mean jump / W^(3/4)) * K^1.5 (8.10): below a few dozen the coverage hangs on a
     * few pairs and the search costs much more than its constant (1.5 sqrt(W) instead of 1.1 in the model and on the curve) */
    const long double pairs = mean / (ks->sqrtW * sqrtl(ks->sqrtW)) * powl((long double)ks->K_total, 1.5L);
    if (ks->schema < 3 && pairs < 64)
        pz_log(PZ_LOG_WARN, "kangaroo: with %d kangaroos on this range the jumps of schema %d are too short (only ~%.2Lg pairs of walks that can "
               "cross): the expected cost is much higher than %.2Lf x sqrt(W). A new campaign (schema 3) avoids this; the work done so far "
               "with this schema does not carry over", ks->K_total, ks->schema, pairs, ks->c_mean);
    return 0;
}

void kang_destroy(kang_state *ks)
{
    if (!ks) return;
    kang_flush_log(ks);
    if (ks->log) fclose(ks->log);
    free(ks->tab);
    free(ks->logbuf);
    pthread_mutex_destroy(&ks->mu);
    free(ks);
}

/* ---------------- herds ---------------- */

kang_worker *kang_worker_new(kang_state *ks, int K, u64 stream)
{
    kang_worker *w = pz_xcalloc(1, sizeof *w);
    w->K = K;
    w->P = &ks->P;
    w->x = pz_xaligned(64, sizeof(fe) * (size_t)K);
    w->y = pz_xaligned(64, sizeof(fe) * (size_t)K);
    w->d = pz_xcalloc((size_t)K, sizeof(i192));
    w->kind = pz_xcalloc((size_t)K, 1);
    w->sigma = pz_xcalloc((size_t)K, 1);
    w->prev = pz_xcalloc((size_t)K, sizeof(u32));
    w->prev2 = pz_xcalloc((size_t)K, sizeof(u32));
    w->lastdp = pz_xcalloc((size_t)K, sizeof(u64));
    w->ck = pz_xcalloc((size_t)K, sizeof(u32));
    w->mn = pz_xcalloc((size_t)K, sizeof(u32));
    w->esc = pz_xcalloc((size_t)K, 1);
    /* dx / tmp: allocated by the first scalar round (the GPU mirrors and the lane-parallel rounds normally never need them) */
    w->jidx = pz_xcalloc((size_t)K, 4);
    w->dpcap = K;                /* a round yields at most one distinguished point per kangaroo */
    w->dpbuf = pz_xcalloc((size_t)w->dpcap, sizeof(dp_rec));
    w->dpidx = pz_xcalloc((size_t)w->dpcap, sizeof(int));
    w->restart = pz_xcalloc((size_t)K, sizeof(int));
    w->vstale = pz_xcalloc((size_t)K, 1);
    w->spec = pz_xcalloc((size_t)K, 1);
    krng_seed(w->rng, ks->r->cfg.seed, "herd", ks->epoch, stream);
    if (ks->schema >= 2) {
        /* the shares of the species, as exact as K allows: walker i takes the species at (i + 1/2) / K of the cumulated
         * shares. Schema 3: the species here is the group of bands with the same coefficient and residue (the first band
         * of the group stands for it), and every start draws the band at random by the shares: with a few dozen walkers
         * and 51 bands, a fixed band per walker would leave some residues without walkers, and the keys that only those
         * could reach without a solution */
        int ns;
        const kang_species *sp = spec_table(ks, &ns);
        u64 gw[256] = {0}, tot = 0;
        int first[256];
        for (int s = 0; s < ns; s++) {
            int g = s;
            if (ks->schema >= 3)
                for (int t = 0; t < s; t++)
                    if (sp[t].coef == sp[s].coef && sp[t].res == sp[s].res) { g = t; break; }
            first[s] = g;
            gw[g] += sp[s].w;
            tot += sp[s].w;
        }
        for (int i = 0; i < K; i++) {
            const u64 pos = ((u64)(2 * i + 1) * tot) / (2 * (u64)K);
            u64 acc = 0;
            int s = 0;
            while (s < ns - 1 && (first[s] != s || pos >= acc + gw[s])) acc += first[s] == s ? gw[s] : 0, s++;
            w->spec[i] = (u8)s;
            w->kind[i] = sp[s].coef;
        }
        return w;
    }
    int ntame = (int)(ks->tame_frac * K);
    for (int i = 0; i < K; i++) w->kind[i] = i < ntame ? 0 : 1;
    return w;
}

void kang_worker_free(kang_worker *w)
{
    if (!w) return;
    pz_afree(w->x); pz_afree(w->y); free(w->d); free(w->kind); free(w->spec); free(w->sigma); free(w->prev); free(w->prev2);
    free(w->lastdp); free(w->ck); free(w->mn); free(w->esc); pz_afree(w->dx); pz_afree(w->tmp); free(w->jidx); free(w->dpbuf); free(w->dpidx); free(w->restart);
    pz_afree(w->vx); pz_afree(w->vy); pz_afree(w->vdx); pz_afree(w->vtmp); pz_afree(w->vjt); pz_afree(w->vaux); free(w->vstale);
    free(w);
}

void kang_worker_sync(kang_worker *w)
{
    if (w->vsync) w->vsync(w);
}

/* Residue of a mod m, in [0, m). */
static u32 i192_mod_u32(const i192 *a, u32 m)
{
    const bool neg = i192_is_neg(a);
    i192 t = *a;
    if (neg) i192_neg(&t);
    u256 mag = {{t.w[0], t.w[1], t.w[2], 0}};
    u32 rem;
    u256_divmod_u32(&mag, m, &rem);
    return neg && rem ? m - rem : rem;
}

/* Schema 2: a start of species sp, E = c*x + u: the point c*Qp + u*G and u. False when it cannot be used (u = 0 on a tame
 * walker, or the point at infinity, which reveals the key). */
static bool start_species(kang_state *ks, const kang_species *sp, u64 rng[4], ge *p, i192 *d)
{
    u256 lo = u256_mul_u32(&ks->W, (u32)(sp->lo < 0 ? -sp->lo : sp->lo)), span = u256_mul_u32(&ks->W, (u32)(sp->hi - sp->lo));
    u32 rem;
    lo = u256_divmod_u32(&lo, 1000, &rem);
    span = u256_divmod_u32(&span, 1000, &rem);
    u256_add_u64(&span, &span, 1);
    const u256 v = krng_below(rng, &span);
    /* u = lo + v, lo negative or not */
    i192 u = i192_from_signed(false, &v), l = i192_from_signed(sp->lo < 0, &lo);
    i192_add(&u, &u, &l);
    const u256 back = u256_from_u64((i192_mod_u32(&u, KANG_MOD) + KANG_MOD - sp->res) % KANG_MOD);
    const i192 b = i192_from_signed(false, &back);
    i192_sub(&u, &u, &b);                                 /* down to the residue of the species */
    const bool neg = i192_is_neg(&u);
    i192 m = u;
    if (neg) i192_neg(&m);
    const u256 mag = {{m.w[0], m.w[1], m.w[2], 0}};
    const ge *cq = sp->coef == 3 ? &ks->Q3 : &ks->Qp;
    if (u256_is_zero(&mag)) {
        if (sp->coef == 0) return false;
        *p = *cq;
    } else {
        ge ug;
        if (!ec_mul_g(&ug, &mag)) return false;
        if (neg) ge_neg(&ug, &ug);
        if (sp->coef == 0) *p = ug;
        else if (!ge_add(p, &ug, cq)) {
            /* c*x + u = 0: x = -u / c */
            u256 x;
            if (!neg) return false;                       /* x would be negative */
            x = sp->coef == 3 ? u256_divmod_u32(&mag, 3, &rem) : mag;
            if (sp->coef == 3 && rem) return false;
            u256 key;
            u256_add(&key, &ks->base, &x);
            run_submit_key(ks->r, &key, "kangaroo (start)");
            return false;
        }
    }
    *d = u;
    return true;
}

void kang_restart_rng(kang_state *ks, kang_worker *w, int i, u64 rng[4])
{
    for (;;) {
        ge p;
        i192 d;
        int8_t sg = 1;
        if (ks->schema >= 2) {
            int ns;
            const kang_species *sp = spec_table(ks, &ns), *band = &sp[w->spec[i]];
            if (ks->schema >= 3) {                       /* a band of the walker's group, by the shares */
                u64 tot = 0;
                for (int s = 0; s < ns; s++)
                    if (sp[s].coef == band->coef && sp[s].res == band->res) tot += sp[s].w;
                u64 pick = krng_next(rng) % tot;
                for (int s = 0; s < ns; s++)
                    if (sp[s].coef == band->coef && sp[s].res == band->res) {
                        if (pick < sp[s].w) { band = &sp[s]; break; }
                        pick -= sp[s].w;
                    }
            }
            if (!start_species(ks, band, rng, &p, &d)) continue;
        } else if (w->kind[i] == 0) {
            /* tame: t*G, t in [1, W/32) */
            u256 t = krng_below(rng, &ks->tame_w);
            if (u256_is_zero(&t) || !ec_mul_g(&p, &t)) continue;
            d = i192_from_signed(false, &t);
        } else {
            /* wild: (-x + v)*G = v*G - Qp, v even in [0, W + W/16) */
            u256 half = u256_shr(&ks->wild_w, 1), v = krng_below(rng, &half);
            v = u256_shl(&v, 1);
            ge nq;
            ge_neg(&nq, &ks->Qp);
            if (u256_is_zero(&v)) p = nq;
            else {
                ge vg;
                if (!ec_mul_g(&vg, &v)) continue;
                if (!ge_add(&p, &vg, &nq)) {
                    /* v*G == Qp: x = v, the key is known */
                    u256 key;
                    u256_add(&key, &ks->base, &v);
                    run_submit_key(ks->r, &key, "kangaroo (start)");
                    continue;
                }
            }
            d = i192_from_signed(false, &v);
            sg = -1;
        }
        fe_normalize(&p.y);
        if (fe_is_odd(&p.y)) {   /* canonical representative: even y */
            ge_neg(&p, &p);
            i192_neg(&d);
            sg = (int8_t)-sg;
        }
        w->x[i] = p.x;
        w->y[i] = p.y;
        w->vstale[i] = 1;
        w->d[i] = d;
        w->sigma[i] = w->kind[i] == 0 ? 1 : sg;
        w->prev[i] = w->prev2[i] = KANG_NOTAG;
        w->ck[i] = w->mn[i] = KANG_NOTAG;
        w->esc[i] = 0;
        w->lastdp[i] = w->round;
        return;
    }
}

void kang_restart_one(kang_state *ks, kang_worker *w, int i) { kang_restart_rng(ks, w, i, w->rng); }

void kang_init_herd(kang_state *ks, kang_worker *w)
{
    for (int i = 0; i < w->K; i++) kang_restart_one(ks, w, i);
}

typedef struct { kang_state *ks; kang_worker *w; int from, to; u64 rng[4]; } init_task;

static void *init_thread(void *p)
{
    init_task *t = p;
    for (int i = t->from; i < t->to; i++) kang_restart_rng(t->ks, t->w, i, t->rng);
    return NULL;
}

/* Initializes a (large) herd with several threads, each using its own random stream. */
void kang_init_herd_parallel(kang_state *ks, kang_worker *w, int nthreads, u64 stream)
{
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 32) nthreads = 32;
    pthread_t th[32];
    init_task tk[32];
    int per = (w->K + nthreads - 1) / nthreads;
    for (int t = 0; t < nthreads; t++) {
        tk[t].ks = ks;
        tk[t].w = w;
        tk[t].from = t * per < w->K ? t * per : w->K;
        tk[t].to = (t + 1) * per < w->K ? (t + 1) * per : w->K;
        krng_seed(tk[t].rng, ks->r->cfg.seed, "init", ks->epoch, stream * 64 + (u64)t);
        pthread_create(&th[t], NULL, init_thread, &tk[t]);
    }
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
}

/* ---------------- CPU worker ---------------- */

static bool always(void) { return true; }
static const struct { const char *id, *name; bool (*supported)(void); kang_round_fn fn; } KROUNDS[] = {
    {"base", "portable", always, kang_round_base},
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    {"x86", "BMI2/ADX", cpu_has_avx2, kang_round_x86},
    {"avx2v", "AVX2 lanes", cpu_has_avx2, kang_round_avx2v},
    {"avx512v", "AVX-512 lanes", cpu_has_avx512, kang_round_avx512v},
    {"avx512i", "AVX-512 IFMA lanes", cpu_has_ifma, kang_round_avx512i},
    {"avx512i2", "AVX-512 IFMA pairs", cpu_has_ifma, kang_round_avx512i2},
#endif
};
#define NKROUNDS ((int)(sizeof KROUNDS / sizeof *KROUNDS))

/* One comparison of `fn` with the portable round. R < KANG_R shrinks the jump table: with R = 4 the walks fall into
 * 2-, 4- and longer cycles every few steps, so the cycle detection and the escape jumps are exercised (*escapes counts
 * the escape jumps of the reference herd). */
static bool kang_round_compare(kang_round_fn fn, int K, int rounds, unsigned R, u64 dp_mask, u64 *escapes)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return false;
    ks->P.R = R;
    ks->P.dp_mask = dp_mask;
    ks->P.cap_since = (1ULL << 32) + 5;   /* restarts of walkers without a DP (tested at the checkpoint steps) */
    kang_worker *a = kang_worker_new(ks, K, 77), *b = kang_worker_new(ks, K, 77);
    kang_init_herd(ks, a);
    kang_init_herd(ks, b);
    /* half the walkers have gone 2^32 - 4 steps without a DP: past the cap after about 70 more steps, never for a 32-bit
     * counter that wraps (it would restart them at the first step) */
    for (int i = 1; i < K; i += 2) a->lastdp[i] = b->lastdp[i] = 0 - ((1ULL << 32) - 4);
    bool ok = true;
    for (int r = 0; r < rounds && ok; r++) {
        if (r == rounds / 2 && R == KANG_R) {
            /* a kangaroo on the jump point it is about to use: the batch product is zero */
            for (unsigned j = 0; j < R; j++) {
                fe x = ks->P.jx[j];
                fe_normalize(&x);
                const unsigned lo = (unsigned)x.n[0] & (R - 1);
                if (lo != j && ((lo + 1) & (R - 1)) != j) continue;
                fe y = ks->P.jy[j];
                fe_normalize(&y);
                const int i = K / 3;
                a->x[i] = b->x[i] = x;
                a->y[i] = b->y[i] = y;
                a->prev2[i] = b->prev2[i] = lo == j ? KANG_NOTAG : kang_tag(x.n[2]);   /* the 2-cycle escape moves lo to lo + 1 */
                a->ck[i] = b->ck[i] = a->mn[i] = b->mn[i] = KANG_NOTAG;
                a->esc[i] = b->esc[i] = 0;
                a->vstale[i] = b->vstale[i] = 1;
                break;
            }
        }
        fn(a);
        kang_round_base(b);
        for (int i = 0; i < K; i++) *escapes += b->jidx[i] >= R;
        if (a->ndp != b->ndp || a->nrestart != b->nrestart) { ok = false; break; }
        for (int k = 0; k < a->ndp && ok; k++)
            ok = a->dpidx[k] == b->dpidx[k] && a->dpbuf[k].tagk == b->dpbuf[k].tagk && !memcmp(&a->dpbuf[k].d, &b->dpbuf[k].d, sizeof(i192));
        for (int k = 0; k < a->nrestart && ok; k++) ok = a->restart[k] == b->restart[k];
        for (int k = 0; k < a->nrestart && ok; k++) {
            kang_restart_one(ks, a, a->restart[k]);
            kang_restart_one(ks, b, b->restart[k]);
        }
        a->ndp = b->ndp = a->nrestart = b->nrestart = 0;
    }
    if (ok) {
        kang_worker_sync(a);
        for (int i = 0; i < K && ok; i++) {
            fe ax = a->x[i], ay = a->y[i], bx = b->x[i], by = b->y[i];
            fe_normalize(&ax); fe_normalize(&ay); fe_normalize(&bx); fe_normalize(&by);
            ok = fe_eq(&ax, &bx) && fe_eq(&ay, &by) && !memcmp(&a->d[i], &b->d[i], sizeof(i192)) && a->sigma[i] == b->sigma[i] &&
                 a->prev[i] == b->prev[i] && a->prev2[i] == b->prev2[i] && a->lastdp[i] == b->lastdp[i] && a->ck[i] == b->ck[i] &&
                 a->mn[i] == b->mn[i] && a->esc[i] == b->esc[i];
        }
    }
    kang_worker_free(a);
    kang_worker_free(b);
    kang_free_test_state(ks);
    return ok;
}

bool kang_round_check(kang_round_fn fn, int K, int rounds)
{
    u64 esc = 0;
    /* distinguished points every 64 steps on average (so that some walkers reach the watchdog at the second checkpoint,
     * step 64), then a tiny jump table: cycles and escapes. `rounds` should be at least 70. */
    if (!kang_round_compare(fn, K, rounds, KANG_R, 63, &esc)) return false;
    esc = 0;
    if (!kang_round_compare(fn, K, rounds * 2, 4, ~0ULL >> 12, &esc)) return false;
    return esc > 0;
}

int kang_round_selftest(void)
{
    int fails = 0;
    for (int i = 1; i < NKROUNDS; i++) {
        if (!KROUNDS[i].supported()) { printf("  kangaroo step %-22s not supported by this CPU: skipped\n", KROUNDS[i].name); continue; }
        bool ok = kang_round_check(KROUNDS[i].fn, 96, 140) && kang_round_check(KROUNDS[i].fn, 1024, 75) && kang_round_check(KROUNDS[i].fn, 40, 140);   /* 40: an odd number of vectors for the pair form */
        printf("  kangaroo step %-22s %s\n", KROUNDS[i].name, ok ? "identical to the reference" : "DIFFERENT FROM THE REFERENCE");
        fails += !ok;
    }
    return fails;
}

static pthread_once_t g_kround_once = PTHREAD_ONCE_INIT;
static int g_kround;

/* Steps per second of `fn` on one thread (a short run on a test herd). */
static double kround_rate(kang_round_fn fn)
{
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) return 0;
    ks->P.dp_mask = ~0ULL >> 8;
    ks->P.cap_since = ~0ULL;
    kang_worker *w = kang_worker_new(ks, 1024, 5);
    kang_init_herd(ks, w);
    fn(w);
    u64 steps = 0;
    double t0 = pz_mono(), t;
    do {
        fn(w);
        steps += (u64)w->K;
        w->ndp = w->nrestart = 0;
        t = pz_mono();
    } while (t - t0 < 0.12);
    kang_worker_free(w);
    kang_free_test_state(ks);
    return (double)steps / (t - t0);
}

static double kbench_run(kang_state *ks, const sysinfo_t *s, int n, const kang_round_fn *fn, const int *pos, double secs);
static void kround_choose_pair(void);

static void kround_pick(void)
{
    g_kround = 0;
    double best = 0;
    const char *forced = getenv("PZ_KANG_ROUND");          /* diagnostics: base, x86, avx2v, avx512v, avx512i */
    if (forced && forced[0]) {
        for (int i = 0; i < NKROUNDS; i++)
            if (!strcmp(forced, KROUNDS[i].id) && KROUNDS[i].supported()) { g_kround = i; return; }
        pz_log(PZ_LOG_WARN, "PZ_KANG_ROUND=%s is not available on this CPU: choosing automatically", forced);
    }
    for (int i = 0; i < NKROUNDS; i++) {
        if (!KROUNDS[i].supported()) continue;
        if (i > 0 && !kang_round_check(KROUNDS[i].fn, 96, 75)) {
            pz_log(PZ_LOG_WARN, "kangaroo CPU: variant %s does not match the reference, excluded", KROUNDS[i].name);
            continue;
        }
        double r = kround_rate(KROUNDS[i].fn);
        if (pz_env_on("PZ_KANG_VERBOSE")) fprintf(stderr, "kangaroo CPU: %-22s %.2f M steps/s (1 thread)\n", KROUNDS[i].name, r / 1e6);
        if (r > best * 1.03) { best = r; g_kround = i; }
    }
    kround_choose_pair();
}

kang_round_fn kang_pick_round(void)
{
    pthread_once(&g_kround_once, kround_pick);
    return KROUNDS[g_kround].fn;
}

const char *kang_round_name(void)
{
    pthread_once(&g_kround_once, kround_pick);
    return KROUNDS[g_kround].name;
}

/* The widest instruction set among the step variants this CPU supports: unlike the measured choice of kang_round_name,
 * which may differ from one process to the next when two variants are about as fast, it never changes on a machine. */
const char *kang_round_isa(void)
{
    int best = 0;
    for (int v = 0; v < NKROUNDS; v++)
        if (KROUNDS[v].supported()) best = v;
    return !strcmp(KROUNDS[best].id, "avx512i2") ? "avx512i" : KROUNDS[best].id;
}

/* ---------------- CPU benchmark of the step variants ---------------- */

typedef struct {
    kang_state *ks;
    const sysinfo_t *s;
    kang_round_fn fn;
    int pos;                     /* position in s->cpu_ids */
    double secs, rate;
    _Atomic int *ready;
    int nthreads;
} kbench_arg;

static void *kbench_thread(void *p)
{
    kbench_arg *a = p;
    sysinfo_pin_thread(a->s, a->pos);
    kang_worker *w = kang_worker_new(a->ks, KANG_K_THREAD_MAX, 900 + (u64)a->pos);
    kang_init_herd(a->ks, w);
    a->fn(w);
    w->ndp = w->nrestart = 0;
    atomic_fetch_add(a->ready, 1);
    while (atomic_load(a->ready) < a->nthreads) { /* all herds ready: measure together */ }
    u64 steps = 0;
    double t0 = pz_mono(), t;
    do {
        a->fn(w);
        steps += (u64)w->K;
        w->ndp = w->nrestart = 0;
        t = pz_mono();
    } while (t - t0 < a->secs);
    a->rate = (double)steps / (t - t0);
    kang_worker_free(w);
    return NULL;
}

/* Steps per second of n threads, thread i running fn[i] on CPU position pos[i]. */
static double kbench_run(kang_state *ks, const sysinfo_t *s, int n, const kang_round_fn *fn, const int *pos, double secs)
{
    pthread_t th[256];
    kbench_arg a[256];
    _Atomic int ready = 0;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        a[i] = (kbench_arg){ks, s, fn[i], pos[i], secs, 0, &ready, n};
        pthread_create(&th[i], NULL, kbench_thread, &a[i]);
    }
    double total = 0;
    for (int i = 0; i < n; i++) { pthread_join(th[i], NULL); total += a[i].rate; }
    return total;
}

/* The IFMA step comes in two forms that do the same (kv_bwd_step): one vector at a time, or two, which keeps the units busier
 * when a core runs a single thread but gains nothing when the sibling thread fills the gaps. So when the single-vector form has
 * won, the two are timed again with every logical CPU busy, and the pair replaces it only if it is faster there too. */
static void kround_choose_pair(void)
{
    int one = -1, two = -1;
    for (int i = 0; i < NKROUNDS; i++) {
        if (!strcmp(KROUNDS[i].id, "avx512i")) one = i;
        if (!strcmp(KROUNDS[i].id, "avx512i2")) two = i;
    }
    if (one < 0 || two < 0 || (g_kround != one && g_kround != two)) return;
    if (!KROUNDS[one].supported() || !kang_round_check(KROUNDS[two].fn, 96, 75)) { g_kround = one; return; }
    sysinfo_t s;
    sysinfo_detect(&s);
    const int n = s.ncpu > 64 ? 64 : s.ncpu;
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) { g_kround = one; return; }
    ks->P.dp_mask = ~0ULL >> 8;
    ks->P.cap_since = ~0ULL;
    int pos[64] = {0};
    kang_round_fn fns[64] = {0};
    for (int i = 0; i < n; i++) pos[i] = i;
    double r[2];
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < n; i++) fns[i] = KROUNDS[v ? two : one].fn;
        r[v] = kbench_run(ks, &s, n, fns, pos, 0.15);
    }
    kang_free_test_state(ks);
    if (pz_env_on("PZ_KANG_VERBOSE"))
        fprintf(stderr, "kangaroo CPU: IFMA with %d threads: one vector %.2f, two vectors %.2f M steps/s\n", n, r[0] / 1e6, r[1] / 1e6);
    g_kround = r[1] > r[0] * 1.03 ? two : one;
}

void kang_bench_cpu(const sysinfo_t *s, double secs)
{
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) return;
    ks->P.dp_mask = ~0ULL >> 8;                /* pure stepping */
    ks->P.cap_since = ~0ULL;
    static int pos_all[1024], pos_core[1024];
    static kang_round_fn fns[1024];
    const int nall = s->ncpu > 256 ? 256 : s->ncpu;
    int ncore = 0;
    for (int c = 0; c < s->ncore && ncore < 256; c++) {
        int p[1];
        if (sysinfo_core_cpus(s, c, p, 1)) pos_core[ncore++] = p[0];
    }
    for (int i = 0; i < nall; i++) pos_all[i] = i;
    printf("kangaroo CPU benchmark by step variant (%d physical cores, %d logical threads, %.1f s per run):\n", ncore, nall, secs);
    double best = 0;
    int best_v = -1;
    char b1[32], b2[32], b3[32];
    for (int v = 0; v < NKROUNDS; v++) {
        if (!KROUNDS[v].supported()) continue;
        for (int i = 0; i < nall; i++) fns[i] = KROUNDS[v].fn;
        const double r1 = kbench_run(ks, s, 1, fns, pos_all, secs);
        const double rc = kbench_run(ks, s, ncore, fns, pos_core, secs);
        const double ra = nall > ncore ? kbench_run(ks, s, nall, fns, pos_all, secs) : rc;
        printf("  %-22s 1 thread %s, %d (one thread per core) %s", KROUNDS[v].name, pz_fmt_si(r1, b1, sizeof b1), ncore, pz_fmt_si(rc, b2, sizeof b2));
        if (nall > ncore) printf(", %d (all) %s", nall, pz_fmt_si(ra, b3, sizeof b3));
        printf(" steps/s\n");
        fflush(stdout);
        const double r = ra > rc ? ra : rc;
        if (r > best) { best = r; best_v = v; }
    }
    /* SMT: the vector variant on the first thread of each core, the scalar one (integer multiplier, not the vector
     * unit) on its sibling */
    int vx86 = -1;
    for (int v = 0; v < NKROUNDS; v++)
        if (!strcmp(KROUNDS[v].id, "x86") && KROUNDS[v].supported()) vx86 = v;
    if (nall > ncore && vx86 >= 0 && best_v >= 0 && best_v != vx86) {
        int n = 0;
        for (int c = 0; c < s->ncore && n < 256; c++) {
            int p[8];
            const int k = sysinfo_core_cpus(s, c, p, 8);
            for (int j = 0; j < k && n < 256; j++) { pos_all[n] = p[j]; fns[n] = j == 0 ? KROUNDS[best_v].fn : KROUNDS[vx86].fn; n++; }
        }
        const double rm = kbench_run(ks, s, n, fns, pos_all, secs);
        printf("  mixed: %s on the first thread of each core, %s on the other: %s steps/s (%+.0f%% against the best uniform setup)%s\n",
               KROUNDS[best_v].name, KROUNDS[vx86].name, pz_fmt_si(rm, b1, sizeof b1), 100.0 * (rm / best - 1.0),
               rm > best * 1.03 ? "\n  -> worth it: start with PZ_KANG_MIX=1" : "");
    }
    kang_free_test_state(ks);
}

/* PZ_KANG_MIX=1: on cores with SMT, the threads after the first run the scalar step (see kang_bench_cpu). */
static kang_round_fn kang_round_for_slot(const sysinfo_t *s, int slot)
{
    kang_round_fn fn = kang_pick_round();
    const char *mix = getenv("PZ_KANG_MIX");
    if (!mix || mix[0] != '1' || s->ncpu <= 0) return fn;
    const int pos = slot % s->ncpu;
    int p[1];
    if (sysinfo_core_cpus(s, s->core_of[pos], p, 1) && p[0] == pos) return fn;
    for (int v = 0; v < NKROUNDS; v++)
        if (!strcmp(KROUNDS[v].id, "x86") && KROUNDS[v].supported() && kang_round_check(KROUNDS[v].fn, 96, 75)) return KROUNDS[v].fn;
    return fn;
}

void kang_cpu_worker_main(kang_state *ks, int widx, int slot, u64 stream)
{
    run_ctx *r = ks->r;
    sysinfo_pin_thread(&r->sys, slot);
    kang_worker *w = kang_worker_new(ks, ks->K_thread, stream);
    kang_init_herd(ks, w);
    kang_round_fn round = kang_round_for_slot(&r->sys, slot);
    int dupcap = w->dpcap;
    bool *dups = pz_xcalloc((size_t)dupcap, sizeof(bool));
    while (!atomic_load(&r->stop)) {
        if (UNLIKELY(slot >= atomic_load_explicit(&r->cpu_cap, memory_order_relaxed))) { pz_sleep(0.01); continue; }
        round(w);
        for (int i = 0; i < w->nrestart; i++) {
            kang_restart_one(ks, w, w->restart[i]);
            atomic_fetch_add(&ks->restarts, 1);
        }
        w->nrestart = 0;
        atomic_fetch_add_explicit(&r->ws[widx].units, (u64)w->K, memory_order_relaxed);
        if (w->ndp > 0) {
            if (UNLIKELY(w->ndp > dupcap)) { dupcap = w->dpcap; dups = pz_xrealloc(dups, (size_t)dupcap * sizeof(bool)); }
            for (int k = 0; k < w->ndp; k++) kang_dp_recode(ks, w, &w->dpbuf[k], w->dpidx[k]);
            if (kang_insert_dps(ks, w->dpbuf, w->ndp, dups)) { w->ndp = 0; break; }
            for (int k = 0; k < w->ndp; k++)
                if (dups[k]) {
                    kang_restart_one(ks, w, w->dpidx[k]);
                    atomic_fetch_add(&ks->dups, 1);
                }
            w->ndp = 0;
        }
    }
    free(dups);
    kang_worker_free(w);
}

/* Two computers' logs, each with half of a collision: neither can solve the key, the merge of both does. */
static int merge_selftest_schema(int schema)
{
    kang_state *ks = kang_make_test_state_s(40, 1, schema);
    if (!ks) return 1;
    int fails = 0;
    u256 key = u256_pow2(90), m;
    u256_add_u64(&key, &key, 123456789);          /* the key of kang_make_test_state */
    u256_sub(&m, &key, &ks->base);
    const i192 X = i192_from_signed(false, &m);   /* the unknown offset from the base */
    u8 fp[32];
    log_fp(ks, fp);
    const u64 km = ((u64)1 << ks->kshift) - 1;
    char dir[700], pa[760], pb[760], pc[760];
    pz_strlcpy(dir, ks->r->cfg.dir, sizeof dir);
    snprintf(pa, sizeof pa, "%s/node_a.bin", dir);
    snprintf(pb, sizeof pb, "%s/node_b.bin", dir);
    snprintf(pc, sizeof pc, "%s/other.bin", dir);
    /* the tame point d_t*G and the wild point (X + d_w)*G coincide when d_t = X + d_w; the others are unrelated points */
    const u64 dws[3] = {777, 5000001, 123};
    FILE *fa = fopen(pa, "wb"), *fb = fopen(pb, "wb"), *fc = fopen(pc, "wb");
    if (!fa || !fb || !fc) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        if (fc) fclose(fc);
        kang_free_test_state(ks);
        return 1;
    }
    fwrite(log_magic(ks), 1, 6, fa); fwrite(fp, 1, 32, fa);
    fwrite(log_magic(ks), 1, 6, fb); fwrite(fp, 1, 32, fb);
    u8 other[32];
    memset(other, 0x5A, sizeof other);
    fwrite(log_magic(ks), 1, 6, fc); fwrite(other, 1, 32, fc);
    for (int i = 0; i < 3; i++) {
        const u256 dw256 = u256_from_u64(dws[i]);
        const i192 dw = i192_from_signed(false, &dw256);
        i192 dt;
        i192_add(&dt, &X, &dw);
        dp_rec ta, wb;
        ta.d = dt;
        ta.tagk = (tag_of(&X, 0, &dt) & ~km) | KIND_TAME;
        wb.d = dw;
        wb.tagk = (tag_of(&X, 1, &dw) & ~km) | KIND_WILD_P;
        if (i == 1) {                              /* the second pair is unrelated points: no collision */
            wb.d = i192_from_signed(false, &(u256){{99999999, 0, 0, 0}});
            wb.tagk = (tag_of(&X, 1, &wb.d) & ~km) | KIND_WILD_P;
        }
        fwrite(&ta, sizeof ta, 1, fa);             /* every tame point goes to computer A, every wild point to computer B */
        fwrite(&wb, sizeof wb, 1, fb);
    }
    fclose(fa); fclose(fb); fclose(fc);
#define MCHK(cond) do { if (cond) { fails++; printf("  merge: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    kang_merge_stats a = {0}, a2 = {0}, b = {0}, c = {0};
    MCHK(kang_load_log(ks) < 0);               /* 1: a new log was created */
    MCHK(kang_merge_file(ks, pa, &a) != 0 || a.read != 3 || a.added != 3 || a.solved);       /* A alone does not solve */
    MCHK(kang_merge_file(ks, pa, &a2) != 0 || a2.read != 3 || a2.added != 0 || a2.dups != 3); /* again: all known */
    MCHK(kang_merge_file(ks, pc, &c) != -2 || c.read != 0);                                   /* another campaign: refused */
    MCHK(kang_merge_file(ks, "/nonexistent/dp.bin", &c) != -1);
    MCHK(kang_merge_file(ks, pb, &b) != 0 || !b.solved);                                      /* A and B together do */
    MCHK(!atomic_load(&ks->r->found) || u256_cmp(&ks->r->found_key, &key) != 0);
    kang_free_test_state(ks);
    printf("  merge of the distinguished points of two computers (schema %d): %s\n", schema, fails ? "FAILED" : "OK");
    return fails;
}

/* c*X for a small c */
static i192 i192_mul_small(const i192 *X, int c)
{
    i192 r = i192_zero();
    for (int i = 0; i < (c < 0 ? -c : c); i++) i192_add(&r, &r, X);
    if (c < 0) i192_neg(&r);
    return r;
}

/* Schema 2: two distinguished points on the same class reveal the key for every two kinds of different coefficient
 * (tame, wild on Q and on 3Q, either sign), the division by c_a - c_b included. */
static int solve_selftest(void)
{
    int fails = 0, pairs = 0;
    const i192 zero = i192_zero();
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);                                   /* "key found" twenty times */
    for (unsigned ka = 0; ka < 5; ka++)
        for (unsigned kb = 0; kb < 5; kb++) {
            if (kind_coef(ka) == kind_coef(kb)) continue;
            kang_state *ks = kang_make_test_state_s(40, 1, 3);
            if (!ks) return fails + 1;
            u256 key = u256_pow2(90), m;
            u256_add_u64(&key, &key, 123456789);       /* the key of kang_make_test_state */
            u256_sub(&m, &key, &ks->base);
            const i192 X = i192_from_signed(false, &m);
            const u256 dm = u256_from_u64(1000003 + 7919 * (u64)(ka * 5 + kb));
            dp_rec a, b;
            b.d = i192_from_signed((ka + kb) & 1, &dm);
            i192 E = i192_mul_small(&X, kind_coef(kb)), cx = i192_mul_small(&X, kind_coef(ka));
            i192_add(&E, &E, &b.d);                      /* E = c_b X + d_b = c_a X + d_a */
            i192_sub(&a.d, &E, &cx);
            const u64 tag = tag_of(&zero, 0, &E) & ~7ULL;
            a.tagk = tag | ka;
            b.tagk = tag | kb;
            if (kang_insert_dps(ks, &a, 1, NULL) || !kang_insert_dps(ks, &b, 1, NULL) || !atomic_load(&ks->r->found) || u256_cmp(&ks->r->found_key, &key) != 0) {
                fails++;
                printf("  solution between kinds %u and %u: FAILED\n", ka, kb);
            }
            pairs++;
            kang_free_test_state(ks);
        }
    pz_log_set_quiet(was_quiet);
    printf("  solving the collisions between all kinds of walkers (schemas 2 and 3, %d pairs): %s\n", pairs, fails ? "FAILED" : "OK");
    return fails;
}

int kang_merge_selftest(void) { return merge_selftest_schema(1) + merge_selftest_schema(2) + merge_selftest_schema(3) + solve_selftest(); }

#ifndef _WIN32
/* The log of distinguished points when the disk refuses writes: no overflow of the buffer, nothing lost, and after a write that stopped in the
 * middle of a record (file size limit) the file is cut back and completed without gaps or duplicates. */
int kang_log_fault_selftest(void)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return 1;
    int fails = 0;
#define LCHK(cond) do { if (!(cond)) { fails++; printf("  full disk: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    const size_t small = 256;
    ks->logbuf = pz_xrealloc(ks->logbuf, small * sizeof(dp_rec));
    ks->caplog = small;
    ks->nlog = 0;
    dp_rec rec;
    memset(&rec, 0, sizeof rec);
    /* 1. a device that fails every write */
    FILE *full = fopen("/dev/full", "wb");
    if (full) {
        ks->log = full;
        pz_strlcpy(ks->log_path, "/dev/full", sizeof ks->log_path);
        const int N = 3000;
        for (int i = 0; i < N; i++) {
            rec.tagk = ((u64)(i + 1) << 20) | KIND_TAME;
            table_insert(ks, &rec, false);
        }
        LCHK(ks->nlog == (size_t)N && ks->caplog >= (size_t)N);        /* the buffer grew instead of overflowing; no point was lost */
        LCHK(kang_flush_log(ks) == -1 && ks->log_broken);
        ks->log_broken = false;
        ks->nlog = 0;
    }
    /* 2. a file size limit that falls inside a record */
    ks->logbuf = pz_xrealloc(ks->logbuf, small * sizeof(dp_rec));      /* small again: it grew in step 1 */
    ks->caplog = small;
    ks->nlog = 0;
    char path[700];
    snprintf(path, sizeof path, "%s/disk.bin", ks->r->cfg.dir);
    pz_strlcpy(ks->log_path, path, sizeof ks->log_path);
    u8 fp[32];
    log_fp(ks, fp);
    if (ks->log) { fclose(ks->log); ks->log = NULL; }                  /* the stream of step 1 (the resync of the log may have closed it already) */
    ks->log = log_create(ks, path, fp);
    ks->log_recs = 0;
    ks->log_broken = false;
    LCHK(ks->log != NULL);
    struct rlimit old, lim;
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    const bool have_rl = getrlimit(RLIMIT_FSIZE, &old) == 0;
    if (ks->log && have_rl) {
        const int N = 2400;
        lim = old;
        lim.rlim_cur = (rlim_t)(38 + 1000 * sizeof(dp_rec) + 7);
        LCHK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
        for (int i = 0; i < N; i++) {
            rec.tagk = ((u64)(i + 100001) << 20) | KIND_TAME;      /* other points than those of step 1, which are still in the table */
            table_insert(ks, &rec, false);
        }
        LCHK(ks->log_broken);
        LCHK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        LCHK(kang_flush_log(ks) == 0 && !ks->log_broken && ks->nlog == 0);
        LCHK(kang_flush_log(ks) == 0);
        fclose(ks->log);
        ks->log = NULL;
        FILE *f = fopen(path, "rb");
        u8 hdr[38];
        LCHK(f && fread(hdr, 1, 38, f) == 38 && !memcmp(hdr, log_magic(ks), 6));
        int seen = 0;
        for (; f && seen < N + 1; seen++) {
            dp_rec r;
            if (fread(&r, sizeof r, 1, f) != 1) break;
            if (r.tagk != (((u64)(seen + 100001) << 20) | KIND_TAME)) { LCHK(0); break; }
        }
        LCHK(seen == N);                                             /* every point once, in order, no torn record */
        if (f) { u8 extra; LCHK(fread(&extra, 1, 1, f) == 0); fclose(f); }
    }
    signal(SIGXFSZ, old_sig);
    kang_free_test_state(ks);
    printf("  distinguished-point log with a full or limited disk: %s\n", fails ? "FAILED" : "OK");
    return fails;
}
#else
int kang_log_fault_selftest(void) { printf("  distinguished-point log with a full disk: not tested on Windows\n"); return 0; }
#endif
