#include <assert.h>
#include <stdint.h>

#include "pad_axis_ps2.h"

int main(void)
{
    assert(ps2PadAxisHorizontalFromRaw(0u) == INT16_MIN);
    assert(ps2PadAxisHorizontalFromRaw(128u) == 0);
    assert(ps2PadAxisHorizontalFromRaw(255u) == 32512);

    assert(ps2PadAxisVerticalFromRaw(0u) == INT16_MAX);
    assert(ps2PadAxisVerticalFromRaw(1u) == 32512);
    assert(ps2PadAxisVerticalFromRaw(128u) == 0);
    assert(ps2PadAxisVerticalFromRaw(255u) == -32512);

    int16_t previous_horizontal = ps2PadAxisHorizontalFromRaw(0u);
    int16_t previous_vertical = ps2PadAxisVerticalFromRaw(0u);
    for (uint16_t raw = 1u; raw <= 255u; ++raw) {
        const int16_t horizontal =
            ps2PadAxisHorizontalFromRaw((uint8_t)raw);
        const int16_t vertical =
            ps2PadAxisVerticalFromRaw((uint8_t)raw);
        assert(horizontal > previous_horizontal);
        assert(vertical < previous_vertical);
        previous_horizontal = horizontal;
        previous_vertical = vertical;
    }

    return 0;
}
