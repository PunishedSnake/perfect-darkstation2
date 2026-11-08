#include "gs_alpha_equation.h"

#include "gs_frame_mask.h"

#include <assert.h>
#include <stdio.h>

static void test_constant_modulate_plan(void)
{
    struct Ps2GsModulatePass passes[3] = {};
    /* Night vision: (0, g, 0) -> R+B zeroed together, G scaled. */
    assert(ps2GsPlanConstantModulate(0x00u, 0xf3u, 0x00u, passes) == 2u);
    assert(passes[0].channels ==
        (PS2_GS_COLOR_WRITE_RED | PS2_GS_COLOR_WRITE_BLUE));
    assert(passes[0].fix == 0u);
    assert(passes[1].channels == PS2_GS_COLOR_WRITE_GREEN);
    assert(passes[1].fix == 122u);
    /* Uniform factor: a single all-channel pass. */
    assert(ps2GsPlanConstantModulate(0xffu, 0xffu, 0xffu, passes) == 1u);
    assert(passes[0].channels == PS2_GS_COLOR_WRITE_RGB);
    assert(passes[0].fix == 0x80u);
    /* Three distinct factors. */
    assert(ps2GsPlanConstantModulate(0x7fu, 0xffu, 0xbfu, passes) == 3u);
    assert(passes[0].fix == 64u && passes[1].fix == 128u &&
        passes[2].fix == 96u);
    /* GS (Cd * FIX) >> 7 stays within one 8-bit step of Cd * c / 255. */
    for (uint32_t c = 0u; c < 256u; ++c) {
        assert(ps2GsPlanConstantModulate((uint8_t)c, (uint8_t)c, (uint8_t)c,
            passes) == 1u);
        for (uint32_t cd = 0u; cd < 256u; cd += 5u) {
            const double exact = (double)cd * (double)c / 255.0;
            const double gs = (double)((cd * passes[0].fix) >> 7);
            assert(exact - gs > -1.0 && exact - gs < 2.0);
        }
    }
    const struct Ps2GsAlphaBlendFactors scale =
        ps2GsDestinationScaleBlend(96u);
    assert(scale.a == 1u && scale.b == 2u && scale.c == 2u &&
        scale.d == 2u && scale.fix == 96u);
}

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

static void test_source_plus_destination(void)
{
    struct Ps2GsAlphaBlendFactors factors{};
    assert(ps2GsDescribeAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_SOURCE_PLUS_DESTINATION, &factors));
    assert(factors.a == 0u);
    assert(factors.b == 2u);
    assert(factors.c == 2u);
    assert(factors.d == 1u);
    assert(factors.fix == 0x80u);
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
    test_constant_modulate_plan();
    test_source_over();
    test_source_rgb_times_inverse_source_alpha();
    test_destination_alpha_lerp();
    test_destination_rgb_multiply();
    test_source_plus_destination();
    test_rejects_invalid_requests();
    puts("gs_alpha_equation tests passed");
    return 0;
}
