#ifndef PERFECT_DARK_PS2_GS_VU1_QUEUE_H
#define PERFECT_DARK_PS2_GS_VU1_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional hardware-validation transport. Disabled builds remain PATH3-only. */
bool ps2GsVu1QueueInit(void);
bool ps2GsVu1QueueEnabled(void);

/*
 * Complete a pending validation batch before a later PATH3 claimant may alter
 * GS state. With the current trailing VIF FLUSH, VIF1 DMA completion also
 * proves that the microprogram and its PATH1 packet have drained.
 */
bool ps2GsVu1QueueWaitIdle(void);

bool ps2GsVu1QueueSubmitColor(
    const struct Ps2GsPackedReg *prim, bool emit_prim,
    const struct Ps2GsColorVertex *vertices, uint32_t vertex_count);

#ifdef __cplusplus
}
#endif

#endif
