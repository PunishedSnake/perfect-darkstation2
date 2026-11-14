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

/*
 * Emotion3D skinned/lit object program (gs_vu1_skin.vsm).
 *
 * Like the object program, but each vertex names its own transform: a
 * palette entry (one per matrix epoch of a model list: Perfect Dark skins
 * by loading vertex blocks under different bone matrices and joining them
 * with triangles), and the entry also carries that matrix's light and
 * lookat directions, so N64 lighting and texture generation run on VU1.
 *
 * Input TOPS bank:
 *   QW 0      control: vertex count, vertex data offset (QW), suffix count,
 *             flags (FOG, LIGHT, TEXGEN, CULL, LOD)
 *   QW 1..2   viewport scale/offset;  QW 3..5 prefix, vertex, suffix tags
 *   QW 6..10  prefix A+D records
 *   QW 11..26 struct Ps2GsVu1SkinConst
 *   QW 27..   palette, PS2_GS_VU1_SKIN_ENTRY_QW each (struct Ps2GsVu1SkinEntry)
 *   then per vertex (struct Ps2GsVu1ObjectVertex, V4-16):
 *     (x, y, z, entry)   entry = the palette entry's QW offset from TOPS
 *     (u, v, -, -)
 *     (r, g, b, a)       or with LIGHT: (nx, ny, nz, a), normal bytes signed
 *   then one suffix record.
 *
 * Per vertex: clip = palette matrix * position. With LIGHT, Fast3D's
 * colour: ambient, plus each light's colour times max(0, n . dir), the sum
 * truncated to an integer after every light, capped at 255. With TEXGEN
 * (Fast3D's spherical G_TEXTURE_GEN with lookat), u/v = trunc((clamp(n .
 * lookat, -1, 1) + 1) / 4 * scale). Then the object program's record
 * program and pack. With CULL, each triangle whose screen cross product has
 * the culled sign keeps its records but collapses to its first corner's
 * position (zero area, nothing drawn).
 */
#define PS2_GS_VU1_SKIN_PROGRAM_ADDRESS 512u
#define PS2_GS_VU1_SKIN_PROGRAM_MAX_INSTRUCTIONS 768u
#define PS2_GS_VU1_TRANSFORM_FLAG_LIGHT (1u << 1)
#define PS2_GS_VU1_TRANSFORM_FLAG_TEXGEN (1u << 2)
#define PS2_GS_VU1_TRANSFORM_FLAG_CULL (1u << 3)
#define PS2_GS_VU1_TRANSFORM_FLAG_LOD (1u << 4)
#define PS2_GS_VU1_SKIN_CONST_QW 16u
#define PS2_GS_VU1_SKIN_HEAD_QW \
    (PS2_GS_VU1_TRANSFORM_HEADER_QW + PS2_GS_VU1_TRANSFORM_MAX_PREFIX_RECORDS + \
     PS2_GS_VU1_SKIN_CONST_QW)
#define PS2_GS_VU1_SKIN_ENTRY_QW 10u
#define PS2_GS_VU1_SKIN_MAX_ENTRIES 8u
#define PS2_GS_VU1_SKIN_MAX_LIGHTS 4u

struct Ps2GsVu1SkinConst {
    float fog[4];           /* as the object program */
    float tex_mul[4];
    float tex_add[4];
    float tex_scale[4];
    float color[4][4];      /* record program rows for r, g, b, a */
    float color_add[4];
    float light[4][4];      /* light i colour (r, g, b, 0); unused: 0 */
    float ambient[4];       /* (r, g, b, 0) */
    /* (G_TEXTURE scale s / 4, scale t / 4, cull sign, 2^64): the cull sign
     * makes a kept triangle's screen cross product negative */
    float misc[4];
    /* LOD: alpha input = trunc(z + clamp(w * x, 0, 1) * y), Fast3D's LOD
     * fraction byte (x = 1/1024, y = 0.3 * 255, z = 0.7 * 255) */
    float lod[4];
};

struct Ps2GsVu1SkinEntry {
    float m[4][4];          /* clip transform, as Ps2GsVu1ObjectTransform.m */
    /* light directions in this matrix's object space / 127, as columns:
     * lane i = light i */
    float light_x[4], light_y[4], light_z[4];
    /* lookat 0 and 1 in lanes x and y, / 127 */
    float look_x[4], look_y[4], look_z[4];
};

/*
 * A skinned batch travels beside the object path: while one is set
 * (ps2GsVu1QueueSetSkin), object batches are drawn with the skin program
 * and these constants instead of the object transform. misc[2] holds the
 * cull sign before the viewport (+1 cull back, -1 cull front, times -1
 * when the clip matrix inverts y); the queue applies the viewport's sign.
 */
struct Ps2GsVu1SkinBatch {
    const struct Ps2GsVu1SkinConst *k;
    const struct Ps2GsVu1SkinEntry *entries;
    uint32_t entry_count;
    uint32_t flags; /* LIGHT, TEXGEN, CULL (FOG comes from the GS state) */
};

/* The VU1 memory image ahead of the vertices; returns its size in QW. */
uint32_t ps2GsVu1BuildSkinHead(uint32_t *head,
    const float scale[4], const float offset[4], uint32_t flags,
    const struct Ps2GsVu1SkinConst *k,
    const struct Ps2GsVu1SkinEntry *entries, uint32_t entry_count,
    const struct Ps2GsPackedReg *prefix, uint32_t prefix_count,
    uint32_t vertex_count, uint32_t suffix_count);

/* Largest vertex count (a multiple of 3) for a batch with this many entries. */
uint32_t ps2GsVu1SkinMaxVertices(uint32_t entry_count);

/*
 * What the program computes for one vertex, before the divide: the EE
 * fallback and the tests. entry is the vertex's palette entry; scale is the
 * viewport scale (for nothing but the cull sign, applied by the caller).
 */
void ps2GsVu1SkinVertexToClip(const struct Ps2GsVu1SkinConst *k,
    const struct Ps2GsVu1SkinEntry *entry, uint32_t flags, bool fog,
    const struct Ps2GsVu1ObjectVertex *in,
    struct Ps2GsVu1TransformVertex *out);

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
