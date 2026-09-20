#ifndef PERFECT_DARK_PS2_STORAGE_PS2_H
#define PERFECT_DARK_PS2_STORAGE_PS2_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diagnostic clean-IOP bootstrap. This is a system-personality change: all
 * inherited IOP module/RPC state is discarded and must be reconstructed.
 */
s32 ps2StorageResetIopForCleanBoot(void);

/*
 * Ensure mass: is usable. A resident launcher stack is reused when present.
 * After a clean IOP reset this naturally falls back to the project-owned
 * current-PS2SDK USBD/USBHDFSD pair and bounded enumeration.
 */
s32 ps2StorageEnsureMass(const char *boot_path);

#ifdef __cplusplus
}
#endif

#endif
