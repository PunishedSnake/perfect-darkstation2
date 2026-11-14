#include "gfx_ps2_object.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void random_matrix(float m[4][4])
{
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = frand(-1.5f, 1.5f);
        }
    }
    m[3][3] = frand(150.0f, 400.0f); /* keeps most points in front */
}

/* Fast3D's PS2 cull expression on transformed vertices (gfx_sp_tri1). */
static double reference_cross(const float m[4][4], const float *p1,
    const float *p2, const float *p3)
{
    float v[3][4];
    gfxPs2ObjectClip(m, p1[0], p1[1], p1[2], v[0]);
    gfxPs2ObjectClip(m, p2[0], p2[1], p2[2], v[1]);
    gfxPs2ObjectClip(m, p3[0], p3[1], p3[2], v[2]);
    const double x1 = v[0][0], y1 = v[0][1], w1 = v[0][3];
    const double x2 = v[1][0], y2 = v[1][1], w2 = v[1][3];
    const double x3 = v[2][0], y3 = v[2][1], w3 = v[2][3];
    return x2 * (y1 * w3 - y3 * w1) - x1 * (y2 * w3 - y3 * w2) -
        x3 * (y1 * w2 - y2 * w1);
}

static void test_cull_sign_matches_transformed(void)
{
    int checked = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        float m[4][4];
        random_matrix(m);
        float alpha[4];
        gfxPs2ObjectCullBasis(m, alpha);
        float p[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) {
                p[i][k] = (float)(int)frand(-100.0f, 100.0f);
            }
        }
        const double ref = reference_cross(m, p[0], p[1], p[2]);
        const float got = gfxPs2ObjectCullCross(alpha, p[0], p[1], p[2]);
        /* near-degenerate triangles may round either way */
        if (fabs(ref) < 1e-3 * (fabs((double)got) + 1.0) + 1.0) {
            continue;
        }
        assert((ref > 0.0) == (got > 0.0f));
        ++checked;
    }
    assert(checked > 15000);
}

static void test_box_classification_is_conservative(void)
{
    int inside = 0, outside = 0, plane_mismatch = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        float m[4][4];
        random_matrix(m);
        m[3][0] = frand(-400.0f, 400.0f);
        m[3][1] = frand(-400.0f, 400.0f);
        m[3][2] = frand(-200.0f, 400.0f);
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k) {
            const float c = frand(-200.0f, 200.0f);
            const float r = frand(1.0f, 120.0f);
            lo[k] = c - r;
            hi[k] = c + r;
        }
        const float gx = frand(1.0f, 8.0f), gy = frand(1.0f, 8.0f);
        uint32_t mask = 0u;
        const enum Ps2ObjectBoxClass cls =
            gfxPs2ObjectClassifyBox(m, lo, hi, gx, gy, 1e-3f, &mask);
        float pl[4][PS2_OBJECT_PLANES], pa[3][PS2_OBJECT_PLANES];
        gfxPs2ObjectBuildPlanes(m, gx, gy, 1e-3f, pl, pa);
        uint32_t pmask = 0u;
        const enum Ps2ObjectBoxClass pcls =
            gfxPs2ObjectClassifyPlanes(pl, pa, lo, hi, &pmask);
        /* exact in real arithmetic; rounding may only differ at a margin */
        if (pcls != cls || pmask != mask) {
            ++plane_mismatch;
        }
        for (int s = 0; s < 64; ++s) {
            float clip[4];
            gfxPs2ObjectClip(m, frand(lo[0], hi[0]), frand(lo[1], hi[1]),
                frand(lo[2], hi[2]), clip);
            const unsigned int codes =
                ps2GsClipVertexCodesInline(clip, gx, gy);
            if (cls == PS2_OBJECT_BOX_INSIDE) {
                assert((codes & 0x3f00u) == 0u && clip[3] > 0.0f);
            } else if (cls == PS2_OBJECT_BOX_OUTSIDE) {
                assert((codes & mask) == mask && mask != 0u);
            }
        }
        inside += cls == PS2_OBJECT_BOX_INSIDE;
        outside += cls == PS2_OBJECT_BOX_OUTSIDE;
    }
    assert(inside > 50 && outside > 50);
    assert(plane_mismatch <= 4);
}

static void test_vertex_to_clip_and_fog(void)
{
    struct Ps2GsVu1ObjectTransform xf = {};
    xf.m[0][0] = 1.0f;
    xf.m[1][1] = 1.0f;
    xf.m[2][2] = 0.5f;
    xf.m[2][3] = 0.0f;
    xf.m[3][2] = 50.0f;
    xf.m[3][3] = 100.0f;
    xf.fog[0] = 255.0f;
    xf.fog[1] = 10.0f;  /* B */
    xf.fog[2] = -1.0f;
    xf.fog[3] = 200.0f; /* A */
    xf.tex_mul[0] = 2.0f;
    xf.tex_add[0] = 1.0f;
    xf.tex_scale[0] = 0.25f;
    xf.tex_mul[1] = 1.0f;
    xf.tex_scale[1] = 1.0f;
    xf.color[0][0] = 128.0f / 255.0f; /* r -> modulate */
    xf.color_add[0] = 0.5f;
    xf.color_add[1] = 7.5f;           /* constant 7 */
    struct Ps2GsVu1ObjectVertex in = {};
    in.pos[0] = 3;
    in.pos[1] = -4;
    in.pos[2] = 20;
    in.uv[0] = 5;
    in.uv[1] = -6;
    in.rgba[0] = 255;
    struct Ps2GsVu1TransformVertex out;
    gfxPs2ObjectVertexToClip(&xf, true, &in, &out);
    assert(out.clip[0] == 3.0f && out.clip[1] == -4.0f);
    assert(out.clip[2] == 60.0f && out.clip[3] == 100.0f);
    assert(out.texcoord.s == 2.75f && out.texcoord.t == -6.0f && out.texcoord.q == 1.0f);
    assert(out.rgba[0] == 128u && out.rgba[1] == 7u && out.rgba[2] == 0u);
    /* z/w = 0.6 -> 0.6 * 200 + 10 = 130 -> (255 - 130) << 4 */
    assert(out.texcoord.xyz_control == (125u << 4));
    gfxPs2ObjectVertexToClip(&xf, false, &in, &out);
    assert(out.texcoord.xyz_control == 0u);
    /* clamps */
    xf.fog[1] = -500.0f;
    assert(gfxPs2ObjectFogControl(xf.fog, 60.0f, 100.0f) == (255u << 4));
    xf.fog[1] = 500.0f;
    assert(gfxPs2ObjectFogControl(xf.fog, 60.0f, 100.0f) == 0u);
}

int main(void)
{
    srand(1234);
    test_cull_sign_matches_transformed();
    test_box_classification_is_conservative();
    test_vertex_to_clip_and_fog();
    puts("gfx_ps2_object tests passed");
    return 0;
}
