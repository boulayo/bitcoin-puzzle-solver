/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_batch.c - the batch HASH160: every implementation the CPU can run against the scalar code and against values computed by Python,
 * batches of every awkward size, unaligned buffers, the choice of the implementation, and use from several threads while it changes. */
#include <pzcore/cpu.h>
#include <pzcore/hash.h>
#include <pzcore/hash160_batch.h>
#include <pthread.h>
#include "testutil.h"
#include "vectors.h"

#define MAXN 1100

/* compares the batch result for n random keys with the scalar HASH160, with data and results at the given byte offsets */
static void compare_random(size_t n, size_t in_off, size_t out_off)
{
    static uint8_t xs_buf[MAXN * 32 + 16], par_buf[MAXN + 16], out_buf[MAXN * 20 + 16 + 4];
    uint8_t *xs = xs_buf + in_off, *par = par_buf + in_off, *out = out_buf + out_off;
    tu_fill(xs, n * 32);
    tu_fill(par, n);
    memset(out_buf, 0xcd, sizeof out_buf);
    pz_hash160_x_batch(n, xs, par, out);
    for (size_t i = 0; i < n; i++) {
        uint8_t want[20];
        pz_hash160_pub33(par[i], xs + 32 * i, want);
        if (memcmp(want, out + 20 * i, 20) != 0) { CHECK(!"hash differs"); return; }
    }
    /* nothing before or after the n results was written */
    for (size_t i = 0; i < out_off; i++) CHECK(out_buf[i] == 0xcd);
    for (size_t i = out_off + 20 * n; i < sizeof out_buf; i++) CHECK(out_buf[i] == 0xcd);
}

static void test_impl(const char *name)
{
    CHECK(pz_hash160_batch_select(name));
    CHECK_STR(pz_hash160_batch_impl(), name);
    const unsigned lanes = pz_hash160_batch_lanes();
    CHECK(lanes == 1 || lanes == 4 || lanes == 8 || lanes == 16);
    for (size_t n = 0; n <= 70; n++) compare_random(n, 0, 0);
    const size_t odd[] = {lanes - 1u, lanes, lanes + 1u, 2u * lanes - 1u, 2u * lanes, 2u * lanes + 1u, 100, 333, MAXN};
    for (size_t i = 0; i < sizeof odd / sizeof *odd; i++)
        for (size_t off = 0; off < 8; off += 3) compare_random(odd[i], off, (off * 5) % 8);

    /* values computed by Python: the x coordinate of k*G with the parity of y */
    uint8_t xs[sizeof EC_VEC / sizeof *EC_VEC * 32], par[sizeof EC_VEC / sizeof *EC_VEC], out[sizeof EC_VEC / sizeof *EC_VEC * 20];
    const size_t nv = sizeof EC_VEC / sizeof *EC_VEC;
    for (size_t i = 0; i < nv; i++) {
        uint8_t y[32];
        CHECK(tu_unhex(EC_VEC[i].x, xs + 32 * i, 32) == 32 && tu_unhex(EC_VEC[i].y, y, 32) == 32);
        par[i] = y[31] & 1;
    }
    pz_hash160_x_batch(nv, xs, par, out);
    for (size_t i = 0; i < nv; i++) {
        char hx[41];
        tu_hex(out + 20 * i, 20, hx);
        CHECK_STR(hx, EC_VEC[i].h160);
    }
    /* the parity byte: its lowest bit decides, so 0/1 and 2/3 are the same */
    {
        uint8_t x[32], a[20], b[20];
        tu_fill(x, 32);
        for (int p = 0; p < 256; p++) {
            uint8_t pb = (uint8_t)p;
            pz_hash160_x_batch(1, x, &pb, a);
            pz_hash160_pub33((uint8_t)(p & 1), x, b);
            CHECK(!memcmp(a, b, 20));
        }
    }
    /* the lane kernel, for the implementations that have one */
    pz_hash160_lanes_fn k = pz_hash160_batch_kernel();
    if (!strcmp(name, "scalar")) {
        CHECK(k == NULL);
    } else {
        CHECK(k != NULL);
        uint32_t X[8 * 16] PZ_ALIGN(64), pfx[16] PZ_ALIGN(64), res[5 * 16] PZ_ALIGN(64);
        uint8_t keys[16][32];
        tu_fill(&keys[0][0], sizeof keys);
        for (unsigned l = 0; l < lanes; l++) {
            for (unsigned w = 0; w < 8; w++)
                X[w * lanes + l] = (uint32_t)keys[l][4 * w] << 24 | (uint32_t)keys[l][4 * w + 1] << 16 | (uint32_t)keys[l][4 * w + 2] << 8 | keys[l][4 * w + 3];
            pfx[l] = 2u + (l & 1u);
        }
        k(X, pfx, res);
        for (unsigned l = 0; l < lanes; l++) {
            uint8_t want[20], got[20];
            pz_hash160_pub33((uint8_t)(l & 1u), keys[l], want);
            for (unsigned w = 0; w < 5; w++) for (unsigned b = 0; b < 4; b++) got[4 * w + b] = (uint8_t)(res[w * lanes + l] >> (8 * b));
            CHECK(!memcmp(want, got, 20));
        }
    }
}

/* ---- threads: batches keep coming out right while another thread changes the implementation ---- */
static volatile int stop_flag;
static int thread_bad[4];
static const char *g_names[8];
static size_t g_nnames;

static void *worker(void *arg)
{
    const int id = (int)(intptr_t)arg;
    uint8_t xs[37 * 32], par[37], out[37 * 20], want[20];
    uint64_t state = 0x1234567ULL * (uint64_t)(id + 1);
    int rounds = 0;
    while (!stop_flag || rounds < 50) {
        for (size_t i = 0; i < sizeof xs; i++) { state = state * 6364136223846793005ULL + 1442695040888963407ULL; xs[i] = (uint8_t)(state >> 33); }
        for (size_t i = 0; i < sizeof par; i++) { state = state * 6364136223846793005ULL + 1442695040888963407ULL; par[i] = (uint8_t)(state >> 33); }
        pz_hash160_x_batch(37, xs, par, out);
        for (size_t i = 0; i < 37; i++) {
            pz_hash160_pub33(par[i], xs + 32 * i, want);
            if (memcmp(want, out + 20 * i, 20)) thread_bad[id]++;
        }
        rounds++;
    }
    return NULL;
}

static void *switcher(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) pz_hash160_batch_select(g_names[(size_t)i % g_nnames]);
    stop_flag = 1;
    return NULL;
}

int main(void)
{
    const char *names[8];
    const size_t n = pz_hash160_batch_list(names, 8);
    CHECK(n >= 2 && n <= 8);
    CHECK_STR(names[n - 1], "scalar");
    CHECK_STR(names[n - 2], "portable");
    for (size_t i = 0; i < n; i++) g_names[i] = names[i];
    g_nnames = n;
    printf("t_batch: implementations on this CPU:");
    for (size_t i = 0; i < n; i++) printf(" %s", names[i]);
    printf("\n");

    /* the default is the best one */
    CHECK(pz_hash160_batch_select(NULL));
    CHECK_STR(pz_hash160_batch_impl(), names[0]);
    CHECK(pz_hash160_batch_select("auto"));
    CHECK_STR(pz_hash160_batch_impl(), names[0]);
    /* the list agrees with the CPU */
    {
        bool has512 = false, has2 = false, hasneon = false;
        for (size_t i = 0; i < n; i++) {
            has512 |= !strcmp(names[i], "avx512");
            has2 |= !strcmp(names[i], "avx2");
            hasneon |= !strcmp(names[i], "neon") || !strcmp(names[i], "neon2");
        }
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
        CHECK(has512 == pz_cpu_has_avx512() && has2 == pz_cpu_has_avx2() && !hasneon);
#elif defined(__aarch64__) && !defined(PZ_NO_ARM_SHA2)
        CHECK(hasneon == pz_cpu_has_sha2() && !has512 && !has2);
#else
        CHECK(!has512 && !has2 && !hasneon);
#endif
    }
    /* each implementation, then a request for something that cannot be had changes nothing */
    for (size_t i = 0; i < n; i++) test_impl(names[i]);
    CHECK(pz_hash160_batch_select("portable"));
    CHECK(!pz_hash160_batch_select("bogus"));
    CHECK(!pz_hash160_batch_select(""));
    CHECK(!pz_hash160_batch_select("AVX2"));
    CHECK_STR(pz_hash160_batch_impl(), "portable");
    if (!pz_cpu_has_avx512()) { CHECK(!pz_hash160_batch_select("avx512")); CHECK_STR(pz_hash160_batch_impl(), "portable"); }
    if (!pz_cpu_has_sha2()) { CHECK(!pz_hash160_batch_select("neon")); CHECK(!pz_hash160_batch_select("neon2")); }
    /* names can be listed with too little room, or with none */
    {
        const char *two[2];
        CHECK(pz_hash160_batch_list(two, 2) == n);
        CHECK(pz_hash160_batch_list(NULL, 0) == n);
        CHECK_STR(two[0], names[0]);
    }
    /* an empty batch reads nothing */
    pz_hash160_x_batch(0, NULL, NULL, NULL);

    /* use from several threads while the implementation is being switched */
    {
        pthread_t w[4], sw;
        for (intptr_t i = 0; i < 4; i++) pthread_create(&w[i], NULL, worker, (void *)i);
        pthread_create(&sw, NULL, switcher, NULL);
        pthread_join(sw, NULL);
        for (int i = 0; i < 4; i++) pthread_join(w[i], NULL);
        for (int i = 0; i < 4; i++) CHECK(thread_bad[i] == 0);
    }
    pz_hash160_batch_select(NULL);
    TEST_DONE("t_batch");
}
