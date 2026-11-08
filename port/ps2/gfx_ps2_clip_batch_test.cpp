#include "gfx_ps2_clip_batch.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t kStride = 7u;
constexpr size_t kCapacity = 81u;
constexpr size_t kMinDirect = 12u;

unsigned int nextRandom(unsigned int *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

float unitRandom(unsigned int *state)
{
    return (float)(nextRandom(state) & 0xffffu) / 65535.0f;
}

/* Build a triangle of the requested class (0 reject, 1 accept, 2 partial). */
void makeTriangle(float *out, int kind, unsigned int *state, size_t index)
{
    for (size_t vertex = 0u; vertex < 3u; ++vertex) {
        float *v = &out[vertex * kStride];
        const float w = 1.0f + unitRandom(state);
        v[0] = (unitRandom(state) - 0.5f) * w;
        v[1] = (unitRandom(state) - 0.5f) * w;
        v[2] = (0.1f + 0.8f * unitRandom(state)) * w;
        v[3] = w;
        if (kind == 0) {
            v[0] = 3.0f * w + (float)vertex;
        } else if (kind == 2 && vertex == 2u) {
            v[2] = -0.5f * w;
        }
        for (size_t c = 4u; c < kStride; ++c) {
            v[c] = (float)(index * 16u + vertex * 4u + c);
        }
    }
    if (kind == 3) {
        out[kStride + 3u] = NAN;
    }
}

/* Reference: the pre-batching behaviour, triangle by triangle. */
std::vector<float> reference(const std::vector<float> &source)
{
    std::vector<float> out;
    float clipped[PS2_GS_CLIP_MAX_OUTPUT_VERTICES * kStride];
    for (size_t t = 0u; t < source.size() / (3u * kStride); ++t) {
        size_t count = 0u;
        assert(ps2GsClipTriangle(&source[t * 3u * kStride], kStride,
            clipped, PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count));
        out.insert(out.end(), clipped, clipped + count * kStride);
    }
    return out;
}

struct Result {
    std::vector<float> drawn;
    std::vector<size_t> batch_sizes;
    std::vector<bool> batch_direct;
    std::vector<size_t> visited_counts;
    size_t emitted = 0u;
};

Result run(std::vector<float> &source)
{
    Result result;
    std::vector<float> scratch(kCapacity * kStride, -12345.0f);
    const float *source_begin = source.data();
    const float *source_end = source.data() + source.size();
    const size_t triangles = source.size() / (3u * kStride);
    result.emitted = gfxPs2ClipAndBatchTriangles(
        source.data(), triangles, kStride, scratch.data(), kCapacity,
        kMinDirect,
        [&](float *vertices, size_t count, bool direct) {
            assert(count != 0u);
            assert(count % 3u == 0u);
            assert(count <= kCapacity);
            if (direct) {
                assert(vertices >= source_begin && vertices < source_end);
            } else {
                assert(vertices == scratch.data());
            }
            result.drawn.insert(result.drawn.end(), vertices,
                vertices + count * kStride);
            result.batch_sizes.push_back(count);
            result.batch_direct.push_back(direct);
        },
        [&](size_t triangle, size_t count, const float *vertices) {
            assert(triangle == result.visited_counts.size());
            assert((count == 0u) == (vertices == NULL));
            result.visited_counts.push_back(count);
        });
    assert(result.visited_counts.size() == triangles);
    return result;
}

void checkPattern(const std::vector<int> &kinds, unsigned int seed)
{
    unsigned int state = seed;
    std::vector<float> source(kinds.size() * 3u * kStride);
    for (size_t t = 0u; t < kinds.size(); ++t) {
        makeTriangle(&source[t * 3u * kStride], kinds[t], &state, t);
    }
    const std::vector<float> original = source;
    const std::vector<float> expected = reference(source);
    const Result result = run(source);

    /* The source buffer is read-only for the batcher. */
    assert(source.empty() || std::memcmp(source.data(), original.data(),
        source.size() * sizeof(float)) == 0);
    assert(result.drawn.size() == expected.size());
    for (size_t i = 0u; i < expected.size(); ++i) {
        assert(std::memcmp(&result.drawn[i], &expected[i],
            sizeof(float)) == 0);
    }
    assert(result.emitted * kStride == expected.size());

    /* Interior direct batches must be long runs. */
    for (size_t b = 0u; b + 1u < result.batch_sizes.size(); ++b) {
        if (result.batch_direct[b]) {
            assert(result.batch_sizes[b] >= kMinDirect * 3u);
        }
    }
}

} // namespace

int main()
{
    /* All accepted: direct batches of exactly the translation capacity. */
    {
        std::vector<int> kinds(100u, 1);
        unsigned int state = 1u;
        std::vector<float> source(kinds.size() * 3u * kStride);
        for (size_t t = 0u; t < kinds.size(); ++t) {
            makeTriangle(&source[t * 3u * kStride], 1, &state, t);
        }
        const Result result = run(source);
        assert(result.batch_sizes.size() == 4u);
        for (size_t b = 0u; b < 3u; ++b) {
            assert(result.batch_direct[b]);
            assert(result.batch_sizes[b] == kCapacity);
        }
        assert(result.batch_direct[3]);
        assert(result.batch_sizes[3] == (100u - 81u) * 3u);
        checkPattern(kinds, 1u);
    }

    /* Sparse rejects keep short runs merged through the scratch buffer. */
    {
        std::vector<int> kinds;
        for (size_t t = 0u; t < 120u; ++t) {
            kinds.push_back(t % 5u == 4u ? 0 : 1);
        }
        unsigned int state = 2u;
        std::vector<float> source(kinds.size() * 3u * kStride);
        for (size_t t = 0u; t < kinds.size(); ++t) {
            makeTriangle(&source[t * 3u * kStride], kinds[t], &state, t);
        }
        const Result result = run(source);
        for (size_t b = 0u; b + 1u < result.batch_sizes.size(); ++b) {
            assert(result.batch_sizes[b] >= 60u);
        }
        checkPattern(kinds, 2u);
    }

    /* Edge cases: empty, single kinds, non-finite input. */
    checkPattern({}, 3u);
    checkPattern({ 0 }, 4u);
    checkPattern({ 1 }, 5u);
    checkPattern({ 2 }, 6u);
    checkPattern({ 3, 1, 1, 3 }, 7u);

    /* Randomised mixtures of every class, including long accept runs. */
    unsigned int pattern_state = 0x5044u;
    for (size_t iteration = 0u; iteration < 2000u; ++iteration) {
        const size_t length = nextRandom(&pattern_state) % 200u;
        std::vector<int> kinds;
        while (kinds.size() < length) {
            const int kind = (int)(nextRandom(&pattern_state) % 4u);
            const size_t repeat = kind == 1
                ? 1u + nextRandom(&pattern_state) % 40u
                : 1u + nextRandom(&pattern_state) % 3u;
            for (size_t r = 0u; r < repeat && kinds.size() < length; ++r) {
                kinds.push_back(kind);
            }
        }
        checkPattern(kinds, (unsigned int)iteration + 100u);
    }

    /* Invalid configuration is refused without drawing. */
    float dummy[3u * kStride] = {};
    float scratch[kCapacity * kStride];
    bool drew = false;
    assert(gfxPs2ClipAndBatchTriangles(dummy, 1u, kStride, scratch,
        PS2_GS_CLIP_MAX_OUTPUT_VERTICES - 1u, 0u,
        [&](float *, size_t, bool) { drew = true; },
        [&](size_t, size_t, const float *) {}) == 0u);
    assert(gfxPs2ClipAndBatchTriangles(dummy, 1u, kStride, scratch,
        kCapacity, kCapacity, [&](float *, size_t, bool) { drew = true; },
        [&](size_t, size_t, const float *) {}) == 0u);
    assert(!drew);

    std::puts("PS2 clip batching tests passed");
    return 0;
}
