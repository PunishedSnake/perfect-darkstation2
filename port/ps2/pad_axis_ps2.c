#include <limits.h>
#include <stdint.h>

#include "pad_axis_ps2.h"

int16_t ps2PadAxisHorizontalFromRaw(uint8_t raw)
{
    return (int16_t)(((int32_t)raw - 128) * 256);
}

int16_t ps2PadAxisVerticalFromRaw(uint8_t raw)
{
    /* libpad reports 0 at the top. Public pad state uses conventional +Y up. */
    const int32_t value = (128 - (int32_t)raw) * 256;

    /* Full forward is +32768 mathematically; saturate instead of wrapping. */
    return value > INT16_MAX ? INT16_MAX : (int16_t)value;
}
