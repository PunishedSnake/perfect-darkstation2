#ifndef PERFECT_DARK_PS2_GFX_PS2_OBJECT_H
#define PERFECT_DARK_PS2_GFX_PS2_OBJECT_H

/*
 * Object-space geometry helpers shared by the Fast3D frontend (deferred
 * G_VTX blocks) and the PS2 backend (EE fallback for object batches).
 *
 * A G_VTX block whose bounding box lies wholly inside the clip guard band is
 * kept in object space: VU1 applies the clip matrix (gs_vu1_object_transform
 * .vsm) and the EE never transforms, classifies or clips those vertices.
 * Everything here is pure math so the host tests can check it.
 */

#include <math.h>
#include <stdint.h>

#include "gs_clip.h"
#include "gs_vu1_transform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* clip_j = x * m[0][j] + y * m[1][j] + z * m[2][j] + m[3][j], in the order
 * VU1's MULA/MADDA/MADDA/MADD chain accumulates it. */
static inline void gfxPs2ObjectClip(const float m[4][4], float x, float y,
    float z, float out[4])
{
    for (int j = 0; j < 4; ++j) {
        float acc = x * m[0][j];
        acc = acc + y * m[1][j];
        acc = acc + z * m[2][j];
        out[j] = acc + m[3][j];
    }
}

/* Fog control lane for fog constants (255, B, -1, A), see the .vsm. */
static inline uint32_t gfxPs2ObjectFogControl(const float fog[4], float z,
    float w)
{
    float f = z * (1.0f / w) * fog[3] + fog[1];
    f = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
    const uint32_t t = (uint32_t)f;
    return (255u - t) << 4;
}

enum Ps2ObjectBoxClass {
    PS2_OBJECT_BOX_INSIDE = 0,  /* all inside the guard band, w >= min_w */
    PS2_OBJECT_BOX_OUTSIDE = 1, /* all outside one exact frustum plane */
    PS2_OBJECT_BOX_MIXED = 2,
};

/*
 * Classify an object-space box by its eight corners. Every test is a
 * half-space linear in clip coordinates and clip coordinates are affine in
 * object coordinates, so a property shared by all corners holds for every
 * point of the box. *outside_mask receives the exact planes (bits 0..5 of
 * ps2GsClipVertexCodes) that every corner is outside of.
 */
static inline enum Ps2ObjectBoxClass gfxPs2ObjectClassifyBox(
    const float m[4][4], const float lo[3], const float hi[3],
    float guard_x, float guard_y, float min_w, uint32_t *outside_mask)
{
    unsigned int all_out = 0x3fu;
    unsigned int any_guard = 0u;
    float w_min = INFINITY;
    for (int corner = 0; corner < 8; ++corner) {
        float clip[4];
        gfxPs2ObjectClip(m,
            (corner & 1) ? hi[0] : lo[0],
            (corner & 2) ? hi[1] : lo[1],
            (corner & 4) ? hi[2] : lo[2], clip);
        const unsigned int codes =
            ps2GsClipVertexCodesInline(clip, guard_x, guard_y);
        all_out &= codes;
        any_guard |= codes & (0x3f00u | PS2_GS_CLIP_CODE_NONFINITE);
        w_min = clip[3] < w_min ? clip[3] : w_min;
    }
    *outside_mask = 0u;
    if ((any_guard & PS2_GS_CLIP_CODE_NONFINITE) == 0u && (all_out & 0x3fu) != 0u) {
        *outside_mask = all_out & 0x3fu;
        return PS2_OBJECT_BOX_OUTSIDE;
    }
    if (any_guard == 0u && w_min >= min_w) {
        return PS2_OBJECT_BOX_INSIDE;
    }
    return PS2_OBJECT_BOX_MIXED;
}

/*
 * The same box tests as planes, for many boxes under one matrix: each test
 * is value(p) = p . pl[0..2][j] + pl[3][j], ranging over a box by
 * value(centre) -/+ |pl[0..2][j]| . half-extent. Planes 0..6 must stay >= 0
 * for INSIDE (guard x/y, near z, far w - z, w - min_w); 7..10 are the exact
 * x/y planes; 11 is padding.
 */
#define PS2_OBJECT_PLANES 12

static inline void gfxPs2ObjectBuildPlanes(const float m[4][4], float guard_x,
    float guard_y, float min_w, float pl[4][PS2_OBJECT_PLANES],
    float pa[3][PS2_OBJECT_PLANES])
{
    for (int i = 0; i < 4; ++i) {
        const float cx = m[i][0], cy = m[i][1], cz = m[i][2], cw = m[i][3];
        const float gxw = guard_x * cw, gyw = guard_y * cw;
        float *p = &pl[i][0];
        p[0] = cx + gxw;
        p[1] = gxw - cx;
        p[2] = cy + gyw;
        p[3] = gyw - cy;
        p[4] = cz;
        p[5] = cw - cz;
        p[6] = cw;
        p[7] = cx + cw;
        p[8] = cw - cx;
        p[9] = cy + cw;
        p[10] = cw - cy;
        p[11] = 0.0f;
    }
    pl[3][6] -= min_w;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < PS2_OBJECT_PLANES; ++j) {
            pa[i][j] = fabsf(pl[i][j]);
        }
    }
}

static inline enum Ps2ObjectBoxClass gfxPs2ObjectClassifyPlanes(
    const float pl[4][PS2_OBJECT_PLANES], const float pa[3][PS2_OBJECT_PLANES],
    const float lo[3], const float hi[3], uint32_t *outside_mask)
{
    const float c0 = (lo[0] + hi[0]) * 0.5f, c1 = (lo[1] + hi[1]) * 0.5f;
    const float c2 = (lo[2] + hi[2]) * 0.5f;
    const float e0 = (hi[0] - lo[0]) * 0.5f, e1 = (hi[1] - lo[1]) * 0.5f;
    const float e2 = (hi[2] - lo[2]) * 0.5f;
    float vmin[PS2_OBJECT_PLANES], vmax[PS2_OBJECT_PLANES];
    for (int j = 0; j < PS2_OBJECT_PLANES; ++j) {
        const float d = c0 * pl[0][j] + c1 * pl[1][j] + c2 * pl[2][j] + pl[3][j];
        const float r = e0 * pa[0][j] + e1 * pa[1][j] + e2 * pa[2][j];
        vmin[j] = d - r;
        vmax[j] = d + r;
    }
    uint32_t mask = 0u;
    mask |= vmax[7] < 0.0f ? 1u : 0u;
    mask |= vmax[8] < 0.0f ? 2u : 0u;
    mask |= vmax[9] < 0.0f ? 4u : 0u;
    mask |= vmax[10] < 0.0f ? 8u : 0u;
    mask |= vmax[4] < 0.0f ? 16u : 0u;
    mask |= vmax[5] < 0.0f ? 32u : 0u;
    *outside_mask = mask;
    if (mask != 0u) {
        return PS2_OBJECT_BOX_OUTSIDE;
    }
    for (int j = 0; j < 7; ++j) {
        if (!(vmin[j] >= 0.0f)) {
            return PS2_OBJECT_BOX_MIXED;
        }
    }
    return PS2_OBJECT_BOX_INSIDE;
}

static inline float gfxPs2ObjectDet3(float a0, float a1, float a2,
    float b0, float b1, float b2, float c0, float c1, float c2)
{
    return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) +
        a2 * (b0 * c1 - b1 * c0);
}

/*
 * Backface test without transforming. Fast3D culls on
 *   cross = -det[[x1 x2 x3], [y1 y2 y3], [w1 w2 w3]]
 * of the transformed vertices. With A the x, y and w columns of the clip
 * matrix (3x4) and P the homogeneous object points (4x3), Cauchy-Binet gives
 *   det(A P) = sum_k det(A without column k) * det(P without row k).
 * The first factors depend on the matrix only (gfxPs2ObjectCullBasis, once
 * per block); the second on the triangle only.
 */
static inline void gfxPs2ObjectCullBasis(const float m[4][4], float alpha[4])
{
    /* rows of A: clip x, y, w as functions of (x, y, z, 1) */
    const float ax[4] = { m[0][0], m[1][0], m[2][0], m[3][0] };
    const float ay[4] = { m[0][1], m[1][1], m[2][1], m[3][1] };
    const float aw[4] = { m[0][3], m[1][3], m[2][3], m[3][3] };
    for (int k = 0; k < 4; ++k) {
        int c[3];
        for (int i = 0, n = 0; i < 4; ++i) {
            if (i != k) {
                c[n++] = i;
            }
        }
        alpha[k] = gfxPs2ObjectDet3(ax[c[0]], ax[c[1]], ax[c[2]],
            ay[c[0]], ay[c[1]], ay[c[2]], aw[c[0]], aw[c[1]], aw[c[2]]);
    }
}

/*
 * Fast3D's `cross` for the triangle (p1, p2, p3), object coordinates. The
 * four minors of P are the plane of the triangle: with n = (p2 - p1) x
 * (p3 - p1), det(P without row 0, 1, 2, 3) = n.x, -n.y, n.z, n . p1.
 */
static inline float gfxPs2ObjectCullCross(const float alpha[4],
    const float p1[3], const float p2[3], const float p3[3])
{
    const float ax = p2[0] - p1[0], ay = p2[1] - p1[1], az = p2[2] - p1[2];
    const float bx = p3[0] - p1[0], by = p3[1] - p1[1], bz = p3[2] - p1[2];
    const float nx = ay * bz - az * by;
    const float ny = az * bx - ax * bz;
    const float nz = ax * by - ay * bx;
    const float d = nx * p1[0] + ny * p1[1] + nz * p1[2];
    return -(alpha[0] * nx - alpha[1] * ny + alpha[2] * nz + alpha[3] * d);
}

/*
 * Object vertex -> the clip-space record the direct path would submit, by the
 * same arithmetic as gs_vu1_object_transform.vsm (EE fallback, tests).
 */
static inline void gfxPs2ObjectVertexToClip(
    const struct Ps2GsVu1ObjectTransform *xf, bool fog,
    const struct Ps2GsVu1ObjectVertex *in,
    struct Ps2GsVu1TransformVertex *out)
{
    float clip[4];
    gfxPs2ObjectClip(xf->m, (float)in->pos[0], (float)in->pos[1],
        (float)in->pos[2], clip);
    for (int i = 0; i < 4; ++i) {
        out->clip[i] = clip[i];
    }
    out->texcoord.s = ((float)in->uv[0] * xf->tex_mul[0] + xf->tex_add[0]) *
        xf->tex_scale[0];
    out->texcoord.t = ((float)in->uv[1] * xf->tex_mul[1] + xf->tex_add[1]) *
        xf->tex_scale[1];
    out->texcoord.q = 1.0f;
    out->texcoord.xyz_control =
        fog ? gfxPs2ObjectFogControl(xf->fog, clip[2], clip[3]) : 0u;
    for (int c = 0; c < 4; ++c) {
        float v = xf->color_add[c];
        for (int j = 0; j < 4; ++j) {
            v += (float)in->rgba[j] * xf->color[j][c];
        }
        out->rgba[c] = (uint32_t)(int)v;
    }
}

#ifdef __cplusplus
}
#endif

#endif
