#ifndef PERFECT_DARK_PS2_GS_RENDER_TARGET_LAYOUT_H
#define PERFECT_DARK_PS2_GS_RENDER_TARGET_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_GS_FRAMEBUFFER_ALIGNMENT 8192u

struct Ps2GsRenderTargetLayout {
    uint32_t width;
    uint32_t height;
    uint32_t fbw;
    uint32_t bytes;
};

/* Describe a PSMCT32 target as complete 64x32 GS pages. */
bool ps2GsDescribeCt32RenderTarget(uint32_t width, uint32_t height,
    struct Ps2GsRenderTargetLayout *layout);

#ifdef __cplusplus
}
#endif

#endif
