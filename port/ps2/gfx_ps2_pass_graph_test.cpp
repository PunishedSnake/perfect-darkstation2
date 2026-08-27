#include "gfx_ps2_pass_graph.h"

#include <assert.h>
#include <stdio.h>

static void test_clips_and_partitions_large_triangle(void)
{
    const struct Ps2GfxPassGraphTriangle triangle = {
        { -12.25f, 300.5f, 4.0f },
        { 3.5f, 2.0f, 130.25f },
    };
    const struct Ps2GfxPassGraphRect clip = { 0, 0, 320, 240 };
    struct Ps2GfxPassGraphTiles tiles = {};
    assert(ps2GfxDescribePassGraphTiles(&triangle, &clip, &tiles));
    assert(tiles.bounds.x == 0);
    assert(tiles.bounds.y == 2);
    assert(tiles.bounds.width == 301);
    assert(tiles.bounds.height == 129);
    assert(tiles.columns == 3u);
    assert(tiles.rows == 3u);

    struct Ps2GfxPassGraphRect tile = {};
    assert(ps2GfxGetPassGraphTile(&tiles, 0u, &tile));
    assert(tile.x == 0 && tile.y == 2);
    assert(tile.width == 128 && tile.height == 62);
    assert(ps2GfxGetPassGraphTile(&tiles, 8u, &tile));
    assert(tile.x == 256 && tile.y == 128);
    assert(tile.width == 45 && tile.height == 3);
    assert(!ps2GfxGetPassGraphTile(&tiles, 9u, &tile));
}

static void test_respects_nonzero_scissor(void)
{
    const struct Ps2GfxPassGraphTriangle triangle = {
        { 40.0f, 250.0f, 80.0f },
        { 20.0f, 80.0f, 180.0f },
    };
    const struct Ps2GfxPassGraphRect clip = { 64, 32, 128, 96 };
    struct Ps2GfxPassGraphTiles tiles = {};
    assert(ps2GfxDescribePassGraphTiles(&triangle, &clip, &tiles));
    assert(tiles.bounds.x == 64 && tiles.bounds.y == 32);
    assert(tiles.bounds.width == 128 && tiles.bounds.height == 96);
    assert(tiles.first_tile_x == 0);
    assert(tiles.first_tile_y == 0);
    assert(tiles.columns == 2u && tiles.rows == 2u);
}

static void test_rejects_empty_or_outside_geometry(void)
{
    const struct Ps2GfxPassGraphRect clip = { 0, 0, 320, 240 };
    struct Ps2GfxPassGraphTiles tiles = {};
    const struct Ps2GfxPassGraphTriangle point = {
        { 4.0f, 4.0f, 4.0f }, { 8.0f, 8.0f, 8.0f },
    };
    assert(!ps2GfxDescribePassGraphTiles(&point, &clip, &tiles));

    const struct Ps2GfxPassGraphTriangle outside = {
        { -30.0f, -20.0f, -10.0f }, { 5.0f, 50.0f, 20.0f },
    };
    assert(!ps2GfxDescribePassGraphTiles(&outside, &clip, &tiles));
}

static void test_maps_screen_pixels_to_normalized_stq(void)
{
    struct Ps2GfxPassGraphSample sample =
        ps2GfxMapPassGraphSample(192.0f, 96.0f, 128, 64);
    assert(sample.s == 0.5f);
    assert(sample.t == 0.5f);

    sample = ps2GfxMapPassGraphSample(128.0f, 64.0f, 128, 64);
    assert(sample.s == 0.0f);
    assert(sample.t == 0.0f);

    sample = ps2GfxMapPassGraphSample(256.0f, 128.0f, 128, 64);
    assert(sample.s == 1.0f);
    assert(sample.t == 1.0f);
}

int main(void)
{
    test_clips_and_partitions_large_triangle();
    test_respects_nonzero_scissor();
    test_rejects_empty_or_outside_geometry();
    test_maps_screen_pixels_to_normalized_stq();
    puts("gfx_ps2_pass_graph tests passed");
    return 0;
}
