#include <string.h>

#include "ps2_renderer_stats.h"

#define PS2_RENDERER_PERF_SAMPLE_CAPACITY 256u

struct Ps2RendererPerfRing {
    uint64_t sample[PS2_RENDERER_PERF_SAMPLE_CAPACITY];
    uint32_t count;
    uint32_t next;
};

static struct Ps2RendererStats s_stats;
static struct Ps2RendererPerfRing s_frame_times;
static struct Ps2RendererPerfRing s_renderer_build_times;
static struct Ps2RendererPerfRing s_present_wait_times;
static uint32_t s_frame_deadline_microseconds;
static bool s_skip_current_perf_frame;

static void ps2RendererStatsRecordTiming(
    struct Ps2RendererPerfRing *ring, uint64_t microseconds)
{
    ring->sample[ring->next] = microseconds;
    ring->next = (ring->next + 1u) % PS2_RENDERER_PERF_SAMPLE_CAPACITY;
    if (ring->count < PS2_RENDERER_PERF_SAMPLE_CAPACITY) {
        ++ring->count;
    }
}

static void ps2RendererStatsSummarizeTiming(
    const struct Ps2RendererPerfRing *ring,
    struct Ps2RendererTimingSummary *summary)
{
    memset(summary, 0, sizeof(*summary));
    const uint32_t count = ring->count;
    summary->sample_count = count;
    if (count == 0u) {
        return;
    }

    uint64_t sorted[PS2_RENDERER_PERF_SAMPLE_CAPACITY];
    for (uint32_t i = 0u; i < count; ++i) {
        sorted[i] = ring->sample[i];
    }
    for (uint32_t i = 1u; i < count; ++i) {
        const uint64_t value = sorted[i];
        uint32_t j = i;
        while (j != 0u && sorted[j - 1u] > value) {
            sorted[j] = sorted[j - 1u];
            --j;
        }
        sorted[j] = value;
    }

    const uint32_t p50 = ((count * 50u + 99u) / 100u) - 1u;
    const uint32_t p95 = ((count * 95u + 99u) / 100u) - 1u;
    const uint32_t p99 = ((count * 99u + 99u) / 100u) - 1u;
    summary->p50_microseconds = sorted[p50];
    summary->p95_microseconds = sorted[p95];
    summary->p99_microseconds = sorted[p99];
    summary->max_microseconds = sorted[count - 1u];
}

extern "C" void ps2RendererStatsReset(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_frame_times, 0, sizeof(s_frame_times));
    memset(&s_renderer_build_times, 0, sizeof(s_renderer_build_times));
    memset(&s_present_wait_times, 0, sizeof(s_present_wait_times));
    s_frame_deadline_microseconds = 0u;
    s_skip_current_perf_frame = false;
}

extern "C" void ps2RendererStatsPerfFrameBegin(void)
{
    s_skip_current_perf_frame = false;
}

extern "C" void ps2RendererStatsPerfSkipCurrentFrame(void)
{
    s_skip_current_perf_frame = true;
}

extern "C" void ps2RendererStatsRecordFrame(
    uint64_t microseconds, uint32_t deadline_microseconds)
{
    if (s_skip_current_perf_frame) {
        return;
    }
    s_frame_deadline_microseconds = deadline_microseconds;
    ps2RendererStatsRecordTiming(&s_frame_times, microseconds);
}

extern "C" void ps2RendererStatsRecordRendererBuild(uint64_t microseconds)
{
    if (!s_skip_current_perf_frame) {
        ps2RendererStatsRecordTiming(&s_renderer_build_times, microseconds);
    }
}

extern "C" void ps2RendererStatsRecordPresentWait(uint64_t microseconds)
{
    if (!s_skip_current_perf_frame) {
        ps2RendererStatsRecordTiming(&s_present_wait_times, microseconds);
    }
}

extern "C" void ps2RendererStatsGetPerfSummary(
    struct Ps2RendererPerfSummary *summary)
{
    if (!summary) {
        return;
    }
    memset(summary, 0, sizeof(*summary));
    ps2RendererStatsSummarizeTiming(&s_frame_times, &summary->frame);
    ps2RendererStatsSummarizeTiming(
        &s_renderer_build_times, &summary->renderer_build);
    ps2RendererStatsSummarizeTiming(
        &s_present_wait_times, &summary->present_wait);
    summary->deadline_microseconds = s_frame_deadline_microseconds;
    if (s_frame_deadline_microseconds != 0u) {
        for (uint32_t i = 0u; i < s_frame_times.count; ++i) {
            if (s_frame_times.sample[i] > s_frame_deadline_microseconds) {
                ++summary->deadline_misses;
            }
        }
    }
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

extern "C" void ps2RendererStatsRecordUnsupportedShader(
    uint32_t triangle_count)
{
    ++s_stats.unsupported_shader_batches;
    s_stats.unsupported_shader_triangles += triangle_count;
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
