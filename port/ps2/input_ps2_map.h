#ifndef PERFECT_DARK_PS2_INPUT_MAP_H
#define PERFECT_DARK_PS2_INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include <PR/os_thread.h>
#include <PR/os_cont.h>

#include "pad_ps2.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Ps2InputMapConfig {
    bool swap_sticks;
    bool dual_analog;
    bool cancel_c_buttons;
    float axis_scale[4];
    int32_t axis_deadzone[4];
};

/*
 * Translate the physical DualShock 2 state into the extended OSContPad
 * contract consumed by the portable Perfect Dark game code. The button layout
 * mirrors the desktop port's default game-controller bindings while keeping
 * left-stick movement and right-stick look native on PS2.
 */
bool ps2InputMapPad(const struct Ps2PadState *source,
    const struct Ps2InputMapConfig *config, OSContPad *destination);

#ifdef __cplusplus
}
#endif

#endif
