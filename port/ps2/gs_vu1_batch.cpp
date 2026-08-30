#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gs_vu1_batch.h"

#define PS2_VIF_CMD_NOP       0x00u
#define PS2_VIF_CMD_STCYCL    0x01u
#define PS2_VIF_CMD_FLUSH     0x11u
#define PS2_VIF_CMD_FLUSHA    0x13u
#define PS2_VIF_CMD_MSCAL     0x14u
#define PS2_VIF_CMD_UNPACK_V4_32 0x6cu

#define PS2_VIF_UNPACK_UNSIGNED (1u << 14)
#define PS2_VIF_UNPACK_TOPS     (1u << 15)

#define PS2_GIF_PACKED 0u
#define PS2_GIF_REG_AD 0x0eu

#define PS2_DMA_TAG_CNT 0x01u
#define PS2_DMA_TAG_REF 0x03u
#define PS2_DMA_TAG_END 0x07u

static uint32_t ps2GsVu1VifCode(
    uint32_t immediate, uint32_t num, uint32_t command)
{
    return (immediate & 0xffffu) |
        ((num & 0xffu) << 16) |
        ((command & 0x7fu) << 24);
}

static uint64_t ps2GsVu1GifPackedAdTag(uint32_t register_count)
{
    return ((uint64_t)register_count << 0) |
        ((uint64_t)1u << 15) |       /* EOP */
        ((uint64_t)PS2_GIF_PACKED << 58) |
        ((uint64_t)1u << 60);        /* NREG = one A+D descriptor */
}

static uint64_t ps2GsVu1DmaTag(
    uint32_t qwords, uint32_t id, uint32_t address)
{
    return ((uint64_t)(qwords & 0xffffu) << 0) |
        ((uint64_t)(id & 0x7u) << 28) |
        ((uint64_t)(address & 0x7fffffffu) << 32);
}

extern "C" bool ps2GsVu1PlanAdBatch(uint32_t register_count,
    struct Ps2GsVu1BatchLayout *layout)
{
    if (!layout || register_count == 0u ||
        register_count > PS2_GS_VU1_MAX_AD_REGISTERS) {
        return false;
    }

    const uint32_t gif_packet_qw = register_count + 1u;
    if (gif_packet_qw > PS2_GS_VU1_BUFFER_QW) {
        return false;
    }

    layout->register_count = register_count;
    layout->gif_packet_qw = gif_packet_qw;
    layout->dma_chain_qw =
        gif_packet_qw + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW;
    return true;
}

extern "C" bool ps2GsVu1BuildAdBatchStream(uint32_t *destination,
    uint32_t capacity_qw, uint32_t payload_dma_address,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsPackedReg *records, uint32_t record_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1BatchLayout *layout)
{
    struct Ps2GsVu1BatchLayout planned;
    if (!destination || !records || record_count == 0u ||
        (prefix_count != 0u && !prefix) ||
        (suffix_count != 0u && !suffix) ||
        prefix_count > UINT32_MAX - record_count ||
        suffix_count > UINT32_MAX - prefix_count - record_count ||
        !ps2GsVu1PlanAdBatch(
            prefix_count + record_count + suffix_count, &planned) ||
        capacity_qw < planned.dma_chain_qw ||
        (payload_dma_address & 0x0fu) != 0u) {
        return false;
    }

    memset(destination, 0, (size_t)planned.dma_chain_qw * 16u);

    const uint64_t ref_tag = ps2GsVu1DmaTag(
        planned.gif_packet_qw, PS2_DMA_TAG_REF, payload_dma_address);
    memcpy(&destination[0], &ref_tag, sizeof(ref_tag));
    destination[2] = ps2GsVu1VifCode(0x0101u, 0u, PS2_VIF_CMD_STCYCL);
    destination[3] = ps2GsVu1VifCode(
        PS2_VIF_UNPACK_UNSIGNED | PS2_VIF_UNPACK_TOPS,
        planned.gif_packet_qw,
        PS2_VIF_CMD_UNPACK_V4_32);

    const uint64_t cnt_tag = ps2GsVu1DmaTag(0u, PS2_DMA_TAG_CNT, 0u);
    memcpy(&destination[4], &cnt_tag, sizeof(cnt_tag));
    destination[6] = ps2GsVu1VifCode(0u, 0u, PS2_VIF_CMD_FLUSHA);
    destination[7] = ps2GsVu1VifCode(0u, 0u, PS2_VIF_CMD_MSCAL);

    const uint64_t end_tag = ps2GsVu1DmaTag(0u, PS2_DMA_TAG_END, 0u);
    memcpy(&destination[8], &end_tag, sizeof(end_tag));
    destination[10] = ps2GsVu1VifCode(0u, 0u, PS2_VIF_CMD_FLUSH);
    destination[11] = ps2GsVu1VifCode(0u, 0u, PS2_VIF_CMD_NOP);

    uint8_t *payload = (uint8_t *)&destination[
        PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW * 4u];
    const uint64_t gif_tag =
        ps2GsVu1GifPackedAdTag(planned.register_count);
    const uint64_t gif_reg = PS2_GIF_REG_AD;
    memcpy(payload, &gif_tag, sizeof(gif_tag));
    memcpy(payload + sizeof(gif_tag), &gif_reg, sizeof(gif_reg));
    payload += 16u;

    if (prefix_count != 0u) {
        memcpy(payload, prefix, (size_t)prefix_count * sizeof(*prefix));
        payload += (size_t)prefix_count * sizeof(*prefix);
    }
    memcpy(payload, records, (size_t)record_count * sizeof(*records));
    payload += (size_t)record_count * sizeof(*records);
    if (suffix_count != 0u) {
        memcpy(payload, suffix, (size_t)suffix_count * sizeof(*suffix));
    }

    if (layout) {
        *layout = planned;
    }
    return true;
}
