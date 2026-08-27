#include "gs_texture_convert.h"

static uint16_t ps2GsConvertN64Rgba5551Word(uint16_t n64)
{
    return
        (uint16_t)((n64 >> 11) & 0x001fu) |
        (uint16_t)((n64 >> 1) & 0x03e0u) |
        (uint16_t)((n64 << 9) & 0x7c00u) |
        (uint16_t)((n64 << 15) & 0x8000u);
}

static void ps2GsPermuteCsm1(void *data, uint32_t entry_size,
    uint32_t entry_count)
{
    if (entry_count != 256u) {
        return;
    }

    uint8_t *bytes = (uint8_t *)data;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if ((i & 0x18u) == 0x08u) {
            const uint32_t other = i + 8u;
            for (uint32_t byte = 0; byte < entry_size; ++byte) {
                const uint8_t value = bytes[i * entry_size + byte];
                bytes[i * entry_size + byte] =
                    bytes[other * entry_size + byte];
                bytes[other * entry_size + byte] = value;
            }
        }
    }
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

    /* CSM1 swaps the middle two 8-entry blocks in every 32-entry group. */
    ps2GsPermuteCsm1(destination, 2u, entry_count);
    return true;
}

extern "C" bool ps2GsBuildN64IntensityClut(
    enum Ps2GsN64IntensityEncoding encoding, uint32_t *destination,
    uint32_t entry_count)
{
    const bool four_bit = encoding == PS2_GS_N64_IA4 ||
        encoding == PS2_GS_N64_I4;
    const bool eight_bit = encoding == PS2_GS_N64_IA8 ||
        encoding == PS2_GS_N64_I8;
    if (!destination || (!four_bit && !eight_bit) ||
        entry_count != (four_bit ? 16u : 256u)) {
        return false;
    }

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t intensity;
        uint8_t alpha;
        if (encoding == PS2_GS_N64_IA4) {
            intensity = (uint8_t)((i >> 1u) * 0x24u);
            alpha = (i & 1u) != 0u ? 0xffu : 0u;
        } else if (encoding == PS2_GS_N64_IA8) {
            intensity = (uint8_t)((i >> 4u) * 0x11u);
            alpha = (uint8_t)((i & 0x0fu) * 0x11u);
        } else if (encoding == PS2_GS_N64_I4) {
            intensity = (uint8_t)(i * 0x11u);
            alpha = intensity;
        } else {
            intensity = (uint8_t)i;
            alpha = intensity;
        }

        destination[i] =
            (uint32_t)intensity |
            ((uint32_t)intensity << 8u) |
            ((uint32_t)intensity << 16u) |
            ((uint32_t)alpha << 24u);
    }

    ps2GsPermuteCsm1(destination, 4u, entry_count);
    return true;
}

extern "C" bool ps2GsBuildIdentityRgba8Clut(
    uint32_t *destination, uint32_t entry_count)
{
    if (!destination || entry_count != 256u) {
        return false;
    }

    for (uint32_t i = 0u; i < entry_count; ++i) {
        destination[i] = i * 0x01010101u;
    }
    ps2GsPermuteCsm1(destination, 4u, entry_count);
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
