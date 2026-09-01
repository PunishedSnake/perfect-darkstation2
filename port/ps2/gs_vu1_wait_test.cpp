#include <assert.h>
#include <stdint.h>

#include "gs_vu1_wait.h"

int main(void)
{
    assert(ps2GsVu1ClassifyWait(0u, 0u) == PS2_GS_VU1_WAIT_IDLE);
    assert(ps2GsVu1ClassifyWait(PS2_GS_VU1_DMAC_STR_MASK, 0u) ==
        PS2_GS_VU1_WAIT_ACTIVE);

    for (uint32_t bit = 0u; bit < 32u; ++bit) {
        const uint32_t mask = 1u << bit;
        if ((mask & PS2_GS_VU1_STAT_ACTIVE_MASK) != 0u) {
            assert(ps2GsVu1ClassifyWait(0u, mask) ==
                PS2_GS_VU1_WAIT_ACTIVE);
        }
        if ((mask & PS2_GS_VU1_STAT_ERROR_MASK) != 0u) {
            assert(ps2GsVu1ClassifyWait(PS2_GS_VU1_DMAC_STR_MASK, mask) ==
                PS2_GS_VU1_WAIT_ERROR);
        }
    }

    assert(!ps2GsVu1WaitTimedOut(100u, 200u, 101u));
    assert(ps2GsVu1WaitTimedOut(100u, 201u, 101u));
    assert(!ps2GsVu1WaitTimedOut(100u, UINT64_MAX, 0u));
    assert(ps2GsVu1WaitTimedOut(200u, 100u, 1u));
    return 0;
}
