#include "audio_ps2_queue.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int main(void)
{
    uint32_t queued = UINT32_MAX;
    uint32_t available = UINT32_MAX;

    assert(ps2AudioDeriveQueued(16384, 12288, &queued, &available));
    assert(queued == 4096);
    assert(available == 12288);
    assert(ps2AudioDeriveQueued(16384, 0, &queued, &available));
    assert(queued == 16384);
    assert(available == 0);
    assert(!ps2AudioDeriveQueued(0, 0, &queued, &available));
    assert(!ps2AudioDeriveQueued(16384, -1, &queued, &available));
    assert(!ps2AudioDeriveQueued(16384, 16385, &queued, &available));
    assert(!ps2AudioDeriveQueued(16384, 0, NULL, &available));
    assert(!ps2AudioDeriveQueued(16384, 0, &queued, NULL));

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

    /* Full speed: one or zero ticks are never altered. */
    assert(ps2AudioFrameBudget(0, 0, 368, 4096, 4) == 0);
    assert(ps2AudioFrameBudget(-3, 0, 368, 4096, 4) == 0);
    assert(ps2AudioFrameBudget(1, 9000, 368, 4096, 4) == 1);
    /* Normal 30/20 fps catch-up fits. */
    assert(ps2AudioFrameBudget(2, 1000, 368, 4096, 4) == 2);
    assert(ps2AudioFrameBudget(3, 0, 368, 4096, 4) == 3);
    /* Death-spiral frame is capped by the per-tick maximum. */
    assert(ps2AudioFrameBudget(39, 0, 368, 4096, 4) == 4);
    /* ...and by the ring space, rounding the last partial frame up. */
    assert(ps2AudioFrameBudget(39, 3500, 368, 4096, 4) == 2);
    assert(ps2AudioFrameBudget(39, 4096, 368, 4096, 4) == 1);
    assert(ps2AudioFrameBudget(39, 5000, 368, 4096, 4) == 1);
    /* No per-tick cap: only the ring bounds the work. */
    assert(ps2AudioFrameBudget(39, 0, 368, 4096, 0) == 12);

    return 0;
}
