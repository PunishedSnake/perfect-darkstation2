#ifndef PERFECT_DARK_PS2_GS_VU1_TRANSFORM_H
#define PERFECT_DARK_PS2_GS_VU1_TRANSFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "gs_core.h"
#include "gs_vu1_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VU1 geometry-stage memory contract.
 *
 * VIF1 double-buffers raw input in QW 0..511. The matching VU1 program writes
 * complete GIF packets into QW 512..1023, keeping producer and consumer banks
 * disjoint while preserving the same 256-QW bank size on both sides. Prefix
 * and suffix slots have fixed positions; unused A+D records target GS NOP.
 */
#define PS2_GS_VU1_TRANSFORM_OUTPUT_BASE_QW 512u
#define PS2_GS_VU1_TRANSFORM_OUTPUT_BANK_STRIDE_QW 256u
#define PS2_GS_VU1_TRANSFORM_OUTPUT_SECOND_BASE_QW 768u
#define PS2_GS_VU1_TRANSFORM_HEADER_QW 6u
#define PS2_GS_VU1_TRANSFORM_VERTEX_QW 3u
#define PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS 5u
#define PS2_GS_VU1_TRANSFORM_MAX_SUFFIX_RECORDS 1u
#define PS2_GS_VU1_TRANSFORM_FLAG_FOG (1u << 0)
#define PS2_GS_VU1_TRANSFORM_PROGRAM_ADDRESS 64u

struct Ps2GsVu1TransformTexcoord {
    float s;
    float t;
    float q;
    /* Raw PACKED W lane: fog in bits 4..11, ADC in bit 15. Never float math. */
    uint32_t xyz_control;
};

struct Ps2GsVu1TransformVertex {
    float clip[4];
    struct Ps2GsVu1TransformTexcoord texcoord;
    uint32_t rgba[4];
};

struct Ps2GsVu1TransformLayout {
    uint32_t vertex_count;
    uint32_t prefix_count;
    uint32_t suffix_count;
    uint32_t input_qw;
    uint32_t output_qw;
    uint32_t dma_chain_qw;
};

/*
 * Object-space variant: VU1 builds the whole record. Each vertex is three
 * quadwords of int32 lanes, straight from the N64 vertex:
 *   (x, y, z, -)      object position (lane w ignored)
 *   (u, v, 0, 0)      texture coordinate after the G_TEXTURE scale (s * S >> 16)
 *   (r, g, b, a)      vertex colour bytes
 * and VU1 computes
 *   clip  = x * m[0] + y * m[1] + z * m[2] + m[3]   (Fast3D's convention)
 *   s, t  = (u * tex_mul + tex_add) * tex_scale     (lanes x, y)
 *   rgba  = trunc(r * color[0] + g * color[1] + b * color[2] + a * color[3]
 *                 + color_add)                       (per lane)
 * then the same divide/viewport/pack as the textured program. fog holds
 * (255, B, -1, A): the XYZF2 fog lane is (255 - trunc(clamp(A z/w + B))) << 4.
 * The constants follow the prefix slots (PS2_GS_VU1_OBJECT_XFORM_QW).
 */
#define PS2_GS_VU1_OBJECT_PROGRAM_ADDRESS 256u
#define PS2_GS_VU1_OBJECT_XFORM_QW 13u
#define PS2_GS_VU1_OBJECT_MAX_VERTICES 75u

struct Ps2GsVu1ObjectTransform {
    float m[4][4];
    float fog[4];
    float tex_mul[4];
    float tex_add[4];
    float tex_scale[4];
    float color[4][4];
    float color_add[4];
};

/*
 * Packed for transport: three 16-bit lanes per quadword the program reads.
 * VIF1 expands them with a V4-16 UNPACK (sign-extended to the int32 lanes
 * the program expects), so the EE writes and DMA moves 24 bytes per vertex
 * instead of 48. The lane values all fit: positions are the N64 s16
 * coordinates, uv the s16 scaled texture coordinates, rgba bytes.
 */
struct Ps2GsVu1ObjectVertex {
    int16_t pos[4];
    int16_t uv[4];
    int16_t rgba[4];
};

/* header, prefix slots and transform constants: one V4-32 UNPACK */
#define PS2_GS_VU1_OBJECT_HEAD_QW \
    (PS2_GS_VU1_TRANSFORM_HEADER_QW + PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS + \
     PS2_GS_VU1_OBJECT_XFORM_QW)

bool ps2GsVu1PlanObjectTransform(uint32_t vertex_count,
    uint32_t prefix_count, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout);

/*
 * The transport form: the head (PS2_GS_VU1_OBJECT_HEAD_QW quadwords, VU1
 * memory image) and the one suffix record. The packed vertices go between
 * them in VU1 memory by their own UNPACK.
 */
bool ps2GsVu1BuildObjectTransformHead(
    uint32_t *head, struct Ps2GsPackedReg *suffix_out,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsVu1ObjectTransform *xform,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count);

/* The whole VU1 memory image (vertices widened to int32 lanes): tests. */
bool ps2GsVu1BuildObjectTransformPayload(
    uint32_t *destination, uint32_t capacity_qw,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsVu1ObjectTransform *xform,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsVu1ObjectVertex *vertices, uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout);

bool ps2GsVu1PlanTexturedTransform(uint32_t vertex_count,
    uint32_t prefix_count, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout);

bool ps2GsVu1BuildViewportMapping(
    int32_t viewport_x, int32_t viewport_y,
    int32_t viewport_width, int32_t viewport_height,
    int32_t gs_offset_x, int32_t gs_offset_y,
    float depth_near, float depth_far,
    float scale[4], float offset[4]);

/*
 * Build the TOP-relative raw input payload consumed by the transform
 * microprogram. Scale and offset are four-float vectors used for viewport and
 * depth mapping after perspective division. GIF tags for fixed-size state,
 * vertices and state restoration sections are embedded in the six-QW header.
 */
bool ps2GsVu1BuildTexturedTransformPayload(
    uint32_t *destination, uint32_t capacity_qw,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsVu1TransformVertex *vertices, uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout);

/*
 * Wrap one raw payload in a three-QW VIF1 source chain. The chain unpacks to
 * TOPS, invokes the transform microprogram at address 64 and retains the
 * validation FLUSHA/FLUSH ordering used by the existing PATH1 diagnostic.
 */
bool ps2GsVu1BuildTexturedTransformStream(
    uint32_t *destination, uint32_t capacity_qw,
    uint32_t payload_dma_address,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    const struct Ps2GsVu1TransformVertex *vertices, uint32_t vertex_count,
    const struct Ps2GsPackedReg *suffix, uint32_t suffix_count,
    struct Ps2GsVu1TransformLayout *layout);

#ifdef __cplusplus
}
#endif

#endif
