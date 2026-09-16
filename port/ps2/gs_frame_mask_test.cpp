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

static void test_clear_is_independent_from_draw_masks(void)
{
    assert(ps2GsClearFrameWriteMask(true) == 0u);
    assert(ps2GsClearFrameWriteMask(false) == UINT32_MAX);
    assert(ps2GsClearDepthWriteMask(true, true) == 0u);
    assert(ps2GsClearDepthWriteMask(false, true) == 1u);
    assert(ps2GsClearDepthWriteMask(true, false) == 1u);
}

static void test_disabled_depth_test_cannot_write_depth(void)
{
    assert(ps2GsDepthTestEnabled(true, true));
    assert(!ps2GsDepthTestEnabled(false, true));
    assert(!ps2GsDepthTestEnabled(true, false));

    assert(ps2GsDepthWriteEnabled(true, true, true));
    assert(!ps2GsDepthWriteEnabled(true, false, true));
    assert(!ps2GsDepthWriteEnabled(false, true, true));
    assert(!ps2GsDepthWriteEnabled(true, true, false));
}

int main(void)
{
    test_ct32_independent_lanes();
    test_ct16_aggregate_contract();
    test_clear_is_independent_from_draw_masks();
    test_disabled_depth_test_cannot_write_depth();
    puts("gs_frame_mask tests passed");
    return 0;
}
