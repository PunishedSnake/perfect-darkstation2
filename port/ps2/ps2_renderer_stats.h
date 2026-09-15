#ifndef PERFECT_DARK_PS2_RENDERER_STATS_H
#define PERFECT_DARK_PS2_RENDERER_STATS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Ps2RendererStats {
    uint64_t frames;
    uint64_t translation_batches;
    uint64_t translated_vertices;
    uint64_t translation_microseconds;
    uint64_t path1_color_batches;
    uint64_t path1_textured_batches;
    /* Subset of PATH1 textured work, not an additional transport total. */
    uint64_t vu1_transform_batches;
    uint64_t vu1_transform_vertices;
    uint64_t path1_vertices;
    uint64_t path1_records;
    uint64_t path3_color_batches;
    uint64_t path3_textured_batches;
    uint64_t path3_vertices;
    uint64_t path3_records;
    uint64_t unsupported_shader_batches;
    uint64_t unsupported_shader_triangles;
    uint64_t vu1_rejected_batches;
    uint64_t vu1_rejected_vertices;
    uint64_t vu1_wait_calls;
    uint64_t vu1_wait_busy_calls;
    uint64_t vu1_wait_elided_calls;
    uint64_t vu1_wait_microseconds;
    uint64_t vu1_wait_max_microseconds;
    uint64_t vu1_wait_timeouts;
    uint64_t vu1_wait_errors;
};

void ps2RendererStatsReset(void);
void ps2RendererStatsBeginFrame(void);
void ps2RendererStatsRecordTranslation(
    uint32_t vertex_count, uint64_t microseconds);
void ps2RendererStatsRecordPath1(
    bool textured, uint32_t vertex_count, uint32_t register_count);
void ps2RendererStatsRecordPath3(
    bool textured, uint32_t vertex_count, uint32_t register_count);
void ps2RendererStatsRecordUnsupportedShader(uint32_t triangle_count);
void ps2RendererStatsRecordVu1Transform(uint32_t vertex_count);
void ps2RendererStatsRecordVu1Reject(uint32_t vertex_count);
void ps2RendererStatsRecordVu1Wait(
    uint64_t microseconds, bool observed_busy);
void ps2RendererStatsRecordVu1WaitElided(void);
void ps2RendererStatsRecordVu1WaitFailure(bool timeout);
void ps2RendererStatsGet(struct Ps2RendererStats *stats);

#ifdef __cplusplus
}
#endif

#endif
