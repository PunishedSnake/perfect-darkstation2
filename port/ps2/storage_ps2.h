#ifndef PERFECT_DARK_PS2_STORAGE_PS2_H
#define PERFECT_DARK_PS2_STORAGE_PS2_H

#include <PR/ultratypes.h>
#include "path_ps2.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diagnostic clean-IOP bootstrap. This is a system-personality change: all
 * inherited IOP module/RPC state is discarded and must be reconstructed.
 */
s32 ps2StorageResetIopForCleanBoot(void);

/*
 * Restore only the storage backend required by the executable's boot medium.
 * This is called after the clean IOP reset, so no launcher-owned device stack
 * is assumed to survive.
 */
s32 ps2StoragePrepareBootMedium(const char *boot_path);

/* USB backend used by the boot-medium dispatcher. */
s32 ps2StorageEnsureMass(const char *boot_path);

#ifdef __cplusplus
}
#endif

#endif
