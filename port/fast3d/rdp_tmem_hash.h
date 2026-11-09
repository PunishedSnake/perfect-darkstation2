#ifndef PERFECT_DARK_FAST3D_RDP_TMEM_HASH_H
#define PERFECT_DARK_FAST3D_RDP_TMEM_HASH_H

#include <stdint.h>

/*
 * Content-identity hash shared by the TMEM model and the live texture
 * fingerprint. Two 32-bit Murmur3-style lanes packed into one uint64_t state:
 * the R5900 has no 64-bit multiplier, so only native 32-bit multiplies are
 * used and a whole 64-bit TMEM word is consumed per step. Only equality of
 * identities matters; values are never persisted.
 */
#define GFX_RDP_TMEM_HASH_SEED UINT64_C(0x6a09e667bb67ae85)

static inline uint32_t gfxRdpTmemHashRotl32(uint32_t x, unsigned r)
{
    return (x << r) | (x >> (32u - r));
}

static inline void gfxRdpTmemHashMix(uint64_t *hash, uint32_t a, uint32_t b)
{
    uint32_t h1 = (uint32_t)(*hash >> 32);
    uint32_t h2 = (uint32_t)*hash;

    a *= 0xcc9e2d51u;
    a = gfxRdpTmemHashRotl32(a, 15);
    a *= 0x1b873593u;
    h1 ^= a;
    h1 = gfxRdpTmemHashRotl32(h1, 13);
    h1 = h1 * 5u + 0xe6546b64u;

    b *= 0x85ebca6bu;
    b = gfxRdpTmemHashRotl32(b, 16);
    b *= 0xc2b2ae35u;
    h2 ^= b;
    h2 = gfxRdpTmemHashRotl32(h2, 17);
    h2 = h2 * 5u + 0x52dce729u;

    h1 += h2;
    h2 += h1;
    *hash = ((uint64_t)h1 << 32) | h2;
}

static inline void gfxRdpTmemHashMix64(uint64_t *hash, uint64_t v)
{
    gfxRdpTmemHashMix(hash, (uint32_t)v, (uint32_t)(v >> 32));
}

#endif
