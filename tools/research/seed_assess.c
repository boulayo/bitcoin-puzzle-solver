/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seed_assess.c - offline assessment of the CPU jitter source of the seed generator (lib/pzcore/src/seed.c, included here, so that
 * the samples come from the generator's own collector and calibration) with the min-entropy estimators of NIST SP 800-90B (2018)
 * section 6.3 for non-IID data: 6.3.1 most common value, 6.3.2 collision and 6.3.3 Markov (on the bitstring), 6.3.5 t-tuple, 6.3.6
 * longest repeated substring, 6.3.7 MultiMCW, 6.3.8 lag, 6.3.9 MultiMMC and 6.3.10 LZ78Y prediction. 6.3.4 (compression) is not
 * implemented. As in the standard, H = min(H on the symbols, bits per symbol x H on their bitstring). Nothing here is built into
 * pzcore or pzsolve:
 *
 *   make lib && cc -O2 -Ilib/pzcore/include -o build/seed_assess tools/research/seed_assess.c build/libpzcore.a -lm
 *   build/seed_assess [-n samples] [-b bits] [-o symbols.bin] [--fast]   samples of the collector, every estimator
 *   build/seed_assess -i FILE [-n max] [-b bits]                           the estimators on any file of symbols
 *   build/seed_assess -R restarts [-n per-restart] [-H claim]              restart test (SP 800-90B 3.1.4), a fresh process each (Linux)
 *
 * The estimators were checked on data of known min-entropy (a binary Markov chain, IID with a known most likely value, a counter, a
 * short period, /dev/urandom); --fast skips the two slowest ones on the bitstring. */
#include "../../lib/pzcore/src/seed.c"
#include <stdlib.h>
#include <sys/wait.h>

enum { EA_MCV, EA_COLL, EA_MARKOV, EA_TTUPLE, EA_LRS, EA_MULTIMCW, EA_LAG, EA_MULTIMMC, EA_LZ78Y, EA_N };

typedef struct {
    double orig[EA_N];       /* on the symbols (negative: not applicable) */
    double bits[EA_N];       /* per bit, on the bitstring */
    double h_original, h_bitstring, h_final;
    unsigned t_orig, v_orig, t_bits, v_bits;
    double seconds;
} ea_report;

#define Z99 2.576     /* the 99% upper bound of the standard */

static const char *const ea_names[EA_N] = {"MCV", "Collision", "Markov", "t-Tuple", "LRS", "MultiMCW", "Lag", "MultiMMC", "LZ78Y"};

static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz);
    if (!p) {
        fprintf(stderr, "seed_assess: out of memory\n");
        exit(3);
    }
    return p;
}

static double upper99(double p, size_t L)
{
    double u = p + Z99 * sqrt(p * (1 - p) / (double)(L - 1));
    return u > 1 ? 1 : u;
}

/* ---- 6.3.1 most common value ---- */
static double ea_mcv(const uint8_t *s, size_t L, unsigned k)
{
    if (L < 2) return -1;
    size_t cnt[256] = {0};
    for (size_t i = 0; i < L; i++) cnt[s[i]]++;
    size_t m = 0;
    for (unsigned v = 0; v < k && v < 256; v++)
        if (cnt[v] > m) m = cnt[v];
    return -log2(upper99((double)m / (double)L, L));
}

/* ---- 6.3.2 collision, binary: t is 2 when the next two bits are equal, else 3; E[t] = 2 + 2pq ---- */
static double ea_collision_bin(const uint8_t *b, size_t L)
{
    size_t i = 0, v = 0;
    double sum = 0, sum2 = 0;
    while (i + 1 < L) {
        size_t t;
        if (b[i] == b[i + 1]) t = 2;
        else if (i + 2 < L) t = 3;
        else break;
        v++;
        sum += (double)t;
        sum2 += (double)(t * t);
        i += t;
    }
    if (v < 2) return -1;
    const double mean = sum / (double)v;
    const double sd = sqrt((sum2 - (double)v * mean * mean) / (double)(v - 1));
    const double xl = mean - Z99 * sd / sqrt((double)v);
    if (xl >= 2.5) return 1.0;
    if (xl <= 2.0) return 0.0;
    const double p = 0.5 + sqrt(0.25 - (xl - 2.0) / 2.0);    /* solves 2 + 2p(1-p) = xl with p >= 1/2 */
    return -log2(p);
}

/* ---- 6.3.3 Markov, binary ---- */
static double markov_from_counts(double c0, double c1, const double c[2][2])
{
    const double L = c0 + c1;
    const double p0 = c0 / L, p1 = c1 / L;
    double t[2][2];
    for (int a = 0; a < 2; a++) {
        const double row = c[a][0] + c[a][1];
        t[a][0] = row > 0 ? c[a][0] / row : 0;
        t[a][1] = row > 0 ? c[a][1] / row : 0;
    }
#define LG(x) ((x) > 0 ? log2(x) : -INFINITY)
    const double cand[6] = {
        LG(p0) + 127 * LG(t[0][0]),
        LG(p0) + 64 * LG(t[0][1]) + 63 * LG(t[1][0]),
        LG(p0) + LG(t[0][1]) + 126 * LG(t[1][1]),
        LG(p1) + LG(t[1][0]) + 126 * LG(t[0][0]),
        LG(p1) + 64 * LG(t[1][0]) + 63 * LG(t[0][1]),
        LG(p1) + 127 * LG(t[1][1]),
    };
#undef LG
    double m = -INFINITY;
    for (int i = 0; i < 6; i++)
        if (cand[i] > m) m = cand[i];
    double h = -m / 128.0;
    return h > 1 ? 1 : h;
}

static double ea_markov_bin(const uint8_t *b, size_t L)
{
    if (L < 2) return -1;
    double c[2][2] = {{0, 0}, {0, 0}}, c1 = 0;
    for (size_t i = 0; i < L; i++) {
        c1 += b[i];
        if (i + 1 < L) c[b[i]][b[i + 1]] += 1;
    }
    return markov_from_counts((double)L - c1, c1, c);
}

static void ea_to_bits(const uint8_t *s, size_t L, unsigned nbits, uint8_t *out)
{
    for (size_t i = 0; i < L; i++)
        for (unsigned j = 0; j < nbits; j++) out[i * nbits + j] = (uint8_t)((s[i] >> (nbits - 1 - j)) & 1);
}

/* ---- 6.3.5 t-tuple and 6.3.6 LRS ----
 * The counts of every W-tuple for W = 1, 2, ... come from refining equivalence classes: positions whose W-tuples are equal form a
 * group; splitting each group by the symbol at offset W gives the groups of (W+1)-tuples. Groups of one position can never repeat
 * again and are dropped, so the work per level is the number of positions still in repeated tuples. Exact (no hashing). */
#define TUPLE_MAX_LEVEL 1024     /* degenerate data (long repeats) is cut there; its estimate is near 0 long before */

static void ea_ttuple_lrs(const uint8_t *s, size_t L, unsigned k, double *ttuple, double *lrs, unsigned *t_out, unsigned *v_out)
{
    *ttuple = *lrs = -1;
    if (t_out) *t_out = 0;
    if (v_out) *v_out = 0;
    if (L < 64) return;
    uint32_t *pos = xcalloc(L, 4), *pos2 = xcalloc(L, 4), *tmp = xcalloc(L, 4);
    uint32_t *gs = xcalloc(L + 2, 4), *gs2 = xcalloc(L + 2, 4);
    double *Q = xcalloc(TUPLE_MAX_LEVEL + 2, sizeof(double)), *S = xcalloc(TUPLE_MAX_LEVEL + 2, sizeof(double));
    size_t ng = 0, np = 0;
    /* level 1 */
    {
        size_t cnt[257] = {0};
        for (size_t i = 0; i < L; i++) cnt[s[i] + 1]++;
        double sum = 0, mx = 0;
        for (unsigned v = 0; v < 256; v++) {
            const double c = (double)cnt[v + 1];
            sum += c * (c - 1) / 2;
            if (c > mx) mx = c;
        }
        Q[1] = mx;
        S[1] = sum;
        size_t start[257];
        start[0] = 0;
        for (unsigned v = 0; v < 256; v++) start[v + 1] = start[v] + cnt[v + 1];
        size_t fillp[256];
        for (unsigned v = 0; v < 256; v++) fillp[v] = start[v];
        for (size_t i = 0; i < L; i++) tmp[fillp[s[i]]++] = (uint32_t)i;
        for (unsigned v = 0; v < 256; v++) {
            const size_t c = cnt[v + 1];
            if (c >= 2) {
                gs[ng++] = (uint32_t)np;
                memcpy(pos + np, tmp + start[v], c * 4);
                np += c;
            }
        }
        gs[ng] = (uint32_t)np;
    }
    unsigned W = 1, v = S[1] > 0 ? 1 : 0;
    while (ng > 0 && W < TUPLE_MAX_LEVEL) {
        size_t ng2 = 0, np2 = 0;
        double sum = 0, mx = 0;
        for (size_t g = 0; g < ng; g++) {
            const uint32_t a = gs[g], b = gs[g + 1];
            /* positions whose (W+1)-tuple exists */
            size_t m = 0;
            for (uint32_t i = a; i < b; i++)
                if ((size_t)pos[i] + W < L) tmp[m++] = pos[i];
            if (m < 2) continue;
            uint32_t *out = pos2 + np2;
            if (m > 64) {
                size_t cnt[257] = {0};
                for (size_t i = 0; i < m; i++) cnt[s[tmp[i] + W] + 1]++;
                for (unsigned x = 0; x < 256; x++) cnt[x + 1] += cnt[x];
                for (size_t i = 0; i < m; i++) out[cnt[s[tmp[i] + W]]++] = tmp[i];
            } else {
                for (size_t i = 0; i < m; i++) {      /* insertion sort by the next symbol */
                    const uint32_t p = tmp[i];
                    const uint8_t key = s[p + W];
                    size_t j = i;
                    while (j > 0 && s[out[j - 1] + W] > key) {
                        out[j] = out[j - 1];
                        j--;
                    }
                    out[j] = p;
                }
            }
            /* split into runs of equal next symbol; keep runs of two or more */
            size_t w = 0;
            for (size_t i = 0; i < m;) {
                size_t j = i + 1;
                const uint8_t key = s[out[i] + W];
                while (j < m && s[out[j] + W] == key) j++;
                const size_t c = j - i;
                if (c >= 2) {
                    sum += (double)c * (double)(c - 1) / 2;
                    if ((double)c > mx) mx = (double)c;
                    gs2[ng2++] = (uint32_t)(np2 + w);
                    if (w != i) memmove(out + w, out + i, c * 4);
                    w += c;
                }
                i = j;
            }
            np2 += w;
        }
        gs2[ng2] = (uint32_t)np2;
        W++;
        Q[W] = mx;
        S[W] = sum;
        if (sum > 0) v = W;
        uint32_t *t;
        t = pos; pos = pos2; pos2 = t;
        t = gs; gs = gs2; gs2 = t;
        ng = ng2;
        np = np2;
    }
    (void)k;
    (void)np;
    /* t: the largest W whose most common W-tuple occurs at least 35 times */
    unsigned t = 0;
    for (unsigned i = 1; i <= W; i++)
        if (Q[i] >= 35) t = i;
    if (t > 0) {
        double pmax = 0;
        for (unsigned i = 1; i <= t; i++) {
            const double p = pow(Q[i] / (double)(L - i + 1), 1.0 / i);
            if (p > pmax) pmax = p;
        }
        *ttuple = -log2(upper99(pmax, L));
    }
    const unsigned u = t + 1;
    if (u <= v) {
        double pmax = 0;
        for (unsigned w = u; w <= v; w++) {
            const double n = (double)(L - w + 1);
            const double p = pow(S[w] / (n * (n - 1) / 2), 1.0 / w);
            if (p > pmax) pmax = p;
        }
        *lrs = -log2(upper99(pmax, L));
    }
    if (t_out) *t_out = t;
    if (v_out) *v_out = v;
    free(pos); free(pos2); free(tmp); free(gs); free(gs2); free(Q); free(S);
}

/* ---- predictors: global and local predictability (6.3.7 steps 3-5) ---- */
static double local_f(double p, double r, double N)
{
    const double q = 1 - p;
    double x = 1;
    for (int j = 0; j < 10; j++) x = 1 + q * pow(p, r) * pow(x, r + 1);
    const double num = 1 - p * x, den = (r + 1 - r * x) * q;
    if (!(num > 0) || !(den > 0) || !isfinite(x)) return 0;
    return exp(log(num) - log(den) - (N + 1) * log(x));
}

static double local_p(uint64_t longest, uint64_t N)
{
    const double r = (double)longest + 1;
    double lo = 0, hi = 1;
    for (int it = 0; it < 80; it++) {
        const double mid = (lo + hi) / 2;
        if (local_f(mid, r, (double)N) > 0.99) lo = mid;
        else hi = mid;
    }
    return lo;
}

static double pred_entropy(uint64_t C, uint64_t N, uint64_t longest, unsigned k)
{
    if (N < 2) return -1;
    double pg;
    if (C == 0) pg = 1 - pow(0.01, 1.0 / (double)N);
    else pg = upper99((double)C / (double)N, N);
    const double pl = local_p(longest, N);
    double p = pg > pl ? pg : pl;
    if (p < 1.0 / k) p = 1.0 / k;
    return -log2(p);
}

typedef struct { uint64_t C, N, run, longest; } pstat;
static inline void pstat_add(pstat *s, int ok)
{
    s->N++;
    if (ok) {
        s->C++;
        if (++s->run > s->longest) s->longest = s->run;
    } else {
        s->run = 0;
    }
}

/* ---- 6.3.7 MultiMCW ---- */
static double ea_multimcw(const uint8_t *s, size_t L, unsigned k)
{
    static const size_t w[4] = {63, 255, 1023, 4095};
    if (L <= w[0] + 1) return -1;
    uint32_t *cnt = xcalloc(4 * 256, 4);
    uint64_t *last = xcalloc(256, 8);
    uint64_t score[4] = {0, 0, 0, 0};
    int winner = 0;
    pstat ps = {0, 0, 0, 0};
    for (size_t i = 0; i < L; i++) {
        if (i >= w[0]) {
            int freq[4] = {-1, -1, -1, -1};
            for (int j = 0; j < 4; j++) {
                if (i < w[j]) continue;
                const uint32_t *c = cnt + 256 * j;
                uint32_t best = 0;
                uint64_t bl = 0;
                for (unsigned x = 0; x < k; x++)
                    if (c[x] > best || (c[x] == best && c[x] > 0 && last[x] > bl)) {
                        best = c[x];
                        bl = last[x];
                        freq[j] = (int)x;
                    }
            }
            pstat_add(&ps, freq[winner] == s[i]);
            for (int j = 0; j < 4; j++)
                if (freq[j] == s[i]) {
                    score[j]++;
                    if (score[j] >= score[winner]) winner = j;
                }
        }
        for (int j = 0; j < 4; j++) {
            cnt[256 * j + s[i]]++;
            if (i >= w[j]) cnt[256 * j + s[i - w[j]]]--;
        }
        last[s[i]] = i + 1;
    }
    free(cnt);
    free(last);
    return pred_entropy(ps.C, ps.N, ps.longest, k);
}

/* ---- 6.3.8 lag ---- */
static double ea_lag(const uint8_t *s, size_t L, unsigned k)
{
    enum { D = 128 };
    if (L < 3) return -1;
    uint64_t score[D + 1] = {0};
    unsigned winner = 1;
    pstat ps = {0, 0, 0, 0};
    for (size_t i = 1; i < L; i++) {
        pstat_add(&ps, winner <= i && s[i - winner] == s[i]);
        const unsigned dmax = i < D ? (unsigned)i : D;
        for (unsigned d = 1; d <= dmax; d++)
            if (s[i - d] == s[i]) {
                score[d]++;
                if (score[d] >= score[winner]) winner = d;
            }
    }
    return pred_entropy(ps.C, ps.N, ps.longest, k);
}

/* ---- a hash map from (tuple, length[, next symbol]) to counts, for MultiMMC and LZ78Y ---- */
typedef struct {
    uint64_t a, b;        /* the tuple: symbols 0-7 and 8-15, 8 bits each */
    uint32_t meta;        /* 0 = empty; else 1<<31 | length | (y+1) << 8, with y+1 = 0 for a tuple entry */
    uint32_t cnt;         /* pair: count; tuple: count of its most frequent next symbol */
    uint32_t besty;       /* tuple: that symbol */
    uint32_t pad;
} hent;
typedef struct { hent *t; size_t cap, n; } hmap;

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
static void hm_init(hmap *m, size_t cap) { m->cap = cap; m->n = 0; m->t = xcalloc(cap, sizeof(hent)); }
static void hm_free(hmap *m) { free(m->t); m->t = NULL; }
static hent *hm_slot(hent *t, size_t cap, uint64_t a, uint64_t b, uint32_t meta)
{
    size_t i = (size_t)(mix64(a ^ mix64(b ^ ((uint64_t)meta << 17))) & (cap - 1));
    for (;;) {
        hent *e = &t[i];
        if (e->meta == 0 || (e->meta == meta && e->a == a && e->b == b)) return e;
        i = (i + 1) & (cap - 1);
    }
}
static void hm_grow(hmap *m)
{
    hent *old = m->t;
    const size_t oc = m->cap;
    m->cap *= 2;
    m->t = xcalloc(m->cap, sizeof(hent));
    for (size_t i = 0; i < oc; i++)
        if (old[i].meta) *hm_slot(m->t, m->cap, old[i].a, old[i].b, old[i].meta) = old[i];
    free(old);
}
/* the entry, or NULL when absent and create is 0 */
static hent *hm_get(hmap *m, uint64_t a, uint64_t b, uint32_t meta, int create)
{
    if (create && 2 * (m->n + 1) > m->cap) hm_grow(m);
    hent *e = hm_slot(m->t, m->cap, a, b, meta);
    if (e->meta == 0) {
        if (!create) return NULL;
        e->meta = meta;
        e->a = a;
        e->b = b;
        e->cnt = 0;
        e->besty = 0;
        m->n++;
    }
    return e;
}
/* pack s[from .. from+len) */
static inline void pack(const uint8_t *s, size_t from, unsigned len, uint64_t *a, uint64_t *b)
{
    uint64_t x = 0, y = 0;
    for (unsigned i = 0; i < len; i++) {
        if (i < 8) x |= (uint64_t)s[from + i] << (8 * i);
        else y |= (uint64_t)s[from + i] << (8 * (i - 8));
    }
    *a = x;
    *b = y;
}
#define META_T(len) (0x80000000u | (uint32_t)(len))
#define META_P(len, y) (0x80000000u | (uint32_t)(len) | ((uint32_t)(y) + 1) << 8)

/* increments pair (tuple, y) and keeps the tuple's argmax; the tuple entry must exist */
static void bump(hmap *pairs, hent *tup, uint64_t a, uint64_t b, unsigned len, unsigned y)
{
    hent *p = hm_get(pairs, a, b, META_P(len, y), 1);
    p->cnt++;
    if (p->cnt > tup->cnt || (p->cnt == tup->cnt && y > tup->besty)) {
        tup->cnt = p->cnt;
        tup->besty = y;
    }
}

/* ---- 6.3.9 MultiMMC ---- */
static double ea_multimmc(const uint8_t *s, size_t L, unsigned k)
{
    enum { D = 16 };
    const uint32_t max_entries = 100000;
    if (L < 4) return -1;
    hmap tuples, pairs;
    hm_init(&tuples, 1u << 20);
    hm_init(&pairs, 1u << 20);
    uint32_t entries[D + 1] = {0};
    uint64_t score[D + 1] = {0};
    unsigned winner = 1;
    pstat ps = {0, 0, 0, 0};
    for (size_t i = 2; i < L; i++) {
        /* update: for d with d < i (1-based i-1 > d), tuple s[i-d-1 .. i-2], next s[i-1] */
        for (unsigned d = 1; d <= D && d + 1 <= i; d++) {
            uint64_t a, b;
            pack(s, i - d - 1, d, &a, &b);
            const unsigned y = s[i - 1];
            hent *p = hm_get(&pairs, a, b, META_P(d, y), 0);
            if (!p) {
                if (entries[d] >= max_entries) continue;
                entries[d]++;
            }
            hent *t = hm_get(&tuples, a, b, META_T(d), 1);
            bump(&pairs, t, a, b, d, y);
        }
        /* predict s[i] from s[i-d .. i-1] */
        int sub[D + 1];
        for (unsigned d = 1; d <= D; d++) {
            sub[d] = -1;
            if (d > i) continue;
            uint64_t a, b;
            pack(s, i - d, d, &a, &b);
            hent *t = hm_get(&tuples, a, b, META_T(d), 0);
            if (t) sub[d] = (int)t->besty;
        }
        pstat_add(&ps, sub[winner] == s[i]);
        for (unsigned d = 1; d <= D; d++)
            if (sub[d] == s[i]) {
                score[d]++;
                if (score[d] >= score[winner]) winner = d;
            }
    }
    hm_free(&tuples);
    hm_free(&pairs);
    return pred_entropy(ps.C, ps.N, ps.longest, k);
}

/* ---- 6.3.10 LZ78Y ---- */
static double ea_lz78y(const uint8_t *s, size_t L, unsigned k)
{
    enum { B = 16 };
    const size_t max_dict = 65536;
    if (L < B + 3) return -1;
    hmap tuples, pairs;
    hm_init(&tuples, 1u << 18);
    hm_init(&pairs, 1u << 20);
    pstat ps = {0, 0, 0, 0};
    for (size_t i = B + 1; i < L; i++) {
        /* 1-based i' = i+1 >= B+2: for j = B..1, tuple s[i-j-1 .. i-2], next s[i-1] */
        for (unsigned j = B; j >= 1; j--) {
            uint64_t a, b;
            pack(s, i - j - 1, j, &a, &b);
            hent *t = hm_get(&tuples, a, b, META_T(j), 0);
            if (!t) {
                if (tuples.n >= max_dict) continue;
                t = hm_get(&tuples, a, b, META_T(j), 1);
            }
            bump(&pairs, t, a, b, j, s[i - 1]);
        }
        int pred = -1;
        uint32_t maxc = 0;
        for (unsigned j = B; j >= 1; j--) {
            uint64_t a, b;
            pack(s, i - j, j, &a, &b);
            hent *t = hm_get(&tuples, a, b, META_T(j), 0);
            if (t && t->cnt > maxc) {
                maxc = t->cnt;
                pred = (int)t->besty;
            }
        }
        pstat_add(&ps, pred == s[i]);
    }
    hm_free(&tuples);
    hm_free(&pairs);
    return pred_entropy(ps.C, ps.N, ps.longest, k);
}

static double secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void ea_run_all(const uint8_t *s, size_t L, unsigned nbits, int fast, ea_report *r)
{
    const double t0 = secs();
    const unsigned k = 1u << nbits;
    for (int i = 0; i < EA_N; i++) r->orig[i] = r->bits[i] = -1;
    if (nbits > 1) {
        r->orig[EA_MCV] = ea_mcv(s, L, k);
        ea_ttuple_lrs(s, L, k, &r->orig[EA_TTUPLE], &r->orig[EA_LRS], &r->t_orig, &r->v_orig);
        r->orig[EA_MULTIMCW] = ea_multimcw(s, L, k);
        r->orig[EA_LAG] = ea_lag(s, L, k);
        r->orig[EA_MULTIMMC] = ea_multimmc(s, L, k);
        r->orig[EA_LZ78Y] = ea_lz78y(s, L, k);
    }
    uint8_t *b = xcalloc(L * nbits, 1);
    const size_t n = L * nbits;
    ea_to_bits(s, L, nbits, b);
    r->bits[EA_MCV] = ea_mcv(b, n, 2);
    r->bits[EA_COLL] = ea_collision_bin(b, n);
    r->bits[EA_MARKOV] = ea_markov_bin(b, n);
    ea_ttuple_lrs(b, n, 2, &r->bits[EA_TTUPLE], &r->bits[EA_LRS], &r->t_bits, &r->v_bits);
    r->bits[EA_MULTIMCW] = ea_multimcw(b, n, 2);
    r->bits[EA_LAG] = ea_lag(b, n, 2);
    if (!fast) {
        r->bits[EA_MULTIMMC] = ea_multimmc(b, n, 2);
        r->bits[EA_LZ78Y] = ea_lz78y(b, n, 2);
    }
    free(b);
    r->h_original = INFINITY;
    r->h_bitstring = INFINITY;
    for (int i = 0; i < EA_N; i++) {
        if (r->orig[i] >= 0 && r->orig[i] < r->h_original) r->h_original = r->orig[i];
        if (r->bits[i] >= 0 && r->bits[i] < r->h_bitstring) r->h_bitstring = r->bits[i];
    }
    if (nbits == 1) r->h_original = INFINITY;
    double hf = nbits * r->h_bitstring;
    if (r->h_original < hf) hf = r->h_original;
    r->h_final = hf;
    r->seconds = secs() - t0;
}

/* ---- the collector of seed.c, and the reports ---- */

/* n raw samples (the low bits of each timer delta) after the generator's calibration; warm-up as in a round unless raw (the restart
 * test wants the first samples of a process) */
static void collect(uint8_t *s, uint32_t *d, size_t n, unsigned nbits, int raw, double *us, uint32_t *walk)
{
    jitter j;
    char method[64], why[160];
    if (!jit_init(&j, method, sizeof method, why, sizeof why)) {
        fprintf(stderr, "seed_assess: %s\n", why);
        exit(2);
    }
    if (!raw)
        for (int i = 0; i < 32; i++) jit_sample(&j);
    const double t0 = now_ms();
    for (size_t i = 0; i < n; i++) {
        const uint32_t v = (uint32_t)jit_sample(&j);
        if (d) d[i] = v;
        s[i] = (uint8_t)(v & ((1u << nbits) - 1));
    }
    if (us) *us = (now_ms() - t0) * 1e3 / (double)n;
    if (walk) *walk = j.walk;
    unmap_pages((uint8_t *)j.mem, JIT_MEM, false);
}

static void print_report(const ea_report *r, unsigned nbits)
{
    printf("  %-10s %12s %12s\n", "estimator", "symbols", "bitstring");
    for (int i = 0; i < EA_N; i++) {
        char a[32] = "-", b[32] = "-";
        if (r->orig[i] >= 0) snprintf(a, sizeof a, "%.4f", r->orig[i]);
        if (r->bits[i] >= 0) snprintf(b, sizeof b, "%.4f", r->bits[i]);
        printf("  %-10s %12s %12s\n", ea_names[i], a, b);
    }
    printf("  H_original %.4f, H_bitstring %.4f per bit (x%u = %.4f)\n", r->h_original, r->h_bitstring, nbits, nbits * r->h_bitstring);
    printf("  min-entropy per sample H = %.4f bits (estimators: %.1f s)\n", r->h_final, r->seconds);
    if (r->h_final > 0)
        printf("  samples for 128 bits at a safety factor of 2: %.0f (factor 4: %.0f); the generator credits 0.5 bit per sample\n",
               ceil(2 * 128 / r->h_final), ceil(4 * 128 / r->h_final));
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* P(X >= k), X ~ Binomial(n, p) */
static double binom_tail(unsigned n, double p, unsigned k)
{
    double s = 0;
    for (unsigned j = k; j <= n; j++) s += exp(lgamma(n + 1.0) - lgamma(j + 1.0) - lgamma(n - j + 1.0) + j * log(p) + (n - j) * log1p(-p));
    return s;
}

/* SP 800-90B 3.1.4: R fresh processes of n samples each. The sanity check compares the most common value of every row (restart)
 * and column (sample position) with the binomial cutoff at alpha = 0.01 / (R + n); the validation wants min(H_rows, H_columns) at
 * least H_claim / 2. A column that repeats across restarts is a sample whose timing does not depend on the run. */
static int restart_test(unsigned R, size_t n, unsigned nbits, double h_claim)
{
    uint8_t *m = xcalloc((size_t)R * n, 1), *col = xcalloc((size_t)R * n, 1);
    char ns[32], bs[8];
    snprintf(ns, sizeof ns, "%zu", n);
    snprintf(bs, sizeof bs, "%u", nbits);
    for (unsigned r = 0; r < R; r++) {
        int fds[2];
        if (pipe(fds) != 0) return 1;
        const pid_t pid = fork();
        if (pid == 0) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
            execl("/proc/self/exe", "seed_assess", "--emit", ns, "-b", bs, (char *)NULL);
            _exit(127);
        }
        close(fds[1]);
        size_t got = 0;
        for (long k; got < n && (k = read(fds[0], m + (size_t)r * n + got, n - got)) > 0;) got += (size_t)k;
        close(fds[0]);
        waitpid(pid, NULL, 0);
        if (got != n) {
            fprintf(stderr, "restart %u: short output\n", r);
            return 1;
        }
    }
    unsigned fr = 0, fc = 0, over = 0;
    for (unsigned r = 0; r < R; r++) {
        unsigned c[256] = {0};
        for (size_t i = 0; i < n; i++) c[m[(size_t)r * n + i]]++;
        for (int v = 0; v < 256; v++) fr = c[v] > fr ? c[v] : fr;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned c[256] = {0}, mx = 0;
        for (unsigned r = 0; r < R; r++) c[col[i * R + r] = m[(size_t)r * n + i]]++;
        for (int v = 0; v < 256; v++) mx = c[v] > mx ? c[v] : mx;
        fc = mx > fc ? mx : fc;
        over += mx > 30;
    }
    const double alpha = 0.01 / (R + n), p = pow(2.0, -h_claim);
    unsigned U = (unsigned)(R * p);
    while (U < R && binom_tail(R, p, U + 1) > alpha) U++;
    printf("restart test: %u restarts x %zu samples, %u-bit symbols, H_I = %.2f\n", R, n, nbits, h_claim);
    printf("  sanity check: most common value %u times in a restart, %u times at a sample position (%u positions over 30); cutoff %u: %s\n",
           fr, fc, over, U, fr > U || fc > U ? "FAIL" : "pass");
    ea_report rr, rc;
    ea_run_all(m, (size_t)R * n, nbits, 1, &rr);
    ea_run_all(col, (size_t)R * n, nbits, 1, &rc);
    const double hm = rr.h_final < rc.h_final ? rr.h_final : rc.h_final;
    printf("  H_rows %.4f, H_columns %.4f: min %.4f %s H_I / 2\n", rr.h_final, rc.h_final, hm, hm >= h_claim / 2 ? ">=" : "<");
    free(col);
    free(m);
    return 0;
}

int main(int argc, char **argv)
{
    size_t n = 1000000;
    unsigned nbits = 8, R = 0;
    const char *outf = NULL, *inf = NULL;
    int fast = 0, emit = 0;
    double h_claim = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--emit") && i + 1 < argc) n = strtoull(argv[++i], NULL, 10), emit = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) nbits = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outf = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) inf = argv[++i];
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) R = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) h_claim = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fast")) fast = 1;
        else {
            fprintf(stderr, "usage: %s [-n samples] [-b bits] [-o out] [--fast] | -i FILE | -R restarts [-H claim]\n", argv[0]);
            return 1;
        }
    }
    if (nbits < 1 || nbits > 8) nbits = 8;
    if (n < 64) n = 64;
    uint8_t *s = xcalloc(n, 1);
    if (emit) {                                /* one restart: the first samples of a fresh process */
        collect(s, NULL, n, nbits, 1, NULL, NULL);
        return fwrite(s, 1, n, stdout) == n ? 0 : 1;
    }
    if (R) return restart_test(R, n, nbits, h_claim);
    ea_report r;
    if (inf) {
        FILE *f = fopen(inf, "rb");
        if (!f) return perror(inf), 1;
        n = fread(s, 1, n, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) s[i] &= (uint8_t)((1u << nbits) - 1);
        printf("file %s: %zu symbols of %u bits\n", inf, n, nbits);
    } else {
        uint32_t *d = xcalloc(n, 4), walk = 0;
        double us = 0;
        collect(s, d, n, nbits, 0, &us, &walk);
        size_t stuck = 0, distinct = 1;
        unsigned seen[256] = {0}, dsym = 0;
        for (size_t i = 2; i < n; i++) {
            const uint32_t d2 = d[i] - d[i - 1], d3 = d2 - (d[i - 1] - d[i - 2]);
            stuck += d[i] == 0 || d2 == 0 || d3 == 0;
        }
        for (size_t i = 0; i < n; i++) dsym += !seen[s[i]]++;
        qsort(d, n, 4, cmp_u32);
        for (size_t i = 1; i < n; i++) distinct += d[i] != d[i - 1];
        printf("timer %s, walk %u steps through %u KiB, %.2f us per sample\n", TIMER_NAME, walk, JIT_MEM >> 10, us);
        printf("delta: min %u, median %u, 99%% %u, max %u ticks; %zu distinct deltas, %u distinct %u-bit symbols; stuck %.2f%%\n", d[0], d[n / 2],
               d[n - n / 100 - 1], d[n - 1], distinct, dsym, nbits, 100.0 * (double)stuck / (double)n);
        free(d);
        if (outf) {
            FILE *f = fopen(outf, "wb");
            if (f) fwrite(s, 1, n, f), fclose(f);
        }
    }
    ea_run_all(s, n, nbits, fast, &r);
    print_report(&r, nbits);
    free(s);
    return 0;
}
