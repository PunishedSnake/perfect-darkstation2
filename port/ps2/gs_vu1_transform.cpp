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
