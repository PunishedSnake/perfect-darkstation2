#include "gs_clip.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>

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

    assert(ps2GsClipClassifyTriangle(visible, stride) == PS2_GS_CLIP_ACCEPT);
    assert(ps2GsClipClassifyTriangle(outside, stride) == PS2_GS_CLIP_REJECT);
    assert(ps2GsClipClassifyTriangle(near_crossing, stride) ==
        PS2_GS_CLIP_PARTIAL);
    assert(ps2GsClipClassifyTriangle(eye_crossing, stride) ==
        PS2_GS_CLIP_PARTIAL);
    float non_finite[3u * stride];
    for (size_t i = 0u; i < 3u * stride; ++i) {
        non_finite[i] = visible[i];
    }
    non_finite[stride + 3u] = INFINITY;
    assert(ps2GsClipClassifyTriangle(non_finite, stride) ==
        PS2_GS_CLIP_REJECT);
    non_finite[stride + 3u] = NAN;
    assert(ps2GsClipClassifyTriangle(non_finite, stride) ==
        PS2_GS_CLIP_REJECT);
    assert(ps2GsClipTriangle(non_finite, stride, output,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
    assert(count == 0u);

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
        /* Classification must predict the clipper's trivial outcomes. */
        const enum Ps2GsClipClass clip_class =
            ps2GsClipClassifyTriangle(triangle, stride);
        if (clip_class == PS2_GS_CLIP_REJECT) {
            assert(count == 0u);
        } else if (clip_class == PS2_GS_CLIP_ACCEPT) {
            assert(count == 3u);
            for (size_t i = 0u; i < 3u * stride; ++i) {
                assert(output[i] == triangle[i]);
            }
        }
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
    /* Per-vertex codes classify exactly like the whole-triangle test, for
     * the exact frustum and a widened guard band, non-finite input included. */
    {
        uint32_t rng = 0x9e3779b9u;
        const auto next = [&rng]() {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return rng;
        };
        const float specials[] = { INFINITY, -INFINITY, NAN, 0.0f, -0.0f, 1e-30f };
        for (int band = 0; band < 2; ++band) {
            ps2GsClipSetGuardBand(band ? 3.5f : 1.0f, band ? 2.25f : 1.0f);
            for (int it = 0; it < 200000; ++it) {
                float tri[3u * 4u];
                for (float &v : tri) {
                    const uint32_t r = next();
                    v = (r & 0xffu) == 0u ? specials[(r >> 8) % 6u]
                        : ((float)(int32_t)r / 2147483648.0f) * ((r & 0x100u) ? 6.0f : 1.5f);
                }
                const unsigned a = ps2GsClipVertexCodes(&tri[0]);
                const unsigned b = ps2GsClipVertexCodes(&tri[4]);
                const unsigned c = ps2GsClipVertexCodes(&tri[8]);
                assert(ps2GsClipClassifyCodes(a, b, c) ==
                    ps2GsClipClassifyTriangle(tri, 4u));
            }
        }
        ps2GsClipSetGuardBand(1.0f, 1.0f);
    }

    return 0;
}
