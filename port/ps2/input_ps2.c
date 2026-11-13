#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include <PR/ultratypes.h>

#include "config.h"
#include "input.h"
#include "platform.h"
#include "system.h"
#include "data.h"

#include "input_ps2_map.h"
#include "pad_ps2.h"

#define PS2_INPUT_PHYSICAL_PORTS PS2_PAD_MAX_PLAYERS
#define PS2_INPUT_DEFAULT_MOVE_DEADZONE 6000
#define PS2_INPUT_DEFAULT_LOOK_DEADZONE 4500

static s32 s_assigned_port[INPUT_MAX_CONTROLLERS] = { 0, 1, -1, -1 };
static s32 s_swap_sticks[INPUT_MAX_CONTROLLERS];
static s32 s_dual_analog[INPUT_MAX_CONTROLLERS] = { 1, 1, 1, 1 };
static s32 s_cancel_c_buttons[INPUT_MAX_CONTROLLERS];
static f32 s_axis_scale[INPUT_MAX_CONTROLLERS][4] = {
    { 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
};
static s32 s_axis_deadzone[INPUT_MAX_CONTROLLERS][4] = {
    { PS2_INPUT_DEFAULT_MOVE_DEADZONE, PS2_INPUT_DEFAULT_MOVE_DEADZONE,
      PS2_INPUT_DEFAULT_LOOK_DEADZONE, PS2_INPUT_DEFAULT_LOOK_DEADZONE },
    { PS2_INPUT_DEFAULT_MOVE_DEADZONE, PS2_INPUT_DEFAULT_MOVE_DEADZONE,
      PS2_INPUT_DEFAULT_LOOK_DEADZONE, PS2_INPUT_DEFAULT_LOOK_DEADZONE },
    { PS2_INPUT_DEFAULT_MOVE_DEADZONE, PS2_INPUT_DEFAULT_MOVE_DEADZONE,
      PS2_INPUT_DEFAULT_LOOK_DEADZONE, PS2_INPUT_DEFAULT_LOOK_DEADZONE },
    { PS2_INPUT_DEFAULT_MOVE_DEADZONE, PS2_INPUT_DEFAULT_MOVE_DEADZONE,
      PS2_INPUT_DEFAULT_LOOK_DEADZONE, PS2_INPUT_DEFAULT_LOOK_DEADZONE },
};
static f32 s_rumble_scale[INPUT_MAX_CONTROLLERS] = {
    0.5f, 0.5f, 0.5f, 0.5f,
};
static u64 s_rumble_deadline[INPUT_MAX_CONTROLLERS];
static u32 s_binds[INPUT_MAX_CONTROLLERS][CK_TOTAL_COUNT][INPUT_MAX_BINDS];
static s32 s_text_input;

/*
 * Control schemes. Bindings are stored per action as a mask of DualShock 2
 * button indices (the VK joy index space below), so a custom layout survives
 * in pd.ini as one integer per action.
 */
static s32 s_scheme[INPUT_MAX_CONTROLLERS];
static s32 s_custom_mask[INPUT_MAX_CONTROLLERS][CK_TOTAL_COUNT];
static s32 s_last_joy_index = -1;

enum {
    PS2_JOY_CROSS = 0, PS2_JOY_CIRCLE = 1, PS2_JOY_SQUARE = 2,
    PS2_JOY_TRIANGLE = 3, PS2_JOY_SELECT = 4, PS2_JOY_START = 6,
    PS2_JOY_L3 = 7, PS2_JOY_R3 = 8, PS2_JOY_L1 = 9, PS2_JOY_R1 = 10,
    PS2_JOY_UP = 11, PS2_JOY_DOWN = 12, PS2_JOY_LEFT = 13,
    PS2_JOY_RIGHT = 14, PS2_JOY_L2 = 30, PS2_JOY_R2 = 31,
};

struct Ps2SchemeBind {
    u8 ck;
    u8 joy;
};

/* Default: matches the desktop port's gamepad layout. */
static const struct Ps2SchemeBind s_scheme_modern[] = {
    { CK_ZTRIG, PS2_JOY_R2 }, { CK_RTRIG, PS2_JOY_L2 },
    { CK_LTRIG, PS2_JOY_R1 }, { CK_DPAD_D, PS2_JOY_L1 },
    { CK_A, PS2_JOY_CROSS }, { CK_DPAD_L, PS2_JOY_CIRCLE },
    { CK_X, PS2_JOY_SQUARE }, { CK_Y, PS2_JOY_TRIANGLE },
    { CK_8000, PS2_JOY_L3 }, { CK_START, PS2_JOY_START },
    { CK_C_U, PS2_JOY_UP }, { CK_C_D, PS2_JOY_DOWN },
    { CK_C_L, PS2_JOY_LEFT }, { CK_C_R, PS2_JOY_RIGHT },
    { CK_ACCEPT, PS2_JOY_CROSS }, { CK_CANCEL, PS2_JOY_CIRCLE },
};

/* Shoulder-button shooter layout (TimeSplitters 2 style). */
static const struct Ps2SchemeBind s_scheme_shooter[] = {
    { CK_ZTRIG, PS2_JOY_R1 }, { CK_RTRIG, PS2_JOY_L1 },
    { CK_LTRIG, PS2_JOY_R2 }, { CK_DPAD_D, PS2_JOY_L2 },
    { CK_A, PS2_JOY_CROSS }, { CK_8000, PS2_JOY_CIRCLE },
    { CK_X, PS2_JOY_SQUARE }, { CK_Y, PS2_JOY_TRIANGLE },
    { CK_Y, PS2_JOY_RIGHT }, { CK_DPAD_L, PS2_JOY_LEFT },
    { CK_START, PS2_JOY_START },
    { CK_C_U, PS2_JOY_UP }, { CK_C_D, PS2_JOY_DOWN },
    { CK_ACCEPT, PS2_JOY_CROSS }, { CK_CANCEL, PS2_JOY_CIRCLE },
};

/* Original N64 controller roles; right stick acts as the C buttons. */
static const struct Ps2SchemeBind s_scheme_n64[] = {
    { CK_A, PS2_JOY_CROSS }, { CK_B, PS2_JOY_CIRCLE },
    { CK_LTRIG, PS2_JOY_L1 }, { CK_RTRIG, PS2_JOY_R1 },
    { CK_ZTRIG, PS2_JOY_R2 }, { CK_ZTRIG, PS2_JOY_L2 },
    { CK_START, PS2_JOY_START },
    { CK_DPAD_U, PS2_JOY_UP }, { CK_DPAD_D, PS2_JOY_DOWN },
    { CK_DPAD_L, PS2_JOY_LEFT }, { CK_DPAD_R, PS2_JOY_RIGHT },
};

static const char *s_scheme_names[PS2_INPUT_SCHEME_COUNT] = {
    "Modern", "Shoulder Shooter", "N64 Classic", "Custom",
};

static const char *s_cont_key_names[CK_TOTAL_COUNT] = {
    "R_CBUTTONS", "L_CBUTTONS", "D_CBUTTONS", "U_CBUTTONS",
    "R_TRIG", "L_TRIG", "X_BUTTON", "Y_BUTTON",
    "R_JPAD", "L_JPAD", "D_JPAD", "U_JPAD", "START_BUTTON",
    "Z_TRIG", "B_BUTTON", "A_BUTTON", "STICK_XNEG", "STICK_XPOS",
    "STICK_YNEG", "STICK_YPOS", "ACCEPT_BUTTON", "CANCEL_BUTTON",
    "CK_0040", "CK_0080", "CK_0100", "CK_0200", "CK_0400",
    "CK_0800", "CK_1000", "CK_2000", "CK_4000", "CK_8000",
};

/* DualShock 2 names, indexed like the desktop VK joy button space. */
static const char *s_joy_key_names[INPUT_MAX_CONTROLLER_BUTTONS] = {
    "Cross", "Circle", "Square", "Triangle", "Select", "-", "Start", "L3",
    "R3", "L1", "R1", "D-Pad Up", "D-Pad Down",
    "D-Pad Left", "D-Pad Right", "-", "-", "-",
    "-", "-", "-", "-", "-",
    "-", "-", "-", "-", "-",
    "-", "-", "L2", "R2",
};

static bool ps2InputControllerIndexValid(s32 index)
{
    return index >= 0 && index < INPUT_MAX_CONTROLLERS;
}

static const struct Ps2PadState *ps2InputAssignedPad(s32 controller)
{
    if (!ps2InputControllerIndexValid(controller)) {
        return NULL;
    }

    const s32 port = s_assigned_port[controller];
    if (port < 0 || port >= PS2_INPUT_PHYSICAL_PORTS) {
        return NULL;
    }
    return ps2PadGetState(port);
}

static uint32_t ps2InputRawButtonForVirtualIndex(uint32_t index)
{
    switch (index) {
        case 0:  return PS2_PAD_CROSS;
        case 1:  return PS2_PAD_CIRCLE;
        case 2:  return PS2_PAD_SQUARE;
        case 3:  return PS2_PAD_TRIANGLE;
        case 4:  return PS2_PAD_SELECT;
        case 6:  return PS2_PAD_START;
        case 7:  return PS2_PAD_L3;
        case 8:  return PS2_PAD_R3;
        case 9:  return PS2_PAD_L1;
        case 10: return PS2_PAD_R1;
        case 11: return PS2_PAD_UP;
        case 12: return PS2_PAD_DOWN;
        case 13: return PS2_PAD_LEFT;
        case 14: return PS2_PAD_RIGHT;
        case 30: return PS2_PAD_L2;
        case 31: return PS2_PAD_R2;
        default: return 0u;
    }
}

static bool ps2InputDecodeVirtualKey(u32 vk, s32 *controller,
    uint32_t *raw_button)
{
    if (vk < VK_JOY_BEGIN || vk >= VK_TOTAL_COUNT) {
        return false;
    }

    const u32 relative = vk - VK_JOY_BEGIN;
    const s32 logical_controller =
        (s32)(relative / INPUT_MAX_CONTROLLER_BUTTONS);
    const uint32_t index = relative % INPUT_MAX_CONTROLLER_BUTTONS;
    const uint32_t mask = ps2InputRawButtonForVirtualIndex(index);
    if (!ps2InputControllerIndexValid(logical_controller) || mask == 0u) {
        return false;
    }

    if (controller) {
        *controller = logical_controller;
    }
    if (raw_button) {
        *raw_button = mask;
    }
    return true;
}

static bool ps2InputBindPressed(s32 controller, u32 cont_key)
{
    if (!ps2InputControllerIndexValid(controller) ||
        cont_key >= CK_TOTAL_COUNT) {
        return false;
    }

    for (s32 bind = 0; bind < INPUT_MAX_BINDS; ++bind) {
        const u32 vk = s_binds[controller][cont_key][bind];
        if (vk != 0u && inputKeyPressed(vk)) {
            return true;
        }
    }
    return false;
}

static void ps2InputAddBind(s32 controller, u32 cont_key, u32 joy_index)
{
    u32 *slots = s_binds[controller][cont_key];
    const u32 vk = VK_JOY_BEGIN +
        (u32)controller * INPUT_MAX_CONTROLLER_BUTTONS + joy_index;
    for (s32 slot = 0; slot < INPUT_MAX_BINDS; ++slot) {
        if (slots[slot] == vk) {
            return;
        }
        if (slots[slot] == 0u) {
            slots[slot] = vk;
            return;
        }
    }
}

static void ps2InputApplyTable(s32 controller,
    const struct Ps2SchemeBind *table, size_t count)
{
    memset(s_binds[controller], 0, sizeof(s_binds[controller]));
    for (size_t i = 0; i < count; ++i) {
        ps2InputAddBind(controller, table[i].ck, table[i].joy);
    }
}

/* Snapshot the current bindings as the controller's custom layout. */
static void ps2InputCaptureCustom(s32 controller)
{
    for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
        s32 mask = 0;
        for (s32 slot = 0; slot < INPUT_MAX_BINDS; ++slot) {
            const u32 vk = s_binds[controller][ck][slot];
            if (vk >= VK_JOY_BEGIN && vk < VK_TOTAL_COUNT) {
                mask |= (s32)(1u << ((vk - VK_JOY_BEGIN) %
                    INPUT_MAX_CONTROLLER_BUTTONS));
            }
        }
        s_custom_mask[controller][ck] = mask;
    }
}

static void ps2InputApplyScheme(s32 controller)
{
    switch (s_scheme[controller]) {
        case PS2_INPUT_SCHEME_SHOOTER:
            ps2InputApplyTable(controller, s_scheme_shooter,
                sizeof(s_scheme_shooter) / sizeof(s_scheme_shooter[0]));
            break;
        case PS2_INPUT_SCHEME_N64:
            ps2InputApplyTable(controller, s_scheme_n64,
                sizeof(s_scheme_n64) / sizeof(s_scheme_n64[0]));
            break;
        case PS2_INPUT_SCHEME_CUSTOM:
            memset(s_binds[controller], 0, sizeof(s_binds[controller]));
            for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
                for (u32 joy = 0; joy < 32u; ++joy) {
                    if ((u32)s_custom_mask[controller][ck] & (1u << joy)) {
                        ps2InputAddBind(controller, ck, joy);
                    }
                }
            }
            break;
        case PS2_INPUT_SCHEME_MODERN:
        default:
            ps2InputApplyTable(controller, s_scheme_modern,
                sizeof(s_scheme_modern) / sizeof(s_scheme_modern[0]));
            break;
    }
}

s32 inputPs2SchemeCount(void)
{
    return PS2_INPUT_SCHEME_COUNT;
}

const char *inputPs2SchemeName(s32 scheme)
{
    return scheme >= 0 && scheme < PS2_INPUT_SCHEME_COUNT
        ? s_scheme_names[scheme] : "";
}

s32 inputPs2GetScheme(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_scheme[controller] : PS2_INPUT_SCHEME_MODERN;
}

void inputPs2SetScheme(s32 controller, s32 scheme)
{
    if (!ps2InputControllerIndexValid(controller) ||
        scheme < 0 || scheme >= PS2_INPUT_SCHEME_COUNT) {
        return;
    }
    if (scheme == PS2_INPUT_SCHEME_CUSTOM &&
        s_scheme[controller] != PS2_INPUT_SCHEME_CUSTOM) {
        /* Start customising from whatever is active now. */
        ps2InputCaptureCustom(controller);
    }
    s_scheme[controller] = scheme;
    ps2InputApplyScheme(controller);
    /* N64 roles need the original control logic and C-button look. */
    g_PlayerExtCfg[controller].extcontrols =
        scheme == PS2_INPUT_SCHEME_N64 ? 0 : 1;
    s_dual_analog[controller] = scheme == PS2_INPUT_SCHEME_N64 ? 0 : 1;
}

/* Replace an action's buttons with one DualShock 2 button (custom scheme). */
void inputPs2BindButton(s32 controller, u32 cont_key, s32 joy_index)
{
    if (!ps2InputControllerIndexValid(controller) ||
        cont_key >= CK_TOTAL_COUNT || joy_index < -1 || joy_index >= 32) {
        return;
    }
    if (s_scheme[controller] != PS2_INPUT_SCHEME_CUSTOM) {
        ps2InputCaptureCustom(controller);
        s_scheme[controller] = PS2_INPUT_SCHEME_CUSTOM;
    }
    s_custom_mask[controller][cont_key] =
        joy_index < 0 ? 0 : (s32)(1u << joy_index);
    ps2InputApplyScheme(controller);
}

/* Comma-separated DualShock 2 buttons bound to an action, or "-". */
const char *inputPs2DescribeBind(s32 controller, u32 cont_key)
{
    static char text[64];
    text[0] = '\0';
    const u32 *binds = inputKeyGetBinds(controller, cont_key);
    for (s32 slot = 0; binds && slot < INPUT_MAX_BINDS; ++slot) {
        if (binds[slot] < VK_JOY_BEGIN || binds[slot] >= VK_TOTAL_COUNT) {
            continue;
        }
        const u32 joy = (binds[slot] - VK_JOY_BEGIN) %
            INPUT_MAX_CONTROLLER_BUTTONS;
        if (text[0]) {
            strncat(text, ", ", sizeof(text) - strlen(text) - 1);
        }
        strncat(text, s_joy_key_names[joy], sizeof(text) - strlen(text) - 1);
    }
    if (!text[0]) {
        strcpy(text, "-");
    }
    return text;
}

/* Joy index of the most recent new button press since the last clear. */
s32 inputPs2GetLastButton(void)
{
    return s_last_joy_index;
}

s32 inputInit(void)
{
    for (s32 controller = 0; controller < INPUT_MAX_CONTROLLERS;
         ++controller) {
        if (s_scheme[controller] < 0 ||
            s_scheme[controller] >= PS2_INPUT_SCHEME_COUNT) {
            s_scheme[controller] = PS2_INPUT_SCHEME_MODERN;
        }
        ps2InputApplyScheme(controller);
    }

    if (!ps2PadInit()) {
        return -1;
    }
    ps2PadUpdate();
    return inputControllerMask();
}

void inputUpdate(void)
{
    ps2PadUpdate();

    for (s32 port = 0; port < PS2_INPUT_PHYSICAL_PORTS; ++port) {
        const struct Ps2PadState *pad = ps2PadGetState(port);
        if (!pad || !pad->connected || pad->pressed == 0u) {
            continue;
        }
        for (u32 joy = 0; joy < 32u; ++joy) {
            const u32 mask = ps2InputRawButtonForVirtualIndex(joy);
            if (mask != 0u && (pad->pressed & mask) != 0u) {
                s_last_joy_index = (s32)joy;
                break;
            }
        }
    }

    const u64 now = sysGetMicroseconds();
    for (s32 controller = 0; controller < INPUT_MAX_CONTROLLERS;
         ++controller) {
        if (s_rumble_deadline[controller] != 0u &&
            now >= s_rumble_deadline[controller]) {
            const s32 port = s_assigned_port[controller];
            if (port >= 0 && port < PS2_INPUT_PHYSICAL_PORTS) {
                ps2PadSetRumble(port, 0u, 0u);
            }
            s_rumble_deadline[controller] = 0u;
        }
    }
}

s32 inputPs2TraceSelectHeld(void)
{
    const struct Ps2PadState *pad = ps2PadGetState(0);
    return pad && pad->connected && (pad->held & PS2_PAD_SELECT) != 0u;
}

s32 inputReadController(s32 controller, OSContPad *pad)
{
    if (!ps2InputControllerIndexValid(controller) || !pad) {
        return -1;
    }

    const struct Ps2PadState *source = ps2InputAssignedPad(controller);
    if (!source || !source->connected) {
        memset(pad, 0, sizeof(*pad));
        pad->errnum = CONT_NO_RESPONSE_ERROR;
        return -1;
    }

    const struct Ps2InputMapConfig config = {
        s_swap_sticks[controller] != 0,
        s_dual_analog[controller] != 0,
        s_cancel_c_buttons[controller] != 0,
        {
            s_axis_scale[controller][0], s_axis_scale[controller][1],
            s_axis_scale[controller][2], s_axis_scale[controller][3],
        },
        {
            s_axis_deadzone[controller][0], s_axis_deadzone[controller][1],
            s_axis_deadzone[controller][2], s_axis_deadzone[controller][3],
        },
        true,
    };
    if (!ps2InputMapPad(source, &config, pad)) {
        return -1;
    }

    /* Bindings add to the right-stick C buttons produced without dual analog. */
    for (u32 cont_key = 0u; cont_key < CONT_NUM_BUTTONS; ++cont_key) {
        if (ps2InputBindPressed(controller, cont_key)) {
            pad->button |= 1u << cont_key;
        }
    }

    if (s_cancel_c_buttons[controller]) {
        if ((pad->button & (L_CBUTTONS | R_CBUTTONS)) ==
                (L_CBUTTONS | R_CBUTTONS)) {
            pad->button &= ~(L_CBUTTONS | R_CBUTTONS);
        }
        if ((pad->button & (U_CBUTTONS | D_CBUTTONS)) ==
                (U_CBUTTONS | D_CBUTTONS)) {
            pad->button &= ~(U_CBUTTONS | D_CBUTTONS);
        }
    }
    return 0;
}

s32 inputControllerConnected(s32 controller)
{
    const struct Ps2PadState *pad = ps2InputAssignedPad(controller);
    return pad && pad->connected;
}

s32 inputControllerMask(void)
{
    s32 mask = 0;
    for (s32 controller = 0; controller < INPUT_MAX_CONTROLLERS;
         ++controller) {
        if (inputControllerConnected(controller)) {
            mask |= 1 << controller;
        }
    }
    return mask;
}

s32 inputRumbleSupported(s32 controller)
{
    const struct Ps2PadState *pad = ps2InputAssignedPad(controller);
    return pad && pad->connected && pad->rumble &&
        s_rumble_scale[controller] > 0.0f;
}

void inputRumble(s32 controller, f32 strength, f32 time)
{
    if (!ps2InputControllerIndexValid(controller)) {
        return;
    }

    const s32 port = s_assigned_port[controller];
    if (port < 0 || port >= PS2_INPUT_PHYSICAL_PORTS) {
        return;
    }

    strength *= s_rumble_scale[controller];
    if (strength <= 0.0f || time <= 0.0f) {
        ps2PadSetRumble(port, 0u, 0u);
        s_rumble_deadline[controller] = 0u;
        return;
    }
    if (strength > 1.0f) {
        strength = 1.0f;
    }

    ps2PadSetRumble(port, strength >= 0.75f ? 1u : 0u,
        (uint8_t)(strength * 255.0f));
    s_rumble_deadline[controller] = sysGetMicroseconds() +
        (u64)(time * 1000000.0f);
}

f32 inputRumbleGetStrength(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_rumble_scale[controller] : 0.0f;
}

void inputRumbleSetStrength(s32 controller, f32 strength)
{
    if (!ps2InputControllerIndexValid(controller)) {
        return;
    }
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    s_rumble_scale[controller] = strength;
}

s32 inputControllerGetSticksSwapped(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_swap_sticks[controller] : 0;
}

void inputControllerSetSticksSwapped(s32 controller, s32 swapped)
{
    if (ps2InputControllerIndexValid(controller)) {
        s_swap_sticks[controller] = swapped != 0;
    }
}

s32 inputControllerGetDualAnalog(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_dual_analog[controller] : 0;
}

void inputControllerSetDualAnalog(s32 controller, s32 enable)
{
    if (ps2InputControllerIndexValid(controller)) {
        s_dual_analog[controller] = enable != 0;
    }
}

f32 inputControllerGetAxisScale(s32 controller, s32 stick, s32 axis)
{
    const s32 index = stick * 2 + axis;
    if (!ps2InputControllerIndexValid(controller) || index < 0 || index >= 4) {
        return 0.0f;
    }
    return s_axis_scale[controller][index];
}

void inputControllerSetAxisScale(s32 controller, s32 stick, s32 axis,
    f32 value)
{
    const s32 index = stick * 2 + axis;
    if (ps2InputControllerIndexValid(controller) && index >= 0 && index < 4) {
        s_axis_scale[controller][index] = value;
    }
}

f32 inputControllerGetAxisDeadzone(s32 controller, s32 stick, s32 axis)
{
    const s32 index = stick * 2 + axis;
    if (!ps2InputControllerIndexValid(controller) || index < 0 || index >= 4) {
        return 0.0f;
    }
    return (f32)s_axis_deadzone[controller][index] / 32767.0f;
}

void inputControllerSetAxisDeadzone(s32 controller, s32 stick, s32 axis,
    f32 value)
{
    const s32 index = stick * 2 + axis;
    if (!ps2InputControllerIndexValid(controller) || index < 0 || index >= 4) {
        return;
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    s_axis_deadzone[controller][index] = (s32)(value * 32767.0f);
}

s32 inputGetConnectedControllers(s32 *out)
{
    s32 count = 0;
    for (s32 port = 0; port < PS2_INPUT_PHYSICAL_PORTS; ++port) {
        const struct Ps2PadState *pad = ps2PadGetState(port);
        if (pad && pad->connected) {
            if (out && count < INPUT_MAX_CONNECTED_CONTROLLERS) {
                out[count] = port;
            }
            ++count;
        }
    }
    return count;
}

const char *inputGetConnectedControllerName(s32 id)
{
    static const char *names[PS2_INPUT_PHYSICAL_PORTS] = {
        "PS2 DualShock 2 Port 1", "PS2 DualShock 2 Port 2",
    };
    return id >= 0 && id < PS2_INPUT_PHYSICAL_PORTS
        ? names[id] : "Invalid";
}

s32 inputGetAssignedControllerId(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_assigned_port[controller] : -1;
}

s32 inputAssignController(s32 controller, s32 id)
{
    if (!ps2InputControllerIndexValid(controller) ||
        id < -1 || id >= PS2_INPUT_PHYSICAL_PORTS) {
        return 0;
    }
    if (id >= 0) {
        for (s32 other = 0; other < INPUT_MAX_CONTROLLERS; ++other) {
            if (other != controller && s_assigned_port[other] == id) {
                s_assigned_port[other] = -1;
            }
        }
    }
    s_assigned_port[controller] = id;
    return 1;
}

const u32 *inputKeyGetBinds(s32 controller, u32 cont_key)
{
    if (!ps2InputControllerIndexValid(controller) ||
        cont_key >= CK_TOTAL_COUNT) {
        return NULL;
    }
    return s_binds[controller][cont_key];
}

s32 inputKeyPressed(u32 vk)
{
    s32 controller;
    uint32_t raw_button;
    if (!ps2InputDecodeVirtualKey(vk, &controller, &raw_button)) {
        return 0;
    }
    const struct Ps2PadState *pad = ps2InputAssignedPad(controller);
    return pad && pad->connected && (pad->held & raw_button) != 0u;
}

s32 inputKeyJustPressed(u32 vk)
{
    s32 controller;
    uint32_t raw_button;
    if (!ps2InputDecodeVirtualKey(vk, &controller, &raw_button)) {
        return 0;
    }
    const struct Ps2PadState *pad = ps2InputAssignedPad(controller);
    return pad && pad->connected && (pad->pressed & raw_button) != 0u;
}

s32 inputMouseIsLocked(void) { return 0; }
s32 inputMouseGetPosition(s32 *x, s32 *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}
void inputMouseGetScaledDelta(f32 *dx, f32 *dy)
{
    if (dx) *dx = 0.0f;
    if (dy) *dy = 0.0f;
}
void inputMouseGetAbsScaledDelta(f32 *dx, f32 *dy)
{
    inputMouseGetScaledDelta(dx, dy);
}
s32 inputMouseIsEnabled(void) { return 0; }
s32 inputAutoLockMouse(s32 wantlock) { (void)wantlock; return 0; }

void inputSaveBinds(void) {}
void inputClearLastKey(void) { s_last_joy_index = -1; }
void inputStartTextInput(void) { s_text_input = 1; }
void inputStopTextInput(void) { s_text_input = 0; }
void inputClearLastTextChar(void) {}
s32 inputTextHandler(char *out, const u32 out_size, s32 *column,
    s32 osk_chars_only)
{
    (void)out;
    (void)out_size;
    (void)column;
    (void)osk_chars_only;
    return 0;
}
u32 inputGetKeyModState(void) { return 0u; }

PD_CONSTRUCTOR static void ps2InputConfigInit(void)
{
    char key[96];
    for (s32 controller = 0; controller < INPUT_MAX_CONTROLLERS;
         ++controller) {
        const s32 player = controller + 1;
        snprintf(key, sizeof(key), "Input.Player%d.SwapSticks", player);
        configRegisterInt(key, &s_swap_sticks[controller], 0, 1);
        snprintf(key, sizeof(key), "Input.Player%d.DualAnalog", player);
        configRegisterInt(key, &s_dual_analog[controller], 0, 1);
        snprintf(key, sizeof(key), "Input.Player%d.CancelCButtons", player);
        configRegisterInt(key, &s_cancel_c_buttons[controller], 0, 1);
        snprintf(key, sizeof(key), "Input.Player%d.ControlScheme", player);
        configRegisterInt(key, &s_scheme[controller], 0,
            PS2_INPUT_SCHEME_COUNT - 1);
        for (u32 ck = 0; ck < CK_TOTAL_COUNT; ++ck) {
            if (strncmp(s_cont_key_names[ck], "CK_", 3) == 0 &&
                ck != CK_8000 && ck != CK_4000 && ck != CK_2000) {
                continue; /* unused extension slots */
            }
            snprintf(key, sizeof(key), "Input.Player%d.Custom.%s",
                player, s_cont_key_names[ck]);
            configRegisterInt(key, &s_custom_mask[controller][ck],
                INT32_MIN, INT32_MAX);
        }
        snprintf(key, sizeof(key), "Input.Player%d.RumbleScale", player);
        configRegisterFloat(key, &s_rumble_scale[controller], 0.0f, 1.0f);

        static const char *axis_names[4] = {
            "LStickX", "LStickY", "RStickX", "RStickY",
        };
        for (s32 axis = 0; axis < 4; ++axis) {
            snprintf(key, sizeof(key), "Input.Player%d.%sScale",
                player, axis_names[axis]);
            configRegisterFloat(key, &s_axis_scale[controller][axis],
                -30.0f, 30.0f);
            snprintf(key, sizeof(key), "Input.Player%d.%sDeadzone",
                player, axis_names[axis]);
            configRegisterInt(key, &s_axis_deadzone[controller][axis],
                0, 32766);
        }
    }
}
