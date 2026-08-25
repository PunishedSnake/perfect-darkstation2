#ifndef PERFECT_DARK_PS2_CONTROLS_H
#define PERFECT_DARK_PS2_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum Ps2ShooterAction {
    PS2_ACTION_FIRE          = 1u << 0,
    PS2_ACTION_AIM           = 1u << 1,
    PS2_ACTION_ALT_FIRE      = 1u << 2,
    PS2_ACTION_QUICK_GADGET  = 1u << 3,
    PS2_ACTION_USE           = 1u << 4,
    PS2_ACTION_RELOAD        = 1u << 5,
    PS2_ACTION_NEXT_WEAPON   = 1u << 6,
    PS2_ACTION_PREV_WEAPON   = 1u << 7,
    PS2_ACTION_CROUCH        = 1u << 8,
    PS2_ACTION_SPRINT        = 1u << 9,
    PS2_ACTION_ZOOM          = 1u << 10,
    PS2_ACTION_WEAPON_MENU   = 1u << 11,
    PS2_ACTION_PAUSE         = 1u << 12,
    PS2_ACTION_DEBUG         = 1u << 13,
};

struct Ps2ShooterControls {
    bool connected;
    float move_x;
    float move_y;
    float look_x;
    float look_y;
    uint32_t held;
    uint32_t pressed;
    uint32_t released;
};

/*
 * Default PS2 FPS layout:
 *   left stick  move                  right stick look/aim
 *   R1          primary fire          L1          aim/precision
 *   R2          alternate fire        L2          quick gadget/function
 *   Cross       use/interact           Square      reload
 *   Triangle    next weapon            Circle      crouch
 *   L3          sprint                 R3          zoom
 *   D-pad L/R   previous/next weapon   D-pad Up    weapon menu
 *   Start       pause                  Select      bring-up/debug action
 *
 * The semantic layer is intentionally independent from the N64 OSContPad
 * representation. Full-game integration can remap these actions without
 * changing the physical PADMAN backend.
 */
bool ps2ShooterControlsInit(void);
void ps2ShooterControlsUpdate(void);
const struct Ps2ShooterControls *ps2ShooterControlsGet(int player);

#ifdef __cplusplus
}
#endif

#endif
