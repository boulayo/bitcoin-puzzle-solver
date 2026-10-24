/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fe.h - arithmetic in GF(p), p = 2^256 - 2^32 - 977 (secp256k1).
 *
 * Field elements are four 64-bit limbs (little-endian). Values are "lazy":
 * any 256-bit pattern is a valid representative; fe_normalize() produces the
 * canonical value in [0, p). Everything is static inline so each engine
 * translation unit gets code compiled for its own instruction set. */
#ifndef PZ_FE_H
#define PZ_FE_H

#include "common.h"
#include "u256.h"

typedef struct { u64 n[4]; } fe;

#define FE_C 0x1000003D1ULL   /* 2^256 mod p */

static const fe FE_P PZ_UNUSED = {{0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}};

static inline void fe_set_u64(fe *r, u64 v) { r->n[0] = v; r->n[1] = r->n[2] = r->n[3] = 0; }
static inline void fe_zero(fe *r) { fe_set_u64(r, 0); }
static inline void fe_copy(fe *r, const fe *a) { *r = *a; }

/* r += c * FE_C with full carry propagation, c small. Keeps r < 2^256. */
static inline void fe_fold_(u64 r[4], u64 c)
{
    u128 t = (u128)c * FE_C + r[0];
    r[0] = (u64)t;
    u64 k = (u64)(t >> 64);
    t = (u128)r[1] + k; r[1] = (u64)t; k = (u64)(t >> 64);
    t = (u128)r[2] + k; r[2] = (u64)t; k = (u64)(t >> 64);
    t = (u128)r[3] + k; r[3] = (u64)t; k = (u64)(t >> 64);
    if (UNLIKELY(k)) {
        /* wrapped past 2^256: the remaining value is tiny, adding FE_C is safe */
        t = (u128)r[0] + FE_C; r[0] = (u64)t; k = (u64)(t >> 64);
        t = (u128)r[1] + k; r[1] = (u64)t; k = (u64)(t >> 64);
        t = (u128)r[2] + k; r[2] = (u64)t; k = (u64)(t >> 64);
        r[3] += k;
    }
}

/* Branch-free in the common case: a carry (or borrow) out of the 256-bit operation is
 * folded back with a mask instead of a data-dependent branch, because that event
 * happens for about half of random operands and would be mispredicted constantly.
 * The (astronomically rare) second wrap is handled by a predicted-not-taken branch. */
static inline void fe_add_c(fe *r, const fe *a, const fe *b)
{
    u64 o[4];
    u128 t = (u128)a->n[0] + b->n[0];
    o[0] = (u64)t;
    t = (t >> 64) + a->n[1] + b->n[1]; o[1] = (u64)t;
    t = (t >> 64) + a->n[2] + b->n[2]; o[2] = (u64)t;
    t = (t >> 64) + a->n[3] + b->n[3]; o[3] = (u64)t;
    const u64 m = (u64)0 - (u64)(t >> 64);          /* all ones if the sum wrapped */
    t = (u128)o[0] + (m & FE_C); o[0] = (u64)t;
    t = (t >> 64) + o[1]; o[1] = (u64)t;
    t = (t >> 64) + o[2]; o[2] = (u64)t;
    t = (t >> 64) + o[3]; o[3] = (u64)t;
    if (UNLIKELY((u64)(t >> 64))) {                 /* wrapped again: value is tiny, adding C is safe */
        t = (u128)o[0] + FE_C; o[0] = (u64)t;
        t = (t >> 64) + o[1]; o[1] = (u64)t;
        t = (t >> 64) + o[2]; o[2] = (u64)t;
        o[3] += (u64)(t >> 64);
    }
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

static inline void fe_sub_c(fe *r, const fe *a, const fe *b)
{
    u64 o[4];
    u128 t = (u128)a->n[0] - b->n[0];
    o[0] = (u64)t;
    u64 br = (u64)(t >> 64) & 1;
    t = (u128)a->n[1] - b->n[1] - br; o[1] = (u64)t; br = (u64)(t >> 64) & 1;
    t = (u128)a->n[2] - b->n[2] - br; o[2] = (u64)t; br = (u64)(t >> 64) & 1;
    t = (u128)a->n[3] - b->n[3] - br; o[3] = (u64)t; br = (u64)(t >> 64) & 1;
    /* result is a-b+2^256 when it borrowed; the residue is that minus C */
    const u64 m = (u64)0 - br;
    t = (u128)o[0] - (m & FE_C); o[0] = (u64)t; br = (u64)(t >> 64) & 1;
    t = (u128)o[1] - br; o[1] = (u64)t; br = (u64)(t >> 64) & 1;
    t = (u128)o[2] - br; o[2] = (u64)t; br = (u64)(t >> 64) & 1;
    t = (u128)o[3] - br; o[3] = (u64)t; br = (u64)(t >> 64) & 1;
    if (UNLIKELY(br)) {                             /* borrowed again: subtract C once more */
        t = (u128)o[0] - FE_C; o[0] = (u64)t; br = (u64)(t >> 64) & 1;
        t = (u128)o[1] - br; o[1] = (u64)t; br = (u64)(t >> 64) & 1;
        t = (u128)o[2] - br; o[2] = (u64)t; br = (u64)(t >> 64) & 1;
        o[3] -= br;
    }
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

#if defined(__x86_64__) && defined(__BMI2__) && defined(__ADX__) && !defined(PZ_NO_ASM)
/* Straight-line x86-64 versions of fe_add / fe_sub: the compiler's u128 carry chains cost about twice as much.
 * A carry (borrow) out of bit 256 is folded back as +-C with a mask; if that fold wraps again the value is tiny
 * (< C), so a second mask-and-add of a single limb is exact and needs no carry propagation. */
#define PZ_FE_ADDSUB_ASM 1
static inline void fe_add(fe *r, const fe *a, const fe *b)
{
    u64 o0, o1, o2, o3, m, c;
    __asm__(
        "movq %[a0], %[o0]\n\t"
        "movq %[a1], %[o1]\n\t"
        "movq %[a2], %[o2]\n\t"
        "movq %[a3], %[o3]\n\t"
        "addq %[b0], %[o0]\n\t"
        "adcq %[b1], %[o1]\n\t"
        "adcq %[b2], %[o2]\n\t"
        "adcq %[b3], %[o3]\n\t"
        "sbbq %[m], %[m]\n\t"
        "movabsq $0x1000003D1, %[c]\n\t"
        "andq %[m], %[c]\n\t"
        "addq %[c], %[o0]\n\t"
        "adcq $0, %[o1]\n\t"
        "adcq $0, %[o2]\n\t"
        "adcq $0, %[o3]\n\t"
        "sbbq %[m], %[m]\n\t"
        "movabsq $0x1000003D1, %[c]\n\t"
        "andq %[m], %[c]\n\t"
        "addq %[c], %[o0]\n\t"
        : [o0] "=&r"(o0), [o1] "=&r"(o1), [o2] "=&r"(o2), [o3] "=&r"(o3), [m] "=&r"(m), [c] "=&r"(c)
        : [a0] "m"(a->n[0]), [a1] "m"(a->n[1]), [a2] "m"(a->n[2]), [a3] "m"(a->n[3]), [b0] "m"(b->n[0]), [b1] "m"(b->n[1]), [b2] "m"(b->n[2]),
          [b3] "m"(b->n[3])
        : "cc");
    r->n[0] = o0; r->n[1] = o1; r->n[2] = o2; r->n[3] = o3;
}

static inline void fe_sub(fe *r, const fe *a, const fe *b)
{
    u64 o0, o1, o2, o3, m, c;
    __asm__(
        "movq %[a0], %[o0]\n\t"
        "movq %[a1], %[o1]\n\t"
        "movq %[a2], %[o2]\n\t"
        "movq %[a3], %[o3]\n\t"
        "subq %[b0], %[o0]\n\t"
        "sbbq %[b1], %[o1]\n\t"
        "sbbq %[b2], %[o2]\n\t"
        "sbbq %[b3], %[o3]\n\t"
        "sbbq %[m], %[m]\n\t"
        "movabsq $0x1000003D1, %[c]\n\t"
        "andq %[m], %[c]\n\t"
        "subq %[c], %[o0]\n\t"
        "sbbq $0, %[o1]\n\t"
        "sbbq $0, %[o2]\n\t"
        "sbbq $0, %[o3]\n\t"
        "sbbq %[m], %[m]\n\t"
        "movabsq $0x1000003D1, %[c]\n\t"
        "andq %[m], %[c]\n\t"
        "subq %[c], %[o0]\n\t"
        : [o0] "=&r"(o0), [o1] "=&r"(o1), [o2] "=&r"(o2), [o3] "=&r"(o3), [m] "=&r"(m), [c] "=&r"(c)
        : [a0] "m"(a->n[0]), [a1] "m"(a->n[1]), [a2] "m"(a->n[2]), [a3] "m"(a->n[3]), [b0] "m"(b->n[0]), [b1] "m"(b->n[1]), [b2] "m"(b->n[2]),
          [b3] "m"(b->n[3])
        : "cc");
    r->n[0] = o0; r->n[1] = o1; r->n[2] = o2; r->n[3] = o3;
}
#endif

#ifndef PZ_FE_ADDSUB_ASM
static inline void fe_add(fe *r, const fe *a, const fe *b) { fe_add_c(r, a, b); }
static inline void fe_sub(fe *r, const fe *a, const fe *b) { fe_sub_c(r, a, b); }
#endif

static inline void fe_neg(fe *r, const fe *a)
{
    fe z;
    fe_zero(&z);
    fe_sub(r, &z, a);
}

/* Reduce a 512-bit product l[0..7] modulo p into r. */
static inline void fe_reduce512_(fe *r, const u64 l[8])
{
    u64 o[4];
    u128 t = (u128)l[4] * FE_C + l[0];
    o[0] = (u64)t;
    t = (t >> 64) + (u128)l[5] * FE_C + l[1]; o[1] = (u64)t;
    t = (t >> 64) + (u128)l[6] * FE_C + l[2]; o[2] = (u64)t;
    t = (t >> 64) + (u128)l[7] * FE_C + l[3]; o[3] = (u64)t;
    u64 c = (u64)(t >> 64);   /* < 2^34 */
    fe_fold_(o, c);
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

static inline void fe_mul_c(fe *r, const fe *a, const fe *b)
{
    u64 l[8];
    u128 t;
    u64 c;
    const u64 b0 = b->n[0], b1 = b->n[1], b2 = b->n[2], b3 = b->n[3];
    /* row 0 */
    t = (u128)a->n[0] * b0; l[0] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[0] * b1 + c; l[1] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[0] * b2 + c; l[2] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[0] * b3 + c; l[3] = (u64)t; l[4] = (u64)(t >> 64);
    /* row 1 */
    t = (u128)a->n[1] * b0 + l[1]; l[1] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[1] * b1 + l[2] + c; l[2] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[1] * b2 + l[3] + c; l[3] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[1] * b3 + l[4] + c; l[4] = (u64)t; l[5] = (u64)(t >> 64);
    /* row 2 */
    t = (u128)a->n[2] * b0 + l[2]; l[2] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[2] * b1 + l[3] + c; l[3] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[2] * b2 + l[4] + c; l[4] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[2] * b3 + l[5] + c; l[5] = (u64)t; l[6] = (u64)(t >> 64);
    /* row 3 */
    t = (u128)a->n[3] * b0 + l[3]; l[3] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[3] * b1 + l[4] + c; l[4] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[3] * b2 + l[5] + c; l[5] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a->n[3] * b3 + l[6] + c; l[6] = (u64)t; l[7] = (u64)(t >> 64);
    fe_reduce512_(r, l);
}

static inline void fe_sqr_c(fe *r, const fe *a)
{
    const u64 a0 = a->n[0], a1 = a->n[1], a2 = a->n[2], a3 = a->n[3];
    u64 l[8];
    u128 t;
    u64 c;
    /* off-diagonal products */
    t = (u128)a0 * a1; l[1] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a0 * a2 + c; l[2] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a0 * a3 + c; l[3] = (u64)t; l[4] = (u64)(t >> 64);
    t = (u128)a1 * a2 + l[3]; l[3] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a1 * a3 + l[4] + c; l[4] = (u64)t; l[5] = (u64)(t >> 64);
    t = (u128)a2 * a3 + l[5]; l[5] = (u64)t; l[6] = (u64)(t >> 64);
    /* double */
    l[7] = l[6] >> 63;
    l[6] = l[6] << 1 | l[5] >> 63;
    l[5] = l[5] << 1 | l[4] >> 63;
    l[4] = l[4] << 1 | l[3] >> 63;
    l[3] = l[3] << 1 | l[2] >> 63;
    l[2] = l[2] << 1 | l[1] >> 63;
    l[1] = l[1] << 1;
    /* add squares */
    t = (u128)a0 * a0; l[0] = (u64)t; c = (u64)(t >> 64);
    t = (u128)l[1] + c; l[1] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a1 * a1 + l[2] + c; l[2] = (u64)t; c = (u64)(t >> 64);
    t = (u128)l[3] + c; l[3] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a2 * a2 + l[4] + c; l[4] = (u64)t; c = (u64)(t >> 64);
    t = (u128)l[5] + c; l[5] = (u64)t; c = (u64)(t >> 64);
    t = (u128)a3 * a3 + l[6] + c; l[6] = (u64)t; c = (u64)(t >> 64);
    l[7] += c;
    fe_reduce512_(r, l);
}


/* Not with clang (Apple clang on Intel Macs): this asm needs 14 registers at once and clang fails with "inline assembly requires
 * more registers than available" when the function around it also needs a frame pointer or a realigned stack. */
#if defined(__x86_64__) && defined(__BMI2__) && defined(__ADX__) && !defined(PZ_NO_ASM) && !defined(__clang__)
#define PZ_FE_ASM 1
/* 4x4-limb multiplication with MULX and the two independent carry chains of ADX
 * (ADCX/ADOX), followed by the secp256k1 reduction 2^256 = 2^32 + 977. Inputs are
 * read before outputs are written, so r may alias a or b. */
static inline void fe_mul(fe *r, const fe *a, const fe *b)
{
    u64 l0, l1, l2, l3, l4, l5, l6, l7, t0, t1, rdx_;
    __asm__ volatile(
        /* row 0 */
        "movq %[b0], %%rdx\n\t"
        "mulxq %[a0], %[l0], %[l1]\n\t"
        "mulxq %[a1], %[t0], %[l2]\n\t"
        "addq %[t0], %[l1]\n\t"
        "mulxq %[a2], %[t0], %[l3]\n\t"
        "adcq %[t0], %[l2]\n\t"
        "mulxq %[a3], %[t0], %[l4]\n\t"
        "adcq %[t0], %[l3]\n\t"
        "adcq $0, %[l4]\n\t"
        /* row 1 */
        "movq %[b1], %%rdx\n\t"
        "xorl %k[l5], %k[l5]\n\t"
        "mulxq %[a0], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l1]\n\t"
        "adoxq %[t1], %[l2]\n\t"
        "mulxq %[a1], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l2]\n\t"
        "adoxq %[t1], %[l3]\n\t"
        "mulxq %[a2], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l3]\n\t"
        "adoxq %[t1], %[l4]\n\t"
        "mulxq %[a3], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l4]\n\t"
        "adoxq %[t1], %[l5]\n\t"
        "movl $0, %k[t0]\n\t"
        "adcxq %[t0], %[l5]\n\t"
        /* row 2 */
        "movq %[b2], %%rdx\n\t"
        "xorl %k[l6], %k[l6]\n\t"
        "mulxq %[a0], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l2]\n\t"
        "adoxq %[t1], %[l3]\n\t"
        "mulxq %[a1], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l3]\n\t"
        "adoxq %[t1], %[l4]\n\t"
        "mulxq %[a2], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l4]\n\t"
        "adoxq %[t1], %[l5]\n\t"
        "mulxq %[a3], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l5]\n\t"
        "adoxq %[t1], %[l6]\n\t"
        "movl $0, %k[t0]\n\t"
        "adcxq %[t0], %[l6]\n\t"
        /* row 3 */
        "movq %[b3], %%rdx\n\t"
        "xorl %k[l7], %k[l7]\n\t"
        "mulxq %[a0], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l3]\n\t"
        "adoxq %[t1], %[l4]\n\t"
        "mulxq %[a1], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l4]\n\t"
        "adoxq %[t1], %[l5]\n\t"
        "mulxq %[a2], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l5]\n\t"
        "adoxq %[t1], %[l6]\n\t"
        "mulxq %[a3], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l6]\n\t"
        "adoxq %[t1], %[l7]\n\t"
        "movl $0, %k[t0]\n\t"
        "adcxq %[t0], %[l7]\n\t"
        /* reduction: low + high * 0x1000003D1 */
        "movabsq $0x1000003D1, %%rdx\n\t"
        "xorl %k[t0], %k[t0]\n\t"
        "mulxq %[l4], %[t0], %[t1]\n\t"
        "movl $0, %k[l4]\n\t"
        "adcxq %[t0], %[l0]\n\t"
        "adoxq %[t1], %[l1]\n\t"
        "mulxq %[l5], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l1]\n\t"
        "adoxq %[t1], %[l2]\n\t"
        "mulxq %[l6], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l2]\n\t"
        "adoxq %[t1], %[l3]\n\t"
        "mulxq %[l7], %[t0], %[t1]\n\t"
        "adcxq %[t0], %[l3]\n\t"
        "adoxq %[t1], %[l4]\n\t"
        "movl $0, %k[t0]\n\t"
        "adcxq %[t0], %[l4]\n\t"
        /* fold the top limb (< 2^34) once more */
        "mulxq %[l4], %[t0], %[t1]\n\t"
        "addq %[t0], %[l0]\n\t"
        "adcq %[t1], %[l1]\n\t"
        "adcq $0, %[l2]\n\t"
        "adcq $0, %[l3]\n\t"
        "sbbq %[t0], %[t0]\n\t"
        "andq %%rdx, %[t0]\n\t"
        "addq %[t0], %[l0]\n\t"
        "adcq $0, %[l1]\n\t"
        "adcq $0, %[l2]\n\t"
        "adcq $0, %[l3]\n\t"
        "movq %[l0], %[r0]\n\t"
        "movq %[l1], %[r1]\n\t"
        "movq %[l2], %[r2]\n\t"
        "movq %[l3], %[r3]\n\t"
        : [l0] "=&r"(l0), [l1] "=&r"(l1), [l2] "=&r"(l2), [l3] "=&r"(l3), [l4] "=&r"(l4), [l5] "=&r"(l5), [l6] "=&r"(l6), [l7] "=&r"(l7),
          [t0] "=&r"(t0), [t1] "=&r"(t1), "=&d"(rdx_), [r0] "=m"(r->n[0]), [r1] "=m"(r->n[1]), [r2] "=m"(r->n[2]), [r3] "=m"(r->n[3])
        : [a0] "m"(a->n[0]), [a1] "m"(a->n[1]), [a2] "m"(a->n[2]), [a3] "m"(a->n[3]), [b0] "m"(b->n[0]), [b1] "m"(b->n[1]), [b2] "m"(b->n[2]),
          [b3] "m"(b->n[3])
        : "cc");
}
static inline void fe_sqr(fe *r, const fe *a) { fe_mul(r, a, a); }
#else
static inline void fe_mul(fe *r, const fe *a, const fe *b) { fe_mul_c(r, a, b); }
static inline void fe_sqr(fe *r, const fe *a) { fe_sqr_c(r, a); }
#endif

static inline void fe_sqr_n(fe *r, const fe *a, int n)
{
    fe_sqr(r, a);
    for (int i = 1; i < n; i++) fe_sqr(r, r);
}

/* Canonical reduction into [0, p). */
static inline void fe_normalize(fe *r)
{
    u128 t = (u128)r->n[0] + FE_C;
    u64 o0 = (u64)t;
    t = (t >> 64) + r->n[1]; u64 o1 = (u64)t;
    t = (t >> 64) + r->n[2]; u64 o2 = (u64)t;
    t = (t >> 64) + r->n[3]; u64 o3 = (u64)t;
    if ((u64)(t >> 64)) {   /* r >= p  <=>  r + C overflows 2^256 */
        r->n[0] = o0; r->n[1] = o1; r->n[2] = o2; r->n[3] = o3;
    }
}

static inline bool fe_is_zero(const fe *a)   /* a must be normalized */
{
    return (a->n[0] | a->n[1] | a->n[2] | a->n[3]) == 0;
}
static inline bool fe_is_odd(const fe *a) { return a->n[0] & 1; }   /* a must be normalized */
static inline bool fe_eq(const fe *a, const fe *b)   /* both normalized */
{
    return ((a->n[0] ^ b->n[0]) | (a->n[1] ^ b->n[1]) | (a->n[2] ^ b->n[2]) | (a->n[3] ^ b->n[3])) == 0;
}
/* true if a == b mod p regardless of representation */
static inline bool fe_equiv(const fe *a, const fe *b)
{
    fe x = *a, y = *b;
    fe_normalize(&x);
    fe_normalize(&y);
    return fe_eq(&x, &y);
}

static inline void fe_from_u256(fe *r, const u256 *v)
{
    for (int i = 0; i < 4; i++) r->n[i] = v->w[i];
    fe_normalize(r);
}
static inline void fe_to_u256(u256 *v, const fe *a)
{
    fe t = *a;
    fe_normalize(&t);
    for (int i = 0; i < 4; i++) v->w[i] = t.n[i];
}

/* big-endian 32-byte serialization (normalizes). */
static inline void fe_to_be(u8 out[32], const fe *a)
{
    u256 v;
    fe_to_u256(&v, a);
    u256_to_be(&v, out);
}
static inline void fe_from_be(fe *r, const u8 in[32])
{
    u256 v;
    u256_from_be(&v, in);
    fe_from_u256(r, &v);
}

/* r = a^-1 mod p (a != 0). Addition chain from libsecp256k1 (255 sqr + 15 mul). */
static inline void fe_inv(fe *r, const fe *a)
{
    fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    fe_sqr(&x2, a);            fe_mul(&x2, &x2, a);
    fe_sqr(&x3, &x2);          fe_mul(&x3, &x3, a);
    fe_sqr_n(&x6, &x3, 3);     fe_mul(&x6, &x6, &x3);
    fe_sqr_n(&x9, &x6, 3);     fe_mul(&x9, &x9, &x3);
    fe_sqr_n(&x11, &x9, 2);    fe_mul(&x11, &x11, &x2);
    fe_sqr_n(&x22, &x11, 11);  fe_mul(&x22, &x22, &x11);
    fe_sqr_n(&x44, &x22, 22);  fe_mul(&x44, &x44, &x22);
    fe_sqr_n(&x88, &x44, 44);  fe_mul(&x88, &x88, &x44);
    fe_sqr_n(&x176, &x88, 88); fe_mul(&x176, &x176, &x88);
    fe_sqr_n(&x220, &x176, 44); fe_mul(&x220, &x220, &x44);
    fe_sqr_n(&x223, &x220, 3); fe_mul(&x223, &x223, &x3);
    fe_sqr_n(&t, &x223, 23);   fe_mul(&t, &t, &x22);
    fe_sqr_n(&t, &t, 5);       fe_mul(&t, &t, a);
    fe_sqr_n(&t, &t, 3);       fe_mul(&t, &t, &x2);
    fe_sqr_n(&t, &t, 2);       fe_mul(r, &t, a);
}

/* Square root: r = a^((p+1)/4). Returns true if r^2 == a. Not performance critical. */
bool fe_sqrt(fe *r, const fe *a);

/* Montgomery batch inversion of n elements in place; tmp must hold n elements.
 * No element may be zero (mod p). Returns false and leaves a[] untouched if
 * the total product is zero. */
static inline bool fe_inv_batch(fe *a, fe *tmp, size_t n)
{
    if (n == 0) return true;
    fe acc = a[0];
    tmp[0] = a[0];
    for (size_t i = 1; i < n; i++) {
        fe_mul(&acc, &acc, &a[i]);
        tmp[i] = acc;
    }
    fe chk = acc;
    fe_normalize(&chk);
    if (UNLIKELY(fe_is_zero(&chk))) return false;
    fe inv;
    fe_inv(&inv, &acc);
    for (size_t i = n - 1; i > 0; i--) {
        fe t;
        fe_mul(&t, &inv, &tmp[i - 1]);
        fe_mul(&inv, &inv, &a[i]);
        a[i] = t;
    }
    a[0] = inv;
    return true;
}

#endif
