#include "gs_clip.h"

#include <cmath>
#include <cstring>

namespace {

constexpr size_t kClipPlaneCount = 6u;

/* Guard-band scale for the X/Y planes (1 = exact frustum). */
float s_guard_x = 1.0f;
float s_guard_y = 1.0f;

float planeDistance(const float *vertex, size_t plane)
{
    const float x = vertex[0];
    const float y = vertex[1];
    const float z = vertex[2];
    const float w = vertex[3];

    switch (plane) {
    case 0u: return x + s_guard_x * w;
    case 1u: return s_guard_x * w - x;
    case 2u: return y + s_guard_y * w;
    case 3u: return s_guard_y * w - y;
    case 4u: return z;
    default: return w - z;
    }
}

inline unsigned int clipCode(const float *vertex)
{
    const float x = vertex[0];
    const float y = vertex[1];
    const float z = vertex[2];
    const float w = vertex[3];
    /* Same tests as planeDistance(vertex, plane) < 0 for planes 0..5. */
    return (x + w < 0.0f ? 1u : 0u) |
        (w - x < 0.0f ? 2u : 0u) |
        (y + w < 0.0f ? 4u : 0u) |
        (w - y < 0.0f ? 8u : 0u) |
        (z < 0.0f ? 16u : 0u) |
        (w - z < 0.0f ? 32u : 0u);
}

/* Same planes widened by the guard band (X/Y only). */
inline unsigned int guardCode(const float *vertex)
{
    const float x = vertex[0];
    const float y = vertex[1];
    const float z = vertex[2];
    const float w = vertex[3];
    const float gx = s_guard_x * w;
    const float gy = s_guard_y * w;
    return (x + gx < 0.0f ? 1u : 0u) |
        (gx - x < 0.0f ? 2u : 0u) |
        (y + gy < 0.0f ? 4u : 0u) |
        (gy - y < 0.0f ? 8u : 0u) |
        (z < 0.0f ? 16u : 0u) |
        (w - z < 0.0f ? 32u : 0u);
}

/*
 * Outcodes of a finite triangle; false if any position is non-finite.
 * codes[] use the exact frustum (trivial reject), guard[] the guard band
 * (accept / clip).
 */
inline bool triangleCodes(const float *triangle, size_t stride,
    unsigned int codes[3], unsigned int guard[3])
{
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        const float *v = &triangle[vertex * stride];
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) ||
            !std::isfinite(v[2]) || !std::isfinite(v[3])) {
            return false;
        }
        codes[vertex] = clipCode(v);
        guard[vertex] = s_guard_x == 1.0f && s_guard_y == 1.0f
            ? codes[vertex] : guardCode(v);
    }
    return true;
}

void copyVertex(float *destination, const float *source, size_t stride)
{
    /* stride is small (<= 32 floats); a library memcpy call costs more. */
    for (size_t component = 0u; component < stride; ++component) {
        destination[component] = source[component];
    }
}

void intersectEdge(float *destination, const float *from, const float *to,
    float from_distance, float to_distance, size_t stride)
{
    const float denominator = from_distance - to_distance;
    const float t = denominator != 0.0f
        ? from_distance / denominator
        : 0.0f;
    for (size_t component = 0u; component < stride; ++component) {
        destination[component] = from[component] +
            (to[component] - from[component]) * t;
    }
}

} // namespace

static enum Ps2GsClipClass classifyCodes(const unsigned int codes[3],
    const unsigned int guard[3])
{
    if ((codes[0] & codes[1] & codes[2]) != 0u) {
        return PS2_GS_CLIP_REJECT;
    }
    return (guard[0] | guard[1] | guard[2]) == 0u
        ? PS2_GS_CLIP_ACCEPT : PS2_GS_CLIP_PARTIAL;
}

void ps2GsClipSetGuardBand(float guard_x, float guard_y)
{
    s_guard_x = guard_x >= 1.0f ? guard_x : 1.0f;
    s_guard_y = guard_y >= 1.0f ? guard_y : 1.0f;
}

enum Ps2GsClipClass ps2GsClipClassifyTriangle(
    const float *triangle, size_t stride)
{
    unsigned int codes[3];
    unsigned int guard[3];
    if (!triangleCodes(triangle, stride, codes, guard)) {
        return PS2_GS_CLIP_REJECT;
    }
    return classifyCodes(codes, guard);
}

bool ps2GsClipTriangle(const float *triangle, size_t stride,
    float *out_vertices, size_t out_capacity_vertices,
    size_t *out_vertex_count)
{
    if (out_vertex_count) {
        *out_vertex_count = 0u;
    }
    if (!triangle || !out_vertices || !out_vertex_count || stride < 4u ||
        stride > PS2_GS_CLIP_MAX_VERTEX_FLOATS) {
        return false;
    }

    unsigned int codes[3];
    unsigned int guard[3];
    const enum Ps2GsClipClass clip_class =
        triangleCodes(triangle, stride, codes, guard)
            ? classifyCodes(codes, guard) : PS2_GS_CLIP_REJECT;
    if (clip_class == PS2_GS_CLIP_REJECT) {
        return true;
    }
    if (clip_class == PS2_GS_CLIP_ACCEPT) {
        if (out_capacity_vertices < 3u) {
            return false;
        }
        std::memcpy(out_vertices, triangle, 3u * stride * sizeof(float));
        *out_vertex_count = 3u;
        return true;
    }

    /*
     * Sutherland-Hodgman over vertex pointers: surviving vertices are never
     * copied between planes, only new intersection points get storage.
     * Planes that no input vertex violates are skipped; every clipped vertex
     * is a convex combination of the inputs, so it cannot violate them
     * either. The result is identical to clipping against all six planes.
     */
    const unsigned int crossed = guard[0] | guard[1] | guard[2];
    constexpr size_t kPoolVertices = 2u * kClipPlaneCount;
    float pool[kPoolVertices][PS2_GS_CLIP_MAX_VERTEX_FLOATS];
    size_t pool_used = 0u;
    const float *polygon_a[PS2_GS_CLIP_MAX_POLYGON_VERTICES];
    const float *polygon_b[PS2_GS_CLIP_MAX_POLYGON_VERTICES];
    const float **input = polygon_a;
    const float **output = polygon_b;
    size_t input_count = 3u;
    input[0] = &triangle[0u * stride];
    input[1] = &triangle[1u * stride];
    input[2] = &triangle[2u * stride];

    for (size_t plane = 0u; plane < kClipPlaneCount; ++plane) {
        if (input_count == 0u) {
            break;
        }
        if ((crossed & (1u << plane)) == 0u) {
            continue;
        }

        size_t output_count = 0u;
        const float *previous = input[input_count - 1u];
        float previous_distance = planeDistance(previous, plane);
        bool previous_inside = previous_distance >= 0.0f;

        for (size_t vertex = 0u; vertex < input_count; ++vertex) {
            const float *current = input[vertex];
            const float current_distance = planeDistance(current, plane);
            const bool current_inside = current_distance >= 0.0f;

            if (previous_inside != current_inside) {
                if (output_count >= PS2_GS_CLIP_MAX_POLYGON_VERTICES ||
                    pool_used >= kPoolVertices) {
                    return false;
                }
                float *point = pool[pool_used++];
                intersectEdge(point, previous, current,
                    previous_distance, current_distance, stride);
                output[output_count++] = point;
            }
            if (current_inside) {
                if (output_count >= PS2_GS_CLIP_MAX_POLYGON_VERTICES) {
                    return false;
                }
                output[output_count++] = current;
            }

            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }

        input_count = output_count;
        const float **swap = input;
        input = output;
        output = swap;
    }

    if (input_count < 3u) {
        return true;
    }

    const size_t required_vertices = (input_count - 2u) * 3u;
    if (required_vertices > out_capacity_vertices) {
        return false;
    }

    size_t written = 0u;
    for (size_t vertex = 1u; vertex + 1u < input_count; ++vertex) {
        copyVertex(&out_vertices[written++ * stride], input[0], stride);
        copyVertex(&out_vertices[written++ * stride], input[vertex], stride);
        copyVertex(&out_vertices[written++ * stride], input[vertex + 1u], stride);
    }
    *out_vertex_count = written;
    return true;
}
