#include "gs_texture_convert.h"

#include <stddef.h>

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

static uint8_t ps2GsReadN64FourBitTexel(
    const uint8_t *source, uint32_t index)
{
    const uint8_t packed = source[index >> 1u];
    return (index & 1u) == 0u ? packed >> 4u : packed & 0x0fu;
}

static void ps2GsWriteN64FourBitTexel(
    uint8_t *destination, uint32_t index, uint8_t value)
{
    uint8_t *packed = &destination[index >> 1u];
    if ((index & 1u) == 0u) {
        *packed = (uint8_t)((*packed & 0x0fu) | (value << 4u));
    } else {
        *packed = (uint8_t)((*packed & 0xf0u) | value);
    }
}

extern "C" bool ps2GsExpandTextureMirror(
    const uint8_t *source, uint8_t *destination,
    uint32_t width, uint32_t height, uint8_t bits_per_texel,
    bool mirror_s, bool mirror_t)
{
    if (!source || !destination || width == 0u || height == 0u ||
        (bits_per_texel != 4u && bits_per_texel != 8u &&
         bits_per_texel != 16u && bits_per_texel != 32u) ||
        (bits_per_texel == 4u && (width & 1u) != 0u)) {
        return false;
    }

    const uint32_t output_width = width << (mirror_s ? 1u : 0u);
    const uint32_t output_height = height << (mirror_t ? 1u : 0u);
    if (bits_per_texel == 4u) {
        const uint32_t output_bytes =
            (output_width * output_height + 1u) / 2u;
        for (uint32_t i = 0u; i < output_bytes; ++i) {
            destination[i] = 0u;
        }
        for (uint32_t y = 0u; y < output_height; ++y) {
            const uint32_t source_y = y < height ? y :
                output_height - 1u - y;
            for (uint32_t x = 0u; x < output_width; ++x) {
                const uint32_t source_x = x < width ? x :
                    output_width - 1u - x;
                const uint8_t texel = ps2GsReadN64FourBitTexel(
                    source, source_y * width + source_x);
                ps2GsWriteN64FourBitTexel(
                    destination, y * output_width + x, texel);
            }
        }
        return true;
    }

    const uint32_t bytes_per_texel = bits_per_texel / 8u;
    for (uint32_t y = 0u; y < output_height; ++y) {
        const uint32_t source_y = y < height ? y :
            output_height - 1u - y;
        for (uint32_t x = 0u; x < output_width; ++x) {
            const uint32_t source_x = x < width ? x :
                output_width - 1u - x;
            const uint8_t *input = source +
                ((size_t)source_y * width + source_x) * bytes_per_texel;
            uint8_t *output = destination +
                ((size_t)y * output_width + x) * bytes_per_texel;
            for (uint32_t byte = 0u; byte < bytes_per_texel; ++byte) {
                output[byte] = input[byte];
            }
        }
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
