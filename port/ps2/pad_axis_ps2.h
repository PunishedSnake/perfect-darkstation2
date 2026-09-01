#ifndef PERFECT_DARK_PS2_PAD_AXIS_H
#define PERFECT_DARK_PS2_PAD_AXIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert libpad's unsigned 0..255 stick bytes to signed public axes. */
int16_t ps2PadAxisHorizontalFromRaw(uint8_t raw);
int16_t ps2PadAxisVerticalFromRaw(uint8_t raw);

#ifdef __cplusplus
}
#endif

#endif
