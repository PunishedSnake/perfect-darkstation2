#include "gs_texture_convert.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_primary_colors_and_alpha(void)
{
    const uint8_t source[] = {
        0xf8, 0x01, /* opaque red */
        0x07, 0xc1, /* opaque green */
        0x00, 0x3f, /* opaque blue */
        0xff, 0xff, /* opaque white */
        0x00, 0x01, /* opaque black */
        0xf8, 0x00, /* transparent red */
    };
    const uint8_t expected[] = {
        0x1f, 0x80,
        0xe0, 0x83,
        0x00, 0xfc,
        0xff, 0xff,
        0x00, 0x80,
        0x1f, 0x00,
    };
    uint8_t destination[sizeof(source)] = {};

    assert(ps2GsConvertN64Rgba16ToGsCt16(source, destination,
        (uint32_t)(sizeof(source) / 2u)));
    assert(memcmp(destination, expected, sizeof(expected)) == 0);
}

static void test_exact_alias_and_empty_input(void)
{
    uint8_t pixels[] = { 0x12, 0x35, 0xab, 0xcc };
    uint8_t expected[sizeof(pixels)] = {};

    assert(ps2GsConvertN64Rgba16ToGsCt16(
        pixels, expected, (uint32_t)(sizeof(pixels) / 2u)));
    assert(ps2GsConvertN64Rgba16ToGsCt16(
        pixels, pixels, (uint32_t)(sizeof(pixels) / 2u)));
    assert(memcmp(pixels, expected, sizeof(pixels)) == 0);

    assert(ps2GsConvertN64Rgba16ToGsCt16(NULL, NULL, 0u));
    assert(!ps2GsConvertN64Rgba16ToGsCt16(NULL, pixels, 1u));
    assert(!ps2GsConvertN64Rgba16ToGsCt16(pixels, NULL, 1u));
}

static void test_all_rgba5551_values_preserve_channels(void)
{
    for (uint32_t value = 0u; value <= UINT16_MAX; ++value) {
        const uint8_t source[2] = {
            (uint8_t)(value >> 8), (uint8_t)value
        };
        uint8_t destination[2] = {};
        assert(ps2GsConvertN64Rgba16ToGsCt16(
            source, destination, 1u));

        const uint16_t gs =
            (uint16_t)destination[0] |
            ((uint16_t)destination[1] << 8);
        assert((gs & 0x001fu) == ((value >> 11) & 0x001fu));
        assert(((gs >> 5) & 0x001fu) == ((value >> 6) & 0x001fu));
        assert(((gs >> 10) & 0x001fu) == ((value >> 1) & 0x001fu));
        assert(((gs >> 15) & 1u) == (value & 1u));
    }
}

static uint16_t read_ct16(const uint8_t *bytes, uint32_t index)
{
    return (uint16_t)bytes[index * 2u] |
        ((uint16_t)bytes[index * 2u + 1u] << 8);
}

static uint16_t convert_palette_word(uint16_t value)
{
    const uint8_t source[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    uint8_t destination[2] = {};
    assert(ps2GsConvertN64Rgba16ToGsCt16(source, destination, 1u));
    return read_ct16(destination, 0u);
}

static void test_ci4_nibble_order(void)
{
    const uint8_t source[] = { 0x01, 0xab, 0xf4, 0x80 };
    const uint8_t expected[] = { 0x10, 0xba, 0x4f, 0x08 };
    uint8_t destination[sizeof(source)] = {};

    assert(ps2GsConvertN64Ci4ToGsT4(
        source, destination, (uint32_t)sizeof(source)));
    assert(memcmp(destination, expected, sizeof(expected)) == 0);

    memcpy(destination, source, sizeof(source));
    assert(ps2GsConvertN64Ci4ToGsT4(
        destination, destination, (uint32_t)sizeof(destination)));
    assert(memcmp(destination, expected, sizeof(expected)) == 0);
    assert(ps2GsConvertN64Ci4ToGsT4(NULL, NULL, 0u));
    assert(!ps2GsConvertN64Ci4ToGsT4(NULL, destination, 1u));
}

static void test_rgba16_palette_conversion_and_csm1_order(void)
{
    uint16_t palette16[16];
    uint8_t converted16[sizeof(palette16)] = {};
    for (uint32_t i = 0; i < 16u; ++i) {
        palette16[i] = (uint16_t)(i * 0x0843u + 1u);
    }
    assert(ps2GsConvertN64Rgba16PaletteToGsCt16(
        palette16, converted16, 16u));
    for (uint32_t i = 0; i < 16u; ++i) {
        assert(read_ct16(converted16, i) ==
            convert_palette_word(palette16[i]));
    }

    uint16_t palette256[256];
    uint8_t converted256[sizeof(palette256)] = {};
    for (uint32_t i = 0; i < 256u; ++i) {
        palette256[i] = (uint16_t)((i << 8) | (i ^ 0x5au));
    }
    assert(ps2GsConvertN64Rgba16PaletteToGsCt16(
        palette256, converted256, 256u));
    for (uint32_t destination = 0; destination < 256u; ++destination) {
        const uint32_t source = ((destination & 0x18u) == 0x08u ||
                                 (destination & 0x18u) == 0x10u)
            ? destination ^ 0x18u : destination;
        assert(read_ct16(converted256, destination) ==
            convert_palette_word(palette256[source]));
    }

    assert(!ps2GsConvertN64Rgba16PaletteToGsCt16(
        palette16, converted16, 0u));
    assert(!ps2GsConvertN64Rgba16PaletteToGsCt16(
        palette16, converted16, 32u));
    assert(!ps2GsConvertN64Rgba16PaletteToGsCt16(
        NULL, converted16, 16u));
}

static void test_gs_texture_buffer_width_alignment(void)
{
    assert(ps2GsTextureBufferWidth(0u, false) == 0u);
    assert(ps2GsTextureBufferWidth(1u, false) == 1u);
    assert(ps2GsTextureBufferWidth(64u, false) == 1u);
    assert(ps2GsTextureBufferWidth(65u, false) == 2u);
    assert(ps2GsTextureBufferWidth(1024u, false) == 16u);

    assert(ps2GsTextureBufferWidth(1u, true) == 2u);
    assert(ps2GsTextureBufferWidth(64u, true) == 2u);
    assert(ps2GsTextureBufferWidth(128u, true) == 2u);
    assert(ps2GsTextureBufferWidth(129u, true) == 4u);
    assert(ps2GsTextureBufferWidth(1024u, true) == 16u);
}

int main(void)
{
    test_primary_colors_and_alpha();
    test_exact_alias_and_empty_input();
    test_all_rgba5551_values_preserve_channels();
    test_ci4_nibble_order();
    test_rgba16_palette_conversion_and_csm1_order();
    test_gs_texture_buffer_width_alignment();
    puts("gs_texture_convert tests passed");
    return 0;
}
