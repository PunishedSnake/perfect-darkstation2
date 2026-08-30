#include <assert.h>
#include <string.h>

#include "gs_vu1_transform.h"

static struct Ps2GsPackedReg makeReg(uint64_t value, uint64_t reg)
{
    const struct Ps2GsPackedReg result = { value, reg };
    return result;
}

int main(void)
{
    struct Ps2GsVu1TransformLayout layout = {};
    assert(!ps2GsVu1PlanTexturedTransform(0u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(4u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(84u, 0u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(3u, 6u, 0u, &layout));
    assert(!ps2GsVu1PlanTexturedTransform(3u, 0u, 2u, &layout));
    assert(ps2GsVu1PlanTexturedTransform(81u, 5u, 1u, &layout));
    assert(layout.input_qw == 255u);
    assert(layout.output_qw == 252u);

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
    assert(layout.input_qw == 18u);
    assert(layout.output_qw == 15u);
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
    assert(tag == 0x1000000000000002ull);
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
    memcpy(copied_vertices, &payload[32], sizeof(copied_vertices));
    assert(memcmp(copied_vertices, vertices, sizeof(vertices)) == 0);
    struct Ps2GsPackedReg copied_suffix = {};
    memcpy(&copied_suffix, &payload[68], sizeof(copied_suffix));
    assert(copied_suffix.value == suffix.value);
    assert(copied_suffix.reg == suffix.reg);

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
    memcpy(&tag, &payload[16], sizeof(tag));
    memcpy(&registers, &payload[18], sizeof(registers));
    assert(tag == 0x3000000000008003ull);
    assert(registers == 0x512ull);

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

    return 0;
}
