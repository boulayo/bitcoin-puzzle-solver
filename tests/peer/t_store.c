/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_store.c - the custodian of store.c: a record stored, the same again (duplicate), a sequence number that does not go up, an invalid
 * record, the coincidences (useful and merge), many records (the index grows), the file opened again after a restart, a torn record at
 * the end cut, a store of another network refused; the limits (in all, per owner, owners of one x); a record damaged on disk, at the
 * end (not indexed, its sequence number not taken) and in the middle (never reported as a coincidence); then the mailbox, its limit per
 * owner and its proofs. The records are built here on points k*G of the test network. */
#include "store.h"
#include "keys.h"
#include <pzcore/hash.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0x57, 0x0e};
static record_net net;
static home_identity ida, idb, idc, idd;
static const char *PATH = PZ_TEST_DIR "/t_store.bin";

static void hashed(const char *s, uint8_t out[32]) { pz_sha256((const uint8_t *)s, strlen(s), out); }

/* the x of a point k*G, k from a label */
static void point_x(const char *label, uint8_t x[32])
{
    uint8_t k[32], c[33];
    secp256k1_pubkey p;
    size_t n = 33;
    hashed(label, k);
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, k) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &p, SECP256K1_EC_COMPRESSED));
    memcpy(x, c + 1, 32);
}

static record make(const home_identity *id, int t, const uint8_t x[32], uint64_t seq)
{
    record r = {.t = t, .seq = seq};
    memcpy(r.x, x, 32);
    memcpy(r.owner, id->xonly, 32);
    CHECK(record_sign(ctx, &net, &id->kp, &r));
    return r;
}

static bool same(const record *a, const record *b)
{
    uint8_t ea[RECORD_LEN], eb[RECORD_LEN];
    record_encode(a, ea), record_encode(b, eb);
    return !memcmp(ea, eb, RECORD_LEN);
}

static long file_size(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_size : -1;
}

#define MANY 3000

static void test_store(void)
{
    char err[200];
    uint8_t x1[32], x2[32], x3[32];
    point_x("x1", x1), point_x("x2", x2), point_x("x3", x3);
    remove(PATH);
    store *st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    record mine, others[4];
    int no;
    const record a1 = make(&ida, 0, x1, 10);
    CHECK(store_put(st, &a1, &mine, others, 4, &no) == STORE_OK && no == 0 && same(&mine, &a1));
    CHECK(store_put(st, &a1, &mine, others, 4, &no) == STORE_DUP && no == 0 && same(&mine, &a1));
    const record a1b = make(&ida, 1, x1, 11);                   /* the same x from the same owner, even with another type */
    CHECK(store_put(st, &a1b, &mine, others, 4, &no) == STORE_DUP && same(&mine, &a1));
    record lower = make(&ida, 0, x2, 9), equal = make(&ida, 0, x2, 10), next = make(&ida, 0, x2, 12);
    CHECK(store_put(st, &lower, &mine, others, 4, &no) == STORE_OLD && no == 0);
    CHECK(store_put(st, &equal, &mine, others, 4, &no) == STORE_OLD);
    CHECK(store_put(st, &next, &mine, others, 4, &no) == STORE_OK);
    uint64_t seq = 0;
    CHECK(store_last_seq(st, ida.xonly, &seq) && seq == 12 && !store_last_seq(st, idb.xonly, &seq));
    record bad = make(&idb, 0, x3, 1);
    bad.sig[5] ^= 1;
    CHECK(store_put(st, &bad, &mine, others, 4, &no) == STORE_INVALID);
    bad = make(&idb, 0, x3, 1);
    bad.x[31] ^= 1;                                             /* signed for another x */
    CHECK(store_put(st, &bad, &mine, others, 4, &no) == STORE_INVALID);
    /* a useful coincidence: the same x from another owner with another type; the other owner's sequence numbers are its own */
    const record b1 = make(&idb, 1, x1, 1);
    CHECK(store_put(st, &b1, &mine, others, 4, &no) == STORE_OK && no == 1 && same(&others[0], &a1) && same(&mine, &b1));
    CHECK(store_put(st, &b1, &mine, others, 4, &no) == STORE_DUP && no == 1 && same(&others[0], &a1));   /* again: still reported */
    /* a merge: a third owner with the type of the first */
    const record c1 = make(&idc, 0, x1, 5);
    CHECK(store_put(st, &c1, &mine, others, 4, &no) == STORE_OK && no == 2);
    CHECK((same(&others[0], &a1) && same(&others[1], &b1)) || (same(&others[0], &b1) && same(&others[1], &a1)));
    CHECK(others[0].t == c1.t || others[1].t == c1.t);
    record found[8];
    CHECK(store_find(st, x1, found, 8) == 3 && store_find(st, x3, found, 8) == 0);
    CHECK(store_count(st) == 4);
    /* many records of one owner: the index grows past its first size */
    for (int i = 0; i < MANY; i++) {
        char label[32];
        uint8_t x[32];
        snprintf(label, sizeof label, "many %d", i);
        point_x(label, x);
        const record r = make(&idc, i % 3 - 1, x, 100 + (uint64_t)i);
        if (store_put(st, &r, &mine, others, 4, &no) != STORE_OK) CHECK(!"stored");
    }
    CHECK(store_count(st) == 4 + MANY);
    store_close(st);
    CHECK(file_size(PATH) == STORE_HDR + (4 + MANY) * RECORD_LEN);

    /* a restart: everything is found again, the duplicates and the sequence numbers are remembered */
    st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    CHECK(store_count(st) == 4 + MANY && store_find(st, x1, found, 8) == 3 && store_find(st, x2, found, 8) == 1 && same(&found[0], &next));
    CHECK(store_put(st, &a1, &mine, others, 4, &no) == STORE_DUP && no == 2);
    CHECK(store_last_seq(st, idc.xonly, &seq) && seq == 100 + MANY - 1);
    const record c_old = make(&idc, 0, x3, 50);
    CHECK(store_put(st, &c_old, &mine, others, 4, &no) == STORE_OLD);
    record r0;
    CHECK(store_read(st, 0, &r0) && same(&r0, &a1) && !store_read(st, 4 + MANY, &r0));
    for (int i = 0; i < MANY; i += 97) {
        char label[32];
        uint8_t x[32];
        snprintf(label, sizeof label, "many %d", i);
        point_x(label, x);
        if (store_find(st, x, found, 8) != 1 || found[0].seq != 100 + (uint64_t)i) CHECK(!"found after the restart");
    }
    store_close(st);

    /* a record torn by a crash at the end: cut, the others kept */
    FILE *f = fopen(PATH, "ab");
    CHECK(f != NULL);
    if (f) {
        uint8_t whole[RECORD_LEN];
        record_encode(&c_old, whole);
        fwrite(whole, 1, RECORD_LEN / 2, f);
        fclose(f);
    }
    CHECK(file_size(PATH) == STORE_HDR + (4 + MANY) * RECORD_LEN + RECORD_LEN / 2);
    st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL && store_count(st) == 4 + MANY);
    CHECK(file_size(PATH) == STORE_HDR + (4 + MANY) * RECORD_LEN);
    if (st) {
        const record c3 = make(&idc, 0, x3, 100000);           /* the next record goes where the torn one was */
        CHECK(store_put(st, &c3, &mine, others, 4, &no) == STORE_OK && store_find(st, x3, found, 8) == 1 && same(&found[0], &c3));
        store_close(st);
    }
    CHECK(file_size(PATH) == STORE_HDR + (5 + MANY) * RECORD_LEN);

    /* another network, or not a store */
    record_net other = net;
    other.fp[0] ^= 1;
    CHECK(store_open(PATH, ctx, &other, err, sizeof err) == NULL && strstr(err, "another network"));
    f = fopen(PATH, "r+b");
    if (f) {
        fputc('X', f);
        fclose(f);
    }
    CHECK(store_open(PATH, ctx, &net, err, sizeof err) == NULL);
    remove(PATH);
}

/* the limits of a custodian: records in all, records of one owner, owners of one x */
static void test_limits(void)
{
    char err[200];
    remove(PATH);
    store *st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    store_limits(st, 6, 3);
    record mine, others[STORE_X_OWNERS + 1];
    int no;
    uint8_t x[32];
    for (int i = 0; i < 4; i++) {                              /* the fourth record of one owner is over its share */
        char label[32];
        snprintf(label, sizeof label, "lim %d", i);
        point_x(label, x);
        const record r = make(&ida, 0, x, 10 + (uint64_t)i);
        CHECK(store_put(st, &r, &mine, others, 4, &no) == (i < 3 ? STORE_OK : STORE_FULL));
    }
    for (int i = 0; i < 4; i++) {                              /* the other owner fills the rest: 6 in all */
        char label[32];
        snprintf(label, sizeof label, "lim b %d", i);
        point_x(label, x);
        const record r = make(&idb, 0, x, 10 + (uint64_t)i);
        CHECK(store_put(st, &r, &mine, others, 4, &no) == (i < 3 ? STORE_OK : STORE_FULL));
    }
    CHECK(store_count(st) == 6);
    point_x("lim 0", x);
    const record dup = make(&ida, 0, x, 99);                   /* a duplicate is no new record: still answered as one */
    CHECK(store_put(st, &dup, &mine, others, 4, &no) == STORE_DUP);
    store_close(st);
    remove(PATH);
    /* owners of one x: STORE_X_OWNERS, then the store is full for that x, and still reports the coincidences of a record it refuses */
    st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    point_x("crowded", x);
    for (int i = 0; i <= STORE_X_OWNERS; i++) {
        char label[32];
        uint8_t seed[32];
        home_identity id;
        snprintf(label, sizeof label, "owner %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, 4, &id));
        const record r = make(&id, i % 2, x, 1);
        const store_status got = store_put(st, &r, &mine, others, STORE_X_OWNERS + 1, &no);
        CHECK(got == (i < STORE_X_OWNERS ? STORE_OK : STORE_FULL) && no == i);   /* not kept, its coincidences told all the same */
    }
    store_close(st);
    remove(PATH);
    /* owners in all: 4096 records may take 4096 / 256 + 1024 owners; one more is refused, an owner already there is not */
    st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    store_limits(st, 4096, 0);
    home_identity first = {0};
    int ok = 0;
    for (int i = 0; i <= 4096 / 256 + 1024; i++) {
        char label[32];
        uint8_t seed[32];
        home_identity id;
        snprintf(label, sizeof label, "many %d", i);
        hashed(label, seed), point_x(label, x);
        if (!keys_identity(ctx, seed, FP, 4, &id)) continue;
        const record r = make(&id, 0, x, 1);
        ok += store_put(st, &r, &mine, others, 4, &no) == (i < 4096 / 256 + 1024 ? STORE_OK : STORE_FULL);
        if (!i) first = id;
    }
    CHECK(ok == 4096 / 256 + 1024 + 1);
    point_x("many again", x);
    const record again = make(&first, 0, x, 2);
    CHECK(store_put(st, &again, &mine, others, 4, &no) == STORE_OK);
    store_close(st);
    remove(PATH);
}

/* A record damaged on disk. At the end of the file (a crash): it is checked again when the file is opened, not indexed, and its sequence
 * number, which would refuse every next record of its owner, is not taken. In the middle (not checked at the opening): never given as
 * the other record of a coincidence. */
static void test_damaged(void)
{
    char err[200];
    uint8_t x1[32], x2[32], x3[32];
    point_x("d1", x1), point_x("d2", x2), point_x("d3", x3);
    remove(PATH);
    store *st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    record mine, others[4], found[4];
    int no;
    const record a1 = make(&ida, 0, x1, 5);
    CHECK(store_put(st, &a1, &mine, others, 4, &no) == STORE_OK);
    for (int i = 0; i < STORE_CHECK_TAIL; i++) {               /* enough records after it that it is out of the tail */
        char label[32];
        uint8_t x[32];
        snprintf(label, sizeof label, "fill %d", i);
        point_x(label, x);
        const record r = make(&idc, 1, x, 10 + (uint64_t)i);
        if (store_put(st, &r, &mine, others, 4, &no) != STORE_OK) CHECK(!"stored");
    }
    store_close(st);
    /* a record of b with a huge sequence number, its signature not over it, written at the end as a crash could leave it */
    record bad = make(&idb, 0, x2, 7);
    bad.seq = UINT64_MAX - 1;
    uint8_t enc[RECORD_LEN];
    record_encode(&bad, enc);
    FILE *f = fopen(PATH, "r+b");
    CHECK(f != NULL);
    if (f) {
        fseek(f, 0, SEEK_END);
        fwrite(enc, 1, RECORD_LEN, f);
        fseek(f, STORE_HDR + RECORD_LEN - 5, SEEK_SET);        /* and a1's signature changed, in the middle */
        const int c = fgetc(f);
        fseek(f, STORE_HDR + RECORD_LEN - 5, SEEK_SET);
        fputc(c ^ 1, f);
        fclose(f);
    }
    st = store_open(PATH, ctx, &net, err, sizeof err);
    CHECK(st != NULL);
    if (!st) return;
    uint64_t seq = 0;
    CHECK(!store_last_seq(st, idb.xonly, &seq) && store_find(st, x2, found, 4) == 0);
    const record b3 = make(&idb, 0, x3, 8);
    CHECK(store_put(st, &b3, &mine, others, 4, &no) == STORE_OK);
    const record b1 = make(&idb, 1, x1, 9);                    /* the same x as the damaged a1: stored, but no coincidence */
    CHECK(store_put(st, &b1, &mine, others, 4, &no) == STORE_OK && no == 0);
    store_close(st);
    remove(PATH);
}

static bool has_pair(const mbox_pair *p, int n, const record *a, const record *b)
{
    for (int i = 0; i < n; i++)
        if ((same(&p[i].r[0], a) && same(&p[i].r[1], b)) || (same(&p[i].r[0], b) && same(&p[i].r[1], a))) return true;
    return false;
}

static void test_mbox(void)
{
    uint8_t x1[32], x2[32];
    point_x("m1", x1), point_x("m2", x2);
    const record a = make(&ida, 0, x1, 1), b = make(&idb, 1, x1, 1), c = make(&idc, 0, x2, 1), a2 = make(&ida, -1, x2, 2);
    CHECK(store_pair_ok(ctx, &net, &a, &b) && !store_pair_ok(ctx, &net, &a, &a) && !store_pair_ok(ctx, &net, &a, &c));
    record bad = b;
    bad.sig[0] ^= 1;
    CHECK(!store_pair_ok(ctx, &net, &a, &bad));
    mbox *m = mbox_new(3, 0);
    mbox_pair pairs[4];
    CHECK(m && mbox_put(m, &b, &a, NULL) && !mbox_put(m, &a, &b, NULL));   /* the same pair, in either order */
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 1 && mbox_get(m, idb.xonly, pairs, 4) == 1 && mbox_get(m, idc.xonly, pairs, 4) == 0);
    const bool a_first = memcmp(ida.xonly, idb.xonly, 32) < 0;   /* kept in the order of the owners */
    CHECK(same(&pairs[0].r[0], a_first ? &a : &b) && same(&pairs[0].r[1], a_first ? &b : &a) && !pairs[0].has[0] && !pairs[0].has[1]);
    /* a proof of each owner, kept with the pair at the place of its owner; the same again is nothing new */
    uint8_t pa[64], pb[64];
    memset(pa, 0xa, 64), memset(pb, 0xb, 64);
    CHECK(mbox_put(m, &a, &b, pa) && !mbox_put(m, &a, &b, pa) && !mbox_put(m, &b, &a, NULL) && mbox_put(m, &b, &a, pb));
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 1 && pairs[0].has[0] && pairs[0].has[1]);
    CHECK(!memcmp(pairs[0].proof[a_first ? 0 : 1], pa, 64) && !memcmp(pairs[0].proof[a_first ? 1 : 0], pb, 64));
    CHECK(mbox_put(m, &c, &a2, NULL));
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 2 && mbox_get(m, ida.xonly, pairs, 1) == 1);
    CHECK(pairs[0].r[0].x[0] == x2[0] && !memcmp(pairs[0].r[0].x, x2, 32));   /* the newest first */
    /* three more pairs in a mailbox of three: the oldest that nobody proved go */
    for (int i = 0; i < 3; i++) {
        char label[16];
        uint8_t x[32];
        snprintf(label, sizeof label, "m3 %d", i);
        point_x(label, x);
        const record p = make(&idb, 0, x, 10 + (uint64_t)i), q = make(&idc, 1, x, 10 + (uint64_t)i);
        CHECK(mbox_put(m, &p, &q, NULL));
    }
    /* the pair of a with c went, and the first of the three; the pair of a with b, which both proved, stays */
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 1 && has_pair(pairs, 1, &a, &b) && mbox_get(m, idc.xonly, pairs, 4) == 2);
    CHECK(mbox_put(m, &c, &a2, NULL));                          /* gone, so new again */
    mbox_free(m);
    /* a limit per owner: a flood of pairs of b and c pushes out their own oldest only, not the pairs of a and d */
    const record d1 = make(&idd, 1, x1, 1);
    m = mbox_new(100, 4);
    CHECK(m && mbox_put(m, &a, &d1, NULL) && mbox_put(m, &a, &c, NULL));
    for (int i = 0; i < 10; i++) {
        char label[16];
        uint8_t x[32];
        snprintf(label, sizeof label, "m4 %d", i);
        point_x(label, x);
        const record p = make(&idb, 0, x, 10 + (uint64_t)i), q = make(&idc, 1, x, 10 + (uint64_t)i);
        CHECK(mbox_put(m, &p, &q, NULL));
    }
    CHECK(mbox_get(m, idb.xonly, pairs, 4) == 4 && mbox_get(m, idd.xonly, pairs, 4) == 1 && has_pair(pairs, 1, &a, &d1));
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 1 && has_pair(pairs, 1, &a, &d1));   /* the pair of a with c went: c had too many */
    mbox_free(m);
    /* What one identity that copied the points of a can do to the pair of a with b, which b proved: the same point under ten sequence
     * numbers is one pair, and ten points push out each other, not the proved pair. Before, any four of them pushed it out. */
    m = mbox_new(100, 4);
    CHECK(m && mbox_put(m, &b, &a, pb));
    for (int i = 0; i < 10; i++) {
        const record q = make(&idc, 1, x1, 20 + (uint64_t)i);
        CHECK(mbox_put(m, &a, &q, NULL) == (i == 0));
    }
    const record c9 = make(&idc, 1, x1, 99);
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 2 && !mbox_put(m, &a, &c9, NULL) && !has_pair(pairs, mbox_get(m, idc.xonly, pairs, 4), &a, &c9));
    for (int i = 0; i < 10; i++) {
        char label[16];
        uint8_t x[32];
        snprintf(label, sizeof label, "m5 %d", i);
        point_x(label, x);
        const record p = make(&ida, 0, x, 30 + (uint64_t)i), q = make(&idc, 1, x, 30 + (uint64_t)i);
        CHECK(mbox_put(m, &p, &q, NULL));
    }
    CHECK(mbox_get(m, ida.xonly, pairs, 4) == 4 && has_pair(pairs, 4, &a, &b));
    /* the pair with other sequence numbers takes the place of the one kept only when it alone is proved */
    const record c1 = make(&idc, 1, x1, 20);
    mbox_free(m);
    m = mbox_new(100, 4);
    CHECK(m && mbox_put(m, &a, &c1, NULL) && mbox_put(m, &a, &c9, pa) && mbox_get(m, ida.xonly, pairs, 4) == 1 && has_pair(pairs, 1, &a, &c9));
    CHECK(!mbox_put(m, &a, &c1, pa) && has_pair(pairs, mbox_get(m, ida.xonly, pairs, 4), &a, &c9));
    mbox_free(m);
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t k[32], base[32] = {0}, p[33], seed[32];
    secp256k1_pubkey pk;
    size_t n = 33;
    hashed("store puzzle", k);
    base[31] = 0x20;
    CHECK(secp256k1_ec_pubkey_create(ctx, &pk, k) && secp256k1_ec_pubkey_serialize(ctx, p, &n, &pk, SECP256K1_EC_COMPRESSED));
    CHECK(record_net_init(ctx, &net, FP, 0, p, base));
    home_identity *ids[4] = {&ida, &idb, &idc, &idd};
    for (int i = 0; i < 4; i++) {
        char label[32];
        snprintf(label, sizeof label, "store %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, 4, ids[i]));
    }
    test_store();
    test_limits();
    test_damaged();
    test_mbox();
    secp256k1_context_destroy(ctx);
    printf(fails ? "t_store: %d FAILED\n" : "t_store: OK\n", fails);
    return fails != 0;
}
