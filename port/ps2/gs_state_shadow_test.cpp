#include "gs_state_shadow.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct Ps2GsStateShadow shadow;
    ps2GsStateShadowReset(&shadow);

    assert(ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_TEST, 0x1234u));
    assert(shadow.emitted_writes == 0u);
    ps2GsStateShadowCommit(&shadow, PS2_GS_STATE_TEST, 0x1234u);
    assert(shadow.emitted_writes == 1u);
    assert(!ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_TEST, 0x1234u));
    assert(shadow.suppressed_writes == 1u);
    assert(ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_TEST, 0x5678u));

    ps2GsStateShadowCommit(&shadow, PS2_GS_STATE_FRAME, 0xa5a5u);
    assert(!ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_FRAME, 0xa5a5u));
    ps2GsStateShadowInvalidate(&shadow, PS2_GS_STATE_TEST);
    assert(ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_TEST, 0x1234u));
    assert(!ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_FRAME, 0xa5a5u));

    const uint32_t emitted = shadow.emitted_writes;
    const uint32_t suppressed = shadow.suppressed_writes;
    ps2GsStateShadowInvalidateAll(&shadow);
    assert(shadow.valid_mask == 0u);
    assert(shadow.emitted_writes == emitted);
    assert(shadow.suppressed_writes == suppressed);
    assert(ps2GsStateShadowNeedsWrite(
        &shadow, PS2_GS_STATE_FRAME, 0xa5a5u));

    /* Invalid callers remain conservative and never poison a valid slot. */
    assert(ps2GsStateShadowNeedsWrite(&shadow,
        (enum Ps2GsStateSlot)PS2_GS_STATE_COUNT, 0u));
    ps2GsStateShadowCommit(&shadow,
        (enum Ps2GsStateSlot)PS2_GS_STATE_COUNT, 0u);
    assert(shadow.valid_mask == 0u);

    puts("gs_state_shadow tests passed");
    return 0;
}
