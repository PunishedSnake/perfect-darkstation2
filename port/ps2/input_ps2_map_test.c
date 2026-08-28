#include <assert.h>
#include <string.h>

#include "input_ps2_map.h"

static struct Ps2InputMapConfig defaultConfig(void)
{
    const struct Ps2InputMapConfig config = {
        false,
        true,
        false,
        { 1.0f, 1.0f, 1.0f, 1.0f },
        { 6000, 6000, 4500, 4500 },
    };
    return config;
}

static void test_disconnected_pad(void)
{
    const struct Ps2PadState source = { 0 };
    const struct Ps2InputMapConfig config = defaultConfig();
    OSContPad destination;
    memset(&destination, 0xff, sizeof(destination));

    assert(!ps2InputMapPad(&source, &config, &destination));
    assert(destination.button == 0u);
    assert(destination.stick_x == 0);
    assert(destination.rstick_y == 0);
    assert(destination.errnum == CONT_NO_RESPONSE_ERROR);
}

static void test_default_button_contract(void)
{
    struct Ps2PadState source = { 0 };
    source.connected = true;
    source.held = PS2_PAD_CROSS | PS2_PAD_CIRCLE | PS2_PAD_SQUARE |
        PS2_PAD_TRIANGLE | PS2_PAD_L1 | PS2_PAD_R1 | PS2_PAD_L2 |
        PS2_PAD_R2 | PS2_PAD_START | PS2_PAD_UP | PS2_PAD_DOWN |
        PS2_PAD_LEFT | PS2_PAD_RIGHT | PS2_PAD_L3;

    const struct Ps2InputMapConfig config = defaultConfig();
    OSContPad destination;
    assert(ps2InputMapPad(&source, &config, &destination));

    const uint32_t expected = A_BUTTON | CONT_0010 |
        L_JPAD | CONT_0020 | X_BUTTON | Y_BUTTON | D_JPAD | L_TRIG |
        R_TRIG | Z_TRIG | START_BUTTON | U_CBUTTONS | D_CBUTTONS |
        L_CBUTTONS | R_CBUTTONS | CONT_8000;
    assert(destination.button == expected);
}

static void test_axes_deadzone_and_swap(void)
{
    struct Ps2PadState source = { 0 };
    source.connected = true;
    source.lx = 5000;
    source.ly = 32767;
    source.rx = -32768;
    source.ry = -5000;

    struct Ps2InputMapConfig config = defaultConfig();
    OSContPad destination;
    assert(ps2InputMapPad(&source, &config, &destination));
    assert(destination.stick_x == 0);
    assert(destination.stick_y == 127);
    assert(destination.rstick_x == -128);
    assert(destination.rstick_y < 0);

    config.swap_sticks = true;
    assert(ps2InputMapPad(&source, &config, &destination));
    assert(destination.stick_x == -128);
    assert(destination.stick_y < 0);
    assert(destination.rstick_x == 0);
    assert(destination.rstick_y == 127);
}

static void test_c_button_emulation_and_cancellation(void)
{
    struct Ps2PadState source = { 0 };
    source.connected = true;
    source.held = PS2_PAD_LEFT | PS2_PAD_RIGHT;
    source.rx = 32767;
    source.ry = 32767;

    struct Ps2InputMapConfig config = defaultConfig();
    config.dual_analog = false;
    config.cancel_c_buttons = true;

    OSContPad destination;
    assert(ps2InputMapPad(&source, &config, &destination));
    assert((destination.button & L_CBUTTONS) == 0u);
    assert((destination.button & R_CBUTTONS) == 0u);
    assert((destination.button & U_CBUTTONS) != 0u);
    assert(destination.rstick_x == 0);
    assert(destination.rstick_y == 0);
}

int main(void)
{
    test_disconnected_pad();
    test_default_button_contract();
    test_axes_deadzone_and_swap();
    test_c_button_emulation_and_cancellation();
    return 0;
}
