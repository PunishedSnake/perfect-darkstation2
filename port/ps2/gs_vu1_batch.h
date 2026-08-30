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
 * contains one complete GIF PACKED A+D packet: one GIF tag followed by up to
 * 255 final register records. This covers 96 color vertices or 81 textured
 * vertices plus their draw-local state without crossing a bank boundary.
 */
#define PS2_GS_VU1_BUFFER_QW 256u
#define PS2_GS_VU1_BUFFER_BASE_QW 0u
#define PS2_GS_VU1_BUFFER_OFFSET_QW 256u
#define PS2_GS_VU1_MAX_AD_REGISTERS 255u
#define PS2_GS_VU1_MAX_COLOR_VERTICES 96u
#define PS2_GS_VU1_MAX_TEXTURED_VERTICES 81u
#define PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW 3u
#define PS2_GS_VU1_DMA_SLOT_QW \
    (PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW)

struct Ps2GsVu1BatchLayout {
    uint32_t register_count;
    uint32_t gif_packet_qw;
    uint32_t dma_chain_qw;
};

bool ps2GsVu1PlanAdBatch(uint32_t register_count,
    struct Ps2GsVu1BatchLayout *layout);

/*
 * Build one source-chain VIF1 transfer. Tag-transfer VIF codes use V4-32
 * UNPACK into TOPS and execute microprogram address zero. `payload_dma_address`
 * is the canonical EE address of the GIF packet stored after the three chain
 * tags. FLUSHA/FLUSH are deliberate validation-mode ordering barriers; they
 * are not the final hot path scheduling policy.
 */
bool ps2GsVu1BuildAdBatchStream(uint32_t *destination,
    uint32_t capacity_qw, uint32_t payload_dma_address,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsPackedReg *records, uint32_t record_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1BatchLayout *layout);

#ifdef __cplusplus
}
#endif

#endif
