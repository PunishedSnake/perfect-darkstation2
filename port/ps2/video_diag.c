#include <stdbool.h>

#include <dmaKit.h>
#include <gsKit.h>

#include "system.h"
#include "log_ps2.h"

bool ps2RendererSceneRun(GSGLOBAL *gs, int romStatus);

/*
 * Bring-up renderer only.
 *
 * This deliberately uses the current gsKit/dmaKit API instead of growing a
 * second graphics abstraction before the real GfxRenderingAPI backend exists.
 * Stage 0 proved framebuffer/GIF presentation. Stage 1 now enables a real
 * Z-buffer and hands the initialized GS to a textured 3D scene.
 */
bool ps2VideoDiagRun(int rom_status)
{
    sysLogPrintf(LOG_NOTE, "GS diagnostic: gsKit_init_global");
    ps2LogCheckpoint();
    GSGLOBAL *gs = gsKit_init_global();

    if (!gs) {
        sysLogPrintf(LOG_ERROR, "GS diagnostic: gsKit_init_global returned NULL");
        return false;
    }

    /*
     * CT16 keeps the double-buffered scanout footprint modest. Z16 is enough
     * for this bounded diagnostic scene and makes the depth-buffer contract
     * explicit without spending another 32-bit framebuffer-sized region.
     */
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16;
    gs->ZBuffering = GS_SETTING_ON;
    gs->Dithering = GS_SETTING_ON;

    sysLogPrintf(LOG_NOTE, "GS diagnostic: initialising dmaKit GIF channel");
    ps2LogCheckpoint();
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /* Reset gsKit's VRAM allocator before framebuffer/Z allocation. */
    gsKit_vram_clear(gs);

    sysLogPrintf(LOG_NOTE, "GS diagnostic: initialising screen + Z16");
    ps2LogCheckpoint();
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_ON);

    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: ready width=%d height=%d PSM=CT16 PSMZ=Z16 zbuffer=on rom_status=%d",
        gs->Width, gs->Height, rom_status);
    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: VRAM screen0=%08x screen1=%08x zbuffer=%08x next=%08x",
        gs->ScreenBuffer[0], gs->ScreenBuffer[1], gs->ZBuffer, gs->CurrentPointer);
    ps2LogCheckpoint();

    return ps2RendererSceneRun(gs, rom_status);
}
