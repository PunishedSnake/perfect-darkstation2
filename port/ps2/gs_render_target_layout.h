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

enum Ps2GsCt32Channel {
    PS2_GS_CT32_CHANNEL_RED,
    PS2_GS_CT32_CHANNEL_GREEN,
    PS2_GS_CT32_CHANNEL_BLUE,
    PS2_GS_CT32_CHANNEL_ALPHA,
};

struct Ps2GsT8PageCoordinate {
    uint32_t u;
    uint32_t v;
};

/* Describe a PSMCT32 target as complete 64x32 GS pages. */
bool ps2GsDescribeCt32RenderTarget(uint32_t width, uint32_t height,
    struct Ps2GsRenderTargetLayout *layout);

/* Describe the exact TEX0/CLAMP geometry for sampling a CT32 target. */
bool ps2GsDescribeCt32RenderTargetTextureView(
    const struct Ps2GsRenderTargetLayout *layout,
    struct Ps2GsRenderTargetTextureView *view);

/* Map one logical pixel lane in a 64x32 CT32 page to its 128x64 T8 view. */
bool ps2GsMapCt32PixelChannelToT8Page(uint32_t x, uint32_t y,
    enum Ps2GsCt32Channel channel,
    struct Ps2GsT8PageCoordinate *coordinate);

#ifdef __cplusplus
}
#endif

#endif
