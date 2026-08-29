#include "gs_alpha_equation.h"

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
        default:
            return false;
    }
}
