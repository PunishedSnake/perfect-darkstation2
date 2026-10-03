#include "memory_card_ps2.h"

#include <stdbool.h>

#include <libmc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "log_ps2.h"
#include "system.h"

extern unsigned char sio2man_irx[] __attribute__((aligned(16)));
extern unsigned int size_sio2man_irx;
extern unsigned char mcman_irx[] __attribute__((aligned(16)));
extern unsigned int size_mcman_irx;
extern unsigned char mcserv_irx[] __attribute__((aligned(16)));
extern unsigned int size_mcserv_irx;

static bool s_memory_card_ready;

static int ps2MemoryCardEnsureModule(
    const char *search_name,
    const char *display_name,
    unsigned char *image,
    unsigned int image_size)
{
    const int existing = SifSearchModuleByName(search_name);
    if (existing >= 0) {
        sysLogPrintf(LOG_NOTE,
            "MC: reuse project-owned %s id=%d", display_name, existing);
        return existing;
    }

    int module_result = 0;
    const int result = SifExecModuleBuffer(
        image, image_size, 0, NULL, &module_result);
    if (result >= 0) {
        sysLogPrintf(LOG_NOTE,
            "MC: executed embedded %s id=%d start_result=%d bytes=%u",
            display_name, result, module_result, image_size);
        return result;
    }

    const int resident = SifSearchModuleByName(search_name);
    if (resident >= 0) {
        sysLogPrintf(LOG_WARNING,
            "MC: %s load returned %d but project-owned module id=%d is resident",
            display_name, result, resident);
        return resident;
    }

    sysLogPrintf(LOG_ERROR,
        "MC: failed to provide %s result=%d start_result=%d",
        display_name, result, module_result);
    return result;
}

s32 ps2MemoryCardEnsureService(void)
{
    if (s_memory_card_ready) {
        return 0;
    }

    /*
     * CURRENT IMPLEMENTATION:
     * current libmc requires the SIO2 memory-card server stack to exist before
     * mcInit(). Keep it lazy so users who never touch memory-card profiles do
     * not spend IOP RAM or startup time on MCMAN/MCSERV.
     */
    sceSifInitRpc(0);
    SifLoadFileInit();
    sbv_patch_enable_lmb();

    int result = ps2MemoryCardEnsureModule(
        "sio2man", "sio2man", sio2man_irx, size_sio2man_irx);
    if (result < 0) {
        return result;
    }

    /*
     * Current PS2SDK's mcman.irx is the newer implementation and advertises
     * the IOP module name "mcman_cex".
     */
    result = ps2MemoryCardEnsureModule(
        "mcman_cex", "mcman", mcman_irx, size_mcman_irx);
    if (result < 0) {
        return result;
    }

    result = ps2MemoryCardEnsureModule(
        "mcserv", "mcserv", mcserv_irx, size_mcserv_irx);
    if (result < 0) {
        return result;
    }

    result = mcInit(MC_TYPE_MC);
    if (result < 0) {
        sysLogPrintf(LOG_ERROR, "MC: mcInit failed result=%d", result);
        return result;
    }

    s_memory_card_ready = true;
    sysLogPrintf(LOG_NOTE,
        "MC: lazy profile/save service ready; no card has been probed yet");
    return 0;
}
