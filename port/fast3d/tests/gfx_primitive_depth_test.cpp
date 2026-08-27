#include "gfx_primitive_depth.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void expect_near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.00001f);
}

int main(void)
{
    expect_near(gfx_primitive_depth_to_ndc(0x0000u), 0.0f);
    expect_near(gfx_primitive_depth_to_ndc(0x4000u), 0.5f);
    expect_near(gfx_primitive_depth_to_ndc(0x7fffu), 32767.0f / 32768.0f);

    /* The primitive-Z register ignores the high input bit. */
    expect_near(
        gfx_primitive_depth_to_ndc(0xffffu),
        gfx_primitive_depth_to_ndc(0x7fffu));
    expect_near(
        gfx_primitive_depth_to_ndc(0x8000u),
        gfx_primitive_depth_to_ndc(0x0000u));

    expect_near(gfx_primitive_depth_to_clip_z(0x0000u, 4.0f), -4.0f);
    expect_near(gfx_primitive_depth_to_clip_z(0x4000u, 4.0f), 0.0f);
    expect_near(
        gfx_primitive_depth_to_clip_z(0x7fffu, 4.0f),
        (32767.0f / 16384.0f - 1.0f) * 4.0f);

    puts("Fast3D primitive depth tests passed");
    return 0;
}
