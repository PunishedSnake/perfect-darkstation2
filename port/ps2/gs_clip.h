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
void ps2GsClipGetGuardBand(float *guard_x, float *guard_y);

/*
 * Inline ps2GsClipVertexCodes for hot loops, with the guard band passed in
 * (from ps2GsClipGetGuardBand). Same tests and results.
 */
static inline unsigned int ps2GsClipVertexCodesInline(const float *v,
    float guard_x, float guard_y)
{
    union { float f; unsigned int u; } c[4];
    c[0].f = v[0]; c[1].f = v[1]; c[2].f = v[2]; c[3].f = v[3];
    if (((c[0].u & 0x7f800000u) == 0x7f800000u) || ((c[1].u & 0x7f800000u) == 0x7f800000u) ||
        ((c[2].u & 0x7f800000u) == 0x7f800000u) || ((c[3].u & 0x7f800000u) == 0x7f800000u)) {
        return PS2_GS_CLIP_CODE_NONFINITE;
    }
    const float x = v[0], y = v[1], z = v[2], w = v[3];
    const unsigned int zc = (z < 0.0f ? 16u : 0u) | (w - z < 0.0f ? 32u : 0u);
    const unsigned int exact = (x + w < 0.0f ? 1u : 0u) | (w - x < 0.0f ? 2u : 0u) |
        (y + w < 0.0f ? 4u : 0u) | (w - y < 0.0f ? 8u : 0u) | zc;
    if (guard_x == 1.0f && guard_y == 1.0f) {
        return exact | (exact << 8);
    }
    const float gx = guard_x * w;
    const float gy = guard_y * w;
    const unsigned int guard = (x + gx < 0.0f ? 1u : 0u) | (gx - x < 0.0f ? 2u : 0u) |
        (y + gy < 0.0f ? 4u : 0u) | (gy - y < 0.0f ? 8u : 0u) | zc;
    return exact | (guard << 8);
}

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
