#include "gfx_exact_div.h"

#include <cassert>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

static unsigned int nextRandom(unsigned int *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void checkRoundingMode(int mode)
{
    std::fesetround(mode);
    unsigned int state = 0x50443253u;
    for (int k = 0; k <= 10; ++k) {
        assert(gfx_pow2_reciprocal[k] == 1.0f / (float)(1 << k));
    }
    for (unsigned int i = 0; i < 400000u; ++i) {
        unsigned int bits = nextRandom(&state);
        float x;
        std::memcpy(&x, &bits, sizeof(x));
        if (!std::isfinite(x)) {
            continue;
        }
        for (int k = 0; k <= 10; ++k) {
            const float divisor = (float)(1 << k);
            assert(sameBits(x * gfx_pow2_reciprocal[k], x / divisor));
        }
        for (unsigned int k = 0; k <= 24; ++k) {
            const uint32_t size = 1u << k;
            const float rcp = gfx_exact_pow2_reciprocal(size);
            assert(rcp != 0.0f);
            assert(sameBits(x * rcp, x / (float)size));
        }
    }
    std::fesetround(FE_TONEAREST);
}

int main()
{
    checkRoundingMode(FE_TONEAREST);
    /* The R5900 FPU rounds toward zero; exact quotients are unaffected. */
    checkRoundingMode(FE_TOWARDZERO);

    const uint32_t npot[] = { 0u, 3u, 5u, 6u, 7u, 12u, 24u, 48u, 96u,
        100u, 320u, 1023u, (1u << 24) + 1u, 1u << 25, 0xffffffffu };
    for (uint32_t value : npot) {
        assert(gfx_exact_pow2_reciprocal(value) == 0.0f);
    }
    std::puts("gfx exact division tests passed");
    return 0;
}
