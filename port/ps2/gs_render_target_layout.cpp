#include "gs_render_target_layout.h"

#include "gs_vram_allocator.h"

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
