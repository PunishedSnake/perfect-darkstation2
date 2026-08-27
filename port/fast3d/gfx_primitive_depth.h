#ifndef PERFECT_DARK_PC_GFX_PRIMITIVE_DEPTH_H
#define PERFECT_DARK_PC_GFX_PRIMITIVE_DEPTH_H

#include <stdint.h>

/*
 * The RDP primitive-Z register exposes 15 integer bits from its 15.3 depth
 * domain. Bit 15 of gDPSetPrimDepth's argument is ignored and the three
 * fractional bits are zero, so the normalized value is z15 / 2^15.
 */
static inline float gfx_primitive_depth_to_ndc(uint16_t raw_z)
{
    return (float)(raw_z & 0x7fffu) * (1.0f / 32768.0f);
}

/* Convert the RDP [0,1) depth into the portable Fast3D [-W,+W] clip domain. */
static inline float gfx_primitive_depth_to_clip_z(uint16_t raw_z, float clip_w)
{
    return (gfx_primitive_depth_to_ndc(raw_z) * 2.0f - 1.0f) * clip_w;
}

#endif
