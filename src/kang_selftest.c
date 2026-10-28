/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_selftest.c - self-tests of the kangaroo: the CPU step variants against the portable one, the merge of two computers' logs, the
 * solution of a collision between every pair of kinds of walkers, and the log of distinguished points on a full disk. */
#include "kangaroo_priv.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#endif

int kang_round_selftest(void)
{
    int fails = 0, nkr;
    const kang_round_variant *kr = kang_round_variants(&nkr);
    for (int i = 1; i < nkr; i++) {
        char lb[64];
        snprintf(lb, sizeof lb, "kangaroo step %s", kr[i].name);
        if (!kr[i].supported()) { pz_result_line(lb, "not supported by this CPU: skipped"); continue; }
        bool ok = kang_round_check(kr[i].fn, 96, 140) && kang_round_check(kr[i].fn, 1024, 75) && kang_round_check(kr[i].fn, 40, 140);   /* 40: an odd number of vectors for the pair form */
        pz_result_line(lb, "%s", ok ? "identical to the reference" : "DIFFERENT FROM THE REFERENCE");
        fails += !ok;
    }
    return fails;
}

/* x.n[2] of (sign*X + d)*G for the self-tests of the merge and of the collisions */
static u64 tag_of(const i192 *X, int sign, const i192 *d)
{
    i192 e = *d, t = *X;
    if (sign < 0) i192_neg(&t);
    if (sign) i192_add(&e, &e, &t);
    const bool neg = i192_is_neg(&e);
    if (neg) i192_neg(&e);
    pz_u256 m = {{e.w[0], e.w[1], e.w[2], 0}};
    pz_ge p;
    if (!pz_ec_mul_g(&p, &m)) return 0;
    pz_fe_normalize(&p.x);
    return p.x.n[2];
}

/* Two computers' logs, each with half of a collision: neither can solve the key, the merge of both does. */
static int merge_selftest(void)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return 1;
    int fails = 0;
    pz_u256 key = pz_u256_pow2(90), m;
    pz_u256_add_u64(&key, &key, 123456789);          /* the key of kang_make_test_state */
    pz_u256_sub(&m, &key, &ks->base);
    const i192 X = i192_from_signed(false, &m);   /* the unknown offset from the base */
    u8 fp[32];
    kang_log_fp(ks, fp);
    const u64 km = ((u64)1 << KANG_KSHIFT) - 1;
    char dir[700], pa[760], pb[760], pc[760], pd[760];
    pz_strlcpy(dir, ks->r->cfg.dir, sizeof dir);
    snprintf(pa, sizeof pa, "%s/node_a.bin", dir);
    snprintf(pb, sizeof pb, "%s/node_b.bin", dir);
    snprintf(pc, sizeof pc, "%s/other.bin", dir);
    snprintf(pd, sizeof pd, "%s/seed.bin", dir);
    /* the tame point d_t*G and the wild point (X + d_w)*G coincide when d_t = X + d_w; the others are unrelated points */
    const u64 dws[3] = {777, 5000001, 123};
    FILE *fa = fopen(pa, "wb"), *fb = fopen(pb, "wb"), *fc = fopen(pc, "wb"), *fd = fopen(pd, "wb");
    if (!fa || !fb || !fc || !fd) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        if (fc) fclose(fc);
        if (fd) fclose(fd);
        kang_free_test_state(ks);
        return 1;
    }
    /* A is a log of this version, B one of version 4 (no jump table in the header: taken as it is), C another campaign's, D this campaign's
     * written with another jump table, with B's points */
    u8 other[32], tag[4];
    memset(other, 0x5A, sizeof other);
    memcpy(tag, ks->jump_tag, 4);
    tag[3] ^= 1;
    fwrite(KANG_LOG_MAGIC, 1, 6, fa); fwrite(fp, 1, 32, fa); fwrite(ks->jump_tag, 1, 4, fa);
    fwrite(KANG_LOG_MAGIC4, 1, 6, fb); fwrite(fp, 1, 32, fb);
    fwrite(KANG_LOG_MAGIC, 1, 6, fc); fwrite(other, 1, 32, fc); fwrite(ks->jump_tag, 1, 4, fc);
    fwrite(KANG_LOG_MAGIC, 1, 6, fd); fwrite(fp, 1, 32, fd); fwrite(tag, 1, 4, fd);
    for (int i = 0; i < 3; i++) {
        const pz_u256 dw256 = pz_u256_from_u64(dws[i]);
        const i192 dw = i192_from_signed(false, &dw256);
        i192 dt;
        i192_add(&dt, &X, &dw);
        dp_rec ta, wb;
        ta.d = dt;
        ta.tagk = (tag_of(&X, 0, &dt) & ~km) | KIND_TAME;
        wb.d = dw;
        wb.tagk = (tag_of(&X, 1, &dw) & ~km) | KIND_WILD_P;
        if (i == 1) {                              /* the second pair is unrelated points: no collision */
            wb.d = i192_from_signed(false, &(pz_u256){{99999999, 0, 0, 0}});
            wb.tagk = (tag_of(&X, 1, &wb.d) & ~km) | KIND_WILD_P;
        }
        fwrite(&ta, sizeof ta, 1, fa);             /* every tame point goes to computer A, every wild point to computer B */
        fwrite(&wb, sizeof wb, 1, fb);
        fwrite(&wb, sizeof wb, 1, fd);
    }
    fclose(fa); fclose(fb); fclose(fc); fclose(fd);
#define MCHK(cond) do { if (cond) { fails++; printf("  merge: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    kang_merge_stats a = {0}, a2 = {0}, b = {0}, c = {0}, d = {0};
    char want_id[9];
    snprintf(want_id, sizeof want_id, "%02x%02x%02x%02x", tag[0], tag[1], tag[2], tag[3]);
    MCHK(kang_load_log(ks) < 0);               /* 1: a new log was created */
    MCHK(kang_merge_file(ks, pa, &a) != 0 || a.read != 3 || a.added != 3 || a.solved);       /* A alone does not solve */
    MCHK(kang_merge_file(ks, pa, &a2) != 0 || a2.read != 3 || a2.added != 0 || a2.dups != 3); /* again: all known */
    MCHK(kang_merge_file(ks, pc, &c) != -2 || c.read != 0);                                   /* another campaign: refused */
    MCHK(kang_merge_file(ks, "/nonexistent/dp.bin", &c) != -1);
    MCHK(kang_merge_file(ks, pd, &d) != -3 || d.read != 0 || strcmp(d.jump_id, want_id) != 0);   /* another jump table: refused */
    MCHK(kang_merge_file(ks, pb, &b) != 0 || !b.solved);                                      /* A and B together do */
    MCHK(!atomic_load(&ks->r->found) || pz_u256_cmp(&ks->r->found_key, &key) != 0);
    kang_free_test_state(ks);
    pz_result_line("merge of two computers' points", "%s", fails ? "FAILED" : "OK");
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

/* Two distinguished points on the same class reveal the key for every two kinds of different coefficient
 * (tame, wild on Q and on 3Q, either sign), the division by c_a - c_b included. Once on a small interval, and once on the largest one
 * the kangaroo takes with the key near its end, where the pairs of walkers on 3Q give 6x = d2 - d1 with 6x close to the limit of an i192. */
static int solve_selftest(void)
{
    int fails = 0, pairs = 0;
    const i192 zero = i192_zero();
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);                                   /* "key found" forty times */
    for (int big = 0; big < 2; big++) {
        const unsigned wbits = big ? KANG_MAX_W_LOG2 : 40;
        pz_u256 off = pz_u256_from_u64(123456789);         /* the offset of the key from the base */
        if (big) { off = pz_u256_pow2(wbits); pz_u256_sub(&off, &off, &(pz_u256){{1234568, 0, 0, 0}}); }
        for (unsigned ka = 0; ka < 5; ka++)
            for (unsigned kb = 0; kb < 5; kb++) {
                if (kang_kind_coef(ka) == kang_kind_coef(kb)) continue;
                kang_state *ks = kang_make_test_state_at(wbits, 1, &off);
                if (!ks) return fails + 1;
                pz_u256 key = pz_u256_pow2(90);
                pz_u256_add(&key, &key, &off);                 /* the key of kang_make_test_state_at */
                const i192 X = i192_from_signed(false, &off);
                const pz_u256 dm = pz_u256_from_u64(1000003 + 7919 * (u64)(ka * 5 + kb));
                dp_rec a, b;
                b.d = i192_from_signed((ka + kb) & 1, &dm);
                i192 E = i192_mul_small(&X, kang_kind_coef(kb)), cx = i192_mul_small(&X, kang_kind_coef(ka));
                i192_add(&E, &E, &b.d);                      /* E = c_b X + d_b = c_a X + d_a */
                i192_sub(&a.d, &E, &cx);
                const u64 tag = tag_of(&zero, 0, &E) & ~7ULL;
                a.tagk = tag | ka;
                b.tagk = tag | kb;
                if (kang_insert_dps(ks, &a, 1, NULL) || !kang_insert_dps(ks, &b, 1, NULL) || !atomic_load(&ks->r->found) || pz_u256_cmp(&ks->r->found_key, &key) != 0) {
                    fails++;
                    printf("  solution between kinds %u and %u on 2^%u keys: FAILED\n", ka, kb, wbits);
                }
                pairs++;
                kang_free_test_state(ks);
            }
    }
    pz_log_set_quiet(was_quiet);
    char lb[64];
    snprintf(lb, sizeof lb, "collisions between all kinds of walkers (%d pairs)", pairs);
    pz_result_line(lb, "%s", fails ? "FAILED" : "OK");
    return fails;
}

/* The limits of the interval, and the walkers on the largest one: every start must be the point (sigma*c*X + d)*G of its exponent. The
 * wild walkers on 3Q start as far as 3W below zero, and an exponent beyond +-2^191 would wrap in the i192 while its point does not. */
static int range_selftest(void)
{
    int fails = 0;
#define RCHK(cond) do { if (!(cond)) { fails++; printf("  range: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    const pz_u256 one = pz_u256_from_u64(1), max = pz_u256_pow2(KANG_MAX_W_LOG2);
    pz_u256 t;
    RCHK(kang_range_error(&max) == NULL);
    pz_u256_add(&t, &max, &one);
    RCHK(kang_range_error(&t) != NULL);                    /* one key beyond the limit */
    t = pz_u256_pow2(KANG_MAX_W_LOG2 + 2);
    RCHK(kang_range_error(&t) != NULL);
    t = pz_u256_pow2(16);
    RCHK(kang_range_error(&t) == NULL);
    pz_u256_sub(&t, &t, &one);
    RCHK(kang_range_error(&t) != NULL);                    /* too small */
    pz_u256 off = pz_u256_pow2(KANG_MAX_W_LOG2);
    pz_u256_sub(&off, &off, &(pz_u256){{1234568, 0, 0, 0}});     /* the key near the end of the interval */
    kang_state *ks = kang_make_test_state_at(KANG_MAX_W_LOG2, 1, &off);
    if (!ks) return fails + 1;
    const i192 X = i192_from_signed(false, &off);
    const int K = 600;
    kang_worker *w = kang_worker_new(ks, K, 3);
    kang_init_herd(ks, w);
    int wrong = 0, wild3 = 0;
    for (int i = 0; i < K; i++) {
        const i192 cx = i192_mul_small(&X, (int)w->kind[i] * w->sigma[i]);
        pz_fe x = w->x[i];
        pz_fe_normalize(&x);
        wrong += x.n[2] != tag_of(&cx, 1, &w->d[i]);
        wild3 += w->kind[i] == 3;
    }
    RCHK(wild3 > K / 10);                                  /* the walkers that start farthest from zero are there */
    RCHK(wrong == 0);
    kang_worker_free(w);
    kang_free_test_state(ks);
    char lb[64];
    snprintf(lb, sizeof lb, "kangaroo, intervals up to 2^%d keys", KANG_MAX_W_LOG2);
    pz_result_line(lb, "%s", fails ? "FAILED" : "OK");
    return fails;
}

/* ---- the table of distinguished points: growth and thinning ---- */

static int table_noise;      /* failures printed: one broken rule would print thousands */
#define TCHK(cond) do { if (!(cond)) { fails++; if (table_noise++ < 10) printf("  table: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)

static u64 table_rnd(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static u64 table_occupied(const kang_state *ks)
{
    u64 n = 0;
    for (u64 i = 0; i < ks->cap; i++) n += ks->tab[i].tagk != 0 || (ks->tab[i].d.w[0] | ks->tab[i].d.w[1] | ks->tab[i].d.w[2]);
    return n;
}

/* A test state whose table starts with 64 slots (as with PZ_TEST_DP_CAP=64) and may use `limit` entries' worth of memory. */
static kang_state *small_table_state(u64 limit)
{
    const long long before = g_env.test_dp_cap;
    g_env.test_dp_cap = 64;
    kang_state *ks = kang_make_test_state(40, 1);
    g_env.test_dp_cap = before;
    if (ks) ks->r->mem_limit = limit * sizeof(dp_rec);
    return ks;
}

/* A random record of the tame kind; d tells the records apart. */
static dp_rec table_record(u64 *s, u64 id)
{
    dp_rec r;
    memset(&r, 0, sizeof r);
    r.tagk = (table_rnd(s) & ~7ULL) | KIND_TAME;
    r.d.w[0] = id + 1;
    return r;
}

/* After the table was rebuilt: the entries that were inserted and satisfy the present rule are all in it, nothing else is, the count
 * and the copies for the progress line are right, and a lookup finds every one of them (inserting a point that is there again is a
 * duplicate and changes nothing; one that the rule drops is refused). rc[i] is what the insertion of recs[i] returned. */
static int table_check(kang_state *ks, const dp_rec *recs, const int *rc, int n)
{
    int fails = 0;
    u64 want = 0;
    for (int i = 0; i < n; i++) want += rc[i] == 1 && !((recs[i].tagk >> KANG_KSHIFT) & ks->thin_mask);
    TCHK(ks->count == want);
    TCHK(table_occupied(ks) == ks->count);
    TCHK(atomic_load(&ks->count_mirror) == ks->count && atomic_load(&ks->cap_mirror) == ks->cap && atomic_load(&ks->thin_mirror) == ks->thin_level);
    if (ks->count * 10 >= ks->cap * 8) { TCHK(!"room for the lookups"); return fails; }      /* they would rebuild the table again */
    const u64 before = ks->count;
    for (int i = 0; i < n; i++) {
        if (rc[i] != 1) continue;
        const bool stays = !((recs[i].tagk >> KANG_KSHIFT) & ks->thin_mask);
        TCHK(kang_table_insert(ks, &recs[i], true) == (stays ? 0 : -1));
    }
    TCHK(ks->count == before);
    return fails;
}

/* Thinning is done in place: on many small tables filled until they thinned four times, every entry that stays is found afterwards. */
static int table_thin_checks(void)
{
    int fails = 0;
    kang_state *ks = small_table_state(64);
    if (!ks) return 1;
    enum { N = 1500 };
    dp_rec *recs = pz_xmalloc(N * sizeof *recs);
    int *rc = pz_xmalloc(N * sizeof *rc);
    unsigned deepest = 99;
    for (int trial = 0; trial < 300; trial++) {
        memset(ks->tab, 0, ks->cap * sizeof(dp_rec));
        ks->count = 0;
        ks->thin_mask = 0;
        ks->thin_level = 0;
        atomic_store(&ks->count_mirror, 0);
        atomic_store(&ks->thin_mirror, 0);
        u64 s = 0x9E3779B97F4A7C15ULL * (u64)(trial + 1);
        int n = 0;
        unsigned level = 0;
        while (n < N && ks->thin_level < 4) {
            recs[n] = table_record(&s, (u64)n);
            rc[n] = kang_table_insert(ks, &recs[n], false);
            n++;
            if (ks->thin_level != level) {
                level = ks->thin_level;
                fails += table_check(ks, recs, rc, n);
            }
        }
        if (level < deepest) deepest = level;
        fails += table_check(ks, recs, rc, n);
    }
    TCHK(deepest == 4);
    free(recs);
    free(rc);
    kang_free_test_state(ks);
    return fails;
}

/* A point that meets one in the table still solves the key after the table was thinned (and the entry moved in the process). */
static int table_collision_checks(void)
{
    int fails = 0;
    const pz_u256 m = pz_u256_from_u64(123456789);      /* the offset of the key of kang_make_test_state from the base */
    pz_u256 want = pz_u256_pow2(90);
    pz_u256_add(&want, &want, &m);
    const i192 X = i192_from_signed(false, &m);
    const u64 km = ((u64)1 << KANG_KSHIFT) - 1;
    dp_rec ta = {0, i192_zero()}, wb = ta;
    for (u64 dw0 = 777;; dw0++) {      /* a pair of points that meet, whose tag has the bits clear that four levels of thinning look at */
        const pz_u256 dw256 = pz_u256_from_u64(dw0);
        const i192 dw = i192_from_signed(false, &dw256);
        const u64 tag = tag_of(&X, 1, &dw) & ~km;
        if (!tag || ((tag >> KANG_KSHIFT) & 15)) continue;
        i192 dt;
        i192_add(&dt, &X, &dw);
        ta.tagk = tag | KIND_TAME; ta.d = dt;
        wb.tagk = tag | KIND_WILD_P; wb.d = dw;
        break;
    }
    int moved = 0;
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(2);                                    /* the thinning warns, and "key found" */
    for (int sc = 0; sc < 40; sc++) {
        kang_state *ks = small_table_state(64);
        if (!ks) return fails + 1;
        u64 s = 0xD1B54A32D192ED03ULL * (u64)(sc + 1), at = 0;
        unsigned level = 0;
        for (int i = 0; i < 3000 && ks->thin_level < 4; i++) {
            if (i == 20 + sc) {                              /* among the others, so that it can be pushed behind its first probe */
                TCHK(kang_table_insert(ks, &ta, false) == 1);
                while (at < ks->cap && ks->tab[at].tagk != ta.tagk) at++;
            }
            const dp_rec f = table_record(&s, (u64)i);
            kang_table_insert(ks, &f, false);
            if (ks->thin_level == level) continue;
            level = ks->thin_level;
            u64 now = 0;
            while (now < ks->cap && ks->tab[now].tagk != ta.tagk) now++;
            TCHK(now < ks->cap);                            /* the entry is still there */
            if (now != at) moved++;
            at = now;
        }
        TCHK(ks->thin_level == 4);
        TCHK(kang_table_insert(ks, &wb, false) == 2 && atomic_load(&ks->r->found) && !pz_u256_cmp(&ks->r->found_key, &want));
        kang_free_test_state(ks);
    }
    pz_log_set_quiet(was_quiet);
    TCHK(moved > 0);                                        /* the thinning did move the entry in some of the tables */
    return fails;
}

/* Growth: by doubling, with the old and the new table together within the limit; nothing lost, and a thinning when the next size would not fit. */
static int table_grow_checks(void)
{
    int fails = 0;
    enum { N = 6000 };
    dp_rec *recs = pz_xmalloc(N * sizeof *recs);
    int *rc = pz_xmalloc(N * sizeof *rc);
    for (int tight = 0; tight < 2; tight++) {
        const u64 limit = tight ? 512 : (1 << 20);          /* entries */
        kang_state *ks = small_table_state(limit);
        if (!ks) return fails + 1;
        u64 s = 0xA0761D6478BD642FULL, cap = ks->cap;
        TCHK(cap == 64);
        for (int n = 0; n < N;) {
            recs[n] = table_record(&s, (u64)n);
            rc[n] = kang_table_insert(ks, &recs[n], false);
            n++;
            if (ks->cap != cap) {
                TCHK(ks->cap == 2 * cap && 3 * cap <= limit);
                cap = ks->cap;
                fails += table_check(ks, recs, rc, n);
            }
        }
        if (tight) TCHK(ks->cap == 256 && ks->thin_level >= 1);     /* 128 -> 256 needs 384 entries, 256 -> 512 would need 768 */
        else TCHK(ks->cap == 8192 && ks->thin_level == 0);
        fails += table_check(ks, recs, rc, N);
        kang_free_test_state(ks);
    }
    free(recs);
    free(rc);
    return fails;
}

/* The size the table starts with: the smallest power of two that holds the points expected at 60 % load, never above what the memory limit
 * allows. A small --dp-bits on a large interval (puzzle 140 with --dp-bits 2) expects more points than a 64-bit number holds: the limit then
 * decides. (The number has to be cut before it is converted to an integer: the conversion of a value out of range is undefined, and gives a
 * table of 4096 entries on x86-64. -fsanitize=float-cast-overflow reports it.) */
static int table_size_checks(void)
{
    int fails = 0;
    const u64 top = (256ULL << 20) / sizeof(dp_rec);           /* the limit of kang_make_test_state, a power of two */
    static const struct { unsigned wbits, dp; } cases[] = {{40, 2}, {40, 4}, {40, 10}, {64, 2}, {100, 2}, {139, 2}, {KANG_MAX_W_LOG2, 2},
                                                           {KANG_MAX_W_LOG2, 3}, {KANG_MAX_W_LOG2, 20}, {KANG_MAX_W_LOG2, 60}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        kang_state *ks = kang_make_test_state_dp(cases[i].wbits, 1, &(pz_u256){{123456789, 0, 0, 0}}, cases[i].dp);
        if (!ks) { TCHK(!"state"); continue; }
        const long double need = 4.0L * ks->sqrtW / ldexpl(1.0L, (int)ks->P.dp_bits) / 0.6L + 4096;
        u64 expect = 4096;
        while ((long double)expect < need && (expect << 1) <= top) expect <<= 1;
        TCHK(ks->P.dp_bits >= 2 && ks->cap == expect && ks->mask == expect - 1 && ks->tab != NULL);
        if (need >= (long double)top) TCHK(ks->cap == top);                      /* too many points for any table: the memory limit decides */
        kang_free_test_state(ks);
    }
    return fails;
}

static int table_selftest(void)
{
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(2);                                    /* "halving the density of stored points", hundreds of times */
    int fails = table_thin_checks();
    fails += table_grow_checks();
    fails += table_size_checks();
    pz_log_set_quiet(was_quiet);
    fails += table_collision_checks();
    pz_result_line("distinguished-point table: growth and thinning", "%s", fails ? "FAILED" : "OK");
    return fails;
}

int kang_merge_selftest(void)
{
    int fails = merge_selftest();
    fails += solve_selftest();
    fails += table_selftest();
    return fails + range_selftest();
}

/* ---- loading a log of distinguished points ---- */

#define FCHK(cond) do { if (!(cond)) { fails++; printf("  log loading: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)

/* Writes the log of the campaign of `ks`: the header (of version 4 with `v4`), `n` records of distinct points, then `tail` bytes of a
 * record cut short. */
static bool write_test_log(const kang_state *ks, int n, int tail, bool v4)
{
    u8 fp[32];
    kang_log_fp(ks, fp);
    FILE *f = fopen(ks->log_path, "wb");
    if (!f) return false;
    fwrite(v4 ? KANG_LOG_MAGIC4 : KANG_LOG_MAGIC, 1, 6, f);
    fwrite(fp, 1, 32, f);
    if (!v4) fwrite(ks->jump_tag, 1, 4, f);
    dp_rec rec;
    memset(&rec, 0, sizeof rec);
    for (int i = 0; i < n; i++) {
        rec.tagk = ((u64)(i + 1) << 20) | KIND_TAME;
        fwrite(&rec, sizeof rec, 1, f);
    }
    for (int i = 0; i < tail; i++) fputc(0xAB, f);
    return fclose(f) == 0;
}

static long long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    const long long size = fseeko(f, 0, SEEK_END) == 0 ? (long long)ftello(f) : -1;
    fclose(f);
    return size;
}

#ifdef __GLIBC__
/* A stream of bytes that fails with an I/O error once `fail_at` bytes were read (or ends there when it is beyond the data). */
typedef struct { const u8 *p; size_t len, pos, fail_at; } flaky_src;

static ssize_t flaky_read(void *c, char *buf, size_t n)
{
    flaky_src *k = c;
    const size_t end = k->fail_at < k->len ? k->fail_at : k->len;
    if (k->pos >= end) return k->fail_at < k->len ? -1 : 0;
    const size_t m = end - k->pos < n ? end - k->pos : n;
    memcpy(buf, k->p + k->pos, m);
    k->pos += m;
    return (ssize_t)m;
}
#endif

/* What the loading of dp.bin does with what it finds: it cuts off a record cut short at the end and nothing else (not the records after
 * the one that solved the key), a read error is an error, not the end of the file, a file shorter than its header is started again, and
 * a file of a newer version is left alone. */
static int log_file_checks(void)
{
    int fails = 0;
    const struct { int n, tail; } cases[] = {{0, 0}, {100, 0}, {100, 13}, {100, 31}, {100, 1}, {0, 20}};
    for (size_t c = 0; c < 2 * (sizeof cases / sizeof *cases); c++) {
        const bool v4 = c >= sizeof cases / sizeof *cases;                /* the same with logs of version 4 */
        const int n = cases[c % (sizeof cases / sizeof *cases)].n, tail = cases[c % (sizeof cases / sizeof *cases)].tail;
        const long long hlen = v4 ? KANG_LOG_HDR4 : KANG_LOG_HDR;
        kang_state *ks = kang_make_test_state(40, 1);
        if (!ks) { fails++; break; }
        FCHK(write_test_log(ks, n, tail, v4));
        FCHK(kang_load_log(ks) == 0 && ks->log);
        FCHK(ks->loaded == (u64)n && ks->count == (u64)n && ks->log_recs == (u64)n && ks->log_hdr == hlen);
        if (ks->log) { fclose(ks->log); ks->log = NULL; }
        FCHK(file_size(ks->log_path) == hlen + 32LL * n);                /* a torn record is gone, the whole ones are all there */
        kang_free_test_state(ks);
    }
    {   /* a log of version 4 stays one: new points are appended to it, and after a failed write it is cut back to whole records after
         * its own header */
        kang_state *ks = kang_make_test_state(40, 1);
        if (!ks) return fails + 1;
        FCHK(write_test_log(ks, 10, 0, true));
        FCHK(kang_load_log(ks) == 0 && ks->log && ks->log_hdr == KANG_LOG_HDR4);
        if (ks->log) {
            fputs("torn", ks->log);                                     /* a write that stopped in the middle of a record */
            fflush(ks->log);
            ks->log_broken = true;
            dp_rec rec;
            memset(&rec, 0, sizeof rec);
            rec.tagk = ((u64)77 << 20) | KIND_TAME;
            kang_insert_dps(ks, &rec, 1, NULL);
            FCHK(kang_flush_log(ks) == 0 && !ks->log_broken && ks->log_recs == 11);
            fclose(ks->log);
            ks->log = NULL;
        }
        FCHK(file_size(ks->log_path) == KANG_LOG_HDR4 + 32 * 11);
        FILE *f = fopen(ks->log_path, "rb");
        u8 m[6];
        FCHK(f && fread(m, 1, 6, f) == 6 && !memcmp(m, KANG_LOG_MAGIC4, 6));
        if (f) fclose(f);
        kang_free_test_state(ks);
    }
    {   /* a collision in the log: the load stops at the record that solves the key, and the records after it stay in the file */
        kang_state *ks = kang_make_test_state(40, 1);
        if (!ks) return fails + 1;
        pz_u256 m = pz_u256_from_u64(123456789);         /* the offset of the key of kang_make_test_state from the base */
        const i192 X = i192_from_signed(false, &m);
        const pz_u256 w777 = pz_u256_from_u64(777);
        const i192 dw = i192_from_signed(false, &w777);
        i192 dt;
        i192_add(&dt, &X, &dw);
        const u64 km = ((u64)1 << KANG_KSHIFT) - 1;
        dp_rec ta = {(tag_of(&X, 0, &dt) & ~km) | KIND_TAME, dt}, wb = {(tag_of(&X, 1, &dw) & ~km) | KIND_WILD_P, dw};
        u8 fp[32];
        kang_log_fp(ks, fp);
        FILE *f = fopen(ks->log_path, "wb");
        FCHK(f != NULL);
        if (f) {
            fwrite(KANG_LOG_MAGIC, 1, 6, f);
            fwrite(fp, 1, 32, f);
            fwrite(ks->jump_tag, 1, 4, f);
            fwrite(&ta, sizeof ta, 1, f);
            fwrite(&wb, sizeof wb, 1, f);
            dp_rec rec;
            memset(&rec, 0, sizeof rec);
            for (int i = 0; i < 10; i++) {
                rec.tagk = ((u64)(i + 1) << 20) | KIND_TAME;
                fwrite(&rec, sizeof rec, 1, f);
            }
            fclose(f);
        }
        FCHK(kang_load_log(ks) == 0 && atomic_load(&ks->r->found) && ks->loaded == 2);
        if (ks->log) { fclose(ks->log); ks->log = NULL; }
        FCHK(file_size(ks->log_path) == KANG_LOG_HDR + 32 * 12);
        kang_free_test_state(ks);
    }
    {   /* a file shorter than its header is an interrupted creation: the log starts again (with the header of this version, also after the
         * beginning of a header of version 4). Another short file is not ours */
        u8 want[KANG_LOG_HDR];
        const int lens[] = {0, 1, 6, 20, 37, 38, 41, -6, -20, -37};
        for (size_t c = 0; c < sizeof lens / sizeof *lens; c++) {
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            memcpy(want, lens[c] < 0 ? KANG_LOG_MAGIC4 : KANG_LOG_MAGIC, 6);
            kang_log_fp(ks, want + 6);
            memcpy(want + 38, ks->jump_tag, 4);
            FILE *f = fopen(ks->log_path, "wb");
            FCHK(f != NULL);
            if (f) { fwrite(want, 1, (size_t)abs(lens[c]), f); fclose(f); }
            FCHK(kang_load_log(ks) == 1 && ks->log);
            if (ks->log) { fclose(ks->log); ks->log = NULL; }
            FCHK(file_size(ks->log_path) == KANG_LOG_HDR);
            FCHK(kang_load_log(ks) == 0 && ks->loaded == 0);          /* the next start loads the new file */
            if (ks->log) { fclose(ks->log); ks->log = NULL; }
            kang_free_test_state(ks);
        }
        kang_state *ks = kang_make_test_state(40, 1);
        if (!ks) return fails + 1;
        FILE *f = fopen(ks->log_path, "wb");
        FCHK(f != NULL);
        if (f) { fputs("hello", f); fclose(f); }
        FCHK(kang_load_log(ks) == -1 && !ks->log && file_size(ks->log_path) == 5);
        kang_free_test_state(ks);
    }
    {   /* the other magics: only PZDP1 to PZDP3 are older layouts that are set aside; PZDP4 is still read; PZDP6 and the like come from a newer
         * version and stay where they are */
        const u8 m[][6] = {{'P', 'Z', 'D', 'P', '1', '\n'}, {'P', 'Z', 'D', 'P', '3', '\n'}, {'P', 'Z', 'D', 'P', '4', '\n'}, {'P', 'Z', 'D', 'P', '5', '\n'},
                           {'P', 'Z', 'D', 'P', '6', '\n'}, {'P', 'Z', 'D', 'P', '0', '\n'}, {'P', 'Z', 'D', 'P', 'x', '\n'}, {'P', 'Z', 'D', 'Q', '5', '\n'}};
        const int want[] = {KANG_LOG_OLDER, KANG_LOG_OLDER, KANG_LOG_V4, KANG_LOG_CURRENT, KANG_LOG_NEWER, KANG_LOG_NEWER, KANG_LOG_NEWER, KANG_LOG_FOREIGN};
        for (size_t c = 0; c < sizeof want / sizeof *want; c++) FCHK(kang_log_class(m[c]) == want[c]);
        const char digits[] = "26";
        for (size_t c = 0; c < sizeof digits - 1; c++) {
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            u8 body[38 + 3 * 32];
            memset(body, 0x5A, sizeof body);
            memcpy(body, "PZDP", 4);
            body[4] = (u8)digits[c];
            body[5] = '\n';
            FILE *f = fopen(ks->log_path, "wb");
            FCHK(f != NULL);
            if (f) { fwrite(body, 1, sizeof body, f); fclose(f); }
            char aside[700];
            snprintf(aside, sizeof aside, "%s.v%c", ks->log_path, digits[c]);
            if (digits[c] == '2') {                       /* an older layout: set aside, and a new log */
                FCHK(kang_load_log(ks) == 1 && ks->log);
                if (ks->log) { fclose(ks->log); ks->log = NULL; }
                FCHK(file_size(aside) == (long long)sizeof body && file_size(ks->log_path) == KANG_LOG_HDR);
            } else {                                      /* a newer version's: an error, the file as it was, nothing set aside */
                FCHK(kang_load_log(ks) == -1 && !ks->log);
                FCHK(file_size(ks->log_path) == (long long)sizeof body && file_size(aside) == -1);
                u8 back[sizeof body];
                FILE *g = fopen(ks->log_path, "rb");
                FCHK(g && fread(back, 1, sizeof back, g) == sizeof back && !memcmp(back, body, sizeof body));
                if (g) fclose(g);
            }
            kang_free_test_state(ks);
        }
        for (int k = 0; k < 2; k++) {   /* this version's magic with another campaign's fingerprint, or with another jump table: refused, untouched */
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            u8 other[KANG_LOG_HDR];
            memcpy(other, KANG_LOG_MAGIC, 6);
            if (k == 0) memset(other + 6, 0x5A, 32);
            else kang_log_fp(ks, other + 6);
            memcpy(other + 38, ks->jump_tag, 4);
            if (k == 1) other[41] ^= 0x80;
            FILE *f = fopen(ks->log_path, "wb");
            FCHK(f != NULL);
            if (f) { fwrite(other, 1, sizeof other, f); fclose(f); }
            FCHK(kang_load_log(ks) == -1 && !ks->log && file_size(ks->log_path) == KANG_LOG_HDR);
            kang_free_test_state(ks);
        }
    }
#ifdef __GLIBC__
    {   /* a read error in the middle of the records */
        enum { N = 100 };
        dp_rec recs[N];
        memset(recs, 0, sizeof recs);
        for (int i = 0; i < N; i++) recs[i].tagk = ((u64)(i + 1) << 20) | KIND_TAME;
        for (int fail = 0; fail < 2; fail++) {
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            flaky_src src = {(const u8 *)recs, sizeof recs, 0, fail ? 40 * sizeof(dp_rec) + 5 : sizeof recs + 1};
            cookie_io_functions_t io = {flaky_read, NULL, NULL, NULL};
            FILE *f = fopencookie(&src, "r", io);
            FCHK(f != NULL);
            if (f) {
                const long long n = kang_log_read(ks, f);
                fclose(f);
                FCHK(fail ? n == -1 : n == N);
                FCHK(ks->count == (fail ? 40u : (u64)N));
            }
            kang_free_test_state(ks);
        }
    }
#endif
    return fails;
}

int kang_log_file_selftest(void)
{
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(2);                                  /* the messages about files that are refused or repaired are the point of the test */
    const int fails = log_file_checks();
    pz_log_set_quiet(was_quiet);
    pz_result_line("distinguished-point log: loading", "%s", fails ? "FAILED" : "OK");
    return fails;
}

#ifndef _WIN32
#define LCHK(cond) do { if (!(cond)) { fails++; printf("  full disk: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)

/* The two buffers of the log hold `n` records each, and are empty. */
static void log_small_buffers(kang_state *ks, size_t n)
{
    ks->logbuf = pz_xrealloc(ks->logbuf, n * sizeof(dp_rec));
    ks->caplog = n;
    ks->nlog = 0;
    ks->wbuf = pz_xrealloc(ks->wbuf, n * sizeof(dp_rec));
    ks->capw = n;
    ks->nw = 0;
}

/* A write that stops in the middle of a record (file size limit): `n` points go in, the first ones reach the file, the limit hides the
 * rest, lifting it and flushing completes the file. With `dps` the points enter as the workers insert them (the thread that fills a buffer
 * writes it after it let go of the table), otherwise one by one without the unlock that writes (a block that was taken out waits for the
 * next push or flush). */
static int log_short_write_check(kang_state *ks, const char *name, u64 first_id, int n, bool dps)
{
    int fails = 0;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", ks->r->cfg.dir, name);
    pz_strlcpy(ks->log_path, path, sizeof ks->log_path);
    u8 fp[32];
    kang_log_fp(ks, fp);
    log_small_buffers(ks, 256);
    if (ks->log) { fclose(ks->log); ks->log = NULL; }
    ks->log = kang_log_create(ks, path, fp);
    ks->log_recs = 0;
    ks->log_broken = false;
    LCHK(ks->log != NULL);
    struct rlimit old, lim;
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    const bool have_rl = getrlimit(RLIMIT_FSIZE, &old) == 0;
    if (ks->log && have_rl) {
        lim = old;
        lim.rlim_cur = (rlim_t)(KANG_LOG_HDR + 1000 * sizeof(dp_rec) + 7);
        LCHK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
        dp_rec rec;
        memset(&rec, 0, sizeof rec);
        for (int i = 0; i < n; i++) {
            rec.tagk = ((first_id + (u64)i) << 20) | KIND_TAME;
            if (dps) kang_insert_dps(ks, &rec, 1, NULL);
            else kang_table_insert(ks, &rec, false);
        }
        LCHK(ks->log_broken);
        LCHK(ks->nlog + ks->nw > 0);                                 /* what the file refused is kept */
        LCHK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        LCHK(kang_flush_log(ks) == 0 && !ks->log_broken && ks->nlog == 0 && ks->nw == 0);
        LCHK(kang_flush_log(ks) == 0);
        fclose(ks->log);
        ks->log = NULL;
        FILE *f = fopen(path, "rb");
        u8 hdr[KANG_LOG_HDR];
        LCHK(f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, KANG_LOG_MAGIC, 6) && !memcmp(hdr + 38, ks->jump_tag, 4));
        int seen = 0;
        for (; f && seen < n + 1; seen++) {
            dp_rec r;
            if (fread(&r, sizeof r, 1, f) != 1) break;
            if (r.tagk != (((first_id + (u64)seen) << 20) | KIND_TAME)) { LCHK(0); break; }
        }
        LCHK(seen == n);                                             /* every point once, in order, no torn record */
        if (f) { u8 extra; LCHK(fread(&extra, 1, 1, f) == 0); fclose(f); }
    }
    signal(SIGXFSZ, old_sig);
    return fails;
}

#ifdef __GLIBC__
/* A log file that can be slowed down: the first write waits until `go`, and what is written is kept. */
typedef struct { u8 *data; size_t len, cap; bool gated; _Atomic int started, go; } slow_sink;

static ssize_t slow_write(void *c, const char *buf, size_t n)
{
    slow_sink *k = c;
    if (k->gated && !atomic_exchange(&k->started, 1))
        for (int i = 0; i < 3000 && !atomic_load(&k->go); i++) pz_sleep(0.005);      /* at most 15 s */
    if (k->len + n > k->cap) {
        k->cap = (k->len + n) * 2;
        k->data = pz_xrealloc(k->data, k->cap);
    }
    memcpy(k->data + k->len, buf, n);
    k->len += n;
    return (ssize_t)n;
}

typedef struct { kang_state *ks; u64 first_id; int n, batch; _Atomic int done; } log_feeder;

static void *log_feed_main(void *p)
{
    log_feeder *f = p;
    for (int i = 0; i < f->n;) {
        dp_rec recs[128];
        const int m = f->n - i < f->batch ? f->n - i : f->batch;       /* batch <= 128 */
        memset(recs, 0, sizeof recs);
        for (int j = 0; j < m; j++) { recs[j].tagk = ((f->first_id + (u64)(i + j)) << 20) | KIND_TAME; recs[j].d.w[0] = f->first_id + (u64)(i + j); }
        kang_insert_dps(f->ks, recs, m, NULL);
        i += m;
    }
    atomic_store(&f->done, 1);
    return NULL;
}

static bool log_wait(_Atomic int *flag, int ms)
{
    for (int i = 0; i < ms / 5 && !atomic_load(flag); i++) pz_sleep(0.005);
    return atomic_load(flag);
}

/* The n records of a sink from record `from` on are the points first_id .. first_id + n - 1, in this order. */
static bool sink_has(const slow_sink *k, size_t from, u64 first_id, size_t n)
{
    if (k->len < (from + n) * sizeof(dp_rec)) return false;
    for (size_t i = 0; i < n; i++) {
        dp_rec r;
        memcpy(&r, k->data + (from + i) * sizeof r, sizeof r);
        if (r.tagk != (((first_id + i) << 20) | KIND_TAME)) return false;
    }
    return true;
}

static kang_state *sink_state(slow_sink *k, size_t buf)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return NULL;
    log_small_buffers(ks, buf);
    const cookie_io_functions_t io = {NULL, slow_write, NULL, NULL};
    ks->log = fopencookie(k, "w", io);
    if (!ks->log) { kang_free_test_state(ks); return NULL; }
    return ks;
}

/* The write of the log happens outside the table lock. The disk is stopped in the middle of the write of a full buffer: another thread's
 * points go into the table meanwhile (nothing waits for the disk); when the other buffer fills as well, the inserting thread waits for the
 * write, loses nothing and does not enlarge the buffers; and many threads inserting with a flusher at work (the checkpoint) leave every point
 * in the file exactly once, in the order of each thread. */
static int log_thread_checks(void)
{
    int fails = 0;
    {   /* 1. points enter while a write is stuck */
        slow_sink k = {NULL, 0, 0, true, 0, 0};
        kang_state *ks = sink_state(&k, 64);
        if (!ks) return 1;
        log_feeder a = {ks, 1000, 65, 65, 0}, b = {ks, 2000, 10, 10, 0};
        pthread_t ta, tb;
        LCHK(pthread_create(&ta, NULL, log_feed_main, &a) == 0);       /* the 65th point fills the buffer: this thread writes it, and is stopped */
        LCHK(log_wait(&k.started, 5000));
        LCHK(pthread_create(&tb, NULL, log_feed_main, &b) == 0);
        LCHK(log_wait(&b.done, 3000));                                 /* with the old code this thread waits for the lock held by the write */
        atomic_store(&k.go, 1);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        LCHK(kang_flush_log(ks) == 0 && ks->nlog == 0 && ks->nw == 0);
        LCHK(k.len == 75 * sizeof(dp_rec) && atomic_load(&ks->dropped) == 0);
        LCHK(sink_has(&k, 0, 1000, 65) && sink_has(&k, 65, 2000, 10));
        fclose(ks->log);
        ks->log = NULL;
        kang_free_test_state(ks);
        free(k.data);
    }
    {   /* 2. both buffers full while the write is stuck: the thread waits */
        slow_sink k = {NULL, 0, 0, true, 0, 0};
        kang_state *ks = sink_state(&k, 64);
        if (!ks) return fails + 1;
        log_feeder a = {ks, 1000, 65, 65, 0}, b = {ks, 2000, 100, 100, 0};
        pthread_t ta, tb;
        LCHK(pthread_create(&ta, NULL, log_feed_main, &a) == 0);
        LCHK(log_wait(&k.started, 5000));
        LCHK(pthread_create(&tb, NULL, log_feed_main, &b) == 0);
        LCHK(!log_wait(&b.done, 300));                                 /* 63 points fit, the next one has no buffer: it waits for the disk */
        LCHK(ks->caplog == 64 && ks->capw == 64);                     /* and nothing was enlarged */
        atomic_store(&k.go, 1);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        LCHK(atomic_load(&b.done));
        LCHK(kang_flush_log(ks) == 0 && ks->nlog == 0 && ks->nw == 0);
        LCHK(k.len == 165 * sizeof(dp_rec) && atomic_load(&ks->dropped) == 0);
        LCHK(sink_has(&k, 0, 1000, 65) && sink_has(&k, 65, 2000, 100));        /* in the order of their insertion */
        fclose(ks->log);
        ks->log = NULL;
        kang_free_test_state(ks);
        free(k.data);
    }
    {   /* 3. four threads and the checkpoint */
        slow_sink k = {NULL, 0, 0, false, 0, 0};
        kang_state *ks = sink_state(&k, 64);
        if (!ks) return fails + 1;
        enum { T = 4, N = 20000 };
        log_feeder f[T];
        pthread_t th[T];
        for (int t = 0; t < T; t++) {
            f[t] = (log_feeder){ks, (u64)(t + 1) * 1000000, N, 50, 0};
            LCHK(pthread_create(&th[t], NULL, log_feed_main, &f[t]) == 0);
        }
        while (!atomic_load(&f[0].done) || !atomic_load(&f[1].done) || !atomic_load(&f[2].done) || !atomic_load(&f[3].done)) {
            LCHK(kang_flush_log(ks) == 0);
            pz_sleep(0.001);
        }
        for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
        LCHK(kang_flush_log(ks) == 0 && ks->nlog == 0 && ks->nw == 0);
        LCHK(k.len == (size_t)T * N * sizeof(dp_rec) && atomic_load(&ks->dropped) == 0);
        u64 next[T] = {0};
        bool ordered = k.len == (size_t)T * N * sizeof(dp_rec);
        for (size_t i = 0; ordered && i < (size_t)T * N; i++) {
            dp_rec r;
            memcpy(&r, k.data + i * sizeof r, sizeof r);
            const u64 t = r.d.w[0] / 1000000 - 1, j = r.d.w[0] % 1000000;
            if (t >= T || j != next[t]++) ordered = false;            /* each point once, the points of a thread in the order they were inserted */
        }
        LCHK(ordered);
        fclose(ks->log);
        ks->log = NULL;
        kang_free_test_state(ks);
        free(k.data);
    }
    return fails;
}
#endif

/* The log of distinguished points when the disk refuses writes: no overflow of the buffer, nothing lost, and after a write that stopped in the
 * middle of a record (file size limit) the file is cut back and completed without gaps or duplicates. Then the writes outside the table lock. */
int kang_log_fault_selftest(void)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return 1;
    int fails = 0;
    const size_t small = 256;
    log_small_buffers(ks, small);
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
            kang_table_insert(ks, &rec, false);
        }
        LCHK(ks->nlog + ks->nw == (size_t)N && ks->caplog >= ks->nlog);       /* the buffer grew instead of overflowing; no point was lost */
        LCHK(atomic_load(&ks->dropped) == 0);
        LCHK(kang_flush_log(ks) == -1 && ks->log_broken);
        LCHK(ks->nlog + ks->nw == (size_t)N);                                  /* and they are all still there for the next try */
        ks->log_broken = false;
        ks->nlog = ks->nw = 0;
    }
    /* 2. a file size limit that falls inside a record, once for each way in */
    fails += log_short_write_check(ks, "disk.bin", 100001, 2400, false);      /* other points than those of step 1, which are still in the table */
    fails += log_short_write_check(ks, "disk2.bin", 200001, 2400, true);
    /* 3. not even the header fits: the creation reports it (it used to hand back a stream whose failed write nobody looked at), and the file
     * that is left, the beginning of a header, is replaced by the next start */
    u8 fp[32];
    kang_log_fp(ks, fp);
    struct rlimit old, lim;
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    if (getrlimit(RLIMIT_FSIZE, &old) == 0) {
        char path[700];
        const int was_quiet = pz_log_quiet();
        pz_log_set_quiet(2);
        snprintf(path, sizeof path, "%s/short.bin", ks->r->cfg.dir);
        pz_strlcpy(ks->log_path, path, sizeof ks->log_path);
        lim = old;
        lim.rlim_cur = 10;
        LCHK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
        FILE *g = kang_log_create(ks, path, fp);
        LCHK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        if (g) fclose(g);
        LCHK(g == NULL);
        LCHK(file_size(path) == 10);
        LCHK(kang_load_log(ks) == 1 && ks->log && file_size(path) == KANG_LOG_HDR);
        if (ks->log) { fclose(ks->log); ks->log = NULL; }
        pz_log_set_quiet(was_quiet);
    }
    signal(SIGXFSZ, old_sig);
    kang_free_test_state(ks);
    pz_result_line("distinguished-point log on a full disk", "%s", fails ? "FAILED" : "OK");
    int tf = 0;
#ifdef __GLIBC__
    {
        const int q = pz_log_quiet();
        pz_log_set_quiet(2);
        fails += tf = log_thread_checks();
        pz_log_set_quiet(q);
    }
    pz_result_line("distinguished-point log: writes outside the table lock", "%s", tf ? "FAILED" : "OK");
#else
    pz_result_line("distinguished-point log: writes outside the table lock", "not tested here");
#endif
    return fails;
}
#else
int kang_log_fault_selftest(void) { pz_result_line("distinguished-point log on a full disk", "not tested on Windows"); return 0; }
#endif
