#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "controls_ps2.h"
#include "pad_ps2.h"

#define PS2_MOVE_DEADZONE 6000
#define PS2_LOOK_DEADZONE 4500

static struct Ps2ShooterControls s_controls[PS2_PAD_MAX_PLAYERS];

static float ps2ControlsNormalizeAxis(int16_t value, int deadzone)
{
    int v = value;
    int sign = 1;
    if (v < 0) {
        sign = -1;
        v = -v;
    }

    if (v <= deadzone) {
        return 0.0f;
    }
    if (v > 32767) {
        v = 32767;
    }

    const float scaled = (float)(v - deadzone) / (float)(32767 - deadzone);
    return sign < 0 ? -scaled : scaled;
}

static uint32_t ps2ControlsMapButtons(uint32_t buttons)
{
    uint32_t actions = 0;

    if (buttons & PS2_PAD_R1)       actions |= PS2_ACTION_FIRE;
    if (buttons & PS2_PAD_L1)       actions |= PS2_ACTION_AIM;
    if (buttons & PS2_PAD_R2)       actions |= PS2_ACTION_ALT_FIRE;
    if (buttons & PS2_PAD_L2)       actions |= PS2_ACTION_QUICK_GADGET;
    if (buttons & PS2_PAD_CROSS)    actions |= PS2_ACTION_USE;
    if (buttons & PS2_PAD_SQUARE)   actions |= PS2_ACTION_RELOAD;
    if (buttons & PS2_PAD_TRIANGLE) actions |= PS2_ACTION_NEXT_WEAPON;
    if (buttons & PS2_PAD_CIRCLE)   actions |= PS2_ACTION_CROUCH;
    if (buttons & PS2_PAD_L3)       actions |= PS2_ACTION_SPRINT;
    if (buttons & PS2_PAD_R3)       actions |= PS2_ACTION_ZOOM;
    if (buttons & PS2_PAD_LEFT)     actions |= PS2_ACTION_PREV_WEAPON;
    if (buttons & PS2_PAD_RIGHT)    actions |= PS2_ACTION_NEXT_WEAPON;
    if (buttons & PS2_PAD_UP)       actions |= PS2_ACTION_WEAPON_MENU;
    if (buttons & PS2_PAD_DOWN)     actions |= PS2_ACTION_QUICK_GADGET;
    if (buttons & PS2_PAD_START)    actions |= PS2_ACTION_PAUSE;
    if (buttons & PS2_PAD_SELECT)   actions |= PS2_ACTION_DEBUG;

    return actions;
}

bool ps2ShooterControlsInit(void)
{
    memset(s_controls, 0, sizeof(s_controls));
    return ps2PadInit();
}

void ps2ShooterControlsUpdate(void)
{
    ps2PadUpdate();

    for (int player = 0; player < PS2_PAD_MAX_PLAYERS; ++player) {
        const struct Ps2PadState *pad = ps2PadGetState(player);
        struct Ps2ShooterControls *out = &s_controls[player];

        if (!pad || !pad->connected) {
            memset(out, 0, sizeof(*out));
            continue;
        }

        out->connected = true;
        out->move_x = ps2ControlsNormalizeAxis(pad->lx, PS2_MOVE_DEADZONE);
        out->move_y = ps2ControlsNormalizeAxis(pad->ly, PS2_MOVE_DEADZONE);
        out->look_x = ps2ControlsNormalizeAxis(pad->rx, PS2_LOOK_DEADZONE);
        out->look_y = ps2ControlsNormalizeAxis(pad->ry, PS2_LOOK_DEADZONE);
        out->held = ps2ControlsMapButtons(pad->held);
        out->pressed = ps2ControlsMapButtons(pad->pressed);
        out->released = ps2ControlsMapButtons(pad->released);
    }
}

const struct Ps2ShooterControls *ps2ShooterControlsGet(int player)
{
    if (player < 0 || player >= PS2_PAD_MAX_PLAYERS) {
        return NULL;
    }
    return &s_controls[player];
}
