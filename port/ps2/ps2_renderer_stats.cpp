#include <string.h>

#include "ps2_renderer_stats.h"

static struct Ps2RendererStats s_stats;

extern "C" void ps2RendererStatsReset(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}

extern "C" void ps2RendererStatsBeginFrame(void)
{
    ++s_stats.frames;
}

extern "C" void ps2RendererStatsRecordTranslation(
    uint32_t vertex_count, uint64_t microseconds)
{
    ++s_stats.translation_batches;
    s_stats.translated_vertices += vertex_count;
    s_stats.translation_microseconds += microseconds;
}

extern "C" void ps2RendererStatsRecordPath1(
    bool textured, uint32_t vertex_count, uint32_t register_count)
{
    if (textured) {
        ++s_stats.path1_textured_batches;
    } else {
        ++s_stats.path1_color_batches;
    }
    s_stats.path1_vertices += vertex_count;
    s_stats.path1_records += register_count;
}

extern "C" void ps2RendererStatsRecordPath3(
    bool textured, uint32_t vertex_count, uint32_t register_count)
{
    if (textured) {
        ++s_stats.path3_textured_batches;
    } else {
        ++s_stats.path3_color_batches;
    }
    s_stats.path3_vertices += vertex_count;
    s_stats.path3_records += register_count;
}

extern "C" void ps2RendererStatsRecordVu1Transform(uint32_t vertex_count)
{
    ++s_stats.vu1_transform_batches;
    s_stats.vu1_transform_vertices += vertex_count;
}

extern "C" void ps2RendererStatsRecordVu1Reject(uint32_t vertex_count)
{
    ++s_stats.vu1_rejected_batches;
    s_stats.vu1_rejected_vertices += vertex_count;
}

extern "C" void ps2RendererStatsRecordVu1Wait(
    uint64_t microseconds, bool observed_busy)
{
    ++s_stats.vu1_wait_calls;
    if (observed_busy) {
        ++s_stats.vu1_wait_busy_calls;
    }
    s_stats.vu1_wait_microseconds += microseconds;
    if (microseconds > s_stats.vu1_wait_max_microseconds) {
        s_stats.vu1_wait_max_microseconds = microseconds;
    }
}

extern "C" void ps2RendererStatsRecordVu1WaitElided(void)
{
    ++s_stats.vu1_wait_elided_calls;
}

extern "C" void ps2RendererStatsRecordVu1WaitFailure(bool timeout)
{
    if (timeout) {
        ++s_stats.vu1_wait_timeouts;
    } else {
        ++s_stats.vu1_wait_errors;
    }
}

extern "C" void ps2RendererStatsGet(struct Ps2RendererStats *stats)
{
    if (stats) {
        *stats = s_stats;
    }
}
