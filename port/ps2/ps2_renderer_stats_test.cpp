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
    ps2RendererStatsRecordPath3(false, 6u, 13u);
    ps2RendererStatsRecordPath3(true, 9u, 31u);
    ps2RendererStatsRecordVu1Reject(9u);
    ps2RendererStatsGet(&stats);

    assert(stats.frames == 2u);
    assert(stats.translation_batches == 2u);
    assert(stats.translated_vertices == 84u);
    assert(stats.translation_microseconds == 252u);
    assert(stats.path1_color_batches == 1u);
    assert(stats.path1_textured_batches == 1u);
    assert(stats.path1_vertices == 84u);
    assert(stats.path1_records == 256u);
    assert(stats.path3_color_batches == 1u);
    assert(stats.path3_textured_batches == 1u);
    assert(stats.path3_vertices == 15u);
    assert(stats.path3_records == 44u);
    assert(stats.vu1_rejected_batches == 1u);
    assert(stats.vu1_rejected_vertices == 9u);

    ps2RendererStatsGet(NULL);
    return 0;
}
