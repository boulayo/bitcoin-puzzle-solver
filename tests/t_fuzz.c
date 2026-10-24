/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_fuzz.c - robustness against corrupted or hostile input files and strings: nothing may crash, hang, leak or read out of
 * bounds (run it under ASan/UBSan: tools/verify.sh does). Deterministic: a fixed-seed generator mutates valid inputs. */
#include "../src/bsgs.h"
#include "../src/platform.h"
#include "../src/campaigns.h"
#include "../src/hash.h"
#include "../src/jobq.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u64 rs = 0x0123456789ABCDEFULL;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static char *mutate(const char *src, size_t len, size_t *outlen)
{
    size_t cap = len + 64;
    char *b = pz_xcalloc(cap + 1, 1);
    memcpy(b, src, len);
    size_t n = len;
    int ops = 1 + (int)(rnd() % 6);
    for (int i = 0; i < ops; i++) {
        switch (rnd() % 6) {
        case 0: if (n) b[rnd() % n] = (char)rnd(); break;                              /* random byte */
        case 1: if (n) b[rnd() % n] ^= (char)(1u << (rnd() % 8)); break;               /* bit flip */
        case 2: if (n) n = rnd() % (n + 1); break;                                      /* truncate */
        case 3: if (n && n + 8 < cap) { size_t p = rnd() % n; memmove(b + p + 4, b + p, n - p); memcpy(b + p, "\n\0=\xff", 4); n += 4; } break;
        case 4: if (n > 8) { size_t p = rnd() % (n - 4); memcpy(b + p, b + (rnd() % (n - 4)), 4); } break;   /* duplicate a fragment */
        default: if (n) b[rnd() % n] = "0123456789abcdefx-=\n "[rnd() % 21]; break;      /* structured character */
        }
    }
    *outlen = n;
    return b;
}

static void put(const char *path, const void *data, size_t n)
{
    int fd = pz_open_trunc(path, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK(write(fd, data, n) == (ssize_t)n); close(fd); }
}

/* Random text through the share parser: it never crashes, and what it accepts is a valid, canonical share list. */
static void fuzz_part_text(void)
{
    static const char alphabet[] = "0123456789,,--//  a\n";
    int accepted = 0;
    for (int it = 0; it < 200000; it++) {
        char t[48];
        size_t n = rnd() % (sizeof t - 1);
        for (size_t i = 0; i < n; i++) t[i] = alphabet[rnd() % (sizeof alphabet - 1)];
        t[n] = 0;
        part_spec p;
        char err[200];
        if (!part_parse(t, &p, err, sizeof err)) continue;
        accepted++;
        if (!p.total) continue;
        CHECK(p.total >= 2 && p.total <= PART_MAX_TOTAL && p.count >= 1 && p.count < p.total);
        for (unsigned i = 0; i < p.count; i++) CHECK(p.res[i] < p.total && (i == 0 || p.res[i] > p.res[i - 1]));
        char canon[PART_TEXT];
        part_format(&p, canon, sizeof canon);
        part_spec q;
        CHECK(part_parse(canon, &q, err, sizeof err) && q.total == p.total && q.count == p.count && !memcmp(q.res, p.res, p.count * sizeof p.res[0]));
    }
    CHECK(accepted > 0);
}

static void fuzz_cfg(const char *dir)
{
    campaign_cfg c;
    memset(&c, 0, sizeof c);
    c.format_version = PZ_FORMAT_VERSION;
    snprintf(c.name, sizeof c.name, "fuzz");
    c.puzzle = 66;
    c.range_start = u256_pow2(65);
    c.range_end = u256_pow2(66);
    u256_sub(&c.range_end, &c.range_end, &(u256){{1, 0, 0, 0}});
    snprintf(c.address, sizeof c.address, "13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so");
    c.has_pubkey = false;
    c.chunk_bits = 30;
    c.bsgs_t = 1000;
    snprintf(c.part, sizeof c.part, "0-2,5/8");
    char path[700];
    snprintf(path, sizeof path, "%s/campaign.conf", dir);
    CHECK(cfg_save(&c, path) == 0);
    size_t len;
    char *good = pz_read_file(path, &len);
    CHECK(good != NULL);
    campaign_cfg l;
    char err[300];
    CHECK(cfg_load(&l, path, err, sizeof err) == 0 && l.puzzle == 66 && l.bsgs_t == 1000 && !strcmp(l.part, "0-2,5/8"));
    int ok = 0, bad = 0;
    for (int it = 0; it < 4000; it++) {
        size_t n;
        char *m = mutate(good, len, &n);
        put(path, m, n);
        if (cfg_load(&l, path, err, sizeof err) == 0) ok++; else bad++;
        free(m);
    }
    CHECK(bad > 0);
    free(good);
    (void)ok;
}

static void fuzz_sched(const char *dir)
{
    campaign_cfg c;
    memset(&c, 0, sizeof c);
    c.format_version = PZ_FORMAT_VERSION;
    c.range_start = u256_from_u64(1);
    c.range_end = u256_from_u64(1000000);
    c.chunk_bits = 8;
    for (int i = 0; i < 32; i++) c.seed[i] = (u8)(i * 7 + 1);
    sched_t *s = sched_new(&c);
    for (int i = 0; i < 40; i++) {
        claim_t cl;
        if (sched_claim(s, 700, &cl) != CLAIM_OK) break;
        sched_complete(s, &cl, i % 5 == 0 ? cl.nkeys / 2 : cl.nkeys);
    }
    char path[700];
    snprintf(path, sizeof path, "%s/state.dat", dir);
    CHECK(sched_save(s, path) == 0);
    size_t len;
    char *good = pz_read_file(path, &len);
    CHECK(good != NULL);
    char err[300];
    sched_t *t = sched_new(&c);
    CHECK(sched_load(t, path, err, sizeof err) == 0);
    CHECK(sched_keys_done(t) == sched_keys_done(s));
    sched_free(t);
    int accepted = 0;
    for (int it = 0; it < 4000; it++) {
        size_t n;
        char *m = mutate(good, len, &n);
        put(path, m, n);
        t = sched_new(&c);
        int rc = sched_load(t, path, err, sizeof err);
        if (rc == 0) {   /* a mutation that still passes the checksum must leave a consistent scheduler */
            accepted++;
            claim_t cl;
            claim_status st = sched_claim(t, 300, &cl);
            if (st == CLAIM_OK) sched_complete(t, &cl, cl.nkeys);
        }
        sched_free(t);
        free(m);
    }
    (void)accepted;
    free(good);
    sched_free(s);
}

static void fuzz_table(const char *dir)
{
    char path[700], err[300];
    snprintf(path, sizeof path, "%s/t.tbl", dir);
    bsgs_table t;
    CHECK(bsgs_table_alloc(&t, 3000, err, sizeof err));
    CHECK(bsgs_table_build(&t, 2, NULL, NULL));
    CHECK(bsgs_table_save(&t, path, 2, err, sizeof err));
    size_t len;
    char *good = pz_read_file(path, &len);
    CHECK(good != NULL);
    int accepted = 0;
    for (int it = 0; it < 300; it++) {
        char *m = pz_xcalloc(len + 1, 1);
        memcpy(m, good, len);
        size_t n = len;
        switch (it % 5) {
        case 0: m[rnd() % 4096] ^= (char)(1 + rnd() % 255); break;                       /* header byte */
        case 1: m[4096 + rnd() % (len - 4096)] ^= (char)(1 + rnd() % 255); break;        /* body byte */
        case 2: n = rnd() % len; break;                                                  /* truncation */
        case 3: n = len; for (int k = 0; k < 8; k++) m[rnd() % len] = (char)rnd(); break;
        default: n = len > 100 ? len - (size_t)(1 + rnd() % 100) : len; break;
        }
        put(path, m, n);
        u64 T;
        bool peek = bsgs_table_peek(path, &T, err, sizeof err);
        bsgs_table l;
        if (bsgs_table_load(&l, path, 2, err, sizeof err)) {
            accepted++;
            CHECK(l.T == 3000 && memcmp(l.slots, t.slots, t.nslots * 8) == 0);   /* never accept a wrong table */
            bsgs_table_free(&l);
        }
        (void)peek;
        free(m);
    }
    CHECK(accepted == 0 || accepted < 300);
    free(good);
    bsgs_table_free(&t);
}

static void fuzz_parsers(void)
{
    char buf[200];
    u8 raw[130], h[20];
    for (int it = 0; it < 20000; it++) {
        size_t n = rnd() % 120;
        for (size_t i = 0; i < n; i++) buf[i] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz0Il\x01\xff"[rnd() % 63];
        buf[n] = 0;
        (void)address_to_hash160(buf, h);
        (void)base58check_decode(buf, raw, sizeof raw);
        u256 v;
        (void)u256_from_hex(&v, buf);
        (void)pz_hex_decode(buf, raw, sizeof raw);
        for (size_t i = 0; i < n; i++) raw[i % 130] = (u8)rnd();
        ge p;
        (void)ge_from_pubkey(&p, raw, n % 3 == 0 ? 33 : n % 3 == 1 ? 65 : n % 70);
    }
    CHECK(!address_to_hash160("", h) && !address_to_hash160("1", h) && !address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMX", h));
    CHECK(address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", h));
}

static void fuzz_list(const char *dir)
{
    /* `list` must survive campaign directories with garbage status.json / campaign.conf */
    char d1[700], p[800];
    snprintf(d1, sizeof d1, "%s/camp", dir);
    CHECK(pz_mkdir1(d1) == 0);
    snprintf(p, sizeof p, "%s/campaign.conf", d1);
    campaign_cfg c;
    memset(&c, 0, sizeof c);
    c.format_version = PZ_FORMAT_VERSION;
    snprintf(c.name, sizeof c.name, "camp");
    c.range_start = u256_from_u64(1);
    c.range_end = u256_from_u64(100);
    snprintf(c.address, sizeof c.address, "x");
    c.chunk_bits = 4;
    CHECK(cfg_save(&c, p) == 0);
    snprintf(p, sizeof p, "%s/status.json", d1);
    const char *good = "{\n \"state\": \"IN CORSO\",\n \"fraction\": 0.5,\n \"rate_per_second\": 1000.0,\n \"eta_seconds_50\": 12.5,\n \"updated_unix\": 100\n}\n";
    for (int it = 0; it < 300; it++) {
        size_t n;
        char *m = mutate(good, strlen(good), &n);
        put(p, m, n);
        free(m);
        fflush(stdout);
        int saved = dup(1);
#ifdef _WIN32
        int devnull = open("NUL", O_WRONLY);
#else
        int devnull = open("/dev/null", O_WRONLY);
#endif
        dup2(devnull, 1);
        campaigns_list(dir);
        fflush(stdout);
        dup2(saved, 1);
        close(saved);
        close(devnull);
    }
}

int main(void)
{
    ec_init();
    char dir[600];
    if (!pz_make_temp_dir(dir, sizeof dir, "pzfuzz")) { puts("mkdtemp"); return 2; }
    fuzz_part_text();
    fuzz_cfg(dir);
    fuzz_sched(dir);
    fuzz_table(dir);
    fuzz_parsers();
    fuzz_list(dir);
    CHECK(pz_remove_tree(dir) == 0);
    puts(fails ? "t_fuzz: FAILED" : "t_fuzz: ok");
    return fails != 0;
}
