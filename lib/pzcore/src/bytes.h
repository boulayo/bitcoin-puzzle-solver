/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bytes.h - words from and to byte strings at any alignment (private to the library). They compile to a load or store and, where the
 * byte order differs, a byte swap; on a machine with another byte order they fall back to shifts. */
#ifndef PZCORE_BYTES_H
#define PZCORE_BYTES_H

#include <pzcore/defs.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PZ_LITTLE_ENDIAN_HOST 1
#endif

static inline uint32_t load_be32(const uint8_t *p)
{
#ifdef PZ_LITTLE_ENDIAN_HOST
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
#else
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
#endif
}

static inline uint32_t load_le32(const uint8_t *p)
{
#ifdef PZ_LITTLE_ENDIAN_HOST
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
#else
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
#endif
}

static inline void store_be32(uint8_t *p, uint32_t v)
{
#ifdef PZ_LITTLE_ENDIAN_HOST
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
#else
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
#endif
}

static inline void store_le32(uint8_t *p, uint32_t v)
{
#ifdef PZ_LITTLE_ENDIAN_HOST
    memcpy(p, &v, 4);
#else
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
#endif
}

#endif
