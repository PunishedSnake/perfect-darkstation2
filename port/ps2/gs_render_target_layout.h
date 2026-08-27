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

struct Ps2GsRenderTargetTextureView {
    uint32_t tbw;
    uint32_t clamp_max_u;
    uint32_t clamp_max_v;
    uint8_t tw;
    uint8_t th;
};

/* Describe a PSMCT32 target as complete 64x32 GS pages. */
bool ps2GsDescribeCt32RenderTarget(uint32_t width, uint32_t height,
    struct Ps2GsRenderTargetLayout *layout);

/* Describe the exact TEX0/CLAMP geometry for sampling a CT32 target. */
bool ps2GsDescribeCt32RenderTargetTextureView(
    const struct Ps2GsRenderTargetLayout *layout,
    struct Ps2GsRenderTargetTextureView *view);

#ifdef __cplusplus
}
#endif

#endif
