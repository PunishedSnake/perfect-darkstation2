#ifndef PERFECT_DARK_PS2_GS_CLIP_H
#define PERFECT_DARK_PS2_GS_CLIP_H

#include <stdbool.h>
#include <stddef.h>

/* gfx_pc.cpp currently guarantees at most 32 floats per emitted vertex. */
#define PS2_GS_CLIP_MAX_VERTEX_FLOATS 32u

/* A triangle clipped against a six-plane homogeneous frustum has at most
 * nine polygon vertices, which triangulate to seven triangles. */
#define PS2_GS_CLIP_MAX_POLYGON_VERTICES 9u
#define PS2_GS_CLIP_MAX_OUTPUT_VERTICES \
    ((PS2_GS_CLIP_MAX_POLYGON_VERTICES - 2u) * 3u)

/*
 * Clip one triangle whose first four components are homogeneous X/Y/Z/W.
 * The PS2 Fast3D contract maps clip Z to [0, W], so the accepted volume is:
 *
 *   -W <= X <= W, -W <= Y <= W, 0 <= Z <= W
 *
 * Every remaining component is linearly interpolated at generated vertices.
 * out_vertex_count is always set. False means invalid arguments/capacity.
 */
bool ps2GsClipTriangle(const float *triangle, size_t stride,
    float *out_vertices, size_t out_capacity_vertices,
    size_t *out_vertex_count);

#endif
