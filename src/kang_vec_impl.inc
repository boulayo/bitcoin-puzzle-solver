/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_vec_impl.inc - one batch round of the kangaroo herd with lane-parallel arithmetic (pz_fev.h or, with PZ_FEV_IFMA,
 * fev52.h), instantiated per ISA. The includer defines ISA (token) and PZ_VBYTES.
 *
 * Exactly the arithmetic and the rules of kang_isa.inc (same jump choice, 2-cycle escape, even-y representative,
 * distinguished points and restarts, reported in the same order), so a herd evolves bit for bit as with the scalar
 * round (checked by kang_round_check). Kangaroo c*NL + l lives in lane l of vector c, and its whole walking state stays
 * in vector form between rounds: coordinates, 192-bit distance (three 64-bit lanes with vector carries), sign, the tags
 * of the 2-cycle escape and the step counter; only distinguished points and restarts (rare) are handled lane by lane.
 * The jump entries are read with ordinary loads, one row per lane, and transposed (faster than hardware gathers): the forward pass
 * reads the x of the jump point, the backward pass its y and distance (x3 needs only x - jx, kept from the forward pass), and the
 * entry is laid out so that each reads as few 64-byte rows as it can. Code that writes the scalar state of a kangaroo (restarts)
 * marks it in w->vstale and it is reloaded at the next round; kang_worker_sync copies everything back. A round whose
 * batch product is zero (a kangaroo on its jump point: practically never) is run by the scalar code. */

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#ifdef PZ_FEV_IFMA
#include <pzcore/fev52.h>
#else
#include <pzcore/fev.h>
#endif

#define NL PZ_FEV_LANES
#define KV_WORDS (2 * PZ_FEV_NLIMB + 2)          /* table entry: x limbs, y limbs, distance (two words) */
#define KV_STRIDE ((KV_WORDS + 7) & ~7)        /* padded to whole 64-byte rows */
/* Word offsets in an entry. The backward pass reads y and the distance (KV_BN words from KV_B), the forward pass x: with 5 limbs
 * y and the distance fill the first row and x the second; with 9 limbs x takes words 0-8 and the distance and y words 9-19,
 * two rows each. */
#if PZ_FEV_NLIMB + 2 <= 8
#define KV_Y 0
#define KV_V PZ_FEV_NLIMB
#define KV_X 8
#else
#define KV_X 0
#define KV_V PZ_FEV_NLIMB
#define KV_Y (PZ_FEV_NLIMB + 2)
#endif
#define KV_B (KV_Y < KV_V ? KV_Y : KV_V)
#define KV_BN (PZ_FEV_NLIMB + 2)
_Static_assert(KV_X + PZ_FEV_NLIMB <= KV_STRIDE && KV_B + KV_BN <= KV_STRIDE && (KV_X >= KV_B + KV_BN || KV_X + PZ_FEV_NLIMB <= KV_B),
               "jump entry layout");

/* sig, esc: 0 or all ones; tags in the low 32 bits; jb: the table offsets of this round's jumps, from the forward pass */
typedef struct { pz_vl d0, d1, d2, prev, prev2, lastdp, sig, ck, mn, esc, jb; } kv_aux;

/* bits [64k, 64k + 64) of a canonical element */
static inline PZ_VABI pz_vl kv_word64(const pz_fev *x, int k)
{
    const int bit = 64 * k, a = bit / PZ_FEV_BITS, sh = bit % PZ_FEV_BITS;
    pz_vl w = x->l[a] >> sh;
#pragma GCC unroll 3
    for (int m = 1; m <= 3; m++)
        if (m * PZ_FEV_BITS - sh < 64 && a + m < PZ_FEV_NLIMB) w |= x->l[a + m] << (m * PZ_FEV_BITS - sh);
    return w;
}

/* table offsets of the jumps (kang_jump_index): low bits of x, moved to the next jump when the walk would close a
 * 2-cycle, or an escape jump at the point of least tag of a detected cycle */
static inline PZ_VABI pz_vl kv_jbase(const pz_fev *x, const kv_aux *a, u64 R)
{
    const pz_vl t = kv_word64(x, 2) & pz_vl_set(0xFFFFFFFFULL);
    const pz_vl two = (pz_vl)(t == a->prev2) & pz_vl_set(1);
    const pz_vl out = (a->esc | (pz_vl)(t == a->ck)) & (pz_vl)(t == a->mn);
    const pz_vl reg = (x->l[0] + two) & pz_vl_set(R - 1), ej = pz_vl_set(R) + ((x->l[0] >> 16) & pz_vl_set(KANG_NESC - 1));
    return ((ej & out) | (reg & ~out)) * pz_vl_set(KV_STRIDE);
}

/* kang_cycle_update on all lanes; bnd: all ones on a checkpoint step, else zero */
static inline PZ_VABI void kv_cycle_update(kv_aux *a, pz_vl t, pz_vl bnd)
{
    const pz_vl notag = pz_vl_set(KANG_NOTAG);
    const pz_vl in = a->esc | (pz_vl)(t == a->ck), eqm = (pz_vl)(t == a->mn);
    const pz_vl out = in & eqm, restart = a->esc & ~eqm & bnd, set = (~in & bnd) | restart;
    const pz_vl lt = (pz_vl)(t < a->mn) & ~in;
    a->esc = in & ~out & ~restart;
    a->ck = (notag & out) | (t & set) | (a->ck & ~(out | set));
    a->mn = (notag & out) | (t & (set | lt)) | (a->mn & ~(out | set | lt));
}

/* From the square l2 of the slope, the walker x (canonical) and dx = x - jx, jx the x of its jump point: x3 = l2 - x - jx = l2 + dx - 2x
 * without its carries (it only goes to pz_fev_normalize), and t = x - x3 = 3x - l2 - dx, tight. Neither needs jx, and t does not wait
 * for x3. */
static inline PZ_VABI void kv_x3_t(pz_fev *x3, pz_fev *t, const pz_fev *l2, const pz_fev *x, const pz_fev *dx)
{
    pz_fev s, x3x;
#pragma GCC unroll 9
    for (int i = 0; i < PZ_FEV_NLIMB; i++) {
        s.l[i] = l2->l[i] + dx->l[i];
        x3x.l[i] = (x->l[i] << 1) + x->l[i];
    }
    pz_fev_sub2_lazy(x3, &s, x, x);
    pz_fev_sub2(t, &x3x, l2, dx);
}

#if PZ_VBYTES == 64
/* 8x8 transpose of 64-bit words: r[l] = row of lane l -> r[w] = word w of every lane */
static inline PZ_VABI void kv_tr8(__m512i r[8])
{
    const __m512i i0 = _mm512_setr_epi64(0, 1, 8, 9, 4, 5, 12, 13), i1 = _mm512_setr_epi64(2, 3, 10, 11, 6, 7, 14, 15);
    const __m512i j0 = _mm512_setr_epi64(0, 1, 2, 3, 8, 9, 10, 11), j1 = _mm512_setr_epi64(4, 5, 6, 7, 12, 13, 14, 15);
    __m512i t[8], u[8];
    for (int k = 0; k < 4; k++) {
        t[2 * k] = _mm512_unpacklo_epi64(r[2 * k], r[2 * k + 1]);
        t[2 * k + 1] = _mm512_unpackhi_epi64(r[2 * k], r[2 * k + 1]);
    }
    for (int k = 0; k < 2; k++) {                /* k = 0: rows 0-3, k = 1: rows 4-7 */
        u[4 * k + 0] = _mm512_permutex2var_epi64(t[4 * k + 0], i0, t[4 * k + 2]);
        u[4 * k + 1] = _mm512_permutex2var_epi64(t[4 * k + 0], i1, t[4 * k + 2]);
        u[4 * k + 2] = _mm512_permutex2var_epi64(t[4 * k + 1], i0, t[4 * k + 3]);
        u[4 * k + 3] = _mm512_permutex2var_epi64(t[4 * k + 1], i1, t[4 * k + 3]);
    }
    r[0] = _mm512_permutex2var_epi64(u[0], j0, u[4]);
    r[4] = _mm512_permutex2var_epi64(u[0], j1, u[4]);
    r[2] = _mm512_permutex2var_epi64(u[1], j0, u[5]);
    r[6] = _mm512_permutex2var_epi64(u[1], j1, u[5]);
    r[1] = _mm512_permutex2var_epi64(u[2], j0, u[6]);
    r[5] = _mm512_permutex2var_epi64(u[2], j1, u[6]);
    r[3] = _mm512_permutex2var_epi64(u[3], j0, u[7]);
    r[7] = _mm512_permutex2var_epi64(u[3], j1, u[7]);
}
#endif

/* words [first, first + nw) of the table entries of the lanes, as one vector per word: out[i] = word first + i */
static inline PZ_VABI void kv_load_words(pz_vl *out, int first, int nw, const u64 *vjt, pz_vl jb)
{
#if PZ_VBYTES == 64
    for (int blk = first / 8; blk * 8 < first + nw; blk++) {
        __m512i r[8];
#pragma GCC unroll 8
        for (int l = 0; l < 8; l++) r[l] = _mm512_load_si512((const void *)(vjt + jb[l] + 8 * blk));
        kv_tr8(r);
        for (int k = 0; k < 8; k++)
            if (blk * 8 + k >= first && blk * 8 + k < first + nw) out[blk * 8 + k - first] = (pz_vl)r[k];
    }
#else
    u64 t[KV_STRIDE][NL] PZ_ALIGN(64);
    for (int l = 0; l < NL; l++)
        for (int i = 0; i < nw; i++) t[i][l] = vjt[jb[l] + (u64)(first + i)];
    memcpy(out, t, sizeof(pz_vl) * (size_t)nw);
#endif
}

/* The events of a round come vector by vector from the last vector to the first, lanes in increasing order: reversing
 * the whole range and then each run of one vector gives the increasing kangaroo order of the scalar round. */
static void kv_rev_dp(kang_worker *w, int a, int b)
{
    for (b--; a < b; a++, b--) {
        const dp_rec tr = w->dpbuf[a]; w->dpbuf[a] = w->dpbuf[b]; w->dpbuf[b] = tr;
        const int ti = w->dpidx[a]; w->dpidx[a] = w->dpidx[b]; w->dpidx[b] = ti;
    }
}
static void kv_rev_int(int *v, int a, int b)
{
    for (b--; a < b; a++, b--) { const int t = v[a]; v[a] = v[b]; v[b] = t; }
}
static void kv_sort_events(kang_worker *w, int dp0, int rs0)
{
    kv_rev_dp(w, dp0, w->ndp);
    for (int a = dp0; a < w->ndp;) {
        int b = a + 1;
        while (b < w->ndp && w->dpidx[b] / NL == w->dpidx[a] / NL) b++;
        kv_rev_dp(w, a, b);
        a = b;
    }
    kv_rev_int(w->restart, rs0, w->nrestart);
    for (int a = rs0; a < w->nrestart;) {
        int b = a + 1;
        while (b < w->nrestart && w->restart[b] / NL == w->restart[a] / NL) b++;
        kv_rev_int(w->restart, a, b);
        a = b;
    }
}

static void CAT(kv_sync_, ISA)(kang_worker *w)
{
    const pz_fev *vx = w->vx, *vy = w->vy;
    const kv_aux *A = w->vaux;
    for (int c = 0; c < w->K / NL; c++)
        for (int l = 0; l < NL; l++) {
            const int i = c * NL + l;
            if (w->vstale[i]) continue;          /* the scalar copy is the newer one */
            pz_fev_get_lane(&w->x[i], &vx[c], l);
            pz_fev_get_lane(&w->y[i], &vy[c], l);
            w->d[i].w[0] = A[c].d0[l];
            w->d[i].w[1] = A[c].d1[l];
            w->d[i].w[2] = A[c].d2[l];
            w->prev[i] = (u32)A[c].prev[l];
            w->prev2[i] = (u32)A[c].prev2[l];
            w->lastdp[i] = A[c].lastdp[l];
            w->sigma[i] = A[c].sig[l] ? -1 : 1;
            w->ck[i] = (u32)A[c].ck[l];
            w->mn[i] = (u32)A[c].mn[l];
            w->esc[i] = A[c].esc[l] != 0;
        }
}

/* First use: vector buffers and the jump table as limbs. */
static void CAT(kv_setup_, ISA)(kang_worker *w)
{
    const int nc = w->K / NL;
    w->vx = pz_xaligned(64, sizeof(pz_fev) * (size_t)nc);
    w->vy = pz_xaligned(64, sizeof(pz_fev) * (size_t)nc);
    w->vdx = pz_xaligned(64, sizeof(pz_fev) * (size_t)nc);
    w->vtmp = pz_xaligned(64, sizeof(pz_fev) * (size_t)nc);
    w->vaux = pz_xaligned(64, sizeof(kv_aux) * (size_t)nc);
    u64 *t = pz_xaligned(64, sizeof(u64) * KV_STRIDE * (w->P->R + KANG_NESC));
    for (unsigned j = 0; j < w->P->R + KANG_NESC; j++) {
        u64 *e = t + (size_t)j * KV_STRIDE;
        pz_fev a;
        pz_fev_set_lane(&a, 0, &w->P->jx[j]);
        for (int i = 0; i < PZ_FEV_NLIMB; i++) e[KV_X + i] = a.l[i][0];
        pz_fev_set_lane(&a, 0, &w->P->jy[j]);
        for (int i = 0; i < PZ_FEV_NLIMB; i++) e[KV_Y + i] = a.l[i][0];
        e[KV_V] = (u64)w->P->jv[j];
        e[KV_V + 1] = (u64)(w->P->jv[j] >> 64);
    }
    w->vjt = t;
    memset(w->vstale, 1, (size_t)w->K);
    w->vsync = CAT(kv_sync_, ISA);
}

#ifdef KV_PAIR
/* Everything the backward pass reads and writes besides the vectors it works on. */
typedef struct {
    kang_worker *w;
    pz_fev *vx, *vy, *vdx, *vtmp;
    kv_aux *A;
    const u64 *vjt;
    const pz_vl *pl;                      /* p as limbs */
    pz_vl one, dpmask, cap, bndv;
    bool bnd;
    u64 n;
} kv_env;

typedef struct {
    kv_aux *a;
    pz_fev ic, jy, t, lam, l2, x3, y3;
    pz_vl jv0, jv1, sel, dpm, rsm, s0, s1, s2;
} kv_ctx;

/* The backward pass of the batch inversion fused with the step, for the nu (1 or 2) vectors c, c-1: the inverse of the
 * batch product is peeled off in order, then every stage of the step runs for all of them before the next stage starts.
 * One vector alone is a chain of about a thousand dependent instructions, longer than the window of out-of-order execution,
 * so the units idle while a product waits for the one before it; two chains side by side keep them busy. The arithmetic of
 * every vector is exactly the same as when it runs alone (the vectors do not depend on each other), and the events are
 * recorded in the same order (c first, then c - 1). */
static inline __attribute__((always_inline)) PZ_VABI void kv_bwd_step(const kv_env *E, pz_fev *inv, int c, int nu)
{
    kang_worker *w = E->w;
    const pz_vl one = E->one;
    kv_ctx x[2];
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) {
        const int cc = c - u;
        x[u].a = &E->A[cc];
        if (cc > 0) {
            pz_fev_mul(&x[u].ic, inv, &E->vtmp[cc - 1]);
            pz_fev_mul(inv, inv, &E->vdx[cc]);
        } else {
            x[u].ic = *inv;
        }
    }
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) {
        pz_vl e[KV_BN];
        kv_load_words(e, KV_B, KV_BN, E->vjt, x[u].a->jb);
        memcpy(x[u].jy.l, e + KV_Y - KV_B, sizeof x[u].jy.l);
        x[u].jv0 = e[KV_V - KV_B];
        x[u].jv1 = e[KV_V - KV_B + 1];
    }
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) pz_fev_sub(&x[u].t, &E->vy[c - u], &x[u].jy);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) pz_fev_mul(&x[u].lam, &x[u].t, &x[u].ic);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) pz_fev_sqr(&x[u].l2, &x[u].lam);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) kv_x3_t(&x[u].x3, &x[u].t, &x[u].l2, &E->vx[c - u], &E->vdx[c - u]);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) pz_fev_mul(&x[u].y3, &x[u].lam, &x[u].t);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) pz_fev_sub_lazy(&x[u].y3, &x[u].y3, &E->vy[c - u]);
#pragma GCC unroll 2
    for (int u = 0; u < nu; u++) {
        pz_fev_normalize(&x[u].x3);
        pz_fev_normalize(&x[u].y3);
        /* canonical representative: even y (p - y for the odd lanes); sel = all ones in those lanes */
        const pz_vl sel = pz_vl_set(0) - (x[u].y3.l[0] & one);
        pz_fev ny;
        pz_vl br = pz_vl_set(0);
        for (int i = 0; i < PZ_FEV_NLIMB; i++) {              /* p - y limb by limb (y canonical, so no reduction) */
            const pz_vl t2 = E->pl[i] - x[u].y3.l[i] - br;
            br = t2 >> 63;
            ny.l[i] = i < PZ_FEV_NLIMB - 1 ? t2 & pz_vl_set(PZ_FEV_MASK) : t2;
        }
        for (int i = 0; i < PZ_FEV_NLIMB; i++) x[u].y3.l[i] = (ny.l[i] & sel) | (x[u].y3.l[i] & ~sel);
        x[u].sel = sel;
    }
    /* the rest of the step, vector by vector, in the order of the scalar round (events: c, then c - 1) */
    for (int u = 0; u < nu; u++) {
        const int cc = c - u;
        kv_aux *a = x[u].a;
        const pz_vl sel = x[u].sel;
        /* distance += jump (192-bit, carries as 0/1 lanes), then negated where y was odd: (d ^ sel) + 1 */
        pz_vl s0 = a->d0 + x[u].jv0, c0 = (pz_vl)(s0 < x[u].jv0) & one;
        pz_vl s1 = a->d1 + x[u].jv1, c1 = (pz_vl)(s1 < x[u].jv1) & one;
        s1 += c0;
        c1 |= (pz_vl)(s1 < c0) & one;
        pz_vl s2 = a->d2 + c1;
        const pz_vl b = sel & one;
        s0 = (s0 ^ sel) + b;
        const pz_vl k0 = (pz_vl)(s0 < b) & one;
        s1 = (s1 ^ sel) + k0;
        const pz_vl k1 = (pz_vl)(s1 < k0) & one;
        s2 = (s2 ^ sel) + k1;
        a->d0 = s0; a->d1 = s1; a->d2 = s2;
        a->sig ^= sel;
        const pz_vl tag = kv_word64(&E->vx[cc], 2) & pz_vl_set(0xFFFFFFFFULL);
        kv_cycle_update(a, tag, E->bndv);
        a->prev2 = a->prev;
        a->prev = tag;
        const pz_vl n1 = kv_word64(&x[u].x3, 1);
        const pz_vl dpm = (pz_vl)((n1 & E->dpmask) == pz_vl_set(0));
        pz_vl rsm = pz_vl_set(0);
        if (E->bnd) rsm = (pz_vl)(pz_vl_set(E->n + 1) - a->lastdp > E->cap) & ~dpm;      /* the watchdog, on checkpoint steps only */
        E->vx[cc] = x[u].x3;
        E->vy[cc] = x[u].y3;
        const pz_vl ev = dpm | rsm;
        u64 any = 0;
        for (int l = 0; l < NL; l++) any |= ev[l];
        if (LIKELY(!any)) continue;
        const pz_vl n2 = kv_word64(&x[u].x3, 2);
        for (int l = 0; l < NL; l++) {
            const int i = cc * NL + l;
            if (ev[l]) a->lastdp[l] = E->n + 1;
            if (dpm[l]) {
                unsigned kind = w->kind[i] == 0 ? KIND_TAME : (a->sig[l] == 0 ? KIND_WILD_P : KIND_WILD_M);
                kang_dp_reserve(w);
                w->dpbuf[w->ndp].tagk = (n2[l] & ~3ULL) | kind;
                w->dpbuf[w->ndp].d.w[0] = s0[l];
                w->dpbuf[w->ndp].d.w[1] = s1[l];
                w->dpbuf[w->ndp].d.w[2] = s2[l];
                w->dpidx[w->ndp] = i;
                w->ndp++;
            } else if (rsm[l]) {
                w->restart[w->nrestart++] = i;
            }
        }
    }
}

#endif

void CAT(kang_round_, ISA)(kang_worker *w)
{
    const int K = w->K, nc = K / NL;
    const kang_params_t *P = w->P;
    if (!w->vx) CAT(kv_setup_, ISA)(w);
    pz_fev *vx = w->vx, *vy = w->vy, *vdx = w->vdx, *vtmp = w->vtmp;
    kv_aux *A = w->vaux;
    const u64 *vjt = w->vjt;
    const u64 n = w->round;                                     /* the step number of this round */
    const bool bnd = (n & (KANG_CK_PERIOD - 1)) == 0;           /* checkpoint step: cycle detection, watchdog */
    const pz_vl bndv = bnd ? pz_vl_set(~0ULL) : pz_vl_set(0);

    /* restarted kangaroos: reload their lanes from the scalar state */
    for (int c = 0; c < nc; c++)
        for (int l = 0; l < NL; l++) {
            const int i = c * NL + l;
            if (!w->vstale[i]) continue;
            pz_fev_set_lane(&vx[c], l, &w->x[i]);
            pz_fev_set_lane(&vy[c], l, &w->y[i]);
            A[c].d0[l] = w->d[i].w[0];
            A[c].d1[l] = w->d[i].w[1];
            A[c].d2[l] = w->d[i].w[2];
            A[c].prev[l] = w->prev[i];
            A[c].prev2[l] = w->prev2[i];
            A[c].lastdp[l] = w->lastdp[i];
            A[c].sig[l] = w->sigma[i] < 0 ? ~0ULL : 0;
            A[c].ck[l] = w->ck[i];
            A[c].mn[l] = w->mn[i];
            A[c].esc[l] = w->esc[i] ? ~0ULL : 0;
            w->vstale[i] = 0;
        }

    /* denominators x - jx and their prefix products */
    pz_fev acc;
    for (int c = 0; c < nc; c++) {
        pz_fev jx;
        A[c].jb = kv_jbase(&vx[c], &A[c], P->R);
        kv_load_words(jx.l, KV_X, PZ_FEV_NLIMB, vjt, A[c].jb);
        pz_fev_sub(&vdx[c], &vx[c], &jx);
        if (c == 0) acc = vdx[0];
        else pz_fev_mul(&acc, &acc, &vdx[c]);
        vtmp[c] = acc;
    }
    {
        pz_fev chk = acc;
        pz_fev_normalize(&chk);
        pz_vl z = chk.l[0];
        for (int i = 1; i < PZ_FEV_NLIMB; i++) z |= chk.l[i];
        bool zero = false;
        for (int l = 0; l < NL; l++) zero |= z[l] == 0;
        if (UNLIKELY(zero)) {
            /* some kangaroo sits on its jump point: the scalar round handles the per-element fallback */
            CAT(kv_sync_, ISA)(w);
            kang_round_base(w);                      /* also advances w->round */
            memset(w->vstale, 1, (size_t)K);
            return;
        }
    }
    /* p as limbs, for the negation of a canonical y */
    pz_vl pl[PZ_FEV_NLIMB];
    {
        u128 lo = (u128)pz_fe_p.n[1] << 64 | pz_fe_p.n[0], hi = (u128)pz_fe_p.n[3] << 64 | pz_fe_p.n[2];
        for (int i = 0; i < PZ_FEV_NLIMB; i++) {
            const int bit = PZ_FEV_BITS * i;
            u64 v;
            if (bit + PZ_FEV_BITS <= 128) v = (u64)(lo >> bit);
            else if (bit >= 128) v = (u64)(hi >> (bit - 128));
            else v = (u64)((lo >> bit) | (hi << (128 - bit)));
            pl[i] = pz_vl_set(i < PZ_FEV_NLIMB - 1 ? v & PZ_FEV_MASK : v);
        }
    }
    /* Montgomery, backward pass fused with the step: each inverse is used as soon as it is peeled off, from the last
     * vector to the first; the rare events are sorted back into index order at the end */
    pz_fev inv;
    pz_fev_inv(&inv, &acc);
    const int ndp0 = w->ndp, nrs0 = w->nrestart;
    const pz_vl one = pz_vl_set(1), dpmask = pz_vl_set(P->dp_mask), cap = pz_vl_set(P->cap_since);
#ifdef KV_PAIR
    const kv_env E = {w, vx, vy, vdx, vtmp, A, vjt, pl, one, dpmask, cap, bndv, bnd, n};
    int c = nc - 1;
    for (; c >= 1; c -= 2) kv_bwd_step(&E, &inv, c, 2);     /* two vectors at a time: see kv_bwd_step */
    if (c == 0) kv_bwd_step(&E, &inv, 0, 1);
#else
    for (int c = nc - 1; c >= 0; c--) {
        kv_aux *a = &A[c];
        pz_fev ic;
        if (c > 0) {
            pz_fev_mul(&ic, &inv, &vtmp[c - 1]);
            pz_fev_mul(&inv, &inv, &vdx[c]);
        } else {
            ic = inv;
        }
        pz_fev jy, t, lam, l2, x3, y3, ny;
        pz_vl e[KV_BN];
        kv_load_words(e, KV_B, KV_BN, vjt, a->jb);
        memcpy(jy.l, e + KV_Y - KV_B, sizeof jy.l);
        const pz_vl jv0 = e[KV_V - KV_B], jv1 = e[KV_V - KV_B + 1];
        pz_fev_sub(&t, &vy[c], &jy);
        pz_fev_mul(&lam, &t, &ic);
        pz_fev_sqr(&l2, &lam);
        kv_x3_t(&x3, &t, &l2, &vx[c], &vdx[c]);
        pz_fev_mul(&y3, &lam, &t);
        pz_fev_sub_lazy(&y3, &y3, &vy[c]);
        pz_fev_normalize(&x3);
        pz_fev_normalize(&y3);
        /* canonical representative: even y (p - y for the odd lanes); sel = all ones in those lanes */
        const pz_vl sel = pz_vl_set(0) - (y3.l[0] & one);
        {
            pz_vl br = pz_vl_set(0);
            for (int i = 0; i < PZ_FEV_NLIMB; i++) {          /* p - y limb by limb (y canonical, so no reduction) */
                const pz_vl t2 = pl[i] - y3.l[i] - br;
                br = t2 >> 63;
                ny.l[i] = i < PZ_FEV_NLIMB - 1 ? t2 & pz_vl_set(PZ_FEV_MASK) : t2;
            }
        }
        for (int i = 0; i < PZ_FEV_NLIMB; i++) y3.l[i] = (ny.l[i] & sel) | (y3.l[i] & ~sel);

        /* distance += jump (192-bit, carries as 0/1 lanes), then negated where y was odd: (d ^ sel) + 1 */
        pz_vl s0 = a->d0 + jv0, c0 = (pz_vl)(s0 < jv0) & one;
        pz_vl s1 = a->d1 + jv1, c1 = (pz_vl)(s1 < jv1) & one;
        s1 += c0;
        c1 |= (pz_vl)(s1 < c0) & one;
        pz_vl s2 = a->d2 + c1;
        const pz_vl b = sel & one;
        s0 = (s0 ^ sel) + b;
        const pz_vl k0 = (pz_vl)(s0 < b) & one;
        s1 = (s1 ^ sel) + k0;
        const pz_vl k1 = (pz_vl)(s1 < k0) & one;
        s2 = (s2 ^ sel) + k1;
        a->d0 = s0; a->d1 = s1; a->d2 = s2;
        a->sig ^= sel;
        const pz_vl tag = kv_word64(&vx[c], 2) & pz_vl_set(0xFFFFFFFFULL);
        kv_cycle_update(a, tag, bndv);
        a->prev2 = a->prev;
        a->prev = tag;
        const pz_vl n1 = kv_word64(&x3, 1);
        const pz_vl dpm = (pz_vl)((n1 & dpmask) == pz_vl_set(0));
        pz_vl rsm = pz_vl_set(0);
        if (bnd) rsm = (pz_vl)(pz_vl_set(n + 1) - a->lastdp > cap) & ~dpm;      /* the watchdog, on checkpoint steps only */
        vx[c] = x3;
        vy[c] = y3;
        const pz_vl ev = dpm | rsm;
        u64 any = 0;
        for (int l = 0; l < NL; l++) any |= ev[l];
        if (LIKELY(!any)) continue;
        const pz_vl n2 = kv_word64(&x3, 2);
        for (int l = 0; l < NL; l++) {
            const int i = c * NL + l;
            if (ev[l]) a->lastdp[l] = n + 1;
            if (dpm[l]) {
                unsigned kind = w->kind[i] == 0 ? KIND_TAME : (a->sig[l] == 0 ? KIND_WILD_P : KIND_WILD_M);
                kang_dp_reserve(w);
                w->dpbuf[w->ndp].tagk = (n2[l] & ~3ULL) | kind;
                w->dpbuf[w->ndp].d.w[0] = s0[l];
                w->dpbuf[w->ndp].d.w[1] = s1[l];
                w->dpbuf[w->ndp].d.w[2] = s2[l];
                w->dpidx[w->ndp] = i;
                w->ndp++;
            } else if (rsm[l]) {
                w->restart[w->nrestart++] = i;
            }
        }
    }
#endif
    kv_sort_events(w, ndp0, nrs0);
    w->round = n + 1;
}
