/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_spv.c - the chain of headers and the Merkle proofs. The rule of the target on real headers of the main chain (the first change of
 * difficulty, at block 32256), then everything else on a chain mined here at the difficulty of regtest: what extends the chain, what is
 * refused (another block before, no proof of work, another target, a time at or below the median, a time in the future), a branch with less
 * work and one with more, the file, and the proofs. */
#include "peertest.h"
#include "spv.h"
#include <pzcore/hash.h>

#define NOW 2000000000u

static const struct { unsigned height; const char *hash, *header; } REAL[] = {
    {2015, "00000000693067b0e6b440bc51450b9f3850561b07f6d3c021c54fbd6abb9763",
     "01000000e25509cde707c3d02a693e4fe9e7cdd57c38a0d2c8d6341f20dae84b000000000c113df1185e162ee92d031fe21d1400ff7d705a3e9b9c860eea855313cd8ca26c087f49ffff001d30b73231"},
    {2016, "00000000a141216a896c54f211301c436e557a8d55900637bbdce14c6c7bddef",
     "010000006397bb6abd4fc521c0d3f6071b5650389f0b4551bc40b4e6b067306900000000ace470aecda9c8818c8fe57688cd2a772b5a57954a00df0420a7dd546b6d2c576b0e7f49ffff001d33f0192f"},
    {30240, "000000000fa8bfa0f0dd32f956b874b2c7f1772c5fbedcb1b35e03335c7fb0a8",
     "01000000e6bf7fd7f7790a63786faa878d0dc7fd8f2ff365732e45862c66075100000000700d342f65c7b6834dffb615358a1897016f0448913372190cbe3d27a4b53355b1512b4bffff001dbfb02519"},
    {32255, "00000000984f962134a7291e3693075ae03e521f0ee33378ec30a334d860034b",
     "0100000049c1daab3b6536ff1b2633c3a316a6e06ec287676cdeec4ca7baae6b00000000ac10b36b8f354b3353207de15940a5edbc05bb8364af75b4b5409e7823f2b48923ec3a4bffff001dbd5fa412"},
    {32256, "000000004f2886a170adb7204cb0c7a824217dd24d11a74423d564c4e0904967",
     "010000004b0360d834a330ec7833e30e1f523ee05a0793361e29a73421964f980000000027b64a020af294e903feed93768705336a20090612a043f47af462a2f5e5b564f8ee3a4b6ad8001dd3a43707"},
    /* the checkpoint pzpeer ships for the main chain, the last block of its period and the first of the next */
    {969696, "000000000000000000001e39df127cbab82a824ac43cfcdbf62e59f6a08b9f0b",
     "00e00020ea7409145abed3693b6aa5e269dea055406e0962804301000000000000000000a8f40cc468f3aa4518fdc368e37fc7aff01eb12b293885f847517602a96e3a93d4abc06af01e021779d3a409"},
    {971711, "0000000000000000000051aed6d87a76a4f2387ce41455b61f3cd64cd7a95e2e",
     "008000209818aca2f44b68ac8382c3849cbf224c9cedd66b51d9010000000000000000007ed241313648e28d9143a78f2065402e3222a2de61c480061edadb3c75a14ee79e18d26af01e0217883f947a"},
    {971712, "00000000000000000000c185f0906690a08d62d6fe842682a888d5560217e467",
     "006005202e5ea9d74cd63c1fb65514e47c38f2a4767ad8d6ae51000000000000000000003d63f20e90eb3b43e72ea91858f41d1d92103442acfa6106400e04e08f4d94df8b1ad26a930002176d7224e8"},
};

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void real(unsigned height, uint8_t hdr[80], uint8_t hash[32])
{
    for (size_t i = 0; i < sizeof REAL / sizeof *REAL; i++)
        if (REAL[i].height == height) {
            CHECK(pt_unhex(REAL[i].header, hdr, 80) == 80 && btc_hash_from_hex(REAL[i].hash, hash));
            return;
        }
    CHECK(!"a header of the table");
}

static void test_main_chain(void)
{
    uint8_t a[80], b[80], c[80], ha[32], hb[32], hc[32], t[32], want[32] = {0};
    CHECK(spv_target(0x1d00ffffu, t));
    want[4] = want[5] = 0xff;
    CHECK(!memcmp(t, want, 32));
    CHECK(spv_target(0x1b0404cbu, t) && t[5] == 0x04 && t[6] == 0x04 && t[7] == 0xcb && t[4] == 0 && t[8] == 0);
    CHECK(!spv_target(0x1d80ffffu, t) && !spv_target(0xff00ffffu, t));          /* negative, overflow */
    CHECK(spv_target(0x03123456u, t) && t[29] == 0x12 && t[31] == 0x56 && spv_target(0x01120000u, t) && t[31] == 0x12);
    /* the first period kept the difficulty: blocks 2015 and 2016 have the bits of the genesis block, though the period took far longer
     * than two weeks (the change is capped at a factor of 4, and the target at its limit) */
    real(2015, a, ha), real(2016, b, hb);
    CHECK(le32(b + 72) == 0x1d00ffffu && spv_retarget(le32(a + 72), 40 * 24 * 3600) == 0x1d00ffffu);
    /* the first change: block 32256, from the times of blocks 30240 and 32255 */
    real(30240, a, ha), real(32255, b, hb), real(32256, c, hc);
    CHECK(le32(c + 72) == 0x1d00d86au);
    CHECK(spv_retarget(le32(b + 72), (int64_t)le32(b + 68) - (int64_t)le32(a + 68)) == le32(c + 72));
    CHECK(spv_retarget(0x1d00d86au, 1) == spv_retarget(0x1d00d86au, 14 * 24 * 3600 / 4));   /* at most 4 times harder */
    /* a real header as a checkpoint, and the real block after 32255 on a chain that starts there: only on a period boundary */
    spv_chain ch;
    CHECK(!spv_init(&ch, BTC_MAIN, 32255, hb));
    CHECK(spv_init(&ch, BTC_MAIN, 32256, hc) && !spv_add(&ch, 32256, b, 1, NOW) && spv_add(&ch, 32256, c, 1, NOW));
    uint64_t tip;
    CHECK(spv_tip(&ch, &tip) && tip == 32256 && !memcmp(spv_header(&ch, 32256), c, 80) && !spv_header(&ch, 32255));
    uint8_t bad[80];
    memcpy(bad, b, 80), memcpy(bad + 4, hc, 32);                                  /* linked, but its work is for another header */
    CHECK(!spv_add(&ch, 32257, bad, 1, NOW));
    spv_free(&ch);
    /* the same at today's difficulty: block 971712 from the times of 969696 and 971711; and 969696 starts a chain by its hash */
    real(969696, a, ha), real(971711, b, hb), real(971712, c, hc);
    CHECK(le32(c + 72) == 0x17020093u && spv_retarget(le32(b + 72), (int64_t)le32(b + 68) - (int64_t)le32(a + 68)) == le32(c + 72));
    CHECK(spv_init(&ch, BTC_MAIN, 969696, ha) && spv_add(&ch, 969696, a, 1, NOW) && !spv_add(&ch, 969697, c, 1, NOW));
    spv_free(&ch);
    uint8_t g[32];
    spv_genesis(BTC_MAIN, g);
    CHECK(g[31] == 0 && g[0] == 0x6f);
}

/* a header after prev (NULL: none) with a nonce that gives it the proof of work of regtest; salt makes different headers */
static void mine(const uint8_t *prev, uint32_t time, uint32_t bits, uint32_t salt, uint8_t out[80])
{
    uint8_t target[32], hash[32], rev[32];
    memset(out, 0, 80);
    out[0] = 2;
    if (prev) pz_sha256d(prev, 80, out + 4);
    for (int b = 0; b < 4; b++) out[36 + b] = (uint8_t)(salt >> (8 * b)), out[68 + b] = (uint8_t)(time >> (8 * b)), out[72 + b] = (uint8_t)(bits >> (8 * b));
    CHECK(spv_target(bits, target));
    for (uint32_t nonce = 0;; nonce++) {
        for (int b = 0; b < 4; b++) out[76 + b] = (uint8_t)(nonce >> (8 * b));
        pz_sha256d(out, 80, hash);
        for (int b = 0; b < 32; b++) rev[b] = hash[31 - b];
        if (memcmp(rev, target, 32) <= 0) return;
    }
}

/* the same header with a nonce that fails the proof of work */
static void unmine(uint8_t h[80])
{
    uint8_t target[32], hash[32], rev[32];
    CHECK(spv_target(le32(h + 72), target));
    for (uint32_t nonce = 0;; nonce++) {
        for (int b = 0; b < 4; b++) h[76 + b] = (uint8_t)(nonce >> (8 * b));
        pz_sha256d(h, 80, hash);
        for (int b = 0; b < 32; b++) rev[b] = hash[31 - b];
        if (memcmp(rev, target, 32) > 0) return;
    }
}

#define N 40
#define BITS 0x207fffffu
#define T0 1700000000u

static void test_chain(void)
{
    static uint8_t h[N][80], f[N][80];
    uint8_t cp[32], x[80];
    uint64_t tip;
    mine(NULL, T0, BITS, 0, h[0]);
    for (int i = 1; i < N; i++) mine(h[i - 1], T0 + 600 * (uint32_t)i, BITS, (uint32_t)i, h[i]);
    pz_sha256d(h[0], 80, cp);
    spv_chain c;
    CHECK(spv_init(&c, BTC_REGTEST, 100, cp) && !spv_tip(&c, &tip));
    CHECK(!spv_add(&c, 100, h[1], 1, NOW) && !spv_add(&c, 101, h[0], 1, NOW));   /* not the checkpoint, not its height */
    CHECK(spv_add(&c, 100, h[0], 10, NOW) && spv_tip(&c, &tip) && tip == 109);
    CHECK(spv_add(&c, 105, h[5], 10, NOW) && spv_tip(&c, &tip) && tip == 114);   /* overlapping what is known */
    CHECK(!spv_add(&c, 116, h[16], 1, NOW));                                      /* a gap */
    /* each way a header can be wrong: refused, and the chain is as it was */
    memcpy(x, h[15], 80), x[4] ^= 1, unmine(x), mine(NULL, 0, BITS, 0, f[0]);
    memcpy(x, h[15], 80), x[5] ^= 1;
    CHECK(!spv_add(&c, 115, x, 1, NOW));                                          /* another block before (and no work) */
    memcpy(x, h[15], 80), unmine(x);
    CHECK(!spv_add(&c, 115, x, 1, NOW));                                          /* no proof of work */
    mine(h[14], T0 + 600 * 15, 0x207ffffeu, 15, x);
    CHECK(!spv_add(&c, 115, x, 1, NOW));                                          /* another target: regtest never changes it */
    mine(h[14], T0 + 600 * 9, BITS, 15, x);                                       /* the median of blocks 4..14 is the time of block 9 */
    CHECK(!spv_add(&c, 115, x, 1, NOW));
    mine(h[14], T0 + 600 * 9 + 1, BITS, 15, x);
    spv_chain c2 = c;
    c2.hdr = malloc(c.cap * 80), memcpy(c2.hdr, c.hdr, c.n * 80);
    CHECK(spv_add(&c2, 115, x, 1, NOW));                                          /* one second above the median: taken */
    spv_free(&c2);
    mine(h[14], NOW + 7201, BITS, 15, x);
    CHECK(!spv_add(&c, 115, x, 1, NOW));                                          /* from the future */
    mine(h[14], NOW + 7200, BITS, 15, x);
    CHECK(spv_tip(&c, &tip) && tip == 114);
    /* two valid headers of which the second is wrong: none taken */
    memcpy(f[0], h[15], 80), memcpy(f[1], h[16], 80), unmine(f[1]);
    CHECK(!spv_add(&c, 115, f[0], 2, NOW) && spv_tip(&c, &tip) && tip == 114);
    CHECK(spv_add(&c, 115, h[15], 15, NOW) && spv_tip(&c, &tip) && tip == 129);
    /* a branch from block 120: with as much work it is not taken, with more it is; an invalid longer one is not */
    for (int i = 20; i < N; i++) mine(i == 20 ? h[19] : f[i - 1], T0 + 600 * (uint32_t)i + 7, BITS, 1000 + (uint32_t)i, f[i]);
    CHECK(spv_add(&c, 120, f[20], 10, NOW) && !memcmp(spv_header(&c, 125), h[25], 80));
    memcpy(x, f[30], 80), unmine(f[30]);
    CHECK(!spv_add(&c, 120, f[20], 11, NOW) && !memcmp(spv_header(&c, 125), h[25], 80));
    memcpy(f[30], x, 80);
    CHECK(spv_add(&c, 120, f[20], 11, NOW) && spv_tip(&c, &tip) && tip == 130 && !memcmp(spv_header(&c, 125), f[25], 80));
    CHECK(!memcmp(spv_header(&c, 119), h[19], 80) && !spv_header(&c, 131) && !spv_header(&c, 99));
    /* the file: read back whole; one that belongs to another checkpoint or is damaged is not taken */
    const char *path = "build/peer/t_spv_headers.bin";
    CHECK(spv_save(&c, path) && spv_save(&c, path));          /* the second over the file of the first */
    spv_chain d;
    CHECK(spv_init(&d, BTC_REGTEST, 100, cp) && spv_load(&d, path, NOW) && d.n == c.n && !memcmp(d.hdr, c.hdr, c.n * 80));
    spv_free(&d);
    CHECK(spv_init(&d, BTC_REGTEST, 101, cp) && !spv_load(&d, path, NOW) && d.n == 0);
    FILE *fp = fopen(path, "r+b");
    CHECK(fp && fseek(fp, 49 + 80 * 20 + 3, SEEK_SET) == 0 && fputc(0x55, fp) != EOF && fclose(fp) == 0);
    CHECK(spv_init(&d, BTC_REGTEST, 100, cp) && !spv_load(&d, path, NOW) && d.n == 20);   /* the headers before the damage are valid */
    spv_free(&d);
    remove(path);
    spv_free(&c);
}

/* the Merkle root of n transaction ids as Bitcoin builds it (the last one doubled on an odd level), and the branch of id `pos` */
static void merkle(const uint8_t (*ids)[32], size_t n, size_t pos, uint8_t root[32], uint8_t (*branch)[32], size_t *depth)
{
    uint8_t level[16][32], cat[64];
    memcpy(level, ids, n * 32);
    *depth = 0;
    while (n > 1) {
        if (n & 1) memcpy(level[n], level[n - 1], 32), n++;
        memcpy(branch[(*depth)++], level[pos ^ 1], 32);
        for (size_t i = 0; i < n; i += 2) {
            memcpy(cat, level[i], 32), memcpy(cat + 32, level[i + 1], 32);
            pz_sha256d(cat, 64, level[i / 2]);
        }
        n /= 2, pos /= 2;
    }
    memcpy(root, level[0], 32);
}

static void test_proof(void)
{
    uint8_t ids[11][32], root[32], branch[SPV_BRANCH_MAX][32], h0[80], h1[80], cp[32];
    size_t depth;
    for (int i = 0; i < 11; i++) pz_sha256((const uint8_t *)&i, sizeof i, ids[i]);
    mine(NULL, T0, BITS, 0, h0);
    pz_sha256d(h0, 80, cp);
    for (size_t n = 1; n <= 11; n++)
        for (size_t pos = 0; pos < n; pos++) {
            spv_chain c;
            merkle((const uint8_t(*)[32])ids, n, pos, root, branch, &depth);
            mine(h0, T0 + 600, BITS, 1, h1);
            memcpy(h1 + 36, root, 32);                                             /* the root of the block, then its work again */
            unmine(h1);
            uint8_t target[32], hash[32], rev[32];
            CHECK(spv_target(BITS, target));
            for (uint32_t nonce = 0;; nonce++) {
                for (int b = 0; b < 4; b++) h1[76 + b] = (uint8_t)(nonce >> (8 * b));
                pz_sha256d(h1, 80, hash);
                for (int b = 0; b < 32; b++) rev[b] = hash[31 - b];
                if (memcmp(rev, target, 32) <= 0) break;
            }
            CHECK(spv_init(&c, BTC_REGTEST, 0, cp) && spv_add(&c, 0, h0, 1, NOW) && spv_add(&c, 1, h1, 1, NOW));
            CHECK(spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos));
            CHECK(!spv_proof(&c, 0, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos));   /* another block */
            CHECK(!spv_proof(&c, 2, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos));   /* a block we do not have */
            /* (with an odd level Bitcoin doubles the last hash, so the last transaction also proves at its twin's position: only
             * a full tree gives each transaction one position) */
            if ((n & (n - 1)) == 0 && n > 1) CHECK(!spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos ^ 1));
            if (n > 1) {
                CHECK(!spv_proof(&c, 1, ids[(pos + 1) % n], (const uint8_t(*)[32])branch, depth, (uint32_t)pos));     /* another transaction */
                branch[depth - 1][7] ^= 1;
                CHECK(!spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos));               /* a changed branch */
                CHECK(!spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, depth - 1, (uint32_t)pos));           /* a short one */
            }
            CHECK(!spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, depth, (uint32_t)pos | 1u << depth));     /* a position outside the tree */
            CHECK(!spv_proof(&c, 1, ids[pos], (const uint8_t(*)[32])branch, SPV_BRANCH_MAX + 1, (uint32_t)pos));
            spv_free(&c);
        }
}

int main(void)
{
    test_main_chain();
    test_chain();
    test_proof();
    TEST_DONE("t_spv");
}
