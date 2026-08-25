#include <stdbool.h>

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
