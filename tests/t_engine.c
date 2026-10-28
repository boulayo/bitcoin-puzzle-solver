/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_engine.c - verifies every CPU engine variant against the scalar reference.
 *   t_engine [ENGINE...]   only the named engines (a name that is unknown or not supported by this CPU is an error)
 * With PZ_EXPECT_SHANI=1 in the environment the test fails unless the "shani" engine (the SHA extensions of x86-64) is supported and was
 * tested, so that a run on an emulated CPU (make test-shani) cannot pass by skipping it. */
#include "../src/cpu_engine.h"
#include <pzcore/hash.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int hits; pz_u256 key; } hitrec;
static void on_hit(void *u, const pz_u256 *k) { hitrec *h = u; h->hits++; h->key = *k; }
typedef struct { hitrec h; _Atomic int *stop; } stoprec;
static void on_hit_stop(void *u, const pz_u256 *k) { stoprec *x = u; x->h.hits++; x->h.key = *k; atomic_store(x->stop, 1); }   /* file scope: clang has no nested functions */

static u64 s = 0x1234567887654321ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static void h160_of_key(const pz_u256 *k, u8 h[20])
{
    pz_ge p; u8 pub[33];
    CHECK(pz_ec_mul_g(&p, k));
    pz_ge_to_pub33(pub, &p);
    pz_hash160(pub, 33, h);
}

static void test_engine(const cpu_engine *e)
{
    printf("  engine %-9s (%d lanes, step of %llu keys)\n", e->name, e->lanes, (unsigned long long)e->granule);
    const int L = e->lanes;
    const u64 G = e->granule;
    /* 1. SIMD hash vs scalar */
    u32 X[8 * 64], pfx[64], out[5 * 64];
    for (int round = 0; round < 400 + 256; round++) {
        u8 xb[64][32];
        for (int l = 0; l < L; l++) {
            for (int i = 0; i < 32; i++) xb[l][i] = round == 0 ? 0 : round == 1 ? 0xff : round == 2 ? (u8)(l * 31 + i) : (u8)rnd();   /* rounds 0-2: edge patterns */
            if (round >= 400) {                       /* rounds 400-655: a single bit set, at a position that walks over the lanes and the rounds */
                memset(xb[l], 0, 32);
                xb[l][((round - 400 + l) % 256) / 8] = (u8)(0x80u >> (((round - 400 + l) % 256) % 8));
            }
            pfx[l] = 2 + (rnd() & 1);
            for (int i = 0; i < 8; i++) X[i * L + l] = (u32)xb[l][4*i] << 24 | (u32)xb[l][4*i+1] << 16 | (u32)xb[l][4*i+2] << 8 | xb[l][4*i+3];
        }
        e->hash160_lanes(X, pfx, out);
        for (int l = 0; l < L; l++) {
            u8 h[20], got[20];
            pz_hash160_pub33((u8)pfx[l], xb[l], h);
            for (int w = 0; w < 5; w++) for (int b = 0; b < 4; b++) got[4*w+b] = (u8)(out[w * L + l] >> (8*b));
            CHECK(!memcmp(h, got, 20));
        }
    }
    /* 2. scans: every key offset of the first two groups, different starting points */
    pz_u256 starts[6];
    starts[0] = pz_u256_from_u64(1);                 /* smallest legal start */
    starts[1] = pz_u256_from_u64(512);
    starts[2] = pz_u256_from_u64(100000);
    starts[3] = pz_u256_pow2(70); pz_u256_add_u64(&starts[3], &starts[3], 12345);
    starts[4] = pz_u256_pow2(130); pz_u256_add_u64(&starts[4], &starts[4], 777);
    starts[5] = pz_u256_from_u64(G - 256);          /* the centre of the first window equals the step point: zero denominator, slow path */
    for (int si = 0; si < 6; si++) {
        const u64 nkeys = 3 * G;
        for (u64 off = (si < 2 || si == 5 ? 0 : 3); off < nkeys; off += (si < 2 || si == 5 ? 61 : 331)) {
            pz_u256 key = starts[si];
            pz_u256_add_u64(&key, &key, off);
            u8 h[20];
            h160_of_key(&key, h);
            scan_job job; scan_job_init(&job, h);
            hitrec r = {0, pz_u256_zero()};
            job.on_hit = on_hit; job.user = &r;
            _Atomic u64 prog = 0; job.progress = &prog;
            u64 cov = e->scan(&job, &starts[si], nkeys);
            CHECK(cov == nkeys);
            CHECK(prog >= nkeys && prog < nkeys + G);
            CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
            if (fails > 20) return;
        }
    }
    /* 2b. group/lane boundaries, hit at every position of the first lane batch and of the group edges */
    {
        pz_u256 st = pz_u256_from_u64(300000);
        const u64 edges[] = {0, 1, 2, (u64)L - 1, (u64)L, (u64)L + 1, CPU_HALF - 1, CPU_HALF, CPU_HALF + 1, CPU_GRP - 2, CPU_GRP - 1, CPU_GRP, CPU_GRP + 1,
                             2 * CPU_GRP - 1, 3 * CPU_GRP - 1, G - 1, G, G + 1, 2 * G - 1, 3 * G - 1};
        for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) {
            pz_u256 key = st; pz_u256_add_u64(&key, &key, edges[i]);
            u8 h[20]; h160_of_key(&key, h);
            scan_job job; scan_job_init(&job, h);
            hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
            CHECK(e->scan(&job, &st, 3 * G) == 3 * G);
            CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
        }
    }
    /* 2c. every window of a round (a lane of the vector engines): edges, centre and the quarter points of each window */
    {
        pz_u256 st = pz_u256_from_u64(400000);
        const u64 nw = G / CPU_GRP;
        for (u64 w = 0; w < nw; w++) {
            const u64 offs[] = {0, 1, 255, 256, 257, 510, 511};
            for (size_t i = 0; i < sizeof offs / sizeof *offs; i++) {
                pz_u256 key = st; pz_u256_add_u64(&key, &key, w * CPU_GRP + offs[i]);
                u8 h[20]; h160_of_key(&key, h);
                scan_job job; scan_job_init(&job, h);
                hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
                CHECK(e->scan(&job, &st, 2 * G) == 2 * G);
                CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
            }
        }
    }
    /* 2d. zero denominators in every window: the centre of window w equals the step point (start = G - 256 - 512 w) */
    {
        const u64 nw = G / CPU_GRP;
        for (u64 w = 0; w < nw; w++) {
            pz_u256 st = pz_u256_from_u64(G - 256 - w * CPU_GRP);
            for (u64 off = 0; off < 2 * G; off += 97 + w) {
                pz_u256 key = st; pz_u256_add_u64(&key, &key, off);
                u8 h[20]; h160_of_key(&key, h);
                scan_job job; scan_job_init(&job, h);
                hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
                CHECK(e->scan(&job, &st, 2 * G) == 2 * G);
                CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
            }
        }
    }
    /* 2e. ranges ending at the group order: a key that is 0 mod n has no point and a window centre can be the point at
     * infinity; neither may crash or corrupt the batch (this used to overrun the hash batch in the vector engines) */
    {
        pz_u256 st;
        pz_u256 back = pz_u256_from_u64(300);
        pz_u256_sub(&st, &pz_secp256k1_n, &back);
        for (u64 off = 0; off < 300; off += 13) {
            pz_u256 key = st; pz_u256_add_u64(&key, &key, off);
            u8 h[20]; h160_of_key(&key, h);
            scan_job job; scan_job_init(&job, h);
            hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
            CHECK(e->scan(&job, &st, 3 * G) == 3 * G);
            CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
        }
        /* a window centre equal to -S*G: start = n - 2G + ... so that some centre is exactly n */
        for (u64 delta = 0; delta < 3; delta++) {
            pz_u256 back2 = pz_u256_from_u64(2 * G + 256 + delta * CPU_GRP);
            pz_u256_sub(&st, &pz_secp256k1_n, &back2);
            pz_u256 key = pz_secp256k1_n;
            pz_u256 one = pz_u256_from_u64(1 + 5 * delta);
            pz_u256_sub(&key, &key, &one);              /* a valid key just below n */
            u8 h[20]; h160_of_key(&key, h);
            scan_job job; scan_job_init(&job, h);
            hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
            const u64 span = (2 * G + 256 + delta * CPU_GRP + 2 * G + G - 1) / G * G;   /* reaches past n */
            CHECK(e->scan(&job, &st, span) == span);
            CHECK(r.hits == 1 && !pz_u256_cmp(&r.key, &key));
        }
    }
    /* 2f. degenerate sizes */
    {
        pz_u256 st = pz_u256_from_u64(600000);
        u8 h[20]; h160_of_key(&st, h);
        scan_job job; scan_job_init(&job, h);
        hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
        CHECK(e->scan(&job, &st, 0) == 0 && r.hits == 0);
        CHECK(e->scan(&job, &st, 1) == 1 && r.hits == 1);
    }
    /* 2g. a stop requested from the hit callback in the middle of a long scan: the covered keys form a prefix that
     * contains the key, is a whole number of steps and is shorter than the claim */
    {
        pz_u256 st = pz_u256_from_u64(900000);
        pz_u256 key = st; pz_u256_add_u64(&key, &key, 9 * G + 5);
        u8 h[20]; h160_of_key(&key, h);
        scan_job job; scan_job_init(&job, h);
        _Atomic int stop = 0;
        stoprec hr = {{0, pz_u256_zero()}, &stop};
        job.on_hit = on_hit_stop; job.user = &hr; job.stop = &stop;
        _Atomic u64 prog = 0; job.progress = &prog;
        u64 cov = e->scan(&job, &st, 64 * G);
        CHECK(hr.h.hits == 1 && !pz_u256_cmp(&hr.h.key, &key));
        CHECK(cov > 9 * G + 5 && cov < 64 * G && cov % G == 0);
        CHECK(prog >= cov - G && prog <= cov + 8 * G);
    }
    /* 3. a key just outside the scanned range is not reported */
    {
        pz_u256 st = pz_u256_from_u64(77777), key = st;
        pz_u256_add_u64(&key, &key, 3 * G);
        u8 h[20]; h160_of_key(&key, h);
        scan_job job; scan_job_init(&job, h);
        hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
        e->scan(&job, &st, 3 * G);
        CHECK(r.hits == 0);
        e->scan(&job, &st, 3 * G + 1);   /* rounds up to a full extra step */
        CHECK(r.hits == 1);
    }
    /* 4. stop flag is honoured */
    {
        pz_u256 st = pz_u256_from_u64(1000000);
        u8 h[20] = {1}; scan_job job; scan_job_init(&job, h);
        _Atomic int stop = 1; job.stop = &stop;
        CHECK(e->scan(&job, &st, 1u << 24) == 0);
    }
}

int main(int argc, char **argv)
{
    cpu_tables_init();
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    int tested = 0;
    bool shani_tested = false;
    for (int a = 1; a < argc; a++) {
        const cpu_engine *e = cpu_engine_by_name(argv[a]);
        if (!e || !e->supported()) { printf("t_engine: engine %s is unknown or not supported by this CPU\n", argv[a]); fails++; }
    }
    for (int i = 0; i < n; i++) {
        if (argc > 1) {
            bool wanted = false;
            for (int a = 1; a < argc; a++) wanted |= !strcmp(argv[a], all[i]->name);
            if (!wanted) continue;
        }
        if (!all[i]->supported()) { printf("  engine %-9s not supported by this CPU, skipped\n", all[i]->name); continue; }
        test_engine(all[i]);
        tested++;
        shani_tested |= !strcmp(all[i]->name, "shani");
    }
    const char *expect = getenv("PZ_EXPECT_SHANI");
    if (expect && !strcmp(expect, "1") && !shani_tested) { printf("t_engine: PZ_EXPECT_SHANI=1 but the shani engine was not tested\n"); fails++; }
    CHECK(tested >= 1);
    /* reference scan agrees on a tiny range */
    {
        pz_u256 st = pz_u256_from_u64(40); pz_u256 key = pz_u256_from_u64(53);
        u8 h[20]; h160_of_key(&key, h);
        scan_job job; scan_job_init(&job, h);
        hitrec r = {0, pz_u256_zero()}; job.on_hit = on_hit; job.user = &r;
        cpu_scan_reference(&job, &st, 30);
        CHECK(r.hits == 1 && r.key.w[0] == 53);
    }
    printf(fails ? "t_engine: %d FAILED\n" : "t_engine: OK\n", fails);
    return fails != 0;
}
