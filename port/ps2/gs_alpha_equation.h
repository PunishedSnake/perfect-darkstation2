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
    PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_DESTINATION_ALPHA,
    PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_SOURCE_ALPHA,
    PS2_GS_ALPHA_BLEND_SOURCE_PLUS_DESTINATION,
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

/* (Cd - 0) * FIX + 0: scale the destination colour by FIX/128. */
struct Ps2GsAlphaBlendFactors ps2GsDestinationScaleBlend(uint8_t fix);

/*
 * Fast3D G_MODULATE_EXT with a constant source colour (GL DST_COLOR, ZERO):
 * dst.rgb *= src.rgb / 255. The GS cannot multiply by Cs, but a constant
 * per-channel factor is exact as FIX with a per-channel FRAME mask. Channels
 * sharing a factor share one pass, so 1..3 passes are produced.
 *
 * fix = round(c * 128 / 255): 0xff -> 0x80 (identity) and 0 -> 0 are exact;
 * other factors differ from c/255 by at most 1/256, so the 8-bit result
 * stays within one step of GL after the GS truncating shift.
 */
struct Ps2GsModulatePass {
    uint8_t channels; /* Ps2GsColorWriteChannel bits */
    uint8_t fix;
};

uint32_t ps2GsPlanConstantModulate(uint8_t r, uint8_t g, uint8_t b,
    struct Ps2GsModulatePass passes[3]);

#ifdef __cplusplus
}
#endif

#endif
