#include "gs_render_target_layout.h"

#include "gs_vram_allocator.h"

static uint8_t ps2GsTextureExponent(uint32_t size)
{
    uint8_t exponent = 0u;
    uint32_t value = 1u;

    while (value < size && exponent < 10u) {
        value <<= 1u;
        ++exponent;
    }
    return exponent;
}

extern "C" bool ps2GsDescribeCt32RenderTarget(
    uint32_t width, uint32_t height,
    struct Ps2GsRenderTargetLayout *layout)
{
    if (!layout || width == 0u || height == 0u ||
        width > 1024u || height > 1024u) {
        return false;
    }

    const uint32_t fbw = (width + 63u) / 64u;
    const uint32_t page_rows = (height + 31u) / 32u;
    const uint64_t bytes =
        (uint64_t)fbw * page_rows * PS2_GS_FRAMEBUFFER_ALIGNMENT;
    if (bytes == 0u || bytes > PS2_GS_VRAM_BYTES) {
        return false;
    }

    layout->width = width;
    layout->height = height;
    layout->fbw = fbw;
    layout->bytes = (uint32_t)bytes;
    return true;
}

extern "C" bool ps2GsDescribeCt32RenderTargetTextureView(
    const struct Ps2GsRenderTargetLayout *layout,
    struct Ps2GsRenderTargetTextureView *view)
{
    if (!layout || !view) {
        return false;
    }

    struct Ps2GsRenderTargetLayout expected;
    if (!ps2GsDescribeCt32RenderTarget(
            layout->width, layout->height, &expected) ||
        expected.fbw != layout->fbw || expected.bytes != layout->bytes) {
        return false;
    }

    view->tbw = layout->fbw;
    view->clamp_max_u = layout->width - 1u;
    view->clamp_max_v = layout->height - 1u;
    view->tw = ps2GsTextureExponent(layout->width);
    view->th = ps2GsTextureExponent(layout->height);
    return true;
}

extern "C" bool ps2GsMapCt32PixelChannelToT8Page(
    uint32_t x, uint32_t y, enum Ps2GsCt32Channel channel,
    struct Ps2GsT8PageCoordinate *coordinate)
{
    if (!coordinate || x >= 64u || y >= 32u ||
        channel > PS2_GS_CT32_CHANNEL_ALPHA) {
        return false;
    }

    const bool right_lane = channel == PS2_GS_CT32_CHANNEL_BLUE ||
        channel == PS2_GS_CT32_CHANNEL_ALPHA;
    const bool lower_lane = channel == PS2_GS_CT32_CHANNEL_GREEN ||
        channel == PS2_GS_CT32_CHANNEL_ALPHA;
    const uint32_t v = (y & ~1u) * 2u + (y & 1u) +
        (lower_lane ? 2u : 0u);
    const uint32_t column_xor =
        (v & 4u) ^ ((v & 2u) << 1u);

    coordinate->u = (x & ~7u) * 2u +
        (right_lane ? 8u : 0u) + ((x & 7u) ^ column_xor);
    coordinate->v = v;
    return true;
}
