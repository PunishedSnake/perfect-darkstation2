#ifndef PERFECT_DARK_PS2_MEMORY_CARD_PS2_H
#define PERFECT_DARK_PS2_MEMORY_CARD_PS2_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lazily provide the project-owned memory-card service.
 *
 * This is deliberately separate from boot-medium selection. A USB/HDD boot
 * should not load MCMAN/MCSERV merely because the executable came from that
 * device. SIO2MAN is shared with PAD and is reused when already resident.
 *
 * Current service dependency:
 *   SIO2MAN -> MCMAN -> MCSERV -> libmc mcInit()
 *
 * Returns 0 on success, < 0 on failure.
 */
s32 ps2MemoryCardEnsureService(void);

#ifdef __cplusplus
}
#endif

#endif
