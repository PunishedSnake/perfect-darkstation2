#ifndef PERFECT_DARK_PS2_GS_FRAME_MASK_H
#define PERFECT_DARK_PS2_GS_FRAME_MASK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum Ps2GsColorWriteChannel {
    PS2_GS_COLOR_WRITE_RED = 1u << 0,
    PS2_GS_COLOR_WRITE_GREEN = 1u << 1,
    PS2_GS_COLOR_WRITE_BLUE = 1u << 2,
    PS2_GS_COLOR_WRITE_RGB = PS2_GS_COLOR_WRITE_RED |
        PS2_GS_COLOR_WRITE_GREEN | PS2_GS_COLOR_WRITE_BLUE,
};

/*
 * Build FRAME.FBMSK for the core's logical write enables. A set FBMSK bit
 * preserves the destination bit. Individual RGB lanes are exposed only for
 * CT32 pass-graph targets; CT16 keeps the hardware-validated aggregate masks.
 */
uint32_t ps2GsFrameWriteMask(uint8_t color_channels, bool alpha_write,
    bool target_ct16);

/* Independent clear masks: a clear must not inherit the preceding draw. */
uint32_t ps2GsClearFrameWriteMask(bool clear_color);
uint8_t ps2GsClearDepthWriteMask(bool clear_depth, bool has_depth_buffer);

/* Match the rendering API contract: disabled depth testing cannot write Z. */
bool ps2GsDepthTestEnabled(bool depth_test, bool has_depth_buffer);
bool ps2GsDepthWriteEnabled(
    bool depth_test, bool depth_update, bool has_depth_buffer);

#ifdef __cplusplus
}
#endif

#endif
