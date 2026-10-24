/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_bsgs.c - baby-step giant-step: coverage arithmetic, table build/persistence, exhaustive solves with planted keys. */
#include "../src/bsgs.h"
#include "../src/bsgs_priv.h"
#include "../src/cpu_engine.h"
#include "../src/platform.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u64 rs = 0x2545F4914F6CDD1DULL;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static pz_ge pub_of(const pz_u256 *k) { pz_ge p; if (!pz_ec_mul_g(&p, k)) { printf("bad key\n"); exit(2); } return p; }

/* every d in [-H, H] must be a*S + b with |a| <= A and |b| <= T */
static void t_coverage(void)
{
    const u64 Ts[] = {1, 2, 3, 7, 50};
    const u64 Ws[] = {1, 2, 3, 10, 99, 100, 1001, 5000};
    for (size_t i = 0; i < sizeof Ts / sizeof *Ts; i++)
        for (size_t j = 0; j < sizeof Ws / sizeof *Ws; j++) {
            u64 T = Ts[i], W = Ws[j];
            pz_u256 lo = pz_u256_from_u64(1000), hi = pz_u256_from_u64(1000 + W - 1);
            pz_ge g = pz_ec_g;
            char err[200];
            bsgs_params p;
            CHECK(bsgs_params_init(&p, &lo, &hi, &g, T, err, sizeof err));
            CHECK(p.S == 2 * T + 1 && p.Ng == 2 * p.A + 1);
            for (u64 off = 0; off < W; off++) {
                pz_u256 key = pz_u256_from_u64(1000 + off);
                u64 idx = bsgs_giant_index(&p, &key);
                CHECK(idx >= 1 && idx <= p.Ng);
                /* the decomposition must exist with |b| <= T */
                __int128 a = (__int128)idx - 1 - (__int128)p.A;
                __int128 d = (__int128)off - (__int128)(((W - 1) >> 1));
                __int128 b = d - a * (__int128)p.S;
                CHECK(b >= -(__int128)T && b <= (__int128)T);
            }
        }
    pz_u256 lo = pz_u256_from_u64(5), hi = pz_u256_from_u64(100), out = pz_u256_from_u64(101);
    pz_ge g = pz_ec_g;
    char err[200];
    bsgs_params p;
    CHECK(bsgs_params_init(&p, &lo, &hi, &g, 4, err, sizeof err));
    CHECK(bsgs_giant_index(&p, &out) == 0);
    CHECK(!bsgs_params_init(&p, &hi, &lo, &g, 4, err, sizeof err));   /* empty range */
    CHECK(!bsgs_params_init(&p, &lo, &hi, &g, 0, err, sizeof err));
}

static void t_sizing(void)
{
    pz_u256 W = pz_u256_pow2(64);
    CHECK(bsgs_optimal_T(&W) == ((u64)1 << 31));
    bool viable;
    u64 T = bsgs_choose_T(&W, (u64)64 << 30, &viable);
    CHECK(viable && T == ((u64)1 << 31));
    T = bsgs_choose_T(&W, (u64)1 << 30, &viable);       /* 1 GiB: far below sqrt(W)/4 entries */
    CHECK(!viable && T < ((u64)1 << 28));
    pz_u256 big = pz_u256_pow2(139);
    bsgs_choose_T(&big, (u64)128 << 30, &viable);
    CHECK(!viable);                                     /* the open puzzles with a public key are out of reach */
    CHECK(bsgs_bytes_for(1000000) >= 8 * 1000000 && bsgs_max_T_for_bytes(bsgs_bytes_for(1000000)) >= 999000);
    CHECK(bsgs_mean_cost(&W, (u64)1 << 31) < 1.01L * 4294967296.0L && bsgs_mean_cost(&W, (u64)1 << 31) > 0.99L * 4294967296.0L);
}

static void t_table(void)
{
    char err[200];
    bsgs_table t;
    const u64 T = 30000;
    CHECK(bsgs_table_alloc(&t, T, err, sizeof err));
    _Atomic u64 u0 = 0, u1 = 0, u2 = 0;
    _Atomic u64 *units[3] = {&u0, &u1, &u2};
    CHECK(bsgs_table_build(&t, 3, units, NULL));
    CHECK(u0 + u1 + u2 >= T);
    CHECK(bsgs_table_count(&t, 2) == T);
    int missing = 0, wrong = 0;
    for (u64 b = 1; b <= T; b += (b < 200 ? 1 : 37)) {
        pz_ge q = pub_of(&(pz_u256){{b, 0, 0, 0}});
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        u64 got;
        if (!bsgs_table_contains(&t, x.n[0], x.n[1], &got)) missing++;
        else if (got != b) wrong++;   /* a tag collision may return another b: tolerated below */
    }
    CHECK(missing == 0);
    CHECK(wrong <= 1);
    int fp = 0;
    for (u64 b = T + 1; b <= T + 3000; b++) {
        pz_ge q = pub_of(&(pz_u256){{b, 0, 0, 0}});
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        u64 got;
        if (bsgs_table_contains(&t, x.n[0], x.n[1], &got)) fp++;
    }
    CHECK(fp <= 2);                     /* only tag collisions (2^-24 per probe) */
    /* a stop request aborts the build and reports it */
    bsgs_table t2;
    CHECK(bsgs_table_alloc(&t2, T, err, sizeof err));
    _Atomic int stop = 1;
    CHECK(!bsgs_table_build(&t2, 2, NULL, &stop));
    bsgs_table_free(&t2);

    /* persistence */
    char tdir[600], path[700];
    CHECK(pz_make_temp_dir(tdir, sizeof tdir, "pzbsgs"));
    snprintf(path, sizeof path, "%s/table.bin", tdir);
    CHECK(bsgs_table_save(&t, path, 3, err, sizeof err));
    u64 pt = 0;
    CHECK(bsgs_table_peek(path, &pt, err, sizeof err) && pt == T);
    bsgs_table l;
    CHECK(bsgs_table_load(&l, path, 2, err, sizeof err));
    CHECK(l.T == T && l.nslots == t.nslots && memcmp(l.slots, t.slots, t.nslots * 8) == 0);
    bsgs_table_free(&l);
    /* corruption in the body, the header, truncation and a foreign file must all be rejected */
    int f = pz_open_rw(path, false);
    CHECK(f >= 0);
    i64 size = pz_fd_size(f);
    CHECK(size > 4096);
    unsigned char byte;
    u64 body_off = 4096 + 8 * (t.nslots / 2);
    CHECK(pz_pread(f, &byte, 1, body_off) == 1);
    unsigned char flipped = byte ^ 0x10;
    CHECK(pz_pwrite(f, &flipped, 1, body_off) == 1);
    CHECK(!bsgs_table_load(&l, path, 2, err, sizeof err));
    CHECK(pz_pwrite(f, &byte, 1, body_off) == 1);
    CHECK(bsgs_table_load(&l, path, 2, err, sizeof err));
    bsgs_table_free(&l);
    CHECK(pz_pread(f, &byte, 1, 40) == 1);
    flipped = byte ^ 1;
    CHECK(pz_pwrite(f, &flipped, 1, 40) == 1);
    CHECK(!bsgs_table_peek(path, &pt, err, sizeof err));
    CHECK(pz_pwrite(f, &byte, 1, 40) == 1);
    CHECK(pz_fd_truncate(f, (u64)size - 8) == 0);
    CHECK(!bsgs_table_load(&l, path, 2, err, sizeof err));
    close(f);
    snprintf(path, sizeof path, "%s/does_not_exist", tdir);
    CHECK(!bsgs_table_peek(path, &pt, err, sizeof err));
    CHECK(pz_remove_tree(tdir) == 0);
    bsgs_table_free(&t);
}

static void solve_expect(const pz_u256 *lo, const pz_u256 *hi, const pz_u256 *key, u64 T, int nthreads)
{
    pz_ge P = pub_of(key);
    pz_u256 got = pz_u256_zero();
    u64 giants;
    bool ok = bsgs_solve(lo, hi, &P, T, nthreads, &got, &giants);
    char a[80], b[80], c[80], d[80];
    if (!ok || pz_u256_cmp(&got, key) != 0) {
        printf("  solve failed: range %s..%s T=%llu key %s found %s\n", pz_u256_to_hex(lo, a), pz_u256_to_hex(hi, b), (unsigned long long)T, pz_u256_to_hex(key, c), ok ? pz_u256_to_hex(&got, d) : "none");
        fails++;
    }
}

static void t_solve(void)
{
    /* assorted ranges and table sizes, keys at the edges, on the centre and exactly on giant steps (b = 0) */
    const struct { u64 lo, W, T; } cfgs[] = {
        {1, 1, 1}, {1, 2, 1}, {7, 3, 1}, {100, 1000, 3}, {1 << 20, 4096, 32}, {123456789, 1000003, 500}, {1ULL << 30, 1 << 22, 1024},
        {99, 700000, 777}, {1ULL << 40, (1 << 24) + 5, 1 << 11}, {5, 100000, 5000},
    };
    for (size_t i = 0; i < sizeof cfgs / sizeof *cfgs; i++) {
        pz_u256 lo = pz_u256_from_u64(cfgs[i].lo), hi = pz_u256_from_u64(cfgs[i].lo + cfgs[i].W - 1);
        u64 T = cfgs[i].T, W = cfgs[i].W, S = 2 * T + 1;
        u64 c = cfgs[i].lo + ((W - 1) >> 1);
        u64 cand[16], nc = 0;
        cand[nc++] = cfgs[i].lo;
        cand[nc++] = cfgs[i].lo + W - 1;
        cand[nc++] = c;
        cand[nc++] = cfgs[i].lo + rnd() % W;
        cand[nc++] = cfgs[i].lo + rnd() % W;
        cand[nc++] = cfgs[i].lo + rnd() % W;
        for (int m = -2; m <= 2; m++) {            /* c + a*S + {0, +T, -T} for a few a */
            for (int dd = -1; dd <= 1; dd++) {
                int64_t k = (int64_t)c + (int64_t)m * (int64_t)S + (int64_t)dd * (int64_t)T;
                if (nc < 16 && k >= (int64_t)cfgs[i].lo && k <= (int64_t)(cfgs[i].lo + W - 1) && (m == 2 || dd == 0 || m == -2)) cand[nc++] = (u64)k;
            }
        }
        for (u64 j = 0; j < nc; j++) {
            pz_u256 key = pz_u256_from_u64(cand[j]);
            solve_expect(&lo, &hi, &key, T, 1 + (int)(j % 3));
        }
    }
    CHECK(atomic_load(&bsgs_slow_windows) > 0);   /* the exact path for keys sitting on a giant step was exercised */
    /* every key of a small range, so no giant/baby combination is skipped */
    {
        pz_u256 lo = pz_u256_from_u64(5000), hi = pz_u256_from_u64(5000 + 1500 - 1);
        for (u64 k = 5000; k < 5000 + 1500; k += 7) {
            pz_u256 key = pz_u256_from_u64(k);
            solve_expect(&lo, &hi, &key, 20, 2);
        }
        for (u64 k = 5000 + 1; k < 5000 + 1500; k += 211) {
            pz_u256 key = pz_u256_from_u64(k);
            solve_expect(&lo, &hi, &key, 1, 1);
        }
    }
    /* huge keys: the offsets wrap around the 256-bit value without trouble */
    {
        pz_u256 base = pz_u256_pow2(255), lo, hi;
        pz_u256_add_u64(&lo, &base, 12345);
        pz_u256_add_u64(&hi, &lo, 99999);
        pz_u256 key;
        pz_u256_add_u64(&key, &lo, 40001);
        solve_expect(&lo, &hi, &key, 100, 2);
    }
    /* a key outside the range is not found */
    {
        pz_u256 lo = pz_u256_from_u64(1000), hi = pz_u256_from_u64(1999), key = pz_u256_from_u64(5000000), got;   /* whole groups are scanned: a key just past the end may still be found */
        pz_ge P = pub_of(&key);
        u64 giants;
        CHECK(!bsgs_solve(&lo, &hi, &P, 16, 2, &got, &giants));
    }
    /* the public key equals the centre */
    {
        pz_u256 lo = pz_u256_from_u64(1000), hi = pz_u256_from_u64(1999), key = pz_u256_from_u64(1000 + 499);
        solve_expect(&lo, &hi, &key, 16, 1);
    }
}


/* keys whose giant sits on, just before and just after the edges of the 1024-giant windows (and of the per-thread
 * slices), with the baby offset at 0, +-1 and +-T: no combination may be skipped at a window seam */
static void t_window_edges(void)
{
    const u64 W = (u64)1 << 24, T = 256, S = 2 * T + 1, lo0 = 1ULL << 33;
    pz_u256 lo = pz_u256_from_u64(lo0), hi = pz_u256_from_u64(lo0 + W - 1);
    bsgs_params p;
    char err[200];
    pz_ge g = pub_of(&(pz_u256){{1, 0, 0, 0}});
    CHECK(bsgs_params_init(&p, &lo, &hi, &g, T, err, sizeof err));
    CHECK(p.Ng > 8 * BS_GRP);
    const u64 c = lo0 + ((W - 1) >> 1);
    int done = 0;
    for (int w = 1; w <= 8; w++) {
        for (int dg = -1; dg <= 1; dg++) {
            const int64_t a = -(int64_t)p.A + (int64_t)w * BS_GRP + dg;
            if (a < -(int64_t)p.A || a > (int64_t)p.A) continue;
            const int64_t offs[] = {0, 1, -1, (int64_t)T, -(int64_t)T, 17};
            for (size_t o = 0; o < sizeof offs / sizeof *offs; o++) {
                int64_t k = (int64_t)c + a * (int64_t)S + offs[o];
                if (k < (int64_t)lo0 || k > (int64_t)(lo0 + W - 1)) continue;
                pz_u256 key = pz_u256_from_u64((u64)k);
                solve_expect(&lo, &hi, &key, T, 1 + (w + dg + 8) % 3);
                done++;
            }
        }
    }
    CHECK(done > 60);
}

typedef struct { pz_u256 key; int hits; _Atomic int *stop; bool stop_on_hit; } hit_rec;
static void on_hit(void *u, const pz_u256 *k) { hit_rec *h = u; h->hits++; h->key = *k; if (h->stop_on_hit) atomic_store(h->stop, 1); }

/* bsgs_scan: return value, progress accounting and the stop flag */
static void t_scan_contract(void)
{
    const u64 W = (u64)1 << 22, T = 128, S = 2 * T + 1, lo0 = 1ULL << 30;
    pz_u256 lo = pz_u256_from_u64(lo0), hi = pz_u256_from_u64(lo0 + W - 1);
    bsgs_params p;
    char err[200];
    const u64 c = lo0 + ((W - 1) >> 1);
    pz_u256 key = pz_u256_from_u64(c + 5 * BS_GRP * S + 11);   /* the giant a = 5 * BS_GRP: first giant of the sixth window */
    pz_ge P = pub_of(&key);
    CHECK(bsgs_params_init(&p, &lo, &hi, &P, T, err, sizeof err));
    bsgs_table tab;
    CHECK(bsgs_table_alloc(&tab, T, err, sizeof err));
    CHECK(bsgs_table_build(&tab, 2, NULL, NULL));
    bsgs_job job;
    bsgs_job_init(&job, &tab, &p);
    const int64_t a0 = 0;                                  /* scanning a = 0, 1, ...: the key's giant is a = 5 * BS_GRP */
    /* a range that does not reach the key: nothing found, everything covered, progress counts whole windows */
    {
        hit_rec h = {pz_u256_zero(), 0, NULL, false};
        _Atomic u64 prog = 0, fp = 0;
        u64 n = 5 * BS_GRP;
        CHECK(bsgs_scan(&job, a0, n, on_hit, &h, &prog, &fp, NULL) == n);
        CHECK(h.hits == 0 && atomic_load(&prog) == n);
    }
    /* a ragged length: the last window is whole, the return value is clamped to n */
    {
        hit_rec h = {pz_u256_zero(), 0, NULL, false};
        _Atomic u64 prog = 0;
        u64 n = 5 * BS_GRP + 1;
        CHECK(bsgs_scan(&job, a0, n, on_hit, &h, &prog, NULL, NULL) == n);
        CHECK(h.hits == 1 && !pz_u256_cmp(&h.key, &key));      /* the extra giant belongs to a whole window that contains the key */
        CHECK(atomic_load(&prog) == 6 * BS_GRP);
    }
    /* zero length */
    {
        hit_rec h = {pz_u256_zero(), 0, NULL, false};
        _Atomic u64 prog = 0;
        CHECK(bsgs_scan(&job, a0, 0, on_hit, &h, &prog, NULL, NULL) == 0 && atomic_load(&prog) == 0 && h.hits == 0);
    }
    /* stop requested before the call: nothing is scanned */
    {
        _Atomic int stop = 1;
        hit_rec h = {pz_u256_zero(), 0, &stop, false};
        _Atomic u64 prog = 0;
        CHECK(bsgs_scan(&job, a0, 20 * BS_GRP, on_hit, &h, &prog, NULL, &stop) == 0 && atomic_load(&prog) == 0 && h.hits == 0);
    }
    /* stop requested by the hit callback: the scan ends at the boundary of the batch of windows with the key (one window
     * with the scalar routine, up to 8 with the lane-parallel ones), and reports exactly what it covered */
    {
        _Atomic int stop = 0;
        hit_rec h = {pz_u256_zero(), 0, &stop, true};
        _Atomic u64 prog = 0;
        u64 got = bsgs_scan(&job, a0, 20 * BS_GRP, on_hit, &h, &prog, NULL, &stop);
        CHECK(h.hits == 1 && !pz_u256_cmp(&h.key, &key));
        CHECK(got % BS_GRP == 0 && got >= 6 * BS_GRP && got <= 8 * BS_GRP && atomic_load(&prog) == got);
    }
    /* the same key scanned starting from a negative index and across the centre (a < 0 .. a > 0) */
    {
        hit_rec h = {pz_u256_zero(), 0, NULL, false};
        CHECK(bsgs_scan(&job, -(int64_t)(3 * BS_GRP), 9 * BS_GRP, on_hit, &h, NULL, NULL, NULL) == 9 * BS_GRP);
        CHECK(h.hits == 1 && !pz_u256_cmp(&h.key, &key));
    }
    bsgs_table_free(&tab);
}

/* the portable and the BMI2/ADX group routines must produce bit-identical windows */
static void t_group_variants(void)
{
#if !defined(__x86_64__) || defined(PZ_NO_X86_VARIANTS)
    return;                                    /* no x86 variant is built (arm64, X86=0) */
#else
    if (!cpu_has_avx2()) { puts("  (BMI2/ADX not available: variants not compared)"); return; }
    const u64 W = (u64)1 << 20;
    pz_u256 lo = pz_u256_from_u64(1000), hi = pz_u256_from_u64(1000 + W - 1);
    pz_ge P = pub_of(&(pz_u256){{987654321, 0, 0, 0}});
    bsgs_params p;
    char err[200];
    CHECK(bsgs_params_init(&p, &lo, &hi, &P, 64, err, sizeof err));
    bsgs_table tab;
    CHECK(bsgs_table_alloc(&tab, 64, err, sizeof err));
    bsgs_job job;
    bsgs_job_init(&job, &tab, &p);
    pz_fe *dx = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1)), *tmp = pz_xaligned(64, sizeof(pz_fe) * (BS_HALF + 1));
    u64 *a_lo = pz_xaligned(64, 8 * BS_GRP), *a_hi = pz_xaligned(64, 8 * BS_GRP), *b_lo = pz_xaligned(64, 8 * BS_GRP), *b_hi = pz_xaligned(64, 8 * BS_GRP);
    for (int round = 0; round < 4; round++) {
        pz_ge C1 = pub_of(&(pz_u256){{12345ULL + 7777ULL * (u64)round, (u64)round, 0, 0}}), C2 = C1;
        for (int win = 0; win < 3; win++) {
            bool ok1 = bs_group_base(&job.giant, &C1, a_lo, a_hi, dx, tmp);
            bool ok2 = bs_group_x86(&job.giant, &C2, b_lo, b_hi, dx, tmp);
            CHECK(ok1 && ok2);
            CHECK(memcmp(a_lo, b_lo, 8 * BS_GRP) == 0 && memcmp(a_hi, b_hi, 8 * BS_GRP) == 0);
            pz_fe x1 = C1.x, x2 = C2.x, y1 = C1.y, y2 = C2.y;
            pz_fe_normalize(&x1); pz_fe_normalize(&x2); pz_fe_normalize(&y1); pz_fe_normalize(&y2);
            CHECK(!memcmp(&x1, &x2, sizeof x1) && !memcmp(&y1, &y2, sizeof y1));
        }
    }
    pz_afree(dx); pz_afree(tmp); pz_afree(a_lo); pz_afree(a_hi); pz_afree(b_lo); pz_afree(b_hi);
    bsgs_table_free(&tab);
#endif
}

int main(void)
{
    pz_ec_init();
    t_coverage();
    t_sizing();
    t_table();
    t_solve();
    t_window_edges();
    t_scan_contract();
    t_group_variants();
    puts(fails ? "t_bsgs: FAILED" : "t_bsgs: ok");
    return fails != 0;
}
