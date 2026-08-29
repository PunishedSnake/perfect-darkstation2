#include "gs_frame_mask.h"

#include <assert.h>
#include <stdio.h>

static void test_ct32_independent_lanes(void)
{
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_RGB, true, false) == 0u);
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_RGB, false, false) == 0xff000000u);
    assert(ps2GsFrameWriteMask(0u, true, false) == 0x00ffffffu);
    assert(ps2GsFrameWriteMask(0u, false, false) == 0xffffffffu);

    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_RED, false, false) == 0xffffff00u);
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_GREEN, false, false) == 0xffff00ffu);
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_BLUE, false, false) == 0xff00ffffu);
}

static void test_ct16_aggregate_contract(void)
{
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_RGB, true, true) == 0u);
    assert(ps2GsFrameWriteMask(
        PS2_GS_COLOR_WRITE_RGB, false, true) == 0x80000000u);
    assert(ps2GsFrameWriteMask(0u, true, true) == 0x7fffffffu);
    assert(ps2GsFrameWriteMask(0u, false, true) == 0xffffffffu);
}

int main(void)
{
    test_ct32_independent_lanes();
    test_ct16_aggregate_contract();
    puts("gs_frame_mask tests passed");
    return 0;
}
