#include "gs_alpha_equation.h"
#include "gs_frame_mask.h"

extern "C" bool ps2GsDescribeAlphaBlendEquation(
    enum Ps2GsAlphaBlendEquation equation,
    struct Ps2GsAlphaBlendFactors *factors)
{
    if (!factors) {
        return false;
    }

    switch (equation) {
        case PS2_GS_ALPHA_BLEND_SOURCE_OVER:
            /* (Cs - Cd) * As + Cd */
            *factors = { 0u, 1u, 0u, 1u, 0u };
            return true;
        case PS2_GS_ALPHA_BLEND_SOURCE_RGB_TIMES_INV_SOURCE_ALPHA:
            /* (0 - Cs) * As + Cs == Cs * (1 - As) */
            *factors = { 2u, 0u, 0u, 0u, 0u };
            return true;
        case PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP:
            /* (Cs - Cd) * Ad + Cd */
            *factors = { 0u, 1u, 1u, 1u, 0u };
            return true;
        case PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_DESTINATION_ALPHA:
            /* (Cd - 0) * Ad + 0 */
            *factors = { 1u, 2u, 1u, 2u, 0u };
            return true;
        case PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_SOURCE_ALPHA:
            /* (Cd - 0) * As + 0 */
            *factors = { 1u, 2u, 0u, 2u, 0u };
            return true;
        case PS2_GS_ALPHA_BLEND_SOURCE_PLUS_DESTINATION:
            /* (Cs - 0) * FIX(0x80) + Cd == Cs + Cd. */
            *factors = { 0u, 2u, 2u, 1u, 0x80u };
            return true;
        default:
            return false;
    }
}

extern "C" struct Ps2GsAlphaBlendFactors ps2GsDestinationScaleBlend(
    uint8_t fix)
{
    const struct Ps2GsAlphaBlendFactors factors = { 1u, 2u, 2u, 2u, fix };
    return factors;
}

static uint8_t ps2GsModulateFix(uint8_t component)
{
    return (uint8_t)(((uint32_t)component * 128u + 127u) / 255u);
}

extern "C" uint32_t ps2GsPlanConstantModulate(uint8_t r, uint8_t g, uint8_t b,
    struct Ps2GsModulatePass passes[3])
{
    if (!passes) {
        return 0u;
    }
    const uint8_t values[3] = { r, g, b };
    const uint8_t masks[3] = {
        PS2_GS_COLOR_WRITE_RED,
        PS2_GS_COLOR_WRITE_GREEN,
        PS2_GS_COLOR_WRITE_BLUE,
    };
    uint32_t count = 0u;
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
        const uint8_t fix = ps2GsModulateFix(values[channel]);
        uint32_t pass = 0u;
        while (pass < count && passes[pass].fix != fix) {
            ++pass;
        }
        if (pass == count) {
            passes[count].channels = 0u;
            passes[count].fix = fix;
            ++count;
        }
        passes[pass].channels |= masks[channel];
    }
    return count;
}
