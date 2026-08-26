#include "rdp_tmem.h"
#include "rdp_tmem_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_tlut(void)
{
    GfxRdpTmem tmem{};
    const uint16_t entries[] = {0xf801u, 0x1234u};

    assert(gfxRdpTmemWriteTlut(&tmem, 256u, entries, 2u));
    const uint8_t *bytes = gfxRdpTmemBytes(&tmem);
    const uint8_t expected0[] = {0xf8, 0x01, 0xf8, 0x01, 0xf8, 0x01, 0xf8, 0x01};
    const uint8_t expected1[] = {0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34};

    assert(memcmp(bytes + 256u * 8u, expected0, sizeof(expected0)) == 0);
    assert(memcmp(bytes + 257u * 8u, expected1, sizeof(expected1)) == 0);
    assert(gfxRdpTmemWordValid(&tmem, 256u));
    assert(gfxRdpTmemWordGeneration(&tmem, 256u) != 0u);

    uint16_t decoded[2]{};
    assert(gfxRdpTmemReadTlut(&tmem, 256u, decoded, 2u));
    assert(decoded[0] == entries[0]);
    assert(decoded[1] == entries[1]);
}

static void test_linear_tile_roundtrip_and_padding(void)
{
    GfxRdpTmem tmem{};
    uint8_t source[26];
    uint8_t decoded[26]{};
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)(0x20u + i);
    }

    assert(gfxRdpTmemLoadTileLinear(&tmem, 8u, 2u,
        source, 13u, 13u, 2u));
    assert(gfxRdpTmemReadTileLinear(&tmem, 8u, 2u,
        decoded, 13u, 13u, 2u));
    assert(memcmp(source, decoded, sizeof(source)) == 0);

    const uint32_t even_partial = 9u * 8u;
    for (uint32_t i = 0; i < 5u; ++i) {
        assert(gfxRdpTmemByteValid(&tmem, even_partial + i));
    }
    for (uint32_t i = 5u; i < 8u; ++i) {
        assert(!gfxRdpTmemByteValid(&tmem, even_partial + i));
    }
    assert(!gfxRdpTmemWordValid(&tmem, 9u));

    const uint32_t odd_partial = 11u * 8u;
    assert(gfxRdpTmemByteValid(&tmem, odd_partial + 0u));
    for (uint32_t i = 1u; i < 4u; ++i) {
        assert(!gfxRdpTmemByteValid(&tmem, odd_partial + i));
    }
    for (uint32_t i = 4u; i < 8u; ++i) {
        assert(gfxRdpTmemByteValid(&tmem, odd_partial + i));
    }
}

static void test_loadblock_dxt(void)
{
    GfxRdpTmem tmem{};
    uint8_t source[32];
    for (uint32_t i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)i;
    }

    assert(gfxRdpTmemLoadBlockLinear(&tmem, 0u, source, sizeof(source), 0x400u));
    const uint8_t *bytes = gfxRdpTmemBytes(&tmem);
    assert(memcmp(bytes + 0u, source + 0u, 8u) == 0);
    assert(memcmp(bytes + 8u, source + 8u, 8u) == 0);

    const uint8_t swapped2[] = {20, 21, 22, 23, 16, 17, 18, 19};
    const uint8_t swapped3[] = {28, 29, 30, 31, 24, 25, 26, 27};
    assert(memcmp(bytes + 16u, swapped2, 8u) == 0);
    assert(memcmp(bytes + 24u, swapped3, 8u) == 0);

    GfxRdpTmem preswapped{};
    assert(gfxRdpTmemLoadBlockLinear(&preswapped, 0u,
        source, sizeof(source), 0u));
    assert(memcmp(gfxRdpTmemBytes(&preswapped), source, sizeof(source)) == 0);
}

static void test_rgba32_figure_14_15_layout(void)
{
    GfxRdpTmem tmem{};
    uint8_t source[2u * 6u * 4u];
    uint8_t decoded[sizeof(source)]{};

    for (uint32_t row = 0; row < 2u; ++row) {
        for (uint32_t x = 0; x < 6u; ++x) {
            const uint32_t texel = row * 6u + x;
            uint8_t *p = source + texel * 4u;
            p[0] = (uint8_t)(0x10u + texel);
            p[1] = (uint8_t)(0x30u + texel);
            p[2] = (uint8_t)(0x50u + texel);
            p[3] = (uint8_t)(0x70u + texel);
        }
    }

    assert(gfxRdpTmemLoadTileRgba32(&tmem, 0u, 2u,
        source, 24u, 6u, 2u));
    assert(gfxRdpTmemReadTileRgba32(&tmem, 0u, 2u,
        decoded, 24u, 6u, 2u));
    assert(memcmp(source, decoded, sizeof(source)) == 0);

    const uint8_t *bytes = gfxRdpTmemBytes(&tmem);
    const uint8_t row0_low0[] = {
        0x10, 0x30, 0x11, 0x31, 0x12, 0x32, 0x13, 0x33
    };
    assert(memcmp(bytes, row0_low0, sizeof(row0_low0)) == 0);

    const uint8_t row1_low0[] = {
        0x18, 0x38, 0x19, 0x39, 0x16, 0x36, 0x17, 0x37
    };
    assert(memcmp(bytes + 2u * 8u, row1_low0, sizeof(row1_low0)) == 0);

    const uint8_t row0_high0[] = {
        0x50, 0x70, 0x51, 0x71, 0x52, 0x72, 0x53, 0x73
    };
    assert(memcmp(bytes + GFX_RDP_TMEM_HALF_WORDS * 8u,
        row0_high0, sizeof(row0_high0)) == 0);

    assert(!gfxRdpTmemWordValid(&tmem, 1u));
    assert(!gfxRdpTmemWordValid(&tmem, GFX_RDP_TMEM_HALF_WORDS + 1u));
    assert(gfxRdpTmemByteValid(&tmem, 1u * 8u + 0u));
    assert(gfxRdpTmemByteValid(&tmem, 1u * 8u + 3u));
    assert(!gfxRdpTmemByteValid(&tmem, 1u * 8u + 4u));
}

static void test_rgba32_loadblock_dxt_roundtrip(void)
{
    GfxRdpTmem tmem{};
    uint8_t source[2u * 4u * 4u];
    uint8_t decoded[sizeof(source)]{};

    for (uint32_t texel = 0; texel < 8u; ++texel) {
        uint8_t *p = source + texel * 4u;
        p[0] = (uint8_t)(0x10u + texel);
        p[1] = (uint8_t)(0x30u + texel);
        p[2] = (uint8_t)(0x50u + texel);
        p[3] = (uint8_t)(0x70u + texel);
    }

    assert(gfxRdpTmemLoadBlockRgba32(&tmem, 0u, 0u,
        source, 8u, 0x400u));
    assert(gfxRdpTmemReadTileRgba32(&tmem, 0u, 1u,
        decoded, 16u, 4u, 2u));
    assert(memcmp(source, decoded, sizeof(source)) == 0);

    const uint8_t *bytes = gfxRdpTmemBytes(&tmem);
    const uint8_t odd_row_low[] = {
        0x16, 0x36, 0x17, 0x37, 0x14, 0x34, 0x15, 0x35
    };
    assert(memcmp(bytes + 8u, odd_row_low, sizeof(odd_row_low)) == 0);
}

static void test_rgba32_loadblock_odd_texel_validity(void)
{
    GfxRdpTmem tmem{};
    const uint8_t source[5u * 4u] = {
        1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16, 17,18,19,20
    };

    assert(gfxRdpTmemLoadBlockRgba32(&tmem, 0u, 0u,
        source, 5u, 0u));

    assert(gfxRdpTmemByteValid(&tmem, 8u));
    assert(gfxRdpTmemByteValid(&tmem, 9u));
    assert(!gfxRdpTmemByteValid(&tmem, 10u));
    assert(!gfxRdpTmemByteValid(&tmem, 11u));
    assert(gfxRdpTmemByteValid(&tmem,
        GFX_RDP_TMEM_BYTES / 2u + 8u));
    assert(!gfxRdpTmemByteValid(&tmem,
        GFX_RDP_TMEM_BYTES / 2u + 10u));
}

static void test_ordered_runtime_rgba32_block(void)
{
    enum : uint8_t {
        FMT_RGBA = 0u,
        SIZ_32B = 3u,
    };

    GfxRdpTmemRuntime runtime{};
    uint8_t source[8u * 4u];
    uint8_t decoded[sizeof(source)]{};
    for (uint32_t texel = 0; texel < 8u; ++texel) {
        source[texel * 4u + 0u] = (uint8_t)(0x10u + texel);
        source[texel * 4u + 1u] = (uint8_t)(0x20u + texel);
        source[texel * 4u + 2u] = (uint8_t)(0x30u + texel);
        source[texel * 4u + 3u] = (uint8_t)(0x40u + texel);
    }

    gfxRdpTmemRuntimeReset(&runtime);
    gfxRdpTmemRuntimeSetTextureImage(&runtime,
        FMT_RGBA, SIZ_32B, 3u, source);
    assert(gfxRdpTmemRuntimeSetTile(&runtime,
        7u, FMT_RGBA, SIZ_32B, 0u, 0u));
    assert(gfxRdpTmemRuntimeLoadBlock(&runtime,
        7u, 0u, 0u, 7u, 0x400u) == GFX_RDP_TMEM_LOAD_EXACT);

    assert(gfxRdpTmemReadTileRgba32(gfxRdpTmemRuntimeState(&runtime),
        0u, 1u, decoded, 16u, 4u, 2u));
    assert(memcmp(source, decoded, sizeof(source)) == 0);
}

static void test_ordered_runtime_yuv_is_conservative(void)
{
    enum : uint8_t {
        FMT_YUV = 1u,
        SIZ_16B = 2u,
    };

    GfxRdpTmemRuntime runtime{};
    const uint8_t seed[8] = {1,2,3,4,5,6,7,8};
    const uint8_t source[16] = {};

    gfxRdpTmemRuntimeReset(&runtime);
    assert(gfxRdpTmemWritePhysical(&runtime.tmem, 0u, seed, sizeof(seed)));
    assert(gfxRdpTmemByteValid(&runtime.tmem, 0u));

    gfxRdpTmemRuntimeSetTextureImage(&runtime,
        FMT_YUV, SIZ_16B, 3u, source);
    assert(gfxRdpTmemRuntimeSetTile(&runtime,
        7u, FMT_YUV, SIZ_16B, 1u, 0u));
    assert(gfxRdpTmemRuntimeLoadTile(&runtime,
        7u, 0u, 0u, 12u, 0u) == GFX_RDP_TMEM_LOAD_CONSERVATIVE);
    assert(!gfxRdpTmemByteValid(&runtime.tmem, 0u));
}

static void test_invalidation_blocks_readback(void)
{
    GfxRdpTmem tmem{};
    const uint8_t source[8] = {1,2,3,4,5,6,7,8};
    uint8_t decoded[8]{};

    assert(gfxRdpTmemLoadTileLinear(&tmem, 4u, 1u,
        source, 8u, 8u, 1u));
    assert(gfxRdpTmemReadTileLinear(&tmem, 4u, 1u,
        decoded, 8u, 8u, 1u));
    assert(gfxRdpTmemInvalidatePhysical(&tmem, 4u * 8u + 3u, 1u));
    assert(!gfxRdpTmemReadTileLinear(&tmem, 4u, 1u,
        decoded, 8u, 8u, 1u));
}

static void test_partial_final_row_readback(void)
{
    GfxRdpTmem linear{};
    uint8_t source_linear[15];
    uint8_t decoded_linear[13]{};
    for (uint32_t i = 0; i < sizeof(source_linear); ++i) {
        source_linear[i] = (uint8_t)(0x40u + i);
    }

    assert(gfxRdpTmemLoadTileLinear(&linear, 0u, 1u,
        source_linear, 5u, 5u, 3u));
    assert(gfxRdpTmemReadTileLinearBytes(&linear, 0u, 1u,
        decoded_linear, 5u, sizeof(decoded_linear)));
    assert(memcmp(source_linear, decoded_linear, sizeof(decoded_linear)) == 0);

    GfxRdpTmem rgba32{};
    uint8_t source_rgba32[6u * 4u];
    uint8_t decoded_rgba32[5u * 4u]{};
    for (uint32_t i = 0; i < sizeof(source_rgba32); ++i) {
        source_rgba32[i] = (uint8_t)(0x80u + i);
    }

    assert(gfxRdpTmemLoadTileRgba32(&rgba32, 0u, 1u,
        source_rgba32, 3u * 4u, 3u, 2u));
    assert(gfxRdpTmemReadTileRgba32Texels(&rgba32, 0u, 1u,
        decoded_rgba32, 3u, 5u));
    assert(memcmp(source_rgba32, decoded_rgba32,
        sizeof(decoded_rgba32)) == 0);
}

int main(void)
{
    test_tlut();
    test_linear_tile_roundtrip_and_padding();
    test_loadblock_dxt();
    test_rgba32_figure_14_15_layout();
    test_rgba32_loadblock_dxt_roundtrip();
    test_rgba32_loadblock_odd_texel_validity();
    test_ordered_runtime_rgba32_block();
    test_ordered_runtime_yuv_is_conservative();
    test_invalidation_blocks_readback();
    test_partial_final_row_readback();
    puts("rdp_tmem tests passed");
    return 0;
}
