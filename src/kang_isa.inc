/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_isa.inc - one batch round of the kangaroo herd (instantiated per ISA).
 * The includer defines KROUND (function name). */

void KROUND(kang_worker *w)
{
    const int K = w->K;
    const kang_params_t *P = w->P;
    if (UNLIKELY(!w->dx)) {
        w->dx = pz_xaligned(64, sizeof(pz_fe) * (size_t)K);
        w->tmp = pz_xaligned(64, sizeof(pz_fe) * (size_t)K);
    }
    for (int i = 0; i < K; i++) {
        const u32 j = kang_jump_index(P->R, w->x[i].n[0], kang_tag(w->x[i].n[2]), w->prev2[i], w->ck[i], w->mn[i], w->esc[i]);
        w->jidx[i] = j;
        pz_fe_sub(&w->dx[i], &w->x[i], &P->jx[j]);
    }
    const u64 n = w->round;                    /* the step number of this round */
    const bool bnd = (n & (KANG_CK_PERIOD - 1)) == 0;   /* checkpoint step: cycle detection, watchdog */
    bool zero_fallback = false;
    if (UNLIKELY(!pz_fe_inv_batch(w->dx, w->tmp, (size_t)K))) zero_fallback = true;
    for (int i = 0; i < K; i++) {
        const u32 j = w->jidx[i];
        pz_fe lam, t, x3, y3;
        if (UNLIKELY(zero_fallback)) {
            pz_fe dn = w->dx[i];
            pz_fe_normalize(&dn);
            if (pz_fe_is_zero(&dn)) { w->restart[w->nrestart++] = i; continue; }
            pz_fe_inv(&w->dx[i], &w->dx[i]);
        }
        pz_fe_sub(&t, &w->y[i], &P->jy[j]);
        pz_fe_mul(&lam, &t, &w->dx[i]);
        pz_fe_sqr(&x3, &lam);
        pz_fe_sub(&x3, &x3, &w->x[i]);
        pz_fe_sub(&x3, &x3, &P->jx[j]);
        pz_fe_sub(&t, &w->x[i], &x3);
        pz_fe_mul(&y3, &lam, &t);
        pz_fe_sub(&y3, &y3, &w->y[i]);
        pz_fe_normalize(&x3);
        pz_fe_normalize(&y3);
        i192_add_u128(&w->d[i], P->jv[j]);
        if (y3.n[0] & 1) {   /* canonical representative has even y */
            u128 b = (u128)pz_fe_p.n[0] - y3.n[0];
            u64 r0 = (u64)b, br = (u64)(b >> 64) & 1;
            b = (u128)pz_fe_p.n[1] - y3.n[1] - br; u64 r1 = (u64)b; br = (u64)(b >> 64) & 1;
            b = (u128)pz_fe_p.n[2] - y3.n[2] - br; u64 r2 = (u64)b; br = (u64)(b >> 64) & 1;
            b = (u128)pz_fe_p.n[3] - y3.n[3] - br; u64 r3 = (u64)b;
            y3.n[0] = r0; y3.n[1] = r1; y3.n[2] = r2; y3.n[3] = r3;
            i192_neg(&w->d[i]);
            w->sigma[i] = (int8_t)-w->sigma[i];
        }
        const u32 tag = kang_tag(w->x[i].n[2]);
        kang_cycle_update(bnd, tag, &w->ck[i], &w->mn[i], &w->esc[i]);
        w->prev2[i] = w->prev[i];
        w->prev[i] = tag;
        w->x[i] = x3;
        w->y[i] = y3;
        if ((x3.n[1] & P->dp_mask) == 0) {
            unsigned kind = w->kind[i] == 0 ? KIND_TAME : (w->sigma[i] > 0 ? KIND_WILD_P : KIND_WILD_M);
            kang_dp_reserve(w);
            w->dpbuf[w->ndp].tagk = (x3.n[2] & ~3ULL) | kind;
            w->dpbuf[w->ndp].d = w->d[i];
            w->dpidx[w->ndp] = i;
            w->ndp++;
            w->lastdp[i] = n + 1;
        } else if (UNLIKELY(bnd && n + 1 - w->lastdp[i] > P->cap_since)) {
            w->restart[w->nrestart++] = i;
            w->lastdp[i] = n + 1;
        }
    }
    w->round = n + 1;
}
