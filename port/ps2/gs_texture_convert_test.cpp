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

static void test_all_ia16_values_preserve_channels(void)
{
    for (uint32_t intensity = 0u; intensity <= UINT8_MAX; ++intensity) {
        for (uint32_t alpha = 0u; alpha <= UINT8_MAX; ++alpha) {
            const uint8_t source[] = {
                (uint8_t)intensity, (uint8_t)alpha
            };
            uint8_t destination[4] = {};
            assert(ps2GsConvertN64Ia16ToGsCt32(
                source, destination, 1u, 1u, false, false));
            assert(destination[0] == intensity);
            assert(destination[1] == intensity);
            assert(destination[2] == intensity);
            assert(destination[3] == alpha);
        }
    }

    assert(!ps2GsConvertN64Ia16ToGsCt32(
        NULL, NULL, 1u, 1u, false, false));
    uint8_t byte = 0u;
    assert(!ps2GsConvertN64Ia16ToGsCt32(
        &byte, &byte, 0u, 1u, false, false));
}

static void test_ia16_mirror(void)
{
    const uint8_t source[] = {
        0x10u, 0x20u, 0x30u, 0x40u,
        0x50u, 0x60u, 0x70u, 0x80u,
    };
    uint8_t output[64] = {};
    const uint8_t expected[] = {
        0x10u, 0x10u, 0x10u, 0x20u,
        0x30u, 0x30u, 0x30u, 0x40u,
        0x30u, 0x30u, 0x30u, 0x40u,
        0x10u, 0x10u, 0x10u, 0x20u,
        0x50u, 0x50u, 0x50u, 0x60u,
        0x70u, 0x70u, 0x70u, 0x80u,
        0x70u, 0x70u, 0x70u, 0x80u,
        0x50u, 0x50u, 0x50u, 0x60u,
        0x50u, 0x50u, 0x50u, 0x60u,
        0x70u, 0x70u, 0x70u, 0x80u,
        0x70u, 0x70u, 0x70u, 0x80u,
        0x50u, 0x50u, 0x50u, 0x60u,
        0x10u, 0x10u, 0x10u, 0x20u,
        0x30u, 0x30u, 0x30u, 0x40u,
        0x30u, 0x30u, 0x30u, 0x40u,
        0x10u, 0x10u, 0x10u, 0x20u,
    };
    assert(ps2GsConvertN64Ia16ToGsCt32(
        source, output, 2u, 2u, true, true));
    assert(memcmp(output, expected, sizeof(output)) == 0);
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

static void test_mirror_u8(void)
{
    const uint8_t source[] = { 1u, 2u, 3u, 4u, 5u, 6u };
    uint8_t horizontal[12] = {};
    uint8_t vertical[12] = {};
    uint8_t both[24] = {};
    const uint8_t expected_horizontal[] = {
        1u, 2u, 3u, 3u, 2u, 1u,
        4u, 5u, 6u, 6u, 5u, 4u,
    };
    const uint8_t expected_vertical[] = {
        1u, 2u, 3u, 4u, 5u, 6u,
        4u, 5u, 6u, 1u, 2u, 3u,
    };
    const uint8_t expected_both[] = {
        1u, 2u, 3u, 3u, 2u, 1u,
        4u, 5u, 6u, 6u, 5u, 4u,
        4u, 5u, 6u, 6u, 5u, 4u,
        1u, 2u, 3u, 3u, 2u, 1u,
    };

    assert(ps2GsExpandTextureMirror(
        source, horizontal, 3u, 2u, 8u, true, false));
    assert(ps2GsExpandTextureMirror(
        source, vertical, 3u, 2u, 8u, false, true));
    assert(ps2GsExpandTextureMirror(
        source, both, 3u, 2u, 8u, true, true));
    assert(memcmp(horizontal, expected_horizontal, sizeof(horizontal)) == 0);
    assert(memcmp(vertical, expected_vertical, sizeof(vertical)) == 0);
    assert(memcmp(both, expected_both, sizeof(both)) == 0);
}

static void test_mirror_n64_four_bit(void)
{
    const uint8_t source[] = { 0x12u, 0x34u, 0x56u, 0x78u };
    uint8_t output[16] = {};
    const uint8_t expected[] = {
        0x12u, 0x34u, 0x43u, 0x21u,
        0x56u, 0x78u, 0x87u, 0x65u,
        0x56u, 0x78u, 0x87u, 0x65u,
        0x12u, 0x34u, 0x43u, 0x21u,
    };
    assert(ps2GsExpandTextureMirror(
        source, output, 4u, 2u, 4u, true, true));
    assert(memcmp(output, expected, sizeof(output)) == 0);
}

static void test_mirror_wide_texels(void)
{
    const uint16_t source16[] = { 0x1122u, 0x3344u };
    uint16_t output16[4] = {};
    assert(ps2GsExpandTextureMirror(
        (const uint8_t *)source16, (uint8_t *)output16,
        2u, 1u, 16u, true, false));
    assert(output16[0] == 0x1122u && output16[1] == 0x3344u &&
           output16[2] == 0x3344u && output16[3] == 0x1122u);

    const uint32_t source32[] = { 0x11223344u, 0xaabbccddu };
    uint32_t output32[4] = {};
    assert(ps2GsExpandTextureMirror(
        (const uint8_t *)source32, (uint8_t *)output32,
        1u, 2u, 32u, false, true));
    assert(output32[0] == 0x11223344u && output32[1] == 0xaabbccddu &&
           output32[2] == 0xaabbccddu && output32[3] == 0x11223344u);
}

static void test_mirror_fused_native_conversion(void)
{
    const uint8_t rgba16_source[] = {
        0xf8u, 0x01u, 0x07u, 0xc1u,
    };
    uint8_t rgba16_output[8] = {};
    const uint8_t rgba16_expected[] = {
        0x1fu, 0x80u, 0xe0u, 0x83u,
        0xe0u, 0x83u, 0x1fu, 0x80u,
    };
    assert(ps2GsExpandTextureMirror(
        rgba16_source, rgba16_output, 2u, 1u, 16u, true, false));
    assert(ps2GsConvertN64Rgba16ToGsCt16(
        rgba16_output, rgba16_output, 4u));
    assert(memcmp(
        rgba16_output, rgba16_expected, sizeof(rgba16_output)) == 0);

    const uint8_t t4_source[] = { 0x12u, 0x34u };
    uint8_t t4_output[4] = {};
    const uint8_t t4_expected[] = { 0x21u, 0x43u, 0x34u, 0x12u };
    assert(ps2GsExpandTextureMirror(
        t4_source, t4_output, 4u, 1u, 4u, true, false));
    assert(ps2GsConvertN64Ci4ToGsT4(
        t4_output, t4_output, sizeof(t4_output)));
    assert(memcmp(t4_output, t4_expected, sizeof(t4_output)) == 0);
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

static void test_ia16_palette_conversion_and_csm1_order(void)
{
    uint16_t palette16[16];
    uint32_t converted16[16] = {};
    for (uint32_t i = 0u; i < 16u; ++i) {
        palette16[i] = (uint16_t)(((i * 13u) << 8u) | (i * 17u));
    }
    assert(ps2GsConvertN64Ia16PaletteToGsCt32(
        palette16, converted16, 16u));
    for (uint32_t i = 0u; i < 16u; ++i) {
        const uint32_t intensity = palette16[i] & 0xffu;
        const uint32_t alpha = palette16[i] >> 8u;
        assert(converted16[i] ==
            intensity * 0x00010101u + (alpha << 24u));
    }

    uint16_t palette256[256];
    uint32_t converted256[256] = {};
    for (uint32_t i = 0u; i < 256u; ++i) {
        palette256[i] = (uint16_t)(((i ^ 0xa5u) << 8u) | i);
    }
    assert(ps2GsConvertN64Ia16PaletteToGsCt32(
        palette256, converted256, 256u));
    for (uint32_t destination = 0u; destination < 256u; ++destination) {
        const uint32_t source = ((destination & 0x18u) == 0x08u ||
                                 (destination & 0x18u) == 0x10u)
            ? destination ^ 0x18u : destination;
        const uint32_t intensity = palette256[source] & 0xffu;
        const uint32_t alpha = palette256[source] >> 8u;
        assert(converted256[destination] ==
            intensity * 0x00010101u + (alpha << 24u));
    }

    assert(!ps2GsConvertN64Ia16PaletteToGsCt32(
        palette16, converted16, 32u));
    assert(!ps2GsConvertN64Ia16PaletteToGsCt32(
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

static uint32_t intensity_clut_source_index(uint32_t destination,
    uint32_t entry_count)
{
    return entry_count == 256u &&
        ((destination & 0x18u) == 0x08u ||
         (destination & 0x18u) == 0x10u)
        ? destination ^ 0x18u : destination;
}

static void test_n64_intensity_cluts(void)
{
    uint32_t clut[256] = {};

    assert(ps2GsBuildN64IntensityClut(PS2_GS_N64_IA4, clut, 16u));
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t intensity = (i >> 1u) * 0x24u;
        const uint32_t alpha = (i & 1u) != 0u ? 0xffu : 0u;
        assert(clut[i] == intensity * 0x00010101u + (alpha << 24u));
    }

    assert(ps2GsBuildN64IntensityClut(PS2_GS_N64_I4, clut, 16u));
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t intensity = i * 0x11u;
        assert(clut[i] == intensity * 0x01010101u);
    }

    assert(ps2GsBuildN64IntensityClut(PS2_GS_N64_IA8, clut, 256u));
    for (uint32_t destination = 0; destination < 256u; ++destination) {
        const uint32_t source = intensity_clut_source_index(
            destination, 256u);
        const uint32_t intensity = (source >> 4u) * 0x11u;
        const uint32_t alpha = (source & 0x0fu) * 0x11u;
        assert(clut[destination] ==
            intensity * 0x00010101u + (alpha << 24u));
    }

    assert(ps2GsBuildN64IntensityClut(PS2_GS_N64_I8, clut, 256u));
    for (uint32_t destination = 0; destination < 256u; ++destination) {
        const uint32_t source = intensity_clut_source_index(
            destination, 256u);
        assert(clut[destination] == source * 0x01010101u);
    }

    assert(!ps2GsBuildN64IntensityClut(PS2_GS_N64_IA4, clut, 256u));
    assert(!ps2GsBuildN64IntensityClut(PS2_GS_N64_IA8, clut, 16u));
    assert(!ps2GsBuildN64IntensityClut(
        (enum Ps2GsN64IntensityEncoding)99, clut, 256u));
    assert(!ps2GsBuildN64IntensityClut(PS2_GS_N64_I8, NULL, 256u));
}

static void test_identity_rgba8_clut(void)
{
    uint32_t clut[256] = {};
    assert(ps2GsBuildIdentityRgba8Clut(clut, 256u));
    for (uint32_t destination = 0u; destination < 256u; ++destination) {
        const uint32_t source = intensity_clut_source_index(
            destination, 256u);
        assert(clut[destination] == source * 0x01010101u);
    }

    assert(!ps2GsBuildIdentityRgba8Clut(clut, 16u));
    assert(!ps2GsBuildIdentityRgba8Clut(NULL, 256u));
}

int main(void)
{
    test_primary_colors_and_alpha();
    test_exact_alias_and_empty_input();
    test_all_rgba5551_values_preserve_channels();
    test_all_ia16_values_preserve_channels();
    test_ia16_mirror();
    test_ci4_nibble_order();
    test_mirror_u8();
    test_mirror_n64_four_bit();
    test_mirror_wide_texels();
    test_mirror_fused_native_conversion();
    test_rgba16_palette_conversion_and_csm1_order();
    test_ia16_palette_conversion_and_csm1_order();
    test_gs_texture_buffer_width_alignment();
    test_n64_intensity_cluts();
    test_identity_rgba8_clut();
    puts("gs_texture_convert tests passed");
    return 0;
}
