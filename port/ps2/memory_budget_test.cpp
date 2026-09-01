#include <assert.h>
#include <stdint.h>

#include "memory_budget.h"

int main(void)
{
    struct Ps2GameHeapPlan plan = {};
    const uint32_t mib = 1024u * 1024u;

    assert(ps2PlanGameHeap(
        4u * mib, 28u * mib, 16u * mib, 4u * mib, 8u * mib, 64u,
        &plan));
    assert(plan.tail_bytes == 24u * mib);
    assert(plan.reserve_bytes == 4u * mib);
    assert(plan.requested_bytes == 16u * mib);
    assert(plan.planned_bytes == 16u * mib);

    assert(ps2PlanGameHeap(
        8u * mib, 22u * mib, 16u * mib, 4u * mib, 8u * mib, 64u,
        &plan));
    assert(plan.tail_bytes == 14u * mib);
    assert(plan.planned_bytes == 10u * mib);

    assert(ps2PlanGameHeap(
        0x1003u, 0x9003u, 0x4001u, 0x1000u, 0x2000u, 64u,
        &plan));
    assert(plan.planned_bytes == 0x4000u);

    /* A deliberate 4 MiB game configuration remains a supported floor. */
    assert(ps2PlanGameHeap(
        4u * mib, 13u * mib, 4u * mib, 4u * mib, 4u * mib, 64u,
        &plan));
    assert(plan.planned_bytes == 4u * mib);

    assert(!ps2PlanGameHeap(0x2000u, 0x2000u,
        16u * mib, 4u * mib, 8u * mib, 64u, &plan));
    assert(!ps2PlanGameHeap(0x1000u, 0x2000u,
        16u * mib, 4u * mib, 8u * mib, 64u, &plan));
    assert(!ps2PlanGameHeap(0x1000u, 7u * mib,
        16u * mib, 4u * mib, 8u * mib, 64u, &plan));
    assert(!ps2PlanGameHeap(0x1000u, 24u * mib,
        16u * mib, 4u * mib, 8u * mib, 48u, &plan));
    assert(!ps2PlanGameHeap(0x1000u, 24u * mib,
        0u, 4u * mib, 8u * mib, 64u, &plan));
    assert(!ps2PlanGameHeap(0u,
        (uintptr_t)UINT32_MAX + 1u, 16u * mib, 4u * mib, 8u * mib, 64u,
        &plan));
    assert(!ps2PlanGameHeap(0x1000u, 24u * mib,
        16u * mib, 4u * mib, 8u * mib, 64u, nullptr));
    return 0;
}
