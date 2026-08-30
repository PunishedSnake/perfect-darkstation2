#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gs_vu1_batch.h"

static struct Ps2GsPackedReg makeReg(uint64_t value, uint64_t reg)
{
    const struct Ps2GsPackedReg result = { value, reg };
    return result;
}

int main(void)
{
    struct Ps2GsVu1BatchLayout layout = {};
    assert(!ps2GsVu1PlanAdBatch(0u, &layout));
    assert(!ps2GsVu1PlanAdBatch(256u, &layout));
    assert(ps2GsVu1PlanAdBatch(193u, &layout));
    assert(layout.register_count == 193u);
    assert(layout.gif_packet_qw == 194u);
    assert(layout.dma_chain_qw == 197u);
    assert(ps2GsVu1PlanAdBatch(249u, &layout));
    assert(layout.gif_packet_qw == 250u);
    assert(layout.dma_chain_qw == 253u);
    assert(ps2GsVu1PlanAdBatch(255u, &layout));
    assert(layout.gif_packet_qw == PS2_GS_VU1_BUFFER_QW);
    assert(layout.dma_chain_qw == PS2_GS_VU1_DMA_SLOT_QW);

    struct Ps2GsColorVertex vertices[3] = {};
    for (uint32_t i = 0; i < 3u; ++i) {
        vertices[i].rgbaq = makeReg(0x1000u + i, 0x01u);
        vertices[i].xyz2 = makeReg(0x2000u + i, 0x05u);
    }
    const struct Ps2GsPackedReg prim = makeReg(0x440u, 0x00u);
    const struct Ps2GsPackedReg restore = makeReg(0x550u, 0x08u);
    uint32_t stream[PS2_GS_VU1_DMA_SLOT_QW * 4u] = {};
    const uint32_t payload_address = 0x00102030u;
    assert(ps2GsVu1BuildAdBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address,
        &prim, 1u, &vertices[0].rgbaq, 6u, &restore, 1u, &layout));

    assert(layout.register_count == 8u);
    assert(layout.gif_packet_qw == 9u);
    assert(layout.dma_chain_qw == 12u);
    assert(stream[0] == 0x30000009u);
    assert(stream[1] == payload_address);
    assert(stream[2] == 0x01000101u);
    assert(stream[3] == 0x6c09c000u);
    assert(stream[4] == 0x10000000u);
    assert(stream[5] == 0u);
    assert(stream[6] == 0x13000000u);
    assert(stream[7] == 0x14000000u);
    assert(stream[8] == 0x70000000u);
    assert(stream[9] == 0u);
    assert(stream[10] == 0x11000000u);
    assert(stream[11] == 0u);

    uint64_t gif_tag = 0u;
    uint64_t gif_reg = 0u;
    memcpy(&gif_tag, &stream[12], sizeof(gif_tag));
    memcpy(&gif_reg, &stream[14], sizeof(gif_reg));
    assert(gif_tag == 0x1000000000008008ull);
    assert(gif_reg == 0x0eull);

    struct Ps2GsPackedReg copied_prim = {};
    memcpy(&copied_prim, &stream[16], sizeof(copied_prim));
    assert(copied_prim.value == prim.value);
    assert(copied_prim.reg == prim.reg);

    struct Ps2GsColorVertex copied_vertices[3] = {};
    memcpy(copied_vertices, &stream[20], sizeof(copied_vertices));
    assert(memcmp(copied_vertices, vertices, sizeof(vertices)) == 0);

    struct Ps2GsPackedReg copied_restore = {};
    memcpy(&copied_restore, &stream[44], sizeof(copied_restore));
    assert(copied_restore.value == restore.value);
    assert(copied_restore.reg == restore.reg);

    assert(!ps2GsVu1BuildAdBatchStream(
        stream, layout.dma_chain_qw - 1u, payload_address, &prim, 1u,
        &vertices[0].rgbaq, 6u, &restore, 1u, NULL));
    assert(!ps2GsVu1BuildAdBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address,
        NULL, 1u, &vertices[0].rgbaq, 6u, NULL, 0u, NULL));
    assert(!ps2GsVu1BuildAdBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address + 4u,
        &prim, 1u, &vertices[0].rgbaq, 6u, NULL, 0u, NULL));

    struct Ps2GsPackedReg textured_prefix[5] = {};
    struct Ps2GsPackedReg textured_records[
        PS2_GS_VU1_MAX_TEXTURED_VERTICES * 3u] = {};
    const struct Ps2GsPackedReg textured_suffix =
        makeReg(0xfeedu, 0x08u);
    for (uint32_t i = 0u; i < 5u; ++i) {
        textured_prefix[i] = makeReg(0x3000u + i, 0x10u + i);
    }
    for (uint32_t i = 0u;
         i < PS2_GS_VU1_MAX_TEXTURED_VERTICES * 3u; ++i) {
        textured_records[i] = makeReg(0x4000u + i, i % 3u);
    }
    assert(ps2GsVu1BuildAdBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address,
        textured_prefix, 5u,
        textured_records, PS2_GS_VU1_MAX_TEXTURED_VERTICES * 3u,
        &textured_suffix, 1u, &layout));
    assert(layout.register_count == 249u);
    assert(layout.gif_packet_qw == 250u);
    assert(layout.dma_chain_qw == 253u);

    struct Ps2GsPackedReg copied_prefix[5] = {};
    memcpy(copied_prefix, &stream[16], sizeof(copied_prefix));
    assert(memcmp(copied_prefix, textured_prefix,
        sizeof(textured_prefix)) == 0);
    struct Ps2GsPackedReg copied_textured_records[
        PS2_GS_VU1_MAX_TEXTURED_VERTICES * 3u] = {};
    memcpy(copied_textured_records, &stream[36],
        sizeof(copied_textured_records));
    assert(memcmp(copied_textured_records, textured_records,
        sizeof(textured_records)) == 0);
    struct Ps2GsPackedReg copied_textured_suffix = {};
    memcpy(&copied_textured_suffix, &stream[1008],
        sizeof(copied_textured_suffix));
    assert(copied_textured_suffix.value == textured_suffix.value);
    assert(copied_textured_suffix.reg == textured_suffix.reg);

    return 0;
}
