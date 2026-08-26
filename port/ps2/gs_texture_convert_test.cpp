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

int main(void)
{
    test_primary_colors_and_alpha();
    test_exact_alias_and_empty_input();
    test_all_rgba5551_values_preserve_channels();
    puts("gs_texture_convert tests passed");
    return 0;
}
