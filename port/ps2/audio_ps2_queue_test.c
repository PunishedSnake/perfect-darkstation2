#include "audio_ps2_queue.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    assert(ps2AudioPlanSubmit(1024, 8192, 1472, 4096) ==
        PS2_AUDIO_SUBMIT_NOW);
    assert(ps2AudioPlanSubmit(4096, 1024, 1472, 4096) ==
        PS2_AUDIO_SUBMIT_AFTER_WAIT);

    assert(ps2AudioPlanSubmit(0, 8192, 0, 4096) ==
        PS2_AUDIO_DROP_EMPTY);
    assert(ps2AudioPlanSubmit(0, 8192, 1471, 4096) ==
        PS2_AUDIO_DROP_MISALIGNED);
    assert(ps2AudioPlanSubmit(16384, 0, 1472, 4096) ==
        PS2_AUDIO_DROP_LATENCY_LIMIT);
    assert(ps2AudioPlanSubmit(0, 4096, 8192, 4096) ==
        PS2_AUDIO_DROP_TOO_LARGE);
    assert(ps2AudioPlanSubmit(0, UINT32_MAX, 4, UINT32_MAX) ==
        PS2_AUDIO_SUBMIT_NOW);
    assert(ps2AudioPlanSubmit(0, 8192, 4, 0) ==
        PS2_AUDIO_DROP_LATENCY_LIMIT);

    return 0;
}
