#include <stdbool.h>
#include <string.h>

#include <gsKit.h>
#include <gsInline.h>

#include "system.h"
#include "log_ps2.h"

#define SCENE_TEXTURE_W 64
#define SCENE_TEXTURE_H 64
#define SCENE_VERTEX_COUNT 8
#define SCENE_FACE_COUNT 6
#define SCENE_TRIANGLES_PER_FACE 2
#define SCENE_VERTICES_PER_TRIANGLE 3
#define SCENE_DRAW_VERTEX_COUNT \
    (SCENE_FACE_COUNT * SCENE_TRIANGLES_PER_FACE * SCENE_VERTICES_PER_TRIANGLE)

/*
 * Renderer bring-up stage 1.
 *
 * This remains deliberately smaller than the final Perfect Dark backend. It
 * proves the contracts we need before wiring Fast3D into the PS2 renderer:
 *
 *   - a resident texture uploaded once before the frame loop,
 *   - a real Z buffer and depth-tested overlapping geometry,
 *   - perspective-correct STQ texture coordinates,
 *   - CPU-produced dynamic screen-space vertices,
 *   - one explicit triangle-list submission for the cube,
 *   - no allocation or texture upload in the frame loop.
 *
 * VIF/VU/MMI are deliberately absent. They only become candidates after a
 * real game workload identifies a measured geometry/transform bottleneck.
 */

struct scene_vec3 {
    float x;
    float y;
    float z;
};

struct scene_screen_vertex {
    float x;
    float y;
    float q;
    int z;
};

struct scene_face {
    unsigned char index[4];
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

static const struct scene_vec3 kCubeVertices[SCENE_VERTEX_COUNT] = {
    { -1.0f, -1.0f, -1.0f },
    {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f },
    { -1.0f,  1.0f, -1.0f },
    { -1.0f, -1.0f,  1.0f },
    {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f },
    { -1.0f,  1.0f,  1.0f },
};

/*
 * Face vertices are stored around each perimeter. The draw path below expands
 * each face explicitly to triangles (a,b,c) + (a,c,d). Do not hand perimeter
 * order directly to gsKit's quad helper: current gsKit implements that helper
 * as a triangle strip, which has different vertex-order semantics.
 *
 * GS texture modulation treats 0x80 as unity. Tints stay at or below 0x80 so
 * the checker remains diagnostic instead of saturating into white.
 */
static const struct scene_face kCubeFaces[SCENE_FACE_COUNT] = {
    { { 0, 1, 2, 3 }, 0x68, 0x70, 0x80 },
    { { 5, 4, 7, 6 }, 0x80, 0x58, 0x58 },
    { { 4, 0, 3, 7 }, 0x58, 0x80, 0x64 },
    { { 1, 5, 6, 2 }, 0x80, 0x70, 0x48 },
    { { 3, 2, 6, 7 }, 0x70, 0x58, 0x80 },
    { { 4, 5, 1, 0 }, 0x50, 0x78, 0x80 },
};

/*
 * gsKit sends this buffer through the GIF DMA texture path. This alignment is
 * device/transport alignment for this one DMA source, not a general allocator
 * policy for the game.
 */
static u32 sCheckerTexture[SCENE_TEXTURE_W * SCENE_TEXTURE_H]
    __attribute__((aligned(64)));

static void buildCheckerTexture(void)
{
    for (int y = 0; y < SCENE_TEXTURE_H; ++y) {
        for (int x = 0; x < SCENE_TEXTURE_W; ++x) {
            const int checker = ((x >> 3) ^ (y >> 3)) & 1;
            const int cross = (x == SCENE_TEXTURE_W / 2) || (y == SCENE_TEXTURE_H / 2);
            u32 pixel;

            if (cross) {
                /* RGBA8888 in EE memory: R is the least-significant byte. */
                pixel = 0x8028d8ffu;
            } else if (checker) {
                pixel = 0x80d8d8d8u;
            } else {
                pixel = 0x80302a28u;
            }

            sCheckerTexture[y * SCENE_TEXTURE_W + x] = pixel;
        }
    }
}

static bool initSceneTexture(GSGLOBAL *gs, GSTEXTURE *texture)
{
    memset(texture, 0, sizeof(*texture));
    buildCheckerTexture();

    texture->Width = SCENE_TEXTURE_W;
    texture->Height = SCENE_TEXTURE_H;
    texture->PSM = GS_PSM_CT32;
    texture->Mem = sCheckerTexture;
    texture->Filter = GS_FILTER_NEAREST;

    const u32 textureBytes = gsKit_texture_size(
        texture->Width, texture->Height, texture->PSM);

    texture->Vram = gsKit_vram_alloc(gs, textureBytes, GSKIT_ALLOC_USERBUFFER);
    if (texture->Vram == GSKIT_ALLOC_ERROR) {
        sysLogPrintf(LOG_ERROR,
            "Renderer scene: GS VRAM allocation failed for %u-byte texture",
            textureBytes);
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: texture %ux%u CT32 EE=%p GS_VRAM=%08x bytes=%u",
        texture->Width, texture->Height, texture->Mem, texture->Vram, textureBytes);
    ps2LogCheckpoint();

    /* Immutable scene texture: upload once, then consume from GS local memory. */
    gsKit_texture_upload(gs, texture);
    gsKit_set_clamp(gs, GS_CMODE_CLAMP);
    gsKit_set_texfilter(gs, GS_FILTER_NEAREST);

    sysLogPrintf(LOG_NOTE, "Renderer scene: procedural texture upload complete");
    ps2LogCheckpoint();
    return true;
}

static int mapDepthToZ16(float q)
{
    /*
     * The rotating unit cube is translated to camera Z=4.6. Its actual depth
     * remains comfortably inside this conservative 2.5..7.0 range.
     *
     * Current gsKit GS_ZTEST_ON uses ZTST=GEQUAL, and gsKit_clear writes Z=0.
     * Reciprocal depth therefore maps nearer geometry to larger integer Z.
     * Use almost the complete Z16 range instead of an arbitrary small slice.
     */
    const float nearZ = 2.5f;
    const float farZ = 7.0f;
    const float nearQ = 1.0f / nearZ;
    const float farQ = 1.0f / farZ;
    float depth = (q - farQ) / (nearQ - farQ);

    if (depth < 0.0f) {
        depth = 0.0f;
    } else if (depth > 1.0f) {
        depth = 1.0f;
    }

    return 1 + (int)(depth * 65534.0f);
}

static struct scene_screen_vertex projectVertex(
    const struct scene_vec3 *source,
    float sinY,
    float cosY,
    float sinX,
    float cosX,
    float centerX,
    float centerY,
    float focal)
{
    /* Y rotation, then X rotation, then a fixed camera translation. */
    const float rx = source->x * cosY + source->z * sinY;
    const float rz0 = -source->x * sinY + source->z * cosY;
    const float ry = source->y * cosX - rz0 * sinX;
    const float rz = source->y * sinX + rz0 * cosX;
    const float cameraZ = rz + 4.6f;
    const float q = 1.0f / cameraZ;

    struct scene_screen_vertex out;
    out.x = centerX + rx * focal * q;
    out.y = centerY - ry * focal * q;
    out.q = q;
    out.z = mapDepthToZ16(q);
    return out;
}

static void advanceRotation(float *sinValue, float *cosValue, float sinStep, float cosStep)
{
    const float oldSin = *sinValue;
    const float oldCos = *cosValue;

    *sinValue = oldSin * cosStep + oldCos * sinStep;
    *cosValue = oldCos * cosStep - oldSin * sinStep;
}

static GSPRIMSTQPOINT makeTexturedVertex(
    GSGLOBAL *gs,
    const struct scene_screen_vertex *vertex,
    float u,
    float v,
    const struct scene_face *face)
{
    GSPRIMSTQPOINT out;
    const float q = vertex->q;

    /*
     * GS perspective-correct texturing uses S,T,Q with FST=0. Current PS2SDK
     * draw3d follows the same contract: Q=1/w, S=u*Q, T=v*Q. The GS then
     * performs the perspective divide during texture interpolation.
     */
    out.rgbaq = color_to_RGBAQ(face->r, face->g, face->b, 0x80, q);
    out.stq = vertex_to_STQ(u * q, v * q);
    out.xyz2 = vertex_to_XYZ2(gs, vertex->x, vertex->y, vertex->z);
    return out;
}

static void drawCube(
    GSGLOBAL *gs,
    GSTEXTURE *texture,
    float sinY,
    float cosY,
    float sinX,
    float cosX)
{
    struct scene_screen_vertex projected[SCENE_VERTEX_COUNT];
    GSPRIMSTQPOINT drawVertices[SCENE_DRAW_VERTEX_COUNT];
    int drawIndex = 0;

    const float centerX = (float)gs->Width * 0.50f;
    const float centerY = (float)gs->Height * 0.49f;
    const float focal = (float)gs->Height * 1.18f;

    for (int i = 0; i < SCENE_VERTEX_COUNT; ++i) {
        projected[i] = projectVertex(
            &kCubeVertices[i], sinY, cosY, sinX, cosX,
            centerX, centerY, focal);
    }

    for (int faceIndex = 0; faceIndex < SCENE_FACE_COUNT; ++faceIndex) {
        const struct scene_face *face = &kCubeFaces[faceIndex];
        const struct scene_screen_vertex *a = &projected[face->index[0]];
        const struct scene_screen_vertex *b = &projected[face->index[1]];
        const struct scene_screen_vertex *c = &projected[face->index[2]];
        const struct scene_screen_vertex *d = &projected[face->index[3]];

        /* Triangle 1: a,b,c. */
        drawVertices[drawIndex++] = makeTexturedVertex(gs, a, 0.0f, 1.0f, face);
        drawVertices[drawIndex++] = makeTexturedVertex(gs, b, 1.0f, 1.0f, face);
        drawVertices[drawIndex++] = makeTexturedVertex(gs, c, 1.0f, 0.0f, face);

        /* Triangle 2: a,c,d. Shares exactly the same face plane and UV seam. */
        drawVertices[drawIndex++] = makeTexturedVertex(gs, a, 0.0f, 1.0f, face);
        drawVertices[drawIndex++] = makeTexturedVertex(gs, c, 1.0f, 0.0f, face);
        drawVertices[drawIndex++] = makeTexturedVertex(gs, d, 0.0f, 0.0f, face);
    }

    gsKit_prim_list_triangle_goraud_texture_stq_3d(
        gs, texture, SCENE_DRAW_VERTEX_COUNT, drawVertices);
}

static void drawBackdrop(GSGLOBAL *gs, int romStatus)
{
    const u64 panel = GS_SETREG_RGBAQ(0x12, 0x18, 0x28, 0x80, 0x00);
    const u64 pass = GS_SETREG_RGBAQ(0x32, 0xd0, 0x74, 0x80, 0x00);
    const u64 fail = GS_SETREG_RGBAQ(0xff, 0x45, 0x45, 0x80, 0x00);
    const u64 unknown = GS_SETREG_RGBAQ(0xf5, 0xa5, 0x24, 0x80, 0x00);
    const u64 status = romStatus > 0 ? pass : (romStatus < 0 ? fail : unknown);

    /* These are intentionally behind the 3D object in reciprocal-Z space. */
    gsKit_prim_sprite(gs,
        (float)gs->Width * 0.10f, (float)gs->Height * 0.10f,
        (float)gs->Width * 0.90f, (float)gs->Height * 0.90f,
        1, panel);

    gsKit_prim_sprite(gs,
        (float)gs->Width * 0.12f, (float)gs->Height * 0.86f,
        (float)gs->Width * 0.88f, (float)gs->Height * 0.88f,
        2, status);
}

bool ps2RendererSceneRun(GSGLOBAL *gs, int romStatus)
{
    GSTEXTURE texture;

    if (!gs) {
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: begin explicit triangles + STQ + reciprocal Z16; no per-frame allocation/upload");
    ps2LogCheckpoint();

    if (!initSceneTexture(gs, &texture)) {
        return false;
    }

    /*
     * These constants are sin/cos of small fixed steps. Updating the basis by
     * recurrence avoids adding libm work to every diagnostic frame. This is a
     * bring-up choice, not a claim that the game should use this math path.
     */
    const float sinStepY = 0.011999712f;
    const float cosStepY = 0.999928001f;
    const float sinStepX = 0.007999915f;
    const float cosStepX = 0.999968000f;
    float sinY = 0.0f;
    float cosY = 1.0f;
    float sinX = 0.0f;
    float cosX = 1.0f;

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: entering frame loop CT16 + Z16 GEQUAL + CT32 resident texture + STQ");
    sysLogPrintf(LOG_NOTE,
        "Renderer scene: depth range cameraZ=2.5..7.0 mapped to Z16=1..65535");
    ps2LogCheckpoint();

    const u64 background = GS_SETREG_RGBAQ(0x05, 0x08, 0x12, 0x80, 0x00);

    for (;;) {
        /*
         * Current gsKit_clear temporarily switches ZTST to ALWAYS and emits
         * Z=0 over the frame, which clears both color and our GEQUAL Z baseline.
         */
        gsKit_clear(gs, background);
        drawBackdrop(gs, romStatus);
        drawCube(gs, &texture, sinY, cosY, sinX, cosX);

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);

        advanceRotation(&sinY, &cosY, sinStepY, cosStepY);
        advanceRotation(&sinX, &cosX, sinStepX, cosStepX);
    }
}
