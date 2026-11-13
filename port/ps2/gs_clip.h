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

/*
 * Guard band: widen the X/Y planes to |X| <= guard * W for ACCEPT and for
 * the planes actually clipped, while trivial rejection still uses the exact
 * frustum. Geometry inside the band but off screen is left to the GS
 * scissor, which discards it for free. 1.0 (the default) is the exact
 * frustum. Z is never widened.
 */
void ps2GsClipSetGuardBand(float guard_x, float guard_y);

enum Ps2GsClipClass {
    /* Outside one plane or non-finite position: produces no output. */
    PS2_GS_CLIP_REJECT = 0,
    /* Fully inside: ps2GsClipTriangle() would copy it unchanged. */
    PS2_GS_CLIP_ACCEPT = 1,
    /* Crosses at least one plane: needs ps2GsClipTriangle(). */
    PS2_GS_CLIP_PARTIAL = 2,
};

/*
 * The exact classification ps2GsClipTriangle() applies before clipping.
 * Callers may draw ACCEPT triangles straight from their source buffer.
 * stride must satisfy the ps2GsClipTriangle() contract.
 */
enum Ps2GsClipClass ps2GsClipClassifyTriangle(
    const float *triangle, size_t stride);

/*
 * Per-vertex form of the classification, for callers that share vertices
 * between triangles: bits 0..5 are the exact-frustum outcodes, bits 8..13
 * the guard-band outcodes, PS2_GS_CLIP_CODE_NONFINITE marks a non-finite
 * position. ps2GsClipClassifyCodes() on three such values returns exactly
 * what ps2GsClipClassifyTriangle() returns for the triangle (under the same
 * guard band).
 */
#define PS2_GS_CLIP_CODE_NONFINITE 0x10000u
unsigned int ps2GsClipVertexCodes(const float *vertex);

static inline enum Ps2GsClipClass ps2GsClipClassifyCodes(
    unsigned int a, unsigned int b, unsigned int c)
{
    if (((a | b | c) & PS2_GS_CLIP_CODE_NONFINITE) != 0u ||
        (a & b & c & 0x3fu) != 0u) {
        return PS2_GS_CLIP_REJECT;
    }
    return ((a | b | c) & 0x3f00u) == 0u
        ? PS2_GS_CLIP_ACCEPT : PS2_GS_CLIP_PARTIAL;
}

#endif
