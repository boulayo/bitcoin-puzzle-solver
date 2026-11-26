/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_canary.c - the canary of the silent misses of the linear search (canary.h), with the scheduler it gives keys back to. A correct worker over
 * a long run is never condemned; one that misses all of its keys, half of them or one lane of eight is, and its keys are scanned again: those
 * of its window, or everything of the session (back to what the checkpoint held). The workers are models, as no engine scans the 2^38
 * keys of a window in a test: the near misses of a claim of K keys are a Poisson draw of mean K / 2^32, which is what a correct engine
 * finds (every key is one with probability 2^-32), and a draw of a smaller mean for an engine that drops keys. */
#include "../src/canary.h"
#include "../src/envopts.h"
#include "../src/platform.h"
#include <math.h>
#ifndef PZ_TEST_DIR
#define PZ_TEST_DIR "build"
#endif
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u64 rs = 0x243F6A8885A308D3ULL;
static double unif(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) * 0x1p-53; }
static u64 poisson(double mean)
{
    const double l = exp(-mean);
    u64 k = 0;
    for (double p = unif(); p > l; p *= unif()) k++;
    return k;
}

static campaign_cfg mkcfg(u64 start, u64 end, unsigned cb)
{
    campaign_cfg c;
    memset(&c, 0, sizeof c);
    c.range_start = pz_u256_from_u64(start);
    c.range_end = pz_u256_from_u64(end);
    c.chunk_bits = cb;
    for (int i = 0; i < 32; i++) c.seed[i] = (u8)(i * 29 + 3);
    return c;
}

/* Claims of `want` keys scanned by a worker that finds the share `keep` of its near misses, until it is condemned or `keys` keys are done.
 * The keys it covered, and in *bad the keys at which it was condemned (0: never). */
static u64 work(canary *cy, sched_t *s, u64 want, double keep, u64 keys, u64 *bad)
{
    u64 done = 0;
    *bad = 0;
    while (done < keys) {
        claim_t c;
        if (sched_claim(s, want, &c) != CLAIM_OK) break;
        const bool ok = canary_complete(cy, s, &c, c.nkeys, poisson(ldexp((double)c.nkeys, -32) * keep));
        done += c.nkeys;
        if (!ok) { *bad = done; break; }
    }
    return done;
}

/* the bound is an upper bound of the true tail, equal to it at 0, and condemns what it must */
static void t_bound(void)
{
    for (double lambda = 1; lambda <= 4096; lambda *= 2)
        for (u64 n = 0; n < (u64)lambda; n += 1 + (u64)(lambda / 64)) {
            double s = 0;                                       /* the exact tail, in logarithms */
            for (u64 k = 0; k <= n; k++) s += exp(-lambda + (double)k * log(lambda) - lgamma((double)k + 1));
            CHECK(canary_log_tail(lambda, n) >= log(s) - 1e-9);
        }
    CHECK(canary_log_tail(64, 0) == -64 && canary_log_tail(64, 64) == 0 && canary_log_tail(64, 100) == 0);
    CHECK(canary_log_tail(64, 19) < CANARY_LOG_P && canary_log_tail(64, 21) > CANARY_LOG_P);   /* a window: 64 expected, 19 found condemn */
}

/* a correct worker, 2^49 keys four times over (8192 windows): never condemned, and everything it did stays done */
static void t_correct(void)
{
    for (int run = 0; run < 4; run++) {
        campaign_cfg cfg = mkcfg(1, (u64)1 << 60, 40);
        sched_t *s = sched_new(&cfg);
        canary cy;
        canary_init(&cy, "a correct worker");
        u64 bad;
        const u64 done = work(&cy, s, (u64)1 << 30, 1.0, (u64)1 << 49, &bad);
        CHECK(!bad && done == (u64)1 << 49 && sched_keys_done(s) == done && cy.keys == done);
        canary_free(&cy);
        sched_free(s);
    }
}

/* Broken workers, each from a fresh campaign: one that drops every key after 64 good windows (its last window goes back, the earlier
 * ones stay), one that drops half of them and one that drops a lane of eight (the session goes back: their earlier windows passed).
 * The keys given back are counted: the keys the workers counted, less those, are what is marked as scanned (the progress), and until the
 * state is written again they are known as not on disk yet, which is what makes the reporter save at once. */
static void t_broken(void)
{
    char path[600], bak[620], err[200];
    snprintf(path, sizeof path, "%s/t_canary_back.state", PZ_TEST_DIR);
    snprintf(bak, sizeof bak, "%s.bak", path);
    const struct { double keep; u64 good, within; bool session; } cases[] = {
        {0.0, (u64)64 << 38, (u64)65 << 38, false},
        {0.5, 0, (u64)8 << 38, true},
        {7.0 / 8, 0, (u64)300 << 38, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        campaign_cfg cfg = mkcfg(1, (u64)1 << 60, 40);
        sched_t *s = sched_new(&cfg);
        canary cy;
        canary_init(&cy, "a broken worker");
        u64 bad;
        const u64 good = work(&cy, s, (u64)1 << 30, 1.0, cases[i].good, &bad);
        bool unsaved = true;
        CHECK(!bad && good == cases[i].good && sched_given_back(s, &unsaved) == 0 && !unsaved);
        work(&cy, s, (u64)1 << 30, cases[i].keep, cases[i].within, &bad);
        CHECK(bad > 0 && good + bad <= cases[i].within);
        /* what goes back: its last window (the total loss: the session before it is plausible) or, as a rule for a partial loss, the session */
        const u64 kd = sched_keys_done(s), window = good + bad - ((u64)1 << 38);
        CHECK(cases[i].session ? kd == 0 || kd == window : kd == window);
        CHECK(good + bad - sched_given_back(s, &unsaved) == kd && unsaved);
        CHECK(sched_save(s, path) == 0 && sched_given_back(s, &unsaved) == good + bad - kd && !unsaved);
        sched_t *l = sched_new(&cfg);                                     /* the state on disk has the hole */
        CHECK(sched_load(l, path, err, sizeof err) == 0 && sched_keys_done(l) == kd);
        sched_free(l);
        CHECK(sched_save(s, path) == 0);                                  /* written twice, as the checkpoint does: its copy has it too */
        l = sched_new(&cfg);
        CHECK(sched_load(l, bak, err, sizeof err) == 0 && sched_keys_done(l) == kd);
        sched_free(l);
        claim_t c;                                                        /* a claim it completes later is given back too */
        CHECK(sched_claim(s, (u64)1 << 30, &c) == CLAIM_OK && !canary_complete(&cy, s, &c, c.nkeys, 99) && sched_keys_done(s) == kd);
        canary_free(&cy);
        sched_free(s);
    }
    remove(path);
    remove(bak);
}

/* A whole range of 66 windows: a worker that drops every key in its 65th window is condemned, a correct one scans the rest; every unit of
 * 2^30 keys is scanned once, those of the lost window twice, and the range ends complete. Then a resumed session and a worker that drops a
 * quarter of its keys: what goes back is what this session added, not what the checkpoint held. */
static void t_range(void)
{
    const u64 unit = (u64)1 << 30, units = 66 * 256;
    campaign_cfg cfg = mkcfg(1, units * unit, 34);
    sched_t *s = sched_new(&cfg);
    u8 *seen = pz_xcalloc(units, 1);
    canary bad_cy, good_cy;
    canary_init(&bad_cy, "the worker that stops seeing");
    canary_init(&good_cy, "a correct worker");
    int nbad = 0;
    for (int phase = 0; phase < 2; phase++) {
        canary *cy = phase ? &good_cy : &bad_cy;
        for (;;) {
            claim_t c;
            const claim_status st = sched_claim(s, unit, &c);
            if (st != CLAIM_OK) { CHECK(st == CLAIM_DONE); break; }
            CHECK(c.nkeys == unit && (c.v_start & (unit - 1)) == 0 && (u64)(c.v_start >> 30) < units);
            seen[(size_t)(c.v_start >> 30)]++;
            const bool blind = !phase && bad_cy.keys >= ((u64)64 << 38);
            if (!canary_complete(cy, s, &c, c.nkeys, poisson(blind ? 0 : 0.25))) { CHECK(!phase); nbad++; break; }
        }
    }
    CHECK(nbad == 1 && sched_finished(s) && sched_keys_done(s) == units * unit && good_cy.keys == 2 * ((u64)1 << 38));
    u64 twice = 0, other = 0;
    for (u64 u = 0; u < units; u++) { twice += seen[u] == 2; other += seen[u] != 1 && seen[u] != 2; }
    CHECK(twice == 256 && other == 0);
    free(seen);
    canary_free(&bad_cy);
    canary_free(&good_cy);
    sched_free(s);

    char path[600], bak[620];
    snprintf(path, sizeof path, "%s/t_canary.state", PZ_TEST_DIR);
    snprintf(bak, sizeof bak, "%s.bak", path);
    cfg = mkcfg(1, (u64)1 << 60, 40);
    s = sched_new(&cfg);
    canary cy;
    canary_init(&cy, "a correct worker");
    u64 bad;
    const u64 before = work(&cy, s, unit, 1.0, (u64)1 << 39, &bad);
    CHECK(!bad && sched_save(s, path) == 0);
    canary_free(&cy);
    sched_free(s);
    s = sched_new(&cfg);
    char err[200];
    CHECK(sched_load(s, path, err, sizeof err) == 0 && sched_keys_done(s) == before);
    canary_init(&cy, "a worker that drops a quarter of its keys");
    work(&cy, s, unit, 0.75, (u64)64 << 38, &bad);
    CHECK(bad > 0 && sched_keys_done(s) == before && sched_given_back(s, NULL) == bad);
    canary_free(&cy);
    sched_free(s);
    remove(path);
    remove(bak);
}

int main(void)
{
    pz_env_init();
    pz_log_set_quiet(2);                                      /* the condemnations are expected: their lines would only be noise */
    t_bound();
    t_correct();
    t_broken();
    t_range();
    printf(fails ? "t_canary: %d FAILED\n" : "t_canary: OK\n", fails);
    return fails != 0;
}
