#ifndef PERFECT_DARK_PS2_GS_COMMAND_BUDGET_H
#define PERFECT_DARK_PS2_GS_COMMAND_BUDGET_H

#include <stdint.h>

enum Ps2GsCommandBudgetDecision {
    PS2_GS_COMMAND_FITS,
    PS2_GS_COMMAND_SPILL,
    PS2_GS_COMMAND_TOO_LARGE,
};

/*
 * A PACKED A+D reservation consumes one GIF tag QW plus one QW per register.
 * Keep the boundary arithmetic host-testable and independent of PS2SDK.
 */
enum Ps2GsCommandBudgetDecision ps2GsClassifyCommandReservation(
    uint32_t used_qw, uint32_t capacity_qw, uint32_t register_count);

#endif
