#ifndef PERFECT_DARK_PS2_STORAGE_PS2_H
#define PERFECT_DARK_PS2_STORAGE_PS2_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ensure mass: is usable without resetting the IOP. A resident launcher stack
 * is reused. If it is absent, current PS2SDK USBD and USBHDFSD are started from
 * embedded IRX images and enumeration is bounded.
 */
s32 ps2StorageEnsureMass(const char *boot_path);

#ifdef __cplusplus
}
#endif

#endif
