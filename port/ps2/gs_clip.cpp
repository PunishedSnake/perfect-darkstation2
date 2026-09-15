#include "gs_clip.h"

#include <cmath>
#include <cstring>

namespace {

constexpr size_t kClipPlaneCount = 6u;

float planeDistance(const float *vertex, size_t plane)
{
    const float x = vertex[0];
    const float y = vertex[1];
    const float z = vertex[2];
    const float w = vertex[3];

    switch (plane) {
    case 0u: return x + w;
    case 1u: return w - x;
    case 2u: return y + w;
    case 3u: return w - y;
    case 4u: return z;
    default: return w - z;
    }
}

unsigned int clipCode(const float *vertex)
{
    unsigned int code = 0u;
    for (size_t plane = 0u; plane < kClipPlaneCount; ++plane) {
        if (planeDistance(vertex, plane) < 0.0f) {
            code |= 1u << plane;
        }
    }
    return code;
}

void copyVertex(float *destination, const float *source, size_t stride)
{
    std::memcpy(destination, source, stride * sizeof(float));
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

    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        for (size_t component = 0u; component < 4u; ++component) {
            if (!std::isfinite(triangle[vertex * stride + component])) {
                return true;
            }
        }
    }

    const unsigned int code0 = clipCode(&triangle[0u * stride]);
    const unsigned int code1 = clipCode(&triangle[1u * stride]);
    const unsigned int code2 = clipCode(&triangle[2u * stride]);
    if ((code0 & code1 & code2) != 0u) {
        return true;
    }
    if ((code0 | code1 | code2) == 0u) {
        if (out_capacity_vertices < 3u) {
            return false;
        }
        std::memcpy(out_vertices, triangle, 3u * stride * sizeof(float));
        *out_vertex_count = 3u;
        return true;
    }

    float polygon_a[PS2_GS_CLIP_MAX_POLYGON_VERTICES]
        [PS2_GS_CLIP_MAX_VERTEX_FLOATS] = {};
    float polygon_b[PS2_GS_CLIP_MAX_POLYGON_VERTICES]
        [PS2_GS_CLIP_MAX_VERTEX_FLOATS] = {};
    float (*input)[PS2_GS_CLIP_MAX_VERTEX_FLOATS] = polygon_a;
    float (*output)[PS2_GS_CLIP_MAX_VERTEX_FLOATS] = polygon_b;
    size_t input_count = 3u;

    for (size_t vertex = 0u; vertex < input_count; ++vertex) {
        copyVertex(input[vertex], &triangle[vertex * stride], stride);
    }

    for (size_t plane = 0u; plane < kClipPlaneCount; ++plane) {
        if (input_count == 0u) {
            break;
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
                if (output_count >= PS2_GS_CLIP_MAX_POLYGON_VERTICES) {
                    return false;
                }
                intersectEdge(output[output_count++], previous, current,
                    previous_distance, current_distance, stride);
            }
            if (current_inside) {
                if (output_count >= PS2_GS_CLIP_MAX_POLYGON_VERTICES) {
                    return false;
                }
                copyVertex(output[output_count++], current, stride);
            }

            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }

        input_count = output_count;
        float (*swap)[PS2_GS_CLIP_MAX_VERTEX_FLOATS] = input;
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
