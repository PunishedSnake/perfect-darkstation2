#include "gfx_ps2_pass_graph.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool nearly_equal(float a, float b)
{
    return fabsf(a - b) < 0.0001f;
}

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

static void test_splits_signed_alpha_and_preserves_stq_planes(void)
{
    const struct Ps2GfxSignedAlphaVertex triangle[3] = {
        { 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f, -0.5f },
        { 4.0f, 0.0f, 20.0f, 2.0f, 0.0f, 0.5f, 0.5f },
        { 0.0f, 4.0f, 30.0f, 0.0f, 1.0f, 0.25f, 0.5f },
    };
    struct Ps2GfxSignedAlphaTriangles positive = {};
    struct Ps2GfxSignedAlphaTriangles negative = {};
    assert(ps2GfxClipSignedAlphaTriangle(
        triangle, true, &positive));
    assert(ps2GfxClipSignedAlphaTriangle(
        triangle, false, &negative));
    assert(positive.vertex_count == 6u);
    assert(negative.vertex_count == 3u);

    /* The two zero crossings are halfway along their source edges. */
    bool saw_top_crossing = false;
    bool saw_left_crossing = false;
    for (uint32_t i = 0u; i < positive.vertex_count; ++i) {
        const struct Ps2GfxSignedAlphaVertex *vertex =
            &positive.vertices[i];
        if (nearly_equal(vertex->x, 2.0f) &&
            nearly_equal(vertex->y, 0.0f)) {
            assert(nearly_equal(vertex->z, 15.0f));
            assert(nearly_equal(vertex->s, 1.0f));
            assert(nearly_equal(vertex->q, 0.75f));
            saw_top_crossing = true;
        }
        if (nearly_equal(vertex->x, 0.0f) &&
            nearly_equal(vertex->y, 2.0f)) {
            assert(nearly_equal(vertex->z, 20.0f));
            assert(nearly_equal(vertex->t, 0.5f));
            assert(nearly_equal(vertex->q, 0.625f));
            saw_left_crossing = true;
        }
    }
    assert(saw_top_crossing && saw_left_crossing);
}

static void test_signed_alpha_edge_test_plan(void)
{
    struct Ps2GfxAlphaEdgeTest test =
        ps2GfxPlanSignedAlphaEdgeTest(25u, 10u, true);
    assert(test.comparison == PS2_GFX_ALPHA_EDGE_GEQUAL);
    assert(test.reference == 15u);

    test = ps2GfxPlanSignedAlphaEdgeTest(25u, 40u, true);
    assert(test.comparison == PS2_GFX_ALPHA_EDGE_ALWAYS);

    test = ps2GfxPlanSignedAlphaEdgeTest(25u, 10u, false);
    assert(test.comparison == PS2_GFX_ALPHA_EDGE_REJECT);

    test = ps2GfxPlanSignedAlphaEdgeTest(25u, 40u, false);
    assert(test.comparison == PS2_GFX_ALPHA_EDGE_LEQUAL);
    assert(test.reference == 15u);
}

int main(void)
{
    test_clips_and_partitions_large_triangle();
    test_respects_nonzero_scissor();
    test_rejects_empty_or_outside_geometry();
    test_maps_screen_pixels_to_normalized_stq();
    test_splits_signed_alpha_and_preserves_stq_planes();
    test_signed_alpha_edge_test_plan();
    puts("gfx_ps2_pass_graph tests passed");
    return 0;
}
