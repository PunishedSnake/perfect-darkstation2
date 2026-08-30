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
    struct Ps2GsVu1ColorBatchLayout layout = {};
    assert(!ps2GsVu1PlanColorBatch(0u, false, &layout));
    assert(!ps2GsVu1PlanColorBatch(4u, false, &layout));
    assert(!ps2GsVu1PlanColorBatch(99u, false, &layout));
    assert(ps2GsVu1PlanColorBatch(96u, true, &layout));
    assert(layout.register_count == 193u);
    assert(layout.gif_packet_qw == 194u);
    assert(layout.dma_chain_qw == 197u);

    struct Ps2GsColorVertex vertices[3] = {};
    for (uint32_t i = 0; i < 3u; ++i) {
        vertices[i].rgbaq = makeReg(0x1000u + i, 0x01u);
        vertices[i].xyz2 = makeReg(0x2000u + i, 0x05u);
    }
    const struct Ps2GsPackedReg prim = makeReg(0x440u, 0x00u);
    uint32_t stream[PS2_GS_VU1_DMA_SLOT_QW * 4u] = {};
    const uint32_t payload_address = 0x00102030u;
    assert(ps2GsVu1BuildColorBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address, &prim, true,
        vertices, 3u, &layout));

    assert(layout.register_count == 7u);
    assert(layout.gif_packet_qw == 8u);
    assert(layout.dma_chain_qw == 11u);
    assert(stream[0] == 0x30000008u);
    assert(stream[1] == payload_address);
    assert(stream[2] == 0x01000101u);
    assert(stream[3] == 0x6c08c000u);
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
    assert(gif_tag == 0x1000000000008007ull);
    assert(gif_reg == 0x0eull);

    struct Ps2GsPackedReg copied_prim = {};
    memcpy(&copied_prim, &stream[16], sizeof(copied_prim));
    assert(copied_prim.value == prim.value);
    assert(copied_prim.reg == prim.reg);

    struct Ps2GsColorVertex copied_vertices[3] = {};
    memcpy(copied_vertices, &stream[20], sizeof(copied_vertices));
    assert(memcmp(copied_vertices, vertices, sizeof(vertices)) == 0);

    assert(!ps2GsVu1BuildColorBatchStream(
        stream, layout.dma_chain_qw - 1u, payload_address, &prim, true,
        vertices, 3u, NULL));
    assert(!ps2GsVu1BuildColorBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address, NULL, true,
        vertices, 3u, NULL));
    assert(!ps2GsVu1BuildColorBatchStream(
        stream, PS2_GS_VU1_DMA_SLOT_QW, payload_address + 4u, &prim, true,
        vertices, 3u, NULL));

    return 0;
}
