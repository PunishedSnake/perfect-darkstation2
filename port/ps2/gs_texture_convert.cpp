#include "gs_texture_convert.h"

static uint16_t ps2GsConvertN64Rgba5551Word(uint16_t n64)
{
    return
        (uint16_t)((n64 >> 11) & 0x001fu) |
        (uint16_t)((n64 >> 1) & 0x03e0u) |
        (uint16_t)((n64 << 9) & 0x7c00u) |
        (uint16_t)((n64 << 15) & 0x8000u);
}

extern "C" bool ps2GsConvertN64Rgba16ToGsCt16(const uint8_t *source,
    uint8_t *destination, uint32_t texel_count)
{
    if ((!source || !destination) && texel_count != 0u) {
        return false;
    }

    for (uint32_t i = 0; i < texel_count; ++i) {
        const uint16_t n64 =
            ((uint16_t)source[i * 2u] << 8) | source[i * 2u + 1u];
        const uint16_t gs = ps2GsConvertN64Rgba5551Word(n64);

        destination[i * 2u] = (uint8_t)gs;
        destination[i * 2u + 1u] = (uint8_t)(gs >> 8);
    }
    return true;
}

extern "C" bool ps2GsConvertN64Ci4ToGsT4(const uint8_t *source,
    uint8_t *destination, uint32_t byte_count)
{
    if ((!source || !destination) && byte_count != 0u) {
        return false;
    }

    for (uint32_t i = 0; i < byte_count; ++i) {
        const uint8_t packed = source[i];
        destination[i] = (uint8_t)((packed << 4) | (packed >> 4));
    }
    return true;
}

extern "C" bool ps2GsConvertN64Rgba16PaletteToGsCt16(
    const uint16_t *source, uint8_t *destination, uint32_t entry_count)
{
    if (!source || !destination ||
        (entry_count != 16u && entry_count != 256u)) {
        return false;
    }

    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint16_t gs = ps2GsConvertN64Rgba5551Word(source[i]);
        destination[i * 2u] = (uint8_t)gs;
        destination[i * 2u + 1u] = (uint8_t)(gs >> 8);
    }

    if (entry_count == 256u) {
        /* CSM1 swaps the middle two 8-entry blocks in every 32-entry group. */
        for (uint32_t i = 0; i < entry_count; ++i) {
            if ((i & 0x18u) == 0x08u) {
                const uint32_t other = i + 8u;
                const uint8_t lo = destination[i * 2u];
                const uint8_t hi = destination[i * 2u + 1u];
                destination[i * 2u] = destination[other * 2u];
                destination[i * 2u + 1u] = destination[other * 2u + 1u];
                destination[other * 2u] = lo;
                destination[other * 2u + 1u] = hi;
            }
        }
    }
    return true;
}

extern "C" uint32_t ps2GsTextureBufferWidth(uint32_t width, bool indexed)
{
    if (width == 0u) {
        return 0u;
    }

    uint32_t units = 1u + (width - 1u) / 64u;
    if (indexed) {
        units = (units + 1u) & ~1u;
    }
    return units;
}
