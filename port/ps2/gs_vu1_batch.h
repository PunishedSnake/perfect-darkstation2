#ifndef PERFECT_DARK_PS2_GS_VU1_BATCH_H
#define PERFECT_DARK_PS2_GS_VU1_BATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VU1 local-memory contract for the first PATH1 transport bring-up.
 *
 * The lower half is split into two 256-QW VIF1 double-buffer banks. Each bank
 * contains one complete GIF PACKED A+D packet: one GIF tag, an optional PRIM
 * write and two final register records per color vertex. The 96-vertex policy
 * matches gfx_ps2's current translation batch and leaves 62 QW free per bank.
 */
#define PS2_GS_VU1_BUFFER_QW 256u
#define PS2_GS_VU1_BUFFER_BASE_QW 0u
#define PS2_GS_VU1_BUFFER_OFFSET_QW 256u
#define PS2_GS_VU1_MAX_COLOR_VERTICES 96u
#define PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW 3u
#define PS2_GS_VU1_DMA_SLOT_QW \
    (PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW)

struct Ps2GsVu1ColorBatchLayout {
    uint32_t register_count;
    uint32_t gif_packet_qw;
    uint32_t dma_chain_qw;
};

bool ps2GsVu1PlanColorBatch(uint32_t vertex_count, bool emit_prim,
    struct Ps2GsVu1ColorBatchLayout *layout);

/*
 * Build one source-chain VIF1 transfer. Tag-transfer VIF codes use V4-32
 * UNPACK into TOPS and execute microprogram address zero. `payload_dma_address`
 * is the canonical EE address of the GIF packet stored after the three chain
 * tags. FLUSHA/FLUSH are deliberate validation-mode ordering barriers; they
 * are not the final hot path scheduling policy.
 */
bool ps2GsVu1BuildColorBatchStream(uint32_t *destination,
    uint32_t capacity_qw, uint32_t payload_dma_address,
    const struct Ps2GsPackedReg *prim, bool emit_prim,
    const struct Ps2GsColorVertex *vertices, uint32_t vertex_count,
    struct Ps2GsVu1ColorBatchLayout *layout);

#ifdef __cplusplus
}
#endif

#endif
