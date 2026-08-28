#include "gs_render_target_layout.h"

#include <assert.h>
#include <stdio.h>

static void test_ct32_page_footprints(void)
{
    Ps2GsRenderTargetLayout layout{};

    assert(ps2GsDescribeCt32RenderTarget(1u, 1u, &layout));
    assert(layout.fbw == 1u);
    assert(layout.bytes == 0x2000u);

    assert(ps2GsDescribeCt32RenderTarget(64u, 32u, &layout));
    assert(layout.fbw == 1u);
    assert(layout.bytes == 0x2000u);

    assert(ps2GsDescribeCt32RenderTarget(65u, 33u, &layout));
    assert(layout.fbw == 2u);
    assert(layout.bytes == 0x8000u);

    assert(ps2GsDescribeCt32RenderTarget(320u, 240u, &layout));
    assert(layout.fbw == 5u);
    assert(layout.bytes == 0x50000u);

    assert(ps2GsDescribeCt32RenderTarget(640u, 448u, &layout));
    assert(layout.fbw == 10u);
    assert(layout.bytes == 0x118000u);
}

static void test_invalid_layouts(void)
{
    Ps2GsRenderTargetLayout layout{};
    assert(!ps2GsDescribeCt32RenderTarget(0u, 1u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 0u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1025u, 1u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 1025u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 1u, NULL));
}

static void test_ct32_texture_views(void)
{
    Ps2GsRenderTargetLayout layout{};
    Ps2GsRenderTargetTextureView view{};

    assert(ps2GsDescribeCt32RenderTarget(1u, 1u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 1u);
    assert(view.tw == 0u);
    assert(view.th == 0u);
    assert(view.clamp_max_u == 0u);
    assert(view.clamp_max_v == 0u);

    assert(ps2GsDescribeCt32RenderTarget(65u, 33u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 2u);
    assert(view.tw == 7u);
    assert(view.th == 6u);
    assert(view.clamp_max_u == 64u);
    assert(view.clamp_max_v == 32u);

    assert(ps2GsDescribeCt32RenderTarget(1024u, 1024u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 16u);
    assert(view.tw == 10u);
    assert(view.th == 10u);
    assert(view.clamp_max_u == 1023u);
    assert(view.clamp_max_v == 1023u);
}

static void test_invalid_texture_views(void)
{
    Ps2GsRenderTargetLayout layout{};
    Ps2GsRenderTargetTextureView view{};

    assert(!ps2GsDescribeCt32RenderTargetTextureView(NULL, &view));
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, NULL));
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));

    assert(ps2GsDescribeCt32RenderTarget(64u, 32u, &layout));
    ++layout.bytes;
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
}

static void decode_t8_page_coordinate(uint32_t u, uint32_t v,
    uint32_t *x, uint32_t *y, Ps2GsCt32Channel *channel)
{
    *x = ((u & ~15u) >> 1u) | (u & 7u);
    *y = ((v & ~3u) >> 1u) | (v & 1u);
    *x ^= (v & 4u) ^ ((v & 2u) << 1u);

    const bool right_lane = (u & 8u) != 0u;
    const bool lower_lane = (v & 2u) != 0u;
    if (lower_lane) {
        *channel = right_lane ? PS2_GS_CT32_CHANNEL_ALPHA :
            PS2_GS_CT32_CHANNEL_GREEN;
    } else {
        *channel = right_lane ? PS2_GS_CT32_CHANNEL_BLUE :
            PS2_GS_CT32_CHANNEL_RED;
    }
}

static void test_ct32_to_t8_page_channel_mapping(void)
{
    for (uint32_t y = 0u; y < 32u; ++y) {
        for (uint32_t x = 0u; x < 64u; ++x) {
            for (uint32_t lane = PS2_GS_CT32_CHANNEL_RED;
                 lane <= PS2_GS_CT32_CHANNEL_ALPHA; ++lane) {
                Ps2GsT8PageCoordinate coordinate{};
                assert(ps2GsMapCt32PixelChannelToT8Page(
                    x, y, (Ps2GsCt32Channel)lane, &coordinate));
                assert(coordinate.u < 128u);
                assert(coordinate.v < 64u);

                uint32_t decoded_x = 0u;
                uint32_t decoded_y = 0u;
                Ps2GsCt32Channel decoded_channel =
                    PS2_GS_CT32_CHANNEL_RED;
                decode_t8_page_coordinate(
                    coordinate.u, coordinate.v,
                    &decoded_x, &decoded_y, &decoded_channel);
                assert(decoded_x == x);
                assert(decoded_y == y);
                assert(decoded_channel == (Ps2GsCt32Channel)lane);
            }
        }
    }

    Ps2GsT8PageCoordinate coordinate{};
    assert(!ps2GsMapCt32PixelChannelToT8Page(
        64u, 0u, PS2_GS_CT32_CHANNEL_RED, &coordinate));
    assert(!ps2GsMapCt32PixelChannelToT8Page(
        0u, 32u, PS2_GS_CT32_CHANNEL_RED, &coordinate));
    assert(!ps2GsMapCt32PixelChannelToT8Page(
        0u, 0u, (Ps2GsCt32Channel)4, &coordinate));
    assert(!ps2GsMapCt32PixelChannelToT8Page(
        0u, 0u, PS2_GS_CT32_CHANNEL_RED, NULL));
}

static void test_red_lane_region_repeat_mapping(void)
{
    for (uint32_t y = 0u; y < 32u; y += 2u) {
        for (uint32_t tile_x = 0u; tile_x < 64u; tile_x += 8u) {
            Ps2GsT8PageCoordinate first{};
            assert(ps2GsMapCt32PixelChannelToT8Page(
                tile_x, y, PS2_GS_CT32_CHANNEL_RED, &first));

            const uint32_t raw_tile_x = tile_x * 2u;
            const uint32_t u_xor = first.u - raw_tile_x;
            assert(u_xor == 0u || u_xor == 4u);
            for (uint32_t x = 0u; x < 8u; ++x) {
                Ps2GsT8PageCoordinate coordinate{};
                assert(ps2GsMapCt32PixelChannelToT8Page(
                    tile_x + x, y, PS2_GS_CT32_CHANNEL_RED,
                    &coordinate));
                assert(coordinate.v == first.v);
                const uint32_t region_repeat_u =
                    ((u_xor + x) & 7u) | raw_tile_x;
                assert(coordinate.u == region_repeat_u);
            }

            Ps2GsT8PageCoordinate second_row{};
            assert(ps2GsMapCt32PixelChannelToT8Page(
                tile_x, y + 1u, PS2_GS_CT32_CHANNEL_RED,
                &second_row));
            assert(second_row.u == first.u);
            assert(second_row.v == first.v + 1u);
        }
    }
}

static void test_red_lane_known_psmt8_anchors(void)
{
    struct Anchor {
        uint32_t x;
        uint32_t y;
        uint32_t u;
        uint32_t v;
    };
    static const Anchor anchors[] = {
        { 0u, 0u, 0u, 0u },
        { 7u, 0u, 7u, 0u },
        { 8u, 0u, 16u, 0u },
        { 0u, 1u, 0u, 1u },
        { 0u, 2u, 4u, 4u },
        { 0u, 3u, 4u, 5u },
        { 0u, 4u, 0u, 8u },
        { 32u, 0u, 64u, 0u },
        { 63u, 31u, 115u, 61u },
    };

    for (const Anchor &anchor : anchors) {
        Ps2GsT8PageCoordinate coordinate{};
        assert(ps2GsMapCt32PixelChannelToT8Page(
            anchor.x, anchor.y, PS2_GS_CT32_CHANNEL_RED,
            &coordinate));
        assert(coordinate.u == anchor.u);
        assert(coordinate.v == anchor.v);
    }
}

int main(void)
{
    test_ct32_page_footprints();
    test_invalid_layouts();
    test_ct32_texture_views();
    test_invalid_texture_views();
    test_ct32_to_t8_page_channel_mapping();
    test_red_lane_region_repeat_mapping();
    test_red_lane_known_psmt8_anchors();
    puts("gs_render_target_layout tests passed");
    return 0;
}
