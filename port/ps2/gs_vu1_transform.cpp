#include <stddef.h>
#include <string.h>

#include "gs_vu1_transform.h"

#define PS2_GIF_PACKED 0u
#define PS2_GIF_REG_RGBAQ 0x01u
#define PS2_GIF_REG_ST 0x02u
#define PS2_GIF_REG_XYZF2 0x04u
#define PS2_GIF_REG_XYZ2 0x05u
#define PS2_GIF_REG_AD 0x0eu
#define PS2_GS_REG_NOP 0x0fu

#define PS2_VIF_CMD_NOP          0x00u
#define PS2_VIF_CMD_STCYCL       0x01u
#define PS2_VIF_CMD_FLUSH        0x11u
#define PS2_VIF_CMD_FLUSHA       0x13u
#define PS2_VIF_CMD_MSCAL        0x14u
#define PS2_VIF_CMD_UNPACK_V4_32 0x6cu

#define PS2_VIF_UNPACK_UNSIGNED (1u << 14)
#define PS2_VIF_UNPACK_TOPS     (1u << 15)

#define PS2_DMA_TAG_CNT 0x01u
#define PS2_DMA_TAG_REF 0x03u
#define PS2_DMA_TAG_END 0x07u

struct Ps2GsVu1GifTagQw {
    uint64_t tag;
    uint64_t registers;
};

struct Ps2GsVu1TransformHeader {
    uint32_t control[4];
    float scale[4];
    float offset[4];
    struct Ps2GsVu1GifTagQw prefix_tag;
    struct Ps2GsVu1GifTagQw vertex_tag;
    struct Ps2GsVu1GifTagQw suffix_tag;
};

static_assert(sizeof(struct Ps2GsVu1TransformVertex) ==
        PS2_GS_VU1_TRANSFORM_VERTEX_QW * 16u,
    "VU1 transform vertices must occupy exactly three quadwords");
static_assert(sizeof(struct Ps2GsVu1TransformHeader) ==
        PS2_GS_VU1_TRANSFORM_HEADER_QW * 16u,
    "VU1 transform header must occupy exactly six quadwords");

static uint64_t ps2GsVu1TransformGifTag(
    uint32_t loops, bool eop, uint32_t register_count)
{
    return ((uint64_t)loops << 0) |
        ((uint64_t)(eop ? 1u : 0u) << 15) |
        ((uint64_t)PS2_GIF_PACKED << 58) |
        ((uint64_t)register_count << 60);
}

static uint32_t ps2GsVu1TransformVifCode(
    uint32_t immediate, uint32_t num, uint32_t command)
{
    return (immediate & 0xffffu) |
        ((num & 0xffu) << 16) |
        ((command & 0x7fu) << 24);
}

static uint64_t ps2GsVu1TransformDmaTag(
    uint32_t qwords, uint32_t id, uint32_t address)
{
    return ((uint64_t)(qwords & 0xffffu) << 0) |
        ((uint64_t)(id & 0x7u) << 28) |
        ((uint64_t)(address & 0x7fffffffu) << 32);
}

extern "C" bool ps2GsVu1BuildViewportMapping(
    int32_t viewport_x, int32_t viewport_y,
    int32_t viewport_width, int32_t viewport_height,
    int32_t gs_offset_x, int32_t gs_offset_y,
    float depth_near, float depth_far,
    float scale[4], float offset[4])
{
    if (!scale || !offset || viewport_width <= 0 || viewport_height <= 0 ||
        !(depth_near >= 0.0f && depth_near <= 1.0f) ||
        !(depth_far >= 0.0f && depth_far <= 1.0f)) {
        return false;
    }

    scale[0] = (float)viewport_width * 0.5f;
    scale[1] = (float)viewport_height * -0.5f;
    scale[2] = -65534.0f * (depth_far - depth_near);
    /* VU1 clamps screen-space XY against scale.w before FTOI4. */
    scale[3] = 0.0f;

    offset[0] = (float)viewport_x +
        (float)viewport_width * 0.5f +
        (float)gs_offset_x * (1.0f / 16.0f);
    offset[1] = (float)viewport_y +
        (float)viewport_height * 0.5f +
        (float)gs_offset_y * (1.0f / 16.0f);
    offset[2] = 65535.0f - 65534.0f * depth_near;
    /* Largest 12.4 value which fits the GS packed X/Y lanes. */
    offset[3] = 4095.9375f;
    return true;
}

extern "C" bool ps2GsVu1PlanTexturedTransform(uint32_t vertex_count,
    uint32_t prefix_count, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout)
{
    if (!layout || vertex_count == 0u || vertex_count % 3u != 0u ||
        vertex_count > PS2_GS_VU1_MAX_TEXTURED_VERTICES ||
        prefix_count > PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS ||
        suffix_count > PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS) {
        return false;
    }

    /*
     * Prefix and suffix storage is deliberately fixed. The microprogram can
     * copy one branch-free stream while unused A+D records target GS NOP.
     */
    const uint32_t input_qw = PS2_GS_VU1_TRANSFORM_HEADER_QW +
        PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS +
        vertex_count * PS2_GS_VU1_TRANSFORM_VERTEX_QW +
        PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS;
    const uint32_t output_qw =
        1u + PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS +
        1u + vertex_count * PS2_GS_VU1_TRANSFORM_VERTEX_QW +
        1u + PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS;
    if (input_qw > PS2_GS_VU1_BUFFER_QW ||
        output_qw > PS2_GS_VU1_BUFFER_QW) {
        return false;
    }

    layout->vertex_count = vertex_count;
    layout->prefix_count = prefix_count;
    layout->suffix_count = suffix_count;
    layout->input_qw = input_qw;
    layout->output_qw = output_qw;
    layout->dma_chain_qw =
        input_qw + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW;
    return true;
}

extern "C" bool ps2GsVu1BuildTexturedTransformPayload(
    uint32_t *destination, uint32_t capacity_qw,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsVu1TransformVertex *vertices, uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout)
{
    struct Ps2GsVu1TransformLayout planned;
    if (!destination || !scale || !offset || !vertices ||
        (flags & ~PS2_GS_VU1_TRANSFORM_FLAG_FOG) != 0u ||
        (prefix_count != 0u && !prefix) ||
        (suffix_count != 0u && !suffix) ||
        !ps2GsVu1PlanTexturedTransform(
            vertex_count, prefix_count, suffix_count, &planned) ||
        capacity_qw < planned.input_qw) {
        return false;
    }

    memset(destination, 0, (size_t)planned.input_qw * 16u);
    struct Ps2GsVu1TransformHeader header = {};
    header.control[0] = vertex_count;
    header.control[1] = prefix_count;
    header.control[2] = suffix_count;
    header.control[3] = flags;
    memcpy(header.scale, scale, sizeof(header.scale));
    memcpy(header.offset, offset, sizeof(header.offset));

    header.prefix_tag.tag = ps2GsVu1TransformGifTag(
        PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS, false, 1u);
    header.prefix_tag.registers = PS2_GIF_REG_AD;
    header.vertex_tag.tag = ps2GsVu1TransformGifTag(
        vertex_count, false, 3u);
    header.vertex_tag.registers =
        ((uint64_t)PS2_GIF_REG_ST << 0) |
        ((uint64_t)PS2_GIF_REG_RGBAQ << 4) |
        ((uint64_t)((flags & PS2_GS_VU1_TRANSFORM_FLAG_FOG) != 0u
            ? PS2_GIF_REG_XYZF2 : PS2_GIF_REG_XYZ2) << 8);
    header.suffix_tag.tag = ps2GsVu1TransformGifTag(
        PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS, true, 1u);
    header.suffix_tag.registers = PS2_GIF_REG_AD;

    uint8_t *cursor = (uint8_t *)destination;
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    struct Ps2GsPackedReg padded_prefix[
        PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS] = {};
    for (uint32_t i = 0u;
         i < PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS; ++i) {
        padded_prefix[i].reg = PS2_GS_REG_NOP;
    }
    if (prefix_count != 0u) {
        memcpy(padded_prefix, prefix,
            (size_t)prefix_count * sizeof(*prefix));
    }
    memcpy(cursor, padded_prefix, sizeof(padded_prefix));
    cursor += sizeof(padded_prefix);

    memcpy(cursor, vertices, (size_t)vertex_count * sizeof(*vertices));
    cursor += (size_t)vertex_count * sizeof(*vertices);

    struct Ps2GsPackedReg padded_suffix = { 0u, PS2_GS_REG_NOP };
    if (suffix_count != 0u) {
        padded_suffix = suffix[0];
    }
    memcpy(cursor, &padded_suffix, sizeof(padded_suffix));

    if (layout) {
        *layout = planned;
    }
    return true;
}

extern "C" bool ps2GsVu1BuildTexturedTransformStream(
    uint32_t *destination, uint32_t capacity_qw,
    uint32_t payload_dma_address,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsVu1TransformVertex *vertices, uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout)
{
    struct Ps2GsVu1TransformLayout planned;
    if (!destination || (payload_dma_address & 0x0fu) != 0u ||
        !ps2GsVu1PlanTexturedTransform(
            vertex_count, prefix_count, suffix_count, &planned) ||
        capacity_qw < planned.dma_chain_qw) {
        return false;
    }

    memset(destination, 0, (size_t)planned.dma_chain_qw * 16u);

    const uint64_t ref_tag = ps2GsVu1TransformDmaTag(
        planned.input_qw, PS2_DMA_TAG_REF, payload_dma_address);
    memcpy(&destination[0], &ref_tag, sizeof(ref_tag));
    destination[2] = ps2GsVu1TransformVifCode(
        0x0101u, 0u, PS2_VIF_CMD_STCYCL);
    destination[3] = ps2GsVu1TransformVifCode(
        PS2_VIF_UNPACK_UNSIGNED | PS2_VIF_UNPACK_TOPS,
        planned.input_qw, PS2_VIF_CMD_UNPACK_V4_32);

    const uint64_t cnt_tag = ps2GsVu1TransformDmaTag(
        0u, PS2_DMA_TAG_CNT, 0u);
    memcpy(&destination[4], &cnt_tag, sizeof(cnt_tag));
    destination[6] = ps2GsVu1TransformVifCode(
        0u, 0u, PS2_VIF_CMD_FLUSHA);
    destination[7] = ps2GsVu1TransformVifCode(
        PS2_GS_VU1_TRANSFORM_PROGRAM_ADDRESS, 0u, PS2_VIF_CMD_MSCAL);

    const uint64_t end_tag = ps2GsVu1TransformDmaTag(
        0u, PS2_DMA_TAG_END, 0u);
    memcpy(&destination[8], &end_tag, sizeof(end_tag));
    destination[10] = ps2GsVu1TransformVifCode(
        0u, 0u, PS2_VIF_CMD_FLUSH);
    destination[11] = ps2GsVu1TransformVifCode(
        0u, 0u, PS2_VIF_CMD_NOP);

    if (!ps2GsVu1BuildTexturedTransformPayload(
            &destination[PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW * 4u],
            planned.input_qw, scale, offset, flags,
            prefix, prefix_count, vertices, vertex_count,
            suffix, suffix_count, NULL)) {
        return false;
    }

    if (layout) {
        *layout = planned;
    }
    return true;
}
