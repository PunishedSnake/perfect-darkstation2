#include <stdbool.h>

#include "gs_core.h"
#include "system.h"
#include "log_ps2.h"

bool ps2GfxApiSceneRun(int romStatus);

/*
 * PS2 graphics bring-up owner.
 *
 * Stage 0 proved framebuffer/GIF presentation, Stage 1 proved direct GS
 * textured 3D, reciprocal Z16 and STQ on real hardware, and Stage 2 proved the
 * Perfect Dark GfxRenderingAPI backend. GS device lifetime, state, residency
 * and presentation now live below that adapter in gs_core.
 */
bool ps2VideoDiagRun(int rom_status)
{
    const struct Ps2GsCreateInfo gs_config = {
        PS2_GS_PSM_CT16,
        PS2_GS_PSMZ_16,
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

    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: core ready width=%d height=%d mode=0x%x rom_status=%d",
        ps2GsCoreGetWidth(), ps2GsCoreGetHeight(), ps2GsCoreGetMode(), rom_status);
    sysLogPrintf(LOG_NOTE,
        "GS diagnostic: handing device-independent core to Fast3D rendering baseline");
    ps2LogCheckpoint();

    /*
     * Keep the already-proven rendering-API scene active while transport is
     * replaced below gs_core. This remains the A/B correctness baseline for
     * packet, residency and synchronization changes.
     */
    return ps2GfxApiSceneRun(rom_status);
}
