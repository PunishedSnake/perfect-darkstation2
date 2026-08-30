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
