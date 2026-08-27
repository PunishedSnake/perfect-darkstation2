#include "gs_render_target_layout.h"

#include <assert.h>
#include <stdio.h>

static void test_ct32_page_footprints(void)
{
    Ps2GsRenderTargetLayout layout{};

    assert(ps2GsDescribeCt32RenderTarget(1u, 1u, &layout));
    assert(layout.fbw == 1u);
    assert(layout.bytes == 0x2000u);

    assert(ps2GsDescribeCt32RenderTarget(64u, 32u, &layout));
    assert(layout.fbw == 1u);
    assert(layout.bytes == 0x2000u);

    assert(ps2GsDescribeCt32RenderTarget(65u, 33u, &layout));
    assert(layout.fbw == 2u);
    assert(layout.bytes == 0x8000u);

    assert(ps2GsDescribeCt32RenderTarget(320u, 240u, &layout));
    assert(layout.fbw == 5u);
    assert(layout.bytes == 0x50000u);

    assert(ps2GsDescribeCt32RenderTarget(640u, 448u, &layout));
    assert(layout.fbw == 10u);
    assert(layout.bytes == 0x118000u);
}

static void test_invalid_layouts(void)
{
    Ps2GsRenderTargetLayout layout{};
    assert(!ps2GsDescribeCt32RenderTarget(0u, 1u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 0u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1025u, 1u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 1025u, &layout));
    assert(!ps2GsDescribeCt32RenderTarget(1u, 1u, NULL));
}

int main(void)
{
    test_ct32_page_footprints();
    test_invalid_layouts();
    puts("gs_render_target_layout tests passed");
    return 0;
}
