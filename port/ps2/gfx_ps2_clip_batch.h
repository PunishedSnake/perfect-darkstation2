#ifndef PERFECT_DARK_PS2_GFX_CLIP_BATCH_H
#define PERFECT_DARK_PS2_GFX_CLIP_BATCH_H

#include <stddef.h>
#include <string.h>

#include "gs_clip.h"

/*
 * Clip a Fast3D triangle list and hand it to the translator in batches.
 *
 * CURRENT IMPLEMENTATION contract (host-tested by gfx_ps2_clip_batch_test):
 *
 * - The concatenation of every drawn batch equals, triangle by triangle and
 *   in source order, the output of ps2GsClipTriangle() for that triangle.
 * - Every batch holds a multiple of three vertices and at most
 *   `capacity_vertices` vertices.
 * - Runs of at least `min_direct_tris` fully-inside triangles that start
 *   while the scratch buffer is empty are drawn straight from `source`
 *   (`direct == true`) instead of being copied into `scratch` first. Shorter
 *   runs are copied and merged with their neighbours exactly like clipped
 *   output, so sparse accept/reject/clip patterns keep large batches.
 *
 * draw(float *vertices, size_t vertex_count, bool direct) submits one batch.
 * visit(size_t triangle, size_t vertex_count, const float *vertices) reports
 * each source triangle's output (vertex_count 0 and NULL when it emits
 * nothing) for diagnostics; `vertices` stays valid only during the call.
 *
 * Returns the number of vertices emitted.
 */
template <typename Draw, typename Visit>
static inline size_t gfxPs2ClipAndBatchTriangles(
    float *source, size_t triangle_count, size_t stride,
    float *scratch, size_t capacity_vertices, size_t min_direct_tris,
    Draw &&draw, Visit &&visit)
{
    if (!source || !scratch || stride < 4u ||
        stride > PS2_GS_CLIP_MAX_VERTEX_FLOATS ||
        capacity_vertices < PS2_GS_CLIP_MAX_OUTPUT_VERTICES ||
        min_direct_tris * 3u >
            capacity_vertices - PS2_GS_CLIP_MAX_OUTPUT_VERTICES) {
        return 0u;
    }

    const size_t max_direct_tris = capacity_vertices / 3u;
    size_t buffered = 0u;
    size_t emitted = 0u;
    size_t run_first = 0u;
    size_t run_tris = 0u;

    /* A run is only ever open while `buffered` is zero. */
    const auto flush_run = [&](bool allow_copy) {
        if (run_tris == 0u) {
            return;
        }
        float *run = &source[run_first * 3u * stride];
        if (allow_copy && run_tris < min_direct_tris) {
            memcpy(scratch, run, run_tris * 3u * stride * sizeof(float));
            buffered = run_tris * 3u;
        } else {
            draw(run, run_tris * 3u, true);
        }
        run_tris = 0u;
    };

    for (size_t triangle = 0u; triangle < triangle_count; ++triangle) {
        float *input = &source[triangle * 3u * stride];
        const enum Ps2GsClipClass clip_class =
            ps2GsClipClassifyTriangle(input, stride);

        if (clip_class == PS2_GS_CLIP_ACCEPT && buffered == 0u) {
            if (run_tris == 0u) {
                run_first = triangle;
            }
            ++run_tris;
            emitted += 3u;
            visit(triangle, (size_t)3u, (const float *)input);
            if (run_tris == max_direct_tris) {
                flush_run(false);
            }
            continue;
        }

        if (clip_class == PS2_GS_CLIP_REJECT) {
            /* Emits nothing, but breaks the contiguous source run. */
            flush_run(true);
            visit(triangle, (size_t)0u, (const float *)NULL);
            continue;
        }

        flush_run(true);
        if (capacity_vertices - buffered < PS2_GS_CLIP_MAX_OUTPUT_VERTICES) {
            draw(scratch, buffered, false);
            buffered = 0u;
        }

        size_t clipped = 0u;
        if (!ps2GsClipTriangle(input, stride, &scratch[buffered * stride],
                capacity_vertices - buffered, &clipped) ||
            clipped == 0u) {
            visit(triangle, (size_t)0u, (const float *)NULL);
            continue;
        }
        visit(triangle, clipped, (const float *)&scratch[buffered * stride]);
        buffered += clipped;
        emitted += clipped;
    }

    flush_run(false);
    if (buffered != 0u) {
        draw(scratch, buffered, false);
    }
    return emitted;
}

#endif
