#ifndef PERFECT_DARK_PC_GFX_PS2_PASS_GRAPH_H
#define PERFECT_DARK_PC_GFX_PS2_PASS_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_GFX_PASS_GRAPH_TILE_WIDTH 128
#define PS2_GFX_PASS_GRAPH_TILE_HEIGHT 64

struct Ps2GfxPassGraphRect {
    int x;
    int y;
    int width;
    int height;
};

struct Ps2GfxPassGraphTriangle {
    float x[3];
    float y[3];
};

struct Ps2GfxPassGraphTiles {
    struct Ps2GfxPassGraphRect bounds;
    int first_tile_x;
    int first_tile_y;
    uint32_t columns;
    uint32_t rows;
};

struct Ps2GfxPassGraphSample {
    float s;
    float t;
};

enum Ps2GfxAlphaEdgeComparison {
    PS2_GFX_ALPHA_EDGE_REJECT = 0,
    PS2_GFX_ALPHA_EDGE_ALWAYS,
    PS2_GFX_ALPHA_EDGE_GEQUAL,
    PS2_GFX_ALPHA_EDGE_LEQUAL,
};

struct Ps2GfxAlphaEdgeTest {
    enum Ps2GfxAlphaEdgeComparison comparison;
    uint8_t reference;
};

/*
 * Screen-linear payload used to split signed alpha equations at zero without
 * asking the GS to represent a negative framebuffer channel. ST and Q are
 * carried separately so newly created vertices preserve the original
 * perspective-correct texture plane.
 */
struct Ps2GfxSignedAlphaVertex {
    float x;
    float y;
    float z;
    float s;
    float t;
    float q;
    float delta;
};

#define PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES 6u

struct Ps2GfxSignedAlphaTriangles {
    struct Ps2GfxSignedAlphaVertex
        vertices[PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES];
    uint32_t vertex_count;
};

/*
 * Clip a screen-space triangle to the active framebuffer/scissor rectangle and
 * describe the fixed-size workspace tiles that cover its conservative pixel
 * bounds. The returned tile rectangles are screen-space, half-open regions.
 */
bool ps2GfxDescribePassGraphTiles(
    const struct Ps2GfxPassGraphTriangle *triangle,
    const struct Ps2GfxPassGraphRect *clip,
    struct Ps2GfxPassGraphTiles *tiles);

bool ps2GfxGetPassGraphTile(
    const struct Ps2GfxPassGraphTiles *tiles, uint32_t index,
    struct Ps2GfxPassGraphRect *tile);

/* GS STQ is normalized; convert a screen point into one workspace texture. */
struct Ps2GfxPassGraphSample ps2GfxMapPassGraphSample(
    float screen_x, float screen_y, int tile_origin_x, int tile_origin_y);

/*
 * Clip one triangle to delta >= 0 or delta < 0 and return a triangle list.
 * A half-plane can turn one triangle into a quad, hence the six-vertex bound.
 */
bool ps2GfxClipSignedAlphaTriangle(
    const struct Ps2GfxSignedAlphaVertex triangle[3],
    bool positive,
    struct Ps2GfxSignedAlphaTriangles *result);

/* Plan PRIMITIVE + signed_term >= threshold for one signed half-plane. */
struct Ps2GfxAlphaEdgeTest ps2GfxPlanSignedAlphaEdgeTest(
    uint8_t threshold, uint8_t primitive_alpha, bool positive);

#ifdef __cplusplus
}
#endif

#endif
