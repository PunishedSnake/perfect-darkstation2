#include <stdbool.h>
#include <stdint.h>

#include <dmaKit.h>
#include <gsKit.h>

#include "gs_core.h"
#include "log_ps2.h"
#include "system.h"

static GSGLOBAL *s_gs;

extern "C" bool ps2GsCoreInit(const struct Ps2GsCreateInfo *info)
{
    if (s_gs) {
        return true;
    }

    const struct Ps2GsCreateInfo defaults = {
        GS_PSM_CT16,
        GS_PSMZ_16,
        true,
        true,
    };
    const struct Ps2GsCreateInfo *config = info ? info : &defaults;

    sysLogPrintf(LOG_NOTE, "GS core: gsKit_init_global");
    ps2LogCheckpoint();

    GSGLOBAL *gs = gsKit_init_global();
    if (!gs) {
        sysLogPrintf(LOG_ERROR, "GS core: gsKit_init_global returned NULL");
        ps2LogCheckpoint();
        return false;
    }

    gs->PSM = config->color_psm;
    gs->PSMZ = config->depth_psm;
    gs->ZBuffering = config->z_buffering ? GS_SETTING_ON : GS_SETTING_OFF;
    gs->Dithering = config->dithering ? GS_SETTING_ON : GS_SETTING_OFF;

    /*
     * CURRENT IMPLEMENTATION: dmaKit + gsKit own the PATH3 queue transport.
     * Keeping this in one device module makes the later direct GIF packet
     * backend a transport replacement instead of a Fast3D rewrite.
     */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /* gsKit's VRAM allocator owns the bootstrap framebuffer/Z allocations. */
    gsKit_vram_clear(gs);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, config->z_buffering ? GS_ZTEST_ON : GS_ZTEST_OFF);

    s_gs = gs;

    sysLogPrintf(LOG_NOTE,
        "GS core: ready %dx%d mode=0x%x PSM=0x%x PSMZ=0x%x z=%d dither=%d",
        gs->Width,
        gs->Height,
        gs->Mode,
        gs->PSM,
        gs->PSMZ,
        gs->ZBuffering == GS_SETTING_ON ? 1 : 0,
        gs->Dithering == GS_SETTING_ON ? 1 : 0);
    sysLogPrintf(LOG_NOTE,
        "GS core: VRAM screen0=%08x screen1=%08x zbuffer=%08x next=%08x",
        gs->ScreenBuffer[0], gs->ScreenBuffer[1], gs->ZBuffer, gs->CurrentPointer);
    ps2LogCheckpoint();

    return true;
}

extern "C" GSGLOBAL *ps2GsCoreGetLegacyGlobal(void)
{
    return s_gs;
}

extern "C" int ps2GsCoreGetWidth(void)
{
    return s_gs ? s_gs->Width : 0;
}

extern "C" int ps2GsCoreGetHeight(void)
{
    return s_gs ? s_gs->Height : 0;
}

extern "C" int ps2GsCoreGetMode(void)
{
    return s_gs ? s_gs->Mode : 0;
}

extern "C" void ps2GsCoreBeginFrame(void)
{
    if (s_gs) {
        gsKit_set_scissor(s_gs, GS_SCISSOR_RESET);
    }
}

extern "C" void ps2GsCoreSubmit(void)
{
    if (s_gs) {
        /* Submit only. No GS FINISH and no VSync wait on this boundary. */
        gsKit_queue_exec(s_gs);
    }
}

extern "C" void ps2GsCorePresent(void)
{
    if (s_gs) {
        /* Final dependency for the scanout buffer remains as late as possible. */
        gsKit_sync_flip(s_gs);
    }
}

extern "C" void ps2GsCoreClear(bool clear_color, bool clear_depth)
{
    if (!s_gs || (!clear_color && !clear_depth)) {
        return;
    }

    /*
     * Bring-up contract: current gsKit clear updates color and Z together.
     * Split clears will become explicit GS packets when a real Perfect Dark
     * pass requires them. Do not silently fake one half of the operation.
     */
    gsKit_clear(s_gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
}

extern "C" void ps2GsCoreSetScissor(int x, int y, int width, int height)
{
    if (!s_gs || width <= 0 || height <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + width - 1;
    int y1 = y + height - 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s_gs->Width) x1 = s_gs->Width - 1;
    if (y1 >= s_gs->Height) y1 = s_gs->Height - 1;

    if (x0 <= x1 && y0 <= y1) {
        gsKit_set_scissor(s_gs, GS_SETREG_SCISSOR(x0, x1, y0, y1));
    }
}

static void ps2GsCoreEmitZbufWriteMask(bool depth_update)
{
    if (!s_gs || !s_gs->ZBuffering) {
        return;
    }

    u64 *p = (u64 *)gsKit_heap_alloc(s_gs, 1, 16, GIF_AD);
    *p++ = GIF_TAG_AD(1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_ZBUF(s_gs->ZBuffer / 8192, s_gs->PSMZ, depth_update ? 0 : 1);
    *p++ = GS_ZBUF_1 + s_gs->PrimContext;
}

extern "C" void ps2GsCoreSetDepthMode(bool depth_test, bool depth_update, bool depth_compare)
{
    if (!s_gs) {
        return;
    }

    if (depth_test && depth_compare) {
        gsKit_set_test(s_gs, GS_ZTEST_ON);
    } else {
        gsKit_set_test(s_gs, GS_ZTEST_OFF);
    }

    ps2GsCoreEmitZbufWriteMask(depth_update);
}

extern "C" void ps2GsCoreSetAlphaBlend(bool enable)
{
    if (!s_gs) {
        return;
    }

    s_gs->PrimAlphaEnable = enable ? GS_SETTING_ON : GS_SETTING_OFF;
    if (enable) {
        /* Standard source-alpha over destination baseline. */
        gsKit_set_primalpha(s_gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    }
}

extern "C" void ps2GsCoreSetTextureClamp(uint32_t cms, uint32_t cmt)
{
    if (!s_gs) {
        return;
    }

    /* N64 G_TX_CLAMP is bit 1; mirror semantics remain a Fast3D TODO. */
    s_gs->Clamp->WMS = (cms & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    s_gs->Clamp->WMT = (cmt & 2u) ? GS_CMODE_CLAMP : GS_CMODE_REPEAT;
    gsKit_set_clamp(s_gs, GS_CMODE_RESET);
}
