#include "gs_clip.h"

#include <cassert>
#include <cmath>
#include <cstdio>

static unsigned int nextRandom(unsigned int *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static float randomRange(unsigned int *state, float minimum, float maximum)
{
    const float unit = (float)(nextRandom(state) & 0xffffu) / 65535.0f;
    return minimum + (maximum - minimum) * unit;
}

static bool inside(const float *vertex)
{
    const float x = vertex[0];
    const float y = vertex[1];
    const float z = vertex[2];
    const float w = vertex[3];
    const float epsilon = 0.00001f;
    return x + w >= -epsilon && w - x >= -epsilon &&
        y + w >= -epsilon && w - y >= -epsilon &&
        z >= -epsilon && w - z >= -epsilon;
}

int main()
{
    constexpr size_t stride = 6u;
    float output[PS2_GS_CLIP_MAX_OUTPUT_VERTICES * stride] = {};
    size_t count = 999u;

    const float visible[3u * stride] = {
        -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f,
         0.0f,  0.5f, 0.5f, 1.0f, 0.5f, 1.0f,
    };
    assert(ps2GsClipTriangle(visible, stride, output,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
    assert(count == 3u);
    for (size_t i = 0u; i < count * stride; ++i) {
        assert(output[i] == visible[i]);
    }

    const float outside[3u * stride] = {
        -2.0f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f,
        -2.0f,  0.5f, 0.5f, 1.0f, 0.0f, 1.0f,
        -3.0f,  0.0f, 0.5f, 1.0f, 1.0f, 0.5f,
    };
    assert(ps2GsClipTriangle(outside, stride, output,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
    assert(count == 0u);

    const float near_crossing[3u * stride] = {
        -0.5f, -0.5f,  0.5f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f,  0.5f, 1.0f, 1.0f, 0.0f,
         0.0f,  0.5f, -0.5f, 1.0f, 0.5f, 1.0f,
    };
    assert(ps2GsClipTriangle(near_crossing, stride, output,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
    assert(count == 6u);
    bool found_left_intersection = false;
    bool found_right_intersection = false;
    for (size_t vertex = 0u; vertex < count; ++vertex) {
        const float *value = &output[vertex * stride];
        assert(inside(value));
        if (std::fabs(value[2]) < 0.00001f) {
            if (std::fabs(value[4] - 0.25f) < 0.00001f &&
                std::fabs(value[5] - 0.5f) < 0.00001f) {
                found_left_intersection = true;
            }
            if (std::fabs(value[4] - 0.75f) < 0.00001f &&
                std::fabs(value[5] - 0.5f) < 0.00001f) {
                found_right_intersection = true;
            }
        }
    }
    assert(found_left_intersection);
    assert(found_right_intersection);

    const float eye_crossing[3u * stride] = {
        -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f,
         0.0f,  0.2f, 0.1f, -0.2f, 0.5f, 1.0f,
    };
    assert(ps2GsClipTriangle(eye_crossing, stride, output,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
    for (size_t vertex = 0u; vertex < count; ++vertex) {
        assert(inside(&output[vertex * stride]));
        for (size_t component = 0u; component < stride; ++component) {
            assert(std::isfinite(output[vertex * stride + component]));
        }
    }

    count = 999u;
    assert(!ps2GsClipTriangle(near_crossing, stride, output, 3u, &count));
    assert(count == 0u);

    unsigned int random_state = 0x504432u;
    for (size_t iteration = 0u; iteration < 10000u; ++iteration) {
        float triangle[3u * stride];
        for (size_t vertex = 0u; vertex < 3u; ++vertex) {
            triangle[vertex * stride + 0u] =
                randomRange(&random_state, -4.0f, 4.0f);
            triangle[vertex * stride + 1u] =
                randomRange(&random_state, -4.0f, 4.0f);
            triangle[vertex * stride + 2u] =
                randomRange(&random_state, -2.0f, 4.0f);
            triangle[vertex * stride + 3u] =
                randomRange(&random_state, -1.0f, 3.0f);
            triangle[vertex * stride + 4u] =
                randomRange(&random_state, -8.0f, 8.0f);
            triangle[vertex * stride + 5u] =
                randomRange(&random_state, -8.0f, 8.0f);
        }
        assert(ps2GsClipTriangle(triangle, stride, output,
            PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
        assert(count <= PS2_GS_CLIP_MAX_OUTPUT_VERTICES);
        assert(count % 3u == 0u);
        for (size_t vertex = 0u; vertex < count; ++vertex) {
            assert(inside(&output[vertex * stride]));
            for (size_t component = 0u; component < stride; ++component) {
                assert(std::isfinite(output[vertex * stride + component]));
            }
        }
    }

    std::puts("GS homogeneous clip tests passed");
    return 0;
}
