#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "log_checkpoint_policy.h"

int main(void)
{
    assert(ps2LogCheckpointShouldClose(0u, 0u, false, false));
    assert(!ps2LogCheckpointShouldClose(99999u, 0u, true, false));
    assert(ps2LogCheckpointShouldClose(100000u, 0u, true, false));
    assert(ps2LogCheckpointShouldClose(1u, 999999u, true, false));

    /* Runtime telemetry may force a durable close inside the throttle window. */
    assert(ps2LogCheckpointShouldClose(50001u, 50000u, true, true));
    return 0;
}
