#include "storage_ps2.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <delaythread.h>
#include <fileio.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "log_ps2.h"
#include "system.h"

#define PS2_STORAGE_ENUM_TIMEOUT_USEC 2000000u
#define PS2_STORAGE_ENUM_RETRY_USEC 10000u

extern unsigned char usbd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbhdfsd_irx;

static bool ps2StoragePathUsesMass(const char *path)
{
    if (!path) {
        return false;
    }
    return strncmp(path, "mass:", 5u) == 0 ||
        strncmp(path, "mass0:", 6u) == 0 ||
        strncmp(path, "mass1:", 6u) == 0;
}

static bool ps2StorageMassRootReady(void)
{
    const int fd = fioDopen("mass:/");
    if (fd < 0) {
        return false;
    }
    fioDclose(fd);
    return true;
}

static int ps2StorageWaitForMass(void)
{
    const uint64_t start = sysGetMicroseconds();
    uint32_t attempts = 0u;

    do {
        ++attempts;
        if (ps2StorageMassRootReady()) {
            sysLogPrintf(LOG_NOTE,
                "STORAGE: mass: ready attempts=%u elapsed=%llu us",
                attempts,
                (unsigned long long)(sysGetMicroseconds() - start));
            return 0;
        }
        DelayThread(PS2_STORAGE_ENUM_RETRY_USEC);
    } while (sysGetMicroseconds() - start <
        PS2_STORAGE_ENUM_TIMEOUT_USEC);

    sysLogPrintf(LOG_WARNING,
        "STORAGE: mass: enumeration timeout attempts=%u limit=%u us",
        attempts, PS2_STORAGE_ENUM_TIMEOUT_USEC);
    return -1;
}

static int ps2StorageEnsureEmbeddedModule(
    const char *name, const unsigned char *image, unsigned int image_size)
{
    int result = SifSearchModuleByName(name);
    if (result > 0) {
        sysLogPrintf(LOG_NOTE,
            "STORAGE: reuse resident IOP module %s id=%d", name, result);
        return result;
    }

    int module_result = 0;
    result = SifExecModuleBuffer(
        (void *)image, image_size, 0, NULL, &module_result);
    if (result >= 0) {
        sysLogPrintf(LOG_NOTE,
            "STORAGE: executed embedded %s id=%d start_result=%d bytes=%u",
            name, result, module_result, image_size);
        return result;
    }

    const int resident = SifSearchModuleByName(name);
    if (resident > 0) {
        sysLogPrintf(LOG_WARNING,
            "STORAGE: %s load returned %d but resident module id=%d is usable",
            name, result, resident);
        return resident;
    }

    sysLogPrintf(LOG_ERROR,
        "STORAGE: failed to provide IOP module %s result=%d start_result=%d",
        name, result, module_result);
    return result;
}

s32 ps2StorageEnsureMass(const char *boot_path)
{
    sceSifInitRpc(0);
    SifLoadFileInit();
    fioInit();

    if (ps2StorageMassRootReady()) {
        sysLogPrintf(LOG_NOTE,
            "STORAGE: inherited mass: service is usable boot_path=%s",
            boot_path ? boot_path : "(null)");
        return 0;
    }

    const bool boot_requires_mass = ps2StoragePathUsesMass(boot_path);
    sysLogPrintf(boot_requires_mass ? LOG_WARNING : LOG_NOTE,
        "STORAGE: mass: unavailable after ExecPS2; boot_path=%s; "
        "starting embedded current-PS2SDK storage stack",
        boot_path ? boot_path : "(null)");

    /*
     * Loading from a buffer requires the standard PS2SDK LMB patch. Do not
     * reset the IOP here: controller/audio services and any working launcher
     * state remain valid, while missing USB services are added incrementally.
     */
    sbv_patch_enable_lmb();

    const int usbd = ps2StorageEnsureEmbeddedModule(
        "USB_driver", usbd_irx, size_usbd_irx);
    if (usbd < 0) {
        return usbd;
    }

    const int usbhdfsd = ps2StorageEnsureEmbeddedModule(
        "usbhdfsd", usbhdfsd_irx, size_usbhdfsd_irx);
    if (usbhdfsd < 0) {
        return usbhdfsd;
    }

    const int ready = ps2StorageWaitForMass();
    if (ready < 0 && boot_requires_mass) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: executable came from mass: but the device could not be restored");
    }
    return ready;
}
