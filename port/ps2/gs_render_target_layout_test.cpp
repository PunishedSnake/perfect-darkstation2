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

static void test_ct32_texture_views(void)
{
    Ps2GsRenderTargetLayout layout{};
    Ps2GsRenderTargetTextureView view{};

    assert(ps2GsDescribeCt32RenderTarget(1u, 1u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 1u);
    assert(view.tw == 0u);
    assert(view.th == 0u);
    assert(view.clamp_max_u == 0u);
    assert(view.clamp_max_v == 0u);

    assert(ps2GsDescribeCt32RenderTarget(65u, 33u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 2u);
    assert(view.tw == 7u);
    assert(view.th == 6u);
    assert(view.clamp_max_u == 64u);
    assert(view.clamp_max_v == 32u);

    assert(ps2GsDescribeCt32RenderTarget(1024u, 1024u, &layout));
    assert(ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
    assert(view.tbw == 16u);
    assert(view.tw == 10u);
    assert(view.th == 10u);
    assert(view.clamp_max_u == 1023u);
    assert(view.clamp_max_v == 1023u);
}

static void test_invalid_texture_views(void)
{
    Ps2GsRenderTargetLayout layout{};
    Ps2GsRenderTargetTextureView view{};

    assert(!ps2GsDescribeCt32RenderTargetTextureView(NULL, &view));
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, NULL));
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));

    assert(ps2GsDescribeCt32RenderTarget(64u, 32u, &layout));
    ++layout.bytes;
    assert(!ps2GsDescribeCt32RenderTargetTextureView(&layout, &view));
}

int main(void)
{
    test_ct32_page_footprints();
    test_invalid_layouts();
    test_ct32_texture_views();
    test_invalid_texture_views();
    puts("gs_render_target_layout tests passed");
    return 0;
}
