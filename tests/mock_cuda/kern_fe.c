/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kern_fe.c - the kernel field arithmetic (pz_common.cl) compiled for the host, for tests/t_gpufe.c. Built twice:
 * with -DPZ_FAST_FE (PTX carry chains, emulated by pz_host.h) and without (portable version); the prefix of the
 * exported functions is chosen with -DGK=... */
#include "pz_host.h"
PZ_MOCK_TLS unsigned pz_mock_gid;
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../src/kernels/pz_common.cl"

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
static void ld(fe *f, const uint *p) { for (int i = 0; i < 8; i++) f->v[i] = p[i]; }
static void st(uint *p, const fe *f) { for (int i = 0; i < 8; i++) p[i] = f->v[i]; }
void CAT(GK, add)(uint *r, const uint *a, const uint *b) { fe x, y, z; ld(&x, a); ld(&y, b); fe_add(&z, &x, &y); st(r, &z); }
void CAT(GK, sub)(uint *r, const uint *a, const uint *b) { fe x, y, z; ld(&x, a); ld(&y, b); fe_sub(&z, &x, &y); st(r, &z); }
void CAT(GK, sub2)(uint *r, const uint *a, const uint *b, const uint *c) { fe x, y, w, z; ld(&x, a); ld(&y, b); ld(&w, c); fe_sub2(&z, &x, &y, &w); st(r, &z); }
void CAT(GK, mul)(uint *r, const uint *a, const uint *b) { fe x, y, z; ld(&x, a); ld(&y, b); fe_mul(&z, &x, &y); st(r, &z); }
void CAT(GK, sqr)(uint *r, const uint *a) { fe x, z; ld(&x, a); fe_sqr(&z, &x); st(r, &z); }
void CAT(GK, norm)(uint *r, const uint *a) { fe x; ld(&x, a); fe_normalize(&x); st(r, &x); }
void CAT(GK, inv)(uint *r, const uint *a) { fe x, z; ld(&x, a); fe_inv(&z, &x); st(r, &z); }
void CAT(GK, invpow)(uint *r, const uint *a) { fe x, z; ld(&x, a); fe_inv_pow(&z, &x); st(r, &z); }
