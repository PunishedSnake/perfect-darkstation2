#ifndef PERFECT_DARK_PS2_PAD_H
#define PERFECT_DARK_PS2_PAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_PAD_MAX_PLAYERS 2

enum Ps2PadButton {
    PS2_PAD_SELECT   = 0x0001u,
    PS2_PAD_L3       = 0x0002u,
    PS2_PAD_R3       = 0x0004u,
    PS2_PAD_START    = 0x0008u,
    PS2_PAD_UP       = 0x0010u,
    PS2_PAD_RIGHT    = 0x0020u,
    PS2_PAD_DOWN     = 0x0040u,
    PS2_PAD_LEFT     = 0x0080u,
    PS2_PAD_L2       = 0x0100u,
    PS2_PAD_R2       = 0x0200u,
    PS2_PAD_L1       = 0x0400u,
    PS2_PAD_R1       = 0x0800u,
    PS2_PAD_TRIANGLE = 0x1000u,
    PS2_PAD_CIRCLE   = 0x2000u,
    PS2_PAD_CROSS    = 0x4000u,
    PS2_PAD_SQUARE   = 0x8000u,
};

struct Ps2PadState {
    bool connected;
    bool analog;
    bool rumble;
    uint32_t held;
    uint32_t pressed;
    uint32_t released;

    /* Signed unit-domain source values. X: left/right. Y: up/down with up > 0. */
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
};

/*
 * Physical PADMAN/libpad backend. This is deliberately below Perfect Dark's
 * semantic controller mapping so native DualShock 2 input is not flattened
 * into an N64 pad before the game decides what an action means.
 */
bool ps2PadInit(void);
void ps2PadShutdown(void);
void ps2PadUpdate(void);
const struct Ps2PadState *ps2PadGetState(int player);

/* Cached actuator update: repeated identical values do not issue another RPC. */
void ps2PadSetRumble(int player, uint8_t small_motor, uint8_t large_motor);

#ifdef __cplusplus
}
#endif

#endif
