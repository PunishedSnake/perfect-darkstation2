#include <assert.h>
#include <string.h>

#include "ps2_renderer_stats.h"

int main(void)
{
    struct Ps2RendererStats stats;
    memset(&stats, 0xff, sizeof(stats));
    ps2RendererStatsReset();
    ps2RendererStatsGet(&stats);
    const struct Ps2RendererStats empty = {};
    assert(memcmp(&stats, &empty, sizeof(stats)) == 0);

    ps2RendererStatsBeginFrame();
    ps2RendererStatsBeginFrame();
    ps2RendererStatsRecordTranslation(81u, 240u);
    ps2RendererStatsRecordTranslation(3u, 12u);
    ps2RendererStatsRecordPath1(false, 3u, 7u);
    ps2RendererStatsRecordPath1(true, 81u, 249u);
    ps2RendererStatsRecordVu1Transform(81u);
    ps2RendererStatsRecordPath3(false, 6u, 13u);
    ps2RendererStatsRecordPath3(true, 9u, 31u);
    ps2RendererStatsRecordUnsupportedShader(12u);
    ps2RendererStatsRecordUnsupportedShader(3u);
    ps2RendererStatsRecordVu1Reject(9u);
    ps2RendererStatsRecordVu1Wait(7u, true);
    ps2RendererStatsRecordVu1Wait(3u, false);
    ps2RendererStatsRecordVu1WaitElided();
    ps2RendererStatsRecordVu1WaitElided();
    ps2RendererStatsRecordVu1WaitFailure(true);
    ps2RendererStatsRecordVu1WaitFailure(false);
    ps2RendererStatsRecordVu1WaitFailure(false);
    ps2RendererStatsGet(&stats);

    assert(stats.frames == 2u);
    assert(stats.translation_batches == 2u);
    assert(stats.translated_vertices == 84u);
    assert(stats.translation_microseconds == 252u);
    assert(stats.path1_color_batches == 1u);
    assert(stats.path1_textured_batches == 1u);
    assert(stats.vu1_transform_batches == 1u);
    assert(stats.vu1_transform_vertices == 81u);
    assert(stats.path1_vertices == 84u);
    assert(stats.path1_records == 256u);
    assert(stats.path3_color_batches == 1u);
    assert(stats.path3_textured_batches == 1u);
    assert(stats.path3_vertices == 15u);
    assert(stats.path3_records == 44u);
    assert(stats.unsupported_shader_batches == 2u);
    assert(stats.unsupported_shader_triangles == 15u);
    assert(stats.vu1_rejected_batches == 1u);
    assert(stats.vu1_rejected_vertices == 9u);
    assert(stats.vu1_wait_calls == 2u);
    assert(stats.vu1_wait_busy_calls == 1u);
    assert(stats.vu1_wait_elided_calls == 2u);
    assert(stats.vu1_wait_microseconds == 10u);
    assert(stats.vu1_wait_max_microseconds == 7u);
    assert(stats.vu1_wait_timeouts == 1u);
    assert(stats.vu1_wait_errors == 2u);

    ps2RendererStatsGet(NULL);
    return 0;
}
