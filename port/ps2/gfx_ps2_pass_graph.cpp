#include "gfx_ps2_pass_graph.h"

#include <limits.h>
#include <stddef.h>

static int ps2GfxFloorToInt(float value)
{
    if (value <= (float)INT_MIN) return INT_MIN;
    if (value >= (float)INT_MAX) return INT_MAX;
    const int integer = (int)value;
    return (float)integer > value ? integer - 1 : integer;
}

static int ps2GfxCeilToInt(float value)
{
    if (value <= (float)INT_MIN) return INT_MIN;
    if (value >= (float)INT_MAX) return INT_MAX;
    const int integer = (int)value;
    return (float)integer < value ? integer + 1 : integer;
}

static int ps2GfxMaxInt(int a, int b)
{
    return a > b ? a : b;
}

static int ps2GfxMinInt(int a, int b)
{
    return a < b ? a : b;
}

extern "C" bool ps2GfxDescribePassGraphTiles(
    const struct Ps2GfxPassGraphTriangle *triangle,
    const struct Ps2GfxPassGraphRect *clip,
    struct Ps2GfxPassGraphTiles *tiles)
{
    if (!triangle || !clip || !tiles ||
        clip->x < 0 || clip->y < 0 ||
        clip->width <= 0 || clip->height <= 0 ||
        clip->x > INT_MAX - clip->width ||
        clip->y > INT_MAX - clip->height) {
        return false;
    }

    float min_x = triangle->x[0];
    float max_x = triangle->x[0];
    float min_y = triangle->y[0];
    float max_y = triangle->y[0];
    for (uint32_t i = 1u; i < 3u; ++i) {
        if (triangle->x[i] < min_x) min_x = triangle->x[i];
        if (triangle->x[i] > max_x) max_x = triangle->x[i];
        if (triangle->y[i] < min_y) min_y = triangle->y[i];
        if (triangle->y[i] > max_y) max_y = triangle->y[i];
    }

    const int clip_x1 = clip->x + clip->width;
    const int clip_y1 = clip->y + clip->height;
    const int x0 = ps2GfxMaxInt(ps2GfxFloorToInt(min_x), clip->x);
    const int y0 = ps2GfxMaxInt(ps2GfxFloorToInt(min_y), clip->y);
    const int x1 = ps2GfxMinInt(ps2GfxCeilToInt(max_x), clip_x1);
    const int y1 = ps2GfxMinInt(ps2GfxCeilToInt(max_y), clip_y1);
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    tiles->bounds.x = x0;
    tiles->bounds.y = y0;
    tiles->bounds.width = x1 - x0;
    tiles->bounds.height = y1 - y0;
    tiles->first_tile_x =
        (x0 / PS2_GFX_PASS_GRAPH_TILE_WIDTH) *
        PS2_GFX_PASS_GRAPH_TILE_WIDTH;
    tiles->first_tile_y =
        (y0 / PS2_GFX_PASS_GRAPH_TILE_HEIGHT) *
        PS2_GFX_PASS_GRAPH_TILE_HEIGHT;
    tiles->columns = (uint32_t)(
        (x1 - tiles->first_tile_x + PS2_GFX_PASS_GRAPH_TILE_WIDTH - 1) /
        PS2_GFX_PASS_GRAPH_TILE_WIDTH);
    tiles->rows = (uint32_t)(
        (y1 - tiles->first_tile_y + PS2_GFX_PASS_GRAPH_TILE_HEIGHT - 1) /
        PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
    return tiles->columns != 0u && tiles->rows != 0u;
}

extern "C" bool ps2GfxGetPassGraphTile(
    const struct Ps2GfxPassGraphTiles *tiles, uint32_t index,
    struct Ps2GfxPassGraphRect *tile)
{
    if (!tiles || !tile || tiles->columns == 0u || tiles->rows == 0u ||
        index >= tiles->columns * tiles->rows) {
        return false;
    }

    const uint32_t column = index % tiles->columns;
    const uint32_t row = index / tiles->columns;
    const int raw_x = tiles->first_tile_x +
        (int)column * PS2_GFX_PASS_GRAPH_TILE_WIDTH;
    const int raw_y = tiles->first_tile_y +
        (int)row * PS2_GFX_PASS_GRAPH_TILE_HEIGHT;
    const int bounds_x1 = tiles->bounds.x + tiles->bounds.width;
    const int bounds_y1 = tiles->bounds.y + tiles->bounds.height;
    const int x0 = ps2GfxMaxInt(raw_x, tiles->bounds.x);
    const int y0 = ps2GfxMaxInt(raw_y, tiles->bounds.y);
    const int x1 = ps2GfxMinInt(
        raw_x + PS2_GFX_PASS_GRAPH_TILE_WIDTH, bounds_x1);
    const int y1 = ps2GfxMinInt(
        raw_y + PS2_GFX_PASS_GRAPH_TILE_HEIGHT, bounds_y1);
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    tile->x = x0;
    tile->y = y0;
    tile->width = x1 - x0;
    tile->height = y1 - y0;
    return true;
}

extern "C" struct Ps2GfxPassGraphSample ps2GfxMapPassGraphSample(
    float screen_x, float screen_y, int tile_origin_x, int tile_origin_y)
{
    const struct Ps2GfxPassGraphSample sample = {
        (screen_x - (float)tile_origin_x) /
            (float)PS2_GFX_PASS_GRAPH_TILE_WIDTH,
        (screen_y - (float)tile_origin_y) /
            (float)PS2_GFX_PASS_GRAPH_TILE_HEIGHT,
    };
    return sample;
}

static void ps2GfxIncludeSpanX(
    float x, bool *hit, float *min_x, float *max_x)
{
    if (!*hit) {
        *min_x = x;
        *max_x = x;
        *hit = true;
        return;
    }
    if (x < *min_x) *min_x = x;
    if (x > *max_x) *max_x = x;
}

static void ps2GfxIncludeEdgeAtY(
    float ax, float ay, float bx, float by, float y,
    bool *hit, float *min_x, float *max_x)
{
    if (ay == by) {
        return;
    }
    const float edge_min_y = ay < by ? ay : by;
    const float edge_max_y = ay > by ? ay : by;
    if (y < edge_min_y || y > edge_max_y) {
        return;
    }
    const float factor = (y - ay) / (by - ay);
    ps2GfxIncludeSpanX(
        ax + (bx - ax) * factor, hit, min_x, max_x);
}

extern "C" bool ps2GfxDescribePassGraphRowSpans(
    const struct Ps2GfxPassGraphTriangle *triangle,
    const struct Ps2GfxPassGraphRect *tile,
    struct Ps2GfxPassGraphRowSpans *spans)
{
    if (!triangle || !tile || !spans ||
        tile->width <= 0 || tile->height <= 0 ||
        tile->width > PS2_GFX_PASS_GRAPH_TILE_WIDTH ||
        tile->height > PS2_GFX_PASS_GRAPH_TILE_HEIGHT) {
        return false;
    }

    const uint32_t row_count = (uint32_t)(
        (tile->height + PS2_GFX_PASS_GRAPH_SHUFFLE_ROW_HEIGHT - 1) /
        PS2_GFX_PASS_GRAPH_SHUFFLE_ROW_HEIGHT);
    if (row_count > PS2_GFX_PASS_GRAPH_SHUFFLE_ROWS) {
        return false;
    }

    spans->row_count = row_count;
    for (uint32_t row = 0u; row < row_count; ++row) {
        const int local_y0 =
            (int)row * PS2_GFX_PASS_GRAPH_SHUFFLE_ROW_HEIGHT;
        int local_y1 =
            local_y0 + PS2_GFX_PASS_GRAPH_SHUFFLE_ROW_HEIGHT;
        if (local_y1 > tile->height) {
            local_y1 = tile->height;
        }

        /*
         * Expand the strip and x extent by one pixel. The GS shuffle works in
         * coarse 8x2 blocks; spending at most one neighbouring block here is
         * much cheaper than risking a raster-edge false negative.
         */
        const float strip_y0 = (float)(tile->y + local_y0) - 1.0f;
        const float strip_y1 = (float)(tile->y + local_y1) + 1.0f;
        bool hit = false;
        float min_x = 0.0f;
        float max_x = 0.0f;

        for (uint32_t i = 0u; i < 3u; ++i) {
            const float y = triangle->y[i];
            if (y >= strip_y0 && y <= strip_y1) {
                ps2GfxIncludeSpanX(
                    triangle->x[i], &hit, &min_x, &max_x);
            }
        }

        for (uint32_t i = 0u; i < 3u; ++i) {
            const uint32_t j = (i + 1u) % 3u;
            ps2GfxIncludeEdgeAtY(
                triangle->x[i], triangle->y[i],
                triangle->x[j], triangle->y[j],
                strip_y0, &hit, &min_x, &max_x);
            ps2GfxIncludeEdgeAtY(
                triangle->x[i], triangle->y[i],
                triangle->x[j], triangle->y[j],
                strip_y1, &hit, &min_x, &max_x);
        }

        if (!hit) {
            spans->x0[row] = 0u;
            spans->x1[row] = 0u;
            continue;
        }

        const int tile_x1 = tile->x + tile->width;
        int x0 = ps2GfxFloorToInt(min_x - 1.0f);
        int x1 = ps2GfxCeilToInt(max_x + 1.0f);
        x0 = ps2GfxMaxInt(x0, tile->x);
        x1 = ps2GfxMinInt(x1, tile_x1);
        if (x0 >= x1) {
            spans->x0[row] = 0u;
            spans->x1[row] = 0u;
            continue;
        }

        spans->x0[row] = (uint16_t)(x0 - tile->x);
        spans->x1[row] = (uint16_t)(x1 - tile->x);
    }
    return true;
}

static struct Ps2GfxSignedAlphaVertex ps2GfxInterpolateSignedAlphaVertex(
    const struct Ps2GfxSignedAlphaVertex *a,
    const struct Ps2GfxSignedAlphaVertex *b)
{
    const float denominator = a->delta - b->delta;
    const float factor = denominator != 0.0f ? a->delta / denominator : 0.0f;
    struct Ps2GfxSignedAlphaVertex result = {
        a->x + (b->x - a->x) * factor,
        a->y + (b->y - a->y) * factor,
        a->z + (b->z - a->z) * factor,
        a->s + (b->s - a->s) * factor,
        a->t + (b->t - a->t) * factor,
        a->q + (b->q - a->q) * factor,
        0.0f,
    };
    return result;
}

static bool ps2GfxSignedAlphaInside(
    const struct Ps2GfxSignedAlphaVertex *vertex, bool positive)
{
    /* Give the zero-width boundary to the positive half only. */
    return positive ? vertex->delta >= 0.0f : vertex->delta < 0.0f;
}

extern "C" bool ps2GfxClipSignedAlphaTriangle(
    const struct Ps2GfxSignedAlphaVertex triangle[3],
    bool positive,
    struct Ps2GfxSignedAlphaTriangles *result)
{
    if (!triangle || !result) {
        return false;
    }

    struct Ps2GfxSignedAlphaVertex polygon[4] = {};
    uint32_t polygon_count = 0u;
    const struct Ps2GfxSignedAlphaVertex *previous = &triangle[2];
    bool previous_inside = ps2GfxSignedAlphaInside(previous, positive);
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2GfxSignedAlphaVertex *current = &triangle[i];
        const bool current_inside =
            ps2GfxSignedAlphaInside(current, positive);
        if (current_inside != previous_inside) {
            polygon[polygon_count++] =
                ps2GfxInterpolateSignedAlphaVertex(previous, current);
        }
        if (current_inside) {
            polygon[polygon_count++] = *current;
        }
        previous = current;
        previous_inside = current_inside;
    }

    result->vertex_count = 0u;
    if (polygon_count < 3u) {
        return true;
    }

    for (uint32_t i = 1u; i + 1u < polygon_count; ++i) {
        result->vertices[result->vertex_count++] = polygon[0];
        result->vertices[result->vertex_count++] = polygon[i];
        result->vertices[result->vertex_count++] = polygon[i + 1u];
    }
    return true;
}

extern "C" struct Ps2GfxAlphaEdgeTest ps2GfxPlanSignedAlphaEdgeTest(
    uint8_t threshold, uint8_t primitive_alpha, bool positive)
{
    struct Ps2GfxAlphaEdgeTest result = {
        PS2_GFX_ALPHA_EDGE_REJECT,
        0u,
    };
    if (positive) {
        if (primitive_alpha >= threshold) {
            result.comparison = PS2_GFX_ALPHA_EDGE_ALWAYS;
        } else {
            result.comparison = PS2_GFX_ALPHA_EDGE_GEQUAL;
            result.reference = (uint8_t)(threshold - primitive_alpha);
        }
    } else if (primitive_alpha >= threshold) {
        result.comparison = PS2_GFX_ALPHA_EDGE_LEQUAL;
        result.reference = (uint8_t)(primitive_alpha - threshold);
    }
    return result;
}
