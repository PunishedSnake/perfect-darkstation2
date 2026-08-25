#ifndef PERFECT_DARK_PS2_GS_NATIVE_QUEUE_H
#define PERFECT_DARK_PS2_GS_NATIVE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Project-owned PATH3 command arena.
 *
 * The current first implementation emits contiguous GIF PACKED A+D streams
 * through dmaKit. It deliberately does not append GS FINISH. Double buffering
 * lets the EE build the next arena without overwriting a buffer still owned by
 * GIF DMA. Buffer sizing is a measured policy knob, not part of the API.
 */
bool ps2GsNativeQueueInit(uint32_t qwords_per_buffer);
void ps2GsNativeQueueBeginFrame(void);

/*
 * Reserve one GIF PACKED A+D packet containing exactly reg_count writes.
 * Returned storage is packet-ready and 16-byte stepped; callers fill the
 * project-owned value/register records directly with no later repack/copy.
 */
struct Ps2GsPackedReg *ps2GsNativeQueueReserveAd(uint32_t reg_count);

/* Submit the active arena to GIF DMA. This never waits for GS FINISH/VSync. */
bool ps2GsNativeQueueSubmit(void);

uint32_t ps2GsNativeQueueUsedQwords(void);
uint32_t ps2GsNativeQueueCapacityQwords(void);
bool ps2GsNativeQueueOverflowed(void);

#ifdef __cplusplus
}
#endif

#endif
