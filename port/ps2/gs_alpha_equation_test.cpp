#include "gs_alpha_equation.h"

#include <assert.h>
#include <stdio.h>

static void test_source_over(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_SOURCE_OVER, &factors));
    assert(factors.a == 0u);
    assert(factors.b == 1u);
    assert(factors.c == 0u);
    assert(factors.d == 1u);
    assert(factors.fix == 0u);
}

static void test_source_rgb_times_inverse_source_alpha(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_SOURCE_RGB_TIMES_INV_SOURCE_ALPHA, &factors));
    assert(factors.a == 2u);
    assert(factors.b == 0u);
    assert(factors.c == 0u);
    assert(factors.d == 0u);
    assert(factors.fix == 0u);
}

static void test_destination_alpha_lerp(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP, &factors));
    assert(factors.a == 0u);
    assert(factors.b == 1u);
    assert(factors.c == 1u);
    assert(factors.d == 1u);
    assert(factors.fix == 0u);
}

static void test_destination_rgb_multiply(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_DESTINATION_ALPHA,
        &factors));
    assert(factors.a == 1u);
    assert(factors.b == 2u);
    assert(factors.c == 1u);
    assert(factors.d == 2u);
    assert(factors.fix == 0u);

    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_SOURCE_ALPHA,
        &factors));
    assert(factors.a == 1u);
    assert(factors.b == 2u);
    assert(factors.c == 0u);
    assert(factors.d == 2u);
    assert(factors.fix == 0u);
}

static void test_rejects_invalid_requests(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(!ps2GsDescribeAlphaBlendEquation(
        (enum Ps2GsAlphaBlendEquation)99, &factors));
    assert(!ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_SOURCE_OVER, nullptr));
}

int main(void)
{
    test_source_over();
    test_source_rgb_times_inverse_source_alpha();
    test_destination_alpha_lerp();
    test_destination_rgb_multiply();
    test_rejects_invalid_requests();
    puts("gs_alpha_equation tests passed");
    return 0;
}
