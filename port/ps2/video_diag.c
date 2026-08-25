#include <stdbool.h>

#include <gsKit.h>

#include "gs_core.h"
#include "system.h"
#include "log_ps2.h"

bool ps2GfxApiSceneRun(GSGLOBAL *gs, int romStatus);

/*
 * PS2 graphics bring-up owner.
 *
 * Stage 0 proved framebuffer/GIF presentation, Stage 1 proved direct GS
 * textured 3D, reciprocal Z16 and STQ on real hardware, and Stage 2 proved the
 * Perfect Dark GfxRenderingAPI backend. GS device lifetime and scanout setup
 * now live below that adapter in gs_core, which is the migration boundary for
 * replacing gsKit queue transport with native GIF/DMAC packets.
 */
bool ps2VideoDiagRun(int rom_status)
{
    const struct Ps2GsCreateInfo gs_config = {
        GS_PSM_CT16,
        GS_PSMZ_16,
        true,
        true,
    };

    sysLogPrintf(LOG_NOTE, "GS diagnostic: initialise GS core");
    ps2LogCheckpoint();

    if (!ps2GsCoreInit(&gs_config)) {
        sysLogPrintf(LOG_ERROR, "GS diagnostic: GS core initialisation failed");
        ps2LogCheckpoint();
        return false;
    }

    GSGLOBAL *gs = ps2GsCoreGetLegacyGlobal();
    if (!gs) {
        sysLogPrintf(LOG_ERROR, "GS diagnostic: GS core has no legacy gsKit handle");
        ps2LogCheckpoint();
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: core ready width=%d height=%d mode=0x%x rom_status=%d",
        ps2GsCoreGetWidth(), ps2GsCoreGetHeight(), ps2GsCoreGetMode(), rom_status);
    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: handing GS device to Fast3D GfxRenderingAPI adapter baseline");
    ps2LogCheckpoint();

    /*
     * Keep the already-proven rendering-API scene active while device/state,
     * texture and draw ownership are moved out of gfx_ps2 into gs_core. This is
     * the A/B correctness baseline for each migration step.
     */
    return ps2GfxApiSceneRun(gs, rom_status);
}
