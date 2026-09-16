#include "gs_frame_mask.h"

extern "C" uint32_t ps2GsFrameWriteMask(
    uint8_t color_channels, bool alpha_write, bool target_ct16)
{
    color_channels &= PS2_GS_COLOR_WRITE_RGB;

    if (target_ct16) {
        /*
         * GS conversion maps FRAME bit 31 to CT16 alpha. Pass graphs never
         * request a partial RGB mask on CT16, so keep that contract explicit.
         */
        if (color_channels == PS2_GS_COLOR_WRITE_RGB) {
            return alpha_write ? 0u : 0x80000000u;
        }
        return alpha_write ? 0x7fffffffu : 0xffffffffu;
    }

    uint32_t mask = 0u;
    if ((color_channels & PS2_GS_COLOR_WRITE_RED) == 0u) {
        mask |= 0x000000ffu;
    }
    if ((color_channels & PS2_GS_COLOR_WRITE_GREEN) == 0u) {
        mask |= 0x0000ff00u;
    }
    if ((color_channels & PS2_GS_COLOR_WRITE_BLUE) == 0u) {
        mask |= 0x00ff0000u;
    }
    if (!alpha_write) {
        mask |= 0xff000000u;
    }
    return mask;
}

extern "C" uint32_t ps2GsClearFrameWriteMask(bool clear_color)
{
    return clear_color ? 0u : UINT32_MAX;
}

extern "C" uint8_t ps2GsClearDepthWriteMask(
    bool clear_depth, bool has_depth_buffer)
{
    return clear_depth && has_depth_buffer ? 0u : 1u;
}

extern "C" bool ps2GsDepthTestEnabled(
    bool depth_test, bool has_depth_buffer)
{
    return depth_test && has_depth_buffer;
}

extern "C" bool ps2GsDepthWriteEnabled(
    bool depth_test, bool depth_update, bool has_depth_buffer)
{
    return depth_test && depth_update && has_depth_buffer;
}
