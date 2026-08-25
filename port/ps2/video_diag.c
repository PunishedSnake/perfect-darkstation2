#include <stdbool.h>

#include <dmaKit.h>
#include <gsKit.h>

#include "system.h"
#include "log_ps2.h"

bool ps2GfxApiSceneRun(GSGLOBAL *gs, int romStatus);
bool ps2Fast3dSceneRun(GSGLOBAL *gs, int romStatus);

/*
 * PS2 graphics bring-up owner.
 *
 * Stage 0 proved framebuffer/GIF presentation, Stage 1 proved direct GS
 * textured 3D, reciprocal Z16 and STQ on real hardware, and Stage 2 proved the
 * actual Perfect Dark GfxRenderingAPI backend. The active stage now preserves
 * the same low-level GS/dmaKit setup but sends a bounded synthetic N64 GBI
 * display list through the real Fast3D translator before it reaches the PS2
 * rendering and window APIs.
 *
 * ps2GfxApiSceneRun remains linked as the lower-level A/B baseline so a future
 * hardware regression can distinguish translator failures from backend/GS
 * failures without reviving a second renderer abstraction.
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
     * for the current bounded correctness scene. The full renderer will get a
     * measured VRAM residency plan before choosing final framebuffer formats.
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
    sysLogPrintf(LOG_NOTE, "GS diagnostic: handing live GS to real Fast3D display-list path");
    ps2LogCheckpoint();

    return ps2Fast3dSceneRun(gs, rom_status);
}
