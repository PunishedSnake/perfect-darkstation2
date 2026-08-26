#include "gs_texture_convert.h"

extern "C" bool ps2GsConvertN64Rgba16ToGsCt16(const uint8_t *source,
    uint8_t *destination, uint32_t texel_count)
{
    if ((!source || !destination) && texel_count != 0u) {
        return false;
    }

    for (uint32_t i = 0; i < texel_count; ++i) {
        const uint16_t n64 =
            ((uint16_t)source[i * 2u] << 8) | source[i * 2u + 1u];
        const uint16_t gs =
            (uint16_t)((n64 >> 11) & 0x001fu) |
            (uint16_t)((n64 >> 1) & 0x03e0u) |
            (uint16_t)((n64 << 9) & 0x7c00u) |
            (uint16_t)((n64 << 15) & 0x8000u);

        destination[i * 2u] = (uint8_t)gs;
        destination[i * 2u + 1u] = (uint8_t)(gs >> 8);
    }
    return true;
}
