/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_seed.c - the seed generator: the cutoffs of its health tests recomputed from SP 800-90B, the tests on good and broken data, the
 * TPM's answers and a file that stands for its device, known answers of the mixing, every source broken in turn, the longer jitter
 * collection, the caller's source, a malicious source against the mixing, and the statistics of a few thousand seeds.
 * It includes seed.c with PZ_SEED_TEST, which adds the fault hooks (g_fault) the library does not have. -v prints every case. */
#define PZ_SEED_TEST 1
#include "../src/seed.c"
#include "testutil.h"
#if defined(__linux__)
#include <sys/wait.h>
#endif

static int g_verbose;
static char g_tpm[600], g_nodev[600];          /* the file that stands for the TPM, and a path where there is none */

static void reset(void)
{
    memset(&g_fault, 0, sizeof g_fault);
    g_fault.tpm_path = g_nodev;                /* no TPM unless a case makes one: the same results on every machine */
}

/* the OS's random bytes, as the generator reads them (getentropy gives at most 256 bytes per call) */
static void os_bytes(uint8_t *p, size_t n)
{
    char method[32], why[128];
    CHECK(os_fill(p, n, method, sizeof method, why, sizeof why));
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t a = 0;
    for (size_t i = 0; i < n; i++) a |= p[i];
    return a == 0;
}

static unsigned lead_zero_bits(const uint8_t *p)
{
    unsigned n = 0;
    for (int i = 0; i < 32 && p[i] == 0; i++) n += 8;
    return n < 256 ? n + (unsigned)__builtin_clz(p[n / 8]) - 24 : n;
}

/* ---- the cutoffs and the health tests ---- */

/* P(X >= k) for X ~ Binomial(n, p), summed in log space */
static double tail(unsigned n, double p, unsigned k)
{
    double s = 0;
    for (unsigned j = k; j <= n; j++)
        s += exp(lgamma(n + 1.0) - lgamma(j + 1.0) - lgamma(n - j + 1.0) + j * log(p) + (n - j) * log1p(-p));
    return s;
}

/* the cutoff C of the adaptive proportion test (and of the byte count, with mult = 256 values): P(X >= C) <= alpha < P(X >= C - 1) */
static bool is_cutoff(unsigned c, unsigned n, double p, double alpha, double mult)
{
    return mult * tail(n, p, c) <= alpha && mult * tail(n, p, c - 1) > alpha;
}

static void test_cutoffs(void)
{
    CHECK(B_RCT == 1 + (int)ceil(40 / 8.0) && J_RCT == 1 + (int)ceil(30 / 0.5));
    CHECK(is_cutoff(B_APT, APT_W, 1.0 / 256, ldexp(1, -40), 1));
    CHECK(is_cutoff(J_APT, APT_W, pow(2, -0.5), ldexp(1, -30), 1));
    CHECK(is_cutoff(B_FREQ, APT_W, 1.0 / 256, ldexp(1, -40), 256));
    double lo = 0, hi = 20;                    /* the two-sided normal bound: erfc(z / sqrt 2) = 2^-40 */
    for (int i = 0; i < 100; i++) {
        const double mid = (lo + hi) / 2;
        if (erfc(mid / sqrt(2)) > ldexp(1, -40)) lo = mid;
        else hi = mid;
    }
    CHECK(B_Z2X100 == (int)floor(hi * hi * 100));
}

static void test_bytes(void)
{
    uint8_t b[1024];
    char why[128];
    unsigned alarms = 0;
    for (int i = 0; i < 2000; i++) {           /* the OS's bytes raise no false alarm */
        os_bytes(b, sizeof b);
        alarms += !bytes_ok(b, sizeof b, why, sizeof why);
    }
    CHECK(alarms == 0);
    tu_fill(b, sizeof b);
    for (size_t i = 1; i < sizeof b; i++) if (b[i] == b[i - 1]) b[i] ^= 0x80;     /* no run at all */
    memset(b + 100, 0x33, B_RCT - 1);
    b[99] = b[100 + B_RCT - 1] = 0x44;
    CHECK(bytes_ok(b, sizeof b, why, sizeof why));                                   /* a run one shorter than the cutoff */
    b[100 + B_RCT - 1] = 0x33;
    CHECK(!bytes_ok(b, sizeof b, why, sizeof why) && strstr(why, "repetition"));
    tu_fill(b, sizeof b);
    for (size_t i = 8; i < sizeof b; i += 8) b[i] = b[0];                          /* 1/8 of the window equals its first byte */
    CHECK(!bytes_ok(b, sizeof b, why, sizeof why) && strstr(why, "adaptive"));
    tu_fill(b, sizeof b);
    for (size_t i = 0; i < sizeof b; i += 8) b[i + 4] = b[i + 5] = b[i + 6] = b[i + 7] = 0;   /* the upper half of each word zero */
    CHECK(!bytes_ok(b, sizeof b, why, sizeof why) && (strstr(why, "frequency") || strstr(why, "repetition")));
    tu_fill(b, sizeof b);
    for (size_t i = 0; i < sizeof b; i++) b[i] &= 0x7f;                            /* one bit in eight stuck at zero */
    CHECK(!bytes_ok(b, sizeof b, why, sizeof why) && strstr(why, "monobit"));
}

static void test_tpm_parse(void)
{
    uint8_t r[TPM_RESP] = {0x80, 0x01, 0, 0, 0, TPM_RESP, 0, 0, 0, 0, 0, TPM_CHUNK};
    char why[96];
    CHECK(tpm_parse(r, sizeof r, why, sizeof why));
    CHECK(!tpm_parse(r, sizeof r - 1, why, sizeof why) && strstr(why, "malformed"));      /* the size field disagrees */
    r[1] = 0x02;
    CHECK(!tpm_parse(r, sizeof r, why, sizeof why) && strstr(why, "malformed"));          /* TPM_ST_SESSIONS: not what was asked */
    r[1] = 0x01;
    r[11] = 16;
    CHECK(!tpm_parse(r, sizeof r, why, sizeof why) && strstr(why, "where"));
    const uint8_t e[10] = {0x80, 0x01, 0, 0, 0, 10, 0, 0, 0x09, 0x22};                    /* TPM_RC_RETRY */
    CHECK(!tpm_parse(e, sizeof e, why, sizeof why) && strstr(why, "0x922"));
    CHECK(!tpm_parse(e, 6, why, sizeof why));
}

/* ---- the mixing ---- */

static pz_seed_source rec(const char *name, const char *method, pz_seed_state st, unsigned bits)
{
    pz_seed_source s;
    memset(&s, 0, sizeof s);
    snprintf(s.name, sizeof s.name, "%s", name);
    snprintf(s.method, sizeof s.method, "%s", method);
    s.state = st;
    s.bits = bits;
    return s;
}

/* The values were computed with Python's hmac and hashlib from the encoding of seed.h:
 *   rec = lambda i, used, name, method, bits, data: bytes([i, used, len(name)]) + name + bytes([len(method)]) + method
 *         + (bits if used else 0).to_bytes(4, 'big') + len(data if used else b'').to_bytes(8, 'big') + (data if used else b'')
 *   ikm = bytes([11]) + b'pzseed1/ikm' + bytes([5]) + b''.join(rec(i, *r) for i, r in enumerate(records))
 *   seed = hmac(hmac(salt, ikm), b'pzseed1/seed' + b'\x01') */
static void test_mix(void)
{
    uint8_t salt[32], cpu[64], os[32], seed[32], seed2[32], big[1024];
    char hx[65];
    static const char gpu[] = "gpu timing jitter";
    for (int i = 0; i < 64; i++) cpu[i] = (uint8_t)(3 * i);
    for (int i = 0; i < 32; i++) salt[i] = (uint8_t)i, os[i] = (uint8_t)(255 - i);
    pz_seed_source s[PZ_SEED_NSRC] = {rec("cpu-rng", "RDSEED", PZ_SEED_USED, 128), rec("tpm", "", PZ_SEED_ABSENT, 0),
                                      rec("gpu", "callback", PZ_SEED_USED, 100), rec("jitter", "rdtsc, walk 256", PZ_SEED_EXCLUDED, 0),
                                      rec("os", "getrandom", PZ_SEED_USED, 256)};
    chunk raw[PZ_SEED_NSRC] = {{cpu, 64}, {NULL, 0}, {(const uint8_t *)gpu, 17}, {(const uint8_t *)"ignored", 7}, {os, 32}};
    mix(salt, s, raw, seed);
    tu_hex(seed, 32, hx);
    CHECK_STR(hx, "234b2db40d4271a81990de34835190f5d2a1c04ce6af0e66658936c8d0c8be92");
    /* the same through pz_hkdf_sha256 over the IKM built here, record by record */
    uint8_t ikm[512];
    size_t n = 0;
    ikm[n++] = 11;
    memcpy(ikm + n, "pzseed1/ikm", 11), n += 11;
    ikm[n++] = PZ_SEED_NSRC;
    for (int i = 0; i < PZ_SEED_NSRC; i++) {
        const bool used = s[i].state == PZ_SEED_USED;
        const size_t len = used ? raw[i].len : 0;
        ikm[n++] = (uint8_t)i;
        ikm[n++] = used;
        ikm[n++] = (uint8_t)strlen(s[i].name);
        memcpy(ikm + n, s[i].name, strlen(s[i].name)), n += strlen(s[i].name);
        ikm[n++] = (uint8_t)strlen(s[i].method);
        memcpy(ikm + n, s[i].method, strlen(s[i].method)), n += strlen(s[i].method);
        for (int k = 3; k >= 0; k--) ikm[n++] = (uint8_t)((used ? s[i].bits : 0) >> (8 * k));
        for (int k = 7; k >= 0; k--) ikm[n++] = (uint8_t)((uint64_t)len >> (8 * k));
        if (len) memcpy(ikm + n, raw[i].data, len), n += len;
    }
    CHECK(pz_hkdf_sha256(salt, 32, ikm, n, (const uint8_t *)"pzseed1/seed", 12, seed2, 32) && !memcmp(seed, seed2, 32));
    /* only the OS */
    memset(salt, 0, 32);
    memset(big, 0, sizeof big);
    pz_seed_source o[PZ_SEED_NSRC] = {rec("cpu-rng", "none", PZ_SEED_ABSENT, 0), rec("tpm", "", PZ_SEED_ABSENT, 0),
                                      rec("caller", "", PZ_SEED_ABSENT, 0), rec("jitter", "rdtsc, walk 256", PZ_SEED_EXCLUDED, 0),
                                      rec("os", "getrandom", PZ_SEED_USED, 256)};
    chunk raw2[PZ_SEED_NSRC] = {{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}, {big, sizeof big}};
    mix(salt, o, raw2, seed);
    tu_hex(seed, 32, hx);
    CHECK_STR(hx, "7bed9677d677031eec7de6be9440290a439eefbd3947c6c936fb0064583e6079");
    /* the salt: the label, then each field as tag, length, bytes */
    uint8_t e[1024];
    size_t m = 12;
    memcpy(e, "pzseed1/salt", 12);
    m = ctx_put(e, m, 1, "1234");
    m = ctx_put(e, m, 5, "host");
    m = ctx_put(e, m, 8, "");
    pz_sha256(e, m, salt);
    tu_hex(salt, 32, hx);
    CHECK_STR(hx, "bfcc2f5148dcdba5ef547d7ac1eb5fe1d12c7f6ad90d2c572267082beb6564f4");
}

/* ---- the TPM file and the caller's source ---- */

enum { TPM_NONE, TPM_GOOD, TPM_RC, TPM_SAME, TPM_SHORT, TPM_BADTAG };

/* A file laid out as the device would be used: each command is written over a 12-byte slot, and the read that follows gets the
 * answer after it. The commands the generator wrote can be read back from the slots. */
static void fake_tpm(int kind)
{
    if (kind == TPM_NONE) {
        g_fault.tpm_path = g_nodev;
        return;
    }
    FILE *f = fopen(g_tpm, "wb");
    CHECK(f != NULL);
    if (!f) return;
    for (int i = 0; i < TPM_CALLS; i++) {
        uint8_t r[TPM_RESP + 12] = {0}, *a = r + 12;
        size_t n = TPM_RESP;
        a[0] = 0x80, a[1] = 0x01, a[5] = TPM_RESP, a[11] = TPM_CHUNK;
        if (kind == TPM_SAME) memset(a + 12, 0x5c, TPM_CHUNK);
        else tu_fill(a + 12, TPM_CHUNK);
        if (kind == TPM_RC) a[5] = 10, a[8] = 0x01, a[9] = 0x01, n = 10;                /* TPM_RC_FAILURE */
        if (kind == TPM_SHORT) a[5] = 12 + 16, a[11] = 16, n = 12 + 16;
        if (kind == TPM_BADTAG) a[1] = 0x02;
        fwrite(r, 1, 12 + n, f);
        if (n != TPM_RESP) break;              /* the end of the file ends the answer */
    }
    fclose(f);
    g_fault.tpm_path = g_tpm;
}

static bool tpm_commands_ok(void)
{
    uint8_t b[TPM_CALLS * (12 + TPM_RESP)];
    FILE *f = fopen(g_tpm, "rb");
    if (!f) return false;
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    bool ok = n == sizeof b;
    for (int i = 0; i < TPM_CALLS && ok; i++) ok = !memcmp(b + i * (12 + TPM_RESP), tpm_cmd, 12);
    return ok;
}

enum { C_NONE, C_GOOD, C_FAIL, C_CONST, C_GREEDY, C_ZEROLEN };
static bool caller_src(void *arg, uint8_t *buf, size_t len, char *why, size_t wn)
{
    const int kind = *(const int *)arg;
    if (kind == C_FAIL) return why_set(why, wn, "the GPU counter did not move");
    if (kind == C_CONST) memset(buf, 7, len);
    else tu_fill(buf, len);
    return true;
}

/* ---- the cases ---- */

typedef struct {
    const char *name;
    int os, hw, jit, tpm, caller;
    unsigned flags, max_ms;
    pz_seed_status rc;
    int cpu_st, tpm_st, jit_st, os_st;        /* the expected states, -1: any */
    const char *why;                           /* a part of a reason, alternatives separated by | */
} fcase;

static const char *const st_name[] = {"absent", "used", "excluded"};

static bool reason_has(const pz_seed_report *r, const char *alts)
{
    char buf[128];
    for (const char *p = alts; *p;) {
        const size_t n = strcspn(p, "|");
        snprintf(buf, sizeof buf, "%.*s", (int)n, p);
        for (int i = 0; i < PZ_SEED_NSRC; i++) if (strstr(r->src[i].reason, buf)) return true;
        if (strstr(r->error, buf)) return true;
        p += n + (p[n] == '|');
    }
    return false;
}

static pz_seed_status run_case(const fcase *c, uint8_t seed[32], pz_seed_report *r)
{
    reset();
    g_fault.os = c->os, g_fault.hw = c->hw, g_fault.jit = c->jit, g_fault.key = 0x4141414141414141ULL;
    if (c->jit && c->jit != JF_FIRSTBAD) g_fault.walk = 16;      /* simulated timers: the walk itself does not matter */
    fake_tpm(c->tpm);
    int kind = c->caller;
    pz_seed_opts o = {NULL, &kind, "gpu", 512, 128, c->flags, c->max_ms};
    if (c->caller) o.extra = caller_src;
    if (c->caller == C_GREEDY) o.extra_bits = 300;
    if (c->caller == C_ZEROLEN) o.extra_len = 0;
    memset(seed, 0x77, 32);
    return pz_seed_generate(seed, &o, r);
}

static void test_cases(void)
{
    enum { U = PZ_SEED_USED, X = PZ_SEED_EXCLUDED, A = PZ_SEED_ABSENT };
    static const fcase cases[] = {
        /* the OS: mandatory */
        {"OS call fails (EIO)", OSF_FAIL, 0, 0, 0, 0, 0, 0, PZ_SEED_E_OS, -1, -1, -1, X, "failed (errno 5)"},
        {"OS short read", OSF_SHORT, 0, 0, 0, 0, 0, 0, PZ_SEED_E_OS, -1, -1, -1, X, "short read"},
        {"OS says success, writes nothing", OSF_ZERO, 0, 0, 0, 0, 0, 0, PZ_SEED_E_OS, -1, -1, -1, X, "same 512"},
        {"OS gives the same block twice", OSF_REPEAT, 0, 0, 0, 0, 0, 0, PZ_SEED_E_OS, -1, -1, -1, X, "same 512"},
        {"OS every other byte zero", OSF_HALFZERO, 0, 0, 0, 0, 0, 0, PZ_SEED_E_OS, -1, -1, -1, X, "adaptive proportion"},
#if defined(__linux__)
        {"OS without getrandom (ENOSYS)", OSF_ENOSYS, 0, 0, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, NULL},
#endif
        {"OS fails, all the rest good", OSF_FAIL, 0, 0, TPM_GOOD, C_GOOD, 0, 0, PZ_SEED_E_OS, U, U, U, X, NULL},
        {"OS attacker-known (passes)", OSF_ATTACKER, 0, 0, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, NULL},
        /* the CPU's generator: excluded alone */
        {"cpu-rng all ones (AMD erratum)", 0, HWF_ALLONES, 0, 0, 0, 0, 0, PZ_SEED_OK, X, A, U, U, "all ones"},
        {"cpu-rng never sets the carry", 0, HWF_NOCARRY, 0, 0, 0, 0, 0, PZ_SEED_OK, X, A, U, U, "gave no word"},
        {"cpu-rng repeats 4 values", 0, HWF_REPEAT, 0, 0, 0, 0, 0, PZ_SEED_OK, X, A, U, U, "repeated"},
        {"cpu-rng zero with the carry", 0, HWF_ZERO, 0, 0, 0, 0, 0, PZ_SEED_OK, X, A, U, U, "zero"},
        {"cpu-rng upper half zero", 0, HWF_HALFZERO, 0, 0, 0, 0, 0, PZ_SEED_OK, X, A, U, U, "frequency|repetition"},
        {"RDSEED starved, RDRAND works", 0, HWF_SEEDFAIL, 0, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, "gave no word"},
        {"cpu-rng attacker-known (passes)", 0, HWF_ATTACKER, 0, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, NULL},
        {"cpu-rng absent", 0, HWF_ABSENT, 0, 0, 0, 0, 0, PZ_SEED_OK, A, A, U, U, NULL},
        /* jitter: excluded alone, the TPM gives the 128 bits */
        {"jitter timer constant", 0, 0, JF_CONST, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "does not advance"},
        {"jitter timer runs backwards", 0, 0, JF_BACK, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "not monotonic"},
        {"jitter constant delta", 0, 0, JF_STEP, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "too little variation"},
        {"jitter two deltas in turn", 0, 0, JF_ALTERNATE, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "live estimate"},
        {"jitter 4096-tick timer", 0, 0, JF_COARSE, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "variation|repetition"},
        {"jitter 1 random bit per sample", 0, 0, JF_LOWENT, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, X, U, "live estimate"},
        {"jitter attacker-known (passes)", 0, 0, JF_ATTACKER, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, NULL},
        /* the TPM: used, excluded or absent alone */
        {"TPM good", 0, 0, 0, TPM_GOOD, 0, 0, 0, PZ_SEED_OK, U, U, U, U, "8 commands"},
        {"TPM error code", 0, 0, 0, TPM_RC, 0, 0, 0, PZ_SEED_OK, U, X, U, U, "response code 0x101"},
        {"TPM the same answer each time", 0, 0, 0, TPM_SAME, 0, 0, 0, PZ_SEED_OK, U, X, U, U, "the same"},
        {"TPM short answer", 0, 0, 0, TPM_SHORT, 0, 0, 0, PZ_SEED_OK, U, X, U, U, "16 bytes where 32"},
        {"TPM malformed answer", 0, 0, 0, TPM_BADTAG, 0, 0, 0, PZ_SEED_OK, U, X, U, U, "malformed"},
        {"no TPM", 0, 0, 0, TPM_NONE, 0, 0, 0, PZ_SEED_OK, U, A, U, U, "no "},
        /* jitter and TPM short: the longer collection, then the end */
        {"jitter fails, no TPM: longer, then none", 0, 0, JF_ALTERNATE, 0, 0, 0, 150, PZ_SEED_E_SHORT, U, A, X, U, "rounds|another computer"},
        {"jitter timer dead, no TPM", 0, 0, JF_CONST, 0, 0, 0, 0, PZ_SEED_E_SHORT, U, A, X, U, "does not advance"},
        {"jitter first round bad, no TPM", 0, 0, JF_FIRSTBAD, 0, 0, 0, 0, PZ_SEED_OK, U, A, U, U, "round 2"},
        {"jitter fails, no TPM, degraded allowed", 0, 0, JF_ALTERNATE, 0, 0, PZ_SEED_ALLOW_DEGRADED, 50, PZ_SEED_OK, U, A, X, U, NULL},
        {"cpu-rng and jitter fail, no TPM", 0, HWF_ALLONES, JF_STEP, 0, 0, 0, 50, PZ_SEED_E_SHORT, X, A, X, U, "give 0 bits"},
        /* the caller's source */
        {"caller 128 bits, jitter fails, no TPM", 0, 0, JF_STEP, 0, C_GOOD, 0, 0, PZ_SEED_OK, U, A, X, U, "128 bits claimed"},
        {"caller fails its own tests", 0, 0, 0, 0, C_FAIL, 0, 0, PZ_SEED_OK, U, A, U, U, "did not move"},
        {"caller constant output", 0, 0, 0, 0, C_CONST, 0, 0, PZ_SEED_OK, U, A, U, U, "constant output"},
        {"caller claims 300 bits", 0, 0, 0, 0, C_GREEDY, 0, 0, PZ_SEED_OK, U, A, U, U, "300 bits claimed"},
        {"caller of 0 bytes", 0, 0, 0, 0, C_ZEROLEN, 0, 0, PZ_SEED_E_ARG, A, A, A, A, "1 to"},
    };
    unsigned bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const fcase *c = &cases[i];
        uint8_t seed[32];
        pz_seed_report r;
        const pz_seed_status rc = run_case(c, seed, &r);
        const int want[4] = {c->cpu_st, c->tpm_st, c->jit_st, c->os_st}, idx[4] = {PZ_SEED_SRC_CPU, PZ_SEED_SRC_TPM, PZ_SEED_SRC_JITTER, PZ_SEED_SRC_OS};
        bool ok = rc == c->rc;
        for (int k = 0; k < 4; k++) ok &= want[k] < 0 || (int)r.src[idx[k]].state == want[k];
        if (rc == PZ_SEED_OK) ok &= !all_zero(seed, 32) && r.src[PZ_SEED_SRC_OS].bits == 256 && (r.counted >= 128 || r.degraded);
        else ok &= all_zero(seed, 32) && r.error[0];
        if (c->why) ok &= reason_has(&r, c->why);
        /* case by case */
        if (c->tpm == TPM_GOOD && rc == PZ_SEED_OK) ok &= tpm_commands_ok() && r.src[PZ_SEED_SRC_TPM].bits == 128;
        if (c->max_ms == 150) ok &= r.jitter_rounds > 1 && r.src[PZ_SEED_SRC_JITTER].ms >= 150;
        if (c->jit == JF_CONST) ok &= r.jitter_rounds == 0;
        if (c->jit == JF_ALTERNATE && c->tpm == TPM_GOOD) ok &= r.jitter_rounds == 1;      /* 128 bits from the TPM: no longer collection */
        if (c->jit == JF_FIRSTBAD) ok &= r.jitter_rounds == 2;
        if (c->flags & PZ_SEED_ALLOW_DEGRADED) ok &= r.degraded && r.counted == 0;
        if (c->hw == HWF_SEEDFAIL) ok &= !strcmp(r.src[PZ_SEED_SRC_CPU].method, hw_name[1]);
        if (c->os == OSF_ENOSYS) ok &= !strcmp(r.src[PZ_SEED_SRC_OS].method, "/dev/urandom");
        if (c->caller == C_GOOD && c->jit) ok &= r.counted == 128;
        if (c->caller == C_GREEDY) ok &= r.src[PZ_SEED_SRC_CALLER].bits == 256;
        if (c->caller == C_FAIL || c->caller == C_CONST) ok &= r.src[PZ_SEED_SRC_CALLER].state == PZ_SEED_EXCLUDED;
        bad += !ok;
        CHECK(ok);
        if (g_verbose || !ok) {
            printf("  %-42s %s rc %d, counted %3u, rounds %u:", c->name, ok ? "ok  " : "FAIL", rc, r.counted, r.jitter_rounds);
            for (int k = 0; k < PZ_SEED_NSRC; k++) if (r.src[k].state != PZ_SEED_USED || k == PZ_SEED_SRC_TPM) printf(" [%s %s: %s]", r.src[k].name, st_name[r.src[k].state], r.src[k].reason);
            printf("\n");
        }
    }
    printf("  %zu cases of broken, missing and attacker-known sources: %u failed\n", sizeof cases / sizeof *cases, bad);
    reset();
}

/* ---- a normal run, and what the report and the memory keep ---- */

static void test_normal(void)
{
    uint8_t seed[32], seed2[32];
    pz_seed_report r;
    reset();
    const pz_seed_status rc = pz_seed_generate(seed, NULL, &r);
    CHECK(rc == PZ_SEED_OK && !all_zero(seed, 32));
    CHECK(r.src[PZ_SEED_SRC_OS].state == PZ_SEED_USED && r.src[PZ_SEED_SRC_JITTER].state == PZ_SEED_USED && r.counted >= 128 && !r.degraded);
    CHECK(r.jitter_rounds == 1 && r.jitter_h >= 1);
    printf("  this machine: %.1f ms (cpu-rng %s %.2f ms, jitter %.1f ms, os %.3f ms); jitter: %s; memory locked: %s\n", r.ms,
           r.src[PZ_SEED_SRC_CPU].method, r.src[PZ_SEED_SRC_CPU].ms, r.src[PZ_SEED_SRC_JITTER].ms, r.src[PZ_SEED_SRC_OS].ms,
           r.src[PZ_SEED_SRC_JITTER].reason, r.locked ? "yes" : "no");
    if (rc != PZ_SEED_OK || g_verbose)
        for (int i = 0; i < PZ_SEED_NSRC; i++) printf("    %-8s %-8s %-22s %3u bits  %s\n", r.src[i].name, st_name[r.src[i].state], r.src[i].method, r.src[i].bits, r.src[i].reason);
    /* no 4 bytes of the seed anywhere in the report, and no 4 bytes of it in hex in its texts */
    char hx[65], piece[9] = "";
    tu_hex(seed, 32, hx);
    const uint8_t *rb = (const uint8_t *)&r;
    bool leak = false;
    for (int k = 0; k + 4 <= 32; k++) {
        for (size_t i = 0; i + 4 <= sizeof r; i++) leak |= !memcmp(rb + i, seed + k, 4);
        memcpy(piece, hx + 2 * k, 8);
        leak |= strstr(r.context, piece) || strstr(r.error, piece);
        for (int i = 0; i < PZ_SEED_NSRC; i++) leak |= strstr(r.src[i].reason, piece) || strstr(r.src[i].method, piece);
    }
    CHECK(!leak);
    CHECK(pz_seed_generate(seed2, NULL, &r) == PZ_SEED_OK && memcmp(seed, seed2, 32) != 0);
#if defined(__linux__) && defined(MADV_WIPEONFORK)
    bool locked;                               /* a forked child sees the secret pages as zeros */
    uint8_t *p = map_pages(4096, true, &locked);
    memset(p, 0xa5, 4096);
    const pid_t pid = fork();
    if (pid == 0) _exit(all_zero(p, 4096) ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    unmap_pages(p, 4096, locked);
#endif
}

/* ---- a malicious source against the mixing ---- */

static void test_attacker(void)
{
    uint8_t salt[32], os[OS_BYTES], jit[JIT_SAMPLES * 4], hw[64], seed[32], target[32], dg[32], naive[32];
    os_bytes(salt, 32);
    os_bytes(os, sizeof os);
    os_bytes(jit, sizeof jit);
    memset(target, 0, 32);                     /* the attacker wants the seed 0 */
    /* a naive combiner, SHA-256(os || jitter) XOR hw: a generator that knows the other inputs sets the seed to its target */
    uint8_t cat[sizeof os + sizeof jit];
    memcpy(cat, os, sizeof os);
    memcpy(cat + sizeof os, jit, sizeof jit);
    pz_sha256(cat, sizeof cat, dg);
    for (int i = 0; i < 32; i++) hw[i] = dg[i] ^ target[i], naive[i] = dg[i] ^ hw[i];
    memset(hw + 32, 0, 32);
    CHECK(!memcmp(naive, target, 32));
    pz_seed_source s[PZ_SEED_NSRC] = {rec("cpu-rng", "RDSEED", PZ_SEED_USED, 128), rec("tpm", "", PZ_SEED_ABSENT, 0),
                                      rec("caller", "", PZ_SEED_ABSENT, 0), rec("jitter", "rdtsc, walk 256", PZ_SEED_USED, 256),
                                      rec("os", "getrandom", PZ_SEED_USED, 256)};
    chunk raw[PZ_SEED_NSRC] = {{hw, 64}, {NULL, 0}, {NULL, 0}, {jit, sizeof jit}, {os, sizeof os}};
    mix(salt, s, raw, seed);
    const unsigned direct = lead_zero_bits(seed);
    CHECK(direct < 24);
    /* one that sees every other input can only grind: 2^16 tries buy about 16 bits (small records, so that the test is quick) */
    chunk small[PZ_SEED_NSRC] = {{hw, 8}, {NULL, 0}, {NULL, 0}, {jit, 64}, {os, 32}};
    unsigned best = 0;
    for (uint64_t t = 0; t < 1 << 16; t++) {
        memcpy(hw, &t, 8);
        mix(salt, s, small, seed);
        const unsigned z = lead_zero_bits(seed);
        best = z > best ? z : best;
    }
    CHECK(best < 32);
    /* the records carry their lengths: 8 bytes moved from the end of one source to the start of the next change the seed */
    uint8_t joined[64 + 64], a[32], b[32];
    memcpy(joined, hw, 64);
    memcpy(joined + 64, jit, 64);
    chunk r1[PZ_SEED_NSRC] = {{joined, 64}, {NULL, 0}, {NULL, 0}, {joined + 64, 64}, {os, 32}};
    chunk r2[PZ_SEED_NSRC] = {{joined, 56}, {NULL, 0}, {NULL, 0}, {joined + 56, 72}, {os, 32}};
    mix(salt, s, r1, a);
    mix(salt, s, r2, b);
    CHECK(memcmp(a, b, 32) != 0);
    printf("  malicious cpu-rng: sets a XOR combiner to its target; through HKDF the same value gives %u leading zero bits, and 2^16 "
           "tries with every other input known give %u\n", direct, best);
    /* the OS attacker-known and the same in every run, jitter real: the seeds still differ */
    enum { R = 100 };
    uint8_t seeds[R][32];
    long ones = 0;
    bool dup = false;
    for (int i = 0; i < R; i++) {
        pz_seed_report rep;
        reset();
        g_fault.os = OSF_ATTACKER, g_fault.key = 42;
        CHECK(pz_seed_generate(seeds[i], NULL, &rep) == PZ_SEED_OK);
        for (int k = 0; k < 32; k++) ones += __builtin_popcount(seeds[i][k]);
        for (int j = 0; j < i; j++) dup |= !memcmp(seeds[i], seeds[j], 32);
    }
    const double z = (ones - R * 128.0) / sqrt(R * 64.0);
    CHECK(!dup && fabs(z) < 5);
    printf("  OS attacker-known and the same each time, jitter real: %d seeds, %s, ones z = %.2f\n", R, dup ? "DUPLICATES" : "all distinct", z);
    reset();
}

/* ---- statistics: only the OS honest ---- */

static int cmp32(const void *a, const void *b) { return memcmp(a, b, 32); }

static void test_stats(void)
{
    enum { N = 2000 };
    static uint8_t all[N][32];
    unsigned fails = 0;
    for (int i = 0; i < N; i++) {
        pz_seed_report r;
        reset();
        g_fault.hw = HWF_ATTACKER, g_fault.jit = JF_ATTACKER, g_fault.walk = 16, g_fault.key = 7;   /* the same in every run */
        fails += pz_seed_generate(all[i], NULL, &r) != PZ_SEED_OK;
    }
    CHECK(fails == 0);
    long ones = 0, cnt[256] = {0};
    double zmax = 0;
    for (int bit = 0; bit < 256; bit++) {
        long c = 0;
        for (int i = 0; i < N; i++) c += all[i][bit / 8] >> (bit % 8) & 1;
        ones += c;
        const double z = fabs(2.0 * c - N) / sqrt((double)N);
        zmax = z > zmax ? z : zmax;
    }
    for (int i = 0; i < N; i++) for (int k = 0; k < 32; k++) cnt[all[i][k]]++;
    double x2 = 0;
    const double e = N * 32 / 256.0;
    for (int v = 0; v < 256; v++) x2 += (cnt[v] - e) * (cnt[v] - e) / e;
    const double zall = (ones - N * 128.0) / sqrt(N * 64.0), zx2 = (x2 - 255) / sqrt(2 * 255.0);
    qsort(all, N, 32, cmp32);
    unsigned dups = 0;
    for (int i = 1; i < N; i++) dups += !memcmp(all[i], all[i - 1], 32);
    CHECK(dups == 0 && fabs(zall) < 4 && zmax < 4.9 && fabs(zx2) < 4.5);
    printf("  %d seeds with cpu-rng and jitter attacker-known, the OS alone honest: %u duplicates, ones z = %.2f, worst of 256 bits "
           "|z| = %.2f, byte chi-square %.1f (255 df)\n", N, dups, zall, zmax, x2);
    reset();
}

/* ---- the process hardening: last, because it stays ---- */

static void test_harden(void)
{
    uint8_t seed[32];
    pz_seed_report r;
    const pz_seed_opts o = {NULL, NULL, NULL, 0, 0, PZ_SEED_HARDEN, 0};
    reset();
    CHECK(pz_seed_generate(seed, &o, &r) == PZ_SEED_OK);
#if defined(__linux__)
    struct rlimit rl;
    CHECK(r.hardened && getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0 && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
#endif
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    /* the TPM's file goes next to the program */
    const char *slash = strrchr(argv[0], '/'), *bslash = strrchr(argv[0], '\\');
    if (bslash > slash) slash = bslash;
    const int dl = slash ? (int)(slash - argv[0]) : 1;
    snprintf(g_tpm, sizeof g_tpm, "%.*s/t_seed_tpm.bin", dl, slash ? argv[0] : ".");
    snprintf(g_nodev, sizeof g_nodev, "%.*s/t_seed_no_tpm", dl, slash ? argv[0] : ".");
    remove(g_nodev);
    reset();
    const double t0 = now_ms();
    test_cutoffs();
    test_bytes();
    test_tpm_parse();
    test_mix();
    test_normal();
    test_cases();
    test_attacker();
    test_stats();
    test_harden();
    remove(g_tpm);
    printf("  (%.1f s)\n", (now_ms() - t0) / 1e3);
    TEST_DONE("t_seed");
}
