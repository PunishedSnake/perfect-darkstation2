#include "gs_state_shadow.h"

#include <string.h>

static_assert(PS2_GS_STATE_COUNT <= 32,
    "state shadow valid_mask must cover every register slot");

extern "C" void ps2GsStateShadowReset(struct Ps2GsStateShadow *shadow)
{
    if (shadow) {
        memset(shadow, 0, sizeof(*shadow));
    }
}

static bool ps2GsStateSlotValid(enum Ps2GsStateSlot slot)
{
    return slot >= PS2_GS_STATE_TEST && slot < PS2_GS_STATE_COUNT;
}

extern "C" bool ps2GsStateShadowNeedsWrite(
    struct Ps2GsStateShadow *shadow, enum Ps2GsStateSlot slot,
    uint64_t value)
{
    if (!shadow || !ps2GsStateSlotValid(slot)) {
        return true;
    }

    const uint32_t bit = 1u << (uint32_t)slot;
    if ((shadow->valid_mask & bit) != 0u &&
        shadow->value[slot] == value) {
        ++shadow->suppressed_writes;
        return false;
    }
    return true;
}

extern "C" void ps2GsStateShadowCommit(
    struct Ps2GsStateShadow *shadow, enum Ps2GsStateSlot slot,
    uint64_t value)
{
    if (!shadow || !ps2GsStateSlotValid(slot)) {
        return;
    }

    shadow->value[slot] = value;
    shadow->valid_mask |= 1u << (uint32_t)slot;
    ++shadow->emitted_writes;
}

extern "C" void ps2GsStateShadowInvalidate(
    struct Ps2GsStateShadow *shadow, enum Ps2GsStateSlot slot)
{
    if (shadow && ps2GsStateSlotValid(slot)) {
        shadow->valid_mask &= ~(1u << (uint32_t)slot);
    }
}

extern "C" void ps2GsStateShadowInvalidateAll(
    struct Ps2GsStateShadow *shadow)
{
    if (shadow) {
        shadow->valid_mask = 0u;
    }
}
