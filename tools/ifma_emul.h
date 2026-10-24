/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* test-only: emulation of vpmadd52luq/vpmadd52huq with AVX-512F, to run the IFMA code paths on a CPU without IFMA */
#include <immintrin.h>
#pragma GCC diagnostic ignored "-Wpsabi"
static inline __m512i pz_emul_52(__m512i a, __m512i b, __m512i *hi_out)
{
    const __m512i M26 = _mm512_set1_epi64((1LL << 26) - 1), M52 = _mm512_set1_epi64((1LL << 52) - 1);
    const __m512i a52 = _mm512_and_si512(a, M52), b52 = _mm512_and_si512(b, M52);
    const __m512i ah = _mm512_srli_epi64(a52, 26), al = _mm512_and_si512(a52, M26);
    const __m512i bh = _mm512_srli_epi64(b52, 26), bl = _mm512_and_si512(b52, M26);
    const __m512i ll = _mm512_mul_epu32(al, bl), hh = _mm512_mul_epu32(ah, bh);
    const __m512i mid = _mm512_add_epi64(_mm512_mul_epu32(ah, bl), _mm512_mul_epu32(al, bh));
    const __m512i lo = _mm512_add_epi64(ll, _mm512_slli_epi64(_mm512_and_si512(mid, M26), 26));
    *hi_out = _mm512_add_epi64(_mm512_add_epi64(hh, _mm512_srli_epi64(mid, 26)), _mm512_srli_epi64(lo, 52));
    return _mm512_and_si512(lo, M52);
}
static inline __m512i pz_emul_madd52lo(__m512i c, __m512i a, __m512i b) { __m512i h; const __m512i l = pz_emul_52(a, b, &h); return _mm512_add_epi64(c, l); }
static inline __m512i pz_emul_madd52hi(__m512i c, __m512i a, __m512i b) { __m512i h; (void)pz_emul_52(a, b, &h); return _mm512_add_epi64(c, h); }
#define _mm512_madd52lo_epu64 pz_emul_madd52lo
#define _mm512_madd52hi_epu64 pz_emul_madd52hi
