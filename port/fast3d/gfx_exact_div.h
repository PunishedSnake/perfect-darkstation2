#ifndef GFX_EXACT_DIV_H
#define GFX_EXACT_DIV_H

#include <stdint.h>

/*
 * Division by a power of two equals multiplication by its reciprocal: the
 * exact quotient x * 2^-k is the same real number either way, so IEEE
 * round-to-nearest, the R5900's round-toward-zero and its flush-to-zero all
 * produce identical bits. Host-tested by tests/gfx_exact_div_test.cpp.
 */

/* 2^-k for k = 0..10, each exactly representable. */
static const float gfx_pow2_reciprocal[11] = {
    1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f,
    0.0078125f, 0.00390625f, 0.001953125f, 0.0009765625f,
};

/* 1/x when x is a nonzero power of two up to 2^24, else 0 (divide instead). */
static inline float gfx_exact_pow2_reciprocal(uint32_t x) {
    if (x == 0u || (x & (x - 1u)) != 0u || x > (1u << 24)) {
        return 0.0f;
    }
    return 1.0f / (float)x;
}

#endif
