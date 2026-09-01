#ifndef PERFECT_DARK_PS2_MEMORY_BUDGET_H
#define PERFECT_DARK_PS2_MEMORY_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

struct Ps2GameHeapPlan {
    uint32_t tail_bytes;
    uint32_t reserve_bytes;
    uint32_t requested_bytes;
    uint32_t planned_bytes;
};

/*
 * Plan the long-lived Perfect Dark memp arena from the still-uncommitted tail
 * of the PS2SDK EE heap. The reserve remains owned by libc/platform services,
 * including lazy ROM assets and renderer staging allocated after game start.
 */
static inline bool ps2PlanGameHeap(
    uintptr_t heap_cursor, uintptr_t heap_end,
    uint32_t requested_bytes, uint32_t reserve_bytes,
    uint32_t minimum_bytes, uint32_t alignment,
    struct Ps2GameHeapPlan *plan)
{
    if (!plan || heap_end <= heap_cursor || requested_bytes == 0u ||
        alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return false;
    }

    const uintptr_t tail_wide = heap_end - heap_cursor;
    if (tail_wide > UINT32_MAX) {
        return false;
    }

    const uint32_t tail = (uint32_t)tail_wide;
    if (tail <= reserve_bytes) {
        return false;
    }

    const uint32_t mask = alignment - 1u;
    const uint32_t usable = (tail - reserve_bytes) & ~mask;
    const uint32_t requested = requested_bytes & ~mask;
    const uint32_t planned = requested < usable ? requested : usable;
    if (planned == 0u || planned < minimum_bytes) {
        return false;
    }

    plan->tail_bytes = tail;
    plan->reserve_bytes = reserve_bytes;
    plan->requested_bytes = requested_bytes;
    plan->planned_bytes = planned;
    return true;
}

#endif
