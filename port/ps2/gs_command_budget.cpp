#include "gs_command_budget.h"

#include <stdint.h>

enum Ps2GsCommandBudgetDecision ps2GsClassifyCommandReservation(
    uint32_t used_qw, uint32_t capacity_qw, uint32_t register_count)
{
    if (register_count == 0u || capacity_qw == 0u) {
        return PS2_GS_COMMAND_TOO_LARGE;
    }

    const uint64_t required_qw = (uint64_t)register_count + 1u;
    if (required_qw > capacity_qw) {
        return PS2_GS_COMMAND_TOO_LARGE;
    }

    if (used_qw <= capacity_qw &&
        required_qw <= (uint64_t)capacity_qw - used_qw) {
        return PS2_GS_COMMAND_FITS;
    }
    return PS2_GS_COMMAND_SPILL;
}
