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

#ifdef __cplusplus
}
#endif

#endif
