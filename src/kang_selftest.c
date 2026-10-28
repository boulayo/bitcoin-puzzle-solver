/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_selftest.c - self-tests of the kangaroo: the CPU step variants against the portable one, the merge of two computers' logs, the
 * solution of a collision between every pair of kinds of walkers, and the log of distinguished points on a full disk. */
#include "kangaroo_priv.h"
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
    fwrite(KANG_LOG_MAGIC, 1, 6, fa); fwrite(fp, 1, 32, fa);
    fwrite(KANG_LOG_MAGIC, 1, 6, fb); fwrite(fp, 1, 32, fb);
    u8 other[32];
    memset(other, 0x5A, sizeof other);
    fwrite(KANG_LOG_MAGIC, 1, 6, fc); fwrite(other, 1, 32, fc);
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

int kang_merge_selftest(void)
{
    int fails = merge_selftest();
    fails += solve_selftest();
    return fails + range_selftest();
}

/* ---- loading a log of distinguished points ---- */

#define FCHK(cond) do { if (!(cond)) { fails++; printf("  log loading: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)

/* Writes the log of the campaign of `ks`: the header, `n` records of distinct points, then `tail` bytes of a record cut short. */
static bool write_test_log(const kang_state *ks, int n, int tail)
{
    u8 fp[32];
    kang_log_fp(ks, fp);
    FILE *f = fopen(ks->log_path, "wb");
    if (!f) return false;
    fwrite(KANG_LOG_MAGIC, 1, 6, f);
    fwrite(fp, 1, 32, f);
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
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        kang_state *ks = kang_make_test_state(40, 1);
        if (!ks) { fails++; break; }
        FCHK(write_test_log(ks, cases[c].n, cases[c].tail));
        FCHK(kang_load_log(ks) == 0 && ks->log);
        FCHK(ks->loaded == (u64)cases[c].n && ks->count == (u64)cases[c].n && ks->log_recs == (u64)cases[c].n);
        if (ks->log) { fclose(ks->log); ks->log = NULL; }
        FCHK(file_size(ks->log_path) == 38 + 32LL * cases[c].n);        /* a torn record is gone, the whole ones are all there */
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
        FCHK(file_size(ks->log_path) == 38 + 32 * 12);
        kang_free_test_state(ks);
    }
    {   /* a file shorter than its header is an interrupted creation: the log starts again. Another short file is not ours */
        u8 want[38];
        const int lens[] = {0, 1, 6, 20, 37};
        for (size_t c = 0; c < sizeof lens / sizeof *lens; c++) {
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            memcpy(want, KANG_LOG_MAGIC, 6);
            kang_log_fp(ks, want + 6);
            FILE *f = fopen(ks->log_path, "wb");
            FCHK(f != NULL);
            if (f) { fwrite(want, 1, (size_t)lens[c], f); fclose(f); }
            FCHK(kang_load_log(ks) == 1 && ks->log);
            if (ks->log) { fclose(ks->log); ks->log = NULL; }
            FCHK(file_size(ks->log_path) == 38);
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
    {   /* the other magics: only PZDP1 to PZDP3 are older layouts that are set aside; PZDP5 and the like come from a newer version and stay where they are */
        const u8 m[][6] = {{'P', 'Z', 'D', 'P', '1', '\n'}, {'P', 'Z', 'D', 'P', '3', '\n'}, {'P', 'Z', 'D', 'P', '4', '\n'}, {'P', 'Z', 'D', 'P', '5', '\n'},
                           {'P', 'Z', 'D', 'P', '0', '\n'}, {'P', 'Z', 'D', 'P', 'x', '\n'}, {'P', 'Z', 'D', 'Q', '5', '\n'}};
        const int want[] = {KANG_LOG_OLDER, KANG_LOG_OLDER, KANG_LOG_CURRENT, KANG_LOG_NEWER, KANG_LOG_NEWER, KANG_LOG_NEWER, KANG_LOG_FOREIGN};
        for (size_t c = 0; c < sizeof want / sizeof *want; c++) FCHK(kang_log_class(m[c]) == want[c]);
        const char digits[] = "25";
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
                FCHK(file_size(aside) == (long long)sizeof body && file_size(ks->log_path) == 38);
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
        kang_state *ks = kang_make_test_state(40, 1);      /* this version's magic with another campaign's fingerprint: refused, untouched */
        if (!ks) return fails + 1;
        u8 other[38];
        memcpy(other, KANG_LOG_MAGIC, 6);
        memset(other + 6, 0x5A, 32);
        FILE *f = fopen(ks->log_path, "wb");
        FCHK(f != NULL);
        if (f) { fwrite(other, 1, sizeof other, f); fclose(f); }
        FCHK(kang_load_log(ks) == -1 && !ks->log && file_size(ks->log_path) == 38);
        kang_free_test_state(ks);
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
            kang_table_insert(ks, &rec, false);
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
    kang_log_fp(ks, fp);
    if (ks->log) { fclose(ks->log); ks->log = NULL; }                  /* the stream of step 1 (the resync of the log may have closed it already) */
    ks->log = kang_log_create(ks, path, fp);
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
            kang_table_insert(ks, &rec, false);
        }
        LCHK(ks->log_broken);
        LCHK(setrlimit(RLIMIT_FSIZE, &old) == 0);
        LCHK(kang_flush_log(ks) == 0 && !ks->log_broken && ks->nlog == 0);
        LCHK(kang_flush_log(ks) == 0);
        fclose(ks->log);
        ks->log = NULL;
        FILE *f = fopen(path, "rb");
        u8 hdr[38];
        LCHK(f && fread(hdr, 1, 38, f) == 38 && !memcmp(hdr, KANG_LOG_MAGIC, 6));
        int seen = 0;
        for (; f && seen < N + 1; seen++) {
            dp_rec r;
            if (fread(&r, sizeof r, 1, f) != 1) break;
            if (r.tagk != (((u64)(seen + 100001) << 20) | KIND_TAME)) { LCHK(0); break; }
        }
        LCHK(seen == N);                                             /* every point once, in order, no torn record */
        if (f) { u8 extra; LCHK(fread(&extra, 1, 1, f) == 0); fclose(f); }
    }
    /* 3. not even the header fits: the creation reports it (it used to hand back a stream whose failed write nobody looked at), and the file
     * that is left, the beginning of a header, is replaced by the next start */
    if (have_rl) {
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
        LCHK(kang_load_log(ks) == 1 && ks->log && file_size(path) == 38);
        if (ks->log) { fclose(ks->log); ks->log = NULL; }
        pz_log_set_quiet(was_quiet);
    }
    signal(SIGXFSZ, old_sig);
    kang_free_test_state(ks);
    pz_result_line("distinguished-point log on a full disk", "%s", fails ? "FAILED" : "OK");
    return fails;
}
#else
int kang_log_fault_selftest(void) { pz_result_line("distinguished-point log on a full disk", "not tested on Windows"); return 0; }
#endif
