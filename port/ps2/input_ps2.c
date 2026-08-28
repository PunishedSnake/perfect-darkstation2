#include <math.h>
#include <stdio.h>
#include <string.h>

#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include <PR/ultratypes.h>

#include "config.h"
#include "input.h"
#include "platform.h"
#include "system.h"

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

static const char *s_cont_key_names[CK_TOTAL_COUNT] = {
    "R_CBUTTONS", "L_CBUTTONS", "D_CBUTTONS", "U_CBUTTONS",
    "R_TRIG", "L_TRIG", "X_BUTTON", "Y_BUTTON",
    "R_JPAD", "L_JPAD", "D_JPAD", "U_JPAD", "START_BUTTON",
    "Z_TRIG", "B_BUTTON", "A_BUTTON", "STICK_XNEG", "STICK_XPOS",
    "STICK_YNEG", "STICK_YPOS", "ACCEPT_BUTTON", "CANCEL_BUTTON",
    "CK_0040", "CK_0080", "CK_0100", "CK_0200", "CK_0400",
    "CK_0800", "CK_1000", "CK_2000", "CK_4000", "CK_8000",
};

static const char *s_joy_key_names[INPUT_MAX_CONTROLLER_BUTTONS] = {
    "A", "B", "X", "Y", "BACK", "GUIDE", "START", "LSTICK",
    "RSTICK", "LSHOULDER", "RSHOULDER", "DPAD_UP", "DPAD_DOWN",
    "DPAD_LEFT", "DPAD_RIGHT", "BUTTON_15", "BUTTON_16", "BUTTON_17",
    "BUTTON_18", "BUTTON_19", "BUTTON_20", "BUTTON_21", "BUTTON_22",
    "BUTTON_23", "BUTTON_24", "BUTTON_25", "BUTTON_26", "BUTTON_27",
    "BUTTON_28", "BUTTON_29", "LTRIGGER", "RTRIGGER",
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

static void ps2InputSetDefaultJoyBind(s32 controller, u32 cont_key,
    u32 joy_index)
{
    inputKeyBind(controller, cont_key, -1,
        VK_JOY_BEGIN + controller * INPUT_MAX_CONTROLLER_BUTTONS + joy_index);
}

void inputSetDefaultKeyBinds(s32 controller, s32 n64mode)
{
    if (!ps2InputControllerIndexValid(controller)) {
        return;
    }

    memset(s_binds[controller], 0, sizeof(s_binds[controller]));
    if (n64mode) {
        ps2InputSetDefaultJoyBind(controller, CK_A, 0);
        ps2InputSetDefaultJoyBind(controller, CK_B, 1);
        ps2InputSetDefaultJoyBind(controller, CK_LTRIG, 9);
        ps2InputSetDefaultJoyBind(controller, CK_RTRIG, 10);
        ps2InputSetDefaultJoyBind(controller, CK_ZTRIG, 31);
        ps2InputSetDefaultJoyBind(controller, CK_START, 6);
        ps2InputSetDefaultJoyBind(controller, CK_DPAD_U, 11);
        ps2InputSetDefaultJoyBind(controller, CK_DPAD_D, 12);
        ps2InputSetDefaultJoyBind(controller, CK_DPAD_L, 13);
        ps2InputSetDefaultJoyBind(controller, CK_DPAD_R, 14);
        return;
    }

    ps2InputSetDefaultJoyBind(controller, CK_A, 0);
    ps2InputSetDefaultJoyBind(controller, CK_X, 2);
    ps2InputSetDefaultJoyBind(controller, CK_Y, 3);
    ps2InputSetDefaultJoyBind(controller, CK_DPAD_L, 1);
    ps2InputSetDefaultJoyBind(controller, CK_DPAD_D, 9);
    ps2InputSetDefaultJoyBind(controller, CK_LTRIG, 10);
    ps2InputSetDefaultJoyBind(controller, CK_RTRIG, 30);
    ps2InputSetDefaultJoyBind(controller, CK_ZTRIG, 31);
    ps2InputSetDefaultJoyBind(controller, CK_START, 6);
    ps2InputSetDefaultJoyBind(controller, CK_C_U, 11);
    ps2InputSetDefaultJoyBind(controller, CK_C_D, 12);
    ps2InputSetDefaultJoyBind(controller, CK_C_L, 13);
    ps2InputSetDefaultJoyBind(controller, CK_C_R, 14);
    ps2InputSetDefaultJoyBind(controller, CK_ACCEPT, 0);
    ps2InputSetDefaultJoyBind(controller, CK_CANCEL, 1);
    ps2InputSetDefaultJoyBind(controller, CK_8000, 7);
}

s32 inputInit(void)
{
    for (s32 controller = 0; controller < INPUT_MAX_CONTROLLERS;
         ++controller) {
        inputSetDefaultKeyBinds(controller, false);
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
    };
    if (!ps2InputMapPad(source, &config, pad)) {
        return -1;
    }

    /* Button bindings remain configurable even though PS2 has no keyboard. */
    pad->button = 0u;
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

s32 inputControllerGetCancelCButtons(s32 controller)
{
    return ps2InputControllerIndexValid(controller)
        ? s_cancel_c_buttons[controller] : 0;
}

void inputControllerSetCancelCButtons(s32 controller, s32 cancel)
{
    if (ps2InputControllerIndexValid(controller)) {
        s_cancel_c_buttons[controller] = cancel != 0;
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

void inputKeyBind(s32 controller, u32 cont_key, s32 bind, u32 vk)
{
    if (!ps2InputControllerIndexValid(controller) ||
        cont_key >= CK_TOTAL_COUNT || bind >= INPUT_MAX_BINDS) {
        return;
    }
    if (bind < 0) {
        for (s32 slot = 0; slot < INPUT_MAX_BINDS; ++slot) {
            if (s_binds[controller][cont_key][slot] == 0u) {
                bind = slot;
                break;
            }
        }
        if (bind < 0) {
            bind = INPUT_MAX_BINDS - 1;
        }
    }
    s_binds[controller][cont_key][bind] = vk;
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

s32 inputButtonPressed(s32 controller, u32 cont_button)
{
    OSContPad pad;
    return inputReadController(controller, &pad) == 0 &&
        (pad.button & cont_button) != 0u;
}

const char *inputGetContKeyName(u32 cont_key)
{
    return cont_key < CK_TOTAL_COUNT ? s_cont_key_names[cont_key] : "";
}

s32 inputGetContKeyByName(const char *name)
{
    if (!name) {
        return -1;
    }
    for (s32 key = 0; key < CK_TOTAL_COUNT; ++key) {
        if (strcmp(name, s_cont_key_names[key]) == 0) {
            return key;
        }
    }
    return -1;
}

const char *inputGetKeyName(s32 vk)
{
    static char name[48];
    if (vk >= VK_JOY_BEGIN && vk < VK_TOTAL_COUNT) {
        const u32 relative = (u32)(vk - VK_JOY_BEGIN);
        const u32 controller = relative / INPUT_MAX_CONTROLLER_BUTTONS;
        const u32 index = relative % INPUT_MAX_CONTROLLER_BUTTONS;
        snprintf(name, sizeof(name), "JOY%u_%s",
            controller + 1u, s_joy_key_names[index]);
        return name;
    }
    return "UNAVAILABLE";
}

s32 inputGetKeyByName(const char *name)
{
    if (!name) {
        return -1;
    }
    for (s32 vk = VK_JOY_BEGIN; vk < VK_TOTAL_COUNT; ++vk) {
        if (strcmp(name, inputGetKeyName(vk)) == 0) {
            return vk;
        }
    }
    return -1;
}

void inputLockMouse(s32 lock) { (void)lock; }
s32 inputMouseIsLocked(void) { return 0; }
s32 inputMouseGetPosition(s32 *x, s32 *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}
void inputMouseGetRawDelta(s32 *dx, s32 *dy)
{
    if (dx) *dx = 0;
    if (dy) *dy = 0;
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
void inputMouseGetSpeed(f32 *x, f32 *y)
{
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}
void inputMouseSetSpeed(f32 x, f32 y) { (void)x; (void)y; }
s32 inputMouseIsEnabled(void) { return 0; }
void inputMouseEnable(s32 enabled) { (void)enabled; }
s32 inputAutoLockMouse(s32 wantlock) { (void)wantlock; return 0; }
void inputMouseShowCursor(s32 show) { (void)show; }
s32 inputGetMouseLockMode(void) { return MLOCK_OFF; }
void inputSetMouseLockMode(s32 lockmode) { (void)lockmode; }

void inputSaveBinds(void) {}
void inputClearLastKey(void) {}
s32 inputGetLastKey(void) { return 0; }
void inputStartTextInput(void) { s_text_input = 1; }
void inputStopTextInput(void) { s_text_input = 0; }
s32 inputIsTextInputActive(void) { return s_text_input; }
void inputClearLastTextChar(void) {}
char inputGetLastTextChar(void) { return 0; }
s32 inputTextHandler(char *out, const u32 out_size, s32 *column,
    s32 osk_chars_only)
{
    (void)out;
    (void)out_size;
    (void)column;
    (void)osk_chars_only;
    return 0;
}
void inputClearClipboard(void) {}
const char *inputGetClipboard(void) { return NULL; }
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
