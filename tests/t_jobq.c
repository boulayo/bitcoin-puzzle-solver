/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../src/jobq.h"
#include "../src/platform.h"
#include <pzcore/hash.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static campaign_cfg mkcfg(u64 start, u64 end, unsigned cb)
{
    campaign_cfg c; memset(&c, 0, sizeof c);
    c.range_start = pz_u256_from_u64(start); c.range_end = pz_u256_from_u64(end); c.chunk_bits = cb;
    for (int i = 0; i < 32; i++) c.seed[i] = (u8)(i * 13 + 5);
    return c;
}


/* ---- several computers on one search order (--part) ---- */

/* Scans every claim of the part `spec` of the range, marking the keys in cov (which must still be free). Returns the keys scanned. */
static u64 scan_part(const campaign_cfg *base, const char *spec, u8 *cov, u64 start, u64 want, sched_t **keep)
{
    campaign_cfg c = *base;
    snprintf(c.part, sizeof c.part, "%s", spec);
    sched_t *s = sched_new(&c);
    claim_t cl;
    u64 got = 0;
    for (;;) {
        claim_status st = sched_claim(s, want, &cl);
        if (st == CLAIM_DONE) break;
        if (st == CLAIM_WAIT) { CHECK(0); break; }
        u64 k0 = cl.key_start.w[0] - start;
        for (u64 i = 0; i < cl.nkeys; i++) { CHECK(cov[k0 + i] == 0); cov[k0 + i] = 1; }
        got += cl.nkeys;
        sched_complete(s, &cl, cl.nkeys);
    }
    CHECK(sched_finished(s));
    CHECK(sched_keys_done(s) == got);
    CHECK(pz_u256_to_ld(&s->part_keys) == (long double)got);          /* the part's size is exact, also with a short last block */
    CHECK(sched_fraction(s) == 1.0);
    if (keep) *keep = s; else sched_free(s);
    return got;
}

/* ---- the seed is part of a checkpoint ---- */

/* Rewrites a checkpoint as an earlier version wrote it: without the seed_id line (the checksum is computed again). */
static void make_legacy_state(const char *from, const char *to)
{
    char *t = pz_read_file(from, NULL);
    CHECK(t != NULL);
    char *sid = strstr(t, "seed_id=");
    char *crc = strstr(t, "crc=");
    CHECK(sid && crc && sid < crc);
    char *eol = strchr(sid, '\n');
    char out[8192];
    size_t n = (size_t)(sid - t);
    memcpy(out, t, n);
    memcpy(out + n, eol + 1, (size_t)(crc - (eol + 1)));
    n += (size_t)(crc - (eol + 1));
    u8 dg[32];
    char hx[17];
    pz_sha256((u8 *)out, n, dg);
    pz_hex_encode(dg, 8, hx);
    n += (size_t)snprintf(out + n, sizeof out - n, "crc=%s\n", hx);
    CHECK(pz_write_file_atomic(to, out, n, 0644) == 0);
    free(t);
}

static void t_seed(void)
{
    char err[300];
    campaign_cfg c = mkcfg(5000, 5000 + (1 << 20) - 1, 12);
    sched_t *a = sched_new(&c);
    claim_t cl;
    CHECK(sched_claim(a, 4096, &cl) == CLAIM_OK);
    sched_complete(a, &cl, cl.nkeys);
    CHECK(sched_save(a, "build/t_jobq_seed.state") == 0);
    char *t = pz_read_file("build/t_jobq_seed.state", NULL);
    CHECK(t && strstr(t, "\nseed_id=") && strstr(t, a->seed_id));
    free(t);
    /* the same seed resumes */
    sched_t *same = sched_new(&c);
    CHECK(sched_load(same, "build/t_jobq_seed.state", err, sizeof err) == 0 && sched_keys_done(same) == 4096);
    /* another seed is refused, and nothing is loaded: the positions of the file point at other blocks */
    campaign_cfg o = c;
    o.seed[7] ^= 1;
    sched_t *other = sched_new(&o);
    CHECK(strcmp(other->seed_id, a->seed_id) != 0);
    CHECK(sched_load(other, "build/t_jobq_seed.state", err, sizeof err) == -2 && strstr(err, "another seed"));
    CHECK(sched_keys_done(other) == 0 && other->ndone == 0);
    /* a checkpoint of an earlier version carries no seed: it is accepted, whatever the seed, and the next save records it */
    make_legacy_state("build/t_jobq_seed.state", "build/t_jobq_seed_old.state");
    t = pz_read_file("build/t_jobq_seed_old.state", NULL);
    CHECK(t && !strstr(t, "seed_id") && !strncmp(t, "PZSCHED 1\ncbits=12\nv_limit=", 27));
    free(t);
    sched_t *old = sched_new(&c);
    CHECK(sched_load(old, "build/t_jobq_seed_old.state", err, sizeof err) == 0 && sched_keys_done(old) == 4096);
    CHECK(sched_save(old, "build/t_jobq_seed_old.state") == 0);
    t = pz_read_file("build/t_jobq_seed_old.state", NULL);
    CHECK(t && strstr(t, old->seed_id));
    free(t);
    /* a checkpoint cut or altered in the seed line fails the checksum like any other damage */
    FILE *f = fopen("build/t_jobq_seed.state", "r+b");
    CHECK(f != NULL);
    if (f) { fseek(f, 40, SEEK_SET); int ch = fgetc(f); fseek(f, 40, SEEK_SET); fputc(ch == 'a' ? 'b' : 'a', f); fclose(f); }
    sched_t *bad = sched_new(&c);
    CHECK(sched_load(bad, "build/t_jobq_seed.state", err, sizeof err) == -1);
    sched_free(a); sched_free(same); sched_free(other); sched_free(old); sched_free(bad);
}

static void t_parts(void)
{
    char err[300];
    part_spec p;
    /* the syntax */
    CHECK(part_parse("", &p, err, sizeof err) && p.total == 0);
    CHECK(part_parse("0/1", &p, err, sizeof err) && p.total == 0);                        /* the whole order */
    CHECK(part_parse("0-3/4", &p, err, sizeof err) && p.total == 0);                      /* all the shares: the same */
    CHECK(part_parse("2/5", &p, err, sizeof err) && p.total == 5 && p.count == 1 && p.res[0] == 2);
    CHECK(part_parse("5,0-2,4/8", &p, err, sizeof err) && p.total == 8 && p.count == 5 && p.res[0] == 0 && p.res[3] == 4 && p.res[4] == 5);
    char txt[PART_TEXT];
    part_format(&p, txt, sizeof txt);
    CHECK(!strcmp(txt, "0-2,4-5/8"));                                                     /* canonical: ascending, runs joined */
    CHECK(part_parse(txt, &p, err, sizeof err) && p.count == 5);
    const char *bad[] = {"3", "/4", "1/", "4/4", "0,0/4", "1-0/4", "0-4/4", "a/4", "1/0", "1/257", "0,/4", ",0/4", "0--1/4", "-1/4", "1-/4", "0/4/5", "1 /4", "99999999999/4", "0,,1/4"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!part_parse(bad[i], &p, err, sizeof err));
    CHECK(part_parse("0-255/256", &p, err, sizeof err) && p.total == 0);
    CHECK(part_parse("0,2,4/256", &p, err, sizeof err) && p.total == 256 && p.count == 3);

    /* proportional shares: they add up, nobody is left out, the error is under one share */
    {
        const u64 sp[][4] = {{800, 100, 0, 0}, {1, 1, 1, 0}, {5000000000ULL, 5000000000ULL, 1000, 0}, {3, 7, 11, 19}, {1, 1000000, 1, 1}};
        const unsigned nn[] = {2, 3, 3, 4, 4};
        for (int t = 0; t < 5; t++) {
            unsigned cnt[4], sum = 0;
            CHECK(part_split(sp[t], nn[t], 100, cnt));
            u128 w = 0;
            for (unsigned i = 0; i < nn[t]; i++) w += sp[t][i];
            unsigned forced = 0;                                  /* computers whose proportional share is under one: given one anyway */
            for (unsigned i = 0; i < nn[t]; i++) forced += 100.0L * (long double)sp[t][i] / (long double)w < 1.0L;
            for (unsigned i = 0; i < nn[t]; i++) {
                sum += cnt[i];
                CHECK(cnt[i] >= 1);
                const long double ideal = 100.0L * (long double)sp[t][i] / (long double)w, slack = 1.0L + forced + 1e-9L;
                CHECK(cnt[i] - ideal < slack && ideal - cnt[i] < slack);
            }
            CHECK(sum == 100);
        }
        unsigned c2[2];
        CHECK(part_split(sp[0], 2, 100, c2) && c2[0] == 89 && c2[1] == 11);                  /* 88.9 and 11.1 */
        const u64 zero[2] = {5, 0};
        CHECK(!part_split(zero, 2, 100, c2) && !part_split(sp[0], 2, 1, c2));
    }

    /* the shares of a range tile it exactly: every key once, short last block included */
    for (int variant = 0; variant < 3; variant++) {
        const u64 start = 1000 + (u64)variant * 77, total = 300000 + (u64)variant * 1234;
        campaign_cfg c = mkcfg(start, start + total - 1, 13);
        const char *sets[3][4] = {{"0/3", "1/3", "2/3", NULL}, {"0,2/3", "1/3", NULL, NULL}, {"0-1/5", "2/5", "3-4/5", NULL}};
        for (int k = 0; k < 3; k++) {
            u8 *cov = calloc(total, 1);
            u64 sum = 0;
            for (int j = 0; sets[(k + variant) % 3][j]; j++) sum += scan_part(&c, sets[(k + variant) % 3][j], cov, start, variant == 1 ? 1024 : 50000, NULL);
            CHECK(sum == total);
            for (u64 i = 0; i < total; i++) if (!cov[i]) { CHECK(0); break; }
            free(cov);
        }
    }

    /* positions: the blocks of a part are exactly those whose order position has the right remainder, and map back one to one */
    {
        campaign_cfg c = mkcfg(1 << 20, (1 << 21) + 5000, 12);                            /* a short last block */
        campaign_cfg w = c;
        sched_t *whole = sched_new(&w);
        const u64 M = (u64)pz_u256_to_ld(&whole->blocks_all);
        snprintf(c.part, sizeof c.part, "1,3-4/6");
        sched_t *s = sched_new(&c);
        u128 expect = 0;
        for (u64 g = 0; g < M; g++) {
            pz_u256 gs = pz_u256_from_u64(g), local;
            bool mine = g % 6 == 1 || g % 6 == 3 || g % 6 == 4;
            CHECK(sched_part_position(s, &gs, &local) == mine);
            if (mine) { CHECK(local.w[0] == expect && !local.w[1] && !local.w[2] && !local.w[3]); expect++; }
        }
        CHECK((u128)(s->v_limit >> s->cbits) == expect && pz_u256_to_ld(&s->blocks_part) == (long double)expect);
        /* every claim of the part lies in a block that locate puts at a position of the part, in the order of the part */
        claim_t cl;
        u128 want_local = 0;
        for (;;) {
            claim_status st = sched_claim(s, 1u << 12, &cl);
            if (st == CLAIM_DONE) break;
            pz_u256 g, local;
            u64 off;
            CHECK(sched_locate(s, &cl.key_start, &g, &off));
            CHECK(sched_part_position(s, &g, &local));
            CHECK(local.w[0] == (u64)(cl.v_start >> 12) && !local.w[1]);
            if (cl.v_start >> 12 == want_local && off == 0) want_local++;
            sched_complete(s, &cl, cl.nkeys);
        }
        CHECK(want_local == expect || want_local + 1 == expect || want_local + 2 == expect);   /* blocks start at offset 0; a short one still counts */
        sched_free(s); sched_free(whole);
    }

    /* checkpoints belong to their share: another share, or none, is refused even when the sizes agree */
    {
        campaign_cfg c = mkcfg(5000, 5000 + (1 << 20) - 1, 12);                           /* 256 blocks: 0/2 and 1/2 have the same size */
        snprintf(c.part, sizeof c.part, "0/2");
        sched_t *a = sched_new(&c);
        claim_t cl;
        CHECK(sched_claim(a, 4096, &cl) == CLAIM_OK);
        sched_complete(a, &cl, cl.nkeys);
        CHECK(sched_save(a, "build/t_jobq_part.state") == 0);
        sched_t *same = sched_new(&c);
        CHECK(sched_load(same, "build/t_jobq_part.state", err, sizeof err) == 0 && sched_keys_done(same) == 4096);
        campaign_cfg o = c;
        snprintf(o.part, sizeof o.part, "1/2");
        sched_t *other = sched_new(&o);
        CHECK(sched_load(other, "build/t_jobq_part.state", err, sizeof err) == -2 && strstr(err, "split between computers"));
        o.part[0] = 0;
        sched_t *none = sched_new(&o);
        CHECK(sched_load(none, "build/t_jobq_part.state", err, sizeof err) == -2);
        /* and the other way round: a checkpoint without a split does not load into a share */
        sched_t *w = sched_new(&o);
        CHECK(sched_claim(w, 4096, &cl) == CLAIM_OK);
        sched_complete(w, &cl, cl.nkeys);
        CHECK(sched_save(w, "build/t_jobq_part.state") == 0);
        sched_t *a2 = sched_new(&c);
        CHECK(sched_load(a2, "build/t_jobq_part.state", err, sizeof err) == -2);
        sched_free(a); sched_free(same); sched_free(other); sched_free(none); sched_free(w); sched_free(a2);
    }

    /* too few blocks for the shares are reported before anything is created */
    {
        campaign_cfg c = mkcfg(0, 5 * 4096 - 1, 12);                                      /* 5 blocks */
        snprintf(c.part, sizeof c.part, "0/8");
        CHECK(sched_check_part(&c, err, sizeof err));
        snprintf(c.part, sizeof c.part, "6/8");
        CHECK(!sched_check_part(&c, err, sizeof err) && strstr(err, "too few"));
        snprintf(c.part, sizeof c.part, "4/8");
        CHECK(sched_check_part(&c, err, sizeof err));
    }

    /* a huge range (the virtual space is capped): the second half-share still starts inside the range, at an odd position */
    {
        campaign_cfg c;
        memset(&c, 0, sizeof c);
        c.range_start = pz_u256_pow2(138);
        pz_u256 one = pz_u256_from_u64(1);
        c.range_end = pz_u256_pow2(139);
        pz_u256_sub(&c.range_end, &c.range_end, &one);
        c.chunk_bits = 36;
        for (int i = 0; i < 32; i++) c.seed[i] = (u8)(i * 7 + 1);
        snprintf(c.part, sizeof c.part, "1/2");
        sched_t *s = sched_new(&c);
        CHECK(s->v_capped);
        for (int i = 0; i < 20; i++) {
            claim_t cl;
            CHECK(sched_claim(s, 1u << 20, &cl) == CLAIM_OK);
            CHECK(pz_u256_cmp(&cl.key_start, &c.range_start) >= 0 && pz_u256_cmp(&cl.key_start, &c.range_end) <= 0);
            pz_u256 g, local;
            u64 off;
            CHECK(sched_locate(s, &cl.key_start, &g, &off));
            CHECK(g.w[0] % 2 == 1);
            CHECK(sched_part_position(s, &g, &local) && local.w[0] == (u64)(cl.v_start >> 36) && local.w[1] == (u64)(cl.v_start >> 100));
            sched_complete(s, &cl, cl.nkeys);
        }
        CHECK(!sched_finished(s));
        sched_free(s);
    }
}

int main(void)
{
    /* Full coverage, no overlap, custom (non power of two) range with a short last chunk. */
    for (int variant = 0; variant < 3; variant++) {
        u64 start = 1000 + (u64)variant * 77, end = start + 300000 + (u64)variant * 1234 - 1;
        campaign_cfg c = mkcfg(start, end, 13);   /* 8192-key chunks */
        sched_t *s = sched_new(&c);
        u64 total = end - start + 1;
        u8 *cov = calloc(total, 1);
        claim_t cl;
        u64 got = 0;
        u64 want = (variant == 0) ? 5000 : (variant == 1 ? 1024 : 100000);
        claim_t pending[4]; int np = 0;
        for (;;) {
            claim_status st = sched_claim(s, want, &cl);
            if (st == CLAIM_DONE) break;
            if (st == CLAIM_WAIT) { for (int j = 0; j < np; j++) sched_complete(s, &pending[j], pending[j].nkeys); np = 0; continue; }
            u64 k0 = cl.key_start.w[0] - start;
            for (u64 i = 0; i < cl.nkeys; i++) { CHECK(cov[k0 + i] == 0); cov[k0 + i] = 1; }
            got += cl.nkeys;
            /* complete out of order, keeping a few in flight */
            pending[np++] = cl;
            if (np == 4) { for (int j = 3; j >= 0; j--) sched_complete(s, &pending[j], pending[j].nkeys); np = 0; }
        }
        for (int j = 0; j < np; j++) sched_complete(s, &pending[j], pending[j].nkeys);
        CHECK(got == total);
        for (u64 i = 0; i < total; i++) if (!cov[i]) { CHECK(0); break; }
        CHECK(sched_finished(s));
        CHECK(sched_keys_done(s) == total);
        CHECK(s->ndone == 1);
        sched_free(s); free(cov);
    }
    /* Checkpoint / resume with holes and partial completion. */
    {
        campaign_cfg c = mkcfg(5000, 5000 + (1 << 20) - 1, 12);
        sched_t *s = sched_new(&c);
        claim_t a, b, d, e;
        CHECK(sched_claim(s, 4096, &a) == CLAIM_OK);
        CHECK(sched_claim(s, 4096, &b) == CLAIM_OK);
        CHECK(sched_claim(s, 4096, &d) == CLAIM_OK);
        sched_complete(s, &a, a.nkeys);
        sched_complete(s, &d, d.nkeys);          /* b is a hole */
        CHECK(sched_claim(s, 4096, &e) == CLAIM_OK);
        sched_complete(s, &e, 2048);             /* partial: second half is a hole */
        CHECK(sched_save(s, "build/t_jobq.state") == 0);
        u128 kd = sched_keys_done(s);
        sched_t *s2 = sched_new(&c);
        char err[200];
        CHECK(sched_load(s2, "build/t_jobq.state", err, sizeof err) == 0);
        CHECK(sched_keys_done(s2) == kd);
        /* resumed scheduler must hand out exactly the missing pieces first (b, then e's tail) before fresh space */
        claim_t r;
        CHECK(sched_claim(s2, 1u << 20, &r) == CLAIM_OK);
        CHECK(r.v_start == b.v_start && r.nkeys == b.nkeys);
        sched_complete(s2, &r, r.nkeys);
        CHECK(sched_claim(s2, 1u << 20, &r) == CLAIM_OK);
        CHECK(r.v_start == e.v_start + 2048);
        sched_free(s); sched_free(s2);
        /* corrupted file is rejected */
        FILE *f = fopen("build/t_jobq.state", "r+b"); fseek(f, 20, SEEK_SET); fputc('Z', f); fclose(f);
        sched_t *s3 = sched_new(&c);
        CHECK(sched_load(s3, "build/t_jobq.state", err, sizeof err) == -1);
        sched_free(s3);
    }
    /* locate: key -> sequence number is consistent with claims */
    {
        campaign_cfg c = mkcfg(1 << 20, (1 << 21) - 1, 12);
        sched_t *s = sched_new(&c);
        claim_t cl; CHECK(sched_claim(s, 4096, &cl) == CLAIM_OK);
        pz_u256 seq; u64 off;
        CHECK(sched_locate(s, &cl.key_start, &seq, &off));
        CHECK(seq.w[0] == (u64)(cl.v_start >> 12) && !seq.w[1] && off == (cl.v_start & 4095));
        sched_free(s);
    }
    /* locate with positions beyond 128 bits (ranges of puzzle 166 and up): the block position is not truncated */
    {
        campaign_cfg c;
        memset(&c, 0, sizeof c);
        c.range_start = pz_u256_pow2(169);
        c.range_end = pz_u256_pow2(170);
        pz_u256_sub(&c.range_end, &c.range_end, &(pz_u256){{1, 0, 0, 0}});
        c.chunk_bits = 36;
        for (int i = 0; i < 32; i++) c.seed[i] = (u8)(i * 11 + 3);
        sched_t *s = sched_new(&c);
        pz_u256 pos = pz_u256_pow2(130);
        pz_u256_add_u64(&pos, &pos, 12345);                              /* the block at position 2^130 + 12345 of the visiting order */
        pz_u256 blk = sched_perm_apply(&s->perm, &pos), off36 = pz_u256_shl(&blk, 36), key;
        pz_u256_add(&key, &s->range_start, &off36);
        pz_u256_add_u64(&key, &key, 7);
        pz_u256 got;
        u64 off;
        CHECK(sched_locate(s, &key, &got, &off) && !pz_u256_cmp(&got, &pos) && off == 7);
        sched_free(s);
        /* an order of visit that does not exist for this range is reported before anything is created */
        char err[300];
        c.range_start = pz_u256_pow2(255);
        c.range_end = pz_u256_zero();
        pz_u256_sub(&c.range_end, &c.range_end, &(pz_u256){{1, 0, 0, 0}});    /* 2^256 - 1 */
        CHECK(!sched_check_part(&c, err, sizeof err) && strstr(err, "too large"));
        c.range_start = pz_u256_pow2(231);
        c.range_end = pz_u256_pow2(232);
        pz_u256_sub(&c.range_end, &c.range_end, &(pz_u256){{1, 0, 0, 0}});
        c.chunk_bits = 40;
        CHECK(sched_check_part(&c, err, sizeof err));                   /* puzzle 232 with the largest blocks: 191 bits, still fine */
        c.chunk_bits = 36;
        CHECK(!sched_check_part(&c, err, sizeof err));
    }
    t_parts();
    t_seed();
    printf(fails ? "t_jobq: %d FAILED\n" : "t_jobq: OK\n", fails);
    return fails != 0;
}
