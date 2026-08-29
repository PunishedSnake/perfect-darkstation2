#ifndef PERFECT_DARK_PS2_GS_ALPHA_EQUATION_H
#define PERFECT_DARK_PS2_GS_ALPHA_EQUATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum Ps2GsAlphaBlendEquation {
    PS2_GS_ALPHA_BLEND_SOURCE_OVER = 0,
    PS2_GS_ALPHA_BLEND_SOURCE_RGB_TIMES_INV_SOURCE_ALPHA,
    PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP,
};

struct Ps2GsAlphaBlendFactors {
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t fix;
};

/*
 * Describe the GS ALPHA equation Cv = ((A - B) * C) / 128 + D without
 * exposing gsKit register macros to the host-tested planner layer.
 */
bool ps2GsDescribeAlphaBlendEquation(enum Ps2GsAlphaBlendEquation equation,
    struct Ps2GsAlphaBlendFactors *factors);

#ifdef __cplusplus
}
#endif

#endif
