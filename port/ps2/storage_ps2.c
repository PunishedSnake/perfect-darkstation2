#define NEWLIB_PORT_AWARE

#include "storage_ps2.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <delaythread.h>
#include <dirent.h>
#include <fileio.h>
#include <fileXio_rpc.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

#include "log_ps2.h"
#include "path_ps2.h"
#include "system.h"

#define PS2_STORAGE_ENUM_TIMEOUT_USEC 2000000u
#define PS2_STORAGE_ENUM_RETRY_USEC 10000u
#define PS2_STORAGE_IOP_RESET_REQUEST_TIMEOUT_USEC 500000u
#define PS2_STORAGE_IOP_RESET_SYNC_TIMEOUT_USEC 3000000u
#define PS2_STORAGE_IOP_RESET_RETRY_USEC 1000u

extern unsigned char usbd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[] __attribute__((aligned(16)));
extern unsigned int size_usbhdfsd_irx;

extern unsigned char iomanX_irx[] __attribute__((aligned(16)));
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[] __attribute__((aligned(16)));
extern unsigned int size_fileXio_irx;
extern unsigned char poweroff_irx[] __attribute__((aligned(16)));
extern unsigned int size_poweroff_irx;
extern unsigned char ps2dev9_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2hdd_irx;
extern unsigned char ps2fs_irx[] __attribute__((aligned(16)));
extern unsigned int size_ps2fs_irx;

static bool ps2StorageMassRootReady(void)
{
    DIR *dir = opendir("mass:/");
    if (!dir) {
        return false;
    }
    closedir(dir);
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

static bool ps2StoragePfsRootReady(void)
{
    const int fd = fileXioDopen("pfs0:/");
    if (fd < 0) {
        return false;
    }
    fileXioDclose(fd);
    return true;
}

s32 ps2StorageResetIopForCleanBoot(void)
{
    /*
     * A clean reboot invalidates launcher modules and RPC bindings. Rebuild
     * only the services selected by ps2StoragePrepareBootMedium() and the
     * runtime subsystems that follow it.
     */
    sceSifInitRpc(0);

    const uint64_t request_start = sysGetMicroseconds();
    while (!SifIopReset(NULL, 0)) {
        if (sysGetMicroseconds() - request_start >=
                PS2_STORAGE_IOP_RESET_REQUEST_TIMEOUT_USEC) {
            sysLogPrintf(LOG_ERROR,
                "IOP: reset request timeout limit=%u us",
                PS2_STORAGE_IOP_RESET_REQUEST_TIMEOUT_USEC);
            return -1;
        }
        DelayThread(PS2_STORAGE_IOP_RESET_RETRY_USEC);
    }

    const uint64_t sync_start = sysGetMicroseconds();
    while (!SifIopSync()) {
        if (sysGetMicroseconds() - sync_start >=
                PS2_STORAGE_IOP_RESET_SYNC_TIMEOUT_USEC) {
            sysLogPrintf(LOG_ERROR,
                "IOP: reset sync timeout limit=%u us",
                PS2_STORAGE_IOP_RESET_SYNC_TIMEOUT_USEC);
            return -2;
        }
        DelayThread(PS2_STORAGE_IOP_RESET_RETRY_USEC);
    }

    sceSifInitRpc(0);
    SifLoadFileInit();

    sysLogPrintf(LOG_NOTE,
        "IOP: clean reboot complete; inherited modules/RPC discarded");
    return 0;
}

static int ps2StorageExecEmbeddedModule(
    const char *name,
    unsigned char *image,
    unsigned int image_size,
    int args_len,
    const char *args)
{
    int module_result = 0;
    const int result = SifExecModuleBuffer(
        image, image_size, args_len, args, &module_result);

    if (result >= 0) {
        sysLogPrintf(LOG_NOTE,
            "STORAGE: executed embedded %s id=%d start_result=%d bytes=%u",
            name, result, module_result, image_size);
        return result;
    }

    sysLogPrintf(LOG_ERROR,
        "STORAGE: failed to execute embedded %s result=%d start_result=%d",
        name, result, module_result);
    return result;
}

static int ps2StorageExecEmbeddedModuleNoArgs(
    const char *name, unsigned char *image, unsigned int image_size)
{
    return ps2StorageExecEmbeddedModule(name, image, image_size, 0, NULL);
}

s32 ps2StorageEnsureMass(const char *boot_path)
{
    sceSifInitRpc(0);
    SifLoadFileInit();

    if (ps2StorageMassRootReady()) {
        sysLogPrintf(LOG_NOTE,
            "STORAGE: project-owned mass: service already ready boot_path=%s",
            boot_path ? boot_path : "(null)");
        return 0;
    }

    sysLogPrintf(LOG_NOTE,
        "STORAGE: boot medium USB; loading only USBD/USBHDFSD boot stack");

    sbv_patch_enable_lmb();

    const int usbd = ps2StorageExecEmbeddedModuleNoArgs(
        "USB_driver", usbd_irx, size_usbd_irx);
    if (usbd < 0) {
        return usbd;
    }

    const int usbhdfsd = ps2StorageExecEmbeddedModuleNoArgs(
        "usbhdfsd", usbhdfsd_irx, size_usbhdfsd_irx);
    if (usbhdfsd < 0) {
        return usbhdfsd;
    }

    const int ready = ps2StorageWaitForMass();
    if (ready < 0) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: USB boot device could not be restored after clean IOP reboot");
    }
    return ready;
}

static s32 ps2StorageEnsureHdd(const char *boot_path)
{
    char partition[96];
    char relative[768];

    if (!ps2PathParseHddBootContext(
            boot_path, partition, sizeof(partition),
            relative, sizeof(relative))) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: HDD boot detected but argv[0] has no recoverable APA partition context: %s",
            boot_path ? boot_path : "(null)");
        return -20;
    }

    sysLogPrintf(LOG_NOTE,
        "STORAGE: boot medium HDD partition=%s path=%s",
        partition, relative);

    /*
     * HDD/PFS uses the extended I/O manager and FILEXIO RPC stack. Keep these
     * modules strictly inside the HDD path so USB boots do not pay the IOP RAM
     * or startup-time cost.
     */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    int result = ps2StorageExecEmbeddedModuleNoArgs(
        "iomanX", iomanX_irx, size_iomanX_irx);
    if (result < 0) {
        return result;
    }

    result = ps2StorageExecEmbeddedModuleNoArgs(
        "fileXio", fileXio_irx, size_fileXio_irx);
    if (result < 0) {
        return result;
    }

    result = fileXioInit();
    if (result < 0) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: fileXioInit failed result=%d", result);
        return result;
    }

    result = ps2StorageExecEmbeddedModuleNoArgs(
        "poweroff", poweroff_irx, size_poweroff_irx);
    if (result < 0) {
        return result;
    }

    result = ps2StorageExecEmbeddedModuleNoArgs(
        "ps2dev9", ps2dev9_irx, size_ps2dev9_irx);
    if (result < 0) {
        return result;
    }

    result = ps2StorageExecEmbeddedModuleNoArgs(
        "ps2atad", ps2atad_irx, size_ps2atad_irx);
    if (result < 0) {
        return result;
    }

    static const char hdd_args[] =
        "-o\0"
        "4\0"
        "-n\0"
        "20";
    result = ps2StorageExecEmbeddedModule(
        "ps2hdd", ps2hdd_irx, size_ps2hdd_irx,
        sizeof(hdd_args), hdd_args);
    if (result < 0) {
        return result;
    }

    static const char pfs_args[] =
        "-m\0"
        "4\0"
        "-o\0"
        "10\0"
        "-n\0"
        "40";
    result = ps2StorageExecEmbeddedModule(
        "ps2fs", ps2fs_irx, size_ps2fs_irx,
        sizeof(pfs_args), pfs_args);
    if (result < 0) {
        return result;
    }

    result = fileXioMount("pfs0:", partition, FIO_MT_RDWR);
    if (result < 0) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: failed to mount %s as pfs0: result=%d",
            partition, result);
        return result;
    }

    if (!ps2StoragePfsRootReady()) {
        sysLogPrintf(LOG_ERROR,
            "STORAGE: pfs0: mount returned success but root is not readable");
        return -21;
    }

    sysLogPrintf(LOG_NOTE,
        "STORAGE: HDD/PFS boot stack ready partition=%s", partition);
    return 0;
}

s32 ps2StoragePrepareBootMedium(const char *boot_path)
{
    const enum Ps2BootMedium medium = ps2PathClassifyBootMedium(boot_path);

    sysLogPrintf(LOG_NOTE,
        "STORAGE: detected boot medium=%s argv0=%s",
        ps2PathBootMediumName(medium),
        boot_path ? boot_path : "(null)");

    switch (medium) {
        case PS2_BOOT_MEDIUM_USB:
            return ps2StorageEnsureMass(boot_path);

        case PS2_BOOT_MEDIUM_HDD:
            return ps2StorageEnsureHdd(boot_path);

        /*
         * The full game boots from USB or HDD. Memory-card services are
         * requested only by explicit mc0:/mc1: file access. Reject other
         * boot paths before fsInit tries to read the ROM from an absent stack.
         */
        case PS2_BOOT_MEDIUM_MC:
        case PS2_BOOT_MEDIUM_CDVD:
        case PS2_BOOT_MEDIUM_HOST:
        case PS2_BOOT_MEDIUM_NETWORK:
        case PS2_BOOT_MEDIUM_ROM:
        case PS2_BOOT_MEDIUM_UNKNOWN:
        default:
            sysLogPrintf(LOG_ERROR,
                "STORAGE: unsupported game boot medium=%s",
                ps2PathBootMediumName(medium));
            return -22;
    }
}
