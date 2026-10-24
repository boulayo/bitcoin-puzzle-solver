/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* fe.h - arithmetic in GF(p), p = 2^256 - 2^32 - 977 (the secp256k1 field).
 *
 * Field elements are four 64-bit limbs (least significant first). Values are "lazy": any 256-bit pattern is a valid
 * representative of its residue, and the operations accept and return such values; pz_fe_normalize() produces the canonical
 * value in [0, p). The functions that need a canonical value (is_zero, is_odd, eq, to_be, the comparisons) say so.
 *
 * Everything but pz_fe_sqrt is static inline, so every translation unit gets code compiled for its own instruction set; on
 * x86-64 with BMI2 and ADX (-mbmi2 -madx, e.g. -march=haswell or newer) the multiplication, the addition and the subtraction are
 * straight-line assembly. The output may alias any input of a function. Nothing here is constant-time, so do not use it with
 * secret values where a side channel matters. */
#ifndef PZCORE_FE_H
#define PZCORE_FE_H

#include <pzcore/defs.h>
#include <pzcore/u256.h>

PZ_BEGIN_DECLS

typedef struct { uint64_t n[4]; } pz_fe;

#define PZ_FE_C 0x1000003D1ULL   /* 2^256 mod p */

static const pz_fe pz_fe_p PZ_UNUSED = {{0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}};

static inline void pz_fe_set_u64(pz_fe *r, uint64_t v) { r->n[0] = v; r->n[1] = r->n[2] = r->n[3] = 0; }
static inline void pz_fe_zero(pz_fe *r) { pz_fe_set_u64(r, 0); }
static inline void pz_fe_copy(pz_fe *r, const pz_fe *a) { *r = *a; }

/* r += c * PZ_FE_C with full carry propagation, c small. Keeps r < 2^256. */
static inline void pz_fe_fold_(uint64_t r[4], uint64_t c)
{
    pz_u128 t = (pz_u128)c * PZ_FE_C + r[0];
    r[0] = (uint64_t)t;
    uint64_t k = (uint64_t)(t >> 64);
    t = (pz_u128)r[1] + k; r[1] = (uint64_t)t; k = (uint64_t)(t >> 64);
    t = (pz_u128)r[2] + k; r[2] = (uint64_t)t; k = (uint64_t)(t >> 64);
    t = (pz_u128)r[3] + k; r[3] = (uint64_t)t; k = (uint64_t)(t >> 64);
    if (PZ_UNLIKELY(k)) {
        /* wrapped past 2^256: the remaining value is tiny, adding PZ_FE_C is safe */
        t = (pz_u128)r[0] + PZ_FE_C; r[0] = (uint64_t)t; k = (uint64_t)(t >> 64);
        t = (pz_u128)r[1] + k; r[1] = (uint64_t)t; k = (uint64_t)(t >> 64);
        t = (pz_u128)r[2] + k; r[2] = (uint64_t)t; k = (uint64_t)(t >> 64);
        r[3] += k;
    }
}

/* Branch-free in the common case: a carry (or borrow) out of the 256-bit operation is
 * folded back with a mask instead of a data-dependent branch, because that event
 * happens for about half of random operands and would be mispredicted constantly.
 * The (astronomically rare) second wrap is handled by a predicted-not-taken branch. */
static inline void pz_fe_add_c(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t o[4];
    pz_u128 t = (pz_u128)a->n[0] + b->n[0];
    o[0] = (uint64_t)t;
    t = (t >> 64) + a->n[1] + b->n[1]; o[1] = (uint64_t)t;
    t = (t >> 64) + a->n[2] + b->n[2]; o[2] = (uint64_t)t;
    t = (t >> 64) + a->n[3] + b->n[3]; o[3] = (uint64_t)t;
    const uint64_t m = (uint64_t)0 - (uint64_t)(t >> 64);          /* all ones if the sum wrapped */
    t = (pz_u128)o[0] + (m & PZ_FE_C); o[0] = (uint64_t)t;
    t = (t >> 64) + o[1]; o[1] = (uint64_t)t;
    t = (t >> 64) + o[2]; o[2] = (uint64_t)t;
    t = (t >> 64) + o[3]; o[3] = (uint64_t)t;
    if (PZ_UNLIKELY((uint64_t)(t >> 64))) {                 /* wrapped again: value is tiny, adding C is safe */
        t = (pz_u128)o[0] + PZ_FE_C; o[0] = (uint64_t)t;
        t = (t >> 64) + o[1]; o[1] = (uint64_t)t;
        t = (t >> 64) + o[2]; o[2] = (uint64_t)t;
        o[3] += (uint64_t)(t >> 64);
    }
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

static inline void pz_fe_sub_c(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t o[4];
    pz_u128 t = (pz_u128)a->n[0] - b->n[0];
    o[0] = (uint64_t)t;
    uint64_t br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)a->n[1] - b->n[1] - br; o[1] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)a->n[2] - b->n[2] - br; o[2] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)a->n[3] - b->n[3] - br; o[3] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    /* result is a-b+2^256 when it borrowed; the residue is that minus C */
    const uint64_t m = (uint64_t)0 - br;
    t = (pz_u128)o[0] - (m & PZ_FE_C); o[0] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)o[1] - br; o[1] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)o[2] - br; o[2] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    t = (pz_u128)o[3] - br; o[3] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
    if (PZ_UNLIKELY(br)) {                             /* borrowed again: subtract C once more */
        t = (pz_u128)o[0] - PZ_FE_C; o[0] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
        t = (pz_u128)o[1] - br; o[1] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
        t = (pz_u128)o[2] - br; o[2] = (uint64_t)t; br = (uint64_t)(t >> 64) & 1;
        o[3] -= br;
    }
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

#if defined(__x86_64__) && defined(__BMI2__) && defined(__ADX__) && !defined(PZ_NO_ASM)
/* Straight-line x86-64 versions of pz_fe_add / pz_fe_sub: the compiler's pz_u128 carry chains cost about twice as much.
 * A carry (borrow) out of bit 256 is folded back as +-C with a mask; if that fold wraps again the value is tiny
 * (< C), so a second mask-and-add of a single limb is exact and needs no carry propagation. */
#define PZ_FE_ADDSUB_ASM 1
static inline void pz_fe_add(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t o0, o1, o2, o3, m, c;
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

static inline void pz_fe_sub(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t o0, o1, o2, o3, m, c;
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
static inline void pz_fe_add(pz_fe *r, const pz_fe *a, const pz_fe *b) { pz_fe_add_c(r, a, b); }
static inline void pz_fe_sub(pz_fe *r, const pz_fe *a, const pz_fe *b) { pz_fe_sub_c(r, a, b); }
#endif

static inline void pz_fe_neg(pz_fe *r, const pz_fe *a)
{
    pz_fe z;
    pz_fe_zero(&z);
    pz_fe_sub(r, &z, a);
}

/* Reduce a 512-bit product l[0..7] modulo p into r. */
static inline void pz_fe_reduce512_(pz_fe *r, const uint64_t l[8])
{
    uint64_t o[4];
    pz_u128 t = (pz_u128)l[4] * PZ_FE_C + l[0];
    o[0] = (uint64_t)t;
    t = (t >> 64) + (pz_u128)l[5] * PZ_FE_C + l[1]; o[1] = (uint64_t)t;
    t = (t >> 64) + (pz_u128)l[6] * PZ_FE_C + l[2]; o[2] = (uint64_t)t;
    t = (t >> 64) + (pz_u128)l[7] * PZ_FE_C + l[3]; o[3] = (uint64_t)t;
    uint64_t c = (uint64_t)(t >> 64);   /* < 2^34 */
    pz_fe_fold_(o, c);
    r->n[0] = o[0]; r->n[1] = o[1]; r->n[2] = o[2]; r->n[3] = o[3];
}

static inline void pz_fe_mul_c(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t l[8];
    pz_u128 t;
    uint64_t c;
    const uint64_t b0 = b->n[0], b1 = b->n[1], b2 = b->n[2], b3 = b->n[3];
    /* row 0 */
    t = (pz_u128)a->n[0] * b0; l[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[0] * b1 + c; l[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[0] * b2 + c; l[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[0] * b3 + c; l[3] = (uint64_t)t; l[4] = (uint64_t)(t >> 64);
    /* row 1 */
    t = (pz_u128)a->n[1] * b0 + l[1]; l[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[1] * b1 + l[2] + c; l[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[1] * b2 + l[3] + c; l[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[1] * b3 + l[4] + c; l[4] = (uint64_t)t; l[5] = (uint64_t)(t >> 64);
    /* row 2 */
    t = (pz_u128)a->n[2] * b0 + l[2]; l[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[2] * b1 + l[3] + c; l[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[2] * b2 + l[4] + c; l[4] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[2] * b3 + l[5] + c; l[5] = (uint64_t)t; l[6] = (uint64_t)(t >> 64);
    /* row 3 */
    t = (pz_u128)a->n[3] * b0 + l[3]; l[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[3] * b1 + l[4] + c; l[4] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[3] * b2 + l[5] + c; l[5] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a->n[3] * b3 + l[6] + c; l[6] = (uint64_t)t; l[7] = (uint64_t)(t >> 64);
    pz_fe_reduce512_(r, l);
}

static inline void pz_fe_sqr_c(pz_fe *r, const pz_fe *a)
{
    const uint64_t a0 = a->n[0], a1 = a->n[1], a2 = a->n[2], a3 = a->n[3];
    uint64_t l[8];
    pz_u128 t;
    uint64_t c;
    /* off-diagonal products */
    t = (pz_u128)a0 * a1; l[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a0 * a2 + c; l[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a0 * a3 + c; l[3] = (uint64_t)t; l[4] = (uint64_t)(t >> 64);
    t = (pz_u128)a1 * a2 + l[3]; l[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a1 * a3 + l[4] + c; l[4] = (uint64_t)t; l[5] = (uint64_t)(t >> 64);
    t = (pz_u128)a2 * a3 + l[5]; l[5] = (uint64_t)t; l[6] = (uint64_t)(t >> 64);
    /* double */
    l[7] = l[6] >> 63;
    l[6] = l[6] << 1 | l[5] >> 63;
    l[5] = l[5] << 1 | l[4] >> 63;
    l[4] = l[4] << 1 | l[3] >> 63;
    l[3] = l[3] << 1 | l[2] >> 63;
    l[2] = l[2] << 1 | l[1] >> 63;
    l[1] = l[1] << 1;
    /* add squares */
    t = (pz_u128)a0 * a0; l[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)l[1] + c; l[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a1 * a1 + l[2] + c; l[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)l[3] + c; l[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a2 * a2 + l[4] + c; l[4] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)l[5] + c; l[5] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (pz_u128)a3 * a3 + l[6] + c; l[6] = (uint64_t)t; c = (uint64_t)(t >> 64);
    l[7] += c;
    pz_fe_reduce512_(r, l);
}


/* Not with clang (Apple clang on Intel Macs): this asm needs 14 registers at once and clang fails with "inline assembly requires
 * more registers than available" when the function around it also needs a frame pointer or a realigned stack. Not without
 * optimization either: GCC at -O0 keeps every variable in memory and rejects the asm for the same reason ("impossible constraints"),
 * so a debug build of a program that includes this header with -mbmi2 -madx uses the C multiplication. */
#if defined(__x86_64__) && defined(__BMI2__) && defined(__ADX__) && !defined(PZ_NO_ASM) && !defined(__clang__) && defined(__OPTIMIZE__)
#define PZ_FE_ASM 1
/* 4x4-limb multiplication with MULX and the two independent carry chains of ADX
 * (ADCX/ADOX), followed by the secp256k1 reduction 2^256 = 2^32 + 977. Inputs are
 * read before outputs are written, so r may alias a or b. */
static inline void pz_fe_mul(pz_fe *r, const pz_fe *a, const pz_fe *b)
{
    uint64_t l0, l1, l2, l3, l4, l5, l6, l7, t0, t1, rdx_;
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
#else
static inline void pz_fe_mul(pz_fe *r, const pz_fe *a, const pz_fe *b) { pz_fe_mul_c(r, a, b); }
#endif

#if defined(__x86_64__) && defined(__BMI2__) && !defined(PZ_NO_ASM)
#include <immintrin.h>
/* Squaring with MULX: the six different products once, doubled, plus the four squares, then the reduction. It takes 11.5 ns where the
 * product of a number with itself takes 15.8 ns and the portable pz_fe_sqr_c 13.9 (an inversion is 255 squarings). Written with
 * intrinsics, so it also builds with clang, where the assembly multiplication above is not used. r may alias a. */
#define PZ_FE_SQR_MULX 1
static inline void pz_fe_sqr(pz_fe *r, const pz_fe *a)
{
    typedef unsigned long long ull;
    const ull a0 = a->n[0], a1 = a->n[1], a2 = a->n[2], a3 = a->n[3];
    ull h01, h02, h03, h12, h13, h23, d0h, d1h, d2h, d3h;
    ull l[8];
    const ull l01 = _mulx_u64(a0, a1, &h01), l02 = _mulx_u64(a0, a2, &h02), l03 = _mulx_u64(a0, a3, &h03);
    const ull l12 = _mulx_u64(a1, a2, &h12), l13 = _mulx_u64(a1, a3, &h13), l23 = _mulx_u64(a2, a3, &h23);
    ull s2, s3, s4, s5, s6, p3, p4, p5, p6;
    unsigned char c;
    c = _addcarry_u64(0, h01, l02, &s2);                  /* a0a1 + a0a2 + a0a3, columns 1 to 4 (column 1 is l01) */
    c = _addcarry_u64(c, h02, l03, &s3);
    (void)_addcarry_u64(c, h03, 0, &s4);
    p3 = l12;                                             /* a1a2 + a1a3 + a2a3, columns 3 to 6 */
    c = _addcarry_u64(0, h12, l13, &p4);
    c = _addcarry_u64(c, h13, l23, &p5);
    (void)_addcarry_u64(c, h23, 0, &p6);
    c = _addcarry_u64(0, s3, p3, &s3);                    /* the two sums together: columns 1 to 6 */
    c = _addcarry_u64(c, s4, p4, &s4);
    c = _addcarry_u64(c, 0, p5, &s5);
    (void)_addcarry_u64(c, 0, p6, &s6);
    l[7] = s6 >> 63;                                      /* twice that, ... */
    l[6] = s6 << 1 | s5 >> 63;
    l[5] = s5 << 1 | s4 >> 63;
    l[4] = s4 << 1 | s3 >> 63;
    l[3] = s3 << 1 | s2 >> 63;
    l[2] = s2 << 1 | l01 >> 63;
    l[1] = l01 << 1;
    l[0] = _mulx_u64(a0, a0, &d0h);                       /* ... plus the squares of the limbs */
    const ull d1l = _mulx_u64(a1, a1, &d1h), d2l = _mulx_u64(a2, a2, &d2h), d3l = _mulx_u64(a3, a3, &d3h);
    c = _addcarry_u64(0, l[1], d0h, &l[1]);
    c = _addcarry_u64(c, l[2], d1l, &l[2]);
    c = _addcarry_u64(c, l[3], d1h, &l[3]);
    c = _addcarry_u64(c, l[4], d2l, &l[4]);
    c = _addcarry_u64(c, l[5], d2h, &l[5]);
    c = _addcarry_u64(c, l[6], d3l, &l[6]);
    (void)_addcarry_u64(c, l[7], d3h, &l[7]);
    uint64_t w[8];
    for (int i = 0; i < 8; i++) w[i] = l[i];
    pz_fe_reduce512_(r, w);
}
#else
static inline void pz_fe_sqr(pz_fe *r, const pz_fe *a) { pz_fe_sqr_c(r, a); }
#endif

static inline void pz_fe_sqr_n(pz_fe *r, const pz_fe *a, int n)
{
    pz_fe_sqr(r, a);
    for (int i = 1; i < n; i++) pz_fe_sqr(r, r);
}

/* Canonical reduction into [0, p). */
static inline void pz_fe_normalize(pz_fe *r)
{
    pz_u128 t = (pz_u128)r->n[0] + PZ_FE_C;
    uint64_t o0 = (uint64_t)t;
    t = (t >> 64) + r->n[1]; uint64_t o1 = (uint64_t)t;
    t = (t >> 64) + r->n[2]; uint64_t o2 = (uint64_t)t;
    t = (t >> 64) + r->n[3]; uint64_t o3 = (uint64_t)t;
    if ((uint64_t)(t >> 64)) {   /* r >= p  <=>  r + C overflows 2^256 */
        r->n[0] = o0; r->n[1] = o1; r->n[2] = o2; r->n[3] = o3;
    }
}

static inline bool pz_fe_is_zero(const pz_fe *a)   /* a must be normalized */
{
    return (a->n[0] | a->n[1] | a->n[2] | a->n[3]) == 0;
}
static inline bool pz_fe_is_odd(const pz_fe *a) { return a->n[0] & 1; }   /* a must be normalized */
static inline bool pz_fe_eq(const pz_fe *a, const pz_fe *b)   /* both normalized */
{
    return ((a->n[0] ^ b->n[0]) | (a->n[1] ^ b->n[1]) | (a->n[2] ^ b->n[2]) | (a->n[3] ^ b->n[3])) == 0;
}
/* true if a == b mod p regardless of representation */
static inline bool pz_fe_equiv(const pz_fe *a, const pz_fe *b)
{
    pz_fe x = *a, y = *b;
    pz_fe_normalize(&x);
    pz_fe_normalize(&y);
    return pz_fe_eq(&x, &y);
}

static inline void pz_fe_from_u256(pz_fe *r, const pz_u256 *v)
{
    for (int i = 0; i < 4; i++) r->n[i] = v->w[i];
    pz_fe_normalize(r);
}
static inline void pz_fe_to_u256(pz_u256 *v, const pz_fe *a)
{
    pz_fe t = *a;
    pz_fe_normalize(&t);
    for (int i = 0; i < 4; i++) v->w[i] = t.n[i];
}

/* big-endian 32-byte serialization (normalizes). */
static inline void pz_fe_to_be(uint8_t out[32], const pz_fe *a)
{
    pz_u256 v;
    pz_fe_to_u256(&v, a);
    pz_u256_to_be(&v, out);
}
static inline void pz_fe_from_be(pz_fe *r, const uint8_t in[32])
{
    pz_u256 v;
    pz_u256_from_be(&v, in);
    pz_fe_from_u256(r, &v);
}

/* r = a^-1 mod p by an addition chain (255 squarings and 15 multiplications). The inverse of 0 is 0. */
static inline void pz_fe_inv(pz_fe *r, const pz_fe *a)
{
    pz_fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    pz_fe_sqr(&x2, a);            pz_fe_mul(&x2, &x2, a);
    pz_fe_sqr(&x3, &x2);          pz_fe_mul(&x3, &x3, a);
    pz_fe_sqr_n(&x6, &x3, 3);     pz_fe_mul(&x6, &x6, &x3);
    pz_fe_sqr_n(&x9, &x6, 3);     pz_fe_mul(&x9, &x9, &x3);
    pz_fe_sqr_n(&x11, &x9, 2);    pz_fe_mul(&x11, &x11, &x2);
    pz_fe_sqr_n(&x22, &x11, 11);  pz_fe_mul(&x22, &x22, &x11);
    pz_fe_sqr_n(&x44, &x22, 22);  pz_fe_mul(&x44, &x44, &x22);
    pz_fe_sqr_n(&x88, &x44, 44);  pz_fe_mul(&x88, &x88, &x44);
    pz_fe_sqr_n(&x176, &x88, 88); pz_fe_mul(&x176, &x176, &x88);
    pz_fe_sqr_n(&x220, &x176, 44); pz_fe_mul(&x220, &x220, &x44);
    pz_fe_sqr_n(&x223, &x220, 3); pz_fe_mul(&x223, &x223, &x3);
    pz_fe_sqr_n(&t, &x223, 23);   pz_fe_mul(&t, &t, &x22);
    pz_fe_sqr_n(&t, &t, 5);       pz_fe_mul(&t, &t, a);
    pz_fe_sqr_n(&t, &t, 3);       pz_fe_mul(&t, &t, &x2);
    pz_fe_sqr_n(&t, &t, 2);       pz_fe_mul(r, &t, a);
}

/* r = a^((p+1)/4) mod p, normalized. Returns true if r*r == a, that is when a is a square (r is then one of its two roots, the
 * one that is a square itself); on false r is unspecified. Out of line: 253 squarings and 13 multiplications. */
bool pz_fe_sqrt(pz_fe *r, const pz_fe *a);

/* Montgomery batch inversion of the n elements of a, in place, with a single inversion; tmp must hold n elements and not overlap
 * a. If some element is zero (mod p) it returns false and leaves a[] untouched (tmp is overwritten); n == 0 is fine. */
static inline bool pz_fe_inv_batch(pz_fe *a, pz_fe *tmp, size_t n)
{
    if (n == 0) return true;
    pz_fe acc = a[0];
    tmp[0] = a[0];
    for (size_t i = 1; i < n; i++) {
        pz_fe_mul(&acc, &acc, &a[i]);
        tmp[i] = acc;
    }
    pz_fe chk = acc;
    pz_fe_normalize(&chk);
    if (PZ_UNLIKELY(pz_fe_is_zero(&chk))) return false;
    pz_fe inv;
    pz_fe_inv(&inv, &acc);
    for (size_t i = n - 1; i > 0; i--) {
        pz_fe t;
        pz_fe_mul(&t, &inv, &tmp[i - 1]);
        pz_fe_mul(&inv, &inv, &a[i]);
        a[i] = t;
    }
    a[0] = inv;
    return true;
}

PZ_END_DECLS

#endif
