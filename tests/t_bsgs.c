/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_bsgs.c - baby-step giant-step: coverage arithmetic, table build/persistence, exhaustive solves with planted keys. */
#include "../src/bsgs.h"
#include "../src/bsgs_priv.h"
#include "../src/cpu_engine.h"
#include "../src/platform.h"
#include <pzcore/hash.h>
#include <math.h>
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
    /* sizes: whole buckets, 8 / 0.70 bytes per baby step, and the two directions agree */
    CHECK(bsgs_bytes_for(1000000000ULL) < 11.5e9 && bsgs_bytes_for(1000000000ULL) > 11.4e9);
    const u64 sizes[] = {1, 2, 3, 7, 64, 100, 999, 4096, 12345, 1000003, 987654321, BS_MAX_T};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        const u64 n = sizes[i];
        CHECK(bsgs_slots_for(n) % BS_BUCKET == 0 && bsgs_slots_for(n) * BS_LOAD_NUM >= n * BS_LOAD_DEN && bsgs_slots_for(n) > n);
        CHECK(bsgs_max_T_for_bytes(bsgs_bytes_for(n)) >= n);                          /* the table of n babies holds n babies... */
        CHECK(bsgs_bytes_for(bsgs_max_T_for_bytes(bsgs_bytes_for(n))) == bsgs_bytes_for(n));   /* ...and no more fit in its RAM */
        const u64 budget = n * 11;                                                        /* any budget: the table fits, one more baby step would not */
        const u64 m = bsgs_max_T_for_bytes(budget);
        CHECK(m == 0 || (bsgs_bytes_for(m) <= budget && (m == BS_MAX_T || bsgs_bytes_for(m + 1) > budget)));
    }
    CHECK(bsgs_max_T_for_bytes(0) == 0 && bsgs_max_T_for_bytes(64 * 8) == 0 && bsgs_max_T_for_bytes(64 * 9) >= 1);
    CHECK(bsgs_max_T_for_bytes(~(u64)0) == BS_MAX_T);
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

typedef struct { pz_u256 key; int hits; _Atomic int *stop; bool stop_on_hit; } hit_rec;
static void on_hit(void *u, const pz_u256 *k) { hit_rec *h = u; h->hits++; h->key = *k; if (h->stop_on_hit) atomic_store(h->stop, 1); }

/* ---- the layout of the table: buckets, overflow, threads, the old format ---- */

static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }

/* every bucket must be filled from its first cell on, and the cells in use must be `want` */
static void check_bucket_order(const bsgs_table *t, u64 want)
{
    u64 used = 0;
    for (u64 i = 0; i < t->nslots; i += BS_BUCKET)
        for (int j = 0, gap = 0; j < BS_BUCKET; j++) {
            if (t->slots[i + j] == 0) gap = 1;
            else { used++; CHECK(!gap); }
        }
    CHECK(used == want);
}

/* A full bucket: the baby goes to the next one (the table wraps around), lookups follow the same chain and stop at the first
 * bucket that still has a free cell. A bounded probe is not needed: a table always has more cells than babies, so such a bucket exists. */
static void t_bucket_overflow(void)
{
    const u64 nb = 16;
    u64 *cells = pz_xaligned(64, nb * BS_BUCKET * 8);
    bsgs_table t;
    memset(&t, 0, sizeof t);
    t.slots = cells;
    t.nslots = nb * BS_BUCKET;
    u64 next_b = 1, tag = 100;
    /* xlo in [h * 2^60, (h + 1) * 2^60) belongs to bucket h of 16 */
#define HOME(h, i) (((u64)(h) << 60) + (i))
    u64 homed5[20], tags5[20];
    for (u64 i = 0; i < 20; i++) {
        tags5[i] = tag++;
        homed5[i] = next_b++;
        CHECK(bs_insert(cells, nb, HOME(5, i), tags5[i], homed5[i]));
    }
    for (u64 i = 0; i < 20; i++)                                   /* 8 in bucket 5, 8 in bucket 6, 4 in bucket 7, in order */
        CHECK(cells[(5 + i / 8) * BS_BUCKET + i % 8] == bs_entry(tags5[i], homed5[i]));
    check_bucket_order(&t, 20);
    for (u64 i = 0; i < 20; i++) {
        u64 b = 0;
        CHECK(bsgs_table_contains(&t, HOME(5, i), tags5[i], &b) && b == homed5[i]);
        CHECK(!bs_insert(cells, nb, HOME(5, i), tags5[i], homed5[i]));          /* the same entry again changes nothing */
    }
    check_bucket_order(&t, 20);
    CHECK(!bsgs_table_contains(&t, HOME(5, 3), 99999, NULL));         /* no such tag: the chain 5, 6, 7 ends at bucket 7, which has room */
    /* a key of bucket 7 reads bucket 7 and stops there (free cells): it does not see the babies of bucket 5 that sit in bucket 6,
     * but it does match one that sits in bucket 7, whatever its own bucket (an equal tag) */
    CHECK(!bsgs_table_contains(&t, HOME(7, 3), tags5[0], NULL));
    u64 b = 0;
    CHECK(bsgs_table_contains(&t, HOME(7, 3), tags5[17], &b) && b == homed5[17]);
    /* wrap-around: ten babies of the last bucket, two end in bucket 0 */
    u64 b15[10], t15[10];
    for (u64 i = 0; i < 10; i++) {
        t15[i] = tag++;
        b15[i] = next_b++;
        CHECK(bs_insert(cells, nb, HOME(15, i), t15[i], b15[i]));
    }
    CHECK(cells[0] == bs_entry(t15[8], b15[8]) && cells[1] == bs_entry(t15[9], b15[9]) && cells[2] == 0);
    for (u64 i = 0; i < 10; i++) CHECK(bsgs_table_contains(&t, HOME(15, i), t15[i], &b) && b == b15[i]);
    check_bucket_order(&t, 30);
    /* fill everything but bucket 9, then put one more baby of a bucket far from it: it lands in bucket 9 after crossing the table */
    for (u64 h = 0; h < nb; h++) {
        if (h == 9) continue;
        int used = 0;
        for (int j = 0; j < BS_BUCKET; j++) used += cells[h * BS_BUCKET + j] != 0;
        for (int j = used; j < BS_BUCKET; j++) CHECK(bs_insert(cells, nb, HOME(h, j + 40), tag++, next_b++));
    }
    int in9 = 0;
    for (int j = 0; j < BS_BUCKET; j++) in9 += cells[9 * BS_BUCKET + j] != 0;
    const u64 used_now = 15 * BS_BUCKET + (u64)in9;
    check_bucket_order(&t, used_now);
    CHECK(!bsgs_table_contains(&t, HOME(10, 5), 123456, NULL));      /* a miss walks 10 .. 15, 0 .. 9 and ends at the only bucket with room */
    CHECK(!bsgs_table_contains(&t, HOME(8, 5), 123456, NULL));
    const u64 tl = tag++, bl = next_b++;
    CHECK(bs_insert(cells, nb, HOME(10, 77), tl, bl));
    CHECK(cells[9 * BS_BUCKET + in9] == bs_entry(tl, bl));
    CHECK(bsgs_table_contains(&t, HOME(10, 77), tl, &b) && b == bl);
    check_bucket_order(&t, used_now + 1);
    pz_afree(cells);
#undef HOME
}

/* random babies in small tables at loads up to 95 %: long chains and wrap-arounds; every baby is found by its own tag and a tag that
 * is not there is never found */
static void t_bucket_random(void)
{
    const u64 loads[] = {50, 70, 85, 95};
    for (size_t li = 0; li < sizeof loads / sizeof *loads; li++)
        for (u64 nb = 3; nb <= 96; nb = nb * 3 + 1) {
            u64 *cells = pz_xaligned(64, nb * BS_BUCKET * 8);
            bsgs_table t;
            memset(&t, 0, sizeof t);
            t.slots = cells;
            t.nslots = nb * BS_BUCKET;
            const u64 n = t.nslots * loads[li] / 100;
            u64 *xs = malloc(n * 8);
            for (u64 i = 0; i < n; i++) {
                xs[i] = rnd();
                CHECK(bs_insert(cells, nb, xs[i], 5 + i, 1 + i));            /* the tag of baby i is 5 + i: all different, none 0 */
            }
            check_bucket_order(&t, n);
            for (u64 i = 0; i < n; i++) {
                u64 b = 0;
                CHECK(bsgs_table_contains(&t, xs[i], 5 + i, &b) && b == 1 + i);
            }
            for (int m = 0; m < 2000; m++) CHECK(!bsgs_table_contains(&t, rnd(), 5 + n + (rnd() & 0xFFFF), NULL));
            free(xs);
            pz_afree(cells);
        }
}

/* the SSE2 and the portable comparison of the 8 tags of a bucket must always agree */
static void t_bucket_compare(void)
{
    u64 *bk = pz_xaligned(64, BS_BUCKET * 8);
    const u64 edge_tags[] = {0, 1, 0xFFFFFF, 0x800000, 0x000100, 0xFFFF00};
    const u64 edge_b[] = {1, 2, BS_VAL_MASK, (u64)1 << 39, (u64)1 << 32, 0xFFFFFFFFULL};
    int yes = 0, no = 0;
    for (int iter = 0; iter < 300000; iter++) {
        int n = (int)(rnd() % (BS_BUCKET + 1));
        for (int j = 0; j < BS_BUCKET; j++) {
            u64 tg = (rnd() & 3) == 0 ? edge_tags[rnd() % 6] : rnd() & BS_TAG_MASK;
            u64 bb = (rnd() & 3) == 0 ? edge_b[rnd() % 6] : 1 + rnd() % BS_VAL_MASK;
            bk[j] = j < n ? (tg << BS_VAL_BITS) | bb : 0;
        }
        u64 tag;
        switch (rnd() % 6) {
        case 0: tag = 0; break;
        case 1: tag = BS_TAG_MASK; break;
        case 2: tag = edge_tags[rnd() % 6]; break;
        case 3: case 4: tag = n ? bk[rnd() % (u64)n] >> BS_VAL_BITS : 5; break;      /* present */
        default: tag = rnd() & BS_TAG_MASK; break;
        }
        bool a = bs_bucket_maybe(bk, tag), c = bs_bucket_maybe_scalar(bk, tag), exact = false;
        for (int j = 0; j < n; j++) exact |= (bk[j] >> BS_VAL_BITS) == tag;
        CHECK(a == c);
        CHECK(!exact || a);                                  /* never a miss for a tag that is in the bucket */
        CHECK(tag == 0 || a == exact);                       /* and exact for any tag but 0 (an empty cell has the tag 0) */
        yes += a; no += !a;
    }
    CHECK(yes > 1000 && no > 1000);
    pz_afree(bk);
}

/* the table built by one thread and by several holds the same babies (where each one sits in its bucket may differ) */
static void t_build_threads(void)
{
    char err[200];
    const u64 T = 20000;
    bsgs_table a, b;
    CHECK(bsgs_table_alloc(&a, T, err, sizeof err) && bsgs_table_alloc(&b, T, err, sizeof err));
    CHECK(a.nslots == bsgs_slots_for(T) && !a.linear && ((uintptr_t)a.slots & 63) == 0);
    CHECK(bsgs_table_build(&a, 1, NULL, NULL));
    CHECK(bsgs_table_build(&b, 4, NULL, NULL));
    CHECK(bsgs_table_count(&a, 3) == T && bsgs_table_count(&b, 3) == T);
    check_bucket_order(&a, T);
    check_bucket_order(&b, T);
    u64 *sa = malloc(T * 8), *sb = malloc(T * 8), n = 0;
    for (u64 i = 0; i < a.nslots; i++) if (a.slots[i]) sa[n++] = a.slots[i];
    CHECK(n == T);
    n = 0;
    for (u64 i = 0; i < b.nslots; i++) if (b.slots[i]) sb[n++] = b.slots[i];
    CHECK(n == T);
    qsort(sa, T, 8, cmp_u64);
    qsort(sb, T, 8, cmp_u64);
    CHECK(memcmp(sa, sb, T * 8) == 0);
    /* every baby step is found in both, with its own index */
    int wrong = 0;
    for (u64 k = 1; k <= T; k++) {
        pz_ge q = pub_of(&(pz_u256){{k, 0, 0, 0}});
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        u64 ga = 0, gb = 0;
        CHECK(bsgs_table_contains(&a, x.n[0], x.n[1], &ga) && bsgs_table_contains(&b, x.n[0], x.n[1], &gb));
        wrong += (ga != k) + (gb != k);                    /* only a tag collision inside a bucket can return another index */
    }
    CHECK(wrong <= 2);
    free(sa); free(sb);
    bsgs_table_free(&a);
    bsgs_table_free(&b);
}

/* ---- the old format (PZBSGS01), written here from its description, and checked against a file written by the version that had it ---- */

static u64 old_slots(u64 T) { u64 n = (u64)ceill((long double)T / (long double)0.65) + 64; return (n + 7) & ~(u64)7; }

static u64 old_mix(u64 v, u64 i)
{
    u64 x = (v + i * 0x9E3779B97F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL;
    x = (x << 31 | x >> 33) * 0x165667B19E3779F9ULL;
    return x ^ (x >> 29);
}

static void put64(unsigned char *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* The file of the first version for T baby steps added in order b = 1..T to a single array of cells (linear probing, load 0.65),
 * as one thread of that version did it; returns the bytes (header and cells) and their number. */
static unsigned char *old_file(u64 T, size_t *len)
{
    const u64 n = old_slots(T);
    unsigned char *f = calloc(1, 4096 + n * 8);
    u64 *cells = calloc(n, 8);
    for (u64 b = 1; b <= T; b++) {
        pz_ge q = pub_of(&(pz_u256){{b, 0, 0, 0}});
        pz_fe x = q.x;
        pz_fe_normalize(&x);
        u64 i = (u64)(((u128)x.n[0] * n) >> 64);
        while (cells[i]) i = i + 1 == n ? 0 : i + 1;
        cells[i] = ((x.n[1] & 0xFFFFFF) << 40) | b;
    }
    u64 sum = 0;
    for (u64 i = 0; i < n; i++) { sum += old_mix(cells[i], i); put64(f + 4096 + i * 8, cells[i]); }
    memcpy(f, "PZBSGS01", 8);
    put64(f + 8, 0x0102030405060708ULL);
    put64(f + 16, 1);
    put64(f + 24, T);
    put64(f + 32, n);
    put64(f + 40, 24);
    put64(f + 48, 40);
    put64(f + 56, T);
    put64(f + 64, sum);
    pz_fe_to_be(f + 72, &pz_ec_g.x);
    pz_ge tg = pub_of(&(pz_u256){{T, 0, 0, 0}});
    pz_fe_to_be(f + 104, &tg.x);
    pz_sha256(f, 4096 - 32, f + 4096 - 32);
    free(cells);
    *len = 4096 + n * 8;
    return f;
}

static bool write_file(const char *path, const unsigned char *d, size_t n)
{
    int fd = pz_open_rw(path, true);
    if (fd < 0) return false;
    bool ok = pz_pwrite(fd, d, n, 0) == (i64)n;
    close(fd);
    return ok;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    int fd = pz_open_read(path);
    if (fd < 0) return NULL;
    i64 sz = pz_fd_size(fd);
    unsigned char *d = malloc((size_t)sz);
    bool ok = pz_pread(fd, d, (size_t)sz, 0) == sz;
    close(fd);
    if (!ok) { free(d); return NULL; }
    *n = (size_t)sz;
    return d;
}

static void t_old_format(void)
{
    char err[200], tdir[600], path[700], path2[700];
    CHECK(pz_make_temp_dir(tdir, sizeof tdir, "pzbsgs1"));
    snprintf(path, sizeof path, "%s/old.bin", tdir);
    snprintf(path2, sizeof path2, "%s/resaved.bin", tdir);

    /* the file for 500 baby steps is, byte for byte, the one that the version with this format wrote with one thread */
    size_t len;
    unsigned char *f = old_file(500, &len), dg[32];
    char hex[65];
    pz_sha256(f, len, dg);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
    CHECK(len == 10816 && strcmp(hex, "ab8979a87569961b08aa4453f421b35442f50e37726d48d69da598a78d2a3d51") == 0);
    free(f);

    const u64 Ts[] = {500, 3000};
    for (size_t ti = 0; ti < 2; ti++) {
        const u64 T = Ts[ti];
        f = old_file(T, &len);
        CHECK(write_file(path, f, len));
        u64 pt = 0, ram = 0;
        CHECK(bsgs_table_peek(path, &pt, err, sizeof err) && pt == T);
        CHECK(bsgs_table_peek_ram(path, &pt, &ram, err, sizeof err) && pt == T && ram == old_slots(T) * 8 && ram > bsgs_bytes_for(T));
        bsgs_table l;
        CHECK(bsgs_table_load(&l, path, 3, err, sizeof err));
        CHECK(l.linear && l.T == T && l.nslots == old_slots(T));
        /* every baby step is there */
        int wrong = 0;
        for (u64 b = 1; b <= T; b++) {
            pz_ge q = pub_of(&(pz_u256){{b, 0, 0, 0}});
            pz_fe x = q.x;
            pz_fe_normalize(&x);
            u64 got = 0;
            CHECK(bsgs_table_contains(&l, x.n[0], x.n[1], &got));
            wrong += got != b;
        }
        CHECK(wrong <= 1);
        /* a search with it finds the key, like one with a table of the new layout */
        {
            const u64 lo0 = 1ULL << 22, W = 1ULL << 20, key0 = lo0 + 777777;
            pz_u256 lo = pz_u256_from_u64(lo0), hi = pz_u256_from_u64(lo0 + W - 1), key = pz_u256_from_u64(key0);
            pz_ge P = pub_of(&key);
            bsgs_params p;
            CHECK(bsgs_params_init(&p, &lo, &hi, &P, T, err, sizeof err));
            bsgs_job job;
            bsgs_job_init(&job, &l, &p);
            hit_rec h = {pz_u256_zero(), 0, NULL, false};
            CHECK(bsgs_scan(&job, -(int64_t)p.A, p.Ng, on_hit, &h, NULL, NULL, NULL) == p.Ng);
            CHECK(h.hits == 1 && !pz_u256_cmp(&h.key, &key));
        }
        /* saved again, it is the same file: the old format stays the old format */
        CHECK(bsgs_table_save(&l, path2, 2, err, sizeof err));
        size_t len2;
        unsigned char *g = read_file(path2, &len2);
        CHECK(g && len2 == len && memcmp(g, f, len) == 0);
        free(g);
        bsgs_table_free(&l);
        /* damage is found as in the new format */
        unsigned char *bad = malloc(len);
        memcpy(bad, f, len);
        bad[4096 + (len - 4096) / 2] ^= 0x10;
        CHECK(write_file(path, bad, len));
        CHECK(!bsgs_table_load(&l, path, 2, err, sizeof err));
        memcpy(bad, f, len);
        bad[16] ^= 1;                                                    /* the version, in the header */
        CHECK(write_file(path, bad, len));
        CHECK(!bsgs_table_peek(path, &pt, err, sizeof err) && !bsgs_table_load(&l, path, 2, err, sizeof err));
        free(bad);
        free(f);
    }
    /* a table of the new layout is not read as the old one and the other way round: the magic decides */
    {
        bsgs_table t;
        CHECK(bsgs_table_alloc(&t, 300, err, sizeof err) && bsgs_table_build(&t, 2, NULL, NULL));
        CHECK(bsgs_table_save(&t, path, 2, err, sizeof err));
        size_t n = 0;
        unsigned char *d = read_file(path, &n);
        CHECK(d && n == 4096 + t.nslots * 8 && memcmp(d, "PZBSGS02", 8) == 0);
        u64 pt, ram;
        CHECK(bsgs_table_peek_ram(path, &pt, &ram, err, sizeof err) && pt == 300 && ram == bsgs_bytes_for(300));
        memcpy(d, "PZBSGS01", 8);                                       /* the checksum of the header no longer fits */
        CHECK(write_file(path, d, n));
        CHECK(!bsgs_table_peek(path, &pt, err, sizeof err));
        memcpy(d, "PZBSGS03", 8);
        CHECK(write_file(path, d, n));
        CHECK(!bsgs_table_peek(path, &pt, err, sizeof err) && strstr(err, "not supported"));
        free(d);
        bsgs_table_free(&t);
    }
    CHECK(pz_remove_tree(tdir) == 0);
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
    t_bucket_overflow();
    t_bucket_random();
    t_bucket_compare();
    t_build_threads();
    t_old_format();
    t_solve();
    t_window_edges();
    t_scan_contract();
    t_group_variants();
    puts(fails ? "t_bsgs: FAILED" : "t_bsgs: ok");
    return fails != 0;
}
