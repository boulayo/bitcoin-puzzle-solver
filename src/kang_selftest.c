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
 * (tame, wild on Q and on 3Q, either sign), the division by c_a - c_b included. */
static int solve_selftest(void)
{
    int fails = 0, pairs = 0;
    const i192 zero = i192_zero();
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);                                   /* "key found" twenty times */
    for (unsigned ka = 0; ka < 5; ka++)
        for (unsigned kb = 0; kb < 5; kb++) {
            if (kang_kind_coef(ka) == kang_kind_coef(kb)) continue;
            kang_state *ks = kang_make_test_state(40, 1);
            if (!ks) return fails + 1;
            pz_u256 key = pz_u256_pow2(90), m;
            pz_u256_add_u64(&key, &key, 123456789);       /* the key of kang_make_test_state */
            pz_u256_sub(&m, &key, &ks->base);
            const i192 X = i192_from_signed(false, &m);
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
                printf("  solution between kinds %u and %u: FAILED\n", ka, kb);
            }
            pairs++;
            kang_free_test_state(ks);
        }
    pz_log_set_quiet(was_quiet);
    char lb[64];
    snprintf(lb, sizeof lb, "collisions between all kinds of walkers (%d pairs)", pairs);
    pz_result_line(lb, "%s", fails ? "FAILED" : "OK");
    return fails;
}

int kang_merge_selftest(void) { return merge_selftest() + solve_selftest(); }

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
    signal(SIGXFSZ, old_sig);
    kang_free_test_state(ks);
    pz_result_line("distinguished-point log on a full disk", "%s", fails ? "FAILED" : "OK");
    return fails;
}
#else
int kang_log_fault_selftest(void) { pz_result_line("distinguished-point log on a full disk", "not tested on Windows"); return 0; }
#endif
