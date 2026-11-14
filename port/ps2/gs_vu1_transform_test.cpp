#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <limits>

#include "gs_vu1_transform.h"

static struct Ps2GsPackedReg makeReg(uint64_t value, uint64_t reg)
{
    const struct Ps2GsPackedReg result = { value, reg };
    return result;
}

static bool test_object_plan_limits(void)
{
    struct Ps2GsVu1TransformLayout layout;
    struct Ps2GsVu1TransformLayout textured;
    if (!ps2GsVu1PlanObjectTransform(PS2_GS_VU1_OBJECT_MAX_VERTICES,
            PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS,
            PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS, &layout) ||
        layout.input_qw > PS2_GS_VU1_BUFFER_QW ||
        layout.output_qw > PS2_GS_VU1_BUFFER_QW ||
        !ps2GsVu1PlanTexturedTransform(3u, 0u, 0u, &textured) ||
        ps2GsVu1PlanObjectTransform(PS2_GS_VU1_OBJECT_MAX_VERTICES + 3u,
            0u, 0u, &layout) ||
        ps2GsVu1PlanObjectTransform(4u, 0u, 0u, &layout)) {
        return false;
    }
    if (!ps2GsVu1PlanObjectTransform(3u, 0u, 0u, &layout) ||
        layout.input_qw != textured.input_qw + PS2_GS_VU1_OBJECT_XFORM_QW ||
        layout.output_qw != textured.output_qw) {
        return false;
    }

    /* payload: transform constants sit right after the prefix slots */
    uint32_t payload[256u * 4u];
    const float scale[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    const float offset[4] = { 0.0f, 0.0f, 0.0f, 4095.9375f };
    struct Ps2GsVu1ObjectTransform xf;
    memset(&xf, 0, sizeof(xf));
    xf.m[0][0] = 2.0f;
    xf.color_add[3] = 9.5f;
    struct Ps2GsVu1ObjectVertex v[3];
    memset(v, 0, sizeof(v));
    v[1].pos[0] = 77;
    if (!ps2GsVu1BuildObjectTransformPayload(payload, 256u, scale, offset,
            0u, &xf, NULL, 0u, v, 3u, NULL, 0u, &layout)) {
        return false;
    }
    const uint32_t xform_qw = PS2_GS_VU1_TRANSFORM_HEADER_QW +
        PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS;
    float m00;
    memcpy(&m00, &payload[xform_qw * 4u], sizeof(m00));
    int32_t pos;
    memcpy(&pos, &payload[(xform_qw + PS2_GS_VU1_OBJECT_XFORM_QW + 3u) * 4u],
        sizeof(pos));
    return m00 == 2.0f && pos == 77 &&
        memcmp(&payload[(xform_qw + PS2_GS_VU1_OBJECT_XFORM_QW - 1u) * 4u + 3u],
            &xf.color_add[3], 4u) == 0;
}

int main(void)
{
    if (!test_object_plan_limits()) {
        fprintf(stderr, "object plan/payload test failed\n");
        return 1;
    }
    struct Ps2GsVu1TransformLayout layout = {};
    float mapped_scale[4] = {};
    float mapped_offset[4] = {};
    assert(ps2GsVu1BuildViewportMapping(
        0, 0, 320, 240, 16, 32, 0.0f, 1.0f,
        mapped_scale, mapped_offset));
    assert(mapped_scale[0] == 160.0f);
    assert(mapped_scale[1] == -120.0f);
    assert(mapped_scale[2] == -65534.0f);
    assert(mapped_scale[3] == 0.0f);
    assert(mapped_offset[0] == 161.0f);
    assert(mapped_offset[1] == 122.0f);
    assert(mapped_offset[2] == 65535.0f);
    assert(mapped_offset[3] == 4095.9375f);
    assert(!ps2GsVu1BuildViewportMapping(
        0, 0, 0, 240, 16, 32, 0.0f, 1.0f,
        mapped_scale, mapped_offset));
    assert(!ps2GsVu1BuildViewportMapping(
        0, 0, 320, 240, 16, 32, -0.1f, 1.0f,
        mapped_scale, mapped_offset));
    assert(!ps2GsVu1BuildViewportMapping(
        0, 0, 320, 240, 16, 32,
        std::numeric_limits<float>::quiet_NaN(), 1.0f,
        mapped_scale, mapped_offset));
    assert(!ps2GsVu1BuildViewportMapping(
        0, 0, 320, 240, 16, 32,
        0.0f, std::numeric_limits<float>::infinity(),
        mapped_scale, mapped_offset));
    assert(ps2GsVu1BuildViewportMapping(
        10, 20, 640, 448, 32768, 32768, 1.0f, 0.0f,
        mapped_scale, mapped_offset));
    assert(mapped_scale[2] == 65534.0f);
    assert(mapped_offset[2] == 1.0f);
    assert(mapped_offset[0] == 2378.0f);
    assert(mapped_offset[1] == 2292.0f);

    assert(!ps2GsVu1PlanTexturedTransform(0u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(4u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(84u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(3u, 6u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(3u, 0u, 2u, &layout));
    assert(ps2GsVu1PlanTexturedTransform(81u, 5u, 1u, &layout));
    assert(layout.input_qw == 255u);
    assert(layout.output_qw == 252u);
    assert(layout.dma_chain_qw == 258u);

    const float scale[4] = { 2560.0f, -1792.0f, -65534.0f, 0.0f };
    const float offset[4] = { 32768.0f, 32768.0f, 65535.0f, 0.0f };
    const struct Ps2GsPackedReg prefix[2] = {
        makeReg(0x1111u, 0x06u),
        makeReg(0x2222u, 0x00u),
    };
    struct Ps2GsVu1TransformVertex vertices[3] = {};
    for (uint32_t i = 0u; i < 3u; ++i) {
        vertices[i].clip[0] = (float)i + 0.25f;
        vertices[i].clip[1] = (float)i + 0.5f;
        vertices[i].clip[2] = (float)i + 0.75f;
        vertices[i].clip[3] = 1.0f;
        vertices[i].texcoord.s = 4.0f + (float)i;
        vertices[i].texcoord.t = 8.0f + (float)i;
        vertices[i].texcoord.q = 1.0f;
        vertices[i].texcoord.xyz_control = i << 4;
        vertices[i].rgba[0] = 0x10u + i;
        vertices[i].rgba[1] = 0x20u + i;
        vertices[i].rgba[2] = 0x30u + i;
        vertices[i].rgba[3] = 0x40u + i;
    }
    const struct Ps2GsPackedReg suffix = makeReg(0x3333u, 0x08u);
    uint32_t payload[PS2_GS_VU1_BUFFER_QW * 4u] = {};
    assert(ps2GsVu1BuildTexturedTransformPayload(
        payload, PS2_GS_VU1_BUFFER_QW, scale, offset,
        PS2_GS_VU1_TRANSFORM_FLAG_FOG,
        prefix, 2u, vertices, 3u, &suffix, 1u, &layout));
    assert(layout.input_qw == 21u);
    assert(layout.output_qw == 18u);
    assert(layout.dma_chain_qw == 24u);
    assert(payload[0] == 3u);
    assert(payload[1] == 2u);
    assert(payload[2] == 1u);
    assert(payload[3] == PS2_GS_VU1_TRANSFORM_FLAG_FOG);
    assert(memcmp(&payload[4], scale, sizeof(scale)) == 0);
    assert(memcmp(&payload[8], offset, sizeof(offset)) == 0);

    uint64_t tag = 0u;
    uint64_t registers = 0u;
    memcpy(&tag, &payload[12], sizeof(tag));
    memcpy(&registers, &payload[14], sizeof(registers));
    assert(tag == 0x1000000000000005ull);
    assert(registers == 0x0eull);
    memcpy(&tag, &payload[16], sizeof(tag));
    memcpy(&registers, &payload[18], sizeof(registers));
    assert(tag == 0x3000000000000003ull);
    assert(registers == 0x412ull);
    memcpy(&tag, &payload[20], sizeof(tag));
    memcpy(&registers, &payload[22], sizeof(registers));
    assert(tag == 0x1000000000008001ull);
    assert(registers == 0x0eull);

    struct Ps2GsPackedReg copied_prefix[2] = {};
    memcpy(copied_prefix, &payload[24], sizeof(copied_prefix));
    assert(memcmp(copied_prefix, prefix, sizeof(prefix)) == 0);
    struct Ps2GsVu1TransformVertex copied_vertices[3] = {};
    memcpy(copied_vertices, &payload[44], sizeof(copied_vertices));
    assert(memcmp(copied_vertices, vertices, sizeof(vertices)) == 0);
    struct Ps2GsPackedReg copied_suffix = {};
    memcpy(&copied_suffix, &payload[80], sizeof(copied_suffix));
    assert(copied_suffix.value == suffix.value);
    assert(copied_suffix.reg == suffix.reg);

    struct Ps2GsPackedReg padded_prefix[3] = {};
    memcpy(padded_prefix, &payload[32], sizeof(padded_prefix));
    for (uint32_t i = 0u; i < 3u; ++i) {
        assert(padded_prefix[i].value == 0u);
        assert(padded_prefix[i].reg == 0x0fu);
    }

    assert(!ps2GsVu1BuildTexturedTransformPayload(
        payload, layout.input_qw - 1u, scale, offset, 0u,
        prefix, 2u, vertices, 3u, &suffix, 1u, NULL));
    assert(!ps2GsVu1BuildTexturedTransformPayload(
        payload, PS2_GS_VU1_BUFFER_QW, scale, offset, 2u,
        prefix, 2u, vertices, 3u, NULL, 0u, NULL));

    memset(payload, 0, sizeof(payload));
    assert(ps2GsVu1BuildTexturedTransformPayload(
        payload, PS2_GS_VU1_BUFFER_QW, scale, offset, 0u,
        NULL, 0u, vertices, 3u, NULL, 0u, &layout));
    assert(layout.input_qw == 21u);
    assert(layout.output_qw == 18u);
    memcpy(&tag, &payload[12], sizeof(tag));
    memcpy(&registers, &payload[14], sizeof(registers));
    assert(tag == 0x1000000000000005ull);
    assert(registers == 0x0eull);
    memcpy(&tag, &payload[16], sizeof(tag));
    memcpy(&registers, &payload[18], sizeof(registers));
    assert(tag == 0x3000000000000003ull);
    assert(registers == 0x512ull);
    memcpy(&tag, &payload[20], sizeof(tag));
    memcpy(&registers, &payload[22], sizeof(registers));
    assert(tag == 0x1000000000008001ull);
    assert(registers == 0x0eull);

    struct Ps2GsPackedReg nop = {};
    for (uint32_t i = 0u;
         i < PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS; ++i) {
        memcpy(&nop, &payload[24u + i * 4u], sizeof(nop));
        assert(nop.value == 0u);
        assert(nop.reg == 0x0fu);
    }
    memcpy(&nop, &payload[80], sizeof(nop));
    assert(nop.value == 0u);
    assert(nop.reg == 0x0fu);

    struct Ps2GsPackedReg max_prefix[
        PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS] = {};
    struct Ps2GsVu1TransformVertex max_vertices[
        PS2_GS_VU1_MAX_TEXTURED_VERTICES] = {};
    assert(ps2GsVu1BuildTexturedTransformPayload(
        payload, PS2_GS_VU1_BUFFER_QW, scale, offset, 0u,
        max_prefix, PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS,
        max_vertices, PS2_GS_VU1_MAX_TEXTURED_VERTICES,
        &suffix, PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS, &layout));
    assert(layout.input_qw == 255u);
    assert(layout.output_qw == 252u);
    assert(layout.dma_chain_qw == 258u);

    uint32_t stream[
        (PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW) *
        4u] = {};
    assert(ps2GsVu1BuildTexturedTransformStream(
        stream,
        PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW,
        0x00102030u, scale, offset, PS2_GS_VU1_TRANSFORM_FLAG_FOG,
        prefix, 2u, vertices, 3u, &suffix, 1u, &layout));
    assert(layout.dma_chain_qw == 24u);

    uint64_t dma_tag = 0u;
    memcpy(&dma_tag, &stream[0], sizeof(dma_tag));
    assert(dma_tag == 0x0010203030000015ull);
    assert(stream[2] == 0x01000101u);
    assert(stream[3] == 0x6c15c000u);
    memcpy(&dma_tag, &stream[4], sizeof(dma_tag));
    assert(dma_tag == 0x0000000010000000ull);
    assert(stream[6] == 0x13000000u);
    assert(stream[7] == 0x14000040u);
    memcpy(&dma_tag, &stream[8], sizeof(dma_tag));
    assert(dma_tag == 0x0000000070000000ull);
    assert(stream[10] == 0x11000000u);
    assert(stream[11] == 0u);

    uint32_t expected_payload[PS2_GS_VU1_BUFFER_QW * 4u] = {};
    assert(ps2GsVu1BuildTexturedTransformPayload(
        expected_payload, PS2_GS_VU1_BUFFER_QW, scale, offset,
        PS2_GS_VU1_TRANSFORM_FLAG_FOG,
        prefix, 2u, vertices, 3u, &suffix, 1u, NULL));
    assert(memcmp(&stream[12], expected_payload,
        (size_t)layout.input_qw * 16u) == 0);
    assert(!ps2GsVu1BuildTexturedTransformStream(
        stream, layout.dma_chain_qw - 1u,
        0x00102030u, scale, offset, 0u,
        prefix, 2u, vertices, 3u, &suffix, 1u, NULL));
    assert(!ps2GsVu1BuildTexturedTransformStream(
        stream, layout.dma_chain_qw,
        0x00102034u, scale, offset, 0u,
        prefix, 2u, vertices, 3u, &suffix, 1u, NULL));

    /*
     * The builders write UCAB memory directly and no longer pre-clear it.
     * Prove that every byte inside the planned stream is defined: building
     * over 0x00 and 0xa5 poisoned slots must yield byte-identical output.
     */
    for (uint32_t prefix_count = 0u; prefix_count <= 2u; prefix_count += 2u) {
        for (uint32_t vertex_count = 3u;
             vertex_count <= PS2_GS_VU1_MAX_TEXTURED_VERTICES;
             vertex_count += 39u) {
            static struct Ps2GsVu1TransformVertex poison_vertices[
                PS2_GS_VU1_MAX_TEXTURED_VERTICES];
            for (uint32_t i = 0u; i < vertex_count; ++i) {
                poison_vertices[i] = vertices[i % 3u];
            }
            const uint32_t slot_qw =
                PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW;
            static uint32_t clean[
                (PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW) *
                4u];
            static uint32_t poisoned[
                (PS2_GS_VU1_BUFFER_QW + PS2_GS_VU1_DMA_CHAIN_OVERHEAD_QW) *
                4u];
            memset(clean, 0x00, sizeof(clean));
            memset(poisoned, 0xa5, sizeof(poisoned));
            struct Ps2GsVu1TransformLayout clean_layout = {};
            assert(ps2GsVu1BuildTexturedTransformStream(
                clean, slot_qw, 0x00102030u, scale, offset,
                PS2_GS_VU1_TRANSFORM_FLAG_FOG,
                prefix_count ? prefix : NULL, prefix_count,
                poison_vertices, vertex_count,
                &suffix, 1u, &clean_layout));
            assert(ps2GsVu1BuildTexturedTransformStream(
                poisoned, slot_qw, 0x00102030u, scale, offset,
                PS2_GS_VU1_TRANSFORM_FLAG_FOG,
                prefix_count ? prefix : NULL, prefix_count,
                poison_vertices, vertex_count,
                &suffix, 1u, NULL));
            assert(memcmp(clean, poisoned,
                (size_t)clean_layout.dma_chain_qw * 16u) == 0);
            /* Bytes past the planned stream stay untouched. */
            const uint8_t *tail = (const uint8_t *)poisoned +
                (size_t)clean_layout.dma_chain_qw * 16u;
            const size_t tail_bytes = sizeof(poisoned) -
                (size_t)clean_layout.dma_chain_qw * 16u;
            for (size_t i = 0u; i < tail_bytes; ++i) {
                assert(tail[i] == 0xa5u);
            }
        }
    }

    return 0;
}
