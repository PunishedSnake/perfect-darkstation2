#include "input_ps2_map.h"

#include <string.h>

static int32_t ps2InputClampAxis(int32_t value)
{
    if (value < -32768) {
        return -32768;
    }
    if (value > 32767) {
        return 32767;
    }
    return value;
}

static int32_t ps2InputScaleAxis(int32_t value, int32_t deadzone,
    float scale)
{
    if (deadzone < 0) {
        deadzone = 0;
    } else if (deadzone > 32766) {
        deadzone = 32766;
    }

    const int32_t sign = value < 0 ? -1 : 1;
    const int32_t endpoint = value < 0 ? 32768 : 32767;
    int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > endpoint) {
        magnitude = endpoint;
    }
    if (magnitude <= deadzone || scale == 0.0f) {
        return 0;
    }

    magnitude = (magnitude - deadzone) * endpoint / (endpoint - deadzone);
    const float scaled = (float)(sign * magnitude) * scale;
    return ps2InputClampAxis((int32_t)scaled);
}

static int8_t ps2InputAxisToPad(int32_t value)
{
    value = ps2InputClampAxis(value);
    if (value >= 0) {
        return (int8_t)(value / 256);
    }

    /* Preserve the full negative endpoint without overflowing signed s8. */
    const int32_t magnitude = value == -32768 ? 32768 : -value;
    return (int8_t)(-(magnitude / 256));
}

static uint32_t ps2InputMapButtons(uint32_t held)
{
    uint32_t buttons = 0u;

    /* Desktop-compatible extended Perfect Dark controller layout. */
    if (held & PS2_PAD_CROSS) {
        buttons |= A_BUTTON | CONT_0010;
    }
    if (held & PS2_PAD_CIRCLE) {
        buttons |= L_JPAD | CONT_0020;
    }
    if (held & PS2_PAD_SQUARE)   buttons |= X_BUTTON;
    if (held & PS2_PAD_TRIANGLE) buttons |= Y_BUTTON;
    if (held & PS2_PAD_L1)       buttons |= D_JPAD;
    if (held & PS2_PAD_R1)       buttons |= L_TRIG;
    if (held & PS2_PAD_L2)       buttons |= R_TRIG;
    if (held & PS2_PAD_R2)       buttons |= Z_TRIG;
    if (held & PS2_PAD_START)    buttons |= START_BUTTON;
    if (held & PS2_PAD_UP)       buttons |= U_CBUTTONS;
    if (held & PS2_PAD_DOWN)     buttons |= D_CBUTTONS;
    if (held & PS2_PAD_LEFT)     buttons |= L_CBUTTONS;
    if (held & PS2_PAD_RIGHT)    buttons |= R_CBUTTONS;
    if (held & PS2_PAD_L3)       buttons |= CONT_8000;

    return buttons;
}

bool ps2InputMapPad(const struct Ps2PadState *source,
    const struct Ps2InputMapConfig *config, OSContPad *destination)
{
    if (!source || !config || !destination) {
        return false;
    }

    memset(destination, 0, sizeof(*destination));
    if (!source->connected) {
        destination->errnum = CONT_NO_RESPONSE_ERROR;
        return false;
    }

    destination->button = ps2InputMapButtons(source->held);

    const int32_t physical[4] = {
        source->lx, source->ly, source->rx, source->ry,
    };
    int32_t mapped[4];
    for (int axis = 0; axis < 4; ++axis) {
        mapped[axis] = ps2InputScaleAxis(physical[axis],
            config->axis_deadzone[axis], config->axis_scale[axis]);
    }

    const int move = config->swap_sticks ? 2 : 0;
    const int look = config->swap_sticks ? 0 : 2;
    destination->stick_x = ps2InputAxisToPad(mapped[move]);
    destination->stick_y = ps2InputAxisToPad(mapped[move + 1]);

    if (config->dual_analog) {
        destination->rstick_x = ps2InputAxisToPad(mapped[look]);
        destination->rstick_y = ps2InputAxisToPad(mapped[look + 1]);
    } else {
        if (mapped[look] < -0x4000) destination->button |= L_CBUTTONS;
        if (mapped[look] >  0x4000) destination->button |= R_CBUTTONS;
        if (mapped[look + 1] >  0x4000) destination->button |= U_CBUTTONS;
        if (mapped[look + 1] < -0x4000) destination->button |= D_CBUTTONS;
    }

    if (config->cancel_c_buttons) {
        if ((destination->button & (L_CBUTTONS | R_CBUTTONS)) ==
                (L_CBUTTONS | R_CBUTTONS)) {
            destination->button &= ~(L_CBUTTONS | R_CBUTTONS);
        }
        if ((destination->button & (U_CBUTTONS | D_CBUTTONS)) ==
                (U_CBUTTONS | D_CBUTTONS)) {
            destination->button &= ~(U_CBUTTONS | D_CBUTTONS);
        }
    }

    destination->errnum = 0;
    return true;
}
